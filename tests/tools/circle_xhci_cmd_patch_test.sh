#!/bin/sh
# Circle の xHCI のコマンドのタイムアウトの扱いの書き換え (tools/circle_xhci_cmd_patch.sh、#308) を確かめる。
#   使い方: tests/tools/circle_xhci_cmd_patch_test.sh <リポジトリの場所>
#   1. Circle の今の版に当たり、書き換えた結果の文字列が期待どおりであること (ベアメタル版は ctest でビルドできないので、
#      文字列を見る。submodule の external/circle がなければ飛ばす)
#      - DoCommand(): タイムアウトで CRR を見てから CA を書き、CRR が 0 になるのと 24 のイベントを待ち、止められなければ
#        コントローラーを止める (Pix68kHalt())。止めたあとはコマンドを出さずに戻る
#        止められたら、タイムアウトしたコマンドの TRB を No Op に書き換える (実行前に止まったとき、次のドアベルで実行させない)
#      - CommandCompleted(): 完了コード 24 を完了として扱わず、止まったことと指している TRB を記録して戻る
#      - Pix68kHalt(): R/S を 0 にして HCH を待つ
#      - ~CXHCIUSBDevice(): assert がなく、Disable Slot が失敗したらコントローラーを止めてから解放する
#   2. 書き換える行がなくなったときに、スクリプトが失敗すること
set -u
ROOT=$1
fail=0
ok() { echo "ok: $1"; }
ng() { echo "NG: $1"; fail=1; }

CM=$ROOT/external/circle/lib/usb/xhcicommandmanager.cpp
CH=$ROOT/external/circle/include/circle/usb/xhcicommandmanager.h
DV=$ROOT/external/circle/lib/usb/xhciusbdevice.cpp
if [ ! -f "$CM" ] || [ ! -f "$CH" ] || [ ! -f "$DV" ]; then
  echo "skip: external/circle がない"
  exit 0
fi
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
PATCH=$ROOT/tools/circle_xhci_cmd_patch.sh

body() {
  START=$2 awk '
    !on && index($0, ENVIRON["START"]) == 1 { on = 1 }
    on { print }
    on && $0 == "}" { exit }' "$1"
}
# 行番号: $1 の文字列 ($2 の中で最初に出てくる行)
lineof() { printf '%s\n' "$2" | grep -nF -m1 "$1" | cut -d: -f1; }
before() { # 説明 本文 前の文字列 後の文字列
  a=$(lineof "$3" "$2"); b=$(lineof "$4" "$2")
  if [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]; then ok "$1"; else ng "$1"; fi
}
has() { # 説明 本文 文字列
  if printf '%s\n' "$2" | grep -qF "$3"; then ok "$1"; else ng "$1"; fi
}

if sh "$PATCH" "$CM" "$T/c.cpp" "$CH" "$T/h.h" "$DV" "$T/d.cpp"; then ok "今の Circle に当たる"; else ng "今の Circle に当たらない"; exit 1; fi

