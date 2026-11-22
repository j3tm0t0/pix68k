// core.c と ring.h をホストで試す: cc -std=c11 -Wall -Wextra -o test_host test_host.c core.c && ./test_host
#include <stdio.h>
#include <string.h>
#include "core.h"
#include "ring.h"

static int s_fail;
#define CHECK(x) do { if (!(x)) { printf("NG %s:%d: %s\n", __FILE__, __LINE__, #x); s_fail++; } } while (0)

// キューを空にし、最後に送ったレポートを返す
static Step drain(unsigned* count) {
    Step last = {{0}, 0};
    unsigned n = 0;
    const Step* s;
    while ((s = keyq_peek()) != NULL) { last = *s; keyq_pop(); n++; }
    if (count) *count = n;
    return last;
}

static bool report_has(const Step* s, uint8_t usage) {
    for (int i = 2; i < 8; i++) if (s->report[i] == usage) return true;
    return false;
}

static void test_up_when_full(void) {
    keyq_init();
    CHECK(keyq_down(0x04, 0));
    while (keyq_tap(0x05, 0, 60)) {}
    if (keyq_free() == 2) CHECK(keyq_down(0x08, 0));
    // 満杯にしたあとも、D と K は入らず、U は入る
    CHECK(keyq_free() == 1);
    CHECK(!keyq_tap(0x05, 0, 60));
    CHECK(!keyq_down(0x06, 0));
    // 入らなかった D 06 は押している状態を変えない (U を挟まずに、空きを作ってから確かめる)
    keyq_pop();
    keyq_pop();
    CHECK(keyq_tap(0x09, 0, 60));
    {
        const Step* s = NULL;
        unsigned n = 0;
        // 足したばかりの tap の押すレポートは、末尾から 2 つ目
        for (const Step* q; (q = keyq_peek()) != NULL && keyq_free() < KEYQ_SIZE - 3; keyq_pop()) n++;
        s = keyq_peek();
        CHECK(n > 0 && s != NULL && report_has(s, 0x09) && report_has(s, 0x04) && !report_has(s, 0x06));
    }
    while (keyq_tap(0x05, 0, 60)) {}
    keyq_up();
    CHECK(keyq_free() == 0);
    keyq_up(); // 続けて U を送っても、キューは壊れず、すべて離した状態のまま
    unsigned count = 0;
    Step last = drain(&count);
    CHECK(count == KEYQ_SIZE - 1);
    static const uint8_t zero[8] = {0};
    CHECK(memcmp(last.report, zero, 8) == 0);
    // 入らなかった D 06 は押したままに残らない
    CHECK(keyq_tap(0x07, 0, 60));
    Step press = *keyq_peek();
    CHECK(report_has(&press, 0x07) && !report_has(&press, 0x06) && !report_has(&press, 0x04) && !report_has(&press, 0x08));
    drain(NULL);
}

static void test_up_every_fill_level(void) {
    // どの埋まり具合からでも、U のあとに最後に送るレポートはすべて離したもの
    for (unsigned fill = 0; fill < KEYQ_SIZE; fill++) {
        keyq_init();
        CHECK(keyq_down(0x04, 0x01));
        for (unsigned i = 0; i < fill; i++) {
            if (i % 3 == 0) keyq_down(0x05 + (uint8_t)(i % 4), 0);
            else keyq_tap(0x10, 0, 60);
        }
        keyq_up();
        Step last = drain(NULL);
        static const uint8_t zero[8] = {0};
        CHECK(memcmp(last.report, zero, 8) == 0);
    }
}

static void test_tap_and_down(void) {
    keyq_init();
    CHECK(keyq_down(0x4C, 0x05)); // Ctrl + Alt + Delete
    const Step* s = keyq_peek();
    CHECK(s->report[0] == 0x05 && s->report[2] == 0x4C);
    keyq_pop();
    CHECK(keyq_tap(0x04, 0x02, 60));
    s = keyq_peek();
    CHECK(s->report[0] == 0x07 && report_has(s, 0x4C) && report_has(s, 0x04) && s->wait_us == 60000);
    keyq_pop();
    s = keyq_peek();
    CHECK(s->report[0] == 0x05 && report_has(s, 0x4C) && !report_has(s, 0x04));
    keyq_pop();
    CHECK(keyq_peek() == NULL);
}

