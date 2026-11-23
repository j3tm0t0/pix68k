// test_host.c -- core.c (FUK の偽物の FDC) をホストで試す。Pico も pico-sdk も要らない
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                        \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

// ---- 偽の時計と、ポートの記録 ----
static uint64_t now = 1000000;
static uint64_t clk(void) { return now; }

static uint8_t p1buf[64];
static int p1n;
static void out1(uint8_t b) { p1buf[p1n++ & 63] = b; }

#define P2MAX 65536
static uint8_t p2[P2MAX];
static uint64_t p2t[P2MAX]; // 届いた時刻
static int p2n, p2r;
static size_t p2limit; // 0 でなければ、1 回に受け付けるバイト数の上限 (送るバッファが一杯のときのまね)
static int p2block;    // 真なら何も受け付けない
static size_t out2(const uint8_t *p, size_t n)
{
    if (p2block) return 0;
    if (p2limit && n > p2limit) n = p2limit;
    for (size_t i = 0; i < n; i++) {
        p2t[p2n] = now;
        p2[p2n++] = p[i];
    }
    return n;
}

static uint8_t images[FUKEMU_UNITS][FUKEMU_IMAGE_BYTES];
static int writes, write_fail;
static int write_image(int unit, uint32_t off, const uint8_t *p, size_t n)
{
    if (write_fail) return -1;
    memcpy(&images[unit][off], p, n);
    writes++;
    return 0;
}

static int verbose;
static void logline(const char *s)
{
    if (verbose) printf("  log: %s\n", s);
}

static const fukemu_io_t io = {clk, out1, out2, write_image, logline, NULL};

static void setup(const fukemu_config_t *cfg, bool wp1)
{
    for (int u = 0; u < FUKEMU_UNITS; u++)
        for (uint32_t i = 0; i < FUKEMU_IMAGE_BYTES; i++) images[u][i] = (uint8_t)(i * 7 + (i >> 10) * 13 + u * 101);
    const uint8_t *im[FUKEMU_UNITS] = {images[0], images[1]};
    bool wp[FUKEMU_UNITS] = {false, wp1};
    fukemu_init(&io, cfg, im, wp);
    p1n = p2n = p2r = 0;
}

// 時刻を t まで進める。途中で送るものがあれば、その時刻で poll する
static void run_until(uint64_t t)
{
    for (int same = 0; same < 4;) { // 送るバッファが詰まっていると、同じ時刻で進まないことがある
        uint64_t d = fukemu_next_due();
        if (d > t) break;
        int before = p2n, moved = d > now;
        if (moved) now = d;
        fukemu_poll();
        same = moved || p2n != before ? 0 : same + 1;
    }
    now = t;
    fukemu_poll();
}
static void run_us(uint64_t us) { run_until(now + us); }

static void w(int reg, uint8_t v)
{
    fukemu_port1_rx((uint8_t)(0x80 | reg));
    fukemu_port1_rx(v);
}
static uint8_t r(int reg)
{
    int before = p1n;
    fukemu_port1_rx((uint8_t)reg);
    CHECK(p1n == before + 1);
    return p1buf[(p1n - 1) & 63];
}
static void command(const uint8_t *c, int n)
{
    for (int i = 0; i < n; i++) w(0x03, c[i]);
}
static int p2avail(void) { return p2n - p2r; }
static uint8_t p2get(void) { return p2r < p2n ? p2[p2r++] : 0xEE; }

static void motor_on(int unit)
{
    w(0x07, (uint8_t)(0x80 | unit));
    w(0x05, (uint8_t)(1 << unit));
}

static void seek(int unit, int cyl)
{
    uint8_t c[3] = {0x0F, (uint8_t)unit, (uint8_t)cyl};
    command(c, 3);
    run_us(10);
    CHECK(p2get() == 'C');
    uint8_t s[1] = {0x08};
    command(s, 1);
    CHECK(p2get() == 'c');
    CHECK(r(0x01) == (0xD0 | 1 << unit)); // ST0 を読むまでは、そのユニットのシーク中のビットが残る
    CHECK(r(0x03) == (0x20 | unit));
    CHECK(r(0x03) == cyl);
    CHECK(r(0x01) == 0x80);
}

