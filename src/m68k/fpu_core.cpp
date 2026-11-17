// MC68881 / MC68882 の計算の部分。fpu_core.h を参照
#include "m68k/fpu_core.h"

namespace m68k {

using fp::X;
using fp::Ctx;

namespace {

const X kNan = {0x7fff, 0xffffffffffffffffull};

// FPSR の例外ステータス (EXC) から AEXC を作る。MC68881UM 2.3.4 の式:
//   IOP = BSUN | SNAN | OPERR、OVFL = OVFL、UNFL = UNFL & INEX2、DZ = DZ、INEX = INEX1 | INEX2 | OVFL
uint32_t aexcOf(uint32_t fpsr) {
    uint32_t a = 0;
    if (fpsr & (fp::EXC_BSUN | fp::EXC_SNAN | fp::EXC_OPERR)) a |= 0x80;
    if (fpsr & fp::EXC_OVFL) a |= 0x40;
    if ((fpsr & (fp::EXC_UNFL | fp::EXC_INEX2)) == (fp::EXC_UNFL | fp::EXC_INEX2)) a |= 0x20;
    if (fpsr & fp::EXC_DZ) a |= 0x10;
    if (fpsr & (fp::EXC_OVFL | fp::EXC_INEX2 | fp::EXC_INEX1)) a |= 0x08;
    return a;
}

bool isNanX(const X& x) { return fp::isNan(x); }

// 指数 (整数) を X にする
X intX(int32_t v) { return fp::fromInt(v); }

// FSCALE (MC68881UM の FSCALE の表)。NaN は呼ぶ側で処理する。
// 元の値は 0 方向に整数にして、行き先の指数に足す。|元の値| >= 2^14 なら必ずオーバーフローかアンダーフローになる
X fscale(const X& dst, const X& src, Ctx& c) {
    fp::W s = fp::toW(src);
    if (s.cls == fp::W_INF) {
        c.exc |= fp::EXC_OPERR;
        return kNan;
    }
    int32_t n = 0;
    if (s.cls == fp::W_NORM && s.e >= 0) {
        n = s.e >= 14 ? 0x10000 : (int32_t)(s.m >> (127 - s.e));
        if (s.s) n = -n;
    }
    // 行き先が 0 か無限大ならそのまま。元の値が 0 なら行き先を丸めるだけ (fp::scale がする)
    return fp::scale(dst, n, c);
}

// FCMP の条件コード (MC68881UM の FCMP の表)。NaN は呼ぶ側で処理する。I は常に 0。
// 等しいときは Z で、行き先が負なら N も立てる (負の値同士は fputest.x の期待値で確かめた)。等しくなければ行き先 < 元の値で N
uint32_t fcmpCc(const X& dst, const X& src) {
    fp::W a = fp::toW(dst), b = fp::toW(src);
    // 大小を比べるための鍵: 0 < 正規化数 (指数、仮数の順) < 無限大
    auto mag = [](const fp::W& w, const fp::W& o) -> int {
        if (w.cls == o.cls) {
            if (w.cls != fp::W_NORM) return 0;
            if (w.e != o.e) return w.e < o.e ? -1 : 1;
            return w.m == o.m ? 0 : (w.m < o.m ? -1 : 1);
        }
        int rw = w.cls == fp::W_ZERO ? 0 : w.cls == fp::W_NORM ? 1 : 2;
        int ro = o.cls == fp::W_ZERO ? 0 : o.cls == fp::W_NORM ? 1 : 2;
        return rw < ro ? -1 : 1;
    };
    if (a.cls == fp::W_ZERO && b.cls == fp::W_ZERO) return FpuCore::CC_Z | (a.s ? (uint32_t)FpuCore::CC_N : 0u);
    if (a.s != b.s) return a.s ? (uint32_t)FpuCore::CC_N : 0u;
    int m = mag(a, b);
    if (m == 0) return FpuCore::CC_Z | (a.s ? (uint32_t)FpuCore::CC_N : 0u);
    bool less = a.s ? m > 0 : m < 0;
    return less ? (uint32_t)FpuCore::CC_N : 0u;
}

} // namespace

// 例外の優先順位 (MC68881UM 6.1.9: BSUN > SNAN > OPERR > OVFL > UNFL > DZ > INEX2 / INEX1) と
// ベクタ番号 (MC68881UM の表 6-1)
int FpuCore::excVector(uint32_t mask) {
    if (mask & fp::EXC_BSUN) return 48;
    if (mask & fp::EXC_SNAN) return 54;
    if (mask & fp::EXC_OPERR) return 52;
    if (mask & fp::EXC_OVFL) return 53;
    if (mask & fp::EXC_UNFL) return 51;
    if (mask & fp::EXC_DZ) return 50;
    return 49; // INEX2 / INEX1
}

X FpuCore::defaultNan() { return kNan; }

void FpuCore::resetRegisters() {
    for (X& r : fp_) r = kNan;
    fpcr_ = fpsr_ = fpiar_ = 0;
}

uint32_t FpuCore::ccOf(const X& x) {
    if (fp::isNan(x)) return CC_NAN;
    uint32_t cc = fp::sign(x) ? (uint32_t)CC_N : 0u;
    if (fp::isInf(x)) cc |= CC_I;
    else if (fp::isZero(x)) cc |= CC_Z;
    return cc;
}

// 条件 (MC68881UM 4.4.1〜4.4.3 の表)。bit4 は NAN のときに BSUN を立てるかどうかだけで、真偽は下位 4 ビットで決まる。
// OR と NE は、Z と NAN が両方立っているとき (FPSR に直接書いたときだけ起きる) に、表の式 (OR = ¬NAN、NE = ¬Z) と
// 合わない。fputest.x (fbcc、fscc、fdbcc、ftrapcc) の期待値に合わせて、OR = Z ∨ ¬NAN、NE = NAN ∨ ¬Z に
// している (表の式にすると、MI+ZE+IN+NA の組み合わせで fbcc が 16 件ずつ落ちる)
bool FpuCore::cond881(int cc) const {
    bool n = (fpsr_ & CC_N) != 0, z = (fpsr_ & CC_Z) != 0, nan = (fpsr_ & CC_NAN) != 0;
    switch (cc & 15) {
    case 0: return false;                    // F
    case 1: return z;                        // EQ
    case 2: return !(nan || z || n);         // OGT
    case 3: return z || !(nan || n);         // OGE
    case 4: return n && !(nan || z);         // OLT
    case 5: return z || (n && !nan);         // OLE
    case 6: return !(nan || z);              // OGL
    case 7: return z || !nan;                // OR
    case 8: return nan;                      // UN
    case 9: return nan || z;                 // UEQ
    case 10: return nan || !(n || z);        // UGT
    case 11: return nan || z || !n;          // UGE
    case 12: return nan || (n && !z);        // ULT
    case 13: return nan || z || n;           // ULE
    case 14: return nan || !z;               // NE
    default: return true;                    // T
    }
}

Ctx FpuCore::ctx() const {
    Ctx c;
    int p = (fpcr_ >> 6) & 3;
    c.prec = p == 1 ? fp::PREC_S : p == 0 ? fp::PREC_X : fp::PREC_D;
    c.rnd = (fpcr_ >> 4) & 3;
    return c;
}

void FpuCore::accrue() { fpsr_ |= aexcOf(fpsr_); }

X FpuCore::canon(const X& x) {
    if (fp::isNan(x)) return kNan;
    if (fp::isInf(x)) return fp::makeInf(fp::sign(x));
    if (x.m == 0) return fp::makeZero(fp::sign(x));
    Ctx c;
    return fp::round(fp::toW(x), c, fp::PREC_X); // 正規化する (正確)
}

uint32_t FpuCore::enabledExceptions() const {
    uint32_t mask = fpcr_ & fpsr_ & 0xff00;
    // オーバーフローで、OVFL の例外は禁止、INEX の例外が許可なら、INEX の例外になる (MC68881UM 6.1.7 の NOTE)
    if (!mask && (fpsr_ & fp::EXC_OVFL) && (fpcr_ & (fp::EXC_INEX2 | fp::EXC_INEX1))) mask = fp::EXC_INEX2;
    return mask;
}

void FpuCore::fmovecr(int off, int n, Ctx& c) {
    // ROM の定数を FPCR の精度と方向で丸めて入れる (MC68881UM の FMOVECR)
    X x = fp::constant(off, c);
    fp_[n] = x;
    uint32_t cc = ccOf(x);
    // 資料にない $03 と $07 は、実機では NaN でないのに NAN が立つ。$03 は丸めで繰り上がると
    // (仮数の bit63 が立つと) I になる。fputest.x の実機の結果に合わせたもの
    if (off == 0x03) cc = (x.m >> 63) ? CC_I : CC_NAN;
    if (off == 0x07) cc = CC_NAN;
    fpsr_ = (fpsr_ & 0x00ff00ff) | cc | c.exc;
}

// 68060 の内蔵 FPU にある演算のサイクル数 (MC68060UM 表 10-25 の FPn どうしの値)。下の arith() の cycles() の 2 つめの
// 引数と同じ値で、NaN のときに演算をせずに戻るところで使う
static int cycles060Of(int opmode) {
    switch (opmode) {
    case 0x00: return 1;             // FMOVE
    case 0x01: case 0x03: return 3;  // FINT、FINTRZ
    case 0x04: case 0x05: return 68; // FSQRT
    case 0x18: case 0x1a: case 0x1b: return 1; // FABS、FNEG
    case 0x20: case 0x24: return 37; // FDIV、FSGLDIV
    case 0x22: case 0x23: case 0x27: return 3; // FADD、FMUL、FSGLMUL
    default:
        if (opmode >= 0x28 && opmode <= 0x2f) return 3; // FSUB
        if (opmode >= 0x38 && opmode <= 0x3f) return 1; // FCMP、FTST
        return 0;
    }
}

bool FpuCore::arith(int opmode, const X& src, int n, Ctx& c, uint32_t& ccOut, bool& setCc) {
    X& dst = fp_[n];
    int prec = c.prec;
    bool srcNan = isNanX(src);
    bool dyadic = (opmode >= 0x20 && opmode <= 0x2f) || (opmode >= 0x38 && opmode <= 0x3d && opmode != 0x3a && opmode != 0x3b);
    setCc = true;
    // シグナリング NaN (FMOVEM で入れたレジスタ) は SNAN を立てる (MC68881UM 6.1.2)
    if (fp::isSnan(src) || (dyadic && fp::isSnan(dst))) c.exc |= fp::EXC_SNAN;
    // NaN の演算数があれば、結果は既定の NaN (仮数がすべて 1) にする (FCMP / FTST は条件コードだけ)。
    // MC68881UM 4.5.4.1 は「演算数の NaN をそのまま返す (両方が NaN なら行き先の NaN)」としていて、既定でない NaN では
    // 食い違う。fputest.x には既定でない NaN の演算数がなく、どちらでも通る
    if (srcNan || (dyadic && isNanX(dst))) {
        cycles(0, cycles060Of(opmode)); // 68060 は NaN でも同じサイクル数 (68882 の値は今までどおり足さない)
        if (opmode >= 0x38) { // FCMP / FTST
            ccOut = CC_NAN;
            return true;
        }
        if (opmode >= 0x30 && opmode <= 0x37) fp_[opmode & 7] = kNan; // FSINCOS
        dst = kNan;
        ccOut = CC_NAN;
        return true;
    }
    X r;
    switch (opmode) {
    case 0x00: r = fp::round(fp::toW(src), c, prec); cycles(4, 1); break; // FMOVE
    case 0x01: r = fp::roundInt(src, c, c.rnd); cycles(60, 3); break;    // FINT
    case 0x02: r = fp::fsinh(src, c); cycles(600); break;
    case 0x03: r = fp::roundInt(src, c, fp::RND_Z); cycles(55, 3); break; // FINTRZ
    case 0x04:
    case 0x05: r = fp::sqrt(src, c, prec); cycles(105, 68); break;
    case 0x06:
    case 0x07: r = fp::flognp1(src, c); cycles(570); break;
    case 0x08: r = fp::fetoxm1(src, c); cycles(590); break;
    case 0x09: r = fp::ftanh(src, c); cycles(625); break;
    case 0x0a:
    case 0x0b: r = fp::fatan(src, c); cycles(400); break;
    case 0x0c: r = fp::fasin(src, c); cycles(580); break;
    case 0x0d: r = fp::fatanh(src, c); cycles(700); break;
    case 0x0e: r = fp::fsin(src, c); cycles(390); break;
    case 0x0f: r = fp::ftan(src, c); cycles(470); break;
    case 0x10: r = fp::fetox(src, c); cycles(500); break;
    case 0x11: r = fp::ftwotox(src, c); cycles(570); break;
    case 0x12:
    case 0x13: r = fp::ftentox(src, c); cycles(570); break;
    case 0x14: r = fp::flogn(src, c); cycles(520); break;
    case 0x15: r = fp::flog10(src, c); cycles(570); break;
    case 0x16:
    case 0x17: r = fp::flog2(src, c); cycles(570); break;
    case 0x18:
    case 0x1a:
    case 0x1b: { // FABS / FNEG
        X t = src;
        if (opmode == 0x18) t.se &= 0x7fff;
        else t.se ^= 0x8000;
        r = fp::round(fp::toW(t), c, prec);
        if (fp::isZero(t)) r = fp::makeZero(fp::sign(t));
        cycles(35, 1);
        break;
    }
    case 0x19: r = fp::fcosh(src, c); cycles(600); break;
    case 0x1c: r = fp::facos(src, c); cycles(600); break;
    case 0x1d: r = fp::fcos(src, c); cycles(390); break;
    case 0x1e: { // FGETEXP
        fp::W w = fp::toW(src);
        if (w.cls == fp::W_INF) {
            c.exc |= fp::EXC_OPERR;
            r = kNan;
        } else if (w.cls == fp::W_ZERO) {
            r = src;
        } else {
            r = intX(w.e);
        }
        cycles(40);
        break;
    }
    case 0x1f: { // FGETMAN
        fp::W w = fp::toW(src);
        if (w.cls == fp::W_INF) {
            c.exc |= fp::EXC_OPERR;
            r = kNan;
        } else if (w.cls == fp::W_ZERO) {
            r = src;
        } else {
            r = X{(uint16_t)((w.s ? 0x8000 : 0) | 0x3fff), (uint64_t)(w.m >> 64)};
        }
        cycles(30);
        break;
    }
    case 0x20: r = fp::div(dst, src, c, prec); cycles(105, 37); break;
    case 0x21:
    case 0x25: { // FMOD / FREM
        int q;
        bool qneg;
        r = fp::remainder(dst, src, c, opmode == 0x25, q, qneg);
        fpsr_ = (fpsr_ & 0xff00ffff) | ((uint32_t)(qneg ? 0x80 : 0) | (uint32_t)q) << 16;
        cycles(100);
        break;
    }
    case 0x22: r = fp::add(dst, src, c, prec); cycles(55, 3); break;
    case 0x23: r = fp::mul(dst, src, c, prec); cycles(75, 3); break;
    case 0x24: r = fp::div(dst, src, c, fp::PREC_XS); cycles(70, 37); break; // FSGLDIV
    case 0x26: // FSCALE
        r = fscale(dst, src, c);
        cycles(45);
        break;
    case 0x27: r = fp::sglmul(dst, src, c); cycles(60, 3); break; // FSGLMUL
    case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f:
        r = fp::sub(dst, src, c, prec);
        cycles(55, 3);
        break;
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: { // FSINCOS
        X s = src;
        fp_[opmode & 7] = fp::fcos(s, c);
        r = fp::fsin(s, c);
        cycles(450);
        break;
    }
    case 0x38: case 0x39: case 0x3c: case 0x3d: // FCMP
        ccOut = fcmpCc(dst, src);
        cycles(40, 1);
        return true;
    case 0x3a: case 0x3b: case 0x3e: case 0x3f: // FTST
        ccOut = ccOf(src);
        cycles(30, 1);
        return true;
    default:
        return false;
    }
    dst = r;
    ccOut = ccOf(r);
    return true;
}

// ---------------------------------------------------------------- オペランドの形 (Fpu::readSource / writeDest と同じ変換)

int FpuCore::operandSize(int format) {
    static const int sizes[8] = {4, 4, 12, 12, 2, 8, 1, 12};
    return sizes[format & 7];
}

X FpuCore::loadOperand(int format, const uint32_t* w, Ctx& c) {
    X out;
    switch (format) {
    case 0: out = fp::fromInt((int32_t)w[0]); break;
    case 1: out = fp::fromSingle(w[0], c); break;
    case 2: {
        X x{(uint16_t)(w[0] >> 16), ((uint64_t)w[1] << 32) | w[2]};
        if (fp::isSnan(x)) c.exc |= fp::EXC_SNAN;
        out = canon(x);
        break;
    }
    case 3: out = fp::fromPacked(w, c); break;
    case 4: out = fp::fromInt((int16_t)(w[0] >> 16)); break;
    case 5: out = fp::fromDouble(((uint64_t)w[0] << 32) | w[1], c); break;
    default: out = fp::fromInt((int8_t)(w[0] >> 24)); break;
    }
    if (fp::isNan(out)) out = defaultNan();
    return out;
}

void FpuCore::storeOperand(const X& x, int format, int k, Ctx& c, uint32_t* w) {
    switch (format) {
    case 0: w[0] = (uint32_t)fp::toInt(x, c, 32); break;
    case 1: w[0] = fp::toSingle(x, c); break;
    case 2: {
        // 非正規化数は UNFL を立てる。シグナリング NaN は SNAN を立て、SNAN のビットを立てて書く (writeDest と同じ)
        if ((x.se & 0x7fff) == 0 && x.m != 0 && !(x.m >> 63)) c.exc |= fp::EXC_UNFL;
        uint64_t m = x.m;
        if (fp::isSnan(x)) {
            c.exc |= fp::EXC_SNAN;
            m |= 0x4000000000000000ull;
        }
        w[0] = (uint32_t)x.se << 16;
        w[1] = (uint32_t)(m >> 32);
        w[2] = (uint32_t)m;
        break;
    }
    case 3:
    case 7: fp::toPacked(x, k, c, w); break;
    case 4: w[0] = (uint32_t)fp::toInt(x, c, 16) << 16; break;
    case 5: {
        uint64_t v = fp::toDouble(x, c);
        w[0] = (uint32_t)(v >> 32);
        w[1] = (uint32_t)v;
        break;
    }
    default: w[0] = (uint32_t)fp::toInt(x, c, 8) << 24; break;
    }
}

} // namespace m68k
