#!/bin/sh
# Circle の USB の、あとから装置をつないだときの処理 (プラグアンドプレイ) を pix68k 用に書き換える (#308)。
#   使い方: tools/circle_hotplug_patch.sh <元の xhcirootport.cpp> <書き出す先> <元の usbstandardhub.cpp> <書き出す先>
#                                         <元の usbdevice.cpp> <書き出す先>
# ベアメタル版のビルド (tools/build_baremetal.sh) が使う。書き換える行が見つからなければ失敗する (Circle を上げたときに気づく)。
#
# 元の Circle は、つないだことに気づくとすぐポートをリセットし、100ms 待ってからデバイスディスクリプタを読む。失敗したら
# 装置を作らずに諦め、抜いて挿し直すまで読み直さない。USB 2.0 の仕様 (7.1.7.3) は、つないでからリセットするまで 100ms
# 以上待つことを求めている (接点のばたつきが収まるまで)。HORI の HORIPAD S (0f0d:00c1) は、Pi 5 であとからつなぐと
# 毎回、最初の読み出しが xHCI の完了コード 4 (USB の転送のエラー) で失敗し、"Cannot get device descriptor (short)" に
# なった (起動したときにつないであれば読める)。300ms 待ってから探すと、今度はアドレスを付ける命令が時間切れになった
# (2026-11-23。時間切れのあと固まらないようにする書き換えは tools/circle_xhci_cmd_patch.sh)。挿してから答えられるように
# なるまで 1 秒以上かかるとみて、探し直しの間隔は 1 秒にする。そこで、次の 2 か所で、つないだと分かってから
# PIX68K_HOTPLUG_DEBOUNCE_MS 待ってから探し、装置を作れなければ、まだつながっていれば PIX68K_HOTPLUG_RETRY_MS 後に探し直す
# (合わせて PIX68K_HOTPLUG_TRIES 回まで):
#   - xHCI のルートポート (CXHCIRootPort::HandlePortStatusChange()。Pi 5 の USB の口は RP1 の xHCI に直接つながる)
#   - ハブのポート (CUSBStandardHub::HandlePortStatusChange()。Pi 3 の LAN とハブのチップ、Pi 4 の VL805 の中のハブ、
#     外付けのハブ)
#     ハブでは、つながっているかを ReScanDevices() (EnumeratePorts()) がポートの状態を読んで確かめるので、ループでは見ない
#     ReScanDevices() はハブのすべてのポートを調べ直すので、待っている間にほかのポートにつないだ装置は、その装置の分の
#     待ちなしで探されることがある (元の Circle と同じ)。待ちと回数はポートごとではなく、変化の知らせごとに数える
# 探し直すのは、装置との最初のやりとり (リセット、デバイスディスクリプタやコンフィグレーションの読み出し、アドレスの設定、
# SET_CONFIGURATION) が失敗したときだけ。そこまで済んだ装置は、ドライバーがない ("Device has no supported function")
# などで装置を作れなくても、探し直しても同じなので探し直さない (挿すたびに主ループが 2 秒以上止まらないように)。
# 探し直す前に、見張るポート (ルートポートか、ハブとそのポートの番号) を g_pPix68kHotplugParent と g_nPix68kHotplugPort に
# 書き、CUSBDevice::Initialize() (usbdevice.cpp) は、そのポートの装置で SET_CONFIGURATION が通ったら
# g_bPix68kHotplugConfigured を立てる。ハブの ReScanDevices() はすべてのポートを調べ直すので (前にドライバーがなくて
# 消した、ほかのポートの装置も読み直す)、ほかのポートの装置では立てない
# 起動したときの探し方 (ルートハブとハブの初期化) は変えない (電源を入れてから探すまでに、どの装置も時間がたっている)。
# 探す前に "Port N: scanning a device plugged in (try K)" をログに出す (固まったときに、何回目で止まったかを見るため)。
# 待つのは CTimer::MsDelay() なので、その間も pix68k_msdelay_hook (baremetal/kernel.cpp) が画面を写す。
set -eu
[ $# -eq 6 ] || { echo "usage: $0 <xhcirootport.cpp> <out.cpp> <usbstandardhub.cpp> <out.cpp> <usbdevice.cpp> <out.cpp>" >&2; exit 2; }

# 関数 fn の中の、行 call (タブを含めて一致) を、同じ字下げの探し直しのループに置き換える。cond は「まだ装置がない」条件、
# check は待ったあとに確かめる式 (空なら確かめない)
patch() { # 元 先 関数の名前の行 置き換える行 条件 確かめる式 ログの引数 (ポートの番号, 何回目) 見張るポート (親, 番号)
  awk -v fn="$3" -v call="$4" -v cond="$5" -v check="$6" -v logargs="$7" -v parent="$8" -v port="$9" '
    index($0, fn) == 1 { infn = 1 }
    infn && $0 == call {
      ind = substr(call, 1, index(call, "R") - 1)
      print ind "// pix68k (#308): wait before the port reset (USB 2.0 7.1.7.3: attach debounce, at least 100 ms), and"
      print ind "// retry while no device could be created. HORI HORIPAD S does not answer the first GET_DESCRIPTOR"
      print ind "// when the reset comes right after it is plugged in (tools/circle_hotplug_patch.sh). A device that"
      print ind "// took SET_CONFIGURATION on this port but has no driver is not retried"
      print ind "g_pPix68kHotplugParent = " parent ";"
      print ind "g_nPix68kHotplugPort = " port ";"
      print ind "g_bPix68kHotplugConfigured = 0;"
      print ind "for (unsigned nTry = 0;"
      print ind "     nTry < PIX68K_HOTPLUG_TRIES && " cond " && !g_bPix68kHotplugConfigured;"
      print ind "     nTry++)"
      print ind "{"
      print ind "\tCTimer::Get ()->MsDelay (nTry == 0 ? PIX68K_HOTPLUG_DEBOUNCE_MS : PIX68K_HOTPLUG_RETRY_MS);"
      if (check != "") {
        print ind "\tif (!(" check "))"
        print ind "\t{"
        print ind "\t\tbreak;"
        print ind "\t}"
      }
      print ind "\tCLogger::Get ()->Write (" logargs ");"
      print ind "\tReScanDevices ();"
      print ind "}"
      print ind "g_pPix68kHotplugParent = 0;"
      n++; infn = 0; next
    }
    # 次の関数に入る前に見つからなければ失敗 (関数の終わりは、行の先頭の "}")
    infn && $0 == "}" { exit 1 }
    { print }
    $0 == "#include <assert.h>" && !d {
      print ""
      print "// pix68k (#308): see HandlePortStatusChange () and tools/circle_hotplug_patch.sh"
      print "#define PIX68K_HOTPLUG_DEBOUNCE_MS\t300"
      print "#define PIX68K_HOTPLUG_RETRY_MS\t\t1000"
      print "#define PIX68K_HOTPLUG_TRIES\t\t3"
      print "extern const void *g_pPix68kHotplugParent;\t// usbdevice.cpp: the port watched on hot-plug"
      print "extern unsigned g_nPix68kHotplugPort;"
      print "extern volatile int g_bPix68kHotplugConfigured;\t// a device on it took SET_CONFIGURATION"
      d = 1
    }
    END { if (n != 1 || !d) exit 1 }' "$1" > "$2" ||
    { echo "$1: the lines to patch were not found (#include <assert.h>, $3, ReScanDevices)" >&2; exit 1; }
  grep -q "CTimer::Get ()" "$1" || { echo "$1: CTimer is not used (timer.h may not be included)" >&2; exit 1; }
}

patch "$1" "$2" "void CXHCIRootPort::HandlePortStatusChange (void)" "			ReScanDevices ();" "m_pUSBDevice == 0" \
  "IsConnected ()" 'From, LogNotice, "Port %u: scanning a device plugged in (try %u)", m_nPortIndex+1, nTry+1' \
  "(const void *) (CUSBHCIRootPort *) this" 0
patch "$3" "$4" "void CUSBStandardHub::HandlePortStatusChange (void)" "					ReScanDevices ();" \
  "m_pDevice[nPort] == 0" "" 'FromHub, LogNotice, "Port %u: scanning a device plugged in (try %u)", nPort+1, nTry+1' \
  "(const void *) this" nPort

# CUSBDevice::Initialize(): 見張っているポートの装置で SET_CONFIGURATION が通ったら、探し直しをやめるよう知らせる
awk '
  index($0, "boolean CUSBDevice::Initialize (void)") == 1 { infn = 1 }
  infn && $0 == "\tunsigned nFunction = 0;" && prev == "\t}" {
    print "\t// pix68k (#308): the device on the port watched on hot-plug answered; do not retry it"
    print "\tif (   g_pPix68kHotplugParent != 0"
    print "\t    && g_pPix68kHotplugParent == (m_pHub != 0 ? (const void *) m_pHub : (const void *) m_pRootPort)"
    print "\t    && (m_pHub == 0 || m_nHubPortIndex == g_nPix68kHotplugPort))"
    print "\t{"
    print "\t\tg_bPix68kHotplugConfigured = 1;"
    print "\t}"
    print ""
    n++; infn = 0
  }
  infn && $0 == "}" { exit 1 }
  { print }
  $0 == "static const char FromDevice[] = \"usbdev\";" && !d {
    print ""
    print "// pix68k (#308): the port watched on hot-plug (the root port, or the hub and its port index), and whether"
    print "// a device on it took SET_CONFIGURATION (tools/circle_hotplug_patch.sh)"
    print "const void *g_pPix68kHotplugParent = 0;"
    print "unsigned g_nPix68kHotplugPort = 0;"
    print "volatile int g_bPix68kHotplugConfigured = 0;"
    d = 1
  }
  $0 != "" { prev = $0 }
  END { if (n != 1 || !d) exit 1 }' "$5" > "$6" ||
  { echo "$5: the lines to patch were not found (FromDevice, unsigned nFunction in CUSBDevice::Initialize)" >&2; exit 1; }
