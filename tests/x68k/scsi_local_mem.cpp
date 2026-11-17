// 060turbo (68060 + ローカルメモリ) で、X68030 の IPL ROM (IPLROM30.DAT) の SCSI IOCS が、ローカルメモリ ($10000000 から)
// のバッファを読み書きできるかを確かめる (x68k/machine.cpp の patchIplFor060。フロッピーディスクは dmaMemAddr)。
// DMAC は本体のバスの 24 ビットのアドレスしか出せないので、IPL ROM 1.3 のまま DMA で転送すると、$10300400 の代わりに
// 本体の $300400 を読み書きする。060turbo.sys の拡張モード (-xm) は Human68k のメモリをローカルメモリに広げるので、
// そのままでは COMMAND.X を読み込んだところで止まる。
// ディスクは起動できない SCSI のイメージ (テスト用に作ったもの) を ID 0 につなぐ。IPL ROM がディスクを待っている間に、
// RAM に小さなプログラムを書いて CPU をそこに飛ばし、IOCS _SCSIDRV ($F5) の _S_READEXT ($26)、_S_READ ($21)、
// _S_WRITEEXT ($27) を、ローカルメモリのバッファで呼ぶ。_S_SELECT / _S_CMDOUT / _S_DATAIN / _S_STSIN / _S_MSGIN を順に呼ぶ読み込み (_S_DATAIN の d3 = 0、256 バイト) も行う。
// 続けて、ドライブ 0 に入れた空のフロッピーディスク (2HD) に
// IOCS _B_WRITE ($45) と _B_READ ($46) を、ローカルメモリのバッファで呼ぶ (DMAC のチャネル 0)。
//   scsi_local_mem [--ipl IPLROM30.DAT] [--local-stack] [--no-dma-block]
// --no-dma-block なら、DMA の転送をまとめず 1 バイトずつ行う (Machine::setDmaBlock(false))。既定ではまとめた転送を使い、
// フロッピーディスクからローカルメモリへの読み込みが、まとめた転送を通ったことも見る。
// --local-stack なら、プログラムのスーパーバイザスタックもローカルメモリに置く (IOCS がスタックに取るものも DMA の届かない所になる)。
// あわせて、IOCS のワークの MMU の有無 ($CBE) が「あり」になっているか、空の SRAM のキャッシュの設定 ($ED0090) が 3 に
// なっているかも見る。
// ROM がなければ 77 (ctest の SKIP_RETURN_CODE)。X68030 の IPL ROM 1.3 でなければ (書き換えの対象でないので) 77。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x68k/machine.h"

