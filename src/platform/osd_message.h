// 画面の左上に重ねて出す短いメッセージ (HTTP の POST /message、ホスト版の --message)。実機で動きを見比べるときに、
// モニタを見ている人に「いま動いているのはどれか」を知らせる。
//
// 本文は UTF-8。半角 (ASCII と半角カタカナ) と全角 (JIS X 0208 の字) を、MSG_LINES 行、1 行 MSG_COLS 桁 (半角の桁)
// まで出す。全角は 2 桁を使う。行や桁からはみ出した分は字の切れ目で捨てる。
//   ASCII       platform/menu_font.h の 8x16 (OSD と同じ。CGROM がなくても出る)
//   半角カタカナ CGROM の 8x16 の半角 ($F3A800 から、1 字 16 バイト)
//   全角        CGROM の 16x16 ($F00000 から、1 字 32 バイト)。Unicode から区点へは platform/jis_table.h で引く
// CGROM がないとき、表にない全角の字、CGROM の字形が空のときは、四角 (豆腐) を出す。表にない半角の字は「?」。
//
// 表示は画面の右上の数字 (platform/osd.h) と同じ仕組みで重ねる: X68000 の画面に重なるところは、写すときに重なる部分だけ
// この絵を書き (platform/frame_scale.h の presentScaled() の 2 つ目の FrameOverlay)、枠の外は写したあとに描く。
// X68000 の画面の中身には書かないので、/screen.ppm にも出ない。右上の数字と同じ行に並ぶときは、数字にかからないよう
// 右を切る (placeMessage() の limitX)。
//
// 流れ: layoutMessage() で字を升目 (MessageLayout) に並べ、rasterMessage() で 1 ビットの字形 (MessageGlyphs) にする
// (ここまでベアメタル版はコア 0)。MessageBox で写す役のコアに渡し、placeMessage() で置く場所を決め、renderMessage() で
// 32 ビットの絵にする。MessageState が、消すか描くかを決める (OsdState と同じ)。
// ベアメタルでも使えるよう、例外、RTTI、STL、stdio は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform/jis_table.h"
#include "platform/menu_font.h"
#include "platform/net_http.h"
#include "platform/osd.h"

