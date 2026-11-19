# tools/qemu_human68k.sh が読む (. で取り込む)。pix68k.cfg の video_scale=、video_filter= と、前の書き方 (公開版 v0.1.0 の
# scale=、scanline=。使えなくなった crt= も) から、ベアメタル版のログに出るはずの行と、出ないはずの行を決める。
# tests/host/qemu_crt_expect_test.sh が組み合わせを確かめる。
#
# qemu_human68k.sh は pix68k.cfg に scale=、crt=、scanline=、video_filter=、video_scale= の順に書く (あとの行が勝つ)。
# 実装 (platform/settings.h、baremetal/machine_run.cpp) のログを出す条件:
#   v0.1.0 の scale= と scanline= は、読み終えてから読み替える (resolveLegacySettings())。「pix68k.cfg: KEY=V is old」と知らせる。
#     scale=off は video_scale=off、auto は integer。scanline=on は video_filter=scanline、off は none。
#     video_scale= / video_filter= の行があとなら、そちらが勝つ
#   crt= は読み替えずに無視し、「pix68k.cfg: crt=V is no longer supported」と知らせる
#   起動時 (StartCrt()): 「screen: video_scale=S video_filter=F」(F は scanline,curve,mask の順に , でつなぐ。なければ none)。
#     Pi 3 (QEMU の raspi3b も) で選べない組み合わせ (curve を含むもの、full と mask の組。platform::videoAllowedPi3()) は、
#     curve、mask の順に外して (それでも選べなければ integer にして)「video_scale=S video_filter=F is not available on this Pi;
#     using ...」と知らせ、替えた値を使う。qemu_human68k.sh はふだん、選べない組み合わせのときカーネルに pix68k_crt_all=1 を
#     渡すので替わらない (PI3_LIMIT=1 なら渡さず、替わることを見る)。
#   写すとき (ShowFrame()): 表で写すのは、F に curve か mask があるときと、整数倍でない枠 (aspect、full で、枠に
#     X68000 の画面が入るとき。fill) のとき。表で写せば 10 秒ごとのログに「; video_filter=F」と、表を作ったときの
#     「screen: video table: 1 built over ...」(表ができるまでのフレームは整数倍の枠にスキャンラインだけで写すので、
#     そのときのログに「; scanlines」が出ることはある。見ない)。
#     表を使わず、F が scanline だけなら、縦の倍率が 2 以上のときだけ「; scanlines」。
#
# video_resolve <scale> <crt> <scanline> <video_filter> <video_scale>
#   最後の値を「video_scale の名前 video_filter のビット」で出す (ビットは scanline 1、curve 2、mask 4)。空は書かない行
video_resolve() {
  _vs=integer _vf=0
  case "${1:-}" in off) _vs=off ;; auto) _vs=integer ;; esac
  case "${3:-}" in on) _vf=1 ;; off) _vf=0 ;; esac
  if [ -n "${4:-}" ]; then
    _vf=0
    for _f in $(echo "$4" | tr ',' ' '); do
      case "$_f" in scanline) _vf=$(( _vf | 1 )) ;; curve) _vf=$(( _vf | 2 )) ;; mask) _vf=$(( _vf | 4 )) ;; esac
    done
  fi
  [ -n "${5:-}" ] && _vs=$5
  echo "$_vs $_vf"
}

# video_pi3_allowed <video_scale の名前> <ビット>: Pi 3 で選べるか (platform::videoAllowedPi3() と同じ)
video_pi3_allowed() {
  [ $(( $2 & 2 )) != 0 ] && return 1
  [ "$1" = full ] && [ $(( $2 & 4 )) != 0 ] && return 1
  return 0
}

# video_pi3_fallback <video_scale の名前> <ビット>: Pi 3 で選べる組み合わせに替えた値 (CKernel::StartCrt() と同じ)
video_pi3_fallback() {
  _fs=$1 _fb=$2
  if ! video_pi3_allowed "$_fs" "$_fb"; then
    _fb=$(( _fb & 5 ))
    video_pi3_allowed "$_fs" "$_fb" || _fb=$(( _fb & 3 ))
    video_pi3_allowed "$_fs" "$_fb" || _fs=integer
  fi
  echo "$_fs $_fb"
}

# video_filter_name <ビット>
video_filter_name() {
  _n=
  [ $(( $1 & 1 )) != 0 ] && _n=scanline
  [ $(( $1 & 2 )) != 0 ] && _n=${_n:+$_n,}curve
  [ $(( $1 & 4 )) != 0 ] && _n=${_n:+$_n,}mask
  echo "${_n:-none}"
}

# crt_log_expect <scale> <crt> <scanline> <video_filter> <video_scale> <ky> <fill> [<pi3_limit>]
#   ky は整数倍の枠の縦の倍率、fill は整数倍でない枠 (aspect か full) に X68000 の画面が入るなら 1 (pi3_limit で替わるときは、
#   替えたあとの枠のもの)。pi3_limit が 1 なら Pi 3 の制限で替わる。ほかは video_resolve と同じ。
#   出るはずの行を「need <文字列>」、出ないはずの行を「absent <文字列>」として 1 行ずつ出す
crt_log_expect() {
  _ky=${6:-1} _fill=${7:-0} _limit=${8:-0}
  set -- "${1:-}" "${2:-}" "${3:-}" "${4:-}" "${5:-}"
  _old=0
  [ -n "$1" ] && echo "need pix68k.cfg: scale=$1 is old" && _old=1
  [ -n "$2" ] && echo "need pix68k.cfg: crt=$2 is no longer supported"
  [ -n "$3" ] && echo "need pix68k.cfg: scanline=$3 is old" && _old=1
  [ "$_old" = 0 ] && echo 'absent is old; use'
  set -- $(video_resolve "$@")
  if [ "$_limit" = 1 ] && ! video_pi3_allowed "$1" "$2"; then
    _was="video_scale=$1 video_filter=$(video_filter_name "$2")"
    set -- $(video_pi3_fallback "$1" "$2")
    echo "need $_was is not available on this Pi; using video_scale=$1 video_filter=$(video_filter_name "$2")"
  else
    echo 'absent is not available on this Pi'
  fi
  _name=$(video_filter_name "$2")
  echo "need screen: video_scale=$1 video_filter=$_name"
  _map=0
  [ $(( $2 & 6 )) != 0 ] && _map=1
  case "$1" in aspect | full) [ "$_fill" = 1 ] && _map=1 ;; esac
  if [ "$_map" = 1 ]; then
    echo "need ; video_filter=$_name"
    echo 'need screen: video table: 1 built over '
    return 0
  fi
  if [ "$2" = 1 ] && [ "$_ky" -ge 2 ]; then echo 'need ; scanlines'; else echo 'absent ; scanlines'; fi
  echo 'absent ; video_filter='
  echo 'absent screen: video table:'
}
