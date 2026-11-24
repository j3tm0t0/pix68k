// platform/key_log.h (キーのログを出すかどうか) の単体テスト
#include <stdio.h>

#include "platform/key_log.h"

static int g_fail = 0;
#define CHECK(c)                                                  \
    do {                                                          \
        if (!(c)) {                                               \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); \
            g_fail++;                                             \
        }                                                         \
    } while (0)

using platform::KeyLogFilter;

int main() {
    const uint8_t none[6] = {};
    const uint8_t a[6] = {4};
    const uint8_t ab[6] = {4, 5};
    const uint8_t ba[6] = {5, 4};
    const uint8_t b[6] = {5};
    const uint8_t over[6] = {1, 1, 1, 1, 1, 1}; // 押しすぎ

    KeyLogFilter f;
    for (int i = 0; i < 3; i++) CHECK(!f.changed(0, none)); // 何も押していないレポートの繰り返しは、最初から出さない
    CHECK(f.changed(0, a)); // 押す
    CHECK(!f.changed(0, a)); // 押したままの繰り返し
    CHECK(f.changed(0, ab));
    CHECK(!f.changed(0, ba)); // 並び順だけが違う
    CHECK(f.changed(0, b));   // 1 つ離す
    CHECK(f.changed(0, a));   // 数は同じでキーが違う
    CHECK(f.changed(0, none));
    CHECK(!f.changed(0, none));
    CHECK(f.changed(0x02, none)); // 修飾キーだけ
    CHECK(!f.changed(0x02, none));
    CHECK(f.changed(0x22, none));
    CHECK(f.changed(0, none));
    CHECK(f.changed(0, over));
    CHECK(!f.changed(0, over));
    CHECK(f.changed(0, none));

    // 6 つ目のキーだけの違いも見る
    const uint8_t six[6] = {4, 5, 6, 7, 8, 9}, six2[6] = {4, 5, 6, 7, 8, 10};
    CHECK(f.changed(0, six));
    CHECK(f.changed(0, six2));

    // 外すと、何も押していないところから始まる
    f.reset();
    CHECK(!f.changed(0, none));
    CHECK(f.changed(0x01, a));
    f.reset();
    CHECK(f.changed(0x01, a));

    // キーボードごとに別
    KeyLogFilter g;
    CHECK(!f.changed(0x01, a));
    CHECK(g.changed(0x01, a));

    if (g_fail == 0) printf("key_log_test: OK\n");
    return g_fail != 0;
}
