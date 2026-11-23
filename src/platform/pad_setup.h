// ゲームパッドのボタンの並びを、画面を見ながら順に押して決める (#379)。X68000 は止めない (ミキサーと同じ)。
//
// 流れ: 設定するパッドのボタンを 1 秒長押し → 全部離して落ち着くのを待つ → 上、下、左、右、A、B、Start、Select、Home を
// 順に押す → 確認 (割り当てたとおりに動くかを見る) → 保存。
//   - 短く押して離すと割り当てる。2 秒以上押して離すと、その項目を飛ばす (上下左右と A は飛ばせない)
//   - キーボード: Enter は飛ばす / 確認の画面で保存、Esc は 1 つ戻る (最初の項目では終わる)、メニューのキーで終わる
//   - 確認の画面では、A の 1 秒長押しで保存、B の 1 秒長押しで最初から
//   - 10 秒なにも変わらなければ終わる (並びは変えない)。壊れたボタンなどで入力がばたついても、同じ段階に 30 秒 (確認の画面は 60 秒)
//     いれば終わる。最初のボタンを離して落ち着くのを待つのは 10 秒まで
// 並びの分からないゲームパッドは、つないでから 15 秒の間、ボタンの 3 秒の長押しだけでも始められる (PadSetupOffer)
// PadSetup は生の値と時刻だけで動き、保存と反映は takeSave() を見て呼ぶ側が行う。コア 0 の主ループだけが触る。
// 例外、RTTI、STL、stdio は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform/menu.h"
#include "platform/pad_map.h"

namespace platform {

class PadSetup {
public:
    static constexpr int PADS = 2, ROWS = 9, COLS = 28;
    static constexpr uint32_t HOLD_MS = 1000, SKIP_MS = 2000, SETTLE_MS = 300, IDLE_MS = 10000, DONE_MS = 1500, STEP_MS = 30000, CONFIRM_MS = 60000;
    enum State { CLOSED, WAIT, SETTLE, ITEM, CONFIRM, SAVING, DONE };

    // ゲームパッド 1 台。id はつないだ回ごとに変わる値 (0 ならつないでいない)
    struct Pad {
        uint32_t id = 0;
        uint16_t vid = 0, pid = 0;
        int iface = -1;
        bool valid = false; // raw を 1 度でも受け取った
        bool direction = false; // X68000 に送る上下左右のどれかを押している (PadSetupOffer が見る)
        PadRaw raw;
    };

    bool isOpen() const { return state_ != CLOSED; }
    State state() const { return state_; }
    int item() const { return item_; }
    int slot() const { return slot_; }
    const PadMap& map() const { return map_; }

    void open(uint32_t now) {
        now_ = step_ = now;
        state_ = WAIT;
        for (int i = 0; i < PADS; i++) {
            holding_[i] = false;
            released_[i] = false;
        }
        save_ = false;
        slot_ = -1;
        padId_ = 0;
        msg_ = "";
        idle_ = now;
    }
    // 枠 slot のゲームパッドを選んだところから始める (PadSetupOffer で長押しした)
    void openFor(int slot, const Pad pads[PADS], uint32_t now) {
        open(now);
        if (slot >= 0 && slot < PADS && pads[slot].id != 0) select(slot, pads[slot], now);
    }
    void close() {
        state_ = CLOSED;
        save_ = false;
    }

    void key(uint8_t u, uint32_t now) {
        using namespace menu_key;
        if (!isOpen()) return;
        idle_ = now_ = step_ = now;
        if (isMenuKey(u)) {
            close();
            return;
        }
        const bool enter = u == ENTER || u == KP_ENTER;
        if (u == ESC) {
            if (state_ == ITEM && item_ > 0) {
                beginItem(item_ - 1);
            } else if (state_ == CONFIRM) {
                beginItem(PadMap::ITEMS - 1);
            } else if (state_ != SAVING && state_ != DONE) {
                close();
            }
        } else if (enter && state_ == ITEM) {
            skip();
        } else if (enter && state_ == CONFIRM) {
            requestSave();
        }
    }

