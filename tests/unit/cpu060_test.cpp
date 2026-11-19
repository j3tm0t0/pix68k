// 68060 (Cpu020 の 68060 と Fpu の 68060 の部分) の単体テスト。060SP / 060FPSP がないとき (ASSIST_OFF) に例外へ
// 行く経路を、060turbo.sys なしで確かめる: 未実装整数命令 (ベクタ 61)、FPU の未実装の命令 (11) と実効アドレス (60)、
// 未実装のデータ型 (55)、アンダーフロー / オーバーフロー (51 / 53) と EXCP の状態、FSAVE / FRESTORE、
// フォーマット $3 / $4 のフレーム、MMU の表引き、アクセスエラーでのアドレスレジスタの復元。根拠は MC68060UM
#include <string.h>
#include <initializer_list>

#include "cpu020_env.h"
#include "m68k/fpu.h"
#include "test_check.h"

using namespace cpu020test;
using m68k::Fpu;
namespace fp = m68k::fp;

namespace {

const fp::X kZero = {0x0000, 0};
const fp::X kOne = {0x3fff, 0x8000000000000000ull};
const fp::X kHuge = {0x7ffe, 0x8000000000000000ull}; // 2^16383
const fp::X kTiny = {0x0001, 0x8000000000000000ull}; // 2^-16382
const fp::X kDenorm = {0x0000, 0x4000000000000000ull};
const fp::X kSnan = {0x7fff, 0x2000000000000000ull};

struct Env060 : Env {
    explicit Env060(bool fast) : Env(fast, m68k::CpuModel::M68060) {}
    uint32_t sp() const { return cpu.a_[7]; }
    uint16_t fv() const { return bus.r16(sp() + 6); }     // フォーマットとベクタオフセット
    uint32_t pcAt() const { return bus.r32(sp() + 2); }   // 積んだ PC
    uint32_t ea() const { return bus.r32(sp() + 8); }     // フォーマット $2 / $3 / $4 の +$08
    uint32_t fslw() const { return bus.r32(sp() + 12); }  // フォーマット $4 の +$0C
};

uint16_t fv(int format, int vector) { return (uint16_t)((format << 12) | (vector * 4)); }
bool same(const fp::X& a, const fp::X& b) { return a.se == b.se && a.m == b.m; }

// 未実装整数命令はベクタ 61、フォーマット $0、PC はこの命令 (MC68060UM 8.2.4)
void testUnimplementedInteger(bool fast) {
    {
        Env060 e(fast);
        e.bus.put(0x1000, {0x0108, 0x0000}); // MOVEP.W (0,A0),D0
        e.cpu.a_[0] = 0x3000;
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, handler(61));
        CHECK_EQ(e.fv(), fv(0, 61));
        CHECK_EQ(e.pcAt(), 0x1000);
    }
    {
        Env060 e(fast);
        e.bus.put(0x1000, {0x4c01, 0x3402}); // MULU.L D1,D2:D3 (64 ビット)
        e.steps(1);
        CHECK_EQ(e.fv(), fv(0, 61));
    }
    {
        Env060 e(fast);
        e.bus.put(0x1000, {0x4c01, 0x3003, 0x4e71}); // MULU.L D1,D3 (32 ビットはある)
        e.cpu.d_[1] = 3;
        e.cpu.d_[3] = 5;
        e.steps(1);
        CHECK_EQ(e.cpu.d_[3], 15);
        CHECK_EQ(e.cpu.pc_, 0x1004);
    }
    // 060SP を肩代わりする CMP2 / CHK2 は、V = 0、N = (Rn < 0) にし、X は残す (68060 の実機の値はなく、#36 で決めた値)。
    // Dn の .b / .w は、その大きさで符号拡張した値の符号
    struct Chk2Case {
        uint16_t op, w;   // 命令と拡張ワード
        uint16_t bounds[4];
        uint32_t d1, a1;
        uint16_t ccr;     // 命令の前の CCR
        uint16_t expect;  // 命令の後の CCR
    };
    const Chk2Case cases[] = {
        // CMP2.L (A0),D1 で -2..0、D1 = $7FFFFFFE: 範囲外 (C=1)、正なので N=0 (68030 なら N=1 V=1 C=1)
        {0x04d0, 0x1000, {0xffff, 0xfffe, 0x0000, 0x0000}, 0x7ffffffe, 0, 0x00, 0x01},
        {0x04d0, 0x1000, {0xffff, 0xfffe, 0x0000, 0x0000}, 0x7ffffffe, 0, 0x1a, 0x11},
        // D1 = -1: 範囲内、負なので N=1
        {0x04d0, 0x1000, {0xffff, 0xfffe, 0x0000, 0x0000}, 0xffffffff, 0, 0x02, 0x08},
        // CMP2.B (A0),D1 で 0..$10、D1 = $00000080: .b では -128 なので N=1。範囲外 (C=1)
        {0x00d0, 0x1000, {0x0010, 0x0000, 0x0000, 0x0000}, 0x00000080, 0, 0x00, 0x09},
        // CMP2.B (A0),D1 で 0..$10、D1 = $FFFFFF10: .b では $10 で上限と同じ (Z=1)、N=0
        {0x00d0, 0x1000, {0x0010, 0x0000, 0x0000, 0x0000}, 0xffffff10, 0, 0x0a, 0x04},
        // CMP2.W (A0),A1 で 0..$10、A1 = $00008000: An は 32 ビットのまま比べるので正 (N=0)、範囲外 (C=1)
        {0x02d0, 0x9000, {0x0000, 0x0010, 0x0000, 0x0000}, 0, 0x00008000, 0x08, 0x01},
        // CMP2.W (A0),A1 で $FFF0..$10 (-16..16)、A1 = $FFFFFFF8: 負 (N=1)、範囲内
        {0x02d0, 0x9000, {0xfff0, 0x0010, 0x0000, 0x0000}, 0, 0xfffffff8, 0x00, 0x08},
    };
    for (const Chk2Case& t : cases) {
        Env060 e(fast);
        e.cpu.setUnimplementedAssist(m68k::Cpu020::ASSIST_ALWAYS);
        e.bus.put(0x1000, {t.op, t.w, 0x4e71});
        e.bus.put(0x3000, {t.bounds[0], t.bounds[1], t.bounds[2], t.bounds[3]});
        e.cpu.a_[0] = 0x3000;
        e.cpu.d_[1] = t.d1;
        e.cpu.a_[1] = t.a1;
        e.cpu.setSR((uint16_t)(0x2700 | t.ccr));
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, 0x1004);
        CHECK_EQ(e.cpu.getSR() & 0x1f, t.expect);
    }
    { // CHK2.L (A0),D1 で -10..10、D1 = -1: 範囲内なので例外にならず、N=1、V=0 (前の V は消える)、X は残す
        Env060 e(fast);
        e.cpu.setUnimplementedAssist(m68k::Cpu020::ASSIST_ALWAYS);
        e.bus.put(0x1000, {0x04d0, 0x1800, 0x4e71});
        e.bus.put(0x3000, {0xffff, 0xfff6, 0x0000, 0x000a});
        e.cpu.a_[0] = 0x3000;
        e.cpu.d_[1] = 0xffffffff;
        e.cpu.setSR(0x2712);
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, 0x1004);
        CHECK_EQ(e.cpu.getSR() & 0x1f, 0x18);
    }
    { // CHK2.L (A0),D1 で範囲外ならベクタ 6。N と V は CMP2 と同じ
        Env060 e(fast);
        e.cpu.setUnimplementedAssist(m68k::Cpu020::ASSIST_ALWAYS);
        e.bus.put(0x1000, {0x04d0, 0x1800, 0x4e71});
        e.bus.put(0x3000, {0x0000, 0x0000, 0x0000, 0x0010});
        e.cpu.a_[0] = 0x3000;
        e.cpu.d_[1] = 0x80000000;
        e.cpu.setSR(0x2702);
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, handler(6));
        CHECK_EQ(e.bus.r16(e.cpu.a_[7]) & 0x1f, 0x09);
    }
}

// FPU の未実装の命令 (ベクタ 11、フォーマット $2、PC は次の命令、+$08 に実効アドレス)。(An)+ / -(An) は An を変えず、
// 060FPSP の _dcalc_ea が足し引きする前のアドレス ((An)+ は An、-(An) は An - 大きさ) を積む。
// 未実装の実効アドレス (ベクタ 60、フォーマット $0、PC はこの命令)
void testFpuUnimplemented(bool fast) {
    {
        Env060 e(fast);
        e.bus.put(0x1000, {0xf200, 0x000e, 0x4e71}); // FSIN.X FP0
        e.steps(1);
        CHECK_EQ(e.fv(), fv(2, 11));
        CHECK_EQ(e.pcAt(), 0x1004);
    }
    {
        Env060 e(fast);
        e.bus.put(0x1000, {0xf218, 0x540e, 0x4e71}); // FSIN.D (A0)+,FP0
        e.cpu.a_[0] = 0x3000;
        e.steps(1);
        CHECK_EQ(e.fv(), fv(2, 11));
        CHECK_EQ(e.ea(), 0x3000);
        CHECK_EQ(e.cpu.a_[0], 0x3000);
    }
    {
        Env060 e(fast);
        e.bus.put(0x1000, {0xf220, 0x540e, 0x4e71}); // FSIN.D -(A0),FP0
        e.cpu.a_[0] = 0x3010;
        e.steps(1);
        CHECK_EQ(e.ea(), 0x3008);
        CHECK_EQ(e.cpu.a_[0], 0x3010);
    }
    { // 68060 に FMOVECR はない (MC68060UM 表 6-11)。060FPSP がなければベクタ 11
        Env060 e(fast);
        e.bus.put(0x1000, {0xf200, 0x5c08, 0x4e71}); // FMOVECR.X #$08,FP0
        e.steps(1);
        CHECK_EQ(e.fv(), fv(2, 11));
        CHECK_EQ(e.pcAt(), 0x1004);
    }
    { // 060FPSP を肩代わりするときは 68882 と同じ値 (マニュアルにない $08 は bit62 が 0 のシグナリング NaN。実機では確かめていない)
        Env060 e(fast);
        e.cpu.setUnimplementedAssist(m68k::Cpu020::ASSIST_ALWAYS);
        e.bus.put(0x1000, {0xf200, 0x5c08, 0x4e71});
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, 0x1004);
        CHECK_EQ(same(e.cpu.fpu()->fp_[0], fp::X{0x7fff, 0x001e000000000000ull}), true);
        CHECK_EQ(e.cpu.fpu()->fpsr_ >> 24, (uint32_t)Fpu::CC_NAN >> 24);
    }
    {
        Env060 e(fast);
        e.bus.put(0x1000, {0xf23c, 0x4800, 0x3fff, 0x0000, 0x8000, 0x0000, 0x0000, 0x0000}); // FMOVE.X #1,FP0
        e.steps(1);
        CHECK_EQ(e.fv(), fv(0, 60));
        CHECK_EQ(e.pcAt(), 0x1000);
    }
}

// 非正規化数のオペランドは未実装のデータ型 (ベクタ 55)。FPn に書く命令は命令の前の例外 (フォーマット $0、
// PC は次の命令)、FMOVE.X OUT は命令の後の例外 (フォーマット $3) で、(An)+ / -(An) では An を変えず、
// 060FPSP の _calc_ea_fout が直す前のアドレス ((An)+ は An、-(An) は An - 4) を積む (MC68060UM 6.5.2)
void testUnsupportedData(bool fast) {
    {
        Env060 e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0422, 0x4e71}); // FADD.X FP1,FP0
        f->fp_[0] = kOne;
        f->fp_[1] = kDenorm;
        e.steps(1);
        CHECK_EQ(e.fv(), fv(0, 55));
        CHECK_EQ(e.pcAt(), 0x1004);
        CHECK_EQ(same(f->fp_[0], kOne), true);
    }
    for (int pre = 0; pre < 2; pre++) {
        Env060 e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {(uint16_t)(pre ? 0xf220 : 0xf218), 0x6800, 0x4e71}); // FMOVE.X FP0,-(A0) / (A0)+
        f->fp_[0] = kDenorm;
        e.cpu.a_[0] = 0x3000;
        e.steps(1);
        CHECK_EQ(e.fv(), fv(3, 55));
        CHECK_EQ(e.pcAt(), 0x1004);
        CHECK_EQ(e.cpu.a_[0], 0x3000);
        CHECK_EQ(e.ea(), pre ? 0x2ffc : 0x3000);
    }
    { // FMOVE.P FP0,-(A0){#0}: パック 10 進はフォーマット $2。An は変えない
        Env060 e(fast);
        e.bus.put(0x1000, {0xf220, 0x6c00, 0x4e71});
        e.cpu.fpu()->fp_[0] = kOne;
        e.cpu.a_[0] = 0x3000;
        e.steps(1);
        CHECK_EQ(e.fv(), fv(2, 55));
        CHECK_EQ(e.cpu.a_[0], 0x3000);
        CHECK_EQ(e.ea(), 0x2ffc);
    }
}

// アンダーフロー / オーバーフロー (ベクタ 51 / 53) は許可しなくても例外になる。FPn に書く命令では、レジスタと
// 条件コードを変えずに EXCP の状態になり、整数の命令を挟んでも、次の FPU の命令を始めるときに命令の前の例外
// (フォーマット $0、PC はその命令) になる。FSAVE はその例外を起こさず、12 バイトの EXCP のフレームを書いて
// IDLE に戻る。FRESTORE で EXCP に戻すと、次の FPU の命令で例外になる (MC68060UM 6.6.4、6.6.5、6.7)
void testPendingExceptions(bool fast) {
    { // UNFL: FMUL → MOVEQ → FNOP
        Env060 e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0023, 0x7001, 0xf280, 0x0000}); // FMUL.X FP0,FP0; MOVEQ #1,D0; FNOP
        f->fp_[0] = kTiny;
        f->fpsr_ = Fpu::CC_Z;
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, 0x1004);
        CHECK_EQ(same(f->fp_[0], kTiny), true);
        CHECK_EQ(f->fpsr_ & 0xff000000u, Fpu::CC_Z);
        CHECK_EQ(f->excp060_, 0x80 | 3);
        e.steps(1);
        CHECK_EQ(e.cpu.d_[0], 1);
        CHECK_EQ(e.cpu.pc_, 0x1006);
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, handler(51));
        CHECK_EQ(e.fv(), fv(0, 51));
        CHECK_EQ(e.pcAt(), 0x1006);
        CHECK_EQ(f->fpiar_, 0x1000);
    }
    { // OVFL: FMUL → MOVEQ → FSAVE -(A0) → FRESTORE (A0)+ → FNOP
        Env060 e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0023, 0x7001, 0xf320, 0xf358, 0xf280, 0x0000});
        f->fp_[0] = kHuge;
        e.cpu.a_[0] = 0x3000;
        e.steps(3);
        CHECK_EQ(e.cpu.pc_, 0x1008); // FSAVE は例外を起こさない
        CHECK_EQ(e.cpu.a_[0], 0x3000 - 12);
        CHECK_EQ(e.bus.r32(0x2ff4), 0x7ffe0000u | 0xe000 | 5); // 例外のオペランドの指数、EXCP、OVFL
        CHECK_EQ(e.bus.r32(0x2ff8), 0x80000000u);
        CHECK_EQ(e.bus.r32(0x2ffc), 0);
        CHECK_EQ(f->excp060_, 0);
        CHECK_EQ(same(f->fp_[0], kHuge), true);
        e.steps(1); // FRESTORE
        CHECK_EQ(e.cpu.a_[0], 0x3000);
        CHECK_EQ(f->excp060_, 0x80 | 5);
        e.steps(1);
        CHECK_EQ(e.fv(), fv(0, 53));
        CHECK_EQ(e.pcAt(), 0x100a);
    }
    { // 丸めてはじめてあふれるオーバーフローは例外にしない
        Env060 e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0422, 0x4e71}); // FADD.X FP1,FP0
        f->fp_[0] = {0x7ffe, 0xffffffffffffffffull};
        f->fp_[1] = {0x7fbe, 0x8000000000000000ull}; // 最大値の最下位ビットの半分
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, 0x1004);
        CHECK_EQ(f->excp060_, 0);
    }
}

