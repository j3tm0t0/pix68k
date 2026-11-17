// 68030 / 68060 のコア (Cpu020) だけの速さを測る。周辺機器は動かさず、RAM に置いた決まった命令の列を、
// Machine と同じく 256 サイクルずつ run() で動かす。
//
//   cpu020_bench [--cpu 68030|68060] [-n 命令の数 (100 万単位、既定 200)] [--slice サイクル] [プログラム...]
//
// プログラム (既定はすべて):
//   siperf  addq.l #1,d0 / tst.b (a0) / bne (si.r の processor performance と同じ形のループ)
//   copy    move.l (a0)+,(a1)+ / dbra で 4KB を写すことの繰り返し
//   call    bsr、link、movem、(d16,An) の読み書き、unlk、rts (サブルーチンの呼び出しが多いもの)
//   mix     読み書き、比較、条件分岐、シフト、ビット、jsr が混ざったループ (Dhrystone に近い混ざり方)
//
// 出る値: 1 命令あたりの時間 (ns。このプロセスが使った CPU の時間)、1 秒あたりの命令の数、68000 系の 1 命令あたりに
// ホストが実行した命令の数とサイクル数 (macOS だけ)、終わりのレジスタとサイクル数から作ったチェックサム
// (結果とサイクル数を変えない変更の前後で同じになる)。
// Pi (Linux) では次のようにビルドする (ベアメタルと同じ -O2):
//   g++ -O2 -mcpu=cortex-a76 -std=c++17 -fno-exceptions -fno-rtti -Isrc tests/bench/cpu020_bench.cpp
//       src/m68k/cpu020.cpp src/m68k/fpu.cpp src/m68k/fpu_core.cpp src/m68k/fpu_math.cpp -o cpu020_bench
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>
#include <vector>
#if defined(__APPLE__)
#include <libproc.h>
#include <unistd.h>
#endif

#include "m68k/cpu020.h"

namespace {

// このプロセスが使った CPU の時間 (秒)。ほかのプロセスに待たされた時間を入れない
double cpuSeconds() {
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
// このプロセスが実行したホストの命令の数とサイクル数 (macOS だけ。ほかは 0)
void hostCounters(uint64_t& instructions, uint64_t& cycles) {
    instructions = cycles = 0;
#if defined(__APPLE__)
    rusage_info_v4 ri;
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t*)&ri) == 0) {
        instructions = ri.ri_instructions;
        cycles = ri.ri_cycles;
    }
#endif
}

struct RamBus : m68k::Bus {
    std::vector<uint8_t> mem;
    RamBus() : mem(1 << 20) {}
    uint8_t read8(uint32_t a, int) override { return mem[a & 0xfffff]; }
    uint16_t read16(uint32_t a, int) override { return (uint16_t)((mem[a & 0xfffff] << 8) | mem[(a + 1) & 0xfffff]); }
    void write8(uint32_t a, uint8_t v, int) override { mem[a & 0xfffff] = v; }
    void write16(uint32_t a, uint16_t v, int) override {
        mem[a & 0xfffff] = (uint8_t)(v >> 8);
        mem[(a + 1) & 0xfffff] = (uint8_t)v;
    }
};

// 命令の語を並べる。分岐の変位は置いた場所から計算する
struct Asm {
    RamBus& bus;
    uint32_t pc;
    Asm(RamBus& b, uint32_t org) : bus(b), pc(org) {}
    void w(uint16_t v) {
        bus.mem[pc] = (uint8_t)(v >> 8);
        bus.mem[pc + 1] = (uint8_t)v;
        pc += 2;
    }
    void l(uint32_t v) {
        w((uint16_t)(v >> 16));
        w((uint16_t)v);
    }
    // 8 ビットの変位の分岐 (op は上位バイト。$60 BRA、$61 BSR、$66 BNE など)
    void b8(uint8_t op, uint32_t target) { w((uint16_t)((op << 8) | ((target - (pc + 2)) & 0xff))); }
    // 16 ビットの変位を持つ命令 (DBcc、JSR (d16,PC))
    void d16(uint16_t op, uint32_t target) {
        w(op);
        w((uint16_t)(target - pc));
    }
};

constexpr uint32_t ORG = 0x1000, DATA = 0x4000, DATA2 = 0x6000;

void progSiperf(Asm& a) {
    a.w(0x41f9), a.l(DATA); // lea DATA,a0
    const uint32_t loop = a.pc;
    a.w(0x5280);        // addq.l #1,d0
    a.w(0x4a10);        // tst.b (a0)
    a.b8(0x66, loop);   // bne loop
}

