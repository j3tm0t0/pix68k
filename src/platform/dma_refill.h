// 音の出力の DMA で、割り込みのたびに「どのバッファを埋めるか」を決める (#296)。ベアメタル版の Circle の DMA の割り込み処理
// (tools/circle_dma_patch.sh が書き換えたもの) が、baremetal/kernel.cpp の pix68k_dma_refill() を通して使う。
// レジスタの読み書きは呼ぶ側から関数で受け取るので、ホストでも同じコードを試験できる (tests/unit/dma_refill_test.cpp)。
//
// 音は、2 つ (HDMI は 2〜4 つ) のバッファを DMA が順に繰り返し送り、1 つ送り終えるたびの割り込みで、送り終えた方を次の音で
// 埋める。元の Circle は送り終えたバッファを割り込みの回数で数えているので、割り込みを 1 回取りこぼすと番号がずれ、そのあとは
// いま送っている最中のバッファを埋め続けていた。ここでは、DMA のレジスタ (CONBLK_AD。いま読み込んでいる制御ブロック) を読んで、
// その 1 つ前のバッファを「送り終えた」とする。
//
// 同じ送り終わりを 2 度数えないために、レジスタを読んだあとで CS の INT (割り込みの要求) を見て、立っていたら消して読み直す
// (立たなくなるまで)。こうすると、前に呼んだときから今までに DMA は少なくとも 1 つのバッファを送り終えているので、時間を
// 見なくても決まる:
//   - loaded がどの制御ブロックでもない: 埋めない。合わせ直しに数える
//   - loaded が前に呼んだときと同じ: DMA は一回りして戻ってきた (割り込みを取りこぼした)。送り終えた方を埋め、合わせ直しに数える
//   - loaded が前と違う: 埋める。割り込みの回数から決めた番号と違っていれば、合わせ直しに数える
// 前提: DMA は、CONBLK_AD を次の制御ブロックに変えるのと同時か、それより少し前に INT を立てる。Pi 3 の実機で測った
// (PR #306)。INT が先に立つ分は、INT が消えたのを見たあとも CONBLK_AD をしばらく読み続けて吸収する (DMA_REFILL_SETTLE_READS)。
// 逆に CONBLK_AD が先に変わると、読み直しのあとで INT が立ち、その割り込みで同じ送り終わりをもう一度数えて、同じバッファを
// 2 回埋める (試験で確かめている)。INT が読み続ける時間より長く先に立つと、DMA が送り始める直前のバッファを埋めることになり、
// 間に合わないことがある。
// 読み直しの間に CS の ERROR が立ったら、INT も消してから (消さないと、エラーを知らせて止めたあとにもう一度割り込みが入る)
// エラーとして返す。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stdint.h>

namespace platform {

struct DmaRefillState {
    uint32_t lastLoaded = 0; // 前に呼んだときのレジスタの値
    bool any = false;        // 1 度でも呼んだか
};

static constexpr unsigned DMA_REFILL_FILL = 1;   // 埋める
static constexpr unsigned DMA_REFILL_RESYNC = 2; // 合わせ直しに数える
static constexpr unsigned DMA_REFILL_ERROR = 4;  // 読み直しの間に DMA がエラーを知らせた (ほかのビットは立てない)
// INT が消えたのを見たあと、CONBLK_AD が変わらないことを確かめるために読む回数。Pi 3 では 1 回の読み出しが約 70ns なので、
// 8 回で約 0.5us (INT から CONBLK_AD が変わるまでの遅れの、測った最大の倍ほど)
static constexpr unsigned DMA_REFILL_SETTLE_READS = 8;

// 決めるだけの部分。返すのは DMA_REFILL_FILL と DMA_REFILL_RESYNC の組み合わせ。loaded が制御ブロックなら、st に覚える
inline unsigned dmaRefillStep(DmaRefillState& st, bool known, uint32_t loaded, bool countAgrees) {
    if (!known) return DMA_REFILL_RESYNC;
    const bool same = st.any && loaded == st.lastLoaded;
    st.lastLoaded = loaded;
    st.any = true;
    return DMA_REFILL_FILL | ((same || !countAgrees) ? DMA_REFILL_RESYNC : 0);
}

// 割り込みの処理から呼ぶ。元の処理が INT を消し、ERROR を見たあとで呼ぶ。
//   regs     uint32_t readBlock() (CONBLK_AD)、uint32_t readCs()、void writeCs(uint32_t) を持つもの
//   blocks   i 番目のバッファの制御ブロックの、CONBLK_AD での値 (n 個)
//   counted  割り込みの回数から決めた「送り終えたバッファ」の番号
//   done     DMA_REFILL_FILL のとき、埋めるバッファの番号 (送り終えたバッファ)
//   loaded   最後に読んだ CONBLK_AD (呼ぶ側が、埋め終えたあとに DMA が進んでいないかを見るのに使う)
// 返すのは DMA_REFILL_FILL、DMA_REFILL_RESYNC、DMA_REFILL_ERROR の組み合わせ
// Word は 32 ビットの符号なし整数 (Circle の u32 と uint32_t は、32 ビットの Arm では別の型になる)
template <class Regs, class Word>
inline unsigned dmaRefillOnInterrupt(DmaRefillState& st, Regs& regs, Word intBit, Word errorBit, const Word* blocks,
                                     unsigned n, unsigned counted, unsigned* done, Word* loaded) {
    static_assert(sizeof(Word) == 4, "a 32-bit register value");
    Word block;
    for (;;) {
        block = regs.readBlock();
        const Word cs = regs.readCs();
        if (cs & errorBit) {
            if (cs & intBit) regs.writeCs(cs); // INT は 1 を書いて消す
            *loaded = block;
            return DMA_REFILL_ERROR;
        }
        if (!(cs & intBit)) {
            // 読んでから今まで、送り終えたバッファはない。ただし DMA は、INT を立ててから少しあと (Pi 3 で測って 0.3us 未満) に
            // CONBLK_AD を変える。その間に INT を消していた (元の処理が消したものを含む) と、ここで古い値を読んでいる。
            // しばらく読み続けて、変わったら初めから読み直す
            bool moved = false;
            for (unsigned k = 0; k < DMA_REFILL_SETTLE_READS && !moved; k++) moved = regs.readBlock() != block;
            if (!moved) break;
            continue;
        }
        regs.writeCs(cs); // 読んでいる間に 1 つ送り終えた: その分もここで受け取り、読み直す
    }
    *loaded = block;
    unsigned active = n;
    for (unsigned i = 0; i < n; i++)
        if (block == blocks[i]) active = i;
    const unsigned d = active < n ? (active + n - 1) % n : counted;
    const unsigned r = dmaRefillStep(st, active < n, block, d == counted);
    if (r & DMA_REFILL_FILL) *done = d;
    return r;
}

} // namespace platform
