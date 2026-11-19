#!/bin/sh
# QEMU (raspi3b) でベアメタル版を起動し、Human68k が立ち上がって DIR が打てること、USB マウスの動きが
# X68000 のマウス (SCC) に届くことを確かめる。
#   使い方: tools/qemu_human68k.sh [kernel8.img]
#   必要なもの: qemu-system-aarch64、mtools、python3、ROM と Human68k の起動ディスク
#     ROM:     ${PIX68K_ROM_DIR:-~/.cache/pix68k/rom}/IPLROM.DAT と CGROM.DAT
#     ディスク: ${HUMAN68K_DISK:-~/.cache/pix68k/disk/HUMAN302.XDF}
#   ROM かディスクがなければ、何もせずに成功で終わる (ROM のない CI のため)。
#   SCREENSHOT=out.png を指定すると、DIR のあとの画面を PNG で保存する (macOS の sips で変換する。out.ppm なら変換しない)。
#   FB_SIZE=1920x1080 のように指定すると、フレームバッファをその大きさにする (既定 768x512。画面の拡大を見るとき)。
#   VIDEO_SCALE=off (か integer、aspect、full) を指定すると、pix68k.cfg に video_scale= を書く (画面の拡大の仕方)。
#   VIDEO_FILTER=scanline,curve,mask (並べ方は自由。none も書ける) を指定すると、pix68k.cfg に video_filter= を書き、
#   ブラウン管のように見せる効果を掛ける (platform/crt_effect.h。FB_SIZE=1920x1080 と SCREENSHOT で見る)。
#   SCALE=off、SCANLINE=on (v0.1.0 の書き方) と CRT=full (使えなくなった書き方) を指定すると、scale=、crt=、scanline= を、
#   video_filter= と video_scale= より前の行に書く (scale= と scanline= は読み替えて、crt= は無視して知らせることを見る)。Pi 3 で選べない組み合わせ (curve を含むもの、full と mask の組)
#   のときは、選べるよう、カーネルに pix68k_crt_all=1 を渡す。PI3_LIMIT=1 なら渡さず、選べる組み合わせに替わることを見る。
#   MESSAGE_PPM_COPY=out.ppm を HOSTNET=1 と指定すると、POST /message で左上にメッセージを出したときの画面 (QEMU の screendump) を写す。
#   BEZEL=cz-600de (か cz-600db) を指定すると、video_filter= にモニタの枠の名前を足し (VIDEO_FILTER がなければ枠だけ)、
#   枠の絵 (tools/bezel_cz600d.py と tools/bezel_pack.py で作る。numpy と Pillow が要る) を SD カードのイメージの /pix68k/ に写す。
#   FB_SIZE=1920x1080 なら、絵を読み、X68000 の画面をガラスの矩形 (1160x870) に写したログを見る。ほかの大きさなら、
#   大きさが違うので枠なしで写すログを見る。BEZEL_FILE=none なら絵を写さず、ないので枠なしで写すログを見る。
#   BEZEL_FILE=bad なら壊れた絵を写し、読めないので枠なしで写すログを見る。
#   OSD=fps を指定すると、pix68k.cfg に osd=fps を書き、画面の右上にフレームの数と速さを出す (SCREENSHOT で見る。既定の osd=on でも、余白がなければ X68000 の画面に重ねて出す。違うのはフレームバッファの高さが 148 ラインより低いときだけ)。
#   BOOT=hd を指定すると、起動ディスクから tools/make_sasi_hdf.py で SASI のイメージを作り、
#   フロッピーディスクの代わりに pix68k.cfg の hd0= でつないで、SASI から起動する。
# RENDER_CORE=off を指定すると、pix68k.cfg に render_core=off を書き、画面の合成をコア 1 で行う。
# SOUND_CORE=off も同じく sound_core=off を書き、音の合成をコア 1 で行う。
# FAIL_CORE=2 を指定すると、カーネルに pix68k_fail_core=2 を渡し、コア 2 以降が起動しなかったときの
# 動き (コア 1 が代わりに合成する) を確かめる。FAIL_CORE=3 ならコア 3 だけ (音をコア 1 で合成する)。
# WFE=0 を指定すると、カーネルに pix68k_wfe=0 を渡し、待つ間を wfe で眠らせずに空回りで待つ (前の動きと比べるため)。
# GPU_SCALE=2 (か 1x2 など) を指定すると、カーネルに pix68k_gpu_scale を渡し、フレームバッファを FB_SIZE の 1/2 にする
# (表示への拡大は GPU に任せる設定。QEMU は拡大せず、小さいフレームバッファをそのまま出す)。FB_SIZE=1920x1080 と使う。
# FB_ROWS=0 を指定すると、カーネルに pix68k_fb_rows=0 を渡し、変わっていない行も毎回フレームバッファに写す。
# 音は pix68k_sound=null (作った音を捨てる) で作る。SOUND_OUT=off なら音を出さない設定 (コア 3 を使わない) で動かす。
# どの設定でも、画面と音をどのコアで合成したかのログを確かめる。
# NEPTUNE=1 を指定すると、pix68k.cfg に neptune=on と neptune_mac=02:00:00:12:34:56 を書き、QEMU に USB の Ethernet (-device usb-net と
# -netdev user) をつなぐ。ネットワークデバイスが見つかること、X68000 の MAC アドレスから出した ARP の問い合わせ
# (pix68k_net_probe=1) に、QEMU のゲートウェイ (10.0.2.2) から X68000 宛ての答えが届くことを確かめる。
# HOSTNET=1 を指定すると、pix68k.cfg に net=on (ネットワークの口。baremetal/host_net.h) を書き、QEMU に USB の Ethernet
# (-device usb-net と -netdev user。ゲストのポート 80 をホストの 127.0.0.1:$HOSTNET_HTTP_PORT に転送) をつなぐ。
# DHCP でアドレスが決まること、curl でファイルの読み書き (4MB のファイルを書いて読み直す)、使っているディスクのイメージの
# 書き換えを断ること、トークンがなければ書けないこと、/log、/thermal、/screen.ppm、/type、/key、syslog (ホストの UDP の
# $HOSTNET_SYSLOG_PORT で受ける) を確かめ、最後に /reboot で再起動を頼む。NEPTUNE=1 と同時には使えない
# (ネットワークの口は動かさない。そのことをログで確かめる)。SCREEN_PPM_COPY=out.ppm なら、DIR のあとに取った /screen.ppm を写す。
# RS232C=1 を HOSTNET=1 と指定すると、pix68k.cfg に rs232c=tcp:2324 を書き、ゲストの 2324 番をホストの 127.0.0.1:$RS232C_PORT
# (既定 12324) に転送する。ホストからつなぎ、モニタから打った「echo pix68kout>aux」の文字列が届くことと、ホストから送った
# 文字列と Ctrl+Z が「COPY AUX CON」で画面に出ることを確かめる (RS-232C のドライバを組み込んだ起動ディスクが要る)。
# MIDI_SF2=file.sf2 を指定すると、pix68k.cfg に midi=on と midi_sf2= を書き、その SoundFont を SD カードのイメージに写す
# (SF2 はリポジトリに入れない。32MB ほどまで)。MIDI ボードがつながり、コア 3 の音源が SoundFont を読んだことをログで確かめる。
# SoundFont は X68000 を動かし始めてから読む (コア 1 が動き出したログより後に、読み終えたログが出る) ことも確かめる。
# MIDI_MT32_CTRL=ctrl.rom と MIDI_MT32_PCM=pcm.rom を指定すると、pix68k.cfg に midi=on、midi_out=mt32 と ROM の名前を書き、
# 2 つの ROM を SD カードのイメージに写す (ROM はリポジトリに入れない)。MIDI ボードがつながり、Munt が ROM を読んで開いたことと、
# それがコア 1 が動き出したログより後であることをログで確かめる。MIDI_SF2 と同時には使えない。
# MIDI_SC55=1 を指定すると、pix68k.cfg に midi=on、midi_out=sc55 と midi_sc55=SC55 を書く。QEMU は Pi 3 なので、SC-55 の音源は
# 使わない (遅すぎる) とログに出し、MIDI を鳴らさずに続けることを確かめる (ROM は要らない)。MIDI_SF2 などと同時には使えない。
# MENU=1 を指定すると、DIR のあとに設定メニュー (Scroll Lock) を開き、クロックを変えて設定を保存し、
# ステートを保存して読み込み、閉じたあとにもう一度 DIR が打てることを確かめる。pix68k.cfg の注釈が残り、
# clock=10 が足されたことも見る。MENU_SHOT=out.png ならメニューを開いた画面を保存する。
# BOOT=hd と一緒に指定したときは、FD0 の入れ替え (HUMAN2.XDF) を飛ばしてリセットだけを行い、pix68k.cfg は fd0= の行の
# 代わりに hd0= の行が残っていることを見る。Restart のあとは hd0 がまたつながることを見る。
# WRITEBACK=1 を指定すると、DIR のあとに「echo pix68kwb>wb.txt」でファイルを書き、イメージのファイルへの書き戻しの
# ログ (FD は「fd0: image file replaced」、BOOT=hd は「hd0: written back」) を待つ。QEMU を終えたあとに SD カードの
# イメージからディスクのイメージを取り出して書いた文字列があることを確かめ、同じ SD カードのイメージでもう一度
# 起動して「type wb.txt」で読めることを確かめる。
# REBOOTKEY=1 を WRITEBACK=1 と指定すると、ファイルを書いたあと書き戻しを待たずに Ctrl + Alt + Delete を送り、再起動の前に
# 書き戻すこと (ログと、終えたあとの SD カードのイメージの中身) と、次の起動に入ることを確かめる。
# STREAM=1 を指定すると、カーネルに pix68k_stream_min=0 を渡し、ハードディスクのイメージを全体をメモリに読まずに、
# 使うチャンクだけを読む (BOOT=hd と使う。大きなイメージの読み込みを QEMU で試す)。
# そのあと同じ SD カードのイメージでもう一度起動し、1 回目に書き戻した SRAM を読むかを見る。
# 画面の中身は、カーネルの pix68k_textdump=1 で UART のログに出るテキスト画面のダンプ (text|...) で見る。
# QEMU の 68000 は実時間より遅いことがあるので、時間の上限 (TIMEOUT 秒) は長めにしてある。
set -eu
trap '' PIPE
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KERNEL=${1:-$ROOT/build/baremetal-raspi3-qemu/app/kernel8.img}
ROM_DIR=${PIX68K_ROM_DIR:-$HOME/.cache/pix68k/rom}
DISK=${HUMAN68K_DISK:-$HOME/.cache/pix68k/disk/HUMAN302.XDF}
TIMEOUT=${TIMEOUT:-600}
SCREENSHOT=${SCREENSHOT:-}
BOOT=${BOOT:-fd}
FB_SIZE=${FB_SIZE:-768x512}

