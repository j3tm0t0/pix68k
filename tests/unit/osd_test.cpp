// src/platform/osd.h のテスト: 数字の詰め方と文字列、置く場所 (余白か重ねるか)、フレームバッファに描く範囲と
// 書き方、描き直すときと消すとき
#include <limits.h>
#include <string.h>

#include <vector>

#include "platform/crt_effect.h"
#include "platform/osd.h"
#include "test_check.h"

using namespace platform;

namespace {

// フレームバッファへの書き込みを調べる Store (platform::DeviceStore と同じ形)。16 バイトにそろわない
// 16 / 64 バイトの書き込みと、書き込みの回数を数える
struct CheckingStore {
    typedef frame_scale_detail::V4 V4;
    static long writes, misaligned;
    static void check(const void* p) {
        if (((uintptr_t)p & 15) != 0) misaligned++;
    }
    static void store4(uint32_t* p, uint32_t v) {
        writes++;
        memcpy(p, &v, 4);
    }
    static void store16(uint32_t* p, V4 v) {
        writes++;
        check(p);
        memcpy(p, &v, 16);
    }
    static void store64(uint32_t* p, V4 a, V4 b, V4 c, V4 d) {
        writes++;
        check(p);
        memcpy(p, &a, 16);
        memcpy(p + 4, &b, 16);
        memcpy(p + 8, &c, 16);
        memcpy(p + 12, &d, 16);
    }
};
long CheckingStore::writes, CheckingStore::misaligned;

// formatOsdRows() の 8 行 (IP アドレスのない形) を作り、どの行も右の OSD_V_ITEM (11) 文字に入っていることを確かめて、
// 1 行 11 文字に詰めて out に入れる (out は OSD_V_ITEM * OSD_V_ITEMS + 1 バイト)
void fmtRows11(char* out, const OsdValue& v) {
    char full[OSD_TEXT_MAX + 1];
    formatOsdRows(full, v);
    CHECK_EQ((int)strlen(full), OSD_V_COLS * OSD_V_ITEMS);
    for (int r = 0; r < OSD_V_ITEMS; r++) {
        CHECK_EQ((int)strspn(full + r * OSD_V_COLS, " ") >= OSD_V_COLS - OSD_V_ITEM, true);
        memcpy(out + r * OSD_V_ITEM, full + r * OSD_V_COLS + (OSD_V_COLS - OSD_V_ITEM), OSD_V_ITEM);
    }
    out[OSD_V_ITEM * OSD_V_ITEMS] = 0;
}

void testFormat() {
    printf("format\n");
    char t[OSD_CHARS + 1];
    OsdStats s;
    s.composedDeci = 555;
    s.shownDeci = 554;
    s.replaced = 1;
    s.speedPct = 100;
    s.tempDeciC = 523;
    const OsdValue v = packOsd(s);
    formatOsd(t, v);
    printf("  [%s]\n", t);
    CHECK_EQ((int)strlen(t), OSD_CHARS);
    CHECK_EQ(strcmp(t + OSD_CHARS - 22, "55.5/55.4/1 100% 52.3C"), 0);
    CHECK_EQ(t[0], ' ');
    // 0.1 の桁が 0 でも出す。10 未満も 1 桁で出す
    s.tempDeciC = 480;
    s.composedDeci = 600;
    s.shownDeci = 5;
    formatOsd(t, packOsd(s));
    CHECK_EQ(strcmp(t + OSD_CHARS - 21, "60.0/0.5/1 100% 48.0C"), 0);
    s.tempDeciC = 5;
    formatOsd(t, packOsd(s));
    CHECK_EQ(strcmp(t + OSD_CHARS - 20, "60.0/0.5/1 100% 0.5C"), 0);
    // 温度が読めなければ出さない
    s.tempDeciC = -1;
    formatOsd(t, packOsd(s));
    CHECK_EQ(strcmp(t + OSD_CHARS - 15, "60.0/0.5/1 100%"), 0);
    CHECK_EQ(t[OSD_CHARS - 16], ' ');
    // フレームが OSD_V_ITEM 文字に入らなければ、fps を四捨五入した整数にする
    s.composedDeci = 555;
    s.shownDeci = 554;
    s.replaced = 12;
    formatOsd(t, packOsd(s));
    CHECK_EQ(strcmp(t + OSD_CHARS - 14, " 56/55/12 100%"), 0);
    s.composedDeci = 1200; // 120.0/55.4/1 は 12 文字
    s.replaced = 1;
    formatOsd(t, packOsd(s));
    CHECK_EQ(strcmp(t + OSD_CHARS - 14, " 120/55/1 100%"), 0);
    s.composedDeci = 999; // 99.9/55.4/1 は 11 文字なので小数のまま
    formatOsd(t, packOsd(s));
    CHECK_EQ(strcmp(t + OSD_CHARS - 16, "99.9/55.4/1 100%"), 0);
    // いちばん長いとき (999.9 fps、999、127.9 度で頭打ち) もちょうど入る。999.9 を四捨五入しても 999
    s.composedDeci = s.shownDeci = 50000;
    s.replaced = 5000;
    s.speedPct = 1234;
    s.tempDeciC = 5000;
    formatOsd(t, packOsd(s));
    CHECK_EQ(strcmp(t, "    999/999/999 999% 127.9C"), 0);
    // クロックを下げたことも足すと、ちょうど OSD_CHARS 文字。ファンは横 1 行には出さない (段階も回転数も)
    s.throttled = true;
    s.fanLevel = 4;
    s.fanRpm = 2200;
    formatOsd(t, packOsd(s));
    CHECK_EQ(strcmp(t, "999/999/999 999% 127.9C THR"), 0);
    // ヒープ、MPU も横 1 行には出さない
    s.heapMB = 52;
    s.mpu = 68030;
    s.mpuMHz = 25;
    formatOsd(t, packOsd(s));
    CHECK_EQ(strcmp(t, "999/999/999 999% 127.9C THR"), 0);
    s.heapMB = -1;
    // packOsd() を通していない値 (すべてのビットが 1) でも左が切れない
    OsdValue ones;
    ones.a = ones.b = ~0ull;
    formatOsd(t, ones);
    CHECK_EQ(strcmp(t, "999/999/999 999% 127.9C THR"), 0);
    // a が 0 はまだ数字がない (空白だけ。b に何があっても)。数字がすべて 0 でも a は 0 にはならない
    OsdValue none;
    none.b = ~0ull;
    CHECK_EQ(none.valid(), false);
    formatOsd(t, none);
    CHECK_EQ((int)strspn(t, " "), OSD_CHARS);
    OsdStats z;
    z.tempDeciC = 0;
    CHECK_EQ(packOsd(z).valid(), true);
    formatOsd(t, packOsd(z));
    CHECK_EQ(strcmp(t + OSD_CHARS - 17, "0.0/0.0/0 0% 0.0C"), 0);
    CHECK_EQ(packOsd(OsdStats()).valid(), true);
    CHECK_EQ(packOsd(OsdStats()).b, 0ull); // 出さない項目は b に何も入れない
    const OsdStats u = unpackOsd(v);
    CHECK_EQ(u.composedDeci, 555u);
    CHECK_EQ(u.shownDeci, 554u);
    CHECK_EQ(u.replaced, 1u);
    CHECK_EQ(u.speedPct, 100u);
    CHECK_EQ(u.tempDeciC, 523);
    CHECK_EQ(u.throttled, false);
    CHECK_EQ(u.fanLevel, -1);
    CHECK_EQ(u.fanRpm, -1);
    CHECK_EQ(u.heapMB, -1);
    CHECK_EQ(u.mpu, 0);
    // それぞれの数字がほかの数字に掛からない (1 つずついちばん大きい値にして、ほかが変わらない)
    {
        OsdStats base;
        base.composedDeci = 1;
        base.shownDeci = 2;
        base.replaced = 3;
        base.speedPct = 4;
        base.tempDeciC = 5;
        base.fanLevel = 1;
        base.fanRpm = 70;
        base.heapMB = 6;
        base.mpu = 68000;
        base.mpuMHz = 8;
        base.pi = OSD_PI_3B;
        base.armMHz = 600;
        base.effMHz = 9;
        for (int k = 0; k < 16; k++) {
            OsdStats m = base;
            if (k == 0) m.composedDeci = OSD_FPS_DECI_MAX;
            if (k == 1) m.shownDeci = OSD_FPS_DECI_MAX;
            if (k == 2) m.replaced = OSD_COUNT_MAX;
            if (k == 3) m.speedPct = OSD_COUNT_MAX;
            if (k == 4) m.tempDeciC = (int)OSD_TEMP_DECI_MAX;
            if (k == 5) m.fanLevel = (int)OSD_FAN_MAX;
            if (k == 6) m.fanRpm = (int)OSD_RPM_MAX;
            if (k == 7) m.heapMB = (int)OSD_HEAP_MAX;
            if (k == 8) m.mpu = 68060;
            if (k == 9) m.mpu = 68030;
            if (k == 10) m.mpuMHz = OSD_MHZ_MAX;
            if (k == 11) m.throttled = true;
            if (k == 12) m.tempDeciC = -1;
            if (k == 13) m.pi = OSD_PI_OTHER;
            if (k == 14) m.armMHz = OSD_ARM_MHZ_MAX;
            if (k == 15) m.effMHz = OSD_EFF_MHZ_MAX;
            const OsdStats r = unpackOsd(packOsd(m));
            CHECK_EQ(r.composedDeci, m.composedDeci);
            CHECK_EQ(r.shownDeci, m.shownDeci);
            CHECK_EQ(r.replaced, m.replaced);
            CHECK_EQ(r.speedPct, m.speedPct);
            CHECK_EQ(r.tempDeciC, m.tempDeciC);
            CHECK_EQ(r.fanLevel, m.fanLevel);
            CHECK_EQ(r.fanRpm, m.fanRpm);
            CHECK_EQ(r.heapMB, m.heapMB);
            CHECK_EQ(r.mpu, m.mpu);
            CHECK_EQ(r.mpuMHz, m.mpuMHz);
            CHECK_EQ(r.throttled, m.throttled);
            CHECK_EQ(r.pi, m.pi);
            CHECK_EQ(r.armMHz, m.armMHz);
            CHECK_EQ(r.effMHz, m.effMHz);
        }
    }
    // 回転数は 10rpm 単位 (四捨五入)。ファンを出さないときは回転数も出さない
    {
        OsdStats f;
        f.fanLevel = 1;
        f.fanRpm = 2204;
        CHECK_EQ(unpackOsd(packOsd(f)).fanRpm, 2200);
        f.fanRpm = 2205;
        CHECK_EQ(unpackOsd(packOsd(f)).fanRpm, 2210);
        f.fanRpm = 20000;
        CHECK_EQ(unpackOsd(packOsd(f)).fanRpm, (int)OSD_RPM_MAX);
        f.fanLevel = -1;
        f.fanRpm = 2200;
        CHECK_EQ(unpackOsd(packOsd(f)).fanRpm, -1);
        // 知らない MPU は出さない
        OsdStats m;
        m.mpu = 68020;
        m.mpuMHz = 16;
        CHECK_EQ(unpackOsd(packOsd(m)).mpu, 0);
        CHECK_EQ(packOsd(m).b, 0ull);
    }

    // 縦に並べるとき: 1 行 OSD_V_ITEM 文字に右寄せして、Pi、MPU、フレーム、速さ、ヒープ、温度、ファン、クロックを下げたか
    char rows[OSD_V_ITEM * OSD_V_ITEMS + 1];
    OsdStats w;
    w.pi = OSD_PI_5;
    w.armMHz = 2400;
    w.mpu = 68030;
    w.mpuMHz = 25;
    w.composedDeci = 555;
    w.shownDeci = 554;
    w.replaced = 1;
    w.speedPct = 100;
    w.tempDeciC = 523;
    w.fanLevel = 1;
    w.fanRpm = 2200;
    w.heapMB = 52;
    fmtRows11(rows, packOsd(w));
    CHECK_EQ(strcmp(rows, "RPi5 2.4GHz"
                          "68030 25MHz"
                          "55.5/55.4/1"
                          "       100%"
                          "   HEAP 52M"
                          "      52.3C"
                          "FAN 2200rpm"
                          "           "), 0);
    // 止めていても、動かしていれば出す。回転数がなければ (負) 0rpm。段階は出さない
    w.fanLevel = 0;
    w.fanRpm = 0;
    fmtRows11(rows, packOsd(w));
    CHECK_EQ(strncmp(rows + 6 * OSD_V_ITEM, "   FAN 0rpm", OSD_V_ITEM), 0);
    w.fanRpm = -1;
    fmtRows11(rows, packOsd(w));
    CHECK_EQ(strncmp(rows + 6 * OSD_V_ITEM, "   FAN 0rpm", OSD_V_ITEM), 0);
    // 4 桁まではそのまま。10000rpm 以上は 0.1k に四捨五入して縮める
    w.fanLevel = 4;
    w.fanRpm = 9990;
    fmtRows11(rows, packOsd(w));
    CHECK_EQ(strncmp(rows + 6 * OSD_V_ITEM, "FAN 9990rpm", OSD_V_ITEM), 0);
    w.fanRpm = 10000;
    fmtRows11(rows, packOsd(w));
    CHECK_EQ(strncmp(rows + 6 * OSD_V_ITEM, "  FAN 10.0k", OSD_V_ITEM), 0);
    w.fanRpm = 12340;
    fmtRows11(rows, packOsd(w));
    CHECK_EQ(strncmp(rows + 6 * OSD_V_ITEM, "  FAN 12.3k", OSD_V_ITEM), 0);
    w.fanRpm = 12350;
    fmtRows11(rows, packOsd(w));
    CHECK_EQ(strncmp(rows + 6 * OSD_V_ITEM, "  FAN 12.4k", OSD_V_ITEM), 0);
    w.fanLevel = 1;
    w.fanRpm = 2200;
    // 68000 の 10MHz と 16MHz、68060 の 50MHz と 75MHz (値は Machine::Config から来る)
    w.mpu = 68000;
    w.mpuMHz = 10;
    fmtRows11(rows, packOsd(w));
    CHECK_EQ(strncmp(rows + OSD_V_ITEM, "68000 10MHz", OSD_V_ITEM), 0);
    w.mpu = 68060;
    w.mpuMHz = 75;
    fmtRows11(rows, packOsd(w));
    CHECK_EQ(strncmp(rows + OSD_V_ITEM, "68060 75MHz", OSD_V_ITEM), 0);
    w.mpuMHz = 100; // 3 桁なら MHz を M にして 11 文字に入れる
    fmtRows11(rows, packOsd(w));
    CHECK_EQ(strncmp(rows + OSD_V_ITEM, " 68060 100M", OSD_V_ITEM), 0);
    // 最高速モードは実効のクロックに「~」を付けて出す (どれも 11 文字に入る)
    w.mpuMHz = 75;
    w.effMHz = 210;
    fmtRows11(rows, packOsd(w));
    CHECK_EQ(strncmp(rows + OSD_V_ITEM, "68060 ~210M", OSD_V_ITEM), 0);
    w.effMHz = 1200;
    fmtRows11(rows, packOsd(w));
    CHECK_EQ(strncmp(rows + OSD_V_ITEM, "68060 ~1.2G", OSD_V_ITEM), 0);
    w.effMHz = 99999; // 9900 まで
    CHECK_EQ(unpackOsd(packOsd(w)).effMHz, OSD_EFF_MHZ_MAX);
    fmtRows11(rows, packOsd(w));
    CHECK_EQ(strncmp(rows + OSD_V_ITEM, "68060 ~9.9G", OSD_V_ITEM), 0);
    w.mpu = 68000;
    w.mpuMHz = 10;
    w.effMHz = 20;
    fmtRows11(rows, packOsd(w));
    CHECK_EQ(strncmp(rows + OSD_V_ITEM, " 68000 ~20M", OSD_V_ITEM), 0);
    w.effMHz = 9;
    fmtRows11(rows, packOsd(w));
    CHECK_EQ(strncmp(rows + OSD_V_ITEM, "68000 ~9MHz", OSD_V_ITEM), 0);
    w.effMHz = 0; // 最高速モードでなければ、ふつうのクロック
    fmtRows11(rows, packOsd(w));
    CHECK_EQ(strncmp(rows + OSD_V_ITEM, "68000 10MHz", OSD_V_ITEM), 0);
    w.mpu = 0;
    w.effMHz = 210; // MPU を出さないなら実効のクロックも出さない
    CHECK_EQ(unpackOsd(packOsd(w)).effMHz, 0u);
    fmtRows11(rows, packOsd(w));
    CHECK_EQ(strncmp(rows + OSD_V_ITEM, "           ", OSD_V_ITEM), 0);
    w.effMHz = 0;
    w.heapMB = 0; // 0 MB も出す (出さないのは負のとき)
    fmtRows11(rows, packOsd(w));
    CHECK_EQ(strncmp(rows + 4 * OSD_V_ITEM, "    HEAP 0M", OSD_V_ITEM), 0);
    w.heapMB = 5000; // 999 まで
    CHECK_EQ(unpackOsd(packOsd(w)).heapMB, 999);
    w.heapMB = -1;
    w.throttled = true;
    w.fanLevel = -1;
    w.tempDeciC = -1;
    w.pi = OSD_PI_NONE;
    fmtRows11(rows, packOsd(w));
    CHECK_EQ(strcmp(rows, "           "
                          "           "
                          "55.5/55.4/1"
                          "       100%"
                          "           "
                          "           "
                          "           "
                          "  THROTTLED"), 0);
    // 詰められるいちばん大きい値でも、どの行も 11 文字に入る (左が切れない)
    {
        OsdStats big;
        big.composedDeci = big.shownDeci = OSD_FPS_DECI_MAX;
        big.replaced = big.speedPct = OSD_COUNT_MAX;
        big.tempDeciC = (int)OSD_TEMP_DECI_MAX;
        big.fanLevel = (int)OSD_FAN_MAX;
        big.fanRpm = (int)OSD_RPM_MAX;
        big.heapMB = (int)OSD_HEAP_MAX;
        big.mpu = 68060;
        big.mpuMHz = OSD_MHZ_MAX;
        big.throttled = true;
        big.pi = OSD_PI_3APLUS; // いちばん長い名前
        big.armMHz = OSD_ARM_MHZ_MAX;
        fmtRows11(rows, packOsd(big));
        CHECK_EQ(strcmp(rows, "RPi3A+ 5.1G"
                              " 68060 255M"
                              "999/999/999"
                              "       999%"
                              "  HEAP 999M"
                              "     127.9C"
                              "  FAN 20.0k"
                              "  THROTTLED"), 0);
    }
    // packOsd() を通していない値 (すべてのビットが 1) でも同じ (実効のクロックのビットも 1 なので、最高速モードの形になる)
    fmtRows11(rows, ones);
    CHECK_EQ(strcmp(rows, "  Pi 5.1GHz"
                          "68060 ~9.9G"
                          "999/999/999"
                          "       999%"
                          "  HEAP 999M"
                          "     127.9C"
                          "  FAN 20.0k"
                          "  THROTTLED"), 0);
    fmtRows11(rows, none);
    CHECK_EQ((int)strlen(rows), OSD_V_ITEM * OSD_V_ITEMS);
    CHECK_EQ((int)strspn(rows, " "), OSD_V_ITEM * OSD_V_ITEMS);
    const OsdStats x = unpackOsd(packOsd(w));
    CHECK_EQ(x.throttled, true);
    CHECK_EQ(x.fanLevel, -1);
    w.fanLevel = 3;
    CHECK_EQ(unpackOsd(packOsd(w)).fanLevel, 3);
    // 3 ビットに入らない段階は OSD_FAN_MAX にする (ほかの数字に掛からない)
    w.fanLevel = 9;
    const OsdStats y = unpackOsd(packOsd(w));
    CHECK_EQ(y.fanLevel, (int)OSD_FAN_MAX);
    CHECK_EQ(y.throttled, true);
    CHECK_EQ(y.heapMB, -1);
    CHECK_EQ(y.speedPct, 100u);
}

// IP アドレス: 縦に並べるときは最後の空でない行のすぐ下に 1 行 (15 文字に右寄せ)。行数は osdRows()
void testIp() {
    printf("ip\n");
    char rows[OSD_TEXT_MAX + 1];
    OsdStats s;
    s.pi = OSD_PI_3BPLUS;
    s.armMHz = 1400;
    s.mpu = 68000;
    s.mpuMHz = 16;
    s.composedDeci = s.shownDeci = 555;
    s.speedPct = 100;
    s.heapMB = 39;
    s.tempDeciC = 397;
    const OsdValue none = packOsd(s);
    CHECK_EQ(none.c, 0ull);
    CHECK_EQ(none.hasIp(), false);
    CHECK_EQ(OsdValue().hasIp(), false);
    CHECK_EQ(osdRows(none), 6); // 温度の行まで
    CHECK_EQ(osdRows(OsdValue()), OSD_V_ITEMS);
    s.ipv4 = 0xc0a8011bu; // 192.168.1.27
    const OsdValue v = packOsd(s);
    CHECK_EQ(v.hasIp(), true);
    CHECK_EQ(v.a, none.a);
    CHECK_EQ(v.b, none.b);
    CHECK_EQ(unpackOsd(v).ipv4, 0xc0a8011bu);
    CHECK_EQ(v != none, true);
    CHECK_EQ(osdRows(v), 7);
    // Pi 3: ファンも THROTTLED もないので、温度のすぐ下
    formatOsdRows(rows, v, osdRows(v));
    CHECK_EQ(strcmp(rows, "    RPi3+1.4GHz"
                          "    68000 16MHz"
                          "    55.5/55.5/0"
                          "           100%"
                          "       HEAP 39M"
                          "          39.7C"
                          "   192.168.1.27"), 0);
    // 15 文字の IP アドレスも 1 行に入る
    const uint32_t ips[] = {0xffffffffu, 0xc0a864c8u, 0x0a00020fu};
    const char* const texts[] = {"255.255.255.255", "192.168.100.200", "      10.0.2.15"};
    for (int k = 0; k < 3; k++) {
        s.ipv4 = ips[k];
        formatOsdRows(rows, packOsd(s), 7);
        CHECK_EQ(strcmp(rows + 6 * OSD_V_COLS, texts[k]), 0);
    }
    // THROTTLED があればその下、ファンがあればその下 (Pi 5)、両方あればいちばん下の行
    s.ipv4 = 0xc0a8011bu;
    s.throttled = true;
    CHECK_EQ(osdRows(packOsd(s)), 9);
    formatOsdRows(rows, packOsd(s), osdRows(packOsd(s)));
    CHECK_EQ(strcmp(rows + 5 * OSD_V_COLS, "          39.7C"
                                           "               "
                                           "      THROTTLED"
                                           "   192.168.1.27"), 0);
    s.throttled = false;
    s.fanLevel = 1;
    s.fanRpm = 2200;
    CHECK_EQ(osdRows(packOsd(s)), 8);
    formatOsdRows(rows, packOsd(s), osdRows(packOsd(s)));
    CHECK_EQ(strcmp(rows + 6 * OSD_V_COLS, "    FAN 2200rpm"
                                           "   192.168.1.27"), 0);
    // ヒープも温度もない (ホスト版の形): 速さのすぐ下
    OsdStats e;
    e.composedDeci = e.shownDeci = 555;
    e.speedPct = 100;
    e.ipv4 = 0x0a00020fu;
    CHECK_EQ(osdRows(packOsd(e)), 5);
    formatOsdRows(rows, packOsd(e), 5);
    CHECK_EQ(strcmp(rows + 4 * OSD_V_COLS, "      10.0.2.15"), 0);
    // 行数より多い rows を渡しても OSD_V_ROWS まで。足りない rows なら切る (場所の高さに合わせる)
    formatOsdRows(rows, packOsd(e), 99);
    CHECK_EQ((int)strlen(rows), OSD_TEXT_MAX);
    formatOsdRows(rows, packOsd(e), 4);
    CHECK_EQ((int)strlen(rows), 4 * OSD_V_COLS);
    // いちばん大きい値でも、どの行も 15 文字に入る
    OsdValue ones;
    ones.a = ones.b = ones.c = ~0ull;
    CHECK_EQ(osdRows(ones), OSD_V_ROWS);
    formatOsdRows(rows, ones, OSD_V_ROWS);
    CHECK_EQ(strcmp(rows + 7 * OSD_V_COLS, "      THROTTLED"
                                           "255.255.255.255"), 0);
}

// IP アドレスが出たり消えたり変わったりしたとき: 行数 (場所) が変われば前の場所を消して描き、アドレスだけ変われば
// 同じ場所の枠全体を描き直す (前の文字は残らない)。重ねるときも同じ
void testIpRedraw() {
    printf("ip redraw\n");
    OsdStats s;
    s.composedDeci = s.shownDeci = 555;
    s.speedPct = 100;
    s.heapMB = 39;
    s.tempDeciC = 400;
    const OsdValue none = packOsd(s);
    s.ipv4 = 0xc0a864c8u;
    const OsdValue ip15 = packOsd(s);
    s.ipv4 = 0xc0a8011bu;
    const OsdValue ip12 = packOsd(s);
    s.throttled = true;
    const OsdValue ip12t = packOsd(s);
    struct Place { int fbW, fbH, picX, picY, picW, picH; bool overlay; };
    const Place places[] = {
        {1920, 1080, 192, 28, 1536, 1024, false}, // 1080p の 2 倍 (余白 192)
        {1920, 1080, 240, 0, 1440, 1080, false},  // video_scale=aspect (4:3 の枠、余白 240)
        {960, 540, 96, 14, 768, 512, true},       // gpu_scale=2 (余白 96): 重ねる
    };
    // video_scale=aspect の枠は 1440x1080 を 240 から (上の 2 つ目と同じ。ShowFrame() が chooseOsd() に渡す枠)
    {
        const FrameScale base = computeFrameScale(768, 512, 1920, 1080, true, 4096 - 4);
        const FrameScale cs = videoFrameScale(VIDEO_SCALE_ASPECT, base, 768, 512, 1920, 1080, 4096 - 4);
        CHECK_EQ(cs.dstX, 240);
        CHECK_EQ(cs.dstY, 0);
        CHECK_EQ(cs.dstW, 1440);
        CHECK_EQ(cs.dstH, 1080);
    }
    for (const Place& pl : places) {
        auto place = [&](const OsdValue& v) {
            return chooseOsd(OSD_MODE_ON, pl.fbW, pl.fbH, pl.picX, pl.picY, pl.picW, pl.picH, osdChars(v), osdRows(v));
        };
        const OsdRect r0 = place(none), r1 = place(ip12), r2 = place(ip15), r3 = place(ip12t);
        printf("  %dx%d pic %d+%d: %d,%d %dx%d overlay %d\n", pl.fbW, pl.fbH, pl.picX, pl.picW, r1.x, r1.y, r1.w, r1.h,
               r1.overlay);
        CHECK_EQ(r1.vertical, true);
        CHECK_EQ(r1.overlay, pl.overlay);
        CHECK_EQ(r1.scale, 1);
        CHECK_EQ(r1.x + r1.w <= pl.fbW - OSD_PAD_X, true);
        if (!pl.overlay) CHECK_EQ(r1.x >= pl.picX + pl.picW, true); // X68000 の画面に重ならない
        CHECK_EQ(r1 == r2, true);  // アドレスの長さでは場所は変わらない (15 文字の幅)
        CHECK_EQ(r0 != r1, true);  // 行が 1 つ増える
        CHECK_EQ(r3 != r1, true);  // THROTTLED で 2 行増える
        OsdState st;
        CHECK_EQ(st.update(true, none, r0).draw, true);
        OsdState::Step p = st.update(true, ip12, r1);
        CHECK_EQ(p.erase && p.eraseRect == r0 && p.draw, true);
        p = st.update(true, ip15, r2);
        CHECK_EQ(!p.erase && p.draw, true);
        p = st.update(true, ip12t, r3);
        CHECK_EQ(p.erase && p.eraseRect == r2 && p.draw, true);
        p = st.update(true, ip12, r1); // THROTTLED が消えた
        CHECK_EQ(p.erase && p.eraseRect == r3 && p.draw, true);
        p = st.update(true, none, r0); // つながらなくなった
        CHECK_EQ(p.erase && p.eraseRect == r1 && p.draw, true);
        CHECK_EQ(st.overlaid(), pl.overlay);
        // 1 枚の絵に 15 文字、12 文字の順に描いた絵は、はじめから 12 文字を描いた絵と同じ
        std::vector<uint32_t> a((size_t)OSD_IMAGE_PIXELS, 0x12345678u), b((size_t)OSD_IMAGE_PIXELS, 0x9abcdef0u);
        renderOsd(a.data(), r2, ip15);
        renderOsd(a.data(), r1, ip12);
        renderOsd(b.data(), r1, ip12);
        CHECK_EQ(memcmp(a.data(), b.data(), (size_t)r1.w * r1.h * 4), 0);
    }
}

// CKernel::ShowFrame() と同じ順に、OSD を消す・描く・X68000 の画面を写す (行の写しあり) を行う
struct ShowSim {
    int fw, fh, sw, sh;
    FrameScale s;
    size_t pitch;
    std::vector<uint32_t> src, fb, line, cachePix, img;
    FrameRowCache cache;
    OsdState st;
    OsdRect rect; // 最後に置いた場所
    ShowSim(int fw_, int fh_, int sw_, int sh_, const FrameScale& s_)
        : fw(fw_), fh(fh_), sw(sw_), sh(sh_), s(s_), pitch((size_t)sw_ * 4), src((size_t)fw_ * fh_),
          fb((size_t)sw_ * sh_ + 4, 0x12345678u), line(4096 + 4), cachePix((size_t)fw_ * fh_),
          img((size_t)OSD_IMAGE_PIXELS) {
        for (size_t i = 0; i < src.size(); i++) src[i] = (uint32_t)(i * 2654435761u) & 0xffffff;
        cache.pixels = cachePix.data();
        cache.pitch = fw;
        cache.rows = fh;
        fillOutside((uint8_t*)fb.data(), pitch, sw, sh, s, 0xff000000u);
    }
    void force(int y0, int y1) {
        if (cache.forceY0 >= cache.forceY1) {
            cache.forceY0 = y0;
            cache.forceY1 = y1;
            return;
        }
        if (y0 < cache.forceY0) cache.forceY0 = y0;
        if (y1 > cache.forceY1) cache.forceY1 = y1;
    }
    void show(const OsdValue& v) {
        uint8_t* base = (uint8_t*)fb.data();
        rect = chooseOsd(OSD_MODE_ON, sw, sh, s.dstX, s.dstY, s.dstW, s.dstH, osdChars(v), osdRows(v));
        const OsdState::Step p = st.update(true, v, rect);
        if (p.erase) {
            eraseOsd<CheckingStore>(base, pitch, p.eraseRect, OSD_BG);
            force(p.eraseRect.y, p.eraseRect.y + p.eraseRect.h);
        }
        if (p.draw && rect.overlay) force(rect.y, rect.y + rect.h);
        if (p.draw) renderOsd(img.data(), rect, v);
        FrameOverlay ov;
        if (st.overlaid()) {
            ov.pixels = img.data();
            ov.x = rect.x;
            ov.y = rect.y;
            ov.w = rect.w;
            ov.h = rect.h;
        }
        presentScaled<CheckingStore>(src.data(), fw, base, pitch, s, line.data(), st.overlaid() ? &ov : nullptr, &cache);
        if (p.draw) drawOsdImage<CheckingStore>(base, pitch, rect, img.data());
    }
};

// 縦に並べたものを X68000 の画面の右端と上端にまたがって重ねる (960x540 に 768x512、gpu_scale=2 の形): 枠の中は重ねる絵、
// 枠の外は drawOsdImage() で、どちらも OSD の絵になり、ほかの画素は重ねないときと同じ。
// 行数が変わる (IP アドレスが消える、THROTTLED が消える) か、横 1 行と縦が入れ替わったあとのフレームバッファは、
// はじめから後の値だけを出したときと同じ (前の場所が残らない)
void testOverlayVertical() {
    printf("overlay vertical\n");
    OsdStats st;
    st.pi = OSD_PI_3BPLUS;
    st.armMHz = 1400;
    st.composedDeci = st.shownDeci = 555;
    st.speedPct = 100;
    st.heapMB = 39;
    st.tempDeciC = 397;
    const OsdValue none = packOsd(st);
    st.ipv4 = 0xc0a864c8u;
    const OsdValue ip = packOsd(st);
    st.throttled = true;
    st.fanLevel = 1;
    st.fanRpm = 2300;
    const OsdValue full = packOsd(st);
    {
        const FrameScale s = computeFrameScale(768, 512, 960, 540, true, 4096);
        ShowSim a(768, 512, 960, 540, s);
        a.show(full);
        const OsdRect r = a.rect;
        CHECK_EQ(r.vertical && r.overlay, true);
        CHECK_EQ(r.x < s.dstX + s.dstW && r.x + r.w > s.dstX + s.dstW, true); // 右端をまたぐ
        CHECK_EQ(r.y < s.dstY && r.y + r.h > s.dstY, true);                 // 上端をまたぐ
        CHECK_EQ(r.h, OSD_V_ROWS * 16);
        ShowSim plain(768, 512, 960, 540, s); // OSD なしで写したもの
        presentScaled<CheckingStore>(plain.src.data(), 768, (uint8_t*)plain.fb.data(), plain.pitch, s, plain.line.data());
        long wrongInside = 0, diffOutside = 0;
        for (int y = 0; y < 540; y++)
            for (int x = 0; x < 960; x++) {
                const size_t i = (size_t)y * 960 + x;
                if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
                    if (a.fb[i] != a.img[(size_t)(y - r.y) * r.w + (x - r.x)]) wrongInside++;
                } else if (a.fb[i] != plain.fb[i]) {
                    diffOutside++;
                }
            }
        CHECK_EQ(wrongInside, 0L);
        CHECK_EQ(diffOutside, 0L);
        // 同じ値で次のフレームを写しても (描き直さず、重ねる絵だけ渡す) 変わらない
        const std::vector<uint32_t> before = a.fb;
        a.show(full);
        CHECK_EQ(a.fb == before, true);
        // IP アドレスと THROTTLED とファンが消えて 3 行減る。前の場所 (画面の外の部分も中の部分も) が残らない
        a.show(none);
        ShowSim b(768, 512, 960, 540, s);
        b.show(none);
        CHECK_EQ(a.rect == b.rect, true);
        CHECK_EQ(a.fb == b.fb, true);
        a.show(ip);
        ShowSim c(768, 512, 960, 540, s);
        c.show(ip);
        CHECK_EQ(a.fb == c.fb, true);
    }
    {
        // 1080p の 2 倍 (余白に縦): IP アドレスが消えて高さが縮んでも、前の行が残らない
        const FrameScale s = computeFrameScale(768, 512, 1920, 1080, true, 4096);
        ShowSim a(768, 512, 1920, 1080, s), b(768, 512, 1920, 1080, s);
        a.show(full);
        CHECK_EQ(a.rect.overlay, false);
        a.show(none);
        b.show(none);
        CHECK_EQ(a.fb == b.fb, true);
    }
    {
        // 上の余白が 100 ライン (768x612 に 768x512 を 100 から): 6 行 (4 + 96 = 100) なら上の余白に縦、IP アドレスで
        // 7 行になると縦は重なるので上の余白に横 1 行。入れ替わったあとも前の場所が残らない
        FrameScale s = computeFrameScale(768, 512, 768, 612, false, 4096);
        s.dstX = 0;
        s.dstY = 100;
        CHECK_EQ(s.dstW, 768);
        CHECK_EQ(osdRows(none), 6);
        CHECK_EQ(osdRows(ip), 7);
        ShowSim a(768, 512, 768, 612, s), b(768, 512, 768, 612, s), c(768, 512, 768, 612, s);
        a.show(none);
        CHECK_EQ(a.rect.vertical && !a.rect.overlay, true);
        a.show(ip);
        CHECK_EQ(!a.rect.vertical && !a.rect.overlay, true);
        b.show(ip);
        CHECK_EQ(a.fb == b.fb, true);
        a.show(none);
        c.show(none);
        CHECK_EQ(a.fb == c.fb, true);
    }
}

