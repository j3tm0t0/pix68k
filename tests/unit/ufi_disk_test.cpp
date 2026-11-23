// platform/ufi_gate.h と platform/ufi_disk.h (USB-FDD の実物のディスクへの書き込み。#386) のテスト。実物は使わず、UFI のドライブの
// 偽物 (中身を持ち、センス、書き込み禁止、入れ替え、抜去、転送の失敗を起こせる) を相手にする
#include "platform/real_floppy.h"
#include "platform/ufd_write_config.h"
#include "platform/ufi_disk.h"
#include "test_check.h"

#include <initializer_list>
#include <stdlib.h>
#include <string.h>

using namespace platform;

namespace {

struct FakeUfi : UfiTransport {
    static constexpr size_t BYTES = (size_t)UfiDisk::BLOCKS * 1024;
    uint8_t* disk = (uint8_t*)malloc(BYTES);
    uint8_t* other = (uint8_t*)malloc(BYTES); // 入れ替えたあとのディスク
    UfiGate gate;
    int unit = 0; // Circle の ufd<unit + 1>
    uint32_t blockBytes = 1024, blocks = 1232;
    bool wp = false, modeSense = true, stallCommand = false, dead = false, shortData = false;
    int wpFromWrite = -1;       // この回数目の WRITE(10) から書き込み禁止 (MODE SENSE では分からないドライブ)
    uint8_t key = 0, asc = 0, ascq = 0; // 今のセンス
    uint8_t attention = 0;      // 次のコマンドを 06/xx で断る
    int notReady = 0;           // 残りの回数だけ 02/04 で断る
    bool noDisk = false, badSense = false;
    uint32_t badBlock = ~0u;
    int writes = 0, commands = 0, waits = 0, breakAt = -1, changeAt = -1;
    uint32_t tornBlocks = 0;    // 転送が切れたときに書けているブロック
    FakeUfi() {
        for (size_t i = 0; i < BYTES; i++) {
            disk[i] = (uint8_t)(i * 31 >> 5);
            other[i] = (uint8_t)(i * 17 >> 4);
        }
    }
    ~FakeUfi() {
        free(disk);
        free(other);
    }
    void waitMs(unsigned) override { waits++; }
    bool armWrite(uint32_t lba, uint32_t count) override { return gate.arm(unit, lba, count); }
    UfiReply reply(uint8_t k, uint8_t a, uint8_t q) {
        key = k;
        asc = a;
        ascq = q;
        UfiReply r;
        r.phase = stallCommand ? UfiReply::COMMAND : UfiReply::DONE;
        if (!stallCommand) {
            r.asc = a;
            r.ascq = q;
        }
        return r;
    }
    UfiReply done(int bytes) {
        UfiReply r;
        r.phase = UfiReply::DONE;
        r.bytes = shortData && bytes > 1 ? bytes - 1 : bytes;
        return r;
    }
    UfiReply command(const uint8_t cdb[12], void* data, size_t len, bool in) override {
        if (!gate.allow(unit, cdb, len, in)) {
            UfiReply r;
            r.phase = UfiReply::REFUSED;
            return r;
        }
        const int at = commands++;
        if (at == changeAt) {
            uint8_t* x = disk;
            disk = other;
            other = x;
            attention = 0x28;
        }
        if (dead) return UfiReply();
        uint8_t* p = (uint8_t*)data;
        const uint32_t lba = (uint32_t)cdb[2] << 24 | (uint32_t)cdb[3] << 16 | (uint32_t)cdb[4] << 8 | cdb[5];
        const uint32_t count = (uint32_t)cdb[7] << 8 | cdb[8];
        if (cdb[0] == UfiOp::REQUEST_SENSE) {
            memset(p, 0, len);
            p[0] = badSense ? 0 : 0x70;
            p[2] = key;
            p[7] = 10;
            p[12] = asc;
            p[13] = ascq;
            UfiReply r = done((int)len);
            r.asc = asc; // 前のコマンドの ASC が残るドライブ
            r.ascq = ascq;
            key = 0;
            return r;
        }
        if (attention) {
            const uint8_t a = attention;
            attention = 0;
            return reply(6, a, 0);
        }
        if (noDisk) return reply(2, 0x3a, 0);
        if (notReady > 0) {
            notReady--;
            return reply(2, 0x04, 0x01);
        }
        if (at == breakAt) {
            dead = true;
            if (cdb[0] == UfiOp::WRITE10) memcpy(disk + (size_t)lba * 1024, data, (size_t)tornBlocks * 1024);
            return UfiReply();
        }
        switch (cdb[0]) {
        case UfiOp::TEST_UNIT_READY: return done(0);
        case UfiOp::READ_CAPACITY: {
            const uint32_t v[2] = {blocks - 1, blockBytes};
            for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v[i / 4] >> (24 - 8 * (i % 4)));
            return done(8);
        }
        case UfiOp::MODE_SENSE10:
            if (!modeSense) return reply(5, 0x20, 0);
            memset(p, 0, len);
            p[2] = 0x93;
            p[3] = wp ? 0x80 : 0;
            return done((int)len);
        case UfiOp::READ10:
            if (badBlock >= lba && badBlock < lba + count) return reply(3, 0x11, 0);
            memcpy(p, disk + (size_t)lba * 1024, len);
            return done((int)len);
        case UfiOp::WRITE10:
            if (wp || (wpFromWrite >= 0 && writes >= wpFromWrite)) return reply(7, 0x27, 0);
            writes++;
            if (badBlock >= lba && badBlock < lba + count) return reply(3, 0x03, 0);
            memcpy(disk + (size_t)lba * 1024, p, len);
            return done((int)len);
        }
        return UfiReply();
    }
};

