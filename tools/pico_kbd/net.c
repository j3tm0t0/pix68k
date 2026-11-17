// 無線 LAN (Pico W / Pico 2 W の CYW43439) で命令を受ける。
//
//   TCP のポート PICO_KBD_TCP_PORT (既定 2323) で待つ。相手は同時に 1 つ。
//   最初の行はトークン (ビルドのときに pico_kbd_secrets.h に入れたもの)。合えば "#pico auth ok" を返し、
//   そのあとは UART と同じ命令を 1 行ずつ受ける。合わなければ何も返さずに切り、そのあと 1 秒はだれも受け付けない。
//   10 秒たってもトークンが来なければ切る。
//   認証を通るまでは、命令を実行せず、応答もログも送らない。
//   送る行: 応答は "#pico ..."、試験機のログは行の先頭に "> " を付ける。どちらも改行は \n。
//   ログの行が 0.2 秒たっても終わらないときは、そこまでを 1 行として送る。
//   送りきれないログは捨てて数える (N 命令の log-dropped)。応答も、送るものがたまりすぎていれば捨てる。
//   mDNS で「ホスト名.local」(ホスト名は CYW43_HOST_NAME) に答える。
//   相手が何も言わずに消えたときは、TCP の keepalive (5 秒 + 2 秒 × 3 回) で気づいて切り、次の相手を待つ。
//
// 基板の LED (無線のチップの先にある):
//   消えたまま         無線のチップの初期化に失敗した (または、ファームウェアが動いていない)
//   1 秒に 1 回の点滅  アクセスポイントにつなごうとしている
//   速い点滅           パスワードが違うと言われた (つなぎ直しは続ける)
//   点いたまま         IP アドレスを取れた。TCP の相手を待っている
//   1 秒ごとに短く消える  認証を通った相手がつないでいる
//
// lwIP は主ループから回す (pico_cyw43_arch_lwip_poll)。lwIP の呼び出しも、ここのリングバッファも、主ループだけが触る。
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "core.h"
#include "lwip/apps/mdns.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "pico_kbd_secrets.h"
#include "ring.h"

#ifndef PICO_KBD_TCP_PORT
#define PICO_KBD_TCP_PORT 2323
#endif

#define AUTH_TIMEOUT_MS 10000
#define AUTH_LOCK_MS 1000
#define JOIN_TIMEOUT_MS 30000
#define RETRY_MS 5000
#define LOG_FLUSH_MS 200

// ---- 無線 LAN ----
typedef enum { WIFI_OFF, WIFI_IDLE, WIFI_JOINING, WIFI_UP } WifiState;
static WifiState s_wifi = WIFI_OFF;
static absolute_time_t s_wifiNext;     // 次につなぎに行く時刻 (WIFI_IDLE)、あきらめる時刻 (WIFI_JOINING)
static absolute_time_t s_wifiPoll;
static int s_wifiLastError;            // 最後に失敗したときの cyw43 の状態 (負の数。0 なら失敗なし)
static unsigned s_wifiTries;
static uint32_t s_wifiUpMs;            // 最後に IP アドレスを取れたときの、起動からのミリ秒
static uint32_t s_wifiDownAt;          // つながっていない状態が始まったときの、起動からのミリ秒 (起動のときは 0)
static bool s_ledOn;

// ---- TCP ----
static struct tcp_pcb* s_listen;
static struct tcp_pcb* s_client;
static bool s_authed;
static absolute_time_t s_authDeadline;
static absolute_time_t s_authLock; // この時刻までは、新しい相手を受け付けない
static uint8_t s_rxBuf[2048], s_txBuf[16384];
static Ring s_rxRing = RING_INIT(s_rxBuf); // 相手から受けたバイト
static Ring s_txRing = RING_INIT(s_txBuf); // 相手に送るバイト
static uint8_t s_chunk[512];               // tcp_write に渡す途中の分 (受け付けられるまで持っておく)
static unsigned s_chunkLen;
static LineBuf s_line;
static LogLine s_logLine;
static absolute_time_t s_logFlush;
static uint32_t s_logDropped, s_replyDropped;

