#!/bin/sh
# Circle の xHCI のドライバの、コントローラーへのコマンドがタイムアウトしたときの扱いを pix68k 用に書き換える (#308)。
#   使い方: tools/circle_xhci_cmd_patch.sh <元の xhcicommandmanager.cpp> <書き出す先> <元の xhcicommandmanager.h> <書き出す先>
#                                          <元の xhciusbdevice.cpp> <書き出す先>
# ベアメタル版のビルド (tools/build_baremetal.sh) が使う。書き換える行が見つからなければ失敗する (Circle を上げたときに気づく)。
#
# HORI の HORIPAD S (0f0d:00c1) を Pi 5 であとからつなぐと、アドレスを付けるコマンド (Address Device) が 3 秒でタイムアウト
# した。元の Circle は、タイムアウトのあとコマンドリング (Command Ring) を止めないので、コントローラーは前のコマンドを続けた
# ままで、次のコマンド (装置を消すときの Disable Slot) もタイムアウトする。そして ~CXHCIUSBDevice() の assert で、すべての
# コアが止まった (2026-11-23 の UART のログ)。次のように直す:
#   - CXHCICommandManager::DoCommand(): タイムアウトしたら、xHCI の仕様 (4.6.1.2) のとおり CRCR の CA (Command Abort) ビットを
#     立ててコマンドリングを止める。CRR が 0 になるのを待ち (最大 5 秒)、さらにコントローラーが出す Command Ring Stopped
#     (完了コード 24) のイベントを割り込みハンドラが受け取るのを待つ (最大 1 秒)。止めたコマンドの Command Aborted (25) の
#     イベントはその前に出るので、これで古いコマンドのイベントはすべて受け取り終わっている。次のコマンドのドアベルで、
#     コマンドリングは次の TRB から動き出す
#     コマンドリングが動いていないとき (CRR が 0) は CRCR に書かない (書くとコマンドリングの場所を 0 に変えてしまう)。
#     書くのは下位の 32 ビットだけ (Pi 5 では DMA の番地の上位が 0x10 なので、上位に 0 を書かない)
#   - 止められなかったとき (CRR が 0 にならない、または 24 のイベントが来ない) は、USBCMD の R/S を 0 にしてコントローラーを
#     止め (Pix68kHalt())、以後の DoCommand() はすぐ失敗を返す。止まったコントローラーはメモリを読み書きしないので、
#     呼び出し元が Input Context や転送リングを解放しても安全になる。止まったこと (USBSTS の HCH が 1) を 1 秒待っても確かめられ
#     なければ、解放したメモリにコントローラーが書く恐れがあるので、LogPanic でシステムを止める (元の assert と同じ結果。
#     仕様では R/S を 0 にしてから 16ms 以内に止まる)。仕様はこの場合コントローラーのリセットを勧めるが、
#     Circle には途中でリセットし直す処理がないので、その USB コントローラーにつないだ装置は再起動まで使えなくなる
#     (Pi が固まったり、解放したメモリにコントローラーが書いたりするよりはよい)
#     止められたら (またはコマンドリングが動いていなかったら)、タイムアウトしたコマンドの TRB を No Op コマンドに書き換える。
#     CA がコマンドの実行より前に届くと、コマンドリングはそのコマンドの TRB で止まり (Command Aborted のイベントは出ず、
#     Command Ring Stopped のイベントがその TRB を指す)、次のドアベルでそのコマンドを実行してしまう。そのときには呼び出し元が
#     Input Context を解放しているので、コントローラーが解放したメモリを読むことになる。実行済み・中断済みのコマンドの TRB は
#     コマンドリングの読む位置より後ろにあるので、書き換えても何も起きない。No Op の完了のイベントは、次のコマンドの TRB とは
#     違う TRB を指すので、CommandCompleted() が捨てる
#   - CommandCompleted(): Command Ring Stopped (24) のイベントは、次に出すコマンドの TRB を指すので、コマンドの完了と
#     取り違えないよう、止まったことと、指している TRB を記録するだけにする
#   - ~CXHCIUSBDevice(): Disable Slot が失敗しても assert で止めず、ログに出す。このときスロットはコントローラーの中で
#     まだ有効で、Device Context や転送リングを使い続けるかもしれないので、上と同じくコントローラーを止めてから、元の
#     とおり解放する
set -eu
[ $# -eq 6 ] || { echo "usage: $0 <xhcicommandmanager.cpp> <out> <xhcicommandmanager.h> <out> <xhciusbdevice.cpp> <out>" >&2; exit 2; }

awk '
  # DoCommand(): 止められなかったコントローラーには、コマンドを出さない
  $0 == "\tTXHCITRB *pCmdTRB = m_CmdRing.GetEnqueueTRB ();" && prev1 == "\tassert (m_bCommandCompleted);" {
    print "\tif (m_bPix68kDead)\t// pix68k (#308): the command ring could not be stopped after a timeout"
    print "\t{"
    print "\t\treturn XHCI_PRIV_COMPLETION_CODE_ERROR;"
    print "\t}"
    print ""
    n0++
  }
  # DoCommand() のタイムアウトの return の前に、コマンドリングを止める処理を足す ((空行を除いた) 直前の 2 行も確かめる)
  $0 == "\t\t\treturn XHCI_PRIV_COMPLETION_CODE_TIMEOUT;" && prev2 == "\t\t\tm_bCommandCompleted = TRUE;" &&
      prev1 == "\t\t\tm_pCurrentCommandTRB = 0;" {
    print "\t\t\t// pix68k (#308): abort the command, so that the ring takes the next one (xHCI 4.6.1.2)."
    print "\t\t\t// Without this the controller keeps working on it, and every later command times out"
    print "\t\t\tboolean bStopped = TRUE;"
    print "\t\t\tm_pPix68kStoppedTRB = 0;"
    print "\t\t\tif (m_pMMIO->op_read32 (XHCI_REG_OP_CRCR) & XHCI_REG_OP_CRCR_CRR)"
    print "\t\t\t{"
    print "\t\t\t\tm_bPix68kRingStopped = FALSE;"
    print "\t\t\t\tDataSyncBarrier ();"
    print "\t\t\t\tm_pMMIO->op_write32 (XHCI_REG_OP_CRCR, XHCI_REG_OP_CRCR_CA);"
    print "\t\t\t\tunsigned nAbortTicks = CTimer::Get ()->GetTicks ();"
    print "\t\t\t\twhile (m_pMMIO->op_read32 (XHCI_REG_OP_CRCR) & XHCI_REG_OP_CRCR_CRR)"
    print "\t\t\t\t{"
    print "\t\t\t\t\tif (CTimer::Get ()->GetTicks () - nAbortTicks >= 5*HZ)"
    print "\t\t\t\t\t{"
    print "\t\t\t\t\t\tbStopped = FALSE;"
    print "\t\t\t\t\t\tbreak;"
    print "\t\t\t\t\t}"
    print "\t\t\t\t}"
    print "\t\t\t\t// wait for the Command Ring Stopped event (after the Command Aborted one), so that no event"
    print "\t\t\t\t// of the old command is pending when the next command is issued"
    print "\t\t\t\tnAbortTicks = CTimer::Get ()->GetTicks ();"
    print "\t\t\t\twhile (bStopped && !m_bPix68kRingStopped)"
    print "\t\t\t\t{"
    print "\t\t\t\t\tif (CTimer::Get ()->GetTicks () - nAbortTicks >= HZ)"
    print "\t\t\t\t\t{"
    print "\t\t\t\t\t\tbStopped = FALSE;"
    print "\t\t\t\t\t}"
    print "\t\t\t\t}"
    print "\t\t\t\tDataMemBarrier ();"
    print "\t\t\t}"
    print "\t\t\tif (bStopped)"
    print "\t\t\t{"
    print "\t\t\t\t// If the abort came before the command started, the ring stopped on it (the Command Ring Stopped"
    print "\t\t\t\t// event points to it) and would run it at the next doorbell, after the caller has freed its input"
    print "\t\t\t\t// context. The ring is not running now, so replace the command by a No Op. A command that ran or"
    print "\t\t\t\t// was aborted is behind the dequeue pointer, and rewriting it does no harm"
    print "\t\t\t\tboolean bNotStarted = m_pPix68kStoppedTRB == pCmdTRB;"
    print "\t\t\t\tpCmdTRB->Parameter1 = 0;"
    print "\t\t\t\tpCmdTRB->Parameter2 = 0;"
    print "\t\t\t\tpCmdTRB->Status = 0;"
    print "\t\t\t\tpCmdTRB->Control =   XHCI_TRB_TYPE_CMD_NO_OP << XHCI_TRB_CONTROL_TRB_TYPE__SHIFT"
    print "\t\t\t\t\t\t   | (pCmdTRB->Control & XHCI_TRB_CONTROL_C);"
    print "\t\t\t\tDataSyncBarrier ();"
    print "\t\t\t\tCLogger::Get ()->Write (From, LogWarning, \"Command timed out (control 0x%X), aborted%s\", nControl,"
    print "\t\t\t\t\t\t\tbNotStarted ? \" before it started\" : \"\");"
    print "\t\t\t}"
    print "\t\t\telse"
    print "\t\t\t{"
    print "\t\t\t\tCLogger::Get ()->Write (From, LogError, \"Command timed out (control 0x%X), cannot stop the command ring\","
    print "\t\t\t\t\t\t\tnControl);"
    print "\t\t\t\tPix68kHalt ();"
    print "\t\t\t}"
    print ""
    n1++
  }
  # CommandCompleted(): Command Ring Stopped (24) は、次のコマンドの TRB を指すので、その完了と取り違えない。
  # 指す TRB が 0 のコントローラーでも止まらないよう、関数の最初の assert (pCommandTRB != 0) より前で見る
  index($0, "void CXHCICommandManager::CommandCompleted (") == 1 { inCC = 1 }
  inCC && $0 == "\tassert (pCommandTRB != 0);" {
    inCC = 0
    print "\tif (uchCompletionCode == 24)\t// pix68k (#308): Command Ring Stopped (after an abort), not a completion"
    print "\t{"
    print "\t\tm_pPix68kStoppedTRB = pCommandTRB;"
    print "\t\tDataMemBarrier ();"
    print "\t\tm_bPix68kRingStopped = TRUE;"
    print "\t\treturn;"
    print "\t}"
    print ""
    n2++
  }
  # コンストラクターの初期化子に足す
  $0 == "\tm_pCurrentCommandTRB (0)" && prev1 == "\tm_bCommandCompleted (TRUE)," {
    print "\tm_pCurrentCommandTRB (0),"
    print "\tm_bPix68kRingStopped (FALSE),"
    print "\tm_pPix68kStoppedTRB (0),"
    print "\tm_bPix68kDead (FALSE)"
    n3++; prev2 = prev1; prev1 = $0; next
  }
  { print }
  $0 != "" { prev2 = prev1; prev1 = $0 }
  END {
    if (n0 != 1 || n1 != 1 || n2 != 1 || n3 != 1) exit 1
    # コントローラーを止める (R/S を 0 にして、HCH が 1 になるのを待つ。仕様では 16ms 以内)
    print ""
    print "// pix68k (#308): stop the controller after a command could not be aborted or a slot not disabled, so that"
    print "// it does not access memory that is freed afterwards. Its ports cannot be used until restart then"
    print "void CXHCICommandManager::Pix68kHalt (void)"
    print "{"
    print "\tif (m_bPix68kDead)"
    print "\t{"
    print "\t\treturn;"
    print "\t}"
    print "\tm_bPix68kDead = TRUE;"
    print ""
    print "\tm_pMMIO->op_write32 (XHCI_REG_OP_USBCMD,   m_pMMIO->op_read32 (XHCI_REG_OP_USBCMD)"
    print "\t\t\t\t\t\t & ~XHCI_REG_OP_USBCMD_RUN_STOP);"
    print "\tunsigned nStartTicks = CTimer::Get ()->GetTicks ();"
    print "\twhile (!(m_pMMIO->op_read32 (XHCI_REG_OP_USBSTS) & XHCI_REG_OP_USBSTS_HCH))"
    print "\t{"
    print "\t\tif (CTimer::Get ()->GetTicks () - nStartTicks >= HZ)"
    print "\t\t{"
    print "\t\t\t// not halted: it may still access the memory the caller frees, so stop here (as the assert did)"
    print "\t\t\tCLogger::Get ()->Write (From, LogPanic, \"USB controller does not halt\");"
    print "\t\t}"
    print "\t}"
    print ""
    print "\tCLogger::Get ()->Write (From, LogError, \"USB controller stopped; its ports cannot be used until restart\");"
    print "}"
  }' "$1" > "$2" ||
  { echo "$1: the lines to patch were not found (DoCommand, CommandCompleted or the constructor)" >&2; exit 1; }
grep -q "CTimer::Get ()" "$1" && grep -q "circle/synchronize.h" "$1" ||
  { echo "$1: CTimer or circle/synchronize.h is not used" >&2; exit 1; }

awk '
  $0 == "\tint NoOp (void);" {
    print
    print ""
    print "\tvoid Pix68kHalt (void);\t// pix68k (#308): stop the controller, no more commands"
    m++; prev = $0; next
  }
  $0 == "\tu8\t  \t m_uchSlotID;" && prev == "\tu8\t  \t m_uchCompletionCode;" {
    print
    print ""
    print "\tvolatile boolean m_bPix68kRingStopped;\t// pix68k (#308): Command Ring Stopped event received"
    print "\tTXHCITRB * volatile m_pPix68kStoppedTRB;\t// pix68k (#308): the TRB that event points to"
    print "\tboolean\t\t m_bPix68kDead;\t\t// pix68k (#308): the command ring could not be stopped"
    n++; prev = $0; next
  }
  { print; prev = $0 }
  END { if (n != 1 || m != 1) exit 1 }' "$3" > "$4" ||
  { echo "$3: the lines to patch were not found (NoOp or the members of CXHCICommandManager)" >&2; exit 1; }

awk '
  # ~CXHCIUSBDevice(): "#ifndef NDEBUG / int nResult = / #endif / DisableSlot / assert" の 5 行を置き換える
  inDtor && $0 == "#ifndef NDEBUG" {
    getline l2; getline l3; getline l4; getline l5
    if (l2 != "\t\tint nResult =" || l3 != "#endif" ||
        l4 != "\t\t\tm_pXHCIDevice->GetCommandManager ()->DisableSlot (m_uchSlotID);" ||
        l5 != "\t\tassert (XHCI_CMD_SUCCESS (nResult));") { bad = 1; exit 1 }
    print "\t\t// pix68k (#308): do not halt the CPU when the slot cannot be disabled (a command timed out)."
    print "\t\t// The slot is still enabled in the controller then, so stop the controller before freeing its memory"
    print "\t\tint nResult = m_pXHCIDevice->GetCommandManager ()->DisableSlot (m_uchSlotID);"
    print "\t\tif (!XHCI_CMD_SUCCESS (nResult))"
    print "\t\t{"
    print "\t\t\tCLogger::Get ()->Write (From, LogWarning, \"Cannot disable slot %u (%d)\", (unsigned) m_uchSlotID, nResult);"
    print ""
    print "\t\t\tm_pXHCIDevice->GetCommandManager ()->Pix68kHalt ();"
    print "\t\t}"
    n2++; next
  }
  index($0, "CXHCIUSBDevice::~CXHCIUSBDevice (void)") == 1 { inDtor = 1 }
  inDtor && $0 == "}" { inDtor = 0 }
  { print }
  END { if (bad || n2 != 1) exit 1 }' "$5" > "$6" ||
  { echo "$5: the lines to patch were not found (DisableSlot and its assert in ~CXHCIUSBDevice)" >&2; exit 1; }
grep -q "^static const char From\[\]" "$5" && grep -q "CLogger::Get ()" "$5" ||
  { echo "$5: From or CLogger is not used" >&2; exit 1; }
