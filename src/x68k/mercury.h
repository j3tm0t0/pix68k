// まーきゅりーゆにっと (Mercury Unit)。16 ビットのステレオ PCM を出す拡張ボード ($ECC080-)。
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
// $ECC080-$ECC0FF のほかの場所 (OPN の YMF288 など) は作っていない: 読むと $FF、書いても何も起きない。
//
// 資料から分からず、推測で決めたこと:
// - ボードの中のバッファ (FIFO) の大きさ。ここでは 8 語にした。空きがあれば EXREQ を出す
// - $ECC090 の bit1 (EXREQ を出すか) は見ない (いつも出す。PilederX の FORCE_REQEN、wifipcm の V3 と同じ)
// - 拡張スロットの EXPCL (DMAC のチャネル 2 の PCL。CSR の bit0 の PCS で読める) には、サンプリング周波数の L / R のクロック
//   (1 サンプルの前半が 1、後半が 0) を、データの有無やステレオ / モノラルにかかわらず出す。PCM8PP は PCS が 1 から 0 に
//   変わるのを待ってからコマンドを書くので、クロックが止まっていると止まる。
//   $ECC090 の bit3 にも同じ値を出す。PCT (PCL の変わり目で立つ CSR の bit1) は作っていない
// - 半分の周波数のときも、取り出す速さと出す音の両方が半分になる
// - データが足りなくなったら、前の値を出し続ける
// - L / R の出力を止めても、データは同じ速さで取り出す
// - $ECC0B1 の割り込みベクタと、ボードからの割り込みは作っていない
// Machine が持ち、CPU と DMAC からの書き込みは Sound::mercuryWrite() を通す (音の合成を別のコアで行うときに記録するため)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace x68k {

class StateIO;

class Mercury {
public:
    static constexpr uint32_t BASE = 0xecc000; // $ECC000-$ECDFFF の 8KB のうち、$ECC080-$ECC0FF だけ使う
    static constexpr int FIFO = 8;             // バッファの語数 (推測)

    Mercury() { reset(); }
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

    // 4MHz のクロックで進める (Sound が OPM の 1 サンプルごとに呼ぶ)
    void advance(uint32_t clocks4M) {
        acc_ += clocks4M * rate();
        while (acc_ >= 4000000) {
            acc_ -= 4000000;
            take();
        }
    }
    // EXPCL の今の値 (1 サンプルの前半なら true)。取り出す間隔の端数と、まだ advance() に渡していない 4MHz のクロック
    // (Sound は OPM の 1 サンプル = 64 クロックごとにしか渡さないので、それだけだと 48kHz のクロックが折り返して見える)
    // から決める (状態を増やさない)
    bool expcl(uint32_t pendingClocks4M) const {
        return (uint32_t)(((uint64_t)acc_ + (uint64_t)pendingClocks4M * rate()) % 4000000) < 2000000;
    }
    // 転送の要求 (バッファに空きがある)
    bool drq() const { return count_ < FIFO; }
    // 今の出力 (16 ビット。出さない側は 0)
    int32_t left() const { return (cmd_ & 0x04) ? outL_ : 0; }
    int32_t right() const { return (cmd_ & 0x08) ? outR_ : 0; }
    // サンプリング周波数 (Hz)
    uint32_t rate() const;
    uint8_t command() const { return cmd_; }
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)

private:
    void take();

    uint8_t cmd_ = 0, mode_ = 0, high_ = 0;
    bool leftNext_ = true; // ステレオで次に書く語が L か (今はどこからも読まない。ステートの並びを変えないために残す)
    uint16_t fifo_[FIFO] = {};
    int head_ = 0, count_ = 0;
    uint32_t acc_ = 0; // 取り出す間隔の端数 (4MHz のクロック x 周波数)
    int32_t outL_ = 0, outR_ = 0;
};

} // namespace x68k
