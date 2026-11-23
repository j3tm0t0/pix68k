#!/bin/bash
# pico_fukemu のホストの試験 (Pico も pico-sdk も要らない) をビルドして走らせる
# 使い方: tools/pico_fukemu/run_host_tests.sh [ビルド先 (既定 build/pico_fukemu_host)]
set -e
src=$(cd "$(dirname "$0")" && pwd)
out=${1:-build/pico_fukemu_host}
mkdir -p "$out"
cc=${CC:-cc}
$cc -std=c11 -O2 -Wall -Wextra -Werror -I"$src" -o "$out/test_host" "$src/test_host.c" "$src/core.c"
"$out/test_host"
$cc -std=c11 -O2 -Wall -Wextra -Werror -I"$src" -o "$out/hostsim" "$src/hostsim.c" "$src/core.c" $([ "$(uname)" = Linux ] && echo -lutil)
# W 命令 (無線 LAN で設定とイメージを書く)。base64 と CRC-32 は pico_kbd の core.c
$cc -std=c11 -O2 -Wall -Wextra -Werror -I"$src" -o "$out/test_netwrite" "$src/test_netwrite.c" "$src/netwrite.c" "$src/../pico_kbd/core.c"
"$out/test_netwrite"
$cc -std=c11 -O2 -Wall -Wextra -Werror -I"$src" -o "$out/nwsim" "$src/nwsim.c" "$src/netwrite.c" "$src/../pico_kbd/core.c"
python3 "$src/test_fukemu_net.py" "$out/nwsim"
python3 "$src/test_mkimg.py"
echo "pico_fukemu のホストの試験はすべて ok"