for f in "$ROM_DIR/IPLROM.DAT" "$ROM_DIR/CGROM.DAT" "$DISK"; do
  if [ ! -f "$f" ]; then
    echo "SKIP: $f not found"
    exit 0
  fi
done
[ -f "$KERNEL" ] || { echo "NG: $KERNEL not found (tools/build_baremetal.sh 3 --qemu)" >&2; exit 1; }

WORK=$(mktemp -d)
QPID=
SPID=
cleanup() {
  [ -n "$QPID" ] && kill "$QPID" 2>/dev/null || true
  [ -n "$SPID" ] && kill "$SPID" 2>/dev/null || true
  exec 3>&- 2>/dev/null || true
  # SERIAL_COPY=ファイル名 なら、シリアルのログをそこに写してから消す (落ちたときに調べるため)
  [ -n "${SERIAL_COPY:-}" ] && cp "$WORK/serial.txt" "$SERIAL_COPY" 2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT

# SD カードのイメージ (MBR の第 1 パーティションが FAT32)。ROM とディスク、設定ファイルを /pix68k に置く。
# ROM とディスクはこの一時ファイルに写すだけで、リポジトリには置かない
dd if=/dev/zero of="$WORK/sd.img" bs=1048576 count=64 2>/dev/null
python3 - "$WORK/sd.img" <<'PY'
import struct, sys
start, count = 2048, 64 * 2048 - 2048
entry = struct.pack('<B3sB3sII', 0, b'\xfe\xff\xff', 0x0c, b'\xfe\xff\xff', start, count)
with open(sys.argv[1], 'r+b') as f:
    f.seek(446); f.write(entry); f.seek(510); f.write(b'\x55\xaa')
PY
IMG="$WORK/sd.img@@1M"
mformat -i "$IMG" -F -v PIX68K ::
mmd -i "$IMG" ::/pix68k
mcopy -i "$IMG" "$ROM_DIR/IPLROM.DAT" ::/pix68k/IPLROM.DAT
mcopy -i "$IMG" "$ROM_DIR/CGROM.DAT" ::/pix68k/CGROM.DAT
if [ "$BOOT" = hd ]; then
  python3 -I "$ROOT/tools/make_sasi_hdf.py" "$DISK" "$WORK/HUMAN302.HDF"
  mcopy -i "$IMG" "$WORK/HUMAN302.HDF" ::/pix68k/HUMAN302.HDF
  printf '# pix68k の設定\nhd0 = HUMAN302.HDF\n' > "$WORK/pix68k.cfg"
  ATTACHED='hd0: SD:/pix68k/HUMAN302.HDF'
  IMAGE_FILE=HUMAN302.HDF
  WRITTEN='hd0: written back'
else
  mcopy -i "$IMG" "$DISK" ::/pix68k/HUMAN302.XDF
  # メニューで入れ替えるディスク (中身は同じ)
  if [ "${MENU:-}" = 1 ]; then mcopy -i "$IMG" "$DISK" ::/pix68k/HUMAN2.XDF; fi
  printf '# pix68k の設定\nfd0 = HUMAN302.XDF\n' > "$WORK/pix68k.cfg"
  ATTACHED='fd0: SD:/pix68k/HUMAN302.XDF'
  IMAGE_FILE=HUMAN302.XDF
  WRITTEN='fd0: image file replaced'
fi
if [ -n "${RENDER_CORE:-}" ]; then printf 'render_core = %s\n' "$RENDER_CORE" >> "$WORK/pix68k.cfg"; fi
if [ -n "${SCALE:-}" ]; then printf 'scale = %s\n' "$SCALE" >> "$WORK/pix68k.cfg"; fi
if [ -n "${CRT:-}" ]; then printf 'crt = %s\n' "$CRT" >> "$WORK/pix68k.cfg"; fi
if [ -n "${SCANLINE:-}" ]; then printf 'scanline = %s\n' "$SCANLINE" >> "$WORK/pix68k.cfg"; fi
# 枠を写すか (絵を写し、フレームバッファが 1920x1080 で、GPU で拡大しない)。左上のメッセージの背景が変わる
BEZEL_ON=0
if [ -n "${BEZEL:-}" ] && [ "${BEZEL_FILE:-}" != none ] && [ "${BEZEL_FILE:-}" != bad ] && [ "$FB_SIZE" = 1920x1080 ] &&
  [ -z "${GPU_SCALE:-}" ]; then
  BEZEL_ON=1
fi
if [ -n "${BEZEL:-}" ]; then
  printf 'video_filter = %s\n' "${VIDEO_FILTER:+$VIDEO_FILTER,}$BEZEL" >> "$WORK/pix68k.cfg"
  case "${BEZEL_FILE:-}" in
    none) ;;
    bad)
      printf 'PXBZ broken' > "$WORK/bezel-$BEZEL.bin"
      mcopy -i "$IMG" "$WORK/bezel-$BEZEL.bin" "::/pix68k/bezel-$BEZEL.bin" ;;
    *)
      mkdir -p "$WORK/bezel"
      python3 "$ROOT/tools/bezel_cz600d.py" "$WORK/bezel" >/dev/null
      python3 "$ROOT/tools/bezel_pack.py" "$WORK/bezel/bezel-$BEZEL.png" "$WORK/bezel-$BEZEL.bin" >/dev/null
      mcopy -i "$IMG" "$WORK/bezel-$BEZEL.bin" "::/pix68k/bezel-$BEZEL.bin" ;;
  esac
elif [ -n "${VIDEO_FILTER:-}" ]; then
  printf 'video_filter = %s\n' "$VIDEO_FILTER" >> "$WORK/pix68k.cfg"
fi
if [ -n "${VIDEO_SCALE:-}" ]; then printf 'video_scale = %s\n' "$VIDEO_SCALE" >> "$WORK/pix68k.cfg"; fi
if [ -n "${OSD:-}" ]; then printf 'osd = %s\n' "$OSD" >> "$WORK/pix68k.cfg"; fi
if [ -n "${SOUND_CORE:-}" ]; then printf 'sound_core = %s\n' "$SOUND_CORE" >> "$WORK/pix68k.cfg"; fi
if [ -n "${MIDI_SF2:-}" ]; then
  mcopy -i "$IMG" "$MIDI_SF2" ::/pix68k/test.sf2
  printf 'midi = on\nmidi_sf2 = test.sf2\n' >> "$WORK/pix68k.cfg"