static struct netif* sta_netif(void) { return &cyw43_state.netif[CYW43_ITF_STA]; }

static void client_reset(void) {
    s_client = NULL;
    s_authed = false;
    s_chunkLen = 0;
    s_rxRing.head = s_rxRing.tail;
    s_txRing.head = s_txRing.tail;
    memset(&s_line, 0, sizeof s_line);
    memset(&s_logLine, 0, sizeof s_logLine);
}

// 相手を切る。ふつうに閉じられず、tcp_abort() で捨てたときは true
// (lwIP の受信の呼び出しの中から捨てたときは、ERR_ABRT を返して lwIP に知らせる決まり)
static bool client_close(void) {
    struct tcp_pcb* pcb = s_client;
    bool aborted = false;
    if (pcb == NULL) return false;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_err(pcb, NULL);
    if (tcp_close(pcb) != ERR_OK) {
        tcp_abort(pcb);
        aborted = true;
    }
    client_reset();
    return aborted;
}

// 1 行まるごと入るときだけ入れる
static bool tx_line(const char* head, const char* s) {
    size_t hn = strlen(head), n = strlen(s);
    if (ring_space(&s_txRing) < hn + n + 1) return false;
    for (size_t i = 0; i < hn; i++) ring_put(&s_txRing, (uint8_t)head[i]);
    for (size_t i = 0; i < n; i++) ring_put(&s_txRing, (uint8_t)s[i]);
    ring_put(&s_txRing, '\n');
    return true;
}

void net_reply(const char* s) {
    if (s_client == NULL || !s_authed) return;
    if (!tx_line("#pico ", s)) s_replyDropped++;
}

static void log_line_out(void) {
    if (!tx_line("> ", s_logLine.buf)) s_logDropped++;
}

void net_log_byte(char c) {
    if (s_client == NULL || !s_authed) return;
    if (logline_feed(&s_logLine, c)) log_line_out();
    s_logFlush = make_timeout_time_ms(LOG_FLUSH_MS);
}

bool net_tx_idle(void) {
    if (s_client == NULL || !s_authed) return true;
    return ring_count(&s_txRing) == 0 && s_chunkLen == 0 && tcp_sndqueuelen(s_client) == 0;
}

static err_t on_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err) {
    (void)arg;
    (void)err;
    if (pcb != s_client) { // 来ないはず
        if (p != NULL) pbuf_free(p);
        return ERR_OK;
    }
    if (p == NULL) { // 相手が閉じた
        return client_close() ? ERR_ABRT : ERR_OK;
    }
    for (struct pbuf* q = p; q != NULL; q = q->next) {
        const uint8_t* d = (const uint8_t*)q->payload;
        // 入りきらない分は捨てる (命令は応答を待ってから送る決まりなので、ふつうはあふれない)
        for (u16_t i = 0; i < q->len; i++) ring_put(&s_rxRing, d[i]);
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

// lwIP が接続を捨てたあとに呼ばれる (pcb はもうない)
static void on_err(void* arg, err_t err) {
    (void)arg;
    (void)err;
    client_reset();
}

static err_t on_accept(void* arg, struct tcp_pcb* pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || pcb == NULL) return ERR_VAL;
    // 違うトークンが来たあとしばらくは、だれも受け付けない (総当たりを遅くする)
    if (!time_reached(s_authLock)) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    if (s_client != NULL) {
        // 認証を通った相手がいる間は、新しい相手を断る。まだ通っていない相手は、新しい相手に譲る
        if (s_authed) {
            tcp_abort(pcb);
            return ERR_ABRT;
        }
        client_close();
    }
    client_reset();
    s_client = pcb;
    s_authDeadline = make_timeout_time_ms(AUTH_TIMEOUT_MS);
    ip_set_option(pcb, SOF_KEEPALIVE);
    pcb->keep_idle = 5000;
    pcb->keep_intvl = 2000;
    pcb->keep_cnt = 3;
    tcp_nagle_disable(pcb);
    tcp_recv(pcb, on_recv);
    tcp_err(pcb, on_err);
    return ERR_OK;
}

static void listen_start(void) {
    struct tcp_pcb* pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb == NULL) return;
    if (tcp_bind(pcb, IP_ANY_TYPE, PICO_KBD_TCP_PORT) != ERR_OK) {
        tcp_abort(pcb);
        return;
    }
    s_listen = tcp_listen_with_backlog(pcb, 1);
    if (s_listen == NULL) {
        tcp_abort(pcb);
        return;
    }
    tcp_accept(s_listen, on_accept);
}