void progCopy(Asm& a) {
    const uint32_t outer = a.pc;
    a.w(0x41f9), a.l(DATA);  // lea DATA,a0
    a.w(0x43f9), a.l(DATA2); // lea DATA2,a1
    a.w(0x323c), a.w(1023);  // move.w #1023,d1
    const uint32_t inner = a.pc;
    a.w(0x22d8);             // move.l (a0)+,(a1)+
    a.d16(0x51c9, inner);    // dbra d1,inner
    a.b8(0x60, outer);       // bra outer
}

void progCall(Asm& a) {
    const uint32_t loop = a.pc;
    a.w(0x7005);                 // moveq #5,d0
    const uint32_t bsr = a.pc;
    a.w(0x6100);                 // bsr sub (あとで変位を入れる)
    a.w(0x5287);                 // addq.l #1,d7
    a.b8(0x60, loop);            // bra loop
    const uint32_t sub = a.pc;
    a.bus.mem[bsr + 1] = (uint8_t)(sub - (bsr + 2));
    a.w(0x4e56), a.w(0xfff8);    // link a6,#-8
    a.w(0x48e7), a.w(0x3020);    // movem.l d2-d3/a2,-(sp)
    a.w(0x2400);                 // move.l d0,d2
    a.w(0xd482);                 // add.l d2,d2
    a.w(0x2d42), a.w(0xfffc);    // move.l d2,-4(a6)
    a.w(0x262e), a.w(0xfffc);    // move.l -4(a6),d3
    a.w(0x45ea), a.w(0x0004);    // lea 4(a2),a2
    a.w(0x4cdf), a.w(0x040c);    // movem.l (sp)+,d2-d3/a2
    a.w(0x4e5e);                 // unlk a6
    a.w(0x4e75);                 // rts
}

void progMix(Asm& a) {
    const uint32_t start = a.pc;
    a.w(0x41f9), a.l(DATA);      // lea DATA,a0
    a.w(0x43f9), a.l(DATA2);     // lea DATA2,a1
    a.w(0x7000);                 // moveq #0,d0
    a.w(0x723f);                 // moveq #63,d1
    const uint32_t l1 = a.pc;
    a.w(0x3418);                 // move.w (a0)+,d2
    a.w(0x48c2);                 // ext.l d2
    a.w(0xd082);                 // add.l d2,d0
    a.w(0xb47c), a.w(0x0100);    // cmp.w #$100,d2
    a.w(0x6506);                 // bcs skip
    a.w(0xe288);                 // lsr.l #1,d0
    a.w(0x5269), a.w(0x0008);    // addq.w #1,8(a1)
    // skip:
    a.w(0x2280);                 // move.l d0,(a1)
    a.w(0x2629), a.w(0x0004);    // move.l 4(a1),d3
    a.w(0xc6bc), a.l(0x00ff00ff); // and.l #$00ff00ff,d3
    a.w(0x2343), a.w(0x0004);    // move.l d3,4(a1)
    a.w(0x0802), a.w(0x0000);    // btst #0,d2
    a.w(0x6702);                 // beq even
    a.w(0x5284);                 // addq.l #1,d4
    // even:
    a.w(0x1a28), a.w(0x0001);    // move.b 1(a0),d5
    a.w(0xba11);                 // cmp.b (a1),d5
    a.w(0x56c6);                 // sne d6
    const uint32_t jsr = a.pc;
    a.w(0x4eba), a.w(0);         // jsr sub(pc) (あとで変位を入れる)
    a.d16(0x51c9, l1);           // dbra d1,l1
    a.b8(0x60, start);           // bra start
    const uint32_t sub = a.pc;
    const uint16_t disp = (uint16_t)(sub - (jsr + 2));
    a.bus.mem[jsr + 2] = (uint8_t)(disp >> 8);
    a.bus.mem[jsr + 3] = (uint8_t)disp;
    a.w(0x2f00);                 // move.l d0,-(sp)
    a.w(0x4a80);                 // tst.l d0
    a.w(0x6b02);                 // bmi neg
    a.w(0x4840);                 // swap d0
    // neg:
    a.w(0x201f);                 // move.l (sp)+,d0
    a.w(0x4e75);                 // rts
}

struct Prog {
    const char* name;
    void (*build)(Asm&);
};
const Prog kProgs[] = {{"siperf", progSiperf}, {"copy", progCopy}, {"call", progCall}, {"mix", progMix}};

