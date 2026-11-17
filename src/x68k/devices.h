// IPL ROM が起動の途中で触る周辺機器のうち、最低限の応答だけ返すもの。
// FDC は fdc.h、DMAC は dmac.h、SASI は sasi.h、SCC は scc.h、8255 は ppi.h、OPM は opm.h、ADPCM は adpcm.h にある。
// 各クラスの offset は、そのデバイスの 8KB の窓の先頭からのバイト位置。
#pragma once

#include <stdint.h>

namespace x68k {

class StateIO;

// システムポート ($E8E001-$E8E00F)
class SysPort {
public:
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)
    void reset();
    uint8_t read(uint32_t offset);
    void write(uint32_t offset, uint8_t v);
    // $E8E00D に 0x31 を書くと SRAM に書き込める
    bool sramWritable() const { return sramWrite_ == 0x31; }
    // $E8E007 の bit1 (HRL): 高解像度の 256/512 ドットのドットクロックを変える
    bool hrl() const { return (keyctrl_ & 0x02) != 0; }
    // $E8E00B が返す値: 0xFF = 68000 10MHz、0xFE = 68000 16MHz (XVI)、0xDC = 68030 25MHz
    uint8_t cpuInfo = 0xfe;

private:
    uint8_t contrast_ = 0, tvctrl_ = 0, imageUnit_ = 0, keyctrl_ = 0, sramWrite_ = 0, power_ = 0;
};

// I/O コントローラ ($E9C001 割り込みの状態とマスク、$E9C003 ベクタ)
// 割り込みレベル 1。要因は FDC、FDD (挿入・排出)、HDD、プリンタ
class Ioc {
public:
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)
    enum Source : int { SRC_FDC = 0, SRC_FDD = 1, SRC_HDD = 2, SRC_PRINTER = 3 };
    void reset();
    uint8_t read(uint32_t offset);
    void write(uint32_t offset, uint8_t v);
    // setRequest() と irq() は周辺機器を進めるたびに呼ぶので、ヘッダに置いてインライン展開させる
    void setRequest(int src, bool on) {
        if (on) status_ |= kStatusBit[src];
        else status_ &= (uint8_t)~kStatusBit[src];
    }
    // 許可した要因の要求があるか。許可のビット (bit3 HDD、bit2 FDC、bit1 FDD、bit0 プリンタ) を状態のビットの
    // 位置 (bit4、bit7、bit6、bit5) に移して比べる
    bool irq() const { return (status_ & (((enable_ & 0x07) << 5) | ((enable_ & 0x08) << 1))) != 0; }
    int ack(); // ベクタ番号。要求がなければ -1

private:
    static constexpr uint8_t kStatusBit[4] = {0x80, 0x40, 0x10, 0x20}; // 要因ごとの状態のビット (FDC, FDD, HDD, プリンタ)
    uint8_t status_ = 0; // bit7 FDC、bit6 FDD、bit5 プリンタ、bit4 HDD
    uint8_t enable_ = 0; // bit3 HDD、bit2 FDC、bit1 FDD、bit0 プリンタ
    uint8_t vector_ = 0;
};

} // namespace x68k
