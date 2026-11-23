// 何台かの USB キーボードを合わせる処理 (src/platform/key_merge.h) と、Ctrl + Alt + Delete (Raspberry Pi の再起動。
// src/platform/reboot_keys.h) のテスト。ベアメタル版の CKernel::KeyReport() / DispatchKeys() / SendKeyReport() と
// 同じ順 (KeyRemap → KeyMerger → RebootKeyFilter → MenuKeyFilter::filterBits() → KeyTracker::reportBits()) で通し、
// X68000 に届くキー (x68k::Keyboard::fromHid() の値) で確かめる
#include "platform/key_merge.h"
#include "platform/key_remap.h"
#include "platform/key_tracker.h"
#include "platform/menu.h"
#include "platform/reboot_keys.h"
#include "test_check.h"

#include <string.h>
#include "x68k/keyboard.h"

namespace {

using platform::KeyLayout;

// X68000 のキー (fromHid() の値) ごとに押しているか。capacity を超えたら送れない (キューが満杯)
struct X68kSink {
    int capacity = 1000;
    int count = 0;
    int down[128] = {};
    bool operator()(uint8_t usage, bool d) {
        if (count >= capacity) return false;
        count++;
        const uint8_t c = x68k::Keyboard::fromHid(usage);
        if (c != 0 && c < 128) down[c] += d ? 1 : -1;
        return true;
    }
    int held() const {
        int n = 0;
        for (int v : down) n += v != 0;
        return n;
    }
};

// ベアメタル版のキーの扱いを真似たもの
struct Pipeline {
    static constexpr int N = 4;
    platform::KeyRemap remap[N];
    platform::KeyMerger<N> merge;
    platform::RebootKeyFilter reboot;
    platform::MenuKeyFilter menu;
    platform::KeyTracker keys;
    X68kSink sink;
    bool menuOpen = false;
    bool mixerOpen = false; // X68000 を止めないミキサー (キーは設定メニューと同じく X68000 に送らない)
    int mixerOpens = 0;
    int reboots = 0;
    int menuOpens = 0;
    uint8_t menuEvents[16] = {};
    int nMenuEvents = 0;
    uint8_t lastMods = 0; // CKernel::m_LastMods (メニューに渡す前の修飾キー)

