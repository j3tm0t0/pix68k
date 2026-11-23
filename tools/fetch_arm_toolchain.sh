#!/bin/sh
# Arm GNU Toolchain (Arm が配っている tar.xz) を取ってきて展開し、bin のディレクトリを標準出力に 1 行で出す。
#   使い方: tools/fetch_arm_toolchain.sh <URL> <SHA-256>
# 置き場所は ${PIX68K_TOOLCHAIN_CACHE:-$HOME/.cache/pix68k/toolchains}/<tar.xz の名前から .tar.xz を除いたもの>。
# すでに展開してあれば何もしない (展開を終えたディレクトリには、照合した SHA-256 を .sha256 に書いておき、それが合うときだけ使う)。
# self-hosted の runner で、同じ機械のいくつかの job が同時に動いてもよいように、flock で 1 つずつにし、別のディレクトリに展開してから mv で置く。
# SHA-256 は Arm が配っている .sha256asc の値を渡す (release-build.yml)。
set -eu
[ $# -eq 2 ] || { echo "usage: $0 <URL> <SHA-256>" >&2; exit 2; }
URL=$1
SHA=$2
case $SHA in
  *[!0-9a-f]*|'') echo "SHA-256 が 16 進でない: $SHA" >&2; exit 2 ;;
esac
[ ${#SHA} -eq 64 ] || { echo "SHA-256 が 64 桁でない: $SHA" >&2; exit 2; }
NAME=$(basename "$URL" .tar.xz)
case $NAME in
  ''|*[!A-Za-z0-9._-]*) echo "URL の名前が正しくない: $NAME" >&2; exit 2 ;;
esac
CACHE=${PIX68K_TOOLCHAIN_CACHE:-$HOME/.cache/pix68k/toolchains}
DEST=$CACHE/$NAME
valid() { [ -f "$DEST/.sha256" ] && [ "$(cat "$DEST/.sha256")" = "$SHA" ] && [ -d "$DEST/bin" ]; }
if valid; then
  echo "$DEST/bin"
  exit 0
fi
mkdir -p "$CACHE"
# 同じ機械の job が同時に取りに来たときに、調べてから置くまでを 1 つずつにする (flock は util-linux。なければロックなしで進む)
LOCKED=
if command -v flock >/dev/null 2>&1; then
  exec 9>"$CACHE/.$NAME.lock"
  flock 9
  LOCKED=yes
fi
if valid; then
  echo "$DEST/bin"
  exit 0
fi
# 前に途中で止まった job (取り消しなど) が残した展開途中のディレクトリを消す。ロックを取れたときだけ (動いている job のものを消さないため)
[ -z "$LOCKED" ] || find "$CACHE" -mindepth 1 -maxdepth 1 -type d -name ".$NAME.??????" -exec rm -rf {} + 2>/dev/null || true
TMP=$(mktemp -d "$CACHE/.$NAME.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
curl -fsSL --retry 3 -o "$TMP/a.tar.xz" "$URL" >&2
if command -v sha256sum >/dev/null 2>&1; then
  HAVE=$(sha256sum "$TMP/a.tar.xz" | awk '{ print $1 }')
else
  HAVE=$(shasum -a 256 "$TMP/a.tar.xz" | awk '{ print $1 }')
fi
[ "$HAVE" = "$SHA" ] || { echo "SHA-256 が合わない: $NAME ($HAVE)" >&2; exit 1; }
mkdir "$TMP/t"
tar -xJf "$TMP/a.tar.xz" -C "$TMP/t" --strip-components=1
echo "$SHA" > "$TMP/t/.sha256"
# 照合できない古いものがあれば消してから置く (ロックの中なので、ほかの job が置いたものを消すことはない)
rm -rf "$DEST"
mv "$TMP/t" "$DEST"
valid || { echo "$DEST を置けない" >&2; exit 1; }
echo "$DEST/bin"
