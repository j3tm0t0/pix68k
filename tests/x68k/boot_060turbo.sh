#!/bin/sh
# 060turbo (68060) で Human68k を起動し、DIR を打つ。
#   boot_060turbo.sh <x68k_boot> <X68030 の IPL ROM> <HUMAN302.XDF> <060turbo.sys | -> [060turbo.sys のオプション]
#                    [x68k_boot に足す引数 (「--clock 75」など。空白で区切る)]
# 060turbo.sys を渡したときは、HUMAN302.XDF を一時ディレクトリに写し、CONFIG.SYS の先頭に DEVICE = \060TURBO.SYS を
# 足して起動する (FLOAT2.X の行は消す。68060 の FPU にない命令は 060turbo.sys の 060FPSP が補う)。- ならそのまま起動する。
# オプションに -xm を渡すと拡張モード (ローカルメモリを Human68k のメモリにつなぎ、COMMAND.X をローカルメモリで動かす)
# で起動する。
# IPL ROM は、~/.cache/pix68k/rom/IPLROM060.DAT (68060 で動く IPL ROM) と、組で使う ROM30_060.DAT ($FC0000 からの ROM) が
# 両方あれば x68k_boot が自分で選ぶものを使い、なければ X68030 の IPL ROM を使う (x68k/machine.cpp の patchIplFor060)。
# IPLROM060.DAT だけでは IPL ROM 1.6 が「SCSIINROM 16 is required」で止まる。テストを動かすときに決める。
# ROM、ディスク、060turbo.sys か mtools (mcopy) がなければ飛ばす (77)。どれもリポジトリに入れない
BOOT=$1
IPL30=$2
DISK=$3
SYS=$4
OPT=$5
EXTRA=$6
if [ -f "$HOME/.cache/pix68k/rom/IPLROM060.DAT" ] && [ -f "$HOME/.cache/pix68k/rom/ROM30_060.DAT" ]; then
  set -- --cpu 68060 $EXTRA
  echo "IPL ROM: $HOME/.cache/pix68k/rom/IPLROM060.DAT"
else
  [ -f "$IPL30" ] || { echo "skip: $IPL30 がない"; exit 77; }
  set -- --cpu 68060 --ipl "$IPL30" $EXTRA
  echo "IPL ROM: $IPL30"
fi
[ -f "$DISK" ] || { echo "skip: $DISK がない"; exit 77; }
if [ "$SYS" = - ]; then
  exec "$BOOT" "$@" -0 "$DISK" -f 900 -o "" --type 'dir\n' --expect "A>dir" --expect "COMMAND            X"
fi
[ -f "$SYS" ] || { echo "skip: $SYS がない"; exit 77; }
command -v mcopy > /dev/null || { echo "skip: mtools (mcopy) がない"; exit 77; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cp "$DISK" "$TMP/boot.xdf"
chmod u+w "$TMP/boot.xdf"
cp "$SYS" "$TMP/060turbo.sys"
# mcopy には一時ディレクトリの中の相対パスで渡す (Windows の mcopy は C:/... の C: を mtools のドライブ名と取り違える)
(cd "$TMP" && mcopy -i boot.xdf ::CONFIG.SYS orig.sys) || exit 1
{
  printf 'DEVICE    = \\060TURBO.SYS %s\r\n' "$OPT"
  grep -v -i 'FLOAT2' "$TMP/orig.sys"
} > "$TMP/CONFIG.SYS"
(cd "$TMP" && mcopy -o -i boot.xdf CONFIG.SYS ::CONFIG.SYS && mcopy -o -i boot.xdf 060turbo.sys ::060TURBO.SYS) || exit 1
# 起動の表示 (060turbo.sys と 060SP の組み込み) を見てから、別に起動して DIR を打つ (DIR の表示で起動の表示が流れるため)
# (拡張モードでは表示の行が増えて 060turbo.sys の題名の行が画面の外へ流れるので、題名の行は見ない)
if [ -n "$OPT" ]; then
  "$BOOT" "$@" -0 "$TMP/boot.xdf" -f 700 -o "" --expect "M68060 Software Package" --expect "Command version" || exit 1
else
  "$BOOT" "$@" -0 "$TMP/boot.xdf" -f 700 -o "" \
    --expect "060turbo.sys version" --expect "M68060 Software Package" --expect "Command version" || exit 1
fi
"$BOOT" "$@" -0 "$TMP/boot.xdf" -f 1100 -o "" --type 'dir\n' \
  --expect "A>dir" --expect "060TURBO           SYS"
