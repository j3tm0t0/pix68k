// FUK のディスクの読み出し (x68k/fuk_reader.h) の単体テスト。相手は偽物の装置 (fuk_fake.h)
#include <stdio.h>
#include <string.h>

#include <vector>

#include "fuk_fake.h"
#include "test_check.h"
#include "x68k/fuk_reader.h"

using x68k::Fuk;
using x68k::FukReader;

namespace {

struct DmaHost : Fuk::Host {
    uint32_t count = 0;
    uint32_t fukDmaCount() override { return count; }
    bool fukDmaSingleBlock() override { return true; }
};

struct Rig {
    FakeFuk dev;
    DmaHost host;
    Fuk fuk;
    FukReader reader;
    std::vector<uint8_t> out, original;
    std::vector<uint8_t> image; // 装置のディスクの、始める前の写し
    explicit Rig(bool swapPorts = false) : fuk(dev, &host), reader(fuk), out(FukReader::IMAGE_BYTES, 0xee) {
        dev.swapPorts = swapPorts;
        CHECK_EQ(fuk.start(1000), 1);
        dev.port1Log.clear();
        dev.commandLog.clear();
        original = dev.image;
    }
    FukReader::State run(int unit = 0, bool specify = false, uint64_t limit = 600000000ull) {
        reader.start(unit, out.data(), specify);
        return resume(limit);
    }
    FukReader::State resume(uint64_t limit = 600000000ull) {
        for (uint64_t t = 0; t < limit; t += 200) {
            const FukReader::State s = reader.poll();
            if (s != FukReader::RUNNING) break;
            dev.advance(200);
        }
        // 残りのやり取りを終わらせる
        for (int i = 0; i < 200; i++) {
            fuk.poll();
            dev.advance(200);
        }
        return reader.state();
    }
    // ディスクに書くもの、排出、転送バイト数を 1 つも送っていない
    void checkNoWrites() {
        CHECK_EQ(dev.writeCommands, 0u);
        CHECK_EQ(fuk.stats().writeFrames, 0u);
        CHECK_EQ(dev.countWrites, 0u);
        CHECK_EQ(dev.port2Unexpected, 0u);
        for (uint8_t v : dev.driveControls) CHECK_EQ(v & 0x20, 0); // 排出しない
        for (uint8_t c : dev.commandLog) CHECK_EQ(FukReader::allowed(c), 1);
        CHECK_EQ(dev.image == original, 1);
        CHECK_EQ(dev.overlappedReads, 0u);
    }
};

void expectError(Rig& r, FukReader::Error e) {
    CHECK_EQ(r.reader.state(), FukReader::FAILED);
    CHECK_EQ(r.reader.result().error, e);
    r.checkNoWrites();
}

void testAllowed() {
    // 1 バイト目の 256 通りのうち、通すのは読み取りの 7 つだけ
    unsigned n = 0;
    for (int v = 0; v < 256; v++) {
        if (!FukReader::allowed((uint8_t)v)) continue;
        n++;
        const int op = v & 0x1f;
        CHECK_EQ(op == 0x05 || op == 0x09 || op == 0x0d || op == 0x11 || op == 0x19 || op == 0x1d, 0);
        CHECK_EQ(op == 0x03 || op == 0x04 || op == 0x06 || op == 0x07 || op == 0x08 || op == 0x0a || op == 0x0f, 1);
    }
    CHECK_EQ(n, 7u);
}

void testWholeDisk() {
    for (int mode = 0; mode < 5; mode++) {
        Rig r(mode == 2);
        if (mode == 1) r.dev.eotAbnormal = true; // TC のない FDC の答え方
        if (mode == 3) r.dev.readyInt = true, r.dev.notReadySenses = 50;
        if (mode == 4) r.dev.image1 = r.dev.image;
        if (mode == 4)
            for (size_t i = 0; i < r.dev.image1.size(); i++) r.dev.image1[i] ^= 0x5a;
        const std::vector<uint8_t> image1 = r.dev.image1;
        CHECK_EQ(r.run(mode == 4 ? 1 : 0, mode == 0), FukReader::DONE);
        CHECK_EQ(r.out == (mode == 4 ? image1 : r.original), 1);
        CHECK_EQ(r.reader.tracksDone(), 154);
        CHECK_EQ(r.dev.driveControls.size(), 0u);
        CHECK_EQ(r.dev.image1 == image1, 1);
        r.checkNoWrites();
        CHECK_EQ(r.dev.lastMotor, 0); // X68000 の値 (モーター停止) に戻る
        CHECK_EQ(r.fuk.irq(), 0); // mode 3 では、モーターを止めたときの割り込みも引き取ってある
        CHECK_EQ(r.reader.result().restored, 1);
        CHECK_EQ(r.fuk.idleForReader(), 1);
        unsigned specify = 0;
        for (uint8_t c : r.dev.commandLog) specify += c == 0x03;
        CHECK_EQ(specify, mode == 0 ? 1u : 0u);
    }
}

void testRecoverableSector() {
    Rig r;
    r.dev.badSectors = {100, 1231};
    r.dev.badTimes = 4; // 読み直せば読める
    CHECK_EQ(r.run(), FukReader::DONE);
    CHECK_EQ(r.out == r.original, 1);
    CHECK_EQ(r.reader.retries() >= 2, 1);
    r.checkNoWrites();
}

void testBadSector() {
    Rig r;
    const int c = 40, h = 1, s = 6;
    r.dev.badSectors = {(uint32_t)((c * 2 + h) * 8 + (s - 1))};
    r.run();
    expectError(r, FukReader::BAD_SECTOR);
    CHECK_EQ(r.reader.result().c, c);
    CHECK_EQ(r.reader.result().h, h);
    CHECK_EQ(r.reader.result().r, s);
    char text[200];
    r.reader.describe(text, sizeof text);
    CHECK_EQ(strcmp(text, "a sector cannot be read at cylinder 40 head 1 sector 6 (result 44 20 20 28 01 06 03)"), 0);
    CHECK_EQ(r.dev.lastMotor, 0);
    // 続けてもう一度読める
    r.dev.badSectors.clear();
    CHECK_EQ(r.run(), FukReader::DONE);
    CHECK_EQ(r.out == r.original, 1);
    char small[10];
    r.reader.describe(small, sizeof small);
}

// データの長さは足りていて、ST1 だけがエラーを示す / ST2 だけ (削除データなど) が立つ
void testBadStatusOnly() {
    for (int mode = 0; mode < 2; mode++) {
        Rig r;
        r.dev.badSectors = {7}; // トラックの最後のセクタ
        r.dev.badSt2 = mode == 0 ? 0x00 : 0x40;
        r.run();
        expectError(r, FukReader::BAD_SECTOR);
        CHECK_EQ(r.reader.result().c, 0);
        CHECK_EQ(r.reader.result().r, 8);
    }
}

void testOtherFormat() {
    Rig r;
    r.dev.readIdN = 2;
    r.run();
    expectError(r, FukReader::BAD_FORMAT);
    CHECK_EQ(r.reader.result().c, 0);
}

void testNoDisk() {
    Rig r;
    r.run(1);
    expectError(r, FukReader::NOT_READY);
    CHECK_EQ(r.reader.result().restored, 1); // ヘッドを動かしていない
    for (uint8_t c : r.dev.commandLog) CHECK_EQ(c == 0x07 || c == 0x0f, 0);
}

void testDiskChanged() {
    Rig r;
    r.dev.ejectAfterReads = 20;
    r.run();
    expectError(r, FukReader::DISK_CHANGED);
    CHECK_EQ(r.fuk.fddIrq(), 1); // X68000 に残る
    // 最後の Read のあと
    Rig q;
    q.dev.ejectAfterReads = 154;
    q.run();
    expectError(q, FukReader::DISK_CHANGED);
}

void testCancel() {
    Rig r;
    r.reader.start(0, r.out.data(), false);
    while (r.reader.poll() == FukReader::RUNNING && r.reader.tracksDone() < 10) r.dev.advance(200);
    r.reader.cancel();
    r.resume();
    expectError(r, FukReader::CANCELLED);
    CHECK_EQ(r.fuk.idleForReader(), 1);
    CHECK_EQ(r.dev.lastMotor, 0);
}

void testUnplug() {
    for (int mode = 0; mode < 3; mode++) {
        Rig r;
        r.reader.start(0, r.out.data(), false);
        while (r.reader.poll() == FukReader::RUNNING && r.reader.tracksDone() < 10) r.dev.advance(200);
        if (mode == 0) r.dev.unplug();
        else if (mode == 1) r.dev.silent = true;
        else r.dev.unplug(), r.dev.plug();
        r.resume();
        expectError(r, FukReader::LINK_LOST);
        // つなぎ直せば、また読める
        r.dev.silent = false;
        if (mode == 0) r.dev.plug();
        for (int i = 0; i < 20000 && !r.fuk.idleForReader(); i++) {
            r.fuk.poll();
            r.dev.advance(200);
        }
        const FukReader::State st = r.run();
        if (st != FukReader::DONE) printf("mode %d error %d\n", mode, r.reader.result().error);
        CHECK_EQ(st, FukReader::DONE);
        CHECK_EQ(r.out == r.original, 1);
    }
    Rig n;
    n.dev.unplug();
    n.run();
    expectError(n, FukReader::NO_DEVICE);
}

void testNoAnswerToRead() {
    Rig r;
    r.dev.noReadFrame = true;
    r.run();
    expectError(r, FukReader::LINK_LOST);
}

// SEEK の終わりが来ない。時間切れで終わり、X68000 には割り込みも SEEK の途中の状態も残らない
void testSeekNeverEnds() {
    Rig r;
    r.dev.seekUs = 0x7fffffff;
    r.run();
    expectError(r, FukReader::TIMED_OUT);
    CHECK_EQ(r.fuk.irq(), 0);
    CHECK_EQ(r.fuk.idleForReader(), 1);
    r.dev.seekUs = 3000;
    CHECK_EQ(r.run(), FukReader::DONE);
    CHECK_EQ(r.out == r.original, 1);
}

// 読み終えたあと、ヘッドを戻すところで失敗した: イメージは読めているが、戻せなかったことを示す
void testRestoreFails() {
    Rig r;
    r.reader.start(0, r.out.data(), false);
    while (r.reader.poll() == FukReader::RUNNING && r.reader.tracksDone() < FukReader::TRACKS) r.dev.advance(200);
    r.dev.failSeeks = true;
    r.resume();
    CHECK_EQ(r.reader.state(), FukReader::DONE);
    CHECK_EQ(r.reader.result().restored, 0);
    CHECK_EQ(r.out == r.original, 1);
    char text[200];
    r.reader.describe(text, sizeof text);
    CHECK_EQ(strcmp(text, "done; the head position could not be restored"), 0);
    r.checkNoWrites();
    Rig q;
    q.reader.start(0, q.out.data(), false);
    while (q.reader.poll() == FukReader::RUNNING && q.reader.tracksDone() < FukReader::TRACKS) q.dev.advance(200);
    q.dev.unplug();
    q.resume();
    CHECK_EQ(q.reader.state(), FukReader::DONE);
    CHECK_EQ(q.reader.result().restored, 0);
    CHECK_EQ(q.out == q.original, 1);
}

// モーターを戻したあとの割り込みが下りない: FDC をリセットし、X68000 に割り込みを見せない
void testInterruptNeverFalls() {
    Rig r;
    r.dev.readyInt = r.dev.stickyInt = true;
    CHECK_EQ(r.run(), FukReader::DONE);
    CHECK_EQ(r.reader.result().restored, 0);
    CHECK_EQ(r.out == r.original, 1);
    CHECK_EQ(r.fuk.irq(), 0);
    CHECK_EQ(r.dev.lastMotor, 0);
}

// 最後の Read でディスクが入れ替わり、そのあとヘッドも戻せない (SEEK が終わらない): 成功にしない
void testChangedAndRestoreFails() {
    Rig r;
    r.dev.ejectAfterReads = 154;
    r.reader.start(0, r.out.data(), false);
    while (r.reader.poll() == FukReader::RUNNING && r.reader.tracksDone() < FukReader::TRACKS) r.dev.advance(200);
    r.dev.seekUs = 0x7fffffff;
    r.resume();
    expectError(r, FukReader::DISK_CHANGED);
    CHECK_EQ(r.fuk.fddIrq(), 1);
}

void testReset() { // 読んでいる途中で X68000 をリセットした
    Rig r;
    r.reader.start(0, r.out.data(), false);
    while (r.reader.poll() == FukReader::RUNNING && r.reader.tracksDone() < 10) r.dev.advance(200);
    r.fuk.reset();
    r.resume();
    expectError(r, FukReader::LINK_LOST);
}

// X68000 の操作の途中なら断り、何も送らない
void testBusy() {
    for (int mode = 0; mode < 4; mode++) {
        Rig r;
        r.fuk.write(7, 0x80);
        for (int i = 0; i < 100; i++) r.fuk.poll(), r.dev.advance(200);
        if (mode == 0) r.host.count = 1024;             // DMAC が動いている
        if (mode == 1) r.fuk.write(3, 0x46);            // コマンドの途中
        if (mode == 2) r.fuk.write(3, 0x07), r.fuk.write(3, 0x00); // SEEK の割り込みを引き取っていない
        if (mode == 3) r.fuk.setForceReady(true);
        for (int i = 0; i < 100; i++) r.fuk.poll(), r.dev.advance(200);
        const size_t sent = r.dev.commandLog.size();
        r.run();
        expectError(r, FukReader::BUSY);
        CHECK_EQ(r.dev.commandLog.size(), sent);
    }
}

// 断っても、X68000 の操作 (書き込み、読み出し) はそのまま続く
void testBusyKeepsX68000() {
    for (int mode = 0; mode < 6; mode++) {
        Rig r;
        auto wait = [&](int us) {
            for (int t = 0; t < us; t += 200) r.fuk.poll(), r.dev.advance(200);
        };
        r.fuk.write(7, 0x80);
        wait(5000);
        r.host.count = 1024;
        const bool writing = mode < 3;
        const uint8_t cmd[] = {(uint8_t)(writing ? 0x45 : 0x46), 0, 5, 0, 2, 3, 2, 0x35, 0xff};
        for (uint8_t b : cmd) r.fuk.write(3, b);
        // 0: 'W' を待つ、1: 途中まで渡した、2: 全部渡して 'w' を待つ、3: 'R' を待つ、4: 途中まで読んだ、5: リザルト待ち
        std::vector<uint8_t> got;
        auto readSome = [&](size_t n) {
            uint8_t b[1024];
            for (int t = 0; t < 100000 && got.size() < n; t += 200) {
                r.fuk.poll();
                if (r.fuk.dataReady()) {
                    const int k = r.fuk.readData(b, (int)(n - got.size()));
                    got.insert(got.end(), b, b + k);
                }
                r.dev.advance(200);
            }
        };
        int given = 0;
        auto give = [&](int n) {
            for (; given < n; given++) r.fuk.write(3, (uint8_t)(given * 3));
        };
        if (mode == 1 || mode == 2) wait(20000), give(mode == 1 ? 512 : 1024);
        if (mode == 4) readSome(512);
        if (mode == 5) readSome(1024), r.fuk.terminalCount(), wait(20000);
        // DMAC が止まっている (チャネルを止めた、まだ始めていない) ように見えても断る
        if (mode == 1) r.host.count = 0;
        r.run();
        CHECK_EQ(r.reader.state(), FukReader::FAILED);
        CHECK_EQ(r.reader.result().error, FukReader::BUSY);
        CHECK_EQ(r.fuk.stats().cancelledWrites, 0u);
        CHECK_EQ(r.fuk.stats().timeouts, 0u);
        CHECK_EQ(r.dev.resets, 1u); // 起動のときだけ
        r.host.count = 1024;
        // 続き
        if (writing) {
            for (int t = 0; t < 100000 && !r.fuk.dataReady() && given < 1024; t += 200) wait(200);
            give(1024);
            wait(100000);
            CHECK_EQ(r.dev.writeCommands, 1u);
        } else {
            readSome(1024);
            if (mode != 5) r.fuk.terminalCount();
            wait(20000);
            CHECK_EQ(got.size(), 1024u);
            CHECK_EQ(memcmp(got.data(), &r.original[(5 * 16 + 1) * 1024], 1024), 0);
        }
        CHECK_EQ(r.fuk.irq(), 1);
        CHECK_EQ(r.fuk.read(3), 0x00);
        for (int i = 0; i < 6; i++) r.fuk.read(3);
        if (writing)
            for (int i = 0; i < 1024; i++) {
                if (r.dev.image[(5 * 16 + 1) * 1024 + i] == (uint8_t)(i * 3)) continue;
                CHECK_EQ(i, 1024);
                break;
            }
        // 操作が終われば、読み出せる
        r.host.count = 0;
        wait(5000);
        CHECK_EQ(r.run(), FukReader::DONE);
        CHECK_EQ(r.out == r.dev.image, 1);
    }
}

// ヘッドを戻せなかったドライブの WRITE ID は、X68000 が SEEK / RECALIBRATE をするまで断る
void testWriteIdAfterLost() {
    for (int mode = 0; mode < 2; mode++) {
        Rig r;
        r.dev.image1 = r.dev.image;
        r.reader.start(0, r.out.data(), false);
        while (r.reader.poll() == FukReader::RUNNING && r.reader.tracksDone() < 20) r.dev.advance(200);
        if (mode == 0) r.dev.failSeeks = true; // 次の SEEK で失敗し、戻すのにも失敗する
        else r.dev.unplug();
        r.resume();
        r.dev.failSeeks = false;
        if (mode == 1) r.dev.plug();
        auto wait = [&](int us) {
            for (int t = 0; t < us; t += 200) r.fuk.poll(), r.dev.advance(200);
        };
        wait(3000000);
        CHECK_EQ(r.reader.result().restored, 0);
        r.fuk.write(7, 0x80);
        wait(5000);
        r.host.count = 32;
        auto format = [&](uint8_t unit) {
            const uint8_t cmd[] = {0x4d, unit, 3, 8, 0x54, 0xe5};
            for (uint8_t b : cmd) r.fuk.write(3, b);
            wait(20000);
        };
        r.dev.writeCommands = 0;
        format(4); // ユニット 0 のヘッド 1
        CHECK_EQ(r.dev.writeCommands, 0u);
        CHECK_EQ(r.fuk.read(3) & 0xc0, 0x40);
        for (int i = 0; i < 6; i++) r.fuk.read(3);
        // ほかのドライブは断らない
        format(1);
        CHECK_EQ(r.dev.writeCommands, 1u);
        for (int i = 0; i < 32; i++) r.fuk.write(3, (uint8_t)(i % 4 == 2 ? i / 4 + 1 : i % 4 == 3 ? 3 : 0));
        wait(100000);
        for (int i = 0; i < 7; i++) r.fuk.read(3);
        wait(5000);
        // SEEK のあとは受け付ける
        const uint8_t recal[] = {0x07, 0};
        for (uint8_t b : recal) r.fuk.write(3, b);
        wait(20000);
        r.fuk.write(3, 0x08);
        wait(20000);
        r.fuk.read(3), r.fuk.read(3);
        wait(5000);
        format(0);
        CHECK_EQ(r.dev.writeCommands, 2u);
    }
}

// 終わったあと、X68000 から見た FDC の状態 (シリンダ、モーター、DMA の Read、SENSE INTERRUPT STATUS) が続く
void testRestoresX68000() {
    Rig r;
    auto wait = [&](int us) {
        for (int t = 0; t < us; t += 200) r.fuk.poll(), r.dev.advance(200);
    };
    r.fuk.write(7, 0x80);
    wait(5000);
    const uint8_t seek[] = {0x0f, 0, 33};
    for (uint8_t b : seek) r.fuk.write(3, b);
    wait(20000);
    CHECK_EQ(r.fuk.irq(), 1);
    r.fuk.write(3, 0x08);
    wait(20000);
    CHECK_EQ(r.fuk.read(3), 0x20);
    CHECK_EQ(r.fuk.read(3), 33);
    wait(5000);
    for (int again = 0; again < 2; again++) {
        CHECK_EQ(r.run(), FukReader::DONE);
        CHECK_EQ(r.dev.cylinder(), 33);
        CHECK_EQ(r.dev.lastMotor, 0x80);
    }
    r.host.count = 1024;
    const uint8_t cmd[] = {0x46, 0, 33, 0, 2, 3, 8, 0x35, 0xff};
    for (uint8_t b : cmd) r.fuk.write(3, b);
    std::vector<uint8_t> buf;
    for (int t = 0; t < 100000 && buf.size() < 1024; t += 200) {
        r.fuk.poll();
        uint8_t b[1024];
        if (r.fuk.dataReady()) {
            const int k = r.fuk.readData(b, (int)(1024 - buf.size()));
            buf.insert(buf.end(), b, b + k);
        }
        r.dev.advance(200);
    }
    r.fuk.terminalCount();
    wait(20000);
    CHECK_EQ(buf.size(), 1024u);
    CHECK_EQ(memcmp(buf.data(), &r.original[(33 * 16 + 1) * 1024], 1024), 0);
    CHECK_EQ(r.fuk.irq(), 1);
    for (int i = 0; i < 7; i++) r.fuk.read(3);
    // 読み出しのあとも、X68000 の DMA の書き込みは FUK に届く (DMAC とのつながりが戻っている)
    wait(5000);
    const uint8_t wr[] = {0x45, 0, 33, 0, 2, 3, 2, 0x35, 0xff};
    for (uint8_t b : wr) r.fuk.write(3, b);
    wait(20000);
    CHECK_EQ(r.dev.writeCommands, 1u);
}

// 二重の守り: 読み出しの間は、許可の一覧を通らずに Fuk へ書かれた書き込みのコマンドも、FUK に届かない
void testFukRefusesWritesWhileReading() {
    Rig r;
    r.host.count = 0;
    r.fuk.beginReader(0);
    r.host.count = 1024; // X68000 の DMAC が動いていても
    const uint8_t ops[] = {0x45, 0x49, 0x4d, 0x51, 0x59, 0x5d};
    for (uint8_t op : ops) {
        const int n = (op & 0x1f) == 0x0d ? 6 : 9;
        for (int i = 0; i < n; i++) r.fuk.write(3, i == 0 ? op : 1);
        for (int i = 0; i < 100; i++) r.fuk.poll(), r.dev.advance(200);
        for (int i = 0; i < 7; i++) r.fuk.read(3);
    }
    CHECK_EQ(r.dev.commandLog.size(), 0u);
    CHECK_EQ(r.fuk.stats().refusedWrites, 6u);
    r.fuk.endReader(Fuk::ReaderHead::UNMOVED);
    r.checkNoWrites();
}

} // namespace

int main() {
    testAllowed();
    testWholeDisk();
    testRecoverableSector();
    testBadSector();
    testBadStatusOnly();
    testOtherFormat();
    testNoDisk();
    testDiskChanged();
    testCancel();
    testUnplug();
    testNoAnswerToRead();
    testSeekNeverEnds();
    testRestoreFails();
    testInterruptNeverFalls();
    testChangedAndRestoreFails();
    testReset();
    testBusy();
    testBusyKeepsX68000();
    testWriteIdAfterLost();
    testRestoresX68000();
    testFukRefusesWritesWhileReading();
    return finish();
}
