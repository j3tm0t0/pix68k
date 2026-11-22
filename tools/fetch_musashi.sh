#!/bin/sh
# Musashi (68000 系のエミュレータ。tests/m68k/musashi_diff.cpp で 68030 のコアと比べる) を取得し、m68kmake で
# m68kops.c を作る。Musashi はリポジトリに入れず、キャッシュディレクトリに置く。
#   使い方: tools/fetch_musashi.sh [出力先 (既定: ~/.cache/pix68k/musashi)]
# CMake の MUSASHI_DIR に出力先を渡すと、musashi_diff をビルドしてテストに足す
set -eu
SHA=313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd
OUT=${1:-$HOME/.cache/pix68k/musashi}
# 末尾の / を取る ($OUT.tmp.<番号> などを、出力先の中ではなく隣に作るため)
while [ "$OUT" != / ] && [ "${OUT%/}" != "$OUT" ]; do OUT=${OUT%/}; done
# 同じ機械のいくつかの runner が同じ HOME で同時に動いても壊れないようにしてある。
# - 自分だけのディレクトリ ($OUT.tmp.<プロセス番号>) に作ってから、名前を変えて $OUT に置く。作りかけの $OUT を、ほかの実行が見ない
# - 置くところ (できあがった $OUT があるかを確かめ、古い $OUT をどけ、新しいものを置く) は、mkdir のロック ($OUT.lock) の中で行う。
#   $OUT を置くのも、どけるのも、ロックの中だけなので、できあがった $OUT (SHA が同じもの) を、ほかの実行がどけることはない
# - SHA を変えたときは、古い $OUT を置き換える。そのとき古い $OUT を使ってビルドしている実行があれば、その実行は失敗する
# ロックは一瞬で終わる処理の間だけ持ち、その間は INT / TERM / HUP を無視する。kill -9 や電源断で $OUT.lock が残ったら、
# あとの実行は PIX68K_FETCH_LOCK_TIMEOUT 秒 (既定 120) 待って終了コード 75 で失敗する。動いている fetch_musashi.sh が
# ないことを確かめてから、rmdir "$OUT.lock" で消す
ready() { [ -s "$OUT/m68kops.c" ] && [ "$(cat "$OUT/.pix68k-sha" 2>/dev/null)" = "$SHA" ]; }
# 消してよいのは、このスクリプトが前に作ったディレクトリ (.pix68k-sha がある) か、空のディレクトリだけ
removable() { [ -f "$OUT/.pix68k-sha" ] || [ -z "$(ls -A "$OUT" 2>/dev/null)" ]; }
refuse() {
  echo "$OUT はこのスクリプトが作ったものではないので消さない。別の出力先を指定するか、手で消すこと" >&2
  exit 1
}
if ready; then
  echo "Musashi in $OUT"
  exit 0
fi
if [ -e "$OUT" ] && ! removable; then refuse; fi
TMP=$OUT.tmp.$$
trap 'rm -rf "$TMP"' EXIT
trap 'exit 1' INT TERM HUP
rm -rf "$TMP"
mkdir -p "$TMP"
curl -fsSL "https://github.com/kstenerud/Musashi/archive/$SHA.tar.gz" | tar -xzf - -C "$TMP" --strip-components 1
cc -O1 -o "$TMP/m68kmake" "$TMP/m68kmake.c"
(cd "$TMP" && ./m68kmake)
echo "$SHA" > "$TMP/.pix68k-sha"
# ロックの中で確かめ直してから置く。終了コード: 0 = 置いた、またはほかの実行が置いていた。3 = $OUT が他人のもの。
# 75 = ロックが取れなかった。1 = 待っている間に止められた
RC=0
python3 -I - "$TMP" "$OUT" "$SHA" "${PIX68K_FETCH_LOCK_TIMEOUT:-120}" <<'PY' || RC=$?
import os, shutil, signal, sys, time

tmp, out, sha, timeout = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
lock = out + ".lock"
old = "%s.old.%d" % (out, os.getpid())
stop = []
for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
    signal.signal(s, lambda *a: stop.append(1))  # ロックを持っている間は、覚えるだけで止まらない


def ready():
    try:
        with open(os.path.join(out, ".pix68k-sha")) as f:
            return f.read().strip() == sha and os.path.getsize(os.path.join(out, "m68kops.c")) > 0
    except OSError:
        return False


def removable():
    return os.path.isfile(os.path.join(out, ".pix68k-sha")) or (os.path.isdir(out) and not os.listdir(out))


deadline = time.monotonic() + timeout
while True:
    if stop:
        sys.exit(1)
    try:
        os.mkdir(lock)
        break
    except FileExistsError:
        if time.monotonic() >= deadline:
            sys.stderr.write("%s が %g 秒待っても空かなかった。動いている fetch_musashi.sh がなければ、rmdir で消すこと\n" % (lock, timeout))
            sys.exit(75)
        time.sleep(0.2)
rc = 0
try:
    if not ready():
        if os.path.lexists(out):
            if removable():
                os.rename(out, old)
            else:
                rc = 3
        if rc == 0:
            os.rename(tmp, out)
finally:
    os.rmdir(lock)
shutil.rmtree(old, ignore_errors=True)
sys.exit(rc)
PY
case $RC in
  0) ;;
  3) refuse ;;
  *) exit "$RC" ;;
esac
echo "Musashi in $OUT"
