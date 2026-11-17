// 68882 (Fpu) の単体テスト: 例外を許可したときの結果の書き方、例外のフレーム、シグナリング NaN、FMOVEM、
// FSAVE / FRESTORE。根拠は MC68881/MC68882 User's Manual (MC68881UM) の 6 章
#include "cpu020_env.h"
#include "m68k/fpu.h"
#include "test_check.h"

using namespace cpu020test;
using m68k::Fpu;
namespace fp = m68k::fp;

namespace {

const fp::X kZero = {0x0000, 0};
const fp::X kOne = {0x3fff, 0x8000000000000000ull};
const fp::X kTwo = {0x4000, 0x8000000000000000ull};
const fp::X kInf = {0x7fff, 0};
const fp::X kHuge = {0x7ffe, 0x8000000000000000ull}; // 2^16383
const fp::X kSnan = {0x7fff, 0x2000000000000000ull};   // シグナリング NaN (bit62 が 0)

bool same(const fp::X& a, const fp::X& b) { return a.se == b.se && a.m == b.m; }

// SNAN、OPERR、DZ の例外を許可していれば、デスティネーションのレジスタは変えない (6.1.2、6.1.3、6.1.6)。
// FPn に書く命令の例外は保留され、次の FPU の命令の前の例外 (フォーマット $0、PC はその命令) になる (6.1、6.4.2.2)
void testNoWriteWhenEnabled(bool fast) {
    { // OPERR: 0 / 0
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0420, 0x4e71, 0xf280, 0x0000}); // FDIV.X FP1,FP0
        f->fp_[0] = kZero;
        f->fp_[1] = kZero;
        f->fpcr_ = fp::EXC_OPERR;
        e.steps(2); // 演算と NOP。例外は保留されたまま
        CHECK_EQ(e.cpu.pc_, 0x1006);
        e.steps(1); // 次の FPU の命令 (FNOP) の前に起こす
        CHECK_EQ(e.cpu.pc_, handler(52));
        CHECK_EQ(same(f->fp_[0], kZero), true);
        CHECK_EQ((f->fpsr_ & fp::EXC_OPERR) != 0, true);
        CHECK_EQ(f->fpiar_, 0x1000);
        CHECK_EQ(e.bus.r16(e.cpu.a_[7] + 6), 52 * 4);
        CHECK_EQ(e.bus.r32(e.cpu.a_[7] + 2), 0x1006); // 命令の前の例外: PC は FNOP (6.1)
    }
    { // 禁止なら NaN を書く
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0420, 0x4e71});
        f->fp_[0] = kZero;
        f->fp_[1] = kZero;
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, 0x1004);
        CHECK_EQ(fp::isNan(f->fp_[0]), true);
    }
    { // DZ: 1 / 0
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0420, 0x4e71, 0xf280, 0x0000});
        f->fp_[0] = kOne;
        f->fp_[1] = kZero;
        f->fpcr_ = fp::EXC_DZ;
        e.steps(2); // 演算と NOP。例外は保留されたまま
        CHECK_EQ(e.cpu.pc_, 0x1006);
        e.steps(1); // 次の FPU の命令 (FNOP) の前に起こす
        CHECK_EQ(e.cpu.pc_, handler(50));
        CHECK_EQ(same(f->fp_[0], kOne), true);
    }
    { // OVFL は許可していても結果を書く (RN なら無限大)
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0422, 0x4e71, 0xf280, 0x0000}); // FADD.X FP1,FP0
        f->fp_[0] = kHuge;
        f->fp_[1] = kHuge;
        f->fpcr_ = fp::EXC_OVFL;
        e.steps(2); // 演算と NOP。例外は保留されたまま
        CHECK_EQ(e.cpu.pc_, 0x1006);
        e.steps(1); // 次の FPU の命令 (FNOP) の前に起こす
        CHECK_EQ(e.cpu.pc_, handler(53));
        CHECK_EQ(same(f->fp_[0], kInf), true);
    }
    { // OVFL を禁止し INEX を許可すると、INEX の例外になる (6.1.7 の NOTE)
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0422, 0x4e71, 0xf280, 0x0000});
        f->fp_[0] = kHuge;
        f->fp_[1] = kHuge;
        f->fpcr_ = fp::EXC_INEX2;
        e.steps(2); // 演算と NOP。例外は保留されたまま
        CHECK_EQ(e.cpu.pc_, 0x1006);
        e.steps(1); // 次の FPU の命令 (FNOP) の前に起こす
        CHECK_EQ(e.cpu.pc_, handler(49));
    }
}

