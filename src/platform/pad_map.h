// /pix68k/pads.txt: ゲームパッドのボタンの並びを、VID:PID ごとに 1 行で書いたもの (#379)。
//
//   0f0d:0138 if=0 up=h0.0 right=h0.2 down=h0.4 left=h0.6 a=b2 b=b1 select=b8 start=b9 # 名前
//
// 行の先頭は VID:PID (16 進 4 桁ずつ)。そのあとは空白で区切った「項目=入力」(順番は自由)。# から行末までは注釈。
//   項目: up down left right a (この 5 つは必須)、b start select home (なくてよい)
//   入力: b<番号>        ボタン (レポートの順。0-31。pad_log=on の buttons= のビット)
//         h<番号>.<向き> ハット (向きは 0 上、2 右、4 下、6 左。斜めは両隣の向きを押したことになる)
//         x<番号>+ / x<番号>- 軸 (真ん中から振れ幅の 40% を超えて、大きいほう / 小さいほうに倒したとき)
//   if=<番号>: USB のインタフェースの番号 (1 つの機器に 2 台分のパッドがあるときなど)。書かなければどれでもよい
//   hatmin=0 / hatmin=1: ハットの値が上で始まる数 (0-7 の機種なら 0、1-8 の機種なら 1)。書かなければ、最初のハットは
//     HatDecoder で決め、ほかのハットは 0 とする
// 同じ VID:PID と if= の行は、後の行が勝つ。if= の合う行が、if= のない行より優先する。
// 使えない行は警告して読み飛ばす (1 行の誤りで、ほかの行まで読めなくならないように)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform/input_map.h"

namespace platform {

// レポートから読んだままの値 (Circle の TGamePadState の写し)
struct PadRaw {
    static constexpr int HAT_COUNT = 6, AXIS_COUNT = 16;
    uint32_t buttons = 0;
    int nhats = 0;
    int hats[HAT_COUNT] = {};
    int naxes = 0;
    struct Axis {
        int value, minimum, maximum;
    } axes[AXIS_COUNT] = {};
};

struct PadSource {
    enum Kind : uint8_t { NONE, BUTTON, HAT, AXIS };
    Kind kind = NONE;
    uint8_t index = 0;
    uint8_t dir = 0; // HAT: 0、2、4、6。AXIS: 1 なら大きいほう、0 なら小さいほう
    bool operator==(const PadSource& o) const { return kind == o.kind && index == o.index && dir == o.dir; }
};

struct PadMap {
    enum Item { UP, DOWN, LEFT, RIGHT, A, B, START, SELECT, HOME, ITEMS };
    static constexpr int REQUIRED = B; // UP から A までは必須
    uint16_t vid = 0, pid = 0;
    int iface = -1;  // -1 ならどのインタフェースでもよい
    int hatMin = -1; // -1 なら書いていない
    PadSource src[ITEMS];

