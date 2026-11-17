// 画面の合成 (Renderer::renderFrame) にかかる時間を測る (テストではない)。
// 場面ごとに何フレームか描き、1 フレームの時間 (ミリ秒) の最小と中央を出す。
//
//   video_bench [-n フレーム数] [場面...]   (場面を省くと全部)
//
// 場面 (名前で選ぶ):
//   fill768      下限の目安。768x512 の画素を 1 色で埋めるだけ (Renderer は使わない)
//   text768      768x512 (31kHz)。テキスト画面だけを出す
//   mix512       512x512 (31kHz)。テキスト、16 色のグラフィック 4 ページ、スプライト 128 個、BG0
//   mix512_slow  mix512 で、グラフィックのページごとにスクロールを変え、テキストの X スクロールを 8 の倍数から外したもの
//   g256_text    512x512。256 色のグラフィック 2 ページとテキスト
//   g65536_text  512x512。65536 色のグラフィックとテキスト
//
// VRAM、パレット、スプライトのデータは乱数で埋める。テキストパレットの 0 番だけは 0 (透明) にして、
// テキストの奥の画面も合成の対象にする。乱数は毎回同じ列を使う。
//
// Pi (Linux) ではベアメタルと同じ -O2 で次のようにビルドする:
//   g++ -O2 -mcpu=cortex-a53 -std=c++17 -Isrc tests/bench/video_bench.cpp \
//       src/x68k/video.cpp src/x68k/gvram.cpp src/x68k/render.cpp -o video_bench
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "x68k/render.h"
#include "x68k/video.h"

