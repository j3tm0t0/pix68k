// 68030 / 68060 で、MFP のタイマ (ディレイモード) と OPM のタイマの割り込みが、要求の出た命令の切れ目で CPU に届くこと
// (Machine::nextTimerIrq()、timersChanged())。
// 68030 / 68060 は、周辺機器を進める間隔の上限が X68000 の時間で 25.6us ある (Machine::run())。タイマが要求を出す時刻で
// 区切らないと、割り込みが届くのが最大で 25.6us 遅れる。
// 遅れは X68000 の側で測る: 割り込みの処理ルーチンの最初の命令で、MFP のタイマのカウンタ (1us ごとに 1 減る) を読む。
// IPL ROM は使わない (RAM に置いたプログラムを動かす)
#include <stdio.h>
#include <string.h>

#include <vector>

#include "test_check.h"
#include "x68k/machine.h"

namespace {

constexpr int SD = m68k::FC_SUPER_DATA;
constexpr uint32_t CODE = 0x1000, HANDLER = 0x1800, FLAG = 0x2800, LOG = 0x3000;
// MFP のレジスタ ($E88001 + 番号 * 2) と OPM
constexpr uint32_t IERB = 0xe88009, IMRB = 0xe88015, VR = 0xe88017, TCDCR = 0xe8801d, TCDR = 0xe88023, TDDR = 0xe88025;
constexpr uint32_t OPM_ADDR = 0xe90001, OPM_DATA = 0xe90003;

// IPL ROM: SSP $2000、PC $FE0100 (bra.s *)
void makeRom(uint8_t* rom) {
    memset(rom, 0xff, x68k::Machine::IPL_SIZE);
    const uint8_t vec[8] = {0x00, 0x00, 0x20, 0x00, 0x00, 0xfe, 0x01, 0x00};
    memcpy(rom + 0x10000, vec, 8);
    rom[0x100] = 0x60;
    rom[0x101] = 0xfe;
}

// 命令の語を並べる
struct Code {
    std::vector<uint16_t> w;
    void put(std::initializer_list<uint16_t> ws) { w.insert(w.end(), ws); }
    void abs(uint32_t a) { put({(uint16_t)(a >> 16), (uint16_t)a}); }
    void moveb(uint8_t v, uint32_t a) { // MOVE.B #v,a
        put({0x13fc, v});
        abs(a);
    }
    void opm(uint8_t reg, uint8_t v) {
        moveb(reg, OPM_ADDR);
        moveb(v, OPM_DATA);
    }
    void vector(int n) { // MOVE.L #HANDLER,n*4
        put({0x23fc});
        abs(HANDLER);
        abs((uint32_t)n * 4);
    }
    void logByte(uint32_t a) { // MOVE.B a,(A1)+
        put({0x12f9});
        abs(a);
    }
    void start() { // LEA LOG,A1; MOVE #$2000,SR
        put({0x43f9});
        abs(LOG);
        put({0x46fc, 0x2000});
    }
    // FLAG が 0 でなくなるのを待ち、0 に戻し、DBRA で count 回まわって待つ。先頭の番地 (語の位置) を返す
    size_t waitFlag(uint16_t count) {
        const size_t top = w.size();
        put({0x4a39}); // TST.B FLAG
        abs(FLAG);
        put({0x67f8}); // BEQ.S top
        put({0x4239}); // CLR.B FLAG
        abs(FLAG);
        put({0x323c, count, 0x51c9, 0xfffe}); // MOVE.W #count,D1; DBRA D1,*
        return top;
    }
    void braTo(size_t top) { // BRA.S top
        const int disp = ((int)top - (int)w.size() - 1) * 2;
        put({(uint16_t)(0x6000 | (disp & 0xff))});
    }
    void setFlag() { // ST FLAG
        put({0x50f9});
        abs(FLAG);
    }
};

// main を CODE、handler を HANDLER に置いて動かし、処理ルーチンが LOG に書いたバイトを n 個集める
std::vector<uint8_t> run(int cpu, const Code& main, const Code& handler, size_t n) {
    x68k::Config cfg;
    cfg.setCpu(cpu);
    x68k::Machine* m = new x68k::Machine(cfg);
    static uint8_t rom[x68k::Machine::IPL_SIZE];
    makeRom(rom);
    m->setIplRom(rom, sizeof rom);
    m->powerOn();
    for (size_t i = 0; i < main.w.size(); i++) m->write16(CODE + 2 * (uint32_t)i, main.w[i], SD);
    for (size_t i = 0; i < handler.w.size(); i++) m->write16(HANDLER + 2 * (uint32_t)i, handler.w[i], SD);
    m68k::Registers r;
    m->getRegisters(r);
    r.pc = CODE;
    r.sr = 0x2700;
    m->setRegisters(r);
    // 1ms ずつ、長くても X68000 の時間で 2 秒
    for (int i = 0; i < 2000; i++) {
        m->run(cfg.cpuHz / 1000);
        m->getRegisters(r);
        if (r.a[1] >= LOG + n && r.a[1] < LOG + 0x1000) break;
    }
    std::vector<uint8_t> out;
    bool ok;
    m->getRegisters(r);
    for (uint32_t a = LOG; a < r.a[1] && out.size() < n && a < LOG + 0x1000; a++) out.push_back(m->peek8(a, ok));
    delete m;
    return out;
}

void show(const char* name, int cpu, const std::vector<uint8_t>& v) {
    printf("%d %s:", cpu, name);
    for (uint8_t b : v) printf(" %d", b);
    printf("\n");
}

// タイマ C を 200us の周期で動かし続ける。処理ルーチンで読むカウンタは、要求が出たときに 200 に戻り、1us ごとに減る。
// 200 から減った分が、要求が出てから処理ルーチンの最初の命令が読むまでの時間 (us)
void testPeriodic(int cpu) {
    Code main, handler;
    main.vector(0x45);
    main.moveb(0x40, VR);
    main.moveb(200, TCDR);
    main.moveb(0x20, IERB);
    main.moveb(0x20, IMRB);
    main.moveb(0x10, TCDCR); // タイマ C: 1/4 (1us)
    main.start();
    const size_t top = main.w.size();
    main.put({0x5280, 0xc4c0, 0x4e71}); // ADDQ.L #1,D0; MULU.W D0,D2; NOP
    main.braTo(top);
    handler.logByte(TCDR);
    handler.put({0x4e73});
    const std::vector<uint8_t> v = run(cpu, main, handler, 40);
    show("periodic MFP timer C, counter in the handler", cpu, v);
    CHECK_EQ(v.size(), 40u);
    bool inRange = true; // 200 を超える値や、次の周期まで待たされた値も通さない
    for (uint8_t b : v) inRange = inRange && 197 <= b && b <= 200;
    CHECK_EQ(inRange, true);
}

// 止めてあるタイマ C を、メインのループが 12us の設定で動かす (その前に周辺機器を進めたときには、動いているタイマは
// なかった)。処理ルーチンはカウンタを読んでからタイマを止める。読むカウンタは 12 から減った分が遅れ
void testRestart(int cpu) {
    Code main, handler;
    main.vector(0x45);
    main.moveb(0x40, VR);
    main.moveb(0x20, IERB);
    main.moveb(0x20, IMRB);
    main.setFlag();
    main.start();
    const size_t top = main.waitFlag(1000);
    main.moveb(12, TCDR);
    main.moveb(0x10, TCDCR);
    main.braTo(top);
    handler.logByte(TCDR);
    handler.moveb(0x00, TCDCR);
    handler.setFlag();
    handler.put({0x4e73});
    const std::vector<uint8_t> v = run(cpu, main, handler, 40);
    show("MFP timer C started by the main loop, counter in the handler", cpu, v);
    CHECK_EQ(v.size(), 40u);
    bool inRange = true;
    for (uint8_t b : v) inRange = inRange && 9 <= b && b <= 12;
    CHECK_EQ(inRange, true);
}

// OPM のタイマの要求は、OPM の 1 サンプル (16us) の境目で出る。止めずに動かし続けるタイマ D (1us ごとに 1 減る) を
// 処理ルーチンで読むと、16 で割った余りがいつも同じ値の近くになる。v の余りが、幅 width の範囲に収まるか
bool samePhase(const std::vector<uint8_t>& v, int width) {
    for (int base = 0; base < 16; base++) {
        bool all = true;
        for (uint8_t b : v) all = all && ((b - base) & 15) <= width;
        if (all) return true;
    }
    return false;
}

// OPM のタイマ A を 256us の周期で動かし続ける (MFP の GPIP3 の割り込み)
void testOpmPeriodic(int cpu) {
    Code main, handler;
    main.vector(0x43);
    main.moveb(0x40, VR);
    main.moveb(0x08, IERB);
    main.moveb(0x08, IMRB);
    main.moveb(0x00, TDDR);
    main.moveb(0x01, TCDCR); // タイマ D: 1/4 (1us)、割り込みなし
    main.opm(0x10, 0xfc);    // タイマ A: 1024 - 1008 = 16 サンプル
    main.opm(0x11, 0x00);
    main.opm(0x14, 0x15);    // タイマ A を動かし、割り込みを許可し、フラグを消す
    main.start();
    const size_t top = main.w.size();
    main.put({0x5280, 0x4e71}); // ADDQ.L #1,D0; NOP
    main.braTo(top);
    handler.logByte(TDDR);
    handler.opm(0x14, 0x15);
    handler.put({0x4e73});
    const std::vector<uint8_t> v = run(cpu, main, handler, 40);
    show("periodic OPM timer A, MFP timer D in the handler", cpu, v);
    CHECK_EQ(v.size(), 40u);
    CHECK_EQ(samePhase(v, 2), true);
}

// 止めてある OPM のタイマ A を、メインのループが 1 サンプルの設定で動かす。処理ルーチンはタイマ D を読んでから
// タイマ A を止める
void testOpmRestart(int cpu) {
    Code main, handler;
    main.vector(0x43);
    main.moveb(0x40, VR);
    main.moveb(0x08, IERB);
    main.moveb(0x08, IMRB);
    main.moveb(0x00, TDDR);
    main.moveb(0x01, TCDCR);
    main.opm(0x10, 0xff); // タイマ A: 1024 - 1023 = 1 サンプル
    main.opm(0x11, 0x03);
    main.setFlag();
    main.start();
    const size_t top = main.waitFlag(1000);
    main.opm(0x14, 0x15);
    main.braTo(top);
    handler.logByte(TDDR);
    handler.opm(0x14, 0x10); // タイマ A を止め、フラグを消す
    handler.setFlag();
    handler.put({0x4e73});
    const std::vector<uint8_t> v = run(cpu, main, handler, 40);
    show("OPM timer A started by the main loop, MFP timer D in the handler", cpu, v);
    CHECK_EQ(v.size(), 40u);
    CHECK_EQ(samePhase(v, 2), true);
}

} // namespace

int main() {
    for (int cpu : {68030, 68060}) {
        testPeriodic(cpu);
        testRestart(cpu);
        testOpmPeriodic(cpu);
        testOpmRestart(cpu);
    }
    return finish();
}