    bool sameKey(const PadMap& o) const { return vid == o.vid && pid == o.pid && iface == o.iface; }
};

namespace pad_map_detail {

// Circle の string.h には memchr がない
inline const char* findChar(const char* p, const char* end, char c) {
    for (; p < end; p++)
        if (*p == c) return p;
    return nullptr;
}

inline const char* itemName(int i) {
    static const char* const kNames[PadMap::ITEMS] = {"up", "down", "left", "right", "a", "b", "start", "select", "home"};
    return kNames[i];
}

inline int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool hex4(const char* p, uint16_t& out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        const int d = hexDigit(p[i]);
        if (d < 0) return false;
        v = v * 16 + (unsigned)d;
    }
    out = (uint16_t)v;
    return true;
}

// p から end までが 0 から max の 10 進数 (1 桁から 3 桁で、余分な 0 を先頭に付けない) だけなら true
inline bool number(const char* p, const char* end, int max, int& out) {
    if (p >= end || end - p > 3 || (*p == '0' && end - p > 1)) return false;
    int v = 0;
    for (; p < end; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (*p - '0');
    }
    if (v > max) return false;
    out = v;
    return true;
}

inline bool source(const char* p, const char* end, PadSource& s) {
    if (end - p < 2) return false;
    int n = 0;
    if (p[0] == 'b') {
        if (!number(p + 1, end, 31, n)) return false;
        s.kind = PadSource::BUTTON;
        s.dir = 0;
    } else if (p[0] == 'h') {
        const char* dot = findChar(p, end, '.');
        int dir = 0;
        if (dot == nullptr || !number(p + 1, dot, PadRaw::HAT_COUNT - 1, n) || !number(dot + 1, end, 6, dir) || (dir & 1))
            return false;
        s.kind = PadSource::HAT;
        s.dir = (uint8_t)dir;
    } else if (p[0] == 'x') {
        const char sign = end[-1];
        if ((sign != '+' && sign != '-') || !number(p + 1, end - 1, PadRaw::AXIS_COUNT - 1, n)) return false;
        s.kind = PadSource::AXIS;
        s.dir = sign == '+' ? 1 : 0;
    } else {
        return false;
    }
    s.index = (uint8_t)n;
    return true;
}

// text の pos から始まる行の終わり (改行の位置か size) を返し、next に次の行の始まりを入れる。改行は LF、CR LF、CR
inline size_t lineEnd(const char* text, size_t size, size_t pos, size_t& next) {
    size_t end = pos;
    while (end < size && text[end] != '\n' && text[end] != '\r') end++;
    next = end;
    if (next < size && text[next] == '\r') next++;
    if (next < size && text[next] == '\n') next++;
    return end;
}

inline bool space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

} // namespace pad_map_detail

enum class PadLineResult {
    OK,
    EMPTY,     // 空行か注釈だけ
    TOO_LONG,  // 長すぎる
    BAD_ID,    // 先頭が VID:PID でない
    BAD_FIELD, // 「項目=入力」の形でないか、知らない項目か、使えない入力
    REPEATED,  // 同じ項目が 2 度ある
    SHARED,    // 同じ入力を 2 つの項目に使っている
    MISSING,   // 必須の項目がない
};

inline const char* padLineError(PadLineResult r) {
    switch (r) {
    case PadLineResult::TOO_LONG: return "the line is too long";
    case PadLineResult::BAD_ID: return "the line does not start with VID:PID (4 hex digits each)";
    case PadLineResult::BAD_FIELD: return "unknown item or input";
    case PadLineResult::REPEATED: return "an item is written twice";
    case PadLineResult::SHARED: return "one input is used for two items";
    case PadLineResult::MISSING: return "up, down, left, right and a are required";
    default: return "";
    }
}

static constexpr size_t PAD_LINE_MAX = 255;

// pads.txt の 1 行 (改行を含まない) を読む。OK のときだけ out を書き換える
inline PadLineResult parsePadLine(const char* line, size_t len, PadMap& out) {
    using namespace pad_map_detail;
    const char* end = line + len;
    if (const char* hash = findChar(line, end, '#')) end = hash;
    while (line < end && space(*line)) line++;
    while (end > line && space(end[-1])) end--;
    if (line == end) return PadLineResult::EMPTY;
    if ((size_t)(end - line) > PAD_LINE_MAX) return PadLineResult::TOO_LONG;
    PadMap m;
    if (end - line < 9 || line[4] != ':' || !hex4(line, m.vid) || !hex4(line + 5, m.pid)) return PadLineResult::BAD_ID;
    if (end - line > 9 && !space(line[9])) return PadLineResult::BAD_ID;
    const char* p = line + 9;
    for (;;) {
        while (p < end && space(*p)) p++;
        if (p == end) break;
        const char* w = p;
        while (p < end && !space(*p)) p++;
        const char* eq = findChar(w, p, '=');
        if (eq == nullptr) return PadLineResult::BAD_FIELD;
        const size_t klen = (size_t)(eq - w);
        int n = 0;
        if (klen == 2 && memcmp(w, "if", 2) == 0) {
            if (m.iface >= 0) return PadLineResult::REPEATED;
            if (!number(eq + 1, p, 255, n)) return PadLineResult::BAD_FIELD;
            m.iface = n;
            continue;
        }
        if (klen == 6 && memcmp(w, "hatmin", 6) == 0) {
            if (m.hatMin >= 0) return PadLineResult::REPEATED;
            if (!number(eq + 1, p, 1, n)) return PadLineResult::BAD_FIELD;
            m.hatMin = n;
            continue;
        }
        int item = -1;
        for (int i = 0; i < PadMap::ITEMS; i++)
            if (strlen(itemName(i)) == klen && memcmp(w, itemName(i), klen) == 0) item = i;
        PadSource s;
        if (item < 0 || !source(eq + 1, p, s)) return PadLineResult::BAD_FIELD;
        if (m.src[item].kind != PadSource::NONE) return PadLineResult::REPEATED;
        for (int i = 0; i < PadMap::ITEMS; i++)
            if (m.src[i] == s) return PadLineResult::SHARED;
        m.src[item] = s;
    }
    for (int i = 0; i < PadMap::REQUIRED; i++)
        if (m.src[i].kind == PadSource::NONE) return PadLineResult::MISSING;
    out = m;
    return PadLineResult::OK;
}