namespace {

const uint32_t kCode = 0x0ff000;   // プログラムを置く場所 (kResult までに収まる長さ)
uint32_t kStack = 0x0ff000;        // プログラムのスーパーバイザスタック (--local-stack なら $10FFF000、ローカルメモリ)
const uint32_t kCdb = 0x100200;    // _S_CMDOUT に渡す CDB (+$10 ステータス、+$12 メッセージ)
const uint32_t kResult = 0x100100; // d0: +0 READEXT、+4 READ、+8 WRITEEXT、+12 終わったら 1、+16 _B_WRITE、+20 _B_READ、+24 _S_DATAIN、
                                   // +28 / +32 境界の _S_DATAOUT のあとの DMAC のチャネル 1 の MAR、+36 / +40 はじめと終わりの SSP
const uint32_t kCopy = 0x110000;   // ローカルメモリに読んだものを、確かめるために写す場所 (READEXT 1024、READ 512、+$800 に _B_READ 2048 バイト、+$1000 に _S_DATAIN 256 バイト)
const uint32_t kLocal = 0x10300400; // ローカルメモリのバッファ (+0 READEXT、+$1000 READ、+$2000 WRITEEXT、+$3000 _B_WRITE、+$4000 _B_READ、+$5000 _S_DATAIN)
const uint32_t kWrap = 0x300400;    // 24 ビットで見たときに kLocal と重なる本体のメモリ
const uint32_t kWrapSize = 0x5200;
const uint8_t kMark = 0xa5;

// 68060 のプログラム (IOCS の引数: d1 = 機能、d2 = ブロック番号、d3 = ブロック数、d4 = SCSI ID、
// d5 = ブロックの大きさ (256 << d5)、a1 = バッファ)
const uint16_t kProgram[] = {
    0x23cf, 0x0010, 0x0124, // move.l sp,$100124  はじめの SSP
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0     _SCSIDRV
    0x7226,                 // moveq #$26,d1      _S_READEXT
    0x7408,                 // moveq #8,d2        ブロック 8
    0x7602,                 // moveq #2,d3        2 ブロック
    0x7800,                 // moveq #0,d4        ID 0
    0x7a01,                 // moveq #1,d5        512 バイト
    0x43f9, 0x1030, 0x0400, // lea $10300400,a1
    0x4e4f,                 // trap #15
    0x23c0, 0x0010, 0x0100, // move.l d0,$100100
    0x41f9, 0x1030, 0x0400, // lea $10300400,a0
    0x43f9, 0x0011, 0x0000, // lea $110000,a1
    0x303c, 0x00ff,         // move.w #255,d0
    0x22d8,                 // move.l (a0)+,(a1)+
    0x51c8, 0xfffc,         // dbra d0,*-2
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0
    0x7221,                 // moveq #$21,d1      _S_READ
    0x7414,                 // moveq #20,d2       ブロック 20
    0x7601,                 // moveq #1,d3        1 ブロック
    0x7800,                 // moveq #0,d4
    0x7a01,                 // moveq #1,d5
    0x43f9, 0x1030, 0x1400, // lea $10301400,a1
    0x4e4f,                 // trap #15
    0x23c0, 0x0010, 0x0104, // move.l d0,$100104
    0x41f9, 0x1030, 0x1400, // lea $10301400,a0
    0x43f9, 0x0011, 0x0400, // lea $110400,a1
    0x303c, 0x007f,         // move.w #127,d0
    0x22d8,                 // move.l (a0)+,(a1)+
    0x51c8, 0xfffc,         // dbra d0,*-2
    0x41f9, 0x1030, 0x2400, // lea $10302400,a0
    0x303c, 0x03ff,         // move.w #1023,d0
    0x10c0,                 // move.b d0,(a0)+    書くデータ: i バイト目は (1023 - i) の下位 8 ビット
    0x51c8, 0xfffc,         // dbra d0,*-2
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0
    0x7227,                 // moveq #$27,d1      _S_WRITEEXT
    0x7428,                 // moveq #40,d2       ブロック 40
    0x7602,                 // moveq #2,d3        2 ブロック
    0x7800,                 // moveq #0,d4
    0x7a01,                 // moveq #1,d5
    0x43f9, 0x1030, 0x2400, // lea $10302400,a1
    0x4e4f,                 // trap #15
    0x23c0, 0x0010, 0x0108, // move.l d0,$100108
    0x41f9, 0x1030, 0x3400, // lea $10303400,a0
    0x303c, 0x07ff,         // move.w #2047,d0
    0x10c0,                 // move.b d0,(a0)+    書くデータ: i バイト目は (2047 - i) の下位 8 ビット
    0x51c8, 0xfffc,         // dbra d0,*-2
    0x7045,                 // moveq #$45,d0      _B_WRITE
    0x323c, 0x9070,         // move.w #$9070,d1   フロッピーディスクのドライブ 0
    0x243c, 0x0300, 0x0003, // move.l #$03000003,d2  1024 バイト / セクタ、シリンダ 0、ヘッド 0、セクタ 3
    0x263c, 0x0000, 0x0800, // move.l #2048,d3    2 セクタ
    0x43f9, 0x1030, 0x3400, // lea $10303400,a1
    0x4e4f,                 // trap #15
    0x23c0, 0x0010, 0x0110, // move.l d0,$100110
    0x7046,                 // moveq #$46,d0      _B_READ
    0x323c, 0x9070,         // move.w #$9070,d1
    0x243c, 0x0300, 0x0003, // move.l #$03000003,d2
    0x263c, 0x0000, 0x0800, // move.l #2048,d3
    0x43f9, 0x1030, 0x4400, // lea $10304400,a1
    0x4e4f,                 // trap #15
    0x23c0, 0x0010, 0x0114, // move.l d0,$100114
    0x41f9, 0x1030, 0x4400, // lea $10304400,a0
    0x43f9, 0x0011, 0x0800, // lea $110800,a1
    0x303c, 0x01ff,         // move.w #511,d0
    0x22d8,                 // move.l (a0)+,(a1)+
    0x51c8, 0xfffc,         // dbra d0,*-2
    // ID 1 (256 バイト / ブロック) から READ(6) で 1 ブロックを、_S_DATAIN に d3 = 0 (256 バイトの意味) を渡して読む
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0
    0x7201,                 // moveq #$01,d1      _S_SELECT
    0x7801,                 // moveq #1,d4        ID 1
    0x4e4f,                 // trap #15
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0
    0x7203,                 // moveq #$03,d1      _S_CMDOUT
    0x7606,                 // moveq #6,d3
    0x43f9, 0x0010, 0x0200, // lea $100200,a1     READ(6) の CDB
    0x4e4f,                 // trap #15
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0
    0x7204,                 // moveq #$04,d1      _S_DATAIN
    0x7600,                 // moveq #0,d3        256 バイト
    0x43f9, 0x1030, 0x5400, // lea $10305400,a1
    0x4e4f,                 // trap #15
    0x23c0, 0x0010, 0x0118, // move.l d0,$100118
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0
    0x7206,                 // moveq #$06,d1      _S_STSIN
    0x43f9, 0x0010, 0x0210, // lea $100210,a1
    0x4e4f,                 // trap #15
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0
    0x7207,                 // moveq #$07,d1      _S_MSGIN
    0x43f9, 0x0010, 0x0212, // lea $100212,a1
    0x4e4f,                 // trap #15
    0x41f9, 0x1030, 0x5400, // lea $10305400,a0
    0x43f9, 0x0011, 0x1000, // lea $111000,a1
    0x703f,                 // moveq #63,d0
    0x22d8,                 // move.l (a0)+,(a1)+
    0x51c8, 0xfffc,         // dbra d0,*-2
    // 16MB の境界: _S_DATAOUT に d3 = 0 (256 バイト) を渡し、DMA で転送したか (チャネル 1 の MAR が変わる) を見る。
    // バッファ $00FFFF80 は境界をまたぐ (ソフト転送のはず)。$00FFFF00 はちょうど境界で終わる (DMA のはず)
    0x23fc, 0x1234, 0x5678, 0x00e8, 0x404c, // move.l #$12345678,$e8404c  DMAC のチャネル 1 の MAR に目じるしの値を置く
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0
    0x7201,                 // moveq #$01,d1      _S_SELECT
    0x7801,                 // moveq #1,d4        ID 1
    0x4e4f,                 // trap #15
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0
    0x7203,                 // moveq #$03,d1      _S_CMDOUT
    0x7606,                 // moveq #6,d3
    0x43f9, 0x0010, 0x0220, // lea CDB,a1         WRITE(6)、1 ブロック
    0x4e4f,                 // trap #15
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0
    0x7205,                 // moveq #$05,d1      _S_DATAOUT
    0x7600,                 // moveq #0,d3        256 バイト
    0x43f9, 0x00ff, 0xff80, // lea バッファ,a1
    0x4e4f,                 // trap #15
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0
    0x7206,                 // moveq #$06,d1      _S_STSIN
    0x43f9, 0x0010, 0x0210, // lea $100210,a1
    0x4e4f,                 // trap #15
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0
    0x7207,                 // moveq #$07,d1      _S_MSGIN
    0x43f9, 0x0010, 0x0212, // lea $100212,a1
    0x4e4f,                 // trap #15
    0x23f9, 0x00e8, 0x404c, 0x0010, 0x011c, // move.l $e8404c,結果
    0x23fc, 0x1234, 0x5678, 0x00e8, 0x404c, // move.l #$12345678,$e8404c  DMAC のチャネル 1 の MAR に目じるしの値を置く
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0
    0x7201,                 // moveq #$01,d1      _S_SELECT
    0x7801,                 // moveq #1,d4        ID 1
    0x4e4f,                 // trap #15
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0
    0x7203,                 // moveq #$03,d1      _S_CMDOUT
    0x7606,                 // moveq #6,d3
    0x43f9, 0x0010, 0x0228, // lea CDB,a1         WRITE(6)、1 ブロック
    0x4e4f,                 // trap #15
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0
    0x7205,                 // moveq #$05,d1      _S_DATAOUT
    0x7600,                 // moveq #0,d3        256 バイト
    0x43f9, 0x00ff, 0xff00, // lea バッファ,a1
    0x4e4f,                 // trap #15
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0
    0x7206,                 // moveq #$06,d1      _S_STSIN
    0x43f9, 0x0010, 0x0210, // lea $100210,a1
    0x4e4f,                 // trap #15
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0
    0x7207,                 // moveq #$07,d1      _S_MSGIN
    0x43f9, 0x0010, 0x0212, // lea $100212,a1
    0x4e4f,                 // trap #15
    0x23f9, 0x00e8, 0x404c, 0x0010, 0x0120, // move.l $e8404c,結果
    // ローカルメモリ (16MB) の終わりをまたぐ: $10FFFE00 からの 1024 バイト (後ろの 512 バイトは SIMM のないところ)
    0x41f9, 0x10ff, 0xfe00, // lea $10fffe00,a0
    0x303c, 0x01ff,         // move.w #511,d0
    0x10c0,                 // move.b d0,(a0)+    書くデータ: i バイト目は (511 - i) の下位 8 ビット
    0x51c8, 0xfffc,         // dbra d0,*-2
    0x7045,                 // moveq #$45,d0      _B_WRITE
    0x323c, 0x9070,         // move.w #$9070,d1
    0x243c, 0x0300, 0x0005, // move.l #$03000005,d2  セクタ 5
    0x263c, 0x0000, 0x0400, // move.l #1024,d3
    0x43f9, 0x10ff, 0xfe00, // lea $10fffe00,a1
    0x4e4f,                 // trap #15
    0x7046,                 // moveq #$46,d0      _B_READ
    0x323c, 0x9070,         // move.w #$9070,d1
    0x243c, 0x0300, 0x0003, // move.l #$03000003,d2  セクタ 3 (上で書いたもの)
    0x263c, 0x0000, 0x0400, // move.l #1024,d3
    0x43f9, 0x10ff, 0xfe00, // lea $10fffe00,a1
    0x4e4f,                 // trap #15
    0x41f9, 0x10ff, 0xfe00, // lea $10fffe00,a0
    0x43f9, 0x0011, 0x1800, // lea $111800,a1
    0x707f,                 // moveq #127,d0
    0x22d8,                 // move.l (a0)+,(a1)+
    0x51c8, 0xfffc,         // dbra d0,*-2
    0x23cf, 0x0010, 0x0128, // move.l sp,$100128  終わりの SSP
    0x23fc, 0x0000, 0x0001, 0x0010, 0x010c, // move.l #1,$10010c
    0x60fe,                 // bra.s *
};
static_assert(sizeof kProgram <= kResult - kCode, "the program must end before the results");

// ディスクのイメージ: 512 バイト x 256 ブロック。バイトはブロック番号と位置から作る (先頭は X68SCSI1 でないので起動しない)
const uint32_t kBlocks = 256;
uint8_t g_disk[kBlocks * 512];

// フロッピーディスクのイメージ: 2HD (1024 バイト x 8 セクタ x 2 面 x 77 シリンダ) の空のもの (起動しない)
uint8_t g_floppy[1024 * 8 * 2 * 77];

// ID 1 のディスク: 256 バイト x 64 ブロック (_S_DATAIN の d3 = 0 で 1 ブロックを読むため)
uint8_t g_disk256[64 * 256];

uint8_t diskByte(uint32_t lba, uint32_t i) { return (uint8_t)(lba * 7 + i * 13 + (i >> 8) + 1); }

x68k::Machine* gm;

uint8_t peek(uint32_t a) {
    bool ok;
    return gm->peek8(a, ok);
}

uint32_t peek32(uint32_t a) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | peek(a + i);
    return v;
}

