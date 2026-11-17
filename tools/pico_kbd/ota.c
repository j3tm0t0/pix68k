// 無線 LAN 越しの更新の O 命令。チップごとの部分は ota_rp2350.c (下の説明) と ota_rp2040.c (otaboot.h の説明) にある。
//
// RP2350: ブート ROM の A/B パーティションと「試してから確定」(TBYB) を使う。
// フラッシュには、先にパーティション表 (pt.json。同じ大きさの A と B) を書いておく。動いているのが A なら B に、B なら A に
// 新しいイメージを書き、ブート ROM に「フラッシュを更新した」と伝えて再起動する。
// 新しいイメージは TBYB の付いたビルド (cmake に PICO_KBD_OTA_TBYB=1) にする。ブート ROM は、起動してから
// 約 16.7 秒以内に確定 (rom_explicit_buy) が呼ばれなければ再起動し、前のイメージに戻す。
// このファームウェアは、TCP でトークンを通った相手が来たときに確定する (無線 LAN なしのビルドは、起動してすぐ)。
// 送る側 (picocmd.py --ota) は、送り終えたらつなぎ直して O 命令を送る。これが確定のきっかけになる。
// つまり、無線 LAN につながらない版や TCP を受けられない版を送ってしまっても、前の版に戻って、もう一度送れる。
// PICO_KBD_OTA_NEVER_BUY=1 を付けたビルドは確定しない (前の版に戻る動きを確かめるための版)。
// 確定したあとは、版の番号 (ビルドした時刻から作る) が大きいほうをブート ROM が選ぶ。
//
// O 命令 (UART でも TCP でも同じ。picocmd.py --ota が送る):
//   O              今の様子: "ota version=V part=N boot-type=T tbyb=XX diag=D buy-rc=R active=0|1 received=N"
//                  part は起動したパーティション (0 = A、1 = B、-1 = パーティション表なし)、tbyb と diag はブート ROM の
//                  起動の記録 (boot_info_t の tbyb_and_update_info と boot_diagnostic)、buy-rc は確定の結果 (呼んでいなければ 1)
//   O B size crc   始める。size はイメージのバイト数 (10 進)、crc は CRC-32 (16 進)。書く先のパーティションを決め、
//                  その先頭の 4KB を消す (古い中身が起動されないように)。"ota begin part-start=... part-size=..."
//   O D base64     続きのデータ (1 行に 336 バイトまで)。"ota ok <受け取った累計>"
//   O E            終える。大きさと CRC が合えば、最後に先頭の 4KB を書き、"ota done" を返して再起動する。
//                  RP2350 では、TBYB の付いていないイメージは "ota error not-tbyb" で断る (確定を待たないので、だめな版でも戻らない)。
//                  それでも入れるときは O E F
//   O A            やめる
// 失敗は "ota error <理由>"。途中で失敗したりやめたりしたときは、書く先の先頭の 4KB を消したままにする
// (動いている側には触らないので、今の版がそのまま動き続ける)。
//
// 先頭の 4KB (ブート ROM がイメージを見分ける IMAGE_DEF がある) を最後に書くので、途中で電源が切れても、書きかけのイメージは起動されない。
// フラッシュの消去と書き込みの間 (4KB ごとに数十ミリ秒) は割り込みが止まるので、試験機のログは欠ける。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "core.h"
#include "ota_backend.h"

#define DATA_MAX 336 // O D の 1 行に入れるバイト数の上限

static __attribute__((aligned(4))) uint8_t s_first[OTA_SECTOR];  // イメージの先頭の 4KB (最後に書く)
static __attribute__((aligned(4))) uint8_t s_sector[OTA_SECTOR]; // 書く前にためる 4KB

static bool s_active;
static uint32_t s_partStart, s_partSize; // 書く先 (フラッシュの先頭からのバイト数)
static uint32_t s_size, s_crcWant, s_crc;
static uint32_t s_received;              // 受け取ったバイト数
static uint32_t s_fill;                  // s_sector にたまっているバイト数

static void replyf(const char* fmt, uint32_t a, uint32_t b) {
    char buf[96];
    snprintf(buf, sizeof buf, fmt, (unsigned long)a, (unsigned long)b);
    app_reply(buf);
}

static void fail(const char* why) {
    char buf[64];
    if (s_active) otab_erase_sector(s_partStart); // 書きかけを起動させない
    s_active = false;
    snprintf(buf, sizeof buf, "ota error %s", why);
    app_reply(buf);
}

