// 実機の試験用: Raspberry Pi Pico (RP2040 の Pico / Pico W、RP2350 の Pico 2 W) を、試験する Pi の USB キーボード兼 UART の中継にする。
//
//   UART0 (GP0 = TX、GP1 = RX、115200bps): 操作する側 (Debug Probe の U 端子など) につなぐ。
//       命令を 1 行ずつ受け、応答 ("#pico ..." の行) と、UART1 で受けたログを返す
//   UART1 (GP5 = RX、115200bps): 試験する Pi の UART の TX (Pi 3 / 4 なら GPIO 14) につなぐ。受けたバイトをそのまま UART0 に送る
//   UART1 (GP4 = TX): 試験する Pi の UART の RX につなぐと、S / W 命令で文字列を送れる (つながなくてもよい)
//   USB: 試験する Pi につなぐ。boot プロトコルの USB キーボードに見える。C 命令で kg を選ぶと、HID のゲームパッドも足した
//       複合装置としてつなぎ直す (起動したときはキーボードだけ。PICO_KBD_PAD_DEFAULT=1 でビルドするとゲームパッドも見せる)
//
// 命令 (改行で終わる 1 行):
//   P              "#pico ok" を返す (USB がつながっていれば "mounted" も付ける)
//   T 文字列       JIS 配列のキーとして打つ。\n = Enter、\t = Tab、\e = Esc、\b = BS、\\ = \ 。
//                  "typed skipped=S dropped=D" を返す。S は JIS 配列で打てずに飛ばした文字の数、D は列に入れなかった数
//   K uu [mm] [ms] HID の usage uu (16 進) を、修飾 mm (16 進。bit0 LCtrl、bit1 LShift、bit2 LAlt、bit3 LGUI、
//                  bit4 RCtrl、bit5 RShift、bit6 RAlt、bit7 RGUI) を付けて ms ミリ秒 (既定 60) 押す。
//                  uu か mm が ff を超えるか、ms が 10000 を超えると "error bad-args"
//   D uu [mm]      押したままにする (uu が 0 なら修飾だけ)。U ですべて離す。uu か mm が ff を超えると "error bad-args"。
//                  押したままのキーがすでに 6 個あって足せないときは "down dropped"
//   U              すべて離す
//   X [ms]         試験する Pi をリセットする: GP2 (Pi の RUN 端子につなぐ) を ms ミリ秒 (既定 200、最大 5000) だけ Low にする。
//                  "#pico reset ..." を送り終えてから Low にする。Low の間も命令とログの受信は続く (2 つ目の X は "reset busy")。
//                  ふだんの GP2 は入力 (Hi-Z) で、High は出さない (RUN は Pi の側で 3.3V に引き上げてある)。
//                  Pi の USB から電源を取っていると、リセットで Pico の電源も落ち、そこで Low も終わる
//   J [S|L|ms]     Pi 5 の電源ボタンを押す: GP3 (Pi 5 の J2 の、GND でない側につなぐ) を Low にする。
//                  引数なしか S は短押し (300 ミリ秒)、L は長押し (5000 ミリ秒)、数は 1 から 15000 のミリ秒。
//                  それ以外は "error bad-args"。"#pico power N ms" を送り終えてから Low にし、終わったら "#pico power done" を返す。
//                  Low の間の 2 つ目の J は "power busy"。ふだんの GP3 は Hi-Z で、High は出さない。X と GP2 とは別に動く
//                  (PICO_KBD_PWR_ACTIVE_HIGH=1 でビルドすると、Low の代わりに High を出す。トランジスタを挟む配線用)
//   S 文字列       試験機の UART に、文字列と改行 (\n) を送る (GP4 = UART1 TX、115200bps。試験機の UART の RX につなぐ)。
//                  T と同じく \n、\r、\t、\e、\b、\\ が書ける。"sent N" を返す (N は送るバイト数)。
//                  送る分が 1023 バイトの待ち行列に入りきらなければ、何も送らずに "send dropped"
//   W 文字列       S と同じだが、最後に改行を付けない
//   G [bb [h [ms]]] ゲームパッドのボタン bb (16 進、bit0 がボタン 1、32 個まで) とハット h (0 = 離す、1 = 上、2 = 右上、
//                  時計回りに 8 = 左上) を押す。ms (1〜10000) を書くと、その時間だけ押して離す (押したレポートを
//                  受け手が読み終えた時刻から数える)。書かなければ押したまま (0 は書けない)。
//                  引数なしの G はすべて離す。応答は "pad"。ゲームパッドを見せていないときは "pad none"、
//                  USB がつながっていないときは "pad dropped"、書き方が違えば "error bad-args"
//   C off          USB を切り離す (tud_disconnect。試験機からは抜いたように見える)。応答は "usb off"
//   C on [k|kg]    USB をつなぐ (tud_connect)。k はキーボードだけ、kg はキーボードとゲームパッド (省略すると今のまま)。
//                  つないだまま違う組み合わせを選んだときは、切り離して 500 ミリ秒後につなぎ直す。応答は "usb on"
//   C ms [k|kg]    USB を切り離し、ms ミリ秒 (100〜60000) 後につなぎ直す。応答は "usb off ms ms"、つなぎ直したら "usb on"。
//                  キーボードとゲームパッドは 1 つの USB 装置なので、どちらも一緒に抜き差しされる。
//                  切り離している間は T / K / D / G / U を入れずに dropped を返す ("up dropped" のときも、押したままのキーは忘れる)
//   O ...          無線 LAN 越しの更新 (PICO_KBD_OTA を付けたビルドだけ。ota.c の先頭を参照)
//   N              無線 LAN の様子を返す (無線 LAN のビルドだけ。"net up ip=... port=... host=... client=..." など。
//                  無線 LAN なしのビルドは "net none")。行の終わりに " wd=W wd-boot=B" が付く
//                  (W はウォッチドッグを掛けているか、B はこの起動がウォッチドッグの時間切れによるものか)
//   R              Pico を再起動する
//   Z              わざと固まる (ウォッチドッグの試験用)。約 8 秒でウォッチドッグが再起動する。
//                  ウォッチドッグをまだ掛けていないとき (新しい版を確定する前) は "hang refused"
//   B              Pico を USB の書き込みモード (BOOTSEL) で再起動する
// 無線 LAN のビルド (cmake に PICO_KBD_WIFI_FILE を渡したもの。Pico W / Pico 2 W) は、同じ命令を TCP でも受ける。
// 応答は UART と TCP の相手の両方に返す。試験機のログは、TCP の相手には行の先頭に "> " を付けて送る。net.c の先頭を参照。
// 511 バイトを超えた行は改行まで捨てて "#pico error too-long" を返す。
// キーの送信は USB の受け手が読みに来たときだけ進む (読みに来なくても命令とログの中継は止まらない)。
// キーの列が満杯のとき、T / K / D は入れずに dropped を返す。U は必ず入る (最後の 1 枠を U のために空けてある)。
// USB がつながっていない (mounted の前) ときも、T / K / D は入れずに dropped を返す (キーは "#pico mounted" を見てから送る)。
// 試験機が止まったとき (suspend)、つながり直したとき (mount)、切れたとき (unmount) は、まだ送っていないキーを捨てる
// (下の tud_suspend_cb() などを参照)。
//
// UART の受信は割り込みでリングバッファに入れ、主ループは待たない。UART0 への送信は、応答を先に、ログをあとに送る。
// ログのリングバッファ (32KB、115200bps で約 2.8 秒分) があふれたら、捨てたバイト数を "#pico log-dropped N" で知らせる。
// 命令のリングバッファがあふれたときは "#pico error cmd-dropped N" を返す。
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "bsp/board_api.h"
#include "core.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "ring.h"
#include "tusb.h"