static void test_probe_and_reset(void)
{
    setup(NULL, false);
    CHECK(r(0x00) == 'f');
    fukemu_port2_rx(0x00);
    CHECK(p2get() == 'F');
    w(0x00, 0xFE);
    fukemu_port1_rx(0x40);
    CHECK(r(0x01) == 0x80);
    // ドライブの状態: 選んだユニットにイメージがあれば bit7
    w(0x05, 0x01);
    CHECK(r(0x05) == 0x80);
    w(0x05, 0x02);
    CHECK(r(0x05) == 0x80);
    w(0x05, 0x04);
    CHECK(r(0x05) == 0x00);
    // イメージなしのユニット 1
    const uint8_t *im[FUKEMU_UNITS] = {images[0], NULL};
    fukemu_init(&io, NULL, im, NULL);
    w(0x05, 0x02);
    CHECK(r(0x05) == 0x00);
}

static void test_seek_and_sense(void)
{
    setup(NULL, false);
    motor_on(0);
    seek(0, 5);
    // SENSE DEVICE STATUS
    uint8_t c[2] = {0x04, 0x00};
    command(c, 2);
    CHECK(r(0x01) == 0xD0);
    CHECK(r(0x03) == 0x28); // レディ、両面
    // 引き取る割り込みがないときの SENSE INTERRUPT STATUS
    uint8_t s[1] = {0x08};
    command(s, 1);
    CHECK(r(0x03) == 0x80);
    CHECK(r(0x01) == 0x80);
    // 不正なコマンド
    uint8_t bad[1] = {0x1F};
    command(bad, 1);
    CHECK(r(0x03) == 0x80);
}

static void read_cmd(int unit, int cyl, int head, int sec, int eot)
{
    uint8_t c[9] = {0x46, (uint8_t)(head << 2 | unit), (uint8_t)cyl, (uint8_t)head, (uint8_t)sec, 3, (uint8_t)eot, 0x35, 0xFF};
    command(c, 9);
}

// 'R' のフレームを受け取ってデータとリザルトに分ける。届いていなければ 0
static int take_read(uint8_t *data, uint8_t res[7])
{
    if (p2avail() < 3 || p2[p2r] != 'R') return 0;
    int len = p2[p2r + 1] | p2[p2r + 2] << 8;
    if (p2avail() < 3 + len) return 0;
    p2r += 3;
    for (int i = 0; i < len - 7; i++) data[i] = p2get();
    for (int i = 0; i < 7; i++) res[i] = p2get();
    return len - 7;
}

static uint8_t buf[FUKEMU_SECS * FUKEMU_SECSIZE];

static void test_read(void)
{
    setup(NULL, false);
    uint8_t res[7];
    // モータが止まっている: ノットレディ
    read_cmd(0, 0, 0, 1, 1);
    run_us(1);
    CHECK(take_read(buf, res) == 0 && res[0] == 0x48 && p2avail() == 0);
    CHECK(r(0x01) == 0x80);
    motor_on(0);
    read_cmd(0, 0, 0, 1, 1);
    run_us(1);
    CHECK(take_read(buf, res) == 1024);
    CHECK(memcmp(buf, images[0], 1024) == 0);
    CHECK(res[0] == 0 && res[5] == 1);
    // トラック全体 (C2 H1 R1-8)、すぐに届く
    uint64_t t0 = now;
    read_cmd(0, 2, 1, 1, 8);
    run_us(0);
    CHECK(take_read(buf, res) == 8192);
    CHECK(p2t[p2n - 1] == t0);
    CHECK(memcmp(buf, &images[0][(2 * 2 + 1) * 8 * 1024], 8192) == 0);
    CHECK(res[0] == 0x04 && res[3] == 2 && res[4] == 1 && res[5] == 8 && res[6] == 3);
    CHECK(r(0x01) == 0x80);
    // ユニット 1 は別のイメージ
    motor_on(1);
    read_cmd(1, 0, 0, 3, 3);
    run_us(0);
    CHECK(take_read(buf, res) == 1024);
    CHECK(memcmp(buf, &images[1][2 * 1024], 1024) == 0);
    CHECK(res[0] == 0x01);
    // 範囲の外: MA
    read_cmd(0, 80, 0, 1, 1);
    run_us(0);
    CHECK(take_read(buf, res) == 0 && res[0] == 0x40 && res[1] == 0x01);
    // ユニット 2 はディスクなし
    read_cmd(2, 0, 0, 1, 1);
    run_us(0);
    CHECK(take_read(buf, res) == 0 && res[0] == 0x4A);
}