// 境界: 横 1 行は上の余白が 20 ライン (4 + 16) あれば重ならず、19 なら重なる。縦の 9 行は高さが 148 ライン (4 + 144)
// あれば入り、147 なら入らない
void testOsdBounds() {
    printf("osd bounds\n");
    OsdStats st;
    st.composedDeci = st.shownDeci = 555;
    st.speedPct = 100;
    st.tempDeciC = 400;
    st.ipv4 = 0xc0a8011bu;
    const OsdValue v = packOsd(st);
    // 768 幅の画面 (右の余白なし) を上から 20 / 19 ラインに置く
    OsdRect r = chooseOsd(OSD_MODE_ON, 768, 700, 0, 20, 768, 512, osdChars(v), osdRows(v));
    CHECK_EQ(r.vertical, false);
    CHECK_EQ(r.overlay, false);
    CHECK_EQ(r.y + r.h, 20);
    r = chooseOsd(OSD_MODE_ON, 768, 700, 0, 19, 768, 512, osdChars(v), osdRows(v));
    CHECK_EQ(r.vertical, true); // 横 1 行も重なるので、縦に並べて重ねる
    CHECK_EQ(r.overlay, true);
    // 縦 9 行の高さ
    st.throttled = true;
    st.fanLevel = 1;
    const OsdValue nine = packOsd(st);
    CHECK_EQ(osdRows(nine), OSD_V_ROWS);
    CHECK_EQ(placeOsdVertical(768, 148, 0, 0, 768, 148, OSD_V_ROWS).w > 0, true);
    CHECK_EQ(placeOsdVertical(768, 147, 0, 0, 768, 147, OSD_V_ROWS).w, 0);
    r = chooseOsd(OSD_MODE_ON, 768, 148, 0, 0, 768, 148, osdChars(nine), osdRows(nine));
    CHECK_EQ(r.vertical && r.overlay, true);
    CHECK_EQ(r.y + r.h, 148);
    // 147 ラインでは縦が入らず、横 1 行も重なるので、osd=on は出さず、osd=fps は横 1 行を重ねる
    CHECK_EQ(chooseOsd(OSD_MODE_ON, 768, 147, 0, 0, 768, 147, osdChars(nine), osdRows(nine)).w, 0);
    r = chooseOsd(OSD_MODE_FPS, 768, 147, 0, 0, 768, 147, osdChars(nine), osdRows(nine));
    CHECK_EQ(!r.vertical && r.overlay, true);
    CHECK_EQ(r.w, osdChars(nine) * 8); // 「 192.168.1.27」まで入る
}

