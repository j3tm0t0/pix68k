// src/platform/sample_clock.h のテスト: 細かく呼んでも実時間より多く取り出さないこと、長く止まったときの上限
#include "platform/sample_clock.h"
#include "test_check.h"

namespace {

void testExactRate() {
    printf("exact rate\n");
    // 1MHz のタイマ、48kHz。21us ごとに呼んでも、10 秒で 480000 サンプルちょうど
    // (前に呼んだ時刻から 1 サンプル 20us として進めると、1 回に 1 サンプルずつ多く取り出していく)
    platform::SampleClock c(1000000, 48000);
    c.start(5);
    uint64_t sum = 0;
    for (uint64_t t = 5; t <= 5 + 10000000; t += 21) sum += c.due(t, 48000);
    sum += c.due(5 + 10000000, 48000);
    CHECK_EQ(sum, 480000u);
    // 1us ごとでも同じ
    c.start(0);
    sum = 0;
    for (uint64_t t = 0; t <= 1000000; t++) sum += c.due(t, 48000);
    CHECK_EQ(sum, 48000u);
}

void testLongStall() {
    printf("long stall\n");
    platform::SampleClock c(1000000, 48000);
    c.start(0);
    // 3 秒止まっていたら、1 秒分だけ取り出し、残りは捨てる
    CHECK_EQ(c.due(3000000, 48000), 48000u);
    CHECK_EQ(c.total(), 144000u);
    // そのあとは実時間どおり
    CHECK_EQ(c.due(3000000 + 1000, 48000), 48u);
}

} // namespace

int main() {
    testExactRate();
    testLongStall();
    return finish();
}
