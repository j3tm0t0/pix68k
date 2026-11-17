// USB キーボードのレポート (修飾キーの 8 ビットと、押しているキーの usage ID 6 つ) を、
// キーごとの「押した / 離した」に直す。
//
// 送った先 (キューなど) が満杯で送れなかったときは、送れたところまでを覚えておき、次の
// report() か resync() で残りを送る。送れなかった「離した」を落としたままにしないので、
// キーが押しっぱなしになることはない。離すほうを先に送り、修飾キーは押すときは先、離すときは後に送る。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stdint.h>
#include <string.h>

namespace platform {

class KeyTracker {
public:
    KeyTracker() { reset(); }
    void reset() {
        memset(want_, 0, sizeof want_);
        memset(sent_, 0, sizeof sent_);
    }

    // レポートを受け取り、変わったキーを emit(usage, down) で送る。emit は送れたら true を返す。
    // すべて送れたら true。1..3 (ErrorRollOver など) を含むレポートは、押しているキーが分からないので無視する
    template <typename Emit>
    bool report(uint8_t modifiers, const uint8_t keys[6], Emit&& emit) {
        for (int i = 0; i < 6; i++)
            if (keys[i] >= 1 && keys[i] <= 3) return resync(emit);
        memset(want_, 0, sizeof want_);
        for (int b = 0; b < 8; b++)
            if (modifiers & (1u << b)) set(want_, 0xe0 + b, true);
        for (int i = 0; i < 6; i++)
            if (keys[i] != 0) set(want_, keys[i], true);
        return resync(emit);
    }

    // 押しているキーを、修飾キーのビットと usage ごとの 1 ビット (32 バイト) で受け取る (何台かのキーボードを
    // 合わせたあと。platform/key_merge.h)。6 キーを超えてもよい。usage 1..3 のビットは無視する
    template <typename Emit>
    bool reportBits(uint8_t modifiers, const uint8_t down[32], Emit&& emit) {
        memcpy(want_, down, sizeof want_);
        want_[0] &= (uint8_t)~0x0eu; // usage 1..3 (ErrorRollOver など)
        for (int b = 0; b < 8; b++)
            if (modifiers & (1u << b)) set(want_, 0xe0 + b, true);
        return resync(emit);
    }

    // すべてのキーを離したことにする (キーボードが外れたとき)
    template <typename Emit>
    bool releaseAll(Emit&& emit) {
        memset(want_, 0, sizeof want_);
        return resync(emit);
    }

    // まだ送れていない変化を送る。すべて送れたら true
    template <typename Emit>
    bool resync(Emit&& emit) {
        // 離すときは普通のキーを先に、修飾キー (0xE0-0xE7) を後に送る。押すときは修飾キーを先に送る
        // (同じレポートの SHIFT + A が、SHIFT、A の順に届くように)
        return send(emit, false, false) && send(emit, false, true) && send(emit, true, true) &&
               send(emit, true, false);
    }

    // まだ送れていない変化があるか
    bool pending() const { return memcmp(want_, sent_, sizeof want_) != 0; }

private:
    static bool isModifier(int u) { return u >= 0xe0 && u <= 0xe7; }
    // down の向きの変化のうち、修飾キーか (modifiers)、それ以外のキーのものを送る
    template <typename Emit>
    bool send(Emit& emit, bool down, bool modifiers) {
        for (int u = 0; u < 256; u++) {
            if (isModifier(u) != modifiers) continue;
            bool w = get(want_, u), s = get(sent_, u);
            if (w == s || w != down) continue;
            if (!emit((uint8_t)u, down)) return false;
            set(sent_, u, down);
        }
        return true;
    }
    static bool get(const uint8_t* bits, int u) { return (bits[u >> 3] >> (u & 7)) & 1; }
    static void set(uint8_t* bits, int u, bool on) {
        if (on) bits[u >> 3] |= (uint8_t)(1u << (u & 7));
        else bits[u >> 3] &= (uint8_t)~(1u << (u & 7));
    }
    uint8_t want_[32]; // レポートで押されているキー
    uint8_t sent_[32]; // 押したと送ったキー
};

} // namespace platform
