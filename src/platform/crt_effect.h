// 画面の出し方: ブラウン管のように見せる効果 (pix68k.cfg の video_filter=。既定は効果なし) と、整数倍でない拡大
// (video_scale=aspect、full)。ベアメタル版の CKernel::ShowFrame() と、ホスト版の SdlVideo::present() が使う。
// X68000 の画面 (合成した画素) は変えず、表示に写すときだけ掛ける。
//
// 効果 (3 つは自由に組み合わせられる。組み合わせの番号は platform/crt_preset.h の CrtPreset のビット):
//   scanline  スキャンライン (拡大した各行の最後の 1 行を半分の明るさにする)。ベアメタル版で整数倍 (か 1 倍) のときに
//             ほかの 2 つがなければ、platform/frame_scale.h の presentScaled() が作る (ここの表は使わない)
//   curve     たる型のゆがみ + 四隅を暗くする (ビネット)
//   mask      アパーチャーグリル (RGB の縦じま) + 少しのにじみ
// 写す枠 (videoFrameScale()): off は 1 倍、integer は整数倍 (platform::computeFrameScale())、aspect は 4:3 で高さいっぱい、
// full は表示いっぱい。整数倍でない枠 (aspect、full) は、効果がなくても表を引いて写す (表示の画素ごとに元の画素を決める)。
//
// 部品の作り方 (どれも整数で計算する。機種やコンパイラで結果が変わらない):
//   ゆがみ: 表示の画素の位置を -1..1 にした (nx, ny) から、元の画面の位置 (u, v) を
//       u = nx * (1 + k * ny^2)、v = ny * (1 + k * nx^2)
//     で決め、いちばん近い画素を取る。画面の真ん中を通る縦と横の線の上では拡大の倍率がそのまま (整数倍なら整数倍のまま) で、
//     四隅に近いほど絵が内側に寄る。元の画面の外になるところは黒。
//   スキャンライン: 元の 1 行から作る表示の行のうち、最後の 1 行だけを半分の明るさにする (中間の明るさは使わない。
//     縦の倍率が整数でないとき (1080 / 512 = 2.11) に、中間の明るさがむらに見えるため)。整数倍なら、拡大した各行の
//     最後の 1 行になる (presentScaled() と同じ)。ゆがみがあると筋が曲がり、各桁で同じように決める。
//     縦の倍率が 2 より小さければ入れない。
//   ビネット: 真ん中からの距離の 4 乗に比例して暗くする。
//   アパーチャーグリル: 表示の横 3 画素を周期として、R、G、B のうち 1 色はそのまま、ほかの 2 色を 1/4 暗くする
//     (色 - (色 >> 2)。掛け算を使わない)。
//   にじみ: 拡大した 1 行の中で、左右の画素と 1:2:1 で混ぜる。
//
// 表 (CrtMap): 表示の大きさと元の画面の大きさが変わったときだけ作り直す。ゆがみかビネットのある表 (curve) は
// 作るのに時間がかかる (Pi 3 の 1920x1080 で 39ms) ので、少しずつ作れる: crtBuildBegin() で始め、crtBuildStep() で
// 何行かずつ進める。ベアメタル版は、フレームを写すたびに crtMapAdvance() で決めた時間の分だけ進め、できあがるまでは
// 整数倍の枠に、スキャンラインだけ (なければ効果なし) で写す (表を作る間に次のフレームが来て、写していないフレームが置き換わらないように。
// フレームは飛ばさない)。crtBuildMap() は 1 回で最後まで作る (ホスト版、X68000 が止まっているとき)。
//   sx      表示の画素ごとの、元の画面の桁。ゆがみがなければ 1 行分だけ
//   segs    表示の行ごとに、「元の同じ行から取る区間」を左から並べたもの (ゆがみがあると、1 行の中で元の行が変わる)
//   weight  表示の画素ごとの明るさ (スキャンライン x ビネット)。ゆがみもビネットもなければ行ごとの 1 つの値 (rowWeight)
//   colMask 表示の桁ごとの、アパーチャーグリルで暗くする色 (暗くする色のバイトが 0xff)
// 明るさは 0..255 の値 w で持ち、画素の各色を (色 * (w + 1)) >> 8 にする (255 ならそのまま)。
//
// 1 行を作る (crtRenderRow()): 元の画素を集め (segs と sx)、にじませ、アパーチャーグリルを掛け、明るさを掛ける。
// ベアメタル版は作った 1 行をフレームバッファに書く (presentCrt())。前に写したときから、その行が使う元の行が
// 変わっていなければ書かない (FrameRowCache)。ゆがみがないときは、元の同じ行から作る表示の行 (縦の倍率の数だけある) で、
// 明るさを掛ける前までを 1 回だけ計算する。重ねる絵 (右上の数字、左上のメッセージ) は書くときに差し替えるので、ゆがまない。
// aarch64 では NEON の命令を使う (Cortex-A53 では、コンパイラのベクトルの拡張で書くと 3 倍ほど遅かった)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>

// PIX68K_CRT_NO_NEON を定義すると、aarch64 でも NEON の命令を使わない (試験で、使わない書き方を確かめるため)
#if defined(__aarch64__) && !defined(PIX68K_CRT_NO_NEON)
#define PIX68K_CRT_NEON 1
#include <arm_neon.h>
#endif

#include "platform/crt_preset.h"
#include "platform/frame_scale.h"

namespace platform {

// 部品の強さ
struct CrtParams {
    int curve = 0;    // ゆがみの k (1.0 を 65536 とする。0 ならゆがませない)
    int vignette = 0; // 四隅で暗くする量 (1.0 を 256 とする)
    int scanline = 0; // スキャンラインの暗い筋で暗くする量 (1.0 を 256 とする。128 なら半分の明るさ)
    bool mask = false; // アパーチャーグリル (ほかの 2 色を 1/4 暗くする)
    bool blur = false;

