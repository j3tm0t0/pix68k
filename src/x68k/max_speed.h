// 最高速モード: CPU (68000 / 68030 / 68060) の命令の速さ (Machine::setCpuSpeed) を、ホストが実時間に間に合う範囲で一番速くする。
// 周辺機器と画面の時間は実時間のまま (フレームは飛ばさない)。CPU のクロックだけを上げたのと同じになる。
//
// 使い方: 1 フレームごとに、X68000 の時間 (emuUs) と、それを作るのにかかったホストの時間 (busyUs、実時間に合わせて
// 待った時間は含めない) を update() に渡す。0.25 秒分たまるごとに、使った時間が X68000 の時間の TARGET_PERCENT % に
// なるよう速さを変える (1 回に変える幅は 1/2 倍から 3/2 倍まで)。ホスト版とベアメタル版で同じものを使う
#pragma once

#include <stdint.h>

#include "x68k/machine.h"

namespace x68k {

class MaxSpeed {
public:
    static constexpr int TARGET_PERCENT = 85;
    static constexpr uint64_t WINDOW_US = 250000;

    void update(Machine& m, uint64_t emuUs, uint64_t busyUs) {
        emuUs_ += emuUs;
        busyUs_ += busyUs;
        if (emuUs_ < WINDOW_US) return;
        const int cur = m.cpuSpeed();
        // 速さを cur から next に変えると、CPU の分の時間はおよそ cur / next 倍になる。周辺機器の分は変わらないが、
        // 何度か繰り返すうちに TARGET_PERCENT % に近づく
        uint64_t next = busyUs_ ? (uint64_t)cur * emuUs_ * TARGET_PERCENT / (busyUs_ * 100) : (uint64_t)cur * 3 / 2;
        if (next > (uint64_t)cur * 3 / 2) next = (uint64_t)cur * 3 / 2;
        if (next < (uint64_t)cur / 2) next = (uint64_t)cur / 2;
        m.setCpuSpeed((int)(next > 0x7fffffff ? 0x7fffffff : next));
        emuUs_ = busyUs_ = 0;
    }

private:
    uint64_t emuUs_ = 0, busyUs_ = 0;
};

} // namespace x68k
