// SRAM のファイルの形式と、2 つのファイルに交互に書く手順 (src/x68k/sram_file.h) のテスト
#include <stdio.h>
#include <string.h>

#include "x68k/sram_file.h"

namespace {

int failures = 0;

#define CHECK_EQ(a, b)                                                                                 \
    do {                                                                                               \
        unsigned long long va_ = (unsigned long long)(a), vb_ = (unsigned long long)(b);              \
        if (va_ != vb_) {                                                                              \
            printf("  %s:%d: %s == %s: got %llx, want %llx\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
            failures++;                                                                                \
        }                                                                                              \
    } while (0)

using x68k::SramSlots;

uint8_t sram[SramSlots::SRAM_SIZE];
uint8_t file[2][SramSlots::FILE_SIZE];

void fill(uint8_t seed) {
    for (size_t i = 0; i < sizeof sram; i++) sram[i] = (uint8_t)(i * 7 + seed);
}

void testCrc() {
    printf("crc32\n");
    CHECK_EQ(SramSlots::crc32((const uint8_t*)"123456789", 9), 0xcbf43926u); // CRC-32 の確認用の値
}

void testParse() {
    printf("parse\n");
    fill(1);
    uint32_t gen = 99;
    // 16KB だけのファイル (以前の形式、ほかのエミュレータ) は世代 0
    CHECK_EQ(SramSlots::parse(sram, sizeof sram, gen), true);
    CHECK_EQ(gen, 0);
    SramSlots::build(sram, 5, file[0]);
    CHECK_EQ(memcmp(file[0], sram, sizeof sram), 0); // 先頭は SRAM そのもの
    CHECK_EQ(SramSlots::parse(file[0], SramSlots::FILE_SIZE, gen), true);
    CHECK_EQ(gen, 5);
    // 途中までのファイル、中身の壊れたファイル、後書きの壊れたファイルは使わない
    CHECK_EQ(SramSlots::parse(file[0], SramSlots::FILE_SIZE - 1, gen), false);
    CHECK_EQ(SramSlots::parse(file[0], 100, gen), false);
    CHECK_EQ(SramSlots::parse(nullptr, 0, gen), false);
    file[0][123] ^= 1;
    CHECK_EQ(SramSlots::parse(file[0], SramSlots::FILE_SIZE, gen), false);
    file[0][123] ^= 1;
    file[0][SramSlots::SRAM_SIZE] = 'X';
    CHECK_EQ(SramSlots::parse(file[0], SramSlots::FILE_SIZE, gen), false);
    file[0][SramSlots::SRAM_SIZE] = 'P';
    CHECK_EQ(SramSlots::parse(file[0], SramSlots::FILE_SIZE, gen), true);
    // 世代番号も CRC の対象 (古いファイルの世代番号が化けて、新しいほうより優先されないように)
    file[0][SramSlots::SRAM_SIZE + 8 + 3] ^= 0x80;
    CHECK_EQ(SramSlots::parse(file[0], SramSlots::FILE_SIZE, gen), false);
}

void testSlots() {
    printf("slots\n");
    // どちらもなければスロット 0 に世代 1 を書く
    SramSlots s;
    s.load(0, nullptr, 0);
    s.load(1, nullptr, 0);
    CHECK_EQ(s.newest(), -1);
    CHECK_EQ(s.nextSlot(), 0);
    CHECK_EQ(s.nextGeneration(), 1);
    s.writing(0);
    s.saved(0, 1);
    // 次は使っていないほう (スロット 1) に書き、そのあとは交互になる
    CHECK_EQ(s.newest(), 0);
    CHECK_EQ(s.nextSlot(), 1);
    CHECK_EQ(s.nextGeneration(), 2);
    s.writing(1);
    s.saved(1, 2);
    CHECK_EQ(s.newest(), 1);
    CHECK_EQ(s.nextSlot(), 0);

    // 書いている途中で止まった (書いているスロットは壊れている) とき: もう一方を使い、同じスロットに書き直す
    s.writing(0);
    CHECK_EQ(s.newest(), 1);
    CHECK_EQ(s.nextSlot(), 0);
    CHECK_EQ(s.nextGeneration(), 3);

    // 起動時: 世代の大きい正しいほうを使う
    fill(10);
    SramSlots::build(sram, 7, file[0]);
    fill(20);
    SramSlots::build(sram, 8, file[1]);
    SramSlots t;
    t.load(0, file[0], SramSlots::FILE_SIZE);
    t.load(1, file[1], SramSlots::FILE_SIZE);
    CHECK_EQ(t.newest(), 1);
    CHECK_EQ(t.nextSlot(), 0);
    CHECK_EQ(t.nextGeneration(), 9);
    // 新しいほうが壊れていたら (電源が切れた)、古いほうを使う
    file[1][0] ^= 0xff;
    t.load(1, file[1], SramSlots::FILE_SIZE);
    CHECK_EQ(t.newest(), 0);
    CHECK_EQ(t.nextSlot(), 1);
    CHECK_EQ(t.nextGeneration(), 8);
    // 古いスロットの世代番号が 1 ビット化けても、そのスロットは使わない
    fill(10);
    SramSlots::build(sram, 7, file[0]);
    fill(20);
    SramSlots::build(sram, 8, file[1]);
    file[0][SramSlots::SRAM_SIZE + 8 + 2] ^= 0x01; // 世代 7 → 0x10007
    t.load(0, file[0], SramSlots::FILE_SIZE);
    t.load(1, file[1], SramSlots::FILE_SIZE);
    CHECK_EQ(t.newest(), 1);
    // 世代番号の周回: 0xFFFFFFFF の次の 0 (と 1) を新しいとみなす
    SramSlots w;
    SramSlots::build(sram, 0xffffffffu, file[0]);
    SramSlots::build(sram, 1, file[1]);
    w.load(0, file[0], SramSlots::FILE_SIZE);
    w.load(1, file[1], SramSlots::FILE_SIZE);
    CHECK_EQ(w.newest(), 1);
    CHECK_EQ(w.nextGeneration(), 2);
    w.load(1, nullptr, 0);
    CHECK_EQ(w.nextGeneration(), 0);
    w.saved(1, 0);
    CHECK_EQ(w.newest(), 1);
    // 以前の 16KB の SRAM.DAT だけがあるとき: それを使い、次は SRAM.BAK (スロット 1) に世代 1 を書く
    SramSlots u;
    u.load(0, sram, sizeof sram);
    u.load(1, nullptr, 0);
    CHECK_EQ(u.newest(), 0);
    CHECK_EQ(u.nextSlot(), 1);
    CHECK_EQ(u.nextGeneration(), 1);
}

} // namespace

int main() {
    testCrc();
    testParse();
    testSlots();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
