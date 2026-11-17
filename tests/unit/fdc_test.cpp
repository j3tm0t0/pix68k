// フロッピーディスクのイメージ、FDC (uPD72065)、DMAC (HD63450) のテスト。
// CPU は使わず、テスト用のバス (64KB のメモリ + $E94001/$E94003 の FDC) の上で DMAC を動かす。
// DMA の再入だけは Machine の上で確かめる
#include <stdio.h>
#include <string.h>

#include <initializer_list>

#include "x68k/dmac.h"
#include "x68k/fdc.h"
#include "x68k/fdd.h"
#include "x68k/machine.h"
#include "x68k/state.h"

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

// XDF のイメージ: セクタごとに中身を変える (先頭のバイト = トラック、2 バイト目 = R、残りは位置)
uint8_t g_xdf[1261568];

void makeXdf() {
    for (int t = 0; t < 154; t++)
        for (int r = 1; r <= 8; r++) {
            uint8_t* p = g_xdf + (t * 8 + r - 1) * 1024;
            for (int i = 0; i < 1024; i++) p[i] = (uint8_t)(i + t + r);
            p[0] = (uint8_t)t;
            p[1] = (uint8_t)r;
        }
}

// テスト用のバス。
//   $0000-$FFFF: メモリ (busErrorAt の番地はバスエラー、ユーザの FC では $8000 以上がバスエラー)
//   $E94001 / $E94003: FDC
//   $F00000-$F0000F: 書き込みを順に記録するポート
struct TestBus : x68k::Dmac::Host {
    uint8_t mem[0x10000];
    x68k::Fdc* fdc;
    uint32_t busErrorAt = 0xffffffff;
    int done[4] = {}, errors[4] = {};
    uint32_t log[16];
    int logLen = 0;
    TestBus(x68k::Fdc* f) : fdc(f) { clear(); }
    void clear() {
        memset(mem, 0, sizeof mem);
        busErrorAt = 0xffffffff;
        for (int i = 0; i < 4; i++) done[i] = errors[i] = 0;
        logLen = 0;
    }
    static bool user(int fc) { return fc == m68k::FC_USER_DATA || fc == m68k::FC_USER_PROG; }
    void record(uint32_t a, uint8_t v) {
        if (logLen < 16) log[logLen++] = a << 8 | v;
    }
    bool dmaRead8(uint32_t a, uint8_t& v, int fc) override {
        if (a == busErrorAt) return false;
        if (a < sizeof mem) {
            if (user(fc) && a >= 0x8000) return false;
            v = mem[a];
            return true;
        }
        if (a == 0xe94001 || a == 0xe94003) {
            v = fdc->read(a & 0x1fff);
            return true;
        }
        return false;
    }
    bool dmaWrite8(uint32_t a, uint8_t v, int fc) override {
        if (a == busErrorAt) return false;
        if (a < sizeof mem) {
            if (user(fc) && a >= 0x8000) return false;
            mem[a] = v;
            record(a, v);
            return true;
        }
        if (a == 0xe94003) {
            fdc->write(a & 0x1fff, v);
            return true;
        }
        if (a >= 0xf00000 && a < 0xf00010) {
            record(a, v);
            return true;
        }
        return false;
    }
    bool dmaReady(int ch) override { return ch == 0 && fdc->dataReady(); }
    void dmaDone(int ch) override {
        done[ch]++;
        if (ch == 0) fdc->terminalCount();
    }
    void dmaError(int ch) override {
        errors[ch]++;
        if (ch == 0) fdc->abortTransfer();
    }
};

void dmacWrite32(x68k::Dmac& d, int ch, int reg, uint32_t v) {
    for (int i = 0; i < 4; i++) d.write((uint32_t)(ch * 64 + reg + i), (uint8_t)(v >> (24 - 8 * i)));
}
uint32_t dmacRead32(x68k::Dmac& d, int ch, int reg) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = v << 8 | d.read((uint32_t)(ch * 64 + reg + i));
    return v;
}
void dmacWrite16(x68k::Dmac& d, int ch, int reg, uint32_t v) {
    d.write((uint32_t)(ch * 64 + reg), (uint8_t)(v >> 8));
    d.write((uint32_t)(ch * 64 + reg + 1), (uint8_t)v);
}

uint32_t dmacRead16(x68k::Dmac& d, int ch, int reg) {
    return (uint32_t)(d.read((uint32_t)(ch * 64 + reg)) << 8 | d.read((uint32_t)(ch * 64 + reg + 1)));
}

// チャネル 0 を FDC 用に設定して開始する (IOCS と同じ設定: 8 ビットのデバイス、外部要求、バイト単位)
void startFdcDma(x68k::Dmac& d, bool toMemory, uint32_t addr, uint32_t count) {
    d.write(0x00, 0xff); // CSR を消す
    d.write(0x04, 0x80); // DCR: XRM = 10 (サイクルスチール)、8 ビットのポート
    d.write(0x05, (uint8_t)((toMemory ? 0x80 : 0x00) | 0x02)); // OCR: 向き、バイト、チェーンなし、外部要求
    d.write(0x06, 0x04); // SCR: メモリ側は増やす、デバイス側は固定
    dmacWrite16(d, 0, 0x0a, count);
    dmacWrite32(d, 0, 0x0c, addr);
    dmacWrite32(d, 0, 0x14, 0xe94003);
    d.write(0x25, 0x64); // NIV
    d.write(0x27, 0x65); // EIV
    d.write(0x07, 0x88); // CCR: 開始、割り込みを許可
}

void fdcCommand(x68k::Fdc& f, std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes) f.write(3, b);
}

// 結果を n バイト読む
void fdcResult(x68k::Fdc& f, uint8_t* out, int n) {
    for (int i = 0; i < n; i++) {
        CHECK_EQ(f.read(1) & 0xd0, 0xd0); // RQM | DIO | CB
        out[i] = f.read(3);
    }
    CHECK_EQ(f.read(1), 0x80);
}

void testXdfImage() {
    printf("xdf image\n");
    x68k::FloppyImage img;
    CHECK_EQ(img.load(g_xdf, sizeof g_xdf, false, "a.xdf"), true);
    CHECK_EQ(img.format(), x68k::FloppyImage::XDF);
    CHECK_EQ(img.sectorCount(0), 8);
    CHECK_EQ(img.sectorCount(153), 8);
    CHECK_EQ(img.sectorCount(154), 0);
    const x68k::FloppyImage::Sector* s = img.find(3, 1, 1, 5, 3);
    CHECK_EQ(s != nullptr, true);
    if (s) {
        CHECK_EQ(img.data(s)[0], 3);
        CHECK_EQ(img.data(s)[1], 5);
    }
    CHECK_EQ(img.find(3, 1, 0, 5, 3) == nullptr, true); // H が違う
    CHECK_EQ(img.find(3, 1, 1, 9, 3) == nullptr, true); // R がない
}

void testD88Image() {
    printf("d88 image\n");
    // トラック 0 に 2 セクタ (R=1 は 256 バイト、R=2 は 128 バイトのデリーテッドデータ)、トラック 1 は未フォーマット
    static uint8_t d88[0x2b0 + 16 + 256 + 16 + 128];
    memset(d88, 0, sizeof d88);
    uint32_t size = sizeof d88;
    d88[0x1c] = (uint8_t)size;
    d88[0x1d] = (uint8_t)(size >> 8);
    d88[0x20] = 0xb0;
    d88[0x21] = 0x02; // トラック 0 は 0x2B0 から
    uint8_t* p = d88 + 0x2b0;
    p[0] = 0; p[1] = 0; p[2] = 1; p[3] = 1; p[4] = 2; p[0x0e] = 0x00; p[0x0f] = 0x01;
    memset(p + 16, 0xaa, 256);
    p += 16 + 256;
    p[0] = 0; p[1] = 0; p[2] = 2; p[3] = 0; p[4] = 2; p[7] = 0x10; p[0x0e] = 0x80;
    memset(p + 16, 0x55, 128);
    x68k::FloppyImage img;
    CHECK_EQ(img.load(d88, sizeof d88, false, "a.d88"), true);
    CHECK_EQ(img.sectorCount(0), 2);
    CHECK_EQ(img.sectorCount(1), 0);
    const x68k::FloppyImage::Sector* s = img.find(0, 0, 0, 2, 0);
    CHECK_EQ(s != nullptr, true);
    if (s) {
        CHECK_EQ(s->deleted, true);
        CHECK_EQ(s->size, 128);
        CHECK_EQ(img.data(s)[0], 0x55);
    }
    // ヘッダの大きさがファイルより大きければ読まない
    d88[0x1c] ^= 1;
    CHECK_EQ(img.load(d88, sizeof d88, false, "a.d88"), false);
    d88[0x1c] ^= 1;
}

void testDimImage() {
    printf("dim image\n");
    // 2HD で、トラック 0 と 2 だけがある (overtrack = 1 でフラグを使う)
    static uint8_t dim[256 + 2 * 8192];
    memset(dim, 0, sizeof dim);
    dim[0] = 0;
    dim[1 + 0] = 1;
    dim[1 + 2] = 1;
    dim[255] = 1;
    dim[256 + 8192 + 3 * 1024] = 0x42; // トラック 2 の R=4
    x68k::FloppyImage img;
    CHECK_EQ(img.load(dim, sizeof dim, false, "a.dim"), true);
    CHECK_EQ(img.sectorCount(0), 8);
    CHECK_EQ(img.sectorCount(1), 0);
    CHECK_EQ(img.sectorCount(2), 8);
    const x68k::FloppyImage::Sector* s = img.find(2, 1, 0, 4, 3);
    CHECK_EQ(s != nullptr, true);
    if (s) CHECK_EQ(img.data(s)[0], 0x42);
}

void testReadData() {
    printf("read data via dma\n");
    x68k::FloppyImage img;
    img.load(g_xdf, sizeof g_xdf, false, "a.xdf");
    x68k::Fdc fdc;
    fdc.reset();
    fdc.insert(0, &img);
    x68k::Dmac dmac;
    dmac.reset();
    static TestBus bus(&fdc);
    memset(bus.mem, 0, sizeof bus.mem);

    // シリンダ 2 へシークして、SENSE INTERRUPT STATUS で確かめる
    fdcCommand(fdc, {0x0f, 0x00, 0x02});
    CHECK_EQ(fdc.irq(), true);
    fdcCommand(fdc, {0x08});
    uint8_t r[7];
    fdcResult(fdc, r, 2);
    CHECK_EQ(r[0], 0x20);
    CHECK_EQ(r[1], 2);
    CHECK_EQ(fdc.irq(), false);

    // シリンダ 2、ヘッド 1 の R=7 と R=8 (EOT=8) を読む。DMA の転送数でちょうど終わる (TC)
    startFdcDma(dmac, true, 0x1000, 2 * 1024);
    fdcCommand(fdc, {0xc6, 0x04, 0x02, 0x01, 0x07, 0x03, 0x08, 0x1b, 0xff});
    CHECK_EQ(fdc.read(1), 0x10); // DMA で転送中
    dmac.run(bus);
    CHECK_EQ(bus.mem[0x1000], 5); // トラック 5 (シリンダ 2、ヘッド 1)
    CHECK_EQ(bus.mem[0x1001], 7);
    CHECK_EQ(bus.mem[0x1400], 5);
    CHECK_EQ(bus.mem[0x1401], 8);
    CHECK_EQ(bus.mem[0x1003], (uint8_t)(3 + 5 + 7));
    CHECK_EQ(memcmp(bus.mem + 0x1002, g_xdf + (5 * 8 + 6) * 1024 + 2, 1022), 0);
    CHECK_EQ(fdc.irq(), true);
    CHECK_EQ(dmac.irq(), true);
    CHECK_EQ(dmac.ack(), 0x64);
    fdcResult(fdc, r, 7);
    CHECK_EQ(r[0], 0x04); // 正常終了、ヘッド 1
    CHECK_EQ(r[1], 0);
    CHECK_EQ(r[3], 3);    // 最後のセクタが EOT なので、次のシリンダの R=1 を指す
    CHECK_EQ(r[5], 1);
    CHECK_EQ(dmac.read(0x00) & 0x88, 0x80); // COC、ACT は消えている
    dmac.write(0x00, 0xff);
    CHECK_EQ(dmac.irq(), false);

    // MT ありで、ヘッド 0 の EOT からヘッド 1 へ続けて読む
    startFdcDma(dmac, true, 0x2000, 2 * 1024);
    fdcCommand(fdc, {0xc6, 0x00, 0x02, 0x00, 0x08, 0x03, 0x08, 0x1b, 0xff});
    dmac.run(bus);
    CHECK_EQ(bus.mem[0x2000], 4);
    CHECK_EQ(bus.mem[0x2001], 8);
    CHECK_EQ(bus.mem[0x2400], 5);
    CHECK_EQ(bus.mem[0x2401], 1);
    fdcResult(fdc, r, 7);
    CHECK_EQ(r[0] & 0xc0, 0);
    CHECK_EQ(r[4], 1);
    CHECK_EQ(r[5], 2);
}

void testEndOfCylinder() {
    printf("end of cylinder without tc\n");
    x68k::FloppyImage img;
    img.load(g_xdf, sizeof g_xdf, false, "a.xdf");
    x68k::Fdc fdc;
    fdc.reset();
    fdc.insert(0, &img);
    x68k::Dmac dmac;
    dmac.reset();
    static TestBus bus(&fdc);
    // DMA は 3 セクタ分を用意するが、EOT が R=2 なので 2 セクタで FDC が止まる
    startFdcDma(dmac, true, 0x1000, 3 * 1024);
    fdcCommand(fdc, {0x46, 0x00, 0x00, 0x00, 0x01, 0x03, 0x02, 0x1b, 0xff});
    dmac.run(bus);
    uint8_t r[7];
    fdcResult(fdc, r, 7);
    CHECK_EQ(r[0], 0x40);
    CHECK_EQ(r[1], 0x80); // EN
    CHECK_EQ(dmac.active(0), true); // DMAC はまだ残りを待っている
}

void testErrors() {
    printf("fdc errors\n");
    x68k::FloppyImage img;
    img.load(g_xdf, sizeof g_xdf, true, "a.xdf");
    x68k::Fdc fdc;
    fdc.reset();
    uint8_t r[7];
    // ディスクがない: ノットレディ
    fdcCommand(fdc, {0x46, 0x00, 0x00, 0x00, 0x01, 0x03, 0x01, 0x1b, 0xff});
    fdcResult(fdc, r, 7);
    CHECK_EQ(r[0], 0x48);
    // 書き込み禁止
    fdc.insert(0, &img);
    fdcCommand(fdc, {0x45, 0x00, 0x00, 0x00, 0x01, 0x03, 0x01, 0x1b, 0xff});
    fdcResult(fdc, r, 7);
    CHECK_EQ(r[0], 0x40);
    CHECK_EQ(r[1], 0x02);
    // 存在しないセクタ
    fdcCommand(fdc, {0x46, 0x00, 0x00, 0x00, 0x09, 0x03, 0x09, 0x1b, 0xff});
    fdcResult(fdc, r, 7);
    CHECK_EQ(r[0], 0x40);
    CHECK_EQ(r[1], 0x04);
    // シーク結果がないときの SENSE INTERRUPT STATUS
    fdcCommand(fdc, {0x08});
    fdcResult(fdc, r, 1);
    CHECK_EQ(r[0], 0x80);
    // SENSE DEVICE STATUS: トラック 0、レディ、書き込み禁止、両面
    fdcCommand(fdc, {0x04, 0x00});
    fdcResult(fdc, r, 1);
    CHECK_EQ(r[0], 0x78);
    // READ ID は回転の順にセクタの ID を返す
    fdcCommand(fdc, {0x4a, 0x00});
    fdcResult(fdc, r, 7);
    CHECK_EQ(r[5], 1);
    fdcCommand(fdc, {0x4a, 0x00});
    fdcResult(fdc, r, 7);
    CHECK_EQ(r[5], 2);
    CHECK_EQ(r[6], 3);
}

void testWriteData() {
    printf("write data via dma\n");
    static uint8_t copy[sizeof g_xdf];
    memcpy(copy, g_xdf, sizeof copy);
    x68k::FloppyImage img;
    img.load(copy, sizeof copy, false, "a.xdf");
    x68k::Fdc fdc;
    fdc.reset();
    fdc.insert(0, &img);
    x68k::Dmac dmac;
    dmac.reset();
    static TestBus bus(&fdc);
    for (int i = 0; i < 1024; i++) bus.mem[0x3000 + i] = (uint8_t)(0xff - i);
    startFdcDma(dmac, false, 0x3000, 1024);
    fdcCommand(fdc, {0x45, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x1b, 0xff});
    dmac.run(bus);
    uint8_t r[7];
    fdcResult(fdc, r, 7);
    CHECK_EQ(r[0], 0x00);
    CHECK_EQ(img.dirty(), true);
    CHECK_EQ(memcmp(copy + 2 * 1024, bus.mem + 0x3000, 1024), 0);
    CHECK_EQ(memcmp(copy + 3 * 1024, g_xdf + 3 * 1024, 1024), 0); // 隣は変わらない
}

