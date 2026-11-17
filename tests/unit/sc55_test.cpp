// SC-55mkII の音源 (platform/sc55_synth.h、Nuked-SC55) のテスト。
// ROM のない環境でも通る試験: ROM のファイルの選び方 (pickRoms())、大きさが足りない ROM、リセットのベクタの確かめ、
// でたらめな ROM で起動させても範囲の外を読み書きせず止まらないこと (ASan / UBSan のビルドで確かめる)、
// load() の中で確保する量が Sc55Synth::heapBytes() に収まること、読めなかったときに解放すること、
// 周波数を変える FIR の通過域と折り返しの減衰 (Sc55Synth::buildFilter() の表から周波数の特性を計算する)。
// 本物の ROM があれば鳴ることも見る (環境変数で ROM のディレクトリを渡す。なければ飛ばす):
//   PIX68K_SC55_ROM=sc55mk2 build/sc55_test
// 本物の ROM での試験: 周波数、音が鳴って止まること、音を作る間に確保しないこと、同期と非同期で同じ音になること
// (非同期は ASYNC_DELAY だけ遅れる)、非同期で間に合わないときに無音を足して数え、あとで追いつくこと、大きく遅れたら
// 飛ばすこと、midiReset() で音が止まりランニングステータスが残ること、2 つ目は開けないこと。
// 非同期で、midiByte() / midiReset() / mixFrame() と pump() を別のスレッドから呼んでも同期と同じ音になること
// (-fsanitize=thread のビルドで、スレッドの間のデータの競合も調べる)。
// 設定の送り直し (MidiShadow::emit()) の最大の量が待ち行列に入り、落とさずに UART に入ること (量は ROM なしで比べる)。
// 送り直しを 2 回続けても落とさず、3 回続けると待ち行列を超えて落とすこと (sc55_synth.h の QUEUE_SIZE の説明)。
// 周波数の変換の標本の番号を SRC_REBASE (2^31) の手前に置いても、引き戻して同じ音を作り続けること (同期と非同期)。
// 番号は 2^31 を超えると引き戻すので、2^32 (pos_ の整数部が一周する数) には届かない。2^32 の手前に置いた試験は、
// SRC_REBASE より大きい番号から始めても引き戻すことを確かめる。
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <initializer_list>
#include <new>
#include <string>
#include <vector>

#include "platform/disk_cache_size.h"
#include "platform/sc55_synth.h"
#include "test_check.h"
#include "x68k/midi_shadow.h"

// ---------------------------------------------------------------- 確保を数える (tests/unit/mt32_test.cpp と同じ)
namespace {
bool g_counting = false;
uint64_t g_allocs = 0;
uint64_t g_heapBytes = 0;
long g_live = 0; // 生きているブロックの数 (いつも数える)
const uint32_t kCircleBuckets[] = {0x40, 0x400, 0x1000, 0x4000, 0x10000, 0x40000, 0x80000, 0};
const platform::HeapModel kCircleHeap = {kCircleBuckets, 64, 64};

void* countedAlloc(size_t n) {
    void* p = malloc(n ? n : 1);
    if (p == nullptr) abort();
    g_live++;
    if (g_counting) {
        g_allocs++;
        g_heapBytes += platform::heapBlockBytes(n, kCircleHeap);
    }
    return p;
}
} // namespace

void* operator new(size_t n) { return countedAlloc(n); }
void* operator new[](size_t n) { return countedAlloc(n); }
namespace {
void countedFree(void* p) {
    if (p == nullptr) return;
    g_live--;
    free(p);
}
} // namespace
void operator delete(void* p) noexcept { countedFree(p); }
void operator delete[](void* p) noexcept { countedFree(p); }
void operator delete(void* p, size_t) noexcept { countedFree(p); }
void operator delete[](void* p, size_t) noexcept { countedFree(p); }