// 受けたバイトを行にして、認証と命令に回す
static void rx_task(void) {
    uint8_t c;
    while (s_client != NULL && ring_get(&s_rxRing, &c)) {
        LineResult r = line_feed(&s_line, (char)c);
        if (r == LINE_NONE) continue;
        if (!s_authed) {
            if (r != LINE_READY || !token_eq(s_line.buf, kNetToken)) {
                client_close();
                s_authLock = make_timeout_time_ms(AUTH_LOCK_MS);
                app_reply("net auth-failed");
                return;
            }
            char buf[48];
            snprintf(buf, sizeof buf, "net client %s", ip4addr_ntoa(ip_2_ip4(&s_client->remote_ip)));
            app_reply(buf); // UART に残す (相手にはまだ送らない)
            s_authed = true;
            tx_line("#pico ", "auth ok");
            continue;
        }
        if (r == LINE_READY) app_run(s_line.buf);
        else app_reply("error too-long");
    }
}

static void tx_task(void) {
    if (s_client == NULL) return;
    if (!s_authed) {
        if (time_reached(s_authDeadline)) client_close();
        return;
    }
    if (time_reached(s_logFlush) && logline_flush(&s_logLine)) log_line_out();
    bool wrote = false;
    for (;;) {
        if (s_chunkLen == 0) {
            uint8_t c;
            while (s_chunkLen < sizeof s_chunk && ring_get(&s_txRing, &c)) s_chunk[s_chunkLen++] = c;
            if (s_chunkLen == 0) break;
        }
        if (tcp_sndbuf(s_client) < s_chunkLen) break;
        if (tcp_write(s_client, s_chunk, (u16_t)s_chunkLen, TCP_WRITE_FLAG_COPY) != ERR_OK) break; // 次の周回でもう一度
        s_chunkLen = 0;
        wrote = true;
    }
    if (wrote) tcp_output(s_client);
}

static void led_task(void) {
    uint32_t ms = to_ms_since_boot(get_absolute_time());
    bool on;
    if (s_wifi == WIFI_UP) on = !(s_authed && ms % 1000 < 100);
    else if (s_wifiLastError == CYW43_LINK_BADAUTH) on = ms % 200 < 100;
    else on = ms % 1000 < 500;
    if (on != s_ledOn) {
        s_ledOn = on;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
    }
}

static void wifi_join(void) {
    // 暗号の方式はアクセスポイントによるので、失敗するたびに順に変える
    static const uint32_t kAuth[] = {CYW43_AUTH_WPA2_AES_PSK, CYW43_AUTH_WPA3_WPA2_AES_PSK, CYW43_AUTH_WPA2_MIXED_PSK};
    uint32_t auth = kWifiPassword[0] ? kAuth[s_wifiTries % (sizeof kAuth / sizeof kAuth[0])] : CYW43_AUTH_OPEN;
    s_wifiTries++;
    if (cyw43_arch_wifi_connect_async(kWifiSsid, kWifiPassword[0] ? kWifiPassword : NULL, auth) != 0) {
        s_wifiNext = make_timeout_time_ms(RETRY_MS);
        return;
    }
    s_wifi = WIFI_JOINING;
    s_wifiNext = make_timeout_time_ms(JOIN_TIMEOUT_MS);
}

