// netwrite.c -- 無線 LAN で設定とディスクイメージを書く W 命令 (OTA のビルドだけ)
//
//   W B fd0|fd1 SIZE CRC [wp]  イメージを書き始める。SIZE は 1261568、CRC は CRC-32 (16 進)。wp を付けると書き込み禁止。
//                              最初に FDC のそのドライブをディスクなしにし (再起動するまで)、見出し (4KB) を消す
//   W B config 4096 CRC        設定 (mkimg.py の設定の 4KB と同じ中身) を書き始める。終えるまでフラッシュには触らない
//   W D base64                 続きのデータ (1 行に 336 バイトまで)。"write ok <累計>"
//   W E                        終える。大きさと CRC が合えば、書いた中身を読み返して CRC を比べ、最後に見出し (設定なら設定の 4KB) を書く。
//                              "write done"。反映されるのは再起動のあと (R 命令)
//   W A                        やめる (イメージは見出しを消したまま)
//   W X fd0|fd1                そのドライブの見出しを消す (再起動のあと、ディスクなし)
//   W                          様子
// 書けるのは layout.h の設定の場所と、FD0 / FD1 の場所だけ。ファームの場所には書かない。
// 途中で切れたり電源が落ちたりしても、見出しを最後に書くので、書きかけのイメージは使われない (そのドライブが空になる)。
#include "netwrite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../pico_kbd/core.h"
#include "core.h"
#include "layout.h"

#define SEC 4096u
#define DATA_MAX 336

static const nw_io_t *io;
static enum { T_NONE, T_FD0, T_FD1, T_CFG } target;
static uint32_t want_size, want_crc, crc, received, fill;
static bool wp;
static uint8_t buf[SEC];

static void replyf(const char *fmt, unsigned long a)
{
    char s[80];
    snprintf(s, sizeof s, fmt, a);
    io->reply(s);
}

static void fail(const char *why)
{
    char s[64];
    target = T_NONE;
    snprintf(s, sizeof s, "write error %s", why);
    io->reply(s);
}

static uint32_t data_off(void) { return FUKEMU_SLOT_OFF(target == T_FD1) + FUKEMU_SLOT_HDR; }

static void begin(const char *p)
{
    char name[8] = {0};
    int n = 0;
    if (sscanf(p, "%7s%n", name, &n) != 1) return fail("bad-args");
    p += n;
    char *end;
    unsigned long size = strtoul(p, &end, 10);
    const char *crc_at = end;
    unsigned long c = strtoul(end, &end, 16);
    if (end == crc_at) return fail("bad-args");
    while (*end == ' ') end++;
    bool w = false;
    if (strcmp(end, "wp") == 0) w = true;
    else if (*end) return fail("bad-args");
    target = T_NONE;
    if (strcmp(name, "config") == 0) {
        if (size != SEC || w) return fail("bad-args");
        target = T_CFG;
    } else if (strcmp(name, "fd0") == 0 || strcmp(name, "fd1") == 0) {
        if (size != FUKEMU_IMAGE_BYTES) return fail("bad-size");
        target = name[2] == '0' ? T_FD0 : T_FD1;
        // 書きかけのイメージを使わせない: 動いている FDC から抜き、フラッシュの見出しを先に消す
        if (!io->eject(target == T_FD1)) {
            target = T_NONE;
            return fail("busy");
        }
        if (!io->erase_sector(FUKEMU_SLOT_OFF(target == T_FD1))) return fail("flash");
    } else {
        return fail("bad-target");
    }
    want_size = (uint32_t)size, want_crc = (uint32_t)c, crc = 0, received = 0, fill = 0, wp = w;
    char s[48];
    snprintf(s, sizeof s, "write begin %s", name);
    io->reply(s);
}

static void data(const char *p)
{
    uint8_t bytes[DATA_MAX];
    if (target == T_NONE) return fail("not-started");
    int n = base64_decode(p, bytes, sizeof bytes);
    if (n <= 0 || received + (uint32_t)n > want_size) return fail("bad-data");
    crc = crc32_update(crc, bytes, (unsigned)n);
    for (int i = 0; i < n; i++) {
        buf[fill++] = bytes[i];
        received++;
        if (fill < SEC) continue;
        fill = 0;
        if (target != T_CFG && !io->write_sector(data_off() + received - SEC, buf)) return fail("flash");
    }
    replyf("write ok %lu", received);
}

static void finish(void)
{
    if (target == T_NONE) return fail("not-started");
    if (received != want_size) return fail("short");
    if (crc != want_crc) return fail("crc");
    if (target == T_CFG) {
        const fukemu_flash_cfg_t *c = (const fukemu_flash_cfg_t *)buf;
        if (memcmp(c->magic, FUKEMU_CFG_MAGIC, 8) != 0 || c->delivery > FUKEMU_END || c->rule > FUKEMU_RT_ALL ||
            c->nstretch > FUKEMU_MAX_STRETCH || c->one_sector > 1)
            return fail("bad-config");
        if (!io->write_sector(FUKEMU_CFG_OFF, buf)) return fail("flash");
    } else {
        // 書いた中身を読み返して CRC を比べてから、見出しを書く
        if (crc32_update(0, io->read(data_off()), want_size) != want_crc) return fail("verify");
        memset(buf, 0xff, SEC);
        fukemu_flash_img_t h;
        memcpy(h.magic, FUKEMU_IMG_MAGIC, 8);
        h.size = FUKEMU_IMAGE_BYTES;
        h.flags = wp ? 1 : 0;
        memcpy(buf, &h, sizeof h);
        if (!io->write_sector(FUKEMU_SLOT_OFF(target == T_FD1), buf)) return fail("flash");
    }
    target = T_NONE;
    io->reply("write done (R で再起動すると使われる)");
}

void nw_command(const char *p)
{
    char sub = *p;
    if (sub) p++;
    while (*p == ' ') p++;
    switch (sub) {
    case 0: {
        static const char *const kName[] = {"none", "fd0", "fd1", "config"};
        char s[80];
        snprintf(s, sizeof s, "write target=%s received=%lu size=%lu", kName[target], (unsigned long)received,
                 (unsigned long)want_size);
        io->reply(s);
        return;
    }
    case 'B': return begin(p);
    case 'D': return data(p);
    case 'E': return finish();
    case 'A':
        target = T_NONE;
        io->reply("write aborted");
        return;
    case 'X':
        target = T_NONE;
        if (strcmp(p, "fd0") != 0 && strcmp(p, "fd1") != 0) return fail("bad-target");
        if (!io->eject(p[2] == '1')) return fail("busy");
        if (!io->erase_sector(FUKEMU_SLOT_OFF(p[2] == '1'))) return fail("flash");
        io->reply("write erased (ディスクなし)");
        return;
    default: return fail("bad-args");
    }
}

bool nw_active(void) { return target != T_NONE; }

void nw_init(const nw_io_t *io_)
{
    io = io_;
    target = T_NONE;
}