#define CMD_UART uart0
#define LOG_UART uart1
#define BAUD 115200
#define RUN_PIN 2 // 試験する Pi の RUN 端子
#define PWR_PIN 3 // Pi 5 の電源ボタンの端子 (J2)
// GP3 と J2 の間に NPN トランジスタを挟む配線では、押すときに High を出す (cmake に -DPICO_KBD_PWR_ACTIVE_HIGH=1)。
// 押していない間はどちらの配線でも Hi-Z
#ifndef PICO_KBD_PWR_ACTIVE_HIGH
#define PICO_KBD_PWR_ACTIVE_HIGH 0
#endif

// SDK や lwIP が続けられなくなったとき (panic、assert) と、CPU の例外 (hard fault) のときは、止まらずに再起動する。
// 人が挿し直さなくても、命令を受けられる状態に戻るように。無線 LAN 越しの更新で試している版なら、前の版に戻る
void __attribute__((noreturn)) app_panic(const char* fmt, ...) {
    (void)fmt;
    watchdog_reboot(0, 0, 10);
    for (;;) tight_loop_contents();
}

void isr_hardfault(void) {
    watchdog_reboot(0, 0, 10);
    for (;;) tight_loop_contents();
}

// ---- ウォッチドッグ ----
// 主ループが 8 秒回らなければ再起動する (panic でも hard fault でもない形で固まったとき用)。
// 無線 LAN 越しの更新で新しい版を試している間は、ブート ROM (RP2350) が確定待ちのウォッチドッグを掛けているので、
// 確定が済むまで掛けない (ota_wd_ok())。掛ける前に watchdog_update() を呼ぶと、ブート ROM が掛けた時間を 0 にしてしまうので呼ばない
#define WD_MS 8000
static bool s_wdOn;
static bool s_wdBoot; // この起動は、ウォッチドッグの時間切れによるものか

