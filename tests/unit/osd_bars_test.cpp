// src/platform/osd_bars.h のテスト (画面の右下の、各コアとメモリの使用率のバー。#239): 出す値の丸め方、詰め方、
// 置く場所、絵、描き直すとき。重ねる絵を 3 つ (右上の数字、左上のメッセージ、右下のバー) 渡したときの
// presentScaled() と presentCrt()
#include <string.h>

#include <algorithm>
#include <vector>

#include "platform/crt_effect.h"
#include "platform/osd_bars.h"
#include "test_check.h"

using namespace platform;

namespace {

// フレームバッファへの書き込み (platform::DeviceStore と同じ形)。16 バイトにそろわない 16 / 64 バイトの書き込みを数える
struct PlainStore {
    typedef frame_scale_detail::V4 V4;
    static long misaligned;
    static void check(const void* p) {
        if (((uintptr_t)p & 15) != 0) misaligned++;
    }
    static void store4(uint32_t* p, uint32_t v) { memcpy(p, &v, 4); }
    static void store16(uint32_t* p, V4 v) {
        check(p);
        memcpy(p, &v, 16);
    }
    static void store64(uint32_t* p, V4 a, V4 b, V4 c, V4 d) {
        check(p);
        memcpy(p, &a, 16);
        memcpy(p + 4, &b, 16);
        memcpy(p + 8, &c, 16);
        memcpy(p + 12, &d, 16);
    }
};
long PlainStore::misaligned = 0;

void testShown() {
    printf("osdBarShown\n");
    CHECK_EQ(osdBarShown(-1, 0), 0);
    CHECK_EQ(osdBarShown(-1, 449), 45);
    CHECK_EQ(osdBarShown(-1, 424), 40);
    CHECK_EQ(osdBarShown(-1, 425), 45);
    CHECK_EQ(osdBarShown(-1, 1000), 100);
    CHECK_EQ(osdBarShown(-1, 5000), 100); // 1000 より大きい値は 100%
    // 前に出した値から 4% 未満なら変えない (境目の 42.5% の前後で行き来しない)
    CHECK_EQ(osdBarShown(45, 411), 45);
    CHECK_EQ(osdBarShown(45, 489), 45);
    CHECK_EQ(osdBarShown(40, 430), 40);
    CHECK_EQ(osdBarShown(45, 410), 40);
    CHECK_EQ(osdBarShown(45, 490), 50);
    CHECK_EQ(osdBarShown(0, 39), 0);
    CHECK_EQ(osdBarShown(0, 40), 5);
    CHECK_EQ(osdBarShown(100, 961), 100);
    CHECK_EQ(osdBarShown(100, 960), 95);
    // 出す値はいつも 5% の刻み
    int bad = 0;
    for (int shown = -1; shown <= 100; shown += (shown < 0 ? 1 : 5))
        for (unsigned p = 0; p <= 1100; p++) {
            const int v = osdBarShown(shown, p);
            if (v < 0 || v > 100 || v % 5 != 0) bad++;
        }
    CHECK_EQ(bad, 0);
}

void testColor() {
    printf("osdBarColor\n");
    CHECK_EQ(osdBarColor(0), OSD_BAR_GREEN);
    CHECK_EQ(osdBarColor(70), OSD_BAR_GREEN);
    CHECK_EQ(osdBarColor(75), OSD_BAR_YELLOW);
    CHECK_EQ(osdBarColor(90), OSD_BAR_YELLOW);
    CHECK_EQ(osdBarColor(95), OSD_BAR_RED);
    CHECK_EQ(osdBarColor(100), OSD_BAR_RED);
}

void testPack() {
    printf("packOsdBars / unpackOsdBars\n");
    OsdBars b;
    CHECK_EQ(packOsdBars(b), 1ull << 63); // どの行も出さない
    b.pct[0] = 0;
    b.pct[1] = 100;
    b.pct[2] = 45;
    b.pct[4] = 120; // 100 にする
    const uint64_t v = packOsdBars(b);
    CHECK_EQ(v >> 63, 1u);
    const OsdBars u = unpackOsdBars(v);
    CHECK_EQ(u.pct[0], 0);
    CHECK_EQ(u.pct[1], 100);
    CHECK_EQ(u.pct[2], 45);
    CHECK_EQ(u.pct[3], -1);
    CHECK_EQ(u.pct[4], 100);
    // packOsdBars() を通していない値でも 100% まで
    const OsdBars all = unpackOsdBars(~0ull);
    for (int i = 0; i < OSD_BAR_ROWS; i++) CHECK_EQ(all.pct[i], 100);
    // 値が違えば詰めた値も違う (描き直す)
    OsdBars c = b;
    c.pct[3] = 0;
    CHECK_EQ(packOsdBars(c) != v, true);
}

void testPlace() {
    printf("placeOsdBars\n");
    // 1920x1080 の aspect (1440x1080、左右に 240 ドット): 右の余白の下の端。右上の数字にぶつからず、重ねない
    const OsdRect top = placeOsdVertical(1920, 1080, 240, 0, 1440, 1080, 9);
    CHECK_EQ(top.w > 0 && !top.overlay, true);
    OsdRect r = placeOsdBars(OSD_MODE_ON, 1920, 1080, 240, 0, 1440, 1080, top);
    CHECK_EQ(r.w, OSD_BAR_W);
    CHECK_EQ(r.h, OSD_BAR_H);
    // 右端は右上の数字と同じ列。左は余白の 1 文字分 (8 ドット) だけ広い (#314)
    CHECK_EQ(r.x + r.w, top.x + top.w);
    CHECK_EQ(r.x, top.x - OSD_BAR_PAD_COLS * 8);
    CHECK_EQ(r.x & 3, 0);
    CHECK_EQ(r.y, 1080 - OSD_BAR_H - OSD_PAD_Y);
    CHECK_EQ(r.overlay, false);
    // osd=fps でも出す。osd=off では出さない
    CHECK_EQ(placeOsdBars(OSD_MODE_FPS, 1920, 1080, 240, 0, 1440, 1080, top).w, OSD_BAR_W);
    CHECK_EQ(placeOsdBars(OSD_MODE_OFF, 1920, 1080, 240, 0, 1440, 1080, top).w, 0);
    // X68000 の画面をまだ写していない
    CHECK_EQ(placeOsdBars(OSD_MODE_ON, 1920, 1080, 0, 0, 0, 0, top).w, 0);
    // full (余白がない): X68000 の画面に重ねる
    const OsdRect topFull = placeOsdVertical(1920, 1080, 0, 0, 1920, 1080, 9);
    r = placeOsdBars(OSD_MODE_ON, 1920, 1080, 0, 0, 1920, 1080, topFull);
    CHECK_EQ(r.w, OSD_BAR_W);
    CHECK_EQ(r.overlay, true);
    // 右上の数字にぶつかる (フレームバッファが低い) なら出さない
    const OsdRect topLow = placeOsdVertical(1920, 200, 240, 0, 1440, 200, 9);
    CHECK_EQ(topLow.w > 0, true);
    CHECK_EQ(placeOsdBars(OSD_MODE_ON, 1920, 200, 240, 0, 1440, 200, topLow).w, 0);
    // 右上の数字がないときは、ぶつかるものがない
    CHECK_EQ(placeOsdBars(OSD_MODE_ON, 1920, 200, 240, 0, 1440, 200, OsdRect()).w, OSD_BAR_W);
    // フレームバッファに入らない
    CHECK_EQ(placeOsdBars(OSD_MODE_ON, 100, 1080, 0, 0, 100, 1080, OsdRect()).w, 0);
    CHECK_EQ(placeOsdBars(OSD_MODE_ON, 1920, 60, 0, 0, 1920, 60, OsdRect()).w, 0);
}

void testRender() {
    printf("renderOsdBars\n");
    const OsdRect r = placeOsdBars(OSD_MODE_ON, 1920, 1080, 240, 0, 1440, 1080, OsdRect());
    CHECK_EQ(OSD_BAR_W, 128);
    // いちばん左の棒は、背景の左端から 10 ドット空ける (余白がないと 2 ドットで、詰まって見えた。#314)
    CHECK_EQ(osdBarX0(0), 10);
    CHECK_EQ(OSD_BAR_H, 80);
    std::vector<uint32_t> img(OSD_BAR_PIXELS, 0x12345678u);
    OsdBars b;
    b.pct[0] = 0;
    b.pct[1] = 50;
    b.pct[2] = 80;
    b.pct[3] = 100;
    renderOsdBars(img.data(), r, packOsdBars(b));
    auto at = [&](int x, int y) { return img[(size_t)y * OSD_BAR_W + x]; };
    // すべての画素を描く
    int untouched = 0;
    for (uint32_t p : img)
        if (p == 0x12345678u) untouched++;
    CHECK_EQ(untouched, 0);
    // 棒は縦。ラベルの文字の真上にあり、隣どうし重ならない
    for (int i = 0; i < OSD_BAR_ROWS; i++) {
        CHECK_EQ(osdBarX0(i) >= osdBarLabelCol(i) * 8, true);
        CHECK_EQ(osdBarX1(i) <= (osdBarLabelCol(i) + osdBarLabelLen(i)) * 8, true);
        CHECK_EQ(osdBarX1(i) - osdBarX0(i) >= 12, true);
        if (i > 0) CHECK_EQ(osdBarX0(i) > osdBarX1(i - 1), true);
    }
    CHECK_EQ(osdBarX1(OSD_BAR_ROWS - 1) <= OSD_BAR_W, true);
    // 塗る高さ: 0% は塗らず、100% は上端まで
    CHECK_EQ(osdBarFillY(0), OSD_BAR_BOTTOM);
    CHECK_EQ(osdBarFillY(100), OSD_BAR_TOP);
    const int x0 = osdBarX0(0) + 2, x1 = osdBarX0(1) + 2, x2 = osdBarX0(2) + 2, x3 = osdBarX0(3) + 2;
    // C0 は 0%: 棒はすべて使っていない色
    CHECK_EQ(at(x0, OSD_BAR_TOP), OSD_BAR_TRACK);
    CHECK_EQ(at(x0, OSD_BAR_BOTTOM - 1), OSD_BAR_TRACK);
    // C1 は 50%: 下半分が緑、上半分が使っていない色
    const int half = (OSD_BAR_TOP + OSD_BAR_BOTTOM) / 2;
    CHECK_EQ(at(x1, OSD_BAR_BOTTOM - 1), OSD_BAR_GREEN);
    CHECK_EQ(at(x1, half + 1), OSD_BAR_GREEN);
    CHECK_EQ(at(x1, half - 2), OSD_BAR_TRACK);
    // C2 は 80%: 黄。C3 は 100%: 上端まで赤
    CHECK_EQ(at(x2, OSD_BAR_BOTTOM - 1), OSD_BAR_YELLOW);
    CHECK_EQ(at(x3, OSD_BAR_TOP), OSD_BAR_RED);
    // 棒の外は背景か文字の色だけ
    int other = 0;
    for (int y = 0; y < OSD_BAR_H; y++)
        for (int x = 0; x < OSD_BAR_W; x++) {
            bool inBar = false;
            for (int i = 0; i < OSD_BAR_ROWS; i++)
                if (x >= osdBarX0(i) && x < osdBarX1(i) && y >= OSD_BAR_TOP && y < OSD_BAR_BOTTOM) inBar = true;
            if (!inBar && at(x, y) != OSD_FG && at(x, y) != OSD_BG) other++;
        }
    CHECK_EQ(other, 0);
    // ラベルの行は renderOsdRow() と同じ。MEM は出さない (pct が負) ので、ラベルも棒もない
    char text[OSD_BAR_COLS + 1];
    memcpy(text, " C0 C1 C2 C3    ", OSD_BAR_COLS + 1);
    std::vector<uint32_t> want(OSD_BAR_W);
    int bad = 0;
    for (int y = 0; y < 16; y++) {
        renderOsdRow(want.data(), text, y, 1, OSD_BAR_COLS);
        for (int x = 0; x < OSD_BAR_W; x++)
            if (at(x, OSD_BAR_LABEL_Y + y) != want[x]) bad++;
    }
    CHECK_EQ(bad, 0);
    int notBg = 0;
    for (int y = 0; y < OSD_BAR_LABEL_Y; y++)
        for (int x = osdBarX0(4); x < osdBarX1(4); x++)
            if (at(x, y) != OSD_BG) notBg++;
    CHECK_EQ(notBg, 0);
    // MEM も出すと、ラベルは「C0 C1 C2 C3 MEM」
    b.pct[4] = 30;
    renderOsdBars(img.data(), r, packOsdBars(b));
    memcpy(text, " C0 C1 C2 C3 MEM", OSD_BAR_COLS + 1);
    bad = 0;
    for (int y = 0; y < 16; y++) {
        renderOsdRow(want.data(), text, y, 1, OSD_BAR_COLS);
        for (int x = 0; x < OSD_BAR_W; x++)
            if (at(x, OSD_BAR_LABEL_Y + y) != want[x]) bad++;
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(at(osdBarX0(4) + 2, OSD_BAR_BOTTOM - 1), OSD_BAR_GREEN);
}

// OsdState で、値が変わったときだけ描く
void testState() {
    printf("OsdState with the bars\n");
    const OsdRect r = placeOsdBars(OSD_MODE_ON, 1920, 1080, 240, 0, 1440, 1080, OsdRect());
    OsdBars b;
    b.pct[0] = 10;
    OsdValue v;
    v.a = packOsdBars(b);
    OsdState st;
    OsdState::Step s = st.update(true, v, r);
    CHECK_EQ(s.draw, true);
    s = st.update(true, v, r);
    CHECK_EQ(s.draw, false);
    b.pct[0] = 15;
    v.a = packOsdBars(b);
    s = st.update(true, v, r);
    CHECK_EQ(s.draw, true);
    CHECK_EQ(s.erase, false);
    s = st.update(false, v, r);
    CHECK_EQ(s.erase, true);
    CHECK_EQ(s.draw, false);
}

// 重ねる絵 3 つ。右上と右下は同じ列 (横の範囲が同じ) で縦に離れている。絵の画素はそのまま出て、絵の外は絵がないときと同じ
struct Overlays {
    std::vector<uint32_t> pix[3];
    FrameOverlay ov[3];
    Overlays(const FrameScale& s) {
        const int w[3] = {64, 40, 64}, h[3] = {9, 6, 10};
        const int x[3] = {s.dstX + s.dstW - 64, s.dstX + 8, s.dstX + s.dstW - 64};
        const int y[3] = {s.dstY + 5, s.dstY + 7, s.dstY + s.dstH - 14};
        for (int i = 0; i < 3; i++) {
            pix[i].resize((size_t)w[i] * h[i]);
            for (size_t k = 0; k < pix[i].size(); k++) pix[i][k] = 0xff000000u + (uint32_t)(i << 16) + (uint32_t)k;
            ov[i].pixels = pix[i].data();
            ov[i].x = x[i];
            ov[i].y = y[i];
            ov[i].w = w[i];
            ov[i].h = h[i];
        }
    }
    // want に絵を重ねたものと fb を比べ、違う画素の数を返す
    int diff(const std::vector<uint32_t>& fb, const std::vector<uint32_t>& want, int pitch, int sw, int sh) const {
        int bad = 0;
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) {
                uint32_t w = want[(size_t)y * pitch + x];
                for (int i = 0; i < 3; i++) {
                    const FrameOverlay& o = ov[i];
                    if (x >= o.x && x < o.x + o.w && y >= o.y && y < o.y + o.h)
                        w = pix[i][(size_t)(y - o.y) * o.w + (x - o.x)];
                }
                if (fb[(size_t)y * pitch + x] != w) bad++;
            }
        return bad;
    }
};

std::vector<uint32_t> pattern(int pitch, int rows) {
    std::vector<uint32_t> v((size_t)pitch * rows);
    for (size_t i = 0; i < v.size(); i++) v[i] = (uint32_t)(i * 2654435761u) >> 8;
    return v;
}

void testThreeOverlaysScaled() {
    printf("presentScaled with three overlays\n");
    const int sw = 1920, sh = 1080, srcPitch = 1024, fw = 768, fh = 512;
    const std::vector<uint32_t> src = pattern(srcPitch, fh);
    const FrameScale s = computeFrameScale(fw, fh, sw, sh, true, 4096);
    std::vector<uint32_t> fb((size_t)sw * sh, 0x12345678), want((size_t)sw * sh, 0x12345678);
    std::vector<uint32_t> line((size_t)s.dstW + 3);
    const Overlays o(s);
    presentScaled<PlainStore>(src.data(), srcPitch, (uint8_t*)want.data(), (size_t)sw * 4, s, line.data());
    PlainStore::misaligned = 0;
    // 渡す順番を変えても同じ (左にあるものを先に並べ直す)
    const int orders[3][3] = {{0, 1, 2}, {2, 0, 1}, {1, 2, 0}};
    for (const auto& ord : orders) {
        std::fill(fb.begin(), fb.end(), 0x12345678u);
        presentScaled<PlainStore>(src.data(), srcPitch, (uint8_t*)fb.data(), (size_t)sw * 4, s, line.data(),
                                  &o.ov[ord[0]], nullptr, false, &o.ov[ord[1]], &o.ov[ord[2]]);
        CHECK_EQ(o.diff(fb, want, sw, sw, sh), 0);
    }
    CHECK_EQ(PlainStore::misaligned, 0);
    // 3 つ目だけ
    std::fill(fb.begin(), fb.end(), 0x12345678u);
    presentScaled<PlainStore>(src.data(), srcPitch, (uint8_t*)fb.data(), (size_t)sw * 4, s, line.data(), nullptr,
                              nullptr, false, nullptr, &o.ov[2]);
    int bad = 0;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            const FrameOverlay& b = o.ov[2];
            const bool in = x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
            const uint32_t w = in ? o.pix[2][(size_t)(y - b.y) * b.w + (x - b.x)] : want[(size_t)y * sw + x];
            if (fb[(size_t)y * sw + x] != w) bad++;
        }
    CHECK_EQ(bad, 0);
}