fi
if [ -n "${MIDI_MT32_CTRL:-}" ]; then
  mcopy -i "$IMG" "$MIDI_MT32_CTRL" ::/pix68k/MT32CTRL.ROM
  mcopy -i "$IMG" "$MIDI_MT32_PCM" ::/pix68k/MT32PCM.ROM
  printf 'midi = on\nmidi_out = mt32\nmidi_mt32_ctrl = MT32CTRL.ROM\nmidi_mt32_pcm = MT32PCM.ROM\n' >> "$WORK/pix68k.cfg"
fi
if [ "${MIDI_SC55:-}" = 1 ]; then
  printf 'midi = on\nmidi_out = sc55\nmidi_sc55 = SC55\n' >> "$WORK/pix68k.cfg"
fi
NETDEV=
if [ "${NEPTUNE:-}" = 1 ]; then
  printf 'neptune = on\nneptune_mac = 02:00:00:12:34:56\n' >> "$WORK/pix68k.cfg"
  NETDEV="-netdev user,id=net0 -device usb-net,netdev=net0"
fi
HTTP_PORT=${HOSTNET_HTTP_PORT:-18080}
RS232C_PORT=${RS232C_PORT:-12324}
SYSLOG_PORT=${HOSTNET_SYSLOG_PORT:-15514}
if [ "${HOSTNET:-}" = 1 ]; then
  printf 'net = on\nnet_token = pix68ktest\nsyslog = 10.0.2.2:%s\n' "$SYSLOG_PORT" >> "$WORK/pix68k.cfg"
  if [ -z "$NETDEV" ]; then
    FWD="hostfwd=tcp:127.0.0.1:$HTTP_PORT-:80"
    if [ "${RS232C:-}" = 1 ]; then FWD="$FWD,hostfwd=tcp:127.0.0.1:$RS232C_PORT-:2324"; fi
    NETDEV="-netdev user,id=net0,$FWD -device usb-net,netdev=net0"
  fi
  if [ "${RS232C:-}" = 1 ]; then printf 'rs232c = tcp:2324\n' >> "$WORK/pix68k.cfg"; fi
  # syslog を受けてファイルに書く (QEMU の -netdev user では、ゲストから 10.0.2.2 に送るとホストの 127.0.0.1 に届く)
  python3 -I -c '
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", int(sys.argv[1])))
with open(sys.argv[2], "ab", buffering=0) as f:
    while True:
        d, _ = s.recvfrom(4096)
        f.write(d + b"\n")
' "$SYSLOG_PORT" "$WORK/syslog.txt" &
  SPID=$!
fi
mcopy -i "$IMG" "$WORK/pix68k.cfg" ::/pix68k/pix68k.cfg

mkfifo "$WORK/mon"
APPEND="width=${FB_SIZE%x*} height=${FB_SIZE#*x} pix68k_textdump=1"
APPEND="$APPEND pix68k_sound=${SOUND_OUT:-null}"
if [ -n "${FAIL_CORE:-}" ]; then APPEND="$APPEND pix68k_fail_core=$FAIL_CORE"; fi
if [ -n "${WFE:-}" ]; then APPEND="$APPEND pix68k_wfe=$WFE"; fi
if [ -n "${GPU_SCALE:-}" ]; then APPEND="$APPEND pix68k_gpu_scale=$GPU_SCALE"; fi
if [ -n "${FB_ROWS:-}" ]; then APPEND="$APPEND pix68k_fb_rows=$FB_ROWS"; fi
# 画面の出し方の最後の値 (tools/qemu_crt_expect.sh の video_resolve。VMODE は video_scale の名前、VBITS は video_filter のビット)
. "$ROOT/tools/qemu_crt_expect.sh"
# 期待の計算には、pix68k.cfg に実際に書く video_filter= の効果を渡す。BEZEL だけなら video_filter=cz-600de の行があるので、
# 効果なし (none) の行として、前の scanline= に勝つ
VF_EXPECT=${VIDEO_FILTER:-}
if [ -n "${BEZEL:-}" ] && [ -z "$VF_EXPECT" ]; then VF_EXPECT=none; fi
set -- $(video_resolve "${SCALE:-}" "${CRT:-}" "${SCANLINE:-}" "$VF_EXPECT" "${VIDEO_SCALE:-}") "$@"
VMODE=$1 VBITS=$2
shift 2
if ! video_pi3_allowed "$VMODE" "$VBITS"; then
  if [ "${PI3_LIMIT:-}" = 1 ]; then
    set -- $(video_pi3_fallback "$VMODE" "$VBITS") "$@"
    VMODE=$1 VBITS=$2
    shift 2
  else
    APPEND="$APPEND pix68k_crt_all=1"
  fi
fi
if [ -n "$NETDEV" ]; then APPEND="$APPEND pix68k_net_probe=1"; fi
STREAM_OPT=
if [ "${STREAM:-}" = 1 ]; then STREAM_OPT=pix68k_stream_min=0; APPEND="$APPEND $STREAM_OPT"; fi
# shellcheck disable=SC2086
qemu-system-aarch64 -M raspi3b -kernel "$KERNEL" -append "$APPEND" \
  -serial "file:$WORK/serial.txt" -monitor stdio -display none -usb -device usb-kbd -device usb-mouse $NETDEV \
  -drive "file=$WORK/sd.img,if=sd,format=raw" \
  < "$WORK/mon" > "$WORK/qemu.out" 2> "$WORK/qemu.err" &
QPID=$!
exec 3> "$WORK/mon"
START=$(date +%s)

# ログ ($SERIAL) に $1 を含む行が $2 行目より後に出るまで待つ
SERIAL=$WORK/serial.txt
wait_for() {
  while ! tail -n "+${2:-1}" "$SERIAL" 2>/dev/null | grep -aqF -- "$1"; do
    if ! kill -0 "$QPID" 2>/dev/null; then echo "QEMU exited while waiting for: $1" >&2; return 1; fi
    if [ $(( $(date +%s) - START )) -ge "$TIMEOUT" ]; then echo "timeout waiting for: $1" >&2; return 1; fi
    sleep 1
  done
  echo "[$(( $(date +%s) - START ))s] found: $1"
}
# 正規表現 $1 に合う行が ($2 があれば $2 行目より後に) 出るまで待つ
wait_for_re() {
  while ! tail -n "+${2:-1}" "$SERIAL" 2>/dev/null | grep -aqE -- "$1"; do
    if ! kill -0 "$QPID" 2>/dev/null; then echo "QEMU exited while waiting for: $1" >&2; return 1; fi
    if [ $(( $(date +%s) - START )) -ge "$TIMEOUT" ]; then echo "timeout waiting for: $1" >&2; return 1; fi
    sleep 1
  done
  echo "[$(( $(date +%s) - START ))s] found: $1"
}
lines() { wc -l < "$SERIAL" | tr -d ' '; }

