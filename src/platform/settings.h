// pix68k.cfg の項目の表と、読んだ値を入れる Settings。ベアメタル版 (baremetal/machine_run.cpp) と
// ホスト版 (host/main.cpp の --config) と設定メニュー (platform/menu.h) が同じ表を使う。
//
// 項目を足すときは、Settings にメンバを足し、kSettingKeys に 1 行足す。値の読み方 (範囲、選択肢) は表の種類で決まる:
//   PATH   ファイル名 (文字列)
//   INT    lo..hi の整数。values があれば、その中の値だけ
//   ONOFF  on / off
//   CHOICE choices の文字列のどれか。Settings には choiceValues の同じ位置の整数を入れる
// 読んだ値を x68k::Config などに写すのは、それぞれの環境が行う (toConfig())。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "x68k/machine.h"

namespace platform {

struct Settings {
    static constexpr size_t PATH_SIZE = 128;
    char fd[2][PATH_SIZE] = {}; // fd0= / fd1= (空なら、ベアメタル版は /pix68k/fd<N>.{xdf,dim,d88} を探す)
    char hd0[PATH_SIZE] = {};   // hd0= (SASI ID 0)
    int clockMHz = 16;          // clock= (68000 では 10 か 16、68060 では 50 か 75。clockFor() が CPU に合わせて読む)
    int cpu = 68000;            // cpu= (68000、68030、68060)
    int ramMB = 4;              // ram= (本体のメモリ、1..12)
    int highMemMB = 0;          // high_mem= (X68030 のハイメモリ)
    int localMemMB = -1;        // local_mem= (060turbo のローカルメモリ。-1 は書いていない: 060turbo では 128)
    int maxSpeed = 0;           // max_speed=
    int speed060 = 100;         // speed060= (060turbo の命令の速さ。選んだクロックの実機の 68060 を 100 とした %)
    int maxSpeedLimit = 0;      // max_speed_limit= (060turbo の最高速モードの上限。同じ単位。0 は上限なし)
    int renderCore = 1;         // render_core= (ベアメタル版だけ)
    int soundCore = 1;          // sound_core= (ベアメタル版だけ)
    int hat = -1;               // hat= (ベアメタル版だけ。-1 auto、0、1)
    int keyboard = 0;           // keyboard= (ベアメタル版だけ。0 auto、1 usb、2 pi400us、3 pi400jp)
    int fixSramMemsize = 1;     // sram_memsize= (1 fix、0 keep)
    int scsiEx = 0;             // scsi_ex= (外付け SCSI ボード)
    char scsiExRom[PATH_SIZE] = {}; // scsi_ex_rom= (外付け SCSI ボードの ROM。空なら SCSIEXROM.DAT)
    char sc[7][PATH_SIZE] = {}; // sc0= から sc6= (SCSI のハードディスクのイメージ)
    char cd[PATH_SIZE] = {};    // cd= (CD-ROM のイメージ)
    int cdId = 6;               // cd_id= (CD-ROM をつなぐ SCSI ID)
    int fpuBoard = 0;           // fpu_board= (数値演算プロセッサボードの枚数 0..2。on は 1 枚、2 は 2 枚。cpu= に関係なくつなぐ)
    int mercury = 0;            // mercury= (まーきゅりーゆにっと)
    int neptune = 0;            // neptune= (ネットワークボード Neptune-X)
    char neptuneMac[PATH_SIZE] = {};  // neptune_mac= (ボードの MAC アドレス。空なら x68k::Neptune::DEFAULT_MAC)
    char neptuneLink[PATH_SIZE] = {}; // neptune_link= (フレームの送受信の先。空なら環境の既定。parseNeptuneLink())
    char rs232c[PATH_SIZE] = {};      // rs232c= (RS-232C の相手。空か off ならつながない。parseRs232c())
    int midi = 0;               // midi= (MIDI ボード CZ-6BM1)
    int midiOut = 0;            // midi_out= (MIDI の行き先: 0 sf2、1 uart (ベアメタル版だけ)、2 none、3 mt32、4 sc55)
    char midiSf2[PATH_SIZE] = {}; // midi_sf2= (SoundFont のファイル。空なら鳴らさない)
    int midiVoices = 48;        // midi_voices= (SoundFont の音源の同時に鳴らすボイスの数)
    char midiMt32Ctrl[PATH_SIZE] = {}; // midi_mt32_ctrl= (MT-32 / CM-32L の CONTROL の ROM)
    char midiMt32Pcm[PATH_SIZE] = {};  // midi_mt32_pcm= (MT-32 / CM-32L の PCM の ROM)
    char midiSc55[PATH_SIZE] = {};     // midi_sc55= (SC-55mkII の ROM を置いたディレクトリ)
    int midiUart = 0;           // midi_uart= (ベアメタル版だけ。midi_out=uart で使う UART。Circle の CSerialDevice の番号)
    int midiUartFamily = 0;     // midi_uart_family= (midi_out=uart の先の音源: 0 gs、1 mt32。送り直しと音の止め方を決める)
    int writeBack = 1;          // write_back= (ベアメタル版だけ。X68000 が書いた内容をイメージのファイルに書き戻す)
    char writeProtect[PATH_SIZE] = {}; // write_protect= (ベアメタル版だけ。書き込み禁止にするディスク。「fd1,sc0」のように並べる)
    // ネットワークの口 (ベアメタル版だけ。baremetal/host_net.h、platform/net_config.h)
    int net = 0;                // net= (on なら TCP/IP を動かし、HTTP の口と syslog を使う)
    char netIp[PATH_SIZE] = {}; // net_ip= (「192.168.1.50/24」。空なら DHCP)
    char netGw[PATH_SIZE] = {}; // net_gw= (固定の IP のときのゲートウェイ)
    char netDns[PATH_SIZE] = {}; // net_dns= (固定の IP のときの DNS サーバ)
    char netName[PATH_SIZE] = {}; // net_name= (DHCP で送るホスト名。空なら pix68k)
    char netAllow[PATH_SIZE] = {}; // net_allow= (受け付ける送り元。「192.168.1.0/24,10.0.0.5」)
    char netToken[PATH_SIZE] = {}; // net_token= (書く操作に要るトークン)
    char syslog[PATH_SIZE] = {}; // syslog= (ログを送る先。「192.168.1.2」か「192.168.1.2:514」)
    int scale = 1;              // scale= (ベアメタル版だけ。1 auto: 整数倍で拡大する、0 off。platform/frame_scale.h)
    int scanline = 0;           // scanline= (ベアメタル版だけ。on なら縦 2 倍以上に拡大したとき、各行の最後の 1 行を暗くする。platform/frame_scale.h)
    // osd= (画面の隅にフレームの数、速さ、温度などを出す。platform/osd.h)。0 off、2 on: X68000 の画面の外の余白に
    // 入るときだけ出す (既定)、1 fps: 余白に入らなければ X68000 の画面に重ねる
    int osd = 2;

