// ベアメタル版の HTTP の口 (baremetal/host_net.cpp) の、要求の頭 (要求行とヘッダ) の読み方と、URL の扱い。
// 1 つの接続で 1 つの要求だけを受ける (応答は Connection: close)。本体は Content-Length の長さだけ読む
// (Transfer-Encoding のある要求は、chunked でもほかでも断る)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace platform {

// 要求の頭を、空行 (何もないか CR だけの行) まで溜める。feed() は使ったバイト数を返す (残りは本体)。
// 行の終わりは LF か CR LF。CR は LF の直前の 1 つだけ受け、ほかの CR (単独、連続、行頭で空行でないもの) は BAD。
// NUL などの制御文字 (HTAB、CR、LF 以外) も BAD (頭は C の文字列として読むので、NUL で途切れさせない)
class HttpHeadReader {
public:
    static constexpr size_t MAX = 4096;
    enum State { MORE, DONE, TOO_LARGE, BAD };
    State feed(const char* data, size_t n, size_t& used) {
        used = 0;
        while (used < n) {
            if (len_ + 1 >= MAX) return TOO_LARGE;
            const char c = data[used++];
            const unsigned char u = (unsigned char)c;
            if ((u < 0x20 && c != '\t' && c != '\r' && c != '\n') || u == 0x7f) return BAD;
            const bool afterCr = len_ > 0 && buf_[len_ - 1] == '\r';
            if (afterCr && c != '\n') return BAD; // LF の前でない CR
            buf_[len_++] = c;
            buf_[len_] = 0;
            if (c == '\n') {
                // 行の長さ (行末の CR LF / LF を除く) が 0 なら空行
                const size_t content = len_ - lineStart_ - 1 - (afterCr ? 1 : 0);
                const bool firstLine = lineStart_ == 0;
                lineStart_ = len_;
                if (content == 0 && !firstLine) return DONE;
                if (content == 0) return BAD; // 要求行がない
            }
        }
        return MORE;
    }
    void reset() {
        len_ = 0;
        lineStart_ = 0;
        buf_[0] = 0;
    }
    const char* text() const { return buf_; }
    size_t size() const { return len_; }

private:
    char buf_[MAX] = {};
    size_t len_ = 0;
    size_t lineStart_ = 0; // いま溜めている行の始まり
};

struct HttpRequest {
    enum Method { GET, HEAD, PUT, POST, DELETE, OTHER };
    Method method = OTHER;
    char path[256] = {};   // %xx を戻す前の、? より前
    char query[256] = {};  // ? より後 (なければ空)
    int64_t contentLength = -1; // Content-Length (なければ -1)
    bool expectContinue = false; // Expect: 100-continue
    char token[128] = {};       // X-Pix68k-Token か Authorization: Bearer の値
    bool hasOrigin = false;     // Origin: がある (ブラウザのページから送られた)
    int minorVersion = 1;       // HTTP/1.x の x
    bool hasHost = false;       // Host: がある
    char host[256] = {};        // Host: の値 (前後の空白を除く)
    const char* error = "";     // parseHttpRequest() が false のときの理由 (応答の本文にする)
};

namespace http_detail {
// memchr (ベアメタルの <string.h> の代わりの Circle にはない)
inline const char* findChar(const char* s, char c, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (s[i] == c) return s + i;
    return nullptr;
}
inline char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }
// ヘッダ名に使える文字 (RFC 9110 の tchar)
inline bool isTokenChar(char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    return c != 0 && strchr("!#$%&'*+-.^_`|~", c) != nullptr;
}
inline bool equalNoCase(const char* a, size_t an, const char* b) {
    const size_t bn = strlen(b);
    if (an != bn) return false;
    for (size_t i = 0; i < an; i++)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}
