// 外付け SCSI ボード (CZ-6BS1 相当) の ROM の SCSI IOCS から、CD-ROM 装置が読めるかを確かめる。
// SASI の機種 (68000) に外付け SCSI の ROM を渡し、ID 6 に CD-ROM のイメージ (テスト用に作った 2048 バイト /
// セクタのもの) をつなぐ。Human68k をフロッピーディスクから起動してプロンプトが出たら、RAM に小さなプログラムを
// 書いて CPU をそこに飛ばし、IOCS _SCSIDRV ($F5) の _S_INQUIRY ($20)、_S_READCAP ($25)、_S_READEXT ($26) を呼ぶ。
// 結果 (d0 と読んだデータ) を RAM から取り出して調べる。
//   scsi_iocs [-r ROM のディレクトリ] --scsi-ex-rom ROM -0 起動ディスク
// ROM か起動ディスクがなければ 77 (ctest の SKIP_RETURN_CODE)。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x68k/machine.h"

static uint8_t* loadWhole(const char* path, size_t& size) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = n > 0 ? new uint8_t[(size_t)n] : nullptr;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        delete[] buf;
        buf = nullptr;
    }
    fclose(f);
    size = buf ? (size_t)n : 0;
    return buf;
}

namespace {

const uint32_t kCode = 0x300000;  // プログラムを置く場所 (起動直後の Human68k は使っていない)
const uint32_t kStack = 0x2ff000; // プログラムのスーパーバイザスタック
const uint32_t kResult = 0x300100; // d0: +0 INQUIRY、+4 READCAP、+8 READEXT、+12 終わったら 1
const uint32_t kInquiry = 0x300200, kCapacity = 0x300300, kData = 0x301000;
const int kCdId = 6;

// 68000 のプログラム (IOCS の引数: d1 = 機能、d2 = ブロック番号、d3 = 長さ / ブロック数、d4 = SCSI ID、
// d5 = ブロックの大きさ (256 << d5)、a1 = バッファ)
const uint16_t kProgram[] = {
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0     _SCSIDRV
    0x7220,                 // moveq #$20,d1      _S_INQUIRY
    0x7624,                 // moveq #36,d3
    0x7806,                 // moveq #6,d4
    0x43f9, 0x0030, 0x0200, // lea $300200,a1
    0x4e4f,                 // trap #15
    0x23c0, 0x0030, 0x0100, // move.l d0,$300100
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0
    0x7225,                 // moveq #$25,d1      _S_READCAP
    0x7806,                 // moveq #6,d4
    0x43f9, 0x0030, 0x0300, // lea $300300,a1
    0x4e4f,                 // trap #15
    0x23c0, 0x0030, 0x0104, // move.l d0,$300104
    0x203c, 0x0000, 0x00f5, // move.l #$f5,d0
    0x7226,                 // moveq #$26,d1      _S_READEXT
    0x7410,                 // moveq #16,d2       ブロック 16 (ISO 9660 の基本ボリューム記述子)
    0x7602,                 // moveq #2,d3        2 ブロック
    0x7806,                 // moveq #6,d4
    0x7a03,                 // moveq #3,d5        2048 バイト
    0x43f9, 0x0030, 0x1000, // lea $301000,a1
    0x4e4f,                 // trap #15
    0x23c0, 0x0030, 0x0108, // move.l d0,$300108
    0x23fc, 0x0000, 0x0001, 0x0030, 0x010c, // move.l #1,$30010c
    0x60fe,                 // bra.s *
};

// CD-ROM のイメージ: 64 セクタ。セクタ 16 は ISO 9660 の基本ボリューム記述子の先頭 (01 'CD001' 01)、
// それ以外のバイトはセクタ番号と位置から作る
const uint32_t kCdBlocks = 64;
uint8_t g_cd[kCdBlocks * 2048];

uint8_t cdByte(uint32_t lba, uint32_t i) { return (uint8_t)(lba * 7 + i * 13 + (i >> 8)); }

void makeCd() {
    for (uint32_t b = 0; b < kCdBlocks; b++)
        for (uint32_t i = 0; i < 2048; i++) g_cd[b * 2048 + i] = cdByte(b, i);
    memcpy(g_cd + 16 * 2048, "\x01" "CD001" "\x01", 7);
}

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

void frames(int n) {
    for (int i = 0; i < n; i++) gm->runFrame();
}

int failures = 0;

void check(bool ok, const char* what, uint32_t value) {
    printf("%s: %s (%08x)\n", what, ok ? "ok" : "FAILED", value);
    if (!ok) failures++;
}

} // namespace

