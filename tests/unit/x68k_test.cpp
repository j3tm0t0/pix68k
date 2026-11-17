// X68000 のメモリマップと周辺機器 (M2) のテスト。本物の ROM は使わず、テスト用の IPL ROM を組み立てて動かす
#include <stdio.h>
#include <string.h>

#include <initializer_list>

#include "x68k/machine.h"
#include "x68k/max_speed.h"
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

// テスト用の IPL ROM ($FE0000-$FFFFFF)
struct Rom {
    uint8_t b[x68k::Machine::IPL_SIZE];
    uint32_t pos = 0x100; // 次に命令を置く位置 ($FE0100 から)
    Rom() {
        memset(b, 0xff, sizeof b);
        put32(0x10000, 0x00002000); // $FF0000: SSP
        put32(0x10004, 0x00fe0100); // $FF0004: PC
        // バスエラーの処理: D7 = 1 にして止まる
        put16(0x0f00, 0x7e01); // moveq #1,d7
        put16(0x0f02, 0x60fe); // bra.s *
        // MFP の割り込み (ベクタ $45) の処理: D5 = 1 にして止まる
        put16(0x0e00, 0x7a01); // moveq #1,d5
        put16(0x0e02, 0x60fe);
        // 最初にバスエラーのベクタを設定する
        code({0x21fc, 0x00fe, 0x0f00, 0x0008}); // move.l #$fe0f00,$8.w
    }
    void put16(uint32_t o, uint16_t v) {
        b[o] = (uint8_t)(v >> 8);
        b[o + 1] = (uint8_t)v;
    }
    void put32(uint32_t o, uint32_t v) {
        put16(o, (uint16_t)(v >> 16));
        put16(o + 2, (uint16_t)v);
    }
    void code(std::initializer_list<uint16_t> words) {
        for (uint16_t w : words) {
            put16(pos, w);
            pos += 2;
        }
    }
    // 最後まで来たら D6 = 1 にして止まる
    void end() { code({0x7c01, 0x60fe}); }
};

struct Result {
    uint32_t d5, d6, d7;
};

Result runRom(x68k::Machine& m, Rom& rom, int64_t cycles = 20000) {
    rom.end();
    m.setIplRom(rom.b, sizeof rom.b);
    m.powerOn();
    m.run(cycles);
    m68k::Registers r;
    m.getRegisters(r);
    return {r.d[5], r.d[6], r.d[7]};
}

// 1 命令だけ実行して、バスエラーになったかを返す
bool causesBusError(std::initializer_list<uint16_t> insn) {
    x68k::Machine m;
    Rom rom;
    rom.code(insn);
    Result r = runRom(m, rom);
    if (r.d7 == 0 && r.d6 == 0) {
        printf("  program did not finish\n");
        failures++;
    }
    return r.d7 == 1;
}

void testResetVector() {
    x68k::Machine m;
    Rom rom;
    runRom(m, rom, 0);
    m68k::Registers r;
    m.cpu().getRegisters(r);
    CHECK_EQ(r.ssp, 0x2000);
    CHECK_EQ(r.pc, 0xfe0100);
}

void testMemoryMapPeek() {
    x68k::Machine m;
    Rom rom;
    rom.b[0] = 0x12;
    m.setIplRom(rom.b, sizeof rom.b);
    uint8_t cg[x68k::Machine::CG_SIZE];
    memset(cg, 0, sizeof cg);
    cg[0] = 0x34;
    cg[sizeof cg - 1] = 0x56;
    CHECK_EQ(m.setCgRom(cg, sizeof cg), true);
    CHECK_EQ(m.setCgRom(cg, 100), false); // 大きさが違う
    bool ok;
    CHECK_EQ(m.peek8(0xfe0000, ok), 0x12);
    CHECK_EQ(ok, true);
    CHECK_EQ(m.peek8(0xf00000, ok), 0x34);
    CHECK_EQ(m.peek8(0xfbffff, ok), 0x56);
    m.peek8(0xfc0000, ok); // 内蔵 SCSI の ROM はない
    CHECK_EQ(ok, false);
    m.peek8(0x3fffff, ok); // RAM 4MB の最後
    CHECK_EQ(ok, true);
    m.peek8(0x400000, ok); // RAM の外
    CHECK_EQ(ok, false);
    m.peek8(0xe00000, ok); // テキスト VRAM
    CHECK_EQ(ok, true);
    m.peek8(0xffffff, ok);
    CHECK_EQ(ok, true);
}

// 68000 の機種のメモリ: メイン RAM は 12MB まで ($000000-$BFFFFF)。アドレスバスは 24 ビットなので、ハイメモリを
// 頼んでも置かず ($01000000 は $000000 と同じ場所)、そこはバスエラーにならない
void testMemory68000() {
    x68k::Config cfg;
    cfg.ramSize = 12 << 20;
    cfg.highMemSize = 16 << 20; // 68000 では使わない
    x68k::Machine m(cfg);
    CHECK_EQ(m.ok(), true);
    Rom rom;
    rom.code({0x23fc, 0x1234, 0x5678, 0x0100, 0x0010}); // move.l #$12345678,$01000010
    rom.code({0x2038, 0x0010});                         // move.l $10.w,d0
    rom.code({0x1239, 0x00bf, 0xffff});                 // move.b $bfffff,d1 (12MB の最後)
    rom.code({0x2439, 0xff00, 0x0010});                 // move.l $ff000010,d2 (上の 8 ビットは出ない)
    Result r = runRom(m, rom);
    CHECK_EQ(r.d7, 0);
    CHECK_EQ(r.d6, 1);
    m68k::Registers regs;
    m.getRegisters(regs);
    CHECK_EQ(regs.d[0], 0x12345678);
    CHECK_EQ(regs.d[2], 0x12345678);
    bool ok;
    m.peek8(0xbfffff, ok);
    CHECK_EQ(ok, true);
}

void testBusErrors() {
    // RAM の外 (4MB の機械で $400000)
    CHECK_EQ(causesBusError({0x1039, 0x0040, 0x0000}), true); // move.b $400000,d0
    CHECK_EQ(causesBusError({0x1039, 0x003f, 0xffff}), false); // move.b $3fffff,d0
    // ROM への書き込み
    CHECK_EQ(causesBusError({0x13c0, 0x00fe, 0x0000}), true); // move.b d0,$fe0000
    CHECK_EQ(causesBusError({0x13c0, 0x00f0, 0x0000}), true); // move.b d0,$f00000 (CGROM)
    // ボードがない I/O
    CHECK_EQ(causesBusError({0x1039, 0x00ea, 0x0000}), true); // 外付け SCSI
    CHECK_EQ(causesBusError({0x1039, 0x00e9, 0xe000}), true); // 数値演算プロセッサ
    CHECK_EQ(causesBusError({0x1039, 0x00ea, 0xfa01}), true); // MIDI
    CHECK_EQ(causesBusError({0x1039, 0x00ec, 0x0000}), true); // ユーザー I/O
    CHECK_EQ(causesBusError({0x1039, 0x00ed, 0x4000}), true); // SRAM の外
    // つながっている I/O
    CHECK_EQ(causesBusError({0x1039, 0x00e8, 0x8001}), false); // MFP
    CHECK_EQ(causesBusError({0x1039, 0x00e8, 0xa001}), false); // RTC
    CHECK_EQ(causesBusError({0x1039, 0x00e8, 0xe00b}), false); // システムポート
    CHECK_EQ(causesBusError({0x1039, 0x00e9, 0x4001}), false); // FDC
    CHECK_EQ(causesBusError({0x1039, 0x00ed, 0x3fff}), false); // SRAM
    CHECK_EQ(causesBusError({0x1039, 0x00eb, 0x0000}), false); // スプライト
    // ユーザーモードからの I/O
    CHECK_EQ(causesBusError({0x027c, 0xdfff, 0x1039, 0x00e8, 0x8001}), true); // andi #$dfff,sr; move.b $e88001,d0
}

void testSramWriteProtect() {
    x68k::Machine m;
    Rom rom;
    rom.code({0x13fc, 0x005a, 0x00ed, 0x0010}); // move.b #$5a,$ed0010 (書き込み禁止なので無視される)
    rom.code({0x13fc, 0x0031, 0x00e8, 0xe00d}); // move.b #$31,$e8e00d (書き込み許可)
    rom.code({0x13fc, 0x00a5, 0x00ed, 0x0011}); // move.b #$a5,$ed0011
    Result r = runRom(m, rom);
    CHECK_EQ(r.d6, 1);
    CHECK_EQ(r.d7, 0);
    CHECK_EQ(m.sram()[0x10], 0x00);
    CHECK_EQ(m.sram()[0x11], 0xa5);
    CHECK_EQ(m.sramDirty(), true);
}

