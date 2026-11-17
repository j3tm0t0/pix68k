// net.c の TCP の受け口 (トークンの認証、命令の受け渡し) を、lwIP と無線のチップの代わり (host_stubs/net_host.h) で試す:
//   cc -std=c11 -Wall -Wextra -Werror -Ihost_stubs -I. -o test_net_host test_net_host.c core.c && ./test_net_host
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net.c"

static int s_fail;
#define CHECK(x) do { if (!(x)) { printf("NG %s:%d: %s\n", __FILE__, __LINE__, #x); s_fail++; } } while (0)

uint64_t g_nowMs = 1;
cyw43_t cyw43_state;

// ---- 代わりの中身 ----
static char s_sent[65536];  // 相手に送ったもの
static size_t s_sentLen;
static char s_ran[4096];    // 実行した命令 (1 つずつ ';' で区切る)
static char s_uart[4096];   // UART に返した応答 (同じく)
static bool s_closeFails;   // tcp_close を失敗させる
static int s_linkStatus = CYW43_LINK_UP;

const char* ip4addr_ntoa(const ip4_addr_t* a) { (void)a; return "192.0.2.1"; }
void tcp_arg(struct tcp_pcb* pcb, void* arg) { (void)pcb; (void)arg; }
void tcp_recv(struct tcp_pcb* pcb, tcp_recv_fn fn) { pcb->recv = fn; }
void tcp_err(struct tcp_pcb* pcb, tcp_err_fn fn) { pcb->errf = fn; }
void tcp_accept(struct tcp_pcb* pcb, tcp_accept_fn fn) { pcb->accept = fn; }
err_t tcp_close(struct tcp_pcb* pcb) {
    if (s_closeFails) return -1;
    CHECK(!pcb->closed && !pcb->aborted);
    pcb->closed = true;
    return ERR_OK;
}
void tcp_abort(struct tcp_pcb* pcb) {
    CHECK(!pcb->closed && !pcb->aborted);
    pcb->aborted = true;
}
void tcp_recved(struct tcp_pcb* pcb, u16_t len) { (void)pcb; (void)len; }
u16_t tcp_sndqueuelen(struct tcp_pcb* pcb) { (void)pcb; return 0; }
u16_t tcp_sndbuf(struct tcp_pcb* pcb) { (void)pcb; return 4096; }
err_t tcp_write(struct tcp_pcb* pcb, const void* data, u16_t len, uint8_t flags) {
    (void)flags;
    CHECK(!pcb->closed && !pcb->aborted && s_sentLen + len < sizeof s_sent);
    memcpy(s_sent + s_sentLen, data, len);
    s_sentLen += len;
    s_sent[s_sentLen] = 0;
    return ERR_OK;
}
err_t tcp_output(struct tcp_pcb* pcb) { (void)pcb; return ERR_OK; }
void tcp_nagle_disable(struct tcp_pcb* pcb) { (void)pcb; }
struct tcp_pcb* tcp_new_ip_type(uint8_t type) { (void)type; return calloc(1, sizeof(struct tcp_pcb)); }
err_t tcp_bind(struct tcp_pcb* pcb, const ip_addr_t* addr, u16_t port) { (void)pcb; (void)addr; CHECK(port == 2323); return ERR_OK; }
struct tcp_pcb* tcp_listen_with_backlog(struct tcp_pcb* pcb, uint8_t backlog) { (void)backlog; return pcb; }
void pbuf_free(struct pbuf* p) { (void)p; }
void mdns_resp_init(void) {}
void mdns_resp_add_netif(struct netif* n, const char* name) { (void)n; (void)name; }
void mdns_resp_announce(struct netif* n) { (void)n; }
int cyw43_arch_init_with_country(uint32_t country) { (void)country; return 0; }
void cyw43_arch_enable_sta_mode(void) {}
void cyw43_wifi_pm(cyw43_t* self, uint32_t pm) { (void)self; (void)pm; }
void cyw43_arch_poll(void) {}
void cyw43_arch_gpio_put(int pin, bool on) { (void)pin; (void)on; }
int cyw43_arch_wifi_connect_async(const char* ssid, const char* pw, uint32_t auth) { (void)ssid; (void)pw; (void)auth; return 0; }
int cyw43_tcpip_link_status(cyw43_t* self, int itf) { (void)self; (void)itf; return s_linkStatus; }
int cyw43_wifi_leave(cyw43_t* self, int itf) { (void)self; (void)itf; return 0; }

static void note(char* list, size_t cap, const char* s) {
    size_t n = strlen(list);
    snprintf(list + n, cap - n, "%s;", s);
}
bool app_reply(const char* s) {
    note(s_uart, sizeof s_uart, s);
    net_reply(s); // main.c と同じく、TCP の相手にも返す
    return true;
}
void app_run(const char* line) { note(s_ran, sizeof s_ran, line); }

// ---- 試験の道具 ----
static void step(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i += 50) {
        g_nowMs += ms - i < 50 ? ms - i : 50;
        net_task();
    }
    net_task();
}
static void clear(void) {
    s_sentLen = 0;
    s_sent[0] = s_ran[0] = s_uart[0] = 0;
}
// つなぐ。断られたら NULL
static struct tcp_pcb* dial(void) {
    struct tcp_pcb* pcb = calloc(1, sizeof *pcb);
    err_t e = s_listen->accept(NULL, pcb, ERR_OK);
    if (e != ERR_OK) {
        CHECK(e == ERR_ABRT && pcb->aborted);
        return NULL;
    }
    return pcb;
}
static err_t feed_n(struct tcp_pcb* pcb, const char* data, size_t n) {
    struct pbuf p = {NULL, (void*)(uintptr_t)data, (u16_t)n, (u16_t)n};
    err_t e = pcb->recv(NULL, pcb, &p, ERR_OK);
    net_task();
    return e;
}
static err_t feed(struct tcp_pcb* pcb, const char* s) { return feed_n(pcb, s, strlen(s)); }
static bool gone(const struct tcp_pcb* pcb) { return pcb->closed || pcb->aborted; }
#define TOKEN "0123456789abcdef"

