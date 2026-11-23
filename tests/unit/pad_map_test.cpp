// platform/pad_map.h (pads.txt: ゲームパッドのボタンの並び) の単体テスト
#include <stdio.h>
#include <string.h>

#include <string>

#include "platform/pad_map.h"

static int g_fail = 0;
#define CHECK(c)                                                  \
    do {                                                          \
        if (!(c)) {                                               \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); \
            g_fail++;                                             \
        }                                                         \
    } while (0)

using namespace platform;

static PadLineResult parse(const char* s, PadMap& m) { return parsePadLine(s, strlen(s), m); }
static const char kPce[] =
    "0f0d:0138 if=0 up=h0.0 right=h0.2 down=h0.4 left=h0.6 a=b2 b=b1 select=b8 start=b9 # PCEngine PAD";
#define DIRS " up=h0.0 right=h0.2 down=h0.4 left=h0.6"

static void testParse() {
    PadMap m;
    CHECK(parse(kPce, m) == PadLineResult::OK);
    CHECK(m.vid == 0x0f0d && m.pid == 0x0138 && m.iface == 0 && m.hatMin == -1);
    CHECK(m.src[PadMap::A].kind == PadSource::BUTTON && m.src[PadMap::A].index == 2);
    CHECK(m.src[PadMap::RIGHT].kind == PadSource::HAT && m.src[PadMap::RIGHT].dir == 2);
    CHECK(m.src[PadMap::HOME].kind == PadSource::NONE);
    CHECK(parse("\t ABCD:00fF a=b31 left=x0- right=x0+ up=x15- down=x1+ hatmin=1\r", m) == PadLineResult::OK);
    CHECK(m.vid == 0xabcd && m.pid == 0xff && m.iface == -1 && m.hatMin == 1);
    CHECK(m.src[PadMap::UP].index == 15 && m.src[PadMap::UP].dir == 0 && m.src[PadMap::DOWN].dir == 1);
    CHECK(parse("", m) == PadLineResult::EMPTY);
    CHECK(parse("  # x=y", m) == PadLineResult::EMPTY);
    CHECK(m.vid == 0xabcd); // 使えない行では書き換えない
    const struct {
        const char* s;
        PadLineResult r;
    } bad[] = {
        {"hello", PadLineResult::BAD_ID},
        {"0f0d", PadLineResult::BAD_ID},
        {"0f0d:138 a=b1", PadLineResult::BAD_ID},
        {"0f0d:01380 a=b1", PadLineResult::BAD_ID},
        {"0f0g:0138 a=b1", PadLineResult::BAD_ID},
        {"0f0d:0138", PadLineResult::MISSING},
        {"0f0d:0138" DIRS, PadLineResult::MISSING},
        {"0f0d:0138 a=b1 up=b2 down=b3 left=b4", PadLineResult::MISSING},
        {"0f0d:0138" DIRS " a=b32", PadLineResult::BAD_FIELD},
        {"0f0d:0138" DIRS " a=b", PadLineResult::BAD_FIELD},
        {"0f0d:0138" DIRS " a=b1x", PadLineResult::BAD_FIELD},
        {"0f0d:0138" DIRS " a=b0001", PadLineResult::BAD_FIELD},
        {"0f0d:0138" DIRS " a=b-1", PadLineResult::BAD_FIELD},
        {"0f0d:0138" DIRS " a=", PadLineResult::BAD_FIELD},
        {"0f0d:0138" DIRS " a", PadLineResult::BAD_FIELD},
        {"0f0d:0138" DIRS " a=h0.1", PadLineResult::BAD_FIELD},
        {"0f0d:0138" DIRS " a=h0.8", PadLineResult::BAD_FIELD},
        {"0f0d:0138" DIRS " a=h6.0", PadLineResult::BAD_FIELD},
        {"0f0d:0138" DIRS " a=h0", PadLineResult::BAD_FIELD},
        {"0f0d:0138" DIRS " a=x16+", PadLineResult::BAD_FIELD},
        {"0f0d:0138" DIRS " a=x+", PadLineResult::BAD_FIELD},
        {"0f0d:0138" DIRS " a=x0", PadLineResult::BAD_FIELD},
        {"0f0d:0138" DIRS " z=b0", PadLineResult::BAD_FIELD},
        {"0f0d:0138" DIRS " a=b1 if=256", PadLineResult::BAD_FIELD},
        {"0f0d:0138" DIRS " a=b1 hatmin=2", PadLineResult::BAD_FIELD},
        {"0f0d:0138" DIRS " a=b1 a=b2", PadLineResult::REPEATED},
        {"0f0d:0138 if=0 if=1" DIRS " a=b1", PadLineResult::REPEATED},
        {"0f0d:0138" DIRS " a=b1 b=b1", PadLineResult::SHARED},
        {"0f0d:0138" DIRS " a=h0.2", PadLineResult::SHARED},
    };
    for (const auto& b : bad) {
        const PadLineResult r = parse(b.s, m);
        if (r != b.r) printf("%s\n", b.s);
        CHECK(r == b.r);
        CHECK(padLineError(r)[0] != 0);
    }
    const std::string lng = std::string(kPce, 60) + std::string(300, ' ') + "b=b9";
    CHECK(parse(lng.c_str(), m) == PadLineResult::TOO_LONG);
    // 長い注釈は構わない
    CHECK(parse((std::string(kPce) + std::string(300, 'x')).c_str(), m) == PadLineResult::OK);
}

