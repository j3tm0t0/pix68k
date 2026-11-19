// 68030 のコア (Cpu020) の単体テスト: 例外のフレーム、トレース、RTE、STOP、バスエラー、BCD、DIVx.L など。
// 根拠は MC68030 User's Manual (MC68030UM) と、instructiontest.x の実機の結果。68882 は fpu882_test.cpp
// どのテストも、直接読み書きするページ (fast page) を使う場合と使わない場合の両方で動かす
#include <stdint.h>

#include "cpu020_env.h"
#include "m68k/fpu.h"
#include "test_check.h"

using namespace m68k;
using namespace cpu020test;

namespace {

// M=1 で割り込み: マスタスタックにフォーマット $0、割り込みスタックにフォーマット $1 (スローアウェイ) を積む。
// スローアウェイの SR は割り込み前の SR に S を立てたもの (M=1 のまま)。RTE はマスタスタックへ戻る (MC68030UM 8.1.9, 8.1.13)
void testInterruptMaster(bool fast) {
    Env e(fast);
    Cpu020& c = e.cpu;
    e.bus.put(0x1000, {0x4e71, 0x4e71, 0x4e71, 0x4e71});
    c.msp_ = 0x8000;
    c.setSR(0x3000); // S=1, M=1: a7 は MSP
    e.steps(1);
    c.setIPL(4);
    e.steps(1); // 割り込み
    CHECK_EQ(c.getSR(), 0x2400);
    CHECK_EQ(c.pc_, handler(28));
    CHECK_EQ(c.a_[7], 0x9000 - 8);          // 割り込みスタック
    CHECK_EQ(e.bus.r16(c.a_[7]), 0x3000);   // スローアウェイの SR
    CHECK_EQ(e.bus.r32(c.a_[7] + 2), 0x1002);
    CHECK_EQ(e.bus.r16(c.a_[7] + 6), 0x1000 | 28 * 4);
    CHECK_EQ(c.msp_, 0x8000 - 8);           // マスタスタックのフレーム
    CHECK_EQ(e.bus.r16(0x8000 - 8), 0x3000);
    CHECK_EQ(e.bus.r16(0x8000 - 2), 28 * 4);
    c.setIPL(0);
    e.steps(2); // ADDQ と RTE
    CHECK_EQ(c.getSR(), 0x3000);
    CHECK_EQ(c.a_[7], 0x8000);
    CHECK_EQ(c.isp_, 0x9000);
    CHECK_EQ(c.pc_, 0x1002);
    CHECK_EQ(e.count(28), 1);
}

// T0 (T1=0): 流れを変えた命令のあとだけトレース (MC68030UM 8.1.7)
void testTraceChangeOfFlow(bool fast) {
    Env e(fast);
    Cpu020& c = e.cpu;
    // NOP; BRA.S +2; NOP; NOP (飛ばされる); NOP
    e.bus.put(0x1000, {0x4e71, 0x6002, 0x4e71, 0x4e71, 0x4e71});
    c.setSR(0x6000);
    e.steps(1); // NOP: トレースしない
    CHECK_EQ(e.count(9), 0);
    CHECK_EQ(c.pc_, 0x1002);
    e.steps(1); // BRA: トレース
    CHECK_EQ(c.pc_, handler(9));
    CHECK_EQ(e.bus.r16(c.a_[7] + 6), 0x2000 | 9 * 4);
    CHECK_EQ(e.bus.r32(c.a_[7] + 2), 0x1006);
    CHECK_EQ(e.bus.r32(c.a_[7] + 8), 0x1002);
    e.steps(2); // 処理ルーチン
    CHECK_EQ(e.count(9), 1);
    CHECK_EQ(c.pc_, 0x1006);
    e.steps(1); // NOP: トレースしない
    CHECK_EQ(e.count(9), 1);
    CHECK_EQ(c.pc_, 0x1008);

    // SR を書く命令もトレースする (ORI #0,SR)
    Env f(fast);
    f.bus.put(0x1000, {0x007c, 0x0000, 0x4e71});
    f.cpu.setSR(0x6000);
    f.steps(1);
    CHECK_EQ(f.cpu.pc_, handler(9));
    // T1 は命令ごと
    Env g(fast);
    g.bus.put(0x1000, {0x4e71, 0x4e71});
    g.cpu.setSR(0xa000);
    g.steps(1);
    CHECK_EQ(g.cpu.pc_, handler(9));
}

// RTE: 68030 にないフレームの形はフォーマットエラー (ベクタ 14)。そのフレームは残す (MC68030UM 8.1.8, 8.1.13)
void testRteFormatError(bool fast) {
    for (int fmt : {3, 4, 5, 6, 7, 8, 0xc, 0xd, 0xe, 0xf}) {
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x4e73});
        c.a_[7] = 0x8000;
        e.bus.w16(0x8000, 0x2700);
        e.bus.w32(0x8002, 0x1100);
        e.bus.w16(0x8006, (uint16_t)(fmt << 12));
        e.steps(1);
        CHECK_EQ(c.pc_, handler(14));
        CHECK_EQ(c.a_[7], 0x8000 - 8);
        CHECK_EQ(e.bus.r32(c.a_[7] + 2), 0x1000); // RTE の番地
        CHECK_EQ(e.bus.r16(c.a_[7] + 6), 14 * 4);
    }
}

// STOP: 特権違反にならない形と、S を 0 にする値で特権違反になる形
void testStop(bool fast) {
    Env e(fast);
    Cpu020& c = e.cpu;
    e.bus.put(0x1000, {0x4e72, 0x2000, 0x4e71});
    e.steps(1);
    CHECK_EQ(c.stopped(), true);
    CHECK_EQ(c.getSR(), 0x2000);
    CHECK_EQ(c.pc_, 0x1004);

    Env f(fast);
    f.bus.put(0x1000, {0x4e72, 0x0000, 0x4e71});
    f.steps(1);
    CHECK_EQ(f.cpu.stopped(), false);
    CHECK_EQ(f.cpu.pc_, handler(8));
    CHECK_EQ(f.bus.r32(f.cpu.a_[7] + 2), 0x1000);
    CHECK_EQ(f.bus.r16(f.cpu.a_[7]), 0x2700);

    // T1 で STOP: SR を書いたあとトレースし、停止しない (MC68030UM 8.1.7)
    Env g(fast);
    g.bus.put(0x1000, {0x4e72, 0xa000, 0x4e71});
    g.cpu.setSR(0xa700);
    g.steps(1);
    CHECK_EQ(g.cpu.stopped(), false);
    CHECK_EQ(g.cpu.pc_, handler(9));
    CHECK_EQ(g.bus.r32(g.cpu.a_[7] + 2), 0x1004);
}

// バスエラーのフォーマット $B のフレーム: SSW (MC68030UM 8.2.1、図 8-9) とフォールトアドレス (32 ビット)
struct Fault {
    uint16_t sr, format, ssw;
    uint32_t pc, addr;
};
Fault faultFrame(const Env& e) {
    uint32_t sp = e.cpu.a_[7];
    return Fault{e.bus.r16(sp), e.bus.r16(sp + 6), e.bus.r16(sp + 10), e.bus.r32(sp + 2), e.bus.r32(sp + 16)};
}

void testBusErrorSsw(bool fast) {
    { // ユーザモードのロングの読み出し (FC=1)。上位 8 ビットのあるアドレスもそのまま積む
        Env e(fast);
        e.bus.put(0x1000, {0x2018, 0x4e71}); // MOVE.L (A0)+,D0
        e.cpu.usp_ = 0x7000;
        e.cpu.setSR(0x0000);
        e.cpu.a_[0] = 0x12c00000;
        e.steps(1);
        Fault f = faultFrame(e);
        CHECK_EQ(e.cpu.pc_, handler(2));
        CHECK_EQ(f.format, 0xb000 | 2 * 4);
        CHECK_EQ(f.ssw, 0x0100 | 0x0040 | 0x00 | 1); // DF | RW | SIZE=ロング | FC=1
        CHECK_EQ(f.addr, 0x12c00000);
        CHECK_EQ(f.pc, 0x1000);
        CHECK_EQ(f.sr, 0x0000);
    }
    { // スーパーバイザのワードの書き込み (FC=5)
        Env e(fast);
        e.bus.put(0x1000, {0x3280}); // MOVE.W D0,(A1)
        e.cpu.a_[1] = 0xc00010;
        e.steps(1);
        Fault f = faultFrame(e);
        CHECK_EQ(f.ssw, 0x0100 | 0x0000 | 0x20 | 5); // DF | 書き込み | SIZE=ワード | FC=5
        CHECK_EQ(f.addr, 0xc00010);
    }
    { // バイトの読み出し
        Env e(fast);
        e.bus.put(0x1000, {0x1011}); // MOVE.B (A1),D0
        e.cpu.a_[1] = 0xc00003;
        e.steps(1);
        CHECK_EQ(faultFrame(e).ssw, 0x0100 | 0x0040 | 0x10 | 5);
    }
    { // 命令の読み込み: DF は立てず、ステージ C の FC と RC を立てる
        Env e(fast);
        e.bus.put(0x1000, {0x4ef9, 0x00c0, 0x0000}); // JMP $C00000
        e.steps(2);
        Fault f = faultFrame(e);
        CHECK_EQ(e.cpu.pc_, handler(2));
        CHECK_EQ(f.ssw & 0xf1c0, 0x8000 | 0x2000);
        CHECK_EQ(f.pc, 0xc00000);
    }
    { // 奇数番地への分岐はアドレスエラー (ベクタ 3): RB と RC だけ
        Env e(fast);
        e.bus.put(0x1000, {0x4e75}); // RTS
        e.cpu.a_[7] = 0x8000;
        e.bus.w32(0x8000, 0x1235);
        e.steps(1);
        Fault f = faultFrame(e);
        CHECK_EQ(e.cpu.pc_, handler(3));
        CHECK_EQ(f.format, 0xb000 | 3 * 4);
        CHECK_EQ(f.ssw, 0x3000);
        CHECK_EQ(f.addr, 0x1235);
    }
}