static void wd_task(void) {
    if (!s_wdOn) {
#if PICO_KBD_OTA
        if (!ota_wd_ok()) return;
#endif
        watchdog_enable(WD_MS, true);
        s_wdOn = true;
    }
    watchdog_update();
}

// 新しい版を確定してよい状態か: TCP でトークンを通った相手が来ている (無線 LAN なしのビルドは、いつでも)
#if PICO_KBD_OTA
static bool ota_ready(void) {
#if PICO_KBD_WIFI
    return net_client_authed();
#else
    return true;
#endif
}
#endif

// 無線 LAN につながらないままこの分数がたったら再起動する (0 なら、しない)
#ifndef PICO_KBD_WIFI_REBOOT_MIN
#define PICO_KBD_WIFI_REBOOT_MIN 10
#endif

// ---- UART のリングバッファ ----
static uint8_t s_cmdBuf[1024], s_logBuf[32768], s_replyBuf[4096];
#if PICO_KBD_WIFI
static uint8_t s_netLogBuf[8192];
static Ring s_netLogRing = RING_INIT(s_netLogBuf); // UART1 の受信の写し (割り込み → 主ループ → TCP の相手)
#endif
static Ring s_cmdRing = RING_INIT(s_cmdBuf);     // UART0 の受信 (割り込み → 主ループ)
static Ring s_logRing = RING_INIT(s_logBuf);     // UART1 の受信 (割り込み → 主ループ)
static Ring s_replyRing = RING_INIT(s_replyBuf); // 応答 (主ループ → 主ループ)
static uint8_t s_piTxBuf[1024];
static Ring s_piTxRing = RING_INIT(s_piTxBuf);   // S / W 命令で試験機の UART に送る分 (主ループ → 主ループ)
// 受信で捨てたバイトの累計 (リングバッファが満杯か、UART の FIFO のあふれ)。割り込みだけが書く
static volatile uint32_t s_cmdDropped, s_logDropped;
static uint32_t s_cmdDroppedSeen, s_logDroppedSeen;
static absolute_time_t s_nextDropNotice;

// copy は、同じバイトを写しておく 2 つ目のリング (なければ NULL)。写しがあふれた分は数えない
static void rx_drain(uart_inst_t* u, Ring* r, Ring* copy, volatile uint32_t* dropped) {
    while (uart_is_readable(u)) {
        uint32_t dr = uart_get_hw(u)->dr;
        if (dr & UART_UARTDR_OE_BITS) (*dropped)++; // FIFO があふれて 1 バイト以上落ちた
        if (!ring_put(r, (uint8_t)dr)) (*dropped)++;
        if (copy != NULL) ring_put(copy, (uint8_t)dr);
    }
}

static void on_uart0(void) { rx_drain(CMD_UART, &s_cmdRing, NULL, &s_cmdDropped); }
#if PICO_KBD_WIFI
static void on_uart1(void) { rx_drain(LOG_UART, &s_logRing, &s_netLogRing, &s_logDropped); }
#else
static void on_uart1(void) { rx_drain(LOG_UART, &s_logRing, NULL, &s_logDropped); }
#endif

// 応答は 1 行まるごと入るときだけ入れる (ログの途中に切れた応答が混ざらないように)
// 入らなければ何も入れずに false を返す
static bool reply(const char* s) {
    static const char kHead[] = "\r\n#pico ", kTail[] = "\r\n";
    size_t n = strlen(s);
    if (ring_space(&s_replyRing) < sizeof kHead - 1 + n + sizeof kTail - 1) return false;
    for (const char* p = kHead; *p; p++) ring_put(&s_replyRing, (uint8_t)*p);
    for (size_t i = 0; i < n; i++) ring_put(&s_replyRing, (uint8_t)s[i]);
    for (const char* p = kTail; *p; p++) ring_put(&s_replyRing, (uint8_t)*p);
#if PICO_KBD_WIFI
    net_reply(s);
#endif
    return true;
}

bool app_reply(const char* s) { return reply(s); }

