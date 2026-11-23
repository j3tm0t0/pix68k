#!/bin/sh
# tools/qemu_crt_expect.sh の crt_log_expect が、ベアメタル版のログの条件 (platform/settings.h の前の書き方の読み替え、
# baremetal/machine_run.cpp の StartCrt()、ShowFrame()、10 秒ごとのログ) どおりの期待を出すことを確かめる。
# video_scale= (off / integer / aspect / full) x video_filter= (8 とおり) x 縦の倍率と枠 (縦 2 倍で整数倍でない枠に入る /
# 縦 1 倍で入らない。off は縦 1 倍だけ) の 56 とおりと、前の書き方 (scale=、crt=、scanline=) を書いた 16 とおり、Pi 3 の制限
# (pix68k_crt_all=1 を渡さないとき) で替わるかの 8 とおり。
# 表の 1 行: scale crt scanline video_filter video_scale ky fill pi3_limit 期待 (「+文字列」は出るはず、「-文字列」は出ないはず。
# | で区切る)。
# 「-」は書かない。pix68k.cfg にはこの順に書く (あとの行が勝つ)。
#   前の書き方: v0.1.0 の scale= と scanline= は読み替えて「pix68k.cfg: KEY=V is old」。scale=off は video_scale=off、
#     auto は integer、scanline=on は video_filter=scanline。crt= は読み替えずに「pix68k.cfg: crt=V is no longer supported」。
#     scale= も scanline= も書かなければ「is old; use」は出ない
#   pi3_limit が 1 で、curve を含むか full と mask の組: curve、mask の順に外し (それでも選べなければ integer)、
#     「... is not available on this Pi; using ...」。ほかは「is not available on this Pi」は出ない
#   起動時: 「screen: video_scale=S video_filter=F」 (替えたあとの値)
#   F に curve か mask がある、か、aspect と full で枠に入る (fill): 「; video_filter=F」と「screen: video table: 1 built over」
#   表を使わず F が scanline だけ: 縦 2 倍以上のときだけ「; scanlines」
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$ROOT/tools/qemu_crt_expect.sh"
fail=0
n=0
while IFS= read -r row; do
  [ -n "$row" ] || continue
  set -- $row
  scale=$1 crt=$2 scanline=$3 filter=$4 vscale=$5 ky=$6 fill=$7 limit=$8
  [ "$scale" = - ] && scale=
  [ "$crt" = - ] && crt=
  [ "$scanline" = - ] && scanline=
  [ "$filter" = - ] && filter=
  [ "$vscale" = - ] && vscale=
  want=${row#* * * * * * * * }
  got=$(crt_log_expect "$scale" "$crt" "$scanline" "$filter" "$vscale" "$ky" "$fill" "$limit" | sed -e 's/^need /+/' -e 's/^absent /-/' | awk 'BEGIN{ORS=""} {if (NR > 1) print "|"; print}')
  n=$((n + 1))
  if [ "$got" != "$want" ]; then
    echo "NG: scale=${scale:-(none)} crt=${crt:-(none)} scanline=${scanline:-(none)} video_filter=${filter:-(none)} video_scale=${vscale:-(none)} ky=$ky fill=$fill pi3_limit=$limit"
    echo "  want: $want"
    echo "  got:  $got"
    fail=1
  fi
done <<'END_OF_TABLE'
- - - none off 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=off video_filter=none|-; scanlines|-; video_filter=|-screen: video table:
- - - scanline off 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=off video_filter=scanline|-; scanlines|-; video_filter=|-screen: video table:
- - - curve off 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=off video_filter=curve|+; video_filter=curve|+screen: video table: 1 built over 
- - - scanline,curve off 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=off video_filter=scanline,curve|+; video_filter=scanline,curve|+screen: video table: 1 built over 
- - - mask off 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=off video_filter=mask|+; video_filter=mask|+screen: video table: 1 built over 
- - - scanline,mask off 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=off video_filter=scanline,mask|+; video_filter=scanline,mask|+screen: video table: 1 built over 
- - - curve,mask off 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=off video_filter=curve,mask|+; video_filter=curve,mask|+screen: video table: 1 built over 
- - - scanline,curve,mask off 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=off video_filter=scanline,curve,mask|+; video_filter=scanline,curve,mask|+screen: video table: 1 built over 
- - - none integer 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=integer video_filter=none|-; scanlines|-; video_filter=|-screen: video table:
- - - none integer 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=integer video_filter=none|-; scanlines|-; video_filter=|-screen: video table:
- - - scanline integer 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=integer video_filter=scanline|+; scanlines|-; video_filter=|-screen: video table:
- - - scanline integer 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=integer video_filter=scanline|-; scanlines|-; video_filter=|-screen: video table:
- - - curve integer 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=integer video_filter=curve|+; video_filter=curve|+screen: video table: 1 built over 
- - - curve integer 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=integer video_filter=curve|+; video_filter=curve|+screen: video table: 1 built over 
- - - scanline,curve integer 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=integer video_filter=scanline,curve|+; video_filter=scanline,curve|+screen: video table: 1 built over 
- - - scanline,curve integer 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=integer video_filter=scanline,curve|+; video_filter=scanline,curve|+screen: video table: 1 built over 
- - - mask integer 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=integer video_filter=mask|+; video_filter=mask|+screen: video table: 1 built over 
- - - mask integer 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=integer video_filter=mask|+; video_filter=mask|+screen: video table: 1 built over 
- - - scanline,mask integer 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=integer video_filter=scanline,mask|+; video_filter=scanline,mask|+screen: video table: 1 built over 
- - - scanline,mask integer 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=integer video_filter=scanline,mask|+; video_filter=scanline,mask|+screen: video table: 1 built over 
- - - curve,mask integer 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=integer video_filter=curve,mask|+; video_filter=curve,mask|+screen: video table: 1 built over 
- - - curve,mask integer 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=integer video_filter=curve,mask|+; video_filter=curve,mask|+screen: video table: 1 built over 
- - - scanline,curve,mask integer 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=integer video_filter=scanline,curve,mask|+; video_filter=scanline,curve,mask|+screen: video table: 1 built over 
- - - scanline,curve,mask integer 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=integer video_filter=scanline,curve,mask|+; video_filter=scanline,curve,mask|+screen: video table: 1 built over 
- - - none aspect 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=none|+; video_filter=none|+screen: video table: 1 built over 
- - - none aspect 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=none|-; scanlines|-; video_filter=|-screen: video table:
- - - scanline aspect 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=scanline|+; video_filter=scanline|+screen: video table: 1 built over 
- - - scanline aspect 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=scanline|-; scanlines|-; video_filter=|-screen: video table:
- - - curve aspect 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=curve|+; video_filter=curve|+screen: video table: 1 built over 
- - - curve aspect 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=curve|+; video_filter=curve|+screen: video table: 1 built over 
- - - scanline,curve aspect 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=scanline,curve|+; video_filter=scanline,curve|+screen: video table: 1 built over 
- - - scanline,curve aspect 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=scanline,curve|+; video_filter=scanline,curve|+screen: video table: 1 built over 
- - - mask aspect 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=mask|+; video_filter=mask|+screen: video table: 1 built over 
- - - mask aspect 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=mask|+; video_filter=mask|+screen: video table: 1 built over 
- - - scanline,mask aspect 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=scanline,mask|+; video_filter=scanline,mask|+screen: video table: 1 built over 
- - - scanline,mask aspect 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=scanline,mask|+; video_filter=scanline,mask|+screen: video table: 1 built over 
- - - curve,mask aspect 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=curve,mask|+; video_filter=curve,mask|+screen: video table: 1 built over 
- - - curve,mask aspect 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=curve,mask|+; video_filter=curve,mask|+screen: video table: 1 built over 
- - - scanline,curve,mask aspect 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=scanline,curve,mask|+; video_filter=scanline,curve,mask|+screen: video table: 1 built over 
- - - scanline,curve,mask aspect 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=scanline,curve,mask|+; video_filter=scanline,curve,mask|+screen: video table: 1 built over 
- - - none full 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=full video_filter=none|+; video_filter=none|+screen: video table: 1 built over 
- - - none full 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=full video_filter=none|-; scanlines|-; video_filter=|-screen: video table:
- - - scanline full 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=full video_filter=scanline|+; video_filter=scanline|+screen: video table: 1 built over 
- - - scanline full 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=full video_filter=scanline|-; scanlines|-; video_filter=|-screen: video table:
- - - curve full 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=full video_filter=curve|+; video_filter=curve|+screen: video table: 1 built over 
- - - curve full 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=full video_filter=curve|+; video_filter=curve|+screen: video table: 1 built over 
- - - scanline,curve full 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=full video_filter=scanline,curve|+; video_filter=scanline,curve|+screen: video table: 1 built over 
- - - scanline,curve full 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=full video_filter=scanline,curve|+; video_filter=scanline,curve|+screen: video table: 1 built over 
- - - mask full 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=full video_filter=mask|+; video_filter=mask|+screen: video table: 1 built over 
- - - mask full 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=full video_filter=mask|+; video_filter=mask|+screen: video table: 1 built over 
- - - scanline,mask full 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=full video_filter=scanline,mask|+; video_filter=scanline,mask|+screen: video table: 1 built over 
- - - scanline,mask full 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=full video_filter=scanline,mask|+; video_filter=scanline,mask|+screen: video table: 1 built over 
- - - curve,mask full 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=full video_filter=curve,mask|+; video_filter=curve,mask|+screen: video table: 1 built over 
- - - curve,mask full 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=full video_filter=curve,mask|+; video_filter=curve,mask|+screen: video table: 1 built over 
- - - scanline,curve,mask full 2 1 0 -is old; use|-is not available on this Pi|+screen: video_scale=full video_filter=scanline,curve,mask|+; video_filter=scanline,curve,mask|+screen: video table: 1 built over 
- - - scanline,curve,mask full 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=full video_filter=scanline,curve,mask|+; video_filter=scanline,curve,mask|+screen: video table: 1 built over 
off - - - - 1 0 0 +pix68k.cfg: scale=off is old|-is not available on this Pi|+screen: video_scale=off video_filter=none|-; scanlines|-; video_filter=|-screen: video table:
auto - - - - 2 0 0 +pix68k.cfg: scale=auto is old|-is not available on this Pi|+screen: video_scale=integer video_filter=none|-; scanlines|-; video_filter=|-screen: video table:
- - on - - 2 0 0 +pix68k.cfg: scanline=on is old|-is not available on this Pi|+screen: video_scale=aspect video_filter=scanline|+; scanlines|-; video_filter=|-screen: video table:
- - on - - 1 0 0 +pix68k.cfg: scanline=on is old|-is not available on this Pi|+screen: video_scale=aspect video_filter=scanline|-; scanlines|-; video_filter=|-screen: video table:
- off - - - 2 0 0 +pix68k.cfg: crt=off is no longer supported|-is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=none|-; scanlines|-; video_filter=|-screen: video table:
- scanline - - - 2 0 0 +pix68k.cfg: crt=scanline is no longer supported|-is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=none|-; scanlines|-; video_filter=|-screen: video table:
- grille - - - 2 0 0 +pix68k.cfg: crt=grille is no longer supported|-is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=none|-; scanlines|-; video_filter=|-screen: video table:
- geom - - - 2 1 0 +pix68k.cfg: crt=geom is no longer supported|-is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=none|+; video_filter=none|+screen: video table: 1 built over 
- full - - - 2 1 0 +pix68k.cfg: crt=full is no longer supported|-is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=none|+; video_filter=none|+screen: video table: 1 built over 
- full off - - 2 1 0 +pix68k.cfg: crt=full is no longer supported|+pix68k.cfg: scanline=off is old|-is not available on this Pi|+screen: video_scale=aspect video_filter=none|+; video_filter=none|+screen: video table: 1 built over 
off full - - - 1 0 0 +pix68k.cfg: scale=off is old|+pix68k.cfg: crt=full is no longer supported|-is not available on this Pi|+screen: video_scale=off video_filter=none|-; scanlines|-; video_filter=|-screen: video table:
- geom - - full 2 1 0 +pix68k.cfg: crt=geom is no longer supported|-is old; use|-is not available on this Pi|+screen: video_scale=full video_filter=none|+; video_filter=none|+screen: video table: 1 built over 
- full - mask integer 2 0 0 +pix68k.cfg: crt=full is no longer supported|-is old; use|-is not available on this Pi|+screen: video_scale=integer video_filter=mask|+; video_filter=mask|+screen: video table: 1 built over 
off - on - - 1 0 0 +pix68k.cfg: scale=off is old|+pix68k.cfg: scanline=on is old|-is not available on this Pi|+screen: video_scale=off video_filter=scanline|-; scanlines|-; video_filter=|-screen: video table:
- - - - - 2 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=none|-; scanlines|-; video_filter=|-screen: video table:
- - - - - 1 0 0 -is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=none|-; scanlines|-; video_filter=|-screen: video table:
- - - mask full 2 1 1 -is old; use|+video_scale=full video_filter=mask is not available on this Pi; using video_scale=full video_filter=none|+screen: video_scale=full video_filter=none|+; video_filter=none|+screen: video table: 1 built over 
- - - scanline,mask full 2 1 1 -is old; use|+video_scale=full video_filter=scanline,mask is not available on this Pi; using video_scale=full video_filter=scanline|+screen: video_scale=full video_filter=scanline|+; video_filter=scanline|+screen: video table: 1 built over 
- - - curve integer 2 0 1 -is old; use|+video_scale=integer video_filter=curve is not available on this Pi; using video_scale=integer video_filter=none|+screen: video_scale=integer video_filter=none|-; scanlines|-; video_filter=|-screen: video table:
- - - scanline,curve,mask aspect 2 1 1 -is old; use|+video_scale=aspect video_filter=scanline,curve,mask is not available on this Pi; using video_scale=aspect video_filter=scanline,mask|+screen: video_scale=aspect video_filter=scanline,mask|+; video_filter=scanline,mask|+screen: video table: 1 built over 
- full - - - 2 1 1 +pix68k.cfg: crt=full is no longer supported|-is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=none|+; video_filter=none|+screen: video table: 1 built over 
- - - curve,mask full 2 1 1 -is old; use|+video_scale=full video_filter=curve,mask is not available on this Pi; using video_scale=full video_filter=none|+screen: video_scale=full video_filter=none|+; video_filter=none|+screen: video table: 1 built over 
- - - mask aspect 2 1 1 -is old; use|-is not available on this Pi|+screen: video_scale=aspect video_filter=mask|+; video_filter=mask|+screen: video table: 1 built over 
- - - scanline full 2 1 1 -is old; use|-is not available on this Pi|+screen: video_scale=full video_filter=scanline|+; video_filter=scanline|+screen: video table: 1 built over 
END_OF_TABLE
[ "$n" -eq 80 ] || { echo "NG: $n rows (want 80)"; fail=1; }
[ "$fail" = 0 ] && echo "OK: $n rows"
exit "$fail"
