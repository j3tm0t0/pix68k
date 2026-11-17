#include "x68k/ppi.h"

namespace x68k {

void Ppi::reset() {
    portC_ = 0;
    control_ = 0x9b;
}

uint8_t Ppi::portValue(uint8_t bits) {
    uint8_t v = 0xff;
    v &= (uint8_t)~(bits & 0x0f);           // 上下左右 → bit0-3
    if (bits & JOY_A) v &= (uint8_t)~0x20;  // トリガ A → bit5
    if (bits & JOY_B) v &= (uint8_t)~0x40;  // トリガ B → bit6
    return v;
}

void Ppi::setJoystick(int port, uint8_t bits) {
    if (port < 0 || port > 1) return;
    // 上と下、左と右を同時に押すことはできない (ゲームが混乱する) ので、両方なら両方離したことにする
    if ((bits & (JOY_UP | JOY_DOWN)) == (JOY_UP | JOY_DOWN)) bits &= (uint8_t)~(JOY_UP | JOY_DOWN);
    if ((bits & (JOY_LEFT | JOY_RIGHT)) == (JOY_LEFT | JOY_RIGHT)) bits &= (uint8_t)~(JOY_LEFT | JOY_RIGHT);
    joy_[port] = bits & 0x3f;
}

uint8_t Ppi::read(uint32_t offset) {
    switch (offset & 7) {
    case 1: {
        uint8_t v = portValue(joy_[0]);
        // ポート C の bit6 / bit7 でジョイスティック 1 のトリガ A / B の線を L にできる
        v &= (uint8_t)~((portC_ >> 1) & 0x60);
        return v;
    }
    case 3: return portValue(joy_[1]);
    case 5: return portC_;
    default: return 0xff;
    }
}

void Ppi::write(uint32_t offset, uint8_t v) {
    switch (offset & 7) {
    case 5:
        portC_ = v;
        break;
    case 7:
        if (v & 0x80) {
            // モードの設定: 出力のラッチは 0 になる
            control_ = v;
            portC_ = 0;
        } else {
            int b = (v >> 1) & 7;
            if (v & 1) portC_ |= (uint8_t)(1u << b);
            else portC_ &= (uint8_t)~(1u << b);
        }
        break;
    default: // ポート A / B は入力なので、書いても何も起きない
        break;
    }
}

} // namespace x68k
