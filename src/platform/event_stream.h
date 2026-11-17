// ARM の汎用タイマのイベントストリームの周期を決める (ベアメタル版の baremetal/event_wait.h が使う)。
// イベントは、カウンタの bit EVNTI が 0 から 1 になるたびに出るので、周期は 2^(EVNTI+1) カウント。
// カウンタの周波数 (CNTFRQ) から、minUs 以上で最も短い周期を選ぶ。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)
#pragma once

#include <stdint.h>

namespace platform {

struct EventStreamPeriod {
    unsigned evnti = 0;    // CNTKCTL_EL1 の EVNTI (0〜15)
    unsigned periodUs = 0; // 周期 (us、切り上げ)。0 なら使えない (周波数が 0)
};

// frq: カウンタの周波数 (Hz)。2^(evnti+1) カウントが minUs 以上になる最小の evnti を選ぶ。
// EVNTI は 4 ビットなので 15 が上限 (それでも足りなければ 15 にする)
inline EventStreamPeriod chooseEventStream(uint64_t frq, unsigned minUs = 20) {
    EventStreamPeriod p;
    if (frq == 0) return p;
    // minUs 分のカウント (切り上げ。端数を切り捨てると、周期が minUs をわずかに下回ることがある)
    const uint64_t want = (frq * minUs + 999999) / 1000000;
    unsigned n = 0;
    while (n < 15 && (2ull << n) < want) n++;
    p.evnti = n;
    p.periodUs = (unsigned)(((2ull << n) * 1000000 + frq - 1) / frq);
    return p;
}

// 起動時の確認: windowUs の間 wfe で眠って wakes 回戻ったとき、周期 periodUs のイベントストリームが届いているとみなすか。
// 周期から見込んだ回数 (windowUs / periodUs) の半分に届けばよい。周期が windowUs より長くて見込みが 0 回のときは、
// この確認では見分けられないので使わない
inline bool streamProbeOk(unsigned wakes, unsigned windowUs, unsigned periodUs) {
    if (periodUs == 0) return false;
    const unsigned expected = windowUs / periodUs;
    return expected != 0 && wakes * 2 >= expected;
}

} // namespace platform
