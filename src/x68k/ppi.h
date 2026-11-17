// 8255 (PPI、$E9A001-$E9A007)。モード 0 だけ作る。
//   $E9A001 ポート A: ジョイスティック 1 (読むだけ)
//   $E9A003 ポート B: ジョイスティック 2 (読むだけ)
//   $E9A005 ポート C: 書くと出力 (読むと書いた値)
//     bit7 ジョイスティック 1 のトリガ B を L にする、bit6 トリガ A を L にする、
//     bit5 ジョイスティック 2 の 8 番ピン、bit4 ジョイスティック 1 の 8 番ピン (ストローブ。ここでは見ない)、
//     bit3-2 ADPCM のサンプリング周波数の選択、bit1-0 ADPCM の出力 (bit0 = 1 で右を切る、bit1 = 1 で左を切る)
//   $E9A007 制御: bit7 = 1 ならモードの設定 (出力のラッチは 0 になる)、0 ならポート C のビットセット /
//           リセット (bit3-1 がビット番号、bit0 が値)
//
// ジョイスティックのポート (ATARI 規格): 押すと 0。bit0 上、bit1 下、bit2 左、bit3 右、bit5 トリガ A、
// bit6 トリガ B。bit4 と bit7 はつながっていないものとして 1 を読む (実機では確かめていない)。
// ポート C の bit6 / bit7 を 1 にすると、ジョイスティック 1 のトリガの線が L になる (押したのと同じに読める)。
//
// ポート C の bit4 / bit5 (ジョイスティック 1 / 2 の 8 番ピン) は、書いた値を覚えるだけで、ポート A / B の読める値には
// 使わない (ATARI 規格の 2 ボタンのパッドは 8 番ピンを見ない)。8 番ピンで信号を切り替えるパッドは作っていない。
//
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stdint.h>

namespace x68k {

class StateIO;

class Ppi {
public:
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)
    // setJoystick() の bits (押していれば 1。platform::InputSink と同じ)
    enum Joy : uint8_t { JOY_UP = 0x01, JOY_DOWN = 0x02, JOY_LEFT = 0x04, JOY_RIGHT = 0x08, JOY_A = 0x10, JOY_B = 0x20 };

    void reset();
    uint8_t read(uint32_t offset);
    void write(uint32_t offset, uint8_t v);

    // ジョイスティックの状態 (port は 0 か 1、bits は JOY_* の組み合わせ)
    void setJoystick(int port, uint8_t bits);
    // ジョイスティックの状態をポートの値 (押すと 0) に変える
    static uint8_t portValue(uint8_t bits);

    uint8_t portC() const { return portC_; }
    uint8_t control() const { return control_; }
    // ADPCM の出力の選択 (ポート C の bit1-0) とサンプリング周波数の選択 (bit3-2)
    uint8_t adpcmPan() const { return portC_ & 3; }
    uint8_t adpcmRate() const { return (portC_ >> 2) & 3; }

private:
    uint8_t joy_[2] = {0, 0};
    uint8_t portC_ = 0;
    uint8_t control_ = 0x9b; // リセット後はすべて入力
};

} // namespace x68k
