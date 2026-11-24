// src/platform/real_floppy.h のテスト: 実物のフロッピーディスクドライブからイメージを読む部分 (#286)
#include "platform/real_floppy.h"
#include "test_check.h"
#include "x68k/fdd.h"

#include <stdlib.h>
#include <string.h>

namespace {

using namespace platform;

// ブロック lba の i バイト目の中身 (ブロックごと、位置ごとに違う値)
uint8_t pattern(uint32_t lba, uint32_t i) { return (uint8_t)(lba * 7 + i * 13 + (i >> 8)); }

struct FakeDrive : RealFloppySource {
    bool present = true;
    uint32_t blockBytes = 1024, blocks = 1232;
    uint32_t bad = ~0u;      // いつも読めないブロック
    uint32_t flaky = ~0u;    // 複数まとめて読むと失敗し、1 つずつなら flakyFails 回失敗してから読めるブロック
    int flakyFails = 0;
    unsigned calls = 0, blocksRead = 0;
    uint32_t maxCount = 0;

    bool geometry(uint32_t& bb, uint32_t& n) override {
        if (!present) return false;
        bb = blockBytes;
        n = blocks;
        return true;
    }
    bool read(uint32_t lba, uint32_t count, uint8_t* dst) override {
        calls++;
        if (count > maxCount) maxCount = count;
        if (lba + count > blocks) return false;
        for (uint32_t b = lba; b < lba + count; b++) {
            if (b == bad) return false;
            if (b == flaky) {
                if (count > 1) return false;
                if (flakyFails > 0) {
                    flakyFails--;
                    return false;
                }
            }
        }
        for (uint32_t b = 0; b < count; b++)
            for (uint32_t i = 0; i < blockBytes; i++) dst[(size_t)b * blockBytes + i] = pattern(lba + b, i);
        blocksRead += count;
        return true;
    }
};

void testName() {
    CHECK_EQ(isRealFloppyName("usb-fdd:"), true);
    CHECK_EQ(isRealFloppyName("USB-FDD:"), true);
    CHECK_EQ(isRealFloppyName("usb-fdd"), false);
    CHECK_EQ(isRealFloppyName("usb-fdd:x"), false);
    CHECK_EQ(isRealFloppyName("GRADIUS.XDF"), false);
    CHECK_EQ(isRealFloppyName(""), false);
    CHECK_EQ(isRealFloppyName(nullptr), false);
    // 番号 (0 から数える)
    CHECK_EQ(realFloppyUnit("usb-fdd:"), 0);
    CHECK_EQ(realFloppyUnit("usb-fdd:0"), 0);
    CHECK_EQ(realFloppyUnit("USB-FDD:1"), 1);
    CHECK_EQ(realFloppyUnit("usb-fdd:3"), REAL_FLOPPY_UNITS - 1);
    CHECK_EQ(realFloppyUnit("usb-fdd:4"), -1);
    CHECK_EQ(realFloppyUnit("usb-fdd:12"), -1);
    CHECK_EQ(realFloppyUnit("usb-fdd:a"), -1);
    CHECK_EQ(realFloppyUnit("usb-fdd: 1"), -1);
    CHECK_EQ(realFloppyUnit("usb-fdd"), -1);
    CHECK_EQ(realFloppyUnit("fd0.xdf"), -1);
    CHECK_EQ(realFloppyUnit(""), -1);
    CHECK_EQ(realFloppyUnit(nullptr), -1);
    CHECK_EQ(isRealFloppyName("usb-fdd:0"), true);
    CHECK_EQ(isRealFloppyName("usb-fdd:3"), true);
    CHECK_EQ(isRealFloppyName("usb-fdd:4"), false);
}

void testImageName() {
    char name[REAL_FLOPPY_IMAGE_NAME_SIZE];
    memset(name, 'x', sizeof name);
    realFloppyImageName(0x602f7858u, name);
    CHECK_EQ(strcmp(name, "FD-602F7858.XDF"), 0);
    realFloppyImageName(0x602f7858u, name, true);
    CHECK_EQ(strcmp(name, "FD-602F7858-PARTIAL.XDF"), 0);
    CHECK_EQ(strlen(name) + 1, REAL_FLOPPY_IMAGE_NAME_SIZE);
    realFloppyImageName(0x0000000au, name);
    CHECK_EQ(strcmp(name, "FD-0000000A.XDF"), 0);
    realFloppyImageName(0xffffffffu, name);
    CHECK_EQ(strcmp(name, "FD-FFFFFFFF.XDF"), 0);
}

void testGeometry() {
    CHECK_EQ(RealFloppyReader::IMAGE_BYTES, 1261568);
    CHECK_EQ(RealFloppyReader::BLOCKS, 1232);
    CHECK_EQ(RealFloppyReader::blockAt(0, 1), 0);
    CHECK_EQ(RealFloppyReader::blockAt(1, 1), 8);        // シリンダ 0、ヘッド 1
    CHECK_EQ(RealFloppyReader::blockAt(76 * 2 + 1, 8), 1231);
}

// 全体を読むと XDF と同じ並びになり、FloppyImage が XDF として読める
void testReadAll() {
    FakeDrive drive;
    uint8_t* buf = (uint8_t*)malloc(RealFloppyReader::IMAGE_BYTES);
    RealFloppyReader r;
    CHECK_EQ(r.begin(&drive, buf, RealFloppyReader::IMAGE_BYTES), true);
    CHECK_EQ((int)r.status(), (int)RealFloppyReader::READING);
    CHECK_EQ(r.step(), true);
    CHECK_EQ(r.track(), 1);
    CHECK_EQ(r.readAll(), true);
    CHECK_EQ((int)r.status(), (int)RealFloppyReader::DONE);
    CHECK_EQ(r.track(), 154);
    CHECK_EQ(drive.calls, 154);
    CHECK_EQ(drive.blocksRead, 1232);
    CHECK_EQ(drive.maxCount, 8);
    CHECK_EQ(r.step(), false);

    x68k::FloppyImage image;
    CHECK_EQ(image.load(buf, RealFloppyReader::IMAGE_BYTES, true), true);
    CHECK_EQ((int)image.format(), (int)x68k::FloppyImage::XDF);
    CHECK_EQ(image.writeProtected(), true);
    // シリンダ 40、ヘッド 1、セクタ 5 の中身は、blockAt() のブロックのもの
    const int track = 40 * 2 + 1;
    const x68k::FloppyImage::Sector* s = image.find(track, 40, 1, 5, 3);
    CHECK_EQ(s != nullptr, true);
    if (s) {
        const uint32_t lba = RealFloppyReader::blockAt(track, 5);
        CHECK_EQ(s->size, 1024);
        CHECK_EQ(image.data(s)[0], pattern(lba, 0));
        CHECK_EQ(image.data(s)[1023], pattern(lba, 1023));
    }
    image.unload();
    free(buf);
}

void testRefuse() {
    uint8_t* buf = (uint8_t*)malloc(RealFloppyReader::IMAGE_BYTES);
    RealFloppyReader r;
    CHECK_EQ(r.step(), false); // begin() の前

    FakeDrive none;
    none.present = false;
    CHECK_EQ(r.begin(&none, buf, RealFloppyReader::IMAGE_BYTES), false);
    CHECK_EQ((int)r.error(), (int)RealFloppyReader::NO_DISK);
    CHECK_EQ(r.begin(nullptr, buf, RealFloppyReader::IMAGE_BYTES), false);
    CHECK_EQ((int)r.error(), (int)RealFloppyReader::NO_DISK);

    // 1.44MB (512 バイト x 2880) は読まない
    FakeDrive hd144;
    hd144.blockBytes = 512;
    hd144.blocks = 2880;
    CHECK_EQ(r.begin(&hd144, buf, RealFloppyReader::IMAGE_BYTES), false);
    CHECK_EQ((int)r.error(), (int)RealFloppyReader::BAD_FORMAT);
    CHECK_EQ(r.blockBytes(), 512);
    CHECK_EQ(r.blocks(), 2880);
    CHECK_EQ(hd144.calls, 0);
    CHECK_EQ(r.step(), false);

    // check() はバッファなしで、ドライブと形式だけを確かめる (何も読まない)
    CHECK_EQ(r.check(&none), false);
    CHECK_EQ((int)r.error(), (int)RealFloppyReader::NO_DISK);
    CHECK_EQ(r.check(&hd144), false);
    CHECK_EQ((int)r.error(), (int)RealFloppyReader::BAD_FORMAT);
    FakeDrive fine;
    CHECK_EQ(r.check(&fine), true);
    CHECK_EQ((int)r.error(), (int)RealFloppyReader::NONE);
    CHECK_EQ((int)r.status(), (int)RealFloppyReader::IDLE);
    CHECK_EQ(r.blocks(), 1232);
    CHECK_EQ(fine.calls, 0);
    CHECK_EQ(r.step(), false); // begin() していない

    FakeDrive ok;
    CHECK_EQ(r.begin(&ok, buf, RealFloppyReader::IMAGE_BYTES - 1), false);
    CHECK_EQ((int)r.error(), (int)RealFloppyReader::SMALL_BUFFER);
    CHECK_EQ(r.begin(&ok, nullptr, RealFloppyReader::IMAGE_BYTES), false);
    CHECK_EQ((int)r.error(), (int)RealFloppyReader::SMALL_BUFFER);
    CHECK_EQ(ok.calls, 0);
    free(buf);
}

void testReadError() {
    uint8_t* buf = (uint8_t*)malloc(RealFloppyReader::IMAGE_BYTES);
    // 1 つずつ読み直せば読めるブロック: 最後まで読める
    FakeDrive flaky;
    flaky.flaky = 100;
    flaky.flakyFails = RealFloppyReader::RETRIES - 1;
    RealFloppyReader r;
    CHECK_EQ(r.begin(&flaky, buf, RealFloppyReader::IMAGE_BYTES), true);
    CHECK_EQ(r.readAll(), true);
    CHECK_EQ(buf[100 * 1024 + 5], pattern(100, 5));
    CHECK_EQ(buf[103 * 1024 + 5], pattern(103, 5));

    // 読めないブロック: そこで止まり、場所が分かる (ブロック 1005 = シリンダ 62、ヘッド 1、セクタ 6)
    FakeDrive bad;
    bad.bad = 1005;
    CHECK_EQ(r.begin(&bad, buf, RealFloppyReader::IMAGE_BYTES), true);
    CHECK_EQ(r.readAll(), false);
    CHECK_EQ((int)r.status(), (int)RealFloppyReader::FAILED);
    CHECK_EQ((int)r.error(), (int)RealFloppyReader::READ_ERROR);
    CHECK_EQ(r.badBlock(), 1005);
    CHECK_EQ(r.badCylinder(), 62);
    CHECK_EQ(r.badHead(), 1);
    CHECK_EQ(r.badSector(), 6);
    CHECK_EQ(r.track(), 125);
    CHECK_EQ(r.step(), false);
    // 読み直せる
    bad.bad = ~0u;
    CHECK_EQ(r.begin(&bad, buf, RealFloppyReader::IMAGE_BYTES), true);
    CHECK_EQ(r.readAll(), true);
    free(buf);
}

} // namespace

int main() {
    testName();
    testImageName();
    testGeometry();
    testReadAll();
    testRefuse();
    testReadError();
    return finish();
}