fail=0
# AUTOEXEC.BAT の「A>ECHO OFF」ではなく、何も打っていないプロンプトの行 (カーソルは ? に読める) を待つ
CR=$(printf '\r')
if wait_for 'starting the X68000 on core 1' && wait_for 'keyboard attached' && wait_for_re "text\|A>\??$CR?\$"; then
  # キーは 1 つずつ、間を空けて送る
  MARK=$(lines)
  for k in d i r ret; do
    echo "sendkey $k" >&3 2>/dev/null || true
    sleep 0.3
  done
  wait_for 'text|A>dir' "$MARK" || fail=1
  wait_for 'COMMAND            X' "$MARK" || fail=1
  # USB マウスを動かす: コア 1 が受け取った移動量の合計がログに出る
  echo "mouse_move 10 5" >&3 2>/dev/null || true
  wait_for 'x68k: mouse: moved (10,5) in total' "$MARK" || fail=1
  if [ "${HOSTNET:-}" = 1 ] && [ "${NEPTUNE:-}" != 1 ]; then
    # Host: が Pi のアドレスか net_name= でなければ断るので (DNS リバインディング対策)、URL は Pi のアドレス (10.0.2.15) にして、
    # つなぐ先だけを QEMU が転送するポートに変える (Host: 10.0.2.15 になる)
    URL=http://10.0.2.15
    LOCAL=http://127.0.0.1:$HTTP_PORT
    TOK='X-Pix68k-Token: pix68ktest'
    check() { if [ "$1" = "$2" ]; then echo "ok: $3"; else echo "NG: $3 (got $1, want $2)"; fail=1; fi; }
    # QEMU の usb-net は速くない (x86_64 の Linux の QEMU で 4MB の PUT に 5 秒ほど。#117 の前は要求がほとんど届かなかった) ので、
    # 時間の上限は長くしておく。curl が途中で失敗したら (途中で切れたなど)、
    # 状態コードの後ろに -failed を付けて NG にする
    # 本体が届かなくても、あとの cat や wc が (set -e で) 止まらないよう、空のファイルを置いてから受け取る
    code() { : > "$WORK/body"; r=$(curl -s -o "$WORK/body" -w '%{http_code}' --max-time 600 --connect-to "10.0.2.15:80:127.0.0.1:$HTTP_PORT" "$@") || r="$r-failed"; echo "$r"; }
    wait_for 'net: address 10.0.2.15' || fail=1
    # 最初の DHCP の OFFER を捨てると、4 秒後の送り直しでアドレスが決まり、この行が出る (#136。QEMU 用のビルドは
    # tools/build_baremetal.sh で Circle の udpconnection.cpp を書き換えて直している)
    if grep -aqF 'dhcp: No response from server' "$SERIAL"; then echo 'NG: DHCP retried (first OFFER dropped, #136)'; fail=1; else echo 'ok: DHCP without retry'; fi
    wait_for 'httpd: listening on port 80' || fail=1
    if [ "${RS232C:-}" = 1 ]; then
      # RS-232C: ホストからつなぎ、X68000 が AUX に書いた文字列を受け取ったら、返事と Ctrl+Z を送る
      wait_for 'rs232c: listening on port 2324' || fail=1
      python3 -I -c '
import socket, sys, time
# QEMU の slirp はホストからの接続をすぐ受け付け、ゲストとの TCP の接続が成り立たないと閉じる (#117)。
# 何も受け取らないうちに閉じられたら、つなぎ直す
got = b""
end = time.time() + 300
s = None
while time.time() < end and b"pix68kout" not in got.lower():
    if s is None:
        try:
            s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5)
            s.settimeout(1)
        except OSError:
            s = None
            time.sleep(2)
            continue
    try:
        d = s.recv(4096)
    except socket.timeout:
        continue
    except OSError:
        d = b""
    if not d:
        s.close()
        s = None
        if got:
            break
        time.sleep(2)
        continue
    got += d
open(sys.argv[2], "wb").write(got)
if s is not None and b"pix68kout" in got.lower():
    time.sleep(1)
    s.sendall(b"HELLO_FROM_HOST\r\n\x1a")
    time.sleep(5)