void testThreeOverlaysCrt() {
    printf("presentCrt with three overlays\n");
    const int sw = 1920, sh = 1080, srcPitch = 1024, fw = 768, fh = 512;
    const std::vector<uint32_t> src = pattern(srcPitch, 1024);
    const FrameScale base = computeFrameScale(fw, fh, sw, sh, true, 4096);
    const FrameScale s = videoFrameScale(VIDEO_SCALE_ASPECT, base, fw, fh, sw, sh, 4096);
    std::vector<uint32_t> mem(crtMapBytes(s.dstW, s.dstH) / 4 + 1);
    CrtMap m;
    crtMapInit(m, mem.data(), s.dstW, s.dstH);
    CHECK_EQ(crtBuildMap(m, CRT_SCANLINE | CRT_CURVE, s.srcW, s.srcH, s.dstW, s.dstH), true);
    std::vector<uint32_t> fb((size_t)sw * sh, 0x12345678), want((size_t)sw * sh, 0x12345678);
    std::vector<uint32_t> a((size_t)s.dstW + 3), b((size_t)s.dstW + 3);
    const Overlays o(s);
    presentCrt<PlainStore>(src.data(), srcPitch, (uint8_t*)want.data(), (size_t)sw * 4, s, m, a.data(), b.data());
    PlainStore::misaligned = 0;
    presentCrt<PlainStore>(src.data(), srcPitch, (uint8_t*)fb.data(), (size_t)sw * 4, s, m, a.data(), b.data(),
                           &o.ov[2], nullptr, &o.ov[1], nullptr, &o.ov[0]);
    CHECK_EQ(o.diff(fb, want, sw, sw, sh), 0);
    CHECK_EQ(PlainStore::misaligned, 0);
}

