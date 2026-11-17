// X68000 全体 (CPU、周辺機器、画面の合成) の速さを測る。実時間の何倍で動くかと、その内訳を出す。
//
//   machine_bench [-r ROM のディレクトリ] [-0 起動ディスク] [-s 秒] [--clock MHz] [--no-render] [--no-sound]
//                 [--cpu 68000|68030|68060] [--ipl IPL ROM のファイル] [--speed percent]
//                 [--profile] [--render-thread] [--sound-thread] [--ring log2] [--pin CPU CPU CPU] [場面...]
//
//   --cpu 68030      X68030 (68030 25MHz)。IPL ROM の既定は IPLROM30.DAT
//   --cpu 68060      060turbo (68060 50MHz)。IPL ROM の既定は IPLROM060.DAT、なければ IPLROM30.DAT
//   --clock MHz      CPU のクロック。68000 は 10 か 16 (既定)、68060 は 50 (既定) か 75
//   --speed percent  CPU (68000 / 68030 / 68060) の命令の速さ (既定 100。Machine::setCpuSpeed)。200 ならクロックを 2 倍にしたのと同じ。
//                    060turbo では、選んだクロックの実機の 68060 を 100 とした % (5 から 3200。100 より小さくもできる)
//                    speed が 1.00 以上なら、その速さで実時間に間に合う (最高速モードの上限の目安になる)
//   --bus-wait N     060turbo で本体のバスの読み書き 1 回に足すサイクル数 (既定は Config::busWait060)
//   --render-thread  画面の合成を別のスレッドで行う (ベアメタルで別のコアに置くのと同じしくみ)
//   --ring log2      そのときのリングの大きさ (記録 2^log2 個、既定 18)
//   --sound-thread   音の合成 (OPM と ADPCM) を別のスレッドで行う (x68k/sound_replay.h)
//   --pin A B C      Linux で、X68000 を動かすスレッドを CPU A に、画面を合成するスレッドを CPU B に、
//                    音を合成するスレッドを CPU C に固定する (-1 なら固定しない)
//
// 場面 (既定はすべて):
//   boot  電源を入れて Human68k のプロンプトが出るまで (900 フレーム。フロッピーディスクの読み込みを含む)
//   idle  プロンプトのまま -s 秒 (既定 10 秒) 動かす
//   cpu   RAM に置いた計算のループ (加算、転送、シフト、比較、分岐) を、割り込みを受け付けながら -s 秒動かす
//   user  cpu と同じループをユーザモードで動かす (ユーザモードのメモリの読み書きの速さ)
//   gfx   512x512 の画面 (テキスト + グラフィック 4 ページ + スプライト 128 枚 + BG0) にし、OPM の 8 チャネルを
//         鳴らして (4 チャネルは LFO つき)、グラフィック VRAM に書き続けるループを -s 秒動かす
//         (画面と音の合成が最も重い場面の 1 つ)
//   fd    IOCS の _B_READ で、起動ディスクの先頭の 16KB (シリンダ 0 の両面) を読み続ける。FDC から RAM への DMA の
//         速さを見る (gfx のあとに動かすので、gfx を選んでいれば音は鳴ったまま)。
//         終わったあと、読み先がディスクの中身と同じか、_B_READ が 2 回以上終わったかを確かめ、違えば終了コードを 1 にする。
//         回数は FDC の読み込みのコマンドが正常に終わった数 (Fdc::transfersOk()。異常終了は数えない) で見る。X68000 の IPL ROM は 16 セクタを
//         1 セクタずつ 16 個のコマンドで読むので、32 個以上なら 2 回終わっている (16MHz の 68000 で 1 秒に 3600 個ほど)
// boot 以外の場面は boot のあとに続けて動かす (boot を省いても内部で起動はする)。
//
// 出る値:
//   speed     X68000 の時間 / 実時間 (1.00 で実機と同じ速さ)
//   checksum  場面の終わりの CPU のサイクル数とレジスタ、RAM、VRAM、合成した画面から作った値。
//             タイミングを変えないはずの変更の前後で同じになることを確かめる
//   worst     最も時間のかかったフレームの実時間と、それがそのフレームの X68000 の時間の何倍か (1 を超えると遅れる)
// --render-thread では、合成するスレッドの稼働率 (記録の適用と合成をしていた時間の、実時間に対する割合)、1 フレームの記録の数、フレームの終わりで
// 合成するスレッドがまだ描いていないラスタの数 (遅れ。フレームの終わりで見た値と、ラスタを入れるたびに見た値の最大)、リングが満杯で待った回数も出す。
// --profile を付けると内訳も出す (時刻を読む分だけ少し遅くなるので、speed は付けないときの値を使う):
//   cpu       全体から周辺機器の処理 (syncDevices) を除いた時間 (CPU のほか、このプログラムのフレームごとの処理も入る)
//   devices   周辺機器の処理から、画面と音の合成を除いた時間
//   render    画面の合成 (Renderer::onRaster) の時間
//   sound     音の合成 (OPM、ADPCM、混合、48kHz への変換) の時間 (--no-sound なら作らない)
//
// 時刻は aarch64 では仮想カウンタ (cntvct_el0) を直接読む (Pi 3 では 19.2MHz、1 回数ナノ秒)。
// Pi (Linux) では次のようにビルドする (ベアメタルと同じ -O2):
//   g++ -O2 -mcpu=cortex-a53 -std=c++17 -fno-exceptions -fno-rtti -Isrc tests/bench/machine_bench.cpp
//       src/m68k/*.cpp src/x68k/*.cpp -o machine_bench -lpthread
//   (68000 だけにするなら -DPIX68K_NO_CPU020 を付け、src/m68k/m68k.cpp だけを渡す)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <chrono>
#include <thread>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include "x68k/machine.h"
#include "x68k/render.h"
#include "x68k/sound_replay.h"
#include "x68k/video_replay.h"
#include "scene.h"

