// platform/pad_setup.h (ゲームパッドのボタンの並びを画面で決める) の単体テスト
#include <stdio.h>
#include <string.h>

#include <string>

#include "platform/mixer.h"
#include "platform/pad_setup.h"

static int g_fail = 0;
#define CHECK(c)                                                  \
    do {                                                          \
        if (!(c)) {                                               \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); \
            g_fail++;                                             \
        }                                                         \
    } while (0)

using namespace platform;
namespace mk = platform::menu_key;
using PS = PadSetup;

struct Rig {
    PadSetup s;
    PS::Pad pads[PS::PADS];
    uint32_t now;
    explicit Rig(uint32_t start = 1000) : now(start) {
        PS::Pad& p = pads[0];
        p.id = 5;
        p.vid = 0x0f0d;
        p.pid = 0x0138;
        p.iface = 0;
        p.valid = true;
        p.raw.nhats = 1;
        p.raw.hats[0] = 8;
        p.raw.naxes = 3;
        for (int i = 0; i < 3; i++) p.raw.axes[i] = {128, 0, 255};
        p.raw.axes[2].value = 0; // アナログのトリガ (離すと端)
        s.open(now);
        s.update(pads, now);
    }
    PadRaw& raw() { return pads[0].raw; }
    void run(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 10) {
            now += 10;
            s.update(pads, now);
        }
    }
    void button(int b, uint32_t ms = 100) {
        raw().buttons = 1u << b;
        run(ms);
        raw().buttons = 0;
        run(50);
    }
    void hat(int v) {
        raw().hats[0] = v;
        run(100);
        raw().hats[0] = 8;
        run(50);
    }
    void axis(int i, int v) {
        const int rest = raw().axes[i].value;
        raw().axes[i].value = v;
        run(100);
        raw().axes[i].value = rest;
        run(50);
    }
    void begin() {
        button(0, 1200);
        run(400);
        CHECK(s.state() == PS::ITEM && s.item() == PadMap::UP);
        run(20);
    }
    void dirs() {
        for (int v : {0, 4, 6, 2}) hat(v);
    }
    void toConfirm() {
        begin();
        dirs();
        for (int b : {2, 1, 9, 8, 12}) button(b);
        CHECK(s.state() == PS::CONFIRM);
    }
};

static void testFull() {
    for (uint32_t start : {1000u, 0xfffff000u}) { // 時刻が 1 周しても同じ
        Rig r(start);
        r.toConfirm();
        const PadMap& m = r.s.map();
        CHECK(m.vid == 0x0f0d && m.pid == 0x0138 && m.iface == 0 && m.hatMin == 0);
        CHECK(m.src[PadMap::UP].kind == PadSource::HAT && m.src[PadMap::UP].dir == 0);
        CHECK(m.src[PadMap::RIGHT].dir == 2);
        CHECK(m.src[PadMap::A].kind == PadSource::BUTTON && m.src[PadMap::A].index == 2);
        CHECK(m.src[PadMap::HOME].index == 12);
        r.button(2, 500);
        CHECK(!r.s.takeSave());
        r.button(2, 1200);
        CHECK(r.s.state() == PS::SAVING);
        CHECK(r.s.takeSave());
        CHECK(!r.s.takeSave());
        r.s.saved("cannot write pads.txt", r.now); // 書けなければ確認に戻る
        CHECK(r.s.state() == PS::CONFIRM);
        r.run(50);
        r.s.key(mk::ENTER, r.now);
        CHECK(r.s.takeSave());
        r.s.saved(nullptr, r.now);
        CHECK(r.s.state() == PS::DONE);
        r.run(1600);
        CHECK(!r.s.isOpen());
        char line[PAD_LINE_MAX + 1];
        const size_t n = formatPadLine(m, line, sizeof line);
        PadMap back;
        CHECK(n > 0 && parsePadLine(line, n, back) == PadLineResult::OK);
        CHECK(back.src[PadMap::LEFT] == m.src[PadMap::LEFT]);
    }
}