// SCAN 系 (Human68k の FD ドライバは、書き込んだあとの照合 (VERIFY) に SCAN EQUAL を使う)。
// 期待値は uPD765A のデータシートの「SCAN COMMANDS」と表 4 (ST2 の SH = $08、SN = $04) から作った。
// メモリの $3000 から count バイトを DMA で FDC に渡し、結果の 7 バイトを res に入れる
struct ScanRig {
    x68k::FloppyImage img;
    x68k::Fdc fdc;
    x68k::Dmac dmac;
    TestBus bus{&fdc};
    ScanRig(uint8_t* image, uint32_t size, const char* name) {
        img.load(image, size, false, name);
        fdc.reset(); // 電源を入れたときの状態から
        fdc.insert(0, &img); // ドライブ 0
        dmac.reset(); // チャネル 0 で転送する
    }
    // XDF のトラック 0 のセクタ r の中身を、渡すデータの n 番目のセクタの場所に置く
    uint8_t* put(int n, int r) {
        uint8_t* p = bus.mem + 0x3000 + n * 1024;
        memcpy(p, g_xdf + (r - 1) * 1024, 1024);
        return p;
    }
    void run(std::initializer_list<uint8_t> cmd, uint32_t count, uint8_t* res) {
        startFdcDma(dmac, false, 0x3000, count);
        fdcCommand(fdc, cmd); // コマンドフェーズ
        dmac.run(bus); // 実行フェーズ (DMA で CPU のデータを渡す)
        fdcResult(fdc, res, 7);
    }
};

// リザルトフェーズの 7 バイト (ST0、ST1、ST2、C、H、R、N) をすべて比べる
static void checkResult(const uint8_t* r, std::initializer_list<int> want) {
    int i = 0;
    for (int w : want) CHECK_EQ(r[i++], w);
}
void testScan() {
    printf("scan equal / low or equal / high or equal\n");
    static ScanRig g(g_xdf, sizeof g_xdf, "a.xdf");
    uint8_t r[7];
    // セクタ 3 の中身。$FF でないバイトを 1 つ選ぶ (位置 100 は (100 + 0 + 3) = $67)
    CHECK_EQ(g_xdf[2 * 1024 + 100], 0x67);

    // SCAN EQUAL: 同じデータなら SH = 1、SN = 0 で正常終了。R は一致したセクタ
    g.put(0, 3);
    g.run({0x51, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x1b, 0x01}, 1024, r);
    checkResult(r, {0x00, 0x00, 0x08, 0, 0, 3, 3});
    CHECK_EQ(r[0], 0x00);
    CHECK_EQ(r[1], 0x00);
    CHECK_EQ(r[2], 0x08);
    CHECK_EQ(r[5], 3);
    CHECK_EQ(g.img.dirty(), false); // ディスクには書かない

    // 1 バイト違い、EOT まで一致しない: SN = 1、SH = 0
    g.put(0, 3)[100] = 0x68;
    g.run({0x51, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x1b, 0x01}, 1024, r);
    checkResult(r, {0x00, 0x00, 0x04, 0, 0, 3, 3});
    CHECK_EQ(r[0], 0x00);
    CHECK_EQ(r[2], 0x04);
    CHECK_EQ(r[5], 3); // 結果の R は EOT のセクタ

    // CPU 側の $FF はマスク: 全部 $FF ならどのセクタとも一致する
    memset(g.bus.mem + 0x3000, 0xff, 1024);
    g.run({0x51, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x1b, 0x01}, 1024, r);
    checkResult(r, {0x00, 0x00, 0x08, 0, 0, 3, 3});
    CHECK_EQ(r[2], 0x08);
    // ディスク側の $FF もマスク (位置 252 は (252 + 0 + 3) = $FF)
    CHECK_EQ(g_xdf[2 * 1024 + 252], 0xff);
    g.put(0, 3)[252] = 0x12;
    g.run({0x51, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x1b, 0x01}, 1024, r);
    checkResult(r, {0x00, 0x00, 0x08, 0, 0, 3, 3});
    CHECK_EQ(r[2], 0x08);

    // 一致しなければ R + STP へ進む: セクタ 3、4 は違い、5 で一致。SN は残らない
    g.put(0, 3)[100] ^= 1;
    g.put(1, 4)[100] ^= 1;
    g.put(2, 5);
    g.run({0x51, 0x00, 0x00, 0x00, 0x03, 0x03, 0x05, 0x1b, 0x01}, 3072, r);
    checkResult(r, {0x00, 0x00, 0x08, 0, 0, 5, 3});
    CHECK_EQ(r[0], 0x00); // 一致して正常終了
    CHECK_EQ(r[2], 0x08);
    CHECK_EQ(r[5], 5);
    // 最初のセクタで一致したら、そこで終わる (残りのデータは受け取らない)
    g.put(0, 3);
    g.run({0x51, 0x00, 0x00, 0x00, 0x03, 0x03, 0x05, 0x1b, 0x01}, 3072, r);
    checkResult(r, {0x00, 0x00, 0x08, 0, 0, 3, 3});
    CHECK_EQ(r[2], 0x08);
    CHECK_EQ(r[5], 3); // 一致したセクタで止まる

    // STP = 2: セクタ 1、3、5 を比べる
    g.put(0, 1)[100] ^= 1;
    g.put(1, 3)[100] ^= 1;
    g.put(2, 5);
    g.run({0x51, 0x00, 0x00, 0x00, 0x01, 0x03, 0x05, 0x1b, 0x02}, 3072, r);
    checkResult(r, {0x00, 0x00, 0x08, 0, 0, 5, 3});
    CHECK_EQ(r[0], 0x00);
    CHECK_EQ(r[2], 0x08);
    CHECK_EQ(r[5], 5);
    // STP = 2 で EOT を飛び越す (2、4 のあと 6 > EOT = 5): 異常終了 (データシートの 21、23、25 の例)
    g.put(0, 2)[100] ^= 1;
    g.put(1, 4)[100] ^= 1;
    g.put(2, 5);
    g.run({0x51, 0x00, 0x00, 0x00, 0x02, 0x03, 0x05, 0x1b, 0x02}, 3072, r);
    checkResult(r, {0x40, 0x80, 0x04, 0, 0, 4, 3});
    CHECK_EQ(r[0], 0x40);
    CHECK_EQ(r[1], 0x80);
    CHECK_EQ(r[2], 0x04);

    // SCAN LOW OR EQUAL (ディスク <= CPU)。表 4: 等しい SH = 1 / ディスク < CPU は SH = 0、SN = 0 / ディスク > CPU は SN = 1
    g.put(0, 3);
    g.run({0x59, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x1b, 0x01}, 1024, r);
    checkResult(r, {0x00, 0x00, 0x08, 0, 0, 3, 3});
    CHECK_EQ(r[2], 0x08);
    g.put(0, 3)[100] = 0x68;
    g.run({0x59, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x1b, 0x01}, 1024, r);
    checkResult(r, {0x00, 0x00, 0x00, 0, 0, 3, 3});
    CHECK_EQ(r[0], 0x00); // ディスク < CPU は条件を満たす
    CHECK_EQ(r[2], 0x00);
    g.put(0, 3)[100] = 0x66;
    g.run({0x59, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x1b, 0x01}, 1024, r);
    checkResult(r, {0x00, 0x00, 0x04, 0, 0, 3, 3});
    CHECK_EQ(r[2], 0x04);

    // SCAN HIGH OR EQUAL (ディスク >= CPU)
    g.put(0, 3);
    g.run({0x5d, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x1b, 0x01}, 1024, r);
    checkResult(r, {0x00, 0x00, 0x08, 0, 0, 3, 3});
    CHECK_EQ(r[2], 0x08);
    g.put(0, 3)[100] = 0x66;
    g.run({0x5d, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x1b, 0x01}, 1024, r);
    checkResult(r, {0x00, 0x00, 0x00, 0, 0, 3, 3});
    CHECK_EQ(r[0], 0x00);
    CHECK_EQ(r[2], 0x00); // ディスク > CPU は条件を満たす (SH = 0、SN = 0)
    g.put(0, 3)[100] = 0x68;
    g.run({0x5d, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x1b, 0x01}, 1024, r);
    checkResult(r, {0x00, 0x00, 0x04, 0, 0, 3, 3});
    CHECK_EQ(r[2], 0x04);

    // セクタの途中の TC: 受け取ったバイトまでを比べて終わる
    g.put(0, 3)[600] ^= 1; // 後ろ半分だけ違う
    g.run({0x51, 0x00, 0x00, 0x00, 0x03, 0x03, 0x05, 0x1b, 0x01}, 512, r);
    checkResult(r, {0x00, 0x00, 0x08, 0, 0, 3, 3});
    CHECK_EQ(r[0], 0x00);
    CHECK_EQ(r[2], 0x08);
    CHECK_EQ(r[5], 3);
    g.put(0, 3)[100] ^= 1; // 前半が違う
    g.run({0x51, 0x00, 0x00, 0x00, 0x03, 0x03, 0x05, 0x1b, 0x01}, 512, r);
    checkResult(r, {0x00, 0x00, 0x04, 0, 0, 3, 3});
    CHECK_EQ(r[2], 0x04); // 受け取った範囲で違うので SN = 1
    // 一致しないセクタの終わりでの TC (EOT の手前): 条件を満たしたセクタがないので SN = 1
    g.put(0, 3)[100] ^= 1;
    g.run({0x51, 0x00, 0x00, 0x00, 0x03, 0x03, 0x05, 0x1b, 0x01}, 1024, r);
    checkResult(r, {0x00, 0x00, 0x04, 0, 0, 3, 3});
    CHECK_EQ(r[0], 0x00); // TC で終わるのは正常終了
    CHECK_EQ(r[2], 0x04);

    // 比べるのはセクタの最初のバイトから最後のバイトまで: 位置 0 だけ、位置 1023 だけが違っても一致しない
    CHECK_EQ(g_xdf[2 * 1024 + 0] != 0xff && g_xdf[2 * 1024 + 1023] != 0xff, true);
    g.put(0, 3)[0] ^= 1;
    g.run({0x51, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x1b, 0x01}, 1024, r);
    checkResult(r, {0x00, 0x00, 0x04, 0, 0, 3, 3});
    CHECK_EQ(r[2], 0x04);
    g.put(0, 3)[1023] ^= 1;
    g.run({0x51, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x1b, 0x01}, 1024, r);
    checkResult(r, {0x00, 0x00, 0x04, 0, 0, 3, 3});
    CHECK_EQ(r[2], 0x04);
    // LOW OR EQUAL / HIGH OR EQUAL も最後のバイトまで見る
    g.put(0, 3)[1023] -= 1; // ディスク > CPU
    g.run({0x59, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x1b, 0x01}, 1024, r);
    checkResult(r, {0x00, 0x00, 0x04, 0, 0, 3, 3});
    CHECK_EQ(r[2], 0x04);
    g.put(0, 3)[0] += 1; // ディスク < CPU
    g.run({0x5d, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x1b, 0x01}, 1024, r);
    checkResult(r, {0x00, 0x00, 0x04, 0, 0, 3, 3});
    CHECK_EQ(r[2], 0x04);

    // EOT のセクタまで一致しなければ、そこで終わる (データが残っていても次へ進まない。EN にもならない)。R は EOT
    g.put(0, 3)[100] ^= 1;
    g.put(1, 4)[100] ^= 1;
    g.put(2, 5)[100] ^= 1;
    g.put(3, 6);
    g.run({0x51, 0x00, 0x00, 0x00, 0x03, 0x03, 0x05, 0x1b, 0x01}, 4096, r);
    checkResult(r, {0x00, 0x00, 0x04, 0, 0, 5, 3});
    CHECK_EQ(r[0], 0x00); // EOT で終わっても正常終了の扱い
    CHECK_EQ(r[1], 0x00);
    CHECK_EQ(r[2], 0x04);
    CHECK_EQ(r[3], 0);
    CHECK_EQ(r[5], 5);
    // TC で終わるとき、結果の R は最後に比べたセクタのまま (READ DATA のように次のセクタへ進めない)
    g.put(0, 3)[100] ^= 1;
    g.run({0x51, 0x00, 0x00, 0x00, 0x03, 0x03, 0x05, 0x1b, 0x01}, 1024, r);
    checkResult(r, {0x00, 0x00, 0x04, 0, 0, 3, 3});
    CHECK_EQ(r[5], 3); // R は進まない
    CHECK_EQ(r[3], 0);
    // 一致したセクタで TC が来たときも同じ
    g.put(0, 3);
    g.run({0x51, 0x00, 0x00, 0x00, 0x03, 0x03, 0x05, 0x1b, 0x01}, 1024, r);
    checkResult(r, {0x00, 0x00, 0x08, 0, 0, 3, 3});
    CHECK_EQ(r[2], 0x08);
    CHECK_EQ(r[5], 3);
    // EOT のセクタで一致して TC のときも、C は進めない
    g.put(0, 5);
    g.run({0x51, 0x00, 0x00, 0x00, 0x05, 0x03, 0x05, 0x1b, 0x01}, 1024, r);
    checkResult(r, {0x00, 0x00, 0x08, 0, 0, 5, 3});
    CHECK_EQ(r[2], 0x08);
    CHECK_EQ(r[3], 0);
    CHECK_EQ(r[5], 5);

    // STP は 2 ちょうどのときだけ 2 つ進む。6 (下位 2 ビットが 2) や 3 は 1 つずつ
    for (uint8_t stp : {(uint8_t)0x06, (uint8_t)0x03, (uint8_t)0x00}) {
        g.put(0, 1)[100] ^= 1;
        g.put(1, 2);
        g.run({0x51, 0x00, 0x00, 0x00, 0x01, 0x03, 0x05, 0x1b, stp}, 2048, r);
        checkResult(r, {0x00, 0x00, 0x08, 0, 0, 2, 3});
        CHECK_EQ(r[0], 0x00);
        CHECK_EQ(r[2], 0x08);
        CHECK_EQ(r[5], 2);
    }

    // SCAN 以外のコマンドは、9 バイト目 (DTL) が 2 でも 1 つずつ進む: READ DATA でセクタ 3、4 を続けて読む
    startFdcDma(g.dmac, true, 0x3000, 2048);
    fdcCommand(g.fdc, {0x46, 0x00, 0x00, 0x00, 0x03, 0x03, 0x05, 0x1b, 0x02});
    g.dmac.run(g.bus);
    fdcResult(g.fdc, r, 7);
    checkResult(r, {0x00, 0x00, 0x00, 0, 0, 5, 3});
    CHECK_EQ(r[0], 0x00); // TC で正常終了
    CHECK_EQ(r[5], 5);
    CHECK_EQ(memcmp(g.bus.mem + 0x3000, g_xdf + 2 * 1024, 2048), 0);

    // MT: ヘッド 0 の EOT で一致しなければ、ヘッド 1 の R = 1 へ進む
    g.put(0, 8)[100] ^= 1;
    memcpy(g.bus.mem + 0x3400, g_xdf + 8 * 1024, 1024); // トラック 1 (C=0、H=1) の R=1
    g.run({0xd1, 0x00, 0x00, 0x00, 0x08, 0x03, 0x08, 0x1b, 0x01}, 2048, r);
    checkResult(r, {0x04, 0x00, 0x08, 0, 1, 1, 3});
    CHECK_EQ(r[0], 0x04);
    CHECK_EQ(r[2], 0x08);
    CHECK_EQ(r[4], 1);
    CHECK_EQ(r[5], 1);
    // MT でもヘッド 1 の EOT まで一致しなければ SN = 1
    g.put(0, 8)[100] ^= 1;
    memcpy(g.bus.mem + 0x3400, g_xdf + 15 * 1024, 1024);
    g.bus.mem[0x3400 + 100] ^= 1;
    g.run({0xd1, 0x04, 0x00, 0x01, 0x08, 0x03, 0x08, 0x1b, 0x01}, 1024, r);
    checkResult(r, {0x04, 0x00, 0x04, 0, 1, 8, 3});
    CHECK_EQ(r[0], 0x04);
    CHECK_EQ(r[2], 0x04); // SN = 1
}

void testMemoryToMemoryChain() {
    printf("dmac memory to memory with array chain\n");
    x68k::Fdc fdc;
    fdc.reset();
    x68k::Dmac dmac;
    dmac.reset();
    static TestBus bus(&fdc);
    memset(bus.mem, 0, sizeof bus.mem);
    for (int i = 0; i < 16; i++) bus.mem[0x100 + i] = (uint8_t)(i + 1);
    // アレイチェーン: 2 ブロック ($100 から 4 バイト、$108 から 8 バイト) を $800 から並べる
    uint8_t table[12] = {0, 0, 0x01, 0x00, 0, 4, 0, 0, 0x01, 0x08, 0, 8};
    memcpy(bus.mem + 0x40, table, sizeof table);
    dmac.write(2 * 64 + 0x00, 0xff);
    dmac.write(2 * 64 + 0x04, 0x08); // DCR: 16 ビットのポート (メモリ)
    dmac.write(2 * 64 + 0x05, 0x09); // OCR: メモリ→デバイス (DAR 側)、バイト、アレイチェーン、オートリクエスト最大
    dmac.write(2 * 64 + 0x06, 0x05); // SCR: 両方増やす
    dmacWrite32(dmac, 2, 0x14, 0x800);
    dmacWrite32(dmac, 2, 0x1c, 0x40);
    dmacWrite16(dmac, 2, 0x1a, 2);
    dmac.write(2 * 64 + 0x07, 0x80);
    dmac.run(bus);
    CHECK_EQ(dmac.active(2), false);
    CHECK_EQ(dmac.read(2 * 64) & 0x90, 0x80); // COC、エラーなし
    CHECK_EQ(bus.mem[0x800], 1);
    CHECK_EQ(bus.mem[0x803], 4);
    CHECK_EQ(bus.mem[0x804], 9);
    CHECK_EQ(bus.mem[0x80b], 16);
    CHECK_EQ(bus.mem[0x80c], 0);
    CHECK_EQ(dmac.irq(), false); // 割り込みは許可していない
    // 転送数 0 で開始するとカウントエラー
    dmac.write(1 * 64 + 0x00, 0xff);
    dmac.write(1 * 64 + 0x07, 0x88);
    CHECK_EQ(dmac.read(1 * 64 + 0x01), 0x0d);
    CHECK_EQ(dmac.irq(), true);
}

