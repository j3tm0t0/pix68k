// 68000 の機種のバスのウェイトと DRAM リフレッシュの遅れ (Machine::setupBusWaits()、refreshWait()) のテスト。
// - 領域ごとのウェイト (10MHz と 16MHz、読みと書き、1/4 クロックの端数の持ち越し)
// - Config::busWait68000 = false と 68030 ではウェイトを入れない
// - CPU を動かす区切りの外の読み書き (Machine を直接呼ぶ) のウェイトを、次の区切りのリフレッシュの計算から引かない
//   (速さ 100 と、最高速モードの runCpu000Scaled())
// - ステートを読んだあとの動きが、読む前の Machine の状態によらない
// - メインメモリの上のループはリフレッシュで約 2.2% 遅くなる
//
// X68030 と 060turbo の本体のバス (Config::busWait060 と busArea) のテスト。
// - I/O とスプライトの読み書きは 1.6 マイクロ秒ごとの区切りで終わる。VRAM、SRAM、ROM、メインメモリは待たない
// - si.r の system performance のループ (I/O を 1 回読む) は、キャッシュの有効 / 無効、50 / 75MHz、speed060、
//   最高速モードのどれでも 1 周 1.6 マイクロ秒 (実機の 177.82% = 毎秒 625000 周)
// - si.r の processor performance のループは 1 周 1 サイクルのまま
#include <initializer_list>
#include <string.h>

#include "test_check.h"
#include "x68k/machine.h"
#include "x68k/state.h"

