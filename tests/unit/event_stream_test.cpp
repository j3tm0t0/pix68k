// src/platform/event_stream.h のテスト: イベントストリームの周期が、20us 以上で最も短いものになること
#include "platform/event_stream.h"
#include "test_check.h"

namespace {

// 選んだ周期 (2^(evnti+1) カウント) が minUs 以上で、1 つ短い周期は minUs 未満であること
void checkMinimal(uint64_t frq, unsigned minUs) {
    const platform::EventStreamPeriod p = platform::chooseEventStream(frq, minUs);
    const uint64_t counts = 2ull << p.evnti;
    if (p.evnti < 15) CHECK_EQ(counts * 1000000 >= frq * minUs, 1);
    if (p.evnti > 0) CHECK_EQ((counts / 2) * 1000000 < frq * minUs, 1);
}

void testBoards() {
    printf("boards\n");
    // Pi 3 (19.2MHz): 512 カウント = 26.67us
    platform::EventStreamPeriod p = platform::chooseEventStream(19200000);
    CHECK_EQ(p.evnti, 8u);
    CHECK_EQ(p.periodUs, 27u);
    // Pi 4 / Pi 5 (54MHz): 2048 カウント = 37.9us
    p = platform::chooseEventStream(54000000);
    CHECK_EQ(p.evnti, 10u);
    CHECK_EQ(p.periodUs, 38u);
    // QEMU の raspi3b (62.5MHz): 2048 カウント = 32.8us
    p = platform::chooseEventStream(62500000);
    CHECK_EQ(p.evnti, 10u);
    CHECK_EQ(p.periodUs, 33u);
}

void testBoundary() {
    printf("boundary\n");
    // 25.6MHz なら 512 カウントがちょうど 20us
    platform::EventStreamPeriod p = platform::chooseEventStream(25600000);
    CHECK_EQ(p.evnti, 8u);
    CHECK_EQ(p.periodUs, 20u);
    // それより少しでも速ければ 512 カウントは 20us を下回るので、1 つ長い周期 (1024 カウント) にする
    // (20us 分のカウントを切り捨てで計算すると 512 になり、20us 未満の周期を選んでしまう)
    p = platform::chooseEventStream(25600049);
    CHECK_EQ(p.evnti, 9u);
    CHECK_EQ(p.periodUs, 40u);
    p = platform::chooseEventStream(25600001);
    CHECK_EQ(p.evnti, 9u);
    static const uint64_t frqs[] = {1000000, 19200000, 25599999, 25600000, 25600001, 25600049, 54000000, 62500000,
                                    1000000000};
    for (uint64_t frq : frqs) checkMinimal(frq, 20);
}

void testLimits() {
    printf("limits\n");
    // 周波数が 0 なら使えない
    platform::EventStreamPeriod p = platform::chooseEventStream(0);
    CHECK_EQ(p.periodUs, 0u);
    // 遅いカウンタ (1kHz) なら 2 カウントでも 20us を超える
    p = platform::chooseEventStream(1000);
    CHECK_EQ(p.evnti, 0u);
    CHECK_EQ(p.periodUs, 2000u);
    // EVNTI = 15 の境界: 2^16 カウントがちょうど 20us (3.2768GHz) なら 15 で満たせる。2^15 カウントがちょうど 20us
    // (1.6384GHz) なら 14、それより少し速ければ 15
    p = platform::chooseEventStream(3276800000ull);
    CHECK_EQ(p.evnti, 15u);
    CHECK_EQ(p.periodUs, 20u);
    checkMinimal(3276800000ull, 20);
    p = platform::chooseEventStream(1638400000ull);
    CHECK_EQ(p.evnti, 14u);
    checkMinimal(1638400000ull, 20);
    p = platform::chooseEventStream(1638400001ull);
    CHECK_EQ(p.evnti, 15u);
    checkMinimal(1638400001ull, 20);
    // 3.2768GHz より少しでも速ければ、15 (最も長い周期) でも 20us に届かない。15 のままにする
    p = platform::chooseEventStream(3276800001ull);
    CHECK_EQ(p.evnti, 15u);
    CHECK_EQ((2ull << 15) * 1000000 < 3276800001ull * 20, 1);
    // とても速いカウンタでも EVNTI は 15 まで
    p = platform::chooseEventStream(100000000000ull);
    CHECK_EQ(p.evnti, 15u);
    CHECK_EQ(p.periodUs, 1u);
}

// 起動時の確認の判定 (見込みの半分、見込みが 0 回なら使わない)
void testProbe() {
    printf("probe\n");
    CHECK_EQ(platform::streamProbeOk(37, 2000, 27), true);  // 見込み 74 回の半分
    CHECK_EQ(platform::streamProbeOk(36, 2000, 27), false);
    CHECK_EQ(platform::streamProbeOk(1, 2000, 27), false);  // コア 0 の 100Hz の sev だけ
    CHECK_EQ(platform::streamProbeOk(1, 2000, 2000), true); // 見込み 1 回
    CHECK_EQ(platform::streamProbeOk(5, 2000, 2001), false); // 周期が 2000us を超える: 見込みが 0 回なので使わない
    CHECK_EQ(platform::streamProbeOk(0, 2000, 0), false);
}

} // namespace

int main() {
    testBoards();
    testBoundary();
    testLimits();
    testProbe();
    return finish();
}
