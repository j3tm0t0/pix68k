#!/bin/sh
# Circle の DMA の書き換え (tools/circle_dma_patch.sh、#296) が、狙った所に 1 回ずつ入ることを確かめる。
#   使い方: tests/tools/circle_dma_patch_test.sh <リポジトリの場所>
# ベアメタル版は ctest でビルドできないので、書き換えた結果の文字列を見る。
#   1. 3 つのファイル (dmachannel.cpp、dma4channel.cpp、sound/dmasoundbuffers.cpp) とも、埋めるかを pix68k_dma_refill() に
#      1 回聞くこと。エラーの知らせは、その判定を通らないこと。PWM / I2S の Start() が、DMA を動かす前に両方のバッファを
#      埋めること (レジスタの読み書きと判定は platform/dma_refill.h にあり、dma_refill_test が試験する)
#   2. 書き換える行がなくなったとき、関数の "{" が名前と同じ行に移ったときに、スクリプトが失敗すること
# submodule の external/circle がなければ飛ばす (終了コード 77)
set -u
ROOT=$1
C=$ROOT/external/circle
[ -f "$C/lib/dma4channel.cpp" ] || { echo "skip: external/circle がない"; exit 77; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
ok() { echo "ok: $1"; }
ng() { echo "NG: $1"; fail=1; }
count() { grep -cF -- "$2" "$1" || true; }
run() { sh "$ROOT/tools/circle_dma_patch.sh" "$1" "$T/a.cpp" "$2" "$T/b.cpp" "$3" "$T/c.cpp" 2>/dev/null; }

SRC1=$C/lib/dmachannel.cpp SRC2=$C/lib/dma4channel.cpp SRC3=$C/lib/sound/dmasoundbuffers.cpp
if run "$SRC1" "$SRC2" "$SRC3"; then ok '書き換えが通る'; else ng '書き換えが失敗した'; fi
# $1: 説明、$2: 書き換えたあと、$3: pix68k_dma_refill() に渡す CS と CONBLK_AD のレジスタ
check() {
  # 埋めるかどうかは pix68k_dma_refill() (中身は platform/dma_refill.h の dmaRefillOnInterrupt()。INT を見て読み直す所も含めて
  # dma_refill_test がホストで試験する) に 1 回だけ聞く
  [ "$(count "$2" 'pix68k_dma_refill (')" -eq 2 ] && ok "$1: pix68k_dma_refill() の宣言と、呼ぶ所が 1 つずつ" || ng "$1: pix68k_dma_refill() の数が違う"
  [ "$(count "$2" "$3")" -eq 1 ] && ok "$1: CS と CONBLK_AD のレジスタを渡す" || ng "$1: '$3' がない"
  [ "$(count "$2" 's_nLast')" -eq 0 ] && [ "$(count "$2" 'nAgain')" -eq 0 ] && ok "$1: 時間の判定と、読み直しのループが残っていない" || ng "$1: 前の作りが残っている"
}
check dmachannel "$T/a.cpp" 'ARM_DMACHAN_CS (m_nChannel), ARM_DMACHAN_CONBLK_AD (m_nChannel), CS_INT, CS_ERROR,'
check dma4channel "$T/b.cpp" 'ARM_DMA4CHAN_CS (m_nChannel), ARM_DMA4CHAN_CONBLK_AD (m_nChannel), CS4_INT, CS4_ERROR,'
check dmasoundbuffers "$T/c.cpp" 'ARM_DMACHAN_CS (m_nDMAChannel), ARM_DMACHAN_CONBLK_AD (m_nDMAChannel),'
# 読み直しの間のエラー (pix68k_dma_refill() が 2 を返す) は、その場でエラーとして知らせる
for f in a b; do
  [ "$(count "$T/$f.cpp" 'm_bStatus = FALSE;	// an error since the first look at CS')" -eq 1 ] && ok "$f.cpp: 読み直しの間のエラーを知らせる" || ng "$f.cpp: 読み直しの間のエラーを落とす"
done
[ "$(count "$T/c.cpp" '(*m_pHandler) (FALSE, 0, 0, m_pParam);')" -eq $(( $(count "$SRC3" '(*m_pHandler) (FALSE, 0, 0, m_pParam);') + 1 )) ] &&
  ok 'c.cpp: 読み直しの間のエラーを知らせる' || ng 'c.cpp: 読み直しの間のエラーを落とす'
# PWM / I2S: Start() は DMA を動かす前に両方のバッファを埋め、動かしたあとには埋めない。GetNextChunk (FALSE) を呼ぶのは割り込みの処理だけ
[ "$(count "$T/c.cpp" '&& !(GetNextChunk (TRUE) && GetNextChunk (TRUE)))')" -eq 1 ] && ok 'c.cpp: DMA を動かす前に両方のバッファを埋める' || ng 'c.cpp: 動かす前に両方を埋めていない'
[ "$(count "$T/c.cpp" 'GetNextChunk (FALSE)')" -eq 1 ] && [ "$(count "$T/c.cpp" 'm_SpinLock.Acquire ();')" -eq $(( $(count "$SRC3" 'm_SpinLock.Acquire ();') - 1 )) ] &&
  awk '/^void CDMASoundBuffers::InterruptHandler/ { i = 1 } i && /GetNextChunk \(FALSE\)/ { f = 1 } END { exit !f }' "$T/c.cpp" &&
  ok 'c.cpp: GetNextChunk (FALSE) を呼ぶのは割り込みの処理だけ' || ng 'c.cpp: Start() がまだ DMA を動かしたあとで埋めている'
# 埋めている間に DMA が進んだら数える (HDMI の側)
for f in a b; do
  [ "$(count "$T/$f.cpp" 'pix68k_dma_late ();')" -eq 1 ] && ok "$f.cpp: 埋めるのが間に合わなかったら数える" || ng "$f.cpp: pix68k_dma_late() を呼ぶ所がない"
done
# DMA のエラーは、判定を通さずに必ず知らせる (判定は m_bStatus が TRUE のときだけ)
for f in a b; do
  [ "$(count "$T/$f.cpp" 'if (m_nBuffers > 1 && m_bStatus)')" -eq 1 ] && [ "$(count "$T/$f.cpp" 'if (m_nBuffers > 1 && m_bStatus && !bPix68kSkip)')" -eq 1 ] &&
    ok "$f.cpp: エラーのときは判定しない" || ng "$f.cpp: エラーのときも判定している"
done
# PWM / I2S: 元の割り込みの処理のエラーの知らせ (m_pHandler に FALSE) は、GetNextChunk() より前にある
awk '/^void CDMASoundBuffers::InterruptHandler/ { i = 1 } i && /\(\*m_pHandler\) \(FALSE, 0, 0, m_pParam\);/ && !e { e = NR } i && /if \(GetNextChunk \(FALSE\)\)/ && !g { g = NR } END { exit !(e && g && e < g) }' "$T/c.cpp" &&
  ok 'c.cpp: エラーの知らせは判定より前' || ng 'c.cpp: エラーの知らせの位置が変わった'
# 埋めなかったときは、番号を進めない / 知らせない
for f in a b; do
  [ "$(count "$T/$f.cpp" 'm_bStatus && !bPix68kSkip')" -eq 2 ] && [ "$(count "$T/$f.cpp" 'if (!bPix68kSkip)')" -eq 1 ] &&
    ok "$f.cpp: 埋めないときは知らせず、番号も進めない" || ng "$f.cpp: 埋めないときの扱いがない"
done

# 書き換える行がなくなったら失敗する
grep -vF '(*pCompletionRoutine) (m_nChannel, m_nCurrentBuffer, m_bStatus, m_pCompletionParam);' "$SRC2" > "$T/no_call.cpp"
if run "$SRC1" "$T/no_call.cpp" "$SRC3"; then ng '知らせる行がないのに通った'; else ok '知らせる行がなければ失敗する'; fi
sed 's/^void CDMAChannel::InterruptHandler (void)$/void CDMAChannel::InterruptHandler (void) {/' "$SRC1" > "$T/moved.cpp"
if run "$T/moved.cpp" "$SRC2" "$SRC3"; then ng '関数の名前の行が変わったのに通った'; else ok '関数の名前の行が変われば失敗する'; fi
awk '$0 == "boolean CDMASoundBuffers::GetNextChunk (boolean bFirstCall)" { print $0 " {"; skip = 1; next } skip { skip = 0; next } { print }' \
  "$SRC3" > "$T/brace.cpp"
if run "$SRC1" "$SRC2" "$T/brace.cpp"; then ng 'GetNextChunk() の { が同じ行にあるのに通った'; else ok 'GetNextChunk() の { が移れば失敗する'; fi
# Start() の、DMA を動かしたあとでバッファ 1 を埋める所が変わったら失敗する
sed 's/^\t\tm_SpinLock.Release ();$/\t\tm_SpinLock.Release (); \/\/ changed/' "$SRC3" > "$T/start.cpp"
if run "$SRC1" "$SRC2" "$T/start.cpp"; then ng 'Start() のバッファ 1 の埋めが変わったのに通った'; else ok 'Start() のバッファ 1 の埋めが変われば失敗する'; fi
exit $fail
