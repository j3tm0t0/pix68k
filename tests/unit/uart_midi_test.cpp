// UART の MIDI の出力 (platform/uart_midi.h): 送る速さの配分 (320us / バイト、FIFO をあふれさせない)、溜まった量、
// 落とさないこと、あふれたときの捨て方、鳴っている音を止めるバイトの差し込み。
// 下の Wire は PL011 の送信を真似る: FIFO は 16 バイト、送っている 1 バイトは FIFO の外、1 バイト 320us。
#include <stdint.h>
#include <stdlib.h>

#include <atomic>
#include <thread>
#include <algorithm>
#include <vector>

#include "platform/uart_midi.h"
#include "test_check.h"

using platform::UartMidi;

namespace {

struct Wire : UartMidi {
    explicit Wire(Family f = GENERAL, int fifo = DEFAULT_FIFO) : UartMidi(f, fifo) {}
    std::vector<uint8_t> bytes;
    std::vector<uint64_t> done;  // 送り終える時刻
    uint64_t now = 0;            // pump() に渡す時刻 (put() の中で見る)
    uint64_t clock = 0;          // nowUs() が返す時刻
    int overflows = 0;           // FIFO が満杯なのに書いた回数 (実機なら put() が待つ)
    unsigned maxQueued = 0;
    void put(uint8_t b) override {
        // 今送り終えていないバイトの数 (送っている 1 つ + FIFO)
        unsigned pending = 0;
        for (size_t i = done.size(); i-- > 0 && done[i] > now;) pending++;
        if (pending > 16) overflows++; // 送っている 1 つ + FIFO 16 が満杯
        if (pending + 1 > maxQueued) maxQueued = pending + 1;
        const uint64_t start = done.empty() || done.back() < now ? now : done.back();
        done.push_back(start + 320);
        bytes.push_back(b);
    }
    uint64_t nowUs() override { return clock; }
    void positionAt(uint64_t pos) { setPositionForTest(pos); }
    int pumpAt(uint64_t t) {
        now = t;
        return pump(t);
    }
};

// 止めるバイト (gs は CC 120 と 123、mt32 は CC 64 と 123)
std::vector<uint8_t> silenceBlock(bool mt32) {
    std::vector<uint8_t> v;
    for (int ch = 0; ch < 16; ch++) {
        v.push_back((uint8_t)(0xb0 | ch));
        v.push_back(mt32 ? 64 : 120);
        v.push_back(0);
        v.push_back(123);
        v.push_back(0);
    }
    return v;
}

void feed(UartMidi& u, std::initializer_list<int> bytes) {
    for (int b : bytes) u.midiByte((uint8_t)b);
}

std::vector<uint8_t> drain(Wire& w) {
    for (int i = 0; i < 100000 && !w.idle(); i++) w.pumpAt(w.now + 1000);
    return w.bytes;
}

bool sameAs(const std::vector<uint8_t>& got, std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> want;
    for (const auto& p : parts) want.insert(want.end(), p.begin(), p.end());
    if (got == want) return true;
    printf("  got %zu bytes, want %zu:", got.size(), want.size());
    for (size_t i = 0; i < got.size() && i < 40; i++) printf(" %02x", got[i]);
    printf("\n");
    return false;
}

// 初めの midiReset() (電源を入れたとき) を済ませておく
void boot(Wire& w) {
    w.midiReset();
    drain(w);
    w.bytes.clear(); // done は残す (FIFO に残っている分を真似るため)
}

// 大量のバイトを一度に受けても、FIFO があふれず、320us ごとに途切れずに送り、1 バイトも落とさない
void testBurstPacing() {
    for (int interval : {100, 1000, 3000}) {
        Wire w;
        w.setPumper(UartMidi::OTHER);
        boot(w);
        const uint64_t t0 = 1000000;
        w.now = t0;
        std::vector<uint8_t> want;
        for (int i = 0; i < 3000; i++) {
            const uint8_t m[3] = {(uint8_t)(0x90 | (i & 15)), (uint8_t)(i % 128), (uint8_t)(1 + i % 127)};
            for (uint8_t b : m) {
                w.midiByte(b);
                want.push_back(b);
            }
        }
        CHECK_EQ(w.backlog(), 9000);
        CHECK_EQ(w.maxBacklog(), 9000);
        CHECK_EQ(w.bytes.size(), 0); // OTHER なので書き手は送らない
        // interval us ごとに pump() する (少しずらす)
        uint64_t t = t0;
        srand(1);
        while (!w.idle()) {
            w.pumpAt(t);
            t += (uint64_t)interval + (uint64_t)(rand() % 50);
        }
        CHECK_EQ(w.overflows, 0);
        CHECK_EQ(w.dropped(), 0);
        CHECK_EQ(w.sent(), 80 + 9000); // 80 は boot() の分
        CHECK_EQ(w.bytes == want, true);
        CHECK_EQ(w.maxQueued <= (unsigned)UartMidi::DEFAULT_FIFO, true);
        // FIFO (12 バイト = 3.84ms) より短い間隔で呼べば、線は休まない: 最後のバイトは 9000 × 320us で送り終える
        if (interval < 3000) CHECK_EQ(w.done.back(), t0 + 9000ull * 320);
        // 続けて送るときの間隔は 320us
        bool spaced = true;
        for (size_t i = 1; i < w.done.size(); i++) spaced &= w.done[i] - w.done[i - 1] >= 320;
        CHECK_EQ(spaced, true);
    }
}

// MIDI ボードより少し遅い速さ (400us ごとに 1 バイト) で受け、ときどき 20ms 呼ばれない間があっても、溜まった分は
// 追いつく (MIDI ボードの最高の速さ 320us で送り続けられると、呼ばれない間に線が休んだ分は取り戻せない)
void testSteady() {
    Wire w;
    w.setPumper(UartMidi::OTHER);
    boot(w);
    uint64_t t = 1000000;
    int fed = 0;
    for (int step = 0; step < 20000; step++) {
        t += 400;
        w.midiByte(fed % 3 == 0 ? 0x90 : (uint8_t)(fed % 100));
        fed++;
        if (step % 500 < 440 && step % 3 == 0) w.pumpAt(t); // 1ms ごと、500 回に 60 回 (19ms) は呼ばない
    }
    w.pumpAt(t);
    CHECK_EQ(w.overflows, 0);
    CHECK_EQ(w.dropped(), 0);
    CHECK_EQ(w.backlog() < 80, true); // 19ms 分 (60 バイト) と少し
    while (!w.idle()) w.pumpAt(t += 1000);
    CHECK_EQ(w.sent(), 80 + (uint32_t)fed);
}

// リングがあふれたら捨てて数える。捨てたあとは次のステータスのバイトまでデータを捨てる。待たない
void testOverflow() {
    Wire w;
    w.setPumper(UartMidi::OTHER);
    boot(w);
    const uint32_t n = UartMidi::RING_SIZE;
    for (uint32_t i = 0; i < n / 3; i++) feed(w, {0x90, 0x40, 0x7f});
    const uint32_t queued = w.backlog();
    CHECK_EQ(queued, n / 3 * 3);
    // 残りを埋めて、さらに 2 つのメッセージを送る (1 つ目の途中であふれる)
    for (uint32_t i = queued; i < n; i++) w.midiByte(0x22);
    feed(w, {0x91, 0x41, 0x7f, 0x42, 0x7f});
    CHECK_EQ(w.dropped(), 5);
    // 空いたあとも、データのバイトはステータスまで捨てる
    w.pumpAt(w.now + 100000);
    feed(w, {0x43, 0x7f, 0x80, 0x40, 0x00});
    CHECK_EQ(w.dropped(), 7);
    std::vector<uint8_t> got = drain(w);
    CHECK_EQ(got.size(), n + 3);
    CHECK_EQ(got[n], 0x80);
    CHECK_EQ(w.overflows, 0);
}

// リセット: メッセージの途中なら、止めるバイトのあとにステータスと受けたデータを送り直す
void testResetMidMessage() {
    Wire w;
    w.setPumper(UartMidi::OTHER);
    boot(w);
    feed(w, {0x90, 0x3c, 0x64, 0x3c});
    w.midiReset();
    feed(w, {0x00, 0x3e, 0x50});
    CHECK_EQ(sameAs(drain(w), {{0x90, 0x3c, 0x64, 0x3c}, silenceBlock(false), {0x90, 0x3c}, {0x00, 0x3e, 0x50}}), true);
    CHECK_EQ(w.resets(), 2);
}

// システムエクスクルーシブの途中なら $F7 で閉じ、残りのデータと $F7 は捨てる
void testResetInSysex() {
    Wire w;
    w.setPumper(UartMidi::OTHER);
    boot(w);
    feed(w, {0x90, 0x3c, 0x64, 0xf0, 0x41, 0x10});
    w.midiReset();
    feed(w, {0x42, 0x12, 0xf7, 0x55, 0xc0, 0x05});
    CHECK_EQ(sameAs(drain(w), {{0x90, 0x3c, 0x64, 0xf0, 0x41, 0x10, 0xf7}, silenceBlock(false), {0xc0, 0x05}}), true);
}

// 前に止めてからチャネルメッセージを送っていなければ、何も送らない。MT-32 ではホールドペダルを離す
void testResetSkipAndFamily() {
    Wire w(UartMidi::MT32);
    w.setPumper(UartMidi::OTHER);
    CHECK_EQ(w.midiFamily(), UartMidi::MT32);
    w.midiReset(); // 初め: 何が鳴っているか分からないので送る
    CHECK_EQ(sameAs(drain(w), {silenceBlock(true)}), true);
    w.bytes.clear();
    w.midiReset();
    feed(w, {0xf0, 0x41, 0xf7, 0xf8}); // システムのメッセージだけなら止めない
    w.midiReset();
    w.requestSilence();
    CHECK_EQ(sameAs(drain(w), {{0xf0, 0x41, 0xf7, 0xf8}}), true);
    CHECK_EQ(w.resets(), 1);
}

// ほかのコアからの頼み (一時停止): リングに溜まったバイトを送り終えてから差し込む
void testRequestSilence() {
    Wire w;
    w.setPumper(UartMidi::OTHER);
    boot(w);
    feed(w, {0x90, 0x3c, 0x64});
    drain(w);
    w.bytes.clear();
    feed(w, {0x3e, 0x64});
    w.requestSilence();
    CHECK_EQ(w.idle(), false);
    CHECK_EQ(sameAs(drain(w), {{0x3e, 0x64}, silenceBlock(false), {0x90}}), true);
    CHECK_EQ(w.idle(), true);
    CHECK_EQ(w.drainedAt(), w.done.back());
    // FIFO が満杯でも、頼みは残る (送るものが残っている間は idle() にならない)
    w.bytes.clear();
    feed(w, {0x3c, 0x7f});
    for (int i = 0; i < 20; i++) w.midiByte((uint8_t)(i & 1 ? 0x00 : 0x3c));
    w.pumpAt(w.now + 100000);
    w.requestSilence();
    w.pumpAt(w.now);
    CHECK_EQ(w.idle(), false);
    std::vector<uint8_t> got = drain(w);
    CHECK_EQ(got.size(), 22u + 80u + 1u);
    CHECK_EQ(got[22], 0xb0);
    CHECK_EQ(w.overflows, 0);
}

// ステータスのないデータのバイトは送らない。システムコモンのデータは送る
void testOrphanData() {
    Wire w;
    w.setPumper(UartMidi::OTHER);
    boot(w);
    feed(w, {0x10, 0xf2, 0x01, 0x02, 0x03, 0xf3, 0x04, 0x05, 0xb0, 0x07, 0x64});
    CHECK_EQ(sameAs(drain(w), {{0xf2, 0x01, 0x02, 0xf3, 0x04, 0xb0, 0x07, 0x64}}), true);
}

// SELF: 書き手が送る (midiByte() と idleWork())
void testSelfPump() {
    Wire w;
    w.setPumper(UartMidi::SELF);
    w.clock = 5000;
    w.now = 5000;
    w.midiReset(); // 80 バイトのうち 12 バイトだけ送る
    CHECK_EQ(w.bytes.size(), (size_t)UartMidi::DEFAULT_FIFO);
    CHECK_EQ(w.idleWork(), true); // 時刻が進んでいないので送れないが、残っているので true (眠らない)
    CHECK_EQ(w.bytes.size(), (size_t)UartMidi::DEFAULT_FIFO);
    w.clock = w.now = 5000 + 320 * 4;
    CHECK_EQ(w.idleWork(), true);
    CHECK_EQ(w.bytes.size(), (size_t)UartMidi::DEFAULT_FIFO + 4);
    w.clock = w.now = 1000000;
    CHECK_EQ(w.idleWork(), true); // 残りの 64 バイトのうち 12 バイト
    while (w.bytes.size() < 80) {
        w.clock = w.now += 320;
        w.idleWork();
    }
    CHECK_EQ(w.idleWork(), false); // 送り終えたら眠ってよい
    CHECK_EQ(w.overflows, 0);
    // NONE なら送らない
    Wire n;
    n.midiByte(0x90);
    CHECK_EQ(n.idleWork(), false);
    CHECK_EQ(n.bytes.size(), 0);
}

// 速さを配分しない (ホスト版でファイルに書くとき)
void testUnpaced() {
    Wire w(UartMidi::GENERAL, 0);
    w.setPumper(UartMidi::SELF);
    for (int i = 0; i < 5000; i++) w.midiByte((uint8_t)(i % 3 == 0 ? 0x90 : 0x40));
    CHECK_EQ(w.bytes.size(), 5000u);
    CHECK_EQ(w.backlog(), 0);
}

// 書き手と読み手が別のスレッド
void testThreads() {
    struct Sink : UartMidi {
        Sink() : UartMidi(GENERAL, 0) {}
        std::vector<uint8_t> bytes;
        void put(uint8_t b) override { bytes.push_back(b); }
        uint64_t nowUs() override { return 0; }
    } s;
    s.setPumper(UartMidi::OTHER);
    const int N = 300000;
    std::thread producer([&] {
        for (int i = 0; i < N; i++) {
            // 読み手が遅れても捨てないよう、溜まりすぎたら待つ (試験のためだけ)
            while (s.backlog() > UartMidi::RING_SIZE - 16) std::this_thread::yield();
            s.midiByte(i % 3 == 0 ? (uint8_t)(0x80 | ((i / 3) & 0x1f)) : (uint8_t)(i & 0x7f));
        }
    });
    while ((int)s.bytes.size() < N) {
        s.pump(0);
        if (s.idle()) std::this_thread::yield();
    }
    producer.join();
    bool ok = s.bytes.size() == (size_t)N;
    for (int i = 0; ok && i < N; i++)
        ok = s.bytes[i] == (i % 3 == 0 ? (uint8_t)(0x80 | ((i / 3) & 0x1f)) : (uint8_t)(i & 0x7f));
    CHECK_EQ(ok, true);
    CHECK_EQ(s.dropped(), 0);
}

// 再起動の前 (期限つきの頼み): 300ms では送り切れないほど溜まっていたら、溜まったバイトを捨てて止めるバイトを先に送り、
// 期限までに UART から出し終える
void testSilenceDeadline() {
    for (uint32_t queued : {3000u, 6400u, 100u}) {
        Wire w;
        w.setPumper(UartMidi::OTHER);
        boot(w);
        w.now = 1000000;
        feed(w, {0x90, 0x3c, 0x7f}); // ノートオン
        w.pumpAt(w.now);
        for (uint32_t i = 0; i < queued; i++) w.midiByte((uint8_t)(i % 3 == 0 ? 0x91 : 0x40));
        const uint64_t t0 = w.now + 50;
        const uint64_t deadline = t0 + 300000 - 1000; // CKernel::SilenceMidiUart(300) と同じ
        const uint32_t resets = w.resets();
        const uint32_t gen = w.requestSilence(deadline);
        CHECK_EQ(w.silenced(gen), false);
        CHECK_EQ(w.idle(), false);
        uint64_t t = t0;
        while (!w.silenced(gen) && t < t0 + 1000000) w.pumpAt(t += 1000); // 主ループと同じく 1ms ごと
        CHECK_EQ(w.silenced(gen), true);
        CHECK_EQ(w.resets(), resets + 1);
        CHECK_EQ(w.drainedAt() <= deadline, true); // 止めるバイトも期限までに出し終える
        CHECK_EQ(w.overflows, 0);
        // 最後は止めるバイト。捨てなかったときは、そのあとに送り直すランニングステータス $91 (捨てたときは、続きを
        // 捨てたので送り直さない)
        const std::vector<uint8_t>& b = w.bytes;
        const std::vector<uint8_t> block = silenceBlock(false);
        const long rs = queued > 1000 ? 0 : 1;
        CHECK_EQ(b.size() > block.size(), true);
        CHECK_EQ(std::equal(block.begin(), block.end(), b.end() - (long)block.size() - rs), true);
        if (rs) CHECK_EQ(b.back(), 0x91);
        if (queued > 1000) {
            CHECK_EQ(w.flushed() > 0, true);
            CHECK_EQ(w.sent() - 80 - 3 - 80 + w.flushed(), queued);
        } else {
            CHECK_EQ(w.flushed(), 0); // 間に合うなら捨てない
            CHECK_EQ(w.sent(), 80u + 3u + queued + 81u);
        }
        CHECK_EQ(w.idle(), true);
    }
    // 期限なし (メニュー) なら捨てない
    Wire w;
    w.setPumper(UartMidi::OTHER);
    boot(w);
    feed(w, {0x90, 0x3c, 0x7f});
    for (int i = 0; i < 3000; i++) w.midiByte(0x40);
    const uint32_t gen = w.requestSilence();
    while (!w.silenced(gen)) w.pumpAt(w.now + 1000);
    CHECK_EQ(w.flushed(), 0);
    CHECK_EQ(w.sent(), 80u + 3u + 3000u + 81u);
}

// 止める頼みを済ませたかの判定が、読み手が頼みを受け取ってから止めるバイトを作るまでの間に true にならないこと。
// 読み手は別のスレッドで回し続け、書き手はノートオンを送ってから頼み、silenced() か idle() が true になったら、
// その時点で止めるバイトを書き終えているかを見る
void testSilenceRace() {
    struct Sink : UartMidi {
        Sink() : UartMidi(GENERAL, 0) {}
        uint8_t buf[1 << 22]; // 1 回 84 バイト。PIX68K_RACE_ROUNDS で 49000 回まで
        std::atomic<uint32_t> count{0};
        void put(uint8_t b) override {
            // 書く前に少し待つ (済ませたことを書く前に見せる誤りがあれば、その間に書き手が見つけられるように)
            for (volatile int i = 0; i < 300; i = i + 1) {
            }
            const uint32_t n = count.load(std::memory_order_relaxed);
            if (n < sizeof buf) buf[n] = b;
            count.store(n + 1, std::memory_order_release);
        }
        uint64_t nowUs() override { return 0; }
    };
    Sink* s = new Sink;
    s->setPumper(UartMidi::OTHER);
    std::atomic<bool> stop{false};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) s->pump(0);
    });
    const std::vector<uint8_t> block = silenceBlock(false);
    int bad = 0, viaIdle = 0;
    const char* env = getenv("PIX68K_RACE_ROUNDS");
    const int rounds = env ? atoi(env) : 3000;
    for (int i = 0; i < rounds; i++) {
        const uint32_t before = s->count.load(std::memory_order_acquire);
        s->midiByte(0x90);
        s->midiByte(0x3c);
        s->midiByte(0x7f);
        const uint32_t gen = s->requestSilence();
        bool idleFirst = false;
        for (;;) {
            if (s->silenced(gen)) break;
            if (s->idle()) { // idle() でも、済ませる前には true にならない
                idleFirst = true;
                break;
            }
        }
        const uint32_t n = s->count.load(std::memory_order_acquire);
        // 書いたバイトは ... 90 3c 7f [止めるバイト 80] 90。この回の分だけで 84 バイト (前の回のバイトで通らないように)
        bool ok = n - before == block.size() + 4 && n <= sizeof s->buf && s->buf[before] == 0x90 &&
                  std::equal(block.begin(), block.end(), s->buf + n - block.size() - 1) && s->buf[n - 1] == 0x90 &&
                  s->buf[n - block.size() - 2] == 0x7f;
        if (idleFirst) {
            viaIdle++;
            ok = ok && s->silenced(gen);
        }
        if (!ok) bad++;
    }
    stop.store(true);
    reader.join();
    CHECK_EQ(bad, 0);
    CHECK_EQ(s->resets(), (uint32_t)rounds);
    printf("  silence race: %d of %d ended by idle()\n", viaIdle, rounds);
    delete s;
}