// バスエラーからの RTE (MC68030UM 8.2)。命令はやり直すので、(An)+ / -(An) などで動かしたアドレスレジスタと、
// X を使う命令の CCR は、フレームを積む前に命令の始めの値に戻す。処理ルーチンが DF を 0 にすれば (8.2.2)、
// RTE はバスエラーになったアクセスを行わずに続ける (読み出しはデータ入力バッファ +$2C の値を使う)
void testBusErrorRestart(bool fast) {
    { // MOVE.L (A0)+,D0 の読み出し: A0 は進めずに積む。DF が 1 のまま RTE すると、読み直して A0 を 1 回だけ進める
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x2018, 0x4e71});
        e.bus.w32(0xc00000, 0x11223344);
        c.a_[0] = 0xc00000;
        e.steps(1);
        CHECK_EQ(c.pc_, handler(2));
        CHECK_EQ(c.a_[0], 0xc00000u);
        CHECK_EQ(e.bus.r16(c.a_[7] + 0x14) & 1, 1); // データのバスエラーの印 (内部レジスタ)
        e.bus.berr = TestBus::BERR_OFF;
        e.steps(3); // 処理ルーチン (ADDQ、RTE) と、やり直した MOVE
        CHECK_EQ(c.pc_, 0x1002u);
        CHECK_EQ(c.d_[0], 0x11223344u);
        CHECK_EQ(c.a_[0], 0xc00004u);
        CHECK_EQ(c.a_[7], 0x9000u);
    }
    { // DF を 0 にして RTE: 読み出しはデータ入力バッファの値。バスエラーのまま続けられる
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x2018, 0x4e71});
        c.a_[0] = 0xc00000;
        e.steps(1);
        uint32_t sp = c.a_[7];
        e.bus.w16(sp + 0x0a, e.bus.r16(sp + 0x0a) & ~0x0100);
        e.bus.w32(sp + 0x2c, 0xcafef00d);
        e.steps(2); // ADDQ、RTE (RTE の中で MOVE を終える)
        CHECK_EQ(c.pc_, 0x1002u);
        CHECK_EQ(c.d_[0], 0xcafef00du);
        CHECK_EQ(c.a_[0], 0xc00004u);
        CHECK_EQ(c.a_[7], 0x9000u);
        CHECK_EQ(e.count(2), 1u);
        e.steps(1);
        CHECK_EQ(c.pc_, 0x1004u);
        // バイトはデータ入力バッファの下位 8 ビット: MOVE.B (A1),D1
        e.bus.put(0x1004, {0x1211, 0x4e71});
        c.a_[1] = 0xc00011;
        c.d_[1] = 0xffffffff;
        e.steps(1);
        sp = c.a_[7];
        CHECK_EQ(e.bus.r16(sp + 0x0a), 0x0100 | 0x0040 | 0x10 | 5);
        e.bus.w16(sp + 0x0a, 0x0040 | 0x10 | 5);
        e.bus.w32(sp + 0x2c, 0x000000a5);
        e.steps(2);
        CHECK_EQ(c.d_[1], 0xffffffa5u);
        CHECK_EQ(c.pc_, 0x1006u);
    }
    { // MOVE.W D0,-(A1) の書き込み: DOB に書く値。DF を 0 にすれば書いたものとして続ける
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x3300, 0x4e71});
        c.a_[1] = 0xc00012;
        c.d_[0] = 0x5555abcd;
        e.steps(1);
        uint32_t sp = c.a_[7];
        CHECK_EQ(c.a_[1], 0xc00012u);
        CHECK_EQ(e.bus.r32(sp + 0x10), 0xc00010u);
        CHECK_EQ(e.bus.r32(sp + 0x18), 0xabcdu);
        CHECK_EQ(e.bus.r16(sp + 0x0a), 0x0100 | 0x20 | 5);
        e.bus.w16(sp + 0x0a, 0x20 | 5);
        e.steps(2);
        CHECK_EQ(c.pc_, 0x1002u);
        CHECK_EQ(c.a_[1], 0xc00010u);
        CHECK_EQ(e.count(2), 1u);
    }
    { // ADDX.B -(A0),-(A1) の書き込みがバスエラー: A0、A1 と X、Z を戻して積む。やり直しで同じ結果になる
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0xd308, 0x4e71});
        e.bus.berr = TestBus::BERR_WRITE;
        e.bus.mem[0x3000 - 1] = 0x10;
        e.bus.mem[0xc00020 - 1] = 0x20;
        c.a_[0] = 0x3000;
        c.a_[1] = 0xc00020;
        c.setCCR(0x14); // X = 1、Z = 1
        e.steps(1);
        uint32_t sp = c.a_[7];
        CHECK_EQ(c.pc_, handler(2));
        CHECK_EQ(e.bus.r16(sp) & 0x1f, 0x14);
        CHECK_EQ(c.a_[0], 0x3000u);
        CHECK_EQ(c.a_[1], 0xc00020u);
        e.bus.berr = TestBus::BERR_OFF;
        e.steps(3);
        CHECK_EQ(e.bus.mem[0xc00020 - 1], 0x31);
        CHECK_EQ(c.a_[0], 0x2fffu);
        CHECK_EQ(c.a_[1], 0xc0001fu);
        CHECK_EQ(c.getSR() & 0x1f, 0x00);
    }
    { // MOVEM.L (A0),D0/A1/A2: A2 を読むところでバスエラー。先に読んだ A1 も戻す
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x4cd0, 0x0601, 0x4e71});
        c.a_[0] = 0xc00000 - 8;
        c.a_[1] = 0x1111;
        c.a_[2] = 0x2222;
        e.steps(1);
        CHECK_EQ(c.pc_, handler(2));
        CHECK_EQ(c.a_[1], 0x1111u);
        CHECK_EQ(c.a_[2], 0x2222u);
        CHECK_EQ(e.bus.r32(c.a_[7] + 0x10), 0xc00000u);
    }
    { // TAS (A0) の書き込みがバスエラー: RM を立てる。DF を 0 にすると、処理ルーチンが命令全体を済ませたとして次の命令へ
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x4ad0, 0x4e71});
        e.bus.berr = TestBus::BERR_WRITE;
        c.a_[0] = 0xc00040;
        e.steps(1);
        uint32_t sp = c.a_[7];
        CHECK_EQ(e.bus.r16(sp + 0x0a), 0x0100 | 0x0080 | 0x10 | 5);
        e.bus.w16(sp + 0x0a, 0x0080 | 0x10 | 5);
        e.bus.w16(sp, 0x2708); // 処理ルーチンが決めた CCR (N)
        e.steps(2);
        CHECK_EQ(c.pc_, 0x1002u);
        CHECK_EQ(c.getSR(), 0x2708);
        CHECK_EQ(e.bus.mem[0xc00040], 0);
    }
    { // ユーザモードの PEA (A0) のスタックがバスエラー: USP を戻す
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x4850, 0x4e71});
        c.usp_ = 0xc00100;
        c.setSR(0x0000);
        e.steps(1);
        CHECK_EQ(c.pc_, handler(2));
        CHECK_EQ(c.usp_, 0xc00100u);
        CHECK_EQ(e.bus.r32(c.a_[7] + 0x10), 0xc000fcu);
    }
    { // SUBX.B -(A0),-(A1) で A0 と A1 が同じ番地。2 回目の読み出し (デスティネーション) がバスエラーで、DF を 0 にして
      // RTE: やり直しでは 1 回目の読み出しは Bus から ($01)、2 回目はデータ入力バッファ ($02) で、$02 - $01 = $01 を書く
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x9308, 0x4e71});
        e.bus.mem[0xc00031] = 0x01;
        e.bus.berrSkip = 1;
        c.a_[0] = c.a_[1] = 0xc00032;
        c.setCCR(0x04); // X = 0、Z = 1
        e.steps(1);
        uint32_t sp = c.a_[7];
        CHECK_EQ(c.pc_, handler(2));
        CHECK_EQ(e.bus.r16(sp + 0x14), 0x0101); // 同じアクセスが前に 1 回あった
        e.bus.w16(sp + 0x0a, e.bus.r16(sp + 0x0a) & ~0x0100);
        e.bus.w32(sp + 0x2c, 0x02);
        e.bus.berr = TestBus::BERR_OFF;
        e.steps(2);
        CHECK_EQ(c.pc_, 0x1002u);
        CHECK_EQ(e.bus.mem[0xc00031], 0x01);
        CHECK_EQ(c.a_[0], 0xc00031u);
        CHECK_EQ(c.a_[1], 0xc00031u);
        CHECK_EQ(c.getSR() & 0x1f, 0x00);
    }
    { // バスエラーのあと、処理ルーチンがそのページを直接読み書きできるページに変えて DF を 0 にした:
      // やり直しでも、読み出しはデータ入力バッファ、書き込みは行わない
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x2010, 0x3280, 0x4e71}); // MOVE.L (A0),D0; MOVE.W D0,(A1)
        e.bus.w32(0xc00000, 0x11223344);
        c.a_[0] = 0xc00000;
        c.a_[1] = 0xc00100;
        e.steps(1);
        uint32_t sp = c.a_[7];
        c.setFastPage(true, 0xc0, &e.bus.mem[0xc00000], &e.bus.mem[0xc00000]);
        e.bus.w16(sp + 0x0a, e.bus.r16(sp + 0x0a) & ~0x0100);
        e.bus.w32(sp + 0x2c, 0xcafef00d);
        e.steps(2);
        CHECK_EQ(c.pc_, 0x1002u);
        CHECK_EQ(c.d_[0], 0xcafef00du);
        e.steps(1); // 直接読み書きできるページなので、バスエラーにならない
        CHECK_EQ(e.bus.r16(0xc00100), 0xf00d);
        // 書き込み: ページを戻してバスエラーにし、また直接読み書きできるページにして DF を 0 にする
        c.setFastPage(true, 0xc0, nullptr, nullptr);
        e.bus.put(0x1004, {0x3281, 0x4e71}); // MOVE.W D1,(A1)
        c.d_[1] = 0x1234;
        e.steps(1);
        CHECK_EQ(c.pc_, handler(2));
        sp = c.a_[7];
        c.setFastPage(true, 0xc0, &e.bus.mem[0xc00000], &e.bus.mem[0xc00000]);
        e.bus.w16(sp + 0x0a, e.bus.r16(sp + 0x0a) & ~0x0100);
        e.steps(2);
        CHECK_EQ(c.pc_, 0x1006u);
        CHECK_EQ(e.bus.r16(0xc00100), 0xf00d);
    }
    { // やり直しの記録は命令ごとに消す: MOVE.L (A0)+,D0 のあとの MOVE.L (A1),D1 がバスエラーでも、A0 は進んだまま
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x2018, 0x2211, 0x4e71});
        c.a_[0] = 0x3000;
        c.a_[1] = 0xc00000;
        e.steps(2);
        CHECK_EQ(c.pc_, handler(2));
        CHECK_EQ(c.a_[0], 0x3004u);
    }
    { // ユーザモードで、スタックがバスエラーになる BSR、LINK: USP を戻す。LINK は An も変えない
        for (int k = 0; k < 2; k++) {
            Env e(fast);
            Cpu020& c = e.cpu;
            if (k == 0) e.bus.put(0x1000, {0x6100, 0x0010}); // BSR.W
            else e.bus.put(0x1000, {0x4e56, 0xfff8});        // LINK A6,#-8
            c.usp_ = 0xc00100;
            c.a_[6] = 0x5555;
            c.setSR(0x0000);
            e.steps(1);
            CHECK_EQ(c.pc_, handler(2));
            CHECK_EQ(c.usp_, 0xc00100u);
            CHECK_EQ(c.a_[6], 0x5555u);
        }
    }
    { // MOVEM.L D0/D1,-(A0) の書き込みがバスエラー: A0 を戻す
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x48e0, 0xc000});
        c.a_[0] = 0xc00010;
        e.steps(1);
        CHECK_EQ(c.pc_, handler(2));
        CHECK_EQ(c.a_[0], 0xc00010u);
    }
    { // X や Z を使う読み書きの命令の書き込みがバスエラー: 積む SR は命令の始めの X と Z (saveCcrUndo())
        struct Case {
            uint16_t op;
            uint8_t src, dst;
        };
        const Case cases[] = {
            {0x4010, 0, 0x01},    // NEGX.B (A0)
            {0xe5d0, 0, 0x80},    // ROXL.W (A0) (語の上位バイト)
            {0xc308, 0x01, 0x01}, // ABCD -(A0),-(A1)
            {0x8308, 0x01, 0x01}, // SBCD -(A0),-(A1)
            {0x4810, 0, 0x00},    // NBCD (A0)
        };
        for (const Case& t : cases) {
            Env e(fast);
            Cpu020& c = e.cpu;
            e.bus.put(0x1000, {t.op, 0x4e71});
            e.bus.berr = TestBus::BERR_WRITE;
            const bool pre = (t.op & 0xf000) == 0xc000 || (t.op & 0xf000) == 0x8000;
            if (pre) {
                c.a_[0] = 0x3001;
                c.a_[1] = 0xc00041;
                e.bus.mem[0x3000] = t.src;
                e.bus.mem[0xc00040] = t.dst;
            } else {
                c.a_[0] = 0xc00040;
                e.bus.mem[0xc00040] = t.dst;
            }
            c.setCCR(0x14); // X = 1、Z = 1
            e.steps(1);
            CHECK_EQ(c.pc_, handler(2));
            CHECK_EQ(e.bus.r16(c.a_[7]) & 0x1f, 0x14);
            CHECK_EQ(c.a_[0], pre ? 0x3001u : 0xc00040u);
        }
    }
    { // MOVEM.L (A0),D0-D2 の 3 つ目の読み出しがバスエラー。DF を 0 にして RTE: D0、D1 は読み直し、D2 は DIB
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x4cd0, 0x0007, 0x4e71});
        e.bus.w32(0xbffff8, 0x11111111);
        e.bus.w32(0xbffffc, 0x22222222);
        c.a_[0] = 0xbffff8;
        e.steps(1);
        uint32_t sp = c.a_[7];
        CHECK_EQ(c.pc_, handler(2));
        CHECK_EQ(e.bus.r32(sp + 0x10), 0xc00000u);
        e.bus.w16(sp + 0x0a, e.bus.r16(sp + 0x0a) & ~0x0100);
        e.bus.w32(sp + 0x2c, 0x33333333);
        e.steps(2);
        CHECK_EQ(c.pc_, 0x1004u);
        CHECK_EQ(c.d_[0], 0x11111111u);
        CHECK_EQ(c.d_[1], 0x22222222u);
        CHECK_EQ(c.d_[2], 0x33333333u);
        CHECK_EQ(c.a_[0], 0xbffff8u);
    }
    { // CAS2.L D0:D1,D2:D3,(A0):(A1) の (A1) の読み出しがバスエラー: SSW の RM が立つ
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x0efc, 0x8080, 0x90c1, 0x4e71});
        c.a_[0] = 0x3000;
        c.a_[1] = 0xc00000;
        e.steps(1);
        CHECK_EQ(c.pc_, handler(2));
        CHECK_EQ(e.bus.r16(c.a_[7] + 0x0a) & 0x01c0, 0x0100 | 0x0080 | 0x0040);
    }
    { // マスタモード (M = 1) でのバスエラー: フレームは MSP に積む
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x2010, 0x4e71});
        c.setSR(0x3700);
        c.a_[7] = 0x7000;
        c.a_[0] = 0xc00000;
        e.steps(1);
        CHECK_EQ(c.pc_, handler(2));
        CHECK_EQ(c.a_[7], 0x7000u - 92);
        CHECK_EQ(e.bus.r16(c.a_[7] + 6), 0xb008);
    }
    { // フレームを積む所 (スーパーバイザスタック) もバスエラー: ダブルバスフォルトで止まる
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x2010, 0x4e71});
        c.a_[7] = 0xc00100;
        c.a_[0] = 0xc00000;
        e.steps(1);
        CHECK_EQ(c.halted_, true);
    }
    { // LINK A7,#-4: 減らしたあとの A7 を積む (MC68030UM の LINK)
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x4e57, 0xfffc});
        c.a_[7] = 0x8000;
        e.steps(1);
        CHECK_EQ(e.bus.r32(0x7ffc), 0x7ffcu);
        CHECK_EQ(c.a_[7], 0x7ff8u);
    }
    { // 奇数番地への JSR のアドレスエラー: 戻り番地を積んだ A7 を戻してから積む
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x4eb9, 0x0000, 0x1235});
        c.a_[7] = 0x8000;
        e.steps(1);
        CHECK_EQ(c.pc_, handler(3));
        CHECK_EQ(c.a_[7], 0x8000u - 92);
        CHECK_EQ(e.bus.r16(c.a_[7] + 0x14) & 1, 0); // データのバスエラーではない
    }
    // 奇数番地の MOVE.L (A0),D0 は、バイト・ワード・バイトに分けて Bus に出す。バスエラーになった部分のアドレスと
    // 大きさが最初のフレームに入る (SSW の SIZE: 1 = バイト、2 = ワード)。berrSkip で前の部分を通して、2 つ目と
    // 3 つ目でバスエラーにする。RTE で戻る動きは確かめない (DF を 0 にして戻ると、命令の始めからやり直すので
    // 前の部分がまたバスエラーになる。pushBusFault() の注釈)
    for (int skip = 0; skip < 3; skip++) {
        static const uint32_t addr[] = {0xc00001, 0xc00002, 0xc00004};
        static const int size[] = {1, 2, 1};
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x2010, 0x4e71});
        c.a_[0] = 0xc00001;
        e.bus.berrSkip = skip;
        e.steps(1);
        const uint32_t sp = c.a_[7];
        CHECK_EQ(c.pc_, handler(2));
        CHECK_EQ(e.bus.r16(sp + 6) >> 12, 0xb);        // 長いフレーム
        CHECK_EQ(e.bus.r32(sp + 0x10), addr[skip]);    // バスエラーになった部分のアドレス
        CHECK_EQ((e.bus.r16(sp + 0x0a) >> 4) & 3, size[skip]);
        CHECK_EQ(e.bus.r16(sp + 0x0a) & 0x40, 0x40);   // 読み出し
        CHECK_EQ(e.bus.r32(sp + 2), 0x1000u);          // 積む PC は命令の先頭
        CHECK_EQ(c.a_[0], 0xc00001u);
    }
    // 処理ルーチンが DF を 0 にし、データ入力バッファに dib を入れて RTE する (ADDQ.L と RTE の 2 命令)
    auto resumeDib = [](Env& e, uint32_t dib) {
        const uint32_t sp = e.cpu.a_[7];
        e.bus.w16(sp + 0x0a, e.bus.r16(sp + 0x0a) & ~0x0100);
        e.bus.w32(sp + 0x2c, dib);
        e.steps(2);
    };
    for (int pre = 0; pre < 2; pre++) {
        // MOVES.L ([A0]),D0 (SFC = 1)。ポインタはスーパーバイザの直接読める番地から読み、ポインタと同じ番地の
        // オペランドをユーザーの空間で読んでバスエラー。DF を 0 にして RTE: ポインタは読み直し、D0 はデータ入力
        // バッファ (FC が違うので、ポインタの読み出しと取り違えない)。pre なら、前の命令でそのページを TLB に入れておく
        Env e(fast);
        Cpu020& c = e.cpu;
        c.setFastPage(true, 0xc0, &e.bus.mem[0xc00000], &e.bus.mem[0xc00000]);
        e.bus.put(0x1000, {(uint16_t)(pre ? 0x2210 : 0x4e71), 0x0eb0, 0x0000, 0x0151, 0x4e71});
        e.bus.w32(0xc00000, 0xc00000);
        c.a_[0] = 0xc00000;
        c.sfc_ = 1;
        e.steps(2);
        uint32_t sp = c.a_[7];
        CHECK_EQ(c.pc_, handler(2));
        CHECK_EQ(e.bus.r32(sp + 0x10), 0xc00000u);
        CHECK_EQ(e.bus.r16(sp + 0x0a) & 7, 1);
        CHECK_EQ(e.bus.r16(sp + 0x14), 0x0001);
        e.bus.berr = TestBus::BERR_OFF;
        resumeDib(e, 0x3000);
        CHECK_EQ(c.pc_, 0x1008u);
        CHECK_EQ(c.d_[0], 0x3000u);
    }
    { // MOVEM.L ([A0]),D0-D7/A0-A7 (ロングの読み出し 17 回)。最後の読み出しは間接参照のポインタと同じ番地で、
      // バスエラー。DF を 0 にして RTE: ポインタは読み直し、A7 はデータ入力バッファ
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x4cf0, 0xffff, 0x0151, 0x4e71});
        for (uint32_t k = 0; k < 15; k++) e.bus.w32(0xc00000 + 4 * k, 0x100 + k);
        e.bus.w32(0xc0003c, 0xc00000);
        c.a_[0] = 0xc0003c;
        e.bus.berrSkip = 2 + 15 * 2; // ロングは Bus の read16 2 回
        e.steps(1);
        uint32_t sp = c.a_[7];
        CHECK_EQ(c.pc_, handler(2));
        CHECK_EQ(e.bus.r32(sp + 0x10), 0xc0003cu);
        CHECK_EQ(e.bus.r16(sp + 0x14), 0x0101); // 同じアクセス (ポインタの読み出し) が前に 1 回あった
        e.bus.berr = TestBus::BERR_OFF;
        resumeDib(e, 0x3000);
        CHECK_EQ(c.pc_, 0x1006u);
        CHECK_EQ(c.d_[0], 0x100u);
        CHECK_EQ(c.a_[0], 0x108u);
        CHECK_EQ(c.a_[6], 0x10eu);
        CHECK_EQ(c.a_[7], 0x3000u);
    }
    { // FMOVEM.X ([A0]),FP0-FP7 (ロングの読み出し 25 回) でも同じ
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0xf230, 0xd0ff, 0x0151, 0x4e71});
        for (uint32_t k = 0; k < 23; k++) e.bus.w32(0xc00000 + 4 * k, 0x3fff0000 + k);
        e.bus.w32(0xc0005c, 0xc00000);
        c.a_[0] = 0xc0005c;
        e.bus.berrSkip = 2 + 23 * 2;
        e.steps(1);
        uint32_t sp = c.a_[7];
        CHECK_EQ(c.pc_, handler(2));
        CHECK_EQ(e.bus.r32(sp + 0x10), 0xc0005cu);
        CHECK_EQ(e.bus.r16(sp + 0x14), 0x0101);
        e.bus.berr = TestBus::BERR_OFF;
        resumeDib(e, 0x3000);
        CHECK_EQ(c.pc_, 0x1006u);
        CHECK_EQ((uint32_t)c.fpu()->fp_[7].m, 0x3000u);
        CHECK_EQ(c.a_[0], 0xc0005cu);
    }
    { // MOVE.L (A0),(A1) で読み出しも書き込みもバスエラー。読み出しを DIB で済ませたやり直しの中で書き込みが
      // バスエラーになる。そこで DF を 0 にして RTE しても、命令をはじめからやり直すので読み出しがまたバスエラーに
      // なる (命令が終わらない。cpu020.cpp の pushBusFault() の「実機と違うもの」)
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x2290, 0x4e71});
        c.a_[0] = 0xc00000;
        c.a_[1] = 0xc00100;
        e.steps(1);
        CHECK_EQ(c.pc_, handler(2));
        resumeDib(e, 0xaabbccdd);
        uint32_t sp = c.a_[7];
        CHECK_EQ(c.pc_, handler(2));
        CHECK_EQ(e.bus.r32(sp + 0x10), 0xc00100u);
        CHECK_EQ(e.bus.r16(sp + 0x0a) & 0x40, 0);    // 書き込み
        CHECK_EQ(e.bus.r32(sp + 0x18), 0xaabbccddu); // DOB は DIB で済ませた読み出しの値
        CHECK_EQ(e.bus.r16(sp + 0x14), 0x0001);
        resumeDib(e, 0);
        sp = c.a_[7];
        CHECK_EQ(c.pc_, handler(2));
        CHECK_EQ(e.bus.r32(sp + 0x10), 0xc00000u);
        CHECK_EQ(e.bus.r16(sp + 0x0a) & 0x40, 0x40); // また読み出し
        CHECK_EQ(e.count(2), 2u);
    }
    { // SUBX.B -(A0),-(A1) で A0 = A1、どの読み出しもバスエラー。2 回目の読み出しは、1 回目の読み出し (やり直しで
      // DIB で済ませたもの) のあとの 2 回目として数える
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x9308, 0x4e71});
        c.a_[0] = c.a_[1] = 0xc00032;
        c.setCCR(0x04);
        e.steps(1);
        CHECK_EQ(e.bus.r16(c.a_[7] + 0x14), 0x0001);
        resumeDib(e, 0x01);
        CHECK_EQ(c.pc_, handler(2));
        CHECK_EQ(e.bus.r32(c.a_[7] + 0x10), 0xc00031u);
        CHECK_EQ(e.bus.r16(c.a_[7] + 0x14), 0x0101); // 同じアクセスが前に 1 回
        CHECK_EQ(c.a_[0], 0xc00032u);
    }
    { // 前の命令で Bus に出したアクセスは数えない (命令の始めに記録を消す)
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x2210, 0x2010, 0x4e71}); // MOVE.L (A0),D1; MOVE.L (A0),D0
        c.a_[0] = 0xc00000;
        e.bus.berrSkip = 2;
        e.steps(2);
        CHECK_EQ(c.pc_, handler(2));
        CHECK_EQ(e.bus.r16(c.a_[7] + 0x14), 0x0001);
    }
    { // 読み出しのバスエラーでは、データ出力バッファは 0 (前の命令で Bus に書いた値を積まない)
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x2281, 0x2010, 0x4e71}); // MOVE.L D1,(A1); MOVE.L (A0),D0
        c.a_[0] = 0xc00000;
        c.a_[1] = 0xc00100;
        c.d_[1] = 0xdeadbeef;
        e.bus.berrSkip = 2;
        e.steps(2);
        CHECK_EQ(c.pc_, handler(2));
        CHECK_EQ(e.bus.r32(0xc00100), 0xdeadbeefu);
        CHECK_EQ(e.bus.r32(c.a_[7] + 0x18), 0u);
    }
    { // 処理ルーチンがページを直接読み書きできるように変え、そのページを読んで TLB に入れてから DF を 0 にして
      // RTE: やり直しは TLB を空にしてから始めるので、読み出しはデータ入力バッファ
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x2010, 0x4e71});
        e.bus.w32(0xc00000, 0x11223344);
        c.a_[0] = 0xc00000;
        e.steps(1);
        c.setFastPage(true, 0xc0, &e.bus.mem[0xc00000], &e.bus.mem[0xc00000]);
        CHECK_EQ(c.xread32(0xc00000), 0x11223344u);
        resumeDib(e, 0xcafef00d);
        CHECK_EQ(c.pc_, 0x1002u);
        CHECK_EQ(c.d_[0], 0xcafef00du);
    }
    { // MOVEM.L (A0),D0-D1 の 2 つ目がバスエラー。処理ルーチンがページを直接読み書きできるように変えて DF を 0 に
      // する: やり直しの 1 つ目の読み出しでそのページを TLB に入れないので、2 つ目はデータ入力バッファ
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x4cd0, 0x0003, 0x4e71});
        e.bus.w32(0xc00000, 0x11111111);
        e.bus.w32(0xc00004, 0x22222222);
        c.a_[0] = 0xc00000;
        e.bus.berrSkip = 2;
        e.steps(1);
        CHECK_EQ(e.bus.r32(c.a_[7] + 0x10), 0xc00004u);
        c.setFastPage(true, 0xc0, &e.bus.mem[0xc00000], &e.bus.mem[0xc00000]);
        resumeDib(e, 0x55555555);
        CHECK_EQ(c.pc_, 0x1004u);
        CHECK_EQ(c.d_[0], 0x11111111u);
        CHECK_EQ(c.d_[1], 0x55555555u);
    }
    { // やり直しでデータ入力バッファを使ったロングの読み出しも、Bus に出したときと同じサイクル数。フレームの
      // 何回目か (+$14) を 1 にしてデータ入力バッファを使わないものと比べる
        uint64_t cyc[2];
        uint32_t d0[2];
        for (int miss = 0; miss < 2; miss++) {
            Env e(fast);
            Cpu020& c = e.cpu;
            e.bus.put(0x1000, {0x2010, 0x4e71});
            e.bus.w32(0xc00000, 0x11223344);
            c.a_[0] = 0xc00000;
            e.steps(1);
            if (miss) e.bus.w16(c.a_[7] + 0x14, 0x0101);
            e.bus.berr = TestBus::BERR_OFF;
            const uint64_t t = c.cycles();
            resumeDib(e, 0xcafef00d);
            cyc[miss] = c.cycles() - t;
            d0[miss] = c.d_[0];
        }
        CHECK_EQ(d0[0], 0xcafef00du);
        CHECK_EQ(d0[1], 0x11223344u);
        CHECK_EQ(cyc[0], cyc[1]);
    }
    { // やり直しの命令が、バスエラーになったアクセスの前に例外になった: 例外のスタックを積む書き込みは、同じ
      // アクセスでもデータ入力バッファで済ませず Bus に出す。MOVES.W D0,(A0) (DFC = 5) の書き込みがバスエラーで、
      // 処理ルーチンが SR をユーザーモード (M = 1) にして DF を 0 にする。やり直しの MOVES は特権違反になり、
      // マスタスタック ($C00002) に積む最初の書き込み ($C00000 のワード) がバスエラーで、ダブルフォルトで止まる
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0x0e50, 0x0800, 0x4e71});
        c.a_[0] = 0xc00000;
        c.dfc_ = 5;
        c.msp_ = 0xc00002;
        e.bus.berr = TestBus::BERR_WRITE;
        e.steps(1);
        CHECK_EQ(c.pc_, handler(2));
        CHECK_EQ(e.bus.r32(c.a_[7] + 0x10), 0xc00000u);
        e.bus.w16(c.a_[7], 0x1000);
        resumeDib(e, 0);
        CHECK_EQ(c.halted_, true);
    }
}