    // 主ループから毎回呼ぶ
    void update(const Pad pads[PADS], uint32_t now) {
        if (!isOpen()) return;
        now_ = now;
        if (state_ == WAIT) {
            updateWait(pads, now);
        } else if (state_ == DONE) {
            if (now - since_ >= DONE_MS) close();
            return;
        } else if (slot_ < 0 || pads[slot_].id != padId_) {
            close(); // 設定している間に外した
            return;
        } else if (state_ == SETTLE) {
            updateSettle(pads[slot_].raw, now);
        } else if (state_ == ITEM) {
            updateItem(pads[slot_].raw, now);
        } else if (state_ == CONFIRM) {
            updateConfirm(pads[slot_].raw, now);
        }
        if (isOpen() && state_ != SAVING && (now - idle_ >= IDLE_MS || now - step_ >= (state_ == CONFIRM ? CONFIRM_MS : STEP_MS))) close();
    }

    // 保存を頼まれていたら 1 回だけ true (map() を保存して、saved() で結果を知らせる)
    bool takeSave() {
        const bool s = save_;
        save_ = false;
        return s;
    }
    // error は書けなかった理由 (1 行に入る英語の文字列定数)。書けたら nullptr
    void saved(const char* error, uint32_t now) {
        if (state_ != SAVING) return;
        idle_ = now;
        if (error == nullptr) {
            state_ = DONE;
            since_ = now;
        } else {
            enterConfirm(now);
            msg_ = error;
        }
    }

    // 画面に出す 9 行 (1 行 28 桁。空白で埋める)
    void text(char out[ROWS][COLS], const Pad pads[PADS]) const {
        memset(out, ' ', (size_t)ROWS * COLS);
        put(out[0], 0, "Gamepad setup   Esc / Menu");
        switch (state_) {
        case WAIT:
            put(out[1], 0, "Hold any button of the");
            put(out[2], 0, "gamepad for 1 second");
            if (pads[0].id == 0 && pads[1].id == 0) put(out[3], 0, "(no gamepad connected)");
            break;
        case SETTLE:
            put(out[1], 0, "Release all buttons");
            break;
        case ITEM: {
            int c = put(out[1], 0, "Press: ");
            c = put(out[1], c, itemLabel(item_));
            char num[16];
            MenuText n(num, sizeof num);
            n.add("(").add((long)item_ + 1).add("/").add((long)PadMap::ITEMS).add(")");
            put(out[1], 22, n.str());
            put(out[2], 0, item_ < PadMap::REQUIRED ? "required" : "hold 2 s or Enter: skip");
            break;
        }
        case CONFIRM:
        case SAVING: {
            put(out[1], 0, "Try:");
            int c = 5;
            if (slot_ >= 0) {
                const PadRaw& raw = pads[slot_].raw;
                static const char* const kShort[PadMap::ITEMS] = {"Up", "Dn", "Lt", "Rt", "A", "B", "St", "Se", "Ho"};
                for (int i = 0; i < PadMap::ITEMS; i++)
                    if (active(i, raw)) c = put(out[1], c, kShort[i]) + 1;
            }
            put(out[2], 0, "A 1 s / Enter: save");
            put(out[3], 0, map_.src[PadMap::B].kind != PadSource::NONE ? "B 1 s: start over  Esc: back" : "Esc: back");
            break;
        }
        case DONE:
            put(out[1], 0, "Saved");
            break;
        default:
            break;
        }
        if (state_ == ITEM || state_ == CONFIRM) put(out[4], 0, msg_);
        // 保存した並びが優先なので、Home も Select もないと、このゲームパッドで設定メニューを開けない
        if (state_ == CONFIRM && msg_[0] == 0 && map_.src[PadMap::HOME].kind == PadSource::NONE &&
            map_.src[PadMap::SELECT].kind == PadSource::NONE)
            put(out[4], 0, "No Home/Select: no pad menu");
        // 生の値 (テスターを兼ねる)。選ぶ前は、値の来ているパッドのうち最初のもの
        int s = slot_;
        for (int i = 0; s < 0 && i < PADS; i++)
            if (pads[i].id != 0 && pads[i].valid) s = i;
        if (s < 0 || !pads[s].valid) return;
        const PadRaw& r = pads[s].raw;
        char buf[COLS * 2];
        {
            MenuText t(buf, sizeof buf);
            t.add("joy").add((long)s + 1).add(" ");
            hex(t, pads[s].vid, 4);
            t.add(":");
            hex(t, pads[s].pid, 4);
            put(out[5], 0, t.str());
        }
        {
            MenuText t(buf, sizeof buf);
            t.add("buttons ");
            hex(t, r.buttons, 8);
            if (r.nhats > 0) t.add(" hat");
            for (int i = 0; i < r.nhats && i < 2; i++) t.add(" ").add((long)r.hats[i]);
            put(out[6], 0, t.str());
        }
        // 軸の「番号:値」。入りきらない値は途中で切らず、そこからあとを出さない
        for (int i = 0, row = 0, c = r.naxes > 0 ? put(out[7], 0, "x") : 0; i < r.naxes && i < PadRaw::AXIS_COUNT; i++) {
            MenuText t(buf, sizeof buf);
            t.add(" ").add((long)i).add(":").add((long)r.axes[i].value);
            const int len = (int)strlen(t.str());
            if (c + len > COLS) {
                if (row++ == 1) break;
                c = put(out[8], 0, "x");
                if (c + len > COLS) break;
            }
            c = put(out[7 + row], c, t.str());
        }
    }