// 許可した SNAN / OPERR / DZ / INEX も、FPn に書く命令では EXCP の状態にして次の FPU の命令で起こす。SNAN / OPERR /
// DZ はデスティネーションを変えない。FMOVE OUT はすぐに命令の後の例外 (フォーマット $3) で、拡張精度の (An)+ /
// -(An) は書いたうえで An を変えず、_calc_ea_fout の前のアドレスを積む (MC68060UM 6.6.2.2、6.6.3.2、6.6.6.2、6.6.7.2)
void testEnabledExceptions(bool fast) {
    { // DZ: FDIV → MOVEQ → FSAVE。例外は起きず、フレームの種類は DZ (2)
        Env060 e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0420, 0x7001, 0xf320}); // FDIV.X FP1,FP0; MOVEQ #1,D0; FSAVE -(A0)
        f->fp_[0] = kOne;
        f->fp_[1] = kZero;
        f->fpcr_ = fp::EXC_DZ;
        e.cpu.a_[0] = 0x3000;
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, 0x1004);
        CHECK_EQ(same(f->fp_[0], kOne), true);
        CHECK_EQ((f->fpsr_ & fp::EXC_DZ) != 0, true);
        e.steps(2);
        CHECK_EQ(e.cpu.pc_, 0x1008);
        CHECK_EQ(e.count(50), 0);
        CHECK_EQ(e.bus.r32(0x2ff4) & 0xffff, 0xe000 | 2);
    }
    { // DZ: FDIV → MOVEQ → FNOP でベクタ 50
        Env060 e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0420, 0x7001, 0xf280, 0x0000});
        f->fp_[0] = kOne;
        f->fp_[1] = kZero;
        f->fpcr_ = fp::EXC_DZ;
        e.steps(3);
        CHECK_EQ(e.cpu.pc_, handler(50));
        CHECK_EQ(e.fv(), fv(0, 50));
        CHECK_EQ(e.pcAt(), 0x1006);
    }
    { // OPERR (0 / 0): 次の FPU の命令 (FMOVE.L FPCR,D1) でベクタ 52
        Env060 e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0420, 0xf201, 0xb000});
        f->fp_[0] = kZero;
        f->fp_[1] = kZero;
        f->fpcr_ = fp::EXC_OPERR;
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, 0x1004);
        CHECK_EQ(same(f->fp_[0], kZero), true);
        e.steps(1);
        CHECK_EQ(e.fv(), fv(0, 52));
        CHECK_EQ(e.pcAt(), 0x1004);
    }
    { // INEX: 結果は書く
        Env060 e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf200, 0x0420, 0x4e71}); // FDIV.X FP1,FP0 (1 / 3)
        f->fp_[0] = kOne;
        f->fp_[1] = {0x4000, 0xc000000000000000ull};
        f->fpcr_ = fp::EXC_INEX2;
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, 0x1004);
        CHECK_EQ(f->fp_[0].se, 0x3ffd);
        CHECK_EQ(f->excp060_, 0x80 | 1);
    }
    for (int pre = 0; pre < 2; pre++) { // SNAN: FMOVE.X FP0,-(A0) / (A0)+
        Env060 e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {(uint16_t)(pre ? 0xf220 : 0xf218), 0x6800, 0x4e71});
        f->fp_[0] = kSnan;
        f->fpcr_ = fp::EXC_SNAN;
        e.cpu.a_[0] = 0x3010;
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, handler(54));
        CHECK_EQ(e.fv(), fv(3, 54));
        CHECK_EQ(e.pcAt(), 0x1004);
        CHECK_EQ(e.cpu.a_[0], 0x3010);
        CHECK_EQ(e.ea(), pre ? 0x300c : 0x3010);
        CHECK_EQ(e.bus.r16(pre ? 0x3004 : 0x3010), 0x7fff); // 書いてある
    }
    { // 060SP を肩代わりするとき (ASSIST_ALWAYS) は、060FPSP が直したあとと同じく An を動かし、書いたアドレスを積む
        Env060 e(fast);
        Fpu* f = e.cpu.fpu();
        e.cpu.setUnimplementedAssist(m68k::Cpu020::ASSIST_ALWAYS);
        e.bus.put(0x1000, {0xf218, 0x6800, 0x4e71}); // FMOVE.X FP0,(A0)+
        f->fp_[0] = kSnan;
        f->fpcr_ = fp::EXC_SNAN;
        e.cpu.a_[0] = 0x3010;
        e.steps(1);
        CHECK_EQ(e.fv(), fv(3, 54));
        CHECK_EQ(e.cpu.a_[0], 0x301c);
        CHECK_EQ(e.ea(), 0x3010);
    }
    { // OPERR: FMOVE.L FP0,(A0)+ (無限大)。単精度などは An を動かし、書いたアドレスを積む
        Env060 e(fast);
        Fpu* f = e.cpu.fpu();
        e.bus.put(0x1000, {0xf218, 0x6000, 0x4e71});
        f->fp_[0] = {0x7fff, 0};
        f->fpcr_ = fp::EXC_OPERR;
        e.cpu.a_[0] = 0x3000;
        e.steps(1);
        CHECK_EQ(e.fv(), fv(3, 52));
        CHECK_EQ(e.cpu.a_[0], 0x3004);
        CHECK_EQ(e.ea(), 0x3000);
        CHECK_EQ(e.bus.r32(0x3000), 0x7fffffff);
    }
}

// PCR の DFP が 1 なら、FPU の命令はベクタ 11 (フォーマット $4)。積む PC は次の命令、+$0C はこの命令
void testFpuDisabled(bool fast) {
    {
        Env060 e(fast);
        e.bus.put(0x1000, {0xf293, 0x0010, 0x4e71}); // FBNE.W
        e.cpu.pcr_ |= 2;
        e.steps(1);
        CHECK_EQ(e.fv(), fv(4, 11));
        CHECK_EQ(e.pcAt(), 0x1004);
        CHECK_EQ(e.fslw(), 0x1000);
    }
    {
        Env060 e(fast);
        e.bus.put(0x1000, {0xf218, 0x4400, 0x4e71}); // FMOVE.S (A0)+,FP0
        e.cpu.pcr_ |= 2;
        e.cpu.a_[0] = 0x3000;
        e.steps(1);
        CHECK_EQ(e.pcAt(), 0x1004);
        CHECK_EQ(e.ea(), 0x3000);
        CHECK_EQ(e.cpu.a_[0], 0x3000);
    }
    {
        Env060 e(fast);
        e.bus.put(0x1000, {0xf228, 0xa800, 0x0010, 0x4e71}); // FMOVEM.L FPSR,(16,A0)
        e.cpu.pcr_ |= 2;
        e.steps(1);
        CHECK_EQ(e.pcAt(), 0x1006);
    }
    // 命令の長さ: イミディエイト、フル拡張の実効アドレス、FTRAPcc.L、FSAVE / FRESTORE
    struct Len {
        uint16_t w[6];
        int n;
        uint32_t next;
    };
    static const Len lens[] = {
        {{0xf23c, 0x4422, 0x3f80, 0x0000}, 4, 0x1008},         // FADD.S #1,FP0
        {{0xf230, 0x4400, 0x0130, 0x0000, 0x0010}, 5, 0x100a}, // FMOVE.S (bd.L,A0,D0.W),FP0
        {{0xf27b, 0x000f, 0x1234, 0x5678}, 4, 0x1008},         // FTRAPT.L #imm
        {{0xf27a, 0x000f, 0x1234}, 3, 0x1006},                 // FTRAPT.W #imm
        {{0xf248, 0x000f, 0xfffe}, 3, 0x1006},                 // FDBT D0,*
        {{0xf268, 0x000f, 0x0010}, 3, 0x1006},                 // FST (16,A0)
        {{0xf328, 0x0010}, 2, 0x1004},                         // FSAVE (16,A0)
        {{0xf368, 0x0010}, 2, 0x1004},                         // FRESTORE (16,A0)
        {{0xf2d3, 0x0000, 0x0010}, 3, 0x1006},                 // FBNE.L
    };
    for (const Len& l : lens) {
        Env060 e(fast);
        for (int i = 0; i < l.n; i++) e.bus.w16(0x1000 + 2 * i, l.w[i]);
        e.cpu.pcr_ |= 2;
        e.cpu.a_[0] = 0x3000;
        e.steps(1);
        CHECK_EQ(e.fv(), fv(4, 11));
        CHECK_EQ(e.pcAt(), l.next);
        CHECK_EQ(e.cpu.a_[0], 0x3000);
    }
}

// FScc <ea> は 68060 にない (ベクタ 11、フォーマット $2)。(An)+ / -(An) は An を変えず、(An)+ なら An、-(An) なら
// An - 1 を積む (060FPSP はこのアドレスに書いてから An を足し引きする)。肩代わりするときは 68882 と同じに動く
void testFscc(bool fast) {
    for (int pre = 0; pre < 2; pre++) {
        Env060 e(fast);
        e.bus.put(0x1000, {(uint16_t)(pre ? 0xf260 : 0xf258), 0x000f, 0x4e71}); // FST -(A0) / (A0)+
        e.cpu.a_[0] = 0x3004;
        e.steps(1);
        CHECK_EQ(e.fv(), fv(2, 11));
        CHECK_EQ(e.pcAt(), 0x1004);
        CHECK_EQ(e.cpu.a_[0], 0x3004);
        CHECK_EQ(e.ea(), pre ? 0x3003 : 0x3004);
    }
    { // -(A7) のバイトは 2 バイト
        Env060 e(fast);
        e.bus.put(0x1000, {0xf267, 0x000f, 0x4e71}); // FST -(A7)
        e.steps(1);
        CHECK_EQ(e.ea(), 0x9000 - 2);
    }
    for (int pre = 0; pre < 2; pre++) {
        Env060 e(fast);
        e.cpu.setUnimplementedAssist(m68k::Cpu020::ASSIST_ALWAYS);
        e.bus.put(0x1000, {(uint16_t)(pre ? 0xf260 : 0xf258), 0x000f, 0x4e71});
        e.cpu.a_[0] = 0x3004;
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, 0x1004);
        CHECK_EQ(e.cpu.a_[0], pre ? 0x3003 : 0x3005);
        CHECK_EQ(e.bus.mem[pre ? 0x3003 : 0x3004], 0xff);
    }
}

// トレース中でも、FMOVE OUT のすぐ起こす FPU の例外 (グループ 3) のあとにトレースの例外は起こさない (8.2.6、表 8-3)
void testTraceFmoveOut(bool fast) {
    Env060 e(fast);
    Fpu* f = e.cpu.fpu();
    e.bus.put(0x1000, {0xf218, 0x6800, 0x4e71}); // FMOVE.X FP0,(A0)+
    f->fp_[0] = kSnan;
    f->fpcr_ = fp::EXC_SNAN;
    e.cpu.a_[0] = 0x3000;
    e.cpu.setSR(0xa700); // T1、S
    e.steps(1);
    CHECK_EQ(e.cpu.pc_, handler(54));
    CHECK_EQ(e.fv(), fv(3, 54));
}

// アクセスエラー (ベクタ 2、フォーマット $4) では、命令の途中で動かしたアドレスレジスタを命令の始めの値に戻す。
// $C00000-$CFFFFF はバスエラー
void testAccessFaultUndo(bool fast) {
    {
        Env060 e(fast);
        e.bus.put(0x1000, {0x2100}); // MOVE.L D0,-(A0)
        e.cpu.a_[0] = 0xc00004;
        e.steps(1);
        CHECK_EQ(e.fv(), fv(4, 2));
        CHECK_EQ(e.ea(), 0xc00000);
        CHECK_EQ(e.pcAt(), 0x1000);
        CHECK_EQ(e.cpu.a_[0], 0xc00004);
        CHECK_EQ(e.fslw() & (1u << 23), 1u << 23); // 書き込み
    }
    // A7 に積む命令 (ユーザモード。USP の下がバスエラー)
    static const uint16_t pushes[][2] = {
        {0x4850, 0x4e71}, // PEA (A0)
        {0x4e90, 0x4e71}, // JSR (A0)
        {0x4e56, 0xfffc}, // LINK A6,#-4
        {0x6102, 0x4e71}, // BSR.S
        {0x48e7, 0xc000}, // MOVEM.L D0-D1,-(SP)
        {0x2f00, 0x4e71}, // MOVE.L D0,-(SP)
    };
    for (const auto& code : pushes) {
        Env060 e(fast);
        e.bus.put(0x1000, {code[0], code[1]});
        e.cpu.usp_ = 0xc00004;
        e.cpu.setSR(0x0000);
        e.cpu.a_[0] = 0x2000;
        e.cpu.a_[6] = 0x2100;
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, handler(2));
        CHECK_EQ(e.pcAt(), 0x1000);
        CHECK_EQ(e.cpu.usp_, 0xc00004);
        CHECK_EQ(e.cpu.a_[6], 0x2100);
    }
    { // MOVE16 (A0)+,(A1)+: 書く先がバスエラー
        Env060 e(fast);
        e.bus.put(0x1000, {0xf620, 0x9000});
        e.cpu.a_[0] = 0x3000;
        e.cpu.a_[1] = 0xc00000;
        e.steps(1);
        CHECK_EQ(e.fv(), fv(4, 2));
        CHECK_EQ(e.cpu.a_[0], 0x3000);
        CHECK_EQ(e.cpu.a_[1], 0xc00000);
    }
}

// 割り込みのベクタが奇数: 割り込みのフレームの下にアドレスエラーのフレームを積む (割り込みのフレームを消さない)
void testOddInterruptVector(bool fast) {
    Env060 e(fast);
    e.bus.put(0x1000, {0x4e71, 0x4e71});
    e.bus.w32(0x40 * 4, 0x10001);
    e.bus.iackVec = 0x40;
    e.cpu.setSR(0x2000);
    e.cpu.setIPL(3);
    e.steps(1);
    CHECK_EQ(e.cpu.a_[7], 0x9000 - 8 - 12);
    CHECK_EQ(e.fv(), fv(2, 3));
    CHECK_EQ(e.ea(), 0x10000);
    CHECK_EQ(e.bus.r16(0x9000 - 8 + 6), fv(0, 0x40));
    CHECK_EQ(e.bus.r32(0x9000 - 8 + 2), 0x1000);
    // アドレスエラーのベクタも奇数なら止まる
    Env060 h(fast);
    h.bus.put(0x1000, {0x4e71});
    h.bus.w32(0x40 * 4, 0x10001);
    h.bus.w32(3 * 4, 0x10003);
    h.bus.iackVec = 0x40;
    h.cpu.setSR(0x2000);
    h.cpu.setIPL(3);
    h.steps(1);
    CHECK_EQ(h.cpu.halted(), true);
    // アクセスエラーのベクタが奇数でも止まる (アクセスエラーの例外処理の途中のアドレスエラーはダブルフォルト。8.2.2)
    Env060 g(fast);
    g.bus.put(0x1000, {0x2010}); // MOVE.L (A0),D0
    g.bus.w32(2 * 4, 0x10003);
    g.cpu.a_[0] = 0xc00000;
    g.steps(1);
    CHECK_EQ(g.cpu.halted(), true);
}

// ADDX.L -(A0),-(A1) の書き込みが保護違反: A0 / A1 と CCR (X) を命令の始めに戻すので、保護を外して RTE すると
// 正しい結果 ($FFFFFFFF + 1 + X=0 = 0、X=1) で終わる
void testRestartAddx(bool fast) {
    Env060 e(fast);
    auto& c = e.cpu;
    e.bus.put(0x1000, {0xd388, 0x4e71}); // ADDX.L -(A0),-(A1)
    e.bus.w32(0x2000, 0xffffffff);
    e.bus.w32(0x3000, 0x00000001);
    c.a_[0] = 0x2004;
    c.a_[1] = 0x01003004;     // $01xxxxxx は DTT0 で書き込み保護 (Bus には下位 24 ビットで出る)
    c.dtt_[0] = 0x0100c004;   // 基準 $01、マスク 0、E、S は見ない、W
    c.updateMmu();
    c.flushTlb();
    c.setSR(0x2700);          // X = 0
    e.steps(1);
    CHECK_EQ(c.pc_, handler(2));
    CHECK_EQ(e.fv(), fv(4, 2));
    CHECK_EQ(e.ea(), 0x01003000);
    CHECK_EQ(e.fslw() & 0x88, 0x88); // WP | TTR
    CHECK_EQ(c.a_[0], 0x2004);
    CHECK_EQ(c.a_[1], 0x01003004);
    CHECK_EQ(e.bus.r16(c.a_[7]) & 0x10, 0); // 積んだ SR の X は 0
    c.dtt_[0] &= ~4u;
    c.updateMmu();
    c.flushTlb();
    e.steps(2); // 処理ルーチン (ADDQ、RTE)
    CHECK_EQ(c.pc_, 0x1000);
    e.steps(1);
    CHECK_EQ(c.pc_, 0x1002);
    CHECK_EQ(e.bus.r32(0x3000), 0);
    CHECK_EQ(c.flagX_, 1);
    CHECK_EQ(c.a_[0], 0x2000);
    CHECK_EQ(c.a_[1], 0x01003000);
}

