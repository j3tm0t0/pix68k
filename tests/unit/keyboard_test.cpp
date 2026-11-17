// キーボード (src/x68k/keyboard.cpp) のテスト: HID → X68000 のキーコード、キューとあふれ、
// 2400bps の送信の間隔、MFP の受信バッファと受信許可、本体からのコマンド、キーリピート
#include <stdio.h>
#include <string.h>

#include "x68k/keyboard.h"
#include "x68k/mfp.h"

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

const uint32_t kHz = 16000000;
const uint64_t kByte = kHz / 240; // 2400bps で 10 ビット

// MFP の受信を許可し (RSR bit0)、受信バッファの割り込みを有効にした状態にする
void setupMfp(x68k::Mfp& mfp) {
    mfp.reset();
    mfp.write(21, 0x01); // RSR: RE
    mfp.write(3, 0x10);  // IERA: 受信バッファフル
    mfp.write(9, 0x10);  // IMRA
}

// 受信したバイトを読む (UDR)。受信バッファが空なら -1
int readByte(x68k::Mfp& mfp) {
    if (!(mfp.read(21) & 0x80)) return -1;
    return mfp.read(23);
}

void testHidTable() {
    printf("hid table\n");
    using K = x68k::Keyboard;
    // HID の usage ID は SDL_scancode.h、X68000 のキーコードの根拠は x68k/keyboard.h の Code の注釈
    struct {
        uint8_t hid, x68k;
    } cases[] = {
        {0x04, 0x1e}, {0x16, 0x1f}, {0x1d, 0x2a}, {0x10, 0x30}, {0x14, 0x11}, {0x13, 0x1a}, // A S Z M Q P
        {0x1e, 0x02}, {0x26, 0x0a}, {0x27, 0x0b},                                           // 1 9 0
        {0x28, 0x1d}, {0x29, 0x01}, {0x2a, 0x0f}, {0x2b, 0x10}, {0x2c, 0x35},               // RET ESC BS TAB SPC
        {0x2d, 0x0c}, {0x2e, 0x0d}, {0x2f, 0x1b}, {0x30, 0x1c}, {0x32, 0x29},               // - ^ @ [ ]
        {0x33, 0x27}, {0x34, 0x28}, {0x36, 0x31}, {0x37, 0x32}, {0x38, 0x33},               // ; : , . /
        {0x87, 0x34}, {0x89, 0x0e},                                                         // _ ¥ (JIS)
        {0x39, 0x5d}, {0x3a, 0x63}, {0x43, 0x6c},                                           // CAPS F1 F10
        {0x49, 0x5e}, {0x4a, 0x36}, {0x4c, 0x37}, {0x4d, 0x3a},                             // INS HOME DEL UNDO
        {0x4b, 0x39}, {0x4e, 0x38},                                                         // ROLL DOWN / UP
        {0x50, 0x3b}, {0x52, 0x3c}, {0x4f, 0x3d}, {0x51, 0x3e}, {0x53, 0x3f},               // ← ↑ → ↓ CLR
        {0x54, 0x40}, {0x55, 0x41}, {0x56, 0x42}, {0x57, 0x46}, {0x58, 0x4e},               // テンキー / * - + ENTER
        {0x5f, 0x43}, {0x60, 0x44}, {0x61, 0x45}, {0x5c, 0x47}, {0x5d, 0x48}, {0x5e, 0x49}, // テンキー 7 8 9 4 5 6
        {0x59, 0x4b}, {0x5a, 0x4c}, {0x5b, 0x4d}, {0x62, 0x4f}, {0x63, 0x51}, {0x67, 0x4a}, // 1 2 3 0 . =
        {0x85, 0x50},                                                                       // テンキー ,
        {0x68, 0x52}, {0x69, 0x53}, {0x6a, 0x54}, {0x75, 0x54},                             // 記号入力 登録 HELP
        {0x46, 0x62}, {0x48, 0x61},                                                         // COPY BREAK
        {0x44, 0x55}, {0x45, 0x56}, {0x8b, 0x56}, {0x8a, 0x57}, {0x88, 0x58},               // XF1-XF4
        {0x90, 0x5f},                                                                       // ひらがな
        {0x6b, 0x5a}, {0x6c, 0x5b}, {0x6d, 0x5c}, {0x6e, 0x60},                             // F16-F19: かな ローマ字 コード入力 全角
        {0x6f, 0x55}, {0x70, 0x56}, {0x71, 0x57}, {0x72, 0x58}, {0x73, 0x59},               // F20-F24: XF1-XF5
        {0xe1, 0x70}, {0xe5, 0x70}, {0xe0, 0x71}, {0xe4, 0x71}, {0xe3, 0x72}, {0xe7, 0x73}, // SHIFT CTRL OPT
        {0xe2, 0x55}, {0xe6, 0x59},                                                         // XF1 XF5
        {0x00, 0x00}, {0x47, 0x00}, {0x65, 0x00}, {0xff, 0x00},                             // 対応なし
    };
    for (auto& c : cases) {
        uint8_t got = K::fromHid(c.hid);
        if (got != c.x68k) {
            printf("  HID %02x: got %02x, want %02x\n", c.hid, got, c.x68k);
            failures++;
        }
    }
    // 対応のあるキーのコードは $01-$73 に収まり、F10 と SHIFT の間 ($6D-$6F) は使わない
    for (int u = 0; u < 256; u++) {
        uint8_t v = K::fromHid((uint8_t)u);
        if (v > 0x73 || (v >= 0x6d && v <= 0x6f)) {
            printf("  HID %02x: bad code %02x\n", u, v);
            failures++;
        }
    }
}