void setup(RamBus& bus, m68k::Cpu020& cpu, const Prog& p) {
    memset(bus.mem.data(), 0, bus.mem.size());
    for (uint32_t i = 0; i < 0x2000; i++) bus.mem[DATA + i] = (uint8_t)(i * 37 + 1) | 1; // 0 でないデータ
    Asm a(bus, ORG);
    p.build(a);
    // 68060 は、はじめに命令キャッシュとデータキャッシュを有効にする (060turbo.sys を組み込んだあとと同じ。
    // 無効のままだと、読み書きのたびにバスの時間を足す経路を通る)
    uint32_t start = ORG;
    if (cpu.is060()) {
        start = ORG - 12;
        Asm pre(bus, start);
        pre.w(0x203c), pre.l(0x80008000); // move.l #$80008000,d0 (EDC | EIC)
        pre.w(0x4e7b), pre.w(0x0002);     // movec d0,cacr
        pre.w(0x7000);                    // moveq #0,d0
    }
    bus.mem[0] = 0, bus.mem[1] = 0, bus.mem[2] = 0x80, bus.mem[3] = 0x00; // SSP = $8000
    bus.mem[4] = 0, bus.mem[5] = 0, bus.mem[6] = (uint8_t)(start >> 8), bus.mem[7] = (uint8_t)start;
    for (int pg = 0; pg < 16; pg++) {
        cpu.setFastPage(true, pg, &bus.mem[(size_t)pg << 16], &bus.mem[(size_t)pg << 16], false);
        cpu.setFastPage(false, pg, &bus.mem[(size_t)pg << 16], &bus.mem[(size_t)pg << 16], false);
    }
    cpu.flushTlb();
    cpu.reset();
}

} // namespace

int main(int argc, char** argv) {
    m68k::CpuModel model = m68k::CpuModel::M68060;
    long long millions = 200;
    int slice = 256;
    std::vector<const Prog*> sel;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cpu") && i + 1 < argc) {
            model = atoi(argv[++i]) == 68030 ? m68k::CpuModel::M68030 : m68k::CpuModel::M68060;
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            millions = atoll(argv[++i]);
        } else if (!strcmp(argv[i], "--slice") && i + 1 < argc) {
            slice = atoi(argv[++i]);
        } else {
            const Prog* f = nullptr;
            for (const Prog& p : kProgs)
                if (!strcmp(argv[i], p.name)) f = &p;
            if (!f) {
                fprintf(stderr, "usage: cpu020_bench [--cpu 68030|68060] [-n millions] [--slice cycles] [siperf|copy|call|mix]...\n");
                return 2;
            }
            sel.push_back(f);
        }
    }
    if (sel.empty())
        for (const Prog& p : kProgs) sel.push_back(&p);
    if (millions < 1 || slice < 1) return 2;
    printf("%s, slice %d cycles\n", model == m68k::CpuModel::M68060 ? "68060" : "68030", slice);
    for (const Prog* p : sel) {
        RamBus bus;
        // 命令の数を数える: はじめの 100 万命令を step() で動かし、かかったサイクル数から全体のサイクル数を決める
        uint64_t cyclesPerMillion;
        {
            m68k::Cpu020 cpu(&bus, model);
            setup(bus, cpu, *p);
            const uint64_t c0 = cpu.cycles();
            for (int i = 0; i < 1000000; i++) cpu.step();
            cyclesPerMillion = cpu.cycles() - c0;
        }
        m68k::Cpu020 cpu(&bus, model);
        setup(bus, cpu, *p);
        const uint64_t total = cyclesPerMillion * (uint64_t)millions;
        const uint64_t c0 = cpu.cycles();
        uint64_t hi0, hc0, hi1, hc1;
        hostCounters(hi0, hc0);
        const double t0 = cpuSeconds();
        while (cpu.cycles() - c0 < total) cpu.run(slice);
        const double sec = cpuSeconds() - t0;
        hostCounters(hi1, hc1);
        const double instr = (double)millions * 1e6; // 概数 (はじめの 100 万命令と同じ割合として)
        m68k::Registers r;
        cpu.getRegisters(r);
        uint32_t sum = (uint32_t)cpu.cycles() * 2654435761u;
        for (int i = 0; i < 8; i++) sum = (sum ^ r.d[i]) * 16777619u;
        for (int i = 0; i < 7; i++) sum = (sum ^ r.a[i]) * 16777619u;
        sum = (sum ^ r.ssp ^ r.pc ^ r.sr) * 16777619u;
        for (uint32_t i = 0; i < 0x8000; i++) sum = (sum ^ bus.mem[i]) * 16777619u;
        printf("%-7s %6.2f ns/instr  %7.1f M instr/s  (%.2f cycles/instr)", p->name, sec / instr * 1e9, instr / sec / 1e6,
               (double)cyclesPerMillion / 1e6);
        if (hi1 > hi0) printf("  host %.1f instr %.1f cycles", (double)(hi1 - hi0) / instr, (double)(hc1 - hc0) / instr);
        printf("  checksum %08x\n", sum);
    }
    return 0;
}