// 前後の空白 (SP と HTAB) を除いた範囲にする (CR は除かない。行末の CR は lineEnd() が 1 つだけ除く)
inline void trim(const char*& s, size_t& n) {
    while (n > 0 && (*s == ' ' || *s == '\t')) s++, n--;
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
}
// line から始まり eol (LF) で終わる行の、中身の終わり。LF の直前の CR は 1 つだけ除く。ほかに CR があれば nullptr
inline const char* lineEnd(const char* line, const char* eol) {
    const char* end = eol > line && eol[-1] == '\r' ? eol - 1 : eol;
    return findChar(line, '\r', (size_t)(end - line)) == nullptr ? end : nullptr;
}
// 入らなければ false (切り詰めない)
inline bool copySpan(char* out, size_t size, const char* s, size_t n) {
    if (n >= size) return false;
    memcpy(out, s, n);
    out[n] = 0;
    return true;
}
} // namespace http_detail

// 要求の頭を読む。読めなければ false
inline bool parseHttpRequest(const char* head, HttpRequest& r) {
    using namespace http_detail;
    r = HttpRequest();
    const char* line = head;
    const char* eol = strchr(line, '\n');
    r.error = "bad request\n";
    if (eol == nullptr) return false;
    // 要求行: メソッド SP 対象 SP 版
    const char* end = lineEnd(line, eol);
    if (end == nullptr) return false;
    const char* sp1 = findChar(line, ' ', (size_t)(end - line));
    if (sp1 == nullptr) return false;
    const char* target = sp1 + 1;
    const char* sp2 = findChar(target, ' ', (size_t)(end - target));
    if (sp2 == nullptr || sp2 == target || *target != '/') return false;
    if ((size_t)(end - sp2 - 1) != 8 || strncmp(sp2 + 1, "HTTP/1.", 7) != 0) return false;
    if (sp2[8] < '0' || sp2[8] > '9') return false;
    r.minorVersion = sp2[8] - '0';
    const size_t mn = (size_t)(sp1 - line);
    static const struct {
        const char* name;
        HttpRequest::Method m;
    } kMethods[] = {{"GET", HttpRequest::GET}, {"HEAD", HttpRequest::HEAD}, {"PUT", HttpRequest::PUT},
                    {"POST", HttpRequest::POST}, {"DELETE", HttpRequest::DELETE}};
    for (const auto& k : kMethods)
        if (mn == strlen(k.name) && memcmp(line, k.name, mn) == 0) r.method = k.m;
    const char* q = findChar(target, '?', (size_t)(sp2 - target));
    const char* pathEnd = q ? q : sp2;
    if ((size_t)(pathEnd - target) >= sizeof r.path) return false;
    memcpy(r.path, target, (size_t)(pathEnd - target));
    r.path[pathEnd - target] = 0;
    if (q != nullptr) {
        if ((size_t)(sp2 - q - 1) >= sizeof r.query) return false;
        memcpy(r.query, q + 1, (size_t)(sp2 - q - 1));
        r.query[sp2 - q - 1] = 0;
    }
    // ヘッダ。値は切り詰めずに、元の範囲のまま見る
    auto fail = [&](const char* why) {
        r.error = why;
        return false;
    };
    for (line = eol + 1;; line = eol + 1) {
        eol = strchr(line, '\n');
        if (eol == nullptr) return fail("bad request\n"); // 空行で終わっていない
        end = lineEnd(line, eol);
        if (end == nullptr) return fail("bad line ending (CR not followed by LF)\n");
        if (end == line) break; // 空行 (CR LF か LF だけ) で頭が終わる
        const char* colon = findChar(line, ':', (size_t)(end - line));
        if (colon == nullptr) return fail("bad header line\n");
        const size_t nn = (size_t)(colon - line);
        // 名前はトークンの文字だけ。コロンの前の空白 ("Transfer-Encoding : chunked") や、空白で始まる続きの行
        // (obs-fold) は、知らないヘッダとして見逃さずに断る
        if (nn == 0) return fail("bad header name\n");
        for (size_t i = 0; i < nn; i++)
            if (!isTokenChar(line[i])) return fail("bad header name\n");
        const char* v = colon + 1;
        size_t vn = (size_t)(end - v);
        trim(v, vn);
        // 値に制御文字 (HTAB 以外。行の途中の CR も) があれば断る
        for (size_t i = 0; i < vn; i++) {
            const unsigned char u = (unsigned char)v[i];
            if ((u < 0x20 && v[i] != '\t') || u == 0x7f) return fail("bad header value\n");
        }
        if (equalNoCase(line, nn, "content-length")) {
            // 数字だけ (1 TB まで)。同じ値が 2 回なら受け、違う値なら断る (どちらを信じるか決められない)
            if (vn == 0 || vn > 13) return fail("bad Content-Length\n");
            int64_t n = 0;
            for (size_t i = 0; i < vn; i++) {
                if (v[i] < '0' || v[i] > '9') return fail("bad Content-Length\n");
                n = n * 10 + (v[i] - '0');
            }
            if (n > ((int64_t)1 << 40)) return fail("Content-Length too large\n");
            if (r.contentLength >= 0 && r.contentLength != n) return fail("conflicting Content-Length\n");
            r.contentLength = n;
        } else if (equalNoCase(line, nn, "transfer-encoding")) {
            // chunked もほかの符号化も受けない (本体の長さは Content-Length だけで決める)
            return fail("Transfer-Encoding is not supported; send Content-Length (curl -T FILE URL)\n");
        } else if (equalNoCase(line, nn, "origin")) {
            r.hasOrigin = true;
        } else if (equalNoCase(line, nn, "host")) {
            // 2 つあれば断る (どちらを見るか決められない)
            if (r.hasHost) return fail("more than one Host:\n");
            if (!copySpan(r.host, sizeof r.host, v, vn)) return fail("Host: too long\n");
            r.hasHost = true;
        } else if (equalNoCase(line, nn, "expect")) {
            r.expectContinue = equalNoCase(v, vn, "100-continue");
        } else if (equalNoCase(line, nn, "x-pix68k-token")) {
            if (!copySpan(r.token, sizeof r.token, v, vn)) return fail("token too long\n");
        } else if (equalNoCase(line, nn, "authorization")) {
            if (vn >= 7 && equalNoCase(v, 7, "bearer ")) {
                const char* t = v + 7;
                size_t tn = vn - 7;
                trim(t, tn);
                if (!copySpan(r.token, sizeof r.token, t, tn)) return fail("token too long\n");
            }
        }
    }
    return true;
}

