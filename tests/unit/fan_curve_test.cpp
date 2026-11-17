// src/platform/fan_curve.h のテスト: 温度から決めるファンの段階 (上げる温度、下げる温度、読めないとき) と速さ
#include "platform/fan_curve.h"
#include "test_check.h"

using platform::FanCurve;

int main() {
    // 上げる: 50 / 60 / 67.5 / 75 度以上
    CHECK_EQ(FanCurve::next(0, 49999), 0);
    CHECK_EQ(FanCurve::next(0, 50000), 1);
    CHECK_EQ(FanCurve::next(1, 59999), 1);
    CHECK_EQ(FanCurve::next(1, 60000), 2);
    CHECK_EQ(FanCurve::next(2, 67499), 2);
    CHECK_EQ(FanCurve::next(2, 67500), 3);
    CHECK_EQ(FanCurve::next(3, 74999), 3);
    CHECK_EQ(FanCurve::next(3, 75000), 4);
    CHECK_EQ(FanCurve::next(4, 90000), 4);
    // 一度に何段階も上がる
    CHECK_EQ(FanCurve::next(0, 68000), 3);
    CHECK_EQ(FanCurve::next(0, 80000), 4);

    // 下げる: 上げた温度より 5 度下がるまで、そのまま
    CHECK_EQ(FanCurve::next(1, 45000), 1);
    CHECK_EQ(FanCurve::next(1, 44999), 0);
    CHECK_EQ(FanCurve::next(2, 55000), 2);
    CHECK_EQ(FanCurve::next(2, 54999), 1);
    CHECK_EQ(FanCurve::next(3, 62500), 3);
    CHECK_EQ(FanCurve::next(3, 62499), 2);
    CHECK_EQ(FanCurve::next(4, 70000), 4);
    CHECK_EQ(FanCurve::next(4, 69999), 3);
    // 一度に何段階も下がる
    CHECK_EQ(FanCurve::next(4, 30000), 0);
    CHECK_EQ(FanCurve::next(4, 56000), 2);
    // 48 度の前後を行き来しても、段階は変わらない
    CHECK_EQ(FanCurve::next(0, 48000), 0);
    CHECK_EQ(FanCurve::next(1, 48000), 1);

    // 温度が読めないときは全開。範囲の外の段階は直す
    CHECK_EQ(FanCurve::next(0, 0), FanCurve::kMaxLevel);
    CHECK_EQ(FanCurve::next(-3, 40000), 0);
    CHECK_EQ(FanCurve::next(9, 80000), FanCurve::kMaxLevel);
    CHECK_EQ(FanCurve::next(9, 30000), 0);

    // 速さ (Linux の pwm-fan の cooling-levels)
    CHECK_EQ(FanCurve::duty(0), 0);
    CHECK_EQ(FanCurve::duty(1), 75);
    CHECK_EQ(FanCurve::duty(2), 125);
    CHECK_EQ(FanCurve::duty(3), 175);
    CHECK_EQ(FanCurve::duty(4), 250);
    // PWM の 1 周期 (2078) に合わせる
    CHECK_EQ(FanCurve::scale(0, 2078), 0);
    CHECK_EQ(FanCurve::scale(75, 2078), 611);
    CHECK_EQ(FanCurve::scale(250, 2078), 2037);
    CHECK_EQ(FanCurve::scale(125, 2078), 1019); // 1018.6 を四捨五入 (切り捨てなら 1018)
    CHECK_EQ(FanCurve::scale(255, 2078), 2078);
    CHECK_EQ(FanCurve::scale(999, 2078), 2078);
    return finish();
}
