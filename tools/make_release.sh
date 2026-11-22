#!/bin/sh
# 配る zip (SD カードのルートに展開して使うもの) を作る。
#   使い方: tools/make_release.sh [--allow-dirty] [版の名前]
#   版の名前の既定は git describe --tags --always。出力は build/release/pix68k-<版の名前>.zip
#   作業ツリー (submodule を含む) にコミットしていない変更があれば失敗する。--allow-dirty を付けると試しに作れる
#   (SOURCE.txt に「試しに作ったもの」と書く。配らない)
#   配る zip は、公開リポジトリ (j3tm0t0/pix68k) の CI (.github/workflows/release.yml) で作る。origin がそれでなければ、
#   SOURCE.txt に書くコミットが公開リポジトリにないので、警告を出して「試し」として作る
# ベアメタル版を Raspberry Pi 2 / 3 / 4 (400) / 5 の分ビルドし (tools/build_baremetal.sh)、Raspberry Pi のファームウェア
# (Circle の boot/Makefile と同じ版。device tree は pix68k が対応する機種のものだけ)、config.txt、cmdline.txt、空の pix68k/ (ROM の置き方の README.txt)、
# LICENSE、NOTICE、同梱のライブラリのライセンス、ソースの場所を書いた SOURCE.txt を入れる。
# モニタの枠の絵 (video_filter= の cz-600de / cz-600db) は、tools/bezel_cz600d.py で描いて tools/bezel_pack.py で .bin にし、
# pix68k/ に入れる (絵と .bin はリポジトリに入れない)。
# IPL ROM、CGROM、ディスクのイメージ、SoundFont、MIDI の音源の ROM は入れない。最後に zip の中の一覧を調べ、
# 決めた名前 (完全一致) 以外のファイルか、ROM やディスクのイメージに見える名前があれば zip を消して失敗する。
# 要るもの: tools/build_baremetal.sh が要るもの (aarch64-elf-gcc か PREFIX64 のコンパイラ、Pi 2 版の arm-none-eabi-gcc か
# PREFIX32 のコンパイラ、rsync)、curl、zip、unzip、
# shasum か sha256sum、python3 と numpy と Pillow (枠の絵)。ソースの URL は PIX68K_SOURCE_URL で変えられる (既定 https://github.com/j3tm0t0/pix68k)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
ALLOW_DIRTY=
if [ "${1:-}" = --allow-dirty ]; then ALLOW_DIRTY=yes; shift; fi
[ $# -le 1 ] || { echo "usage: $0 [--allow-dirty] [版の名前]" >&2; exit 2; }
VERSION=${1:-$(git describe --tags --always)}
case $VERSION in
  ''|*/*|*' '*) echo "版の名前が正しくない: '$VERSION'" >&2; exit 2 ;;
esac
SOURCE_URL=${PIX68K_SOURCE_URL:-https://github.com/j3tm0t0/pix68k}
COMMIT=$(git rev-parse HEAD)
CIRCLE=$ROOT/external/circle
[ -f "$CIRCLE/boot/Makefile" ] || { echo "external/circle がない (git submodule update --init)" >&2; exit 1; }
# 作業ツリーが clean か (追跡していないファイルも、submodule の中の変更も見る。build/ は .gitignore で外れる)
DIRTY=
[ -z "$(git status --porcelain --ignore-submodules=none)" ] || DIRTY=yes
[ -z "$(git -C "$CIRCLE" status --porcelain)" ] || DIRTY=yes
if [ -n "$DIRTY" ] && [ -z "$ALLOW_DIRTY" ]; then
  git status --short >&2
  echo "コミットしていない変更がある。配る zip は clean な作業ツリーから作る (試しなら --allow-dirty)" >&2
  exit 1
fi
# origin が公開リポジトリでなければ、SOURCE.txt のコミットは公開側にないので「試し」として作る
NOT_PUBLIC=
ORIGIN=$(git remote get-url origin 2>/dev/null || true)
if ! echo "$ORIGIN" | grep -Eq '[:/]j3tm0t0/pix68k(\.git)?/?$'; then
  echo "警告: origin ($ORIGIN) は公開リポジトリ (j3tm0t0/pix68k) ではない。SOURCE.txt のコミットは公開リポジトリにないので、試しとして作る" >&2
  NOT_PUBLIC=yes
fi
# HEAD を指すタグに版の名前があるときだけ、SOURCE.txt にタグと書く
TAG=
if git tag --points-at HEAD | grep -qxF "$VERSION"; then TAG=$VERSION; fi
if command -v sha256sum >/dev/null 2>&1; then SHA256="sha256sum"; else SHA256="shasum -a 256"; fi
OUT=$ROOT/build/release
NAME=pix68k-$VERSION
STAGE=$OUT/$NAME
ZIP=$OUT/$NAME.zip
mkdir -p "$OUT"
rm -rf "$STAGE" "$ZIP"

# 1. ベアメタル版のビルド
for n in 2 3 4 5; do
  tools/build_baremetal.sh "$n"
done
mkdir -p "$STAGE/pix68k" "$STAGE/overlays" "$STAGE/licenses"
cp build/baremetal-raspi2/app/kernel7.img "$STAGE/"
cp build/baremetal-raspi3/app/kernel8.img "$STAGE/"
cp build/baremetal-raspi4/app/kernel8-rpi4.img "$STAGE/"
cp build/baremetal-raspi5/app/kernel_2712.img "$STAGE/"
# モニタの枠の絵 (グレーの cz-600de と黒の cz-600db)
BEZEL_PNG=$OUT/bezel-png
rm -rf "$BEZEL_PNG"
mkdir -p "$BEZEL_PNG"
python3 tools/bezel_cz600d.py "$BEZEL_PNG"
for b in cz-600de cz-600db; do
  python3 tools/bezel_pack.py "$BEZEL_PNG/bezel-$b.png" "$STAGE/pix68k/bezel-$b.bin"
done
rm -rf "$BEZEL_PNG"

# 2. ファームウェア。版とファイルの一覧は Circle の boot/Makefile から読む (Circle を上げれば一緒に変わる)。
#    32 ビットの armstub やブートローダは使わないので入れない。取ってきたものは build/release/firmware-<版> に残して使い回す
PRINT=$OUT/print-firmware.mk
printf 'print-firmware:\n\t@echo $(FIRMWARE)\n\t@echo $(FILES)\n\t@echo $(OVERLAYS)\n' > "$PRINT"
VARS=$(make -s -C "$CIRCLE/boot" -f Makefile -f "$PRINT" print-firmware)
rm -f "$PRINT"
FW_REV=$(echo "$VARS" | sed -n 1p)
FW_FILES=$(echo "$VARS" | sed -n 2p)
FW_OVERLAYS=$(echo "$VARS" | sed -n 3p)
[ -n "$FW_REV" ] && [ -n "$FW_FILES" ] || { echo "Circle の boot/Makefile からファームウェアの一覧を読めない" >&2; exit 1; }
case $FW_REV in
  *[!0-9a-f]*|'') echo "ファームウェアの版がコミットの 16 進でない: '$FW_REV'" >&2; exit 1 ;;
esac
# device tree は、pix68k が対応する機種 (Raspberry Pi 4 Model B、400、5) のものだけを入れる。Circle の一覧にある
# Zero 2 W、CM0、CM4、CM5、Raspberry Pi 500 のものは入れない (Raspberry Pi 2 と Raspberry Pi 3 は device tree を使わない。
# Pi 2 Model B は、device tree を置かずに起動することを実機で確かめた。#302)
KEEP_DTB="bcm2711-rpi-4-b.dtb bcm2711-rpi-400.dtb bcm2712-rpi-5-b.dtb bcm2712d0-rpi-5-b.dtb"
FILES=
for f in $FW_FILES; do
  case $f in
    *.dtb) case " $KEEP_DTB " in *" $f "*) ;; *) continue ;; esac ;;
  esac
  FILES="$FILES $f"
done
FW_FILES=${FILES# }
for f in $KEEP_DTB; do
  case " $FW_FILES " in *" $f "*) ;; *) echo "Circle の一覧に $f がない" >&2; exit 1 ;; esac
done
FW_DIR=$OUT/firmware-$FW_REV
FW_URL=https://github.com/raspberrypi/firmware/raw/$FW_REV
mkdir -p "$FW_DIR/overlays" "$FW_DIR/extra"
# 取ってきたファイルの SHA-256 を $FW_DIR/SHA256SUMS に記録し、キャッシュを使うときは記録と照合する
SUMS=$FW_DIR/SHA256SUMS
touch "$SUMS"
fetch() { # fetch <URL> <保存先 ($FW_DIR の中)>
  rel=${2#"$FW_DIR"/}
  if [ -s "$2" ]; then
    want=$(awk -v f="$rel" '$2 == f { print $1 }' "$SUMS")
    have=$($SHA256 "$2" | awk '{ print $1 }')
    [ -n "$have" ] || { echo "$rel の SHA-256 を計算できない" >&2; exit 1; }
    if [ -n "$want" ] && [ "$want" = "$have" ]; then return 0; fi
    echo "キャッシュの $rel が記録と合わない (か、記録がない) ので取り直す" >&2
    rm -f "$2"
  fi
  echo "ダウンロード: $1"
  curl -fsSL --retry 3 -o "$2.part" "$1"
  mv "$2.part" "$2"
  have=$($SHA256 "$2" | awk '{ print $1 }')
  [ -n "$have" ] || { echo "$rel の SHA-256 を計算できない" >&2; exit 1; }
  awk -v f="$rel" '$2 != f' "$SUMS" > "$SUMS.tmp"
  echo "$have  $rel" >> "$SUMS.tmp"
  mv "$SUMS.tmp" "$SUMS"
}
for f in $FW_FILES; do
  fetch "$FW_URL/boot/$f" "$FW_DIR/$f"
  cp "$FW_DIR/$f" "$STAGE/$f"
done
for f in $FW_OVERLAYS; do
  fetch "$FW_URL/boot/overlays/$f" "$FW_DIR/overlays/$f"
  cp "$FW_DIR/overlays/$f" "$STAGE/overlays/$f"
done
# device tree のもとの Linux のコミット (ファームウェアのリポジトリの extra/git_hash)
fetch "$FW_URL/extra/git_hash" "$FW_DIR/extra/git_hash"
LINUX_HASH=$(tr -d ' \r\n' < "$FW_DIR/extra/git_hash")
if ! echo "$LINUX_HASH" | grep -Eqx '[0-9a-f]{40}'; then
  echo "extra/git_hash が 40 桁の 16 進でない: '$LINUX_HASH'" >&2; exit 1
fi

# 3. config.txt。Circle の boot/config64.txt をもとに、pix68k に合わせて次を変えたもの:
#    [pi4] の armstub の行を消す (FIQ を使うときだけ要る)。[pi5] の framebuffer の 2 行を有効にする (ないと Pi 5 で画面を作れない)。
#    pix68k が動かない機種 (Zero 2 W) の節は入れない。
#    [pi2] は 32 ビットのカーネル (kernel7.img、0x8000 に読む)。Pi 2 のカーネルは、1920x1080 ではフレームバッファを 960x540 にして
#    2 倍の拡大を GPU に任せる (baremetal/kernel.cpp の InitScreen()。#302)。scaling_kernel=8 は、その拡大を最近傍にする
#    (実機のモニタで、文字の輪郭がにじまないことを確かめた)。disable_overscan=1 がないと、Pi 2 のファームウェアは画面の縁を
#    48 画素ずつ空け、表示の大きさが 2 で割り切れても 768x512 が入らなくなる
cat > "$STAGE/config.txt" <<'EOF'
# pix68k の config.txt (Circle の boot/config64.txt をもとにしたもの)
arm_64bit=1
kernel_address=0x80000
initial_turbo=0

# Raspberry Pi 2 Model B (32 ビット。X68000 の 68000 の機種だけ)
[pi2]
arm_64bit=0
kernel_address=0x8000
kernel=kernel7.img
disable_overscan=1
scaling_kernel=8

[pi3]
kernel=kernel8.img

[pi3+]
kernel=kernel8.img

# Raspberry Pi 4 Model B と Raspberry Pi 400
[pi4]
kernel=kernel8-rpi4.img
max_framebuffers=2

[pi5]
kernel=kernel_2712.img
framebuffer_depth=32
framebuffer_ignore_alpha=1

[all]
EOF

# 4. cmdline.txt (1 行。オプションは README の「cmdline.txt」と docs/config-reference.md)
echo "pix68k_sound=hdmi" > "$STAGE/cmdline.txt"

# 5. ROM の置き方
cat > "$STAGE/pix68k/README.txt" <<'EOF'
pix68k: この pix68k フォルダに置くもの
=====================================

IPL ROM、CGROM、ディスクのイメージは入っていない。自分で用意して、このフォルダに置く。

  IPLROM.DAT      X68000 の IPL ROM (128KB)。なければ X68000 は動かない
  CGROM.DAT       CGROM (768KB)。なければ文字が出ない
  IPLROM30.DAT    X68030 と 060turbo を動かすときの IPL ROM (128KB)。Raspberry Pi 2 では X68030 と 060turbo は動かない
  IPLROM060.DAT   060turbo の IPL ROM (なくてよい)
  fd0.xdf         FD0 に入れるフロッピーディスクのイメージ (.dim、.d88 も可)。fd1.xdf は FD1
  hd0.hdf         SASI ID 0 のハードディスクのイメージ
  pix68k.cfg      設定 (なくてよい)

このフォルダにはじめから入っているもの:

  bezel-cz-600de.bin  モニタの枠の絵 (グレー)。pix68k.cfg に video_filter = scanline,curve,cz-600de のように書くと使う
  bezel-cz-600db.bin  同じく黒の枠 (cz-600db)。どちらも画面が 1920x1080 のときだけ出る

pix68k.cfg の例 (1 行に 1 つ「キー=値」。# から行末までは注釈):

  fd0 = HUMAN302.XDF
  cpu = 68000
  clock = 16

キーの一覧と詳しい説明: https://github.com/j3tm0t0/pix68k の README.md と docs/config-reference.md
SRAM (SRAM.DAT、SRAM.BAK) とステート (state0.p68 など) も、pix68k がこのフォルダに書く。
EOF

# 6. ライセンスとソースの場所
cp LICENSE NOTICE "$STAGE/"
cp external/munt/mt32emu/COPYING.LESSER.txt "$STAGE/licenses/mt32emu-COPYING.LESSER.txt"
cp external/nuked-sc55/LICENSE "$STAGE/licenses/nuked-sc55-LICENSE.txt"
cp external/tinysoundfont/LICENSE "$STAGE/licenses/tinysoundfont-LICENSE.txt"
CIRCLE_COMMIT=$(git -C "$CIRCLE" rev-parse HEAD)
# SHA-256 は先にファイルに書く (失敗すれば set -e で止まる)。中身は zip に入る名前で書く
SUMS_OUT=$OUT/sha256-$NAME.txt
: > "$SUMS_OUT"
for f in $FW_FILES; do (cd "$STAGE" && $SHA256 "$f") >> "$SUMS_OUT"; done
for f in $FW_OVERLAYS; do (cd "$STAGE" && $SHA256 "overlays/$f") >> "$SUMS_OUT"; done
[ "$(wc -l < "$SUMS_OUT")" -eq $(( $(echo $FW_FILES | wc -w) + $(echo $FW_OVERLAYS | wc -w) )) ] || { echo "SHA-256 の数が合わない" >&2; exit 1; }
if [ -n "$TAG" ]; then WHERE="タグ ${TAG} (コミット ${COMMIT})"; else WHERE="コミット ${COMMIT}"; fi
{
  echo "pix68k $VERSION"
  if [ -n "$DIRTY" ]; then
    echo
    echo "これは試しに作ったもの (コミットしていない変更を含む作業ツリーからビルドした)。配らないこと。"
  fi
  if [ -n "$NOT_PUBLIC" ]; then
    echo
    echo "これは試しに作ったもの (公開リポジトリでないところでビルドしたので、下のコミットは公開リポジトリにない)。配らないこと。"
  fi
  echo
  echo "このパッケージのカーネル (kernel7.img、kernel8.img、kernel8-rpi4.img、kernel_2712.img) は GPLv3 で配布する。"
  echo "同梱のライブラリのライセンスは NOTICE と licenses/ にある。カーネルを作ったソース一式は次の場所にある。"
  echo
  echo "  pix68k のソース: ${SOURCE_URL} (${WHERE})"
  echo "  Circle: https://github.com/rsta2/circle のコミット ${CIRCLE_COMMIT} (pix68k のリポジトリの external/circle)"
  echo "  ビルドのとき、tools/build_baremetal.sh は Circle を写した先の addon/fatfs/ffconf.h と include/circle/timer.h を書き換える"
  echo "  (Circle のリポジトリの中身は変えない。書き換えの中身は tools/build_baremetal.sh にある)。"
  echo
  echo "pix68k/bezel-cz-600de.bin と bezel-cz-600db.bin (モニタの枠の絵) は、ソースの tools/bezel_cz600d.py で描き、"
  echo "tools/bezel_pack.py で作ったもの (カーネルと同じく GPLv3)。numpy と Pillow の版が違うと、作り直したときに"
  echo "数画素の色が 1 だけ違うことがある。"
  echo
  echo "Munt (libmt32emu) を差し替えてリンクし直すには、ソースの external/munt/mt32emu を別の版に替え、"
  echo "tools/build_baremetal.sh でビルドし直す。"
  echo
  echo "Raspberry Pi のファームウェア (bootcode.bin、start*.elf、fixup*.dat) は Broadcom と Raspberry Pi (Trading) Ltd のもので、"
  echo "LICENCE.broadcom の条件で配布する。device tree (*.dtb、overlays/*.dtbo) は COPYING.linux (GPLv2) の条件で配布する。"
  echo "  取得元: https://github.com/raspberrypi/firmware のコミット ${FW_REV} の boot/"
  echo "  device tree のソース: https://github.com/raspberrypi/linux のコミット ${LINUX_HASH} の"
  echo "  arch/arm/boot/dts と arch/arm64/boot/dts/broadcom (コミットはファームウェアのリポジトリの extra/git_hash による)"
  echo
  echo "ファームウェアと device tree の SHA-256:"
  sed 's/^/  /' "$SUMS_OUT"
} > "$STAGE/SOURCE.txt"
rm -f "$SUMS_OUT"

# 7. zip にする (展開した中身を、そのまま SD カードのルートに置く形)
(cd "$STAGE" && zip -q -r -X "$ZIP" .)

# 8. 中身を調べる: 決めた名前 (完全一致) 以外のファイルと、ROM やディスクのイメージに見える名前があれば失敗する
ALLOWED="kernel7.img kernel8.img kernel8-rpi4.img kernel_2712.img config.txt cmdline.txt LICENSE NOTICE SOURCE.txt
pix68k/ pix68k/README.txt pix68k/bezel-cz-600de.bin pix68k/bezel-cz-600db.bin overlays/ licenses/ licenses/mt32emu-COPYING.LESSER.txt licenses/nuked-sc55-LICENSE.txt
licenses/tinysoundfont-LICENSE.txt $FW_FILES"
for f in $FW_OVERLAYS; do ALLOWED="$ALLOWED overlays/$f"; done
REQUIRED="kernel7.img kernel8.img kernel8-rpi4.img kernel_2712.img config.txt cmdline.txt LICENSE NOTICE SOURCE.txt pix68k/README.txt
pix68k/bezel-cz-600de.bin pix68k/bezel-cz-600db.bin
LICENCE.broadcom COPYING.linux bootcode.bin start.elf start4.elf fixup.dat fixup4.dat
$KEEP_DTB"
LIST=$(unzip -Z1 "$ZIP")
BAD=
OLDIFS=$IFS
IFS='
'
for e in $LIST; do
  ok=
  for a in $(echo "$ALLOWED" | tr ' ' '\n'); do
    [ "$e" = "$a" ] && { ok=yes; break; }
  done
  [ -n "$ok" ] || BAD="$BAD $e"
  lower=$(echo "$e" | tr 'A-Z' 'a-z')
  case $lower in
    *iplrom*|*cgrom*|*scsiexrom*|*scsiinrom*|*rom30*|*sram*|*.rom|*.sf2|*.sf3|*.xdf|*.dim|*.d88|*.hdm|*.2hd|*.hdf|*.hds|*.iso|*.cue|*.p68|*control*|*pcm*|*.wav)
      BAD="$BAD $e" ;;
    *.dat) case $lower in fixup*.dat) ;; *) BAD="$BAD $e" ;; esac ;;
  esac
done
for f in $(echo "$REQUIRED" | tr ' ' '\n'); do
  echo "$LIST" | grep -qxF "$f" || BAD="$BAD (ない: $f)"
done
IFS=$OLDIFS
if [ -n "$BAD" ]; then
  echo "zip の中身が正しくない:$BAD" >&2
  rm -f "$ZIP"
  exit 1
fi
unzip -l "$ZIP"
echo "できた: $ZIP"
