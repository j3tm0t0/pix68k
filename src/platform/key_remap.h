// USB キーボードのレポートを、キーボードの種類に合わせて書き換える (KeyTracker に渡す前)。
//
// Pi 400 の本体のキーボード (英語版、日本語版) は、X68000 にしかないキー (かな、ローマ字、コード入力、記号入力、
// 登録、HELP、XF3〜5、OPT.1 / OPT.2 など) を置ける余りのキーがない。そこでラズベリーのキー (左 GUI) を
// 切り替えのキーにし、押している間はほかのキーを X68000 のキーに置き換える (下の kPi400Layer)。
// 置き換えた先は、x68k::Keyboard::fromHid() が X68000 のキーに対応させている USB の usage ID
// (F13〜F24、Help、Undo、Copy、LANG1 など) なので、X68000 の側は普通の USB キーボードと区別しない。
// Fn はキーボードの中で処理され、USB には出てこないので使えない。
//
// 置き換え先は、物理キーを押した時点の状態 (ラズベリーのキー、OPT.1 / OPT.2 を押しているか) で決め、離すまで変えない。
// ラズベリーのキーや O / P を先に離しても、押したままのキーの出力は変わらない (かななどのロックキーを、離す順で
// 意図せず押したことにしないため)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stdint.h>
#include <string.h>