// ---------------------------------------------------------------- レビューの指摘の回帰テスト

void put16le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
void put32le(uint8_t* p, uint32_t v) {
    put16le(p, v);
    put16le(p + 2, v >> 16);
}

// D88 を作る: tracks トラック x nsec セクタ (各 256 バイト、N=1、中身は 0x40 + R)。大きさを返す
uint32_t makeD88(uint8_t* buf, uint32_t header, int tracks, int nsec) {
    uint32_t p = header;
    memset(buf, 0, header);
    for (int t = 0; t < tracks; t++) {
        put32le(buf + 0x20 + t * 4, p);
        for (int r = 1; r <= nsec; r++) {
            uint8_t* h = buf + p;
            memset(h, 0, 16);
            h[0] = (uint8_t)(t >> 1);
            h[1] = (uint8_t)(t & 1);
            h[2] = (uint8_t)r;
            h[3] = 1;
            put16le(h + 4, (uint32_t)nsec);
            put16le(h + 0x0e, 256);
            memset(h + 16, 0x40 + r, 256);
            p += 16 + 256;
        }
    }
    put32le(buf + 0x1c, p);
    return p;
}

uint8_t g_d88[0x2b0 + 4 * 4 * (16 + 256) + 64];

void testBrokenD88() {
    printf("broken d88\n");
    x68k::FloppyImage img;
    // 最後のセクタのデータがファイルの外まで続く: そのトラックはないものとする
    uint32_t size = makeD88(g_d88, 0x2b0, 2, 4);
    put16le(g_d88 + size - 256 - 16 + 0x0e, 0x2000);
    CHECK_EQ(img.load(g_d88, size, false, "a.d88"), true);
    CHECK_EQ(img.sectorCount(0), 4);
    CHECK_EQ(img.sectorCount(1), 0);
    // トラックの位置が桁あふれする値: 飛ばす (p + 16 の桁あふれで検査を抜けない)
    size = makeD88(g_d88, 0x2b0, 2, 4);
    put32le(g_d88 + 0x24, 0xfffffff8u);
    CHECK_EQ(img.load(g_d88, size, false, "a.d88"), true);
    CHECK_EQ(img.sectorCount(1), 0);
    // トラックの位置がファイルの外: 飛ばす
    put32le(g_d88 + 0x24, size + 0x100);
    CHECK_EQ(img.load(g_d88, size, false, "a.d88"), true);
    CHECK_EQ(img.sectorCount(0), 4);
    CHECK_EQ(img.sectorCount(1), 0);
    // ヘッダの大きさよりファイルが長い (後ろに余分なデータ): 受け付ける
    size = makeD88(g_d88, 0x2b0, 2, 4);
    CHECK_EQ(img.load(g_d88, size + 32, false, "a.d88"), true);
    CHECK_EQ(img.sectorCount(1), 4);

    // データの大きさが 0 のセクタ: READ DATA は実行フェーズに残らず、データのマークがないとして終わる
    size = makeD88(g_d88, 0x2b0, 1, 1);
    put16le(g_d88 + 0x2b0 + 0x0e, 0);
    size -= 256;
    put32le(g_d88 + 0x1c, size);
    CHECK_EQ(img.load(g_d88, size, false, "a.d88"), true);
    x68k::Fdc fdc;
    fdc.reset();
    fdc.insert(0, &img);
    fdcCommand(fdc, {0x46, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x1b, 0xff});
    uint8_t r[7];
    fdcResult(fdc, r, 7);
    CHECK_EQ(r[0], 0x40);
    CHECK_EQ(r[1], 0x01); // MA
    CHECK_EQ(r[2], 0x01); // MD
}

void testD88Header2a0() {
    printf("d88 with 0x2a0 header\n");
    uint32_t size = makeD88(g_d88, 0x2a0, 4, 4);
    x68k::FloppyImage img;
    CHECK_EQ(img.load(g_d88, size, false, "a.d88"), true);
    CHECK_EQ(img.sectorCount(0), 4);
    CHECK_EQ(img.sectorCount(3), 4);
    const x68k::FloppyImage::Sector* s = img.find(3, 1, 1, 2, 1);
    CHECK_EQ(s != nullptr, true);
    if (s) CHECK_EQ(img.data(s)[0], 0x42);
}

void testDimFlags() {
    printf("dim with missing tracks and overtrack = 0\n");
    // トラック 0、1、3 だけがある (2 はない)。overtrack (0xFF) は 0
    static uint8_t dim[256 + 3 * 8192];
    memset(dim, 0, sizeof dim);
    dim[0] = 0;
    dim[1 + 0] = dim[1 + 1] = dim[1 + 3] = 1;
    memset(dim + 256, 0xa0, 8192);
    memset(dim + 256 + 8192, 0xa1, 8192);
    memset(dim + 256 + 2 * 8192, 0xa3, 8192);
    x68k::FloppyImage img;
    CHECK_EQ(img.load(dim, sizeof dim, false, "a.dim"), true);
    CHECK_EQ(img.sectorCount(2), 0);
    const x68k::FloppyImage::Sector* s = img.find(3, 1, 1, 1, 3);
    CHECK_EQ(s != nullptr, true);
    if (s) CHECK_EQ(img.data(s)[0], 0xa3);
}

// DMA の転送が MFP に書くと syncDevices() → DMA に戻る。再入せずに最後まで転送できること
void testDmaReentry() {
    printf("dma reentry through machine\n");
    static x68k::Machine m;
    static uint8_t ipl[x68k::Machine::IPL_SIZE];
    memset(ipl, 0xff, sizeof ipl);
    uint8_t* r = ipl + 0x10000;
    const uint8_t vec[] = {0x00, 0x00, 0x20, 0x00, 0x00, 0xff, 0x00, 0x10, 0x60, 0xfe}; // SSP、PC、bra.s *
    memcpy(r, vec, 8);
    memcpy(r + 0x10, vec + 8, 2);
    CHECK_EQ(m.setIplRom(ipl, sizeof ipl), true);
    m.powerOn();
    const int fc = m68k::FC_SUPER_DATA;
    const uint32_t b = 0xe84080; // チャネル 2
    m.write8(b + 0x00, 0xff, fc);
    m.write8(b + 0x04, 0x08, fc); // 16 ビットのポート
    m.write8(b + 0x05, 0x01, fc); // メモリ → デバイス、バイト、オートリクエスト最大
    m.write8(b + 0x06, 0x04, fc); // メモリ側は増やす、デバイス側は固定
    m.write8(b + 0x0a, 0x10, fc); // MTC = $1000
    m.write8(b + 0x0b, 0x00, fc);
    for (int i = 0; i < 4; i++) m.write8(b + 0x0c + i, (uint8_t)(0x00003000 >> (24 - 8 * i)), fc);
    for (int i = 0; i < 4; i++) m.write8(b + 0x14 + i, (uint8_t)(0x00e8802d >> (24 - 8 * i)), fc); // MFP の UDR
    m.write8(b + 0x07, 0x80, fc);
    m.run(2000);
    CHECK_EQ(m.read8(b + 0x00, fc) & 0x98, 0x80); // COC、エラーなし、ACT なし
    CHECK_EQ(m.read8(b + 0x0a, fc), 0);
    CHECK_EQ(m.read8(b + 0x0b, fc), 0);
}

// メモリからメモリ / ポートへ 1 ブロック転送するチャネル 2 の設定
void startMemoryDma(x68k::Dmac& d, uint8_t dcr, uint8_t ocr, uint8_t scr, uint32_t mar, uint32_t dar, uint32_t count) {
    d.write(2 * 64 + 0x00, 0xff);
    d.write(2 * 64 + 0x04, dcr);
    d.write(2 * 64 + 0x05, ocr);
    d.write(2 * 64 + 0x06, scr);
    dmacWrite16(d, 2, 0x0a, count);
    dmacWrite32(d, 2, 0x0c, mar);
    dmacWrite32(d, 2, 0x14, dar);
    d.write(2 * 64 + 0x07, 0x80);
}

void testWordTransfer() {
    printf("dmac word and long transfers\n");
    x68k::Fdc fdc;
    fdc.reset();
    x68k::Dmac dmac;
    dmac.reset();
    static TestBus bus(&fdc);
    bus.clear();
    const uint8_t src[8] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
    memcpy(bus.mem + 0x100, src, 8);

    // 16 ビットのポートで、アドレス固定のポートへワードを 2 つ: 12 34 56 78 の順に $F00000、$F00001 へ
    startMemoryDma(dmac, 0x08, 0x11, 0x04, 0x100, 0xf00000, 2);
    dmac.run(bus);
    CHECK_EQ(dmac.read(2 * 64 + 0x01), 0);
    CHECK_EQ(bus.logLen, 4);
    CHECK_EQ(bus.log[0], 0xf0000012);
    CHECK_EQ(bus.log[1], 0xf0000134);
    CHECK_EQ(bus.log[2], 0xf0000056);
    CHECK_EQ(bus.log[3], 0xf0000178);
    CHECK_EQ(dmacRead32(dmac, 2, 0x0c), 0x104);
    CHECK_EQ(dmacRead32(dmac, 2, 0x14), 0xf00000);

    // 8 ビットのポートでロングワード 1 つ、デバイス側は増やす: $F00001、3、5、7 の順。DAR は 8 進む
    bus.logLen = 0;
    startMemoryDma(dmac, 0x00, 0x21, 0x05, 0x100, 0xf00001, 1);
    dmac.run(bus);
    CHECK_EQ(bus.logLen, 4);
    CHECK_EQ(bus.log[0], 0xf0000112);
    CHECK_EQ(bus.log[3], 0xf0000778u);
    CHECK_EQ(dmacRead32(dmac, 2, 0x14), 0xf00009);

    // メモリ側を減らすワード転送: オペランドの中は昇順のまま、次のオペランドは 2 つ前
    bus.logLen = 0;
    startMemoryDma(dmac, 0x08, 0x11, 0x08, 0x102, 0xf00000, 2);
    dmac.run(bus);
    CHECK_EQ(bus.logLen, 4);
    CHECK_EQ(bus.log[0], 0xf0000056);
    CHECK_EQ(bus.log[1], 0xf0000178);
    CHECK_EQ(bus.log[2], 0xf0000012);
    CHECK_EQ(bus.log[3], 0xf0000134);

    // 奇数番地のワード転送はアドレスエラー (メモリ側 5、16 ビットポートのデバイス側 6)
    startMemoryDma(dmac, 0x08, 0x11, 0x04, 0x101, 0xf00000, 1);
    dmac.run(bus);
    CHECK_EQ(dmac.read(2 * 64 + 0x01), 0x05);
    startMemoryDma(dmac, 0x08, 0x11, 0x04, 0x100, 0xf00001, 1);
    dmac.run(bus);
    CHECK_EQ(dmac.read(2 * 64 + 0x01), 0x06);
    // 奇数番地のチェーンの表はアドレスエラー (7)
    dmac.write(2 * 64 + 0x00, 0xff);
    CHECK_EQ(dmac.read(2 * 64 + 0x01), 0); // ERR を消すと CER も 0
    dmac.write(2 * 64 + 0x05, 0x09);
    dmacWrite32(dmac, 2, 0x1c, 0x41);
    dmacWrite16(dmac, 2, 0x1a, 1);
    dmac.write(2 * 64 + 0x07, 0x80);
    dmac.run(bus);
    CHECK_EQ(dmac.read(2 * 64 + 0x01), 0x07);
    // アレイチェーンのブロック数が 0 はカウントエラー (0x0F)
    dmac.write(2 * 64 + 0x00, 0xff);
    dmacWrite32(dmac, 2, 0x1c, 0x40);
    dmacWrite16(dmac, 2, 0x1a, 0);
    dmac.write(2 * 64 + 0x07, 0x80);
    dmac.run(bus);
    CHECK_EQ(dmac.read(2 * 64 + 0x01), 0x0f);
    // ユーザの FC (MFC = 1) では保護された領域 ($8000 以上) がバスエラー
    dmac.write(2 * 64 + 0x29, 0x01);
    startMemoryDma(dmac, 0x08, 0x01, 0x04, 0x9000, 0xf00000, 1);
    dmac.run(bus);
    CHECK_EQ(dmac.read(2 * 64 + 0x01), 0x09);
}

void testBusErrorDuringWrite() {
    printf("dma bus error during fdc write\n");
    static uint8_t copy[sizeof g_xdf];
    memcpy(copy, g_xdf, sizeof copy);
    x68k::FloppyImage img;
    img.load(copy, sizeof copy, false, "a.xdf");
    x68k::Fdc fdc;
    fdc.reset();
    fdc.insert(0, &img);
    x68k::Dmac dmac;
    dmac.reset();
    static TestBus bus(&fdc);
    bus.clear();
    for (int i = 0; i < 1024; i++) bus.mem[0x3000 + i] = 0x5a;
    bus.busErrorAt = 0x3000 + 100; // セクタの途中でメモリが読めない
    startFdcDma(dmac, false, 0x3000, 1024);
    fdcCommand(fdc, {0x45, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x1b, 0xff});
    dmac.run(bus);
    CHECK_EQ(bus.done[0], 0); // 正常な TC としては知らせない
    CHECK_EQ(bus.errors[0], 1);
    CHECK_EQ(dmac.read(0x01), 0x09);
    CHECK_EQ(dmac.ack(), 0x65); // EIV
    uint8_t r[7];
    fdcResult(fdc, r, 7);
    CHECK_EQ(r[0], 0x40);
    CHECK_EQ(r[1], 0x10); // オーバーラン
    CHECK_EQ(r[5], 3);    // 今のセクタを指す
    CHECK_EQ(img.dirty(), false);
    CHECK_EQ(memcmp(copy + 2 * 1024, g_xdf + 2 * 1024, 1024), 0); // セクタは書き換わらない
}

// 書き込みの途中でディスクを入れ替えても、前のディスクのセクタ (取り出したら解放されることがある) を指したまま
// 新しいディスクに書かない (設定メニューの入れ替え)
void testSwapDuringWrite() {
    printf("disk swap during write\n");
    static uint8_t a[sizeof g_xdf], b[sizeof g_xdf];
    memcpy(a, g_xdf, sizeof a);
    memcpy(b, g_xdf, sizeof b);
    x68k::FloppyImage* imgA = new x68k::FloppyImage;
    imgA->load(a, sizeof a, false, "a.xdf");
    x68k::FloppyImage imgB;
    imgB.load(b, sizeof b, false, "b.xdf");
    x68k::Fdc fdc;
    fdc.reset();
    fdc.insert(0, imgA);
    fdcCommand(fdc, {0x45, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x1b, 0xff});
    for (int i = 0; i < 100; i++) fdc.write(3, 0x77);
    fdc.eject(0);
    delete imgA; // 取り出したイメージを解放する (ASan なら、解放したセクタの表を読むと止まる)
    fdc.insert(0, &imgB);
    for (int i = 100; i < 1024; i++) fdc.write(3, 0x77);
    CHECK_EQ(memcmp(a, g_xdf, sizeof a), 0);
    CHECK_EQ(memcmp(b, g_xdf, sizeof b), 0);
    CHECK_EQ(imgB.dirty(), false);
}

void testSoftwareAbort() {
    printf("dma software abort\n");
    x68k::FloppyImage img;
    img.load(g_xdf, sizeof g_xdf, false, "a.xdf");
    x68k::Fdc fdc;
    fdc.reset();
    fdc.insert(0, &img);
    x68k::Dmac dmac;
    dmac.reset();
    static TestBus bus(&fdc);
    bus.clear();
    startFdcDma(dmac, true, 0x1000, 2048);
    fdcCommand(fdc, {0x46, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x1b, 0xff});
    CHECK_EQ(fdc.read(1), 0x10);
    dmac.write(0x07, 0x10); // SAB
    CHECK_EQ(dmac.read(0x01), 0x11);
    dmac.notify(bus);
    CHECK_EQ(bus.errors[0], 1);
    uint8_t r[7];
    fdcResult(fdc, r, 7); // 実行フェーズに残らない
    CHECK_EQ(r[0], 0x40);
    CHECK_EQ(r[1], 0x10);
    // 次のコマンドを受け付ける
    fdcCommand(fdc, {0x08});
    fdcResult(fdc, r, 1);
    CHECK_EQ(r[0], 0x80);
}

