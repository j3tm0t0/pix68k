// YMF288 (x68k/opn3.h) のテスト: タイマーとフラグと割り込み、動作モードと読み出し、音程、音量、エンベロープ、
// アルゴリズム、LFO、チャネル 3 のスロットごとの音程、SSG、リズム
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "platform/opn_rhythm.h"
#include "x68k/opn3.h"

namespace {

int failures = 0;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("  %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            failures++;                                            \
        }                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        long long va_ = (long long)(a), vb_ = (long long)(b);                                       \
        if (va_ != vb_) {                                                                           \
            printf("  %s:%d: %s == %s: got %lld, want %lld\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
            failures++;                                                                             \
        }                                                                                           \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                            \
    do {                                                                                                 \
        double va_ = (double)(a), vb_ = (double)(b);                                                     \
        if (va_ < vb_ - (tol) || va_ > vb_ + (tol)) {                                                    \
            printf("  %s:%d: %s ~= %s: got %g, want %g +- %g\n", __FILE__, __LINE__, #a, #b, va_, vb_, \
                   (double)(tol));                                                                       \
            failures++;                                                                                  \
        }                                                                                                \
    } while (0)

using x68k::Opn3;

const double kRate = 8000000.0 / 144; // サンプリング周波数

struct Wave {
    std::vector<int32_t> l, r;
};

Wave render(Opn3& opn, int samples) {
    Wave w;
    for (int i = 0; i < samples; i++) {
        int32_t out[2] = {0, 0};
        opn.clockSample(out);
        w.l.push_back(out[0]);
        w.r.push_back(out[1]);
    }
    return w;
}

int peak(const std::vector<int32_t>& v, size_t from = 0) {
    int p = 0;
    for (size_t i = from; i < v.size(); i++) p = abs(v[i]) > p ? abs(v[i]) : p;
    return p;
}

// 負から正への変わり目の数から周波数を求める (変わり目の位置は補間する)
double frequency(const std::vector<int32_t>& v, double center = 0) {
    double first = -1, last = -1;
    int count = 0;
    for (size_t i = 1; i < v.size(); i++) {
        const double a = v[i - 1] - center, b = v[i] - center;
        if (a < 0 && b >= 0) {
            const double t = (double)(i - 1) + a / (a - b);
            if (first < 0) first = t;
            last = t;
            count++;
        }
    }
    if (count < 2) return 0;
    return (count - 1) * kRate / (last - first);
}

// チャネル c (0..5) を、S4 だけが鳴る設定 (アルゴリズム 7、ほかのスロットは TL = 127) にする
void setupCarrier(Opn3& opn, int c, int fnum, int block) {
    const int bank = c / 3, n = c % 3;
    opn.writeReg(0, 0x29, 0x83); // 6 チャネル
    for (int s = 0; s < 4; s++) {
        const int o = n + s * 4;
        opn.writeReg(bank, (uint8_t)(0x30 + o), 0x01);                 // DT 0、MUL 1
        opn.writeReg(bank, (uint8_t)(0x40 + o), s == 3 ? 0x00 : 0x7f); // TL
        opn.writeReg(bank, (uint8_t)(0x50 + o), 0x1f);                 // AR 31
        opn.writeReg(bank, (uint8_t)(0x60 + o), 0x00);
        opn.writeReg(bank, (uint8_t)(0x70 + o), 0x00);
        opn.writeReg(bank, (uint8_t)(0x80 + o), 0x0f); // SL 0、RR 15
    }
    opn.writeReg(bank, (uint8_t)(0xb0 + n), 0x07);
    opn.writeReg(bank, (uint8_t)(0xb4 + n), 0xc0);
    opn.writeReg(bank, (uint8_t)(0xa4 + n), (uint8_t)(block << 3 | fnum >> 8));
    opn.writeReg(bank, (uint8_t)(0xa0 + n), (uint8_t)fnum);
}

void keyOn(Opn3& opn, int c, int slots = 0xf) { opn.writeReg(0, 0x28, (uint8_t)(slots << 4 | (c % 3) | (c >= 3 ? 4 : 0))); }
void keyOff(Opn3& opn, int c) { keyOn(opn, c, 0); }

void testTimers() {
    printf("timers, flags and IRQ\n");
    Opn3 opn;
    CHECK_EQ(opn.status(), 0);
    CHECK_EQ(opn.irq(), false);
    CHECK_EQ(opn.clocksToIrq(), 0);
    // タイマー A: 144 x (1024 - NA) マスタークロック = 72 x (1024 - NA) の 4MHz のクロック (8MHz で 1 つ 18 マイクロ秒)
    opn.writeReg(0, 0x24, 1000 >> 2);
    opn.writeReg(0, 0x25, 1000 & 3);
    opn.writeReg(0, 0x27, 0x05); // 動かす、フラグを立てる
    CHECK_EQ(opn.clocksToIrq(), 72 * 24);
    opn.advanceTimers(72 * 24 - 1);
    CHECK_EQ(opn.status(), 0);
    CHECK_EQ(opn.clocksToIrq(), 1);
    opn.advanceTimers(1);
    CHECK_EQ(opn.status(), 1);
    CHECK_EQ(opn.irq(), true);
    CHECK_EQ(opn.clocksToIrq(), 0);
    // フラグを消すと /IRQ も消え、次の周期でまた立つ
    opn.writeReg(0, 0x27, 0x15);
    CHECK_EQ(opn.status(), 0);
    CHECK_EQ(opn.irq(), false);
    CHECK_EQ(opn.reg(0, 0x27), 0x05); // リセットのビットは 0 に読める
    CHECK_EQ(opn.clocksToIrq(), 72 * 24);
    opn.advanceTimers(72 * 24 * 3); // まとめて進めても、端数は次の周期に持ち越す
    CHECK_EQ(opn.status(), 1);
    opn.writeReg(0, 0x27, 0x15);
    CHECK_EQ(opn.clocksToIrq(), 72 * 24);
    // フラグを立てない設定 (bit2 = 0) では、あふれても立たない
    opn.writeReg(0, 0x27, 0x01);
    opn.advanceTimers(72 * 24 * 2);
    CHECK_EQ(opn.status(), 0);
    CHECK_EQ(opn.clocksToIrq(), 0);
    opn.writeReg(0, 0x27, 0x00);

    // タイマー B: 2304 x (256 - NB) マスタークロック (8MHz で 1 つ 288 マイクロ秒)
    opn.writeReg(0, 0x26, 200);
    opn.writeReg(0, 0x27, 0x0a);
    CHECK_EQ(opn.clocksToIrq(), 1152 * 56);
    opn.advanceTimers(1152 * 56 - 1);
    CHECK_EQ(opn.status(), 0);
    opn.advanceTimers(1);
    CHECK_EQ(opn.status(), 2);
    CHECK_EQ(opn.irq(), true);
    // $29 で割り込みを止めると、フラグは立ったままで /IRQ だけ消える
    opn.writeReg(0, 0x29, 0x81);
    CHECK_EQ(opn.status(), 2);
    CHECK_EQ(opn.irq(), false);
    opn.writeReg(0, 0x29, 0x83);
    CHECK_EQ(opn.irq(), true);
    // $110 のマスク: フラグが 0 に見え、/IRQ も出ない。IRQ RESET (bit7) はフラグを消し、マスクは変えない
    opn.writeReg(1, 0x10, 0x02);
    CHECK_EQ(opn.status(), 0);
    CHECK_EQ(opn.irq(), false);
    opn.writeReg(1, 0x10, 0x00);
    CHECK_EQ(opn.status(), 2);
    opn.writeReg(1, 0x10, 0x83);
    CHECK_EQ(opn.status(), 0);
    opn.advanceTimers(1152 * 56);
    CHECK_EQ(opn.status(), 2);
    // 止めると残りは消え、動かし直すと最初から数える
    opn.writeReg(0, 0x27, 0x28);
    CHECK_EQ(opn.status(), 0);
    CHECK_EQ(opn.clocksToIrq(), 0);
    opn.writeReg(0, 0x27, 0x0a);
    CHECK_EQ(opn.clocksToIrq(), 1152 * 56);
    CHECK(opn.stateOk());
}

void testModeAndRead() {
    printf("mode, device ID, register read, busy\n");
    Opn3 opn;
    // リセットのあとは YM2608B 互換モード: 識別コード $01、読めるのは SSG のレジスタだけ、BUSY はデータのあと 192 クロック
    CHECK_EQ(opn.newMode(), false);
    opn.writeAddress(0, 0xff);
    CHECK_EQ(opn.readData(), 0x01);
    opn.writeReg(0, 0x00, 0x34);
    opn.writeReg(0, 0x01, 0xf2);
    opn.writeReg(0, 0xa4, 0x22);
    opn.writeAddress(0, 0x00);
    CHECK_EQ(opn.readData(), 0x34);
    opn.writeAddress(0, 0x01);
    CHECK_EQ(opn.readData(), 0x02);
    opn.writeAddress(0, 0xa4);
    CHECK_EQ(opn.readData(), 0xa4); // バスにあった値 (アドレス)
    CHECK_EQ(opn.busyClocks(false, 0, 0x30), 0);
    CHECK_EQ(opn.busyClocks(true, 0, 0x30), 192);
    // $20 の NEW で YMF288 モード: 識別コード $02、すべてのレジスタが読める、BUSY は短い
    opn.writeReg(0, 0x20, 0x02);
    CHECK_EQ(opn.newMode(), true);
    opn.writeAddress(0, 0xff);
    CHECK_EQ(opn.readData(), 0x02);
    opn.writeAddress(0, 0xa4);
    CHECK_EQ(opn.readData(), 0x22);
    opn.writeReg(1, 0x45, 0x99);
    opn.writeAddress(1, 0x45);
    CHECK_EQ(opn.readData(), 0x19); // TL は 7 ビット
    opn.writeAddress(0, 0x45);
    CHECK_EQ(opn.readData(), 0x00); // A1 = 0 の側は別のレジスタ
    opn.writeAddress(0, 0x0e);
    CHECK_EQ(opn.readData(), 0xff); // I/O ポートはない
    opn.writeReg(0, 0x28, 0xf0);
    opn.writeAddress(0, 0x28);
    CHECK_EQ(opn.readData(), 0x00); // Slot と CH は 0 に読める
    opn.writeAddress(0, 0x29);
    CHECK_EQ(opn.readData(), 0x03); // IRQ ENABLE の初期値
    CHECK_EQ(opn.busyClocks(false, 0, 0x30), 15);
    CHECK_EQ(opn.busyClocks(true, 0, 0x30), 15);
    CHECK_EQ(opn.busyClocks(true, 0, 0x10), 180);
    CHECK_EQ(opn.busyClocks(true, 0, 0x28), 192);
    CHECK_EQ(opn.busyClocks(true, 1, 0x28), 15);
    opn.reset();
    CHECK_EQ(opn.newMode(), false);
}

void testPitchAndLevel() {
    printf("pitch and level\n");
    // マニュアルの例: 8MHz、A4 (440Hz) は Block 4 で F-Number 1038
    {
        Opn3 opn;
        setupCarrier(opn, 0, 1038, 4);
        keyOn(opn, 0);
        const Wave w = render(opn, 60000);
        CHECK_NEAR(frequency(w.l), 440.0, 0.2);
        CHECK_NEAR(peak(w.l, 1000), 8168, 8);
        CHECK(w.l == w.r);
    }
    // Block を 1 つ下げると半分、MUL 3 で 3 倍、MUL 0 で半分
    {
        Opn3 opn;
        setupCarrier(opn, 1, 1038, 3);
        keyOn(opn, 1);
        CHECK_NEAR(frequency(render(opn, 60000).l), 220.0, 0.2);
        opn.writeReg(0, 0x3d, 0x03);
        CHECK_NEAR(frequency(render(opn, 60000).l), 660.0, 0.3);
        opn.writeReg(0, 0x3d, 0x00);
        CHECK_NEAR(frequency(render(opn, 60000).l), 110.0, 0.2);
    }
    // $A4 は $A0 を書くまで入らない
    {
        Opn3 opn;
        setupCarrier(opn, 0, 1038, 4);
        keyOn(opn, 0);
        opn.writeReg(0, 0xa4, (uint8_t)(5 << 3 | 1038 >> 8));
        CHECK_NEAR(frequency(render(opn, 30000).l), 440.0, 0.3);
        opn.writeReg(0, 0xa0, (uint8_t)1038);
        CHECK_NEAR(frequency(render(opn, 30000).l), 880.0, 0.6);
    }
    // Detune: Block 4、Note 3 (F-Number 1038 は F11 = 1、F10-F8 = 0 なので Note 2) の表の値だけずれる。
    // キーコード 18 の FD = 3 は 9 x 0.053Hz
    {
        Opn3 opn;
        setupCarrier(opn, 0, 1038, 4);
        opn.writeReg(0, 0x3c, 0x31); // DT 3
        keyOn(opn, 0);
        const double up = frequency(render(opn, 200000).l);
        opn.writeReg(0, 0x3c, 0x71); // DT 7 (-3)
        const double down = frequency(render(opn, 200000).l);
        CHECK_NEAR(up - 440.0, 9 * 0.053, 0.05);
        CHECK_NEAR(440.0 - down, 9 * 0.053, 0.05);
    }
    // TL は 0.75dB 刻み: 8 で -6dB、16 で -12dB
    {
        Opn3 opn;
        setupCarrier(opn, 0, 1038, 4);
        opn.writeReg(0, 0x4c, 8);
        keyOn(opn, 0);
        CHECK_NEAR(peak(render(opn, 3000).l, 1000), 8168 / 2, 30);
        opn.writeReg(0, 0x4c, 16);
        CHECK_NEAR(peak(render(opn, 3000).l, 1000), 8168 / 4, 20);
        opn.writeReg(0, 0x4c, 127);
        CHECK_EQ(peak(render(opn, 3000).l, 1000), 0);
    }
    // L / R の出力選択
    {
        Opn3 opn;
        setupCarrier(opn, 0, 1038, 4);
        opn.writeReg(0, 0xb4, 0x80);
        keyOn(opn, 0);
        Wave w = render(opn, 3000);
        CHECK(peak(w.l) > 8000);
        CHECK_EQ(peak(w.r), 0);
        opn.writeReg(0, 0xb4, 0x40);
        w = render(opn, 3000);
        CHECK_EQ(peak(w.l), 0);
        CHECK(peak(w.r) > 8000);
    }
    // $29 の SCH が 0 なら、チャネル 4-6 は鳴らない
    {
        Opn3 opn;
        setupCarrier(opn, 4, 1038, 4);
        keyOn(opn, 4);
        CHECK(peak(render(opn, 3000).l) > 8000);
        opn.writeReg(0, 0x29, 0x03);
        CHECK_EQ(peak(render(opn, 3000).l), 0);
        opn.writeReg(0, 0x29, 0x83);
        CHECK(peak(render(opn, 3000).l) > 8000);
        CHECK(opn.stateOk());
    }
}

// キーオンから、S4 の減衰量が level になるまでのサンプル数
int samplesToLevel(Opn3& opn, int c, int level, bool rising, int limit) {
    for (int n = 1; n <= limit; n++) {
        int32_t out[2] = {0, 0};
        opn.clockSample(out);
        const int e = opn.envelope(c, 3);
        if (rising ? e >= level : e <= level) return n;
    }
    return -1;
}

void testEnvelope() {
    printf("envelope\n");
    // AR 31 はキーオンですぐ最大。DR を 2 上げる (レートを 4 上げる) と、減衰の速さは 2 倍
    int decay[2];
    for (int i = 0; i < 2; i++) {
        Opn3 opn;
        setupCarrier(opn, 0, 1038, 4);
        opn.writeReg(0, 0x6c, (uint8_t)(10 + i * 2)); // DR
        opn.writeReg(0, 0x8c, 0xff);                  // SL 15、RR 15
        keyOn(opn, 0);
        int32_t out[2] = {0, 0};
        opn.clockSample(out);
        CHECK_EQ(opn.envelope(0, 3), 0);
        decay[i] = samplesToLevel(opn, 0, 512, true, 2000000);
        CHECK(decay[i] > 0);
    }
    CHECK_NEAR((double)decay[0] / decay[1], 2.0, 0.05);
    // レート 40 (DR 20、KS 0、キーコードによる補正は KS 0 では Block 4 で 2): 1 回の刻み (3 サンプル) の半分で 1 ずつ増える
    // レート 42 なら、3 サンプル x 2^(11 - 10) ごとに 8 回のうち 6 回、1 増える
    {
        Opn3 opn;
        setupCarrier(opn, 0, 1038, 4);
        opn.writeReg(0, 0x6c, 20);
        opn.writeReg(0, 0x8c, 0xff);
        keyOn(opn, 0);
        const int n = samplesToLevel(opn, 0, 600, true, 100000);
        CHECK_NEAR(n, 600 * 3 * 2 * 8 / 6, 40);
    }
    // SL: 減衰が SL x 3dB (32 x 0.09375dB) まで進んだら SR に移る。SR = 0 ならそこで止まる
    {
        Opn3 opn;
        setupCarrier(opn, 0, 1038, 4);
        opn.writeReg(0, 0x6c, 28);
        opn.writeReg(0, 0x8c, 0x4f); // SL 4
        keyOn(opn, 0);
        render(opn, 20000);
        CHECK_EQ(opn.envelope(0, 3), 4 * 32);
        // キーオフでリリース。鳴り終わると silent() になる
        CHECK_EQ(opn.silent(), false);
        keyOff(opn, 0);
        render(opn, 20000);
        CHECK_EQ(opn.envelope(0, 3), 1023);
        CHECK_EQ(opn.silent(), true);
        // 鳴り終わったあとも、キーオンでまた鳴る
        keyOn(opn, 0);
        CHECK(peak(render(opn, 3000).l) > 4000);
    }
    // アタック: AR が小さいほど遅い。途中の減衰量は 1023 から 0 へ向かって減る
    {
        int attack[2];
        for (int i = 0; i < 2; i++) {
            Opn3 opn;
            setupCarrier(opn, 0, 1038, 4);
            opn.writeReg(0, 0x5c, (uint8_t)(12 + i * 2));
            keyOn(opn, 0);
            attack[i] = samplesToLevel(opn, 0, 0, false, 2000000);
            CHECK(attack[i] > 100);
        }
        CHECK_NEAR((double)attack[0] / attack[1], 2.0, 0.1);
    }
    // KS: 高い音ほどレートが上がる (KS 3 では、Block 7 と Block 0 でレートが 28 違う)
    {
        int n[2];
        for (int i = 0; i < 2; i++) {
            Opn3 opn;
            setupCarrier(opn, 0, 600, i ? 7 : 0);
            opn.writeReg(0, 0x5c, 0xdf); // KS 3、AR 31
            opn.writeReg(0, 0x6c, 8);
            opn.writeReg(0, 0x8c, 0xff);
            keyOn(opn, 0);
            n[i] = samplesToLevel(opn, 0, 512, true, 4000000);
        }
        CHECK_NEAR((double)n[0] / n[1], 128.0, 8.0);
    }
    // SSG 型エンベロープ ($90 = 8): 減衰が $200 に届くたびにアタックからやり直す (のこぎり波の形で繰り返す)
    {
        Opn3 opn;
        setupCarrier(opn, 0, 1038, 4);
        opn.writeReg(0, 0x6c, 24);
        opn.writeReg(0, 0x8c, 0xff);
        opn.writeReg(0, 0x9c, 0x08);
        keyOn(opn, 0);
        int restarts = 0, prev = 0, top = 0;
        for (int i = 0; i < 60000; i++) {
            int32_t out[2] = {0, 0};
            opn.clockSample(out);
            const int e = opn.envelope(0, 3);
            if (e > top) top = e;
            if (e < prev - 256) restarts++;
            prev = e;
        }
        CHECK(restarts >= 3);
        CHECK(top >= 0x200 && top < 0x210);
        // $90 = $0B (止める、裏返す): $200 に届いたら最大の音量で止まる
        opn.writeReg(0, 0x9c, 0x0b);
        keyOff(opn, 0);
        render(opn, 3000);
        keyOn(opn, 0);
        render(opn, 60000);
        CHECK(peak(render(opn, 3000).l) > 8000);
        CHECK(opn.stateOk());
    }
}

void testAlgorithms() {
    printf("algorithms and feedback\n");
    // アルゴリズム 7: 4 つのスロットをそのまま足す (同じ位相なので 4 倍)
    {
        Opn3 opn;
        setupCarrier(opn, 0, 1038, 4);
        for (int s = 0; s < 4; s++) opn.writeReg(0, (uint8_t)(0x40 + s * 4), 0);
        keyOn(opn, 0);
        CHECK_NEAR(peak(render(opn, 3000).l, 1000), 8168 * 4, 40);
        // S1 と S2 だけキーオン
        keyOff(opn, 0);
        render(opn, 3000);
        keyOn(opn, 0, 0x3);
        CHECK_NEAR(peak(render(opn, 3000).l, 1000), 8168 * 2, 20);
    }
    // 変調: キャリアの数はアルゴリズムで 1、1、1、1、2、3、3、4。変調する側の TL を 127 にすると、キャリアの数 x 最大
    static const int carriers[8] = {1, 1, 1, 1, 2, 3, 3, 4};
    // レジスタの並びは S1、S3、S2、S4。キャリアになるスロット (S1-S4 を bit0-3) は次のとおり
    static const int carrierSlots[8] = {0x8, 0x8, 0x8, 0x8, 0xa, 0xe, 0xe, 0xf};
    static const int regOfSlot[4] = {0, 8, 4, 12};
    for (int alg = 0; alg < 8; alg++) {
        Opn3 opn;
        setupCarrier(opn, 0, 1038, 4);
        opn.writeReg(0, 0xb0, (uint8_t)alg);
        for (int s = 0; s < 4; s++) opn.writeReg(0, (uint8_t)(0x40 + regOfSlot[s]), (carrierSlots[alg] >> s & 1) ? 0 : 127);
        keyOn(opn, 0);
        const Wave plain = render(opn, 4000);
        CHECK_NEAR(peak(plain.l, 1000), 8168 * carriers[alg], 10 * carriers[alg]);
        if (alg == 7) continue;
        // 変調する側のスロットをすべて鳴らしておき、1 つずつ止めると、どれでも波形が変わる (キャリアまでつながっている)
        const auto renderWith = [&](int muted) {
            Opn3 mod;
            setupCarrier(mod, 0, 1038, 4);
            mod.writeReg(0, 0xb0, (uint8_t)alg);
            for (int s = 0; s < 4; s++)
                mod.writeReg(0, (uint8_t)(0x40 + regOfSlot[s]), (carrierSlots[alg] >> s & 1) ? 0 : s == muted ? 127 : 24);
            keyOn(mod, 0);
            return render(mod, 4000);
        };
        const Wave all = renderWith(-1);
        for (int m = 0; m < 4; m++) {
            if (carrierSlots[alg] >> m & 1) continue;
            const Wave fm = renderWith(m);
            long long diff = 0;
            for (size_t i = 0; i < fm.l.size(); i++) diff += llabs((long long)fm.l[i] - all.l[i]);
            if (diff < 1000000) {
                printf("  algorithm %d: muting S%d did not change the wave (diff %lld)\n", alg, m + 1, diff);
                failures++;
            }
        }
    }
    // フィードバック: S1 だけを鳴らし、FB を上げると波形が正弦波から変わる (最大の振れは変わらない)
    {
        Opn3 a, b;
        for (Opn3* opn : {&a, &b}) {
            setupCarrier(*opn, 0, 1038, 4);
            opn->writeReg(0, 0x40, 0);
            opn->writeReg(0, 0x4c, 127);
        }
        b.writeReg(0, 0xb0, 0x07 | 6 << 3);
        keyOn(a, 0);
        keyOn(b, 0);
        const Wave wa = render(a, 4000), wb = render(b, 4000);
        long long diff = 0;
        for (size_t i = 0; i < wa.l.size(); i++) diff += llabs((long long)wa.l[i] - wb.l[i]);
        CHECK(diff > 1000000);
        CHECK(peak(wb.l) <= 8168);
    }
}

void testLfo() {
    printf("LFO\n");
    // AM: AMS 3 で 11.8dB (0 から 126 x 0.09375dB)。周波数は $22 の 7 で 72.2Hz (7.9872MHz のとき。8MHz では 72.3Hz)
    {
        Opn3 opn;
        setupCarrier(opn, 0, 1038, 6); // 1760Hz
        opn.writeReg(0, 0x6c, 0x80);   // AM を受ける
        opn.writeReg(0, 0xb4, 0xf0);   // AMS 3
        opn.writeReg(0, 0x22, 0x0f);
        keyOn(opn, 0);
        const Wave w = render(opn, 55556);
        // 包絡線: 32 サンプル (1760Hz の 1 周期ほど) ごとの最大
        std::vector<int32_t> env;
        for (size_t i = 0; i + 32 <= w.l.size(); i += 32) {
            int p = 0;
            for (size_t j = i; j < i + 32; j++) p = abs(w.l[j]) > p ? abs(w.l[j]) : p;
            env.push_back(p);
        }
        int lo = 1 << 30, hi = 0;
        for (size_t i = 8; i < env.size(); i++) {
            lo = env[i] < lo ? env[i] : lo;
            hi = env[i] > hi ? env[i] : hi;
        }
        CHECK_NEAR(20 * log10((double)hi / lo), 11.8, 0.5);
        // 包絡線が中央の値を下から上へ横切る回数 = LFO の周波数
        const double mid = (hi + lo) / 2.0;
        int crossings = 0;
        for (size_t i = 9; i < env.size(); i++)
            if (env[i - 1] < mid && env[i] >= mid) crossings++;
        CHECK_NEAR(crossings, 72, 2);
        // AMS 1 は 1.4dB
        opn.writeReg(0, 0xb4, 0xd0);
        const Wave w1 = render(opn, 20000);
        lo = 1 << 30, hi = 0;
        for (size_t i = 0; i + 32 <= w1.l.size(); i += 32) {
            int p = 0;
            for (size_t j = i; j < i + 32; j++) p = abs(w1.l[j]) > p ? abs(w1.l[j]) : p;
            lo = p < lo ? p : lo;
            hi = p > hi ? p : hi;
        }
        CHECK_NEAR(20 * log10((double)hi / lo), 1.4, 0.2);
        // LFO を止めると、変調は消える
        opn.writeReg(0, 0x22, 0x00);
        const Wave w2 = render(opn, 3000);
        CHECK_NEAR(peak(w2.l, 1000), 8168, 8);
    }
    // PM: PMS 7 で +-80 セント。短い区間の周波数の最大と最小の比を見る ($22 の 0 は約 4Hz)
    {
        Opn3 opn;
        setupCarrier(opn, 0, 1038, 6);
        opn.writeReg(0, 0xb4, 0xc7);
        opn.writeReg(0, 0x22, 0x08);
        keyOn(opn, 0);
        const Wave w = render(opn, 55556);
        double lo = 1e9, hi = 0;
        for (size_t i = 0; i + 400 <= w.l.size(); i += 400) {
            const std::vector<int32_t> part(w.l.begin() + (long)i, w.l.begin() + (long)i + 400);
            const double f = frequency(part);
            lo = f < lo ? f : lo;
            hi = f > hi ? f : hi;
        }
        CHECK_NEAR(1200 * log2(hi / 1760.0), 80, 6);
        CHECK_NEAR(1200 * log2(1760.0 / lo), 80, 6);
        // PMS 0 なら揺れない
        opn.writeReg(0, 0xb4, 0xc0);
        CHECK_NEAR(frequency(render(opn, 30000).l), 1760.0, 1.0);
        CHECK(opn.stateOk());
    }
}

void testCh3Special() {
    printf("channel 3 per-slot F-Number\n");
    Opn3 opn;
    setupCarrier(opn, 2, 1038, 4);
    // $A8 が S3、$A9 が S1、$AA が S2。S4 はチャネルの F-Number
    opn.writeReg(0, 0xad, (uint8_t)(3 << 3 | 1038 >> 8)); // S1: 220Hz
    opn.writeReg(0, 0xa9, (uint8_t)1038);
    opn.writeReg(0, 0xae, (uint8_t)(5 << 3 | 1038 >> 8)); // S2: 880Hz
    opn.writeReg(0, 0xaa, (uint8_t)1038);
    opn.writeReg(0, 0xac, (uint8_t)(6 << 3 | 1038 >> 8)); // S3: 1760Hz
    opn.writeReg(0, 0xa8, (uint8_t)1038);
    static const int tlReg[4] = {0x42, 0x4a, 0x46, 0x4e}; // S1 S2 S3 S4
    static const double want[4] = {220, 880, 1760, 440};
    for (int s = 0; s < 4; s++) {
        for (int i = 0; i < 4; i++) opn.writeReg(0, (uint8_t)tlReg[i], i == s ? 0 : 127);
        // ふつうのモードでは、どのスロットもチャネルの F-Number
        opn.writeReg(0, 0x27, 0x00);
        keyOff(opn, 2);
        render(opn, 2000);
        keyOn(opn, 2);
        CHECK_NEAR(frequency(render(opn, 30000).l), 440.0, 0.5);
        opn.writeReg(0, 0x27, 0x40);
        CHECK_NEAR(frequency(render(opn, 30000).l), want[s], want[s] / 800);
    }
    // チャネル 6 ($A8-$AE の A1 = 1 の側) にはない
    CHECK_EQ(opn.reg(1, 0xa8), 0);
}

void testSsg() {
    printf("SSG\n");
    // トーン: 周波数は fM / (64 x TP)。0 を中心に振れ、1 チャネルの最大の振れ幅 (片側) は FM の 1 スロットの 0.25 倍
    {
        Opn3 opn;
        opn.writeReg(0, 0x00, 284 & 0xff); // 8MHz / (64 x 284) = 440.1Hz
        opn.writeReg(0, 0x01, 284 >> 8);
        opn.writeReg(0, 0x07, 0x3e); // A のトーンだけ
        opn.writeReg(0, 0x08, 15);
        const Wave w = render(opn, 60000);
        int lo = 1 << 30, hi = -(1 << 30);
        long long total = 0;
        for (int32_t v : w.l) {
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
            total += v;
        }
        CHECK_NEAR(lo, -8168 / 4, 4);
        CHECK_NEAR(hi, 8168 / 4, 4);
        CHECK_NEAR((double)total / (double)w.l.size(), 0, 8); // 直流は乗らない
        CHECK_NEAR(frequency(w.l), 8000000.0 / 64 / 284, 0.3);
        CHECK(w.l == w.r);
        // 音量は 3dB 刻み: 13 で半分
        opn.writeReg(0, 0x08, 13);
        CHECK_NEAR(peak(render(opn, 3000).l), 8168 / 8, 4);
        opn.writeReg(0, 0x08, 0);
        CHECK_EQ(peak(render(opn, 3000).l), 0);
        CHECK_EQ(opn.silent(), true);
    }
    // 3 チャネルを同時に: 和になる
    {
        Opn3 opn;
        static const int tp[3] = {284, 200, 100};
        for (int c = 0; c < 3; c++) {
            opn.writeReg(0, (uint8_t)(c * 2), (uint8_t)tp[c]);
            opn.writeReg(0, (uint8_t)(c * 2 + 1), (uint8_t)(tp[c] >> 8));
            opn.writeReg(0, (uint8_t)(8 + c), 15);
        }
        opn.writeReg(0, 0x07, 0x38);
        CHECK_NEAR(peak(render(opn, 60000).l), 3 * (8168 / 4), 12);
    }
    // TP が 7 以下ならトーンは出ず、音量の分の直流になる。トーンもノイズも止めたときは、その 2 倍の直流 (データシート)
    {
        Opn3 opn;
        opn.writeReg(0, 0x00, 7);
        opn.writeReg(0, 0x07, 0x3e);
        opn.writeReg(0, 0x08, 15);
        Wave w = render(opn, 1000);
        CHECK_EQ(w.l[10], w.l[900]);
        CHECK_NEAR(w.l[10], 8168 / 4, 4);
        opn.writeReg(0, 0x00, 100);
        opn.writeReg(0, 0x07, 0x3f);
        w = render(opn, 1000);
        CHECK_EQ(w.l[10], w.l[900]);
        CHECK_NEAR(w.l[10], 8168 / 2, 4);
    }
    // 音量が 0 の間も、矩形波とエンベロープは進む: 途中で音量を 0 にして戻しても、ずっと鳴らしていたときと同じ波形になる
    {
        Opn3 a, b;
        for (Opn3* opn : {&a, &b}) {
            opn->writeReg(0, 0x00, 200);
            opn->writeReg(0, 0x02, 77);
            opn->writeReg(0, 0x06, 9);
            opn->writeReg(0, 0x07, 0x2c); // A と B のトーン、B のノイズ
            opn->writeReg(0, 0x0b, 50);
            opn->writeReg(0, 0x0d, 0x0a); // 三角波のエンベロープ
            opn->writeReg(0, 0x08, 12);
            opn->writeReg(0, 0x09, 0x10);
        }
        render(a, 5000);
        render(b, 1000);
        b.writeReg(0, 0x08, 0);
        b.writeReg(0, 0x09, 0);
        CHECK_EQ(peak(render(b, 3000).l), 0);
        b.writeReg(0, 0x08, 12);
        b.writeReg(0, 0x09, 0x10);
        render(b, 1000);
        const Wave wa = render(a, 4000), wb = render(b, 4000);
        CHECK(wa.l == wb.l);
        CHECK(peak(wa.l) > 1000);
    }
    // ノイズ: 周期を変えると、値の変わる回数が変わる (fM / (64 x NP) で 1 回)
    {
        int changes[2];
        for (int i = 0; i < 2; i++) {
            Opn3 opn;
            opn.writeReg(0, 0x06, (uint8_t)(i ? 31 : 8));
            opn.writeReg(0, 0x07, 0x37); // A のノイズだけ
            opn.writeReg(0, 0x08, 15);
            const Wave w = render(opn, 55556);
            changes[i] = 0;
            for (size_t k = 1; k < w.l.size(); k++) {
                const bool a = w.l[k - 1] > 0, b = w.l[k] > 0;
                if (a != b) changes[i]++;
            }
        }
        // 1 秒に 8MHz / 64 / NP 回ずらし、その半分ほどで値が変わる
        CHECK_NEAR(changes[0], 8000000 / 64 / 8 / 2, 800);
        CHECK_NEAR(changes[1], 8000000 / 64 / 31 / 2, 250);
    }
    // エンベロープ: 形 $0E (三角波) の周期は 2 x 32 段、1 段は fM / (32 x EP)... 1 周は 256 x EP x 2 x 8 / fM 秒
    {
        Opn3 opn;
        opn.writeReg(0, 0x07, 0x3f);
        opn.writeReg(0, 0x08, 0x10);
        opn.writeReg(0, 0x0b, 100);
        opn.writeReg(0, 0x0c, 0);
        opn.writeReg(0, 0x0d, 0x0e);
        const Wave w = render(opn, 55556);
        // 1 段は 32 x EP マスタークロック、三角波の 1 周は 64 段
        const double period = 32.0 * 100 * 64 / 8000000;
        int tops = 0;
        for (size_t k = 1; k + 1 < w.l.size(); k++)
            if (w.l[k] == 8168 / 2 && w.l[k - 1] < 8168 / 2) tops++;
        CHECK_NEAR(tops, 1.0 / period, 2);
        // 形 $00 (\\___): 最大から下がって 0 で止まる
        opn.writeReg(0, 0x0d, 0x00);
        const Wave d = render(opn, 4000);
        CHECK_NEAR(d.l[0], 8168 / 2, 4);
        CHECK(d.l[500] < d.l[0] && d.l[500] > 0);
        CHECK_EQ(d.l[3999], 0);
        // 形 $0D (/‾‾‾): 0 から上がって最大で止まる
        opn.writeReg(0, 0x0d, 0x0d);
        const Wave u = render(opn, 4000);
        CHECK(u.l[0] < 100);
        CHECK_NEAR(u.l[3999], 8168 / 2, 4);
        CHECK(opn.stateOk());
    }
}

void testRhythm() {
    printf("rhythm\n");
    // 試験用の音: 6 つとも長さの違う矩形波 (本物の音は持っていない)
    static int16_t pcm[Opn3::RHYTHMS][4000];
    static Opn3::RhythmSet set;
    for (int i = 0; i < Opn3::RHYTHMS; i++) {
        const int len = 1000 + i * 500;
        for (int k = 0; k < len; k++) pcm[i][k] = (int16_t)((k / 10) & 1 ? -32000 : 32000);
        set.pcm[i] = pcm[i];
        set.length[i] = (uint32_t)len;
        set.rate[i] = (uint32_t)(kRate + 0.5); // チップのサンプリング周波数と同じ: 1 サンプルずつ進む
    }
    {
        // データがなければ鳴らない
        Opn3::setRhythm(nullptr);
        Opn3 opn;
        opn.writeReg(0, 0x11, 63);
        opn.writeReg(0, 0x18, 0xdf);
        opn.writeReg(0, 0x10, 0x01);
        CHECK_EQ(peak(render(opn, 2000).l), 0);
        CHECK_EQ(opn.silent(), true);
    }
    Opn3::setRhythm(&set);
    {
        Opn3 opn;
        opn.writeReg(0, 0x11, 63);   // RTL 0dB
        opn.writeReg(0, 0x18, 0xdf); // BD: L R、IL 0dB
        opn.writeReg(0, 0x10, 0x01);
        CHECK_EQ(opn.silent(), false);
        Wave w = render(opn, 1100);
        // 16 ビットの最大が FM の 1 スロットの最大 (1/4) になる
        CHECK_NEAR(peak(w.l), 32000 / 4, 4);
        CHECK(w.l == w.r);
        CHECK(w.l[998] != 0);
        CHECK_EQ(w.l[1050], 0); // 長さの分だけ鳴って終わる
        CHECK_EQ(opn.silent(), true);
        // RTL と IL はどちらも 0.75dB 刻み: 合わせて 8 段で -6dB
        opn.writeReg(0, 0x11, 59);
        opn.writeReg(0, 0x18, 0xdb);
        opn.writeReg(0, 0x10, 0x01);
        CHECK_NEAR(peak(render(opn, 500).l), 32000 / 8, 30);
        // ダンプで止まる
        opn.writeReg(0, 0x10, 0x81);
        CHECK_EQ(peak(render(opn, 300).l), 0);
        // L だけ、R だけ
        opn.writeReg(0, 0x11, 63);
        opn.writeReg(0, 0x19, 0x9f); // SD: L
        opn.writeReg(0, 0x1d, 0x5f); // RIM: R
        opn.writeReg(0, 0x10, 0x02);
        w = render(opn, 200);
        CHECK(peak(w.l) > 7000);
        CHECK_EQ(peak(w.r), 0);
        opn.writeReg(0, 0x10, 0x82);
        opn.writeReg(0, 0x10, 0x20);
        w = render(opn, 200);
        CHECK_EQ(peak(w.l), 0);
        CHECK(peak(w.r) > 7000);
        // 6 音を同時に: いちばん長い音 (3500 サンプル) が終わるまで鳴る
        for (int i = 0; i < 6; i++) opn.writeReg(0, (uint8_t)(0x18 + i), 0xdf);
        opn.writeReg(0, 0x10, 0x3f);
        w = render(opn, 4000);
        CHECK_NEAR(peak(w.l), 6 * (32000 / 4), 30);
        CHECK(w.l[3400] != 0);
        CHECK_EQ(w.l[3600], 0);
        CHECK(opn.stateOk());
    }
    // 音のサンプリング周波数が半分なら、2 倍の時間で鳴る
    {
        Opn3::RhythmSet half = set;
        half.rate[0] = (uint32_t)(kRate / 2 + 0.5);
        Opn3::setRhythm(&half);
        Opn3 opn;
        opn.writeReg(0, 0x11, 63);
        opn.writeReg(0, 0x18, 0xdf);
        opn.writeReg(0, 0x10, 0x01);
        const Wave w = render(opn, 2100);
        CHECK(w.l[1990] != 0);
        CHECK_EQ(w.l[2050], 0);
    }
    Opn3::setRhythm(nullptr);
}

// リズムの音を WAV から作る (platform/opn_rhythm.h)
std::vector<uint8_t> makeWav(int channels, int bits, uint32_t rate, const std::vector<int16_t>& samples, bool extraChunk) {
    std::vector<uint8_t> f;
    const auto p16 = [&f](uint32_t v) {
        f.push_back((uint8_t)v);
        f.push_back((uint8_t)(v >> 8));
    };
    const auto p32 = [&p16](uint32_t v) {
        p16(v & 0xffff);
        p16(v >> 16);
    };
    const auto tag = [&f](const char* s) { f.insert(f.end(), s, s + 4); };
    tag("RIFF");
    p32(0);
    tag("WAVE");
    if (extraChunk) { // 長さが奇数のチャンク (あとに詰め物が 1 バイト入る)
        tag("LIST");
        p32(3);
        f.insert(f.end(), {1, 2, 3, 0});
    }
    tag("fmt ");
    p32(16);
    p16(1);
    p16((uint32_t)channels);
    p32(rate);
    p32(rate * (uint32_t)channels * (uint32_t)bits / 8);
    p16((uint32_t)(channels * bits / 8));
    p16((uint32_t)bits);
    tag("data");
    p32((uint32_t)samples.size() * (uint32_t)bits / 8);
    for (int16_t s : samples) {
        if (bits == 16) p16((uint16_t)s);
        else f.push_back((uint8_t)(s >> 8));
    }
    return f;
}

void testRhythmWav() {
    printf("rhythm sounds from WAV files\n");
    uint32_t frames = 0, rate = 0;
    {
        std::vector<uint8_t> f = makeWav(1, 16, 44100, {100, -200, 300, -32768, 32767}, false);
        CHECK(platform::opnRhythmFromWav(f.data(), f.size(), frames, rate));
        CHECK_EQ(frames, 5);
        CHECK_EQ(rate, 44100);
        int16_t s[5];
        memcpy(s, f.data(), sizeof s);
        CHECK_EQ(s[0], 100);
        CHECK_EQ(s[1], -200);
        CHECK_EQ(s[3], -32768);
        CHECK_EQ(s[4], 32767);
    }
    {
        // ステレオは左右を足して半分。前に別のチャンクがあってもよい
        std::vector<uint8_t> f = makeWav(2, 16, 22050, {100, 300, -1000, -3000, 32767, 32767}, true);
        CHECK(platform::opnRhythmFromWav(f.data(), f.size(), frames, rate));
        CHECK_EQ(frames, 3);
        CHECK_EQ(rate, 22050);
        int16_t s[3];
        memcpy(s, f.data(), sizeof s);
        CHECK_EQ(s[0], 200);
        CHECK_EQ(s[1], -2000);
        CHECK_EQ(s[2], 32767);
    }
    {
        // 8 ビット、WAV でないもの、途中で切れたもの、データのないものは読まない
        std::vector<uint8_t> f = makeWav(1, 8, 44100, {100, 200}, false);
        CHECK(!platform::opnRhythmFromWav(f.data(), f.size(), frames, rate));
        f = makeWav(1, 16, 44100, {1, 2, 3, 4}, false);
        CHECK(!platform::opnRhythmFromWav(f.data(), f.size() - 3, frames, rate));
        f = makeWav(1, 16, 44100, {}, false);
        CHECK(!platform::opnRhythmFromWav(f.data(), f.size(), frames, rate));
        f = makeWav(1, 16, 44100, {1, 2}, false);
        f[0] = 'X';
        CHECK(!platform::opnRhythmFromWav(f.data(), f.size(), frames, rate));
        uint8_t tiny[4] = {'R', 'I', 'F', 'F'};
        CHECK(!platform::opnRhythmFromWav(tiny, sizeof tiny, frames, rate));
    }
    {
        // OpnRhythm に入れて鳴らす
        static std::vector<uint8_t> f;
        std::vector<int16_t> pcm;
        for (int i = 0; i < 2000; i++) pcm.push_back((int16_t)((i / 8) & 1 ? -20000 : 20000));
        f = makeWav(1, 16, 44100, pcm, false);
        static platform::OpnRhythm rhythm;
        CHECK(rhythm.add(3, f.data(), f.size())); // HH
        CHECK(!rhythm.add(0, nullptr, 0));
        Opn3::setRhythm(&rhythm.set);
        Opn3 opn;
        opn.writeReg(0, 0x11, 63);
        opn.writeReg(0, 0x1b, 0xdf);
        opn.writeReg(0, 0x10, 0x09); // BD (データなし) と HH
        const Wave w = render(opn, 3000);
        CHECK_NEAR(peak(w.l), 20000 / 4, 4);
        // 44.1kHz の 2000 サンプルは、チップの 2000 x 55556 / 44100 = 2520 サンプル
        CHECK(w.l[2500] != 0);
        CHECK_EQ(w.l[2540], 0);
        Opn3::setRhythm(nullptr);
    }
}

} // namespace

int main() {
    testTimers();
    testModeAndRead();
    testPitchAndLevel();
    testEnvelope();
    testAlgorithms();
    testLfo();
    testCh3Special();
    testSsg();
    testRhythm();
    testRhythmWav();
    if (failures) {
        printf("FAILED: %d\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
