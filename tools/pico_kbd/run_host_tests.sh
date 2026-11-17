#!/bin/bash
# pico_kbd のホストの試験 (Pico も pico-sdk も要らない) をビルドして走らせる。CI の host-linux の job から呼ぶ。
# 使い方: tools/pico_kbd/run_host_tests.sh [ビルド先 (既定 build/pico_kbd_host)]
set -e
src=$(cd "$(dirname "$0")" && pwd)
out=${1:-build/pico_kbd_host}
mkdir -p "$out"
cc=${CC:-cc}
flags="-std=c11 -O2 -Wall -Wextra -Werror"
run() { # 名前 コンパイルの引数...
    name=$1
    shift
    $cc $flags -I"$src/host_stubs" -I"$src" -o "$out/$name" "$@"
    echo "== $name"
    "$out/$name"
}
run test_host "$src/test_host.c" "$src/core.c"
run test_ota_host -DPICO_KBD_VERSION=7 "$src/test_ota_host.c" "$src/core.c"
run test_otaboot_host "$src/test_otaboot_host.c" "$src/otaboot.c" "$src/core.c"
run test_net_host "$src/test_net_host.c" "$src/core.c"
python3 -m py_compile "$src/picocmd.py" "$src/mkuf2.py" "$src/gen_secrets.py"
echo "== test_tools_host.py (picocmd.py と mkuf2.py)"
python3 "$src/test_tools_host.py"
echo "pico_kbd のホストの試験はすべて ok"