// m を pads.txt の 1 行 (改行なし) にする。parsePadLine() で読むと同じものになる。書いた長さを返す。入りきらなければ 0
inline size_t formatPadLine(const PadMap& m, char* buf, size_t size) {
    using namespace pad_map_detail;
    size_t n = 0;
    bool ok = true;
    auto ch = [&](char c) {
        if (n + 1 < size) buf[n++] = c;
        else ok = false;
    };
    auto str = [&](const char* s) {
        while (*s) ch(*s++);
    };
    auto num = [&](unsigned v) { // 255 まで
        if (v >= 100) ch((char)('0' + v / 100 % 10));
        if (v >= 10) ch((char)('0' + v / 10 % 10));
        ch((char)('0' + v % 10));
    };
    auto hex = [&](unsigned v) {
        for (int i = 12; i >= 0; i -= 4) ch("0123456789abcdef"[(v >> i) & 15]);
    };
    hex(m.vid);
    ch(':');
    hex(m.pid);
    if (m.iface >= 0) {
        str(" if=");
        num((unsigned)m.iface);
    }
    if (m.hatMin >= 0) {
        str(" hatmin=");
        num((unsigned)m.hatMin);
    }
    for (int i = 0; i < PadMap::ITEMS; i++) {
        const PadSource& s = m.src[i];
        if (s.kind == PadSource::NONE) continue;
        ch(' ');
        str(itemName(i));
        ch('=');
        ch(s.kind == PadSource::BUTTON ? 'b' : s.kind == PadSource::HAT ? 'h' : 'x');
        num(s.index);
        if (s.kind == PadSource::HAT) {
            ch('.');
            num(s.dir);
        } else if (s.kind == PadSource::AXIS) {
            ch(s.dir ? '+' : '-');
        }
    }
    if (size) buf[n] = 0;
    return ok ? n : 0;
}

// 読んだ pads.txt の中身。ゲームパッドをつないだときに find() で引く
class PadMapTable {
public:
    static constexpr int MAX = 32, MAX_WARNINGS = 8;