namespace platform {

enum class KeyLayout : uint8_t {
    STANDARD, // 普通の USB キーボード (書き換えない)
    PI400_US, // Pi 400 の英語版
    PI400_JP, // Pi 400 の日本語版
};

// Pi 400 の本体のキーボード (Holtek のコントローラ) の VID:PID。英語版と日本語版で同じ
static constexpr uint16_t PI400_KEYBOARD_VID = 0x04d9;
static constexpr uint16_t PI400_KEYBOARD_PID = 0x0007;
// HID 記述子の bCountryCode で日本語配列を表す値 (HID 1.11 の 6.2.1。15 = Japan (Katakana))
static constexpr uint8_t HID_COUNTRY_JAPAN = 15;

inline const char* keyLayoutName(KeyLayout l) {
    switch (l) {
    case KeyLayout::PI400_US: return "pi400us";
    case KeyLayout::PI400_JP: return "pi400jp";
    default: return "usb";
    }
}

// pix68k.cfg の keyboard= の値を読む。auto なら detect を true にする。分からない値なら false
inline bool parseKeyLayout(const char* s, KeyLayout& layout, bool& detect) {
    detect = false;
    if (strcmp(s, "auto") == 0) {
        detect = true;
        layout = KeyLayout::STANDARD;
    } else if (strcmp(s, "usb") == 0) layout = KeyLayout::STANDARD;
    else if (strcmp(s, "pi400us") == 0 || strcmp(s, "pi400") == 0) layout = KeyLayout::PI400_US;
    else if (strcmp(s, "pi400jp") == 0) layout = KeyLayout::PI400_JP;
    else return false;
    return true;
}

// つないだキーボードの VID:PID と、HID 記述子の bCountryCode (なければ 0) から種類を決める。
// pi400 は動いている機種が Pi 400 か。04d9 は Holtek の VID でほかのキーボードにもあるので、Pi 400 の上でだけ
// Pi 400 のキーボードとみなす。Pi 400 の英語版と日本語版は VID:PID が同じなので、bCountryCode が日本 (15) のときだけ
// 日本語版とする (実機の日本語版 (c03130) は 0 を返し、英語版と判定された。そのときは pix68k.cfg の keyboard=pi400jp で選ぶ。#359)
inline KeyLayout detectKeyLayout(uint16_t vid, uint16_t pid, uint8_t country, bool pi400) {
    if (!pi400 || vid != PI400_KEYBOARD_VID || pid != PI400_KEYBOARD_PID) return KeyLayout::STANDARD;
    return country == HID_COUNTRY_JAPAN ? KeyLayout::PI400_JP : KeyLayout::PI400_US;
}

// Pi 400 で、ラズベリーのキーを押している間に置き換えるキー (USB の usage ID)。
// 置き換えた先と X68000 のキーの対応は x68k::Keyboard::fromHid() のとおり
struct KeyLayerEntry {
    uint8_t from, to;
};
static constexpr KeyLayerEntry kPi400Layer[] = {
    {0x1e, 0x6f}, // 1 → F20 (XF1)
    {0x1f, 0x70}, // 2 → F21 (XF2)
    {0x20, 0x71}, // 3 → F22 (XF3)
    {0x21, 0x72}, // 4 → F23 (XF4)
    {0x22, 0x73}, // 5 → F24 (XF5)
    {0x0e, 0x6b}, // K → F16 (かな)
    {0x15, 0x6c}, // R → F17 (ローマ字)
    {0x06, 0x6d}, // C → F18 (コード入力)
    {0x1d, 0x6e}, // Z → F19 (全角)
    {0x0a, 0x90}, // G → LANG1 (ひらがな)
    {0x16, 0x68}, // S → F13 (記号入力)
    {0x17, 0x69}, // T → F14 (登録)
    {0x0b, 0x6a}, // H → F15 (HELP)
    {0x18, 0x7a}, // U → Undo (UNDO)
    {0x1c, 0x7c}, // Y → Copy (COPY)
    {0x05, 0x48}, // B → Pause (BREAK)
    {0x0f, 0x53}, // L → Num Lock (CLR)
    {0x12, 0xe3}, // O → 左 GUI (OPT.1)
    {0x13, 0xe7}, // P → 右 GUI (OPT.2)
    {0x52, 0x4b}, // ↑ → Page Up (ROLL DOWN)
    {0x51, 0x4e}, // ↓ → Page Down (ROLL UP)
    {0x50, 0x4a}, // ← → Home (HOME)
    {0x4f, 0x4d}, // → → End (UNDO)
    {0x4c, 0x49}, // Delete → Insert (INS)
    {0x38, 0x87}, // / → International1 (_。JIS の「ろ」のキーで、英語版にはない)
    {0x10, 0x76}, // M → Menu (X68000 のキーではない。pix68k の設定メニューを開く。platform/menu.h)
};
static constexpr uint8_t PI400_LAYER_MODIFIER = 0x08; // 左 GUI (ラズベリーのキー) の修飾キーのビット

class KeyRemap {
public:
    void setLayout(KeyLayout l) {
        layout_ = l;
        reset();
    }
    // 押しているキーと Num Lock の状態を忘れる (キーボードが外れたとき)
    void reset() {
        memset(pressed_, 0, sizeof pressed_);
        memset(out_, 0, sizeof out_);
        numLock_ = false;
    }
    KeyLayout layout() const { return layout_; }

