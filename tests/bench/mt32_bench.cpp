// MT-32 の音源 (platform/mt32_synth.h、Munt) の速さを測る (テストではない)。
//
//   mt32_bench [-s 秒] CONTROL の ROM PCM の ROM
//
// 8 つのパートとリズムのパートで音を鳴らし続けながら (少しずつ鳴らし直し、8 分音符ごとにドラムを打つ)、-s 秒 (既定 10 秒) の
// 48kHz の音を作るのにかかった時間を出す。パーシャル (MT-32 の発音の単位、32 個) がほぼすべて埋まる重さにする。
//   open      ROM を調べて Munt を開くのにかかった時間
//   partials  鳴っていたパーシャルの数の平均 (最大 32)
//   speed     作った音の長さ / かかった時間 (1.00 で実時間とちょうど同じ)
//   core      実時間の音を作るのに 1 つのコアの何 % を使うか
// 次の 2 つを比べる:
//   accurate  pix68k のやり方。Munt の AnalogOutputMode_ACCURATE で 48kHz をそのまま出す (変換なし)
//   coarse    Munt の既定 (AnalogOutputMode_COARSE、32kHz) を SampleRateConverter (GOOD) で 48kHz に変える (比べるため)
// Pi (Linux) では次のようにビルドする (ベアメタルと同じ -O2):
//   g++ -O2 -mcpu=cortex-a53 -std=c++17 -fno-exceptions -w -Isrc -Iexternal/munt/mt32emu/src -Iexternal/munt/config
//       -DMT32EMU_WITH_INTERNAL_RESAMPLER=1 tests/bench/mt32_bench.cpp src/platform/mt32_synth.cpp
//       external/munt/mt32emu/src/*.cpp external/munt/mt32emu/src/sha1/*.cpp
//       external/munt/mt32emu/src/srchelper/InternalResampler.cpp external/munt/mt32emu/src/srchelper/srctools/src/*.cpp
//       -o mt32_bench
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vector>

#include "File.h"
#include "ROMInfo.h"
#include "SampleRateConverter.h"
#include "Synth.h"
#include "platform/mt32_synth.h"

namespace {

double now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

uint8_t* readFile(const char* path, size_t& size) {
    size = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* p = new uint8_t[n > 0 ? (size_t)n : 1];
    if (n <= 0 || fread(p, 1, (size_t)n, f) != (size_t)n) {
        delete[] p;
        fclose(f);
        return nullptr;
    }
    fclose(f);
    size = (size_t)n;
    return p;
}

// 鳴らし方 (どちらのやり方でも同じ)。send(msg) で 3 バイトのメッセージを送る
template <class Send> void startNotes(Send send, int* keys) {
    for (int part = 0; part < 8; part++) {
        // パート 1-8 (チャネル 2-9) に 2 音ずつ
        for (int k = 0; k < 2; k++) {
            keys[part * 2 + k] = 40 + part * 4 + k * 7;
            send(0x91 + part, keys[part * 2 + k], 100);
        }
    }
}
template <class Send> void tickNotes(Send send, int* keys, int tick) {
    send(0x99, tick & 1 ? 38 : 36, 110); // リズムのパート (チャネル 10)
    send(0x99, 42, 90);
    const int i = tick % 16;
    const int part = i / 2;
    send(0x81 + part, keys[i], 0);
    keys[i] = 40 + (keys[i] - 40 + 5) % 36;
    send(0x91 + part, keys[i], 100);
}

} // namespace

