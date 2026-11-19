// src/platform/osd_message.h のテスト: UTF-8 の読み方と切り方、升目への並べ方 (行と桁の上限、全角の幅)、
// CGROM からの字形 (試験用の CGROM を作る)、置く場所、絵、出すか消すか、コアの間の受け渡し、
// POST /message の要求の読み方 (?sec=、空の本体) とログの 1 行、書く操作にトークンが要ること
#include <string.h>

#include <string>
#include <vector>

#include "platform/net_config.h"
#include "platform/osd_message.h"
#include "test_check.h"

using namespace platform;

namespace {

void layout(const char* s, MessageLayout& out) { layoutMessage(s, strlen(s), out); }

void testJis() {
    CHECK_EQ(unicodeToJis(0x3042), 0x2422); // あ
    CHECK_EQ(unicodeToJis(0x4e9c), 0x3021); // 亜
    CHECK_EQ(unicodeToJis(0x9fa0), 0x737e); // 龠 (2 水準の終わり)
    CHECK_EQ(unicodeToJis(0xff5e), 0x2141); // ～ (Windows の表)
    CHECK_EQ(unicodeToJis(0x301c), 0x2141); // 〜 (JIS の表)
    CHECK_EQ(unicodeToJis(0x3000), 0x2121); // 全角の空白
    CHECK_EQ(unicodeToJis(0x2460), 0);      // ① (NEC の 13 区は CGROM にない)
    CHECK_EQ(unicodeToJis('A'), 0);
    CHECK_EQ(unicodeToJis(0x1f600), 0);
    CHECK_EQ(unicodeToJis(0x2212), 0x215d); // − (JIS の表。cp932 の表では U+FF0D)
    CHECK_EQ(unicodeToJis(0xff0d), 0x215d);
    CHECK_EQ(unicodeToJis(0x582f), 0x7421); // 堯 (84 区)
    // どの区点も、表の Unicode から引き直すと同じ区点になる (1 つの Unicode が 2 つの区点に入っていない)
    int bad = 0;
    for (int r = 0; r < JIS_TABLE_ROWS; r++)
        for (int c = 0; c < 94; c++) {
            const uint16_t u = kJisToUnicode[r][c];
            const int row = r < 8 ? 0x21 + r : 0x30 + (r - 8);
            if (u != 0 && unicodeToJis(u) != (row << 8 | (0x21 + c)) && bad++ < 4)
                printf("  0x%02x%02x: U+%04X\n", row, 0x21 + c, u);
        }
    CHECK_EQ(bad, 0);
    CHECK_EQ(cgromFont16Offset(0x2121), 0);
    CHECK_EQ(cgromFont16Offset(0x2821), 7 * 94 * 32);
    CHECK_EQ(cgromFont16Offset(0x3021), 8 * 94 * 32);
    CHECK_EQ(cgromFont16Offset(0x3022), 8 * 94 * 32 + 32);
    CHECK_EQ(cgromFont16Offset(0x2921), -1); // 9-15 区はない
    CHECK_EQ(cgromFont16Offset(0x7521), -1);
    CHECK_EQ(cgromFont16Offset(0x3020), -1);
    // 最後の区 (0x74) の終わりは 8x16 の半角より前
    CHECK_EQ(cgromFont16Offset(0x747e) + 32 <= (long)CGROM_FONT8X16, 1);
}

void testUtf8() {
    const char s[] = "a\xc3\xa9\xe3\x81\x82\xf0\x9f\x98\x80"; // a é あ 😀
    const size_t n = strlen(s);
    size_t i = 0;
    uint32_t cp = 0;
    CHECK_EQ(utf8Next(s, n, i, cp), 1);
    CHECK_EQ(cp, 'a');
    CHECK_EQ(utf8Next(s, n, i, cp), 1);
    CHECK_EQ(cp, 0xe9);
    CHECK_EQ(utf8Next(s, n, i, cp), 1);
    CHECK_EQ(cp, 0x3042);
    CHECK_EQ(utf8Next(s, n, i, cp), 1);
    CHECK_EQ(cp, 0x1f600);
    CHECK_EQ(i, n);
    CHECK_EQ(utf8Next(s, n, i, cp), 0);
    // 正しくない並びは 1 バイト進めて U+FFFD
    const char* bad[] = {"\x80", "\xc0\x80", "\xe3\x81", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xff"};
    for (const char* b : bad) {
        i = 0;
        CHECK_EQ(utf8Next(b, strlen(b), i, cp), 1);
        CHECK_EQ(cp, 0xfffd);
        CHECK_EQ(i, 1);
    }
    // 字の切れ目で切る
    const char* t = "a\xe3\x81\x82" "b"; // aあb (5 バイト)
    CHECK_EQ(utf8Truncate(t, 5, 10), 5);
    CHECK_EQ(utf8Truncate(t, 5, 5), 5);
    CHECK_EQ(utf8Truncate(t, 5, 4), 4);
    CHECK_EQ(utf8Truncate(t, 5, 3), 1);
    CHECK_EQ(utf8Truncate(t, 5, 2), 1);
    CHECK_EQ(utf8Truncate(t, 5, 1), 1);
    CHECK_EQ(utf8Truncate(t, 5, 0), 0);
}

void testLayout() {
    MessageLayout l;
    layout("Hi", l);
    CHECK_EQ(l.lines, 1);
    CHECK_EQ(l.cols[0], 2);
    CHECK_EQ(l.cell[0][0], 'H');
    CHECK_EQ(l.cell[0][1], 'i');

    // 全角は 2 桁
    layout("A\xe3\x81\x82" "B", l); // AあB
    CHECK_EQ(l.cols[0], 4);
    CHECK_EQ(l.cell[0][1], 0x2422);
    CHECK_EQ(l.cell[0][2], MSG_CELL_RIGHT);
    CHECK_EQ(l.cell[0][3], 'B');

    // 改行、\r は捨てる、TAB は空白、ほかの制御文字は捨てる、行末の空白は数えない
    layout("ab\r\nc\td\x01 \n", l);
    CHECK_EQ(l.lines, 2);
    CHECK_EQ(l.cols[0], 2);
    CHECK_EQ(l.cols[1], 3);
    CHECK_EQ(l.cell[1][1], ' ');
    CHECK_EQ(l.cell[1][2], 'd');

    // 空、空白と改行だけ
    layout("", l);
    CHECK_EQ(l.lines, 0);
    layout(" \n\n  \n", l);
    CHECK_EQ(l.lines, 0);
    // 途中の空の行は残し、終わりの空の行は数えない
    layout("a\n\nb\n\n\n", l);
    CHECK_EQ(l.lines, 3);
    CHECK_EQ(l.cols[1], 0);

    // MSG_LINES 行より後ろは捨て、最後の行の終わりを「…」にする
    CHECK_EQ(unicodeToJis(0x2026), MSG_JIS_ELLIPSIS);
    layout("1\n2\n3\n4\n5\n6", l);
    CHECK_EQ(l.lines, MSG_LINES);
    CHECK_EQ(l.cell[MSG_LINES - 1][0], '0' + MSG_LINES);
    CHECK_EQ(l.cols[MSG_LINES - 1], 3);
    CHECK_EQ(l.cell[MSG_LINES - 1][1], MSG_JIS_ELLIPSIS);
    CHECK_EQ(l.cell[MSG_LINES - 1][2], MSG_CELL_RIGHT);
    // ちょうど MSG_LINES 行なら「…」は付けない (後ろの改行と空白は入りきらない字に数えない)
    layout("1\n2\n3\n4\n \n\n", l);
    CHECK_EQ(l.lines, MSG_LINES);
    CHECK_EQ(l.cols[MSG_LINES - 1], 1);
    CHECK_EQ(l.cell[MSG_LINES - 1][1], 0);
    // 後ろの行が空でも、入りきらない字があれば「…」
    layout("a\n\n\n\nb", l);
    CHECK_EQ(l.lines, MSG_LINES);
    CHECK_EQ(l.cols[MSG_LINES - 1], 2);
    CHECK_EQ(l.cell[MSG_LINES - 1][0], MSG_JIS_ELLIPSIS);

    // MSG_COLS 桁で折り返す
    CHECK_EQ(MSG_COLS, 10);
    layout("0123456789abc", l);
    CHECK_EQ(l.lines, 2);
    CHECK_EQ(l.cols[0], MSG_COLS);
    CHECK_EQ(l.cols[1], 3);
    CHECK_EQ(l.cell[1][0], 'a');
    // ちょうど MSG_COLS 桁のあとの改行は、空の行を作らない
    layout("0123456789\nab", l);
    CHECK_EQ(l.lines, 2);
    CHECK_EQ(l.cell[1][0], 'a');
    // 折り返しで行頭に来る空白は捨てる
    layout("0123456789 ab", l);
    CHECK_EQ(l.lines, 2);
    CHECK_EQ(l.cell[1][0], 'a');
    layout("0123456789   ab", l);
    CHECK_EQ(l.cell[1][0], 'a');
    layout("0123456789\n  ab", l); // 改行のあとの空白は残す
    CHECK_EQ(l.cell[1][2], 'a');
    // 10 桁ちょうどの行のあとの空白と改行で、行が 2 つ進まない
    layout("0123456789 \nab", l);
    CHECK_EQ(l.lines, 2);
    CHECK_EQ(l.cell[1][0], 'a');
    layout("aaaaaaaaaa \nb\nc\nd", l);
    CHECK_EQ(l.lines, MSG_LINES);
    CHECK_EQ(l.cols[MSG_LINES - 1], 1);
    CHECK_EQ(l.cell[MSG_LINES - 1][0], 'd');
    layout("0123456789 \na\nb\nc", l);
    CHECK_EQ(l.lines, MSG_LINES);
    CHECK_EQ(l.cols[MSG_LINES - 1], 1);
    CHECK_EQ(l.cell[MSG_LINES - 1][0], 'c');
    // 全角の字で折り返した行の頭の空白ではない (全角のあとの空白は残す)
    layout("012345678\xe3\x81\x82 b", l);
    CHECK_EQ(l.cell[1][2], ' ');
    CHECK_EQ(l.cell[1][3], 'b');
    // 全角は 5 字で 1 行
    std::string k;
    for (int i = 0; i < 6; i++) k += "\xe3\x81\x82"; // あ x 6
    layout(k.c_str(), l);
    CHECK_EQ(l.lines, 2);
    CHECK_EQ(l.cols[0], 10);
    CHECK_EQ(l.cols[1], 2);
    CHECK_EQ(l.cell[1][0], 0x2422);
    // 全角の字が 1 桁しか残らなければ、その字から次の行へ (半分だけ出さない)
    std::string w(MSG_COLS - 1, 'x');
    w += "\xe3\x81\x82" "z"; // あ z
    layout(w.c_str(), l);
    CHECK_EQ(l.lines, 2);
    CHECK_EQ(l.cols[0], MSG_COLS - 1);
    CHECK_EQ(l.cell[0][MSG_COLS - 1], 0);
    CHECK_EQ(l.cell[1][0], 0x2422);
    CHECK_EQ(l.cell[1][1], MSG_CELL_RIGHT);
    CHECK_EQ(l.cell[1][2], 'z');
    // ちょうど入る全角
    std::string w2(MSG_COLS - 2, 'x');
    w2 += "\xe3\x81\x82";
    layout(w2.c_str(), l);
    CHECK_EQ(l.lines, 1);
    CHECK_EQ(l.cols[0], MSG_COLS);
    CHECK_EQ(l.cell[0][MSG_COLS - 2], 0x2422);
    CHECK_EQ(l.cell[0][MSG_COLS - 1], MSG_CELL_RIGHT);

    // 4 行を超える長い本文: 最後の行の 9、10 桁目を消して「…」を 9-10 桁目に置く
    layout(std::string(MSG_COLS * MSG_LINES + 1, 'x').c_str(), l);
    CHECK_EQ(l.lines, MSG_LINES);
    for (int i = 0; i < MSG_LINES - 1; i++) CHECK_EQ(l.cols[i], MSG_COLS);
    CHECK_EQ(l.cols[MSG_LINES - 1], MSG_COLS);
    CHECK_EQ(l.cell[MSG_LINES - 1][MSG_COLS - 3], 'x');
    CHECK_EQ(l.cell[MSG_LINES - 1][MSG_COLS - 2], MSG_JIS_ELLIPSIS);
    CHECK_EQ(l.cell[MSG_LINES - 1][MSG_COLS - 1], MSG_CELL_RIGHT);
    // ちょうど 4 行に入れば「…」は付けない
    layout(std::string(MSG_COLS * MSG_LINES, 'x').c_str(), l);
    CHECK_EQ(l.cell[MSG_LINES - 1][MSG_COLS - 1], 'x');
    // 最後の行が全角で埋まっていれば、全角を 1 字消して「…」 (全角の右半分から始めない)
    std::string kk;
    for (int i = 0; i < 5 * MSG_LINES + 1; i++) kk += "\xe3\x81\x82";
    layout(kk.c_str(), l);
    CHECK_EQ(l.lines, MSG_LINES);
    CHECK_EQ(l.cols[MSG_LINES - 1], MSG_COLS);
    CHECK_EQ(l.cell[MSG_LINES - 1][MSG_COLS - 4], 0x2422);
    CHECK_EQ(l.cell[MSG_LINES - 1][MSG_COLS - 2], MSG_JIS_ELLIPSIS);
    // 最後の行が半角 9 桁 + 次の行へ送られた全角: 9 桁目を消して「…」は 9-10 桁目
    std::string h = "1\n2\n3\n" + std::string(MSG_COLS - 1, 'y') + "\xe3\x81\x82";
    layout(h.c_str(), l);
    CHECK_EQ(l.cols[MSG_LINES - 1], MSG_COLS);
    CHECK_EQ(l.cell[MSG_LINES - 1][MSG_COLS - 3], 'y');
    CHECK_EQ(l.cell[MSG_LINES - 1][MSG_COLS - 2], MSG_JIS_ELLIPSIS);
    // 4 行目がちょうど 10 桁 (abcde あ fgh) なら「…」は付けない
    std::string h2 = "1\n2\n3\nabcde\xe3\x81\x82" "fgh";
    layout(h2.c_str(), l);
    CHECK_EQ(l.cols[MSG_LINES - 1], MSG_COLS);
    CHECK_EQ(l.cell[MSG_LINES - 1][7], 'f');
    CHECK_EQ(l.cell[MSG_LINES - 1][9], 'h');
    // 「…」を置く 9 桁目が全角の右半分なら、その全角の左 (8 桁目) から「…」
    h2 = "1\n2\n3\nabcdefg\xe3\x81\x82" "\nz"; // 4 行目は abcdefg あ (9 桁目が右半分) の次に 5 行目
    layout(h2.c_str(), l);
    CHECK_EQ(l.cols[MSG_LINES - 1], MSG_COLS - 1);
    CHECK_EQ(l.cell[MSG_LINES - 1][6], 'g');
    CHECK_EQ(l.cell[MSG_LINES - 1][7], MSG_JIS_ELLIPSIS);
    CHECK_EQ(l.cell[MSG_LINES - 1][8], MSG_CELL_RIGHT);
    CHECK_EQ(l.cell[MSG_LINES - 1][9], 0);

    // 半角カタカナ (ｱ = U+FF71 は X68000 の 0xB1)、表にない全角 (絵文字) は豆腐、表にない半角は ?
    layout("\xef\xbd\xb1\xf0\x9f\x98\x80\xc2\xa5", l); // ｱ 😀 ¥
    CHECK_EQ(l.cols[0], 4);
    CHECK_EQ(l.cell[0][0], 0xb1);
    CHECK_EQ(l.cell[0][1], MSG_CELL_TOFU);
    CHECK_EQ(l.cell[0][2], MSG_CELL_RIGHT);
    CHECK_EQ(l.cell[0][3], '?');
    // 正しくない UTF-8 は ?
    layout("a\xff" "b", l);
    CHECK_EQ(l.cols[0], 3);
    CHECK_EQ(l.cell[0][1], '?');
}

// 試験用の CGROM。あ (0x2422) の左は 0x10 + ライン、右は 0x80 | ライン、ｱ (0xB1) は 0x40 + ライン
std::vector<uint8_t> fakeCgrom() {
    std::vector<uint8_t> cg(786432, 0);
    const long a = cgromFont16Offset(0x2422);
    for (int y = 0; y < 16; y++) {
        cg[(size_t)a + 2 * y] = (uint8_t)(0x10 + y);
        cg[(size_t)a + 2 * y + 1] = (uint8_t)(0x80 | y);
    }
    for (int y = 0; y < 16; y++) cg[CGROM_FONT8X16 + 0xb1 * 16 + y] = (uint8_t)(0x40 + y);
    return cg;
}

bool isTofu(const uint8_t* l, const uint8_t* r) {
    uint8_t tl[16], tr[16];
    message_detail::tofu(tl, tr);
    return memcmp(l, tl, 16) == 0 && memcmp(r, tr, 16) == 0;
}

void testRaster() {
    const std::vector<uint8_t> cg = fakeCgrom();
    MessageLayout l;
    MessageGlyphs g;
    layout("A\xe3\x81\x82\xef\xbd\xb1\xe4\xba\x9c\xe3\x80\x80", l); // A あ ｱ 亜 (CGROM で空) 全角の空白
    rasterMessage(l, cg.data(), cg.size(), g);
    CHECK_EQ(g.lines, 1);
    CHECK_EQ(g.cols[0], 8);
    CHECK_EQ(memcmp(g.bits[0][0], kMenuFont['A' - MENU_FONT_FIRST], 16), 0);
    CHECK_EQ(g.bits[0][1][3], 0x13);
    CHECK_EQ(g.bits[0][2][3], 0x83);
    CHECK_EQ(g.bits[0][3][5], 0x45);
    CHECK_EQ(isTofu(g.bits[0][4], g.bits[0][5]), 1); // 字形が空なら豆腐
    uint8_t zero[16] = {};
    CHECK_EQ(memcmp(g.bits[0][6], zero, 16), 0); // 全角の空白は空のまま
    CHECK_EQ(memcmp(g.bits[0][7], zero, 16), 0);

    // CGROM がなければ、全角は豆腐、半角カタカナは ?、ASCII はそのまま
    rasterMessage(l, nullptr, 0, g);
    CHECK_EQ(memcmp(g.bits[0][0], kMenuFont['A' - MENU_FONT_FIRST], 16), 0);
    CHECK_EQ(isTofu(g.bits[0][1], g.bits[0][2]), 1);
    CHECK_EQ(memcmp(g.bits[0][3], kMenuFont['?' - MENU_FONT_FIRST], 16), 0);
    CHECK_EQ(isTofu(g.bits[0][6], g.bits[0][7]), 1);
    // 短すぎる CGROM も読まない
    rasterMessage(l, cg.data(), 1000, g);
    CHECK_EQ(isTofu(g.bits[0][1], g.bits[0][2]), 1);
    CHECK_EQ(g.maxCols(), 8);

    // 「…」は CGROM がなくても豆腐にしない (点を 3 つ)
    layout("1\n2\n3\n4\n5", l);
    rasterMessage(l, nullptr, 0, g);
    CHECK_EQ(g.lines, MSG_LINES);
    CHECK_EQ(isTofu(g.bits[MSG_LINES - 1][1], g.bits[MSG_LINES - 1][2]), 0);
    CHECK_EQ(g.bits[MSG_LINES - 1][1][11] != 0, 1);
    CHECK_EQ(g.bits[MSG_LINES - 1][2][12] != 0, 1);
    CHECK_EQ(g.bits[MSG_LINES - 1][1][5], 0);
}

MessageGlyphs glyphsOf(const char* s, const uint8_t* cg = nullptr, size_t cgSize = 0) {
    MessageLayout l;
    MessageGlyphs g;
    layout(s, l);
    rasterMessage(l, cg, cgSize, g);
    return g;
}

void testPlace() {
    const MessageGlyphs g = glyphsOf("0123456789\nab");
    // 1920x1080、X68000 の画面は真ん中 (左に余白 448)。2 倍で左上の余白に入る
    OsdRect r = placeMessage(1920, 1080, 448, 28, 1024, 1024, g, messageScale(1080), 1920);
    CHECK_EQ(r.scale, 2);
    CHECK_EQ(r.x, 16);
    CHECK_EQ(r.y, 8);
    CHECK_EQ(r.w, 10 * 16);
    CHECK_EQ(r.h, 2 * 32);
    CHECK_EQ(r.overlay, 0);
    CHECK_EQ(r.x % 4, 0);
    // 画面いっぱいなら重ねる
    r = placeMessage(640, 480, 0, 0, 640, 480, g, messageScale(480), 640);
    CHECK_EQ(r.scale, 1);
    CHECK_EQ(r.x, 8);
    CHECK_EQ(r.w, 80);
    CHECK_EQ(r.overlay, 1);
    // 右の limitX に入らない桁は切る (桁の途中では切らない)
    r = placeMessage(640, 480, 0, 0, 640, 480, g, 1, 8 + 8 + 4 * 8 + 3);
    CHECK_EQ(r.w, 4 * 8);
    r = placeMessage(640, 480, 0, 0, 640, 480, g, 1, 16);
    CHECK_EQ(r.w, 0);
    // 倍率は上限まで
    r = placeMessage(1920, 1080, 0, 0, 1920, 1080, g, 9, 1920);
    CHECK_EQ(r.scale, MSG_MAX_SCALE);
    // 何もなければ置かない
    r = placeMessage(640, 480, 0, 0, 640, 480, MessageGlyphs(), 1, 640);
    CHECK_EQ(r.w, 0);

    // 右上の数字と同じ行に並ぶときは、その左端まで
    OsdRect osd = placeOsd(640, 480, 0, 0, 640, 480, 1, OSD_CHARS_BASE);
    CHECK_EQ(messageLimitX(640, 480, osd, g, 1), osd.x);
    CHECK_EQ(messageLimitX(640, 480, OsdRect(), g, 1), 640);
    osd.y = 200; // 行が違えば切らない
    CHECK_EQ(messageLimitX(640, 480, osd, g, 1), 640);
    // MSG_COLS 桁でも、640 の画面で右上の数字にかからない
    const MessageGlyphs wide = glyphsOf(std::string(MSG_COLS, 'w').c_str());
    osd = placeOsd(640, 480, 0, 0, 640, 480, 1, OSD_CHARS);
    r = placeMessage(640, 480, 0, 0, 640, 480, wide, 1, messageLimitX(640, 480, osd, wide, 1));
    CHECK_EQ(r.x + r.w + OSD_PAD_X <= osd.x, 1);
    CHECK_EQ(r.w > 0, 1);
}

void testRender() {
    const MessageGlyphs g = glyphsOf("I\nab");
    const OsdRect r = placeMessage(640, 480, 0, 0, 640, 480, g, 2, 640);
    CHECK_EQ(r.w, 2 * 16);
    CHECK_EQ(r.h, 2 * 32);
    std::vector<uint32_t> img((size_t)r.w * r.h, 0x12345678u);
    renderMessage(img.data(), r, g);
    // どの画素も白か黒
    int fg = 0;
    for (uint32_t p : img) {
        if (p != OSD_FG && p != OSD_BG) {
            CHECK_EQ(p, OSD_FG);
            break;
        }
        fg += p == OSD_FG;
    }
    CHECK_EQ(fg > 0, 1);
    // 1 行目の 2 桁目 (I の行は 1 桁) は背景
    for (int y = 0; y < 32; y++)
        for (int x = 16; x < 32; x++) CHECK_EQ(img[(size_t)y * r.w + x], OSD_BG);
    // 字形のビットどおり (倍率 2)
    const uint8_t* a = kMenuFont['a' - MENU_FONT_FIRST];
    for (int gy = 0; gy < 16; gy++)
        for (int b = 0; b < 8; b++) {
            const uint32_t want = (a[gy] & (0x80 >> b)) ? OSD_FG : OSD_BG;
            CHECK_EQ(img[(size_t)(32 + gy * 2 + 1) * r.w + b * 2 + 1], want);
        }
}

void testState() {
    MessageGlyphs g = glyphsOf("hello");
    const OsdRect r = placeMessage(640, 480, 0, 0, 640, 480, g, 1, 640);
    MessageState st;
    // 出す
    OsdState::Step s = st.update(messageVisible(g, 0, 100), 2, r);
    CHECK_EQ(s.draw, 1);
    CHECK_EQ(s.erase, 0);
    CHECK_EQ(st.overlaid(), 1);
    // 変わらなければ描かない
    s = st.update(messageVisible(g, 0, 200), 2, r);
    CHECK_EQ(s.draw, 0);
    CHECK_EQ(s.erase, 0);
    // 新しいメッセージ (版が変わる) は描き直す
    s = st.update(true, 4, r);
    CHECK_EQ(s.draw, 1);
    CHECK_EQ(s.erase, 0);
    // 場所が変われば、前の場所を消して描く
    OsdRect r2 = r;
    r2.w -= 8;
    s = st.update(true, 4, r2);
    CHECK_EQ(s.erase, 1);
    CHECK_EQ(s.eraseRect == r, 1);
    CHECK_EQ(s.draw, 1);
    // 時刻を過ぎたら消す
    CHECK_EQ(messageVisible(g, 1000, 999), 1);
    CHECK_EQ(messageVisible(g, 1000, 1000), 0);
    s = st.update(messageVisible(g, 1000, 1000), 4, r2);
    CHECK_EQ(s.erase, 1);
    CHECK_EQ(s.draw, 0);
    CHECK_EQ(st.overlaid(), 0);
    // 消えたあとは何もしない
    s = st.update(false, 4, r2);
    CHECK_EQ(s.erase, 0);
    CHECK_EQ(s.draw, 0);
    // 空のメッセージ (消す) は出さない
    CHECK_EQ(messageVisible(MessageGlyphs(), 0, 0), 0);
    // 枠の外を塗り直したら (invalidate) 描き直す
    st.update(true, 6, r);
    st.invalidate();
    s = st.update(true, 6, r);
    CHECK_EQ(s.draw, 1);
    CHECK_EQ(s.erase, 0);
}

void testBox() {
    MessageBox box;
    MessageGlyphs out;
    uint64_t until = 99;
    CHECK_EQ(box.version(), 0);
    CHECK_EQ(box.load(out, until), 0);
    CHECK_EQ(out.lines, 0);
    CHECK_EQ(until, 0);
    const MessageGlyphs g = glyphsOf("ab\ncd");
    box.publish(g, 12345);
    CHECK_EQ(box.version(), 2);
    CHECK_EQ(box.load(out, until), 2);
    CHECK_EQ(memcmp(&out, &g, sizeof g), 0);
    CHECK_EQ(until, 12345);
    box.publish(MessageGlyphs(), 0);
    CHECK_EQ(box.load(out, until), 4);
    CHECK_EQ(out.lines, 0);
}

void testRequest() {
    MessageRequest q;
    CHECK_EQ(parseMessageRequest("", "hi", 2, q), 1);
    CHECK_EQ(q.sec, MSG_SEC_DEFAULT);
    CHECK_EQ(q.clear, 0);
    CHECK_EQ(parseMessageRequest("sec=0", "hi", 2, q), 1);
    CHECK_EQ(q.sec, 0);
    CHECK_EQ(parseMessageRequest("x=1&sec=30", "hi", 2, q), 1);
    CHECK_EQ(q.sec, 30);
    CHECK_EQ(parseMessageRequest("sec=86400", "hi", 2, q), 1);
    CHECK_EQ(q.sec, 86400);
    const char* bad[] = {"sec=86401", "sec=abc", "sec=-1", "sec=", "sec", "sec=1234567", "sec=1.5", "sec=%31x"};
    for (const char* b : bad) {
        CHECK_EQ(parseMessageRequest(b, "hi", 2, q), 0);
        CHECK_EQ(q.error != nullptr, 1);
    }
    // v (16 バイト) に入らない長い値や、戻せない %xx も、既定の秒数にせずに断る
    const char* bad2[] = {"sec=0000000000000001", "sec=1234567890123456789", "sec=%zz", "a=1&sec=00000000000000005"};
    for (const char* b : bad2) {
        CHECK_EQ(parseMessageRequest(b, "hi", 2, q), 0);
        CHECK_EQ(q.error != nullptr, 1);
    }
    CHECK_EQ(parseMessageRequest("second=5&secs=7", "hi", 2, q), 1); // sec= ではない
    CHECK_EQ(q.sec, MSG_SEC_DEFAULT);
    CHECK_EQ(parseMessageRequest("sec=%35", "hi", 2, q), 1); // %xx は戻す
    CHECK_EQ(q.sec, 5);
    // 空の本体、空白と改行だけの本体は消す
    CHECK_EQ(parseMessageRequest("", "", 0, q), 1);
    CHECK_EQ(q.clear, 1);
    CHECK_EQ(parseMessageRequest("sec=5", " \r\n\t", 4, q), 1);
    CHECK_EQ(q.clear, 1);
    // 制御文字だけ、出せる行より後ろにしか字がない本体も、何も出ないので消す (「出した」と返さない)
    CHECK_EQ(parseMessageRequest("", "\x1b\x01\x7f", 3, q), 1);
    CHECK_EQ(q.clear, 1);
    CHECK_EQ(parseMessageRequest("", "\n\n\n\nx", 5, q), 1);
    CHECK_EQ(q.clear, 1);
    CHECK_EQ(parseMessageRequest("", "\xe3\x81\x82", 3, q), 1);
    CHECK_EQ(q.clear, 0);
    // 長すぎる本体
    std::vector<char> big(MSG_BODY_MAX + 1, 'x');
    CHECK_EQ(parseMessageRequest("", big.data(), big.size(), q), 0);
    CHECK_EQ(parseMessageRequest("", big.data(), MSG_BODY_MAX, q), 1);

    // POST はトークンが要る (net_token= があれば、ないか違えば断る。/type と同じ)
    const uint8_t ip[4] = {192, 168, 1, 2};
    CHECK_EQ((int)checkNetAccess("", "tok", ip, "", NetOp::WRITE), (int)NetAccess::DENIED_TOKEN);
    CHECK_EQ((int)checkNetAccess("", "tok", ip, "bad", NetOp::WRITE), (int)NetAccess::DENIED_TOKEN);
    CHECK_EQ((int)checkNetAccess("", "tok", ip, "tok", NetOp::WRITE), (int)NetAccess::OK);
    CHECK_EQ((int)checkNetAccess("", "", ip, "", NetOp::WRITE), (int)NetAccess::NO_WRITE_RULE);
}

std::string logOf(const char* s, unsigned sec, bool clear = false) {
    char out[256];
    const size_t n = formatMessageLog(out, sizeof out, s, strlen(s), sec, clear);
    CHECK_EQ(n, strlen(out));
    return out;
}

void checkStr(const std::string& got, const char* want) {
    if (got != want) {
        printf("  got  [%s]\n  want [%s]\n", got.c_str(), want);
        failures++;
    }
}

bool endsWith(const std::string& s, const char* t) {
    const size_t n = strlen(t);
    return s.size() >= n && s.compare(s.size() - n, n, t) == 0;
}

void testLog() {
    checkStr(logOf("hello", 10), "message: \"hello\" (10 s)");
    checkStr(logOf("a\nb\n\r\n", 5), "message: \"a\\nb\" (5 s)");
    checkStr(logOf("\xe3\x81\x82\x01\x1b" "z\xff", 0), "message: \"\xe3\x81\x82??z?\" (until cleared)");
    checkStr(logOf("x", 3, true), "message: cleared");
    // 長い本文は字の切れ目で切って ... を付ける (あ は 3 バイト。160 バイトなら 53 字)
    std::string longText;
    for (int i = 0; i < 100; i++) longText += "\xe3\x81\x82";
    std::string want = "message: \"";
    for (int i = 0; i < 53; i++) want += "\xe3\x81\x82";
    want += "\"... (10 s)";
    checkStr(logOf(longText.c_str(), 10), want.c_str());
    // 改行が多くて out に入りきらなくても、閉じの " と秒数は残す (\n は 2 文字になるので 160 バイトでも 256 を超える)
    {
        std::string many(150, '\n');
        many = "a" + many + "b";
        char out[256];
        const size_t n = formatMessageLog(out, sizeof out, many.c_str(), many.size(), 10, false);
        CHECK_EQ(n < sizeof out, 1);
        const std::string got(out);
        CHECK_EQ(endsWith(got, "\"... (10 s)"), 1);
        char out2[64];
        formatMessageLog(out2, sizeof out2, many.c_str(), many.size(), 0, false);
        const std::string got2(out2);
        CHECK_EQ(endsWith(got2, "\"... (until cleared)"), 1);
        // 全角の字は途中で切らない
        std::string kana;
        for (int i = 0; i < 40; i++) kana += "\xe3\x81\x82";
        char out3[48];
        formatMessageLog(out3, sizeof out3, kana.c_str(), kana.size(), 5, false);
        size_t j = 0;
        uint32_t cp;
        const std::string g3(out3);
        bool valid = true;
        while (utf8Next(g3.c_str(), g3.size(), j, cp)) valid = valid && cp != 0xfffd;
        CHECK_EQ(valid, 1);
        CHECK_EQ(endsWith(g3, "\"... (5 s)"), 1);
    }
    // 小さな出力でもあふれない
    char small[8];
    CHECK_EQ(formatMessageLog(small, sizeof small, "hello", 5, 10, false), 7);
    CHECK_EQ(small[7], 0);
}

} // namespace

int main() {
    testJis();
    testUtf8();
    testLayout();
    testRaster();
    testPlace();
    testRender();
    testState();
    testBox();
    testRequest();
    testLog();
    return finish();
}