// FPn に書く命令の例外を知らせる時期 (MC68881UM 6.1、6.4.2.2、5.2.2)。保留している間も CPU の命令は進む。
// FMOVEM と FMOVE の制御レジスタは保留を知らせず、演算・FMOVE と条件の命令 (FBcc など) が命令の前の例外にする。
// 68882 は知らせても保留を消さないので、RTE だけの処理ルーチンなら同じ例外がまた起きる。FSAVE、bit27 を立てる、
// FRESTORE をすれば、命令は続く
void testPendingTiming(bool fast) {
    { // 保留したまま FPCR の許可を消すと、次の FPU の命令で保留も消える (許可を戻しても、あとで例外にならない)
        Env e(fast);
        Cpu020& c = e.cpu;
        Fpu* f = c.fpu();
        e.bus.put(0x1000, {
                              0xf200, 0x0422,                 // $1000 FADD.X FP1,FP0 (あふれ)
                              0xf23c, 0x9000, 0x0000, 0x0000, // $1004 FMOVE.L #0,FPCR
                              0xf280, 0x0000,                 // $100C FNOP
                              0xf23c, 0x9000, 0x0000, 0x1000, // $1010 FMOVE.L #OVFL,FPCR
                              0xf280, 0x0000,                 // $1018 FNOP
                              0x4e71,                         // $101C NOP
                          });
        f->fp_[0] = kHuge;
        f->fp_[1] = kHuge;
        f->fpcr_ = fp::EXC_OVFL;
        e.steps(5);
        CHECK_EQ(c.pc_, 0x101cu);
        CHECK_EQ(f->excPending_, false);
        CHECK_EQ(e.count(53), 0u);
    }
    for (int properHandler = 0; properHandler < 2; properHandler++) {
        Env e(fast);
        Cpu020& c = e.cpu;
        Fpu* f = c.fpu();
        e.bus.put(0x1000, {
                              0xf200, 0x0422, // $1000 FADD.X FP1,FP0 (あふれ)
                              0x7005,         // $1004 MOVEQ #5,D0
                              0xf227, 0xe080, // $1006 FMOVEM.X FP0,-(A7)
                              0xf201, 0xa800, // $100A FMOVE.L FPSR,D1
                              0xf281, 0x0002, // $100E FBNE.W $1012 (つねに成り立つ: Z は 0)
                              0x4e71,         // $1012 NOP
                          });
        if (properHandler) {
            // FSAVE -(A7); BSET #3,56(A7); FRESTORE (A7)+; ADDQ.L #1,COUNTS+53*4; RTE
            e.bus.put(0x5000, {0xf327, 0x08ef, 0x0003, 0x0038, 0xf35f, 0x52b9, (uint16_t)(COUNTS >> 16),
                               (uint16_t)(COUNTS + 53 * 4), 0x4e73});
            e.bus.w32(53 * 4, 0x5000);
        }
        f->fp_[0] = kHuge;
        f->fp_[1] = kHuge;
        f->fpcr_ = fp::EXC_OVFL;
        e.steps(1);
        CHECK_EQ(c.pc_, 0x1004u);
        CHECK_EQ(same(f->fp_[0], kInf), true);
        CHECK_EQ(f->excPending_, true);
        e.steps(3); // MOVEQ、FMOVEM、FMOVE.L FPSR: どれも例外にしない
        CHECK_EQ(c.pc_, 0x100eu);
        CHECK_EQ(c.d_[0], 5u);
        CHECK_EQ(c.a_[7], 0x9000u - 12);
        CHECK_EQ((c.d_[1] & fp::EXC_OVFL) != 0, true);
        e.steps(1); // FBNE の前に起こす
        uint32_t sp = c.a_[7];
        CHECK_EQ(c.pc_, properHandler ? 0x5000u : handler(53));
        CHECK_EQ(e.bus.r16(sp + 6), 53 * 4);
        CHECK_EQ(e.bus.r32(sp + 2), 0x100eu);
        CHECK_EQ(f->fpiar_, 0x1000u);
        if (!properHandler) {
            e.steps(3); // ADDQ、RTE、FBNE でまた同じ例外
            CHECK_EQ(c.pc_, handler(53));
            CHECK_EQ(e.count(53), 1u);
            continue;
        }
        e.steps(1); // FSAVE: IDLE のフレームに保留が移る (BIU の bit27 が 0)。例外のオペランドは +$28、コマンドの語は +$04
        sp = c.a_[7];
        CHECK_EQ(e.bus.r32(sp), 0x1f380000u);
        CHECK_EQ(e.bus.r32(sp + 0x38) & 0x08000000u, 0u);
        CHECK_EQ(e.bus.r16(sp + 0x04), 0x0422);
        CHECK_EQ(e.bus.r32(sp + 0x24), 0u);
        CHECK_EQ(e.bus.r16(sp + 0x28), kHuge.se);
        CHECK_EQ(f->excPending_, false);
        e.steps(4); // BSET、FRESTORE、ADDQ、RTE
        CHECK_EQ(c.pc_, 0x100eu);
        CHECK_EQ(e.count(53), 1u);
        e.steps(1); // FBNE が実行される
        CHECK_EQ(c.pc_, 0x1012u);
    }
}

// FMOVE FPn,<ea> の例外は、書いたあとの命令の途中の例外 (フォーマット $9)。+$08 に命令のアドレス、
// +$10 に書いた先のアドレス (6.1.3、MC68030UM 表 8-6)
void testFmoveOutMidInstruction(bool fast) {
    Env e(fast);
    Fpu* f = e.cpu.fpu();
    e.bus.put(0x1000, {0xf210, 0x6000, 0x4e71}); // FMOVE.L FP0,(A0)
    e.cpu.a_[0] = 0x3000;
    f->fp_[0] = kInf;
    f->fpcr_ = fp::EXC_OPERR;
    e.steps(1);
    uint32_t sp = e.cpu.a_[7];
    CHECK_EQ(e.cpu.pc_, handler(52));
    CHECK_EQ(e.bus.r32(0x3000), 0x7fffffff); // 書いてから例外になる
    CHECK_EQ(sp, 0x9000 - 20);
    CHECK_EQ(e.bus.r16(sp + 6), 0x9000 | 52 * 4);
    CHECK_EQ(e.bus.r32(sp + 2), 0x1004);
    CHECK_EQ(e.bus.r32(sp + 8), 0x1000);
    CHECK_EQ(e.bus.r32(sp + 0x10), 0x3000);
    e.steps(2); // 処理ルーチンの RTE で次の命令に戻る
    CHECK_EQ(e.cpu.pc_, 0x1004);
    CHECK_EQ(e.cpu.a_[7], 0x9000);
}