static void testSelect() {
    Rig r;
    r.button(0, 800); // 短い
    CHECK(r.s.state() == PS::WAIT);
    r.raw().buttons = 3; // 2 つ
    r.run(1500);
    CHECK(r.s.state() == PS::WAIT);
    r.raw().buttons = 1;
    r.run(1200);
    CHECK(r.s.state() == PS::SETTLE);
    r.run(1000); // 離すまで進まない
    CHECK(r.s.state() == PS::SETTLE);
    Rig two;
    two.pads[1] = two.pads[0];
    two.pads[1].id = 9;
    two.s.update(two.pads, two.now);
    two.pads[1].raw.buttons = 4;
    two.run(1200);
    CHECK(two.s.slot() == 1);
}

static void testHeldAtOpen() {
    Rig r;
    r.raw().buttons = 4;
    r.s.open(r.now); // メニューで押したまま
    r.run(1500);
    CHECK(r.s.state() == PS::WAIT);
    r.raw().buttons = 0;
    r.run(50);
    r.button(2, 1200);
    CHECK(r.s.state() != PS::WAIT);
}
static void testSkipBackDuplicate() {
    Rig r;
    r.begin();
    r.button(3, 2200); // 必須なので飛ばせない
    CHECK(r.s.item() == PadMap::UP);
    r.s.key(mk::ENTER, r.now);
    CHECK(r.s.item() == PadMap::UP);
    r.hat(0);
    CHECK(r.s.item() == PadMap::DOWN);
    r.hat(0); // 上と同じ
    CHECK(r.s.item() == PadMap::DOWN);
    r.hat(1); // 斜め
    CHECK(r.s.item() == PadMap::DOWN);
    r.s.key(mk::ESC, r.now);
    CHECK(r.s.item() == PadMap::UP && r.s.map().src[PadMap::UP].kind == PadSource::NONE && r.s.map().hatMin == -1);
    r.run(20);
    r.dirs();
    r.button(2);
    CHECK(r.s.item() == PadMap::B);
    r.button(2);
    CHECK(r.s.item() == PadMap::B);
    r.button(7, 2200); // 長押しで飛ばす
    CHECK(r.s.item() == PadMap::START && r.s.map().src[PadMap::B].kind == PadSource::NONE);
    r.s.key(mk::KP_ENTER, r.now);
    CHECK(r.s.item() == PadMap::SELECT);
    r.run(20);
    r.raw().buttons = 1u << 8; // 押したまま Enter で飛ばしても、次の項目には割り当てない
    r.run(50);
    r.s.key(mk::ENTER, r.now);
    r.run(50);
    r.raw().buttons = 0;
    r.run(50);
    CHECK(r.s.item() == PadMap::HOME && r.s.map().src[PadMap::HOME].kind == PadSource::NONE);
    r.s.key(mk::ENTER, r.now);
    CHECK(r.s.state() == PS::CONFIRM);
    r.s.key(mk::ESC, r.now);
    CHECK(r.s.state() == PS::ITEM && r.s.item() == PadMap::HOME);
    Rig c;
    c.toConfirm();
    c.button(1, 1200); // B の長押しで最初から
    c.run(400);
    CHECK(c.s.state() == PS::ITEM && c.s.item() == PadMap::UP);
    c.s.key(mk::ESC, c.now);
    CHECK(!c.s.isOpen());
}

