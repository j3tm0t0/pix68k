// platform/dma_refill.h: 音の DMA の割り込みで、バッファを埋めるかを決める (#296)。決めるだけの部分 (dmaRefillStep()) と、
// レジスタの読み書きを含めた割り込みの処理 (dmaRefillOnInterrupt()) を、DMA のまねで試す
#include <stdint.h>
#include <stdio.h>

#include "platform/dma_refill.h"

static int failures = 0;
#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        const long long va = (long long)(a), vb = (long long)(b);                                   \
        if (va != vb) {                                                                             \
            printf("  %s:%d: %s: got %lld, want %lld\n", __FILE__, __LINE__, #a " == " #b, va, vb); \
            failures++;                                                                             \
        }                                                                                           \
    } while (0)

using namespace platform;

namespace {
const unsigned FILL = DMA_REFILL_FILL, RESYNC = DMA_REFILL_RESYNC;
const uint32_t A = 0x1000, B = 0x2000; // 2 つの制御ブロックのレジスタでの値

// 割り込みの回数から決めた番号も一緒に進める (呼ぶ側の m_nCurrentBuffer と同じ動き): 埋めたら、レジスタから決めた番号に
// 直して 1 つ進める。loaded が A なら送り終えたのは B (1)、B なら A (0)
struct Sim {
    DmaRefillState st;
    unsigned current = 0; // 次に送り終えるはずのバッファ
    unsigned fills[2] = {};
    unsigned resyncs = 0;
    unsigned step(uint32_t loaded) {
        const unsigned done = loaded == A ? 1 : 0;
        const unsigned r = dmaRefillStep(st, true, loaded, done == current);
        if (r & RESYNC) resyncs++;
        if (r & FILL) {
            fills[done]++;
            current = done ^ 1;
        }
        return r;
    }
};

// ここから、割り込みの処理 (dmaRefillOnInterrupt()) を、まねた DMA で動かす。レジスタを 1 回読み書きするたびに時刻が 1 進み、
// その時刻に決めておいた出来事 (DMA が 1 つ送り終える、エラー) が起きる
enum Event { NONE, COMPLETE, COMPLETE_BLOCK_FIRST, COMPLETE_INT_FIRST, ERROR_EVENT };
const uint32_t INT = 1u << 2, ERR = 1u << 8;

struct FakeDma {
    unsigned n = 2;
    uint32_t blocks[4] = {0x1000, 0x2000, 0x3000, 0x4000};
    unsigned active = 0;      // DMA が送っているバッファ
    bool intPending = false;  // CS の INT
    bool error = false;       // CS の ERROR
    unsigned intLater = 0;    // COMPLETE_BLOCK_FIRST: この回数あとの時刻に INT を立てる
    unsigned blockLater = 0;  // COMPLETE_INT_FIRST: この回数あとの時刻に CONBLK_AD を変える
    unsigned intLead = 1;     // COMPLETE_INT_FIRST で、INT から CONBLK_AD が変わるまでの時刻の差
    bool fresh[4] = {true, true, true, true}; // 埋めてから、まだ送り終えていない
    unsigned now = 0;
    Event events[128] = {};   // 時刻ごとの出来事
    // 数えるもの
    unsigned stale = 0;       // 埋め直していないバッファを送った (音が飛ぶ)
    unsigned doubleFill = 0;  // 送り終えていないバッファをまた埋めた (同じ送り終わりを 2 度数えた。1 回分の音を捨てる)
    unsigned fillActive = 0;  // 送っている最中のバッファを埋めた
    unsigned fills = 0;
    unsigned fired = 0;       // 起きた出来事の数

