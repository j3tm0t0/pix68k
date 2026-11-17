// src/platform/config_text.h と settings.h のテスト: pix68k.cfg の読み方、書き換えたときに注釈・知らない行・
// 改行の形が残ること、項目の表 (範囲、選択肢)
#include "platform/config_text.h"
#include "platform/settings.h"
#include "platform/settings_file.h"
#include "test_check.h"

#include <string.h>

namespace {

using platform::ConfigText;
using platform::Settings;
using platform::SettingResult;

bool same(const ConfigText& c, const char* want) {
    bool ok = c.size() == strlen(want) && memcmp(c.data(), want, c.size()) == 0;
    if (!ok) printf("  got:\n%.*s\n  want:\n%s\n", (int)c.size(), c.data(), want);
    return ok;
}

void testRead() {
    printf("read\n");
    const char* text = "# pix68k の設定\n"
                       "fd0 = HUMAN302.XDF   # 起動ディスク\n"
                       "  clock=10\n"
                       "garbage line\n"
                       "\n"
                       "clock = 16\r\n"
                       "sc0=HUMAN.HDS\n"
                       "cpu=68030";
    ConfigText c;
    CHECK_EQ(c.load(text, strlen(text)), 1);
    int entries = 0, bad = 0;
    char last[64] = {};
    c.forEach(
        [&](const ConfigText::Entry& e) {
            entries++;
            platform::copyString(last, sizeof last, e.key);
        },
        [&](const char* line) {
            bad++;
            CHECK_EQ(strcmp(line, "garbage line"), 0);
        });
    CHECK_EQ(entries, 5);
    CHECK_EQ(bad, 1);
    CHECK_EQ(strcmp(last, "cpu"), 0);
    char v[64];
    CHECK_EQ(c.get("fd0", v, sizeof v), 1);
    CHECK_EQ(strcmp(v, "HUMAN302.XDF"), 0);
    CHECK_EQ(c.get("clock", v, sizeof v), 1); // 後の行が勝つ
    CHECK_EQ(strcmp(v, "16"), 0);
    CHECK_EQ(c.get("cpu", v, sizeof v), 1); // 最後の行に改行がなくても読む
    CHECK_EQ(strcmp(v, "68030"), 0);
    CHECK_EQ(c.get("hd0", v, sizeof v), 0);
}

void testWrite() {
    printf("write\n");
    // 注釈、知らない行 (PR #38 のキーなど)、空行、値の後ろの注釈が残る。最後の行の値だけが変わる
    const char* text = "# pix68k の設定\n"
                       "fd0 = HUMAN302.XDF   # 起動ディスク\n"
                       "clock=10\n"
                       "ram=4\n"
                       "scsi_ex=on\n"
                       "sc0=HUMAN.HDS\n"
                       "cd=GAME.ISO\n"
                       "cd_id=6\n"
                       "\n"
                       "clock = 16 # 後の行\n"
                       "mystery=1\n";
    ConfigText c;
    c.load(text, strlen(text));
    CHECK_EQ(c.set("clock", "10"), 1);
    CHECK_EQ(c.set("fd0", "GAME.XDF"), 1);
    CHECK_EQ(c.set("cpu", "68030"), 1); // ない行は最後に足す
    CHECK_EQ(c.set("fd1", ""), 1);
    CHECK_EQ(same(c, "# pix68k の設定\n"
                     "fd0 = GAME.XDF   # 起動ディスク\n"
                     "clock=10\n"
                     "ram=4\n"
                     "scsi_ex=on\n"
                     "sc0=HUMAN.HDS\n"
                     "cd=GAME.ISO\n"
                     "cd_id=6\n"
                     "\n"
                     "clock = 10 # 後の行\n"
                     "mystery=1\n"
                     "cpu=68030\n"
                     "fd1=\n"),
             1);
    // 値に # や改行は書けない
    CHECK_EQ(c.set("fd0", "A#B"), 0);
    CHECK_EQ(c.set("fd0", "A\nB"), 0);
    // 空の値の行も書き換えられる
    CHECK_EQ(c.set("fd1", "X.XDF"), 1);
    char v[64];
    CHECK_EQ(c.get("fd1", v, sizeof v), 1);
    CHECK_EQ(strcmp(v, "X.XDF"), 0);
}

void testCrlf() {
    printf("crlf\n");
    // CRLF のファイルには CRLF で足す。最後の行に改行がなければ先に足す
    const char* text = "fd0=A.XDF\r\nclock=16";
    ConfigText c;
    c.load(text, strlen(text));
    CHECK_EQ(c.set("clock", "10"), 1);
    CHECK_EQ(c.set("cpu", "68000"), 1);
    CHECK_EQ(same(c, "fd0=A.XDF\r\nclock=10\r\ncpu=68000\r\n"), 1);
    // 空のファイル
    ConfigText e;
    e.load("", 0);
    CHECK_EQ(e.set("fd0", "B.XDF"), 1);
    CHECK_EQ(same(e, "fd0=B.XDF\n"), 1);
    // 注釈の中の = や、注釈にしたキーは行とみなさない
    const char* t2 = "#fd0=OLD.XDF\nfd1=C.XDF # a=b\n";
    ConfigText d;
    d.load(t2, strlen(t2));
    CHECK_EQ(d.set("fd0", "NEW.XDF"), 1);
    CHECK_EQ(same(d, "#fd0=OLD.XDF\nfd1=C.XDF # a=b\nfd0=NEW.XDF\n"), 1);
}

void testSettings() {
    printf("settings\n");
    Settings s;
    CHECK_EQ((int)applySetting(s, "clock", "10"), (int)SettingResult::OK);
    CHECK_EQ(s.clockMHz, 10);
    CHECK_EQ((int)applySetting(s, "clock", "12"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ(s.clockMHz, 10);
    CHECK_EQ((int)applySetting(s, "cpu", "68020"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "cpu", "68060"), (int)SettingResult::OK);
    CHECK_EQ((int)applySetting(s, "ram", "13"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "ram", "12"), (int)SettingResult::OK);
    CHECK_EQ((int)applySetting(s, "local_mem", "x"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "max_speed", "on"), (int)SettingResult::OK);
    CHECK_EQ(s.maxSpeed, 1);
    CHECK_EQ((int)applySetting(s, "max_speed", "yes"), (int)SettingResult::BAD_VALUE);
    // 060turbo の命令の速さと、最高速モードの上限 (実機の 68060 を 100 とした %)
    CHECK_EQ(s.speed060, 100);
    CHECK_EQ(s.maxSpeedLimit, 0);
    CHECK_EQ((int)applySetting(s, "speed060", "30"), (int)SettingResult::OK);
    CHECK_EQ(s.speed060, 30);
    CHECK_EQ((int)applySetting(s, "speed060", "4"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "speed060", "3201"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ(s.speed060, 30);
    CHECK_EQ((int)applySetting(s, "max_speed_limit", "100"), (int)SettingResult::OK);
    CHECK_EQ(s.maxSpeedLimit, 100);
    CHECK_EQ((int)applySetting(s, "max_speed_limit", "-1"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "keyboard", "pi400"), (int)SettingResult::OK);
    CHECK_EQ(s.keyboard, 2);
    CHECK_EQ((int)applySetting(s, "hat", "1"), (int)SettingResult::OK);
    CHECK_EQ(s.hat, 1);
    CHECK_EQ((int)applySetting(s, "sram_memsize", "keep"), (int)SettingResult::OK);
    CHECK_EQ(s.fixSramMemsize, 0);
    CHECK_EQ(s.scale, 1); // 既定は auto (拡大する)
    CHECK_EQ((int)applySetting(s, "scale", "off"), (int)SettingResult::OK);
    CHECK_EQ(s.scale, 0);
    CHECK_EQ((int)applySetting(s, "scale", "2"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "scale", "auto"), (int)SettingResult::OK);
    CHECK_EQ(s.scale, 1);
    CHECK_EQ(s.scanline, 0); // 既定は off
    CHECK_EQ((int)applySetting(s, "scanline", "on"), (int)SettingResult::OK);
    CHECK_EQ(s.scanline, 1);
    CHECK_EQ((int)applySetting(s, "scanline", "50"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "scanline", "off"), (int)SettingResult::OK);
    CHECK_EQ(s.scanline, 0);
    CHECK_EQ(s.osd, 2); // 既定は on (余白に入るときだけ出す)
    CHECK_EQ((int)applySetting(s, "osd", "fps"), (int)SettingResult::OK);
    CHECK_EQ(s.osd, 1);
    CHECK_EQ((int)applySetting(s, "osd", "on"), (int)SettingResult::OK);
    CHECK_EQ(s.osd, 2);
    CHECK_EQ((int)applySetting(s, "osd", "yes"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "osd", "off"), (int)SettingResult::OK);
    CHECK_EQ(s.osd, 0);
    CHECK_EQ((int)applySetting(s, "fd1", "DISK.XDF"), (int)SettingResult::OK);
    CHECK_EQ(strcmp(s.fd[1], "DISK.XDF"), 0);
    CHECK_EQ((int)applySetting(s, "scsi_ex", "on"), (int)SettingResult::OK); // PR #38 の項目
    CHECK_EQ((int)applySetting(s, "sc7", "X.HDS"), (int)SettingResult::UNKNOWN_KEY);
    CHECK_EQ((int)applySetting(s, "cd_id", "7"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "mystery", "1"), (int)SettingResult::UNKNOWN_KEY);
    CHECK_EQ((int)applySetting(s, "neptune", "on"), (int)SettingResult::OK);
    CHECK_EQ(s.neptune, 1);
    CHECK_EQ((int)applySetting(s, "neptune_mac", "02:68:6B:00:00:2a"), (int)SettingResult::OK);
    uint8_t mac[6];
    CHECK_EQ(platform::parseMac(s.neptuneMac, mac), 1);
    CHECK_EQ(mac[3] << 16 | mac[4] << 8 | mac[5], 0x00002a);
    CHECK_EQ(mac[2], 0x6b);
    CHECK_EQ(platform::parseMac("02-00-00-00-00-01", mac), 1);
    CHECK_EQ(platform::parseMac("03:00:00:00:00:01", mac), 0); // マルチキャスト
    CHECK_EQ(platform::parseMac("02:00:00:00:00", mac), 0);
    CHECK_EQ(platform::parseMac("02:00:00:00:00:011", mac), 0);
    CHECK_EQ(platform::parseMac("02:00:00:00:0g:01", mac), 0);
    // 数値演算プロセッサボード: on / off と、2 枚の 2
    CHECK_EQ(s.fpuBoard, 0); // 既定は off
    CHECK_EQ((int)applySetting(s, "fpu_board", "on"), (int)SettingResult::OK);
    CHECK_EQ(s.fpuBoard, 1);
    CHECK_EQ((int)applySetting(s, "fpu_board", "2"), (int)SettingResult::OK);
    CHECK_EQ(s.fpuBoard, 2);
    CHECK_EQ((int)applySetting(s, "fpu_board", "3"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "fpu_board", "yes"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ(s.fpuBoard, 2);
    {
        char fv[8];
        const platform::SettingKey* fk = platform::findSettingKey("fpu_board");
        CHECK_EQ(fk != nullptr && fk->restart && fk->host, 1);
        CHECK_EQ(formatSetting(s, *fk, fv, sizeof fv), 1);
        CHECK_EQ(strcmp(fv, "2"), 0); // 2 枚は 2 のまま書く (on に丸めない)
        Settings f;
        CHECK_EQ((int)applySetting(f, "fpu_board", "1"), (int)SettingResult::OK);
        CHECK_EQ(f.fpuBoard, 1);
        CHECK_EQ(formatSetting(f, *fk, fv, sizeof fv), 1);
        CHECK_EQ(strcmp(fv, "on"), 0);
        CHECK_EQ((int)applySetting(f, "fpu_board", "0"), (int)SettingResult::OK);
        CHECK_EQ(formatSetting(f, *fk, fv, sizeof fv), 1);
        CHECK_EQ(strcmp(fv, "off"), 0);
    }
    // MIDI ボード
    CHECK_EQ(s.midi, 0); // 既定は off
    CHECK_EQ(s.midiOut, 0);
    CHECK_EQ(s.midiVoices, 48);
    CHECK_EQ((int)applySetting(s, "midi", "on"), (int)SettingResult::OK);
    CHECK_EQ(s.midi, 1);
    CHECK_EQ((int)applySetting(s, "midi_out", "uart"), (int)SettingResult::OK);
    CHECK_EQ(s.midiOut, 1);
    CHECK_EQ((int)applySetting(s, "midi_out", "usb"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "midi_sf2", "GeneralUser.sf2"), (int)SettingResult::OK);
    CHECK_EQ(strcmp(s.midiSf2, "GeneralUser.sf2"), 0);
    CHECK_EQ((int)applySetting(s, "midi_voices", "3"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "midi_voices", "32"), (int)SettingResult::OK);
    CHECK_EQ(s.midiVoices, 32);
    CHECK_EQ((int)applySetting(s, "midi_out", "mt32"), (int)SettingResult::OK);
    CHECK_EQ(s.midiOut, 3);
    CHECK_EQ(s.midiMt32Ctrl[0], 0);
    CHECK_EQ((int)applySetting(s, "midi_mt32_ctrl", "MT32_CONTROL.ROM"), (int)SettingResult::OK);
    CHECK_EQ(strcmp(s.midiMt32Ctrl, "MT32_CONTROL.ROM"), 0);
    CHECK_EQ((int)applySetting(s, "midi_mt32_pcm", "/roms/MT32_PCM.ROM"), (int)SettingResult::OK);
    CHECK_EQ(strcmp(s.midiMt32Pcm, "/roms/MT32_PCM.ROM"), 0);
    CHECK_EQ((int)applySetting(s, "midi_out", "sc55"), (int)SettingResult::OK);
    CHECK_EQ(s.midiOut, 4);
    CHECK_EQ(s.midiSc55[0], 0);
    CHECK_EQ((int)applySetting(s, "midi_sc55", "SC55MK2"), (int)SettingResult::OK);
    CHECK_EQ(strcmp(s.midiSc55, "SC55MK2"), 0);
    CHECK_EQ(s.midiUart, 0);
    CHECK_EQ((int)applySetting(s, "midi_uart", "3"), (int)SettingResult::OK);
    CHECK_EQ(s.midiUart, 3);
    CHECK_EQ((int)applySetting(s, "midi_uart", "10"), (int)SettingResult::OK);
    CHECK_EQ(s.midiUart, 10);
    CHECK_EQ((int)applySetting(s, "midi_uart", "6"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "midi_uart", "11"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ(s.midiUart, 10);
    CHECK_EQ(s.midiUartFamily, 0);
    CHECK_EQ((int)applySetting(s, "midi_uart_family", "mt32"), (int)SettingResult::OK);
    CHECK_EQ(s.midiUartFamily, 1);
    CHECK_EQ((int)applySetting(s, "midi_uart_family", "gm"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(s, "midi_uart_family", "gs"), (int)SettingResult::OK);
    CHECK_EQ(s.midiUartFamily, 0);
    const char* arg = nullptr;
    CHECK_EQ((int)platform::parseNeptuneLink("", arg), (int)platform::NeptuneLink::DEFAULT);
    CHECK_EQ((int)platform::parseNeptuneLink("echo", arg), (int)platform::NeptuneLink::ECHO);
    CHECK_EQ((int)platform::parseNeptuneLink("pcap:en0", arg), (int)platform::NeptuneLink::PCAP);
    CHECK_EQ(strcmp(arg, "en0"), 0);
    CHECK_EQ((int)platform::parseNeptuneLink("pcap", arg), (int)platform::NeptuneLink::BAD);
    CHECK_EQ((int)platform::parseNeptuneLink("echo:x", arg), (int)platform::NeptuneLink::BAD);
    CHECK_EQ((int)platform::parseNeptuneLink("tap:tap0", arg), (int)platform::NeptuneLink::TAP);

    // 書く形
    char v[32];
    const platform::SettingKey* k = platform::findSettingKey("keyboard");
    CHECK_EQ(formatSetting(s, *k, v, sizeof v), 1);
    CHECK_EQ(strcmp(v, "pi400us"), 0);
    k = platform::findSettingKey("local_mem");
    CHECK_EQ(formatSetting(Settings(), *k, v, sizeof v), 0); // 書いていない (-1)
    k = platform::findSettingKey("hat");
    CHECK_EQ(formatSetting(Settings(), *k, v, sizeof v), 1);
    CHECK_EQ(strcmp(v, "auto"), 0);
    // rs232c= (RS-232C の相手)
    {
        unsigned port = 1;
        char addr[16] = "x";
        CHECK_EQ((int)platform::parseRs232c("", port, addr, sizeof addr), (int)platform::Rs232cLink::OFF);
        CHECK_EQ((int)platform::parseRs232c("off", port, addr, sizeof addr), (int)platform::Rs232cLink::OFF);
        CHECK_EQ((int)platform::parseRs232c("tcp:2324", port, addr, sizeof addr), (int)platform::Rs232cLink::TCP);
        CHECK_EQ(port, 2324u);
        CHECK_EQ(addr[0], 0);
        CHECK_EQ((int)platform::parseRs232c("tcp:0.0.0.0:23", port, addr, sizeof addr), (int)platform::Rs232cLink::TCP);
        CHECK_EQ(port, 23u);
        CHECK_EQ(strcmp(addr, "0.0.0.0"), 0);
        static const char* const kBad[] = {"tcp:", "tcp:0", "tcp:65536", "tcp:12x", "udp:1", "tcp::5", "tcp:1:",
                                           "tcp:255.255.255.255.1:5"};
        for (const char* v : kBad)
            CHECK_EQ((int)platform::parseRs232c(v, port, addr, sizeof addr), (int)platform::Rs232cLink::BAD);
        Settings r;
        CHECK_EQ((int)applySetting(r, "rs232c", "tcp:2324"), (int)SettingResult::OK);
        CHECK_EQ(strcmp(r.rs232c, "tcp:2324"), 0);
    }
    // 表のすべての項目で、既定の値を書いて読むと同じになる
    for (const platform::SettingKey& key : platform::kSettingKeys) {
        Settings a, b;
        if (!formatSetting(a, key, v, sizeof v)) continue;
        CHECK_EQ((int)applySetting(b, key.key, v), (int)SettingResult::OK);
        CHECK_EQ(sameSetting(a, b, key), 1);
    }

    // Config に写す: 68030 のときは clock を使わない、060turbo のローカルメモリの既定は 128MB
    Settings c;
    applySetting(c, "cpu", "68060");
    x68k::Config cfg;
    c.toConfig(cfg);
    if (x68k::Config::has020()) {
        CHECK_EQ(cfg.cpuHz, 50000000u);
        CHECK_EQ(cfg.localMemSize, 128u << 20);
    }
    CHECK_EQ(cfg.speed060, 100);
    CHECK_EQ(cfg.maxSpeedLimit060, 0);
    applySetting(c, "speed060", "40");
    applySetting(c, "max_speed_limit", "150");
    c.toConfig(cfg);
    CHECK_EQ(cfg.speed060, 40);
    CHECK_EQ(cfg.maxSpeedLimit060, 150);
    Settings d;
    applySetting(d, "clock", "10");
    x68k::Config cfg2;
    d.toConfig(cfg2);
    CHECK_EQ(cfg2.cpuHz, 10000000u);
    CHECK_EQ(cfg2.localMemSize, 0u);

    // clock= は CPU ごとに読む: 68060 は 50 (既定) か 75、68000 は 10 か 16 (既定)。合わない値はその CPU の既定
    Settings e;
    CHECK_EQ((int)applySetting(e, "clock", "75"), (int)SettingResult::OK);
    CHECK_EQ((int)applySetting(e, "clock", "50"), (int)SettingResult::OK);
    CHECK_EQ((int)applySetting(e, "clock", "60"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ((int)applySetting(e, "clock", "25"), (int)SettingResult::BAD_VALUE);
    CHECK_EQ(Settings::clockFor(68000, 75), 16);
    CHECK_EQ(Settings::clockFor(68000, 10), 10);
    CHECK_EQ(Settings::clockFor(68030, 10), 25);
    CHECK_EQ(Settings::clockFor(68060, 16), 50); // 既定の clock=16 のままの cpu=68060 は 50MHz
    CHECK_EQ(Settings::clockFor(68060, 75), 75);
    if (x68k::Config::has020()) {
        Settings f;
        applySetting(f, "cpu", "68060");
        x68k::Config cfg3;
        f.toConfig(cfg3);
        CHECK_EQ(cfg3.cpuHz, 50000000u);
        CHECK_EQ(cfg3.busWait060, 6);
        applySetting(f, "clock", "75");
        x68k::Config cfg4;
        f.toConfig(cfg4);
        CHECK_EQ(cfg4.cpuHz, 75000000u);
        CHECK_EQ(cfg4.busWait060, 9);
        applySetting(f, "cpu", "68000");
        x68k::Config cfg5;
        f.toConfig(cfg5);
        CHECK_EQ(cfg5.cpuHz, 16000000u); // 75 は 68000 では使わない
    }
}

// メニューで取り出して保存すると「fd0=」を書く (ベアメタル版は空の fd0= を「ドライブを空にする」と読む)
void testEject() {
    printf("eject\n");
    const char* text = "fd0=GAME.XDF # 起動\nclock=10\n";
    ConfigText c;
    c.load(text, strlen(text));
    Settings base;
    platform::readSettings(c, base, [](const char*, const char*, SettingResult) {}, [](const char*) {});
    Settings s = base;
    s.fd[0][0] = 0;
    CHECK_EQ(platform::writeSettings(c, s, base), 1);
    CHECK_EQ(same(c, "fd0= # 起動\nclock=10\n"), 1);
    char v[8];
    CHECK_EQ(c.get("fd0", v, sizeof v), 1); // 行はある (空の値)
    CHECK_EQ(v[0], 0);
}

} // namespace

int main() {
    testRead();
    testWrite();
    testCrlf();
    testSettings();
    testEject();
    return finish();
}
