// FPU の浮動小数点演算 (ソフトウェア)。fpu_math.h を参照
#include "m68k/fpu_math.h"

namespace m68k {
namespace fp {

#include "m68k/fpu_consts.inc"

namespace {

const u128 ONE128 = (u128)1 << 127; // W で 1.0 の仮数

int clz128(u128 v) {
    uint64_t hi = (uint64_t)(v >> 64);
    if (hi) return __builtin_clzll(hi);
    uint64_t lo = (uint64_t)v;
    return lo ? 64 + __builtin_clzll(lo) : 128;
}

void normalize(W& w) {
    if (w.cls != W_NORM) return;
    if (w.m == 0) {
        w.cls = W_ZERO;
        return;
    }
    int n = clz128(w.m);
    w.m <<= n;
    w.e -= n;
}

u128 shr(u128 v, int n, bool& st) {
    if (n <= 0) return v;
    if (n >= 128) {
        if (v) st = true;
        return 0;
    }
    if (v & (((u128)1 << n) - 1)) st = true;
    return v >> n;
}

W wzero(int s = 0) { return W{s, 0, 0, false, W_ZERO}; }

W wfromU128(u128 v, int s = 0) {
    W w{s, 127, v, false, W_NORM};
    normalize(w);
    return w;
}

W wfromInt(int64_t v) {
    if (v < 0) return wfromU128((u128)(uint64_t)(-(v + 1)) + 1, 1);
    return wfromU128((u128)(uint64_t)v, 0);
}

W wone() { return W{0, 0, ONE128, false, W_NORM}; }

// |a| と |b| を比べる (NORM と ZERO だけ)
int cmpAbs(const W& a, const W& b) {
    if (a.cls == W_ZERO) return b.cls == W_ZERO ? 0 : -1;
    if (b.cls == W_ZERO) return 1;
    if (a.e != b.e) return a.e < b.e ? -1 : 1;
    if (a.m != b.m) return a.m < b.m ? -1 : 1;
    return 0;
}

// 加算。rnd は 0 になったときの符号を決めるのに使う (RM なら -0)
W wadd(const W& a, const W& b, int rnd = RND_N) {
    if (a.cls == W_ZERO && b.cls == W_ZERO) {
        if (a.s == b.s) return wzero(a.s);
        return wzero(rnd == RND_M ? 1 : 0);
    }
    if (a.cls == W_ZERO) return b;
    if (b.cls == W_ZERO) return a;
    const W* x = &a;
    const W* y = &b;
    if (cmpAbs(a, b) < 0) {
        x = &b;
        y = &a;
    }
    bool st = x->sticky;
    u128 mx = x->m >> 1;
    if (x->m & 1) st = true;
    bool sty = y->sticky;
    int64_t d = (int64_t)x->e - y->e;
    u128 my = shr(y->m >> 1, d > 200 ? 200 : (int)d, sty);
    if (y->m & 1) sty = true;
    W r{x->s, x->e + 1, 0, false, W_NORM};
    if (x->s == y->s) {
        r.m = mx + my;
        r.sticky = st || sty;
    } else {
        r.m = mx - my;
        if (sty) {
            r.m -= 1; // 引く数の下のビットの分だけ減らし、あとは sticky で表す
            r.sticky = true;
        } else {
            r.sticky = st;
        }
        if (r.m == 0 && !r.sticky) return wzero(rnd == RND_M ? 1 : 0);
    }
    normalize(r);
    return r;
}

W wneg(W a) {
    a.s ^= 1;
    return a;
}

W wsub(const W& a, const W& b, int rnd = RND_N) { return wadd(a, wneg(b), rnd); }

// 128 x 128 → 上位 128 ビット (下位が 0 でなければ sticky)
u128 mulHi(u128 a, u128 b, bool& st) {
    uint64_t a1 = (uint64_t)(a >> 64), a0 = (uint64_t)a, b1 = (uint64_t)(b >> 64), b0 = (uint64_t)b;
    u128 p00 = (u128)a0 * b0, p01 = (u128)a0 * b1, p10 = (u128)a1 * b0, p11 = (u128)a1 * b1;
    u128 mid = (p00 >> 64) + (uint64_t)p01 + (uint64_t)p10;
    uint64_t lo = (uint64_t)p00;
    uint64_t midLo = (uint64_t)mid;
    if (lo || midLo) st = true;
    return p11 + (p01 >> 64) + (p10 >> 64) + (mid >> 64);
}

W wmul(const W& a, const W& b) {
    int s = a.s ^ b.s;
    if (a.cls == W_ZERO || b.cls == W_ZERO) return wzero(s);
    bool st = a.sticky || b.sticky;
    W r{s, a.e + b.e + 1, mulHi(a.m, b.m, st), false, W_NORM};
    r.sticky = st;
    normalize(r);
    return r;
}

// 割り算 (b は 0 でない)。商を 128 ビット求め、余りが残れば sticky
W wdiv(const W& a, const W& b) {
    int s = a.s ^ b.s;
    if (a.cls == W_ZERO) return wzero(s);
    u128 rem = a.m, mb = b.m, q = 0;
    int32_t e = a.e - b.e;
    bool carry = false;
    if (rem < mb) {
        e--;
        carry = (rem >> 127) != 0;
        rem <<= 1;
    }
    for (int i = 0; i < 128; i++) {
        bool bit = carry || rem >= mb;
        if (bit) rem -= mb;
        q = (q << 1) | (bit ? 1 : 0);
        carry = (rem >> 127) != 0;
        rem <<= 1;
    }
    W r{s, e, q, rem != 0 || carry || a.sticky || b.sticky, W_NORM};
    normalize(r);
    return r;
}

W wscale(W a, int32_t n) {
    if (a.cls == W_NORM) a.e += n;
    return a;
}

// double (ホストの浮動小数点) との変換。初期値の計算にだけ使う
double toDbl(const W& w) {
    if (w.cls == W_ZERO) return 0.0;
    double m = (double)(uint64_t)(w.m >> 64) * (1.0 / 9223372036854775808.0); // [1, 2)
    int e = w.e;
    if (e > 1000) e = 1000;
    if (e < -1000) e = -1000;
    double p = 1.0;
    double b = e >= 0 ? 2.0 : 0.5;
    for (int k = e >= 0 ? e : -e; k; k >>= 1, b *= b)
        if (k & 1) p *= b;
    return w.s ? -m * p : m * p;
}

W fromDbl(double d) {
    if (d == 0) return wzero();
    uint64_t bits;
    __builtin_memcpy(&bits, &d, 8);
    int s = (int)(bits >> 63);
    int ex = (int)((bits >> 52) & 0x7ff);
    uint64_t frac = bits & 0xfffffffffffffull;
    W w{s, ex - 1023, ((u128)((1ull << 52) | frac)) << 75, false, W_NORM};
    return w;
}

// double の平方根 (d > 0)。ベアメタルには libm がないので、AArch64 では命令を直接使う
double hostSqrt(double d) {
#if defined(__aarch64__)
    double r;
    __asm__("fsqrt %d0, %d1" : "=w"(r) : "w"(d));
    return r;
#else
    return __builtin_sqrt(d);
#endif
}

// 平方根 (内部用)。ニュートン法
W wsqrt(const W& a) {
    if (a.cls == W_ZERO) return a;
    // 指数を偶数にそろえて double で初期値を作る
    W x = a;
    int32_t half = x.e >> 1;
    x.e -= half * 2;
    W y = fromDbl(hostSqrt(toDbl(x)));
    for (int i = 0; i < 3; i++) y = wscale(wadd(y, wdiv(x, y)), -1);
    y.e += half;
    y.sticky = true;
    return y;
}

int64_t wToIntRound(const W& w) { // 最も近い整数 (|w| < 2^62)
    if (w.cls == W_ZERO || w.e < -1) return 0;
    int sh = 127 - w.e;
    u128 v = w.m >> (sh - 1);
    v = (v + 1) >> 1;
    int64_t r = (int64_t)v;
    return w.s ? -r : r;
}

// 級数の項 t が和 s に対して 2^-bits より小さいか。級数は bits = 126 で打ち切る: wadd() で仮数 (128 ビット) の下に
// 丸ごとはみ出す最初の項まで足し、その項の符号を丸めの方向として残す (足せば sticky か、逆の符号なら 1 減らして
// sticky)。さらに小さい逆の符号の項を足すと、sticky の上からもう 1 減らしてしまい、方向を誤る
bool tinyRel(const W& t, const W& s, int bits) { return t.cls == W_ZERO || (s.cls != W_ZERO && t.e < s.e - bits); }

// e^x (|x| < 2^20 程度)
W wexp(const W& x) {
    if (x.cls == W_ZERO) return wone();
    int64_t k = wToIntRound(wmul(x, kLog2e));
    W r = wsub(x, wmul(wfromInt(k), kLn2));
    r = wscale(r, -8);
    W sum = wone(), term = wone();
    for (int n = 1; n < 40; n++) {
        term = wdiv(wmul(term, r), wfromInt(n));
        sum = wadd(sum, term);
        if (tinyRel(term, sum, 126)) break;
    }
    for (int i = 0; i < 8; i++) sum = wmul(sum, sum);
    sum.e += (int32_t)k;
    sum.sticky = true;
    return sum;
}

// e^x - 1 (|x| < 2^20 程度)。|x| が小さいときはテイラー級数で桁落ちを避ける
W wexpm1(const W& x) {
    if (x.cls == W_ZERO) return x;
    if (x.e < -2) {
        W sum = x, term = x;
        for (int n = 2; n < 60; n++) {
            term = wdiv(wmul(term, x), wfromInt(n));
            sum = wadd(sum, term);
            if (tinyRel(term, sum, 126)) break;
        }
        sum.sticky = true;
        return sum;
    }
    W r = wsub(wexp(x), wone());
    r.sticky = true;
    return r;
}

// 2 atanh(z) = z + z^3/3 + ... の 2 倍
W watanh2(const W& z) {
    W z2 = wmul(z, z);
    W sum = z, t = z;
    for (int n = 3; n < 400; n += 2) {
        t = wmul(t, z2);
        W term = wdiv(t, wfromInt(n));
        sum = wadd(sum, term);
        if (tinyRel(term, sum, 126)) break;
    }
    sum.e++;
    return sum;
}

// log(x) (x > 0)
W wlog(const W& x) {
    W m = x;
    int64_t e = m.e;
    m.e = 0;
    // m が sqrt(2) より大きければ 2 で割る
    const u128 SQRT2 = ((u128)0xb504f333f9de6484ull << 64) | 0x597d89b3754abe9full;
    if (m.m > SQRT2) {
        m.e = -1;
        e++;
    }
    W z = wdiv(wsub(m, wone()), wadd(m, wone()));
    W r = z.cls == W_ZERO ? z : watanh2(z);
    if (e) r = wadd(r, wmul(wfromInt(e), kLn2));
    r.sticky = true;
    return r;
}

// log(1 + x) (x > -1)
W wlog1p(const W& x) {
    if (x.cls == W_ZERO) return x;
    if (x.e < -2) {
        W two = wfromInt(2);
        W z = wdiv(x, wadd(two, x));
        W r = watanh2(z);
        r.sticky = true;
        return r;
    }
    return wlog(wadd(wone(), x));
}

// atan(x) (|x| <= 1)
W watanSmall(const W& x0) {
    if (x0.cls == W_ZERO) return x0;
    W x = x0;
    for (int i = 0; i < 3; i++) x = wdiv(x, wadd(wone(), wsqrt(wadd(wone(), wmul(x, x)))));
    W x2 = wmul(x, x);
    W sum = x, t = x;
    for (int n = 3; n < 200; n += 2) {
        t = wneg(wmul(t, x2));
        W term = wdiv(t, wfromInt(n));
        sum = wadd(sum, term);
        if (tinyRel(term, sum, 126)) break;
    }
    sum.e += 3;
    sum.sticky = true;
    return sum;
}

W watan(const W& x) {
    if (x.cls == W_ZERO) return x;
    W ax = x;
    ax.s = 0;
    W r;
    if (cmpAbs(ax, wone()) > 0) r = wsub(kPiHalf, watanSmall(wdiv(wone(), ax)));
    else r = watanSmall(ax);
    r.s = x.s;
    r.sticky = true;
    return r;
}

// sin と cos の級数 (|r| <= pi/4)
void wsincosSeries(const W& r, W& s, W& c) {
    W r2 = wmul(r, r);
    s = r;
    W t = r;
    for (int n = 2; n < 80; n += 2) {
        t = wneg(wdiv(wmul(t, r2), wfromInt((int64_t)n * (n + 1))));
        s = wadd(s, t);
        if (tinyRel(t, s, 126)) break;
    }
    c = wone();
    t = wone();
    for (int n = 1; n < 80; n += 2) {
        t = wneg(wdiv(wmul(t, r2), wfromInt((int64_t)n * (n + 1))));
        c = wadd(c, t);
        if (tinyRel(t, c, 126)) break;
    }
    s.sticky = c.sticky = true;
}

// 2/pi の i 番目 (1 から) から 64 ビット
uint64_t twoOverPiBits(int64_t i) {
    int64_t b = i - 1; // 0 始まりのビット位置
    if (b < 0) return 0;
    int64_t w = b >> 6;
    int sh = (int)(b & 63);
    uint64_t hi = w < 260 ? kTwoOverPi[w] : 0;
    uint64_t lo = w + 1 < 260 ? kTwoOverPi[w + 1] : 0;
    return sh ? (hi << sh) | (lo >> (64 - sh)) : hi;
}

// x を pi/2 で割った余り r (|r| <= pi/4) と、商の下位 2 ビット n
void reduce(const X& a, W& r, int& n) {
    W w = toW(a);
    int s0 = w.s;
    w.s = 0;
    // |x| <= pi/4 ならそのまま
    W piq = wscale(kPiHalf, -1);
    if (cmpAbs(w, piq) <= 0) {
        r = w;
        r.s = s0;
        n = 0;
        return;
    }
    uint64_t M = (uint64_t)(w.m >> 64);
    int64_t s = (int64_t)w.e - 63; // M の最下位ビットの指数
    int64_t i0 = s - 1 > 1 ? s - 1 : 1;
    // B = 2/pi の i0 番目から 384 ビット。P = M x B (448 ビット、limb[0] が最下位)。小数部は P の下位 382 ビット
    // (i0 = s - 1 のとき。下の bp)。B を途中で切るので P の下位 64 ビットは正しくなく、小数部で正しいのは上位の約 318 ビット。
    // 2/pi の表 (kTwoOverPi) は 260 x 64 = 16640 ビットまでで、その先は 0 として読む (twoOverPiBits())。
    // i0 + 383 が 16640 を超える指数 (16322 以上) では、超えた分だけ正しいビットが減り、最大の指数 (16383。62 ビット超える)
    // では約 256 ビット (382 - 64 - 62)。下で取る 128 ビットには足りている (h の後ろの注釈)
    const int NB = 6;
    uint64_t B[NB];
    for (int k = 0; k < NB; k++) B[k] = twoOverPiBits(i0 + 64 * (NB - 1 - k)); // B[0] が最下位
    uint64_t P[NB + 1];
    uint64_t carry = 0;
    for (int k = 0; k < NB; k++) {
        u128 t = (u128)M * B[k] + carry;
        P[k] = (uint64_t)t;
        carry = (uint64_t)(t >> 64);
    }
    P[NB] = carry;
    const int64_t PBITS = 64 * (NB + 1);
    int64_t bp = i0 + 64 * NB - 1 - s; // 小数点の位置 (P のビット番号)
    auto bit = [&](int64_t b) -> int { return b >= 0 && b < PBITS ? (int)((P[b >> 6] >> (b & 63)) & 1) : 0; };
    int q = (bit(bp + 1) << 1) | bit(bp);
    bool neg = false;
    if (bit(bp - 1)) { // 小数部が 0.5 以上なら次の整数に丸め、小数部を 1 から引く (bp より下の 2 の補数)
        q = (q + 1) & 3;
        neg = true;
        uint64_t c1 = 1;
        for (int k = 0; k <= NB; k++) {
            P[k] = ~P[k] + c1;
            c1 = (c1 && P[k] == 0) ? 1 : 0;
        }
    }
    // 小数部の最上位の 1 から 128 ビットを取る。x が pi/2 の倍数に近いと小数部の上位に 0 が続くので、
    // 小数点のすぐ下から固定の 128 ビットを取ると、有効な桁が足りなくなる
    int64_t h = bp - 1;
    while (h >= 0 && !bit(h)) h--;
    // 64 ビットの仮数の x では、x * 2/pi と最も近い整数との差は 2^-76.19 以上 (指数 -1 から 16383 のそれぞれで、
    // 2/pi の連分数から求めた下限。最も小さいのは指数 10594。tools/fpu_reduce_bound.py)。小数部の最上位の 1 は
    // bp - 78 より上にあり、取る 128 ビットの最下位は bp - 205 より上で、最大の指数でも正しい範囲 (bp - 256 より上) に入る。
    // 小数部が全部 0 になること (h < 0) もない
    if (h < 0) { // 64 ビットの仮数では起きない
        r = wzero(s0);
        n = s0 ? (4 - q) & 3 : q;
        return;
    }
    u128 f = 0;
    for (int k = 0; k < 128; k++) f = (f << 1) | (u128)bit(h - k);
    W fw{neg ? 1 : 0, (int32_t)(h - bp), f, true, W_NORM};
    r = wmul(fw, kPiHalf);
    if (s0) {
        r.s ^= 1;
        q = (4 - q) & 3;
    }
    n = q;
}

// 丸めの一般形。
// p は形式の仮数のビット数、precBits は丸める桁数 (p 以下)、bias は指数のバイアス、
// minBe は正規化数の最小の指数 (68881 の拡張精度は 0、IEEE の単精度・倍精度は 1)、beMax は最大の指数。
// 68881 の拡張精度は整数部のビットを持ち、指数 0 の重みは 2^-16383 (x87 と違う)
struct Rounded {
    bool zero = false, inf = false;
    int32_t be = 0;
    uint64_t mant = 0; // 形式の仮数 (p ビット、整数部のビットを含む)。値 = mant x 2^(max(be, minBe) - bias - (p - 1))
};

void roundCore(const W& w, Ctx& c, int p, int precBits, int bias, int minBe, int beMax, Rounded& out) {
    out = Rounded();
    int32_t be = w.e + bias;
    bool tiny = be < minBe;
    const int keepPrec = precBits < p ? precBits : p;
    int keep = keepPrec;
    if (tiny) {
        int64_t k2 = (int64_t)keep - (minBe - be); // 仮数だけを丸めるとき (FSGLMUL など) も、丸める位置は形式の仮数の中で固定
        if (k2 < -2) k2 = -2;
        if (k2 < keep) keep = (int)k2;
    }
    int drop = 128 - keep;
    u128 kept;
    bool roundBit, st = w.sticky;
    if (drop >= 129) {
        kept = 0;
        roundBit = false;
        if (w.m) st = true;
    } else if (drop == 128) {
        kept = 0;
        roundBit = (w.m >> 127) & 1;
        if (w.m << 1) st = true;
    } else {
        kept = w.m >> drop;
        roundBit = (w.m >> (drop - 1)) & 1;
        if (drop >= 2 && (w.m & (((u128)1 << (drop - 1)) - 1))) st = true;
    }
    bool inexact = roundBit || st;
    bool inc = false;
    switch (c.rnd) {
    case RND_N: inc = roundBit && (st || (kept & 1)); break;
    case RND_Z: break;
    case RND_M: inc = inexact && w.s; break;
    case RND_P: inc = inexact && !w.s; break;
    }
    if (inc) kept++;
    if (inexact) c.exc |= EXC_INEX2;
    if (!tiny) {
        if (kept >> keep) {
            kept >>= 1;
            be++;
        }
        if (be > beMax) {
            // OVFL を立てる (MC68881UM 6.1.4)。INEX2 は仮数に端数があったときだけ立てる (6.1.7: INEX2 は、仮数が
            // 丸めの精度で正確に表せないとき)。オーバーフローでいつも INEX2 を立てると fputest.x の fadd などが落ちる
            c.exc |= EXC_OVFL;
            bool toInf = c.rnd == RND_N || (c.rnd == RND_M && w.s) || (c.rnd == RND_P && !w.s);
            if (toInf) {
                out.inf = true;
            } else {
                out.be = beMax;
                int k = minBe == 0 ? p : precBits; // 拡張精度の形式なら (FSGLMUL などでも) 64 ビットの最大値
                out.mant = (k >= 64 ? ~0ull : ((1ull << k) - 1)) << (p - k);
            }
            return;
        }
        out.be = be;
        out.mant = (uint64_t)(kept << (p - keep));
        return;
    }
    // 非正規化数の範囲。UNFL は正確でも立てる (MC68881UM 6.1.5: 中間結果の指数が、丸めの精度の最小の指数以下なら
    // アンダーフロー。正確かどうかは条件にない)。
    // fputest.x の期待値に合わせ、丸める前に判定する。ただし仮数だけを 24 ビットに丸めるとき
    // (FSGLMUL / FSGLDIV) は、丸めた結果が正規化数 (2^-16383) になったら立てない
    int sh = be - minBe + p - keep; // keep を -2 で止めたときだけ小さくなる (kept は 0 か 1 で、最小の単位にする)
    if (sh < p - keepPrec) sh = p - keepPrec;
    uint64_t mant = (uint64_t)(kept << sh);
    if (mant == 0) {
        c.exc |= EXC_UNFL;
        out.zero = true;
        return;
    }
    bool normal = (mant >> (p - 1)) != 0;
    if (c.tinyAfterRounding ? !normal : (minBe == 1 || keepPrec == p || !normal)) c.exc |= EXC_UNFL;
    out.be = (minBe == 1 && normal) ? 1 : 0;
    out.mant = mant;
}

X packX(int s, const Rounded& r) {
    X x;
    if (r.zero) return makeZero(s);
    if (r.inf) return makeInf(s);
    x.se = (uint16_t)((s ? 0x8000 : 0) | r.be);
    x.m = r.mant;
    return x;
}

X specialResult(const W& w) {
    if (w.cls == W_ZERO) return makeZero(w.s);
    if (w.cls == W_INF) return makeInf(w.s);
    return defaultNan();
}

W pow10w(int n) { // 10^n (n >= 0)
    W r = wone(), b = wfromInt(10);
    for (; n; n >>= 1, b = wmul(b, b))
        if (n & 1) r = wmul(r, b);
    return r;
}

} // namespace

X defaultNan() { return X{0x7fff, 0xffffffffffffffffull}; }
X makeZero(bool neg) { return X{(uint16_t)(neg ? 0x8000 : 0), 0}; }
X makeInf(bool neg) { return X{(uint16_t)(neg ? 0xffff : 0x7fff), 0}; }

W toW(const X& x) {
    W w{(x.se & 0x8000) ? 1 : 0, 0, 0, false, W_NORM};
    int ex = x.se & 0x7fff;
    if (ex == 0x7fff) {
        w.cls = (x.m << 1) ? W_NAN : W_INF; // 無限大は整数部のビットを見ない
        w.m = (u128)x.m << 64;
        return w;
    }
    if (x.m == 0) {
        w.cls = W_ZERO;
        return w;
    }
    w.e = ex - 16383;
    w.m = (u128)x.m << 64;
    normalize(w);
    return w;
}

X round(const W& w, Ctx& c, int prec) {
    if (w.cls != W_NORM) return specialResult(w);
    Rounded r;
    switch (prec) {
    case PREC_S:
    case PREC_D: {
        // 単精度・倍精度の指数の範囲で丸め、拡張精度に戻す (MC68881UM 6.1.4 と 6.1.5 の NOTE: 仮数も指数も選んだ
        // 精度に丸めてから、拡張精度の形式で入れる)
        bool sgl = prec == PREC_S;
        int p = sgl ? 24 : 53, bias = sgl ? 127 : 1023;
        roundCore(w, c, p, p, bias, 1, sgl ? 254 : 2046, r);
        if (r.zero) return makeZero(w.s);
        if (r.inf) return makeInf(w.s);
        W v{w.s, (r.be < 1 ? 1 : r.be) - bias - (p - 1) + 127, (u128)r.mant, false, W_NORM};
        normalize(v);
        return X{(uint16_t)((w.s ? 0x8000 : 0) | (v.e + 16383)), (uint64_t)(v.m >> 64)};
    }
    default:
        roundCore(w, c, 64, prec == PREC_XS ? 24 : prec == PREC_XD ? 53 : 64, 16383, 0, 0x7ffe, r);
        return packX(w.s, r);
    }
}

X roundCtx(const W& w, Ctx& c) { return round(w, c, c.prec); }

X add(const X& a, const X& b, Ctx& c, int prec) {
    W wa = toW(a), wb = toW(b);
    if (wa.cls == W_INF || wb.cls == W_INF) {
        if (wa.cls == W_INF && wb.cls == W_INF && wa.s != wb.s) {
            c.exc |= EXC_OPERR;
            return defaultNan();
        }
        return makeInf(wa.cls == W_INF ? wa.s : wb.s);
    }
    W r = wadd(wa, wb, c.rnd);
    return round(r, c, prec);
}

X sub(const X& a, const X& b, Ctx& c, int prec) {
    X nb = b;
    nb.se ^= 0x8000;
    return add(a, nb, c, prec);
}

X mul(const X& a, const X& b, Ctx& c, int prec) {
    W wa = toW(a), wb = toW(b);
    int s = wa.s ^ wb.s;
    if (wa.cls == W_INF || wb.cls == W_INF) {
        if (wa.cls == W_ZERO || wb.cls == W_ZERO) {
            c.exc |= EXC_OPERR;
            return defaultNan();
        }
        return makeInf(s);
    }
    return round(wmul(wa, wb), c, prec);
}

X sglmul(const X& a, const X& b, Ctx& c) {
    W wa = toW(a), wb = toW(b);
    // 正規化した仮数を上位 24 ビットに切り捨てる (INEX2 は立てない)
    const u128 mask = ~(((u128)1 << 104) - 1);
    if (wa.cls == W_NORM) wa.m &= mask;
    if (wb.cls == W_NORM) wb.m &= mask;
    if (wa.cls == W_INF || wb.cls == W_INF) {
        if (wa.cls == W_ZERO || wb.cls == W_ZERO) {
            c.exc |= EXC_OPERR;
            return defaultNan();
        }
        return makeInf(wa.s ^ wb.s);
    }
    return round(wmul(wa, wb), c, PREC_XS);
}

X div(const X& a, const X& b, Ctx& c, int prec) {
    W wa = toW(a), wb = toW(b);
    int s = wa.s ^ wb.s;
    if (wa.cls == W_INF) {
        if (wb.cls == W_INF) {
            c.exc |= EXC_OPERR;
            return defaultNan();
        }
        return makeInf(s);
    }
    if (wb.cls == W_INF) return makeZero(s);
    if (wb.cls == W_ZERO) {
        if (wa.cls == W_ZERO) {
            c.exc |= EXC_OPERR;
            return defaultNan();
        }
        c.exc |= EXC_DZ;
        return makeInf(s);
    }
    return round(wdiv(wa, wb), c, prec);
}

X sqrt(const X& a, Ctx& c, int prec) {
    W w = toW(a);
    if (w.cls == W_ZERO) return a;
    if (w.s) {
        c.exc |= EXC_OPERR;
        return defaultNan();
    }
    if (w.cls == W_INF) return a;
    uint64_t M = (uint64_t)(w.m >> 64);
    u128 N;
    int32_t E;
    if (w.e & 1) {
        N = (u128)M << 64;
        E = (w.e - 1) / 2;
    } else {
        N = (u128)M << 63;
        E = w.e / 2;
    }
    // 64 ビットの整数の平方根
    u128 rem = 0, root = 0;
    for (int i = 0; i < 64; i++) {
        rem = (rem << 2) | (N >> 126);
        N <<= 2;
        root <<= 1;
        u128 t = (root << 1) | 1;
        if (rem >= t) {
            rem -= t;
            root |= 1;
        }
    }
    bool roundBit = rem > root;
    W r{0, E, (root << 64) | ((u128)(roundBit ? 1 : 0) << 63), rem != 0, W_NORM};
    return round(r, c, prec);
}

X roundInt(const X& a, Ctx& c, int mode) {
    W w = toW(a);
    if (w.cls != W_NORM || w.e >= 63) return a;
    u128 ip, frac;
    if (w.e < 0) {
        ip = 0;
        frac = w.m; // 値は 1 より小さい
    } else {
        int sh = 127 - w.e;
        ip = w.m >> sh;
        frac = w.m << (128 - sh);
    }
    if (frac == 0 && w.e >= 0) return a;
    c.exc |= EXC_INEX2;
    bool half, above;
    if (w.e < 0) {
        half = w.e == -1;
        above = w.e == -1 && (w.m << 1) != 0;
        if (w.e < -1) half = above = false;
    } else {
        half = (frac >> 127) != 0;
        above = half && (frac << 1) != 0;
    }
    bool inc = false;
    switch (mode) {
    case RND_N: inc = above || (half && (ip & 1)); break;
    case RND_Z: break;
    case RND_M: inc = w.s; break;
    case RND_P: inc = !w.s; break;
    }
    if (inc) ip++;
    if (ip == 0) return makeZero(w.s);
    W r = wfromU128(ip, w.s);
    return round(r, c, PREC_X);
}

X remainder(const X& a, const X& b, Ctx& c, bool frem, int& quot, bool& quotNeg) {
    W wa = toW(a), wb = toW(b);
    quot = 0;
    quotNeg = (wa.s ^ wb.s) != 0;
    if (wa.cls == W_INF || wb.cls == W_ZERO) {
        c.exc |= EXC_OPERR;
        quotNeg = false; // fputest.x の期待値では商のバイトを 0 にする
        return defaultNan();
    }
    if (wa.cls == W_ZERO) return a;
    if (wb.cls == W_INF) return roundCtx(wa, c); // 非正規化数なら UNFL を立てる
    u128 A = wa.m >> 64, B = wb.m >> 64;
    int32_t d = wa.e - wb.e;
    u128 R;
    int32_t re;
    unsigned q = 0;
    bool qOdd = false;
    int rs = wa.s;
    if (d < 0) {
        R = A;
        re = wa.e;
        if (frem && d == -1 && A > B) {
            R = (B << 1) - A;
            rs ^= 1;
            q = 1;
        }
    } else {
        R = A;
        bool bit = R >= B;
        if (bit) R -= B;
        q = bit ? 1 : 0;
        for (int32_t i = 0; i < d; i++) {
            R <<= 1;
            bit = R >= B;
            if (bit) R -= B;
            q = ((q << 1) | (bit ? 1 : 0)) & 0xff;
        }
        qOdd = q & 1;
        re = wb.e;
        if (frem && (R * 2 > B || (R * 2 == B && qOdd))) {
            R = B - R;
            rs ^= 1;
            q++;
        }
    }
    quot = (int)(q & 0x7f);
    if (R == 0) return makeZero(wa.s);
    W r = wscale(wfromU128(R, rs), re - 63); // 値は R x 2^(re - 63)
    return roundCtx(r, c);
}

X scale(const X& a, int32_t n, Ctx& c) {
    W w = toW(a);
    if (w.cls != W_NORM) return a;
    if (n > 40000) n = 40000;
    if (n < -40000) n = -40000;
    w.e += n;
    return roundCtx(w, c);
}

// ---------------------------------------------------------------- 超越関数

namespace {

// 超越関数の結果を丸める (端数があるものとして扱う)
X finish(W r, Ctx& c) {
    r.sticky = true;
    c.tinyAfterRounding = true;
    X x = roundCtx(r, c);
    c.tinyAfterRounding = false;
    return x;
}

X operr(Ctx& c) {
    c.exc |= EXC_OPERR;
    return defaultNan();
}

// 正確な結果。fputest.x の期待値では、超越関数は正確でも INEX2 を立てる
X exactInex(const W& r, Ctx& c) {
    c.tinyAfterRounding = true;
    X x = roundCtx(r, c);
    c.tinyAfterRounding = false;
    c.exc |= EXC_INEX2;
    return x;
}

// log(1) などの 0。fputest.x の期待値では RM のとき -0
X logZero(const Ctx& c) { return makeZero(c.rnd == RND_M); }

// x が小さすぎるとき (|x| < 2^-70)、f(x) = x (+ 微小) とみなす関数のため
bool tiny(const W& w) { return w.cls == W_NORM && w.e < -70; }
// f(x) = x (1 + O(x^2)) の関数で、x^2 の項が拡張精度の丸めに方向しか影響しないほど |x| が小さいか (|x| < 2^-34 なら
// x^2 / 3 < 2^-69 で、64 ビットの仮数の 1 ulp の 1/32 より小さい)。W (128 ビット) で計算すると x^3 の項が仮数の下に
// はみ出して丸めの方向を誤ることがあるので、tinyResult() で方向だけを渡す
bool tinyOdd(const W& w) { return w.cls == W_NORM && w.e < -34; }

// |x| が小さいとき (tiny)、f(x) = x (1 + O(x^2)) の関数の結果。2 次の項は W の精度より小さいので、
// x より絶対値が大きいか小さいかだけを丸めに伝える (larger: 絶対値が x より少し大きい)
X tinyResult(W w, bool larger, Ctx& c) {
    if (!larger) {
        w.m -= 1; // x より 2^-127 ulp 小さく、その下に端数がある
        if (!(w.m >> 127)) {
            w.m = (w.m << 1) | 1;
            w.e--;
        }
    }
    return finish(w, c);
}

} // namespace

namespace {

// sin (cosine = false) か cos (cosine = true)
X sinOrCos(const X& a, Ctx& c, bool cosine) {
    W w = toW(a);
    if (w.cls == W_INF) return operr(c);
    if (w.cls == W_ZERO) return cosine ? X{0x3fff, 0x8000000000000000ull} : a;
    if (!cosine && tinyOdd(w)) return tinyResult(w, false, c); // sin x = x - x^3/6
    W r;
    int n;
    reduce(a, r, n);
    W ss, cc;
    wsincosSeries(r, ss, cc);
    if (cosine) n++; // cos(x) = sin(x + pi/2)
    switch (n & 3) {
    case 0: return finish(ss, c);
    case 1: return finish(cc, c);
    case 2: return finish(wneg(ss), c);
    default: return finish(wneg(cc), c);
    }
}

} // namespace

X fsin(const X& a, Ctx& c) { return sinOrCos(a, c, false); }
X fcos(const X& a, Ctx& c) { return sinOrCos(a, c, true); }

X ftan(const X& a, Ctx& c) {
    W w = toW(a);
    if (w.cls == W_INF) return operr(c);
    if (w.cls == W_ZERO) return a;
    W r;
    int n;
    reduce(a, r, n);
    W ss, cc;
    wsincosSeries(r, ss, cc);
    W t = (n & 1) ? wneg(wdiv(cc, ss)) : wdiv(ss, cc);
    return finish(t, c);
}

X fatan(const X& a, Ctx& c) {
    W w = toW(a);
    if (w.cls == W_ZERO) return a;
    if (w.cls == W_INF) return finish(w.s ? wneg(kPiHalf) : kPiHalf, c);
    return finish(watan(w), c);
}

X fasin(const X& a, Ctx& c) {
    W w = toW(a);
    if (w.cls == W_ZERO) return a;
    if (w.cls == W_INF) return operr(c);
    int cm = cmpAbs(w, wone());
    if (cm > 0) return operr(c);
    if (cm == 0) return finish(w.s ? wneg(kPiHalf) : kPiHalf, c);
    if (tinyOdd(w)) return tinyResult(w, true, c); // asin x = x + x^3/6
    W ax = w;
    ax.s = 0;
    W d = wsqrt(wmul(wsub(wone(), ax), wadd(wone(), ax)));
    W r = watan(wdiv(ax, d));
    r.s = w.s;
    return finish(r, c);
}

X facos(const X& a, Ctx& c) {
    W w = toW(a);
    if (w.cls == W_INF) return operr(c);
    if (w.cls == W_ZERO) return finish(kPiHalf, c);
    int cm = cmpAbs(w, wone());
    if (cm > 0) return operr(c);
    if (cm == 0) {
        if (!w.s) return makeZero(false);
        return finish(kPi, c);
    }
    W t = wsqrt(wdiv(wsub(wone(), w), wadd(wone(), w)));
    W r = wscale(watan(t), 1);
    return finish(r, c);
}

X fsinh(const X& a, Ctx& c) {
    W w = toW(a);
    if (w.cls != W_NORM) return a;
    if (tinyOdd(w)) return tinyResult(w, true, c); // sinh x = x + x^3/6
    W ax = w;
    ax.s = 0;
    if (ax.e >= 15) { // 大きすぎる
        W big{w.s, 100000, ONE128, true, W_NORM};
        return roundCtx(big, c);
    }
    W em1 = wexpm1(ax);
    W r = wscale(wadd(em1, wdiv(em1, wadd(em1, wone()))), -1);
    r.s = w.s;
    return finish(r, c);
}

X fcosh(const X& a, Ctx& c) {
    W w = toW(a);
    if (w.cls == W_ZERO) return X{0x3fff, 0x8000000000000000ull};
    if (w.cls == W_INF) return makeInf(false);
    w.s = 0;
    if (w.e >= 15) {
        W big{0, 100000, ONE128, true, W_NORM};
        return roundCtx(big, c);
    }
    W e = wexp(w);
    W r = wscale(wadd(e, wdiv(wone(), e)), -1);
    return finish(r, c);
}

// 1 より少しだけ小さい値 (1 - 2^-128 より大きく 1 より小さい)。tanh x や e^x - 1 が ±1 に近すぎて W の桁に収まらない
// とき (1 との差が 2^-128 より小さいとき) に使う。wone() に sticky を付けると「1 より少し大きい」の意味になり、方向を決めた丸めで逆に丸めてしまう
W wbelowOne(int s) { return W{s, -1, ~(u128)0, true, W_NORM}; }

X ftanh(const X& a, Ctx& c) {
    W w = toW(a);
    if (w.cls == W_ZERO) return a;
    if (w.cls == W_INF) return X{(uint16_t)(w.s ? 0xbfff : 0x3fff), 0x8000000000000000ull};
    if (tinyOdd(w)) return tinyResult(w, false, c); // tanh x = x - x^3/3
    W ax = w;
    ax.s = 0;
    W r;
    if (ax.e >= 6) {
        r = wbelowOne(0); // 1 - tanh x = 2 / (e^2x + 1) < 2^-180
    } else {
        W em1 = wexpm1(wscale(ax, 1));
        if (ax.e >= 3) {
            // |x| >= 8: e^2x - 1 に 2 を足すと W の桁からはみ出す (|x| >= 44.5 では仮数が変わらず sticky だけになり、
            // 商が 1 + sticky、つまり 1 より大きくなる)。1 - 2 / (e^2x + 1) として、wsub で 1 より小さい側に丸める
            r = wsub(wone(), wdiv(wfromInt(2), wadd(em1, wfromInt(2))));
        } else {
            r = wdiv(em1, wadd(em1, wfromInt(2)));
        }
    }
    r.s = w.s;
    return finish(r, c);
}

X fatanh(const X& a, Ctx& c) {
    W w = toW(a);
    if (w.cls == W_ZERO) return a;
    if (w.cls == W_INF) return operr(c);
    int cm = cmpAbs(w, wone());
    if (cm > 0) return operr(c);
    if (cm == 0) {
        c.exc |= EXC_DZ;
        return makeInf(w.s);
    }
    if (tinyOdd(w)) return tinyResult(w, true, c); // atanh x = x + x^3/3
    W ax = w;
    ax.s = 0;
    W r = wscale(wlog1p(wdiv(wscale(ax, 1), wsub(wone(), ax))), -1);
    r.s = w.s;
    return finish(r, c);
}

X fetox(const X& a, Ctx& c) {
    W w = toW(a);
    if (w.cls == W_ZERO) return X{0x3fff, 0x8000000000000000ull};
    if (w.cls == W_INF) return w.s ? makeZero(false) : a;
    if (tiny(w)) return finish(wadd(wone(), w), c);
    if (w.e >= 15) {
        W r = w.s ? W{0, -100000, ONE128, true, W_NORM} : W{0, 100000, ONE128, true, W_NORM};
        return roundCtx(r, c);
    }
    return finish(wexp(w), c);
}

X fetoxm1(const X& a, Ctx& c) {
    W w = toW(a);
    if (w.cls == W_ZERO) return a;
    if (w.cls == W_INF) return w.s ? X{0xbfff, 0x8000000000000000ull} : a;
    if (tiny(w)) return tinyResult(w, !w.s, c); // e^x - 1 = x + x^2/2
    if (w.e >= 15) {
        if (w.s) return finish(wbelowOne(1), c); // -1 + e^x (e^x < 2^-23000)
        W r{0, 100000, ONE128, true, W_NORM};
        return roundCtx(r, c);
    }
    return finish(wexpm1(w), c);
}

X ftwotox(const X& a, Ctx& c) {
    W w = toW(a);
    if (w.cls == W_ZERO) return X{0x3fff, 0x8000000000000000ull};
    if (w.cls == W_INF) return w.s ? makeZero(false) : a;
    if (w.e >= 16) {
        W r = w.s ? W{0, -100000, ONE128, true, W_NORM} : W{0, 100000, ONE128, true, W_NORM};
        return roundCtx(r, c);
    }
    // 整数なら正確に 2^n
    if (w.e >= 0) {
        int sh = 127 - w.e;
        if ((w.m << (128 - sh)) == 0) {
            int64_t n = (int64_t)(w.m >> sh);
            if (w.s) n = -n;
            W r = wone();
            r.e = (int32_t)n;
            return exactInex(r, c);
        }
    }
    if (tiny(w)) return finish(wadd(wone(), wmul(w, kLn2)), c);
    return finish(wexp(wmul(w, kLn2)), c);
}

X ftentox(const X& a, Ctx& c) {
    W w = toW(a);
    if (w.cls == W_ZERO) return X{0x3fff, 0x8000000000000000ull};
    if (w.cls == W_INF) return w.s ? makeZero(false) : a;
    if (w.e >= 15) {
        W r = w.s ? W{0, -100000, ONE128, true, W_NORM} : W{0, 100000, ONE128, true, W_NORM};
        return roundCtx(r, c);
    }
    if (w.e >= 0 && !w.s) {
        int sh = 127 - w.e;
        if ((w.m << (128 - sh)) == 0) {
            int64_t n = (int64_t)(w.m >> sh);
            if (n <= 27) return exactInex(pow10w((int)n), c); // 10^27 までは W で正確
        }
    }
    if (tiny(w)) return finish(wadd(wone(), wmul(w, kLn10)), c);
    return finish(wexp(wmul(w, kLn10)), c);
}

X flogn(const X& a, Ctx& c) {
    W w = toW(a);
    if (w.cls == W_ZERO) {
        c.exc |= EXC_DZ;
        return makeInf(true);
    }
    if (w.s) return operr(c);
    if (w.cls == W_INF) return a;
    if (w.e == 0 && w.m == ONE128) return logZero(c);
    return finish(wlog(w), c);
}

X flognp1(const X& a, Ctx& c) {
    W w = toW(a);
    if (w.cls == W_ZERO) return a;
    if (w.cls == W_INF) return w.s ? operr(c) : a;
    if (w.s) {
        int cm = cmpAbs(w, wone());
        if (cm > 0) return operr(c);
        if (cm == 0) {
            c.exc |= EXC_DZ;
            return makeInf(true);
        }
    }
    if (tiny(w)) return tinyResult(w, w.s, c); // log(1 + x) = x - x^2/2
    return finish(wlog1p(w), c);
}

X flog10(const X& a, Ctx& c) {
    W w = toW(a);
    if (w.cls == W_ZERO) {
        c.exc |= EXC_DZ;
        return makeInf(true);
    }
    if (w.s) return operr(c);
    if (w.cls == W_INF) return a;
    // 10 の整数乗なら正確に
    if (w.e >= 0 && w.e < 90) {
        for (int n = 0; n <= 27; n++) {
            W p = pow10w(n);
            if (p.e == w.e && p.m == w.m) {
                if (n == 0) return logZero(c);
                return exactInex(wfromInt(n), c);
            }
        }
    }
    return finish(wmul(wlog(w), kLog10e), c);
}

X flog2(const X& a, Ctx& c) {
    W w = toW(a);
    if (w.cls == W_ZERO) {
        c.exc |= EXC_DZ;
        return makeInf(true);
    }
    if (w.s) return operr(c);
    if (w.cls == W_INF) return a;
    if (w.m == ONE128) { // 2 の整数乗
        if (w.e == 0) return logZero(c);
        return exactInex(wfromInt(w.e), c);
    }
    return finish(wmul(wlog(w), kLog2e), c);
}

namespace {

// FMOVECR の ROM の 1 項目。m は拡張精度の仮数 (64 ビット) で、正規化していないものもある。
// half / below は m より下のビット (拡張精度に丸めるときの丸めのビットと、その下が 0 でないか)
struct RomConst {
    uint16_t se;
    uint64_t m;
    bool half, below;
};

// W の定数を 64 ビットの仮数と、その下のビットに分ける
RomConst romFromW(const W& w) {
    uint64_t lo = (uint64_t)w.m;
    return RomConst{(uint16_t)(w.e + 16383), (uint64_t)(w.m >> 64), (lo >> 63) != 0, (lo << 1) != 0 || w.sticky};
}

RomConst romEntry(int off) {
    // $01-$0A と $10-$2F はマニュアルにない。FMOVE.X で書き出した値と条件コードは、fputest.x の FMOVECR の期待値に
    // 合わせた。XEiJ のマニュアルによると、この期待値は実機の 68882 の値 (fputest.x は FMOVECR のあと FMOVE.X FPn,-(A7)
    // で書き出して比べる)。
    // $08 は bit62 が 0 のシグナリング NaN にしてある。fputest.x は FMOVE.X で書き出した値 (bit62 が立っている) を
    // 見るので、レジスタの中の bit62 は見分けられず、実機では確かめていない。このため $08 のあとの FMOVE.X や演算は
    // SNAN と AIOP を立て、SNAN の例外を許可していればベクタ 54 になる。
    // FMOVEM で書き出したときの値 (レジスタの中の形) も fputest.x では決まらず、実機では確かめていない:
    // - $09、$0A、$10-$2F は、正規化した形で持つ (FMOVE.X で書き出した値は fputest.x の期待値と合う)
    // - $03 は正規化しない形で持ち、そのまま FMOVE.X で書き出す (fputest.x の期待値)
    switch (off) {
    case 0x00: return romFromW(kPi);
    case 0x01: return RomConst{0x4001, 0xfe00068200000000ull, false, false};
    case 0x02: return RomConst{0x4001, 0xffc0050380000000ull, false, false};
    case 0x03: return RomConst{0x2000, 0x7fffffff00000000ull, false, false}; // 正規化していない
    case 0x04: return RomConst{0x0000, 0xffffffffffffffffull, false, false};
    case 0x05: return RomConst{0x3c00, 0xfffffffffffff800ull, false, false};
    case 0x06: return RomConst{0x3f80, 0xffffff0000000000ull, false, false};
    case 0x07: return RomConst{0x0001, 0xf65d8d9c00000000ull, false, false};
    case 0x08: return RomConst{0x7fff, 0x001e000000000000ull, false, false}; // シグナリング NaN (bit62 が 0)
    case 0x09: return RomConst{0x43f3, 0xe000000000000000ull, false, false}; // 正規化した形
    case 0x0a: return RomConst{0x4072, 0xc000000000000000ull, false, false}; // 正規化した形
    case 0x0b: { // log10(2)。実機は本当の値 (下のビットは半分より上) と違い、半分より下として丸める
        RomConst r = romFromW(kLog10_2);
        r.half = false;
        return r;
    }
    case 0x0c: { // e。log10(2) と同じく、半分より下として丸める
        RomConst r = romFromW(kE);
        r.half = false;
        return r;
    }
    case 0x0d: return romFromW(kLog2e);
    case 0x0e: { // log10(e)。実機は拡張精度では正確な値として扱う (INEX2 を立てない)
        RomConst r = romFromW(kLog10e);
        r.half = r.below = false;
        return r;
    }
    case 0x30: return romFromW(kLn2);
    case 0x31: return romFromW(kLn10);
    default:
        if (off >= 0x32 && off <= 0x3f) { // 10^0、10^1、10^2、10^4、...、10^4096
            W w = off == 0x32 ? wone() : pow10w(1 << (off - 0x33));
            return romFromW(w); // 10^27 までは W で正確 (下のビットは 0)
        }
        return RomConst{0, 0, false, false}; // $0F と $10-$2F は 0
    }
}

} // namespace

// FMOVECR の定数 (MC68881UM の FMOVECR)。FPCR の精度と方向で丸める。
// 単精度・倍精度でも指数は拡張精度のまま、仮数だけを丸める。
// 実機の丸めは変わっていて、正規化していない値も (正規化せずに) 仮数の bit63 から数えて丸める。
// $01、$02、$07 は単精度で 25 ビットに丸める。以上は fputest.x の実機の結果に合わせたもの。
// 精度と丸めの方向のすべての組の値は、tests/unit/fpu882_test.cpp の testFmovecrUndefined() で固定している
// (fputest.x が見ていない組は、実機では確かめていない)
X constant(int off, Ctx& c) {
    RomConst r = romEntry(off);
    X x{r.se, r.m};
    if ((r.se & 0x7fff) == 0x7fff || (r.m == 0 && !r.half && !r.below)) return x; // NaN と 0 はそのまま
    int bits = 64;
    if (c.prec == PREC_S) bits = (off == 0x01 || off == 0x02 || off == 0x07) ? 25 : 24;
    else if (c.prec == PREC_D) bits = 53;
    u128 kept = r.m; // 繰り上がりを見るため 128 ビット
    bool roundBit = r.half, st = r.below;
    if (bits < 64) {
        int drop = 64 - bits;
        kept = r.m >> drop;
        roundBit = (r.m >> (drop - 1)) & 1;
        st = r.half || r.below || (r.m & ((1ull << (drop - 1)) - 1)) != 0;
    }
    bool inexact = roundBit || st;
    bool neg = (r.se & 0x8000) != 0, inc = false;
    switch (c.rnd) {
    case RND_N: inc = roundBit && (st || (kept & 1)); break;
    case RND_Z: break;
    case RND_M: inc = inexact && neg; break;
    case RND_P: inc = inexact && !neg; break;
    }
    if (inexact) c.exc |= EXC_INEX2;
    if (inc && ++kept >> bits) { // 繰り上がり
        kept >>= 1;
        x.se++;
    }
    x.m = (uint64_t)(kept << (64 - bits));
    return x;
}

// ---------------------------------------------------------------- 形式の変換

X fromInt(int64_t v) {
    if (v == 0) return makeZero(false);
    Ctx c;
    return round(wfromInt(v), c, PREC_X);
}

X fromSingle(uint32_t v, Ctx& c) {
    bool s = (v >> 31) != 0;
    int ex = (v >> 23) & 0xff;
    uint32_t f = v & 0x7fffff;
    if (ex == 0xff) {
        if (!f) return makeInf(s);
        X x{(uint16_t)(s ? 0xffff : 0x7fff), 0x8000000000000000ull | ((uint64_t)f << 40)};
        if (!(f & 0x400000)) {
            c.exc |= EXC_SNAN;
            x.m |= 0x4000000000000000ull;
        }
        return x;
    }
    if (ex == 0) {
        if (!f) return makeZero(s);
        W w = wfromU128(f, s);
        w.e -= 149;
        return round(w, c, PREC_X);
    }
    return X{(uint16_t)((s ? 0x8000 : 0) | (ex - 127 + 16383)), 0x8000000000000000ull | ((uint64_t)f << 40)};
}

X fromDouble(uint64_t v, Ctx& c) {
    bool s = (v >> 63) != 0;
    int ex = (int)((v >> 52) & 0x7ff);
    uint64_t f = v & 0xfffffffffffffull;
    if (ex == 0x7ff) {
        if (!f) return makeInf(s);
        X x{(uint16_t)(s ? 0xffff : 0x7fff), 0x8000000000000000ull | (f << 11)};
        if (!(f & 0x8000000000000ull)) {
            c.exc |= EXC_SNAN;
            x.m |= 0x4000000000000000ull;
        }
        return x;
    }
    if (ex == 0) {
        if (!f) return makeZero(s);
        W w = wfromU128(f, s);
        w.e -= 1074;
        return round(w, c, PREC_X);
    }
    return X{(uint16_t)((s ? 0x8000 : 0) | (ex - 1023 + 16383)), 0x8000000000000000ull | (f << 11)};
}

uint32_t toSingle(const X& a, Ctx& c) {
    uint32_t s = sign(a) ? 0x80000000u : 0;
    if (isNan(a)) {
        if (isSnan(a)) c.exc |= EXC_SNAN;
        return s | 0x7fc00000u | (uint32_t)((a.m >> 40) & 0x3fffff);
    }
    if (isInf(a)) return s | 0x7f800000u;
    W w = toW(a);
    if (w.cls == W_ZERO) return s;
    Rounded r;
    roundCore(w, c, 24, 24, 127, 1, 254, r);
    if (r.zero) return s;
    if (r.inf) return s | 0x7f800000u;
    return s | ((uint32_t)r.be << 23) | (uint32_t)(r.mant & 0x7fffff);
}

uint64_t toDouble(const X& a, Ctx& c) {
    uint64_t s = sign(a) ? 0x8000000000000000ull : 0;
    if (isNan(a)) {
        if (isSnan(a)) c.exc |= EXC_SNAN;
        return s | 0x7ff8000000000000ull | ((a.m >> 11) & 0x7ffffffffffffull);
    }
    if (isInf(a)) return s | 0x7ff0000000000000ull;
    W w = toW(a);
    if (w.cls == W_ZERO) return s;
    Rounded r;
    roundCore(w, c, 53, 53, 1023, 1, 2046, r);
    if (r.zero) return s;
    if (r.inf) return s | 0x7ff0000000000000ull;
    return s | ((uint64_t)r.be << 52) | (r.mant & 0xfffffffffffffull);
}

int32_t toInt(const X& a, Ctx& c, int bits) {
    int32_t maxv = (int32_t)((1u << (bits - 1)) - 1), minv = -maxv - 1;
    if (isNan(a)) {
        // NaN は OPERR (MC68881UM の表 6-2) で、-1 (すべてのビットが 1) を返す。6.1.3 は「NaN の仮数の上位 8 / 16 / 32
        // ビットを書く」としていて、既定の NaN (仮数がすべて 1) では -1 になるが、ほかの NaN では食い違う
        c.exc |= EXC_OPERR;
        if (isSnan(a)) c.exc |= EXC_SNAN;
        return -1;
    }
    if (isInf(a)) {
        c.exc |= EXC_OPERR;
        return sign(a) ? minv : maxv;
    }
    X r = roundInt(a, c, c.rnd);
    W w = toW(r);
    if (w.cls == W_ZERO) return 0;
    if (w.e >= bits) {
        // 整数の範囲を超えたら OPERR で、入る最大 / 最小の整数を返す (MC68881UM 6.1.3)。整数への丸めで立った INEX2 は
        // 残す (6.1.9: OPERR と INEX2 は同時に起きうる)
        c.exc |= EXC_OPERR;
        return w.s ? minv : maxv;
    }
    int64_t v = (int64_t)(uint64_t)(w.m >> (127 - w.e));
    if (w.s) v = -v;
    if (v > maxv || v < minv) {
        c.exc |= EXC_OPERR; // 上と同じ (INEX2 は残す)
        return w.s ? minv : maxv;
    }
    return (int32_t)v;
}

namespace {

// 10^n (n >= 0)。n <= 27 なら正確
W pow10any(int n) { return pow10w(n); }

// |w| を整数に丸める (|w| < 2^100)。mode は |w| に対する丸めの方向 (RND_M は切り捨て、RND_P は切り上げ)
u128 wRoundIntAbs(const W& w, int mode, bool& inexact) {
    inexact = false;
    if (w.cls == W_ZERO) return 0;
    u128 ip;
    bool half, rest;
    if (w.e < 0) {
        ip = 0;
        half = w.e == -1;
        rest = w.e < -1 || (w.m << 1) != 0 || w.sticky;
    } else {
        int sh = 127 - w.e;
        ip = sh > 0 ? w.m >> sh : w.m;
        u128 frac = sh > 0 ? w.m << (128 - sh) : 0;
        half = (frac >> 127) != 0;
        rest = (frac << 1) != 0 || w.sticky;
    }
    if (!half && !rest) return ip;
    inexact = true;
    bool up = false;
    switch (mode) {
    case RND_N: up = half && (rest || (ip & 1)); break;
    case RND_Z:
    case RND_M: break;
    case RND_P: up = true; break;
    }
    return ip + (up ? 1 : 0);
}

int bcd(uint32_t v, int sh) { return (int)((v >> sh) & 15); }

} // namespace

X fromPacked(const uint32_t p[3], Ctx& c) {
    // 無限大と NaN。MC68881UM の表 3-4 では、SE と YY (bit29-28) が 1 で指数が $FFF のものが無限大 (小数部が 0) か NaN。
    // ここでは、YY のどちらかが 1 か、0〜9 でない数字があれば無限大か NaN とし、NaN は既定の NaN にする。
    // 表 3-4 とその注 (範囲内の数の YY は見ない、0〜9 でない数字は検出せずそのまま変換する、NaN の小数部はそのまま
    // 仮数に写す) とは、この 3 点で食い違う。YY と数字の 2 点は、表のとおりにしても fputest.x の fmove.p / fadd.p は通る
    bool sm = (p[0] >> 31) & 1, se = (p[0] >> 30) & 1;
    int digits[17];
    bool bad = false;
    digits[0] = bcd(p[0], 0);
    for (int i = 0; i < 8; i++) digits[1 + i] = bcd(p[1], 28 - 4 * i);
    for (int i = 0; i < 8; i++) digits[9 + i] = bcd(p[2], 28 - 4 * i);
    int ed[3] = {bcd(p[0], 24), bcd(p[0], 20), bcd(p[0], 16)};
    for (int d : digits)
        if (d > 9) bad = true;
    for (int d : ed)
        if (d > 9) bad = true;
    if ((p[0] & 0x30000000u) || bad) {
        uint64_t v = ((uint64_t)p[1] << 32) | p[2];
        if (v == 0) return makeInf(sm);
        if (!(v & 0x4000000000000000ull)) c.exc |= EXC_SNAN;
        return defaultNan();
    }
    uint64_t n = 0;
    for (int d : digits) n = n * 10 + (uint64_t)d;
    if (n == 0) return makeZero(sm);
    int exp10 = ed[0] * 100 + ed[1] * 10 + ed[2];
    if (se) exp10 = -exp10;
    int k = exp10 - 16;
    W w = wfromU128(n, sm);
    if (k > 0) w = wmul(w, pow10any(k));
    else if (k < 0) w = wdiv(w, pow10any(-k));
    // 拡張精度に丸める。そこで端数が出たら INEX1 (MC68881UM 6.1.8: FPCR の丸めの精度に関係なく、拡張精度に丸める。
    // FPCR の精度での丸めは呼ぶ側で行う)
    Ctx cx;
    cx.rnd = c.rnd;
    X r = round(w, cx, PREC_X);
    if (cx.exc & EXC_INEX2) c.exc |= EXC_INEX1;
    return r;
}

void toPacked(const X& a, int k, Ctx& c, uint32_t p[3]) {
    p[0] = p[1] = p[2] = 0;
    k = (int)((int8_t)(k << 1) >> 1); // 下位 7 ビットを符号拡張する
    bool neg = sign(a);
    if (isNan(a)) {
        // 符号と仮数はそのまま書く。シグナリング NaN は SNAN を立て、SNAN のビット (bit62) を立てて書く (MC68881UM 6.1.2)
        uint64_t m = a.m;
        if (isSnan(a)) {
            c.exc |= EXC_SNAN;
            m |= 0x4000000000000000ull;
        }
        p[0] = (neg ? 0x80000000u : 0) | 0x7fff0000u;
        p[1] = (uint32_t)(m >> 32);
        p[2] = (uint32_t)m;
        return;
    }
    if (isInf(a)) {
        p[0] = (neg ? 0x80000000u : 0) | 0x7fff0000u;
        return;
    }
    W w = toW(a);
    if (w.cls == W_ZERO) {
        p[0] = neg ? 0x80000000u : 0;
        return;
    }
    w.s = 0;
    if (k > 17) {
        c.exc |= EXC_OPERR;
        k = 17;
    }
    // 絶対値に対する丸めの方向
    int mode = c.rnd;
    if (neg && mode == RND_M) mode = RND_P;
    else if (neg && mode == RND_P) mode = RND_M;
    else if (!neg && mode == RND_M) mode = RND_Z;
    // ilog = floor(log10 |x|)
    int ilog = (int)(((int64_t)w.e * 78913) >> 18); // log10(2) ~ 78913 / 2^18
    auto pow10s = [](int n) { return n >= 0 ? pow10any(n) : wdiv(wone(), pow10any(-n)); };
    for (;;) {
        if (cmpAbs(w, pow10s(ilog)) < 0) {
            ilog--;
            continue;
        }
        if (cmpAbs(w, pow10s(ilog + 1)) >= 0) {
            ilog++;
            continue;
        }
        break;
    }
    int len = 0;
    u128 y = 0;
    bool inexact = false;
    for (int pass = 0; pass < 2; pass++) {
        len = k > 0 ? k : ilog + 1 - k;
        if (len > 17) len = 17;
        // k が 0 以下で、値が小数点以下 -k 桁より小さいときも、0 にせず 1 桁で出す (fputest.x の期待値。
        // FMOVE.P で -0.5833 を k=0、RN で書くと -6E-1)
        if (len < 1) len = 1;
        int sc = ilog + 1 - len; // |x| / 10^sc を len 桁の整数にする
        W yw = sc > 0 ? wdiv(w, pow10any(sc)) : sc < 0 ? wmul(w, pow10any(-sc)) : w;
        y = wRoundIntAbs(yw, mode, inexact);
        uint64_t lim = 1;
        for (int i = 0; i < len; i++) lim *= 10;
        if (y >= lim) { // 丸めで桁が増えた (99.9 → 100)
            if (pass == 0 && k <= 0) {
                ilog++;
                continue;
            }
            y /= 10; // 末尾は 0
            ilog++;
        }
        break;
    }
    if (inexact) c.exc |= EXC_INEX2;
    int digits[17] = {0};
    uint64_t yy = (uint64_t)y;
    for (int i = len - 1; i >= 0; i--) {
        digits[i] = (int)(yy % 10);
        yy /= 10;
    }
    uint32_t aexp = (uint32_t)(ilog < 0 ? -ilog : ilog);
    // 指数が 3 桁に収まらないときは OPERR で、千の位 (EXP3) を bit15-12 に入れる (MC68881UM 6.1.3 と図 3-11)
    if (aexp >= 1000) c.exc |= EXC_OPERR;
    p[0] = (neg ? 0x80000000u : 0) | (ilog < 0 ? 0x40000000u : 0) | ((aexp / 100 % 10) << 24) | ((aexp / 10 % 10) << 20) |
           ((aexp % 10) << 16) | ((aexp / 1000 % 10) << 12) | (uint32_t)digits[0];
    for (int i = 0; i < 8; i++) p[1] |= (uint32_t)digits[1 + i] << (28 - 4 * i);
    for (int i = 0; i < 8; i++) p[2] |= (uint32_t)digits[9 + i] << (28 - 4 * i);
}

} // namespace fp
} // namespace m68k
