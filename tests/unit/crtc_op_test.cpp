// CRTC の動作ポート ($E80481) の高速クリアと、65536 色モードのパレットの表のテスト。
// 期待値は Data Crystal の X68k:IOMAP (CRTC の動作ポートと R21、グラフィックパレットの節) から作った。
// 高速クリアで消す範囲は資料に書いていないので、video.cpp の fastClear() の説明の決め方 (1 画面の走査で読む所。
// 細かい所は前の版と同じ結果) による
#include <stdio.h>

#include "x68k/video.h"

namespace {

int failures = 0;

#define CHECK_EQ(a, b)                                                                                 \
    do {                                                                                               \
        unsigned long long va_ = (unsigned long long)(a), vb_ = (unsigned long long)(b);              \
        if (va_ != vb_) {                                                                              \
            printf("  %s:%d: %s == %s: got %llx, want %llx\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
            failures++;                                                                                \
        }                                                                                              \
    } while (0)

x68k::Video g_video; // VRAM を持つので大きい

void crtc(x68k::Video& v, int n, uint16_t val) {
    v.crtcWrite((uint32_t)n * 2, (uint8_t)(val >> 8));
    v.crtcWrite((uint32_t)n * 2 + 1, (uint8_t)val);
}

void palette(x68k::Video& v, int word, uint16_t c) {
    v.paletteWrite((uint32_t)word * 2, (uint8_t)(c >> 8));
    v.paletteWrite((uint32_t)word * 2 + 1, (uint8_t)c);
}

// 65536 色のメモリモードにして、グラフィック VRAM の全部のワードを val にする
void fill(x68k::Video& v, uint16_t r20, uint16_t val) {
    crtc(v, 20, 0x0300);
    for (uint32_t o = 0; o < x68k::Video::GVRAM_SIZE; o += 2) {
        v.gvramWrite(o, (uint8_t)(val >> 8));
        v.gvramWrite(o + 1, (uint8_t)val);
    }
    crtc(v, 20, r20);
}

// すべてのワードが val か
bool all(const x68k::Video& v, uint16_t val) {
    for (uint32_t i = 0; i < x68k::Video::GVRAM_SIZE / 2; i++)
        if (v.gvramWord(i) != val) return false;
    return true;
}

uint8_t fc(x68k::Video& v) { return (uint8_t)(v.crtcRead(0x481) & 0x02); }

void testFastClear() {
    printf("fast clear\n");
    x68k::Video& v = g_video;
    v.reset();
    // 31kHz、512 ライン、65536 色。CP3-0 をすべて選ぶ
    fill(v, 0x0315, 0xa5c3);
    crtc(v, 21, 0x000f);
    CHECK_EQ(fc(v), 0x00);
    v.crtcWrite(0x481, 0x02);
    CHECK_EQ(fc(v), 0x02);          // 要求してから終わるまで 1
    CHECK_EQ(all(v, 0xa5c3), true); // 垂直表示期間が始まるまでは消えない
    v.beginVerticalDisplay();
    CHECK_EQ(all(v, 0x0000), true);
    CHECK_EQ(fc(v), 0x02); // 1 画面分の走査の間は 1
    v.beginVerticalDisplay();
    CHECK_EQ(fc(v), 0x00); // 終わると自動で 0
    v.beginVerticalDisplay();
    CHECK_EQ(fc(v), 0x00);

    // 要求していなければ消さない
    fill(v, 0x0315, 0x1234);
    v.beginVerticalDisplay();
    v.beginVerticalDisplay();
    CHECK_EQ(all(v, 0x1234), true);

    // CP で選んだページ (ワードの 4 ビットずつ) だけを消す
    static const struct {
        uint16_t cp, left;
    } kPages[] = {{0x1, 0xfff0}, {0x2, 0xff0f}, {0x4, 0xf0ff}, {0x8, 0x0fff}, {0x3, 0xff00}, {0xc, 0x00ff}, {0x0, 0xffff}};
    for (const auto& k : kPages) {
        fill(v, 0x0015, 0xffff); // 16 色
        crtc(v, 21, k.cp);
        v.crtcWrite(0x481, 0x02);
        v.beginVerticalDisplay();
        CHECK_EQ(all(v, k.left), true);
        v.beginVerticalDisplay();
        CHECK_EQ(fc(v), 0x00);
    }

    // 0 を書くと止まる (消す前なら消えない)
    fill(v, 0x0315, 0x1234);
    crtc(v, 21, 0x000f);
    v.crtcWrite(0x481, 0x02);
    v.crtcWrite(0x481, 0x00);
    CHECK_EQ(fc(v), 0x00);
    v.beginVerticalDisplay();
    CHECK_EQ(all(v, 0x1234), true);

    // インタレース (31kHz の 1024 ライン) は 2 フィールドかかる
    fill(v, 0x0319, 0x1234);
    v.crtcWrite(0x481, 0x02);
    v.beginVerticalDisplay();
    CHECK_EQ(all(v, 0x0000), true);
    CHECK_EQ(fc(v), 0x02);
    v.beginVerticalDisplay();
    CHECK_EQ(fc(v), 0x02);
    v.beginVerticalDisplay();
    CHECK_EQ(fc(v), 0x00);

    // 高速クリアの途中の要求は、今のクリアを延ばさない。ラスタコピーのビット (bit3) は書いた値が読める
    fill(v, 0x0315, 0x1234);
    v.crtcWrite(0x481, 0x02);
    v.beginVerticalDisplay();
    v.crtcWrite(0x481, 0x0a);
    CHECK_EQ(v.crtcRead(0x481), 0x0a);
    v.beginVerticalDisplay();
    CHECK_EQ(v.crtcRead(0x481), 0x08);
    v.crtcWrite(0x481, 0x00);

    // 消している途中 (1 画面分の走査の間) に 0 を書くと、そこで終わる
    fill(v, 0x0315, 0x1234);
    crtc(v, 21, 0x000f);
    v.crtcWrite(0x481, 0x02);
    v.beginVerticalDisplay();
    CHECK_EQ(fc(v), 0x02);
    v.crtcWrite(0x481, 0x00);
    CHECK_EQ(fc(v), 0x00);
    v.beginVerticalDisplay();
    CHECK_EQ(fc(v), 0x00);
    // 止めたあとの要求は、次の垂直表示期間から新しく始まる
    fill(v, 0x0315, 0x1234);
    v.crtcWrite(0x481, 0x02);
    CHECK_EQ(fc(v), 0x02);
    v.beginVerticalDisplay();
    CHECK_EQ(all(v, 0x0000), true);
    v.beginVerticalDisplay();
    CHECK_EQ(fc(v), 0x00);

    // 15kHz のインタレース (512 ライン) も 2 フィールドかかる。15kHz の 256 ラインは 1 フィールド
    fill(v, 0x0305, 0x1234);
    v.crtcWrite(0x481, 0x02);
    v.beginVerticalDisplay();
    CHECK_EQ(all(v, 0x0000), true);
    CHECK_EQ(fc(v), 0x02);
    v.beginVerticalDisplay();
    CHECK_EQ(fc(v), 0x02);
    v.beginVerticalDisplay();
    CHECK_EQ(fc(v), 0x00);
    fill(v, 0x0301, 0x1234);
    v.crtcWrite(0x481, 0x02);
    v.beginVerticalDisplay();
    CHECK_EQ(fc(v), 0x02);
    v.beginVerticalDisplay();
    CHECK_EQ(fc(v), 0x00);
}

uint16_t word(const x68k::Video& v, int x, int y) { return v.gvramWord((uint32_t)(y * 512 + x)); }

// 消す範囲: 表示の大きさ、スクロールの位置、実画面 1024x1024
void testFastClearArea() {
    printf("fast clear area\n");
    x68k::Video& v = g_video;
    v.reset();
    // 256x256 の表示 (31kHz、256 ドット、256 ラインの 2 度読み): 水平 R02..R03 が 32 文字、垂直 R06..R07 が 512 ラスタ
    crtc(v, 2, 0x000a);
    crtc(v, 3, 0x002a);
    crtc(v, 6, 0x0028);
    crtc(v, 7, 0x0228);
    fill(v, 0x0010, 0xffff);
    crtc(v, 21, 0x0001);
    v.crtcWrite(0x481, 0x02);
    v.beginVerticalDisplay();
    CHECK_EQ(word(v, 0, 0), 0xfff0);
    CHECK_EQ(word(v, 255, 255), 0xfff0);
    CHECK_EQ(word(v, 256, 0), 0xffff); // 表示の外は残る
    CHECK_EQ(word(v, 0, 256), 0xffff);
    CHECK_EQ(word(v, 300, 300), 0xffff);
    v.beginVerticalDisplay();
    CHECK_EQ(fc(v), 0x00);

    // スクロール: 位置はページ 0 の R12 / R13 から。(400, 300) にすると、消えるのは x = 400..511 と 0..143、
    // y = 300..511 と 0..43。ほかのページを選んでも、ページ 0 のスクロールを使う (ページ 1 の R14 / R15 は関係しない)
    fill(v, 0x0010, 0xffff);
    crtc(v, 12, 400);
    crtc(v, 13, 300);
    crtc(v, 14, 0x0010);
    crtc(v, 15, 0x0020);
    crtc(v, 21, 0x0002);
    v.crtcWrite(0x481, 0x02);
    v.beginVerticalDisplay();
    CHECK_EQ(word(v, 400, 300), 0xff0f);
    CHECK_EQ(word(v, 511, 511), 0xff0f);
    CHECK_EQ(word(v, 0, 0), 0xff0f);
    CHECK_EQ(word(v, 143, 43), 0xff0f);
    CHECK_EQ(word(v, 144, 43), 0xffff);
    CHECK_EQ(word(v, 143, 44), 0xffff);
    CHECK_EQ(word(v, 399, 300), 0xffff);
    CHECK_EQ(word(v, 400, 299), 0xffff);
    v.beginVerticalDisplay();
    // X のスクロールの bit0 は見ない: 7 は 6 から
    fill(v, 0x0010, 0xffff);
    crtc(v, 12, 7);
    crtc(v, 13, 0);
    crtc(v, 21, 0x0001);
    v.crtcWrite(0x481, 0x02);
    v.beginVerticalDisplay();
    CHECK_EQ(word(v, 5, 0), 0xffff);
    CHECK_EQ(word(v, 6, 0), 0xfff0);
    CHECK_EQ(word(v, 261, 0), 0xfff0);
    CHECK_EQ(word(v, 262, 0), 0xffff);
    v.beginVerticalDisplay();

    // 実画面 1024x1024、768x512 の表示 (リセットのあとの CRTC の値)。走査のラインは上半分なので、ページ 0 と 1 の
    // 全体が消える。CP は 0 でも消える
    v.reset();
    fill(v, 0x0416, 0xffff);
    crtc(v, 21, 0x0000);
    v.crtcWrite(0x481, 0x02);
    v.beginVerticalDisplay();
    CHECK_EQ(word(v, 5, 5), 0xff00);
    CHECK_EQ(all(v, 0xff00), true); // 下の 2 ページ (2、3) は残る
    v.beginVerticalDisplay();
    CHECK_EQ(fc(v), 0x00);

    // 実画面 1024x1024 の Y のスクロールは R13 の下位 10 ビット。256x256 の表示で (1000, 700) にすると、
    // ライン 700..955 は下半分なので、ページ 2 と 3 の y = 188..443、x = 488..511 と 0..231 が消える
    crtc(v, 2, 0x000a);
    crtc(v, 3, 0x002a);
    fill(v, 0x0410, 0xffff);
    crtc(v, 12, 1000);
    crtc(v, 13, 700);
    crtc(v, 21, 0x0001);
    v.crtcWrite(0x481, 0x02);
    v.beginVerticalDisplay();
    CHECK_EQ(word(v, 488, 188), 0x00ff);
    CHECK_EQ(word(v, 231, 443), 0x00ff);
    CHECK_EQ(word(v, 232, 188), 0xffff);
    CHECK_EQ(word(v, 487, 188), 0xffff);
    CHECK_EQ(word(v, 488, 187), 0xffff);
    CHECK_EQ(word(v, 488, 444), 0xffff);
    v.beginVerticalDisplay();
    // 上半分と下半分にまたがる: Y = 496 から 256 ライン。ページ 0 と 1 の y = 496..511、ページ 2 と 3 の y = 0..239
    fill(v, 0x0410, 0xffff);
    crtc(v, 12, 0);
    crtc(v, 13, 496);
    v.crtcWrite(0x481, 0x02);
    v.beginVerticalDisplay();
    CHECK_EQ(word(v, 0, 495), 0xffff);
    CHECK_EQ(word(v, 0, 496), 0xff00);
    CHECK_EQ(word(v, 255, 511), 0xff00);
    CHECK_EQ(word(v, 0, 0), 0x00ff);
    CHECK_EQ(word(v, 255, 239), 0x00ff);
    CHECK_EQ(word(v, 0, 240), 0xffff);
    CHECK_EQ(word(v, 256, 0), 0xffff);
    v.beginVerticalDisplay();

    // 実画面 1024x1024 で、表示の高さが 512 ラインを超える場合。31kHz の 1024 ラインのインタレース (R20 = $0519) で、
    // R06..R07 が 300 ラスタなので 600 ライン、幅は 64 文字で 512 ドット。Y は R13 = $0B54 の下位 10 ビットで 852。
    // ライン 852..1023 (下半分。ページ 2 と 3 の y = 340..511) と 0..427 (上半分。ページ 0 と 1 の y = 0..427) が消える。
    // CP (R21 = $000D) は見ない
    crtc(v, 2, 0x001c);
    crtc(v, 3, 0x005c);
    crtc(v, 6, 0x0028);
    crtc(v, 7, 0x0028 + 300);
    fill(v, 0x0519, 0xffff);
    crtc(v, 12, 0);
    crtc(v, 13, 0x0b54);
    crtc(v, 21, 0x000d);
    v.crtcWrite(0x481, 0x02);
    v.beginVerticalDisplay();
    CHECK_EQ(word(v, 0, 0), 0xff00);
    CHECK_EQ(word(v, 511, 339), 0xff00);
    CHECK_EQ(word(v, 0, 340), 0x0000);
    CHECK_EQ(word(v, 511, 427), 0x0000);
    CHECK_EQ(word(v, 0, 428), 0x00ff);
    CHECK_EQ(word(v, 511, 511), 0x00ff);
    v.beginVerticalDisplay();
}

void testPalette65536() {
    printf("palette tables of the 65536 color mode\n");
    x68k::Video& v = g_video;
    v.reset();
    // CL = $E82000 + PL x 2 - (PL & 1)、CH = $E82002 + PH x 2 - (PH & 1)
    palette(v, 0, 0x1234); // $E82000 = PL $00、$E82001 = PL $01
    palette(v, 1, 0x5678); // $E82002 = PH $00、$E82003 = PH $01
    palette(v, 2, 0x9abc); // $E82004 = PL $02、$E82005 = PL $03
    palette(v, 254, 0xa1b2); // $E821FC = PL $FE、$E821FD = PL $FF
    palette(v, 255, 0xc3d4); // $E821FE = PH $FE、$E821FF = PH $FF
    CHECK_EQ(v.color64kLow()[0x00], 0x12);
    CHECK_EQ(v.color64kLow()[0x01], 0x34);
    CHECK_EQ(v.color64kHigh()[0x00], 0x5600);
    CHECK_EQ(v.color64kHigh()[0x01], 0x7800);
    CHECK_EQ(v.color64kLow()[0x02], 0x9a);
    CHECK_EQ(v.color64kLow()[0x03], 0xbc);
    CHECK_EQ(v.color64kLow()[0xfe], 0xa1);
    CHECK_EQ(v.color64kLow()[0xff], 0xb2);
    CHECK_EQ(v.color64kHigh()[0xfe], 0xc300);
    CHECK_EQ(v.color64kHigh()[0xff], 0xd400);
    CHECK_EQ(v.color64kHigh()[0x02], 0x0000); // 書いていない所はそのまま
    // ワードとしての値と XRGB
    CHECK_EQ(v.paletteColors()[1], 0x5678);
    CHECK_EQ(v.paletteXrgb()[1], x68k::paletteToRgb(0x5678));
    CHECK_EQ(v.paletteRead(0x1fe), 0xc3);

    // ワード 2k と 2k+1 を (2k << 8) | (2k+1) にすると、画素の値がそのまま色になる
    for (int k = 0; k < 128; k++) {
        palette(v, 2 * k, (uint16_t)((2 * k) << 8 | (2 * k + 1)));
        palette(v, 2 * k + 1, (uint16_t)((2 * k) << 8 | (2 * k + 1)));
    }
    int wrong = 0;
    for (int i = 0; i < 256; i++)
        if (v.color64kHigh()[i] != (i << 8) || v.color64kLow()[i] != i) wrong++;
    CHECK_EQ(wrong, 0);

    // テキストパレット ($E82200-) は 65536 色の表に関係しない
    palette(v, 256, 0xffff);
    palette(v, 257, 0xffff);
    CHECK_EQ(v.color64kLow()[0], 0x00);
    CHECK_EQ(v.color64kHigh()[0], 0x0000);
    CHECK_EQ(v.paletteColors()[256], 0xffff);
}

} // namespace

int main() {
    testFastClear();
    testFastClearArea();
    testPalette65536();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