    static const char* itemLabel(int i) {
        static const char* const kLabels[PadMap::ITEMS] = {"Up", "Down", "Left", "Right", "A",
                                                           "B", "Start", "Select", "Home"};
        return kLabels[i];
    }

private:
    static constexpr int MAX_ACTIVE = 8;

    static int put(char* row, int col, const char* s) {
        for (; *s && col < COLS; s++) row[col++] = *s;
        return col;
    }
    static void hex(MenuText& t, uint32_t v, int digits) {
        char b[9];
        for (int i = 0; i < digits; i++) b[i] = "0123456789abcdef"[(v >> (4 * (digits - 1 - i))) & 15];
        b[digits] = 0;
        t.add(b);
    }

    void updateWait(const Pad pads[PADS], uint32_t now) {
        for (int i = 0; i < PADS; i++) {
            const uint32_t b = pads[i].valid ? pads[i].raw.buttons : 0;
            // メニューで選ぶのに押したままのボタンを数えないよう、一度すべて離してから
            if (b == 0) released_[i] = true;
            // ちょうど 1 つのボタンだけを押し続けている
            if (pads[i].id == 0 || b == 0 || (b & (b - 1)) != 0 || !released_[i]) {
                holding_[i] = false;
                continue;
            }
            if (!holding_[i] || holdId_[i] != pads[i].id || holdButton_[i] != b) {
                holding_[i] = true;
                holdId_[i] = pads[i].id;
                holdButton_[i] = b;
                holdSince_[i] = now;
                idle_ = now;
            } else if (now - holdSince_[i] >= HOLD_MS) {
                select(i, pads[i], now);
                return;
            }
        }
    }

    void select(int slot, const Pad& pad, uint32_t now) {
        slot_ = slot;
        padId_ = pad.id;
        map_ = PadMap();
        map_.vid = pad.vid;
        map_.pid = pad.pid;
        map_.iface = pad.iface;
        beginSettle(pad.raw, now);
    }