void testChannelPriority() {
    printf("dmac channel priority\n");
    x68k::Fdc fdc;
    fdc.reset();
    x68k::Dmac dmac;
    dmac.reset();
    static TestBus bus(&fdc);
    bus.clear();
    // チャネル 0 と 1 を転送数 0 で開始してカウントエラーにする (どちらも割り込みを要求する)
    for (int ch = 0; ch < 2; ch++) {
        dmac.write((uint32_t)(ch * 64 + 0x00), 0xff);
        dmac.write((uint32_t)(ch * 64 + 0x27), (uint8_t)(0x70 + ch)); // EIV
        dmac.write((uint32_t)(ch * 64 + 0x2d), ch == 0 ? 3 : 0);      // CPR: チャネル 1 が高い
        dmac.write((uint32_t)(ch * 64 + 0x07), 0x88);
    }
    CHECK_EQ(dmac.ack(), 0x71);
    CHECK_EQ(dmac.ack(), 0x71); // 消すまでは高いほうが応答し続ける
    dmac.write(1 * 64 + 0x2d, 3);
    dmac.write(0 * 64 + 0x2d, 0);
    CHECK_EQ(dmac.ack(), 0x70);
    // 転送の順: チャネル 3 (CPR = 0) がチャネル 2 (CPR = 1) より先に転送する
    bus.mem[0x100] = 0x22;
    bus.mem[0x200] = 0x33;
    dmac.write(2 * 64 + 0x2d, 1);
    dmac.write(3 * 64 + 0x2d, 0);
    for (int ch = 2; ch < 4; ch++) {
        dmac.write((uint32_t)(ch * 64 + 0x00), 0xff);
        dmac.write((uint32_t)(ch * 64 + 0x04), 0x08);
        dmac.write((uint32_t)(ch * 64 + 0x05), 0x01);
        dmac.write((uint32_t)(ch * 64 + 0x06), 0x05);
        dmacWrite16(dmac, ch, 0x0a, 1);
        dmacWrite32(dmac, ch, 0x0c, ch == 2 ? 0x100 : 0x200);
        dmacWrite32(dmac, ch, 0x14, ch == 2 ? 0xf00000 : 0xf00008);
        dmac.write((uint32_t)(ch * 64 + 0x07), 0x80);
    }
    dmac.run(bus);
    CHECK_EQ(bus.logLen, 2);
    CHECK_EQ(bus.log[0], 0xf0000833u);
    CHECK_EQ(bus.log[1], 0xf0000022u);
}

void testEjectRequest() {
    printf("eject request\n");
    x68k::FloppyImage img;
    img.load(g_xdf, sizeof g_xdf, false, "a.xdf");
    x68k::Fdc fdc;
    fdc.reset();
    // ディスクがないドライブへの排出は要求にならない
    fdc.write(5, 0x21);
    fdc.write(5, 0x00);
    CHECK_EQ(fdc.takeEjectRequest(0), false);
    // ディスクがあれば要求になる。入れ直すと消える
    fdc.insert(0, &img);
    fdc.write(5, 0x21);
    fdc.write(5, 0x00);
    fdc.insert(0, &img);
    CHECK_EQ(fdc.takeEjectRequest(0), false);
    fdc.write(5, 0x21);
    fdc.write(5, 0x00);
    CHECK_EQ(fdc.takeEjectRequest(0), true);
    CHECK_EQ(fdc.takeEjectRequest(0), false);
}

void testDeletedData() {
    printf("deleted data, sk and write marks\n");
    // R=2 がデリーテッドデータ
    uint32_t size = makeD88(g_d88, 0x2b0, 1, 4);
    g_d88[0x2b0 + (16 + 256) + 7] = 0x10;
    x68k::FloppyImage img;
    CHECK_EQ(img.load(g_d88, size, false, "a.d88"), true);
    x68k::Fdc fdc;
    fdc.reset();
    fdc.insert(0, &img);
    x68k::Dmac dmac;
    dmac.reset();
    static TestBus bus(&fdc);
    bus.clear();
    uint8_t r[7];
    // SK = 1: R=1 から 3 を読むと R=2 を飛ばし、R=1 と R=3 だけが転送される
    startFdcDma(dmac, true, 0x1000, 512);
    fdcCommand(fdc, {0x66, 0x00, 0x00, 0x00, 0x01, 0x01, 0x03, 0x1b, 0xff});
    dmac.run(bus);
    CHECK_EQ(bus.mem[0x1000], 0x41);
    CHECK_EQ(bus.mem[0x1100], 0x43);
    fdcResult(fdc, r, 7);
    CHECK_EQ(r[0] & 0xc0, 0x00);
    CHECK_EQ(r[2], 0x40); // CM
    CHECK_EQ(r[3], 1);    // EOT (R=3) で TC が来たので、次のシリンダの R=1 を指す
    CHECK_EQ(r[5], 1);
    // SK = 0: R=2 を読んで、そのセクタで異常終了する
    bus.clear();
    startFdcDma(dmac, true, 0x1000, 512);
    fdcCommand(fdc, {0x46, 0x00, 0x00, 0x00, 0x02, 0x01, 0x03, 0x1b, 0xff});
    dmac.run(bus);
    CHECK_EQ(bus.mem[0x1000], 0x42);
    CHECK_EQ(bus.mem[0x1100], 0x00);
    fdcResult(fdc, r, 7);
    CHECK_EQ(r[0], 0x40);
    CHECK_EQ(r[2], 0x40);
    // WRITE DELETED DATA で R=3 をデリーテッドデータにする (D88 の見出しも変わる)
    startFdcDma(dmac, false, 0x2000, 256);
    fdcCommand(fdc, {0x49, 0x00, 0x00, 0x00, 0x03, 0x01, 0x03, 0x1b, 0xff});
    dmac.run(bus);
    fdcResult(fdc, r, 7);
    CHECK_EQ(r[0] & 0xc0, 0x00);
    const x68k::FloppyImage::Sector* s3 = img.find(0, 0, 0, 3, 1);
    CHECK_EQ(s3 && s3->deleted, true);
    CHECK_EQ(g_d88[0x2b0 + 2 * (16 + 256) + 7], 0x10);
    // WRITE DATA で R=2 を通常のデータに戻す (CRC エラーの状態も消える)
    g_d88[0x2b0 + (16 + 256) + 8] = 0xb0;
    img.load(g_d88, size, false, "a.d88");
    startFdcDma(dmac, false, 0x2000, 256);
    fdcCommand(fdc, {0x45, 0x00, 0x00, 0x00, 0x02, 0x01, 0x02, 0x1b, 0xff});
    dmac.run(bus);
    fdcResult(fdc, r, 7);
    const x68k::FloppyImage::Sector* s2 = img.find(0, 0, 0, 2, 1);
    CHECK_EQ(s2 && !s2->deleted && s2->status == 0, true);
    CHECK_EQ(g_d88[0x2b0 + (16 + 256) + 7], 0x00);
    CHECK_EQ(g_d88[0x2b0 + (16 + 256) + 8], 0x00);
}

// SCAN 系とデータのマーク、CRC エラー (uPD765A のデータシートの「SCAN COMMANDS」: デリーテッドデータのセクタは、
// SK = 0 ならシリンダの最後のセクタとして扱い CM = 1 で終わる。SK = 1 なら飛ばして CM = 1)
void testScanDeletedAndCrc() {
    printf("scan with deleted data and crc errors\n");
    // 256 バイトのセクタが 4 つ (中身は $40 + R)。R=2 をデリーテッドデータにする
    uint32_t size = makeD88(g_d88, 0x2b0, 2, 4);
    g_d88[0x2b0 + 1 * (16 + 256) + 7] = 0x10;
    static ScanRig g(g_d88, size, "a.d88");
    uint8_t r[7]; // リザルトフェーズの 7 バイト
    uint8_t* m = g.bus.mem + 0x3000;
    // SK = 0: R=1 は一致せず、R=2 (デリーテッドデータ) で終わる。R=3 へは進まない
    memset(m, 0x00, 256);
    memset(m + 256, 0x00, 256);
    memset(m + 512, 0x43, 256);
    g.run({0x51, 0x00, 0x00, 0x00, 0x01, 0x01, 0x04, 0x1b, 0x01}, 768, r);
    checkResult(r, {0x40, 0x00, 0x44, 0, 0, 2, 1});
    CHECK_EQ(r[2], 0x44); // CM = 1
    CHECK_EQ(r[2], 0x44);
    CHECK_EQ(r[5], 2); // デリーテッドデータのセクタで止まる
    // SK = 1: R=2 を飛ばして R=3 と比べる。CM は立つ
    memset(m, 0x00, 256);
    memset(m + 256, 0x43, 256);
    g.run({0x71, 0x00, 0x00, 0x00, 0x01, 0x01, 0x04, 0x1b, 0x01}, 512, r);
    checkResult(r, {0x00, 0x00, 0x48, 0, 0, 3, 1});
    CHECK_EQ(r[0], 0x00); // R=3 で一致して正常終了
    CHECK_EQ(r[2], 0x48);
    CHECK_EQ(r[5], 3); // 飛ばした先のセクタ

    // データの CRC エラーのセクタ: DE と DD で異常終了
    size = makeD88(g_d88, 0x2b0, 2, 4);
    g_d88[0x2b0 + 0 * (16 + 256) + 8] = 0xb0;
    static ScanRig e(g_d88, size, "b.d88");
    memset(e.bus.mem + 0x3000, 0x41, 256);
    e.run({0x51, 0x00, 0x00, 0x00, 0x01, 0x01, 0x04, 0x1b, 0x01}, 256, r);
    checkResult(r, {0x40, 0x20, 0x28, 0, 0, 1, 1});
    CHECK_EQ(r[0], 0x40); // 異常終了
    CHECK_EQ(r[1], 0x20); // DE
    // CRC はセクタのデータのあとにあるので、比べた結果 (ここでは一致で SH = 1) も残る。データシートには、CRC エラーの
    // ときの SH / SN の決まりはない
    CHECK_EQ(r[2], 0x28);
    CHECK_EQ(r[5], 1);
    // 一致しないデータなら SN = 1 と DD
    memset(e.bus.mem + 0x3000, 0x00, 256);
    e.run({0x51, 0x00, 0x00, 0x00, 0x01, 0x01, 0x04, 0x1b, 0x01}, 256, r);
    checkResult(r, {0x40, 0x20, 0x24, 0, 0, 1, 1});
    CHECK_EQ(r[0], 0x40); // 異常終了
    CHECK_EQ(r[1], 0x20); // DE
    CHECK_EQ(r[2], 0x24);

    // N = 0: SCAN 系の 9 バイト目は DTL ではなく STP なので、1 セクタは 128 バイト。ID の N も 0 のセクタを使う
    size = makeD88(g_d88, 0x2b0, 2, 4);
    for (int k = 0; k < 4; k++) g_d88[0x2b0 + k * (16 + 256) + 3] = 0;
    static ScanRig z(g_d88, size, "c.d88");
    m = z.bus.mem + 0x3000;
    memset(m, 0x00, 512);
    memset(m, 0x41, 128);
    z.run({0x51, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04, 0x1b, 0x01}, 128, r);
    checkResult(r, {0x00, 0x00, 0x08, 0, 0, 1, 0});
    CHECK_EQ(r[0], 0x00);
    CHECK_EQ(r[2], 0x08);
    CHECK_EQ(r[5], 1);
    CHECK_EQ(r[6], 0);
    // 128 バイトごとに次のセクタへ進む: R=1 は違い、R=2 で一致
    memset(m, 0x00, 128);
    memset(m + 128, 0x42, 128);
    z.run({0x51, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04, 0x1b, 0x01}, 256, r);
    checkResult(r, {0x00, 0x00, 0x08, 0, 0, 2, 0});
    CHECK_EQ(r[0], 0x00);
    CHECK_EQ(r[2], 0x08);
    CHECK_EQ(r[5], 2);

    // N = 8: データシートの表にない。1 セクタ 128 バイトとして扱う (下位 3 ビット)。ID の N も 8 のセクタ 3 つを
    // 768 バイト分の DMA で読むと、128 バイトずつ 3 セクタで EOT を過ぎ、TC が来ないので EN
    size = makeD88(g_d88, 0x2b0, 2, 4);
    for (int k = 0; k < 4; k++) g_d88[0x2b0 + k * (16 + 256) + 3] = 8;
    static ScanRig n8(g_d88, size, "e.d88");
    memset(n8.bus.mem + 0x3000, 0xee, 1024);
    startFdcDma(n8.dmac, true, 0x3000, 768);
    fdcCommand(n8.fdc, {0x46, 0x00, 0x00, 0x00, 0x01, 0x08, 0x03, 0x1b, 0xff});
    n8.dmac.run(n8.bus);
    fdcResult(n8.fdc, r, 7);
    checkResult(r, {0x40, 0x80, 0x00, 0, 0, 3, 8});
    CHECK_EQ(r[0], 0x40);
    CHECK_EQ(r[1], 0x80);
    CHECK_EQ(r[2], 0x00);
    CHECK_EQ(r[5], 3);
    CHECK_EQ(n8.bus.mem[0x3000], 0x41);
    CHECK_EQ(n8.bus.mem[0x3000 + 127], 0x41);
    CHECK_EQ(n8.bus.mem[0x3000 + 128], 0x42);
    CHECK_EQ(n8.bus.mem[0x3000 + 256], 0x43);
    CHECK_EQ(n8.bus.mem[0x3000 + 383], 0x43);
    CHECK_EQ(n8.bus.mem[0x3000 + 384], 0xee);
    // WRITE DATA も 128 バイトずつ: 256 バイトを書くと R=1 と R=2 の前半の 128 バイトが変わる
    memset(n8.bus.mem + 0x3000, 0x11, 128);
    memset(n8.bus.mem + 0x3000 + 128, 0x22, 128);
    startFdcDma(n8.dmac, false, 0x3000, 256);
    fdcCommand(n8.fdc, {0x45, 0x00, 0x00, 0x00, 0x01, 0x08, 0x03, 0x1b, 0xff});
    n8.dmac.run(n8.bus);
    fdcResult(n8.fdc, r, 7);
    checkResult(r, {0x00, 0x00, 0x00, 0, 0, 3, 8});
    CHECK_EQ(r[0], 0x00);
    CHECK_EQ(r[5], 3);
    CHECK_EQ(g_d88[0x2b0 + 16 + 0], 0x11);
    CHECK_EQ(g_d88[0x2b0 + 16 + 127], 0x11);
    CHECK_EQ(g_d88[0x2b0 + 16 + 128], 0x41);
    CHECK_EQ(g_d88[0x2b0 + (16 + 256) + 16 + 0], 0x22);
    CHECK_EQ(g_d88[0x2b0 + (16 + 256) + 16 + 128], 0x42);
    n8.fdc.eject(0);
    // READ DIAGNOSTIC で N = 8、ID の N は 9: N が合わないので ND。128 バイトずつ読んで EOT を過ぎ、EN
    size = makeD88(g_d88, 0x2b0, 2, 4);
    for (int k = 0; k < 4; k++) g_d88[0x2b0 + k * (16 + 256) + 3] = 9;
    static ScanRig n9(g_d88, size, "f.d88");
    startFdcDma(n9.dmac, true, 0x3000, 768);
    fdcCommand(n9.fdc, {0x42, 0x00, 0x00, 0x00, 0x01, 0x08, 0x03, 0x1b, 0xff});
    n9.dmac.run(n9.bus);
    fdcResult(n9.fdc, r, 7);
    checkResult(r, {0x40, 0x84, 0x00, 0, 0, 3, 8});
    CHECK_EQ(r[0], 0x40);
    CHECK_EQ(r[1], 0x84);
    CHECK_EQ(r[2], 0x00);
    CHECK_EQ(r[5], 3);
    n9.fdc.eject(0);

    // 書き込み禁止のディスクでも SCAN 系は動く (ディスクには書かないので、NW にならない)
    size = makeD88(g_d88, 0x2b0, 2, 4);
    x68k::FloppyImage ro;
    ro.load(g_d88, size, true, "d.d88");
    z.fdc.insert(0, &ro);
    memset(m, 0x41, 256);
    z.run({0x51, 0x00, 0x00, 0x00, 0x01, 0x01, 0x04, 0x1b, 0x01}, 256, r);
    checkResult(r, {0x00, 0x00, 0x08, 0, 0, 1, 1});
    CHECK_EQ(r[0], 0x00); // 正常終了
    CHECK_EQ(r[1], 0x00);
    CHECK_EQ(r[2], 0x08); // 一致 (SH = 1)
    CHECK_EQ(r[5], 1);
    m[10] = 0x40;
    z.run({0x51, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x1b, 0x01}, 256, r);
    checkResult(r, {0x00, 0x00, 0x04, 0, 0, 1, 1});
    CHECK_EQ(r[0], 0x00);
    CHECK_EQ(r[1], 0x00);
    CHECK_EQ(r[2], 0x04);
    z.fdc.eject(0);
}

