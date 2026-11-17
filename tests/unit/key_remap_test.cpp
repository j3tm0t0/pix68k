// src/platform/key_remap.h のテスト: キーボードの種類の見分け方、Pi 400 の切り替えのキー (ラズベリーのキー) で
// 置き換えるキー、Num Lock と LED、Pi 400 のキーだけで X68000 のキーがすべて打てること (テンキーを除く)
#include "platform/key_remap.h"
#include "platform/key_tracker.h"
#include "test_check.h"

#include <string.h>
#include "x68k/keyboard.h"

namespace {

using platform::KeyLayout;
using platform::KeyRemap;

void testDetect() {
    printf("detect\n");
    CHECK_EQ((int)platform::detectKeyLayout(0x04d9, 0x0007, 0, true), (int)KeyLayout::PI400_US);
    CHECK_EQ((int)platform::detectKeyLayout(0x04d9, 0x0007, 33, true), (int)KeyLayout::PI400_US);
    CHECK_EQ((int)platform::detectKeyLayout(0x04d9, 0x0007, 15, true), (int)KeyLayout::PI400_JP);
    // Pi 400 でなければ、同じ VID:PID でも普通の USB キーボード (04d9 は Holtek の VID でほかのキーボードにもある)
    CHECK_EQ((int)platform::detectKeyLayout(0x04d9, 0x0007, 0, false), (int)KeyLayout::STANDARD);
    CHECK_EQ((int)platform::detectKeyLayout(0x04d9, 0x0006, 15, true), (int)KeyLayout::STANDARD); // 単体の Pi のキーボード
    CHECK_EQ((int)platform::detectKeyLayout(0x046d, 0xc31c, 0, true), (int)KeyLayout::STANDARD);

    KeyLayout l = KeyLayout::STANDARD;
    bool detect = false;
    CHECK_EQ(platform::parseKeyLayout("auto", l, detect), 1);
    CHECK_EQ(detect, 1);
    CHECK_EQ(platform::parseKeyLayout("pi400jp", l, detect), 1);
    CHECK_EQ(detect, 0);
    CHECK_EQ((int)l, (int)KeyLayout::PI400_JP);
    CHECK_EQ(platform::parseKeyLayout("pi400", l, detect), 1);
    CHECK_EQ((int)l, (int)KeyLayout::PI400_US);
    CHECK_EQ(platform::parseKeyLayout("usb", l, detect), 1);
    CHECK_EQ((int)l, (int)KeyLayout::STANDARD);
    CHECK_EQ(platform::parseKeyLayout("jis", l, detect), 0);
}

void testStandard() {
    printf("standard\n");
    KeyRemap r;
    const uint8_t in[6] = {0x0e, 0x53, 0x35, 0, 0, 0};
    uint8_t out[6], mods = 0;
    r.apply(0x08, in, mods, out);
    CHECK_EQ(mods, 0x08); // 左 GUI は OPT.1 のまま
    for (int i = 0; i < 6; i++) CHECK_EQ(out[i], in[i]);
    CHECK_EQ(r.hidLeds(0x07), 0x07);
}

void testPi400Layer() {
    printf("pi400 layer\n");
    KeyRemap r;
    r.setLayout(KeyLayout::PI400_US);
    uint8_t out[6], mods = 0;
    // ラズベリーのキーだけでは何も送らない
    const uint8_t none[6] = {0, 0, 0, 0, 0, 0};
    r.apply(0x08, none, mods, out);
    CHECK_EQ(mods, 0);
    for (int i = 0; i < 6; i++) CHECK_EQ(out[i], 0);
    // ラズベリー + K → F16 (かな)、SHIFT はそのまま
    const uint8_t k[6] = {0x0e, 0, 0, 0, 0, 0};
    r.apply(0x08 | 0x02, k, mods, out);
    CHECK_EQ(mods, 0x02);
    CHECK_EQ(out[0], 0x6b);
    CHECK_EQ(x68k::Keyboard::fromHid(out[0]), 0x5a);
    // ラズベリーを先に離しても、押したままの K は かな のまま
    r.apply(0, k, mods, out);
    CHECK_EQ(out[0], 0x6b);
    r.apply(0, none, mods, out);
    CHECK_EQ(out[0], 0);
    // ラズベリーを離して押し直した K は K
    r.apply(0, k, mods, out);
    CHECK_EQ(out[0], 0x0e);
    r.apply(0, none, mods, out);
    // ラズベリー + O / P → OPT.1 / OPT.2 (修飾キーのビットになる)
    const uint8_t op[6] = {0x12, 0x13, 0, 0, 0, 0};
    r.apply(0x08, op, mods, out);
    CHECK_EQ(mods, 0x88);
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[1], 0);
    r.apply(0, none, mods, out);
    // ラズベリー + O + C → OPT.1 + C (OPT.1 を押している間は置き換えない)
    const uint8_t oc[6] = {0x12, 0x06, 0, 0, 0, 0};
    r.apply(0x08, oc, mods, out);
    CHECK_EQ(mods, 0x08);
    CHECK_EQ(out[0], 0x06); // 出力は前に詰める
    CHECK_EQ(out[1], 0);
    r.apply(0, none, mods, out);
    // ErrorRollOver はそのまま
    const uint8_t roll[6] = {1, 1, 1, 1, 1, 1};
    r.apply(0x08, roll, mods, out);
    CHECK_EQ(out[0], 1);
    r.apply(0, none, mods, out);
    // 英語版の ` は ¥ のまま
    const uint8_t grave[6] = {0x35, 0, 0, 0, 0, 0};
    r.apply(0, grave, mods, out);
    CHECK_EQ(out[0], 0x35);
    // 日本語版の 半角/全角 は 全角 (ラズベリーや OPT.1 を押していても)
    r.setLayout(KeyLayout::PI400_JP);
    r.apply(0, grave, mods, out);
    CHECK_EQ(out[0], 0x6e);
    CHECK_EQ(x68k::Keyboard::fromHid(out[0]), 0x60);
    r.apply(0, none, mods, out);
    r.apply(0x08, grave, mods, out);
    CHECK_EQ(out[0], 0x6e);
    r.apply(0, none, mods, out);
    const uint8_t og[6] = {0x12, 0x35, 0, 0, 0, 0};
    r.apply(0x08, og, mods, out);
    CHECK_EQ(mods, 0x08);
    CHECK_EQ(out[0], 0x6e);
}

