#!/bin/sh
# ホスト版の設定メニューを --press で操作して確かめる。
#   1. 「Save settings」: 変えた項目 (clock) だけを足し、注釈と知らないキーを残す
#   2. ステートの保存と読み込み。合わないステート (壊したもの) は読まない
#   3. メニューを続けて開いたり閉じたりしても止まらない
#   4. 設定ファイルの中のディレクトリ (images/) に置いたイメージを選んで保存すると、設定ファイルからの相対パスで書く
#   5. 拡張ボード (FPU board、Mercury Unit、MIDI board、MIDI out) を変えて保存し、Restart でつないで作り直す
#   6. 画面の出し方 (Display のページの Scale、Scanlines、Mask) を変えて保存する。ホスト版では、前の crt= も scanline= も
#      保存時に残す (crt= は知らせて無視し、scanline= は v0.1.0 のホスト版と同じく読まない)
# 使い方: menu_test.sh <pix68k> 。ROM がなければ 77 (ctest の SKIP_RETURN_CODE)
set -eu
PIX68K=$1
ROM_DIR=${PIX68K_ROM_DIR:-$HOME/.cache/pix68k/rom}
[ -f "$ROM_DIR/IPLROM.DAT" ] || { echo "SKIP: no IPLROM.DAT"; exit 77; }
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
cd "$WORK"
mkdir images
printf '# メニューの試験\nmystery = 1   # 知らないキー\n' > images/pix68k.cfg
run() {
  "$PIX68K" -r "$ROM_DIR" --config images/pix68k.cfg --no-sram --no-wait --frames "$1" --press-after 60 --press "$2" \
    --state-dir . > out.txt 2>&1 || { cat out.txt; echo "NG: pix68k failed"; exit 1; }
  cat out.txt
}
want() { grep -qF -- "$1" out.txt || { echo "NG: $1"; exit 1; }; }

# 68000 の項目: FD0 FD1 HD0 CD-ROM CPU Clock RAM Max-speed State-slot Save-state Load-state Reset Save-settings Restart Close
run 200 menu,down,down,down,down,down,right,end,up,up,enter,esc
want 'menu: settings saved to images/pix68k.cfg'
grep -q '^# メニューの試験$' images/pix68k.cfg || { echo "NG: comment lost"; exit 1; }
grep -q '^mystery = 1   # 知らないキー$' images/pix68k.cfg || { echo "NG: unknown key lost"; exit 1; }
grep -q '^clock=10$' images/pix68k.cfg || { echo "NG: clock=10 not written"; cat images/pix68k.cfg; exit 1; }

# Save state、Load state
run 200 menu,end,up,up,up,up,up,enter,down,enter
want 'menu: state saved to ./state0.p68'
want 'menu: state loaded from ./state0.p68'
want 'menu: closed (state 0 loaded)'
# 壊したステートは読まない
printf 'X' | dd of=state0.p68 bs=1 seek=100 conv=notrunc 2>/dev/null
run 200 menu,end,up,up,up,up,enter,esc
grep -qF 'state loaded' out.txt && { echo "NG: broken state was loaded"; exit 1; }
want 'menu: closed'

# 続けて開いたり閉じたりする
run 200 menu,esc,menu,esc,menu,down,esc,menu,menu,esc,menu,esc
[ "$(grep -c 'menu: opened' out.txt)" -ge 5 ] || { echo "NG: menu did not reopen"; exit 1; }

# images/ の中のイメージを FD1 に入れて保存すると、設定ファイルからの相対パス (GAME.XDF) で書く
head -c 1261568 /dev/zero > images/GAME.XDF
run 200 menu,down,enter,down,enter,end,up,up,enter,esc
want 'menu: fd1: images/GAME.XDF'
grep -q '^fd1=GAME.XDF$' images/pix68k.cfg || { echo "NG: fd1 path"; cat images/pix68k.cfg; exit 1; }
# 次の起動で読める (images/ が二重に付かない)
"$PIX68K" -r "$ROM_DIR" --config images/pix68k.cfg --no-sram --no-wait --frames 3 > out.txt 2>&1 || { cat out.txt; echo "NG: second run"; exit 1; }
grep -qi 'cannot' out.txt && { cat out.txt; echo "NG: cannot load"; exit 1; }

# 拡張ボード: 主のページの Boards (下から Close Restart Save-settings Reset Load-state Save-state State-slot OSD Sound-mixer
# Display Boards) で
# Enter。そのページは FPU-board Mercury-Unit Neptune-X MIDI-board (MIDI-out) Back。FPU board と Mercury Unit を on、
# MIDI board を on、MIDI out を mt32 (ホスト版は uart を飛ばすので、sf2 から右に 2 回) にし、Esc で戻って保存し、Restart で作り直す
run 200 menu,end,up,up,up,up,up,up,up,up,up,up,enter,right,down,right,down,down,right,down,right,right,esc,end,up,up,enter,end,up,enter
want 'menu: settings saved to images/pix68k.cfg'
for line in fpu_board=on mercury=on midi=on midi_out=mt32; do
  grep -q "^$line\$" images/pix68k.cfg || { echo "NG: $line not written"; cat images/pix68k.cfg; exit 1; }
