// OPM (YM2151) のテスト: タイマと状態のフラグ、割り込み、音程、エンベロープの速さ、アルゴリズム、LFO、ノイズ
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x68k/opm.h"

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

void w(x68k::Opm& opm, uint8_t r, uint8_t v) {
    opm.write(1, r);
    opm.write(3, v);
}

// サンプル数を数えながら、条件が成り立つまで進める (上限 limit)
template <class F>
int samplesUntil(x68k::Opm& opm, F cond, int limit) {
    for (int n = 1; n <= limit; n++) {
        opm.clockSample(nullptr);
        if (cond()) return n;
    }
    return -1;
}

void testTimerA() {
    printf("timer A\n");
    x68k::Opm opm;
    // NA = 1000: 周期は 64 x (1024 - 1000) クロック = 24 サンプル
    w(opm, 0x10, 1000 >> 2);
    w(opm, 0x11, 1000 & 3);
    CHECK_EQ(opm.read(3) & 3, 0);
    w(opm, 0x14, 0x05); // LOAD A、IRQEN A
    CHECK(!opm.irq());
    int n = samplesUntil(opm, [&] { return opm.irq(); }, 1000);
    CHECK_EQ(n, 24);
    CHECK_EQ(opm.read(3) & 3, 1);
    // フラグを消すと割り込みも消える。タイマは動き続け、次も 24 サンプル後
    w(opm, 0x14, 0x15);
    CHECK(!opm.irq());
    CHECK_EQ(opm.read(3) & 3, 0);
    n = samplesUntil(opm, [&] { return opm.irq(); }, 1000);
    CHECK_EQ(n, 24);
    // IRQEN を落とすと、オーバーフローしてもフラグは立たない
    w(opm, 0x14, 0x11);
    n = samplesUntil(opm, [&] { return opm.irq(); }, 200);
    CHECK_EQ(n, -1);
    CHECK_EQ(opm.read(3) & 3, 0);
}

void testTimerB() {
    printf("timer B\n");
    x68k::Opm opm;
    // NB = 200: 周期は 1024 x (256 - 200) クロック = 16 x 56 サンプル
    w(opm, 0x12, 200);
    w(opm, 0x14, 0x0a); // LOAD B、IRQEN B
    int n = samplesUntil(opm, [&] { return opm.irq(); }, 100000);
    CHECK_EQ(n, 16 * 56);
    CHECK_EQ(opm.read(3) & 3, 2);
    // LOAD を落とすと止まる
    w(opm, 0x14, 0x28);
    CHECK(!opm.irq());
    n = samplesUntil(opm, [&] { return opm.irq(); }, 3000);
    CHECK_EQ(n, -1);
    // 2 つのタイマを同時に
    w(opm, 0x10, 0xff);
    w(opm, 0x11, 0x03); // NA = 1023: 1 サンプル
    w(opm, 0x12, 0xff); // NB = 255: 16 サンプル
    w(opm, 0x14, 0x0f);
    opm.clockSample(nullptr);
    CHECK_EQ(opm.read(3) & 3, 1);
    for (int i = 0; i < 15; i++) opm.clockSample(nullptr);
    CHECK_EQ(opm.read(3) & 3, 3);
    w(opm, 0x14, 0x1f); // A のフラグだけ消す
    CHECK_EQ(opm.read(3) & 3, 2);
}

void testBusy() {
    printf("busy\n");
    x68k::Opm opm;
    CHECK_EQ(opm.read(3) & 0x80, 0);
    w(opm, 0x20, 0xc7);
    CHECK_EQ(opm.read(3) & 0x80, 0x80);
    opm.elapse(63);
    CHECK_EQ(opm.read(3) & 0x80, 0x80);
    opm.elapse(1);
    CHECK_EQ(opm.read(3) & 0x80, 0);
    // アドレスを書いても BUSY にはならない
    opm.write(1, 0x08);
    CHECK_EQ(opm.read(3) & 0x80, 0);
}

// チャネル 0 に、C2 だけを鳴らす音色 (アルゴリズム 7、M1 / C1 / M2 は TL = 127) を設定する
void setupSine(x68k::Opm& opm, uint8_t kc, uint8_t kf, uint8_t ar = 31, uint8_t rr = 15, uint8_t mul = 1) {
    w(opm, 0x20, 0xc7); // L / R、FB = 0、CON = 7
    w(opm, 0x28, kc);
    w(opm, 0x30, (uint8_t)(kf << 2));
    for (int s = 0; s < 4; s++) {
        int base = s * 8;
        bool carrier = s == 3; // C2
        w(opm, (uint8_t)(0x40 + base), mul & 15);
        w(opm, (uint8_t)(0x60 + base), carrier ? 0 : 127);
        w(opm, (uint8_t)(0x80 + base), ar);
        w(opm, (uint8_t)(0xa0 + base), 0);
        w(opm, (uint8_t)(0xc0 + base), 0);
        w(opm, (uint8_t)(0xe0 + base), rr);
    }
}

