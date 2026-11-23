#!/bin/sh
# Circle の USB-FDD のドライバ (lib/usb/usbfloppydevice.cpp と include/circle/usb/usbfloppydevice.h。CBI の UFI、
# デバイス名 ufd1) を pix68k 用に書き換える (#286)。
#   使い方: tools/circle_ufd_patch.sh <元の .cpp> <書き出す .cpp> <元の .h> <書き出す .h> <元の xhciendpoint.cpp> <書き出す先>
# ベアメタル版のビルド (tools/build_baremetal.sh) が使う。書き換える行が見つからなければ失敗する (Circle を上げたときに気づく)。
#   - ブロックの大きさ: 元は 512 バイトに決め打ちで、X68000 の 2HD (1024 バイト/セクタ) のディスクが入っていると
#     Configure() が "Unsupported block size: 1024" で失敗する。READ CAPACITY の答えが 512 か 1024 なら受け付け、
#     ブロックの番号と数の計算 (BLOCK_SIZE / BLOCK_MASK / BLOCK_SHIFT) を装置ごとの m_nBlockShift から求める。
#     GetBlockSize() を足す (pix68k が形式を確かめるのに使う)
#   - 割り込みで届く 2 バイト: 元は「0 と、コマンドの結果 (0 = 成功)」として見ているが、UFI では ASC と ASCQ が届く
#     (CBI の仕様で、サブクラスが UFI のときの決まり)。電源を入れた直後は、成功した INQUIRY にも 29 00 (電源投入・
#     リセットがあった) が付くので、元のままだと Configure() が "Device does not respond" で失敗する (Y-E DATA の
#     USB-FDU で確かめた)。INQUIRY と REQUEST SENSE ではこの 2 バイトを見ず、ほかのコマンドは ASC が 0 でなければ
#     失敗にする (失敗のあとは、元のとおり REQUEST SENSE でセンスを読む)
#   - 書き込み: Write() と TryWrite() は何もせずに -1 を返す。pix68k は実物のディスクを読むだけで、書かない
#     (WRITE(10) を送る処理を、呼んでも届かないようにしておく)
#   - 読み直し: 元の Read() は、読めなかったときに Reset() で SEND DIAGNOSTIC (自己診断) を送ってから読み直す。
#     pix68k がドライブに送るのは、読み出しと状態の取得 (INQUIRY、TEST UNIT READY、REQUEST SENSE、READ CAPACITY、
#     READ(10)) だけにしたいので、Reset() は何も送らずに 0 を返し、そのまま読み直す
#   - 転送の時間切れ (Pi 4 / Pi 5 の xHCI。lib/usb/xhciendpoint.cpp): 元は、終わるのを待つ転送を 3 秒で打ち切り、
#     打ち切ったあとはそのエンドポイントの転送がすべて失敗する (コントローラの側に転送が残ったままになる)。
#     フロッピーディスクは、モーターが回り出すまで (約 2.7 秒かかった) や、読みにくいセクタをドライブが読み直す間に
#     3 秒を超えることがあり、実際にシリンダ 49 の読み出しで打ち切られて、ディスクを読めなくなった (Pi 5、Y-E DATA の
#     USB-FDU)。USB-FDD のドライバの Configure() と Read() の間、その USB-FDD への転送だけを 30 秒まで待つ
#     (g_pPix68kXHCILongTimeoutDevice に自分の装置を入れる。ほかの装置への転送は、その間も 3 秒のまま)。
#     30 秒まで待つ転送の待ちのループでは、g_pPix68kXHCIWaitHook があれば呼ぶ (pix68k がウォッチドッグを延ばす)
#   - 送るコマンドの制限: Command() の入口で、INQUIRY、TEST UNIT READY、REQUEST SENSE、READ CAPACITY、READ(10) 以外と、
#     装置へデータを送る (データアウトの) 要求を断る。書き込みやフォーマットにあたるコマンドは、どこから呼ばれても送らない
# 7 つ目の引数 (baremetal/ufd_write.inc) があれば (Pi 4 / Pi 5。#386)、上の書き換えを
# 全部したあとで、その中身を .cpp の終わりに足し、.h にメンバを足す。元の関数は読み取り専用のままで、書けるのは足した関数だけ
# 関数の頭に行を足すところは、関数の名前の行のすぐ次が "{" だけの行であることを確かめる (Circle の書き方が変わって
# "{" が別のところに移ったら、次の関数に足してしまわずに失敗する)。
set -eu
[ $# -eq 6 ] || [ $# -eq 7 ] || { echo "usage: $0 <usbfloppydevice.cpp> <out.cpp> <usbfloppydevice.h> <out.h> <xhciendpoint.cpp> <out.cpp> [ufd_write.inc]" >&2; exit 2; }
awk '
  # 関数の名前の行の次の行: "{" だけでなければ失敗。そうなら "{" のあとに ins を足す
  want != "" {
    if ($0 != "{") { bad = 1; exit 1 }
    print
    n = split(ins, L, "\n")
    for (i = 1; i <= n; i++) print L[i]
    print ""
    done[want]++
    want = ""
    next
  }
  $0 == "#define BLOCK_SIZE\t\t512" { print "#define BLOCK_SIZE\t\t(1U << m_nBlockShift)\t// pix68k (#286): 512 or 1024"; n1++; next }
  $0 == "#define BLOCK_SHIFT\t\t9" { print "#define BLOCK_SHIFT\t\tm_nBlockShift"; n2++; next }
  $0 == "\tif (nBlockSize != BLOCK_SIZE)" {
    print "\tm_nBlockShift = nBlockSize == 1024 ? 10 : 9;\t// pix68k (#286)"
    print "\tif (nBlockSize != 512 && nBlockSize != 1024)"
    n3++; next
  }
  $0 == "\t\tif (   Status[0] != 0" {
    print "\t\t// pix68k (#286): UFI returns ASC and ASCQ here, not a command block status. A pending unit attention"
    print "\t\t// (29 00 after power-on) is also reported for INQUIRY, which has succeeded; let REQUEST SENSE through too"
    print "\t\tif (   CmdBuffer[0] != SCSI_OP_INQUIRY"
    print "\t\t    && CmdBuffer[0] != SCSI_REQUEST_SENSE"
    getline
    if ($0 != "\t\t    || Status[1] != STATUS_PASS)") { bad = 1; exit 1 }
    print "\t\t    && Status[0] != 0)"
    n5++; next
  }
  { print }
  $0 == "#include <assert.h>" {
    print ""
    print "// pix68k (#286): a floppy drive may need more than 3 seconds for one transfer (spin-up, retries on a weak sector)."
    print "// While this driver is in Configure() or Read(), transfers to its own device may take up to 30 seconds"
    print "#if RASPPI >= 4"
    print "extern CUSBDevice *g_pPix68kXHCILongTimeoutDevice;\t// lib/usb/xhciendpoint.cpp"
    print "struct TPix68kLongTimeout"
    print "{"
    print "\tCUSBDevice *m_pOld;"
    print "\tTPix68kLongTimeout (CUSBDevice *pDevice) : m_pOld (g_pPix68kXHCILongTimeoutDevice) { g_pPix68kXHCILongTimeoutDevice = pDevice; }"
    print "\t~TPix68kLongTimeout (void) { g_pPix68kXHCILongTimeoutDevice = m_pOld; }"
    print "};"
    print "#define PIX68K_LONG_TIMEOUT\tTPix68kLongTimeout LongTimeout (GetDevice ())"
    print "#else"
    print "#define PIX68K_LONG_TIMEOUT"
    print "#endif"
    n7++
  }
  index($0, "boolean CUSBFloppyDiskDevice::Configure (") == 1 { want = "Configure"; ins = "\tPIX68K_LONG_TIMEOUT;\t// pix68k (#286)" }
  index($0, "int CUSBFloppyDiskDevice::Read (") == 1 { want = "Read"; ins = "\tPIX68K_LONG_TIMEOUT;\t// pix68k (#286)" }
  index($0, "int CUSBFloppyDiskDevice::Write (") == 1 { want = "Write"; ins = "\treturn -1;\t\t// pix68k (#286): read only, never send WRITE(10)" }
  index($0, "int CUSBFloppyDiskDevice::TryWrite (") == 1 { want = "TryWrite"; ins = "\treturn -1;\t\t// pix68k (#286): read only, never send WRITE(10)" }
  index($0, "int CUSBFloppyDiskDevice::Reset (") == 1 { want = "Reset"; ins = "\treturn 0;\t\t// pix68k (#286): no SEND DIAGNOSTIC; just read again" }
  # Command() は名前の行が 2 行に分かれている。1 行目のすぐ次が 2 行目であることも確かめる
  cmd1 {
    if ($0 != "\t\t\t\t   void *pBuffer, size_t nBufLen, boolean bIn)") { bad = 1; exit 1 }
    cmd1 = 0
    want = "Command"
    ins = "\t// pix68k (#286): only these five commands, and no data-out phase, ever reach the drive\n" \
          "\tswitch (*(const u8 *) pCmdBlk)\n" \
          "\t{\n" \
          "\tcase SCSI_OP_INQUIRY:\n" \
          "\tcase SCSI_OP_TEST_UNIT_READY:\n" \
          "\tcase SCSI_REQUEST_SENSE:\n" \
          "\tcase SCSI_OP_READ_CAPACITY:\n" \
          "\tcase SCSI_OP_READ:\n" \
          "\t\tbreak;\n" \
          "\n" \
          "\tdefault:\n" \
          "\t\treturn -1;\n" \
          "\t}\n" \
          "\tif (nBufLen > 0 && !bIn)\n" \
          "\t{\n" \
          "\t\treturn -1;\n" \
          "\t}"
  }
  $0 == "int CUSBFloppyDiskDevice::Command (void *pCmdBlk, size_t nCmdBlkLen," { cmd1 = 1 }
  END {
    if (bad || want != "" || cmd1) exit 1
    if (n1 != 1 || n2 != 1 || n3 != 1 || n5 != 1 || n7 != 1) exit 1
    if (done["Configure"] != 1 || done["Read"] != 1 || done["Write"] != 1 || done["TryWrite"] != 1 ||
        done["Reset"] != 1 || done["Command"] != 1) exit 1
  }' "$1" > "$2" ||
  { echo "$1: the lines to patch were not found, or a function does not start with a line of \"{\" (BLOCK_SIZE, BLOCK_SHIFT, the block size check, the status check, #include <assert.h>, Configure, Read, Write, TryWrite, Reset, Command)" >&2; exit 1; }
awk '
  $0 == "\tu64 GetSize (void) const;" { print; print "\tunsigned GetBlockSize (void) const { return 1U << m_nBlockShift; }\t// pix68k (#286)"; n1++; next }
  $0 == "\tunsigned m_nBlockCount;" { print; print "\tunsigned m_nBlockShift = 9;\t// pix68k (#286): 9 = 512 bytes, 10 = 1024 bytes"; n2++; next }
  { print }
  END { if (n1 != 1 || n2 != 1) exit 1 }' "$3" > "$4" ||
  { echo "$3: the lines to patch were not found (GetSize, m_nBlockCount)" >&2; exit 1; }
awk '
  $0 == "static const char From[] = \"xhciep\";" {
    print
    print ""
    print "// pix68k (#286): transfers to this device may take up to 30 seconds (set by the USB floppy driver while it is in"
    print "// Configure() or Read()); the wait loop of such a transfer calls the hook (pix68k feeds the watchdog there)"
    print "CUSBDevice *g_pPix68kXHCILongTimeoutDevice = 0;"
    print "void (*volatile g_pPix68kXHCIWaitHook) (void) = 0;"
    n1++; next
  }
  $0 == "\t\tif (CTimer::Get ()->GetTicks () - nStartTicks >= 3*HZ)" {
    print "\t\tboolean bPix68kLong =    g_pPix68kXHCILongTimeoutDevice != 0"
    print "\t\t\t\t      && pURB->GetEndpoint ()->GetDevice () == g_pPix68kXHCILongTimeoutDevice;"
    print "\t\tif (bPix68kLong)"
    print "\t\t{"
    print "\t\t\tvoid (*pHook) (void) = g_pPix68kXHCIWaitHook;"
    print "\t\t\tif (pHook != 0)"
    print "\t\t\t{"
    print "\t\t\t\t(*pHook) ();"
    print "\t\t\t}"
    print "\t\t}"
    print ""
    print "\t\tif (CTimer::Get ()->GetTicks () - nStartTicks >= (bPix68kLong ? 30*HZ : 3*HZ))"
    n2++; next
  }
  { print }
  END { if (n1 != 1 || n2 != 1) exit 1 }' "$5" > "$6" ||
  { echo "$5: the lines to patch were not found (From, the 3*HZ timeout)" >&2; exit 1; }
# 書き込みの命令 (SCSI_OP_WRITE) を使うのは TryWrite() だけで、その前に return が入っていること
[ "$(grep -c 'OperationCode[[:space:]]*= SCSI_OP_WRITE;' "$2")" = 1 ] || { echo "$2: SCSI_OP_WRITE is used in more than one place" >&2; exit 1; }
[ $# -eq 7 ] || exit 0
cat "$7" >> "$2"
awk '
  $0 == "#include <circle/types.h>" { print; print "#include \"pix68k_ufi_gate.h\"\t// pix68k (#386)"; n1++; next }
  $0 == "\tu64 GetSize (void) const;" {
    print
    print "\t// pix68k (#386)"
    print "\tboolean Pix68kArmWrite (u32 nBlock, u32 nCount);"
    print "\tint Pix68kCommand (const u8 *pCmdBlk, void *pBuffer, size_t nBufLen, boolean bIn, u8 *pStatus, int *pBytes);"
    n2++; next
  }
  $0 == "\tunsigned m_nBlockCount;" {
    print
    print "\tplatform::UfiGate m_Pix68kGate;\t// pix68k (#386)"
    print "\tboolean m_bPix68kBroken = FALSE;"
    print "\tboolean m_bPix68kArmed = FALSE;"
    print "\tu8 *m_pPix68kBuffer = 0;"
    print "\tu8 *m_pPix68kSmall = 0;"
    n3++; next
  }
  { print }
  END { if (n1 != 1 || n2 != 1 || n3 != 1) exit 1 }' "$4" > "$4.w" ||
  { echo "$3: the lines to patch were not found (types.h, GetSize, m_nBlockCount)" >&2; exit 1; }
mv "$4.w" "$4"
# 転送を断られた (STALL、完了コード 6) ことを、ほかの失敗と見分けられるようにする (制御転送だけ。
# ほかの転送の USBErrorUnknown を見ているドライバがあるため)
awk '
  $0 == "\t\tCLogger::Get ()->Write (From, LogWarning, \"Transfer error %u on endpoint %u\"," {
    print "\t\tif (uchCompletionCode == 6 && pURB->GetEndpoint ()->GetType () == EndpointTypeControl) pURB->SetUSBError (USBErrorStall);\t// pix68k (#386): 6 = Stall Error (xHCI)"
    n++
  }
  { print }
  END { if (n != 1) exit 1 }' "$6" > "$6.w" || { echo "$5: the line to patch was not found (Transfer error)" >&2; exit 1; }
mv "$6.w" "$6"