int main(int argc, char** argv) {
    char romDir[1024];
    const char* home = getenv("HOME");
    snprintf(romDir, sizeof romDir, "%s/.cache/pix68k/rom", home ? home : ".");
    const char* disk = nullptr;
    const char* exRom = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) snprintf(romDir, sizeof romDir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "-0") && i + 1 < argc) disk = argv[++i];
        else if (!strcmp(argv[i], "--scsi-ex-rom") && i + 1 < argc) exRom = argv[++i];
        else {
            fprintf(stderr, "usage: %s [-r romdir] --scsi-ex-rom rom -0 bootdisk\n", argv[0]);
            return 2;
        }
    }
    char path[1100];
    snprintf(path, sizeof path, "%s/IPLROM.DAT", romDir);
    size_t iplSize = 0, exSize = 0, size = 0;
    uint8_t* ipl = loadWhole(path, iplSize);
    if (!ipl || iplSize != x68k::Machine::IPL_SIZE) {
        fprintf(stderr, "cannot read %s; skipped\n", path);
        return 77;
    }
    uint8_t* ex = exRom ? loadWhole(exRom, exSize) : nullptr;
    if (!ex) {
        fprintf(stderr, "cannot read the SCSI ROM %s; skipped\n", exRom ? exRom : "(none)");
        return 77;
    }
    uint8_t* data = disk ? loadWhole(disk, size) : nullptr;
    if (!data) {
        fprintf(stderr, "cannot read the boot disk %s; skipped\n", disk ? disk : "(none)");
        return 77;
    }
    static x68k::Machine m;
    gm = &m;
    static x68k::FloppyImage image;
    static x68k::HardDiskImage cd;
    makeCd();
    if (!m.ok() || !image.load(data, size, false, disk) || !cd.load(g_cd, sizeof g_cd, 2048, true) ||
        !m.setScsiExRom(ex, exSize)) {
        fprintf(stderr, "cannot set up the machine\n");
        return 1;
    }
    m.setIplRom(ipl, iplSize);
    m.insertDisk(0, &image);
    m.attachScsiEx(kCdId, &cd, true);
    m.setTime(1767225600);
    m.powerOn();
    frames(900); // Human68k のプロンプトまで

    // 割り込みを受け付けている (マスク 0) ときに飛ばす (割り込みの処理の途中で横取りしない)
    m68k::Registers r;
    for (int i = 0; i < 200; i++) {
        m.cpu().getRegisters(r);
        if ((r.sr & 0x0700) == 0) break;
        m.run(100);
    }
    if (r.sr & 0x0700) {
        printf("the interrupt mask never became 0 (sr=%04x)\n", r.sr);
        return 1;
    }
    for (size_t i = 0; i < sizeof kProgram / 2; i++) poke16(kCode + (uint32_t)i * 2, kProgram[i]);
    for (uint32_t a = kResult; a < kResult + 16; a += 2) poke16(a, 0xffff);
    poke16(kResult + 12, 0);
    poke16(kResult + 14, 0);
    r.pc = kCode;
    r.sr = 0x2000;
    r.ssp = kStack;
    r.prefetch[0] = kProgram[0];
    r.prefetch[1] = kProgram[1];
    m.cpu().setRegisters(r);
    for (int i = 0; i < 120 && peek32(kResult + 12) != 1; i++) frames(1);

    check(peek32(kResult + 12) == 1, "the program finished", peek32(kResult + 12));
    check(peek32(kResult) == 0, "_S_INQUIRY returned 0", peek32(kResult));
    check(peek(kInquiry) == 0x05, "INQUIRY: peripheral device type 5 (CD-ROM)", peek(kInquiry));
    check(peek(kInquiry + 1) == 0x80, "INQUIRY: removable medium", peek(kInquiry + 1));
    check(peek32(kResult + 4) == 0, "_S_READCAP returned 0", peek32(kResult + 4));
    check(peek32(kCapacity) == kCdBlocks - 1, "READ CAPACITY: last block", peek32(kCapacity));
    check(peek32(kCapacity + 4) == 2048, "READ CAPACITY: block length", peek32(kCapacity + 4));
    check(peek32(kResult + 8) == 0, "_S_READEXT returned 0", peek32(kResult + 8));
    bool same = true;
    for (uint32_t i = 0; i < 2 * 2048; i++)
        if (peek(kData + i) != g_cd[16 * 2048 + i]) same = false;
    check(same, "READ(10): blocks 16-17 match the image", peek32(kData));

    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
