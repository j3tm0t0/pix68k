// まーきゅりーゆにっと (Mercury Unit V4、満開製作所 MK-MU1O)。16 ビットのステレオ PCM と、FM 音源の YMF288 を 2 個持つ
// 拡張ボード ($ECC080-)。
//
// データは DMAC のチャネル 2 (拡張スロットの外部要求) で $ECC080 に 1 語ずつ書かれる。ボードは自分のサンプリング周波数で
// データを取り出し、取り出して空きができると転送を要求する (EXREQ)。
//
// レジスタは yunkya2 さんの wifipcm (GIMONS さんの PilederX をもとにした互換ボード) の README と pcm.c の説明から
// 作った (コードは使っていない)。本物のボードでは確かめていない:
//   $ECC080.w (W)  PCM のデータ。上位バイト、下位バイトの順に書き、下位バイトを書いたところで 1 語になる。
//                  ステレオでは L、R の順に交互、モノラルでは同じ値を L と R に出す
//   $ECC090   (W)  DMAC のモード。bit1 = EXREQ / EXPCL を出す、bit0 = EXACK で EXREQ を下げる (記録するだけ)
//             (R)  bit3 = PCS (EXPCL と同じ値。1 サンプルの前半なら 1)、bit2 = EXREQ (0 なら要求している)、bit1-0 = 書いた値。ほかは 1
//   $ECC091   (R/W) コマンド。bit7 = 1 なら等倍、0 なら半分の周波数。bit5-4 = 周波数 (0 44.1kHz、1 32kHz、
//                  2 44.1kHz、3 48kHz)。bit3 = R を出す、bit2 = L を出す、bit1 = ステレオ。bit0 は分からない
//   $ECC0A1   (R)  ステータス: コマンドの上位 4 ビット | $0F
//   $ECC0B1   (R/W) YMF288 の割り込みのベクタ。リセットで $FF ($ECC0B1-$ECC0BF の奇数番地のどこでも同じ)
//   $ECC0C1-$ECC0C7  マスターの YMF288 (x68k/opn3.h): $C1 アドレス 0 / ステータス 0、$C3 データ、
//                  $C5 アドレス 1 / ステータス 1、$C7 データ
//   $ECC0C9-$ECC0CF  スレーブの YMF288 (同じ並び)
// $ECC080-$ECC0FF のほかの場所は、読むと $FF、書いても何も起きない。
//
// YMF288 のまわりは、V4 の回路図と ispLSI の論理 (Artemis.abl、Lunar.abl。公開されているもの) と、MercuryDriver 0.88
// (BEL. さん、nenetto さん) のソースと説明から作った:
// - YMF288 は 2 個。クロックは 8.000MHz の発振器 (1 サンプルは 144 クロック、約 55.6kHz)。/COM は GND につながっている
//   ので、$20 の NEW を 1 にすると YMF288 モードになる
// - バスの A1 / A2 が YMF288 の A0 / A1。データは下位バイト (奇数番地) だけ
// - /IRQ がつながっているのはマスターだけ。ボードは /IRQ が出た変わり目で割り込みの要求を覚え、CPU の割り込み応答か
//   リセットで消す (レベルではなく変わり目)。応答ではベクタのレジスタの値を出す
// - 割り込みのレベルはジャンパ (JP1 / JP2) で 2 か 4。ここでは 4 に固定した (INTERRUPT_LEVEL)
// 音は、チップごとに 1 サンプル (約 55.6kHz) ずつ作り、Sound の 62.5kHz のサンプルに直して PCM と一緒に混ぜる。
// 2 つの周波数の比はちょうど 8:9 (4MHz の 72 クロックと 64 クロック) なので、9 相の FIR (窓付きの sinc、1 相 16 タップ) で直す
// (線形補間では、数 kHz の音で関係のない周波数の成分が -40dB ほど出た。mercury.cpp の buildOpnFir())。
// 一度もレジスタに書いていないチップは進めない。リズムの音は Opn3::setRhythm() で渡したものを使う。
//
// 資料から分からず、推測で決めたこと:
// - ボードの中のバッファ (FIFO) の大きさ。ここでは 8 語にした。空きがあれば EXREQ を出す
// - $ECC090 の bit1 (EXREQ を出すか) は見ない (いつも出す。PilederX の FORCE_REQEN、wifipcm の V3 と同じ)
// - 拡張スロットの EXPCL (DMAC のチャネル 2 の PCL。CSR の bit0 の PCS で読める) には、サンプリング周波数の L / R のクロック
//   (1 サンプルの前半が 1、後半が 0) を、データの有無やステレオ / モノラルにかかわらず出す。PCM8PP は PCS が 1 から 0 に
//   変わるのを待ってからコマンドを書くので、クロックが止まっていると止まる。
//   $ECC090 の bit3 にも同じ値を出す。PCT (PCL の変わり目で立つ CSR の bit1) は作っていない
// - 半分の周波数のときも、取り出す速さと出す音の両方が半分になる
// - データが足りなくなったら、前の値を出し続ける (前の値を、もう 1 サンプル取り出したことにする)
// - L / R の出力を止めても、データは同じ速さで取り出す
// Machine が持ち、CPU と DMAC からの書き込みは Sound::mercuryWrite() を通す (音の合成を別のコアで行うときに記録するため)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "x68k/opn3.h"