// 再起動の前に、たまっている応答を送り切る (ここだけは待つ)
static void flush_replies(void) {
    uint8_t c;
    while (ring_get(&s_replyRing, &c)) uart_putc_raw(CMD_UART, (char)c);
    uart_tx_wait_blocking(CMD_UART);
#if PICO_KBD_WIFI
    net_flush(500);
#endif
}

void app_flush(void) { flush_replies(); }

// ---- 端子を決めた時間だけ Low にする (GP2 = RUN 端子のリセット、GP3 = Pi 5 の電源ボタン) ----
typedef enum { PULSE_IDLE, PULSE_WAIT_TX, PULSE_LOW, PULSE_DONE } PulseState;
typedef struct {
    unsigned pin;
    const char* done; // 終わったときの応答
    bool level;       // 出している間の向き (false = Low)
    PulseState state;
    unsigned ms;
    absolute_time_t end;
    absolute_time_t netWait; // TCP の相手に応答が届くのを待つのは、この時刻まで
} Pulse;
static Pulse s_rst = {.pin = RUN_PIN, .done = "reset done"};
static Pulse s_pwr = {.pin = PWR_PIN, .done = "power done", .level = PICO_KBD_PWR_ACTIVE_HIGH};

static bool net_tx_done(void) {
#if PICO_KBD_WIFI
    return net_tx_idle();
#else
    return true;
#endif
}

static void pulse_start(Pulse* p, unsigned ms) {
    p->ms = ms;
    p->netWait = make_timeout_time_ms(1000);
    p->state = PULSE_WAIT_TX;
}

// 応答を送り終えるのを待っている間は、ログの中継を止める
static bool pulse_waiting_tx(void) { return s_rst.state == PULSE_WAIT_TX || s_pwr.state == PULSE_WAIT_TX; }

static void pulse_task(Pulse* p) {
    switch (p->state) {
    case PULSE_IDLE:
        break;
    case PULSE_WAIT_TX:
        // 試験する Pi から Pico が電源を取っていると Pico も止まるので、"#pico reset ..." や "#pico power ..." を送り終えてから Low にする
        // TCP の相手にも送り終える (相手が受け取りを返してこないときは、1 秒であきらめて押す)
        if (ring_count(&s_replyRing) == 0 && !(uart_get_hw(CMD_UART)->fr & UART_UARTFR_BUSY_BITS) &&
            (net_tx_done() || time_reached(p->netWait))) {
            gpio_put(p->pin, p->level);
            gpio_set_dir(p->pin, GPIO_OUT); // 決めた向きだけを出す (Low ならオープンドレインと同じ使い方)
            p->end = make_timeout_time_ms(p->ms);
            p->state = PULSE_LOW;
        }
        break;
    case PULSE_LOW:
        if (time_reached(p->end)) {
            gpio_set_dir(p->pin, GPIO_IN);
            p->state = PULSE_DONE;
        }
        break;
    case PULSE_DONE:
        // 終わりの応答は、応答のリングに積めるまで待つ (操作機はこの応答を待つ)
        if (reply(p->done)) p->state = PULSE_IDLE;
        break;
    }
}

// ---- USB ----
#ifndef PICO_KBD_PAD_DEFAULT
#define PICO_KBD_PAD_DEFAULT 0
#endif
static bool s_padEnabled = PICO_KBD_PAD_DEFAULT; // 今のつながり (切り離しているなら次のつながり) でゲームパッドを見せるか
static bool s_usbOff;                            // C 命令で切り離している
static bool s_usbReconnect;                      // s_reconnectAt につなぎ直す
static absolute_time_t s_reconnectAt;

bool app_pad_enabled(void) { return s_padEnabled; }

static bool s_mountSeen; // 最後に切り離したあと (起動してから)、試験機が設定を終えた (tud_mount_cb())
static bool s_inFlight;  // 送ったキーボードのレポートを受け手がまだ読んでいない

// キーとゲームパッドを送ってよいか。切り離したあとも、試験機がバスをリセットするまで tud_mounted() は true のままなので、
// 切り離したあとは、試験機が設定し直すまで (tud_mount_cb()) つながっていないとみなす
static bool usb_up(void) { return tud_mounted() && !s_usbOff && s_mountSeen; }

// ゲームパッドの状態。送るのは変わったときだけ。押している時間は、押したレポートが読まれてから数える (core.c の PadState)
static PadState s_pad;

static void pad_reset(void) { pad_init(&s_pad); } // 次につながったら、離した状態を 1 回送る

