// Machine が LineListener (画面を合成する相手) を呼ぶタイミングのテスト。
// 仕様: 垂直表示期間 (CRTC の R06 から R07 の手前まで) のラスタごとに onRaster() を 1 回、垂直表示期間を抜けたところで
// onFrameEnd() を 1 回呼ぶ。水平表示期間のないラスタ (R02 >= R03、または R02 が R00 + 1 以上) でも onRaster() は呼ぶ。
// そのとき Renderer は幅 0 の画面として扱う
#include <stdio.h>
#include <string.h>

#include "x68k/machine.h"
#include "x68k/render.h"

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

uint8_t g_ipl[x68k::Machine::IPL_SIZE];
uint32_t g_pixels[x68k::Renderer::MAX_WIDTH * x68k::Renderer::MAX_HEIGHT];

// 合成はそのまま Renderer に任せ、呼ばれ方だけを数える
class Counter : public x68k::Renderer {
public:
    explicit Counter(const x68k::Video& v) : Renderer(v) {}
    void onRaster(int raster) override {
        if (raster != inFrame) outOfOrder++; // 0 から順に 1 つずつ来るはず
        inFrame++;
        Renderer::onRaster(raster);
    }
    void onFrameEnd() override {
        lastFrame = inFrame;
        inFrame = 0;
        frameEnds++;
        Renderer::onFrameEnd();
    }
    int inFrame = 0;    // 今のフレームで呼ばれた onRaster() の数
    int lastFrame = -1; // 終わったフレームでの数
    int outOfOrder = 0;
    int frameEnds = 0;
};

void crtc(x68k::Machine& m, int n, uint16_t v) {
    m.write8(0xe80000 + 2 * (uint32_t)n, (uint8_t)(v >> 8), m68k::FC_SUPER_DATA);
    m.write8(0xe80001 + 2 * (uint32_t)n, (uint8_t)v, m68k::FC_SUPER_DATA);
}

// 512x512 (31kHz) の画面にしてから、R02 と R03 を変え、そのあとのフレームでの呼ばれ方と幅を調べる
void testNoHorizontalDisplay(const char* name, uint16_t r02, uint16_t r03, bool wantWidth0) {
    printf("%s\n", name);
    memset(g_ipl, 0xff, sizeof g_ipl);
    const uint8_t vectors[8] = {0x00, 0x00, 0x20, 0x00, 0x00, 0xff, 0x00, 0x10}; // SSP = $2000、PC = $FF0010
    memcpy(g_ipl + 0x10000, vectors, sizeof vectors);
    g_ipl[0x10010] = 0x60; // bra.s * (CPU はここで回り続ける)
    g_ipl[0x10011] = 0xfe;
    static x68k::Machine m;
    m.setIplRom(g_ipl, sizeof g_ipl);
    m.powerOn();
    const uint16_t kRegs[9] = {91, 9, 17, 81, 567, 5, 40, 552, 27}; // 垂直表示期間は 40 から 551 までの 512 ラスタ
    for (int i = 0; i < 9; i++) crtc(m, i, kRegs[i]);
    crtc(m, 20, 0x0115);
    Counter c(m.video());
    c.setTarget(g_pixels, x68k::Renderer::MAX_WIDTH, x68k::Renderer::MAX_WIDTH, x68k::Renderer::MAX_HEIGHT);
    m.setLineListener(&c);
    for (int f = 0; f < 4; f++) m.runFrame();
    CHECK_EQ(c.lastFrame, 512);
    CHECK_EQ(c.width(), 512);
    CHECK_EQ(c.outOfOrder, 0);

    crtc(m, 2, r02);
    crtc(m, 3, r03);
    const int ends = c.frameEnds;
    for (int f = 0; f < 4; f++) m.runFrame();
    CHECK_EQ(c.frameEnds >= ends + 3, true);
    CHECK_EQ(c.lastFrame, 512); // 水平表示期間がなくても、表示するラスタごとに呼ばれる
    CHECK_EQ(c.outOfOrder, 0);
    if (wantWidth0) CHECK_EQ(c.width(), 0); // 合成が続いているので、幅が今の設定 (0) に更新される

    // 元に戻せば、また 512 画素の画面になる
    crtc(m, 2, kRegs[2]);
    crtc(m, 3, kRegs[3]);
    for (int f = 0; f < 4; f++) m.runFrame();
    CHECK_EQ(c.lastFrame, 512);
    CHECK_EQ(c.width(), 512);
    CHECK_EQ(c.outOfOrder, 0);
    m.setLineListener(nullptr);
}

} // namespace

int main() {
    testNoHorizontalDisplay("R02 = R03", 17, 17, true);
    testNoHorizontalDisplay("R02 > R03", 81, 17, true);
    testNoHorizontalDisplay("R02 = R00 + 1", 92, 100, false); // 境界: 水平表示期間の始まりが、ちょうどラスタの終わり
    testNoHorizontalDisplay("R02 > R00 + 1", 93, 100, false);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
