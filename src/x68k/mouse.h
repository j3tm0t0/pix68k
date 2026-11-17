// X68000 のマウス。SCC のチャネル B につながり、RTS (MSCTRL) を出されると 3 バイトを 4800bps で送る。
//
// 3 バイト: 状態、X の移動量、Y の移動量 (符号付き 8 ビット。右と下が正)。
//   状態 bit0 左ボタン、bit1 右ボタン (押していれば 1)、bit4 X が +127 を超えた、bit5 X が -128 を下回った、
//   bit6 Y が +127 を超えた、bit7 Y が -128 を下回った
// 超えた分は捨てる (実機では確かめていない)。
// ボタンの位置は、本物の IPL ROM の IOCS (_MS_GETDT) で左右が合うことを確かめた
// (tests/x68k/mouse_iocs.cpp)。
// 動いていなくても求められるたびに送る (実機では確かめていない)。
// 前のパケットを送り終える前の要求は無視する。
//
// ホストのマウスの動きは onMouse() で溜め、送るときにまとめて渡す。求められる前に押して離したボタンも
// 1 回は押したと送る (クリックを落とさない)。
//
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stdint.h>

#include "x68k/scc.h"

namespace x68k {

class StateIO;

class Mouse : public Scc::Listener {
public:
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)
    // 状態のバイトのビット
    enum Status : uint8_t {
        ST_LEFT = 0x01, ST_RIGHT = 0x02,
        ST_X_OVER = 0x10, ST_X_UNDER = 0x20, ST_Y_OVER = 0x40, ST_Y_UNDER = 0x80,
    };
    // onMouse() の buttons のビット (platform::InputSink と同じ)
    enum Button : uint8_t { BTN_LEFT = 0x01, BTN_RIGHT = 0x02, BTN_MIDDLE = 0x04 };

    explicit Mouse(uint32_t cpuHz = 16000000) { setClock(cpuHz); reset(); }
    void setClock(uint32_t cpuHz);
    void reset();

    // ホストのマウスが動いた / ボタンが変わった。buttons は今押しているボタン (BTN_*)
    void onMouse(int dx, int dy, uint8_t buttons);

    // 移動量とボタン (状態のビット) から 3 バイトを作る
    static void makePacket(int dx, int dy, uint8_t buttons, uint8_t out[3]);

    // ---- Scc::Listener ----
    void onRts(int ch, bool asserted) override;

    // now (CPU サイクル) まで進め、送る時刻になったバイトを SCC に渡す (周辺機器を進めるたびに呼ぶ)
    void update(uint64_t now, Scc& scc) {
        lastNow_ = now;
        if (busy() && now >= nextByte_) sendDue(now, scc);
    }

    // 1 バイトを送る時間 (CPU サイクル): スタート、データ 8、ストップ 2 の 11 ビット
    uint64_t byteCycles() const { return byteCycles_; }
    bool busy() const { return sent_ < 3; }
    int pendingDx() const { return dx_; }
    int pendingDy() const { return dy_; }
    uint32_t packets() const { return packets_; }

private:
    void sendDue(uint64_t now, Scc& scc); // update() の中身: 送る時刻になったバイトを渡す
    uint64_t byteCycles_ = 0;
    int32_t dx_ = 0, dy_ = 0;
    uint8_t buttons_ = 0;  // 今押しているボタン (状態のビット)
    uint8_t latched_ = 0;  // 前のパケットのあとに押したボタン
    uint8_t packet_[3] = {};
    int sent_ = 3;         // packet_ のうち SCC に渡したバイト数 (3 なら送っていない)
    uint64_t nextByte_ = 0;
    uint64_t lastNow_ = 0;
    uint32_t packets_ = 0;
};

} // namespace x68k