static void testAxes() {
    Rig r;
    r.begin();
    r.axis(1, 60); // 60% に届かない
    CHECK(r.s.item() == PadMap::UP);
    r.axis(1, 0);
    r.axis(1, 255);
    r.axis(0, 0);
    r.axis(0, 255);
    CHECK(r.s.item() == PadMap::A);
    CHECK(r.s.map().src[PadMap::UP].kind == PadSource::AXIS && r.s.map().src[PadMap::UP].index == 1 &&
          r.s.map().src[PadMap::UP].dir == 0 && r.s.map().src[PadMap::RIGHT].dir == 1);
    r.axis(2, 255); // 端で止まるトリガ
    CHECK(r.s.map().src[PadMap::A].kind == PadSource::AXIS && r.s.map().src[PadMap::A].index == 2 &&
          r.s.map().src[PadMap::A].dir == 1);
    // 軸が先に動き、あとからボタンも出る機種はボタンにする
    r.raw().axes[2].value = 200;
    r.run(30);
    r.raw().axes[2].value = 255;
    r.raw().buttons = 1u << 6;
    r.run(50);
    r.raw().axes[2].value = 0;
    r.raw().buttons = 0;
    r.run(50);
    CHECK(r.s.map().src[PadMap::B].kind == PadSource::BUTTON && r.s.map().src[PadMap::B].index == 6);
    // 1-8 のハット (離すと 0)
    Rig h;
    h.raw().hats[0] = 0;
    h.begin();
    for (int v : {1, 5, 7, 3}) {
        h.raw().hats[0] = v;
        h.run(100);
        h.raw().hats[0] = 0;
        h.run(50);
    }
    CHECK(h.s.item() == PadMap::A && h.s.map().hatMin == 1 && h.s.map().src[PadMap::DOWN].dir == 4);
}

static void testCloses() {
    Rig r;
    r.run(9900);
    CHECK(r.s.isOpen());
    r.run(200);
    CHECK(!r.s.isOpen());
    Rig u;
    u.begin();
    u.pads[0].id = 6; // つなぎ直した
    u.run(10);
    CHECK(!u.s.isOpen());
    Rig t;
    t.toConfirm();
    t.run(10100);
    CHECK(!t.s.isOpen() && !t.s.takeSave());
    Rig k;
    k.begin();
    k.s.key(mk::SCROLL_LOCK, k.now);
    CHECK(!k.s.isOpen());
}

static void testFlapping() {
    Rig r;
    r.begin();
    r.hat(0);
    for (int i = 0; i < 1000 && r.s.isOpen(); i++) r.hat(0); // 「already used」を繰り返す
    CHECK(!r.s.isOpen() && r.now < 45000);
    Rig c;
    c.toConfirm();
    for (int i = 0; i < 1000 && c.s.isOpen(); i++) c.button(2);
    CHECK(!c.s.isOpen());
}

