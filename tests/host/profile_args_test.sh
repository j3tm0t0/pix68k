#!/bin/sh
# --profile の引数の読み方 (#231)。ROM もディスクも要らない (ROM の場所を空のディレクトリにして、テストパターンで終わる)
#   - ほかのオプションの値の "--profile" (--type "--profile") を --profile と取り違えない (--config がないと断られていた)
#   - --config の隣に pix68k-<名前>.cfg がなければ、終了コード 2 で終わる
#   - あれば受け付けて、テストパターンを出して終わる
# 使い方: profile_args_test.sh <pix68k>
set -u
BIN=$1
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
ng=0
# 調べる引数を先に置く (後ろにほかの引数がある形。前の作りは --profile の次の引数を値として拾った)
run() { "$BIN" "$@" -r "$WORK/rom" --frames 1 --no-sram --no-audio > "$WORK/out.txt" 2>&1; }
mkdir -p "$WORK/rom"
run --type "--profile"
rc=$?
if [ $rc != 0 ] || ! grep -q "showing the test pattern" "$WORK/out.txt"; then
    cat "$WORK/out.txt"; echo "NG: --type \"--profile\" was read as --profile (exit $rc)"; ng=1
fi
printf 'osd = off\n' > "$WORK/pix68k.cfg"
run --config "$WORK/pix68k.cfg" --profile nothere
rc=$?
if [ $rc != 2 ]; then cat "$WORK/out.txt"; echo "NG: --profile with no file: exit $rc (want 2)"; ng=1; fi
printf '# test\nscale = off\n' > "$WORK/pix68k-t1.cfg"
run --config "$WORK/pix68k.cfg" --profile t1 --type "--profile"
rc=$?
if [ $rc != 0 ] || ! grep -q "showing the test pattern" "$WORK/out.txt"; then
    cat "$WORK/out.txt"; echo "NG: --profile t1: exit $rc"; ng=1
fi
[ $ng = 0 ] && echo "OK: --profile arguments"
exit $ng