uint8_t g_buf[8192], g_in[8192];
uint8_t* g_start() {
    static uint8_t b[(size_t)UfiDisk::BLOCKS * 1024];
    return b;
}

void testGate() {
    UfiGate::enable(0);
    UfiGate::confirm(true);
    UfiGate g;
    uint8_t w[12] = {0x2a, 0, 0, 0, 1, 0x10, 0, 0, 8, 0, 0, 0}; // lba 272、8 ブロック
    CHECK_EQ(g.allow(0, w, 8192, false), false); // 許していない
    CHECK_EQ(g.arm(0, 272, 8), true);
    CHECK_EQ(g.allow(0, w, 8192, false), true);
    CHECK_EQ(g.allow(0, w, 8192, false), false); // 1 回だけ
    g.arm(0, 272, 8);
    CHECK_EQ(g.allow(0, w, 8192, true), false); // 向き
    g.arm(0, 272, 8);
    CHECK_EQ(g.allow(0, w, 4096, false), false); // 大きさ
    g.arm(0, 272, 7);
    CHECK_EQ(g.allow(0, w, 8192, false), false);
    g.arm(0, 271, 8);
    CHECK_EQ(g.allow(0, w, 8192, false), false);
    g.arm(0, 272, 8);
    uint8_t tur[12] = {};
    CHECK_EQ(g.allow(0, tur, 0, true), true); // ほかのコマンドで許しが消える
    CHECK_EQ(g.allow(0, w, 8192, false), false);
    w[1] = 0x20; // LUN 1
    g.arm(0, 272, 8);
    CHECK_EQ(g.allow(0, w, 8192, false), false);
    CHECK_EQ(g.arm(0, 1232, 1), false);
    CHECK_EQ(g.arm(0, 1231, 2), false);
    CHECK_EQ(g.arm(0, 0xffffffffu, 2), false);
    CHECK_EQ(g.arm(0, 0, 0), false);
    CHECK_EQ(g.arm(0, 0, 9), false);
    // 全部のコマンドの番号: 許しがなければ、通るのは読むだけの 5 つ
    for (int op = 0; op < 256; op++) {
        uint8_t c[12] = {(uint8_t)op};
        const bool in = op == 0x03 || op == 0x25 || op == 0x28 || op == 0x5a;
        CHECK_EQ(g.allow(0, c, 8, true), in);
        CHECK_EQ(g.allow(0, c, 8, false), false);
        CHECK_EQ(g.allow(0, c, 0, true), op == 0);
        // 許しがあっても、WRITE(10) のほかは増えない
        g.arm(0, 0, 1);
        if (op != 0x2a) CHECK_EQ(g.allow(0, c, 1024, false), false);
    }
}

