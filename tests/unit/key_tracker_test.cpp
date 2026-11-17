// USB キーボードのレポートを押した / 離したに直す処理 (src/platform/key_tracker.h) のテスト。
// 特に、送り先が満杯で送れなかったときに、キーが押しっぱなしにならないこと
#include <stdio.h>
#include <string.h>

#include "platform/key_tracker.h"

namespace {

int failures = 0;

#define CHECK_EQ(a, b)                                                                                 \
    do {                                                                                               \
        unsigned long long va_ = (unsigned long long)(a), vb_ = (unsigned long long)(b);              \
        if (va_ != vb_) {                                                                              \
            printf("  %s:%d: %s == %s: got %llx, want %llx\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
            failures++;                                                                                \
        }                                                                                              \
    } while (0)

// 送り先。capacity 個まで受け取り、受け取ったイベントの記録と、押している数を持つ
struct Sink {
    int capacity = 1000;
    int count = 0;
    uint16_t events[64];
    bool down[256] = {};
    bool operator()(uint8_t u, bool d) {
        if (count >= capacity) return false;
        if (count < 64) events[count] = (uint16_t)(u | (d ? 0x100 : 0));
        count++;
        down[u] = d;
        return true;
    }
    int held() const {
        int n = 0;
        for (bool b : down) n += b;
        return n;
    }
};

void testDiff() {
    printf("diff\n");
    platform::KeyTracker t;
    Sink s;
    const uint8_t a[6] = {0x04, 0, 0, 0, 0, 0};    // A
    const uint8_t ab[6] = {0x05, 0x04, 0, 0, 0, 0}; // B と A (並びが変わっても同じキー)
    const uint8_t none[6] = {};
    CHECK_EQ(t.report(0x02, a, s), true); // 左 SHIFT + A
    CHECK_EQ(s.count, 2);
    CHECK_EQ(s.down[0xe1], true);
    CHECK_EQ(s.down[0x04], true);
    s.count = 0;
    CHECK_EQ(t.report(0x02, ab, s), true); // B を押す
    CHECK_EQ(s.count, 1);
    CHECK_EQ(s.events[0], 0x105);
    s.count = 0;
    CHECK_EQ(t.report(0x00, a, s), true); // SHIFT と B を離す。離すほうを先に送る
    CHECK_EQ(s.count, 2);
    CHECK_EQ(s.events[0] & 0x100, 0);
    CHECK_EQ(s.events[1] & 0x100, 0);
    // ErrorRollOver (1) のレポートは無視する (押しているキーを離さない)
    const uint8_t rollover[6] = {1, 1, 1, 1, 1, 1};
    s.count = 0;
    CHECK_EQ(t.report(0x00, rollover, s), true);
    CHECK_EQ(s.count, 0);
    CHECK_EQ(s.down[0x04], true);
    CHECK_EQ(t.report(0x00, none, s), true);
    CHECK_EQ(s.held(), 0);
    CHECK_EQ(t.pending(), false);
}

// 同じレポートの修飾キーと普通のキー: 押すときは修飾キーが先、離すときは修飾キーが後
void testModifierOrder() {
    printf("modifier order\n");
    platform::KeyTracker t;
    Sink s;
    const uint8_t a[6] = {0x04, 0, 0, 0, 0, 0};
    const uint8_t none[6] = {};
    CHECK_EQ(t.report(0x22, a, s), true); // 左 SHIFT + 右 SHIFT + A
    CHECK_EQ(s.count, 3);
    CHECK_EQ(s.events[0], 0x1e1);
    CHECK_EQ(s.events[1], 0x1e5);
    CHECK_EQ(s.events[2], 0x104);
    s.count = 0;
    CHECK_EQ(t.report(0x00, none, s), true);
    CHECK_EQ(s.count, 3);
    CHECK_EQ(s.events[0], 0x04);
    CHECK_EQ(s.events[1], 0xe1);
    CHECK_EQ(s.events[2], 0xe5);
    // 満杯で SHIFT だけ送れたときも、A は SHIFT のあとに届く
    s.count = 0;
    s.capacity = 1;
    CHECK_EQ(t.report(0x02, a, s), false);
    CHECK_EQ(s.events[0], 0x1e1);
    s.capacity = 1000;
    CHECK_EQ(t.resync(s), true);
    CHECK_EQ(s.events[1], 0x104);
}

void testFull() {
    printf("queue full\n");
    platform::KeyTracker t;
    Sink s;
    const uint8_t six[6] = {0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    const uint8_t none[6] = {};
    CHECK_EQ(t.report(0xff, six, s), true); // 修飾キー 8 つと 6 つのキー
    CHECK_EQ(s.held(), 14);
    // 送り先が満杯: 離したことを送れない
    s.capacity = s.count + 3;
    CHECK_EQ(t.report(0x00, none, s), false);
    CHECK_EQ(t.pending(), true);
    CHECK_EQ(s.held(), 11);
    // 同じレポートがもう来なくても、resync() で残りを送れる
    s.capacity = s.count + 5;
    CHECK_EQ(t.resync(s), false);
    CHECK_EQ(s.held(), 6);
    s.capacity = 1000;
    CHECK_EQ(t.resync(s), true);
    CHECK_EQ(s.held(), 0);
    CHECK_EQ(t.pending(), false);

    // 満杯の間に押して離したキーは、送らなくてよい (押しっぱなしにならない)
    s.capacity = s.count;
    const uint8_t a[6] = {0x04, 0, 0, 0, 0, 0};
    CHECK_EQ(t.report(0x00, a, s), false);
    CHECK_EQ(t.report(0x00, none, s), true);
    CHECK_EQ(t.pending(), false);
    CHECK_EQ(s.held(), 0);

    // キーボードが外れたら、すべて離す
    s.capacity = 1000;
    CHECK_EQ(t.report(0x01, a, s), true);
    CHECK_EQ(s.held(), 2);
    CHECK_EQ(t.releaseAll(s), true);
    CHECK_EQ(s.held(), 0);
}

} // namespace

int main() {
    testDiff();
    testModifierOrder();
    testFull();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