void testMultiTrackResult() {
    printf("mt result on head 1 and misc\n");
    x68k::FloppyImage img;
    img.load(g_xdf, sizeof g_xdf, false, "a.xdf");
    x68k::Fdc fdc;
    fdc.reset();
    fdc.insert(0, &img);
    fdc.insert(1, &img);
    x68k::Dmac dmac;
    dmac.reset();
    static TestBus bus(&fdc);
    bus.clear();
    uint8_t r[7];
    // MT、ヘッド 1 の EOT で TC: 次は C+1、H=0、R=1
    startFdcDma(dmac, true, 0x1000, 1024);
    fdcCommand(fdc, {0xc6, 0x04, 0x00, 0x01, 0x08, 0x03, 0x08, 0x1b, 0xff});
    dmac.run(bus);
    fdcResult(fdc, r, 7);
    CHECK_EQ(r[0], 0x04);
    CHECK_EQ(r[3], 1);
    CHECK_EQ(r[4], 0);
    CHECK_EQ(r[5], 1);
    // MT でヘッド 0 からヘッド 1 へ移ったら、ST0 の HD も 1
    startFdcDma(dmac, true, 0x1000, 2048);
    fdcCommand(fdc, {0xc6, 0x00, 0x00, 0x00, 0x08, 0x03, 0x08, 0x1b, 0xff});
    dmac.run(bus);
    fdcResult(fdc, r, 7);
    CHECK_EQ(r[0], 0x04);
    CHECK_EQ(r[4], 1);
    CHECK_EQ(r[5], 2);
    // READ ID が読めないときは、前のコマンドの ID を返さない
    fdc.eject(1);
    fdcCommand(fdc, {0x4a, 0x05});
    fdcResult(fdc, r, 7);
    CHECK_EQ(r[0], 0x4d);
    CHECK_EQ(r[3], 0);
    CHECK_EQ(r[4], 1);
    CHECK_EQ(r[5], 0);
    // 2 台のシークの終わりは、SENSE INTERRUPT STATUS で 1 台ずつ返し、残っている間は割り込みが続く
    fdc.insert(1, &img);
    fdcCommand(fdc, {0x0f, 0x00, 0x05});
    fdcCommand(fdc, {0x0f, 0x01, 0x07});
    fdcCommand(fdc, {0x08});
    fdcResult(fdc, r, 2);
    CHECK_EQ(r[0], 0x20);
    CHECK_EQ(r[1], 5);
    CHECK_EQ(fdc.irq(), true);
    fdcCommand(fdc, {0x08});
    fdcResult(fdc, r, 2);
    CHECK_EQ(r[0], 0x21);
    CHECK_EQ(r[1], 7);
    CHECK_EQ(fdc.irq(), false);
}

// ---------------------------------------------------------------- 再レビューの指摘の回帰テスト

void testSkipAcrossHeads() {
    printf("sk with mt across heads\n");
    // トラック 0 の R=4 (EOT) がデリーテッドデータ。トラック 1 の R=1 は先頭を 0x99 にする
    uint32_t size = makeD88(g_d88, 0x2b0, 2, 4);
    g_d88[0x2b0 + 3 * (16 + 256) + 7] = 0x10;
    g_d88[0x2b0 + 4 * (16 + 256) + 16] = 0x99;
    x68k::FloppyImage img;
    CHECK_EQ(img.load(g_d88, size, false, "a.d88"), true);
    x68k::Fdc fdc;
    fdc.reset();
    fdc.insert(0, &img);
    x68k::Dmac dmac;
    dmac.reset();
    static TestBus bus(&fdc);
    bus.clear();
    startFdcDma(dmac, true, 0x1000, 512);
    fdcCommand(fdc, {0xe6, 0x00, 0x00, 0x00, 0x03, 0x01, 0x04, 0x1b, 0xff}); // MT、SK、R=3 から
    dmac.run(bus);
    CHECK_EQ(bus.mem[0x1000], 0x43);
    CHECK_EQ(bus.mem[0x1100], 0x99); // R=4 を飛ばしてヘッド 1 の R=1
    uint8_t r[7];
    fdcResult(fdc, r, 7);
    CHECK_EQ(r[0], 0x04);
    CHECK_EQ(r[2], 0x40);
    CHECK_EQ(r[4], 1);
    CHECK_EQ(r[5], 2);

    // SK = 0 でデリーテッドデータの R=4 をちょうど読み終えたところで TC: 結果は R=4 を指したまま
    bus.clear();
    startFdcDma(dmac, true, 0x1000, 256);
    fdcCommand(fdc, {0x46, 0x00, 0x00, 0x00, 0x04, 0x01, 0x04, 0x1b, 0xff});
    dmac.run(bus);
    fdcResult(fdc, r, 7);
    CHECK_EQ(r[0], 0x40);
    CHECK_EQ(r[2], 0x40);
    CHECK_EQ(r[3], 0);
    CHECK_EQ(r[5], 4);
}

void testD88Header2a0WithoutTrack0() {
    printf("d88 with 0x2a0 header and no track 0\n");
    uint32_t size = makeD88(g_d88, 0x2a0, 2, 4);
    // トラック 0 を消し、トラック 1 のデータを 0x2A0 からに詰める
    memmove(g_d88 + 0x2a0, g_d88 + 0x2a0 + 4 * (16 + 256), 4 * (16 + 256));
    size -= 4 * (16 + 256);
    put32le(g_d88 + 0x20, 0);
    put32le(g_d88 + 0x24, 0x2a0);
    put32le(g_d88 + 0x1c, size);
    x68k::FloppyImage img;
    CHECK_EQ(img.load(g_d88, size, false, "a.d88"), true);
    CHECK_EQ(img.sectorCount(0), 0);
    CHECK_EQ(img.sectorCount(1), 4);
}

void testDmacStartModes() {
    printf("dmac continue, reqg = 11 and chain with cnt\n");
    x68k::Fdc fdc;
    fdc.reset();
    x68k::Dmac dmac;
    dmac.reset();
    static TestBus bus(&fdc);
    bus.clear();
    // コンティニューの次のブロックの転送数が 0: カウントエラー (0x0D)。1 つ目のブロックの終わりの DONE は出る
    // (dmac.cpp の runChannel() の注釈。DONE を出してからエラーにするのは推定)
    dmac.write(2 * 64 + 0x00, 0xff);
    dmac.write(2 * 64 + 0x04, 0x08);
    dmac.write(2 * 64 + 0x05, 0x01);
    dmac.write(2 * 64 + 0x06, 0x04);
    dmacWrite16(dmac, 2, 0x0a, 1);
    dmacWrite32(dmac, 2, 0x0c, 0x100);
    dmacWrite32(dmac, 2, 0x14, 0xf00000);
    dmacWrite16(dmac, 2, 0x1a, 0);
    dmac.write(2 * 64 + 0x07, 0xc0); // STR + CNT
    dmac.run(bus);
    CHECK_EQ(dmac.read(2 * 64 + 0x01), 0x0d);
    CHECK_EQ(bus.done[2], 1);
    CHECK_EQ(bus.errors[2], 1);

    // コンティニューの次のブロックがユーザの FC で、途中 ($8000) からバスエラーになる (オートリクエスト、メモリからデバイスへ)。
    // DONE は 1 つ目のブロックの終わりで 1 回出て、エラーは dmaError で知らせる
    bus.clear();
    for (int i = 0; i < 4; i++) bus.mem[0x100 + i] = (uint8_t)(0x11 * (i + 1));
    for (int i = 0; i < 4; i++) bus.mem[0x7ffe + i] = (uint8_t)(0xa0 + i);
    dmac.write(2 * 64 + 0x00, 0xff);
    dmac.write(2 * 64 + 0x04, 0x08);
    dmac.write(2 * 64 + 0x05, 0x01);
    dmac.write(2 * 64 + 0x06, 0x04);
    dmacWrite16(dmac, 2, 0x0a, 2);
    dmacWrite32(dmac, 2, 0x0c, 0x100);
    dmacWrite32(dmac, 2, 0x14, 0xf00000);
    dmac.write(2 * 64 + 0x29, m68k::FC_SUPER_DATA);
    dmacWrite32(dmac, 2, 0x1c, 0x7ffe);
    dmacWrite16(dmac, 2, 0x1a, 4);
    dmac.write(2 * 64 + 0x39, m68k::FC_USER_DATA);
    dmac.write(2 * 64 + 0x07, 0xc0); // STR + CNT
    dmac.run(bus);
    CHECK_EQ(dmac.active(2), false);
    CHECK_EQ(dmac.read(2 * 64 + 0x00) & 0xf8, 0xd0); // COC、BTC、ERR
    CHECK_EQ(dmac.read(2 * 64 + 0x01), 0x09);        // メモリのバスエラー
    CHECK_EQ(dmacRead32(dmac, 2, 0x0c), 0x8000);
    CHECK_EQ(dmacRead16(dmac, 2, 0x0a), 2);
    CHECK_EQ(bus.logLen, 4); // $100、$101、$7FFE、$7FFF を読んで送った
    CHECK_EQ(bus.log[2] & 0xff, 0xa0);
    CHECK_EQ(bus.log[3] & 0xff, 0xa1);
    CHECK_EQ(bus.done[2], 1);
    CHECK_EQ(bus.errors[2], 1);

    // REQG = 11: 最初の 1 オペランドだけは要求なしで転送し、あとは外部要求を待つ (チャネル 1 は要求が来ない)
    bus.logLen = 0;
    dmac.write(1 * 64 + 0x00, 0xff);
    dmac.write(1 * 64 + 0x04, 0x08);
    dmac.write(1 * 64 + 0x05, 0x03);
    dmac.write(1 * 64 + 0x06, 0x04);
    dmacWrite16(dmac, 1, 0x0a, 3);
    dmacWrite32(dmac, 1, 0x0c, 0x100);
    dmacWrite32(dmac, 1, 0x14, 0xf00000);
    dmac.write(1 * 64 + 0x07, 0x80);
    dmac.run(bus);
    dmac.run(bus);
    CHECK_EQ(bus.logLen, 1);
    CHECK_EQ(dmac.active(1), true);

    // チェーンモードで CNT を立てると構成エラー (0x01)
    dmac.write(2 * 64 + 0x00, 0xff);
    dmac.write(2 * 64 + 0x05, 0x09);
    dmacWrite32(dmac, 2, 0x1c, 0x40);
    dmacWrite16(dmac, 2, 0x1a, 1);
    dmac.write(2 * 64 + 0x07, 0xc0);
    CHECK_EQ(dmac.read(2 * 64 + 0x01), 0x01);
    CHECK_EQ(dmac.active(2), false);
}

// チェーンの表の 1 つ分を書く (アレイは 6 バイト、リンクアレイは続けてリンク先の 4 バイト)
void putDescriptor(uint8_t* p, uint32_t addr, uint32_t count) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(addr >> (24 - 8 * i));
    p[4] = (uint8_t)(count >> 8);
    p[5] = (uint8_t)count;
}
void putLink(uint8_t* p, uint32_t addr, uint32_t count, uint32_t link) {
    putDescriptor(p, addr, count);
    for (int i = 0; i < 4; i++) p[6 + i] = (uint8_t)(link >> (24 - 8 * i));
}

// リンクアレイチェーンの表が輪になっている (終わらない転送)。ADPCM を IOCS の _ADPCMLOT で回し続け、
// 転送中に MAR を読んで再生位置を知るプログラムがある。
// HD63450 のデータシート (日立の 1987 年のデータブック) の Linked Array Chaining の節と、MC68450 のデータシート
// (1989 年 7 月) の 5.5.3: 開始すると最初の表を MAR と MTC に読み、表のリンク先を BAR に入れる。MTC が 0 になると
// BAR を見て、0 でなければその番地の表を読む。MC68450 の 5.4.2 の 4: 次の表の読み込みは 1 オペランドの転送と同じ扱いで、
// ブロックの終わりに続けて行う (次の要求を待たない)
void testLinkChainRing() {
    printf("dmac linked array chain forming a ring\n");
    x68k::Fdc fdc;
    fdc.reset();
    x68k::Dmac dmac;
    dmac.reset();
    static TestBus bus(&fdc);
    bus.clear();
    for (int i = 0; i < 0x200; i++) bus.mem[0x100 + i] = (uint8_t)(i * 3 + 1);
    // 表 A ($40): $100 から 3 バイト、次は B。表 B ($50): $200 から 2 バイト、次は A
    putLink(bus.mem + 0x40, 0x100, 3, 0x50);
    putLink(bus.mem + 0x50, 0x200, 2, 0x40);
    const int ch = 2;
    dmac.write(ch * 64 + 0x00, 0xff);
    dmac.write(ch * 64 + 0x04, 0x08); // DCR: 16 ビットのポート
    dmac.write(ch * 64 + 0x05, 0x0d); // OCR: メモリ → デバイス、バイト、リンクアレイチェーン、オートリクエスト最大
    dmac.write(ch * 64 + 0x06, 0x04); // SCR: メモリ側は増やす、デバイス側は固定
    dmacWrite32(dmac, ch, 0x14, 0xf00000);
    dmacWrite32(dmac, ch, 0x1c, 0x40);
    dmacWrite16(dmac, ch, 0x1a, 0x1234); // BTC は使わない (変わらない)
    dmac.write(ch * 64 + 0x07, 0x88);
    // 1 オペランドずつ進めて、そのたびに MAR / MTC / BAR を読む (輪を 3 周)
    struct Want {
        uint32_t mar, mtc, bar;
    };
    const Want ring[5] = {
        {0x101, 2, 0x50}, {0x102, 1, 0x50}, {0x200, 2, 0x40}, // A の 3 バイト目のあと、続けて B を読む
        {0x201, 1, 0x40}, {0x100, 3, 0x50},                   // B の 2 バイト目のあと、続けて A を読む
    };
    const uint32_t src[5] = {0x100, 0x101, 0x102, 0x200, 0x201};
    for (int i = 0; i < 15; i++) {
        bus.logLen = 0;
        dmac.run(bus, 1);
        const int before = failures;
        CHECK_EQ(dmacRead32(dmac, ch, 0x0c), ring[i % 5].mar);
        CHECK_EQ(dmacRead16(dmac, ch, 0x0a), ring[i % 5].mtc);
        CHECK_EQ(dmacRead32(dmac, ch, 0x1c), ring[i % 5].bar);
        CHECK_EQ(dmacRead16(dmac, ch, 0x1a), 0x1234);
        CHECK_EQ(dmac.read(ch * 64 + 0x00) & 0xf8, 0x08); // ACT だけ (COC も BTC も立たない)
        CHECK_EQ(bus.logLen, 1);
        CHECK_EQ(bus.log[0], 0xf00000u << 8 | bus.mem[src[i % 5]]);
        CHECK_EQ(dmac.irq(), false);
        if (failures != before) printf("  operand %d\n", i);
    }
    CHECK_EQ(bus.done[ch], 0);
    // 1 回の量を決めなくても run() は戻る (既定の 65536 オペランドで止まる。65536 = 5 x 13107 + 1)
    dmac.run(bus);
    CHECK_EQ(dmac.active(ch), true);
    CHECK_EQ(dmacRead32(dmac, ch, 0x0c), 0x101);
    CHECK_EQ(dmacRead16(dmac, ch, 0x0a), 2);
    CHECK_EQ(dmacRead32(dmac, ch, 0x1c), 0x50);
    CHECK_EQ(bus.done[ch], 0);
    // 止めるのはソフトウェアアボート
    dmac.write(ch * 64 + 0x07, 0x18);
    dmac.notify(bus);
    CHECK_EQ(dmac.active(ch), false);
    CHECK_EQ(dmac.read(ch * 64 + 0x00) & 0xf8, 0x90); // COC と ERR
    CHECK_EQ(dmac.read(ch * 64 + 0x01), 0x11);
    CHECK_EQ(bus.errors[ch], 1);
    CHECK_EQ(dmacRead32(dmac, ch, 0x0c), 0x101); // レジスタは止めたときのまま
    CHECK_EQ(dmac.irq(), true);

    // 外部要求 (ADPCM と同じ REQG = 10) では、デバイスが要求していない間は進まず、レジスタも変わらない
    dmac.reset();
    bus.clear();
    putLink(bus.mem + 0x40, 0x100, 3, 0x40); // 自分を指す表
    dmac.write(1 * 64 + 0x00, 0xff);
    dmac.write(1 * 64 + 0x04, 0x80);
    dmac.write(1 * 64 + 0x05, 0x0e); // リンクアレイチェーン、外部要求
    dmac.write(1 * 64 + 0x06, 0x04);
    dmacWrite32(dmac, 1, 0x14, 0xf00000);
    dmacWrite32(dmac, 1, 0x1c, 0x40);
    dmac.write(1 * 64 + 0x07, 0x80);
    dmac.run(bus);
    dmac.run(bus);
    CHECK_EQ(dmac.active(1), true);
    CHECK_EQ(bus.logLen, 0);
    CHECK_EQ(dmacRead32(dmac, 1, 0x0c), 0x100); // 表は開始のときに読む
    CHECK_EQ(dmacRead16(dmac, 1, 0x0a), 3);
    CHECK_EQ(dmacRead32(dmac, 1, 0x1c), 0x40);
}