static void write_cmd(int op, int unit, int cyl, int head, int sec, int eot, const uint8_t *data, int n)
{
    w(0x30, (uint8_t)n);
    w(0x31, (uint8_t)(n >> 8));
    uint8_t c[9] = {(uint8_t)op, (uint8_t)(head << 2 | unit), (uint8_t)cyl, (uint8_t)head, (uint8_t)sec, 3, (uint8_t)eot, 0x35, 0xFF};
    command(c, 9);
    run_us(0);
    CHECK(p2get() == 'W');
    CHECK(r(0x01) == 0x10);
    fukemu_port2_rx('M');
    for (int i = 0; i < n; i++) fukemu_port2_rx(data[i]);
    CHECK(p2get() == 'w');
}

static void test_write(void)
{
    setup(NULL, true);
    motor_on(0);
    static uint8_t d[2048];
    for (int i = 0; i < 2048; i++) d[i] = (uint8_t)(i ^ 0x5A);
    write_cmd(0x45, 0, 3, 0, 2, 3, d, 2048);
    CHECK(r(0x01) == 0xD0);
    uint8_t res[7];
    for (int i = 0; i < 7; i++) res[i] = r(0x03);
    CHECK(res[0] == 0 && res[1] == 0 && res[3] == 3 && res[5] == 3);
    CHECK(r(0x01) == 0x80);
    CHECK(memcmp(&images[0][(3 * 2 * 8 + 1) * 1024], d, 2048) == 0);
    // 読み直す
    read_cmd(0, 3, 0, 2, 3);
    run_us(0);
    CHECK(take_read(buf, res) == 2048 && memcmp(buf, d, 2048) == 0);
    // ユニット 1 は書き込み禁止
    motor_on(1);
    uint8_t c[2] = {0x04, 0x01};
    command(c, 2);
    CHECK(r(0x03) == 0x79); // 書き込み禁止、レディ、トラック 0、両面
    int before = writes;
    write_cmd(0x45, 1, 0, 0, 1, 1, d, 1024);
    for (int i = 0; i < 7; i++) res[i] = r(0x03);
    CHECK(res[0] == 0x41 && res[1] == 0x02);
    CHECK(writes == before);
    // 書けない (ノットレディ): 'W' なしで 'w'
    w(0x07, 0x00);
    uint8_t wc[9] = {0x45, 0, 0, 0, 1, 3, 1, 0x35, 0xFF};
    command(wc, 9);
    CHECK(p2get() == 'w');
    CHECK(r(0x03) == 0x48);
    for (int i = 0; i < 6; i++) r(0x03);
    // WRITE ID (C5 H1 を 8 セクタ、0xE5 で埋める)
    motor_on(0);
    uint8_t ids[32];
    for (int i = 0; i < 8; i++) ids[i * 4] = 5, ids[i * 4 + 1] = 1, ids[i * 4 + 2] = (uint8_t)(i + 1), ids[i * 4 + 3] = 3;
    w(0x30, 32);
    w(0x31, 0);
    uint8_t f[6] = {0x4D, 0x04, 3, 8, 0x74, 0xE5};
    command(f, 6);
    CHECK(p2get() == 'W');
    fukemu_port2_rx('M');
    for (int i = 0; i < 32; i++) fukemu_port2_rx(ids[i]);
    CHECK(p2get() == 'w');
    for (int i = 0; i < 7; i++) res[i] = r(0x03);
    CHECK(res[0] == 0x04 && res[3] == 5 && res[5] == 8);
    const uint8_t *t = &images[0][(5 * 2 + 1) * 8 * 1024];
    int all = 1;
    for (int i = 0; i < 8192; i++) all &= t[i] == 0xE5;
    CHECK(all);
}

