// YMF288 (まーきゅりーゆにっとの 2 個) の合成の速さを測る (テストではない)。2 個の FM 12 チャネルと SSG 6 チャネルを
// すべて鳴らし、エミュレートした 1 秒分にかかる時間を出す。比べるために、OPM の 8 チャネルも同じように測る。
// Pi (Linux) では: g++ -O2 -std=c++17 -Isrc tests/bench/opn3_bench.cpp src/x68k/opn3.cpp src/x68k/mercury.cpp src/x68k/opm.cpp
#include <stdio.h>
#include <time.h>

#include "x68k/mercury.h"
#include "x68k/opm.h"

namespace {

double now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// FM 6 チャネル (アルゴリズムはチャネルごとに変える) と SSG 3 チャネルを鳴らす
void setup(x68k::Mercury& m, int chip, bool lfo) {
    const auto w = [&m, chip](int bank, uint8_t r, uint8_t v) { m.opnWriteReg(chip, bank, r, v); };
    w(0, 0x29, 0x83);
    if (lfo) w(0, 0x22, 0x0d);
    for (int c = 0; c < 6; c++) {
        const int bank = c / 3, n = c % 3;
        for (int s = 0; s < 4; s++) {
            const int o = n + s * 4;
            w(bank, (uint8_t)(0x30 + o), (uint8_t)(1 + s));
            w(bank, (uint8_t)(0x40 + o), (uint8_t)(s == 3 ? 10 : 30));
            w(bank, (uint8_t)(0x50 + o), 0x1f);
            w(bank, (uint8_t)(0x60 + o), lfo ? 0x85 : 0x05);
            w(bank, (uint8_t)(0x70 + o), 0x02);
            w(bank, (uint8_t)(0x80 + o), 0x14);
        }
        w(bank, (uint8_t)(0xb0 + n), (uint8_t)(5 << 3 | (c + chip * 2) % 8));
        w(bank, (uint8_t)(0xb4 + n), (uint8_t)(0xc0 | (lfo && (c & 1) ? 0x33 : 0)));
        w(bank, (uint8_t)(0xa4 + n), (uint8_t)((2 + c) << 3 | 3));
        w(bank, (uint8_t)(0xa0 + n), (uint8_t)(0x40 + c * 20));
        w(0, 0x28, (uint8_t)(0xf0 | n | (bank ? 4 : 0)));
    }
    for (int c = 0; c < 3; c++) {
        w(0, (uint8_t)(c * 2), (uint8_t)(100 + c * 37));
        w(0, (uint8_t)(c * 2 + 1), 0);
        w(0, (uint8_t)(8 + c), 12);
    }
    w(0, 0x06, 5);
    w(0, 0x07, 0x30);
}

double runMercury(int chips, bool lfo, int seconds) {
    x68k::Mercury m;
    for (int i = 0; i < chips; i++) setup(m, i, lfo);
    int64_t sum = 0;
    const double t0 = now();
    // Sound と同じに、OPM の 1 サンプル (4MHz の 64 クロック) ごとに進める
    for (int i = 0; i < 62500 * seconds; i++) {
        m.advance(64);
        sum += m.opnLeft();
    }
    const double t = (now() - t0) / seconds;
    printf("YMF288 x %d%s: %.2f ms per emulated second (checksum %lld)\n", chips, lfo ? " with LFO" : "", t * 1000,
           (long long)sum);
    return t;
}

} // namespace

int main() {
    const int seconds = 5;
    runMercury(0, false, seconds);
    runMercury(1, false, seconds);
    runMercury(2, false, seconds);
    runMercury(2, true, seconds);
    {
        x68k::Opm opm;
        const auto w = [&opm](uint8_t r, uint8_t v) {
            opm.write(1, r);
            opm.write(3, v);
        };
        for (int c = 0; c < 8; c++) {
            w((uint8_t)(0x20 + c), (uint8_t)(0xc0 | 5 << 3 | c));
            w((uint8_t)(0x28 + c), (uint8_t)(0x30 + c * 3));
            for (int s = 0; s < 4; s++) {
                const int r = c + s * 8;
                w((uint8_t)(0x40 + r), (uint8_t)(1 + s));
                w((uint8_t)(0x60 + r), (uint8_t)(s == 3 ? 10 : 30));
                w((uint8_t)(0x80 + r), 0x1f);
                w((uint8_t)(0xa0 + r), 0x05);
                w((uint8_t)(0xc0 + r), 0x02);
                w((uint8_t)(0xe0 + r), 0x14);
            }
            w(0x08, (uint8_t)(0x78 | c));
        }
        int32_t out[2];
        int64_t sum = 0;
        const double t0 = now();
        for (int i = 0; i < 62500 * seconds; i++) {
            opm.clockSample(out);
            sum += out[0];
        }
        printf("OPM 8 channels: %.2f ms per emulated second (checksum %lld)\n", (now() - t0) / seconds * 1000, (long long)sum);
    }
    return 0;
}
