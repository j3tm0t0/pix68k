#!/bin/sh
# QEMU (raspi3b) でベアメタル版を起動し、M0 の機能が動くことを UART のログで確かめる。
#   使い方: tools/qemu_smoke.sh [kernel8.img]
#   必要なもの: qemu-system-aarch64、mtools (SD カードのイメージを作る)、python3
# 決まった時間だけ待つのではなく、ログに必要な行が出るのを待ってから次の操作をする (上限 TIMEOUT 秒)。
set -eu
# QEMU が先に終わってモニタの FIFO に読み手がいなくなっても、書き込みで死なずに終了コードを確かめられるようにする
trap '' PIPE
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KERNEL=${1:-$ROOT/build/baremetal-raspi3-qemu/app/kernel8.img}
TIMEOUT=${TIMEOUT:-120}
WORK=$(mktemp -d)
QPID=
cleanup() {
  [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true
  exec 3>&- 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT
# シグナルで止められたときも cleanup が動くようにする (dash は、シグナルで終わるときに EXIT の trap を呼ばない)
trap 'exit 1' INT TERM HUP

# 実機の SD カードと同じく、MBR の第 1 パーティション (1MB 目から) を FAT32 にしたイメージを作り、
# 確認用のファイルを置く
dd if=/dev/zero of="$WORK/sd.img" bs=1048576 count=64 2>/dev/null
python3 - "$WORK/sd.img" <<'PY'
import struct, sys
start, count = 2048, 64 * 2048 - 2048  # 512 バイトのセクタ単位
entry = struct.pack('<B3sB3sII', 0, b'\xfe\xff\xff', 0x0c, b'\xfe\xff\xff', start, count)  # 0x0c: FAT32 (LBA)
with open(sys.argv[1], 'r+b') as f:
    f.seek(446); f.write(entry); f.seek(510); f.write(b'\x55\xaa')
PY
mformat -i "$WORK/sd.img@@1M" -F -v PIX68K ::
mmd -i "$WORK/sd.img@@1M" ::/pix68k
echo "hello from qemu" > "$WORK/hello.txt"
mcopy -i "$WORK/sd.img@@1M" "$WORK/hello.txt" ::/pix68k/hello.txt

# QEMU のモニタには FIFO 経由でコマンドを送る
mkfifo "$WORK/mon"
qemu-system-aarch64 -M raspi3b -kernel "$KERNEL" -serial "file:$WORK/serial.txt" -monitor stdio \
  -display none -usb -device usb-kbd -device usb-mouse -drive "file=$WORK/sd.img,if=sd,format=raw" \
  < "$WORK/mon" > "$WORK/qemu.out" 2> "$WORK/qemu.err" &
QPID=$!
exec 3> "$WORK/mon"
START=$(date +%s)

# ログに $1 が出るまで待つ。QEMU が先に終わったり、時間切れになったりしたら失敗
wait_for() {
  while ! grep -qF "$1" "$WORK/serial.txt" 2>/dev/null; do
    if ! kill -0 "$QPID" 2>/dev/null; then echo "QEMU exited while waiting for: $1" >&2; return 1; fi
    if [ $(( $(date +%s) - START )) -ge "$TIMEOUT" ]; then echo "timeout waiting for: $1" >&2; return 1; fi
    sleep 1
  done
}

fail=0
if wait_for 'keyboard attached' && wait_for 'mouse attached'; then
  echo "sendkey a" >&3 2>/dev/null || true
  echo "mouse_move 10 5" >&3 2>/dev/null || true
  wait_for 'mouse: buttons=0 dx=10 dy=5' || fail=1
  wait_for '68000 benchmark (core 1)' || fail=1
  wait_for 'secondary cores started: mask=E' || fail=1
else
  fail=1
fi
echo quit >&3 2>/dev/null || true
exec 3>&-
QRC=0
wait "$QPID" || QRC=$?
QPID=

cat "$WORK/serial.txt" 2>/dev/null || true
echo
if [ "$QRC" -ne 0 ]; then
  echo "NG: QEMU exited with status $QRC"
  cat "$WORK/qemu.err"
  fail=1
fi
for pattern in 'pix68k/hello.txt: hello from qemu' 'keyboard attached' 'mouse attached' \
  'secondary cores started: mask=E' '68000 benchmark (core 1): 200000002 cycles' 'key: mod=00 04' \
  'mouse: buttons=0 dx=10 dy=5'; do
  if grep -qF "$pattern" "$WORK/serial.txt" 2>/dev/null; then
    echo "ok: $pattern"
  else
    echo "NG: $pattern"
    fail=1
  fi
done
exit $fail
