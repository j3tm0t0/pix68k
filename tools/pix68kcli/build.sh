#!/bin/sh
# pix68kcli を各環境向けにクロスビルドする (CGO なし)。成果物は dist/ に置く。
#   tools/pix68kcli/build.sh            # すべて
#   tools/pix68kcli/build.sh linux/arm64 windows/amd64   # 選んだものだけ
# 版は PIX68KCLI_VERSION (なければ git describe) を埋め込む
set -eu
cd "$(dirname "$0")"
VERSION=${PIX68KCLI_VERSION:-$(git describe --tags --always --dirty 2>/dev/null || echo dev)}
TARGETS=${*:-"darwin/arm64 darwin/amd64 linux/arm64 linux/arm/7 linux/amd64 windows/amd64 windows/arm64"}
mkdir -p dist
for t in $TARGETS; do
    os=${t%%/*}
    rest=${t#*/}
    arch=${rest%%/*}
    arm=
    name=pix68kcli-$os-$arch
    if [ "$arch" = arm ]; then
        arm=${rest#arm}
        arm=${arm#/}
        arm=${arm:-7}
        name=pix68kcli-$os-armv$arm
    fi
    [ "$os" = windows ] && name=$name.exe
    echo "build $name"
    CGO_ENABLED=0 GOOS=$os GOARCH=$arch GOARM=$arm \
        go build -trimpath -ldflags "-s -w -X main.version=$VERSION" -o "dist/$name" .
done