namespace {

using x68k::Renderer;
using x68k::Video;

double seconds() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

// 乱数 (splitmix32)。場面を作るたびに種から始め直す
struct Random {
    uint32_t state;
    explicit Random(uint32_t seed) : state(seed) {}
    uint32_t next() {
        uint32_t z = (state += 0x9e3779b9u);
        z = (z ^ (z >> 16)) * 0x85ebca6bu;
        z = (z ^ (z >> 13)) * 0xc2b2ae35u;
        return z ^ (z >> 16);
    }
};

// 16 ビットのレジスタを、上位、下位の順にバイトで書く
void crtcReg(Video& v, int n, uint16_t value) {
    v.crtcWrite((uint32_t)n * 2, (uint8_t)(value >> 8));
    v.crtcWrite((uint32_t)n * 2 + 1, (uint8_t)value);
}
void paletteWord(Video& v, int n, uint16_t value) {
    v.paletteWrite((uint32_t)n * 2, (uint8_t)(value >> 8));
    v.paletteWrite((uint32_t)n * 2 + 1, (uint8_t)value);
}
// ビデオコントローラ R0..R2 ($E82400 + n x $100)
void vcReg(Video& v, int n, uint16_t value) {
    v.paletteWrite(0x400 + (uint32_t)n * 0x100, (uint8_t)(value >> 8));
    v.paletteWrite(0x401 + (uint32_t)n * 0x100, (uint8_t)value);
}
void spriteWord(Video& v, uint32_t offset, uint16_t value) {
    v.spriteWrite(offset, (uint8_t)(value >> 8));
    v.spriteWrite(offset + 1, (uint8_t)value);
}

// 512x512 (31kHz) の表示タイミング。Video::reset() の 768x512 の値から、垂直はそのまま使い、
// 水平は 1 ラスタの長さが同じになるように決めた: 768 ドットはドットクロック 69.552MHz / 2 で 1 ラスタ 138 x 8 ドット、
// 512 ドットは 69.552MHz / 3 なので 92 x 8 ドット (R00 = 91)。表示は 64 x 8 ドットで、R02 から始める
void timing512(Video& v, uint16_t r20) {
    const uint16_t hTotal = 91, hStart = 18, vStart = 0x28, vEnd = 0x228;
    crtcReg(v, 0, hTotal);
    crtcReg(v, 2, hStart);
    crtcReg(v, 3, hStart + 64);
    crtcReg(v, 6, vStart);
    crtcReg(v, 7, vEnd);
    crtcReg(v, 20, r20);
}

void randomPalette(Video& v, Random& rnd) {
    for (int n = 0; n < 512; n++) paletteWord(v, n, (uint16_t)rnd.next());
    paletteWord(v, 256, 0x0000); // テキストパレット 0 は透明
}

void randomTextVram(Video& v, Random& rnd) {
    for (uint32_t a = 0; a < Video::TVRAM_SIZE; a += 4) {
        const uint32_t r = rnd.next();
        for (int b = 0; b < 4; b++) v.tvramWrite(a + (uint32_t)b, (uint8_t)(r >> (8 * b)));
    }
}

// グラフィック VRAM ($C00000-$DFFFFF) を、今の R20 の色数での CPU からの見え方のまま乱数で埋める
void randomGraphicVram(Video& v, Random& rnd) {
    for (uint32_t a = 0; a < 0x200000; a += 4) {
        const uint32_t r = rnd.next();
        for (int b = 0; b < 4; b++) v.gvramWrite(a + (uint32_t)b, (uint8_t)(r >> (8 * b)));
    }
}

// スプライト 128 個と BG0 (Inside X68000 のスプライトの章。render.cpp の先頭の説明も同じ)
//   $EB0000 + 8n: X 座標、Y 座標、パターンと色と反転、優先度 (下位 2 ビット。0 は表示しない)
//   $EB0808: BG コントロール。bit9 が表示 ON、bit0 が BG0 の ON (データエリアは $EBC000)
//   $EB080C / $EB080E: 水平 / 垂直の表示位置。CRTC の R02 + 4 と R06 に合わせる
//   $EB0810: 解像度。bit1-0 が 0 でなければ BG は 16x16 ドットで BG0 だけ
//   $EB8000-$EBFFFF: パターン (PCG) と BG のデータ
void spritesAndBg(Video& v, Random& rnd) {
    for (uint32_t a = 0x8000; a < Video::SPRITE_SIZE; a++) v.spriteWrite(a, (uint8_t)rnd.next());
    for (uint32_t n = 0; n < 128; n++) {
        const uint32_t base = n * 8;
        spriteWord(v, base + 0, (uint16_t)(rnd.next() % 544)); // 16 で画面の左端。はみ出すものも作る
        spriteWord(v, base + 2, (uint16_t)(rnd.next() % 544));
        spriteWord(v, base + 4, (uint16_t)(rnd.next() & 0xcfff));
        spriteWord(v, base + 6, (uint16_t)(1 + rnd.next() % 3));
    }
    spriteWord(v, 0x80c, (uint16_t)(v.horizontalDisplayStart() + 4));
    spriteWord(v, 0x80e, (uint16_t)v.verticalDisplayStart());
    spriteWord(v, 0x810, 0x0015);
    spriteWord(v, 0x808, 0x0201);
}

// ビデオコントローラ R1: スプライト 1 番目、テキスト 0 番目 (一番手前)、グラフィック 2 番目。
// グラフィックのページは 0 番を一番手前に、番号の順に並べる
const uint16_t kPriority = (1 << 12) | (0 << 10) | (2 << 8) | (3 << 6) | (2 << 4) | (1 << 2) | 0;
// R2 の表示 ON のビット
const uint16_t kShowSprite = 0x0040, kShowText = 0x0020, kShowGraphic = 0x000f;

void setupText768(Video& v) {
    Random rnd(1);
    // reset() の CRTC は 768x512 (31kHz)
    randomPalette(v, rnd);
    randomTextVram(v, rnd);
    vcReg(v, 1, kPriority);
    vcReg(v, 2, kShowText);
}

void setupMix512(Video& v) {
    Random rnd(2);
    timing512(v, 0x0015); // 31kHz、512 ライン、512 ドット、16 色
    randomPalette(v, rnd);
    randomTextVram(v, rnd);
    randomGraphicVram(v, rnd);
    spritesAndBg(v, rnd);
    vcReg(v, 0, 0x0000);
    vcReg(v, 1, kPriority);
    vcReg(v, 2, kShowSprite | kShowText | kShowGraphic);
}

void setupMix512Slow(Video& v) {
    setupMix512(v);
    crtcReg(v, 10, 3);   // テキストの X スクロール (8 の倍数でない)
    crtcReg(v, 11, 21);  // テキストの Y スクロール
    for (int page = 0; page < 4; page++) {
        crtcReg(v, 12 + page * 2, (uint16_t)(37 * page + 5)); // ページごとの X スクロール
        crtcReg(v, 13 + page * 2, (uint16_t)(53 * page + 9)); // ページごとの Y スクロール
    }
}

void setupG256(Video& v) {
    Random rnd(3);
    timing512(v, 0x0115); // R20 の bit9-8 = 01 で 256 色
    randomPalette(v, rnd);
    randomTextVram(v, rnd);
    randomGraphicVram(v, rnd);
    vcReg(v, 0, 0x0001);
    vcReg(v, 1, kPriority);
    vcReg(v, 2, kShowText | kShowGraphic);
}

void setupG65536(Video& v) {
    Random rnd(4);
    timing512(v, 0x0315); // R20 の bit9-8 = 11 で 65536 色
    randomPalette(v, rnd);
    randomTextVram(v, rnd);
    randomGraphicVram(v, rnd);
    vcReg(v, 0, 0x0003);
    vcReg(v, 1, kPriority);
    vcReg(v, 2, kShowText | kShowGraphic);
}

struct Scene {
    const char* name;
    void (*setup)(Video&); // nullptr なら fill768 (画素を埋めるだけ)
};

const Scene kScenes[] = {
    {"fill768", nullptr},
    {"text768", setupText768},
    {"mix512", setupMix512},
    {"mix512_slow", setupMix512Slow},
    {"g256_text", setupG256},
    {"g65536_text", setupG65536},
};

int compareDouble(const void* a, const void* b) {
    const double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

uint32_t g_pixels[Renderer::MAX_WIDTH * Renderer::MAX_HEIGHT];

void measure(const Scene& scene, int frames) {
    double* ms = new double[frames];
    Video* video = new Video();
    video->reset();
    Renderer* renderer = nullptr;
    if (scene.setup) {
        scene.setup(*video);
        renderer = new Renderer(*video);
        renderer->setTarget(g_pixels, Renderer::MAX_WIDTH, Renderer::MAX_WIDTH, Renderer::MAX_HEIGHT);
        renderer->renderFrame(); // 1 回目は表の準備も含むので数えない
    }
    for (int i = 0; i < frames; i++) {
        const double t0 = seconds();
        if (renderer) {
            renderer->renderFrame();
        } else {
            const uint32_t color = 0x00102030u + (uint32_t)i;
            for (int line = 0; line < 512; line++) {
                uint32_t* row = g_pixels + line * Renderer::MAX_WIDTH;
                for (int x = 0; x < 768; x++) row[x] = color;
            }
        }
        ms[i] = (seconds() - t0) * 1000.0;
    }
    qsort(ms, (size_t)frames, sizeof ms[0], compareDouble);
    int w = 768, h = 512;
    if (renderer) {
        w = renderer->width();
        h = renderer->height();
    }
    printf("%-12s %4dx%-4d  min %7.3f ms  median %7.3f ms\n", scene.name, w, h, ms[0], ms[frames / 2]);
    delete renderer;
    delete video;
    delete[] ms;
}

} // namespace

int main(int argc, char* argv[]) {
    int frames = 200;
    int first = 1;
    if (argc >= 3 && !strcmp(argv[1], "-n")) {
        frames = atoi(argv[2]);
        first = 3;
        if (frames < 1) {
            fprintf(stderr, "-n: want a positive number of frames\n");
            return 2;
        }
    }
    const int count = (int)(sizeof kScenes / sizeof kScenes[0]);
    if (first >= argc) {
        for (int i = 0; i < count; i++) measure(kScenes[i], frames);
        return EXIT_SUCCESS;
    }
    for (int a = first; a < argc; a++) {
        int i = 0;
        while (i < count && strcmp(kScenes[i].name, argv[a])) i++;
        if (i == count) {
            fprintf(stderr, "unknown scene: %s\n", argv[a]);
            return 2;
        }
        measure(kScenes[i], frames);
    }
    return EXIT_SUCCESS;
}
