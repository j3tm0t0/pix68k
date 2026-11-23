// netwrite.h -- 無線 LAN で設定とディスクイメージを書く W 命令 (OTA のビルドだけ)。フラッシュの読み書きは io に任せる
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool (*write_sector)(uint32_t off, const uint8_t *p); // 4KB を消して書き、読み返して比べる (off はフラッシュの先頭から)
    bool (*erase_sector)(uint32_t off);                   // 4KB を消す
    const uint8_t *(*read)(uint32_t off);                 // フラッシュの中身 (読み返しの CRC に使う)
    void (*reply)(const char *s);                         // "#pico ..." の応答
    bool (*eject)(int unit);                              // FDC のそのドライブをディスクなしにする (再起動まで)。できなければ false
} nw_io_t;

void nw_init(const nw_io_t *io);
// W 命令の W のあと (例 "B fd0 1261568 1a2b3c4d wp")
void nw_command(const char *args);
bool nw_active(void);