// ページをまたぐアクセスの後半がバスエラー: 元のアドレスと大きさを積み、MA を立てる (8.4.4.2、8.4.4.3)。
// MOVES で FC 3 を読むときのバスエラー: TM = 3、TT = 2 (表 2-2、2-4)、大きさは命令のもの
void testFaultInfo(bool fast) {
    {
        Env060 e(fast);
        e.bus.put(0x1000, {0x2010}); // MOVE.L (A0),D0
        e.cpu.a_[0] = 0x00bffffe;
        e.steps(1);
        CHECK_EQ(e.fv(), fv(4, 2));
        CHECK_EQ(e.ea(), 0x00bffffe);
        CHECK_EQ(e.fslw(), (1u << 27) | (2u << 23) | (2u << 21) | (5u << 16) | (1u << 5)); // MA、読み、ロング、FC=5、RE
    }
    {
        Env060 e(fast);
        e.bus.put(0x1000, {0x2080}); // MOVE.L D0,(A0)
        e.cpu.a_[0] = 0x00bfffff;
        e.steps(1);
        CHECK_EQ(e.ea(), 0x00bfffff);
        CHECK_EQ(e.fslw(), (1u << 27) | (1u << 23) | (2u << 21) | (5u << 16) | (1u << 4)); // MA、書き、ロング、FC=5、WE
    }
    { // 最初の転送でエラーなら MA は立てない (8.4.4.3)
        Env060 e(fast);
        e.bus.put(0x1000, {0x2010}); // MOVE.L (A0),D0
        e.cpu.a_[0] = 0x00c00001;
        e.steps(1);
        CHECK_EQ(e.ea(), 0x00c00001);
        CHECK_EQ(e.fslw(), (2u << 23) | (2u << 21) | (5u << 16) | (1u << 5));
    }
    for (int write = 0; write < 2; write++) { // MOVES.L のページをまたぐアクセスの後半でエラー: MA
        Env060 e(fast);
        e.bus.put(0x1000, {0x0e90, (uint16_t)(write ? 0x0800 : 0x0000)}); // MOVES.L (A0),D0 / D0,(A0)
        e.cpu.sfc_ = e.cpu.dfc_ = 3;
        e.cpu.a_[0] = 0x00bffffe;
        e.steps(1);
        CHECK_EQ(e.fv(), fv(4, 2));
        CHECK_EQ(e.ea(), 0x00bffffe);
        CHECK_EQ(e.fslw(), (1u << 27) | (write ? 1u << 23 : 2u << 23) | (2u << 21) | (2u << 19) | (3u << 16) |
                               (write ? 1u << 4 : 1u << 5));
    }
    {
        Env060 e(fast);
        e.bus.put(0x1000, {0x0e90, 0x0000}); // MOVES.L (A0),D0
        e.cpu.sfc_ = 3;
        e.cpu.a_[0] = 0x00c00010;
        e.steps(1);
        CHECK_EQ(e.fv(), fv(4, 2));
        CHECK_EQ(e.ea(), 0x00c00010);
        CHECK_EQ(e.fslw(), (2u << 23) | (2u << 21) | (2u << 19) | (3u << 16) | (1u << 5)); // 読み、ロング、TT=2、FC=3、RE
    }
}

// MMU: 表を引いて変換する。保護違反などはフォーマット $4 で FSLW に原因を積む。表を引く途中のバスエラーは TWE で、
// アドレス、読み書き、FC、大きさ、命令の読み込みかは、もとのアクセスのもの (MC68060UM 4.2、8.2.1)
void testMmu(bool fast) {
    { // 変換、U / M、書き込み保護
        Env060 e(fast);
        auto& b = e.bus;
        auto& c = e.cpu;
        b.w32(0x40000, 0x40200 | 2);
        b.w32(0x40200, 0x40400 | 2);
        b.w32(0x40200 + 0x20 * 4, 0x40600 | 2);
        for (int i = 0; i < 64; i++) b.w32(0x40400 + i * 4, (uint32_t)(i << 12) | 1);
        b.w32(0x40600, 0x30000 | 1);
        b.w32(0x40604, 0x31000 | 4 | 1);
        b.w32(0x30010, 0xdeadbeef);
        c.urp_ = c.srp060_ = 0x40000;
        c.tc060_ = 0x8000;
        c.updateMmu();
        c.flushTlb();
        b.put(0x1000, {0x2010, 0x2080, 0x2081});
        c.a_[0] = 0x800010;
        c.d_[1] = 0x55;
        e.steps(1);
        CHECK_EQ(c.d_[0], 0xdeadbeef);
        CHECK_EQ(b.r32(0x40600) & 0x18, 0x08); // U だけ
        c.d_[0] = 0xcafe0001;
        e.steps(1);
        CHECK_EQ(b.r32(0x30010), 0xcafe0001);
        CHECK_EQ(b.r32(0x40600) & 0x10, 0x10); // M
        c.a_[0] = 0x801000;
        e.steps(1);
        CHECK_EQ(e.fv(), fv(4, 2));
        CHECK_EQ(e.ea(), 0x801000);
        CHECK_EQ(e.fslw() & 0x80, 0x80); // WP
        CHECK_EQ(b.r32(0x31000), 0);
    }
    // 表がバスエラーの場所にある。データの読み書き: 命令は ITT0 で透過、$00xxxxxx のデータは DTT0 で透過
    for (int write = 0; write < 2; write++) {
        Env060 e(fast);
        auto& c = e.cpu;
        e.bus.put(0x1000, {(uint16_t)(write ? 0x3080 : 0x3010), 0x4e71}); // MOVE.W D0,(A0) / MOVE.W (A0),D0
        e.bus.w32(0x5000, 0x11112222);
        e.bus.put(0x5100, {0x2011}); // MOVE.L (A1),D0: 前のアクセスの logical_ を $5000 台にしておく
        c.itt_[0] = 0x00ffc000;
        c.dtt_[0] = 0x0000c000;
        c.srp060_ = 0xc00000;
        c.tc060_ = 0x8000;
        c.updateMmu();
        c.flushTlb();
        c.a_[1] = 0x5000;
        c.pc_ = 0x5100;
        e.steps(1);
        CHECK_EQ(c.d_[0], 0x11112222);
        c.pc_ = 0x1000;
        c.a_[0] = 0x12345678;
        e.steps(1);
        CHECK_EQ(e.fv(), fv(4, 2));
        CHECK_EQ(e.pcAt(), 0x1000);
        CHECK_EQ(e.ea(), 0x12345678);
        const uint32_t want = (write ? 1u << 23 : 2u << 23) | (1u << 21) | (5u << 16) | (1u << 6); // RW、ワード、FC=5、TWE
        CHECK_EQ(e.fslw(), want);
    }
    { // 命令の読み込みで表を引く: IO、FC=6、アドレスはその命令
        Env060 e(fast);
        auto& c = e.cpu;
        e.bus.put(0x1000, {0x4e71});
        c.dtt_[0] = 0x00ffc000;
        c.srp060_ = 0xc00000;
        c.tc060_ = 0x8000;
        c.updateMmu();
        c.flushTlb();
        e.steps(1);
        CHECK_EQ(e.fv(), fv(4, 2));
        CHECK_EQ(e.ea(), 0x1000);
        CHECK_EQ(e.fslw(), (2u << 23) | (1u << 21) | (6u << 16) | (1u << 15) | (1u << 6));
    }
}

// MOVES で FC 0 / 4 / 7 (変換しない物理アドレス)。奇数番地のワード / ロングも普通に読み書きできる。バスエラーは
// TM に FC、TT = 2、SIZE は命令の大きさで積み、2 回目以降の転送のエラーなら MA を立てる (表 2-2、8.4.4.3)。
// (An)+ の An は命令の始めの値に戻す
void testMovesAlt(bool fast) {
    static const int fcs[] = {0, 4, 7};
    for (int fc : fcs) {
        { // 奇数番地のロング: 書いて読み戻す
            Env060 e(fast);
            e.bus.put(0x1000, {0x0e90, 0x0800, 0x0e90, 0x1000}); // MOVES.L D0,(A0); MOVES.L (A0),D1
            e.cpu.sfc_ = e.cpu.dfc_ = (uint32_t)fc;
            e.cpu.a_[0] = 0x3001;
            e.cpu.d_[0] = 0x11223344;
            e.steps(2);
            CHECK_EQ(e.cpu.pc_, 0x1008);
            CHECK_EQ(e.bus.mem[0x3001], 0x11);
            CHECK_EQ(e.bus.mem[0x3002], 0x22);
            CHECK_EQ(e.bus.mem[0x3003], 0x33);
            CHECK_EQ(e.bus.mem[0x3004], 0x44);
            CHECK_EQ(e.cpu.d_[1], 0x11223344);
        }
        { // 奇数番地のワード: 書いて読み戻す (A1 に読むと符号拡張)
            Env060 e(fast);
            e.bus.put(0x1000, {0x0e50, 0x0800, 0x0e50, 0x9000}); // MOVES.W D0,(A0); MOVES.W (A0),A1
            e.cpu.sfc_ = e.cpu.dfc_ = (uint32_t)fc;
            e.cpu.a_[0] = 0x3003;
            e.cpu.d_[0] = 0xabcd8765;
            e.steps(2);
            CHECK_EQ(e.bus.mem[0x3003], 0x87);
            CHECK_EQ(e.bus.mem[0x3004], 0x65);
            CHECK_EQ(e.cpu.a_[1], 0xffff8765);
        }
        for (int write = 0; write < 2; write++) {
            const uint32_t rw = write ? 1u << 23 : 2u << 23;
            const uint32_t re = write ? 1u << 4 : 1u << 5;
            const uint32_t base = (2u << 19) | ((uint32_t)fc << 16) | rw | re; // TT = 2、TM = FC
            { // 奇数番地のワードの 2 回目のバイトでエラー: MA、SIZE はワード
                Env060 e(fast);
                e.bus.put(0x1000, {0x0e50, (uint16_t)(write ? 0x0800 : 0x0000)}); // MOVES.W (A0),D0 / D0,(A0)
                e.cpu.sfc_ = e.cpu.dfc_ = (uint32_t)fc;
                e.cpu.a_[0] = 0x00bfffff;
                e.steps(1);
                CHECK_EQ(e.fv(), fv(4, 2));
                CHECK_EQ(e.pcAt(), 0x1000);
                CHECK_EQ(e.ea(), 0x00bfffff);
                CHECK_EQ(e.fslw(), (1u << 27) | (1u << 21) | base);
            }
            { // (A0)+ の奇数番地のロングの 2 回目の転送 (ワード) でエラー: MA、A0 は戻す
                Env060 e(fast);
                e.bus.put(0x1000, {0x0e98, (uint16_t)(write ? 0x0800 : 0x0000)}); // MOVES.L (A0)+,D0 / D0,(A0)+
                e.cpu.sfc_ = e.cpu.dfc_ = (uint32_t)fc;
                e.cpu.a_[0] = 0x00bfffff;
                e.steps(1);
                CHECK_EQ(e.fv(), fv(4, 2));
                CHECK_EQ(e.ea(), 0x00bfffff);
                CHECK_EQ(e.fslw(), (1u << 27) | (2u << 21) | base);
                CHECK_EQ(e.cpu.a_[0], 0x00bfffff);
            }
            { // 最初の転送でエラー: MA は立てない
                Env060 e(fast);
                e.bus.put(0x1000, {0x0e90, (uint16_t)(write ? 0x0800 : 0x0000)}); // MOVES.L (A0),D0 / D0,(A0)
                e.cpu.sfc_ = e.cpu.dfc_ = (uint32_t)fc;
                e.cpu.a_[0] = 0x00c00001;
                e.steps(1);
                CHECK_EQ(e.fv(), fv(4, 2));
                CHECK_EQ(e.ea(), 0x00c00001);
                CHECK_EQ(e.fslw(), (2u << 21) | base);
            }
        }
    }
}

// 普通の例外: TRAP のベクタが奇数なら、TRAP のフレーム (フォーマット $0、PC は次の命令) の下にアドレスエラーの
// フレーム (フォーマット $2、PC は TRAP の番地、アドレスはベクタの A0 を 0 にしたもの) を積む (MC68060UM 8.2.2)。
// ユーザモードからの例外は SSP に積み、USP はそのまま。RTE でユーザモードと USP に戻る
void testTrapExceptions(bool fast) {
    {
        Env060 e(fast);
        e.bus.put(0x1000, {0x4e40, 0x4e71}); // TRAP #0
        e.bus.w32(32 * 4, 0x10001);
        e.cpu.setSR(0x2704);
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, handler(3));
        CHECK_EQ(e.cpu.a_[7], 0x9000 - 8 - 12);
        CHECK_EQ(e.fv(), fv(2, 3));
        CHECK_EQ(e.pcAt(), 0x1000);
        CHECK_EQ(e.ea(), 0x10000);
        CHECK_EQ(e.bus.r16(0x9000 - 8), 0x2704);
        CHECK_EQ(e.bus.r32(0x9000 - 8 + 2), 0x1002);
        CHECK_EQ(e.bus.r16(0x9000 - 8 + 6), fv(0, 32));
    }
    {
        Env060 e(fast);
        e.bus.put(0x1000, {0x4e43, 0x4e71}); // TRAP #3
        e.cpu.usp_ = 0x5000;
        e.cpu.setSR(0x001f);
        CHECK_EQ(e.cpu.a_[7], 0x5000);
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, handler(35));
        CHECK_EQ(e.cpu.sflag_, true);
        CHECK_EQ(e.cpu.a_[7], 0x9000 - 8);
        CHECK_EQ(e.cpu.usp_, 0x5000);
        CHECK_EQ(e.bus.r16(0x9000 - 8), 0x001f);
        CHECK_EQ(e.pcAt(), 0x1002);
        CHECK_EQ(e.fv(), fv(0, 35));
        e.steps(2); // 処理ルーチン (ADDQ、RTE)
        CHECK_EQ(e.count(35), 1);
        CHECK_EQ(e.cpu.pc_, 0x1002);
        CHECK_EQ(e.cpu.sflag_, false);
        CHECK_EQ(e.cpu.a_[7], 0x5000);
        CHECK_EQ(e.cpu.getSR(), 0x001f);
    }
}

