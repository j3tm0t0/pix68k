#!/bin/sh
# Circle の USB シリアルのドライバの書き換え (tools/circle_fuk_patch.sh。#295) を確かめる。
#   使い方: tests/tools/circle_fuk_patch_test.sh <リポジトリの場所>
#   1. 今の Circle (submodule の external/circle。なければ飛ばす) を書き換えられ、足した関数と呼び出しが入ること。
#      xhciusbdevice.cpp は、#308 の書き換え (circle_xhci_cmd_patch.sh) の結果に重ねても両方が残ること
#   2. 書き換える行がなくなったとき、関数の "{" が名前と同じ行に移ったときに、スクリプトが失敗すること
#      (気づかずに、足りないままビルドしない)
set -u
ROOT=$1
PATCH=$ROOT/tools/circle_fuk_patch.sh
CPP=$ROOT/external/circle/lib/usb/usbserialhost.cpp
H=$ROOT/external/circle/include/circle/usb/usbserialhost.h
X=$ROOT/external/circle/lib/usb/xhciusbdevice.cpp
fail=0
ok() { echo "ok: $1"; }
ng() { echo "NG: $1"; fail=1; }
if [ ! -f "$CPP" ] || [ ! -f "$H" ]; then
  echo "skip: external/circle がない"
  exit 0
fi
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

if sh "$PATCH" "$CPP" "$T/out.cpp" "$H" "$T/out.h" "$X" "$T/outx.cpp" 2>"$T/err"; then ok '今の Circle を書き換えられる'; else ng "書き換えに失敗: $(cat "$T/err")"; fi
has() { if grep -qF -- "$3" "$2"; then ok "$1"; else ng "$1: '$3' がない"; fi; }
has 'ヘッダに受信の関数' "$T/out.h" 'boolean Pix68kStartReceive (void);'
has 'ヘッダに送信の関数' "$T/out.h" 'boolean Pix68kSend (const void *pData, unsigned nLength);'
has '送信の実装' "$T/out.cpp" 'boolean CUSBSerialHostDevice::Pix68kSend (const void *pData, unsigned nLength)'
has '受信の実装' "$T/out.cpp" 'void CUSBSerialHostDevice::Pix68kReceiveStub (CUSBRequest *pURB, void *pParam, void *pContext)'
[ "$(grep -cF 'Pix68kRegister ();' "$T/out.cpp")" = 1 ] && ok 'Configure() が一覧に足す (1 か所)' || ng 'Pix68kRegister () の呼び出しが 1 か所でない'
[ "$(grep -cF 'Pix68kUnregister ();' "$T/out.cpp")" = 1 ] && ok 'デストラクタが一覧から外す (1 か所)' || ng 'Pix68kUnregister () の呼び出しが 1 か所でない'
# デストラクタの "{" のすぐあとに入っていること
if awk '$0 == "CUSBSerialHostDevice::~CUSBSerialHostDevice (void)" { getline a; getline b; if (a == "{" && index(b, "Pix68kUnregister ();")) f = 1 } END { exit !f }' "$T/out.cpp"; then
  ok 'デストラクタの頭で外す'
else
  ng 'デストラクタの頭に Pix68kUnregister () がない'
fi
has '止める関数' "$T/out.cpp" 'void CUSBSerialHostDevice::Pix68kStop (void)'
has '止めたあとは受信の転送を出さない' "$T/out.cpp" 'if (m_pEndpointIn == 0 || m_pPix68kRx == 0 || m_bPix68kStopped)'
has '止めたあとは送信の転送を出さない' "$T/out.cpp" '|| m_bPix68kSending || m_bPix68kStopped'
# xHCI の装置のデストラクタ: スロットを無効にする (DisableSlot) より前に、pix68k に知らせる
# xHCI の装置のデストラクタで、スロットを無効にする (DisableSlot) より前に pix68k に知らせているか
before_slot() {
  awk '$0 == "CXHCIUSBDevice::~CXHCIUSBDevice (void)" { on = 1 } on && index($0, "(*pRemoving) (this);") { told = NR } on && index($0, "DisableSlot") { slot = NR } on && $0 == "}" { exit } END { exit !(told && slot && told < slot) }' "$1"
}
if before_slot "$T/outx.cpp"; then
  ok 'xHCI の装置の破棄: スロットを無効にする前に知らせる'
