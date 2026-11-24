#!/bin/sh
# tools/sst_files.txt (fetch_sst.sh が取るファイルの一覧) を作り直す。SingleStepTests/680x0 の版を変えるときだけ使う。
#   使い方: tools/make_sst_list.sh <コミットの SHA (40 桁)> > tools/sst_files.txt.new && mv tools/sst_files.txt.new tools/sst_files.txt
# 中身 (blob) は取らず、木だけを git で取る (api.github.com は使わない)。
# 確かめ方: api.github.com なしで動き、今の SHA で作ると、今の tools/sst_files.txt と同じになる。
#   tools/make_sst_list.sh "$(sed -n 's/^commit //p' tools/sst_files.txt)" | diff - tools/sst_files.txt
# 版を変えると、fetch_sst.sh は中身の変わったファイルだけを取り直す。一覧からなくなったものは残るので (警告が出る)、
# ~/.cache/pix68k/sst から消す。CI の runner の機械も同じ。
# 違う版の一覧を持つ 2 つの作業ツリーで、同じ出力先を使わない (同時に動かしたり、版を変える途中で止めて前の版に戻したりすると、
# 変換したものが別の版になることがある)。版を試すときは、fetch_sst.sh に別の出力先を渡し、cmake に -DSST_DIR=<出力先>/bin を渡す。
set -eu
SHA=${1:?usage: make_sst_list.sh <commit>}
echo "$SHA" | grep -Eq '^[0-9a-f]{40}$' || { echo "not a 40-digit commit: $SHA" >&2; exit 1; }
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
trap 'exit 1' INT TERM HUP
git init -q --bare "$DIR"
git -C "$DIR" fetch -q --depth 1 --filter=blob:none https://github.com/SingleStepTests/680x0.git "$SHA"
git -C "$DIR" ls-tree "$SHA:68000/v1" > "$DIR/tree"
# ls-tree の行は「モード 種類 SHA<タブ>名前」
awk -v sha="$SHA" -F '[ \t]' '
  $2 == "blob" && $4 ~ /\.json\.gz$/ { line[n++] = $3 " " $4 }
  END {
    if (n == 0) { print "no files" > "/dev/stderr"; exit 1 }
    print "# SingleStepTests/680x0 の 68000/v1 のファイル (tools/make_sst_list.sh で作る。手で書き換えない)"
    print "# commit: 取る版。count: 下の行の数。下の行は、git の blob の SHA-1 と名前"
    print "commit " sha
    print "count " n
    for (i = 0; i < n; i++) print line[i]
  }' "$DIR/tree"
