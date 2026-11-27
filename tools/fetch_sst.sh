#!/bin/sh
# SingleStepTests/680x0 の 68000 テストを取得し、テストランナー用のバイナリ形式に変換する。
# データはリポジトリに入れず、キャッシュディレクトリに置く。
#   使い方: tools/fetch_sst.sh [出力先 (既定: ~/.cache/pix68k/sst)]
# 取る版とファイルの一覧は tools/sst_files.txt にある (作り直し方は tools/make_sst_list.sh)。api.github.com は、共有の CI の機械で
# 認証なしの回数の上限に当たって 403 になるので使わず、raw.githubusercontent.com だけから取る (#333)。
# 一覧のファイルが 1 つでもそろわなければ失敗する。
set -eu
OUT=${1:-$HOME/.cache/pix68k/sst}
HERE=$(cd "$(dirname "$0")" && pwd)
LIST=$HERE/sst_files.txt
SHA=$(sed -n 's/^commit //p' "$LIST")
COUNT=$(sed -n 's/^count //p' "$LIST")
# 一覧が途中で切れていたり、書き換わっていたりしないか
FILES=$(grep -E '^[0-9a-f]{40} [A-Za-z0-9._-]+\.json\.gz$' "$LIST" || true)
NAMES=$(echo "$FILES" | cut -d ' ' -f 2)
ENTRIES=$(grep -Evc '^(#|commit [0-9a-f]{40}$|count [0-9]+$)' "$LIST" || true)
UNIQUE=$(echo "$NAMES" | sort -u | wc -l | tr -d ' ')
if [ "${#SHA}" -ne 40 ] || [ "$ENTRIES" != "$COUNT" ] || [ "$UNIQUE" != "$COUNT" ] ||
   [ "$(echo "$NAMES" | wc -l | tr -d ' ')" != "$COUNT" ]; then
  echo "$LIST: broken list (count $COUNT, $ENTRIES lines, $UNIQUE names)" >&2
  exit 1
fi
# 同じ機械のいくつかの runner が同じ HOME で同時に動いても壊れないよう、ファイルは $OUT/tmp の自分だけのディレクトリに書いてから、mv で置く
# (書きかけのファイルを、ほかの実行が読まない。途中で止められても、書きかけが残らない)
mkdir -p "$OUT/json" "$OUT/bin" "$OUT/tmp"
TMP=$(mktemp -d "$OUT/tmp/XXXXXX")
trap 'rm -rf "$TMP"' EXIT
trap 'exit 1' INT TERM HUP
# ファイルの並び (標準入力の「blob の SHA-1 名前」) のうち、ディレクトリ $1 にあって中身が一覧と違うものの名前を出す
differing() {
  python3 -I -c 'import hashlib,os,sys
for line in sys.stdin:
    want, name = line.split()
    try:
        with open(os.path.join(sys.argv[1], name), "rb") as f:
            d = f.read()
    except FileNotFoundError:
        continue
    if hashlib.sha1(b"blob %d\0" % len(d) + d).hexdigest() != want:
        print(name)' "$1" | tr -d '\r'
}
# 今あるファイルのうち、壊れているか別の版のもの。ほかの実行が使っているかもしれないので、消さずに、取り直せてから置き換える
BAD=$(echo "$FILES" | differing "$OUT/json")
echo "$FILES" | while read -r sha n; do
  j=$OUT/json/$n
  b=$OUT/bin/${n%.json.gz}.sst
  if [ ! -s "$j" ] || echo "$BAD" | grep -qxF "$n"; then
    curl -fsSL --retry 3 -o "$TMP/$n" "https://raw.githubusercontent.com/SingleStepTests/680x0/$SHA/68000/v1/$n"
    if [ -n "$(echo "$sha $n" | differing "$TMP")" ]; then
      echo "$n: the downloaded file is not the one in $LIST" >&2
      exit 1
    fi
    # 変換したものを先に置く (間で止まっても、次に取り直す)
    python3 -I "$HERE/sst2bin.py" "$TMP/$n" "$TMP/sst"
    mv -f "$TMP/sst" "$b"
    mv -f "$TMP/$n" "$j"
  elif [ ! -s "$b" ]; then
    python3 -I "$HERE/sst2bin.py" "$j" "$TMP/sst"
    mv -f "$TMP/sst" "$b"
  fi
done
# 一覧のものがすべてそろったか。一覧にないもの (前の版の残りなど) は、ctest が一緒に動かすので知らせる
have=0
for n in $NAMES; do
  if [ -s "$OUT/json/$n" ] && [ -s "$OUT/bin/${n%.json.gz}.sst" ]; then have=$((have + 1)); else echo "missing: $n" >&2; fi
done
for b in "$OUT"/bin/*.sst; do
  n=$(basename "$b" .sst).json.gz
  echo "$NAMES" | grep -qxF "$n" || echo "warning: $b is not in the list" >&2
done
if [ "$have" != "$COUNT" ]; then
  echo "only $have of $COUNT tests" >&2
  exit 1
fi
echo "$have tests in $OUT/bin"