    void advance() {
        if (!fresh[active]) stale++;
        fresh[active] = false;
        active = (active + 1) % n;
    }
    void complete(bool blockFirst) {
        if (!fresh[active]) stale++;
        fresh[active] = false;
        active = (active + 1) % n;
        if (blockFirst) intLater = 2;
        else intPending = true;
    }
    void tick() {
        if (intLater && --intLater == 0) intPending = true;
        if (blockLater && --blockLater == 0) advance();
        const Event e = now < 128 ? events[now] : NONE;
        now++;
        if (e != NONE) fired++;
        if (e == COMPLETE) complete(false);
        if (e == COMPLETE_BLOCK_FIRST) complete(true);
        if (e == COMPLETE_INT_FIRST) {
            intPending = true;
            blockLater = intLead;
        }
        if (e == ERROR_EVENT) {
            error = true;
            intPending = true;
        }
    }
    uint32_t readBlock() {
        tick();
        return blocks[active];
    }
    uint32_t readCs() {
        tick();
        return (intPending ? INT : 0) | (error ? ERR : 0);
    }
    void writeCs(uint32_t v) {
        tick();
        if (v & INT) intPending = false;
    }
    // 処理の外で時間がたつ: 遅れていた INT も立つ。決めておいた出来事は消す
    void idle() {
        if (intLater) intPending = true;
        intLater = 0;
        if (blockLater) advance();
        blockLater = 0;
        for (Event& e : events) e = NONE;
    }
    void fill(unsigned b) {
        fills++;
        if (fresh[b]) doubleFill++;
        if (b == active) fillActive++;
        fresh[b] = true;
    }
};

// Circle の割り込みの処理 (書き換えたあと) のまね: INT を消し、エラーでなければ dmaRefillOnInterrupt() に聞いて埋め、回数の番号を進める
struct Handler {
    DmaRefillState st;
    unsigned current = 0; // 割り込みの回数から決めた、次に送り終えるバッファ
    unsigned resyncs = 0, errors = 0, runs = 0;
    void irq(FakeDma& d) {
        runs++;
        const uint32_t cs = d.readCs();
        d.writeCs(cs);
        if (cs & ERR) {
            errors++;
            return;
        }
        unsigned done = 99;
        uint32_t loaded = 0;
        const unsigned r = dmaRefillOnInterrupt(st, d, INT, ERR, d.blocks, d.n, current, &done, &loaded);
        if (r & DMA_REFILL_ERROR) {
            errors++;
            return;
        }
        if (r & DMA_REFILL_RESYNC) resyncs++;
        if (r & DMA_REFILL_FILL) {
            d.fill(done);
            current = (done + 1) % d.n;
        }
    }
    // INT が立っている間 (割り込みが入り続ける間) 呼ぶ。エラーのあとは止める (知らせを受けた側が止める)
    void serve(FakeDma& d) {
        for (int guard = 0; guard < 100; guard++) {
            if (d.intLater || d.blockLater) d.idle();
            if (!d.intPending || d.error) break;
            irq(d);
        }
    }
};

// 0 から 0xffffffff の簡単な乱数
struct Rand {
    uint32_t x = 12345;
    uint32_t next() {
        x = x * 1103515245u + 12345u;
        return x >> 8;
    }
};
} // namespace