void testPermille() {
    printf("osdBarPermille\n");
    CHECK_EQ(osdBarPermille(250000, 1000000, false), 250u);
    CHECK_EQ(osdBarPermille(250000, 1000000, true), 750u); // コア 0: 0.25 秒待った
    CHECK_EQ(osdBarPermille(0, 1000000, true), 1000u);
    CHECK_EQ(osdBarPermille(1200000, 1000000, false), 1000u); // 区間より長ければ 100%
    CHECK_EQ(osdBarPermille(1200000, 1000000, true), 0u);
    CHECK_EQ(osdBarPermille(5, 0, false), 0u);
    CHECK_EQ(osdBarPermille(333, 1001, false), 332u);
    // 測った時間から出す値まで: 1.02 秒のうち 0.71 秒待ったコア 0 は 30%
    CHECK_EQ(osdBarShown(-1, osdBarPermille(710000, 1020000, true)), 30);
}

// 値が変わらなければ描かない、消してから出し直す、メニューを開いたあと (invalidate()) は描き直す、場所が変われば消して描く
void testStateMore() {
    printf("OsdState: erase, redisplay, invalidate, move\n");
    const OsdRect top = placeOsdVertical(1920, 1080, 240, 0, 1440, 1080, 9);
    const OsdRect r = placeOsdBars(OSD_MODE_ON, 1920, 1080, 240, 0, 1440, 1080, top);
    OsdBars b;
    b.pct[0] = 20;
    OsdValue v;
    v.a = packOsdBars(b);
    OsdState st;
    CHECK_EQ(st.update(true, v, r).draw, true);
    for (int i = 0; i < 3; i++) {
        const OsdState::Step s = st.update(true, v, r);
        CHECK_EQ(s.draw || s.erase, false);
    }
    // 消す (右上の数字を消したとき) → もう一度消すことはない → 出し直すと描く
    OsdState::Step s = st.update(false, v, r);
    CHECK_EQ(s.erase, true);
    CHECK_EQ(s.eraseRect.y, r.y);
    s = st.update(false, v, r);
    CHECK_EQ(s.erase || s.draw, false);
    s = st.update(true, v, r);
    CHECK_EQ(s.draw, true);
    // メニューがフレームバッファを描き直した (invalidate()): 同じ値でも描く
    st.invalidate();
    s = st.update(true, v, r);
    CHECK_EQ(s.draw, true);
    // 場所が変わった (余白から重ねるへ): 前の場所を消して新しい場所に描く
    const OsdRect full = placeOsdBars(OSD_MODE_ON, 1920, 1080, 0, 0, 1920, 1080, OsdRect());
    CHECK_EQ(full.overlay, true);
    s = st.update(true, v, full);
    CHECK_EQ(s.draw, true);
    CHECK_EQ(st.overlaid(), true);
}