static void test_ring(void) {
    static uint8_t storage[8];
    Ring r = RING_INIT(storage);
    uint8_t c;
    CHECK(!ring_get(&r, &c));
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 8; i++) CHECK(ring_put(&r, (uint8_t)(round * 10 + i)));
        CHECK(!ring_put(&r, 0xFF));
        CHECK(ring_count(&r) == 8 && ring_space(&r) == 0);
        for (int i = 0; i < 8; i++) CHECK(ring_get(&r, &c) && c == round * 10 + i);
        CHECK(!ring_get(&r, &c));
    }
    // 累計が 32 ビットを回っても数えられる
    r.head = r.tail = 0xFFFFFFFEu;
    for (int i = 0; i < 5; i++) CHECK(ring_put(&r, (uint8_t)i));
    CHECK(ring_count(&r) == 5);
    for (int i = 0; i < 5; i++) CHECK(ring_get(&r, &c) && c == i);
}

static void test_line(void) {
    static LineBuf lb;
    const char* in = "P\r\nT abc\n";
    LineResult r = LINE_NONE;
    int ready = 0;
    for (const char* p = in; *p; p++) {
        r = line_feed(&lb, *p);
        if (r == LINE_READY) {
            ready++;
            CHECK(strcmp(lb.buf, ready == 1 ? "P" : "T abc") == 0);
        }
    }
    CHECK(ready == 2);
    // ちょうど LINE_MAX バイトは通る
    for (int i = 0; i < LINE_MAX; i++) CHECK(line_feed(&lb, 'a') == LINE_NONE);
    CHECK(line_feed(&lb, '\n') == LINE_READY && strlen(lb.buf) == LINE_MAX);
    // LINE_MAX + 1 バイトは改行まで捨てて LINE_TOO_LONG
    for (int i = 0; i < LINE_MAX + 100; i++) CHECK(line_feed(&lb, 'b') == LINE_NONE);
    CHECK(line_feed(&lb, '\n') == LINE_TOO_LONG);
    // 次の行はふつうに読める
    line_feed(&lb, 'U');
    CHECK(line_feed(&lb, '\n') == LINE_READY && strcmp(lb.buf, "U") == 0);
}

static void test_jis(void) {
    uint8_t u;
    bool sh;
    CHECK(jis_key('a', &u, &sh) && u == 0x04 && !sh);
    CHECK(jis_key('"', &u, &sh) && u == 0x1F && sh);
    CHECK(jis_key('_', &u, &sh) && u == 0x87 && sh);
    CHECK(jis_key('|', &u, &sh) && u == 0x89 && sh);
    CHECK(jis_key('}', &u, &sh) && u == 0x32 && sh);
    CHECK(!jis_key(0, &u, &sh));
    // 印字できる ASCII はすべて打てる
    for (int c = 0x20; c < 0x7F; c++) CHECK(jis_key((char)c, &u, &sh));
}

// 押したままのキーが 6 個あると、K と D は足せずに false を返す (すでに押しているキーなら受ける)
static void test_six_held(void) {
    keyq_init();
    for (uint8_t k = 0x04; k < 0x0a; k++) CHECK(keyq_down(k, 0));
    unsigned before = keyq_free();
    CHECK(!keyq_down(0x10, 0));
    CHECK(!keyq_tap(0x10, 0, 60));
    CHECK(keyq_free() == before);
    CHECK(keyq_down(0x04, 0));
    CHECK(keyq_tap(0x05, 0, 60));
    keyq_init();
}