// Raspberry Pi のモデルと ARM のクロックの行 (縦に並べるときのいちばん上)。どのモデルでも 11 文字に入る
void testPiRow() {
    printf("pi row\n");
    char rows[OSD_V_ITEM * OSD_V_ITEMS + 1];
    auto row0 = [&](int pi, unsigned mhz) {
        OsdStats s;
        s.pi = pi;
        s.armMHz = mhz;
        fmtRows11(rows, packOsd(s));
        rows[OSD_V_ITEM] = 0;
        return (const char*)rows;
    };
    CHECK_EQ(strcmp(row0(OSD_PI_5, 2400), "RPi5 2.4GHz"), 0);
    CHECK_EQ(strcmp(row0(OSD_PI_4B, 1500), "RPi4 1.5GHz"), 0);
    CHECK_EQ(strcmp(row0(OSD_PI_4B, 1800), "RPi4 1.8GHz"), 0);
    CHECK_EQ(strcmp(row0(OSD_PI_3BPLUS, 1400), "RPi3+1.4GHz"), 0); // 空白を詰める (名前が数字で終わらない)
    CHECK_EQ(strcmp(row0(OSD_PI_3B, 1200), "RPi3 1.2GHz"), 0);
    CHECK_EQ(strcmp(row0(OSD_PI_3APLUS, 1400), "RPi3A+ 1.4G"), 0); // 「RPi3A+1.4GHz」は 12 文字なので「GHz」を「G」に縮める
    CHECK_EQ(strcmp(row0(OSD_PI_400, 1800), " Pi400 1.8G"), 0);   // 数字で終わる名前は空白を詰めない
    CHECK_EQ(strcmp(row0(OSD_PI_500, 2400), " Pi500 2.4G"), 0);
    CHECK_EQ(strcmp(row0(OSD_PI_ZERO2W, 1000), " Zero2 1.0G"), 0);
    CHECK_EQ(strcmp(row0(OSD_PI_CM4, 1500), " CM4 1.5GHz"), 0);
    CHECK_EQ(strcmp(row0(OSD_PI_CM5LITE, 2400), "CM5L 2.4GHz"), 0);
    CHECK_EQ(strcmp(row0(OSD_PI_CM3PLUS, 1200), "CM3+ 1.2GHz"), 0);
    CHECK_EQ(strcmp(row0(OSD_PI_OTHER, 1500), "  Pi 1.5GHz"), 0);
    CHECK_EQ(strcmp(row0(OSD_PI_3B, 700), "RPi3 700MHz"), 0);         // 1GHz より下は MHz (QEMU など)
    CHECK_EQ(strcmp(row0(OSD_PI_5, 0), "       RPi5"), 0);          // クロックがなければ名前だけ
    CHECK_EQ(strcmp(row0(OSD_PI_NONE, 2400), "           "), 0);    // モデルがなければ出さない
    // どのモデルとクロックでも 11 文字に入る (右寄せの左が切れない = 先頭の文字が名前の先頭)
    for (int pi = 1; pi < OSD_PI_COUNT; pi++) {
        const unsigned clocks[] = {0, 600, 700, 990, 1000, 1400, 2400, 3000, OSD_ARM_MHZ_MAX};
        for (unsigned mhz : clocks) {
            const char* r = row0(pi, mhz);
            const char* name = osdPiName(pi);
            const char* p = r;
            while (*p == ' ') p++;
            CHECK_EQ(strncmp(p, name, strlen(name)), 0);
        }
    }
    // 出すクロックに丸める (1GHz 以上は 100MHz、より下は 10MHz)。b に入れて戻しても同じ
    CHECK_EQ(osdArmMHzShown(2399), 2400u);
    CHECK_EQ(osdArmMHzShown(2449), 2400u);
    CHECK_EQ(osdArmMHzShown(1450), 1500u);
    CHECK_EQ(osdArmMHzShown(996), 1000u);
    CHECK_EQ(osdArmMHzShown(700), 700u);
    CHECK_EQ(osdArmMHzShown(604), 600u);
    CHECK_EQ(osdArmMHzShown(0), 0u);
    OsdStats s;
    s.pi = OSD_PI_4B;
    s.armMHz = 1800;
    s.mpu = 68000;
    s.mpuMHz = 16;
    s.heapMB = 999;
    const OsdStats u = unpackOsd(packOsd(s));
    CHECK_EQ(u.pi, (int)OSD_PI_4B);
    CHECK_EQ(u.armMHz, 1800u);
    CHECK_EQ(u.mpu, 68000);
    CHECK_EQ(u.mpuMHz, 16u);
    CHECK_EQ(u.heapMB, 999);
    s.armMHz = 9999; // 5110 まで
    CHECK_EQ(unpackOsd(packOsd(s)).armMHz, OSD_ARM_MHZ_MAX);
    s.pi = 99; // 知らないモデルは出さない
    CHECK_EQ(unpackOsd(packOsd(s)).pi, (int)OSD_PI_NONE);
    // a は変わらない (横 1 行には出さない)
    OsdStats h;
    const OsdValue h0 = packOsd(h);
    h.pi = OSD_PI_5;
    h.armMHz = 2400;
    CHECK_EQ(packOsd(h).a, h0.a);
}

