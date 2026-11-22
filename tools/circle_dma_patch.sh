#!/bin/sh
# Circle の DMA のチャネル (lib/dmachannel.cpp と lib/dma4channel.cpp) と、PWM / I2S の音のバッファ
# (lib/sound/dmasoundbuffers.cpp) を pix68k 用に書き換える (#296)。
#   使い方: tools/circle_dma_patch.sh <元の dmachannel.cpp> <書き出す先> <元の dma4channel.cpp> <書き出す先> \
#                                     <元の dmasoundbuffers.cpp> <書き出す先>
# ベアメタル版のビルド (tools/build_baremetal.sh) が使う。書き換える行が見つからなければ失敗する (Circle を上げたときに気づく)。
#
# 音の出力 (HDMI) は、2 つのバッファを DMA が順に繰り返し送り (SetupCyclicIOWrite())、1 つ送り終えるたびの割り込みで、
# 送り終えたバッファを次の音で埋める。元の割り込みの処理は、送り終えたバッファの番号を割り込みの回数で数えている
# (m_nCurrentBuffer)。割り込みを 1 回取りこぼす (割り込みを止めている間に DMA が 2 つ送り終える) と番号がずれ、
# そのあとは「いま送っている最中のバッファ」を埋め続ける。DMA はもう先頭の 64 フレームほどを読んだあとなので、
# バッファごとに先頭だけ前の中身が出て、16ms ごとに音が飛ぶ (Pi 5 の HDMI の録音で確かめた。もう 1 回取りこぼすまで続く)。
#
# 直し方: 割り込みのたびに pix68k_dma_refill() (baremetal/kernel.cpp。中身は src/platform/dma_refill.h にあり、ホストで
# 単体試験する) を呼ぶ。これが DMA のレジスタ (CONBLK_AD。いま読み込んでいる制御ブロック) を読んで、いま送っているバッファの
# 1 つ前を「送り終えたバッファ」として返す:
#   - 回数から決めた番号とずれていたら、レジスタの方に合わせ直す (合わせ直した回数を数え、ログに出す。バッファが 2 つなので、
#     取りこぼした回数そのものは分からない)
#   - レジスタの値がどの制御ブロックでもなければ、埋めずに戻る (回数から決めた番号では続けない)
#   - 元の処理は、先に割り込みの要求 (CS の INT) を消してからここに来る。消したあと、レジスタを読む前に DMA が次の
#     バッファを送り終えると、その要求が残り、この処理を出たあとでもう一度割り込みが入って、同じバッファを 2 回
#     「送り終えた」と見てしまう。そこで pix68k_dma_refill() は、レジスタを読んだあとに INT を見て、立っていたら消して
#     読み直す (立たなくなるまで)。DMA は INT を CONBLK_AD より少し先に立てることがあるので、INT が消えたのを見たあとも
#     CONBLK_AD をしばらく読み続け、変わったら読み直す。読み直しの間にエラーが立ったら、INT も消してから 2 を返し、ここで
#     エラーとして知らせる
#   - 埋めたあとにレジスタの値が変わっていたら、埋めている間に DMA がそのバッファを送り始めている (音が 1 回飛ぶ)。
#     これも合わせ直しに数える (pix68k_dma_late())
#   - DMA がエラーを知らせたとき (m_bStatus が FALSE) は、何も判定せず、元のとおり知らせる (知らせを受ける側が止める)
#   - レジスタを読んでから埋め終えるまでに DMA がそのバッファに入ってしまう遅れ (バッファ 1 つ分より長く割り込みを
#     止めたあと) は防げない。そのバッファは 1 回だけ音が飛び、次の割り込みでまたレジスタから決め直す
# バッファが 1 つの転送 (繰り返さない転送) は変えない。
#
# PWM と I2S の音の出力 (lib/sound/dmasoundbuffers.cpp。Pi 3 と Pi 4。Pi 5 は RP1 の別の処理) も、次に埋めるバッファを
# 割り込みの回数で入れ替えている (m_nNextBuffer ^= 1)。GetNextChunk() で、同じように pix68k_dma_refill() に聞く。
# 元の Start() は、DMA を動かしたあとでバッファ 1 を埋める (割り込みの外から GetNextChunk() を呼ぶ)。その途中で割り込みが
# 入ると、古い観測のまま送信中のバッファを埋めたり、m_nNextBuffer が途中で変わったりする。そこで、DMA を動かす前に両方の
# バッファを無音で埋め、動かしたあとには埋めない (出始めの無音が 1 バッファ分、11ms 長くなる)。こうすると GetNextChunk (FALSE)
# は割り込みの処理 (m_SpinLock を取っている) からしか呼ばれない。
set -eu
[ $# -eq 6 ] || { echo "usage: $0 <dmachannel.cpp> <out.cpp> <dma4channel.cpp> <out.cpp> <dmasoundbuffers.cpp> <out.cpp>" >&2; exit 2; }
# $1 元、$2 先、$3 割り込みの処理の関数の名前の行、$4 CONBLK_AD のレジスタ、$5 i 番目の制御ブロックのレジスタでの値、
# $6 CS のレジスタ、$7 CS の INT のビット、$8 CS のエラーのビット
patch() {
  awk -v head="$3" -v reg="$4" -v block="$5" -v cs="$6" -v intbit="$7" -v errbit="$8" '
    $0 == head {
      print "extern \"C\" unsigned pix68k_dma_refill (unsigned nSlot, uintptr nCS, uintptr nBlock, u32 nIntBit, u32 nErrorBit,"
      print "\t\t\t\t\t const u32 *pBlocks, unsigned nBlocks, unsigned nCounted, unsigned *pDone, u32 *pLoaded);\t// pix68k (#296)"
      print "extern \"C\" void pix68k_dma_late (void);"
      print ""
      h++
    }
    index($0, "(*pCompletionRoutine) (m_nChannel, m_nCurrentBuffer, m_bStatus, m_pCompletionParam);") {
      print "\t// pix68k (#296): find the completed buffer from the hardware, not by counting"
      print "\tboolean bPix68kSkip = FALSE;"
      print "\tu32 nPix68kLoaded = 0;"
      print "\tif (m_nBuffers > 1 && m_bStatus)\t// an error is always passed on"
      print "\t{"
      print "\t\tu32 aPix68kBlocks[MaxCyclicBuffers];"
      print "\t\tfor (unsigned i = 0; i < m_nBuffers; i++)"
      print "\t\t{"
      print "\t\t\taPix68kBlocks[i] = " block ";"
      print "\t\t}"
      print "\t\tunsigned nDone = m_nCurrentBuffer;"
      print "\t\tswitch (pix68k_dma_refill (m_nChannel & 15, " cs " (m_nChannel), " reg " (m_nChannel), " intbit ", " errbit ","
      print "\t\t\t\t\t   aPix68kBlocks, m_nBuffers, m_nCurrentBuffer, &nDone, &nPix68kLoaded))"
      print "\t\t{"
      print "\t\tcase 1:"
      print "\t\t\tm_nCurrentBuffer = nDone;"
      print "\t\t\tbreak;"
      print "\t\tcase 2:"
      print "\t\t\tm_bStatus = FALSE;\t// an error since the first look at CS: report it now"
      print "\t\t\tbreak;"
      print "\t\tdefault:"
      print "\t\t\tbPix68kSkip = TRUE;\t// not one of our control blocks: fill nothing"
      print "\t\t\tbreak;"
      print "\t\t}"
      print "\t}"
      print "\tif (!bPix68kSkip)"
      m++
    }
    # 関数の終わり (埋めたバッファのキャッシュを書き出したあと) で、埋めている間に DMA がそのバッファに進んでいないかを見る
    m == 1 && g == 1 && !z && $0 == "}" {
      print ""
      print "\tif (m_nBuffers > 1 && m_bStatus && !bPix68kSkip)\t// pix68k (#296)"
      print "\t{"
      print "\t\tPeripheralEntry ();"
      print "\t\tif (read32 (" reg " (m_nChannel)) != nPix68kLoaded)"
      print "\t\t{"
      print "\t\t\tpix68k_dma_late ();\t// the DMA moved on while the buffer was being filled"
      print "\t\t}"
      print "\t\tPeripheralExit ();"
      print "\t}"
      z++
    }
    # 元の処理は、知らせたあとに m_nCurrentBuffer を 1 つ進める。埋めなかったときは進めない
    m == 1 && !g && index($0, "if (   m_bStatus") { sub(/m_bStatus/, "m_bStatus \\&\\& !bPix68kSkip"); g++ }
    { print }
    END { if (h != 1 || m != 1 || g != 1 || z != 1) exit 1 }' "$1" > "$2" ||
    { echo "$3, its completion call or the buffer advance not found once in $1" >&2; exit 1; }
}
patch "$1" "$2" 'void CDMAChannel::InterruptHandler (void)' \
  ARM_DMACHAN_CONBLK_AD 'BUS_ADDRESS ((uintptr) m_pControlBlock[i])' \
  ARM_DMACHAN_CS CS_INT CS_ERROR
patch "$3" "$4" 'void CDMA4Channel::InterruptHandler (void)' \
  ARM_DMA4CHAN_CONBLK_AD '(u32) ((uintptr) m_pControlBlock[i] >> CONBLK_AD4_ADDR_SHIFT)' \
  ARM_DMA4CHAN_CS CS4_INT CS4_ERROR
# dmasoundbuffers.cpp:
#   Start(): DMA を動かす前に、バッファ 0 と 1 を無音で埋める。動かしたあとのバッファ 1 の埋め (割り込みの外) はやめる
#   GetNextChunk(): 頭 (最初の埋めを除く。割り込みの処理からだけ呼ばれる) で、pix68k_dma_refill() に埋めるバッファを聞く。
#   埋めないとき (知らない制御ブロック) は、TRUE を返して続ける
awk '
  $0 == "boolean CDMASoundBuffers::Start (TChunkCompletedHandler *pHandler, void *pParam)" { s = 1 }
  s == 1 && $0 == "\t    && !GetNextChunk (TRUE))" {
    print "\t    && !(GetNextChunk (TRUE) && GetNextChunk (TRUE)))\t// pix68k (#296): fill both buffers before starting the DMA"
    s = 2
    next
  }
  # 動かしたあとのバッファ 1 の埋め: 元の行と一字一句同じときだけ消す
  s == 2 && $0 == "\t// fill buffer 1" { s = 3; buf = ""; next }
  s == 3 {
    buf = buf $0 "\n"
    if ($0 == "\treturn TRUE;") {
      want1 = "\tif (   m_bDirectionOut\n\t    && !GetNextChunk (FALSE))\n\t{\n\t\tm_SpinLock.Acquire ();\n\n\t\tif (m_State == StateRunning)\n\t\t{\n\t\t\tPeripheralEntry ();\n\t\t\twrite32 (ARM_DMACHAN_NEXTCONBK (m_nDMAChannel), 0);\n\t\t\tPeripheralExit ();\n\n\t\t\tm_State = StateTerminating;\n\t\t}\n\n\t\tm_SpinLock.Release ();\n\t}\n\n\treturn TRUE;\n"
      if (buf != want1) { bad = 1; exit 1 }
      print "\t// pix68k (#296): buffer 1 was filled before starting the DMA. Filling it here, outside the interrupt"
      print "\t// handler, could race with the first completion"
      print ""
      print "\treturn TRUE;"
      s = 4
    }
    next
  }
  $0 == "boolean CDMASoundBuffers::GetNextChunk (boolean bFirstCall)" {
    print "extern \"C\" unsigned pix68k_dma_refill (unsigned nSlot, uintptr nCS, uintptr nBlock, u32 nIntBit, u32 nErrorBit,"
    print "\t\t\t\t\t const u32 *pBlocks, unsigned nBlocks, unsigned nCounted, unsigned *pDone, u32 *pLoaded);\t// pix68k (#296)"
    print ""
    want = 1
    h++
    print
    next
  }
  want {
    if ($0 != "{") { bad = 1; exit 1 }
    print
    print "\tif (!bFirstCall)\t// pix68k (#296): fill the buffer that is not being sent, whatever the count says"
    print "\t{"
    print "\t\t// only the interrupt handler gets here (Start() fills both buffers before starting the DMA)"
    print "\t\tconst u32 aBlocks[2] = {(u32) BUS_ADDRESS ((uintptr) m_pControlBlock[0]), (u32) BUS_ADDRESS ((uintptr) m_pControlBlock[1])};"
    print "\t\tunsigned nDone = m_nNextBuffer;"
    print "\t\tu32 nLoaded;"
    print "\t\tconst unsigned nResult = pix68k_dma_refill (16, ARM_DMACHAN_CS (m_nDMAChannel), ARM_DMACHAN_CONBLK_AD (m_nDMAChannel),"
    print "\t\t\t\t\t\t\t    CS_INT, CS_ERROR, aBlocks, 2, m_nNextBuffer, &nDone, &nLoaded);"
    print "\t\tif (nResult == 2)"
    print "\t\t{"
    print "\t\t\tm_State = StateFailed;\t// an error since the first look at CS: report it as InterruptHandler() does"
    print "\t\t\tassert (m_pHandler != 0);"
    print "\t\t\t(*m_pHandler) (FALSE, 0, 0, m_pParam);"
    print "\t\t\treturn TRUE;"
    print "\t\t}"
    print "\t\tif (nResult == 0)"
    print "\t\t{"
    print "\t\t\treturn TRUE;\t// not one of our control blocks: fill nothing"
    print "\t\t}"
    print "\t\tm_nNextBuffer = nDone;"
    print "\t}"
    print ""
    want = 0
    m++
    next
  }
  { print }
  END { if (bad || h != 1 || m != 1 || s != 4) exit 1 }' "$5" > "$6" ||
  { echo "CDMASoundBuffers::Start() or GetNextChunk() not found once in $5" >&2; exit 1; }
