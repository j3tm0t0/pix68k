// 画面の隅に小さく出す数字 (pix68k.cfg の osd=on / fps)。直近 1 秒の「合成したフレームの速さ / 表示したフレームの速さ
// (fps、0.1 まで) / 置き換わった数」、X68000 の速さ (%)、温度 (℃、0.1 度まで) を「55.5/55.5/0 100% 52.3C」のように出す。
// ファームウェアが ARM のクロックを下げたか電圧が足りないときは「THR」を足す。
// 縦に並べるとき (右の余白) は、いちばん上に MPU とクロック (「68030 25MHz」)、ヒープの使った量 (「HEAP 52M」)、
// ファンの回転数 (「FAN 2200rpm」。Pi 5 でファンを動かしているとき) も出す (横 1 行には出さない。幅を取りすぎるため)。
// ネットワークの IP アドレス (net=on でアドレスが決まっているとき) も出す。縦に並べるときは最後の空でない行のすぐ下に
// 1 行で、横 1 行のときはいちばん右に出す。
// 最高速モード (max_speed=on) のときは、MPU の行のクロックを実効の値 (クロック × Machine::cpuSpeed() / 100) にして
// 「~」を付ける (「68060 ~210M」。osdEffMHzShown() で丸める)。
// ベアメタル版の CKernel::ShowFrame() (baremetal/machine_run.cpp) とホスト版 (host/main.cpp) が使う。
//
// 置く場所 (chooseOsd()): 画面の右上。文字はいつも 1 倍 (8x16)。
//   1. 画面の右上に 1 項目ずつ縦に並べたもの (vertical。1 行 OSD_V_COLS 文字。行数は最後の空でない行まで
//      (osdRows()、OSD_V_ROWS まで)) が X68000 の画面に重ならなければ、そこに出す。ふつうは右の余白
//      (16:9 のモニタの左右の黒いところ) が 128 ドットあるとき。右の余白が狭くても、上の余白が 4 + 行数 x 16 ライン
//      以上あれば重ならないので縦に並べる (行数は 4 から 9 で変わるので、上の余白が 68 から 147 ラインのときは、
//      THROTTLED や IP アドレスが出たり消えたりすると、縦と 2. の横 1 行が入れ替わる。前の場所は OsdState が消す)
//   2. 重なるなら横 1 行 (placeOsd())。X68000 の画面に重ならなければ (上の余白が 20 ライン以上) そこに出す
//   3. どちらも重なるときは、縦に並べたものを右上に置き、X68000 の画面に重ねる (overlay。osd=on でも osd=fps でも)
//      (IP アドレスをいつも出すため。2026-11 に持ち主が決めた)。フレームバッファに縦に並べたものが入らなければ、
//      osd=fps なら横 1 行を重ねる
// 重ねるときは、X68000 の画面をフレームバッファに写すときに、
// 重なる部分だけ数字の絵を書く (platform/frame_scale.h の FrameOverlay。写したあとに上から描くとちらつくため)。
// 数字は packOsd() で 64 ビットの 3 つの値 (OsdValue の a、b、c) にまとめる。前に描いた値と比べれば描き直しが要るかがわかる。
// 数字を作るコア (ベアメタル版はコア 0) と描くコアが違うときは、版の番号をつけて受け渡す (seqlock。CKernel::PublishOsd())。
// 文字は platform/menu_font.h の 8x16 (白、背景は不透明の黒)。決まった文字数に右寄せするので、桁が減っても前の文字は
// 残らない。横 1 行の文字数は出す項目で決める (osdChars()。「THR」は出すときだけ幅を取る)。
// 縦に並べるときの 1 行は OSD_V_COLS (15) 文字 (「255.255.255.255」が入る)。IP アドレスのほかの行は、詰められるいちばん
// 大きい値で OSD_V_ITEM (11) 文字に入るようにする (縮めた形を選ぶ基準。15 文字に右寄せする)。
// ベアメタルでも使えるよう、例外、RTTI、STL、stdio は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "platform/frame_scale.h"
#include "platform/menu_font.h"