namespace platform {

static constexpr int MSG_LINES = 4;            // 行数の上限
static constexpr int MSG_COLS = 40;            // 1 行の桁数の上限 (半角の桁。全角は 2 桁)
static constexpr unsigned MSG_BODY_MAX = 1024; // HTTP の本体の上限 (バイト)
static constexpr unsigned MSG_SEC_DEFAULT = 10; // ?sec= がないときの秒数
static constexpr unsigned MSG_SEC_MAX = 86400;  // ?sec= の上限 (1 日)
static constexpr int MSG_MAX_SCALE = 2;         // 文字の倍率の上限
static constexpr unsigned MSG_LOG_TEXT_MAX = 160; // ログに出す本文の上限 (バイト。字の切れ目で切る)
// 絵の画素数の上限 (いちばん大きい倍率で、MSG_COLS 桁 x MSG_LINES 行)
static constexpr int MSG_IMAGE_PIXELS = MSG_COLS * 8 * MSG_MAX_SCALE * MSG_LINES * 16 * MSG_MAX_SCALE;

// 升目の中身
static constexpr uint16_t MSG_CELL_SPACE = 0x20;   // 0x20-0x7E は ASCII、0xA1-0xDF は半角カタカナ (X68000 の文字コード)
static constexpr uint16_t MSG_CELL_TOFU = 0xfffe;  // 出せない全角の字 (左の桁。右の桁は MSG_CELL_RIGHT)
static constexpr uint16_t MSG_CELL_RIGHT = 0xffff; // 全角の字の右の桁 (0x2121-0x7E7E の区点か MSG_CELL_TOFU の右)

static constexpr uint32_t CGROM_FONT8X16 = 0x3a800; // CGROM の 8x16 の半角 ($F3A800)

// Unicode から JIS X 0208 の区点を引く。なければ 0。表 (platform/jis_table.h) は区点の順なので、端から探す
// (7000 字ほど。HTTP の要求ごとに、コア 0 が 1 回だけ引く)
inline uint16_t unicodeToJis(uint32_t cp) {
    if (cp < 0x80 || cp > 0xffff) return 0;
    for (int i = 0; i < JIS_ALT_SIZE; i++)
        if (kJisAltTable[i][0] == cp) return kJisAltTable[i][1];
    for (int r = 0; r < JIS_TABLE_ROWS; r++)
        for (int c = 0; c < 94; c++)
            if (kJisToUnicode[r][c] == cp) {
                const int row = r < 8 ? 0x21 + r : 0x30 + (r - 8);
                return (uint16_t)(row << 8 | (0x21 + c));
            }
    return 0;
}

// s[i] から UTF-8 の 1 字を読み、i を進める。終わりなら false。正しくない並び (途中で切れた、長すぎる形、サロゲート、
// 0x10FFFF より大きい) は、1 バイトだけ進めて U+FFFD にする
inline bool utf8Next(const char* s, size_t n, size_t& i, uint32_t& cp) {
    if (i >= n) return false;
    const uint8_t c = (uint8_t)s[i];
    if (c < 0x80) {
        cp = c;
        i++;
        return true;
    }
    int len = 0;
    uint32_t v = 0, min = 0;
    if ((c & 0xe0) == 0xc0) {
        len = 2;
        v = c & 0x1f;
        min = 0x80;
    } else if ((c & 0xf0) == 0xe0) {
        len = 3;
        v = c & 0x0f;
        min = 0x800;
    } else if ((c & 0xf8) == 0xf0) {
        len = 4;
        v = c & 0x07;
        min = 0x10000;
    }
    bool ok = len != 0 && i + (size_t)len <= n;
    for (int k = 1; ok && k < len; k++) {
        const uint8_t d = (uint8_t)s[i + (size_t)k];
        if ((d & 0xc0) != 0x80) ok = false;
        else v = v << 6 | (d & 0x3f);
    }
    if (ok && (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))) ok = false;
    if (!ok) {
        cp = 0xfffd;
        i++;
        return true;
    }
    cp = v;
    i += (size_t)len;
    return true;
}

// s の先頭 max バイト以内で、字の切れ目になるいちばん長い長さ (utf8Next() で読む切れ目)
inline size_t utf8Truncate(const char* s, size_t n, size_t max) {
    if (n <= max) return n;
    size_t i = 0, last = 0;
    uint32_t cp;
    while (utf8Next(s, n, i, cp) && i <= max) last = i;
    return last;
}

// 字を並べた升目。cell[行][桁]。cols[行] はその行の桁数 (行末の空白は除く)
struct MessageLayout {
    int lines = 0;
    uint8_t cols[MSG_LINES] = {};
    uint16_t cell[MSG_LINES][MSG_COLS] = {};
};

// UTF-8 の本文を升目に並べる。\n で改行 (\r は捨てる)、TAB は空白、ほかの制御文字は捨てる。桁からはみ出した字は、
// 次の改行まで捨てる (全角の字が 1 桁しか残っていなければ、その字から捨てる)。MSG_LINES 行より後ろは捨てる。
// 終わりの空の行は数えない (本文が空か空白と改行だけなら lines は 0)
inline void layoutMessage(const char* s, size_t n, MessageLayout& out) {
    out = MessageLayout();
    int line = 0, col = 0;
    bool full = false; // この行は桁からはみ出した (改行まで捨てる)
    size_t i = 0;
    uint32_t cp;
    while (line < MSG_LINES && utf8Next(s, n, i, cp)) {
        if (cp == '\n') {
            line++;
            col = 0;
            full = false;
            continue;
        }
        if (full) continue;
        if (cp == '\t') cp = ' ';
        if (cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp < 0xa0)) continue;
        uint16_t code = 0;
        int width = 1;
        if (cp < 0x80) {
            code = (uint16_t)cp;
        } else if (cp == 0xfffd) {
            code = '?'; // 正しくない UTF-8 (utf8Next())
        } else if (cp >= 0xff61 && cp <= 0xff9f) {
            code = (uint16_t)(cp - 0xff61 + 0xa1); // 半角カタカナ
        } else if (const uint16_t jis = unicodeToJis(cp)) {
            code = jis;
            width = 2;
        } else if (cp >= 0x1100) {
            code = MSG_CELL_TOFU; // 表にない全角らしい字 (漢字の拡張、絵文字など)
            width = 2;
        } else {
            code = '?';
        }
        if (col + width > MSG_COLS) {
            full = true;
            continue;
        }
        out.cell[line][col++] = code;
        if (width == 2) out.cell[line][col++] = MSG_CELL_RIGHT;
        // 行末の空白は桁数に数えない (枠を狭くする)
        if (code != MSG_CELL_SPACE) out.cols[line] = (uint8_t)col;
        if (out.cols[line] > 0 && line + 1 > out.lines) out.lines = line + 1;
    }
}