static void test_read_id(void)
{
    setup(NULL, false);
    motor_on(0);
    seek(0, 7);
    uint8_t c[2] = {0x4A, 0x04};
    command(c, 2);
    CHECK(p2get() == 'C');
    CHECK(r(0x01) == 0xD0);
    CHECK(r(0x03) == 0x04);
    CHECK(p2get() == 'c');
    for (int i = 0; i < 6; i++) r(0x03);
    CHECK(r(0x01) == 0x80);
}

static void test_geometry(void)
{
    uint32_t z[FUKEMU_SECS] = {0};
    CHECK(fukemu_id_offset(1) == 146);
    CHECK(fukemu_data_offset(1) == 206);
    CHECK(fukemu_id_offset(8) == 146 + 7 * 1202);
    CHECK((fukemu_data_offset(8) + 1024 + 2 + 116) * FUKEMU_BYTE_US * 3 + FUKEMU_MAX_EXTRA_US * 3 < FUKEMU_REV_3US);
    uint64_t s, e;
    fukemu_sector_window(0, 7, z, &s, &e);
    CHECK(s == fukemu_data_offset(7) * 16u && e == s + 16384);
    // ID の先頭を 1us 過ぎてから探すと、次の回転
    fukemu_sector_window(fukemu_id_offset(7) * 16u + 1, 7, z, &s, &e);
    CHECK(s == 166667 + fukemu_data_offset(7) * 16u);
    // 3 回転目の先頭は 500000us (端数がたまらない)
    fukemu_sector_window(400000, 1, z, &s, &e);
    CHECK(s == 500000 + fukemu_data_offset(1) * 16u);
    // セクタ 7 を長くすると、セクタ 8 は後ろにずれる
    uint32_t x[FUKEMU_SECS] = {0, 0, 0, 0, 0, 0, 400, 0};
    fukemu_sector_window(0, 8, x, &s, &e);
    CHECK(s == fukemu_data_offset(8) * 16u + 400);
}

// ---- 実時間の Read のデータの時刻 (#320) ----
// DMA (1024 バイト) の最初のバイトから COC までを、COC の立ち方の 2 通りで測る:
//   *t_tc: データの最初のバイトが届いてから、1024 バイト目が届くまで (1024 バイトで TC になり、すぐ COC)
//   戻り値: データの最初のバイトが届いてから、リザルトの最後のバイトが届くまで (リザルトを受け取ってから COC)
static uint64_t timed_read_eot(int unit, int cyl, int sec, int eot, int *realtime, uint64_t *t_tc)
{
    uint64_t t0 = now;
    read_cmd(unit, cyl, 0, sec, eot);
    int start = p2n;
    run_us(400000);
    int len = p2n - start;
    CHECK(len >= 3 + 1024 + 7 && p2[start] == 'R' && len == 3 + (p2[start + 1] | p2[start + 2] << 8));
    if (len < 3 + 1024 + 7) return 0;
    CHECK(memcmp(&p2[start + 3], &images[unit][(cyl * 2 * 8 + sec - 1) * 1024], 1024) == 0);
    p2r = p2n;
    *realtime = p2t[p2n - 1] != t0;
    if (t_tc) *t_tc = p2t[start + 3 + 1023] - p2t[start + 3];
    run_us(700 + (uint64_t)(rand() % 3000)); // 処理の時間 (ばらつく)
    return p2t[p2n - 1] - p2t[start + 3];
}
static uint64_t timed_read(int unit, int cyl, int sec, int *realtime)
{
    return timed_read_eot(unit, cyl, sec, sec, realtime, NULL);
}

// 2 つの Read の長さの差
static long score1(uint64_t t7, uint64_t t8)
{
    int64_t avg = (int64_t)((t7 + t8) / 2);
    return avg ? (long)(((int64_t)t8 - (int64_t)t7) * 10000 / avg) : 0;
}
static void timing_score(int unit, int cyl, int *rt7, int *rt8, long *score_tc, long *score_res)
{
    long sum_tc = 0, sum_res = 0;
    for (int i = 0; i < 3; i++) {
        run_us((uint64_t)(rand() % 200000));
        uint64_t c7 = 0, c8 = 0;
        seek(unit, cyl); // IOCS の _B_SEEK (SEEK と SENSE INTERRUPT STATUS)
        uint64_t t7 = timed_read_eot(unit, cyl, 7, 8, rt7, &c7); // R=7 EOT=8
        seek(unit, cyl);
        uint64_t t8 = timed_read_eot(unit, cyl, 8, 8, rt8, &c8); // 同じものの R だけを 8 に書き換えたもの
        sum_tc += score1(c7, c8);
        sum_res += score1(t7, t8);
    }
    *score_tc = sum_tc / 3;
    *score_res = sum_res / 3;
}