// シグナリング NaN: FMOVEM はそのまま読み書きし、演算のオペランドになると SNAN を立てる (6.1.2)
void testSignalingNan(bool fast) {
    { // FMOVEM.X (A0),FP0 / FMOVEM.X FP0,(A1) で値が変わらない (非正規化数も)
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf210, 0xd080, 0xf211, 0xf080, 0xf210, 0xd080, 0xf211, 0xf080});
        e.cpu.a_[0] = 0x3000;
        e.cpu.a_[1] = 0x3100;
        e.bus.w32(0x3000, 0x7fff0000);
        e.bus.w32(0x3004, 0x20000000);
        e.bus.w32(0x3008, 0x00000001);
        e.steps(2);
        CHECK_EQ(f->fp_[0].se, 0x7fff);
        CHECK_EQ(f->fp_[0].m, 0x2000000000000001ull);
        CHECK_EQ(e.bus.r32(0x3104), 0x20000000);
        CHECK_EQ(e.bus.r32(0x3108), 0x00000001);
        CHECK_EQ(f->fpsr_ & 0xff00, 0);
        e.bus.w32(0x3000, 0x00000000); // 正規化されていない値
        e.bus.w32(0x3004, 0x00001234);
        e.bus.w32(0x3008, 0x56789abc);
        e.steps(2);
        CHECK_EQ(e.bus.r32(0x3104), 0x00001234);
        CHECK_EQ(e.bus.r32(0x3108), 0x56789abc);
    }
    { // レジスタのシグナリング NaN を演算に使うと SNAN が立つ。禁止なら NaN を書く
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0422, 0x4e71}); // FADD.X FP1,FP0
        f->fp_[0] = kOne;
        f->fp_[1] = kSnan;
        e.steps(1);
        CHECK_EQ((f->fpsr_ & fp::EXC_SNAN) != 0, true);
        CHECK_EQ(fp::isNan(f->fp_[0]), true);
        CHECK_EQ(fp::isSnan(f->fp_[0]), false);
    }
    { // 許可なら、レジスタは変えずにベクタ 54
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0422, 0x4e71, 0xf280, 0x0000});
        f->fp_[0] = kTwo;
        f->fp_[1] = kSnan;
        f->fpcr_ = fp::EXC_SNAN;
        e.steps(2); // 演算と NOP。例外は保留されたまま
        CHECK_EQ(e.cpu.pc_, 0x1006);
        e.steps(1); // 次の FPU の命令 (FNOP) の前に起こす
        CHECK_EQ(e.cpu.pc_, handler(54));
        CHECK_EQ(same(f->fp_[0], kTwo), true);
    }
    { // FMOVE.X FPn,<ea> は SNAN を立て、SNAN のビットを立てた NaN を書く
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf210, 0x6800, 0x4e71}); // FMOVE.X FP0,(A0)
        e.cpu.a_[0] = 0x3000;
        f->fp_[0] = kSnan;
        e.steps(1);
        CHECK_EQ((f->fpsr_ & fp::EXC_SNAN) != 0, true);
        CHECK_EQ(e.bus.r32(0x3004), 0x60000000);
    }
}

// FSAVE / FRESTORE: ユーザモードでは特権違反 (6.2.7)、68882 のものでないフレームはフォーマットエラー (6.2.8)
void testSaveRestore(bool fast) {
    for (uint16_t op : {0xf310, 0xf350}) { // FSAVE (A0) / FRESTORE (A0)
        Env e(fast);
        e.bus.put(0x1000, {op});
        e.cpu.usp_ = 0x7000;
        e.cpu.setSR(0x0000);
        e.cpu.a_[0] = 0x3000;
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, handler(8));
        CHECK_EQ(e.bus.r32(e.cpu.a_[7] + 2), 0x1000);
    }
    for (uint32_t head : {0x55aa0000u, 0x1f180000u, 0x20380000u}) {
        Env e(fast);
        e.bus.put(0x1000, {0xf358}); // FRESTORE (A0)+
        e.cpu.a_[0] = 0x3000;
        e.bus.w32(0x3000, head);
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, handler(14));
        CHECK_EQ(e.bus.r32(e.cpu.a_[7] + 2), 0x1000);
        CHECK_EQ(e.cpu.a_[0], 0x3000);
    }
    { // NULL → FSAVE は NULL。FPU の命令のあとの FSAVE は IDLE ($1F38、BIU フラグは保留なし)。FRESTORE (A0)+ は 60 バイト進む
        Env e(fast);
        e.bus.put(0x1000, {0xf327, 0xf200, 0x0422, 0xf327, 0xf35f, 0x4e71}); // FSAVE -(A7); FADD; FSAVE -(A7); FRESTORE (A7)+
        e.steps(1);
        CHECK_EQ(e.cpu.a_[7], 0x9000 - 4);
        CHECK_EQ(e.bus.r32(0x9000 - 4), 0x00000000);
        e.steps(2);
        uint32_t sp = e.cpu.a_[7];
        CHECK_EQ(sp, 0x9000 - 4 - 60);
        CHECK_EQ(e.bus.r32(sp), 0x1f380000);
        CHECK_EQ(e.bus.r32(sp + 56), 0x7c00ffff);
        e.steps(1);
        CHECK_EQ(e.cpu.a_[7], 0x9000 - 4);
        CHECK_EQ(e.cpu.pc_, 0x100a);
    }
    { // IDLE の BIU フラグの bit27 が 0 なら、例外の保留。次の FPU の命令の前 (PC はその命令) に起こす (6.4.2.2)
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf350, 0x4e71, 0xf280, 0x0000}); // FRESTORE (A0); NOP; FNOP
        e.cpu.a_[0] = 0x3000;
        e.bus.w32(0x3000, 0x1f380000);
        e.bus.w32(0x3000 + 56, 0x7400ffff);
        f->fpsr_ = fp::EXC_INEX2;
        f->fpcr_ = fp::EXC_INEX2;
        e.steps(2);
        CHECK_EQ(e.cpu.pc_, 0x1004);
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, handler(49));
        CHECK_EQ(e.bus.r32(e.cpu.a_[7] + 2), 0x1004);
        CHECK_EQ(e.bus.r16(e.cpu.a_[7] + 6), 49 * 4);
    }
}

// FMOVECR #0,FP0 は π を拡張精度・RN で丸めて入れ、INEX2 を立てる (MC68881UM の FMOVECR)
void testFmovecr(bool fast) {
    Env e(fast);
    Fpu* f = e.cpu.fpu();
    e.bus.put(0x1000, {0xf200, 0x5c00, 0x4e71}); // FMOVECR.X #0,FP0
    e.steps(1);
    CHECK_EQ(e.cpu.pc_, 0x1004);
    CHECK_EQ(same(f->fp_[0], fp::X{0x4000, 0xc90fdaa22168c235ull}), true);
    CHECK_EQ(f->fpsr_ & 0xff000000u, 0u);
    CHECK_EQ((f->fpsr_ & fp::EXC_INEX2) != 0, true);
}

