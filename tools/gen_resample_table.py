#!/usr/bin/env python3
"""音の周波数を直すフィルタの表を作る。
  resample: src/x68k/resample_table.inc (62.5kHz から出力の周波数へ。x68k/sound.cpp)
  pcm:      src/x68k/pcm_interp_table.inc (まーきゅりーゆにっとの PCM から 62.5kHz へ。x68k/mercury.cpp)

窓 (カイザー) をかけた sinc の多相フィルタ。kResample[p][j] は、出力の位置が標本 i + p / PHASES のときに、
標本 i - (TAPS / 2 - 1) + j に掛ける値 (2^15 倍)。p は 0 から PHASES (PHASES は次の標本の位置で、丸めのために持つ)。
どの p でも合計がちょうど同じ値 (resample は 2^15、pcm は 2^14) になるようにする (合計が p ごとに違うと、一定の入力が p の並びの周期で揺れる)。

resample の通す範囲は 18kHz まで、止める範囲は 26kHz から (62.5kHz の標本で。真ん中は 22kHz)。出力が 47.4kHz 以上のときに使う
(x68k/sound.cpp の Sound::resampleCenterHz())。48kHz に直すときは 24kHz から上が折り返すが、26kHz から上は 22kHz から
下に折り返すので止め、24kHz から 26kHz は 22kHz から 24kHz に折り返すだけなので、減衰の途中でよい
(22kHz で約 -6dB、24kHz で約 -19dB、26kHz で約 -57dB)。
出力がそれより低いとき (44.1kHz、32kHz など) は、sound.cpp が同じ式で、真ん中の周波数を下げた表を実行時に作る。

三角関数とベッセル関数は、標準の math だけを使う (表はコミットするので、ビルドする機械で値が変わることはない)。
  使い方: tools/gen_resample_table.py resample > src/x68k/resample_table.inc
          tools/gen_resample_table.py pcm > src/x68k/pcm_interp_table.inc
"""
import math
import sys

TAPS = 32
PHASES = 512
ATTEN_DB = 70.0
# 表ごとの、通す範囲の終わりと止める範囲の始まり (標本の周波数に対する比) と、1 行の合計 (係数の倍率)。
# pcm は、位置 0 の行が「真ん中の 1 つだけが 1」になるので、16 ビットに入るように倍率を 2^14 にする
TABLES = {
    # 62.5kHz から出力の周波数へ (18kHz まで通し、26kHz から止める)
    "resample": (18000.0 / 62500.0, 26000.0 / 62500.0, 32768),
    # まーきゅりーゆにっとの PCM (48kHz、44.1kHz、32kHz とその半分) から 62.5kHz へ。PCM の周波数の半分を境に、
    # 0.43 倍まで通し、0.57 倍から止める (48kHz なら 20.6kHz まで通し、27.4kHz から上にできる写しを止める)
    "pcm": (0.43, 0.57, 16384),
}


def bessel_i0(x):
    s = t = 1.0
    k = 1
    while t > 1e-17 * s:
        t *= (x / (2 * k)) ** 2
        s += t
        k += 1
    return s


def row(p, fc, unit):
    beta = 0.1102 * (ATTEN_DB - 8.7)
    half = TAPS // 2
    h = []
    for j in range(TAPS):
        d = (j - (half - 1)) - p / PHASES
        x = 2 * fc * d
        sinc = 1.0 if d == 0 else math.sin(math.pi * x) / (math.pi * x)
        r = d / half
        w = bessel_i0(beta * math.sqrt(1 - r * r)) / bessel_i0(beta) if abs(r) < 1 else 0.0
        h.append(2 * fc * sinc * w)
    total = sum(h)
    q = [int(round(v / total * unit)) for v in h]
    # 丸めで合計がずれた分は、いちばん大きい係数に足す
    q[max(range(TAPS), key=lambda i: abs(q[i]))] += unit - sum(q)
    assert sum(q) == unit and all(-32768 <= v <= 32767 for v in q)
    return q


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in TABLES:
        sys.exit("usage: %s %s" % (sys.argv[0], "|".join(TABLES)))
    lo, hi, unit = TABLES[sys.argv[1]]
    out = sys.stdout
    out.write("// tools/gen_resample_table.py %s が作った表。手で直さない。\n" % sys.argv[1])
    out.write("// TAPS %d、PHASES %d、通す範囲は標本の周波数の %.4f 倍まで、止める範囲は %.4f 倍から、カイザー窓 %d dB、1 行の合計 %d\n"
              % (TAPS, PHASES, lo, hi, ATTEN_DB, unit))
    for p in range(PHASES + 1):
        out.write("{" + ",".join(str(v) for v in row(p, (lo + hi) / 2, unit)) + "},\n")


if __name__ == "__main__":
    main()