// FrameRowCache の変わっていなくても写す範囲: 上と下に離れた 2 つは別に持ち、間の行は写さない
void testForceRanges() {
    printf("FrameRowCache::force\n");
    FrameRowCache c;
    c.force(10, 20);
    c.force(1000, 1080);
    CHECK_EQ(c.forceY0, 10);
    CHECK_EQ(c.forceY1, 20);
    CHECK_EQ(c.forceY2, 1000);
    CHECK_EQ(c.forceY3, 1080);
    CHECK_EQ(c.forced(15, 16), true);
    CHECK_EQ(c.forced(1079, 1080), true);
    CHECK_EQ(c.forced(20, 1000), false);
    // 近いほうに合わせる (左上のメッセージは右上の数字と同じ範囲へ)
    c.force(5, 30);
    CHECK_EQ(c.forceY0, 5);
    CHECK_EQ(c.forceY1, 30);
    CHECK_EQ(c.forceY2, 1000);
    c.force(900, 950);
    CHECK_EQ(c.forceY2, 900);
    CHECK_EQ(c.forceY1, 30);
    // 重なれば 1 つにする
    c.force(25, 905);
    CHECK_EQ(c.forceY0, 5);
    CHECK_EQ(c.forceY1, 1080);
    CHECK_EQ(c.forceY2 >= c.forceY3, true);
    c.clearForce();
    CHECK_EQ(c.forced(0, 2000), false);
    // 空の範囲は足さない
    c.force(7, 7);
    CHECK_EQ(c.forced(0, 2000), false);
}