// FMOVECR のマニュアルにないオフセット ($01-$0A、$10-$2F) を、FPCR の精度 (X / S / D / 3) と丸めの方向 (RN / RZ / RM / RP)
// のすべての組で読む。期待値は、次の規則を別に書いたモデルで作ったもの: 表の値を正規化せずに、仮数の bit63 から数えて
// 単精度 / 倍精度に丸める。$01、$02、$07 は単精度で 25 ビットに丸める。$03 と $07 は条件コードを変える (fpu_core.cpp の
// fmovecr())。fputest.x の FMOVECR の期待値 (実機の値) が見ている組はこれと合う。fputest.x が見ていない組は実機では確かめていない。
// $09、$0A、$10-$2F は、fputest.x の期待値 (実機の値) と同じく正規化した形にしてある
struct FmovecrCase {
    uint8_t off, prec, mode;
    uint16_t se;
    uint64_t m;
    uint32_t cc; // FPSR の条件コード (N Z I NAN)
    bool inex;
};
const FmovecrCase kFmovecrCases[] = {
    {0x01, 0, 0, 0x4001, 0xfe00068200000000ull, 0x0, false},
    {0x01, 0, 1, 0x4001, 0xfe00068200000000ull, 0x0, false},
    {0x01, 0, 2, 0x4001, 0xfe00068200000000ull, 0x0, false},
    {0x01, 0, 3, 0x4001, 0xfe00068200000000ull, 0x0, false},
    {0x01, 1, 0, 0x4001, 0xfe00068000000000ull, 0x0, true},
    {0x01, 1, 1, 0x4001, 0xfe00068000000000ull, 0x0, true},
    {0x01, 1, 2, 0x4001, 0xfe00068000000000ull, 0x0, true},
    {0x01, 1, 3, 0x4001, 0xfe00070000000000ull, 0x0, true},
    {0x01, 2, 0, 0x4001, 0xfe00068200000000ull, 0x0, false},
    {0x01, 2, 1, 0x4001, 0xfe00068200000000ull, 0x0, false},
    {0x01, 2, 2, 0x4001, 0xfe00068200000000ull, 0x0, false},
    {0x01, 2, 3, 0x4001, 0xfe00068200000000ull, 0x0, false},
    {0x01, 3, 0, 0x4001, 0xfe00068200000000ull, 0x0, false},
    {0x01, 3, 1, 0x4001, 0xfe00068200000000ull, 0x0, false},
    {0x01, 3, 2, 0x4001, 0xfe00068200000000ull, 0x0, false},
    {0x01, 3, 3, 0x4001, 0xfe00068200000000ull, 0x0, false},
    {0x02, 0, 0, 0x4001, 0xffc0050380000000ull, 0x0, false},
    {0x02, 0, 1, 0x4001, 0xffc0050380000000ull, 0x0, false},
    {0x02, 0, 2, 0x4001, 0xffc0050380000000ull, 0x0, false},
    {0x02, 0, 3, 0x4001, 0xffc0050380000000ull, 0x0, false},
    {0x02, 1, 0, 0x4001, 0xffc0050000000000ull, 0x0, true},
    {0x02, 1, 1, 0x4001, 0xffc0050000000000ull, 0x0, true},
    {0x02, 1, 2, 0x4001, 0xffc0050000000000ull, 0x0, true},
    {0x02, 1, 3, 0x4001, 0xffc0058000000000ull, 0x0, true},
    {0x02, 2, 0, 0x4001, 0xffc0050380000000ull, 0x0, false},
    {0x02, 2, 1, 0x4001, 0xffc0050380000000ull, 0x0, false},
    {0x02, 2, 2, 0x4001, 0xffc0050380000000ull, 0x0, false},
    {0x02, 2, 3, 0x4001, 0xffc0050380000000ull, 0x0, false},
    {0x02, 3, 0, 0x4001, 0xffc0050380000000ull, 0x0, false},
    {0x02, 3, 1, 0x4001, 0xffc0050380000000ull, 0x0, false},
    {0x02, 3, 2, 0x4001, 0xffc0050380000000ull, 0x0, false},
    {0x02, 3, 3, 0x4001, 0xffc0050380000000ull, 0x0, false},
    {0x03, 0, 0, 0x2000, 0x7fffffff00000000ull, 0x1, false},
    {0x03, 0, 1, 0x2000, 0x7fffffff00000000ull, 0x1, false},
    {0x03, 0, 2, 0x2000, 0x7fffffff00000000ull, 0x1, false},
    {0x03, 0, 3, 0x2000, 0x7fffffff00000000ull, 0x1, false},
    {0x03, 1, 0, 0x2000, 0x8000000000000000ull, 0x2, true},
    {0x03, 1, 1, 0x2000, 0x7fffff0000000000ull, 0x1, true},
    {0x03, 1, 2, 0x2000, 0x7fffff0000000000ull, 0x1, true},
    {0x03, 1, 3, 0x2000, 0x8000000000000000ull, 0x2, true},
    {0x03, 2, 0, 0x2000, 0x7fffffff00000000ull, 0x1, false},
    {0x03, 2, 1, 0x2000, 0x7fffffff00000000ull, 0x1, false},
    {0x03, 2, 2, 0x2000, 0x7fffffff00000000ull, 0x1, false},
    {0x03, 2, 3, 0x2000, 0x7fffffff00000000ull, 0x1, false},
    {0x03, 3, 0, 0x2000, 0x7fffffff00000000ull, 0x1, false},
    {0x03, 3, 1, 0x2000, 0x7fffffff00000000ull, 0x1, false},
    {0x03, 3, 2, 0x2000, 0x7fffffff00000000ull, 0x1, false},
    {0x03, 3, 3, 0x2000, 0x7fffffff00000000ull, 0x1, false},
    {0x04, 0, 0, 0x0000, 0xffffffffffffffffull, 0x0, false},
    {0x04, 0, 1, 0x0000, 0xffffffffffffffffull, 0x0, false},
    {0x04, 0, 2, 0x0000, 0xffffffffffffffffull, 0x0, false},
    {0x04, 0, 3, 0x0000, 0xffffffffffffffffull, 0x0, false},
    {0x04, 1, 0, 0x0001, 0x8000000000000000ull, 0x0, true},
    {0x04, 1, 1, 0x0000, 0xffffff0000000000ull, 0x0, true},
    {0x04, 1, 2, 0x0000, 0xffffff0000000000ull, 0x0, true},
    {0x04, 1, 3, 0x0001, 0x8000000000000000ull, 0x0, true},
    {0x04, 2, 0, 0x0001, 0x8000000000000000ull, 0x0, true},
    {0x04, 2, 1, 0x0000, 0xfffffffffffff800ull, 0x0, true},
    {0x04, 2, 2, 0x0000, 0xfffffffffffff800ull, 0x0, true},
    {0x04, 2, 3, 0x0001, 0x8000000000000000ull, 0x0, true},
    {0x04, 3, 0, 0x0001, 0x8000000000000000ull, 0x0, true},
    {0x04, 3, 1, 0x0000, 0xfffffffffffff800ull, 0x0, true},
    {0x04, 3, 2, 0x0000, 0xfffffffffffff800ull, 0x0, true},
    {0x04, 3, 3, 0x0001, 0x8000000000000000ull, 0x0, true},
    {0x05, 0, 0, 0x3c00, 0xfffffffffffff800ull, 0x0, false},
    {0x05, 0, 1, 0x3c00, 0xfffffffffffff800ull, 0x0, false},
    {0x05, 0, 2, 0x3c00, 0xfffffffffffff800ull, 0x0, false},
    {0x05, 0, 3, 0x3c00, 0xfffffffffffff800ull, 0x0, false},
    {0x05, 1, 0, 0x3c01, 0x8000000000000000ull, 0x0, true},
    {0x05, 1, 1, 0x3c00, 0xffffff0000000000ull, 0x0, true},
    {0x05, 1, 2, 0x3c00, 0xffffff0000000000ull, 0x0, true},
    {0x05, 1, 3, 0x3c01, 0x8000000000000000ull, 0x0, true},
    {0x05, 2, 0, 0x3c00, 0xfffffffffffff800ull, 0x0, false},
    {0x05, 2, 1, 0x3c00, 0xfffffffffffff800ull, 0x0, false},
    {0x05, 2, 2, 0x3c00, 0xfffffffffffff800ull, 0x0, false},
    {0x05, 2, 3, 0x3c00, 0xfffffffffffff800ull, 0x0, false},
    {0x05, 3, 0, 0x3c00, 0xfffffffffffff800ull, 0x0, false},
    {0x05, 3, 1, 0x3c00, 0xfffffffffffff800ull, 0x0, false},
    {0x05, 3, 2, 0x3c00, 0xfffffffffffff800ull, 0x0, false},
    {0x05, 3, 3, 0x3c00, 0xfffffffffffff800ull, 0x0, false},
    {0x06, 0, 0, 0x3f80, 0xffffff0000000000ull, 0x0, false},
    {0x06, 0, 1, 0x3f80, 0xffffff0000000000ull, 0x0, false},
    {0x06, 0, 2, 0x3f80, 0xffffff0000000000ull, 0x0, false},
    {0x06, 0, 3, 0x3f80, 0xffffff0000000000ull, 0x0, false},
    {0x06, 1, 0, 0x3f80, 0xffffff0000000000ull, 0x0, false},
    {0x06, 1, 1, 0x3f80, 0xffffff0000000000ull, 0x0, false},
    {0x06, 1, 2, 0x3f80, 0xffffff0000000000ull, 0x0, false},
    {0x06, 1, 3, 0x3f80, 0xffffff0000000000ull, 0x0, false},
    {0x06, 2, 0, 0x3f80, 0xffffff0000000000ull, 0x0, false},
    {0x06, 2, 1, 0x3f80, 0xffffff0000000000ull, 0x0, false},
    {0x06, 2, 2, 0x3f80, 0xffffff0000000000ull, 0x0, false},
    {0x06, 2, 3, 0x3f80, 0xffffff0000000000ull, 0x0, false},
    {0x06, 3, 0, 0x3f80, 0xffffff0000000000ull, 0x0, false},
    {0x06, 3, 1, 0x3f80, 0xffffff0000000000ull, 0x0, false},
    {0x06, 3, 2, 0x3f80, 0xffffff0000000000ull, 0x0, false},
    {0x06, 3, 3, 0x3f80, 0xffffff0000000000ull, 0x0, false},
    {0x07, 0, 0, 0x0001, 0xf65d8d9c00000000ull, 0x1, false},
    {0x07, 0, 1, 0x0001, 0xf65d8d9c00000000ull, 0x1, false},
    {0x07, 0, 2, 0x0001, 0xf65d8d9c00000000ull, 0x1, false},
    {0x07, 0, 3, 0x0001, 0xf65d8d9c00000000ull, 0x1, false},
    {0x07, 1, 0, 0x0001, 0xf65d8d8000000000ull, 0x1, true},
    {0x07, 1, 1, 0x0001, 0xf65d8d8000000000ull, 0x1, true},
    {0x07, 1, 2, 0x0001, 0xf65d8d8000000000ull, 0x1, true},
    {0x07, 1, 3, 0x0001, 0xf65d8e0000000000ull, 0x1, true},
    {0x07, 2, 0, 0x0001, 0xf65d8d9c00000000ull, 0x1, false},
    {0x07, 2, 1, 0x0001, 0xf65d8d9c00000000ull, 0x1, false},
    {0x07, 2, 2, 0x0001, 0xf65d8d9c00000000ull, 0x1, false},
    {0x07, 2, 3, 0x0001, 0xf65d8d9c00000000ull, 0x1, false},
    {0x07, 3, 0, 0x0001, 0xf65d8d9c00000000ull, 0x1, false},
    {0x07, 3, 1, 0x0001, 0xf65d8d9c00000000ull, 0x1, false},
    {0x07, 3, 2, 0x0001, 0xf65d8d9c00000000ull, 0x1, false},
    {0x07, 3, 3, 0x0001, 0xf65d8d9c00000000ull, 0x1, false},
    {0x08, 0, 0, 0x7fff, 0x001e000000000000ull, 0x1, false},
    {0x08, 0, 1, 0x7fff, 0x001e000000000000ull, 0x1, false},
    {0x08, 0, 2, 0x7fff, 0x001e000000000000ull, 0x1, false},
    {0x08, 0, 3, 0x7fff, 0x001e000000000000ull, 0x1, false},
    {0x08, 1, 0, 0x7fff, 0x001e000000000000ull, 0x1, false},
    {0x08, 1, 1, 0x7fff, 0x001e000000000000ull, 0x1, false},
    {0x08, 1, 2, 0x7fff, 0x001e000000000000ull, 0x1, false},
    {0x08, 1, 3, 0x7fff, 0x001e000000000000ull, 0x1, false},
    {0x08, 2, 0, 0x7fff, 0x001e000000000000ull, 0x1, false},
    {0x08, 2, 1, 0x7fff, 0x001e000000000000ull, 0x1, false},
    {0x08, 2, 2, 0x7fff, 0x001e000000000000ull, 0x1, false},
    {0x08, 2, 3, 0x7fff, 0x001e000000000000ull, 0x1, false},
    {0x08, 3, 0, 0x7fff, 0x001e000000000000ull, 0x1, false},
    {0x08, 3, 1, 0x7fff, 0x001e000000000000ull, 0x1, false},
    {0x08, 3, 2, 0x7fff, 0x001e000000000000ull, 0x1, false},
    {0x08, 3, 3, 0x7fff, 0x001e000000000000ull, 0x1, false},
    {0x09, 0, 0, 0x43f3, 0xe000000000000000ull, 0x0, false},
    {0x09, 0, 1, 0x43f3, 0xe000000000000000ull, 0x0, false},
    {0x09, 0, 2, 0x43f3, 0xe000000000000000ull, 0x0, false},
    {0x09, 0, 3, 0x43f3, 0xe000000000000000ull, 0x0, false},
    {0x09, 1, 0, 0x43f3, 0xe000000000000000ull, 0x0, false},
    {0x09, 1, 1, 0x43f3, 0xe000000000000000ull, 0x0, false},
    {0x09, 1, 2, 0x43f3, 0xe000000000000000ull, 0x0, false},
    {0x09, 1, 3, 0x43f3, 0xe000000000000000ull, 0x0, false},
    {0x09, 2, 0, 0x43f3, 0xe000000000000000ull, 0x0, false},
    {0x09, 2, 1, 0x43f3, 0xe000000000000000ull, 0x0, false},
    {0x09, 2, 2, 0x43f3, 0xe000000000000000ull, 0x0, false},
    {0x09, 2, 3, 0x43f3, 0xe000000000000000ull, 0x0, false},
    {0x09, 3, 0, 0x43f3, 0xe000000000000000ull, 0x0, false},
    {0x09, 3, 1, 0x43f3, 0xe000000000000000ull, 0x0, false},
    {0x09, 3, 2, 0x43f3, 0xe000000000000000ull, 0x0, false},
    {0x09, 3, 3, 0x43f3, 0xe000000000000000ull, 0x0, false},
    {0x0a, 0, 0, 0x4072, 0xc000000000000000ull, 0x0, false},
    {0x0a, 0, 1, 0x4072, 0xc000000000000000ull, 0x0, false},
    {0x0a, 0, 2, 0x4072, 0xc000000000000000ull, 0x0, false},
    {0x0a, 0, 3, 0x4072, 0xc000000000000000ull, 0x0, false},
    {0x0a, 1, 0, 0x4072, 0xc000000000000000ull, 0x0, false},
    {0x0a, 1, 1, 0x4072, 0xc000000000000000ull, 0x0, false},
    {0x0a, 1, 2, 0x4072, 0xc000000000000000ull, 0x0, false},
    {0x0a, 1, 3, 0x4072, 0xc000000000000000ull, 0x0, false},
    {0x0a, 2, 0, 0x4072, 0xc000000000000000ull, 0x0, false},
    {0x0a, 2, 1, 0x4072, 0xc000000000000000ull, 0x0, false},
    {0x0a, 2, 2, 0x4072, 0xc000000000000000ull, 0x0, false},
    {0x0a, 2, 3, 0x4072, 0xc000000000000000ull, 0x0, false},
    {0x0a, 3, 0, 0x4072, 0xc000000000000000ull, 0x0, false},
    {0x0a, 3, 1, 0x4072, 0xc000000000000000ull, 0x0, false},
    {0x0a, 3, 2, 0x4072, 0xc000000000000000ull, 0x0, false},
    {0x0a, 3, 3, 0x4072, 0xc000000000000000ull, 0x0, false},
    {0x10, 0, 0, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x10, 0, 1, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x10, 0, 2, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x10, 0, 3, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x10, 1, 0, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x10, 1, 1, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x10, 1, 2, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x10, 1, 3, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x10, 2, 0, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x10, 2, 1, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x10, 2, 2, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x10, 2, 3, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x10, 3, 0, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x10, 3, 1, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x10, 3, 2, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x10, 3, 3, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x2f, 0, 0, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x2f, 0, 1, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x2f, 0, 2, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x2f, 0, 3, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x2f, 1, 0, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x2f, 1, 1, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x2f, 1, 2, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x2f, 1, 3, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x2f, 2, 0, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x2f, 2, 1, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x2f, 2, 2, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x2f, 2, 3, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x2f, 3, 0, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x2f, 3, 1, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x2f, 3, 2, 0x0000, 0x0000000000000000ull, 0x4, false},
    {0x2f, 3, 3, 0x0000, 0x0000000000000000ull, 0x4, false},
};

