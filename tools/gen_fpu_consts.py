#!/usr/bin/env python3
# FPU (src/m68k/fpu_math.cpp) が使う定数を、128 ビットの仮数で作る。整数演算だけを使う。
#   python3 -I tools/gen_fpu_consts.py > src/m68k/fpu_consts.inc
P = 17200  # 作業のビット数 (2/pi のビットを 16640 個取るため)
ONE = 1 << P


def arctan_inv(n):
    # atan(1/n) * ONE
    s = 0
    t = ONE // n
    k = 1
    n2 = n * n
    sign = 1
    while t:
        s += sign * (t // k)
        t //= n2
        k += 2
        sign = -sign
    return s


def atanh2(num, den):
    # 2 atanh(num/den) * ONE = log((den+num)/(den-num)) * ONE
    s = 0
    t = ONE * num // den
    k = 1
    z2n = num * num
    z2d = den * den
    while t:
        s += t // k
        t = t * z2n // z2d
        k += 2
    return 2 * s


pi = 4 * (4 * arctan_inv(5) - arctan_inv(239))
ln2 = atanh2(1, 3)
ln10 = 3 * ln2 + atanh2(1, 9)  # log(10) = 3 log(2) + log(5/4)
e = 0
t = ONE
k = 0
while t:
    e += t
    k += 1
    t //= k


def w(name, v):
    bl = v.bit_length()
    exp = bl - 1 - P
    m = v >> (bl - 128)
    print('const W %s = {0, %d, ((u128)0x%016xull << 64) | 0x%016xull, false, W_NORM};'
          % (name, exp, m >> 64, m & ((1 << 64) - 1)))


print('// tools/gen_fpu_consts.py で作った。手で直さないこと')
w('kPi', pi)
w('kPiHalf', pi // 2)
w('kLn2', ln2)
w('kLn10', ln10)
w('kE', e)
w('kLog2e', ONE * ONE // ln2)
w('kLog10_2', ln2 * ONE // ln10)
w('kLog10e', ONE * ONE // ln10)
# 2/pi の 2 進の小数部 (先頭から 16640 ビット)。三角関数の引数を pi/2 で割った余りにするのに使う
nw = 260
twoopi = 2 * ONE * ONE // pi
bits = twoopi >> (P - 64 * nw)
words = [(bits >> (64 * (nw - 1 - i))) & ((1 << 64) - 1) for i in range(nw)]
print('const uint64_t kTwoOverPi[%d] = {' % nw)
for i in range(0, nw, 4):
    print('    ' + ', '.join('0x%016xull' % x for x in words[i:i + 4]) + ',')
print('};')
