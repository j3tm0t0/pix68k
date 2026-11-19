// pix68k.cfg の文字列を、行の並びと注釈を残したまま読み書きする。
//
// 形式: 1 行に 1 つ「キー=値」。# から行末までは注釈。キーと値の前後の空白は無視する。
// 同じキーが何度も出てきたら、後の行が勝つ (読む側はすべての行を順に当てる)。
// set() は、そのキーの最後の行の値だけを書き換える (前後の空白、行末の注釈、改行の形 (CRLF か LF) は残す)。
// その行がなければ、ファイルの最後に「キー=値」の行を足す。知らないキーの行、注釈、空行はそのまま残る。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。バッファは new で確保する
// (ベアメタルではコア 0 だけが使う)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace platform {

class ConfigText {
public:
    ConfigText() {}
    ~ConfigText() { delete[] buf_; }
    ConfigText(const ConfigText&) = delete;
    ConfigText& operator=(const ConfigText&) = delete;

    // 中身を置き換える。メモリが足りなければ false (中身は空になる)
    bool load(const char* data, size_t size) {
        len_ = 0;
        if (!reserve(size + 1)) return false;
        if (size) memcpy(buf_, data, size);
        len_ = size;
        return true;
    }
    const char* data() const { return buf_ ? buf_ : ""; }
    size_t size() const { return len_; }

    // 1 行の中身。key と value は 0 で終わる (行の長さが MAX_LINE を超える分は切る)
    static constexpr size_t MAX_LINE = 256;
    struct Entry {
        char key[64];
        char value[MAX_LINE];
    };

    // 「キー=値」の行ごとに fn(const Entry&) を呼ぶ (上の行から)。= のない行 (注釈と空行を除く) は
    // bad(const char* line) を呼ぶ
    template <typename Fn, typename Bad>
    void forEach(Fn&& fn, Bad&& bad) const {
        size_t pos = 0;
        while (pos < len_) {
            Line l = parseLine(pos);
            pos = l.next;
            if (l.eq < 0) {
                if (l.textEnd > l.keyStart) { // 空白でも注釈でもない、= のない行
                    char tmp[MAX_LINE];
                    copy(tmp, sizeof tmp, l.keyStart, l.textEnd);
                    bad((const char*)tmp);
                }
                continue;
            }
            Entry e;
            copy(e.key, sizeof e.key, l.keyStart, l.keyEnd);
            copy(e.value, sizeof e.value, l.valueStart, l.valueEnd);
            fn((const Entry&)e);
        }
    }

    // key の値を value にする。値に # と改行は書けない (false)。メモリが足りなくても false
    bool set(const char* key, const char* value) {
        if (strchr(value, '#') || strchr(value, '\r') || strchr(value, '\n') || key[0] == 0) return false;
        const size_t klen = strlen(key), vlen = strlen(value);
        // key の最後の行を探す
        long hitStart = -1, hitEnd = -1;
        size_t pos = 0;
        bool crlf = false;
        while (pos < len_) {
            Line l = parseLine(pos);
            if (l.crlf) crlf = true;
            if (l.eq >= 0 && l.keyEnd - l.keyStart == klen && memcmp(buf_ + l.keyStart, key, klen) == 0) {
                hitStart = (long)l.valueStart;
                hitEnd = (long)l.valueEnd;
            }
            pos = l.next;
        }
        if (hitStart >= 0) return replace((size_t)hitStart, (size_t)hitEnd, value, vlen);
        // 行を足す。最後の行に改行がなければ先に足す
        const char* nl = crlf ? "\r\n" : "\n";
        const size_t nlen = strlen(nl);
        const bool needNl = len_ > 0 && buf_[len_ - 1] != '\n';
        if (!reserve(len_ + (needNl ? nlen : 0) + klen + 1 + vlen + nlen + 1)) return false;
        if (needNl) append(nl, nlen);
        append(key, klen);
        append("=", 1);
        append(value, vlen);
        append(nl, nlen);
        return true;
    }

