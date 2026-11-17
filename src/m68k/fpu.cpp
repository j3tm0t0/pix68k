// MC68882 の命令。fpu.h を参照
#include "m68k/fpu.h"

#include "m68k/cpu020.h"

namespace m68k {

using fp::X;
using fp::Ctx;

namespace {

// FSAVE の IDLE のフレームの BIU フラグ (保留なし)
const uint32_t kBiuIdle = 0x7c00ffffu;

// 68060 の FSxxx / FDxxx (opmode $40-$6F) を、精度を決めない演算の opmode にする。prec に丸めの精度を返す。
// どれでもなければ -1
int fsdBase(int opmode, int& prec) {
    static const struct {
        uint8_t op, base;
        bool dbl;
    } kTable[] = {
        {0x40, 0x00, false}, {0x44, 0x00, true}, {0x41, 0x04, false}, {0x45, 0x04, true}, // FSMOVE FDMOVE FSSQRT FDSQRT
        {0x58, 0x18, false}, {0x5c, 0x18, true}, {0x5a, 0x1a, false}, {0x5e, 0x1a, true}, // FSABS FDABS FSNEG FDNEG
        {0x60, 0x20, false}, {0x64, 0x20, true}, {0x62, 0x22, false}, {0x66, 0x22, true}, // FSDIV FDDIV FSADD FDADD
        {0x63, 0x23, false}, {0x67, 0x23, true}, {0x68, 0x28, false}, {0x6c, 0x28, true}, // FSMUL FDMUL FSSUB FDSUB
    };
    for (const auto& e : kTable) {
        if (e.op == opmode) {
            prec = e.dbl ? fp::PREC_D : fp::PREC_S;
            return e.base;
        }
    }
    return -1;
}

// 68060 の FPU が実行する演算 (MC68060UM 表 6-11 にないもの)
bool implemented060(int opmode) {
    switch (opmode) {
    case 0x00: // FMOVE
    case 0x01: // FINT
    case 0x03: // FINTRZ
    case 0x04: // FSQRT
    case 0x18: // FABS
    case 0x1a: // FNEG
    case 0x20: // FDIV
    case 0x22: // FADD
    case 0x23: // FMUL
    case 0x24: // FSGLDIV
    case 0x27: // FSGLMUL
    case 0x28: // FSUB
    case 0x38: // FCMP
    case 0x3a: // FTST
        return true;
    default:
        return false;
    }
}

} // namespace

Fpu::Fpu(Cpu020* cpu) : cpu_(cpu) {
    cycleSink_ = &cpu->sub_;
    cycleCost_ = &cpu->fpuCost_;
    cycleCost060_ = &cpu->unit060_;
    reset();
}

void Fpu::reset() {
    resetRegisters();
    nullState_ = true;
    excPending_ = false;
    excp060_ = 0;
}

// ---------------------------------------------------------------- 68060

bool Fpu::m060() const { return cpu_->is060(); }

// PCR の DFP が 1 なら、FPU の命令はベクタ 11 (フォーマット $4: +$08 に実効アドレス、+$0C にこの命令のアドレス)。
// 積む PC は次の命令なので、拡張語と実効アドレスの拡張語は読み飛ばす。アドレスレジスタは変えない (MC68060UM 8.2.4)。
// 実効アドレスを積むのは一般の命令 (FMOVE、演算) のメモリのオペランドだけで、ほかは 0 にする
bool Fpu::fpDisabled060(uint16_t op) {
    if (!(cpu_->pcr_ & 2)) return false;
    uint32_t ea = 0;
    const int type = (op >> 6) & 7, mode = (op >> 3) & 7, reg = op & 7;
    switch (type) {
    case 0: { // 一般の命令、FMOVE / FMOVEM
        const uint16_t w = cpu_->xfetch16();
        const int cls = w >> 13, m = (w >> 10) & 7;
        static const int sizes[8] = {4, 4, 12, 12, 2, 8, 1, 12};
        if ((cls == 2 || cls == 3) && mode >= 2 && !(cls == 2 && m == 7)) {
            ea = eaData(mode, reg, sizes[m]);
        } else if ((cls == 4 || cls == 5) && mode >= 5) {
            // FMOVE(M) の制御レジスタ: イミディエイトは選んだレジスタの数 x 4 バイト
            const int n = ((w >> 12) & 1) + ((w >> 11) & 1) + ((w >> 10) & 1);
            eaData(mode, reg, 4 * (n ? n : 1));
        } else if (cls >= 6 && mode >= 5) {
            eaData(mode, reg, 12);
        }
        break;
    }
    case 1: { // FScc / FDBcc / FTRAPcc
        cpu_->xfetch16();
        if (mode == 1) cpu_->xfetch16();                     // FDBcc の変位
        else if (mode == 7 && reg == 2) cpu_->xfetch16();    // FTRAPcc.W
        else if (mode == 7 && reg == 3) eaData(7, 4, 4);     // FTRAPcc.L
        else if (mode >= 5 && !(mode == 7 && reg == 4)) eaData(mode, reg, 1);
        break;
    }
    case 2: cpu_->xfetch16(); break; // FBcc.W
    case 3: eaData(7, 4, 4); break;  // FBcc.L
    case 4:
    case 5: // FSAVE / FRESTORE
        if (mode >= 5) eaData(mode, reg, 12);
        break;
    default: break;
    }
    cpu_->undoAddressRegs();
    cpu_->noTrace_ = true;
    cpu_->exception(11, 4, cpu_->pc_, ea, cpu_->pc0_);
    return true;
}

// EXCP の状態なら、命令の前の例外 (フォーマット $0、PC はこの命令) にする
bool Fpu::takeExcp060() {
    if (!excp060_) return false;
    cpu_->noTrace_ = true;
    cpu_->exception(48 + (excp060_ & 7), 0, cpu_->pc0_);
    return true;
}

// 060FPSP に渡す例外 (フォーマット $2 / $3) のフレームに積む、メモリのオペランドのアドレス。(An)+ / -(An) で、
// 060FPSP が自分で An を書き直す場合は、An を変えずに (An)+ なら An、-(An) なら An - size (12 バイトなら An - 4) を積む。
// 060FPSP がそうするのは、メモリから読むオペランド (_dcalc_ea: (An)+ は An に size を足し、-(An) は An から size を
// 引いて、12 バイトのときはフレームのアドレスから 8 を引く) と、拡張精度・パック 10 進の FMOVE OUT (_calc_ea_fout:
// (An)+ は An に 12 を足し、-(An) はフレームのアドレスから 8 を引いて An にする)。単精度などの FMOVE OUT は
// フレームのアドレスにそのまま書くので、An を動かしたうえで実際のアドレスを積む
uint32_t Fpu::frameEa060(int mode, int reg, int size, bool out) {
    if ((mode == 3 || mode == 4) && (!out || size == 12)) {
        const int n = size == 1 && reg == 7 ? 2 : size; // A7 のバイトは 2 バイト動かす
        return mode == 3 ? cpu_->a_[reg] : cpu_->a_[reg] - (n == 12 ? 4 : n);
    }
    return eaData(mode, reg, size);
}

// FMOVE OUT で書いたあとに命令の後の例外にするときの、フレームのアドレス。拡張精度の (An)+ / -(An) は、
// writeDest() が動かした An を戻して frameEa060() と同じ形にする (060FPSP が An を書き直す)。addr は書いたアドレス
uint32_t Fpu::foutEa060(uint16_t op, int format, uint32_t addr) {
    const int mode = (op >> 3) & 7, reg = op & 7;
    if (mode == 0) return 0;
    if (format == 2 && (mode == 3 || mode == 4)) {
        cpu_->a_[reg] = mode == 3 ? addr : addr + 12;
        return frameEa060(mode, reg, 12, true);
    }
    return addr;
}

// 未実装の命令 (ベクタ 11) / パック 10 進 (ベクタ 55) の例外 (フォーマット $2、PC は次の命令)。eaSize が 0 でなく
// オペランドがメモリにあれば、実効アドレスを積む (frameEa060)。out は FMOVE OUT。060SP がない環境で肩代わりするなら false
bool Fpu::unimplemented060(uint16_t op, int vector, int format, int eaSize, bool out) {
    if (cpu_->assist060(vector)) return false;
    const int mode = (op >> 3) & 7, reg = op & 7;
    const uint32_t ea = eaSize && mode >= 2 ? frameEa060(mode, reg, eaSize, out) : 0;
    fpiar_ = cpu_->pc0_;
    if (vector == 55) {
        excp060_ = 0x80 | 7;
        excpOp_ = X{0, 0};
    }
    cpu_->noTrace_ = true;
    if (cpu_->onUnimplemented) cpu_->onUnimplemented(cpu_->onUnimplementedCtx, cpu_->pc0_, cpu_->op_, vector);
    cpu_->exception(vector, format, cpu_->pc_, ea);
    return true;
}

// 未実装の実効アドレス (ベクタ 60、フォーマット $0、PC はこの命令)
bool Fpu::unimpEa060() {
    if (cpu_->assist060(60)) return false;
    cpu_->noTrace_ = true;
    if (cpu_->onUnimplemented) cpu_->onUnimplemented(cpu_->onUnimplementedCtx, cpu_->pc0_, cpu_->op_, 60);
    cpu_->exception(60, 0, cpu_->pc0_);
    return true;
}

// 許可した演算の例外: EXCP の状態にする。例外のオペランドはソースのオペランド。
// FMOVE FPn,<ea> (opclass 3) は、すぐに命令の後の例外 (フォーマット $3、PC は次の命令、+$08 にメモリの
// デスティネーションのアドレス) にする。FPn に書く命令 (opclass 0 / 2) は、EXCP の状態のまま次の FPU の命令まで
// 待ち、その命令の前の例外 (フォーマット $0、PC はその命令) として takeExcp060 で起こす
// (MC68060UM 6.6.2.2、6.6.3.2、6.6.6.2、6.6.7.2)
void Fpu::fpException060(uint32_t mask, bool post, uint32_t ea) {
    const int vector = excVector(mask);
    excp060_ = (uint8_t)(0x80 | (vector - 48));
    excpOp_ = post ? lastSrc_ : frameSrc_;
    if (post) {
        // グループ 3 の例外のあとはトレースの例外を起こさない (MC68060UM 8.2.6、表 8-3)
        cpu_->noTrace_ = true;
        cpu_->exception(vector, 3, cpu_->pc_, ea);
    }
}

// 68060 が扱わない拡張精度の値 (MC68060UM 6.5.2): 非正規化数 (指数が 0 で、整数ビットが 0 で、仮数が 0 でない) と、
// 非正規化の値 (指数が 0 でも $7FFF でもなく、整数ビットが 0)。指数が 0 で整数ビットが 1 の値は正規化数として
// 扱う (表 6-6 の正規化数の指数の範囲は 0 <= e < $7FFF)
bool Fpu::unsupportedX(const X& x) {
    const int e = x.se & 0x7fff;
    if (e == 0x7fff || (x.m >> 63)) return false;
    return e != 0 || x.m != 0;
}

// 060FPSP に処理させる例外 (MC68060UM 6.5.2、6.6.4、6.6.5): 未実装のデータ型 (ベクタ 55) と、アンダーフロー
// (ベクタ 51) / オーバーフロー (ベクタ 53)。68060 はこれらを許可されていなくても例外にし、060FPSP が結果を作る。
// EXCP の状態にし、例外のオペランドはソースのオペランド (060FPSP はフレームからソースのオペランドを読む)。
// FPn に書く命令は命令の前の例外 (フォーマット $0)、FMOVE FPn,<ea> は命令の後の例外 (フォーマット $3、+$08 に
// デスティネーションのアドレス)。PC は次の命令。060FPSP がない環境で肩代わりするなら false (その場で計算する)
bool Fpu::unsupported060(int vector, uint32_t ea, int format, const X& operand) {
    if (cpu_->assist060(vector)) return false;
    excp060_ = (uint8_t)(0x80 | (vector - 48));
    excpOp_ = operand;
    cpu_->noTrace_ = true;
    if (cpu_->onUnimplemented) cpu_->onUnimplemented(cpu_->onUnimplementedCtx, cpu_->pc0_, cpu_->op_, vector);
    if (format == 3) cpu_->exception(vector, 3, cpu_->pc_, ea);
    else cpu_->exception(vector, 0, cpu_->pc_);
    return true;
}

// FSAVE (MC68060UM 6.7): 3 ロング。NULL は 0、IDLE は $00006000、EXCP は例外のオペランドの指数 << 16 | $E000 | 種類。
// EXCP の状態は消える (IDLE になる)。NULL の状態のままにする (FSAVE は NULL から抜けない)
void Fpu::fsave060(int mode, int reg) {
    const uint32_t a = mode == 4 ? cpu_->a_[reg] - 12 : cpu_->xeaAddr(mode, reg, 12);
    uint32_t w0 = 0, hi = 0, lo = 0;
    if (excp060_) {
        w0 = ((uint32_t)excpOp_.se << 16) | 0xe000u | (excp060_ & 7);
        hi = (uint32_t)(excpOp_.m >> 32);
        lo = (uint32_t)excpOp_.m;
    } else if (!nullState_) {
        w0 = 0x6000;
    }
    cpu_->xwrite32(a, w0);
    cpu_->xwrite32(a + 4, hi);
    cpu_->xwrite32(a + 8, lo);
    excp060_ = 0;
    if (mode == 4) cpu_->a_[reg] = a;
    cycles(10);
}

// FRESTORE: フォーマットが $00 / $60 / $E0 でなければフォーマットエラー (ベクタ 14)
void Fpu::frestore060(int mode, int reg) {
    const uint32_t a = mode == 3 ? cpu_->a_[reg] : cpu_->xeaAddr(mode, reg, 12);
    const uint32_t w0 = cpu_->xread32(a);
    const int format = (int)((w0 >> 8) & 0xff);
    if (format == 0x00) {
        reset();
    } else if (format == 0x60) {
        nullState_ = false;
        excp060_ = 0;
    } else if (format == 0xe0) {
        const uint32_t hi = cpu_->xread32(a + 4), lo = cpu_->xread32(a + 8);
        nullState_ = false;
        excp060_ = (uint8_t)(0x80 | (w0 & 7));
        excpOp_ = X{(uint16_t)(w0 >> 16), ((uint64_t)hi << 32) | lo};
    } else {
        cpu_->exception(14, 0, cpu_->pc0_);
        return;
    }
    if (mode == 3) cpu_->a_[reg] = a + 12;
    cycles(10);
}

void Cpu020::opFpu(uint16_t op) { fpu_->execute(op); }

// 条件。68882 の条件は FpuCore::cond881
bool Fpu::cond(int cc) const {
    // 68060 は MC68060UM 表 6-9 の式のとおり: OR は NAN だけ、NE は Z だけを見る (Z と NAN が両方立っているときに
    // 68882 と違う。fputest.x の期待値で確かめた)
    if (cpu_->is060()) {
        if ((cc & 15) == 7) return !(fpsr_ & CC_NAN);
        if ((cc & 15) == 14) return !(fpsr_ & CC_Z);
    }
    return cond881(cc);
}

// 許可された例外があれば起こす (MC68881UM 6.1)。
// - FMOVE FPn,<ea> は、書いたあとにその場で命令の途中の例外 (フォーマット $9) にし、+$10 に書いた先のアドレスを積む
//   (6.1.2〜6.1.7 の「Trap Enabled Results」)
// - FPn に書く命令 (演算、FMOVE <ea>,FPn、FMOVECR) は、例外を保留する (EXC PEND) だけで、次の FPU の命令を始める
//   ときに、その命令の前の例外 (フォーマット $0、PC はその命令) として知らせる (takePending())。保留を知らせるのは
//   演算、FMOVE、条件の命令 (FBcc、FScc、FDBcc、FTRAPcc) で、FMOVEM、FMOVE の制御レジスタ、FSAVE、FRESTORE は
//   知らせない (6.4.2.2 の BIU フラグの説明)。それまでの間の CPU の命令はそのまま進む。保留は FSAVE で IDLE の
//   フレームに移る (BIU フラグの bit27 が 0)。68882 は例外を知らせても保留を消さないので、処理ルーチンは FSAVE、
//   bit27 を立てる、FRESTORE を行う (5.2.2)
// - BSUN は条件の命令の、その場で命令の前の例外にする (checkBsun())
// 実機と違う点: 68882 が計算と並行して次の命令を受け付けていると、保留を次の命令の途中の例外として知らせることがある
// (6.1 の FMUL の例) が、ここでは並行動作をしないので、いつも命令の前の例外にする。フレームの例外のオペランドは、
// どの例外でもソースのオペランドにする (実機は OVFL / UNFL では指数を折り返した中間結果)
// cmd と operand は保留するときに IDLE のフレームに入れるコマンドの語と例外のオペランド
bool Fpu::fpException(bool mid, uint32_t ea, uint16_t cmd, const X& operand) {
    uint32_t mask = enabledExceptions();
    if (!mask) return false;
    if (m060()) {
        fpException060(mask, mid, mid ? ea : 0);
        return true;
    }
    if (mid) {
        cpu_->exception(excVector(mask), 9, cpu_->pc_, cpu_->pc0_, ea);
        return true;
    }
    // IDLE のフレームのヘッダより後 (fsave() が書く 14 ロング。[0] が +$04)。コマンド / 条件レジスタが +$04、
    // その後ろの +$08〜+$27 が 68882 で増えた内部の状態 (32 バイト)、例外のオペランドが +$28、オペランドのレジスタが
    // +$34、BIU フラグが +$38 (MC68881UM 図 6-5、5.2.2 の「68882 では $20 大きい」。68881 のボードの fpu_board.cpp と
    // 同じ並びで、例外のオペランドから後ろが 32 バイトずれる)。ほかは 0 にする
    for (uint32_t& v : pendingFrame_) v = 0;
    pendingFrame_[0] = (uint32_t)cmd << 16;
    pendingFrame_[9] = (uint32_t)operand.se << 16;
    pendingFrame_[10] = (uint32_t)(operand.m >> 32);
    pendingFrame_[11] = (uint32_t)operand.m;
    pendingFrame_[13] = kBiuIdle & ~0x08000000u;
    excPending_ = true;
    return false;
}

uint32_t Fpu::eaData(int mode, int reg, int size) {
    if (mode == 7 && reg == 4) {
        uint32_t a = cpu_->pc_;
        // バイトのイミディエイトは 1 語の下位
        int n = size == 1 ? 2 : size;
        for (int i = 0; i < n; i += 2) cpu_->xfetch16();
        return size == 1 ? a + 1 : a;
    }
    return cpu_->xeaAddr(mode, reg, size);
}

void Fpu::execute(uint16_t op) {
    if ((op & 0x0e00) != 0x0200) { // コプロセッサ ID 1 以外
        cpu_->lineF();
        return;
    }
    if (m060() && fpDisabled060(op)) return;
    switch ((op >> 6) & 7) {
    case 0: general(op); break;
    case 1: scc(op); break;
    case 2: bcc(op, false); break;
    case 3: bcc(op, true); break;
    case 4: fsave(op); break;
    case 5: frestore(op); break;
    default: cpu_->lineF(); break;
    }
}

// ---------------------------------------------------------------- オペランド

bool Fpu::readSource(uint16_t op, int format, X& out, Ctx& c, uint32_t& addr) {
    int mode = (op >> 3) & 7, reg = op & 7;
    static const int sizes[8] = {4, 4, 12, 12, 2, 8, 1, 0};
    int size = sizes[format];
    srcUnsupported_ = false;
    if (mode == 0) { // Dn は B / W / L / S だけ
        uint32_t v = cpu_->d_[reg];
        switch (format) {
        case 0: out = fp::fromInt((int32_t)v); break;
        case 1: out = fp::fromSingle(v, c); singleFrame(v); break;
        case 4: out = fp::fromInt((int16_t)v); break;
        case 6: out = fp::fromInt((int8_t)v); break;
        default: cpu_->illegal(); return false;
        }
        if (format != 1) frameSrc_ = out;
        return true;
    }
    if (mode == 1 || (mode == 7 && reg > 4)) {
        cpu_->illegal();
        return false;
    }
    addr = eaData(mode, reg, size);
    switch (format) {
    case 0: out = fp::fromInt((int32_t)cpu_->xread32(addr)); break;
    case 1: {
        const uint32_t v = cpu_->xread32(addr);
        out = fp::fromSingle(v, c);
        singleFrame(v);
        break;
    }
    case 2: {
        uint32_t hi = cpu_->xread32(addr);
        uint64_t m = ((uint64_t)cpu_->xread32(addr + 4) << 32) | cpu_->xread32(addr + 8);
        X x{(uint16_t)(hi >> 16), m};
        if (fp::isSnan(x)) c.exc |= fp::EXC_SNAN;
        out = canon(x);
        frameSrc_ = x;
        srcUnsupported_ = unsupportedX(x);
        break;
    }
    case 3: {
        uint32_t p[3] = {cpu_->xread32(addr), cpu_->xread32(addr + 4), cpu_->xread32(addr + 8)};
        out = fp::fromPacked(p, c);
        break;
    }
    case 4: out = fp::fromInt((int16_t)cpu_->xread16(addr)); break;
    case 5: {
        uint64_t v = ((uint64_t)cpu_->xread32(addr) << 32) | cpu_->xread32(addr + 4);
        out = fp::fromDouble(v, c);
        // 060FPSP の fix_skewed_ops が直す形: 指数のバイアスを付け替えただけ (非正規化数と 0 は $3C00、
        // 無限大と NaN は $43FF)。仮数は小数部を左に詰め、正規化数なら最上位ビットを立てる
        const int e = (int)((v >> 52) & 0x7ff);
        const uint64_t f = v & 0xfffffffffffffull;
        frameSrc_ = X{(uint16_t)((v >> 48 & 0x8000) | (uint32_t)(e - 1023 + 0x3fff)), (e ? 1ull << 63 : 0) | f << 11};
        srcUnsupported_ = e == 0 && f != 0;
        break;
    }
    case 6: out = fp::fromInt((int8_t)cpu_->xread8(addr)); break;
    default: return false;
    }
    if (format != 1 && format != 2 && format != 5) frameSrc_ = out;
    if (fp::isNan(out)) out = defaultNan();
    return true;
}

// 単精度のソースを、68060 が EXCP のフレームに入れる形にする (指数のバイアスを付け替えただけ。非正規化数と 0 は
// $3F80、無限大と NaN は $407F。060FPSP の fix_skewed_ops が直す)
void Fpu::singleFrame(uint32_t v) {
    const int e = (int)((v >> 23) & 0xff);
    const uint32_t f = v & 0x7fffff;
    frameSrc_ = X{(uint16_t)((v >> 16 & 0x8000) | (uint32_t)(e - 127 + 0x3fff)), (e ? 1ull << 63 : 0) | (uint64_t)f << 40};
    srcUnsupported_ = e == 0 && f != 0;
}

void Fpu::writeDest(uint16_t op, uint16_t w, int format, int n, Ctx& c, uint32_t& addr) {
    int mode = (op >> 3) & 7, reg = op & 7;
    const X& x = fp_[n];
    static const int sizes[8] = {4, 4, 12, 12, 2, 8, 1, 12};
    if (mode == 0) {
        uint32_t& d = cpu_->d_[reg];
        switch (format) {
        case 0: d = (uint32_t)fp::toInt(x, c, 32); return;
        case 1: d = fp::toSingle(x, c); return;
        case 4: d = (d & 0xffff0000u) | ((uint32_t)fp::toInt(x, c, 16) & 0xffff); return;
        case 6: d = (d & 0xffffff00u) | ((uint32_t)fp::toInt(x, c, 8) & 0xff); return;
        default: cpu_->illegal(); return;
        }
    }
    if (mode == 1 || (mode == 7 && reg > 1)) {
        cpu_->illegal();
        return;
    }
    addr = cpu_->xeaAddr(mode, reg, sizes[format]);
    switch (format) {
    case 0: cpu_->xwrite32(addr, (uint32_t)fp::toInt(x, c, 32)); break;
    case 1: cpu_->xwrite32(addr, fp::toSingle(x, c)); break;
    case 2: {
        // 非正規化数は UNFL を立てる (正確でも)。MC68881UM 6.1.5: メモリへの書き込みは、値が行き先の形式の
        // 正規化数で表せないほど小さければアンダーフロー (正確かどうかは条件にない)
        if ((x.se & 0x7fff) == 0 && x.m != 0 && !(x.m >> 63)) c.exc |= fp::EXC_UNFL;
        // シグナリング NaN は SNAN を立て、SNAN のビットを立てて書く (MC68881UM 6.1.2)
        uint64_t m = x.m;
        if (fp::isSnan(x)) {
            c.exc |= fp::EXC_SNAN;
            m |= 0x4000000000000000ull;
        }
        cpu_->xwrite32(addr, (uint32_t)x.se << 16);
        cpu_->xwrite32(addr + 4, (uint32_t)(m >> 32));
        cpu_->xwrite32(addr + 8, (uint32_t)m);
        break;
    }
    case 3:
    case 7: {
        int k = format == 3 ? (w & 0x7f) : (int)(cpu_->d_[(w >> 4) & 7] & 0x7f);
        uint32_t p[3];
        fp::toPacked(x, k, c, p);
        for (int i = 0; i < 3; i++) cpu_->xwrite32(addr + 4 * i, p[i]);
        break;
    }
    case 4: cpu_->xwrite16(addr, (uint16_t)fp::toInt(x, c, 16)); break;
    case 5: {
        uint64_t v = fp::toDouble(x, c);
        cpu_->xwrite32(addr, (uint32_t)(v >> 32));
        cpu_->xwrite32(addr + 4, (uint32_t)v);
        break;
    }
    case 6: cpu_->xwrite8(addr, (uint8_t)fp::toInt(x, c, 8)); break;
    }
}

// ---------------------------------------------------------------- 演算

void Fpu::general(uint16_t op) {
    uint16_t w = cpu_->xfetch16();
    cycles(16);
    int cls = w >> 13;
    int m = (w >> 10) & 7, n = (w >> 7) & 7, opmode = w & 0x7f;
    Ctx c = ctx();
    uint32_t addr = 0;
    switch (cls) {
    case 0: // FPm,FPn
    case 2: { // <ea>,FPn
        if (opmode >= 0x40) { // FSxxx / FDxxx は 68060 だけ (68882 にはない)
            int prec = 0;
            opmode = m060() ? fsdBase(opmode, prec) : -1;
            if (opmode < 0) {
                cpu_->lineF();
                return;
            }
            c.prec = prec;
        }
        if (takePending()) return;
        if (m060()) {
            static const int srcSizes[8] = {4, 4, 12, 12, 2, 8, 1, 0};
            const int mode = (op >> 3) & 7, reg = op & 7;
            const bool fmovecr = cls == 2 && m == 7;
            if (cls == 2 && !fmovecr && mode == 7 && reg == 4 && (m == 2 || m == 3) && unimpEa060()) return;
            if (cls == 2 && m == 3 && unimplemented060(op, 55, 2, 12)) return;
            if ((fmovecr || !implemented060(opmode)) && unimplemented060(op, 11, 2, cls == 2 ? srcSizes[m] : 0)) return;
        }
        nullState_ = false;
        // 例外で結果を捨てるときに戻す条件コード (消す前に取っておく)
        const uint32_t keepCc = fpsr_ & 0xff000000u;
        fpsr_ &= 0x00ff00ff;
        fpiar_ = cpu_->pc0_;
        X src;
        if (cls == 2) {
            if (m == 7) { // FMOVECR
                if (opmode >= 0x40) {
                    cpu_->lineF();
                    return;
                }
                fmovecr(opmode, n, c);
                accrue();
                cycles(10);
                fpException(false, 0, w, fp_[n]);
                return;
            }
            if (!readSource(op, m, src, c, addr)) return;
        } else {
            src = fp_[m];
            frameSrc_ = src;
            srcUnsupported_ = unsupportedX(src);
        }
        lastSrc_ = src;
        if (m060()) {
            // 非正規化数などのオペランドは、未実装のデータ型の例外 (ベクタ 55)。FCMP と 2 項の演算はデスティネーションも見る
            const bool dyadic = (opmode >= 0x20 && opmode <= 0x2f) || opmode == 0x38 || opmode == 0x39 ||
                                opmode == 0x3c || opmode == 0x3d;
            if ((srcUnsupported_ || (dyadic && unsupportedX(fp_[n]))) && unsupported060(55, 0, 0, frameSrc_)) {
                fpsr_ = (fpsr_ & 0x00ff00ff) | keepCc;
                return;
            }
        }
        // SNAN、OPERR、DZ の例外が許可されていれば、レジスタは変えない (MC68881UM 6.1.2、6.1.3、6.1.6)。
        // OVFL、UNFL、INEX は例外が許可されていても結果を書く
        const X keepDst = fp_[n], keepCos = fp_[opmode & 7];
        uint32_t cc = 0;
        bool setCc;
        if (!arith(opmode, src, n, c, cc, setCc)) {
            fp_[n] = keepDst;
            cpu_->lineF();
            return;
        }
        // 68060 はアンダーフロー・オーバーフローを、許可されていなくても例外にし (ベクタ 51 / 53)、060FPSP が結果を
        // 作る。レジスタと条件コードは変えない (MC68060UM 6.6.4、6.6.5)
        bool trap060 = m060() && (c.exc & (fp::EXC_OVFL | fp::EXC_UNFL));
        if (trap060 && (c.exc & fp::EXC_OVFL)) {
            // オーバーフローを例外にするのは、丸める前の中間結果があふれるときだけにしている。0 への丸めで計算し直しても
            // あふれるかで見分ける。丸めて初めてあふれる場合は、無限大などを返し OVFL を立てるが例外にしない。
            // MC68060UM 6.6.4 の 1 文目は「中間結果の指数が最大値以上なら」だが、続けて「アンダーフローを調べ、丸めて
            // から、オーバーフローを調べる」「EXC の OVFL が立ったら必ずオーバーフローの例外にする」とあり、丸めて
            // あふれる場合も例外にするのがマニュアルどおりと読める。いまの形にしているのは、fputest (期待値は PARI/GP の
            // 計算で、実機の値ではない) の FSMUL 20 件・FDMUL 8 件が合うため。例外にすると、060FPSP がこれらで OVFL の
            // ない 2^128 などを返して合わなくなり、代わりに FTWOTOX の 4 件が合う。実機の 68060 で確かめるまで決めない (#36)
            const X res = fp_[n], resCos = fp_[opmode & 7];
            fp_[n] = keepDst;
            if (opmode >= 0x30 && opmode <= 0x37) fp_[opmode & 7] = keepCos;
            Ctx rz = c;
            rz.rnd = fp::RND_Z;
            rz.exc = 0;
            uint32_t ccRz = 0;
            bool setCcRz;
            arith(opmode, src, n, rz, ccRz, setCcRz);
            trap060 = (rz.exc & fp::EXC_OVFL) != 0;
            fp_[n] = res;
            if (opmode >= 0x30 && opmode <= 0x37) fp_[opmode & 7] = resCos;
        }
        if (trap060) {
            const int vector = (c.exc & fp::EXC_OVFL) ? 53 : 51;
            if (!cpu_->assist060(vector)) {
                fp_[n] = keepDst;
                if (opmode >= 0x30 && opmode <= 0x37) fp_[opmode & 7] = keepCos;
                // 命令の前の例外として、次の FPU の命令を始めるときに起こす (MC68060UM 6.6.5.2)。それまでは EXCP の状態で、
                // FSAVE で読める。060FPSP は超越関数の最後の演算のあとに FSAVE でこれを調べて、自分で結果を直す
                fpsr_ = (fpsr_ & 0x00ff00ff) | keepCc | (c.exc & 0xff00);
                excp060_ = (uint8_t)(0x80 | (vector - 48));
                excpOp_ = frameSrc_;
                cycles(4);
                return;
            }
        }
        fpsr_ = (fpsr_ & 0x00ffffff) | cc | c.exc;
        if (fpcr_ & c.exc & (fp::EXC_SNAN | fp::EXC_OPERR | fp::EXC_DZ)) {
            fp_[n] = keepDst;
            if (opmode >= 0x30 && opmode <= 0x37) fp_[opmode & 7] = keepCos;
            fpsr_ = (fpsr_ & 0x00ffffff) | keepCc;
        }
        accrue();
        fpException(false, 0, w, src);
        return;
    }
    case 3: { // FMOVE FPn,<ea>
        if (takePending()) return;
        // 68060 はパック 10 進に書けない (ベクタ 55)
        if (m060() && (m == 3 || m == 7) && unimplemented060(op, 55, 2, 12, true)) return;
        lastSrc_ = fp_[n];
        nullState_ = false;
        fpsr_ &= 0xffff00ff;
        fpiar_ = cpu_->pc0_;
        if (m060() && unsupportedX(fp_[n]) && !cpu_->assist060(55)) {
            // 非正規化数などを書くときは、書かずに未実装のデータ型の例外 (命令の後、フォーマット $3)
            const int mode = (op >> 3) & 7, reg = op & 7;
            static const int sizes[8] = {4, 4, 12, 12, 2, 8, 1, 12};
            if (mode == 1 || (mode == 7 && reg > 1)) {
                cpu_->illegal();
                return;
            }
            unsupported060(55, mode == 0 ? 0 : frameEa060(mode, reg, sizes[m], true), 3, fp_[n]);
            return;
        }
        writeDest(op, w, m, n, c, addr);
        // 68060 はアンダーフロー・オーバーフローを、許可されていなくても例外にする (書いた値は不定。命令の後の例外)
        if (m060() && (c.exc & (fp::EXC_OVFL | fp::EXC_UNFL))) {
            const int vector = (c.exc & fp::EXC_OVFL) ? 53 : 51;
            if (!cpu_->assist060(vector)) {
                unsupported060(vector, foutEa060(op, m, addr), 3, fp_[n]);
                return;
            }
        }
        fpsr_ |= c.exc;
        accrue();
        cycles(20, 1);
        // 書いたあと、命令の途中の例外 (フォーマット $9)。Dn に書いたときのアドレスは 0
        // 68060 で 060FPSP が受け取るなら、拡張精度の (An)+ / -(An) は An を戻して 060FPSP が直す形のアドレスにする。
        // 060SP を肩代わりするとき (assist060) は、060FPSP が直したあとと同じく An を動かしたまま、書いたアドレスを積む
        const uint32_t en = enabledExceptions();
        const bool toFpsp = m060() && en && !cpu_->assist060(excVector(en));
        fpException(true, toFpsp ? foutEa060(op, m, addr) : (((op >> 3) & 7) == 0 ? 0 : addr), w, fp_[n]);
        return;
    }
    case 4:
    case 5:
        // 68060 は、待っている例外 (EXCP の状態) を FMOVE / FMOVEM の制御レジスタでも起こす。起こさないのは FSAVE だけ
        // (MC68060UM 6.6.5.2: 例外ハンドラは最初の FPU の命令を FSAVE にしなければならない)
        if (m060() && takeExcp060()) return;
        nullState_ = false;
        moveControl(op, w, cls == 5);
        return;
    case 6:
    case 7:
        if (m060() && takeExcp060()) return;
        nullState_ = false;
        movemData(op, w, cls == 7);
        return;
    default:
        fpiar_ = cpu_->pc0_;
        cpu_->lineF();
        return;
    }
}

// FMOVE / FMOVEM の FPCR / FPSR / FPIAR (w の bit12-10 が FPCR / FPSR / FPIAR)
void Fpu::moveControl(uint16_t op, uint16_t w, bool toMem) {
    int mode = (op >> 3) & 7, reg = op & 7;
    int list = (w >> 10) & 7;
    int count = (list & 4 ? 1 : 0) + (list & 2 ? 1 : 0) + (list & 1 ? 1 : 0);
    // レジスタの並びが 000 のときは、FPIAR 1 つとして扱う。MC68881UM の表 4-17 では 000 は「未定義、予約」で、
    // 実機の動きは確かめていない
    if (count == 0) list = 1, count = 1;
    uint32_t* regs[3] = {&fpcr_, &fpsr_, &fpiar_};
    const uint32_t masks[3] = {0x0000fff0u, 0x0ffffff8u, 0xffffffffu};
    if (count == 1 && (mode == 0 || (mode == 1 && list == 1))) { // Dn (FPIAR だけは An も)
        int i = list == 4 ? 0 : list == 2 ? 1 : 2;
        uint32_t& r = mode == 0 ? cpu_->d_[reg] : cpu_->a_[reg];
        if (toMem) r = *regs[i];
        else *regs[i] = r & masks[i];
        cycles(10, 3);
        return;
    }
    if (mode <= 1 || (toMem && mode == 7 && reg > 1) || (!toMem && mode == 7 && reg > 4) ||
        (mode == 7 && reg == 4 && count > 1 && toMem)) {
        cpu_->illegal();
        return;
    }
    uint32_t a;
    if (!toMem && mode == 7 && reg == 4) {
        // 68060 はイミディエイトから 2 つ以上の制御レジスタに入れられない (ベクタ 60)
        if (count > 1 && m060() && unimpEa060()) return;
        a = cpu_->pc_;
        for (int i = 0; i < count * 2; i++) cpu_->xfetch16();
    } else {
        a = cpu_->xeaAddr(mode, reg, 4 * count);
    }
    for (int i = 0; i < 3; i++) {
        if (!(list & (4 >> i))) continue;
        if (toMem) cpu_->xwrite32(a, *regs[i]);
        else *regs[i] = cpu_->xread32(a) & masks[i];
        a += 4;
    }
    cycles(10 + 8 * count, 4);
}

// FMOVEM.X (w の bit11 が 1 ならリストは Dn の下位 8 ビット)
void Fpu::movemData(uint16_t op, uint16_t w, bool toMem) {
    int mode = (op >> 3) & 7, reg = op & 7;
    // 68060 は動的なリスト (Dn) を扱わない (ベクタ 60)
    if ((w & 0x0800) && m060() && unimpEa060()) return;
    uint32_t list = ((w & 0x0800) ? cpu_->d_[(w >> 4) & 7] : w) & 0xff;
    bool ok = toMem ? (mode == 4 || mode == 2 || mode == 5 || mode == 6 || (mode == 7 && reg <= 1))
                    : (mode == 3 || mode == 2 || mode == 5 || mode == 6 || (mode == 7 && reg <= 3));
    if (!ok) {
        cpu_->illegal();
        return;
    }
    if (toMem && mode == 4) { // -(An): 最上位のビットが FP7
        uint32_t a = cpu_->a_[reg];
        for (int i = 7; i >= 0; i--, list <<= 1) {
            if (!(list & 0x80)) continue;
            a -= 12;
            const X& x = fp_[i];
            cpu_->xwrite32(a, (uint32_t)x.se << 16);
            cpu_->xwrite32(a + 4, (uint32_t)(x.m >> 32));
            cpu_->xwrite32(a + 8, (uint32_t)x.m);
            cycles(12, 3);
        }
        cpu_->a_[reg] = a;
        return;
    }
    uint32_t a = mode == 3 ? cpu_->a_[reg] : cpu_->xeaAddr(mode, reg, 12);
    for (int i = 0; i < 8; i++, list <<= 1) {
        if (!(list & 0x80)) continue;
        if (toMem) {
            const X& x = fp_[i];
            cpu_->xwrite32(a, (uint32_t)x.se << 16);
            cpu_->xwrite32(a + 4, (uint32_t)(x.m >> 32));
            cpu_->xwrite32(a + 8, (uint32_t)x.m);
        } else {
            uint32_t hi = cpu_->xread32(a);
            uint64_t m = ((uint64_t)cpu_->xread32(a + 4) << 32) | cpu_->xread32(a + 8);
            fp_[i] = X{(uint16_t)(hi >> 16), m}; // そのまま入れる (シグナリング NaN や非正規化数も保つ)
        }
        a += 12;
        cycles(12, 3);
    }
    if (mode == 3) cpu_->a_[reg] = a;
}

// ---------------------------------------------------------------- 条件

bool Fpu::checkBsun(int cc) {
    fpiar_ = cpu_->pc0_;
    if ((cc & 0x10) && (fpsr_ & CC_NAN)) {
        fpsr_ |= fp::EXC_BSUN;
        accrue();
        if (fpcr_ & fp::EXC_BSUN) {
            cpu_->exception(48, 0, cpu_->pc0_);
            return true;
        }
    }
    return false;
}

void Fpu::scc(uint16_t op) {
    uint16_t w = cpu_->xfetch16();
    nullState_ = false;
    int mode = (op >> 3) & 7, reg = op & 7;
    if (mode == 7 && reg > 4) {
        cpu_->lineF();
        return;
    }
    if (takePending()) return;
    if (m060() && !cpu_->assist060(11)) {
        // 68060 に FScc、FDBcc、FTRAPcc はない (ベクタ 11、PC は次の命令、FScc <ea> は実効アドレスを積む)
        uint32_t ea = 0;
        if (mode == 1) cpu_->xfetch16();
        else if (mode == 7 && reg == 2) cpu_->xfetch16();
        else if (mode == 7 && reg == 3) cpu_->xfetch32();
        // FScc の (An)+ / -(An) は、An を変えずに (An)+ なら An、-(An) なら An - 1 (A7 は 2) を積む。060FPSP は
        // フレームのアドレスに書いてから、自分で An を足し引きする (frameEa060 と同じ形)
        else if (mode >= 2 && !(mode == 7 && reg == 4)) ea = frameEa060(mode, reg, 1, false);
        fpiar_ = cpu_->pc0_;
        cpu_->noTrace_ = true;
        if (cpu_->onUnimplemented) cpu_->onUnimplemented(cpu_->onUnimplementedCtx, cpu_->pc0_, cpu_->op_, 11);
        cpu_->exception(11, 2, cpu_->pc_, ea);
        return;
    }
    if (checkBsun(w)) return;
    bool t = cond(w & 63);
    cycles(16, 1);
    if (mode == 0) { // FScc Dn
        cpu_->d_[reg] = (cpu_->d_[reg] & 0xffffff00u) | (t ? 0xff : 0);
    } else if (mode == 1) { // FDBcc
        if (t) {
            cpu_->xfetch16();
            return;
        }
        uint32_t& d = cpu_->d_[reg];
        uint16_t lo = (uint16_t)(d - 1);
        d = (d & 0xffff0000u) | lo;
        if (lo == 0xffff) {
            cpu_->xfetch16();
            return;
        }
        uint32_t base = cpu_->pc_;
        int16_t disp = (int16_t)cpu_->xfetch16();
        cpu_->xjumpTo(base + (int32_t)disp);
    } else if (mode == 7 && reg >= 2) { // FTRAPcc (.W / .L / なし)
        if (reg == 2) cpu_->xfetch16();
        else if (reg == 3) cpu_->xfetch32();
        if (t) cpu_->trapFormat2(7);
    } else { // FScc <ea>
        uint32_t a = cpu_->xeaAddr(mode, reg, 1);
        cpu_->xwrite8(a, t ? 0xff : 0);
    }
}

void Fpu::bcc(uint16_t op, bool longDisp) {
    nullState_ = false;
    if (takePending()) return;
    if (checkBsun(op)) return;
    uint32_t base = cpu_->pc_;
    int32_t disp = longDisp ? (int32_t)cpu_->xfetch32() : (int16_t)cpu_->xfetch16();
    cycles(10, 2);
    if (cond(op & 63)) cpu_->xjumpTo(base + (uint32_t)disp);
}

// ---------------------------------------------------------------- FSAVE / FRESTORE

// 保留している例外 (FPn に書く命令の例外か、FRESTORE したフレームの保留) を、命令の前の例外 (フォーマット $0、PC は
// この命令) として起こす。68882 は例外を知らせても保留を消さない (MC68881UM 6.4.2.2)。処理ルーチンは FSAVE で保留を
// フレームに移し、bit27 を立てて FRESTORE する
bool Fpu::takePending() {
    if (m060()) return takeExcp060();
    if (!excPending_) return false;
    uint32_t mask = enabledExceptions();
    if (!mask) { // FPCR / FPSR を書き換えて、知らせる例外がなくなった: 命令を始めるので保留は消える (6.4.2.2)
        excPending_ = false;
        return false;
    }
    cpu_->exception(excVector(mask), 0, cpu_->pc0_);
    return true;
}

void Fpu::fsave(uint16_t op) {
    if (!cpu_->sflag_) { // ユーザモードでは特権違反 (MC68881UM 6.2.7)
        cpu_->privilege();
        return;
    }
    int mode = (op >> 3) & 7, reg = op & 7;
    if (!(mode == 2 || mode == 4 || mode == 5 || mode == 6 || (mode == 7 && reg <= 1))) {
        cpu_->lineF();
        return;
    }
    if (m060()) {
        fsave060(mode, reg);
        return;
    }
    // NULL は 1 ロング (フォーマット語 $0000。MC68881UM の FRESTORE / FSAVE の説明)、IDLE は $1F380000 と 14 ロング (MC68881UM 図 6-5)。
    // IDLE の最後のロングは BIU フラグ: 例外の保留なし (bit27)、メモリへの転送の保留なし (bit26)、
    // 命令の保留なし (bit30-28 = 111)、定義されていない下位 16 ビットは 1。ほかは 0 にする
    int n = nullState_ ? 0 : 14;
    uint32_t head = nullState_ ? 0x00000000u : 0x1f380000u;
    nullState_ = false;
    uint32_t size = 4 + 4 * (uint32_t)n;
    uint32_t a;
    if (mode == 4) {
        a = cpu_->a_[reg] - size;
    } else {
        a = cpu_->xeaAddr(mode, reg, 4);
    }
    // 例外が保留されていれば (FRESTORE したフレームの bit27 が 0)、そのフレームの中身 (例外のオペランドなど) を
    // そのまま書き、保留を FPU から消す。保留はフレームに移り、次の FRESTORE でまた保留になる (MC68881UM 6.4.2.2)
    const bool pending = excPending_ && n;
    for (int k = 0; k <= n; k++) {
        uint32_t v = k == 0 ? head : pending ? pendingFrame_[k - 1] : k == n ? kBiuIdle : 0;
        cpu_->xwrite32(a + 4 * (uint32_t)k, v);
    }
    excPending_ = false;
    if (mode == 4) cpu_->a_[reg] = a;
    // サイクル数は MC68881UM の表 8-9 (FSAVE の始まり、最良 13)、表 8-22 (フレームの転送)、表 8-23 (終わり、最良 1) の和。
    // 表 8-22 は MC68881 の値 (IDLE は 6 ロングで 36 = 1 ロング 6 クロック) なので、68882 の IDLE (14 ロング) は
    // 14 x 6 = 84 とした: 13 + 84 + 1 = 98。NULL は転送がなく 13 + 1 = 14
    cycles(n ? 98 : 14, 3);
}

void Fpu::frestore(uint16_t op) {
    if (!cpu_->sflag_) { // ユーザモードでは特権違反 (MC68881UM 6.2.7)
        cpu_->privilege();
        return;
    }
    int mode = (op >> 3) & 7, reg = op & 7;
    if (!(mode == 2 || mode == 3 || mode == 5 || mode == 6 || (mode == 7 && reg <= 3))) {
        cpu_->lineF();
        return;
    }
    if (m060()) {
        frestore060(mode, reg);
        return;
    }
    uint32_t a = mode == 3 ? cpu_->a_[reg] : cpu_->xeaAddr(mode, reg, 4);
    uint32_t head = cpu_->xread32(a);
    uint32_t size = 4;
    int version = (int)(head >> 24), frameSize = (int)((head >> 16) & 0xff);
    if (version == 0) { // NULL (大きさは見ない): FPU をリセットする
        reset();
        cycles(19, 6); // 表 8-9 (FRESTORE の始まり、最良 16) + 表 8-23 (終わり、最良 3)
    } else if (version == 0x1f && (frameSize == 0x38 || frameSize == 0xd4)) { // 68882 の IDLE / BUSY
        int n = frameSize / 4;
        uint32_t biu = 0;
        for (int k = 1; k <= n; k++) {
            biu = cpu_->xread32(a + 4 * (uint32_t)k);
            if (k <= 14) pendingFrame_[k - 1] = biu;
        }
        size += (uint32_t)frameSize;
        nullState_ = false;
        // IDLE の BIU フラグの bit27 が 0 なら、例外が保留されている。次の FPU の命令を始めるときに起こす (6.4.2.2)
        excPending_ = frameSize == 0x38 && !(biu & 0x08000000u);
        // 表 8-9 (16) + 表 8-22 を 68882 の IDLE の 14 ロングに直した 84 + 表 8-23 (3) = 103。
        // BUSY (53 ロング) も同じ値にしている (表のとおりなら 16 + 53 x 6 + 3 = 337)
        cycles(103, 6);
    } else {
        // 68882 のものでないフレームはフォーマットエラー (命令の前の例外、ベクタ 14。MC68881UM 6.2.8)
        cpu_->exception(14, 0, cpu_->pc0_);
        return;
    }
    // (An)+ は、読んだフレームの大きさ (フォーマットワードの 4 バイトと、その長さのフィールドの分) だけ進める
    // (MC68030UM 10.2.3.4.2: フォーマットワードの長さのフィールドの分だけ転送する)
    if (mode == 3) cpu_->a_[reg] = a + size;
}

} // namespace m68k