' "$RS232C_PORT" "$WORK/rs232c.txt" &
      RPID=$!
      wait_for 'rs232c: connected from' || fail=1
      MARK=$(lines)
      # キーは QEMU のモニタから打つ (HTTP の /type を使わず、RS-232C だけを確かめる)。
      # 「echo pix68kout>aux」「copy aux con」 (> は Shift + .)
      for k in e c h o spc p i x 6 8 k o u t shift-dot a u x ret c o p y spc a u x spc c o n ret; do
        echo "sendkey $k" >&3 2>/dev/null || true
        sleep 0.3
      done
      wait_for 'text|HELLO_FROM_HOST' "$MARK" || fail=1
      wait "$RPID" || true
      if grep -aqi 'pix68kout' "$WORK/rs232c.txt" 2>/dev/null; then echo 'ok: rs232c: received pix68kout'; else echo 'NG: rs232c: pix68kout not received'; fail=1; fi
    fi
    check "$(code "$URL/")" 200 'GET /'
    check "$(code -H 'Host: pix68k' "$URL/")" 200 'GET / with Host: net_name (pix68k)'
    check "$(code "$LOCAL/log")" 403 'GET /log with Host: 127.0.0.1 (not this address)'
    check "$(code -H 'Host: evil.example' "$URL/log")" 403 'GET /log with Host: evil.example'
    check "$(code -0 -H 'Host:' "$URL/")" 200 'GET / over HTTP/1.0 without Host:'
    # net_token= があれば、ファイルを読むのもトークンが要る (pix68k.cfg にトークンが書いてあるため)
    check "$(code "$URL/files/pix68k/pix68k.cfg")" 403 'GET pix68k.cfg without the token'
    check "$(code -H "$TOK" "$URL/files/pix68k/pix68k.cfg")" 200 'GET pix68k.cfg'
    cmp -s "$WORK/body" "$WORK/pix68k.cfg" && echo 'ok: pix68k.cfg read back' || { echo 'NG: pix68k.cfg differs'; fail=1; }
    check "$(code -H "$TOK" "$URL/files/pix68k/")" 200 'GET /files/pix68k/ (list)'
    grep -q '^IPLROM.DAT' "$WORK/body" && echo 'ok: list has IPLROM.DAT' || { echo 'NG: list'; cat "$WORK/body"; fail=1; }
    head -c 4194304 /dev/urandom > "$WORK/up.bin"
    printf 'second\n' > "$WORK/up2.txt"
    # トークンなしは小さいファイルで試す (4MB だと、断ったあとも curl が本体を送り続け、usb-net が遅いと次の要求を待たせるおそれがある。確かめてはいない)
    check "$(code -T "$WORK/up2.txt" "$URL/files/pix68k/up.bin")" 403 'PUT without the token'
    MARK=$(lines)
    T0=$(date +%s)
    check "$(code -H "$TOK" -T "$WORK/up.bin" "$URL/files/pix68k/up.bin")" 201 'PUT 4MB with the token'
    echo "  ($(( $(date +%s) - T0 )) s; $(cat "$WORK/body"))"
    tail -n "+$MARK" "$SERIAL" | grep -a 'httpd:' | sed 's/^/  /' || true
    check "$(code -H "$TOK" "$URL/files/pix68k/up.bin")" 200 'GET 4MB'
    cmp -s "$WORK/body" "$WORK/up.bin" && echo 'ok: 4MB read back' || { echo 'NG: 4MB differs'; fail=1; }
    check "$(code -H "$TOK" --data-binary @"$WORK/up2.txt" "$URL/files/pix68k/up.bin")" 201 'POST replaces the file'
    check "$(code -H "$TOK" "$URL/files/pix68k/up.bin")" 200 'GET replaced'
    cmp -s "$WORK/body" "$WORK/up2.txt" && echo 'ok: replaced content' || { echo 'NG: replaced content'; fail=1; }
    check "$(code -H "$TOK" -X DELETE "$URL/files/pix68k/up.bin")" 200 'DELETE'
    check "$(code -H "$TOK" "$URL/files/pix68k/up.bin")" 404 'GET the deleted file'
    check "$(code -H "$TOK" -X DELETE "$URL/files/pix68k/$IMAGE_FILE")" 409 'DELETE the image in use' 
    check "$(code -H "$TOK" -T "$WORK/up2.txt" "$URL/files/pix68k/$IMAGE_FILE")" 409 'PUT the image in use'
    check "$(code -H "$TOK" -T "$WORK/up2.txt" "$URL/files/PIX68K//$(echo "$IMAGE_FILE" | tr 'A-Z' 'a-z')")" 409 'PUT the image in use (other case, //)'
    check "$(code -H "$TOK" -H 'Transfer-Encoding: chunked' -T "$WORK/up2.txt" "$URL/files/pix68k/te.txt")" 400 'PUT with Transfer-Encoding'
    check "$(code -H "$TOK" -T "$WORK/up2.txt" "$URL/files/nodir/x.txt")" 404 'PUT into a missing directory'
    check "$(code -H "$TOK" --path-as-is -T "$WORK/up2.txt" "$URL/files/pix68k/../x.txt")" 400 'PUT with ..'
    check "$(code -H "$TOK" "$URL/files/pix68k/nofile")" 404 'GET a missing file'
    check "$(code "$URL/log")" 200 'GET /log'
    # 最新の 64KB だけを返すので (pix68k_textdump=1 の行で起動直後の行は押し出されることがある)、直前の要求の行を探す
    grep -aq 'httpd: 10.0.2.2 GET /files/pix68k/nofile: 404' "$WORK/body" && echo 'ok: /log has the latest lines' || { echo 'NG: /log'; fail=1; }
    check "$(code "$URL/thermal")" 200 'GET /thermal'
    grep -aq '^temp_c=' "$WORK/body" && echo "ok: /thermal ($(tr '\n' ' ' < "$WORK/body"))" || { echo 'NG: /thermal'; cat "$WORK/body"; fail=1; }
    check "$(code "$URL/screen.ppm")" 200 'GET /screen.ppm'
    head -c 15 "$WORK/body" | grep -aq 'P6' && echo "ok: screen.ppm ($(head -c 15 "$WORK/body" | tr '\n' ' '))" || { echo 'NG: screen.ppm'; fail=1; }
    # 768x512 の画面なら、ヘッダ (15 バイト) と画素 (RGB) の分だけある
    [ "$(wc -c < "$WORK/body" | tr -d ' ')" = $((15 + 768 * 512 * 3)) ] && echo 'ok: screen.ppm size' || { echo 'NG: screen.ppm size'; fail=1; }
    # 写したのが今の画面か (Human68k の文字があり、黒だけではない)。画面を写す役を合成するコアに任せている間も、
    # 古いフレーム (起動の途中の黒い画面) を返さないこと (#57)
    NONZERO=$(tail -c +16 "$WORK/body" | LC_ALL=C tr -d '\000' | wc -c | tr -d ' ')
    [ "$NONZERO" -gt 0 ] && echo "ok: screen.ppm is not all black ($NONZERO bytes not 0)" || { echo 'NG: screen.ppm is all black'; fail=1; }
    cp "$WORK/body" "$WORK/screen1.ppm"
    [ -n "${SCREEN_PPM_COPY:-}" ] && cp "$WORK/body" "$SCREEN_PPM_COPY"
    MARK=$(lines)
    check "$(code -H "$TOK" --data 'echo hostnet\n' "$URL/type")" 200 'POST /type'
    wait_for 'text|A>echo hostnet' "$MARK" || fail=1
    # echo の出力 (hostnet だけの行) まで出たら、画面はもう変わらない (カーソルの点滅を除く)
    wait_for 'text|hostnet' "$MARK" || fail=1
    # 打った文字が画面に出たあとは、すぐに取っても、打った文字が写っている (任せている間に合成したフレームを返す)。
    # 画像が変わったかだけを見ると、カーソルの点滅でも通ってしまうので、テキスト画面のダンプで打った文字の行と桁を探し、
    # その文字の升目がすべて変わり、黒だけでないことを見る (tools/screen_text_changed.py)
    check "$(code "$URL/screen.ppm")" 200 'GET /screen.ppm after /type'
    cp "$WORK/body" "$WORK/screen2.ppm"
    if R=$(python3 -I "$ROOT/tools/screen_text_changed.py" "$WORK/screen1.ppm" "$WORK/screen2.ppm" "$SERIAL" 'echo hostnet'); then
      echo "ok: screen.ppm shows the typed text ($R)"
    else
      echo "NG: screen.ppm does not show the typed text ($R)"; fail=1
    fi
    check "$(code -H "$TOK" -X POST "$URL/key?down=04")" 200 'POST /key?down=04'
    check "$(code -H "$TOK" -X POST "$URL/key?up")" 200 'POST /key?up'
    # 画面の左上のメッセージ (POST /message。platform/osd_message.h): トークン、?sec= の検査、ログの 1 行、フレームバッファに
    # 出ること (QEMU の screendump。出ている間は左上の枠が白と黒だけで、消すと変わる)、/screen.ppm には出ないこと、
    # ?sec=2 なら 2 秒で消えること。本文は 10 桁で折り返して「pix68k MSG」「1234567890」「案1 ｶﾅ」の 10 桁 x 3 行になる
    check "$(code --data 'x' "$URL/message")" 403 'POST /message without the token'
    check "$(code -H "$TOK" --data 'x' "$URL/message?sec=abc")" 400 'POST /message?sec=abc'
    MARK=$(lines)
    check "$(code -H "$TOK" --data-binary 'pix68k MSG 1234567890\n案1 ｶﾅ' "$URL/message?sec=0")" 200 'POST /message'
    wait_for 'message: "pix68k MSG 1234567890\n案1 ｶﾅ" (until cleared)' "$MARK" || fail=1
    sleep 2
    echo "screendump $WORK/msg_on.ppm" >&3 2>/dev/null || true
    sleep 1
    [ -n "${MESSAGE_PPM_COPY:-}" ] && cp "$WORK/msg_on.ppm" "$MESSAGE_PPM_COPY"
    check "$(code "$URL/screen.ppm")" 200 'GET /screen.ppm with a message'
    cp "$WORK/body" "$WORK/msg_screen.ppm"
    MARK=$(lines)
    check "$(code -H "$TOK" -X POST "$URL/message")" 200 'POST /message (clear)'
    wait_for 'message: cleared' "$MARK" || fail=1
    sleep 2
    echo "screendump $WORK/msg_off.ppm" >&3 2>/dev/null || true
    sleep 1
    MARK=$(lines)
    check "$(code -H "$TOK" --data-binary 'pix68k MSG 1234567890\n案1 ｶﾅ' "$URL/message?sec=2")" 200 'POST /message?sec=2'
    wait_for 'message: "pix68k MSG 1234567890\n案1 ｶﾅ" (2 s)' "$MARK" || fail=1
    sleep 5
    echo "screendump $WORK/msg_expired.ppm" >&3 2>/dev/null || true
    sleep 2
    if R=$(python3 -I -c '
import sys
def ppm(p):
    d = open(p, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]
w, h, on = ppm(sys.argv[1])
_, _, off = ppm(sys.argv[2])
sw, sh, scr = ppm(sys.argv[3])
_, _, exp = ppm(sys.argv[4])
# 左上 (8, 4) から 10 桁 x 3 行。1080 ライン以上なら 2 倍 (messageScale())
k = 2 if h >= 900 else 1
x0, y0 = 8 * k, 4 * k
x1, y1 = x0 + 10 * 8 * k, y0 + 3 * 16 * k
px = lambda d, x, y, w: d[(y * w + x) * 3:(y * w + x) * 3 + 3]
box = [(x, y) for y in range(y0, y1) for x in range(x0, x1)]
cols = set(px(on, x, y, w) for x, y in box)
# モニタの枠を写しているときは、背景は枠の絵を半分の明るさにしたもの (どの色も 0x7f まで)
if sys.argv[5] == "1":
    dark = all(max(c) <= 0x7f for c in cols if c != b"\xff\xff\xff")
    if b"\xff\xff\xff" not in cols or not dark or len(cols) < 2: sys.exit("box is not white on the dimmed bezel: %d colors" % len(cols))
elif not cols <= {b"\x00\x00\x00", b"\xff\xff\xff"} or len(cols) != 2: sys.exit("box is not white on black: %d colors" % len(cols))
diff = sum(px(on, x, y, w) != px(off, x, y, w) for x, y in box)
if diff == 0: sys.exit("the box did not change after clearing")
# ?sec=2 のメッセージは消えて、消したときと同じ画面に戻っている
left = sum(px(exp, x, y, w) != px(off, x, y, w) for x, y in box)
if left: sys.exit("%d pixels differ after the message expired" % left)
# /screen.ppm は X68000 の画面だけ。同じ大きさ (1 倍) なら、同じ位置が白と黒だけの枠になっていない
if (sw, sh) == (w, h) and all(px(scr, x, y, sw) == px(on, x, y, w) for x, y in box): sys.exit("/screen.ppm has the message")
print("%d pixels changed after clearing, %dx%d x%d" % (diff, w, h, k))
' "$WORK/msg_on.ppm" "$WORK/msg_off.ppm" "$WORK/msg_screen.ppm" "$WORK/msg_expired.ppm" "$BEZEL_ON"); then
      echo "ok: the message is on the framebuffer and expires ($R)"
    else
      echo "NG: the message on the framebuffer ($R)"; fail=1
    fi
    sleep 2
    grep -aq 'pix68k bare metal on' "$WORK/syslog.txt" && echo 'ok: syslog has the first lines' || { echo 'NG: syslog'; fail=1; }
    grep -aq 'up.bin replaced' "$WORK/syslog.txt" && echo 'ok: syslog has httpd lines' || { echo 'NG: syslog httpd'; fail=1; }
  fi
  if [ -n "$SCREENSHOT" ]; then
    # DIR の一覧が描き終わるまで少し待ってから撮る
    sleep 3
    echo "screendump $WORK/screen.ppm" >&3 2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do [ -s "$WORK/screen.ppm" ] && break; sleep 1; done
    sleep 1
  fi
  if [ "${WRITEBACK:-}" = 1 ]; then
    # ファイルを書き、書き込みが止んでから (2 秒) イメージのファイルに書き戻されるのを待つ
    MARK=$(lines)
    for k in e c h o spc p i x 6 8 k w b shift-dot w b dot t x t ret; do
      echo "sendkey $k" >&3 2>/dev/null || true
      sleep 0.3
    done
    wait_for 'text|A>echo pix68kwb>wb.txt' "$MARK" || fail=1
    if [ "${REBOOTKEY:-}" = 1 ]; then
      # 書き終えて次のプロンプトが出たら、書き戻しを待たずに Ctrl + Alt + Delete を送る。再起動の前に書き戻すはず
      MARK2=$(lines)
      wait_for_re "text\|A>\??$CR?\$" "$MARK2" || fail=1
      echo "sendkey ctrl-alt-delete" >&3 2>/dev/null || true
      wait_for 'restarting (Ctrl+Alt+Delete)' "$MARK2" || fail=1
      wait_for "$WRITTEN" "$MARK" || fail=1
      # 次の起動に入るのを待つ (QEMU の raspi3b はウォッチドッグの再起動を扱う)
      wait_for 'boot time: reading pix68k.cfg' "$(grep -anF 'restarting (Ctrl+Alt+Delete)' "$SERIAL" | tail -n 1 | cut -d: -f1)" || fail=1
      # 書き戻したのが Ctrl + Alt + Delete の処理か、その前のふだんの書き戻しか (どちらでも中身は確かめる)
      if awk -v w="$WRITTEN" 'index($0, "Ctrl+Alt+Delete: writing back") { c = 1 } index($0, w) && c { found = 1 } END { exit !found }' \
        "$SERIAL"; then
        echo "ok: written back by Ctrl+Alt+Delete"
      else
        echo "note: written back before Ctrl+Alt+Delete (by the periodic write-back)"
      fi
    else
      wait_for "$WRITTEN" "$MARK" || fail=1
    fi
  fi
  if [ "${MENU:-}" = 1 ]; then
    keys() { for k in "$@"; do echo "sendkey $k" >&3 2>/dev/null || true; sleep 0.3; done; }
    MARK=$(lines)
    keys scroll_lock
    wait_for 'menu: opened' "$MARK" || fail=1
    # 68000 の項目: FD0 FD1 HD0 CD-ROM CPU Clock RAM Max-speed State-slot Save-state Load-state Reset
    # Save-settings Restart Close。Clock を 10MHz にして Save settings
    keys down down down down down right end up up ret
    wait_for 'settings saved to' "$MARK" || fail=1
    if [ -n "${MENU_SHOT:-}" ]; then
      echo "screendump $WORK/menu.ppm" >&3 2>/dev/null || true
      for _ in 1 2 3 4 5; do [ -s "$WORK/menu.ppm" ] && break; sleep 1; done
      sleep 1
    fi
    keys up up up ret
    wait_for 'menu: state saved to' "$MARK" || fail=1
    keys down ret
    wait_for 'menu: state loaded from' "$MARK" || fail=1
    wait_for 'menu: closed' "$MARK" || fail=1
    # 閉じたあとはキーが X68000 に届く (X68000 が動いている)
    sleep 1
    MARK=$(lines)
    keys d i r ret
    wait_for 'text|A>dir' "$MARK" || fail=1
    # FD0 を HUMAN2.XDF に入れ替え (一覧では今の HUMAN302.XDF の 1 つ上)、リセットして、そのディスクから起動する。
    # BOOT=hd では FD0 は空で HUMAN2.XDF もないので、入れ替えずにリセットだけして、HD から起動する
    MARK=$(lines)
    keys scroll_lock
    wait_for 'menu: opened' "$MARK" || fail=1
    if [ "$BOOT" != hd ]; then
      keys ret up ret
      wait_for 'fd0: SD:/pix68k/HUMAN2.XDF' "$MARK" || fail=1
    fi
    keys end up up up ret
    wait_for 'menu: reset' "$MARK" || fail=1
    # 起動の途中で打ったキーは捨てられることがあるので、出るまで打ち直す
    n=0
    while ! tail -n "+$MARK" "$WORK/serial.txt" | grep -aqF 'text|A>ver'; do
      n=$((n + 1)); [ $n -gt 20 ] && break
      keys v e r ret
      sleep 3
    done
    wait_for 'text|A>ver' "$MARK" || fail=1
    # FD0 を取り出して設定を保存し、Restart する (pix68k.cfg を書いてから再起動する)。
    # 再起動したあとは FD0 が空のまま (fd0= を空で書いたので、既定の名前も探さない)
    MARK=$(lines)
    keys scroll_lock
    wait_for 'menu: opened' "$MARK" || fail=1
    keys ret home ret
    wait_for 'menu: fd0: ejected' "$MARK" || fail=1
    keys end up up ret
    wait_for 'settings saved to' "$MARK" || fail=1
    keys down ret
    wait_for 'restarting (settings changed in the menu)' "$MARK" || fail=1
    sleep 10
    # 再起動したあとのログ。再起動は速く (QEMU で 0.1 秒ほどで pix68k.cfg を読む)、上の wait_for が見つけたときには
    # 次の起動のログが出ていることがあるので、行数ではなく restarting の行より後を見る
    awk 'found; /restarting \(settings changed in the menu\)/ { found = 1 }' "$WORK/serial.txt" > "$WORK/after_restart.txt"
    if grep -aqF 'boot time: reading pix68k.cfg' "$WORK/after_restart.txt"; then
      echo "ok: restarted"
      if grep -aqF 'fd0: SD:' "$WORK/after_restart.txt"; then
        echo "NG: fd0 was inserted after Restart"; fail=1
      else
        echo "ok: fd0 stays empty after Restart"
      fi
      if [ "$BOOT" = hd ]; then
        if grep -aqF "$ATTACHED" "$WORK/after_restart.txt"; then
          echo "ok: $ATTACHED after Restart"
        else
          echo "NG: $ATTACHED after Restart"; fail=1
        fi
      fi
    else
      echo "note: no second boot after Restart (QEMU may not emulate the reboot)"
    fi
  fi
  # SRAM の書き戻し (5 秒に 1 回まで) が起きるかを見るため、少し動かし続ける
  sleep 6
  if [ "${HOSTNET:-}" = 1 ] && [ "${NEPTUNE:-}" != 1 ]; then
    MARK=$(lines)
    check "$(code -H "$TOK" -X POST "$URL/reboot")" 202 'POST /reboot'
    wait_for 'restarting (requested over the network)' "$MARK" || fail=1
    sleep 3
  fi
else
  fail=1
fi
echo quit >&3 2>/dev/null || true
exec 3>&-
QRC=0
wait "$QPID" || QRC=$?
QPID=

echo "---- serial log (last dump and speed) ----"
grep -av '^.*text|' "$WORK/serial.txt" | grep -a 'x68k\|pix68k' | tail -n 40 || true
# 最後のテキスト画面のダンプ
awk '/textdump begin/ { buf = "" } { buf = buf $0 "\n" } /textdump end/ { last = buf } END { printf "%s", last }' \
  "$WORK/serial.txt" | sed 's/^.*text|/|/'
echo
if [ "$QRC" -ne 0 ]; then
  echo "NG: QEMU exited with status $QRC"
  cat "$WORK/qemu.err"
  fail=1
fi
for pattern in "$ATTACHED" 'starting the X68000 on core 1' 'text|A>' 'text|A>dir' \
  'COMMAND            X' 'mouse attached' 'mouse: moved (10,5) in total'; do
  if grep -aqF -- "$pattern" "$WORK/serial.txt" 2>/dev/null; then
    echo "ok: $pattern"
  else
    echo "NG: $pattern"
    fail=1
  fi
done
# 画面と音をどのコアで合成したか
case "${RENDER_CORE:-on}/${FAIL_CORE:-}" in
  off/*) SCREEN='screen composition on core 1 (render_core=off)' ;;
  */2) SCREEN='core 2 did not start; screen composition on core 1' ;;
  *) SCREEN='screen composition on core 2' ;;
