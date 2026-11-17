// SingleStepTests にない動作 (例外、割り込み、STOP、トレース、RESET、バスエラー) のテスト
#include <stdio.h>
#include <string.h>

#include "m68k/m68k.h"

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

class Mem : public m68k::Bus {
public:
    uint8_t m[1 << 24];
    m68k::Cpu* cpu = nullptr;
    uint32_t busErrorAddr = 0xffffffff;
    int ackVector = -1;
    int lastAckLevel = -1;
    int resets = 0;
    int lastFc = -1;
    bool berrOnAck = false;
    int writes = 0;
    uint32_t fcProbeAddr = 0xffffffff;
    int fcProbe = -1;

    uint8_t read8(uint32_t a, int fc) override {
        lastFc = fc;
        if (a == busErrorAddr) cpu->busError(a, false, fc);
        return m[a];
    }
    uint16_t read16(uint32_t a, int fc) override {
        lastFc = fc;
        if (a == fcProbeAddr) fcProbe = fc;
        if (a == busErrorAddr) cpu->busError(a, false, fc);
        return (uint16_t)((m[a] << 8) | m[a + 1]);
    }
    void write8(uint32_t a, uint8_t v, int fc) override {
        lastFc = fc;
        if (a == busErrorAddr) cpu->busError(a, true, fc);
        m[a] = v;
    }
    void write16(uint32_t a, uint16_t v, int fc) override {
        lastFc = fc;
        writes++;
        if (a == busErrorAddr) cpu->busError(a, true, fc);
        m[a] = (uint8_t)(v >> 8);
        m[a + 1] = (uint8_t)v;
    }
    int interruptAck(int level) override {
        lastAckLevel = level;
        if (berrOnAck) cpu->busError(0xfffff0 | (level << 1), false, m68k::FC_IACK);
        return ackVector;
    }
    void resetDevices() override { resets++; }

    void w16(uint32_t a, uint16_t v) {
        m[a] = (uint8_t)(v >> 8);
        m[a + 1] = (uint8_t)v;
    }
    void w32(uint32_t a, uint32_t v) {
        w16(a, (uint16_t)(v >> 16));
        w16(a + 2, (uint16_t)v);
    }
    uint16_t r16(uint32_t a) const { return (uint16_t)((m[a] << 8) | m[a + 1]); }
    uint32_t r32(uint32_t a) const { return ((uint32_t)r16(a) << 16) | r16(a + 2); }
};

Mem mem;

// 0 番地にベクタ (SSP=0x8000, PC=0x1000)、各例外ベクタ n を 0x4000 + n*0x10 に向ける
m68k::Cpu& setup() {
    static m68k::Cpu* cpu = nullptr;
    memset(mem.m, 0, sizeof mem.m);
    mem.busErrorAddr = 0xffffffff;
    mem.ackVector = -1;
    mem.lastAckLevel = -1;
    mem.resets = 0;
    mem.berrOnAck = false;
    mem.writes = 0;
    mem.fcProbeAddr = 0xffffffff;
    mem.fcProbe = -1;
    mem.w32(0, 0x8000);
    mem.w32(4, 0x1000);
    for (int v = 2; v < 256; v++) {
        mem.w32(v * 4, 0x4000 + v * 0x10);
        mem.w16(0x4000 + v * 0x10, 0x4e71); // 各ハンドラは NOP
    }
    if (!cpu) cpu = new m68k::Cpu(&mem);
    mem.cpu = cpu;
    return *cpu;
}

void startAt(m68k::Cpu& cpu, uint16_t sr, uint32_t usp = 0x7000) {
    m68k::Registers r;
    memset(&r, 0, sizeof r);
    r.ssp = 0x8000;
    r.usp = usp;
    r.sr = sr;
    r.pc = 0x1000;
    r.prefetch[0] = mem.r16(0x1000);
    r.prefetch[1] = mem.r16(0x1002);
    cpu.setRegisters(r);
}

m68k::Registers regs(m68k::Cpu& cpu) {
    m68k::Registers r;
    cpu.getRegisters(r);
    return r;
}

// グループ 1/2 の例外フレーム (SR, PC) を確かめる
void checkFrame(uint32_t sp, uint16_t sr, uint32_t pc) {
    CHECK_EQ(mem.r16(sp), sr);
    CHECK_EQ(mem.r32(sp + 2), pc);
}

void testReset() {
    m68k::Cpu& cpu = setup();
    mem.w16(0x1000, 0x7001); // MOVEQ #1,D0
    cpu.reset();
    m68k::Registers r = regs(cpu);
    CHECK_EQ(r.ssp, 0x8000);
    CHECK_EQ(r.pc, 0x1000);
    CHECK_EQ(r.sr, 0x2700);
    cpu.step();
    CHECK_EQ(regs(cpu).d[0], 1);
}