// 1 ビットの字形。bits[行][桁] は 8x16 (1 バイトが 1 ラインで bit7 が左)。lines が 0 なら出すものがない
struct MessageGlyphs {
    uint8_t lines = 0;
    uint8_t cols[MSG_LINES] = {};
    uint8_t reserved[3] = {};
    uint8_t bits[MSG_LINES][MSG_COLS][16] = {};

    int maxCols() const {
        int m = 0;
        for (int i = 0; i < lines; i++)
            if (cols[i] > m) m = cols[i];
        return m;
    }
};
static_assert(sizeof(MessageGlyphs) % 8 == 0, "MessageBox copies 8 bytes at a time");

// 区点 jis の 16x16 の字の CGROM の中の位置。CGROM にない区なら -1。
// CGROM は 1-8 区 (0x21-0x28) のあとに 16 区 (0x30) から続く (9-15 区はない)
inline long cgromFont16Offset(uint16_t jis) {
    const int row = jis >> 8, col = jis & 0xff;
    if (col < 0x21 || col > 0x7e) return -1;
    int r;
    if (row >= 0x21 && row <= 0x28) r = row - 0x21;
    else if (row >= 0x30 && row <= 0x74) r = row - 0x21 - 7;
    else return -1;
    return (long)(r * 94 + (col - 0x21)) * 32;
}

namespace message_detail {
// 豆腐 (16x16 の四角。全角の左と右に分けて 8x16 を 2 つ)
inline void tofu(uint8_t left[16], uint8_t right[16]) {
    for (int y = 0; y < 16; y++) {
        const bool edge = y == 2 || y == 13, inside = y > 2 && y < 13;
        left[y] = edge ? 0x3f : inside ? 0x20 : 0;
        right[y] = edge ? 0xfc : inside ? 0x04 : 0;
    }
}
} // namespace message_detail

// 升目を字形にする。cgrom は CGROM の全体 (cgromSize バイト。なければ nullptr)
inline void rasterMessage(const MessageLayout& lay, const uint8_t* cgrom, size_t cgromSize, MessageGlyphs& out) {
    out = MessageGlyphs();
    out.lines = (uint8_t)lay.lines;
    for (int l = 0; l < lay.lines; l++) {
        out.cols[l] = lay.cols[l];
        for (int c = 0; c < lay.cols[l]; c++) {
            const uint16_t code = lay.cell[l][c];
            uint8_t* g = out.bits[l][c];
            if (code == MSG_CELL_RIGHT) continue; // 左の桁で書いた
            if (code >= MENU_FONT_FIRST && code <= MENU_FONT_LAST) {
                memcpy(g, kMenuFont[code - MENU_FONT_FIRST], 16);
            } else if (code >= 0xa1 && code <= 0xdf) {
                const size_t off = CGROM_FONT8X16 + (size_t)code * 16;
                if (cgrom != nullptr && off + 16 <= cgromSize) memcpy(g, cgrom + off, 16);
                else memcpy(g, kMenuFont['?' - MENU_FONT_FIRST], 16);
            } else if (c + 1 < MSG_COLS) {
                uint8_t* r = out.bits[l][c + 1];
                const long off = code == MSG_CELL_TOFU ? -1 : cgromFont16Offset(code);
                bool drawn = false;
                if (cgrom != nullptr && off >= 0 && (size_t)off + 32 <= cgromSize) {
                    uint8_t any = 0;
                    for (int y = 0; y < 16; y++) {
                        g[y] = cgrom[off + 2 * y];
                        r[y] = cgrom[off + 2 * y + 1];
                        any |= g[y] | r[y];
                    }
                    // 字形が空なら豆腐にする (CGROM にない字。全角の空白 0x2121 は空のまま)
                    drawn = any != 0 || code == 0x2121;
                }
                if (!drawn) message_detail::tofu(g, r);
            }
        }
    }
}

