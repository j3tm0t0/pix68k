// pix68k.cfg (platform/config_text.h) と Settings (platform/settings.h) の間の読み書き。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include "platform/config_text.h"
#include "platform/settings.h"

namespace platform {

// text の「キー=値」を順に Settings に入れる。知らないキーと正しくない値は warn(key, value, result) を呼んで飛ばし、
// = のない行は bad(line) を呼ぶ。v0.1.0 の書き方 (scale=、scanline=) は読み終えてから読み替える (resolveLegacySettings())
template <typename Warn, typename Bad>
void readSettings(const ConfigText& text, Settings& s, Warn&& warn, Bad&& bad) {
    text.forEach(
        [&](const ConfigText::Entry& e) {
            SettingResult r = applySetting(s, e.key, e.value);
            if (r != SettingResult::OK) warn((const char*)e.key, (const char*)e.value, r);
        },
        bad);
    resolveLegacySettings(s);
}

// 前の書き方の行 (v0.1.0 の scale= と scanline=。isLegacySettingKey()) を消し、代わりに video_scale= と video_filter= を
// values の値で書く (保存するときに呼ぶ)。values は text を読んだときの値 (起動の組み合わせのファイルなら、pix68k.cfg と
// 重ねて読んだ値)。scale= があれば video_scale=、scanline= があれば video_filter= を書く (行があれば最後の行の値を
// 書き換え、なければ足す)。前の書き方の行がなければ何もしない。使えなくなった crt= の行は触らない。
// ホスト版 (values.legacyVideo が 0) は、v0.1.0 と同じく scale= と scanline= をベアメタル版の項目として残す。
// 書き換えたら 1、前の書き方の行がなければ 0、書けなければ -1 を返す
inline int migrateLegacySettings(ConfigText& text, const Settings& values) {
    if (!values.legacyVideo) return 0;
    bool scale = false, filter = false;
    text.forEach(
        [&](const ConfigText::Entry& e) {
            if (strcmp(e.key, "scale") == 0) scale = true;
            if (strcmp(e.key, "scanline") == 0) filter = true;
        },
        [](const char*) {});
    if (!scale && !filter) return 0;
    text.removeLines("scale");
    text.removeLines("scanline");
    bool ok = true;
    if (scale) ok = text.set("video_scale", videoScaleName(values.videoScale)) && ok;
    if (filter) ok = text.set("video_filter", videoFilterName(values.videoFilter)) && ok;
    return ok ? 1 : -1;
}

// s のうち base と違う項目を text に書く (行があれば値を書き換え、なければ足す)。
// base は text を読んだときの Settings (読んだあとに変えていない項目は、行に触らない)。前の書き方の行は
// いまの書き方にする (migrateLegacySettings())。
// 書けなければ (値に # がある、メモリが足りない) false
inline bool writeSettings(ConfigText& text, const Settings& s, const Settings& base) {
    bool ok = migrateLegacySettings(text, base) >= 0;
    for (const SettingKey& k : kSettingKeys) {
        if (sameSetting(s, base, k)) continue;
        char v[Settings::PATH_SIZE];
        if (!formatSetting(s, k, v, sizeof v)) continue;
        ok = text.set(k.key, v) && ok;
    }
    return ok;
}

} // namespace platform
