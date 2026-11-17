// 画面の合成を別のスレッドで行ったとき (x68k/video_replay.h) に、1 つのスレッドで合成したときと同じ画面になるかを
// 確かめる。同じ Machine を 2 つ作り、片方は今までどおり、もう片方は合成を別のスレッドで行い、
// Human68k の起動 (900 フレーム) と machine_bench の gfx の場面 (グラフィック VRAM に書き続けながら、
// テキスト + グラフィック 4 ページ + スプライト 128 枚 + BG0 を出す。途中で 1 回高速クリアをする) を動かして、
//   - フレームごとの画面のハッシュがすべて同じこと
//   - 最後に、合成するスレッドの Video の写しが X68000 の Video と同じこと (記録し忘れた操作がないこと)
// を調べる。ThreadSanitizer でビルドして動かすと、合成するスレッドが X68000 の Video を読んでいないことも確かめられる。
//   render_thread_test [-r ROM のディレクトリ] [-0 起動ディスク] [--ring log2] [--frames N] [--no-gfx]
//   --frames N: 起動のフレーム数 (既定 900)。--no-gfx: gfx の場面を動かさない (ゲームのディスクで確かめるとき)
// ROM か起動ディスクがなければ 77 (ctest の SKIP_RETURN_CODE)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <thread>

#include "../bench/scene.h"
#include "x68k/machine.h"
#include "x68k/render.h"
#include "x68k/video_replay.h"

namespace {

uint8_t* loadWhole(const char* path, size_t& size) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = n > 0 ? new uint8_t[(size_t)n] : nullptr;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        delete[] buf;
        buf = nullptr;
    }
    fclose(f);
    size = buf ? (size_t)n : 0;
    return buf;
}

uint32_t fnv(uint32_t h, const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

// フレームを描き終えるたびに、描いた範囲のハッシュを残す Renderer
class HashRenderer : public x68k::Renderer {
public:
    static constexpr int MAX_FRAMES = 16384;
    explicit HashRenderer(const x68k::Video& v) : Renderer(v) {
        pixels = new uint32_t[(size_t)MAX_WIDTH * MAX_HEIGHT];
        memset(pixels, 0, (size_t)MAX_WIDTH * MAX_HEIGHT * 4);
        setTarget(pixels, MAX_WIDTH, MAX_WIDTH, MAX_HEIGHT);
    }
    ~HashRenderer() override { delete[] pixels; }
    // 渡されたラスタの番号もハッシュに入れる (負の番号が同じに届くかも見る)
    void onRaster(int raster) override {
        rasterHash = fnv(rasterHash, &raster, sizeof raster);
        if (raster < 0) negativeRasters++;
        Renderer::onRaster(raster);
    }
    void onFrameEnd() override {
        Renderer::onFrameEnd();
        int w = width(), h = height();
        uint32_t x = fnv(rasterHash, &w, sizeof w);
        rasterHash = 2166136261u;
        x = fnv(x, &h, sizeof h);
        for (int y = 0; y < h && y < MAX_HEIGHT; y++) x = fnv(x, pixels + (size_t)y * MAX_WIDTH, (size_t)w * 4);
        if (count < MAX_FRAMES) hashes[count] = x;
        count++;
    }
    uint32_t* pixels;
    uint32_t hashes[MAX_FRAMES];
    int count = 0;
    uint32_t rasterHash = 2166136261u;
    long negativeRasters = 0;
};

// 負のラスタの番号が、リングを通したあとも同じ値で届くか
class RasterLog : public x68k::LineListener {
public:
    void onRaster(int raster) override {
        if (n < 8) got[n++] = raster;
    }
    void onFrameEnd() override {}
    int got[8];
    int n = 0;
};

bool checkRasterNumbers() {
    static uint32_t ring[64];
    x68k::VideoJournal journal;
    journal.init(ring, 6);
    static x68k::Video shadow;
    RasterLog log;
    x68k::VideoReplayer replayer(journal, shadow, log);
    const int want[] = {-40, -1, 0, 1, 511, 1023};
    for (int r : want) journal.push(x68k::VideoJournal::make(x68k::VideoJournal::OP_RASTER, (uint32_t)r, 0));
    journal.publish();
    while (replayer.step() != 0) {
    }
    bool ok = log.n == 6;
    for (int i = 0; ok && i < 6; i++) ok = log.got[i] == want[i];
    if (!ok) fprintf(stderr, "FAIL: raster numbers changed through the ring\n");
    return ok;
}

struct Setup {
    const uint8_t* ipl;
    size_t iplSize;
    const uint8_t* cg;
    size_t cgSize;
    uint8_t* disk;
    size_t diskSize;
    const char* diskPath;
};

int kBootFrames = 900, kGfxFrames = 120;

x68k::Machine* makeMachine(const Setup& s, x68k::FloppyImage& image) {
    x68k::Machine* m = new x68k::Machine();
    // イメージはディスクの内容を指す (書き込みで変わる) ので、Machine ごとに写しを使う
    uint8_t* copy = new uint8_t[s.diskSize];
    memcpy(copy, s.disk, s.diskSize);
    if (!m->ok() || !m->setIplRom(s.ipl, s.iplSize) || !image.load(copy, s.diskSize, false, s.diskPath)) return nullptr;
    if (s.cg) m->setCgRom(s.cg, s.cgSize);
    m->insertDisk(0, &image);
    m->setTime(1767225600);
    m->sound().setOutputRate(48000);
    return m;
}

// 起動と gfx の場面を動かす。onFrame はフレームごとに呼ぶ (音を捨てる)
bool runScenes(x68k::Machine& m) {
    static int16_t audio[2 * 4096];
    m.powerOn();
    for (int i = 0; i < kBootFrames; i++) {
        m.runFrame();
        while (m.sound().ring().drain(audio, 4096) > 0) {
        }
    }
    if (kGfxFrames == 0) return true;
    if (!scene::startGfx(m)) return false;
    for (int i = 0; i < kGfxFrames; i++) {
        // 途中で 1 回、グラフィックの高速クリアを頼む (次の垂直表示期間の始まりに消える)
        if (i == kGfxFrames / 2) scene::poke8(m, 0xe80481, 0x02);
        m.runFrame();
        while (m.sound().ring().drain(audio, 4096) > 0) {
        }
    }
    return true;
}

unsigned quitFlag = 0;

void relax() { std::this_thread::yield(); }

} // namespace