void testLineAF() {
    m68k::Cpu& cpu = setup();
    mem.w16(0x1000, 0xff12); // Human68k の DOS コール ($FFxx) は F ライン例外になる
    startAt(cpu, 0x0015);   // ユーザーモード
    int c = cpu.step();
    m68k::Registers r = regs(cpu);
    CHECK_EQ(r.pc, 0x4000 + 11 * 0x10);
    CHECK_EQ(r.sr & 0x2000, 0x2000);
    CHECK_EQ(r.ssp, 0x8000 - 6);
    CHECK_EQ(r.usp, 0x7000);
    checkFrame(r.ssp, 0x0015, 0x1000); // 積む PC は命令自身のアドレス
    CHECK_EQ(c, 34);

    setup();
    mem.w16(0x1000, 0xa123);
    startAt(cpu, 0x2700);
    cpu.step();
    r = regs(cpu);
    CHECK_EQ(r.pc, 0x4000 + 10 * 0x10);
    checkFrame(r.ssp, 0x2700, 0x1000);
}

void testIllegal() {
    m68k::Cpu& cpu = setup();
    const uint16_t ops[] = {0x4afc, 0x4e74 /* RTD (68010) */, 0x4e7a /* MOVEC */, 0x49c0 /* EXTB (68020) */};
    for (uint16_t op : ops) {
        setup();
        mem.w16(0x1000, op);
        startAt(cpu, 0x2700);
        cpu.step();
        m68k::Registers r = regs(cpu);
        CHECK_EQ(r.pc, 0x4000 + 4 * 0x10);
        checkFrame(r.ssp, 0x2700, 0x1000);
    }
}

void testPrivilege() {
    m68k::Cpu& cpu = setup();
    mem.w16(0x1000, 0x46fc); // MOVE #$2000,SR
    mem.w16(0x1002, 0x2000);
    startAt(cpu, 0x0000);
    cpu.step();
    m68k::Registers r = regs(cpu);
    CHECK_EQ(r.pc, 0x4000 + 8 * 0x10);
    CHECK_EQ(r.sr, 0x2000);
    checkFrame(r.ssp, 0x0000, 0x1000);

    const uint16_t privOps[] = {0x4e73 /* RTE */, 0x4e70 /* RESET */, 0x4e72 /* STOP */, 0x4e60 /* MOVE A0,USP */,
                                0x007c /* ORI to SR */};
    for (uint16_t op : privOps) {
        setup();
        mem.w16(0x1000, op);
        startAt(cpu, 0x0000);
        cpu.step();
        CHECK_EQ(regs(cpu).pc, 0x4000 + 8 * 0x10);
    }
}

void testTrap() {
    m68k::Cpu& cpu = setup();
    mem.w16(0x1000, 0x4e45); // TRAP #5
    startAt(cpu, 0x2004);
    int c = cpu.step();
    m68k::Registers r = regs(cpu);
    CHECK_EQ(r.pc, 0x4000 + 37 * 0x10);
    checkFrame(r.ssp, 0x2004, 0x1002); // 次の命令のアドレス
    CHECK_EQ(c, 34);
}

void testAutovectorInterrupt() {
    m68k::Cpu& cpu = setup();
    mem.w16(0x1000, 0x4e71);
    mem.w16(0x1002, 0x4e71);
    startAt(cpu, 0x2200);
    cpu.setIPL(2); // マスク 2 と同じなので受け付けない
    cpu.step();
    CHECK_EQ(regs(cpu).pc, 0x1002);
    cpu.setIPL(3);
    cpu.step();
    m68k::Registers r = regs(cpu);
    CHECK_EQ(mem.lastAckLevel, 3);
    CHECK_EQ(r.pc, 0x4000 + 27 * 0x10);
    CHECK_EQ(r.sr, 0x2300);
    checkFrame(r.ssp, 0x2200, 0x1002);
    cpu.setIPL(0);
}

void testVectoredInterrupt() {
    m68k::Cpu& cpu = setup();
    mem.w16(0x1000, 0x4e71);
    mem.ackVector = 0x40; // MFP のようにベクタ番号を返す
    startAt(cpu, 0x0000, 0x7000);
    cpu.setIPL(6);
    cpu.step();
    m68k::Registers r = regs(cpu);
    CHECK_EQ(r.pc, 0x4000 + 0x40 * 0x10);
    CHECK_EQ(r.sr, 0x2600);
    CHECK_EQ(r.usp, 0x7000);
    checkFrame(r.ssp, 0x0000, 0x1000);
    cpu.setIPL(0);
}

