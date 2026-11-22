#!/bin/sh
# tools/fetch_musashi.sh の試験。通信はしない: curl の代わりに、にせの Musashi (m68kmake.c だけ) の tar.gz を出すコマンドを
# PATH の先頭に置く。いくつかの runner が同じ HOME で同時に動いても壊れないこと、途中で失敗したり止められたりしても
# 作りかけが残らないことを確かめる。
#   使い方: tests/tools/fetch_musashi_test.sh <tools/fetch_musashi.sh>
set -u
SCRIPT=$1
SHA=$(sed -n 's/^SHA=//p' "$SCRIPT")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fail=0
ok() { echo "ok: $1"; }
ng() { echo "NG: $1"; fail=1; }
# $1: 説明、残り: 条件のコマンド
check() { d=$1; shift; if "$@"; then ok "$d"; else ng "$d"; fi; }
# コマンドを、出力を捨てて動かす
quiet() { "$@" > /dev/null 2>&1; }

# にせの Musashi: m68kmake は m68kops.c を作るだけ
mkdir -p "$WORK/src/Musashi-fake" "$WORK/bin"
cat > "$WORK/src/Musashi-fake/m68kmake.c" <<'C'
#include <stdio.h>
int main(void) {
    FILE* f = fopen("m68kops.c", "w");
    if (!f) return 1;
    fputs("/* fake */\n", f);
    return fclose(f) != 0;
}
C
tar -czf "$WORK/fake.tar.gz" -C "$WORK/src" Musashi-fake
# にせの curl: FAKE_CURL_SLEEP 秒待ち、FAKE_CURL_FAIL があれば失敗し、なければ tar.gz を出す
cat > "$WORK/bin/curl" <<'SH'
#!/bin/sh
if [ -n "${FAKE_CURL_SLEEP:-}" ]; then sleep "$FAKE_CURL_SLEEP"; fi
if [ -n "${FAKE_CURL_FAIL:-}" ]; then exit 22; fi
cat "$FAKE_TARBALL"
SH
chmod +x "$WORK/bin/curl"
FAKE_TARBALL=$WORK/fake.tar.gz
PATH=$WORK/bin:$PATH
export FAKE_TARBALL PATH

# $1 個を同時に動かす。$2 は出力先。どれかが失敗したら 1
par() {
  n=$1; out=$2; pids=; i=0
  while [ $i -lt "$n" ]; do sh "$SCRIPT" "$out" > /dev/null 2>&1 & pids="$pids $!"; i=$((i + 1)); done
  rc=0; for p in $pids; do wait "$p" || rc=1; done
  return $rc
}
# $1 (末尾の / なし) ができあがっているか
ready() { [ -s "$1/m68kops.c" ] && [ "$(cat "$1/.pix68k-sha" 2>/dev/null)" = "$SHA" ]; }
# $1 の隣に、作りかけ (.tmp.)、どけたもの (.old.)、ロック (.lock) が残っていないか
clean() { [ -z "$(ls -d "$1".tmp.* "$1".old.* "$1".lock 2>/dev/null)" ]; }
inode() { ls -di "$1" | awk '{ print $1 }'; }

# 1. 出力先がないところへ、4 つ同時
O=$WORK/c1/mus; mkdir -p "$WORK/c1"
check '出力先がないところへ 4 つ同時: すべて成功' par 4 "$O"
check '出力先がないところへ 4 つ同時: できあがっている' ready "$O"
check '出力先がないところへ 4 つ同時: 残りがない' clean "$O"

# 2. できあがった出力先は、触らない
before=$(inode "$O")
check 'できあがった出力先に 3 つ同時: すべて成功' par 3 "$O"
check 'できあがった出力先は置き換えない' [ "$(inode "$O")" = "$before" ]

# 3. 空のディレクトリ
O=$WORK/c3/mus; mkdir -p "$O"
check '空の出力先に 3 つ同時: すべて成功' par 3 "$O"
check '空の出力先に 3 つ同時: できあがっている' ready "$O"
check '空の出力先に 3 つ同時: 残りがない' clean "$O"

# 4. 末尾に / がある出力先
O=$WORK/c4/mus; mkdir -p "$WORK/c4"
check '末尾に / がある出力先: 成功' quiet sh "$SCRIPT" "$O//"
check '末尾に / がある出力先: できあがっている' ready "$O"
check '末尾に / がある出力先: 残りがない' clean "$O"

# 5. SHA が古い出力先は置き換える
echo old > "$O/.pix68k-sha"
check 'SHA が古い出力先に 3 つ同時: すべて成功' par 3 "$O"
check 'SHA が古い出力先に 3 つ同時: 置き換わっている' ready "$O"
check 'SHA が古い出力先に 3 つ同時: 残りがない' clean "$O"

# 6. このスクリプトが作ったものでないディレクトリは断る
O=$WORK/c6/mus; mkdir -p "$O"; echo x > "$O/file"
if sh "$SCRIPT" "$O" > /dev/null 2>&1; then ng '他人のディレクトリを断る'; else ok '他人のディレクトリを断る'; fi
check '他人のディレクトリの中身はそのまま' [ "$(ls -A "$O")" = file ]
check '他人のディレクトリ: 残りがない' clean "$O"

# 7. 取得に失敗したら、何も残さない
O=$WORK/c7/mus; mkdir -p "$WORK/c7"
if FAKE_CURL_FAIL=1 sh "$SCRIPT" "$O" > /dev/null 2>&1; then ng '取得に失敗したら失敗で終わる'; else ok '取得に失敗したら失敗で終わる'; fi
check '取得に失敗: 出力先を作らない' [ ! -e "$O" ]
check '取得に失敗: 残りがない' clean "$O"

# 8. 取得の途中で TERM を受けたら、何も残さない
O=$WORK/c8/mus; mkdir -p "$WORK/c8"
FAKE_CURL_SLEEP=2 sh "$SCRIPT" "$O" > /dev/null 2>&1 &
pid=$!
sleep 1
kill -TERM "$pid"
if wait "$pid"; then ng 'TERM を受けたら失敗で終わる'; else ok 'TERM を受けたら失敗で終わる'; fi
check 'TERM: 出力先を作らない' [ ! -e "$O" ]
check 'TERM: 残りがない' clean "$O"

# 9. ロックが残っていたら、待ってから 75 で終わる。ロックと出力先には触らない
O=$WORK/c9/mus; mkdir -p "$WORK/c9" "$O.lock"
rc=0; PIX68K_FETCH_LOCK_TIMEOUT=1 sh "$SCRIPT" "$O" > /dev/null 2>&1 || rc=$?
check '残ったロック: 終了コード 75' [ "$rc" = 75 ]
check '残ったロック: ロックを消さない' [ -d "$O.lock" ]
check '残ったロック: 出力先を作らない' [ ! -e "$O" ]
check '残ったロック: 作りかけを残さない' [ -z "$(ls -d "$O".tmp.* "$O".old.* 2>/dev/null)" ]

# 10. 空の出力先を見てから作り始めた実行 (遅い) は、その間にほかの実行が置いたものを、どけない
O=$WORK/c10/mus; mkdir -p "$O"
FAKE_CURL_SLEEP=2 sh "$SCRIPT" "$O" > /dev/null 2>&1 &
pid=$!
sleep 1
check '遅い実行の途中で、速い実行が置く' quiet sh "$SCRIPT" "$O"
before=$(inode "$O")
check '遅い実行も成功' wait "$pid"
check '遅い実行は、置いてあるものをどけない' [ "$(inode "$O")" = "$before" ]
check '遅い実行: 残りがない' clean "$O"

exit $fail