// 右上の数字と右下のバーを同じフレームで描き直す: 写しありの presentScaled() / presentCrt() は、2 つの絵の行だけを
// 写し直し、間の変わっていない行は書かない。結果はすべて写したときと同じ
struct CountingStore {
    typedef frame_scale_detail::V4 V4;
    static long bytes;
    static void store4(uint32_t* p, uint32_t v) {
        bytes += 4;
        memcpy(p, &v, 4);
    }
    static void store16(uint32_t* p, V4 v) {
        bytes += 16;
        memcpy(p, &v, 16);
    }
    static void store64(uint32_t* p, V4 a, V4 b, V4 c, V4 d) {
        bytes += 64;
        memcpy(p, &a, 16);
        memcpy(p + 4, &b, 16);
        memcpy(p + 8, &c, 16);
        memcpy(p + 12, &d, 16);
    }
};
long CountingStore::bytes = 0;

void testCacheTopAndBottom() {
    printf("row cache: top and bottom overlays redrawn in the same frame\n");
    const int sw = 1920, sh = 1080, srcPitch = 1024, fw = 768, fh = 512;
    const std::vector<uint32_t> src = pattern(srcPitch, 1024);
    // presentScaled (2 倍)
    {
        const FrameScale s = computeFrameScale(fw, fh, sw, sh, true, 4096);
        std::vector<uint32_t> fb((size_t)sw * sh, 0x12345678), want((size_t)sw * sh, 0x12345678);
        std::vector<uint32_t> line((size_t)s.dstW + 3), prev((size_t)srcPitch * 1024);
        Overlays o(s);
        FrameRowCache cache;
        cache.pixels = prev.data();
        cache.pitch = srcPitch;
        cache.rows = 1024;
        presentScaled<CountingStore>(src.data(), srcPitch, (uint8_t*)fb.data(), (size_t)sw * 4, s, line.data(),
                                     &o.ov[0], &cache, false, &o.ov[1], &o.ov[2]);
        // 同じ画面で、上 (右上と左上) と下 (右下) の絵の行を写し直させる
        for (int i = 0; i < 3; i++) cache.force(o.ov[i].y, o.ov[i].y + o.ov[i].h);
        CHECK_EQ(cache.forceY2 < cache.forceY3, true); // 上と下は別の範囲
        // 写す元の行: 上は y = dstY+5 .. dstY+14 に重なる 5 行 (2 から 6)、下は 10 ラインに重なる 5 行。どれも 2 倍
        long rows = 0;
        for (int y = 0; y < s.srcH; y++)
            if (cache.forced(s.dstY + y * s.ky, s.dstY + (y + 1) * s.ky)) rows++;
        CHECK_EQ(rows, 10);
        CountingStore::bytes = 0;
        presentScaled<CountingStore>(src.data(), srcPitch, (uint8_t*)fb.data(), (size_t)sw * 4, s, line.data(),
                                     &o.ov[0], &cache, false, &o.ov[1], &o.ov[2]);
        CHECK_EQ(CountingStore::bytes, rows * s.ky * s.dstW * 4);
        CHECK_EQ(cache.forced(0, sh), false); // 写したあとは空
        presentScaled<PlainStore>(src.data(), srcPitch, (uint8_t*)want.data(), (size_t)sw * 4, s, line.data());
        CHECK_EQ(o.diff(fb, want, sw, sw, sh), 0);
    }
    // presentCrt (aspect、scanline と curve)
    {
        const FrameScale base = computeFrameScale(fw, fh, sw, sh, true, 4096);
        const FrameScale s = videoFrameScale(VIDEO_SCALE_ASPECT, base, fw, fh, sw, sh, 4096);
        std::vector<uint32_t> mem(crtMapBytes(s.dstW, s.dstH) / 4 + 1);
        CrtMap m;
        crtMapInit(m, mem.data(), s.dstW, s.dstH);
        CHECK_EQ(crtBuildMap(m, CRT_SCANLINE | CRT_CURVE, s.srcW, s.srcH, s.dstW, s.dstH), true);
        std::vector<uint32_t> fb((size_t)sw * sh, 0x12345678), want((size_t)sw * sh, 0x12345678);
        std::vector<uint32_t> a((size_t)s.dstW + 3), b((size_t)s.dstW + 3), prev((size_t)srcPitch * 1024);
        Overlays o(s);
        FrameRowCache cache;
        cache.pixels = prev.data();
        cache.pitch = srcPitch;
        cache.rows = 1024;
        presentCrt<CountingStore>(src.data(), srcPitch, (uint8_t*)fb.data(), (size_t)sw * 4, s, m, a.data(), b.data(),
                                  &o.ov[0], &cache, &o.ov[1], nullptr, &o.ov[2]);
        for (int i = 0; i < 3; i++) cache.force(o.ov[i].y, o.ov[i].y + o.ov[i].h);
        const long rows = 9 + 10; // 上は dstY+5 から dstY+14 の手前までの 9 行 (右上と左上を合わせた範囲)、下は 10 行
        CountingStore::bytes = 0;
        presentCrt<CountingStore>(src.data(), srcPitch, (uint8_t*)fb.data(), (size_t)sw * 4, s, m, a.data(), b.data(),
                                  &o.ov[0], &cache, &o.ov[1], nullptr, &o.ov[2]);
        CHECK_EQ(CountingStore::bytes, rows * s.dstW * 4);
        presentCrt<PlainStore>(src.data(), srcPitch, (uint8_t*)want.data(), (size_t)sw * 4, s, m, a.data(), b.data());
        CHECK_EQ(o.diff(fb, want, sw, sw, sh), 0);
    }
}

