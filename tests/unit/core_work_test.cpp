// src/platform/core_work.h のテスト: 合成するコアが足す時間の統計 (合計、フレームの数、最大) を、読み手が
// 区間ごとに、3 つがそろった形で受け取れること。書き手は i 番目のフレームに i (us) を足すので、フレーム a から b の
// 区間は、合計が a から b の和、フレームの数が b - a + 1、最大が b になる。区間は前の区間の続きから始まる。
// -fsanitize=thread でビルドすると、2 つのスレッドの間のデータの競合も調べられる
#include <thread>

#include "platform/core_work.h"
#include "test_check.h"

namespace {

void testSingleThread() {
    printf("single thread\n");
    platform::CoreWorkMeter m;
    platform::CoreWork w;
    CHECK_EQ(m.take(w), 0); // 頼んだだけ
    m.addFrames(100, 1);    // ここで区切られる
    m.addFrames(300, 1);    // 次の区間
    m.addFrames(900, 3);    // 3 フレーム分をまとめて: 最大は 1 フレームあたりの 300
    CHECK_EQ(m.take(w), 1);
    CHECK_EQ(w.totalUs, 100u);
    CHECK_EQ(w.frames, 1u);
    CHECK_EQ(w.maxUs, 100u);
    CHECK_EQ(w.averageUs(), 100u);
    CHECK_EQ(m.take(w), 0);
    m.addFrames(50, 1);
    CHECK_EQ(m.take(w), 1);
    CHECK_EQ(w.totalUs, 1250u);
    CHECK_EQ(w.frames, 5u);
    CHECK_EQ(w.maxUs, 300u);
    CHECK_EQ(w.averageUs(), 250u);
    // 区切ったあとの区間は空から始まる (前の最大が残らない)
    CHECK_EQ(m.take(w), 0);
    m.addFrames(7, 1);
    CHECK_EQ(m.take(w), 1);
    CHECK_EQ(w.totalUs, 7u);
    CHECK_EQ(w.frames, 1u);
    CHECK_EQ(w.maxUs, 7u);
    // 大きすぎる値は 32 ビットの上限にする
    CHECK_EQ(m.take(w), 0);
    m.addFrames(0x500000000ull, 1);
    CHECK_EQ(m.take(w), 1);
    CHECK_EQ(w.maxUs, 0xffffffffu);
    CHECK_EQ(platform::CoreWork().averageUs(), 0u);
}

void testThreads() {
    printf("two threads\n");
    static platform::CoreWorkMeter m;
    const unsigned N = 2000000;
    unsigned done = 0;
    std::thread writer([&] {
        for (unsigned i = 1; i <= N; i++) m.addFrames(i, 1);
        // 足し終えたあとも、頼まれたら残りを渡す (0us のフレームを足して区切る。読み手は N を超えた分を数えない)
        while (__atomic_load_n(&done, __ATOMIC_ACQUIRE) == 0) m.addFrames(0, 1);
    });
    unsigned long long frames = 0, total = 0;
    unsigned next = 1, intervals = 0; // next: 次の区間の最初のフレーム
    int mismatches = 0;
    while (frames < N) {
        platform::CoreWork w;
        if (!m.take(w)) continue;
        intervals++;
        if (w.frames == 0) continue;
        // 区間は next から始まり、N までは i 番目が i (us)、N より後は 0 (us)
        const unsigned long long a = next, b = next + w.frames - 1;
        const unsigned long long hi = b < N ? b : N;
        const unsigned long long want = a <= hi ? (a + hi) * (hi - a + 1) / 2 : 0;
        if (w.totalUs != want || w.maxUs != (a <= hi ? hi : 0)) mismatches++;
        next += w.frames;
        frames += w.frames;
        total += w.totalUs;
    }
    __atomic_store_n(&done, 1u, __ATOMIC_RELEASE);
    writer.join();
    CHECK_EQ(mismatches, 0);
    CHECK_EQ(total, (unsigned long long)N * (N + 1) / 2);
    printf("  %u intervals\n", intervals);
}

} // namespace

int main() {
    testSingleThread();
    testThreads();
    return finish();
}
