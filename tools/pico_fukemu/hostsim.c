// hostsim.c -- core.c を Mac / Linux の上で動かし、2 本の pty を FUK のポート 1 と 2 として見せる。
// Pico を書かずに、pix68k のホスト版を偽物の FUK につなぐのに使う。
//
//   hostsim [-d stream|head|end] [-r none|pair|single|all] [-s U:C:H:R:PERCENT]... [-t 秒] [-1] [-w] [-v] FD0 [FD1]
//   pix68k -r ~/x68k --fuk <表示された 1 本目>,<2 本目>
//
// -1 を付けると、実時間の Read は最初のセクタで終える (既定は EOT まで)。-s を省くと FD1 の C1 H0 R8 を 2.3% 長くする (-s none で長くしない)。-t を付けると、その秒数で終わる。イメージは読み込んだ写しに書く。-w を付けたときだけ、終わるとき (Ctrl+C) にファイルへ書き戻す。
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

#include "core.h"

static int master[2];
static volatile sig_atomic_t stop;
static uint64_t end_us = UINT64_MAX;
static uint64_t now_us(void);
static int verbose;
static uint8_t *images[FUKEMU_UNITS];

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void put(int port, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t k = write(master[port], p, n);
        if (k < 0) {
            if ((errno == EAGAIN || errno == EINTR) && !stop && now_us() < end_us) { // 相手が読まないときも、止められるように
                usleep(100);
                continue;
            }
            return;
        }
        p += k, n -= (size_t)k;
    }
}
static void out1(uint8_t b) { put(0, &b, 1); }
static size_t out2(const uint8_t *p, size_t n) // pty が一杯なら、入った分だけ (残りは core が送り直す)
{
    ssize_t k = write(master[1], p, n);
    return k > 0 ? (size_t)k : 0;
}
static int write_image(int unit, uint32_t off, const uint8_t *p, size_t n)
{
    memcpy(images[unit] + off, p, n);
    return 0;
}
static void logline(const char *s)
{
    if (verbose) fprintf(stderr, "%llu %s\n", (unsigned long long)now_us(), s);
}

static void on_sig(int s)
{
    (void)s;
    stop = 1;
}

static int open_pty(char *name, size_t n)
{
    int m, s;
    struct termios t;
    if (openpty(&m, &s, NULL, NULL, NULL) < 0) return -1;
    if (ttyname_r(s, name, n) != 0) return -1;
    tcgetattr(s, &t);
    cfmakeraw(&t);
    tcsetattr(s, TCSANOW, &t);
    fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK);
    // 相手が開くまで閉じた扱いにならないよう、従側も開いたままにする (s は閉じない)
    return m;
}

static int parse_stretch(const char *s, fukemu_stretch_t *st)
{
    unsigned u, c, h, r;
    double pct;
    if (sscanf(s, "%u:%u:%u:%u:%lf", &u, &c, &h, &r, &pct) != 5) return -1;
    if (u > 1 || c > 76 || h > 1 || r < 1 || r > 8 || pct < 0 || pct >= 50) return -1;
    st->unit = (uint8_t)u, st->c = (uint8_t)c, st->h = (uint8_t)h, st->r = (uint8_t)r;
    st->ppm = (int32_t)(pct * 10000 + 0.5);
    return 0;
}

