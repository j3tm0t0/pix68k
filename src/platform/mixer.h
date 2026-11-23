// 音を鳴らしたまま音量を変えるミキサー (#364)。設定メニューの Sound mixer と違い、X68000 を止めない。
//
// MixerControl は、音量を読む、変える、レベルを取る、保存する、の口。画面のミキサー (このファイルの Mixer) と設定メニューが
// 使い、ネットワークの口からも使う予定。どれもコア 0 の主ループからだけ呼ぶ。
// Mixer は画面のミキサーの操作と、出す中身 (MixerView) を作る。ベアメタル版は MixerBox で写す役のコアに渡し、写す役のコアが
// 変わった行だけ renderMixerRow() で絵にして、X68000 の画面に重ねる (画面の隅の数字 (platform/osd.h) と同じ仕組み)。
//
// 操作: ↑↓ で選ぶ、←→ で 1dB 変える、Save の行で Enter を押すと vol_* だけを設定のファイルに書く (pix68k.cfg か、起動の組み合わせのファイル)、Esc (か開いたキー) で閉じる。
// 開いている間、キー、マウス、ゲームパッドは X68000 に送らない (設定メニューと同じ。platform/menu.h の MenuKeyFilter、
// platform/input_map.h の PadMenuInput)。
// ベアメタルでも使えるよう、例外、RTTI、STL、stdio は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform/menu.h"
#include "platform/osd.h"
#include "platform/sound_info.h"
#include "x68k/sound.h"

