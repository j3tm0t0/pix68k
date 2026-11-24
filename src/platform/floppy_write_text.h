// 実物のディスクへの書き込み (Write XDF。#386、experimental) で、設定メニューに出す文 (#402)。
// どれもメニューの枠 (Menu::COLS 桁) に収める。収まることは tests/unit/floppy_write_text_test.cpp で確かめる。
// 何をするかは決めない (手順は platform/floppy_restore.h、尋ねるのは platform/menu.h)。
// ベアメタルでも使えるよう、例外、RTTI、STL、stdio は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform/floppy_restore.h"
#include "platform/menu.h"
#include "platform/text_cells.h"

namespace platform {
namespace floppy_write_text {

inline const char* ufiErrorName(UfiDisk::Error e) {
    using D = UfiDisk;
    switch (e) {
    case D::NOT_READY: return "no disk, or not ready";
    case D::BAD_FORMAT: return "not an X68000 2HD disk";
    case D::PROTECTED: return "write protected";
    case D::MEDIUM: return "bad sector";
    case D::CHANGED: return "disk changed";
    case D::REJECTED: return "command rejected";
    case D::HARDWARE: return "drive error";
    case D::NOT_ALLOWED: return "not allowed";
    case D::BROKEN: return "USB transfer failed";
    default: return "error";
    }
}

// 進み
inline void progress(MenuText& t, FloppyRestoreProgress::Phase phase, uint32_t done, uint32_t total) {
    using P = FloppyRestoreProgress;
    t.add(phase == P::READING    ? "Reading for the backup "
          : phase == P::CHECKING ? "Checking the disk "
          : phase == P::WRITING  ? "Writing (do not remove) "
                                 : "Verifying ");
    t.add((long)done).add("/").add((long)total);
    if (phase == P::READING) t.add("  Esc: cancel");
}

// 準備 (読み) に失敗した。何も書いていない
inline void prepareError(MenuText& t, FloppyRestore::Error e, UfiDisk::Error disk, uint32_t track) {
    using R = FloppyRestore;
    switch (e) {
    case R::CANCELED: t.add("not written"); break;
    case R::PROTECTED: t.add("the disk is write protected"); break;
    case R::UNREADABLE: t.add("no readable track; not written"); break;
    case R::READ_ERROR:
        t.add("cannot read C").add((long)(track / 2)).add(" H").add((long)(track % 2)).add("; not written");
        break;
    default: t.add(ufiErrorName(disk)).add("; not written"); break;
    }
}

// 書いて照合し終えた。mark は、前に終わらなかった書き込みの記録が残っているか
inline void done(MenuText& t, bool mark) { t.add(mark ? "written and verified; see also ufd-write.txt" : "written and verified"); }

// 書くのに失敗した。touched は、ディスクに書き始めたか。backup は控えの名前
inline void writeError(MenuText& t, FloppyRestore::Error e, UfiDisk::Error disk, uint32_t track, bool touched, bool partial,
                       const char* backup) {
    using R = FloppyRestore;
    if (!touched) {
        t.add(e == R::SWAPPED ? "the disk has changed" : e == R::PROTECTED ? "the disk is write protected" : ufiErrorName(disk));
        t.add("; not written");
        return;
    }
    t.add(e == R::MISMATCH ? "VERIFY" : "FAILED").add(" C").add((long)(track / 2)).add(" H").add((long)(track % 2));
    if (partial) t.add(", incomplete; see ufd-write.txt");
    else t.add(", incomplete; old: ").add(backup);
}

namespace detail {
// 1 字の桁数 (layoutLine() と同じ)
inline int cellsOf(uint32_t cp) {
    if (cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp < 0xa0)) return cp == '\t' ? 1 : 0;
    if (cp < 0x80 || cp == 0xfffd || (cp >= 0xff61 && cp <= 0xff9f)) return 1;
    return unicodeToJis(cp) != 0 || cp >= 0x1100 ? 2 : 1;
}
// s の from バイト目から cols 桁までに入る分の、終わりのバイト位置
inline size_t fit(const char* s, size_t n, size_t from, int cols) {
    size_t i = from;
    int used = 0;
    for (;;) {
        size_t next = i;
        uint32_t cp;
        if (!utf8Next(s, n, next, cp)) return i;
        used += cellsOf(cp);
        if (used > cols) return i;
        i = next;
    }
}
inline void addPart(MenuText& t, const char* s, size_t from, size_t to) {
    for (; from < to; from++) {
        const char c[2] = {s[from], 0};
        t.add(c);
    }
}
} // namespace detail

static constexpr int NAME_COLS = 40; // 「 File:  」の右の桁数

// ファイルの名前を 1 行か 2 行に分けて入れる (行の頭は「File:  」と空白)。2 行にも入らなければ、2 行目は「...」と終わりの部分
// (拡張子まで見えるように)
inline void addFileName(MenuHost::FloppyWriteInfo& info, const char* name) {
    const size_t n = strlen(name);
    const size_t first = detail::fit(name, n, 0, NAME_COLS);
    char buf[MenuHost::FloppyWriteInfo::TEXT];
    MenuText t(buf, sizeof buf);
    t.add("File:  ");
    detail::addPart(t, name, 0, first);
    info.add(buf);
    if (first == n) return;
    t.clear();
    t.add("       ");
    size_t from = first;
    if (detail::fit(name, n, first, NAME_COLS) != n) {
        t.add("...");
        while (detail::fit(name, n, from, NAME_COLS - 3) != n) { // 1 字ずつ前を落とす
            uint32_t cp;
            if (!utf8Next(name, n, from, cp)) break;
        }
    }
    detail::addPart(t, name, from, n);
    info.add(buf);
}

inline void addHex(MenuText& t, uint32_t v) {
    char s[9];
    for (int i = 0; i < 8; i++) s[i] = "0123456789abcdef"[v >> (28 - 4 * i) & 15];
    s[8] = 0;
    t.add(s);
}

// 書いてよいかを尋ねるときに見せる行
inline void confirmInfo(MenuHost::FloppyWriteInfo& info, int unit, const char* name, uint32_t fileCrc, uint32_t diskCrc,
                        const char* backup, bool sameBoot, bool protectUnknown) {
    char buf[MenuHost::FloppyWriteInfo::TEXT];
    MenuText t(buf, sizeof buf);
    t.add("Drive: usb-fdd:").add((long)unit);
    info.add(buf);
    addFileName(info, name);
    t.clear();
    t.add("       CRC-32 ");
    addHex(t, fileCrc);
    info.add(buf);
    t.clear();
    t.add("Disk:  CRC-32 ");
    addHex(t, diskCrc);
    if (fileCrc == diskCrc) t.add(" (same as the file)");
    info.add(buf);
    t.clear();
    t.add("Backup: ").add(backup);
    info.add(buf);
    info.add(sameBoot ? "Boot sector: same" : "Boot sector: will change");
    info.add(protectUnknown ? "Write protect: unknown" : "Write protect: off");
}

// 控えが欠けているときに見せる行 (読めなかったトラックの一覧)
inline void partialInfo(MenuHost::FloppyWriteInfo& info, const FloppyRestore& r) {
    info.partial = true;
    char buf[MenuHost::FloppyWriteInfo::TEXT];
    MenuText t(buf, sizeof buf);
    t.add((long)r.badBlocks()).add(" sectors in ").add((long)r.badTracks()).add(" tracks cannot be read:");
    info.addBad(buf);
    uint32_t shown = 0;
    for (uint32_t i = 0; i < FloppyRestore::TRACKS && info.badCount < MenuHost::FloppyWriteInfo::BAD_LINES - 1;) {
        t.clear();
        int n = 0;
        for (; i < FloppyRestore::TRACKS && n < 7; i++)
            if (r.badTrack(i)) {
                t.add(i / 2 < 10 ? "C0" : "C").add((long)(i / 2)).add("H").add((long)(i % 2)).add(" ");
                n++;
                shown++;
            }
        if (n > 0) info.addBad(buf);
    }
    if (shown < r.badTracks()) {
        t.clear();
        t.add("and ").add((long)(r.badTracks() - shown)).add(" more (see the .TXT of the backup)");
        info.addBad(buf);
    }
}

} // namespace floppy_write_text
} // namespace platform