    void beginSettle(const PadRaw& raw, uint32_t now) {
        state_ = SETTLE;
        rest_ = raw;
        since_ = idle_ = step_ = now;
        msg_ = "";
    }
    // 全部のボタンを離し、ハットと軸が動かなくなったら、その値を「離している値」とする
    void updateSettle(const PadRaw& raw, uint32_t now) {
        if (raw.buttons != 0 || moved(rest_, raw)) {
            rest_ = raw;
            since_ = now;
            return;
        }
        if (now - since_ < SETTLE_MS) return;
        map_.hatMin = -1;
        for (int i = 0; i < PadMap::ITEMS; i++) map_.src[i] = PadSource();
        idle_ = now;
        beginItem(0);
    }
public:
    static bool moved(const PadRaw& a, const PadRaw& b) {
        if (a.nhats != b.nhats || a.naxes != b.naxes) return true;
        for (int i = 0; i < b.nhats && i < PadRaw::HAT_COUNT; i++)
            if (a.hats[i] != b.hats[i]) return true;
        for (int i = 0; i < b.naxes && i < PadRaw::AXIS_COUNT; i++) {
            const int64_t d = (int64_t)a.axes[i].value - b.axes[i].value;
            const int64_t range = (int64_t)b.axes[i].maximum - b.axes[i].minimum;
            if ((d < 0 ? -d : d) > range / 16) return true;
        }
        return false;
    }

private:
    void beginItem(int i) {
        state_ = ITEM;
        item_ = i;
        step_ = now_;
        for (int k = i; k < PadMap::ITEMS; k++) map_.src[k] = PadSource();
        refreshHatMin();
        pressing_ = false;
        needRelease_ = true;
        msg_ = "";
    }
    // ハットを使う項目がなくなったら、hatmin= も消す
    void refreshHatMin() {
        map_.hatMin = -1;
        for (int k = 0; k < PadMap::ITEMS; k++)
            if (map_.src[k].kind == PadSource::HAT) map_.hatMin = hatMinOf(map_.src[k].index);
    }
    // ハットの値が上で始まる数。離している値が 0 なら 1-8 の機種、それ以外なら 0-7 の機種
    int hatMinOf(int index) const { return rest_.hats[index] == 0 ? 1 : 0; }

    void nextItem() {
        if (item_ + 1 < PadMap::ITEMS) {
            beginItem(item_ + 1);
        } else {
            msg_ = "";
            enterConfirm(idle_);
        }
    }
    void skip() {
        if (item_ < PadMap::REQUIRED) {
            msg_ = "this one is required";
            return;
        }
        map_.src[item_] = PadSource();
        nextItem();
    }

    // 離しているときと違う入力を集める。ボタン、ハット、軸の順 (アナログのトリガは、ボタンと軸の両方に出る機種がある)
    int activeSources(const PadRaw& raw, PadSource out[MAX_ACTIVE]) const {
        int n = 0;
        auto add = [&](PadSource::Kind kind, int index, int dir) {
            if (n >= MAX_ACTIVE) return;
            out[n].kind = kind;
            out[n].index = (uint8_t)index;
            out[n].dir = (uint8_t)dir;
            n++;
        };
        for (int i = 0; i < 32; i++)
            if (raw.buttons >> i & 1) add(PadSource::BUTTON, i, 0);
        for (int i = 0; i < raw.nhats && i < PadRaw::HAT_COUNT && i < rest_.nhats; i++) {
            if (raw.hats[i] == rest_.hats[i]) continue;
            const int64_t v = (int64_t)raw.hats[i] - hatMinOf(i);
            if (v >= 0 && v <= 7 && (v & 1) == 0) add(PadSource::HAT, i, (int)v);
        }
        for (int i = 0; i < raw.naxes && i < PadRaw::AXIS_COUNT && i < rest_.naxes; i++) {
            const int64_t lo = raw.axes[i].minimum, hi = raw.axes[i].maximum;
            if (hi <= lo) continue;
            const int64_t center = lo + (hi - lo) / 2, half = (hi - lo) / 2;
            const int64_t v = raw.axes[i].value - center, r = rest_.axes[i].value - center;
            // 真ん中で止まる軸は、半分の幅の 60%。端で止まる軸 (アナログのトリガ) は、反対の側に 60% 入ったとき
            const int64_t th = half * 6 / 10;
            const bool restCenter = (r < 0 ? -r : r) <= half / 4;
            if (v > th && (restCenter || r < 0)) add(PadSource::AXIS, i, 1);
            else if (v < -th && (restCenter || r > 0)) add(PadSource::AXIS, i, 0);
        }
        return n;
    }
    // 斜めのハットも、押していることには数える
    bool anyActive(const PadRaw& raw) const {
        PadSource s[MAX_ACTIVE];
        if (activeSources(raw, s) > 0) return true;
        for (int i = 0; i < raw.nhats && i < PadRaw::HAT_COUNT && i < rest_.nhats; i++)
            if (raw.hats[i] != rest_.hats[i]) return true;
        return false;
    }