struct OfferRig {
    PadSetupOffer o;
    PS::Pad pads[PS::PADS];
    bool unknown[PS::PADS] = {true, true};
    bool blocked = false;
    uint32_t now;
    int started = -1;
    explicit OfferRig(uint32_t t = 1000) : now(t) {
        pads[0].id = 5;
        pads[0].valid = true;
    }
    void run(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 10) {
            now += 10;
            const int s = o.update(pads, unknown, blocked, now);
            if (s >= 0) {
                started = s;
                o.started(s);
            }
        }
    }
    void hold(uint32_t buttons, uint32_t ms) {
        pads[0].raw.buttons = buttons;
        run(ms);
        pads[0].raw.buttons = 0;
        run(20);
    }
};
static void testOffer() {
    for (uint32_t t : {1000u, 0xfffff000u}) {
        OfferRig a(t);
        a.run(100);
        CHECK(a.o.offering());
        a.hold(1, 2900);
        CHECK(a.started == -1);
        a.hold(3, 4000); // 2 つ
        CHECK(a.started == -1);
        a.hold(1, 3100);
        CHECK(a.started == 0 && !a.o.offering() && !a.o.wants(0, 5));
        a.started = -1;
        a.hold(1, 3100); // 1 回つなぐごとに 1 回
        CHECK(a.started == -1);
        a.pads[0].id = 7; // つなぎ直した
        a.run(20);
        a.hold(1, 3100);
        CHECK(a.started == 0);
    }
    OfferRig d; // 方向を入れながら
    d.run(20);
    d.pads[0].direction = true;
    d.hold(1, 4000);
    CHECK(d.started == -1);
    d.pads[0].direction = false;
    d.pads[0].raw.buttons = 1; // 途中で方向を入れて戻しても、離すまで数えない
    d.run(500);
    d.pads[0].direction = true;
    d.run(20);
    d.pads[0].direction = false;
    d.run(3500);
    CHECK(d.started == -1);
    d.hold(0, 20);
    d.hold(1, 3100);
    CHECK(d.started == 0);
    for (uint32_t other : {3u, 2u}) { // ボタンを足して戻す、替える
        OfferRig e;
        e.run(20);
        e.pads[0].raw.buttons = 1;
        e.run(2000);
        e.pads[0].raw.buttons = other;
        e.run(20);
        e.pads[0].raw.buttons = other == 3 ? 1 : other;
        e.run(4000);
        CHECK(e.started == -1);
    }
    OfferRig v; // 方向でない軸 (右スティックなど) が動き続ける
    v.pads[0].raw.naxes = 4;
    for (int i = 0; i < 4; i++) v.pads[0].raw.axes[i] = {128, 0, 255};
    v.run(20);
    v.pads[0].raw.buttons = 1;
    for (int i = 0; i < 40; i++) {
        v.pads[0].raw.axes[3].value = i & 1 ? 0 : 255;
        v.run(100);
    }
    CHECK(v.started == -1);
    v.run(3100); // 止まれば、止まったところから 3 秒
    CHECK(v.started == 0);
    OfferRig n; // 始められなかったら、受け付けを使い切らない
    n.run(20);
    n.pads[0].raw.buttons = 1;
    int got = -1;
    for (int i = 0; i < 400 && got < 0; i++) got = n.o.update(n.pads, n.unknown, false, n.now += 10);
    CHECK(got == 0 && n.o.wants(0, 5));
    n.blocked = true;
    n.run(50);
    n.blocked = false;
    n.hold(0, 20);
    n.hold(1, 3100);
    CHECK(n.started == 0);
    OfferRig shut; // ふさがっている間に 15 秒が過ぎる
    shut.blocked = true;
    shut.run(15100);
    shut.blocked = false;
    shut.hold(1, 4000);
    CHECK(shut.started == -1);
    OfferRig late; // 15 秒を過ぎたら受けない
    late.run(15100);
    CHECK(!late.o.offering() && !late.o.wants(0, 5));
    late.hold(1, 4000);
    CHECK(late.started == -1);
    OfferRig edge; // 15 秒の中で押し始めれば、過ぎてもよい
    edge.run(14000);
    edge.hold(1, 3100);
    CHECK(edge.started == 0);
    for (bool hat : {false, true}) { // 数え直すときに 15 秒を過ぎていれば、つなぎ直すまで受けない
        OfferRig w;
        w.pads[0].raw.nhats = 2;
        w.pads[0].raw.hats[0] = w.pads[0].raw.hats[1] = 8;
        w.pads[0].raw.naxes = 4;
        for (int i = 0; i < 4; i++) w.pads[0].raw.axes[i] = {128, 0, 255};
        w.run(13000);
        w.pads[0].raw.buttons = 1;
        w.run(2500);
        CHECK(w.started == -1 && w.o.wants(0, 5));
        if (hat) w.pads[0].raw.hats[1] = 0;
        else w.pads[0].raw.axes[3].value = 255;
        w.run(4000);
        CHECK(w.started == -1 && !w.o.wants(0, 5) && !w.o.offering());
        w.pads[0].id = 7;
        w.pads[0].raw.buttons = 0;
        w.run(20);
        w.hold(1, 3100);
        CHECK(w.started == 0);
        OfferRig in; // 15 秒の中なら数え直す
        in.pads[0].raw = w.pads[0].raw;
        in.run(10000);
        in.pads[0].raw.buttons = 1;
        in.run(2500);
        if (hat) in.pads[0].raw.hats[1] = 4;
        else in.pads[0].raw.axes[3].value = 0;
        in.run(2900);
        CHECK(in.started == -1);
        in.run(200);
        CHECK(in.started == 0);
    }
    for (int a = 0; a < 2; a++) { // 最初の 2 本の軸が端で止まる機種は、方向を入れたままと見なす (カーネルと同じ joystickBits())
        OfferRig p;
        p.pads[0].raw.naxes = 2;
        GamePadInput gi;
        gi.naxes = 2;
        for (int i = 0; i < 2; i++) {
            gi.minimum[i] = 0;
            gi.maximum[i] = 255;
            gi.value[i] = i == a ? 255 : 128;
            p.pads[0].raw.axes[i] = {gi.value[i], 0, 255};
        }
        p.pads[0].direction = (joystickBits(gi) & (InputSink::JOY_UP | InputSink::JOY_DOWN | InputSink::JOY_LEFT |
                                                   InputSink::JOY_RIGHT)) != 0;
        CHECK(p.pads[0].direction);
        p.run(20);
        CHECK(p.o.offering());
        p.hold(1, 5000);
        CHECK(p.started == -1);
    }
    OfferRig held; // 動かす前から押していた
    held.pads[0].raw.buttons = 1;
    held.run(4000);
    CHECK(held.started == -1);
    held.hold(0, 20);
    held.hold(1, 3100);
    CHECK(held.started == 0);
    OfferRig first; // 変わったときだけ送る機種: 最初のレポートが押したところ
    first.pads[0].valid = false;
    first.run(100);
    first.pads[0].valid = true;
    first.hold(1, 3100);
    CHECK(first.started == 0);
    OfferRig menu; // メニューで押したままのボタンは数えない
    menu.run(20);
    menu.blocked = true;
    menu.pads[0].raw.buttons = 1;
    menu.run(1000);
    CHECK(!menu.o.offering());
    menu.blocked = false;
    menu.run(4000);
    CHECK(menu.started == -1);
    OfferRig known;
    known.unknown[0] = false;
    known.hold(1, 4000);
    CHECK(known.started == -1 && !known.o.offering());
    OfferRig two;
    two.pads[1].id = 9;
    two.pads[1].valid = true;
    two.run(20);
    two.pads[1].raw.buttons = 4;
    two.run(3100);
    CHECK(two.started == 1);
    // 押したままハットを動かし続けても、10 秒で閉じる
    Rig h;
    h.raw().buttons = 4;
    h.s.openFor(0, h.pads, h.now);
    for (int i = 0; i < 110; i++) {
        h.raw().hats[0] = i & 7;
        h.run(100);
    }
    CHECK(!h.s.isOpen());
    // 選んだところから始まる
    Rig r;
    r.raw().buttons = 4;
    r.s.close();
    r.s.openFor(0, r.pads, r.now);
    CHECK(r.s.state() == PS::SETTLE && r.s.slot() == 0);
    r.run(1000);
    r.raw().buttons = 0;
    r.run(400);
    CHECK(r.s.state() == PS::ITEM && r.s.map().vid == 0x0f0d);
    r.pads[0].id = 0;
    r.s.openFor(0, r.pads, r.now);
    CHECK(r.s.state() == PS::WAIT);
}