// SRAM のメインメモリの容量 ($ED0008) は、電源投入でつないだ容量 (既定 4MB) に合わせる
// (XEiJ の説明書の modifymemorysize)。合わせないと IPL ROM の初期値 (1MB) のままになり、大きなプログラムがメモリ不足で実行できない
void testSramMemorySize() {
    // 初期化済みの SRAM (1MB) は、電源投入で 4MB に直る
    {
        x68k::Machine m;
        static const uint8_t kHead[12] = {0x82, 0x77, '6', '8', '0', '0', '0', 'W', 0x00, 0x10, 0x00, 0x00};
        memcpy(m.sram(), kHead, sizeof kHead);
        m.sram()[0x0c] = 0x00;
        m.sram()[0x0d] = 0xbf;
        Rom rom;
        runRom(m, rom, 0);
        CHECK_EQ(m.sram()[8], 0x00);
        CHECK_EQ(m.sram()[9], 0x40);
        CHECK_EQ(m.sram()[10], 0x00);
        CHECK_EQ(m.sram()[11], 0x00);
        CHECK_EQ(m.sram()[0x0d], 0xbf); // ほかは変えない
        CHECK_EQ(m.sramDirty(), true);
    }
    // 空の SRAM: IPL ROM が初期値 (1MB) を書くとき、容量だけ 4MB にする。そのあとの書き込みはそのまま
    {
        x68k::Machine m;
        Rom rom;
        rom.code({0x13fc, 0x0031, 0x00e8, 0xe00d});                 // move.b #$31,$e8e00d
        rom.code({0x23fc, 0x8277, 0x3638, 0x00ed, 0x0000});         // move.l #$82773638,$ed0000
        rom.code({0x23fc, 0x3030, 0x3057, 0x00ed, 0x0004});         // move.l #'000W',$ed0004
        rom.code({0x23fc, 0x0010, 0x0000, 0x00ed, 0x0008});         // move.l #$00100000,$ed0008
        rom.code({0x23fc, 0x0010, 0x0000, 0x00ed, 0x000c});         // move.l #$00100000,$ed000c
        rom.code({0x13fc, 0x0020, 0x00ed, 0x0009});                 // move.b #$20,$ed0009 (SWITCH.X などで 2MB に)
        Result r = runRom(m, rom);
        CHECK_EQ(r.d7, 0);
        CHECK_EQ(m.sram()[0], 0x82);
        CHECK_EQ(m.sram()[8], 0x00);
        CHECK_EQ(m.sram()[9], 0x20);
        CHECK_EQ(m.sram()[0x0d], 0x10);
        // 書かれた 2MB は、次の電源投入で 4MB に直る
        m.powerOn();
        CHECK_EQ(m.sram()[9], 0x40);
    }
    // 空の SRAM で、識別子 ("\x82\x77" "68000W") より先に容量が書かれた場合も 4MB に差し替える。
    // 識別子がまだないので初期化は終わったとみなさず、続けて書かれた値も差し替える
    {
        x68k::Machine m; // 既定の構成 (メインメモリ 4MB、容量を合わせる)
        Rom rom;         // 書き込みを許可して、識別子を書かずに容量を書く
        // 許可の前 ($E8E00D に $31 を書く前) の書き込みは無視され、差し替えも終わらせない
        rom.code({0x23fc, 0x0010, 0x0000, 0x00ed, 0x0008}); // move.l #$00100000,$ed0008 (書き込み禁止の間)
        rom.code({0x13fc, 0x0031, 0x00e8, 0xe00d});         // move.b #$31,$e8e00d
        rom.code({0x23fc, 0x0010, 0x0000, 0x00ed, 0x0008}); // move.l #$00100000,$ed0008
        rom.code({0x13fc, 0x0020, 0x00ed, 0x0009});         // move.b #$20,$ed0009
        CHECK_EQ(runRom(m, rom).d7, 0);
        const uint8_t* sizeBytes = m.sram() + 8;
        CHECK_EQ(sizeBytes[0], 0x00);
        CHECK_EQ(sizeBytes[1], 0x40);
        CHECK_EQ(sizeBytes[2], 0x00);
        CHECK_EQ(sizeBytes[3], 0x00);
        CHECK_EQ(m.sramDirty(), 1);
    }
    // sram_memsize=keep (Config::fixSramMemorySize が false) なら、初期化済みの SRAM の 1MB は電源投入のあとも 1MB で、
    // SRAM は書き換えない (書き戻しも起きない)
    {
        x68k::Config keepCfg;
        keepCfg.fixSramMemorySize = false;
        x68k::Machine m(keepCfg);
        static const uint8_t kInit1M[12] = {0x82, 0x77, '6', '8', '0', '0', '0', 'W', 0x00, 0x10, 0x00, 0x00};
        memcpy(m.sram(), kInit1M, sizeof kInit1M);
        Rom rom; // 命令は置かない
        runRom(m, rom, 0); // 電源投入だけ
        for (int k = 0; k < 4; k++) CHECK_EQ(m.sram()[8 + k], kInit1M[8 + k]);
        CHECK_EQ(m.sramDirty(), 0);
    }
    // keep で空の SRAM なら、IPL ROM が書く初期値 (1MB) をそのまま残す
    {
        x68k::Config keepCfg;
        keepCfg.fixSramMemorySize = false;
        x68k::Machine m(keepCfg);
        Rom rom; // IPL ROM と同じく、識別子を書いてから容量を書く
        rom.code({0x13fc, 0x0031, 0x00e8, 0xe00d});         // move.b #$31,$e8e00d
        rom.code({0x23fc, 0x8277, 0x3638, 0x00ed, 0x0000}); // move.l #$82773638,$ed0000
        rom.code({0x23fc, 0x3030, 0x3057, 0x00ed, 0x0004}); // move.l #'000W',$ed0004
        rom.code({0x23fc, 0x0010, 0x0000, 0x00ed, 0x0008}); // move.l #$00100000,$ed0008
        CHECK_EQ(runRom(m, rom).d7, 0);
        static const uint8_t kOneMB[4] = {0x00, 0x10, 0x00, 0x00};
        for (int k = 0; k < 4; k++) CHECK_EQ(m.sram()[8 + k], kOneMB[k]);
    }
}

void testCpuInfo() {
    x68k::Machine m;
    Rom rom;
    rom.code({0x1039, 0x00e8, 0xe00b}); // move.b $e8e00b,d0
    runRom(m, rom);
    m68k::Registers r;
    m.cpu().getRegisters(r);
    CHECK_EQ(r.d[0] & 0xff, 0xfe); // XVI (16MHz)
}

void testMfpTimerInterrupt() {
    // タイマ C (4MHz / 200 / 200 = 100Hz) の割り込みがベクタ $45 で入る
    x68k::Machine m;
    Rom rom;
    rom.code({0x21fc, 0x00fe, 0x0e00, 0x0114}); // move.l #$fe0e00,$114.w (ベクタ $45)
    rom.code({0x13fc, 0x0040, 0x00e8, 0x8017}); // VR = $40
    rom.code({0x13fc, 0x0020, 0x00e8, 0x8009}); // IERB = $20 (タイマ C)
    rom.code({0x13fc, 0x0020, 0x00e8, 0x8015}); // IMRB = $20
    rom.code({0x13fc, 0x00c8, 0x00e8, 0x8023}); // TCDR = 200
    rom.code({0x13fc, 0x0070, 0x00e8, 0x801d}); // TCDCR = $70 (プリスケーラ 200)
    rom.code({0x46fc, 0x2000});                 // move.w #$2000,sr (割り込みを許可)
    rom.code({0x60fe});                         // bra.s *
    Result r = runRom(m, rom, 16000000 / 200); // 5ms: まだ入らない
    CHECK_EQ(r.d5, 0);
    m.run(16000000 / 100); // さらに 10ms
    m68k::Registers regs;
    m.cpu().getRegisters(regs);
    CHECK_EQ(regs.d[5], 1);
}

// 動いているタイマのプリスケーラの分周比を変える。MC68901 のデータシート (Timers、Delay Mode): 最初のタイムアウトは
// 1 から 200 クロックの間の決まらない時刻、そのあとは正しい間隔。数えかけが新しい分周比を超えていても、
// カウンタが一度にいくつも進むことはない
void testMfpPrescalerChange() {
    x68k::Mfp mfp{};
    mfp.reset(); // レジスタを初期値にする (タイマは止まっている)
    const int TACR = 12, TCDCR = 14, TADR = 15, TCDR = 17, TDDR = 18;
    mfp.write(TADR, 100);
    mfp.write(TACR, 0x07); // 1/200 で動かす: 150 クロックではまだ 1 回も数えない
    mfp.advance(150);
    CHECK_EQ(mfp.read(TADR), 100);
    mfp.write(TACR, 1); // 1/4: 数えかけの 150 は 4 を超えているので、数え直す
    mfp.advance(3);
    CHECK_EQ(mfp.read(TADR), 100);
    mfp.advance(1); // 1/4 で数え直して 4 クロック目
    CHECK_EQ(mfp.read(TADR), 99);
    // 大きい分周比に変えるときは、数えかけを引き継ぐ
    mfp.advance(3);
    mfp.write(TACR, 2); // 1/10
    mfp.advance(6);
    CHECK_EQ(mfp.read(TADR), 99);
    mfp.advance(1); // 引き継いだ 3 と合わせて 10 クロック目
    CHECK_EQ(mfp.read(TADR), 98);
    // そのあとは正しい間隔
    mfp.advance(10);
    CHECK_EQ(mfp.read(TADR), 97);
    // 数えかけが新しい分周比とちょうど同じときも、数え直す
    mfp.write(TACR, 0);
    mfp.write(TADR, 100);
    mfp.write(TACR, 7); // 1/200
    mfp.advance(10);
    mfp.write(TACR, 2); // 1/10: 数えかけは 10
    mfp.advance(9);
    CHECK_EQ(mfp.read(TADR), 100);
    mfp.advance(1);
    CHECK_EQ(mfp.read(TADR), 99);
    // 1 つ手前 (9) なら引き継いで、次の 1 クロックで数える
    mfp.write(TACR, 7);
    mfp.advance(9);
    mfp.write(TACR, 2);
    mfp.advance(1);
    CHECK_EQ(mfp.read(TADR), 98);
    // タイマ C / D (TCDCR)
    mfp.write(TCDR, 50);
    mfp.write(TDDR, 60);
    mfp.write(TCDCR, 0x77); // どちらも 1/200
    mfp.advance(199);
    mfp.write(TCDCR, 0x15); // C は 1/4、D は 1/64
    mfp.advance(3);
    CHECK_EQ(mfp.read(TCDR), 50);
    CHECK_EQ(mfp.read(TDDR), 60);
    mfp.advance(1);
    CHECK_EQ(mfp.read(TCDR), 49);
    CHECK_EQ(mfp.read(TDDR), 60);
    mfp.advance(60);
    CHECK_EQ(mfp.read(TDDR), 59);
}

void testFrames() {
    // V-DISP が 1 フレームに 1 回立ち上がり、GPIP4 に見える
    x68k::Machine m;
    Rom rom;
    rom.code({0x60fe});
    runRom(m, rom, 0);
    uint64_t f0 = m.frameCount();
    m.runFrame();
    m.runFrame();
    CHECK_EQ(m.frameCount() - f0, 2);
    // 1 フレームの長さ (55.46Hz) がおおむね合っている
    uint64_t c0 = m.cpu().cycles();
    m.runFrame();
    uint64_t c = m.cpu().cycles() - c0;
    CHECK_EQ(c > 16000000 / 56 && c < 16000000 / 55, true);
}

// 68000 の速さ (Machine::setCpuSpeed): 1 フレームに実行する命令の数は速さに比例して増え、1 フレームの長さ
// (X68000 のクロックのサイクル数) と周辺機器の時間は変わらない。速さ 100 に戻せば前と同じに動く
void testCpuSpeed68000() {
    x68k::Machine m;
    Rom rom;
    // 数えるループ。I/O (MFP の GPIP) を読むので、区切りの途中でも周辺機器を進める
    rom.code({0x5280,                 // addq.l #1,d0
              0x4a39, 0x00e8, 0x8001, // tst.b $e88001
              0x60f6});               // bra.s 先頭
    runRom(m, rom, 0);
    CHECK_EQ(m.cpuSpeed(), 100);
    m.runFrame();
    auto frame = [&](uint64_t& cycles) {
        m68k::Registers r;
        m.getRegisters(r);
        const uint32_t d0 = r.d[0];
        const uint64_t c0 = m.cycles(), f0 = m.frameCount();
        m.runFrame();
        CHECK_EQ(m.frameCount() - f0, 1);
        cycles = m.cycles() - c0;
        m.getRegisters(r);
        return r.d[0] - d0;
    };
    uint64_t c100, c300, cBack;
    const uint32_t n100 = frame(c100);
    m.setCpuSpeed(300);
    CHECK_EQ(m.cpuSpeed(), 300);
    m.runFrame(); // フレームの途中で変えた分を除く
    const uint32_t n300 = frame(c300);
    // 1 フレームの長さはサイクル数で数えて変わらない (runFrame() はラスタ 1 本分ずつ進めるので、
    // フレームの終わりがラスタ 1 本分 (約 1000 サイクル) ほどずれてよい)
    const uint64_t tol = c100 / 100;
    CHECK_EQ(c300 + tol > c100 && c300 < c100 + tol, true);
    // 命令の数はおよそ 3 倍
    CHECK_EQ(n300 > n100 * 29 / 10 && n300 < n100 * 31 / 10, true);
    // 割り切れない速さと上限でも、命令の数は速さに比例し、フレームの長さは変わらない (どちらも 2% まで)
    for (int sp : {137, x68k::Machine::SPEED_MAX}) {
        m.setCpuSpeed(sp);
        CHECK_EQ(m.cpuSpeed(), sp);
        m.runFrame();
        uint64_t c;
        const uint64_t n = frame(c);
        const uint64_t want = (uint64_t)n100 * (uint64_t)sp / 100;
        CHECK_EQ(n * 50 > want * 49 && n * 50 < want * 51, true);
        CHECK_EQ(c + tol > c100 && c < c100 + tol, true);
    }
    // 範囲の外は丸める
    m.setCpuSpeed(50);
    CHECK_EQ(m.cpuSpeed(), 100);
    m.setCpuSpeed(100000);
    CHECK_EQ(m.cpuSpeed(), x68k::Machine::SPEED_MAX);
    m.setCpuSpeed(100);
    m.runFrame();
    const uint32_t nBack = frame(cBack);
    CHECK_EQ(cBack + tol > c100 && cBack < c100 + tol, true);
    CHECK_EQ(nBack + n100 / 100 > n100 && nBack < n100 + n100 / 100, true);
}