    // cpu と clock の組み合わせから、実際に使うクロック (MHz) を決める。組み合わせが合わなければ (68060 で clock=16 など)
    // その CPU の既定 (68000 は 16、68060 は 50) にする。68030 はいつも 25
    static int clockFor(int cpu, int clockMHz) {
        if (cpu == 68030) return 25;
        if (cpu == 68060) return x68k::Config::valid060MHz(clockMHz) ? clockMHz : 50;
        return clockMHz == 10 ? 10 : 16;
    }
    int effectiveClockMHz() const { return clockFor(cpu, clockMHz); }

    // x68k::Config に写す。cpu と clock の組み合わせが合わなければ (68030 で clock=10 など) clock は使わない
    void toConfig(x68k::Config& c) const {
        c.setCpu(cpu);
        c.setClockMHz(effectiveClockMHz());
        c.ramSize = (uint32_t)ramMB << 20;
        c.highMemSize = (uint32_t)highMemMB << 20;
        c.localMemSize = (uint32_t)(localMemMB >= 0 ? localMemMB : (cpu == 68060 ? 128 : 0)) << 20;
        c.fixSramMemorySize = fixSramMemsize != 0;
        c.speed060 = speed060;
        c.maxSpeedLimit060 = maxSpeedLimit;
    }
};

struct SettingKey {
    enum Kind : uint8_t { PATH, INT, ONOFF, CHOICE };
    const char* key;
    Kind kind;
    size_t offset; // Settings の中の位置 (PATH は char[PATH_SIZE]、ほかは int)
    int lo, hi;    // INT の範囲
    const int* values;          // INT で、使える値の並び (nullptr なら lo..hi のすべて)。-1 で終わる
    const char* const* choices; // CHOICE の文字列 (nullptr で終わる)
    const int* choiceValues;    // CHOICE の文字列に対応する値
    bool restart;  // 変えたら電源を入れ直す (エミュレータを作り直す) まで反映されない
    bool host;     // ホスト版でも使う (false ならホスト版は黙って無視する)
};

namespace settings_detail {
static const int kClockValues[] = {10, 16, 50, 75, -1}; // 50 と 75 は cpu=68060 のとき
static const int kCpuValues[] = {68000, 68030, 68060, -1};
static const char* const kHatChoices[] = {"auto", "0", "1", nullptr};
static const int kHatValues[] = {-1, 0, 1};
static const char* const kKeyboardChoices[] = {"auto", "usb", "pi400us", "pi400jp", "pi400", nullptr};
static const int kKeyboardValues[] = {0, 1, 2, 3, 2};
static const char* const kSramChoices[] = {"fix", "keep", nullptr};
static const int kSramValues[] = {1, 0};
static const char* const kScaleChoices[] = {"auto", "off", nullptr};
static const int kScaleValues[] = {1, 0};
static const char* const kOsdChoices[] = {"off", "on", "fps", nullptr};
static const int kOsdValues[] = {0, 2, 1};
// fpu_board= は on / off のほか、2 で 2 枚目のボード ($E9E080) もつなぐ。1 と 0 も受け付ける (書くときは on / off)
static const char* const kFpuBoardChoices[] = {"off", "on", "2", "0", "1", nullptr};
static const int kFpuBoardValues[] = {0, 1, 2, 0, 1};
static const char* const kMidiOutChoices[] = {"sf2", "uart", "none", "mt32", "sc55", nullptr};
static const int kMidiOutValues[] = {0, 1, 2, 3, 4};
// 使えるかは機種で違う (Pi 3 は 0、Pi 4 は 0 と 2-5、Pi 5 は 0-4 と 10。ベアメタル版の StartMidiUart() が確かめる)
static const int kMidiUartValues[] = {0, 1, 2, 3, 4, 5, 10, -1};
static const char* const kMidiUartFamilyChoices[] = {"gs", "mt32", nullptr};
static const int kMidiUartFamilyValues[] = {0, 1};
} // namespace settings_detail

#define PIX68K_OFF(m) offsetof(Settings, m)
// clang-format off
static const SettingKey kSettingKeys[] = {
    // key            kind                 offset                       lo  hi    values / choices                                                                 restart host
    {"fd0",          SettingKey::PATH,   PIX68K_OFF(fd[0]),           0, 0,   nullptr, nullptr, nullptr,                                                          false, true},
    {"fd1",          SettingKey::PATH,   PIX68K_OFF(fd[1]),           0, 0,   nullptr, nullptr, nullptr,                                                          false, true},
    {"hd0",          SettingKey::PATH,   PIX68K_OFF(hd0),             0, 0,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"clock",        SettingKey::INT,    PIX68K_OFF(clockMHz),        10, 75, settings_detail::kClockValues, nullptr, nullptr,                                    true,  true},
    {"cpu",          SettingKey::INT,    PIX68K_OFF(cpu),             68000, 68060, settings_detail::kCpuValues, nullptr, nullptr,                                true,  true},
    {"ram",          SettingKey::INT,    PIX68K_OFF(ramMB),           1, 12,  nullptr, nullptr, nullptr,                                                          true,  true},
    {"high_mem",     SettingKey::INT,    PIX68K_OFF(highMemMB),       0, 16,  nullptr, nullptr, nullptr,                                                          true,  true},
    {"local_mem",    SettingKey::INT,    PIX68K_OFF(localMemMB),      0, 256, nullptr, nullptr, nullptr,                                                          true,  true},
    {"max_speed",    SettingKey::ONOFF,  PIX68K_OFF(maxSpeed),        0, 1,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"speed060",     SettingKey::INT,    PIX68K_OFF(speed060),        5, 3200, nullptr, nullptr, nullptr,                                                         true,  true},
    {"max_speed_limit", SettingKey::INT, PIX68K_OFF(maxSpeedLimit),   0, 3200, nullptr, nullptr, nullptr,                                                         true,  true},
    {"render_core",  SettingKey::ONOFF,  PIX68K_OFF(renderCore),      0, 1,   nullptr, nullptr, nullptr,                                                          true,  false},
    {"sound_core",   SettingKey::ONOFF,  PIX68K_OFF(soundCore),       0, 1,   nullptr, nullptr, nullptr,                                                          true,  false},
    {"sram_memsize", SettingKey::CHOICE, PIX68K_OFF(fixSramMemsize),  0, 0,   nullptr, settings_detail::kSramChoices, settings_detail::kSramValues,               true,  true},
    {"hat",          SettingKey::CHOICE, PIX68K_OFF(hat),             0, 0,   nullptr, settings_detail::kHatChoices, settings_detail::kHatValues,                 true,  false},
    {"keyboard",     SettingKey::CHOICE, PIX68K_OFF(keyboard),        0, 0,   nullptr, settings_detail::kKeyboardChoices, settings_detail::kKeyboardValues,       true,  false},
    {"scsi_ex",      SettingKey::ONOFF,  PIX68K_OFF(scsiEx),          0, 1,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"scsi_ex_rom",  SettingKey::PATH,   PIX68K_OFF(scsiExRom),       0, 0,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"sc0",          SettingKey::PATH,   PIX68K_OFF(sc[0]),           0, 0,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"sc1",          SettingKey::PATH,   PIX68K_OFF(sc[1]),           0, 0,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"sc2",          SettingKey::PATH,   PIX68K_OFF(sc[2]),           0, 0,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"sc3",          SettingKey::PATH,   PIX68K_OFF(sc[3]),           0, 0,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"sc4",          SettingKey::PATH,   PIX68K_OFF(sc[4]),           0, 0,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"sc5",          SettingKey::PATH,   PIX68K_OFF(sc[5]),           0, 0,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"sc6",          SettingKey::PATH,   PIX68K_OFF(sc[6]),           0, 0,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"cd",           SettingKey::PATH,   PIX68K_OFF(cd),              0, 0,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"cd_id",        SettingKey::INT,    PIX68K_OFF(cdId),            0, 6,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"fpu_board",    SettingKey::CHOICE, PIX68K_OFF(fpuBoard),        0, 0,   nullptr, settings_detail::kFpuBoardChoices, settings_detail::kFpuBoardValues,       true,  true},
    {"mercury",      SettingKey::ONOFF,  PIX68K_OFF(mercury),         0, 1,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"neptune",      SettingKey::ONOFF,  PIX68K_OFF(neptune),         0, 1,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"neptune_mac",  SettingKey::PATH,   PIX68K_OFF(neptuneMac),      0, 0,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"neptune_link", SettingKey::PATH,   PIX68K_OFF(neptuneLink),     0, 0,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"rs232c",       SettingKey::PATH,   PIX68K_OFF(rs232c),          0, 0,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"midi",         SettingKey::ONOFF,  PIX68K_OFF(midi),            0, 1,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"midi_out",     SettingKey::CHOICE, PIX68K_OFF(midiOut),         0, 0,   nullptr, settings_detail::kMidiOutChoices, settings_detail::kMidiOutValues,         true,  true},
    {"midi_sf2",     SettingKey::PATH,   PIX68K_OFF(midiSf2),         0, 0,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"midi_voices",  SettingKey::INT,    PIX68K_OFF(midiVoices),      4, 256, nullptr, nullptr, nullptr,                                                          true,  true},
    {"midi_mt32_ctrl", SettingKey::PATH, PIX68K_OFF(midiMt32Ctrl),    0, 0,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"midi_mt32_pcm", SettingKey::PATH,  PIX68K_OFF(midiMt32Pcm),     0, 0,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"midi_sc55",    SettingKey::PATH,   PIX68K_OFF(midiSc55),        0, 0,   nullptr, nullptr, nullptr,                                                          true,  true},
    {"midi_uart",    SettingKey::INT,    PIX68K_OFF(midiUart),        0, 10,  settings_detail::kMidiUartValues, nullptr, nullptr,                                 true,  false},
    {"midi_uart_family", SettingKey::CHOICE, PIX68K_OFF(midiUartFamily), 0, 0, nullptr, settings_detail::kMidiUartFamilyChoices, settings_detail::kMidiUartFamilyValues, true, true},
    {"write_back",   SettingKey::ONOFF,  PIX68K_OFF(writeBack),       0, 1,   nullptr, nullptr, nullptr,                                                          true,  false},
    {"write_protect", SettingKey::PATH,  PIX68K_OFF(writeProtect),    0, 0,   nullptr, nullptr, nullptr,                                                          true,  false},
    {"net",          SettingKey::ONOFF,  PIX68K_OFF(net),             0, 1,   nullptr, nullptr, nullptr,                                                          true,  false},
    {"net_ip",       SettingKey::PATH,   PIX68K_OFF(netIp),           0, 0,   nullptr, nullptr, nullptr,                                                          true,  false},
    {"net_gw",       SettingKey::PATH,   PIX68K_OFF(netGw),           0, 0,   nullptr, nullptr, nullptr,                                                          true,  false},
    {"net_dns",      SettingKey::PATH,   PIX68K_OFF(netDns),          0, 0,   nullptr, nullptr, nullptr,                                                          true,  false},
    {"net_name",     SettingKey::PATH,   PIX68K_OFF(netName),         0, 0,   nullptr, nullptr, nullptr,                                                          true,  false},
    {"net_allow",    SettingKey::PATH,   PIX68K_OFF(netAllow),        0, 0,   nullptr, nullptr, nullptr,                                                          true,  false},
    {"net_token",    SettingKey::PATH,   PIX68K_OFF(netToken),        0, 0,   nullptr, nullptr, nullptr,                                                          true,  false},
    {"syslog",       SettingKey::PATH,   PIX68K_OFF(syslog),          0, 0,   nullptr, nullptr, nullptr,                                                          true,  false},
    {"scale",        SettingKey::CHOICE, PIX68K_OFF(scale),           0, 0,   nullptr, settings_detail::kScaleChoices, settings_detail::kScaleValues,             false, false},
    {"scanline",     SettingKey::ONOFF,  PIX68K_OFF(scanline),        0, 1,   nullptr, nullptr, nullptr,                                                          false, false},
    {"osd",          SettingKey::CHOICE, PIX68K_OFF(osd),             0, 0,   nullptr, settings_detail::kOsdChoices,   settings_detail::kOsdValues,               false, true},
};
// clang-format on
#undef PIX68K_OFF
static constexpr int kSettingKeyCount = (int)(sizeof kSettingKeys / sizeof kSettingKeys[0]);

inline const SettingKey* findSettingKey(const char* key) {
    for (const SettingKey& k : kSettingKeys)
        if (strcmp(k.key, key) == 0) return &k;
    return nullptr;
}

inline int* settingInt(Settings& s, const SettingKey& k) { return (int*)((char*)&s + k.offset); }
inline const int* settingInt(const Settings& s, const SettingKey& k) { return (const int*)((const char*)&s + k.offset); }
inline char* settingPath(Settings& s, const SettingKey& k) { return (char*)&s + k.offset; }
inline const char* settingPath(const Settings& s, const SettingKey& k) { return (const char*)&s + k.offset; }

// 10 進の整数 (先頭に - があってもよい)。数字以外があれば false
inline bool parseDecimal(const char* s, long& out) {
    bool neg = *s == '-';
    if (neg) s++;
    if (*s == 0) return false;
    long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000000) return false;
        v = v * 10 + (*s - '0');
    }
    out = neg ? -v : v;
    return true;
}

