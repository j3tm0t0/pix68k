// X68000 の画面をフレームバッファに写すときの拡大 (整数倍、最近傍)。ベアメタル版の CKernel::ShowFrame()
// (baremetal/machine_run.cpp) が使う。pix68k.cfg の scale=auto (既定) で拡大し、scale=off で拡大しない。
//
// 倍率の決め方 (computeFrameScale()):
//   768x512 の画面を縦横同じ倍率 n で拡大した大きさ (768n x 512n) を「枠」とする。n はフレームバッファに入る最大の
//   整数で、2 までにする (3840x2160 などで 3 倍、4 倍にすると、写す量が 9 倍、16 倍になるため)。
//   どの画面モードも、横と縦それぞれ枠に入る最大の整数倍にする。768x512 は 2x2、512x512 は 3x2、256x256 は 6x4 で、
//   1920x1080 ではどれも 1536x1024 になる (X68000 のモニタで、画面モードを変えても絵の大きさがほぼ同じなのに近い)。
//   割り切れない大きさ (352x240 など) は切り捨てるので、枠より少し小さくなる。
//   枠はフレームバッファにまるごと入る大きさにする。ファームウェアのオーバースキャンでフレームバッファが少し
//   小さくなったとき (1920x1080 で 1824x984 など) も、2 倍にしてはみ出す上下を切り出すことはせず、1 倍にする。
//   984 ラインに 1024 ラインを出すと、X68000 の 20 ラインずつが上下で切れ、Human68k の画面のいちばん下の行
//   (ファンクションキーの表示) やゲームの上下の表示が見えなくなるため。オーバースキャンは config.txt の
//   disable_overscan=1 でやめられる (起動時のログに出す)。
//   n が 0 (フレームバッファが 768x512 より小さい) か、scale=off なら拡大しない。拡大しないときと、画面のほうが
//   フレームバッファより大きいときは、今までどおり真ん中を切り出す。
//
// 写し方 (presentScaled()):
//   フレームバッファは Circle では Device のメモリとして写像されている (キャッシュを通らず、書き込みはまとめられない)。
//   読み戻すと遅いので、拡大した 1 行をふつうのメモリ (line) に作ってから、縦の倍率の回数だけフレームバッファに書く。
//   書くときは 16 バイトにそろったところから、aarch64 では 64 バイトずつ (stp q を 2 回)、残りを 16 バイトずつ書く (Device のメモリは境界にそろわない書き込みで例外になる
//   ので、そろうまでは 4 バイトずつ)。左端は 4 画素 (16 バイト) の倍数にそろえる。
//
//   FrameRowCache を渡すと、前に写したときと同じ行は書かない (ふつうのメモリにある前の画面の写しと比べる)。
//
// スキャンライン (pix68k.cfg の scanline=on、既定は off): 縦の倍率が 2 以上なら、同じ行を書く ky 回のうち最後の 1 回だけ、
// 拡大した 1 行 (line) をふつうのメモリの中で半分の明るさにしてから書く (darkenLine()。ブラウン管の走査線の間の
// 暗い筋のように見せる)。フレームバッファに書く量は変わらず、増えるのは line を暗くする計算だけ。縦の倍率が 1 のとき
// (scale=off、1024 ラインの画面、pix68k_gpu_scale でフレームバッファの中の倍率が 1 になるとき) は何もしない。
// 重ねる絵 (FrameOverlay。画面の隅の数字) は暗くしない。
//
// GPU の拡大 (カーネルのオプション pix68k_gpu_scale): フレームバッファを表示より小さくし、表示への拡大をファームウェアに
// 任せると、ここで書く量が減る。computeFrameScale() の gx, gy を参照。
//
// 枠の外 (fillOutside()) は不透明の黒 (0xff000000) で塗る。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace platform {

struct FrameScale {
    int kx = 1, ky = 1;                         // 横と縦の倍率
    int srcX = 0, srcY = 0, srcW = 0, srcH = 0; // 写す X68000 の画面の範囲
    int dstX = 0, dstY = 0, dstW = 0, dstH = 0; // フレームバッファの中の位置と大きさ (dstW = srcW * kx)