// 狭い並び (#314): 右上の数字が 1 行 11 文字のときは、バーも 88 ドットに詰めて同じ列に置く
void testNarrow() {
    printf("narrow bars\n");
    const OsdRect top = placeOsdVertical(960, 540, 96, 14, 768, 512, 6, OSD_V_ITEM);
    CHECK_EQ(osdIsNarrow(top), true);
    CHECK_EQ(top.overlay, false);
    const OsdRect r = placeOsdBars(OSD_MODE_ON, 960, 540, 96, 14, 768, 512, top);
    CHECK_EQ(r.w, OSD_BAR_NARROW_W);
    CHECK_EQ(r.w, 88);
    CHECK_EQ(r.h, OSD_BAR_H);
    CHECK_EQ(r.x, top.x);
    CHECK_EQ(r.y, 540 - OSD_BAR_H - OSD_PAD_Y);
    CHECK_EQ(r.overlay, false); // X68000 の画面 (右端 864) に重ならない
    // 棒は 5 本とも絵に入り、隣と重ならず、左に余白がある
    CHECK_EQ(osdBarX0(0, true), 6);
    for (int i = 0; i < OSD_BAR_ROWS; i++) {
        CHECK_EQ(osdBarX1(i, true) - osdBarX0(i, true), 12);
        if (i > 0) CHECK_EQ(osdBarX0(i, true) - osdBarX1(i - 1, true), 4);
    }
    CHECK_EQ(osdBarX1(OSD_BAR_ROWS - 1, true) <= OSD_BAR_NARROW_W, true);
    CHECK_EQ(OSD_BAR_NARROW_W * OSD_BAR_H <= OSD_BAR_PIXELS, true);
    std::vector<uint32_t> img(OSD_BAR_PIXELS, 0x12345678u);
    OsdBars b;
    b.pct[0] = 0;
    b.pct[1] = 50;
    b.pct[2] = 80;
    b.pct[3] = 100;
    b.pct[4] = 30;
    renderOsdBars(img.data(), r, packOsdBars(b));
    auto at = [&](int x, int y) { return img[(size_t)y * r.w + x]; };
    int untouched = 0;
    for (int i = 0; i < r.w * r.h; i++)
        if (img[(size_t)i] == 0x12345678u) untouched++;
    CHECK_EQ(untouched, 0);
    CHECK_EQ(img[(size_t)r.w * r.h], 0x12345678u); // 絵の外には書かない
    CHECK_EQ(at(osdBarX0(0, true) + 2, OSD_BAR_BOTTOM - 1), OSD_BAR_TRACK);
    CHECK_EQ(at(osdBarX0(1, true) + 2, OSD_BAR_BOTTOM - 1), OSD_BAR_GREEN);
    CHECK_EQ(at(osdBarX0(2, true) + 2, OSD_BAR_BOTTOM - 1), OSD_BAR_YELLOW);
    CHECK_EQ(at(osdBarX0(3, true) + 2, OSD_BAR_TOP), OSD_BAR_RED);
    CHECK_EQ(at(osdBarX0(4, true) + 2, OSD_BAR_BOTTOM - 1), OSD_BAR_GREEN);
    CHECK_EQ(at(2, OSD_BAR_TOP + 4), OSD_BG); // 左の余白
    // ラベルの行は「 0 1 2 3 M 」
    std::vector<uint32_t> want(OSD_BAR_NARROW_W);
    int bad = 0;
    for (int y = 0; y < 16; y++) {
        renderOsdRow(want.data(), " 0 1 2 3 M ", y, 1, OSD_BAR_NARROW_COLS);
        for (int x = 0; x < OSD_BAR_NARROW_W; x++)
            if (at(x, OSD_BAR_LABEL_Y + y) != want[(size_t)x]) bad++;
    }
    CHECK_EQ(bad, 0);
    // 右の余白がちょうど 128 ドット (1920x1080 に 1664x1024 の枠): 右上の数字は重ならない。左に余白を足したバー
    // (128 ドット幅) は 8 ドット重なるので、狭い並びのバーを右端にそろえて置き、重ねない
    const OsdRect top128 = placeOsdVertical(1920, 1080, 128, 28, 1664, 1024, 9);
    CHECK_EQ(top128.x, 1792);
    CHECK_EQ(top128.overlay, false);
    const OsdRect b128 = placeOsdBars(OSD_MODE_ON, 1920, 1080, 128, 28, 1664, 1024, top128);
    CHECK_EQ(b128.w, OSD_BAR_NARROW_W);
    CHECK_EQ(b128.x + b128.w, top128.x + top128.w);
    CHECK_EQ(b128.x >= 128 + 1664, true);
    CHECK_EQ(b128.overlay, false);
    // 余白が 136 ドットあれば、ふつうの幅で重ならない
    const OsdRect top136 = placeOsdVertical(1920, 1080, 120, 28, 1664, 1024, 9);
    const OsdRect b136 = placeOsdBars(OSD_MODE_ON, 1920, 1080, 120, 28, 1664, 1024, top136);
    CHECK_EQ(b136.w, OSD_BAR_W);
    CHECK_EQ(b136.overlay, false);
    // 余白がなくて狭い並びでも重なるとき (full) は、今までどおりふつうの幅を重ねる
    const OsdRect topFull = placeOsdVertical(1920, 1080, 0, 0, 1920, 1080, 9);
    const OsdRect bFull = placeOsdBars(OSD_MODE_ON, 1920, 1080, 0, 0, 1920, 1080, topFull);
    CHECK_EQ(bFull.w, OSD_BAR_W);
    CHECK_EQ(bFull.overlay, true);
    // 右上の数字が横 1 行 (幅がたまたま 88 でも vertical でない) のときは、ふつうの幅
    OsdRect h = top;
    h.vertical = false;
    CHECK_EQ(placeOsdBars(OSD_MODE_ON, 1920, 1080, 240, 0, 1440, 1080, h).w, OSD_BAR_W);
}