    bool any() const { return curve != 0 || vignette != 0 || scanline != 0 || mask || blur; }
};

// ゆがみとビネットの強さ (見た目を調整するときは、この 2 つだけを変える。表と試験はこの値から決まる)。
// kCrtCurve: ゆがみの k (1.0 を 65536)。四隅は縦横とも k の分だけ内側に寄り、上下左右の辺は真ん中に向かってふくらむ。
//   0.0225 (1475) なら、1440x1080 の枠で元の画面の四隅が上から約 11 行、左から約 15 桁の所に出る。
//   最初は 0.045 (2949) だったが、持ち主が見て強すぎたので半分にした
// kCrtVignette: 四隅で暗くする量 (1.0 を 256)。最初は 0.375 (96)
static constexpr int kCrtCurve = 1475;   // 0.0225
static constexpr int kCrtVignette = 64;  // 四隅で 0.25 暗くする
static constexpr int kCrtScanline = 128; // 整数倍のスキャンライン (presentScaled()) と同じ半分

inline CrtParams crtParams(int preset) {
    CrtParams p;
    if (preset <= CRT_OFF || preset >= CRT_PRESET_COUNT) return p;
    if (preset & CRT_SCANLINE) p.scanline = kCrtScanline;
    if (preset & CRT_MASK) {
        p.mask = true;
        p.blur = true;
    }
    if (preset & CRT_CURVE) {
        p.curve = kCrtCurve;
        p.vignette = kCrtVignette;
    }
    return p;
}

static constexpr int kCrtFillMaxH = 1200; // 整数倍でない枠の高さの上限 (3840x2160 などで写す量が増えすぎないように)

// video_scale= の mode で、fw x fh の画面を sw x sh の表示に写す範囲。base は computeFrameScale() の結果
// (enable は mode が off でないこと。maxLine、gx、gy もそれに渡したもの)。off と integer は base のまま。
// aspect: 表示の高さいっぱい (maxH まで) を使い、横は X68000 のモニタと同じ 4:3 にする (1920x1080 なら 1440x1080)。
//   どの画面モード (768x512、512x512、256x256 など) も同じ枠に出す (実機も 4:3 のブラウン管に映していた)。
// full: 表示いっぱい (1920x1080 なら 1920x1080)。高さが maxH を超えるときは、表示の縦横比のまま maxH に縮める。
// 返す kx と ky は 0 (整数倍ではない印。presentScaled() には渡さない。表を引いて写す)。
// 次のときは base をそのまま返す: gx と gy が違う、枠が元の画面より小さい (フレームバッファが小さいと、元の画素を
// 落とすことになるため)。maxH はベアメタル版は kCrtFillMaxH、ホスト版は表示の高さ (表は別に小さく作る。videoWindowFrame())
// shrink が true なら、枠が元の画面より小さくても base に戻さず、縮めて出す (ホスト版。ウィンドウが小さくても切らない)
inline FrameScale videoFrameScale(int mode, const FrameScale& base, int fw, int fh, int sw, int sh, int maxLine,
                                  int gx = 1, int gy = 1, int maxH = kCrtFillMaxH, bool shrink = false) {
    if ((mode != VIDEO_SCALE_ASPECT && mode != VIDEO_SCALE_FULL) || fw <= 0 || fh <= 0 || sw <= 0 || sh <= 0) return base;
    if (gx < 1) gx = 1;
    if (gy < 1) gy = 1;
    if (gx != gy) return base;
    maxH /= gy;
    const int lim = sw < maxLine ? sw : maxLine;
    int w, h;
    if (mode == VIDEO_SCALE_ASPECT) {
        h = sh < maxH ? sh : maxH;
        w = h * 4 / 3;
        if (w > lim) {
            w = lim;
            h = w * 3 / 4;
        }
    } else {
        h = sh < maxH ? sh : maxH;
        w = (int)((int64_t)sw * h / sh);
        if (w > lim) {
            w = lim;
            h = (int)((int64_t)sh * w / sw);
        }
    }
    if ((w < fw || h < fh) && !shrink) return base;
    if (w < 1 || h < 1) return base;
    FrameScale s;
    s.kx = s.ky = 0;
    s.srcX = s.srcY = 0;
    s.srcW = fw;
    s.srcH = fh;
    s.dstW = w;
    s.dstH = h;
    s.dstX = ((sw - w) / 2) & ~3; // 16 バイトにそろえる
    s.dstY = (sh - h) / 2;
    return s;
}

// 表示の 1 行の中の、元の同じ行から取る区間。x1 の手前まで (前の区間の x1 から) を、元の row 行から取る。
// row が -1 なら黒
struct CrtSeg {
    uint16_t x1;
    int16_t row;
};

static constexpr int kCrtMaxSegs = 64;      // 表示の 1 行の区間の数の上限
static constexpr int kCrtMaxWidth = 4096;   // 表示の幅の上限 (sx と CrtSeg::x1 は 16 ビット)
static constexpr int kCrtMaxSrc = 2048;     // 元の画面の幅と高さの上限

// ホスト版 (host/main.cpp の SdlVideo) の、ウィンドウ (ow x oh 画素) の中の X68000 の画面 (fw x fh) の置き方。
// ベアメタル版と同じく computeFrameScale() と videoFrameScale() で決める (mode は video_scale=)。
//   s: 写す範囲 (s.dstX、s.dstY、s.dstW x s.dstH がウィンドウの中の絵。kx が 0 なら整数倍でない)。aspect と full の高さは
//     ウィンドウいっぱいまで (ベアメタル版の 1200 ラインの上限はない)。ウィンドウが X68000 の画面より小さければ縮めて出す
//     (ベアメタル版と違い、整数倍に戻して切り出さない)
//   cw x ch: 効果の表 (テクスチャ) の大きさ。高さが kCrtFillMaxH を超えるときは、その高さに縮めた大きさで作り
//     (CPU で作る量を抑えるため)、幅が表の上限 (kCrtMaxWidth) を超えるときは整数で割った大きさで作る。
//     SDL に絵の大きさ (s.dstW x s.dstH) まで拡大させる。表を作れない大きさなら cw = ch = 0
struct CrtWindowFrame {
    FrameScale s;
    int cw = 0, ch = 0;
};

static constexpr int kHostMaxLine = 1 << 14; // ホスト版の絵の幅の上限 (computeFrameScale() の maxLine)

inline CrtWindowFrame videoWindowFrame(int mode, int ow, int oh, int fw, int fh) {
    CrtWindowFrame f;
    if (ow <= 0 || oh <= 0 || fw <= 0 || fh <= 0) return f;
    const FrameScale base = computeFrameScale(fw, fh, ow, oh, mode != VIDEO_SCALE_OFF, kHostMaxLine);
    f.s = videoFrameScale(mode, base, fw, fh, ow, oh, kHostMaxLine, 1, 1, oh, true);
    int cw = f.s.dstW, ch = f.s.dstH;
    if (ch > kCrtFillMaxH) {
        cw = (int)((int64_t)cw * kCrtFillMaxH / ch);
        ch = kCrtFillMaxH;
    }
    const int div = (cw + kCrtMaxWidth - 1) / kCrtMaxWidth;
    if (div > 1) {
        cw /= div;
        ch /= div;
    }
    if (cw >= 16 && ch >= 16) {
        f.cw = cw;
        f.ch = ch;
    }
    return f;
}

struct CrtMap {
    // ---- メモリ (crtMapInit() が mem から切り出す) ----
    int maxW = 0, maxH = 0;       // 作れる表示の大きさの上限
    uint16_t* sx = nullptr;       // maxW * maxH
    uint8_t* weight = nullptr;    // maxW * maxH
    CrtSeg* segs = nullptr;       // maxH * kCrtMaxSegs
    uint32_t* segStart = nullptr; // maxH + 1 (行 y の区間は segs[segStart[y]] から segs[segStart[y + 1]] の手前まで)
    uint32_t* colMask = nullptr;  // maxW (アパーチャーグリルで暗くする色のバイトが 0xff。0x00RRGGBB)
    int16_t* rowMin = nullptr;    // maxH (行 y が使う元の行の範囲。使わなければ rowMin > rowMax)
    int16_t* rowMax = nullptr;    // maxH
    uint8_t* rowWeight = nullptr; // maxH (weight を画素ごとに持たないときの、行ごとの明るさ)
    uint16_t* dirty = nullptr;    // kCrtMaxSrc + 1 (presentCrt() が使う。元の行が変わったかの数の累計)
    int64_t* colTmp = nullptr;    // maxW * 2 (crtBuildMap() が使う)