// pump() が頼みの番号を読んでからリングの書き込み位置を読むまでの間に、書き手がノートオンと止める頼みを入れても、
// ノートオンを止めるバイトのあとに送らない (番号を先に読むので、その頼みは次の pump() で、ノートオンのあとに済ませる)
void testSilenceReadOrder() {
    struct Hook : Wire {
        bool armed = false;
        uint32_t gen = 0;
        void betweenReads() override {
            if (!armed) return;
            armed = false;
            feed(*this, {0x90, 0x3c, 0x7f});
            gen = requestSilence();
        }
    } w;
    w.setPumper(UartMidi::OTHER);
    boot(w);
    feed(w, {0x80, 0x3c, 0x00}); // 止めるバイトを送るよう、音のメッセージを送っておく
    drain(w);
    w.bytes.clear();
    w.armed = true;
    w.pumpAt(w.now + 1000); // この中で書き手がノートオンと頼みを入れる
    CHECK_EQ(w.armed, false);
    while (!w.silenced(w.gen)) w.pumpAt(w.now + 1000);
    drain(w);
    CHECK_EQ(sameAs(w.bytes, {{0x90, 0x3c, 0x7f}, silenceBlock(false), {0x90}}), true);
    CHECK_EQ(w.backlog(), 0);
}

