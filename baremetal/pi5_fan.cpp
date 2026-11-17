#include "pi5_fan.h"

#if RASPPI >= 5

#include "platform/fan_curve.h"

#include <circle/bcm2712.h>
#include <circle/memio.h>
#include <circle/util.h>

namespace {

const char FromFan[] = "fan";

// RP1 の PWM1 のレジスタ (Linux の drivers/pwm/pwm-rp1.c と Circle の lib/pwmoutput-rp1.cpp と同じ番地)。
// Circle の CPWMOutput は使わない: 4 本のチャネルをすべて有効にして位相のレジスタを 0 にする (0x3c は、RP1 が回転数を
// 入れるレジスタと同じ番地) うえ、どれかのチャネルがすでに有効だと assert で止まる。ここではファンのチャネルだけを触る
const unsigned kChannel = 3;     // 0 から数える (device tree の pwms = <&rp1_pwm1 3 ...>)
const unsigned kFanPin = 45;     // RP1 の GPIO45 (FAN_PWM)
const unsigned kClockHz = 50000000;
const unsigned kRange = 2078;    // 41566ns / 20ns (50MHz の 1 クロック) を四捨五入
const uintptr kGlobalCtrl = ARM_PWM1_BASE + 0x00;
const uintptr kChanCtrl = ARM_PWM1_BASE + 0x14 + kChannel * 0x10;
const uintptr kChanRange = ARM_PWM1_BASE + 0x18 + kChannel * 0x10;
const uintptr kChanDuty = ARM_PWM1_BASE + 0x20 + kChannel * 0x10;
const uintptr kRpm = ARM_PWM1_BASE + 0x3c;
const u32 kGlobalUpdate = 1u << 31;                   // 書いた設定を反映する
// Linux の pwm-rp1 と同じ設定 (bit8 FIFO_POP_MASK と bit0 trailing edge) に、bit3 (出力を反転) を足したもの
const u32 kChanMode = (1u << 8) | (1u << 0) | (1u << 3);

} // namespace

void CPi5Fan::Initialize(CLogger& logger, const char* mode, unsigned gpioFanPin) {
    if (gpioFanPin != 0) {
        logger.Write(FromFan, LogNotice, "not controlled here (gpiofanpin=%u: Circle switches the pin)", gpioFanPin);
        return;
    }
    if (strcmp(mode, "off") == 0) {
        logger.Write(FromFan, LogNotice, "not controlled (pix68k_fan=off)");
        return;
    }
    if (strcmp(mode, "full") == 0) {
        m_Full = true;
    } else if (strcmp(mode, "auto") != 0) {
        logger.Write(FromFan, LogWarning, "pix68k_fan=%s: use auto (default), full or off", mode);
    }
    // 始めは全開にする (温度を読むまでの間と、温度が読めないときに止めたままにしない)
    m_Level = platform::FanCurve::kMaxLevel;
    m_Duty = platform::FanCurve::kMaxDuty;
    // StartRate() は PWM1 のクロック (4 つのチャネルで共通) を一度止めて設定し直す。いま PWM1 のクロックを使うものは
    // ほかにない (pix68k_sound=pwm の音は PWM0)。PWM1 のほかのチャネルを使うものを足すときは、ここより後で動かし始めるか、
    // クロックを同じ値 (kClockHz) で使う
    m_pClock = new CGPIOClock(GPIOClockPWM1);
    if (m_pClock != nullptr && m_pClock->StartRate(kClockHz)) {
        // ファームウェアが残した設定 (実機で、何もしないときにファンが回らないわけを調べるため)。
        // PWM1 のレジスタは、クロックを動かしてから読む (Linux も Circle も、クロックを先に動かす)
        logger.Write(FromFan, LogNotice, "PWM1 before: global %X, channel %u ctrl %X range %u duty %u, %u rpm",
                     read32(kGlobalCtrl), kChannel, read32(kChanCtrl), read32(kChanRange), read32(kChanDuty),
                     read32(kRpm));
        write32(kChanCtrl, kChanMode);
        write32(kChanRange, kRange);
        write32(kChanDuty, kRange);
        write32(kGlobalCtrl, read32(kGlobalCtrl) | (1u << kChannel));
        write32(kGlobalCtrl, read32(kGlobalCtrl) | kGlobalUpdate);
        m_pPin = new CGPIOPin(kFanPin, GPIOModeAlternateFunction0);
        if (m_pPin != nullptr) {
            m_pPin->SetPullMode(GPIOPullModeDown); // device tree の rp1_pwm1_gpio45 と同じ (bias-pull-down)
            m_Drive = Drive::Pwm;
        }
    } else {
        // PWM が使えないので、GPIO45 を Low にして全開で回す
        m_pPin = new CGPIOPin(kFanPin, GPIOModeOutput);
        if (m_pPin != nullptr) {
            m_pPin->Write(LOW);
            m_Drive = Drive::OnOff;
        }
    }
    if (m_Drive == Drive::None) {
        m_Level = 0;
        m_Duty = 0;
        logger.Write(FromFan, LogError, "cannot set up GPIO%u (out of memory)", kFanPin);
        return;
    }
    logger.Write(FromFan, m_Drive == Drive::Pwm ? LogNotice : LogWarning, "%s on GPIO%u (active low), %s",
                 m_Drive == Drive::Pwm ? "PWM1 channel 3, 24 kHz" : "PWM1 clock failed; on/off only", kFanPin,
                 m_Full ? "always full speed (pix68k_fan=full)" : "speed follows the temperature (50/60/67.5/75 C)");
}

void CPi5Fan::SetTemperature(unsigned milliC) {
    if (m_Drive == Drive::None || m_Full) return;
    const int level = platform::FanCurve::next(m_Level, milliC);
    if (level == m_Level) return;
    m_Level = level;
    SetDuty(platform::FanCurve::duty(level));
}

void CPi5Fan::SetDuty(unsigned duty) {
    m_Duty = duty;
    if (m_Drive == Drive::Pwm) {
        // 出力を反転しているので、duty が kRange なら GPIO45 はずっと Low (全開)、0 ならずっと High (止まる)
        write32(kChanDuty, platform::FanCurve::scale(duty, kRange));
        write32(kGlobalCtrl, read32(kGlobalCtrl) | kGlobalUpdate);
    } else if (m_Drive == Drive::OnOff) {
        m_pPin->Write(duty != 0 ? LOW : HIGH);
    }
}

unsigned CPi5Fan::rpm() const { return m_Drive == Drive::Pwm ? read32(kRpm) : 0; }

#endif // RASPPI >= 5