#define NO_CHECK 999999
// lo..hi が NO_CHECK なら確かめない (表に出すだけ)
static void test_timing(int delivery, int one_sector, int32_t ppm, long tc_lo, long tc_hi, long res_lo, long res_hi)
{
    fukemu_config_t cfg = {0};
    cfg.delivery = (uint8_t)delivery;
    cfg.rule = FUKEMU_RT_PAIR;
    cfg.one_sector = (uint8_t)one_sector;
    cfg.nstretch = 1;
    cfg.stretch[0] = (fukemu_stretch_t){1, 1, 0, 8, ppm};
    setup(&cfg, false);
    motor_on(1);
    seek(1, 1);
    int rt7 = 0, rt8 = 0;
    long sc_tc, sc_res;
    timing_score(1, 1, &rt7, &rt8, &sc_tc, &sc_res);
    printf("  %-6s %-10s R8 +%5d ppm: TC で COC なら %6ld、リザルトで COC なら %6ld\n",
           delivery == FUKEMU_STREAM ? "stream" : delivery == FUKEMU_HEAD ? "head" : "end",
           one_sector ? "one-sector" : "", (int)ppm, sc_tc, sc_res);
    CHECK(rt7 && rt8);
    if (tc_lo != NO_CHECK) CHECK(sc_tc >= tc_lo && sc_tc <= tc_hi);
    if (res_lo != NO_CHECK) CHECK(sc_res >= res_lo && sc_res <= res_hi);
}

// 既定の設定 (stream、pair、EOT まで、--stretch も既定) で、TC で COC のとき
static void test_timing_default(void)
{
    fukemu_config_t cfg;
    fukemu_default_config(&cfg);
    setup(&cfg, false);
    motor_on(1);
    int rt7 = 0, rt8 = 0;
    long sc_tc, sc_res;
    timing_score(1, 1, &rt7, &rt8, &sc_tc, &sc_res);
    printf("  既定の設定: TC で COC なら %ld、リザルトで COC なら %ld\n", sc_tc, sc_res);
    CHECK(rt7 && rt8);
    CHECK(sc_tc >= 200 && sc_tc <= 260);
}

// 送るバッファが詰まっている間に割り込みが何度上下しても、通知は溜まり過ぎず、空いたら今の状態で終わる
static void test_ctl_coalesce(void)
{
    setup(NULL, false);
    motor_on(0);
    p2block = 1;
    for (int i = 0; i < 40; i++) {
        uint8_t c[2] = {0x4A, 0x00}; // READ ID: 'C'、1 バイト目を読むと 'c'
        command(c, 2);
        for (int k = 0; k < 7; k++) r(0x03);
    }
    uint8_t c[2] = {0x4A, 0x00};
    command(c, 2); // 最後は立ったまま
    p2block = 0;
    run_us(1);
    CHECK(p2avail() == 1 && p2get() == 'C');
    CHECK(r(0x13) == 0x80);
    for (int k = 0; k < 7; k++) r(0x03);
    CHECK(p2get() == 'c');
}

