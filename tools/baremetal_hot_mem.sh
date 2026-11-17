#!/bin/sh
# ベアメタル版のカーネルの逆アセンブル (Circle の Rules.mk が作る kernel*.lst) を調べる。見つけたことを出して 1 で終わる。
# tools/build_baremetal.sh がビルドの最後に呼ぶ。
#   使い方: tools/baremetal_hot_mem.sh <kernel*.lst> [kernel*.map]
#
# 1. CPU のコア (m68k::) と X68000 の本体 (x68k::。画面の合成を含む) の関数に、大きさが 128 バイト未満の定数の
#    memcpy() の呼び出しがないこと。CPU のコアは命令ごと、画面の合成は画素の 8 個ごとに通るので、小さい memcpy が
#    関数の呼び出しになると大きく遅くなる。baremetal/builtin_mem.h が読まれていれば、大きさが定数の小さい写しは
#    その場のロードとストアになり、呼び出しは残らない。名前に m68k:: か x68k:: がある関数のすべてを見る
#    (無名の名前空間 "x68k::(anonymous namespace)::f(int)"、テンプレート "void x68k::f<x68k::T>(int)" を含む)。
#    大きさは、呼び出しの前の 16 命令のうち、長さの引数 (x2) に最後に書いた命令が即値の mov かどうかで見る
#    (ldp の 2 つ目のレジスタへの書き込みも見る)。大きさが変数の写し (1 行の写しなど) は、呼び出しのままで
#    よいので数えない。memset() は調べない。
# 2. memcpy() と memset() (baremetal/mem_fast.cpp) が、ほかの関数や自分自身を呼ばず、関数の外に分岐せず、
#    NEON / 浮動小数点のレジスタとスタックを使わないこと。Circle は MMU を有効にする前の .bss の消去から memset() を
#    呼ぶ。コンパイラが変わって、ループを memset() の呼び出しや NEON の命令に置き換えると、起動しなくなる。
# 3. map を渡したときは、Circle の memcpy() / memset() (lib/util_fast.S の util_fast.o) がリンクされていないこと。
set -eu
{ [ $# -eq 1 ] || [ $# -eq 2 ]; } && [ -f "$1" ] || { echo "usage: $0 <kernel*.lst> [kernel*.map]" >&2; exit 2; }
BAD=0
awk '
  function bad(what) { m[name ": " what]++ }
  # 関数の始まり: "0000000000081234 <名前>:"
  /^[0-9a-f]+ <.*>:$/ {
    f = $0; sub(/^[0-9a-f]+ </, "", f); sub(/>:$/, "", f)
    # 引数の並びを除く。無名の名前空間の "(anonymous namespace)" は引数の並びではないので、先に置き換える
    name = f; gsub(/\(anonymous namespace\)/, "{anon}", name); sub(/\(.*/, "", name)
    hot = (name ~ /(m68k|x68k)::/)
    mem = (f == "memcpy" || f == "memset")
    if (mem) seen[f] = 1
    np = 0
    next
  }
  hot && /[ \t]bl?[ \t]+[0-9a-f]+ <memcpy>$/ {
    # 前の命令をさかのぼり、x2 / w2 に最後に書いた命令を探す
    for (i = np; i > np - 16 && i > 0; i--) {
      split(prev[i], t, "\t"); op = t[3]; args = t[4]
      if (op ~ /^(bl|blr|ret|b|br)$/) break
      # x2 / w2 に書く命令か (最初のオペランド。ldp は 2 つ目のオペランドにも書く)
      if (args !~ /^[wx]2(,|$)/ && !(op ~ /^ldp/ && args ~ /^[wx][0-9]+, [wx]2,/)) continue
      if (op ~ /^(cmp|cmn|tst|ccmp|cbz|cbnz|tbz|tbnz|str|strb|strh|stp)$/) continue  # x2 を読むだけ
      if (op == "mov" && args ~ /^[wx]2, #/) {
        v = args; sub(/^[wx]2, #/, "", v); sub(/[ \t].*/, "", v)
        if (v ~ /^0x[0-7]?[0-9a-f]$/) n[f]++  # 128 未満 (objdump は即値を 16 進で出す)
      }
      break
    }
  }
  hot && /^ +[0-9a-f]+:\t/ { prev[++np] = $0 }
  # 命令の行: "   80860:<TAB>ca000023 <TAB>eor<TAB>x3, x1, x0"
  mem && /^ +[0-9a-f]+:\t/ {
    split($0, t, "\t"); op = t[3]; args = t[4]
    sub(/[ \t]*\/\/.*/, "", args)
    if (op ~ /^(bl|blr|br)$/) bad("関数の呼び出し (" op " " args ")")
    else if (op ~ /^(b|b\.[a-z]+|cbz|cbnz|tbz|tbnz)$/) {
      if (args !~ ("<" name "(\\+0x[0-9a-f]+)?>$")) bad("関数の外への分岐 (" op " " args ")")
    } else {
      if (args ~ /(^|[ ,\[])(sp|x29|x30)([ ,\]!]|$)/) bad("スタックの使用 (" op " " args ")")
      if (args ~ /(^|[ ,\[{])[vqdshb][0-9]+([ ,\]}.]|$)/) bad("NEON / 浮動小数点のレジスタ (" op " " args ")")
    }
  }
  END {
    r = 0
    for (k in n) { printf "  %s: 小さい定数の memcpy() の呼び出しが %d か所\n", k, n[k]; r = 1 }
    for (k in m) { printf "  %s\n", k; r = 1 }
    if (!seen["memcpy"]) { print "  memcpy() が逆アセンブルにない"; r = 1 }
    if (!seen["memset"]) { print "  memset() が逆アセンブルにない"; r = 1 }
    exit r
  }' "$1" || BAD=1
if [ $# -eq 2 ]; then
  [ -f "$2" ] || { echo "  $2 がない"; exit 1; }
  if grep -q 'util_fast\.o' "$2"; then
    echo "  Circle の util_fast.o (memcpy() / memset()) がリンクされている"
    BAD=1
  fi
  grep -q 'mem_fast\.o' "$2" || { echo "  mem_fast.o がリンクされていない"; BAD=1; }
fi
exit $BAD