// メニューで止めて、止めるバイトを送り終える前に再開しても、止めるバイトは頼んだときに溜まっていたバイトのすぐあとに入り、
// 再開したあとのバイトはそのあとに送る (溜まったバイトが空になるのを待って、再開後の演奏の途中で止めない)
void testSilenceMarkOnResume() {
    Wire w;
    w.setPumper(UartMidi::OTHER);
    boot(w);
    uint64_t t = 1000000;
    w.now = t;
    std::vector<uint8_t> before;
    for (int i = 0; i < 1000; i++)
        for (uint8_t b : {(uint8_t)0x90, (uint8_t)(i & 0x7f), (uint8_t)0x40}) {
            w.midiByte(b);
            before.push_back(b);
        }
    const uint32_t gen = w.requestSilence(); // メニュー (期限なし)
    for (int i = 0; i < 100; i++) w.pumpAt(t += 1000); // 100ms 後に再開
    std::vector<uint8_t> after;
    uint64_t silencedAt = 0;
    for (int i = 0; i < 3000; i++) { // 再開後は 1ms ごとに 3 バイト (31.25kbps より少し遅い)
        for (uint8_t b : {(uint8_t)0x91, (uint8_t)(i & 0x7f), (uint8_t)0x40}) {
            w.midiByte(b);
            after.push_back(b);
        }
        w.pumpAt(t += 1000);
        if (silencedAt == 0 && w.silenced(gen)) silencedAt = t;
    }
    while (!w.idle()) w.pumpAt(t += 1000);
    CHECK_EQ(silencedAt != 0, true);
    // 3000 バイトと止めるバイトを送る時間 (約 1 秒) で済む
    CHECK_EQ(silencedAt - 1000000 < 1100000, true);
    // 止めたあとはランニングステータス $90 を送り直してから、再開後のバイト
    CHECK_EQ(sameAs(w.bytes, {before, silenceBlock(false), {0x90}, after}), true);
    CHECK_EQ(w.dropped() + w.flushed(), 0);
}