namespace platform {

static constexpr int OSD_CHARS = 27;   // 「999/999/999 999% 127.9C THR」が入る文字数 (ホスト版の横 1 行。IP アドレスは出さない)
static constexpr int OSD_CHARS_BASE = 23; // 「999/999/999 999% 127.9C」が入る文字数 (横 1 行。THR を出さないとき)
static constexpr int OSD_CHARS_MAX = OSD_CHARS + 16; // 横 1 行の上限 (「 255.255.255.255」を足したとき)
static constexpr int OSD_V_COLS = 15;  // 縦に並べるときの 1 行の文字数 (「255.255.255.255」)
static constexpr int OSD_V_ITEM = 11;  // IP アドレスのほかの行の文字数の上限 (上の注釈を参照)
static constexpr int OSD_V_ITEMS = 8;  // IP アドレスのほかの行の数 (Pi、MPU、フレーム、速さ、ヒープ、温度、ファン、クロックを下げたか)
static constexpr int OSD_V_ROWS = OSD_V_ITEMS + 1; // 縦に並べるときの行数の上限 (IP アドレスの行を足す)
static constexpr int OSD_TEXT_MAX = OSD_V_COLS * OSD_V_ROWS; // formatOsdRows() が作る文字数の上限 (横 1 行より多い)
// 絵の画素数の上限 (縦に並べて、いちばん多い行数のとき。横 1 行は OSD_CHARS_MAX * 8 * 16 で、これより小さい)
static constexpr int OSD_IMAGE_PIXELS = OSD_V_COLS * 8 * OSD_V_ROWS * 16;
static constexpr int OSD_MODE_OFF = 0, OSD_MODE_FPS = 1, OSD_MODE_ON = 2; // Settings::osd
static constexpr int OSD_PAD_X = 8;    // 画面の右端からの間 (画素)
static constexpr int OSD_PAD_Y = 4;    // 画面の上端からの間 (画素)
static constexpr uint32_t OSD_FG = 0xffffffffu, OSD_BG = 0xff000000u;
// 詰められるいちばん大きい値 (これより大きい値はこの値にする)
static constexpr unsigned OSD_FPS_DECI_MAX = 9999; // fps (0.1 単位。999.9)
static constexpr unsigned OSD_COUNT_MAX = 999;     // 置き換わった数、速さ (%)
static constexpr unsigned OSD_TEMP_DECI_MAX = 1279; // 温度 (0.1 度単位。127.9 度)
static constexpr unsigned OSD_FAN_MAX = 6;          // ファンの段階
static constexpr unsigned OSD_RPM_MAX = 20000;      // 回転数 (10rpm 単位で持つ。10000 以上は「FAN 12.3k」と縮めて出す)
static constexpr unsigned OSD_HEAP_MAX = 999;       // ヒープ (MB)
static constexpr unsigned OSD_MHZ_MAX = 255;        // MPU のクロック (MHz)
static constexpr unsigned OSD_ARM_MHZ_MAX = 5110;   // ARM のクロック (MHz。10MHz 単位で持つ)
static constexpr unsigned OSD_EFF_MHZ_MAX = 9900;   // 最高速モードの実効のクロック (MHz。「~9.9G」まで)

// Raspberry Pi のモデル (OsdStats::pi)。ベアメタル版が Circle の CMachineInfo のモデルから決める (osdPiName() で名前にする)
enum OsdPi {
    OSD_PI_NONE = 0, // 出さない
    OSD_PI_3B, OSD_PI_3BPLUS, OSD_PI_3APLUS, OSD_PI_CM3, OSD_PI_CM3PLUS, OSD_PI_ZERO2W,
    OSD_PI_4B, OSD_PI_400, OSD_PI_CM4, OSD_PI_CM4S,
    OSD_PI_5, OSD_PI_500, OSD_PI_CM5, OSD_PI_CM5LITE,
    OSD_PI_OTHER, // 上のどれでもない (「Pi」と出す)
    OSD_PI_COUNT
};
// OSD に出すモデルの名前 (6 文字まで。クロックと合わせて 11 文字に入るように)
inline const char* osdPiName(int pi) {
    static const char* const names[OSD_PI_COUNT] = {"",     "RPi3", "RPi3+", "RPi3A+", "CM3",  "CM3+",  "Zero2", "RPi4",
                                                    "Pi400", "CM4", "CM4S",  "RPi5",   "Pi500", "CM5", "CM5L",  "Pi"};
    return pi > 0 && pi < OSD_PI_COUNT ? names[pi] : "";
}
// 出す ARM のクロック (MHz) に丸める。1000MHz 以上は 100MHz 単位 (「2.4GHz」と出す)、それより下は 10MHz 単位
// (測った値は少しずつ違うことがあるので、出す桁に丸めて、変わらなければ描き直さない)
inline unsigned osdArmMHzShown(unsigned mhz) {
    return mhz >= 950 ? (mhz + 50) / 100 * 100 : (mhz + 5) / 10 * 10;
}

// 最高速モードの実効のクロック (kHz、切り捨て)。hz は MPU のクロック、speed は Machine::cpuSpeed() (既定の何 %)。
// MHz に丸めるのは osdEffMHzShown() の 1 回だけにする (ここで MHz に丸めると、214.5MHz が 215 から 220 になるように
// 2 回丸めることになるため)
inline uint64_t osdEffKHz(uint64_t hz, int speed) {
    if (speed <= 0) return 0;
    return hz * (uint64_t)speed / 100000ull;
}

// OSD に出す実効のクロック (MHz) を決める。khz は osdEffKHz() の値。上 2 桁に丸め (100MHz 未満は 1MHz 単位)、前に出した値
// shown (0 ならまだ出していない) から、丸めの単位、shown の 5% (MHz に四捨五入)、2MHz のいちばん大きい幅以上変わった
// ときだけ変える (比べるのは丸める前の kHz)。速さは 0.25 秒ごとに少しずつ変わるので、1 秒ごとに描き直したり、丸めの
// 境目で行き来したりしないため。100MHz より下は単位が 1MHz と細かいので 5% で押さえ、40MHz より下は 5% も 1MHz 以下に
// なるので 2MHz にする (10MHz の 200% 前後で 20 と 21 を行き来しないように)。1MHz より下でも 1 にする (0 は「出さない」)
inline unsigned osdEffMHzShown(unsigned shown, uint64_t khz) {
    if (khz > OSD_EFF_MHZ_MAX * 1000ull) khz = OSD_EFF_MHZ_MAX * 1000ull;
    unsigned unit = 1;
    for (uint64_t v = khz / 1000; v >= 100; v /= 10) unit *= 10;
    const unsigned pct5 = (shown + 10) / 20; // shown の 5% (四捨五入)
    unsigned bandMHz = pct5 > unit ? pct5 : unit;
    if (bandMHz < 2) bandMHz = 2;
    const uint64_t band = (uint64_t)bandMHz * 1000;
    const uint64_t shownKHz = (uint64_t)shown * 1000;
    if (shown != 0 && khz < shownKHz + band && shownKHz < khz + band) return shown;
    const uint64_t r = (khz + unit * 500ull) / (unit * 1000ull) * unit;
    return r == 0 ? 1 : r > OSD_EFF_MHZ_MAX ? OSD_EFF_MHZ_MAX : (unsigned)r;
}

struct OsdStats {
    unsigned composedDeci = 0, shownDeci = 0; // 直近 1 秒に合成した / 写したフレームの速さ (0.1fps 単位。555 なら 55.5fps)
    unsigned replaced = 0;                    // 直近 1 秒に写す前に置き換わった数
    unsigned speedPct = 0;                    // X68000 の速さ (実時間の何 %)
    int tempDeciC = -1;                       // 温度 (0.1℃ 単位。523 なら 52.3℃)。負なら出さない (読めない環境)
    bool throttled = false;                   // ファームウェアが ARM のクロックを下げたか、電圧が足りない
    int fanLevel = -1;                        // ファンの段階 (0 止めている、から 4)。負ならファンの行を出さない (ファンを動かしていない)。
                                              // 段階そのものは出さない (ログの cpu: の行と /thermal に出る)
    int fanRpm = -1;                          // ファンの回転数 (fanLevel が負でないときだけ出す。負なら 0 にする)
    int heapMB = -1;                          // ヒープの使った量 (MB)。負なら出さない (縦に並べるときだけ出す)
    int mpu = 0;                              // MPU (68000、68030、68060)。0 なら出さない (縦に並べるときだけ出す)
    unsigned mpuMHz = 0;                      // MPU のクロック (MHz)
    unsigned effMHz = 0;                      // 最高速モードの実効のクロック (MHz。osdEffMHzShown() の値)。0 なら mpuMHz を出す
    int pi = OSD_PI_NONE;                     // Raspberry Pi のモデル (OsdPi)。OSD_PI_NONE なら出さない (縦に並べるときだけ出す)
    unsigned armMHz = 0;                      // ARM のクロック (MHz。osdArmMHzShown() で丸めた値。0 ならモデルだけ出す)
    uint32_t ipv4 = 0;                        // IP アドレス (192.168.1.27 なら 0xc0a8011b)。0 なら出さない (縦でも横 1 行でも出す)
};

// 詰めた数字。a が 0 なら「まだ数字がない」(a の bit 63 が 1 なら数字がある)。下位からの並び:
//   a: 0-13 合成した fps、14-27 写した fps (0.1 単位)、28-37 置き換わった数、38-47 速さ (%)、
//      48-58 温度 (0.1 度単位に 1 を足した値。0 なら出さない)、59 THR、63 数字がある
//   b: 0-2 ファンの段階に 1 を足した値 (0 なら出さない)、3-13 回転数 (10rpm 単位)、14-23 ヒープの MB に 1 を足した値
//      (0 なら出さない)、24-25 MPU (0 出さない、1 68000、2 68030、3 68060)、26-33 MPU のクロック (MHz)、
//      34-38 Pi のモデル (OsdPi)、39-47 ARM のクロック (10MHz 単位)、48-61 最高速モードの実効のクロック (MHz。0 なら出さない)
//   c: 0-31 IP アドレス (0 なら出さない。上位 32 ビットは使わない)
struct OsdValue {
    uint64_t a = 0, b = 0, c = 0;
    bool valid() const { return a != 0; }
    // IP アドレスを出す
    bool hasIp() const { return valid() && (uint32_t)c != 0; }
    bool operator==(const OsdValue& o) const { return a == o.a && b == o.b && c == o.c; }
    bool operator!=(const OsdValue& o) const { return !(*this == o); }
};

namespace osd_detail {
inline uint64_t capTo(unsigned v, unsigned max) { return v > max ? max : v; }
} // namespace osd_detail

inline OsdValue packOsd(const OsdStats& s) {
    using osd_detail::capTo;
    OsdValue v;
    v.a = capTo(s.composedDeci, OSD_FPS_DECI_MAX) | capTo(s.shownDeci, OSD_FPS_DECI_MAX) << 14 |
          capTo(s.replaced, OSD_COUNT_MAX) << 28 | capTo(s.speedPct, OSD_COUNT_MAX) << 38;
    if (s.tempDeciC >= 0) v.a |= (capTo((unsigned)s.tempDeciC, OSD_TEMP_DECI_MAX) + 1) << 48;
    if (s.throttled) v.a |= 1ull << 59;
    v.a |= 1ull << 63;
    if (s.fanLevel >= 0) {
        v.b |= capTo((unsigned)s.fanLevel, OSD_FAN_MAX) + 1;
        if (s.fanRpm >= 0) v.b |= ((capTo((unsigned)s.fanRpm, OSD_RPM_MAX) + 5) / 10) << 3;
    }
    if (s.heapMB >= 0) v.b |= (capTo((unsigned)s.heapMB, OSD_HEAP_MAX) + 1) << 14;
    const uint64_t mpu = s.mpu == 68000 ? 1 : s.mpu == 68030 ? 2 : s.mpu == 68060 ? 3 : 0;
    if (mpu != 0) v.b |= mpu << 24 | capTo(s.mpuMHz, OSD_MHZ_MAX) << 26 | capTo(s.effMHz, OSD_EFF_MHZ_MAX) << 48;
    if (s.pi > OSD_PI_NONE && s.pi < OSD_PI_COUNT)
        v.b |= (uint64_t)s.pi << 34 | ((capTo(s.armMHz, OSD_ARM_MHZ_MAX) + 5) / 10) << 39;
    v.c = s.ipv4;
    return v;
}

// 詰めた値を数字に戻す。packOsd() を通していない値 (ビットがすべて 1 など) でも、詰められるいちばん大きい値までにする
// (どの行も決まった文字数に入るように)
inline OsdStats unpackOsd(const OsdValue& v) {
    using osd_detail::capTo;
    OsdStats s;
    s.composedDeci = (unsigned)capTo((unsigned)(v.a & 0x3fff), OSD_FPS_DECI_MAX);
    s.shownDeci = (unsigned)capTo((unsigned)(v.a >> 14 & 0x3fff), OSD_FPS_DECI_MAX);
    s.replaced = (unsigned)capTo((unsigned)(v.a >> 28 & 1023), OSD_COUNT_MAX);
    s.speedPct = (unsigned)capTo((unsigned)(v.a >> 38 & 1023), OSD_COUNT_MAX);
    const unsigned temp = (unsigned)(v.a >> 48 & 2047);
    s.tempDeciC = temp != 0 ? (int)capTo(temp - 1, OSD_TEMP_DECI_MAX) : -1;
    s.throttled = (v.a >> 59 & 1) != 0;
    const unsigned fan = (unsigned)(v.b & 7);
    s.fanLevel = fan != 0 ? (int)capTo(fan - 1, OSD_FAN_MAX) : -1;
    s.fanRpm = fan != 0 ? (int)capTo((unsigned)(v.b >> 3 & 2047) * 10, OSD_RPM_MAX) : -1;
    const unsigned heap = (unsigned)(v.b >> 14 & 1023);
    s.heapMB = heap != 0 ? (int)capTo(heap - 1, OSD_HEAP_MAX) : -1;
    const unsigned mpu = (unsigned)(v.b >> 24 & 3);
    s.mpu = mpu == 1 ? 68000 : mpu == 2 ? 68030 : mpu == 3 ? 68060 : 0;
    s.mpuMHz = mpu != 0 ? (unsigned)(v.b >> 26 & 255) : 0;
    s.effMHz = mpu != 0 ? (unsigned)capTo((unsigned)(v.b >> 48 & 0x3fff), OSD_EFF_MHZ_MAX) : 0;
    const unsigned pi = (unsigned)(v.b >> 34 & 31);
    s.pi = pi < OSD_PI_COUNT ? (int)pi : OSD_PI_OTHER;
    s.armMHz = s.pi != OSD_PI_NONE ? (unsigned)(v.b >> 39 & 511) * 10 : 0;
    s.ipv4 = (uint32_t)v.c;
    return s;
}

// fps を 0.1 単位で求める (frames 枚を micros マイクロ秒で。四捨五入)。micros が 0 なら 0
inline unsigned osdFpsDeci(uint64_t frames, uint64_t micros) {
    return micros != 0 ? (unsigned)((frames * 10000000ull + micros / 2) / micros) : 0;
}

// 出す回転数を決める。前に出した値 shown (負ならまだ出していない) から 100rpm 以上変わったときだけ、100rpm に丸めた値に
// 変える (毎秒少しずつ変わる回転数で、1 秒ごとに描き直したり、丸めの境目で行き来したりしないため)
inline int osdRpmShown(int shown, unsigned rpm) {
    const int r = (int)rpm;
    if (shown >= 0 && r - shown < 100 && shown - r < 100) return shown;
    return (r + 50) / 100 * 100;
}

namespace osd_detail {
// 文字を並べていく小さな入れ物
struct Text {
    char buf[OSD_CHARS_MAX + 8];
    int n = 0;
    void ch(char c) { buf[n++] = c; }
    void str(const char* s) {
        while (*s) buf[n++] = *s++;
    }
    void num(unsigned v) {
        char d[10];
        int k = 0;
        do {
            d[k++] = (char)('0' + v % 10);
            v /= 10;
        } while (v != 0);
        while (k > 0) buf[n++] = d[--k];
    }
    // 0.1 単位の値を「52.3」の形で足す
    void deci(unsigned v) {
        num(v / 10);
        ch('.');
        ch((char)('0' + v % 10));
    }
    // out の width 文字に右寄せして写す (終わりの 0 は付けない)
    void right(char* out, int width) const {
        const int pad = width - n;
        for (int i = 0; i < width; i++) out[i] = i < pad ? ' ' : buf[i - pad];
    }
};
// 文字数を数えるだけのとき (どちらの形にするかを決める)
inline int digits(unsigned v) {
    int k = 1;
    while (v >= 10) {
        v /= 10;
        k++;
    }
    return k;
}
// 「55.5/55.5/0」(fps は 0.1 まで)。OSD_V_ITEM 文字に入らなければ「56/56/12」(fps を四捨五入した整数) にする
inline void frames(Text& t, const OsdStats& s) {
    const int deciLen = digits(s.composedDeci / 10) + 2 + 1 + digits(s.shownDeci / 10) + 2 + 1 + digits(s.replaced);
    if (deciLen <= OSD_V_ITEM) {
        t.deci(s.composedDeci);
        t.ch('/');
        t.deci(s.shownDeci);
    } else {
        // 999.9 を四捨五入すると 1000 (4 桁) になるので、999 までにする
        t.num((unsigned)osd_detail::capTo((s.composedDeci + 5) / 10, 999));
        t.ch('/');
        t.num((unsigned)osd_detail::capTo((s.shownDeci + 5) / 10, 999));
    }
    t.ch('/');
    t.num(s.replaced);
}
// ARM のクロックを「2.4GHz」「700MHz」(long) か「2.4G」「700M」の形で足す
inline void armClock(Text& t, unsigned mhz, bool longUnit) {
    if (mhz >= 1000) {
        t.deci(mhz / 100);
        t.str(longUnit ? "GHz" : "G");
    } else {
        t.num(mhz);
        t.str(longUnit ? "MHz" : "M");
    }
}
// 「RPi5 2.4GHz」。OSD_V_ITEM 文字に入るよう、次の順に試す: 「名前 2.4GHz」、「名前2.4GHz」(名前が数字で終わらないとき。
// 「RPi3+1.4GHz」)、「名前 2.4G」、「名前2.4G」(同じ)。クロックがなければ名前だけ
inline void piRow(Text& t, const OsdStats& s) {
    const char* name = osdPiName(s.pi);
    int len = 0;
    while (name[len]) len++;
    t.str(name);
    if (s.armMHz == 0) return;
    const bool digitEnd = len > 0 && name[len - 1] >= '0' && name[len - 1] <= '9';
    Text c;
    armClock(c, s.armMHz, true);
    const int longLen = c.n;
    for (int k = 0; k < 4; k++) {
        const bool longUnit = k < 2, space = (k % 2) == 0;
        if (!space && digitEnd) continue;
        const int clockLen = longUnit ? longLen : longLen - 2;
        if (len + (space ? 1 : 0) + clockLen > OSD_V_ITEM && k < 3) continue;
        if (space) t.ch(' ');
        armClock(t, s.armMHz, longUnit);
        return;
    }
}
// 「192.168.100.200」(15 文字まで)
inline void ipRow(Text& t, uint32_t ip) {
    for (int i = 0; i < 4; i++) {
        if (i != 0) t.ch('.');
        t.num(ip >> (24 - 8 * i) & 255);
    }
}
} // namespace osd_detail

// packed を横 1 行で出すときの文字数。「THR」(4 文字) は出すときだけ足す
// (ふだん THR が出ないときに、重ねるときに X68000 の画面を隠す範囲を広げないため)
inline int osdChars(const OsdValue& packed) {
    const OsdStats s = unpackOsd(packed);
    int n = OSD_CHARS_BASE + (s.throttled ? 4 : 0);
    if (packed.hasIp()) {
        osd_detail::Text t;
        osd_detail::ipRow(t, s.ipv4);
        n += 1 + t.n; // 「 192.168.1.27」
    }
    return n;
}

// packed を縦に並べるときの行数 (formatOsdRows() が作る、最後の空でない行まで)。数字がなければ OSD_V_ITEMS
inline int osdRows(const OsdValue& packed) {
    if (!packed.valid()) return OSD_V_ITEMS;
    const OsdStats s = unpackOsd(packed);
    int n = s.throttled ? 8 : s.fanLevel >= 0 ? 7 : s.tempDeciC >= 0 ? 6 : s.heapMB >= 0 ? 5 : 4; // 速さの行 (4 行目) はいつもある
    return n + (packed.hasIp() ? 1 : 0);
}

// packed の数字を width 文字 (OSD_CHARS_MAX まで) に右寄せして out に入れる (out は width + 1 バイト)。数字がなければ空白だけ
inline void formatOsd(char* out, const OsdValue& packed, int width = OSD_CHARS) {
    osd_detail::Text t;
    if (packed.valid()) {
        const OsdStats s = unpackOsd(packed);
        osd_detail::frames(t, s);
        t.ch(' ');
        t.num(s.speedPct);
        t.ch('%');
        if (s.tempDeciC >= 0) {
            t.ch(' ');
            t.deci((unsigned)s.tempDeciC);
            t.ch('C');
        }
        if (s.throttled) t.str(" THR");
        if (packed.hasIp()) {
            t.ch(' ');
            osd_detail::ipRow(t, s.ipv4);
        }
    }
    if (width > OSD_CHARS_MAX) width = OSD_CHARS_MAX;
    t.right(out, width);
    out[width] = 0;
}

// 縦に並べるときの文字。rows 行 (OSD_V_ROWS まで) を、1 行 OSD_V_COLS 文字に右寄せして続けて out に入れる
// (out は OSD_TEXT_MAX + 1 バイト。OSD_V_COLS x rows 文字のあとに 0 を置く)。rows は osdRows() の値 (場所の高さ)。
// 上から、Pi のモデルと ARM のクロック、MPU とクロック、フレームの速さ、X68000 の速さ、ヒープの使った量、温度、
// ファンの回転数、クロックを下げたか (温度とファンを隣に置く)。出さない項目の行は空白。
// IP アドレスは決まった行に置かず、最後の空でない行のすぐ下に出す (ファンや THROTTLED がないときに、空いた行を挟まない)。
// 数字がなければ空白だけ
inline void formatOsdRows(char* out, const OsdValue& packed, int rows_ = OSD_V_ITEMS) {
    if (rows_ > OSD_V_ROWS) rows_ = OSD_V_ROWS;
    if (rows_ < 0) rows_ = 0;
    osd_detail::Text rows[OSD_V_ROWS];
    if (packed.valid()) {
        const OsdStats s = unpackOsd(packed);
        if (s.pi != OSD_PI_NONE) osd_detail::piRow(rows[0], s);
        if (s.mpu != 0 && s.effMHz != 0) {
            // 最高速モード: 「68000 ~9MHz」「68000 ~20M」「68060 ~210M」「68060 ~1.2G」(どれも 11 文字まで)
            rows[1].num((unsigned)s.mpu);
            rows[1].str(" ~");
            if (s.effMHz >= 1000) {
                rows[1].deci((s.effMHz + 50) / 100);
                rows[1].ch('G');
            } else {
                rows[1].num(s.effMHz);
                rows[1].str(osd_detail::digits(s.effMHz) <= 1 ? "MHz" : "M");
            }
        } else if (s.mpu != 0) {
            // 「68030 25MHz」。11 文字を超える (3 桁の MHz) ときは「68060 100M」
            rows[1].num((unsigned)s.mpu);
            rows[1].ch(' ');
            rows[1].num(s.mpuMHz);
            rows[1].str(osd_detail::digits(s.mpuMHz) <= 2 ? "MHz" : "M");
        }
        osd_detail::frames(rows[2], s);
        rows[3].num(s.speedPct);
        rows[3].ch('%');
        if (s.heapMB >= 0) {
            rows[4].str("HEAP ");
            rows[4].num((unsigned)s.heapMB);
            rows[4].ch('M');
        }
        if (s.tempDeciC >= 0) {
            rows[5].deci((unsigned)s.tempDeciC);
            rows[5].ch('C');
        }
        if (s.fanLevel >= 0) {
            // 「FAN 2200rpm」。10000rpm 以上は「FAN 12.3k」(0.1k に四捨五入) にして 11 文字に入れる
            const unsigned rpm = (unsigned)(s.fanRpm >= 0 ? s.fanRpm : 0);
            rows[6].str("FAN ");
            if (rpm < 10000) {
                rows[6].num(rpm);
                rows[6].str("rpm");
            } else {
                rows[6].deci((rpm + 50) / 100);
                rows[6].ch('k');
            }
        }
        if (s.throttled) rows[7].str("THROTTLED");
        if (packed.hasIp()) {
            int k = OSD_V_ITEMS;
            while (k > 0 && rows[k - 1].n == 0) k--;
            osd_detail::ipRow(rows[k], s.ipv4);
        }
    }
    for (int i = 0; i < rows_; i++) rows[i].right(out + i * OSD_V_COLS, OSD_V_COLS);
    out[rows_ * OSD_V_COLS] = 0;
}

struct OsdRect {
    int x = 0, y = 0, w = 0, h = 0; // フレームバッファの中の位置と大きさ。w が 0 なら出さない
    int scale = 1;                  // 文字の倍率
    bool overlay = false;           // X68000 の画面に重なる (写すたびに描く)
    bool vertical = false;          // 1 項目ずつ縦に並べる (OSD_V_COLS 文字 x h / 16 行)。false なら横 1 行