void testNumLock() {
    printf("num lock\n");
    KeyRemap r;
    r.setLayout(KeyLayout::PI400_US);
    uint8_t out[6], mods = 0;
    const uint8_t nl[6] = {0x53, 0, 0, 0, 0, 0};
    const uint8_t none[6] = {0, 0, 0, 0, 0, 0};
    // X68000 の INS (Num Lock の LED) は出さない
    CHECK_EQ(r.hidLeds(0x01 | 0x02), 0x02);
    r.apply(0, nl, mods, out);
    CHECK_EQ(out[0], 0); // X68000 には送らない
    CHECK_EQ(r.numLock(), 1);
    CHECK_EQ(r.hidLeds(0x02), 0x03);
    r.apply(0, nl, mods, out); // 押したまま: 変わらない
    CHECK_EQ(r.numLock(), 1);
    r.apply(0, none, mods, out);
    r.apply(0, nl, mods, out);
    CHECK_EQ(r.numLock(), 0);
    CHECK_EQ(r.hidLeds(0x01), 0x00);
    // ラズベリー + L は CLR (Num Lock の usage で送る) で、切り替えない
    const uint8_t l[6] = {0x0f, 0, 0, 0, 0, 0};
    r.apply(0x08, l, mods, out);
    CHECK_EQ(out[0], 0x53);
    CHECK_EQ(r.numLock(), 0);
    CHECK_EQ(x68k::Keyboard::fromHid(out[0]), 0x3f);
    // reset() で Num Lock の状態を忘れる (キーボードが外れたとき)
    r.apply(0, none, mods, out);
    r.apply(0, nl, mods, out);
    CHECK_EQ(r.numLock(), 1);
    r.reset();
    CHECK_EQ(r.numLock(), 0);
    r.apply(0, nl, mods, out); // 押したままでも、reset() のあとは押したことになる
    CHECK_EQ(r.numLock(), 1);
}