// 期限つきで捨てるとき、送っていたメッセージの途中から捨てても、受け取る側の状態に合わせて閉じるか送り直す
void testSilenceDeadlineMidMessage() {
    struct Case {
        std::vector<uint8_t> first; // 捨てる前に送る 12 バイト (FIFO に入る分)
        std::vector<uint8_t> rest;  // 続き (溜まっていて捨てる)
        std::vector<uint8_t> tail;  // 止めるバイトのあとに送るもの
        bool closeSysex;            // 止めるバイトの前に $F7
    };
    const std::vector<uint8_t> head9 = {0x90, 0x3c, 0x7f, 0xb0, 0x07, 0x64, 0xb0, 0x07, 0x64};
    std::vector<Case> cases;
    // チャネルメッセージの途中 ($91 3c まで送った)
    cases.push_back({{0xc0, 0x91, 0x3c}, {0x7f}, {}, false}); // 続きを捨てたので $91 3c は送り直さない
    // システムエクスクルーシブの途中
    cases.push_back({{0xf0, 0x41, 0x10}, {0x42, 0x12}, {}, true});
    // システムコモンの途中 ($F2 は 2 バイトのデータ)
    cases.push_back({{0x05, 0xf2, 0x01}, {0x02}, {}, false});
    for (const Case& c : cases) {
        Wire w;
        w.setPumper(UartMidi::OTHER);
        boot(w);
        w.now = 1000000;
        for (uint8_t b : head9) w.midiByte(b);
        for (uint8_t b : c.first) w.midiByte(b);
        for (uint8_t b : c.rest) w.midiByte(b);
        for (int i = 0; i < 3000; i++) w.midiByte(0x22); // 続きのデータ (捨てる)
        w.midiByte(0xf7);
        w.pumpAt(w.now); // 12 バイトだけ送る
        CHECK_EQ(w.bytes.size(), 12u);
        const uint64_t deadline = w.now + 299000;
        const uint32_t gen = w.requestSilence(deadline);
        uint64_t t = w.now;
        while (!w.silenced(gen)) w.pumpAt(t += 1000);
        CHECK_EQ(w.drainedAt() <= deadline, true);
        std::vector<uint8_t> first12 = head9;
        first12.insert(first12.end(), c.first.begin(), c.first.end());
        std::vector<uint8_t> f7;
        if (c.closeSysex) f7.push_back(0xf7);
        CHECK_EQ(sameAs(drain(w), {first12, f7, silenceBlock(false), c.tail}), true);
        // そのあとのバイト: ステータスのないデータは送らない (捨てたメッセージの続き。受け取る側には $Bx が残っている)
        w.bytes.clear();
        feed(w, {0x22, 0x80, 0x3c, 0x00});
        CHECK_EQ(sameAs(drain(w), {{0x80, 0x3c, 0x00}}), true);
    }
}