    void report(int slot, uint8_t mods, const uint8_t k[6]) {
        uint8_t m, out[6];
        remap[slot].apply(mods, k, m, out);
        if (!merge.set(slot, m, out)) return;
        dispatch();
    }
    void report(int slot, uint8_t mods, uint8_t k0 = 0, uint8_t k1 = 0, uint8_t k2 = 0, uint8_t k3 = 0) {
        const uint8_t k[6] = {k0, k1, k2, k3, 0, 0};
        report(slot, mods, k);
    }
    void remove(int slot) {
        remap[slot].reset();
        merge.clear(slot);
        dispatch();
    }
    void dispatch() {
        uint8_t mods, down[32];
        merge.merged(mods, down);
        if (reboot.filter(mods, down)) reboots++;
        lastMods = mods;
        uint8_t outMods = 0, outDown[32], ev[6];
        int n = 0;
        switch (menu.filterBits(mods, down, menuOpen || mixerOpen, outMods, outDown, ev, n)) {
        case platform::MenuKeyFilter::OPEN:
            keys.releaseAll(sink);
            menuOpen = true;
            menuOpens++;
            return;
        case platform::MenuKeyFilter::MENU:
            for (int i = 0; i < n && nMenuEvents < 16; i++) menuEvents[nMenuEvents++] = ev[i];
            return;
        case platform::MenuKeyFilter::OPEN_MIXER:
            keys.releaseAll(sink);
            mixerOpen = true;
            mixerOpens++;
            return;
        case platform::MenuKeyFilter::PASS:
            break;
        }
        keys.reportBits(outMods, outDown, sink);
    }
    // メニューを閉じる (CKernel::CloseMenu() と同じく、いま押しているキーは一度離すまで送らない)
    void closeMenu() {
        menu.closed(lastMods);
        menuOpen = mixerOpen = false;
    }
    int x(uint8_t usage) const { return sink.down[x68k::Keyboard::fromHid(usage)]; }
};

constexpr uint8_t LCTRL = 0x01, LALT = 0x04, LGUI = 0x08, RCTRL = 0x10, RALT = 0x40, LSHIFT = 0x02;
constexpr uint8_t DEL = 0x4c, F11 = 0x44, A = 0x04, B = 0x05;

void testMergeSameKey() {
    printf("same key on two keyboards\n");
    Pipeline p;
    p.report(0, 0, A);
    p.report(1, 0, A);
    CHECK_EQ(p.x(A), 1);
    // 片方を離しても押したまま
    p.report(0, 0);
    CHECK_EQ(p.x(A), 1);
    p.report(1, 0);
    CHECK_EQ(p.x(A), 0);
    // SHIFT も同じ
    p.report(0, LSHIFT);
    p.report(1, LSHIFT, B);
    p.report(0, 0);
    CHECK_EQ(p.x(0xe1), 1);
    CHECK_EQ(p.x(B), 1);
    p.report(1, 0);
    CHECK_EQ(p.sink.held(), 0);
}

void testRemove() {
    printf("remove one keyboard\n");
    Pipeline p;
    p.report(0, LSHIFT, A);
    p.report(1, 0, A, B);
    p.remove(1);
    // キーボード 1 の A と SHIFT は残り、B だけ離す
    CHECK_EQ(p.x(A), 1);
    CHECK_EQ(p.x(B), 0);
    CHECK_EQ(p.x(0xe1), 1);
    p.remove(0);
    CHECK_EQ(p.sink.held(), 0);
}

void testMoreThanSix() {
    printf("more than 6 keys over two keyboards\n");
    Pipeline p;
    p.report(0, 0, 0x04, 0x05, 0x06, 0x07);
    p.report(1, 0, 0x08, 0x09, 0x0a, 0x0b);
    CHECK_EQ(p.sink.held(), 8);
    p.report(0, 0);
    CHECK_EQ(p.sink.held(), 4);
    p.report(1, 0);
    CHECK_EQ(p.sink.held(), 0);
}

void testRollOver() {
    printf("ErrorRollOver keeps the keyboard's previous keys\n");
    Pipeline p;
    p.report(0, 0, A);
    p.report(1, 0, B);
    const uint8_t ro[6] = {1, 1, 1, 1, 1, 1};
    p.report(0, LSHIFT, ro);
    CHECK_EQ(p.x(A), 1);
    CHECK_EQ(p.x(B), 1);
    CHECK_EQ(p.x(0xe1), 0);
    p.report(0, 0);
    CHECK_EQ(p.x(A), 0);
    CHECK_EQ(p.x(B), 1);
}

void testQueueFull() {
    printf("queue full, then resync\n");
    Pipeline p;
    p.report(0, 0, A);
    p.report(1, 0, B);
    p.sink.capacity = p.sink.count; // 送れない
    p.remove(0);
    p.remove(1);
    CHECK_EQ(p.keys.pending(), 1);
    p.sink.capacity = 1000;
    p.keys.resync(p.sink);
    CHECK_EQ(p.keys.pending(), 0);
    CHECK_EQ(p.sink.held(), 0);
}

void testRebootCombo() {
    printf("Ctrl+Alt+Delete\n");
    {
        Pipeline p;
        p.report(0, LCTRL);
        p.report(0, LCTRL | LALT);
        CHECK_EQ(p.x(0xe0), 1); // CTRL
        CHECK_EQ(p.x(0xe2), 1); // XF1
        p.report(0, LCTRL | LALT, DEL);
        CHECK_EQ(p.reboots, 1);
        // Delete は送らず、Ctrl と Alt は離したことにする
        CHECK_EQ(p.x(DEL), 0);
        CHECK_EQ(p.sink.held(), 0);
        // 押したままのあいだは送らない。もう一度 Delete を押しても、Ctrl と Alt を離していなければ X68000 には送らない
        p.report(0, LCTRL | LALT);
        p.report(0, LCTRL | LALT, DEL);
        CHECK_EQ(p.reboots, 2); // 再起動を断ったあとにもう一度押した (主ループが書き戻さずに再起動する)
        CHECK_EQ(p.sink.held(), 0);
        // 離せば、また送る
        p.report(0, 0);
        p.report(0, LCTRL, A);
        CHECK_EQ(p.x(0xe0), 1);
        CHECK_EQ(p.x(A), 1);
    }
    {
        // 右 Ctrl + 右 Alt でも、同じレポートで押しても
        Pipeline p;
        p.report(0, RCTRL | RALT, DEL);
        CHECK_EQ(p.reboots, 1);
        CHECK_EQ(p.sink.held(), 0);
    }
    {
        // Delete を先に押したときは再起動しない (Delete は X68000 に送ってある)
        Pipeline p;
        p.report(0, 0, DEL);
        p.report(0, LCTRL | LALT, DEL);
        CHECK_EQ(p.reboots, 0);
        CHECK_EQ(p.x(DEL), 1);
    }
    {
        // Ctrl をキーボード 1、Alt + Delete をキーボード 2 で押しても再起動する
        Pipeline p;
        p.report(0, LCTRL);
        p.report(1, LALT);
        p.report(1, LALT, DEL);
        CHECK_EQ(p.reboots, 1);
        CHECK_EQ(p.sink.held(), 0);
        // キーボード 1 の Ctrl を離さずに、キーボード 2 だけ離して押し直しても、Ctrl は送らない
        p.report(1, 0);
        CHECK_EQ(p.x(0xe0), 0);
    }
}

// Ctrl + F11 + Delete は Pi の再起動にせず、X68000 に CTRL + XF1 + DEL としてそのまま送る
void testCtrlXf1Delete() {
    printf("Ctrl+F11+Delete goes to the X68000 (CTRL+XF1+DEL)\n");
    CHECK_EQ(x68k::Keyboard::fromHid(F11), x68k::Keyboard::fromHid(0xe2)); // F11 も 左 Alt も XF1
    {
        Pipeline p;
        p.report(0, LCTRL);
        p.report(0, LCTRL, F11);
        p.report(0, LCTRL, F11, DEL);
        CHECK_EQ(p.reboots, 0);
        CHECK_EQ(p.x(0xe0), 1);
        CHECK_EQ(p.x(F11), 1);
        CHECK_EQ(p.x(DEL), 1);
    }
    {
        // Ctrl + Alt + F11 + Delete も X68000 に送る (Alt と F11 はどちらも XF1)
        Pipeline p;
        p.report(0, LCTRL | LALT, F11);
        p.report(0, LCTRL | LALT, F11, DEL);
        CHECK_EQ(p.reboots, 0);
        CHECK_EQ(p.x(DEL), 1);
        CHECK_EQ(p.x(F11), 2); // XF1 は Alt と F11 の 2 つの usage から押している
    }
    {
        // F11 を別のキーボードで押していても同じ
        Pipeline p;
        p.report(1, 0, F11);
        p.report(0, LCTRL | RALT, DEL);
        CHECK_EQ(p.reboots, 0);
        CHECK_EQ(p.x(DEL), 1);
    }
    {
        // F20 (XF1。Pi 400 のラズベリー + 1) を押していても再起動しない
        Pipeline p;
        p.report(0, LCTRL | LALT, 0x6f, DEL);
        CHECK_EQ(p.reboots, 0);
    }
}

void testX68kReset() {
    printf("Ctrl+GUI+Delete (CTRL+OPT.1+DEL) goes to the X68000\n");
    {
        Pipeline p;
        p.report(0, LCTRL | LGUI, DEL);
        CHECK_EQ(p.reboots, 0);
        CHECK_EQ(p.x(0xe3), 1); // OPT.1
        CHECK_EQ(p.x(DEL), 1);
    }
    {
        // GUI を押していれば、Alt があっても X68000 に送る
        Pipeline p;
        p.report(0, LCTRL | LALT | LGUI, DEL);
        CHECK_EQ(p.reboots, 0);
        CHECK_EQ(p.x(DEL), 1);
    }
}

void testPi400() {
    printf("Pi 400\n");
    {
        // Ctrl + Alt + Delete はそのまま再起動
        Pipeline p;
        p.remap[0].setLayout(KeyLayout::PI400_US);
        p.report(0, LCTRL | LALT, DEL);
        CHECK_EQ(p.reboots, 1);
    }
    {
        // ラズベリー + Delete は INS (Insert) になるので、Ctrl + Alt + ラズベリー + Delete は X68000 に送る
        Pipeline p;
        p.remap[0].setLayout(KeyLayout::PI400_US);
        p.report(0, LCTRL | LALT | LGUI, DEL);
        CHECK_EQ(p.reboots, 0);
        CHECK_EQ(p.x(0x49), 1); // INS
        CHECK_EQ(p.x(DEL), 0);
    }
    {
        // ラズベリー + O (OPT.1) を押したまま Ctrl + Delete: X68000 のリセット (CTRL + OPT.1 + DEL)。Delete は INS にならない
        Pipeline p;
        p.remap[0].setLayout(KeyLayout::PI400_US);
        p.report(0, LCTRL | LGUI, 0x12);
        p.report(0, LCTRL | LGUI, 0x12, DEL);
        CHECK_EQ(p.reboots, 0);
        CHECK_EQ(p.x(0xe0), 1);
        CHECK_EQ(p.x(0xe3), 1);
        CHECK_EQ(p.x(DEL), 1);
        CHECK_EQ(p.x(0x49), 0);
    }
    {
        // O と Delete を同じレポートで押しても同じ (O を先に決める)
        Pipeline p;
        p.remap[0].setLayout(KeyLayout::PI400_US);
        p.report(0, LCTRL | LGUI, 0x12, DEL);
        CHECK_EQ(p.reboots, 0);
        CHECK_EQ(p.x(0xe3), 1);
        CHECK_EQ(p.x(DEL), 1);
        CHECK_EQ(p.x(0x49), 0);
    }
    {
        // Alt も押していても (Ctrl + Alt + ラズベリー + O + Delete)、OPT.1 を押しているので再起動しない
        Pipeline p;
        p.remap[0].setLayout(KeyLayout::PI400_US);
        p.report(0, LCTRL | LALT | LGUI, 0x12);
        p.report(0, LCTRL | LALT | LGUI, 0x12, DEL);
        CHECK_EQ(p.reboots, 0);
        CHECK_EQ(p.x(DEL), 1);
    }
    {
        // Delete を先に押してからラズベリー + O: Delete はラズベリーを押す前に決まった Delete のまま
        Pipeline p;
        p.remap[0].setLayout(KeyLayout::PI400_US);
        p.report(0, LCTRL, DEL);
        p.report(0, LCTRL | LGUI, DEL, 0x12);
        CHECK_EQ(p.reboots, 0);
        CHECK_EQ(p.x(DEL), 1);
        CHECK_EQ(p.x(0xe3), 1);
    }
    {
        // Pi 400 の本体 (キーボード 1) と、外付けの普通のキーボード (キーボード 2)。置き換えは本体だけ
        Pipeline p;
        p.remap[0].setLayout(KeyLayout::PI400_US);
        p.report(0, LGUI, 0x0e); // ラズベリー + K → かな (F16)
        p.report(1, LGUI, 0x0e); // 外付けの GUI + K は OPT.1 + K
        CHECK_EQ(p.x(0x6b), 1);
        CHECK_EQ(p.x(0xe3), 1);
        CHECK_EQ(p.x(0x0e), 1);
    }
}

void testMenuWithReboot() {
    printf("menu keys and Ctrl+Alt+Delete with the menu open\n");
    Pipeline p;
    p.report(1, 0, 0x47); // Scroll Lock (キーボード 2)
    CHECK_EQ(p.menuOpens, 1);
    p.report(1, 0);
    p.report(0, 0, 0x51); // ↓ (キーボード 1)
    CHECK_EQ(p.nMenuEvents, 1);
    CHECK_EQ(p.menuEvents[0], 0x51);
    // メニューを開いていても再起動する。Delete はメニューに渡さない
    p.report(0, LCTRL | LALT, DEL);
    CHECK_EQ(p.reboots, 1);
    CHECK_EQ(p.nMenuEvents, 1);
}

void testMenuCloseWithKeysHeld() {
    printf("close the menu while keys are held on two keyboards\n");
    Pipeline p;
    p.report(0, LCTRL, A);
    CHECK_EQ(p.x(A), 1);
    p.report(1, 0, 0x47); // キーボード 2 の Scroll Lock でメニューを開く
    CHECK_EQ(p.menuOpens, 1);
    CHECK_EQ(p.sink.held(), 0); // 押していたキーは離したことにする
    p.report(1, 0, 0x28);       // Enter (キーボード 2) を押したまま閉じる
    p.closeMenu();
    // キーボード 1 の CTRL + A と、キーボード 2 の Enter は、離すまで送らない
    p.report(1, 0, 0x28, B);
    CHECK_EQ(p.x(B), 1);
    CHECK_EQ(p.x(A), 0);
    CHECK_EQ(p.x(0x28), 0);
    CHECK_EQ(p.x(0xe0), 0);
    p.report(1, 0);
    p.report(0, 0);
    CHECK_EQ(p.sink.held(), 0);
    p.report(0, LCTRL, A);
    CHECK_EQ(p.x(A), 1);
    CHECK_EQ(p.x(0xe0), 1);
    p.report(0, 0);
    CHECK_EQ(p.sink.held(), 0);
}

void testRemoveInMenu() {
    printf("remove a keyboard while the menu is open\n");
    Pipeline p;
    p.report(0, 0, 0x47);
    p.report(0, 0);
    p.report(1, 0, 0x51); // キーボード 2 で ↓ を押したまま
    CHECK_EQ(p.menu.isHeld(0x51), 1);
    p.remove(1);
    CHECK_EQ(p.menu.isHeld(0x51), 0); // 繰り返しが止まる
    p.closeMenu();
    p.report(0, 0, A);
    CHECK_EQ(p.x(A), 1);
    p.report(0, 0);
    CHECK_EQ(p.sink.held(), 0);
}

void testRebootRefusedThenClose() {
    printf("Ctrl+Alt+Delete refused in the menu, then close\n");
    Pipeline p;
    p.report(0, 0, 0x47);
    p.report(0, 0);
    p.report(0, LCTRL | LALT, DEL); // メニューを開いたまま再起動を頼む (主ループが断ったとする)
    CHECK_EQ(p.reboots, 1);
    CHECK_EQ(p.nMenuEvents, 0);
    p.closeMenu(); // Ctrl と Alt を押したまま閉じる
    p.report(0, LCTRL | LALT, A);
    CHECK_EQ(p.x(A), 1);
    CHECK_EQ(p.x(0xe0), 0);
    CHECK_EQ(p.x(0xe2), 0);
    p.report(0, 0);
    CHECK_EQ(p.sink.held(), 0);
    // 離したあとは普通に送る
    p.report(0, LCTRL, A);
    CHECK_EQ(p.x(0xe0), 1);
    p.report(0, 0);
    CHECK_EQ(p.sink.held(), 0);
    // メニューを開かずに断ったときも、離せば X68000 に送る
    p.report(1, RCTRL | RALT, DEL);
    CHECK_EQ(p.reboots, 2);
    p.report(1, 0);
    p.report(1, RCTRL, DEL);
    CHECK_EQ(p.x(0xe4), 1);
    CHECK_EQ(p.x(DEL), 1);
    p.report(1, 0);
    CHECK_EQ(p.sink.held(), 0);
}

// Ctrl + Alt + Delete の受け渡しと、書けずに断ったあとの 2 回目 (CKernel::RebootFromKeyboard() の判定)
void testRebootRequest() {
    printf("reboot request: refusal and the second press\n");
    const uint64_t S = 1000; // 1 秒を 1000 とする
    platform::RebootRequest r(60 * S);
    CHECK_EQ(r.take(), 0);
    r.press();
    CHECK_EQ(r.take(), 1);
    CHECK_EQ(r.take(), 0);
    // 1 回目を処理している間 (書いている最中) に押し直した
    r.press();
    // 1 回目は書けずに断る: 断る前の押し直しは捨てる (2 回目と取り違えない)
    CHECK_EQ(r.refused(10 * S), 0);
    CHECK_EQ(r.take(), 0);
    // 断ったあとに押した 2 回目: もう一度書けなければ、書かずに再起動してよい
    r.press();
    CHECK_EQ(r.take(), 1);
    CHECK_EQ(r.refused(10 * S + 700), 1);
    // 60 秒を過ぎたら、もう一度断る (時刻を覚え直す)
    platform::RebootRequest q(60 * S);
    CHECK_EQ(q.refused(5 * S), 0);
    CHECK_EQ(q.refused(65 * S), 1);  // ちょうど 60 秒は 2 回目として扱う
    CHECK_EQ(q.refused(126 * S), 0); // 61 秒後: 断り直す
    CHECK_EQ(q.refused(127 * S), 1);
}

// MenuKeyFilter::filterBits() を直接確かめる
void testFilterBits() {
    printf("MenuKeyFilter::filterBits\n");
    auto bits = [](uint8_t* d, uint8_t a = 0, uint8_t b = 0) {
        memset(d, 0, 32);
        if (a) d[a >> 3] |= (uint8_t)(1u << (a & 7));
        if (b) d[b >> 3] |= (uint8_t)(1u << (b & 7));
    };
    auto has = [](const uint8_t* d, uint8_t u) { return (d[u >> 3] >> (u & 7)) & 1; };
    uint8_t down[32], out[32], ev[6], outMods = 0;
    int n = 0;
    {
        // メニューのキーを押したまま、メニューを開かなかった (開けなかった) とき: 2 回目からは PASS だが、メニューのキーは送らない
        platform::MenuKeyFilter f;
        bits(down, 0x47, A);
        CHECK_EQ((int)f.filterBits(0, down, false, outMods, out, ev, n), (int)platform::MenuKeyFilter::OPEN);
        CHECK_EQ((int)f.filterBits(0, down, false, outMods, out, ev, n), (int)platform::MenuKeyFilter::PASS);
        CHECK_EQ(has(out, 0x47), 0);
        CHECK_EQ(has(out, A), 1);
    }
    {
        // closed() のときに押していたキーと修飾キーは、離すまで送らない。離して押し直せば送る
        platform::MenuKeyFilter f;
        bits(down, A);
        f.filterBits(LSHIFT, down, true, outMods, out, ev, n);
        f.closed(LSHIFT);
        bits(down, A, B);
        CHECK_EQ((int)f.filterBits(LSHIFT | LCTRL, down, false, outMods, out, ev, n), (int)platform::MenuKeyFilter::PASS);
        CHECK_EQ(has(out, A), 0);
        CHECK_EQ(has(out, B), 1);
        CHECK_EQ(outMods, LCTRL);
        bits(down);
        f.filterBits(0, down, false, outMods, out, ev, n);
        bits(down, A);
        f.filterBits(LSHIFT, down, false, outMods, out, ev, n);
        CHECK_EQ(has(out, A), 1);
        CHECK_EQ(outMods, LSHIFT);
    }
    {
        // メニューを開いている間、新しく押したキーだけをメニューに渡す (押したままのキーは渡さない)
        platform::MenuKeyFilter f;
        bits(down, 0x51);
        CHECK_EQ((int)f.filterBits(0, down, true, outMods, out, ev, n), (int)platform::MenuKeyFilter::MENU);
        CHECK_EQ(n, 1);
        CHECK_EQ(ev[0], 0x51);
        bits(down, 0x51, 0x52);
        f.filterBits(0, down, true, outMods, out, ev, n);
        CHECK_EQ(n, 1);
        CHECK_EQ(ev[0], 0x52);
        CHECK_EQ(f.isHeld(0x51), 1);
    }
}

// KeyMerger::set() はキーの欄に来た修飾キーの usage を修飾キーのビットにする。KeyTracker::reportBits() は usage 1..3 を無視する
void testBitsEdges() {
    printf("modifier usages in the key array, usage 1..3 in bits\n");
    platform::KeyMerger<2> m;
    const uint8_t k[6] = {0xe1, A, 0, 0, 0, 0};
    CHECK_EQ(m.set(0, 0, k), 1);
    uint8_t mods, down[32];
    m.merged(mods, down);
    CHECK_EQ(mods, LSHIFT);
    CHECK_EQ((down[0xe1 >> 3] >> (0xe1 & 7)) & 1, 0);
    CHECK_EQ((down[A >> 3] >> (A & 7)) & 1, 1);

    platform::KeyTracker t;
    X68kSink sink;
    int events = 0;
    uint8_t bits[32] = {};
    bits[0] = 0x0e; // usage 1, 2, 3
    t.reportBits(0, bits, [&](uint8_t u, bool d) {
        events++;
        return sink(u, d);
    });
    CHECK_EQ(events, 0);
    CHECK_EQ(t.pending(), 0);
}

} // namespace