else
  ng 'xHCI の装置のデストラクタで、DisableSlot の前に知らせていない'
fi
# ベアメタル版のビルドと同じく、#308 の書き換え (tools/circle_xhci_cmd_patch.sh) の結果に重ねても、両方の書き換えが残る
XC=$ROOT/external/circle/lib/usb/xhcicommandmanager.cpp
XH=$ROOT/external/circle/include/circle/usb/xhcicommandmanager.h
if sh "$ROOT/tools/circle_xhci_cmd_patch.sh" "$XC" "$T/c308.cpp" "$XH" "$T/c308.h" "$X" "$T/x308.cpp" 2>"$T/err308" &&
   sh "$PATCH" "$CPP" "$T/o2.cpp" "$H" "$T/o2.h" "$T/x308.cpp" "$T/xboth.cpp" 2>"$T/err2"; then
  ok '#308 の書き換えの結果に重ねられる'
  has '重ねたあとも #308 の直しが残る' "$T/xboth.cpp" 'pix68k (#308)'
  has '重ねたあとも #308 の止める処理が残る' "$T/xboth.cpp" 'Pix68kHalt ()'
  has '重ねたあとに FUK の知らせが入る' "$T/xboth.cpp" '(*pRemoving) (this);'
  if before_slot "$T/xboth.cpp"; then
    ok '重ねたあとも、スロットを無効にする前に知らせる'
  else
    ng '重ねたあと、xHCI の装置のデストラクタで DisableSlot の前に知らせていない'
  fi
else
  ng "#308 の結果に重ねられない: $(cat "$T/err308" "$T/err2")"
fi
# 元の行は消していない (足すだけ)
if [ "$(grep -vF 'Pix68k' "$T/out.cpp" | grep -cF 'int CUSBSerialHostDevice::Read (void *pBuffer, size_t nCount)')" = 1 ]; then ok '元の Read() は残る'; else ng '元の Read() がない'; fi

# 書き換える行がなければ失敗する
must_fail() { # $1: 説明、$2: 壊した .cpp、$3: 壊した .h
  if sh "$PATCH" "$2" "$T/x.cpp" "$3" "$T/x.h" 2>/dev/null; then ng "$1 のに成功した"; else ok "$1 と失敗する"; fi
}
grep -vF 'RegisterSetLinePropertiesHandler (SetLinePropertiesHandler, this);' "$CPP" > "$T/a.cpp"
must_fail 'Configure() の足す場所がない' "$T/a.cpp" "$H"
sed 's/^CUSBSerialHostDevice::~CUSBSerialHostDevice (void)$/CUSBSerialHostDevice::~CUSBSerialHostDevice (void) {/' "$CPP" > "$T/b.cpp"
must_fail 'デストラクタの書き方が変わった' "$T/b.cpp" "$H"
awk '$0 == "CUSBSerialHostDevice::~CUSBSerialHostDevice (void)" { print; getline; print "// moved"; next } { print }' "$CPP" > "$T/c.cpp"
must_fail 'デストラクタの次の行が { でない' "$T/c.cpp" "$H"
grep -vx 'private:' "$H" > "$T/d.h"
must_fail 'ヘッダに private: がない' "$CPP" "$T/d.h"
sed 's/^CXHCIUSBDevice::~CXHCIUSBDevice (void)$/CXHCIUSBDevice::~CXHCIUSBDevice ()/' "$X" > "$T/e.cpp"
if sh "$PATCH" "$CPP" "$T/x.cpp" "$H" "$T/x.h" "$T/e.cpp" "$T/xx.cpp" 2>/dev/null; then ng 'xHCI のデストラクタがないのに成功した'; else ok 'xHCI のデストラクタがない と失敗する'; fi
exit $fail
