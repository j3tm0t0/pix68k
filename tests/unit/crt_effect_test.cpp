// src/platform/crt_effect.h のテスト: 表の作り方 (桁、区間、明るさ)、端の画素、明るさを掛ける計算とにじみ (ベクトルを
// 使う書き方と使わない書き方が同じ結果になること)、フレームバッファへの写し (書く番地、変わっていない行を書かないこと、
// 重ねる絵)。crt=off と crt=scanline のときに使う presentScaled() の結果が変わっていないことも、結果のハッシュで確かめる
#include <string.h>

#include <vector>

#include "platform/crt_effect.h"
#include "platform/frame_handoff.h"
#include "platform/osd.h"
#include "platform/osd_message.h"
#include "test_check.h"

using namespace platform;

namespace {

struct CheckingStore {
    typedef frame_scale_detail::V4 V4;
    static long n4, n16, n64, misaligned;
    static void reset() { n4 = n16 = n64 = misaligned = 0; }
    static long bytes() { return n4 * 4 + n16 * 16 + n64 * 64; }
    static void check(const void* p) {
        if (((uintptr_t)p & 15) != 0) misaligned++;
    }
    static void store4(uint32_t* p, uint32_t v) {
        n4++;
        memcpy(p, &v, 4);
    }
    static void store16(uint32_t* p, V4 v) {
        n16++;
        check(p);
        memcpy(p, &v, 16);
    }
    static void store64(uint32_t* p, V4 a, V4 b, V4 c, V4 d) {
        n64++;
        check(p);
        memcpy(p, &a, 16);
        memcpy(p + 4, &b, 16);
        memcpy(p + 8, &c, 16);
        memcpy(p + 12, &d, 16);
    }
};
long CheckingStore::n4, CheckingStore::n16, CheckingStore::n64, CheckingStore::misaligned;

// 表とそのメモリ
struct Map {
    std::vector<uint32_t> mem;
    CrtMap m;
    Map(int maxW, int maxH) : mem(crtMapBytes(maxW, maxH) / 4 + 1) { crtMapInit(m, mem.data(), maxW, maxH); }
};

std::vector<uint32_t> pattern(int pitch, int rows, uint32_t seed = 0) {
    std::vector<uint32_t> v((size_t)pitch * rows);
    // アルファは 0 のまま (合成した画面と同じ)
    for (size_t i = 0; i < v.size(); i++) v[i] = (uint32_t)((i + seed) * 2654435761u) >> 8;
    return v;
}

uint64_t fnv(const uint32_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// 表の中身がそろっているか (区間が左から右へ並び、行と桁が元の画面の中にあり、行の範囲が区間と合う)
void checkMapSane(const CrtMap& m) {
    int bad = 0, maxSegs = 0;
    for (int y = 0; y < m.dstH; y++) {
        const int n = (int)(m.segStart[y + 1] - m.segStart[y]);
        if (n < 1 || n > kCrtMaxSegs) bad++;
        if (n > maxSegs) maxSegs = n;
        int x = 0, lo = 32767, hi = -1;
        const uint16_t* sx = m.perPixel ? m.sx + (size_t)y * m.dstW : m.sx;
        for (uint32_t i = m.segStart[y]; i < m.segStart[y + 1]; i++) {
            const CrtSeg& sg = m.segs[i];
            if (sg.x1 <= x && !(sg.x1 == x && n == 1)) bad++;
            if (sg.row < -1 || sg.row >= m.srcH) bad++;
            if (sg.row >= 0) {
                if (sg.row < lo) lo = sg.row;
                if (sg.row > hi) hi = sg.row;
                int last = -1;
                for (int k = x; k < sg.x1; k++) {
                    if (sx[k] >= m.srcW) bad++;
                    if ((int)sx[k] < last) bad++; // 桁は左から右へ増える
                    last = sx[k];
                }
            }
            x = sg.x1;
        }
        if (x != m.dstW) bad++;
        if (hi >= 0 && (m.rowMin[y] != lo || m.rowMax[y] != hi)) bad++;
        if (hi < 0 && m.rowMin[y] <= m.rowMax[y]) bad++;
    }
    CHECK_EQ(bad, 0);
    printf("    at most %d segment(s) in a row\n", maxSegs);
}

void testPresets() {
    printf("presets\n");
    CHECK_EQ(crtParams(CRT_OFF).any(), false);
    CHECK_EQ(crtParams(-1).any(), false);
    CHECK_EQ(crtParams(CRT_PRESET_COUNT).any(), false);
    // 組み合わせは 3 つの効果のビット。scanline はスキャンライン、curve はゆがみとビネット、mask はアパーチャーグリルとにじみ
    for (int p = CRT_SCANLINE; p < CRT_PRESET_COUNT; p++) {
        const CrtParams c = crtParams(p);
        const bool scan = (p & CRT_SCANLINE) != 0, curve = (p & CRT_CURVE) != 0, mask = (p & CRT_MASK) != 0;
        CHECK_EQ(c.scanline == (scan ? kCrtScanline : 0), true);
        CHECK_EQ(c.curve == (curve ? kCrtCurve : 0) && c.vignette == (curve ? kCrtVignette : 0), true);
        CHECK_EQ(c.mask == mask && c.blur == mask, true);
        CHECK_EQ(crtNeedsMap(p), curve || mask);
        CHECK_EQ(c.any(), true);
    }
    // 前の crt= の組み合わせ
    CHECK_EQ((int)CRT_GRILLE, CRT_SCANLINE | CRT_MASK);
    CHECK_EQ((int)CRT_GEOM, CRT_SCANLINE | CRT_CURVE);
    CHECK_EQ((int)CRT_FULL, CRT_SCANLINE | CRT_CURVE | CRT_MASK);
    CHECK_EQ(strcmp(videoFilterName(CRT_OFF), "none"), 0);
    CHECK_EQ(strcmp(videoFilterName(CRT_FULL), "scanline,curve,mask"), 0);
    CHECK_EQ(strcmp(videoFilterName(CRT_CURVE | CRT_MASK), "curve,mask"), 0);
    CHECK_EQ(strcmp(videoFilterName(CRT_MASK), "mask"), 0);
    CHECK_EQ(strcmp(videoFilterName(99), "?"), 0);
    CHECK_EQ(strcmp(videoScaleName(VIDEO_SCALE_OFF), "off"), 0);
    CHECK_EQ(strcmp(videoScaleName(VIDEO_SCALE_INTEGER), "integer"), 0);
    CHECK_EQ(strcmp(videoScaleName(VIDEO_SCALE_ASPECT), "aspect"), 0);
    CHECK_EQ(strcmp(videoScaleName(VIDEO_SCALE_FULL), "full"), 0);
    CHECK_EQ(strcmp(videoScaleName(4), "?"), 0);
    // Pi 3 で選べるのは curve がなく、full と mask の組でない組み合わせ
    for (int sc = 0; sc < VIDEO_SCALE_COUNT; sc++)
        for (int p = 0; p < CRT_PRESET_COUNT; p++)
            CHECK_EQ(videoAllowedPi3(sc, p), (p & CRT_CURVE) == 0 && !(sc == VIDEO_SCALE_FULL && (p & CRT_MASK)));
    CHECK_EQ(videoAllowedPi3(VIDEO_SCALE_ASPECT, CRT_GRILLE) && videoAllowedPi3(VIDEO_SCALE_FULL, CRT_SCANLINE), true);
    // 枠は制限を変えない (枠を出せないと video_scale= のとおりに写すため)
    for (int bz : {CRT_BEZEL_DE, CRT_BEZEL_DB})
        for (int sc = 0; sc < VIDEO_SCALE_COUNT; sc++)
            for (int p = 0; p < CRT_PRESET_COUNT; p++) CHECK_EQ(videoAllowedPi3(sc, p | bz), videoAllowedPi3(sc, p));
    CHECK_EQ(videoAllowedPi3(VIDEO_SCALE_FULL, CRT_MASK | CRT_BEZEL_DE), false);
    CHECK_EQ(videoAllowedPi3(VIDEO_SCALE_INTEGER, CRT_GRILLE | CRT_BEZEL_DB), true);
}

// ゆがみのない表: 桁は整数倍、明るさは行ごとで、拡大した各行の最後の 1 行だけ半分
void testFlatMap(int srcW, int srcH, int dstW, int dstH) {
    printf("flat map %dx%d -> %dx%d\n", srcW, srcH, dstW, dstH);
    Map t(dstW, dstH);
    CHECK_EQ(crtBuildMap(t.m, CRT_GRILLE, srcW, srcH, dstW, dstH), true);
    const CrtMap& m = t.m;
    CHECK_EQ(m.perPixel, false);
    CHECK_EQ(m.kx, dstW / srcW);
    checkMapSane(m);
    const int kx = dstW / srcW, ky = dstH / srcH;
    int bad = 0;
    for (int x = 0; x < dstW; x++)
        if (m.sx[x] != x / kx) bad++;
    for (int y = 0; y < dstH; y++) {
        if (m.rowMin[y] != y / ky || m.rowMax[y] != y / ky) bad++;
        const int want = ky >= 2 && y % ky == ky - 1 ? 127 : 255;
        if (m.rowWeight[y] != want) bad++;
    }
    // アパーチャーグリル: 3 画素を周期に R、G、B が 1 色ずつそのまま (ほかの 2 色のバイトが 0xff)
    for (int x = 0; x < dstW; x++) {
        const uint32_t c = m.colMask[x];
        if ((c >> 24) != 0) bad++;
        for (int ch = 0; ch < 3; ch++)
            if (((c >> (16 - 8 * ch)) & 0xff) != (x % 3 == ch ? 0u : 0xffu)) bad++;
    }
    CHECK_EQ(bad, 0);
}

// crt=scanline の表で作った行は、presentScaled() のスキャンライン (scanline=on) と 1 ビットも違わない
void testFlatMatchesScanline(int fw, int fh) {
    printf("scanline preset equals presentScaled() with scanlines, %dx%d\n", fw, fh);
    const int sw = 1920, sh = 1080, srcPitch = 1024;
    const std::vector<uint32_t> src = pattern(srcPitch, 1024);
    const FrameScale s = computeFrameScale(fw, fh, sw, sh, true, 4096);
    std::vector<uint32_t> want((size_t)sw * sh, 0x11111111), got((size_t)sw * sh, 0x11111111);
    std::vector<uint32_t> a((size_t)s.dstW + 3), b((size_t)s.dstW + 3);
    presentScaled<CheckingStore>(src.data(), srcPitch, (uint8_t*)want.data(), (size_t)sw * 4, s, a.data(), nullptr, nullptr,
                                 true);
    Map t(s.dstW, s.dstH);
    CHECK_EQ(crtBuildMap(t.m, CRT_SCANLINE, s.srcW, s.srcH, s.dstW, s.dstH), true);
    CheckingStore::reset();
    presentCrt<CheckingStore>(src.data(), srcPitch, (uint8_t*)got.data(), (size_t)sw * 4, s, t.m, a.data(), b.data());
    CHECK_EQ(got == want, true);
    CHECK_EQ(CheckingStore::misaligned, 0);
    CHECK_EQ(CheckingStore::bytes(), (long)s.dstW * s.dstH * 4);
}

// 整数倍でない表 (ホスト版のウィンドウ): 桁と行は画素の真ん中をいちばん近い画素に写す。縦が 2 倍より小さければ暗くしない
void testFractionalMap() {
    printf("flat map with a fractional scale\n");
    {
        Map t(1000, 700);
        CHECK_EQ(crtBuildMap(t.m, CRT_SCANLINE, 768, 512, 1000, 700), true);
        const CrtMap& m = t.m;
        CHECK_EQ(m.kx, 0);
        checkMapSane(m);
        CHECK_EQ(m.sx[0], 0);
        CHECK_EQ(m.sx[999], 767);
        CHECK_EQ(m.rowMin[0], 0);
        CHECK_EQ(m.rowMin[699], 511);
        int bad = 0;
        for (int y = 0; y < 700; y++)
            if (m.rowWeight[y] != 255) bad++;
        CHECK_EQ(bad, 0);
        // 作った行は元の画素にアルファを足したもの
        const std::vector<uint32_t> src = pattern(1024, 512);
        std::vector<uint32_t> a(1003), b(1003);
        const uint32_t* line = crtRenderRow(m, src.data(), 1024, 350, a.data(), b.data());
        for (int x = 0; x < 1000; x++)
            if (line[x] != (src[(size_t)m.rowMin[350] * 1024 + m.sx[x]] | 0xff000000u)) bad++;
        CHECK_EQ(bad, 0);
    }
    {
        // 2.5 倍: 元の 1 行から作る表示の行のうち、最後の 1 行だけが半分の明るさ (中間の明るさは使わない)
        Map t(640, 640);
        CHECK_EQ(crtBuildMap(t.m, CRT_SCANLINE, 256, 256, 640, 640), true);
        const CrtMap& m = t.m;
        checkMapSane(m);
        // 元の 2 行が表示の 5 行: 表示の行の真ん中の位置は 0.2, 0.6 | 1.0, 1.4, 1.8 (元の行 0 が 2 行、行 1 が 3 行)
        const int want[5] = {255, 127, 255, 255, 127};
        int bad = 0;
        for (int y = 0; y < 640; y++)
            if (m.rowWeight[y] != want[y % 5]) bad++;
        CHECK_EQ(bad, 0);
    }
    {
        // aspect の枠 (512 ラインを 1080 ラインに): 明るさはそのままか半分の 2 つだけで、元の行ごとに暗い行がちょうど 1 つ
        // (むらに見える中間の明るさを使わない)。効果がなければどの行もそのまま
        Map t(1440, 1080);
        CHECK_EQ(crtBuildMap(t.m, CRT_SCANLINE, 768, 512, 1440, 1080), true);
        int bad = 0, dark[512] = {};
        for (int y = 0; y < 1080; y++) {
            if (t.m.rowWeight[y] != 255 && t.m.rowWeight[y] != 127) bad++;
            if (t.m.rowWeight[y] == 127) dark[t.m.rowMin[y]]++;
        }
        for (int r = 0; r < 512; r++)
            if (dark[r] != 1) bad++;
        CHECK_EQ(bad, 0);
        CHECK_EQ(crtBuildMap(t.m, CRT_OFF, 768, 512, 1440, 1080), true);
        for (int y = 0; y < 1080; y++)
            if (t.m.rowWeight[y] != 255) bad++;
        CHECK_EQ(bad, 0);
    }
}

// ゆがみのある表: 真ん中を通る線の上は拡大の倍率のまま、四隅は黒、明るさは四隅に近いほど暗い
void testCurvedMap(int srcW, int srcH, int dstW, int dstH) {
    printf("curved map %dx%d -> %dx%d\n", srcW, srcH, dstW, dstH);
    Map t(dstW, dstH);
    CHECK_EQ(crtBuildMap(t.m, CRT_FULL, srcW, srcH, dstW, dstH), true);
    const CrtMap& m = t.m;
    CHECK_EQ(m.perPixel, true);
    CHECK_EQ(m.kx, 0);
    checkMapSane(m);
    const int kx = dstW / srcW, ky = dstH / srcH;
    auto rowAt = [&](int x, int y) {
        int x0 = 0;
        for (uint32_t i = m.segStart[y]; i < m.segStart[y + 1]; i++) {
            if (x >= x0 && x < m.segs[i].x1) return (int)m.segs[i].row;
            x0 = m.segs[i].x1;
        }
        return -2;
    };
    // 真ん中の行: 桁は整数倍のまま (ゆがみは 1 画素より小さい) で、元の行は 1 つ
    {
        const int y = dstH / 2;
        int bad = 0;
        for (int x = 0; x < dstW; x++)
            if (m.sx[(size_t)y * dstW + x] != x / kx || rowAt(x, y) != y / ky) bad++;
        CHECK_EQ(bad, 0);
        CHECK_EQ(m.rowMin[y], m.rowMax[y]);
    }
    // 真ん中の桁: 元の行は整数倍のまま
    {
        int bad = 0;
        for (int y = 0; y < dstH; y++)
            if (rowAt(dstW / 2, y) != y / ky) bad++;
        CHECK_EQ(bad, 0);
    }
    // 四隅は黒 (元の画面の外)。上下左右の辺の真ん中は元の画面の端の行 / 桁
    CHECK_EQ(rowAt(0, 0), -1);
    CHECK_EQ(rowAt(dstW - 1, 0), -1);
    CHECK_EQ(rowAt(0, dstH - 1), -1);
    CHECK_EQ(rowAt(dstW - 1, dstH - 1), -1);
    CHECK_EQ(rowAt(dstW / 2, 0), 0);
    CHECK_EQ(rowAt(dstW / 2, dstH - 1), srcH - 1);
    CHECK_EQ(m.sx[(size_t)(dstH / 2) * dstW], 0);
    CHECK_EQ(m.sx[(size_t)(dstH / 2) * dstW + dstW - 1], srcW - 1);
    // 黒い画素は四隅だけで、全体の 5% より少ない。元の画面のどの行も、どこかの表示の行が使う
    {
        long black = 0;
        std::vector<int> used(srcH, 0);
        for (int y = 0; y < dstH; y++) {
            int x0 = 0;
            for (uint32_t i = m.segStart[y]; i < m.segStart[y + 1]; i++) {
                if (m.segs[i].row < 0)
                    black += m.segs[i].x1 - x0;
                else
                    used[m.segs[i].row] = 1;
                x0 = m.segs[i].x1;
            }
        }
        CHECK_EQ(black > 0 && black < (long)dstW * dstH / 20, true);
        int unused = 0;
        for (int v : used) unused += !v;
        CHECK_EQ(unused, 0);
    }
    // 明るさ: 真ん中の明るい行はほぼそのまま、同じ行の端は暗い (ビネット)。暗い筋は真ん中でも半分くらい
    {
        const int yb = (dstH / 2 / ky) * ky; // 拡大した行の最初
        const uint8_t* w = m.weight + (size_t)yb * dstW;
        CHECK_EQ(w[dstW / 2] >= 250, true);
        CHECK_EQ(w[4] < w[dstW / 2] - 10, true);
        const uint8_t* wd = m.weight + (size_t)(yb + ky - 1) * dstW;
        if (ky >= 2) CHECK_EQ(wd[dstW / 2] >= 120 && wd[dstW / 2] <= 135, true);
    }
    // 左右は対称 (表を左半分から作っているので、ぴったり同じ)。上下は、元の行が 1 行のずれまで同じ
    {
        int bad = 0;
        for (int y = 0; y < dstH; y++) {
            const uint16_t* sx = m.sx + (size_t)y * dstW;
            const uint8_t* w = m.weight + (size_t)y * dstW;
            const bool rows = y % 7 == 0;
            for (int x = 0; x < dstW / 2; x++) {
                const int mx = dstW - 1 - x;
                if (w[x] != w[mx]) bad++;
                if (!rows) continue;
                const int r0 = rowAt(x, y), r1 = rowAt(mx, y);
                if (r0 != r1) bad++;
                if (r0 >= 0 && sx[x] + sx[mx] != srcW - 1) bad++;
                const int r2 = rowAt(x, dstH - 1 - y);
                if ((r0 < 0) != (r2 < 0)) bad++;
                if (r0 >= 0 && (r0 + r2 < srcH - 2 || r0 + r2 > srcH)) bad++;
            }
        }
        CHECK_EQ(bad, 0);
    }
}

// 整数倍でない、奇数の大きさ (ホスト版のウィンドウ) のゆがみのある表: 中身がそろっていて、左右が対称
void testCurvedOdd() {
    printf("curved map with odd sizes\n");
    static const int sizes[][4] = {{768, 512, 1537, 1025}, {256, 256, 1001, 667}, {512, 512, 2303, 1535}, {768, 512, 17, 17}};
    for (const int* z : sizes) {
        Map t(z[2], z[3]);
        CHECK_EQ(crtBuildMap(t.m, CRT_FULL, z[0], z[1], z[2], z[3]), true);
        const CrtMap& m = t.m;
        checkMapSane(m);
        int bad = 0;
        for (int y = 0; y < m.dstH; y++) {
            const uint16_t* sx = m.sx + (size_t)y * m.dstW;
            const uint8_t* w = m.weight + (size_t)y * m.dstW;
            // 区間も左右で対称
            const uint32_t s0 = m.segStart[y], s1 = m.segStart[y + 1];
            for (uint32_t i = s0; i < s1; i++) {
                const uint32_t j = s1 - 1 - (i - s0);
                if (m.segs[i].row != m.segs[j].row) bad++;
                const int start = i > s0 ? m.segs[i - 1].x1 : 0;
                if (start != m.dstW - m.segs[j].x1) bad++;
            }
            for (int x = 0; x < m.dstW; x++) {
                const int mx = m.dstW - 1 - x;
                if (w[x] != w[mx]) bad++;
            }
            int x0 = 0;
            for (uint32_t i = s0; i < s1; i++) {
                if (m.segs[i].row >= 0)
                    for (int x = x0; x < m.segs[i].x1; x++)
                        if (x != m.dstW - 1 - x && sx[x] + sx[m.dstW - 1 - x] != m.srcW - 1) bad++;
                x0 = m.segs[i].x1;
            }
        }
        CHECK_EQ(bad, 0);
    }
}

void testBuildFails() {
    printf("maps that cannot be built\n");
    Map t(640, 480);
    CHECK_EQ(crtBuildMap(t.m, CRT_OFF, 256, 256, 512, 480), true); // 効果がなくても作る (整数倍でない枠で使う)
    CHECK_EQ(crtBuildMap(t.m, CRT_FULL, 256, 256, 768, 480), false); // 表より広い
    CHECK_EQ(crtBuildMap(t.m, CRT_FULL, 256, 256, 512, 512), false); // 表より高い
    CHECK_EQ(crtBuildMap(t.m, CRT_FULL, 0, 256, 512, 480), false);
    CHECK_EQ(t.m.valid, false);
    CHECK_EQ(crtBuildMap(t.m, CRT_FULL, 256, 240, 640, 480), true);
    CHECK_EQ(t.m.matches(CRT_FULL, 256, 240, 640, 480), true);
    CHECK_EQ(t.m.matches(CRT_GEOM, 256, 240, 640, 480), false);
    CHECK_EQ(t.m.matches(CRT_FULL, 256, 240, 636, 480), false);
    CrtMap none;
    CHECK_EQ(crtBuildMap(none, CRT_FULL, 256, 240, 640, 480), false);
}

// crtMapPrepare() の数え方 (ベアメタル版のログの「作り進めたフレームの数」と「作った表の数」)。作れなかった表は、
// そのフレームだけ作り進めたと数え、次のフレームからは数えない。作った表の数にも入れない
void testPrepareCounts() {
    printf("counting map builds\n");
    uint64_t ticks = 0;
    auto now = [&] { return ticks += 1; };
    struct Count {
        int frames = 0, worked = 0, finished = 0, ready = 0;
        void add(const CrtPrepared& r) {
            frames++;
            worked += r.worked;
            finished += r.finished;
            ready += r.ready;
        }
    };
    {
        // 少しずつ作る表: できあがったフレームで 1 つと数え、次のフレームから効果を掛ける (作り進めない)
        Map t(1536, 1024);
        Count c;
        while (c.ready == 0 && c.frames < 10000) c.add(crtMapPrepare(t.m, CRT_FULL, 768, 512, 1536, 1024, false, 40, now));
        CHECK_EQ(c.ready, 1);
        CHECK_EQ(c.finished, 1);
        CHECK_EQ(c.worked, c.frames - 1);
        CHECK_EQ(c.frames > 2, true);
        for (int i = 0; i < 3; i++) c.add(crtMapPrepare(t.m, CRT_FULL, 768, 512, 1536, 1024, false, 40, now));
        CHECK_EQ(c.ready, 4);
        CHECK_EQ(c.finished, 1);
        // X68000 が止まっているとき (finish) は 1 回で作る
        Count f;
        f.add(crtMapPrepare(t.m, CRT_GEOM, 768, 512, 1536, 1024, true, 0, now));
        CHECK_EQ(f.ready == 1 && f.worked == 1 && f.finished == 1, true);
        // ゆがみのない表 (mask だけ) は作り始めたフレームでできる
        Count g;
        g.add(crtMapPrepare(t.m, CRT_MASK, 768, 512, 1536, 1024, false, 40, now));
        CHECK_EQ(g.ready == 1 && g.worked == 1 && g.finished == 1, true);
    }
    {
        // 表より大きい: 最初のフレームだけ作り進めたと数え、作った表の数には入れない
        Map t(640, 480);
        Count c;
        for (int i = 0; i < 5; i++) c.add(crtMapPrepare(t.m, CRT_FULL, 256, 256, 768, 480, false, 40, now));
        CHECK_EQ(c.worked, 1);
        CHECK_EQ(c.finished, 0);
        CHECK_EQ(c.ready, 0);
    }
    {
        // 区間が入りきらない (強さを 4 倍にした部品で作る。testBuildFails() と同じ): 作りかけの表を作り進めて
        // 入りきらなかったフレームは数え、作った表には入れない。次のフレームからは作り進めない
        Map t(512, 480);
        CrtParams strong = crtParams(CRT_GEOM);
        strong.curve = kCrtCurve * 4;
        CHECK_EQ(crtBuildBegin(t.m, CRT_GEOM, 256, 2048, 512, 480, &strong), true);
        Count c;
        for (int i = 0; i < 2000 && (c.frames == 0 || t.m.building); i++)
            c.add(crtMapPrepare(t.m, CRT_GEOM, 256, 2048, 512, 480, false, 40, now));
        CHECK_EQ(t.m.building, false);
        CHECK_EQ(t.m.valid, false);
        CHECK_EQ(c.worked, c.frames);
        CHECK_EQ(c.finished, 0);
        Count after;
        for (int i = 0; i < 5; i++) after.add(crtMapPrepare(t.m, CRT_GEOM, 256, 2048, 512, 480, false, 40, now));
        for (int i = 0; i < 2; i++) after.add(crtMapPrepare(t.m, CRT_GEOM, 256, 2048, 512, 480, true, 0, now));
        CHECK_EQ(after.worked, 0);
        CHECK_EQ(after.finished, 0);
        CHECK_EQ(after.ready, 0);
    }
}

// 2 つの表の、使う部分が同じか
bool sameMap(const CrtMap& a, const CrtMap& b) {
    if (a.valid != b.valid || a.building != b.building || !a.tried(b.preset, b.srcW, b.srcH, b.dstW, b.dstH)) return false;
    if (!a.valid) return true;
    if (a.perPixel != b.perPixel || a.kx != b.kx) return false;
    const size_t w = (size_t)a.dstW, h = (size_t)a.dstH;
    if (memcmp(a.segStart, b.segStart, (h + 1) * sizeof(uint32_t)) != 0) return false;
    if (memcmp(a.segs, b.segs, a.segStart[h] * sizeof(CrtSeg)) != 0) return false;
    if (memcmp(a.colMask, b.colMask, w * 4) != 0) return false;
    if (memcmp(a.rowMin, b.rowMin, h * 2) != 0 || memcmp(a.rowMax, b.rowMax, h * 2) != 0) return false;
    if (memcmp(a.sx, b.sx, (a.perPixel ? w * h : w) * 2) != 0) return false;
    if (a.perPixel ? memcmp(a.weight, b.weight, w * h) != 0 : memcmp(a.rowWeight, b.rowWeight, h) != 0) return false;
    return true;
}

// 少しずつ作った表は、1 回で作った表と同じ。途中で別の組み合わせに変えたら、最初から作り直す
void testSlicedBuild() {
    printf("building the map a few rows at a time\n");
    static const int steps[] = {1, 7, 8, 64, 5000};
    for (int preset = CRT_OFF; preset < CRT_PRESET_COUNT; preset++) {
        Map whole(1536, 1024);
        CHECK_EQ(crtBuildMap(whole.m, preset, 768, 512, 1536, 1024), true);
        for (int step : steps) {
            Map t(1536, 1024);
            CHECK_EQ(crtBuildBegin(t.m, preset, 768, 512, 1536, 1024), true);
            CHECK_EQ(t.m.tried(preset, 768, 512, 1536, 1024), true);
            const bool perPixel = (preset & CRT_CURVE) != 0; // ゆがみのない表は軽いので、始めたときにできあがる
            CHECK_EQ(t.m.building, perPixel);
            CHECK_EQ(t.m.valid, !perPixel);
            int calls = 0;
            while (crtBuildStep(t.m, step)) {
                calls++;
                // 作りかけは使えない (presentCrt() も写さない)
                CHECK_EQ(t.m.valid, false);
                CHECK_EQ(t.m.matches(preset, 768, 512, 1536, 1024), false);
                CHECK_EQ(t.m.buildY, calls * step);
            }
            CHECK_EQ(calls, perPixel ? (1024 - 1) / step : 0);
            CHECK_EQ(t.m.valid, true);
            CHECK_EQ(t.m.building, false);
            CHECK_EQ(crtBuildStep(t.m, step), false); // できあがったあとは何もしない
            CHECK_EQ(sameMap(t.m, whole.m), true);
            checkMapSane(t.m);
        }
    }
    {
        // 作りかけで別の組み合わせ、別の大きさにする
        Map whole(1536, 1024), t(1536, 1024);
        CHECK_EQ(crtBuildMap(whole.m, CRT_GEOM, 256, 256, 1536, 1024), true);
        CHECK_EQ(crtBuildBegin(t.m, CRT_FULL, 768, 512, 1536, 1024), true);
        const uint32_t serial = t.m.serial;
        for (int i = 0; i < 40; i++) CHECK_EQ(crtBuildStep(t.m, 8), true);
        auto clock = [] { return (uint64_t)0; }; // 時刻が進まないので、最後まで作る
        CHECK_EQ(crtMapAdvance(t.m, CRT_GEOM, 256, 256, 1536, 1024, 0, clock), false); // budget が 0 なら始めるだけ
        CHECK_EQ(t.m.building, true);
        CHECK_EQ(t.m.buildY, 0);
        CHECK_EQ(t.m.serial, serial + 1);
        // 作り進めた呼び出しは、できあがっても false (効果を掛けるのは次のフレームから)
        CHECK_EQ(crtMapAdvance(t.m, CRT_GEOM, 256, 256, 1536, 1024, 1, clock), false);
        CHECK_EQ(t.m.valid, true);
        CHECK_EQ(t.m.serial, serial + 1); // 続きを作っただけ
        CHECK_EQ(sameMap(t.m, whole.m), true);
        // できている表は作り直さない
        CHECK_EQ(crtMapAdvance(t.m, CRT_GEOM, 256, 256, 1536, 1024, 1, clock), true);
        CHECK_EQ(crtMapFinish(t.m, CRT_GEOM, 256, 256, 1536, 1024), true);
        CHECK_EQ(t.m.serial, serial + 1);
        // crtMapFinish() は作りかけを最後まで作る
        CHECK_EQ(crtMapAdvance(t.m, CRT_FULL, 256, 256, 1536, 1024, 0, clock), false);
        CHECK_EQ(crtMapFinish(t.m, CRT_FULL, 256, 256, 1536, 1024), true);
        CHECK_EQ(crtBuildMap(whole.m, CRT_FULL, 256, 256, 1536, 1024), true);
        CHECK_EQ(sameMap(t.m, whole.m), true);
    }
    // 区間が多すぎて入りきらない (縦に長い画面を小さく出す)。作りかけのままにならず、同じものを作り直さない。
    // 1 行の左半分が使う元の行は、いちばん上の行で約 k * srcH / 2 で、ふだんの強さ (k = 0.0225) では 2048 ライン
    // (元の画面の上限) でも 23 行ほどで、区間の上限 (kCrtMaxSegs / 2 - 1 = 31) に届かない。今の強さで作れることを確かめ、
    // 入りきらない場合は、強さを 4 倍にした部品で作る (crtBuildBegin() の params)
    {
        Map t(512, 480);
        CHECK_EQ(crtBuildMap(t.m, CRT_FULL, 256, 2048, 512, 480), true);
        checkMapSane(t.m);
    }
    {
        Map t(512, 480);
        CrtParams strong = crtParams(CRT_GEOM);
        strong.curve = kCrtCurve * 4;
        CHECK_EQ(crtBuildBegin(t.m, CRT_GEOM, 256, 2048, 512, 480, &strong), true);
        CHECK_EQ(t.m.building, true);
        while (crtBuildStep(t.m, 8)) {
        }
        CHECK_EQ(t.m.building, false);
        CHECK_EQ(t.m.valid, false);
        CHECK_EQ(t.m.tried(CRT_GEOM, 256, 2048, 512, 480), true);
        // 同じ組み合わせは作り直さない (crtMapAdvance() と crtMapFinish() は表を作り始めず、false を返す)
        auto clock = [] { return (uint64_t)0; };
        const uint32_t serial = t.m.serial;
        CHECK_EQ(crtMapAdvance(t.m, CRT_GEOM, 256, 2048, 512, 480, 1, clock), false);
        CHECK_EQ(crtMapFinish(t.m, CRT_GEOM, 256, 2048, 512, 480), false);
        CHECK_EQ(t.m.serial, serial);
        CHECK_EQ(t.m.valid, false);
    }
}

// 表を作っている間にも、合成するコアは 1 フレームごとに次のフレームを渡す (platform/frame_handoff.h)。写す役のコア 0 が
// 表を 1 回で作ると (Pi 3 の 1920x1080 で 39ms)、その間に 2 フレームが渡され、写していないフレームが置き換わる
// (dropped())。フレームを写すたびに crtMapAdvance() で少しずつ作れば、どのフレームも写す。
// 時間は作りものの時計で進める (1 行を作る時間と 1 フレームを写す時間は Pi 3 で測った値)。sliceUs が 0 なら 1 回で作る。
// 置き換わったフレームの数を返す
struct BuildRun {
    uint32_t dropped = 0, shown = 0, shownPlain = 0, shownCrt = 0, slices = 0;
    uint64_t maxShowUs = 0;
};
BuildRun runBuildWhileFramesArrive(uint64_t sliceUs, CrtMap& m) {
    static constexpr uint64_t kFrameUs = 18026; // 55.46Hz
    static constexpr uint64_t kRowUs = 38;      // 表の 1 行 (39ms / 1024 行)
    static constexpr uint64_t kCopyUs = 10200;  // スキャンラインだけですべての行を写す時間の最大
    static constexpr uint32_t kFrames = 200;
    FrameHandoff h;
    BuildRun r;
    uint64_t t = 0;          // コア 0 の今の時刻
    uint32_t published = 0;  // 合成するコアは k * kFrameUs にフレーム k を渡す
    // 表の作った行の数だけ進む時計
    int rows0 = 0;
    uint64_t t0 = 0;
    auto now = [&] { return t0 + (uint64_t)(m.buildY - rows0) * kRowUs; };
    auto publishUntil = [&](uint64_t until) {
        while (published < kFrames && (uint64_t)published * kFrameUs <= until) {
            h.publish([](unsigned) {});
            published++;
        }
    };
    // 最初の 5 フレームは 256x256、そのあと 768x512 (画面モードが変わって、表を作り直す)
    uint32_t taken = 0;
    while (r.shown + h.dropped() < kFrames) {
        publishUntil(t);
        if (h.take() < 0) {
            t = (uint64_t)published * kFrameUs; // 次のフレームまで待つ
            continue;
        }
        taken = published; // 受け取ったのは、いちばん新しいフレーム
        const int sw = taken <= 5 ? 256 : 768, sh = taken <= 5 ? 256 : 512;
        // ShowFrame() と同じ: 表ができていれば効果を掛け、できていなければ作り進めてスキャンラインだけで写す
        rows0 = m.buildY;
        t0 = t;
        bool ready = m.matches(CRT_FULL, sw, sh, 1536, 1024);
        if (!ready) {
            if (!m.tried(CRT_FULL, sw, sh, 1536, 1024)) rows0 = 0; // 作り直すので 0 行から
            ready = sliceUs != 0 ? crtMapAdvance(m, CRT_FULL, sw, sh, 1536, 1024, sliceUs, now)
                                 : crtMapFinish(m, CRT_FULL, sw, sh, 1536, 1024);
            r.slices++;
        }
        const uint64_t showUs = now() - t + kCopyUs;
        if (showUs > r.maxShowUs) r.maxShowUs = showUs;
        if (ready)
            r.shownCrt++;
        else
            r.shownPlain++;
        r.shown++;
        t += showUs;
    }
    r.dropped = h.dropped();
    CHECK_EQ(h.published(), kFrames);
    return r;
}

void testBuildWhileFramesArrive() {
    printf("frames published while the map is being built\n");
    Map whole(1536, 1024);
    CHECK_EQ(crtBuildMap(whole.m, CRT_FULL, 768, 512, 1536, 1024), true);
    {
        // 1 回で作る (直す前): 作る間に来たフレームが置き換わる。この試験が置き換わりを見つけられることの確かめ
        Map t(1536, 1024);
        const BuildRun r = runBuildWhileFramesArrive(0, t.m);
        CHECK_EQ(r.dropped >= 2, true); // 表を 2 回作り (256x256、768x512)、どちらでも 1 フレーム以上
        CHECK_EQ(r.maxShowUs > 2 * 18026, true);
        CHECK_EQ(r.shown + r.dropped, 200u);
        CHECK_EQ(sameMap(t.m, whole.m), true);
    }
    for (uint64_t sliceUs : {500u, 2000u, 4000u}) {
        // 1 フレームに sliceUs ずつ作る: どのフレームも写し、1 フレームを写す時間は 1 フレームより短い
        Map t(1536, 1024);
        const BuildRun r = runBuildWhileFramesArrive(sliceUs, t.m);
        CHECK_EQ(r.dropped, 0u);
        CHECK_EQ(r.shown, 200u);
        // 時刻を見るのは kCrtBuildChunkRows 行ごと (1 行 38us)
        CHECK_EQ(r.maxShowUs <= 10200 + sliceUs + kCrtBuildChunkRows * 38, true);
        CHECK_EQ(r.maxShowUs < 18026, true);
        // できあがるまで (できあがったフレームも) スキャンラインだけで写し、次のフレームから効果を掛ける。256x256 の表は 5 フレームでは
        // できあがらない (39ms かかる) ので、作りかけのまま 768x512 の表に作り直す
        CHECK_EQ(r.shownPlain >= 5 + 39000 / (sliceUs + kCrtBuildChunkRows * 38), true);
        CHECK_EQ(r.shownPlain, r.slices);
        CHECK_EQ(r.shownCrt, 200u - r.shownPlain);
        CHECK_EQ(r.shownCrt > 100, true);
        CHECK_EQ(sameMap(t.m, whole.m), true);
    }
}

// 明るさを掛ける計算: どの長さでも、ベクトルを使わない書き方 (weighPixel()) と同じ。別のところへ書くときも同じ
void testWeighLine() {
    printf("weighLine against the scalar reference\n");
    int bad = 0;
    static const int scalars[] = {255, 127, 0, 1, 128, 200, 254};
    for (int n = 0; n <= 37; n++)
        for (int off = 0; off < 4; off++)
            for (int mode = 0; mode < 8 * 2; mode++) {
                const bool map = mode % 8 == 7, inPlace = mode < 8; // 7 と 15 は画素ごとの明るさ (15 は別のところへ書く)
                const uint32_t scalar = (uint32_t)scalars[(mode % 8) % 7];
                std::vector<uint32_t> buf(n + off + 8), dst(n + off + 8, 0xdeadbeefu);
                std::vector<uint8_t> w(n + 1);
                for (int i = 0; i < n + off + 8; i++) buf[i] = (uint32_t)(i * 2654435761u + n * 97u + mode);
                for (int i = 0; i < n; i++) w[i] = (uint8_t)(i * 37 + n);
                if (n > 2) {
                    buf[off] = 0xffffffffu; // いちばん明るい画素にいちばん大きい明るさ
                    w[0] = 255;
                    buf[off + 1] = 0x00ffffffu;
                    w[1] = 0;
                }
                const std::vector<uint32_t> before = buf;
                uint32_t* out = inPlace ? buf.data() : dst.data();
                crt_detail::weighLine(out + off, buf.data() + off, n, map ? w.data() : nullptr, scalar);
                for (int i = 0; i < n + off + 8; i++) {
                    const bool in = i >= off && i < off + n;
                    const uint32_t want = in ? crt_detail::weighPixel(before[i], map ? w[i - off] : scalar)
                                             : inPlace ? before[i] : 0xdeadbeefu;
                    if (out[i] != want) bad++;
                    if (!inPlace && buf[i] != before[i]) bad++;
                }
            }
    CHECK_EQ(bad, 0);
    // 255 はそのまま、127 は半分 (1 ビットの右シフト)、0 は 1/256
    CHECK_EQ(crt_detail::weighPixel(0x00ffffffu, 255), 0xffffffffu);
    CHECK_EQ(crt_detail::weighPixel(0x00123456u, 255), 0xff123456u);
    CHECK_EQ(crt_detail::weighPixel(0x00ffffffu, 127), 0xff7f7f7fu);
    CHECK_EQ(crt_detail::weighPixel(0x00030201u, 127), 0xff010100u);
    CHECK_EQ(crt_detail::weighPixel(0x00ffffffu, 0), 0xff000000u);
}

// アパーチャーグリル: 暗くする色だけ 1/4 暗くなる。どの長さでも 1 画素ずつの計算と同じ
void testMaskLine() {
    printf("maskLine against the scalar reference\n");
    CHECK_EQ(crt_detail::maskPixel(0xffffffffu, 0x0000ffffu), 0xffffc0c0u);
    CHECK_EQ(crt_detail::maskPixel(0xff808080u, 0x00ff00ffu), 0xff608060u);
    CHECK_EQ(crt_detail::maskPixel(0xff030201u, 0x00ffff00u), 0xff030201u);
    CHECK_EQ(crt_detail::maskPixel(0xff123456u, 0), 0xff123456u);
    int bad = 0;
    static const uint32_t pat[3] = {0x0000ffffu, 0x00ff00ffu, 0x00ffff00u};
    for (int n = 0; n <= 37; n++) {
        std::vector<uint32_t> buf(n + 6), mask(n + 1);
        for (int i = 0; i < n + 6; i++) buf[i] = (uint32_t)(i * 2654435761u + n);
        for (int i = 0; i < n; i++) mask[i] = pat[i % 3];
        const std::vector<uint32_t> before = buf;
        crt_detail::maskLine(buf.data() + 1, n, mask.data());
        for (int i = 0; i < n + 6; i++) {
            const bool in = i >= 1 && i < 1 + n;
            if (buf[i] != (in ? crt_detail::maskPixel(before[i], mask[i - 1]) : before[i])) bad++;
        }
    }
    CHECK_EQ(bad, 0);
}

// にじみ: どの長さでも、1 画素ずつの計算と同じ。行の端は端の画素を外にも並べたものとして混ぜる
void testBlurLine() {
    printf("blurLine against the scalar reference\n");
    int bad = 0;
    static const uint32_t pat[3] = {0x0000ffffu, 0x00ff00ffu, 0x00ffff00u};
    for (int n = 0; n <= 37; n++)
        for (int withMask = 0; withMask < 2; withMask++) {
            std::vector<uint32_t> in(n + 2), out(n + 4, 0xdeadbeefu), mask(n + 1);
            for (int i = 0; i < n; i++) in[i] = (uint32_t)(i * 2654435761u + n);
            for (int i = 0; i < n; i++) mask[i] = pat[i % 3];
            crt_detail::blurLine(out.data() + 1, in.data(), n, withMask ? mask.data() : nullptr);
            for (int i = 0; i < n; i++) {
                const uint32_t l = in[i > 0 ? i - 1 : 0], r = in[i < n - 1 ? i + 1 : n - 1];
                uint32_t want = crt_detail::blurPixel(l, in[i], r);
                if (withMask) want = crt_detail::maskPixel(want, mask[i]);
                if (out[i + 1] != want) bad++;
            }
            if (out[0] != 0xdeadbeefu || out[n + 1] != 0xdeadbeefu) bad++;
        }
    CHECK_EQ(bad, 0);
    // 同じ色が並んでいれば変わらない。白と黒の境目は 3/4 と 1/4
    CHECK_EQ(crt_detail::blurPixel(0xff808080u, 0xff808080u, 0xff808080u), 0xff808080u);
    CHECK_EQ(crt_detail::blurPixel(0xffffffffu, 0xffffffffu, 0xff000000u), 0xffbfbfbfu);
    CHECK_EQ(crt_detail::blurPixel(0xffffffffu, 0xff000000u, 0xff000000u), 0xff3f3f3fu);
}

// 1 行を作る: 黒い区間、にじみ、明るさがこの順に掛かる
void testRenderRow() {
    printf("crtRenderRow\n");
    const int srcW = 256, srcH = 256, dstW = 1536, dstH = 1024, pitch = 1024;
    const std::vector<uint32_t> src(pattern(pitch, srcH).size(), 0x00ffffffu); // 真っ白
    std::vector<uint32_t> a(dstW + 3), b(dstW + 3);
    for (int preset = CRT_SCANLINE; preset < CRT_PRESET_COUNT; preset++) {
        Map t(dstW, dstH);
        CHECK_EQ(crtBuildMap(t.m, preset, srcW, srcH, dstW, dstH), true);
        const CrtMap& m = t.m;
        int bad = 0;
        for (int y = 0; y < dstH; y += 3) {
            const uint32_t* line = crtRenderRow(m, src.data(), pitch, y, a.data(), b.data());
            for (int x = 0; x < dstW; x++) {
                const uint32_t p = line[x];
                if ((p >> 24) != 0xff) bad++;
                // 白い画面なので、黒い区間の中 (にじみの届かないところ) は黒、ほかは明るさそのもの
                if (!m.perPixel) {
                    const uint32_t want =
                        crt_detail::weighPixel(crt_detail::maskPixel(0xffffffffu, m.colMask[x]), m.rowWeight[y]);
                    if (p != want) bad++;
                }
            }
        }
        CHECK_EQ(bad, 0);
        if (m.perPixel) {
            // 左上の隅は黒、真ん中は明るい
            const uint32_t* top = crtRenderRow(m, src.data(), pitch, 0, a.data(), b.data());
            CHECK_EQ(top[0], 0xff000000u);
            CHECK_EQ(top[dstW - 1], 0xff000000u);
            const uint32_t* mid = crtRenderRow(m, src.data(), pitch, dstH / 2, a.data(), b.data());
            const uint32_t c = mid[dstW / 2 - (dstW / 2) % 3 + 1]; // 緑がそのままの桁
            CHECK_EQ(((c >> 8) & 0xff) >= 250, true);
        }
    }
}

// フレームバッファへの写し: 書く番地がそろっている、枠の外に書かない、変わっていない行は書かない、重ねる絵はそのまま出る
void testPresent(int preset, int mode, int fw, int fh) {
    printf("presentCrt video_scale=%s video_filter=%s %dx%d\n", videoScaleName(mode), videoFilterName(preset), fw, fh);
    const int sw = 1920, sh = 1080, srcPitch = 1024;
    const size_t pitchBytes = (size_t)sw * 4 + 16;
    const size_t fbWords = pitchBytes / 4 * sh;
    std::vector<uint32_t> src = pattern(srcPitch, 1024, (uint32_t)preset);
    // aspect は 4:3 の枠 (1440x1080)、full は 1920x1080、ほかは整数倍
    const FrameScale base = computeFrameScale(fw, fh, sw, sh, mode != VIDEO_SCALE_OFF, 4096);
    const FrameScale s = videoFrameScale(mode, base, fw, fh, sw, sh, 4096);
    CHECK_EQ(s.dstW, mode == VIDEO_SCALE_ASPECT ? 1440 : mode == VIDEO_SCALE_FULL ? 1920 : base.dstW);
    Map t(s.dstW, s.dstH);
    CHECK_EQ(crtBuildMap(t.m, preset, s.srcW, s.srcH, s.dstW, s.dstH), true);
    const CrtMap& m = t.m;
    std::vector<uint32_t> fb(fbWords, 0x12345678), want(fbWords, 0x12345678);
    std::vector<uint32_t> a((size_t)s.dstW + 3), b((size_t)s.dstW + 3), prev((size_t)srcPitch * 1024);
    auto at = [&](std::vector<uint32_t>& f, int x, int y) -> uint32_t& { return f[(size_t)y * (pitchBytes / 4) + x]; };

    // 写しなし: どの行も crtRenderRow() のとおり。枠の外は変わらない
    CheckingStore::reset();
    presentCrt<CheckingStore>(src.data(), srcPitch, (uint8_t*)fb.data(), pitchBytes, s, m, a.data(), b.data());
    CHECK_EQ(CheckingStore::misaligned, 0);
    CHECK_EQ(CheckingStore::bytes(), (long)s.dstW * s.dstH * 4);
    {
        int bad = 0;
        const uint32_t* origin = src.data() + (size_t)s.srcY * srcPitch + s.srcX;
        for (int y = 0; y < sh; y++) {
            const bool inY = y >= s.dstY && y < s.dstY + s.dstH;
            const uint32_t* line = inY ? crtRenderRow(m, origin, srcPitch, y - s.dstY, a.data(), b.data()) : nullptr;
            for (int x = 0; x < (int)(pitchBytes / 4); x++) {
                const bool in = inY && x >= s.dstX && x < s.dstX + s.dstW;
                if (at(fb, x, y) != (in ? line[x - s.dstX] : 0x12345678u)) bad++;
            }
        }
        CHECK_EQ(bad, 0);
    }

    // 写しあり: 1 回目はすべて書き、同じ画面の 2 回目は 1 バイトも書かない
    FrameRowCache cache;
    cache.pixels = prev.data();
    cache.pitch = srcPitch;
    cache.rows = 1024;
    CheckingStore::reset();
    presentCrt<CheckingStore>(src.data(), srcPitch, (uint8_t*)want.data(), pitchBytes, s, m, a.data(), b.data(), nullptr,
                              &cache);
    CHECK_EQ(want == fb, true);
    CHECK_EQ(CheckingStore::bytes(), (long)s.dstW * s.dstH * 4);
    CHECK_EQ(cache.copied, (uint32_t)s.srcH);
    CheckingStore::reset();
    presentCrt<CheckingStore>(src.data(), srcPitch, (uint8_t*)fb.data(), pitchBytes, s, m, a.data(), b.data(), nullptr,
                              &cache);
    CHECK_EQ(CheckingStore::bytes(), 0);
    CHECK_EQ(cache.skipped, (uint32_t)s.srcH);

    // 元の 1 行の 1 画素を変える: その行を使う表示の行だけを書き、結果はすべて書いたときと同じ
    const int changed = s.srcH / 4;
    src[(size_t)(s.srcY + changed) * srcPitch + s.srcX + s.srcW / 2] ^= 0x00ffffffu;
    long users = 0;
    for (int y = 0; y < s.dstH; y++)
        if (m.rowMin[y] <= changed && changed <= m.rowMax[y]) users++;
    CHECK_EQ(users >= (s.ky > 0 ? s.ky : s.dstH / s.srcH), true);
    CheckingStore::reset();
    presentCrt<CheckingStore>(src.data(), srcPitch, (uint8_t*)fb.data(), pitchBytes, s, m, a.data(), b.data(), nullptr,
                              &cache);
    CHECK_EQ(CheckingStore::bytes(), users * s.dstW * 4);
    CHECK_EQ(CheckingStore::misaligned, 0);
    presentCrt<CheckingStore>(src.data(), srcPitch, (uint8_t*)want.data(), pitchBytes, s, m, a.data(), b.data());
    CHECK_EQ(want == fb, true);

    // 重ねる絵 2 つ (右上と左上)。絵の画素はそのまま出て、絵の外は絵がないときと同じ。
    // 絵を置いた行は forceY0..forceY1 で書かせる (ShowFrame() の ForceRows() と同じ)
    std::vector<uint32_t> ovA(64 * 9), ovB(40 * 6);
    for (size_t i = 0; i < ovA.size(); i++) ovA[i] = 0xffe0e0e0u + (uint32_t)i;
    for (size_t i = 0; i < ovB.size(); i++) ovB[i] = 0xff00c000u + (uint32_t)i;
    FrameOverlay oa, ob;
    oa.pixels = ovA.data();
    oa.x = s.dstX + s.dstW - 64;
    oa.y = s.dstY + 5;
    oa.w = 64;
    oa.h = 9;
    ob.pixels = ovB.data();
    ob.x = s.dstX + 8;
    ob.y = s.dstY + 7;
    ob.w = 40;
    ob.h = 6;
    cache.forceY0 = s.dstY + 5;
    cache.forceY1 = s.dstY + 14;
    CheckingStore::reset();
    presentCrt<CheckingStore>(src.data(), srcPitch, (uint8_t*)fb.data(), pitchBytes, s, m, a.data(), b.data(), &oa, &cache,
                              &ob);
    CHECK_EQ(CheckingStore::bytes(), 9L * s.dstW * 4);
    CHECK_EQ(CheckingStore::misaligned, 0);
    CHECK_EQ(cache.forceY0 == 0 && cache.forceY1 == 0, true);
    {
        int bad = 0;
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) {
                uint32_t w = at(want, x, y);
                if (x >= oa.x && x < oa.x + oa.w && y >= oa.y && y < oa.y + oa.h)
                    w = ovA[(size_t)(y - oa.y) * oa.w + (x - oa.x)];
                if (x >= ob.x && x < ob.x + ob.w && y >= ob.y && y < ob.y + ob.h)
                    w = ovB[(size_t)(y - ob.y) * ob.w + (x - ob.x)];
                if (at(fb, x, y) != w) bad++;
            }
        CHECK_EQ(bad, 0);
    }
    // 写しを作り直す (valid = false) と、すべての行を書く
    cache.valid = false;
    CheckingStore::reset();
    presentCrt<CheckingStore>(src.data(), srcPitch, (uint8_t*)fb.data(), pitchBytes, s, m, a.data(), b.data(), nullptr,
                              &cache);
    CHECK_EQ(CheckingStore::bytes(), (long)s.dstW * s.dstH * 4);
    CHECK_EQ(want == fb, true);
    // 表の大きさが合わなければ何も書かない
    FrameScale other = s;
    other.dstW -= 4;
    CheckingStore::reset();
    presentCrt<CheckingStore>(src.data(), srcPitch, (uint8_t*)fb.data(), pitchBytes, other, m, a.data(), b.data());
    CHECK_EQ(CheckingStore::bytes(), 0);
}

// crt=off と crt=scanline (ベアメタル版) は presentScaled() をそのまま使う。その結果 (画素と、書き込みの大きさごとの回数) が
// この効果を足す前と変わっていないことを、足す前のコードで取ったハッシュと比べて確かめる
void testOffUnchanged() {
    printf("presentScaled() is unchanged (crt=off and crt=scanline)\n");
    struct Case {
        int fw, fh, sw, sh;
        bool scale, scanline;
        uint64_t hash;
        long n4, n16, n64;
    };
    static const Case cases[] = {
#include "crt_off_golden.inc"
    };
    for (const Case& c : cases) {
        const int srcPitch = 1024;
        const std::vector<uint32_t> src = pattern(srcPitch, 1024, 7);
        const FrameScale s = computeFrameScale(c.fw, c.fh, c.sw, c.sh, c.scale, 4096);
        std::vector<uint32_t> fb((size_t)c.sw * c.sh, 0x12345678), line((size_t)s.dstW + 3);
        std::vector<uint32_t> ovPixels(96 * 16);
        for (size_t i = 0; i < ovPixels.size(); i++) ovPixels[i] = 0xff202020u + (uint32_t)i;
        FrameOverlay ov;
        ov.pixels = ovPixels.data();
        ov.x = s.dstX + s.dstW - 96;
        ov.y = s.dstY + 1;
        ov.w = 96;
        ov.h = 16;
        CheckingStore::reset();
        presentScaled<CheckingStore>(src.data(), srcPitch, (uint8_t*)fb.data(), (size_t)c.sw * 4, s, line.data(), &ov,
                                     nullptr, c.scanline);
        const uint64_t h = fnv(fb.data(), fb.size());
        if (c.hash == 0) {
            // 表を作るとき (crt_off_golden.inc を作り直すとき) に写す行
            printf("{%d, %d, %d, %d, %s, %s, 0x%016llxull, %ld, %ld, %ld},\n", c.fw, c.fh, c.sw, c.sh,
                   c.scale ? "true" : "false", c.scanline ? "true" : "false", (unsigned long long)h, CheckingStore::n4,
                   CheckingStore::n16, CheckingStore::n64);
            failures++;
            continue;
        }
        CHECK_EQ(h, c.hash);
        CHECK_EQ(CheckingStore::n4, c.n4);
        CHECK_EQ(CheckingStore::n16, c.n16);
        CHECK_EQ(CheckingStore::n64, c.n64);
        CHECK_EQ(CheckingStore::misaligned, 0);
    }
}

// video_scale= の写す範囲 (videoFrameScale())。aspect は表示の高さいっぱいの 4:3、full は表示いっぱい。off と integer は
// computeFrameScale() のまま
void testVideoScale() {
    printf("videoFrameScale\n");
    auto same = [](const FrameScale& a, const FrameScale& b) { return a == b; };
    static const int modes[][2] = {{768, 512}, {512, 512}, {256, 256}, {384, 256}, {352, 240}, {1024, 1024}, {640, 480}};
    for (const auto& md : modes) {
        const int fw = md[0], fh = md[1];
        const FrameScale base = computeFrameScale(fw, fh, 1920, 1080, true, 4092);
        CHECK_EQ(same(videoFrameScale(VIDEO_SCALE_INTEGER, base, fw, fh, 1920, 1080, 4092), base), true);
        const FrameScale off = computeFrameScale(fw, fh, 1920, 1080, false, 4092);
        CHECK_EQ(same(videoFrameScale(VIDEO_SCALE_OFF, off, fw, fh, 1920, 1080, 4092), off), true);
        const FrameScale a = videoFrameScale(VIDEO_SCALE_ASPECT, base, fw, fh, 1920, 1080, 4092);
        CHECK_EQ(a.dstW == 1440 && a.dstH == 1080 && a.dstX == 240 && a.dstY == 0, true);
        CHECK_EQ(a.srcX == 0 && a.srcY == 0 && a.srcW == fw && a.srcH == fh, true);
        CHECK_EQ(a.kx == 0 && a.ky == 0, true);
        const FrameScale f = videoFrameScale(VIDEO_SCALE_FULL, base, fw, fh, 1920, 1080, 4092);
        CHECK_EQ(f.dstW == 1920 && f.dstH == 1080 && f.dstX == 0 && f.dstY == 0, true);
        CHECK_EQ(f.srcW == fw && f.srcH == fh && f.kx == 0, true);
    }
    {
        // 4K は高さを kCrtFillMaxH にする (full は表示の縦横比のまま)
        const FrameScale base = computeFrameScale(768, 512, 3840, 2160, true, 4092);
        const FrameScale s = videoFrameScale(VIDEO_SCALE_ASPECT, base, 768, 512, 3840, 2160, 4092);
        CHECK_EQ(s.dstW == 1600 && s.dstH == kCrtFillMaxH && s.dstX == 1120 && s.dstY == 480, true);
        const FrameScale f = videoFrameScale(VIDEO_SCALE_FULL, base, 768, 512, 3840, 2160, 4092);
        CHECK_EQ(f.dstW == 2133 && f.dstH == kCrtFillMaxH && f.dstX == 852 && f.dstY == 480, true);
    }
    {
        // 横が足りない (5:4) なら、aspect は幅いっぱいで 4:3、full は表示いっぱい
        const FrameScale base = computeFrameScale(768, 512, 1280, 1024, true, 4092);
        const FrameScale s = videoFrameScale(VIDEO_SCALE_ASPECT, base, 768, 512, 1280, 1024, 4092);
        CHECK_EQ(s.dstW == 1280 && s.dstH == 960 && s.dstX == 0 && s.dstY == 32, true);
        const FrameScale f = videoFrameScale(VIDEO_SCALE_FULL, base, 768, 512, 1280, 1024, 4092);
        CHECK_EQ(f.dstW == 1280 && f.dstH == 1024 && f.dstX == 0 && f.dstY == 0, true);
        // 1 行の上限 (maxLine) が表示より狭ければ、その幅に合わせる
        const FrameScale n = videoFrameScale(VIDEO_SCALE_FULL, base, 768, 512, 1280, 1024, 1000);
        CHECK_EQ(n.dstW == 1000 && n.dstH == 800, true);
    }
    {
        // 枠が元の画面より小さい (オーバースキャンの 984 ラインに 1024 ライン、GPU の拡大で 960x540) なら整数倍のまま
        const FrameScale b1 = computeFrameScale(1024, 1024, 1824, 984, true, 4092);
        CHECK_EQ(same(videoFrameScale(VIDEO_SCALE_ASPECT, b1, 1024, 1024, 1824, 984, 4092), b1), true);
        CHECK_EQ(same(videoFrameScale(VIDEO_SCALE_FULL, b1, 1024, 1024, 1824, 984, 4092), b1), true);
        const FrameScale b2 = computeFrameScale(768, 512, 960, 540, true, 4092, 2, 2);
        CHECK_EQ(same(videoFrameScale(VIDEO_SCALE_ASPECT, b2, 768, 512, 960, 540, 4092, 2, 2), b2), true);
        // 縦と横の GPU の倍率が違えば、整数倍のまま
        const FrameScale b3 = computeFrameScale(256, 256, 1920, 540, true, 4092, 1, 2);
        CHECK_EQ(same(videoFrameScale(VIDEO_SCALE_FULL, b3, 256, 256, 1920, 540, 4092, 1, 2), b3), true);
        // オーバースキャンでも 768x512 は入る (984 ラインいっぱい)
        const FrameScale b4 = computeFrameScale(768, 512, 1824, 984, true, 4092);
        const FrameScale s4 = videoFrameScale(VIDEO_SCALE_ASPECT, b4, 768, 512, 1824, 984, 4092);
        CHECK_EQ(s4.dstW == 1312 && s4.dstH == 984 && s4.dstX == 256, true);
    }
}

// 4:3 の枠のゆがみのある表: 元の画面を切らない (上下左右の辺の真ん中が枠の端の行 / 桁に届く)。四隅は黒で、
// 絵は枠の中に収まる。横の倍率は整数でない (1440 / 768 = 1.875)
void testFillMap(int fw, int fh) {
    printf("fill map %dx%d -> 1440x1080\n", fw, fh);
    const int dstW = 1440, dstH = 1080;
    Map t(dstW, dstH);
    for (int preset : {(int)CRT_CURVE, (int)CRT_GEOM, CRT_CURVE | CRT_MASK, (int)CRT_FULL}) {
        CHECK_EQ(crtBuildMap(t.m, preset, fw, fh, dstW, dstH), true);
        const CrtMap& m = t.m;
        checkMapSane(m);
        auto rowAt = [&](int x, int y) {
            int x0 = 0;
            for (uint32_t i = m.segStart[y]; i < m.segStart[y + 1]; i++) {
                if (x >= x0 && x < m.segs[i].x1) return (int)m.segs[i].row;
                x0 = m.segs[i].x1;
            }
            return -2;
        };
        // 上下の辺の真ん中は元の最初と最後の行、左右の辺の真ん中は元の最初と最後の桁 (切らない)
        CHECK_EQ(rowAt(dstW / 2, 0), 0);
        CHECK_EQ(rowAt(dstW / 2, dstH - 1), fh - 1);
        CHECK_EQ(m.sx[(size_t)(dstH / 2) * dstW], 0);
        CHECK_EQ(m.sx[(size_t)(dstH / 2) * dstW + dstW - 1], fw - 1);
        // 四隅は黒。元の画面の四隅の画素は、枠の内側 (k = 0.0225 なら上から約 11 行、左から約 15 桁) に出る
        CHECK_EQ(rowAt(0, 0), -1);
        CHECK_EQ(rowAt(dstW - 1, dstH - 1), -1);
        int cornerX = -1, cornerY = -1; // 元の画面の左上の画素が出る、いちばん外の表示の画素
        for (int y = 0; y < dstH / 2 && cornerY < 0; y++)
            for (int x = 0; x < dstW / 2; x++)
                if (rowAt(x, y) == 0 && m.sx[(size_t)y * dstW + x] == 0) {
                    cornerX = x;
                    cornerY = y;
                    break;
                }
        printf("    corner of the picture at (%d, %d)\n", cornerX, cornerY);
        // ゆがみの k から決まる位置 (四隅は縦横とも約 k / 2 の割合だけ内側。k が大きいと少し手前になる) の前後
        const int wantY = (int)((int64_t)dstH * kCrtCurve / 65536 / 2), wantX = (int)((int64_t)dstW * kCrtCurve / 65536 / 2);
        CHECK_EQ(cornerY > 0 && cornerY >= wantY - 3 - wantY / 8 && cornerY <= wantY + 3, true);
        CHECK_EQ(cornerX > 0 && cornerX >= wantX - 3 - wantX / 8 && cornerX <= wantX + 3, true);
        // 元の画面のどの行と桁も使う
        std::vector<int> rows(fh, 0), cols(fw, 0);
        for (int y = 0; y < dstH; y++) {
            int x0 = 0;
            for (uint32_t i = m.segStart[y]; i < m.segStart[y + 1]; i++) {
                if (m.segs[i].row >= 0) {
                    rows[m.segs[i].row] = 1;
                    for (int x = x0; x < m.segs[i].x1; x++) cols[m.sx[(size_t)y * dstW + x]] = 1;
                }
                x0 = m.segs[i].x1;
            }
        }
        int unused = 0;
        for (int v : rows) unused += !v;
        for (int v : cols) unused += !v;
        CHECK_EQ(unused, 0);
    }
}

// 4:3 の枠 (縦の倍率が整数でない。1080 / 512 = 2.11 など) のスキャンライン: どの桁でも、元の 1 行から作る表示の行
// (縦に続く) のうち最後の 1 行だけが暗く、ほかは暗くない (中間の明るさはない)。暗さは真ん中で半分
void testFillScanline(int fw, int fh) {
    printf("scanline in the 4:3 frame %dx%d -> 1440x1080\n", fw, fh);
    const int dstW = 1440, dstH = 1080;
    Map t(dstW, dstH);
    for (int preset : {CRT_GEOM, CRT_FULL}) {
        CHECK_EQ(crtBuildMap(t.m, preset, fw, fh, dstW, dstH), true);
        const CrtMap& m = t.m;
        auto rowAt = [&](int x, int y) {
            int x0 = 0;
            for (uint32_t i = m.segStart[y]; i < m.segStart[y + 1]; i++) {
                if (x >= x0 && x < m.segs[i].x1) return (int)m.segs[i].row;
                x0 = m.segs[i].x1;
            }
            return -2;
        };
        int bad = 0, runs = 0;
        for (int x : {dstW / 2, dstW / 4, 40, dstW - 41}) {
            int y = 0;
            while (y < dstH) {
                const int r = rowAt(x, y);
                int e = y;
                while (e < dstH && rowAt(x, e) == r) e++;
                // 元の画面の外 (黒) と、枠の端で切れた最初と最後の続きは見ない
                if (r >= 0 && y > 0 && e < dstH && r != 0 && r != fh - 1) {
                    runs++;
                    const uint8_t* w = m.weight + x;
                    const int last = w[(size_t)(e - 1) * dstW];
                    for (int k = y; k < e - 1; k++)
                        if (w[(size_t)k * dstW] < last + 40) bad++; // 最後の 1 行より明らかに明るい
                    if (e - y < dstH / fh || e - y > dstH / fh + 1) bad++; // 2.11 倍なら 2 行か 3 行
                    // ビネットのない真ん中の桁では、暗い行は半分 (127)、ほかはそのまま (255。真ん中の近く)
                    if (x == dstW / 2 && y > dstH / 4 && e < dstH * 3 / 4) {
                        if (last < 120 || last > 128) bad++;
                        for (int k = y; k < e - 1; k++)
                            if (w[(size_t)k * dstW] < 240) bad++;
                    }
                }
                y = e;
            }
        }
        CHECK_EQ(runs > 4 * fh / 2, true);
        CHECK_EQ(bad, 0);
    }
}

// 4:3 の枠のとき (1920x1080 で 1440x1080)、右上の数字は右の余白 (240 ドット) に縦に並び、左上のメッセージも出る
void testFillOsd() {
    printf("osd and message beside the 4:3 frame\n");
    const FrameScale base = computeFrameScale(768, 512, 1920, 1080, true, 4092);
    const FrameScale s = videoFrameScale(VIDEO_SCALE_ASPECT, base, 768, 512, 1920, 1080, 4092);
    for (int mode : {OSD_MODE_ON, OSD_MODE_FPS}) {
        const OsdRect r = chooseOsd(mode, 1920, 1080, s.dstX, s.dstY, s.dstW, s.dstH);
        CHECK_EQ(r.w > 0, true);
        CHECK_EQ(r.overlay, false);
        CHECK_EQ(r.x >= s.dstX + s.dstW && r.x + r.w <= 1920, true);
        CHECK_EQ(r.h > r.w, true); // 縦に並ぶ
    }
    // メッセージ: 左の余白 (240 ドット) に入らない長さは、X68000 の画面に重ねて出す (切らない)
    MessageGlyphs g;
    g.lines = 1;
    g.cols[0] = 20;
    const int ms = messageScale(1080);
    const OsdRect osd = chooseOsd(OSD_MODE_ON, 1920, 1080, s.dstX, s.dstY, s.dstW, s.dstH);
    const OsdRect r = placeMessage(1920, 1080, s.dstX, s.dstY, s.dstW, s.dstH, g, ms, messageLimitX(1920, 1080, osd, g, ms));
    CHECK_EQ(r.w, 20 * 8 * ms);
    CHECK_EQ(r.overlay, true);
}

// 同じフレームバッファに「整数倍 (表ができるまでのスキャンラインだけ) → 4:3 の枠 (表ができた) → 整数倍 (画面モードが
// 変わって表を作り直す)」と続けて写す (ShowFrame() の流れ: 枠が変わったら fillOutside() で外を塗り、写しを作り直して
// すべての行を写す)。どの段でも、前の枠の絵が残らず、枠の外は黒、フレームバッファの行の右の余り (番兵) は書かない。
// 右上の数字は、どちらの枠でも右の余白に縦に並び、X68000 の画面に重ならない
void testFrameSwitch() {
    printf("switching between the integer frame and the 4:3 frame\n");
    const int sw = 1920, sh = 1080, fw = 768, fh = 512, srcPitch = 1024;
    const size_t pitchWords = (size_t)sw + 32; // 右に 32 画素の番兵
    const uint32_t kSentinel = 0x5a5a5a5au, kBlack = 0xff000000u;
    std::vector<uint32_t> src = pattern(srcPitch, 1024, 7), prev((size_t)srcPitch * 1024);
    std::vector<uint32_t> fb(pitchWords * sh, kSentinel), ref(pitchWords * sh);
    std::vector<uint32_t> a(4100), b(4100);
    const FrameScale is = computeFrameScale(fw, fh, sw, sh, true, 4092);
    const FrameScale cs = videoFrameScale(VIDEO_SCALE_ASPECT, is, fw, fh, sw, sh, 4092);
    CHECK_EQ(is.dstW == 1536 && cs.dstW == 1440, true);
    Map t(sw, sh);
    CHECK_EQ(crtBuildMap(t.m, CRT_FULL, fw, fh, cs.dstW, cs.dstH), true);
    FrameRowCache cache;
    cache.pixels = prev.data();
    cache.pitch = srcPitch;
    cache.rows = 1024;
    const size_t pitchBytes = pitchWords * 4;
    // 1 フレームを写す。枠を替えたときは外を塗り、写しを作り直す
    auto show = [&](std::vector<uint32_t>& f, const FrameScale& s, bool crt, bool changed) {
        uint8_t* p = (uint8_t*)f.data();
        if (changed) {
            fillOutside<CheckingStore>(p, pitchBytes, sw, sh, s, kBlack);
            cache.valid = false;
        }
        if (crt)
            presentCrt<CheckingStore>(src.data(), srcPitch, p, pitchBytes, s, t.m, a.data(), b.data(), nullptr, &cache);
        else
            presentScaled<CheckingStore>(src.data(), srcPitch, p, pitchBytes, s, a.data(), nullptr, &cache, true);
    };
    // 何も写していないフレームバッファに、その枠で 1 回だけ写した結果と比べる (番兵も含めて)
    auto check = [&](const FrameScale& s, bool crt) {
        std::fill(ref.begin(), ref.end(), kSentinel);
        const FrameRowCache keep = cache;
        uint8_t* p = (uint8_t*)ref.data();
        fillOutside<CheckingStore>(p, pitchBytes, sw, sh, s, kBlack);
        if (crt)
            presentCrt<CheckingStore>(src.data(), srcPitch, p, pitchBytes, s, t.m, a.data(), b.data());
        else
            presentScaled<CheckingStore>(src.data(), srcPitch, p, pitchBytes, s, a.data(), nullptr, nullptr, true);
        cache = keep;
        CHECK_EQ(fb == ref, true);
        long outside = 0, sentinel = 0;
        for (int y = 0; y < sh; y++)
            for (size_t x = 0; x < pitchWords; x++) {
                const uint32_t v = fb[(size_t)y * pitchWords + x];
                if (x >= (size_t)sw) {
                    if (v != kSentinel) sentinel++;
                } else if (!((int)x >= s.dstX && (int)x < s.dstX + s.dstW && y >= s.dstY && y < s.dstY + s.dstH)) {
                    if (v != kBlack) outside++;
                }
            }
        CHECK_EQ(outside, 0);
        CHECK_EQ(sentinel, 0);
        // 右上の数字は右の余白に縦に並ぶ
        const OsdRect r = chooseOsd(OSD_MODE_ON, sw, sh, s.dstX, s.dstY, s.dstW, s.dstH);
        CHECK_EQ(r.w > 0 && !r.overlay && r.x >= s.dstX + s.dstW && r.x + r.w <= sw, true);
    };
    show(fb, is, false, true); // 表ができるまで: 整数倍、スキャンラインだけ
    check(is, false);
    show(fb, is, false, false); // 同じ枠の次のフレーム (変わった行だけ)
    check(is, false);
    show(fb, cs, true, true); // 表ができた: 4:3 の枠 (整数倍の枠の左右の 48 ドットずつと、上下の 28 ラインに絵が残らない)
    check(cs, true);
    src[(size_t)100 * srcPitch + 300] ^= 0x00ffffffu; // 1 画素変える (変わった行だけ写す)
    show(fb, cs, true, false);
    check(cs, true);
    show(fb, is, false, true); // 画面モードが変わって表を作り直す: 整数倍に戻る (4:3 の枠の上下の行に絵が残らない)
    check(is, false);
    // 右上の数字の場所は枠で変わる (ShowFrame() は OsdState で、前の場所を消してから描く)
    const OsdRect ri = chooseOsd(OSD_MODE_ON, sw, sh, is.dstX, is.dstY, is.dstW, is.dstH);
    const OsdRect rc = chooseOsd(OSD_MODE_ON, sw, sh, cs.dstX, cs.dstY, cs.dstW, cs.dstH);
    CHECK_EQ(ri.w > 0 && rc.w > 0, true);
    CHECK_EQ(rc.x >= ri.x, true);
}

// ホスト版のウィンドウの中の置き方 (videoWindowFrame())。ベアメタル版と同じ枠で、aspect と full はウィンドウいっぱい。
// 表は高さ 1200 ラインまでで作って拡大し、幅が 4096 を超えるときは整数で割って作る
void testWindowFrame() {
    printf("videoWindowFrame (host)\n");
    auto eq = [](const CrtWindowFrame& f, int x, int y, int w, int h, int cw, int ch) {
        const bool ok = f.s.dstX == x && f.s.dstY == y && f.s.dstW == w && f.s.dstH == h && f.cw == cw && f.ch == ch;
        if (!ok)
            printf("  got %d,%d %dx%d table %dx%d\n", f.s.dstX, f.s.dstY, f.s.dstW, f.s.dstH, f.cw, f.ch);
        return ok;
    };
    // integer: ベアメタル版と同じ整数倍 (768x512 の枠の 1 倍か 2 倍)
    CHECK_EQ(eq(videoWindowFrame(VIDEO_SCALE_INTEGER, 768, 512, 768, 512), 0, 0, 768, 512, 768, 512), true);
    CHECK_EQ(eq(videoWindowFrame(VIDEO_SCALE_INTEGER, 1152, 768, 768, 512), 192, 128, 768, 512, 768, 512), true);
    CHECK_EQ(eq(videoWindowFrame(VIDEO_SCALE_INTEGER, 1920, 1080, 768, 512), 192, 28, 1536, 1024, 1536, 1024), true);
    CHECK_EQ(eq(videoWindowFrame(VIDEO_SCALE_INTEGER, 1920, 1080, 256, 256), 192, 28, 1536, 1024, 1536, 1024), true);
    // off: 1 倍
    CHECK_EQ(eq(videoWindowFrame(VIDEO_SCALE_OFF, 1920, 1080, 768, 512), 576, 284, 768, 512, 768, 512), true);
    // aspect: 1920x1080 なら 1440x1080。4K は表を 1600x1200 で作り、2880x2160 に拡大する
    CHECK_EQ(eq(videoWindowFrame(VIDEO_SCALE_ASPECT, 1920, 1080, 256, 256), 240, 0, 1440, 1080, 1440, 1080), true);
    CHECK_EQ(eq(videoWindowFrame(VIDEO_SCALE_ASPECT, 3840, 2160, 768, 512), 480, 0, 2880, 2160, 1600, 1200), true);
    // 縦長のウィンドウ: 幅いっぱいの 4:3
    CHECK_EQ(eq(videoWindowFrame(VIDEO_SCALE_ASPECT, 1000, 1200, 768, 512), 0, 225, 1000, 750, 1000, 750), true);
    // full: ウィンドウいっぱい
    CHECK_EQ(eq(videoWindowFrame(VIDEO_SCALE_FULL, 1920, 1080, 768, 512), 0, 0, 1920, 1080, 1920, 1080), true);
    CHECK_EQ(eq(videoWindowFrame(VIDEO_SCALE_FULL, 3840, 2160, 768, 512), 0, 0, 3840, 2160, 2133, 1200), true);
    // 幅が 4096 を超える表: 3 で割って作る
    CHECK_EQ(eq(videoWindowFrame(VIDEO_SCALE_FULL, 9000, 1000, 768, 512), 0, 0, 9000, 1000, 3000, 333), true);
    // 枠が X68000 の画面より小さいウィンドウ: aspect と full は縮めて出す (切らない。ベアメタル版は整数倍に戻す)
    CHECK_EQ(eq(videoWindowFrame(VIDEO_SCALE_ASPECT, 700, 500, 768, 512), 16, 0, 666, 500, 666, 500), true);
    const CrtWindowFrame small = videoWindowFrame(VIDEO_SCALE_ASPECT, 700, 500, 768, 512);
    CHECK_EQ(small.s.kx == 0 && small.s.srcX == 0 && small.s.srcW == 768 && small.s.srcH == 512, true);
    CHECK_EQ(eq(videoWindowFrame(VIDEO_SCALE_FULL, 640, 400, 768, 512), 0, 0, 640, 400, 640, 400), true);
    // integer は 1 倍で真ん中を切り出す (ベアメタル版と同じ)
    const CrtWindowFrame cut = videoWindowFrame(VIDEO_SCALE_INTEGER, 700, 500, 768, 512);
    CHECK_EQ(cut.s.kx == 1 && cut.s.dstW == 700 && cut.s.dstH == 500 && cut.s.srcX == 34, true);
    // 既定のウィンドウ (768x512) の aspect: 高さいっぱいの 4:3 (682x512)
    CHECK_EQ(eq(videoWindowFrame(VIDEO_SCALE_ASPECT, 768, 512, 768, 512), 40, 0, 682, 512, 682, 512), true);
    // 小さすぎるウィンドウは表を作らない
    CHECK_EQ(videoWindowFrame(VIDEO_SCALE_FULL, 20, 10, 768, 512).cw, 0);
    CHECK_EQ(videoWindowFrame(VIDEO_SCALE_FULL, 0, 1080, 768, 512).cw, 0);
}

} // namespace