    // ---- 作っている途中 (crtBuildBegin() / crtBuildStep()) ----
    bool building = false;   // 途中まで作ってある (valid は false)
    int buildY = 0;          // 次に作る表示の行
    uint32_t buildSegs = 0;  // ここまでの区間の数
    uint32_t serial = 0;     // crtBuildBegin() のたびに増える (表を作り直したことを、使う側が知るため)

    // ---- 作った表 ----
    bool valid = false;
    int preset = -1;
    CrtParams params;
    int srcW = 0, srcH = 0, dstW = 0, dstH = 0;
    int kx = 0;              // ゆがみがなく、横が整数倍ならその倍率 (frame_scale.h の scaleLine() で集める)。ほかは 0
    bool perPixel = false;   // sx と weight を表示の行ごとに持つ (ゆがみかビネットがある)

    // この組み合わせの表ができている
    bool matches(int p, int sw, int sh, int dw, int dh) const { return valid && tried(p, sw, sh, dw, dh); }
    // この組み合わせの表を最後に作ろうとした (できなかったときと、作りかけ (building) のときも。同じものを何度も
    // 作り直さないため)
    bool tried(int p, int sw, int sh, int dw, int dh) const {
        return preset == p && srcW == sw && srcH == sh && dstW == dw && dstH == dh;
    }
};

// maxW x maxH までの表示の表に要るメモリ (バイト)
inline size_t crtMapBytes(int maxW, int maxH) {
    const size_t w = (size_t)maxW, h = (size_t)maxH;
    return w * 2 * 8 + (h + 1) * 4 + w * 4 + h * kCrtMaxSegs * sizeof(CrtSeg) + w * h * 2 + h * 2 * 2 +
           (kCrtMaxSrc + 1) * 2 + w * h + h + 64;
}

// mem (crtMapBytes(maxW, maxH) バイト以上。8 バイトにそろっていること) を表のメモリにする
inline void crtMapInit(CrtMap& m, void* mem, int maxW, int maxH) {
    const size_t w = (size_t)maxW, h = (size_t)maxH;
    uint8_t* p = (uint8_t*)mem;
    // そろえの大きいものから並べる
    m.colTmp = (int64_t*)p;
    p += w * 2 * 8;
    m.segStart = (uint32_t*)p;
    p += (h + 1) * 4;
    m.colMask = (uint32_t*)p;
    p += w * 4;
    m.segs = (CrtSeg*)p;
    p += h * kCrtMaxSegs * sizeof(CrtSeg);
    m.sx = (uint16_t*)p;
    p += w * h * 2;
    m.rowMin = (int16_t*)p;
    p += h * 2;
    m.rowMax = (int16_t*)p;
    p += h * 2;
    m.dirty = (uint16_t*)p;
    p += (kCrtMaxSrc + 1) * 2;
    m.weight = p;
    p += w * h;
    m.rowWeight = p;
    m.maxW = maxW;
    m.maxH = maxH;
    m.valid = false;
    m.building = false;
    m.preset = -1;
}

namespace crt_detail {
// スキャンラインを入れるか。縦の倍率が 2 より小さければ入れない (元の 1 行が表示の 1 行になる所が多く、暗い行が
// 元の行を消してしまうため)
struct Scanline {
    bool on = false;
    Scanline(int srcH, int dstH, int d) { on = d != 0 && srcH > 0 && dstH >= 2 * srcH; }
};

inline uint8_t packWeight(int w256) { return (uint8_t)(w256 < 1 ? 0 : w256 > 256 ? 255 : w256 - 1); }
} // namespace crt_detail

// srcW x srcH の画面を dstW x dstH に出すときの表を作り始める (前に作った表、作りかけの表は捨てる)。
// 作れなければ (大きすぎる、メモリがない) false を返す (m.valid も m.building も false。m.tried() は true になる)。
// ゆがみもビネットもない表 (curve がない) は軽いので、ここで作り終える (m.valid が true)。ほかは m.building を true にして返すので、
// crtBuildStep() で続きを作る。
// params を渡すと、preset の部品の強さ (crtParams()) の代わりにそれを使う (試験で、ふだんの強さでは起きない
// 「区間が入りきらない」場合を作るため)
inline bool crtBuildBegin(CrtMap& m, int preset, int srcW, int srcH, int dstW, int dstH,
                          const CrtParams* params = nullptr) {
    using namespace crt_detail;
    m.valid = false;
    m.building = false;
    m.buildY = 0;
    m.buildSegs = 0;
    m.serial++;
    m.preset = preset;
    m.srcW = srcW;
    m.srcH = srcH;
    m.dstW = dstW;
    m.dstH = dstH;
    if (m.sx == nullptr || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return false;
    if (dstW > m.maxW || dstH > m.maxH || dstW > kCrtMaxWidth || srcW > kCrtMaxSrc || srcH > kCrtMaxSrc) return false;
    // 効果がなくても作る (整数倍でない枠で、表示の画素ごとに元の画素を決めるため)
    const CrtParams p = params != nullptr ? *params : crtParams(preset);
    m.params = p;
    m.perPixel = p.curve != 0 || p.vignette != 0;
    m.kx = !m.perPixel && dstW % srcW == 0 ? dstW / srcW : 0;

    // アパーチャーグリル: 横 3 画素を周期に、R、G、B の順で 1 色だけそのままにする (ほかの 2 色を暗くする)
    {
        static const uint32_t pat[3] = {0x0000ffffu, 0x00ff00ffu, 0x00ffff00u};
        for (int x = 0; x < dstW; x++) m.colMask[x] = p.mask ? pat[x % 3] : 0;
    }

    if (!m.perPixel) {
        const Scanline sl(srcH, dstH, p.scanline);
        // ゆがみなし: 桁は 1 行分、明るさは行ごと。画素の真ん中 (x + 0.5) を元の画面に写して、いちばん近い画素を取る
        for (int x = 0; x < dstW; x++) m.sx[x] = (uint16_t)(((int64_t)(2 * x + 1) * srcW) / (2 * dstW));
        // スキャンラインは、元の 1 行から作る表示の行のうち最後の 1 行 (次の行が別の元の行になる行) だけを暗くする
        auto rowOf = [&](int y) { return (int)(((int64_t)(2 * y + 1) * srcH) / (2 * dstH)); };
        for (int y = 0; y < dstH; y++) {
            const int row = rowOf(y);
            m.segStart[y] = (uint32_t)y;
            m.segs[y] = CrtSeg{(uint16_t)dstW, (int16_t)row};
            m.rowMin[y] = m.rowMax[y] = (int16_t)row;
            const bool last = y == dstH - 1 || rowOf(y + 1) != row;
            m.rowWeight[y] = packWeight(sl.on && last ? 256 - p.scanline : 256);
        }
        m.segStart[dstH] = (uint32_t)dstH;
        m.buildY = dstH;
        m.valid = true;
        return true;
    }

    // ゆがみあり。元の画面の位置は 1 画素を 2^32 として持つ (64 ビット)。
    //   横: 元の桁 = srcW / 2 + (2x + 1 - dstW) * A * srcW / (2 * dstW)。A = 1 + k * ny^2 は行で決まるので、
    //       x が 1 増えるごとに A * srcW / dstW を足していく。
    //   縦: 元の行 = srcH / 2 + (2y + 1 - dstH) * B * srcH / (2 * dstH)。B = 1 + k * nx^2 は桁で決まるので、
    //       桁ごとの B * srcH / dstH を先に作り (ここ)、画素ごとに (2y + 1 - dstH) を掛ける (crtBuildStep())。
    int64_t* const colStep = m.colTmp;     // 桁ごとの B * srcH / dstH
    int64_t* const nx2q = m.colTmp + dstW; // 桁ごとの nx^2 (1.0 を 65536)
    for (int x = 0; x < dstW; x++) {
        const int64_t nx = (int64_t)(2 * x + 1 - dstW) * 65536 / dstW;
        nx2q[x] = (nx * nx) >> 16;
        const int64_t b = 65536 + ((p.curve * nx2q[x]) >> 16);
        colStep[x] = ((b * srcH) << 16) / dstH;
    }
    m.building = true;
    return true;
}

// 作りかけの表 (m.building) を、表示の行で長くて rows 行だけ作る。まだ残っていれば true を返す。
// 最後の行まで作ったら m.valid を true に、入りきらなければ (区間が多すぎる) false のままにして、false を返す
// (どちらも m.building は false)。作りかけでなければ何もしない
inline bool crtBuildStep(CrtMap& m, int rows) {
    using namespace crt_detail;
    if (!m.building) return false;
    const CrtParams& p = m.params;
    const int srcW = m.srcW, srcH = m.srcH, dstW = m.dstW, dstH = m.dstH;
    const Scanline sl(srcH, dstH, p.scanline);
    const bool scan = sl.on;
    const int64_t* const colStep = m.colTmp;
    const int64_t* const nx2q = m.colTmp + dstW;
    // 左右は対称なので、左半分 (奇数の幅なら真ん中の桁まで) だけを計算し、右半分は左右を入れ替えて写す
    // (元の桁は srcW - 1 - 桁、元の行と明るさは同じ)。計算が半分で済む
    const int64_t limX = (int64_t)srcW << 32, limY = (int64_t)srcH << 32;
    const int64_t halfY = (int64_t)srcH << 31;
    const int half = (dstW + 1) / 2;
    uint32_t nseg = m.buildSegs;
    const int yEnd = rows < dstH - m.buildY ? m.buildY + rows : dstH;
    for (int y = m.buildY; y < yEnd; y++) {
        const int64_t yn = 2 * y + 1 - dstH;
        const int64_t ny = yn * 65536 / dstH;
        const int64_t ny2 = (ny * ny) >> 16;
        const int64_t a = 65536 + ((p.curve * ny2) >> 16);
        const int64_t stepX = ((a * srcW) << 16) / dstW;
        int64_t px = ((int64_t)srcW << 31) + (1 - dstW) * stepX / 2;
        uint16_t* const sx = m.sx + (size_t)y * dstW;
        uint8_t* const wt = m.weight + (size_t)y * dstW;
        m.segStart[y] = nseg;
        CrtSeg* const segs = m.segs + nseg; // この行の区間
        int n = 0;                          // 左半分の区間の数 (最後の区間は half で終わる)
        int cur = -2, lo = 32767, hi = -1;
        for (int x = 0; x < half; x++, px += stepX) {
            const int64_t py = halfY + ((yn * colStep[x]) >> 1);
            int row = -1;
            if ((uint64_t)px < (uint64_t)limX && (uint64_t)py < (uint64_t)limY) row = (int)(py >> 32);
            if (row != cur) {
                if (cur != -2) segs[n++] = CrtSeg{(uint16_t)x, (int16_t)cur};
                if (n >= kCrtMaxSegs / 2 - 1) {
                    m.building = false;
                    return false;
                }
                cur = row;
            }
            const int mx = dstW - 1 - x; // 右半分の同じ位置
            if (row < 0) {
                sx[x] = sx[mx] = 0;
                wt[x] = wt[mx] = 255;
                continue;
            }
            if (row < lo) lo = row;
            if (row > hi) hi = row;
            const int col = (int)(px >> 32);
            sx[mx] = (uint16_t)(srcW - 1 - col);
            sx[x] = (uint16_t)col; // 奇数の幅の真ん中の桁 (mx == x) は、こちらが残る
            // スキャンライン: この桁で、元の 1 行から作る表示の行 (縦に並ぶ) のうち最後の 1 行だけを暗くする。表示の行が
            // 1 増えるごとに元の位置は colStep[x] 進むので、元の 1 行の終わりの colStep[x] の幅に真ん中が入る画素が
            // ちょうど 1 つある。重なる割合で中間の明るさにすると、倍率が整数でないとき (1080 / 512 = 2.11) に、
            // 暗い 1 行と少し暗い 2 行が 19 行ほどの周期で入れ替わってむらに見えるため。
            // 幅は元の 1 行の半分まで (整数倍の 2 倍の枠の左右の端では、ゆがみで元の 1 行が表示の 1 行になることがあり、
            // その 1 行を暗くすると元の行が暗く見えるので、なるべく暗くしない)
            int w = scan && (int64_t)(py & 0xffffffffll) + (colStep[x] < ((int64_t)1 << 31) ? colStep[x] : (int64_t)1 << 31) >=
                                ((int64_t)1 << 32)
                        ? 256 - p.scanline
                        : 256;
            if (p.vignette != 0) {
                const uint32_t r2 = (uint32_t)((nx2q[x] + ny2) >> 1); // 0..1 (四隅で 1。1.0 を 65536)
                const uint32_t r4 = (uint32_t)(((uint64_t)r2 * r2) >> 16);
                w = (w * (256 - (int)(((uint32_t)p.vignette * r4) >> 16))) >> 8;
            }
            wt[x] = wt[mx] = packWeight(w);
        }
        // 真ん中の区間は左右でつながる (終わりを右半分の同じ位置にする)。その右に、左半分の区間を逆の順に並べる
        int total = n;
        segs[total++] = CrtSeg{(uint16_t)(n > 0 ? dstW - segs[n - 1].x1 : dstW), (int16_t)cur};
        for (int i = n - 1; i >= 0; i--)
            segs[total++] = CrtSeg{(uint16_t)(i > 0 ? dstW - segs[i - 1].x1 : dstW), segs[i].row};
        nseg += (uint32_t)total;
        m.rowMin[y] = (int16_t)lo;
        m.rowMax[y] = (int16_t)hi;
    }
    m.buildY = yEnd;
    m.buildSegs = nseg;
    if (yEnd < dstH) return true;
    m.segStart[dstH] = nseg;
    m.building = false;
    m.valid = true;
    return false;
}

// 表を 1 回で最後まで作る。入りきらなければ (大きすぎる、区間が多すぎる) false を返し、m.valid を false にする
inline bool crtBuildMap(CrtMap& m, int preset, int srcW, int srcH, int dstW, int dstH) {
    if (!crtBuildBegin(m, preset, srcW, srcH, dstW, dstH)) return false;
    while (crtBuildStep(m, dstH)) {
    }
    return m.valid;
}

static constexpr int kCrtBuildChunkRows = 8; // crtMapAdvance() が時刻を見る間隔 (表示の行。Pi 3 の 1920 画素で約 0.3ms)

// フレームを写す前に呼ぶ。この組み合わせの表ができていれば true を返す (効果を掛けて写せる)。できていなければ、
// 作り始めるか続きを作り、now() (増えていく時刻。単位は呼び手が決める) で測って budget だけたったらやめる
// (時刻を見るのは kCrtBuildChunkRows 行ごとなので、そのぶんは超える)。budget が 0 なら、作り始めるだけで進めない
// (画面の大きさが変わってすべての行を写すフレームなど)。false を返したフレームは、呼び手が効果なしかスキャンラインだけで写す。
// 作り進めた呼び出しでは、そこでできあがっても false を返す (効果を掛けるのは次のフレームから。できあがった表で初めて
// 写すときはすべての行を書くので、作る時間と同じフレームに重ねない)。
// X68000 が止まっていて次のフレームが来ないとき (設定メニューなど) は、crtMapFinish() で最後まで作る
template <class Now>
inline bool crtMapAdvance(CrtMap& m, int preset, int srcW, int srcH, int dstW, int dstH, uint64_t budget, Now&& now) {
    if (!m.tried(preset, srcW, srcH, dstW, dstH)) crtBuildBegin(m, preset, srcW, srcH, dstW, dstH);
    if (!m.building || budget == 0) return m.valid;
    const uint64_t t0 = now();
    while (crtBuildStep(m, kCrtBuildChunkRows) && (uint64_t)(now() - t0) < budget) {
    }
    return false;
}

// crtMapAdvance() と同じだが、時間を区切らずに最後まで作る
inline bool crtMapFinish(CrtMap& m, int preset, int srcW, int srcH, int dstW, int dstH) {
    if (!m.tried(preset, srcW, srcH, dstW, dstH)) crtBuildBegin(m, preset, srcW, srcH, dstW, dstH);
    while (crtBuildStep(m, m.dstH)) {
    }
    return m.valid;
}

// crtMapPrepare() の結果
struct CrtPrepared {
    bool ready = false;    // 表ができている (このフレームは効果を掛けて写せる)
    bool worked = false;   // 表を作り始めたか作り進めた (ログの「作り進めたフレームの数」に入れる)
    bool finished = false; // 作り進めて、表ができあがった (ログの「作った表の数」に入れる)
};

// フレームを写す前に呼ぶ (ベアメタル版の CKernel::ShowFrame())。表ができていればそのまま、できていなければ
// crtMapAdvance() (finish なら crtMapFinish()) で作り進める。作れなかった表 (大きすぎる、区間が多すぎる) は
// 作り直さず、worked も false にする (フレームごとに「作り進めた」と数えないように)
template <class Now>
inline CrtPrepared crtMapPrepare(CrtMap& m, int preset, int srcW, int srcH, int dstW, int dstH, bool finish,
                                 uint64_t budget, Now&& now) {
    CrtPrepared r;
    r.ready = m.matches(preset, srcW, srcH, dstW, dstH);
    r.worked = !r.ready && (m.building || !m.tried(preset, srcW, srcH, dstW, dstH));
    if (!r.worked) return r;
    r.ready = finish ? crtMapFinish(m, preset, srcW, srcH, dstW, dstH)
                     : crtMapAdvance(m, preset, srcW, srcH, dstW, dstH, budget, now);
    r.finished = !m.building && m.valid;
    return r;
}

namespace crt_detail {

// 1 画素に明るさ w (0..255) を掛ける (ベクトルを使わない書き方。行の端の画素と、試験の比べる相手)
inline uint32_t weighPixel(uint32_t p, uint32_t w) {
    uint32_t out = 0xff000000u;
    for (int sh = 0; sh < 24; sh += 8) out |= ((((p >> sh) & 0xff) * (w + 1)) >> 8) << sh;
    return out;
}

// 1 画素にアパーチャーグリルを掛ける。mask のバイトが 0xff の色を 1/4 暗くする (色 - (色 >> 2))
inline uint32_t maskPixel(uint32_t p, uint32_t mask) { return p - ((p >> 2) & 0x3f3f3f3fu & mask); }

// in の n 画素に明るさを掛け、アルファを 0xff にして out に書く (in と out は同じでもよい。ずれて重なるのは不可)。
// wmap があれば画素ごとの明るさ (n バイト)、なければどの画素も w。
// aarch64 では NEON の命令で 4 画素ずつ計算する ((色 * 明るさ + 色) >> 8。weighPixel() と同じ結果)
inline void weighLine(uint32_t* out, const uint32_t* in, int n, const uint8_t* wmap, uint32_t w) {
    int x = 0;
#if defined(PIX68K_CRT_NEON)
    const uint8x16_t alpha = vreinterpretq_u8_u32(vdupq_n_u32(0xff000000u));
    if (wmap != nullptr) {
        static const uint8_t kSpread[16] = {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3};
        const uint8x16_t spread = vld1q_u8(kSpread);
        for (; x + 4 <= n; x += 4) {
            const uint8x16_t p = vld1q_u8((const uint8_t*)(in + x));
            uint32_t w4;
            __builtin_memcpy(&w4, wmap + x, 4);
            const uint8x16_t wv = vqtbl1q_u8(vreinterpretq_u8_u32(vdupq_n_u32(w4)), spread);
            const uint16x8_t lo = vaddw_u8(vmull_u8(vget_low_u8(p), vget_low_u8(wv)), vget_low_u8(p));
            const uint16x8_t hi = vaddw_high_u8(vmull_high_u8(p, wv), p);
            vst1q_u8((uint8_t*)(out + x), vorrq_u8(vcombine_u8(vshrn_n_u16(lo, 8), vshrn_n_u16(hi, 8)), alpha));
        }
    } else if (w == 255) {
        for (; x + 4 <= n; x += 4)
            vst1q_u8((uint8_t*)(out + x), vorrq_u8(vld1q_u8((const uint8_t*)(in + x)), alpha));
    } else if (w == 127) {
        // 半分 (スキャンラインの暗い行): (色 * 128) >> 8 は 1 ビットの右シフトと同じ
        for (; x + 4 <= n; x += 4)
            vst1q_u8((uint8_t*)(out + x), vorrq_u8(vshrq_n_u8(vld1q_u8((const uint8_t*)(in + x)), 1), alpha));
    } else {
        const uint8x16_t wv = vdupq_n_u8((uint8_t)w);
        for (; x + 4 <= n; x += 4) {
            const uint8x16_t p = vld1q_u8((const uint8_t*)(in + x));
            const uint16x8_t lo = vaddw_u8(vmull_u8(vget_low_u8(p), vget_low_u8(wv)), vget_low_u8(p));
            const uint16x8_t hi = vaddw_high_u8(vmull_high_u8(p, wv), p);
            vst1q_u8((uint8_t*)(out + x), vorrq_u8(vcombine_u8(vshrn_n_u16(lo, 8), vshrn_n_u16(hi, 8)), alpha));
        }
    }
#endif
    for (; x < n; x++) out[x] = weighPixel(in[x], wmap != nullptr ? wmap[x] : w);
}

// line の n 画素にアパーチャーグリルを掛ける (mask は桁ごとの、暗くする色。n 画素)
inline void maskLine(uint32_t* line, int n, const uint32_t* mask) {
    int x = 0;
#if defined(PIX68K_CRT_NEON)
    for (; x + 4 <= n; x += 4) {
        const uint8x16_t p = vld1q_u8((const uint8_t*)(line + x));
        const uint8x16_t d = vandq_u8(vshrq_n_u8(p, 2), vld1q_u8((const uint8_t*)(mask + x)));
        vst1q_u8((uint8_t*)(line + x), vsubq_u8(p, d));
    }
#endif
    for (; x < n; x++) line[x] = maskPixel(line[x], mask[x]);
}

// 2 つの画素の各色の平均 (切り捨て)
inline uint32_t avgPixel(uint32_t a, uint32_t b) { return (a & b) + (((a ^ b) & 0xfefefefeu) >> 1); }
// 左、真ん中、右を 1:2:1 で混ぜる (左右の平均と、真ん中の平均)
inline uint32_t blurPixel(uint32_t l, uint32_t c, uint32_t r) { return avgPixel(avgPixel(l, r), c); }

// in の n 画素を横ににじませて out に作る (左右の画素と 1:2:1 で混ぜる。行の端は端の画素を外にも並べたものとする)。
// mask が nullptr でなければ、にじませたあとにアパーチャーグリルも掛ける (maskLine() と同じ。1 回で済ませる)。
// in と out は重ならないこと
inline void blurLine(uint32_t* __restrict out, const uint32_t* __restrict in, int n, const uint32_t* mask = nullptr) {
    if (n <= 0) return;
    auto one = [&](int x, uint32_t l, uint32_t c, uint32_t r) {
        const uint32_t v = blurPixel(l, c, r);
        out[x] = mask != nullptr ? maskPixel(v, mask[x]) : v;
    };
    if (n == 1) {
        one(0, in[0], in[0], in[0]);
        return;
    }
    one(0, in[0], in[0], in[1]);
    int x = 1;
#if defined(PIX68K_CRT_NEON)
    // 切り捨ての平均 (vhaddq_u8) を 2 回。4 画素ずつ
    if (mask != nullptr) {
        for (; x + 4 <= n - 1; x += 4) {
            const uint8_t* p = (const uint8_t*)(in + x);
            const uint8x16_t v = vhaddq_u8(vhaddq_u8(vld1q_u8(p - 4), vld1q_u8(p + 4)), vld1q_u8(p));
            const uint8x16_t d = vandq_u8(vshrq_n_u8(v, 2), vld1q_u8((const uint8_t*)(mask + x)));
            vst1q_u8((uint8_t*)(out + x), vsubq_u8(v, d));
        }
    } else {
        for (; x + 4 <= n - 1; x += 4) {
            const uint8_t* p = (const uint8_t*)(in + x);
            vst1q_u8((uint8_t*)(out + x), vhaddq_u8(vhaddq_u8(vld1q_u8(p - 4), vld1q_u8(p + 4)), vld1q_u8(p)));
        }
    }
#endif
    for (; x < n - 1; x++) one(x, in[x - 1], in[x], in[x + 1]);
    one(n - 1, in[n - 2], in[n - 1], in[n - 1]);
}

// 表示の y 行目の画素を、元の画面から集めて out に作る (明るさはまだ掛けない)。アルファは 0xff にする
// (にじみとアパーチャーグリルはアルファを変えないので、明るさが 255 の行はそのまま書ける)
inline void gatherRow(const CrtMap& m, const uint32_t* src, int srcPitch, int y, uint32_t* __restrict out) {
    const CrtSeg* seg = m.segs + m.segStart[y];
    const CrtSeg* const end = m.segs + m.segStart[y + 1];
    if (m.kx > 0) {
        // ゆがみがなく整数倍 (区間は 1 つ)
        frame_scale_detail::scaleLine(out, src + (size_t)seg->row * srcPitch, m.srcW, m.kx);
        return;
    }
    const uint16_t* __restrict sx = m.perPixel ? m.sx + (size_t)y * m.dstW : m.sx;
    int x = 0;
    for (; seg != end; seg++) {
        const int x1 = seg->x1;
        if (seg->row < 0) {
            for (; x < x1; x++) out[x] = 0xff000000u;
            continue;
        }
        const uint32_t* __restrict in = src + (size_t)seg->row * srcPitch;
        for (; x + 4 <= x1; x += 4) {
            const uint32_t a = in[sx[x]], b = in[sx[x + 1]], c = in[sx[x + 2]], d = in[sx[x + 3]];
            out[x] = a | 0xff000000u;
            out[x + 1] = b | 0xff000000u;
            out[x + 2] = c | 0xff000000u;
            out[x + 3] = d | 0xff000000u;
        }
        for (; x < x1; x++) out[x] = in[sx[x]] | 0xff000000u;
    }
}

// 表示の y 行目の、明るさを掛ける前まで (集める、にじませる、アパーチャーグリル) を作って返す (a か b のどちらか)
inline uint32_t* renderBase(const CrtMap& m, const uint32_t* src, int srcPitch, int y, uint32_t* a, uint32_t* b) {
    gatherRow(m, src, srcPitch, y, a);
    if (m.params.blur) {
        blurLine(b, a, m.dstW, m.params.mask ? m.colMask : nullptr);
        return b;
    }
    if (m.params.mask) maskLine(a, m.dstW, m.colMask);
    return a;
}
} // namespace crt_detail

// 表示の y 行目 (m.dstW 画素。アルファは 0xff) を作って返す。src は元の画面の左上 (1 行 srcPitch 画素)。
// a と b は m.dstW + 3 画素以上のふつうのメモリ (重ならないこと)。返すのは a か b のどちらか
inline uint32_t* crtRenderRow(const CrtMap& m, const uint32_t* src, int srcPitch, int y, uint32_t* a, uint32_t* b) {
    using namespace crt_detail;
    uint32_t* const line = renderBase(m, src, srcPitch, y, a, b);
    if (m.perPixel)
        weighLine(line, line, m.dstW, m.weight + (size_t)y * m.dstW, 255);
    else if (m.rowWeight[y] != 255) // 255 はそのまま (アルファは集めたときに 0xff にしてある)
        weighLine(line, line, m.dstW, nullptr, m.rowWeight[y]);
    return line;
}

// src (1 行 srcPitch 画素) を、m の効果を掛けて fb (1 行 fbPitch バイト) に写す。s は computeFrameScale() の結果で、
// m は (s.srcW, s.srcH, s.dstW, s.dstH) で作った表。lineA と lineB は s.dstW + 3 画素以上のふつうのメモリ。
// ov、ov2、ov3、cache は presentScaled() と同じ (cache があれば、使う元の行が前に写したときと同じ行は書かない)。
// 書く画素は、どの行も crtRenderRow() と同じ。
// bezel があれば (モニタの枠。platform/bezel.h)、各行の左の bezel->left[y] 画素と、bezel->right[y] から右は、写した行の
// 代わりに枠の絵を書く (重ねる絵よりも枠が前)
struct CrtBezel {
    const uint32_t* pixels = nullptr; // 写す範囲 (s.dstX, s.dstY) の位置の枠の絵
    int pitch = 0;                    // 画素で数える
    const uint16_t* left = nullptr;   // s.dstH 行
    const uint16_t* right = nullptr;
};

template <class Store = DeviceStore>
inline void presentCrt(const uint32_t* src, int srcPitch, uint8_t* fb, size_t fbPitch, const FrameScale& s,
                       const CrtMap& m, uint32_t* lineA, uint32_t* lineB, const FrameOverlay* ov = nullptr,
                       FrameRowCache* cache = nullptr, const FrameOverlay* ov2 = nullptr,
                       const CrtBezel* bezel = nullptr, const FrameOverlay* ov3 = nullptr) {
    if (!m.matches(m.preset, s.srcW, s.srcH, s.dstW, s.dstH)) return;
    if (bezel != nullptr && bezel->pixels == nullptr) bezel = nullptr;
    using Span = frame_scale_detail::OverlaySpan;
    Span spans[frame_scale_detail::MAX_OVERLAYS];
    const FrameOverlay* const ovs[frame_scale_detail::MAX_OVERLAYS] = {ov, ov2, ov3};
    const int nspans = frame_scale_detail::overlaySpans(ovs, frame_scale_detail::MAX_OVERLAYS, s, spans);
    if (cache != nullptr && (cache->pixels == nullptr || s.srcX + s.srcW > cache->pitch || s.srcY + s.srcH > cache->rows))
        cache = nullptr;
    const bool all = cache == nullptr || !cache->valid;
    const uint32_t* const origin = src + (size_t)s.srcY * srcPitch + s.srcX;
    // 元の行が変わったか (dirty[y + 1] - dirty[y] が 1 なら変わった)
    if (!all) {
        m.dirty[0] = 0;
        for (int y = 0; y < s.srcH; y++) {
            uint32_t* prev = cache->pixels + (size_t)(s.srcY + y) * cache->pitch + s.srcX;
            const bool same = frame_scale_detail::syncRow(prev, origin + (size_t)y * srcPitch, s.srcW);
            m.dirty[y + 1] = (uint16_t)(m.dirty[y] + (same ? 0 : 1));
            if (same)
                cache->skipped++;
            else
                cache->copied++;
        }
    } else if (cache != nullptr) {
        for (int y = 0; y < s.srcH; y++)
            frame_scale_detail::copyPixels(cache->pixels + (size_t)(s.srcY + y) * cache->pitch + s.srcX,
                                           origin + (size_t)y * srcPitch, s.srcW);
        cache->copied += (uint32_t)s.srcH;
    }
    // ゆがみがないとき: 明るさを掛ける前の 1 行 (base) は、元の同じ行から作る表示の行で同じなので、使い回す
    const uint32_t* base = nullptr;
    int baseRow = -1;
    for (int y = 0; y < s.dstH; y++) {
        const int fy = s.dstY + y;
        if (!all && !cache->forced(fy, fy + 1)) {
            const int lo = m.rowMin[y], hi = m.rowMax[y];
            if (lo > hi || m.dirty[hi + 1] == m.dirty[lo]) continue;
        }
        const uint32_t* line;
        if (m.perPixel) {
            line = crtRenderRow(m, origin, srcPitch, y, lineA, lineB);
        } else {
            if (base == nullptr || m.rowMin[y] != baseRow) {
                base = crt_detail::renderBase(m, origin, srcPitch, y, lineA, lineB);
                baseRow = m.rowMin[y];
            }
            const uint32_t w = m.rowWeight[y];
            if (w == 255) {
                line = base;
            } else {
                uint32_t* const other = base == lineA ? lineB : lineA;
                crt_detail::weighLine(other, base, s.dstW, nullptr, w);
                line = other;
            }
        }
        uint32_t* dst = (uint32_t*)(fb + (size_t)fy * fbPitch) + s.dstX;
        int x = 0;
        for (int i = 0; i < nspans; i++) {
            const Span& sp = spans[i];
            if (fy < sp.img->y || fy >= sp.img->y + sp.img->h) continue;
            const int x0 = sp.x0 > x ? sp.x0 : x;
            if (x0 >= sp.x1) continue;
            const uint32_t* o = sp.img->pixels + (size_t)(fy - sp.img->y) * sp.img->w + (s.dstX + x0 - sp.img->x);
            frame_scale_detail::storeRow<Store>(dst + x, line + x, x0 - x);
            frame_scale_detail::storeRow<Store>(dst + x0, o, sp.x1 - x0);
            x = sp.x1;
        }
        if (bezel != nullptr) {
            // 左右の端は枠の絵 (重ねる絵が掛かっていても枠を書く)
            const uint32_t* b = bezel->pixels + (size_t)y * bezel->pitch;
            int l = bezel->left[y], r = bezel->right[y];
            if (l > s.dstW) l = s.dstW;
            if (r < l) r = l;
            if (r > s.dstW) r = s.dstW;
            if (x < l) x = l;
            frame_scale_detail::storeRow<Store>(dst, b, l);
            if (x < r) frame_scale_detail::storeRow<Store>(dst + x, line + x, r - x);
            frame_scale_detail::storeRow<Store>(dst + r, b + r, s.dstW - r);
            continue;
        }
        frame_scale_detail::storeRow<Store>(dst + x, line + x, s.dstW - x);
    }
    if (cache != nullptr) {
        cache->valid = true;
        cache->clearForce();
    }
}

} // namespace platform
