#!/bin/sh
# Circle の USB シリアルのドライバ (lib/usb/usbserialhost.cpp と include/circle/usb/usbserialhost.h。CDC は
# これを継いだ CUSBSerialCDCDevice) に、pix68k が FUK (x68k/fuk.h。#295) とやり取りするための口を足す。
#   使い方: tools/circle_fuk_patch.sh <元の .cpp> <書き出す .cpp> <元の .h> <書き出す .h> [<元の xhciusbdevice.cpp> <書き出す先>]
# ベアメタル版のビルド (tools/build_baremetal.sh) が使う。書き換える行が見つからなければ失敗する (Circle を上げたときに気づく)。
# 元のドライバは、送信は終わるまで待ち (Write())、受信は呼ばれたときに見に行く (Read())。コア 0 の主ループから
# 呼ぶと、FUK のレジスタの読み出し 1 回に主ループの 1 周 (2ms 以上) かかる。そこで次を足す (元の関数は変えない):
#   - Pix68kStartReceive(): 受信の転送を出したままにする。届くたびに、割り込みの中から受け取りの関数を呼んで、
#     すぐ次の転送を出す。1 回の転送は Pix68kBufferSize バイトまで (満杯か、短いパケットで終わる)。
#     転送が失敗したら (抜いたときなど)、長さ 0 で受け取りの関数を呼んで止まる
#   - Pix68kSend(): 終わるのを待たずに送る (Pix68kBufferSize バイトまで)。送り終えたら、割り込みの中から
#     送り終わりの関数を呼ぶ。前の送信が終わる前に呼んだら false
#   - Pix68kGet(): Configure() が済んだ装置の一覧 (pix68k が VID と PID で FUK を探す)。装置が消えるときは、
#     消える前に「外した」の関数を呼ぶ。転送の置き場 (装置 1 つで 1280 バイト) は、装置が消えても解放しない
#     (抜いたときに出したままだった転送が、あとからコントローラの側で終わっても、解放した場所に書かないように)
#   - Pix68kStop(): これ以上転送を出さず、ハンドラも呼ばないようにする (装置を外すとき)
# どれもコア 0 から (割り込みの中か、割り込みを止めて) 呼ぶこと。
# 5 つ目と 6 つ目の引数を渡すと、xHCI のドライバ (lib/usb/xhciusbdevice.cpp。Pi 4 / Pi 5) も書き換える:
#   - 装置のデストラクタ (CXHCIUSBDevice::~CXHCIUSBDevice()) の最初に、g_pPix68kXHCIDeviceRemoving があれば呼ぶ。
#     このデストラクタはスロットを無効にしてスロットの番号を 0 にし、そのあと (基底のデストラクタで) USB シリアルの
#     ドライバを消す。その間にほかのコアからの頼みで転送を出すと、無効なスロットに出すことになる (assert で止まる)。
#     pix68k は、スロットを無効にする前に知らせを受けて、転送を出すのをやめる
set -eu
[ $# -eq 4 ] || [ $# -eq 6 ] || { echo "usage: $0 <usbserialhost.cpp> <out.cpp> <usbserialhost.h> <out.h> [<xhciusbdevice.cpp> <out.cpp>]" >&2; exit 2; }
awk '
  $0 == "private:" && !done {
    print "public:\t// pix68k (#295): asynchronous transfers (see tools/circle_fuk_patch.sh)"
    print "\tstatic const unsigned Pix68kBufferSize = 512;"
    print "\tstatic const unsigned Pix68kMaxDevices = 8;"
    print "\ttypedef void TPix68kReceiveHandler (const u8 *pData, unsigned nLength, void *pParam);"
    print "\ttypedef void TPix68kSentHandler (boolean bOK, void *pParam);"
    print "\ttypedef void TPix68kRemovedHandler (void *pParam);"
    print "\tstatic CUSBSerialHostDevice *Pix68kGet (unsigned nIndex);"
    print "\tvoid Pix68kSetHandlers (TPix68kReceiveHandler *pReceive, TPix68kSentHandler *pSent,"
    print "\t\t\t\tTPix68kRemovedHandler *pRemoved, void *pParam);"
    print "\tboolean Pix68kStartReceive (void);"
    print "\tboolean Pix68kReceiving (void) const\t{ return m_bPix68kReceiving; }"
    print "\tboolean Pix68kSend (const void *pData, unsigned nLength);"
    print "\tvoid Pix68kStop (void);"
    print ""
    print "private:"
    print "\tvoid Pix68kRegister (void);"
    print "\tvoid Pix68kUnregister (void);"
    print "\tstatic void Pix68kReceiveStub (CUSBRequest *pURB, void *pParam, void *pContext);"
    print "\tstatic void Pix68kSentStub (CUSBRequest *pURB, void *pParam, void *pContext);"
    print ""
    print "\tTPix68kReceiveHandler *m_pPix68kReceive = 0;"
    print "\tTPix68kSentHandler *m_pPix68kSent = 0;"
    print "\tTPix68kRemovedHandler *m_pPix68kRemoved = 0;"
    print "\tvoid *m_pPix68kParam = 0;"
    print "\tu8 *m_pPix68kAlloc[2] = {0, 0};"
    print "\tu8 *m_pPix68kRx = 0;"
    print "\tu8 *m_pPix68kTx = 0;"
    print "\tvolatile boolean m_bPix68kReceiving = FALSE;"
    print "\tvolatile boolean m_bPix68kSending = FALSE;"
    print "\tboolean m_bPix68kStopped = FALSE;"
    print ""
    done = 1
  }
  { print }
  END { if (!done) exit 1 }' "$3" > "$4" || { echo "private: not found in $3" >&2; exit 1; }
awk '
  want {
    if ($0 != "{") { bad = 1; exit 1 }
    print
    print "\tPix68kUnregister ();\t// pix68k (#295)"
    print ""
    want = 0; n1++
    next
  }
  $0 == "CUSBSerialHostDevice::~CUSBSerialHostDevice (void)" { want = 1 }
  { print }
  $0 == "\tm_pInterface->RegisterSetLinePropertiesHandler (SetLinePropertiesHandler, this);" {
    print ""
    print "\tPix68kRegister ();\t// pix68k (#295)"
    n2++
  }
  END {
    if (bad || n1 != 1 || n2 != 1) exit 1
    print ""
    print "// ---- pix68k (#295): asynchronous transfers (see tools/circle_fuk_patch.sh) ----"
    print ""
    print "static CUSBSerialHostDevice *s_pPix68kDevice[CUSBSerialHostDevice::Pix68kMaxDevices];"
    print ""
    print "CUSBSerialHostDevice *CUSBSerialHostDevice::Pix68kGet (unsigned nIndex)"
    print "{"
    print "\treturn nIndex < Pix68kMaxDevices ? s_pPix68kDevice[nIndex] : 0;"
    print "}"
    print ""
    print "void CUSBSerialHostDevice::Pix68kRegister (void)"
    print "{"
    print "\tfor (unsigned i = 0; i < Pix68kMaxDevices; i++)"
    print "\t{"
    print "\t\tif (s_pPix68kDevice[i] == 0)"
    print "\t\t{"
    print "\t\t\ts_pPix68kDevice[i] = this;"
    print "\t\t\treturn;"
    print "\t\t}"
    print "\t}"
    print "}"
    print ""
    print "void CUSBSerialHostDevice::Pix68kUnregister (void)"
    print "{"
    print "\tfor (unsigned i = 0; i < Pix68kMaxDevices; i++)"
    print "\t{"
    print "\t\tif (s_pPix68kDevice[i] == this)"
    print "\t\t{"
    print "\t\t\ts_pPix68kDevice[i] = 0;"
    print "\t\t}"
    print "\t}"
    print "\tif (m_pPix68kRemoved != 0)"
    print "\t{"
    print "\t\t(*m_pPix68kRemoved) (m_pPix68kParam);"
    print "\t}"
    print "\tm_pPix68kReceive = 0;"
    print "\tm_pPix68kSent = 0;"
    print "\tm_pPix68kRemoved = 0;"
    print "\t// The transfer buffers are not freed: a transfer that was pending when the device went away may"
    print "\t// still be completed by the host controller after this, and must not write to freed memory"
    print "\t// (1280 bytes stay allocated for each removal)."
    print "\tm_pPix68kRx = m_pPix68kTx = 0;"
    print "}"
    print ""
    print "// The device is going away: submit no more transfers and call no more handlers"
    print "void CUSBSerialHostDevice::Pix68kStop (void)"
    print "{"
    print "\tm_bPix68kStopped = TRUE;"
    print "\tm_pPix68kReceive = 0;"
    print "\tm_pPix68kSent = 0;"
    print "\tm_pPix68kRemoved = 0;"
    print "}"
    print ""
    print "void CUSBSerialHostDevice::Pix68kSetHandlers (TPix68kReceiveHandler *pReceive, TPix68kSentHandler *pSent,"
    print "\t\t\t\t\t      TPix68kRemovedHandler *pRemoved, void *pParam)"
    print "{"
    print "\tm_pPix68kReceive = pReceive;"
    print "\tm_pPix68kSent = pSent;"
    print "\tm_pPix68kRemoved = pRemoved;"
    print "\tm_pPix68kParam = pParam;"
    print "\tif (m_pPix68kRx == 0)"
    print "\t{"
    print "\t\t// the transfer buffers must not share a cache line with other data"
    print "\t\tfor (unsigned i = 0; i < 2; i++)"
    print "\t\t{"
    print "\t\t\tm_pPix68kAlloc[i] = new u8[Pix68kBufferSize + 128];"
    print "\t\t}"
    print "\t\tm_pPix68kRx = (u8 *) (((uintptr) m_pPix68kAlloc[0] + 63) & ~(uintptr) 63);"
    print "\t\tm_pPix68kTx = (u8 *) (((uintptr) m_pPix68kAlloc[1] + 63) & ~(uintptr) 63);"
    print "\t}"
    print "}"
    print ""
    print "boolean CUSBSerialHostDevice::Pix68kStartReceive (void)"
    print "{"
    print "\tif (m_pEndpointIn == 0 || m_pPix68kRx == 0 || m_bPix68kStopped)"
    print "\t{"
    print "\t\treturn FALSE;"
    print "\t}"
    print ""
    print "\tCUSBRequest *pURB = new CUSBRequest (m_pEndpointIn, m_pPix68kRx, Pix68kBufferSize);"
    print "\tassert (pURB != 0);"
    print "\tpURB->SetCompletionRoutine (Pix68kReceiveStub, 0, this);"
    print "\tm_bPix68kReceiving = TRUE;"
    print "\tif (!GetHost ()->SubmitAsyncRequest (pURB))"
    print "\t{"
    print "\t\tm_bPix68kReceiving = FALSE;"
    print "\t\tdelete pURB;"
    print ""
    print "\t\treturn FALSE;"
    print "\t}"
    print ""
    print "\treturn TRUE;"
    print "}"
    print ""
    print "void CUSBSerialHostDevice::Pix68kReceiveStub (CUSBRequest *pURB, void *pParam, void *pContext)"
    print "{"
    print "\tCUSBSerialHostDevice *pThis = static_cast <CUSBSerialHostDevice *> (pContext);"
    print "\tassert (pThis != 0);"
    print "\tassert (pURB != 0);"
    print ""
    print "\tboolean bOK = pURB->GetStatus () != 0;"
    print "\tunsigned nLength = bOK ? pURB->GetResultLength () : 0;"
    print "\tdelete pURB;"
    print ""
    print "\tif (bOK)"
    print "\t{"
    print "\t\tif (nLength > 0 && pThis->m_pPix68kReceive != 0)"
    print "\t\t{"
    print "\t\t\t(*pThis->m_pPix68kReceive) (pThis->m_pPix68kRx, nLength, pThis->m_pPix68kParam);"
    print "\t\t}"
    print "\t\tbOK = pThis->Pix68kStartReceive ();"
    print "\t}"
    print "\tif (!bOK)"
    print "\t{"
    print "\t\tpThis->m_bPix68kReceiving = FALSE;"
    print "\t\tif (pThis->m_pPix68kReceive != 0)"
    print "\t\t{"
    print "\t\t\t(*pThis->m_pPix68kReceive) (0, 0, pThis->m_pPix68kParam);"
    print "\t\t}"
    print "\t}"
    print "}"
    print ""
    print "boolean CUSBSerialHostDevice::Pix68kSend (const void *pData, unsigned nLength)"
    print "{"
    print "\tif (   m_pEndpointOut == 0 || m_pPix68kTx == 0 || m_bPix68kSending || m_bPix68kStopped"
    print "\t    || nLength == 0 || nLength > Pix68kBufferSize)"
    print "\t{"
    print "\t\treturn FALSE;"
    print "\t}"
    print ""
    print "\tmemcpy (m_pPix68kTx, pData, nLength);"
    print "\tCUSBRequest *pURB = new CUSBRequest (m_pEndpointOut, m_pPix68kTx, nLength);"
    print "\tassert (pURB != 0);"
    print "\tpURB->SetCompletionRoutine (Pix68kSentStub, 0, this);"
    print "\tm_bPix68kSending = TRUE;"
    print "\tif (!GetHost ()->SubmitAsyncRequest (pURB))"
    print "\t{"
    print "\t\tm_bPix68kSending = FALSE;"
    print "\t\tdelete pURB;"
    print ""
    print "\t\treturn FALSE;"
    print "\t}"
    print ""
    print "\treturn TRUE;"
    print "}"
    print ""
    print "void CUSBSerialHostDevice::Pix68kSentStub (CUSBRequest *pURB, void *pParam, void *pContext)"
    print "{"
    print "\tCUSBSerialHostDevice *pThis = static_cast <CUSBSerialHostDevice *> (pContext);"
    print "\tassert (pThis != 0);"
    print "\tassert (pURB != 0);"
    print ""
    print "\tboolean bOK = pURB->GetStatus () != 0;"
    print "\tdelete pURB;"
    print "\tpThis->m_bPix68kSending = FALSE;"
    print "\tif (pThis->m_pPix68kSent != 0)"
    print "\t{"
    print "\t\t(*pThis->m_pPix68kSent) (bOK, pThis->m_pPix68kParam);"
    print "\t}"
    print "}"
  }' "$1" > "$2" || { echo "the lines to patch were not found in $1" >&2; exit 1; }
if [ $# -eq 6 ]; then
  awk '
    want {
      if ($0 != "{") { bad = 1; exit 1 }
      print
      print "\t// pix68k (#295): tell the user of the device before the slot is disabled (see tools/circle_fuk_patch.sh)"
      print "\tvoid (*pRemoving) (CUSBDevice *) = g_pPix68kXHCIDeviceRemoving;"
      print "\tif (pRemoving != 0)"
      print "\t{"
      print "\t\t(*pRemoving) (this);"
      print "\t}"
      print ""
      want = 0; n++
      next
    }
    $0 == "CXHCIUSBDevice::~CXHCIUSBDevice (void)" {
      print "void (*volatile g_pPix68kXHCIDeviceRemoving) (CUSBDevice *pDevice) = 0;\t// pix68k (#295)"
      print ""
      want = 1
    }
    { print }
    END { if (bad || n != 1) exit 1 }' "$5" > "$6" || { echo "CXHCIUSBDevice::~CXHCIUSBDevice() not found in $5" >&2; exit 1; }
fi