namespace platform {

struct MixerControl {
    virtual ~MixerControl() {}
    // いまの音量 (Settings::volume の値。group は Settings::volume の添字)
    virtual int mixerVolume(int group) const = 0;
    // 音量を変える (すぐに音に反映する。保存はしない)
    virtual void setMixerVolume(int group, int dB) = 0;
    // 前に取ってからのレベル。読むと 0 に戻るので、取るのは Mixer だけ
    virtual x68k::Sound::Levels takeMixerLevels() = 0;
    virtual SoundSources mixerSources() const = 0;
    // 音量だけを保存する (ほかの設定の、まだ保存していない変更は書かない)
    virtual bool saveMixerVolumes() = 0;
};

static constexpr int MIXER_STEPS = 20; // レベルの棒の段数 (1 段 2dB)
static constexpr int MIXER_GROUPS = Settings::VOLUMES;
static constexpr int MIXER_ROWS = 1 + MIXER_GROUPS + 1; // 見出し、音量の行、Save
static constexpr int MIXER_COLS = 28;                   // 1 行の桁数
static constexpr int MIXER_BAR_COL = 18;                // 棒が始まる桁
static constexpr int MIXER_MAX_SCALE = 2;
static constexpr int MIXER_IMAGE_PIXELS = MIXER_COLS * 8 * MIXER_MAX_SCALE * MIXER_ROWS * 16 * MIXER_MAX_SCALE;
static constexpr uint32_t MIXER_BAR = 0xff40c060u, MIXER_CLIP = 0xffff5040u, MIXER_TRACK = 0xff303030u,
                          MIXER_SELECTED = 0xffffe060u;

// 16 ビットの最大値 (32767) を 0dB として、-40dB から 2dB ごとの段に直す。0 は -40dB 未満、MIXER_STEPS は -2dB 以上
inline int mixerStep(uint32_t peak) {
    static const uint16_t kAt[MIXER_STEPS] = {328,  413,  519,  654,   823,   1036,  1305,  1642,  2068,  2603,
                                              3277, 4125, 5193, 6538, 8231, 10362, 13045, 16423, 20675, 26028};
    int n = 0;
    while (n < MIXER_STEPS && peak >= kAt[n]) n++;
    return n;
}

// 音源ごとのレベル (16 ビットの最大値。並びは Settings::volume と同じ)
inline void mixerPeaks(const x68k::Sound::Levels& lv, uint32_t peak[MIXER_GROUPS]) {
    using S = x68k::Sound;
    peak[S::VOL_OPM] = lv.opm;
    peak[S::VOL_ADPCM] = lv.adpcm;
    peak[S::VOL_MERCURY] = lv.mercury;
    peak[S::VOL_YMF288] = lv.opn;
    peak[S::VOL_MIDI] = lv.midiSrc;
    peak[S::VOL_CD] = lv.cd;
    peak[S::VOL_MASTER] = lv.out;
}
inline bool mixerClipped(const x68k::Sound::Levels& lv, const uint32_t peak[MIXER_GROUPS], int i) {
    return peak[i] > 32767 || (i == x68k::Sound::VOL_MASTER && lv.clipped != 0);
}
// つないでいて、音を作る音源か
inline bool mixerPresent(const SoundSources& src, int i) {
    using S = x68k::Sound;
    if (i == S::VOL_MERCURY || i == S::VOL_YMF288) return src.mercury;
    if (i == S::VOL_MIDI)
        return src.midi && src.midiOut != 1 && src.midiOut != 2 && osdMidiFaultText(src.midiOut, src.midiFault) == nullptr;
    return i != S::VOL_CD || src.cd;
}

// 画面に出す中身。コアの間を 8 バイトずつ写すので、詰め物のない 304 バイト。
// open が PANEL_TEXT なら、ミキサーの代わりに text の文字をそのまま出す (platform/pad_setup.h。場所と大きさはミキサーと同じ)
struct MixerView {
    enum Flags : uint8_t { PRESENT = 1, CLIPPED = 2 };
    enum Open : uint8_t { PANEL_CLOSED = 0, PANEL_MIXER = 1, PANEL_TEXT = 2 };
    struct Row {
        int8_t volume = 0;
        uint8_t level = 0; // 0 から MIXER_STEPS
        uint8_t flags = 0;
        bool operator==(const Row& o) const { return volume == o.volume && level == o.level && flags == o.flags; }
    };
    Row rows[MIXER_GROUPS];
    uint8_t open = 0;
    uint8_t cursor = 0; // 0 から MIXER_GROUPS (最後は Save)
    uint8_t midiFault = 0;  // MIDI の音源を用意できなければ midiOut << 4 | OsdMidiFault (#362)。用意できていれば 0
    char status[24] = {};
    char text[MIXER_ROWS][MIXER_COLS] = {};
    uint8_t reserved[4] = {};
};
static_assert(sizeof(MixerView) == 304, "MixerView");

// row 行目 (0 は見出し) の見た目が違うか
inline bool mixerRowDiffers(const MixerView& a, const MixerView& b, int row) {
    if (a.open != b.open) return true;
    if (a.open == MixerView::PANEL_TEXT) return memcmp(a.text[row], b.text[row], MIXER_COLS) != 0;
    if (row == 0) return false;
    const int i = row - 1;
    if ((a.cursor == i) != (b.cursor == i)) return true;
    if (i == x68k::Sound::VOL_MIDI && a.midiFault != b.midiFault) return true;
    if (i < MIXER_GROUPS) return !(a.rows[i] == b.rows[i]);
    return strcmp(a.status, b.status) != 0;
}

// 操作と中身。コア 0 の主ループだけが触る
class Mixer {
public:
    explicit Mixer(MixerControl& c) : c_(c) {}
    bool isOpen() const { return view_.open != 0; }
    const MixerView& view() const { return view_; }
    void open() {
        const uint8_t cursor = view_.cursor;
        view_ = MixerView();
        view_.open = MixerView::PANEL_MIXER;
        view_.cursor = cursor; // 前に閉じたときの行
        (void)c_.takeMixerLevels(); // 開く前の分は出さない
        refresh();
    }
    void close() { view_.open = 0; }
    // 押したキー。Save の行の Enter は saveRequested() で知らせる (書く間は呼ぶ側が写す役を任せるので、ここでは書かない)
    void key(uint8_t u) {
        using namespace menu_key;
        if (!isOpen()) return;
        if (u == ESC || isMenuKey(u)) {
            close();
        } else if (u == UP || u == DOWN) {
            const int n = MIXER_GROUPS + 1;
            view_.cursor = (uint8_t)((view_.cursor + (u == UP ? n - 1 : 1)) % n);
        } else if (u == LEFT || u == RIGHT) {
            if (view_.cursor >= MIXER_GROUPS) return;
            const int v = c_.mixerVolume(view_.cursor);
            const int to = stepVolume(v, u == RIGHT ? 1 : -1);
            if (to == v) return;
            c_.setMixerVolume(view_.cursor, to);
            setStatus("not saved");
            refresh();
        } else if ((u == ENTER || u == KP_ENTER) && view_.cursor == MIXER_GROUPS) {
            save_ = true;
        }
    }
    // 保存を頼まれていたら 1 回だけ true
    bool takeSave() {
        const bool s = save_;
        save_ = false;
        return s;
    }
    // ネットワークの口が音量を変えた
    void changedElsewhere() {
        if (!isOpen()) return;
        setStatus("not saved");
        refresh();
    }
    void saved(bool ok) { setStatus(ok ? "saved" : "cannot write"); }
    // 0.1 秒ごとに呼ぶ。上がるときはすぐ、下がるときは 1 回に 1 段ずつ
    void tick() {
        if (!isOpen()) return;
        const x68k::Sound::Levels lv = c_.takeMixerLevels();
        uint32_t peak[MIXER_GROUPS];
        mixerPeaks(lv, peak);
        for (int i = 0; i < MIXER_GROUPS; i++) {
            MixerView::Row& r = view_.rows[i];
            const int to = mixerStep(peak[i]);
            r.level = (uint8_t)(to >= r.level ? to : r.level - 1);
            const bool clip = mixerClipped(lv, peak, i);
            r.flags = (uint8_t)((r.flags & ~MixerView::CLIPPED) | (clip ? MixerView::CLIPPED : 0));
        }
        refresh();
    }

private:
    void refresh() {
        const SoundSources src = c_.mixerSources();
        const bool fault = src.midi && osdMidiFaultText(src.midiOut, src.midiFault) != nullptr;
        view_.midiFault = fault ? (uint8_t)(src.midiOut << 4 | src.midiFault) : 0;
        for (int i = 0; i < MIXER_GROUPS; i++) {
            MixerView::Row& r = view_.rows[i];
            r.volume = (int8_t)c_.mixerVolume(i);
            const bool present = mixerPresent(src, i);
            r.flags = (uint8_t)((r.flags & ~MixerView::PRESENT) | (present ? MixerView::PRESENT : 0));
            if (!present) r.level = 0;
        }
    }
    void setStatus(const char* s) { copyString(view_.status, sizeof view_.status, s); }
    MixerControl& c_;
    MixerView view_;
    bool save_ = false;
};

// コア 0 から写す役のコアに渡す (seqlock。書くのは 1 つのコアだけ。書いている間は seq が奇数)
struct MixerBox {
    static constexpr int WORDS = (int)(sizeof(MixerView) / 8);
    uint64_t words[WORDS] = {};
    unsigned seq = 0;

