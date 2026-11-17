// MFP (MC68901): GPIP、割り込みコントローラ、タイマ 4 本、USART (キーボード)
// X68000 では割り込みレベル 6 につながる。
#pragma once

#include <stdint.h>

namespace x68k {

class StateIO;

class Mfp {
public:
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)
    // 割り込み要因の番号 (大きいほど優先度が高い。ベクタの下位 4 ビットになる)
    enum Source : int {
        SRC_GPIP0 = 0,  // RTC アラーム
        SRC_GPIP1 = 1,  // EXPWON
        SRC_GPIP2 = 2,  // POWSW
        SRC_GPIP3 = 3,  // OPM
        SRC_TIMER_D = 4,
        SRC_TIMER_C = 5,
        SRC_GPIP4 = 6,  // V-DISP
        SRC_GPIP5 = 7,
        SRC_TIMER_B = 8,
        SRC_TX_ERROR = 9,
        SRC_TX_EMPTY = 10,
        SRC_RX_ERROR = 11,
        SRC_RX_FULL = 12,
        SRC_TIMER_A = 13,
        SRC_GPIP6 = 14, // CRTC のラスタ割り込み
        SRC_GPIP7 = 15, // H-SYNC
    };
    // GPIP のビット
    enum Gpip : int {
        GPIP_ALARM = 0,
        GPIP_EXPWON = 1,
        GPIP_POWSW = 2,
        GPIP_OPMIRQ = 3,
        GPIP_VDISP = 4,
        GPIP_CIRQ = 6,
        GPIP_HSYNC = 7,
    };

    void reset();
    // reg はレジスタ番号 (0..23)。アドレス $E88001 + reg*2
    uint8_t read(int reg);
    void write(int reg, uint8_t v);

    // GPIP の入力を変える。AER で選んだ向きの変化なら割り込み要因を立てる。
    // GPIP4 (V-DISP) はタイマ A のイベント入力も兼ねる
    void setGpip(int bit, bool level);
    bool gpip(int bit) const { return (gpipIn_ >> bit) & 1; }

    // MFP のクロック (4MHz) で ticks だけ進める
    void advance(uint32_t ticks);
    // ディレイモードのタイマが次に割り込みを要求するまでのクロック数 (advance() にこの数を渡すと、ちょうど要求が出る)。
    // 割り込みが許可されていて (IER) マスクされていない (IMR) タイマだけを見る。なければ NO_TIMER_IRQ。
    // Machine が、CPU を止めて周辺機器を進める時刻を決めるのに使う (状態は変えない)
    static constexpr uint32_t NO_TIMER_IRQ = 0xffffffffu;
    uint32_t clocksToTimerIrq() const;
    // 割り込み要因 src が許可されていて (IER)、マスクされていない (IMR) か
    bool sourceOpen(int src) const;

    // キーボードからの受信データ (USART)。受信バッファに入れて受信割り込みを立てる
    void receive(uint8_t data);
    // キーボードへの送信データを取り出す。なければ -1
    int takeTransmit();
    // キーボードへの送信データがあるか (takeTransmit() が -1 以外を返すか)
    bool txPending() const { return txData_ >= 0; }
    // 受信バッファにまだ読まれていないデータがあるか
    bool rxFull() const { return rxFull_; }
    // 受信が許可されているか (RSR bit0 = RE)
    bool rxEnabled() const;

    // 割り込み要求が出ているか (周辺機器を進めるたびに見るので、ヘッダに置いてインライン展開させる)
    bool irq() const { return highestPending() >= 0; }
    // 割り込みアクノリッジ。ベクタ番号を返す。要求がなければ -1
    int ack();

private:
    // highestPending() が読むレジスタの番号 (mfp.cpp の Reg と同じ。static_assert で確かめる)
    enum : int { R_IPRA = 5, R_IPRB = 6, R_ISRA = 7, R_ISRB = 8, R_IMRA = 9, R_IMRB = 10, R_VR = 11 };
    // 優先度の最も高い、受け付けられる割り込み要因。なければ -1。
    // 要因 15..8 は A のレジスタ、7..0 は B のレジスタのビット 7..0 なので、A:B を 16 ビットにして上から探す
    int highestPending() const {
        const uint32_t p = ((uint32_t)(reg_[R_IPRA] & reg_[R_IMRA]) << 8) | (reg_[R_IPRB] & reg_[R_IMRB]);
        // ソフトウェア EOI モードでは、処理中の要因より優先度の低い要因は待たされる (同じ要因も)
        const uint32_t s = (reg_[R_VR] & 0x08) ? (((uint32_t)reg_[R_ISRA] << 8) | reg_[R_ISRB]) : 0;
        const uint32_t any = p | s;
        if (any == 0) return -1;
        const int top = 31 - __builtin_clz(any);
        if (s & (1u << top)) return -1;
        return top;
    }
    void request(int src);
    bool timerEvent(int t); // タイマ t のカウンタを 1 減らし、0 になったら割り込みを立てる
    uint8_t reg_[24] = {};
    uint8_t gpipIn_ = 0;
    uint8_t counter_[4] = {};  // タイマのカウンタ (データレジスタを読むと見える値)
    uint8_t pad0_[3] = {};     // 詰め物 (ステートに入るので 0 にしておく。x68k/state.h)
    uint32_t prescale_[4] = {}; // プリスケーラの残り
    uint8_t rxData_ = 0;
    bool rxFull_ = false;
    uint8_t pad1_[2] = {}; // 詰め物
    int txData_ = -1;
};

} // namespace x68k