static std::string row(const Rig& r, int i) {
    char t[PS::ROWS][PS::COLS];
    r.s.text(t, r.pads);
    return std::string(t[i], PS::COLS);
}
static void testText() {
    Rig r;
    CHECK(row(r, 1).find("Hold any button") == 0);
    CHECK(row(r, 5).find("joy1 0f0d:0138") == 0);
    r.begin();
    CHECK(row(r, 1).find("Press: Up") == 0 && row(r, 1).find("(1/9)") == 22);
    CHECK(row(r, 2).find("required") == 0);
    r.raw().buttons = 0xffffffffu;
    for (int i = 0; i < 3; i++) r.raw().axes[i].value = -2147483647;
    CHECK(row(r, 6).find("buttons ffffffff") == 0);
    {
        Rig x;
        x.raw().naxes = 8;
        for (int i = 0; i < 8; i++) x.raw().axes[i] = {-32768, -32768, 32767};
        CHECK(row(x, 7) == "x 0:-32768 1:-32768 2:-32768");
        CHECK(row(x, 8) == "x 3:-32768 4:-32768 5:-32768");
        for (int i = 0; i < 8; i++) x.raw().axes[i] = {128, 0, 255};
        CHECK(row(x, 7).find("x 0:128 1:128 2:128 3:128 ") == 0 && row(x, 8).find("x 4:128 5:128 6:128 7:128 ") == 0);
    }
    // ミキサーの絵の仕組みにそのまま渡せる
    MixerView a, b;
    a.open = b.open = MixerView::PANEL_TEXT;
    r.s.text(a.text, r.pads);
    memcpy(b.text, a.text, sizeof a.text);
    CHECK(!mixerRowDiffers(a, b, 6));
    b.text[6][3] = '!';
    CHECK(mixerRowDiffers(a, b, 6) && !mixerRowDiffers(a, b, 5));
    Rig c;
    c.toConfirm();
    c.raw().buttons = 1u << 2;
    c.run(10);
    CHECK(row(c, 1).find("Try: A") == 0);
    CHECK(row(c, 4).find_first_not_of(' ') == std::string::npos);
    Rig w;
    w.begin();
    w.dirs();
    w.button(2);
    for (int i = 0; i < 4; i++) w.s.key(mk::ENTER, w.now);
    CHECK(w.s.state() == PS::CONFIRM && row(w, 4).find("No Home/Select: no pad menu") == 0);
}

