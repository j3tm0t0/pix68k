#!/bin/sh
# 実機の Pi でベアメタル版を 1 回だけ動かし、ログを取ってくる。Linux の入った SD カード (NVMe) はそのまま使う。
#   使い方: tools/deploy_pi.sh <3|4|5> <ssh のホスト名> [動かす秒数 (5〜3600、既定 30)]
#   例:     tools/deploy_pi.sh 3 mypi 30
#
# 手順:
#   1. カーネルを Linux の boot パーティション (/boot/firmware) に置く
#   2. tryboot.txt を置き、reboot "0 tryboot" で再起動する。tryboot はファームウェアの機能で、
#      次の 1 回だけ config.txt の代わりに tryboot.txt で起動する (全モデル対応)。
#      ベアメタル版が途中で止まっても、次に再起動すれば (ウォッチドッグか電源の入れ直し) Linux に戻る
#   3. ベアメタル版は pix68k_deploy=<秒数> を受け取ると、ウォッチドッグを動かし、
#      指定の秒数動いたらログを /boot/firmware/pix68k/log.txt に書いて再起動する
#   4. Linux に戻ったら (再起動前と boot_id が変わったことで判定する)、ログを取ってきて表示する
set -eu
N=${1:?usage: $0 <3|4|5> <host> [seconds]}
HOST=${2:?usage: $0 <3|4|5> <host> [seconds]}
SECS=${3:-30}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
case $N in
3) IMG=$ROOT/build/baremetal-raspi3/app/kernel8.img; NAME=pix68k-rpi3.img ;;
4) IMG=$ROOT/build/baremetal-raspi4/app/kernel8-rpi4.img; NAME=pix68k-rpi4.img ;;
5) IMG=$ROOT/build/baremetal-raspi5/app/kernel_2712.img; NAME=pix68k-rpi5.img ;;
*) echo "unknown model: $N" >&2; exit 2 ;;
esac
# 0 はカーネル側で「普通の起動」の意味になり、自動で再起動しなくなるので受け付けない
# 先頭の 0 は 8 進数と解釈されるので取り除く。桁数も先に制限する (大きすぎる値は比較でエラーになるため)
case $SECS in ''|*[!0-9]*) echo "seconds must be a number: $SECS" >&2; exit 2 ;; esac
SECS=$(printf '%s' "$SECS" | sed 's/^0*//')
case $SECS in ?|??|???|????) ;; *) echo "seconds must be 5..3600" >&2; exit 2 ;; esac
if [ "$SECS" -lt 5 ] || [ "$SECS" -gt 3600 ]; then echo "seconds must be 5..3600: $SECS" >&2; exit 2; fi
# 起動元のドライブを見分けるための値 (カーネルは cmdline の値と /pix68k/id.txt が一致するドライブにログを書く)
DEPLOY_ID=$(( $(date +%s) % 1000000000 + 1 ))
[ -f "$IMG" ] || { echo "no kernel: $IMG (run tools/build_baremetal.sh $N)" >&2; exit 1; }

# 機種の違うカーネルを送ると、ウォッチドッグが動く前に止まり、電源を入れ直すまで戻らないので確かめる
MODEL=$(ssh "$HOST" "tr -d '\\0' < /proc/device-tree/model")
case "$N:$MODEL" in
"3:Raspberry Pi 3"*|"4:Raspberry Pi 4"*|"5:Raspberry Pi 5"*) ;;
*) echo "$HOST is '$MODEL', not a Raspberry Pi $N" >&2; exit 2 ;;
esac
BOOT_ID=$(ssh "$HOST" cat /proc/sys/kernel/random/boot_id)
scp -q "$IMG" "$HOST:/tmp/$NAME"
ssh "$HOST" sudo sh -s "$NAME" "$SECS" "$DEPLOY_ID" <<'REMOTE'
set -eu
NAME=$1; SECS=$2; ID=$3
B=/boot/firmware
mv /tmp/$NAME $B/$NAME
mkdir -p $B/pix68k
rm -f $B/pix68k/log.txt
echo "hello from $(hostname)" > $B/pix68k/hello.txt
echo "$ID" > $B/pix68k/id.txt
echo "pix68k_deploy=$SECS pix68k_id=$ID" > $B/pix68k_cmdline.txt
# Circle の boot/config64.txt と同じ設定。kernel_address は Pi 3 / Pi 4 用 (Pi 5 では無視される)。
# Pi 4 の armstub8-rpi4.bin は FIQ を使うときだけ要る (pix68k は使わない) ので置かない
# Pi 5 は 32 ビットのフレームバッファに framebuffer_depth=32 と framebuffer_ignore_alpha=1 が要る (ないと Circle が画面を作れずに止まる)
printf '[all]\narm_64bit=1\nkernel=%s\nkernel_address=0x80000\ncmdline=pix68k_cmdline.txt\n[pi5]\nframebuffer_depth=32\nframebuffer_ignore_alpha=1\n' "$NAME" > $B/tryboot.txt
sync
nohup sh -c 'sleep 1; reboot "0 tryboot"' >/dev/null 2>&1 &
REMOTE

echo "rebooting $HOST into pix68k for ${SECS}s ..."
# 新しく起動した Linux に戻るのを待つ (ベアメタル版の動作時間 + 起動 2 回分)
deadline=$(( $(date +%s) + SECS + 300 ))
while :; do
  sleep 10
  NOW_ID=$(ssh -o ConnectTimeout=5 -o BatchMode=yes "$HOST" cat /proc/sys/kernel/random/boot_id 2>/dev/null || true)
  # 2 回起動する (ベアメタル版 → Linux) ので、Linux が見えた時点で boot_id は必ず変わっている
  [ -n "$NOW_ID" ] && [ "$NOW_ID" != "$BOOT_ID" ] && break
  [ "$(date +%s)" -lt "$deadline" ] || { echo "$HOST did not come back" >&2; exit 1; }
done
echo "=== $HOST: /boot/firmware/pix68k/log.txt"
ssh "$HOST" 'cat /boot/firmware/pix68k/log.txt' || { echo "no log (pix68k may not have run)" >&2; exit 1; }
