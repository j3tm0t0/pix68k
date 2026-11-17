// FPU の浮動小数点演算 (ソフトウェア)。
//
// ホストの long double は Mac (arm64) では倍精度しかなく、ベアメタルには libm がないので使わない。
// 拡張精度 (符号 1 + 指数 15 + 仮数 64 ビット) の値を、128 ビットの仮数を持つ内部形式 (W) に広げて計算する。
//
// 精度:
// - 加減乗除、平方根、整数への丸め、形式の変換は、FPCR の丸めの精度と方向で正しく丸める (誤差 0.5 ulp 以下)
// - 超越関数 (sin、log、exp など) は 128 ビットの仮数で計算し (相対誤差 2^-110 程度)、最後に 1 回だけ丸める。
//   丸めの境目にごく近い値を除けば正しく丸めた値になる (実機の 68881/68882 は 1 ulp 程度の誤差を持つ)
#pragma once

#include <stdint.h>

namespace m68k {
namespace fp {

typedef unsigned __int128 u128;

// 拡張精度の値 (メモリの 96 ビット形式から、使わない 16 ビットを除いたもの)
struct X {
    uint16_t se; // 符号 (bit15) と指数 (バイアス 16383)
    uint64_t m;  // 仮数 (bit63 が整数部)
};

enum { W_NORM = 0, W_ZERO = 1, W_INF = 2, W_NAN = 3 };

// 内部形式: 値 = (-1)^s × m × 2^(e - 127)。W_NORM なら m の bit127 が 1
struct W {
    int s;
    int32_t e;
    u128 m;
    bool sticky; // m より下に 0 でないビットがあった (丸めに使う)
    int cls;
};

// FPSR の例外ステータスのビット
enum : uint32_t {
    EXC_BSUN = 1u << 15,
    EXC_SNAN = 1u << 14,
    EXC_OPERR = 1u << 13,
    EXC_OVFL = 1u << 12,
    EXC_UNFL = 1u << 11,
    EXC_DZ = 1u << 10,
    EXC_INEX2 = 1u << 9,
    EXC_INEX1 = 1u << 8,
};

// PREC_XS と PREC_XD は仮数だけを 24 ビット・53 ビットに丸める (指数は拡張精度の範囲。FSGLDIV、FSGLMUL)
enum { PREC_X = 0, PREC_S = 1, PREC_D = 2, PREC_XS = 3, PREC_XD = 4 };
enum { RND_N = 0, RND_Z = 1, RND_M = 2, RND_P = 3 };

// 丸めの設定と、演算で起きた例外
struct Ctx {
    int prec = PREC_X;
    int rnd = RND_N;
    uint32_t exc = 0;
    // UNFL を丸めた後に判定する (結果が非正規化数のときだけ立てる)。fputest.x の期待値では超越関数がこうなる
    bool tinyAfterRounding = false;
};

inline bool isNan(const X& x) { return (x.se & 0x7fff) == 0x7fff && (x.m << 1) != 0; }
inline bool isSnan(const X& x) { return isNan(x) && !(x.m & 0x4000000000000000ull); }
inline bool isInf(const X& x) { return (x.se & 0x7fff) == 0x7fff && (x.m << 1) == 0; }
inline bool isZero(const X& x) { return (x.se & 0x7fff) != 0x7fff && x.m == 0; }
inline bool sign(const X& x) { return (x.se & 0x8000) != 0; }
X defaultNan();
X makeZero(bool neg);
X makeInf(bool neg);

W toW(const X& x);
// 丸めて拡張精度にする。prec は PREC_X など。PREC_S と PREC_D は指数も単精度・倍精度の範囲にする
// (MC68881UM 6.1.4 と 6.1.5 の NOTE)
X round(const W& w, Ctx& c, int prec);
X roundCtx(const W& w, Ctx& c); // c.prec の精度で丸める

// 基本演算 (正しく丸める)。NaN は呼ぶ側で処理する
X add(const X& a, const X& b, Ctx& c, int prec);
X sub(const X& a, const X& b, Ctx& c, int prec);
X mul(const X& a, const X& b, Ctx& c, int prec);
// FSGLMUL: 両方の仮数を 24 ビットに切り捨ててから掛け、仮数を 24 ビットに丸める
X sglmul(const X& a, const X& b, Ctx& c);
X div(const X& a, const X& b, Ctx& c, int prec);
X sqrt(const X& a, Ctx& c, int prec);
// 整数への丸め。mode は丸めの方向
X roundInt(const X& a, Ctx& c, int mode);
// 剰余。frem なら最も近い整数の商、fmod なら 0 方向の商。quot に商の下位 7 ビットと符号
X remainder(const X& a, const X& b, Ctx& c, bool frem, int& quot, bool& quotNeg);
X scale(const X& a, int32_t n, Ctx& c);

// 超越関数
X fsin(const X& a, Ctx& c);
X fcos(const X& a, Ctx& c);
X ftan(const X& a, Ctx& c);
X fatan(const X& a, Ctx& c);
X fasin(const X& a, Ctx& c);
X facos(const X& a, Ctx& c);
X fsinh(const X& a, Ctx& c);
X fcosh(const X& a, Ctx& c);
X ftanh(const X& a, Ctx& c);
X fatanh(const X& a, Ctx& c);
X fetox(const X& a, Ctx& c);
X fetoxm1(const X& a, Ctx& c);
X ftwotox(const X& a, Ctx& c);
X ftentox(const X& a, Ctx& c);
X flogn(const X& a, Ctx& c);
X flognp1(const X& a, Ctx& c);
X flog10(const X& a, Ctx& c);
X flog2(const X& a, Ctx& c);
// FMOVECR の定数 (ROM のオフセット)
X constant(int offset, Ctx& c);

// 形式の変換
X fromInt(int64_t v);
X fromSingle(uint32_t v, Ctx& c);
X fromDouble(uint64_t v, Ctx& c);
uint32_t toSingle(const X& a, Ctx& c);
uint64_t toDouble(const X& a, Ctx& c);
// 整数に丸める (bits = 8 / 16 / 32)。範囲外は OPERR で、最大値か最小値になる
int32_t toInt(const X& a, Ctx& c, int bits);
// パック 10 進 (96 ビット = 3 語)
X fromPacked(const uint32_t p[3], Ctx& c);
void toPacked(const X& a, int k, Ctx& c, uint32_t p[3]);

} // namespace fp
} // namespace m68k