// チェーンの転送中に読む BAR と BTC。HD63450 のデータシートの Array Chaining の節と MC68450 のデータシートの 5.5.2:
// アレイチェーンは、表を 1 つ読むたびに BTC を 1 減らし、BAR を 6 増やす (ブロックの転送中の BTC は、まだ読んでいない
// 表の数)。ブロックを終えたときに BTC が 0 なら終わる。
// MC68450 の 5.6.3.1.4: チェーンで MTC に 0 が入ったら、BAR と BTC を新しくする前にカウントエラーで止まる
// (HD63450 のデータシートも、表を読む途中で止まったときの BAR は読んでいた表を指すとしている)
void testChainRegisters() {
    printf("dmac chain: bar and btc during the transfer\n");
    x68k::Fdc fdc;
    fdc.reset();
    x68k::Dmac dmac;
    dmac.reset();
    static TestBus bus(&fdc);
    bus.clear();
    for (int i = 0; i < 0x300; i++) bus.mem[0x100 + i] = (uint8_t)(i * 5 + 2);
    const int ch = 2;
    auto start = [&](uint8_t ocr, uint32_t bar, uint32_t btc) {
        dmac.write(ch * 64 + 0x00, 0xff);
        dmac.write(ch * 64 + 0x04, 0x08);
        dmac.write(ch * 64 + 0x05, ocr);
        dmac.write(ch * 64 + 0x06, 0x04);
        dmacWrite32(dmac, ch, 0x14, 0xf00000);
        dmacWrite32(dmac, ch, 0x1c, bar);
        dmacWrite16(dmac, ch, 0x1a, btc);
        dmac.write(ch * 64 + 0x07, 0x80);
    };
    // アレイチェーン: 3 つの表 ($100 から 2 バイト、$200 から 1 バイト、$300 から 2 バイト)
    putDescriptor(bus.mem + 0x40, 0x100, 2);
    putDescriptor(bus.mem + 0x46, 0x200, 1);
    putDescriptor(bus.mem + 0x4c, 0x300, 2);
    start(0x09, 0x40, 3);
    struct Want {
        uint32_t mar, mtc, bar, btc;
        bool active;
    };
    const Want steps[5] = {
        {0x101, 1, 0x46, 2, true},  // 1 つ目の表を読んだ: BAR + 6、BTC - 1
        {0x200, 1, 0x4c, 1, true},  // 1 つ目のブロックを終え、2 つ目の表を読んだ
        {0x300, 2, 0x52, 0, true},  // 3 つ目 (最後) の表を読んだ: BTC は 0
        {0x301, 1, 0x52, 0, true},
        {0x302, 0, 0x52, 0, false}, // 終わり
    };
    for (int i = 0; i < 5; i++) {
        dmac.run(bus, 1);
        const int before = failures;
        CHECK_EQ(dmacRead32(dmac, ch, 0x0c), steps[i].mar);
        CHECK_EQ(dmacRead16(dmac, ch, 0x0a), steps[i].mtc);
        CHECK_EQ(dmacRead32(dmac, ch, 0x1c), steps[i].bar);
        CHECK_EQ(dmacRead16(dmac, ch, 0x1a), steps[i].btc);
        CHECK_EQ(dmac.active(ch), steps[i].active);
        if (failures != before) printf("  array chain, operand %d\n", i);
    }
    CHECK_EQ(dmac.read(ch * 64 + 0x00) & 0xf8, 0x80); // COC、エラーなし
    CHECK_EQ(bus.done[ch], 1);
    CHECK_EQ(bus.logLen, 5);

    // アレイチェーンの 2 つ目の表の転送数が 0: カウントエラー。BAR と BTC は 2 つ目の表を読む前のまま
    putDescriptor(bus.mem + 0x46, 0x200, 0);
    start(0x09, 0x40, 3);
    dmac.run(bus);
    CHECK_EQ(dmac.active(ch), false);
    CHECK_EQ(dmac.read(ch * 64 + 0x00) & 0xf8, 0x90);
    CHECK_EQ(dmac.read(ch * 64 + 0x01), 0x0d);
    CHECK_EQ(dmacRead32(dmac, ch, 0x1c), 0x46);
    CHECK_EQ(dmacRead16(dmac, ch, 0x1a), 2);
    CHECK_EQ(bus.done[ch], 1); // 終わりの知らせは増えない
    CHECK_EQ(bus.errors[ch], 1);

    // リンクアレイチェーンの 2 つ目の表の転送数が 0: BAR は 2 つ目の表の番地のまま (その表のリンク先にしない)
    putLink(bus.mem + 0x60, 0x100, 2, 0x70);
    putLink(bus.mem + 0x70, 0x200, 0, 0x80);
    putLink(bus.mem + 0x80, 0x300, 1, 0);
    start(0x0d, 0x60, 0);
    dmac.run(bus);
    CHECK_EQ(dmac.active(ch), false);
    CHECK_EQ(dmac.read(ch * 64 + 0x01), 0x0d);
    CHECK_EQ(dmacRead32(dmac, ch, 0x1c), 0x70);
    CHECK_EQ(bus.errors[ch], 2);

    // リンクアレイチェーンが終わったときの BAR は 0 (最後の表のリンク先)
    putLink(bus.mem + 0x70, 0x200, 1, 0x80);
    bus.logLen = 0;
    start(0x0d, 0x60, 0);
    dmac.run(bus, 2);
    CHECK_EQ(dmacRead32(dmac, ch, 0x1c), 0x80); // 2 つ目の表を読んだところ
    CHECK_EQ(dmacRead32(dmac, ch, 0x0c), 0x200);
    dmac.run(bus);
    CHECK_EQ(dmac.active(ch), false);
    CHECK_EQ(dmac.read(ch * 64 + 0x00) & 0xf8, 0x80);
    CHECK_EQ(dmacRead32(dmac, ch, 0x1c), 0);
    CHECK_EQ(dmacRead32(dmac, ch, 0x0c), 0x301);
    CHECK_EQ(bus.logLen, 4);
    CHECK_EQ(bus.done[ch], 2);
}

// アレイチェーンの転送の途中で保存した、版 6 までのステート (BTC をブロックの終わりに減らしていたので、今より 1 大きい)
// を読むと、BTC を今の数え方に直す (直さなければ、表を 1 つ余分に読んで転送する)
void testChainStateVersion() {
    printf("dmac state: btc of an array chain saved by version 6\n");
    x68k::Fdc fdc;
    fdc.reset();
    static TestBus bus(&fdc);
    bus.clear();
    for (int i = 0; i < 0x300; i++) bus.mem[0x100 + i] = (uint8_t)(i * 5 + 2);
    putDescriptor(bus.mem + 0x40, 0x100, 2);
    putDescriptor(bus.mem + 0x46, 0x200, 1);
    putDescriptor(bus.mem + 0x4c, 0x300, 2);
    putDescriptor(bus.mem + 0x52, 0x380, 4); // 表の続きにある 4 つ目 (BTC は 3 なので読まない)
    const int ch = 2;
    x68k::Dmac a;
    a.reset();
    a.write(ch * 64 + 0x00, 0xff);
    a.write(ch * 64 + 0x04, 0x08);
    a.write(ch * 64 + 0x05, 0x09); // アレイチェーン、オートリクエスト最大
    a.write(ch * 64 + 0x06, 0x04);
    dmacWrite32(a, ch, 0x14, 0xf00000);
    dmacWrite32(a, ch, 0x1c, 0x40);
    dmacWrite16(a, ch, 0x1a, 3);
    a.write(ch * 64 + 0x07, 0x80);
    // 開始しただけで表を読んでいないチャネルは、どの版でも BTC は同じ
    static uint8_t buf[4096];
    {
        x68k::StateIO save(x68k::StateIO::SAVE, buf, sizeof buf, 6);
        a.state(save);
        x68k::Dmac b;
        x68k::StateIO load(x68k::StateIO::LOAD, buf, sizeof buf, 6);
        b.state(load);
        CHECK_EQ(dmacRead16(b, ch, 0x1a), 3);
    }
    a.run(bus, 1); // 1 つ目のブロックの途中
    CHECK_EQ(dmacRead16(a, ch, 0x1a), 2);
    x68k::StateIO save(x68k::StateIO::SAVE, buf, sizeof buf, 6);
    a.state(save);
    CHECK_EQ(save.ok(), true);
    CHECK_EQ(dmacRead16(a, ch, 0x1a), 2); // 保存しても変わらない
    // 版 6 の並びの中の BTC は 3 (前の数え方)。版 7 として読むとそのまま見える
    x68k::Dmac raw;
    x68k::StateIO asNew(x68k::StateIO::LOAD, buf, sizeof buf, 7);
    raw.state(asNew);
    CHECK_EQ(dmacRead16(raw, ch, 0x1a), 3);
    // 版 8 (CD-ROM 装置の状態を足した版) でも、DMAC の読み方は版 7 と同じ
    x68k::Dmac raw8;
    x68k::StateIO asNewest(x68k::StateIO::LOAD, buf, sizeof buf, 8);
    raw8.state(asNewest);
    CHECK_EQ(dmacRead16(raw8, ch, 0x1a), 3);
    // 版 6 として読むと 2 になり、続きを動かすと 3 つのブロックで終わる
    x68k::Dmac b;
    x68k::StateIO load(x68k::StateIO::LOAD, buf, sizeof buf, 6);
    b.state(load);
    CHECK_EQ(load.ok(), true);
    CHECK_EQ(dmacRead16(b, ch, 0x1a), 2);
    bus.logLen = 0;
    b.run(bus);
    CHECK_EQ(b.active(ch), false);
    CHECK_EQ(b.read(ch * 64 + 0x00) & 0xf8, 0x80);
    CHECK_EQ(bus.logLen, 4); // 残りの 4 バイト (4 つ目の表は読まない)
    CHECK_EQ(dmacRead16(b, ch, 0x1a), 0);
    CHECK_EQ(dmacRead32(b, ch, 0x1c), 0x52);
    CHECK_EQ(dmacRead32(b, ch, 0x0c), 0x302);
    // 今の版は、そのまま読み書きする
    x68k::StateIO save7(x68k::StateIO::SAVE, buf, sizeof buf);
    a.state(save7);
    x68k::Dmac c;
    x68k::StateIO load7(x68k::StateIO::LOAD, buf, sizeof buf);
    c.state(load7);
    CHECK_EQ(dmacRead16(c, ch, 0x1a), 2);
}

// 版 6 のステートを読むときに BTC を 1 減らすのは、表を読み終えて転送している途中のアレイチェーンだけであること。
// HLT 中 (途中なので読み替える)、正常終了後、エラー停止後、リンクチェーン、版 6 の BTC が 0 (読み替えない) を見る
void testChainStateVersionCases() {
    printf("dmac state: version 6 btc in other channel states\n");
    x68k::Fdc fdc;
    fdc.reset();
    static TestBus bus(&fdc);
    const int ch = 2;
    // 表を用意して開始する (メモリからデバイス、オートリクエスト)。link なら リンクアレイチェーン
    auto start = [&](x68k::Dmac& d, bool link) {
        bus.clear();
        for (int i = 0; i < 0x300; i++) bus.mem[0x100 + i] = (uint8_t)(i * 3 + 1);
        if (link) {
            putLink(bus.mem + 0x40, 0x100, 2, 0x60);
            putLink(bus.mem + 0x60, 0x200, 1, 0x50);
            putLink(bus.mem + 0x50, 0x300, 2, 0);
        } else {
            putDescriptor(bus.mem + 0x40, 0x100, 2);
            putDescriptor(bus.mem + 0x46, 0x200, 1);
            putDescriptor(bus.mem + 0x4c, 0x300, 2);
            putDescriptor(bus.mem + 0x52, 0x380, 4);
        }
        d.reset();
        d.write(ch * 64 + 0x00, 0xff);
        d.write(ch * 64 + 0x04, 0x08);
        d.write(ch * 64 + 0x05, link ? 0x0d : 0x09);
        d.write(ch * 64 + 0x06, 0x04);
        dmacWrite32(d, ch, 0x14, 0xf00000);
        dmacWrite32(d, ch, 0x1c, 0x40);
        dmacWrite16(d, ch, 0x1a, link ? 0 : 3);
        d.write(ch * 64 + 0x07, 0x80);
    };
    static uint8_t buf[4096];
    // 版 version で保存したものを、版 6 として読む。保存した側の BTC が変わらないことも見る
    auto roundTrip = [&](x68k::Dmac& from, x68k::Dmac& to, uint32_t version) {
        const uint32_t btc = dmacRead16(from, ch, 0x1a);
        x68k::StateIO save(x68k::StateIO::SAVE, buf, sizeof buf, version);
        from.state(save);
        CHECK_EQ(save.ok(), true);
        CHECK_EQ(dmacRead16(from, ch, 0x1a), btc);
        x68k::StateIO load(x68k::StateIO::LOAD, buf, sizeof buf, 6);
        to.state(load);
        CHECK_EQ(load.ok(), true);
    };

    // HLT 中: 動いている (ACT) ので読み替える。HLT を下ろすと 3 つのブロックで終わる
    {
        x68k::Dmac a, b;
        start(a, false);
        a.run(bus, 1);
        a.write(ch * 64 + 0x07, 0x20); // HLT
        a.run(bus);
        CHECK_EQ(a.active(ch), true);
        CHECK_EQ(dmacRead16(a, ch, 0x1a), 2);
        roundTrip(a, b, 6);
        CHECK_EQ(dmacRead16(b, ch, 0x1a), 2);
        CHECK_EQ(b.active(ch), true);
        bus.logLen = 0;
        b.write(ch * 64 + 0x07, 0x00); // HLT を下ろす
        b.run(bus);
        CHECK_EQ(b.active(ch), false);
        CHECK_EQ(b.read(ch * 64 + 0x00) & 0xf8, 0x80);
        CHECK_EQ(bus.logLen, 4);
        CHECK_EQ(dmacRead32(b, ch, 0x1c), 0x52);
    }
    // 正常終了後: 動いていないので読み替えない (BTC は 0 のまま)
    {
        x68k::Dmac a, b;
        start(a, false);
        a.run(bus);
        CHECK_EQ(a.active(ch), false);
        CHECK_EQ(a.read(ch * 64 + 0x00) & 0xf8, 0x80);
        CHECK_EQ(dmacRead16(a, ch, 0x1a), 0);
        roundTrip(a, b, 6);
        CHECK_EQ(dmacRead16(b, ch, 0x1a), 0);
        CHECK_EQ(b.read(ch * 64 + 0x00) & 0xf8, 0x80);
    }
    // エラー停止後 (2 つ目のブロックでバスエラー): 動いていないので読み替えない
    {
        x68k::Dmac a, b;
        start(a, false);
        bus.busErrorAt = 0x200;
        a.run(bus);
        CHECK_EQ(a.active(ch), false);
        CHECK_EQ(a.read(ch * 64 + 0x00) & 0xf8, 0x90); // COC、ERR
        CHECK_EQ(dmacRead16(a, ch, 0x1a), 1);
        roundTrip(a, b, 6);
        CHECK_EQ(dmacRead16(b, ch, 0x1a), 1);
        CHECK_EQ(b.active(ch), false);
    }
    // リンクアレイチェーン: BTC を数えないので、途中でも読み替えない
    {
        x68k::Dmac a, b;
        start(a, true);
        a.run(bus, 1);
        CHECK_EQ(a.active(ch), true);
        const uint32_t btc = dmacRead16(a, ch, 0x1a);
        roundTrip(a, b, 6);
        CHECK_EQ(dmacRead16(b, ch, 0x1a), btc);
        bus.logLen = 0;
        b.run(bus);
        CHECK_EQ(b.active(ch), false);
        CHECK_EQ(b.read(ch * 64 + 0x00) & 0xf8, 0x80);
        CHECK_EQ(bus.logLen, 4);
        CHECK_EQ(dmacRead32(b, ch, 0x1c), 0);
    }
    // 版 6 の並びの中の BTC が 0 のまま途中のブロックにある (版 6 の数え方では起きない): 読み替えずに 0 のまま
    // ($FFFF にすると、表を 65535 個読みにいく)。最後のブロックの途中を今の版で保存し、版 6 として読んで作る
    {
        x68k::Dmac a, b;
        start(a, false);
        a.run(bus, 4); // 3 つ目のブロックの 1 バイト目まで
        CHECK_EQ(a.active(ch), true);
        CHECK_EQ(dmacRead16(a, ch, 0x1a), 0);
        CHECK_EQ(dmacRead16(a, ch, 0x0a), 1);
        roundTrip(a, b, x68k::STATE_VERSION);
        CHECK_EQ(dmacRead16(b, ch, 0x1a), 0);
        bus.logLen = 0;
        b.run(bus);
        CHECK_EQ(b.active(ch), false);
        CHECK_EQ(b.read(ch * 64 + 0x00) & 0xf8, 0x80);
        CHECK_EQ(bus.logLen, 1);
        CHECK_EQ(dmacRead32(b, ch, 0x1c), 0x52);
    }
}

// WRITE ID (フォーマット) を 1 回する: C H R N を 4 バイトずつ count 個、$1000 から DMA で渡す。結果の 7 バイトを返す
void writeId(x68k::Fdc& fdc, x68k::Dmac& dmac, TestBus& bus, uint8_t n, uint8_t fill, const uint8_t* ids, int count,
             int dmaBytes, uint8_t* result) {
    memcpy(bus.mem + 0x1000, ids, (size_t)count * 4);
    startFdcDma(dmac, false, 0x1000, (uint32_t)dmaBytes);
    fdcCommand(fdc, {0x4d, 0x00, n, (uint8_t)count, 0x54, fill});
    dmac.run(bus);
    if (dmac.active(0)) { // FDC が DMA の前に終わった (書き込み禁止など)
        dmac.write(0x07, 0x10);
        dmac.notify(bus);
    }
    fdcResult(fdc, result, 7);
}