    bool operator==(const FrameScale& o) const {
        return kx == o.kx && ky == o.ky && srcX == o.srcX && srcY == o.srcY && srcW == o.srcW && srcH == o.srcH &&
               dstX == o.dstX && dstY == o.dstY && dstW == o.dstW && dstH == o.dstH;
    }
    bool operator!=(const FrameScale& o) const { return !(*this == o); }
};

static constexpr int kScaleBoxW = 768, kScaleBoxH = 512; // 枠のもとの大きさ
static constexpr int kScaleMaxBox = 2;                   // 枠の倍率の上限

// fw x fh の画面を sw x sh のフレームバッファに写すときの倍率と位置。dstW は maxLine 以下にする
// (presentScaled() の line は dstW + 3 画素要る)。
// gx, gy は GPU がフレームバッファを表示に拡大する倍率 (カーネルのオプション pix68k_gpu_scale。ふだんは 1)。
// 枠と倍率は表示の大きさ (sw * gx x sh * gy) で決め、フレームバッファの中の倍率はそれを gx, gy で割ったもの
// (割り切れなければ切り捨て、1 より小さくはしない) にする。1920x1080 の表示に 960x540 のフレームバッファ
// (gx = gy = 2) なら、768x512 は 1x1、256x256 は 3x2 で写し、GPU が 2 倍にして今と同じ 1536x1024 になる。
// 512x512 は表示で 3 倍のところが 1 倍になり、1024x1024 に出る
inline FrameScale computeFrameScale(int fw, int fh, int sw, int sh, bool enable, int maxLine, int gx = 1,
                                    int gy = 1) {
    FrameScale s;
    if (fw <= 0 || fh <= 0 || sw <= 0 || sh <= 0) return s;
    if (gx < 1) gx = 1;
    if (gy < 1) gy = 1;
    if (enable) {
        const int dw = sw * gx, dh = sh * gy;
        int n = dw / kScaleBoxW < dh / kScaleBoxH ? dw / kScaleBoxW : dh / kScaleBoxH;
        if (n > kScaleMaxBox) n = kScaleMaxBox;
        if (n >= 1) {
            s.kx = kScaleBoxW * n / fw / gx;
            s.ky = kScaleBoxH * n / fh / gy;
            if (s.kx < 1) s.kx = 1;
            if (s.ky < 1) s.ky = 1;
        }
    }
    int lineW = sw < maxLine ? sw : maxLine;
    s.srcW = fw * s.kx <= lineW ? fw : lineW / s.kx;
    s.srcH = fh * s.ky <= sh ? fh : sh / s.ky;
    s.srcX = (fw - s.srcW) / 2;
    s.srcY = (fh - s.srcH) / 2;
    s.dstW = s.srcW * s.kx;
    s.dstH = s.srcH * s.ky;
    s.dstX = ((sw - s.dstW) / 2) & ~3; // 16 バイトにそろえる
    s.dstY = (sh - s.dstH) / 2;
    return s;
}

namespace frame_scale_detail {
typedef uint32_t V4 __attribute__((vector_size(16)));
} // namespace frame_scale_detail

// フレームバッファへの書き込み。テストは同じ形の型を渡して、書き込む番地と大きさを調べる (frame_scale_test.cpp)
struct DeviceStore {
    typedef frame_scale_detail::V4 V4;
    // 4 バイト
    static void store4(uint32_t* p, uint32_t v) { *(volatile uint32_t*)p = v; }
    // 16 バイト (p は 16 バイトにそろっていること)
    static void store16(uint32_t* p, V4 v) { *(volatile V4*)p = v; }
    // 64 バイト (p は 16 バイトにそろっていること)。aarch64 では stp q を 2 回 (1 回の書き込みを大きくする)
    static void store64(uint32_t* p, V4 a, V4 b, V4 c, V4 d) {
#if defined(__aarch64__)
        asm volatile("stp %q1, %q2, [%0]\n\tstp %q3, %q4, [%0, #32]" : : "r"(p), "w"(a), "w"(b), "w"(c), "w"(d) : "memory");
#else
        store16(p, a);
        store16(p + 4, b);
        store16(p + 8, c);
        store16(p + 12, d);
#endif
    }
};