static void begin(const char* p) {
    char* end;
    unsigned long size = strtoul(p, &end, 10);
    const char* crcAt = end;
    unsigned long crc = strtoul(end, &end, 16);
    if (size < OTA_SECTOR || end == crcAt || *end != 0) { // CRC がないのも受けない
        fail("bad-args");
        return;
    }
    if (s_active) otab_erase_sector(s_partStart);
    s_active = false;
    const char* why = otab_target(&s_partStart, &s_partSize);
    if (why != NULL) {
        fail(why);
        return;
    }
    if (size > s_partSize) {
        fail("too-big");
        return;
    }
    s_size = (uint32_t)size;
    s_crcWant = (uint32_t)crc;
    s_crc = 0;
    s_received = 0;
    s_fill = 0;
    s_active = true;
    if (!otab_erase_sector(s_partStart)) {
        fail("erase");
        return;
    }
    replyf("ota begin part-start=%lx part-size=%lx", s_partStart, s_partSize);
}

static void data(const char* p) {
    uint8_t bytes[DATA_MAX];
    if (!s_active) {
        fail("not-started");
        return;
    }
    int n = base64_decode(p, bytes, sizeof bytes);
    if (n <= 0 || s_received + (uint32_t)n > s_size) {
        fail("bad-data");
        return;
    }
    s_crc = crc32_update(s_crc, bytes, (unsigned)n);
    for (int i = 0; i < n; i++) {
        s_sector[s_fill++] = bytes[i];
        s_received++;
        if (s_fill < OTA_SECTOR) continue;
        uint32_t offset = s_received - OTA_SECTOR;
        s_fill = 0;
        if (offset == 0) {
            memcpy(s_first, s_sector, OTA_SECTOR); // 先頭は最後に書く
        } else if (!otab_write_sector(s_partStart + offset, s_sector)) {
            fail("flash");
            return;
        }
    }
    replyf("ota ok %lu", s_received, 0);
}

static void finish(const char* p) {
    bool force = p[0] == 'F' && p[1] == 0;
    if (p[0] != 0 && !force) {
        fail("bad-args");
        return;
    }
    if (!s_active) {
        fail("not-started");
        return;
    }
    if (s_received != s_size) {
        fail("short");
        return;
    }
    if (s_crc != s_crcWant) {
        fail("crc");
        return;
    }
    const char* why = otab_check_first(s_first, force);
    if (why != NULL) {
        fail(why);
        return;
    }
    if (s_fill > 0) { // 最後の半端は 0xff で埋める (先頭の 4KB は size >= 4096 なので、もうよけてある)
        memset(s_sector + s_fill, 0xff, OTA_SECTOR - s_fill);
        if (!otab_write_sector(s_partStart + s_received - s_fill, s_sector)) {
            fail("flash");
            return;
        }
    }
    if (!otab_write_sector(s_partStart, s_first)) {
        fail("flash");
        return;
    }
    if (!otab_commit(s_partStart, s_size, s_crc)) {
        fail("commit");
        return;
    }
    s_active = false;
    app_reply("ota done");
    app_flush();
    otab_reboot(s_partStart);
    for (;;) {}
}

void ota_command(const char* p) {
    char sub = p[0];
    if (sub != 0) p++;
    while (*p == ' ') p++;
    switch (sub) {
    case 0: {
        char buf[160];
        int n = snprintf(buf, sizeof buf, "ota version=%lu ", (unsigned long)PICO_KBD_VERSION);
        otab_status(buf + n, sizeof buf - (size_t)n);
        n = (int)strlen(buf);
        snprintf(buf + n, sizeof buf - (size_t)n, " active=%d received=%lu", s_active, (unsigned long)s_received);
        app_reply(buf);
        return;
    }
    case 'B':
        begin(p);
        return;
    case 'D':
        data(p);
        return;
    case 'E':
        finish(p);
        return;
    case 'A':
        if (s_active) otab_erase_sector(s_partStart);
        s_active = false;
        app_reply("ota aborted");
        return;
    default:
        fail("bad-args");
        return;
    }
}

void ota_init(void) { otab_init(); }

bool ota_wd_ok(void) {
#if PICO_KBD_OTA_NEVER_BUY
    return false; // 確定しない版は、ブート ROM / ブートローダのウォッチドッグだけに任せる
#else
    return otab_wd_ok();
#endif
}

void ota_run(const char* args, bool net_ready) {
    // トークンと O 命令が同じ受信データで来たときも、先に確定を済ませる (確定の前の O B は断るため)
    ota_task(net_ready);
    ota_command(args);
}

void ota_task(bool net_ready) {
#if PICO_KBD_OTA_NEVER_BUY
    net_ready = false; // 確定しない (前の版に戻る動きを確かめるための版)
#endif
    otab_task(net_ready);
}
