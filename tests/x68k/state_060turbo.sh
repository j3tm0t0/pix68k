#!/bin/sh
# 060turbo (68060) で、060turbo.sys を組み込んだ Human68k を起動しながら、ステートの保存と読み込みを確かめる
# (060SP と 060FPSP が組み込まれた状態)。X68030 の IPL ROM 1.3 の起動では 68060 の MMU は有効にならない
# (state_test が数を出す。IPLROM060.DAT のときは有効になる)。MMU が有効な間の保存は、state_test がテスト用の
# プログラムでも確かめる。IPLROM060.DAT は、同じディレクトリに ROM30_060.DAT もあるときだけ使う (state_test が読む)。
#   state_060turbo.sh <state_test> <X68030 の IPL ROM> <HUMAN302.XDF> <060turbo.sys>
# ディスクの作り方と IPL ROM の選び方は boot_060turbo.sh と同じ。
# ROM、ディスク、060turbo.sys か mtools (mcopy) がなければ飛ばす (77)。どれもリポジトリに入れない
TEST=$1
IPL30=$2
DISK=$3
SYS=$4
if [ -f "$HOME/.cache/pix68k/rom/IPLROM060.DAT" ] && [ -f "$HOME/.cache/pix68k/rom/ROM30_060.DAT" ]; then
  IPL="$HOME/.cache/pix68k/rom/IPLROM060.DAT"
else
  IPL="$IPL30"
fi
[ -f "$IPL" ] || { echo "skip: $IPL がない"; exit 77; }
echo "IPL ROM: $IPL"
[ -f "$DISK" ] || { echo "skip: $DISK がない"; exit 77; }
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
  printf 'DEVICE    = \\060TURBO.SYS\r\n'
  grep -v -i 'FLOAT2' "$TMP/orig.sys"
} > "$TMP/CONFIG.SYS"
(cd "$TMP" && mcopy -o -i boot.xdf CONFIG.SYS ::CONFIG.SYS && mcopy -o -i boot.xdf 060turbo.sys ::060TURBO.SYS) || exit 1
"$TEST" -0 "$TMP/boot.xdf" --cpu 68060 --ipl "$IPL"
