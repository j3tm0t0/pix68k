#include "x68k/devices.h"

namespace x68k {

// ---------------------------------------------------------------- システムポート

void SysPort::reset() {
    tvctrl_ = imageUnit_ = sramWrite_ = power_ = 0;
    // コントラストの電源投入時の値は資料がない。IPL ROM は起動の初めに SRAM の値を書く ($FF00A4 など) ので、
    // その前に見えるものはない。IPL ROM を通らずに動かすとき (試験) に画面が黒くならないよう、最大 (15) にしておく
    contrast_ = 15;
    keyctrl_ = 0x08; // bit3 = 1: キーボードがつながっている
}

uint8_t SysPort::read(uint32_t offset) {
    switch (offset & 0x0f) {
    case 0x1: return (uint8_t)(0xf0 | contrast_);
    case 0x3: return (uint8_t)(0xf0 | tvctrl_);
    case 0x5: return (uint8_t)(0xe0 | imageUnit_);
    case 0x7: return (uint8_t)(0xf0 | keyctrl_); // bit3 = 1: キーボードがつながっている
    case 0xb: return cpuInfo;
    default: return 0xff;
    }
}

void SysPort::write(uint32_t offset, uint8_t v) {
    switch (offset & 0x0f) {
    case 0x1: contrast_ = v & 0x0f; break;
    case 0x3: tvctrl_ = v & 0x0b; break;
    case 0x5: imageUnit_ = v & 0x1f; break;
    case 0x7: keyctrl_ = (uint8_t)((v & 0x0e) | 0x08); break;
    case 0xd: sramWrite_ = v; break;
    case 0xf: power_ = v & 0x0f; break;
    }
}

// ---------------------------------------------------------------- I/O コントローラ

void Ioc::reset() { status_ = enable_ = vector_ = 0; }

uint8_t Ioc::read(uint32_t offset) {
    if ((offset & 0x0f) == 0x1) return (uint8_t)(status_ | enable_);
    return 0xff;
}

void Ioc::write(uint32_t offset, uint8_t v) {
    if ((offset & 0x0f) == 0x1) enable_ = v & 0x0f;
    else if ((offset & 0x0f) == 0x3) vector_ = v & 0xfc;
}

// 状態のビットと許可のビットの対応
static const uint8_t kIocStatus[4] = {0x80, 0x40, 0x10, 0x20}; // FDC, FDD, HDD, プリンタ
static const uint8_t kIocEnable[4] = {0x04, 0x02, 0x08, 0x01};

int Ioc::ack() {
    for (int s = 0; s < 4; s++)
        if ((status_ & kIocStatus[s]) && (enable_ & kIocEnable[s])) return vector_ | s;
    return -1;
}

} // namespace x68k