// n サンプルを作り、L の値を buf に入れる
void render(x68k::Opm& opm, int32_t* buf, int n) {
    for (int i = 0; i < n; i++) {
        int32_t o[2];
        opm.clockSample(o);
        buf[i] = o[0];
    }
}

// 0 を負から正へ横切る回数から周波数を求める (62.5kHz)
double frequency(const int32_t* buf, int n) {
    int first = -1, last = -1, count = 0;
    for (int i = 1; i < n; i++) {
        if (buf[i - 1] < 0 && buf[i] >= 0) {
            if (first < 0) first = i;
            last = i;
            count++;
        }
    }
    if (count < 2) return 0;
    return (count - 1) * 62500.0 / (last - first);
}

void testPitch() {
    printf("pitch\n");
    static int32_t buf[62500];
    struct Case {
        uint8_t kc, kf, mul;
        double hz; // 4MHz のクロックでの周波数
    };
    // KC = $4A (A4)、KF = 0 は 3.579545MHz で 440Hz。X68000 (4MHz) ではその 4 / 3.579545 倍
    const double a4 = 440.0 * 4000000 / 3579545;
    const Case cases[] = {
        {0x4a, 0, 1, a4},
        {0x3a, 0, 1, a4 / 2},           // 1 オクターブ下
        {0x4a, 0, 2, a4 * 2},           // MUL = 2
        {0x4a, 0, 0, a4 / 2},           // MUL = 0 は 1/2
        {0x4a, 32, 1, a4 * 1.0293022},  // 半音の半分 (2^(1/24))
        {0x4c, 0, 1, a4 * 1.0594631},   // A# ($4B は A# と同じ番号の飛び)
        {0x4d, 0, 1, a4 * 1.1224620},   // B
        {0x4e, 0, 1, a4 * 1.1892071},   // C (オクターブの最後)
        {0x50, 0, 1, a4 * 1.2599210},   // C# (次のオクターブの最初)
    };
    for (const Case& c : cases) {
        x68k::Opm opm;
        setupSine(opm, c.kc, c.kf, 31, 15, c.mul);
        w(opm, 0x08, 0x40); // C2 をキーオン
        render(opm, buf, 62500);
        double f = frequency(buf, 62500);
        CHECK_NEAR(f, c.hz, c.hz * 0.002);
    }
}

void testWaveform() {
    printf("waveform\n");
    // 最大音量 (TL = 0、AR = 31) の正弦波の振幅はおよそ 8191 で、左右の出力を RL で選べる
    x68k::Opm opm;
    setupSine(opm, 0x4a, 0);
    w(opm, 0x20, 0x47); // L だけ
    w(opm, 0x08, 0x40);
    int32_t maxL = 0, minL = 0, maxR = 0;
    for (int i = 0; i < 2000; i++) {
        int32_t o[2];
        opm.clockSample(o);
        if (o[0] > maxL) maxL = o[0];
        if (o[0] < minL) minL = o[0];
        if (o[1] > maxR) maxR = o[1];
    }
    CHECK_NEAR(maxL, 8191, 30);
    CHECK_NEAR(minL, -8191, 30);
    CHECK_EQ(maxR, 0);
    // TL = 8 (6dB) で振幅は半分
    w(opm, 0x78, 8);
    maxL = 0;
    for (int i = 0; i < 2000; i++) {
        int32_t o[2];
        opm.clockSample(o);
        if (o[0] > maxL) maxL = o[0];
    }
    CHECK_NEAR(maxL, 4096, 40);
}