// RTE で PC が奇数: フレームを外してからアドレスエラー (フォーマット $2)。積む SR は、上位バイトが RTE を始めたときの
// 値、CCR は新しい SR が 0 なら Z、bit15 が 1 なら N で、ほかは 0。PC は RTE の番地、アドレスは PC の A0 を 0 にしたもの。
// 上位バイトと PC は MC68060UM 8.4。フレームを外すことと CCR はマニュアルになく、実機では確かめていない (今の動きを固定する)
void testRteOddPc(bool fast) {
    struct Case {
        uint16_t frameSr;
        int format;
        uint16_t startSr;
        uint16_t want;
    };
    static const Case cases[] = {
        {0x2300, 0x0, 0x2715, 0x2700}, // スーパーバイザモードに戻るフレーム
        {0x001f, 0x0, 0x2715, 0x2700}, // ユーザモードに戻るフレーム: USP には切り替えない
        {0x0000, 0x0, 0x2715, 0x2704}, // 新しい SR が 0: Z
        {0xa71f, 0x0, 0x2715, 0x2708}, // 新しい SR の bit15 が 1: N
        {0x4000, 0x0, 0x2715, 0x2700}, // 新しい SR が $4000 (68060 にない T0 だけ): 0 ではないので Z にならない
        {0x2300, 0x2, 0x2015, 0x2000},
        {0x2300, 0x3, 0x2015, 0x2000},
        {0x2300, 0x4, 0x2015, 0x2000},
        {0x2300, 0x0, 0xa715, 0xa700}, // T1 = 1: 積む SR には T1 が残る
    };
    for (const Case& k : cases) {
        Env060 e(fast);
        e.bus.put(0x1000, {0x4e73}); // RTE
        int size = k.format == 0 ? 8 : k.format == 4 ? 16 : 12;
        uint32_t base = 0x9000 - size;
        e.cpu.usp_ = 0x5000;
        e.cpu.setSR(k.startSr);
        e.cpu.a_[7] = base;
        e.bus.w16(base, k.frameSr);
        e.bus.w32(base + 2, 0x3001);
        e.bus.w16(base + 6, fv(k.format, 0));
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, handler(3));
        CHECK_EQ(e.cpu.a_[7], 0x9000 - 12);
        CHECK_EQ(e.cpu.usp_, 0x5000);
        CHECK_EQ(e.fv(), fv(2, 3));
        CHECK_EQ(e.bus.r16(e.sp()), k.want);
        CHECK_EQ(e.pcAt(), 0x1000);
        CHECK_EQ(e.ea(), 0x3000);
        CHECK_EQ(e.cpu.getSR() & 0xc000, 0); // 処理ルーチンはトレースなしで始まる
    }
    { // フォーマットが 68060 にない ($1) うえに PC も奇数: フォーマットエラー (ベクタ 14) が先。フレームは残る
        Env060 e(fast);
        e.bus.put(0x1000, {0x4e73});
        e.cpu.setSR(0x2715);
        e.cpu.a_[7] = 0x9000 - 8;
        e.bus.w16(0x9000 - 8, 0x2300);
        e.bus.w32(0x9000 - 8 + 2, 0x3001);
        e.bus.w16(0x9000 - 8 + 6, fv(1, 0));
        e.steps(1);
        CHECK_EQ(e.cpu.pc_, handler(14));
        CHECK_EQ(e.cpu.a_[7], 0x9000 - 8 - 8);
        CHECK_EQ(e.fv(), fv(0, 14));
        CHECK_EQ(e.bus.r16(e.sp()), 0x2715);
        CHECK_EQ(e.pcAt(), 0x1000);
        CHECK_EQ(e.bus.r32(0x9000 - 8 + 2), 0x3001);
    }
}

// MOVES の FC 0 / 4 / 7 は MMU も TTR も通らない (MC68060UM 4.1、表 4-1)。MMU を有効にし、ユーザのアクセスの全体に
// 当たる書き込み禁止の DTT を置いても、FC 0 / 4 / 7 は論理アドレスをそのまま物理アドレスとして読み書きする。
// FC 1 は TTR に当たって止まり、FC 5 は MMU で変換される (DTT をスーパーバイザにも当てると、例外のフレームを積めない)
void testMovesNoTranslate(bool fast) {
    for (int fc = 0; fc <= 7; fc++) {
        if (fc == 2 || fc == 3 || fc == 6) continue;
        Env060 e(fast);
        auto& b = e.bus;
        auto& c = e.cpu;
        // $0〜$3FFFF はそのまま、$800000 のページは $30000 に変換する表 (testMmu と同じ)
        b.w32(0x40000, 0x40200 | 2);
        b.w32(0x40200, 0x40400 | 2);
        b.w32(0x40200 + 0x20 * 4, 0x40600 | 2);
        for (int i = 0; i < 64; i++) b.w32(0x40400 + i * 4, (uint32_t)(i << 12) | 1);
        b.w32(0x40600, 0x30000 | 1);
        c.urp_ = c.srp060_ = 0x40000;
        c.tc060_ = 0x8000;
        c.dtt_[0] = 0x00ff8004; // 全体に当たる、ユーザだけ、書き込み禁止
        c.updateMmu();
        c.flushTlb();
        b.put(0x1000, {0x0e90, 0x1800, 0x0e90, 0x2000}); // MOVES.L D1,(A0); MOVES.L (A0),D2
        c.sfc_ = c.dfc_ = (uint32_t)fc;
        c.a_[0] = 0x800010;
        c.d_[1] = 0x13572468;
        b.w32(0x30010, 0xdeadbeef);
        e.steps(1);
        if (fc == 1) {
            CHECK_EQ(e.fv(), fv(4, 2));
            CHECK_EQ(e.fslw() & 0x88, 0x88); // WP と TTR
            CHECK_EQ(b.r32(0x800010), 0);
            CHECK_EQ(b.r32(0x30010), 0xdeadbeef);
            continue;
        }
        if (fc == 5) {
            CHECK_EQ(c.pc_, 0x1004);
            CHECK_EQ(b.r32(0x30010), 0x13572468); // MMU で $30010 に変換される
            CHECK_EQ(b.r32(0x800010), 0);
            continue;
        }
        CHECK_EQ(c.pc_, 0x1004);
        CHECK_EQ(b.r32(0x800010), 0x13572468); // 変換されない
        CHECK_EQ(b.r32(0x30010), 0xdeadbeef);
        e.steps(1);
        CHECK_EQ(c.pc_, 0x1008);
        CHECK_EQ(c.d_[2], 0x13572468);
    }
}

// TTR の書き込み保護 (W): 当たったデータの書き込みはアクセスエラー (FSLW の WP と TTR)、読み出しはできる。
// S が 00 ならユーザ、01 ならスーパーバイザのアクセスだけに当たる。MOVES は SFC / DFC の FC 1 をユーザ、FC 5 を
// スーパーバイザのアクセスとして扱う。ITT はデータのアクセスには使われない
// (MC68060UM 4.1.3、8.4.4)
void testTtrWriteProtect(bool fast) {
    { // S を見ない TTR: 読めるが書けない。メモリは変わらない
        Env060 e(fast);
        auto& c = e.cpu;
        e.bus.put(0x1000, {0x2010, 0x2081}); // MOVE.L (A0),D0; MOVE.L D1,(A0)
        e.bus.w32(0x3000, 0x12345678);
        c.a_[0] = 0x01003000;
        c.d_[1] = 0xdeadbeef;
        c.dtt_[0] = 0x0100c004;
        c.updateMmu();
        c.flushTlb();
        e.steps(1);
        CHECK_EQ(c.d_[0], 0x12345678);
        e.steps(1);
        CHECK_EQ(e.fv(), fv(4, 2));
        CHECK_EQ(e.pcAt(), 0x1002);
        CHECK_EQ(e.ea(), 0x01003000);
        CHECK_EQ(e.fslw() & 0x88, 0x88);
        CHECK_EQ(e.bus.r32(0x3000), 0x12345678);
    }
    // S = 00 (ユーザ) / 01 (スーパーバイザ) の TTR と、スーパーバイザ / ユーザモードの書き込み
    struct Case {
        uint32_t ttr;
        uint16_t sr;
        bool fault;
    };
    static const Case cases[] = {
        {0x01008004, 0x2700, false}, // ユーザだけの TTR、スーパーバイザの書き込み
        {0x01008004, 0x0700, true},  // ユーザだけの TTR、ユーザの書き込み
        {0x0100a004, 0x2700, true},  // スーパーバイザだけの TTR、スーパーバイザの書き込み
        {0x0100a004, 0x0700, false}, // スーパーバイザだけの TTR、ユーザの書き込み
    };
    for (const Case& k : cases) {
        Env060 e(fast);
        auto& c = e.cpu;
        e.bus.put(0x1000, {0x2081, 0x4e71}); // MOVE.L D1,(A0)
        c.a_[0] = 0x01003000;
        c.d_[1] = 0xcafef00d;
        c.usp_ = 0x5000;
        c.setSR(k.sr);
        c.dtt_[0] = k.ttr;
        c.updateMmu();
        c.flushTlb();
        e.steps(1);
        if (k.fault) {
            CHECK_EQ(c.pc_, handler(2));
            CHECK_EQ(e.fslw() & 0x88, 0x88);
            CHECK_EQ(e.bus.r32(0x3000), 0);
        } else {
            CHECK_EQ(c.pc_, 0x1002);
            CHECK_EQ(e.bus.r32(0x3000), 0xcafef00d);
        }
    }
    { // MOVES で FC 1 (ユーザのデータ) に書く: ユーザだけの TTR に当たる。FC 5 なら当たらない
        for (int fc = 1; fc <= 5; fc += 4) {
            Env060 e(fast);
            auto& c = e.cpu;
            e.bus.put(0x1000, {0x0e90, 0x1800}); // MOVES.L D1,(A0)
            c.a_[0] = 0x01003000;
            c.d_[1] = 0x01020304;
            c.dfc_ = (uint32_t)fc;
            c.dtt_[0] = 0x01008004;
            c.updateMmu();
            c.flushTlb();
            e.steps(1);
            if (fc == 1) {
                CHECK_EQ(e.fv(), fv(4, 2));
                CHECK_EQ(e.fslw() & 0x88, 0x88);
                CHECK_EQ((e.fslw() >> 16) & 7, 1u);
                CHECK_EQ(e.bus.r32(0x3000), 0);
            } else {
                CHECK_EQ(c.pc_, 0x1004);
                CHECK_EQ(e.bus.r32(0x3000), 0x01020304);
            }
        }
    }
    { // ITT の W はデータの書き込みには使われない
        Env060 e(fast);
        auto& c = e.cpu;
        e.bus.put(0x1000, {0x2081, 0x4e71}); // MOVE.L D1,(A0)
        c.a_[0] = 0x01003000;
        c.d_[1] = 0x55aa55aa;
        c.itt_[0] = 0x0100c004;
        c.updateMmu();
        c.flushTlb();
        e.steps(1);
        CHECK_EQ(c.pc_, 0x1002);
        CHECK_EQ(e.bus.r32(0x3000), 0x55aa55aa);
    }
}

// CHK と DIVx の未定義のフラグ。68060 は CHK で N だけを書き (Dn < 0)、Z / V / C を変えない。DIVx はゼロ除算で C だけを
// 0 にし、あふれで V を 1、C を 0 にして N と Z を変えない (cpu020_ops.inc の opChk() / opDivW() / opDivL() の説明。
// instructiontest.x hard の chk / div で実機と合わせた)。68030 の規則とは違う結果になる組み合わせを選ぶ
void testChkDivFlags(bool fast) {
    struct Case {
        uint16_t op, ext;
        uint32_t d0, d1;
        uint16_t ccr, want;
        int vector; // 0 なら例外なし
    };
    static const Case cases[] = {
        {0x4181, 0, 5, 10, 0x1f, 0x17, 0},                   // CHK.W D1,D0: 範囲内。N だけ 0 に
        {0x4181, 0, 0xffff, 10, 0x07, 0x0f, 6},              // CHK.W: Dn < 0 で N を 1 に、Z / V / C は残る
        {0x4101, 0, 20, 10, 0x00, 0x00, 6},                  // CHK.L: 上限を超える。68030 なら C=1
        {0x80c1, 0, 10, 0, 0x1f, 0x1e, 5},                   // DIVU.W D1,D0: ゼロ除算は C だけ 0 に
        {0x81c1, 0, 10, 0, 0x0a, 0x0a, 5},                   // DIVS.W: ゼロ除算。68030 なら Z=1 N=0
        {0x80c1, 0, 0x10000, 1, 0x1d, 0x1e, 0},              // DIVU.W: あふれ。N と Z は残る
        {0x81c1, 0, 0x8000, 1, 0x04, 0x06, 0},               // DIVS.W: 商が 16 ビットに収まらない
        {0x80c1, 0, 10, 3, 0x1f, 0x10, 0},                   // DIVU.W: 通常の結果は定義どおり
        {0x4c41, 0x0800, 10, 0, 0x0b, 0x0a, 5},              // DIVS.L D1,D0: ゼロ除算
        {0x4c41, 0x0000, 10, 0, 0x0e, 0x0e, 5},              // DIVU.L: ゼロ除算 (V は残る)
        {0x4c41, 0x0800, 0x80000000u, 0xffffffffu, 0x0c, 0x0e, 0}, // DIVS.L: あふれ
    };
    for (const Case& k : cases) {
        Env060 e(fast);
        if (k.op == 0x4c41) e.bus.put(0x1000, {k.op, k.ext, 0x4e71});
        else e.bus.put(0x1000, {k.op, 0x4e71});
        e.cpu.d_[0] = k.d0;
        e.cpu.d_[1] = k.d1;
        e.cpu.setSR((uint16_t)(0x2700 | k.ccr));
        e.steps(1);
        CHECK_EQ(e.cpu.getSR() & 0x1f, k.want);
        CHECK_EQ(e.cpu.pc_, k.vector ? handler(k.vector) : (k.op == 0x4c41 ? 0x1004u : 0x1002u));
    }
}

