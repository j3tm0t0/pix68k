// ベアメタル版のネットワークの口 (baremetal/host_net.h) の設定を読む: IPv4 のアドレス、「アドレス/プレフィクス長」、
// 「アドレス:ポート」、送り元の制限 (net_allow= のアドレスの並び)。
// 値は pix68k.cfg (platform/settings.h の net_* と syslog) か cmdline.txt (pix68k_net_* と pix68k_syslog) から来る。
// cmdline.txt のほうが勝つ (mergeNetOption())。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace platform {

// "192.168.1.10" を読む。end が nullptr でなければ、読み終えた位置を入れる (後ろに文字があってもよい)。
// end が nullptr なら、後ろに文字があれば false
inline bool parseIpv4(const char* s, uint8_t out[4], const char** end = nullptr) {
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return false;
        unsigned v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s - '0');
            if (++digits > 3 || v > 255) return false;
            s++;
        }
        out[i] = (uint8_t)v;
        if (i < 3) {
            if (*s != '.') return false;
            s++;
        }
    }
    if (end != nullptr) *end = s;
    else if (*s != 0) return false;
    return true;
}

// "192.168.1.10/24" か "192.168.1.10" を読む。プレフィクス長がなければ prefix に -1 を入れる
inline bool parseIpv4Cidr(const char* s, uint8_t ip[4], int& prefix) {
    const char* p = nullptr;
    if (!parseIpv4(s, ip, &p)) return false;
    prefix = -1;
    if (*p == 0) return true;
    if (*p++ != '/' || *p < '0' || *p > '9') return false;
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p++ - '0');
        if (v > 32) return false;
    }
    if (*p != 0) return false;
    prefix = v;
    return true;
}

inline void prefixToMask(int prefix, uint8_t mask[4]) {
    const uint32_t m = prefix <= 0 ? 0u : prefix >= 32 ? 0xffffffffu : ~((1u << (32 - prefix)) - 1);
    for (int i = 0; i < 4; i++) mask[i] = (uint8_t)(m >> (24 - 8 * i));
}

// "192.168.1.2:5514" か "192.168.1.2" (ポートは defPort) を読む。名前 (DNS) は使えない
inline bool parseIpv4Port(const char* s, uint8_t ip[4], uint16_t& port, uint16_t defPort) {
    const char* p = nullptr;
    if (!parseIpv4(s, ip, &p)) return false;
    port = defPort;
    if (*p == 0) return true;
    if (*p++ != ':' || *p == 0) return false;
    unsigned v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (unsigned)(*p++ - '0');
        if (v > 65535) return false;
    }
    if (*p != 0 || v == 0) return false;
    port = (uint16_t)v;
    return true;
}

// net_allow= の並び ("192.168.1.5,192.168.1.0/24"。「,」か空白で区切る) を 1 つずつ fn(ip, prefix) に渡す。
// 読めない項目があれば false (fn はそこまでの項目に呼んだまま)
template <class Fn> bool forEachAllowEntry(const char* list, Fn&& fn) {
    const char* s = list;
    while (*s) {
        while (*s == ',' || *s == ' ' || *s == '\t') s++;
        if (*s == 0) break;
        char item[24];
        size_t n = 0;
        while (*s && *s != ',' && *s != ' ' && *s != '\t') {
            if (n + 1 >= sizeof item) return false;
            item[n++] = *s++;
        }
        item[n] = 0;
        uint8_t ip[4];
        int prefix;
        if (!parseIpv4Cidr(item, ip, prefix)) return false;
        fn(ip, prefix < 0 ? 32 : prefix);
    }
    return true;
}

inline bool allowListValid(const char* list) {
    return forEachAllowEntry(list, [](const uint8_t*, int) {});
}

// ip が並びのどれかに入るか (並びが空なら false。読めない並びも false)
inline bool allowListHas(const char* list, const uint8_t ip[4]) {
    bool hit = false;
    const bool ok = forEachAllowEntry(list, [&](const uint8_t* net, int prefix) {
        uint8_t mask[4];
        prefixToMask(prefix, mask);
        bool same = true;
        for (int i = 0; i < 4; i++) same = same && (ip[i] & mask[i]) == (net[i] & mask[i]);
        hit = hit || same;
    });
    return ok && hit;
}

// トークンを比べる。どちらも 0 で埋めた TOKEN_COMPARE バイトのバッファに写してから、全バイトを同じ手順で比べる
// (かかる時間から、何文字目まで合っているかは分からない。写すところで文字列の長さは時間に出うる)。
// TOKEN_COMPARE - 1 文字より長いトークンは一致しない
constexpr size_t TOKEN_COMPARE = 128;
inline bool sameToken(const char* want, const char* given) {
    uint8_t a[TOKEN_COMPARE] = {}, b[TOKEN_COMPARE] = {};
    const size_t an = strlen(want), bn = strlen(given);
    if (an >= TOKEN_COMPARE || bn >= TOKEN_COMPARE) return false;
    memcpy(a, want, an);
    memcpy(b, given, bn);
    unsigned diff = 0;
    for (size_t i = 0; i < TOKEN_COMPARE; i++) diff |= (unsigned)(a[i] ^ b[i]);
    return diff == 0;
}

// 頼まれた操作を受けてよいか。
//   READ (ログ、画面): net_allow= があれば、その中の送り元からだけ
//   READ_FILES (ファイルを読む): それに加えて、net_token= があればトークンが一致すること。
//     トークンは pix68k.cfg や cmdline.txt に書いてあり、その写し (.new、.part、.old、手で作った控え) もありうる。
//     名前で選んで守るのでは漏れが出るので、ファイルを読むのは全部トークンが要ることにする
//   WRITE (ファイルを書く、消す、再起動、キー): READ_FILES と同じ。ただし net_allow= も net_token= もなければ
//     受けない (どこからでも書き換えられないように)
enum class NetOp { READ, READ_FILES, WRITE };
enum class NetAccess { OK, DENIED_SOURCE, DENIED_TOKEN, NO_WRITE_RULE };
inline NetAccess checkNetAccess(const char* allow, const char* token, const uint8_t source[4], const char* givenToken,
                                NetOp op) {
    if (allow[0] != 0 && !allowListHas(allow, source)) return NetAccess::DENIED_SOURCE;
    if (op == NetOp::READ) return NetAccess::OK;
    if (op == NetOp::WRITE && allow[0] == 0 && token[0] == 0) return NetAccess::NO_WRITE_RULE;
    if (token[0] != 0) {
        if (!sameToken(token, givenToken)) return NetAccess::DENIED_TOKEN;
    }
    return NetAccess::OK;
}

namespace net_detail {
inline char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }
// s の先頭が word (小文字) か (大文字小文字を区別しない)
inline bool startsWithNoCase(const char* s, const char* word) {
    for (; *word; s++, word++)
        if (lowerAscii(*s) != *word) return false;
    return true;
}
} // namespace net_detail

// 値をログに出さない設定の名前 (net_token。cmdline.txt の pix68k_net_token も)。/log はトークンなしで読めるため
inline bool isSecretSettingKey(const char* key) {
    using namespace net_detail;
    if (startsWithNoCase(key, "pix68k_")) key += 7;
    return startsWithNoCase(key, "net_token") && key[9] == 0;
}

// 行に net_token が書いてあるか (大文字小文字を区別しない)。読めずに捨てた行をログに出すかを決める
inline bool mentionsSecretSetting(const char* line) {
    for (const char* p = line; *p; p++)
        if (net_detail::startsWithNoCase(p, "net_token")) return true;
    return false;
}

} // namespace platform