static void test_rule(void)
{
    fukemu_config_t cfg = {0};
    cfg.rule = FUKEMU_RT_PAIR;
    setup(&cfg, false);
    motor_on(1);
    int rt;
    // セクタ 8 の単独読みは、直前がセクタ 7 の単独読みでなければすぐに答える
    timed_read(1, 1, 8, &rt);
    CHECK(!rt);
    timed_read(1, 1, 7, &rt);
    CHECK(rt);
    timed_read(1, 1, 8, &rt);
    CHECK(rt);
    timed_read(1, 1, 8, &rt);
    CHECK(!rt);
    // 7 のあとに別のトラックを挟むと、8 はすぐ
    timed_read(1, 1, 7, &rt);
    timed_read(1, 2, 7, &rt);
    CHECK(rt);
    timed_read(1, 1, 8, &rt);
    CHECK(!rt);
    // 7 と 8 の間に同じシリンダへの SEEK が挟まっても続けて読んだとみなす
    timed_read(1, 1, 7, &rt);
    seek(1, 1);
    timed_read(1, 1, 8, &rt);
    CHECK(rt);
    // 別のシリンダへの SEEK が挟まると、8 はすぐ
    timed_read(1, 1, 7, &rt);
    uint8_t sk[3] = {0x0F, 1, 2};
    command(sk, 3);
    run_us(10);
    p2r = p2n;
    timed_read(1, 1, 8, &rt);
    CHECK(!rt);
    // SENSE INTERRUPT STATUS が挟まっても続けて読んだとみなす
    timed_read(1, 1, 7, &rt);
    uint8_t si[1] = {0x08};
    command(si, 1);
    r(0x03);
    if (r(0x01) == 0xD0) r(0x03);
    timed_read(1, 1, 8, &rt);
    CHECK(rt);
    // R=7 EOT=8 も実時間で、続く R=8 EOT=8 も
    timed_read_eot(1, 1, 7, 8, &rt, NULL);
    CHECK(rt);
    timed_read(1, 1, 8, &rt);
    CHECK(rt);
    // セクタ 7 から始まらない Read はすぐ
    uint64_t t0 = now;
    uint8_t res[7];
    read_cmd(1, 1, 0, 1, 8);
    run_us(0);
    CHECK(take_read(buf, res) == 8192 && p2t[p2n - 1] == t0);
    // one_sector: 実時間の R=7 EOT=8 はセクタ 7 だけを送り、リザルトの R は 7
    cfg.one_sector = 1;
    setup(&cfg, false);
    motor_on(1);
    read_cmd(1, 1, 0, 7, 8);
    run_us(400000);
    CHECK(take_read(buf, res) == 1024 && res[5] == 7);
    cfg.one_sector = 0;
    // 数と最後の Read
    fukemu_stats_t st;
    fukemu_get_stats(&st);
    CHECK(st.reads == 1 && st.rt_reads == 1 && st.last_unit == 1 && st.last_r == 7 && st.last_eot == 7 && st.last_rt);
    CHECK(st.last_done_us >= st.last_due_us && st.last_due_us > st.last_cmd_us && !st.last_aborted);
    // 送り終える前にリセットした Read は aborted
    read_cmd(1, 1, 0, 7, 7);
    fukemu_port1_rx(0x40);
    fukemu_get_stats(&st);
    CHECK(st.reads == 2 && st.last_done_us == 0 && st.last_aborted);
    // FUKEMU_RT_NONE なら 7 も 8 もすぐ
    cfg.rule = FUKEMU_RT_NONE;
    setup(&cfg, false);
    motor_on(1);
    timed_read(1, 1, 7, &rt);
    CHECK(!rt);
    // 実時間の Read の間はビジー
    cfg.rule = FUKEMU_RT_SINGLE;
    setup(&cfg, false);
    motor_on(0);
    read_cmd(0, 0, 0, 3, 3);
    CHECK(r(0x01) == 0x10);
    run_us(400000);
    CHECK(r(0x01) == 0x80);
    // STREAM: バイトはデータの流れる速さで届く (512 バイト目はデータの先頭から 8192us)
    p2r = p2n;
    cfg.rule = FUKEMU_RT_ALL;
    cfg.delivery = FUKEMU_STREAM;
    setup(&cfg, false);
    motor_on(0);
    read_cmd(0, 0, 0, 1, 1);
    run_us(400000);
    CHECK(p2n == 3 + 1024 + 7);
    CHECK(p2t[3 + 511] - p2t[0] == 512 * 16);
    CHECK(p2t[3 + 1023] - p2t[0] == 1024 * 16);
    CHECK(p2t[3 + 1024] - p2t[3 + 1023] == 32); // リザルトは CRC の 2 バイトのあと
}

