// machine_bench の場面 (cpu、user、gfx) を作る関数。machine_bench と render_thread_test が使う。
// Human68k が起動したあとの Machine に対して呼ぶ。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "x68k/machine.h"

namespace scene {

inline void poke8(x68k::Machine& m, uint32_t a, uint8_t v) { m.write8(a, v, m68k::FC_SUPER_DATA); }
inline void poke16(x68k::Machine& m, uint32_t a, uint16_t v) {
    poke8(m, a, (uint8_t)(v >> 8));
    poke8(m, a + 1, (uint8_t)v);
}

// 同じ乱数の列を使うため、場面を作るたびに初期化する
struct Rng {
    uint32_t s = 12345;
    uint32_t operator()() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
};

const uint32_t kCode = 0x300000;  // プログラムを置く場所 (起動直後の Human68k は使っていない)
const uint32_t kStack = 0x2ff000; // プログラムのスーパーバイザスタック

// 計算のループ (src/bench/bench_program.h と同じ命令の組み合わせ、平均 9 サイクル/命令程度)
const uint16_t kCpuLoop[] = {
    0x41f9, 0x0032, 0x0000, // LEA $320000,A0
    0x43f9, 0x0033, 0x0000, // LEA $330000,A1
    0x303c, 0x00ff,         // MOVE.W #255,D0
    0x2218,                 // loop: MOVE.L (A0)+,D1
    0xd481,                 // ADD.L D1,D2
    0x22c2,                 // MOVE.L D2,(A1)+
    0xe38b,                 // LSL.L #1,D3
    0xb682,                 // CMP.L D2,D3
    0x51c8, 0xfff4,         // DBRA D0,loop
    0x60e0,                 // BRA start
};

// グラフィック VRAM に書き続けるループ
const uint16_t kGfxLoop[] = {
    0x43f9, 0x00c0, 0x0000, // start: LEA $C00000,A1
    0x303c, 0x7fff,         // MOVE.W #$7FFF,D0
    0x22c1,                 // loop: MOVE.L D1,(A1)+
    0x5281,                 // ADDQ.L #1,D1
    0x51c8, 0xfffa,         // DBRA D0,loop
    0x60ec,                 // BRA start
};

// フロッピーディスクを読み続けるループ (IOCS の _B_READ でドライブ 0 のシリンダ 0 の両面、16KB を読む。
// FDC から RAM へ DMA で転送する。読み込みのたびに転送が 1 フレームに数 KB〜数十 KB になる)
const uint32_t kFdBuffer = 0x310000;
const uint32_t kFdBytes = 0x4000;
const uint16_t kFdLoop[] = {
    0x7046,                 // start: MOVEQ #$46,D0 (_B_READ)
    0x323c, 0x9070,         // MOVE.W #$9070,D1 (2HD のドライブ 0、MFM)
    0x243c, 0x0300, 0x0001, // MOVE.L #$03000001,D2 (N=3、C=0、H=0、R=1)
    0x263c, 0x0000, 0x4000, // MOVE.L #kFdBytes,D3
    0x43f9, 0x0031, 0x0000, // LEA kFdBuffer,A1
    0x4e4f,                 // TRAP #15
    0x60e4,                 // BRA start
};

// 割り込みを受け付けている (マスク 0) ときに、CPU を RAM のプログラムに飛ばす。user ならユーザモードで
inline bool jumpTo(x68k::Machine& m, const uint16_t* prog, size_t words, bool user = false) {
    m68k::Registers r;
    for (int i = 0; i < 2000; i++) {
        m.getRegisters(r);
        if ((r.sr & 0x0700) == 0) break;
        m.run(100);
    }
    if (r.sr & 0x0700) return false;
    for (size_t i = 0; i < words; i++) poke16(m, kCode + (uint32_t)i * 2, prog[i]);
    r.pc = kCode;
    r.sr = user ? 0x0000 : 0x2000;
    if (user) r.usp = kStack - 0x1000; // 割り込みの処理は今の SSP (Human68k のスタック) を使う
    else r.ssp = kStack;
    r.prefetch[0] = prog[0];
    r.prefetch[1] = prog[1];
    m.setRegisters(r);
    return true;
}

inline void opm(x68k::Machine& m, uint8_t reg, uint8_t v) {
    poke8(m, 0xe90001, reg);
    poke8(m, 0xe90003, v);
}

// opm_bench と同じ音色で 8 チャネルをキーオンする (4 チャネルは LFO の PM / AM つき)
inline void setupSound(x68k::Machine& m) {
    opm(m, 0x18, 0xc8);
    opm(m, 0x19, 0x80 | 40);
    opm(m, 0x19, 30);
    opm(m, 0x1b, 2);
    for (int c = 0; c < 8; c++) {
        opm(m, (uint8_t)(0x20 + c), (uint8_t)(0xc0 | 5 << 3 | c));
        opm(m, (uint8_t)(0x28 + c), (uint8_t)(0x30 + c * 3));
        opm(m, (uint8_t)(0x38 + c), (c & 1) ? 0x31 : 0);
        for (int s = 0; s < 4; s++) {
            int r = c + s * 8;
            opm(m, (uint8_t)(0x40 + r), (uint8_t)(1 + s));
            opm(m, (uint8_t)(0x60 + r), (uint8_t)(s == 3 ? 10 : 30));
            opm(m, (uint8_t)(0x80 + r), 0x1f);
            opm(m, (uint8_t)(0xa0 + r), 0x85);
            opm(m, (uint8_t)(0xc0 + r), 0x02);
            opm(m, (uint8_t)(0xe0 + r), 0x14);
        }
        opm(m, 0x08, (uint8_t)(0x78 | c));
    }
}

// gfx の場面の画面 (video_bench の mix512 と同じ構成で、表示タイミングの値も同じ。乱数の列は video_bench と違う):
// 512x512 (31kHz)、テキスト、16 色のグラフィック 4 ページ、スプライト 128 個、BG0。
// パレット、テキスト VRAM、スプライトのデータは乱数。テキストパレット 0 は透明。グラフィック VRAM は書かない
// (gfx の場面のループが書き続ける)。レジスタの意味は video_bench.cpp の説明と同じ
inline void setupGraphics(x68k::Machine& m) {
    Rng rnd;
    // CRTC: 水平の全体、表示の始まりと終わり (8 ドット単位)、垂直の表示の始まりと終わり、R20
    struct { int reg; uint16_t value; } const crtc[] = {{0, 91}, {2, 18}, {3, 18 + 64}, {6, 0x28}, {7, 0x228}, {20, 0x0015}};
    for (const auto& c : crtc) poke16(m, 0xe80000 + (uint32_t)c.reg * 2, c.value);
    for (uint32_t n = 0; n < 512; n++) poke16(m, 0xe82000 + n * 2, (uint16_t)rnd());
    poke16(m, 0xe82200, 0x0000); // テキストパレット 0
    for (uint32_t a = 0xe00000; a < 0xe80000; a += 2) poke16(m, a, (uint16_t)rnd());
    for (uint32_t a = 0xeb8000; a < 0xec0000; a += 2) poke16(m, a, (uint16_t)rnd());
    for (uint32_t n = 0; n < 128; n++) {
        const uint32_t base = 0xeb0000 + n * 8;
        poke16(m, base + 0, (uint16_t)(rnd() % 544));
        poke16(m, base + 2, (uint16_t)(rnd() % 544));
        poke16(m, base + 4, (uint16_t)(rnd() & 0xcfff));
        poke16(m, base + 6, (uint16_t)(1 + rnd() % 3));
    }
    poke16(m, 0xeb080c, 18 + 4); // R02 + 4
    poke16(m, 0xeb080e, 0x28);   // R06
    poke16(m, 0xeb0810, 0x0015);
    poke16(m, 0xeb0808, 0x0201); // 表示 ON、BG0 ON
    // ビデオコントローラ: R0 = 512x512 の 16 色、R1 = テキスト、スプライト、グラフィックの順 (ページは 0 番が手前)、
    // R2 = スプライト、テキスト、グラフィック 4 ページを表示
    const uint16_t vc[3] = {0x0000, (1 << 12) | (0 << 10) | (2 << 8) | 0xe4, 0x0040 | 0x0020 | 0x000f};
    for (uint32_t n = 0; n < 3; n++) poke16(m, 0xe82400 + n * 0x100, vc[n]);
}

// gfx の場面にする (画面、音、グラフィック VRAM に書き続けるループ)
inline bool startGfx(x68k::Machine& m) {
    setupGraphics(m);
    setupSound(m);
    return jumpTo(m, kGfxLoop, sizeof kGfxLoop / 2);
}

} // namespace scene
