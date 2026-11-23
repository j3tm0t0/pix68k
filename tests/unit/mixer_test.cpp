// src/platform/mixer.h (音を鳴らしたまま変えるミキサー。#364) と、menu.h の MenuKeyFilter のミキサーのキーの分け方のテスト
#include <stdio.h>
#include <string.h>

#include <vector>

#include "platform/crt_effect.h"
#include "platform/mixer.h"
#include "platform/mixer_web.h"
#include "test_check.h"

using namespace platform;
using S = x68k::Sound;

namespace {

struct FakeControl : MixerControl {
    int vol[MIXER_GROUPS] = {};
    int sets = 0, takes = 0;
    S::Levels next = {};
    SoundSources src;
    int mixerVolume(int g) const override { return vol[g]; }
    void setMixerVolume(int g, int dB) override {
        vol[g] = dB;
        sets++;
    }
    S::Levels takeMixerLevels() override {
        takes++;
        const S::Levels lv = next;
        next = {};
        return lv;
    }
    SoundSources mixerSources() const override { return src; }
    bool saveMixerVolumes() override { return true; }
};

void testStep() {
    printf("stepVolume and mixerStep\n");
    CHECK_EQ(stepVolume(0, 1), 1);
    CHECK_EQ(stepVolume(12, 1), 12);
    CHECK_EQ(stepVolume(-40, -1), (int)Settings::VOLUME_OFF);
    CHECK_EQ(stepVolume(Settings::VOLUME_OFF, -1), (int)Settings::VOLUME_OFF);
    CHECK_EQ(stepVolume(Settings::VOLUME_OFF, 1), -40);
    CHECK_EQ(mixerStep(0), 0);
    CHECK_EQ(mixerStep(327), 0);
    CHECK_EQ(mixerStep(328), 1);
    CHECK_EQ(mixerStep(3277), 11); // -20dB
    CHECK_EQ(mixerStep(16423), 18);
    CHECK_EQ(mixerStep(32767), MIXER_STEPS);
    CHECK_EQ(mixerStep(100000), MIXER_STEPS);
}

void testMixer() {
    printf("Mixer\n");
    FakeControl c;
    Mixer m(c);
    m.key(menu_key::RIGHT); // 開いていなければ何もしない
    CHECK_EQ(c.sets, 0);
    c.next.opm = 30000; // 開く前の分は出さない
    m.open();
    m.tick();
    CHECK_EQ((int)m.view().rows[S::VOL_OPM].level, 0);
    m.key(menu_key::RIGHT);
    CHECK_EQ(c.vol[S::VOL_OPM], 1);
    CHECK_EQ((int)m.view().rows[S::VOL_OPM].volume, 1);
    CHECK_EQ(strcmp(m.view().status, "not saved"), 0);
    m.key(menu_key::DOWN);
    m.key(menu_key::LEFT);
    CHECK_EQ(c.vol[S::VOL_ADPCM], -1);
    m.key(menu_key::ENTER); // Save の行でなければ保存しない
    CHECK_EQ(m.takeSave(), false);
    m.key(menu_key::UP);
    m.key(menu_key::UP); // 上の端から Save へ
    CHECK_EQ((int)m.view().cursor, MIXER_GROUPS);
    const int sets = c.sets;
    m.key(menu_key::LEFT);
    CHECK_EQ(c.sets, sets);
    m.key(menu_key::ENTER);
    CHECK_EQ(m.takeSave(), true);
    m.key(menu_key::KP_ENTER);
    CHECK_EQ(m.takeSave(), true);
    CHECK_EQ(m.takeSave(), false);
    m.saved(true);
    CHECK_EQ(strcmp(m.view().status, "saved"), 0);
    // レベル: 上がるときはすぐ、下がるときは 1 段ずつ。止めたら CLIPPED
    c.next.opm = 32767;
    c.next.out = 40000;
    c.next.clipped = 1;
    m.tick();
    CHECK_EQ((int)m.view().rows[S::VOL_OPM].level, MIXER_STEPS);
    CHECK_EQ((m.view().rows[S::VOL_MASTER].flags & MixerView::CLIPPED) != 0, true);
    CHECK_EQ((m.view().rows[S::VOL_OPM].flags & MixerView::CLIPPED) == 0, true);
    m.tick();
    CHECK_EQ((int)m.view().rows[S::VOL_OPM].level, MIXER_STEPS - 1);
    CHECK_EQ((m.view().rows[S::VOL_MASTER].flags & MixerView::CLIPPED) == 0, true);
    // つないでいない音源
    c.next.cd = 32767;
    m.tick();
    CHECK_EQ((int)m.view().rows[S::VOL_CD].level, 0);
    CHECK_EQ((m.view().rows[S::VOL_CD].flags & MixerView::PRESENT) == 0, true);
    m.key(menu_key::ESC);
    CHECK_EQ(m.isOpen(), false);
    m.open();
    CHECK_EQ((int)m.view().cursor, MIXER_GROUPS); // 前に閉じたときの行
    m.key(menu_key::SCROLL_LOCK);
    CHECK_EQ(m.isOpen(), false);
}

void testKeys() {
    printf("MenuKeyFilter with the mixer\n");
    const uint8_t A = 4, SCROLL = menu_key::SCROLL_LOCK, SHIFT = 0x02;
    uint8_t down[32], out[32], ev[6], mods = 0;
    int n = 0;
    const auto set = [&](std::vector<uint8_t> keys) {
        memset(down, 0, 32);
        for (uint8_t u : keys) down[u >> 3] |= (uint8_t)(1u << (u & 7));
    };
    const auto has = [&](uint8_t u) { return (out[u >> 3] >> (u & 7)) & 1; };
    MenuKeyFilter f;
    // Shift + Scroll Lock で開く (Shift なしなら設定メニュー)
    set({A});
    CHECK_EQ((int)f.filterBits(SHIFT, down, false, mods, out, ev, n), (int)MenuKeyFilter::PASS);
    set({A, SCROLL});
    CHECK_EQ((int)f.filterBits(SHIFT, down, false, mods, out, ev, n), (int)MenuKeyFilter::OPEN_MIXER);
    CHECK_EQ(n, 0);
    // 開いている間は、どのキーもミキサーに渡す。押したままのものは渡さない
    set({A, SCROLL, menu_key::LEFT});
    CHECK_EQ((int)f.filterBits(SHIFT, down, true, mods, out, ev, n), (int)MenuKeyFilter::MENU);
    CHECK_EQ(n, 1);
    CHECK_EQ(ev[0], menu_key::LEFT);
    set({A, menu_key::ESC});
    CHECK_EQ((int)f.filterBits(SHIFT, down, true, mods, out, ev, n), (int)MenuKeyFilter::MENU);
    CHECK_EQ(n, 1);
    f.closed(SHIFT);
    // 閉じたあと、押したままのものは、離して押し直すまで送らない
    CHECK_EQ((int)f.filterBits(SHIFT, down, false, mods, out, ev, n), (int)MenuKeyFilter::PASS);
    CHECK_EQ((int)mods, 0);
    CHECK_EQ(has(A), 0);
    CHECK_EQ(has(menu_key::ESC), 0);
    set({});
    f.filterBits(0, down, false, mods, out, ev, n);
    set({A});
    f.filterBits(SHIFT, down, false, mods, out, ev, n);
    CHECK_EQ(has(A), 1);
    CHECK_EQ((int)mods, (int)SHIFT);
    set({});
    f.filterBits(0, down, false, mods, out, ev, n);
    set({SCROLL});
    CHECK_EQ((int)f.filterBits(0, down, false, mods, out, ev, n), (int)MenuKeyFilter::OPEN);
    set({});
    f.filterBits(0, down, true, mods, out, ev, n);
    // 右の Shift でも開く
    set({SCROLL});
    CHECK_EQ((int)f.filterBits(0x20, down, false, mods, out, ev, n), (int)MenuKeyFilter::OPEN_MIXER);
}

void testBoxAndState() {
    printf("MixerBox, MixerState, placeMixer\n");
    FakeControl c;
    Mixer m(c);
    m.open();
    MixerBox box;
    box.publish(m.view());
    MixerView v;
    unsigned ver = 0;
    CHECK_EQ(box.load(v, ver), true);
    CHECK_EQ(ver, box.version());
    CHECK_EQ((int)v.open, 1);
    box.seq++; // 書いている途中
    CHECK_EQ(box.load(v, ver), false);

    const OsdRect r = placeMixer(1920, 1080, 240, 0, 1440, 1080);
    CHECK_EQ(r.scale, 2);
    CHECK_EQ(r.x % 4, 0);
    CHECK_EQ(r.y + r.h <= 1080, true);
    CHECK_EQ(r.overlay, true);
    CHECK_EQ(placeMixer(640, 480, 0, 0, 640, 480).scale, 1);
    CHECK_EQ(placeMixer(200, 480, 0, 0, 200, 480).w, 0);
    CHECK_EQ(r.w * r.h <= MIXER_IMAGE_PIXELS, true);
    CHECK_EQ(mixerCovers(r, OsdRect()), false);
    CHECK_EQ(mixerCovers(r, r), true);

    MixerState st;
    MixerState::Step s = st.update(true, v, r);
    CHECK_EQ(s.rows, (1u << MIXER_ROWS) - 1);
    s = st.update(true, v, r);
    CHECK_EQ(s.rows, 0u);
    v.rows[2].level = 3;
    s = st.update(true, v, r);
    CHECK_EQ(s.rows, 1u << 3);
    v.cursor = 1;
    s = st.update(true, v, r);
    CHECK_EQ(s.rows, 1u << 1 | 1u << 2);
    OsdRect r2 = r;
    r2.x += 4;
    s = st.update(true, v, r2);
    CHECK_EQ(s.erase, true);
    CHECK_EQ(s.eraseRect == r, true);
    CHECK_EQ(s.rows, (1u << MIXER_ROWS) - 1);
    s = st.update(false, v, r2);
    CHECK_EQ(s.erase, true);
    CHECK_EQ(st.overlaid(), false);
}

void testRender() {
    printf("renderMixerRow\n");
    FakeControl c;
    c.vol[S::VOL_OPM] = -12;
    c.vol[S::VOL_ADPCM] = Settings::VOLUME_OFF;
    c.vol[S::VOL_MASTER] = 5;
    Mixer m(c);
    m.open();
    c.next.opm = 3277; // 11 段
    m.tick();
    char t[MIXER_COLS + 1];
    mixerRowText(t, m.view(), 1);
    CHECK_EQ(strncmp(t, "> OPM     -12 dB  ", 18), 0);
    mixerRowText(t, m.view(), 2);
    CHECK_EQ(strncmp(t, "  ADPCM       off ", 18), 0);
    mixerRowText(t, m.view(), 6);
    CHECK_EQ(strncmp(t, "  CD        0 dB  -", 19), 0);
    mixerRowText(t, m.view(), 7);
    CHECK_EQ(strncmp(t, "  Master   +5 dB  ", 18), 0);
    // MIDI の音源を用意できなければ、棒の代わりに理由 (#362)
    MixerView before = m.view();
    c.src.midi = true;
    c.src.midiOut = 3;
    c.src.midiFault = OSD_MIDI_NO_FILE;
    m.tick();
    mixerRowText(t, m.view(), 1 + S::VOL_MIDI);
    CHECK_EQ(strncmp(t, "  MIDI      0 dB  no ROM", 24), 0);
    CHECK_EQ(mixerRowDiffers(before, m.view(), 1 + S::VOL_MIDI), true);
    c.src.midiFault = OSD_MIDI_OK;
    m.tick();
    CHECK_EQ((m.view().rows[S::VOL_MIDI].flags & MixerView::PRESENT) != 0, true);
    c.src.midi = false;
    c.next.opm = 3277;
    m.tick();
    for (int scale = 1; scale <= 2; scale++) {
        OsdRect r;
        r.scale = scale;
        r.w = MIXER_COLS * 8 * scale;
        r.h = MIXER_ROWS * 16 * scale;
        std::vector<uint32_t> image((size_t)r.w * r.h, 0x12345678u); // 端からはみ出せば ASan で分かる
        for (int i = 0; i < MIXER_ROWS; i++) renderMixerRow(image.data(), r, m.view(), i);
        long untouched = 0, lit = 0;
        for (uint32_t px : image) {
            untouched += px == 0x12345678u;
            lit += px == MIXER_BAR;
        }
        CHECK_EQ(untouched, 0);
        CHECK_EQ(lit, (long)(11 * 3 * 10 * scale * scale));
    }
}

struct PlainStore {
    typedef frame_scale_detail::V4 V4;
    static void store4(uint32_t* p, uint32_t v) { memcpy(p, &v, 4); }
    static void store16(uint32_t* p, V4 v) { memcpy(p, &v, 16); }
    static void store64(uint32_t* p, V4 a, V4 b, V4 c, V4 d) {
        memcpy(p, &a, 16);
        memcpy(p + 4, &b, 16);
        memcpy(p + 8, &c, 16);
        memcpy(p + 12, &d, 16);
    }
};

// 4 つ目の重ねる絵が出て、ほかの画素は渡さないときと同じ
void testOverlay4() {
    printf("fourth overlay\n");
    const int W = 64, H = 32;
    std::vector<uint32_t> src((size_t)W * H);
    for (int i = 0; i < W * H; i++) src[i] = 0x101010u * (i % 13);
    std::vector<uint32_t> a((size_t)W * H), b((size_t)W * H), line(W + 4), img(16 * 4, 0xff00ff00u);
    FrameScale s;
    s.kx = s.ky = 1;
    s.srcW = s.dstW = W;
    s.srcH = s.dstH = H;
    FrameOverlay o;
    o.pixels = img.data();
    o.x = 8;
    o.y = 20;
    o.w = 16;
    o.h = 4;
    presentScaled<PlainStore>(src.data(), W, (uint8_t*)a.data(), W * 4, s, line.data());
    presentScaled<PlainStore>(src.data(), W, (uint8_t*)b.data(), W * 4, s, line.data(), nullptr, nullptr, false, nullptr,
                              nullptr, &o);
    int bad = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const bool in = x >= 8 && x < 24 && y >= 20 && y < 24;
            bad += b[y * W + x] != (in ? 0xff00ff00u : a[y * W + x]);
        }
    CHECK_EQ(bad, 0);
}