// 長くした分の合計は、トラックが 1 回転に収まるところまで
static void test_stretch_cap(void)
{
    fukemu_config_t cfg = {0};
    cfg.delivery = FUKEMU_STREAM;
    cfg.rule = FUKEMU_RT_SINGLE;
    cfg.nstretch = 2;
    cfg.stretch[0] = (fukemu_stretch_t){0, 0, 0, 7, 400000}; // 40% = 6553us
    cfg.stretch[1] = (fukemu_stretch_t){0, 0, 0, 8, 400000}; // 残りの 2921us だけ足す
    setup(&cfg, false);
    motor_on(0);
    int rt;
    uint64_t t7 = timed_read(0, 0, 7, &rt), t8 = timed_read(0, 0, 8, &rt);
    // 測る時間 = データの長さ + CRC (32us) - 最初のバイトの分 (長さ / 1024 の切り上げ)
    uint64_t d7 = 16384 + 6553, d8 = 16384 + (FUKEMU_MAX_EXTRA_US - 6553);
    CHECK(t7 == d7 + 32 - (d7 + 1023) / 1024);
    CHECK(t8 == d8 + 32 - (d8 + 1023) / 1024);
    // 同じセクタを 2 度書いたら最後のもの。R7 を 0% に戻すと、R8 は 40% 全部
    cfg.nstretch = 3;
    cfg.stretch[2] = (fukemu_stretch_t){0, 0, 0, 7, 0};
    setup(&cfg, false);
    motor_on(0);
    t7 = timed_read(0, 0, 7, &rt), t8 = timed_read(0, 0, 8, &rt);
    d8 = 16384 + 6553;
    CHECK(t7 == 16384 + 32 - 16);
    CHECK(t8 == d8 + 32 - (d8 + 1023) / 1024);
}

// 送るバッファが一杯でも止まらず、空いたら続きを送る。その間もポート 1 には答える
// リセットで、まだ送れていない通知を捨てる。割り込みの線はホストが最後に受け取った状態から下げ直す
static void test_reset_drops_stale(void)
{
    uint8_t c[2] = {0x4A, 0x00}; // READ ID: 'C'
    setup(NULL, false);
    motor_on(0);
    p2block = 1;
    command(c, 2); // 'C' は送れずに溜まる
    fukemu_port1_rx(0x40);
    p2block = 0;
    run_us(1);
    CHECK(p2avail() == 0); // ホストは 'C' を受け取っていないので、'c' も要らない
    command(c, 2);
    run_us(1);
    CHECK(p2avail() == 1 && p2get() == 'C'); // 今度は届いた
    p2block = 1;
    fukemu_port1_rx(0x40);
    p2block = 0;
    run_us(1);
    CHECK(p2avail() == 1 && p2get() == 'c');
    // コマンドの途中はディスクを抜けない。抜くとディスクなし (ノットレディ)
    p2block = 1;
    read_cmd(0, 0, 0, 1, 1);
    run_us(10);
    CHECK(!fukemu_eject(0)); // データを送っている途中
    fukemu_port1_rx(0x40);
    p2block = 0;
    CHECK(fukemu_eject(0));
    motor_on(0);
    command(c, 2);
    for (int k = 0; k < 7; k++) r(0x03);
}

static void test_tx_full(void)
{
    setup(NULL, false);
    motor_on(0);
    p2block = 1;
    read_cmd(0, 0, 0, 1, 8);
    run_us(100);
    CHECK(p2n == 0);
    CHECK(r(0x01) == 0x10);
    fukemu_port1_rx(0x40); // リセットも受け付ける
    CHECK(r(0x01) == 0x80);
    p2block = 0;
    motor_on(0);
    p2limit = 100;
    uint8_t res[7];
    read_cmd(0, 0, 0, 1, 8);
    run_us(100);
    CHECK(take_read(buf, res) == 8192 && memcmp(buf, images[0], 8192) == 0);
    // 通知のバイトも、詰まっている間は溜めて、空いたら送る
    p2block = 1;
    fukemu_port2_rx(0x00);
    CHECK(p2avail() == 0);
    p2block = 0;
    run_us(1);
    CHECK(p2get() == 'F');
    p2limit = 0;
}