// WRITE ID で書き換えたセクタに dirty フラグが立つ (ベアメタル版は dirty フラグの立ったチャンクだけを書き戻すので、
// 立たなければフォーマットがイメージのファイルに残らない)。書き換えなかったときは立たない
void testWriteId() {
    printf("write id marks the sectors dirty\n");
    const uint32_t CHUNK = x68k::DirtyMap::CHUNK;
    x68k::FloppyImage img;
    CHECK_EQ(img.load(g_xdf, sizeof g_xdf, false, "a.xdf"), true);
    x68k::Fdc fdc;
    fdc.reset();
    fdc.insert(0, &img);
    x68k::Dmac dmac;
    dmac.reset();
    static TestBus bus(&fdc);
    bus.clear();
    uint8_t r[7], sense[2];
    // シリンダ 10 のヘッド 0 (トラック 20) をフォーマットする
    fdcCommand(fdc, {0x0f, 0x00, 10});
    fdcCommand(fdc, {0x08});
    fdcResult(fdc, sense, 2);
    uint8_t ids[8 * 4];
    for (int i = 0; i < 8; i++) {
        ids[i * 4 + 0] = 10;
        ids[i * 4 + 1] = 0;
        ids[i * 4 + 2] = (uint8_t)(i + 1);
        ids[i * 4 + 3] = 3;
    }
    CHECK_EQ(img.dirty(), false);
    // ID を半分渡したところで TC: 何も書き換えず、dirty フラグも立たない
    writeId(fdc, dmac, bus, 3, 0xe5, ids, 8, 16, r);
    CHECK_EQ(img.dirty(), false);
    CHECK_EQ(img.data(img.find(20, 10, 0, 1, 3))[5], (uint8_t)(5 + 20 + 1));
    // 8 セクタ分を渡す: 8 セクタが $E5 で埋まり、その範囲のチャンクにだけ dirty フラグが立つ
    writeId(fdc, dmac, bus, 3, 0xe5, ids, 8, 32, r);
    CHECK_EQ(r[0] & 0xc0, 0x00);
    CHECK_EQ(dmac.read(0x00) & 0x90, 0x80);
    const x68k::FloppyImage::Sector* first = img.find(20, 10, 0, 1, 3);
    const x68k::FloppyImage::Sector* last = img.find(20, 10, 0, 8, 3);
    CHECK_EQ(first && last, true);
    if (!first || !last) return;
    int wrong = 0;
    for (uint32_t i = 0; i < 8 * 1024; i++)
        if (img.data(first)[i] != 0xe5) wrong++;
    CHECK_EQ(wrong, 0);
    CHECK_EQ(img.data(img.find(19, 9, 1, 8, 3))[5], (uint8_t)(5 + 19 + 8)); // 前後のトラックはそのまま
    CHECK_EQ(img.data(img.find(21, 10, 1, 1, 3))[5], (uint8_t)(5 + 21 + 1));
    CHECK_EQ(img.dirty(), true);
    const uint32_t c0 = first->offset / CHUNK, c1 = (last->offset + last->size - 1) / CHUNK;
    wrong = 0;
    for (uint32_t c = 0; c < img.dirtyMap().chunks(); c++)
        if (img.dirtyMap().test(c) != (c >= c0 && c <= c1)) wrong++;
    CHECK_EQ(wrong, 0);
    CHECK_EQ(img.dirtyMap().count(), c1 - c0 + 1);
    // イメージにない ID (R=9) が混じる: 異常終了するが、あったセクタは書き換わって dirty フラグが立つ
    img.clearDirty();
    ids[7 * 4 + 2] = 9;
    writeId(fdc, dmac, bus, 3, 0x11, ids, 8, 32, r);
    CHECK_EQ(r[0] & 0xc0, 0x40);
    CHECK_EQ(img.data(first)[0], 0x11);
    CHECK_EQ(img.data(last)[0], 0xe5);
    CHECK_EQ(img.dirty(), true);
    CHECK_EQ(img.dirtyMap().test(c0), true);
    // 書き込み禁止: 書き換えず、dirty フラグも立たない (ST1 の NW)
    x68k::FloppyImage ro;
    CHECK_EQ(ro.load(g_xdf, sizeof g_xdf, true, "a.xdf"), true);
    fdc.insert(0, &ro);
    ids[7 * 4 + 2] = 8;
    writeId(fdc, dmac, bus, 3, 0x22, ids, 8, 32, r);
    CHECK_EQ(r[0] & 0xc0, 0x40);
    CHECK_EQ(r[1], 0x02);
    CHECK_EQ(ro.dirty(), false);
    CHECK_EQ(ro.data(ro.find(20, 10, 0, 1, 3))[0], 0x11);
    makeXdf(); // ほかの試験のために元に戻す

    // D88: デリーテッドデータで CRC エラーのセクタも、フォーマットすると通常のデータになる (ヘッダの中のフラグと状態も消え、
    // ヘッダのあるチャンクにも dirty フラグが立つ)
    const uint32_t size = makeD88(g_d88, 0x2b0, 2, 4);
    uint8_t* h2 = g_d88 + 0x2b0 + (16 + 256); // トラック 0 の R=2 のヘッダ
    h2[7] = 0x10;
    h2[8] = 0xb0;
    x68k::FloppyImage d88;
    CHECK_EQ(d88.load(g_d88, size, false, "a.d88"), true);
    fdc.insert(0, &d88);
    fdcCommand(fdc, {0x07, 0x00}); // RECALIBRATE
    fdcCommand(fdc, {0x08});
    fdcResult(fdc, sense, 2);
    for (int i = 0; i < 4; i++) {
        ids[i * 4 + 0] = 0;
        ids[i * 4 + 1] = 0;
        ids[i * 4 + 2] = (uint8_t)(i + 1);
        ids[i * 4 + 3] = 1;
    }
    writeId(fdc, dmac, bus, 1, 0xe5, ids, 4, 16, r);
    CHECK_EQ(r[0] & 0xc0, 0x00);
    const x68k::FloppyImage::Sector* s2 = d88.find(0, 0, 0, 2, 1);
    CHECK_EQ(s2 != nullptr, true);
    if (!s2) return;
    CHECK_EQ(s2->deleted, false);
    CHECK_EQ(s2->status, 0);
    CHECK_EQ(h2[7], 0x00);
    CHECK_EQ(h2[8], 0x00);
    CHECK_EQ(d88.data(s2)[0], 0xe5);
    CHECK_EQ(d88.data(s2)[255], 0xe5);
    CHECK_EQ(d88.dirtyMap().test((s2->offset - 16) / CHUNK), true);
    CHECK_EQ(d88.data(d88.find(1, 0, 1, 1, 1))[0], 0x41); // ヘッド 1 はそのまま
    // フォーマットしたセクタは READ DATA で正常に読める
    bus.clear();
    startFdcDma(dmac, true, 0x2000, 256);
    fdcCommand(fdc, {0x46, 0x00, 0x00, 0x00, 0x02, 0x01, 0x02, 0x1b, 0xff});
    dmac.run(bus);
    fdcResult(fdc, r, 7);
    CHECK_EQ(r[0] & 0xc0, 0x00);
    CHECK_EQ(r[1], 0x00);
    CHECK_EQ(r[2], 0x00);
    CHECK_EQ(bus.mem[0x2000], 0xe5);
}

} // namespace

// ---------------------------------------------------------------- まとめた転送 (Dmac::Host::dmaReadBlock)

// Machine::dmaReadBlock と同じように、FDC のデータのレジスタからメモリ (64KB) へまとめて転送するバス。
// メモリの外とユーザの FC で止められる番地 ($8000 以上) と busErrorAt にかかるバイトは、1 バイトずつの処理に任せる
struct BlockBus : TestBus {
    int blocks = 0;
    BlockBus(x68k::Fdc* f) : TestBus(f) {}
    int dmaReadBlock(int ch, uint32_t dar, int dfc, uint32_t mar, int mfc, int n) override {
        if (ch != 0 || dar != 0xe94003 || user(dfc)) return 0;
        uint32_t end = user(mfc) ? 0x8000 : (uint32_t)sizeof mem;
        if (busErrorAt >= mar && busErrorAt < end) end = busErrorAt;
        if (mar >= end) return 0;
        if ((uint32_t)n > end - mar) n = (int)(end - mar);
        int k = fdc->readData(mem + mar, n);
        if (k > 0) blocks++;
        return k;
    }
};

// FDC と DMAC を 1 つの場合について動かし、メモリ、DMAC のレジスタ、FDC の結果をまとめて返す
struct ReadOutcome {
    uint8_t mem[0x10000];
    uint8_t dmacRegs[64];
    uint8_t result[7];
    int resultLen;
    int done, errors;
    bool fdcIrq, dmacIrq;
    int blocks;
};

// 読むイメージ: XDF (g_xdf)、D88 で R=2 がデータの CRC エラー、D88 で R=2 がデリーテッドデータ
// (D88 は 1 トラック、4 セクタ、256 バイト、中身は $40 + R)
enum class ReadImage { XDF, D88_CRC, D88_DELETED };

struct ReadCase {
    const char* name;
    uint8_t cmd[9];
    uint32_t mar, count;
    int budget;            // dmac.run() の 1 回の量 (0 なら既定)
    uint32_t busErrorAt;   // 0xffffffff ならなし
    bool userMemory;       // MFC をユーザのデータにする
    // ここから下を変えなければ、IOCS と同じ設定 (チェーンなし、外部要求) で XDF を読む
    uint8_t ocr = 0x82;
    uint8_t ccr = 0x88;
    void (*setup)(x68k::Dmac&, TestBus&) = nullptr; // CCR を書く前に、チェーンの表や BAR / BTC / BFC を用意する
    ReadImage image = ReadImage::XDF;
    bool runBeforeCommand = false; // FDC のコマンドの前に 1 オペランド分だけ run() する (REQG = 11 の最初の 1 オペランド)
    int haltAfter = 0;             // run() をこの回数したあとで HLT を立て、進まないことを確かめてから下ろす
    bool expectBlocks = true;      // まとめた転送を 1 回は使うはず
    void (*check)(const ReadOutcome&) = nullptr; // 1 バイトずつでも、まとめても満たすはずの結果
};

uint32_t outReg16(const ReadOutcome& o, int r) { return (uint32_t)(o.dmacRegs[r] << 8 | o.dmacRegs[r + 1]); }
uint32_t outReg32(const ReadOutcome& o, int r) { return outReg16(o, r) << 16 | outReg16(o, r + 2); }

// XDF のトラック t の R = r のセクタから続けて読んだときの、先頭から k バイト目 (makeXdf())
uint8_t xdfStream(int t, int r, uint32_t k) {
    const int rr = r + (int)(k / 1024);
    const uint32_t i = k % 1024;
    return i == 0 ? (uint8_t)t : i == 1 ? (uint8_t)rr : (uint8_t)(i + (uint32_t)t + (uint32_t)rr);
}
// mem の n バイトのうち、上の並びの skip バイト目からと違うバイトの数
int xdfMismatch(const uint8_t* mem, int t, int r, uint32_t skip, uint32_t n) {
    int bad = 0;
    for (uint32_t j = 0; j < n; j++)
        if (mem[j] != xdfStream(t, r, skip + j)) bad++;
    return bad;
}
// mem の n バイトのうち v でないバイトの数
int fillMismatch(const uint8_t* mem, uint8_t v, uint32_t n) {
    int bad = 0;
    for (uint32_t j = 0; j < n; j++)
        if (mem[j] != v) bad++;
    return bad;
}
// 転送の前にメモリに入れておく値 (転送していない番地はこのまま)
uint8_t untouched(uint32_t a) { return (uint8_t)(a * 7 + 3); }

void runReadCase(const ReadCase& c, bool block, ReadOutcome& out) {
    static x68k::FloppyImage img;
    if (c.image == ReadImage::XDF) {
        img.load(g_xdf, sizeof g_xdf, false, "a.xdf");
    } else {
        const uint32_t size = makeD88(g_d88, 0x2b0, 1, 4);
        uint8_t* h = g_d88 + 0x2b0 + (16 + 256); // R=2 のセクタのヘッダ
        if (c.image == ReadImage::D88_CRC) h[8] = 0xb0;
        else h[7] = 0x10;
        img.load(g_d88, size, false, "a.d88");
    }
    x68k::Fdc fdc;
    fdc.reset();
    fdc.insert(0, &img);
    x68k::Dmac dmac;
    dmac.reset();
    static TestBus plain(nullptr);
    static BlockBus blockBus(nullptr);
    TestBus& bus = block ? blockBus : plain;
    bus.fdc = &fdc;
    bus.clear();
    blockBus.blocks = 0;
    for (uint32_t i = 0; i < sizeof bus.mem; i++) bus.mem[i] = untouched(i);
    bus.busErrorAt = c.busErrorAt;
    // シリンダは R の ID と同じにしておく (SEEK)
    fdcCommand(fdc, {0x0f, (uint8_t)(c.cmd[1] & 3), c.cmd[2]});
    fdcCommand(fdc, {0x08});
    uint8_t sense[2];
    fdcResult(fdc, sense, 2);
    dmac.write(0x00, 0xff);  // CSR を消す
    dmac.write(0x04, 0x80);  // DCR: 8 ビットのポート
    dmac.write(0x05, c.ocr); // OCR
    dmac.write(0x06, 0x04);  // SCR: メモリ側は増やす、デバイス側は固定
    dmacWrite16(dmac, 0, 0x0a, c.count);
    dmacWrite32(dmac, 0, 0x0c, c.mar);
    dmacWrite32(dmac, 0, 0x14, 0xe94003);
    dmac.write(0x25, 0x64); // NIV
    dmac.write(0x27, 0x65); // EIV
    if (c.userMemory) dmac.write(0x29, m68k::FC_USER_DATA);
    if (c.setup) c.setup(dmac, bus);
    dmac.write(0x07, c.ccr);
    if (c.runBeforeCommand) dmac.run(bus, 1);
    for (uint8_t b : c.cmd) fdc.write(3, b);
    int runs = 0;
    for (int i = 0; i < 100000 && dmac.active(0) && (fdc.read(1) & 0xd0) != 0xd0; i++) {
        if (c.budget) dmac.run(bus, c.budget);
        else dmac.run(bus);
        if (!dmac.active(0)) break;
        if (!c.budget) break; // 既定の量なら 1 回で進むところまで進む
        if (c.haltAfter && ++runs == c.haltAfter) {
            // HLT を立てている間は、要求があっても転送しない (レジスタも FDC も進まない)。下ろすと続きから進む
            const uint32_t mtc = dmacRead16(dmac, 0, 0x0a), mar = dmacRead32(dmac, 0, 0x0c);
            dmac.write(0x07, 0x28);
            dmac.run(bus, c.budget);
            dmac.run(bus);
            CHECK_EQ(dmac.active(0), true);
            CHECK_EQ(dmacRead16(dmac, 0, 0x0a), mtc);
            CHECK_EQ(dmacRead32(dmac, 0, 0x0c), mar);
            CHECK_EQ(mtc, c.count - (uint32_t)(c.haltAfter * c.budget));
            CHECK_EQ(fdc.dataReady(), true);
            dmac.write(0x07, 0x08);
        }
    }
    memcpy(out.mem, bus.mem, sizeof out.mem);
    for (int r = 0; r < 64; r++) out.dmacRegs[r] = dmac.read((uint32_t)r);
    out.fdcIrq = fdc.irq();
    out.dmacIrq = dmac.irq();
    out.done = bus.done[0];
    out.errors = bus.errors[0];
    out.resultLen = 0;
    memset(out.result, 0, sizeof out.result);
    if (dmac.active(0)) { // EOT やエラーで FDC が先に止まり、DMAC は残りを待っている
        dmac.write(0x07, 0x10); // ソフトウェアアボート
        dmac.notify(bus);
    }
    while (out.resultLen < 7 && (fdc.read(1) & 0xd0) == 0xd0) out.result[out.resultLen++] = fdc.read(3);
    out.blocks = blockBus.blocks;
}

// 3 つのブロック ($1000 に 600 バイト、$3000 に 1024 バイト、$2000 に 424 バイト) に分けて読んだ 2 セクタ
// (C=2 H=1 の R=7 と R=8、トラック 5)
void checkThreeBlocks(const ReadOutcome& o) {
    CHECK_EQ(xdfMismatch(o.mem + 0x1000, 5, 7, 0, 600), 0);
    CHECK_EQ(o.mem[0x1000 + 600], untouched(0x1000 + 600));
    CHECK_EQ(xdfMismatch(o.mem + 0x3000, 5, 7, 600, 1024), 0);
    CHECK_EQ(o.mem[0x3000 + 1024], untouched(0x3000 + 1024));
    CHECK_EQ(xdfMismatch(o.mem + 0x2000, 5, 7, 1624, 424), 0);
    CHECK_EQ(o.mem[0x2000 + 424], untouched(0x2000 + 424));
    CHECK_EQ(o.dmacRegs[0x00] & 0xf8, 0x80); // COC、エラーなし
    CHECK_EQ(outReg16(o, 0x0a), 0);
    CHECK_EQ(outReg32(o, 0x0c), 0x2000 + 424);
    CHECK_EQ(o.done, 1);
    CHECK_EQ(o.errors, 0);
    CHECK_EQ(o.dmacIrq, true);
    // チェーンでは、DONE (FDC の TC) は最後のブロックの最後の転送でだけ出る (HD63450 のデータシートの DONE の説明):
    // EOT の R=8 の終わりなので、結果は次のシリンダの R=1
    CHECK_EQ(o.resultLen, 7);
    CHECK_EQ(o.result[0] & 0xc0, 0x00);
    CHECK_EQ(o.result[3], 3);
    CHECK_EQ(o.result[5], 1);
}