// Shift + メニューのキーでミキサーを開く。キーを押したまま開き、押したまま閉じる
void testMixerWithKeysHeld() {
    printf("open and close the mixer while keys are held\n");
    Pipeline p;
    p.report(0, LSHIFT, A);
    CHECK_EQ(p.x(A), 1);
    p.report(1, 0, 0x47); // キーボード 2 の Scroll Lock
    CHECK_EQ(p.mixerOpens, 1);
    CHECK_EQ(p.menuOpens, 0);
    CHECK_EQ(p.sink.held(), 0); // 押していたキーと Shift は離したことにする
    CHECK_EQ(p.nMenuEvents, 0);
    p.report(1, 0, 0x50); // ←
    CHECK_EQ(p.nMenuEvents, 1);
    CHECK_EQ(p.menuEvents[0], 0x50);
    p.report(0, LSHIFT, A, B); // 開いている間に押した B もミキサーへ
    CHECK_EQ(p.sink.held(), 0);
    p.closeMenu();
    p.report(1, 0);
    CHECK_EQ(p.sink.held(), 0); // SHIFT、A、B は押したまま
    p.report(0, 0);
    CHECK_EQ(p.sink.held(), 0);
    p.report(0, LSHIFT, A);
    CHECK_EQ(p.x(A), 1);
    CHECK_EQ(p.x(0xe1), 1);
    p.report(0, 0);
    CHECK_EQ(p.sink.held(), 0);
}

// Pi 400 は Shift + ラズベリー + M
void testMixerPi400() {
    printf("Pi 400: Shift + raspberry + M opens the mixer\n");
    Pipeline p;
    p.remap[0].setLayout(platform::KeyLayout::PI400_JP);
    p.report(0, LSHIFT | 0x08, 0x10);
    CHECK_EQ(p.mixerOpens, 1);
    CHECK_EQ(p.menuOpens, 0);
    CHECK_EQ(p.sink.held(), 0);
}

int main() {
    testMixerWithKeysHeld();
    testMixerPi400();
    testMergeSameKey();
    testRemove();
    testMoreThanSix();
    testRollOver();
    testQueueFull();
    testRebootCombo();
    testCtrlXf1Delete();
    testX68kReset();
    testPi400();
    testMenuWithReboot();
    testMenuCloseWithKeysHeld();
    testRemoveInMenu();
    testRebootRefusedThenClose();
    testRebootRequest();
    testFilterBits();
    testBitsEdges();
    return finish();
}
