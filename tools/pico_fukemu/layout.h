// layout.h -- フラッシュの置き場所。ファームと別の場所に置くので、ファームを書き直してもイメージは残る。
// mkimg.py と同じ値にする (変えたら両方を直す)。Pico 2 のフラッシュは 4MB
//
//   0x000000〜0x0FEFFF  ファーム
//   0x0FF000            設定 (4KB): 実時間で答える Read の決まりと、セクタを長くする表
//   0x100000            FD0: 見出し (4KB) + イメージ (1261568 バイト)
//   0x240000            FD1: 同じ
//   0x3FF000〜0x3FFFFF  使わない (picotool uf2 convert --abs-block が足す RP2350-E10 の対策のブロックが、
//                       4MB のフラッシュではここに折り返して書かれるため)
// OTA のビルドでは、0x000000〜0x0F9FFF をパーティション表と A / B に使う (pt.json。test_mkimg.py で重ならないことを確かめる)
#pragma once
#include <stdint.h>

#define FUKEMU_FW_MAX 0x0FF000u
#define FUKEMU_CFG_OFF 0x0FF000u
#define FUKEMU_SLOT_OFF(u) (0x100000u + (uint32_t)(u) * 0x140000u)
#define FUKEMU_SLOT_HDR 0x1000u

#define FUKEMU_CFG_MAGIC "FUKECFG1"
#define FUKEMU_IMG_MAGIC "FUKEIMG1"

// 設定の 4KB の先頭 (リトルエンディアン)
typedef struct {
    char magic[8];    // FUKEMU_CFG_MAGIC
    uint8_t delivery; // enum fukemu_delivery
    uint8_t rule;     // enum fukemu_rule
    uint8_t nstretch;
    uint8_t one_sector; // fukemu_config_t の one_sector
    struct {
        uint8_t unit, c, h, r;
        int32_t ppm;
    } stretch[16];
} fukemu_flash_cfg_t;

// イメージの見出し
typedef struct {
    char magic[8]; // FUKEMU_IMG_MAGIC
    uint32_t size; // 1261568
    uint32_t flags; // bit0: 書き込み禁止
} fukemu_flash_img_t;

// mkimg.py の struct.pack と同じ並び
_Static_assert(sizeof(fukemu_flash_cfg_t) == 12 + 16 * 8, "fukemu_flash_cfg_t");
_Static_assert(sizeof(fukemu_flash_img_t) == 16, "fukemu_flash_img_t");