// 68000 の速さを変えて動かしたとき、STOP で待っている間は X68000 の時間のまま進めるが、割り込みで起きたあとの
// 割り込みの処理と命令には速さを掛ける
void testCpuSpeedStopWake() {
    x68k::Machine m;
    Rom rom;
    rom.code({0x4e72, 0x2700}); // stop #$2700 (割り込みはレベル 7 (NMI) だけ受け付ける)
    runRom(m, rom, 2000);
    CHECK_EQ(m.cpu().stopped(), true);
    m.setCpuSpeed(x68k::Machine::SPEED_MAX);
    // 割り込みがなければ、待っている間も X68000 の時間で進む
    uint64_t c0 = m.cycles();
    m.run(1000);
    CHECK_EQ(m.cycles() - c0 >= 1000 && m.cycles() - c0 < 1010, true);
    CHECK_EQ(m.cpu().stopped(), true);
    // NMI で起きる: 割り込みの処理 (44 サイクル) は 1/32 になる
    m.cpu().setIPL(7);
    c0 = m.cycles();
    m.run(1);
    CHECK_EQ(m.cpu().stopped(), false);
    CHECK_EQ(m.cycles() - c0 <= 2, true);
}

// 68000 の速さを変えて動かしたとき、区切りの途中で I/O を読んでも周辺機器の時間は戻らない。
// MFP のタイマ A (1/4、1MHz で減る) のカウンタを読み続け、値が増える (時間が戻る) ことがないかを見る
void testCpuSpeedMonotonic() {
    for (int sp : {137, 300, x68k::Machine::SPEED_MAX}) {
        x68k::Machine m;
        Rom rom;
        rom.code({0x13fc, 0x0000, 0x00e8, 0x801f, // move.b #0,$e8801f (TADR。0 は 256)
                  0x13fc, 0x0001, 0x00e8, 0x8019, // move.b #1,$e88019 (TACR: ディレイモード、1/4)
                  0x41f9, 0x0001, 0x0000,         // lea $10000,a0
                  0x323c, 0x0f9f,                 // move.w #3999,d1
                  0x10f9, 0x00e8, 0x801f,         // move.b $e8801f,(a0)+
                  0x51c9, 0xfff8});               // dbra d1,前の行
        runRom(m, rom, 0);
        m.setCpuSpeed(sp);
        for (int i = 0; i < 100; i++) {
            m68k::Registers r;
            m.getRegisters(r);
            if (r.d[6] == 1) break;
            m.run(100000);
        }
        m68k::Registers r;
        m.getRegisters(r);
        CHECK_EQ(r.d[6], 1);
        bool ok = true;
        unsigned prev = 0, total = 0, bad = 0;
        for (uint32_t i = 0; i < 4000; i++) {
            const unsigned v = m.peek8(0x10000 + i, ok);
            if (i > 0) {
                const unsigned down = (prev - v) & 0xff; // 減った数 (256 で 1 周する)
                if (down >= 64) bad++;                  // 増えた (時間が戻った) か、飛びすぎた
                total += down;
            }
            prev = v;
        }
        CHECK_EQ(ok, true);
        CHECK_EQ(bad, 0u);
        CHECK_EQ(total > 0, true);
    }
}

// 68000 の速さを変えて動かしたときの時間の端数はステートに入る: 同じ速さの別の Machine に読めば、同じ時刻で進む
void testCpuSpeedState() {
    auto setup = [](x68k::Machine& m) {
        Rom rom;
        rom.code({0x5280,                 // addq.l #1,d0
                  0x4a39, 0x00e8, 0x8001, // tst.b $e88001
                  0x60f6});               // bra.s 先頭
        runRom(m, rom, 0);
        m.setCpuSpeed(300);
    };
    x68k::Machine a, b;
    setup(a);
    setup(b);
    a.run(12345);
    for (int i = 0; i < 7; i++) a.run(1); // 端数を 0 でなくする
    const size_t cap = x68k::stateSize(a) + 4096; // 版 4 の並びは、SPC の状態が今より大きい
    uint8_t* buf = new uint8_t[cap];
    size_t n = 0;
    CHECK_EQ(x68k::saveState(a, buf, cap, n), true);
    const char* err = nullptr;
    CHECK_EQ(x68k::loadState(b, buf, n, err), true);
    bool same = true;
    for (int i = 0; i < 20; i++) {
        a.run(1);
        b.run(1);
        if (a.cycles() != b.cycles()) same = false;
    }
    CHECK_EQ(same, true);
    // 版 4 のステート (端数がない) も読める。端数は 0 になる
    CHECK_EQ(x68k::saveState(a, buf, cap, n, 4), true);
    CHECK_EQ(x68k::loadState(b, buf, n, err), true);
    delete[] buf;
}

// 最高速モード (x68k/max_speed.h) を 68000 で使う: 0.25 秒分たまるごとに、ホストの使った時間が X68000 の時間の
// 85% になるよう Machine::setCpuSpeed() で速さを変える。1 回に変える幅は 1/2 倍から 3/2 倍まで、68000 の範囲は
// 100 から SPEED_MAX まで。そのあと runFrame() で 1 フレームに実行する命令が増える
void testMaxSpeed68000() {
    x68k::Machine m;
    Rom rom;
    rom.code({0x5280,                 // addq.l #1,d0
              0x4a39, 0x00e8, 0x8001, // tst.b $e88001
              0x60f6});               // bra.s 先頭
    runRom(m, rom, 0);
    m.runFrame();
    auto frame = [&]() {
        m68k::Registers r;
        m.getRegisters(r);
        const uint32_t d0 = r.d[0];
        m.runFrame();
        m.getRegisters(r);
        return r.d[0] - d0;
    };
    const uint32_t n100 = frame();
    x68k::MaxSpeed ms;
    CHECK_EQ(m.cpuSpeed(), 100);
    // 0.25 秒分たまるまでは変えない
    ms.update(m, 100000, 0);
    ms.update(m, 100000, 0);
    CHECK_EQ(m.cpuSpeed(), 100);
    // ホストが何もしていなければ (負荷 0) 3/2 倍
    ms.update(m, 50000, 0);
    CHECK_EQ(m.cpuSpeed(), 150);
    // 実時間いっぱい使っていれば 85/100 倍 (150 * 85 / 100 = 127.5 を切り捨て)
    ms.update(m, 250000, 250000);
    CHECK_EQ(m.cpuSpeed(), 127);
    // 負荷は 0.25 秒分をまとめて見る: 何回かに分けて、合わせて 250000us のうち 212500us (85%) 使っていれば変えない
    // (最後の 1 回の負荷 42500us だけを見ると、5 倍に上げることになる)
    m.setCpuSpeed(400);
    ms.update(m, 100000, 85000);
    ms.update(m, 100000, 85000);
    CHECK_EQ(m.cpuSpeed(), 400);
    ms.update(m, 50000, 42500);
    CHECK_EQ(m.cpuSpeed(), 400);
    // 1 回に変える幅は 1/2 倍まで: 式の値は 400 * 85 / 1000 = 34 だが、200 で止まる
    ms.update(m, 250000, 250000 * 10);
    CHECK_EQ(m.cpuSpeed(), 200);
    // 1 回に変える幅は 3/2 倍まで: 負荷 10% なら式の値は 400 * 850 / 100 = 3400 だが、600 で止まる
    m.setCpuSpeed(400);
    ms.update(m, 250000, 25000);
    CHECK_EQ(m.cpuSpeed(), 600);
    // 68000 の下限は 100: 150 から 1/2 倍の 75 にはならない
    m.setCpuSpeed(150);
    ms.update(m, 250000, 250000 * 10);
    CHECK_EQ(m.cpuSpeed(), 100);
    // 空いていれば 1 回に 3/2 倍ずつ上がり、SPEED_MAX で止まる
    for (int i = 0; i < 20; i++) {
        const int prev = m.cpuSpeed();
        int want = prev * 3 / 2;
        if (want > x68k::Machine::SPEED_MAX) want = x68k::Machine::SPEED_MAX;
        ms.update(m, 250000, 1);
        CHECK_EQ(m.cpuSpeed(), want);
    }
    CHECK_EQ(m.cpuSpeed(), x68k::Machine::SPEED_MAX);
    // 速さに比例して 1 フレームの命令が増える (区切りの途中で変えた分を除いて 2% まで)
    m.runFrame();
    const uint64_t n = frame();
    const uint64_t want = (uint64_t)n100 * x68k::Machine::SPEED_MAX / 100;
    CHECK_EQ(n * 50 > want * 49 && n * 50 < want * 51, true);
}

void testRtc() {
    x68k::Rtc rtc;
    rtc.reset();
    rtc.setTime(1700000000); // 2023-11-14 22:13:20 (火曜)
    CHECK_EQ(rtc.read(0) & 15, 0);
    CHECK_EQ(rtc.read(1) & 15, 2);
    CHECK_EQ(rtc.read(2) & 15, 3);
    CHECK_EQ(rtc.read(3) & 15, 1);
    CHECK_EQ(rtc.read(4) & 15, 2);
    CHECK_EQ(rtc.read(5) & 15, 2);
    CHECK_EQ(rtc.read(6) & 15, 2); // 火曜
    CHECK_EQ(rtc.read(7) & 15, 4);
    CHECK_EQ(rtc.read(8) & 15, 1);
    CHECK_EQ(rtc.read(9) & 15, 1);
    CHECK_EQ(rtc.read(10) & 15, 1);
    CHECK_EQ(rtc.read(11) & 15, 3); // 2023 - 1980 = 43
    CHECK_EQ(rtc.read(12) & 15, 4);
    // 分の 1 の位を書き換える
    rtc.write(2, 7);
    CHECK_EQ(rtc.read(2) & 15, 7);
    CHECK_EQ(rtc.read(3) & 15, 1);
    rtc.tickSecond();
    CHECK_EQ(rtc.read(0) & 15, 1);
}

