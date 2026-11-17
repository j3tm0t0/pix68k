// MIDI ボード CZ-6BM1 の YM3802 (MCS: MIDI Communication and Service controller)。
//
// アドレスと割り込み:
//   $EAFA01-$EAFA0F の奇数番地  R0-R7 ($EAFA01 + n x 2 が Rn)。偶数番地は読むと $FF、書いても何も起きない
//   $EAFA10-                    2 枚目のボード (作っていない) とその先はバスエラー
//   割り込み                     拡張スロットの IRQ4 (レベル 4)。ベクタは YM3802 が出す (IVR)
// レジスタ (R0-R3 は固定、R4-R7 は RGR の下位 4 ビットで選ぶ群 0-9 の 4 つ。群 g の Rn を「R gn」と書く):
//   R0  IVR  (読み) 割り込みベクタ = (IOR & $E0) | (要因の番号 x 2)。要因がなければ (IOR & $E0) | $10
//   R1  RGR  (書き) bit7 = 1 でリセット、bit3-0 = 群
//   R2  ISR  (読み) 割り込みの要因 (下の表)
//   R3  ICR  (書き) 1 を書いたビットの要因を消す
//   R04 IOR  ベクタの上位 3 ビット     R05 IMR  割り込みのモード (覚えるだけ)  R06 IER  要因ごとの許可
//   R14-R17  リアルタイムメッセージ (覚えるだけ)
//   R24-R27  受信の速さ、アドレスハンタ (覚えるだけ)
//   R34 RSR  受信 FIFO の状態 (いつも 0: MIDI IN はつながっていない)  R35 RCR (覚えるだけ)  R36 RDR (読むと 0)
//   R44 TRR  送信の速さ      R45 TMR  送信のビット数 (bit5 = 7 ビット、bit4 = パリティあり、bit1 = ストップビット 2 つ)
//   R54 TSR  送信 FIFO の状態: bit7 = FIFO が空、bit6 = FIFO に空きがある、bit0 = 送信中 (FIFO かシフトレジスタにデータ)
//   R55 TCR  bit0 = 送信する   R56 TDR  (書き) 送信 FIFO に入れる
//   R64-R67  FSK、クリックカウンタ (覚えるだけ)
//   R74-R77  シーケンサのカウンタ (覚えるだけ)
//   R84/R85 GTR  汎用タイマ (14 ビット、R85 の下位 6 ビットが上位)   R86/R87 MTR  MIDI クロックタイマ (同じ形)
//   R94-R96  外部 I/O (覚えるだけ。R96 を読むと覚えた値)
// 割り込みの要因 (ISR、IER のビット): bit0 MIDI のメッセージ、bit1 MIDI クロック (クリックカウンタ)、
//   bit2 再生カウンタ、bit3 記録カウンタ、bit4 オフライン、bit5 受信 FIFO にデータ、bit6 送信 FIFO が空、bit7 汎用タイマ。
//   このうち作っているのは bit1 (MTR が 0 になった)、bit6、bit7 (GTR が 0 になった) だけ。
//
// 資料: YM3802 の公開のデータシートは見つからなかった。レジスタの名前と並び、割り込みの要因のビット、TSR と TMR と TRR の
// ビット、アドレスと IRQ4、ベクタの要因の番号 (送信 FIFO が空で $0C、MIDI クロックで $02、汎用タイマで $0E)、タイマの
// 刻み (8us) は、メーカーの資料でも実機でも確かめていない。この値で、悪魔城ドラキュラが MIDI ボードを通して MT-32 と
// GS の曲を鳴らす (docs/compat.md)。tests/unit/midi_test.cpp は今の動きを固定するもの。
//
// 資料から決められず、推測で決めたこと:
// - クロック: CLKM = 1MHz とした。送信の速さは TRR の bit4 = 0 なら CLKM / (bit3 ? 32 : 16) (TRR = $08 で 31.25kbps)、
//   bit4 = 1 なら CLKF (614.4kHz) / (bit3 ? 64 << (bit2-0) : 32)。CLKM を 500kHz とすると、X68000 の
//   MIDI のドライバが書く値で 31.25kbps にならない
// - タイマ (GTR、MTR) は CLKM / 8 (125kHz、8us) で 1 ずつ減り、0 になったら値を読み直して要因を立てる。値が 0 なら止まる。
//   上位のレジスタ (R85、R87) を書いたときに数え直す
// - MTR が 0 になると、いつも bit1 を立てる (クリックカウンタ (CDR) で数を割ることはしていない)
// - 要因は IER のビットが 1 のときだけ ISR に立つ。送信 FIFO が空の要因は、FIFO が空で IER の bit6 が 1 の間は
//   立ち続ける (ICR で消しても、まだ空ならすぐに立つ)。TDR に書くと消える
// - 送信 FIFO は 16 バイト。満杯のときに TDR に書いた値は捨てる
// - TCR の bit0 が 0 の間は送らない (FIFO にたまる)。送っている途中のバイトは送り終える
// - ベクタは、立っている要因のうちビットの番号が小さいものから出す (MIDI クロックの bit1 が先で、送信 FIFO が空の bit6、
//   汎用タイマの bit7 の順。たとえば bit6 と bit7 が同時に立っていれば、先に (IOR & $E0) | $0C を出し、ICR で bit6 を消すと (IOR & $E0) | $0E)
// - 割り込みを受け付けても (IACK) 要因は消えない (ICR で消す)
// - リセットの後は、すべてのレジスタが 0、FIFO は空
// - IMR (R05) は覚えるだけで、割り込みには使わない (IMR のビットの意味が分かる資料が見つからなかった)
// - GTR / MTR は、上位 (R85 / R87) を書くといつも数え直す (R85 / R87 の bit7-6 の意味が分かる資料が見つからなかった)
// 送り終えたバイトは takeSent() で取り出す (Machine が Sound に渡し、音の合成をするコアに時刻つきで届く)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "x68k/midi_shadow.h"