enum class HostCheck { OK, MISSING, OTHER };

// Host: が自分あてか (DNS リバインディング対策)。ほかの名前 (攻撃する側のドメイン) を自分のアドレスに向けたページから、
// ブラウザが読み書きを送ってくるのを、GET も含めて断るため。通すのは、自分のアドレス (self) か net_name (name。
// 大文字小文字を区別しない) で、後ろに :ポート や名前の終わりの . が付いていてもよい。
// curl の既定 (http://192.168.1.50/ なら Host: 192.168.1.50) と http://pix68k/ (Host: pix68k) は通る。
// Host: がないのは HTTP/1.0 だけ受ける (HTTP/1.1 では Host: が要る)
inline HostCheck checkHost(const HttpRequest& r, const uint8_t self[4], const char* name) {
    if (!r.hasHost) return r.minorVersion == 0 ? HostCheck::OK : HostCheck::MISSING;
    size_t n = strlen(r.host);
    // :ポート (数字だけ) を除く
    for (size_t i = n; i > 0; i--) {
        const char c = r.host[i - 1];
        if (c == ':') {
            if (i == n) return HostCheck::OTHER; // ":" だけ
            n = i - 1;
            break;
        }
        if (c < '0' || c > '9') break;
    }
    if (n > 0 && r.host[n - 1] == '.') n--;
    if (n == 0) return HostCheck::OTHER;
    char ip[16];
    size_t k = 0;
    for (int i = 0; i < 4; i++) {
        unsigned v = self[i];
        char d[3];
        int m = 0;
        do {
            d[m++] = (char)('0' + v % 10);
            v /= 10;
        } while (v != 0);
        while (m > 0) ip[k++] = d[--m];
        if (i < 3) ip[k++] = '.';
    }
    ip[k] = 0;
    if (n == k && memcmp(r.host, ip, k) == 0) return HostCheck::OK;
    if (name != nullptr && name[0] != 0 && http_detail::equalNoCase(r.host, n, name)) return HostCheck::OK;
    return HostCheck::OTHER;
}