// 年月日時分秒を設定する (1980 年より前は 1980 年に丸められる)
int64_t unixTime(int y, int mo, int d, int h, int mi, int s) {
    // 1970 年からの日数 (days_from_civil)
    int64_t yy = y - (mo <= 2 ? 1 : 0);
    int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
    int64_t yoe = yy - era * 400;
    int64_t mp = mo > 2 ? mo - 3 : mo + 9;
    int64_t doy = (153 * mp + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + s;
}

int rtcValue(x68k::Rtc& rtc, int lo) { return (rtc.read(lo + 1) & 15) * 10 + (rtc.read(lo) & 15); }

void testRtcDigitWrites() {
    using R = x68k::Rtc;
    x68k::Rtc rtc;
    rtc.reset();
    // 22 時に、時の 1 の位 = 9、10 の位 = 1 と書くと 19 時 (途中の 29 時で値が壊れない)
    rtc.setTime(unixTime(2023, 11, 14, 22, 0, 0));
    rtc.write(R::HOUR1, 9);
    rtc.write(R::HOUR10, 1);
    CHECK_EQ(rtcValue(rtc, R::HOUR1), 19);
    // 12/31 に、日の 1 の位 = 5、10 の位 = 1 と書くと 12/15
    rtc.setTime(unixTime(2023, 12, 31, 0, 0, 0));
    rtc.write(R::DAY1, 5);
    rtc.write(R::DAY10, 1);
    CHECK_EQ(rtcValue(rtc, R::MON1), 12);
    CHECK_EQ(rtcValue(rtc, R::DAY1), 15);
    // 11/30 に、月 = 02 (10 の位 0、1 の位 2)、日 = 15 と書くと 02/15
    rtc.setTime(unixTime(2023, 11, 30, 0, 0, 0));
    rtc.write(R::MON10, 0);
    rtc.write(R::MON1, 2);
    rtc.write(R::DAY10, 1);
    rtc.write(R::DAY1, 5);
    CHECK_EQ(rtcValue(rtc, R::MON1), 2);
    CHECK_EQ(rtcValue(rtc, R::DAY1), 15);
    // 02/15 に、日 = 31、月 = 03 と書くと 03/31
    rtc.setTime(unixTime(2023, 2, 15, 0, 0, 0));
    rtc.write(R::DAY10, 3);
    rtc.write(R::DAY1, 1);
    rtc.write(R::MON10, 0);
    rtc.write(R::MON1, 3);
    CHECK_EQ(rtcValue(rtc, R::MON1), 3);
    CHECK_EQ(rtcValue(rtc, R::DAY1), 31);
}

void testRtcCarry() {
    using R = x68k::Rtc;
    x68k::Rtc rtc;
    rtc.reset();
    // うるう年の 2 月末 → 3 月 1 日、年末 → 翌年
    rtc.setTime(unixTime(2024, 2, 28, 23, 59, 59));
    rtc.tickSecond();
    CHECK_EQ(rtcValue(rtc, R::MON1), 2);
    CHECK_EQ(rtcValue(rtc, R::DAY1), 29);
    rtc.setTime(unixTime(2023, 2, 28, 23, 59, 59));
    rtc.tickSecond();
    CHECK_EQ(rtcValue(rtc, R::MON1), 3);
    CHECK_EQ(rtcValue(rtc, R::DAY1), 1);
    rtc.setTime(unixTime(2023, 12, 31, 23, 59, 59)); // 日曜
    rtc.tickSecond();
    CHECK_EQ(rtcValue(rtc, R::YEAR1), 44);
    CHECK_EQ(rtcValue(rtc, R::MON1), 1);
    CHECK_EQ(rtcValue(rtc, R::DAY1), 1);
    CHECK_EQ(rtcValue(rtc, R::HOUR1), 0);
    CHECK_EQ(rtc.read(R::WDAY) & 15, 1); // 月曜
    // モードレジスタの bit3 (計時許可) が 0 の間は進まない
    rtc.write(13, 0x00);
    rtc.tickSecond();
    CHECK_EQ(rtcValue(rtc, R::SEC1), 0);
    rtc.write(13, 0x08);
    rtc.tickSecond();
    CHECK_EQ(rtcValue(rtc, R::SEC1), 1);
    // 1980 年より前は 1980-01-01 に丸める
    rtc.setTime(0);
    CHECK_EQ(rtcValue(rtc, R::YEAR1), 0);
    CHECK_EQ(rtcValue(rtc, R::MON1), 1);
    CHECK_EQ(rtcValue(rtc, R::DAY1), 1);
}

void testHsyncInterrupt() {
    // H-SYNC (GPIP7、要因 15) の割り込みが入る
    x68k::Machine m;
    Rom rom;
    rom.code({0x21fc, 0x00fe, 0x0e00, 0x013c}); // move.l #$fe0e00,$13c.w (ベクタ $4f)
    rom.code({0x13fc, 0x0040, 0x00e8, 0x8017}); // VR = $40
    rom.code({0x13fc, 0x0080, 0x00e8, 0x8003}); // AER = $80 (0→1 で反応)
    rom.code({0x13fc, 0x0080, 0x00e8, 0x8007}); // IERA = $80
    rom.code({0x13fc, 0x0080, 0x00e8, 0x8013}); // IMRA = $80
    rom.code({0x46fc, 0x2000});
    rom.code({0x60fe});
    runRom(m, rom, 2000);
    m68k::Registers regs;
    m.cpu().getRegisters(regs);
    CHECK_EQ(regs.d[5], 1);
}

void testLineTiming() {
    // 768x512 (31kHz): 1 ラスタは 1104 ドット / 34.776MHz、1 フレームは 568 ラスタ
    x68k::Machine m;
    Rom rom;
    // R00 を 8 ビットで書ける最大 (255 → 2048 ドット) にする。R00 は bit7-0 のレジスタ (X68k:IOMAP) なので、
    // 幅の外の bit8 は見ない ($1FF と書いても $FF と同じ。#178)
    rom.code({0x33fc, 0x01ff, 0x00e8, 0x0000}); // move.w #$1ff,$e80000
    rom.code({0x60fe});
    runRom(m, rom, 0);
    uint64_t c0 = m.cpu().cycles();
    m.runFrame();
    m.runFrame(); // ここまでで R00 の書き込みが済んでいる
    c0 = m.cpu().cycles();
    m.runFrame();
    uint64_t c = m.cpu().cycles() - c0;
    // 16MHz x 2048 / 34.776MHz x 568 = 535,000 サイクル程度
    CHECK_EQ(c > 528000 && c < 542000, true);
}

void testCrtcMirrorAndRasterCopy() {
    x68k::Video* v = new x68k::Video;
    v->reset();
    // $E80068 は R20 ($E80028) と同じ
    v->crtcWrite(0x68, 0x01);
    v->crtcWrite(0x69, 0x23);
    CHECK_EQ(v->reg(20), 0x0123);
    CHECK_EQ(v->crtcRead(0x829), 0x23); // 0x800 バイトごとのミラー
    // ラスタコピー: bit3 を立てている間、水平帰線期間ごとに R22 の転送をする
    v->tvramWrite(0x000, 0x11); // ラスタ 0
    v->tvramWrite(0x200, 0x22); // ラスタ 1
    v->crtcWrite(0x2b, 0x01);  // R21: プレーン 0 をコピー
    v->crtcWrite(0x2c, 0x00);  // R22: 0 → 2
    v->crtcWrite(0x2d, 0x02);
    v->crtcWrite(0x481, 0x08);
    CHECK_EQ(v->tvramRead(0x400), 0x00); // まだコピーしていない
    v->horizontalBlank();
    CHECK_EQ(v->tvramRead(0x400), 0x11);
    CHECK_EQ(v->crtcRead(0x481) & 0x08, 0x08); // bit3 は残る
    v->crtcWrite(0x2c, 0x01); // R22: 1 → 3
    v->crtcWrite(0x2d, 0x03);
    v->horizontalBlank();
    CHECK_EQ(v->tvramRead(0x600), 0x22);
    v->crtcWrite(0x481, 0x00);
    v->crtcWrite(0x2d, 0x04);
    v->horizontalBlank();
    CHECK_EQ(v->tvramRead(0x800), 0x00); // bit3 を落とすと止まる
    delete v;
}

void testTvramSimultaneousWrite() {
    x68k::Video* v = new x68k::Video;
    v->reset();
    // R21: 同時アクセス (bit8) でプレーン 0 と 2 (bit4、bit6) に書く
    v->crtcWrite(0x2a, 0x01);
    v->crtcWrite(0x2b, 0x50);
    v->tvramWrite(0x10, 0xaa);
    CHECK_EQ(v->tvramRead(0x10), 0xaa);
    CHECK_EQ(v->tvramRead(0x20010), 0x00);
    CHECK_EQ(v->tvramRead(0x40010), 0xaa);
    // ビットマスク (bit9): R23 の 1 のビットは書き換えない
    v->crtcWrite(0x2a, 0x02);
    v->crtcWrite(0x2e, 0xf0);
    v->tvramWrite(0x10, 0x55);
    CHECK_EQ(v->tvramRead(0x10), 0xa5);
    // R20、R21 は読める
    CHECK_EQ(v->crtcRead(0x2a), 0x02);
    CHECK_EQ(v->crtcRead(0x00), 0x00);
    delete v;
}

// IPL ROM の IOCS のラスタコピー ($FF7862-$FF7889) と同じループ: 水平帰線期間 (GPIP7 = 1) を待ち、
// R22 を書いて $E80480 に 8 を書く。これを 8 回続け、ラスタ 1→0、2→1、... と 1 つずつ上へ写す
bool rasterCopyLoop(uint32_t hz) {
    x68k::Config cfg;
    cfg.cpuHz = hz;
    x68k::Machine m(cfg);
    Rom rom;
    rom.code({0x33fc, 0x000f, 0x00e8, 0x002a,  // move.w #$000f,$e8002a (R21: 全プレーンをコピー)
              0x323c, 0x0100,                  // move.w #$0100,d1 (R22: 1→0)
              0x343c, 0x0101,                  // move.w #$0101,d2
              0x7007,                          // moveq #7,d0
              0x007c, 0x0700,                  // loop: ori #$0700,sr
              0x0839, 0x0007, 0x00e8, 0x8001,  // btst #7,$e88001
              0x67f6,                          // beq loop の btst へ
              0x33c1, 0x00e8, 0x002c,          // move.w d1,$e8002c (R22)
              0x33fc, 0x0008, 0x00e8, 0x0480,  // move.w #8,$e80480
              0x027c, 0xf8ff,                  // andi #$f8ff,sr
              0xd242,                          // add.w d2,d1
              0x51c8, 0xffdc,                  // dbra d0,loop
              0x0839, 0x0007, 0x00e8, 0x8001,  // btst #7,$e88001
              0x67f6,
              0x4279, 0x00e8, 0x0480});        // clr.w $e80480
    runRom(m, rom, 0);
    for (int r = 0; r < 16; r++) m.video().tvramWrite((uint32_t)r * 512, (uint8_t)(r + 1));
    m.run(hz / 10);
    bool ok = true;
    for (int r = 0; r < 8; r++) {
        if (m.video().tvramRead((uint32_t)r * 512) != r + 2) ok = false;
    }
    if (!ok) {
        printf("  %uHz:", hz);
        for (int r = 0; r < 10; r++) printf(" %d", m.video().tvramRead((uint32_t)r * 512));
        printf(" (want 2 3 4 5 6 7 8 9 9 10)\n");
    }
    return ok;
}

void testRasterCopyLoop() {
    CHECK_EQ(rasterCopyLoop(10000000), true);
    CHECK_EQ(rasterCopyLoop(16000000), true);
    CHECK_EQ(rasterCopyLoop(25000000), true);
}

void testHsyncAcrossLongInstructions() {
    // 10MHz で DIVU (約 140 サイクル) を続けても、H-SYNC の割り込みがラスタごとに 1 回入る
    x68k::Config cfg;
    cfg.cpuHz = 10000000;
    x68k::Machine m(cfg);
    Rom rom;
    rom.put16(0x0d00, 0x5285); // 割り込みの処理: addq.l #1,d5
    rom.put16(0x0d02, 0x4e73); // rte
    rom.code({0x21fc, 0x00fe, 0x0d00, 0x013c}); // move.l #$fe0d00,$13c.w (ベクタ $4f)
    rom.code({0x13fc, 0x0040, 0x00e8, 0x8017}); // VR = $40
    rom.code({0x13fc, 0x0080, 0x00e8, 0x8003}); // AER = $80 (0→1: 帰線期間の始まり)
    rom.code({0x13fc, 0x0080, 0x00e8, 0x8007}); // IERA = $80
    rom.code({0x13fc, 0x0080, 0x00e8, 0x8013}); // IMRA = $80
    rom.code({0x7a00,                           // moveq #0,d5
              0x7001,                           // moveq #1,d0
              0x263c, 0x0000, 0xffff,           // move.l #$ffff,d3
              0x46fc, 0x2000,                   // move.w #$2000,sr
              0x2203,                           // loop: move.l d3,d1
              0x82c0,                           // divu d0,d1
              0x60fa});                         // bra loop
    runRom(m, rom, 2000); // 割り込みを許可するところまで
    m68k::Registers r;
    m.cpu().getRegisters(r);
    uint32_t d5 = r.d[5];
    uint64_t c0 = m.cpu().cycles();
    m.run(1000000); // 0.1 秒、約 3150 ラスタ
    m.cpu().getRegisters(r);
    // 過ぎたラスタの数: 1 ラスタは 1104 ドット / 34.776MHz = 10MHz で 317.46 サイクル
    int64_t lines = (int64_t)((m.cpu().cycles() - c0) * 34776000ULL / (10000000ULL * 1104));
    int64_t irqs = (int64_t)(r.d[5] - d5);
    if (irqs < lines - 2 || irqs > lines + 2) printf("  lines %lld, interrupts %lld\n", (long long)lines, (long long)irqs);
    CHECK_EQ(irqs >= lines - 2 && irqs <= lines + 2, true);
    CHECK_EQ(lines > 3000, true);
}

void testHrlDotClock() {
    // R20 = $15 (31kHz、512 ドット)、R00 = $5B (736 ドット) で、HRL = 1 ならドットクロックは 69.552MHz / 4。
    // 16MHz で 1 フレーム (568 ラスタ) は 16MHz x 736 / 17.388MHz x 568 = 384,680 サイクル程度
    x68k::Machine m;
    Rom rom;
    rom.code({0x33fc, 0x0015, 0x00e8, 0x0028}); // move.w #$15,$e80028 (R20)
    rom.code({0x33fc, 0x005b, 0x00e8, 0x0000}); // move.w #$5b,$e80000 (R00)
    rom.code({0x13fc, 0x0002, 0x00e8, 0xe007}); // move.b #2,$e8e007 (HRL = 1)
    rom.code({0x60fe});
    runRom(m, rom, 0);
    m.runFrame();
    m.runFrame();
    uint64_t c0 = m.cpu().cycles();
    m.runFrame();
    uint64_t c = m.cpu().cycles() - c0;
    if (c < 383000 || c > 386500) printf("  cycles/frame %llu\n", (unsigned long long)c);
    CHECK_EQ(c >= 383000 && c <= 386500, true);
}

// ---------------------------------------------------------------- FDC から RAM への DMA (Machine::dmaReadBlock())

// XDF のイメージ: セクタごとに中身を変える (先頭のバイト = トラック、2 バイト目 = R、残りは位置)
uint8_t g_xdf[1261568];

// シリンダ 0 のヘッド 0 を R=1 から続けて読んだときの、先頭から k バイト目
uint8_t fdStream(uint32_t k) {
    const uint32_t r = 1 + k / 1024, i = k % 1024;
    return i == 0 ? 0 : i == 1 ? (uint8_t)r : (uint8_t)(i + r);
}

struct FdRead {
    uint8_t csr, cer;
    uint32_t mar, mtc;
    uint8_t result[7];
    int resultLen;
};

// DMAC のチャネル 0 と FDC を IOCS と同じ設定にして、シリンダ 0 のヘッド 0 の R=1 から count バイトを mar へ読む。
// mfc / dfc は DMAC の MFC / DFC に書くファンクションコード
FdRead fdRead(x68k::Machine& m, uint32_t mar, uint32_t count, int mfc, int dfc, int64_t cycles = 4000) {
    const int fc = m68k::FC_SUPER_DATA;
    const uint32_t b = 0xe84000;
    m.write8(b + 0x00, 0xff, fc);
    m.write8(b + 0x04, 0x80, fc); // DCR: 8 ビットのポート
    m.write8(b + 0x05, 0x82, fc); // OCR: デバイス → メモリ、バイト、外部要求
    m.write8(b + 0x06, 0x04, fc); // SCR: メモリ側は増やす、デバイス側は固定
    m.write8(b + 0x0a, (uint8_t)(count >> 8), fc);
    m.write8(b + 0x0b, (uint8_t)count, fc);
    for (int i = 0; i < 4; i++) m.write8(b + 0x0c + i, (uint8_t)(mar >> (24 - 8 * i)), fc);
    for (int i = 0; i < 4; i++) m.write8(b + 0x14 + i, (uint8_t)(0x00e94003 >> (24 - 8 * i)), fc);
    m.write8(b + 0x29, (uint8_t)mfc, fc);
    m.write8(b + 0x31, (uint8_t)dfc, fc);
    m.write8(b + 0x07, 0x80, fc);
    const uint8_t cmd[9] = {0x46, 0x00, 0x00, 0x00, 0x01, 0x03, 0x08, 0x1b, 0xff}; // READ DATA、N=3、EOT=8
    for (uint8_t v : cmd) m.write8(0xe94003, v, fc);
    m.run(cycles);
    FdRead r = {};
    r.csr = m.read8(b + 0x00, fc);
    r.cer = m.read8(b + 0x01, fc);
    r.mtc = (uint32_t)(m.read8(b + 0x0a, fc) << 8 | m.read8(b + 0x0b, fc));
    for (int i = 0; i < 4; i++) r.mar = r.mar << 8 | m.read8(b + 0x0c + i, fc);
    while (r.resultLen < 7 && (m.read8(0xe94001, fc) & 0xd0) == 0xd0) r.result[r.resultLen++] = m.read8(0xe94003, fc);
    return r;
}

// RAM の a からの n バイトのうち、fdStream() の skip バイト目からと違うバイトの数
int fdMismatch(x68k::Machine& m, uint32_t a, uint32_t skip, uint32_t n) {
    int bad = 0;
    bool ok;
    for (uint32_t j = 0; j < n; j++)
        if (m.peek8(a + j, ok) != fdStream(skip + j) || !ok) bad++;
    return bad;
}

// Machine::dmaReadBlock() は、1 バイトずつの転送 (dmaRead8 → Fdc::read、dmaWrite8 → RAM) と同じ結果になる分だけを
// まとめて転送する。Machine の中では 1 バイトずつの転送と比べられないので、1 バイトずつなら起きるはずの結果
// (RAM の中身、バスエラーで止まる番地、DMAC と FDC の結果) を確かめる
void testFdDmaBlock() {
    for (int t = 0; t < 154; t++)
        for (int r = 1; r <= 8; r++) {
            uint8_t* p = g_xdf + (t * 8 + r - 1) * 1024;
            for (int i = 0; i < 1024; i++) p[i] = (uint8_t)(i + t + r);
            p[0] = (uint8_t)t;
            p[1] = (uint8_t)r;
        }
    static x68k::FloppyImage img;
    CHECK_EQ(img.load(g_xdf, sizeof g_xdf, false, "a.xdf"), true);
    static x68k::Machine m; // RAM は 4MB
    Rom rom;
    runRom(m, rom);
    m.insertDisk(0, &img);
    const int sd = m68k::FC_SUPER_DATA, ud = m68k::FC_USER_DATA;
    const uint32_t ramSize = m.config().ramSize;
    bool ok;
    auto mark = [&](uint32_t a) { m.write8(a, 0xa5, sd); }; // 転送されないはずの番地に置く値

    // スーパーバイザの FC で 2 セクタ: TC はセクタの終わり
    mark(0x10000 + 2048);
    FdRead r = fdRead(m, 0x10000, 2048, sd, sd);
    CHECK_EQ(r.csr & 0xf8, 0x80);
    CHECK_EQ(r.mar, 0x10000 + 2048);
    CHECK_EQ(r.mtc, 0);
    CHECK_EQ(fdMismatch(m, 0x10000, 0, 2048), 0);
    CHECK_EQ(m.peek8(0x10000 + 2048, ok), 0xa5);
    CHECK_EQ(r.resultLen, 7);
    CHECK_EQ(r.result[0] & 0xc0, 0x00);
    CHECK_EQ(r.result[5], 3);

    // MAR の上位 8 ビットは本体のバスに出ない: $10020000 への転送は本体の $020000 に入る (MAR は 32 ビットで進む)。
    // 060turbo (68060) のときだけ、チャネル 0 はローカルメモリへ届ける (Machine::dmaMemAddr、testFdDmaLocal()。
    // tests/x68k/scsi_local_mem.cpp)。この Machine (68000) では折り返す
    r = fdRead(m, 0x10020000, 1024, sd, sd);
    CHECK_EQ(r.csr & 0xf8, 0x80);
    CHECK_EQ(r.mar, 0x10020000u + 1024);
    CHECK_EQ(fdMismatch(m, 0x20000, 0, 1024), 0);

    // RAM の終わりをまたぐ: 終わりまでの 100 バイトは入り、次のバイトでメモリのバスエラー
    r = fdRead(m, ramSize - 100, 1024, sd, sd);
    CHECK_EQ(r.csr & 0xf8, 0x90);
    CHECK_EQ(r.cer, 0x09);
    CHECK_EQ(r.mar, ramSize);
    CHECK_EQ(r.mtc, 1024 - 100);
    CHECK_EQ(fdMismatch(m, ramSize - 100, 0, 100), 0);
    CHECK_EQ(r.resultLen, 7);
    CHECK_EQ(r.result[0] & 0xc0, 0x40);
    CHECK_EQ(r.result[1], 0x10); // オーバーラン
    // RAM の最後の 1 バイトから: 1 バイトだけ入る
    m.write8(ramSize - 1, 0xa5, sd);
    r = fdRead(m, ramSize - 1, 16, sd, sd);
    CHECK_EQ(r.cer, 0x09);
    CHECK_EQ(r.mar, ramSize);
    CHECK_EQ(r.mtc, 15);
    CHECK_EQ(m.peek8(ramSize - 1, ok), fdStream(0));
    // RAM の外から: 1 バイトも入らない
    r = fdRead(m, ramSize, 16, sd, sd);
    CHECK_EQ(r.cer, 0x09);
    CHECK_EQ(r.mar, ramSize);
    CHECK_EQ(r.mtc, 16);

    // エリアセット ($E86001 = 3): ユーザの FC では $8000 より下がスーパーバイザ専用
    m.write8(0xe86001, 0x03, sd);
    // MFC がユーザで、境のすぐ下から: 最初のバイトでバスエラー (境から上にも書かない)
    mark(0x7fff);
    mark(0x8000);
    r = fdRead(m, 0x7fff, 1024, ud, sd);
    CHECK_EQ(r.csr & 0xf8, 0x90);
    CHECK_EQ(r.cer, 0x09);
    CHECK_EQ(r.mar, 0x7fff);
    CHECK_EQ(r.mtc, 1024);
    CHECK_EQ(m.peek8(0x7fff, ok), 0xa5);
    CHECK_EQ(m.peek8(0x8000, ok), 0xa5);
    CHECK_EQ(r.result[0] & 0xc0, 0x40);
    CHECK_EQ(r.result[1], 0x10);
    // MFC がユーザで、境から: 全部入る
    mark(0x8000 + 1024);
    r = fdRead(m, 0x8000, 1024, ud, sd);
    CHECK_EQ(r.csr & 0xf8, 0x80);
    CHECK_EQ(r.mtc, 0);
    CHECK_EQ(fdMismatch(m, 0x8000, 0, 1024), 0);
    CHECK_EQ(m.peek8(0x7fff, ok), 0xa5);
    CHECK_EQ(m.peek8(0x8000 + 1024, ok), 0xa5);
    CHECK_EQ(r.result[0] & 0xc0, 0x00);
    // MFC がスーパーバイザなら、境をまたいで入る
    r = fdRead(m, 0x7f00, 1024, sd, sd);
    CHECK_EQ(r.csr & 0xf8, 0x80);
    CHECK_EQ(fdMismatch(m, 0x7f00, 0, 1024), 0);
    // MFC がユーザで RAM の終わりをまたぐ
    r = fdRead(m, ramSize - 50, 1024, ud, sd);
    CHECK_EQ(r.cer, 0x09);
    CHECK_EQ(r.mar, ramSize);
    CHECK_EQ(r.mtc, 1024 - 50);
    CHECK_EQ(fdMismatch(m, ramSize - 50, 0, 50), 0);

    // DFC がユーザ: FDC のレジスタ (I/O) はスーパーバイザ専用なので、最初のバイトでデバイスのバスエラー
    mark(0x20000);
    r = fdRead(m, 0x20000, 1024, sd, ud);
    CHECK_EQ(r.csr & 0xf8, 0x90);
    CHECK_EQ(r.cer, 0x0a);
    CHECK_EQ(r.mar, 0x20000);
    CHECK_EQ(r.mtc, 1024);
    CHECK_EQ(m.peek8(0x20000, ok), 0xa5);
    CHECK_EQ(r.result[0] & 0xc0, 0x40);
    CHECK_EQ(r.result[1], 0x10);
    // そのあとも普通に読める (FDC は前の転送の途中のまま残っていない)
    r = fdRead(m, 0x20000, 3000, sd, sd);
    CHECK_EQ(r.csr & 0xf8, 0x80);
    CHECK_EQ(fdMismatch(m, 0x20000, 0, 3000), 0);
    CHECK_EQ(r.result[0] & 0xc0, 0x00);
    CHECK_EQ(r.result[5], 4); // R=3 の途中で TC
}

// CPU で src から n バイトを RAM の dst へ写す (68060 のローカルメモリは CPU からしか読めないので、確かめるために使う)。
// RAM の $60000 に小さなプログラムを置いて CPU を飛ばす
void cpuCopy(x68k::Machine& m, uint32_t src, uint32_t dst, uint32_t n) {
    const uint16_t prog[] = {
        0x41f9, (uint16_t)(src >> 16), (uint16_t)src, // lea src,a0
        0x43f9, (uint16_t)(dst >> 16), (uint16_t)dst, // lea dst,a1
        0x303c, (uint16_t)(n - 1),                    // move.w #n-1,d0
        0x12d8,                                       // loop: move.b (a0)+,(a1)+
        0x51c8, 0xfffc,                               // dbra d0,loop
        0x60fe,                                       // bra *
    };
    const uint32_t at = 0x60000;
    for (size_t i = 0; i < sizeof prog / 2; i++) {
        m.write8(at + (uint32_t)i * 2, (uint8_t)(prog[i] >> 8), m68k::FC_SUPER_DATA);
        m.write8(at + (uint32_t)i * 2 + 1, (uint8_t)prog[i], m68k::FC_SUPER_DATA);
    }
    m68k::Registers r;
    m.getRegisters(r);
    r.pc = at;
    r.sr = 0x2700;
    r.prefetch[0] = prog[0];
    r.prefetch[1] = prog[1];
    m.setRegisters(r);
    // 終わりの bra * まで進めば止まる位置なので、多めに動かす (キャッシュなしのローカルメモリは 1 バイトに 40 サイクルほど)
    m.run((int64_t)n * 200 + 10000);
    m.getRegisters(r);
    CHECK_EQ(r.pc, at + (uint32_t)sizeof prog - 2);
}

// フロッピーディスクの DMA (DMAC のチャネル 0) の MAR がローカルメモリの範囲 ($1xxxxxxx) を指すとき: 68060 では
// IPL ROM によらずローカルメモリへ届ける (Machine::dmaMemAddr。テスト用の IPL ROM は X68030 の IPL ROM 1.3 ではない)。
// 68000 / 68030 では、MAR の下位 24 ビット (本体のメモリ) に入る。チャネル 2 と 3 は 68060 でも下位 24 ビット
// (チャネル 1 は spc_test の testDmaLocal())
void testFdDmaLocal() {
    static x68k::FloppyImage img;
    CHECK_EQ(img.load(g_xdf, sizeof g_xdf, false, "a.xdf"), true); // testFdDmaBlock() が作った中身
    const int sd = m68k::FC_SUPER_DATA;
    const uint32_t kLocalSize = 16u << 20;
    for (int cpu : {68000, 68030, 68060}) {
        x68k::Config config;
        if (!config.setCpu(cpu)) continue;
        if (cpu == 68060) config.localMemSize = kLocalSize;
        x68k::Machine m(config);
        CHECK_EQ(m.ok(), true);
        Rom rom;
        runRom(m, rom);
        m.insertDisk(0, &img);
        bool ok;
        for (uint32_t j = 0; j < 1024; j++) m.write8(0x20000 + j, 0xa5, sd);
        FdRead r = fdRead(m, 0x10020000, 1024, sd, sd);
        CHECK_EQ(r.csr & 0xf8, 0x80);
        CHECK_EQ(r.mar, 0x10020000u + 1024);
        CHECK_EQ(r.mtc, 0);
        if (cpu != 68060) {
            CHECK_EQ(fdMismatch(m, 0x20000, 0, 1024), 0); // 本体の $020000 に入る
            continue;
        }
        // 68060: 本体の $020000 は変わらず、ローカルメモリの $10020000 に入る
        int kept = 0;
        for (uint32_t j = 0; j < 1024; j++) kept += m.peek8(0x20000 + j, ok) == 0xa5;
        CHECK_EQ(kept, 1024);
        cpuCopy(m, 0x10020000, 0x30000, 1024);
        CHECK_EQ(fdMismatch(m, 0x30000, 0, 1024), 0);
        // 積んである範囲の終わりをまたぐ: 終わりまでの 100 バイトは入り、その先は捨てる (エラーにならない)
        const uint32_t end = x68k::Config::LOCAL_MEM_BASE + kLocalSize;
        r = fdRead(m, end - 100, 1024, sd, sd);
        CHECK_EQ(r.csr & 0xf8, 0x80);
        CHECK_EQ(r.mar, end - 100 + 1024);
        CHECK_EQ(r.mtc, 0);
        cpuCopy(m, end - 100, 0x30000, 100);
        CHECK_EQ(fdMismatch(m, 0x30000, 0, 100), 0);
        // 積んでいないところから始まる転送: 正常に終わるが、どこにも入らない (本体の下位 24 ビットにも入らない)
        for (uint32_t j = 0; j < 1024; j++) m.write8((end & 0xffffff) + j, 0xa5, sd);
        r = fdRead(m, end, 1024, sd, sd);
        CHECK_EQ(r.csr & 0xf8, 0x80);
        CHECK_EQ(r.mar, end + 1024);
        kept = 0;
        for (uint32_t j = 0; j < 1024; j++) kept += m.peek8((end & 0xffffff) + j, ok) == 0xa5;
        CHECK_EQ(kept, 1024);
        // ローカルメモリの範囲の最後 ($1FFFFFFF) をまたぐ転送: $1FFFFFF0-$1FFFFFFF は積んでいないので捨て、
        // $20000000 からは範囲の外なので本体のバスの下位 24 ビット ($000000 から) に入る (今の作り。実機の DMAC は
        // ローカルメモリに届かないので比べられない)
        for (uint32_t j = 0; j < 0x40; j++) m.write8(j, 0xa5, sd);
        r = fdRead(m, 0x1ffffff0, 32, sd, sd);
        CHECK_EQ(r.mar, 0x20000010u);
        CHECK_EQ(r.mtc, 0);
        kept = 0;
        for (uint32_t j = 0; j < 0x10; j++) kept += m.peek8(j, ok) == 0xa5;
        CHECK_EQ(kept, 0);
        CHECK_EQ(fdMismatch(m, 0x0, 16, 16), 0);
        kept = 0;
        for (uint32_t j = 0x10; j < 0x40; j++) kept += m.peek8(j, ok) == 0xa5;
        CHECK_EQ(kept, 0x30);
        // チャネル 2 (オートリクエストのメモリ間の転送) は、68060 でも MAR の下位 24 ビット: $40000 の 256 バイトを
        // MAR = $10050000 へ写すと、本体の $050000 に入る
        for (uint32_t j = 0; j < 256; j++) {
            m.write8(0x40000 + j, (uint8_t)(j ^ 0x5a), sd);
            m.write8(0x50000 + j, 0xa5, sd);
        }
        const uint32_t b = 0xe84080;
        m.write8(b + 0x00, 0xff, sd);
        m.write8(b + 0x04, 0x08, sd); // DCR: 16 ビットのポート (デバイス側のアドレスは 1 ずつ進む)
        m.write8(b + 0x05, 0x81, sd); // OCR: デバイス → メモリ、バイト、オートリクエスト (最大の速さ)
        m.write8(b + 0x06, 0x05, sd); // SCR: メモリ側もデバイス側も増やす
        m.write8(b + 0x0a, 0x01, sd);
        m.write8(b + 0x0b, 0x00, sd);
        for (int i = 0; i < 4; i++) m.write8(b + 0x0c + i, (uint8_t)(0x10050000u >> (24 - 8 * i)), sd);
        for (int i = 0; i < 4; i++) m.write8(b + 0x14 + i, (uint8_t)(0x00040000u >> (24 - 8 * i)), sd);
        m.write8(b + 0x29, (uint8_t)sd, sd);
        m.write8(b + 0x31, (uint8_t)sd, sd);
        m.write8(b + 0x07, 0x80, sd);
        m.run(4000);
        CHECK_EQ(m.read8(b + 0x00, sd) & 0xf8, 0x80);
        int same = 0;
        for (uint32_t j = 0; j < 256; j++) same += m.peek8(0x50000 + j, ok) == (uint8_t)(j ^ 0x5a);
        CHECK_EQ(same, 256);
    }
    // 68060 でローカルメモリ 0: 範囲の全体が積んでいないところで、転送は正常に終わるが本体には入らない
    {
        x68k::Config config;
        CHECK_EQ(config.setCpu(68060), true);
        config.localMemSize = 0;
        x68k::Machine m(config);
        CHECK_EQ(m.ok(), true);
        Rom rom;
        runRom(m, rom);
        m.insertDisk(0, &img);
        bool ok;
        for (uint32_t j = 0; j < 1024; j++) m.write8(0x20000 + j, 0xa5, sd);
        FdRead r = fdRead(m, 0x10020000, 1024, sd, sd);
        CHECK_EQ(r.csr & 0xf8, 0x80);
        CHECK_EQ(r.mar, 0x10020000u + 1024);
        CHECK_EQ(r.mtc, 0);
        int kept = 0;
        for (uint32_t j = 0; j < 1024; j++) kept += m.peek8(0x20000 + j, ok) == 0xa5;
        CHECK_EQ(kept, 1024);
    }
}

// チェーンで開始した DMAC のチャネルは、最初の表をすぐに読む (HD63450 は開始すると、転送の要求を待たずに表を読む)。
// CPU が開始の直後に MAR ($E840CC) を読んでも、表から読んだ番地が見える (ADPCM を IOCS の _ADPCMLOT で回し、
// MAR で再生位置を知るプログラムがある)
// RS-232C (SCC のチャネル A): 送ったバイトを返す相手につなぎ、9600bps で 1 バイト送ると、受信の割り込み
// (レベル 5、ベクタ $5C) が入り、データのポートから同じバイトが読める
struct EchoSerial : x68k::SerialLink {
    uint8_t q[16];
    int n = 0;
    int sent = 0;
    void send(uint8_t b) override {
        sent++;
        if (n < 16) q[n++] = b;
    }
    int receive() override {
        if (n == 0) return -1;
        int b = q[0];
        memmove(q, q + 1, (size_t)--n);
        return b;
    }
    bool carrier() override { return true; }
};

void testRs232cInterrupt() {
    x68k::Machine m;
    EchoSerial link;
    m.setSerialLink(&link);
    Rom rom;
    // 受信の割り込みの処理: D4 に受け取ったバイト、D5 = 1 にして止まる
    rom.put32(0x0d00, 0x183900e9); // move.b $e98007,d4
    rom.put16(0x0d04, 0x8007);
    rom.put16(0x0d06, 0x7a01); // moveq #1,d5
    rom.put16(0x0d08, 0x60fe);
    rom.code({0x21fc, 0x00fe, 0x0d00, 0x0170}); // move.l #$fe0d00,$170.w (ベクタ $5C)
    static const uint8_t kInit[][2] = {
        {9, 0xc0}, {2, 0x50}, {4, 0x44}, {12, 14}, {13, 0}, {14, 0x03}, {3, 0xc1}, {5, 0xea}, {1, 0x10}, {9, 0x09}};
    for (const auto& w : kInit) {
        rom.code({0x13fc, w[0], 0x00e9, 0x8005}); // move.b #reg,$e98005
        rom.code({0x13fc, w[1], 0x00e9, 0x8005}); // move.b #val,$e98005
    }
    rom.code({0x13fc, 0x0041, 0x00e9, 0x8007}); // move.b #'A',$e98007
    rom.code({0x46fc, 0x2000});                 // move.w #$2000,sr
    rom.code({0x60fe});
    Result r = runRom(m, rom, 16000); // 1 文字 (約 16400 サイクル) を送り終える前
    CHECK_EQ(link.sent, 0);
    CHECK_EQ(r.d5, 0);
    m.run(50000);
    m68k::Registers regs;
    m.cpu().getRegisters(regs);
    CHECK_EQ(link.sent, 1);
    CHECK_EQ(regs.d[5], 1);
    CHECK_EQ(regs.d[4] & 0xff, 0x41);
    m.setSerialLink(nullptr);
}

// チャネル A を読むときは周辺機器を今の時刻まで進める (machine.cpp の syncDevices())。送信バッファの空き (RR0 bit2) を
// 読み続け、空いたのが 1 文字の時間ちょうどに見えるか。進めないと、最大で 1 回の実行の区切り (256 サイクル) 遅れて見える
void testRs232cReadSync() {
    x68k::Machine m;
    EchoSerial link;
    m.setSerialLink(&link);
    Rom rom;
    static const uint8_t kInit[][2] = {
        {9, 0xc0}, {4, 0x44}, {12, 14}, {13, 0}, {14, 0x03}, {3, 0xc1}, {5, 0xea}, {1, 0x00}};
    for (const auto& w : kInit) {
        rom.code({0x13fc, w[0], 0x00e9, 0x8005}); // move.b #reg,$e98005
        rom.code({0x13fc, w[1], 0x00e9, 0x8005}); // move.b #val,$e98005
    }
    rom.code({0x13fc, 0x0041, 0x00e9, 0x8007}); // move.b #'A',$e98007 (すぐシフトレジスタに移る)
    rom.code({0x13fc, 0x0042, 0x00e9, 0x8007}); // move.b #'B',$e98007 (A を送り終えるまでバッファに残る)
    rom.code({0x7c00});                         // moveq #0,d6
    rom.code({0x1039, 0x00e9, 0x8005});         // L: move.b $e98005,d0 (RR0)
    rom.code({0x5286});                         // addq.l #1,d6
    rom.code({0x0800, 0x0002});                 // btst #2,d0
    rom.code({0x67f2});                         // beq.s L
    rom.code({0x7a01});                         // moveq #1,d5
    rom.code({0x60fe});
    Result r = runRom(m, rom, 40000);
    CHECK_EQ(r.d5, 1);
    CHECK_EQ(link.sent >= 1, 1);
    // 1 文字は 16384 サイクルで、1 回の繰り返しは I/O の待ちを含めて約 59 サイクルなので 278 回。
    // チャネル A を読むときに進めないと 282 回になった (区切りの 256 サイクルの分、遅れて見える)
    CHECK_EQ(r.d6 >= 277 && r.d6 <= 279, 1);
    m.setSerialLink(nullptr);
}

void testDmacChainLoadAtStart() {
    static x68k::Machine m;
    Rom rom;
    runRom(m, rom);
    const int fc = m68k::FC_SUPER_DATA;
    // $1000 の表: $2000 から $100 バイト、リンク先は自分 (輪)
    const uint8_t table[10] = {0x00, 0x00, 0x20, 0x00, 0x01, 0x00, 0x00, 0x00, 0x10, 0x00};
    for (int i = 0; i < 10; i++) m.write8(0x1000 + (uint32_t)i, table[i], fc);
    const uint32_t b = 0xe840c0; // チャネル 3 (ADPCM)
    m.write8(b + 0x00, 0xff, fc);
    m.write8(b + 0x04, 0x80, fc);
    m.write8(b + 0x05, 0x3e, fc); // OCR: メモリ → デバイス、バイト (パックなし)、リンクアレイチェーン、外部要求
    m.write8(b + 0x06, 0x04, fc);
    for (int i = 0; i < 4; i++) m.write8(b + 0x0c + i, 0xee, fc); // MAR と MTC に前の値を残しておく
    m.write8(b + 0x0a, 0xee, fc);
    m.write8(b + 0x0b, 0xee, fc);
    for (int i = 0; i < 4; i++) m.write8(b + 0x14 + i, (uint8_t)(0x00e92003 >> (24 - 8 * i)), fc);
    for (int i = 0; i < 4; i++) m.write8(b + 0x1c + i, (uint8_t)(0x00001000 >> (24 - 8 * i)), fc);
    m.write8(b + 0x07, 0x80, fc);
    // run() をはさまずに読む
    uint32_t mar = 0, bar = 0;
    for (int i = 0; i < 4; i++) mar = mar << 8 | m.read8(b + 0x0c + i, fc);
    for (int i = 0; i < 4; i++) bar = bar << 8 | m.read8(b + 0x1c + i, fc);
    const uint32_t mtc = (uint32_t)(m.read8(b + 0x0a, fc) << 8 | m.read8(b + 0x0b, fc));
    CHECK_EQ(m.read8(b + 0x00, fc) & 0xf8, 0x08); // ACT
    CHECK_EQ(mar, 0x2000);
    CHECK_EQ(mtc, 0x100);
    CHECK_EQ(bar, 0x1000);
    m.write8(b + 0x07, 0x10, fc); // 止める
}

struct Case {
    const char* name;
    void (*fn)();
};

} // namespace