// J 命令の引数
static void test_pwr(void) {
    unsigned ms = 0;
    CHECK(pwr_parse("", &ms) && ms == PWR_SHORT_MS);
    CHECK(pwr_parse("S", &ms) && ms == PWR_SHORT_MS);
    CHECK(pwr_parse("L", &ms) && ms == PWR_LONG_MS);
    CHECK(pwr_parse(" L ", &ms) && ms == PWR_LONG_MS);
    CHECK(pwr_parse("1", &ms) && ms == 1);
    CHECK(pwr_parse("15000", &ms) && ms == 15000);
    ms = 7;
    CHECK(!pwr_parse("0", &ms));
    CHECK(!pwr_parse("15001", &ms));
    CHECK(!pwr_parse("99999999999999999999", &ms));
    CHECK(!pwr_parse("x", &ms));
    CHECK(!pwr_parse("l", &ms));
    CHECK(!pwr_parse("LL", &ms));
    CHECK(!pwr_parse("L 5", &ms));
    CHECK(!pwr_parse("12a", &ms));
    CHECK(!pwr_parse("-5", &ms));
    CHECK(ms == 7); // 失敗したときは書かない
}

// TCP の相手のトークン
static void test_token(void) {
    CHECK(token_eq("abcd1234", "abcd1234"));
    CHECK(!token_eq("abcd1235", "abcd1234"));
    CHECK(!token_eq("abcd123", "abcd1234"));
    CHECK(!token_eq("abcd12345", "abcd1234"));
    CHECK(!token_eq("", "abcd1234"));
    CHECK(!token_eq("", "")); // トークンが空なら、だれも通さない
    CHECK(!token_eq("x", ""));
}

// 試験機のログを行に切る
static void test_logline(void) {
    static LogLine l;
    memset(&l, 0, sizeof l);
    const char* in = "ab\r\n\ncd";
    unsigned lines = 0;
    for (const char* p = in; *p; p++) {
        if (!logline_feed(&l, *p)) continue;
        if (lines == 0) CHECK(strcmp(l.buf, "ab") == 0);
        if (lines == 1) CHECK(strcmp(l.buf, "") == 0);
        lines++;
    }
    CHECK(lines == 2);
    // 改行の来ない残りは flush で 1 行になる。2 回目は何も返さない
    CHECK(logline_flush(&l) && strcmp(l.buf, "cd") == 0);
    CHECK(!logline_flush(&l));
    // 長い行は LOGLINE_MAX で切り、続きは次の行になる
    lines = 0;
    for (unsigned i = 0; i < LOGLINE_MAX + 3; i++) {
        if (logline_feed(&l, 'x')) {
            CHECK(strlen(l.buf) == LOGLINE_MAX);
            lines++;
        }
    }
    CHECK(lines == 1);
    CHECK(logline_feed(&l, '\n') && strcmp(l.buf, "xxx") == 0);
    CHECK(!logline_flush(&l));
}

// O 命令の base64 と CRC-32
static void test_base64_crc(void) {
    uint8_t out[16];
    CHECK(base64_decode("", out, sizeof out) == 0);
    CHECK(base64_decode("QQ==", out, sizeof out) == 1 && out[0] == 'A');
    CHECK(base64_decode("QUI=", out, sizeof out) == 2 && memcmp(out, "AB", 2) == 0);
    CHECK(base64_decode("QUJD", out, sizeof out) == 3 && memcmp(out, "ABC", 3) == 0);
    CHECK(base64_decode("//79AA==", out, sizeof out) == 4 && out[0] == 0xff && out[1] == 0xfe && out[2] == 0xfd && out[3] == 0);
    CHECK(base64_decode("QUJ", out, sizeof out) == -1);      // 4 文字の倍数でない
    CHECK(base64_decode("QU=D", out, sizeof out) == -1);     // = のあとに文字
    CHECK(base64_decode("QQ==QUJD", out, sizeof out) == -1); // 途中に =
    CHECK(base64_decode("QU J", out, sizeof out) == -1);     // 空白
    CHECK(base64_decode("====", out, sizeof out) == -1);
    CHECK(base64_decode("QUJDQUJD", out, 5) == -1);          // 入りきらない
    CHECK(base64_decode("QUJDQUJD", out, 6) == 6);
    // zlib.crc32(b"123456789") == 0xcbf43926
    CHECK(crc32_update(0, (const uint8_t*)"123456789", 9) == 0xcbf43926u);
    CHECK(crc32_update(crc32_update(0, (const uint8_t*)"1234", 4), (const uint8_t*)"56789", 5) == 0xcbf43926u);
    CHECK(crc32_update(0, NULL, 0) == 0);
}