// ABCD / SBCD / NBCD: X を加味した 10 進の加減算。Z は結果が 0 でなければ落とし、0 なら変えない (MC68030UM 表 3-12)。
// 68030 では V は常に 0 になる (instructiontest.x の実機の結果から)
void testBcd(bool fast) {
    { // ABCD D1,D0: 45 + 67 + X(1) = 113 → 13、桁上がり
        Env e(fast);
        e.bus.put(0x1000, {0xc101});
        e.cpu.d_[0] = 0x12345645;
        e.cpu.d_[1] = 0x67;
        e.cpu.setSR(0x2700 | 0x14); // X=1, Z=1
        e.steps(1);
        CHECK_EQ(e.cpu.d_[0], 0x12345613);
        CHECK_EQ(e.cpu.getSR() & 0x1f, 0x11); // X=1, C=1, Z を落とす
    }
    { // SBCD D1,D0: 23 - 45 - X(0) = -22 → 78、借り
        Env e(fast);
        e.bus.put(0x1000, {0x8101});
        e.cpu.d_[0] = 0x23;
        e.cpu.d_[1] = 0x45;
        e.cpu.setSR(0x2700 | 0x04);
        e.steps(1);
        CHECK_EQ(e.cpu.d_[0], 0x78);
        CHECK_EQ(e.cpu.getSR() & 0x1f, 0x11);
    }
    { // NBCD D0: 0 - 01 = 99、借り。続けて 0 - 00 - X(1) = 99
        Env e(fast);
        e.bus.put(0x1000, {0x4800, 0x4801});
        e.cpu.d_[0] = 0x01;
        e.cpu.d_[1] = 0x00;
        e.cpu.setSR(0x2700 | 0x04);
        e.steps(1);
        CHECK_EQ(e.cpu.d_[0], 0x99);
        CHECK_EQ(e.cpu.getSR() & 0x1f, 0x19); // X=1, N=1, C=1
        e.steps(1);
        CHECK_EQ(e.cpu.d_[1], 0x99);
        CHECK_EQ(e.cpu.getSR() & 0x1f, 0x19);
    }
    { // NBCD D0: 0 - 00 - X(0) = 00。借りなし、Z は変えない
        Env e(fast);
        e.bus.put(0x1000, {0x4800});
        e.cpu.d_[0] = 0x00;
        e.cpu.setSR(0x2700 | 0x04);
        e.steps(1);
        CHECK_EQ(e.cpu.d_[0], 0x00);
        CHECK_EQ(e.cpu.getSR() & 0x1f, 0x04);
    }
}