#ifndef PIX68K_NO_CPU020
// X68030: 68030 のコアで動き、ハイメモリは繰り返さず、本体の 16MB は繰り返し見え、バスエラーは 68030 に届く
void testX68030() {
    x68k::Config cfg;
    CHECK_EQ(cfg.setCpu(68030), true);
    CHECK_EQ(cfg.setClockMHz(16), false); // X68030 のクロックは変えない
    {
        // 68000 に戻すと XVI (16MHz、$FE) に戻る。68000 のままなら選んだクロックを残す
        x68k::Config back = cfg;
        CHECK_EQ(back.setCpu(68000), true);
        CHECK_EQ(back.cpuHz, 16000000);
        CHECK_EQ(back.cpuInfo, 0xfe);
        x68k::Config ten;
        CHECK_EQ(ten.setClockMHz(10), true);
        CHECK_EQ(ten.setCpu(68000), true);
        CHECK_EQ(ten.cpuHz, 10000000);
        CHECK_EQ(ten.cpuInfo, 0xff);
    }
    cfg.highMemSize = 16 << 20;
    x68k::Machine* m = new x68k::Machine(cfg);
    CHECK_EQ(m->ok(), true);
    CHECK_EQ(m->cpu020() != nullptr, true);
    CHECK_EQ(m->cpuHz(), 25000000);
    Rom rom;
    rom.code({0x23fc, 0x1234, 0x5678, 0x0100, 0x0000}); // move.l #$12345678,$01000000 (ハイメモリ)
    rom.code({0x2039, 0x0100, 0x0000});                 // move.l $01000000,d0
    rom.code({0x2238, 0x0000});                         // move.l $0.w,d1
    rom.code({0x2439, 0x0200, 0x0000});                 // move.l $02000000,d2 ($000000 が見える)
    rom.code({0x1639, 0x00e8, 0xe00b});                 // move.b $e8e00b,d3 (CPU の種類)
    rom.code({0x1039, 0x1240, 0x0000});                 // move.b $12400000,d0 ($400000 の写し。4MB の先: バスエラー)
    rom.end();
    m->setIplRom(rom.b, sizeof rom.b);
    m->powerOn();
    m->run(20000);
    m68k::Registers r;
    m->getRegisters(r);
    CHECK_EQ(r.d[0], 0x12345678);
    CHECK_EQ(r.d[1], 0x00002000); // リセットの SSP ($FF0000 の写し)
    CHECK_EQ(r.d[2], 0x00002000);
    CHECK_EQ(r.d[3] & 0xff, 0xdc);
    CHECK_EQ(r.d[7], 1); // バスエラーの処理に入った
    CHECK_EQ(r.d[6], 0);
    // フォーマット $B のフレーム: フォールトアドレスは 32 ビットのまま、SSW は DF、RW (読み)、SIZE (バイト)、FC (5)
    bool ok;
    auto peek16 = [&](uint32_t a) { return (uint32_t)((m->peek8(a, ok) << 8) | m->peek8(a + 1, ok)); };
    uint32_t sp = 0x2000 - 92;
    CHECK_EQ(peek16(sp + 6), 0xb008);
    CHECK_EQ(peek16(sp + 10), 0x0155);
    CHECK_EQ((peek16(sp + 16) << 16) | peek16(sp + 18), 0x12400000);
    delete m;
}