// D88 の R=1 から読んで、R=2 を読んだところで FDC が異常終了した (DMAC は残りの 256 バイトを待っている)
void checkStoppedAtR2(const ReadOutcome& o) {
    CHECK_EQ(fillMismatch(o.mem + 0x1000, 0x41, 256), 0);
    CHECK_EQ(fillMismatch(o.mem + 0x1100, 0x42, 256), 0);
    CHECK_EQ(o.mem[0x1200], untouched(0x1200));
    CHECK_EQ(o.dmacRegs[0x00] & 0xf8, 0x08); // ACT のまま
    CHECK_EQ(outReg16(o, 0x0a), 256);
    CHECK_EQ(outReg32(o, 0x0c), 0x1200);
    CHECK_EQ(o.done, 0);
    CHECK_EQ(o.fdcIrq, true);
    CHECK_EQ(o.resultLen, 7);
    CHECK_EQ(o.result[0], 0x40);
    CHECK_EQ(o.result[5], 2); // 止まったセクタ
}

void testBlockRead() {
    printf("dma block read matches byte-by-byte\n");
    const uint32_t none = 0xffffffff;
    static ReadCase cases[32];
    int n = 0;
    auto add = [&](const ReadCase& c) -> ReadCase& {
        cases[n] = c;
        return cases[n++];
    };
    // C=2 H=1 R=7 から、TC がセクタの途中 (1500 バイト)
    add({"tc mid-sector", {0xc6, 0x04, 0x02, 0x01, 0x07, 0x03, 0x08, 0x1b, 0xff}, 0x1000, 1500, 0, none, false});
    // TC がセクタの終わり (2 セクタ)
    add({"tc at sector end", {0xc6, 0x04, 0x02, 0x01, 0x07, 0x03, 0x08, 0x1b, 0xff}, 0x1000, 2048, 0, none, false});
    // MT でヘッド 0 の R=7 からヘッド 1 へ続けて 4 セクタ
    add({"mt across heads", {0xc6, 0x00, 0x03, 0x00, 0x07, 0x03, 0x08, 0x1b, 0xff}, 0x2000, 4096, 0, none, false});
    // EOT (R=2) で FDC が止まる (DMA は 3 セクタ分)
    add({"eot without tc", {0x46, 0x00, 0x00, 0x00, 0x01, 0x03, 0x02, 0x1b, 0xff}, 0x1000, 3072, 0, none, false});
    // 1 回の量を小さくして、何回にも分けて転送する
    add({"small budget", {0xc6, 0x00, 0x01, 0x00, 0x05, 0x03, 0x08, 0x1b, 0xff}, 0x3000, 5000, 100, none, false});
    // メモリの終わり ($10000) を越える: そのバイトでバスエラー
    add({"past memory end", {0x46, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x1b, 0xff}, 0xfe00, 1024, 0, none, false});
    // 途中の番地でバスエラー
    add({"bus error", {0x46, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x1b, 0xff}, 0x4000, 2048, 0, 0x4405, false});
    // ユーザの FC: $8000 からはバスエラー
    add({"user fc", {0x46, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x1b, 0xff}, 0x7f00, 1024, 0, none, true});

    // ---- DMAC のチェーンとコンティニュー (ブロックの終わりの処理は 1 バイトずつの側に任せている) ----
    // 下の 3 つは、どれも C=2 H=1 の R=7 と R=8 (2048 バイト) を読む
    const ReadCase twoSectors = {"", {0xc6, 0x04, 0x02, 0x01, 0x07, 0x03, 0x08, 0x1b, 0xff}, 0, 0, 0, none, false};
    {
        // アレイチェーン: 表は $40 から 3 つ
        ReadCase& c = add(twoSectors);
        c.name = "array chain";
        c.ocr = 0x8a;
        c.setup = [](x68k::Dmac& d, TestBus& bus) {
            putDescriptor(bus.mem + 0x40, 0x1000, 600);
            putDescriptor(bus.mem + 0x46, 0x3000, 1024);
            putDescriptor(bus.mem + 0x4c, 0x2000, 424);
            dmacWrite32(d, 0, 0x1c, 0x40);
            dmacWrite16(d, 0, 0x1a, 3);
        };
        c.check = [](const ReadOutcome& o) {
            checkThreeBlocks(o);
            CHECK_EQ(outReg32(o, 0x1c), 0x40 + 18);
            CHECK_EQ(outReg16(o, 0x1a), 0);
        };
    }
    {
        // 同じ転送を、1 回の量を 1 バイトにして (ブロックの終わりと量の終わりが重なる)
        ReadCase& c = add(cases[n - 1]);
        c.name = "array chain, budget 1";
        c.budget = 1;
    }
    {
        // リンクアレイチェーン: 表は $40 → $60 → $50 の順
        ReadCase& c = add(twoSectors);
        c.name = "linked array chain";
        c.ocr = 0x8e;
        c.setup = [](x68k::Dmac& d, TestBus& bus) {
            putLink(bus.mem + 0x40, 0x1000, 600, 0x60);
            putLink(bus.mem + 0x60, 0x3000, 1024, 0x50);
            putLink(bus.mem + 0x50, 0x2000, 424, 0);
            dmacWrite32(d, 0, 0x1c, 0x40);
        };
        c.check = [](const ReadOutcome& o) {
            checkThreeBlocks(o);
            CHECK_EQ(outReg32(o, 0x1c), 0);
        };
    }
    {
        // コンティニュー: $1000 に 1000 バイト、続けて BAR / BTC / BFC の $4000 に 1048 バイト。
        // HD63450 のデータシートでは、コンティニューの DONE はブロックごとに出る。FDC には 1000 バイトで TC が届き、
        // FDC は R=7 の終わりで止まる (結果は次の R=8)。DMAC は 2 つ目のブロックに移るが、FDC はもう要求を出さないので、
        // 2 つ目のブロックには何も入らない (試験の最後のソフトウェアアボートで止める)
        ReadCase& c = add(twoSectors);
        c.name = "continue";
        c.mar = 0x1000;
        c.count = 1000;
        c.ccr = 0xc8;
        c.setup = [](x68k::Dmac& d, TestBus&) {
            dmacWrite32(d, 0, 0x1c, 0x4000);
            dmacWrite16(d, 0, 0x1a, 1048);
            d.write(0x39, m68k::FC_SUPER_DATA);
        };
        c.check = [](const ReadOutcome& o) {
            CHECK_EQ(xdfMismatch(o.mem + 0x1000, 5, 7, 0, 1000), 0);
            CHECK_EQ(o.mem[0x1000 + 1000], untouched(0x1000 + 1000));
            CHECK_EQ(o.mem[0x4000], untouched(0x4000));
            CHECK_EQ(o.dmacRegs[0x07] & 0x40, 0x00); // CNT は下りた
            CHECK_EQ(o.dmacRegs[0x29], m68k::FC_SUPER_DATA);
            CHECK_EQ(outReg32(o, 0x0c), 0x4000); // 2 つ目のブロックは始まったところ
            CHECK_EQ(outReg16(o, 0x0a), 1048);
            CHECK_EQ(o.done, 1); // 1 つ目のブロックの終わり
            CHECK_EQ(o.errors, 0);
            CHECK_EQ(o.resultLen, 7);
            CHECK_EQ(o.result[0] & 0xc0, 0x00);
            CHECK_EQ(o.result[3], 2);
            CHECK_EQ(o.result[5], 8);
        };
    }
    {
        // REQG = 11: 最初の 1 オペランドは要求を待たずに転送する (FDC はまだコマンドを受けていないので $00 が入る)。
        // そのあとは外部要求で、R=7 の 1 セクタが $1001 から入る
        ReadCase& c = add(twoSectors);
        c.name = "reqg 11";
        c.mar = 0x1000;
        c.count = 1025;
        c.ocr = 0x83;
        c.runBeforeCommand = true;
        c.check = [](const ReadOutcome& o) {
            CHECK_EQ(o.mem[0x1000], 0x00);
            CHECK_EQ(xdfMismatch(o.mem + 0x1001, 5, 7, 0, 1024), 0);
            CHECK_EQ(o.mem[0x1401], untouched(0x1401));
            CHECK_EQ(o.dmacRegs[0x00] & 0xf8, 0x80);
            CHECK_EQ(o.done, 1);
            CHECK_EQ(o.result[0] & 0xc0, 0x00);
            CHECK_EQ(o.result[5], 8); // R=7 の終わりで TC
        };
    }
    {
        // HLT: 300 バイト転送したところで止め、下ろして続ける
        ReadCase& c = add(twoSectors);
        c.name = "hlt";
        c.mar = 0x1000;
        c.count = 2048;
        c.budget = 100;
        c.haltAfter = 3;
        c.check = [](const ReadOutcome& o) {
            CHECK_EQ(xdfMismatch(o.mem + 0x1000, 5, 7, 0, 2048), 0);
            CHECK_EQ(o.dmacRegs[0x00] & 0xf8, 0x80);
            CHECK_EQ(o.dmacRegs[0x07] & 0x20, 0x00);
            CHECK_EQ(o.done, 1);
            CHECK_EQ(o.result[0] & 0xc0, 0x00);
        };
    }
    {
        // MTC = 1 で、1 回の量も 1: まとめた転送には聞かず (最後のバイトは 1 バイトずつの処理)、1 バイトで終わる
        ReadCase& c = add(twoSectors);
        c.name = "mtc 1, budget 1";
        c.mar = 0x1000;
        c.count = 1;
        c.budget = 1;
        c.expectBlocks = false;
        c.check = [](const ReadOutcome& o) {
            CHECK_EQ(o.mem[0x1000], 5); // セクタの先頭のバイト (トラック)
            CHECK_EQ(o.mem[0x1001], untouched(0x1001));
            CHECK_EQ(o.dmacRegs[0x00] & 0xf8, 0x80);
            CHECK_EQ(outReg32(o, 0x0c), 0x1001);
            CHECK_EQ(o.done, 1);
            CHECK_EQ(o.result[0] & 0xc0, 0x00);
            CHECK_EQ(o.result[5], 8); // セクタの途中の TC: 結果は次のセクタ
        };
    }
    {
        // MTC = 2 で、1 回の量は 1: 1 バイト目はまとめた転送 (1 バイト)、2 バイト目は 1 バイトずつの処理
        ReadCase& c = add(twoSectors);
        c.name = "mtc 2, budget 1";
        c.mar = 0x1000;
        c.count = 2;
        c.budget = 1;
        c.check = [](const ReadOutcome& o) {
            CHECK_EQ(o.mem[0x1000], 5);
            CHECK_EQ(o.mem[0x1001], 7); // 2 バイト目は R
            CHECK_EQ(o.mem[0x1002], untouched(0x1002));
            CHECK_EQ(o.dmacRegs[0x00] & 0xf8, 0x80);
            CHECK_EQ(o.done, 1);
        };
    }

    // ---- FDC の側で止まる場合 (D88: 256 バイトのセクタ、R=1 から EOT=3 まで、DMA は 3 セクタ分) ----
    const ReadCase d88Read = {"", {0x46, 0x00, 0x00, 0x00, 0x01, 0x01, 0x03, 0x1b, 0xff}, 0x1000, 768, 0, none, false};
    {
        // R=2 がデータの CRC エラー: R=2 を転送したところで異常終了する (ST1 の DE、ST2 の DD)
        ReadCase& c = add(d88Read);
        c.name = "crc error";
        c.image = ReadImage::D88_CRC;
        c.check = [](const ReadOutcome& o) {
            checkStoppedAtR2(o);
            CHECK_EQ(o.result[1], 0x20);
            CHECK_EQ(o.result[2], 0x20);
        };
    }
    {
        // 同じ読み込みを、セクタの途中で 1 回の量が終わるように分ける
        ReadCase& c = add(cases[n - 1]);
        c.name = "crc error, budget 100";
        c.budget = 100;
    }
    {
        // R=2 がデリーテッドデータで SK = 0: R=2 を転送したところで異常終了する (ST2 の CM)
        ReadCase& c = add(d88Read);
        c.name = "deleted data, sk 0";
        c.image = ReadImage::D88_DELETED;
        c.check = [](const ReadOutcome& o) {
            checkStoppedAtR2(o);
            CHECK_EQ(o.result[1], 0x00);
            CHECK_EQ(o.result[2], 0x40);
        };
    }
    {
        // SK = 1: R=2 を飛ばして R=1 と R=3 を転送する (DMA は 2 セクタ分で、R=3 の終わりで TC)
        ReadCase& c = add(d88Read);
        c.name = "deleted data, sk 1";
        c.image = ReadImage::D88_DELETED;
        c.cmd[0] = 0x66;
        c.count = 512;
        c.check = [](const ReadOutcome& o) {
            CHECK_EQ(fillMismatch(o.mem + 0x1000, 0x41, 256), 0);
            CHECK_EQ(fillMismatch(o.mem + 0x1100, 0x43, 256), 0);
            CHECK_EQ(o.mem[0x1200], untouched(0x1200));
            CHECK_EQ(o.dmacRegs[0x00] & 0xf8, 0x80);
            CHECK_EQ(o.done, 1);
            CHECK_EQ(o.result[0] & 0xc0, 0x00);
            CHECK_EQ(o.result[2], 0x40);
            CHECK_EQ(o.result[3], 1); // EOT の終わりの TC: 次のシリンダの R=1
            CHECK_EQ(o.result[5], 1);
        };
    }
    {
        // SK = 1 で、1 回の量を飛ばすセクタの手前で終える (次の run() の最初にセクタを飛ばす)
        ReadCase& c = add(cases[n - 1]);
        c.name = "deleted data, sk 1, budget 256";
        c.budget = 256;
    }
    {
        // READ DELETED DATA で SK = 1: 通常のデータの R=1 を飛ばして R=2 だけを転送する
        ReadCase& c = add(d88Read);
        c.name = "read deleted data, sk 1";
        c.image = ReadImage::D88_DELETED;
        c.cmd[0] = 0x6c;
        c.count = 256;
        c.check = [](const ReadOutcome& o) {
            CHECK_EQ(fillMismatch(o.mem + 0x1000, 0x42, 256), 0);
            CHECK_EQ(o.mem[0x1100], untouched(0x1100));
            CHECK_EQ(o.dmacRegs[0x00] & 0xf8, 0x80);
            CHECK_EQ(o.done, 1);
            CHECK_EQ(o.result[0] & 0xc0, 0x00);
            CHECK_EQ(o.result[2], 0x40);
            CHECK_EQ(o.result[5], 3);
        };
    }

    static ReadOutcome a, b;
    for (int i = 0; i < n; i++) {
        const ReadCase& c = cases[i];
        const int before = failures;
        runReadCase(c, false, a);
        runReadCase(c, true, b);
        CHECK_EQ(memcmp(a.mem, b.mem, sizeof a.mem), 0);
        CHECK_EQ(memcmp(a.dmacRegs, b.dmacRegs, sizeof a.dmacRegs), 0);
        CHECK_EQ(a.resultLen, b.resultLen);
        CHECK_EQ(memcmp(a.result, b.result, sizeof a.result), 0);
        CHECK_EQ(a.done, b.done);
        CHECK_EQ(a.errors, b.errors);
        CHECK_EQ(a.fdcIrq, b.fdcIrq);
        CHECK_EQ(a.dmacIrq, b.dmacIrq);
        CHECK_EQ(b.blocks > 0, c.expectBlocks); // まとめた転送を使ったか
        if (c.check) {
            c.check(a);
            c.check(b);
        }
        if (failures != before) printf("  case: %s\n", c.name);
    }
}

int main() {
    makeXdf();
    testXdfImage();
    testD88Image();
    testDimImage();
    testReadData();
    testEndOfCylinder();
    testErrors();
    testWriteData();
    testScan();
    testMemoryToMemoryChain();
    testBrokenD88();
    testD88Header2a0();
    testDimFlags();
    testDmaReentry();
    testWordTransfer();
    testBusErrorDuringWrite();
    testSwapDuringWrite();
    testSoftwareAbort();
    testChannelPriority();
    testEjectRequest();
    testDeletedData();
    testScanDeletedAndCrc();
    testMultiTrackResult();
    testSkipAcrossHeads();
    testD88Header2a0WithoutTrack0();
    testDmacStartModes();
    testLinkChainRing();
    testChainRegisters();
    testWriteId();
    testChainStateVersion();
    testChainStateVersionCases();
    testBlockRead();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