namespace frame_scale_detail {
// フレームバッファの 1 行の n 画素を書く。dst は 4 バイトにそろっていればよい。src はふつうのメモリ。
// 16 バイトにそろうまでは 4 バイトずつ書く (Device のメモリは、そろわない 16 バイトの書き込みで例外になる)
template <class Store> inline void storeRow(uint32_t* dst, const uint32_t* src, int n) {
    int i = 0;
    while (i < n && ((uintptr_t)(dst + i) & 15) != 0) {
        Store::store4(dst + i, src[i]);
        i++;
    }
    for (; i + 16 <= n; i += 16) {
        V4 a, b, c, d;
        __builtin_memcpy(&a, src + i, 16);
        __builtin_memcpy(&b, src + i + 4, 16);
        __builtin_memcpy(&c, src + i + 8, 16);
        __builtin_memcpy(&d, src + i + 12, 16);
        Store::store64(dst + i, a, b, c, d);
    }
    for (; i + 4 <= n; i += 4) {
        V4 v;
        __builtin_memcpy(&v, src + i, 16);
        Store::store16(dst + i, v);
    }
    for (; i < n; i++) Store::store4(dst + i, src[i]);
}

// フレームバッファの 1 行の n 画素を color で塗る
template <class Store> inline void fillRow(uint32_t* dst, uint32_t color, int n) {
    int i = 0;
    while (i < n && ((uintptr_t)(dst + i) & 15) != 0) {
        Store::store4(dst + i, color);
        i++;
    }
    const V4 v = {color, color, color, color};
    for (; i + 16 <= n; i += 16) Store::store64(dst + i, v, v, v, v);
    for (; i + 4 <= n; i += 4) Store::store16(dst + i, v);
    for (; i < n; i++) Store::store4(dst + i, color);
}
} // namespace frame_scale_detail