    void publish(const MixerView& v) {
        uint64_t tmp[WORDS];
        memcpy(tmp, &v, sizeof v);
        const unsigned s = __atomic_load_n(&seq, __ATOMIC_RELAXED);
        __atomic_store_n(&seq, s + 1, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        for (int i = 0; i < WORDS; i++) __atomic_store_n(&words[i], tmp[i], __ATOMIC_RELAXED);
        __atomic_store_n(&seq, s + 2, __ATOMIC_RELEASE);
    }
    unsigned version() const { return __atomic_load_n(&seq, __ATOMIC_ACQUIRE); }
    // 写せたら true。書いている途中なら false (次のフレームで読めばよい)
    bool load(MixerView& v, unsigned& version) const {
        const unsigned s = __atomic_load_n(&seq, __ATOMIC_ACQUIRE);
        if (s & 1) return false;
        uint64_t tmp[WORDS];
        for (int i = 0; i < WORDS; i++) tmp[i] = __atomic_load_n(&words[i], __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&seq, __ATOMIC_RELAXED) != s) return false;
        memcpy(&v, tmp, sizeof v);
        version = s;
        return true;
    }
};

// 左下に置く (右上の数字、右下のバー、左上のメッセージと離れる)。入らなければ倍率を下げ、それでも入らなければ w = 0
inline OsdRect placeMixer(int fbW, int fbH, int picX, int picY, int picW, int picH) {
    for (int s = fbH >= 900 ? MIXER_MAX_SCALE : 1; s >= 1; s--) {
        OsdRect r;
        r.scale = s;
        r.w = MIXER_COLS * 8 * s;
        r.h = MIXER_ROWS * 16 * s;
        r.x = OSD_PAD_X * s; // 8 の倍数なので 4 画素にそろっている
        r.y = fbH - OSD_PAD_Y * s - r.h;
        if (r.x + r.w > fbW || r.y < 0) continue;
        r.overlay = picW > 0 && picH > 0 && r.x < picX + picW && picX < r.x + r.w && r.y < picY + picH && picY < r.y + r.h;
        return r;
    }
    return OsdRect();
}

// 2 つの場所が重なるか (どちらかを出していなければ false)
inline bool mixerCovers(const OsdRect& a, const OsdRect& b) {
    return a.w > 0 && b.w > 0 && a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// row 行目の文字 (MIXER_COLS 文字 + 終端)
inline void mixerRowText(char out[MIXER_COLS + 1], const MixerView& v, int row) {
    static const char* const kNames[MIXER_GROUPS] = {"OPM", "ADPCM", "Mercury", "YMF288", "MIDI", "CD", "Master"};
    memset(out, ' ', MIXER_COLS);
    out[MIXER_COLS] = 0;
    if (v.open == MixerView::PANEL_TEXT) {
        memcpy(out, v.text[row], MIXER_COLS);
        return;
    }
    const auto put = [out](int col, const char* s) {
        for (; *s && col < MIXER_COLS; s++) out[col++] = *s;
    };
    if (row == 0) {
        put(0, "Mixer    Up/Dn Lt/Rt Esc");
        return;
    }
    const int i = row - 1;
    if (v.cursor == i) out[0] = '>';
    if (i == MIXER_GROUPS) {
        put(2, "Save");
        put(10, v.status);
        return;
    }
    put(2, kNames[i]);
    if (!(v.rows[i].flags & MixerView::PRESENT)) {
        const char* why =
            i == x68k::Sound::VOL_MIDI && v.midiFault != 0 ? osdMidiFaultText(v.midiFault >> 4, v.midiFault & 15) : nullptr;
        put(MIXER_BAR_COL, why != nullptr ? why : "-");
    }
    if (v.rows[i].volume == Settings::VOLUME_OFF) {
        put(14, "off");
        return;
    }
    char num[8];
    MenuText n(num, sizeof num);
    if (v.rows[i].volume > 0) n.add("+");
    n.add((long)v.rows[i].volume);
    put(13 - (int)strlen(num), num);
    put(14, "dB");
}

// row 行目を、絵 image (r.w x r.h 画素、1 行 r.w 画素) の中に描く
inline void renderMixerRow(uint32_t* image, const OsdRect& r, const MixerView& v, int row) {
    char text[MIXER_COLS + 1];
    mixerRowText(text, v, row);
    const int s = r.scale, i = row - 1;
    const bool mixer = v.open != MixerView::PANEL_TEXT;
    const bool bar = mixer && i >= 0 && i < MIXER_GROUPS && (v.rows[i].flags & MixerView::PRESENT);
    const uint32_t fg = mixer && row > 0 && v.cursor == i ? MIXER_SELECTED : OSD_FG;
    for (int y = 0; y < 16 * s; y++) {
        uint32_t* line = image + (size_t)(row * 16 * s + y) * r.w;
        renderOsdRow(line, text, y, s, MIXER_COLS);
        if (fg != OSD_FG)
            for (int x = 0; x < r.w; x++)
                if (line[x] == OSD_FG) line[x] = fg;
        if (!bar || y < 3 * s || y >= 13 * s) continue;
        const uint32_t on = (v.rows[i].flags & MixerView::CLIPPED) ? MIXER_CLIP : MIXER_BAR;
        for (int k = 0; k < MIXER_STEPS; k++) {
            const uint32_t c = k < v.rows[i].level ? on : MIXER_TRACK;
            uint32_t* px = line + (MIXER_BAR_COL * 8 + k * 4) * s;
            for (int x = 0; x < 3 * s; x++) px[x] = c; // 1 段は 4 画素 (右の 1 画素は間)
        }
    }
}

// 写す役のコアが持つ、いま出ているもの。update() が、消すか、描き直す行 (bit n が n 行目) を決める
struct MixerState {
    OsdRect rect;
    MixerView shownView;
    bool shown = false;
    struct Step {
        bool erase = false;
        OsdRect eraseRect;
        unsigned rows = 0;
    };
    void invalidate() { shown = false; }
    bool overlaid() const { return shown && rect.overlay; }
    Step update(bool want, const MixerView& v, const OsdRect& r) {
        Step st;
        want = want && r.w > 0;
        if (shown && (!want || r != rect)) {
            st.erase = true;
            st.eraseRect = rect;
        }
        if (!want) {
            shown = false;
            return st;
        }
        const bool all = !shown || r != rect;
        for (int i = 0; i < MIXER_ROWS; i++)
            if (all || mixerRowDiffers(v, shownView, i)) st.rows |= 1u << i;
        rect = r;
        shownView = v;
        shown = true;
        return st;
    }
};

} // namespace platform
