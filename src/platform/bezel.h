// モニタの枠 (video_filter= の cz-600de / cz-600db。#230)。ベアメタル版の CKernel::ShowFrame() が使う (ホスト版は使わない)。
//
// 枠の絵は起動したドライブ (SD カード、NVMe、USB) の /pix68k/bezel-<名前>.bin (bezelFileName())。tools/bezel_pack.py が PNG から作る。形式 (リトルエンディアン):
//   "PXBZ"、u16 幅、u16 高さ、u16 ガラスの x、y、幅、高さ (16 バイト)。そのあと 幅 x 高さ の u32 (A << 24 | 黒に重ねた RGB)。
//   A はガラスの中が 0、枠が 255、縁がその間。ガラスの矩形の中で A が 0 でない画素は、各行の左右の端から続く区間に
//   入っていること (bezelEdges() が行ごとに左右の幅だけを持つ。bezel_pack.py が確かめる)
//
// 置き方: 枠の絵がフレームバッファと同じ大きさなら、X68000 の画面をガラスの矩形に写す (bezelFrameScale()。video_scale= は
// 使わない)。curve、scanline、mask はガラスの中の絵に掛かる (platform/crt_effect.h の表は、ガラスの矩形の大きさで作る)。
// ガラスの外は枠の絵で、モードが変わったときとメニューを描き直すときだけ書く (drawBezelOutside())。毎フレームは、写す行のうち、ガラスの矩形の中で
// 枠が重なる左右の端 (角の丸みと縁) だけを枠の絵で書く (presentCrt() の CrtBezel)。縁は X68000 の画面と混ぜず、
// 枠の絵を黒に重ねた色で書く。
// 右上の数字と左上のメッセージは、背景を枠の絵を半分の明るさにしたものにする (drawOsdOnBezel()。黒の半透明に見える)。
// 消すときは枠の絵に戻す (restoreBezel())。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform/crt_preset.h"
#include "platform/frame_scale.h"

