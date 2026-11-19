// 文字を USB キーボードのキー (HID の usage) に直す (ベアメタル版のネットワークの口の /type。baremetal/host_net.cpp)。
// X68000 の JIS 配列での打ち方 (x68k::Keyboard::fromAscii()) と、USB のキーから X68000 のキーへの対応
// (x68k::Keyboard::fromHid()) を逆にたどって決めるので、2 つの表と食い違わない。
// USB キーボードと同じ経路 (CKernel::SendKeyReport()) で X68000 に送る。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

// 起動したあとに決まった文字列を打つ (ベアメタル版の autotype=。platform/boot_profile.h)。主ループから poll() を呼び、
// 返したレポートを送れたら ack() を呼ぶ (送れなければ、次の poll() で同じものを返す)。
// /type と同じく、押して PRESS_MS、離して PRESS_MS 空けて次を打つ。打てない字は飛ばす
class AutoTyper {
public:
    static constexpr unsigned MAX_TEXT = 256;
    static constexpr uint32_t PRESS_MS = 40;
    // text (/type と同じエスケープを戻したもの) を、startMs から waitMs たったら打ち始める
    void start(const char* text, size_t n, uint32_t startMs, uint32_t waitMs) {
        len_ = n < MAX_TEXT ? (unsigned)n : MAX_TEXT;
        memcpy(text_, text, len_);
        pos_ = 0;
        pressed_ = false;
        next_ = startMs + waitMs;
        typed_ = skipped_ = 0;
        active_ = len_ > 0;
    }
    bool active() const { return active_; }
    unsigned typed() const { return typed_; }
    unsigned skipped() const { return skipped_; }
    // 送るレポートがあれば true (mods と usage。usage が 0 なら離す)
    bool poll(uint32_t nowMs, uint8_t& mods, uint8_t& usage) {
        if (!active_ || (int32_t)(nowMs - next_) < 0) return false;
        if (pressed_) {
            mods = 0;
            usage = 0;
            return true;
        }
        while (pos_ < len_) {
            bool shift = false;
            if (asciiToUsage(text_[pos_], usage, shift)) {
                mods = shift ? HID_MOD_LSHIFT : 0;
                return true;
            }
            skipped_++;
            pos_++;
        }
        active_ = false;
        return false;
    }
    // poll() が返したレポートを送れた
    void ack(uint32_t nowMs) {
        if (pressed_) {
            pressed_ = false;
            pos_++;
            if (pos_ >= len_) active_ = false;
        } else {
            pressed_ = true;
            typed_++;
        }
        next_ = nowMs + PRESS_MS;
    }

private:
    char text_[MAX_TEXT] = {};
    unsigned len_ = 0, pos_ = 0, typed_ = 0, skipped_ = 0;
    uint32_t next_ = 0;
    bool pressed_ = false, active_ = false;
};

} // namespace platform
