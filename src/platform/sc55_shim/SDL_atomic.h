// Nuked-SC55 (external/nuked-sc55) を SDL なしでビルドするための SDL_atomic.h。
// Nuked-SC55 はパネルのボタンの状態 (mcu_button_pressed) にだけ使う。pix68k はボタンを押さないので、ただの値でよい
#pragma once
#include <stdint.h>

struct SDL_atomic_t {
    int value;
};

inline int SDL_AtomicGet(SDL_atomic_t* a) { return a->value; }
inline int SDL_AtomicSet(SDL_atomic_t* a, int v) {
    const int old = a->value;
    a->value = v;
    return old;
}