// フラッシュへの書き込みに失敗したら異常終了
static void test_write_fail_and_bad_format(void)
{
    setup(NULL, false);
    motor_on(0);
    static uint8_t d[1024];
    write_fail = 1;
    write_cmd(0x45, 0, 0, 0, 1, 1, d, 1024);
    CHECK(r(0x03) == 0x40 && r(0x03) == 0x10);
    for (int i = 0; i < 5; i++) r(0x03);
    write_fail = 0;
    // R が 9 の ID を含むフォーマットは、書かずに異常終了
    int before = writes;
    uint8_t ids[8] = {0, 0, 1, 3, 0, 0, 9, 3};
    w(0x30, 8);
    w(0x31, 0);
    uint8_t f[6] = {0x4D, 0x00, 3, 2, 0x74, 0xE5};
    command(f, 6);
    CHECK(p2get() == 'W');
    fukemu_port2_rx('M');
    for (int i = 0; i < 8; i++) fukemu_port2_rx(ids[i]);
    CHECK(p2get() == 'w');
    CHECK(r(0x03) == 0x40);
    for (int i = 0; i < 6; i++) r(0x03);
    CHECK(writes == before);
    // 1 トラックのフォーマットは、書き込み 1 回にまとめる
    uint8_t t[32];
    for (int i = 0; i < 8; i++) t[i * 4] = 4, t[i * 4 + 1] = 0, t[i * 4 + 2] = (uint8_t)(i + 1), t[i * 4 + 3] = 3;
    w(0x30, 32);
    uint8_t f8[6] = {0x4D, 0x00, 3, 8, 0x74, 0xE5};
    command(f8, 6);
    CHECK(p2get() == 'W');
    fukemu_port2_rx('M');
    for (int i = 0; i < 32; i++) fukemu_port2_rx(t[i]);
    CHECK(p2get() == 'w');
    CHECK(writes == before + 1);
    CHECK(r(0x03) == 0x00);
    for (int i = 0; i < 6; i++) r(0x03);
}

// 複数セクタを実時間で読むと、同じ回転で続けて流れる
static void test_multi_realtime(void)
{
    fukemu_config_t cfg = {0};
    cfg.rule = FUKEMU_RT_ALL;
    setup(&cfg, false);
    motor_on(0);
    read_cmd(0, 0, 0, 2, 4);
    run_us(400000);
    CHECK(p2n == 3 + 3072 + 7);
    CHECK(memcmp(&p2[3], &images[0][1024], 3072) == 0);
    // セクタ 2 の最後のバイトからセクタ 3 の最初のバイトまでは、CRC、GAP3、次の ID とデータの前 (180 バイト) と 1 バイト
    CHECK(p2t[3 + 1024] - p2t[3 + 1023] == (2 + 116 + 60 + 1) * 16);
    CHECK(p2t[p2n - 1] - p2t[3 + 3071] == 32);
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && argv[1][0] == '-';
    srand(320);
    test_probe_and_reset();
    test_seek_and_sense();
    test_read();
    test_write();
    test_read_id();
    test_geometry();
    test_rule();
    test_stretch_cap();
    test_tx_full();
    test_ctl_coalesce();
    test_reset_drops_stale();
    test_write_fail_and_bad_format();
    test_multi_realtime();
    // R=7 EOT=8 と R=8 EOT=8 の 2 つの Read で、送り方ごとの長さの差
    printf("  2 つの Read の長さの差 (3 回の平均):\n");
    test_timing_default();
    test_timing(FUKEMU_STREAM, 0, 23000, 200, 260, NO_CHECK, 0);
    // COC がリザルトで立つなら、R=7 EOT=8 はセクタ 8 の終わりまで待つ
    test_timing(FUKEMU_STREAM, 0, 23000, NO_CHECK, 0, -8000, -6000);
    test_timing(FUKEMU_STREAM, 1, 23000, 200, 260, 200, 260);
    test_timing(FUKEMU_HEAD, 1, 23000, NO_CHECK, 0, 200, 260);
    test_timing(FUKEMU_HEAD, 0, 23000, NO_CHECK, 0, NO_CHECK, 0);
    // 長くしなければ 0 のあたり
    test_timing(FUKEMU_STREAM, 1, 0, -20, 20, -20, 20);
    // 全部をまとめて送ると、測る時間がほぼ 0 になり差が出ない
    test_timing(FUKEMU_END, 1, 23000, -1, 1, -1, 1);
    if (fails) {
        printf("%d 件失敗\n", fails);
        return 1;
    }
    printf("test_host: ok\n");
    return 0;
}