// エンベロープ: アタックとリリースの時間がおおよそ合っているか
void testEnvelope() {
    printf("envelope\n");
    // AR = 31 は即座に最大音量
    {
        x68k::Opm opm;
        setupSine(opm, 0x4a, 0, 31, 15);
        w(opm, 0x08, 0x40);
        CHECK_EQ(opm.envelope(0, 3), 0);
    }
    // AR = 20 (実効レート 40、KS = 0、KC の影響なし): 減衰量 1023 から 0 まで。
    // レート 40 は 3 サンプルごとの EG の刻みで 2 回に 1 回、変化量 (~env * 1) >> 4
    // 計算上 0 になるまでの EG の刻みの数を数え、時間に直したものと比べる
    {
        x68k::Opm opm;
        setupSine(opm, 0x0a, 0, 20, 15);
        w(opm, 0x08, 0x40);
        int32_t dummy[2];
        int n = 0;
        while (opm.envelope(0, 3) > 0 && n < 1000000) {
            opm.clockSample(dummy);
            n++;
        }
        // レート 40 は EG の刻み (3 サンプル) の 2 回に 1 回、減衰量が (~env x 1) / 16 ずつ減る。
        // 1023 から 0 まで約 145 回 = 870 サンプル (およそ 14ms) になるはず
        double ms = n / 62.5;
        CHECK(ms > 10.0 && ms < 18.0);
        printf("  AR=20: %d samples (%.2f ms) to full level\n", n, ms);
    }
    // RR = 4 (実効レート 18): リリースで 0 から 1023 まで
    {
        x68k::Opm opm;
        setupSine(opm, 0x0a, 0, 31, 4);
        w(opm, 0x08, 0x40);
        w(opm, 0x08, 0x00);
        int32_t dummy[2];
        int n = 0;
        while (opm.envelope(0, 3) < 1023 && n < 10000000) {
            opm.clockSample(dummy);
            n++;
        }
        // レート 18 は 2^(11-4) = 128 回の EG の刻みに 1 回、変化量は平均 1.5 (パターン 0x11101110 の 6/8 x 2...)
        // 1023 / (6/8) x 128 x 3 サンプル ~ 523776 サンプル ~ 8.4 秒
        double s = n / 62500.0;
        printf("  RR=4: %d samples (%.2f s) to silence\n", n, s);
        CHECK(s > 6.0 && s < 11.0);
    }
    // D1R / D1L: D1L = 2 (6dB) まで D1R で下がり、そのあとは D2R = 0 なので止まる
    {
        x68k::Opm opm;
        setupSine(opm, 0x4a, 0, 31, 15);
        w(opm, 0xb8, 20);        // D1R
        w(opm, 0xd8, 0);         // D2R = 0
        w(opm, 0xf8, 0x2f);      // D1L = 2、RR = 15
        w(opm, 0x08, 0x40);
        int32_t dummy[2];
        for (int i = 0; i < 62500; i++) opm.clockSample(dummy);
        CHECK_NEAR(opm.envelope(0, 3), 64, 8);
    }
    // キーオフしたチャネルはやがて計算されなくなり、出力は 0
    {
        x68k::Opm opm;
        setupSine(opm, 0x4a, 0, 31, 15);
        w(opm, 0x08, 0x40);
        int32_t o[2];
        for (int i = 0; i < 100; i++) opm.clockSample(o);
        w(opm, 0x08, 0x00);
        for (int i = 0; i < 6000; i++) opm.clockSample(o);
        CHECK_EQ(opm.envelope(0, 3), 1023);
        CHECK_EQ(o[0], 0);
    }
}

// FM 変調: アルゴリズム 0 で M1 だけを変調にすると、倍音が増える (C2 だけと比べて波形が変わる)
void testModulation() {
    printf("modulation\n");
    static int32_t a[4096], b[4096];
    {
        x68k::Opm opm;
        setupSine(opm, 0x4a, 0);
        w(opm, 0x20, 0xc7);
        w(opm, 0x08, 0x78);
        render(opm, a, 4096);
    }
    {
        // CON = 4: (M1 -> C1) + (M2 -> C2)。M2 を TL = 0 にすると C2 が変調される
        x68k::Opm opm;
        setupSine(opm, 0x4a, 0);
        w(opm, 0x20, 0xc4);
        w(opm, 0x68, 0); // M2 の TL = 0
        w(opm, 0x08, 0x78);
        render(opm, b, 4096);
    }
    long diff = 0;
    for (int i = 0; i < 4096; i++) diff += labs((long)(a[i] - b[i]));
    CHECK(diff > 4096L * 1000);
    // すべてのアルゴリズムで、出力が出て、+-32767 に収まる (4 つ全部がキャリアでも 4 x 8191)
    for (int con = 0; con < 8; con++) {
        x68k::Opm opm;
        setupSine(opm, 0x4a, 0);
        w(opm, 0x20, (uint8_t)(0xc0 | 7 << 3 | con)); // FB = 7
        for (int s = 0; s < 4; s++) w(opm, (uint8_t)(0x60 + s * 8), 0);
        w(opm, 0x08, 0x78);
        int32_t peak = 0;
        for (int i = 0; i < 4096; i++) {
            int32_t o[2];
            opm.clockSample(o);
            int32_t v = o[0] < 0 ? -o[0] : o[0];
            if (v > peak) peak = v;
        }
        CHECK(peak > 2000);
        CHECK(peak <= 4 * 8191);
    }
}