int main(int argc, char **argv)
{
    fukemu_config_t cfg;
    fukemu_default_config(&cfg);
    int stretch_given = 0;
    int writeback = 0, opt;
    double limit = 0;
    while ((opt = getopt(argc, argv, "d:r:s:t:1wv")) != -1) {
        switch (opt) {
        case 'd':
            cfg.delivery = !strcmp(optarg, "head") ? FUKEMU_HEAD : !strcmp(optarg, "end") ? FUKEMU_END : FUKEMU_STREAM;
            break;
        case 'r':
            cfg.rule = !strcmp(optarg, "none")     ? FUKEMU_RT_NONE
                       : !strcmp(optarg, "single") ? FUKEMU_RT_SINGLE
                       : !strcmp(optarg, "all")    ? FUKEMU_RT_ALL
                                                   : FUKEMU_RT_PAIR;
            break;
        case 's':
            if (!stretch_given) cfg.nstretch = 0, stretch_given = 1; // -s を書いたら既定の表は使わない
            if (!strcmp(optarg, "none")) break;
            if (cfg.nstretch >= FUKEMU_MAX_STRETCH || parse_stretch(optarg, &cfg.stretch[cfg.nstretch]) < 0) {
                fprintf(stderr, "hostsim: -s %s: U:C:H:R:PERCENT の形で書く\n", optarg);
                return 2;
            }
            cfg.nstretch++;
            break;
        case 't': limit = atof(optarg); break;
        case '1': cfg.one_sector = 1; break;
        case 'w': writeback = 1; break;
        case 'v': verbose = 1; break;
        default:
            fprintf(stderr, "使い方: hostsim [-d stream|head|end] [-r none|pair|single|all] [-s U:C:H:R:PERCENT] [-t 秒] [-1] [-w] [-v] FD0 [FD1]\n");
            return 2;
        }
    }
    const uint8_t *im[FUKEMU_UNITS] = {NULL, NULL};
    for (int u = 0; u < FUKEMU_UNITS && optind + u < argc; u++) {
        FILE *f = fopen(argv[optind + u], "rb");
        images[u] = malloc(FUKEMU_IMAGE_BYTES);
        if (!f || fread(images[u], 1, FUKEMU_IMAGE_BYTES, f) != FUKEMU_IMAGE_BYTES || fgetc(f) != EOF) {
            fprintf(stderr, "hostsim: %s は 2HD のイメージ (%d バイト) でない\n", argv[optind + u], FUKEMU_IMAGE_BYTES);
            return 1;
        }
        fclose(f);
        im[u] = images[u];
    }
    char n1[128], n2[128];
    master[0] = open_pty(n1, sizeof n1);
    master[1] = open_pty(n2, sizeof n2);
    if (master[0] < 0 || master[1] < 0) {
        perror("openpty");
        return 1;
    }
    printf("%s,%s\n", n1, n2);
    fflush(stdout);
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    static const fukemu_io_t io = {now_us, out1, out2, write_image, logline, NULL};
    fukemu_init(&io, &cfg, im, NULL);
    if (limit > 0) end_us = now_us() + (uint64_t)(limit * 1e6);
    while (!stop && now_us() < end_us) {
        struct pollfd pf[2] = {{master[0], POLLIN, 0}, {master[1], POLLIN, 0}};
        uint64_t due = fukemu_next_due(), t = now_us();
        // poll の待ちは 1ms 単位で、しかも OS によって長めに寝る (Mac では 1〜2ms 遅れた)。
        // 次の送信が 3ms 以内なら寝ずに回り、それより先なら 2ms 早めに起きる (送る時刻を due どおりにする)
        int ms = due == UINT64_MAX ? 50 : due <= t + 3000 ? 0 : (int)((due - t) / 1000) - 2;
        if (ms > 50) ms = 50;
        poll(pf, 2, ms);
        for (int p = 0; p < 2; p++) {
            uint8_t b[256];
            ssize_t k;
            while ((k = read(master[p], b, sizeof b)) > 0)
                for (ssize_t i = 0; i < k; i++) (p == 0 ? fukemu_port1_rx : fukemu_port2_rx)(b[i]);
        }
        fukemu_poll();
    }
    if (writeback)
        for (int u = 0; u < FUKEMU_UNITS; u++) {
            if (!images[u]) continue;
            FILE *f = fopen(argv[optind + u], "wb");
            if (!f || fwrite(images[u], 1, FUKEMU_IMAGE_BYTES, f) != FUKEMU_IMAGE_BYTES) perror(argv[optind + u]);
            if (f) fclose(f);
        }
    return 0;
}