esac
# sound_core=off なら、音を出さない設定でも machine_run.cpp は sound_core=off の行を出す
if [ "${SOUND_CORE:-on}" = off ]; then
  SOUND='sound synthesis on core 1 (sound_core=off)'
elif [ "${SOUND_OUT:-null}" = off ]; then
  SOUND='no sound output; sound state on core 1'
elif [ -n "${FAIL_CORE:-}" ]; then
  SOUND='core 3 did not start; sound synthesis on core 1'
else
  SOUND='sound synthesis on core 3'
fi
set -- "$SCREEN" "$SOUND"
if [ -n "${FAIL_CORE:-}" ]; then set -- "$@" 'are treated as not started' 'not all secondary cores started'; fi
if [ "${STREAM:-}" = 1 ]; then set -- "$@" 'read on demand'; fi
if [ -n "${GPU_SCALE:-}" ]; then set -- "$@" 'by the GPU to'; fi
if [ "${FB_ROWS:-}" = 0 ]; then set -- "$@" 'pix68k_fb_rows=0; copying every row'; fi
if [ "${HOSTNET:-}" = 1 ] && [ "${NEPTUNE:-}" = 1 ]; then
  set -- "$@" 'the Ethernet is used by Neptune-X (neptune=on); the network services are off'
fi
# 768x512 の画面を拡大した大きさ (platform/frame_scale.h の computeFrameScale() と同じ計算: フレームバッファに
# まるごと入る最大の整数倍で、2 倍まで。scale=off か、768x512 より小さいフレームバッファなら 1 倍)
FB_W=${FB_SIZE%x*} FB_H=${FB_SIZE#*x}
K=$(( FB_W / 768 < FB_H / 512 ? FB_W / 768 : FB_H / 512 ))
[ "$K" -gt 2 ] && K=2
{ [ "$K" -lt 1 ] || [ "$VMODE" = off ]; } && K=1
# GPU_SCALE なら、枠は表示 (FB_SIZE) の大きさで決め、フレームバッファの中の倍率は GPU の倍率で割ったもの
GX=1 GY=1
case "${GPU_SCALE:-}" in
  '') ;;
  *x*) GX=${GPU_SCALE%x*} GY=${GPU_SCALE#*x} ;;
  *) GX=$GPU_SCALE GY=$GPU_SCALE ;;