// fps を 0.1 まで求める (測った時間で割る)
void testFps() {
    printf("fps\n");
    // X68000 の 55.46Hz: 1 秒あまりで 56 フレーム
    CHECK_EQ(osdFpsDeci(56, 1009700), 555u);
    CHECK_EQ(osdFpsDeci(55, 1000000), 550u);
    CHECK_EQ(osdFpsDeci(60, 1000000), 600u);
    CHECK_EQ(osdFpsDeci(111, 2001443), 555u); // 2 秒ほどでも同じ
    CHECK_EQ(osdFpsDeci(1, 1000000), 10u);
    CHECK_EQ(osdFpsDeci(0, 1000000), 0u);
    CHECK_EQ(osdFpsDeci(55, 0), 0u); // 時間が 0 なら 0
    // 四捨五入: 55.45 は 55.5、55.44 は 55.4
    CHECK_EQ(osdFpsDeci(5545, 100000000), 555u);
    CHECK_EQ(osdFpsDeci(5544, 100000000), 554u);
}

// 出す回転数は 100rpm に丸め、前に出した値から 100rpm 以上変わったときだけ変える
void testRpm() {
    printf("rpm\n");
    CHECK_EQ(osdRpmShown(-1, 2234), 2200);
    CHECK_EQ(osdRpmShown(-1, 2250), 2300);
    CHECK_EQ(osdRpmShown(-1, 0), 0);
    // 毎秒少しずつ変わっても、100rpm 未満なら変えない (丸めの境目 2250 をまたいでも)
    CHECK_EQ(osdRpmShown(2200, 2299), 2200);
    CHECK_EQ(osdRpmShown(2200, 2101), 2200);
    CHECK_EQ(osdRpmShown(2200, 2260), 2200);
    CHECK_EQ(osdRpmShown(2200, 2240), 2200);
    // 100rpm 以上変われば、丸めた値に変える
    CHECK_EQ(osdRpmShown(2200, 2300), 2300);
    CHECK_EQ(osdRpmShown(2200, 2100), 2100);
    CHECK_EQ(osdRpmShown(2200, 2449), 2400);
    // 止まった
    CHECK_EQ(osdRpmShown(2200, 0), 0);
    CHECK_EQ(osdRpmShown(0, 40), 0);
    CHECK_EQ(osdRpmShown(0, 1700), 1700);
}