static void testFormat() {
    const char* lines[] = {
        kPce, "abcd:00ff hatmin=1 up=h5.0 down=x15- left=x0- right=x0+ a=b31 b=h1.6 start=b0 select=b10 home=b12 if=255"};
    for (const char* l : lines) {
        PadMap a, b;
        CHECK(parse(l, a) == PadLineResult::OK);
        char buf[256];
        const size_t n = formatPadLine(a, buf, sizeof buf);
        CHECK(n > 0 && n == strlen(buf) && n <= PAD_LINE_MAX);
        CHECK(parse(buf, b) == PadLineResult::OK);
        CHECK(a.sameKey(b) && a.hatMin == b.hatMin);
        for (int i = 0; i < PadMap::ITEMS; i++) CHECK(a.src[i] == b.src[i]);
        CHECK(formatPadLine(a, buf, n + 1) == n);
        CHECK(formatPadLine(a, buf, n) == 0);
        CHECK(formatPadLine(a, buf, 1) == 0 && buf[0] == 0);
    }
}

static void testTable() {
    PadMapTable t;
    int warns = 0, line = 0;
    auto warn = [&](int l, const char* why) {
        warns++;
        line = l;
        CHECK(why[0] != 0);
    };
    const std::string text = std::string("\xef\xbb\xbf# comment\r\n") + "1234:5678" DIRS " a=b0\r\n" + "broken\n\n" +
                             "1234:5678 if=1" DIRS " a=b1\n" + "1234:5678" DIRS " a=b5\n" + kPce;
    t.load(text.data(), text.size(), warn);
    CHECK(t.count() == 3 && warns == 1 && line == 3);
    const PadMap* any = t.find(0x1234, 0x5678, 0);
    CHECK(any != nullptr && any->src[PadMap::A].index == 5); // 後の行が勝つ
    const PadMap* one = t.find(0x1234, 0x5678, 1);
    CHECK(one != nullptr && one->src[PadMap::A].index == 1); // if= の合う行が勝つ
    CHECK(t.find(0x0f0d, 0x0138, 0) != nullptr);
    CHECK(t.find(0x0f0d, 0x0138, 1) == nullptr); // if=0 の行は、ほかのインタフェースには当てない
    CHECK(t.find(0x0f0d, 0x0139, 0) == nullptr);
    t.load("", 0, warn);
    CHECK(t.count() == 0 && t.find(0x1234, 0x5678, 0) == nullptr);
    // 上限を超えた行は警告し、前のものは残す。同じ機種の置き換えは、いっぱいでもできる
    std::string many;
    for (int i = 0; i < PadMapTable::MAX + 1; i++) {
        char b[96];
        snprintf(b, sizeof b, "0001:%04x" DIRS " a=b0\n", i);
        many += b;
    }
    many += "0001:0000" DIRS " a=b7\n";
    warns = 0;
    t.load(many.data(), many.size(), warn);
    CHECK(t.count() == PadMapTable::MAX && warns == 1 && line == PadMapTable::MAX + 1);
    CHECK(t.find(1, 0, 0)->src[PadMap::A].index == 7);
    // 改行が CR だけでも読む。行の番号も合う
    const std::string cr = "# c\r1234:5678" DIRS " a=b0\rbroken\r\n1234:5679" DIRS " a=b0";
    warns = 0;
    t.load(cr.data(), cr.size(), warn);
    CHECK(t.count() == 2 && warns == 1 && line == 3);
    // 警告は MAX_WARNINGS 件と、打ち切りの 1 件まで
    std::string junk;
    for (int i = 0; i < 100; i++) junk += "junk\n";
    warns = 0;
    t.load(junk.data(), junk.size(), warn);
    CHECK(t.count() == 0 && warns == PadMapTable::MAX_WARNINGS + 1 && line == PadMapTable::MAX_WARNINGS + 1);
    PadMap m;
    CHECK(parse("1234:5678" DIRS " a=b01", m) == PadLineResult::BAD_FIELD);
    CHECK(parse("1234:5678 if=00" DIRS " a=b1", m) == PadLineResult::BAD_FIELD);
}