namespace x68k {

class StateIO;

class Mercury {
public:
    static constexpr uint32_t BASE = 0xecc000; // $ECC000-$ECDFFF の 8KB のうち、$ECC080-$ECC0FF だけ使う
    static constexpr int FIFO = 8;             // バッファの語数 (推測)
    static constexpr int PCM_TAPS = 32;        // PCM を 62.5kHz に直すフィルタのタップの数 (mercury.cpp の kPcmInterp)
    static constexpr int OPN_TAPS = 16;        // YMF288 の音を 62.5kHz に直す FIR の、1 相のタップの数
    static constexpr int INTERRUPT_LEVEL = 4;  // YMF288 の割り込みのレベル (ボードのジャンパで 2 か 4。ここでは 4)

    Mercury();
    void reset();

    // CPU から見えるレジスタ (off は $ECC000 からの位置)。データの上位バイトはここで覚える
    // pendingClocks4M は expcl() と同じ (Sound がまだ advance() に渡していない 4MHz のクロック)
    uint8_t read(uint32_t off, uint32_t pendingClocks4M = 0) const;
    void latchHigh(uint8_t v) { high_ = v; }
    uint8_t latched() const { return high_; }
    // 書き込み (Sound が記録してから呼ぶ)
    void writeData(uint16_t w);
    void writeMode(uint8_t v) { mode_ = v; }
    void writeCommand(uint8_t v) { cmd_ = v; }

    // 4MHz のクロックで進める (Sound が OPM の 1 サンプルごとに呼ぶ)。synth が false なら YMF288 の音は作らず、
    // タイマーとステータスだけ進める (音を別のコアで作るときの、CPU の側)
    void advance(uint32_t clocks4M, bool synth = true) {
        acc_ += clocks4M * rate();
        while (acc_ >= 4000000) {
            acc_ -= 4000000;
            take();
        }
        advanceOpn(clocks4M, synth);
    }

