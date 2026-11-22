// ADPCM (MSM6258) のテスト: 復号、サンプリング周波数、DMAC のチャネル 3 での転送、
// Machine につないだ OPM (タイマの割り込みが MFP の GPIP3 に届くこと、CT2 で ADPCM のクロックが変わること)、
// 音の出力 (Sound のリングバッファ、48kHz への変換)
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <initializer_list>

#include "x68k/adpcm.h"
#include "x68k/machine.h"
#include "x68k/sound.h"
#include "x68k/state.h"

namespace {

int failures = 0;

#define CHECK(cond)                                             \
    do {                                                        \
        if (!(cond)) {                                          \
            printf("  %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                         \
        }                                                       \
    } while (0)

#define CHECK_EQ(a, b)                                                                                \
    do {                                                                                              \
        long long va_ = (long long)(a), vb_ = (long long)(b);                                         \
        if (va_ != vb_) {                                                                             \
            printf("  %s:%d: %s == %s: got %lld, want %lld\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
            failures++;                                                                               \
        }                                                                                             \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                              \
    do {                                                                                                   \
        long long va_ = (long long)(a), vb_ = (long long)(b);                                              \
        if (va_ < vb_ - (tol) || va_ > vb_ + (tol)) {                                                      \
            printf("  %s:%d: %s ~= %s: got %lld, want %lld +- %d\n", __FILE__, __LINE__, #a, #b, va_, vb_, \
                   (int)(tol));                                                                            \
            failures++;                                                                                    \
        }                                                                                                  \
    } while (0)

// OKI (Dialogic) ADPCM の参考の実装: 量子化幅は floor(16 x 1.1^n) を 49 段、出力は 12 ビットで飽和
struct Reference {
    int signal = 0, index = 0;
    int step() const {
        double s = 16;
        for (int i = 0; i < index; i++) s *= 1.1;
        return (int)s;
    }
    int decode(int nibble) {
        int ss = step();
        int diff = ss / 8 + ((nibble & 4) ? ss : 0) + ((nibble & 2) ? ss / 2 : 0) + ((nibble & 1) ? ss / 4 : 0);
        signal += (nibble & 8) ? -diff : diff;
        if (signal > 2047) signal = 2047;
        if (signal < -2048) signal = -2048;
        static const int adj[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
        index += adj[nibble & 7];
        if (index < 0) index = 0;
        if (index > 48) index = 48;
        return signal;
    }
};

uint32_t rng = 12345;
uint8_t nextByte() {
    rng = rng * 1103515245 + 12345;
    return (uint8_t)(rng >> 16);
}

void testDecode() {
    printf("decode\n");
    // 決まった並び: 7 を続けると大きく上がり、飽和する
    {
        int32_t s = 0, st = 0;
        CHECK_EQ(x68k::Adpcm::decode(7, s, st), 30); // 16 + 8 + 4 + 2
        CHECK_EQ(st, 8);
        CHECK_EQ(x68k::Adpcm::decode(15, s, st), 30 - (34 + 17 + 8 + 4)); // 量子化幅 34 (8 段目)
        for (int i = 0; i < 20; i++) x68k::Adpcm::decode(7, s, st);
        CHECK_EQ(s, 2047);
        CHECK_EQ(st, 48);
    }
    // 乱数の並びで参考の実装と一致する
    {
        Reference ref;
        int32_t s = 0, st = 0;
        int mismatch = 0;
        for (int i = 0; i < 100000; i++) {
            int n = nextByte() & 15;
            if (x68k::Adpcm::decode((uint8_t)n, s, st) != ref.decode(n)) mismatch++;
        }
        CHECK_EQ(mismatch, 0);
    }
}

// バイトを $E92003 に書いて再生し、下位 4 ビット → 上位 4 ビットの順に復号されること、
// サンプリング周波数 (クロック / 分周)、転送の要求 (drq) の出方
void testPlayback() {
    printf("playback\n");
    x68k::Adpcm a;
    a.reset();
    a.setPanRate(0, 2); // 分周 512
    CHECK_EQ(a.period(), 512u); // 8MHz / 512 = 15.6kHz
    a.setClock4M(true);
    CHECK_EQ(a.period(), 1024u);
    a.setClock4M(false);
    a.setPanRate(0, 0);
    CHECK_EQ(a.period(), 1024u);
    a.setPanRate(0, 1);
    CHECK_EQ(a.period(), 768u);
    a.setPanRate(0, 2);

    CHECK_EQ(a.read(1), 0xc0); // 止まっている
    CHECK(!a.drq());
    a.write(1, 0x02); // 再生
    CHECK_EQ(a.read(1), 0x40);
    CHECK(a.drq()); // 最初のバイトを要求する
    Reference ref;
    const uint8_t data[4] = {0x17, 0x80, 0x3c, 0xf5};
    int expected[8];
    for (int i = 0; i < 4; i++) {
        expected[2 * i] = ref.decode(data[i] & 15);
        expected[2 * i + 1] = ref.decode(data[i] >> 4);
    }
    a.write(3, data[0]);
    CHECK(!a.drq());
    // 1 サンプルは 8MHz で 512 クロック = OPM のクロック (4MHz) で 256
    a.advance(255);
    CHECK_EQ(a.left(), 0);
    a.advance(1);
    CHECK_EQ(a.left(), expected[0]);
    CHECK(a.drq()); // ラッチが空いたので次を要求する
    a.write(3, data[1]);
    a.advance(256);
    CHECK_EQ(a.left(), expected[1]);
    CHECK(!a.drq()); // 次のバイトはまだラッチにある
    for (int i = 2; i < 8; i++) {
        if (i % 2 == 0) {
            CHECK(!a.drq());
        }
        a.advance(256);
        CHECK_EQ(a.left(), expected[i]);
        if (i % 2 == 0) {
            CHECK(a.drq());
            if (i / 2 + 1 < 4) a.write(3, data[i / 2 + 1]);
        }
    }
    // 左右: ポート C の bit0 で右、bit1 で左を止める
    a.setPanRate(1, 2);
    CHECK_EQ(a.right(), 0);
    CHECK_EQ(a.left(), expected[7]);
    a.setPanRate(2, 2);
    CHECK_EQ(a.left(), 0);
    CHECK_EQ(a.right(), expected[7]);
    // 停止
    a.write(1, 0x01);
    CHECK_EQ(a.read(1), 0xc0);
    CHECK(!a.drq());
}

// 量子化幅の表: 段 n の幅は 16 x 1.1^n の切り捨て (16 から 1552 まで 49 段)。符号 4 (B2 だけ) の差は 幅 + 幅 / 8
void testStepTable() {
    printf("step sizes\n");
    double size = 16;
    for (int n = 0; n <= 48; n++) {
        int32_t signal = 0, step = n;
        const int w = (int)size;
        CHECK_EQ(x68k::Adpcm::decode(4, signal, step), w + (w >> 3) > 2047 ? 2047 : w + (w >> 3));
        CHECK_EQ(step, n + 2 > 48 ? 48 : n + 2);
        size *= 1.1;
    }
    {
        int32_t signal = 0, step = 48;
        x68k::Adpcm::decode(0, signal, step);
        CHECK_EQ(signal, 1552 >> 3);
    }
    // 符号ごとの段の動き (-1 -1 -1 -1 +2 +4 +6 +8)。符号ビット (8) は動きに関係しない
    static const int kMove[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
    for (int n = 0; n < 16; n++) {
        int32_t signal = 0, step = 20;
        x68k::Adpcm::decode((uint8_t)n, signal, step);
        CHECK_EQ(step, 20 + kMove[n & 7]);
        CHECK_EQ(signal < 0, (n & 8) != 0); // 符号ビットが 1 なら波形が下がる
    }
    // 段は 0 より下がらない: 段 0 で符号 0〜3 (段を 1 つ下げる符号) を復号しても 0 のまま。幅は 16 のまま
    for (int n = 0; n < 4; n++) {
        int32_t signal = 0, step = 0;
        CHECK_EQ(x68k::Adpcm::decode((uint8_t)n, signal, step), 2 + ((n & 2) ? 8 : 0) + ((n & 1) ? 4 : 0));
        CHECK_EQ(step, 0);
        CHECK_EQ(x68k::Adpcm::decode(4, signal, step) - signal + 18, 18); // 返す値は新しい波形
        step = 0;
        signal = 0;
        x68k::Adpcm::decode((uint8_t)n, signal, step);
        signal = 0;
        CHECK_EQ(x68k::Adpcm::decode(4, signal, step), 16 + 2); // 次も幅 16
    }
    // 段は 48 より上がらない
    {
        int32_t signal = 0, step = 48;
        x68k::Adpcm::decode(7, signal, step);
        CHECK_EQ(step, 48);
    }
    // 12 ビットで止まる
    {
        int32_t signal = 2000, step = 48;
        CHECK_EQ(x68k::Adpcm::decode(7, signal, step), 2047);
        signal = -2000;
        CHECK_EQ(x68k::Adpcm::decode(15, signal, step), -2048);
    }
}

// 止めるコマンドと始めるコマンド (MSM6258 のデータシートの DAOUT の説明と「HOW TO USE THE DATA BUS」):
// 止めると出力は止まったときの値を保ち、始めると波形の中心に戻る
void testStopAndStart() {
    printf("stop and start\n");
    x68k::Adpcm a;
    a.reset(); // 止まっていて、出力は 0
    a.setPanRate(0, 2); // 1 サンプルは 8MHz の 512 クロック = advance(256)
    // 再生していない間に書いたバイトは使わない
    a.write(3, 0x77);
    a.write(1, 0x02); // PLAY ST (再生を始める)
    CHECK(a.playing());
    CHECK(a.drq());
    a.advance(256);
    CHECK_EQ(a.left(), 0); // データがまだ来ていない: 出力は変わらず、要求は出たまま
    a.write(3, 0x77); // 2 サンプル分 (下位の 4 ビットから使う)
    CHECK(!a.drq()); // 受け取ったので要求は下がる
    CHECK_EQ(a.left(), 0); // 次のサンプルの時刻までは変わらない
    a.advance(256);
    Reference ref;
    int expected = ref.decode(7);
    CHECK_EQ(a.left(), expected); // 1 つ目 (下位の 4 ビット)
    CHECK(a.drq()); // バイトを取り込んだら次を求める (2 サンプルに 1 回)
    a.advance(256);
    expected = ref.decode(7);
    CHECK_EQ(a.left(), expected); // 2 つ目 (上位の 4 ビット)
    CHECK(expected != 0);
    // 再生中の PLAY ST も始め直す (波形は中心、量子化幅は最小に戻り、書いてあったバイトは捨てて次を求める)
    a.write(3, 0x77);
    a.write(1, 0x02); // 再生中の PLAY ST
    CHECK(a.playing());
    CHECK_EQ(a.left(), 0);
    CHECK(a.drq()); // 書いてあったバイトは捨てたので、次を求める
    a.write(3, 0x07);
    a.advance(256);
    ref = Reference();
    expected = ref.decode(7);
    CHECK_EQ(a.left(), expected); // 最初からの復号になる
    // 止める: 要求は下がり、出力は止まったときの値のまま
    a.write(1, 0x01); // STOP
    CHECK(!a.playing());
    CHECK(!a.drq());
    CHECK_EQ(a.read(1), 0xc0);
    a.advance(256 * 100);
    CHECK_EQ(a.left(), expected); // 止まっている間も同じ値
    CHECK_EQ(a.right(), expected);
    // 止まっている間のデータは受け取らない
    a.write(3, 0xff);
    a.advance(256 * 4);
    CHECK_EQ(a.left(), expected); // 止まったまま
    // 始める: 波形の中心 (0) に戻り、量子化幅も最小から
    a.write(1, 0x02);
    CHECK_EQ(a.left(), 0);
    CHECK(a.drq());
    a.write(3, 0x07);
    a.advance(256);
    Reference fresh;
    CHECK_EQ(a.left(), fresh.decode(7));
    // SP と PLAY ST を同時に書いたら止まる
    a.write(1, 0x03);
    CHECK(!a.playing());
    // REC ST (録音) は作っていない。再生は始まらない
    a.write(1, 0x04);
    CHECK(!a.playing());
    CHECK(!a.drq());
    // 偶数番地には何もない
    CHECK_EQ(a.read(0), 0xff);
    CHECK_EQ(a.read(2), 0xff);
    // データ ($E92003) の読み出しは 0 (録音は作っていない)。再生中も同じ
    CHECK_EQ(a.read(3), 0x00);
    a.write(1, 0x02);
    a.write(3, 0x5a);
    CHECK_EQ(a.read(3), 0x00);

    // 再生中の PLAY ST は、書いてあったバイトを捨てる: 次のサンプルの時刻になっても復号せず、要求を出したまま待つ
    a.write(1, 0x02); // 0x5a が書いてある状態で始め直す
    a.advance(256);
    CHECK_EQ(a.left(), 0); // 0x5a は使わない
    CHECK(a.drq());
    a.advance(256);
    CHECK_EQ(a.left(), 0);
    CHECK(a.drq());
    // PLAY ST はサンプルの時刻も数え直す: 半分過ぎたところで PLAY ST を書くと、そこから 1 サンプル分待つ
    a.write(3, 0x07);
    a.advance(128);
    a.write(1, 0x02); // サンプルの途中で始め直す
    a.write(3, 0x07);
    a.advance(255);
    CHECK_EQ(a.left(), 0);
    a.advance(1);
    CHECK_EQ(a.left(), Reference().decode(7));
    // 再生中の REC ST は再生を止める (出力は保つ)
    a.write(3, 0x07);
    a.write(1, 0x04);
    CHECK(!a.playing());
    CHECK(!a.drq());
    CHECK_EQ(a.read(1), 0xc0);
    a.advance(256 * 4);
    CHECK_EQ(a.left(), Reference().decode(7));
    // reset() は再生を止め、出力を中心に戻す。左右と周波数の選択は変えない
    a.write(1, 0x02); // PLAY ST (再生を始める)
    a.write(3, 0x07);
    a.advance(256);
    a.setPanRate(0, 1);
    a.reset();
    CHECK(!a.playing());
    CHECK(!a.drq());
    CHECK_EQ(a.read(1), 0xc0);
    CHECK_EQ(a.left(), 0);
    CHECK_EQ(a.period(), 768u);
    // reset() のあとの PLAY ST は、前に書いてあったバイトを使わない
    a.write(1, 0x02); // PLAY ST (再生を始める)
    a.advance(384);
    CHECK_EQ(a.left(), 0);
    CHECK(a.drq());
}

// 4MHz のクロックと分周: サンプルの間隔が period() (8MHz のクロックの数) になる
void testSampleTiming() {
    printf("sample timing\n");
    static const struct {
        bool clock4M;
        uint8_t rate;
        uint32_t period;
    } kCases[] = {{false, 0, 1024}, {false, 1, 768}, {false, 2, 512}, {false, 3, 512},
                  {true, 0, 2048},  {true, 1, 1536}, {true, 2, 1024}, {true, 3, 1024}};
    for (const auto& k : kCases) {
        x68k::Adpcm a;
        a.reset();
        a.setClock4M(k.clock4M);
        a.setPanRate(0, k.rate);
        CHECK_EQ(a.period(), k.period);
        CHECK_EQ(a.lines(), (k.clock4M ? 0x10u : 0u) | (uint32_t)k.rate << 2);
        a.write(1, 0x02); // PLAY ST (再生を始める)
        // 100 バイト (200 サンプル) を、要求が出るたびに書く。advance() は 4MHz のクロックで数える
        int written = 0;
        uint64_t clocks = 0;
        while (written < 100) {
            if (a.drq()) {
                a.write(3, 0x80);
                written++;
            }
            a.advance(16);
            clocks += 16;
        }
        // 最初のバイトはすぐ、2 つ目は 1 サンプル目で、あとは 2 サンプルごとに 1 バイト: 100 個目は 197 サンプル目。
        // 1 サンプルは 4MHz のクロックで period / 2
        const uint64_t want = (uint64_t)197 * k.period / 2;
        CHECK(clocks >= want && clocks <= want + 32);
    }
}

// テスト用の IPL ROM: 何もせずに回り続ける
struct IdleRom {
    uint8_t b[x68k::Machine::IPL_SIZE];
    IdleRom() {
        memset(b, 0xff, sizeof b);
        const uint8_t vec[8] = {0x00, 0x00, 0x20, 0x00, 0x00, 0xfe, 0x01, 0x00}; // SSP $2000、PC $FE0100
        memcpy(b + 0x10000, vec, 8);
        b[0x100] = 0x60; // bra.s *
        b[0x101] = 0xfe;
    }
};

const int FC = m68k::FC_SUPER_DATA;

void put32(x68k::Machine& m, uint32_t addr, uint32_t v) {
    for (int i = 0; i < 4; i++) m.write8(addr + (uint32_t)i, (uint8_t)(v >> (24 - 8 * i)), FC);
}

// DMAC のチャネル 3 で ADPCM にデータを送る (IOCS の ADPCMOUT と同じ設定)。
// 転送はサンプリング周波数の半分 (1 バイト = 2 サンプル) の速さで進み、終わると COC が立つ
void testDmaChannel3() {
    printf("dma channel 3\n");
    static x68k::Machine m;
    static IdleRom rom;
    m.setIplRom(rom.b, sizeof rom.b);
    m.powerOn();
    const uint32_t buf = 0x10000, n = 2000;
    for (uint32_t i = 0; i < n; i++) m.write8(buf + i, nextByte(), FC);
    const uint32_t ch3 = 0xe840c0;
    m.write8(ch3 + 0x04, 0x80, FC); // DCR: サイクルスチール、68000 型、8 ビットポート
    m.write8(ch3 + 0x05, 0x32, FC); // OCR: メモリ → デバイス、バイト、外部要求 (REQG = 10)
    m.write8(ch3 + 0x06, 0x04, FC); // SCR: MAR を増やす
    m.write8(ch3 + 0x0a, (uint8_t)(n >> 8), FC);
    m.write8(ch3 + 0x0b, (uint8_t)n, FC);
    put32(m, ch3 + 0x0c, buf);
    put32(m, ch3 + 0x14, 0xe92003);
    m.write8(ch3 + 0x29, 5, FC); // MFC
    m.write8(ch3 + 0x31, 5, FC); // DFC
    m.write8(0xe9a005, 0x08, FC); // 8255 ポート C: 分周 512、左右とも出す
    m.write8(ch3 + 0x07, 0x80, FC); // CCR: STR
    // 再生する前は転送しない
    m.run(16000);
    CHECK_EQ(m.read8(ch3 + 0x0a, FC) << 8 | m.read8(ch3 + 0x0b, FC), n);
    m.write8(0xe92001, 0x02, FC); // 再生
    // 0.1 秒: 15625 サンプル / 2 = 781 バイトと、ラッチに入っている 1 バイト
    m.run(1600000);
    uint32_t left = (uint32_t)(m.read8(ch3 + 0x0a, FC) << 8 | m.read8(ch3 + 0x0b, FC));
    CHECK_NEAR(n - left, 782, 3);
    CHECK_EQ(m.read8(0xe92001, FC), 0x40);
    // CT2 = 1 (4MHz) にすると半分の速さになる
    m.write8(0xe90001, 0x1b, FC);
    m.write8(0xe90003, 0x80, FC);
    m.run(1600000);
    uint32_t left2 = (uint32_t)(m.read8(ch3 + 0x0a, FC) << 8 | m.read8(ch3 + 0x0b, FC));
    CHECK_NEAR(left - left2, 391, 3);
    // 最後まで送ると、チャネルは終わる (CSR の COC が立ち、ACT が消える)
    m.run(16000000);
    CHECK_EQ(m.read8(ch3 + 0x00, FC) & 0x88, 0x80);
    m.write8(0xe92001, 0x01, FC);
    CHECK_EQ(m.read8(0xe92001, FC), 0xc0);
}

// 版 12 までのステート (ADPCM の中身の並びが違う) を読む: ADPCM は「再生中か」だけを引き継ぐ。再生の途中で保存した
// ものは再生を始め直すので、DMAC のチャネル 3 の転送は最後まで進む。発振と分周、左右は OPM と 8255 から決まる。
// 止まっているときに保存したものは、DMAC のチャネル 3 が動いていても、止まったまま
void testOldStateWhilePlaying() {
    printf("old state saved while playing\n");
    static x68k::Machine m;
    static IdleRom rom;
    m.setIplRom(rom.b, sizeof rom.b);
    m.powerOn();
    const uint32_t buf = 0x10000, n = 1000;
    for (uint32_t i = 0; i < n; i++) m.write8(buf + i, nextByte(), FC);
    const uint32_t ch3 = 0xe840c0;
    m.write8(ch3 + 0x04, 0x80, FC);
    m.write8(ch3 + 0x05, 0x32, FC);
    m.write8(ch3 + 0x06, 0x04, FC);
    m.write8(ch3 + 0x0a, (uint8_t)(n >> 8), FC);
    m.write8(ch3 + 0x0b, (uint8_t)n, FC);
    put32(m, ch3 + 0x0c, buf);
    put32(m, ch3 + 0x14, 0xe92003);
    m.write8(ch3 + 0x29, 5, FC);
    m.write8(ch3 + 0x31, 5, FC);
    m.write8(0xe9a005, 0x05, FC); // 8255 ポート C: 分周 768、右を切る
    m.write8(ch3 + 0x07, 0x80, FC);
    m.write8(0xe92001, 0x02, FC);
    m.run(800000);
    CHECK(m.sound().adpcm().playing());
    const uint32_t lines = m.sound().adpcm().lines();
    CHECK_EQ(lines, 0x05u);
    for (uint32_t version : {12u, x68k::STATE_VERSION}) {
        const size_t cap = x68k::stateSize(m);
        uint8_t* state = new uint8_t[cap];
        size_t size = 0;
        CHECK(x68k::saveState(m, state, cap, size, version));
        // 読む前に止めて、選択も変えておく
        m.write8(0xe92001, 0x01, FC);
        m.sound().setAdpcmLines(true, 0, 0);
        const char* err = nullptr;
        CHECK(x68k::loadState(m, state, size, err));
        delete[] state;
        CHECK(m.sound().adpcm().playing());
        CHECK_EQ(m.sound().adpcm().lines(), lines);
        CHECK_EQ(m.read8(0xe92001, FC), 0x40);
        if (version < x68k::STATE_REWRITTEN_ADPCM) CHECK_EQ(m.sound().adpcm().left(), 0); // 波形は中心から
        CHECK_EQ(m.sound().adpcm().right(), 0);
    }
    // SP で止めたあと (DMAC のチャネル 3 は転送の途中のまま) に保存した古い版は、読んでも止まったまま。
    // 読む前に鳴らしていた出力は残さない (ADPCM はリセットした状態から始まる)
    m.write8(0xe92001, 0x01, FC);
    CHECK_EQ(m.read8(ch3 + 0x00, FC) & 0x08, 0x08); // ACT
    const uint32_t left = (uint32_t)(m.read8(ch3 + 0x0a, FC) << 8 | m.read8(ch3 + 0x0b, FC));
    CHECK(left > 0);
    for (uint32_t version : {12u}) {
        const size_t cap = x68k::stateSize(m);
        uint8_t* state = new uint8_t[cap];
        size_t size = 0;
        CHECK(x68k::saveState(m, state, cap, size, version));
        m.write8(0xe92001, 0x02, FC); // 読む前に再生して、出力を 0 でない値にしておく
        m.run(160000);
        CHECK(m.sound().adpcm().left() != 0);
        m.sound().setAdpcmLines(true, 0, 0);
        const char* err = nullptr;
        CHECK(x68k::loadState(m, state, size, err));
        delete[] state;
        CHECK(!m.sound().adpcm().playing());
        CHECK(!m.sound().adpcm().drq());
        CHECK_EQ(m.sound().adpcm().left(), 0);
        CHECK_EQ(m.sound().adpcm().lines(), lines);
        CHECK_EQ(m.read8(0xe92001, FC), 0xc0);
        m.run(160000);
        CHECK_EQ((uint32_t)(m.read8(ch3 + 0x0a, FC) << 8 | m.read8(ch3 + 0x0b, FC)), left); // 転送は進まない
    }
    // 再生し直すと、転送は最後まで進む
    m.write8(0xe92001, 0x02, FC);
    m.run(16000000);
    CHECK_EQ(m.read8(ch3 + 0x00, FC) & 0x88, 0x80);
}

// DMAC のチャネル 3 を準備しただけで、PLAY ST を書く前に保存した古い版: 読んでも再生は始まらない
void testOldStateBeforePlay() {
    printf("old state saved before play\n");
    static x68k::Machine m;
    static IdleRom rom;
    m.setIplRom(rom.b, sizeof rom.b);
    m.powerOn();
    const uint32_t ch3 = 0xe840c0, n = 1000;
    m.write8(ch3 + 0x04, 0x80, FC);
    m.write8(ch3 + 0x05, 0x32, FC);
    m.write8(ch3 + 0x06, 0x04, FC);
    m.write8(ch3 + 0x0a, (uint8_t)(n >> 8), FC);
    m.write8(ch3 + 0x0b, (uint8_t)n, FC);
    put32(m, ch3 + 0x0c, 0x10000);
    put32(m, ch3 + 0x14, 0xe92003);
    m.write8(ch3 + 0x29, 5, FC);
    m.write8(ch3 + 0x31, 5, FC);
    m.write8(0xe9a005, 0x08, FC);
    m.write8(ch3 + 0x07, 0x80, FC);
    CHECK_EQ(m.read8(ch3 + 0x00, FC) & 0x08, 0x08);
    for (uint32_t version : {12u, x68k::STATE_VERSION}) {
        const size_t cap = x68k::stateSize(m);
        uint8_t* state = new uint8_t[cap];
        size_t size = 0;
        CHECK(x68k::saveState(m, state, cap, size, version));
        const char* err = nullptr;
        CHECK(x68k::loadState(m, state, size, err));
        delete[] state;
        CHECK(!m.sound().adpcm().playing());
        m.run(160000);
        CHECK_EQ(m.read8(ch3 + 0x0a, FC) << 8 | m.read8(ch3 + 0x0b, FC), n);
    }
}

// OPM のタイマ B の割り込みが MFP の GPIP3 (負論理) に届き、フラグを消すと戻る
void testOpmIrqToMfp() {
    printf("opm irq -> mfp gpip3\n");
    static x68k::Machine m;
    static IdleRom rom;
    m.setIplRom(rom.b, sizeof rom.b);
    m.powerOn();
    CHECK_EQ(m.mfp().gpip(x68k::Mfp::GPIP_OPMIRQ), 1);
    m.write8(0xe90001, 0x12, FC);
    m.write8(0xe90003, 0xff, FC); // NB = 255: 1024 クロック = 256us
    m.write8(0xe90001, 0x14, FC);
    m.write8(0xe90003, 0x0a, FC);
    m.run(16 * 250); // 250us
    CHECK_EQ(m.mfp().gpip(x68k::Mfp::GPIP_OPMIRQ), 1);
    m.run(16 * 10);
    CHECK_EQ(m.mfp().gpip(x68k::Mfp::GPIP_OPMIRQ), 0);
    CHECK_EQ(m.read8(0xe90003, FC) & 3, 2);
    m.write8(0xe90001, 0x14, FC);
    m.write8(0xe90003, 0x2a, FC); // フラグを消す
    CHECK_EQ(m.mfp().gpip(x68k::Mfp::GPIP_OPMIRQ), 1);
    // CT1 は FDC の強制レディ (ドライブにディスクがなくても準備完了に見える)。ここでは書けることだけ確かめる
    m.write8(0xe90001, 0x1b, FC);
    m.write8(0xe90003, 0x40, FC);
    CHECK(m.sound().opm().ct1());
}

// Sound: 出力の周波数に直した音がリングバッファにたまる (1 秒で 48000 フレーム)。
// 440Hz 付近の音を鳴らし、出力の周波数でも同じ音程になる
void testSoundOutput() {
    printf("sound output\n");
    static x68k::Sound s;
    s.setOutputRate(48000);
    x68k::Opm& opm = s.opm();
    auto w = [&](uint8_t r, uint8_t v) {
        opm.write(1, r);
        opm.write(3, v);
    };
    w(0x20, 0xc7);
    w(0x28, 0x4a);
    for (int k = 0; k < 4; k++) {
        w((uint8_t)(0x40 + k * 8), 1);
        w((uint8_t)(0x60 + k * 8), k == 3 ? 0 : 127);
        w((uint8_t)(0x80 + k * 8), 31);
        w((uint8_t)(0xe0 + k * 8), 15);
    }
    w(0x08, 0x40);
    static int16_t out[2 * 48000];
    int got = 0;
    for (int i = 0; i < 4000; i++) { // 4000 x 1000 クロック = 1 秒
        s.advance(1000);
        got += s.ring().drain(out + 2 * got, 48000 - got);
    }
    CHECK_NEAR(got, 48000, 2);
    int cross = 0, first = -1, last = -1;
    // 初めの 0.1 秒は数えない (音の出始めの前に、出力の周波数に直すフィルタの小さな波が 0 をまたぐ)
    for (int i = 4800; i < got; i++)
        if (out[2 * i - 2] < 0 && out[2 * i] >= 0) {
            if (first < 0) first = i;
            last = i;
            cross++;
        }
    double hz = (cross - 1) * 48000.0 / (last - first);
    CHECK(hz > 490 && hz < 493.5); // 440 x 4 / 3.579545 = 491.7
    // リングバッファ: 読み手がいなければ満杯で止まり (捨てる)、readAudio は足りない分を埋める
    for (int i = 0; i < 4000; i++) s.advance(1000);
    CHECK_EQ(s.ring().fill(), x68k::AudioRing::SIZE);
    const int more = (int)x68k::AudioRing::SIZE + 1000;
    static int16_t tmp[2 * (x68k::AudioRing::SIZE + 1000)];
    s.ring().readAudio(tmp, more);
    CHECK_EQ(s.ring().fill(), 0u);
    CHECK_EQ(s.ring().underruns(), 1u);
}

// 速さの合わせ方: 音を出す側が 48000Hz から +-0.1% ずれていても、30 秒の間に足りなくならず、
// たまり具合が目標の近くに収まる (エミュレータの時間で 1ms ずつ進め、512 フレームずつ取り出す)
// ADPCM の復号値が 0 からずれたまま止まっても (無音のデータを流し続けるドライバ)、出力に直流は残らない
// (実機の出力はコンデンサでつながっている)。音の変化はそのまま通る
// 出力の 48kHz の音 (左、n フレーム) から、hz の成分の大きさ (正弦波の振幅) を求める。ハン窓をかけて、その周波数だけを計算する
double toneAmplitude(const int16_t* out, int n, double hz, double rate = 48000) {
    const double pi = 3.14159265358979323846;
    double re = 0, im = 0, wsum = 0;
    for (int i = 0; i < n; i++) {
        const double w = 0.5 - 0.5 * cos(2 * pi * i / n);
        re += w * out[2 * i] * cos(2 * pi * hz * i / rate);
        im += w * out[2 * i] * sin(2 * pi * hz * i / rate);
        wsum += w;
    }
    return 2 * sqrt(re * re + im * im) / wsum;
}

// OPM の 1 つのオペレータだけを最大の音量で鳴らす (正弦波。KC と MUL で高さを決める)。0.5 秒分を捨ててから、
// 続く n フレームを out に取る
void sineThroughSound(uint8_t kc, uint8_t mul, int16_t* out, int n, int rate = 48000) {
    x68k::Sound s;
    s.setOutputRate(rate);
    auto w = [&](uint8_t r, uint8_t v) {
        s.opmWrite(1, r);
        s.opmWrite(3, v);
    };
    w(0x20, 0xc7);
    w(0x28, kc);
    for (int k = 0; k < 4; k++) {
        w((uint8_t)(0x40 + k * 8), mul);
        w((uint8_t)(0x60 + k * 8), k == 0 ? 0 : 127);
        w((uint8_t)(0x80 + k * 8), 31);
        w((uint8_t)(0xe0 + k * 8), 15);
    }
    w(0x08, 0x78);
    static int16_t skip[2 * 4096];
    int got = 0, dropped = 0;
    while (got < n) {
        s.advance(4000);
        if (dropped < rate / 2) dropped += s.ring().drain(skip, 4096);
        else got += s.ring().drain(out + 2 * got, n - got);
    }
}

// 62.5kHz から出力の周波数に直すフィルタ (#296): 表のどの行も合計が同じ、高い音でも関係のない周波数の成分が小さい、
// 出力の周波数の半分より上の音が聞こえる範囲に折り返さない
void testResampler() {
    printf("sound: the windowed-sinc filter to the output rate\n");
    // 表: どの行も合計が 2^15 (一定の入力が、行の並びの周期で揺れない)。行 0 は標本の位置そのもので、16 番目が最大。
    // 行 RESAMPLE_PHASES は次の標本の位置なので、行 0 を 1 つずらしたもの
    for (int p = 0; p <= x68k::Sound::RESAMPLE_PHASES; p++) {
        const int16_t* row = x68k::Sound::resampleRow(p);
        int sum = 0;
        for (int j = 0; j < x68k::Sound::RESAMPLE_TAPS; j++) sum += row[j];
        if (sum != 32768) {
            printf("  row %d sums to %d\n", p, sum);
            CHECK(sum == 32768);
            break;
        }
    }
    const int16_t* first = x68k::Sound::resampleRow(0);
    const int16_t* last = x68k::Sound::resampleRow(x68k::Sound::RESAMPLE_PHASES);
    for (int j = 0; j < x68k::Sound::RESAMPLE_TAPS; j++) CHECK(first[j] <= first[15]);
    for (int j = 0; j + 1 < x68k::Sound::RESAMPLE_TAPS; j++) CHECK_NEAR(last[j + 1], first[j], 1);

    const int n = 16384;
    static int16_t out[2 * 16384];
    // 4.7kHz の正弦波 (KC $7E)。線形補間のときは、62.5kHz で折り返した 57.8kHz の成分が 48kHz で 9.8kHz に出ていた
    // (-44dB)。今は -60dB より小さい
    sineThroughSound(0x7e, 1, out, n);
    double best = 0, bestHz = 0;
    for (double hz = 4600; hz < 4760; hz += 1) {
        const double a = toneAmplitude(out, n, hz);
        if (a > best) best = a, bestHz = hz;
    }
    CHECK(best > 3500 && best < 4200); // 1 オペレータの最大 8191 の 1/2
    const double image = toneAmplitude(out, n, 62500 - bestHz - 48000);
    printf("  %.0f Hz tone: amplitude %.0f, image at %.0f Hz %.1f dB\n", bestHz, best, 62500 - bestHz - 48000,
           20 * log10(image / best + 1e-9));
    CHECK(image < best / 1000);
    // 28kHz の正弦波 (同じ高さの 6 倍)。48kHz では出せない音で、線形補間のときは 20kHz に -10dB で折り返していた。
    // 今は、どの周波数にも元の大きさの 1/300 (-50dB) より大きくは出ない (大きさは実効値で見る)
    sineThroughSound(0x7e, 6, out, n);
    double sq = 0;
    for (int i = 0; i < n; i++) sq += (double)out[2 * i] * out[2 * i];
    const double rms = sqrt(sq / n);
    printf("  %.0f Hz tone: rms %.1f in the output (the tone has rms %.0f)\n", bestHz * 6, rms, best / sqrt(2.0));
    CHECK(rms < best / sqrt(2.0) / 300);
    // 減衰の途中 (18kHz から 26kHz): 23.4kHz の正弦波 (5 倍) は、48kHz では 23.4kHz のまま出る。-8dB から -26dB の間
    // (表だけなら約 -14dB。X68000 の出力の低域通過フィルタの分と合わせて約 -19dB)
    sineThroughSound(0x7e, 5, out, n);
    const double edge = toneAmplitude(out, n, bestHz * 5);
    printf("  %.0f Hz tone: %.1f dB\n", bestHz * 5, 20 * log10(edge / best + 1e-9));
    CHECK(edge < best * 0.4 && edge > best * 0.05);

    // 出力の周波数に合わせた表 (resampleCenterHz())。48kHz 以上はコミットしてある表 (真ん中 22kHz)、低い周波数は
    // 止め始めが出力の周波数の 0.55 倍になるように下げる
    CHECK_EQ(x68k::Sound::resampleCenterHz(48000), 22000);
    CHECK_EQ(x68k::Sound::resampleCenterHz(96000), 22000);
    CHECK_EQ(x68k::Sound::resampleCenterHz(44100), 44100 * 55 / 100 - 4000);
    CHECK_EQ(x68k::Sound::resampleCenterHz(32000), 13600);
    CHECK_EQ(x68k::Sound::resampleCenterHz(8000), 3200); // 0.4 倍より下にはしない
    // 実行時に作る表は、コミットしてある表と同じ式: 真ん中 22kHz で作ると、係数の差は 1 まで。行の合計は 2^15
    {
        static int16_t made[x68k::Sound::RESAMPLE_PHASES + 1][x68k::Sound::RESAMPLE_TAPS];
        x68k::Sound::buildResampleTable(made, 22000);
        int worst = 0, badSum = 0;
        for (int p = 0; p <= x68k::Sound::RESAMPLE_PHASES; p++) {
            int sum = 0;
            for (int j = 0; j < x68k::Sound::RESAMPLE_TAPS; j++) {
                const int d = made[p][j] - x68k::Sound::resampleRow(p)[j];
                if (d > worst) worst = d;
                if (-d > worst) worst = -d;
                sum += made[p][j];
            }
            badSum += sum != 32768;
        }
        CHECK_EQ(worst <= 1, true);
        CHECK_EQ(badSum, 0);
        x68k::Sound low;
        low.setOutputRate(32000);
        CHECK_EQ(low.activeResampleRow(0) != x68k::Sound::resampleRow(0), true);
        int sum = 0;
        for (int j = 0; j < x68k::Sound::RESAMPLE_TAPS; j++) sum += low.activeResampleRow(100)[j];
        CHECK_EQ(sum, 32768);
        low.setOutputRate(48000);
        CHECK_EQ(low.activeResampleRow(0) == x68k::Sound::resampleRow(0), true);
    }
    // 32kHz の出力: 4.7kHz の音はそのまま出て、18.7kHz の音 (4 倍。32kHz では 13.3kHz に折り返す) はほとんど出ない。
    // 48kHz 用の表のままだと、18.7kHz は通す範囲のすぐ上で、ほぼそのままの大きさで折り返す
    sineThroughSound(0x7e, 1, out, n, 32000);
    const double low = toneAmplitude(out, n, bestHz, 32000);
    CHECK(low > best * 0.95 && low < best * 1.05);
    sineThroughSound(0x7e, 4, out, n, 32000);
    sq = 0;
    for (int i = 0; i < n; i++) sq += (double)out[2 * i] * out[2 * i];
    printf("  32000 Hz output: %.0f Hz tone %.0f, %.0f Hz tone leaves rms %.1f\n", bestHz, low, bestHz * 4, sqrt(sq / n));
    CHECK(sqrt(sq / n) < best / sqrt(2.0) / 100);
    // 44.1kHz の出力: 28kHz の音 (6 倍。16kHz に折り返す) はほとんど出ない
    sineThroughSound(0x7e, 6, out, n, 44100);
    sq = 0;
    for (int i = 0; i < n; i++) sq += (double)out[2 * i] * out[2 * i];
    printf("  44100 Hz output: %.0f Hz tone leaves rms %.1f\n", bestHz * 6, sqrt(sq / n));
    CHECK(sqrt(sq / n) < best / sqrt(2.0) / 300);
}

// まーきゅりーゆにっとの PCM (48kHz、44.1kHz) を 62.5kHz に直すフィルタ (#296): 5kHz の正弦波を入れて、出力 (48kHz) に
// 元の音と関係のない周波数の成分が出ない。前は取り出した値をそのまま出していたので、48kHz の PCM では 9.5kHz に
// -22dB、44.1kHz の PCM では 13.4kHz に -23dB の成分が出ていた
void testMercuryPcmFilter() {
    printf("sound: Mercury Unit PCM through the filter to 62.5 kHz\n");
    const double pi = 3.14159265358979323846;
    const int n = 16384;
    static int16_t out[2 * 16384], skip[2 * 4096];
    struct Case {
        int rate;
        uint8_t command;
        double junkHz;
    };
    for (const Case& c : {Case{48000, 0xbe, 9500}, Case{44100, 0x8e, 13400}}) {
        x68k::Sound s;
        s.setOutputRate(48000);
        x68k::Mercury m;
        s.mercuryAttached(true);
        s.mercuryWrite(m, 0x91, c.command); // 等倍、ステレオ、L と R を出す
        CHECK_EQ((int)m.rate(), c.rate);
        long words = 0;
        int got = 0, dropped = 0;
        while (got < n) {
            while (m.drq()) { // L、R の順に同じ値を書く
                const int16_t v = (int16_t)lrint(16000 * sin(2 * pi * 5000 * (double)(words / 2) / c.rate));
                words++;
                s.mercuryWrite(m, 0x80, (uint8_t)(v >> 8));
                s.mercuryWrite(m, 0x81, (uint8_t)v);
            }
            s.advance(16, &m);
            if (dropped < 24000) dropped += s.ring().drain(skip, 4096);
            else got += s.ring().drain(out + 2 * got, n - got);
        }
        const double tone = toneAmplitude(out, n, 5000), junk = toneAmplitude(out, n, c.junkHz);
        printf("  %d Hz PCM: 5000 Hz tone %.0f, %.0f Hz %.1f dB\n", c.rate, tone, c.junkHz, 20 * log10(junk / tone + 1e-9));
        CHECK(tone > 7000 && tone < 8100); // 16000 の 1/2。X68000 の出力の低域通過フィルタで少し下がる
        CHECK(junk < tone / 500);
        // 全体でも、5kHz の音のほかはほとんどない (実効値で見る。振幅 tone の正弦波の実効値は tone / sqrt(2))
        double sq = 0;
        for (int i = 0; i < n; i++) sq += (double)out[2 * i] * out[2 * i];
        const double rest = sq / n - tone * tone / 2;
        CHECK(rest < tone * tone / 2 / 10000); // -40dB より小さい (OPM と ADPCM は鳴っていない)
    }
}

void testDcBlock() {
    printf("dc block\n");
    static x68k::Sound s;
    s.setOutputRate(48000);
    x68k::Adpcm& a = s.adpcm();
    a.setPanRate(0, 2);
    a.write(1, 0x02);
    static int16_t out[2 * 48000];
    int got = 0, peak = 0, bytes = 0;
    for (int i = 0; i < 62500 * 2; i++) { // 2 秒 (OPM の 1 サンプル = 64 クロックずつ)
        if (a.drq()) {
            // 最初に +側へ大きく振ってから、+ と - を交互にして値を保つ
            a.write(3, bytes < 64 ? 0x77 : 0x80);
            bytes++;
        }
        s.advance(64);
        int n = s.ring().drain(out, 48000);
        for (int j = 0; j < n; j++) {
            int v = out[2 * j] < 0 ? -out[2 * j] : out[2 * j];
            if (v > peak) peak = v;
        }
        got += n;
    }
    CHECK(a.left() > 1000);  // 復号値はずれたまま
    CHECK(peak > 4000);      // 振ったときの変化は出る
    int last = out[0] < 0 ? -out[0] : out[0];
    CHECK(last < 100);       // 2 秒後には直流は消えている
    // リセットでフィルタの状態も 0 に戻る: 振った直後にリセットしても、そのあとの出力は 0 のまま
    a.reset();
    a.write(1, 0x02);
    for (int i = 0; i < 200; i++) {
        if (a.drq()) a.write(3, 0x77);
        s.advance(64);
    }
    s.ring().drain(out, 48000);
    s.reset();
    int after = 0;
    for (int i = 0; i < 100; i++) {
        s.advance(64);
        int n = s.ring().drain(out, 48000);
        for (int j = 0; j < 2 * n; j++) after = out[j] < 0 ? (-out[j] > after ? -out[j] : after) : (out[j] > after ? out[j] : after);
    }
    CHECK_EQ(after, 0);
}

void testRateControl() {
    printf("rate control\n");
    for (int sign = -1; sign <= 1; sign += 2) {
        static x68k::Sound s;
        s = x68k::Sound();
        s.setOutputRate(48000);
        const unsigned target = 960; // 20ms
        s.setLatency(target);
        const double consumerRate = 48000.0 * (1 + sign * 0.001);
        double owed = 0;
        static int16_t buf[2 * 512];
        unsigned maxFill = 0, minFill = ~0u;
        for (int ms = 0; ms < 30000; ms++) {
            s.advance(4000);
            owed += consumerRate / 1000;
            while (owed >= 512) {
                s.ring().readAudio(buf, 512);
                owed -= 512;
            }
            if (ms >= 10000) { // 最初の 10 秒で落ち着く
                unsigned f = s.ring().fill();
                if (f > maxFill) maxFill = f;
                if (f < minFill) minFill = f;
            }
        }
        CHECK_EQ(s.ring().underruns(), 0u);
        CHECK_EQ(s.latencyGoal(), target); // 速さのずれだけでは目標は変わらない
        CHECK(minFill > target / 4);
        CHECK(maxFill < target * 2 + 512 + 100);
        printf("  consumer %+.1f%%: fill %u..%u\n", sign * 0.1, minFill, maxFill);
    }
}

// 足りなくなったあとの目標の上げ下げ: 作る側が 200ms 止まって足りなくなると、鳴らし直したあとは多くためる
// (目標に足す)。そのあと足りなくならない 30 秒の区間で、たまり具合の最小値が目標を超えていれば、足した量を
// 少し減らし、たまり具合が下がる。鳴らし直した直後の区間は、ため直す間にたまり具合が 0 近くまで下がって
// いるので減らさず、減らすのはその次の区間 (止まってから約 60 秒後) の終わり
void testLatencyRecovery() {
    printf("latency recovery\n");
    static x68k::Sound s;
    s = x68k::Sound();
    s.setOutputRate(48000);
    const unsigned target = 960; // 20ms
    s.setLatency(target);
    static int16_t buf[2 * 512];
    double owed = 0;
    int ms = 0;
    // until (ms) まで進める。作る側は stallFrom <= ms < stallTo の間だけ止まり、読む側はいつも 48000Hz で
    // 512 フレームずつ読む。返すのは from <= ms < until の間のたまり具合の平均
    auto run = [&](int until, int from, int stallFrom = -1, int stallTo = -1) {
        double sum = 0;
        int n = 0;
        for (; ms < until; ms++) {
            if (ms < stallFrom || ms >= stallTo) s.advance(4000);
            for (owed += 48; owed >= 512; owed -= 512) s.ring().readAudio(buf, 512);
            if (ms >= from) {
                sum += s.ring().fill();
                n++;
            }
        }
        return n ? sum / n : 0.0;
    };
    const double before = run(10000, 5000); // 落ち着いたところ
    CHECK_EQ(s.ring().underruns(), 0u);
    run(10200, 10200, 10000, 10200); // 200ms 止まる
    const unsigned under = s.ring().underruns();
    CHECK(under > 0u);
    const double raised = run(65000, 60000);   // 足した量で落ち着いたところ (まだ減らしていない)
    const double lowered = run(100000, 95000); // 1 回減らしたあと
    CHECK_EQ(s.ring().underruns(), under);     // 鳴らし直してからは足りなくならない
    printf("  fill: before %.0f, after the stall %.0f, after 30s calm %.0f\n", before, raised, lowered);
    CHECK(raised > before + target / 2); // 少なくとも目標の半分は多くためる
    CHECK(lowered < raised - 48);        // 少なくとも 1ms 分は減る
    CHECK(lowered > before + 48 * 10);   // 減らすのは少しずつ (一度に元へは戻さない)
}

// エミュレータが実時間に間に合わない間の音 (#217)。ベアメタル版のコア 1 の進め方をまねる: 1 フレーム (18ms) 分の
// X68000 の時間を busy の時間をかけて作り (音は 0.25ms 分ずつ増える)、フレームの時刻まで待つ。遅れは 250ms までは
// あとで取り戻し (待たずに次のフレームを作る)、それを超えたら基準を取り直す (その分の時間は捨てる)。読む側は HDMI と
// 同じく 768 フレーム (16ms) ずつ読む。busyUs(フレームの番号) が、そのフレームを作るのにかかる時間 (us) を返す。
// 間に合わない間は、Sound が補間の刻みを小さくして多くのフレームを作るので (Sound::stretchIfLow())、足りなくならない
struct LagResult {
    unsigned underruns;    // from 秒より後に足りなくなった回数
    unsigned maxFill;      // from 秒より後のたまり具合の最大
    unsigned maxStretch;   // 刻みを小さくした割合の最大 (千分率)
    unsigned stretchedMs;  // 0.5% を超えて小さくしていた X68000 の時間 (全体)
    unsigned lateStretchedMs; // そのうち最後の 10 秒
    unsigned goal;         // 終わったときのたまり具合の目標
    unsigned goalAfterCalm; // そのあと間に合う速さで calmSeconds 秒動かしたあとの目標
    unsigned underrunsAfterCalm; // そこまでに足りなくなった回数 (from 秒より後)
};
template <typename Busy> LagResult runLag(int seconds, int from, Busy busyUs, int calmSeconds = 0) {
    static x68k::Sound s;
    s = x68k::Sound();
    s.setOutputRate(48000);
    s.setLatency(960); // 20ms (ベアメタル版と同じ)
    LagResult r = {};
    r.goalAfterCalm = 0;
    static int16_t buf[2 * 768];
    static constexpr int FRAME_US = 18000, SLICE_US = 250;
    const int64_t endUs = (int64_t)seconds * 1000000;
    int64_t now = 0, due = 0, nextRead = 0; // 実時間 (us)、今のフレームの時刻、次に読む時刻
    unsigned underAtStart = 0;
    bool counting = false;
    unsigned stretchedSlices = 0, lateSlices = 0;
    for (int frame = 0; now < endUs; frame++) {
        const int busy = busyUs(frame);
        for (int i = 0; i < FRAME_US / SLICE_US; i++) {
            now += busy / (FRAME_US / SLICE_US);
            for (; nextRead <= now; nextRead += 16000) s.ring().readAudio(buf, 768);
            s.advance(SLICE_US * 4);
            const unsigned st = s.stretchPermille();
            if (st > r.maxStretch) r.maxStretch = st;
            if (st > 5) {
                stretchedSlices++;
                if (now >= endUs - 10000000) lateSlices++;
            }
            if (!counting && now >= (int64_t)from * 1000000) {
                counting = true;
                underAtStart = s.ring().underruns();
            }
            if (counting && s.ring().fill() > r.maxFill) r.maxFill = s.ring().fill();
        }
        due += FRAME_US;
        if (now < due) {
            for (; nextRead <= due; nextRead += 16000) s.ring().readAudio(buf, 768);
            now = due; // フレームの時刻まで待つ
        } else if (now - due > 250000) {
            due = now; // 基準を取り直す
        }
    }
    r.underruns = s.ring().underruns() - underAtStart;
    r.stretchedMs = stretchedSlices * SLICE_US / 1000;
    r.lateStretchedMs = lateSlices * SLICE_US / 1000;
    r.goal = s.latencyGoal();
    // このあと calmSeconds 秒、間に合う速さ (1 フレーム 8ms) で動かしたあとの目標
    for (int64_t until = now + (int64_t)calmSeconds * 1000000; now < until;) {
        for (int i = 0; i < FRAME_US / SLICE_US; i++) {
            now += 8000 / (FRAME_US / SLICE_US);
            for (; nextRead <= now; nextRead += 16000) s.ring().readAudio(buf, 768);
            s.advance(SLICE_US * 4);
        }
        due += FRAME_US;
        if (now < due) {
            for (; nextRead <= due; nextRead += 16000) s.ring().readAudio(buf, 768);
            now = due;
        } else if (now - due > 250000) {
            due = now;
        }
    }
    r.goalAfterCalm = s.latencyGoal();
    r.underrunsAfterCalm = s.ring().underruns() - underAtStart;
    return r;
}

void printLag(const char* what, const LagResult& r) {
    printf("  %s: %u underrun(s), fill up to %u, goal %u, stretched %u ms (%u ms in the last 10 s), at most %u.%u%%\n",
           what, r.underruns, r.maxFill, r.goal, r.stretchedMs, r.lateStretchedMs, r.maxStretch / 10,
           r.maxStretch % 10);
}

void testLag() {
    printf("emulator behind real time\n");
    // ふだん (1 フレーム 8ms): 刻みは 0.5% までしか変えず、目標も変わらない
    LagResult calm = runLag(60, 5, [](int) { return 8000; });
    printLag("8 ms frames", calm);
    CHECK_EQ(calm.underruns, 0u);
    CHECK(calm.maxStretch <= 5u);
    CHECK_EQ(calm.goal, 960u);
    // ときどき 1 フレームか 2 フレームが 22ms かかる (平均では間に合う)。2 フレーム続くと 8ms 遅れ、たまり具合が
    // 15ms を切ることがあるので、刻みを少し (5% まで) 小さくすることがある。目標に足すのは 5ms まで
    LagResult jitter = runLag(60, 5, [](int f) { return f % 50 == 0 || f % 170 <= 1 ? 22000 : 8000; });
    printLag("one or two 22 ms frames now and then", jitter);
    CHECK_EQ(jitter.underruns, 0u);
    CHECK(jitter.maxStretch <= 60u);
    CHECK(jitter.stretchedMs <= 200u);
    CHECK_EQ(jitter.lateStretchedMs, 0u);
    CHECK(jitter.goal <= 960u + 240u);
    // 2 秒ごとに 10 フレーム続けて 22ms かかり (40ms 遅れる)、そのあと取り戻す。はじめの何回かは刻みを小さくし、
    // 多く作った分だけ目標が上がると、あとはためた分で足りる (最後の 10 秒は小さくしない)
    LagResult burst = runLag(60, 5, [](int f) { return f % 111 < 10 ? 22000 : 8000; });
    printLag("ten 22 ms frames every 2 s", burst);
    CHECK_EQ(burst.underruns, 0u);
    CHECK(burst.stretchedMs > 0u);
    CHECK_EQ(burst.lateStretchedMs, 0u);
    CHECK(burst.goal > 960u && burst.goal < 960u + 48 * 60); // 足すのは遅れの分 (40ms) ほどまで
    // 22ms のフレームが続く (速さは 82%。遅れが 250ms を超えるたびに時間を捨てる)。10 秒から 40 秒まで
    // そのあと 5 分、間に合う速さで動かすと、上がった目標が下がる (30 秒ごとに 1/8 ずつ。元の 20ms までは戻りきらない)
    LagResult steady = runLag(60, 10, [](int f) { return f >= 555 && f < 555 + 1364 ? 22000 : 8000; }, 300);
    printLag("22 ms frames for 30 s", steady);
    printf("  goal after 5 more minutes at full speed: %u\n", steady.goalAfterCalm);
    CHECK(steady.goal > 960u && steady.goal <= x68k::AudioRing::SIZE / 2); // 多く作った分だけ上がる (上限はリングバッファの半分)
    CHECK(steady.goalAfterCalm < steady.goal - steady.goal / 4);          // 5 分で 1/4 より多く下がる
    CHECK(steady.goalAfterCalm >= 960u);
    CHECK_EQ(steady.underrunsAfterCalm, 0u);
    CHECK_EQ(steady.underruns, 0u);
    CHECK(steady.maxStretch >= 150u && steady.maxStretch <= 250u);
    CHECK(steady.stretchedMs > 20000u);
    CHECK_EQ(steady.lateStretchedMs, 0u); // 間に合うようになれば戻る
    // 19ms のフレームが続く (95%)
    LagResult mild = runLag(60, 10, [](int f) { return f >= 555 && f < 555 + 1580 ? 19000 : 8000; });
    printLag("19 ms frames for 30 s", mild);
    CHECK_EQ(mild.underruns, 0u);
    CHECK(mild.goal > 960u && mild.goal <= x68k::AudioRing::SIZE / 2);
    CHECK_EQ(mild.lateStretchedMs, 0u);
    // リングバッファ (16384 フレーム) からあふれない
    CHECK(burst.maxFill < 16000u && steady.maxFill < 16000u && mild.maxFill < 16000u);
    // 刻みを小さくするのは 25% まで。それより遅いと途切れる (止まりはしない)
    LagResult tooSlow = runLag(60, 10, [](int f) { return f >= 555 && f < 555 + 1000 ? 30000 : 8000; });
    printLag("30 ms frames for 30 s", tooSlow);
    CHECK(tooSlow.underruns > 0u);
    CHECK(tooSlow.maxStretch >= 240u && tooSlow.maxStretch <= 250u);
}

// 出力の周波数が高いときと低いときも、0.25 秒ごとの調整と、間に合わない間の調整が働く (#217 の見直し)。
// 0.25 秒のフレーム数は、262144Hz 以上では 16 ビットに入らない (SDL は、開いたデバイスの周波数をそのまま渡す)。
// 32Hz より低いと、1/32 秒のフレーム数が 0 になる (0 で割らない)
void testRateRange() {
    printf("output rates outside the usual range\n");
    static int16_t buf[2 * 4096];
    // rate Hz で、読む側が 1ms ごとに rate/1000 フレームを読む。作る側は stallFrom <= ms < stallTo の間だけ止まり、
    // slowFrom <= ms の間は 4 回に 1 回休む (75% の速さ)
    struct Result {
        unsigned goalAfterStall, goalAtEnd, underruns, maxStretch;
    };
    auto run = [&](int rate, unsigned latency, int stallFrom, int stallTo, int slowFrom, int until) {
        static x68k::Sound s;
        s = x68k::Sound();
        s.setOutputRate(rate);
        s.setLatency(latency);
        Result r = {};
        double owed = 0;
        for (int ms = 0; ms < until; ms++) {
            const bool stalled = ms >= stallFrom && ms < stallTo;
            const bool slow = slowFrom >= 0 && ms >= slowFrom && ms % 4 == 0;
            if (!stalled && !slow) s.advance(4000);
            for (owed += rate / 1000.0; owed >= 1; owed -= 1) s.ring().readAudio(buf, 1);
            if (ms == stallTo + 2000) r.goalAfterStall = s.latencyGoal();
            if (slowFrom >= 0 && ms >= slowFrom && s.stretchPermille() > r.maxStretch) r.maxStretch = s.stretchPermille();
        }
        r.goalAtEnd = s.latencyGoal();
        r.underruns = s.ring().underruns();
        return r;
    };
    // 384kHz、目標は 20ms (7680 フレーム)。5 秒のところで 30ms 止まると足りなくなり、0.25 秒ごとの調整が目標を上げる。
    // そのあと 100 秒足りなくならなければ、目標を下げる。どちらも、0.25 秒のフレーム数 (96000) まで数えられないと起きない
    {
        const Result r = run(384000, 7680, 5000, 5030, -1, 110000);
        printf("  384000 Hz: %u underrun(s), goal %u after a stall, %u after 100 s\n", r.underruns, r.goalAfterStall,
               r.goalAtEnd);
        CHECK(r.underruns > 0u);
        CHECK(r.goalAfterStall > 7680u);
        CHECK(r.goalAtEnd < r.goalAfterStall);
    }
    // 31Hz と 3Hz、目標は 8 フレーム。5 秒から 75% の速さにすると、たまり具合が目標の 3/4 を切り、刻みを小さくする
    for (int rate : {31, 3}) {
        const Result r = run(rate, 8, -1, -1, 5000, 60000);
        printf("  %d Hz: stretched at most %u.%u%%, goal %u\n", rate, r.maxStretch / 10, r.maxStretch % 10, r.goalAtEnd);
        CHECK(r.maxStretch > 5u && r.maxStretch <= 250u);
        CHECK(r.goalAtEnd >= 8u);
    }
    // 大きさと並びは #217 の前と同じ (ステートには入らない変数だが、Machine の中の並びを変えない)
    printf("  sizeof(Sound) = %u\n", (unsigned)sizeof(x68k::Sound));
}

} // namespace

int main() {
    testDecode();
    testStepTable();
    testPlayback();
    testStopAndStart();
    testSampleTiming();
    testDmaChannel3();
    testOldStateWhilePlaying();
    testOldStateBeforePlay();
    testOpmIrqToMfp();
    testSoundOutput();
    testResampler();
    testMercuryPcmFilter();
    testDcBlock();
    testRateControl();
    testLatencyRecovery();
    testLag();
    testRateRange();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
