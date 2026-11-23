// ベアメタル版 (Pi 4 / Pi 400 / Pi 5) で、画面を出す HDMI の口の選び方 (#329)。
//
// Pi 4 と Pi 400 には micro HDMI の口が 2 つあり、config.txt に max_framebuffers=2 があると、ファームウェアは画面を 2 つ
// 数える。Circle の画面 (CBcmFrameBuffer) は、その何番目かを番号 (0, 1) で選ぶが、番号と口の対応はファームウェアが
// 決める。Pi 400J (c03130) では、番号 0 が HDMI0、番号 1 が HDMI1 で、モニタは HDMI1 にあった。Circle は番号 0 に
// 出すので、画面がレインボーのままだった。
// そこで番号では決めず、ファームウェアに番号ごとの口の ID (HDMI0 は 2、HDMI1 は 7) と、その口のモニタの EDID が
// 読めるかを聞いて選ぶ。モニタのない口も数えられるので、EDID が読めた口をモニタがつながっている口とみなす
// (モニタのない口では、タグの応答は成功のまま、状態が 0 でなくなり、EDID の頭の 8 バイトも入らない)。
//
// 選び方 (pickDisplay()):
//   - cmdline.txt の pix68k_display=hdmi0 / hdmi1 なら、その口 (その口が数えられていなければ auto と同じ)
//   - auto (既定) なら、EDID が読めた口のうち HDMI0 を先に、なければ HDMI1、なければ数えられた順で最初に EDID が
//     読めた口。どれも EDID が読めなければ、いままでどおり番号 0
// 画面が 1 つ以下 (Pi 2 / Pi 3、max_framebuffers=1) なら番号 0 のまま。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

namespace platform {

// ファームウェアの口の ID (mailbox の Get display ID、0x00040016 が返す値)
enum : unsigned { DISPLAY_ID_HDMI0 = 2, DISPLAY_ID_HDMI1 = 7 };
enum : unsigned { MAX_DISPLAYS = 4 }; // 調べる画面の数の上限 (Pi 4 は DSI と HDMI 2 つ、Pi 5 は HDMI 2 つ)

// cmdline.txt の pix68k_display の値
enum class DisplayOption { AUTO, HDMI0, HDMI1, INVALID };

struct DisplayInfo {
    bool idKnown = false; // 口の ID を聞けた
    unsigned id = 0;      // 口の ID (idKnown のとき)
    bool edid = false;    // その口のモニタの EDID が読めた
};

inline bool displayNameIs(const char* s, const char* name) {
    while (*s != 0 && *s == *name) {
        s++;
        name++;
    }
    return *s == 0 && *name == 0;
}

// nullptr (書いていない) は AUTO
inline DisplayOption parseDisplayOption(const char* s) {
    if (s == nullptr || displayNameIs(s, "auto")) return DisplayOption::AUTO;
    if (displayNameIs(s, "hdmi0")) return DisplayOption::HDMI0;
    if (displayNameIs(s, "hdmi1")) return DisplayOption::HDMI1;
    return DisplayOption::INVALID;
}

inline const char* displayIdName(bool idKnown, unsigned id) {
    if (!idKnown) return "unknown";
    if (id == DISPLAY_ID_HDMI0) return "HDMI0";
    if (id == DISPLAY_ID_HDMI1) return "HDMI1";
    return "other";
}

// 選んだ理由 (ログに出す)
enum class DisplayPick { SINGLE, OPTION, MONITOR_HDMI0, MONITOR_HDMI1, MONITOR_OTHER, NO_MONITOR };

inline const char* displayPickName(DisplayPick p) {
    switch (p) {
    case DisplayPick::SINGLE: return "only one display";
    case DisplayPick::OPTION: return "pix68k_display";
    case DisplayPick::MONITOR_HDMI0: return "monitor on HDMI0";
    case DisplayPick::MONITOR_HDMI1: return "monitor on HDMI1";
    case DisplayPick::MONITOR_OTHER: return "monitor found";
    case DisplayPick::NO_MONITOR: return "no monitor found; using display 0";
    }
    return "?";
}

// 数えられた n 個の画面 (info[0..n-1]) から、Circle に渡す番号を選ぶ
inline unsigned pickDisplay(const DisplayInfo* info, unsigned n, DisplayOption opt, DisplayPick& why) {
    if (n > MAX_DISPLAYS) n = MAX_DISPLAYS;
    if (n <= 1) {
        why = DisplayPick::SINGLE;
        return 0;
    }
    auto find = [&](unsigned id, bool needEdid) -> int {
        for (unsigned i = 0; i < n; i++)
            if (info[i].idKnown && info[i].id == id && (!needEdid || info[i].edid)) return (int)i;
        return -1;
    };
    if (opt == DisplayOption::HDMI0 || opt == DisplayOption::HDMI1) {
        const int i = find(opt == DisplayOption::HDMI0 ? DISPLAY_ID_HDMI0 : DISPLAY_ID_HDMI1, false);
        if (i >= 0) {
            why = DisplayPick::OPTION;
            return (unsigned)i;
        }
    }
    int i = find(DISPLAY_ID_HDMI0, true);
    if (i >= 0) {
        why = DisplayPick::MONITOR_HDMI0;
        return (unsigned)i;
    }
    i = find(DISPLAY_ID_HDMI1, true);
    if (i >= 0) {
        why = DisplayPick::MONITOR_HDMI1;
        return (unsigned)i;
    }
    for (unsigned k = 0; k < n; k++)
        if (info[k].edid) {
            why = DisplayPick::MONITOR_OTHER;
            return k;
        }
    why = DisplayPick::NO_MONITOR;
    return 0;
}

// TS-6BGA (2 つめの画面) に使う番号: 選んだ番号でない画面のうち、口の ID が HDMI0 / HDMI1 のもの。
// なければ選んだ番号でない最初の画面。画面が 1 つ以下なら -1
inline int otherDisplay(const DisplayInfo* info, unsigned n, unsigned primary) {
    if (n > MAX_DISPLAYS) n = MAX_DISPLAYS;
    for (unsigned i = 0; i < n; i++)
        if (i != primary && info[i].idKnown && (info[i].id == DISPLAY_ID_HDMI0 || info[i].id == DISPLAY_ID_HDMI1))
            return (int)i;
    for (unsigned i = 0; i < n; i++)
        if (i != primary) return (int)i;
    return -1;
}

} // namespace platform