    // ---- YMF288 ----
    // CPU からの書き込み。pendingClocks4M は read() と同じ (BUSY の終わりを決めるのに使う)
    void writeVector(uint8_t v) { vector_ = v; }
    void opnWriteAddress(int chip, int port, uint8_t v, uint32_t pendingClocks4M);
    // データの書き込み。書いた先のレジスタを bank と reg に返す (Sound が記録する)
    void opnWriteData(int chip, uint8_t v, uint32_t pendingClocks4M, int& bank, uint8_t& reg);
    // レジスタに書く (記録を当てる側はこれだけを呼ぶ)
    void opnWriteReg(int chip, int bank, uint8_t reg, uint8_t v);
    // 割り込みの要求 (覚えているもの) と、応答で出すベクタ。ack() で要求を消す
    bool irq() const { return irqLatched_; }
    uint8_t vector() const { return vector_; }
    void ack() { irqLatched_ = false; }
    // マスターの YMF288 のタイマーが次に割り込みを要求するまでの 4MHz のクロック数 (advance() に渡した数で数える)。
    // もう /IRQ が出ているか、見込みがなければ 0
    uint32_t opnClocksToIrq() const { return opn_[0].clocksToIrq(); }
    // YMF288 の今の出力 (2 個の和。FM の 1 スロットの最大が +-8168)。チップのサンプルから 8 サンプル (0.14ms) 遅れる
    int32_t opnLeft() const { return opnOutput(0); }
    int32_t opnRight() const { return opnOutput(1); }
    Opn3& opn(int chip) { return opn_[chip & 1]; }
    // 音を作っている側 (別のコア) の Mercury から、YMF288 の音の状態を写す (ステートを保存する前)。
    // CPU の側だけが持つもの (選んでいるレジスタ、BUSY、ベクタ、割り込みの要求) は変えない
    void copyOpnSynthFrom(const Mercury& synth);
    // EXPCL の今の値 (1 サンプルの前半なら true)。取り出す間隔の端数と、まだ advance() に渡していない 4MHz のクロック
    // (Sound は OPM の 1 サンプル = 64 クロックごとにしか渡さないので、それだけだと 48kHz のクロックが折り返して見える)
    // から決める (状態を増やさない)
    bool expcl(uint32_t pendingClocks4M) const {
        return (uint32_t)(((uint64_t)acc_ + (uint64_t)pendingClocks4M * rate()) % 4000000) < 2000000;
    }
    // 転送の要求 (バッファに空きがある)
    bool drq() const { return count_ < FIFO; }
    // 今の出力 (16 ビット。出さない側は 0)
    // PCM の周波数 (48kHz など) のサンプルを、呼ばれた時点 (Sound の 62.5kHz のサンプル) の値に直して返す
    // (窓をかけた sinc の多相フィルタ。mercury.cpp の pcmOutput())
    int32_t left() const { return (cmd_ & 0x04) ? pcmOutput(0) : 0; }
    int32_t right() const { return (cmd_ & 0x08) ? pcmOutput(1) : 0; }
    // 最後に取り出したサンプル (フィルタを通す前。出さない側は 0)
    int32_t sampleLeft() const { return (cmd_ & 0x04) ? outL_ : 0; }
    int32_t sampleRight() const { return (cmd_ & 0x08) ? outR_ : 0; }
    // サンプリング周波数 (Hz)
    uint32_t rate() const;
    uint8_t command() const { return cmd_; }
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)

private:
    void take();
    void advanceOpn(uint32_t clocks4M, bool synth);
    void updateOpnIrq();
    int32_t opnOutput(int side) const;
    int32_t pcmOutput(int side) const;
    void pushPcm();

    uint8_t cmd_ = 0, mode_ = 0, high_ = 0;
    bool leftNext_ = true; // ステレオで次に書く語が L か (今はどこからも読まない。ステートの並びを変えないために残す)
    uint16_t fifo_[FIFO] = {};
    int head_ = 0, count_ = 0;
    uint32_t acc_ = 0; // 取り出す間隔の端数 (4MHz のクロック x 周波数)
    int32_t outL_ = 0, outR_ = 0;
    // ここまでが版 18 までのステートに入っていた分 (state.cpp の Mercury::state())

    Opn3 opn_[2]; // マスター、スレーブ
    uint32_t opnBusy_[2] = {}; // BUSY が消える時刻 (最後の advance() からのマスタークロックの数)
    uint32_t opnAcc_ = 0;      // YMF288 の次のサンプルまでに進んだ 4MHz のクロック
    uint32_t opnPos_ = 0;      // opnHist_ の中の、いちばん新しいサンプルの位置
    int32_t opnHist_[2][OPN_TAPS] = {}; // 最近のサンプル (L、R)。輪にして使う
    uint8_t vector_ = 0xff;
    bool irqLine_ = false;    // マスターの /IRQ が出ているか (変わり目を見るために覚える)
    bool irqLatched_ = false; // 割り込みの要求
    bool opnUsed_[2] = {};    // レジスタに書いたことがある (ないチップは音を作らない)
    uint8_t opnPad_[3] = {};
    // ここまでが版 20 までのステートに入っていた分

    // PCM を 62.5kHz に直すフィルタの履歴 (L、R)。取り出したサンプルを、同じ値で j と j + PCM_TAPS に書くので、
    // pcmHist_[c] + pcmPos_ からの PCM_TAPS 個が、古いものから順に並ぶ
    int16_t pcmHist_[2][2 * PCM_TAPS] = {};
    uint32_t pcmPos_ = 0;   // 次に書く位置 (0 から PCM_TAPS - 1)
    uint32_t pcmQuiet_ = 0; // 続けて入った 0 のサンプルの数 (PCM_TAPS まで。履歴がすべて 0 なら計算しない)
};

} // namespace x68k
