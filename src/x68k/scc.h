// SCC (Z8530、$E98001-$E98007)。チャネル B がマウス、A が RS-232C。割り込みレベル 5。
//
// アドレス: $E98001 チャネル B のコマンド、$E98003 B のデータ、$E98005 A のコマンド、$E98007 A のデータ。
// コマンドのポートは、WR0 の下位 3 ビット (と Point High) で選んだレジスタを次の 1 回だけ読み書きする。
//
// 作るもの:
//   - レジスタの選択: WR0 の下位 3 ビット (と Point High) で選んだレジスタを、次にどちらかのチャネルのコマンドのポートを
//     読み書きしたときに使う。ポインタは 2 つのチャネルで 1 つ (実物と同じ。A の WR0 で選んで B のポートを読むと B の
//     そのレジスタになる)。データのポートはポインタを使わず、変えない
//   - 受信: 3 バイトの FIFO。RR0 bit0 で受信あり、データのポートで読む。FIFO が満ちているときに届いたバイトは最後の
//     バイトを上書きし、そのバイトのオーバーランのフラグを立てる。そのバイトが FIFO の先頭に来ると RR1 bit5 が立ち、
//     Error Reset (WR0 のコマンド 6) まで残る (特殊受信条件。パリティとフレーミングの誤りは線の相手から来ないので作らない)
//   - 受信割り込み (WR1 bit4-3): 01 最初の 1 文字か特殊受信条件、10 すべての文字か特殊受信条件、11 特殊受信条件だけ。
//     WR9 bit3 (MIE) で全体を許可。最初の 1 文字の要求は、その文字を読み出したときに下ろす (Zilog の SCC の
//     ユーザーマニュアル: "This IP is reset when this character is removed from the SCC")。次は Enable Int on Next
//     Rx Character (WR0 のコマンド 4) か、WR1 でこのモードにし直したときに出す。そのとき FIFO に文字があればすぐ出す。
//     01 と 11 のモードでは、特殊受信条件のバイトが先頭にある間、データを読んでも FIFO は進まない (Error Reset で進む)
//     10 のモードでは FIFO を止めず、特殊受信条件の要求はそのバイトを読めば下ろす (RR1 bit5 は Error Reset まで残る)
//   - 特殊受信条件の要求は受信の要因の場所 (優先順位と IUS) を使い、ベクタの符号だけを変える (A 111、B 011)
//   - 送信 (チャネル A): 送信バッファとシフトレジスタ。WR12 / WR13 の時定数と WR4 のクロックの倍率、WR5 / WR4 の
//     ビット数から 1 文字の時間を数え、シフトレジスタが空いたらバッファのバイトを移す (移したときに送信バッファ空の要求)。
//     送り終えたバイトは SerialLink に渡す。WR5 bit3 (送信の許可) が 0、または WR3 bit5 (Auto Enables) で CTS が出て
//     いなければ移さない。チャネル B (マウスにつながり、送るものはない) は、前と同じく書いたらすぐ送り終えたことにする
//   - 受信 (チャネル A): SerialLink から、1 文字の時間ごとに 1 バイトまで取り出して FIFO に入れる。WR3 bit0 (受信の許可)
//     と WR5 bit1 (RTS) が 1 のときだけ取り出す (相手は RTS / CTS のハードウェアのフロー制御に従うものとする。RTS を
//     下ろしている間のバイトは相手の側で待たせる)。WR3 bit5 (Auto Enables) なら DCD が出ていないと受け取らない
//   - DCD / CTS (RR0 bit3 / bit5): チャネル A は SerialLink の carrier() (TCP なら接続している間 1)。チャネル B は 0
//   - 外部 / 状態の割り込み: WR15 bit3 (DCD) / bit5 (CTS) で選んだ線が変わると RR0 のその値を止め (ラッチ)、WR1 bit0
//     なら要求を出す。Reset Ext/Status Interrupts (WR0 のコマンド 2) で要求を下ろしてラッチを開く。開いたときに止めた値と
//     今の値が違えば、また止めて要求を出す。ブレーク、ゼロカウント、送信アンダーランの割り込みは作らない
//   - 送信割り込み (WR1 bit1)
//   - 割り込みの優先順位と IUS: A 受信 > A 送信 > A 外部 > B 受信 > B 送信 > B 外部。CPU が応答すると
//     その要因の IUS が立ち、WR0 のコマンド 7 (Reset Highest IUS) で下ろすまで、同じか低い要因は出ない
//   - ベクタ: WR2。WR9 bit0 (VIS) なら要因で書き換える (bit4 = 0 で bit3-1、1 で bit4-6 を逆順に)。
//     チャネル B の RR2 は、VIS や MIE、IUS に関係なく、優先度の最も高い要求 (IP) の状態で書き換えたベクタが読める
//     (Z8530 Technical Manual 7.2.3 Read Register 2。要求がなければ状態は 011)。チャネル A の RR2 は WR2 のまま
//   - WR5 bit1 (RTS): チャネル B の RTS はマウスの MSCTRL につながっていて、1 にする (RTS を出す、
//     端子は L) とマウスが 3 バイトを送ってくる。変わったら Listener に知らせる
// ボーレート: X68000 の SCC のクロック (PCLK) は 5MHz。時定数 TC、倍率 M (WR4 bit7-6: x1 / x16 / x32 / x64) で
// 5000000 / (2 x M x (TC + 2)) bps。WR14 (ボーレートジェネレータの許可とクロックの選択) は見ずに、いつもこの式で数える。
// 同期モードは作らない (WR4 bit3-2 が 00 でも非同期の 1 ストップビットとして数える)。
//
// IPL ROM (IPLROM.DAT) は起動時に WR9=$C0 (リセット)、WR2=$50、チャネル B に WR4=$4C (2 ストップビット)、
// WR3=$C1、WR1=$10 (すべての文字で割り込み)、WR9=$09 (MIE、VIS) を書き、WR5 の $60 / $62 を繰り返して
// マウスにデータを求める (トレースで確かめた)。受信割り込みのベクタは $54 になる。
//
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stdint.h>