int main(int argc, char** argv) {
    char romDir[1024];
    const char* home = getenv("HOME");
    snprintf(romDir, sizeof romDir, "%s/.cache/pix68k/rom", home ? home : ".");
    char diskPath[1100];
    snprintf(diskPath, sizeof diskPath, "%s/.cache/pix68k/disk/HUMAN302.XDF", home ? home : ".");
    const char* disk = diskPath;
    unsigned ringLog2 = 16;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) snprintf(romDir, sizeof romDir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "-0") && i + 1 < argc) disk = argv[++i];
        else if (!strcmp(argv[i], "--ring") && i + 1 < argc) ringLog2 = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) kBootFrames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-gfx")) kGfxFrames = 0;
        else {
            fprintf(stderr, "usage: %s [-r romdir] [-0 bootdisk] [--ring log2] [--frames N] [--no-gfx]\n", argv[0]);
            return 2;
        }
    }
    if (!checkRasterNumbers()) return 1;
    char path[1100];
    Setup s = {};
    s.diskPath = disk;
    snprintf(path, sizeof path, "%s/IPLROM.DAT", romDir);
    s.ipl = loadWhole(path, s.iplSize);
    snprintf(path, sizeof path, "%s/CGROM.DAT", romDir);
    s.cg = loadWhole(path, s.cgSize);
    s.disk = loadWhole(disk, s.diskSize);
    if (!s.ipl || !s.disk) {
        fprintf(stderr, "skip: cannot read %s/IPLROM.DAT or %s\n", romDir, disk);
        return 77;
    }

    // 1 つのスレッドで合成する
    static x68k::FloppyImage imageA, imageB;
    x68k::Machine* a = makeMachine(s, imageA);
    if (!a) return 1;
    HashRenderer* ra = new HashRenderer(a->video());
    a->setLineListener(ra);
    if (!runScenes(*a)) {
        fprintf(stderr, "the interrupt mask never became 0\n");
        return 1;
    }

    // 合成を別のスレッドで行う。リングを小さくして、満杯で待つ場合も通るようにする
    x68k::Machine* b = makeMachine(s, imageB);
    if (!b) return 1;
    x68k::Video* shadow = new x68k::Video;
    HashRenderer* rb = new HashRenderer(*shadow);
    uint32_t* ring = new uint32_t[1u << ringLog2];
    x68k::VideoJournal journal;
    journal.init(ring, ringLog2);
    journal.setRelax(relax);
    x68k::VideoReplayer replayer(journal, *shadow, *rb);
    x68k::RenderLink link;
    link.attach(*b, journal, *shadow, replayer);
    std::thread thread([&] {
        while (!__atomic_load_n(&quitFlag, __ATOMIC_ACQUIRE))
            if (replayer.step() == 0) std::this_thread::yield();
    });
    bool ok = runScenes(*b);
    link.detach(*b); // 合成するスレッドが追いつくまで待つ
    __atomic_store_n(&quitFlag, 1u, __ATOMIC_RELEASE);
    thread.join();
    if (!ok) {
        fprintf(stderr, "the interrupt mask never became 0\n");
        return 1;
    }

    int failures = 0;
    printf("frames: single %d, threaded %d; ring %u entries, %u entries pushed, full %llu times, "
           "waited for frames %llu times; negative rasters %ld\n",
           ra->count, rb->count, journal.capacity(), journal.pushed(), (unsigned long long)journal.stats().waits,
           (unsigned long long)link.frameWaits(), ra->negativeRasters);
    if (ra->negativeRasters != rb->negativeRasters) {
        fprintf(stderr, "FAIL: negative raster counts differ\n");
        failures++;
    }
    if (ra->count != rb->count || ra->count < kBootFrames - 1) {
        fprintf(stderr, "FAIL: frame counts differ (or too few frames)\n");
        failures++;
    }
    int n = ra->count < rb->count ? ra->count : rb->count;
    if (n > HashRenderer::MAX_FRAMES) n = HashRenderer::MAX_FRAMES;
    int diff = 0;
    for (int i = 0; i < n; i++) {
        if (ra->hashes[i] != rb->hashes[i]) {
            if (diff < 5) fprintf(stderr, "FAIL: frame %d: %08x != %08x\n", i, ra->hashes[i], rb->hashes[i]);
            diff++;
        }
    }
    if (diff) {
        fprintf(stderr, "FAIL: %d of %d frames differ\n", diff, n);
        failures++;
    }
    // 写しが X68000 の Video と同じか (記録し忘れた操作があれば違ってくる)
    const x68k::Video& va = b->video();
    struct {
        const char* name;
        const void* p;
        const void* q;
        size_t n;
    } areas[] = {
        {"text VRAM", va.tvram(), shadow->tvram(), x68k::Video::TVRAM_SIZE},
        {"graphic VRAM", va.gvramWords(), shadow->gvramWords(), x68k::Video::GVRAM_SIZE},
        {"palette", va.palette(), shadow->palette(), x68k::Video::PALETTE_SIZE},
        {"palette colors", va.paletteXrgb(), shadow->paletteXrgb(), 513 * 4},
        {"sprite", va.sprite(), shadow->sprite(), x68k::Video::SPRITE_SIZE},
    };
    for (const auto& ar : areas) {
        if (memcmp(ar.p, ar.q, ar.n) != 0) {
            fprintf(stderr, "FAIL: shadow %s differs\n", ar.name);
            failures++;
        }
    }
    for (int r = 0; r < 24; r++) {
        if (va.reg(r) != shadow->reg(r)) {
            fprintf(stderr, "FAIL: shadow CRTC R%02d differs\n", r);
            failures++;
        }
    }
    if (va.crtcGeneration() != shadow->crtcGeneration() || const_cast<x68k::Video&>(va).crtcRead(0x481) != shadow->crtcRead(0x481)) {
        fprintf(stderr, "FAIL: shadow CRTC state differs\n");
        failures++;
    }
    // 最後のフレームの画面がまったく同じか (ハッシュの衝突を除くため)
    if (memcmp(ra->pixels, rb->pixels, (size_t)x68k::Renderer::MAX_WIDTH * x68k::Renderer::MAX_HEIGHT * 4) != 0) {
        fprintf(stderr, "FAIL: final pixels differ\n");
        failures++;
    }
    if (failures) return 1;
    printf("OK: %d frames identical (boot %d + gfx %d)\n", n, kBootFrames, kGfxFrames);
    return 0;
}
