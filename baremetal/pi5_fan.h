// Raspberry Pi 5 のファンのコネクタ (Active Cooler、Case Fan) を回す。Pi 5 だけ (ほかの機種のビルドには入らない)。
//
// Linux では pwm-fan ドライバが回すが、ベアメタルでは誰も回さないので、ここで回す。つなぎ方は Raspberry Pi の Linux の
// device tree (arch/arm64/boot/dts/broadcom/bcm2712-rpi-5-b.dts の cooling_fan と rp1_pwm1) に合わせる:
//   - ファンの PWM (FAN_PWM) は RP1 の GPIO45。機能は alt0 (PWM1 のチャネル 3。チャネルは 0 から 3 の 4 本)
//   - 極性は反転 (PWM_POLARITY_INVERTED)。GPIO45 が Low の間が「回す」。Circle の CCPUThrottle (gpiofanpin=45) も
//     Low で回している
//   - 周期は 41566ns (約 24kHz)。PWM1 のクロックは 50MHz なので、1 周期は 2078 クロック
//   - 回転数は RP1 が PWM1 のレジスタの 0x3c に入れる (pwm-fan の rpm-regmap / rpm-offset)
// 速さは温度から決める (platform/fan_curve.h。Linux と同じ 50 / 60 / 67.5 / 75 度の 4 段階)。
// PWM1 のクロックを動かせなければ、GPIO45 をふつうの出力にして、回す (Low) か止める (High) かだけにする。
#pragma once

#if RASPPI >= 5

#include <circle/gpioclock.h>
#include <circle/gpiopin.h>
#include <circle/logger.h>
#include <circle/types.h>

class CPi5Fan {
public:
    enum class Drive { None, Pwm, OnOff }; // None: 動かしていない (pix68k_fan=off か、gpiofanpin= がある)

    // mode: cmdline.txt の pix68k_fan= ("auto" 温度で変える、"full" いつも全開、"off" 触らない)。
    // gpioFanPin: cmdline.txt の gpiofanpin= (0 でなければ Circle の CCPUThrottle が GPIO を動かすので、触らない)
    void Initialize(CLogger& logger, const char* mode, unsigned gpioFanPin);
    // 温度 (1/1000 度。0 は読めなかった) から速さを決めて、変わっていれば設定する
    void SetTemperature(unsigned milliC);

    Drive drive() const { return m_Drive; }
    int level() const { return m_Level; }    // 0 (止める) から 4
    unsigned duty() const { return m_Duty; } // 0 から 255
    unsigned rpm() const;                    // RP1 が測った回転数 (PWM で動かしていなければ 0)

private:
    void SetDuty(unsigned duty);

    Drive m_Drive = Drive::None;
    bool m_Full = false; // pix68k_fan=full
    int m_Level = 0;
    unsigned m_Duty = 0;
    CGPIOClock* m_pClock = nullptr;
    CGPIOPin* m_pPin = nullptr;
};

#endif // RASPPI >= 5