void testAscii() {
    printf("ascii\n");
    using K = x68k::Keyboard;
    bool shift;
    CHECK_EQ(K::fromAscii('a', shift), 0x1e);
    CHECK_EQ(shift, false);
    CHECK_EQ(K::fromAscii('A', shift), 0x1e);
    CHECK_EQ(shift, true);
    CHECK_EQ(K::fromAscii(':', shift), 0x28);
    CHECK_EQ(shift, false);
    CHECK_EQ(K::fromAscii('*', shift), 0x28);
    CHECK_EQ(shift, true);
    CHECK_EQ(K::fromAscii('"', shift), 0x03);
    CHECK_EQ(shift, true);
    CHECK_EQ(K::fromAscii('_', shift), 0x34); // X68000 の _ は SHIFT が要る (Human68k で確かめた)
    CHECK_EQ(shift, true);
    CHECK_EQ(K::fromAscii('\n', shift), 0x1d);
    CHECK_EQ(K::fromAscii(' ', shift), 0x35);
    CHECK_EQ(K::fromAscii('\x7f', shift), 0);
    // すべての印字可能な文字が打てる
    for (int c = 0x20; c < 0x7f; c++) {
        if (K::fromAscii((char)c, shift) == 0) {
            printf("  ascii %02x: no key\n", c);
            failures++;
        }
    }
}

// HID の A を押して離すと、$1E と $9E が 2400bps の間隔で届く
void testMakeBreakAndPacing() {
    printf("make/break and pacing\n");
    x68k::Mfp mfp;
    setupMfp(mfp);
    static x68k::Keyboard kb(kHz);
    kb.reset();
    uint64_t now = 1000;
    kb.update(now, mfp);
    kb.onKey(0x04, true);
    kb.onKey(0x04, true); // OS のキーリピートは無視する
    kb.onKey(0x04, false);
    CHECK_EQ(kb.queued(), 2);
    kb.update(now, mfp);
    CHECK_EQ(mfp.irq(), true);
    CHECK_EQ(readByte(mfp), 0x1e);
    // 次のバイトは 1 バイト分の時間がたつまで来ない
    kb.update(now + kByte - 1, mfp);
    CHECK_EQ(readByte(mfp), -1);
    kb.update(now + kByte, mfp);
    CHECK_EQ(readByte(mfp), 0x9e);
    CHECK_EQ(kb.queued(), 0);
    CHECK_EQ(kByte, 66666u); // 約 4.17ms
}