static void test_pad_parse(void) {
    uint32_t b = 99;
    uint8_t h = 99;
    unsigned ms = 99;
    CHECK(pad_parse("", &b, &h, &ms) && b == 0 && h == 0 && ms == 0);   // すべて離す
    CHECK(pad_parse("  ", &b, &h, &ms) && b == 0 && h == 0 && ms == 0);
    CHECK(pad_parse("1", &b, &h, &ms) && b == 1 && h == 0 && ms == 0);  // 押したまま
    CHECK(pad_parse("ffffffff 8 10000", &b, &h, &ms) && b == 0xffffffffu && h == 8 && ms == 10000);
    CHECK(pad_parse(" 0 1 200 ", &b, &h, &ms) && b == 0 && h == 1 && ms == 200);
    CHECK(pad_parse("Ab 3", &b, &h, &ms) && b == 0xab && h == 3 && ms == 0);
    b = 7;
    CHECK(!pad_parse("100000000", &b, &h, &ms));  // 32 ビットを超える
    CHECK(b == 7);                                // 失敗したときは書き換えない
    CHECK(!pad_parse("1 9", &b, &h, &ms));        // ハットは 8 まで
    CHECK(!pad_parse("1 0 10001", &b, &h, &ms));  // 10 秒まで
    CHECK(!pad_parse("1 0 99999999999", &b, &h, &ms));
    CHECK(!pad_parse("1 0 100 5", &b, &h, &ms));  // 余計な引数
    CHECK(!pad_parse("1 0 0", &b, &h, &ms));      // 明示した 0 ミリ秒は断る (押したままは、ミリ秒を書かない)
    CHECK(!pad_parse("x", &b, &h, &ms));
    CHECK(!pad_parse("1x", &b, &h, &ms));
    CHECK(!pad_parse("1 2a", &b, &h, &ms));
}

static void test_pad_state(void) {
    PadState s;
    uint32_t b;
    uint8_t h;
    // 起動したとき: 離した状態を 1 回送る
    pad_init(&s);
    CHECK(pad_next(&s, 0, &b, &h) && b == 0 && h == 0);
    pad_sent(&s);
    CHECK(!pad_next(&s, 0, &b, &h)); // 読まれるまで次を送らない
    // 前のレポートが読まれる前に G 1 0 1。読まれるのが 5ms 遅れても、押したレポートは送られ、そこから 1ms 押す
    pad_set(&s, 1, 0, 1);
    CHECK(!pad_next(&s, 5000, &b, &h));
    pad_complete(&s, 5000); // 前の (離した) レポートが読まれた。ここでは数え始めない
    CHECK(!s.releasePending);
    CHECK(pad_next(&s, 5000, &b, &h) && b == 1);
    pad_sent(&s);
    CHECK(!pad_next(&s, 20000, &b, &h)); // 押したレポートがまだ読まれていない。時間が過ぎても離さない
    pad_complete(&s, 20000);             // 読まれた。ここから 1ms
    CHECK(!pad_next(&s, 20999, &b, &h));
    CHECK(pad_next(&s, 21000, &b, &h) && b == 0 && h == 0);
    pad_sent(&s);
    pad_complete(&s, 22000);
    CHECK(!s.releasePending && !pad_next(&s, 100000, &b, &h)); // 離したレポートでは数え始めない
    // 押したまま (ミリ秒なし) は離さない
    pad_set(&s, 2, 3, 0);
    CHECK(pad_next(&s, 0, &b, &h) && b == 2 && h == 3);
    pad_sent(&s);
    pad_complete(&s, 0);
    CHECK(!pad_next(&s, 100000000, &b, &h));
    // 送れなかったときは、同じ状態を送り直し、押す時間はその送信が読まれてから数える
    pad_set(&s, 4, 0, 10);
    CHECK(pad_next(&s, 0, &b, &h) && b == 4);
    pad_sent(&s);
    pad_failed(&s);
    CHECK(!s.releasePending);
    CHECK(pad_next(&s, 50000, &b, &h) && b == 4);
    pad_sent(&s);
    pad_complete(&s, 60000);
    CHECK(!pad_next(&s, 69999, &b, &h));
    CHECK(pad_next(&s, 70000, &b, &h) && b == 0);
    pad_sent(&s);
    pad_complete(&s, 70000);
    // 押している間に別の G が来たら、前の離す時刻は捨てる
    pad_set(&s, 8, 0, 5);
    CHECK(pad_next(&s, 0, &b, &h));
    pad_sent(&s);
    pad_complete(&s, 0);
    pad_set(&s, 16, 0, 0);
    CHECK(pad_next(&s, 1000, &b, &h) && b == 16);
    pad_sent(&s);
    pad_complete(&s, 1000);
    CHECK(!pad_next(&s, 10000000, &b, &h)); // 5ms で離さない
}