// リングがあふれてシステムエクスクルーシブの途中を捨てたら、その $F7 も捨てる (中身の欠けたものを閉じて送らない)
void testOverflowInSysex() {
    Wire w;
    w.setPumper(UartMidi::OTHER);
    boot(w);
    w.midiByte(0xf0);
    for (uint32_t i = 1; i < UartMidi::RING_SIZE; i++) w.midiByte(0x11);
    feed(w, {0x22, 0x33});
    w.pumpAt(w.now + 1000); // 空きを作る
    feed(w, {0x44, 0xf7, 0x55, 0x90, 0x3c, 0x7f});
    CHECK_EQ(w.dropped(), 5); // 22 33 44 f7 55
    std::vector<uint8_t> got = drain(w);
    CHECK_EQ(got.size(), UartMidi::RING_SIZE + 3);
    CHECK_EQ(got[UartMidi::RING_SIZE - 1], 0x11);
    CHECK_EQ(got[UartMidi::RING_SIZE], 0x90);
    // システムエクスクルーシブの外で捨てたときの $F7 (単独) は捨てない
    Wire v;
    v.setPumper(UartMidi::OTHER);
    boot(v);
    for (uint32_t i = 0; i < UartMidi::RING_SIZE / 3; i++) feed(v, {0x90, 0x3c, 0x7f});
    v.midiByte(0x90);
    v.midiByte(0x3c); // 満杯
    v.pumpAt(v.now + 1000);
    feed(v, {0x7f, 0xf7});
    CHECK_EQ(v.dropped(), 2); // 3c 7f
    CHECK_EQ(drain(v).back(), 0xf7);
}

