// 画面の右下に出す、各コア (0 から 3) とメモリの使用率のバー (pix68k.cfg の osd_bars=on。#239)。
// 縦の棒を横に 5 本並べ、それぞれ下から上に伸ばす。幅は右上の数字の縦の並び (OSD_V_COLS = 15 文字) の左に 1 文字分の
// 余白を足した 16 文字 (128 ドット)、高さは 80 ライン。余白がないと、いちばん左の棒 (C0) が背景の左端から 2 ドットの
// ところに来て、詰まって見える (#314)。右端は右上の数字とそろえる。いちばん下の 16 ラインに「C0 C1 C2 C3 MEM」のラベル (右上の数字と同じ 1 倍の文字。8x16) を出し、
// それぞれの棒はラベルの文字の真上に置く。% の数字は出さない (5 本を 120 ドットに並べると「100」が隣とつながるため)。
// 右上の数字が狭い並び (1 行 OSD_V_ITEM = 11 文字。platform/osd.h の 1b.) のときは、バーも 11 文字 (88 ドット) に詰める:
// ラベルは 1 文字ずつ「0 1 2 3 M」、棒は幅 12 ドットで 4 ドットずつ空ける。左の余白は同じ 1 文字分。
// 棒の色は使用率で変える (OSD_BAR_GREEN_MAX % まで緑、OSD_BAR_YELLOW_MAX % まで黄、それより上は赤)。
//
// 使用率は、コア 0 が 1 秒に 1 回 (CKernel::UpdateOsd()) 各コアが仕事をしていた時間の割合を求め、osdBarShown() で
// 出す値に丸めてから packOsdBars() で 64 ビット 1 つに詰める。前に描いた値と比べて、変わったときだけ描く。
// 置く場所 (placeOsdBars()): 右上の数字の縦の並びと同じ列の、画面の下の端。右上の数字にぶつかるか、フレームバッファに
// 入らなければ出さない。X68000 の画面に重なれば重ねる (右上の数字と同じく、写すときに重なる部分だけ絵を書く)。
// ベアメタル版の CKernel::ShowFrame() が使う (ホスト版はコアごとの数字がないので出さない)。
// ベアメタルでも使えるよう、例外、RTTI、STL、stdio は使わない (ヘッダだけ)。
#pragma once

#include <stdint.h>

#include "platform/osd.h"