void testOpen() {
    for (bool stall : {false, true}) {
        FakeUfi f;
        f.stallCommand = stall;
        f.attention = 0x29;
        f.notReady = 3;
        UfiDisk d;
        CHECK_EQ(d.open(&f), true);
        CHECK_EQ(f.waits, 3);
        CHECK_EQ(d.protect(), UfiDisk::WRITABLE);
        uint32_t bb = 0, n = 0;
        CHECK_EQ(d.geometry(bb, n), true);
        CHECK_EQ(bb, 1024u);
        CHECK_EQ(n, 1232u);
    }
    {
        UfiDisk d;
        CHECK_EQ(d.open(nullptr), false);
        CHECK_EQ(d.write(0, 1, g_buf), false); // 開いていない
    }
    {
        FakeUfi f;
        UfiDisk d;
        f.noDisk = true;
        CHECK_EQ(d.open(&f), false);
        CHECK_EQ(d.error(), UfiDisk::NOT_READY);
        CHECK_EQ(d.read(0, 1, g_buf), false);
    }
    {
        FakeUfi f;
        UfiDisk d;
        f.notReady = 1000;
        CHECK_EQ(d.open(&f), false);
        CHECK_EQ(d.error(), UfiDisk::NOT_READY);
    }
    for (uint32_t bb : {512u, 2048u}) {
        FakeUfi f;
        UfiDisk d;
        f.blockBytes = bb;
        CHECK_EQ(d.open(&f), false);
        CHECK_EQ(d.error(), UfiDisk::BAD_FORMAT);
        CHECK_EQ(d.blockBytes(), bb);
    }
    {
        FakeUfi f;
        UfiDisk d;
        f.blocks = 2880;
        CHECK_EQ(d.open(&f), false);
        CHECK_EQ(d.error(), UfiDisk::BAD_FORMAT);
    }
    {
        FakeUfi f;
        UfiDisk d;
        f.modeSense = false;
        CHECK_EQ(d.open(&f), true);
        CHECK_EQ(d.protect(), UfiDisk::UNKNOWN);
    }
    {
        FakeUfi f;
        UfiDisk d;
        f.wp = true;
        CHECK_EQ(d.open(&f), true);
        CHECK_EQ(d.protect(), UfiDisk::WRITE_PROTECTED);
        const int c = f.commands;
        CHECK_EQ(d.write(0, 1, g_buf), false);
        CHECK_EQ(d.error(), UfiDisk::PROTECTED);
        CHECK_EQ(f.commands, c); // 何も送らない
    }
    {
        FakeUfi f;
        UfiDisk d;
        f.shortData = true; // 足りないデータは失敗
        CHECK_EQ(d.open(&f), false);
        CHECK_EQ(d.error(), UfiDisk::BROKEN);
    }
    {
        FakeUfi f;
        UfiDisk d;
        f.attention = 0x28;
        f.badSense = true;
        CHECK_EQ(d.open(&f), false);
        CHECK_EQ(d.error(), UfiDisk::BROKEN);
    }
}

void testReadWrite() {
    FakeUfi f;
    UfiDisk d;
    CHECK_EQ(d.open(&f), true);
    for (size_t i = 0; i < sizeof g_buf; i++) g_buf[i] = (uint8_t)(i * 7 + 3);
    CHECK_EQ(d.write(1224, 8, g_buf), true);
    CHECK_EQ(memcmp(f.disk + 1224 * 1024, g_buf, 8192), 0);
    CHECK_EQ(d.read(1224, 8, g_in), true);
    CHECK_EQ(memcmp(g_in, g_buf, 8192), 0);
    // 範囲の外は送らない
    const int c = f.commands;
    for (uint32_t lba : {1232u, 1225u, 0xfffffff8u, 0xffffffffu}) {
        CHECK_EQ(d.write(lba, 8, g_buf), false);
        CHECK_EQ(d.read(lba, 8, g_in), false);
        CHECK_EQ(d.error(), UfiDisk::BAD_RANGE);
    }
    CHECK_EQ(d.write(0, 0, g_buf), false);
    CHECK_EQ(d.write(0, 9, g_buf), false);
    CHECK_EQ(d.write(0, 1, nullptr), false);
    CHECK_EQ(f.commands, c);
    CHECK_EQ(d.stopped(), false);
    CHECK_EQ(d.write(16, 8, g_buf), true);
    CHECK_EQ(f.writes, 2);
    // MODE SENSE では分からなかった書き込み禁止
    f.wpFromWrite = 2;
    CHECK_EQ(d.write(0, 8, g_buf), false);
    CHECK_EQ(d.error(), UfiDisk::PROTECTED);
    CHECK_EQ(d.protect(), UfiDisk::WRITE_PROTECTED);
    const int c2 = f.commands;
    CHECK_EQ(d.write(0, 8, g_buf), false);
    CHECK_EQ(f.commands, c2);
}