namespace {

constexpr int SD = m68k::FC_SUPER_DATA;
constexpr uint32_t CODE = 0x1000; // メインメモリに置く命令

// IPL ROM: SSP $2000、PC $FE0100 (bra.s *)
void makeRom(uint8_t* rom) {
    memset(rom, 0xff, x68k::Machine::IPL_SIZE);
    const uint8_t vec[8] = {0x00, 0x00, 0x20, 0x00, 0x00, 0xfe, 0x01, 0x00};
    memcpy(rom + 0x10000, vec, 8);
    rom[0x100] = 0x60;
    rom[0x101] = 0xfe;
}

x68k::Machine* make(int mhz, bool waits = true, int cpu = 68000) {
    x68k::Config cfg;
    cfg.setCpu(cpu);
    if (cpu == 68000) cfg.setClockMHz(mhz);
    cfg.busWait68000 = waits;
    auto* m = new x68k::Machine(cfg);
    static uint8_t rom[x68k::Machine::IPL_SIZE];
    makeRom(rom);
    m->setIplRom(rom, sizeof rom);
    m->powerOn();
    return m;
}

// メインメモリの CODE に命令を置き、PC をそこに移す
void jumpToRam(x68k::Machine& m, const uint16_t* code, int n) {
    for (int i = 0; i < n; i++) m.write16(CODE + 2 * i, code[i], SD);
    m68k::Registers r;
    m.getRegisters(r);
    r.pc = CODE;
    r.prefetch[0] = code[0];
    r.prefetch[1] = n > 1 ? code[1] : 0;
    r.sr = 0x2700;
    m.setRegisters(r);
}

// addr を 4 回読む (write なら書く) のに Machine が足したクロック数 (CPU が数える 4 クロックは入らない)
uint64_t wait4(x68k::Machine& m, uint32_t addr, bool write, bool word = false) {
    const uint64_t c0 = m.cycles();
    for (int i = 0; i < 4; i++) {
        if (word) {
            if (write) m.write16(addr, 0, SD);
            else m.read16(addr, SD);
        } else {
            if (write) m.write8(addr, 0, SD);
            else m.read8(addr, SD);
        }
    }
    return m.cycles() - c0;
}

void testRegions() {
    printf("wait cycles per region (10MHz / 16MHz)\n");
    struct Case {
        uint32_t addr;
        bool write;
        uint64_t w10, w16; // 4 回の合計 (クロック)
    };
    const Case cases[] = {
        {0x001000, false, 0, 0},    // メインメモリ
        {0x001000, true, 0, 0},
        {0xc00000, false, 4, 8},    // グラフィック VRAM 1 / 2
        {0xc00000, true, 4, 8},
        {0xe00000, false, 6, 9},    // テキスト VRAM 1.5 / 2.25
        {0xe00000, true, 6, 9},
        {0xe8a001, false, 8, 28},   // RTC 2 / 7
        {0xe88001, false, 16, 60},  // MFP 4 / 15
        {0xe92001, false, 8, 28},   // ADPCM 読み 2 / 7
        {0xe92001, true, 15, 28},   // ADPCM 書き 3.75 / 7
        {0xeb0000, false, 16, 28},  // スプライト 4 / 7
        {0xeb8000, false, 15, 28},  // PCG 3.75 / 7
        {0xed0000, false, 4, 4},    // SRAM 1
        {0xf00000, false, 4, 4},    // CGROM 1
        {0xff0000, false, 4, 4},    // IPL ROM 1
    };
    x68k::Machine* m10 = make(10);
    x68k::Machine* m16 = make(16);
    for (const Case& c : cases) {
        CHECK_EQ(wait4(*m10, c.addr, c.write), c.w10);
        CHECK_EQ(wait4(*m16, c.addr, c.write), c.w16);
    }
    // 語の読み書きも 1 回分 (read16 の、ROM、CGROM、テキスト VRAM を直接読む経路も)
    CHECK_EQ(wait4(*m10, 0xe00000, false, true), 6);
    CHECK_EQ(wait4(*m16, 0xe00000, false, true), 9);
    CHECK_EQ(wait4(*m10, 0xfe0000, false, true), 4);
    CHECK_EQ(wait4(*m10, 0xf00000, false, true), 4);
    CHECK_EQ(wait4(*m10, 0xc00000, true, true), 4);
    // 端数の持ち越し: テキスト VRAM (1.5) を 1 回ずつ読むと 1、2、1、2 クロック
    uint64_t c0 = m10->cycles();
    m10->read8(0xe00000, SD);
    CHECK_EQ(m10->cycles() - c0, 1);
    m10->read8(0xe00000, SD);
    CHECK_EQ(m10->cycles() - c0, 3);
    // 読んだ値は表 (setFastPage) を通さなくても同じ
    m10->write16(0xe00010, 0x1234, SD);
    CHECK_EQ(m10->read16(0xe00010, SD), 0x1234);
    delete m10;
    delete m16;
}

void testDisabled() {
    printf("no 68000 wait cycles with busWait68000 = false and on the 68030\n");
    x68k::Machine* m = make(10, false);
    CHECK_EQ(wait4(*m, 0xe8a001, false), 0);
    CHECK_EQ(wait4(*m, 0xe00000, true), 0);
    CHECK_EQ(wait4(*m, 0xfe0000, false, true), 0);
    delete m;
    if (x68k::Config::has020()) {
        x68k::Machine* m30 = make(25, true, 68030);
        CHECK_EQ(m30->ok(), true);
        // 68000 の表のウェイトは入らない (I/O は 1.6 マイクロ秒の区切りにそろえるだけ。testSync030())
        CHECK_EQ(wait4(*m30, 0xc00000, true), 0);
        CHECK_EQ(wait4(*m30, 0xed0000, false), 0);
        delete m30;
    }
}

// 区切りの外で読み書きしたウェイトを、次の区切りの経過クロックから引かない (引くと符号なしの引き算があふれる)
void testOutsideAccess(int speed) {
    printf("accesses outside CPU slices do not leak into the refresh (speed %d)\n", speed);
    x68k::Machine* m = make(10);
    const uint16_t loop[] = {0x60fe}; // bra.s *
    jumpToRam(*m, loop, 1);
    m->setCpuSpeed(speed);
    for (int i = 0; i < 1000; i++) m->write8(0xe00000, 0, SD); // 1500 クロックのウェイト
    for (int i = 0; i < 20; i++) {
        const uint64_t c0 = m->cycles();
        m->run(1);
        const uint64_t d = m->cycles() - c0;
        CHECK_EQ(d >= 1 && d < 300, true); // 1 命令と、多くても区切り 1 つ分
    }
    delete m;
}

// 同じステートを、起動した直後の Machine と動かしたあとの Machine に読むと、そのあとの動きが同じ
void testState() {
    printf("state load does not depend on the previous machine state\n");
    const size_t cap = 16 << 20;
    uint8_t* buf = new uint8_t[cap];
    x68k::Machine* a = make(10);
    const uint16_t loop[] = {0x5280, 0x60fc}; // addq.l #1,d0; bra.s *-2
    jumpToRam(*a, loop, 2);
    a->run(5000);
    a->read8(0xe00000, SD); // 端数を残す (1.5 クロック)
    size_t n = 0;
    CHECK_EQ(x68k::saveState(*a, buf, cap, n), true);
    x68k::Machine* b = make(10); // 起動した直後
    x68k::Machine* c = make(10);
    jumpToRam(*c, loop, 2);
    c->run(20000);
    c->write8(0xe00000, 0, SD);
    const char* err = nullptr;
    CHECK_EQ(x68k::loadState(*b, buf, n, err), true);
    CHECK_EQ(x68k::loadState(*c, buf, n, err), true);
    for (int i = 0; i < 15; i++) {
        a->run(1);
        b->run(1);
        c->run(1);
        CHECK_EQ(b->cycles(), a->cycles());
        CHECK_EQ(c->cycles(), a->cycles());
        b->read8(0xe00000, SD);
        c->read8(0xe00000, SD);
        a->read8(0xe00000, SD);
    }
    delete a;
    delete b;
    delete c;
    delete[] buf;
}

// メインメモリの上のループは約 2.2% (45 クロックごとに 1 クロック) 遅くなる。ROM の上では遅くならない
void testRefresh() {
    printf("DRAM refresh slows a loop in main memory by about 2.2%%\n");
    const uint16_t loop[] = {0x5280, 0x60fc}; // addq.l #1,d0 (8); bra.s (10)
    uint32_t count[2];
    for (int w = 0; w < 2; w++) {
        x68k::Machine* m = make(10, w == 1);
        jumpToRam(*m, loop, 2);
        m->run(10000000);
        m68k::Registers r;
        m->getRegisters(r);
        count[w] = r.d[0];
        delete m;
    }
    // ウェイトなしでは 18 クロックで 1 回。リフレッシュで 18 x 46/45 クロックになる
    const double ratio = (double)count[0] / count[1];
    CHECK_EQ(ratio > 1.018 && ratio < 1.026, true);
    if (!(ratio > 1.018 && ratio < 1.026)) printf("  ratio %f\n", ratio);
}

// ---- 060turbo ----

struct Opt060 {
    int cpu = 68060;     // 68030 なら X68030 (25MHz。mhz、cache、speed060 は使わない)
    int mhz = 50;
    bool cache = true;
    int speed060 = 100;  // Config::speed060
    int maxSpeed = 100;  // Machine::setCpuSpeed()
    int syncNs = -1;     // 0 以上なら Config::setBusSync() の周期
    int areas = -1;      // 0 以上なら Config::setBusSync() の場所
    void (*tune)(x68k::Config&) = nullptr; // Config::busArea などを書き換える (setBusSync() のあとに呼ぶ)
};

using Code = std::initializer_list<uint16_t>;

// IPL ROM の $FE0100 に code を置いた 060turbo。cache なら、始めに命令キャッシュとデータキャッシュを有効にする
// (スーパースカラと分岐キャッシュはどちらでも有効にする。si.r を動かすときの 060turbo.sys の設定)
x68k::Machine* make060(const Opt060& o, Code code) {
    x68k::Config cfg;
    cfg.setCpu(o.cpu);
    if (o.cpu == 68060) {
        cfg.setClockMHz(o.mhz);
        cfg.speed060 = o.speed060;
    }
    if (o.syncNs >= 0 || o.areas >= 0)
        cfg.setBusSync(o.syncNs >= 0 ? o.syncNs : x68k::Config::BUS_SYNC_NS,
                       o.areas >= 0 ? (uint8_t)o.areas : (uint8_t)(x68k::Config::BUS_SYNC_IO | x68k::Config::BUS_SYNC_SPRITE));
    if (o.tune) o.tune(cfg);
    auto* m = new x68k::Machine(cfg);
    static uint8_t rom[x68k::Machine::IPL_SIZE];
    makeRom(rom);
    int at = 0x100;
    auto put = [&](uint16_t w) {
        rom[at++] = (uint8_t)(w >> 8);
        rom[at++] = (uint8_t)w;
    };
    if (o.cpu == 68060) {
        for (uint16_t w : {0x7001, 0x4e7b, 0x0808}) put(w); // moveq #1,d0 / movec d0,pcr (ESS)
        // move.l #$80808000 (EDC、EBC、EIC) か #$00800000 (EBC),d0 / movec d0,cacr
        for (uint16_t w : {0x203c, o.cache ? 0x8080 : 0x0080, o.cache ? 0x8000 : 0x0000, 0x4e7b, 0x0002}) put(w);
    }
    for (uint16_t w : code) put(w);
    put(0x60fe); // bra.s *
    m->setIplRom(rom, sizeof rom);
    m->powerOn();
    if (o.maxSpeed != 100) m->setCpuSpeed(o.maxSpeed);
    return m;
}

// code を X68000 の時間で 0.1 秒動かしたあとの reg (d0 か d1) の値
uint32_t loops060(const Opt060& o, Code code, int reg) {
    x68k::Machine* m = make060(o, code);
    CHECK_EQ(m->ok(), true);
    const uint64_t c0 = m->cycles();
    const uint64_t end = c0 + m->cpuHz() / 10;
    while (m->cycles() < end) m->run((int64_t)(end - m->cycles() < 10000 ? end - m->cycles() : 10000));
    m68k::Registers r;
    m->getRegisters(r);
    delete m;
    return r.d[reg];
}

// 待つ場所と待たない場所。Machine を直接呼ぶ (Cpu020 が足す busWait060 は入らない) ので、足されるのは区切りまでの分だけ
void testSync060Regions() {
    printf("060turbo: I/O accesses end on the 1.6 us strobe\n");
    struct Case {
        uint32_t addr;
        bool sync, syncVram; // 既定と、BUS_SYNC_VRAM を足したとき
    };
    const Case cases[] = {
        {0x001000, false, false}, // メインメモリ
        {0xc00000, false, true},  // グラフィック VRAM
        {0xe00000, false, true},  // テキスト VRAM
        {0xe80000, true, true},   // CRTC
        {0xe88001, true, true},   // MFP
        {0xe8a001, true, true},   // RTC
        {0xe9a001, true, true},   // PPI (ジョイスティック)
        {0xeb0000, true, true},   // スプライト
        {0xeb8000, true, true},   // PCG
        {0xed0000, false, false}, // SRAM
        {0xf00000, false, false}, // CGROM
        {0xff0000, false, false}, // IPL ROM
    };
    for (int mhz : {50, 75}) {
        const uint64_t period = (uint64_t)mhz * 1600 / 1000; // 80 / 120 サイクル
        for (int v = 0; v < 2; v++) {
            Opt060 o;
            o.mhz = mhz;
            if (v) o.areas = x68k::Config::BUS_SYNC_IO | x68k::Config::BUS_SYNC_SPRITE | x68k::Config::BUS_SYNC_VRAM;
            x68k::Machine* m = make060(o, {});
            CHECK_EQ(m->ok(), true);
            for (const Case& c : cases) {
                for (int w = 0; w < 4; w++) { // 読み 8 / 16 ビット、書き 8 / 16 ビット
                    if (w >= 2 && (c.addr >= 0xed0000 || c.addr < 0xc00000)) continue; // 書くのは VRAM と I/O だけ
                    m->run(1237); // 区切りからずらす
                    const uint64_t c0 = m->cycles();
                    if (w == 0) m->read8(c.addr, SD);
                    else if (w == 1) m->read16(c.addr, SD);
                    else if (w == 2) m->write8(c.addr, 0, SD);
                    else m->write16(c.addr, 0, SD);
                    const uint64_t d = m->cycles() - c0;
                    if (v ? c.syncVram : c.sync) {
                        CHECK_EQ(m->cycles() % period, 0);
                        CHECK_EQ(d < period, true);
                    } else {
                        CHECK_EQ(d, 0);
                    }
                }
            }
            delete m;
        }
    }
    // 外付け SCSI ボード: SPC ($EA0000-$EA001F) は待ち、同じ 8KB にある ROM ($EA0020-$EA1FFF) は待たない
    {
        x68k::Machine* m = make060(Opt060(), {});
        static uint8_t exRom[0x1fe0];
        memset(exRom, 0x4e, sizeof exRom);
        CHECK_EQ(m->setScsiExRom(exRom, sizeof exRom), true);
        for (uint32_t addr : {0xea0001u, 0xea0020u, 0xea0100u, 0xea1ffeu}) {
            m->run(1237);
            const uint64_t c0 = m->cycles();
            m->read8(addr, SD);
            if (addr < 0xea0020) CHECK_EQ(m->cycles() % 80, 0);
            else CHECK_EQ(m->cycles() - c0, 0);
            m->run(1237);
            const uint64_t c1 = m->cycles();
            m->read16(addr & ~1u, SD);
            if (addr < 0xea0020) CHECK_EQ(m->cycles() % 80, 0);
            else CHECK_EQ(m->cycles() - c1, 0);
        }
        delete m;
    }
    // 拡張スロットのボード ($EC0000-$ECFFFF。まーきゅりーゆにっとの $ECC091) も待つ。区切りちょうどの時刻では待たない
    // (Machine を直接呼ぶと busWait060 が入らないので、そろえた直後にもう一度読むと区切りちょうどになる)
    for (int mhz : {50, 75}) {
        Opt060 o;
        o.mhz = mhz;
        x68k::Machine* m = make060(o, {});
        const uint64_t period = (uint64_t)mhz * 1600 / 1000;
        CHECK_EQ(m->setMercury(true), true);
        m->run(1237);
        m->read8(0xecc091, SD);
        const uint64_t c0 = m->cycles();
        CHECK_EQ(c0 % period, 0);
        m->read8(0xecc091, SD);
        m->read8(0xe8a001, SD);
        m->write16(0xeb0000, 0, SD);
        CHECK_EQ(m->cycles(), c0);
        m->run(1); // 区切りから少しでも進めば、次の区切りまで待つ
        const uint64_t c1 = m->cycles();
        CHECK_EQ(c1 > c0, true);
        m->read8(0xecc091, SD);
        CHECK_EQ(m->cycles() % period, 0);
        CHECK_EQ(m->cycles() > c1 || c1 % period == 0, true);
        delete m;
    }
    // 周期が 0 ならそろえない
    Opt060 off;
    off.syncNs = 0;
    x68k::Machine* m = make060(off, {});
    m->run(1237);
    const uint64_t c0 = m->cycles();
    m->read8(0xe8a001, SD);
    CHECK_EQ(m->cycles() - c0, 0);
    delete m;
}

// si.r の 2 つのループ。実機の 060turbo (50MHz) は processor 13348%、system 177.82% (docs/060turbo.md の表)。
// system は RTC の秒 ($E8A001) を読むが、ここでは値の変わらない RTC のモードのレジスタ ($E8A01B) を読む
void testSiLoops060() {
    printf("060turbo: the si.r system loop takes 1.6 us, the processor loop 1 cycle\n");
    // movea.l #$e8a01b,a0 / moveq #0,d1 / move.b (a0),d0 / addq.l #1,d1 / cmp.b (a0),d0 / beq.s (addq へ)
    const Code sys = {0x207c, 0x00e8, 0xa01b, 0x7200, 0x1010, 0x5281, 0xb010, 0x67fa};
    // movea.l #$fe0000,a0 ($FF が読める) / moveq #0,d0 / addq.l #1,d0 / tst.b (a0) / bne.s (addq へ)
    const Code proc = {0x207c, 0x00fe, 0x0000, 0x7000, 0x5280, 0x4a10, 0x66fa};
    struct Case {
        const char* name;
        Opt060 o;
    };
    Case cases[6];
    cases[0].name = "50MHz, cache on";
    cases[1].name = "50MHz, cache off";
    cases[1].o.cache = false;
    cases[2].name = "75MHz, cache on";
    cases[2].o.mhz = 75;
    cases[3].name = "75MHz, cache off";
    cases[3].o.mhz = 75;
    cases[3].o.cache = false;
    cases[4].name = "50MHz, speed060 = 50";
    cases[4].o.speed060 = 50;
    cases[5].name = "50MHz, max speed 300";
    cases[5].o.maxSpeed = 300;
    for (const Case& c : cases) {
        // 0.1 秒で 62500 周 (毎秒 625000 周 = 351472 x 177.82%)。始めの数命令の分だけ少ない
        const uint32_t n = loops060(c.o, sys, 1);
        printf("  %s: system loop %u / 0.1 s (%.2f%%)\n", c.name, n, n * 10 / 351472.0 * 100);
        CHECK_EQ(n >= 62490 && n <= 62500, true);
    }
    // そろえなければ CPU の側の時間で変わる (前の動き: 6 + 1 サイクルで 1 周)
    Opt060 off;
    off.syncNs = 0;
    const uint32_t fast = loops060(off, sys, 1);
    printf("  bus sync off: system loop %u / 0.1 s\n", fast);
    CHECK_EQ(fast > 700000 && fast < 720000, true);
    // VRAM は既定ではそろえない: グラフィック VRAM を読む同じループは速いまま
    const Code gv = {0x207c, 0x00c0, 0x0000, 0x7200, 0x1010, 0x5281, 0xb010, 0x67fa};
    CHECK_EQ(loops060(Opt060(), gv, 1), fast);
    Opt060 vram;
    vram.areas = x68k::Config::BUS_SYNC_IO | x68k::Config::BUS_SYNC_SPRITE | x68k::Config::BUS_SYNC_VRAM;
    const uint32_t slow = loops060(vram, gv, 1);
    CHECK_EQ(slow >= 62490 && slow <= 62500, true);
    // processor performance のループは 1 周 1 サイクル (50MHz の 0.1 秒で 500 万周) のまま
    const uint32_t p = loops060(Opt060(), proc, 0);
    printf("  processor loop %u / 0.1 s\n", p);
    CHECK_EQ(p > 4990000 && p <= 5000000, true);
}

// 上限を超えた時刻を戻す量 (ステートを読むときなど。state.h の stateKeptTime()) は 240 サイクルの倍数で、
// 1.6 マイクロ秒の区切り (50MHz の 80 サイクル、75MHz の 120 サイクル) の位置を変えない
void testTimeShiftKeepsPhase() {
    const uint64_t max = x68k::STATE_CPU020_CYCLES_MAX, past = x68k::STATE_CPU_TIME_PAST;
    for (uint64_t t : {max + 1, max + 79, max + max / 2, max + max / 2 + 12345, 3 * max + 7, (uint64_t)1 << 50, ((uint64_t)1 << 50) - 1}) {
        const uint64_t kept = x68k::stateKeptTime(t, max, past);
        CHECK_EQ(kept < t, true);
        CHECK_EQ((t - kept) % 240, 0);
        CHECK_EQ(kept % 40, t % 40); // 68030 の 25MHz
        CHECK_EQ(kept % 80, t % 80);
        CHECK_EQ(kept % 120, t % 120);
    }
}

// X68030 (68030 25MHz) にも同じ待ちを入れる。区切りは 40 サイクルごと (1.6 マイクロ秒)。
// 実機の X68030 の si.r は、キャッシュの有効 / 無効によらず system 177.82% / 177.81% (docs/060turbo.md の表)
void testSync030() {
    printf("X68030: I/O accesses end on the 1.6 us strobe (40 cycles)\n");
    Opt060 o30;
    o30.cpu = 68030;
    struct Case {
        uint32_t addr;
        bool sync;
    };
    const Case cases[] = {
        {0x001000, false}, {0xc00000, false}, {0xe00000, false}, {0xe80000, true},  {0xe88001, true}, {0xe8a001, true},
        {0xe9a001, true},  {0xeb0000, true},  {0xeb8000, true},  {0xed0000, false}, {0xf00000, false}, {0xff0000, false},
    };
    x68k::Machine* m = make060(o30, {});
    CHECK_EQ(m->ok(), true);
    CHECK_EQ(m->cpuHz(), 25000000u);
    for (const Case& c : cases) {
        for (int w = 0; w < 4; w++) {
            if (w >= 2 && (c.addr >= 0xed0000 || c.addr < 0xc00000)) continue;
            m->run(1237);
            const uint64_t c0 = m->cycles();
            if (w == 0) m->read8(c.addr, SD);
            else if (w == 1) m->read16(c.addr, SD);
            else if (w == 2) m->write8(c.addr, 0, SD);
            else m->write16(c.addr, 0, SD);
            if (c.sync) { // 3 クロック足してから、次の区切りまで
                CHECK_EQ(m->cycles() % 40, 0);
                CHECK_EQ(m->cycles() - c0 >= 3 && m->cycles() - c0 < 40 + 3, true);
            } else {
                CHECK_EQ(m->cycles() - c0, 0);
            }
        }
    }
    // 先に足すのは 3 クロックちょうど。一度読んで区切りちょうどの時刻にしてから、時刻を進めて読む: 区切りちょうどからは
    // 40 サイクル、区切りの 4 サイクル前からは 4 サイクル (3 足して 39、次の区切りまで 1)、3 サイクル前からは 3 サイクル
    // (3 足すと区切りちょうど)、2 サイクル前からは 42 サイクル (3 足すと区切りを 1 越える)
    for (int before : {0, 4, 3, 2}) {
        m->read8(0xe8a001, SD);
        CHECK_EQ(m->cpu020()->subCycles() % (40 * (uint64_t)m68k::Cpu020::SUB), 0u);
        m->cpu020()->addSubCycles((uint64_t)((40 - before) % 40) * m68k::Cpu020::SUB);
        const uint64_t c0 = m->cycles();
        m->read8(0xe8a001, SD);
        CHECK_EQ(m->cycles() - c0, before == 0 ? 40 : before == 2 ? 42 : before);
    }
    delete m;
    // 周期が 0 ならそろえない (前の動き)
    Opt060 off = o30;
    off.syncNs = 0;
    m = make060(off, {});
    m->run(1237);
    const uint64_t c0 = m->cycles();
    m->read8(0xe8a001, SD);
    m->write16(0xeb0000, 0, SD);
    CHECK_EQ(m->cycles() - c0, 0);
    delete m;
    // si.r の system のループは、速さ 100 でも最高速モードでも 0.1 秒に 62500 周。そろえなければ速い
    const Code sys = {0x207c, 0x00e8, 0xa01b, 0x7200, 0x1010, 0x5281, 0xb010, 0x67fa};
    const Code proc = {0x207c, 0x00fe, 0x0000, 0x7000, 0x5280, 0x4a10, 0x66fa};
    for (int speed : {100, 300}) {
        Opt060 o = o30;
        o.maxSpeed = speed;
        const uint32_t n = loops060(o, sys, 1);
        printf("  68030, speed %d: system loop %u / 0.1 s (%.2f%%)\n", speed, n, n * 10 / 351472.0 * 100);
        CHECK_EQ(n >= 62490 && n <= 62500, true);
    }
    const uint32_t fast = loops060(off, sys, 1);
    printf("  68030, bus sync off: system loop %u / 0.1 s\n", fast);
    CHECK_EQ(fast > 150000, true);
    // 32 ビットの書き込みは 16 ビットの 2 回で、区切り 2 つ分 (3.2 マイクロ秒) かかる: 0.1 秒に 31250 周。060turbo も同じ
    // movea.l #$eb0000,a0 / moveq #0,d1 / addq.l #1,d1 / move.l d0,(a0) / bra.s (addq へ)
    const Code longWrite = {0x207c, 0x00eb, 0x0000, 0x7200, 0x5281, 0x2080, 0x60fa};
    const uint32_t l30 = loops060(o30, longWrite, 1), l60 = loops060(Opt060(), longWrite, 1);
    printf("  long write to $EB0000: 68030 %u, 68060 %u / 0.1 s\n", l30, l60);
    CHECK_EQ(l30 >= 31240 && l30 <= 31250, true);
    CHECK_EQ(l60 >= 31240 && l60 <= 31250, true);
    // processor のループ (I/O を読まない) は、そろえてもそろえなくても同じ
    const uint32_t p1 = loops060(o30, proc, 0), p0 = loops060(off, proc, 0);
    printf("  68030: processor loop %u / 0.1 s\n", p1);
    CHECK_EQ(p1, p0);
}

// 場所の種類ごとの待ちの表 (Config::busArea)。終わる時刻 = (始まる時刻 + pre) を period の倍数に切り上げ + post
void testBusAreas() {
    printf("X68030 / 060turbo: per-area bus timing table\n");
    using C = x68k::Config;
    // 場所の種類。I/O は 8KB ごとのデバイスで分け、パレットとビデオコントローラ、スプライトのレジスタと BG のレジスタは
    // 同じ 8KB の中で分ける
    struct At {
        uint32_t addr;
        C::BusArea area;
    };
    const At ats[] = {
        {0x000000, C::BUS_AREA_NONE},    {0xbfffff, C::BUS_AREA_NONE},    {0xc00000, C::BUS_AREA_GVRAM},
        {0xdfffff, C::BUS_AREA_GVRAM},   {0xe00000, C::BUS_AREA_TVRAM},   {0xe7ffff, C::BUS_AREA_TVRAM},
        {0xe80000, C::BUS_AREA_CRTC},    {0xe81fff, C::BUS_AREA_CRTC},    {0xe82000, C::BUS_AREA_PALETTE},
        {0xe82200, C::BUS_AREA_PALETTE}, {0xe823ff, C::BUS_AREA_PALETTE}, {0xe82400, C::BUS_AREA_VIDEO},
        {0xe82600, C::BUS_AREA_VIDEO},   {0xe83fff, C::BUS_AREA_VIDEO},   {0xe84000, C::BUS_AREA_DMAC},
        {0xe86001, C::BUS_AREA_AREASET}, {0xe88001, C::BUS_AREA_MFP},     {0xe8a001, C::BUS_AREA_RTC},
        {0xe8c001, C::BUS_AREA_PRINTER}, {0xe8e001, C::BUS_AREA_SYSPORT}, {0xe90001, C::BUS_AREA_OPM},
        {0xe92001, C::BUS_AREA_ADPCM},   {0xe94001, C::BUS_AREA_FDC},     {0xe96001, C::BUS_AREA_HDC},
        {0xe98001, C::BUS_AREA_SCC},     {0xe9a001, C::BUS_AREA_PPI},     {0xe9c001, C::BUS_AREA_IOCTRL},
        {0xe9dfff, C::BUS_AREA_IOCTRL},  {0xe9e000, C::BUS_AREA_IO},      {0xea0001, C::BUS_AREA_IO},
        {0xeafa01, C::BUS_AREA_IO},      {0xeaffff, C::BUS_AREA_IO},      {0xeb0000, C::BUS_AREA_SPRITE},
        {0xeb07ff, C::BUS_AREA_SPRITE},  {0xeb0800, C::BUS_AREA_BGCTRL},  {0xeb0808, C::BUS_AREA_BGCTRL},
        {0xeb7fff, C::BUS_AREA_BGCTRL},  {0xeb8000, C::BUS_AREA_PCG},     {0xebbfff, C::BUS_AREA_PCG},
        {0xebc000, C::BUS_AREA_BG},      {0xebffff, C::BUS_AREA_BG},      {0xec0000, C::BUS_AREA_EXT},
        {0xecffff, C::BUS_AREA_EXT},     {0xed0000, C::BUS_AREA_NONE},    {0xffffff, C::BUS_AREA_NONE},
    };
    // 名前はどれも違う (x68k_boot の --bus-area)
    for (int i = 1; i < C::BUS_AREAS; i++) {
        CHECK_EQ(C::busAreaName(i)[0] != 0, true);
        for (int j = 1; j < i; j++) CHECK_EQ(strcmp(C::busAreaName(i), C::busAreaName(j)) != 0, true);
    }
    for (const At& a : ats) CHECK_EQ((int)C::busAreaAt(a.addr), (int)a.area);
    // 既定の表: 060turbo は I/O とスプライトが period = 1600 だけ、68030 は pre = 120 も。VRAM は 0
    for (int cpu : {68030, 68060}) {
        C cfg;
        cfg.setCpu(cpu);
        for (int i = 1; i < C::BUS_AREAS; i++) {
            const bool vram = i == C::BUS_AREA_GVRAM || i == C::BUS_AREA_TVRAM;
            for (const C::BusTiming* t : {&cfg.busArea[i].read, &cfg.busArea[i].write}) {
                CHECK_EQ(t->period, vram ? 0 : 1600);
                CHECK_EQ(t->pre, !vram && cpu == 68030 ? 120 : 0);
                CHECK_EQ(t->post, 0);
            }
        }
    }
    // 種類ごとに別の値: PCG の書き込みだけ長さの決まった 800ns (50MHz の 40 サイクル) に、BG のレジスタ ($EB0800 から) の
    // 書き込みだけ 400ns (20 サイクル) にする。PCG の読み出し、BG のレジスタと同じ 8KB にあるスプライトのレジスタ、
    // BG のテキストエリアは既定のまま (80 サイクルごとの区切り)。ビデオコントローラ ($E82400 から) を待ちなしにしても、
    // 同じ 8KB にあるパレットは既定のまま
    {
        Opt060 o;
        o.tune = [](C& cfg) {
            cfg.busArea[C::BUS_AREA_PCG].write = C::BusTiming();
            cfg.busArea[C::BUS_AREA_PCG].write.pre = 800;
            cfg.busArea[C::BUS_AREA_BGCTRL].write = C::BusTiming();
            cfg.busArea[C::BUS_AREA_BGCTRL].write.pre = 400;
            cfg.busArea[C::BUS_AREA_VIDEO] = C::BusAreaTiming();
        };
        x68k::Machine* m = make060(o, {});
        CHECK_EQ(m->busSyncKeepsPhase(), true);
        m->run(1237);
        uint64_t c0 = m->cycles();
        m->write16(0xeb8000, 0, SD);
        m->write8(0xeb8001, 0, SD);
        CHECK_EQ(m->cycles() - c0, 80);
        c0 = m->cycles();
        m->write16(0xeb0800, 0, SD);
        m->write16(0xeb0808, 0, SD);
        CHECK_EQ(m->cycles() - c0, 40);
        c0 = m->cycles();
        m->write16(0xe82600, 0, SD);
        m->read16(0xe82400, SD);
        CHECK_EQ(m->cycles() - c0, 0);
        struct Kept {
            uint32_t addr;
            bool read, write;
        };
        const Kept kept[] = {{0xeb8000, true, false}, {0xeb0000, true, true},  {0xeb0800, true, false},
                             {0xebc000, true, true},  {0xe82000, true, true},  {0xe82200, true, true}};
        for (const Kept& k : kept) {
            for (int w = 0; w < 2; w++) {
                if (!(w ? k.write : k.read)) continue;
                m->run(1237);
                c0 = m->cycles();
                if (w) m->write16(k.addr, 0, SD);
                else m->read16(k.addr, SD);
                CHECK_EQ(m->cycles() % 80, 0);
                CHECK_EQ(m->cycles() - c0 < 80, true);
            }
        }
        delete m;
    }
    // pre、period、post を全部使う: RTC の読み出しを、100ns 足して 400ns の区切りに切り上げ、さらに 200ns
    // (50MHz で 5、20、10 サイクル。75MHz で 7.5、30、15 サイクル)。書き込みは待ちなしにする。
    // 時刻は 1/SUB サイクル単位 (Cpu020::subCycles()) で比べる: 始まる時刻 + pre が区切りちょうどなら切り上げず、
    // 1 単位でも越えていれば次の区切りまで待つ
    for (int mhz : {50, 75}) {
        Opt060 o;
        o.mhz = mhz;
        o.tune = [](C& cfg) {
            C::BusTiming t;
            t.pre = 100;
            t.period = 400;
            t.post = 200;
            cfg.busArea[C::BUS_AREA_RTC].read = t;
            cfg.busArea[C::BUS_AREA_RTC].write = C::BusTiming();
        };
        x68k::Machine* m = make060(o, {});
        CHECK_EQ(m->busSyncKeepsPhase(), true);
        const uint64_t SUB = m68k::Cpu020::SUB, hz = (uint64_t)mhz * 1000000;
        const uint64_t pre = C::busNsToSub((uint32_t)hz, 100), period = C::busNsToSub((uint32_t)hz, 400),
                       post = C::busNsToSub((uint32_t)hz, 200);
        CHECK_EQ(pre, (uint64_t)mhz * SUB / 10); // 5 / 7.5 サイクル
        CHECK_EQ(period, (uint64_t)mhz * SUB * 4 / 10);
        CHECK_EQ(post, (uint64_t)mhz * SUB * 2 / 10);
        m->run(1237);
        // 始まる時刻 + pre を、区切りから rem だけ進んだ時刻にして読む
        const uint64_t rems[] = {0, 1, period - 1, period / 2, SUB / 2, pre, period - pre};
        for (uint64_t rem : rems) {
            const uint64_t now = m->cpu020()->subCycles();
            const uint64_t at = (now + pre) % period;
            m->cpu020()->addSubCycles((rem + period - at) % period);
            const uint64_t t0 = m->cpu020()->subCycles();
            CHECK_EQ((t0 + pre) % period, rem);
            m->write8(0xe8a001, 0, SD);
            CHECK_EQ(m->cpu020()->subCycles(), t0);
            m->read8(0xe8a001, SD);
            const uint64_t want = (rem ? t0 + pre + period - rem : t0 + pre) + post;
            CHECK_EQ(m->cpu020()->subCycles(), want);
            CHECK_EQ((want - post) % period, 0u);
        }
        delete m;
    }
    // ns から 1/SUB サイクル単位への変換は、大きい値でもあふれない。負の値は 0、BUS_NS_MAX (1 ミリ秒) を超える値は
    // BUS_NS_MAX として扱う
    {
        const uint64_t SUB = m68k::Cpu020::SUB;
        CHECK_EQ(C::busNsToSub(75000000, 1600), 120 * SUB);
        CHECK_EQ(C::busNsToSub(25000000, 1600), 40 * SUB);
        CHECK_EQ(C::busNsToSub(75000000, 0), 0u);
        CHECK_EQ(C::busNsToSub(75000000, -1), 0u);
        CHECK_EQ(C::busNsToSub(75000000, 1), 75000000ull * SUB / 1000000000ull); // 960
        CHECK_EQ(C::busNsToSub(75000000, C::BUS_NS_MAX - 1), 75000ull * SUB - 960);
        CHECK_EQ(C::busNsToSub(75000000, C::BUS_NS_MAX), 75000ull * SUB);
        CHECK_EQ(C::busNsToSub(75000000, C::BUS_NS_MAX + 1), 75000ull * SUB);
        CHECK_EQ(C::busNsToSub(75000000, 20000000), 75000ull * SUB);
        CHECK_EQ(C::busNsToSub(75000000, 0x7fffffff), 75000ull * SUB);
        // クロックが 10^9 の約数でないときも、割り算を分けた計算が 1 回で計算した値と同じ (端数は切り捨て)
        for (uint32_t hz : {33333333u, 0xffffffffu})
            for (int ns : {1, 7, 1599, 999999, 1000000})
                CHECK_EQ(C::busNsToSub(hz, ns), (uint64_t)((unsigned __int128)hz * SUB * (unsigned)ns / 1000000000u));
        // Machine も同じ: PCG の書き込みの pre を BUS_NS_MAX とそれより大きい値にすると、どちらも 1 回 1 ミリ秒 (75MHz の
        // 75000 サイクル)。負の値は待ちなし
        for (int v = 0; v < 3; v++) {
            Opt060 o;
            o.mhz = 75;
            o.tune = v == 0   ? +[](C& cfg) { cfg.busArea[C::BUS_AREA_PCG].write = {C::BUS_NS_MAX, 0, 0}; }
                     : v == 1 ? +[](C& cfg) { cfg.busArea[C::BUS_AREA_PCG].write = {20000000, 0, 0}; }
                              : +[](C& cfg) { cfg.busArea[C::BUS_AREA_PCG].write = {-5, 0, -5}; };
            x68k::Machine* m = make060(o, {});
            m->run(1237);
            const uint64_t t0 = m->cpu020()->subCycles();
            m->write16(0xeb8000, 0, SD);
            CHECK_EQ(m->cpu020()->subCycles() - t0, v == 2 ? 0u : 75000ull * SUB);
            delete m;
        }
    }
    // "名前=pre,period,post" の読み取り (x68k_boot の --bus-area)
    {
        C cfg;
        cfg.setCpu(68060);
        CHECK_EQ(cfg.setBusAreaText("pcg=750,0,0"), true);
        CHECK_EQ(cfg.busArea[C::BUS_AREA_PCG].read.pre, 750);
        CHECK_EQ(cfg.busArea[C::BUS_AREA_PCG].write.pre, 750);
        CHECK_EQ(cfg.busArea[C::BUS_AREA_PCG].write.period, 0);
        CHECK_EQ(cfg.setBusAreaText("rtc.r=1,2,3"), true);
        CHECK_EQ(cfg.busArea[C::BUS_AREA_RTC].read.pre, 1);
        CHECK_EQ(cfg.busArea[C::BUS_AREA_RTC].read.period, 2);
        CHECK_EQ(cfg.busArea[C::BUS_AREA_RTC].read.post, 3);
        CHECK_EQ(cfg.busArea[C::BUS_AREA_RTC].write.period, 1600);
        CHECK_EQ(cfg.setBusAreaText("bg.w=1000000,1000000,1000000"), true); // 上限ちょうど
        CHECK_EQ(cfg.busArea[C::BUS_AREA_BG].write.post, C::BUS_NS_MAX);
        CHECK_EQ(cfg.busArea[C::BUS_AREA_BG].read.period, 1600);
        // 範囲の外や形の違うものは拒み、表を変えない。4294968096 は 2^32 + 800、2147483648 は INT_MAX + 1
        const C before = cfg;
        for (const char* bad :
             {"pcg=1000001,0,0", "pcg=0,1000001,0", "pcg=0,0,1000001", "pcg=4294968096,0,0", "pcg=2147483648,0,0",
              "pcg=99999999999999999999,0,0", "pcg=-1,0,0", "pcg=+1,0,0", "pcg= 1,0,0", "pcg=1,0", "pcg=1,0,0,0", "pcg=1,0,0 ",
              "pcg=1,,0", "pcg=,0,0", "pcg=0x10,0,0", "pcg", "pcg=", "=1,0,0", "none=1,0,0", "pc=1,0,0", "pcgx=1,0,0",
              "pcg.=1,0,0", "pcg.x=1,0,0", "pcg.rw=1,0,0", ".r=1,0,0", "PCG=1,0,0", ""})
            CHECK_EQ(cfg.setBusAreaText(bad), false);
        CHECK_EQ(memcmp(&before.busArea, &cfg.busArea, sizeof cfg.busArea), 0);
    }
    // 周期が 240 サイクルを割り切らないと、時刻を直したときに区切りの位置が動く: 1200ns は 50MHz で 60 サイクル (可)、
    // 75MHz で 90 サイクル (不可)。68030 の 25MHz では 30 サイクル (可)
    for (int v = 0; v < 3; v++) {
        Opt060 o;
        if (v == 0) o.cpu = 68030;
        if (v == 2) o.mhz = 75;
        o.syncNs = 1200;
        x68k::Machine* m = make060(o, {});
        CHECK_EQ(m->busSyncKeepsPhase(), v != 2);
        delete m;
    }
    // 場所ごとに、32 ビットの書き込みを続けたときの 1 回の時間 (ns)。既定では、I/O とスプライトは区切り 2 つ分
    // (3200ns)、VRAM は最短のバスサイクル 2 回とループの分だけ。実機やほかのエミュレータの値と比べるために出す
    struct Loop {
        const char* name;
        uint16_t hi, lo;
        bool sync;
    };
    const Loop loops[] = {
        {"gvram", 0x00c0, 0x0000, false}, {"tvram", 0x00e0, 0x0000, false}, {"crtc", 0x00e8, 0x0028, true},
        {"palette", 0x00e8, 0x2000, true}, {"sprite", 0x00eb, 0x0000, true}, {"pcg", 0x00eb, 0x8000, true},
        {"bg", 0x00eb, 0xc000, true},
    };
    for (int cpu : {68030, 68060}) {
        Opt060 o;
        o.cpu = cpu;
        printf("  %d, ns per long write:", cpu);
        for (const Loop& l : loops) {
            // movea.l #addr,a0 / moveq #0,d1 / addq.l #1,d1 / move.l d0,(a0) / bra.s (addq へ)
            const uint32_t n = loops060(o, {0x207c, l.hi, l.lo, 0x7200, 0x5281, 0x2080, 0x60fa}, 1);
            printf(" %s %.0f", l.name, 1e8 / n);
            if (l.sync) CHECK_EQ(n >= 31240 && n <= 31250, true);
            else CHECK_EQ(n > 100000, true);
        }
        printf("\n");
    }
}

} // namespace

int main() {
    if (x68k::Config::has020()) {
        testSync060Regions();
        testSiLoops060();
        testSync030();
        testBusAreas();
        testTimeShiftKeepsPhase();
    }
    testRegions();
    testDisabled();
    testOutsideAccess(100);
    testOutsideAccess(300);
    testState();
    testRefresh();
    return finish();
}