// すべてのオペコードで、アクセスエラーか、奇数番地へ飛ぶアドレスエラーになったときに、A0-A6、A7、CCR が命令の
// 始めの値に戻ることを確かめる (68060 は命令をはじめからやり直す。MC68060UM 8.4)。命令は run() で動かす (step() と違い、2 つ目の命令からは
// 命令ごとの検査を省いた経路を通る)。$C00000-$CFFFFF はバスエラーにする。アドレスレジスタはその中を指し、
// はじめの何回かのアクセスを通す (berrSkip) ことと、書き込みだけをバスエラーにすることで、命令の途中のいろいろな所で
// アクセスエラーにする。命令を $BFFFFE に置き、拡張語の読み込みをバスエラーにする場合も調べる。
// 調べる命令の前には、CCR と A0 と A7 を変える命令を置く (下の kPre の説明)。
// その中のメモリと、スーパーバイザのスタックの上はすべて $01 にしておく (RTS などの戻り先やメモリ間接のポインタが奇数になる)。
// 調べるのは、1 つ目の例外がこの命令のアクセスエラーかアドレスエラーだった場合だけ (ほかの例外になる命令、
// どちらにもならない命令は数えない)
void testFaultSweep() {
    Env060 e(true);
    auto& c = e.cpu;
    const uint32_t kOther = 0x11000, kFault = 0x12000, kSsp = 0x9000, kUsp = 0xc00804;
    struct Scenario {
        uint32_t code; // 前に置く 3 命令の番地 (調べる命令は kPre バイト先)
        bool user;
        int berr;      // TestBus::BERR_ALL / BERR_WRITE
        int skip;
        uint8_t ccr;
    };
    static const Scenario scenarios[] = {
        {0x1000, false, 0, 0, 0x1f}, {0x1000, false, 0, 0, 0x00}, {0x1000, false, 0, 1, 0x1f}, {0x1000, false, 0, 2, 0x00},
        {0x1000, false, 1, 0, 0x1f}, {0x1000, false, 1, 0, 0x00}, {0x1000, false, 1, 1, 0x1f},
        {0x1000, true, 0, 0, 0x1f},  {0x1000, true, 0, 1, 0x00},  {0x1000, true, 0, 2, 0x1f},
        {0x1000, true, 1, 0, 0x1f},  {0x1000, true, 1, 0, 0x00},  {0x1000, true, 1, 1, 0x1f},
        {0xbffff6, false, 0, 0, 0x1f}, {0xbffff6, false, 0, 1, 0x00}, {0xbffff6, false, 0, 2, 0x1f},
        {0xbffff6, true, 0, 0, 0x00},  {0xbffff6, true, 0, 1, 0x1f},
    };
    // 拡張語: $4E72 (STOP の語。次の命令として実行されると特権違反か STOP)、$4AFC (ILLEGAL)、
    // $0122 はフルフォーマットの ([bd.W,An,D0.W],od.W)
    static const uint16_t exts[2][6] = {
        {0x4e72, 0x4e72, 0x4e72, 0x4e72, 0x4e72, 0x4e72},
        {0x0122, 0x4afc, 0x4afc, 0x4afc, 0x4afc, 0x4afc},
    };
    // 調べる命令の前に、CCR、A0、A7 を変える 3 命令を置く: MOVE #ccr,CCR; ADDQ.L #2,A0; ADDQ.L #2,A7。
    // run() の 1 つ目の命令は命令ごとの検査を通り、A0-A7 と CCR を写す (saveUndo())。そのあとの命令が、前の命令の
    // 写しや記録を消さずに使うと、調べる命令の例外で、この 3 命令が変える前の値に戻る。前が NOP だと、戻す先が
    // 同じ値なので見分けられない
    const uint32_t kPre = 8;
    long checked = 0, checkedAccess = 0, checkedAddress = 0;
    const int before = failures;
    for (int op = 0; op < 65536 && failures < before + 20; op++) {
        // BSR.S で後ろへ飛ぶもの: 飛んだ先からこの命令へ戻ってくると、1 回目は積めて、2 回目に実行したときに
        // アクセスエラーになる場合があり、そのときの A7 は 1 回目に進んだ値で正しい
        if ((op & 0xff80) == 0x6180 && op != 0x61ff) continue;
        memset(&e.bus.mem[0xc00000], 0x01, 0x2000);
        for (const auto& ext : exts) {
            for (const Scenario& sc : scenarios) {
                e.bus.berr = TestBus::BERR_OFF;
                e.bus.put(kOther, {0x4e72, 0x2700}); // STOP
                e.bus.put(kFault, {0x4e72, 0x2700});
                e.bus.w32(0, kSsp);
                e.bus.w32(4, sc.code);
                // 前の命令がベクタを書き換えていることがあるので、毎回入れ直す
                for (int v = 2; v < 256; v++) e.bus.w32((uint32_t)v * 4, v <= 3 ? kFault : kOther);
                memset(&e.bus.mem[kSsp], 0x01, 16);
                e.bus.put(sc.code, {0x44fc, sc.ccr, 0x5488, 0x548f, (uint16_t)op, ext[0], ext[1], ext[2], ext[3], ext[4],
                                    ext[5], 0x4afc, 0x4afc});
                c.reset();
                for (int i = 0; i < 7; i++) c.a_[i] = 0xc01000 + (uint32_t)i * 0x100;
                for (int i = 0; i < 8; i++) c.d_[i] = 4 + (uint32_t)i * 2;
                c.usp_ = kUsp;
                const uint16_t sr = (uint16_t)((sc.user ? 0x0000 : 0x2700) | sc.ccr);
                // 前の 3 命令が変える前の値: CCR は逆、A0 と A7 は 2 小さい
                c.setSR((uint16_t)(sr ^ 0x1f));
                c.a_[0] -= 2;
                c.a_[7] -= 2;
                e.bus.berr = sc.berr ? TestBus::BERR_WRITE : TestBus::BERR_ALL;
                e.bus.berrSkip = sc.skip;
                c.run(400);
                e.bus.berr = TestBus::BERR_OFF;
                const uint32_t sp = c.a_[7];
                if (!c.stopped() || c.pc_ != kFault + 4 || sp < 0x100 || sp > kSsp) continue;
                const bool access = e.bus.r16(sp + 6) == fv(4, 2), address = e.bus.r16(sp + 6) == fv(2, 3);
                if (!(access || address) || e.bus.r32(sp + 2) != sc.code + kPre) continue;
                // RTE で戻り先が奇数のときは、フレームを外してから例外にする (testRteOddPc())
                if (address && op == 0x4e73) continue;
                checked++;
                (address ? checkedAddress : checkedAccess)++;
                bool ok = sp == kSsp - (access ? 16 : 12) && e.bus.r16(sp) == sr && (!sc.user || c.usp_ == kUsp);
                for (int i = 0; i < 7; i++) ok = ok && c.a_[i] == 0xc01000 + (uint32_t)i * 0x100;
                if (!ok) {
                    printf("  fault sweep: op %04x ext %04x code %x user %d berr %d skip %d ccr %02x: sp %x sr %04x usp %x a",
                           op, ext[0], sc.code, sc.user, sc.berr, sc.skip, sc.ccr, sp, e.bus.r16(sp), c.usp_);
                    for (int i = 0; i < 7; i++) printf(" %x", c.a_[i]);
                    printf("\n");
                    failures++;
                }
            }
        }
    }
    // 調べた場合の数が少なければ、この試験が意味をなしていない
    CHECK_EQ(checked > 300000, true);
    CHECK_EQ(checkedAddress > 1000, true);
    printf("fault sweep: %ld cases checked (access error %ld, address error %ld)\n", checked, checkedAccess, checkedAddress);
}

// アドレスエラーのフレームを積んでいる途中に止まった (スタックがバスエラーになる所にある) あと、setRegisters() で
// 続きを動かすと、次のアドレスエラーはふつうに例外になる (ダブルフォルトの判定に使う値が残っていない)
void testSetRegistersAfterHalt(bool fast) {
    Env060 e(fast);
    auto& c = e.cpu;
    e.bus.put(0x1000, {0x4ef9, 0x0000, 0x2001}); // JMP $2001
    c.a_[7] = 0xc00100;
    e.steps(1);
    CHECK_EQ(c.halted(), true);
    m68k::Registers r;
    c.getRegisters(r);
    r.ssp = 0x9000;
    r.pc = 0x1000;
    c.setRegisters(r);
    CHECK_EQ(c.halted(), false);
    e.steps(1);
    CHECK_EQ(c.halted(), false);
    CHECK_EQ(c.pc_, handler(3));
    CHECK_EQ(e.fv(), fv(2, 3));
    CHECK_EQ(e.pcAt(), 0x1000);
}

// ---- サイクル数 (MC68060UM 10 章。m68k/cpu060_cycles.inc) ----

// CACR を書く (MOVEC と同じく、キャッシュが無効のときのバスの時間を決め直す)。
// $80008000 は命令 / データキャッシュ (EIC、EDC) が有効、$00800000 は分岐キャッシュ (EBC) が有効
void setCacr(Env060& e, uint32_t v) {
    e.cpu.cacr_ = v;
    e.cpu.updateCacheCosts();
}
constexpr uint32_t CACHE_ON = 0x80008000u, EBC = 0x00800000u;

// words を $1000 に置いて n 命令を実行し、かかったサイクル数を返す (速さ 100 では 1 サイクルが Cpu020::SUB)
int cyclesOf(Env060& e, std::initializer_list<uint16_t> words, int n) {
    e.bus.put(0x1000, words);
    int c = 0;
    for (int i = 0; i < n; i++) c += e.cpu.step();
    return c;
}

// 命令ごとのサイクル数 (スーパースカラなし: リセットのあとの PCR の ESS は 0)
void testCycleTable(bool fast) {
    struct Case {
        const char* name;
        std::initializer_list<uint16_t> words;
        int cycles;
    };
    const Case cases[] = {
        {"MOVEQ #1,D0 (表 10-10)", {0x7001}, 1},
        {"ADD.L (A0),D0 (表 10-9)", {0xd090}, 1},
        {"ADD.L D0,(A0) (表 10-9)", {0xd190}, 1},
        {"MOVE.L (A0),(A1) (表 10-7)", {0x2290}, 2},
        {"MOVE.L #imm,(A1) (表 10-7)", {0x22bc, 0x0000, 0x0001}, 1},
        {"MOVE.L #imm,(d16,A1) (表 10-7)", {0x237c, 0x0000, 0x0001, 0x0004}, 2},
        {"ADDI.L #imm,D0 (表 10-10)", {0x0680, 0x0000, 0x0001}, 1},
        {"ADDI.L #imm,(d16,A0) (表 10-10)", {0x06a8, 0x0000, 0x0001, 0x0004}, 2},
        {"MULU.W D1,D0 (表 10-9)", {0xc0c1}, 2},
        {"DIVU.W D1,D0 (表 10-9)", {0x80c1}, 22},
        {"DIVU.L D1,D0 (表 10-9)", {0x4c41, 0x0000}, 38},
        {"MULU.L (d8,A0,D2),D0 (表 10-9 の注 3)", {0x4c30, 0x0000, 0x2000}, 3},
        {"LSL.L #8,D0 (表 10-13)", {0xe188}, 1},
        {"BTST #0,(d16,A0) (表 10-15)", {0x0828, 0x0000, 0x0004}, 2},
        {"BFEXTU D0{0:8},D1 (表 10-16)", {0xe9c0, 0x1008}, 6},
        {"LEA (d16,A0),A1 (表 10-20)", {0x43e8, 0x0004}, 1},
        {"LEA (bd,A0,D0),A1 (表 10-5)", {0x43f0, 0x0120, 0x0004}, 2},
        {"MOVE.L ([A0]),D0 (表 10-5)", {0x2030, 0x0151}, 4},
        {"PEA (d16,A0) (表 10-20)", {0x4868, 0x0004}, 2},
        {"MOVEM.L D0-D3,-(A7) (表 10-20)", {0x48e7, 0xf000}, 4},
        {"MOVEM.L (d8,A0,D0),D0-D2 (表 10-20)", {0x4cf0, 0x0007, 0x0800}, 4},
        {"CMPM.L (A0)+,(A1)+ (表 10-21)", {0xb388}, 2},
        {"ADDX.L D0,D1 (表 10-21)", {0xd380}, 1},
        {"NOP (表 10-24)", {0x4e71}, 9},
        {"LINK A6,#-4 (表 10-24)", {0x4e56, 0xfffc}, 2},
        {"MOVEC VBR,D0 (表 10-24)", {0x4e7a, 0x0801}, 11},
        {"MOVEC D0,CACR (表 10-24)", {0x4e7b, 0x0002}, 15},
        {"ANDI #$FFFF,SR (表 10-22)", {0x027c, 0xffff}, 12},
        {"TRAP #0 (表 10-26)", {0x4e40}, 19},
        {"JMP (A0) (表 10-18)", {0x4ed0}, 5},
        {"JSR (A0) (表 10-18)", {0x4e90}, 5},
        {"BRA.S (分岐キャッシュなし。表 10-17)", {0x6002}, 3},
        {"BSR.S (表 10-17)", {0x6102}, 3},
        {"BNE.S 前向き、分岐しない (表 10-17)", {0x6602}, 1},
        {"FADD.X FP1,FP0 (表 10-25)", {0xf200, 0x0422}, 3},
        {"FDIV.X FP1,FP0 (表 10-25)", {0xf200, 0x0420}, 37},
        {"FMOVE.X FP1,FP0 (表 10-25)", {0xf200, 0x0400}, 1},
    };
    for (const Case& k : cases) {
        Env060 e(fast);
        setCacr(e, CACHE_ON);
        e.cpu.a_[0] = 0x3000;
        e.cpu.a_[1] = 0x4000;
        e.cpu.d_[1] = 3;
        e.cpu.d_[0] = 0;
        e.cpu.setSR(0x2704); // Z = 1 (BNE は分岐しない)
        e.bus.w32(0x3000, 0x5000);
        e.cpu.fpu()->fp_[0] = kOne;
        e.cpu.fpu()->fp_[1] = kOne;
        const int c = cyclesOf(e, k.words, 1);
        if (c != k.cycles) printf("  %s: %d cycles, want %d\n", k.name, c, k.cycles);
        CHECK_EQ(c, k.cycles);
    }
    {
        Env060 e(fast); // RTS (表 10-19)、RTE
        setCacr(e, CACHE_ON);
        e.bus.w32(0x8000, 0x2000);
        e.cpu.a_[7] = 0x8000;
        CHECK_EQ(cyclesOf(e, {0x4e75}, 1), 7);
        CHECK_EQ(e.cpu.pc_, 0x2000u);
    }
    {
        Env060 e(fast);
        setCacr(e, CACHE_ON);
        e.cpu.a_[7] = 0x8000;
        e.bus.put(0x8000, {0x2000, 0x0000, 0x2000, 0x0000});
        CHECK_EQ(cyclesOf(e, {0x4e73}, 1), 17);
    }
}

// FPU のオペランドが NaN でも、命令のサイクル数は同じ (演算をせずに戻るところでも足す)
void testFpuNanCycles(bool fast) {
    struct Case {
        uint16_t ext;
        int cycles;
    };
    // FMOVE.X FP0,FP0 / FADD.X FP1,FP0 / FDIV.X FP1,FP0 / FSQRT.X FP1,FP0 / FCMP.X FP1,FP0 / FTST.X FP1
    const Case cases[] = {{0x0000, 1}, {0x0422, 3}, {0x0420, 37}, {0x0404, 68}, {0x0438, 1}, {0x043a, 1}};
    for (const Case& k : cases) {
        for (int nan = 0; nan < 2; nan++) {
            Env060 e(fast);
            setCacr(e, CACHE_ON);
            e.cpu.fpu()->fp_[0] = nan ? kSnan : kOne;
            e.cpu.fpu()->fp_[1] = nan ? kSnan : kOne;
            const int c = cyclesOf(e, {0xf200, k.ext}, 1);
            if (c != k.cycles) printf("  F200 %04x (%s): %d cycles, want %d\n", k.ext, nan ? "NaN" : "1.0", c, k.cycles);
            CHECK_EQ(c, k.cycles);
        }
    }
}

// 分岐キャッシュ (CACR の EBC) と DBRA / Bcc (表 10-17)
void testBranchCycles(bool fast) {
    {
        // DBRA D0,自分: 予測なしで 3、当たれば 1、ループを抜けるとき (予測が外れる) は 7
        Env060 e(fast);
        setCacr(e, CACHE_ON | EBC);
        e.cpu.d_[0] = 4;
        e.bus.put(0x1000, {0x51c8, 0xfffe, 0x4e71});
        CHECK_EQ(e.cpu.step(), 3);
        CHECK_EQ(e.cpu.step(), 1);
        CHECK_EQ(e.cpu.step(), 1);
        CHECK_EQ(e.cpu.step(), 1);
        CHECK_EQ(e.cpu.step(), 7);
        CHECK_EQ(e.cpu.pc_, 0x1004u);
    }
    {
        // 分岐キャッシュが無効なら、後ろ向きに分岐する DBRA はいつも 3
        Env060 e(fast);
        setCacr(e, CACHE_ON);
        e.cpu.d_[0] = 4;
        e.bus.put(0x1000, {0x51c8, 0xfffe, 0x4e71});
        for (int i = 0; i < 4; i++) CHECK_EQ(e.cpu.step(), 3);
        CHECK_EQ(e.cpu.step(), 7);
    }
    {
        // BNE 後ろ向き: 予測なしで 3、当たれば 0
        Env060 e(fast);
        setCacr(e, CACHE_ON | EBC);
        e.cpu.setSR(0x2700);
        e.bus.put(0x1000, {0x4a80, 0x66fc}); // TST.L D0 / BNE $1000
        e.cpu.d_[0] = 1;
        CHECK_EQ(e.cpu.step(), 1);
        CHECK_EQ(e.cpu.step(), 3);
        CHECK_EQ(e.cpu.step(), 1);
        CHECK_EQ(e.cpu.step(), 0);
        e.cpu.d_[0] = 0; // ループを抜ける: 予測が外れて 7
        CHECK_EQ(e.cpu.step(), 1);
        CHECK_EQ(e.cpu.step(), 7);
    }
}