#include "x68k/serial_link.h"

namespace x68k {

class StateIO;

class Scc {
public:
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)
    enum Channel : int { CH_A = 0, CH_B = 1 };
    static constexpr int FIFO_SIZE = 3;
    static constexpr uint32_t PCLK = 5000000; // SCC のクロック (Hz)

    // RTS の変化を受け取る (マウス)
    class Listener {
    public:
        virtual ~Listener() {}
        // asserted: WR5 bit1 が 1 (RTS を出している)
        virtual void onRts(int ch, bool asserted) = 0;
    };

    explicit Scc(uint32_t cpuHz = 16000000) : cpuHz_(cpuHz ? cpuHz : 1) {}
    void setListener(Listener* l) { listener_ = l; }
    // RS-232C (チャネル A) の相手。nullptr ならつながっていない (DCD と CTS は 0、送ったバイトは捨てる)
    void setLink(SerialLink* link) { link_ = link; }
    SerialLink* link() const { return link_; }
    // CPU のクロック (時刻の単位)。1 文字の時間を数えるのに使う
    void setClock(uint32_t cpuHz) { cpuHz_ = cpuHz ? cpuHz : 1; }

    // 電源投入 / リセット信号 (WR9 のハードウェアリセットと同じ)
    void reset();
    // offset: デバイスの窓の先頭からのバイト位置 (下位 3 ビットを見る)
    uint8_t read(uint32_t offset);
    void write(uint32_t offset, uint8_t v);
    // now (CPU サイクル) まで進める: チャネル A の送信を進め、SerialLink から受信し、DCD / CTS の変化を見る。
    // 周辺機器を進めるたびに呼ぶ (read() / write() の前にも、その時刻まで呼んでおく)
    void update(uint64_t now) {
        now_ = now;
        // lines: 相手を外した (setLink(nullptr)) あとや、相手のいないままステートを読んだあとに、DCD / CTS を下ろす
        if (link_ || chan_[CH_A].shiftBusy || chan_[CH_A].txBufFull || chan_[CH_A].lines) updateA();
    }

    // 相手から 1 バイト届いた。受信が許可されていなければ捨てる。FIFO が満ちていれば
    // 最後のバイトを上書きし、オーバーランにする
    void receive(int ch, uint8_t data);

    // 割り込み要求が出ているか
    bool irq() const { return highestPending() >= 0; }
    // 割り込みアクノリッジ。ベクタ番号を返す。要求がない、または WR9 bit1 (NV) なら -1
    int ack();