// 入れ替え、抜去、転送の失敗、リセットのあとは何も送らず、入れ替えたあとのディスクには書かない
void testStop() {
    for (int kind = 0; kind < 6; kind++) {
        FakeUfi f;
        UfiDisk d;
        CHECK_EQ(d.open(&f), true);
        CHECK_EQ(d.write(0, 8, g_buf), true);
        uint8_t* keep = (uint8_t*)malloc(FakeUfi::BYTES);
        memcpy(keep, f.other, FakeUfi::BYTES);
        UfiDisk::Error want = UfiDisk::CHANGED;
        switch (kind) {
        case 0: f.changeAt = f.commands; break;
        case 1: f.attention = 0x29; break;
        case 2:
            f.breakAt = f.commands;
            f.tornBlocks = 3;
            want = UfiDisk::BROKEN;
            break;
        case 3:
            f.noDisk = true;
            want = UfiDisk::NOT_READY;
            break;
        case 5:
            f.badBlock = 12;
            want = UfiDisk::MEDIUM;
            break;
        case 4:
            f.shortData = true;
            want = UfiDisk::BROKEN;
            break;
        }
        CHECK_EQ(d.write(8, 8, g_buf), false);
        CHECK_EQ(d.error(), want);
        CHECK_EQ(d.stopped(), true);
        const int c = f.commands, w = f.writes;
        f.noDisk = f.shortData = f.dead = false;
        CHECK_EQ(d.write(16, 8, g_buf), false);
        CHECK_EQ(d.read(16, 8, g_in), false);
        CHECK_EQ(d.ready(), false);
        CHECK_EQ(d.open(&f), false); // 開き直せない
        CHECK_EQ(d.write(16, 8, g_buf), false);
        uint32_t bb, n;
        CHECK_EQ(d.geometry(bb, n), false);
        CHECK_EQ(d.error(), want);
        CHECK_EQ(f.commands, c);
        CHECK_EQ(f.writes, w);
        if (kind == 0) CHECK_EQ(memcmp(f.disk, keep, FakeUfi::BYTES), 0);
        free(keep);
    }
}

// ドライブが自分で直した (センスキー 1) ときは成功。覚えのないセンスは止める
void testSense() {
    struct Odd final : UfiTransport {
        uint8_t key = 0;
        bool armed = false;
        FakeUfi f;
        bool odd = false;
        void waitMs(unsigned) override {}
        bool armWrite(uint32_t l, uint32_t c) override { return f.armWrite(l, c); }
        UfiReply command(const uint8_t cdb[12], void* p, size_t len, bool in) override {
            if (odd && cdb[0] == UfiOp::REQUEST_SENSE) {
                f.key = key;
                f.asc = 0x18;
                return f.command(cdb, p, len, in);
            }
            UfiReply r = f.command(cdb, p, len, in);
            if (odd) r.asc = 0x18;
            return r;
        }
    };
    for (int key = 0; key < 16; key++) {
        Odd o;
        UfiDisk d;
        CHECK_EQ(d.open(&o), true);
        o.odd = true;
        o.key = (uint8_t)key;
        CHECK_EQ(d.write(0, 8, g_buf), key == 1);
        CHECK_EQ(d.stopped(), key != 1 && key != 5 && key != 7);
    }
}

} // namespace