// 分岐だけが続くループでも時間が進む: 予測が当たって分岐する BRA / Bcc / JMP の 0 サイクルは、分岐でない命令に
// 折りたたまれるときだけ (Cpu020::fold060())
void testBranchOnlyLoop(bool fast) {
    for (int kind = 0; kind < 3; kind++) {
        Env060 e(fast);
        setCacr(e, CACHE_ON | EBC);
        e.cpu.pcr_ |= 1;
        e.cpu.setSR(0x2700); // Z = 0
        if (kind == 0) e.bus.put(0x1000, {0x60fe});              // BRA.S 自分
        else if (kind == 1) e.bus.put(0x1000, {0x66fe});         // BNE.S 自分
        else e.bus.put(0x1000, {0x4ef8, 0x1000});                // JMP $1000.W
        e.steps(1000);
        const uint64_t c0 = e.cpu.cycles();
        e.steps(1000);
        CHECK_EQ(e.cpu.cycles() - c0, 1000u); // 1 つの分岐に 1 サイクル
        // run() が終わる (0 サイクルの命令が続いて止まらない)
        const uint64_t c1 = e.cpu.cycles();
        e.cpu.run(500);
        CHECK_EQ(e.cpu.cycles() - c1 >= 500 && e.cpu.cycles() - c1 < 510, true);
    }
    {
        // BRA / DBRA / BRA のように、種類の違う分岐だけが続いても 0 サイクルの分岐は出ない:
        // $1000 BRA.S $1004 / $1004 DBRA D0,$1008 / $1008 BRA.S $1000 は、予測が当たって 1 + 1 + 1
        Env060 e(fast);
        setCacr(e, CACHE_ON | EBC);
        e.cpu.d_[0] = 0x7fff;
        e.bus.put(0x1000, {0x6002, 0x4e71, 0x51c8, 0x0002, 0x60f6});
        e.steps(30);
        const uint64_t c0 = e.cpu.cycles();
        e.steps(300);
        CHECK_EQ(e.cpu.cycles() - c0, 300u);
    }
    {
        // BSR / RTS だけのループ ($1000 BSR.S $1004 / $1002 BRA.S $1000 / $1004 RTS): BSR 1 + RTS 7 + BRA 0 (RTS に折りたたむ)
        Env060 e(fast);
        setCacr(e, CACHE_ON | EBC);
        e.bus.put(0x1000, {0x6102, 0x60fc, 0x4e75});
        e.steps(30);
        const uint64_t c0 = e.cpu.cycles();
        e.steps(300);
        CHECK_EQ(e.cpu.cycles() - c0, 100u * (1 + 7 + 0));
    }
    {
        // 分岐でない命令が間にあれば、分岐は 0 サイクルのまま: NOP (9) / BRA.S で 1 周 9 サイクル
        Env060 e(fast);
        setCacr(e, CACHE_ON | EBC);
        e.bus.put(0x1000, {0x4e71, 0x60fc});
        e.steps(20);
        const uint64_t c0 = e.cpu.cycles();
        e.steps(200);
        CHECK_EQ(e.cpu.cycles() - c0, 900u);
    }
    {
        // 分岐が 2 つ続くループ (BRA.S $1004 / NOP / BRA.S $1000): 1 つめは折りたたまれず 1、2 つめは NOP に折りたたまれて 0
        Env060 e(fast);
        setCacr(e, CACHE_ON | EBC);
        e.bus.put(0x1000, {0x6002, 0x4e71, 0x4e71, 0x60f8});
        e.steps(30);
        const uint64_t c0 = e.cpu.cycles();
        e.steps(300);
        CHECK_EQ(e.cpu.cycles() - c0, 100u * (1 + 9 + 0));
    }
}

// 2 命令の同時実行 (PCR の ESS。10.1)
void testSuperscalar(bool fast) {
    // n 命令のサイクル数の合計。ess が false ならスーパースカラなし
    auto run = [fast](bool ess, std::initializer_list<uint16_t> words, int n) {
        Env060 e(fast);
        if (ess) e.cpu.pcr_ |= 1;
        setCacr(e, CACHE_ON | EBC);
        e.cpu.a_[0] = 0x3000;
        e.cpu.a_[1] = 0x4000;
        e.bus.w32(0x3000, 0x01000000);
        return cyclesOf(e, words, n);
    };
    // si.r の processor performance のループ: ADDQ.L #1,D0 / TST.B (A0) / BNE。1 周めは BNE の予測がなく 1 + 3、
    // 2 周めからは 1 周 1 サイクル (実機の 060turbo で 13348%、XVI 10MHz の 26.8 クロックの 1/133.5 = 1.0 サイクル)
    CHECK_EQ(run(true, {0x5280, 0x4a10, 0x66fa}, 3), 4);
    CHECK_EQ(run(true, {0x5280, 0x4a10, 0x66fa}, 3 + 3 * 10), 4 + 10);
    CHECK_EQ(run(false, {0x5280, 0x4a10, 0x66fa}, 3 + 3 * 10), 5 + 2 * 10);
    // 前の命令の結果を後の命令が使うと、同時に進まない (10.1.6)
    CHECK_EQ(run(true, {0xd081, 0xd480}, 2), 2); // ADD.L D1,D0 / ADD.L D0,D2
    CHECK_EQ(run(true, {0xd081, 0xd483}, 2), 1); // ADD.L D1,D0 / ADD.L D3,D2
    // MOVE.L <ea>,Rx の結果は、後の命令が演算に使っても同時に進む (10.1.6 の例外 1)。アドレスに使うなら進まない (10.1.5)
    CHECK_EQ(run(true, {0x2001, 0xd480}, 2), 1); // MOVE.L D1,D0 / ADD.L D0,D2
    CHECK_EQ(run(true, {0x2041, 0x2010}, 2), 2); // MOVEA.L D1,A0 / MOVE.L (A0),D0
    // Scc Dn の結果、(An)+ / -(An) で動かした An を、後の命令が演算に使うときも同時に進まない
    CHECK_EQ(run(true, {0x50c0, 0xd280}, 2), 2); // ST D0 / ADD.L D0,D1
    CHECK_EQ(run(true, {0x50c0, 0xd282}, 2), 1); // ST D0 / ADD.L D2,D1
    CHECK_EQ(run(true, {0x2018, 0x2208}, 2), 2); // MOVE.L (A0)+,D0 / MOVE.L A0,D1
    CHECK_EQ(run(true, {0x2018, 0x2209}, 2), 1); // MOVE.L (A0)+,D0 / MOVE.L A1,D1
    CHECK_EQ(run(true, {0x08c0, 0x0003, 0xd280}, 2), 2); // BSET #3,D0 / ADD.L D0,D1
    // CMPM は 2 つの An を動かし、メモリを読む。LINK は An と A7 を書き、スタックに書く
    CHECK_EQ(run(true, {0xb388, 0x3008}, 2), 3); // CMPM.L (A0)+,(A1)+ / MOVE.W A0,D0
    CHECK_EQ(run(true, {0xb388, 0x3009}, 2), 3); // CMPM.L (A0)+,(A1)+ / MOVE.W A1,D0
    CHECK_EQ(run(true, {0xb388, 0x300a}, 2), 2); // CMPM.L (A0)+,(A1)+ / MOVE.W A2,D0
    CHECK_EQ(run(true, {0xb388, 0x2012}, 2), 3); // CMPM.L (A0)+,(A1)+ / MOVE.L (A2),D0 (メモリどうし)
    CHECK_EQ(run(true, {0x4e56, 0x0000, 0x2010}, 2), 3); // LINK.W A6,#0 / MOVE.L (A0),D0 (メモリどうし)
    CHECK_EQ(run(true, {0x4e56, 0x0000, 0x200e}, 2), 3); // LINK.W A6,#0 / MOVE.L A6,D0
    CHECK_EQ(run(true, {0x4e56, 0x0000, 0x2001}, 2), 2); // LINK.W A6,#0 / MOVE.L D1,D0
    // メモリの読み書きは 2 命令で 1 つまで (10.1.4)
    CHECK_EQ(run(true, {0x2010, 0x2211}, 2), 2); // MOVE.L (A0),D0 / MOVE.L (A1),D1
    CHECK_EQ(run(true, {0x2010, 0x2202}, 2), 1); // MOVE.L (A0),D0 / MOVE.L D2,D1
    // pOEP-only の命令 (MULU) の前後では同時に進まない。3 つめは 1 つめと組にならない
    CHECK_EQ(run(true, {0xc0c1, 0x7401}, 2), 3);          // MULU.W D1,D0 / MOVEQ #1,D2
    CHECK_EQ(run(true, {0x7201, 0x7401, 0x7601}, 3), 2);  // MOVEQ を 3 つ: 2 つが同時、3 つめは次のサイクル
    // pOEP-until-last (メモリからメモリの MOVE) の最後のサイクルに、次の命令が入る
    CHECK_EQ(run(true, {0x2290, 0x7401}, 2), 2); // MOVE.L (A0),(A1) / MOVEQ #1,D2
    CHECK_EQ(run(false, {0x2290, 0x7401}, 2), 3);
}

// 命令キャッシュ / データキャッシュ (CACR の EIC / EDC) が無効のとき: 命令の取り出し (ロングワードごと) とデータの読み書きが
// バスに出て、1 回 10.5 サイクル (本体のメモリ)、6 サイクル (ローカルメモリ) かかる (Cpu020::updateCacheCosts())
void testCacheOff(bool fast) {
    // si.r の processor performance のループを、実機と同じ並び ($xx78 から) に置く。base は置くアドレス
    auto loop = [fast](uint32_t cacr, uint32_t base, int iterations) {
        Env060 e(fast);
        e.cpu.pcr_ |= 1;
        setCacr(e, cacr);
        e.cpu.a_[0] = 0x3000;
        e.bus.w32(0x3000, 0x01000000);
        e.bus.put(base, {0x5280, 0x4a10, 0x66fa}); // ADDQ.L #1,D0 / TST.B (A0) / BNE
        e.cpu.pc_ = base;
        e.steps(3 * 4); // 分岐キャッシュに入るまで
        const uint64_t c0 = e.cpu.sub_;
        e.steps(3 * iterations);
        return (e.cpu.sub_ - c0) * 2 / m68k::Cpu020::SUB; // 半サイクル単位
    };
    // データの読み書きの分は、メモリ (TLB に入るページ) だったときだけ足す。fast でない試験のバスは Bus に出すので足さない
    // (I/O と VRAM と同じ扱い)。d は 1 回のデータの読み出しの半サイクル数
    const uint64_t d = fast ? 21 : 0;
    // すべて有効: 1 周 1 サイクル (実機 13348%)
    CHECK_EQ(loop(CACHE_ON | EBC, 0x1078, 10), (uint64_t)(2 * 10));
    // 命令 / データキャッシュが無効: 1 周に命令の取り出し 2 回 ($xx78 と $xx7C) とデータの読み出し 1 回で、
    // 1 + 3 x 10.5 = 32.5 サイクル (実機 410.38% / 413.36% は 1 周 約 32.4 サイクル)
    CHECK_EQ(loop(EBC, 0x1078, 10), (44 + d) * 10);
    // 命令キャッシュだけ無効: 1 + 2 x 10.5、データキャッシュだけ無効: 1 + 10.5
    CHECK_EQ(loop(EBC | 0x80000000u, 0x1078, 10), (uint64_t)(44 * 10));
    CHECK_EQ(loop(EBC | 0x00008000u, 0x1078, 10), (2 + d) * 10);
    // ループが 2 つのロングワードの境目から始まらない並び ($xx7A から) でも、1 周に取り出しは 2 回
    CHECK_EQ(loop(EBC, 0x107a, 10), (44 + d) * 10);
    {
        // 拡張語のある命令: MOVE.L #imm,D0 (6 バイト) を 2 つ。$1000、$1004、$1008 の 3 つのロングワードを読む
        Env060 e(fast);
        setCacr(e, 0x80000000u);
        e.bus.put(0x1000, {0x203c, 0x0000, 0x0001, 0x223c, 0x0000, 0x0002, 0x4e71});
        e.steps(2);
        const uint64_t c0 = e.cpu.sub_;
        e.steps(1); // NOP ($100C): 3 つめのロングワード ($1008) と 4 つめ ($100C) の 2 回 + NOP の 9
        CHECK_EQ((e.cpu.sub_ - c0) / m68k::Cpu020::SUB, 9u + 21u);
    }
    {
        // MOVEC で命令キャッシュを切った直後から、取り出しの時間を足す (分岐を待たない)。
        // $1000 MOVEC D0,CACR / $1004 から NOP: $1004 と $1008 のロングワードを読むので、NOP 4 つで 4 x 9 + 2 x 10.5
        Env060 e(fast);
        setCacr(e, CACHE_ON);
        e.cpu.d_[0] = 0x80000000u; // EDC だけ有効
        e.bus.put(0x1000, {0x4e7b, 0x0002, 0x4e71, 0x4e71, 0x4e71, 0x4e71, 0x4e71});
        e.steps(1);
        CHECK_EQ(e.cpu.cacr_, 0x80000000u);
        const uint64_t c0 = e.cpu.sub_;
        e.steps(4);
        CHECK_EQ((e.cpu.sub_ - c0) / m68k::Cpu020::SUB, 4u * 9 + 21);
    }
    {
        // 分岐の命令の拡張語の分: $1000 に置いた 6 バイトの BRA.L 自分は、1 周に 2 つのロングワードを読む。
        // 命令キャッシュも分岐キャッシュも無効なら 3 + 2 x 10.5 = 24 サイクル
        Env060 e(fast);
        setCacr(e, 0x80000000u);
        e.bus.put(0x1000, {0x60ff, 0xffff, 0xfffe});
        e.steps(4);
        const uint64_t c0 = e.cpu.sub_;
        e.steps(10);
        CHECK_EQ((e.cpu.sub_ - c0) / m68k::Cpu020::SUB, 240u);
    }
    if (fast) {
        // ローカルメモリ ($10000000 から) のデータ: 1 回 6 サイクル
        Env060 e(fast);
        static uint8_t local[0x10000];
        e.cpu.setExtMemory(0x10000000, sizeof local, local);
        setCacr(e, 0x00008000u);
        e.cpu.a_[0] = 0x10003000;
        CHECK_EQ(cyclesOf(e, {0x2010}, 1), 1 + 6); // MOVE.L (A0),D0
        e.cpu.a_[0] = 0x3000;
        e.cpu.pc_ = 0x1000;
        CHECK_EQ(e.cpu.step(), 1 + 10); // 本体のメモリ: 10.5 (端数は次の命令に持ち越す)
    }
}

// 速さ (Cpu020::setSpeed): 68060 の 100 は表どおり、50 で 2 倍のサイクル数、200 で半分。命令の結果は変わらない
void testSpeed060(bool fast) {
    uint32_t want[8] = {};
    for (int speed : {100, 50, 200, 5, 3200}) {
        for (int ess = 0; ess < 2; ess++) {
            Env060 e(fast);
            e.cpu.setSpeed(speed);
            CHECK_EQ(e.cpu.speed(), speed);
            if (ess) e.cpu.pcr_ |= 1;
            setCacr(e, CACHE_ON | EBC);
            e.cpu.a_[0] = 0x3000;
            // MOVEQ #100,D0 / loop: ADD.L D0,D1 / MULU.W D0,D2 / ADDQ.L #3,D2 / MOVE.L D1,(A0) / ADD.L (A0),D3 /
            // DBRA D0,loop / DIVU.W #7,D3
            e.bus.put(0x1000, {0x7064, 0xd280, 0xc4c0, 0x5682, 0x2081, 0xd690, 0x51c8, 0xfff4, 0x86fc, 0x0007});
            uint64_t before = e.cpu.cycles();
            e.steps(1 + 6 * 101 + 1);
            const uint64_t c = e.cpu.cycles() - before;
            CHECK_EQ(e.cpu.pc_, 0x1014u);
            if (speed == 100 && !ess) {
                for (int i = 0; i < 8; i++) want[i] = e.cpu.d_[i];
                // MOVEQ 1 + 101 周 x (ADD 1 + MULU 2 + ADDQ 1 + MOVE 1 + ADD 1) + DBRA (3 + 99 x 1 + 7) + DIVU 22
                CHECK_EQ(c, (uint64_t)(1 + 101 * 6 + 3 + 99 + 7 + 22));
            }
            for (int i = 0; i < 8; i++) CHECK_EQ(e.cpu.d_[i], want[i]);
            if (!ess) {
                const uint64_t base = 1 + 101 * 6 + 3 + 99 + 7 + 22;
                // 1 サイクルは SUB * 100 / speed (切り捨て) なので、3200 では表の値より少し短い
                const uint64_t unit = (uint64_t)m68k::Cpu020::SUB * 100 / (uint64_t)speed;
                CHECK_EQ(c, base * unit / m68k::Cpu020::SUB);
            }
        }
    }
    // 68030 は tick060() で何も足さない (68060 の表を使わない)
    Env e30(fast);
    e30.bus.put(0x1000, {0x7001});
    const int c30 = e30.cpu.step();
    Env e30b(fast);
    e30b.cpu.setSpeed(100);
    e30b.bus.put(0x1000, {0x7001});
    CHECK_EQ(e30b.cpu.step(), c30);
    CHECK_EQ(c30, 2); // MOVEQ: 68000 の 4 サイクルの 3/5 (端数は次の命令に持ち越す)
}

