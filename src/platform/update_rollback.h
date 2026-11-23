// 更新 (#369) で残した前の版に戻せるかの判定 (設定メニューと起動メニューの「Roll back」)。pix68kcli update --rollback と同じ条件
// (tools/pix68kcli/update_apply.go)。ここでは書かない。書き換えは baremetal/config_switch.h の SwitchConfigKernel() だけ。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform/config_kernel.h"

namespace platform {

constexpr size_t UPDATE_TXT_MAX = 4096;

struct RollbackPlan {
    char from[24] = {}, to[24] = {};  // config.txt の kernel= の、今の値と戻す先
    char sha256[65] = {};             // 戻す先のファイル (更新したときの値。書く前に確かめる)
    char version[40] = {};            // 戻す先の版
    bool running = false;             // 戻す先の版が、今動いている (更新のあと、まだ再起動していない)
};

namespace rollback_detail {
// 「key=値」の行の値を out に入れる。ない、2 つある、長すぎる、使えない字がある、のどれかなら false
inline bool value(const char* text, size_t n, const char* key, char* out, size_t size) {
    const size_t kn = strlen(key);
    bool found = false;
    for (size_t pos = 0; pos < n;) {
        size_t eol = pos;
        while (eol < n && text[eol] != '\n') eol++;
        size_t end = eol;
        if (end > pos && text[end - 1] == '\r') end--;
        if (end - pos > kn && text[pos + kn] == '=' && memcmp(text + pos, key, kn) == 0) {
            const size_t len = end - pos - kn - 1;
            if (found || len == 0 || len >= size) return false;
            for (size_t i = 0; i < len; i++) {
                const char c = text[pos + kn + 1 + i];
                if (!config_kernel_detail::isNameChar(c) && c != '+') return false;
                out[i] = c;
            }
            out[len] = 0;
            found = true;
        }
        pos = eol + 1;
    }
    return found;
}
} // namespace rollback_detail

// 戻せるなら nullptr を返して plan に入れる。戻せなければ理由。config は config.txt、update は /pix68k/update.txt、
// version と commit は今動いている版
inline const char* planRollback(const char* config, size_t configSize, const char* update, size_t updateSize,
                                const char* const* sections, unsigned raspi, const char* version, const char* commit,
                                RollbackPlan& plan) {
    using rollback_detail::value;
    if (sections == nullptr) return "not supported on this model";
    const ConfigKernel k = findConfigKernel(config, configSize, sections);
    if (k.error != nullptr) return k.error;
    RollbackPlan p;
    if (k.length >= sizeof p.from) return "config.txt: not a kernel name used for updates";
    memcpy(p.from, config + k.offset, k.length);
    const char* other = otherKernelName(raspi, p.from);
    if (other == nullptr) return "config.txt: not a kernel name used for updates";
    char current[24], currentVersion[65], previousCommit[65], previousVersion[65];
    if (updateSize > UPDATE_TXT_MAX || !value(update, updateSize, "previous", p.to, sizeof p.to) ||
        !value(update, updateSize, "current", current, sizeof current) ||
        !value(update, updateSize, "previous_version", previousVersion, sizeof previousVersion) ||
        !value(update, updateSize, "previous_commit", previousCommit, sizeof previousCommit) ||
        !value(update, updateSize, "current_version", currentVersion, sizeof currentVersion) ||
        !value(update, updateSize, "previous_sha256", p.sha256, sizeof p.sha256) || strlen(p.sha256) != 64)
        return "cannot read update.txt";
    for (const char* c = p.sha256; *c; c++)
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'))) return "cannot read update.txt";
    if (strcmp(p.to, other) != 0 || strcmp(current, p.from) != 0) return "no previous version";
    // 更新した版か、更新のあと再起動していない前の版が動いているときだけ (LAN から起動したカーネルなどでは断る)
    p.running = strcmp(version, previousVersion) == 0 && strcmp(commit, previousCommit) == 0;
    if (!p.running && strcmp(version, currentVersion) != 0) return "the running version is not the updated one";
    size_t offset = 0;
    if (const char* e = planKernelSwitch(config, configSize, sections, raspi, p.from, p.to, offset)) return e;
    const size_t vn = strlen(previousVersion) < sizeof p.version ? strlen(previousVersion) : sizeof p.version - 1;
    memcpy(p.version, previousVersion, vn);
    plan = p;
    return nullptr;
}

} // namespace platform