// 閉じているときは、WRITE(10) が 1 つもドライブに届かない: キーなし、値が不正、別のドライブ、確認の前と後
void testClosed() {
    for (int c = 0; c < 8; c++) {
        FakeUfi f;
        memcpy(g_start(), f.disk, FakeUfi::BYTES);
        UfiGate::enable(-1);
        UfiDisk d;
        const char* value = c == 0 ? "" : c == 1 ? "off" : c == 2 ? "on" : c == 3 ? "usb-fdd:" : c == 4 ? "fd0" : c == 5 ? "usb-fdd:1" : "usb-fdd:0";
        UfiGate::enable(usbFddWriteUnit(value));
        if (c != 6) UfiGate::confirm(true); // 6: 確認の前
        const bool opened = d.open(&f);
        CHECK_EQ(opened, c >= 6);
        if (c == 7) {
            UfiGate::confirm(false); // 7: 書き終わって閉じたあと
            CHECK_EQ(UfiGate::enabledUnit(), 0);
        }
        CHECK_EQ(d.write(8, 8, g_buf), false);
        if (opened) CHECK_EQ(d.error(), UfiDisk::NOT_ALLOWED);
        CHECK_EQ(f.writes, 0);
        if (c < 6) CHECK_EQ(f.commands, 0);
        // UfiDisk を通さず、許しを求めてから送っても断る
        uint8_t w[12] = {0x2a, 0, 0, 0, 0, 8, 0, 0, 8};
        CHECK_EQ(f.armWrite(8, 8), false);
        CHECK_EQ((int)f.command(w, g_buf, 8192, false).phase, (int)UfiReply::REFUSED);
        CHECK_EQ(f.writes, 0);
        CHECK_EQ(memcmp(g_start(), f.disk, FakeUfi::BYTES), 0);
    }
    // 許したあとで確認を取り消せば、届かない
    UfiGate::enable(0);
    UfiGate::confirm(true);
    UfiGate g;
    uint8_t w[12] = {0x2a, 0, 0, 0, 0, 8, 0, 0, 8};
    CHECK_EQ(g.arm(0, 8, 8), true);
    UfiGate::confirm(false);
    CHECK_EQ(g.allow(0, w, 8192, false), false);
    static const char* const kBad[] = {"on", "usb-fdd:", "usb-fdd:4", "usb-fdd:0x", "usb-fdd:-1", "fd0", "0", "experimental", " usb-fdd:0"};
    for (const char* v : kBad) CHECK_EQ(usbFddWriteUnit(v), -2);
    CHECK_EQ(usbFddWriteUnit("usb-fdd:3"), 3);
    CHECK_EQ(usbFddWriteUnit("OFF"), -1);
    CHECK_EQ(usbFddWriteUnit("Off"), -1);
    CHECK_EQ(usbFddWriteUnit("offs"), -2);
    for (int u = -2; u < 6; u++) {
        UfiGate::enable(u);
        CHECK_EQ(UfiGate::enabledUnit(), u >= 0 && u < 4 ? u : -1);
    }
    UfiGate::enable(-1);
    UfiGate::confirm(true);
    CHECK_EQ(UfiGate::enabled(-1), false);
    CHECK_EQ(g.arm(-1, 0, 8), false);
}

// pix68k.cfg そのものを読めたときだけ許す
void testConfig() {
    static const char kCfg[] = "SD:/pix68k/pix68k.cfg", kNew[] = "SD:/pix68k/pix68k.new";
    static const char kOn[] = "ram=8\nexperimental_usb_fdd_write = usb-fdd:1 # x\n";
    char v[ConfigText::MAX_LINE];
    ConfigText on, none;
    on.load(kOn, sizeof kOn - 1);
    none.load("ram=8\n", 6);
    CHECK_EQ(usbFddWriteUnitFrom(true, kCfg, kCfg, on, v, sizeof v), 1);
    CHECK_EQ(strcmp(v, "usb-fdd:1"), 0);
    CHECK_EQ(usbFddWriteUnitFrom(false, kCfg, kCfg, on, v, sizeof v), -1); // 読めない
    CHECK_EQ(usbFddWriteUnitFrom(true, kNew, kCfg, on, v, sizeof v), -1);  // 書き直しの残り
    CHECK_EQ(usbFddWriteUnitFrom(true, "SD:/pix68k/a.cfg", kCfg, on, v, sizeof v), -1);
    CHECK_EQ(usbFddWriteUnitFrom(true, nullptr, kCfg, on, v, sizeof v), -1);
    CHECK_EQ(usbFddWriteUnitFrom(true, kCfg, kCfg, none, v, sizeof v), -1); // 組み合わせのファイルにだけ書いたとき
    CHECK_EQ(v[0], 0);
    static const char kTwice[] = "experimental_usb_fdd_write=usb-fdd:1\nexperimental_usb_fdd_write=on\n"; // あとの行が勝つ
    on.load(kTwice, sizeof kTwice - 1);
    CHECK_EQ(usbFddWriteUnitFrom(true, kCfg, kCfg, on, v, sizeof v), -2);
}

int main() {
    CHECK_EQ(UfiGate::enabledUnit(), -1); // 何もしなければ閉じている
    testClosed();
    testConfig();
    testGate();
    testOpen();
    testReadWrite();
    testStop();
    testSense();
    return finish();
}