esac
KX=$(( K / GX )) KY=$(( K / GY ))
[ "$KX" -lt 1 ] && KX=1
[ "$KY" -lt 1 ] && KY=1
# video_scale=aspect と full は整数倍でない枠 (platform/crt_effect.h の videoFrameScale())。aspect は高さいっぱい
# (1200 ラインまで) の 4:3、full はフレームバッファいっぱい (高さは 1200 ラインまで)。
# 表ができるまでの数フレームは整数倍で写すが、ログ (コア 0 が出す) に出るのは整数倍でない枠のほうだけのことがあるので、
# そちらだけを見る (GPU_SCALE なら、枠はフレームバッファ (FB_SIZE を GPU の倍率で割った大きさ) の中で決める)
SW=$(( FB_W / GX )) SH=$(( FB_H / GY ))
FILL_H=$(( SH < 1200 / GY ? SH : 1200 / GY )) FILL_W=0
if [ "$GX" = "$GY" ]; then
  case "$VMODE" in
    aspect)
      FILL_W=$(( FILL_H * 4 / 3 ))
      [ "$FILL_W" -gt "$SW" ] && FILL_W=$SW && FILL_H=$(( SW * 3 / 4 )) ;;
    full)
      FILL_W=$(( SW * FILL_H / SH )) ;;
  esac
  { [ "$FILL_W" -lt 768 ] || [ "$FILL_H" -lt 512 ]; } && FILL_W=0
fi
FILL=0
[ "$FILL_W" -gt 0 ] && FILL=1
# モニタの枠: 絵を読めて、フレームバッファが 1920x1080 なら、ガラスの矩形に写す (表を使う)
BEZEL_SHOWN=
if [ -n "${BEZEL:-}" ]; then
  case "${BEZEL_FILE:-}" in
    none) set -- "$@" "bezel: SD:/pix68k/bezel-$BEZEL.bin not found; showing without the bezel" ;;
    bad) set -- "$@" "bezel: SD:/pix68k/bezel-$BEZEL.bin: not a bezel file; showing without the bezel" ;;
    *)
      if [ "$FB_SIZE" = 1920x1080 ] && [ "$GX" = 1 ] && [ "$GY" = 1 ]; then
        set -- "$@" "bezel: SD:/pix68k/bezel-$BEZEL.bin (glass 1160x870 at 380,68)"
        BEZEL_SHOWN=1
      else
        set -- "$@" "bezel: SD:/pix68k/bezel-$BEZEL.bin is 1920x1080 but the screen is $SW"x"$SH; showing without the bezel"
      fi ;;
  esac
fi
if [ -n "$BEZEL_SHOWN" ]; then
  set -- "$@" "screen mode 768x512, shown 1160x870 (bezel $BEZEL)"
elif [ "$FB_W" -ge 768 ] && [ "$FB_H" -ge 512 ]; then
  if [ "$FILL_W" -gt 0 ]; then
    set -- "$@" "screen mode 768x512, shown ${FILL_W}x${FILL_H} (video_scale=$VMODE)"
  else
    set -- "$@" "screen mode 768x512, shown $((768 * KX))x$((512 * KY)) (x$KX, y$KY)"
  fi
fi
# video_scale=、video_filter= と前の書き方のログ (tools/qemu_crt_expect.sh。出るはずの行はここで足し、出ないはずの行は下で確かめる)
CRT_EXPECT=$(crt_log_expect "${SCALE:-}" "${CRT:-}" "${SCANLINE:-}" "$VF_EXPECT" "${VIDEO_SCALE:-}" "$KY" "$FILL" "${PI3_LIMIT:-0}")
if [ -n "${BEZEL:-}" ]; then
  # 起動時の行は、効果のあとに枠の名前 (効果がなければ枠だけ)
  CRT_EXPECT=$(echo "$CRT_EXPECT" | sed -e "s/^\(need screen: video_scale=[a-z]* video_filter=\)none\$/\1$BEZEL/" \
    -e "/^need screen: video_scale=/{/$BEZEL\$/!s/\$/,$BEZEL/;}")
  # Pi 3 で選べない組み合わせを替えた行も、両方の video_filter= に枠の名前が付く (枠は制限を変えない。platform::videoAllowedPi3())
  CRT_EXPECT=$(echo "$CRT_EXPECT" | sed -E -e "/is not available on this Pi/{s/video_filter=([a-z,]+)( is|\$)/video_filter=\1,$BEZEL\2/g;s/=none,$BEZEL/=$BEZEL/g;}")
fi
if [ -n "$BEZEL_SHOWN" ]; then
  # 枠を写すときは、効果がなくても表で写す
  CRT_EXPECT=$(echo "$CRT_EXPECT" | grep -v -e '^absent ; video_filter=' -e '^absent screen: video table:' -e '^need ; video_filter=' \
    -e '^need screen: video table:' -e '^absent ; scanlines' -e '^need ; scanlines'
    echo "need ; video_filter=$(video_filter_name "$VBITS")"
    echo 'need screen: video table: 1 built over ')
fi
CRT_ABSENT=$(echo "$CRT_EXPECT" | sed -n 's/^absent //p')
CRT_NEED=$(echo "$CRT_EXPECT" | sed -n 's/^need //p')
while IFS= read -r line; do
  [ -n "$line" ] && set -- "$@" "$line"
done <<EOF_NEED
$CRT_NEED
EOF_NEED
if [ -n "${MIDI_SF2:-}" ]; then
  set -- "$@" 'MIDI board (CZ-6BM1, YM3802) at $EAFA00' 'presets, 48 voices, '
fi
if [ -n "${MIDI_MT32_CTRL:-}" ]; then
  set -- "$@" 'MIDI board (CZ-6BM1, YM3802) at $EAFA00' 'MT-32 (Munt): '
fi
if [ "${MIDI_SC55:-}" = 1 ]; then
  set -- "$@" 'MIDI board (CZ-6BM1, YM3802) at $EAFA00' 'midi_out=sc55: the SC-55 emulation is too slow for this Pi'
fi
if [ "${NEPTUNE:-}" = 1 ]; then
  set -- "$@" 'Neptune-X at $ECE000 (MAC 02:00:00:12:34:56)' 'neptune: network device: CDC Ethernet' 'neptune: probe: ARP reply to the X68000 from 10.0.2.2'
fi
for pattern in "$@"; do
  if grep -aqF -- "$pattern" "$WORK/serial.txt" 2>/dev/null; then
    echo "ok: $pattern"
  else
    echo "NG: $pattern"
    fail=1
  fi
done
# 出ないはずの効果のログ (tools/qemu_crt_expect.sh)
while IFS= read -r pattern; do
  [ -n "$pattern" ] || continue
  if grep -aqF -- "$pattern" "$WORK/serial.txt" 2>/dev/null; then
    echo "NG: not expected: $pattern"
    fail=1
  else
    echo "ok: not expected and not found: $pattern"
  fi