void poke16(uint32_t a, uint16_t v) {
    gm->write8(a, (uint8_t)(v >> 8), m68k::FC_SUPER_DATA);
    gm->write8(a + 1, (uint8_t)v, m68k::FC_SUPER_DATA);
}

int failures = 0;

void check(bool ok, const char* what, uint32_t value) {
    printf("%s: %s (%08x)\n", what, ok ? "ok" : "FAILED", value);
    if (!ok) failures++;
}

} // namespace

int main(int argc, char** argv) {
    const char* iplPath = nullptr;
    bool dmaBlock = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ipl") && i + 1 < argc) iplPath = argv[++i];
        else if (!strcmp(argv[i], "--local-stack")) kStack = 0x10fff000;
        else if (!strcmp(argv[i], "--no-dma-block")) dmaBlock = false;
        else {
            fprintf(stderr, "usage: %s [--ipl IPLROM30.DAT] [--local-stack] [--no-dma-block]\n", argv[0]);
            return 2;
        }
    }
    static uint8_t ipl[x68k::Machine::IPL_SIZE];
    FILE* f = iplPath ? fopen(iplPath, "rb") : nullptr;
    const bool got = f && fread(ipl, 1, sizeof ipl, f) == sizeof ipl;
    if (f) fclose(f);
    if (!got) {
        fprintf(stderr, "cannot read %s; skipped\n", iplPath ? iplPath : "(no --ipl)");
        return 77;
    }
    static const uint8_t kRomver30[] = {0x20, 0x3c, 0x13, 0x92, 0x11, 0x27, 0x4e, 0x75};
    if (memcmp(ipl + 0x10030, kRomver30, sizeof kRomver30) != 0) {
        fprintf(stderr, "%s is not the IPL ROM 1.3 of the X68030; skipped\n", iplPath);
        return 77;
    }
    x68k::Config config;
    if (!config.setCpu(68060)) {
        fprintf(stderr, "no 68060 core in this build; skipped\n");
        return 77;
    }
    config.localMemSize = 16u << 20;
    static x68k::Machine m(config);
    gm = &m;
    static x68k::HardDiskImage disk;
    static x68k::FloppyImage floppy;
    static x68k::HardDiskImage disk256;
    for (uint32_t i = 0; i < sizeof g_disk256; i++) g_disk256[i] = (uint8_t)(i * 5 + (i >> 8) + 3);
    for (uint32_t b = 0; b < kBlocks; b++)
        for (uint32_t i = 0; i < 512; i++) g_disk[b * 512 + i] = diskByte(b, i);
    static uint8_t scsi30[x68k::Machine::SCSI_ROM_SIZE];
    x68k::Machine::makeScsiRom30(ipl, scsi30);
    if (!m.ok() || !m.setIplRom(ipl, sizeof ipl) || !m.setScsiRom(scsi30, sizeof scsi30) ||
        !disk.load(g_disk, sizeof g_disk, 512, false) || !floppy.load(g_floppy, sizeof g_floppy, false, "test.xdf") ||
        !disk256.load(g_disk256, sizeof g_disk256, 256, false)) {
        fprintf(stderr, "cannot set up the machine\n");
        return 1;
    }
    m.attachScsi(0, &disk);
    m.attachScsi(1, &disk256);
    m.setTime(1767225600);
    m.powerOn();
    for (int i = 0; i < 400; i++) m.runFrame(); // IPL ROM がディスクを待つところまで

    // 割り込みを受け付けている (マスク 0) ときに飛ばす (割り込みの処理の途中で横取りしない)
    m68k::Registers r;
    for (int i = 0; i < 200; i++) {
        m.cpu020()->getRegisters(r);
        if ((r.sr & 0x0700) == 0) break;
        m.run(100);
    }
    if (r.sr & 0x0700) {
        printf("the interrupt mask never became 0 (sr=%04x)\n", r.sr);
        return 1;
    }
    for (size_t i = 0; i < sizeof kProgram / 2; i++) poke16(kCode + (uint32_t)i * 2, kProgram[i]);
    for (uint32_t a = kResult; a < kResult + 44; a += 2) poke16(a, 0xffff);
    static uint8_t vectors[0x400]; // 例外ベクタの表 (ローカルメモリの終わりをまたぐ転送で変わらないことを見る)
    for (uint32_t i = 0; i < sizeof vectors; i++) vectors[i] = peek(i);
    poke16(kCdb + 0x20, 0x0a00); // WRITE(6)、ブロック 20、1 ブロック
    poke16(kCdb + 0x22, 0x0014);
    poke16(kCdb + 0x24, 0x0100);
    poke16(kCdb + 0x28, 0x0a00); // WRITE(6)、ブロック 21、1 ブロック
    poke16(kCdb + 0x2a, 0x0015);
    poke16(kCdb + 0x2c, 0x0100);
    poke16(kCdb, 0x0800); // READ(6)、ブロック 9、1 ブロック
    poke16(kCdb + 2, 0x0009);
    poke16(kCdb + 4, 0x0100);
    poke16(kResult + 12, 0);
    poke16(kResult + 14, 0);
    for (uint32_t a = kWrap; a < kWrap + kWrapSize; a += 2) poke16(a, (uint16_t)(kMark << 8 | kMark));
    // フロッピーディスクはここで入れる (起動のときに入っていると、IPL ROM が FDC を使っている途中で横取りすることがある)
    m.insertDisk(0, &floppy);
    // ここから先の DMA のまとめた転送は、フロッピーディスクからローカルメモリへの読み込み (2048 バイトと、ローカルメモリの
    // 終わりまでの 512 バイト) だけ (SCSI はソフト転送か、メモリからの書き込み)
    static x68k::Profile profile;
    profile.clock = []() -> uint64_t { return 0; }; // 時間は測らない (まとめて転送したバイト数だけ使う)
    m.setProfile(&profile);
    m.setDmaBlock(dmaBlock);
    r.pc = kCode;
    r.sr = 0x2000;
    r.ssp = kStack;
    m.cpu020()->setRegisters(r);
    for (int i = 0; i < 900 && peek32(kResult + 12) != 1; i++) m.runFrame();

    // IPL ROM 1.3 は 68060 の MMU を見つけられないので、patchIplFor060 が IOCS のワークの MMU の有無 ($CBE) を「あり」にする
    check(peek(0xcbe) == 0xff, "IOCS work $CBE: the MMU is present", peek(0xcbe));
    // 空の SRAM で電源を入れたので、キャッシュの設定 ($ED0090) は命令・データとも有効 (3) で、IPL ROM の初期化のあとも残る
    check(m.sram()[0x90] == 0x03, "new SRAM: $ED0090 (cache) is 3", m.sram()[0x90]);
    // IOCS のワークの $0CB8 (ROM の上の dbra のループの 1ms あたりの回数) は、実機の 060turbo の値 8333
    check((peek32(0xcb8) >> 16) == 8333, "IOCS work $0CB8 is 8333", peek32(0xcb8));
    check(peek32(kResult + 12) == 1, "the program finished", peek32(kResult + 12));
    if (peek32(kResult + 12) != 1) {
        m.cpu020()->getRegisters(r);
        for (int i = 0; i < 24; i += 4) printf("  result+%d = %08x\n", i, peek32(kResult + (uint32_t)i));
        printf("  stopped at pc=%08x sr=%04x d0=%08x d1=%08x a1=%08x\n", r.pc, r.sr, r.d[0], r.d[1], r.a[1]);
    }
    check(peek32(kResult) == 0, "_S_READEXT returned 0", peek32(kResult));
    bool same = true;
    for (uint32_t i = 0; i < 1024; i++)
        if (peek(kCopy + i) != g_disk[8 * 512 + i]) same = false;
    check(same, "READ(10) to local memory: blocks 8-9 match the image", peek32(kCopy));
    check(peek32(kResult + 4) == 0, "_S_READ returned 0", peek32(kResult + 4));
    same = true;
    for (uint32_t i = 0; i < 512; i++)
        if (peek(kCopy + 0x400 + i) != g_disk[20 * 512 + i]) same = false;
    check(same, "READ(6) to local memory: block 20 matches the image", peek32(kCopy + 0x400));
    check(peek32(kResult + 8) == 0, "_S_WRITEEXT returned 0", peek32(kResult + 8));
    same = true;
    for (uint32_t i = 0; i < 1024; i++)
        if (g_disk[40 * 512 + i] != (uint8_t)(1023 - i)) same = false;
    check(same, "WRITE(10) from local memory: blocks 40-41 hold the data", g_disk[40 * 512]);
    check(peek32(kResult + 24) == 0, "_S_DATAIN (d3 = 0) returned 0", peek32(kResult + 24));
    same = true;
    for (uint32_t i = 0; i < 256; i++)
        if (peek(kCopy + 0x1000 + i) != g_disk256[9 * 256 + i]) same = false;
    check(same, "_S_DATAIN of 256 bytes (d3 = 0) to local memory: block 9 matches", peek32(kCopy + 0x1000));
    // 境界: またぐときは DMAC を使わない (MAR は置いた値のまま)。ちょうど境界で終わるときは DMA (MAR は $01000000 まで進む)
    check(peek32(kResult + 28) == 0x12345678, "_S_DATAOUT of 256 bytes across $01000000: no DMA", peek32(kResult + 28));
    check(peek32(kResult + 32) == 0x01000000, "_S_DATAOUT of 256 bytes ending at $01000000: DMA", peek32(kResult + 32));
    same = true;
    for (uint32_t i = 0; i < 256; i++)
        if (g_disk256[21 * 256 + i] != peek(0xffff00 + i)) same = false;
    check(same, "the block written by DMA holds the bytes at $FFFF00", g_disk256[21 * 256]);
    // フロッピーディスク: エラーなら d0 のビット 30 (FDC の ST0 の異常終了) などが立つ
    check((peek32(kResult + 16) & 0xc0000000) == 0, "_B_WRITE returned without an error", peek32(kResult + 16));
    same = true;
    for (uint32_t i = 0; i < 2048; i++)
        if (g_floppy[2 * 1024 + i] != (uint8_t)(2047 - i)) same = false;
    check(same, "floppy write from local memory: sectors 3-4 hold the data", g_floppy[2 * 1024]);
    check((peek32(kResult + 20) & 0xc0000000) == 0, "_B_READ returned without an error", peek32(kResult + 20));
    same = true;
    for (uint32_t i = 0; i < 2048; i++)
        if (peek(kCopy + 0x800 + i) != (uint8_t)(2047 - i)) same = false;
    check(same, "floppy read to local memory: sectors 3-4 match", peek32(kCopy + 0x800));
    // ローカルメモリの終わりをまたぐ転送: 積んであるところだけ読み書きし、積んでいないところは読むと $FF、書いても変わらない。
    // 本体のメモリ (24 ビットで見た $000000 から) には折り返さない
    same = true;
    for (uint32_t i = 0; i < 1024; i++)
        if (g_floppy[4 * 1024 + i] != (i < 512 ? (uint8_t)(511 - i) : 0xff)) same = false;
    check(same, "floppy write across the end of local memory: 512 bytes of data, then $FF", g_floppy[4 * 1024 + 512]);
    same = true;
    for (uint32_t i = 0; i < 512; i++)
        if (peek(kCopy + 0x1800 + i) != (uint8_t)(2047 - i)) same = false;
    check(same, "floppy read across the end of local memory: the first 512 bytes arrive", peek32(kCopy + 0x1800));
    same = true;
    for (uint32_t i = 0; i < sizeof vectors; i++)
        if (peek(i) != vectors[i]) same = false;
    check(same, "the exception vectors are untouched", peek32(0));
    // まとめた転送: 1 セクタ (DMAC の 1 ブロック) の最後のバイトは 1 バイトずつの処理なので、2 セクタで 2046 バイト、
    // ローカルメモリの終わりまでの 512 バイトと合わせて 2558 バイト。それより多ければ、終わりを越えて写している
    if (dmaBlock)
        check(profile.dmaBlockBytes == 2046 + 512, "block transfers to local memory: 2046 + 512 bytes",
              (uint32_t)profile.dmaBlockBytes);
    else
        check(profile.dmaBlockBytes == 0, "no block transfers", (uint32_t)profile.dmaBlockBytes);
    check(peek32(kResult + 36) == kStack && peek32(kResult + 40) == kStack, "SSP is the same before and after",
          peek32(kResult + 40));
    same = true;
    for (uint32_t i = 0; i < kWrapSize; i++)
        if (peek(kWrap + i) != kMark) same = false;
    check(same, "main memory at the same low 24 bits is untouched", peek32(kWrap));

    // まとめた転送を断る場合 (Machine::dmaBlockCopy() のローカルメモリの分): メモリ側がローカルメモリでも、オートリクエストの
    // 転送と、デバイス側が FDC のデータのレジスタ ($E94003) でない転送は、まとめずに 1 バイトずつ行う。
    // DMAC のチャネル 0 を直接設定する (プログラムは bra で止まっている)
    {
        const int fc = m68k::FC_SUPER_DATA;
        const uint32_t b = 0xe84000;
        auto dma = [&](uint8_t ocr, uint32_t dar, uint32_t mar, uint32_t count) {
            m.write8(b + 0x00, 0xff, fc);
            m.write8(b + 0x04, 0x80, fc); // DCR: 8 ビットのポート
            m.write8(b + 0x05, ocr, fc);  // OCR: デバイス → メモリ、バイト
            m.write8(b + 0x06, 0x04, fc); // SCR: メモリ側は増やす、デバイス側は固定
            m.write8(b + 0x0a, (uint8_t)(count >> 8), fc);
            m.write8(b + 0x0b, (uint8_t)count, fc);
            for (int i = 0; i < 4; i++) m.write8(b + 0x0c + i, (uint8_t)(mar >> (24 - 8 * i)), fc);
            for (int i = 0; i < 4; i++) m.write8(b + 0x14 + i, (uint8_t)(dar >> (24 - 8 * i)), fc);
            m.write8(b + 0x29, (uint8_t)fc, fc);
            m.write8(b + 0x31, (uint8_t)fc, fc);
            m.write8(b + 0x07, 0x80, fc);
        };
        auto mtc = [&]() { return (uint32_t)(m.read8(b + 0x0a, fc) << 8 | m.read8(b + 0x0b, fc)); };
        // FDC を READ DATA の実行フェーズにする (前のコマンドの結果が残っていれば読み捨てる)。DMA の転送でデータの要求が出て、
        // 読まれないまま時間がたつとオーバーランで終わるので、要求が出たところで戻る。出なければ false
        auto startRead = [&]() {
            for (int i = 0; i < 16 && (m.read8(0xe94001, fc) & 0xd0) == 0xd0; i++) m.read8(0xe94003, fc);
            const uint8_t cmd[9] = {0x46, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x1b, 0xff}; // READ DATA、N=3、EOT=8
            for (uint8_t v : cmd) m.write8(0xe94003, v, fc);
            for (int i = 0; i < 2000 && !m.fdc().dataReady(); i++) m.run(200);
            return m.fdc().dataReady();
        };
        // 外部要求 (REQG = 10) で、デバイス側が FDC のステータスのレジスタ ($E94001)。FDC が要求を出している間、
        // ステータスのレジスタの値が 64 バイト入る (FDC のデータは読まれない)
        check(startRead(), "the FDC asks for a transfer (before the transfer from the status register)",
              m.read8(0xe94001, fc));
        uint64_t before = profile.dmaBlockBytes;
        dma(0x82, 0x00e94001, 0x10306800, 64);
        m.run(40000);
        check(mtc() == 0, "external transfer from the FDC status register finished", mtc());
        check(profile.dmaBlockBytes == before, "external transfer from another register: not a block transfer",
              (uint32_t)(profile.dmaBlockBytes - before));
        // オートリクエスト (OCR の REQG = 00) で、FDC のデータのレジスタから 64 バイト。READ DATA を始め直し、データが出ている
        // 間に行う (データが出ていなければ、条件を確かめなくても、まとめた転送は 0 バイトになってしまう)
        m.run(4000000); // 上の READ DATA がオーバーランで終わるのを待つ
        check(startRead(), "the FDC asks for a transfer (before the auto-request transfer)", m.read8(0xe94001, fc));
        before = profile.dmaBlockBytes;
        dma(0x80, 0x00e94003, 0x10306400, 64);
        m.run(40000);
        check(mtc() == 0, "auto-request transfer to local memory finished", mtc());
        check(profile.dmaBlockBytes == before, "auto-request transfer to local memory: not a block transfer",
              (uint32_t)(profile.dmaBlockBytes - before));
    }

    // 中身のある SRAM (IPL ROM が初期化したあと) のキャッシュの設定は、電源を入れ直しても変えない
    m.sram()[0x90] = 0x00;
    m.powerOn();
    check(m.sram()[0x90] == 0x00, "initialized SRAM: $ED0090 is kept at power on", m.sram()[0x90]);

    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
