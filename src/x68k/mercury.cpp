// まーきゅりーゆにっと。mercury.h を参照
#include "x68k/mercury.h"

namespace x68k {

void Mercury::reset() {
    cmd_ = mode_ = high_ = 0;
    leftNext_ = true;
    head_ = count_ = 0;
    acc_ = 0;
    outL_ = outR_ = 0;
}

uint32_t Mercury::rate() const {
    static const uint32_t kRates[4] = {44100, 32000, 44100, 48000};
    const uint32_t hz = kRates[(cmd_ >> 4) & 3];
    return (cmd_ & 0x80) ? hz : hz / 2;
}

uint8_t Mercury::read(uint32_t off, uint32_t pendingClocks4M) const {
    switch (off & 0x1fff) {
    case 0x90: {
        // PCS は DMAC の PCS に出す EXPCL と同じ値にする (推測で決めたこと。mercury.h)
        const bool pcs = expcl(pendingClocks4M);
        return (uint8_t)(0xf0 | (pcs ? 0x08 : 0) | (drq() ? 0 : 0x04) | (mode_ & 3));
    }
    case 0x91: return cmd_;
    case 0xa1: return (uint8_t)((cmd_ & 0xf0) | 0x0f);
    default: return 0xff;
    }
}

void Mercury::writeData(uint16_t w) {
    if (count_ >= FIFO) return; // あふれた分は捨てる (L / R の順も進めない。進めると以後 L と R が入れ替わる)
    if (cmd_ & 0x02) leftNext_ = !leftNext_;
    fifo_[(head_ + count_) % FIFO] = w;
    count_++;
}

// 1 サンプル分を取り出す (ステレオは L と R の 2 語、モノラルは 1 語)
void Mercury::take() {
    if (cmd_ & 0x02) {
        if (count_ < 2) return;
        outL_ = (int16_t)fifo_[head_];
        outR_ = (int16_t)fifo_[(head_ + 1) % FIFO];
        head_ = (head_ + 2) % FIFO;
        count_ -= 2;
    } else {
        if (count_ < 1) return;
        outL_ = outR_ = (int16_t)fifo_[head_];
        head_ = (head_ + 1) % FIFO;
        count_--;
    }
}

} // namespace x68k
