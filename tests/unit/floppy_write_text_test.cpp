// platform/floppy_write_text.h (実物のディスクへの書き込みで、設定メニューに出す文。#402) のテスト: どの文もメニューの枠の幅に収まること
#include "platform/floppy_write_text.h"
#include "fake_ufi.h"
#include "test_check.h"

#include <stdio.h>
#include <string.h>

namespace {

namespace fwt = platform::floppy_write_text;

int cells(const char* s) {
    static uint16_t buf[512];
    return layoutLine(s, buf, 500);
}
void checkFits(const char* s) {
    if (cells(s) > Menu::COLS) printf("  %d cells: %s\n", cells(s), s);
    CHECK_EQ(cells(s) <= Menu::COLS, true);
    CHECK_EQ(strlen(s) < (size_t)Menu::LINE_BYTES, true);
}

struct Text {
    char buf[Menu::LINE_BYTES * 2];
    MenuText t{buf, sizeof buf};
};

const char* const kBackup = "FD-6930FE0F.XDF";

void testStatus() {
    using P = FloppyRestoreProgress;
    for (int ph = P::READING; ph <= P::VERIFYING; ph++) {
        Text x;
        fwt::progress(x.t, (P::Phase)ph, 154, 154);
        checkFits(x.buf);
        CHECK_EQ(strstr(x.buf, "154/154") != nullptr, true);
        CHECK_EQ(strstr(x.buf, "Esc") != nullptr, ph == P::READING);
        CHECK_EQ(strstr(x.buf, "do not remove") != nullptr, ph == P::WRITING);
    }
    for (int e = FloppyRestore::NONE; e <= FloppyRestore::NOT_ACCEPTED; e++)
        for (int d = 0; d <= 32; d++) {
            const auto de = (UfiDisk::Error)d;
            Text x;
            fwt::prepareError(x.t, (FloppyRestore::Error)e, de, 153);
            checkFits(x.buf);
            for (int touched = 0; touched < 2; touched++)
                for (int partial = 0; partial < 2; partial++) {
                    Text y;
                    fwt::writeError(y.t, (FloppyRestore::Error)e, de, 153, touched, partial, kBackup);
                    checkFits(y.buf);
                    CHECK_EQ(strstr(y.buf, "not written") != nullptr, !touched);
                    CHECK_EQ(strstr(y.buf, "C76 H1, incomplete") != nullptr, touched != 0);
                    CHECK_EQ(strstr(y.buf, kBackup) != nullptr, touched && !partial);
                }
        }
    Text a, b;
    fwt::done(a.t, false);
    fwt::done(b.t, true);
    checkFits(a.buf);
    checkFits(b.buf);
    Text m;
    fwt::writeError(m.t, FloppyRestore::MISMATCH, UfiDisk::MEDIUM, 2, true, false, kBackup);
    CHECK_EQ(strcmp(m.buf, "VERIFY C1 H0, incomplete; old: FD-6930FE0F.XDF"), 0);
}

void checkInfo(const MenuHost::FloppyWriteInfo& info) {
    for (int i = 0; i < info.count + info.badCount; i++) {
        const char* s = i < info.count ? info.line[i] : info.bad[i - info.count];
        Text x;
        x.t.add(" ").add(s); // メニューは空白を 1 つ付ける
        checkFits(x.buf);
        CHECK_EQ(strlen(s) < MenuHost::FloppyWriteInfo::TEXT - 1, true);
    }
}

// 名前の行 (1 行か 2 行) をつないだもの
void nameOf(const char* name, char* out, int& lines) {
    MenuHost::FloppyWriteInfo info;
    fwt::addFileName(info, name);
    checkInfo(info);
    lines = info.count;
    out[0] = 0;
    for (int i = 0; i < info.count; i++) {
        if (strchr(name, '\xff') == nullptr) { // 行ごとに、字の途中で切れていない
            size_t at = 0;
            uint32_t cp;
            while (utf8Next(info.line[i], strlen(info.line[i]), at, cp)) CHECK_EQ(cp != 0xfffd, true);
        }
        CHECK_EQ(strncmp(info.line[i], i == 0 ? "File:  " : "       ", 7), 0);
        strcat(out, info.line[i] + 7);
    }
}

void testNames() {
    char got[400];
    int lines = 0;
    char name[128];
    // 半角。40 桁までは 1 行、80 桁までは 2 行で全部、それより長ければ終わりの部分 (拡張子) も見せる
    for (int n = 5; n <= 95; n++) {
        for (int i = 0; i < n; i++) name[i] = (char)('a' + i % 26);
        memcpy(name + n - 4, ".XDF", 5);
        nameOf(name, got, lines);
        CHECK_EQ(lines, n <= 40 ? 1 : 2);
        CHECK_EQ(strcmp(got, name) == 0, n <= 80);
        CHECK_EQ(strcmp(got + strlen(got) - 4, ".XDF"), 0);
        if (n > 80) CHECK_EQ(strstr(got, "...") != nullptr, true);
    }
    // 全角 (1 字 3 バイト、2 桁)、半角カタカナ (1 字 3 バイト、1 桁)、結合しない字が混じるもの
    static const char* const kChars[] = {"漢", "ｱ", "é", "😀", "\xff"};
    for (const char* c : kChars)
        for (int lead = 0; lead < 3; lead++) {
            name[0] = 0;
            for (int i = 0; i < lead; i++) strcat(name, "a");
            while (strlen(name) + strlen(c) + 4 < 96) strcat(name, c);
            strcat(name, ".XDF");
            nameOf(name, got, lines);
            CHECK_EQ(strcmp(got + strlen(got) - 4, ".XDF"), 0);
            if (strcmp(c, "\xff") != 0) CHECK_EQ(strstr(got, "...") != nullptr || strcmp(got, name) == 0, true);
        }
}

void testConfirm() {
    char longName[96];
    memset(longName, 'W', 91);
    strcpy(longName + 91, ".XDF");
    for (int k = 0; k < 8; k++) {
        MenuHost::FloppyWriteInfo info;
        fwt::confirmInfo(info, 3, k & 1 ? longName : "A.XDF", 0x0930fe0f, k & 2 ? 0x0930fe0f : 0xffffffff,
                         k & 4 ? "FD-6930FE0F-PARTIAL.XDF" : kBackup, k & 1, k & 2);
        CHECK_EQ(info.count, k & 1 ? 8 : 7);
        checkInfo(info);
        CHECK_EQ(strcmp(info.line[info.count - 5], "       CRC-32 0930fe0f"), 0);
        CHECK_EQ(strstr(info.line[info.count - 4], "(same as the file)") != nullptr, (k & 2) != 0);
        CHECK_EQ(strcmp(info.line[info.count - 2], k & 1 ? "Boot sector: same" : "Boot sector: will change"), 0);
        CHECK_EQ(strcmp(info.line[info.count - 1], k & 2 ? "Write protect: unknown" : "Write protect: off"), 0);
    }
}

void testPartial() {
    static uint8_t old[FloppyRestore::IMAGE_BYTES], work[FloppyRestore::TRACK_BYTES];
    for (int tracks = 1; tracks < 154; tracks += 19) {
        FakeUfi f;
        for (int t = 154 - tracks; t < 154; t++)
            for (int b = 0; b < 8; b++) f.unreadable[t * 8 + b] = true;
        FloppyRestore r;
        CHECK_EQ(r.prepare(&f, old, work, nullptr), true);
        MenuHost::FloppyWriteInfo info;
        fwt::partialInfo(info, r);
        checkInfo(info);
        CHECK_EQ(info.partial, true);
        CHECK_EQ(strstr(info.bad[info.badCount - 1], "more") != nullptr, tracks > 56);
        CHECK_EQ(strstr(info.bad[1], tracks == 1 ? "C76H1" : "C") != nullptr, true);
    }
}

// 確認と進みの画面を、実際の行で見る (見出し、尋ねる文、Yes / No も)
class Host : public MenuHost {
public:
    Menu* menu = nullptr;
    bool partial = false;
    int floppyWriteUnit() const override { return 3; }
    void listFiles(MenuFileKind kind, MenuFileList& list) override {
        if (MenuFileList::matches("LONG.XDF", kind)) list.add("LONG.XDF");
    }
    bool prepareFloppyWrite(const char* name, FloppyWriteInfo& info, MenuText& msg) override {
        (void)msg;
        fwt::confirmInfo(info, 3, name, 1, 1, "FD-6930FE0F-PARTIAL.XDF", true, true);
        if (partial) {
            info.partial = true;
            info.addBad("1232 sectors in 154 tracks cannot be read:");
        }
        char buf[Menu::LINE_BYTES];
        MenuText t(buf, sizeof buf);
        fwt::progress(t, FloppyRestoreProgress::READING, 154, 154);
        menu->showProgress(buf);
        checkPage();
        menu->showProgress("");
        return true;
    }
    void checkPage() {
        static char lines[40][Menu::LINE_BYTES];
        int hl = -1;
        const int n = menu->linesForTest(lines, hl);
        for (int i = 0; i < n; i++) checkFits(lines[i]);
    }
    bool insertDisk(int, const char*, MenuText&) override { return true; }
    const char* diskName(int) override { return ""; }
    bool saveState(int, MenuText&) override { return true; }
    bool loadState(int, MenuText&) override { return true; }
    bool saveSettings(const Settings&, MenuText&) override { return true; }
    void resetMachine() override {}
    bool restart(const Settings&, MenuText&) override { return true; }
};

} // namespace

int main() {
    UfiGate::enable(0);
    testStatus();
    testNames();
    testConfirm();
    testPartial();
    for (int partial = 0; partial < 2; partial++) {
        Host h;
        Menu m(h);
        h.menu = &m;
        h.partial = partial;
        m.open(Settings(), Settings());
        for (int i = 0; i < 40 && strcmp(m.currentLabel(), "Write XDF") != 0; i++) m.key(menu_key::DOWN);
        CHECK_EQ(strcmp(m.currentLabel(), "Write XDF"), 0);
        h.checkPage();
        m.key(menu_key::ENTER);
        h.checkPage();
        m.key(menu_key::DOWN);
        m.key(menu_key::ENTER);
        CHECK_EQ(strcmp(m.currentLabel(), "No"), 0);
        h.checkPage();
        if (partial) {
            m.key(menu_key::DOWN);
            m.key(menu_key::ENTER);
            CHECK_EQ(strcmp(m.currentLabel(), "No"), 0);
            h.checkPage();
        }
        m.key(menu_key::ENTER);
        h.checkPage();
        CHECK_EQ(strcmp(m.status(), "not written"), 0);
    }
    return finish();
}
