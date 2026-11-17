// RTC (RP5C15)。$E8A001 から奇数番地に 16 個のレジスタ (下位 4 ビット) が並ぶ。
// 実際のチップと同じく、日時を桁ごとの 4 ビットのカウンタで持ち、1 秒ごとに桁上がりさせる。
// 起動時の日時は環境 (ホスト / Pi) が setTime() で渡す。
#pragma once

#include <stdint.h>

namespace x68k {

class StateIO;

class Rtc {
public:
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)
    // 桁のレジスタの番号 (バンク 0)
    enum Digit : int {
        SEC1 = 0, SEC10, MIN1, MIN10, HOUR1, HOUR10, WDAY, DAY1, DAY10, MON1, MON10, YEAR1, YEAR10,
    };

    void reset();
    // 1970-01-01 00:00:00 からの秒数 (地方時) を桁に入れる。
    // RTC は 1980〜2079 年しか表せないので、範囲外はその端に丸める
    void setTime(int64_t unixSeconds);
    // 1 秒進める。モードレジスタの bit3 (計時許可) が 0 の間は進まない
    void tickSecond();

    uint8_t read(int reg);
    void write(int reg, uint8_t v);

    // アラーム出力 (MFP の GPIP0)。1Hz / 16Hz のクロック出力は未実装
    bool alarmOut() const { return true; }

private:
    int value(int lo) const { return digit_[lo + 1] * 10 + digit_[lo]; }
    void setValue(int lo, int v) {
        digit_[lo] = (uint8_t)(v % 10);
        digit_[lo + 1] = (uint8_t)(v / 10);
    }
    uint8_t digit_[13] = {};  // 桁のカウンタ (秒の 1 の位〜年の 10 の位)
    uint8_t mode_ = 0x08;     // レジスタ D: bit0 バンク、bit2 アラーム許可、bit3 計時許可
    uint8_t test_ = 0;
    uint8_t resetReg_ = 0;
    uint8_t bank1_[16] = {}; // アラームなど
};

} // namespace x68k