// 最高速モードの実効のクロック: クロック × 速さ / 100 を kHz で求めて上 2 桁に 1 回だけ丸める。前に出した値から、
// 丸めの単位、前に出した値の 5% (MHz に四捨五入)、2MHz のいちばん大きい幅以上変わったときだけ変える
void testEffMHz() {
    printf("eff mhz\n");
    CHECK_EQ(osdEffKHz(75000000, 280), 210000ull);
    CHECK_EQ(osdEffKHz(75000000, 286), 214500ull);
    CHECK_EQ(osdEffKHz(10000000, 105), 10500ull);
    // 68000 (10MHz / 16MHz / 24MHz) でも同じ式
    CHECK_EQ(osdEffKHz(16000000, 100), 16000ull);
    CHECK_EQ(osdEffKHz(16000000, 3200), 512000ull);
    CHECK_EQ(osdEffKHz(24000000, 137), 32880ull);
    CHECK_EQ(osdEffMHzShown(0, osdEffKHz(16000000, 3200)), 510u);
    CHECK_EQ(osdEffMHzShown(0, osdEffKHz(24000000, 137)), 33u);
    CHECK_EQ(osdEffKHz(75000000, 3200), 2400000ull);
    CHECK_EQ(osdEffKHz(75000000, 0), 0ull);
    CHECK_EQ(osdEffKHz(75000000, -5), 0ull);
    CHECK_EQ(osdEffKHz(75000000, INT_MAX), 75000000ull * INT_MAX / 100000);
    // まだ出していなければ丸めるだけ
    CHECK_EQ(osdEffMHzShown(0, 213000), 210u);
    CHECK_EQ(osdEffMHzShown(0, 215000), 220u);
    CHECK_EQ(osdEffMHzShown(0, 75000), 75u);
    CHECK_EQ(osdEffMHzShown(0, 1234000), 1200u);
    CHECK_EQ(osdEffMHzShown(0, 0), 1u); // 0 は「出さない」なので 1
    CHECK_EQ(osdEffMHzShown(0, 20000000), OSD_EFF_MHZ_MAX);
    CHECK_EQ(osdEffMHzShown(0, 9960000), OSD_EFF_MHZ_MAX);
    // 丸めの境目 (215) をまたいで行き来しても、11MHz (210 の 5%) 未満の変化なら変えない
    CHECK_EQ(osdEffMHzShown(210, 214000), 210u);
    CHECK_EQ(osdEffMHzShown(210, 216000), 210u);
    CHECK_EQ(osdEffMHzShown(210, 219000), 210u);
    CHECK_EQ(osdEffMHzShown(210, 201000), 210u);
    CHECK_EQ(osdEffMHzShown(210, 220000), 210u);
    CHECK_EQ(osdEffMHzShown(210, 200000), 210u);
    // 11MHz 以上変われば、丸めた値に変える
    CHECK_EQ(osdEffMHzShown(210, 221000), 220u);
    CHECK_EQ(osdEffMHzShown(210, 199000), 200u);
    CHECK_EQ(osdEffMHzShown(210, 300000), 300u);
    // 100MHz 未満は 1MHz 単位に丸めるが、5% 未満の変化では変えない
    CHECK_EQ(osdEffMHzShown(75, 75000), 75u);
    CHECK_EQ(osdEffMHzShown(75, 76000), 75u);
    CHECK_EQ(osdEffMHzShown(75, 72000), 75u);
    CHECK_EQ(osdEffMHzShown(75, 79000), 79u);
    CHECK_EQ(osdEffMHzShown(75, 71000), 71u);
    // 40MHz より下は 5% が 1MHz 以下なので、幅は 2MHz (1MHz 変わっただけでは変えない)
    CHECK_EQ(osdEffMHzShown(9, 10000), 9u);
    CHECK_EQ(osdEffMHzShown(9, 10999), 9u);
    CHECK_EQ(osdEffMHzShown(9, 11000), 11u);
    CHECK_EQ(osdEffMHzShown(9, 7000), 7u);
    CHECK_EQ(osdEffMHzShown(20, 21000), 20u);
    CHECK_EQ(osdEffMHzShown(20, 22000), 22u);
    // 10MHz の 204% と 205% (20.4MHz と 20.5MHz。丸めの境目) を行き来しても、20 のまま
    {
        unsigned shown = osdEffMHzShown(0, osdEffKHz(10000000, 204));
        CHECK_EQ(shown, 20u);
        for (int i = 0; i < 10; i++) {
            shown = osdEffMHzShown(shown, osdEffKHz(10000000, i % 2 ? 205 : 204));
            CHECK_EQ(shown, 20u);
        }
        // 20MHz から 27MHz のあたりで、1MHz ずつ交互に揺れても変わらない
        const unsigned starts[] = {20, 21, 22, 23, 24, 25, 26, 27};
        for (unsigned st : starts) {
            unsigned s2 = osdEffMHzShown(0, st * 1000ull);
            CHECK_EQ(s2, st);
            for (int i = 0; i < 10; i++) {
                s2 = osdEffMHzShown(s2, (st * 1000ull) + (i % 2 ? 1000 : 0) - (i % 4 == 2 ? 1000 : 0) + 400);
                CHECK_EQ(s2, st);
            }
        }
    }
    // 52MHz、80MHz、210MHz のあたりで 1 秒ごとに少しずつ揺れても、出す値は変わらない
    {
        const unsigned noisy52[] = {52, 53, 51, 54, 52, 50, 53, 51, 52, 54};
        unsigned shown = osdEffMHzShown(0, 52000);
        for (unsigned v : noisy52) shown = osdEffMHzShown(shown, v * 1000ull);
        CHECK_EQ(shown, 52u);
        const unsigned noisy80[] = {80, 82, 78, 83, 77, 81, 79, 80};
        shown = osdEffMHzShown(0, 80000);
        for (unsigned v : noisy80) shown = osdEffMHzShown(shown, v * 1000ull);
        CHECK_EQ(shown, 80u);
        const unsigned noisy210[] = {214, 206, 219, 203, 216, 211, 201};
        shown = osdEffMHzShown(0, 210000);
        for (unsigned v : noisy210) shown = osdEffMHzShown(shown, v * 1000ull);
        CHECK_EQ(shown, 210u);
    }
    // 1000MHz 以上は 100MHz 単位
    CHECK_EQ(osdEffMHzShown(1200, 1290000), 1200u);
    CHECK_EQ(osdEffMHzShown(1200, 1300000), 1300u);
    // 変換から表示まで通して: 丸めは 1 回だけ (214.5MHz は 215 を経て 220 にせず、210)
    CHECK_EQ(osdEffMHzShown(0, osdEffKHz(75000000, 286)), 210u);
    CHECK_EQ(osdEffMHzShown(0, osdEffKHz(10000000, 105)), 11u); // 10.5MHz は 11 (四捨五入)
    CHECK_EQ(osdEffMHzShown(0, osdEffKHz(10000000, 104)), 10u);
    // 99/100MHz、999/1000MHz の境目
    CHECK_EQ(osdEffMHzShown(0, osdEffKHz(75000000, 132)), 99u);   // 99.0MHz
    CHECK_EQ(osdEffMHzShown(0, osdEffKHz(75000000, 133)), 100u);  // 99.75MHz は 100
    CHECK_EQ(osdEffMHzShown(0, osdEffKHz(100000000, 100)), 100u); // 100MHz
    CHECK_EQ(osdEffMHzShown(0, osdEffKHz(100000000, 104)), 100u); // 104MHz は 10MHz 単位で 100
    CHECK_EQ(osdEffMHzShown(0, osdEffKHz(100000000, 105)), 110u);
    CHECK_EQ(osdEffMHzShown(0, osdEffKHz(75000000, 1332)), 1000u); // 999MHz は 10MHz 単位で 1000
    CHECK_EQ(osdEffMHzShown(0, osdEffKHz(75000000, 1326)), 990u);  // 994.5MHz
    CHECK_EQ(osdEffMHzShown(0, osdEffKHz(100000000, 1049)), 1000u); // 1049MHz は 100MHz 単位で 1000
    CHECK_EQ(osdEffMHzShown(0, osdEffKHz(100000000, 1050)), 1100u);
    // 速さが 0 以下 (起こらないはず) は 1、INT_MAX は上限
    CHECK_EQ(osdEffMHzShown(0, osdEffKHz(75000000, -1)), 1u);
    CHECK_EQ(osdEffMHzShown(0, osdEffKHz(75000000, INT_MAX)), OSD_EFF_MHZ_MAX);
    CHECK_EQ(osdEffMHzShown(0, osdEffKHz(0xffffffffu, INT_MAX)), OSD_EFF_MHZ_MAX);
    // 5% の幅は四捨五入 (89MHz なら 4.45 で 4MHz)
    CHECK_EQ(osdEffMHzShown(89, 92900), 89u);
    CHECK_EQ(osdEffMHzShown(89, 93000), 93u);
    CHECK_EQ(osdEffMHzShown(89, 85001), 89u); // 下側の境目
    CHECK_EQ(osdEffMHzShown(89, 85000), 85u);
}