// DIVU.L / DIVS.L (MC68030UM 表 3-12): あふれたらレジスタは変えずに V=1、ゼロ除算はベクタ 5 のフォーマット $2
void testDivL(bool fast) {
    { // DIVU.L D1,D3:D2: $00000002_00000000 / 1 は 32 ビットに入らない
        Env e(fast);
        e.bus.put(0x1000, {0x4c41, 0x2403});
        e.cpu.d_[1] = 1;
        e.cpu.d_[2] = 0;
        e.cpu.d_[3] = 2;
        e.cpu.setSR(0x2700);
        e.steps(1);
        CHECK_EQ(e.cpu.d_[2], 0);
        CHECK_EQ(e.cpu.d_[3], 2);
        CHECK_EQ(e.cpu.getSR() & 0x03, 0x02); // V=1, C=0
        CHECK_EQ(e.cpu.pc_, 0x1004);
    }
    { // DIVS.L D1,D2 で D1=0: ベクタ 5。積む PC は次の命令、フレームの最後は命令のアドレス
        Env e(fast);
        e.bus.put(0x1000, {0x4c41, 0x2802});
        e.cpu.d_[1] = 0;
        e.cpu.d_[2] = 1234;
        e.cpu.setSR(0x2700 | 0x01);
        e.steps(1);
        uint32_t sp = e.cpu.a_[7];
        CHECK_EQ(e.cpu.pc_, handler(5));
        CHECK_EQ(sp, 0x9000 - 12);
        CHECK_EQ(e.bus.r16(sp) & 0xff01, 0x2700); // C=0
        CHECK_EQ(e.bus.r32(sp + 2), 0x1004);
        CHECK_EQ(e.bus.r16(sp + 6), 0x2000 | 5 * 4);
        CHECK_EQ(e.bus.r32(sp + 8), 0x1000);
        CHECK_EQ(e.cpu.d_[2], 1234);
    }
    { // DIVSL.L D1,D3:D2: -7 / 2 = -3 余り -1 (余りは被除数の符号)
        Env e(fast);
        e.bus.put(0x1000, {0x4c41, 0x2803});
        e.cpu.d_[1] = 2;
        e.cpu.d_[2] = (uint32_t)-7;
        e.cpu.d_[3] = 0x55555555;
        e.cpu.setSR(0x2700 | 0x07);
        e.steps(1);
        CHECK_EQ(e.cpu.d_[2], (uint32_t)-3);
        CHECK_EQ(e.cpu.d_[3], 0xffffffffu);
        CHECK_EQ(e.cpu.getSR() & 0x0f, 0x08); // N=1, Z=V=C=0
    }
}

