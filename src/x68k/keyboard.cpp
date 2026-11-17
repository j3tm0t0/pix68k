#include "x68k/keyboard.h"

#include <string.h>

#include "x68k/mfp.h"

namespace x68k {

namespace {

// USB HID の usage ID (キーボード/キーパッドのページ) → X68000 のキーコード。0 は対応なし。
// X68000 側は JIS 配列とし、ホストのキーは物理的な位置で対応させる (US 配列のキーボードでも、
// JIS 配列で同じ位置にあるキーになる)。X68000 のキーコードの根拠は keyboard.h の Code の注釈、
// HID の usage ID は SDL2 の SDL_scancode.h で確かめた。
const uint8_t kHidToX68k[256] = {
    // 0x00-0x03: なし
    0x00, 0x00, 0x00, 0x00,
    // 0x04-0x1D: A-Z
    0x1e, 0x2e, 0x2c, 0x20, 0x13, 0x21, 0x22, 0x23, 0x18, 0x24, 0x25, 0x26, 0x30, // A B C D E F G H I J K L M
    0x2f, 0x19, 0x1a, 0x11, 0x14, 0x1f, 0x15, 0x17, 0x2d, 0x12, 0x2b, 0x16, 0x2a, // N O P Q R S T U V W X Y Z
    // 0x1E-0x27: 1-9, 0
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
    // 0x28 Enter, 0x29 Esc, 0x2A BS, 0x2B Tab, 0x2C Space
    0x1d, 0x01, 0x0f, 0x10, 0x35,
    // 0x2D - (JIS -)、0x2E = (JIS ^)、0x2F [ (JIS @)、0x30 ] (JIS [)、0x31 \ (JIS の ] の位置)、
    // 0x32 Non-US # (JIS ])、0x33 ; 、0x34 ' (JIS :)、0x35 ` (JIS 半角/全角。X68000 の ¥ にする)
    0x0c, 0x0d, 0x1b, 0x1c, 0x29, 0x29, 0x27, 0x28, 0x0e,
    // 0x36 , 0x37 . 0x38 /
    0x31, 0x32, 0x33,
    // 0x39 CapsLock → CAPS
    0x5d,
    // 0x3A-0x43: F1-F10
    0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c,
    // 0x44 F11 → XF1、0x45 F12 → XF2
    0x55, 0x56,
    // 0x46 PrintScreen → COPY、0x47 ScrollLock → なし、0x48 Pause → BREAK
    0x62, 0x00, 0x61,
    // 0x49 Insert → INS、0x4A Home → HOME、0x4B PageUp → ROLL DOWN、0x4C Delete → DEL、
    // 0x4D End → UNDO、0x4E PageDown → ROLL UP (ROLL UP は画面を上に送る = 次のページ)
    0x5e, 0x36, 0x39, 0x37, 0x3a, 0x38,
    // 0x4F → 0x50 ← 0x51 ↓ 0x52 ↑
    0x3d, 0x3b, 0x3e, 0x3c,
    // 0x53 NumLock → テンキーの CLR、0x54 / 0x55 * 0x56 - 0x57 + 0x58 Enter
    0x3f, 0x40, 0x41, 0x42, 0x46, 0x4e,
    // 0x59-0x61: テンキーの 1-9
    0x4b, 0x4c, 0x4d, 0x47, 0x48, 0x49, 0x43, 0x44, 0x45,
    // 0x62 テンキーの 0、0x63 テンキーの .
    0x4f, 0x51,
    // 0x64 Non-US \ (ISO 配列の左 SHIFT の右) → _、0x65 Application → なし、0x66 Power → なし、
    // 0x67 テンキーの = → テンキーの =
    0x34, 0x00, 0x00, 0x4a,
    // 0x68 F13 → 記号入力、0x69 F14 → 登録、0x6A F15 → HELP、0x6B F16 → かな、0x6C F17 → ローマ字、
    // 0x6D F18 → コード入力、0x6E F19 → 全角、0x6F-0x73 F20-F24 → XF1-XF5
    // (F16 から F24 は、Pi 400 のように X68000 にしかないキーを持たないキーボードで使う。platform/key_remap.h)
    0x52, 0x53, 0x54, 0x5a, 0x5b, 0x5c, 0x60, 0x55, 0x56, 0x57, 0x58, 0x59,
    // 0x74 Execute、0x75 Help → HELP、0x76 Menu、0x77 Select、0x78 Stop → BREAK、0x79 Again、
    // 0x7A Undo → UNDO、0x7B Cut、0x7C Copy → COPY、0x7D-0x84 なし
    0x00, 0x54, 0x00, 0x00, 0x61, 0x00, 0x3a, 0x00, 0x62, 0, 0, 0, 0, 0, 0, 0, 0,
    // 0x85 テンキーの , 、0x86 なし
    0x50, 0x00,
    // 0x87 International1 (JIS の \ _ ろ) → _、0x88 International2 (カタカナひらがな) → XF4、
    // 0x89 International3 (JIS の ¥) → ¥、0x8A International4 (変換) → XF3、
    // 0x8B International5 (無変換) → XF2、0x8C-0x8F なし
    0x34, 0x58, 0x0e, 0x57, 0x56, 0, 0, 0, 0,
    // 0x90 LANG1 (Mac の かな) → ひらがな、0x91 LANG2 (Mac の 英数) → なし
    0x5f, 0x00,
    // 0x92-0xDF: なし (ここから下は初期化子の残りが 0 になるのを使い、修飾キーだけ下で入れる)
};

// 修飾キー 0xE0-0xE7: LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI
// Alt は XF1 / XF5 (JIS の Alt、無変換、空白、変換、カタカナ が X68000 の XF1、XF2、空白、XF3、XF4 の並び)、
// GUI (Mac の Command) は OPT.1 / OPT.2
const uint8_t kModifiers[8] = {0x71, 0x70, 0x55, 0x72, 0x71, 0x70, 0x59, 0x73};

// ASCII (0x20-0x7E) → キーコード。bit7 = SHIFT が要る。0 は打てない
const uint8_t kAscii[95] = {
    0x35,        // ' '
    0x82,        // !
    0x83,        // "
    0x84,        // #
    0x85,        // $
    0x86,        // %
    0x87,        // &
    0x88,        // '
    0x89,        // (
    0x8a,        // )
    0xa8,        // *
    0xa7,        // +
    0x31,        // ,
    0x0c,        // -
    0x32,        // .
    0x33,        // /
    0x0b, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, // 0-9
    0x28,        // :
    0x27,        // ;
    0xb1,        // <
    0x8c,        // =
    0xb2,        // >
    0xb3,        // ?
    0x1b,        // @
    // A-Z (SHIFT)
    0x9e, 0xae, 0xac, 0xa0, 0x93, 0xa1, 0xa2, 0xa3, 0x98, 0xa4, 0xa5, 0xa6, 0xb0,
    0xaf, 0x99, 0x9a, 0x91, 0x94, 0x9f, 0x95, 0x97, 0xad, 0x92, 0xab, 0x96, 0xaa,
    0x1c,        // [
    0x0e,        // \ (X68000 では ¥)
    0x29,        // ]
    0x0d,        // ^
    0xb4,        // _ (SHIFT が要る。SHIFT なしでは何も出ない: Human68k で確かめた)
    0x9b,        // `
    // a-z
    0x1e, 0x2e, 0x2c, 0x20, 0x13, 0x21, 0x22, 0x23, 0x18, 0x24, 0x25, 0x26, 0x30,
    0x2f, 0x19, 0x1a, 0x11, 0x14, 0x1f, 0x15, 0x17, 0x2d, 0x12, 0x2b, 0x16, 0x2a,
    0x9c,        // {
    0x8e,        // |
    0xa9,        // }
    0x8d,        // ~
};

} // namespace

void Keyboard::setClock(uint32_t cpuHz) {
    cpuHz_ = cpuHz ? cpuHz : 1;
    // 2400bps、1 バイト 10 ビット
    byteCycles_ = cpuHz_ / 240;
    if (byteCycles_ == 0) byteCycles_ = 1;
}

void Keyboard::reset() {
    wp_ = rp_ = 0;
    memset(queue_, 0, sizeof queue_);
    memset(hidDown_, 0, sizeof hidDown_);
    memset(count_, 0, sizeof count_);
    memset(pendingBreak_, 0, sizeof pendingBreak_);
    anyPending_ = false;
    enabled_ = true;
    leds_ = 0xff;
    delayMs_ = 500;
    intervalMs_ = 110;
    repeatCode_ = 0;
    nextRepeat_ = 0;
    nextSend_ = 0;
    lastNow_ = 0;
}

uint8_t Keyboard::fromHid(uint8_t usage) {
    if (usage >= 0xe0 && usage <= 0xe7) return kModifiers[usage - 0xe0];
    return kHidToX68k[usage];
}

uint8_t Keyboard::fromAscii(char c, bool& shift) {
    shift = false;
    switch (c) {
    case '\n':
    case '\r': return K_RETURN;
    case '\t': return K_TAB;
    case '\b': return K_BS;
    case 0x1b: return K_ESC;
    default: break;
    }
    if (c < 0x20 || c > 0x7e) return 0;
    uint8_t v = kAscii[c - 0x20];
    shift = (v & 0x80) != 0;
    return (uint8_t)(v & 0x7f);
}

bool Keyboard::typeAscii(char c) {
    bool shift;
    uint8_t code = fromAscii(c, shift);
    if (!code) return false;
    if (shift) press(K_SHIFT, true);
    press(code, true);
    press(code, false);
    if (shift) press(K_SHIFT, false);
    return true;
}

void Keyboard::onKey(uint8_t usage, bool down) {
    uint8_t& bits = hidDown_[usage >> 3];
    uint8_t m = (uint8_t)(1u << (usage & 7));
    // ホストの OS のキーリピート (押したままの down の繰り返し) は無視する。リピートはこちらでする
    if (down == ((bits & m) != 0)) return;
    uint8_t code = fromHid(usage);
    if (!code) return;
    if (down) {
        // make がキューに入らなかったら押していないことにする (break だけが届かないように)
        uint8_t before = count_[code];
        press(code, true);
        if (count_[code] == before) return;
        bits |= m;
    } else {
        bits &= (uint8_t)~m;
        press(code, false);
    }
}

bool Keyboard::push(uint8_t v) {
    if (freeSlots() <= 0) return false;
    queue_[wp_] = v;
    wp_ = (wp_ + 1) & (QUEUE_SIZE - 1);
    return true;
}

bool Keyboard::repeatable(uint8_t code) const {
    // 修飾キー (SHIFT、CTRL、OPT.1、OPT.2) と、状態を切り替えるキー (かな〜全角) はリピートしない
    if (code >= 0x70) return false;
    if (code >= 0x5a && code <= 0x60) return false;
    return true;
}

void Keyboard::press(uint8_t code, bool down) {
    code &= 0x7f;
    if (!code) return;
    if (down) {
        if (count_[code] == 0) {
            // break を送るための空きを残し、満ちていれば押さなかったことにする
            if (freeSlots() <= 4 || !push(code)) return;
            pendingBreak_[code >> 3] &= (uint8_t)~(1u << (code & 7));
            if (repeatable(code)) {
                repeatCode_ = code;
                nextRepeat_ = lastNow_ + msToCycles(delayMs_);
            }
        }
        if (count_[code] < 255) count_[code]++;
    } else {
        if (count_[code] == 0) return;
        if (--count_[code] != 0) return;
        if (code == repeatCode_) repeatCode_ = 0;
        // キューが満ちていても break は落とさない (あとで送る)
        if (!push((uint8_t)(code | 0x80))) {
            pendingBreak_[code >> 3] |= (uint8_t)(1u << (code & 7));
            anyPending_ = true;
        }
    }
}

uint8_t Keyboard::hidLeds(uint8_t x68kLeds) {
    uint8_t on = (uint8_t)~x68kLeds;
    uint8_t v = 0;
    if (on & LED_INS) v |= 0x01;  // Num Lock
    if (on & LED_CAPS) v |= 0x02; // Caps Lock
    if (on & LED_KANA) v |= 0x04; // Scroll Lock
    return v;
}

void Keyboard::command(uint8_t v) {
    if (v & 0x80) {
        leds_ = v;
    } else if ((v & 0xf8) == 0x48) {
        enabled_ = (v & 1) != 0;
    } else if ((v & 0xf0) == 0x60) {
        delayMs_ = 200 + 100u * (v & 0x0f);
    } else if ((v & 0xf0) == 0x70) {
        uint32_t n = v & 0x0f;
        intervalMs_ = 30 + 5 * n * n;
    }
    // $40-$47 (マウスの制御)、$50-$5F (表示の制御など)、$00-$3F (テレビの制御) は何もしない
}

void Keyboard::flushPendingBreaks() {
    for (int i = 0; i < 16; i++) {
        for (int b = 0; b < 8; b++) {
            if (!(pendingBreak_[i] & (1u << b))) continue;
            if (!push((uint8_t)(0x80 | (i * 8 + b)))) return; // まだ残っている
            pendingBreak_[i] &= (uint8_t)~(1u << b);
        }
    }
    anyPending_ = false;
}

void Keyboard::updateSlow(uint64_t now, Mfp& mfp) {
    // 本体からのコマンド。MFP の送信データは 1 バイト分しかないので、書かれるたびに (同期のたびに) 取る
    for (int t = mfp.takeTransmit(); t >= 0; t = mfp.takeTransmit()) command((uint8_t)t);

    // 送れなかった break を空いた分だけ入れる
    if (anyPending_) flushPendingBreaks();

    // キーリピート: 送るものがないときだけ足す (キューにリピートを溜めない)
    if (repeatCode_ && now >= nextRepeat_) {
        if (queued() == 0) push(repeatCode_);
        nextRepeat_ = now + msToCycles(intervalMs_);
    }

    if (now < nextSend_ || queued() == 0 || !enabled_) return;
    // 受信が許可されていなければ、本物と同じくそのバイトは失われる
    if (!mfp.rxEnabled()) {
        rp_ = (rp_ + 1) & (QUEUE_SIZE - 1);
        nextSend_ = now + byteCycles_;
        return;
    }
    // 本体がまだ前のバイトを読んでいなければ待つ。本物のキーボードは待たずに送り、MFP ではオーバーランに
    // なるが、ここでは割り込みを止めている間 (ディスクの読み書きなど) のキーを落とさないよう少し待つ。
    // ただし受信の割り込みを禁止したまま読まないソフトで止まり続けないよう、一定の時間 (OVERRUN_MS) が
    // たったら上書きする
    if (mfp.rxFull() && now < nextSend_ + msToCycles(OVERRUN_MS)) return;
    mfp.receive(queue_[rp_]);
    rp_ = (rp_ + 1) & (QUEUE_SIZE - 1);
    nextSend_ = now + byteCycles_;
}

} // namespace x68k
