// ホスト上での CPU コアの速度を測る。X68000 の何 MHz 相当で動くかを表示する。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>

#include "bench/bench_program.h"

static bench::Memory mem;

int main(int argc, char** argv) {
    memset(mem.m, 0, sizeof mem.m);
    bench::load(mem);
    m68k::Cpu cpu(&mem);
    cpu.reset();
    // サイクル数 (既定 2G)。Pi では熱で速度が落ちる前に終わるよう少なくするとよい
    const int64_t total = argc > 1 ? atoll(argv[1]) : 2000000000LL;
    auto t0 = std::chrono::steady_clock::now();
    int64_t done = cpu.run(total);
    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    printf("%lld cycles in %.3f s = %.1f MHz (x%.1f of 16MHz)\n", (long long)done, sec, done / sec / 1e6,
           done / sec / 16e6);
    return 0;
}
