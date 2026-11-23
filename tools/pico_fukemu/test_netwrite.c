// netwrite.c (W 命令) の試験。フラッシュはメモリの上に置く
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../pico_kbd/core.h"
#include "core.h"
#include "layout.h"
#include "netwrite.h"

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

#define FLASH (4u << 20)
static uint8_t flash[FLASH];
static char last[128];
static int writes, erases;
static uint32_t lowest = 0xffffffff;

static bool wr(uint32_t off, const uint8_t *p)
{
    if (off % 4096 || off + 4096 > FLASH) return false;
    if (off < lowest) lowest = off;
    memcpy(flash + off, p, 4096);
    writes++;
    return true;
}
static bool er(uint32_t off)
{
    if (off % 4096) return false;
    if (off < lowest) lowest = off;
    memset(flash + off, 0xff, 4096);
    erases++;
    return true;
}
static const uint8_t *rd(uint32_t off) { return flash + off; }
static void rep(const char *s) { snprintf(last, sizeof last, "%s", s); }
static bool ej_ok = true;
static int ejected = -1;
static bool ej(int unit)
{
    if (ej_ok) ejected = unit;
    return ej_ok;
}
static const nw_io_t io = {wr, er, rd, rep, ej};

static void b64(const uint8_t *p, int n, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int j = 0;
    for (int i = 0; i < n; i += 3) {
        uint32_t v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out[j++] = t[v >> 18 & 63], out[j++] = t[v >> 12 & 63];
        out[j++] = i + 1 < n ? t[v >> 6 & 63] : '=';
        out[j++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[j] = 0;
}

static void send(const uint8_t *p, uint32_t n)
{
    char line[600] = "D ";
    for (uint32_t pos = 0; pos < n; pos += 336) {
        int k = n - pos < 336 ? (int)(n - pos) : 336;
        b64(p + pos, k, line + 2);
        nw_command(line);
    }
}

static void cmd(const char *fmt, unsigned long a, unsigned long b)
{
    char s[128];
    snprintf(s, sizeof s, fmt, a, b);
    nw_command(s);
}

int main(void)
{
    memset(flash, 0xff, sizeof flash);
    nw_init(&io);
    uint8_t *img = malloc(FUKEMU_IMAGE_BYTES);
    for (uint32_t i = 0; i < FUKEMU_IMAGE_BYTES; i++) img[i] = (uint8_t)(i * 7 + (i >> 12));
    uint32_t c = crc32_update(0, img, FUKEMU_IMAGE_BYTES);
    // 前に入っていた FD1 の見出し
    memcpy(flash + FUKEMU_SLOT_OFF(1), "FUKEIMG1", 8);

    // FD1 を書く (wp)
    cmd("B fd1 %lu %lx wp", FUKEMU_IMAGE_BYTES, c);
    CHECK(strcmp(last, "write begin fd1") == 0 && ejected == 1);
    CHECK(flash[FUKEMU_SLOT_OFF(1)] == 0xff); // 見出しは先に消える
    send(img, FUKEMU_IMAGE_BYTES);
    CHECK(strncmp(last, "write ok 1261568", 16) == 0);
    CHECK(flash[FUKEMU_SLOT_OFF(1)] == 0xff); // まだ見出しはない
    nw_command("E");
    CHECK(strncmp(last, "write done", 10) == 0);
    const fukemu_flash_img_t *h = (const fukemu_flash_img_t *)(flash + FUKEMU_SLOT_OFF(1));
    CHECK(memcmp(h->magic, "FUKEIMG1", 8) == 0 && h->size == FUKEMU_IMAGE_BYTES && h->flags == 1);
    CHECK(memcmp(flash + FUKEMU_SLOT_OFF(1) + FUKEMU_SLOT_HDR, img, FUKEMU_IMAGE_BYTES) == 0);
    CHECK(lowest >= FUKEMU_SLOT_OFF(1)); // ほかの場所には触らない
    CHECK(!nw_active());

    // CRC 違い: 見出しは消えたまま
    cmd("B fd0 %lu %lx", FUKEMU_IMAGE_BYTES, c ^ 1);
    send(img, FUKEMU_IMAGE_BYTES);
    nw_command("E");
    CHECK(strcmp(last, "write error crc") == 0);
    CHECK(flash[FUKEMU_SLOT_OFF(0)] == 0xff);

    // 読み返しが違う (書いたあとに壊れた)
    cmd("B fd0 %lu %lx", FUKEMU_IMAGE_BYTES, c);
    send(img, FUKEMU_IMAGE_BYTES);
    flash[FUKEMU_SLOT_OFF(0) + FUKEMU_SLOT_HDR + 5000] ^= 1;
    nw_command("E");
    CHECK(strcmp(last, "write error verify") == 0);
    CHECK(flash[FUKEMU_SLOT_OFF(0)] == 0xff);

    // 足りないまま終える、大きさが違う、知らない先、始めずにデータ
    cmd("B fd0 %lu %lx", FUKEMU_IMAGE_BYTES, c);
    send(img, 4096);
    nw_command("E");
    CHECK(strcmp(last, "write error short") == 0);
    cmd("B fd0 %lu %lx", 1000, c);
    CHECK(strcmp(last, "write error bad-size") == 0);
    cmd("B fw %lu %lx", 4096, c);
    CHECK(strcmp(last, "write error bad-target") == 0);
    nw_command("D AAAA");
    CHECK(strcmp(last, "write error not-started") == 0);
    cmd("B fd0 %lu %lx", FUKEMU_IMAGE_BYTES, c);
    send(img, FUKEMU_IMAGE_BYTES);
    send(img, 3); // 多すぎる
    CHECK(strcmp(last, "write error bad-data") == 0);

    // 設定: 終えるまでフラッシュに触らない。中身を確かめる
    uint8_t cfg[4096];
    memset(cfg, 0xff, sizeof cfg);
    fukemu_flash_cfg_t fc;
    memset(&fc, 0, sizeof fc);
    memcpy(fc.magic, "FUKECFG1", 8);
    fc.delivery = 0, fc.rule = 1, fc.nstretch = 1;
    fc.stretch[0].unit = 1, fc.stretch[0].c = 1, fc.stretch[0].r = 8, fc.stretch[0].ppm = 23000;
    memcpy(cfg, &fc, sizeof fc);
    int w0 = writes, e0 = erases;
    cmd("B config %lu %lx", 4096, crc32_update(0, cfg, 4096));
    send(cfg, 4096);
    CHECK(writes == w0 && erases == e0);
    nw_command("E");
    CHECK(strncmp(last, "write done", 10) == 0);
    CHECK(memcmp(flash + FUKEMU_CFG_OFF, cfg, 4096) == 0);
    cfg[8] = 9; // 送り方が範囲の外
    cmd("B config %lu %lx", 4096, crc32_update(0, cfg, 4096));
    send(cfg, 4096);
    nw_command("E");
    CHECK(strcmp(last, "write error bad-config") == 0);
    CHECK(flash[FUKEMU_CFG_OFF + 8] == 0); // 前の設定のまま
    cmd("B config %lu %lx wp", 4096, 0);
    CHECK(strcmp(last, "write error bad-args") == 0);

    // FDC が使っている途中なら、見出しを消さずに断る
    memcpy(flash + FUKEMU_SLOT_OFF(1), "FUKEIMG1", 8);
    ej_ok = false;
    cmd("B fd1 %lu %lx", FUKEMU_IMAGE_BYTES, c);
    CHECK(strcmp(last, "write error busy") == 0 && flash[FUKEMU_SLOT_OFF(1)] == 'F' && !nw_active());
    nw_command("X fd1");
    CHECK(strcmp(last, "write error busy") == 0 && flash[FUKEMU_SLOT_OFF(1)] == 'F');
    ej_ok = true;
    // 見出しを消す
    nw_command("X fd1");
    CHECK(strncmp(last, "write erased", 12) == 0 && flash[FUKEMU_SLOT_OFF(1)] == 0xff);
    nw_command("X fw");
    CHECK(strcmp(last, "write error bad-target") == 0);

    free(img);
    printf("test_netwrite: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