// 文字の倍率。1080 ラインなら 2 倍 (モニタから離れて見ても読めるように)、それより低ければ 1 倍
inline int messageScale(int fbH) {
    return fbH >= 900 ? 2 : 1;
}

// fbW x fbH の画面の左上に置く。X68000 の画面 (picX, picY から picW x picH) に重なれば overlay。右端は limitX
// (右上の数字の左端など。なければ fbW) の OSD_PAD_X 手前まで。入らない桁は右を切る (w が桁の途中で終わることはない)。
// 1 桁も入らなければ w = 0
inline OsdRect placeMessage(int fbW, int fbH, int picX, int picY, int picW, int picH, const MessageGlyphs& g, int scale,
                            int limitX) {
    OsdRect r;
    if (g.lines == 0) return r;
    r.scale = scale < 1 ? 1 : scale > MSG_MAX_SCALE ? MSG_MAX_SCALE : scale;
    if (limitX > fbW) limitX = fbW;
    r.x = OSD_PAD_X * r.scale; // 8 の倍数なので 4 画素にそろっている
    r.y = OSD_PAD_Y * r.scale;
    int cols = g.maxCols();
    const int room = (limitX - OSD_PAD_X * r.scale - r.x) / (8 * r.scale);
    if (cols > room) cols = room;
    int lines = g.lines;
    const int roomLines = (fbH - r.y) / (16 * r.scale);
    if (lines > roomLines) lines = roomLines;
    if (cols <= 0 || lines <= 0) return OsdRect();
    r.w = cols * 8 * r.scale;
    r.h = lines * 16 * r.scale;
    r.overlay = picW > 0 && picH > 0 && r.x < picX + picW && picX < r.x + r.w && r.y < picY + picH && picY < r.y + r.h;
    return r;
}

// 右上の数字 osd と同じ行に並ぶなら、その左端 (でなければ fbW)。placeMessage() の limitX に渡す
inline int messageLimitX(int fbW, int fbH, const OsdRect& osd, const MessageGlyphs& g, int scale) {
    if (osd.w <= 0) return fbW;
    if (scale < 1) scale = 1;
    if (scale > MSG_MAX_SCALE) scale = MSG_MAX_SCALE;
    const int y = OSD_PAD_Y * scale;
    int h = g.lines * 16 * scale;
    if (y + h > fbH) h = fbH - y;
    return osd.y < y + h && y < osd.y + osd.h ? osd.x : fbW;
}

// 字形を r の絵 (r.w x r.h 画素、1 行 r.w 画素、ふつうのメモリ) にする。白の字、不透明の黒の背景 (OSD と同じ)
inline void renderMessage(uint32_t* image, const OsdRect& r, const MessageGlyphs& g) {
    if (r.w <= 0) return;
    const int s = r.scale, cells = r.w / (8 * s), rowH = 16 * s;
    for (int y = 0; y < r.h; y++) {
        uint32_t* line = image + (size_t)y * r.w;
        const int l = y / rowH, gy = (y % rowH) / s;
        int x = 0;
        for (int c = 0; c < cells; c++) {
            const uint8_t bits = l < g.lines && c < g.cols[l] ? g.bits[l][c][gy] : 0;
            for (int b = 0; b < 8; b++) {
                const uint32_t px = (bits & (0x80 >> b)) ? OSD_FG : OSD_BG;
                for (int k = 0; k < s; k++) line[x++] = px;
            }
        }
    }
}