int main(void) {
    net_init();
    CHECK(net_down_ms() == 0 || !net_is_up());
    step(1000);
    CHECK(net_is_up() && net_down_ms() == 0 && s_listen != NULL);
    struct tcp_pcb* c;

    // 正しいトークン: "auth ok" を返し、そのあとの行を命令として実行する
    clear();
    c = dial();
    CHECK(c != NULL && !net_client_authed());
    feed(c, TOKEN "\n");
    CHECK(net_client_authed() && strcmp(s_sent, "#pico auth ok\n") == 0 && s_ran[0] == 0);
    feed(c, "P\n");
    CHECK(strcmp(s_ran, "P;") == 0 && !gone(c));
    // 認証のあとの長すぎる行は、エラーを返すだけで切らない
    {
        static char big[700];
        memset(big, 'x', 600);
        big[600] = '\n';
        clear();
        feed(c, big);
        CHECK(s_ran[0] == 0 && strstr(s_sent, "#pico error too-long\n") != NULL && !gone(c));
    }
    // 認証を通った相手がいる間は、ほかの相手を断る
    CHECK(dial() == NULL && net_client_authed() && !gone(c));
    // 相手が閉じた
    CHECK(c->recv(NULL, c, NULL, ERR_OK) == ERR_OK && c->closed && !net_client_authed());

    // 違うトークン: 何も返さずに切り、命令も実行しない。そのあと 1 秒はだれも受け付けない
    static const char* const kWrong[] = {"wrong\n", "\n", "0123456789abcde\n", TOKEN "0\n", " " TOKEN "\n", "0123456789abcdeF\n", "P\n"};
    for (unsigned i = 0; i < sizeof kWrong / sizeof kWrong[0]; i++) {
        clear();
        c = dial();
        CHECK(c != NULL);
        if (c == NULL) break;
        feed(c, kWrong[i]);
        CHECK(gone(c) && s_sentLen == 0 && s_ran[0] == 0 && !net_client_authed());
        CHECK(strstr(s_uart, "net auth-failed;") != NULL);
        CHECK(dial() == NULL); // すぐには受け付けない
        step(900);
        CHECK(dial() == NULL);
        step(100);
    }
    // 違うトークンのあとに正しいトークンと命令を続けても、通らない (切ったあとのデータは読まない)
    clear();
    c = dial();
    feed(c, "wrong\n" TOKEN "\nB\n");
    CHECK(gone(c) && s_sentLen == 0 && s_ran[0] == 0 && !net_client_authed());
    step(1000);

    // トークンの前の長すぎる行: 切る
    {
        static char big[700];
        memset(big, '0', 600);
        big[600] = '\n';
        clear();
        c = dial();
        feed(c, big);
        CHECK(gone(c) && s_sentLen == 0 && s_ran[0] == 0);
        step(1000);
    }

    // トークンが来ないまま 10 秒: 切る。それまでに送った応答やログは相手に行かない
    clear();
    c = dial();
    net_reply("secret-reply");
    net_log_byte('x');
    net_log_byte('\n');
    step(9900);
    CHECK(!gone(c) && s_sentLen == 0);
    step(100);
    CHECK(gone(c) && s_sentLen == 0 && !net_client_authed());
    // 改行のないまま 10 秒 (トークンの途中まで) でも同じ
    c = dial();
    feed(c, "01234567");
    step(10000);
    CHECK(gone(c) && s_sentLen == 0);

    // トークンと命令が同じ受信データで来る: 認証のあと、命令を順に実行する
    clear();
    c = dial();
    feed(c, TOKEN "\nO\nN\n");
    CHECK(net_client_authed() && strcmp(s_ran, "O;N;") == 0 && strncmp(s_sent, "#pico auth ok\n", 14) == 0);
    c->recv(NULL, c, NULL, ERR_OK);
    // トークンが 2 つの受信データに分かれて来る。CR LF でもよい
    clear();
    c = dial();
    feed(c, "01234567");
    CHECK(!net_client_authed() && !gone(c));
    feed(c, "89abcdef\r\nP\r\n");
    CHECK(net_client_authed() && strcmp(s_ran, "P;") == 0);
    // 認証を通ったあとは、10 秒たっても切らない
    step(20000);
    CHECK(net_client_authed() && !gone(c));
    // 相手が閉じたとき、ふつうに閉じられなければ捨てて、lwIP に ERR_ABRT を返す
    s_closeFails = true;
    CHECK(c->recv(NULL, c, NULL, ERR_OK) == ERR_ABRT && c->aborted && !net_client_authed());
    s_closeFails = false;

    // まだトークンを送っていない相手は、新しい相手に譲る
    c = dial();
    struct tcp_pcb* d = dial();
    CHECK(gone(c) && d != NULL && !gone(d));
    feed(d, TOKEN "\n");
    CHECK(net_client_authed());

    // 無線 LAN が切れたら相手を切り、つながっていない時間を数える
    s_linkStatus = 0;
    step(1000);
    CHECK(gone(d) && !net_is_up() && !net_client_authed());
    step(5000);
    CHECK(net_down_ms() >= 5000 && net_down_ms() < 7000);
    s_linkStatus = CYW43_LINK_UP;
    step(3000);
    CHECK(net_is_up() && net_down_ms() == 0);

    if (s_fail) { printf("%d failed\n", s_fail); return 1; }
    printf("ok\n");
    return 0;
}