// 保存した並びの Select + 上と Home でメニューを開く。ハットの端の値でもあふれない
static void testMenu() {
    PadMap m;
    CHECK(parse("1234:5678 hatmin=1" DIRS " a=b2 select=b8 home=b12", m) == PadLineResult::OK);
    PadRaw raw;
    raw.nhats = 1;
    PadMenuInput menu;
    auto send = [&](uint32_t buttons, int hat) {
        raw.buttons = buttons;
        raw.hats[0] = hat;
        const GamePadInput in = applyPadMap(m, raw, 8);
        uint8_t out, events[PadMenuInput::MAX_EVENTS];
        int n;
        return menu.update(in.known, in.buttons, joystickBits(in), false, out, events, n);
    };
    CHECK(send(0, 0) == PadMenuInput::PASS);
    CHECK(send(1u << 8, 0) == PadMenuInput::PASS);
    CHECK(send(1u << 8, 1) == PadMenuInput::OPEN);
    CHECK(send(0, 0) == PadMenuInput::PASS);
    CHECK(send(1u << 12, 0) == PadMenuInput::OPEN);
    CHECK(send(0, -2147483647 - 1) == PadMenuInput::PASS);
}

static void testPriority() {
    PadMap m;
    const PadLayout* l = findPadLayout(0x0f0d, 0x0138);
    CHECK(choosePadMapping(&m, true, l) == PadMapping::SAVED);
    CHECK(choosePadMapping(&m, false, nullptr) == PadMapping::SAVED);
    CHECK(choosePadMapping(nullptr, true, l) == PadMapping::CIRCLE);
    CHECK(choosePadMapping(nullptr, false, l) == PadMapping::BUILTIN);
    CHECK(choosePadMapping(nullptr, false, nullptr) == PadMapping::UNKNOWN);
}

static uint8_t bits(const PadMap& m, const PadRaw& raw, int hat0) { return joystickBits(applyPadMap(m, raw, hat0)); }