int main(int argc, char** argv) {
    double seconds = 10;
    const char* ctrlPath = nullptr;
    const char* pcmPath = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (argv[i][0] != '-' && !ctrlPath) ctrlPath = argv[i];
        else if (argv[i][0] != '-' && !pcmPath) pcmPath = argv[i];
        else ctrlPath = nullptr, pcmPath = nullptr, i = argc;
    }
    if (!ctrlPath || !pcmPath || seconds <= 0) {
        fprintf(stderr, "usage: %s [-s seconds] control.rom pcm.rom\n", argv[0]);
        return 2;
    }
    const int rate = 48000;
    const int frames = (int)(seconds * rate);
    const int step = rate / 100; // 10ms ずつ作る
    std::vector<int16_t> out(2 * (size_t)step);

    // accurate: pix68k の Mt32Synth
    {
        size_t cn, pn;
        uint8_t* c = readFile(ctrlPath, cn);
        uint8_t* p = readFile(pcmPath, pn);
        platform::Mt32Synth s;
        const double t0 = now();
        const platform::Mt32Synth::Result r = s.load(c, cn, p, pn, rate);
        const double open = now() - t0;
        if (r != platform::Mt32Synth::OK) {
            fprintf(stderr, "cannot load the ROMs: %s\n", platform::Mt32Synth::resultText(r));
            return 1;
        }
        printf("%s, %s, %d Hz, %.0f s of audio per run\n", s.controlName(), s.pcmName(), rate, seconds);
        auto send = [&](int a, int b, int c2) {
            s.midiByte((uint8_t)a);
            s.midiByte((uint8_t)b);
            s.midiByte((uint8_t)c2);
        };
        int keys[16];
        startNotes(send, keys);
        double partials = 0;
        int samples = 0, next = 0, tick = 0;
        const double t1 = now();
        for (int done = 0; done < frames; done += step) {
            if (done >= next) {
                next += rate / 4;
                tickNotes(send, keys, tick++);
            }
            s.renderFrames(out.data(), step);
            partials += s.activePartials();
            samples++;
        }
        const double speed = seconds / (now() - t1);
        printf("accurate  open %5.3f s  partials %5.1f  speed %6.2fx  core %5.1f %%\n", open, partials / samples, speed,
               100.0 / speed);
    }

    // coarse: Munt の既定のアナログの出力と、Munt の変換
    {
        size_t cn, pn;
        uint8_t* c = readFile(ctrlPath, cn);
        uint8_t* p = readFile(pcmPath, pn);
        MT32Emu::ArrayFile cf(c, cn), pf(p, pn);
        const MT32Emu::ROMImage* ci = MT32Emu::ROMImage::makeROMImage(&cf);
        const MT32Emu::ROMImage* pi = MT32Emu::ROMImage::makeROMImage(&pf);
        struct Quiet : MT32Emu::ReportHandler {
            void printDebug(const char*, va_list) override {}
            void showLCDMessage(const char*) override {}
        } quiet;
        MT32Emu::Synth* synth = new MT32Emu::Synth(&quiet);
        const double t0 = now();
        if (!synth->open(*ci, *pi, MT32Emu::AnalogOutputMode_COARSE)) {
            fprintf(stderr, "cannot open Munt\n");
            return 1;
        }
        MT32Emu::SampleRateConverter src(*synth, rate, MT32Emu::SamplerateConversionQuality_GOOD);
        const double open = now() - t0;
        auto send = [&](int a, int b, int c2) { synth->playMsg((MT32Emu::Bit32u)(a | b << 8 | c2 << 16)); };
        int keys[16];
        startNotes(send, keys);
        double partials = 0;
        int samples = 0, next = 0, tick = 0;
        MT32Emu::PartialState states[64];
        const double t1 = now();
        for (int done = 0; done < frames; done += step) {
            if (done >= next) {
                next += rate / 4;
                tickNotes(send, keys, tick++);
            }
            src.getOutputSamples(out.data(), step);
            synth->getPartialStates(states);
            for (unsigned i = 0; i < synth->getPartialCount(); i++) partials += states[i] != MT32Emu::PartialState_INACTIVE;
            samples++;
        }
        const double speed = seconds / (now() - t1);
        printf("coarse    open %5.3f s  partials %5.1f  speed %6.2fx  core %5.1f %%\n", open, partials / samples, speed,
               100.0 / speed);
        synth->close();
        delete synth;
        MT32Emu::ROMImage::freeROMImage(ci);
        MT32Emu::ROMImage::freeROMImage(pi);
        delete[] c;
        delete[] p;
    }
    return 0;
}
