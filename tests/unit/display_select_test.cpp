// src/platform/display_select.h のテスト: ベアメタル版で画面を出す HDMI の口の選び方 (#329)
#include "platform/display_select.h"
#include "test_check.h"

namespace {

using namespace platform;

DisplayInfo d(unsigned id, bool edid) {
    DisplayInfo r;
    r.idKnown = true;
    r.id = id;
    r.edid = edid;
    return r;
}

void testOption() {
    CHECK_EQ((int)parseDisplayOption(nullptr), (int)DisplayOption::AUTO);
    CHECK_EQ((int)parseDisplayOption("auto"), (int)DisplayOption::AUTO);
    CHECK_EQ((int)parseDisplayOption("hdmi0"), (int)DisplayOption::HDMI0);
    CHECK_EQ((int)parseDisplayOption("hdmi1"), (int)DisplayOption::HDMI1);
    CHECK_EQ((int)parseDisplayOption("hdmi"), (int)DisplayOption::INVALID);
    CHECK_EQ((int)parseDisplayOption("1"), (int)DisplayOption::INVALID);
    CHECK_EQ((int)parseDisplayOption(""), (int)DisplayOption::INVALID);
}

void testSingle() {
    DisplayPick why;
    DisplayInfo one[1] = {d(DISPLAY_ID_HDMI1, true)};
    CHECK_EQ(pickDisplay(one, 1, DisplayOption::AUTO, why), 0u);
    CHECK_EQ((int)why, (int)DisplayPick::SINGLE);
    CHECK_EQ(pickDisplay(one, 0, DisplayOption::HDMI1, why), 0u);
    CHECK_EQ((int)why, (int)DisplayPick::SINGLE);
    CHECK_EQ(otherDisplay(one, 1, 0), -1);
}

void testAuto() {
    DisplayPick why;
    // Pi 400J (#329) で見えた値: 番号 0 が HDMI0 (モニタなし、EDID が読めない)、番号 1 が HDMI1 (モニタあり)
    DisplayInfo pi400[2] = {d(DISPLAY_ID_HDMI0, false), d(DISPLAY_ID_HDMI1, true)};
    CHECK_EQ(pickDisplay(pi400, 2, DisplayOption::AUTO, why), 1u);
    CHECK_EQ((int)why, (int)DisplayPick::MONITOR_HDMI1);
    CHECK_EQ(otherDisplay(pi400, 2, 1), 0);
    // 並びが逆でも口で選ぶ
    DisplayInfo pi4[2] = {d(DISPLAY_ID_HDMI0, true), d(DISPLAY_ID_HDMI1, false)};
    CHECK_EQ(pickDisplay(pi4, 2, DisplayOption::AUTO, why), 0u);
    CHECK_EQ(otherDisplay(pi4, 2, 0), 1);
    // モニタが HDMI1 だけ
    DisplayInfo h1[2] = {d(DISPLAY_ID_HDMI0, false), d(DISPLAY_ID_HDMI1, true)};
    CHECK_EQ(pickDisplay(h1, 2, DisplayOption::AUTO, why), 1u);
    CHECK_EQ((int)why, (int)DisplayPick::MONITOR_HDMI1);
    // 両方にモニタがあれば HDMI0
    DisplayInfo both[2] = {d(DISPLAY_ID_HDMI1, true), d(DISPLAY_ID_HDMI0, true)};
    CHECK_EQ(pickDisplay(both, 2, DisplayOption::AUTO, why), 1u);
    CHECK_EQ((int)why, (int)DisplayPick::MONITOR_HDMI0);
    // どれも EDID が読めなければ番号 0 (いままでどおり)
    DisplayInfo none[2] = {d(DISPLAY_ID_HDMI1, false), d(DISPLAY_ID_HDMI0, false)};
    CHECK_EQ(pickDisplay(none, 2, DisplayOption::AUTO, why), 0u);
    CHECK_EQ((int)why, (int)DisplayPick::NO_MONITOR);
    // 口の ID が分からなくても、EDID が読めた画面
    DisplayInfo unk[2] = {DisplayInfo(), DisplayInfo()};
    unk[1].edid = true;
    CHECK_EQ(pickDisplay(unk, 2, DisplayOption::AUTO, why), 1u);
    CHECK_EQ((int)why, (int)DisplayPick::MONITOR_OTHER);
    // DSI (ID 0) など HDMI でない画面がほかにあっても、TS-6BGA には HDMI の口を選ぶ
    DisplayInfo dsi[3] = {d(0, true), d(DISPLAY_ID_HDMI0, true), d(DISPLAY_ID_HDMI1, false)};
    CHECK_EQ(pickDisplay(dsi, 3, DisplayOption::AUTO, why), 1u);
    CHECK_EQ(otherDisplay(dsi, 3, 1), 2);
    // 上限より多く数えられても、上限までしか見ない
    DisplayInfo many[MAX_DISPLAYS] = {d(0, false), d(0, false), d(0, false), d(DISPLAY_ID_HDMI0, true)};
    CHECK_EQ(pickDisplay(many, 9, DisplayOption::AUTO, why), 3u);
}

void testForced() {
    DisplayPick why;
    DisplayInfo pi400[2] = {d(DISPLAY_ID_HDMI0, false), d(DISPLAY_ID_HDMI1, true)};
    // EDID がなくても、書いた口を使う
    CHECK_EQ(pickDisplay(pi400, 2, DisplayOption::HDMI0, why), 0u);
    CHECK_EQ((int)why, (int)DisplayPick::OPTION);
    CHECK_EQ(pickDisplay(pi400, 2, DisplayOption::HDMI1, why), 1u);
    CHECK_EQ((int)why, (int)DisplayPick::OPTION);
    // 書いた口が数えられていなければ auto と同じ
    DisplayInfo only0[2] = {d(0, false), d(DISPLAY_ID_HDMI0, true)};
    CHECK_EQ(pickDisplay(only0, 2, DisplayOption::HDMI1, why), 1u);
    CHECK_EQ((int)why, (int)DisplayPick::MONITOR_HDMI0);
    // 読めない値は auto と同じ
    CHECK_EQ(pickDisplay(pi400, 2, DisplayOption::INVALID, why), 1u);
}

} // namespace

int main() {
    testOption();
    testSingle();
    testAuto();
    testForced();
    return finish();
}
