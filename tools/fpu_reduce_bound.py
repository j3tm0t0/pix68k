#!/usr/bin/env python3
# src/m68k/fpu_math.cpp の reduce() の注釈の根拠: 64 ビットの仮数の拡張精度の x で、x * 2/pi と最も近い整数との差が
# どこまで小さくなるかの下限を求める。
#
# x = M * 2^s (2^63 <= M < 2^64、s = 指数 - 63)。指数 e ごとに a = frac(2^s * 2/pi) の連分数を作り、分母が 2^64 以下で
# 最後の近似分数の分母 q_k について ||q_k * a|| を求める。q_k <= q < q_(k+1) の範囲に限らず、1 <= q < q_(k+1) の
# すべての整数 q で ||q * a|| >= ||q_k * a|| (最良近似の性質) なので、これがその指数のすべての M での下限になる。
# reduce() が引数を縮めるのは |x| > pi/4 (指数 -1 以上) のときなので、指数 -1 から 16383 までを調べる。
# 使い方: python3 tools/fpu_reduce_bound.py
import math


def atan_inv(n, f):
    # atan(1/n) * 2^f
    x = (1 << f) // n
    n2 = n * n
    s = x
    k = 1
    sign = -1
    while x:
        x //= n2
        s += sign * (x // (2 * k + 1))
        sign = -sign
        k += 1
    return s


G = 17200  # 2/pi のビット数 (指数 16383 の 2^s * 2/pi の小数部を 600 ビット取れる)
PI = (16 * atan_inv(5, G + 64) - 4 * atan_inv(239, G + 64)) >> 64  # pi * 2^G
TWO_OVER_PI = (1 << (2 * G + 1)) // PI  # 2/pi * 2^G
P = 600  # a の小数部のビット数 (q * 2^-P の誤差は 2^-536 以下で、求める差より十分小さい)


def frac_alpha(s):
    sh = G - s - P
    v = TWO_OVER_PI >> sh if sh >= 0 else TWO_OVER_PI << -sh
    return v & ((1 << P) - 1)


def lower_bound(num, limit=1 << 64):
    a, b = num, 1 << P
    q0, q1 = 1, 0
    while b:
        c = a // b
        q0, q1 = q1, c * q1 + q0
        a, b = b, a - c * b
        if q1 > limit:
            break
    t = (q0 * num) % (1 << P)
    return min(t, (1 << P) - t), q0


worst = None
for e in range(-1, 16384):
    d, q = lower_bound(frac_alpha(e - 63))
    log2d = math.log2(d) - P
    if worst is None or log2d < worst[0]:
        worst = (log2d, e, q)
print("min |x * 2/pi - n| >= 2^%.2f (exponent %d, q = %d)" % worst)