    // 状態 (テスト用)
    bool rts(int ch) const { return (wr_[ch][5] & 0x02) != 0; }
    bool rxEnabled(int ch) const { return (wr_[ch][3] & 0x01) != 0; }
    int rxCount(int ch) const { return chan_[ch].count; }
    uint8_t wr(int ch, int n) const { return wr_[ch][n & 15]; }
    int pointer() const { return pointer_; }
    // 1 文字の送信 / 受信にかかる CPU サイクル
    uint64_t txCharCycles(int ch) const { return charCycles(ch, true); }
    uint64_t rxCharCycles(int ch) const { return charCycles(ch, false); }
    bool txBusy(int ch) const { return chan_[ch].shiftBusy || chan_[ch].txBufFull; }

private:
    // 割り込みの要因 (番号が小さいほど優先度が高い)
    enum Source : int { SRC_A_RX = 0, SRC_A_TX, SRC_A_EXT, SRC_B_RX, SRC_B_TX, SRC_B_EXT, NUM_SRC };
    // RR0 の外部 / 状態のビット
    static constexpr uint8_t RR0_DCD = 0x08, RR0_CTS = 0x20, RR0_EXT_MASK = RR0_DCD | RR0_CTS;

    struct Chan {
        uint8_t fifo[FIFO_SIZE] = {};
        bool fifoErr[FIFO_SIZE] = {}; // そのバイトでオーバーランが起きた
        int count = 0;
        bool overrun = false;      // RR1 bit5 (特殊受信条件。Error Reset まで残る)
        bool rxLocked = false;     // 特殊受信条件のバイトで FIFO を止めている (01 と 11 のモード。Error Reset まで)
        bool rxLockedRead = false; // 止めたバイトを読み出した (このあとは Error Reset まで受信なしに見える)
        bool rxFirstArmed = true;  // 「最初の 1 文字」のモードで、次の文字で割り込みを出すか
        bool rxFirstIp = false;    // 「最初の 1 文字」のモードの受信の要求
        bool txIp = false;         // 送信バッファ空の要求
        bool extIp = false;        // 外部 / 状態の要求
        bool extLatched = false;   // RR0 の外部 / 状態のビットを止めている
        uint8_t extLatch = 0;      // 止めた値 (RR0_EXT_MASK のビット)
        uint8_t lines = 0;         // 今の DCD / CTS (RR0_EXT_MASK のビット)
        // 送信 (チャネル A)
        bool txBufFull = false;
        uint8_t txBuf = 0;
        bool shiftBusy = false;
        uint8_t shiftByte = 0;
        uint64_t shiftEnd = 0;     // シフトレジスタのバイトを送り終える時刻
        uint64_t rxNext = 0;       // 次のバイトを受け取れる時刻 (チャネル A)
    };

    void resetChannel(int ch);
    bool special(int ch) const; // 特殊受信条件の要求
    bool pending(int src) const;
    int highestIp() const;      // 要求 (IP) の立っている要因のうち優先度の最も高いもの (MIE と IUS は見ない)。なければ -1
    int highestPending() const; // 今 CPU に割り込みを要求する要因 (MIE と IUS を見る)。なければ -1
    uint8_t vectorFor(int src) const; // 状態で書き換えたベクタ
    uint8_t readReg(int ch, int reg);
    void writeReg(int ch, int reg, uint8_t v);
    void command(int ch, uint8_t v);
    int readData(int ch);
    void topChanged(int ch);    // FIFO の先頭が変わった: オーバーランのバイトなら特殊受信条件にする
    void armFirst(int ch);      // 「最初の 1 文字」の割り込みを出せるようにする
    uint64_t charCycles(int ch, bool tx) const;
    void updateA();
    void tryLoadTx(int ch, uint64_t t); // 送信バッファのバイトをシフトレジスタに移す (時刻 t)
    void setLines(int ch, uint8_t lines);
    void extReset(int ch);
    uint8_t rr0Ext(int ch) const { return chan_[ch].extLatched ? chan_[ch].extLatch : chan_[ch].lines; }

    uint8_t wr_[2][16] = {};
    Chan chan_[2];
    int pointer_ = 0; // 次にコマンドのポートで読み書きするレジスタ (2 つのチャネルで共通)
    uint8_t ius_ = 0; // 応答して処理中の要因 (Source ごとに 1 ビット)
    uint64_t now_ = 0;
    uint32_t cpuHz_;
    Listener* listener_ = nullptr;
    SerialLink* link_ = nullptr;
};

} // namespace x68k