    void updateItem(const PadRaw& raw, uint32_t now) {
        PadSource s[MAX_ACTIVE];
        const int n = activeSources(raw, s);
        const bool any = anyActive(raw);
        if (needRelease_) {
            if (!any) needRelease_ = false;
            return;
        }
        if (!pressing_) {
            if (n == 0) return;
            pressing_ = true;
            candidate_ = s[0];
            since_ = idle_ = now;
            msg_ = "";
            return;
        }
        // 押している間に、同じ操作でボタンも出てきたら、ボタンのほうを使う (軸が先に動くアナログのトリガ)
        if (n > 0 && s[0].kind < candidate_.kind) candidate_ = s[0];
        if (any) return;
        pressing_ = false;
        idle_ = now;
        if (now - since_ >= SKIP_MS) {
            skip();
            return;
        }
        for (int i = 0; i < item_; i++) {
            if (!(map_.src[i] == candidate_)) continue;
            msg_ = "already used";
            return;
        }
        if (candidate_.kind == PadSource::HAT) {
            // hatmin= は 1 行に 1 つなので、値の範囲の違うハットは一緒に使えない
            if (map_.hatMin >= 0 && map_.hatMin != hatMinOf(candidate_.index)) {
                msg_ = "cannot use this hat";
                return;
            }
        }
        map_.src[item_] = candidate_;
        nextItem();
        // beginItem() は離すのを待つ状態にするが、もう離している
        needRelease_ = false;
        refreshHatMin();
    }

    bool active(int i, const PadRaw& raw) const { return padSourceActive(map_.src[i], map_.hatMin, raw, 8); }

    void enterConfirm(uint32_t now) {
        state_ = CONFIRM;
        step_ = now;
        needRelease_ = true;
        pressing_ = false;
        idle_ = now;
        lastMask_ = 0;
    }
    void updateConfirm(const PadRaw& raw, uint32_t now) {
        unsigned mask = 0;
        for (int i = 0; i < PadMap::ITEMS; i++)
            if (active(i, raw)) mask |= 1u << i;
        if (mask != lastMask_) {
            lastMask_ = mask;
            idle_ = since_ = now;
        }
        if (needRelease_) {
            if (mask == 0) needRelease_ = false;
            return;
        }
        // A か B だけを 1 秒押し続けた
        if (now - since_ < HOLD_MS) return;
        if (mask == 1u << PadMap::A) {
            requestSave();
        } else if (mask == 1u << PadMap::B) {
            beginSettle(raw, now);
        }
    }
    void requestSave() {
        state_ = SAVING;
        save_ = true;
        msg_ = "";
    }

    State state_ = CLOSED;
    int item_ = 0, slot_ = -1;
    uint32_t padId_ = 0;
    uint32_t now_ = 0, step_ = 0; // いまの時刻、今の段階に入ったか最後にキーを押した時刻
    uint32_t idle_ = 0, since_ = 0; // 最後に変わった時刻、今の段階や押し始めの時刻
    unsigned lastMask_ = 0;
    bool holding_[PADS] = {}, released_[PADS] = {};
    uint32_t holdId_[PADS] = {}, holdButton_[PADS] = {}, holdSince_[PADS] = {};
    bool pressing_ = false, needRelease_ = false, save_ = false;
    PadSource candidate_;
    PadRaw rest_;
    PadMap map_;
    const char* msg_ = "";
};

// 並びの分からないゲームパッドの設定を、パッドだけで始める。つないだとき (X68000 を動かす前なら動かしたとき) から 15 秒の
// 間に押し始めたボタン 1 つを、3 秒押し続けたら始める。ゲーム中の押しっぱなしと区別できないので、過ぎたら受けない (つなぎ
// 直せばまた受ける)。1 回つなぐごとに 1 回だけ。
// 遊んでいる途中の操作で始まらないよう、上下左右を押したり、ボタンを足したり替えたりしたら、ボタンをすべて離すまで数えない。ほかの軸や
// ハットが動いたら、そこから数え直す (押し込みの強さが軸に出る機種があるので、離すのは待たない)。コア 0 の主ループだけが触る
class PadSetupOffer {
public:
    static constexpr uint32_t WINDOW_MS = 15000, HOLD_MS = 3000;

