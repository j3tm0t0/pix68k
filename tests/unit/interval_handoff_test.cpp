// src/platform/interval_handoff.h のテスト: 書き手のスレッドが足し続ける統計を、読み手のスレッドが
// 区間ごとに受け取っても、どの区間にも入らないもの (取りこぼし) や、2 つの区間に入るものがないこと。
// -fsanitize=thread でビルドすると、2 つのスレッドの間のデータの競合も調べられる
#include <thread>

#include "platform/interval_handoff.h"
#include "test_check.h"

namespace {

struct Stats {
    unsigned long long count = 0; // 足した回数
    unsigned long long sum = 0;   // 足した値の合計
    unsigned first = 0, last = 0; // 区間の最初と最後の値 (1 から数える)
};

void testSingleThread() {
    printf("single thread\n");
    platform::IntervalHandoff<Stats> h;
    Stats out;
    CHECK_EQ(h.take(out), 0); // 頼んだだけ
    h.current().count = 5;
    CHECK_EQ(h.take(out), 0); // まだ区切られていない (同じ頼みのまま)
    CHECK_EQ(h.endSample(), 1);
    CHECK_EQ(h.endSample(), 0); // 頼まれていなければ区切らない
    h.current().count += 2;
    CHECK_EQ(h.take(out), 1);
    CHECK_EQ(out.count, 5u);
    CHECK_EQ(h.take(out), 0);
    CHECK_EQ(h.endSample(), 1);
    CHECK_EQ(h.take(out), 1);
    CHECK_EQ(out.count, 2u);
}

void testThreads() {
    printf("two threads\n");
    static platform::IntervalHandoff<Stats> h;
    const unsigned N = 2000000;
    unsigned done = 0;
    std::thread writer([&] {
        for (unsigned i = 1; i <= N; i++) {
            Stats& s = h.current();
            if (s.count == 0) s.first = i;
            s.last = i;
            s.count++;
            s.sum += i;
            h.endSample();
        }
        // 足し終えたあとも、頼まれたら残りを渡す
        while (__atomic_load_n(&done, __ATOMIC_ACQUIRE) == 0) h.endSample();
    });
    unsigned long long count = 0, sum = 0;
    unsigned expectFirst = 1, intervals = 0;
    int gaps = 0;
    while (count < N) {
        Stats s;
        if (!h.take(s)) continue;
        intervals++;
        if (s.count == 0) continue;
        if (s.first != expectFirst || s.last != s.first + s.count - 1) gaps++;
        expectFirst = s.last + 1;
        count += s.count;
        sum += s.sum;
    }
    __atomic_store_n(&done, 1u, __ATOMIC_RELEASE);
    writer.join();
    CHECK_EQ(count, N);
    CHECK_EQ(sum, (unsigned long long)N * (N + 1) / 2);
    CHECK_EQ(gaps, 0);
    printf("  %u intervals\n", intervals);
}

} // namespace

int main() {
    testSingleThread();
    testThreads();
    return finish();
}