DC=$(body "$T/c.cpp" "int CXHCICommandManager::DoCommand")
has "DoCommand: 止められなかったコントローラーには出さない" "$DC" "if (m_bPix68kDead)"
# 分岐の中で戻ること ({ の次の行が return)
inbranch() { # 説明 本文 if の行 return の行
  printf '%s\n' "$2" | awk -v c="$3" -v r="$4" '
    index($0, c) { f = NR } f && NR == f + 2 && index($0, r) { ok = 1 } END { exit !ok }' && ok "$1" || ng "$1"
}
inbranch "DoCommand: 止めたあとは戻る" "$DC" "if (m_bPix68kDead)" "return XHCI_PRIV_COMPLETION_CODE_ERROR;"
before "DoCommand: その判定はドアベルより前" "$DC" "if (m_bPix68kDead)" "db_write32 (XHCI_REG_DB_HOST_CONTROLLER"
has "DoCommand: CRR を見てから CA を書く" "$DC" "if (m_pMMIO->op_read32 (XHCI_REG_OP_CRCR) & XHCI_REG_OP_CRCR_CRR)"
before "DoCommand: 24 の記録を消してから CA を書く" "$DC" "m_bPix68kRingStopped = FALSE;" "op_write32 (XHCI_REG_OP_CRCR, XHCI_REG_OP_CRCR_CA)"
before "DoCommand: CA のあと CRR が 0 になるのを待つ" "$DC" "XHCI_REG_OP_CRCR_CA)" "while (m_pMMIO->op_read32 (XHCI_REG_OP_CRCR) & XHCI_REG_OP_CRCR_CRR)"
before "DoCommand: そのあと 24 のイベントを待つ" "$DC" "while (m_pMMIO->op_read32 (XHCI_REG_OP_CRCR) & XHCI_REG_OP_CRCR_CRR)" "while (bStopped && !m_bPix68kRingStopped)"
has "DoCommand: 止められなければコントローラーを止める" "$DC" "Pix68kHalt ();"
# 実行前に止まったコマンドを、次のドアベルで実行させない (No Op に書き換える)
before "DoCommand: 24 が指す TRB の記録を消してから CA を書く" "$DC" "m_pPix68kStoppedTRB = 0;" "op_write32 (XHCI_REG_OP_CRCR, XHCI_REG_OP_CRCR_CA)"
before "DoCommand: 24 のイベントを待ってから、タイムアウトしたコマンドを No Op に書き換える" "$DC" "while (bStopped && !m_bPix68kRingStopped)" "XHCI_TRB_TYPE_CMD_NO_OP << XHCI_TRB_CONTROL_TRB_TYPE__SHIFT"
has "DoCommand: サイクルビットは元のまま" "$DC" "| (pCmdTRB->Control & XHCI_TRB_CONTROL_C);"
has "DoCommand: 実行前に止まったかを 24 の TRB で見る" "$DC" "boolean bNotStarted = m_pPix68kStoppedTRB == pCmdTRB;"
# 書き換えは止められたとき (if (bStopped) の中) だけ。止められなければコントローラーを止めるので書き換えない
printf '%s\n' "$DC" | awk '
  /^\t\t\tif \(bStopped\)$/ { b = NR }
  b && !e && NR > b + 1 && /^\t\t\t}$/ { e = NR }
  /XHCI_TRB_TYPE_CMD_NO_OP/ { n = NR }
  /DataSyncBarrier/ && n && !d { d = NR }
  END { exit !(b && n && e && b < n && n < d && d < e) }' &&
  ok "DoCommand: No Op への書き換えとバリアは if (bStopped) の中" || ng "DoCommand: No Op への書き換えの場所が違う"
before "DoCommand: 書き換えはタイムアウトの return の前" "$DC" "XHCI_TRB_TYPE_CMD_NO_OP" "return XHCI_PRIV_COMPLETION_CODE_TIMEOUT;"
before "DoCommand: これらはタイムアウトの return の前" "$DC" "Pix68kHalt ();" "return XHCI_PRIV_COMPLETION_CODE_TIMEOUT;"
HT=$(body "$T/c.cpp" "void CXHCICommandManager::Pix68kHalt (void)")
has "Pix68kHalt: 以後のコマンドを出さないようにする" "$HT" "m_bPix68kDead = TRUE;"
has "Pix68kHalt: R/S を 0 にする" "$HT" "& ~XHCI_REG_OP_USBCMD_RUN_STOP);"
before "Pix68kHalt: そのあと HCH を待つ" "$HT" "~XHCI_REG_OP_USBCMD_RUN_STOP" "XHCI_REG_OP_USBSTS_HCH"
# HCH が立たないまま時間切れになったら、戻らずに止める (LogPanic)。待つループの中にあること
printf '%s\n' "$HT" | awk '
  /while \(!\(m_pMMIO->op_read32 \(XHCI_REG_OP_USBSTS\) & XHCI_REG_OP_USBSTS_HCH\)\)/ { w = NR }
  w && !e && /^\t}$/ { e = NR }
  /LogPanic/ { p = NR }
  /break;/ && w && !e { b = 1 }
  END { exit !(w && p && e && w < p && p < e && !b) }' &&
  ok "Pix68kHalt: HCH が立たなければ止める (LogPanic、ループを抜けない)" || ng "Pix68kHalt: HCH が立たないときに先へ進む"
if printf '%s\n' "$DC" | grep -qF "XHCI_REG_OP_CRCR_HI"; then ng "CRCR の上位に書いている"; else ok "CRCR の上位には書かない"; fi
CC=$(body "$T/c.cpp" "void CXHCICommandManager::CommandCompleted")
has "CommandCompleted: 完了コード 24 を見る" "$CC" "if (uchCompletionCode == 24)"
before "CommandCompleted: 24 は assert (pCommandTRB != 0) より前に見る" "$CC" "if (uchCompletionCode == 24)" "assert (pCommandTRB != 0);"
before "CommandCompleted: 24 では止まったことを記録して、完了の処理の前に戻る" "$CC" "m_bPix68kRingStopped = TRUE;" "m_uchCompletionCode = uchCompletionCode;"
before "CommandCompleted: 24 が指す TRB を、止まったことより先に記録する" "$CC" "m_pPix68kStoppedTRB = pCommandTRB;" "m_bPix68kRingStopped = TRUE;"
printf '%s\n' "$CC" | awk '/m_bPix68kRingStopped = TRUE;/ { f = NR } f && NR == f + 1 && /^\t\treturn;$/ { ok = 1 } END { exit !ok }' &&
  ok "CommandCompleted: 24 の分岐の中で戻る" || ng "CommandCompleted: 24 の分岐の中で戻っていない"
