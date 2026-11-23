// 起動にかかわる書き換えの警告 (platform/boot_warn.h、FloppyImage::takeBootChanged()) のテスト。中身は作ったものだけを使う
#include <stdio.h>
#include <string.h>

#include <initializer_list>
#include <vector>

#include "platform/boot_profile.h"
#include "platform/boot_warn.h"
#include "platform/osd_message.h"
#include "platform/settings.h"
#include "test_check.h"
#include "x68k/fdc.h"
#include "x68k/fdd.h"

#define CHECK(c) CHECK_EQ(!!(c), 1)

namespace {

constexpr size_t XDF = 1261568;

void command(x68k::Fdc& f, std::initializer_list<uint8_t> b) {
    for (uint8_t v : b) f.write(3, v);
}
void drain(x68k::Fdc& f) {
    for (int i = 0; i < 7; i++) f.read(3);
}
// WRITE DATA (トラックは seek() 済みのもの)。count バイトを渡して TC
void writeSector(x68k::Fdc& f, int c, int h, int r, int n, uint8_t v, int count, int op = 0x45) {
    command(f, {(uint8_t)op, (uint8_t)(h << 2), (uint8_t)c, (uint8_t)h, (uint8_t)r, (uint8_t)n, (uint8_t)r, 0x1b, 0xff});
    for (int i = 0; i < count; i++) f.write(3, v);
    f.terminalCount();
    drain(f);
}
void seek(x68k::Fdc& f, int cyl) {
    command(f, {0x0f, 0x00, (uint8_t)cyl});
    command(f, {0x08});
    f.read(3);
    f.read(3);
}

void testXdf() {
    printf("xdf\n");
    std::vector<uint8_t> d(XDF, 0x11);
    x68k::FloppyImage img;
    CHECK(img.load(d.data(), d.size(), false, "a.xdf"));
    x68k::Fdc f;
    f.reset();
    f.insert(0, &img);
    writeSector(f, 0, 0, 1, 3, 0x11, 1024); // 同じ中身
    CHECK(img.dirty());
    CHECK(!img.takeBootChanged());
    writeSector(f, 0, 0, 2, 3, 0x22, 1024); // 隣のセクタ
    writeSector(f, 0, 1, 1, 3, 0x22, 1024); // 裏面
    seek(f, 1);
    writeSector(f, 1, 0, 1, 3, 0x22, 1024); // 次のシリンダ
    CHECK_EQ(d[16 * 1024], 0x22);
    CHECK(!img.takeBootChanged());
    seek(f, 0);
    writeSector(f, 0, 0, 1, 3, 0x22, 1024);
    CHECK_EQ(d[0], 0x22);
    CHECK(img.takeBootChanged());
    CHECK(!img.takeBootChanged()); // 読むと忘れる
    writeSector(f, 0, 0, 1, 3, 0x33, 100); // 途中で TC: 残りは 0 で埋める
    CHECK(img.takeBootChanged());
    writeSector(f, 0, 0, 1, 3, 0x44, 1024, 0x49); // WRITE DELETED DATA
    CHECK(img.takeBootChanged());
    // SCAN EQUAL は書かない
    writeSector(f, 0, 0, 1, 3, 0x55, 1024, 0x51);
    CHECK_EQ(d[0], 0x44);
    CHECK(!img.takeBootChanged());
    // 書き込み禁止
    x68k::FloppyImage ro;
    CHECK(ro.load(d.data(), d.size(), true, "a.xdf"));
    f.insert(0, &ro);
    writeSector(f, 0, 0, 1, 3, 0x66, 1024);
    CHECK_EQ(d[0], 0x44);
    CHECK(!ro.takeBootChanged());
}

void testFormat() {
    printf("format\n");
    std::vector<uint8_t> d(XDF, 0xe5);
    x68k::FloppyImage img;
    CHECK(img.load(d.data(), d.size(), false, "a.xdf"));
    x68k::Fdc f;
    f.reset();
    f.insert(0, &img);
    auto format = [&](uint8_t fill) {
        command(f, {0x4d, 0x00, 0x03, 0x01, 0x54, fill});
        for (int v : {0, 0, 1, 3}) f.write(3, (uint8_t)v);
        f.terminalCount();
        drain(f);
    };
    format(0xe5); // 同じ中身
    CHECK(!img.takeBootChanged());
    format(0x00);
    CHECK_EQ(d[0], 0x00);
    CHECK(img.takeBootChanged());
    // イメージを読み直したら忘れる
    format(0x01);
    CHECK(img.load(d.data(), d.size(), false, "a.xdf"));
    CHECK(!img.takeBootChanged());
}

void testSmallSectors() {
    printf("512 byte sectors\n");
    std::vector<uint8_t> d(256 + 160 * 15 * 512, 0x11); // DIM の 2HC
    memset(d.data(), 0, 256);
    d[0] = 2;
    memset(d.data() + 1, 1, 160);
    x68k::FloppyImage img;
    CHECK(img.load(d.data(), d.size(), false, "a.dim"));
    x68k::Fdc f;
    f.reset();
    f.insert(0, &img);
    writeSector(f, 0, 0, 3, 2, 0x22, 512);
    CHECK(!img.takeBootChanged());
    writeSector(f, 0, 0, 2, 2, 0x22, 512); // FAT の先頭のことがある
    CHECK_EQ(d[256 + 512], 0x22);
    CHECK(!img.takeBootChanged());
    writeSector(f, 0, 0, 1, 2, 0x22, 512);
    CHECK(img.takeBootChanged());
}

// D88 の R = 1 が 2048 バイト: 先頭の 1024 バイトだけを見る
void testLargeSector() {
    printf("2048 byte sector (D88)\n");
    const uint32_t size = 0x2b0 + 16 + 2048;
    std::vector<uint8_t> d(size, 0);
    d[0x1b] = 0x20; // 2HD
    for (int i = 0; i < 4; i++) d[0x1c + i] = (uint8_t)(size >> (8 * i));
    d[0x20] = 0xb0, d[0x21] = 0x02; // トラック 0 の位置 ($2B0)
    uint8_t* s = d.data() + 0x2b0;
    s[2] = 1, s[3] = 4, s[4] = 1; // R、N、セクタ数
    s[0x0f] = 0x08;               // 2048 バイト
    x68k::FloppyImage img;
    CHECK(img.load(d.data(), d.size(), false, "a.d88"));
    const x68k::FloppyImage::Sector* sec = img.find(0, 0, 0, 1, 4);
    CHECK(sec != nullptr);
    if (!sec) return;
    std::vector<uint8_t> w(2048, 0);
    w[1024] = 1;
    img.noteWrite(sec, w.data(), 2048);
    CHECK(!img.takeBootChanged());
    w[1023] = 1;
    img.noteWrite(sec, w.data(), 2048);
    CHECK(img.takeBootChanged());
}

void testSram() {
    printf("sram\n");
    std::vector<uint8_t> s(0x4000, 0);
    uint32_t a = 0;
    s[0x18] = 0xb0;
    s[0x10] = 0x00, s[0x11] = 0xed, s[0x12] = 0x12, s[0x13] = 0x34;
    CHECK(!platform::sramBootsFromSram(s.data(), a)); // 識別子がない
    memcpy(s.data(), "\x82\x77" "68000W", 8);
    CHECK(platform::sramBootsFromSram(s.data(), a));
    CHECK_EQ(a, 0xed1234u);
    for (int v : {0x00, 0x80, 0x90, 0xa0}) {
        s[0x18] = (uint8_t)v;
        CHECK(!platform::sramBootsFromSram(s.data(), a));
    }
}

void testOnceAndText() {
    printf("once per image, text, setting\n");
    platform::BootWarnSeen seen;
    CHECK(seen.first("SD:/a.xdf", true));
    CHECK(!seen.first("SD:/a.xdf", true));
    CHECK(!seen.first("sd:/A.XDF", true)); // FAT は大文字と小文字を区別しない
    CHECK(seen.first("SD:/b.xdf", true));
    CHECK(!seen.first("SD:/a.xdf", true));
    platform::BootWarnSeen host;
    CHECK(host.first("/a.xdf", false));
    CHECK(host.first("/A.xdf", false));
    char name[16];
    for (int i = 2; i < platform::BootWarnSeen::MAX; i++) {
        snprintf(name, sizeof name, "%d", i);
        CHECK(seen.first(name, true));
    }
    CHECK(seen.first("one too many", true)); // いちばん古いものを忘れて覚える
    CHECK(!seen.first("one too many", true));
    CHECK(!seen.first("SD:/b.xdf", true));
    CHECK(seen.first("SD:/a.xdf", true));
    // 文が左上の 4 行に入りきる (「…」が付かない)
    for (const char* t : {platform::kBootWarnSram, platform::kBootWarnFd[0], platform::kBootWarnFd[1]}) {
        static platform::MessageLayout lay;
        platform::layoutMessage(t, strlen(t), lay);
        bool ellipsis = false;
        for (int l = 0; l < lay.lines; l++)
            for (int c = 0; c < lay.cols[l]; c++) ellipsis |= lay.cell[l][c] == platform::MSG_JIS_ELLIPSIS;
        CHECK(lay.lines > 0 && !ellipsis);
    }
    platform::Settings st;
    CHECK_EQ(st.bootWarn, 1);
    CHECK_EQ((int)platform::applySetting(st, "boot_warn", "off"), (int)platform::SettingResult::OK);
    CHECK_EQ(st.bootWarn, 0);
    CHECK(platform::isCommonSettingKey("boot_warn"));
}

} // namespace

int main() {
    testXdf();
    testFormat();
    testSmallSectors();
    testLargeSector();
    testSram();
    testOnceAndText();
    return finish();
}