// KeyRemap → KeyTracker → X68000 のキーコードの順に流し、押した / 離したの並びを確かめる
struct Chain {
    KeyRemap remap;
    platform::KeyTracker tracker;
    char log[256];
    int n = 0;
    int failNext = 0; // この回数だけ送るのに失敗する (キューが満杯のとき)
    explicit Chain(KeyLayout l) {
        remap.setLayout(l);
        log[0] = 0;
    }
    bool emit(uint8_t u, bool d) {
        if (failNext > 0) {
            failNext--;
            return false;
        }
        n += snprintf(log + n, sizeof log - n, "%s%c%02x", n ? " " : "", d ? '+' : '-',
                      (unsigned)x68k::Keyboard::fromHid(u));
        return true;
    }
    void report(uint8_t mods, uint8_t a = 0, uint8_t b = 0) {
        const uint8_t in[6] = {a, b, 0, 0, 0, 0};
        uint8_t out[6], om = 0;
        remap.apply(mods, in, om, out);
        tracker.report(om, out, [this](uint8_t u, bool d) { return emit(u, d); });
    }
    void resync() { tracker.resync([this](uint8_t u, bool d) { return emit(u, d); }); }
    bool expect(const char* want) {
        bool ok = strcmp(log, want) == 0;
        if (!ok) {
            printf("  got  \"%s\"\n  want \"%s\"\n", log, want);
            failures++;
        }
        n = 0;
        log[0] = 0;
        return ok;
    }
};

void testOrder() {
    printf("press / release order\n");
    const uint8_t G = 0x08, K = 0x0e, C = 0x06, O = 0x12;
    {
        // ラズベリー + K のあと、ラズベリーを先に離す: かな を離すだけ (K を押したことにしない)
        Chain c(KeyLayout::PI400_US);
        c.report(G);
        c.report(G, K);
        c.report(0, K);
        c.report(0);
        c.expect("+5a -5a");
    }
    {
        // K を押したままラズベリーを押す: K のまま
        Chain c(KeyLayout::PI400_US);
        c.report(0, K);
        c.report(G, K);
        c.report(0);
        c.expect("+25 -25");
    }
    {
        // ラズベリー + K のあと、K を先に離す
        Chain c(KeyLayout::PI400_US);
        c.report(G, K);
        c.report(G);
        c.report(0);
        c.expect("+5a -5a");
    }
    {
        // ラズベリー + O + C のあと O を先に離し、続いてラズベリーを離す: OPT.1 + C のまま (コード入力を押さない)
        Chain c(KeyLayout::PI400_US);
        c.report(G);
        c.report(G, O);
        c.report(G, O, C);
        c.report(G, C);
        c.report(0, C);
        c.report(0);
        c.expect("+72 +2c -72 -2c");
    }
    {
        // 同じレポートで ラズベリー + O + C
        Chain c(KeyLayout::PI400_US);
        c.report(G, C, O);
        c.report(0);
        c.expect("+72 +2c -2c -72");
    }
    {
        // 送るのに失敗したら、置き換えた先のまま送り直す
        Chain c(KeyLayout::PI400_US);
        c.failNext = 1;
        c.report(G, K);
        c.expect("");
        c.report(0, K); // ラズベリーを離しても、まだ送れていない かな を送る
        c.expect("+5a");
        c.failNext = 1;
        c.report(0);
        c.expect("");
        c.resync();
        c.expect("-5a");
    }
}