int main() {
    testPresets();
    testFlatMap(768, 512, 1536, 1024);
    testFlatMap(256, 256, 1536, 1024);
    testFlatMap(512, 512, 1536, 1024);
    testFlatMap(384, 256, 1536, 1024);
    testFlatMap(768, 512, 768, 512);
    testFlatMatchesScanline(768, 512);
    testFlatMatchesScanline(256, 256);
    testFlatMatchesScanline(512, 512);
    testFlatMatchesScanline(352, 240);
    testFractionalMap();
    testCurvedMap(768, 512, 1536, 1024);
    testCurvedMap(256, 256, 1536, 1024);
    testCurvedMap(512, 512, 1536, 1024);
    testCurvedMap(1024, 1024, 1024, 1024);
    testCurvedOdd();
    testBuildFails();
    testPrepareCounts();
    testSlicedBuild();
    testBuildWhileFramesArrive();
    testWeighLine();
    testMaskLine();
    testBlurLine();
    testRenderRow();
    for (int preset = CRT_OFF; preset < CRT_PRESET_COUNT; preset++) {
        testPresent(preset, VIDEO_SCALE_INTEGER, 768, 512);
        testPresent(preset, VIDEO_SCALE_ASPECT, 256, 256);
        testPresent(preset, VIDEO_SCALE_FULL, 768, 512);
    }
    testPresent(CRT_FULL, VIDEO_SCALE_ASPECT, 512, 512);
    testPresent(CRT_FULL, VIDEO_SCALE_FULL, 352, 240);
    testPresent(CRT_SCANLINE, VIDEO_SCALE_OFF, 768, 512);
    testOffUnchanged();
    testVideoScale();
    testFillMap(768, 512);
    testFillMap(256, 256);
    testFillMap(512, 512);
    testFillMap(1024, 1024);
    testFillScanline(768, 512);
    testFillScanline(256, 256);
    testFillScanline(352, 240);
    testFillOsd();
    testFrameSwitch();
    testWindowFrame();
    for (int preset : {CRT_OFF, CRT_CURVE, CRT_GEOM, CRT_FULL}) testPresent(preset, VIDEO_SCALE_ASPECT, 1024, 1024);
    testPresent(CRT_MASK, VIDEO_SCALE_FULL, 1024, 1024);
    return finish();
}
