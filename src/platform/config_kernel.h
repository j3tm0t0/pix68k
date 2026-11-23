// config.txt の kernel= の値だけを書き換える (POST /config/kernel、baremetal/host_net.cpp。#369)。
// 書き換えるのは、この機種が使う kernel= の値で、決まった 2 つの名前 (kernel8.img と pix68k8.img など。長さが同じ) の
// 入れ替えだけ。config.txt の大きさが変わらないので、値のある 1 セクタを直接書き、FAT もディレクトリも書かない
// (改名で置き換えると、改名の間に電源が切れたときに config.txt がなくなる)。
// その 1 セクタを書いている最中に切れたときに、前の中身か新しい中身のどちらかが残るかは、SD カード次第。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。tools/pix68kcli/update.go にも同じ読み方がある
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace platform {

constexpr size_t CONFIG_TXT_MAX = 16 * 1024; // 読む config.txt の上限
constexpr size_t CONFIG_SECTOR = 512;

namespace config_kernel_detail {
inline bool equal(const char* s, size_t n, const char* want) { return strlen(want) == n && memcmp(s, want, n) == 0; }
inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r'; }
inline bool isNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}
// 機種を選ぶ見出しか (Raspberry Pi の config.txt の文書にあるもの。[all] と [none] は別に扱う)
inline bool isModelFilter(const char* s, size_t n) {
    static const char* const kModels[] = {"pi0",  "pi0w", "pi02",  "pi1", "pi2", "pi3", "pi3+", "pi4",  "pi400", "pi5",
                                          "pi500", "cm0", "cm1", "cm3", "cm3+", "cm4", "cm4s", "cm5"};
    for (const char* m : kModels)
        if (equal(s, n, m)) return true;
    return false;
}
} // namespace config_kernel_detail

// この機種が使う名前の組の、もう一方。組にない名前なら nullptr (raspi は 2〜5)
inline const char* otherKernelName(unsigned raspi, const char* name) {
    static const struct {
        unsigned raspi;
        const char *a, *b;
    } kPairs[] = {{2, "kernel7.img", "pix68k7.img"},
                  {3, "kernel8.img", "pix68k8.img"},
                  {4, "kernel8-rpi4.img", "pix68k8-rpi4.img"},
                  {5, "kernel_2712.img", "pix68k_2712.img"}};
    for (const auto& p : kPairs) {
        if (p.raspi != raspi) continue;
        if (strcmp(name, p.a) == 0) return p.b;
        if (strcmp(name, p.b) == 0) return p.a;
    }
    return nullptr;
}

struct ConfigKernel {
    const char* error = nullptr; // 決められない理由 (HTTP の応答に出す)。なければ nullptr
    size_t offset = 0, length = 0; // kernel= の値
};

