// CPU コアの速度計測用の 68000 プログラム。ホスト版 (tests/bench) とベアメタル版で共用する。
// 加算、メモリ転送、シフト、比較、分岐を混ぜた小さなループ (平均 9 サイクル/命令程度)。
#pragma once

#include <stdint.h>

#include "m68k/m68k.h"

namespace bench {

class Memory : public m68k::Bus {
public:
    uint8_t m[1 << 24];
    uint8_t read8(uint32_t a, int) override { return m[a]; }
    uint16_t read16(uint32_t a, int) override { return (uint16_t)((m[a] << 8) | m[a + 1]); }
    void write8(uint32_t a, uint8_t v, int) override { m[a] = v; }
    void write16(uint32_t a, uint16_t v, int) override {
        m[a] = (uint8_t)(v >> 8);
        m[a + 1] = (uint8_t)v;
    }
    void w16(uint32_t a, uint16_t v) { write16(a, v, 0); }
};

// メモリにリセットベクタとプログラムを置く (m はゼロクリア済みであること)
inline void load(Memory& mem) {
    mem.w16(0, 0x0001);
    mem.w16(2, 0x0000); // SSP = 0x10000
    mem.w16(4, 0x0000);
    mem.w16(6, 0x1000); // PC = 0x1000
    static const uint16_t prog[] = {
        0x41f9, 0x0002, 0x0000, // 1000: LEA $20000,A0
        0x43f9, 0x0003, 0x0000, // 1006: LEA $30000,A1
        0x303c, 0x00ff,         // 100c: MOVE.W #255,D0
        0x2218,                 // 1010: MOVE.L (A0)+,D1
        0xd481,                 // 1012: ADD.L D1,D2
        0x22c2,                 // 1014: MOVE.L D2,(A1)+
        0xe38b,                 // 1016: LSL.L #1,D3
        0xb682,                 // 1018: CMP.L D2,D3
        0x51c8, 0xfff4,         // 101a: DBRA D0,$1010
        0x60e0,                 // 101e: BRA $1000
    };
    for (unsigned i = 0; i < sizeof prog / sizeof prog[0]; i++) mem.w16(0x1000 + i * 2, prog[i]);
}

} // namespace bench
