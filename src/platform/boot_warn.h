// 起動にかかわる書き換えの警告 (boot_warn=。#371)。知らせるだけで、X68000 の動作も書き込みも変えない。
//
// 1. 起動のとき、ファイルから読んだ SRAM の起動装置 ($ED0018 のワード) が SRAM ($B000) なら知らせる。初期化されていない
//    SRAM (先頭の識別子がない) では出さない。ステートから読んだ SRAM は見ない
// 2. フロッピーディスクのイメージのブートセクタ (先頭のトラックの R = 1。1024 バイトまで) の中身が X68000 の書き込みで変わったら知らせる
//    (x68k::FloppyImage::takeBootChanged())。同じイメージのファイルについては 1 回の起動で 1 回だけ (BootWarnSeen)
//
// どちらも正規の使い方でも起きる (SRAM に置いたプログラムからの起動、FORMAT、SYS など)。
// ベアメタルでも使えるよう、例外、RTTI、STL、stdio は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace platform {

// 画面の左上に出す文 (1 行 5 字、4 行まで。platform/osd_message.h) と、出しておく秒数
static const char kBootWarnSram[] = "起動装置がSRAMになっています";
static const char kBootWarnFd[2][64] = {"FD0のブートセクタが書き換えられました", "FD1のブートセクタが書き換えられました"};
static constexpr unsigned BOOT_WARN_SEC = 15;

// SRAM (16KB) の起動装置が SRAM か。address には SRAM の起動の番地 ($ED0010 の 4 バイト) を入れる
inline bool sramBootsFromSram(const uint8_t* sram, uint32_t& address) {
    static const uint8_t kMagic[8] = {0x82, 0x77, '6', '8', '0', '0', '0', 'W'};
    if (memcmp(sram, kMagic, sizeof kMagic) != 0) return false;
    address = (uint32_t)sram[0x10] << 24 | (uint32_t)sram[0x11] << 16 | (uint32_t)sram[0x12] << 8 | sram[0x13];
    return sram[0x18] == 0xb0 && sram[0x19] == 0x00;
}

// 知らせたイメージ。パスの文字列のハッシュで覚える。MAX 個を超えたら古いものから忘れる (忘れたものは、また知らせる)
class BootWarnSeen {
public:
    static constexpr int MAX = 32;
    // path を初めて見たなら覚えて true。foldCase なら ASCII の大文字と小文字を区別しない (FAT のパス)
    bool first(const char* path, bool foldCase) {
        uint64_t h = 14695981039346656037ull;
        for (; *path; path++) {
            uint8_t c = (uint8_t)*path;
            if (foldCase && c >= 'A' && c <= 'Z') c = (uint8_t)(c + 0x20);
            h = (h ^ c) * 1099511628211ull;
        }
        for (int i = 0; i < n_; i++)
            if (seen_[i] == h) return false;
        seen_[next_] = h;
        next_ = (next_ + 1) % MAX;
        if (n_ < MAX) n_++;
        return true;
    }

private:
    uint64_t seen_[MAX] = {};
    int n_ = 0, next_ = 0;
};

} // namespace platform