// X68030 の内蔵 SCSI の ROM の先頭: ID 0-7 の起動の入り口、初期化の入り口、'SCSIIN'。入り口は IPL ROM の
// $FF0020 (起動) と $FF0024 (初期化) から取り、起動の入り口の $14 バイト前に 'SCSI' がなければ作らない
// 060turbo の速さ (Machine::setCpuSpeed): 単位は実機の 68060 の %。下限は Config::speed060、上限は
// Config::maxSpeedLimit060 (0 なら SPEED_MAX)
void testSpeed060() {
    {
        x68k::Config cfg;
        CHECK_EQ(cfg.setCpu(68060), true);
        x68k::Machine* m = new x68k::Machine(cfg);
        CHECK_EQ(m->cpuSpeed(), 100); // 既定は実機と同じ
        m->setCpuSpeed(50);
        CHECK_EQ(m->cpuSpeed(), 100);
        m->setCpuSpeed(400);
        CHECK_EQ(m->cpuSpeed(), 400);
        m->setCpuSpeed(100000);
        CHECK_EQ(m->cpuSpeed(), x68k::Machine::SPEED_MAX);
        delete m;
    }
    {
        x68k::Config cfg;
        CHECK_EQ(cfg.setCpu(68060), true);
        cfg.speed060 = 25;
        cfg.maxSpeedLimit060 = 100;
        x68k::Machine* m = new x68k::Machine(cfg);
        CHECK_EQ(m->cpuSpeed(), 25);
        m->setCpuSpeed(10);
        CHECK_EQ(m->cpuSpeed(), 25);
        m->setCpuSpeed(50);
        CHECK_EQ(m->cpuSpeed(), 50);
        m->setCpuSpeed(800);
        CHECK_EQ(m->cpuSpeed(), 100); // 実機の速さを超えない
        delete m;
    }
    {
        // 上限が下限より小さいときは、下限のまま
        x68k::Config cfg;
        CHECK_EQ(cfg.setCpu(68060), true);
        cfg.speed060 = 200;
        cfg.maxSpeedLimit060 = 100;
        x68k::Machine* m = new x68k::Machine(cfg);
        m->setCpuSpeed(3200);
        CHECK_EQ(m->cpuSpeed(), 200);
        delete m;
    }
}