// 横 1 行の文字数は出す項目で決まる (THR とファンは出すときだけ幅を取る)
void testChars() {
    printf("chars\n");
    OsdStats s;
    s.composedDeci = s.shownDeci = 550;
    s.speedPct = 100;
    s.tempDeciC = 523;
    const OsdValue pi4 = packOsd(s); // Pi 3 / Pi 4 (ファンを動かしていない)
    CHECK_EQ(osdChars(pi4), OSD_CHARS_BASE);
    s.fanLevel = 1;
    s.fanRpm = 2200;
    const OsdValue pi5 = packOsd(s);
    CHECK_EQ(osdChars(pi5), OSD_CHARS_BASE); // ファンは横 1 行に出さないので幅は変わらない
    s.throttled = true;
    const OsdValue thr = packOsd(s);
    CHECK_EQ(osdChars(thr), OSD_CHARS);
    s.fanLevel = -1;
    CHECK_EQ(osdChars(packOsd(s)), OSD_CHARS_BASE + 4);
    CHECK_EQ(OSD_CHARS, OSD_CHARS_BASE + 4);
    // いちばん長い数字でも OSD_CHARS_BASE 文字に入る
    char t[OSD_CHARS + 1];
    OsdStats big;
    big.composedDeci = big.shownDeci = OSD_FPS_DECI_MAX;
    big.replaced = big.speedPct = OSD_COUNT_MAX;
    big.tempDeciC = (int)OSD_TEMP_DECI_MAX;
    big.heapMB = 999;
    big.mpu = 68060;
    big.mpuMHz = 75;
    formatOsd(t, packOsd(big), OSD_CHARS_BASE);
    CHECK_EQ(strcmp(t, "999/999/999 999% 127.9C"), 0);
    CHECK_EQ(osdChars(packOsd(big)), OSD_CHARS_BASE);
    formatOsd(t, pi5, osdChars(pi5));
    CHECK_EQ(strcmp(t, " 55.0/55.0/0 100% 52.3C"), 0);
    // 重ねる範囲の幅も文字数に合わせる (Pi 3 / Pi 4 は 184 画素)
    CHECK_EQ(OSD_CHARS_BASE * 8, 184);
    OsdRect r = placeOsd(768, 512, 0, 0, 768, 512, 1, osdChars(pi4));
    CHECK_EQ(r.w, OSD_CHARS_BASE * 8);
    CHECK_EQ(r.overlay, true);
    CHECK_EQ(r.x, 768 - OSD_CHARS_BASE * 8 - OSD_PAD_X);
    // 絵も同じ幅の文字で作る
    std::vector<uint32_t> a((size_t)r.w * r.h), b((size_t)r.w * r.h);
    formatOsd(t, pi4, OSD_CHARS_BASE);
    for (int y = 0; y < r.h; y++) renderOsdRow(a.data() + (size_t)y * r.w, t, y, 1, OSD_CHARS_BASE);
    renderOsd(b.data(), r, pi4);
    CHECK_EQ(a == b, true);
    renderOsdImage(b.data(), r, t);
    CHECK_EQ(a == b, true);
    // 文字数が変わる (THR が消える) と場所が変わるので、前の広い範囲を消してから描く
    const OsdRect wide = placeOsd(768, 512, 0, 0, 768, 512, 1, osdChars(thr));
    const OsdRect narrow = placeOsd(768, 512, 0, 0, 768, 512, 1, osdChars(pi5));
    CHECK_EQ(wide.w, OSD_CHARS * 8);
    CHECK_EQ(narrow.w, OSD_CHARS_BASE * 8);
    OsdState st;
    CHECK_EQ(st.update(true, thr, wide).draw, true);
    const OsdState::Step p = st.update(true, pi5, narrow);
    CHECK_EQ(p.erase, true);
    CHECK_EQ(p.eraseRect == wide, true);
    CHECK_EQ(p.draw, true);
    CHECK_EQ(st.overlaid(), true);
    // 範囲の外の文字数は OSD_CHARS_MAX にする
    CHECK_EQ(placeOsd(768, 512, 0, 0, 768, 512, 1, 0).w, OSD_CHARS_MAX * 8);
    CHECK_EQ(placeOsd(768, 512, 0, 0, 768, 512, 1, OSD_CHARS_MAX + 1).w, OSD_CHARS_MAX * 8);
    // IP アドレスがあれば、横 1 行のいちばん右に足す (「 192.168.1.27」の分だけ広げる)。いちばん長いときに OSD_CHARS_MAX
    char w[OSD_CHARS_MAX + 1];
    s.ipv4 = 0xc0a8011bu;
    CHECK_EQ(osdChars(packOsd(s)), OSD_CHARS_BASE + 4 + 13);
    formatOsd(w, packOsd(s), osdChars(packOsd(s)));
    CHECK_EQ(strcmp(w, " 55.0/55.0/0 100% 52.3C THR 192.168.1.27"), 0);
    big.throttled = true;
    big.ipv4 = 0xffffffffu;
    CHECK_EQ(osdChars(packOsd(big)), OSD_CHARS_MAX);
    formatOsd(w, packOsd(big), OSD_CHARS_MAX);
    CHECK_EQ(strcmp(w, "999/999/999 999% 127.9C THR 255.255.255.255"), 0);
}