// 本体が読まないうちは次のバイトを渡さない。受信が禁止なら捨てる
void testRxBuffer() {
    printf("rx buffer\n");
    x68k::Mfp mfp;
    setupMfp(mfp);
    static x68k::Keyboard kb(kHz);
    kb.reset();
    kb.press(0x11, true);
    kb.press(0x11, false);
    kb.update(0, mfp);
    CHECK_EQ(kb.queued(), 1);
    kb.update(kByte * 5, mfp); // 前のバイト ($11) がまだ読まれていない
    CHECK_EQ(kb.queued(), 1);
    CHECK_EQ(readByte(mfp), 0x11);
    // 読まれないまま 100ms たったら上書きする (受信の割り込みを禁止したソフトで止まらないように)
    kb.press(0x14, true);
    kb.update(kByte * 5 + 1, mfp); // $91 を渡す
    kb.update(kByte * 7, mfp);
    CHECK_EQ(kb.queued(), 1);
    const uint64_t t = kByte * 6 + 1 + kHz / 10; // $91 を渡した次の送信の時刻から 100ms
    kb.update(t - 1, mfp);
    CHECK_EQ(kb.queued(), 1);
    kb.update(t, mfp);
    CHECK_EQ(kb.queued(), 0);
    CHECK_EQ(readByte(mfp), 0x14);
    kb.press(0x14, false);
    kb.update(t + kByte, mfp);
    CHECK_EQ(readByte(mfp), 0x94);

    mfp.write(21, 0x00); // 受信を禁止
    kb.press(0x12, true);
    kb.update(t + kByte * 2, mfp);
    CHECK_EQ(kb.queued(), 0);
    CHECK_EQ(readByte(mfp), -1);
    kb.press(0x12, false);

    // キーボードの送出の禁止 ($48) と許可 ($49)
    mfp.write(21, 0x01);
    kb.update(t + kByte * 3, mfp); // $92 を捨てずに受ける
    CHECK_EQ(readByte(mfp), 0x92);
    mfp.write(23, 0x48);
    kb.update(t + kByte * 4, mfp);
    CHECK_EQ(kb.enabled(), false);
    kb.press(0x13, true);
    kb.update(t + kByte * 6, mfp);
    CHECK_EQ(readByte(mfp), -1);
    mfp.write(23, 0x49);
    kb.update(t + kByte * 7, mfp);
    CHECK_EQ(kb.enabled(), true);
    CHECK_EQ(readByte(mfp), 0x13);
}

// 2 つの物理キーが同じコードのとき (左右の SHIFT)、両方を離したときだけ break を送る
void testSharedCode() {
    printf("shared code\n");
    x68k::Mfp mfp;
    setupMfp(mfp);
    static x68k::Keyboard kb(kHz);
    kb.reset();
    kb.onKey(0xe1, true);  // 左 SHIFT
    kb.onKey(0xe5, true);  // 右 SHIFT
    kb.onKey(0xe1, false);
    CHECK_EQ(kb.queued(), 1); // $70 だけ
    kb.onKey(0xe5, false);
    CHECK_EQ(kb.queued(), 2);
    kb.update(0, mfp);
    CHECK_EQ(readByte(mfp), 0x70);
    kb.update(kByte, mfp);
    CHECK_EQ(readByte(mfp), 0xf0);
}

// キューがあふれても、break だけが届くことはなく、break は落とさない
void testOverflow() {
    printf("overflow\n");
    x68k::Mfp mfp;
    setupMfp(mfp);
    static x68k::Keyboard kb(kHz);
    kb.reset();
    // 押したままのキーを増やしていき、キューを満たす
    int made = 0;
    for (int u = 0x04; u <= 0x63; u++) {
        int before = kb.queued();
        kb.onKey((uint8_t)u, true);
        if (kb.queued() > before) made++;
    }
    CHECK_EQ(made < 0x60, true);
    CHECK_EQ(kb.queued() <= x68k::Keyboard::QUEUE_SIZE - 1, true);
    // 全部離す: make を送ったキーの break はすべて (あとからでも) 届き、ほかの break は出ない
    for (int u = 0x04; u <= 0x63; u++) kb.onKey((uint8_t)u, false);
    int makes = 0, breaks = 0;
    bool seen[128] = {};
    uint64_t t = 0;
    for (int i = 0; i < 1000; i++) {
        kb.update(t, mfp);
        t += kByte;
        int v = readByte(mfp);
        if (v < 0) continue;
        if (v & 0x80) {
            if (!seen[v & 0x7f]) {
                printf("  break %02x without make\n", v);
                failures++;
            }
            breaks++;
        } else {
            seen[v] = true;
            makes++;
        }
    }
    CHECK_EQ(makes, made);
    CHECK_EQ(breaks, made);
}

