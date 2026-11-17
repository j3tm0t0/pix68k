// ota.c (O 命令) が使う、チップごとの部分。RP2350 は ota_rp2350.c、RP2040 は ota_rp2040.c
#ifndef PICO_KBD_OTA_BACKEND_H
#define PICO_KBD_OTA_BACKEND_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTA_SECTOR 4096u

// 場所 (offset) は、どれもフラッシュの先頭からのバイト数
const char* otab_target(uint32_t* start, uint32_t* size);     // 書く先を決める。だめなら理由 (ota error のあとに付ける語)、よければ NULL
bool otab_erase_sector(uint32_t offset);                      // 4KB を消す
bool otab_write_sector(uint32_t offset, uint8_t* data);       // 4KB を消して書き、読み返して比べる
// 受け終えたイメージの先頭の 4KB を見て、書いてよいか決める。だめなら理由、よければ NULL (force なら、確かめずに通す)
const char* otab_check_first(const uint8_t* first, bool force);
bool otab_commit(uint32_t start, uint32_t size, uint32_t crc); // 書き終えたイメージを、次の起動で試すように記録する
void otab_reboot(uint32_t start);                             // 新しいイメージを試すために再起動する (戻らない)
void otab_init(void);
void otab_task(bool ready);                                   // 主ループから毎回呼ぶ。ready なら、試している版を確定する
bool otab_wd_ok(void);                                        // 常時のウォッチドッグを掛けてよい状態か
void otab_status(char* buf, size_t size);                     // O の応答に入れる様子

#endif
