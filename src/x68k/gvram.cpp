// グラフィック VRAM の CPU からの読み書き。
//
// CPU から見たグラフィック VRAM ($C00000-$DFFFFF) の並びは、CRTC R20 のメモリモードで変わる
// (Data Crystal の X68k:IOMAP の CRTC R20 とグラフィック VRAM の節)。資料の要点:
//   - 実体は 512KB。どのモードでも 1 ドットは 1 ワードで、16 色 / 256 色ではその下位 4 / 8 ビットだけが有効
//   - 資料の ADR / BIT は「65536 色表示モードで (X, Y) の点を読み書きするときのアドレスとビット位置」で、
//     ここでは実体のワード (gvram_ の添字) とビット位置にあたる
//   - R20 bit11 (G-MEM) = 1 (バッファ用): bit10-8 は無効で、65536 色と同じ並びになる
//   - 512x512、65536 色 (COL = %11): ページ 0 だけ ($C00000-$C7FFFF)。1 ラインは 1024 バイト
//   - 512x512、256 色 (COL = %01): ページ 0、1 が $C00000、$C80000 から。ADR = $C00000 + Y x 1024 + X x 2、
//     BIT = P x 8 から 8 ビット
//   - 512x512、16 色 (COL = %00): ページ 0-3 が $C00000、$C80000、$D00000、$D80000 から。BIT = P x 4 から 4 ビット
//   - 1024x1024 (bit10 = 1)、16 色: ページ 0 だけ ($C00000-$DFFFFF)。1 ラインは 2048 バイト。
//     ADR = $C00000 + (Y & 511) x 1024 + (X & 511) x 2、BIT = (Y >= 512) x 8 + (X >= 512) x 4 から 4 ビット
// COL = %10 は資料では未定義。1024x1024 は 16 色の表示しかないので、bit10 = 1 のときは COL を見ない。
// どのページにも当たらない位置は、読むと 0 で、書いても何も変わらない。
// 表示 (render.cpp) は、どのモードでも実体のワードを読む。

#include "x68k/video.h"

namespace x68k {

namespace {

struct Where {
    uint32_t word;  // gvram_ の添字。範囲外なら 0xffffffff
    int shift;      // 対象のビットの位置
    uint16_t mask;  // 対象のビット (shift する前)
};

Where locate(uint16_t r20, uint32_t offset) {
    Where w = {0xffffffff, 0, 0};
    int col = (r20 >> 8) & 3;
    if ((r20 & 0x0800) || (!(r20 & 0x0400) && col == 3)) { // バッファ用か 65536 色: ワードがそのまま見える
        if (offset < Video::GVRAM_SIZE) w = {offset >> 1, 0, 0xffff};
        return w;
    }
    if (r20 & 0x0400) { // 実画面 1024x1024 (16 色)
        uint32_t x = (offset >> 1) & 1023, y = (offset >> 11) & 1023;
        int page = (y >= 512 ? 2 : 0) + (x >= 512 ? 1 : 0);
        w = {(y & 511) * 512 + (x & 511), page * 4, 0xf};
        return w;
    }
    uint32_t page = offset >> 19;
    uint32_t word = (offset & 0x7ffff) >> 1;
    if (col == 0) {
        w = {word, (int)page * 4, 0xf};
    } else if (page < 2) { // 256 色 (col = 2 は未定義なので 256 色と同じに扱う)
        w = {word, (int)page * 8, 0xff};
    }
    return w;
}

} // namespace

uint8_t Video::gvramRead(uint32_t offset) const {
    Where w = locate(crtc_[20], offset);
    if (w.word == 0xffffffff) return 0;
    uint16_t v = gvram_[w.word];
    if (w.mask == 0xffff) return (offset & 1) ? (uint8_t)v : (uint8_t)(v >> 8);
    if (!(offset & 1)) return 0;
    return (uint8_t)((v >> w.shift) & w.mask);
}

void Video::gvramWrite(uint32_t offset, uint8_t v) {
    if (journal_) journal_->push(VideoJournal::make(VideoJournal::OP_GVRAM, offset, v));
    Where w = locate(crtc_[20], offset);
    if (w.word == 0xffffffff) return;
    uint16_t& d = gvram_[w.word];
    if (w.mask == 0xffff) {
        d = (offset & 1) ? (uint16_t)((d & 0xff00) | v) : (uint16_t)((d & 0x00ff) | (v << 8));
        return;
    }
    if (!(offset & 1)) return;
    d = (uint16_t)((d & ~(w.mask << w.shift)) | ((v & w.mask) << w.shift));
}

} // namespace x68k