// CHK.W / CHK.L (MC68030UM 表 3-12): 0 <= Dn <= 上限なら例外なし、外れたらベクタ 6 のフォーマット $2
void testChk(bool fast) {
    { // CHK.W D1,D2: D2=5、上限 10 で例外なし
        Env e(fast);
        e.bus.put(0x1000, {0x4581});
        e.cpu.d_[1] = 10;
        e.cpu.d_[2] = 5;
        e.cpu.setSR(0x2700);
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, 0x1002);
        CHECK_EQ(e.count(6), 0);
        CHECK_EQ(e.cpu.getSR() & 0x08, 0x00); // N=0
    }
    { // CHK.L D1,D2: D2=11 > 10 でベクタ 6。積む PC は次の命令、フレームの最後は命令のアドレス
        Env e(fast);
        e.bus.put(0x1000, {0x4501});
        e.cpu.d_[1] = 10;
        e.cpu.d_[2] = 11;
        e.cpu.setSR(0x2700);
        e.steps(1);
        uint32_t sp = e.cpu.a_[7];
        CHECK_EQ(e.cpu.pc_, handler(6));
        CHECK_EQ(sp, 0x9000 - 12);
        CHECK_EQ(e.bus.r16(sp) & 0x08, 0x00); // N=0
        CHECK_EQ(e.bus.r32(sp + 2), 0x1002);
        CHECK_EQ(e.bus.r16(sp + 6), 0x2000 | 6 * 4);
        CHECK_EQ(e.bus.r32(sp + 8), 0x1000);
    }
    { // CHK.W D1,D2: D2.W=-1 (上位は無視) でベクタ 6、N=1。未定義の Z/V/C は Z=(Dn==0)、V/C は上限 - Dn のもの
        Env e(fast);
        e.bus.put(0x1000, {0x4581});
        e.cpu.d_[1] = 10;
        e.cpu.d_[2] = 0x0000ffff;
        e.cpu.setSR(0x2700);
        e.steps(1);
        uint32_t sp = e.cpu.a_[7];
        CHECK_EQ(e.cpu.pc_, handler(6));
        CHECK_EQ(e.bus.r16(sp) & 0x0f, 0x09); // N=1 Z=0 V=0 C=1 ($000a - $ffff は借りが出る)
    }
    { // CHK.W D1,D2: D2=0 は範囲内で Z=1
        Env e(fast);
        e.bus.put(0x1000, {0x4581});
        e.cpu.d_[1] = 10;
        e.cpu.d_[2] = 0;
        e.cpu.setSR(0x2700 | 0x0b);
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, 0x1002);
        CHECK_EQ(e.cpu.getSR() & 0x0f, 0x04);
    }
}

// CMP2 / CHK2 (MC68030UM 表 3-12): Z は境界のどちらかに等しいとき、C は範囲外のとき。CHK2 は範囲外でベクタ 6
void testChk2Cmp2(bool fast) {
    { // CMP2.B (A0),D1: 境界 $10..$20、D1.B=$20 で Z=1 C=0
        Env e(fast);
        e.bus.put(0x1000, {0x00d0, 0x1000});
        e.bus.put(0x3000, {0x1020});
        e.cpu.a_[0] = 0x3000;
        e.cpu.d_[1] = 0xffffff20;
        e.cpu.setSR(0x2700);
        e.steps(1);
        CHECK_EQ(e.cpu.getSR() & 0x05, 0x04);
        CHECK_EQ(e.cpu.pc_, 0x1004);
    }
    { // CMP2.B (A0),D1: D1.B=$21 で Z=0 C=1、例外なし
        Env e(fast);
        e.bus.put(0x1000, {0x00d0, 0x1000});
        e.bus.put(0x3000, {0x1020});
        e.cpu.a_[0] = 0x3000;
        e.cpu.d_[1] = 0x21;
        e.cpu.setSR(0x2700);
        e.steps(1);
        CHECK_EQ(e.cpu.getSR() & 0x05, 0x01);
        CHECK_EQ(e.cpu.pc_, 0x1004);
        CHECK_EQ(e.count(6), 0);
    }
    { // CHK2.W (A0),A1: 境界 -2..5 は符号拡張して 32 ビットで比べる。A1=-3 は範囲外でベクタ 6
        Env e(fast);
        e.bus.put(0x1000, {0x02d0, 0x9800});
        e.bus.put(0x3000, {0xfffe, 0x0005});
        e.cpu.a_[0] = 0x3000;
        e.cpu.a_[1] = 0xfffffffd;
        e.cpu.setSR(0x2700);
        e.steps(1);
        uint32_t sp = e.cpu.a_[7];
        CHECK_EQ(e.cpu.pc_, handler(6));
        CHECK_EQ(e.bus.r16(sp) & 0x05, 0x01); // C=1
        CHECK_EQ(e.bus.r32(sp + 2), 0x1004);
        CHECK_EQ(e.bus.r16(sp + 6), 0x2000 | 6 * 4);
        CHECK_EQ(e.bus.r32(sp + 8), 0x1000);
    }
    { // CHK2.W (A0),A1: A1=-2 は下限に等しく範囲内
        Env e(fast);
        e.bus.put(0x1000, {0x02d0, 0x9800});
        e.bus.put(0x3000, {0xfffe, 0x0005});
        e.cpu.a_[0] = 0x3000;
        e.cpu.a_[1] = 0xfffffffe;
        e.cpu.setSR(0x2700);
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, 0x1004);
        CHECK_EQ(e.cpu.getSR() & 0x05, 0x04);
    }
    // 未定義の N と V (cpu020_ops.inc の chk2Flags()): U - D、L - D、D - L のどれか 1 つの引き算の N と V
    struct NV {
        uint16_t op, ext, lo, hi; // .l のときは lo / hi が上位の語、lo2 / hi2 が下位の語
        uint16_t lo2, hi2;
        uint32_t reg;
        uint16_t ccr;
    };
    const NV nv[] = {
        // CMP2.B (A0),D1: $10..$20 で D1.B = $05。L < D でも D < U < L でもないので D - L = -11: N=1 V=0 C=1
        {0x00d0, 0x1000, 0x1020, 0, 0, 0, 0x05, 0x09},
        // D1.B = $80 は符号拡張すると $FFFFFF80 で、符号なしで L < D、L <= U なので U - D = $A0: N=0 V=0 (C=1)
        {0x00d0, 0x1000, 0x1020, 0, 0, 0, 0x80, 0x01},
        // CMP2.L (A0),D1: -2..0 で D1 = $7FFFFFFE。D - L = $80000000 があふれる: N=1 V=1 C=1
        {0x04d0, 0x1000, 0xffff, 0x0000, 0xfffe, 0x0000, 0x7ffffffe, 0x0b},
        // CMP2.W (A0),A1: -$8000..$7FFF (符号拡張する) で A1 = $7FFFFFFF。D - L = $80007FFF: N=1 V=1 C=1
        {0x02d0, 0x9000, 0x8000, 0x7fff, 0, 0, 0x7fffffff, 0x0b},
        // CMP2.L (A0),D1: 0..$7FFFFFFF で D1 = $80000000。L < D、L <= U なので U - D = $FFFFFFFF: N=1 V=1 C=1
        {0x04d0, 0x1000, 0x0000, 0x7fff, 0x0000, 0xffff, 0x80000000, 0x0b},
        // CMP2.B (A0),D1: 2..1 (回り込む) で D1.B = 0。D < U < L なので L - D = 2: N=0 V=0 (範囲内で C=0)
        {0x00d0, 0x1000, 0x0201, 0, 0, 0, 0x00, 0x00},
        // 1..0 で D1.B = 0。D = U なので U - D = 0: N=0 V=0、Z=1
        {0x00d0, 0x1000, 0x0100, 0, 0, 0, 0x00, 0x04},
        // 0..0 で D1.B = 1。L < D、L <= U (等しい) なので U - D = -1: N=1 V=0 C=1
        {0x00d0, 0x1000, 0x0000, 0, 0, 0, 0x01, 0x09},
        // CMP2.W (A0),D1: $10..$20 で D1.W = $8000 は符号拡張して $FFFF8000。L < D、L <= U なので
        // U - D = $8020: N=0 V=0 (C=1)
        {0x02d0, 0x1000, 0x0010, 0x0020, 0, 0, 0x8000, 0x01},
    };
    for (const NV& t : nv) {
        Env e(fast);
        e.bus.put(0x1000, {t.op, t.ext});
        if (t.op == 0x04d0) e.bus.put(0x3000, {t.lo, t.lo2, t.hi, t.hi2});
        else if (t.op == 0x02d0) e.bus.put(0x3000, {t.lo, t.hi});
        else e.bus.put(0x3000, {t.lo});
        e.cpu.a_[0] = 0x3000;
        if (t.ext & 0x8000) e.cpu.a_[1] = t.reg;
        else e.cpu.d_[1] = t.reg;
        e.cpu.setSR(0x2700);
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, 0x1004);
        CHECK_EQ(e.cpu.getSR() & 0x1f, t.ccr);
    }
}

// DIVU.W / DIVS.W (MC68030UM 表 3-12): 結果は上位 16 ビットが余り、下位 16 ビットが商。
// あふれたらレジスタは変えずに V=1、ゼロ除算はベクタ 5 のフォーマット $2
void testDivW(bool fast) {
    { // DIVU.W D1,D2: 100003 / 10 = 10000 余り 3
        Env e(fast);
        e.bus.put(0x1000, {0x84c1});
        e.cpu.d_[1] = 10;
        e.cpu.d_[2] = 100003;
        e.cpu.setSR(0x2700 | 0x0f);
        e.steps(1);
        CHECK_EQ(e.cpu.d_[2], (3u << 16) | 10000);
        CHECK_EQ(e.cpu.getSR() & 0x0f, 0x00);
    }
    { // DIVS.W D1,D2: -7 / 2 = -3 余り -1
        Env e(fast);
        e.bus.put(0x1000, {0x85c1});
        e.cpu.d_[1] = 2;
        e.cpu.d_[2] = (uint32_t)-7;
        e.cpu.setSR(0x2700);
        e.steps(1);
        CHECK_EQ(e.cpu.d_[2], 0xfffffffdu);
        CHECK_EQ(e.cpu.getSR() & 0x0f, 0x08); // N=1
    }
    { // DIVS.W D1,D2: $10000 / 1 は 16 ビットに入らない
        Env e(fast);
        e.bus.put(0x1000, {0x85c1});
        e.cpu.d_[1] = 1;
        e.cpu.d_[2] = 0x10000;
        e.cpu.setSR(0x2700 | 0x0d);
        e.steps(1);
        CHECK_EQ(e.cpu.d_[2], 0x10000);
        CHECK_EQ(e.cpu.getSR() & 0x0f, 0x02); // V=1 C=0。N と Z は被除数の絶対値 $00010000 から
        CHECK_EQ(e.cpu.pc_, 0x1002);
    }
    { // DIVS.W D1,D2: $8000 / 1 も入らない。上位 16 ビット < 除数のときは N が商のビット 7、Z が商の下位 8 ビットから
        Env e(fast);
        e.bus.put(0x1000, {0x85c1});
        e.cpu.d_[1] = 1;
        e.cpu.d_[2] = 0x8000;
        e.cpu.setSR(0x2700 | 0x09);
        e.steps(1);
        CHECK_EQ(e.cpu.d_[2], 0x8000);
        CHECK_EQ(e.cpu.getSR() & 0x0f, 0x06); // N=0 Z=1 V=1
    }
    { // DIVU.W D1,D2: $80000000 / 1 は入らない。N と Z は被除数 (32 ビット) から
        Env e(fast);
        e.bus.put(0x1000, {0x84c1});
        e.cpu.d_[1] = 1;
        e.cpu.d_[2] = 0x80000000;
        e.cpu.setSR(0x2700 | 0x05);
        e.steps(1);
        CHECK_EQ(e.cpu.d_[2], 0x80000000);
        CHECK_EQ(e.cpu.getSR() & 0x0f, 0x0a);
    }
    { // DIVS.W D1,D2 で D1=0: ベクタ 5、N=0 Z=1 V=0 C=0
        Env e(fast);
        e.bus.put(0x1000, {0x85c1});
        e.cpu.d_[1] = 0;
        e.cpu.d_[2] = 0xffffffff;
        e.cpu.setSR(0x2700 | 0x0b);
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, handler(5));
        CHECK_EQ(e.bus.r16(e.cpu.a_[7]) & 0x0f, 0x04);
    }
    { // DIVU.W D1,D2 で D1.W=0 (上位は無視): ベクタ 5
        Env e(fast);
        e.bus.put(0x1000, {0x84c1});
        e.cpu.d_[1] = 0x12340000;
        e.cpu.d_[2] = 1234;
        e.cpu.setSR(0x2700);
        e.steps(1);
        uint32_t sp = e.cpu.a_[7];
        CHECK_EQ(e.cpu.pc_, handler(5));
        CHECK_EQ(sp, 0x9000 - 12);
        CHECK_EQ(e.bus.r32(sp + 2), 0x1002);
        CHECK_EQ(e.bus.r16(sp + 6), 0x2000 | 5 * 4);
        CHECK_EQ(e.bus.r32(sp + 8), 0x1000);
        CHECK_EQ(e.cpu.d_[2], 1234);
    }
}

} // namespace