void testFmovecrUndefined(bool fast) {
    for (const FmovecrCase& t : kFmovecrCases) {
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, (uint16_t)(0x5c00 | t.off), 0x4e71}); // FMOVECR.X #off,FP0
        f->fpcr_ = (uint32_t)(t.prec << 6 | t.mode << 4);
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, 0x1004);
        CHECK_EQ(f->fp_[0].se, t.se);
        CHECK_EQ(f->fp_[0].m, t.m);
        CHECK_EQ(f->fpsr_ >> 24, t.cc);
        CHECK_EQ(f->fpsr_ & 0xff00u, t.inex ? (uint32_t)fp::EXC_INEX2 : 0u);
    }
    // FMOVE.X FP0,(A0) で書き出した値は、fputest.x の FMOVECR の期待値 (実機の 68882 の値) と同じ。
    // FMOVEM.X FP0,(A1) はレジスタの値をそのまま書く ($08 の bit62 が 0 なのは実機では確かめていない。fpu_math.cpp の romEntry())
    struct OutCase {
        uint8_t off;
        uint32_t fpcr;
        uint16_t se;
        uint64_t m, movem;
    };
    const OutCase outs[] = {
        {0x03, 0x00, 0x2000, 0x7fffffff00000000ull, 0x7fffffff00000000ull}, // 正規化しない
        {0x03, 0x50, 0x2000, 0x7fffff0000000000ull, 0x7fffff0000000000ull}, // 単精度、RZ
        {0x09, 0x00, 0x43f3, 0xe000000000000000ull, 0xe000000000000000ull},
        {0x0a, 0x00, 0x4072, 0xc000000000000000ull, 0xc000000000000000ull},
        {0x10, 0x00, 0x0000, 0, 0},
        {0x08, 0x00, 0x7fff, 0x401e000000000000ull, 0x001e000000000000ull}, // FMOVE.X は bit62 を立てて書く
    };
    for (const OutCase& t : outs) {
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        // FMOVECR.X #off,FP0、FMOVE.X FP0,(A0)、FMOVEM.X FP0,(A1)
        e.bus.put(0x1000, {0xf200, (uint16_t)(0x5c00 | t.off), 0xf210, 0x6800, 0xf211, 0xf080, 0x4e71});
        e.cpu.a_[0] = 0x3000;
        e.cpu.a_[1] = 0x3100;
        f->fpcr_ = t.fpcr;
        e.steps(3);
        CHECK_EQ(e.cpu.pc_, 0x100c);
        CHECK_EQ(e.bus.r16(0x3000), t.se);
        CHECK_EQ(((uint64_t)e.bus.r32(0x3004) << 32) | e.bus.r32(0x3008), t.m);
        CHECK_EQ(e.bus.r16(0x3100), t.se);
        CHECK_EQ(((uint64_t)e.bus.r32(0x3104) << 32) | e.bus.r32(0x3108), t.movem);
        // $08 はシグナリング NaN なので、FMOVE.X で書き出すと SNAN と AIOP が立つ
        const uint32_t snan = t.off == 0x08 ? (uint32_t)(fp::EXC_SNAN | 0x80) : 0u;
        CHECK_EQ(f->fpsr_ & (uint32_t)(fp::EXC_SNAN | 0x80), snan);
    }
    { // $08 のあと SNAN の例外を許可して FMOVE.X で書き出すと、ベクタ 54 (MC68881UM 6.1.2)
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x5c08, 0xf210, 0x6800, 0x4e71});
        e.cpu.a_[0] = 0x3000;
        f->fpcr_ = fp::EXC_SNAN;
        e.steps(2);
        CHECK_EQ(e.cpu.pc_, handler(54));
    }
}

