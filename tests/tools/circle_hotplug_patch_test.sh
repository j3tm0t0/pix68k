#!/bin/sh
# Circle の、あとからつないだ装置の処理の書き換え (tools/circle_hotplug_patch.sh、#308) を確かめる。
#   使い方: tests/tools/circle_hotplug_patch_test.sh <リポジトリの場所>
#   1. Circle の今の版に当たり、xHCI のルートポートとハブのポートの HandlePortStatusChange() の中で、待ってから探し直す
#      ループになっていること。ベアメタル版は ctest でビルドできないので、書き換えた結果の文字列を見る
#      (submodule の external/circle がなければ飛ばす)
#      探し直すのは、見張るポートの装置で SET_CONFIGURATION が通っていない間だけ (ドライバーのない装置を探し直さない)。
#      ハブのほかのポートの装置では止めない (ポートの親と番号を比べる)
#   2. 書き換える行がなくなったときと、関数の外にしかないときに、スクリプトが失敗すること
set -u
ROOT=$1
fail=0
ok() { echo "ok: $1"; }
ng() { echo "NG: $1"; fail=1; }

XR=$ROOT/external/circle/lib/usb/xhcirootport.cpp
HUB=$ROOT/external/circle/lib/usb/usbstandardhub.cpp
DEV=$ROOT/external/circle/lib/usb/usbdevice.cpp
if [ ! -f "$XR" ] || [ ! -f "$HUB" ] || [ ! -f "$DEV" ]; then
  echo "skip: external/circle がない"
  exit 0
fi
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
PATCH=$ROOT/tools/circle_hotplug_patch.sh

# $1 のファイルの、$2 で始まる行から、次の "}" だけの行までを出す
body() {
  START=$2 awk '
    !on && index($0, ENVIRON["START"]) == 1 { on = 1 }
    on { print }
    on && $0 == "}" { exit }' "$1"
}

if sh "$PATCH" "$XR" "$T/x.cpp" "$HUB" "$T/h.cpp" "$DEV" "$T/d.cpp" 2>"$T/err"; then
  ok 'Circle の今の版に書き換えが当たる'
else
  ng "Circle の今の版に書き換えが当たらない: $(cat "$T/err")"
  exit 1
fi

# 関数の中で、待つ (MsDelay) → (確かめる) → 探す (ReScanDevices) の順に並び、ループの条件に「まだ装置がない」が入っていること
order() { # 説明 ファイル 関数 条件
  B=$(body "$2" "$3")
  if printf '%s\n' "$B" | grep -qF "nTry < PIX68K_HOTPLUG_TRIES && $4 && !g_bPix68kHotplugConfigured;" &&
     printf '%s\n' "$B" | grep -qF "g_pPix68kHotplugParent = $5;" &&
     printf '%s\n' "$B" | grep -qF "g_nPix68kHotplugPort = $6;" &&
     printf '%s\n' "$B" | awk '/g_bPix68kHotplugConfigured = 0;/ { a = NR } /for \(unsigned nTry = 0;/ { f = NR }
                              /ReScanDevices \(\);/ { r = NR } /g_pPix68kHotplugParent = 0;/ { z = NR }
                              END { exit !(a && f && r && z && a < f && f < r && r < z) }' &&
     printf '%s\n' "$B" | awk '/MsDelay \(nTry == 0 \? PIX68K_HOTPLUG_DEBOUNCE_MS : PIX68K_HOTPLUG_RETRY_MS\)/ { d = NR }
                              /ReScanDevices \(\);/ { r = NR } END { exit !(d && r && d < r) }'; then
    ok "$1"
  else
    ng "$1: $3 に待ってから探し直すループがない"
  fi
  # 元の、待たずに 1 回だけ探す行が残っていないこと (ReScanDevices の呼び出しは 1 つだけ)
  n=$(printf '%s\n' "$B" | grep -c 'ReScanDevices ();')
  [ "$n" = 1 ] && ok "$1: ReScanDevices の呼び出しは 1 つ" || ng "$1: ReScanDevices の呼び出しが $n 個"
}
order 'xHCI のルートポート' "$T/x.cpp" 'void CXHCIRootPort::HandlePortStatusChange (void)' 'm_pUSBDevice == 0' \
  '(const void *) (CUSBHCIRootPort *) this' 0
order 'ハブのポート' "$T/h.cpp" 'void CUSBStandardHub::HandlePortStatusChange (void)' 'm_pDevice[nPort] == 0' '(const void *) this' nPort
if body "$T/x.cpp" 'void CXHCIRootPort::HandlePortStatusChange (void)' | grep -qF 'if (!(IsConnected ()))'; then
  ok 'xHCI のルートポート: 待ったあと、抜かれていたら探さない'
else
  ng 'xHCI のルートポート: 待ったあとに IsConnected () を見ていない'
