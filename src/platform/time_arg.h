// --time の値 (RTC を始める時刻の秒数) を読む。pix68k と x68k_boot で共通。
// 10 進の数字だけを受け付ける。空、先頭の空白や符号 (strtoll は読み飛ばす)、数字のあとの文字、
// int64_t に入らない値は false。RTC が表せる範囲 (1980〜2079 年) への丸めは Rtc::setTime() が行う
#pragma once

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

namespace platform {

inline bool parseTimeArg(const char* v, int64_t& out) {
    if (!v || v[0] < '0' || v[0] > '9') return false;
    char* end = nullptr;
    errno = 0;
    const long long t = strtoll(v, &end, 10);
    if (*end != 0 || errno == ERANGE) return false;
    out = (int64_t)t;
    return true;
}

} // namespace platform