// Pi 400 にある (と分かっている) キーだけで、X68000 のキーがすべて打てるか。Fn との組み合わせ
// (Insert、Home など) は使わない。テンキー ($40-$51) は Num Lock で打つので除く
void checkReachable(KeyLayout layout, const uint8_t* keys, int n) {
    bool want[128] = {}, got[128] = {};
    for (int u = 0; u < 256; u++) {
        uint8_t c = x68k::Keyboard::fromHid((uint8_t)u);
        if (c != 0 && !(c >= 0x40 && c <= 0x51)) want[c] = true;
    }
    KeyRemap r;
    r.setLayout(layout);
    for (int i = 0; i < n; i++) {
        for (int layer = 0; layer < 2; layer++) {
            uint8_t in[6] = {0, 0, 0, 0, 0, 0}, inMods = layer ? 0x08 : 0;
            if (keys[i] >= 0xe0) inMods |= (uint8_t)(1u << (keys[i] - 0xe0));
            else in[0] = keys[i];
            uint8_t out[6], mods = 0;
            r.apply(inMods, in, mods, out);
            const uint8_t none[6] = {0, 0, 0, 0, 0, 0};
            uint8_t dummy[6], dm = 0;
            r.apply(0, none, dm, dummy); // 離す (次は押し直したことにする)
            for (int b = 0; b < 8; b++)
                if (mods & (1u << b)) got[x68k::Keyboard::fromHid((uint8_t)(0xe0 + b))] = true;
            if (out[0] != 0) got[x68k::Keyboard::fromHid(out[0])] = true;
        }
    }
    for (int c = 1; c < 128; c++) {
        if (want[c] && !got[c]) {
            printf("  %s: X68000 key %02x is not reachable\n", platform::keyLayoutName(layout), c);
            failures++;
        }
    }
}

void testReachable() {
    printf("reachable\n");
    uint8_t us[96];
    int n = 0;
    for (int u = 0x04; u <= 0x31; u++) us[n++] = (uint8_t)u; // A-Z、1-0、Enter Esc BS Tab Space - = [ ] '\'
    for (int u = 0x33; u <= 0x45; u++) us[n++] = (uint8_t)u; // ; ' ` , . / CapsLock F1-F12
    us[n++] = 0x4c;                                           // Delete
    for (int u = 0x4f; u <= 0x53; u++) us[n++] = (uint8_t)u; // → ← ↓ ↑ NumLock
    const uint8_t mods[] = {0xe0, 0xe1, 0xe2, 0xe3, 0xe5, 0xe6}; // 左 Ctrl、SHIFT、Alt、ラズベリー、右 SHIFT、AltGr
    for (uint8_t m : mods) us[n++] = m;
    checkReachable(KeyLayout::PI400_US, us, n);

    uint8_t jp[96];
    int m = 0;
    for (int i = 0; i < n; i++) jp[m++] = us[i];
    const uint8_t extra[] = {0x32, 0x87, 0x88, 0x89, 0x8a, 0x8b}; // ] ろ カタカナひらがな ¥ 変換 無変換
    for (uint8_t e : extra) jp[m++] = e;
    checkReachable(KeyLayout::PI400_JP, jp, m);
}

// ラズベリー + M は Menu (0x76) になる。Menu は X68000 のキーではなく、設定メニューを開く (platform/menu.h)
void testMenuKey() {
    printf("menu key\n");
    KeyRemap r;
    r.setLayout(KeyLayout::PI400_JP);
    uint8_t out[6], mods = 0;
    const uint8_t m[6] = {0x10, 0, 0, 0, 0, 0};
    r.apply(0x08, m, mods, out);
    CHECK_EQ(out[0], 0x76);
    CHECK_EQ(mods, 0);
    CHECK_EQ(x68k::Keyboard::fromHid(0x76), 0); // X68000 のキーには対応しない
    // ラズベリーなしの M はそのまま
    const uint8_t none[6] = {0, 0, 0, 0, 0, 0};
    r.apply(0, none, mods, out);
    r.apply(0, m, mods, out);
    CHECK_EQ(out[0], 0x10);
}

} // namespace

int main() {
    testDetect();
    testStandard();
    testPi400Layer();
    testNumLock();
    testOrder();
    testReachable();
    testMenuKey();
    return finish();
}