// BFFFO: 結果はオフセット + 先頭の 0 の数。オフセットが $7FFFFFFF でも 32 ビットで回り込む (符号付きであふれない)
void testBfffoOffset(bool fast) {
    { // BFFFO D0{D1:32},D2。D0 = 0 なので $7FFFFFFF + 32
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0xedc0, 0x2840, 0x4e71});
        c.d_[0] = 0;
        c.d_[1] = 0x7fffffff;
        e.steps(1);
        CHECK_EQ(c.d_[2], 0x8000001fu);
        CHECK_EQ(c.flagZ_, 1);
    }
    { // BFFFO (A0){D1:32},D2。読むのは A0 + $0FFFFFFF から (テストのバスは 24 ビットで回り込む)
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0xedd0, 0x2840, 0x4e71});
        c.a_[0] = 0x3000;
        c.d_[1] = 0x7fffffff;
        e.steps(1);
        CHECK_EQ(c.pc_, 0x1004);
        CHECK_EQ(c.d_[2], 0x8000001fu);
    }
    { // ふつうの場合: BFFFO D0{4:8},D2。D0 のビット 31-4 の 0 が 2 つ続いたあとに 1
        Env e(fast);
        Cpu020& c = e.cpu;
        e.bus.put(0x1000, {0xedc0, 0x2108, 0x4e71});
        c.d_[0] = 0x02000000;
        e.steps(1);
        CHECK_EQ(c.d_[2], 6u);
        CHECK_EQ(c.flagZ_, 0);
    }
}

// run() は、attn_ が 0 の間、命令の前の検査 (止まっているか、割り込み、トレース) を省く (cpu020.h の attn_)。
// 検査が要るようになる所 (setSR()、setIPL()、setRegisters()、命令の打ち切り、reset()) が attn_ を立てることを確かめる。
// どれも run() だけで動かし (step() は自分で attn_ を立てる)、先に何命令か動かして attn_ の ATTN_CHECK が 0 になったのを
// 見てから調べる (68060 は、リセットのあと命令キャッシュが無効なので、ATTN_FETCH は立ったまま)。
// 例外の行き先は STOP #$2700 にして、積まれた PC から、どの命令の切れ目で例外になったかを見る。
// 68030 と 68060 の両方で動かす (run() の繰り返しは CPU の種類ごとに別のコードになる)
void testAttention(bool fast, CpuModel model) {
    const uint32_t kStop = 0x3000;
    const uint16_t NOP = 0x4e71, ADDQ_D0 = 0x5280, LOOP = 0x60fe; // LOOP は BRA.S *
    auto prepare = [&](Env& e, std::initializer_list<uint16_t> code, int vector) {
        e.bus.put(0x1000, code);
        e.bus.put(kStop, {0x4e72, 0x2700});
        if (vector) e.bus.w32((uint32_t)vector * 4, kStop);
    };
    { // 命令 (ANDI to SR) が割り込みマスクを下げた: 待っていたレベル 3 の割り込みを、その命令の直後に受け付ける
        Env e(fast, model);
        Cpu020& c = e.cpu;
        prepare(e, {NOP, NOP, NOP, NOP, NOP, NOP, 0x027c, 0xf8ff, ADDQ_D0, ADDQ_D0, ADDQ_D0, ADDQ_D0, LOOP}, 27);
        c.setIPL(3);
        c.run(2);
        CHECK_EQ(c.attn_ & Cpu020::ATTN_CHECK, 0);
        CHECK_EQ(c.pc_ < 0x100c, true);
        c.run(300);
        CHECK_EQ(c.stopped(), true);
        CHECK_EQ(c.pc_, kStop + 4);
        CHECK_EQ(e.bus.r32(c.a_[7] + 2), 0x1010); // ANDI の次の命令
        CHECK_EQ(c.d_[0], 0u);
    }
    { // 命令 (ORI to SR) が T1 を立てた: 次の命令のあとでトレースの例外になる (ORI 自身は、始めの T が 0 なのでトレースしない)
        Env e(fast, model);
        Cpu020& c = e.cpu;
        prepare(e, {NOP, NOP, NOP, NOP, NOP, NOP, 0x007c, 0x8000, ADDQ_D0, ADDQ_D0, ADDQ_D0, ADDQ_D0, LOOP}, 9);
        c.run(2);
        CHECK_EQ(c.attn_ & Cpu020::ATTN_CHECK, 0);
        CHECK_EQ(c.pc_ < 0x100c, true);
        c.run(300);
        CHECK_EQ(c.stopped(), true);
        CHECK_EQ(c.pc_, kStop + 4);
        CHECK_EQ(e.bus.r32(c.a_[7] + 2), 0x1012); // ORI の次の ADDQ の、その次
        CHECK_EQ(e.bus.r32(c.a_[7] + 8), 0x1010); // トレースした命令
        CHECK_EQ(c.d_[0], 1u);
    }
    { // STOP: 次の命令へ進まない。割り込みで起きて、STOP の次から続ける
        Env e(fast, model);
        Cpu020& c = e.cpu;
        prepare(e, {NOP, NOP, NOP, NOP, NOP, NOP, 0x4e72, 0x2000, ADDQ_D0, LOOP}, 0);
        c.run(2);
        CHECK_EQ(c.attn_ & Cpu020::ATTN_CHECK, 0);
        CHECK_EQ(c.pc_ < 0x100c, true);
        c.run(300);
        CHECK_EQ(c.stopped(), true);
        CHECK_EQ(c.pc_, 0x1010);
        CHECK_EQ(c.d_[0], 0u);
        c.setIPL(5);
        c.run(200); // 処理ルーチン (ADDQ.L #1,(abs).L; RTE) が、レベル 5 を下げないので繰り返し動く
        CHECK_EQ(c.stopped(), false);
        CHECK_EQ(e.count(29) > 0, true);
        CHECK_EQ(c.d_[0], 0u);
        c.setIPL(0);
        c.run(300);
        CHECK_EQ(c.d_[0], 1u);
        CHECK_EQ(c.pc_, 0x1012);
    }
    { // run() と run() の間に setIPL() で割り込みのレベルを上げた: 次の run() の最初の命令の前に受け付ける
        Env e(fast, model);
        Cpu020& c = e.cpu;
        prepare(e, {LOOP}, 28);
        c.setSR(0x2000);
        c.run(20);
        CHECK_EQ(c.attn_ & Cpu020::ATTN_CHECK, 0);
        c.setIPL(4);
        c.run(300);
        CHECK_EQ(c.stopped(), true);
        CHECK_EQ(c.pc_, kStop + 4);
        CHECK_EQ(e.bus.r32(c.a_[7] + 2), 0x1000);
    }
    { // setRegisters() で割り込みマスクを下げた: 待っていた割り込みを、最初の命令の前に受け付ける
        Env e(fast, model);
        Cpu020& c = e.cpu;
        prepare(e, {LOOP}, 27);
        e.bus.put(0x2000, {ADDQ_D0, LOOP});
        c.setIPL(3);
        c.run(20);
        CHECK_EQ(c.attn_ & Cpu020::ATTN_CHECK, 0);
        Registers r;
        c.getRegisters(r);
        r.sr = 0x2000;
        r.pc = 0x2000;
        c.setRegisters(r);
        c.run(300);
        CHECK_EQ(c.stopped(), true);
        CHECK_EQ(e.bus.r32(c.a_[7] + 2), 0x2000);
        CHECK_EQ(c.d_[0], 0u);
    }
    { // setRegisters() で T1 を立てた: 最初の命令のあとでトレースの例外になる
        Env e(fast, model);
        Cpu020& c = e.cpu;
        prepare(e, {LOOP}, 9);
        e.bus.put(0x2000, {ADDQ_D0, ADDQ_D0, LOOP});
        c.run(20);
        CHECK_EQ(c.attn_ & Cpu020::ATTN_CHECK, 0);
        Registers r;
        c.getRegisters(r);
        r.sr = 0xa700;
        r.pc = 0x2000;
        c.setRegisters(r);
        c.run(300);
        CHECK_EQ(c.stopped(), true);
        CHECK_EQ(e.bus.r32(c.a_[7] + 2), 0x2002);
        CHECK_EQ(c.d_[0], 1u);
    }
    { // 例外のフレームを積めずに止まった (スタックがバスエラーになる所にある): 止まったままで、次の命令へ進まない
        Env e(fast, model);
        Cpu020& c = e.cpu;
        prepare(e, {NOP, NOP, NOP, NOP, NOP, NOP, 0x4a39, 0x00c0, 0x0000, ADDQ_D0, ADDQ_D0, LOOP}, 0); // TST.B $C00000
        c.a_[7] = 0xc00100;
        c.run(2);
        CHECK_EQ(c.attn_ & Cpu020::ATTN_CHECK, 0);
        CHECK_EQ(c.pc_ < 0x100c, true);
        c.run(300);
        CHECK_EQ(c.halted(), true);
        CHECK_EQ(c.d_[0], 0u);
        c.run(300);
        CHECK_EQ(c.halted(), true);
        CHECK_EQ(c.d_[0], 0u);
    }
}

// reset() がベクタを読めずに止まったあとは、run() が命令を実行しない (reset() の前に動いていて attn_ が 0 でも)。
// アドレス 0-7 の読み出しをバスエラーにできるバスを使う (fast page は使わない)
void testResetHalts(CpuModel model) {
    struct Bus0 : TestBus {
        bool bad0 = false;
        uint8_t read8(uint32_t a, int fc) override {
            if (bad0 && (a & 0xffffff) < 8) cpu->busError(a, false, fc);
            return TestBus::read8(a, fc);
        }
        uint16_t read16(uint32_t a, int fc) override {
            if (bad0 && (a & 0xffffff) < 8) cpu->busError(a, false, fc);
            return TestBus::read16(a, fc);
        }
    };
    Bus0 bus;
    Cpu020 c(&bus, model);
    bus.cpu = &c;
    bus.w32(0, 0x9000);
    bus.w32(4, 0x1000);
    bus.put(0x1000, {0x5280, 0x60fc}); // ADDQ.L #1,D0; BRA.S $1000
    c.reset();
    c.run(100);
    CHECK_EQ(c.attn_ & Cpu020::ATTN_CHECK, 0);
    CHECK_EQ(c.d_[0] > 0, true);
    bus.bad0 = true;
    c.reset();
    CHECK_EQ(c.halted(), true);
    const uint32_t d0 = c.d_[0];
    c.run(100);
    CHECK_EQ(c.halted(), true);
    CHECK_EQ(c.d_[0], d0);
}