// text (n バイト) の中で、この機種 (sections: 当てはまる見出しの並び。nullptr で終わる) が使う kernel= の値を探す。
// 使うものを決められない書き方なら error にする
inline ConfigKernel findConfigKernel(const char* text, size_t n, const char* const* sections) {
    using namespace config_kernel_detail;
    ConfigKernel out;
    bool active = true, known = true, none = false, found = false;
    for (size_t pos = 0; pos < n;) {
        size_t eol = pos;
        while (eol < n && text[eol] != '\n') eol++;
        const size_t start = pos;
        size_t rawEnd = eol;
        while (rawEnd > start && text[rawEnd - 1] == '\r') rawEnd--;
        pos = eol < n ? eol + 1 : n;
        size_t b = start, e = rawEnd;
        while (b < e && isSpace(text[b])) b++;
        while (e > b && isSpace(text[e - 1])) e--;
        const char* line = text + b;
        const size_t len = e - b;
        if (len > 0 && line[0] == '[') {
            const char* f = line + 1;
            const size_t fn = len >= 2 && line[len - 1] == ']' ? len - 2 : 0;
            if (fn > 0 && equal(f, fn, "all")) {
                active = known = true;
                none = false;
            } else if (fn > 0 && equal(f, fn, "none")) {
                active = false;
                none = true;
            } else if (fn > 0 && isModelFilter(f, fn)) {
                active = false;
                for (const char* const* s = sections; *s; s++)
                    if (equal(f, fn, *s)) active = true; // [none] のあとなら、使われるか分からない
                if (none) known = false;
            } else {
                known = false; // ほかの条件 ([gpio4=1]、[tryboot] など)
            }
            continue;
        }
        // = か空白までの語
        size_t wn = 0;
        while (wn < len && line[wn] != '=' && !isSpace(line[wn])) wn++;
        const bool isKernel = equal(line, wn, "kernel"), isOther = equal(line, wn, "include") || equal(line, wn, "os_prefix");
        if ((!isKernel && !isOther) || !active) continue;
        if (!known) {
            out.error = "config.txt: kernel=, include or os_prefix under a conditional section";
            return out;
        }
        if (isOther) {
            out.error = "config.txt: include or os_prefix is used";
            return out;
        }
        // 前後に空白のない kernel=名前 だけを受ける
        bool good = b == start && e == rawEnd && wn < len && line[wn] == '=' && len > wn + 1;
        for (size_t i = wn + 1; good && i < len; i++) good = isNameChar(line[i]);
        if (!good) {
            out.error = "config.txt: cannot read the kernel= line";
            return out;
        }
        found = true;
        out.offset = b + wn + 1;
        out.length = len - wn - 1;
    }
    if (!found) out.error = "config.txt: no kernel= for this model";
    return out;
}

// kernel= を from から to に替えてよいか。よければ nullptr を返し、offset に値の位置を入れる
inline const char* planKernelSwitch(const char* text, size_t n, const char* const* sections, unsigned raspi, const char* from,
                                    const char* to, size_t& offset) {
    const char* other = otherKernelName(raspi, from);
    if (other == nullptr || strcmp(other, to) != 0) return "from= and to= must be the two kernel names of this model";
    const ConfigKernel k = findConfigKernel(text, n, sections);
    if (k.error != nullptr) return k.error;
    if (!config_kernel_detail::equal(text + k.offset, k.length, from)) return "config.txt: kernel= is not from=";
    if (k.offset / CONFIG_SECTOR != (k.offset + k.length - 1) / CONFIG_SECTOR)
        return "config.txt: the kernel= value crosses a sector boundary";
    offset = k.offset;
    return nullptr;
}

enum class SwitchResult {
    OK,
    READ_ERROR,   // 読めない。書いていない
    CHANGED,      // セクタの中身が、読んだ config.txt と違う。書いていない
    WRITE_ERROR,  // 書き込みがエラーになった (変わったかは分からない)
    VERIFY_ERROR, // 書いたあとに読むと違う
};

// config.txt (text、n バイト) の値のあるセクタを disk から読み、text と同じことを確かめ、値だけを to に替えて書き、
// 読み直して比べる。disk は read(buf) / write(buf) / sync() (どれもそのセクタだけを扱い、bool を返す)。
// before と after は CONFIG_SECTOR バイトの作業用
template <class Disk>
SwitchResult switchKernelSector(Disk& disk, const char* text, size_t n, size_t offset, const char* to, uint8_t* before,
                                uint8_t* after) {
    const size_t start = offset / CONFIG_SECTOR * CONFIG_SECTOR;
    const size_t inFile = n - start < CONFIG_SECTOR ? n - start : CONFIG_SECTOR;
    if (!disk.read(before)) return SwitchResult::READ_ERROR;
    if (memcmp(before, text + start, inFile) != 0) return SwitchResult::CHANGED;
    memcpy(after, before, CONFIG_SECTOR);
    memcpy(after + (offset - start), to, strlen(to));
    if (!disk.write(after) || !disk.sync()) return SwitchResult::WRITE_ERROR;
    if (!disk.read(before)) return SwitchResult::VERIFY_ERROR;
    return memcmp(before, after, CONFIG_SECTOR) == 0 ? SwitchResult::OK : SwitchResult::VERIFY_ERROR;
}

} // namespace platform