// %xx を戻す ('+' はそのまま)。%00、制御文字、壊れた %、out に入らないときは false
inline bool urlDecode(const char* in, char* out, size_t size) {
    auto hex = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    size_t n = 0;
    for (const char* p = in; *p; p++) {
        int c = (unsigned char)*p;
        if (c == '%') {
            const int h = hex(p[1]), l = h >= 0 ? hex(p[2]) : -1;
            if (h < 0 || l < 0) return false;
            c = h * 16 + l;
            p += 2;
        }
        if (c < 0x20 || c == 0x7f) return false;
        if (n + 1 >= size) return false;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return true;
}

// クエリ ("a=1&b=2") から key の値を取り出す (%xx を戻す)。key だけで = がなければ空の値。なければ false
inline bool queryValue(const char* query, const char* key, char* out, size_t size) {
    const size_t kn = strlen(key);
    for (const char* p = query; *p;) {
        const char* amp = strchr(p, '&');
        const size_t n = amp ? (size_t)(amp - p) : strlen(p);
        if (n >= kn && memcmp(p, key, kn) == 0 && (n == kn || p[kn] == '=')) {
            char raw[256];
            const size_t vn = n == kn ? 0 : n - kn - 1;
            if (vn >= sizeof raw) return false;
            memcpy(raw, p + kn + 1, vn);
            raw[vn] = 0;
            return urlDecode(raw, out, size);
        }
        p += n;
        if (*p == '&') p++;
    }
    return false;
}

// URL の /files/ より後ろ (%xx は戻したもの) を、ドライブの中のパス ("/pix68k/pix68k.cfg"。ルートは "/") にする。
// 「.」と「..」の段、\、:、FAT に使えない文字 (* ? " < > |) があれば false。続いた / と末尾の / は 1 つにまとめる
inline bool normalizeFilePath(const char* in, char* out, size_t size) {
    size_t n = 0;
    if (size < 2) return false;
    out[n++] = '/';
    const char* p = in;
    while (*p) {
        while (*p == '/') p++;
        if (*p == 0) break;
        const char* seg = p;
        while (*p && *p != '/') {
            const char c = *p;
            if (c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
                (unsigned char)c < 0x20)
                return false;
            p++;
        }
        const size_t sn = (size_t)(p - seg);
        if ((sn == 1 && seg[0] == '.') || (sn == 2 && seg[0] == '.' && seg[1] == '.')) return false;
        if (n > 1) {
            if (n + 1 >= size) return false;
            out[n++] = '/';
        }
        if (n + sn >= size) return false;
        memcpy(out + n, seg, sn);
        n += sn;
    }
    out[n] = 0;
    return true;
}

// /type の文字列のエスケープ (\n、\r は Enter、\t は TAB、\b は BS、\e は ESC、\\ は \) を戻す。
// 知らないエスケープは、\ とその文字のまま
inline size_t unescapeTyped(const char* in, size_t n, char* out, size_t size) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < size; i++) {
        char c = in[i];
        if (c == '\\' && i + 1 < n) {
            const char e = in[i + 1];
            const char m = e == 'n' || e == 'r' ? '\n' : e == 't' ? '\t' : e == 'b' ? '\b' : e == 'e' ? 0x1b : e == '\\' ? '\\' : 0;
            if (m != 0) {
                c = m;
                i++;
            }
        }
        out[o++] = c;
    }
    out[o] = 0;
    return o;
}

inline const char* httpReason(int status) {
    switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Unknown";
    }
}

} // namespace platform