// 060turbo のクロック (Config::setClockMHz の 50 / 75)。75MHz では X68000 の 1 秒が 7500 万サイクルになり、
// フレームの数 (時間) は 50MHz と同じで、同じ時間に実行する命令の数は 1.5 倍になる
void testClock060() {
    {
        x68k::Config cfg;
        CHECK_EQ(cfg.setClockMHz(75), false); // 68000 では選べない
        CHECK_EQ(cfg.setCpu(68060), true);
        CHECK_EQ(cfg.cpuHz, 50000000);
        CHECK_EQ(cfg.busWait060, 6);
        CHECK_EQ(cfg.setClockMHz(16), false);
        CHECK_EQ(cfg.setClockMHz(60), false);
        CHECK_EQ(cfg.setClockMHz(75), true);
        CHECK_EQ(cfg.cpuHz, 75000000);
        CHECK_EQ(cfg.busWait060, 9); // 本体のバスの 1 回 (25MHz の 3 クロック = 120ns) は 75MHz の 9 サイクル
        CHECK_EQ(cfg.setCpu(68060), true); // 68060 のままなら選んだクロックを残す
        CHECK_EQ(cfg.cpuHz, 75000000);
        CHECK_EQ(cfg.setClockMHz(50), true);
        CHECK_EQ(cfg.busWait060, 6);
        CHECK_EQ(cfg.setClockMHz(75), true);
        x68k::Config back = cfg;
        CHECK_EQ(back.setCpu(68000), true);
        CHECK_EQ(back.cpuHz, 16000000);
        x68k::Config c030 = cfg;
        CHECK_EQ(c030.setCpu(68030), true);
        CHECK_EQ(c030.cpuHz, 25000000);
    }
    // k = 0 / 1: 50MHz / 75MHz、cache = 1 で命令キャッシュとデータキャッシュを有効にする
    for (int cache = 1; cache >= 0; cache--) {
        uint64_t frames[2] = {}, loops[2] = {};
        for (int k = 0; k < 2; k++) {
            x68k::Config cfg;
            cfg.setCpu(68060);
            cfg.setClockMHz(k ? 75 : 50);
            x68k::Machine* m = new x68k::Machine(cfg);
            CHECK_EQ(m->ok(), true);
            CHECK_EQ(m->cpuHz(), k ? 75000000u : 50000000u);
            Rom rom;
            if (cache) rom.code({0x203c, 0x8000, 0x8000, 0x4e7b, 0x0002}); // move.l #$80008000,d0 / movec d0,cacr
            rom.code({0x7000});         // moveq #0,d0
            rom.code({0x5280, 0x60fc}); // addq.l #1,d0 / bra.s *-2
            m->setIplRom(rom.b, sizeof rom.b);
            m->powerOn();
            m->runFrame(); // 始めの区切りをそろえる
            const uint64_t c0 = m->cycles(), f0 = m->frameCount();
            m68k::Registers r;
            m->getRegisters(r);
            const uint32_t d0 = r.d[0];
            while (m->cycles() - c0 < m->cpuHz()) m->run(10000); // X68000 の時間で 1 秒
            frames[k] = m->frameCount() - f0;
            m->getRegisters(r);
            loops[k] = r.d[0] - d0;
            delete m;
        }
        printf("  1 s, cache %s: 50MHz %llu frames %llu loops, 75MHz %llu frames %llu loops\n", cache ? "on" : "off",
               (unsigned long long)frames[0], (unsigned long long)loops[0], (unsigned long long)frames[1],
               (unsigned long long)loops[1]);
        CHECK_EQ(frames[0] > 50 && frames[0] < 70, true);
        CHECK_EQ(frames[1] + 1 >= frames[0] && frames[1] <= frames[0] + 1, true);
        if (cache) {
            // キャッシュに当たる間は、命令の数は 1.5 倍 (1% まで)
            CHECK_EQ(loops[1] * 200 > loops[0] * 297 && loops[1] * 200 < loops[0] * 303, true);
        } else {
            // キャッシュを切ると、命令の取り出しは毎回本体のバス (ROM) に出る。その時間はクロックによらないので、
            // 命令の数は 1.5 倍より少ない (このループは 50MHz で 1 周 25 サイクル、うち取り出し 2 回が 21 サイクル)
            CHECK_EQ(loops[1] > loops[0] && loops[1] * 100 < loops[0] * 115, true);
        }
    }
}