void testWeb() {
    printf("web\n");
    MixerWebRequest r;
    CHECK_EQ(parseMixerRequest("ch=midi&db=-3", r), true);
    CHECK_EQ(r.group, (int)x68k::Sound::VOL_MIDI);
    CHECK_EQ(r.dB, -3);
    CHECK_EQ(parseMixerRequest("ch=master&db=%2B12", r) && r.dB == 12 && r.group == x68k::Sound::VOL_MASTER, true);
    CHECK_EQ(parseMixerRequest("db=off&ch=opm", r) && r.dB == Settings::VOLUME_OFF && r.group == 0, true);
    for (const char* bad : {"", "ch=x&db=1", "ch=midi", "ch=midi&db=", "ch=midi&db=-", "ch=midi&db=13", "ch=midi&db=-41",
                            "ch=midi&db=1x", "ch=midi&db=-128", "ch=midi&db=0001", "db=1"})
        CHECK_EQ(parseMixerRequest(bad, r), false);

    WebLevels w;
    x68k::Sound::Levels lv = {};
    lv.opm = 100;
    w.add(lv); // 読まれていない間は溜めない
    CHECK_EQ(w.due(5000), false);
    CHECK_EQ(w.touch(10000), true);
    w.add(lv);
    CHECK_EQ(w.due(10999), false);
    CHECK_EQ(w.due(11000), true);
    w.close(11000);
    CHECK_EQ((int)w.recent().opm, 100);
    CHECK_EQ(w.touch(11001), false);
    CHECK_EQ((int)w.recent().opm, 100); // 2 つ目のページが読んでも同じ
    w.close(12000);
    CHECK_EQ((int)w.recent().opm, 0);
    CHECK_EQ(w.due(16001), false); // 5 秒読まれなければやめる
    CHECK_EQ(w.touch(20000), true);
    CHECK_EQ(w.touch(0xfffffff0u), false);
    CHECK_EQ(w.due(8), true); // ミリ秒が一周しても続ける

    FakeControl c;
    char json[MIXER_JSON_SIZE];
    c.vol[0] = Settings::VOLUME_OFF;
    c.vol[4] = -40;
    lv.opm = 32768;
    formatMixerJson(json, sizeof json, c, lv, true);
    CHECK_EQ(strstr(json, "{\"unsaved\":true,\"steps\":20,\"ch\":[{\"name\":\"opm\",\"db\":null,\"level\":20,\"clip\":true,"
                          "\"present\":true}") == json, true);
    CHECK_EQ(strstr(json, "\"name\":\"midi\",\"db\":-40,") != nullptr, true);
    CHECK_EQ(strcmp(json + strlen(json) - 4, "}]}\n"), 0); // 切れていない
    CHECK_EQ(strlen(json) + 100 < MIXER_JSON_SIZE, true);

    WebSave s;
    CHECK_EQ(s.wanted() || s.done(), false);
    s.request();
    CHECK_EQ(s.wanted(), true);
    s.finish(true);
    CHECK_EQ(s.done() && s.ok(), true);
    s.refuse();
    CHECK_EQ(s.done() && s.busy() && !s.ok(), true);
    s.cancel();
    CHECK_EQ(s.done() || s.busy(), false);

    static HttpRequest q;
    const auto origin = [&](const char* o, const char* token) {
        char head[512];
        snprintf(head, sizeof head, "POST /mixer HTTP/1.1\r\nHost: pix68k\r\n%s\r\n", o);
        return parseHttpRequest(head, q) && mixerOriginAllowed(q, token);
    };
    CHECK_EQ(origin("Origin: http://pix68k\r\n", "t"), true);
    CHECK_EQ(origin("Origin: http://PIX68K\r\n", "t"), true);
    CHECK_EQ(origin("Origin: http://pix68k\r\n", ""), false); // net_token= がない
    for (const char* bad : {"", "Origin: null\r\n", "Origin: http://evil\r\n", "Origin: https://pix68k\r\n",
                            "Origin: http://pix68k.evil\r\n", "Origin: http://pix68k:81\r\n", "Origin: http://\r\n",
                            "Origin: http://pix68k\r\nOrigin: http://pix68k\r\n"})
        CHECK_EQ(origin(bad, "t"), false);
}

} // namespace

int main() {
    testWeb();
    testStep();
    testMixer();
    testKeys();
    testBoxAndState();
    testRender();
    testOverlay4();
    return finish();
}