// 本体からのコマンド: LED、リピートの時間
void testCommands() {
    printf("commands\n");
    x68k::Mfp mfp;
    setupMfp(mfp);
    static x68k::Keyboard kb(kHz);
    kb.reset();
    CHECK_EQ(kb.repeatDelayMs(), 500u);
    CHECK_EQ(kb.repeatIntervalMs(), 110u);
    mfp.write(23, 0x63);
    kb.update(0, mfp);
    CHECK_EQ(kb.repeatDelayMs(), 500u);
    mfp.write(23, 0x72);
    kb.update(0, mfp);
    CHECK_EQ(kb.repeatIntervalMs(), 50u);
    mfp.write(23, 0x6f);
    kb.update(0, mfp);
    CHECK_EQ(kb.repeatDelayMs(), 1700u);
    mfp.write(23, 0x7f);
    kb.update(0, mfp);
    CHECK_EQ(kb.repeatIntervalMs(), 1155u);
    mfp.write(23, 0xf6);
    kb.update(0, mfp);
    CHECK_EQ(kb.leds(), 0xf6);
    mfp.write(23, 0x41); // マウスの制御は無視
    kb.update(0, mfp);
    CHECK_EQ(kb.leds(), 0xf6);
    CHECK_EQ(kb.enabled(), true);
    // USB キーボードの LED: 0xf6 は かな (bit0) と CAPS (bit3) が点灯 → Scroll Lock と Caps Lock
    CHECK_EQ(x68k::Keyboard::hidLeds(0xf6), 0x06);
    CHECK_EQ(x68k::Keyboard::hidLeds(0xff), 0x00);
    CHECK_EQ(x68k::Keyboard::hidLeds(0xef), 0x01); // INS → Num Lock
    CHECK_EQ(x68k::Keyboard::hidLeds(0x80), 0x07); // ほかの LED は出さない
}

// キーリピート: 500ms 後から 110ms ごとに make を繰り返す。SHIFT はリピートしない
void testRepeat() {
    printf("repeat\n");
    x68k::Mfp mfp;
    setupMfp(mfp);
    static x68k::Keyboard kb(kHz);
    kb.reset();
    const uint64_t ms = kHz / 1000;
    kb.update(0, mfp);
    kb.onKey(0xe1, true); // SHIFT
    kb.onKey(0x04, true); // A
    int count[256] = {};
    uint64_t t = 0;
    // 1 秒押し続ける: 最初の make + (1000 - 500) / 110 + 1 = 5 回のリピート
    for (; t <= 1000 * ms; t += ms / 4) {
        kb.update(t, mfp);
        int v = readByte(mfp);
        if (v >= 0) count[v]++;
    }
    CHECK_EQ(count[0x70], 1);
    CHECK_EQ(count[0x1e], 6);
    kb.onKey(0x04, false);
    for (int i = 0; i < 2000; i++, t += ms / 4) {
        kb.update(t, mfp);
        int v = readByte(mfp);
        if (v >= 0) count[v]++;
    }
    CHECK_EQ(count[0x1e], 6); // 離したら止まる
    CHECK_EQ(count[0x9e], 1);
    CHECK_EQ(count[0xf0], 0);
}

} // namespace

int main() {
    testHidTable();
    testAscii();
    testMakeBreakAndPacing();
    testRxBuffer();
    testSharedCode();
    testOverflow();
    testCommands();
    testRepeat();
    if (failures) {
        printf("FAILED: %d\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
