// SASI のハードディスク (x68k/sasi.h) のテスト。CPU は使わず、ポートを直接読み書きする。
// 期待する値は「Inside X68000」の SASI の章 (ポート、バスの信号とフェーズの進み方、コマンド、センスデータ) から決めた。
// センスデータのエラーコードの値は sasi.h の決め (本には値がない)。
#include <string.h>

#include "test_check.h"
#include "x68k/disk_cache.h"
#include "x68k/sasi.h"
#include "x68k/state.h"

namespace {

using x68k::HardDiskImage, x68k::Sasi; // C++17 の複数の宣言

// ポート ($E96000 からのオフセット)
enum Port : uint32_t { DATA = 1, STATUS = 3, RESET = 5, SELECT = 7 }; // STATUS に書くと SEL が戻る
// $E96003 で読める値: bit4 MSG、bit3 C/D、bit2 I/O、bit1 BSY、bit0 REQ (Inside X68000 の図 5)
const uint8_t REQ = 0x01, BSY = 0x02, IO = 0x04, CD = 0x08, MSG = 0x10;
const uint8_t kCommand = BSY | CD | REQ;        // コマンド: I/O = 0、C/D = 1、MSG = 0
const uint8_t kDataOut = BSY | REQ;             // データ (ホストから): どれも 0
const uint8_t kDataIn = BSY | IO | REQ;         // データ (ホストへ): I/O = 1
const uint8_t kStatus = BSY | CD | IO | REQ;    // ステータス: C/D = 1、I/O = 1
const uint8_t kMessage = BSY | MSG | CD | IO | REQ;

const uint32_t kBlocks = 200;
uint8_t g_disk[kBlocks * 256];

uint8_t pattern(uint32_t i) { return (uint8_t)(i * 13 + (i >> 8) * 5 + 1); }

void fillDisk() {
    for (uint32_t i = 0; i < sizeof g_disk; i++) g_disk[i] = pattern(i);
}

struct Rig {
    HardDiskImage img; // ID 0 の論理ユニット 0
    Sasi s; // ホストから見たインタフェース
    explicit Rig(bool writeProtect = false) {
        fillDisk();
        CHECK_EQ(img.load(g_disk, sizeof g_disk, Sasi::BLOCK_SIZE, writeProtect), true);
        s.reset(); // 電源を入れた状態
        s.attach(0, &img); // unit 0 = ID 0、LUN 0
    }
    uint8_t st() { return s.read(STATUS); }
    // セレクションをして、コマンドの 6 バイトを送る
    void command(const uint8_t cmd[6], uint8_t idBits = 0x01) {
        s.write(SELECT, idBits);
        s.write(STATUS, 0);
        for (int i = 0; i < 6; i++) s.write(DATA, cmd[i]);
    }
    // ステータスとメッセージを受け取る。フェーズの並びも確かめる。ステータスを返す
    uint8_t end() {
        CHECK_EQ(st(), kStatus);
        const uint8_t status = s.read(DATA);
        CHECK_EQ(st(), kMessage);
        CHECK_EQ(s.read(DATA), 0x00);
        CHECK_EQ(st(), 0);
        return status;
    }
    uint8_t simple(const uint8_t cmd[6], uint8_t idBits = 0x01) {
        command(cmd, idBits);
        return end();
    }
    // REQUEST SENSE STATUS: 4 バイトを out に
    void sense(uint8_t out[4], uint8_t lun = 0) {
        const uint8_t cmd[6] = {0x03, (uint8_t)(lun << 5), 0, 0, 0, 0};
        command(cmd);
        for (int i = 0; i < 4; i++) {
            CHECK_EQ(st(), kDataIn);
            out[i] = s.read(DATA);
        }
        CHECK_EQ(end(), 0);
    }
};

// バスフリーからバスフリーまで (Inside X68000 の「SASI のバス動作」)
void testPhases() {
    printf("phases: selection, command, status, message\n");
    Rig r;
    CHECK_EQ(r.st(), 0); // バスフリー: すべての信号が 0
    CHECK_EQ(r.s.busy(), false);
    // セレクション: ID 0 のビットをバスに出して SEL。コントローラが BSY で答える
    r.s.write(SELECT, 0x01);
    CHECK_EQ(r.st(), BSY);
    CHECK_EQ(r.s.busy(), true);
    // SEL を戻すと、コマンドを求める (C/D = 1、I/O = 0、REQ)
    r.s.write(STATUS, 0);
    CHECK_EQ(r.st(), kCommand);
    // TEST DRIVE READY: 6 バイト送る間はコマンドフェーズ、送り終えるとステータスフェーズ
    for (int i = 0; i < 5; i++) {
        r.s.write(DATA, 0);
        CHECK_EQ(r.st(), kCommand);
    }
    r.s.write(DATA, 0);
    CHECK_EQ(r.end(), 0x00);
    CHECK_EQ(r.s.busy(), false);

    // 答えるコントローラがない ID: BSY は出ず、バスフリーのまま
    r.s.write(SELECT, 0x08);
    CHECK_EQ(r.st(), 0);
    r.s.write(STATUS, 0);
    CHECK_EQ(r.st(), 0);
    r.s.write(DATA, 0x12); // バスフリーの間の書き込みは捨てる
    CHECK_EQ(r.st(), 0);
    CHECK_EQ(r.s.read(DATA), 0);

    // ID 3 の論理ユニット 1 (unit = 3 x 2 + 1) につなぐと、ID 3 が答える
    r.s.attach(7, &r.img);
    CHECK_EQ(r.s.image(7) == &r.img, true);
    const uint8_t ready1[6] = {0x00, 0x20, 0, 0, 0, 0};
    CHECK_EQ(r.simple(ready1, 0x08), 0x00);
    // 同じコントローラの論理ユニット 0 にはドライブがない: エラー (ステータスの bit1) で、センスは「ドライブが準備できていない」
    const uint8_t ready0[6] = {0x00, 0x00, 0, 0, 0, 0};
    CHECK_EQ(r.simple(ready0, 0x08), 0x02);
    r.s.detach(7);
    CHECK_EQ(r.s.image(7) == nullptr, true);
    r.s.write(SELECT, 0x08);
    CHECK_EQ(r.st(), 0);

    // 奇数番地でないところは読めない
    CHECK_EQ(r.s.read(0), 0xff);
    CHECK_EQ(r.s.read(2), 0xff);
}

void testReadWrite() {
    printf("read / write\n");
    Rig r;
    // READ: ブロック 3 から 2 ブロック。データフェーズは I/O = 1
    const uint8_t rd[6] = {0x08, 0, 0, 3, 2, 0};
    r.command(rd);
    int diff = 0;
    for (uint32_t i = 0; i < 512; i++) {
        if (r.st() != kDataIn) diff++;
        if (r.s.read(DATA) != pattern(3 * 256 + i)) diff++;
    }
    CHECK_EQ(diff, 0);
    CHECK_EQ(r.end(), 0x00);

    // WRITE: ブロック 10 から 1 ブロック。データフェーズは I/O = 0。前後のブロックは変わらない
    const uint8_t wr[6] = {0x0a, 0, 0, 10, 1, 0};
    r.command(wr);
    diff = 0;
    for (uint32_t i = 0; i < 256; i++) {
        if (r.st() != kDataOut) diff++;
        r.s.write(DATA, (uint8_t)(i ^ 0x5a));
    }
    CHECK_EQ(diff, 0);
    CHECK_EQ(r.end(), 0x00);
    diff = 0;
    for (uint32_t i = 0; i < 256; i++) diff += g_disk[10 * 256 + i] != (uint8_t)(i ^ 0x5a);
    CHECK_EQ(diff, 0);
    CHECK_EQ(g_disk[10 * 256 - 1], pattern(10 * 256 - 1));
    CHECK_EQ(g_disk[11 * 256], pattern(11 * 256));
    CHECK_EQ(r.img.dirty(), true);

    // ブロック数 0 は 256 ブロック: 200 ブロックのドライブでは範囲の外。先頭からちょうど終わりまでは読める
    const uint8_t rdAll[6] = {0x08, 0, 0, 0, 200, 0};
    r.command(rdAll);
    static uint8_t buf[kBlocks * 256];
    CHECK_EQ(r.s.readData(buf, (int)sizeof buf), (int)sizeof buf);
    CHECK_EQ(memcmp(buf, g_disk, sizeof buf), 0);
    CHECK_EQ(r.end(), 0x00);
    const uint8_t rd256[6] = {0x08, 0, 0, 0, 0, 0};
    CHECK_EQ(r.simple(rd256), 0x02);

    // readData(): 今渡せる分だけを写す。データフェーズでなければ 0
    CHECK_EQ(r.s.readData(buf, 100), 0);
    r.command(rd);
    CHECK_EQ(r.s.dataReady(), true);
    CHECK_EQ(r.s.readData(buf, 100), 100);
    CHECK_EQ(r.s.read(DATA), pattern(3 * 256 + 100));
    CHECK_EQ(r.s.readData(buf + 101, 1000), 512 - 101);
    CHECK_EQ(memcmp(buf, g_disk + 3 * 256, 100), 0);
    CHECK_EQ(memcmp(buf + 101, g_disk + 3 * 256 + 101, 512 - 101), 0);
    CHECK_EQ(r.end(), 0x00);
    CHECK_EQ(r.s.dataReady(), false);

    // 向きの違う読み書きは何もしない: WRITE のデータフェーズで読んでも進まない
    r.command(wr);
    CHECK_EQ(r.s.read(DATA), 0);
    CHECK_EQ(r.st(), kDataOut);
    for (uint32_t i = 0; i < 256; i++) r.s.write(DATA, 0x77);
    CHECK_EQ(r.end(), 0x00);
    CHECK_EQ(g_disk[10 * 256], 0x77);
}

// エラーとセンスデータ (Inside X68000 の図 11: 1 バイト目は bit7 が「番地が有効」、下位がエラーのクラスとコード。
// 続く 3 バイトは論理ユニット番号と番地)
void testErrorStatus() {
    printf("errors and sense data\n");
    Rig r;
    uint8_t sn[4];
    // エラーがなければ、センスは $00
    r.sense(sn);
    CHECK_EQ(sn[0], 0x00);
    // 範囲の外の READ: ステータスの bit1、センスは「番地が正しくない」(クラス 2 のコード 1) と、その番地
    const uint8_t far[6] = {0x08, 0x01, 0x23, 0x45, 1, 0};
    CHECK_EQ(r.simple(far), 0x02);
    r.sense(sn);
    CHECK_EQ(sn[0], 0x80 | Sasi::SENSE_INVALID_ADDRESS);
    CHECK_EQ(Sasi::SENSE_INVALID_ADDRESS, 0x21);
    CHECK_EQ(sn[1], 0x01);
    CHECK_EQ(sn[2], 0x23);
    CHECK_EQ(sn[3], 0x45);
    // 引き取ると消える
    r.sense(sn);
    CHECK_EQ(sn[0], 0x00);
    // 終わりをまたぐ READ (最後のブロックから 2 ブロック)
    const uint8_t over[6] = {0x08, 0, 0, (uint8_t)(kBlocks - 1), 2, 0};
    CHECK_EQ(r.simple(over), 0x02);
    // エラーのあと、正常に終わったコマンドがあれば、センスは消える
    const uint8_t ready[6] = {0x00, 0, 0, 0, 0, 0};
    CHECK_EQ(r.simple(ready), 0x00);
    r.sense(sn);
    CHECK_EQ(sn[0], 0x00);
    // 知らないコマンド: クラス 2 のコード 0
    const uint8_t unknown[6] = {0x1f, 0, 0, 0, 0, 0};
    CHECK_EQ(r.simple(unknown), 0x02);
    r.sense(sn);
    CHECK_EQ(sn[0], 0x20);
    // ドライブのない論理ユニット 1: ステータスに論理ユニット番号 (bit5-6) が入り、センスはクラス 0 のコード 4
    const uint8_t ready1[6] = {0x00, 0x20, 0, 0, 0, 0};
    CHECK_EQ(r.simple(ready1), 0x22);
    r.sense(sn, 1);
    CHECK_EQ(sn[0], 0x04);
    CHECK_EQ(sn[1], 0x20);
    // SEEK と RECALIBRATE
    const uint8_t seek[6] = {0x0b, 0, 0, 100, 0, 0}, seekFar[6] = {0x0b, 0, 0, (uint8_t)kBlocks, 0, 0};
    const uint8_t recal[6] = {0x01, 0, 0, 0, 0, 0};
    CHECK_EQ(r.simple(seek), 0x00);
    CHECK_EQ(r.simple(seekFar), 0x02);
    CHECK_EQ(r.simple(recal), 0x00);

    // 書き込み禁止のイメージ: WRITE と FORMAT はエラーで、中身は変わらない
    Rig p(true);
    const uint8_t wr[6] = {0x0a, 0, 0, 10, 1, 0}, fmt[6] = {0x06, 0, 0, 0, 1, 0};
    CHECK_EQ(p.simple(wr), 0x02);
    p.sense(sn);
    CHECK_EQ(sn[0], Sasi::SENSE_WRITE_FAULT);
    CHECK_EQ(p.simple(fmt), 0x02);
    CHECK_EQ(g_disk[0], pattern(0));
    CHECK_EQ(p.img.dirty(), false);
}

void testFormat() {
    printf("format track / format drive / assign drive\n");
    Rig r;
    // FORMAT TRACK: 番地を含む 1 トラック (33 ブロック) を 0 にする。ブロック 70 はトラック 2 (ブロック 66-98)
    const uint8_t track[6] = {0x06, 0, 0, 70, 1, 0};
    CHECK_EQ(r.simple(track), 0x00);
    int nonzero = 0;
    for (uint32_t i = 66 * 256; i < 99 * 256; i++) nonzero += g_disk[i] != 0;
    CHECK_EQ(nonzero, 0);
    CHECK_EQ(g_disk[66 * 256 - 1], pattern(66 * 256 - 1));
    CHECK_EQ(g_disk[99 * 256], pattern(99 * 256));
    // 最後のトラックは、ドライブの終わりまで (198 と 199 の 2 ブロック)
    const uint8_t last[6] = {0x06, 0, 0, 199, 1, 0};
    CHECK_EQ(r.simple(last), 0x00);
    CHECK_EQ(g_disk[198 * 256], 0);
    CHECK_EQ(g_disk[sizeof g_disk - 1], 0);
    CHECK_EQ(g_disk[198 * 256 - 1], pattern(198 * 256 - 1));
    const uint8_t out[6] = {0x06, 0, 0, 200, 1, 0};
    CHECK_EQ(r.simple(out), 0x02);
    // FORMAT DRIVE: 番地からドライブの終わりまで
    const uint8_t drive[6] = {0x04, 0, 0, 150, 0, 0};
    CHECK_EQ(r.simple(drive), 0x00);
    nonzero = 0;
    for (uint32_t i = 150 * 256; i < sizeof g_disk; i++) nonzero += g_disk[i] != 0;
    CHECK_EQ(nonzero, 0);
    CHECK_EQ(g_disk[150 * 256 - 1], pattern(150 * 256 - 1));
    // ASSIGN DRIVE: ドライブのパラメータ 10 バイトを受け取って、正常に終わる (ドライブがなくても受け取る)
    const uint8_t assign[6] = {0xc2, 0, 0, 0, 0, 0};
    r.command(assign);
    for (uint8_t param = 0; param < 10; param++) { // パラメータの中身は見ない
        CHECK_EQ(r.st(), kDataOut);
        r.s.write(DATA, param);
    }
    CHECK_EQ(r.end(), 0x00);
}

// 割り込みとリセット。割り込みは、ステータスフェーズに入ったときに出し、ステータスを読むか応答で消す (sasi.h の説明)
void testIrqAndReset() {
    printf("interrupt request / reset\n");
    Rig r;
    const uint8_t ready[6] = {0x00, 0, 0, 0, 0, 0};
    CHECK_EQ(r.s.irq(), false);
    r.command(ready);
    CHECK_EQ(r.s.irq(), true);
    r.s.acknowledgeInterrupt();
    CHECK_EQ(r.s.irq(), false);
    CHECK_EQ(r.end(), 0x00);
    r.command(ready);
    CHECK_EQ(r.s.irq(), true);
    CHECK_EQ(r.s.read(DATA), 0x00); // ステータスを読むと消える
    CHECK_EQ(r.s.irq(), false);
    CHECK_EQ(r.s.read(DATA), 0x00);

    // RST ($E96005): どのフェーズからでもバスフリーになり、センスも消える
    const uint8_t far[6] = {0x08, 0, 0xff, 0, 1, 0};
    CHECK_EQ(r.simple(far), 0x02);
    const uint8_t rd[6] = {0x08, 0, 0, 3, 2, 0};
    r.command(rd);
    r.s.read(DATA);
    CHECK_EQ(r.st(), kDataIn);
    r.s.write(RESET, 0);
    CHECK_EQ(r.st(), 0);
    CHECK_EQ(r.s.irq(), false);
    uint8_t sn[4];
    r.sense(sn);
    CHECK_EQ(sn[0], 0x00);

    // 転送の途中でドライブを外すと、コマンドは捨てる
    r.command(rd);
    r.s.attach(0, nullptr);
    CHECK_EQ(r.st(), 0);

    // コマンドが終わるたびに、コマンドとステータス、センスを知らせる (setTrace())
    struct Seen {
        int count = 0;
        uint8_t cmd[6] = {};
        uint8_t status = 0xff, sense = 0xff;
    } seen;
    r.s.attach(0, &r.img);
    r.s.setTrace(
        [](void* ctx, const uint8_t* cmd, uint8_t status, uint8_t sense) {
            Seen* p = (Seen*)ctx;
            p->count++;
            memcpy(p->cmd, cmd, 6);
            p->status = status;
            p->sense = sense;
        },
        &seen);
    CHECK_EQ(r.simple(far), 0x02);
    CHECK_EQ(seen.count, 1);
    CHECK_EQ(memcmp(seen.cmd, far, 6), 0);
    CHECK_EQ(seen.status, 0x02);
    CHECK_EQ(seen.sense, Sasi::SENSE_INVALID_ADDRESS);
}

// 使うチャンクだけを読むイメージ (x68k/disk_cache.h): 読み込みを待つ間は REQ を出さない
void testCached() {
    printf("cached image: no REQ until the chunk is loaded\n");
    const uint32_t CHUNK = x68k::DiskCache::CHUNK;
    static uint8_t file[8 * 16384 + 512];
    for (uint32_t i = 0; i < sizeof file; i++) file[i] = pattern(i * 3);
    static x68k::DiskCache cache;
    CHECK_EQ(cache.init(sizeof file, 4), true);
    static HardDiskImage img; // チャンクのキャッシュを通すディスク
    CHECK_EQ(img.loadCached(&cache, sizeof file, Sasi::BLOCK_SIZE, false), true);
    auto serve = [&]() {
        unsigned slot;
        uint32_t c, len;
        uint8_t* buf;
        int n = 0;
        while (cache.nextLoad(slot, c, buf, len)) {
            memcpy(buf, file + (size_t)c * CHUNK, len);
            cache.loaded(slot, true);
            n++;
        }
        return n;
    };
    Sasi s;
    s.reset();
    s.attach(0, &img);
    // READ: ブロック 60 から 10 ブロック (チャンクの境目 16384 = ブロック 64 をまたぐ)
    const uint8_t rd[6] = {0x08, 0, 0, 60, 10, 0};
    s.write(SELECT, 1);
    s.write(STATUS, 0);
    for (int i = 0; i < 6; i++) s.write(DATA, rd[i]);
    CHECK_EQ(s.read(STATUS), BSY | IO); // データフェーズだが、まだ REQ は出ない
    CHECK_EQ(s.dataReady(), false);
    int waits = 0, diff = 0;
    for (uint32_t i = 0; i < 10 * 256;) {
        if (!(s.read(STATUS) & REQ)) {
            waits++;
            if (serve() == 0) {
                printf("  no chunk was requested at byte %u\n", i);
                failures++;
                break;
            }
            continue;
        }
        diff += s.read(DATA) != file[60 * 256 + i];
        i++;
    }
    CHECK_EQ(diff, 0);
    CHECK_EQ(waits > 0, true);
    CHECK_EQ(s.read(STATUS), kStatus);
    CHECK_EQ(s.read(DATA), 0);
    CHECK_EQ(s.read(DATA), 0);
    // FORMAT TRACK も、チャンクが読めてから書く。終わるまでステータスフェーズにならない
    const uint8_t fmt[6] = {0x06, 0, 0, 33, 1, 0};
    s.write(SELECT, 1);
    s.write(STATUS, 0);
    for (int i = 0; i < 6; i++) s.write(DATA, fmt[i]);
    for (int i = 0; i < 20 && s.read(STATUS) != kStatus; i++) serve();
    CHECK_EQ(s.read(STATUS), kStatus);
    CHECK_EQ(s.read(DATA), 0);
    CHECK_EQ(s.read(DATA), 0);
    // トラック (ブロック 33-65) は 2 つのチャンクにまたがる。両方とも 0 になり、次のブロックは変わらない
    int nonzero = 0, checked = 0;
    for (uint32_t off = 33 * 256; off < 67 * 256;) {
        bool failed = false;
        uint32_t got = 0;
        uint8_t* p = img.window(off, 67 * 256 - off, got, failed);
        if (!p) {
            if (failed || serve() == 0) break;
            continue;
        }
        for (uint32_t i = 0; i < got; i++) {
            if (off + i < 66 * 256) nonzero += p[i] != 0;
            else nonzero += p[i] != file[off + i];
        }
        checked += (int)got;
        off += got;
    }
    CHECK_EQ(checked, 34 * 256);
    CHECK_EQ(nonzero, 0);
}

// ステート: コマンドの途中、データの途中で保存して、別の状態から読むと続きができる
void testState() {
    printf("state: in the middle of a command and of the data\n");
    Rig r;
    static uint8_t st[4096];
    auto save = [&](Sasi& s) {
        x68k::StateIO io(x68k::StateIO::SAVE, st, sizeof st);
        s.state(io);
        CHECK_EQ(io.ok(), true);
        return io.pos();
    };
    auto load = [&](Sasi& s, size_t n) {
        x68k::StateIO check(x68k::StateIO::CHECK, st, n);
        s.state(check);
        if (!check.ok() || check.pos() != n) return false;
        x68k::StateIO io(x68k::StateIO::LOAD, st, n);
        s.state(io);
        return io.ok();
    };
    // READ の 100 バイト目で保存
    const uint8_t rd[6] = {0x08, 0, 0, 3, 2, 0};
    r.command(rd);
    for (int i = 0; i < 100; i++) r.s.read(DATA);
    size_t n = save(r.s);
    r.s.write(RESET, 0);
    CHECK_EQ(load(r.s, n), true);
    int diff = 0;
    for (uint32_t i = 100; i < 512; i++) diff += r.s.read(DATA) != pattern(3 * 256 + i);
    CHECK_EQ(diff, 0);
    CHECK_EQ(r.end(), 0);
    // コマンドの 3 バイト目で保存
    r.s.write(SELECT, 1);
    r.s.write(STATUS, 0);
    for (int i = 0; i < 3; i++) r.s.write(DATA, rd[i]);
    n = save(r.s);
    r.s.write(RESET, 0);
    CHECK_EQ(load(r.s, n), true);
    for (int i = 3; i < 6; i++) r.s.write(DATA, rd[i]);
    CHECK_EQ(r.st(), kDataIn);
    CHECK_EQ(r.s.read(DATA), pattern(3 * 256));
    // データの途中のステートを、ドライブを外した SASI に読むと、コマンドを捨ててバスフリーになる
    n = save(r.s);
    Sasi other;
    other.reset();
    CHECK_EQ(load(other, n), true);
    CHECK_EQ(other.read(STATUS), 0);
    // イメージの大きさが保存したときと違うか、転送の範囲 (位置から 512 バイト) がイメージからはみ出すステートは、
    // 読むが、コマンドを捨ててバスフリーにする (イメージの外を読み書きしない)。並びの最後が、位置 8 バイト、
    // イメージの大きさ 8 バイト
    {
        r.s.write(RESET, 0);
        r.command(rd);
        for (int i = 0; i < 100; i++) r.s.read(DATA);
        n = save(r.s);
        static uint8_t orig[sizeof st];
        memcpy(orig, st, n);
        auto put64 = [&](size_t at, uint64_t v) {
            for (int i = 0; i < 8; i++) st[at + (size_t)i] = (uint8_t)(v >> (8 * i));
        };
        const uint64_t kBytes = sizeof g_disk;
        const struct {
            uint64_t base, image;
            bool keep;
        } kCases[] = {
            {3 * 256, kBytes, true},        // 保存したまま
            {3 * 256, kBytes + 1, false},   // イメージの大きさが違う
            {3 * 256, kBytes - 256, false},
            {kBytes - 512, kBytes, true},   // ちょうど最後まで
            {kBytes - 511, kBytes, false},  // 最後の 1 バイトがはみ出す
            {kBytes + 1, kBytes, false},
            {0xffffffffffffff00ull, kBytes, false},
            {0xfffffffffffffe00ull, 0xffffffffffffffffull, false},
        };
        for (const auto& c : kCases) {
            memcpy(st, orig, n);
            put64(n - 16, c.base);
            put64(n - 8, c.image);
            r.s.write(RESET, 0);
            CHECK_EQ(load(r.s, n), true);
            CHECK_EQ(r.st() != 0, c.keep);
            if (c.keep) {
                CHECK_EQ(r.st(), kDataIn);
                CHECK_EQ(r.s.read(DATA), pattern((uint32_t)c.base + 100));
                for (int i = 101; i < 511; i++) r.s.read(DATA);
                CHECK_EQ(r.s.read(DATA), pattern((uint32_t)c.base + 511));
                CHECK_EQ(r.end(), 0);
            } else {
                r.s.read(DATA); // 読んでも落ちない
            }
        }
        // 転送の途中のドライブをつなぎ直すと、同じイメージでも転送を捨てる
        r.s.write(RESET, 0);
        r.command(rd);
        r.s.read(DATA);
        r.s.attach(0, &r.img);
        CHECK_EQ(r.st(), 0);
    }
    // 壊れた値 (ない段階、バッファからはみ出す長さ) は読まない
    r.s.write(RESET, 0);
    n = save(r.s);
    st[4] = 0x7f; // 区切り (4 バイト) の次が段階
    CHECK_EQ(load(r.s, n), false);
    // センスデータ (16 バイトのバッファから送る) のデータインの途中: 長さはバッファまで、位置は長さより前。
    // 並びの最後から 24 バイト目が長さ、20 バイト目が位置 (どちらも 32 ビット)
    {
        r.s.write(RESET, 0);
        const uint8_t rs[6] = {0x03, 0, 0, 0, 0, 0};
        r.command(rs);
        CHECK_EQ(r.st(), kDataIn);
        r.s.read(DATA);
        n = save(r.s);
        auto put32 = [&](size_t at, uint32_t v) {
            for (int i = 0; i < 4; i++) st[at + (size_t)i] = (uint8_t)(v >> (8 * i));
        };
        const size_t kLen = n - 24, kPos = n - 20;
        CHECK_EQ(st[kLen], 4);
        CHECK_EQ(st[kPos], 1);
        const struct {
            uint32_t len, pos;
            bool ok;
        } kCases[] = {{16, 1, true}, {17, 1, false}, {0x10000, 1, false}, {4, 3, true}, {4, 4, false}, {4, 5, false}, {16, 16, false}};
        for (const auto& c : kCases) {
            put32(kLen, c.len);
            put32(kPos, c.pos);
            r.s.write(RESET, 0);
            CHECK_EQ(load(r.s, n), c.ok);
        }
        // コマンドフェーズの途中: 受け取ったバイト数は 5 まで (並びの 14 バイト目)
        r.s.write(RESET, 0);
        r.s.write(SELECT, 1);
        r.s.write(STATUS, 0);
        for (int i = 0; i < 3; i++) r.s.write(DATA, rd[i]);
        n = save(r.s);
        CHECK_EQ(st[14], 3);
        st[14] = 6;
        CHECK_EQ(load(r.s, n), false);
        st[14] = 5;
        st[8 + 3] = rd[3]; // コマンドの 4、5 バイト目 (区切りから 8 バイト目からがコマンド)
        st[8 + 4] = rd[4];
        CHECK_EQ(load(r.s, n), true);
        r.s.write(DATA, rd[5]);
        CHECK_EQ(r.st(), kDataIn);
    }
}

} // namespace

int main() { // フェーズ、転送、エラー、フォーマット、割り込み、キャッシュ、状態の保存の順
    testPhases();
    testReadWrite();
    testErrorStatus();
    testFormat();
    testIrqAndReset();
    testCached();
    testState();
    return finish();
}
