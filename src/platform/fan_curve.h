// Raspberry Pi 5 のファン (Active Cooler、Case Fan) の速さを、SoC の温度から決める。
//
// 段階と温度は Raspberry Pi の Linux の device tree (arch/arm64/boot/dts/broadcom/bcm2712-rpi-5-b.dts) と同じにする:
//   段階    0    1     2     3      4
//   上げる  -    50    60    67.5   75 度以上で、この段階にする
//   下げる       45    55    62.5   70 度未満で、1 つ下の段階にする (5 度のヒステリシス)
//   速さ    0    75    125   175    250 (255 が全開。pwm-fan の cooling-levels)
// 温度が読めないとき (0) は、いちばん上の段階にする (止めたままにしない)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stdint.h>

namespace platform {

struct FanCurve {
    static constexpr int kLevels = 5;
    static constexpr int kMaxLevel = kLevels - 1;
    static constexpr unsigned kMaxDuty = 255;
    static constexpr unsigned kHysteresisMilliC = 5000;

    // 段階 level (1 から kMaxLevel) に上げる温度 (1/1000 度)
    static constexpr unsigned onMilliC(int level) {
        return level == 1 ? 50000u : level == 2 ? 60000u : level == 3 ? 67500u : 75000u;
    }
    // 段階の速さ (0 から kMaxDuty)
    static constexpr unsigned duty(int level) {
        return level <= 0 ? 0u : level == 1 ? 75u : level == 2 ? 125u : level == 3 ? 175u : 250u;
    }
    // 今の段階 level と温度 milliC (1/1000 度。0 は読めなかった) から、次の段階を返す
    static constexpr int next(int level, unsigned milliC) {
        if (milliC == 0) return kMaxLevel;
        if (level < 0) level = 0;
        if (level > kMaxLevel) level = kMaxLevel;
        while (level < kMaxLevel && milliC >= onMilliC(level + 1)) level++;
        while (level > 0 && milliC < onMilliC(level) - kHysteresisMilliC) level--;
        return level;
    }
    // 速さ duty (0 から kMaxDuty) を、PWM の 1 周期の長さ range に合わせた値にする (四捨五入。全開は range)
    static constexpr unsigned scale(unsigned duty, unsigned range) {
        return duty >= kMaxDuty ? range : (range * duty + kMaxDuty / 2) / kMaxDuty;
    }
};

} // namespace platform
