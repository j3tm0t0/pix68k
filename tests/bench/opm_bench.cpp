// OPM の合成の速さを測る (テストではない)。8 チャネルすべてを鳴らし (一部は LFO の PM / AM つき)、
// エミュレートした 1 秒分 (62500 サンプル) にかかる時間を出す。Sound (ADPCM との混合、フィルタ、48kHz への変換) 込みも測る。
// Pi (Linux) では: g++ -O2 -std=c++17 -Isrc tests/bench/opm_bench.cpp src/x68k/opm.cpp src/x68k/adpcm.cpp src/x68k/sound.cpp
#include <stdio.h>
#include <time.h>

#include "x68k/sound.h"

namespace {

double now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void w(x68k::Opm& opm, uint8_t r, uint8_t v) {
    opm.write(1, r);
    opm.write(3, v);
}

// 8 チャネルに音色を設定してキーオンする (アルゴリズムはチャネルごとに変える)
void setup(x68k::Opm& opm, bool lfo) {
    if (lfo) {
        w(opm, 0x18, 0xc8);
        w(opm, 0x19, 0x80 | 40);
        w(opm, 0x19, 30);
        w(opm, 0x1b, 2);
    }
    for (int c = 0; c < 8; c++) {
        w(opm, (uint8_t)(0x20 + c), (uint8_t)(0xc0 | 5 << 3 | c));
        w(opm, (uint8_t)(0x28 + c), (uint8_t)(0x30 + c * 3));
        w(opm, (uint8_t)(0x38 + c), lfo && (c & 1) ? 0x31 : 0);
        for (int s = 0; s < 4; s++) {
            int r = c + s * 8;
            w(opm, (uint8_t)(0x40 + r), (uint8_t)(1 + s));
            w(opm, (uint8_t)(0x60 + r), (uint8_t)(s == 3 ? 10 : 30));
            w(opm, (uint8_t)(0x80 + r), 0x1f);
            w(opm, (uint8_t)(0xa0 + r), lfo ? 0x85 : 0x05);
            w(opm, (uint8_t)(0xc0 + r), 0x02);
            w(opm, (uint8_t)(0xe0 + r), 0x14);
        }
        w(opm, 0x08, (uint8_t)(0x78 | c));
    }
}

} // namespace

int main() {
    const int seconds = 5;
    {
        x68k::Opm opm;
        setup(opm, false);
        int32_t out[2];
        int64_t sum = 0;
        double t0 = now();
        for (int i = 0; i < 62500 * seconds; i++) {
            opm.clockSample(out);
            sum += out[0];
        }
        double t = now() - t0;
        printf("OPM 8ch (no LFO): %.1f ms per emulated second (checksum %lld)\n", t * 1000 / seconds, (long long)sum);
    }
    {
        x68k::Opm opm;
        setup(opm, true);
        int32_t out[2];
        int64_t sum = 0;
        double t0 = now();
        for (int i = 0; i < 62500 * seconds; i++) {
            opm.clockSample(out);
            sum += out[0];
        }
        double t = now() - t0;
        printf("OPM 8ch (LFO PM/AM on 4ch): %.1f ms per emulated second (checksum %lld)\n", t * 1000 / seconds,
               (long long)sum);
    }
    {
        x68k::Opm opm;
        int32_t out[2];
        double t0 = now();
        for (int i = 0; i < 62500 * seconds; i++) opm.clockSample(out);
        double t = now() - t0;
        printf("OPM silent: %.1f ms per emulated second\n", t * 1000 / seconds);
    }
    {
        static x68k::Sound sound;
        sound.setOutputRate(48000);
        setup(sound.opm(), true);
        static int16_t buf[2 * 4096];
        double t0 = now();
        for (int i = 0; i < 62500 * seconds; i++) {
            sound.advance(64);
            if ((i & 1023) == 0) sound.ring().drain(buf, 4096);
        }
        double t = now() - t0;
        printf("Sound (OPM 8ch + mix + 48kHz): %.1f ms per emulated second\n", t * 1000 / seconds);
    }
    return 0;
}