namespace frame_scale_detail {
// in の n 画素を横に kx 倍して line に作る。画素は不透明にする (アルファを 0xff にする)。line は 16 バイトに
// そろっていなくてもよい。3 倍以上では、行の終わりから 3 画素先まで書くことがある (line は n * kx + 3 画素以上)。
// よく使う 1 倍と 2 倍は 8 画素ずつ (16 バイトの読み込み 2 回)、2 倍は zip で並べて 16 バイトの書き込み 4 回にする
inline void scaleLine(uint32_t* __restrict line, const uint32_t* __restrict in, int n, int kx) {
    const V4 alpha = {0xff000000u, 0xff000000u, 0xff000000u, 0xff000000u};
    int x = 0;
    if (kx == 1) {
        for (; x + 8 <= n; x += 8) {
            V4 a, b;
            __builtin_memcpy(&a, in + x, 16);
            __builtin_memcpy(&b, in + x + 4, 16);
            a |= alpha;
            b |= alpha;
            __builtin_memcpy(line + x, &a, 16);
            __builtin_memcpy(line + x + 4, &b, 16);
        }
        for (; x < n; x++) line[x] = in[x] | 0xff000000u;
    } else if (kx == 2) {
        uint32_t* out = line;
        for (; x + 8 <= n; x += 8, out += 16) {
            V4 a, b;
            __builtin_memcpy(&a, in + x, 16);
            __builtin_memcpy(&b, in + x + 4, 16);
            a |= alpha;
            b |= alpha;
            const V4 a0 = __builtin_shufflevector(a, a, 0, 0, 1, 1), a1 = __builtin_shufflevector(a, a, 2, 2, 3, 3);
            const V4 b0 = __builtin_shufflevector(b, b, 0, 0, 1, 1), b1 = __builtin_shufflevector(b, b, 2, 2, 3, 3);
            __builtin_memcpy(out, &a0, 16);
            __builtin_memcpy(out + 4, &a1, 16);
            __builtin_memcpy(out + 8, &b0, 16);
            __builtin_memcpy(out + 12, &b1, 16);
        }
        for (; x < n; x++, out += 2) out[0] = out[1] = in[x] | 0xff000000u;
    } else {
        uint32_t* out = line;
        // 3 倍 (512x512)、4 倍 (384x256)、6 倍 (256x256) は 4 画素ずつ並べ替えて 16 バイトずつ書く
        if (kx == 3) {
            for (; x + 4 <= n; x += 4, out += 12) {
                V4 a;
                __builtin_memcpy(&a, in + x, 16);
                a |= alpha;
                const V4 o0 = __builtin_shufflevector(a, a, 0, 0, 0, 1), o1 = __builtin_shufflevector(a, a, 1, 1, 2, 2),
                         o2 = __builtin_shufflevector(a, a, 2, 3, 3, 3);
                __builtin_memcpy(out, &o0, 16);
                __builtin_memcpy(out + 4, &o1, 16);
                __builtin_memcpy(out + 8, &o2, 16);
            }
        } else if (kx == 4) {
            for (; x + 4 <= n; x += 4, out += 16) {
                V4 a;
                __builtin_memcpy(&a, in + x, 16);
                a |= alpha;
                const V4 o0 = __builtin_shufflevector(a, a, 0, 0, 0, 0), o1 = __builtin_shufflevector(a, a, 1, 1, 1, 1),
                         o2 = __builtin_shufflevector(a, a, 2, 2, 2, 2), o3 = __builtin_shufflevector(a, a, 3, 3, 3, 3);
                __builtin_memcpy(out, &o0, 16);
                __builtin_memcpy(out + 4, &o1, 16);
                __builtin_memcpy(out + 8, &o2, 16);
                __builtin_memcpy(out + 12, &o3, 16);
            }
        } else if (kx == 6) {
            for (; x + 4 <= n; x += 4, out += 24) {
                V4 a;
                __builtin_memcpy(&a, in + x, 16);
                a |= alpha;
                const V4 o0 = __builtin_shufflevector(a, a, 0, 0, 0, 0), o1 = __builtin_shufflevector(a, a, 0, 0, 1, 1),
                         o2 = __builtin_shufflevector(a, a, 1, 1, 1, 1), o3 = __builtin_shufflevector(a, a, 2, 2, 2, 2),
                         o4 = __builtin_shufflevector(a, a, 2, 2, 3, 3), o5 = __builtin_shufflevector(a, a, 3, 3, 3, 3);
                __builtin_memcpy(out, &o0, 16);
                __builtin_memcpy(out + 4, &o1, 16);
                __builtin_memcpy(out + 8, &o2, 16);
                __builtin_memcpy(out + 12, &o3, 16);
                __builtin_memcpy(out + 16, &o4, 16);
                __builtin_memcpy(out + 20, &o5, 16);
            }
        }
        // ほかの倍率と残りの画素は、同じ画素を 4 つ並べたものを 16 バイトずつ書く (最後は次の画素で上書きする)
        for (; x < n; x++, out += kx) {
            const uint32_t p = in[x] | 0xff000000u;
            const V4 v = {p, p, p, p};
            for (int k = 0; k < kx; k += 4) __builtin_memcpy(out + k, &v, 16);
        }
    }
}

// line の n 画素を半分の明るさにする (各色を 1 ビット右へずらす)。アルファは 0xff に戻す (アルファが 0 だと黒く出る
// ファームウェアがあるため)。line は 16 バイトにそろっていなくてもよい
inline void darkenLine(uint32_t* line, int n) {
    const V4 alpha = {0xff000000u, 0xff000000u, 0xff000000u, 0xff000000u};
    const V4 mask = {0x007f7f7fu, 0x007f7f7fu, 0x007f7f7fu, 0x007f7f7fu};
    int x = 0;
    for (; x + 8 <= n; x += 8) {
        V4 a, b;
        __builtin_memcpy(&a, line + x, 16);
        __builtin_memcpy(&b, line + x + 4, 16);
        a = ((a >> 1) & mask) | alpha;
        b = ((b >> 1) & mask) | alpha;
        __builtin_memcpy(line + x, &a, 16);
        __builtin_memcpy(line + x + 4, &b, 16);
    }
    for (; x < n; x++) line[x] = ((line[x] >> 1) & 0x007f7f7fu) | 0xff000000u;
}
} // namespace frame_scale_detail