namespace platform {

static constexpr int OSD_BAR_ROWS = 5;            // 棒の数 (C0、C1、C2、C3、MEM)
static constexpr int OSD_BAR_CORES = 4;           // コアの棒の数 (MEM の棒は OSD_BAR_CORES 番目)
static constexpr int OSD_BAR_PAD_COLS = 1;        // 左の余白 (文字数)
static constexpr int OSD_BAR_COLS = OSD_BAR_PAD_COLS + OSD_V_COLS; // 幅の文字数 (余白 + 右上の数字の縦の並びと同じ幅)
static constexpr int OSD_BAR_W = OSD_BAR_COLS * 8;
static constexpr int OSD_BAR_NARROW_COLS = OSD_V_ITEM;       // 狭い並びの幅の文字数 (余白 1 + 「0 1 2 3 M」9 + 右の 1)
static constexpr int OSD_BAR_NARROW_W = OSD_BAR_NARROW_COLS * 8;
static constexpr int OSD_BAR_H = 80;
static constexpr int OSD_BAR_PIXELS = OSD_BAR_W * OSD_BAR_H; // 絵の画素数
static constexpr int OSD_BAR_LABEL_Y = OSD_BAR_H - 16;            // ラベルの行の上端
static constexpr int OSD_BAR_TOP = 2, OSD_BAR_BOTTOM = OSD_BAR_LABEL_Y - 2; // 棒の上端と、下端の次 (100% と 0% の位置)
static constexpr unsigned OSD_BAR_STEP = 5;       // 出す使用率の刻み (%)。70 と 90 が境目に来るように 5 にする
static constexpr unsigned OSD_BAR_GREEN_MAX = 70, OSD_BAR_YELLOW_MAX = 90;
static constexpr uint32_t OSD_BAR_TRACK = 0xff404040u; // バーの使っていない部分
static constexpr uint32_t OSD_BAR_GREEN = 0xff20c040u, OSD_BAR_YELLOW = 0xffe0c020u, OSD_BAR_RED = 0xffe03030u;

// 出す使用率 (%) を決める。permille は 0.1% 単位の測った値 (1000 より大きければ 1000 にする)、shown は前に出した値
// (負ならまだ出していない)。前に出した値から 4% 以上離れたときだけ、OSD_BAR_STEP に丸めた値に変える (毎秒少しずつ
// 変わる使用率で、1 秒ごとに描き直したり、丸めの境目で行き来したりしないため。osdRpmShown() と同じ考え)
inline int osdBarShown(int shown, unsigned permille) {
    if (permille > 1000) permille = 1000;
    if (shown >= 0 && permille < (unsigned)shown * 10 + 40 && (unsigned)shown * 10 < permille + 40) return shown;
    return (int)((permille + OSD_BAR_STEP * 5) / (OSD_BAR_STEP * 10) * OSD_BAR_STEP);
}

// 測った時間から使用率 (0.1% 単位) を求める。delta は前に数えてからの合計の差 (us)、dtUs はその間の時間 (us)。
// idle なら delta は待った時間 (コア 0)、そうでなければ仕事をした時間 (コア 1 から 3)。dtUs が 0 なら 0
inline unsigned osdBarPermille(uint64_t delta, uint64_t dtUs, bool idle) {
    if (dtUs == 0) return 0;
    if (delta > dtUs) delta = dtUs;
    if (idle) delta = dtUs - delta;
    return (unsigned)(delta * 1000 / dtUs);
}

// 使用率の色
inline uint32_t osdBarColor(unsigned pct) {
    return pct <= OSD_BAR_GREEN_MAX ? OSD_BAR_GREEN : pct <= OSD_BAR_YELLOW_MAX ? OSD_BAR_YELLOW : OSD_BAR_RED;
}

// 詰めた使用率。下位から 8 ビットずつ C0、C1、C2、C3、MEM の「使用率 (%) に 1 を足した値」(0 ならその棒は出さない)。
// bit 63 が 1 なら値がある (OsdValue の a に入れて OsdState で比べる)
struct OsdBars {
    int pct[OSD_BAR_ROWS] = {-1, -1, -1, -1, -1}; // 使用率 (%)。負ならその棒を出さない
};

inline uint64_t packOsdBars(const OsdBars& b) {
    uint64_t v = 1ull << 63;
    for (int i = 0; i < OSD_BAR_ROWS; i++)
        if (b.pct[i] >= 0) v |= (uint64_t)((b.pct[i] > 100 ? 100 : b.pct[i]) + 1) << (8 * i);
    return v;
}

// 詰めた値を戻す。packOsdBars() を通していない値でも 100 % までにする
inline OsdBars unpackOsdBars(uint64_t v) {
    OsdBars b;
    for (int i = 0; i < OSD_BAR_ROWS; i++) {
        const unsigned p = (unsigned)(v >> (8 * i) & 255);
        b.pct[i] = p == 0 ? -1 : p > 101 ? 100 : (int)p - 1;
    }
    return b;
}

// 置く場所。右端を右上の数字の縦の並び (placeOsdVertical()) にそろえた、画面の下の端 (下から OSD_PAD_Y 画素)。
// mode が OSD_MODE_OFF か、X68000 の画面をまだ写していない (picW が 0 以下) か、フレームバッファに入らないか、
// 右上の数字 (top。w が 0 なら出していない) にぶつかるなら w = 0。X68000 の画面に重なれば overlay
inline OsdRect placeOsdBars(int mode, int fbW, int fbH, int picX, int picY, int picW, int picH, const OsdRect& top) {
    OsdRect r;
    if (mode == OSD_MODE_OFF || picW <= 0) return r;
    r.h = OSD_BAR_H;
    if (osdIsNarrow(top)) {
        // 右上の数字が狭い並び: 同じ幅、同じ列に置く
        r.w = OSD_BAR_NARROW_W;
        r.x = top.x;
    } else {
        r.w = OSD_BAR_W;
        // 右上の数字と同じ式で右端の列を決めてから、余白の分だけ左に広げる (& ~3 を幅ごとに掛けると、右端がずれることがある)
        const int right = ((fbW - OSD_V_COLS * 8 - OSD_PAD_X) & ~3) + OSD_V_COLS * 8;
        r.x = right - OSD_BAR_W;
        // 右の余白が 128〜135 ドットのとき (1280x1024 に 512x512 の画面を 2 倍で出したとき、768x512 のフレームバッファに
        // 512x512 の画面など) は、右上の数字は X68000 の画面に重ならないのに、左に広げた 8 ドットだけが重なる。そのときは
        // 狭い並びのバーを右端にそろえて置く (余白を足す前は重ならなかった)。ラベルは「0 1 2 3 M」になる (左の余白を優先する)
        const int y = fbH - r.h - OSD_PAD_Y;
        const bool rows = picH > 0 && y < picY + picH && picY < y + r.h;
        if (rows && r.x < picX + picW && picX < r.x + r.w && right - OSD_BAR_NARROW_W >= picX + picW) {
            r.w = OSD_BAR_NARROW_W;
            r.x = right - OSD_BAR_NARROW_W;
        }
    }
    r.y = fbH - r.h - OSD_PAD_Y;
    if (r.x < 0 || r.y < 0) return OsdRect();
    if (top.w > 0 && r.x < top.x + top.w && top.x < r.x + r.w && r.y < top.y + top.h && top.y < r.y + r.h)
        return OsdRect();
    r.vertical = true;
    r.overlay = picH > 0 && r.x < picX + picW && picX < r.x + r.w && r.y < picY + picH && picY < r.y + r.h;
    return r;
}

// i 番目の棒のラベル (いちばん下の行の文字の位置。左の余白のあとに「C0 C1 C2 C3 MEM」) と、棒の左端と右端の次 (画素)
// narrow は狭い並び (ラベルは「0 1 2 3 M」の 1 文字ずつ。棒は文字の左右に 2 ドットずつはみ出す 12 ドット)
static constexpr const char* OSD_BAR_LABELS = "C0 C1 C2 C3 MEM";
static constexpr const char* OSD_BAR_NARROW_LABELS = "0123M";
inline int osdBarLabelCol(int i, bool narrow = false) { return OSD_BAR_PAD_COLS + i * (narrow ? 2 : 3); }
inline int osdBarLabelLen(int i, bool narrow = false) { return narrow ? 1 : i < OSD_BAR_CORES ? 2 : 3; }
inline int osdBarX0(int i, bool narrow = false) { return osdBarLabelCol(i, narrow) * 8 + (narrow ? -2 : 2); }
inline int osdBarX1(int i, bool narrow = false) {
    return (osdBarLabelCol(i, narrow) + osdBarLabelLen(i, narrow)) * 8 + (narrow ? 2 : -2);
}
// 使用率 pct (%) の棒の、塗る部分の上端 (pct が 0 なら OSD_BAR_BOTTOM で、塗らない)
inline int osdBarFillY(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return OSD_BAR_BOTTOM - ((OSD_BAR_BOTTOM - OSD_BAR_TOP) * pct + 50) / 100;
}

// packed (packOsdBars() の値) を r (placeOsdBars() の場所。OSD_BAR_W x OSD_BAR_H か、狭い並びの OSD_BAR_NARROW_W x
// OSD_BAR_H) の絵にする。image は OSD_BAR_PIXELS 画素まで (1 行 r.w 画素、ふつうのメモリ)。出さない棒 (pct が負) は、
// 棒もラベルも描かない
inline void renderOsdBars(uint32_t* image, const OsdRect& r, uint64_t packed) {
    if ((r.w != OSD_BAR_W && r.w != OSD_BAR_NARROW_W) || r.h != OSD_BAR_H) return;
    const bool narrow = r.w == OSD_BAR_NARROW_W;
    const int cols = r.w / 8;
    const OsdBars b = unpackOsdBars(packed);
    for (int i = 0; i < r.w * r.h; i++) image[i] = OSD_BG;
    char text[OSD_BAR_COLS];
    for (int i = 0; i < OSD_BAR_COLS; i++) text[i] = ' ';
    for (int i = 0; i < OSD_BAR_ROWS; i++) {
        if (b.pct[i] < 0) continue;
        for (int k = 0; k < osdBarLabelLen(i, narrow); k++)
            text[osdBarLabelCol(i, narrow) + k] = narrow ? OSD_BAR_NARROW_LABELS[i] : OSD_BAR_LABELS[i * 3 + k];
        const int fy = osdBarFillY(b.pct[i]);
        const uint32_t color = osdBarColor((unsigned)b.pct[i]);
        for (int y = OSD_BAR_TOP; y < OSD_BAR_BOTTOM; y++) {
            uint32_t* px = image + (size_t)y * r.w;
            for (int x = osdBarX0(i, narrow); x < osdBarX1(i, narrow); x++) px[x] = y >= fy ? color : OSD_BAR_TRACK;
        }
    }
    for (int y = 0; y < 16; y++) renderOsdRow(image + (size_t)(OSD_BAR_LABEL_Y + y) * r.w, text, y, 1, cols);
}

} // namespace platform
