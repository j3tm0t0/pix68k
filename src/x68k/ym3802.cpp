// MIDI ボード CZ-6BM1 の YM3802。ym3802.h を参照
#include "x68k/ym3802.h"

#include <string.h>

namespace x68k {

namespace {
// タイマの 1 刻み (CLKM / 8 = 125kHz) の 4MHz のクロック数
constexpr uint32_t kTimerTick = Ym3802::CLOCK / (Ym3802::CLKM / 8);
} // namespace

void Ym3802::reset() {
    rgr_ = 0;
    isr_ = 0;
    memset(regs_, 0, sizeof regs_);
    txHead_ = txCount_ = 0;
    shifting_ = 0;
    shiftByte_ = 0;
    shiftLeft_ = 0;
    for (int i = 0; i < 2; i++) timerPeriod_[i] = timerLeft_[i] = 0;
    // 送り終えたバイト (sent_) はリセットで捨てない (リセットの前に送り終えたものは Machine が取り出す)
}

// ステートから読んだ値を、配列の範囲に収める (ステートはネットワークの口から書き換えられる)
void Ym3802::sanitize() {
    if (txHead_ < 0 || txHead_ >= TX_FIFO) txHead_ = 0;
    if (txCount_ < 0 || txCount_ > TX_FIFO) txCount_ = 0;
    if (sentHead_ < 0 || sentHead_ >= SENT_QUEUE) sentHead_ = 0;
    if (sentCount_ < 0 || sentCount_ > SENT_QUEUE) sentCount_ = 0;
    shifting_ = shifting_ ? 1 : 0;
    shadow_.sanitize();
}

uint32_t Ym3802::byteClocks() const {
    const uint8_t trr = reg(4, 4), tmr = reg(4, 5);
    const uint32_t bits = 1 + ((tmr & 0x20) ? 7 : 8) + ((tmr & 0x10) ? 1 : 0) + ((tmr & 0x02) ? 2 : 1);
    uint64_t div, src;
    if (trr & 0x10) {
        src = CLKF;
        div = (trr & 0x08) ? (64u << (trr & 7)) : 32;
    } else {
        src = CLKM;
        div = (trr & 0x08) ? 32 : 16;
    }
    return (uint32_t)((CLOCK * div * bits + src / 2) / src);
}

uint8_t Ym3802::vector() const {
    const uint8_t base = reg(0, 4) & 0xe0;
    for (int n = 0; n < 8; n++)
        if (isr_ & (1 << n)) return (uint8_t)(base | n << 1);
    return (uint8_t)(base | 0x10);
}

// 送信 FIFO が空の要因 (FIFO が空で、許可されている間は立ち続ける)
void Ym3802::update() {
    if (txCount_ == 0 && (reg(0, 6) & IRQ_TX_EMPTY)) isr_ |= IRQ_TX_EMPTY;
}

// FIFO の先頭をシフトレジスタに移して送り始める
void Ym3802::startByte() {
    if (shifting_ || txCount_ == 0 || !(reg(5, 5) & 0x01)) return;
    shiftByte_ = txFifo_[txHead_];
    txHead_ = (txHead_ + 1) % TX_FIFO;
    txCount_--;
    shifting_ = 1;
    shiftLeft_ = byteClocks();
    update();
}

void Ym3802::timerReload(int which) {
    const int n = which == 0 ? 4 : 6; // GTR は R84/R85、MTR は R86/R87
    const uint32_t v = (uint32_t)reg(8, n) | (uint32_t)(reg(8, n + 1) & 0x3f) << 8;
    timerPeriod_[which] = v * kTimerTick;
    timerLeft_[which] = timerPeriod_[which];
}

uint8_t Ym3802::read(int r) {
    switch (r & 7) {
    case 0: return vector();
    case 1: return rgr_;
    case 2: return isr_;
    case 3: return 0;
    default: break;
    }
    const int g = rgr_ & 0x0f;
    if (g > 9) return 0;
    const int n = r & 7;
    if (g == 3 && (n == 4 || n == 6)) return 0; // RSR、RDR: 受信するものがない
    if (g == 5 && n == 4) {                     // TSR
        uint8_t v = 0;
        if (txCount_ == 0) v |= 0x80;
        if (txCount_ < TX_FIFO) v |= 0x40;
        if (txCount_ != 0 || shifting_) v |= 0x01;
        return v;
    }
    return reg(g, n);
}

void Ym3802::write(int r, uint8_t v) {
    switch (r & 7) {
    case 0: return; // IVR は読むだけ
    case 1:
        if (v & 0x80) {
            reset();
            return;
        }
        rgr_ = v & 0x0f;
        return;
    case 2: return; // ISR は読むだけ
    case 3:
        isr_ &= (uint8_t)~v;
        update();
        return;
    default: break;
    }
    const int g = rgr_ & 0x0f;
    if (g > 9) return;
    const int n = r & 7;
    if (g == 5 && n == 6) { // TDR
        if (txCount_ < TX_FIFO) {
            txFifo_[(txHead_ + txCount_) % TX_FIFO] = v;
            txCount_++;
        }
        isr_ &= (uint8_t)~IRQ_TX_EMPTY;
        startByte();
        return;
    }
    reg(g, n) = v;
    if (g == 0 && n == 6) {
        isr_ &= v; // 許可を外した要因は消す (推測)
        update();
    } else if (g == 5 && n == 5) {
        startByte();
    } else if (g == 8 && (n == 5 || n == 7)) {
        timerReload(n == 5 ? 0 : 1);
    }
}

void Ym3802::advance(uint32_t clocks) {
    // タイマ
    static const uint8_t kTimerIrq[2] = {IRQ_GENERAL_TIMER, IRQ_MIDI_CLOCK};
    for (int t = 0; t < 2; t++) {
        if (timerPeriod_[t] == 0) continue;
        uint32_t c = clocks;
        while (c >= timerLeft_[t]) {
            c -= timerLeft_[t];
            timerLeft_[t] = timerPeriod_[t];
            if (reg(0, 6) & kTimerIrq[t]) isr_ |= kTimerIrq[t];
        }
        timerLeft_[t] -= c;
    }
    // 送信
    uint32_t c = clocks;
    while (shifting_ && c >= shiftLeft_) {
        c -= shiftLeft_;
        shifting_ = 0;
        if (sentCount_ < SENT_QUEUE) {
            sent_[(sentHead_ + sentCount_) % SENT_QUEUE] = shiftByte_;
            sentCount_++;
        }
        startByte();
    }
    if (shifting_) shiftLeft_ -= c;
}

void Ym3802::advanceTo(uint64_t now, uint32_t cpuHz) {
    if (now < synced_) {
        setTime(now);
        return;
    }
    acc_ += (now - synced_) * CLOCK;
    synced_ = now;
    if (acc_ < cpuHz) return;
    const uint64_t clocks = acc_ / cpuHz;
    acc_ %= cpuHz;
    // 長く進めていなかったときも (ラスタごとに呼ぶので、ふつうは 1 ラスタ分)、1 秒ずつに分けて進める
    for (uint64_t left = clocks; left;) {
        const uint32_t step = left > CLOCK ? CLOCK : (uint32_t)left;
        advance(step);
        left -= step;
    }
}

int Ym3802::takeSent() {
    if (sentCount_ == 0) return -1;
    const uint8_t b = sent_[sentHead_];
    sentHead_ = (sentHead_ + 1) % SENT_QUEUE;
    sentCount_--;
    return b;
}

} // namespace x68k