// 写すときに X68000 の画面に重ねる絵 (画面の右上の数字 (platform/osd.h) と左上のメッセージ (platform/osd_message.h))。pixels は w x h 画素 (1 行 w 画素)
// で、ふつうのメモリ。x は 4 画素にそろえる。X68000 の画面の中身 (src) は変えず、フレームバッファに書く 1 行の
// うち重なる部分だけをこの絵にする (写したあとに上から描くと、写してから描くまでの間、重ねる場所に X68000 の画面が
// 出て、ちらつくため)
struct FrameOverlay {
    const uint32_t* pixels = nullptr;
    int x = 0, y = 0, w = 0, h = 0; // フレームバッファの中の位置と大きさ
};

// 前に写した X68000 の画面の写し (presentScaled() が、変わっていない行をフレームバッファに書かずに済ませるため)。
// 写す役のコアだけが触る。pixels は pitch 画素 x rows 行で、src と同じ位置に、前に写した画素 (アルファを足す前) を置く。
// valid が false なら、すべての行を写して写しを作り直す (フレームバッファをほかの処理が書いたとき、倍率が変わったとき)。
// フレームバッファの forceY0 行目から forceY1 行目の手前までに重なる行は、変わっていなくても写す (画面の隅の数字を
// 描いた、消したなど)。copied と skipped は、写した行と省いた行の数 (X68000 の画面の行で数える。足していく)。
// scanline は前に写したときのスキャンライン。違うスキャンラインで写すときは、すべての行を写す (暗くする行が変わるため)
struct FrameRowCache {
    uint32_t* pixels = nullptr;
    int pitch = 0, rows = 0;
    bool valid = false;
    bool scanline = false;
    int forceY0 = 0, forceY1 = 0;
    uint32_t copied = 0, skipped = 0;
};

namespace frame_scale_detail {
// ふつうのメモリどうしで n 画素を写す。長さが決まっていない __builtin_memcpy は Circle の memcpy() を呼び、
// 8 バイトにそろっていないところや短いところを 1 バイトずつ写すので、16 バイトずつ写す
inline void copyPixels(uint32_t* __restrict dst, const uint32_t* __restrict src, int n) {
    int x = 0;
    for (; x + 8 <= n; x += 8) {
        V4 a, b;
        __builtin_memcpy(&a, src + x, 16);
        __builtin_memcpy(&b, src + x + 4, 16);
        __builtin_memcpy(dst + x, &a, 16);
        __builtin_memcpy(dst + x + 4, &b, 16);
    }
    for (; x < n; x++) dst[x] = src[x];
}

// prev と in の n 画素を比べる。同じなら true。違えば、違ったところから後ろを prev に写して false を返す
// (prev は違ったところより前は in と同じなので、写したあとは in と同じになる)
inline bool syncRow(uint32_t* __restrict prev, const uint32_t* __restrict in, int n) {
    int x = 0;
    bool diff = false;
    for (; x + 8 <= n; x += 8) {
        V4 a, b, c, d;
        __builtin_memcpy(&a, in + x, 16);
        __builtin_memcpy(&b, in + x + 4, 16);
        __builtin_memcpy(&c, prev + x, 16);
        __builtin_memcpy(&d, prev + x + 4, 16);
        const V4 t = (a ^ c) | (b ^ d);
        uint64_t u[2];
        __builtin_memcpy(u, &t, 16);
        if ((u[0] | u[1]) != 0) {
            diff = true;
            break;
        }
    }
    if (!diff) {
        for (; x < n; x++)
            if (in[x] != prev[x]) {
                diff = true;
                break;
            }
        if (!diff) return true;
    }
    copyPixels(prev + x, in + x, n - x);
    return false;
}
} // namespace frame_scale_detail