static void pad_task(void) {
    uint32_t b;
    uint8_t h;
    if (!s_padEnabled || !usb_up() || !tud_hid_n_ready(1)) return;
    if (!pad_next(&s_pad, time_us_64(), &b, &h)) return;
    if (tud_hid_n_gamepad_report(1, 0, 0, 0, 0, 0, 0, 0, h, b)) pad_sent(&s_pad);
}

// 切り離す。まだ送っていないキーとゲームパッドの状態は捨てる
static void usb_off(void) {
    tud_disconnect();
    s_usbOff = true;
    s_usbReconnect = false;
    s_mountSeen = false;
    s_inFlight = false;
    keyq_init();
    pad_reset();
}

static void usb_on(void) {
    s_usbOff = false;
    s_usbReconnect = false;
    tud_connect();
}

static void usbc_task(void) {
    if (s_usbReconnect && time_reached(s_reconnectAt) && reply("usb on")) usb_on();
}

static absolute_time_t s_next; // 次の項目を送ってよい時刻
static uint32_t s_sentWaitUs;  // 送った項目の待ち時間。受け手が読み終えた時刻から数える

static void usb_task(void) {
    const Step* s = keyq_peek();
    if (s == NULL) return;
    if (!time_reached(s_next)) return;
    if (s_usbOff || !tud_hid_n_ready(0)) return;     // つながっていない、または前の送信がまだ読まれていない
    if (s_inFlight) return;
    if (!tud_hid_n_report(0, 0, s->report, 8)) return; // 受け付けられなければ、次の周回でもう一度送る
    // 押している時間は、受け手が読み終えた時刻から数える (tud_hid_report_complete_cb)。
    // 受け手が読みに来るのが遅れても、押してから離すまでの時間が縮まないように
    s_sentWaitUs = s->wait_us;
    s_inFlight = true;
    keyq_pop();
}

// ---- 命令 ----
static unsigned parse_hex(const char** p, bool* ok) {
    while (**p == ' ') (*p)++;
    unsigned v = 0, n = 0;
    for (;;) {
        char c = **p;
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) break;
        v = v * 16 + (unsigned)d;
        n++;
        (*p)++;
    }
    *ok = n > 0;
    return v;
}

static unsigned parse_dec(const char** p, bool* ok) {
    while (**p == ' ') (*p)++;
    unsigned v = 0, n = 0;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (unsigned)(**p - '0');
        n++;
        (*p)++;
    }
    *ok = n > 0;
    return v;
}

static void run(const char* line);
void app_run(const char* line) { run(line); }