void testPlace() {
    printf("place\n");
    // 1920x1080 に 2 倍 (1536x1024、192,28 から): 右の余白 (192 画素) に、1 倍の文字で縦に並べる (15 文字)
    FrameScale s = computeFrameScale(768, 512, 1920, 1080, true, 4096);
    OsdRect r = chooseOsd(OSD_MODE_ON, 1920, 1080, s.dstX, s.dstY, s.dstW, s.dstH);
    printf("  1920x1080 x2: %d,%d %dx%d overlay %d vertical %d scale %d\n", r.x, r.y, r.w, r.h, r.overlay, r.vertical,
           r.scale);
    CHECK_EQ(r.vertical, true);
    CHECK_EQ(r.scale, 1);
    CHECK_EQ(r.w, OSD_V_COLS * 8);
    CHECK_EQ(r.h, OSD_V_ITEMS * 16);
    CHECK_EQ(r.x, 1792);
    CHECK_EQ(r.y, OSD_PAD_Y);
    CHECK_EQ(r.overlay, false);
    CHECK_EQ(r.x >= s.dstX + s.dstW, true);
    CHECK_EQ(chooseOsd(OSD_MODE_FPS, 1920, 1080, s.dstX, s.dstY, s.dstW, s.dstH) == r, true);
    CHECK_EQ(chooseOsd(OSD_MODE_OFF, 1920, 1080, s.dstX, s.dstY, s.dstW, s.dstH).w, 0);
    // 行数は rows (osdRows()) で決まる。OSD_V_ROWS まで
    r = chooseOsd(OSD_MODE_ON, 1920, 1080, s.dstX, s.dstY, s.dstW, s.dstH, OSD_CHARS, OSD_V_ROWS);
    CHECK_EQ(r.h, OSD_V_ROWS * 16);
    CHECK_EQ(r.w * r.h, OSD_IMAGE_PIXELS);
    r = chooseOsd(OSD_MODE_ON, 1920, 1080, s.dstX, s.dstY, s.dstW, s.dstH, OSD_CHARS, OSD_V_ROWS + 5);
    CHECK_EQ(r.h, OSD_V_ROWS * 16);
    // 3840x2160 に 4 倍: 倍率はいつも 1
    s = computeFrameScale(768, 512, 3840, 2160, true, 4096);
    r = chooseOsd(OSD_MODE_ON, 3840, 2160, s.dstX, s.dstY, s.dstW, s.dstH);
    CHECK_EQ(r.vertical, true);
    CHECK_EQ(r.scale, 1);
    CHECK_EQ(r.overlay, false);
    // 余白が 128 画素 (15 文字 120 画素と右の間 8 画素がちょうど入る) なら重ならない。127 画素なら、左にずらして重ねる
    r = chooseOsd(OSD_MODE_ON, 768 + 128, 512, 0, 0, 768, 512);
    CHECK_EQ(r.vertical, true);
    CHECK_EQ(r.overlay, false);
    CHECK_EQ(r.x, 768);
    for (int mode : {OSD_MODE_ON, OSD_MODE_FPS}) {
        r = chooseOsd(mode, 768 + 127, 512, 0, 0, 768, 512);
        CHECK_EQ(r.vertical, true);
        CHECK_EQ(r.overlay, true);
        CHECK_EQ(r.x, 764);
        CHECK_EQ(r.x + r.w <= 768 + 127, true);
    }
    // gpu_scale=2 の 960x540 に 768x512 (96,14 から): 余白 96 画素、上 14 画素で、どちらにも入らないので重ねる
    r = chooseOsd(OSD_MODE_ON, 960, 540, 96, 14, 768, 512);
    CHECK_EQ(r.vertical, true);
    CHECK_EQ(r.overlay, true);
    CHECK_EQ(r.x, 832);
    CHECK_EQ(r.x + r.w + OSD_PAD_X <= 960, true);
    // 余白がない (768x512 にそのまま): どちらの設定でも、縦に並べて右上に重ねる
    for (int mode : {OSD_MODE_ON, OSD_MODE_FPS}) {
        r = chooseOsd(mode, 768, 512, 0, 0, 768, 512);
        CHECK_EQ(r.vertical, true);
        CHECK_EQ(r.overlay, true);
        CHECK_EQ(r.x + r.w <= 768, true);
    }
    // 上だけに余白がある (横いっぱい): どちらの設定でも、上の余白に横 1 行で出す (縦に並べると重なるため)
    r = chooseOsd(OSD_MODE_ON, 768, 600, 0, 40, 768, 512);
    CHECK_EQ(r.w, OSD_CHARS * 8);
    CHECK_EQ(r.overlay, false);
    CHECK_EQ(r.vertical, false);
    // 縦に並べたものが入らない高さ (フレームバッファの高さ 100) では、osd=fps なら横 1 行を重ね、osd=on なら出さない
    CHECK_EQ(placeOsdVertical(768, 100, 0, 0, 768, 100).w, 0);
    r = chooseOsd(OSD_MODE_FPS, 768, 100, 0, 0, 768, 100);
    CHECK_EQ(r.vertical, false);
    CHECK_EQ(r.overlay, true);
    CHECK_EQ(chooseOsd(OSD_MODE_ON, 768, 100, 0, 0, 768, 100).w, 0);
    // 1280x720 に 1 倍 (768x512): 余白
    s = computeFrameScale(768, 512, 1280, 720, true, 4096);
    r = placeOsd(1280, 720, s.dstX, s.dstY, s.dstW, s.dstH);
    CHECK_EQ(r.overlay, false);
    // 768x512 にそのまま: 余白がないので重ねる
    s = computeFrameScale(768, 512, 768, 512, true, 4096);
    r = placeOsd(768, 512, s.dstX, s.dstY, s.dstW, s.dstH);
    CHECK_EQ(r.w > 0, true);
    CHECK_EQ(r.overlay, true);
    CHECK_EQ(r.x % 4, 0);
    // 上だけに余白があっても (横いっぱい)、上の余白に入れば重ねない
    r = placeOsd(768, 600, 0, 40, 768, 512);
    CHECK_EQ(r.overlay, false);
    // 倍率 2 (ホスト版)
    r = placeOsd(3000, 2000, 0, 0, 0, 0, 2);
    CHECK_EQ(r.w, OSD_CHARS * 16);
    CHECK_EQ(r.h, 32);
    CHECK_EQ(r.overlay, false);
    // 入らない
    r = placeOsd(100, 100, 0, 0, 100, 100);
    CHECK_EQ(r.w, 0);
    CHECK_EQ(placeOsdVertical(100, 100, 0, 0, 100, 100).w, 0);
    // フレームバッファの幅が 4 の倍数でなくても、左端は 4 画素にそろえる (横 1 行と縦)
    r = placeOsd(1923, 1080, 0, 0, 0, 0);
    CHECK_EQ(r.x % 4, 0);
    CHECK_EQ(r.x + r.w <= 1923 - OSD_PAD_X, true);
    r = placeOsdVertical(1923, 1080, 0, 0, 1536, 1080);
    CHECK_EQ(r.x % 4, 0);
    CHECK_EQ(r.x, 1792);
    // X68000 の画面の幅が 0 (まだ写していない) なら縦に並べない
    CHECK_EQ(placeOsdVertical(1920, 1080, 0, 0, 0, 0).w, 0);
    CHECK_EQ(placeOsdVertical(1920, 1080, 0, 0, -1, 0).w, 0);
    // 並べ方だけが違う場所は別の場所 (描き直す)
    OsdRect p, q;
    q.vertical = true;
    CHECK_EQ(p == q, false);
    CHECK_EQ(p != q, true);
}

// 縦に並べた絵: 行ごとに、その行の文字が横 1 行の絵と同じ形で入る。空の行は黒だけ
void testRenderVertical() {
    printf("render vertical\n");
    OsdStats s;
    s.composedDeci = s.shownDeci = 555;
    s.speedPct = 100;
    s.tempDeciC = 480;
    s.fanLevel = 2;
    s.fanRpm = 2600;
    s.mpu = 68030;
    s.mpuMHz = 25;
    s.ipv4 = 0xc0a864c8u;
    const OsdValue v = packOsd(s);
    CHECK_EQ(osdRows(v), 8); // ファンの行 (7 行目) と IP アドレス
    const OsdRect r = placeOsdVertical(1920, 1080, 192, 28, 1536, 1024, osdRows(v));
    CHECK_EQ(r.h, 8 * 16);
    std::vector<uint32_t> img((size_t)r.w * r.h, 0x12345678u);
    renderOsd(img.data(), r, v);
    char rows[OSD_TEXT_MAX + 1];
    formatOsdRows(rows, v, osdRows(v));
    long diff = 0, white[OSD_V_ROWS] = {};
    std::vector<uint32_t> line((size_t)r.w);
    for (int y = 0; y < r.h; y++) {
        const int row = y / 16;
        renderOsdRow(line.data(), rows + row * OSD_V_COLS, y % 16, 1, OSD_V_COLS);
        for (int x = 0; x < r.w; x++) {
            const uint32_t px = img[(size_t)y * r.w + x];
            if (px != line[x]) diff++;
            if (px == OSD_FG) white[row]++;
        }
    }
    CHECK_EQ(diff, 0L);
    CHECK_EQ(white[0], 0L); // Pi のモデルを出さないので、空の行
    CHECK_EQ(white[1] > 0 && white[2] > 0 && white[3] > 0 && white[5] > 0 && white[6] > 0 && white[7] > 0, true);
    CHECK_EQ(white[4], 0L); // ヒープを出さないので、空の行
    // 横 1 行の絵は renderOsdImage() と同じ (IP アドレスも入る)
    const OsdRect h = placeOsd(768, 512, 0, 0, 0, 0, 1, osdChars(v));
    CHECK_EQ(h.w, (OSD_CHARS_BASE + 16) * 8);
    std::vector<uint32_t> a((size_t)h.w * h.h), b((size_t)h.w * h.h);
    char t[OSD_CHARS_MAX + 1];
    formatOsd(t, v, osdChars(v));
    renderOsdImage(a.data(), h, t);
    renderOsd(b.data(), h, v);
    CHECK_EQ(a == b, true);
}


void testDraw(int fbW, int fbH, size_t pitch) {
    printf("draw %dx%d pitch %zu\n", fbW, fbH, pitch);
    std::vector<uint8_t> mem(pitch * fbH + 16);
    uint8_t* fb = mem.data() + (16 - ((uintptr_t)mem.data() & 15)) % 16;
    memset(fb, 0x55, pitch * fbH);
    const OsdRect r = placeOsd(fbW, fbH, 0, 0, 0, 0);
    char t[OSD_CHARS + 1];
    OsdStats s;
    s.composedDeci = 550;
    s.shownDeci = 550;
    s.speedPct = 100;
    s.tempDeciC = 480;
    formatOsd(t, packOsd(s));
    std::vector<uint32_t> img((size_t)r.w * r.h);
    renderOsdImage(img.data(), r, t);
    CheckingStore::writes = CheckingStore::misaligned = 0;
    drawOsdImage<CheckingStore>(fb, pitch, r, img.data());
    CHECK_EQ(CheckingStore::misaligned, 0L);
    // 書いたのは r の中だけで、画素は白か黒
    long white = 0, outside = 0, odd = 0;
    for (int y = 0; y < fbH; y++) {
        const uint8_t* row = fb + (size_t)y * pitch;
        for (int x = 0; x < fbW; x++) {
            uint32_t px;
            memcpy(&px, row + x * 4, 4);
            const bool in = x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
            if (!in && px != 0x55555555u) outside++;
            if (in && px == OSD_FG) white++;
            if (in && px != OSD_FG && px != OSD_BG) odd++;
        }
        for (size_t b = (size_t)fbW * 4; b < pitch; b++)
            if (row[b] != 0x55) outside++;
    }
    CHECK_EQ(outside, 0L);
    CHECK_EQ(odd, 0L);
    CHECK_EQ(white > 100, true);
    // 消すと r は黒だけ
    eraseOsd<CheckingStore>(fb, pitch, r, OSD_BG);
    CHECK_EQ(CheckingStore::misaligned, 0L);
    long notBlack = 0;
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) {
            uint32_t px;
            memcpy(&px, fb + (size_t)y * pitch + x * 4, 4);
            if (px != OSD_BG) notBlack++;
        }
    CHECK_EQ(notBlack, 0L);
}

void testState() {
    printf("state\n");
    OsdState st;
    OsdStats s;
    s.composedDeci = s.shownDeci = 550;
    const OsdValue a = packOsd(s);
    s.shownDeci = 540;
    const OsdValue b = packOsd(s);
    s.shownDeci = 550;
    s.heapMB = 52;
    const OsdValue c = packOsd(s); // a とは、OsdValue の b だけが違う
    CHECK_EQ(c.a, a.a);
    const OsdRect margin = chooseOsd(OSD_MODE_ON, 1920, 1080, 192, 28, 1536, 1024);
    const OsdRect over = placeOsd(768, 512, 0, 0, 768, 512);
    CHECK_EQ(margin.overlay, false);
    CHECK_EQ(over.overlay, true);
    // 出さない設定なら何もしない
    OsdState::Step p = st.update(false, a, margin);
    CHECK_EQ(p.draw || p.erase, false);
    // 数字がまだなければ描かない
    p = st.update(true, OsdValue(), margin);
    CHECK_EQ(p.draw || p.erase, false);
    // 余白: 最初に描き、変わらなければ描かない。変われば描く
    CHECK_EQ(st.update(true, a, margin).draw, true);
    p = st.update(true, a, margin);
    CHECK_EQ(p.draw || p.erase, false);
    CHECK_EQ(st.update(true, b, margin).draw, true);
    // OsdValue の b だけが変わっても描く
    CHECK_EQ(st.update(true, a, margin).draw, true);
    CHECK_EQ(st.update(true, c, margin).draw, true);
    CHECK_EQ(st.update(true, b, margin).draw, true);
    // 枠の外を塗り直したら描き直す
    st.invalidate();
    p = st.update(true, b, margin);
    CHECK_EQ(p.draw, true);
    CHECK_EQ(p.erase, false);
    // 出さなくしたら余白の数字を消す。2 回目は何もしない
    p = st.update(false, b, margin);
    CHECK_EQ(p.erase, true);
    CHECK_EQ(p.draw, false);
    CHECK_EQ(p.eraseRect == margin, true);
    p = st.update(false, b, margin);
    CHECK_EQ(p.draw || p.erase, false);
    // 重ねるときも、描くのは変わったときだけ (写すたびに重ねる絵を渡す)。出さなくしたら消す
    CHECK_EQ(st.update(true, a, over).draw, true);
    CHECK_EQ(st.overlaid(), true);
    p = st.update(true, a, over);
    CHECK_EQ(p.draw || p.erase, false);
    CHECK_EQ(st.overlaid(), true);
    p = st.update(false, a, over);
    CHECK_EQ(p.erase, true);
    CHECK_EQ(p.draw, false);
    CHECK_EQ(st.overlaid(), false);
    // 余白の場所が変わったら、前の場所を消して描く
    st.update(true, a, margin);
    const OsdRect moved = placeOsd(1280, 720, 256, 104, 768, 512);
    p = st.update(true, a, moved);
    CHECK_EQ(p.erase, true);
    CHECK_EQ(p.eraseRect == margin, true);
    CHECK_EQ(p.draw, true);
    CHECK_EQ(st.overlaid(), false);
    // 余白から重ねるに変わったら、余白の数字を消して重ねる
    p = st.update(true, a, over);
    CHECK_EQ(p.erase, true);
    CHECK_EQ(p.draw, true);
    CHECK_EQ(st.overlaid(), true);
}