// run() の繰り返し (検査を省く経路。Cpu020::runLoop()) と step() (命令ごとに検査を通す経路) で、サイクル数と状態が同じになる。
// 命令の途中で数え方が変わる所を通す (#224): MOVEC で PCR の ESS を立てる / 消す、CACR の EIC を立てる / 消す
// (命令キャッシュが無効の間、run() は取り出しの時間を足す経路 ATTN_FETCH を使う)、表のサイクル数が 0 の命令
// (レジスタの数が 0 の MOVEM)、同時実行の組、分岐。リセットのあとは命令キャッシュが無効、ESS は 0
void testRunMatchesStep(bool fast) {
    const uint32_t kBody = 0x1100, kEnd = 0x102e;
    auto prepare = [&](Env060& e) {
        // 本体: 4 周するループ (DBRA) のあと、分岐しない Bcc と NOP、RTS
        e.bus.put(kBody, {0x7a03,                         // MOVEQ #3,D5
                          0x5280, 0x4a10, 0xd280, 0x2810, // ADDQ.L #1,D0 / TST.B (A0) / ADD.L D0,D1 / MOVE.L (A0),D4
                          0x48e7, 0x0000,                 // MOVEM.L #0,-(A7) (レジスタの数が 0)
                          0x51cd, 0xfff2,                 // DBRA D5,ループの先頭
                          0x6702, 0x4e71, 0x4e75});       // BEQ.S (分岐しない) / NOP / RTS
        e.bus.put(0x1000, {0x6100, 0x00fe,                // BSR.W 本体 (命令キャッシュ無効、ESS 0)
                           0x4e7b, 0x2808,                // MOVEC D2,PCR: ESS を立てる
                           0x6100, 0x00f6,                // BSR.W 本体
                           0x4e7b, 0x6002,                // MOVEC D6,CACR: 命令 / データキャッシュと分岐キャッシュを有効にする
                           0x6100, 0x00ee,
                           0x7000, 0x7201, 0xd280, 0x5281, // MOVEQ #0,D0 / MOVEQ #1,D1 / ADD.L D0,D1 / ADDQ.L #1,D1
                           0x4e7b, 0x3808,                // MOVEC D3,PCR: ESS を消す
                           0x6100, 0x00de,
                           0x4e7b, 0x7002,                // MOVEC D7,CACR: 命令キャッシュを無効にする
                           0x6100, 0x00d6,
                           0x4e71,                        // NOP (9 サイクル。ここで run(1) が必ず区切れる)
                           0x60fe});                      // kEnd: BRA.S *
        e.cpu.a_[0] = 0x3000;
        e.bus.w32(0x3000, 0x01000000);
        e.cpu.d_[2] = e.cpu.pcr_ | 1;
        e.cpu.d_[3] = e.cpu.pcr_ & ~1u;
        e.cpu.d_[6] = CACHE_ON | EBC;
        e.cpu.d_[7] = EBC;
    };
    Env060 a(fast), b(fast);
    prepare(a);
    prepare(b);
    int n = 0;
    for (; n < 1000 && a.cpu.pc_ != kEnd; n++) a.cpu.step();
    CHECK_EQ(a.cpu.pc_, kEnd);
    b.cpu.run(1); // 最初の命令は検査を通る。そのあとは検査を省く
    bool fetchPath = false, fastPath = false;
    for (int i = 0; i < 1000 && b.cpu.pc_ != kEnd; i++) {
        fetchPath = fetchPath || b.cpu.attn_ == m68k::Cpu020::ATTN_FETCH;
        fastPath = fastPath || b.cpu.attn_ == 0;
        b.cpu.run(1);
    }
    CHECK_EQ(b.cpu.pc_, kEnd);
    CHECK_EQ(fetchPath, true); // 命令キャッシュが無効の間
    CHECK_EQ(fastPath, true);  // 有効の間
    CHECK_EQ(b.cpu.sub_, a.cpu.sub_);
    for (int i = 0; i < 8; i++) {
        CHECK_EQ(b.cpu.d_[i], a.cpu.d_[i]);
        CHECK_EQ(b.cpu.a_[i], a.cpu.a_[i]);
    }
    CHECK_EQ(b.cpu.pairLead_, a.cpu.pairLead_);
    CHECK_EQ(b.cpu.pairDest_, a.cpu.pairDest_);
    CHECK_EQ(b.cpu.foldOk060_, a.cpu.foldOk060_);
    CHECK_EQ(b.cpu.fetchLong_, a.cpu.fetchLong_);
    CHECK_EQ(b.cpu.fetchValid_, a.cpu.fetchValid_);
    CHECK_EQ(b.cpu.pcr_, a.cpu.pcr_);
    CHECK_EQ(b.cpu.cacr_, a.cpu.cacr_);
}

// 分岐キャッシュ: 空の状態、アドレスの全部のビット、CACR の CABC / CUBC (MC68060UM 5.2、5.11)
void testBranchCache(bool fast) {
    const uint32_t CABC = 0x00400000u, CUBC = 0x00200000u;
    {
        // 空の分岐キャッシュは、どのアドレスの分岐も「入っていない」。$00000000 に置いた BRA.S 自分の 1 回めは、
        // 予測なしの 3 サイクル (予測が外れた 7 サイクルではない)。2 回めからは予測が当たる
        Env060 e(fast);
        setCacr(e, CACHE_ON | EBC);
        e.bus.put(0x0000, {0x60fe});
        e.cpu.pc_ = 0;
        CHECK_EQ(e.cpu.step(), 3);
        CHECK_EQ(e.cpu.step(), 1);
    }
    {
        // 256 個の表の、すべての位置で同じ: 空なら 3、2 回めは当たる (分岐だけなので 1)
        Env060 e(fast);
        setCacr(e, CACHE_ON | EBC);
        int bad = 0;
        for (uint32_t i = 0; i < 256; i++) {
            const uint32_t at = 0x2000 + i * 2;
            e.bus.put(at, {0x60fe});
            e.cpu.pc_ = at;
            if (e.cpu.step() != 3) bad++;
            if (e.cpu.step() != 1) bad++;
        }
        CHECK_EQ(bad, 0);
        // 表の同じ位置に入る別のアドレス ($2000 と $2200) は、互いを追い出す: $2200 で覚えたあと、$2000 は予測なしに戻る
        e.bus.put(0x2200, {0x60fe});
        e.cpu.pc_ = 0x2200;
        CHECK_EQ(e.cpu.step(), 3);
        e.cpu.pc_ = 0x2000;
        CHECK_EQ(e.cpu.step(), 3);
    }
    if (!fast) {
        // アドレスは全部のビットを比べる。$00001000 で覚えた分岐を、上位のビットだけが違うアドレス (試験のバスは 24 ビットで
        // 折り返すので、同じ命令が読める) では使わない
        for (uint32_t high : {0x40000000u, 0x80000000u, 0xc0000000u, 0x01000000u}) {
            Env060 e(fast);
            setCacr(e, CACHE_ON | EBC);
            e.bus.put(0x1000, {0x60fe});
            e.steps(4);
            CHECK_EQ(e.cpu.step(), 1);
            e.cpu.pc_ = high | 0x1000;
            CHECK_EQ(e.cpu.step(), 3);
            CHECK_EQ(e.cpu.pc_, high | 0x1000);
            CHECK_EQ(e.cpu.step(), 1);
        }
    }
    {
        // CABC: MOVEC D0,CACR で CABC を書くと、覚えた分岐を全部忘れる。CABC は CACR に残らない (読むと 0)
        Env060 e(fast);
        setCacr(e, CACHE_ON | EBC);
        e.bus.put(0x1000, {0x60fe});
        e.bus.put(0x2000, {0x4e7b, 0x0002, 0x4e7a, 0x1002}); // MOVEC D0,CACR / MOVEC CACR,D1
        e.steps(4);
        CHECK_EQ(e.cpu.step(), 1);
        e.cpu.d_[0] = CACHE_ON | EBC | CABC;
        e.cpu.pc_ = 0x2000;
        e.steps(2);
        CHECK_EQ(e.cpu.d_[1], CACHE_ON | EBC);
        e.cpu.pc_ = 0x1000;
        CHECK_EQ(e.cpu.step(), 3);
        // CABC も CUBC も書かなければ、覚えたまま (MOVEC のあとなので、分岐は MOVEC に折りたたまれて 0)
        e.steps(3);
        e.cpu.d_[0] = CACHE_ON | EBC;
        e.cpu.pc_ = 0x2000;
        e.steps(2);
        e.cpu.pc_ = 0x1000;
        CHECK_EQ(e.cpu.step(), 0);
    }
    {
        // CUBC: ユーザモードで覚えた分岐だけを忘れる。$1000 の BRA はユーザモードで、$1100 の BRA はスーパーバイザモードで覚える
        Env060 e(fast);
        setCacr(e, CACHE_ON | EBC);
        e.bus.put(0x1000, {0x60fe});
        e.bus.put(0x1100, {0x60fe});
        e.bus.put(0x2000, {0x4e7b, 0x0002});
        e.cpu.setSR(0x0000);
        e.cpu.pc_ = 0x1000;
        e.steps(4);
        CHECK_EQ(e.cpu.step(), 1);
        e.cpu.setSR(0x2700);
        e.cpu.pc_ = 0x1100;
        e.steps(4);
        CHECK_EQ(e.cpu.step(), 1);
        e.cpu.d_[0] = CACHE_ON | EBC | CUBC;
        e.cpu.pc_ = 0x2000;
        e.steps(1);
        e.cpu.pc_ = 0x1100; // スーパーバイザモードで覚えた分岐は残る (MOVEC に折りたたまれて 0)
        CHECK_EQ(e.cpu.step(), 0);
        e.cpu.setSR(0x0000);
        e.cpu.pc_ = 0x1000; // ユーザモードで覚えた分岐は消えている
        CHECK_EQ(e.cpu.step(), 3);
    }
    {
        // 分岐キャッシュが無効 (EBC = 0) の間は覚えない: EBC を 1 にした直後も予測なし
        Env060 e(fast);
        setCacr(e, CACHE_ON);
        e.bus.put(0x1000, {0x60fe});
        e.steps(5);
        setCacr(e, CACHE_ON | EBC);
        CHECK_EQ(e.cpu.step(), 3);
        CHECK_EQ(e.cpu.step(), 1);
    }
}

// 命令と命令の間の状態 (同時実行の相手、分岐の折りたたみ、命令の取り出しの位置) の移り方
void testTimingState(bool fast) {
    auto env = [fast](Env060& e) {
        (void)fast;
        e.cpu.pcr_ |= 1;
        setCacr(e, CACHE_ON | EBC);
        e.cpu.a_[0] = 0x3000;
        e.cpu.fpu()->fp_[0] = kOne;
        e.cpu.fpu()->fp_[1] = kOne;
    };
    {
        // 表のサイクル数が 0 の命令 (MOVEM: レジスタの数の分を命令の中で足す) を越えて、同時実行の状態が残らない:
        // MOVEQ #1,D0 / MOVEM.L (A0),D1 / ADD.L D1,D2 は 1 + 1 + 1 (最初と最後が組にならない)
        Env060 e(fast);
        env(e);
        e.bus.put(0x1000, {0x7001, 0x4cd0, 0x0002, 0xd481});
        CHECK_EQ(e.cpu.step(), 1);
        CHECK_EQ(e.cpu.step(), 1);
        CHECK_EQ(e.cpu.step(), 1);
    }
    {
        // FPU の命令を越えても残らない: MOVEQ #1,D0 / FADD.X FP1,FP0 / MOVEQ #2,D1 は 1 + 3 + 1
        Env060 e(fast);
        env(e);
        e.bus.put(0x1000, {0x7001, 0xf200, 0x0422, 0x7202});
        CHECK_EQ(e.cpu.step(), 1);
        CHECK_EQ(e.cpu.step(), 3);
        CHECK_EQ(e.cpu.step(), 1);
    }
    {
        // FPU の命令のあとの分岐は折りたためる: FADD.X FP1,FP0 / BRA.S のループは、予測が当たれば 3 + 0
        Env060 e(fast);
        env(e);
        e.bus.put(0x1000, {0xf200, 0x0422, 0x60fa});
        e.steps(20);
        const uint64_t c0 = e.cpu.cycles();
        e.steps(200);
        CHECK_EQ(e.cpu.cycles() - c0, 300u);
    }
    {
        // MOVEM のあとの分岐も折りたためる。レジスタの数が 0 の MOVEM は 1 サイクルとする (時間が止まらない):
        // MOVEM.L (レジスタなし),(A0) / BRA.S のループは 1 + 0
        Env060 e(fast);
        env(e);
        e.bus.put(0x1000, {0x48d0, 0x0000, 0x60fa});
        e.steps(20);
        const uint64_t c0 = e.cpu.cycles();
        e.steps(200);
        CHECK_EQ(e.cpu.cycles() - c0, 100u);
        const uint64_t c1 = e.cpu.cycles();
        e.cpu.run(300);
        CHECK_EQ(e.cpu.cycles() - c1 >= 300 && e.cpu.cycles() - c1 < 310, true);
    }
    {
        // リセットで、同時実行の相手、折りたたみ、分岐キャッシュを消す。ESS を有効にして MOVEQ を実行してからリセットすると、
        // 次の MOVEQ は 1 サイクル (リセットで PCR の ESS は 0。前の MOVEQ と組にならない)
        Env060 e(fast);
        env(e);
        e.bus.put(0x1000, {0x7001, 0x7001, 0x60fe});
        CHECK_EQ(e.cpu.step(), 1);
        e.cpu.reset();
        CHECK_EQ(e.cpu.pcr_ & 1, 0u);
        setCacr(e, CACHE_ON | EBC);
        CHECK_EQ(e.cpu.step(), 1);
        e.cpu.step();
        // BRA.S 自分を予測させてからリセットすると、次は予測なしの 3
        e.steps(5);
        CHECK_EQ(e.cpu.step(), 1);
        e.cpu.reset();
        setCacr(e, CACHE_ON | EBC);
        e.cpu.pc_ = 0x1004;
        CHECK_EQ(e.cpu.step(), 3);
    }
    {
        // 例外のあと、ハンドラの先頭が予測済みの BRA でも 0 サイクルにならない (折りたたむ相手がない):
        // $1000 TRAP #0 / $1002 BRA.S $1000、ハンドラ $2000 BRA.S $2004 / $2004 RTE
        // TRAP 19 + BRA 1 + RTE 17 + BRA 0 (RTE に折りたたむ)
        Env060 e(fast);
        env(e);
        e.bus.w32(32 * 4, 0x2000);
        e.bus.put(0x1000, {0x4e40, 0x60fc});
        e.bus.put(0x2000, {0x6002, 0x4e71, 0x4e73});
        e.steps(40);
        const uint64_t c0 = e.cpu.cycles();
        e.steps(400);
        CHECK_EQ(e.cpu.cycles() - c0, 100u * (19 + 1 + 17 + 0));
    }
    {
        // 割り込みのあとも同じ: レベル 1 の割り込み (オートベクタ 25) をかけたままにし、ハンドラ $2000 BRA.S $2004 / $2004 RTE。
        // RTE のたびにまた受け付けるので、割り込み 23 + BRA 1 + RTE 17 の繰り返し
        Env060 e(fast);
        env(e);
        e.bus.w32(25 * 4, 0x2000);
        e.bus.put(0x1000, {0x4e71, 0x4e71});
        e.bus.put(0x2000, {0x6002, 0x4e71, 0x4e73});
        e.cpu.setSR(0x2000);
        e.cpu.setIPL(1);
        e.steps(30);
        const uint64_t c0 = e.cpu.cycles();
        e.steps(300);
        CHECK_EQ(e.cpu.cycles() - c0, 100u * (23 + 1 + 17));
    }
    // 命令キャッシュを切った直後の最初の取り出しは、アドレスによらず 1 回分 (10.5 サイクル)。MOVEC D0,CACR を $0000、
    // $0008、$0014 に置き、次の NOP ($0004、$000C、$0018) が 9 + 10.5 サイクルになることを見る
    for (uint32_t at : {0x0000u, 0x0008u, 0x0014u, 0x1000u}) {
        Env060 e(fast);
        setCacr(e, CACHE_ON);
        e.cpu.d_[0] = 0x80000000u; // EDC だけ有効
        e.bus.put(at, {0x4e7b, 0x0002, 0x4e71, 0x4e71});
        e.cpu.pc_ = at;
        e.steps(1);
        const uint64_t c0 = e.cpu.sub_;
        e.steps(1);
        CHECK_EQ((e.cpu.sub_ - c0) * 2 / m68k::Cpu020::SUB, 2u * 9 + 21);
    }
}

