// Video の画面の大きさ (screenSize)、パレットとビデオコントローラの番地、CRTC のレジスタの読み書き、
// グラフィック VRAM のメモリモードごとの並びのテスト。
// 期待値は Data Crystal の X68k:IOMAP (CRTC R02、R03、R06、R07、R20、グラフィック VRAM、ビデオコントローラの節) から
// 作った。CRTC のレジスタの繰り返しは video.cpp の crtcRead / crtcWrite の説明による
#include <stdio.h>

#include "test_check.h"
#include "x68k/video.h"

namespace {

using x68k::paletteToRgb;
using x68k::Video;

Video g_video; // VRAM を持つので大きい

void crtc(Video& v, int n, uint16_t val) {
    v.crtcWrite((uint32_t)n * 2, (uint8_t)(val >> 8));
    v.crtcWrite((uint32_t)n * 2 + 1, (uint8_t)val);
}

void display(Video& v, uint16_t r20, uint16_t r2, uint16_t r3, uint16_t r6, uint16_t r7) {
    crtc(v, 20, r20);
    crtc(v, 2, r2);
    crtc(v, 3, r3);
    crtc(v, 6, r6);
    crtc(v, 7, r7);
}

void size(Video& v, int& w, int& h) {
    w = -1;
    h = -1;
    v.screenSize(w, h);
}

void testScreenSize() {
    printf("screenSize\n");
    Video& v = g_video;
    v.reset();
    int w, h;
    // リセットのあとの値: 768x512 (31kHz、512 ライン)。R02..R03 が 96 文字、R06..R07 が 512 ラスタ
    size(v, w, h);
    CHECK_EQ(w, 768);
    CHECK_EQ(h, 512);
    // 31kHz の 256 ライン: 2 ラスタで 1 ライン
    display(v, 0x0010, 0x0a, 0x2a, 0x28, 0x228);
    size(v, w, h);
    CHECK_EQ(w, 256);
    CHECK_EQ(h, 256);
    // 31kHz の 512 ライン
    display(v, 0x0015, 0x1c, 0x5c, 0x28, 0x228);
    size(v, w, h);
    CHECK_EQ(w, 512);
    CHECK_EQ(h, 512);
    // 31kHz の 1024 ラインのインタレース (VD = %10 と %11): 1 ラスタで 2 ライン
    display(v, 0x0019, 0x1c, 0x5c, 0x28, 0x228);
    size(v, w, h);
    CHECK_EQ(h, 1024);
    display(v, 0x001d, 0x1c, 0x5c, 0x28, 0x228);
    size(v, w, h);
    CHECK_EQ(h, 1024);
    // 15kHz の 256 ライン: 1 ラスタで 1 ライン
    display(v, 0x0000, 0x0a, 0x2a, 0x10, 0x110);
    size(v, w, h);
    CHECK_EQ(w, 256);
    CHECK_EQ(h, 256);
    // 15kHz の 512 ライン (インタレース): 1 ラスタで 2 ライン
    display(v, 0x0005, 0x0a, 0x4a, 0x10, 0x110);
    size(v, w, h);
    CHECK_EQ(w, 512);
    CHECK_EQ(h, 512);
    // 水平の解像度 (HD) と色数、実画面の大きさは、大きさに関係しない (表示期間のレジスタだけで決まる)
    display(v, 0x0716, 0x1c, 0x5c, 0x28, 0x228);
    size(v, w, h);
    CHECK_EQ(w, 512);
    CHECK_EQ(h, 512);
    // 終わりが始まりと同じか前なら 0
    display(v, 0x0016, 0x40, 0x40, 0x28, 0x228);
    size(v, w, h);
    CHECK_EQ(w, 0);
    display(v, 0x0016, 0x50, 0x40, 0x228, 0x28);
    size(v, w, h);
    CHECK_EQ(w, 0);
    CHECK_EQ(h, 0);
    // 31kHz の 256 ラインで、奇数のラスタ数は切り捨て
    display(v, 0x0010, 0x0a, 0x2a, 0x28, 0x29 + 0x100);
    size(v, w, h);
    CHECK_EQ(h, 128);
    // 15kHz の VD = %10 と %11 も、512 ラインのインタレースと同じ (1 ラスタで 2 ライン)
    display(v, 0x0009, 0x0a, 0x4a, 0x10, 0x110);
    size(v, w, h);
    CHECK_EQ(h, 512);
    display(v, 0x000d, 0x0a, 0x4a, 0x10, 0x110);
    size(v, w, h);
    CHECK_EQ(h, 512);
    // 垂直の開始と終了が同じなら 0、差が 1 ラスタなら、ライン数はモードごとに 0 / 1 / 2
    display(v, 0x0015, 0x1c, 0x5c, 0x100, 0x100);
    size(v, w, h);
    CHECK_EQ(h, 0);
    display(v, 0x0010, 0x1c, 0x5c, 0x100, 0x101);
    size(v, w, h);
    CHECK_EQ(h, 0);
    display(v, 0x0015, 0x1c, 0x5c, 0x100, 0x101);
    size(v, w, h);
    CHECK_EQ(h, 1);
    display(v, 0x0019, 0x1c, 0x5c, 0x100, 0x101);
    size(v, w, h);
    CHECK_EQ(h, 2);
    display(v, 0x0000, 0x1c, 0x5c, 0x100, 0x101);
    size(v, w, h);
    CHECK_EQ(h, 1);
    // 全部 0 なら幅も高さも 0
    display(v, 0x0016, 0, 0, 0, 0);
    size(v, w, h);
    CHECK_EQ(w, 0);
    CHECK_EQ(h, 0);
    // R02 / R03 は bit7-0、R06 / R07 は bit9-0 のレジスタ (X68k:IOMAP)。幅の外のビットは見ない (#152)
    display(v, 0x0015, 0, 0xffff, 0, 0xffff);
    size(v, w, h);
    CHECK_EQ(w, 0xff * 8);
    CHECK_EQ(h, 0x3ff);
    display(v, 0x0015, 0xffff, 0, 0xffff, 0);
    size(v, w, h);
    CHECK_EQ(w, 0);
    CHECK_EQ(h, 0);
    display(v, 0x0015, 0xffff, 0xffff, 0xffff, 0xffff);
    size(v, w, h);
    CHECK_EQ(w, 0);
    CHECK_EQ(h, 0);
    // 幅の外のビットだけが違う値は、同じ大きさになる (512x512 の値の上に、幅の外のビットを立てる)
    display(v, 0x0015, 0x0100 | 0x1c, 0xff00 | 0x5c, 0x0400 | 0x28, 0xfc00 | 0x228);
    size(v, w, h);
    CHECK_EQ(w, 512);
    CHECK_EQ(h, 512);
    // 始まりの幅の外のビットだけが大きくても、終わりより後ろにはならない (マスクしなければ 0 になる)
    display(v, 0x0015, 0x8000 | 0x1c, 0x5c, 0x8000 | 0x28, 0x228);
    size(v, w, h);
    CHECK_EQ(w, 512);
    CHECK_EQ(h, 512);
    // R07 の bit9 は効く (0x3ff - 0x200 = 0x1ff ラスタ)
    display(v, 0x0015, 0x1c, 0x5c, 0x200, 0x3ff);
    size(v, w, h);
    CHECK_EQ(h, 0x1ff);
}

// 表示期間のアクセサ (verticalDisplayStart / End、horizontalDisplayStart / End、horizontalDisplay) も
// screenSize() と同じ幅でマスクする (#152)
void testDisplayPeriodMask() {
    printf("display period mask\n");
    Video& v = g_video;
    v.reset();
    display(v, 0x0015, 0xab00 | 0x1c, 0xcd00 | 0x5c, 0xf400 | 0x28, 0xfc00 | 0x228);
    CHECK_EQ(v.horizontalDisplayStart(), 0x1c);
    CHECK_EQ(v.horizontalDisplayEnd(), 0x5c);
    CHECK_EQ(v.verticalDisplayStart(), 0x28);
    CHECK_EQ(v.verticalDisplayEnd(), 0x228);
    // reg() は書いた 16 ビットの値をそのまま返す
    CHECK_EQ(v.reg(2), 0xab1c);
    CHECK_EQ(v.reg(7), 0xfe28);
    // 水平表示期間の範囲 (1 ラスタの中の割合 x 65536) も、マスクした値から求める
    crtc(v, 0, 0x89);
    uint32_t s = 0, e = 0;
    v.horizontalDisplay(s, e);
    CHECK_EQ(s, (uint32_t)0x1c * 65536 / 0x8a);
    CHECK_EQ(e, (uint32_t)0x5c * 65536 / 0x8a);
    // 幅の中のビットは全部効く (R02 / R03 の bit7、R06 / R07 の bit9)
    display(v, 0x0015, 0x80, 0xff, 0x200, 0x3ff);
    CHECK_EQ(v.horizontalDisplayStart(), 0x80);
    CHECK_EQ(v.horizontalDisplayEnd(), 0xff);
    CHECK_EQ(v.verticalDisplayStart(), 0x200);
    CHECK_EQ(v.verticalDisplayEnd(), 0x3ff);
}

// タイミングのアクセサ (horizontalTotal、horizontalTotalDots、verticalTotal、rasterInterruptLine、
// horizontalDisplay の分母) も資料の幅でマスクする。R00 は bit7-0、R04 / R09 は bit9-0 (#178)
void testTimingMask() {
    printf("timing mask\n");
    Video& v = g_video;
    v.reset();
    crtc(v, 0, 0xff00 | 0x89);
    crtc(v, 4, 0xfc00 | 0x237);
    crtc(v, 9, 0xf800 | 0x123);
    CHECK_EQ(v.horizontalTotal(), 0x89);
    CHECK_EQ(v.horizontalTotalDots(), (uint32_t)(0x89 + 1) * 8);
    CHECK_EQ(v.verticalTotal(), 0x237 + 1);
    CHECK_EQ(v.rasterInterruptLine(), 0x123);
    CHECK_EQ(v.reg(0), 0xff89);
    CHECK_EQ(v.reg(4), 0xfe37);
    CHECK_EQ(v.reg(9), 0xf923);
    // 水平表示期間の範囲の分母 (R00 + 1) もマスクした値
    crtc(v, 2, 0x1c);
    crtc(v, 3, 0x5c);
    uint32_t s = 0, e = 0;
    v.horizontalDisplay(s, e);
    CHECK_EQ(s, (uint32_t)0x1c * 65536 / 0x8a);
    CHECK_EQ(e, (uint32_t)0x5c * 65536 / 0x8a);
    // R04 の幅の外のビットだけを立てた値は、16 ラスタより短いとして扱う (下限の 16)
    crtc(v, 4, 0xfc00 | 0x005);
    CHECK_EQ(v.verticalTotal(), 16);
    // 幅の中のビットは全部効く (R00 の bit7、R04 / R09 の bit9)
    crtc(v, 0, 0xff);
    crtc(v, 4, 0x3ff);
    crtc(v, 9, 0x3ff);
    CHECK_EQ(v.horizontalTotal(), 0xff);
    CHECK_EQ(v.horizontalTotalDots(), (uint32_t)0x100 * 8);
    CHECK_EQ(v.verticalTotal(), 0x400);
    CHECK_EQ(v.rasterInterruptLine(), 0x3ff);
}

void testPaletteAddress() {
    printf("palette address\n");
    Video& v = g_video;
    v.reset();
    // $E82000-$E823FF はパレット (グラフィック 256 ワード、テキスト / スプライト 256 ワード)。書いたバイトが読める
    for (uint32_t o = 0; o < 0x400; o++) v.paletteWrite(o, (uint8_t)(o * 5 + 1));
    bool ok = true;
    for (uint32_t o = 0; o < 0x400; o++) ok = ok && v.paletteRead(o) == (uint8_t)(o * 5 + 1);
    CHECK_EQ(ok, true);
    CHECK_EQ(v.paletteColors()[0x123], (uint16_t)(((0x246 * 5 + 1) & 0xff) << 8 | ((0x247 * 5 + 1) & 0xff)));
    CHECK_EQ(v.paletteXrgb()[0x123], paletteToRgb(v.paletteColors()[0x123]));
    // ビデオコントローラ R0 = $E82400、R1 = $E82500、R2 = $E82600 (各 1 ワード)
    v.paletteWrite(0x400, 0x00);
    v.paletteWrite(0x401, 0x03);
    v.paletteWrite(0x500, 0x12);
    v.paletteWrite(0x501, 0xe4);
    v.paletteWrite(0x600, 0x10);
    v.paletteWrite(0x601, 0x7f);
    CHECK_EQ(v.videoControl(0), 0x0003);
    CHECK_EQ(v.videoControl(1), 0x12e4);
    CHECK_EQ(v.videoControl(2), 0x107f);
    CHECK_EQ(v.paletteRead(0x400), 0x00);
    CHECK_EQ(v.paletteRead(0x401), 0x03);
    CHECK_EQ(v.paletteRead(0x500), 0x12);
    CHECK_EQ(v.paletteRead(0x601), 0x7f);
    // ビデオコントローラに書いても、パレットは変わらない
    CHECK_EQ(v.paletteRead(0x000), 0x01);
    CHECK_EQ(v.paletteRead(0x3ff), (uint8_t)(0x3ff * 5 + 1));
    // 範囲の外
    CHECK_EQ(v.paletteRead(Video::PALETTE_SIZE), 0x00);
}

// CRTC のレジスタの読み書き (video.cpp の crtcRead / crtcWrite の説明の決まり):
// レジスタは $E80000-$E803FF の中で 0x40 バイトごとに繰り返し、窓全体は 0x800 バイトごとに同じものが見える。
// 読めるのは R20 と R21 だけで、ほかは 0。R24 から先 ($E80030-) への書き込みは無視する
void testCrtcRegisters() {
    printf("crtc registers\n");
    Video& v = g_video;
    v.reset();
    crtc(v, 20, 0x0316);
    crtc(v, 21, 0x0123);
    CHECK_EQ(v.reg(20), 0x0316);
    CHECK_EQ(v.crtcRead(0x28), 0x03);
    CHECK_EQ(v.crtcRead(0x29), 0x16);
    CHECK_EQ(v.crtcRead(0x2a), 0x01);
    CHECK_EQ(v.crtcRead(0x2b), 0x23);
    CHECK_EQ(v.crtcRead(0x28 + 0x40), 0x03);
    CHECK_EQ(v.crtcRead(0x29 + 0x800), 0x16);
    // R20、R21 のほかは 0 が読める (R00..R23 の全部。書いた値は reg() で見える)
    for (int n = 0; n < 24; n++) {
        if (n == 20 || n == 21) continue;
        crtc(v, n, (uint16_t)(0x5a00 | (n + 1)));
        CHECK_EQ(v.reg(n), (uint16_t)(0x5a00 | (n + 1)));
        CHECK_EQ(v.crtcRead((uint32_t)n * 2), 0x00);
        CHECK_EQ(v.crtcRead((uint32_t)n * 2 + 1), 0x00);
    }
    CHECK_EQ(v.crtcRead(0x29), 0x16);
    // 0x40 ごとの繰り返しに書いても同じレジスタ
    v.crtcWrite(0x40 + 0x06, 0x00);
    v.crtcWrite(0x40 + 0x07, 0x5c);
    CHECK_EQ(v.reg(3), 0x005c);
    v.crtcWrite(0x800 + 0x0c, 0x01);
    CHECK_EQ(v.reg(6), 0x0107); // 下位バイトは上のループで書いた 0x5a07 の 0x07 のまま
    // 1 バイトずつ書ける (上位と下位)
    v.crtcWrite(0x0e, 0x02);
    CHECK_EQ(v.reg(7) >> 8, 0x02);
    v.crtcWrite(0x0f, 0x28);
    CHECK_EQ(v.reg(7), 0x0228);
    // $E80030 から先は無視する
    const uint16_t before = v.reg(0);
    v.crtcWrite(0x30, 0xff);
    v.crtcWrite(0x31, 0xff);
    CHECK_EQ(v.reg(0), before);
    // 書くたびに世代が進む
    const uint32_t gen = v.crtcGeneration();
    crtc(v, 3, 0x007c);
    CHECK_EQ(v.crtcGeneration() != gen, true);
}

uint16_t word(const Video& v, int x, int y) { return v.gvramWord((uint32_t)(y * 512 + x)); }

void testGvramLayout() {
    printf("gvram layout\n");
    Video& v = g_video;

    // 65536 色: ドット (X, Y) は $C00000 + Y x 1024 + X x 2 のワード。$C80000 から先はない
    v.reset();
    crtc(v, 20, 0x0315);
    v.gvramWrite(100 * 1024 + 30 * 2, 0x12);
    v.gvramWrite(100 * 1024 + 30 * 2 + 1, 0x34);
    CHECK_EQ(word(v, 30, 100), 0x1234);
    CHECK_EQ(v.gvramRead(100 * 1024 + 30 * 2), 0x12);
    CHECK_EQ(v.gvramRead(100 * 1024 + 30 * 2 + 1), 0x34);
    v.gvramWrite(0x80000 + 1, 0x55);
    CHECK_EQ(v.gvramRead(0x80000 + 1), 0x00);
    CHECK_EQ(word(v, 0, 0), 0x0000);

    // 256 色: ページ 1 は $C80000 から。ワードの下位バイトに見え、実体の bit15-8 になる
    v.reset();
    crtc(v, 20, 0x0115);
    v.gvramWrite(0x80000 + 7 * 1024 + 9 * 2 + 1, 0xa5);
    CHECK_EQ(word(v, 9, 7), 0xa500);
    CHECK_EQ(v.gvramRead(0x80000 + 7 * 1024 + 9 * 2 + 1), 0xa5);
    CHECK_EQ(v.gvramRead(7 * 1024 + 9 * 2 + 1), 0x00); // ページ 0 は bit7-0
    v.gvramWrite(7 * 1024 + 9 * 2 + 1, 0x3c);
    CHECK_EQ(word(v, 9, 7), 0xa53c);
    // ページ 2、3 の位置 ($D00000 から) はない
    v.gvramWrite(0x100000 + 1, 0xff);
    CHECK_EQ(v.gvramRead(0x100000 + 1), 0x00);
    CHECK_EQ(word(v, 0, 0), 0x0000);

    // COL = %10 は資料では未定義。256 色と同じ並びとして扱う (今の動作を固定する)
    v.reset();
    crtc(v, 20, 0x0215);
    v.gvramWrite(0x80000 + 7 * 1024 + 9 * 2 + 1, 0xa5);
    v.gvramWrite(7 * 1024 + 9 * 2 + 1, 0x3c);
    CHECK_EQ(word(v, 9, 7), 0xa53c);
    CHECK_EQ(v.gvramRead(0x80000 + 7 * 1024 + 9 * 2 + 1), 0xa5);
    v.gvramWrite(0x100000 + 1, 0xff);
    CHECK_EQ(v.gvramRead(0x100000 + 1), 0x00);
    CHECK_EQ(word(v, 0, 0), 0x0000);

    // 16 色: ページ P は $C00000 + P x $80000 から。実体の bit P x 4 .. P x 4 + 3
    v.reset();
    crtc(v, 20, 0x0015);
    for (uint32_t p = 0; p < 4; p++) v.gvramWrite(p * 0x80000 + 3 * 1024 + 4 * 2 + 1, (uint8_t)(0xf0 | (p + 1)));
    CHECK_EQ(word(v, 4, 3), 0x4321);
    CHECK_EQ(v.gvramRead(0x100000 + 3 * 1024 + 4 * 2 + 1), 0x03);

    // 1024x1024 (16 色): 1 ラインは 2048 バイト。ドット (600, 700) は $C00000 + 700 x 2048 + 600 x 2。
    // 実体は (700 & 511, 600 & 511) のワードの bit (700 >= 512) x 8 + (600 >= 512) x 4 = bit 12 から
    v.reset();
    crtc(v, 20, 0x0415);
    const uint32_t a = 700 * 2048 + 600 * 2 + 1;
    v.gvramWrite(a, 0x0b);
    CHECK_EQ(word(v, 600 & 511, 700 & 511), 0xb000);
    CHECK_EQ(v.gvramRead(a), 0x0b);
    v.gvramWrite(100 * 2048 + 100 * 2 + 1, 0x07); // 左上の 512x512 は bit3-0
    v.gvramWrite(100 * 2048 + 612 * 2 + 1, 0x06); // 右上は bit7-4
    v.gvramWrite(612 * 2048 + 100 * 2 + 1, 0x05); // 左下は bit11-8
    CHECK_EQ(word(v, 100, 100), 0x0567);
    // 1024x1024 は COL を見ない (65536 色の指定でも 16 色の並び)
    crtc(v, 20, 0x0715);
    CHECK_EQ(v.gvramRead(a), 0x0b);

    // バッファ用 (R20 bit11): bit10-8 は無効で、65536 色と同じ並び
    crtc(v, 20, 0x0c15);
    v.gvramWrite(0, 0x9a);
    v.gvramWrite(1, 0xbc);
    CHECK_EQ(word(v, 0, 0), 0x9abc);
    v.gvramWrite(200 * 1024 + 300 * 2, 0x11);
    CHECK_EQ(word(v, 300, 200), 0x1100);
    CHECK_EQ(v.gvramRead(200 * 1024 + 300 * 2), 0x11);
}

} // namespace

int main() {
    testScreenSize();
    testDisplayPeriodMask();
    testTimingMask();
    testPaletteAddress();
    testCrtcRegisters();
    testGvramLayout();
    return finish();
}