    // レポート (修飾キーのビットと usage ID 6 つ) を書き換える。in と out は同じでもよい
    void apply(uint8_t inMods, const uint8_t inKeys[6], uint8_t& outMods, uint8_t outKeys[6]) {
        uint8_t keys[6];
        memcpy(keys, inKeys, 6);
        outMods = inMods;
        if (layout_ == KeyLayout::STANDARD) {
            memcpy(outKeys, keys, 6);
            return;
        }
        // ErrorRollOver などはそのまま渡す (KeyTracker が無視する。押しているキーの状態も変えない)
        for (int i = 0; i < 6; i++) {
            if (keys[i] >= 1 && keys[i] <= 3) {
                memcpy(outKeys, keys, 6);
                return;
            }
        }
        // 離したキーを忘れる
        uint8_t down[256 / 8] = {};
        for (int i = 0; i < 6; i++)
            if (keys[i] != 0) down[keys[i] >> 3] |= (uint8_t)(1u << (keys[i] & 7));
        for (int u = 0; u < 256; u++)
            if (isPressed(u) && !((down[u >> 3] >> (u & 7)) & 1)) setPressed(u, false);
        const bool layer = (inMods & PI400_LAYER_MODIFIER) != 0;
        // 新しく押したキーの置き換え先を決める。O / P を先に決める (同じレポートのラズベリー + O + C を OPT.1 + C にする)
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < 6; i++) {
                const uint8_t u = keys[i];
                if (u == 0 || isPressed(u)) continue;
                const bool optKey = u == 0x12 || u == 0x13;
                if ((pass == 0) != optKey) continue;
                setPressed(u, true);
                out_[u] = decide(u, layer);
            }
        }
        // 出力: 左 GUI (ラズベリーのキー) は送らない。ほかの修飾キーはそのまま
        uint8_t mods = (uint8_t)(inMods & ~PI400_LAYER_MODIFIER);
        int n = 0;
        for (int i = 0; i < 6; i++) outKeys[i] = 0;
        for (int i = 0; i < 6; i++) {
            const uint8_t u = keys[i];
            if (u == 0) continue;
            const uint8_t o = out_[u];
            if (o == 0) continue;
            if (o >= 0xe0 && o <= 0xe7) mods |= (uint8_t)(1u << (o - 0xe0));
            else outKeys[n++] = o;
        }
        outMods = mods;
    }

    // X68000 の LED から作った USB の LED (x68k::Keyboard::hidLeds() の値) を、このキーボード向けに直す。
    // Pi 400 では Num Lock の LED を点けるとキーボードの一部がテンキーになるので、X68000 の INS では点けず、
    // Num Lock のキーで切り替えた状態を出す
    uint8_t hidLeds(uint8_t leds) const {
        if (layout_ == KeyLayout::STANDARD) return leds;
        return (uint8_t)((leds & ~0x01u) | (numLock_ ? 0x01u : 0u));
    }
    bool numLock() const { return numLock_; }

private:
    // 新しく押したキー u の置き換え先 (0 なら送らない)
    uint8_t decide(uint8_t u, bool layer) {
        if (u == 0x53) {
            // Num Lock は X68000 に送らず、USB キーボードの Num Lock (の LED) を切り替える。
            // Pi のキーボードは Num Lock が点いている間、文字のキーの一部がテンキーになる
            numLock_ = !numLock_;
            return 0;
        }
        // 日本語版の 半角/全角 → 全角 (F19)。¥ は専用のキー (International3) がある。ラズベリーを押していても同じ
        if (layout_ == KeyLayout::PI400_JP && u == 0x35) return 0x6e;
        if (!layer) return u;
        if (u == 0x12) return 0xe3; // O → OPT.1
        if (u == 0x13) return 0xe7; // P → OPT.2
        // OPT.1 / OPT.2 を押している間は、ほかのキーを置き換えない
        // (X68000 の OPT.1 / OPT.2 は修飾キーとして使うので、OPT.1 + C などを打てるように)
        if (optHeld()) return u;
        for (const KeyLayerEntry& e : kPi400Layer)
            if (e.from == u) return e.to;
        return u;
    }
    bool optHeld() const {
        return (isPressed(0x12) && out_[0x12] == 0xe3) || (isPressed(0x13) && out_[0x13] == 0xe7);
    }
    bool isPressed(int u) const { return (pressed_[u >> 3] >> (u & 7)) & 1; }
    void setPressed(int u, bool on) {
        if (on) pressed_[u >> 3] |= (uint8_t)(1u << (u & 7));
        else {
            pressed_[u >> 3] &= (uint8_t)~(1u << (u & 7));
            out_[u] = 0;
        }
    }

    KeyLayout layout_ = KeyLayout::STANDARD;
    uint8_t pressed_[32] = {}; // 押している物理キー (usage ごとに 1 ビット)
    uint8_t out_[256] = {};    // 押している物理キーの置き換え先 (押した時点で決める。0 は送らない)
    bool numLock_ = false;     // Pi 400: Num Lock のキーで切り替えた状態
};

} // namespace platform
