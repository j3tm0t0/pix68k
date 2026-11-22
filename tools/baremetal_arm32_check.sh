#!/bin/sh
# 32 ビット (AArch32) のベアメタル版のカーネルの逆アセンブル (Circle の Rules.mk が作る kernel7.lst) を調べる。
# 見つけたことを出して 1 で終わる。tools/build_baremetal.sh 2 がビルドの最後に呼ぶ。
#   使い方: tools/baremetal_arm32_check.sh <kernel7.lst>
#
# ARM の命令の ldrd で、インデックスのレジスタが、読み込む 2 つのレジスタ (Rt と Rt+1) のどちらかと同じもの
# (「ldrd r2, [r2, r3]」など) がないこと。この形は ARM の仕様で UNPREDICTABLE で、Cortex-A7 (Pi 2) では未定義命令の
# 例外になる。strd は、インデックスが書くレジスタと同じでも読むだけなので、調べない (GNU as も警告しない)。Ubuntu のパッケージの arm-none-eabi-gcc 14.2.1 は、64 ビットの値をレジスタ + レジスタの番地から読む
# ところでこの命令を出し、できたカーネルは実機で X68000 を動かし始めた直後に
# 「core2: Undefined instruction (PC 0x204F8 ...)」で止まった (#302。命令は「ldrd r2, [r2, r3]」)。
# Homebrew の 16.2.0 と Arm GNU Toolchain 15.2.Rel1 は出さない。コンパイラを替えたときに、実機で動かす前に気づくための検査。
# (mawk でも動くよう、正規表現の {8} は使わない。)
# 調べていないもの: 書き戻し付きでベースのレジスタが Rt / Rt+1 と重なる形 (「ldrd r2, [r2], #8」など。GNU as は警告する)。
# Thumb の命令 (libgcc の中。「strd ip, lr, [sp, #-16]!」のようにレジスタを 2 つとも書く) は調べない。
set -eu
[ $# -eq 1 ] && [ -f "$1" ] || { echo "usage: $0 <kernel7.lst>" >&2; exit 2; }
awk '
  function regnum(r) {
    if (r == "sl") return 10; if (r == "fp") return 11; if (r == "ip") return 12
    if (r == "sp") return 13; if (r == "lr") return 14; if (r == "pc") return 15
    sub(/^r/, "", r); return r + 0
  }
  /^[0-9a-f]+ <.*>:$/ { f = $0; sub(/^[0-9a-f]+ </, "", f); sub(/>:$/, "", f); next }
  # ldrd / strd の行を数える (下の検査が読める形かどうかに関係なく。END で、1 つもなければ失敗にする)
  /\t(ldrd|strd)[a-z]*\t/ { any++ }
  # ARM の命令の ldrd の行: "   204f8:<TAB>e18220d3 <TAB>ldrd<TAB>r2, [r2, r3]" (命令のコードは 8 桁が 1 つ)
  /^ *[0-9a-f]+:\t[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f] *\tldrd[a-z]*\t/ {
    arm++
    split($0, t, "\t"); a = t[4]; sub(/[ \t]*[@;].*/, "", a)
    n = split(a, p, /[ ,\[\]!]+/)
    if (n < 3) next
    rt = regnum(p[1]); idx = p[3]
    if (idx !~ /^-?(r[0-9]+|sl|fp|ip|lr)$/) next
    sub(/^-/, "", idx); i = regnum(idx)
    if (i == rt || i == rt + 1) { printf "  %s: %s %s\n", f, t[3], a; bad++ }
  }
  END {
    # 逆アセンブルの形が変わって 1 つも読めなくなったら、検査が通ったことにしない
    if (any == 0) { print "  ldrd / strd が 1 つもない (逆アセンブルの形が変わった?)"; exit 1 }
    if (arm == 0) { print "  ARM の命令の ldrd を 1 つも読めなかった (逆アセンブルの形が変わった?)"; exit 1 }
    if (bad) { printf "  UNPREDICTABLE な ldrd / strd が %d 個ある (コンパイラを替えること。このファイルの先頭を参照)\n", bad; exit 1 }
  }' "$1"