    // text を読む (前の中身は捨てる)。使えない行と、MAX を超えた行は warn(行番号 (1 から), 理由) を呼ぶ。
    // pads.txt でないファイルを置いたときにログを埋めないよう、MAX_WARNINGS 件まで (その次は 1 回だけ、以後は出さないと知らせる)
    template <typename Warn>
    void load(const char* text, size_t size, Warn&& warn) {
        count_ = 0;
        size_t pos = 0;
        if (size >= 3 && memcmp(text, "\xef\xbb\xbf", 3) == 0) pos = 3; // BOM
        int warnings = 0;
        auto report = [&](int line, const char* why) {
            if (warnings < MAX_WARNINGS) warn(line, why);
            else if (warnings == MAX_WARNINGS) warn(line, "too many errors; further ones are not reported");
            warnings++;
        };
        for (int line = 1; pos < size; line++) {
            size_t next;
            const size_t end = pad_map_detail::lineEnd(text, size, pos, next);
            PadMap m;
            const PadLineResult r = parsePadLine(text + pos, end - pos, m);
            if (r == PadLineResult::OK) {
                if (!add(m)) report(line, "too many lines");
            } else if (r != PadLineResult::EMPTY) {
                report(line, padLineError(r));
            }
            pos = next;
        }
    }
    // 足す。同じ VID:PID と if= があれば置き換える。いっぱいなら false
    bool add(const PadMap& m) {
        for (int i = 0; i < count_; i++) {
            if (!maps_[i].sameKey(m)) continue;
            maps_[i] = m;
            return true;
        }
        if (count_ >= MAX) return false;
        maps_[count_++] = m;
        return true;
    }
    const PadMap* find(uint16_t vid, uint16_t pid, int iface) const {
        const PadMap* any = nullptr;
        for (int i = 0; i < count_; i++) {
            const PadMap& m = maps_[i];
            if (m.vid != vid || m.pid != pid) continue;
            if (m.iface == iface && iface >= 0) return &m;
            if (m.iface < 0) any = &m;
        }
        return any;
    }
    int count() const { return count_; }
    // add(m) が通るか (置き換えか、空きがある)
    bool canAdd(const PadMap& m) const {
        for (int i = 0; i < count_; i++)
            if (maps_[i].sameKey(m)) return true;
        return count_ < MAX;
    }

private:
    PadMap maps_[MAX];
    int count_ = 0;
};

// ゲームパッドの並びをどれで決めるか: 保存した並び、Circle が知っている配置、内蔵の表、の順
enum class PadMapping { SAVED, CIRCLE, BUILTIN, UNKNOWN };
inline PadMapping choosePadMapping(const PadMap* saved, bool circleKnows, const PadLayout* builtin) {
    if (saved != nullptr) return PadMapping::SAVED;
    if (circleKnows) return PadMapping::CIRCLE;
    return builtin != nullptr ? PadMapping::BUILTIN : PadMapping::UNKNOWN;
}

// s を押しているか。hat0 は最初のハットを HatDecoder で直した値 (hatmin= がないときに使う)
inline bool padSourceActive(const PadSource& s, int hatMin, const PadRaw& raw, int hat0) {
    switch (s.kind) {
    case PadSource::BUTTON:
        return s.index < 32 && (raw.buttons >> s.index) & 1u;
    case PadSource::HAT: {
        if (s.index >= PadRaw::HAT_COUNT || s.index >= raw.nhats) return false;
        int64_t v = hat0;
        if (hatMin >= 0 || s.index != 0) v = (int64_t)raw.hats[s.index] - (hatMin > 0 ? hatMin : 0);
        if (v < 0 || v > 7) return false;
        const int d = (int)(v - s.dir + 8) & 7; // 0 なら同じ向き、1 と 7 は斜め
        return d == 0 || d == 1 || d == 7;
    }
    case PadSource::AXIS: {
        if (s.index >= PadRaw::AXIS_COUNT || s.index >= raw.naxes) return false;
        const PadRaw::Axis& a = raw.axes[s.index];
        const int64_t lo = a.minimum, hi = a.maximum;
        if (hi <= lo) return false;
        // joystickBits() と同じ遊び: 半分の幅の 40%
        const int64_t d = (int64_t)a.value - (lo + (hi - lo) / 2), dead = (hi - lo) * 2 / 10;
        return s.dir ? d > dead : d < -dead;
    }
    default:
        return false;
    }
}

// 生の値を、配置の分かるゲームパッドと同じ形 (known、TGamePadButton のビット) にする。方向も十字キーのビットにするので、
// ハットと軸は見ない
inline GamePadInput applyPadMap(const PadMap& m, const PadRaw& raw, int hat0) {
    // UP、DOWN、LEFT、RIGHT、A、B、START、SELECT、HOME の順
    static const uint32_t kBits[PadMap::ITEMS] = {1u << 15, 1u << 17, 1u << 18, 1u << 16, pad_bit::A,
                                                  pad_bit::B, 1u << 14, pad_bit::SELECT, pad_bit::HOME};
    GamePadInput in;
    in.known = true;
    for (int i = 0; i < PadMap::ITEMS; i++)
        if (padSourceActive(m.src[i], m.hatMin, raw, hat0)) in.buttons |= kBits[i];
    return in;
}

} // namespace platform