fi
for f in "$T/x.cpp" "$T/h.cpp"; do
  # 値も確かめる (300ms 待ってから 1 回目、そのあと 1 秒おき、合わせて 3 回。HORIPAD S で確かめた値)
  if grep -qE '^#define PIX68K_HOTPLUG_DEBOUNCE_MS[[:space:]]+300$' "$f" &&
     grep -qE '^#define PIX68K_HOTPLUG_RETRY_MS[[:space:]]+1000$' "$f" &&
     grep -qE '^#define PIX68K_HOTPLUG_TRIES[[:space:]]+3$' "$f"; then
    ok "$(basename "$f"): 待ち時間と回数の定義がある (300、1000、3)"
  else
    ng "$(basename "$f"): 待ち時間と回数の定義がない"
  fi
  grep -q '^extern const void \*g_pPix68kHotplugParent;' "$f" && grep -q '^extern unsigned g_nPix68kHotplugPort;$' "$f" &&
    grep -q '^extern volatile int g_bPix68kHotplugConfigured;' "$f" &&
    ok "$(basename "$f"): 見張りの変数を extern で宣言する" || ng "$(basename "$f"): 見張りの変数の宣言がない"
done
# usbdevice.cpp: 変数を定義し、CUSBDevice::Initialize () の SET_CONFIGURATION が通ったあと (機能を探す前) に、見張っている
# ポートの装置なら g_bPix68kHotplugConfigured を立てる
grep -qxF 'const void *g_pPix68kHotplugParent = 0;' "$T/d.cpp" && grep -qxF 'unsigned g_nPix68kHotplugPort = 0;' "$T/d.cpp" &&
  grep -qxF 'volatile int g_bPix68kHotplugConfigured = 0;' "$T/d.cpp" && ok 'usbdevice.cpp: 変数を定義する' || ng 'usbdevice.cpp: 変数の定義がない'
DI=$(body "$T/d.cpp" 'boolean CUSBDevice::Initialize (void)')
printf '%s\n' "$DI" | awk '/m_pHost->SetConfiguration \(m_pEndpoint0, m_pConfigDesc->bConfigurationValue\)/ { s = NR }
  /g_bPix68kHotplugConfigured = 1;/ { c = NR } /unsigned nFunction = 0;/ { f = NR }
  /Device has no supported function/ { u = NR } END { exit !(s && c && f && u && s < c && c < f && f < u) }' &&
  ok 'usbdevice.cpp: SET_CONFIGURATION のあと、機能を探す前に知らせる' || ng 'usbdevice.cpp: 知らせる場所が違う'
[ "$(grep -c 'g_bPix68kHotplugConfigured = 1;' "$T/d.cpp")" = 1 ] && ok 'usbdevice.cpp: 知らせるのは 1 か所' || ng 'usbdevice.cpp: 知らせる場所の数が違う'
# ほかのポートの装置では知らせない: 親 (ハブかルートポート) とハブのポートの番号を比べる
has_d() { printf '%s\n' "$DI" | grep -qF "$2" && ok "$1" || ng "$1"; }
has_d 'usbdevice.cpp: 見張っていないときは知らせない' 'if (   g_pPix68kHotplugParent != 0'
has_d 'usbdevice.cpp: 親を比べる' '&& g_pPix68kHotplugParent == (m_pHub != 0 ? (const void *) m_pHub : (const void *) m_pRootPort)'
has_d 'usbdevice.cpp: ハブではポートの番号を比べる' '&& (m_pHub == 0 || m_nHubPortIndex == g_nPix68kHotplugPort))'
diff "$DEV" "$T/d.cpp" | grep -q '^<' && ng 'usbdevice.cpp: 元の行が消えた' || ok 'usbdevice.cpp は足しただけ'

# 書き換えた行のほかは変えていないこと (足した行を除くと元と同じ)
for pair in "$XR $T/x.cpp" "$HUB $T/h.cpp"; do
  set -- $pair
  if diff "$1" "$2" | grep '^<' | grep -qv 'ReScanDevices ();'; then
    ng "$(basename "$1"): ReScanDevices の行のほかにも消した行がある"
  else
    ok "$(basename "$1"): 消したのは ReScanDevices の行だけ"
  fi
done

# 書き換える行がないとき: 失敗する
awk '$0 == "\t\t\tReScanDevices ();" { $0 = $0 " // moved" } { print }' "$XR" > "$T/x_bad.cpp"
if sh "$PATCH" "$T/x_bad.cpp" "$T/o1" "$HUB" "$T/o2" "$DEV" "$T/o3" 2>/dev/null; then
  ng 'ルートポートの ReScanDevices の行がなくても失敗しない'
else
  ok 'ルートポートの ReScanDevices の行がなければ失敗する'
fi
# 関数の名前が変わったとき (ほかの関数の同じ行を書き換えてしまわない): 失敗する
awk '$0 == "void CUSBStandardHub::HandlePortStatusChange (void)" { $0 = "void CUSBStandardHub::HandlePortStatusChanged (int)" } { print }' "$HUB" > "$T/h_bad.cpp"
if sh "$PATCH" "$XR" "$T/o1" "$T/h_bad.cpp" "$T/o2" "$DEV" "$T/o3" 2>/dev/null; then
  ng 'ハブの HandlePortStatusChange () がなくても失敗しない'
else
  ok 'ハブの HandlePortStatusChange () がなければ失敗する'
fi
# usbdevice.cpp の数える場所がないとき: 失敗する
grep -v 'unsigned nFunction = 0;' "$DEV" > "$T/d_bad.cpp"
if sh "$PATCH" "$XR" "$T/o1" "$HUB" "$T/o2" "$T/d_bad.cpp" "$T/o3" 2>/dev/null; then
  ng 'usbdevice.cpp の数える場所がなくても失敗しない'
else
  ok 'usbdevice.cpp の数える場所がなければ失敗する'
fi
exit $fail