// FSCALE: 行き先 × 2^int(元の値) (MC68881UM の FSCALE)
void testFscale(bool fast) {
    const fp::X kThree = {0x4000, 0xc000000000000000ull};
    { // 3 × 2^2 = 12
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0426, 0x4e71}); // FSCALE.X FP1,FP0
        f->fp_[0] = kThree;
        f->fp_[1] = kTwo;
        e.steps(1);
        CHECK_EQ(same(f->fp_[0], fp::X{0x4002, 0xc000000000000000ull}), true);
        CHECK_EQ(f->fpsr_ & 0xff00ff00u, 0u);
    }
    { // 2^16383 × 2^2 はオーバーフロー。RN なら無限大
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0426, 0x4e71});
        f->fp_[0] = kHuge;
        f->fp_[1] = kTwo;
        e.steps(1);
        CHECK_EQ(same(f->fp_[0], kInf), true);
        CHECK_EQ((f->fpsr_ & fp::EXC_OVFL) != 0, true);
        CHECK_EQ(f->fpsr_ & 0xff000000u, (uint32_t)Fpu::CC_I);
    }
    { // |元の値| >= 2^14 なら必ずオーバーフローかアンダーフロー: 1 × 2^16384 と 1 × 2^-16384
        const fp::X k2p14 = {0x400d, 0x8000000000000000ull};
        for (int neg = 0; neg < 2; neg++) {
            Env e(fast);
            Fpu* f = e.cpu.fpu();
            e.bus.put(0x1000, {0xf200, 0x0426, 0x4e71});
            f->fp_[0] = kOne;
            f->fp_[1] = fp::X{(uint16_t)(k2p14.se | (neg ? 0x8000 : 0)), k2p14.m};
            e.steps(1);
            if (!neg) {
                CHECK_EQ(same(f->fp_[0], kInf), true);
                CHECK_EQ((f->fpsr_ & fp::EXC_OVFL) != 0, true);
                CHECK_EQ(f->fpsr_ & 0xff000000u, (uint32_t)Fpu::CC_I);
            } else {
                CHECK_EQ(same(f->fp_[0], kZero), true);
                CHECK_EQ((f->fpsr_ & fp::EXC_UNFL) != 0, true);
                CHECK_EQ(f->fpsr_ & 0xff000000u, (uint32_t)Fpu::CC_Z);
            }
        }
    }
}