    // 枠 slot の、つないだ回が id のゲームパッドをまだ見る必要があるか
    bool wants(int slot, uint32_t id) const { return id != 0 && (s_[slot].id != id || !s_[slot].done); }
    // 主ループから呼ぶ。unknown[i] は並びの分からないゲームパッドか。blocked は、設定メニューなどを開いているか X68000 が止まっている。
    // 始めるなら枠の番号 (始めたら started() を呼ぶ)、始めないなら -1
    int update(const PadSetup::Pad pads[PadSetup::PADS], const bool unknown[PadSetup::PADS], bool blocked, uint32_t now) {
        offering_ = false;
        int start = -1;
        for (int i = 0; i < PadSetup::PADS; i++) {
            Slot& s = s_[i];
            const uint32_t b = pads[i].valid ? pads[i].raw.buttons : 0;
            if (pads[i].id != s.id) {
                s = Slot();
                s.id = pads[i].id;
                s.since = now;
                s.needRelease = b != 0; // X68000 を動かす前から押していた
            }
            if (s.id == 0 || s.done) continue;
            if (b == 0) s.needRelease = false;
            if (!unknown[i]) {
                s.holding = false;
            } else if (blocked || s.needRelease || b == 0 || (b & (b - 1)) != 0 || pads[i].direction ||
                       (s.holding && s.button != b)) {
                if (b != 0) s.needRelease = true;
                s.holding = false;
            } else if (!s.holding || PadSetup::moved(s.raw, pads[i].raw)) {
                s.holding = now - s.since < WINDOW_MS;
                s.button = b;
                s.raw = pads[i].raw;
                s.hold = now;
            } else if (now - s.hold >= HOLD_MS && start < 0) {
                start = i;
            }
            if (!s.holding && now - s.since >= WINDOW_MS) s.done = true;
            else if (unknown[i] && !blocked) offering_ = true;
        }
        return start;
    }
    void started(int slot) { s_[slot].done = true; }
    bool offering() const { return offering_; } // 受け付けている (画面に案内を出す)

private:
    struct Slot {
        uint32_t id = 0, since = 0, hold = 0, button = 0;
        bool done = false, holding = false, needRelease = false;
        PadRaw raw; // 数え始めたときの値
    };
    Slot s_[PadSetup::PADS];
    bool offering_ = false;
};

// pads.txt の中身 text に m の行を入れる: 同じ VID:PID と if= の使える行を (行末の注釈も) 消し、最後に m の行を足す (ほかの行はそのまま)。
// 改行は text の最初の改行に合わせる。out の大きさは size + PAD_LINE_MAX + 5 あれば足りる。書いた長さを返す。入りきらなければ 0
inline size_t upsertPadLine(const char* text, size_t size, const PadMap& m, char* out, size_t cap) {
    size_t n = 0, pos = 0;
    auto add = [&](const char* p, size_t len) {
        if (len > cap - n) return false;
        if (len) memcpy(out + n, p, len);
        n += len;
        return true;
    };
    if (size >= 3 && memcmp(text, "\xef\xbb\xbf", 3) == 0) {
        if (!add(text, 3)) return 0;
        pos = 3;
    }
    const size_t head = n;
    const char* eol = "\n";
    for (size_t i = pos; i < size; i++) {
        if (text[i] == '\n') break;
        if (text[i] != '\r') continue;
        eol = i + 1 < size && text[i + 1] == '\n' ? "\r\n" : "\r";
        break;
    }
    const size_t eolLen = strlen(eol);
    while (pos < size) {
        size_t next;
        const size_t end = pad_map_detail::lineEnd(text, size, pos, next);
        PadMap old;
        if (parsePadLine(text + pos, end - pos, old) != PadLineResult::OK || !old.sameKey(m))
            if (!add(text + pos, next - pos)) return 0;
        pos = next;
    }
    char line[PAD_LINE_MAX + 1];
    const size_t len = formatPadLine(m, line, sizeof line);
    if (len == 0) return 0;
    if (n > head && out[n - 1] != '\n' && out[n - 1] != '\r' && !add(eol, eolLen)) return 0;
    return add(line, len) && add(eol, eolLen) ? n : 0;
}

} // namespace platform