done <<EOF_ABSENT
$CRT_ABSENT
EOF_ABSENT
if [ -n "${MIDI_SF2:-}" ]; then
  # SoundFont を読み終えたログが、コア 1 が動き出したログより後にあること (起動を遅らせない)
  c1=$(grep -anF 'core 1 started' "$WORK/serial.txt" | head -n 1 | cut -d: -f1)
  sf=$(grep -anF 'presets, 48 voices, ' "$WORK/serial.txt" | head -n 1 | cut -d: -f1)
  if [ -n "$c1" ] && [ -n "$sf" ] && [ "$sf" -gt "$c1" ]; then
    echo 'ok: the SoundFont is read after core 1 started'
  else
    echo "NG: the SoundFont is read after core 1 started (lines: core 1 ${c1:-none}, SoundFont ${sf:-none})"
    fail=1
  fi
  grep -aF 'presets, 48 voices, ' "$WORK/serial.txt" | head -n 1 || true
fi
if [ -n "${MIDI_MT32_CTRL:-}" ]; then
  # ROM を読んで Munt を開いたログが、コア 1 が動き出したログより後にあること
  c1=$(grep -anF 'core 1 started' "$WORK/serial.txt" | head -n 1 | cut -d: -f1)
  mt=$(grep -anF 'MT-32 (Munt): ' "$WORK/serial.txt" | head -n 1 | cut -d: -f1)
  if [ -n "$c1" ] && [ -n "$mt" ] && [ "$mt" -gt "$c1" ]; then
    echo 'ok: the MT-32 ROMs are read after core 1 started'
  else
    echo "NG: the MT-32 ROMs are read after core 1 started (lines: core 1 ${c1:-none}, MT-32 ${mt:-none})"
    fail=1
  fi
  grep -aF 'MT-32 (Munt): ' "$WORK/serial.txt" | head -n 1 || true
fi
grep -aF 'speed:' "$WORK/serial.txt" | tail -n 1 || true
grep -aF 'boot time:' "$WORK/serial.txt" || true
grep -aF 'screen: overscan' "$WORK/serial.txt" | head -n 1 || true
# 最初の 10 秒 (起動して USB の装置を探す間を含む) に、表示する前に置き換わったフレームの数 (0 になるはず)
grep -aF 'shown by the composing core' "$WORK/serial.txt" | head -n 1 || true
grep -aF 'replaced before shown' "$WORK/serial.txt" | head -n 1 || true
if [ "${MENU:-}" = 1 ]; then
  mcopy -i "$IMG" ::/pix68k/pix68k.cfg "$WORK/cfg.out"
  # BOOT=hd では fd0= は元からないので、hd0= の行が残っていることを見る
  if [ "$BOOT" = hd ]; then
    CFG_LINE='^hd0 *= *HUMAN302\.HDF *$'; CFG_WHAT='hd0 kept'
  else
    CFG_LINE='^fd0 *= *$'; CFG_WHAT='fd0 emptied'
  fi
  if grep -q '^# pix68k の設定' "$WORK/cfg.out" && grep -q '^clock=10' "$WORK/cfg.out" && grep -qE "$CFG_LINE" "$WORK/cfg.out"; then
    echo "ok: pix68k.cfg rewritten (comment kept, clock=10 added, $CFG_WHAT)"
  else
    echo "NG: pix68k.cfg"; cat "$WORK/cfg.out"; fail=1
  fi
  if [ -n "${MENU_SHOT:-}" ] && [ -s "$WORK/menu.ppm" ]; then
    sips -s format png "$WORK/menu.ppm" --out "$MENU_SHOT" >/dev/null 2>&1 && echo "menu screenshot: $MENU_SHOT"
  fi
fi

# ネットワークの口: 置き換えに使った一時ファイル (.part、.old) が残っていないこと
if [ "${HOSTNET:-}" = 1 ] && [ "${NEPTUNE:-}" != 1 ]; then
  if mdir -i "$IMG" ::/pix68k | grep -iE '\.(part|old)'; then echo "NG: a .part / .old file is left"; fail=1
  else echo "ok: no .part / .old file is left"; fi
fi

# 書き戻し: SD カードのイメージからディスクのイメージを取り出して書いた文字列を探し、もう一度起動して読む
if [ "${WRITEBACK:-}" = 1 ]; then
  rm -f "$WORK/image.out"
  if mcopy -i "$IMG" "::/pix68k/$IMAGE_FILE" "$WORK/image.out" 2>/dev/null && grep -aq pix68kwb "$WORK/image.out"; then
    echo "ok: pix68kwb is in $IMAGE_FILE on the SD card image"
  else
    echo "NG: pix68kwb is not in $IMAGE_FILE on the SD card image"; fail=1
  fi
  mdir -i "$IMG" ::/pix68k | grep -i '\.new' && { echo "NG: a .new file is left"; fail=1; }
  rm -f "$WORK/mon3" "$WORK/serial3.txt"
  mkfifo "$WORK/mon3"
  qemu-system-aarch64 -M raspi3b -kernel "$KERNEL" -append "width=768 height=512 pix68k_textdump=1 pix68k_sound=off $STREAM_OPT" \
    -serial "file:$WORK/serial3.txt" -monitor stdio -display none -usb -device usb-kbd \
    -drive "file=$WORK/sd.img,if=sd,format=raw" < "$WORK/mon3" > /dev/null 2>&1 &
  QPID=$!
  exec 3> "$WORK/mon3"
  START=$(date +%s)
  SERIAL=$WORK/serial3.txt
  if wait_for 'keyboard attached' && wait_for_re "text\|A>\??$CR?\$"; then
    n=0
    while ! grep -aqF 'text|pix68kwb' "$SERIAL"; do
      n=$((n + 1)); [ $n -gt 10 ] && break
      for k in t y p e spc w b dot t x t ret; do echo "sendkey $k" >&3 2>/dev/null || true; sleep 0.3; done
      sleep 3
    done
    if grep -aqF 'text|pix68kwb' "$SERIAL"; then echo "ok (second boot): type wb.txt shows pix68kwb"
    else echo "NG (second boot): type wb.txt"; fail=1; fi
  else
    echo "NG (second boot): no prompt"; fail=1
  fi
  echo quit >&3 2>/dev/null || true
  exec 3>&-
  wait "$QPID" 2>/dev/null || true
  QPID=
  SERIAL=$WORK/serial.txt
fi

# 2 回目の起動: 1 回目に書き戻した SRAM (最後に書いた世代) を読むかを見る
LAST=$(grep -aoE 'SRAM saved to SD:/pix68k/SRAM\.(DAT|BAK) \(generation [0-9]+\)' "$WORK/serial.txt" | tail -n 1 || true)
if [ -z "$LAST" ]; then
  echo "NG: SRAM was not saved"
  fail=1
else
  echo "ok: $LAST"
  WANT="SRAM: ${LAST#SRAM saved to }"
  qemu-system-aarch64 -M raspi3b -kernel "$KERNEL" -append "width=768 height=512" \
    -serial "file:$WORK/serial2.txt" -monitor none -display none -usb -device usb-kbd \
    -drive "file=$WORK/sd.img,if=sd,format=raw" > /dev/null 2>&1 &
  QPID=$!
  START=$(date +%s)
  found=0
  while [ $(( $(date +%s) - START )) -lt 60 ] && kill -0 "$QPID" 2>/dev/null; do
    if grep -aqF -- "$WANT" "$WORK/serial2.txt" 2>/dev/null; then found=1; break; fi
    sleep 1
  done
  kill "$QPID" 2>/dev/null || true
  wait "$QPID" 2>/dev/null || true
  QPID=
  if [ "$found" = 1 ]; then
    echo "ok (second boot): $WANT"
  else
    echo "NG (second boot): $WANT"
    grep -a 'SRAM' "$WORK/serial2.txt" || true
    fail=1
  fi
fi
if [ -n "$SCREENSHOT" ]; then
  if [ -s "$WORK/screen.ppm" ] && case "$SCREENSHOT" in *.ppm) cp "$WORK/screen.ppm" "$SCREENSHOT" ;; *) false ;; esac; then
    echo "screenshot: $SCREENSHOT"  # .ppm ならそのまま写す (sips のない Linux でも取れる)
  elif [ -s "$WORK/screen.ppm" ] && sips -s format png "$WORK/screen.ppm" --out "$SCREENSHOT" >/dev/null 2>&1; then
    echo "screenshot: $SCREENSHOT"
  else
    echo "warning: cannot save the screenshot"
  fi
fi
exit $fail