static void wifi_task(void) {
    if (!time_reached(s_wifiPoll)) return;
    s_wifiPoll = make_timeout_time_ms(250);
    int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    char buf[96];
    switch (s_wifi) {
    case WIFI_OFF:
        break;
    case WIFI_IDLE:
        if (time_reached(s_wifiNext)) wifi_join();
        break;
    case WIFI_JOINING:
        if (st == CYW43_LINK_UP) {
            s_wifi = WIFI_UP;
            s_wifiLastError = 0;
            s_wifiUpMs = to_ms_since_boot(get_absolute_time());
            mdns_resp_announce(sta_netif()); // アドレスが決まったことを mDNS に知らせる
            snprintf(buf, sizeof buf, "wifi up ip=%s port=%d host=%s ms=%lu tries=%u",
                     ip4addr_ntoa(netif_ip4_addr(sta_netif())), PICO_KBD_TCP_PORT, CYW43_HOST_NAME,
                     (unsigned long)s_wifiUpMs, s_wifiTries);
            app_reply(buf);
        } else if (st < 0 || time_reached(s_wifiNext)) {
            s_wifiLastError = st < 0 ? st : CYW43_LINK_FAIL;
            snprintf(buf, sizeof buf, "wifi error %d", st);
            app_reply(buf);
            cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
            s_wifi = WIFI_IDLE;
            s_wifiNext = make_timeout_time_ms(RETRY_MS);
        }
        break;
    case WIFI_UP:
        if (st != CYW43_LINK_UP) {
            client_close();
            s_wifiDownAt = to_ms_since_boot(get_absolute_time());
            snprintf(buf, sizeof buf, "wifi down %d", st);
            app_reply(buf);
            cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
            s_wifi = WIFI_IDLE;
            s_wifiNext = make_timeout_time_ms(1000);
        }
        break;
    }
}

void net_init(void) {
    if (cyw43_arch_init_with_country(CYW43_COUNTRY_JAPAN) != 0) return; // LED は消えたまま
    cyw43_arch_enable_sta_mode(); // ホスト名 (CYW43_HOST_NAME) はここで netif に入る
    cyw43_wifi_pm(&cyw43_state, CYW43_PERFORMANCE_PM); // 応答が遅れないよう、省電力を弱める
    // mDNS で「ホスト名.local」に答える (ルーターが DHCP の名前を引けなくても、名前でつなげるように)
    mdns_resp_init();
    mdns_resp_add_netif(sta_netif(), CYW43_HOST_NAME);
    listen_start();
    s_wifi = WIFI_IDLE;
    s_wifiNext = get_absolute_time();
    s_wifiPoll = get_absolute_time();
}

void net_task(void) {
    if (s_wifi == WIFI_OFF) return;
    cyw43_arch_poll();
    wifi_task();
    rx_task();
    tx_task();
    led_task();
}

void net_flush(unsigned ms) {
    if (s_wifi == WIFI_OFF) return;
    absolute_time_t end = make_timeout_time_ms(ms);
    while (!net_tx_idle() && !time_reached(end)) {
        cyw43_arch_poll();
        tx_task();
    }
    // 相手が切れたと分かるように、閉じてから再起動する
    if (s_client != NULL) {
        client_close();
        end = make_timeout_time_ms(100);
        while (!time_reached(end)) cyw43_arch_poll();
    }
}

bool net_is_up(void) { return s_wifi == WIFI_UP; }
bool net_client_authed(void) { return s_client != NULL && s_authed; }

uint32_t net_down_ms(void) {
    if (s_wifi == WIFI_UP) return 0;
    return to_ms_since_boot(get_absolute_time()) - s_wifiDownAt;
}

void net_status(char* buf, size_t size) {
    static const char* const kNames[] = {"off", "idle", "joining", "up"};
    if (s_wifi == WIFI_UP)
        snprintf(buf, size, "net up ip=%s port=%d host=%s client=%s log-dropped=%lu reply-dropped=%lu up-ms=%lu tries=%u",
                 ip4addr_ntoa(netif_ip4_addr(sta_netif())), PICO_KBD_TCP_PORT, CYW43_HOST_NAME,
                 s_client == NULL ? "none" : s_authed ? "authed" : "waiting", (unsigned long)s_logDropped,
                 (unsigned long)s_replyDropped, (unsigned long)s_wifiUpMs, s_wifiTries);
    else
        snprintf(buf, size, "net %s last-error=%d tries=%u", kNames[s_wifi], s_wifiLastError, s_wifiTries);
}
