#!/bin/sh
# SingleStepTests/680x0 の 68000 テストを取得し、テストランナー用のバイナリ形式に変換する。
# データはリポジトリに入れず、キャッシュディレクトリに置く。
#   使い方: tools/fetch_sst.sh [出力先 (既定: ~/.cache/pix68k/sst)]
set -eu
SHA=e0d5ece9670205cc84a0101081837deb446f86a3
OUT=${1:-$HOME/.cache/pix68k/sst}
HERE=$(cd "$(dirname "$0")" && pwd)
# 同じ機械のいくつかの runner が同じ HOME で同時に動いても壊れないよう、ファイルは $OUT/tmp の自分だけの名前に書いてから、mv で置く
# (書きかけのファイルを、ほかの実行が読まない。途中で止められても、書きかけが残らない)
mkdir -p "$OUT/json" "$OUT/bin" "$OUT/tmp"
TMP=$OUT/tmp/$$
trap 'rm -f "$TMP"' EXIT
trap 'exit 1' INT TERM HUP
NAMES=$(curl -fsSL "https://api.github.com/repos/SingleStepTests/680x0/contents/68000/v1?ref=$SHA" \
  | python3 -I -c 'import json,sys; print("\n".join(e["name"] for e in json.load(sys.stdin) if e["name"].endswith(".json.gz")))')
for n in $NAMES; do
  if [ ! -s "$OUT/json/$n" ]; then
    curl -fsSL -o "$TMP" "https://raw.githubusercontent.com/SingleStepTests/680x0/$SHA/68000/v1/$n"
    mv -f "$TMP" "$OUT/json/$n"
  fi
  b="$OUT/bin/${n%.json.gz}.sst"
  if [ ! -s "$b" ]; then
    python3 -I "$HERE/sst2bin.py" "$OUT/json/$n" "$TMP"
    mv -f "$TMP" "$b"
  fi
done
echo "tests in $OUT/bin"
