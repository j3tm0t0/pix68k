#!/bin/sh
# 060turbo (68060) で、内蔵 SCSI のハードディスクから Human68k を 060turbo.sys の拡張モード (-xm) で起動する。
#   boot_060turbo_scsi.sh <x68k_boot> <IPL ROM> <HUMAN302.XDF> <060turbo.sys> <リポジトリの tools> [$FC0000 からの ROM]
# 拡張モードでは COMMAND.X の読み込み先がローカルメモリになる。
# - $FC0000 からの ROM を渡さないとき: IPL ROM は X68030 のもの (IPL ROM 1.3)。その SCSI IOCS は DMA で転送するので、
#   x68k/machine.cpp の patchIplFor060 の書き換えがないと、COMMAND.X を読み込んだところで止まる
# - 渡したとき: IPL ROM は 68060 で動く IPL ROM 1.6、$FC0000 からの ROM はそれと組で使う SCSI の ROM。その SCSI IOCS も
#   DMA で転送するので、DMAC のチャネル 1 をローカルメモリへ届ける互換の処理 (Machine::dmaMemAddr) がないと止まる (#167)
# ディスクのイメージは tests/x68k/make_scsi_hds.py で一時ディレクトリに作る。
# ROM、ディスク、060turbo.sys か python3 がなければ飛ばす (77)。どれもリポジトリに入れない
BOOT=$1
IPL=$2
DISK=$3
SYS=$4
TOOLS=$5
SCSIROM=$6
[ -f "$IPL" ] || { echo "skip: $IPL がない"; exit 77; }
if [ -n "$SCSIROM" ]; then
  [ -f "$SCSIROM" ] || { echo "skip: $SCSIROM がない"; exit 77; }
  set -- --scsi-rom "$SCSIROM"
else
  set --
fi
[ -f "$DISK" ] || { echo "skip: $DISK がない"; exit 77; }
[ -f "$SYS" ] || { echo "skip: $SYS がない"; exit 77; }
command -v python3 > /dev/null || { echo "skip: python3 がない"; exit 77; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
python3 -I "$(dirname "$0")/make_scsi_hds.py" "$TOOLS" "$DISK" "$TMP/boot.hds" "$SYS" -xm || exit 1
"$BOOT" --cpu 68060 --ipl "$IPL" "$@" --local-mem 16 -s "$TMP/boot.hds" -f 900 -o "" --type 'dir\n' \
  --expect "A>dir" --expect "060TURBO           SYS"