void testNmi() {
    m68k::Cpu& cpu = setup();
    for (int i = 0; i < 8; i++) mem.w16(0x1000 + i * 2, 0x4e71);
    startAt(cpu, 0x2700);
    cpu.setIPL(7); // マスク 7 でもレベル 7 は立ち上がりで受け付ける
    cpu.step();
    CHECK_EQ(regs(cpu).pc, 0x4000 + 31 * 0x10);
    cpu.step(); // ハンドラの NOP。レベル 7 のままでも再度は受け付けない
    CHECK_EQ(regs(cpu).pc, 0x4000 + 31 * 0x10 + 2);
    cpu.setIPL(0);
}

void testStop() {
    m68k::Cpu& cpu = setup();
    mem.w16(0x1000, 0x4e72); // STOP #$2000
    mem.w16(0x1002, 0x2000);
    mem.w16(0x1004, 0x4e71);
    startAt(cpu, 0x2700);
    cpu.step();
    CHECK_EQ(cpu.stopped(), true);
    CHECK_EQ(regs(cpu).sr, 0x2000);
    uint32_t pc = regs(cpu).pc;
    cpu.step();
    CHECK_EQ(regs(cpu).pc, pc); // 停止中は進まない
    cpu.setIPL(1);
    cpu.step();
    CHECK_EQ(cpu.stopped(), false);
    m68k::Registers r = regs(cpu);
    CHECK_EQ(r.pc, 0x4000 + 25 * 0x10);
    checkFrame(r.ssp, 0x2000, 0x1004); // STOP の次から再開する
    cpu.setIPL(0);
}

void testTrace() {
    m68k::Cpu& cpu = setup();
    mem.w16(0x1000, 0x7005); // MOVEQ #5,D0
    startAt(cpu, 0xa700);    // T=1
    int c = cpu.step();
    CHECK_EQ(c, 4 + 34);
    m68k::Registers r = regs(cpu);
    CHECK_EQ(r.d[0], 5);
    CHECK_EQ(r.pc, 0x4000 + 9 * 0x10);
    CHECK_EQ(r.sr & 0x8000, 0);
    checkFrame(r.ssp, 0xa700, 0x1002);
}

void testTraceNotAfterLineF() {
    m68k::Cpu& cpu = setup();
    mem.w16(0x1000, 0xf000);
    startAt(cpu, 0xa700); // T=1。実行されなかった命令のあとはトレース例外を起こさない
    cpu.step();
    m68k::Registers r = regs(cpu);
    CHECK_EQ(r.pc, 0x4000 + 11 * 0x10);
    CHECK_EQ(r.ssp, 0x8000 - 6);
}

void testDivideByZero() {
    m68k::Cpu& cpu = setup();
    mem.w16(0x1000, 0x80c1); // DIVU D1,D0
    startAt(cpu, 0x271f);
    m68k::Registers r = regs(cpu);
    r.d[0] = 0x80001234;
    r.d[1] = 0;
    cpu.setRegisters(r);
    cpu.step();
    r = regs(cpu);
    CHECK_EQ(r.pc, 0x4000 + 5 * 0x10);
    checkFrame(r.ssp, 0x2718, 0x1002); // N=上位語<0, Z=0, V=C=0。積む PC は次の命令
    CHECK_EQ(r.d[0], 0x80001234);

    setup();
    mem.w16(0x1000, 0x81fc); // DIVS #0,D0
    mem.w16(0x1002, 0x0000);
    startAt(cpu, 0x271b);
    cpu.step();
    r = regs(cpu);
    CHECK_EQ(r.pc, 0x4000 + 5 * 0x10);
    checkFrame(r.ssp, 0x2714, 0x1004); // Z=1, N=V=C=0
}

void testTraceStop() {
    m68k::Cpu& cpu = setup();
    mem.w16(0x1000, 0x4e72); // STOP #$A700 (T=1 のまま)
    mem.w16(0x1002, 0xa700);
    startAt(cpu, 0xa700);
    cpu.step(); // STOP のあとトレース例外に入り、停止状態から抜ける
    CHECK_EQ(cpu.stopped(), false);
    CHECK_EQ(regs(cpu).pc, 0x4000 + 9 * 0x10);
    cpu.step(); // ハンドラの NOP が実行される
    CHECK_EQ(regs(cpu).pc, 0x4000 + 9 * 0x10 + 2);
}

void testResetBusError() {
    m68k::Cpu& cpu = setup();
    mem.busErrorAddr = 0x000004; // リセットベクタ (PC) の読み出しでバスエラー
    mem.writes = 0;
    cpu.reset();
    CHECK_EQ(cpu.halted(), true);
    CHECK_EQ(mem.writes, 0); // スタックフレームを積まない
}