    // key の行をすべて消す (行の注釈と改行も)。消した行の数を返す
    int removeLines(const char* key) {
        const size_t klen = strlen(key);
        int count = 0;
        size_t pos = 0;
        while (pos < len_) {
            const Line l = parseLine(pos);
            if (l.eq >= 0 && l.keyEnd - l.keyStart == klen && memcmp(buf_ + l.keyStart, key, klen) == 0) {
                memmove(buf_ + pos, buf_ + l.next, len_ - l.next);
                len_ -= l.next - pos;
                count++;
                continue;
            }
            pos = l.next;
        }
        return count;
    }

    // key の (最後の行の) 値を out に入れる。なければ false
    bool get(const char* key, char* out, size_t size) const {
        bool found = false;
        forEach(
            [&](const Entry& e) {
                if (strcmp(e.key, key) == 0) {
                    copyStr(out, size, e.value);
                    found = true;
                }
            },
            [](const char*) {});
        return found;
    }

private:
    struct Line {
        size_t keyStart, keyEnd;     // キー (前後の空白を除く)。= がなければ行の中身の先頭
        long eq;                     // = の位置 (なければ -1)
        size_t valueStart, valueEnd; // 値 (前後の空白を除く)
        size_t textEnd;              // 注釈と後ろの空白を除いた行の終わり
        size_t next;                 // 次の行の先頭
        bool crlf;
    };

    static bool space(char c) { return c == ' ' || c == '\t'; }

    Line parseLine(size_t pos) const {
        Line l;
        size_t end = pos;
        while (end < len_ && buf_[end] != '\n') end++;
        l.next = end < len_ ? end + 1 : end;
        l.crlf = end < len_ && end > pos && buf_[end - 1] == '\r';
        size_t lineEnd = l.crlf ? end - 1 : end;
        // 注釈を除く
        size_t text = pos;
        while (text < lineEnd && buf_[text] != '#') text++;
        size_t te = text;
        while (te > pos && (space(buf_[te - 1]) || buf_[te - 1] == '\r')) te--;
        size_t ks = pos;
        while (ks < te && space(buf_[ks])) ks++;
        l.keyStart = ks;
        l.textEnd = te;
        l.eq = -1;
        for (size_t i = ks; i < te; i++)
            if (buf_[i] == '=') {
                l.eq = (long)i;
                break;
            }
        if (l.eq < 0) {
            l.keyEnd = ks;
            l.valueStart = l.valueEnd = te;
            return l;
        }
        size_t ke = (size_t)l.eq;
        while (ke > ks && space(buf_[ke - 1])) ke--;
        l.keyEnd = ke;
        size_t vs = (size_t)l.eq + 1;
        while (vs < te && space(buf_[vs])) vs++;
        l.valueStart = vs;
        l.valueEnd = te > vs ? te : vs;
        return l;
    }

    void copy(char* out, size_t size, size_t from, size_t to) const {
        size_t n = to > from ? to - from : 0;
        if (n >= size) n = size - 1;
        if (n) memcpy(out, buf_ + from, n);
        out[n] = 0;
    }
    static void copyStr(char* out, size_t size, const char* s) {
        size_t n = strlen(s);
        if (n >= size) n = size - 1;
        memcpy(out, s, n);
        out[n] = 0;
    }

    bool reserve(size_t want) {
        if (want <= cap_) return true;
        size_t cap = cap_ ? cap_ : 256;
        while (cap < want) cap *= 2;
        char* nb = new char[cap];
        if (nb == nullptr) return false;
        if (len_) memcpy(nb, buf_, len_);
        delete[] buf_;
        buf_ = nb;
        cap_ = cap;
        return true;
    }
    void append(const char* s, size_t n) {
        memcpy(buf_ + len_, s, n);
        len_ += n;
    }
    bool replace(size_t from, size_t to, const char* s, size_t n) {
        const size_t old = to - from;
        if (!reserve(len_ - old + n + 1)) return false;
        memmove(buf_ + from + n, buf_ + to, len_ - to);
        memcpy(buf_ + from, s, n);
        len_ = len_ - old + n;
        return true;
    }

    char* buf_ = nullptr;
    size_t len_ = 0, cap_ = 0;
};

} // namespace platform