// 出すか。g は字形、deadline は消す時刻 (0 なら消すまで出す)、now はいまの時刻 (同じ時計)
inline bool messageVisible(const MessageGlyphs& g, uint64_t deadline, uint64_t now) {
    return g.lines != 0 && (deadline == 0 || now < deadline);
}

// 描く側が持つ「いま画面に出ているもの」。OsdState と同じく、出したいもの (版の番号 gen) か場所が変わったときだけ描く
struct MessageState {
    OsdRect rect;
    uint32_t gen = 0;
    bool shown = false;

    void invalidate() { shown = false; }
    bool overlaid() const { return shown && rect.overlay; }
    OsdState::Step update(bool want, uint32_t g, const OsdRect& r) {
        OsdState::Step st;
        want = want && r.w > 0;
        if (shown && (!want || r != rect)) {
            st.erase = true;
            st.eraseRect = rect;
        }
        if (!want) {
            shown = false;
            return st;
        }
        st.draw = !shown || r != rect || g != gen;
        rect = r;
        gen = g;
        shown = true;
        return st;
    }
};

// 字形と消す時刻を、書くコア (ベアメタル版はコア 0 の HTTP) から写す役のコアに渡す (seqlock。書くのは 1 つのコアだけ。
// 書いている間は seq が奇数)。deadline は消す時刻 (呼ぶ側の時計。0 なら消すまで出す)
struct MessageBox {
    static constexpr int WORDS = (int)(sizeof(MessageGlyphs) / 8);
    uint64_t words[WORDS] = {};
    uint64_t deadline = 0;
    unsigned seq = 0;

