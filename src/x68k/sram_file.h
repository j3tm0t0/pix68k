// SRAM (16KB) を保存するファイルの形式と、2 つのファイルに交互に書く手順 (ベアメタル版で使う)。
//
// 書いている途中で電源が切れても前の内容が残るよう、2 つのファイル (スロット 0 / 1) に交互に書く。
// ファイルは SRAM の 16KB のあとに 16 バイトの後書き (trailer) を付けたもの:
//   +0     SRAM の中身 (16KB。X68000 から見える SRAM そのもの)
//   +16384 "PX68SRAM" (8 バイト)
//   +16392 世代番号 (32 ビット、リトルエンディアン。書くたびに 1 増やす)
//   +16396 ここまで (SRAM、"PX68SRAM"、世代番号) の CRC-32 (リトルエンディアン)
// 先頭が SRAM そのものなので、先頭の 16KB だけを読むほかのエミュレータでも使える。
// 後書きのない 16KB だけのファイル (以前の形式、ほかのエミュレータの SRAM.DAT) も、世代 0 として読む。
// 読むときは、正しい (大きさと CRC が合う) ファイルのうち世代番号の新しいほうを使う
// (世代番号は周回してよい。差を符号付きで見る)。
// 書くときは、使っているほうではないファイルに書く (使っているほうは書き終えるまで残る)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace x68k {

class SramSlots {
public:
    static constexpr size_t SRAM_SIZE = 0x4000;
    static constexpr size_t TRAILER_SIZE = 16;
    static constexpr size_t FILE_SIZE = SRAM_SIZE + TRAILER_SIZE;

    // ファイルの中身を確かめる。使えれば true で、世代番号を返す (16KB だけのファイルは 0)
    static bool parse(const uint8_t* data, size_t size, uint32_t& generation);
    // 書き込むファイルの中身 (FILE_SIZE バイト) を作る
    static void build(const uint8_t* sram, uint32_t generation, uint8_t* out);
    static uint32_t crc32(const uint8_t* data, size_t size);

    // 起動時に、スロットのファイルを読んだ結果を渡す (読めなければ data = nullptr)
    void load(int slot, const uint8_t* data, size_t size);
    // 使うスロット (0 / 1)。どちらも使えなければ -1
    int newest() const;
    // 次に書くスロットと世代番号
    int nextSlot() const;
    uint32_t nextGeneration() const;
    // 書き始める前に呼ぶ (ファイルを切り詰めるので、書き終えるまでそのスロットは使えない)
    void writing(int slot) { valid_[slot] = false; }
    // 書き終えた (ファイルを閉じるまで成功した) ら呼ぶ
    void saved(int slot, uint32_t generation);

private:
    bool valid_[2] = {false, false};
    uint32_t gen_[2] = {0, 0};
};

} // namespace x68k