namespace {

using platform::Sc55Synth;

void startCount() {
    g_allocs = 0;
    g_heapBytes = 0;
    g_counting = true;
}
void stopCount() { g_counting = false; }

// ---------------------------------------------------------------- ROM なし

void testPick() {
    int index[Sc55Synth::ROM_COUNT];
    // Nuked-SC55 の名前 (大文字でもよい)。同じ大きさのものがほかにあっても名前で選ぶ
    {
        const char* names[] = {"ROM1.BIN", "rom2.bin", "rom_sm.bin", "waverom1.bin", "waverom2.bin", "other.bin"};
        const uint64_t sizes[] = {0x8000, 0x80000, 0x1000, 0x200000, 0x100000, 0x100000};
        CHECK_EQ(Sc55Synth::pickRoms(names, sizes, 6, index), -1);
        for (int i = 0; i < Sc55Synth::ROM_COUNT; i++) CHECK_EQ(index[i], i);
    }
    // 部品の番号の名前 (MAME などの名前) は大きさで選ぶ。macOS の ._ のファイル (4KB) は見ない
    {
        const char* names[] = {"._r15199880_secondary_mcu.bin", "r15209359_pcm_1.bin", "r15279813_pcm_2.bin",
                               "r00233567_control.bin", "r15199858_main_mcu.bin", "r15199880_secondary_mcu.bin"};
        const uint64_t sizes[] = {4096, 0x200000, 0x100000, 0x80000, 0x8000, 0x1000};
        CHECK_EQ(Sc55Synth::pickRoms(names, sizes, 6, index), -1);
        CHECK_EQ(index[Sc55Synth::ROM1], 4);
        CHECK_EQ(index[Sc55Synth::ROM2], 3);
        CHECK_EQ(index[Sc55Synth::ROM_SM], 5);
        CHECK_EQ(index[Sc55Synth::WAVE1], 1);
        CHECK_EQ(index[Sc55Synth::WAVE2], 2);
    }
    // 同じ大きさのものが 2 つあって名前で決まらなければ選ばない。足りなければその番号を返す
    {
        const char* names[] = {"a.bin", "b.bin", "c.bin", "d.bin", "e.bin", "f.bin"};
        const uint64_t sizes[] = {0x8000, 0x80000, 0x1000, 0x200000, 0x100000, 0x100000};
        CHECK_EQ(Sc55Synth::pickRoms(names, sizes, 6, index), (int)Sc55Synth::WAVE2);
        CHECK_EQ(index[Sc55Synth::WAVE2], -1);
        CHECK_EQ(index[Sc55Synth::WAVE1], 3);
        CHECK_EQ(Sc55Synth::pickRoms(names, sizes, 0, index), (int)Sc55Synth::ROM1);
    }
}

struct FakeRoms {
    std::vector<uint8_t> data[Sc55Synth::ROM_COUNT];
    const uint8_t* ptr[Sc55Synth::ROM_COUNT];
    FakeRoms(uint32_t seed, bool random) {
        for (int i = 0; i < Sc55Synth::ROM_COUNT; i++) {
            data[i].resize(Sc55Synth::romSize(i));
            for (size_t k = 0; k < data[i].size() && random; k++) {
                seed = seed * 1103515245u + 12345u;
                data[i][k] = (uint8_t)(seed >> 16);
            }
            ptr[i] = data[i].data();
        }
    }
    // H8/532 のリセットのベクタ (ページ 0、番地 addr)
    void setReset(uint32_t addr) {
        data[Sc55Synth::ROM1][0] = 0;
        data[Sc55Synth::ROM1][1] = 0;
        data[Sc55Synth::ROM1][2] = (uint8_t)(addr >> 8);
        data[Sc55Synth::ROM1][3] = (uint8_t)addr;
    }
};

// 短すぎるファイルを読む口
class ShortSource : public platform::Sf2Source {
public:
    int read(void*, unsigned n) override { return (int)(n / 2); }
    bool skip(unsigned) override { return false; }
};

void testBadRoms() {
    // リセットのベクタが 0 (中身が 0 の ROM) は読まない。確保するのは H8/532 の中の ROM (32KB) だけで、解放する
    {
        FakeRoms roms(1, false);
        Sc55Synth s;
        const long live = g_live;
        startCount();
        CHECK_EQ(s.load(roms.ptr, 48000), Sc55Synth::BAD_ROM);
        stopCount();
        CHECK_EQ(g_allocs, 1);
        CHECK_EQ(g_live, live);
        CHECK_EQ(s.loaded(), false);
        // 読めていない音源は音を足さず、バイトも受けない
        int16_t out[8] = {1, 1, 1, 1, 1, 1, 1, 1};
        s.midiByte(0x90);
        s.renderFrames(out, 4);
        for (int16_t v : out) CHECK_EQ(v, 0);
        CHECK_EQ(s.pump(), 0);
    }
    // ベクタが ROM の外
    {
        FakeRoms roms(1, false);
        roms.setReset(0x9000);
        Sc55Synth s;
        CHECK_EQ(s.load(roms.ptr, 48000), Sc55Synth::BAD_ROM);
    }
    // 出力の周波数
    {
        FakeRoms roms(1, false);
        Sc55Synth s;
        CHECK_EQ(s.load(roms.ptr, 0), Sc55Synth::BAD_RATE);
        CHECK_EQ(s.load(roms.ptr, 400000), Sc55Synth::BAD_RATE);
    }
    // 途中で読めなくなったファイル (波形の ROM)。確保したものは解放する
    {
        FakeRoms roms(1, false);
        roms.setReset(0x100);
        ShortSource bad;
        struct Mem : platform::Sf2Source {
            const uint8_t* p;
            unsigned pos = 0;
            int read(void* out, unsigned n) override {
                memcpy(out, p + pos, n);
                pos += n;
                return (int)n;
            }
            bool skip(unsigned) override { return false; }
        } m[3];
        for (int i = 0; i < 3; i++) m[i].p = roms.ptr[i];
        platform::Sf2Source* src[Sc55Synth::ROM_COUNT] = {&m[0], &m[1], &m[2], &bad, &bad};
        Sc55Synth s;
        const long live = g_live;
        CHECK_EQ(s.load(src, 48000), Sc55Synth::READ_FAILED);
        CHECK_EQ(g_live, live);
        CHECK_EQ(s.loaded(), false);
    }
    printf("sc55: bad ROMs ok\n");
}

// ベクタだけが正しく、ほかは 0 かでたらめな ROM で起動させる。範囲の外を読み書きせず (ASan)、止まらずに終わること。
// PCM の LSI が SC-55 の周波数で音を出さなければ NO_SOUND。確保の量は heapBytes() に収まる
void testGarbageRom(uint32_t seed, bool random) {
    FakeRoms roms(seed, random);
    roms.setReset(0x100);
    Sc55Synth s;
    startCount();
    const Sc55Synth::Result r = s.load(roms.ptr, 48000);
    stopCount();
    CHECK_EQ(r == Sc55Synth::NO_SOUND || r == Sc55Synth::OK, true);
    if (r != Sc55Synth::OK) CHECK_EQ(s.loaded(), false);
    if (g_heapBytes > Sc55Synth::heapBytes()) {
        printf("  load() allocated %llu bytes > heapBytes() %llu\n", (unsigned long long)g_heapBytes,
               (unsigned long long)Sc55Synth::heapBytes());
        failures++;
    }
    CHECK_EQ(g_allocs > 0, true);
    if (r == Sc55Synth::OK) {
        int16_t out[2 * 480];
        for (int b = 0; b < 100; b++) {
            s.midiByte((uint8_t)(0x90 + b % 16));
            s.renderFrames(out, 480);
        }
    }
    printf("sc55: garbage ROM (%s): %s\n", random ? "random" : "zeros", Sc55Synth::resultText(r));
}

// ---------------------------------------------------------------- 本物の ROM

struct RomFiles {
    std::vector<uint8_t> data[Sc55Synth::ROM_COUNT];
    const uint8_t* ptr[Sc55Synth::ROM_COUNT] = {};
    bool ok = false;
    explicit RomFiles(const char* dir) {
        std::vector<std::string> names;
        std::vector<uint64_t> sizes;
        if (DIR* d = opendir(dir)) {
            while (dirent* e = readdir(d)) {
                struct stat st;
                if (stat((std::string(dir) + "/" + e->d_name).c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                    names.push_back(e->d_name);
                    sizes.push_back((uint64_t)st.st_size);
                }
            }
            closedir(d);
        }
        std::vector<const char*> cnames;
        for (auto& n : names) cnames.push_back(n.c_str());
        int index[Sc55Synth::ROM_COUNT];
        if (Sc55Synth::pickRoms(cnames.data(), sizes.data(), (int)names.size(), index) >= 0) return;
        for (int i = 0; i < Sc55Synth::ROM_COUNT; i++) {
            FILE* f = fopen((std::string(dir) + "/" + names[index[i]]).c_str(), "rb");
            if (!f) return;
            data[i].resize(Sc55Synth::romSize(i));
            const size_t n = fread(data[i].data(), 1, data[i].size(), f);
            fclose(f);
            if (n != data[i].size()) return;
            ptr[i] = data[i].data();
        }
        ok = true;
    }
};

void send(Sc55Synth& s, std::initializer_list<int> bytes) {
    for (int b : bytes) s.midiByte((uint8_t)b);
}

void gsReset(Sc55Synth& s) { send(s, {0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7}); }

double rms(const std::vector<int16_t>& v, size_t from, size_t to) {
    double sum = 0;
    for (size_t i = from; i < to; i++) sum += (double)v[i] * v[i];
    return to > from ? __builtin_sqrt(sum / (double)(to - from)) : 0;
}

// 同期で frames フレーム作る
std::vector<int16_t> render(Sc55Synth& s, int frames) {
    std::vector<int16_t> out(2 * (size_t)frames);
    s.renderFrames(out.data(), frames);
    return out;
}

// 送り直す量が最も多くなる設定を覚えた MidiShadow (MidiShadow::emit() が送るものを、すべて最も長くなるように受ける)
// - RESET_SYSEX_MAX (16) バイトより長い GS リセット (送り直すのは初めの 16 バイトと $F0、$F7)
// - どのチャネルも、覚えるコントロールチェンジをすべて受け、RPN 0-2 を初めと違う値にし、NRPN を選び、ピッチベンドを受ける
// - バンクセレクトは、LSB を受けたあとに MSB を 2 つ続けて受ける (最後の 2 つが MSB だけなので、その前に最後の LSB を送る。
//   音色を選んだときのバンクと今のバンクで 3 つずつ)
// - 最後に、SYSEX_MAX バイトを超えて切れたシステムエクスクルーシブを読みかけにする ($F0、64 バイト、超えたことを表す 1 バイト)
// 送り直す量は 18 + 16 x 404 + 1 + 66 = 6549 バイト (testShadowFits())
constexpr size_t LARGEST_RESEND = 6549;
void fillShadow(x68k::MidiShadow& m) {
    auto feed = [&](std::initializer_list<int> bytes) {
        for (int b : bytes) m.feed((uint8_t)b);
    };
    // GS リセットのあとに 11 バイトを足して 20 バイトにする
    feed({0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41});
    for (int i = 0; i < 11; i++) m.feed(0x00);
    m.feed(0xf7);
    for (int n = 0; n < 16; n++) {
        const int cc = 0xb0 | n;
        for (int k = 1; k < 120; k++) feed({cc, k, 1});
        feed({cc, 32, 2, cc, 0, 1, cc, 0, 3, 0xc0 | n, 5});
        for (int r = 0; r < 3; r++) feed({cc, 101, 0, cc, 100, r, cc, 6, 10, cc, 38, 1});
        feed({cc, 99, 1, cc, 98, 1, 0xe0 | n, 1, 1});
    }
    m.feed(0xf0);
    for (int i = 0; i < platform::MidiStream::SYSEX_MAX + 1; i++) m.feed(0x10);
}

std::vector<uint8_t> shadowBytes(const x68k::MidiShadow& m) {
    std::vector<uint8_t> v;
    m.emit([&](uint8_t b) { v.push_back(b); });
    return v;
}

// 送り直しの最大の量 (と midiReset() の 1 つ) が待ち行列の半分に収まる (残りは UART に入れる間に届くバイトのため。
// MIDI ボードからは 31.25kbps で 1 秒に 3125 バイトまで、UART に入れる速さも同じ)。
// 3 回分は待ち行列に入らない (sc55_synth.h の QUEUE_SIZE の説明。testRealResendBurst() で落ちることを確かめる)
void testShadowFits() {
    x68k::MidiShadow m;
    fillShadow(m);
    const size_t n = shadowBytes(m).size();
    printf("sc55: largest resend %zu bytes, queue %d\n", n, Sc55Synth::QUEUE_SIZE);
    CHECK_EQ(n, LARGEST_RESEND); // fillShadow() が最大の量を作っていること
    CHECK_EQ(n + 1 <= (size_t)Sc55Synth::QUEUE_SIZE / 2, true);
    CHECK_EQ(3 * (n + 1) > (size_t)Sc55Synth::QUEUE_SIZE, true);
}

// 周波数を変える FIR (Sc55Synth::buildFilter()) の周波数の特性。表のすべての位相を 1 つの FIR (PCM の LSI の標本の
// 1 / PHASES ごとの値) とみて、周波数 f の正弦波を通したときの振幅の倍率 (直流で 1) を dB で返す
double filterGainDb(const std::vector<float>& h, double srcRate, double f) {
    const int half = Sc55Synth::TAPS / 2;
    double re = 0, im = 0, dc = 0;
    for (int p = 0; p < Sc55Synth::PHASES; p++) {
        for (int j = 0; j < Sc55Synth::TAPS; j++) {
            const double v = h[(size_t)p * Sc55Synth::TAPS + j];
            const double t = (double)(j - (half - 1)) - (double)p / Sc55Synth::PHASES; // 標本の単位の位置
            const double w = 2 * M_PI * f / srcRate * t;
            re += v * cos(w);
            im += v * sin(w);
            dc += v;
        }
    }
    return 20 * log10(sqrt(re * re + im * im) / dc);
}

// 範囲 [f0, f1] での倍率の最大 (most = true) か最小 (dB)
double filterGainRange(const std::vector<float>& h, double srcRate, double f0, double f1, bool most) {
    double r = most ? -1e9 : 1e9;
    for (double f = f0; f <= f1; f += 25) {
        const double g = filterGainDb(h, srcRate, f);
        r = most ? (g > r ? g : r) : (g < r ? g : r);
    }
    return r;
}

// SC-55mkII の PCM の LSI の周波数 (66207Hz) から 48kHz に変えるときの、通過域の落ち込みと、24kHz を超える成分の減衰
// (出力の 48kHz で 48kHz - f に折り返す)。値は README に書いたもの。FIR を変えずにタップの数の限界を見るための試験で、
// 下限は測った値より少しゆるくしてある
void testFilter() {
    const double srcRate = 24000000.0 / 725 * 2;
    std::vector<float> h((size_t)(Sc55Synth::PHASES + 1) * Sc55Synth::TAPS);
    Sc55Synth::buildFilter(h.data(), srcRate, 48000);
    // どの位相も合計が 1 (直流をそのまま通す)
    for (int p = 0; p <= Sc55Synth::PHASES; p++) {
        double sum = 0;
        for (int j = 0; j < Sc55Synth::TAPS; j++) sum += h[(size_t)p * Sc55Synth::TAPS + j];
        CHECK_EQ(fabs(sum - 1) < 1e-5, true);
    }
    const double g10 = filterGainDb(h, srcRate, 10000), g15 = filterGainDb(h, srcRate, 15000);
    const double g20 = filterGainDb(h, srcRate, 20000), g24 = filterGainDb(h, srcRate, 24000);
    const double pass = filterGainRange(h, srcRate, 0, 20000, true);
    const double a24 = filterGainRange(h, srcRate, 24000, 28000, true);   // 20kHz から 24kHz に折り返す
    const double a28 = filterGainRange(h, srcRate, 28000, srcRate / 2, true); // 15kHz から 20kHz に折り返す
    const double image = filterGainRange(h, srcRate, srcRate / 2, 2 * srcRate, true); // 標本の周波数の折り返し (イメージ)
    printf("sc55: FIR 16 taps: 10kHz %.2f dB, 15kHz %.2f dB, 20kHz %.2f dB, 24kHz %.2f dB, max 0-20kHz %.2f dB\n", g10,
           g15, g20, g24, pass);
    printf("sc55: FIR 16 taps: max 24-28kHz %.1f dB, max 28-33.1kHz %.1f dB, max above 33.1kHz %.1f dB\n", a24, a28,
           image);
    CHECK_EQ(pass < 0.05, true);
    CHECK_EQ(g10 > -0.05, true);
    CHECK_EQ(g15 > -0.5, true);
    CHECK_EQ(g20 > -4.0, true);
    CHECK_EQ(a24 < -11.0, true);
    CHECK_EQ(a28 < -26.0, true);
    CHECK_EQ(image < -72.0, true);
}

void testRealBasic(const RomFiles& roms) {
    Sc55Synth s;
    CHECK_EQ(s.load(roms.ptr, 48000), Sc55Synth::OK);
    if (!s.loaded()) return;
    CHECK_EQ(s.sourceRate() > 66200 && s.sourceRate() < 66210, true);
    // 2 つ目は開けない
    {
        Sc55Synth t;
        CHECK_EQ(t.load(roms.ptr, 48000), Sc55Synth::IN_USE);
    }
    std::vector<int16_t> quiet = render(s, 4800);
    gsReset(s);
    render(s, 9600); // GS リセットを受けるのに 0.2 秒
    std::vector<int16_t> on(2 * 24000), off(2 * 48000);
    startCount();
    send(s, {0xc0, 0, 0x90, 60, 110, 0x99, 38, 120});
    s.renderFrames(on.data(), 24000);
    send(s, {0x80, 60, 0});
    s.renderFrames(off.data(), 48000);
    stopCount();
    CHECK_EQ(g_allocs, 0); // 音を作る間は確保しない
    const double q = rms(quiet, 0, quiet.size()), a = rms(on, 0, on.size()), b = rms(off, off.size() / 2, off.size());
    printf("sc55: rms quiet %.1f, note %.1f, after note off %.1f\n", q, a, b);
    CHECK_EQ(q < 20, true);
    CHECK_EQ(a > 300, true);
    CHECK_EQ(b < a / 10, true);
    CHECK_EQ(s.droppedBytes(), 0);
}

// 同じ MIDI を同期と非同期で鳴らす。非同期の音は同期の音より ASYNC_DELAY フレーム遅れ、ほかは同じ
void testRealAsyncMatchesSync(const RomFiles& roms) {
    const int frames = 48000;
    auto script = [](Sc55Synth& s, int frame) {
        if (frame == 0) gsReset(s);
        if (frame == 9600) send(s, {0xc0, 19, 0x90, 64, 100, 0x99, 36, 120});
        if (frame == 20000) send(s, {0x80, 64, 0, 0x90, 67, 90});
        if (frame == 30000) s.midiReset();
        if (frame == 30001) send(s, {72, 100}); // ランニングステータス ($90)
    };
    std::vector<int16_t> sync(2 * (size_t)(frames + Sc55Synth::ASYNC_DELAY));
    {
        Sc55Synth s;
        CHECK_EQ(s.load(roms.ptr, 48000), Sc55Synth::OK);
        s.renderFrames(sync.data(), Sc55Synth::ASYNC_DELAY);
        for (int f = 0; f < frames; f++) {
            script(s, f);
            s.renderFrames(&sync[2 * (size_t)(Sc55Synth::ASYNC_DELAY + f)], 1);
        }
    }
    std::vector<int16_t> async(2 * (size_t)(frames + Sc55Synth::ASYNC_DELAY));
    {
        Sc55Synth s;
        CHECK_EQ(s.load(roms.ptr, 48000), Sc55Synth::OK);
        s.setAsync(true);
        for (int f = 0; f < frames + Sc55Synth::ASYNC_DELAY; f++) {
            if (f < frames) script(s, f);
            while (s.pump() != 0) {
            }
            s.renderFrames(&async[2 * (size_t)f], 1);
        }
        CHECK_EQ(s.underruns(), 0);
        CHECK_EQ(s.skips(), 0);
    }
    size_t diff = 0;
    for (size_t i = 0; i < sync.size(); i++)
        if (sync[i] != async[i]) diff++;
    CHECK_EQ(diff, 0);
    // midiReset() のあと (30000) はランニングステータスで送った音 (30001) だけが鳴る
    const size_t d = Sc55Synth::ASYNC_DELAY;
    const double before = rms(sync, 2 * (d + 25000), 2 * (d + 30000));
    const double after = rms(sync, 2 * (d + 36000), 2 * (d + 48000));
    printf("sc55: async == sync (%zu differ), rms before reset %.1f, after %.1f\n", diff, before, after);
    CHECK_EQ(before > 300, true);
    CHECK_EQ(after > 100, true);
}

// midiReset() で鳴っている音が止まる
void testRealReset(const RomFiles& roms) {
    Sc55Synth s;
    CHECK_EQ(s.load(roms.ptr, 48000), Sc55Synth::OK);
    gsReset(s);
    render(s, 9600);
    send(s, {0xc0, 52, 0xb0, 64, 127, 0x90, 60, 110, 64, 110, 67, 110}); // ホールドを踏んで和音
    render(s, 9600);
    s.midiReset();
    render(s, 9600);
    std::vector<int16_t> after = render(s, 9600);
    const double a = rms(after, 0, after.size());
    printf("sc55: rms after midiReset %.1f\n", a);
    CHECK_EQ(a < 20, true);
}

// 非同期で pump() を呼ばないと無音を足して数え、呼べば追いつく。大きく遅れたら飛ばす
void testRealAsyncLate(const RomFiles& roms) {
    Sc55Synth s;
    CHECK_EQ(s.load(roms.ptr, 48000), Sc55Synth::OK);
    s.setAsync(true);
    gsReset(s);
    std::vector<int16_t> out(2 * 4800);
    s.renderFrames(out.data(), 100); // pump() を呼ばない
    CHECK_EQ(s.underruns(), 100);
    for (int16_t v : std::vector<int16_t>(out.begin(), out.begin() + 200)) CHECK_EQ(v, 0);
    while (s.pump() != 0) {
    }
    const uint32_t u = s.underruns();
    for (int i = 0; i < 1000; i++) {
        while (s.pump() != 0) {
        }
        s.renderFrames(out.data(), 1);
    }
    CHECK_EQ(s.underruns(), u); // 追いついた
    // 0.25 秒より遅れたら飛ばす
    s.renderFrames(out.data(), 4800);
    s.renderFrames(out.data(), 4800);
    s.renderFrames(out.data(), 4800);
    const uint32_t skips = s.skips();
    s.pump();
    CHECK_EQ(s.skips(), skips + 1);
    while (s.pump() != 0) {
    }
    // 出力の位置から ASYNC_DELAY / 2 先までは作らない (無音)。そこからは間に合う
    const uint32_t u1 = s.underruns();
    s.renderFrames(out.data(), Sc55Synth::ASYNC_DELAY / 2);
    CHECK_EQ(s.underruns(), u1 + Sc55Synth::ASYNC_DELAY / 2);
    while (s.pump() != 0) {
    }
    const uint32_t u2 = s.underruns();
    s.renderFrames(out.data(), 100);
    CHECK_EQ(s.underruns(), u2);
    printf("sc55: async late ok (underruns %u, skips %u)\n", s.underruns(), s.skips());
}

// 設定の送り直しの最大の量を、同期と非同期で落とさずに UART に入れ、そのあとの音が鳴る
void testRealResend(const RomFiles& roms, bool async) {
    Sc55Synth s;
    CHECK_EQ(s.load(roms.ptr, 48000), Sc55Synth::OK);
    s.setAsync(async);
    x68k::MidiShadow m;
    fillShadow(m);
    const std::vector<uint8_t> bytes = shadowBytes(m);
    s.midiReset();
    for (uint8_t b : bytes) s.midiByte(b);
    // 読みかけのシステムエクスクルーシブを閉じ、GS リセットで初めの設定に戻す (fillShadow() の設定では音程が下がりすぎる)
    s.midiByte(0xf7);
    gsReset(s);
    CHECK_EQ(s.droppedBytes(), 0);
    std::vector<int16_t> out(2 * 48000);
    for (int sec = 0; sec < 4; sec++) {
        if (sec == 3) {
            CHECK_EQ(s.pendingBytes(), 0); // 3 秒で送り直しのバイトをすべて UART に入れた
            send(s, {0xc0, 0, 0x90, 60, 110});
        }
        for (int f = 0; f < 48000; f++) {
            if (async)
                while (s.pump() != 0) {
                }
            s.renderFrames(&out[2 * (size_t)f], 1);
        }
    }
    const double a = rms(out, 0, out.size());
    printf("sc55: resend %zu bytes (%s), dropped %u, rms after %.1f\n", bytes.size(), async ? "async" : "sync",
           s.droppedBytes(), a);
    CHECK_EQ(a > 100, true);
}

// 周波数の変換の標本の番号 (srcCount_) を start の手前に置いても、番号を進めないときと同じ音を作り、H8/532 が進み、
// MIDI が届く。srcCount_ は SRC_REBASE (2^31) を超えると引き戻すので、start が SRC_REBASE なら引き戻す前後を
// 確かめる。start を 2^32 (pos_ の整数部が一周する数) にしても、初めのフレームで 2^31 より大きいとして引き戻すので
// 2^32 には届かない (SRC_REBASE より大きい番号から始めても引き戻すことを確かめる)。
// async なら非同期で作る (音は ASYNC_DELAY 遅れるが、番号を進めないときと比べるので同じになる)
void testRealLongRun(const RomFiles& roms, uint64_t start, bool async) {
    const int frames = 48000;
    auto run = [&](uint64_t advance, std::vector<int16_t>& out, uint64_t& cycles, uint64_t& countAfter) {
        Sc55Synth s;
        CHECK_EQ(s.load(roms.ptr, 48000), Sc55Synth::OK);
        s.setAsync(async);
        if (advance) s.advanceSourceCount(advance);
        gsReset(s);
        out.assign(2 * (size_t)frames, 0);
        const uint64_t c0 = s.mcuCycles();
        for (int f = 0; f < frames; f++) {
            if (f == 24000) send(s, {0xc0, 0, 0x90, 60, 110});
            if (async)
                while (s.pump() != 0) {
                }
            s.renderFrames(&out[2 * (size_t)f], 1);
        }
        if (async) {
            CHECK_EQ(s.underruns(), 0);
            CHECK_EQ(s.skips(), 0);
        }
        cycles = s.mcuCycles() - c0;
        countAfter = s.sourceCount();
    };
    std::vector<int16_t> ref, moved;
    uint64_t c1, c2, n1, n2;
    run(0, ref, c1, n1);
    // load() のあとの番号は毎回同じなので、1 回目の数から、start の 20032 標本ほど手前に置く量 (SRC_RING の倍数) を決める
    // (非同期では、出力より ASYNC_DELAY フレーム先まで作っている)
    const int made = frames + (async ? Sc55Synth::ASYNC_DELAY : 0);
    const uint64_t loaded = n1 - (uint64_t)((double)made * 66206.9 / 48000.0 + 0.5);
    const uint64_t adv = (start - 20032 - loaded) & ~(uint64_t)63;
    run(adv, moved, c2, n2);
    size_t diff = 0;
    for (size_t i = 0; i < ref.size(); i++)
        if (ref[i] != moved[i]) diff++;
    const double a = rms(moved, 2 * 30000, 2 * (size_t)frames);
    printf("sc55: source count from %llu to %llu (%s): %zu samples differ, H8 cycles %llu, rms after note on %.1f\n",
           (unsigned long long)(loaded + adv), (unsigned long long)n2, async ? "async" : "sync", diff,
           (unsigned long long)c2, a);
    CHECK_EQ(diff, 0);
    CHECK_EQ(c2, c1);
    CHECK_EQ(a > 300, true);
    CHECK_EQ(n2 < start, true); // 境目を越えて数を減らした
}

// 非同期で、midiByte() / midiReset() / mixFrame() を呼ぶスレッドと、pump() を呼ぶスレッドを分ける。今のベアメタル版は
// どちらもコア 3 で呼ぶが、pump() を別のコアに移したとき (README の案) にも同じ音になることを確かめる。mixFrame() を呼ぶ側は、pump() が作ったフレームの数 (試験の側で数える) を見て待つので、
// 間に合わないことはなく、同期で作った音より ASYNC_DELAY 遅れるほかは同じになる。-fsanitize=thread のビルドで
// 走らせると、2 つのスレッドの間のデータの競合を調べられる。スレッドの中では確保しない (operator new の数を数えているため)
struct PumpThread {
    Sc55Synth* synth;
    uint32_t made;  // pump() が作ったフレームの数 (__atomic で読み書きする)
    uint32_t stop;  // 1 で終わる
    uint32_t calls; // pump() を呼んだ回数
    static void* run(void* arg) {
        PumpThread* t = (PumpThread*)arg;
        while (!__atomic_load_n(&t->stop, __ATOMIC_ACQUIRE)) {
            const int n = t->synth->pump();
            t->calls++;
            if (n > 0) __atomic_fetch_add(&t->made, (uint32_t)n, __ATOMIC_RELEASE);
            else sched_yield();
        }
        return nullptr;
    }
};

void testRealThreads(const RomFiles& roms) {
    const int frames = 24000;
    auto script = [](Sc55Synth& s, int frame) {
        if (frame == 0) gsReset(s);
        if (frame == 9600) send(s, {0xc0, 19, 0x90, 64, 100, 0x99, 36, 120});
        if (frame == 14000) send(s, {0x80, 64, 0, 0x90, 67, 90});
        if (frame == 18000) s.midiReset();
        if (frame == 18001) send(s, {72, 100}); // ランニングステータス ($90)
    };
    std::vector<int16_t> sync(2 * (size_t)(frames + Sc55Synth::ASYNC_DELAY));
    {
        Sc55Synth s;
        CHECK_EQ(s.load(roms.ptr, 48000), Sc55Synth::OK);
        s.renderFrames(sync.data(), Sc55Synth::ASYNC_DELAY);
        for (int f = 0; f < frames; f++) {
            script(s, f);
            s.renderFrames(&sync[2 * (size_t)(Sc55Synth::ASYNC_DELAY + f)], 1);
        }
    }
    std::vector<int16_t> async(2 * (size_t)(frames + Sc55Synth::ASYNC_DELAY));
    Sc55Synth s;
    CHECK_EQ(s.load(roms.ptr, 48000), Sc55Synth::OK);
    s.setAsync(true);
    PumpThread t = {&s, 0, 0, 0};
    pthread_t th;
    const int created = pthread_create(&th, nullptr, PumpThread::run, &t);
    CHECK_EQ(created, 0);
    if (created != 0) return; // pump() を呼ぶスレッドがないと、下の待ちが終わらない
    // pump() のスレッドが止まったとき (pump() から戻らない場合も含む) に試験が終わらなくならないよう、
    // 1 フレームを待つ時間に上限 (10 秒) を付け、超えたらプロセスごと失敗で終える
    auto nowSec = [] {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
    };
    uint32_t waits = 0;
    bool timedOut = false;
    for (int f = 0; f < frames + Sc55Synth::ASYNC_DELAY && !timedOut; f++) {
        if (f < frames) script(s, f);
        const double start = nowSec();
        while (__atomic_load_n(&t.made, __ATOMIC_ACQUIRE) <= (uint32_t)f) {
            waits++;
            if ((waits & 1023) == 0 && nowSec() - start > 10.0) {
                printf("  sc55: threads: frame %d did not arrive within 10 s\n", f);
                timedOut = true;
                break;
            }
            sched_yield();
        }
        if (!timedOut) s.renderFrames(&async[2 * (size_t)f], 1);
    }
    __atomic_store_n(&t.stop, 1, __ATOMIC_RELEASE);
    if (timedOut) {
        // pump() の中から戻らなくなっていると stop を見ないので、join せずにプロセスごと失敗で終える
        printf("  sc55: threads: timed out; exiting without joining the pump() thread\n");
        fflush(stdout);
        _Exit(1);
    }
    pthread_join(th, nullptr);
    size_t diff = 0;
    for (size_t i = 0; i < sync.size(); i++)
        if (sync[i] != async[i]) diff++;
    const size_t d = Sc55Synth::ASYNC_DELAY;
    const double a = rms(async, 2 * (d + 10000), 2 * (d + 18000));
    printf("sc55: threads: %zu samples differ, underruns %u, skips %u, dropped %u, waits %u, pump calls %u, rms %.1f\n",
           diff, s.underruns(), s.skips(), s.droppedBytes(), waits, t.calls, a);
    CHECK_EQ(diff, 0);
    CHECK_EQ(s.underruns(), 0);
    CHECK_EQ(s.skips(), 0);
    CHECK_EQ(s.droppedBytes(), 0);
    CHECK_EQ(a > 300, true);
}

// ステートを続けて読んだときのように、midiReset() と最大の量の送り直しを gapFrames フレームおきに count 回送る。
// 落としたバイトの数を返す
uint32_t resendBurst(const RomFiles& roms, int count, int gapFrames) {
    Sc55Synth s;
    CHECK_EQ(s.load(roms.ptr, 48000), Sc55Synth::OK);
    x68k::MidiShadow m;
    fillShadow(m);
    const std::vector<uint8_t> bytes = shadowBytes(m);
    std::vector<int16_t> out(2 * (size_t)gapFrames);
    for (int k = 0; k < count; k++) {
        if (k > 0) s.renderFrames(out.data(), gapFrames);
        s.midiReset();
        for (uint8_t b : bytes) s.midiByte(b);
    }
    return s.droppedBytes();
}

// 送り直しを 2 回続けても落とさない。3 回続けると待ち行列を超えて落とす。1 秒おきなら落とさない
// (sc55_synth.h の QUEUE_SIZE の説明)
void testRealResendBurst(const RomFiles& roms) {
    const uint32_t two = resendBurst(roms, 2, 0);
    const uint32_t three = resendBurst(roms, 3, 0);
    const uint32_t threeHalf = resendBurst(roms, 3, 24000);
    const uint32_t threeOne = resendBurst(roms, 3, 48000);
    printf("sc55: resend burst dropped: 2 at once %u, 3 at once %u, 3 every 0.5 s %u, 3 every 1 s %u\n", two, three,
           threeHalf, threeOne);
    CHECK_EQ(two, 0);
    CHECK_EQ(three > 0, true);
    CHECK_EQ(threeOne, 0);
}

void testRealRate(const RomFiles& roms) {
    Sc55Synth s;
    CHECK_EQ(s.load(roms.ptr, 44100), Sc55Synth::OK);
    gsReset(s);
    render(s, 8820);
    send(s, {0xc0, 0, 0x90, 69, 110});
    std::vector<int16_t> on = render(s, 22050);
    const double a = rms(on, 0, on.size());
    printf("sc55: 44.1kHz rms %.1f\n", a);
    CHECK_EQ(a > 300, true);
}

} // namespace

int main() {
    testPick();
    testBadRoms();
    testGarbageRom(1, false);
    testGarbageRom(12345, true);
    testShadowFits();
    testFilter();
    const char* dir = getenv("PIX68K_SC55_ROM");
    if (dir && *dir) {
        RomFiles roms(dir);
        CHECK_EQ(roms.ok, true);
        if (roms.ok) {
            testRealBasic(roms);
            testRealAsyncMatchesSync(roms);
            testRealReset(roms);
            testRealAsyncLate(roms);
            testRealRate(roms);
            testRealResend(roms, false);
            testRealResend(roms, true);
            testRealResendBurst(roms);
            testRealThreads(roms);
            testRealLongRun(roms, Sc55Synth::SRC_REBASE, false);
            testRealLongRun(roms, 1ull << 32, false);
            testRealLongRun(roms, Sc55Synth::SRC_REBASE, true);
            testRealLongRun(roms, 1ull << 32, true);
        }
    } else {
        printf("sc55: PIX68K_SC55_ROM is not set; tests with real ROMs skipped\n");
    }
    return finish();
}