// FCMP: 行き先 - 元の値 で条件コードだけを決める。行き先は変えない (MC68881UM の FCMP)
void testFcmp(bool fast) {
    const fp::X kNegZero = {0x8000, 0};
    struct Case {
        fp::X dst, src;
        uint32_t cc;
    } cases[] = {
        {kOne, kTwo, Fpu::CC_N},        // 小さい
        {kTwo, kTwo, Fpu::CC_Z},        // 等しい
        {kTwo, kOne, 0},                // 大きい
        {kZero, kNegZero, Fpu::CC_Z},   // +0 と -0 は等しい
        {kNegZero, kZero, Fpu::CC_N | Fpu::CC_Z},
        {kInf, kInf, Fpu::CC_Z},        // I は立てない
    };
    for (const Case& t : cases) {
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0438, 0x4e71}); // FCMP.X FP1,FP0
        f->fp_[0] = t.dst;
        f->fp_[1] = t.src;
        e.steps(1);
        CHECK_EQ(f->fpsr_ & 0xff000000u, t.cc);
        CHECK_EQ(same(f->fp_[0], t.dst), true);
    }
}

} // namespace

// 再レビューで見つかった点: 例外で結果を捨てるときの条件コード、FSAVE での保留の保存、FMOVE.P のシグナリング NaN
void testReview2(bool fast) {
    { // DZ を許可して 1 / 0: 条件コードは命令の前のまま (N+Z) (6.1.6)
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0420, 0x4e71, 0xf280, 0x0000}); // FDIV.X FP1,FP0
        f->fp_[0] = kOne;
        f->fp_[1] = kZero;
        f->fpcr_ = fp::EXC_DZ;
        f->fpsr_ = Fpu::CC_N | Fpu::CC_Z;
        e.steps(2); // 演算と NOP。例外は保留されたまま
        CHECK_EQ(e.cpu.pc_, 0x1006);
        e.steps(1); // 次の FPU の命令 (FNOP) の前に起こす
        CHECK_EQ(e.cpu.pc_, handler(50));
        CHECK_EQ(f->fpsr_ & 0xff000000u, Fpu::CC_N | Fpu::CC_Z);
        CHECK_EQ((f->fpsr_ & fp::EXC_DZ) != 0, true);
    }
    { // 保留のある IDLE のフレームを FRESTORE → FSAVE: 保留はフレームに移り (bit27 が 0)、FPU からは消える。
      // もう一度 FRESTORE すると、次の FPU の命令の前に例外になる (6.4.2.2)
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        // FRESTORE (A0); FSAVE -(A7); FNOP; FRESTORE (A7)+; FNOP
        e.bus.put(0x1000, {0xf350, 0xf327, 0xf280, 0x0000, 0xf35f, 0xf280, 0x0000});
        e.cpu.a_[0] = 0x3000;
        e.bus.w32(0x3000, 0x1f380000);
        e.bus.w32(0x3000 + 8, 0x12345678); // 例外の情報 (そのまま書き戻されること)
        e.bus.w32(0x3000 + 56, 0x7400ffff);
        f->fpsr_ = fp::EXC_INEX2;
        f->fpcr_ = fp::EXC_INEX2;
        e.steps(2);
        uint32_t sp = e.cpu.a_[7];
        CHECK_EQ(sp, 0x9000 - 60);
        CHECK_EQ(e.bus.r32(sp), 0x1f380000);
        CHECK_EQ(e.bus.r32(sp + 8), 0x12345678);
        CHECK_EQ(e.bus.r32(sp + 56), 0x7400ffff);
        e.steps(1); // FSAVE のあとの FNOP では例外にならない
        CHECK_EQ(e.cpu.pc_, 0x1008);
        e.steps(2);
        CHECK_EQ(e.cpu.pc_, handler(49));
        CHECK_EQ(e.bus.r32(e.cpu.a_[7] + 2), 0x100a);
    }
    // FMOVE.P FP0,(A0){#0} で NaN: 符号と仮数はそのまま書く。シグナリング NaN は SNAN を立て、bit62 を立てて書く。
    // 許可なら書いてから命令の途中の例外 (6.1.2)
    struct PackedNan {
        fp::X v;
        uint32_t fpcr;
        uint32_t w0, w1, w2;
        bool snan;
    };
    const PackedNan cases[] = {
        {{0x7fff, 0x2000000000000001ull}, 0, 0x7fff0000, 0x60000000, 0x00000001, true},
        {{0x7fff, 0x2000000000000001ull}, fp::EXC_SNAN, 0x7fff0000, 0x60000000, 0x00000001, true},
        {{0xffff, 0x2000000000000001ull}, 0, 0xffff0000, 0x60000000, 0x00000001, true},
        {{0x7fff, 0x4000000012345678ull}, 0, 0x7fff0000, 0x40000000, 0x12345678, false}, // quiet NaN はそのまま
        {{0xffff, 0xc0000000abcdef01ull}, 0, 0xffff0000, 0xc0000000, 0xabcdef01, false},
    };
    for (const PackedNan& t : cases) {
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf210, 0x6c00, 0x4e71});
        e.cpu.a_[0] = 0x3000;
        f->fp_[0] = t.v;
        f->fpcr_ = t.fpcr;
        e.steps(1);
        CHECK_EQ((f->fpsr_ & fp::EXC_SNAN) != 0, t.snan);
        CHECK_EQ(e.bus.r32(0x3000), t.w0);
        CHECK_EQ(e.bus.r32(0x3004), t.w1);
        CHECK_EQ(e.bus.r32(0x3008), t.w2);
        if (t.fpcr) {
            CHECK_EQ(e.cpu.pc_, handler(54));
            CHECK_EQ(e.bus.r16(e.cpu.a_[7] + 6), 0x9000 | 54 * 4);
        } else {
            CHECK_EQ(e.cpu.pc_, 0x1004);
        }
    }
    // SNAN と OPERR を許可して結果を捨てるときも、条件コードは命令の前のまま
    { // SNAN: FADD.X FP1,FP0 で FP1 がシグナリング NaN
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0422, 0x4e71, 0xf280, 0x0000});
        f->fp_[0] = kOne;
        f->fp_[1] = kSnan;
        f->fpcr_ = fp::EXC_SNAN;
        f->fpsr_ = Fpu::CC_N | Fpu::CC_I;
        e.steps(2); // 演算と NOP。例外は保留されたまま
        CHECK_EQ(e.cpu.pc_, 0x1006);
        e.steps(1); // 次の FPU の命令 (FNOP) の前に起こす
        CHECK_EQ(e.cpu.pc_, handler(54));
        CHECK_EQ(f->fpsr_ & 0xff000000u, Fpu::CC_N | Fpu::CC_I);
    }
    { // OPERR: 0 / 0
        Env e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0420, 0x4e71, 0xf280, 0x0000});
        f->fp_[0] = kZero;
        f->fp_[1] = kZero;
        f->fpcr_ = fp::EXC_OPERR;
        f->fpsr_ = Fpu::CC_Z;
        e.steps(2); // 演算と NOP。例外は保留されたまま
        CHECK_EQ(e.cpu.pc_, 0x1006);
        e.steps(1); // 次の FPU の命令 (FNOP) の前に起こす
        CHECK_EQ(e.cpu.pc_, handler(52));
        CHECK_EQ(f->fpsr_ & 0xff000000u, Fpu::CC_Z);
    }
}

int main() {
    for (int fast = 0; fast < 2; fast++) {
        testNoWriteWhenEnabled(fast);
        testPendingTiming(fast);
        testFmoveOutMidInstruction(fast);
        testSignalingNan(fast);
        testSaveRestore(fast);
        testFmovecr(fast);
        testFmovecrUndefined(fast);
        testFscale(fast);
        testFcmp(fast);
        testReview2(fast);
    }
    return finish();
}