    void publish(const MessageGlyphs& g, uint64_t until) {
        uint64_t tmp[WORDS];
        memcpy(tmp, &g, sizeof g);
        const unsigned s = __atomic_load_n(&seq, __ATOMIC_RELAXED);
        __atomic_store_n(&seq, s + 1, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        for (int i = 0; i < WORDS; i++) __atomic_store_n(&words[i], tmp[i], __ATOMIC_RELAXED);
        __atomic_store_n(&deadline, until, __ATOMIC_RELAXED);
        __atomic_store_n(&seq, s + 2, __ATOMIC_RELEASE);
    }
    // いまの版の番号 (偶数。書いている途中なら奇数)
    unsigned version() const { return __atomic_load_n(&seq, __ATOMIC_ACQUIRE); }
    // 写して、写した版の番号を返す
    unsigned load(MessageGlyphs& g, uint64_t& until) const {
        uint64_t tmp[WORDS];
        for (;;) {
            const unsigned s = __atomic_load_n(&seq, __ATOMIC_ACQUIRE);
            for (int i = 0; i < WORDS; i++) tmp[i] = __atomic_load_n(&words[i], __ATOMIC_RELAXED);
            const uint64_t d = __atomic_load_n(&deadline, __ATOMIC_RELAXED);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (!(s & 1) && __atomic_load_n(&seq, __ATOMIC_RELAXED) == s) {
                memcpy(&g, tmp, sizeof g);
                until = d;
                return s;
            }
        }
    }
};

// POST /message の要求。本体 body (n バイト。\n などのエスケープは戻す前) とクエリから決める
struct MessageRequest {
    unsigned sec = MSG_SEC_DEFAULT; // 出す秒数 (0 なら消すまで)
    bool clear = false;             // 本体が空 (空白と改行だけ) なので消す
    const char* error = nullptr;    // 400 で返す理由 (なければ nullptr)
};

// ?sec=N を読み、本体が空かを見る。sec は 10 進の 0 から MSG_SEC_MAX。それ以外は error を入れて false
inline bool parseMessageRequest(const char* query, const char* body, size_t n, MessageRequest& req) {
    req = MessageRequest();
    char v[16];
    const bool got = queryValue(query, "sec", v, sizeof v);
    // sec= があるのに読めない (長すぎて v に入らない、%xx が正しくない) ときも断る (既定の 10 秒にしない)
    bool present = got;
    for (const char* p = query; !present && *p;) {
        const char* amp = strchr(p, '&');
        const size_t len = amp ? (size_t)(amp - p) : strlen(p);
        if (len >= 3 && memcmp(p, "sec", 3) == 0 && (len == 3 || p[3] == '=')) present = true;
        p += len;
        if (*p == '&') p++;
    }
    if (present) {
        unsigned long x = 0;
        int digits = got ? 0 : -1;
        for (const char* p = v; got && *p; p++) {
            if (*p < '0' || *p > '9' || ++digits > 6) {
                digits = -1;
                break;
            }
            x = x * 10 + (unsigned long)(*p - '0');
        }
        if (digits <= 0 || x > MSG_SEC_MAX) {
            req.error = "sec= is seconds from 0 to 86400 (0 keeps it until cleared)\n";
            return false;
        }
        req.sec = (unsigned)x;
    }
    if (n > MSG_BODY_MAX) {
        req.error = "the body is too long (1024 bytes at most)\n";
        return false;
    }
    // 出す字が 1 つもなければ (空、空白と改行と制御文字だけ) 消す。layoutMessage() と同じ決め方にする
    MessageLayout lay;
    layoutMessage(body, n, lay);
    req.clear = lay.lines == 0;
    return true;
}

// ログの 1 行 (「message: ...」) を out (size バイト) に作り、長さを返す。text はエスケープを戻した本文 (n バイト)。
// 終わりの改行は除き、途中の改行は「\n」の 2 文字、ほかの制御文字と正しくない UTF-8 のバイトは「?」にする (ログの行を
// 偽れないように)。MSG_LOG_TEXT_MAX バイトを超えるか、out に入りきらなければ字の切れ目で切って「...」を付ける
// (閉じの「"」と秒数はいつも残す)
inline size_t formatMessageLog(char* out, size_t size, const char* text, size_t n, unsigned sec, bool clear) {
    if (size == 0) return 0;
    size_t o = 0;
    auto put = [&](const char* s, size_t len) {
        for (size_t k = 0; k < len && o + 1 < size; k++) out[o++] = s[k];
    };
    auto puts = [&](const char* s) { put(s, strlen(s)); };
    puts("message: ");
    if (clear) {
        puts("cleared");
        out[o] = 0;
        return o;
    }
    // 後ろに付ける「 (10 s)」か「 (until cleared)」
    char tail[32];
    size_t tlen = 0;
    if (sec == 0) {
        const char* u = " (until cleared)";
        tlen = strlen(u);
        memcpy(tail, u, tlen);
    } else {
        char d[12];
        int k = 0;
        unsigned v = sec;
        do {
            d[k++] = (char)('0' + v % 10);
            v /= 10;
        } while (v != 0);
        tail[tlen++] = ' ';
        tail[tlen++] = '(';
        while (k > 0) tail[tlen++] = d[--k];
        tail[tlen++] = ' ';
        tail[tlen++] = 's';
        tail[tlen++] = ')';
    }
    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\r')) n--;
    const size_t cut = utf8Truncate(text, n, MSG_LOG_TEXT_MAX);
    put("\"", 1);
    // 本文に使える終わりの位置 (閉じの「"」、「...」、tail、終わりの 0 の分を空ける)
    const size_t reserve = 1 + 3 + tlen + 1;
    const size_t limit = size > o + reserve ? size - reserve : o;
    bool truncated = cut < n;
    size_t i = 0;
    uint32_t cp;
    while (i < cut) {
        const size_t start = i;
        utf8Next(text, cut, i, cp);
        const char* s = text + start;
        size_t len = i - start;
        if (cp == '\n') {
            s = "\\n";
            len = 2;
        } else if (cp == '\r') {
            continue;
        } else if (cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp < 0xa0) || cp == 0xfffd) {
            s = "?";
            len = 1;
        }
        if (o + len > limit) {
            truncated = true;
            break;
        }
        put(s, len);
    }
    put("\"", 1);
    if (truncated) puts("...");
    put(tail, tlen);
    out[o] = 0;
    return o;
}

} // namespace platform