CT=$(body "$T/c.cpp" "CXHCICommandManager::CXHCICommandManager")
has "コンストラクターで変数を初期化する" "$CT" "m_bPix68kDead (FALSE)"
has "コンストラクターで 24 の TRB を初期化する" "$CT" "m_pPix68kStoppedTRB (0),"
grep -qF "volatile boolean m_bPix68kRingStopped;" "$T/h.h" && grep -qF "m_bPix68kDead;" "$T/h.h" &&
  grep -qF "TXHCITRB * volatile m_pPix68kStoppedTRB;" "$T/h.h" &&
  grep -qF "void Pix68kHalt (void);" "$T/h.h" &&
  ok "ヘッダーに変数を足した" || ng "ヘッダーに変数がない"

DD=$(body "$T/d.cpp" "CXHCIUSBDevice::~CXHCIUSBDevice (void)")
if printf '%s\n' "$DD" | grep -qF "assert (XHCI_CMD_SUCCESS"; then ng "デストラクタに assert が残っている"; else ok "デストラクタの assert を外した"; fi
[ "$(printf '%s\n' "$DD" | grep -c "DisableSlot (m_uchSlotID)")" -eq 1 ] && ok "DisableSlot は 1 回" || ng "DisableSlot の数が違う"
[ "$(printf '%s\n' "$DD" | grep -c "FreeSlot (m_uchSlotID)")" -eq 1 ] && ok "FreeSlot は元のとおり 1 回" || ng "FreeSlot の数が違う"
inbranch "失敗したらコントローラーを止める" "$DD" "if (!XHCI_CMD_SUCCESS (nResult))" "Cannot disable slot"
before "コントローラーを止めてから FreeSlot" "$DD" "Pix68kHalt ();" "FreeSlot (m_uchSlotID)"
has "失敗をログに出す" "$DD" "Cannot disable slot"

# 書き換えた行のほかは変わっていない
diff "$CM" "$T/c.cpp" | grep '^<' | grep -qvF "m_pCurrentCommandTRB (0)" &&
  ng "xhcicommandmanager.cpp の元の行が消えた" || ok "xhcicommandmanager.cpp は足しただけ (初期化子の 1 行を除く)"
diff "$CH" "$T/h.h" | grep -q '^<' && ng "ヘッダーの元の行が消えた" || ok "ヘッダーは足しただけ"

# 書き換える行がないと失敗する
fails() { # 説明 c.cpp h.h d.cpp
  if sh "$PATCH" "$2" "$T/o1" "$3" "$T/o2" "$4" "$T/o3" 2>/dev/null; then ng "$1 がなくても通る"; else ok "$1 がないと失敗する"; fi
}
grep -v "return XHCI_PRIV_COMPLETION_CODE_TIMEOUT;" "$CM" > "$T/c2.cpp"; fails "タイムアウトの return" "$T/c2.cpp" "$CH" "$DV"
grep -v "assert (pCommandTRB != 0);" "$CM" > "$T/c3.cpp"; fails "CommandCompleted の最初の assert" "$T/c3.cpp" "$CH" "$DV"
grep -v "TXHCITRB \*pCmdTRB = m_CmdRing.GetEnqueueTRB ();" "$CM" > "$T/c4.cpp"; fails "GetEnqueueTRB の行" "$T/c4.cpp" "$CH" "$DV"
grep -v "m_uchSlotID;" "$CH" > "$T/h2.h"; fails "ヘッダーの m_uchSlotID" "$CM" "$T/h2.h" "$DV"
grep -v "assert (XHCI_CMD_SUCCESS (nResult));" "$DV" > "$T/d2.cpp"; fails "デストラクタの assert" "$CM" "$CH" "$T/d2.cpp"
grep -v "int NoOp (void);" "$CH" > "$T/h3.h"; fails "ヘッダーの NoOp" "$CM" "$T/h3.h" "$DV"

exit $fail
