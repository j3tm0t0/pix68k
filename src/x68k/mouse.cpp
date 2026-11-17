#include "x68k/mouse.h"

namespace x68k {

void Mouse::setClock(uint32_t cpuHz) {
    // 4800bps、1 バイト 11 ビット
    byteCycles_ = (uint64_t)(cpuHz ? cpuHz : 1) * 11 / 4800;
    if (byteCycles_ == 0) byteCycles_ = 1;
}

void Mouse::reset() {
    dx_ = dy_ = 0;
    buttons_ = latched_ = 0;
    sent_ = 3;
    nextByte_ = 0;
    lastNow_ = 0;
    packets_ = 0;
}

void Mouse::onMouse(int dx, int dy, uint8_t buttons) {
    // 溜める量は、あふれない範囲に抑える (送るときは ±127 に切り詰める)
    // 足す前にも切り詰める (int の範囲を超えて足さないように)
    const int32_t lim = 1 << 20;
    dx = dx > lim ? lim : dx < -lim ? -lim : dx;
    dy = dy > lim ? lim : dy < -lim ? -lim : dy;
    dx_ += dx;
    dy_ += dy;
    if (dx_ > lim) dx_ = lim;
    if (dx_ < -lim) dx_ = -lim;
    if (dy_ > lim) dy_ = lim;
    if (dy_ < -lim) dy_ = -lim;
    uint8_t b = (uint8_t)(((buttons & BTN_LEFT) ? ST_LEFT : 0) | ((buttons & BTN_RIGHT) ? ST_RIGHT : 0));
    latched_ |= (uint8_t)(b & ~buttons_);
    buttons_ = b;
}

void Mouse::makePacket(int dx, int dy, uint8_t buttons, uint8_t out[3]) {
    uint8_t st = (uint8_t)(buttons & (ST_LEFT | ST_RIGHT));
    if (dx > 127) {
        st |= ST_X_OVER;
        dx = 127;
    } else if (dx < -128) {
        st |= ST_X_UNDER;
        dx = -128;
    }
    if (dy > 127) {
        st |= ST_Y_OVER;
        dy = 127;
    } else if (dy < -128) {
        st |= ST_Y_UNDER;
        dy = -128;
    }
    out[0] = st;
    out[1] = (uint8_t)(int8_t)dx;
    out[2] = (uint8_t)(int8_t)dy;
}

void Mouse::onRts(int ch, bool asserted) {
    if (ch != Scc::CH_B || !asserted || busy()) return;
    makePacket(dx_, dy_, (uint8_t)(buttons_ | latched_), packet_);
    dx_ = dy_ = 0;
    latched_ = 0;
    sent_ = 0;
    nextByte_ = lastNow_ + byteCycles_;
    packets_++;
}

void Mouse::sendDue(uint64_t now, Scc& scc) {
    while (busy() && now >= nextByte_) {
        scc.receive(Scc::CH_B, packet_[sent_++]);
        nextByte_ += byteCycles_;
    }
}

} // namespace x68k