// 重ねて写す: X68000 の画面を写すときに、重なる部分だけ数字の絵を書く。ほかの画素は重ねないときと同じで、
// 16 バイトにそろわない書き込みはない
void testOverlay(int fw, int fh, int sw, int sh, bool scaleOn) {
    printf("overlay %dx%d on %dx%d %s\n", fw, fh, sw, sh, scaleOn ? "auto" : "off");
    const FrameScale s = computeFrameScale(fw, fh, sw, sh, scaleOn, 4096);
    const OsdRect r = placeOsd(sw, sh, s.dstX, s.dstY, s.dstW, s.dstH);
    CHECK_EQ(r.overlay, true);
    std::vector<uint32_t> src((size_t)fw * fh);
    for (size_t i = 0; i < src.size(); i++) src[i] = (uint32_t)(i * 2654435761u) & 0xffffff;
    const size_t pitch = (size_t)sw * 4;
    std::vector<uint32_t> a((size_t)sw * sh + 4, 0x12345678u), b((size_t)sw * sh + 4, 0x12345678u);
    uint8_t* fa = (uint8_t*)a.data();
    uint8_t* fbb = (uint8_t*)b.data();
    std::vector<uint32_t> line(4096 + 4);
    presentScaled<CheckingStore>(src.data(), fw, fa, pitch, s, line.data());
    OsdStats st;
    st.composedDeci = st.shownDeci = 550;
    st.speedPct = 100;
    char t[OSD_CHARS + 1];
    formatOsd(t, packOsd(st));
    std::vector<uint32_t> img((size_t)r.w * r.h);
    renderOsdImage(img.data(), r, t);
    FrameOverlay ov;
    ov.pixels = img.data();
    ov.x = r.x;
    ov.y = r.y;
    ov.w = r.w;
    ov.h = r.h;
    CheckingStore::misaligned = 0;
    presentScaled<CheckingStore>(src.data(), fw, fbb, pitch, s, line.data(), &ov);
    CHECK_EQ(CheckingStore::misaligned, 0L);
    long diffOutside = 0, wrongInside = 0;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            const size_t i = (size_t)y * sw + x;
            const bool inPic = x >= s.dstX && x < s.dstX + s.dstW && y >= s.dstY && y < s.dstY + s.dstH;
            const bool inOsd = x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
            if (inPic && inOsd) {
                if (b[i] != img[(size_t)(y - r.y) * r.w + (x - r.x)]) wrongInside++;
            } else if (a[i] != b[i]) {
                diffOutside++;
            }
        }
    CHECK_EQ(diffOutside, 0L);
    CHECK_EQ(wrongInside, 0L);
}

} // namespace

// 既定の osd=on で、モニタの大きさと X68000 の画面モードごとに、どこに出るかを表にする (いちばん行の多い値: ファン、
// THROTTLED、15 文字の IP アドレス)。どの組み合わせでも出て (IP アドレスも)、フレームバッファからはみ出さない。
// X68000 の画面に重なるのは overlay のときだけ。16:9 のモニタ (720p 以上) では、重ねずに縦に並べる
// (gpu_scale=2 の 960x540 で、余白が 120 + 8 ドットに足りないときを除く)
void testCoverage() {
    printf("coverage (osd=on: V 縦に並べる、v 縦に並べて重ねる、H 横 1 行、h 横 1 行を重ねる)\n");
    OsdStats st;
    st.pi = OSD_PI_5;
    st.armMHz = 2400;
    st.mpu = 68060;
    st.mpuMHz = 75;
    st.composedDeci = st.shownDeci = 555;
    st.speedPct = 100;
    st.heapMB = 213;
    st.tempDeciC = 516;
    st.fanLevel = 1;
    st.fanRpm = 2300;
    st.throttled = true;
    st.ipv4 = 0xc0a864c8u; // 192.168.100.200
    const OsdValue v = packOsd(st);
    CHECK_EQ(osdRows(v), OSD_V_ROWS);
    struct Size { int w, h, gx, gy; const char* note; };
    const Size sizes[] = {
        {1920, 1080, 1, 1, "1080p"}, {960, 540, 2, 2, "1080p gpu_scale=2"}, {1920, 540, 1, 2, "1080p gpu_scale=1x2"},
        {1824, 984, 1, 1, "1080p overscan"}, {1280, 720, 1, 1, "720p"}, {1366, 768, 1, 1, "WXGA"},
        {1600, 900, 1, 1, "900p"}, {2560, 1440, 1, 1, "1440p"}, {3840, 2160, 1, 1, "2160p"},
        {1920, 1200, 1, 1, "16:10"}, {1680, 1050, 1, 1, "16:10"}, {1280, 800, 1, 1, "16:10"},
        {1280, 1024, 1, 1, "5:4"}, {1024, 768, 1, 1, "4:3"}, {1600, 1200, 1, 1, "4:3"}, {800, 600, 1, 1, "4:3"},
        {640, 480, 1, 1, "VGA"}, {768, 512, 1, 1, "QEMU"},
    };
    struct Mode { int w, h; };
    const Mode modes[] = {{768, 512}, {512, 512}, {256, 256}, {512, 256}, {384, 256}, {640, 480}, {1024, 1024}};
    char text[OSD_TEXT_MAX + 1];
    // video_scale=aspect では、X68000 の画面を表示の高さいっぱいの 4:3 の枠に出す (1920x1080 なら 1440x1080、
    // 右の余白 240)。その枠でも同じことを確かめる (ShowFrame() と同じく platform::videoFrameScale() の枠)
    for (int vs : {(int)VIDEO_SCALE_INTEGER, (int)VIDEO_SCALE_ASPECT})
    for (const Size& z : sizes) {
        char line[200];
        int n = snprintf(line, sizeof line, "  %4dx%-4d %-20s", z.w, z.h,
                         vs == VIDEO_SCALE_INTEGER ? z.note : "video_scale=aspect");
        bool allClear = true;
        for (const Mode& m : modes) {
            const FrameScale base = computeFrameScale(m.w, m.h, z.w, z.h, true, 4096 - 4, z.gx, z.gy);
            const FrameScale fs = videoFrameScale(vs, base, m.w, m.h, z.w, z.h, 4096 - 4, z.gx, z.gy);
            const OsdRect r = chooseOsd(OSD_MODE_ON, z.w, z.h, fs.dstX, fs.dstY, fs.dstW, fs.dstH, osdChars(v), osdRows(v));
            if (r.overlay && !(z.gx > 1 && z.gy > 1)) allClear = false;
            n += snprintf(line + n, sizeof line - n, " %4dx%-4d %c ", m.w, m.h,
                          r.w == 0 ? '-' : r.vertical ? (r.overlay ? 'v' : 'V') : (r.overlay ? 'h' : 'H'));
            // どれでも出て、倍率は 1、フレームバッファに入る
            CHECK_EQ(r.w > 0, true);
            if (r.w == 0) continue;
            CHECK_EQ(r.scale, 1);
            CHECK_EQ(r.x >= 0 && r.y >= 0 && r.x + r.w <= z.w && r.y + r.h <= z.h, true);
            CHECK_EQ(r.w * r.h <= OSD_IMAGE_PIXELS, true);
            // X68000 の画面に重なるのは overlay のときだけ
            const bool overlap =
                r.x < fs.dstX + fs.dstW && fs.dstX < r.x + r.w && r.y < fs.dstY + fs.dstH && fs.dstY < r.y + r.h;
            CHECK_EQ(overlap, r.overlay);
            // IP アドレスが入る (縦なら 9 行目、横 1 行ならいちばん右)
            if (r.vertical) {
                CHECK_EQ(r.h, OSD_V_ROWS * 16);
                formatOsdRows(text, v, r.h / 16);
                CHECK_EQ(strncmp(text + 8 * OSD_V_COLS, "192.168.100.200", OSD_V_COLS), 0);
            } else {
                formatOsd(text, v, r.w / 8);
                CHECK_EQ(strcmp(text + r.w / 8 - 15, "192.168.100.200"), 0);
            }
        }
        printf("%s\n", line);
        // 16:9 のモニタ (720p 以上。GPU で縦横とも 2 倍にするときを除く) では、どの画面モードでも重ねない
        // (video_scale=aspect の 4:3 の枠でも。1280x720 なら 960x720 の枠で右の余白 160)
        const int dw = z.w * z.gx, dh = z.h * z.gy; // 表示の大きさ
        if (dw * 9 == dh * 16 && dh >= 720) CHECK_EQ(allClear, true);
    }
}

int main() {
    testFormat();
    testFps();
    testCoverage();
    testPiRow();
    testIp();
    testIpRedraw();
    testOverlayVertical();
    testOsdBounds();
    testRpm();
    testEffMHz();
    testChars();
    testPlace();
    testRenderVertical();
    testDraw(1920, 1080, 1920 * 4);
    testDraw(1366, 768, 1366 * 4 + 8); // 16 バイトにそろわない行
    testDraw(768, 512, 768 * 4);
    testState();
    testOverlay(768, 512, 768, 512, true);
    testOverlay(512, 512, 768, 512, true);  // 512 画素の画面を 768 に (数字は画面の外にはみ出す)
    testOverlay(768, 512, 640, 480, true);  // 切り出す
    testOverlay(256, 256, 768, 512, true);  // 3x2 倍 (3 倍の line)
    return finish();
}