// ---- 060turbo で X68030 の IPL ROM (1.3) を使うときの手当て (Machine::patchIplFor060) ----
// テスト用の IPL ROM に、X68030 の IPL ROM 1.3 の _ROMVER ($FF0030: move.l #$13921127,d0 / rts) を置く
void putRomver13(Rom& rom) {
    static const uint8_t kRomver30[] = {0x20, 0x3c, 0x13, 0x92, 0x11, 0x27, 0x4e, 0x75};
    memcpy(rom.b + 0x10030, kRomver30, sizeof kRomver30);
}

// IOCS のワークの $0CB8 の書き換え: IPL ROM 1.3 が測った値を $0CB8 / $0CBA に書くところ ($FF0198: swap d2 /
// move.l d2,(a0)) を、$FF0830 に置いた処理の呼び出しにする。$0CB8 は 8333 になり、$0CBA は測った値のまま。
// 元の命令が違うとき、$FF0830 からが空いていない ($FF でない) とき、_ROMVER が 1.3 でないときは書き換えない。
// k = 4 は 75MHz で、$0CB8 は cpuHz / 6000 = 12500
void testIplClockPatch060() {
    for (int k = 0; k < 5; k++) {
        Rom rom;
        if (k != 3) putRomver13(rom);
        rom.code({0x41f8, 0x0cb8});         // lea $0cb8.w,a0
        rom.code({0x243c, 0x2222, 0x1111}); // move.l #$22221111,d2
        rom.code({0x4ef9, 0x00ff, 0x0198}); // jmp $ff0198
        rom.put16(0x10198, k == 1 ? 0x4840 : 0x4842); // swap d2 (k = 1 は swap d0: 元の命令が違うので書き換えない)
        rom.put16(0x1019a, 0x2082);                  // move.l d2,(a0)
        rom.put16(0x1019c, 0x7c01);                  // moveq #1,d6
        rom.put16(0x1019e, 0x60fe);                  // bra.s *
        if (k == 2) rom.b[0x10835] = 0x00;           // 書き換えの処理を置くところが使われている
        x68k::Config cfg;
        CHECK_EQ(cfg.setCpu(68060), true);
        if (k == 4) CHECK_EQ(cfg.setClockMHz(75), true);
        x68k::Machine* m = new x68k::Machine(cfg);
        // runRom() は最後に end() を足すが、プログラムは $FF0198 から先で止まる
        Result r = runRom(*m, rom, 200000);
        CHECK_EQ(r.d6, 1);
        bool ok;
        const uint32_t cb8 = (uint32_t)(m->peek8(0xcb8, ok) << 8 | m->peek8(0xcb9, ok));
        const uint32_t cba = (uint32_t)(m->peek8(0xcba, ok) << 8 | m->peek8(0xcbb, ok));
        const bool patched = m->peek8(0xff0198, ok) == 0x61;
        CHECK_EQ(patched, k == 0 || k == 4);
        CHECK_EQ(m->peek8(0xff0830, ok), k == 0 || k == 4 ? 0x48 : 0xff);
        // $0CBA は測った値のまま (k = 1 は d2 を入れ替えないので $2222 と $1111)
        CHECK_EQ(cb8, k == 0 ? 8333u : k == 4 ? 12500u : k == 1 ? 0x2222u : 0x1111u);
        CHECK_EQ(cba, k == 1 ? 0x1111u : 0x2222u);
        delete m;
    }
}

void testScsiRom30() {
    static uint8_t ipl[x68k::Machine::IPL_SIZE];
    static uint8_t out[x68k::Machine::SCSI_ROM_SIZE];
    memset(ipl, 0xff, sizeof ipl);
    auto put32 = [](uint8_t* p, uint32_t v) {
        p[0] = (uint8_t)(v >> 24);
        p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);
        p[3] = (uint8_t)v;
    };
    auto get32 = [](const uint8_t* p) {
        return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    };
    put32(ipl + 0x10020, 0x00ff1234); // 起動の入り口
    put32(ipl + 0x10024, 0x00ff1200); // 初期化の入り口
    memcpy(ipl + 0x11234 - 0x14, "SCSI", 4);
    x68k::Machine::makeScsiRom30(ipl, out);
    for (int id = 0; id < 8; id++) CHECK_EQ(get32(out + id * 4), 0x00ff1234);
    CHECK_EQ(get32(out + 0x20), 0x00ff1200);
    CHECK_EQ(memcmp(out + 0x24, "SCSIIN", 6), 0);
    CHECK_EQ(out[0x2a], 0xff);
    CHECK_EQ(out[x68k::Machine::SCSI_ROM_SIZE - 1], 0xff);
    // 'SCSI' の印がなければ全部 $FF (内蔵 SCSI なし)
    memset(out, 0, sizeof out);
    memcpy(ipl + 0x11234 - 0x14, "XXXX", 4);
    x68k::Machine::makeScsiRom30(ipl, out);
    CHECK_EQ(get32(out), 0xffffffff);
    CHECK_EQ(out[0x24], 0xff);
}
#endif

int main() {
    const Case cases[] = {
        {"reset vector", testResetVector},
        {"memory map (peek)", testMemoryMapPeek},
        {"bus errors", testBusErrors},
        {"68000 memory (12MB, no high memory)", testMemory68000},
        {"SRAM write protect", testSramWriteProtect},
        {"SRAM memory size", testSramMemorySize},
        {"CPU info", testCpuInfo},
        {"MFP timer interrupt", testMfpTimerInterrupt},
        {"MFP prescaler change", testMfpPrescalerChange},
        {"frames", testFrames},
        {"68000 CPU speed", testCpuSpeed68000},
        {"68000 CPU speed: STOP and wake", testCpuSpeedStopWake},
        {"68000 CPU speed: time does not go back", testCpuSpeedMonotonic},
        {"68000 CPU speed: state", testCpuSpeedState},
        {"68000 max speed", testMaxSpeed68000},
        {"RTC", testRtc},
        {"RTC digit writes", testRtcDigitWrites},
        {"RTC carry", testRtcCarry},
        {"H-SYNC interrupt", testHsyncInterrupt},
        {"line timing", testLineTiming},
        {"CRTC mirror and raster copy", testCrtcMirrorAndRasterCopy},
        {"raster copy loop (IOCS)", testRasterCopyLoop},
        {"H-SYNC across long instructions", testHsyncAcrossLongInstructions},
        {"HRL dot clock", testHrlDotClock},
        {"TVRAM simultaneous write", testTvramSimultaneousWrite},
        {"FDC to RAM DMA in blocks", testFdDmaBlock},
        {"FDC DMA to 68060 local memory", testFdDmaLocal},
        {"DMAC chain: first descriptor is read at start", testDmacChainLoadAtStart},
        {"RS-232C interrupt", testRs232cInterrupt},
        {"RS-232C read sync", testRs232cReadSync},
#ifndef PIX68K_NO_CPU020
        {"X68030", testX68030},
        {"060turbo speed", testSpeed060},
        {"060turbo clock", testClock060},
        {"X68030 SCSI ROM header", testScsiRom30},
        {"060turbo with IPL ROM 1.3: $0CB8 patch", testIplClockPatch060},
#endif
    };
    for (const Case& c : cases) {
        int before = failures;
        c.fn();
        printf("%s %s\n", failures == before ? "ok  " : "FAIL", c.name);
    }
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