done
grep -q '^neptune=' images/pix68k.cfg && { echo "NG: neptune written though not changed"; exit 1; }
# Restart のあと、選んだボードをつないで作り直している (最初の起動ではつないでいない)
want 'restarting with the new settings'
sed -n '/restarting with the new settings/,$p' out.txt > after.txt
grep -qF 'FPU board (CZ-6BP1, MC68881) at $E9E000' after.txt || { echo "NG: FPU board not attached after Restart"; exit 1; }
grep -qF 'Mercury Unit at $ECC080' after.txt || { echo "NG: Mercury Unit not attached after Restart"; exit 1; }
sed '/restarting with the new settings/,$d' out.txt | grep -qF 'FPU board' && { echo "NG: FPU board before Restart"; exit 1; }
# 次の起動でも設定ファイルからつなぐ
"$PIX68K" -r "$ROM_DIR" --config images/pix68k.cfg --no-sram --no-wait --frames 3 > out.txt 2>&1 || { cat out.txt; echo "NG: third run"; exit 1; }
want 'FPU board (CZ-6BP1, MC68881) at $E9E000'
want 'Mercury Unit at $ECC080'

# 音のミキサー: 主のページの Sound mixer (下から 9 つ目) で Enter。OPM を 3dB 下げ (左 3 回)、Master (下に 6 つ) を 2dB 上げ、
# Esc で戻って保存する。再起動は要らない
run 200 menu,end,up,up,up,up,up,up,up,up,enter,left,left,left,down,down,down,down,down,down,right,right,esc,end,up,up,enter,esc
want 'menu: settings saved to images/pix68k.cfg'
grep -q '^vol_opm=-3$' images/pix68k.cfg || { echo "NG: vol_opm=-3 not written"; cat images/pix68k.cfg; exit 1; }
grep -q '^vol_master=2$' images/pix68k.cfg || { echo "NG: vol_master=2 not written"; cat images/pix68k.cfg; exit 1; }
grep -q '^vol_adpcm=' images/pix68k.cfg && { echo "NG: vol_adpcm written though not changed"; exit 1; }
"$PIX68K" -r "$ROM_DIR" --config images/pix68k.cfg --no-sram --no-wait --frames 3 > out.txt 2>&1 || { cat out.txt; echo "NG: run with vol_ settings"; exit 1; }
grep -qi 'bad value\|unknown key: vol' out.txt && { cat out.txt; echo "NG: vol_ settings not read"; exit 1; }

# 画面の出し方: 主のページの Display (下から 10 個目。OSD の上に Sound mixer がある) で Enter。そのページは Scale Scanlines Curve Mask Back。
# Scale を full に (ホスト版の既定の aspect の次)、Scanlines と Mask を on にし、Esc で戻って保存する。
# 次の起動でも設定ファイルから読む (読めない値なら bad value と出る)
run 200 menu,end,up,up,up,up,up,up,up,up,up,enter,right,down,right,down,down,right,esc,end,up,up,enter,esc
want 'menu: settings saved to images/pix68k.cfg'
grep -q '^video_scale=full$' images/pix68k.cfg || { echo "NG: video_scale=full not written"; cat images/pix68k.cfg; exit 1; }
grep -q '^video_filter=scanline,mask$' images/pix68k.cfg || { echo "NG: video_filter not written"; cat images/pix68k.cfg; exit 1; }
"$PIX68K" -r "$ROM_DIR" --config images/pix68k.cfg --no-sram --no-wait --frames 3 > out.txt 2>&1 || { cat out.txt; echo "NG: fourth run"; exit 1; }
grep -qF 'bad value' out.txt && { cat out.txt; echo "NG: video settings not read"; exit 1; }

# crt= (公開版に出ていない) は読み替えずに知らせて無視する。v0.1.0 の scale= と scanline= は、v0.1.0 のホスト版と同じく
# 読まない (知らせない)。どちらも保存で書き直さない (ベアメタル版と共用の pix68k.cfg を変えない)
printf 'clock=10\ncrt = geom  # 前の書き方\nscanline=on\n' > images/pix68k.cfg
run 200 menu,end,up,up,enter,esc
want 'crt=geom is no longer supported; use video_filter= and video_scale= (ignored)'
grep -qF 'scanline=on' out.txt && { cat out.txt; echo "NG: scanline= warned on the host"; exit 1; }
want 'menu: settings saved to images/pix68k.cfg'
printf 'clock=10\ncrt = geom  # 前の書き方\nscanline=on\n' > want.cfg
cmp -s want.cfg images/pix68k.cfg || { echo "NG: legacy lines rewritten on the host"; cat images/pix68k.cfg; exit 1; }
echo OK