// 長く動かしたあと (リングの位置が 2^31、2^32 を過ぎたあと) に、前の頼みから間をおいて頼んでも、止めるバイトは
// 溜まっていたノートオンのあとに入る (位置の比べ方が一周して壊れない)
void testSilenceAfterLongRun() {
    for (uint64_t pos : {0x80000000ull, 0x100000000ull + 5, 0x8000000000000000ull}) {
        Wire w;
        w.setPumper(UartMidi::OTHER);
        boot(w);
        // 位置 0 で一度頼んでおく (区切りは 3 で残る。0x80000000 のときは、32 ビットで比べると差がちょうど半周になる)
        w.positionAt(0);
        feed(w, {0x80, 0x3c, 0x00});
        uint32_t gen = w.requestSilence();
        while (!w.silenced(gen)) w.pumpAt(w.now + 1000);
        drain(w);
        w.bytes.clear();
        w.positionAt(pos);
        feed(w, {0x90, 0x3c, 0x7f});
        gen = w.requestSilence();
        CHECK_EQ(w.silenced(gen), false);
        w.pumpAt(w.now + 1000);
        CHECK_EQ(w.bytes.size() >= 3 && w.bytes[0] == 0x90, true);
        while (!w.silenced(gen)) w.pumpAt(w.now + 1000);
        CHECK_EQ(sameAs(drain(w), {{0x90, 0x3c, 0x7f}, silenceBlock(false), {0x90}}), true);
        CHECK_EQ(w.idle(), true);
    }
}

