// SoundFont の MIDI 音源 (platform/sf2_synth.h、TinySoundFont) の速さを測る (テストではない)。
//
//   midi_bench [-s 秒] [-r 周波数] [-v ボイスの数,...] [SF2 のファイル]
//
// ボイスの数 (同時に鳴らす音の数の上限) ごとに、その数だけ音を鳴らし続けながら (16 チャネル、いろいろなプログラム、
// 少しずつ鳴らし直す。チャネル 10 はドラムを 8 分音符ごとに打つ)、-s 秒 (既定 10 秒) の音を作るのにかかった時間を出す。
//   load    SF2 を読む (標本を float に直す) のにかかった時間
//   active  鳴っていたボイスの数の平均
//   speed   作った音の長さ / かかった時間 (1.00 で実時間とちょうど同じ。ベアメタルでは 1 つのコアを OPM / ADPCM と分け合う)
//   core    実時間の音を作るのに 1 つのコアの何 % を使うか
// SF2 のファイルを渡さなければ、試験用の正弦波の SF2 (tests/unit/sf2_builder.h) を使う。
// Pi (Linux) では次のようにビルドする (ベアメタルと同じ -O2):
//   g++ -O2 -mcpu=cortex-a53 -std=c++17 -fno-exceptions -fno-rtti -Isrc tests/bench/midi_bench.cpp
//       src/platform/sf2_synth.cpp -o midi_bench
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vector>

#include "../unit/sf2_builder.h"
#include "platform/sf2_synth.h"

namespace {

double now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

bool readFile(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    bool ok = n > 0 && fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

void send(platform::Sf2Synth& s, std::initializer_list<int> bytes) {
    for (int b : bytes) s.midiByte((uint8_t)b);
}

} // namespace

int main(int argc, char** argv) {
    double seconds = 10;
    int rate = 48000;
    std::vector<int> voiceCounts = {8, 16, 24, 32, 48, 64};
    const char* path = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v") && i + 1 < argc) {
            voiceCounts.clear();
            for (char* p = argv[++i]; *p;) {
                voiceCounts.push_back((int)strtol(p, &p, 10));
                if (*p == ',') p++;
                else break;
            }
        } else if (argv[i][0] != '-') path = argv[i];
        else {
            fprintf(stderr, "usage: %s [-s seconds] [-r rate] [-v n,n,...] [file.sf2]\n", argv[0]);
            return 2;
        }
    }
    if (seconds <= 0 || rate <= 0) {
        fprintf(stderr, "bad -s or -r\n");
        return 2;
    }
    std::vector<uint8_t> sf2;
    if (path) {
        if (!readFile(path, sf2)) {
            fprintf(stderr, "cannot read %s\n", path);
            return 1;
        }
    } else {
        sf2 = sf2test::makeSineSf2();
        path = "(sine test SoundFont)";
    }
    printf("%s, %.1f MB, %d Hz, %.0f s of audio per run\n", path, sf2.size() / 1048576.0, rate, seconds);
    // GM のプログラム (ピアノ、ストリングス、ベース、ギター、ブラス、オルガン、パッド、フルート ...)
    static const int kPrograms[15] = {0, 48, 33, 25, 61, 16, 89, 73, 4, 52, 40, 65, 81, 11, 56};
    const int frames = (int)(seconds * rate);
    std::vector<int16_t> out(2 * (size_t)rate / 100);
    for (int voices : voiceCounts) {
        platform::Sf2Synth s;
        const double t0 = now();
        if (!s.load(sf2.data(), sf2.size(), rate, voices)) {
            fprintf(stderr, "cannot load the SoundFont\n");
            return 1;
        }
        const double load = now() - t0;
        for (int ch = 0; ch < 16; ch++)
            if (ch != 9) send(s, {0xc0 | ch, kPrograms[ch < 9 ? ch : ch - 1]});
        // ドラム以外のチャネルに、ボイスの数の 3/4 ほどの音を鳴らし続ける (残りはドラムと鳴らし直しの重なり)
        const int held = voices * 3 / 4 > 1 ? voices * 3 / 4 : 1;
        std::vector<int> keys(held);
        for (int i = 0; i < held; i++) {
            const int ch = i % 15 < 9 ? i % 15 : i % 15 + 1;
            keys[i] = 36 + (i * 7) % 48;
            send(s, {0x90 | ch, keys[i], 90});
        }
        double active = 0;
        int samples = 0, next = 0, tick = 0;
        const int step = rate / 100; // 10ms ずつ作る
        const double t1 = now();
        for (int done = 0; done < frames; done += step) {
            // 8 分音符 (120 BPM で 250ms) ごとにドラムを打ち、鳴っている音を 2 つ鳴らし直す
            if (done >= next) {
                next += rate / 4;
                send(s, {0x99, tick & 1 ? 38 : 36, 110, 0x99, 42, 80});
                for (int k = 0; k < 2; k++) {
                    const int i = (tick * 2 + k) % held;
                    const int ch = i % 15 < 9 ? i % 15 : i % 15 + 1;
                    send(s, {0x80 | ch, keys[i], 0});
                    keys[i] = 36 + (keys[i] - 36 + 5) % 48;
                    send(s, {0x90 | ch, keys[i], 90});
                }
                tick++;
            }
            s.renderFrames(out.data(), step);
            active += s.activeVoices();
            samples++;
        }
        const double t = now() - t1;
        const double speed = seconds / t;
        printf("voices %3d  load %6.2f s  active %5.1f  speed %6.2fx  core %5.1f %%\n", voices, load, active / samples,
               speed, 100.0 / speed);
    }
    return 0;
}