// LFO の PM: PMS = 7、PMD = 127 で音程が揺れる (周波数の最大と最小の差が大きい)
void testLfo() {
    printf("lfo\n");
    x68k::Opm opm;
    setupSine(opm, 0x4a, 0);
    w(opm, 0x18, 0xc0);        // LFRQ: 数 Hz
    w(opm, 0x19, 0x80 | 127);  // PMD
    w(opm, 0x1b, 2);           // 三角波
    w(opm, 0x38, 0x70);        // PMS = 7
    w(opm, 0x08, 0x40);
    static int32_t buf[62500];
    render(opm, buf, 62500);
    double lo = 1e9, hi = 0;
    for (int i = 0; i + 2500 <= 62500; i += 2500) {
        double f = frequency(buf + i, 2500);
        if (f < lo) lo = f;
        if (f > hi) hi = f;
    }
    CHECK(hi / lo > 1.3);
    // AM: AMS = 3、AMD = 127 で振幅が揺れる
    x68k::Opm am;
    setupSine(am, 0x4a, 0);
    w(am, 0x18, 0xc0);
    w(am, 0x19, 127);
    w(am, 0x1b, 2);
    w(am, 0x38, 0x03);
    w(am, 0xb8, 0x80); // C2 の AMS-EN
    w(am, 0x08, 0x40);
    render(am, buf, 62500);
    int32_t pmin = 1 << 30, pmax = 0;
    for (int i = 0; i + 2500 <= 62500; i += 2500) {
        int32_t p = 0;
        for (int j = 0; j < 2500; j++) p = buf[i + j] > p ? buf[i + j] : p;
        if (p < pmin) pmin = p;
        if (p > pmax) pmax = p;
    }
    CHECK(pmax > pmin * 4);
}

// LFO の波形がノイズのとき、AMD / PMD や波形を書き直しても、波形の位置が変わるまでは同じ乱数の値を使う
// (ymfm と同じ。書くたびに LFSR から取り直さない)
void testNoiseLfoKeepsValue() {
    printf("noise lfo keeps value on AMD/PMD writes\n");
    static x68k::Opm a, b;
    x68k::Opm* both[2] = {&a, &b};
    for (x68k::Opm* o : both) {
        *o = x68k::Opm();
        setupSine(*o, 0x4a, 0);
        w(*o, 0x18, 0x40); // LFRQ: 遅い (波形の位置がめったに変わらない)
        w(*o, 0x19, 100);  // AMD
        w(*o, 0x1b, 3);    // ノイズ
        w(*o, 0x38, 0x03); // AMS = 3
        w(*o, 0xb8, 0x80); // C2 の AMS-EN
        w(*o, 0x08, 0x40);
    }
    int diff = 0;
    for (int i = 0; i < 20000; i++) {
        // b だけ、同じ AMD と波形を書き直す
        if (i % 97 == 0) w(b, 0x19, 100);
        if (i % 89 == 0) w(b, 0x1b, 3);
        int32_t oa[2], ob[2];
        a.clockSample(oa);
        b.clockSample(ob);
        if (oa[0] != ob[0]) diff++;
    }
    CHECK_EQ(diff, 0);
}

void testNoise() {
    printf("noise\n");
    x68k::Opm opm;
    // チャネル 7 の C2 をノイズにする
    w(opm, 0x27, 0xc7);
    w(opm, 0x7f, 0);
    w(opm, 0x9f, 31);
    w(opm, 0xff, 15);
    for (int s = 0; s < 3; s++) w(opm, (uint8_t)(0x67 + s * 8), 127);
    w(opm, 0x0f, 0x80 | 16);
    w(opm, 0x08, 0x47);
    static int32_t buf[8192];
    render(opm, buf, 8192);
    int pos = 0, neg = 0, changes = 0;
    for (int i = 0; i < 8192; i++) {
        if (buf[i] > 0) pos++;
        if (buf[i] < 0) neg++;
        if (i && (buf[i] > 0) != (buf[i - 1] > 0)) changes++;
    }
    CHECK(pos > 2000 && neg > 2000);
    CHECK(changes > 300);
    // ノイズの周期 32 x (32 - 16) クロック = 8 サンプルより速くは変わらない
    CHECK(changes < 8192 / 8 * 2);
}

void testCt() {
    printf("ct\n");
    x68k::Opm opm;
    w(opm, 0x1b, 0x40);
    CHECK(opm.ct1());
    CHECK(!opm.ct2());
    w(opm, 0x1b, 0x80);
    CHECK(!opm.ct1());
    CHECK(opm.ct2());
}

} // namespace

int main() {
    testTimerA();
    testTimerB();
    testBusy();
    testPitch();
    testWaveform();
    testEnvelope();
    testModulation();
    testLfo();
    testNoiseLfoKeepsValue();
    testNoise();
    testCt();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
