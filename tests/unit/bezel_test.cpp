// src/platform/bezel.h のテスト: 枠の絵のファイルの頭の検査、ガラスの左右の端、写す範囲 (video_scale= によらずガラスの矩形、
// 使えないときは枠なし)、枠の外とメニューの描き直し、右上の数字の背景、presentCrt() の左右の端 (CrtBezel)
#include <string.h>

#include <vector>

#include "platform/bezel.h"
#include "platform/crt_effect.h"
#include "test_check.h"

using namespace platform;

namespace {

struct PlainStore {
    typedef frame_scale_detail::V4 V4;
    static void store4(uint32_t* p, uint32_t v) { memcpy(p, &v, 4); }
    static void store16(uint32_t* p, V4 v) { memcpy(p, &v, 16); }
    static void store64(uint32_t* p, V4 a, V4 b, V4 c, V4 d) {
        memcpy(p, &a, 16);
        memcpy(p + 4, &b, 16);
        memcpy(p + 8, &c, 16);
        memcpy(p + 12, &d, 16);
    }
};

void put16(std::vector<uint8_t>& v, int o, int x) {
    v[o] = (uint8_t)x;
    v[o + 1] = (uint8_t)(x >> 8);
}

std::vector<uint8_t> header(int w, int h, int gx, int gy, int gw, int gh) {
    std::vector<uint8_t> v(kBezelHeaderBytes);
    memcpy(v.data(), "PXBZ", 4);
    put16(v, 4, w);
    put16(v, 6, h);
    put16(v, 8, gx);
    put16(v, 10, gy);
    put16(v, 12, gw);
    put16(v, 14, gh);
    return v;
}

// 小さな枠の絵: 64x48、ガラスは (8,4) 48x40。ガラスの矩形の中の四隅を 3 画素ずつ枠 (A が 255)、左右の端の 1 画素を縁 (A が 128)
struct Small {
    BezelHeader h;
    std::vector<uint32_t> px;
    std::vector<uint16_t> edges;
    BezelImage b;
    Small() {
        h.w = 64;
        h.h = 48;
        h.gx = 8;
        h.gy = 4;
        h.gw = 48;
        h.gh = 40;
        px.assign((size_t)h.w * h.h, 0);
        for (int y = 0; y < h.h; y++)
            for (int x = 0; x < h.w; x++) {
                const bool in = x >= h.gx && x < h.gx + h.gw && y >= h.gy && y < h.gy + h.gh;
                px[(size_t)y * h.w + x] = in ? 0 : (0xffu << 24 | (uint32_t)(x * 4) << 8 | (uint32_t)y);
            }
        for (int y = 0; y < h.gh; y++) {
            uint32_t* row = &px[(size_t)(h.gy + y) * h.w + h.gx];
            const int corner = y < 3 || y >= h.gh - 3 ? 3 : 1;
            for (int x = 0; x < corner; x++) {
                row[x] = (corner == 3 ? 0xffu : 0x80u) << 24 | 0x404040u;
                row[h.gw - 1 - x] = (corner == 3 ? 0xffu : 0x80u) << 24 | 0x404040u;
            }
        }
        edges.assign((size_t)h.gh * 2, 0);
        CHECK_EQ(bezelEdges(px.data(), h, edges.data(), edges.data() + h.gh), true);
        b.pixels = px.data();
        b.left = edges.data();
        b.right = edges.data() + h.gh;
        b.h = h;
        b.filter = CRT_BEZEL_DE;
    }
};

void testHeader() {
    printf("parseBezelHeader\n");
    BezelHeader h;
    const uint64_t size = kBezelHeaderBytes + 1920ull * 1080 * 4;
    std::vector<uint8_t> v = header(1920, 1080, 380, 68, 1160, 870);
    CHECK_EQ(parseBezelHeader(v.data(), size, h) == nullptr, true);
    CHECK_EQ(h.w, 1920);
    CHECK_EQ(h.h, 1080);
    CHECK_EQ(h.gx, 380);
    CHECK_EQ(h.gy, 68);
    CHECK_EQ(h.gw, 1160);
    CHECK_EQ(h.gh, 870);
    // 大きさが合わない、短い、印が違う、ガラスがはみ出す、ガラスの x が 4 の倍数でない
    CHECK_EQ(strcmp(parseBezelHeader(v.data(), size - 4, h), "wrong size"), 0);
    CHECK_EQ(strcmp(parseBezelHeader(v.data(), 8, h), "not a bezel file"), 0);
    std::vector<uint8_t> bad = v;
    bad[0] = 'Q';
    CHECK_EQ(strcmp(parseBezelHeader(bad.data(), size, h), "not a bezel file"), 0);
    bad = header(1920, 1080, 800, 68, 1160, 870);
    CHECK_EQ(strcmp(parseBezelHeader(bad.data(), size, h), "bad header"), 0);
    bad = header(1920, 1080, 381, 68, 1160, 870);
    CHECK_EQ(strcmp(parseBezelHeader(bad.data(), size, h), "glass x is not a multiple of 4"), 0);
    // ファイルの名前は設定の名前から決まる
    CHECK_EQ(strcmp(bezelFileName(CRT_SCANLINE | CRT_BEZEL_DE), "bezel-cz-600de.bin"), 0);
    CHECK_EQ(strcmp(bezelFileName(CRT_BEZEL_DB), "bezel-cz-600db.bin"), 0);
    CHECK_EQ(bezelFileName(CRT_FULL) == nullptr, true);
}

void testEdges() {
    printf("bezelEdges\n");
    Small s;
    CHECK_EQ(s.edges[0], 3);
    CHECK_EQ(s.edges[s.h.gh + 0], s.h.gw - 3);
    CHECK_EQ(s.edges[10], 1);
    CHECK_EQ(s.edges[s.h.gh + 10], s.h.gw - 1);
    // アルファは 0xff にした
    CHECK_EQ(s.px[0] >> 24, 0xffu);
    CHECK_EQ(s.px[(size_t)(s.h.gy + 10) * s.h.w + s.h.gx + 5], 0xff000000u);
    // ガラスの真ん中に枠の画素があれば描けない
    BezelHeader h = s.h;
    std::vector<uint32_t> px((size_t)h.w * h.h, 0);
    px[(size_t)(h.gy + 5) * h.w + h.gx + 20] = 0xff123456u;
    std::vector<uint16_t> e((size_t)h.gh * 2);
    CHECK_EQ(bezelEdges(px.data(), h, e.data(), e.data() + h.gh), false);
    // 行のすべてが枠なら left = right = gw
    std::fill(px.begin(), px.end(), 0xff000000u);
    CHECK_EQ(bezelEdges(px.data(), h, e.data(), e.data() + h.gh), true);
    CHECK_EQ(e[0], h.gw);
    CHECK_EQ(e[h.gh], h.gw);
}

void testFrameScale() {
    printf("bezelFrameScale\n");
    Small s;
    FrameScale fs;
    CHECK_EQ(bezelFrameScale(s.b, 32, 32, 64, 48, 1, 1, fs), true);
    CHECK_EQ(fs.kx, 0);
    CHECK_EQ(fs.dstX, 8);
    CHECK_EQ(fs.dstY, 4);
    CHECK_EQ(fs.dstW, 48);
    CHECK_EQ(fs.dstH, 40);
    CHECK_EQ(fs.srcW, 32);
    CHECK_EQ(isBezelScale(s.b, fs), true);
    // フレームバッファの大きさが違う、画面がガラスより大きい、GPU で拡大している、絵がない
    CHECK_EQ(bezelFrameScale(s.b, 32, 32, 128, 96, 1, 1, fs), false);
    CHECK_EQ(bezelFrameScale(s.b, 32, 41, 64, 48, 1, 1, fs), false);
    CHECK_EQ(bezelFrameScale(s.b, 32, 32, 64, 48, 2, 2, fs), false);
    BezelImage none;
    CHECK_EQ(bezelFrameScale(none, 32, 32, 64, 48, 1, 1, fs), false);
    FrameScale integer = fs;
    integer.kx = 1;
    CHECK_EQ(isBezelScale(s.b, integer), false);
}

void testOutside() {
    printf("drawBezelOutside / restoreBezel / drawOsdOnBezel\n");
    Small s;
    const int pitch = s.h.w + 4; // 画素で数える
    std::vector<uint32_t> fb((size_t)pitch * s.h.h, 0x11111111u);
    drawBezelOutside<PlainStore>((uint8_t*)fb.data(), (size_t)pitch * 4, s.b);
    int bad = 0;
    for (int y = 0; y < s.h.h; y++)
        for (int x = 0; x < pitch; x++) {
            const uint32_t got = fb[(size_t)y * pitch + x];
            bool frame = x < s.h.w;
            if (y >= s.h.gy && y < s.h.gy + s.h.gh && x >= s.h.gx + s.b.left[y - s.h.gy] &&
                x < s.h.gx + s.b.right[y - s.h.gy])
                frame = false; // ガラスの中 (X68000 の画面) は残す
            if (got != (frame ? s.px[(size_t)y * s.h.w + x] : 0x11111111u)) bad++;
        }
    CHECK_EQ(bad, 0);
    // 右上の数字: 背景 (bg) の画素は枠の絵の半分の明るさ、字の画素はそのまま。消すと枠の絵に戻る
    const int ox = 50, oy = 0, ow = 12, oh = 4;
    std::vector<uint32_t> img((size_t)ow * oh, 0xff000000u), line(ow);
    img[1] = 0xffffffffu;
    drawOsdOnBezel<PlainStore>((uint8_t*)fb.data(), (size_t)pitch * 4, s.b, ox, oy, ow, oh, img.data(), 0xff000000u,
                               line.data());
    CHECK_EQ(fb[ox + 1], 0xffffffffu);
    const uint32_t under = s.px[ox];
    CHECK_EQ(fb[ox], 0xff000000u | ((under >> 1) & 0x7f7f7fu));
    restoreBezel<PlainStore>((uint8_t*)fb.data(), (size_t)pitch * 4, s.b, ox, oy, ow, oh);
    CHECK_EQ(fb[ox + 1], s.px[ox + 1]);
    CHECK_EQ(fb[ox], under);
    // ガラスの矩形の中で枠が重なる左右の端 (角) には描かず、枠の絵のまま
    {
        const int gy0 = s.h.gy, cx = s.h.gx; // ガラスの矩形の左上の角 (左の端は枠が重なる)
        CHECK_EQ(s.b.left[0] > 0, true);
        std::vector<uint32_t> img2(16 * 2, 0xffffffffu), line2(16);
        drawOsdOnBezel<PlainStore>((uint8_t*)fb.data(), (size_t)pitch * 4, s.b, cx - 4, gy0, 16, 2, img2.data(),
                                   0xff000000u, line2.data());
        int wrong = 0;
        for (int k = 0; k < 16; k++) {
            const int fx = cx - 4 + k;
            const bool edge = fx >= cx && fx < cx + s.b.left[0];
            const uint32_t want = edge ? s.px[(size_t)gy0 * s.h.w + fx] : 0xffffffffu;
            if (fb[(size_t)gy0 * pitch + fx] != want) wrong++;
        }
        CHECK_EQ(wrong, 0);
        restoreBezel<PlainStore>((uint8_t*)fb.data(), (size_t)pitch * 4, s.b, cx - 4, gy0, 16, 2);
    }
    // はみ出す矩形は切る
    restoreBezel<PlainStore>((uint8_t*)fb.data(), (size_t)pitch * 4, s.b, s.h.w - 2, -3, 10, 6);
    CHECK_EQ(fb[pitch - 1], 0x11111111u); // 絵の幅の外 (pitch の余り) は書かない
}

// presentCrt() に CrtBezel を渡すと、各行の左右の端は枠の絵で、その間は渡さないときと同じ
void testPresent() {
    printf("presentCrt with the bezel\n");
    Small s;
    const int pitch = s.h.w;
    const int fw = 32, fh = 32;
    FrameScale fs;
    CHECK_EQ(bezelFrameScale(s.b, fw, fh, s.h.w, s.h.h, 1, 1, fs), true);
    std::vector<uint32_t> src((size_t)64 * 64);
    for (size_t i = 0; i < src.size(); i++) src[i] = (uint32_t)(i * 2654435761u) >> 8;
    for (int filter : {0, (int)CRT_SCANLINE, (int)CRT_FULL}) {
        std::vector<uint32_t> mem(crtMapBytes(fs.dstW, fs.dstH) / 4 + 1);
        CrtMap m;
        crtMapInit(m, mem.data(), fs.dstW, fs.dstH);
        CHECK_EQ(crtBuildMap(m, filter, fs.srcW, fs.srcH, fs.dstW, fs.dstH), true);
        std::vector<uint32_t> a((size_t)fs.dstW + 4), b((size_t)fs.dstW + 4);
        std::vector<uint32_t> plain((size_t)pitch * s.h.h, 0), with((size_t)pitch * s.h.h, 0);
        presentCrt<PlainStore>(src.data(), 64, (uint8_t*)plain.data(), (size_t)pitch * 4, fs, m, a.data(), b.data());
        CrtBezel cb;
        cb.pixels = s.px.data() + (size_t)fs.dstY * s.h.w + fs.dstX;
        cb.pitch = s.h.w;
        cb.left = s.b.left;
        cb.right = s.b.right;
        presentCrt<PlainStore>(src.data(), 64, (uint8_t*)with.data(), (size_t)pitch * 4, fs, m, a.data(), b.data(), nullptr,
                               nullptr, nullptr, &cb);
        int bad = 0;
        for (int y = 0; y < fs.dstH; y++)
            for (int x = 0; x < fs.dstW; x++) {
                const size_t i = (size_t)(fs.dstY + y) * pitch + fs.dstX + x;
                const bool edge = x < s.b.left[y] || x >= s.b.right[y];
                if (with[i] != (edge ? s.px[(size_t)(fs.dstY + y) * s.h.w + fs.dstX + x] : plain[i])) bad++;
            }
        CHECK_EQ(bad, 0);
    }
}

// 変わった行だけ写す (FrameRowCache) ときも、写した行の左右の端は枠の絵に戻し、変わっていない行には書かない
void testPresentCache() {
    printf("presentCrt with the bezel and the row cache\n");
    Small s;
    const int pitch = s.h.w;
    const int fw = 32, fh = 32, srcPitch = 64;
    FrameScale fs;
    CHECK_EQ(bezelFrameScale(s.b, fw, fh, s.h.w, s.h.h, 1, 1, fs), true);
    CrtBezel cb;
    cb.pixels = s.px.data() + (size_t)fs.dstY * s.h.w + fs.dstX;
    cb.pitch = s.h.w;
    cb.left = s.b.left;
    cb.right = s.b.right;
    for (int filter : {0, (int)CRT_FULL}) {
        std::vector<uint32_t> src((size_t)srcPitch * fh);
        for (size_t i = 0; i < src.size(); i++) src[i] = (uint32_t)(i * 2654435761u) >> 8;
        std::vector<uint32_t> mem(crtMapBytes(fs.dstW, fs.dstH) / 4 + 1);
        CrtMap m;
        crtMapInit(m, mem.data(), fs.dstW, fs.dstH);
        CHECK_EQ(crtBuildMap(m, filter, fs.srcW, fs.srcH, fs.dstW, fs.dstH), true);
        std::vector<uint32_t> a((size_t)fs.dstW + 4), b((size_t)fs.dstW + 4);
        std::vector<uint32_t> prev((size_t)srcPitch * fh);
        FrameRowCache cache;
        cache.pixels = prev.data();
        cache.pitch = srcPitch;
        cache.rows = fh;
        std::vector<uint32_t> fb((size_t)pitch * s.h.h, 0);
        presentCrt<PlainStore>(src.data(), srcPitch, (uint8_t*)fb.data(), (size_t)pitch * 4, fs, m, a.data(), b.data(),
                               nullptr, &cache, nullptr, &cb);
        // 写したあとで画面を番兵で埋め、元の 1 行だけを変えて写す
        const uint32_t kSentinel = 0x5a5a5a5au;
        std::fill(fb.begin(), fb.end(), kSentinel);
        src[(size_t)10 * srcPitch + 5] ^= 0x00ffffffu;
        presentCrt<PlainStore>(src.data(), srcPitch, (uint8_t*)fb.data(), (size_t)pitch * 4, fs, m, a.data(), b.data(),
                               nullptr, &cache, nullptr, &cb);
        // 比べる相手: 変えたあとの画面を、写しを使わずにすべて写したもの
        std::vector<uint32_t> fresh((size_t)pitch * s.h.h, 0);
        presentCrt<PlainStore>(src.data(), srcPitch, (uint8_t*)fresh.data(), (size_t)pitch * 4, fs, m, a.data(), b.data(),
                               nullptr, nullptr, nullptr, &cb);
        int written = 0, kept = 0, bad = 0;
        for (int y = 0; y < fs.dstH; y++) {
            const size_t row = (size_t)(fs.dstY + y) * pitch + fs.dstX;
            bool allSentinel = true, same = true;
            for (int x = 0; x < fs.dstW; x++) {
                allSentinel &= fb[row + x] == kSentinel;
                same &= fb[row + x] == fresh[row + x];
            }
            if (allSentinel) kept++;
            else if (same) written++;
            else bad++;
            // 写した行の左右の端は枠の絵
            if (!allSentinel)
                for (int x = 0; x < fs.dstW; x++)
                    if ((x < s.b.left[y] || x >= s.b.right[y]) && fb[row + x] != cb.pixels[(size_t)y * cb.pitch + x]) bad++;
        }
        CHECK_EQ(bad, 0);
        CHECK_EQ(written > 0 && kept > 0, true);
        // ガラスの矩形の外には書かない
        int outside = 0;
        for (int y = 0; y < s.h.h; y++)
            for (int x = 0; x < pitch; x++)
                if ((y < fs.dstY || y >= fs.dstY + fs.dstH || x < fs.dstX || x >= fs.dstX + fs.dstW) &&
                    fb[(size_t)y * pitch + x] != kSentinel)
                    outside++;
        CHECK_EQ(outside, 0);
    }
}

// 枠を出さない理由のログ: 写す範囲を変えずに (ガラスより大きい画面のまま) off → cz-600de → cz-600db → off → cz-600de と
// 替えると、枠を選ぶたびに出し、外したときは出さない。同じ理由が続くとき (画面モードを行き来する) は出さない
void testSkipLog() {
    printf("bezelSkipCode / BezelSkipLog\n");
    CHECK_EQ(bezelSkipCode(CRT_SCANLINE, true, false, false), 0);          // 枠なしの設定
    CHECK_EQ(bezelSkipCode(CRT_BEZEL_DE, false, false, false), 0);         // 絵がない (読むときにログに出している)
    CHECK_EQ(bezelSkipCode(CRT_BEZEL_DE | CRT_MASK, true, true, false), 0); // 枠を出した
    CHECK_EQ(bezelSkipCode(CRT_BEZEL_DE, true, false, false), 1 << 4 | BEZEL_SKIP_LARGE);
    CHECK_EQ(bezelSkipCode(CRT_BEZEL_DB, true, false, true), 2 << 4 | BEZEL_SKIP_GPU);
    BezelSkipLog log;
    struct Step {
        int filter;
        bool logs;
    } steps[] = {{0, false},
                 {CRT_BEZEL_DE, true},
                 {CRT_BEZEL_DE, false}, // 次のフレームも同じ理由
                 {CRT_BEZEL_DB, true},
                 {0, false},
                 {CRT_BEZEL_DE, true}};
    for (const Step& st : steps) {
        const int code = bezelSkipCode(st.filter, st.filter != 0, false, false);
        CHECK_EQ(log.update(code), st.logs);
        CHECK_EQ(log.logged, code);
    }
}

} // namespace

int main() {
    testHeader();
    testEdges();
    testFrameScale();
    testOutside();
    testPresent();
    testPresentCache();
    testSkipLog();
    return finish();
}
