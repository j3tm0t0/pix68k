// 文字を USB キーボードのキー (HID の usage) に直す (ベアメタル版のネットワークの口の /type。baremetal/host_net.cpp)。
// X68000 の JIS 配列での打ち方 (x68k::Keyboard::fromAscii()) と、USB のキーから X68000 のキーへの対応
// (x68k::Keyboard::fromHid()) を逆にたどって決めるので、2 つの表と食い違わない。
// USB キーボードと同じ経路 (CKernel::SendKeyReport()) で X68000 に送る。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stdint.h>

#include "x68k/keyboard.h"

namespace platform {

// c を打つ USB のキーを usage に入れ、SHIFT が要るかを shift に入れる。打てなければ false。
// 探す順: 主なキー (0x04-0x38)、JIS の ろ (0x87)、¥ (0x89)。テンキーより先に見つかる
inline bool asciiToUsage(char c, uint8_t& usage, bool& shift) {
    const uint8_t code = x68k::Keyboard::fromAscii(c, shift);
    if (code == 0) return false;
    static const uint8_t kRanges[][2] = {{0x04, 0x38}, {0x87, 0x87}, {0x89, 0x89}};
    for (const auto& r : kRanges)
        for (unsigned u = r[0]; u <= r[1]; u++)
            if (x68k::Keyboard::fromHid((uint8_t)u) == code) {
                usage = (uint8_t)u;
                return true;
            }
    return false;
}

// HID の修飾キーのビット (LShift)
constexpr uint8_t HID_MOD_LSHIFT = 0x02;

} // namespace platform
