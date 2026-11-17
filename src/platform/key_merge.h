// 何台かの USB キーボードのレポートを合わせて、1 台分の押しているキーにする (ベアメタル版)。
//
// キーボードごとに、最後のレポート (KeyRemap で書き換えたあと) の修飾キーと押しているキーを持ち、
// すべてのキーボードの OR を返す。同じキーを 2 台で押したときは、片方を離しても押したまま。
// 合わせたあとは 6 キーに収まらないことがあるので、押しているキーは usage ごとの 1 ビット (32 バイト) で返す
// (KeyTracker::reportBits()、MenuKeyFilter::filterBits() に渡す)。
// ErrorRollOver など (usage 1..3) を含むレポートは、そのキーボードの押しているキーが分からないので無視し、
// そのキーボードの前の状態を残す (KeyTracker::report() と同じ扱い)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stdint.h>
#include <string.h>

namespace platform {

template <int N>
class KeyMerger {
public:
    KeyMerger() {
        for (int s = 0; s < N; s++) clear(s);
    }
    // キーボード slot のレポート。ErrorRollOver などを含むレポートなら何もせず false
    bool set(int slot, uint8_t mods, const uint8_t keys[6]) {
        if (slot < 0 || slot >= N) return false;
        for (int i = 0; i < 6; i++)
            if (keys[i] >= 1 && keys[i] <= 3) return false;
        Slot& s = slots_[slot];
        s.mods = mods;
        memset(s.down, 0, sizeof s.down);
        for (int i = 0; i < 6; i++) {
            const uint8_t u = keys[i];
            if (u == 0) continue;
            // 修飾キーの usage がキーの欄に来たら、修飾キーのビットにする
            if (u >= 0xe0 && u <= 0xe7) s.mods |= (uint8_t)(1u << (u - 0xe0));
            else s.down[u >> 3] |= (uint8_t)(1u << (u & 7));
        }
        return true;
    }
    // キーボード slot を外した (押していたキーを離したことにする)
    void clear(int slot) {
        if (slot < 0 || slot >= N) return;
        slots_[slot].mods = 0;
        memset(slots_[slot].down, 0, sizeof slots_[slot].down);
    }
    // すべてのキーボードを合わせた修飾キーと、押しているキー (修飾キーを除く usage ごとの 1 ビット)
    void merged(uint8_t& mods, uint8_t down[32]) const {
        mods = 0;
        memset(down, 0, 32);
        for (int s = 0; s < N; s++) {
            mods |= slots_[s].mods;
            for (int b = 0; b < 32; b++) down[b] |= slots_[s].down[b];
        }
    }

private:
    struct Slot {
        uint8_t mods;
        uint8_t down[32];
    };
    Slot slots_[N];
};

} // namespace platform