namespace x68k {

class StateIO;

class Ym3802 {
public:
    static constexpr uint32_t BASE = 0xeafa00; // R0 は $EAFA01
    static constexpr uint32_t SIZE = 0x10;     // $EAFA00-$EAFA0F
    static constexpr int INTERRUPT_LEVEL = 4;
    static constexpr int TX_FIFO = 16;
    static constexpr uint32_t CLKM = 1000000;   // 推測
    static constexpr uint32_t CLKF = 614400;
    static constexpr uint32_t CLOCK = 4000000;  // advance() に渡すクロック
    static constexpr int SENT_QUEUE = 64;       // 送り終えたバイトをためる数 (Machine がラスタごとに取り出す)

    enum Irq : uint8_t {
        IRQ_MIDI_CLOCK = 0x02,
        IRQ_TX_EMPTY = 0x40,
        IRQ_GENERAL_TIMER = 0x80,
    };

    Ym3802() { reset(); }
    void reset();

    // CPU から見えるレジスタ (reg は 0..7: Rn)
    uint8_t read(int reg);
    void write(int reg, uint8_t v);

    // 4MHz のクロックで clocks だけ進める
    void advance(uint32_t clocks);
    // CPU のサイクル数 now まで進める (前に進めた時刻からの差を 4MHz に直す)。now が前より小さければ、そこから数え直す
    void advanceTo(uint64_t now, uint32_t cpuHz);
    void setTime(uint64_t now) {
        synced_ = now;
        acc_ = 0;
    }
    // ステートを読んだあとに呼ぶ: 進め終えた時刻が now より先か、now より far を超えて古いか、端数が 1 秒分 (cpuHz) 以上
    // なら、now から数え直す (壊れたステートで、追いつくまでの繰り返しが終わらなくならないように)
    void limitLag(uint64_t now, uint64_t far, uint32_t cpuHz) {
        if (synced_ > now || now - synced_ > far || acc_ >= cpuHz) setTime(now);
    }

    bool irq() const { return isr_ != 0; }
    uint8_t vector() const;
    // 送り終えたバイト (なければ -1)
    int takeSent();

    // 試験用
    uint8_t isr() const { return isr_; }
    int txCount() const { return txCount_; }
    // 1 バイトを送るのにかかる 4MHz のクロック数 (今の TRR と TMR から)
    uint32_t byteClocks() const;
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)。読んだ値は sanitize() で範囲に収める
    void sanitize();
    // 音源に渡したバイトから覚えた、音源の設定 (Machine が渡すときに feed() する)。外の音源の状態なので、
    // YM3802 のリセットでは消さない。ステートには一緒に入る
    MidiShadow& shadow() { return shadow_; }

private:
    uint8_t& reg(int group, int n) { return regs_[group * 4 + (n - 4)]; }
    uint8_t reg(int group, int n) const { return regs_[group * 4 + (n - 4)]; }
    void update();
    void startByte();
    void timerReload(int which);

    uint8_t rgr_ = 0;
    uint8_t isr_ = 0;
    uint8_t regs_[40] = {}; // 群 0-9 の R4-R7
    // 送信
    uint8_t txFifo_[TX_FIFO] = {};
    uint8_t pad0_[2] = {}; // 詰め物 (ステートに入るので 0 にしておく。x68k/state.h。下の pad1_、pad2_ も同じ)
    int txHead_ = 0, txCount_ = 0;
    uint8_t shifting_ = 0;      // 1 ならシフトレジスタにバイトがある (0 か 1。ステートの値を bool で読まないため)
    uint8_t shiftByte_ = 0;
    uint8_t pad1_[2] = {};
    uint32_t shiftLeft_ = 0;    // そのバイトを送り終えるまでの 4MHz のクロック数
    // タイマ (0 が汎用タイマ、1 が MIDI クロックタイマ)。残りは 4MHz のクロック数。周期が 0 なら止まっている
    uint32_t timerPeriod_[2] = {};
    uint32_t timerLeft_[2] = {};
    // 送り終えたバイト
    uint8_t sent_[SENT_QUEUE] = {};
    int sentHead_ = 0, sentCount_ = 0;
    uint8_t pad2_[4] = {};
    // CPU のサイクルからの換算
    uint64_t synced_ = 0;
    uint64_t acc_ = 0; // 端数 (CPU のサイクル x 4MHz)
    MidiShadow shadow_;
};

} // namespace x68k