void testSpuriousInterrupt() {
    m68k::Cpu& cpu = setup();
    mem.w16(0x1000, 0x4e71);
    mem.berrOnAck = true;
    startAt(cpu, 0x2000);
    cpu.setIPL(3);
    cpu.step();
    m68k::Registers r = regs(cpu);
    CHECK_EQ(r.pc, 0x4000 + 24 * 0x10);
    CHECK_EQ(r.ssp, 0x8000 - 6);
    CHECK_EQ(r.sr, 0x2300);
    checkFrame(r.ssp, 0x2000, 0x1000);
    cpu.setIPL(0);
}

void testPcRelativeFc() {
    m68k::Cpu& cpu = setup();
    mem.w16(0x1000, 0x303a); // MOVE.W ($0FFE,PC),D0 → $2000
    mem.w16(0x1002, 0x0ffe);
    mem.w16(0x2000, 0x1234);
    mem.fcProbeAddr = 0x2000;
    startAt(cpu, 0x2700);
    cpu.step();
    CHECK_EQ(regs(cpu).d[0] & 0xffff, 0x1234);
    CHECK_EQ(mem.fcProbe, m68k::FC_SUPER_PROG);
}

void testResetInstruction() {
    m68k::Cpu& cpu = setup();
    mem.w16(0x1000, 0x4e70);
    startAt(cpu, 0x2700);
    int c = cpu.step();
    CHECK_EQ(mem.resets, 1);
    CHECK_EQ(c, 132);
    CHECK_EQ(regs(cpu).pc, 0x1002);
}

void testBusError() {
    m68k::Cpu& cpu = setup();
    mem.w16(0x1000, 0x3039); // MOVE.W $00E80000,D0
    mem.w32(0x1002, 0x00e80000);
    mem.busErrorAddr = 0xe80000;
    startAt(cpu, 0x2700);
    cpu.step();
    m68k::Registers r = regs(cpu);
    CHECK_EQ(r.pc, 0x4000 + 2 * 0x10);
    CHECK_EQ(r.ssp, 0x8000 - 14);
    CHECK_EQ(mem.r32(r.ssp + 2), 0xe80000); // アクセスしたアドレス
    CHECK_EQ(mem.r16(r.ssp + 6), 0x3039);   // 命令
    CHECK_EQ(mem.r16(r.ssp) & 0x1f, 0x15);  // 読み出し、スーパーバイザデータ
}

void testDoubleFault() {
    m68k::Cpu& cpu = setup();
    mem.w16(0x1000, 0x3010); // MOVE.W (A0),D0  A0 は奇数
    startAt(cpu, 0x2700);
    m68k::Registers r = regs(cpu);
    r.a[0] = 0x2001;
    r.ssp = 0x7fff; // スタックも奇数なので例外処理中に再びアドレスエラー
    cpu.setRegisters(r);
    cpu.step();
    CHECK_EQ(cpu.halted(), true);
}

void testUserFunctionCodes() {
    m68k::Cpu& cpu = setup();
    mem.w16(0x1000, 0x3010); // MOVE.W (A0),D0
    startAt(cpu, 0x0000);
    m68k::Registers r = regs(cpu);
    r.a[0] = 0x2000;
    cpu.setRegisters(r);
    cpu.step();
    // 最後のアクセスはプリフェッチ (ユーザープログラム)
    CHECK_EQ(mem.lastFc, m68k::FC_USER_PROG);
}

struct Case {
    const char* name;
    void (*fn)();
};

} // namespace

int main() {
    const Case cases[] = {
        {"reset", testReset},
        {"line A / line F", testLineAF},
        {"illegal", testIllegal},
        {"privilege violation", testPrivilege},
        {"trap", testTrap},
        {"autovector interrupt", testAutovectorInterrupt},
        {"vectored interrupt", testVectoredInterrupt},
        {"nmi", testNmi},
        {"stop", testStop},
        {"trace", testTrace},
        {"trace not after line F", testTraceNotAfterLineF},
        {"divide by zero", testDivideByZero},
        {"trace STOP", testTraceStop},
        {"bus error during reset", testResetBusError},
        {"spurious interrupt", testSpuriousInterrupt},
        {"pc-relative fc", testPcRelativeFc},
        {"reset instruction", testResetInstruction},
        {"bus error", testBusError},
        {"double fault", testDoubleFault},
        {"user function codes", testUserFunctionCodes},
    };
    for (const Case& c : cases) {
        int before = failures;
        c.fn();
        printf("%-24s %s\n", c.name, failures == before ? "ok" : "FAIL");
    }
    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
