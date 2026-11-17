// pix68k.cfg (platform/config_text.h) と Settings (platform/settings.h) の間の読み書き。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include "platform/config_text.h"
#include "platform/settings.h"

namespace platform {

// text の「キー=値」を順に Settings に入れる。知らないキーと正しくない値は warn(key, value, result) を呼んで飛ばし、
// = のない行は bad(line) を呼ぶ
template <typename Warn, typename Bad>
void readSettings(const ConfigText& text, Settings& s, Warn&& warn, Bad&& bad) {
    text.forEach(
        [&](const ConfigText::Entry& e) {
            SettingResult r = applySetting(s, e.key, e.value);
            if (r != SettingResult::OK) warn((const char*)e.key, (const char*)e.value, r);
        },
        bad);
}

// s のうち base と違う項目を text に書く (行があれば値を書き換え、なければ足す)。
// base は text を読んだときの Settings (読んだあとに変えていない項目は、行に触らない)。
// 書けなければ (値に # がある、メモリが足りない) false
inline bool writeSettings(ConfigText& text, const Settings& s, const Settings& base) {
    bool ok = true;
    for (const SettingKey& k : kSettingKeys) {
        if (sameSetting(s, base, k)) continue;
        char v[Settings::PATH_SIZE];
        if (!formatSetting(s, k, v, sizeof v)) continue;
        ok = text.set(k.key, v) && ok;
    }
    return ok;
}

} // namespace platform
