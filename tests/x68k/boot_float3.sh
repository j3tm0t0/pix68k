#!/bin/sh
# 数値演算プロセッサボード (CZ-6BP1 相当) を Human68k のドライバ FLOAT3.X で使う。
# Human68k のシステムディスクの CONFIG.SYS の FLOAT2.X (ソフトウェアで計算するもの) を FLOAT3.X に書き換えた
# イメージを作り、ボードをつないで起動する。X-BASIC で sin(1.0) と exp(1.0) を表示させ、FLOAT2.X と同じ値になるかを見る。
# ボードがないと FLOAT3.X が組み込まれない (「\SYS\FLOAT3.X」で始まる行が出る) ことも確かめる。
#   boot_float3.sh <x68k_boot> <Human68k のシステムディスク> <作るイメージ>
# ディスクか ROM がなければ 77 (ctest の SKIP_RETURN_CODE)
boot="$1"
disk="$2"
out="$3"
test -f "$disk" || exit 77
python3 -I - "$disk" "$out" <<'EOF' || exit 1
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
# CONFIG.SYS と、同じ内容の \ETC\CONFIG.HD の両方を書き換える
key = b'DEVICE    = \\SYS\\FLOAT2.X'
if d.find(key) < 0:
    sys.exit('FLOAT2.X is not in CONFIG.SYS')
d = d.replace(key, key[:-8] + b'FLOAT3.X')
open(sys.argv[2], 'wb').write(d)
EOF
"$boot" -0 "$out" -f 900 -o "" --fpu-board --expect 'Co-PROCESSOR DRIVER' || exit $?
# X-BASIC は画面を消すので、組み込んだときの表示とは別に動かす
"$boot" -0 "$out" -f 900 -o "" --fpu-board --type 'basic\nprint sin(1.0),exp(1.0)\n' --after 600 \
    --expect ' 0.8414709848079         2.718281828459' || exit $?
"$boot" -0 "$out" -f 900 -o "" --expect '\SYS\FLOAT3.X' --reject 'Co-PROCESSOR DRIVER'