namespace platform {

static constexpr size_t kBezelHeaderBytes = 16;

struct BezelHeader {
    int w = 0, h = 0;                // 絵の大きさ (フレームバッファと同じであること)
    int gx = 0, gy = 0, gw = 0, gh = 0; // ガラスの矩形
};

// 枠の絵のファイル (起動したドライブの /pix68k/ の中)。枠なしなら nullptr
inline const char* bezelFileName(int filter) {
    const int b = videoBezel(filter);
    return b == 1 ? "bezel-cz-600de.bin" : b == 2 ? "bezel-cz-600db.bin" : nullptr;
}

// ヘッダ (16 バイト) を読む。fileSize はファイル全体の大きさ。読めなければ理由 (ログ用) を返し、読めたら nullptr
inline const char* parseBezelHeader(const uint8_t* p, uint64_t fileSize, BezelHeader& h) {
    if (fileSize < kBezelHeaderBytes || memcmp(p, "PXBZ", 4) != 0) return "not a bezel file";
    auto u16 = [&](int o) { return (int)(p[o] | p[o + 1] << 8); };
    h.w = u16(4);
    h.h = u16(6);
    h.gx = u16(8);
    h.gy = u16(10);
    h.gw = u16(12);
    h.gh = u16(14);
    if (h.w <= 0 || h.h <= 0 || h.gw < 16 || h.gh < 16 || h.gx + h.gw > h.w || h.gy + h.gh > h.h) return "bad header";
    if ((h.gx & 3) != 0) return "glass x is not a multiple of 4";
    if (fileSize != kBezelHeaderBytes + (uint64_t)h.w * h.h * 4) return "wrong size";
    return nullptr;
}

// ガラスの矩形の各行で、枠が重なる (A が 0 でない) 左の幅 left[y] と、右で重なり始める桁 right[y] を求め、絵のアルファを 0xff に
// する (フレームバッファにそのまま書けるように)。A が 0 の画素がない行は left = right = gw (行のすべてが枠)。
// 左右の区間の間に A が 0 でない画素があれば false (その画素は描けない。絵を作り直す)
inline bool bezelEdges(uint32_t* px, const BezelHeader& h, uint16_t* left, uint16_t* right) {
    bool ok = true;
    for (int y = 0; y < h.gh; y++) {
        const uint32_t* row = px + (size_t)(h.gy + y) * h.w + h.gx;
        int l = 0;
        while (l < h.gw && (row[l] >> 24) != 0) l++;
        int r = h.gw;
        while (r > l && (row[r - 1] >> 24) != 0) r--;
        for (int x = l; x < r; x++)
            if ((row[x] >> 24) != 0) ok = false;
        left[y] = (uint16_t)l;
        right[y] = (uint16_t)(l == h.gw ? h.gw : r);
    }
    for (size_t i = 0; i < (size_t)h.w * h.h; i++) px[i] |= 0xff000000u;
    return ok;
}

// 読み込んだ枠の絵 (アルファは 0xff にしてある)。pixels が nullptr なら枠なし
struct BezelImage {
    const uint32_t* pixels = nullptr;
    const uint16_t* left = nullptr;
    const uint16_t* right = nullptr;
    BezelHeader h;
    int filter = 0; // どの枠か (CRT_BEZEL_DE か CRT_BEZEL_DB)
};

// 枠を使うときの写す範囲。fw x fh の画面を sw x sh のフレームバッファに、枠の絵 b のガラスの矩形に写す。
// 使えなければ (絵がない、大きさが違う、画面がガラスより大きい、gx と gy が違う) false。
// 画面がガラスより大きいとき (768x1024 など) は縮めず、枠なしで video_scale= のとおりに写す (元の画素を落とさないため)
inline bool bezelFrameScale(const BezelImage& b, int fw, int fh, int sw, int sh, int gx, int gy, FrameScale& out) {
    if (b.pixels == nullptr || b.h.w != sw || b.h.h != sh || fw <= 0 || fh <= 0) return false;
    if (gx != 1 || gy != 1) return false;
    if (fw > b.h.gw || fh > b.h.gh) return false;
    FrameScale s;
    s.kx = s.ky = 0;
    s.srcX = s.srcY = 0;
    s.srcW = fw;
    s.srcH = fh;
    s.dstX = b.h.gx;
    s.dstY = b.h.gy;
    s.dstW = b.h.gw;
    s.dstH = b.h.gh;
    out = s;
    return true;
}

// 絵は読めたのに枠を出さない理由 (ログ用)。0 なら出さない理由はない (枠なしの設定か、絵がないか、枠を出す)。
// それ以外は 枠の番号 (1 cz-600de、2 cz-600db) << 4 | 理由 (1 GPU で拡大している、2 画面がガラスより大きい)。
// 合成するコアが毎フレーム置き、コア 0 が BezelSkipLog で変わったときだけログに出す
enum : int { BEZEL_SKIP_GPU = 1, BEZEL_SKIP_LARGE = 2 };
inline int bezelSkipCode(int filter, bool loaded, bool shown, bool gpuScaled) {
    const int id = videoBezel(filter);
    if (id == 0 || !loaded || shown) return 0;
    return id << 4 | (gpuScaled ? BEZEL_SKIP_GPU : BEZEL_SKIP_LARGE);
}

// 枠を出さない理由をログに出すか (コア 0)。理由が変わったときだけ true (0 に戻ったときは出さない)
struct BezelSkipLog {
    int logged = 0;
    bool update(int code) {
        if (code == logged) return false;
        logged = code;
        return code != 0;
    }
};

// s が枠のガラスの矩形か
inline bool isBezelScale(const BezelImage& b, const FrameScale& s) {
    return b.pixels != nullptr && s.kx == 0 && s.dstX == b.h.gx && s.dstY == b.h.gy && s.dstW == b.h.gw &&
           s.dstH == b.h.gh;
}

// ガラスの外と、ガラスの矩形の中で枠が重なる左右の端だけを書く (ガラスの中の X68000 の画面は残す。メニューを描き直すとき)
template <class Store = DeviceStore>
inline void drawBezelOutside(uint8_t* fb, size_t fbPitch, const BezelImage& b) {
    for (int y = 0; y < b.h.h; y++) {
        uint32_t* row = (uint32_t*)(fb + (size_t)y * fbPitch);
        const uint32_t* src = b.pixels + (size_t)y * b.h.w;
        if (y < b.h.gy || y >= b.h.gy + b.h.gh) {
            frame_scale_detail::storeRow<Store>(row, src, b.h.w);
            continue;
        }
        const int l = b.h.gx + b.left[y - b.h.gy], r = b.h.gx + b.right[y - b.h.gy];
        frame_scale_detail::storeRow<Store>(row, src, l);
        frame_scale_detail::storeRow<Store>(row + r, src + r, b.h.w - r);
    }
}

// r (フレームバッファの中の矩形) を枠の絵に戻す (右上の数字やメッセージを消す)
template <class Store = DeviceStore>
inline void restoreBezel(uint8_t* fb, size_t fbPitch, const BezelImage& b, int x, int y, int w, int h) {
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > b.h.w) w = b.h.w - x;
    if (y + h > b.h.h) h = b.h.h - y;
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < h; i++)
        frame_scale_detail::storeRow<Store>((uint32_t*)(fb + (size_t)(y + i) * fbPitch) + x,
                                            b.pixels + (size_t)(y + i) * b.h.w + x, w);
}

// 右上の数字やメッセージの絵 (w x h 画素。背景は bg) を、背景を枠の絵の半分の明るさにして r に書く。
// ガラスの矩形の中で枠が重なる左右の端 (角の丸みと縁) には描かず、枠の絵のままにする (presentCrt() が変わった行だけ
// そこを枠の絵で書くので、描くと行ごとに字が残ったり消えたりする)
template <class Store = DeviceStore>
inline void drawOsdOnBezel(uint8_t* fb, size_t fbPitch, const BezelImage& b, int x, int y, int w, int h,
                           const uint32_t* image, uint32_t bg, uint32_t* line) {
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > b.h.w || y + h > b.h.h) return;
    for (int i = 0; i < h; i++) {
        const uint32_t* src = image + (size_t)i * w;
        const uint32_t* under = b.pixels + (size_t)(y + i) * b.h.w + x;
        for (int k = 0; k < w; k++) line[k] = src[k] == bg ? (0xff000000u | ((under[k] >> 1) & 0x7f7f7fu)) : src[k];
        const int gyRow = y + i - b.h.gy;
        if (gyRow >= 0 && gyRow < b.h.gh) {
            const int l = b.h.gx + b.left[gyRow], r = b.h.gx + b.right[gyRow];
            for (int k = 0; k < w; k++) {
                const int fx = x + k;
                if (fx >= b.h.gx && fx < b.h.gx + b.h.gw && (fx < l || fx >= r)) line[k] = under[k];
            }
        }
        frame_scale_detail::storeRow<Store>((uint32_t*)(fb + (size_t)(y + i) * fbPitch) + x, line, w);
    }
}

} // namespace platform