static void run(const char* line) {
    char buf[64];
    char cmd = line[0];
    const char* p = line[0] ? line + 1 : line;
    if (*p == ' ') p++;
    bool ok;
    switch (cmd) {
    case 0:
        return;
    case 'P':
        reply(usb_up() ? "ok mounted" : "ok not-mounted");
        return;
    case 'T': {
        // つながる前に入れたキーは、つながったとき (tud_mount_cb()) に捨てられるので、入れずに dropped に数える
        const bool mounted = usb_up();
        unsigned skipped = 0, full = 0;
        for (; *p; p++) {
            char c = *p;
            if (c == '\\' && p[1]) {
                p++;
                c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'e' ? '\x1b' : *p == 'b' ? '\b' : *p;
            }
            uint8_t usage;
            bool shift;
            if (!jis_key(c, &usage, &shift)) { skipped++; continue; }
            if (!mounted || !keyq_tap(usage, shift ? 0x02 : 0, 60)) full++;
        }
        snprintf(buf, sizeof buf, "typed skipped=%u dropped=%u", skipped, full);
        reply(buf);
        return;
    }
    case 'K': {
        unsigned u = parse_hex(&p, &ok);
        if (!ok) break;
        unsigned m = parse_hex(&p, &ok);
        if (!ok) m = 0;
        unsigned ms = parse_dec(&p, &ok);
        if (!ok) ms = 60;
        if (u > 0xff || m > 0xff || ms > 10000) {
            reply("error bad-args");
            return;
        }
        reply(usb_up() && keyq_tap((uint8_t)u, (uint8_t)m, ms) ? "key" : "key dropped");
        return;
    }
    case 'D': {
        unsigned u = parse_hex(&p, &ok);
        if (!ok) break;
        unsigned m = parse_hex(&p, &ok);
        if (!ok) m = 0;
        if (u > 0xff || m > 0xff) {
            reply("error bad-args");
            return;
        }
        reply(usb_up() && keyq_down((uint8_t)u, (uint8_t)m) ? "down" : "down dropped");
        return;
    }
    case 'U':
        // 切り離している間は列に入れない (つなぎ直したとき tud_mount_cb() が列を捨てる)。押したままのキーは忘れる
        if (!usb_up()) {
            keyq_init();
            reply("up dropped");
            return;
        }
        keyq_up();
        reply("up");
        return;
    case 'X': {
        if (s_rst.state != PULSE_IDLE) {
            reply("reset busy");
            return;
        }
        unsigned ms = parse_dec(&p, &ok);
        if (!ok || ms == 0 || ms > 5000) ms = 200;
        snprintf(buf, sizeof buf, "reset %u ms", ms);
        // 応答を入れられないときはリセットしない (知らせずにリセットしないように。送る側は応答がなければ送り直す)
        if (!reply(buf)) return;
        pulse_start(&s_rst, ms);
        return;
    }
    case 'J': {
        if (s_pwr.state != PULSE_IDLE) {
            reply("power busy");
            return;
        }
        unsigned ms;
        if (!pwr_parse(p, &ms)) {
            reply("error bad-args");
            return;
        }
        snprintf(buf, sizeof buf, "power %u ms", ms);
        // X と同じく、応答を入れられないときは押さない
        if (!reply(buf)) return;
        pulse_start(&s_pwr, ms);
        return;
    }
    case 'S':
    case 'W': {
        // 先に長さを数え、全部入るときだけ入れる
        unsigned n = cmd == 'S' ? 1 : 0;
        for (const char* q = p; *q; q++) {
            if (*q == '\\' && q[1]) q++;
            n++;
        }
        if (n > ring_space(&s_piTxRing)) {
            reply("send dropped");
            return;
        }
        for (; *p; p++) {
            char c = *p;
            if (c == '\\' && p[1]) {
                p++;
                c = *p == 'n' ? '\n' : *p == 'r' ? '\r' : *p == 't' ? '\t' : *p == 'e' ? '\x1b' : *p == 'b' ? '\b' : *p;
            }
            ring_put(&s_piTxRing, (uint8_t)c);
        }
        if (cmd == 'S') ring_put(&s_piTxRing, '\n');
        snprintf(buf, sizeof buf, "sent %u", n);
        reply(buf);
        return;
    }
    case 'G': {
        uint32_t b;
        uint8_t h;
        unsigned ms;
        if (!pad_parse(p, &b, &h, &ms)) {
            reply("error bad-args");
            return;
        }
        if (!s_padEnabled) {
            reply("pad none");
            return;
        }
        if (!usb_up()) {
            reply("pad dropped");
            return;
        }
        pad_set(&s_pad, b, h, ms);
        reply("pad");
        return;
    }
    case 'C': {
        UsbcAction a;
        unsigned ms;
        UsbcMode m;
        if (!usbc_parse(p, &a, &ms, &m)) {
            reply("error bad-args");
            return;
        }
        const bool pad = m == USBC_SAME ? s_padEnabled : m == USBC_KBD_PAD;
        if (a == USBC_OFF) {
            usb_off();
            reply("usb off");
            return;
        }
        if (a == USBC_ON && (s_usbOff || pad == s_padEnabled)) {
            s_padEnabled = pad;
            if (s_usbOff) usb_on();
            reply("usb on");
            return;
        }
        // 切り離して、決めた時間のあとにつなぎ直す (つないだまま組み合わせを変える C on は 500 ミリ秒)
        if (a == USBC_ON) ms = 500;
        usb_off();
        s_padEnabled = pad; // 記述子は、つなぎ直したときに試験機が読み直す
        s_usbReconnect = true;
        s_reconnectAt = make_timeout_time_ms(ms);
        snprintf(buf, sizeof buf, "usb off %u ms", ms);
        reply(buf);
        return;
    }
    case 'O':
#if PICO_KBD_OTA
        ota_run(p, ota_ready());
#else
        reply("ota none");
#endif
        return;
    case 'N': {
        // 応答は 1 行にまとめる (送る側は、命令 1 つにつき応答を 1 行待つ)
        char nbuf[192];
#if PICO_KBD_WIFI
        net_status(nbuf, sizeof nbuf - 20);
#else
        snprintf(nbuf, sizeof nbuf, "net none");
#endif
        size_t n = strlen(nbuf);
        snprintf(nbuf + n, sizeof nbuf - n, " wd=%d wd-boot=%d", s_wdOn, s_wdBoot);
        reply(nbuf);
        return;
    }
    case 'Z':
        if (!s_wdOn) {
            reply("hang refused");
            return;
        }
        reply("hang");
        flush_replies();
        for (;;) tight_loop_contents();
    case 'R':
        reply("reboot");
        flush_replies();
        watchdog_reboot(0, 0, 10);
        for (;;) tight_loop_contents();
    case 'B':
        reply("bootsel");
        flush_replies();
        // 書き込みモードの間にウォッチドッグが再起動しないよう、止めてから移る
        hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
        reset_usb_boot(0, 0);
        return;
    default:
        break;
    }
    snprintf(buf, sizeof buf, "? %.40s", line);
    reply(buf);
}

