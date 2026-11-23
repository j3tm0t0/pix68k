// USB キーボードの Ctrl + Alt + Delete を見つけ、X68000 には送らないようにする (ベアメタル版で Raspberry Pi を再起動する)。
//
// 見るのは、キーボードの種類に合わせて書き換え (KeyRemap)、すべてのキーボードを合わせた (KeyMerger) あとの状態。
// 再起動にするのは、Delete (0x4C) を新しく押したときに、次のすべてを満たすときだけ:
//   - USB の修飾キーの Ctrl (左右どちらでも、0xE0 / 0xE4) を押している
//   - USB の修飾キーの Alt (左右どちらでも、0xE2 / 0xE6) を押している
//   - GUI (0xE3 / 0xE7、X68000 の OPT.1 / OPT.2) を押していない。X68000 のリセット (ホットリスタート、CTRL + OPT.1 + DEL) は
//     Ctrl + GUI + Delete で打ち、X68000 に送るため。Ctrl + Alt + GUI + Delete も X68000 に送る。
//     Pi 400 では Ctrl + ラズベリー + O + Delete が CTRL + OPT.1 + DEL になる (KeyRemap が O を OPT.1 にし、OPT.1 を押している間は
//     Delete を INS に置き換えない)
//   - Alt のほかに X68000 の XF1 になるキー (F11 0x44、F20 0x6F) を押していない。Ctrl + F11 + Delete と Ctrl + Alt + F11 + Delete は、
//     Pi の再起動にせず X68000 に CTRL + XF1 + DEL としてそのまま送る (X68000 のリセットにはならない。実機の Human68k では
//     CTRL + XF1 で ASK68K が動く)
// Delete を先に押してから Ctrl と Alt を押したときは再起動しない (Delete は X68000 に送ってある)。
// Ctrl、Alt、Delete は別のキーボードで押してもよい (合わせたあとの状態で見る)。
// Pi 400 のラズベリー + Delete は KeyRemap で Insert になるので、Ctrl + Alt + ラズベリー + Delete は再起動しない。
//
// 見つけたときに押していたキー (Ctrl、Alt、Delete など) は、それぞれ一度離すまで X68000 にも設定メニューにも渡さない
// (X68000 には、押していた Ctrl と Alt を離したと送ることになる。Delete は送らない)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stdint.h>
#include <string.h>

namespace platform {

class RebootKeyFilter {
public:
    static constexpr uint8_t DELETE_KEY = 0x4c;
    static constexpr uint8_t CTRL_BITS = 0x11, ALT_BITS = 0x44, GUI_BITS = 0x88; // 修飾キーのビット (左右)
    static constexpr uint8_t F11 = 0x44, F20 = 0x6f; // X68000 の XF1 になる、修飾キーでないキー

    // Delete を押した状態 (mods、down) が再起動の組み合わせか (Delete を新しく押したかは見ない)
    static bool isCombo(uint8_t mods, const uint8_t down[32]) {
        return (mods & CTRL_BITS) != 0 && (mods & ALT_BITS) != 0 && (mods & GUI_BITS) == 0 && get(down, DELETE_KEY) &&
               !get(down, F11) && !get(down, F20);
    }

    // 合わせたあとのレポート (修飾キーのビットと、修飾キーを除く usage ごとの 1 ビット)。
    // 再起動の組み合わせを見つけたら true。mods と down からは、見つけたときに押していて、まだ離していないキーを消す
    bool filter(uint8_t& mods, uint8_t down[32]) {
        const bool del = get(down, DELETE_KEY);
        const bool fire = del && !deleteHeld_ && isCombo(mods, down);
        deleteHeld_ = del;
        // 離したキーは消すのをやめる
        for (int b = 0; b < 32; b++) masked_[b] &= down[b];
        maskedMods_ &= mods;
        if (fire) {
            memcpy(masked_, down, sizeof masked_);
            maskedMods_ = mods;
        }
        mods = (uint8_t)(mods & ~maskedMods_);
        for (int b = 0; b < 32; b++) down[b] = (uint8_t)(down[b] & ~masked_[b]);
        return fire;
    }
    void reset() {
        deleteHeld_ = false;
        memset(masked_, 0, sizeof masked_);
        maskedMods_ = 0;
    }

private:
    static bool get(const uint8_t* bits, int u) { return (bits[u >> 3] >> (u & 7)) & 1; }
    bool deleteHeld_ = false; // 前のレポートで Delete を押していた
    uint8_t masked_[32] = {}; // 見つけたときに押していて、まだ離していないキー
    uint8_t maskedMods_ = 0;
};

// Ctrl + Alt + Delete を押したことの受け渡しと、書けずに断ったあとの 2 回目の扱い。
// USB の割り込みが press() を呼び、コア 0 の主ループが take() で受け取って、X68000 を止めてディスクと SRAM を書く。
// 書けなければ refused() を呼ぶ:
//   - 前に断ってから window 以内 (ticks) なら true を返す。主ループは書かずに再起動する (メニューの Restart を 2 回選んだときと同じ)
//   - そうでなければ断った時刻を覚え、それまでに来た press() を捨てて false を返す。書いている間 (コア 1 が止まるまでの
//     最大 5 秒や、書き戻しの失敗まで) に押し直したものを、断ったあとの 2 回目と取り違えないため
// MenuDiscardConfirm はメニューでほかの操作をすると「1 回断った」状態を消すが、キーボードにはそれに当たる操作がないので、
// 時間 (window) で消す
class RebootRequest {
public:
    explicit RebootRequest(uint64_t windowTicks) : window_(windowTicks) {}
    // Ctrl + Alt + Delete を押した (USB の割り込み)
    void press() { __atomic_store_n(&pressed_, true, __ATOMIC_RELEASE); }
    // 押されていたら true を返し、受け取ったことにする (コア 0 の主ループ)
    bool take() { return __atomic_exchange_n(&pressed_, false, __ATOMIC_ACQ_REL); }
    bool pressed() const { return __atomic_load_n(&pressed_, __ATOMIC_ACQUIRE); }
    // 書けなかった。true なら書かずに再起動してよい。false なら断った (now を覚え、それまでの押下を捨てた)
    bool refused(uint64_t now) {
        if (armed_ && now - armedAt_ <= window_) return true;
        armed_ = true;
        armedAt_ = now;
        __atomic_store_n(&pressed_, false, __ATOMIC_RELEASE);
        return false;
    }

private:
    uint64_t window_;
    bool pressed_ = false;
    bool armed_ = false;    // 書けずに断ったことがある
    uint64_t armedAt_ = 0;  // 断った時刻
};

} // namespace platform