namespace {

#if defined(__aarch64__) && defined(__linux__)
uint64_t ticks() {
    uint64_t v;
    asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
double tickHz() {
    uint64_t f;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return (double)f;
}
#else
uint64_t ticks() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
double tickHz() { return 1e9; }
#endif

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

// 画面の合成の時間を測る Renderer
class TimedRenderer : public x68k::Renderer {
public:
    using Renderer::Renderer;
    uint64_t time = 0;
    bool timing = false;
    void onRaster(int raster) override {
        if (!timing) {
            Renderer::onRaster(raster);
            return;
        }
        uint64_t t0 = ticks();
        Renderer::onRaster(raster);
        time += ticks() - t0;
    }
};

uint32_t pixels[x68k::Renderer::MAX_WIDTH * x68k::Renderer::MAX_HEIGHT];

x68k::Machine* gm;
TimedRenderer* gr;
x68k::Profile prof;

// --render-thread: 画面の合成を別のスレッドで行う (ベアメタルで別のコアに置くのと同じしくみ。x68k/video_replay.h)
struct RenderThread {
    x68k::Video* shadow = nullptr;
    uint32_t* ring = nullptr;
    x68k::VideoJournal journal;
    x68k::VideoReplayer* replayer = nullptr;
    x68k::RenderLink link;
    std::thread thread;
    unsigned quit = 0;
    int cpu = -1; // 合成するスレッドを置く CPU (Linux、--pin)
    uint64_t time = 0; // 記録を処理していた時間 (写しへの適用と合成。atomic で読み書きする)
};
RenderThread* rt;

// 今のスレッドを CPU cpu に固定する (Linux だけ。ほかでは何もしない)
void pinTo(int cpu) {
#if defined(__linux__)
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0) fprintf(stderr, "cannot pin to CPU %d\n", cpu);
#else
    (void)cpu;
#endif
}

void relax() { std::this_thread::yield(); }

void renderLoop() {
    pinTo(rt->cpu);
    while (!__atomic_load_n(&rt->quit, __ATOMIC_ACQUIRE)) {
        uint64_t t0 = ticks();
        if (rt->replayer->step() != 0) {
            __atomic_store_n(&rt->time, rt->time + (ticks() - t0), __ATOMIC_RELAXED);
        } else {
#if defined(__aarch64__)
            asm volatile("yield");
#elif defined(__x86_64__)
            __builtin_ia32_pause();
#endif
        }
    }
}

void startRenderThread(unsigned log2, int cpu) {
    rt = new RenderThread;
    rt->shadow = new x68k::Video;
    rt->ring = new uint32_t[1u << log2];
    rt->journal.init(rt->ring, log2);
    rt->journal.setRelax(relax);
    rt->cpu = cpu;
    gr = new TimedRenderer(*rt->shadow);
    rt->replayer = new x68k::VideoReplayer(rt->journal, *rt->shadow, *gr);
    rt->link.attach(*gm, rt->journal, *rt->shadow, *rt->replayer);
    rt->link.setLagProbe(rt->replayer);
    rt->thread = std::thread(renderLoop);
}

// --sound-thread: 音の合成を別のスレッドで行う
struct SoundThread {
    x68k::Sound synth;
    uint32_t* ring = nullptr;
    x68k::VideoJournal journal;
    x68k::SoundReplayer* replayer = nullptr;
    x68k::SoundLink link;
    std::thread thread;
    unsigned quit = 0;
    int cpu = -1;
    uint64_t time = 0; // 記録を処理していた時間 (合成するスレッドだけが書く。consume() のあとに書くので atomic で読み書きする)
};
SoundThread* st;

void soundLoop() {
    pinTo(st->cpu);
    while (!__atomic_load_n(&st->quit, __ATOMIC_ACQUIRE)) {
        uint64_t t0 = ticks();
        if (st->replayer->step() != 0) {
            __atomic_store_n(&st->time, st->time + (ticks() - t0), __ATOMIC_RELAXED);
        } else {
#if defined(__aarch64__)
            asm volatile("yield");
#elif defined(__x86_64__)
            __builtin_ia32_pause();
#endif
        }
    }
}

// powerOn() の前に呼ぶ
void startSoundThread(unsigned log2, int cpu) {
    st = new SoundThread;
    st->ring = new uint32_t[1u << log2];
    st->journal.init(st->ring, log2);
    st->journal.setRelax(relax);
    st->cpu = cpu;
    st->synth.setOutputRate(48000);
    st->replayer = new x68k::SoundReplayer(st->journal, st->synth);
    st->link.attach(*gm, st->journal);
    st->thread = std::thread(soundLoop);
}

void stopSoundThread() {
    st->link.detach(*gm);
    __atomic_store_n(&st->quit, 1u, __ATOMIC_RELEASE);
    st->thread.join();
}

// 音がたまるリング (音を別のスレッドで作るなら、そのスレッドの Sound のもの)
x68k::AudioRing& audioRing() { return st ? st->synth.ring() : gm->sound().ring(); }

void stopRenderThread() {
    rt->link.detach(*gm);
    __atomic_store_n(&rt->quit, 1u, __ATOMIC_RELEASE);
    rt->thread.join();
}

bool profile = false;

uint32_t fnv(uint32_t h, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

// 結果が変わっていないことを確かめるための値: CPU のサイクル数とレジスタ、メイン RAM、VRAM、合成した画面。
// 速くするための変更 (タイミングを変えないもの) の前後で同じ値になること
uint32_t checksum() {
    uint32_t h = 2166136261u;
    uint64_t c = gm->cycles();
    h = fnv(h, (const uint8_t*)&c, sizeof c);
    m68k::Registers r;
    gm->getRegisters(r);
    h = fnv(h, (const uint8_t*)r.d, sizeof r.d);
    h = fnv(h, (const uint8_t*)r.a, sizeof r.a);
    uint32_t misc[4] = {r.usp, r.ssp, r.sr, r.pc};
    h = fnv(h, (const uint8_t*)misc, sizeof misc);
    static uint8_t ram[0x400000];
    bool ok;
    for (uint32_t a = 0; a < sizeof ram; a++) ram[a] = gm->peek8(a, ok);
    h = fnv(h, ram, sizeof ram);
    h = fnv(h, gm->video().tvram(), x68k::Video::TVRAM_SIZE);
    h = fnv(h, (const uint8_t*)gm->video().gvramWords(), x68k::Video::GVRAM_SIZE);
    h = fnv(h, (const uint8_t*)pixels, sizeof pixels);
    return h;
}

// frames フレーム、または seconds 秒分動かして結果を出す
void measure(const char* name, int frames, double seconds) {
    const double hz = tickHz();
    prof = x68k::Profile();
    prof.clock = ticks;
    if (profile) gm->setProfile(&prof);
    uint64_t renderTime0 = 0;
    if (rt) {
        rt->journal.flush();
        renderTime0 = __atomic_load_n(&rt->time, __ATOMIC_RELAXED);
        rt->journal.resetStats();
        rt->link.resetMaxLag();
    } else {
        gr->time = 0;
        gr->timing = profile;
    }
    uint64_t soundTime0 = 0;
    if (st) {
        gm->sound().flushJournal();
        st->journal.flush();
        soundTime0 = __atomic_load_n(&st->time, __ATOMIC_RELAXED);
        st->journal.resetStats();
    }
    unsigned maxLag = 0;     // フレームの終わりで見た、合成するスレッドがまだ描いていないラスタの数の最大
    uint64_t sumLag = 0;
    const unsigned pushed0 = rt ? rt->journal.pushed() : 0;
    const uint64_t c0 = gm->cycles();
    const uint64_t limit = seconds > 0 ? (uint64_t)(seconds * gm->cpuHz()) : 0;
    double worstRatio = 0, worstMs = 0;
    int n = 0;
    uint64_t minCycles = ~0ull, maxCycles = 0;
    const uint64_t t0 = ticks();
    for (;;) {
        if (frames > 0 && n >= frames) break;
        if (limit && gm->cycles() - c0 >= limit) break;
        uint64_t fc = gm->cycles(), ft = ticks();
        gm->runFrame();
        static int16_t audio[2 * 4096]; // 音を出す側の代わりに、たまった音を捨てる
        while (audioRing().drain(audio, 4096) > 0) {
        }
        if (rt) {
            unsigned lag = (unsigned)(rt->link.rasters() - rt->replayer->rasters());
            if (lag > maxLag) maxLag = lag;
            sumLag += lag;
        }
        uint64_t cyc = gm->cycles() - fc;
        double wall = (ticks() - ft) / hz;
        double emu = (double)cyc / gm->cpuHz();
        if (wall / emu > worstRatio) worstRatio = wall / emu;
        if (wall > worstMs / 1000) worstMs = wall * 1000;
        if (cyc < minCycles) minCycles = cyc;
        if (cyc > maxCycles) maxCycles = cyc;
        n++;
    }
    // 合成するスレッドが描き終えるまで待つ (checksum() は合成した画面も読む)。待つ時間も実時間に入れる
    if (rt) rt->journal.flush();
    if (st) {
        gm->sound().flushJournal();
        st->journal.flush();
    }
    const double wall = (ticks() - t0) / hz;
    gm->setProfile(nullptr);
    const uint64_t cycles = gm->cycles() - c0;
    const double emu = (double)cycles / gm->cpuHz();
    printf("%-5s emulated %6.2f s in %6.2f s: speed %5.2fx  checksum %08x\n", name, emu, wall, emu / wall, checksum());
    printf("      %d frames, %llu..%llu cycles/frame (%.2f ms per frame on average), "
           "worst frame %.2f ms (%.2f of its own time)\n",
           n, (unsigned long long)minCycles, (unsigned long long)maxCycles, wall * 1000 / n, worstMs, worstRatio);
    if (rt) {
        const x68k::VideoJournal::Stats& js = rt->journal.stats();
        const double busy = (__atomic_load_n(&rt->time, __ATOMIC_RELAXED) - renderTime0) / hz;
        printf("      render thread: busy %4.1f%% of the time, %.0f entries per frame, lag at frame end "
               "avg %.0f / max %u rasters, max %u rasters at any raster (ring %u entries), full %llu times (%llu spins), "
               "waited for frames %llu times\n",
               busy / wall * 100, (double)(rt->journal.pushed() - pushed0) / n, (double)sumLag / n, maxLag,
               rt->link.maxLag(), rt->journal.capacity(), (unsigned long long)js.waits, (unsigned long long)js.waitSpins,
               (unsigned long long)rt->link.frameWaits());
    }
    if (st) {
        printf("      sound thread: busy %4.1f%% of the time, full %llu times\n", (__atomic_load_n(&st->time, __ATOMIC_RELAXED) - soundTime0) / hz / wall * 100,
               (unsigned long long)st->journal.stats().waits);
    }
    if (profile) {
        const double render = rt ? 0 : gr->time / hz, sound = prof.sound / hz;
        const double dev = prof.devices / hz - render - sound, cpu = wall - prof.devices / hz;
        printf("      cpu %4.1f%%  devices %4.1f%%  render %4.1f%%  sound %4.1f%%  "
               "(%.0f slices, %.0f syncs per emulated s)\n",
               cpu / wall * 100, dev / wall * 100, render / wall * 100, sound / wall * 100, prof.slices / emu,
               prof.syncs / emu);
    }
}

bool want(int argc, char** argv, int first, const char* name) {
    if (first >= argc) return true;
    for (int i = first; i < argc; i++)
        if (!strcmp(argv[i], name)) return true;
    return false;
}

} // namespace

// -s の上限 (1 日。16MHz でもサイクル数は 64 ビットに収まる)
const double kMaxSeconds = 86400;

int main(int argc, char** argv) {
    char romDir[1024];
    const char* home = getenv("HOME");
    snprintf(romDir, sizeof romDir, "%s/.cache/pix68k/rom", home ? home : ".");
    char diskPath[1100];
    snprintf(diskPath, sizeof diskPath, "%s/.cache/pix68k/disk/HUMAN302.XDF", home ? home : ".");
    const char* disk = diskPath;
    double seconds = 10;
    uint32_t clock = 0; // 0 は CPU の既定
    bool render = true, sound = true, renderThread = false;
    int pinMain = -1, pinRender = -1, pinSound = -1;
    bool soundThread = false;
    unsigned ringLog2 = 18;
    int cpuModel = 68000;
    const char* iplPath = nullptr;
    int cpuSpeed = 100;
    int busWait = -1;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        const char* a = argv[i];
        bool hasArg = i + 1 < argc;
        if (!strcmp(a, "-r") && hasArg) snprintf(romDir, sizeof romDir, "%s", argv[++i]);
        else if (!strcmp(a, "-0") && hasArg) disk = argv[++i];
        else if (!strcmp(a, "-s") && hasArg) {
            // 0 以下、数でないもの、無限大 (整数のサイクル数にできない) は受け付けない
            const char* v = argv[++i];
            char* end = nullptr;
            seconds = strtod(v, &end);
            if (end == v || *end != 0 || !(seconds > 0) || !(seconds <= kMaxSeconds)) {
                fprintf(stderr, "%s: -s: want seconds in (0, %.0f], got '%s'\n", argv[0], kMaxSeconds, v);
                return 2;
            }
        }
        else if (!strcmp(a, "--clock") && hasArg) clock = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(a, "--cpu") && hasArg) cpuModel = atoi(argv[++i]);
        else if (!strcmp(a, "--ipl") && hasArg) iplPath = argv[++i];
        else if (!strcmp(a, "--speed") && hasArg) cpuSpeed = atoi(argv[++i]);
        else if (!strcmp(a, "--bus-wait") && hasArg) busWait = atoi(argv[++i]);
        else if (!strcmp(a, "--no-render")) render = false;
        else if (!strcmp(a, "--profile")) profile = true;
        else if (!strcmp(a, "--no-sound")) sound = false;
        else if (!strcmp(a, "--render-thread")) renderThread = true;
        else if (!strcmp(a, "--ring") && hasArg) ringLog2 = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--sound-thread")) soundThread = true;
        else if (!strcmp(a, "--pin") && i + 3 < argc) {
            pinMain = atoi(argv[++i]);
            pinRender = atoi(argv[++i]);
            pinSound = atoi(argv[++i]);
        }
        else {
            fprintf(stderr, "usage: %s [-r romdir] [-0 bootdisk] [-s seconds] [--clock 10|16] [--no-render] "
                            "[--no-sound] [--cpu 68000|68030|68060] [--ipl file] [--speed percent] [--bus-wait N] [--profile] [--render-thread] [--sound-thread] [--ring log2] "
                            "[--pin cpu cpu cpu] "
                            "[boot|idle|cpu|user|gfx ...]\n",
                    argv[0]);
            return 2;
        }
    }
    // サイクル数に直して 1 未満なら、0 (秒数の指定なし) と同じになって終わらないので受け付けない
    if (seconds * (clock ? clock : 10) * 1e6 < 1) {
        fprintf(stderr, "%s: -s: want seconds in (0, %.0f], got %g (less than 1 CPU cycle)\n", argv[0], kMaxSeconds,
                seconds);
        return 2;
    }
    const int first = i;
    char path[1100];
    if (iplPath) snprintf(path, sizeof path, "%s", iplPath);
    else snprintf(path, sizeof path, "%s/%s", romDir, cpuModel != 68000 ? "IPLROM30.DAT" : "IPLROM.DAT");
    size_t iplSize = 0, cgSize = 0, diskSize = 0;
    uint8_t* ipl = nullptr;
    if (!iplPath && cpuModel == 68060) {
        char p060[1100];
        snprintf(p060, sizeof p060, "%s/IPLROM060.DAT", romDir);
        ipl = loadWhole(p060, iplSize);
    }
    if (!ipl) ipl = loadWhole(path, iplSize);
    snprintf(path, sizeof path, "%s/CGROM.DAT", romDir);
    uint8_t* cg = loadWhole(path, cgSize);
    uint8_t* data = loadWhole(disk, diskSize);
    if (!ipl || !data) {
        fprintf(stderr, "cannot read the IPL ROM or %s\n", disk);
        return 77;
    }
    x68k::Config config;
    if (!config.setCpu(cpuModel)) {
        fprintf(stderr, "--cpu: 68000, 68030 or 68060\n");
        return 2;
    }
    if (clock && !config.setClockMHz((int)clock)) {
        fprintf(stderr, "--clock: 10 or 16 (68000), 50 or 75 (68060)\n");
        return 2;
    }
    if (busWait >= 0) config.busWait060 = busWait; // setCpu() と setClockMHz() がクロックから決めた値を書き換える
    // 060turbo の --speed は実機の 68060 を 100 とした %。100 より小さい値は、設定の速さ (Config::speed060) として渡す
    if (cpuModel == 68060 && cpuSpeed < 100) config.speed060 = cpuSpeed;
    gm = new x68k::Machine(config);
    static x68k::FloppyImage image;
    if (!gm->ok() || !gm->setIplRom(ipl, iplSize) || !image.load(data, diskSize, false, disk)) {
        fprintf(stderr, "cannot set up the machine\n");
        return 1;
    }
    if (cg) gm->setCgRom(cg, cgSize);
    gm->setCpuSpeed(cpuSpeed);
    if (cpuModel != 68000 && iplSize == x68k::Machine::IPL_SIZE) {
        static uint8_t scsi30[x68k::Machine::SCSI_ROM_SIZE];
        x68k::Machine::makeScsiRom30(ipl, scsi30);
        gm->setScsiRom(scsi30, sizeof scsi30);
    }
    memset(pixels, 0, sizeof pixels); // 最初のフレームでページを割り当てる時間を測らないよう、先に触っておく
    pinTo(pinMain);
    if (render && renderThread) {
        startRenderThread(ringLog2, pinRender);
    } else {
        gr = new TimedRenderer(gm->video());
        if (render) gm->setLineListener(gr);
    }
    gr->setTarget(pixels, x68k::Renderer::MAX_WIDTH, x68k::Renderer::MAX_WIDTH, x68k::Renderer::MAX_HEIGHT);
    gm->insertDisk(0, &image);
    gm->setTime(1767225600);
    if (sound && soundThread) startSoundThread(16, pinSound); // 1ms ごとに記録を見せるので、2^16 個で足りる
    else if (sound) gm->sound().setOutputRate(48000);
    gm->powerOn();
    printf("%u MHz, render %s, sound %s, timer %.1f MHz\n", (unsigned)(gm->cpuHz() / 1000000),
           !render ? "off" : rt ? "on (separate thread)" : "on", !sound ? "off" : st ? "on (separate thread)" : "on",
           tickHz() / 1e6);

    int rc = 0;
    measure("boot", 900, 0);
    if (want(argc, argv, first, "idle")) measure("idle", 0, seconds);
    if (want(argc, argv, first, "cpu")) {
        if (!scene::jumpTo(*gm, scene::kCpuLoop, sizeof scene::kCpuLoop / 2)) {
            fprintf(stderr, "the interrupt mask never became 0\n");
            return 1;
        }
        measure("cpu", 0, seconds);
    }
    if (want(argc, argv, first, "user")) {
        if (!scene::jumpTo(*gm, scene::kCpuLoop, sizeof scene::kCpuLoop / 2, true)) {
            fprintf(stderr, "the interrupt mask never became 0\n");
            return 1;
        }
        measure("user", 0, seconds);
    }
    if (want(argc, argv, first, "gfx")) {
        if (!scene::startGfx(*gm)) {
            fprintf(stderr, "the interrupt mask never became 0\n");
            return 1;
        }
        measure("gfx", 0, seconds);
    }
    if (want(argc, argv, first, "fd")) {
        if (!scene::jumpTo(*gm, scene::kFdLoop, sizeof scene::kFdLoop / 2)) {
            fprintf(stderr, "the interrupt mask never became 0\n");
            return 1;
        }
        // 読み込みが始まったことを確かめるため、読み先を消しておく
        for (uint32_t i = 0; i < scene::kFdBytes; i++) scene::poke8(*gm, scene::kFdBuffer + i, 0);
        const uint32_t reads0 = gm->fdc().transfersOk();
        measure("fd", 0, seconds);
        const uint32_t reads = gm->fdc().transfersOk() - reads0;
        bool same = true, ok;
        for (uint32_t i = 0; i < scene::kFdBytes && same; i++)
            same = gm->peek8(scene::kFdBuffer + i, ok) == data[i];
        printf("      fd: %u read commands finished normally\n", reads);
        // 1 回目の _B_READ だけで読み先は合うので、読み込みを繰り返していることも確かめる。1 回の _B_READ のコマンドは
        // 多くても 1 セクタに 1 個 (16 個) なので、その 2 倍あれば 2 回は終わっている
        const uint32_t wantReads = 2 * (scene::kFdBytes / 1024);
        if (!same) {
            printf("      fd: the buffer does not match the disk\n");
            rc = 1;
        }
        if (reads < wantReads) {
            printf("      fd: fewer than %u read commands finished normally\n", wantReads);
            rc = 1;
        }
    }
    if (rt) stopRenderThread();
    if (st) stopSoundThread();
    return rc;
}
