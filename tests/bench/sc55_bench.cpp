// SC-55mkII の音源 (platform/sc55_synth.h、Nuked-SC55) の速さを測る (テストではない)。
//
//   sc55_bench [-s 秒] [-r 周波数] ROM のディレクトリ
//
// GS リセットのあと、16 のチャネルで音を鳴らし続けながら (少しずつ鳴らし直し、8 分音符ごとにドラムを打つ)、
// -s 秒 (既定 10 秒) の音 (-r、既定 48000Hz) を作るのにかかった時間を出す。
//   load      ROM を読んで起動する (Sc55Synth::BOOT_MS だけ進める) のにかかった時間
//   source    PCM の LSI が出す標本の周波数
//   speed     作った音の長さ / かかった時間 (1.00 で実時間とちょうど同じ。これより小さいと間に合わない)
//   core      実時間の音を作るのに 1 つのコアの何 % を使うか
//   peak      0.1 秒ごとに測ったうちの最も遅いところの speed
// Pi (Linux) では次のようにビルドする (ベアメタル版は Nuked-SC55 のファイルを -O3 でビルドする。-mcpu は Pi 3 なら
// cortex-a53、Pi 5 なら cortex-a76。Nuked-SC55 が知らない書き込みを printf で出すので、grep -v Unknown で除く):
//   g++ -O3 -mcpu=cortex-a53 -std=c++17 -fno-exceptions -fno-rtti -w -Isrc -Isrc/platform/sc55_shim
//       -Iexternal/nuked-sc55/src tests/bench/sc55_bench.cpp src/platform/sc55_synth.cpp
//       external/nuked-sc55/src/*.cpp -o sc55_bench
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <string>
#include <vector>

#include "platform/sc55_synth.h"

namespace {

double now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

class FileSource final : public platform::Sf2Source {
public:
    explicit FileSource(FILE* f) : f_(f) {}
    int read(void* out, unsigned n) override { return f_ ? (int)fread(out, 1, n, f_) : 0; }
    bool skip(unsigned n) override { return f_ && fseek(f_, n, SEEK_CUR) == 0; }

private:
    FILE* f_;
};

void send(platform::Sc55Synth& s, std::initializer_list<int> bytes) {
    for (int b : bytes) s.midiByte((uint8_t)b);
}

} // namespace

int main(int argc, char** argv) {
    double seconds = 10;
    int rate = 48000;
    const char* dir = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) rate = atoi(argv[++i]);
        else dir = argv[i];
    }
    if (dir == nullptr || seconds <= 0) {
        fprintf(stderr, "usage: sc55_bench [-s seconds] [-r rate] ROM-directory\n");
        return 2;
    }
    std::vector<std::string> names;
    std::vector<uint64_t> sizes;
    if (DIR* d = opendir(dir)) {
        while (dirent* e = readdir(d)) {
            std::string path = std::string(dir) + "/" + e->d_name;
            struct stat st;
            if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                names.push_back(e->d_name);
                sizes.push_back((uint64_t)st.st_size);
            }
        }
        closedir(d);
    }
    std::vector<const char*> cnames;
    for (auto& n : names) cnames.push_back(n.c_str());
    int index[platform::Sc55Synth::ROM_COUNT];
    const int missing = platform::Sc55Synth::pickRoms(cnames.data(), sizes.data(), (int)names.size(), index);
    if (missing >= 0) {
        fprintf(stderr, "%s: %s (%s, %u bytes) not found\n", dir, platform::Sc55Synth::romFileName(missing),
                platform::Sc55Synth::romPartName(missing), platform::Sc55Synth::romSize(missing));
        return 1;
    }
    FILE* files[platform::Sc55Synth::ROM_COUNT];
    std::vector<FileSource> sources;
    sources.reserve(platform::Sc55Synth::ROM_COUNT);
    platform::Sf2Source* src[platform::Sc55Synth::ROM_COUNT];
    for (int i = 0; i < platform::Sc55Synth::ROM_COUNT; i++) {
        files[i] = fopen((std::string(dir) + "/" + names[index[i]]).c_str(), "rb");
        sources.emplace_back(files[i]);
        src[i] = &sources.back();
    }
    platform::Sc55Synth synth;
    const double t0 = now();
    const platform::Sc55Synth::Result r = synth.load(src, rate);
    const double t1 = now();
    for (FILE* f : files)
        if (f) fclose(f);
    if (r != platform::Sc55Synth::OK) {
        fprintf(stderr, "load: %s\n", platform::Sc55Synth::resultText(r));
        return 1;
    }
    printf("load      %.0f ms (boot %d ms)\n", (t1 - t0) * 1000, platform::Sc55Synth::BOOT_MS);
    printf("source    %.1f Hz -> %d Hz\n", synth.sourceRate(), rate);

    // GS リセット
    send(synth, {0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7});
    std::vector<int16_t> buf(2 * 4800);
    for (int i = 0; i < 2; i++) synth.renderFrames(buf.data(), rate / 20); // 0.1 秒

    const int programs[16] = {0, 48, 33, 25, 61, 73, 89, 5, 19, 0, 40, 56, 71, 11, 52, 81};
    for (int ch = 0; ch < 16; ch++) {
        if (ch == 9) continue;
        send(synth, {0xc0 | ch, programs[ch]});
        send(synth, {0xb0 | ch, 7, 100});
        send(synth, {0xb0 | ch, 91, 64}); // リバーブ
        send(synth, {0xb0 | ch, 93, 32}); // コーラス
    }
    const int block = rate / 10; // 0.1 秒
    const int blocks = (int)(seconds * 10);
    buf.resize(2 * (size_t)block);
    double total = 0, worst = 1e9;
    uint32_t seed = 1;
    int note[16] = {};
    double sum = 0;
    for (int b = 0; b < blocks; b++) {
        // 0.1 秒ごとに 2 つのチャネルを鳴らし直し、0.25 秒ごとにドラムを打つ (8 分音符 = 120BPM)
        for (int k = 0; k < 2; k++) {
            seed = seed * 1103515245 + 12345;
            int ch = (int)((seed >> 16) % 16);
            if (ch == 9) ch = 0;
            if (note[ch]) send(synth, {0x80 | ch, note[ch], 0});
            note[ch] = 48 + (int)((seed >> 8) % 24);
            send(synth, {0x90 | ch, note[ch], 100});
            send(synth, {0x90 | ch, note[ch] + 4, 90});
            send(synth, {0x90 | ch, note[ch] + 7, 90});
        }
        if (b % 5 == 0 || b % 5 == 2) send(synth, {0x99, (b % 10 == 0) ? 36 : 38, 110, 0x99, 42, 90});
        const double a = now();
        synth.renderFrames(buf.data(), block);
        const double e = now() - a;
        total += e;
        const double sp = 0.1 / e;
        if (sp < worst) worst = sp;
        for (int i = 0; i < 2 * block; i++) sum += (double)buf[i] * buf[i];
    }
    const double speed = seconds / total;
    printf("speed     %.2f x realtime\n", speed);
    printf("core      %.0f %%\n", 100.0 / speed);
    printf("peak      %.2f x realtime (slowest 0.1 s)\n", worst);
    printf("rms       %.0f\n", sqrt(sum / (2.0 * block * blocks)));
    return 0;
}