// src (1 行 srcPitch 画素) を s のとおりに拡大して fb (1 行 fbPitch バイト) に写す。画素は不透明にする
// (アルファが 0 だと黒く出るファームウェアがあるため)。line は s.dstW + 3 画素以上で、ふつうのメモリ。
// ov (と ov2) があれば、重なる部分はその絵を書く。scanline が true で縦の倍率が 2 以上なら、各行の最後の 1 回を暗くする
// (上の「スキャンライン」。line を書き換える)。
// cache があれば、前に写したときと同じ行はフレームバッファに書かない (FrameRowCache)。比べるのはふつうのメモリどうし
// なので、Device のメモリのフレームバッファに書くより速い (画面がほとんど変わらない起動の間やディスクを読む間に、
// 写す時間を減らす)
template <class Store = DeviceStore>
inline void presentScaled(const uint32_t* src, int srcPitch, uint8_t* fb, size_t fbPitch, const FrameScale& s,
                          uint32_t* line, const FrameOverlay* ov = nullptr, FrameRowCache* cache = nullptr,
                          bool scanline = false, const FrameOverlay* ov2 = nullptr) {
    // 重ねる絵 (ov と ov2) の重なる横の範囲 (s.dstX からの位置)。重ならないものは数えない。左にあるものを先にする
    struct Span {
        const FrameOverlay* img;
        int x0, x1;
    } spans[2];
    int nspans = 0;
    const FrameOverlay* const ovs[2] = {ov, ov2};
    for (const FrameOverlay* o : ovs) {
        if (o == nullptr || o->pixels == nullptr || o->w <= 0 || o->h <= 0) continue;
        int x0 = o->x > s.dstX ? o->x - s.dstX : 0;
        int x1 = o->x + o->w - s.dstX;
        if (x1 > s.dstW) x1 = s.dstW;
        if (x0 >= x1) continue;
        spans[nspans++] = Span{o, x0, x1};
    }
    if (nspans == 2 && spans[1].x0 < spans[0].x0) {
        const Span t = spans[0];
        spans[0] = spans[1];
        spans[1] = t;
    }
    // 写しが s の写す範囲を収められなければ使わない
    if (cache != nullptr && (cache->pixels == nullptr || s.srcX + s.srcW > cache->pitch || s.srcY + s.srcH > cache->rows))
        cache = nullptr;
    const bool all = cache == nullptr || !cache->valid || cache->scanline != scanline;
    // 倍率と幅はローカルに写す (line への書き込みが s を書き換えうると見なされ、画素ごとに読み直されないように)
    const int kx = s.kx, ky = s.ky, srcW = s.srcW;
    // 暗くする回 (k == dimK)。暗くしないときは ky (どの k とも一致しない)
    const int dimK = scanline && ky >= 2 ? ky - 1 : ky;
    for (int y = 0; y < s.srcH; y++) {
        const uint32_t* in = src + (size_t)(s.srcY + y) * srcPitch + s.srcX;
        if (cache != nullptr) {
            uint32_t* prev = cache->pixels + (size_t)(s.srcY + y) * cache->pitch + s.srcX;
            const int fy0 = s.dstY + y * ky;
            const bool forced = all || (fy0 < cache->forceY1 && fy0 + ky > cache->forceY0);
            if (forced) {
                frame_scale_detail::copyPixels(prev, in, srcW);
            } else if (frame_scale_detail::syncRow(prev, in, srcW)) {
                cache->skipped++;
                continue;
            }
            cache->copied++;
        }
        frame_scale_detail::scaleLine(line, in, srcW, kx);
        for (int k = 0; k < ky; k++) {
            const int fy = s.dstY + y * ky + k;
            uint32_t* dst = (uint32_t*)(fb + (size_t)fy * fbPitch) + s.dstX;
            if (k == dimK) frame_scale_detail::darkenLine(line, s.dstW);
            // 重なる行は、X68000 の画面と重ねる絵に分けて、左から書く (2 つの絵が横に重なれば、左の絵を優先する)
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
            frame_scale_detail::storeRow<Store>(dst + x, line + x, s.dstW - x);
        }
    }
    if (cache != nullptr) {
        cache->valid = true;
        cache->scanline = scanline;
        cache->forceY0 = cache->forceY1 = 0;
    }
}

// sw x sh のフレームバッファのうち、s の写す範囲の外を color で塗る
template <class Store = DeviceStore>
inline void fillOutside(uint8_t* fb, size_t fbPitch, int sw, int sh, const FrameScale& s, uint32_t color) {
    for (int y = 0; y < sh; y++) {
        uint32_t* row = (uint32_t*)(fb + (size_t)y * fbPitch);
        if (y < s.dstY || y >= s.dstY + s.dstH) {
            frame_scale_detail::fillRow<Store>(row, color, sw);
        } else {
            frame_scale_detail::fillRow<Store>(row, color, s.dstX);
            const int right = s.dstX + s.dstW;
            if (right < sw) frame_scale_detail::fillRow<Store>(row + right, color, sw - right);
        }
    }
}

} // namespace platform