// 余白の境目 (#314)。fbW が 4 の倍数でなくても、右端は右上の数字とそろい、右上の数字が重ならないときはバーも重ならない
void testNarrowBoundaries() {
    printf("bars at the margin boundaries\n");
    struct Case { int margin, topW, barW; bool overlay; };
    // 右の余白 (fbW = 1920、X68000 の画面は左端から fbW - 余白 まで) ごとの、右上の数字の幅とバーの幅
    const Case cases[] = {
        {95, OSD_V_COLS * 8, OSD_BAR_W, true},              // どちらも入らない: 15 文字を重ね、バーもふつうの幅を重ねる
        {96, OSD_V_ITEM * 8, OSD_BAR_NARROW_W, false},      // 11 文字がちょうど入る
        {127, OSD_V_ITEM * 8, OSD_BAR_NARROW_W, false},
        {128, OSD_V_COLS * 8, OSD_BAR_NARROW_W, false},     // 15 文字は入るが、左の余白を足したバーは入らない
        {135, OSD_V_COLS * 8, OSD_BAR_NARROW_W, false},
        {136, OSD_V_COLS * 8, OSD_BAR_W, false},            // どちらも入る
        {240, OSD_V_COLS * 8, OSD_BAR_W, false},
    };
    for (const Case& c : cases) {
        const int picW = 1920 - c.margin;
        const OsdRect top = chooseOsd(OSD_MODE_ON, 1920, 1080, 0, 14, picW, 1052, OSD_CHARS, 9, 10);
        CHECK_EQ(top.vertical, true);
        CHECK_EQ(top.w, c.topW);
        CHECK_EQ(top.overlay, c.overlay);
        const OsdRect b = placeOsdBars(OSD_MODE_ON, 1920, 1080, 0, 14, picW, 1052, top);
        CHECK_EQ(b.w, c.barW);
        CHECK_EQ(b.overlay, c.overlay);
        CHECK_EQ(b.x + b.w, top.x + top.w);
    }
    // 4 の倍数でない幅と、すべての余白: フレームバッファに入り、左端は 4 の倍数、右端は右上の数字とそろい、
    // overlay は実際の重なりと同じで、右上の数字が重ならないときはバーも重ならない
    int bad = 0;
    for (int fbW = 900; fbW <= 1923; fbW += (fbW < 1916 ? 61 : 1))
        for (int margin = 0; margin <= 160; margin++) {
            const int picW = fbW - margin;
            const OsdRect top = chooseOsd(OSD_MODE_ON, fbW, 1080, 0, 14, picW, 1052, OSD_CHARS, 9, 10);
            const OsdRect b = placeOsdBars(OSD_MODE_ON, fbW, 1080, 0, 14, picW, 1052, top);
            if (top.w <= 0 || !top.vertical || b.w <= 0) { bad++; continue; }
            const bool overlap = b.x < picW;
            if (b.x < 0 || b.x + b.w > fbW || (b.x & 3) != 0) bad++;
            if (b.x + b.w != top.x + top.w) bad++;
            if (b.overlay != overlap) bad++;
            if (!top.overlay && b.overlay) bad++;
        }
    CHECK_EQ(bad, 0);
}

} // namespace

int main() {
    testNarrow();
    testNarrowBoundaries();
    testPermille();
    testStateMore();
    testForceRanges();
    testCacheTopAndBottom();
    testShown();
    testColor();
    testPack();
    testPlace();
    testRender();
    testState();
    testThreeOverlaysScaled();
    testThreeOverlaysCrt();
    return finish();
}