// testStopFastForward() で、run() で進めた a と step() で進めた b の状態を比べる。68060 の命令のやり直しの記録
// (saveUndo() の写し) は、止まっているときだけ比べる (動いている間は、run() の速い経路と step() で記録の取り方が違う)
bool sameStopState(const Env& a, const Env& b, bool is060) {
    const Cpu020 &x = a.cpu, &y = b.cpu;
    bool same = x.sub_ == y.sub_ && x.pc_ == y.pc_ && x.getSR() == y.getSR() && x.stopped() == y.stopped() &&
                x.lastIpl_ == y.lastIpl_ && x.nmiPending_ == y.nmiPending_;
    for (int i = 0; i < 8; i++) same = same && x.d_[i] == y.d_[i] && x.a_[i] == y.a_[i];
    for (int v = 0; v < 256; v++) same = same && a.count(v) == b.count(v);
    if (is060 && x.stopped() && y.stopped()) {
        for (int i = 0; i < 8; i++) same = same && x.undoA_[i] == y.undoA_[i];
        for (int i = 0; i < 5; i++) same = same && x.undoCcr_[i] == y.undoCcr_[i];
        same = same && x.undoS_ == y.undoS_ && x.undoM_ == y.undoM_ && x.undoFull_ == y.undoFull_;
    }
    return same;
}

// STOP で止まっている間、run() は区間の終わりまで 1 回で進める (Cpu020::stopUntil())。進めた先が、execOne() を
// 4 サイクルずつ繰り返したとき (step() は毎回 execOne() を通る) と 1/SUB サイクルまで同じで、割り込みで起きる時刻も
// 同じことを、速さ、区間の長さ、STOP の SR (T0 のトレース、レベル 7 の割り込み) を変えて確かめる。
// プログラムは STOP; ADDQ.L #1,D0; BRA.S STOP。割り込みは何区間かおきに上げて、次の区間で下げる
void testStopFastForward(CpuModel model) {
    const bool is060 = model == CpuModel::M68060;
    const int speeds030[] = {100, 137, 250, 3200};
    const int speeds060[] = {5, 100, 137, 1300, 3200};
    const int64_t slices[] = {1, 2, 3, 7, 50, 333, 1000};
    const uint16_t srs[] = {0x2000, 0x6000, 0x2700}; // 0x6000 は T0 (68030 だけ)、0x2700 はレベル 7 で起こす
    for (uint16_t sr : srs) {
        if (is060 && sr == 0x6000) continue;
        const int level = sr == 0x2700 ? 7 : 3;
        const int* speeds = is060 ? speeds060 : speeds030;
        const int nSpeeds = is060 ? 5 : 4;
        for (int si = 0; si < nSpeeds; si++) {
            for (int64_t slice : slices) {
                Env a(true, model), b(true, model); // a は run()、b は step() で進める
                for (Env* e : {&a, &b}) {
                    e->bus.put(0x1000, {0x4e72, sr, 0x5280, 0x60f8});
                    e->cpu.setSpeed(speeds[si]);
                }
                bool same = true;
                for (int k = 0; k < 120 && same; k++) {
                    int ipl = -1;
                    if (k % 9 == 4) ipl = level;
                    else if (k % 9 == 5) ipl = 0;
                    if (ipl >= 0) {
                        a.cpu.setIPL(ipl);
                        b.cpu.setIPL(ipl);
                    }
                    a.cpu.run(slice);
                    // run() と同じ区間の終わり (始めの sub_ は a と b で同じ)
                    const uint64_t bEnd = b.cpu.sub_ + (uint64_t)slice * Cpu020::SUB;
                    while (b.cpu.sub_ < bEnd) b.cpu.step();
                    same = sameStopState(a, b, is060);
                }
                CHECK_EQ(same, true);
                // 止まって、起きて、ループを回ったこと (試験が空回りしていない)
                CHECK_EQ(a.count(24 + level) > 0, true);
                CHECK_EQ(a.cpu.d_[0] > 0, true);
                if (sr == 0x6000) CHECK_EQ(a.count(9) > 0, true);
            }
        }
    }
    // 割り込みのレベルが上がったまま (lastIpl_ と同じ) マスクで待たされている状態から、STOP がマスクを下げる:
    // STOP のあと、区間の途中でも早送りせずに、次の命令の切れ目で割り込みを受け付ける (runLoop() の ipl_ <= imask_)。
    // プログラムは NOP; STOP #$2000; ADDQ.L #1,D0; BRA.S STOP。SR = $2700 で始め、レベル 3 を上げたままにする
    for (int64_t slice : slices) {
        Env a(true, model), b(true, model);
        for (Env* e : {&a, &b}) {
            e->bus.put(0x1000, {0x4e71, 0x4e72, 0x2000, 0x5280, 0x60f8});
            e->cpu.setSR(0x2700);
            e->cpu.setIPL(3);
        }
        bool same = true;
        for (int k = 0; k < 40 && same; k++) {
            if (k == 20) { // 下げると、処理ルーチンから戻って STOP で止まる
                a.cpu.setIPL(0);
                b.cpu.setIPL(0);
            }
            a.cpu.run(slice);
            const uint64_t bEnd = b.cpu.sub_ + (uint64_t)slice * Cpu020::SUB;
            while (b.cpu.sub_ < bEnd) b.cpu.step();
            same = sameStopState(a, b, is060);
        }
        CHECK_EQ(same, true);
        CHECK_EQ(a.count(27) > 0, true);
        CHECK_EQ(a.cpu.stopped(), true);
    }
}

// endRun(): 命令の途中の書き込みから呼ぶと、run() はその命令が終わったところで戻る。run() の外で呼んだものは残らない
void testEndRun(CpuModel model) {
    struct HookBus : TestBus {
        void write8(uint32_t a, uint8_t v, int fc) override {
            if ((a & 0xffffff) == 0x200000) cpu->endRun();
            TestBus::write8(a, v, fc);
        }
        void write16(uint32_t a, uint16_t v, int fc) override {
            if ((a & 0xffffff) == 0xbffffc) cpu->endRun();
            TestBus::write16(a, v, fc);
        }
    };
    const uint16_t NOP = 0x4e71, ADDQ_D0 = 0x5280;
    auto make = [&](HookBus& bus, Cpu020& c, std::initializer_list<uint16_t> code) {
        bus.cpu = &c;
        bus.w32(0, 0x9000);
        bus.w32(4, 0x1000);
        bus.put(0x1000, code);
        c.reset();
    };
    { // 検査を省いて動いている間 (run() の 2 つ目からの命令) に呼ぶ: MOVE.B D0,$200000 の次で戻る
        HookBus bus;
        Cpu020 c(&bus, model);
        make(bus, c, {NOP, NOP, 0x13c0, 0x0020, 0x0000, ADDQ_D0, ADDQ_D0, 0x60fe});
        const int64_t ran = c.run(1000);
        CHECK_EQ(c.pc_, 0x100a);
        CHECK_EQ(c.d_[0], 0u);
        CHECK_EQ(ran < 100, true);
        c.run(1000); // 続きは最後まで動く
        CHECK_EQ(c.d_[0], 2u);
    }
    { // 検査を通る命令 (reset() の直後の 1 つ目) の途中で呼ぶ
        HookBus bus;
        Cpu020 c(&bus, model);
        make(bus, c, {0x13c0, 0x0020, 0x0000, ADDQ_D0, ADDQ_D0, 0x60fe});
        c.run(1000);
        CHECK_EQ(c.pc_, 0x1006);
        CHECK_EQ(c.d_[0], 0u);
    }
    { // 呼んだあと、同じ命令の後のアクセスがバスエラーになる (MOVEM.L D0-D1,(A0) で、1 つ目を $BFFFFC に書き、
      // 2 つ目が $C00000): 例外のフレームを積んだところで戻り、処理ルーチンの命令は実行しない
        HookBus bus;
        Cpu020 c(&bus, model);
        make(bus, c, {NOP, NOP, 0x48d0, 0x0003, ADDQ_D0, 0x60fe});
        bus.w32(2 * 4, 0x3000);
        bus.put(0x3000, {ADDQ_D0, ADDQ_D0, 0x60fe});
        c.a_[0] = 0xbffffc;
        const int64_t ran = c.run(1000);
        CHECK_EQ(c.pc_, 0x3000);
        CHECK_EQ(c.d_[0], 0u);
        CHECK_EQ(ran < 200, true);
        c.run(1000);
        CHECK_EQ(c.d_[0], 2u);
    }
    { // run() の外で呼ぶ: 次の run() は渡したサイクル数だけ動く
        HookBus bus;
        Cpu020 c(&bus, model);
        make(bus, c, {ADDQ_D0, ADDQ_D0, 0x60fe});
        c.run(10);
        c.endRun();
        const int64_t ran = c.run(1000);
        CHECK_EQ(ran >= 1000, true);
        CHECK_EQ(c.d_[0], 2u);
    }
}

// バスエラー / アドレスエラーのベクタが奇数: フレームを 1 つ積んだところで止まる (ダブルバスフォルト。MC68030UM 8.1.3)。
// 以前は、ベクタ 3 が奇数だと、アドレスエラーのフレームを積み続けて戻らなかった
void testOddFaultVector(bool fast) {
    for (int useRun = 0; useRun < 2; useRun++) {
        { // アドレスエラーのベクタが奇数で、奇数番地へ JMP
            Env e(fast);
            Cpu020& c = e.cpu;
            e.bus.put(0x1000, {0x4ef9, 0x0000, 0x2001, 0x5280}); // JMP $2001; ADDQ.L #1,D0
            e.bus.w32(3 * 4, 0x4001);
            if (useRun) c.run(200);
            else e.steps(3);
            CHECK_EQ(c.halted(), true);
            CHECK_EQ(c.a_[7], 0x9000 - 92);
            CHECK_EQ(e.bus.r16(c.a_[7] + 6), 0xb000 | 3 * 4);
            CHECK_EQ(c.d_[0], 0u);
        }
        { // バスエラーのベクタが奇数で、バスエラー
            Env e(fast);
            Cpu020& c = e.cpu;
            e.bus.put(0x1000, {0x4a39, 0x00c0, 0x0000, 0x5280}); // TST.B $C00000; ADDQ.L #1,D0
            e.bus.w32(2 * 4, 0x4001);
            if (useRun) c.run(200);
            else e.steps(3);
            CHECK_EQ(c.halted(), true);
            CHECK_EQ(c.a_[7], 0x9000 - 92);
            CHECK_EQ(e.bus.r16(c.a_[7] + 6), 0xb000 | 2 * 4);
            CHECK_EQ(c.d_[0], 0u);
        }
        { // ほかの例外のベクタが奇数で、アドレスエラーのベクタも奇数: アドレスエラーのフレームまで積んで止まる
            Env e(fast);
            Cpu020& c = e.cpu;
            e.bus.put(0x1000, {0x4afc, 0x5280}); // ILLEGAL; ADDQ.L #1,D0
            e.bus.w32(4 * 4, 0x4001);
            e.bus.w32(3 * 4, 0x4001);
            if (useRun) c.run(200);
            else e.steps(3);
            CHECK_EQ(c.halted(), true);
            CHECK_EQ(c.a_[7], 0x9000 - 8 - 92);
            CHECK_EQ(c.d_[0], 0u);
        }
        { // ほかの例外のベクタだけが奇数なら、アドレスエラーの処理ルーチンへ進む (止まらない)
            Env e(fast);
            Cpu020& c = e.cpu;
            e.bus.put(0x1000, {0x4afc});
            e.bus.w32(4 * 4, 0x4001);
            if (useRun) c.run(30);
            else e.steps(1);
            CHECK_EQ(c.halted(), false);
            if (!useRun) CHECK_EQ(c.pc_, handler(3));
            CHECK_EQ(e.bus.r16(0x9000 - 8 - 92 + 6), 0xb000 | 3 * 4);
        }
    }
}