// パスの最後の / から後 (/ がなければ全体)。Circle には strrchr がない
inline const char* baseName(const char* path) {
    const char* b = path;
    for (const char* p = path; *p; p++)
        if (*p == '/') b = p + 1;
    return b;
}

inline void copyString(char* out, size_t size, const char* s) {
    if (size == 0) return;
    size_t n = strlen(s);
    if (n >= size) n = size - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

inline void formatDecimal(long v, char* out, size_t size) {
    char tmp[24];
    int n = 0;
    unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v;
    do {
        tmp[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0) tmp[n++] = '-';
    char rev[24];
    for (int i = 0; i < n; i++) rev[i] = tmp[n - 1 - i];
    rev[n] = 0;
    copyString(out, size, rev);
}

// write_protect= の値 (「fd1,sc0」のように , か空白で区切ったディスクの名前) に name があるか (大文字と小文字は区別しない)
inline bool listHas(const char* list, const char* name) {
    const size_t n = strlen(name);
    for (const char* p = list; *p;) {
        while (*p == ',' || *p == ' ') p++;
        const char* e = p;
        while (*e && *e != ',' && *e != ' ') e++;
        if ((size_t)(e - p) == n) {
            size_t i = 0;
            for (; i < n; i++) {
                char a = p[i], b = name[i];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                if (a != b) break;
            }
            if (i == n) return true;
        }
        p = e;
    }
    return false;
}

// MAC アドレス (「02:68:6b:00:00:01」の形。区切りは : か -)。読めなければ false。
// 送り元に使えないアドレス (先頭のバイトの bit0 が 1 のマルチキャスト) も false
inline bool parseMac(const char* s, uint8_t out[6]) {
    for (int i = 0; i < 6; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            const char c = *s++;
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return false;
            v = v * 16 + d;
        }
        out[i] = (uint8_t)v;
        if (i < 5 && *s != ':' && *s != '-') return false;
        if (i < 5) s++;
    }
    return *s == 0 && !(out[0] & 1);
}

// neptune_link= の値。kind は種類、arg は : の後ろ (なければ空)
enum class NeptuneLink { DEFAULT, ETH, ECHO, LOOPBACK, PCAP, TAP, BAD };
inline NeptuneLink parseNeptuneLink(const char* s, const char*& arg) {
    static const struct {
        const char* name;
        NeptuneLink kind;
        bool hasArg;
    } kLinks[] = {{"eth", NeptuneLink::ETH, false},       {"echo", NeptuneLink::ECHO, false},
                  {"loopback", NeptuneLink::LOOPBACK, false}, {"pcap", NeptuneLink::PCAP, true},
                  {"tap", NeptuneLink::TAP, true}};
    arg = "";
    if (*s == 0) return NeptuneLink::DEFAULT;
    for (const auto& l : kLinks) {
        const size_t n = strlen(l.name);
        if (strncmp(s, l.name, n) != 0) continue;
        if (s[n] == 0 && !l.hasArg) return l.kind;
        if (s[n] == ':' && l.hasArg && s[n + 1] != 0) {
            arg = s + n + 1;
            return l.kind;
        }
    }
    return NeptuneLink::BAD;
}

// rs232c= の値 (RS-232C の相手)
//   空、off       つながない
//   tcp:PORT      TCP の PORT で待ち受け、つないできた 1 つの相手とやり取りする (ホスト版は 127.0.0.1、ベアメタル版は
//                 net=on の IP アドレス)
//   tcp:ADDR:PORT ホスト版で、待ち受けるアドレスを ADDR にする (0.0.0.0 ならすべて。ベアメタル版は ADDR を見ない)
// addr には ADDR (なければ空) を、addrSize バイトまで入れる
enum class Rs232cLink { OFF, TCP, BAD };
inline Rs232cLink parseRs232c(const char* s, unsigned& port, char* addr, size_t addrSize) {
    port = 0;
    if (addrSize) addr[0] = 0;
    if (*s == 0 || strcmp(s, "off") == 0) return Rs232cLink::OFF;
    if (strncmp(s, "tcp:", 4) != 0) return Rs232cLink::BAD;
    s += 4;
    const char* colon = nullptr; // 最後の : (Circle には strrchr() がない)
    for (const char* p = s; *p; p++)
        if (*p == ':') colon = p;
    const char* num = s;
    if (colon) {
        const size_t n = (size_t)(colon - s);
        if (n == 0 || n + 1 > addrSize) return Rs232cLink::BAD;
        memcpy(addr, s, n);
        addr[n] = 0;
        num = colon + 1;
    }
    if (*num == 0) return Rs232cLink::BAD;
    unsigned v = 0;
    for (; *num; num++) {
        if (*num < '0' || *num > '9') return Rs232cLink::BAD;
        v = v * 10 + (unsigned)(*num - '0');
        if (v > 65535) return Rs232cLink::BAD;
    }
    if (v == 0) return Rs232cLink::BAD;
    port = v;
    return Rs232cLink::TCP;
}

// 値を読んで Settings に入れる
enum class SettingResult { OK, UNKNOWN_KEY, BAD_VALUE };
inline SettingResult applySetting(Settings& s, const char* key, const char* value) {
    const SettingKey* k = findSettingKey(key);
    if (k == nullptr) return SettingResult::UNKNOWN_KEY;
    switch (k->kind) {
    case SettingKey::PATH: {
        char* p = settingPath(s, *k);
        size_t n = strlen(value);
        if (n >= Settings::PATH_SIZE) return SettingResult::BAD_VALUE;
        memcpy(p, value, n + 1);
        return SettingResult::OK;
    }
    case SettingKey::INT: {
        long v;
        if (!parseDecimal(value, v) || v < k->lo || v > k->hi) return SettingResult::BAD_VALUE;
        if (k->values) {
            bool ok = false;
            for (const int* p = k->values; *p >= 0; p++) ok = ok || *p == v;
            if (!ok) return SettingResult::BAD_VALUE;
        }
        *settingInt(s, *k) = (int)v;
        return SettingResult::OK;
    }
    case SettingKey::ONOFF:
        if (strcmp(value, "on") == 0) *settingInt(s, *k) = 1;
        else if (strcmp(value, "off") == 0) *settingInt(s, *k) = 0;
        else return SettingResult::BAD_VALUE;
        return SettingResult::OK;
    case SettingKey::CHOICE:
        for (int i = 0; k->choices[i]; i++)
            if (strcmp(k->choices[i], value) == 0) {
                *settingInt(s, *k) = k->choiceValues[i];
                return SettingResult::OK;
            }
        return SettingResult::BAD_VALUE;
    }
    return SettingResult::BAD_VALUE;
}

// Settings の値を pix68k.cfg に書く形にする。書く値がなければ (local_mem が -1 など) false
inline bool formatSetting(const Settings& s, const SettingKey& k, char* out, size_t size) {
    switch (k.kind) {
    case SettingKey::PATH:
        copyString(out, size, settingPath(s, k));
        return true;
    case SettingKey::INT: {
        int v = *settingInt(s, k);
        if (v < k.lo || v > k.hi) return false;
        formatDecimal(v, out, size);
        return true;
    }
    case SettingKey::ONOFF:
        copyString(out, size, *settingInt(s, k) ? "on" : "off");
        return true;
    case SettingKey::CHOICE: {
        int v = *settingInt(s, k);
        for (int i = 0; k.choices[i]; i++)
            if (k.choiceValues[i] == v) {
                copyString(out, size, k.choices[i]);
                return true;
            }
        return false;
    }
    }
    return false;
}

// 2 つの Settings で key の値が同じか
inline bool sameSetting(const Settings& a, const Settings& b, const SettingKey& k) {
    if (k.kind == SettingKey::PATH) return strcmp(settingPath(a, k), settingPath(b, k)) == 0;
    return *settingInt(a, k) == *settingInt(b, k);
}

} // namespace platform
