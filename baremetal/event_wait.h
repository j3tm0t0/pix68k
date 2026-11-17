// 待つ間にコアを wfe で眠らせる (空回りで熱くならないように)。
// ARM の汎用タイマのイベントストリームを有効にすると、決まった周期でそのコアにイベントが届き、wfe が戻る。
// 「時刻を見て、まだなら wfe」を繰り返せば、周期 1 つ分の遅れで起きられる。
// ほかに wfe が戻るのは、どれかのコアの sev (コア 1 がコア 2 / 3 を起こすために送る)、割り込み (マスクしていなければ)。
// 余計に起きても時刻を見て眠り直すだけなので、困ることはない。
// イベントストリームは CNTKCTL_EL1 の設定で、コアごとに有効にする (ほかのコアには届かない)。
#pragma once

#include <circle/timer.h>
#include <circle/types.h>

#include "platform/event_stream.h"

namespace event_wait {

// 呼んだコアでイベントストリームを有効にし、周期 (us) を返す (0 なら有効にしていない)。
// 周期は CNTFRQ_EL0 から、20us 以上で最も短いものを選ぶ (platform/event_stream.h。
// Pi 3 の 19.2MHz で約 27us、Pi 4 / 5 の 54MHz で約 38us、QEMU の 62.5MHz で約 33us)
inline unsigned enableStream() {
    u64 frq;
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(frq));
    const platform::EventStreamPeriod p = platform::chooseEventStream(frq);
    if (p.periodUs == 0) return 0;
    u64 k;
    asm volatile("mrs %0, CNTKCTL_EL1" : "=r"(k));
    k &= ~(u64)0xFC;                   // EVNTEN (bit 2)、EVNTDIR (bit 3)、EVNTI (bit 4-7) を消す
    k |= (u64)p.evnti << 4 | 1u << 2; // EVNTI、EVNTEN = 1 (カウンタの bit EVNTI が 0 から 1 になるたびにイベント)
    asm volatile("msr CNTKCTL_EL1, %0; isb" ::"r"(k) : "memory");
    return p.periodUs;
}

// 呼んだコアでイベントストリームを止める (確かめて使わないと決めたとき)
inline void disableStream() {
    u64 k;
    asm volatile("mrs %0, CNTKCTL_EL1" : "=r"(k));
    k &= ~(u64)(1u << 2); // EVNTEN = 0
    asm volatile("msr CNTKCTL_EL1, %0; isb" ::"r"(k) : "memory");
}

// 時刻 (CTimer::GetClockTicks64()) が deadline になるまで wfe で眠る。
// イベントストリームが有効なら、deadline を過ぎてから周期 1 つ分以内に戻る
inline void sleepUntil(u64 deadline) {
    while (CTimer::GetClockTicks64() < deadline) asm volatile("wfe" ::: "memory");
}

// イベントストリームが届くかを確かめる。us の間 wfe で眠り、戻った回数を返す。
// wfe から戻って初めて期限を見るので、イベントストリームが届かなくても、ほかに wfe が戻る元がないと終わらない。
// コア 0 では Circle の 100Hz の定期割り込み (タイマ) がその元になるので、長くて 10ms ほどで戻る
// (ほかのコアを起動する前に呼べば sev もなく、戻るのはほぼイベントストリームと割り込みだけ)。
// コア 1 には定期割り込みが来ないが、イベントストリームを使うと決めたらコア 0 がタイマの割り込みから 100Hz で
// sev を送る (CKernel::StartEventStream())。それが元になるので、コア 1 でも長くて us + 10ms ほどで終わる
inline unsigned countWakes(unsigned us) {
    const u64 end = CTimer::GetClockTicks64() + us;
    unsigned n = 0;
    while (CTimer::GetClockTicks64() < end) {
        asm volatile("wfe" ::: "memory");
        n++;
    }
    return n;
}

// イベントストリームそのものが届くかを確かめる (countWakes() のあとに、コア 1 で呼ぶ)。
// 周期の 4 倍先の時刻まで眠り、その時刻から「周期 + 10us」以内に起きられたかを WAKE_TRIALS 回見て、その回数を返す。
// countWakes() は、ほかのコアの sev でも戻った回数に数えるので、sev が多いとイベントストリームが届かなくても通ることがある。
// こちらは起きた時刻を見る。イベントストリームが届かなければ、起きるのはほかのコアの次の sev なので、間に合うのは
// その sev がたまたま期限の直後に来たときだけになる。コア 1 が動き始めるころのコア 0 は、USB の装置を探す待ちの間に
// 0.2ms ごとに sev を送る (CKernel::UsbScanDelay())。周期が 27us (Pi 3) なら 1 回あたり 2 割ほど間に合うので、
// WAKE_TRIALS_NEEDED 回以上で届いているとみなす (届かないのに通るのは 1 万回に 1 回より少ない)。
// ほかのコアが空回りしながら sev を送り続けている間 (数 us ごと) は、これでも見分けられない
constexpr unsigned WAKE_TRIALS = 16, WAKE_TRIALS_NEEDED = 14;
inline unsigned wakesOnTime(unsigned periodUs) {
    unsigned onTime = 0;
    for (unsigned i = 0; i < WAKE_TRIALS; i++) {
        const u64 target = CTimer::GetClockTicks64() + 4 * (u64)periodUs;
        sleepUntil(target);
        if (CTimer::GetClockTicks64() - target <= (u64)periodUs + 10) onTime++;
    }
    return onTime;
}

} // namespace event_wait
