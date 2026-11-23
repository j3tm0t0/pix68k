// audio_worker_test と render_worker_test の、flush() を何万回も往復する試験の共通部分 (#378)。
// CPU が混んで書き手と読み手が同じ CPU に載ると、往復ごとにスレッドの切り替えが 2 回要り、ほかのプロセスの順番も
// 待つので、待ち続けていなくても時間が何十倍にもなる。そこで、
// - 回数は、MIN 回を超えたら時間 (秒) でも打ち切る。空いていれば rounds 回のすべてを回る
// - 待ち続けているかは、かかった時間の合計ではなく、STALL 秒の間に 1 往復も進まないことで見分ける (遅いだけなら
//   掛け直す)。止まってから知らせるまでは STALL 秒から、その 2 倍まで
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <signal.h>
#include <unistd.h>

namespace worker_stress {
constexpr unsigned MIN = 512, STEP = 256, STALL = 10, BUDGET = 4;

static_assert(std::atomic<unsigned>::is_always_lock_free, "used in a signal handler");
inline std::atomic<unsigned> progress{0}; // 1 往復 (か、ほかの 1 段階) 進むごとに増やす
inline std::atomic<unsigned> seen{0};     // 前に見張りが見た progress (見張りを始めたあとはハンドラだけが触る)

inline void tick() { progress.fetch_add(1, std::memory_order_relaxed); }

inline void check(int) {
    const unsigned now = progress.load(std::memory_order_relaxed);
    if (now != seen.load(std::memory_order_relaxed)) {
        seen.store(now, std::memory_order_relaxed);
        alarm(STALL);
        return;
    }
    static const char msg[] = "NG: no progress; the worker thread is stuck (flush() or teardown)\n";
    if (write(2, msg, sizeof msg - 1) < 0) _exit(2);
    _exit(1);
}

// 作ってから BUDGET 秒が過ぎたか
struct Budget {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    bool spent() const { return std::chrono::steady_clock::now() - start > std::chrono::seconds(BUDGET); }
};

// main の初めに 1 回だけ呼ぶ。STALL 秒の間に tick() が 1 回もなければ、待ち続けているとして失敗にする。
// 掛け直すのはハンドラだけ (ハンドラはどのスレッドで動くか分からないので、途中で掛け直すと競合する)
inline void watch() {
    struct sigaction sa = {};
    sa.sa_handler = check;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, nullptr);
    alarm(STALL);
}
inline void unwatch() { alarm(0); }

// 1 往復ごとに tick() を呼ぶ。
// body(i) を i = 0 から rounds 回呼ぶ。STEP 回ごとに、読み手が眠るまで少し待つ
template <class Body> void run(const char* name, unsigned rounds, Body&& body) {
    Budget budget;
    unsigned i = 0;
    for (; i < rounds; i++) {
        if (i % STEP == 0) {
            if (i >= MIN && budget.spent()) break;
            usleep(2000);
        }
        body(i);
        tick();
    }
    if (i < rounds) std::printf("%s: stopped after %u of %u rounds (%u seconds)\n", name, i, rounds, BUDGET);
}
}
