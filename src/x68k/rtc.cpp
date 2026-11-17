#include "x68k/rtc.h"

namespace x68k {

namespace {

// 書き込める桁のビット (10 の位は桁によってビット数が少ない)
const uint8_t kDigitMask[13] = {0x0f, 0x07, 0x0f, 0x07, 0x0f, 0x03, 0x07, 0x0f, 0x03, 0x0f, 0x01, 0x0f, 0x0f};

// 通算日から日付 (Howard Hinnant の civil_from_days)
void civilFromDays(int64_t days, int& year, int& month, int& day) {
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    day = (int)(doy - (153 * mp + 2) / 5 + 1);
    month = (int)(mp < 10 ? mp + 3 : mp - 9);
    year = (int)(yoe + era * 400 + (month <= 2 ? 1 : 0));
}

// 月の日数。year は 1980 年を 0 とする年 (RTC は 4 で割り切れる年をうるう年とする)
int daysInMonth(int year, int month) {
    static const uint8_t kDays[13] = {31, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && year % 4 == 0) return 29;
    return (month >= 1 && month <= 12) ? kDays[month] : 31;
}

} // namespace

void Rtc::reset() {
    mode_ = 0x08; // 計時許可 (電池で動き続けている状態)
    test_ = 0;
    resetReg_ = 0;
    for (int i = 0; i < 16; i++) bank1_[i] = 0;
    bank1_[10] = 1; // 24 時間制
}

void Rtc::setTime(int64_t unixSeconds) {
    const int64_t min = 315532800;  // 1980-01-01 00:00:00
    const int64_t max = 3471292799; // 2079-12-31 23:59:59
    if (unixSeconds < min) unixSeconds = min;
    if (unixSeconds > max) unixSeconds = max;
    int64_t days = unixSeconds / 86400;
    int64_t rem = unixSeconds % 86400;
    int year, month, day;
    civilFromDays(days, year, month, day);
    setValue(SEC1, (int)(rem % 60));
    setValue(MIN1, (int)(rem / 60 % 60));
    setValue(HOUR1, (int)(rem / 3600));
    digit_[WDAY] = (uint8_t)((days + 4) % 7); // 1970-01-01 は木曜
    setValue(DAY1, day);
    setValue(MON1, month);
    setValue(YEAR1, year - 1980);
}

void Rtc::tickSecond() {
    if (!(mode_ & 0x08)) return;
    int s = value(SEC1) + 1;
    if (s < 60) {
        setValue(SEC1, s);
        return;
    }
    setValue(SEC1, 0);
    int m = value(MIN1) + 1;
    if (m < 60) {
        setValue(MIN1, m);
        return;
    }
    setValue(MIN1, 0);
    int h = value(HOUR1) + 1;
    if (h < 24) {
        setValue(HOUR1, h);
        return;
    }
    setValue(HOUR1, 0);
    digit_[WDAY] = (uint8_t)((digit_[WDAY] + 1) % 7);
    int year = value(YEAR1);
    int month = value(MON1);
    int d = value(DAY1) + 1;
    if (d <= daysInMonth(year, month)) {
        setValue(DAY1, d);
        return;
    }
    setValue(DAY1, 1);
    if (++month <= 12) {
        setValue(MON1, month);
        return;
    }
    setValue(MON1, 1);
    setValue(YEAR1, (year + 1) % 100);
}

uint8_t Rtc::read(int reg) {
    reg &= 15;
    uint8_t v = 0;
    if (reg == 13) v = mode_;
    else if (reg == 14) v = test_;
    else if (reg == 15) v = resetReg_;
    else if (mode_ & 1) {
        if (reg == 11) v = (uint8_t)(value(YEAR1) & 3); // うるう年からの年数
        else v = bank1_[reg];
    } else {
        v = digit_[reg];
    }
    // 上位 4 ビットはデータバスにつながっていない。実機では不定だが 1 とする
    return (uint8_t)(0xf0 | (v & 0x0f));
}

void Rtc::write(int reg, uint8_t v) {
    reg &= 15;
    v &= 0x0f;
    if (reg == 13) mode_ = v & 0x0d;
    else if (reg == 14) test_ = v;
    else if (reg == 15) resetReg_ = v & 0x0c;
    else if (mode_ & 1) bank1_[reg] = v;
    else digit_[reg] = v & kDigitMask[reg];
}

} // namespace x68k
