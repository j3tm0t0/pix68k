#!/bin/sh
# tools/baremetal_hot_mem.sh の試験。逆アセンブル (kernel*.lst の形) の例を作って渡し、通るはずのものが通り、
# 失敗するはずのものが失敗することを確かめる。
#   使い方: tests/tools/baremetal_hot_mem_test.sh <tools/baremetal_hot_mem.sh の場所>
set -u
TOOL=$1
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
FAILS=0

# 命令の 1 行: insn <命令> <オペランド>
insn() { printf '   80000:\t00000000 \t%s\t%s\n' "$1" "$2"; }
func() { printf '\n0000000000080000 <%s>:\n' "$1"; }
# そのまま通る memcpy() と memset() (分岐は関数の中だけ、スタックと NEON を使わない)
good_mem() {
  func memcpy
  insn cbz 'x2, 80010 <memcpy+0x10>'
  insn ldp 'x6, x7, [x1], #16'
  insn stp 'x6, x7, [x0], #16'
  insn b.ne '80004 <memcpy+0x4>  // b.any'
  insn ret ''
  func memset
  insn and 'x1, x1, #0xff'
  insn strb 'w1, [x0], #1'
  insn stp 'x1, x1, [x3, #-32]'
  insn tbz 'w2, #3, 80020 <memset+0x20>'
  insn ret ''
}
# 関数 $1 の中で、長さの引数を $2 の命令の並びで入れて memcpy() を呼ぶ
call_in() {
  func "$1"
  insn mov 'x0, x19'
  shift
  while [ $# -gt 0 ]; do insn "$1" "$2"; shift 2; done
  insn bl '80000 <memcpy>'
  insn ret ''
}

# expect <0 か 1> <説明> [map]: $TMP/in.lst を渡して、終わりの値を比べる
expect() {
  if [ $# -eq 3 ]; then sh "$TOOL" "$TMP/in.lst" "$3" > "$TMP/out.txt" 2>&1; else sh "$TOOL" "$TMP/in.lst" > "$TMP/out.txt" 2>&1; fi
  got=$?
  if [ "$got" -ne "$1" ]; then
    echo "FAIL: $2: 終わりの値 $got (期待 $1)"
    sed 's/^/    /' "$TMP/out.txt"
    FAILS=$((FAILS + 1))
  fi
}

# ---- 通るはずのもの ----
good_mem > "$TMP/in.lst"
expect 0 "memcpy() と memset() だけ"

{ good_mem; call_in 'x68k::Renderer::renderLineEx(int, int, unsigned int*, bool, int)' lsl 'x2, x20, #1'; } > "$TMP/in.lst"
expect 0 "大きさが変数の memcpy() (x68k::)"

{ good_mem; call_in 'x68k::Renderer::drawBg(int, int, int, int, int)' mov 'x2, #0x8                   	// #8' ldp 'x0, x2, [sp]'; } > "$TMP/in.lst"
expect 0 "即値の mov のあとで ldp の 2 つ目のレジスタに書いた (大きさは変数)"

{ good_mem; call_in 'x68k::Machine::load(unsigned char const*)' mov 'x2, #0x80                  	// #128'; } > "$TMP/in.lst"
expect 0 "128 バイトの定数"

{ good_mem; call_in 'x68k::Machine::load(unsigned char const*)' mov 'x2, #0x3000                	// #12288'; } > "$TMP/in.lst"
expect 0 "大きい定数"

{ good_mem; call_in 'CKernel::Run()' mov 'x2, #0x8                   	// #8'; } > "$TMP/in.lst"
expect 0 "m68k:: でも x68k:: でもない関数の小さい定数"

{ good_mem; call_in 'CKernel::Show(x68k::Renderer const&)' mov 'x2, #0x8                   	// #8'; } > "$TMP/in.lst"
expect 0 "引数の型にだけ x68k:: がある関数"

# ---- 失敗するはずのもの: 小さい定数の memcpy() ----
for name in \
  'x68k::Renderer::drawBg(int, int, int, int, int)' \
  'x68k::Renderer::renderLine(int) [clone .part.0]' \
  'x68k::(anonymous namespace)::helper(int)' \
  'void x68k::graphicLine<x68k::GR16>(int)' \
  'void m68k::Cpu020::execOne<true>()' \
  'void m68k::Cpu020::thunk<&(void m68k::Cpu020::opMove<4>(unsigned short))>(m68k::Cpu020*, unsigned short)' \
  'm68k::Cpu020::read32(unsigned int)' \
  'x68k::Machine::reset()'; do
  { good_mem; call_in "$name" mov 'x2, #0x8                   	// #8'; } > "$TMP/in.lst"
  expect 1 "小さい定数の memcpy(): $name"
  grep -q '小さい定数の memcpy() の呼び出しが 1 か所' "$TMP/out.txt" || { echo "FAIL: 見つけた理由が出ていない: $name"; FAILS=$((FAILS + 1)); }
done

{ good_mem; call_in 'm68k::Cpu020::saveUndo()' mov 'w2, #0x20                  	// #32' add 'x1, x19, #0x40'; } > "$TMP/in.lst"
expect 1 "w2 に入れた 32 バイト"

{ good_mem; call_in 'm68k::Cpu020::saveUndo()' mov 'x2, #0x7f                  	// #127'; } > "$TMP/in.lst"
expect 1 "127 バイト"

{ good_mem; call_in 'x68k::Renderer::drawBg(int, int, int, int, int)' mov 'x2, #0x8                   	// #8' cmp 'x2, x3' str 'x2, [sp, #8]' mov 'x1, x21'; } > "$TMP/in.lst"
expect 1 "即値の mov のあとは x2 を読むだけ"

# ---- 失敗するはずのもの: memcpy() / memset() の中身 ----
bad_mem() { # $1 $2: memset() の中に足す命令
  good_mem
  insn "$1" "$2"
}
bad_mem bl '80000 <memset>' > "$TMP/in.lst";                 expect 1 "memset() が自分自身を呼ぶ"
bad_mem bl '90000 <strlen>' > "$TMP/in.lst";                 expect 1 "memset() がほかの関数を呼ぶ"
bad_mem b '90000 <strlen>' > "$TMP/in.lst";                  expect 1 "memset() が関数の外に分岐する"
bad_mem blr 'x3' > "$TMP/in.lst";                            expect 1 "memset() がレジスタの先を呼ぶ"
bad_mem stp 'q0, q0, [x3, #-32]' > "$TMP/in.lst";            expect 1 "memset() が q0 を使う"
bad_mem dup 'v0.16b, w1' > "$TMP/in.lst";                    expect 1 "memset() が v0 を使う"
bad_mem fmov 'd0, x1' > "$TMP/in.lst";                       expect 1 "memset() が d0 を使う"
bad_mem stp 'x29, x30, [sp, #-16]!' > "$TMP/in.lst";         expect 1 "memset() がスタックを使う"
bad_mem str 'x19, [sp, #16]' > "$TMP/in.lst";                expect 1 "memset() が sp を使う"
good_mem | sed 's/<memset>:$/<memset_other>:/' > "$TMP/in.lst"; expect 1 "memset() がない"
good_mem | sed 's/<memcpy>:$/<memcpy_other>:/' > "$TMP/in.lst"; expect 1 "memcpy() がない"

# ---- map ----
good_mem > "$TMP/in.lst"
printf ' .text          0x0000000000080860      0x310 mem_fast.o\n' > "$TMP/good.map"
expect 0 "map: mem_fast.o だけ" "$TMP/good.map"
printf ' .text          0x0000000000080860      0x310 mem_fast.o\n .text  0x90000  0x100 ../circle/lib/libcircle.a(util_fast.o)\n' > "$TMP/bad.map"
expect 1 "map: Circle の util_fast.o がある" "$TMP/bad.map"
printf ' .text          0x0000000000080860      0x310 main.o\n' > "$TMP/none.map"
expect 1 "map: mem_fast.o がない" "$TMP/none.map"

if [ "$FAILS" -ne 0 ]; then
  echo "baremetal_hot_mem_test: $FAILS failures"
  exit 1
fi
echo "baremetal_hot_mem_test: ok"