    bool operator==(const OsdRect& o) const {
        return x == o.x && y == o.y && w == o.w && h == o.h && scale == o.scale && overlay == o.overlay &&
               vertical == o.vertical;
    }
    bool operator!=(const OsdRect& o) const { return !(*this == o); }
};

// fbW x fbH の画面の右上に、横 1 行 (chars 文字。OSD_CHARS_MAX まで) で置く。X68000 の画面 (picX, picY から picW x picH) に
// 重なれば overlay。左端は 4 画素にそろえる (フレームバッファに 16 バイトずつ書けるように)。入らなければ w = 0
inline OsdRect placeOsd(int fbW, int fbH, int picX, int picY, int picW, int picH, int scale = 1, int chars = OSD_CHARS) {
    OsdRect r;
    r.scale = scale < 1 ? 1 : scale;
    if (chars < 1 || chars > OSD_CHARS_MAX) chars = OSD_CHARS_MAX;
    r.w = chars * 8 * r.scale;
    r.h = 16 * r.scale;
    r.x = (fbW - r.w - OSD_PAD_X * r.scale) & ~3;
    r.y = OSD_PAD_Y * r.scale;
    if (r.x < 0 || r.y + r.h > fbH) return OsdRect();
    r.overlay = picW > 0 && picH > 0 && r.x < picX + picW && picX < r.x + r.w && r.y < picY + picH && picY < r.y + r.h;
    return r;
}

// 画面の右上に、縦に並べて置く (OSD_V_COLS 文字 x rows 行、1 倍)。X68000 の画面 (picX, picY から picW x picH) に重なれば
// overlay。左端は 4 画素にそろえる。X68000 の画面をまだ写していない (picW が 0 以下) か、入らなければ w = 0
inline OsdRect placeOsdVertical(int fbW, int fbH, int picX, int picY, int picW, int picH, int rows = OSD_V_ITEMS) {
    OsdRect r;
    if (picW <= 0 || rows < 1) return r;
    if (rows > OSD_V_ROWS) rows = OSD_V_ROWS;
    r.w = OSD_V_COLS * 8;
    r.h = rows * 16;
    r.x = (fbW - r.w - OSD_PAD_X) & ~3;
    r.y = OSD_PAD_Y;
    if (r.x < 0 || r.y + r.h > fbH) return OsdRect();
    r.vertical = true;
    r.overlay = picH > 0 && r.x < picX + picW && picX < r.x + r.w && r.y < picY + picH && picY < r.y + r.h;
    return r;
}

// 置く場所を決める (このファイルの先頭の 1. から 3.)。mode は Settings::osd。chars は横 1 行にするときの文字数
// (osdChars())、rows は縦に並べるときの行数 (osdRows())
inline OsdRect chooseOsd(int mode, int fbW, int fbH, int picX, int picY, int picW, int picH, int chars = OSD_CHARS,
                         int rows = OSD_V_ITEMS) {
    if (mode == OSD_MODE_OFF) return OsdRect();
    const OsdRect v = placeOsdVertical(fbW, fbH, picX, picY, picW, picH, rows);
    if (v.w > 0 && !v.overlay) return v;
    const OsdRect h = placeOsd(fbW, fbH, picX, picY, picW, picH, 1, chars);
    if (h.w > 0 && !h.overlay) return h;
    if (v.w > 0) return v;
    if (h.overlay && mode != OSD_MODE_FPS) return OsdRect();
    return h;
}

// text (chars 文字) の y 行目 (0 から 16 * scale - 1) を line (chars * 8 * scale 画素) に作る
inline void renderOsdRow(uint32_t* line, const char* text, int y, int scale, int chars = OSD_CHARS) {
    const int gy = y / scale;
    int x = 0;
    for (int i = 0; i < chars; i++) {
        unsigned c = (unsigned char)text[i];
        if (c < (unsigned)MENU_FONT_FIRST || c > (unsigned)MENU_FONT_LAST) c = ' ';
        const uint8_t bits = kMenuFont[c - MENU_FONT_FIRST][gy];
        for (int b = 0; b < 8; b++) {
            const uint32_t px = (bits & (0x80 >> b)) ? OSD_FG : OSD_BG;
            for (int k = 0; k < scale; k++) line[x++] = px;
        }
    }
}

// renderOsdImage() で作った絵 (r.w x r.h 画素) をフレームバッファの r に書く
template <class Store = DeviceStore>
inline void drawOsdImage(uint8_t* fb, size_t fbPitch, const OsdRect& r, const uint32_t* image) {
    if (r.w <= 0) return;
    for (int y = 0; y < r.h; y++)
        frame_scale_detail::storeRow<Store>((uint32_t*)(fb + (size_t)(r.y + y) * fbPitch) + r.x, image + (size_t)y * r.w,
                                            r.w);
}

// r を color で塗る (数字を消す)
template <class Store = DeviceStore>
inline void eraseOsd(uint8_t* fb, size_t fbPitch, const OsdRect& r, uint32_t color) {
    if (r.w <= 0) return;
    for (int y = 0; y < r.h; y++)
        frame_scale_detail::fillRow<Store>((uint32_t*)(fb + (size_t)(r.y + y) * fbPitch) + r.x, color, r.w);
}

// text (横 1 行。r.w / (8 * r.scale) 文字) を w x h 画素の絵 (1 行 w 画素、ふつうのメモリ) にする。X68000 の画面に
// 重ねるときに presentScaled() (platform/frame_scale.h) に FrameOverlay として渡す
inline void renderOsdImage(uint32_t* image, const OsdRect& r, const char* text) {
    const int chars = r.w / (8 * r.scale);
    for (int y = 0; y < r.h; y++) renderOsdRow(image + (size_t)y * r.w, text, y, r.scale, chars);
}

// packed の数字を、r の並べ方 (縦か横 1 行) の絵 (r.w x r.h 画素。OSD_IMAGE_PIXELS 画素まで) にする
inline void renderOsd(uint32_t* image, const OsdRect& r, const OsdValue& packed) {
    char text[OSD_TEXT_MAX + 1];
    if (!r.vertical) {
        formatOsd(text, packed, r.w / (8 * r.scale));
        renderOsdImage(image, r, text);
        return;
    }
    formatOsdRows(text, packed, r.h / (16 * r.scale));
    const int rowH = 16 * r.scale;
    for (int y = 0; y < r.h; y++)
        renderOsdRow(image + (size_t)y * r.w, text + (y / rowH) * OSD_V_COLS, y % rowH, r.scale, OSD_V_COLS);
}

// 描く側が持つ「いま画面に出ているもの」。update() に出したい値と場所を渡すと、消す場所と描くかを決める。
// 値か場所が変わったときだけ描く。X68000 の画面に重ねるとき (rect.overlay) は、写すたびに presentScaled() に
// 重ねる絵を渡す (overlaid())。枠の外を塗り直したら invalidate() を呼ぶ
struct OsdState {
    struct Step {
        bool erase = false; // eraseRect を背景の色で塗る (前に描いたものを消す。X68000 の画面に重なる部分は、
        OsdRect eraseRect;  // そのあとに写す画面で元に戻る)
        bool draw = false;  // rect に描く
    };
    OsdRect rect;           // 描いた (描く) 場所
    OsdValue value;         // 描いた (描く) 数字
    bool shown = false;

    void invalidate() { shown = false; }
    // X68000 の画面に重ねて出している (写すときに重ねる絵を渡す)
    bool overlaid() const { return shown && rect.overlay; }
    // on: 出すか。packed: 数字 (valid() でなければまだない)。r: 出す場所 (placeOsd())
    Step update(bool on, const OsdValue& packed, const OsdRect& r) {
        Step st;
        const bool want = on && r.w > 0 && packed.valid();
        // 出さなくなったときと場所が変わったときは、前に描いたものを消す
        if (shown && (!want || r != rect)) {
            st.erase = true;
            st.eraseRect = rect;
        }
        if (!want) {
            shown = false;
            return st;
        }
        st.draw = !shown || r != rect || packed != value;
        rect = r;
        value = packed;
        shown = true;
        return st;
    }
};

} // namespace platform