// ---- UART ----
static LineBuf s_line;

static void uart_task(void) {
    // 命令
    uint8_t c;
    while (ring_get(&s_cmdRing, &c)) {
        LineResult r = line_feed(&s_line, (char)c);
        if (r == LINE_READY) run(s_line.buf);
        else if (r == LINE_TOO_LONG) reply("error too-long");
    }
    // 受信で捨てたバイトを知らせる (多くても 100ms に 1 回)
    uint32_t cd = s_cmdDropped, ld = s_logDropped;
    if ((cd != s_cmdDroppedSeen || ld != s_logDroppedSeen) && time_reached(s_nextDropNotice)) {
        char buf[48];
        // 積めなかったら数を進めず、次の通知で合わせて知らせる
        if (cd != s_cmdDroppedSeen) {
            snprintf(buf, sizeof buf, "error cmd-dropped %lu", (unsigned long)(cd - s_cmdDroppedSeen));
            if (reply(buf)) s_cmdDroppedSeen = cd;
        }
        if (ld != s_logDroppedSeen) {
            snprintf(buf, sizeof buf, "log-dropped %lu", (unsigned long)(ld - s_logDroppedSeen));
            if (reply(buf)) s_logDroppedSeen = ld;
        }
        s_nextDropNotice = make_timeout_time_ms(100);
    }
#if PICO_KBD_WIFI
    // 試験機のログの写しを TCP の相手に回す (相手がいなければ捨てる)
    while (ring_get(&s_netLogRing, &c)) net_log_byte((char)c);
#endif
    // 試験機の UART への送信 (S / W 命令)
    while (uart_is_writable(LOG_UART) && ring_get(&s_piTxRing, &c)) uart_putc_raw(LOG_UART, (char)c);
    // UART0 への送信: 応答を先に、ログをあとに。X と J が応答の送信を待つ間はログを止めて、応答を送り終えられるようにする
    while (uart_is_writable(CMD_UART)) {
        if (ring_get(&s_replyRing, &c) || (!pulse_waiting_tx() && ring_get(&s_logRing, &c)))
            uart_putc_raw(CMD_UART, (char)c);
        else
            break;
    }
}

int main(void) {
    s_wdBoot = watchdog_enable_caused_reboot();
    board_init();
#if PICO_KBD_OTA
    ota_init(); // 試している版 (RP2040) は、ここでブートローダが掛けたウォッチドッグを掛け直す
#endif
    // 無線のチップの初期化で固まっても再起動するように、ここから掛ける。ただし、この起動がウォッチドッグの時間切れによるものなら、
    // 初期化が済んでから掛ける (初期化が 8 秒より長くかかる状態のとき、再起動を繰り返さないように)
    if (!s_wdBoot) wd_task();
#if PICO_KBD_WIFI
    net_init(); // 数秒かかるので、USB を始める前に済ませる
#endif
    keyq_init();
    uart_init(CMD_UART, BAUD);
    gpio_set_function(0, GPIO_FUNC_UART);
    gpio_set_function(1, GPIO_FUNC_UART);
    uart_init(LOG_UART, BAUD);
    gpio_set_function(4, GPIO_FUNC_UART); // S / W 命令で試験機に送る
    gpio_set_function(5, GPIO_FUNC_UART);
    // 試験機が GPIO 14 を出力にするまで (リセット中や起動の初め) 線が Low のままだと、0x00 (ブレーク) を中継してしまう
    gpio_pull_up(5);
    gpio_pull_up(1);
    irq_set_exclusive_handler(UART0_IRQ, on_uart0);
    irq_set_exclusive_handler(UART1_IRQ, on_uart1);
    irq_set_enabled(UART0_IRQ, true);
    irq_set_enabled(UART1_IRQ, true);
    uart_set_irqs_enabled(CMD_UART, true, false);
    uart_set_irqs_enabled(LOG_UART, true, false);
    gpio_init(RUN_PIN); // 入力 (Hi-Z)。引き上げも引き下げもしない
    gpio_disable_pulls(RUN_PIN);
    // 電源ボタンの端子も同じ。入力としては読まないので、入力バッファを切る
    // (RP2350 の A2 は、入力バッファを入れたまま端子を浮かせると漏れ電流が増える。RP2350-E9)
    gpio_init(PWR_PIN);
    gpio_disable_pulls(PWR_PIN);
    gpio_set_input_enabled(PWR_PIN, false);
    tusb_init();
    if (board_init_after_tusb) board_init_after_tusb();
    reply(s_wdBoot ? "started wd" : "started");
    for (;;) {
        wd_task();
        tud_task();
        usb_task();
        pad_task();
        usbc_task();
        pulse_task(&s_rst);
        pulse_task(&s_pwr);
        uart_task();
#if PICO_KBD_WIFI
        net_task();
#endif
#if PICO_KBD_OTA
        ota_task(ota_ready());
#endif
#if PICO_KBD_WIFI && PICO_KBD_WIFI_REBOOT_MIN > 0
        // 無線 LAN につながらないまま長くたったら、再起動して無線のチップを初期化し直す (端子を動かしている間は待つ)
        if (net_down_ms() >= PICO_KBD_WIFI_REBOOT_MIN * 60000u && s_rst.state == PULSE_IDLE && s_pwr.state == PULSE_IDLE) {
            reply("wifi down reboot");
            flush_replies();
            watchdog_reboot(0, 0, 10);
            for (;;) tight_loop_contents();
        }
#endif
    }
}

