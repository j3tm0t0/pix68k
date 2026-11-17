#!/bin/sh
# XEiJ の instructiontest.x (実機の結果を持つ) / fputest.x (計算で求めた期待値を持つ。FMOVECR だけは実機の値) を
# run_x で動かし、失敗がないかを見る。
#   xeij_test.sh <run_x> <68000|68030|68060> <file.x> [テストの引数...]
# ファイルがなければ飛ばす (77)。XEiJ のファイルはリポジトリに入れない
RUNX=$1
CPU=$2
X=$3
shift 3
[ -f "$X" ] || { echo "skip: $X がない"; exit 77; }
OUT=$("$RUNX" --cpu "$CPU" --limit 400000000000 "$X" "$@") || { echo "$OUT"; exit 1; }
echo "$OUT" | grep -E "^score|^tested:|ERROR"
echo "$OUT" | grep -q -E "failed[:=][1-9]" && exit 1
echo "$OUT" | grep -q -E "failed[:=]0" || exit 1
exit 0
