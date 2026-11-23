// 設定メニューの音のミキサー (#300) と、画面の隅の音源の表示 (#310) に出す、音の様子を作る。
// ベアメタル版 (baremetal/kernel.h) とホスト版 (host/main.cpp) が使う。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "platform/menu.h"
#include "platform/osd.h"
#include "platform/settings.h"
#include "x68k/sound.h"

namespace platform {

// いまの設定で使える音源
struct SoundSources {
    bool mercury = false; // まーきゅりーゆにっと (PCM と YMF288)
    bool cd = false;      // CD-ROM 装置
    bool midi = false;    // MIDI ボード
    int midiOut = 0;      // Settings::midiOut (0 sf2、1 uart、2 none、3 mt32、4 sc55)
};

inline SoundSources soundSources(const Settings& s) {
    SoundSources src;
    src.mercury = s.mercury != 0;
    src.cd = s.cd[0] != 0;
    src.midi = s.midi != 0;
    src.midiOut = s.midiOut;
    return src;
}

// MIDI の行き先の短い名前 (画面に出す。画面の隅の音源の行と同じ)
inline const char* midiOutShortName(int midiOut) { return osdMidiOutName(midiOut); }

// 使える音源を 1 行にする: 「OPM ADPCM MERC(PCM+OPN3) MIDI:MT-32 CD」。out は 48 バイト以上
inline void soundSourcesLine(const SoundSources& src, char* out, size_t size) {
    MenuText t(out, size);
    t.add("OPM ADPCM");
    if (src.mercury) t.add(" MERC(PCM+OPN3)");
    if (src.midi) t.add(" MIDI:").add(midiOutShortName(src.midiOut));
    if (src.cd) t.add(" CD");
}

// 16 ビットいっぱいを 100 とした % (切り上げない)。16 ビットを超えていれば、切り捨てで 100 になっても 101 にする
// (画面は 100 を超えると「!」を付ける。32768〜33094 も振り切れているので)
inline int soundPercent(uint32_t peak) {
    const int p = (int)((uint64_t)peak * 100 / 32767);
    return peak > 32767 && p <= 100 ? 101 : p;
}

// 直近の 10 秒ほどの音の大きさ (ミキサーの画面に出す)。1 秒ごとの区間を SECONDS 個持ち、古い区間から捨てる。
// 呼ぶ側は、Sound::takeLevels() で取った値をすべて add() に渡し、X68000 が動いている間だけ、1 秒ごとに tick() を呼ぶ
// (メニューを開いて止めている間は進めないので、開き直しても、ページを行き来しても、開く前の 10 秒の値のまま)。
// recent() は、いまの区間 (前の tick() からの分) と、その前の SECONDS - 1 区間を合わせた値 (9〜10 秒分)
class SoundWindow {
public:
    static constexpr int SECONDS = 10;
    void add(const x68k::Sound::Levels& lv) { x68k::Sound::mergeLevels(bucket_[pos_], lv); }
    void tick() {
        if (unreadCount_ < SECONDS) unreadCount_++; // 画面の隅がまだ見ていない閉じた区間の数
        pos_ = (pos_ + 1) % SECONDS;
        bucket_[pos_] = {};
    }
    // 画面の隅の音源の明るさ (#310) に使う値: 前に取ってから tick() で閉じた区間を合わせたもの。画面の隅の 1 秒と tick() の
    // 1 秒は別の時計なので、2 つ閉じていれば 2 つとも入れ (取りこぼさない)、1 つも閉じていなければ前の区間 (lastSecond()) を返す。
    // 合わせるのは新しい UNREAD_MAX 区間まで (画面の隅を消していて長く見なかったあと、出し直した最初の 1 秒に、ずっと前に鳴った
    // 音源を白にしない)
    static constexpr int UNREAD_MAX = 2;
    x68k::Sound::Levels takeUnread() {
        if (unreadCount_ == 0) return lastSecond();
        const int n = unreadCount_ < UNREAD_MAX ? unreadCount_ : UNREAD_MAX;
        x68k::Sound::Levels lv = {};
        for (int k = 1; k <= n; k++) x68k::Sound::mergeLevels(lv, bucket_[(pos_ + SECONDS - k) % SECONDS]);
        unreadCount_ = 0;
        return lv;
    }
    // 前の tick() で閉じた 1 秒の区間 (画面の隅の音源の明るさに使う。#310)
    const x68k::Sound::Levels& lastSecond() const { return bucket_[(pos_ + SECONDS - 1) % SECONDS]; }
    x68k::Sound::Levels recent() const {
        x68k::Sound::Levels sum = {};
        for (const x68k::Sound::Levels& b : bucket_) x68k::Sound::mergeLevels(sum, b);
        return sum;
    }

private:
    x68k::Sound::Levels bucket_[SECONDS] = {};
    int pos_ = 0, unreadCount_ = 0;
};

// SoundWindow::tick() を呼ぶ時を決める。X68000 が動いていた時間 (elapsed。止めている間は 0 を渡す) を足し、1 秒 (second)
// たまるごとに true を返す。止める前後の端数はつながる。2 秒以上たまっていたら (長く回らなかったとき) 1 回だけ進めて捨てる
struct SoundRunTimer {
    uint64_t run = 0;
    bool add(uint64_t elapsed, uint64_t second) {
        run += elapsed;
        if (run < second) return false;
        run = run >= 2 * second ? 0 : run - second;
        return true;
    }
};

// ベアメタル版のコア 0 が、回るたびに今の時刻 (now) とメニューを開いているか (paused) を渡す。X68000 が動いていた時間を
// SoundRunTimer に足し、1 秒たまったら true。前に見たときに開いていたら、この回の時間も数えない (閉じた回の時間には、
// 閉じる前のステートの読み込みなど、X68000 を止めていた時間が入る)
struct SoundRunClock {
    SoundRunTimer timer;
    uint64_t last = 0;
    bool pausedBefore = false;
    explicit SoundRunClock(uint64_t start) : last(start) {}
    bool update(uint64_t now, bool paused, uint64_t second) {
        const bool tick = timer.add(paused || pausedBefore ? 0 : now - last, second);
        pausedBefore = paused;
        last = now;
        return tick;
    }
};

// ホスト版の Machine::runFrame() の前後のサイクル数 (totalCycles()) から、動いたサイクル数を求める。ステートはフレームの
// 外で読むので入らない。戻っていたら (runFrame() の中で戻すことはないが、念のため) 0
inline uint64_t soundCyclesElapsed(uint64_t before, uint64_t after) { return after >= before ? after - before : 0; }

// 画面の隅の音源の行 (#310): つないでいる音源 (OsdSound のビット) と、そのうち lv (直近 1 秒ほど) で鳴ったもの。
// 鳴ったとみなすのは、その音源の振れ幅 (Levels::swing。音量を掛けたあと) が SOUND_ACTIVE_SWING (16 ビットの幅の約 0.1%、
// 振幅で -60dB ほど) を超えたとき。最大の絶対値で見ると、止まったあとも最後の値を出し続ける ADPCM が鳴っていることになる
static constexpr uint32_t SOUND_ACTIVE_SWING = 64;
inline void osdSound(OsdStats& st, const SoundSources& src, const x68k::Sound::Levels& lv) {
    unsigned sources = OSD_SND_OPM | OSD_SND_ADPCM, on = 0;
    if (src.mercury) sources |= OSD_SND_PCM | OSD_SND_OPN;
    if (src.midi) sources |= OSD_SND_MIDI;
    if (src.cd) sources |= OSD_SND_CD;
    using S = x68k::Sound;
    if (lv.swing[S::SWING_OPM] > SOUND_ACTIVE_SWING) on |= OSD_SND_OPM;
    if (lv.swing[S::SWING_ADPCM] > SOUND_ACTIVE_SWING) on |= OSD_SND_ADPCM;
    if (lv.swing[S::SWING_PCM] > SOUND_ACTIVE_SWING) on |= OSD_SND_PCM;
    if (lv.swing[S::SWING_OPN] > SOUND_ACTIVE_SWING) on |= OSD_SND_OPN;
    if (lv.swing[S::SWING_MIDI] > SOUND_ACTIVE_SWING) on |= OSD_SND_MIDI;
    if (lv.swing[S::SWING_CD] > SOUND_ACTIVE_SWING) on |= OSD_SND_CD;
    st.soundSources = sources;
    st.soundActive = on & sources;
    st.midiOut = src.midiOut;
}

// ミキサーの画面に出す値。lv は、メニューを開く前の 10 秒ほどの Sound::takeLevels() をまとめたもの (SoundWindow::recent())
inline void fillSoundInfo(MenuHost::SoundInfo& info, const x68k::Sound::Levels& lv, const SoundSources& src) {
    info.peak[x68k::Sound::VOL_OPM] = soundPercent(lv.opm);
    info.peak[x68k::Sound::VOL_ADPCM] = soundPercent(lv.adpcm);
    info.peak[x68k::Sound::VOL_MERCURY] = src.mercury ? soundPercent(lv.mercury) : -1;
    info.peak[x68k::Sound::VOL_YMF288] = src.mercury ? soundPercent(lv.opn) : -1;
    info.peak[x68k::Sound::VOL_MIDI] = src.midi && src.midiOut != 1 && src.midiOut != 2 ? soundPercent(lv.midiSrc) : -1;
    info.peak[x68k::Sound::VOL_CD] = src.cd ? soundPercent(lv.cd) : -1;
    info.peak[x68k::Sound::VOL_MASTER] = soundPercent(lv.out);
    info.clipped = lv.clipped;
    soundSourcesLine(src, info.sources, sizeof info.sources);
}

} // namespace platform
