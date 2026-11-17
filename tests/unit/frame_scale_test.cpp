// src/platform/frame_scale.h のテスト: 画面モードとフレームバッファの大きさごとの倍率と位置、拡大して写した画素、
// 枠の外を塗る範囲
#include <string.h>

#include <vector>

#include "platform/frame_scale.h"
#include "test_check.h"

using platform::computeFrameScale;
using platform::FrameScale;

namespace {

// フレームバッファへの書き込みを調べる Store (platform::DeviceStore と同じ形)。16 バイト / 64 バイトの書き込みが
// 16 バイトにそろっていない (Device のメモリでは例外になる) 回数と、大きさごとの回数を数え、ふつうに書く
struct CheckingStore {
    typedef platform::frame_scale_detail::V4 V4;
    static long n4, n16, n64, misaligned;
    static void reset() { n4 = n16 = n64 = misaligned = 0; }
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

void checkScale(int fw, int fh, int sw, int sh, bool on, int kx, int ky, int dstW, int dstH) {
    const FrameScale s = computeFrameScale(fw, fh, sw, sh, on, 4096);
    printf("  %dx%d on %dx%d %s: x%d y%d -> %dx%d at %d,%d\n", fw, fh, sw, sh, on ? "auto" : "off", s.kx, s.ky,
           s.dstW, s.dstH, s.dstX, s.dstY);
    CHECK_EQ(s.kx, kx);
    CHECK_EQ(s.ky, ky);
    CHECK_EQ(s.dstW, dstW);
    CHECK_EQ(s.dstH, dstH);
    CHECK_EQ(s.dstX % 4, 0);
    CHECK_EQ(s.dstX >= 0 && s.dstX + s.dstW <= sw, true);
    CHECK_EQ(s.dstY >= 0 && s.dstY + s.dstH <= sh, true);
    CHECK_EQ(s.srcX >= 0 && s.srcX + s.srcW <= fw, true);
    CHECK_EQ(s.srcY >= 0 && s.srcY + s.srcH <= fh, true);
}

void checkScaleGpu(int fw, int fh, int sw, int sh, int gx, int gy, int kx, int ky, int dstW, int dstH) {
    const FrameScale s = computeFrameScale(fw, fh, sw, sh, true, 4096, gx, gy);
    printf("  %dx%d on %dx%d gpu x%d y%d: x%d y%d -> %dx%d at %d,%d\n", fw, fh, sw, sh, gx, gy, s.kx, s.ky, s.dstW,
           s.dstH, s.dstX, s.dstY);
    CHECK_EQ(s.kx, kx);
    CHECK_EQ(s.ky, ky);
    CHECK_EQ(s.dstW, dstW);
    CHECK_EQ(s.dstH, dstH);
    CHECK_EQ(s.dstX % 4, 0);
    CHECK_EQ(s.dstX >= 0 && s.dstX + s.dstW <= sw, true);
    CHECK_EQ(s.dstY >= 0 && s.dstY + s.dstH <= sh, true);
}

void testFactors() {
    printf("factors\n");
    // 1920x1080: 枠は 1536x1024
    checkScale(768, 512, 1920, 1080, true, 2, 2, 1536, 1024);
    checkScale(512, 512, 1920, 1080, true, 3, 2, 1536, 1024);
    checkScale(256, 256, 1920, 1080, true, 6, 4, 1536, 1024);
    checkScale(256, 240, 1920, 1080, true, 6, 4, 1536, 960);
    checkScale(384, 256, 1920, 1080, true, 4, 4, 1536, 1024);
    checkScale(352, 240, 1920, 1080, true, 4, 4, 1408, 960);    // 割り切れないので切り捨てる
    checkScale(1024, 1024, 1920, 1080, true, 1, 1, 1024, 1024); // 枠より大きいので拡大しない
    checkScale(768, 512, 1920, 1080, false, 1, 1, 768, 512);
    // 1280x720 / 1366x768: 枠は 768x512
    checkScale(768, 512, 1280, 720, true, 1, 1, 768, 512);
    checkScale(512, 512, 1280, 720, true, 1, 1, 512, 512);
    checkScale(256, 256, 1280, 720, true, 3, 2, 768, 512);
    checkScale(256, 256, 1366, 768, true, 3, 2, 768, 512);
    // オーバースキャンで少し小さくなったフレームバッファ: 2 倍にして上下を切り出さず、1 倍にする
    // (切ると Human68k のファンクションキーの行などが見えなくなる。frame_scale.h の注釈を参照)
    checkScale(768, 512, 1824, 984, true, 1, 1, 768, 512);
    checkScale(768, 512, 1856, 1016, true, 1, 1, 768, 512);
    checkScale(256, 256, 1824, 984, true, 3, 2, 768, 512);
    // 3840x2160: 枠の倍率は 2 まで
    checkScale(768, 512, 3840, 2160, true, 2, 2, 1536, 1024);
    // 2560x1440 / 1600x1200
    checkScale(768, 512, 2560, 1440, true, 2, 2, 1536, 1024);
    checkScale(256, 256, 1600, 1200, true, 6, 4, 1536, 1024);
    // フレームバッファが 768x512 (QEMU の試験) か、それより小さい: 大きければ真ん中を切り出す
    checkScale(768, 512, 768, 512, true, 1, 1, 768, 512);
    checkScale(256, 256, 768, 512, true, 3, 2, 768, 512);
    checkScale(768, 512, 640, 480, true, 1, 1, 640, 480);
    const FrameScale c = computeFrameScale(768, 512, 640, 480, true, 4096);
    CHECK_EQ(c.srcX, 64);
    CHECK_EQ(c.srcY, 16);
    // 行のバッファより広くならない
    const FrameScale l = computeFrameScale(768, 512, 1920, 1080, true, 1000);
    CHECK_EQ(l.dstW, 1000 / 2 * 2);
    // 3 倍以上で、行のバッファいっぱい (testPresent() で行の終わりの先に書く分も見る)
    const FrameScale f6 = computeFrameScale(256, 256, 1920, 1080, true, 1536);
    CHECK_EQ(f6.kx, 6);
    CHECK_EQ(f6.dstW, 1536);
    const FrameScale f5 = computeFrameScale(256, 256, 1920, 1080, true, 1530);
    CHECK_EQ(f5.srcW, 255);
    CHECK_EQ(f5.dstW, 1530);
    // 5 倍は 4 画素ずつ書くと 8 画素目まで書くので、行の終わりの先に 3 画素書く
    const FrameScale k5 = computeFrameScale(300, 300, 1920, 1080, true, 1500);
    CHECK_EQ(k5.kx, 5);
    CHECK_EQ(k5.dstW, 1500);
    // GPU が 2 倍に拡大する (pix68k_gpu_scale=2): 1920x1080 の表示に 960x540 のフレームバッファ
    checkScaleGpu(768, 512, 960, 540, 2, 2, 1, 1, 768, 512);
    checkScaleGpu(256, 256, 960, 540, 2, 2, 3, 2, 768, 512);
    checkScaleGpu(384, 256, 960, 540, 2, 2, 2, 2, 768, 512);
    checkScaleGpu(512, 512, 960, 540, 2, 2, 1, 1, 512, 512); // 表示で 3 倍のところを 1 倍 (1024x1024 に出る)
    checkScaleGpu(1024, 1024, 960, 540, 2, 2, 1, 1, 960, 540); // フレームバッファより大きいので真ん中を切り出す
    // 縦だけ GPU が 2 倍にする (pix68k_gpu_scale=1x2): 1920x540 のフレームバッファ
    checkScaleGpu(768, 512, 1920, 540, 1, 2, 2, 1, 1536, 512);
    checkScaleGpu(512, 512, 1920, 540, 1, 2, 3, 1, 1536, 512);
    checkScaleGpu(256, 256, 1920, 540, 1, 2, 6, 2, 1536, 512);
    checkScaleGpu(352, 240, 1920, 540, 1, 2, 4, 2, 1408, 480);
    // 3840x2160 の表示に 1920x1080 (枠の倍率は 2 まで。GPU の 2 倍で 1536x1024)
    checkScaleGpu(768, 512, 1920, 1080, 2, 2, 1, 1, 768, 512);
    // GPU の倍率が 1 なら今までと同じ
    checkScaleGpu(768, 512, 1920, 1080, 1, 1, 2, 2, 1536, 1024);
    // 大きさが 0 のときは何もしない
    const FrameScale z = computeFrameScale(0, 512, 1920, 1080, true, 4096);
    CHECK_EQ(z.dstW, 0);
    CHECK_EQ(z.dstH, 0);
}

// 拡大して写した画素と、外を塗った画素。フレームバッファの 1 行のバイト数が 16 の倍数でないときも見る
// maxLine は computeFrameScale() に渡す行のバッファの大きさ (dstW がこれいっぱいになるときも見る)
// scanline なら、縦の倍率が 2 以上のとき、拡大した各行の最後の 1 行が半分の明るさになる
void testPresent(int fw, int fh, int sw, int sh, size_t pitchBytes, bool on, int maxLine = 4096,
                 bool scanline = false) {
    printf("present %dx%d on %dx%d pitch %zu max line %d%s\n", fw, fh, sw, sh, pitchBytes, maxLine,
           scanline ? " scanline" : "");
    const int srcPitch = 1024;
    std::vector<uint32_t> src((size_t)srcPitch * fh);
    for (int y = 0; y < fh; y++)
        for (int x = 0; x < fw; x++) src[(size_t)y * srcPitch + x] = (uint32_t)(y * 4096 + x) & 0xffffff;
    std::vector<uint32_t> storage(pitchBytes * sh / 4 + 8);
    uint8_t* fb = (uint8_t*)storage.data();
    for (size_t i = 0; i < storage.size(); i++) storage[i] = 0x12345678;
    const FrameScale s = computeFrameScale(fw, fh, sw, sh, on, maxLine);
    CHECK_EQ(s.dstW <= maxLine, true);
    std::vector<uint32_t> line((size_t)s.dstW + 3); // presentScaled() は dstW + 3 画素まで書く (ASan で見る)
    CheckingStore::reset();
    platform::fillOutside<CheckingStore>(fb, pitchBytes, sw, sh, s, 0xff000000u);
    platform::presentScaled<CheckingStore>(src.data(), srcPitch, fb, pitchBytes, s, line.data(), nullptr, nullptr,
                                           scanline);
    // そろっていない 16 / 64 バイトの書き込みはない。4 バイトずつ書くのは、1 回の書き込み (行の一部) の
    // 頭と終わりの 3 画素ずつまで (写す行が dstH、外を塗るのは 1 行に 3 か所まで)
    CHECK_EQ(CheckingStore::misaligned, 0);
    CHECK_EQ(CheckingStore::n4 <= 6L * (s.dstH + 3L * sh), true);
    CHECK_EQ(CheckingStore::n64 > 0, true);
    printf("  stores: %ld x 4, %ld x 16, %ld x 64 bytes\n", CheckingStore::n4, CheckingStore::n16, CheckingStore::n64);
    int bad = 0;
    for (int y = 0; y < sh; y++) {
        const uint8_t* rowp = fb + (size_t)y * pitchBytes;
        for (int x = 0; x < sw; x++) {
            uint32_t want = 0xff000000u;
            if (x >= s.dstX && x < s.dstX + s.dstW && y >= s.dstY && y < s.dstY + s.dstH) {
                const int sx = s.srcX + (x - s.dstX) / s.kx, sy = s.srcY + (y - s.dstY) / s.ky;
                want = src[(size_t)sy * srcPitch + sx] | 0xff000000u;
                if (scanline && s.ky >= 2 && (y - s.dstY) % s.ky == s.ky - 1)
                    want = ((want >> 1) & 0x007f7f7fu) | 0xff000000u;
            }
            uint32_t got;
            memcpy(&got, rowp + (size_t)x * 4, 4);
            if (got != want && bad++ < 4) printf("  %d,%d: got %08x, want %08x\n", x, y, got, want);
        }
        // 行の右の余り (pitch の残り) には書かない
        for (size_t b = (size_t)sw * 4; b + 4 <= pitchBytes; b += 4) {
            uint32_t v;
            memcpy(&v, rowp + b, 4);
            if (v != 0x12345678 && bad++ < 4) printf("  wrote past the row at %d,%zu\n", y, b);
        }
    }
    CHECK_EQ(bad, 0);
}

// 変わった行だけ写す (FrameRowCache)。1 回目はすべて写し、2 回目からは変えた行と、forceY0〜forceY1 に重なる行と、
// 重ねる絵 (画面の隅の数字) の行だけを書く。どの回もフレームバッファの中身は、すべて写したときと同じになる
void testRowCache(int fw, int fh, int sw, int sh, int gx, int gy = -1, bool scanline = false) {
    if (gy < 0) gy = gx;
    printf("row cache %dx%d on %dx%d gpu x%d y%d%s\n", fw, fh, sw, sh, gx, gy, scanline ? " scanline" : "");
    const int srcPitch = 1024;
    std::vector<uint32_t> src((size_t)srcPitch * fh);
    unsigned seed = 1;
    auto rnd = [&seed] {
        seed = seed * 1103515245u + 12345u;
        return seed >> 8;
    };
    for (auto& p : src) p = rnd() & 0xffffff;
    const size_t pitchBytes = (size_t)sw * 4;
    std::vector<uint32_t> fb((size_t)sw * sh, 0x12345678), want((size_t)sw * sh, 0x12345678);
    const FrameScale s = computeFrameScale(fw, fh, sw, sh, true, 4096, gx, gy);
    std::vector<uint32_t> line((size_t)s.dstW + 3);
    std::vector<uint32_t> prev((size_t)srcPitch * 1024, 0xdeadbeef);
    platform::FrameRowCache cache;
    cache.pixels = prev.data();
    cache.pitch = srcPitch;
    cache.rows = 1024;
    // 重ねる絵 (左上が s.dstX + 8, s.dstY + 4 の 16x6)
    std::vector<uint32_t> ovPixels(16 * 6, 0xff00ff00u);
    platform::FrameOverlay ov;
    ov.pixels = ovPixels.data();
    ov.x = s.dstX + 8;
    ov.y = s.dstY + 4;
    ov.w = 16;
    ov.h = 6;
    for (int round = 0; round < 6; round++) {
        int changed = 0;
        if (round >= 1) {
            // 何行かの 1 画素だけを変える (行の始め、終わり、真ん中)
            const int ys[3] = {0, fh / 2, fh - 1};
            const int xs[3] = {0, fw / 2 + 1, fw - 1};
            for (int i = 0; i < 3; i++) src[(size_t)ys[(i + round) % 3] * srcPitch + xs[i]] ^= 0x10101u * round;
            changed = 3;
        }
        if (round == 3) {
            cache.forceY0 = s.dstY + 10 * s.ky;
            cache.forceY1 = cache.forceY0 + 2 * s.ky;
        }
        if (round == 4) {
            // 重ねる絵が出たとき (ShowFrame() では画面の隅の数字を描いたとき) は、重なる行を写させる
            cache.forceY0 = ov.y;
            cache.forceY1 = ov.y + ov.h;
        }
        if (round == 5) cache.valid = false;
        const platform::FrameOverlay* o = round >= 4 ? &ov : nullptr;
        const uint32_t copied0 = cache.copied, skipped0 = cache.skipped;
        platform::presentScaled<CheckingStore>(src.data(), srcPitch, (uint8_t*)fb.data(), pitchBytes, s, line.data(),
                                               o, &cache, scanline);
        platform::presentScaled<CheckingStore>(src.data(), srcPitch, (uint8_t*)want.data(), pitchBytes, s,
                                               line.data(), o, nullptr, scanline);
        const uint32_t copied = cache.copied - copied0, skipped = cache.skipped - skipped0;
        printf("  round %d: %u rows copied, %u skipped\n", round, copied, skipped);
        CHECK_EQ(copied + skipped, (uint32_t)s.srcH);
        if (round == 0 || round == 5) CHECK_EQ(copied, (uint32_t)s.srcH);
        else if (round == 3) CHECK_EQ(copied, (uint32_t)changed + 2);
        else if (round == 1 || round == 2) CHECK_EQ(copied, (uint32_t)changed);
        CHECK_EQ(cache.forceY0 == 0 && cache.forceY1 == 0, true);
        int bad = 0;
        for (size_t i = 0; i < fb.size(); i++)
            if (fb[i] != want[i] && bad++ < 4) printf("  %zu: got %08x, want %08x\n", i, fb[i], want[i]);
        CHECK_EQ(bad, 0);
    }
}

// 同じ写しのまま画面モードが変わる (ShowFrame() は倍率が変わると枠の外を塗り直し、写しを無効にする)。
// 変わったあとも、フレームバッファの中身はすべて写したときと同じになる
void testRowCacheModeChange() {
    printf("row cache: screen mode change\n");
    const int srcPitch = 1024, sw = 1920, sh = 1080;
    const size_t pitchBytes = (size_t)sw * 4;
    std::vector<uint32_t> src((size_t)srcPitch * 512);
    for (size_t i = 0; i < src.size(); i++) src[i] = (uint32_t)(i * 2654435761u) & 0xffffff;
    std::vector<uint32_t> fb((size_t)sw * sh, 0x12345678), want((size_t)sw * sh, 0x12345678);
    std::vector<uint32_t> line(4096), prev((size_t)srcPitch * 1024);
    platform::FrameRowCache cache;
    cache.pixels = prev.data();
    cache.pitch = srcPitch;
    cache.rows = 1024;
    const int modes[3][2] = {{768, 512}, {256, 256}, {768, 512}};
    FrameScale last;
    for (int m = 0; m < 3; m++) {
        const FrameScale s = computeFrameScale(modes[m][0], modes[m][1], sw, sh, true, 4096);
        if (s != last) {
            cache.valid = false;
            platform::fillOutside<CheckingStore>((uint8_t*)fb.data(), pitchBytes, sw, sh, s, 0xff000000u);
            platform::fillOutside<CheckingStore>((uint8_t*)want.data(), pitchBytes, sw, sh, s, 0xff000000u);
            last = s;
        }
        for (int r = 0; r < 2; r++) {
            platform::presentScaled<CheckingStore>(src.data(), srcPitch, (uint8_t*)fb.data(), pitchBytes, s,
                                                   line.data(), nullptr, &cache);
            platform::presentScaled<CheckingStore>(src.data(), srcPitch, (uint8_t*)want.data(), pitchBytes, s,
                                                   line.data());
            CHECK_EQ(fb == want, true);
        }
    }
}

// スキャンライン: 重ねる絵 (画面の隅の数字) は暗くしない。写しを使っているときにスキャンラインを切り替えても、
// presentScaled() がすべての行を写し直し、すべて写したときと同じになる (写しは無効にしない)
void testScanlineOverlay() {
    printf("scanline: overlay and toggling with the row cache\n");
    const int srcPitch = 1024, sw = 1920, sh = 1080, fw = 768, fh = 512;
    const size_t pitchBytes = (size_t)sw * 4;
    std::vector<uint32_t> src((size_t)srcPitch * fh);
    for (size_t i = 0; i < src.size(); i++) src[i] = (uint32_t)(i * 2654435761u) & 0xffffff;
    const FrameScale s = computeFrameScale(fw, fh, sw, sh, true, 4096);
    CHECK_EQ(s.ky, 2);
    std::vector<uint32_t> fb((size_t)sw * sh, 0x12345678), want((size_t)sw * sh, 0x12345678);
    std::vector<uint32_t> line((size_t)s.dstW + 3), prev((size_t)srcPitch * 1024);
    // X68000 の画面に重なる絵 (左上が s.dstX + 100, s.dstY + 3 の 32x5。奇数行から始まり、暗くする行を含む)
    std::vector<uint32_t> ovPixels(32 * 5);
    for (size_t i = 0; i < ovPixels.size(); i++) ovPixels[i] = 0xffe0e0e0u + (uint32_t)i;
    platform::FrameOverlay ov;
    ov.pixels = ovPixels.data();
    ov.x = s.dstX + 100;
    ov.y = s.dstY + 3;
    ov.w = 32;
    ov.h = 5;
    platform::presentScaled<CheckingStore>(src.data(), srcPitch, (uint8_t*)fb.data(), pitchBytes, s, line.data(), &ov,
                                           nullptr, true);
    int bad = 0;
    for (int y = ov.y; y < ov.y + ov.h; y++)
        for (int x = ov.x; x < ov.x + ov.w; x++)
            if (fb[(size_t)y * sw + x] != ovPixels[(size_t)(y - ov.y) * ov.w + (x - ov.x)]) bad++;
    CHECK_EQ(bad, 0);
    // 重なる行の、絵の外は暗くなっている (y = dstY + 3 は拡大した行の 2 回目)
    {
        const int y = s.dstY + 3, x = s.dstX + 10;
        const uint32_t p = src[(size_t)(s.srcY + 1) * srcPitch + s.srcX + 5] | 0xff000000u;
        CHECK_EQ(fb[(size_t)y * sw + x], ((p >> 1) & 0x007f7f7fu) | 0xff000000u);
        CHECK_EQ(fb[(size_t)(y - 1) * sw + x], p);
    }
    // off → on → off と切り替える
    platform::FrameRowCache cache;
    cache.pixels = prev.data();
    cache.pitch = srcPitch;
    cache.rows = 1024;
    const bool seq[4] = {false, true, true, false};
    for (int i = 0; i < 4; i++) {
        const uint32_t copied0 = cache.copied;
        platform::presentScaled<CheckingStore>(src.data(), srcPitch, (uint8_t*)fb.data(), pitchBytes, s, line.data(),
                                               nullptr, &cache, seq[i]);
        platform::presentScaled<CheckingStore>(src.data(), srcPitch, (uint8_t*)want.data(), pitchBytes, s,
                                               line.data(), nullptr, nullptr, seq[i]);
        CHECK_EQ(fb == want, true);
        // 1 回目と切り替えたときはすべての行を、同じスキャンラインのまま (i == 2) なら 1 行も写さない
        CHECK_EQ(cache.copied - copied0, i == 2 ? 0u : (uint32_t)s.srcH);
    }
}

// 重ねる絵が 2 つ (右上の数字と左上のメッセージ)。同じ行に並ぶとき、横に重なるとき (左の絵を優先する)、
// 片方が画面の左にはみ出すとき。重ねた画素の外は X68000 の画面のまま
void testTwoOverlays() {
    printf("two overlays\n");
    const int srcPitch = 1024, sw = 640, sh = 480, fw = 512, fh = 480;
    const size_t pitchBytes = (size_t)sw * 4;
    std::vector<uint32_t> src((size_t)srcPitch * fh);
    for (size_t i = 0; i < src.size(); i++) src[i] = (uint32_t)(i * 2654435761u) & 0xffffff;
    const FrameScale s = computeFrameScale(fw, fh, sw, sh, true, 4096);
    std::vector<uint32_t> line((size_t)s.dstW + 3);
    std::vector<uint32_t> pa(40 * 10), pb(36 * 6);
    for (size_t i = 0; i < pa.size(); i++) pa[i] = 0xffa00000u + (uint32_t)i;
    for (size_t i = 0; i < pb.size(); i++) pb[i] = 0xff00b000u + (uint32_t)i;
    struct Case {
        int ax, ay, bx, by;
    } cases[] = {
        {s.dstX + 400, s.dstY + 4, s.dstX + 8, s.dstY + 8},   // 右と左、行は一部だけ重なる
        {s.dstX + 100, s.dstY + 4, s.dstX + 120, s.dstY + 6}, // 横に重なる (左の a を優先)
        {s.dstX + 120, s.dstY + 4, s.dstX + 100, s.dstY + 6}, // 横に重なる (左の b を優先)
        {s.dstX - 20, s.dstY, s.dstX + s.dstW - 30, s.dstY},  // 画面の左右にはみ出す
    };
    for (const Case& c : cases) {
        platform::FrameOverlay a, b;
        a.pixels = pa.data();
        a.x = c.ax;
        a.y = c.ay;
        a.w = 40;
        a.h = 10;
        b.pixels = pb.data();
        b.x = c.bx;
        b.y = c.by;
        b.w = 36;
        b.h = 6;
        std::vector<uint32_t> fb((size_t)sw * sh, 0x12345678);
        platform::presentScaled<CheckingStore>(src.data(), srcPitch, (uint8_t*)fb.data(), pitchBytes, s, line.data(),
                                               &a, nullptr, false, &b);
        int bad = 0;
        for (int y = s.dstY; y < s.dstY + s.dstH; y++)
            for (int x = s.dstX; x < s.dstX + s.dstW; x++) {
                const bool inA = x >= a.x && x < a.x + a.w && y >= a.y && y < a.y + a.h;
                const bool inB = x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
                uint32_t want;
                if (inA && (!inB || a.x <= b.x)) want = pa[(size_t)(y - a.y) * a.w + (x - a.x)];
                else if (inB) want = pb[(size_t)(y - b.y) * b.w + (x - b.x)];
                else
                    want = src[(size_t)(s.srcY + (y - s.dstY) / s.ky) * srcPitch + s.srcX + (x - s.dstX) / s.kx] |
                           0xff000000u;
                if (fb[(size_t)y * sw + x] != want && bad++ < 4)
                    printf("  %d,%d: got %08x, want %08x\n", x, y, fb[(size_t)y * sw + x], want);
            }
        CHECK_EQ(bad, 0);
    }
}

} // namespace

int main() {
    testFactors();
    testTwoOverlays();
    testPresent(256, 256, 1600, 1200, 1600 * 4, true);
    testPresent(512, 512, 1920, 1080, 1920 * 4, true);
    testPresent(256, 240, 1280, 720, 1280 * 4 + 4, true); // 16 バイトにそろわない行
    testPresent(352, 240, 1366, 768, 1366 * 4 + 8, true);
    testPresent(768, 512, 640, 480, 640 * 4, true); // 切り出す
    testPresent(768, 512, 1920, 1080, 1920 * 4, false);
    testPresent(768, 512, 1920, 1080, 1920 * 4, true);  // 2 倍 (4 画素ずつ作る)
    testPresent(766, 510, 1920, 1080, 1920 * 4, true);  // 2 倍で、4 で割り切れない幅
    testPresent(765, 500, 1920, 1080, 1920 * 4, false); // 1 倍で、4 で割り切れない幅
    testPresent(255, 255, 1920, 1080, 1920 * 4, true);  // 6x4 で、行の終わりの先に書く分
    // 3 倍以上で dstW が行のバッファいっぱいになる (行の終わりの先の 3 画素まで書く。line は dstW + 3 画素)
    testPresent(256, 256, 1920, 1080, 1920 * 4, true, 1536); // 6 倍、dstW = 1536
    testPresent(512, 512, 1920, 1080, 1920 * 4, true, 1536); // 3 倍、dstW = 1536
    testPresent(256, 256, 1920, 1080, 1920 * 4, true, 1530); // 6 倍、1530 / 6 = 255 画素を写す (dstW = 1530)
    testPresent(300, 300, 1920, 1080, 1920 * 4, true, 1500); // 5 倍、dstW = 1500 (行の終わりの先に 3 画素書く)
    testPresent(384, 256, 1920, 1080, 1920 * 4, true);       // 4 倍
    testPresent(382, 256, 1920, 1080, 1920 * 4, true);       // 4 倍で、4 で割り切れない幅
    testPresent(510, 512, 1920, 1080, 1920 * 4, true);       // 3 倍で、4 で割り切れない幅
    testPresent(764, 512, 1920, 1080, 1920 * 4, true);       // 2 倍で、8 で割り切れない幅
    testPresent(1020, 512, 1920, 1080, 1920 * 4, true);      // 1 倍で、8 で割り切れない幅
    testRowCache(768, 512, 1920, 1080, 1);
    testRowCache(768, 512, 960, 540, 2);
    testRowCache(256, 256, 1920, 1080, 1);
    testRowCache(766, 510, 1920, 1080, 1); // 8 で割り切れない幅
    testRowCache(510, 512, 1920, 540, 1, 2);
    testRowCacheModeChange();
    // スキャンライン (縦 2 倍、4 倍、3x2、1 倍 (何もしない)、拡大しない)
    testPresent(768, 512, 1920, 1080, 1920 * 4, true, 4096, true);
    testPresent(766, 510, 1920, 1080, 1920 * 4, true, 4096, true);
    testPresent(256, 256, 1920, 1080, 1920 * 4, true, 4096, true);
    testPresent(512, 512, 1920, 1080, 1920 * 4, true, 4096, true);
    testPresent(256, 240, 1280, 720, 1280 * 4 + 4, true, 4096, true);
    testPresent(768, 512, 1920, 1080, 1920 * 4, false, 4096, true);
    testPresent(1020, 512, 1920, 1080, 1920 * 4, true, 4096, true);
    testRowCache(768, 512, 1920, 1080, 1, -1, true);
    testRowCache(256, 256, 1920, 1080, 1, -1, true);
    testRowCache(510, 512, 1920, 540, 1, 2, true); // フレームバッファの中は縦 1 倍
    testScanlineOverlay();
    return finish();
}
