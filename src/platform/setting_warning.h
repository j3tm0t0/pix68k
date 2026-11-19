// pix68k.cfg の読めなかった行を知らせるログの文言を作る (ベアメタル版の CKernel::LoadSettings() とホスト版が使う)。
// net_token が書いてある行は、値を出さずに「ignored a line about net_token」とだけ書く。ベアメタル版のログは
// ネットワークの口の /log からトークンなしで読めるため。キーに net_token が入っているかも見る
// (「net_token s3cret=」と書き間違えると、= より前がキーになる)。
// ベアメタルでも使えるよう、例外、RTTI、STL、stdio は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <string.h>

#include "platform/net_config.h"
#include "platform/settings.h"

namespace platform {

namespace setting_warning_detail {
// out (size バイト) の pos から s を足す。入りきらない分は捨てる。いつも NUL で終える
inline size_t append(char* out, size_t size, size_t pos, const char* s) {
    if (size == 0) return 0;
    while (*s && pos + 1 < size) out[pos++] = *s++;
    out[pos] = 0;
    return pos;
}
} // namespace setting_warning_detail

static constexpr const char* SECRET_LINE_WARNING = "ignored a line about net_token";

// readSettings() の 1 つ目の呼び出し (キーが不明か、値が読めないか、前の書き方) のときの文言
inline void formatSettingWarning(char* out, size_t size, const char* key, const char* value, SettingResult r) {
    using setting_warning_detail::append;
    if (mentionsSecretSetting(key)) {
        append(out, size, 0, SECRET_LINE_WARNING);
    } else if (r == SettingResult::UNKNOWN_KEY) {
        append(out, size, append(out, size, 0, "unknown key: "), key);
    } else if (r == SettingResult::LEGACY) {
        // 前の書き方 (読み替えて使う)。代わりのキーを出す
        size_t n = append(out, size, 0, key);
        n = append(out, size, n, "=");
        n = append(out, size, n, value);
        n = append(out, size, n, strcmp(key, "scale") == 0 ? " is old; use video_scale=" : " is old; use video_filter=");
        append(out, size, n, " (Save settings rewrites it)");
    } else if (r == SettingResult::REMOVED) {
        // 使えなくなったキー (crt=)。読み替えずに無視する
        size_t n = append(out, size, 0, key);
        n = append(out, size, n, "=");
        n = append(out, size, n, value);
        append(out, size, n, " is no longer supported; use video_filter= and video_scale= (ignored)");
    } else if (r == SettingResult::IGNORED_NAME) {
        size_t n = append(out, size, 0, key);
        n = append(out, size, n, "=");
        n = append(out, size, n, value);
        append(out, size, n, ": unknown names ignored");
    } else if (r == SettingResult::EXCLUSIVE_NAME) {
        size_t n = append(out, size, 0, key);
        n = append(out, size, n, "=");
        n = append(out, size, n, value);
        append(out, size, n, ": only one bezel can be used; using the last one");
    } else {
        size_t n = append(out, size, 0, key);
        n = append(out, size, n, "=");
        n = append(out, size, n, value);
        append(out, size, n, ": bad value; ignored");
    }
}

// readSettings() の 2 つ目の呼び出し (「キー=値」の形でない行) のときの文言
inline void formatIgnoredLine(char* out, size_t size, const char* line) {
    using setting_warning_detail::append;
    if (mentionsSecretSetting(line))
        append(out, size, 0, SECRET_LINE_WARNING);
    else
        append(out, size, append(out, size, 0, "ignored: "), line);
}

} // namespace platform