// システムエクスクルーシブのデータを捨てたあと、満杯のままステータス ($90) も捨てたら、そのステータスは音源に届いて
// いないので、あとの $F7 も捨てる (音源の側で開いたままの欠けたシステムエクスクルーシブを閉じない)
void testOverflowSysexThenStatus() {
    Wire w;
    w.setPumper(UartMidi::OTHER);
    boot(w);
    w.midiByte(0xf0);
    for (uint32_t i = 1; i < UartMidi::RING_SIZE; i++) w.midiByte(0x11);
    feed(w, {0x22, 0x90}); // データとステータスを捨てる
    w.pumpAt(w.now + 1000); // 空きを作る
    feed(w, {0x3c, 0xf7, 0xf7, 0x90, 0x3c, 0x7f});
    CHECK_EQ(w.dropped(), 5); // 22 90 3c f7 f7
    std::vector<uint8_t> got = drain(w);
    CHECK_EQ(got.size(), UartMidi::RING_SIZE + 3);
    CHECK_EQ(got[UartMidi::RING_SIZE - 1], 0x11);
    CHECK_EQ(got[UartMidi::RING_SIZE], 0x90);
    CHECK_EQ(std::count(got.begin(), got.end(), 0xf7), 0);
}

// 期限つきで頼んだあとに入ったバイトは捨てない (捨てるのは区切りまで)。区切りのあとのステータスのないデータは送らず、
// ステータスから送り始める
void testSilenceDeadlineAfterMark() {
    Wire w;
    w.setPumper(UartMidi::OTHER);
    boot(w);
    w.now = 1000000;
    for (int i = 0; i < 1000; i++) feed(w, {0x91, i & 0x7f, 0x40});
    w.pumpAt(w.now); // 12 バイト送る
    CHECK_EQ(w.bytes.size(), 12u);
    const uint64_t deadline = w.now + 299000;
    const uint32_t gen = w.requestSilence(deadline);
    feed(w, {0x22, 0x40, 0x92, 0x10, 0x40, 0x11, 0x40}); // 頼んだあと (区切りのあと)
    uint64_t t = w.now;
    while (!w.silenced(gen)) w.pumpAt(t += 1000);
    CHECK_EQ(w.drainedAt() <= deadline, true);
    CHECK_EQ(w.flushed(), 3000u - 12u);
    std::vector<uint8_t> first(w.bytes.begin(), w.bytes.begin() + 12);
    CHECK_EQ(sameAs(drain(w), {first, silenceBlock(false), {0x92, 0x10, 0x40, 0x11, 0x40}}), true);
}

} // namespace

int main() {
    testBurstPacing();
    testSteady();
    testOverflow();
    testResetMidMessage();
    testResetInSysex();
    testResetSkipAndFamily();
    testRequestSilence();
    testOrphanData();
    testSelfPump();
    testUnpaced();
    testThreads();
    testSilenceDeadline();
    testSilenceRace();
    testSilenceReadOrder();
    testSilenceMarkOnResume();
    testSilenceDeadlineMidMessage();
    testOverflowInSysex();
    testSilenceAfterLongRun();
    testOverflowSysexThenStatus();
    testSilenceDeadlineAfterMark();
    return finish();
}