int main() {
    printf("dma refill: decide whether to refill a sound buffer on each DMA interrupt\n");
    {
        // ふつう: B、A、B、A と変わる。毎回埋め、合わせ直しはない (最初から)
        Sim s;
        for (int i = 0; i < 1000; i++) CHECK_EQ(s.step(i % 2 ? A : B), FILL);
        CHECK_EQ(s.resyncs, 0);
        CHECK_EQ(s.fills[0], 500);
        CHECK_EQ(s.fills[1], 500);
    }
    {
        // 割り込みを 1 回取りこぼす: A を見たあと、また A (一回りした)。埋めて、合わせ直しに数える。続きは元どおり。
        // 時間は見ない (呼ぶ側が、同じ送り終わりを 2 度知らせない) ので、どれだけ遅れて呼ばれても同じ
        Sim s;
        s.step(B);
        s.step(A);
        CHECK_EQ(s.step(A), FILL | RESYNC);
        CHECK_EQ(s.step(B), FILL);
        CHECK_EQ(s.step(A), FILL);
        CHECK_EQ(s.resyncs, 1);
        CHECK_EQ(s.fills[1], 3);
        // 取りこぼしが続いても、毎回埋めて数える (間引かない)
        Sim t;
        t.step(B);
        for (int i = 0; i < 20; i++) CHECK_EQ(t.step(B), FILL | RESYNC);
        CHECK_EQ(t.step(A), FILL);
        CHECK_EQ(t.resyncs, 20);
        CHECK_EQ(t.fills[0], 21);
        // 始めから毎回 1 つずつ取りこぼして、同じ値ばかり見る (測って覚える期間はない)
        Sim u;
        CHECK_EQ(u.step(A) & FILL, FILL);
        for (int i = 0; i < 20; i++) CHECK_EQ(u.step(A), FILL | RESYNC);
    }
    {
        // 割り込みを 2 回続けて取りこぼす (3 つ送り終えて、違う値): 埋める。回数の偶奇は合っているので、数えない
        Sim s;
        s.step(B);
        s.step(A);
        CHECK_EQ(s.step(B), FILL);
        CHECK_EQ(s.resyncs, 0);
        // 回数が 1 つずれた状態 (前の作りで、ずれたまま続いていた状態) から: 最初の割り込みで合わせ直し、そのあとは合う
        Sim t;
        t.current = 1;
        CHECK_EQ(t.step(B), FILL | RESYNC);
        CHECK_EQ(t.step(A), FILL);
        CHECK_EQ(t.step(B), FILL);
        CHECK_EQ(t.resyncs, 1);
        // PWM / I2S: 動かす前に両方のバッファを埋める (回数の番号は 0 のまま)。最初の割り込みは B を送っている最中で、0 を埋める
        Sim u;
        CHECK_EQ(u.step(B), FILL);
        CHECK_EQ(u.step(A), FILL);
        CHECK_EQ(u.resyncs, 0);
    }
    {
        // どの制御ブロックでもない値: 埋めない。数える。覚えている値は変えない
        DmaRefillState st;
        CHECK_EQ(dmaRefillStep(st, true, B, true), FILL);
        CHECK_EQ(dmaRefillStep(st, false, 0x3000, true), RESYNC);
        CHECK_EQ(st.lastLoaded, B);
        CHECK_EQ(dmaRefillStep(st, true, A, true), FILL);
        // まだ 1 度も呼んでいないときは、値が 0 でも「前と同じ」にしない
        DmaRefillState fresh;
        CHECK_EQ(dmaRefillStep(fresh, true, 0, true), FILL);
        CHECK_EQ(dmaRefillStep(fresh, true, 0, true), FILL | RESYNC);
    }
    {
        // 割り込みが k 回分遅れて入り (k 個送り終えたあと)、処理の途中のどこか (レジスタの j 回目の読み書きの前) で DMA が
        // もう 1 つ送り終える。どの組み合わせでも、送っている最中のバッファは埋めず、同じバッファを 2 回埋めず、INT は残らない
        for (unsigned n = 2; n <= 4; n++)
            for (unsigned k = 1; k <= 3; k++)
                for (unsigned j = 0; j < 12; j++) {
                    FakeDma d;
                    d.n = n;
                    Handler h;
                    for (unsigned i = 0; i < k; i++) d.complete(false);
                    d.events[j] = COMPLETE;
                    h.serve(d);
                    CHECK_EQ(d.doubleFill, 0);
                    CHECK_EQ(d.fillActive, 0);
                    CHECK_EQ(d.intPending, false);
                    // そのあとは、ふつうに 1 つずつ送り終えて、毎回 1 つ埋める。遅れた間に埋められなかったバッファが一回り
                    // 送られたあとは、飛びも合わせ直しもない
                    d.idle();
                    for (unsigned i = 0; i < n; i++) {
                        d.complete(false);
                        h.serve(d);
                    }
                    const unsigned stale = d.stale, resyncs = h.resyncs;
                    for (int i = 0; i < 10; i++) {
                        d.complete(false);
                        h.serve(d);
                    }
                    CHECK_EQ(d.stale, stale);
                    CHECK_EQ(h.resyncs, resyncs);
                    CHECK_EQ(d.doubleFill, 0);
                    CHECK_EQ(d.fillActive, 0);
                }
    }
    {
        // ふつう (取りこぼしなし): 合わせ直しも飛びもない。回数と DMA の進みが合う
        FakeDma d;
        Handler h;
        for (int i = 0; i < 100; i++) {
            d.complete(false);
            h.serve(d);
        }
        CHECK_EQ(h.resyncs, 0);
        CHECK_EQ(d.stale, 0);
        CHECK_EQ(d.fills, 100);
        CHECK_EQ(h.runs, 100);
        // 1 回取りこぼす (割り込みが入る前に 2 つ送り終える): 合わせ直しに 1 回数え、送っている最中のバッファは埋めない
        d.complete(false);
        d.complete(false);
        h.serve(d);
        CHECK_EQ(h.resyncs, 1);
        CHECK_EQ(d.fillActive, 0);
        CHECK_EQ(d.doubleFill, 0);
        for (int i = 0; i < 10; i++) {
            d.complete(false);
            h.serve(d);
        }
        CHECK_EQ(h.resyncs, 1);
    }
    {
        // 処理の途中のどこかで DMA がエラーを知らせる: 1 回だけ知らせ、INT は残さない (残すと、止めたあとにまた割り込みが入る)。
        // バッファは埋めない
        for (unsigned j = 0; j < 4; j++) { // 処理が読み書きするのは 4 回 (CS を読む、消す、CONBLK_AD を読む、CS を読む)
            FakeDma d;
            Handler h;
            d.complete(false);
            d.events[j] = ERROR_EVENT;
            h.irq(d);
            CHECK_EQ(h.errors, 1);
            CHECK_EQ(d.intPending, false);
            CHECK_EQ(d.fillActive, 0);
            CHECK_EQ(d.doubleFill, 0);
        }
        // 読み直しの中でエラーと送り終わりが重なる (INT を消したあと、ERROR と INT が一緒に立つ)
        FakeDma d;
        Handler h;
        d.complete(false);
        d.events[3] = COMPLETE; // 1 回目の CS を読む前: 消して読み直す
        d.events[5] = ERROR_EVENT;
        h.irq(d);
        CHECK_EQ(h.errors, 1);
        CHECK_EQ(d.intPending, false);
        CHECK_EQ(d.fills, 0);
    }
    {
        // 前提が崩れたとき (DMA が CONBLK_AD を先に変え、INT を少しあとで立てる): 読み直しのすぐあとに INT が立つと、次の割り込みで
        // 同じ送り終わりをもう一度数えて、同じバッファを 2 回埋める。この作りは、INT が CONBLK_AD と同時か先に変わることに頼っている
        unsigned worst = 0;
        for (unsigned j = 0; j < 12; j++) {
            FakeDma d;
            Handler h;
            d.complete(false);
            d.events[j] = COMPLETE_BLOCK_FIRST;
            h.serve(d);
            if (d.doubleFill > worst) worst = d.doubleFill;
        }
        CHECK_EQ(worst > 0, true);
    }
    {
        // 長く動かす: DMA は決まった間隔で送り終え、割り込みはときどき長く止められる (0 から 3 バッファ分)。送り終わりは処理の
        // 途中にも入る。どこでも、送っている最中のバッファは埋めず、同じバッファを 2 回埋めない
        for (unsigned n = 2; n <= 4; n += 2) {
            FakeDma d;
            d.n = n;
            Handler h;
            Rand rnd;
            unsigned masked = 0;
            for (int t = 0; t < 200000; t++) {
                if (rnd.next() % 64 == 0) d.complete(false);
                if (masked) {
                    masked--;
                    continue;
                }
                if (rnd.next() % 2000 == 0) masked = rnd.next() % (64 * 3);
                if (d.intPending) {
                    d.now = 0;
                    d.idle();
                    if (rnd.next() % 8 == 0) d.events[rnd.next() % 10] = COMPLETE;
                    h.serve(d);
                }
            }
            CHECK_EQ(d.doubleFill, 0);
            CHECK_EQ(d.fillActive, 0);
            CHECK_EQ(h.resyncs > 0, true); // 長く止めたときは合わせ直している
        }
    }
    {
        // DMA が INT を CONBLK_AD より少し先に立てる (実機でも見えた。Pi 3 で 0.3us 未満): INT から L 回分の時刻のあとで CONBLK_AD が
        // 変わる。その送り終わりが処理のどの時点に重なっても、送っている最中のバッファを埋めず、同じバッファを 2 回埋めない。
        // INT が消えたのを見たあとも CONBLK_AD を読み続ける (DMA_REFILL_SETTLE_READS) ので、L がその回数を超えても (ここでは 16 まで試す)、埋めるのは
        // CONBLK_AD が変わる前 (送り終える直前のバッファの次) になる
        unsigned cases = 0; // 出来事が処理の途中で起きた組み合わせ (lead ごとに、処理が読み書きする 12 回分)
        for (unsigned lead = 1; lead <= 16; lead++)
            for (unsigned j = 0; j < 40; j++) {
                FakeDma d;
                d.intLead = lead;
                Handler h;
                d.complete(false);
                d.events[j] = COMPLETE_INT_FIRST;
                h.serve(d);
                if (!d.fired) continue; // 処理がそこまで読み書きしなかった
                cases++;
                d.idle();
                for (int i = 0; i < 4; i++) {
                    d.complete(false);
                    h.serve(d);
                }
                CHECK_EQ(d.fillActive, 0);
                CHECK_EQ(d.doubleFill, 0);
                CHECK_EQ(d.intPending, false);
            }
        CHECK_EQ(cases, 16 * 12);
    }
    if (failures) {
        printf("FAILED: %d\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
