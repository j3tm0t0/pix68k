#!/bin/sh
# Circle の GENET (Pi 4 / Pi 400 の Ethernet) の書き換え (tools/circle_genet_patch.sh、#356) を確かめる。
#   使い方: tests/tools/circle_genet_patch_test.sh <リポジトリの場所>
#   1. Circle の今の版に当たり、mii_config() が MAC の送信の遅延を有効に、PHY の送信の遅延を無効にすること
#      (submodule の external/circle がなければ飛ばす)
#   2. 書き換える行がなくなったら、スクリプトが失敗すること
set -u
ROOT=$1
fail=0
ok() { echo "ok: $1"; }
ng() { echo "NG: $1"; fail=1; }
SRC=$ROOT/external/circle/lib/bcm54213.cpp
[ -f "$SRC" ] || { echo "skip: external/circle がない"; exit 0; }
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
PATCH=$ROOT/tools/circle_genet_patch.sh

if sh "$PATCH" "$SRC" "$T/out.cpp" 2>"$T/err"; then ok '今の版に書き換えが当たる'; else ng "当たらない: $(cat "$T/err")"; exit 1; fi
awk 'index($0, "int CBcm54213Device::mii_config(") == 1 { on = 1 } on { print } on && $0 == "}" { exit }' "$T/out.cpp" > "$T/body"
has() { grep -qF "$1" "$T/body" && ok "$2" || ng "$2"; }
has 'reg &= ~id_mode_dis;' 'MAC の遅延を無効にするビットを消す'
has 'clk_ctl & 0x3FF & ~0x200' 'PHY の送信の遅延を無効にする'
if grep -q 'RGMII_MODE_EN | id_mode_dis' "$T/body"; then ng '元の行が残っている'; else ok '元の行が残っていない'; fi

for line in 'u32 id_mode_dis = BIT(16);' 'reg |= RGMII_MODE_EN | id_mode_dis;' 'int CBcm54213Device::mii_config(bool init)'; do
  L=$line awk 'index($0, ENVIRON["L"]) == 0' "$SRC" > "$T/bad.cpp"
  if sh "$PATCH" "$T/bad.cpp" "$T/o2.cpp" 2>/dev/null; then ng "行がなくても成功した: $line"; else ok "行がないと失敗する: $line"; fi
done
exit $fail