// ロングの拡張語をまとめて読んでも、1 語ずつ読んだときと値・PC・時刻が同じ。
// 4 バイト境界、ページの末尾、ページまたぎ、TLB にないページを含める。
void testFetchLong(bool fast, CpuModel model) {
    Env e(fast, model);
    for (uint32_t a : {0x1002u, 0x1004u, 0x1ffcu, 0x1ffeu, 0x2000u, 0xffffeu}) {
        e.bus.put(a - 2, {0x4e71, 0x1234, 0xabcd});
        auto prepare = [&]() {
            e.cpu.flushTlb();
            e.cpu.pc_ = a - 2;
            e.cpu.xfetch16(); // 直前のオペコードを読んだ状態
        };
        prepare();
        const uint64_t before = e.cpu.sub_;
        const uint32_t got = e.cpu.xfetch32();
        const uint64_t elapsed = e.cpu.sub_ - before;
        CHECK_EQ(got, 0x1234abcdu);
        CHECK_EQ(e.cpu.pc_, a + 4);
        prepare();
        const uint64_t refBefore = e.cpu.sub_;
        const uint32_t hi = e.cpu.xfetch16();
        const uint32_t lo = e.cpu.xfetch16();
        CHECK_EQ(got, (hi << 16) | lo);
        CHECK_EQ(elapsed, e.cpu.sub_ - refBefore);
        CHECK_EQ(e.cpu.pc_, a + 4);
        // ホスト側で語を書き換えた直後にも、新しい値を読む。
        e.bus.w32(a, 0x87654321);
        e.cpu.pc_ = a;
        CHECK_EQ(e.cpu.xfetch32(), 0x87654321u);
    }
    // 即値の後半の語だけが次のページでバスエラー。前のページが直接読めても、
    // ホストの連続したメモリから 4 バイトを読み抜けて命令を完了してはいけない。
    if (fast) e.cpu.setFastPage(true, 0xbf, &e.bus.mem[0xbf0000], &e.bus.mem[0xbf0000]);
    e.bus.put(0xbffffc, {0x203c, 0x1234, 0xabcd}); // MOVE.L #$1234ABCD,D0
    e.cpu.pc_ = 0xbffffc;
    e.cpu.d_[0] = 0xdeadbeef;
    e.cpu.step();
    CHECK_EQ(e.cpu.pc_, handler(2));
    CHECK_EQ(e.cpu.d_[0], 0xdeadbeefu);
    CHECK_EQ(e.bus.r32(e.cpu.a_[7] + 2), 0xbffffcu);
}

// 同じ論理アドレスに別のページを割り当て、両方の TLB を温めてから S を切り替える。
// 例外・割り込みの入口と RTE、外からのレジスタ設定、リセットでもデータ空間が切り替わる。
void testDataSpace(CpuModel model) {
    Env e(true, model);
    Cpu020& c = e.cpu;
    std::vector<uint8_t> user(65536);
    c.setFastPage(false, 3, user.data(), user.data());
    const uint32_t addr = 0x30000;
    c.xwrite32(addr, 0x11223344);
    c.setSR(0x0000);
    c.xwrite32(addr, 0x55667788);
    CHECK_EQ(c.xread32(addr), 0x55667788u);
    c.setSR(0x2000);
    CHECK_EQ(c.xread32(addr), 0x11223344u);
    c.setSR(0x0000);
    c.exception(4, 0, c.pc_);
    CHECK_EQ(c.xread32(addr), 0x11223344u);
    c.opRte(0x4e73);
    CHECK_EQ(c.xread32(addr), 0x55667788u);
    c.interrupt(4);
    CHECK_EQ(c.xread32(addr), 0x11223344u);
    c.opRte(0x4e73);
    CHECK_EQ(c.xread32(addr), 0x55667788u);
    Registers r;
    c.getRegisters(r);
    r.sr = 0x2000;
    c.setRegisters(r);
    CHECK_EQ(c.xread32(addr), 0x11223344u);
    r.sr = 0;
    c.setRegisters(r);
    CHECK_EQ(c.xread32(addr), 0x55667788u);
    c.reset();
    CHECK_EQ(c.xread32(addr), 0x11223344u);
    CHECK_EQ(e.bus.r32(addr), 0x11223344u);
    CHECK_EQ(user[0], 0x55);
}

// 拡張語の brief 形式 ((d8,An,Xn) と (d8,PC,Xn)) のアドレスとサイクル数。直前の語を読んでから計算する速い経路 (indexAddr()) と、
// TLB を空にした直後の extAddr() の経路で、アドレス・PC・時刻の進みが同じで、アドレスが base + Xn × スケール + d8 になる。
// スケール 4 通り、語/ロング、Dn/An、d8 の符号の境目を、ページの中、ページの最後の語、次のページの先頭で試す。
void testIndexAddr(bool fast, CpuModel model) {
    Env e(fast, model);
    Cpu020& c = e.cpu;
    const uint32_t regs[16] = {0x00008000, 0x00007fff, 0xffff8001, 0x12340004, 0x8000fffe, 0x00010000, 0x7fffffff, 0x80000000,
                               0x00002000, 0x0000ff00, 0xfffe8000, 0x00017ffe, 0x00100000, 0x0000fffc, 0x00003000, 0x00008ffe};
    auto setRegs = [&]() {
        for (int i = 0; i < 8; i++) {
            c.d_[i] = regs[i];
            c.a_[i] = regs[8 + i];
        }
    };
    int bad = 0;
    for (uint32_t a : {0x1002u, 0x1ffeu, 0x2000u}) {
        for (int pcRel = 0; pcRel < 2; pcRel++) {
            for (int xn = 0; xn < 16; xn++) {
                for (int wl = 0; wl < 2; wl++) {
                    for (int scale = 0; scale < 4; scale++) {
                        for (uint32_t d8 : {0x00u, 0x01u, 0x7fu, 0x80u, 0xffu}) {
                            const uint16_t ext = (uint16_t)(xn << 12 | wl << 11 | scale << 9 | d8);
                            e.bus.put(a - 2, {0x4e71, ext});
                            // A: 直前の語を読んだ状態 (速い経路)
                            c.flushTlb();
                            c.pc_ = a - 2;
                            c.xfetch16();
                            setRegs();
                            uint64_t t0 = c.sub_;
                            const uint32_t gotA = pcRel ? c.xeaAddr(7, 3, 2) : c.xeaAddr(6, 3, 2);
                            const uint64_t dA = c.sub_ - t0;
                            const uint32_t pcA = c.pc_;
                            // B: TLB を空にした直後 (extAddr() の経路)
                            c.flushTlb();
                            c.pc_ = a;
                            setRegs();
                            t0 = c.sub_;
                            const uint32_t gotB = pcRel ? c.xeaAddr(7, 3, 2) : c.xeaAddr(6, 3, 2);
                            const uint64_t dB = c.sub_ - t0;
                            uint32_t x = regs[xn];
                            if (!wl) x = (uint32_t)(int32_t)(int16_t)x;
                            x <<= scale;
                            const uint32_t want = (pcRel ? a : regs[8 + 3]) + x + (uint32_t)(int32_t)(int8_t)d8;
                            if ((gotA != want || gotB != want || dA != dB || pcA != a + 2 || c.pc_ != a + 2) && bad++ < 5)
                                printf("  a %x pcRel %d ext %04x: A %08x B %08x want %08x dA %llu dB %llu pc %x %x\n", a, pcRel, ext, gotA,
                                       gotB, want, (unsigned long long)dA, (unsigned long long)dB, pcA, c.pc_);
                        }
                    }
                }
            }
        }
    }
    CHECK_EQ(bad, 0);
}

// setSpeed() の刻み (#131): 速さを 1% ずつ上げると、実際の速さ (1 サイクルあたりのコストから求めた値) は減らず、
// 指定との差は 0.5% より小さい (コストは切り捨てなので、実際の速さは指定以上)。speed() は実際の速さの切り捨て。
// 既定の速さ (100) のコストは、SUB に対してちょうどの値 (68030 は 68000 の 3/5 サイクル、68060 は 1 サイクル)
void testSpeedSteps(CpuModel model) {
    const bool is060 = model == CpuModel::M68060;
    Env e(true, model);
    Cpu020& c = e.cpu;
    c.setSpeed(100);
    if (is060) {
        CHECK_EQ(c.unit060_, Cpu020::SUB);
        CHECK_EQ(c.cost_, 0);
    } else {
        CHECK_EQ(c.cost_, Cpu020::SUB * 3 / 5);
        CHECK_EQ(c.fpuCost_, Cpu020::SUB);
    }
    CHECK_EQ(c.speed(), 100);
    double prev = 0;
    int bad = 0;
    for (int n = is060 ? Cpu020::SPEED060_MIN : 100; n <= Cpu020::SPEED_MAX; n++) {
        c.setSpeed(n);
        const double eff = is060 ? 100.0 * Cpu020::SUB / c.unit060_ : 100.0 * c.baseCost_ / c.cost_;
        const bool ok = eff >= prev && eff >= n && eff < n * 1.005 && c.speed() == (int)eff;
        if (!ok && bad++ < 5) printf("  setSpeed(%d): effective %.3f%%, speed() %d\n", n, eff, c.speed());
        prev = eff;
    }
    CHECK_EQ(bad, 0);
    c.setSpeed(Cpu020::SPEED_MAX + 1000); // 上限で止まる
    CHECK_EQ(c.speed() >= Cpu020::SPEED_MAX && c.speed() < Cpu020::SPEED_MAX * 1.005, true);
}

int main() {
    for (int fast = 0; fast < 2; fast++) {
        testInterruptMaster(fast);
        testTraceChangeOfFlow(fast);
        testRteFormatError(fast);
        testStop(fast);
        testBusErrorSsw(fast);
        testBusErrorRestart(fast);
        testBcd(fast);
        testDivL(fast);
        testChk(fast);
        testChk2Cmp2(fast);
        testDivW(fast);
        testBfffoOffset(fast);
        testAttention(fast, CpuModel::M68030);
        testAttention(fast, CpuModel::M68060);
        testOddFaultVector(fast);
        testFetchLong(fast, CpuModel::M68030);
        testFetchLong(fast, CpuModel::M68060);
        testIndexAddr(fast, CpuModel::M68030);
        testIndexAddr(fast, CpuModel::M68060);
    }
    testDataSpace(CpuModel::M68030);
    testDataSpace(CpuModel::M68060);
    testResetHalts(CpuModel::M68030);
    testResetHalts(CpuModel::M68060);
    testStopFastForward(CpuModel::M68030);
    testStopFastForward(CpuModel::M68060);
    testEndRun(CpuModel::M68030);
    testEndRun(CpuModel::M68060);
    testSpeedSteps(CpuModel::M68030);
    testSpeedSteps(CpuModel::M68060);
    return finish();
}