static void test_usbc_parse(void) {
    UsbcAction a;
    unsigned ms;
    UsbcMode m;
    CHECK(usbc_parse("off", &a, &ms, &m) && a == USBC_OFF && m == USBC_SAME);
    CHECK(usbc_parse(" off ", &a, &ms, &m) && a == USBC_OFF);
    CHECK(usbc_parse("on", &a, &ms, &m) && a == USBC_ON && m == USBC_SAME);
    CHECK(usbc_parse("on kg", &a, &ms, &m) && a == USBC_ON && m == USBC_KBD_PAD);
    CHECK(usbc_parse("on k", &a, &ms, &m) && a == USBC_ON && m == USBC_KBD);
    CHECK(usbc_parse("3000", &a, &ms, &m) && a == USBC_CYCLE && ms == 3000 && m == USBC_SAME);
    CHECK(usbc_parse("100 kg", &a, &ms, &m) && a == USBC_CYCLE && ms == 100 && m == USBC_KBD_PAD);
    CHECK(!usbc_parse("99", &a, &ms, &m));  // 100 ミリ秒より短い切り離しは受けない
    CHECK(!usbc_parse("1 kg", &a, &ms, &m));
    CHECK(usbc_parse("60000 k", &a, &ms, &m) && ms == 60000 && m == USBC_KBD);
    CHECK(!usbc_parse("", &a, &ms, &m));
    CHECK(!usbc_parse("0", &a, &ms, &m));
    CHECK(!usbc_parse("60001", &a, &ms, &m));
    CHECK(!usbc_parse("off k", &a, &ms, &m));
    CHECK(!usbc_parse("on g", &a, &ms, &m));
    CHECK(!usbc_parse("on kgx", &a, &ms, &m));
    CHECK(!usbc_parse("onn", &a, &ms, &m));
    CHECK(!usbc_parse("100 kg 1", &a, &ms, &m));
    CHECK(!usbc_parse("100ms", &a, &ms, &m));
}

int main(void) {
    test_pad_parse();
    test_usbc_parse();
    test_pad_state();
    test_base64_crc();
    test_token();
    test_logline();
    test_pwr();
    test_six_held();
    test_up_when_full();
    test_up_every_fill_level();
    test_tap_and_down();
    test_ring();
    test_line();
    test_jis();
    // RP2350 のイメージの TBYB
    {
        uint8_t img[64] = {0};
        CHECK(picobin_tbyb(img, sizeof img) == -1);
        static const uint8_t kDef[] = {0xd3, 0xde, 0xff, 0xff, 0x42, 0x01, 0x21, 0x90};
        memcpy(img + 20, kDef, sizeof kDef);
        CHECK(picobin_tbyb(img, sizeof img) == 1);
        img[27] = 0x10;
        CHECK(picobin_tbyb(img, sizeof img) == 0);
        CHECK(picobin_tbyb(img, 27) == -1); // 途中で切れている
        img[24] = 0x41; // IMAGE_TYPE の項目でない
        CHECK(picobin_tbyb(img, sizeof img) == -1);
    }
    if (s_fail) { printf("%d failed\n", s_fail); return 1; }
    printf("ok\n");
    return 0;
}
