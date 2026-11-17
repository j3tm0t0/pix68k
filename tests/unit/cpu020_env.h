// 68030 のコア (Cpu020) の単体テストの道具: 16MB の RAM のバスと、例外の処理ルーチンを置いた環境
#pragma once

#include <initializer_list>
#include <stdint.h>

#include <vector>

#include "m68k/cpu020.h"

namespace cpu020test {

using m68k::Cpu020;

// 16MB の RAM。$C00000-$CFFFFF はバスエラーになる (berr で変えられる。BERR_WRITE なら書き込みだけ、BERR_OFF ならならない。
// berrSkip で、はじめの何回かを通せる)
struct TestBus : m68k::Bus {
    std::vector<uint8_t> mem;
    Cpu020* cpu = nullptr;
    int iackVec = -1;
    enum { BERR_ALL, BERR_WRITE, BERR_OFF } berr = BERR_ALL;
    TestBus() : mem(16 << 20) {}
    static bool badArea(uint32_t a) {
        a &= 0xffffff;
        return a >= 0xc00000 && a < 0xd00000;
    }
    int berrSkip = 0; // バスエラーにする範囲のアクセスを、はじめのこの回数だけ通す
    bool bad(uint32_t a, bool write) {
        if (berr == BERR_OFF || !(write || berr == BERR_ALL) || !badArea(a)) return false;
        if (berrSkip > 0) {
            berrSkip--;
            return false;
        }
        return true;
    }
    uint8_t read8(uint32_t a, int fc) override {
        if (bad(a, false)) cpu->busError(a, false, fc);
        return mem[a & 0xffffff];
    }
    uint16_t read16(uint32_t a, int fc) override {
        if (bad(a, false)) cpu->busError(a, false, fc);
        return (uint16_t)((mem[a & 0xffffff] << 8) | mem[(a + 1) & 0xffffff]);
    }
    void write8(uint32_t a, uint8_t v, int fc) override {
        if (bad(a, true)) cpu->busError(a, true, fc);
        mem[a & 0xffffff] = v;
    }
    void write16(uint32_t a, uint16_t v, int fc) override {
        if (bad(a, true)) cpu->busError(a, true, fc);
        mem[a & 0xffffff] = (uint8_t)(v >> 8);
        mem[(a + 1) & 0xffffff] = (uint8_t)v;
    }
    int interruptAck(int) override { return iackVec; }
    void resetDevices() override {}
    void w16(uint32_t a, uint16_t v) {
        mem[a] = (uint8_t)(v >> 8);
        mem[a + 1] = (uint8_t)v;
    }
    void w32(uint32_t a, uint32_t v) {
        w16(a, (uint16_t)(v >> 16));
        w16(a + 2, (uint16_t)v);
    }
    uint16_t r16(uint32_t a) const { return (uint16_t)((mem[a] << 8) | mem[a + 1]); }
    uint32_t r32(uint32_t a) const { return ((uint32_t)r16(a) << 16) | r16(a + 2); }
    void put(uint32_t a, std::initializer_list<uint16_t> ws) {
        for (uint16_t w : ws) {
            w16(a, w);
            a += 2;
        }
    }
};

// ベクタ n の処理ルーチンは $10000 + n * 16 にあり、$20000 + n * 4 のロングを 1 増やして RTE する。
// リセットで SSP = $9000、PC = $1000
constexpr uint32_t HANDLERS = 0x10000;
constexpr uint32_t COUNTS = 0x20000;
inline uint32_t handler(int v) { return HANDLERS + (uint32_t)v * 16; }

struct Env {
    TestBus bus;
    Cpu020 cpu;
    explicit Env(bool fast, m68k::CpuModel model = m68k::CpuModel::M68030) : cpu(&bus, model) {
        bus.cpu = &cpu;
        for (int i = 0; i < 256; i++) {
            bus.w32((uint32_t)i * 4, handler(i));
            bus.put(handler(i), {0x52b9, (uint16_t)(COUNTS >> 16), (uint16_t)(COUNTS + i * 4), 0x4e73}); // ADDQ.L #1,(abs).L; RTE
        }
        bus.w32(0, 0x9000);
        bus.w32(4, 0x1000);
        if (fast) {
            for (int p = 0; p < 16; p++) {
                cpu.setFastPage(true, p, &bus.mem[(size_t)p << 16], &bus.mem[(size_t)p << 16]);
                cpu.setFastPage(false, p, &bus.mem[(size_t)p << 16], &bus.mem[(size_t)p << 16]);
            }
        }
        cpu.reset();
    }
    uint32_t count(int v) const { return bus.r32(COUNTS + (uint32_t)v * 4); }
    void steps(int n) {
        for (int i = 0; i < n; i++) cpu.step();
    }
};

} // namespace cpu020test