// ---- TinyUSB の呼び出し ----
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type, uint8_t* buffer, uint16_t reqlen) {
    (void)report_id;
    (void)type;
    if (instance == 1) { // ゲームパッド: 今の状態
        hid_gamepad_report_t r = {.hat = s_pad.hat, .buttons = s_pad.buttons};
        if (reqlen < sizeof r) return 0;
        memcpy(buffer, &r, sizeof r);
        return sizeof r;
    }
    if (reqlen < 8) return 0;
    memset(buffer, 0, 8);
    return 8;
}

// 受け手 (試験する Pi) が送ってきた LED の状態を知らせる (pix68k がこのキーボードを使っているかの目安)
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type, uint8_t const* buffer, uint16_t size) {
    (void)report_id;
    if (instance == 0 && type == HID_REPORT_TYPE_OUTPUT && size >= 1) {
        char buf[24];
        snprintf(buf, sizeof buf, "led %02x", buffer[0]);
        reply(buf);
    }
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const* report, uint16_t len) {
    (void)report;
    (void)len;
    if (instance == 1) { // ゲームパッド: 押している時間は、ここから数える
        pad_complete(&s_pad, time_us_64());
        return;
    }
    if (instance != 0) return;
    s_next = make_timeout_time_us(s_sentWaitUs);
    s_inFlight = false;
}

// 送れなかったときも、次の項目に進めるようにする
void tud_hid_report_failed_cb(uint8_t instance, hid_report_type_t report_type, uint8_t const* report,
                              uint16_t xferred_bytes) {
    (void)report_type;
    (void)report;
    (void)xferred_bytes;
    if (instance == 1) { // ゲームパッドは、送れなかった状態をもう一度送る
        pad_failed(&s_pad);
        return;
    }
    if (instance != 0) return;
    s_next = make_timeout_time_us(s_sentWaitUs);
    s_inFlight = false;
}

// 試験機が止まったとき (suspend) と、つながり直したとき (mount) も残っているキーを捨てる。
// RP2040 の TinyUSB は VBUS を見ないので、試験機のリセットでは tud_umount_cb() が呼ばれない
// (Pico を試験機以外から給電しているとき、リセット前に送った T の残りが起動し直した試験機に届かないように)
// suspend では、押すレポートを読まれたあと離すレポートを捨てることがあるので、すべて離したレポートを 1 つ残す。
// 再列挙なしで resume したときは、それが最初に送られてキーが押されたままにならない
void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    keyq_init();
    keyq_up();
    s_inFlight = false;
    s_next = get_absolute_time();
    pad_reset();
}

void tud_mount_cb(void) {
    s_mountSeen = true;
    keyq_init();
    pad_reset();
    s_inFlight = false;
    s_next = get_absolute_time(); // 前のつながりの待ち時間 (K の ms など) を持ち越さない
    reply("mounted");
}
// つながりが切れたら、残っているキーを捨てる
void tud_umount_cb(void) {
    s_mountSeen = false;
    keyq_init();
    pad_reset();
    s_inFlight = false;
    reply("unmounted");
}