static void testApply() {
    typedef InputSink S;
    PadMap m;
    CHECK(parse(kPce, m) == PadLineResult::OK);
    // 内蔵の表 (PCEngine PAD) と同じ動きになる
    const PadLayout* l = findPadLayout(0x0f0d, 0x0138);
    for (unsigned b = 0; b < (1u << 10); b++)
        for (int hat = 0; hat <= 8; hat++) {
            PadRaw raw;
            raw.buttons = b;
            raw.nhats = 1;
            raw.hats[0] = hat == 8 ? 15 : hat;
            GamePadInput want;
            want.known = true;
            want.buttons = remapPadButtons(*l, b);
            want.nhats = 1;
            want.hat = hat;
            CHECK(bits(m, raw, hat) == joystickBits(want));
            PadResetCombo c1, c2;
            c1.update(true, 0);
            c2.update(true, 0);
            CHECK(c1.update(true, applyPadMap(m, raw, hat).buttons) == c2.update(true, want.buttons));
        }
    // Home でメニュー
    CHECK(parse("1:1", m) == PadLineResult::BAD_ID);
    CHECK(parse("1234:5678 up=b0 down=b1 left=b2 right=b3 a=b4 home=b12", m) == PadLineResult::OK);
    PadRaw raw;
    raw.buttons = 1u << 12;
    GamePadInput in = applyPadMap(m, raw, 8);
    CHECK(in.known && in.nhats == 0 && in.naxes == 0);
    PadMenuInput menu;
    uint8_t out, ev[PadMenuInput::MAX_EVENTS];
    int n;
    CHECK(menu.update(in.known, in.buttons, joystickBits(in), false, out, ev, n) == PadMenuInput::OPEN);
    // ハットと軸は、割り当てていなければ見ない
    raw.buttons = 1u << 4;
    raw.nhats = 1;
    raw.naxes = 2;
    raw.axes[0] = {255, 0, 255};
    CHECK(bits(m, raw, 0) == S::JOY_A);
    // 4 つの方向はそれぞれのボタン
    const uint8_t dirs[4] = {S::JOY_UP, S::JOY_DOWN, S::JOY_LEFT, S::JOY_RIGHT};
    for (int i = 0; i < 4; i++) {
        raw.buttons = 1u << i;
        CHECK(bits(m, raw, 8) == dirs[i]);
    }

    // hatmin=1 の 2 番目のハット
    CHECK(parse("1234:5678 hatmin=1 up=h1.0 right=h1.2 down=h1.4 left=h1.6 a=b0", m) == PadLineResult::OK);
    PadRaw h;
    h.nhats = 2;
    const uint8_t want[10] = {0, S::JOY_UP, S::JOY_UP | S::JOY_RIGHT, S::JOY_RIGHT, S::JOY_RIGHT | S::JOY_DOWN,
                              S::JOY_DOWN, S::JOY_DOWN | S::JOY_LEFT, S::JOY_LEFT, S::JOY_LEFT | S::JOY_UP, 0};
    for (int v = 0; v < 10; v++) {
        h.hats[1] = v;
        CHECK(bits(m, h, 2) == want[v]); // 最初のハットの値は関係ない
    }
    h.hats[1] = 1;
    h.nhats = 1; // レポートにないハットは離している
    CHECK(bits(m, h, 0) == 0);
    // hatmin= がなければ、2 番目のハットは 0 から
    CHECK(parse("1234:5678 up=h1.0 right=h1.2 down=h1.4 left=h1.6 a=b0", m) == PadLineResult::OK);
    h.nhats = 2;
    h.hats[1] = 0;
    CHECK(bits(m, h, 8) == S::JOY_UP);

    // 軸: 遊びは joystickBits() と同じ。値の範囲が大きくてもあふれない
    CHECK(parse("1234:5678 up=x1- down=x1+ left=x0- right=x0+ a=x5+ b=b0", m) == PadLineResult::OK);
    PadRaw a;
    a.naxes = 6;
    a.axes[5] = {0, 0, 255}; // 離していると端 (0) のアナログトリガ
    for (int v = 0; v < 256; v++) {
        a.axes[0] = {v, 0, 255};
        a.axes[1] = {128, 0, 255};
        GamePadInput w;
        w.naxes = 2;
        w.value[0] = v;
        w.value[1] = 128;
        CHECK(bits(m, a, 8) == joystickBits(w));
    }
    a.axes[0] = {128, 0, 255};
    a.axes[5].value = 255;
    CHECK(bits(m, a, 8) == S::JOY_A);
    a.axes[5].value = 0;
    a.axes[0] = {0x7fffffff, -0x7fffffff - 1, 0x7fffffff};
    CHECK(bits(m, a, 8) == S::JOY_RIGHT);
    a.axes[0] = {5, 5, 5}; // 範囲のない軸
    CHECK(bits(m, a, 8) == 0);
    a.axes[0] = {0, 0, 255};
    a.naxes = 0;
    CHECK(bits(m, a, 8) == 0);
}

int main() {
    testParse();
    testFormat();
    testTable();
    testMenu();
    testPriority();
    testApply();
    printf(g_fail ? "pad_map_test: FAILED\n" : "pad_map_test: ok\n");
    return g_fail ? 1 : 0;
}