static std::string upsert(const std::string& in, const PadMap& m) {
    std::string out(in.size() + PAD_LINE_MAX + 5, 0);
    out.resize(upsertPadLine(in.data(), in.size(), m, &out[0], out.size()));
    return out;
}
static void testUpsert() {
    Rig r;
    r.toConfirm();
    const PadMap& m = r.s.map();
    char line[PAD_LINE_MAX + 1];
    const std::string mine(line, formatPadLine(m, line, sizeof line));
    CHECK(upsert("", m) == mine + "\n");
    const std::string other = "1234:5678 up=b0 down=b1 left=b2 right=b3 a=b4\n";
    const std::string same = "0f0d:0138 if=0 up=b0 down=b1 left=b2 right=b3 a=b4 # x\r\n";
    const std::string any = "0f0d:0138 up=b0 down=b1 left=b2 right=b3 a=b4\n"; // if= なしは別の行
    const std::string bad = "0f0d:0138 if=0 up=zz\n";
    CHECK(upsert("# c\r\n" + same + other + any + same + bad + "# end", m) ==
          "# c\r\n" + other + any + bad + "# end\r\n" + mine + "\r\n");
    CHECK(upsert("\xef\xbb\xbf" + same, m) == "\xef\xbb\xbf" + mine + "\r\n");
    CHECK(upsert("# c\r" + same.substr(0, same.size() - 1), m) == "# c\r" + mine + "\r"); // CR だけの改行
    const std::string once = upsert(other, m);
    CHECK(upsert(once, m) == once);
    char small[10];
    CHECK(upsertPadLine("", 0, m, small, sizeof small) == 0);
}

int main() {
    testFull();
    testSelect();
    testHeldAtOpen();
    testSkipBackDuplicate();
    testAxes();
    testCloses();
    testFlapping();
    testOffer();
    testText();
    testUpsert();
    if (g_fail == 0) printf("pad_setup_test: ok\n");
    return g_fail != 0;
}