// 命令の並びをランダムに作り、続けて実行したときの命令ごとのサイクル数を、「1 命令ずつ、数え方の状態を消してから
// 実行したときのサイクル数」と表の規則から計算した値と比べる。命令と命令の間で変わってよいのは次の 2 つだけ:
//   - 前の命令が相手にできる命令 (T_LEAD) で、この命令が sOEP に入れる命令 (T_PAIR) で、メモリの読み書きが重ならず、
//     前の命令が書くレジスタをこの命令が使わないとき、この命令は 0 サイクル (同時実行)。相手にされた命令のあとは、
//     もう相手にできない
//   - 予測が当たった BRA は、前の分岐のあとに分岐でない命令があれば 0 サイクル、なければ 1 サイクル (折りたたみ)
// それ以外の命令 (MOVEM、FPU、複数サイクルの命令) は、前後が何であっても 1 命令で実行したときと同じサイクル数になる
void testRandomSequences(bool fast) {
    struct Item {
        std::initializer_list<uint16_t> words;
    };
    // 分岐は BRA.S $+4 (次の 1 語を飛ばす) だけ。フラグやレジスタの値でサイクル数が変わる命令 (Bcc、DBcc、DIV) は入れない
    static const Item pool[] = {
        {{0x7001}},                 // MOVEQ #1,D0
        {{0x7203}},                 // MOVEQ #3,D1
        {{0xd480}},                 // ADD.L D0,D2
        {{0xd681}},                 // ADD.L D1,D3
        {{0x2401}},                 // MOVE.L D1,D2
        {{0x2010}},                 // MOVE.L (A0),D0
        {{0x2211}},                 // MOVE.L (A1),D1
        {{0x2480}},                 // MOVE.L D0,(A2)
        {{0x2490}},                 // MOVE.L (A0),(A2)
        {{0x2618}},                 // MOVE.L (A0)+,D3
        {{0x2608}},                 // MOVE.L A0,D3
        {{0x5488}},                 // ADDQ.L #2,A0
        {{0x5588}},                 // SUBQ.L #2,A0
        {{0x43e8, 0x0004}},         // LEA (4,A0),A1
        {{0x4a92}},                 // TST.L (A2)
        {{0xe38a}},                 // LSL.L #1,D2
        {{0x50c3}},                 // ST D3
        {{0x4843}},                 // SWAP D3
        {{0xc4c1}},                 // MULU.W D1,D2
        {{0x0682, 0x0000, 0x0001}}, // ADDI.L #1,D2
        {{0x0692, 0x0000, 0x0001}}, // ADDI.L #1,(A2)
        {{0xb5ca}},                 // CMPA.L A2,A2
        {{0x4cd2, 0x0030}},         // MOVEM.L (A2),D4-D5
        {{0x48d2, 0x0030}},         // MOVEM.L D4-D5,(A2)
        {{0x48d2, 0x0000}},         // MOVEM.L (レジスタなし),(A2)
        {{0xf200, 0x0422}},         // FADD.X FP1,FP0
        {{0xf200, 0x0400}},         // FMOVE.X FP1,FP0
        {{0x4e71}},                 // NOP
        {{0x6002, 0x4e71}},         // BRA.S $+4
        {{0x6002, 0x4e71}},         // (分岐を多めに)
    };
    const int poolSize = (int)(sizeof pool / sizeof pool[0]);
    uint32_t rng = 12345;
    auto next = [&rng]() {
        rng = rng * 1664525u + 1013904223u;
        return rng >> 8;
    };
    int bad = 0;
    for (int trial = 0; trial < 400; trial++) {
        Env060 e(fast);
        e.cpu.pcr_ |= 1;
        setCacr(e, CACHE_ON | EBC);
        e.cpu.fpu()->fp_[0] = kOne;
        e.cpu.fpu()->fp_[1] = kZero;
        // 並びを $1000 から置き、最後に BRA.W で先頭に戻る
        const int n = 6 + (int)(next() % 6);
        uint16_t first[16];
        uint32_t at = 0x1000;
        for (int i = 0; i < n; i++) {
            const Item& it = pool[next() % (uint32_t)poolSize];
            first[i] = *it.words.begin();
            e.bus.put(at, it.words);
            at += 2 * (uint32_t)it.words.size();
        }
        e.bus.put(at, {0x6000, (uint16_t)(0x1000 - (at + 2))});
        auto regs = [&e]() {
            e.cpu.a_[0] = 0x3000;
            e.cpu.a_[1] = 0x3100;
            e.cpu.a_[2] = 0x3200;
        };
        // 1 命令ずつ、数え方の状態を消してから実行したときのサイクル数
        int alone[16];
        regs();
        e.cpu.pc_ = 0x1000;
        for (int i = 0; i < n; i++) {
            e.cpu.clearTiming060();
            alone[i] = e.cpu.step();
        }
        e.cpu.clearTiming060();
        e.cpu.step(); // 最後の BRA.W
        // 続けて実行する。分岐キャッシュに入るまで 3 周回してから、4 周めを比べる
        for (int lap = 0; lap < 3; lap++) {
            regs();
            e.steps(n + 1);
        }
        regs();
        // 規則から計算した値。4 周めの始めの状態は、3 周めの最後の BRA.W (分岐) のあと
        bool lead = false, leadMem = false, foldOk = false;
        uint32_t leadWr = 0;
        for (int i = 0; i < n; i++) {
            const uint16_t op = first[i];
            const uint16_t t = Cpu020::cyc060(op);
            int want;
            if (t & 0x2000) { // T_BRANCH (BRA.S)
                want = foldOk ? 0 : 1;
                foldOk = false;
                lead = false;
            } else {
                const bool mem = (t & 0x1000) != 0;
                if ((t & 0x0400) && lead && !(Cpu020::use060(op) & leadWr) && !(mem && leadMem)) {
                    want = 0;
                    lead = false;
                } else {
                    want = alone[i];
                    lead = (t & 0x0800) != 0;
                    leadMem = mem;
                    leadWr = Cpu020::wr060(op);
                }
                foldOk = true;
            }
            const int got = e.cpu.step();
            if (got != want) {
                if (bad < 20) {
                    printf("  trial %d, instruction %d (%04x): %d cycles, want %d (alone %d). sequence:", trial, i, op, got, want, alone[i]);
                    for (int k = 0; k < n; k++) printf(" %04x", first[k]);
                    printf("\n");
                }
                bad++;
            }
        }
    }
    CHECK_EQ(bad, 0);
}

// ---- 同時実行の表 (ops060_ の cyc060() / use060() / wr060()) を、実際に実行した結果と比べる ----
//
// 表は「メモリの読み書きがあるか」「どのレジスタを書くか」「どのレジスタを使うか」をオペコードの 1 語めから決めている。
// 書き漏らしがあると、同時に進まないはずの 2 命令が同時に進む。ここでは、同時実行に関わるすべてのオペコード
// (T_LEAD か T_PAIR が立っているもの) を 1 命令ずつ実行して、次のことを確かめる:
//   1. データの読み書きをしたら T_MEM が立っている。T_MEM が立っていたら、データの読み書きをしている
//   2. 値が変わったレジスタは、wr060() に入っている (T_LEAD の命令)
//   3. use060() に入っていないレジスタの値を変えても、結果 (ほかのレジスタ、CCR、PC、メモリへの書き込み) が変わらない
//      (T_PAIR の命令)
// 例外になったオペコードは見ない。表が見ないと決めているもの (インデックスレジスタ) は 3. から外す。
struct LogBus : m68k::Bus {
    std::vector<uint8_t> mem;
    struct Write {
        uint32_t addr;
        uint8_t old, val;
    };
    std::vector<Write> writes;
    int dataAccess = 0;
    LogBus() : mem(16 << 20) {}
    void note(int fc) {
        if ((fc & 3) == 1) dataAccess++;
    }
    void put(uint32_t a, uint8_t v) {
        a &= 0xffffff;
        writes.push_back({a, mem[a], v});
        mem[a] = v;
    }
    uint8_t read8(uint32_t a, int fc) override {
        note(fc);
        return mem[a & 0xffffff];
    }
    uint16_t read16(uint32_t a, int fc) override {
        note(fc);
        return (uint16_t)((mem[a & 0xffffff] << 8) | mem[(a + 1) & 0xffffff]);
    }
    void write8(uint32_t a, uint8_t v, int fc) override {
        note(fc);
        put(a, v);
    }
    void write16(uint32_t a, uint16_t v, int fc) override {
        note(fc);
        put(a, (uint8_t)(v >> 8));
        put(a + 1, (uint8_t)v);
    }
    void undo() {
        for (size_t i = writes.size(); i-- > 0;) mem[writes[i].addr] = writes[i].old;
        writes.clear();
    }
};

struct Outcome {
    uint32_t d[8], a[8], pc;
    uint16_t sr;
    int dataAccess;
    bool exception;
    std::vector<LogBus::Write> writes;
};

constexpr uint32_t TABLE_HANDLER = 0x9000;

// op を 1 命令実行する。seed で初めの値を変え、perturb が 0 以上ならそのレジスタ (0-7 は Dn、8-15 は An) の値をずらす
Outcome runOne(Cpu020& cpu, LogBus& bus, uint16_t op, int seed, int perturb, uint32_t* before) {
    bus.undo();
    for (int i = 0; i < 8; i++) {
        // Dn の下位語は小さい偶数 (インデックスに使っても範囲の中)、An は偶数で、まわりにメモリがある
        cpu.d_[i] = 0x11110000u * (uint32_t)(i + 1 + seed) + 0x10u * (uint32_t)(i + 1) + (uint32_t)seed * 2;
        cpu.a_[i] = 0x20000u + 0x1000u * (uint32_t)i + (uint32_t)seed * 0x100;
    }
    if (perturb >= 0) {
        if (perturb < 8) cpu.d_[perturb] ^= 0x01010140u;
        else cpu.a_[perturb - 8] += 0x40;
    }
    for (int i = 0; i < 8; i++) {
        before[i] = cpu.d_[i];
        before[8 + i] = cpu.a_[i];
    }
    cpu.setSR((uint16_t)(0x2700 | (seed ? 0x1f : 0)));
    cpu.isp_ = cpu.a_[7];
    cpu.pc_ = 0x1000;
    cpu.pcr_ |= 1;
    uint8_t* m = bus.mem.data();
    m[0x1000] = (uint8_t)(op >> 8);
    m[0x1001] = (uint8_t)op;
    for (int i = 1; i < 6; i++) { // 拡張語: ディスプレースメント、イミディエイト、絶対アドレス、インデックス (D0.W) のどれでも通る値
        m[0x1000 + 2 * i] = 0x00;
        m[0x1001 + 2 * i] = 0x10;
    }
    bus.dataAccess = 0;
    cpu.step();
    Outcome o;
    for (int i = 0; i < 8; i++) {
        o.d[i] = cpu.d_[i];
        o.a[i] = cpu.a_[i];
    }
    o.pc = cpu.pc_;
    o.sr = cpu.getSR();
    o.dataAccess = bus.dataAccess;
    o.exception = cpu.pc_ == TABLE_HANDLER || cpu.halted() || cpu.stopped();
    o.writes = bus.writes;
    return o;
}

bool sameExcept(const Outcome& x, const Outcome& y, int skip) {
    for (int i = 0; i < 8; i++) {
        if (i != skip && x.d[i] != y.d[i]) return false;
        if (8 + i != skip && x.a[i] != y.a[i]) return false;
    }
    if (x.pc != y.pc || x.sr != y.sr || x.writes.size() != y.writes.size()) return false;
    for (size_t i = 0; i < x.writes.size(); i++)
        if (x.writes[i].addr != y.writes[i].addr || x.writes[i].val != y.writes[i].val) return false;
    return true;
}

void testTableAgainstExecution() {
    static LogBus bus;
    for (size_t i = 0; i < bus.mem.size(); i++) bus.mem[i] = (uint8_t)(i * 7 + (i >> 8) * 3 + 1);
    auto w32 = [&](uint32_t a, uint32_t v) {
        for (int i = 0; i < 4; i++) bus.mem[a + (uint32_t)i] = (uint8_t)(v >> (24 - 8 * i));
    };
    for (int v = 0; v < 256; v++) w32((uint32_t)v * 4, TABLE_HANDLER);
    w32(0, 0x8000);
    w32(4, 0x1000);
    static Cpu020 cpu(&bus, m68k::CpuModel::M68060);
    cpu.reset();
    cpu.cacr_ = 0x80808000u;
    cpu.updateCacheCosts();
    int checked = 0, bad = 0;
    auto fail = [&](uint16_t op, const char* what, int reg) {
        if (bad < 40) {
            if (reg >= 0) printf("  %04x: %s %c%d\n", op, what, reg < 8 ? 'D' : 'A', reg & 7);
            else printf("  %04x: %s\n", op, what);
        }
        bad++;
    };
    for (int opi = 0; opi < 65536; opi++) {
        const uint16_t op = (uint16_t)opi;
        const uint16_t t = Cpu020::cyc060(op);
        const bool lead = (t & 0x0800) != 0, pair = (t & 0x0400) != 0; // T_LEAD、T_PAIR
        if (!lead && !pair) continue;
        const uint32_t use = Cpu020::use060(op), wr = Cpu020::wr060(op);
        const uint32_t useRegs = (use & 0xffff) | ((use >> 16 & 0xff) << 8);
        const uint32_t wrRegs = (wr & 0xffff) | ((wr >> 16 & 0xff) << 8);
        // MOVE.L <ea>,Dn は、書く Dn を wr060() に入れない (後の命令が演算に使っても同時に進む。10.1.6 の例外 1)
        const bool moveLongToDn = (op >> 12) == 2 && ((op >> 6) & 7) == 0;
        // インデックスつきの実効アドレス (拡張語のインデックスは D0)。表はインデックスレジスタを見ない
        const int mode = (op >> 3) & 7, dmode = (op >> 6) & 7;
        const bool indexed = mode == 6 || ((op >> 12) >= 1 && (op >> 12) <= 3 && dmode == 6);
        for (int seed = 0; seed < 2; seed++) {
            uint32_t before[16], scratch[16];
            const Outcome base = runOne(cpu, bus, op, seed, -1, before);
            if (base.exception) break;
            if (seed == 0) checked++;
            if (base.dataAccess > 0 && !(t & 0x1000)) fail(op, "reads or writes memory, but T_MEM is not set", -1);
            if (base.dataAccess == 0 && (t & 0x1000)) fail(op, "T_MEM is set, but no memory access", -1);
            if (lead) {
                for (int r = 0; r < 16; r++) {
                    const uint32_t after = r < 8 ? base.d[r] : base.a[r - 8];
                    if (after == before[r] || (wrRegs >> r & 1)) continue;
                    if (moveLongToDn && r == ((op >> 9) & 7)) continue;
                    fail(op, "changes a register that is not in wr060():", r);
                }
            }
            if (pair) {
                for (int r = 0; r < 16; r++) {
                    if (useRegs >> r & 1) continue;
                    if (indexed && r == 0) continue;
                    const Outcome o = runOne(cpu, bus, op, seed, r, scratch);
                    if (o.exception || !sameExcept(base, o, r)) fail(op, "depends on a register that is not in use060():", r);
                }
            }
        }
    }
    bus.undo();
    printf("table vs execution: %d opcodes checked, %d mismatches\n", checked, bad);
    CHECK_EQ(bad, 0);
    CHECK_EQ(checked > 20000, true);
}

} // namespace

int main() {
    testFaultSweep();
    for (int fast = 0; fast < 2; fast++) {
        testUnimplementedInteger(fast);
        testFpuUnimplemented(fast);
        testUnsupportedData(fast);
        testPendingExceptions(fast);
        testEnabledExceptions(fast);
        testFpuDisabled(fast);
        testAccessFaultUndo(fast);
        testRestartAddx(fast);
        testOddInterruptVector(fast);
        testFaultInfo(fast);
        testFscc(fast);
        testTraceFmoveOut(fast);
        testMmu(fast);
        testMovesAlt(fast);
        testTrapExceptions(fast);
        testRteOddPc(fast);
        testTtrWriteProtect(fast);
        testMovesNoTranslate(fast);
        testChkDivFlags(fast);
        testSetRegistersAfterHalt(fast);
        testCycleTable(fast);
        testFpuNanCycles(fast);
        testBranchCycles(fast);
        testBranchOnlyLoop(fast);
        testSuperscalar(fast);
        testCacheOff(fast);
        testRunMatchesStep(fast);
        testBranchCache(fast);
        testTimingState(fast);
        testRandomSequences(fast);
        testSpeed060(fast);
    }
    testTableAgainstExecution();
    return finish();
}
