// SCSI の CD-ROM 装置のテスト: CUE シート (platform/cue_sheet.h)、トラックの並び、MODE SENSE のページ、
// 媒体の入れ替え、オーディオの再生のコマンド (x68k/spc.cpp、x68k/cd_drive.h) と、再生した音が Sound の出力に出ること。
// イメージはテストの中で作る (データトラックは 2352 バイト / セクタの MODE1、オーディオは決まった PCM)。
// CPU は使わず、SPC のレジスタを直接読み書きする。音の出力は、中身のない IPL ROM の Machine を動かして調べる。
// X68000 の CD-ROM のドライバは手元にないので、ドライバが出すコマンドの順では確かめていない
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <initializer_list>

#include "old_state_data.h"
#include "platform/cue_sheet.h"
#include "x68k/cd_drive.h"
#include "x68k/machine.h"
#include "x68k/sound_replay.h"
#include "x68k/spc.h"
#include "x68k/state.h"

namespace {

int failures = 0;

#define CHECK_EQ(a, b)                                                                                 \
    do {                                                                                               \
        unsigned long long va_ = (unsigned long long)(a), vb_ = (unsigned long long)(b);              \
        if (va_ != vb_) {                                                                              \
            printf("  %s:%d: %s == %s: got %llx, want %llx\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
            failures++;                                                                                \
        }                                                                                              \
    } while (0)

using x68k::CdDrive;
using x68k::CdToc;
using x68k::Spc;

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// ---------------------------------------------------------------- テスト用のイメージ
//
// 1 つの BIN (2352 バイト / セクタ、210 セクタ):
//   トラック 1: MODE1/2352、セクタ 0-39 (ユーザデータはセクタの +16 から 2048 バイト)
//   トラック 2: AUDIO、INDEX 00 がセクタ 40 (プリギャップ 10 セクタ)、INDEX 01 がセクタ 50
//   トラック 3: AUDIO、INDEX 01 がセクタ 150 (00:02:00)
const uint32_t kSectors = 210;
const uint32_t kData = 40, kTrack2 = 50, kTrack3 = 150;
uint8_t g_bin[kSectors * 2352];
const char kCue[] = "REM test image\r\n"
                    "FILE \"TEST.BIN\" BINARY\r\n"
                    "  TRACK 01 MODE1/2352\r\n"
                    "    INDEX 01 00:00:00\r\n"
                    "  TRACK 02 AUDIO\r\n"
                    "    FLAGS DCP\r\n"
                    "    INDEX 00 00:00:40\r\n"
                    "    INDEX 01 00:00:50\r\n"
                    "  TRACK 03 AUDIO\r\n"
                    "    INDEX 01 00:02:00\r\n";

// ユーザデータのバイト (論理ブロック lba の i バイト目)
uint8_t dataByte(uint32_t lba, uint32_t i) { return (uint8_t)(lba * 7 + i * 3 + (i >> 8)); }
// オーディオのフレーム (論理ブロック lba の f フレーム目) の左と右
int16_t pcmLeft(uint32_t lba, uint32_t f) { return (int16_t)((lba * 588 + f) * 37); }
int16_t pcmRight(uint32_t lba, uint32_t f) { return (int16_t)((lba * 588 + f) * 53 + 11); }

void makeBin() {
    for (uint32_t s = 0; s < kSectors; s++) {
        uint8_t* p = g_bin + s * 2352;
        if (s < kData) {
            memset(p, 0xee, 2352); // 同期、ヘッダ、EDC / ECC のところ (読まれたら分かる値)
            for (uint32_t i = 0; i < 2048; i++) p[16 + i] = dataByte(s, i);
        } else {
            for (uint32_t f = 0; f < 588; f++) {
                const uint16_t l = (uint16_t)pcmLeft(s, f), r = (uint16_t)pcmRight(s, f);
                p[4 * f] = (uint8_t)l;
                p[4 * f + 1] = (uint8_t)(l >> 8);
                p[4 * f + 2] = (uint8_t)r;
                p[4 * f + 3] = (uint8_t)(r >> 8);
            }
        }
    }
}

bool makeToc(CdToc& toc) {
    static platform::CueSheet cue;
    if (!cue.parse(kCue, sizeof kCue - 1)) return false;
    const uint64_t base = 0, bytes = sizeof g_bin;
    return cue.layout(&base, &bytes, toc);
}

// ---------------------------------------------------------------- SPC の操作 (spc_test.cpp と同じ手順)

// 転送カウンタ (24 ビット) を、上位のバイトから TCH、TCM、TCL に書く (3 つのレジスタは続いた番号)
void setTc(Spc& s, uint32_t count) {
    for (int k = 0, shift = 16; k < 3; k++, shift -= 8) s.write(Spc::TCH + k, (uint8_t)(count >> shift));
}

// INTS を読み、立っている要因を書き戻して消す
uint8_t takeInts(Spc& s) {
    const uint8_t pending = s.read(Spc::INTS);
    s.write(Spc::INTS, pending);
    return pending;
}

// ID 7 から id をセレクションする。相手が応答すれば (コマンド終了の割り込み) true
bool select(Spc& s, int id) {
    s.write(Spc::PCTL, 0);
    s.write(Spc::TEMP, (uint8_t)(0x80 | 1 << id));
    setTc(s, 0x0f0003); // 応答を待つ時間
    s.write(Spc::SCMD, Spc::CMD_SELECT);
    return takeInts(s) == Spc::INTS_COMPLETE;
}

// フェーズと長さを決めて、プログラム転送の Transfer コマンドを出す
void startTransfer(Spc& s, uint8_t phase, uint32_t n) {
    s.write(Spc::PCTL, phase);
    setTc(s, n);
    const uint8_t programTransfer = Spc::CMD_TRANSFER | Spc::SCMD_PT; // DREG を CPU が読み書きする
    s.write(Spc::SCMD, programTransfer);
}

// プログラム転送で n バイト送る。終わったときの割り込みの要因を返す
uint8_t transferOut(Spc& s, uint8_t phase, const uint8_t* bytes, uint32_t count) {
    startTransfer(s, phase, count);
    for (const uint8_t* end = bytes + count; bytes != end; ++bytes) {
        if (!(s.read(Spc::SSTS) & Spc::SSTS_BUSY)) break; // ターゲットがフェーズを変えた
        s.write(Spc::DREG, *bytes);
    }
    return takeInts(s);
}

uint8_t transferIn(Spc& s, uint8_t phase, uint8_t* data, uint32_t n, uint32_t* got) {
    startTransfer(s, phase, n);
    uint32_t count = 0;
    // FIFO が空になったら、ターゲットが送り終えたかフェーズを変えた
    for (; count < n && (s.read(Spc::SSTS) & Spc::SSTS_EMPTY) == 0; count++) data[count] = s.read(Spc::DREG);
    if (got) *got = count; // 受け取ったバイト数 (要らなければ nullptr)
    return takeInts(s);
}

uint8_t manualIn(Spc& s) {
    CHECK_EQ(s.read(Spc::PSNS) & Spc::PSNS_REQ, Spc::PSNS_REQ);
    const uint8_t v = s.read(Spc::TEMP); // REQ が来ているときのバスのデータ
    s.write(Spc::SCMD, Spc::CMD_SET_ACK); // ACK を出してから下げると、ターゲットが次へ進む
    s.write(Spc::SCMD, Spc::CMD_RESET_ACK);
    return v;
}

// コマンドを送り、データインを n バイトまで受け取って、ステータスを返す (セレクションできなければ -1)
int runCommand(Spc& s, int id, std::initializer_list<uint8_t> cdb, uint8_t* data = nullptr, uint32_t n = 0,
               uint32_t* got = nullptr) {
    if (got) *got = 0;
    if (!select(s, id)) return -1;
    uint8_t buf[12];
    int len = 0;
    for (uint8_t b : cdb) buf[len++] = b;
    CHECK_EQ(s.read(Spc::PSNS), Spc::PSNS_REQ | Spc::PSNS_BSY | Spc::PH_COMMAND); // ATN なし
    CHECK_EQ(transferOut(s, Spc::PH_COMMAND, buf, (uint32_t)len), Spc::INTS_COMPLETE);
    if (n && (s.read(Spc::PSNS) & 7) == Spc::PH_DATA_IN) transferIn(s, Spc::PH_DATA_IN, data, n, got);
    CHECK_EQ(s.read(Spc::PSNS), Spc::PSNS_REQ | Spc::PSNS_BSY | Spc::PH_STATUS); // データのあとはステータス
    const int st = manualIn(s);
    CHECK_EQ(manualIn(s), 0);
    CHECK_EQ(s.read(Spc::PSNS), 0);
    return st;
}

void initSpc(Spc& s) {
    s.reset();
    s.write(Spc::SCTL, Spc::SCTL_RESET); // リセットの状態で ID を決める
    s.write(Spc::BDID, 7); // 自分の ID は 7
    s.write(Spc::SCTL, 0x11);
    s.write(Spc::INTS, 0xff);
}

// CUE のイメージを ID 6 につないだ SPC
struct Fixture {
    x68k::HardDiskImage img;
    CdToc toc;
    Spc s;
    uint8_t buf[4096];
    uint32_t got = 0;
    Fixture() {
        makeBin();
        CHECK_EQ(makeToc(toc), true);
        CHECK_EQ(img.load(g_bin, sizeof g_bin, 1, true), true);
        s.attach(6, &img, true, &toc);
        initSpc(s);
    }
    int run(std::initializer_list<uint8_t> cdb, uint32_t n = 0) { return runCommand(s, 6, cdb, buf, n, &got); }
    // REQUEST SENSE をして、センスキー << 16 | 追加センスコード << 8 | クオリファイアを返す
    uint32_t sense() {
        CHECK_EQ(run({0x03, 0, 0, 0, 18, 0}, 18), 0);
        return (uint32_t)buf[2] << 16 | (uint32_t)buf[12] << 8 | buf[13];
    }
    CdDrive& cd() { return *s.cd(); }
    // READ SUB-CHANNEL (今の位置、LBA) をして、オーディオステータスを返す
    int status() {
        CHECK_EQ(run({0x42, 0, 0x40, 0x01, 0, 0, 0, 0, 16, 0}, 16), 0);
        return buf[1];
    }
};

// ---------------------------------------------------------------- CUE シート

void testCue() {
    printf("cue sheet\n");
    CdToc toc;
    CHECK_EQ(makeToc(toc), true);
    CHECK_EQ(toc.count, 3);
    CHECK_EQ(toc.leadOut, kSectors);
    CHECK_EQ(toc.tracks[0].number, 1);
    CHECK_EQ(toc.tracks[0].control, 0x04);
    CHECK_EQ(toc.tracks[0].sector, 2352);
    CHECK_EQ(toc.tracks[0].dataOffset, 16);
    CHECK_EQ(toc.tracks[0].start, 0);
    CHECK_EQ(toc.tracks[0].fileEnd, kData);
    CHECK_EQ(toc.tracks[1].control, 0x02); // FLAGS DCP
    CHECK_EQ(toc.tracks[1].begin, kData);
    CHECK_EQ(toc.tracks[1].fileLba, kData);
    CHECK_EQ(toc.tracks[1].start, kTrack2);
    CHECK_EQ(toc.tracks[1].offset, (uint64_t)kData * 2352);
    CHECK_EQ(toc.tracks[2].begin, kTrack3);
    CHECK_EQ(toc.tracks[2].start, kTrack3);
    CHECK_EQ(toc.tracks[2].fileEnd, kSectors);
    CHECK_EQ(toc.tracks[2].offset, (uint64_t)kTrack3 * 2352);
    CHECK_EQ(toc.find(0), 0);
    CHECK_EQ(toc.find(kData), 1);
    CHECK_EQ(toc.find(kTrack3 - 1), 1);
    CHECK_EQ(toc.find(kSectors - 1), 2);
    CHECK_EQ(toc.find(kSectors), -1);

    // ファイルが複数: ISO (2048 バイト / セクタ) と WAV 2 つ。PREGAP はファイルに入っていない無音、
    // 引用符のない名前、小文字の語
    static platform::CueSheet cue;
    const char multi[] = "FILE \"GAME.ISO\" BINARY\n"
                         "TRACK 01 MODE1/2048\n"
                         "INDEX 01 00:00:00\n"
                         "file track 02.wav wave\n"
                         "track 02 audio\n"
                         "pregap 00:02:00\n"
                         "index 01 00:00:00\n"
                         "FILE \"T3.WAV\" WAVE\n"
                         "TRACK 03 AUDIO\n"
                         "INDEX 00 00:00:00\n"
                         "INDEX 01 00:01:00\n"
                         "POSTGAP 00:00:10\n";
    CHECK_EQ(cue.parse(multi, sizeof multi - 1), true);
    CHECK_EQ(cue.fileCount(), 3);
    CHECK_EQ(cue.trackCount(), 3);
    char name[64];
    CHECK_EQ(cue.fileName(1, name, sizeof name), true);
    CHECK_EQ(strcmp(name, "track 02.wav"), 0);
    CHECK_EQ(cue.fileName(2, name, sizeof name), true);
    CHECK_EQ(strcmp(name, "T3.WAV"), 0);
    CHECK_EQ(cue.fileName(2, name, 6), false);
    CHECK_EQ(cue.fileType(0), platform::CueSheet::BINARY);
    CHECK_EQ(cue.fileType(1), platform::CueSheet::WAVE);
    // つなげたイメージの中の位置: ISO 100 セクタ、WAV (ヘッダ 44 バイト) 200 セクタ、WAV 300 セクタ
    const uint64_t base[3] = {0, 100 * 2048 + 44, 100 * 2048 + 44 + 200 * 2352 + 44};
    const uint64_t bytes[3] = {100 * 2048, 200 * 2352, 300 * 2352};
    CHECK_EQ(cue.layout(base, bytes, toc), true);
    CHECK_EQ(toc.count, 3);
    CHECK_EQ(toc.tracks[0].sector, 2048);
    CHECK_EQ(toc.tracks[0].fileEnd, 100);
    CHECK_EQ(toc.tracks[1].begin, 100);   // PREGAP の 150 セクタは無音
    CHECK_EQ(toc.tracks[1].fileLba, 250);
    CHECK_EQ(toc.tracks[1].start, 250);
    CHECK_EQ(toc.tracks[1].fileEnd, 450);
    CHECK_EQ(toc.tracks[1].offset, base[1]);
    CHECK_EQ(toc.tracks[2].begin, 450);
    CHECK_EQ(toc.tracks[2].fileLba, 450);
    CHECK_EQ(toc.tracks[2].start, 525);
    CHECK_EQ(toc.tracks[2].fileEnd, 750);
    CHECK_EQ(toc.leadOut, 760); // POSTGAP の 10 セクタ
    // トラックが入りきらないファイル
    const uint64_t shortBytes[3] = {100 * 2048, 200 * 2352, 50 * 2352};
    CHECK_EQ(cue.layout(base, shortBytes, toc), false);

    // 読めない書き方
    auto bad = [&](const char* text) { return cue.parse(text, strlen(text)); };
    CHECK_EQ(bad(""), false);
    CHECK_EQ(bad("TRACK 01 AUDIO\nINDEX 01 00:00:00\n"), false);                         // FILE がない
    CHECK_EQ(bad("FILE \"A.BIN\" BINARY\nTRACK 01 AUDIO\n"), false);                     // INDEX 01 がない
    CHECK_EQ(bad("FILE \"A.MP3\" MP3\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"), false);     // 扱わない種類
    CHECK_EQ(bad("FILE \"A.BIN\" BINARY\nTRACK 01 CDG\nINDEX 01 00:00:00\n"), false);
    CHECK_EQ(bad("FILE \"A.BIN\" BINARY\nTRACK 01 AUDIO\nINDEX 01 00:61:00\n"), false);  // 秒が 60 以上
    CHECK_EQ(bad("FILE \"A.BIN\" BINARY\nTRACK 02 AUDIO\nINDEX 01 00:00:00\nTRACK 01 AUDIO\nINDEX 01 00:01:00\n"), false);
    // 時刻が大きすぎる: 954437:10:46 は 2^32 フレームちょうど (32 ビットで計算すると 0 になる)。使えるのは 24 ビットまで
    CHECK_EQ(bad("FILE \"A.BIN\" BINARY\nTRACK 01 AUDIO\nINDEX 01 954437:10:46\n"), false);
    CHECK_EQ(bad("FILE \"A.BIN\" BINARY\nTRACK 01 AUDIO\nINDEX 01 3728:16:16\n"), false); // 0x1000000 フレーム
    CHECK_EQ(bad("FILE \"A.BIN\" BINARY\nTRACK 01 AUDIO\nINDEX 01 3728:16:15\n"), true);  // 0xffffff フレーム
    CHECK_EQ(bad("FILE \"A.BIN\" BINARY\nTRACK 01 AUDIO\nPREGAP 954437:10:46\nINDEX 01 00:00:00\n"), false);
    CHECK_EQ(bad("FILE \"A.BIN\" BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:75\n"), false); // フレームが 75 以上
    CHECK_EQ(bad("\xef\xbb\xbf" "FILE \"A.BIN\" BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"), true); // BOM

    // WAV のヘッダ
    uint8_t wav[64] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
                       1,   0,   2,   0,   0x44, 0xac, 0, 0, 0x10, 0xb1, 2, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a',
                       0x00, 0x10, 0, 0};
    uint32_t off = 0;
    uint64_t len = 0;
    const char* err = nullptr;
    CHECK_EQ(platform::wavData(wav, 44, 44 + 0x1000, off, len, err), true);
    CHECK_EQ(off, 44);
    CHECK_EQ(len, 0x1000);
    CHECK_EQ(platform::wavData(wav, 44, 44 + 0x800, off, len, err), true); // 短いファイルは、あるだけ
    CHECK_EQ(len, 0x800);
    wav[24] = 0x80; // 48kHz
    wav[25] = 0xbb;
    CHECK_EQ(platform::wavData(wav, 44, 44 + 0x1000, off, len, err), false);
    wav[0] = 'X';
    CHECK_EQ(platform::wavData(wav, 44, 44 + 0x1000, off, len, err), false);
}

// ---------------------------------------------------------------- トラックの並びとデータの読み出し

void testTocAndRead() {
    printf("read toc / read from 2352-byte sectors\n");
    Fixture f;
    // READ CAPACITY: 最後の論理ブロックと 2048
    CHECK_EQ(f.run({0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 8), 0);
    CHECK_EQ(be32(f.buf), kSectors - 1);
    CHECK_EQ(be32(f.buf + 4), 2048);
    // READ TOC (LBA): 3 トラックとリードアウト
    CHECK_EQ(f.run({0x43, 0, 0, 0, 0, 0, 0, 0x03, 0x24, 0}, 804), 0);
    CHECK_EQ(f.got, 4 + 8 * 4);
    CHECK_EQ(f.buf[0] << 8 | f.buf[1], 2 + 8 * 4);
    CHECK_EQ(f.buf[2], 1);
    CHECK_EQ(f.buf[3], 3);
    CHECK_EQ(f.buf[5], 0x14); // データ
    CHECK_EQ(f.buf[6], 1);
    CHECK_EQ(be32(f.buf + 8), 0);
    CHECK_EQ(f.buf[13], 0x12); // オーディオ (DCP)
    CHECK_EQ(f.buf[14], 2);
    CHECK_EQ(be32(f.buf + 16), kTrack2);
    CHECK_EQ(f.buf[21], 0x10);
    CHECK_EQ(f.buf[22], 3);
    CHECK_EQ(be32(f.buf + 24), kTrack3);
    CHECK_EQ(f.buf[30], 0xaa);
    CHECK_EQ(be32(f.buf + 32), kSectors);
    // MSF: トラック 3 は 150 + 150 フレーム = 00:04:00、リードアウトは 360 フレーム = 00:04:60
    CHECK_EQ(f.run({0x43, 0x02, 0, 0, 0, 0, 0, 0x03, 0x24, 0}, 804), 0);
    CHECK_EQ(be32(f.buf + 24), 0x00000400);
    CHECK_EQ(be32(f.buf + 32), 0x0000043c);
    // 開始トラック 3: トラック 3 とリードアウト。4 はない
    CHECK_EQ(f.run({0x43, 0, 0, 0, 0, 0, 3, 0, 100, 0}, 100), 0);
    CHECK_EQ(f.got, 20);
    CHECK_EQ(f.buf[6], 3);
    CHECK_EQ(f.buf[14], 0xaa);
    CHECK_EQ(f.run({0x43, 0, 0, 0, 0, 0, 4, 0, 100, 0}, 100), 2);
    CHECK_EQ(f.sense(), 0x052400);

    // READ(10): データトラックの 3 ブロック。セクタの +16 からの 2048 バイトだけが来る
    static uint8_t data[3 * 2048];
    uint32_t got = 0;
    CHECK_EQ(runCommand(f.s, 6, {0x28, 0, 0, 0, 0, 37, 0, 0, 3, 0}, data, sizeof data, &got), 0);
    CHECK_EQ(got, 3 * 2048);
    int diff = 0;
    for (uint32_t i = 0; i < 3 * 2048; i++) diff += data[i] != dataByte(37 + i / 2048, i % 2048);
    CHECK_EQ(diff, 0);
    // READ(6)
    CHECK_EQ(runCommand(f.s, 6, {0x08, 0, 0, 5, 1, 0}, data, 2048, &got), 0);
    CHECK_EQ(got, 2048);
    CHECK_EQ(data[0], dataByte(5, 0));
    CHECK_EQ(data[2047], dataByte(5, 2047));
    // オーディオのトラックは読めない: BLANK CHECK、ILLEGAL MODE FOR THIS TRACK。データトラックからはみ出しても同じ
    CHECK_EQ(f.run({0x28, 0, 0, 0, 0, 60, 0, 0, 1, 0}), 2);
    CHECK_EQ(f.sense(), 0x086400);
    CHECK_EQ(f.run({0x28, 0, 0, 0, 0, 39, 0, 0, 2, 0}), 2);
    CHECK_EQ(f.sense(), 0x086400);
    // リードアウトより後ろ: ILLEGAL REQUEST、LBA OUT OF RANGE
    CHECK_EQ(f.run({0x28, 0, 0, 0, 0, (uint8_t)kSectors, 0, 0, 1, 0}), 2);
    CHECK_EQ(f.sense(), 0x052100);
}

// ---------------------------------------------------------------- MODE SENSE のページ

void testModePages() {
    printf("mode sense pages 01h / 0Dh / 0Eh\n");
    Fixture f;
    // ページ 01h (読み出しのエラー回復): 長さ 6
    CHECK_EQ(f.run({0x1a, 0x08, 0x01, 0, 255, 0}, 255), 0);
    CHECK_EQ(f.got, 4 + 8);
    CHECK_EQ(f.buf[0], 11);
    CHECK_EQ(f.buf[3], 0);
    CHECK_EQ(f.buf[4], 0x01);
    CHECK_EQ(f.buf[5], 0x06);
    // ページ 0Dh (CD-ROM のパラメータ): 1 M = 60 S、1 S = 75 F。ブロックディスクリプタつき
    CHECK_EQ(f.run({0x1a, 0, 0x0d, 0, 255, 0}, 255), 0);
    CHECK_EQ(f.got, 4 + 8 + 8);
    CHECK_EQ(f.buf[3], 8);
    CHECK_EQ(f.buf[4], 0x01); // 密度コード
    CHECK_EQ(be32(f.buf + 4) & 0xffffff, kSectors);
    CHECK_EQ(be32(f.buf + 8), 2048);
    CHECK_EQ(f.buf[12], 0x0d);
    CHECK_EQ(f.buf[13], 0x06);
    CHECK_EQ(f.buf[15] & 0x0f, 0x0d);
    CHECK_EQ(f.buf[16] << 8 | f.buf[17], 60);
    CHECK_EQ(f.buf[18] << 8 | f.buf[19], 75);
    // ページ 0Eh (オーディオの制御): 長さ 14、Immed、出力 0 / 1 はチャネル 0 / 1、音量は最大
    CHECK_EQ(f.run({0x1a, 0x08, 0x0e, 0, 255, 0}, 255), 0);
    CHECK_EQ(f.got, 4 + 16);
    CHECK_EQ(f.buf[4], 0x0e);
    CHECK_EQ(f.buf[5], 0x0e);
    CHECK_EQ(f.buf[6], 0x04);
    CHECK_EQ(f.buf[12], 0x01);
    CHECK_EQ(f.buf[13], 0xff);
    CHECK_EQ(f.buf[14], 0x02);
    CHECK_EQ(f.buf[15], 0xff);
    // 3Fh: 01h、0Dh、0Eh の順にすべて
    CHECK_EQ(f.run({0x1a, 0x08, 0x3f, 0, 255, 0}, 255), 0);
    CHECK_EQ(f.got, 4 + 8 + 8 + 16);
    CHECK_EQ(f.buf[0], 4 + 8 + 8 + 16 - 1);
    CHECK_EQ(f.buf[4], 0x01);
    CHECK_EQ(f.buf[12], 0x0d);
    CHECK_EQ(f.buf[20], 0x0e);
    // MODE SENSE(10)
    CHECK_EQ(f.run({0x5a, 0, 0x0d, 0, 0, 0, 0, 0, 255, 0}, 255), 0);
    CHECK_EQ(f.got, 8 + 8 + 8);
    CHECK_EQ(f.buf[0] << 8 | f.buf[1], 8 + 8 + 8 - 2);
    CHECK_EQ(f.buf[7], 8);
    CHECK_EQ(f.buf[16], 0x0d);
    CHECK_EQ(f.buf[20] << 8 | f.buf[21], 60);
    // 変えられるビット (PC = 1): ページの中身はすべて 0
    CHECK_EQ(f.run({0x1a, 0x08, 0x4d, 0, 255, 0}, 255), 0);
    CHECK_EQ(f.buf[4], 0x0d);
    CHECK_EQ(f.buf[5], 0x06);
    CHECK_EQ(f.buf[9] | f.buf[11], 0);
    // 既定の値 (PC = 2) は今の値と同じ
    CHECK_EQ(f.run({0x1a, 0x08, 0x8d, 0, 255, 0}, 255), 0);
    CHECK_EQ(f.buf[11], 75);
    // 保存した値 (PC = 3) はない: SAVING PARAMETERS NOT SUPPORTED。持っていないページ: INVALID FIELD IN CDB
    CHECK_EQ(f.run({0x1a, 0x08, 0xcd, 0, 255, 0}, 255), 2);
    CHECK_EQ(f.sense(), 0x053900);
    CHECK_EQ(f.run({0x1a, 0x08, 0x08, 0, 255, 0}, 255), 2);
    CHECK_EQ(f.sense(), 0x052400);
    // 割り当て長で切る
    CHECK_EQ(f.run({0x1a, 0x08, 0x3f, 0, 10, 0}, 255), 0);
    CHECK_EQ(f.got, 10);
}

// ---------------------------------------------------------------- 媒体の入れ替え

void testMediumChange() {
    printf("medium change / eject / prevent\n");
    Fixture f;
    CHECK_EQ(f.run({0x00, 0, 0, 0, 0, 0}), 0); // つないだ直後は UNIT ATTENTION を出さない
    // 入れ替え: 次のコマンドに 1 回だけ UNIT ATTENTION、MEDIUM MAY HAVE CHANGED。INQUIRY はその前でも実行する
    static uint8_t iso[32 * 2048];
    for (uint32_t i = 0; i < sizeof iso; i++) iso[i] = (uint8_t)(i * 11);
    x68k::HardDiskImage img2;
    CHECK_EQ(img2.load(iso, sizeof iso, 2048, true), true);
    CHECK_EQ(f.s.changeCd(&img2), true);
    CHECK_EQ(f.s.image(6) == &img2, true);
    CHECK_EQ(f.run({0x12, 0, 0, 0, 36, 0}, 36), 0);
    CHECK_EQ(f.buf[0], 0x05);
    CHECK_EQ(f.run({0x00, 0, 0, 0, 0, 0}), 2);
    CHECK_EQ(f.sense(), 0x062800);
    CHECK_EQ(f.run({0x00, 0, 0, 0, 0, 0}), 0);
    // 新しい媒体はデータトラック 1 つ (ISO)
    CHECK_EQ(f.run({0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 8), 0);
    CHECK_EQ(be32(f.buf), 31);
    static uint8_t data[2048];
    uint32_t got = 0;
    CHECK_EQ(runCommand(f.s, 6, {0x28, 0, 0, 0, 0, 3, 0, 0, 1, 0}, data, 2048, &got), 0);
    CHECK_EQ(memcmp(data, iso + 3 * 2048, 2048), 0);
    // 入れ替えたあと、最初が REQUEST SENSE なら、そこで UNIT ATTENTION を返す (次のコマンドには出さない)
    CHECK_EQ(f.s.changeCd(&f.img, &f.toc), true);
    CHECK_EQ(f.sense(), 0x062800);
    CHECK_EQ(f.run({0x00, 0, 0, 0, 0, 0}), 0);
    CHECK_EQ(f.run({0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 8), 0);
    CHECK_EQ(be32(f.buf), kSectors - 1);

    // 取り出し: 装置はセレクションに答え、媒体の要るコマンドは NOT READY、MEDIUM NOT PRESENT
    CHECK_EQ(f.s.changeCd(nullptr), true);
    CHECK_EQ(f.s.isCdrom(6), true);
    CHECK_EQ(f.run({0x00, 0, 0, 0, 0, 0}), 2);
    CHECK_EQ(f.sense(), 0x023a00);
    CHECK_EQ(f.run({0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0}), 2);
    CHECK_EQ(f.sense(), 0x023a00);
    CHECK_EQ(f.run({0x43, 0, 0, 0, 0, 0, 0, 0, 100, 0}, 100), 2);
    CHECK_EQ(f.run({0x45, 0, 0, 0, 0, 60, 0, 0, 10, 0}), 2);
    CHECK_EQ(f.run({0x12, 0, 0, 0, 36, 0}, 36), 0); // INQUIRY と MODE SENSE は媒体がなくても答える
    CHECK_EQ(f.run({0x1a, 0, 0x0d, 0, 255, 0}, 255), 0);
    CHECK_EQ(be32(f.buf + 4) & 0xffffff, 0);
    // 入れると、UNIT ATTENTION のあと使える
    CHECK_EQ(f.s.changeCd(&f.img, &f.toc), true);
    CHECK_EQ(f.run({0x00, 0, 0, 0, 0, 0}), 2);
    CHECK_EQ(f.sense(), 0x062800);
    CHECK_EQ(f.run({0x00, 0, 0, 0, 0, 0}), 0);

    // START STOP UNIT: LoEj + Start = 0 でトレイを開ける。開いている間は MEDIUM NOT PRESENT。
    // LoEj + Start = 1 で閉じると、UNIT ATTENTION のあと使える
    CHECK_EQ(f.run({0x1b, 0, 0, 0, 0x02, 0}), 0);
    CHECK_EQ(f.cd().trayOpen(), true);
    CHECK_EQ(f.run({0x00, 0, 0, 0, 0, 0}), 2);
    CHECK_EQ(f.sense(), 0x023a00);
    CHECK_EQ(f.run({0x1b, 0, 0, 0, 0x03, 0}), 0);
    CHECK_EQ(f.cd().trayOpen(), false);
    CHECK_EQ(f.run({0x00, 0, 0, 0, 0, 0}), 2);
    CHECK_EQ(f.sense(), 0x062800);
    CHECK_EQ(f.run({0x00, 0, 0, 0, 0, 0}), 0);
    // Start だけ (LoEj なし) ではトレイは動かない
    CHECK_EQ(f.run({0x1b, 0, 0, 0, 0x00, 0}), 0);
    CHECK_EQ(f.cd().trayOpen(), false);
    CHECK_EQ(f.run({0x00, 0, 0, 0, 0, 0}), 0);

    // PREVENT ALLOW MEDIUM REMOVAL: 禁止している間は開けられない (ILLEGAL REQUEST、MEDIUM REMOVAL PREVENTED)
    CHECK_EQ(f.run({0x1e, 0, 0, 0, 0x01, 0}), 0);
    CHECK_EQ(f.cd().prevented(), true);
    CHECK_EQ(f.run({0x1b, 0, 0, 0, 0x02, 0}), 2);
    CHECK_EQ(f.sense(), 0x055302);
    CHECK_EQ(f.cd().trayOpen(), false);
    CHECK_EQ(f.run({0x1e, 0, 0, 0, 0x00, 0}), 0);
    CHECK_EQ(f.run({0x1b, 0, 0, 0, 0x02, 0}), 0);
    CHECK_EQ(f.cd().trayOpen(), true);
    // リセットで禁止は解け、トレイは閉じる (UNIT ATTENTION は出さない)
    CHECK_EQ(f.run({0x1e, 0, 0, 0, 0x01, 0}), 0);
    initSpc(f.s);
    CHECK_EQ(f.cd().prevented(), false);
    CHECK_EQ(f.cd().trayOpen(), false);
    CHECK_EQ(f.run({0x00, 0, 0, 0, 0, 0}), 0);

    // 入れ替えたあと、コマンドを出す前にリセットしたら、UNIT ATTENTION は出さない (起動の最初のコマンドを失敗させない)
    CHECK_EQ(f.s.changeCd(&f.img, &f.toc), true);
    initSpc(f.s);
    CHECK_EQ(f.run({0x00, 0, 0, 0, 0, 0}), 0);

    // CD-ROM 装置のない SPC では changeCd() は false。ハードディスクとしてつなぎ直すと装置は外れる
    Spc none;
    CHECK_EQ(none.changeCd(&img2), false);
    f.s.attach(6, nullptr);
    CHECK_EQ(f.s.isCdrom(6), false);
    CHECK_EQ(select(f.s, 6), false);
}

// ---------------------------------------------------------------- オーディオの再生のコマンド

// clocks だけ進めて、再生されたフレームが [lba, f) から続く PCM と同じかを調べ、フレーム数を返す
int checkFrames(CdDrive& cd, uint32_t clocks, uint32_t& lba, uint32_t& f) {
    static int16_t out[2 * 4096];
    int total = 0, n;
    uint32_t c = clocks;
    while ((n = cd.produce(c, out, 4096)) > 0) {
        c = 0;
        int diff = 0;
        for (int i = 0; i < n; i++) {
            if (out[2 * i] != pcmLeft(lba, f) || out[2 * i + 1] != pcmRight(lba, f)) diff++;
            if (++f == 588) {
                f = 0;
                lba++;
            }
        }
        CHECK_EQ(diff, 0);
        total += n;
        if (n < 4096) break;
    }
    return total;
}

void testPlay() {
    printf("play audio / pause resume / read sub-channel\n");
    Fixture f;
    CdDrive& cd = f.cd();
    CHECK_EQ(f.status(), CdDrive::AUDIO_NONE);
    // 長さ 0 の PLAY AUDIO は何もしない (エラーではない)
    CHECK_EQ(f.run({0x45, 0, 0, 0, 0, 60, 0, 0, 0, 0}), 0);
    CHECK_EQ(cd.playing(), false);
    // データトラックは再生できない: BLANK CHECK、ILLEGAL MODE FOR THIS TRACK。範囲の外: LBA OUT OF RANGE
    CHECK_EQ(f.run({0x45, 0, 0, 0, 0, 10, 0, 0, 5, 0}), 2);
    CHECK_EQ(f.sense(), 0x086400);
    CHECK_EQ(f.run({0x45, 0, 0, 0, 0, (uint8_t)kSectors, 0, 0, 5, 0}), 2);
    CHECK_EQ(f.sense(), 0x052100);

    // PLAY AUDIO(10): 論理ブロック 60 から 20 ブロック
    CHECK_EQ(f.run({0x45, 0, 0, 0, 0, 60, 0, 0, 20, 0}), 0);
    CHECK_EQ(cd.playing(), true);
    CHECK_EQ(f.status(), CdDrive::AUDIO_PLAY);
    // 4MHz のクロック 40000 個で 441 フレーム。1 秒 (4000000) で 44100 フレーム = 75 ブロック
    uint32_t lba = 60, fr = 0;
    CHECK_EQ(checkFrames(cd, 40000, lba, fr), 441);
    CHECK_EQ(checkFrames(cd, 40000, lba, fr), 441);
    CHECK_EQ(checkFrames(cd, 1, lba, fr), 0);
    // READ SUB-CHANNEL (今の位置): 882 フレーム = 1 ブロックと 294 フレーム進んだので、論理ブロック 61、トラック 2、
    // インデックス 1、トラックの中では 61 - 50 = 11
    CHECK_EQ(f.run({0x42, 0, 0x40, 0x01, 0, 0, 0, 0, 16, 0}, 16), 0);
    CHECK_EQ(f.got, 16);
    CHECK_EQ(f.buf[1], CdDrive::AUDIO_PLAY);
    CHECK_EQ(f.buf[2] << 8 | f.buf[3], 12);
    CHECK_EQ(f.buf[4], 0x01);
    CHECK_EQ(f.buf[5], 0x12);
    CHECK_EQ(f.buf[6], 2);
    CHECK_EQ(f.buf[7], 1);
    CHECK_EQ(be32(f.buf + 8), 61);
    CHECK_EQ(be32(f.buf + 12), 11);
    // MSF: 絶対位置は 61 + 150 = 211 フレーム = 00:02:61、トラックの中は 00:00:11
    CHECK_EQ(f.run({0x42, 0x02, 0x40, 0x01, 0, 0, 0, 0, 16, 0}, 16), 0);
    CHECK_EQ(be32(f.buf + 8), 0x0000023d);
    CHECK_EQ(be32(f.buf + 12), 0x0000000b);
    // SubQ = 0 ならヘッダだけ。形式 0 は 48 バイト、形式 2 と 3 は 24 バイト
    CHECK_EQ(f.run({0x42, 0, 0x00, 0x01, 0, 0, 0, 0, 16, 0}, 16), 0);
    CHECK_EQ(f.got, 4);
    CHECK_EQ(f.buf[1], CdDrive::AUDIO_PLAY);
    CHECK_EQ(f.buf[2] << 8 | f.buf[3], 0);
    CHECK_EQ(f.run({0x42, 0, 0x40, 0x00, 0, 0, 0, 0, 100, 0}, 100), 0);
    CHECK_EQ(f.got, 48);
    CHECK_EQ(be32(f.buf + 8), 61);
    CHECK_EQ(f.buf[16] & 0x80, 0); // MCVal = 0
    CHECK_EQ(f.run({0x42, 0, 0x40, 0x02, 0, 0, 0, 0, 100, 0}, 100), 0);
    CHECK_EQ(f.got, 24);
    CHECK_EQ(f.buf[4], 0x02);
    CHECK_EQ(f.run({0x42, 0, 0x40, 0x03, 0, 0, 3, 0, 100, 0}, 100), 0);
    CHECK_EQ(f.got, 24);
    CHECK_EQ(f.buf[6], 3);
    CHECK_EQ(f.run({0x42, 0, 0x40, 0x04, 0, 0, 0, 0, 100, 0}, 100), 2);
    // REQUEST SENSE でも再生の状態が分かる (NO SENSE、クオリファイアがオーディオステータス)
    CHECK_EQ(f.sense() & 0xff00ff, 0x050000 | 0); // 直前のエラー (形式 4) のセンス
    CHECK_EQ(f.sense(), 0x000011);

    // PAUSE: 止まっている間はフレームが出ず、位置も進まない。もう一度 PAUSE してもエラーではない
    CHECK_EQ(f.run({0x4b, 0, 0, 0, 0, 0, 0, 0, 0x00, 0}), 0);
    CHECK_EQ(f.status(), CdDrive::AUDIO_PAUSED);
    CHECK_EQ(checkFrames(cd, 400000, lba, fr), 0);
    CHECK_EQ(f.run({0x4b, 0, 0, 0, 0, 0, 0, 0, 0x00, 0}), 0);
    // RESUME: 続きから
    CHECK_EQ(f.run({0x4b, 0, 0, 0, 0, 0, 0, 0, 0x01, 0}), 0);
    CHECK_EQ(f.status(), CdDrive::AUDIO_PLAY);
    CHECK_EQ(f.run({0x4b, 0, 0, 0, 0, 0, 0, 0, 0x01, 0}), 0);
    // 最後まで: 20 ブロック = 11760 フレーム。終わったら、状態 13h を 1 回だけ返して 15h になる
    const int rest = 20 * 588 - 882;
    CHECK_EQ(checkFrames(cd, 4000000, lba, fr), rest);
    CHECK_EQ(lba, 80);
    CHECK_EQ(cd.playing(), false);
    CHECK_EQ(f.status(), CdDrive::AUDIO_DONE);
    CHECK_EQ(be32(f.buf + 8), 80);
    CHECK_EQ(f.status(), CdDrive::AUDIO_NONE);
    // 再生していないときの PAUSE / RESUME はエラー
    CHECK_EQ(f.run({0x4b, 0, 0, 0, 0, 0, 0, 0, 0x00, 0}), 2);
    CHECK_EQ(f.sense(), 0x052c00);
    CHECK_EQ(f.run({0x4b, 0, 0, 0, 0, 0, 0, 0, 0x01, 0}), 2);

    // PLAY AUDIO MSF: 00:02:45 (論理ブロック 45、トラック 2 のプリギャップ) から 00:02:52 まで。
    // プリギャップではインデックス 0、トラックの中の位置は INDEX 01 までの残り (LBA では負)
    CHECK_EQ(f.run({0x47, 0, 0, 0, 2, 45, 0, 2, 52, 0}), 0);
    CHECK_EQ(f.run({0x42, 0, 0x40, 0x01, 0, 0, 0, 0, 16, 0}, 16), 0);
    CHECK_EQ(f.buf[6], 2);
    CHECK_EQ(f.buf[7], 0);
    CHECK_EQ(be32(f.buf + 8), 45);
    CHECK_EQ(be32(f.buf + 12), (uint32_t)-5);
    CHECK_EQ(f.run({0x42, 0x02, 0x40, 0x01, 0, 0, 0, 0, 16, 0}, 16), 0);
    CHECK_EQ(be32(f.buf + 12), 0x00000005);
    lba = 45;
    fr = 0;
    CHECK_EQ(checkFrames(cd, 4000000, lba, fr), 7 * 588);
    CHECK_EQ(f.status(), CdDrive::AUDIO_DONE);
    // 始まりと終わりが同じなら何もしない。始まりが後ろならエラー
    CHECK_EQ(f.run({0x47, 0, 0, 0, 2, 60, 0, 2, 60, 0}), 0);
    CHECK_EQ(cd.playing(), false);
    CHECK_EQ(f.run({0x47, 0, 0, 0, 2, 61, 0, 2, 60, 0}), 2);
    CHECK_EQ(f.sense(), 0x052400);
    // データトラックからは再生できない
    CHECK_EQ(f.run({0x47, 0, 0, 0, 2, 0, 0, 2, 60, 0}), 2);
    CHECK_EQ(f.sense(), 0x086400);

    // PLAY AUDIO MSF の終わりがリードアウトより後ろ (09:00:00) なら、最後のトラックの終わりまで再生して正常に終わる
    CHECK_EQ(f.run({0x47, 0, 0, 0, 4, 50, 9, 0, 0, 0}), 0); // 00:04:50 = 論理ブロック 200
    lba = 200;
    fr = 0;
    CHECK_EQ(checkFrames(cd, 8000000, lba, fr), 10 * 588);
    CHECK_EQ(f.status(), CdDrive::AUDIO_DONE);
    CHECK_EQ(be32(f.buf + 8), kSectors - 1); // 今の位置は、最後のブロックとして返す
    CHECK_EQ(cd.position(), kSectors);

    // 始まりが FFFFFFFFh (PLAY AUDIO) / FF:FF:FF (PLAY AUDIO MSF) なら、今の位置から
    CHECK_EQ(f.run({0x2b, 0, 0, 0, 0, 70, 0, 0, 0, 0}), 0); // SEEK で論理ブロック 70 へ
    CHECK_EQ(f.run({0x45, 0, 0xff, 0xff, 0xff, 0xff, 0, 0, 3, 0}), 0);
    CHECK_EQ(f.status(), CdDrive::AUDIO_PLAY);
    CHECK_EQ(be32(f.buf + 8), 70);
    lba = 70;
    fr = 0;
    CHECK_EQ(checkFrames(cd, 4000000, lba, fr), 3 * 588);
    CHECK_EQ(f.status(), CdDrive::AUDIO_DONE);
    CHECK_EQ(f.run({0xa5, 0, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 2, 0, 0}), 0); // 終わった位置 (73) から 2 ブロック
    lba = 73;
    fr = 0;
    CHECK_EQ(checkFrames(cd, 4000000, lba, fr), 2 * 588);
    CHECK_EQ(f.run({0x2b, 0, 0, 0, 0, 80, 0, 0, 0, 0}), 0);
    CHECK_EQ(f.run({0x47, 0, 0, 0xff, 0xff, 0xff, 0, 3, 9, 0}), 0); // 今の位置 (80) から 00:03:09 (論理ブロック 84) まで
    lba = 80;
    fr = 0;
    CHECK_EQ(checkFrames(cd, 4000000, lba, fr), 4 * 588);
    // 今の位置がデータトラックなら、再生できない
    CHECK_EQ(f.run({0x2b, 0, 0, 0, 0, 5, 0, 0, 0, 0}), 0);
    CHECK_EQ(f.run({0x45, 0, 0xff, 0xff, 0xff, 0xff, 0, 0, 3, 0}), 2);
    CHECK_EQ(f.sense(), 0x086400);

    // PLAY AUDIO(12): 長さが 32 ビット。リードアウトを超える長さは、最後まで
    CHECK_EQ(f.run({0xa5, 0, 0, 0, 0, 200, 0, 1, 0, 0, 0, 0}), 0);
    lba = 200;
    fr = 0;
    CHECK_EQ(checkFrames(cd, 8000000, lba, fr), 10 * 588);
    CHECK_EQ(f.status(), CdDrive::AUDIO_DONE);

    // PLAY AUDIO TRACK INDEX: トラック 2 のインデックス 1 から、トラック 3 の終わりまで (トラックをまたいで続く)
    CHECK_EQ(f.run({0x48, 0, 0, 0, 2, 1, 0, 3, 1, 0}), 0);
    CHECK_EQ(f.status(), CdDrive::AUDIO_PLAY);
    CHECK_EQ(be32(f.buf + 8), kTrack2);
    lba = kTrack2;
    fr = 0;
    CHECK_EQ(checkFrames(cd, 2 * 4000000, lba, fr), 150 * 588); // 2 秒 = 150 ブロック
    CHECK_EQ(f.status(), CdDrive::AUDIO_PLAY);
    CHECK_EQ(f.buf[6], 3);
    CHECK_EQ(be32(f.buf + 8), 200);
    CHECK_EQ(be32(f.buf + 12), 50);
    // ないトラック、終わりが始まりより前
    CHECK_EQ(f.run({0x48, 0, 0, 0, 4, 1, 0, 4, 1, 0}), 2);
    CHECK_EQ(f.run({0x48, 0, 0, 0, 3, 1, 0, 2, 1, 0}), 2);
    CHECK_EQ(cd.playing(), true); // エラーになったコマンドは、今の再生を止めない
    // VERIFY: 範囲の中なら正常に終わり、再生は止まる。範囲の外ならエラーで、再生は止まらない。長さ 0 はエラーではない
    {
        const uint32_t out = cd.toc().leadOut;
        auto verify = [&](uint32_t at, uint8_t count) {
            return f.run({0x2f, 0, (uint8_t)(at >> 24), (uint8_t)(at >> 16), (uint8_t)(at >> 8), (uint8_t)at, 0, 0, count, 0});
        };
        CHECK_EQ(verify(out - 1, 2), 2);
        CHECK_EQ(f.sense(), 0x052100);
        CHECK_EQ(verify(out + 1, 0), 2);
        CHECK_EQ(cd.playing(), true);
        CHECK_EQ(verify(out, 0), 0);
        CHECK_EQ(cd.playing(), false);
        CHECK_EQ(verify(out - 1, 1), 0);
        CHECK_EQ(f.run({0x48, 0, 0, 0, 2, 1, 0, 3, 1, 0}), 0);
        CHECK_EQ(cd.playing(), true);
    }
    CHECK_EQ(f.run({0x48, 0, 0, 0, 2, 0, 0, 3, 1, 0}), 0);
    CHECK_EQ(f.status(), CdDrive::AUDIO_PLAY);
    CHECK_EQ(be32(f.buf + 8), kData);
    // 終わりのインデックスは 0 でも (正しい値ではないが)、終わりのトラックの終わりまで
    CHECK_EQ(f.run({0x48, 0, 0, 0, 2, 1, 0, 2, 0, 0}), 0);
    lba = kTrack2;
    fr = 0;
    CHECK_EQ(checkFrames(cd, 2 * 4000000, lba, fr), (int)(kTrack3 - kTrack2) * 588);
    CHECK_EQ(f.status(), CdDrive::AUDIO_DONE);
    // トラックにない始まりのインデックス (2 以上): 次のトラックの始まりから
    CHECK_EQ(f.run({0x48, 0, 0, 0, 2, 2, 0, 3, 1, 0}), 0);
    CHECK_EQ(f.status(), CdDrive::AUDIO_PLAY);
    CHECK_EQ(be32(f.buf + 8), kTrack3);
    // 終わりが同じトラックか、次のトラックがなければ、何もせずに正常に終わる (今の再生は続く)
    CHECK_EQ(f.run({0x48, 0, 0, 0, 2, 2, 0, 2, 1, 0}), 0);
    CHECK_EQ(f.run({0x48, 0, 0, 0, 3, 2, 0, 3, 1, 0}), 0);
    CHECK_EQ(f.status(), CdDrive::AUDIO_PLAY);
    CHECK_EQ(be32(f.buf + 8) >= kTrack3, true);
    CHECK_EQ(cd.playing(), true);

    // START STOP UNIT: Start = 1 だけでは止まらず、Start = 0 で止まる。READ と SEEK でも止まる
    CHECK_EQ(f.run({0x1b, 0, 0, 0, 0x01, 0}), 0);
    CHECK_EQ(f.status(), CdDrive::AUDIO_PLAY);
    CHECK_EQ(f.run({0x1b, 0, 0, 0, 0x00, 0}), 0);
    CHECK_EQ(f.status(), CdDrive::AUDIO_NONE);
    CHECK_EQ(f.run({0x45, 0, 0, 0, 0, 60, 0, 0, 20, 0}), 0);
    static uint8_t data[2048];
    uint32_t got = 0;
    CHECK_EQ(runCommand(f.s, 6, {0x28, 0, 0, 0, 0, 2, 0, 0, 1, 0}, data, 2048, &got), 0);
    CHECK_EQ(f.status(), CdDrive::AUDIO_NONE);
    CHECK_EQ(be32(f.buf + 8), 3); // 読んだブロックの次
    CHECK_EQ(f.run({0x45, 0, 0, 0, 0, 60, 0, 0, 20, 0}), 0);
    CHECK_EQ(f.run({0x2b, 0, 0, 0, 0, 100, 0, 0, 0, 0}), 0);
    CHECK_EQ(f.status(), CdDrive::AUDIO_NONE);
    CHECK_EQ(be32(f.buf + 8), 100);
    // 媒体を入れ替えると止まる
    CHECK_EQ(f.run({0x45, 0, 0, 0, 0, 60, 0, 0, 20, 0}), 0);
    f.s.changeCd(&f.img, &f.toc);
    CHECK_EQ(cd.playing(), false);
    CHECK_EQ(f.run({0x00, 0, 0, 0, 0, 0}), 2);
    // バスのリセットでも止まる
    CHECK_EQ(f.run({0x45, 0, 0, 0, 0, 60, 0, 0, 20, 0}), 0);
    f.s.write(Spc::SCMD, Spc::SCMD_RST);
    f.s.write(Spc::SCMD, 0);
    f.s.write(Spc::INTS, 0xff);
    CHECK_EQ(cd.playing(), false);
}

// 再生がデータトラックに入ったら止まり、次のコマンドに遅れて知らせるエラーを返す
void testPlayIntoData() {
    printf("play runs into a data track\n");
    makeBin();
    // トラック 1 がオーディオ (セクタ 150-209 を使う)、トラック 2 がデータ (セクタ 0-39)
    CdToc toc;
    toc.count = 2;
    toc.tracks[0].number = 1;
    toc.tracks[0].control = 0;
    toc.tracks[0].sector = 2352;
    toc.tracks[0].fileEnd = 60;
    toc.tracks[0].offset = (uint64_t)150 * 2352;
    toc.tracks[1].number = 2;
    toc.tracks[1].sector = 2352;
    toc.tracks[1].dataOffset = 16;
    toc.tracks[1].begin = toc.tracks[1].start = toc.tracks[1].fileLba = 60;
    toc.tracks[1].fileEnd = 100;
    toc.leadOut = 100;
    x68k::HardDiskImage img;
    CHECK_EQ(img.load(g_bin, sizeof g_bin, 1, true), true);
    Spc s;
    s.attach(5, &img, true, &toc);
    initSpc(s);
    uint8_t buf[64];
    uint32_t got = 0;
    CHECK_EQ(runCommand(s, 5, {0x45, 0, 0, 0, 0, 58, 0, 0, 10, 0}), 0);
    static int16_t out[2 * 4096];
    CHECK_EQ(s.cd()->produce(4000000, out, 4096), 2 * 588);
    CHECK_EQ(out[0], pcmLeft(150 + 58, 0));
    CHECK_EQ(s.cd()->audioStatus(), CdDrive::AUDIO_ERROR);
    // INQUIRY は実行する。次のコマンドは CHECK CONDITION: エラーコード 71h、BLANK CHECK、END OF USER AREA
    CHECK_EQ(runCommand(s, 5, {0x12, 0, 0, 0, 36, 0}, buf, 36, &got), 0);
    CHECK_EQ(runCommand(s, 5, {0x00, 0, 0, 0, 0, 0}), 2);
    CHECK_EQ(runCommand(s, 5, {0x03, 0, 0, 0, 18, 0}, buf, 18, &got), 0);
    CHECK_EQ(buf[0], 0x71);
    CHECK_EQ(buf[2], 0x08);
    CHECK_EQ(buf[12], 0x63);
    // そのあとの READ SUB-CHANNEL は、状態 14h を 1 回だけ返す
    CHECK_EQ(runCommand(s, 5, {0x42, 0, 0x40, 0x01, 0, 0, 0, 0, 16, 0}, buf, 16, &got), 0);
    CHECK_EQ(buf[1], CdDrive::AUDIO_ERROR);
    CHECK_EQ(runCommand(s, 5, {0x42, 0, 0x40, 0x01, 0, 0, 0, 0, 16, 0}, buf, 16, &got), 0);
    CHECK_EQ(buf[1], CdDrive::AUDIO_NONE);
    // データトラックは読める (2352 バイト / セクタの +16 から)
    static uint8_t data[2048];
    CHECK_EQ(runCommand(s, 5, {0x28, 0, 0, 0, 0, 61, 0, 0, 1, 0}, data, 2048, &got), 0);
    CHECK_EQ(got, 2048);
    CHECK_EQ(data[5], dataByte(1, 5));
}

// 44.1kHz のフレームを OPM のサンプルの刻みに直すところ: 再生を始めたとき (前の再生の位相が残っていても) に、
// 無音のフレームをはさまない
void testMixer() {
    printf("mixer: no gap after the play starts\n");
    int bad = 0;
    for (int k = 0; k < 12; k++) {
        for (uint32_t acc0 = 0; acc0 < 40000; acc0 += 3999) {
            x68k::CdMixer mx;
            int32_t l, r;
            // 鳴らすものがないまま k 回進めて、位相を残す (441 x k を 625 で割った余り)
            mx.push(0, 0);
            for (int i = 0; i < k + 2; i++) {
                l = r = 0;
                mx.clock(l, r);
            }
            CHECK_EQ(mx.idle(), true);
            // Machine と同じ順: OPM の 1 サンプル (64 クロック) ごとに、その間のフレームを入れてから進める。
            // 値が 1000 になったあと、1000 でなくなったら、フレームが届く前に取り出している
            uint32_t acc = acc0;
            bool reached = false;
            for (int i = 0; i < 3000; i++) {
                acc += 64 * 441;
                while (acc >= 40000) {
                    acc -= 40000;
                    mx.push(1000, -1000);
                }
                if (mx.idle()) continue;
                l = r = 0;
                mx.clock(l, r);
                if (l == 1000 && r == -1000) reached = true;
                else if (reached) bad++;
            }
            if (!reached) bad++;
        }
    }
    CHECK_EQ(bad, 0);
}

// step() と between(): フレーム 1 つだけ (前後は無音) を入れて、鳴らすものがなくなるまで進める。
//   - between(0) は 1 つ前の step() の時点の値、between(4095) は今の時点の値の手前 (差の 4095 / 4096 まで)
//   - 値は clock() で進めたものと同じ
//   - idle() になったときには、between() はどの位置でも 0 (終わりの値から 0 までを出し切ってから idle() になる)
void testMixerStep() {
    printf("mixer: step / between with a single frame\n");
    int bad = 0, late = 0;
    for (int k = 0; k < 12; k++) {
        x68k::CdMixer mx, ref;
        // 位相を残す (testMixer() と同じ)。step() で進めた mx と clock() で進めた ref を同じ位相にする
        mx.push(0, 0);
        ref.push(0, 0);
        for (int i = 0; i < k + 2; i++) {
            int32_t l = 0, r = 0;
            ref.clock(l, r);
            if (!mx.idle()) mx.step();
        }
        CHECK_EQ(mx.idle(), true);
        // 鳴らすものがないまま進めた分の位相を残してから、フレームを 1 つ入れる
        for (int i = 0; i < k; i++) {
            int32_t l = 0, r = 0;
            mx.clock(l, r);
            ref.clock(l, r);
        }
        mx.push(1000, -2000);
        ref.push(1000, -2000);
        CHECK_EQ(mx.idle(), false);
        int32_t prevL = 0, prevR = 0, peak = 0;
        int steps = 0, refIdleAt = -1;
        while (!mx.idle() && steps < 100) {
            mx.step();
            int32_t curL = 0, curR = 0;
            ref.clock(curL, curR);
            steps++;
            if (refIdleAt < 0 && ref.idle()) refIdleAt = steps;
            if (curL > peak) peak = curL;
            int32_t l, r;
            mx.between(0, l, r);
            bad += l != prevL || r != prevR;
            mx.between(4095, l, r);
            bad += l != prevL + (((curL - prevL) * 4095) >> 12) || r != prevR + (((curR - prevR) * 4095) >> 12);
            mx.between(2048, l, r);
            bad += l != prevL + (((curL - prevL) * 2048) >> 12) || r != prevR + (((curR - prevR) * 2048) >> 12);
            // 左は 0 から 1000、右は 0 から -2000 の間
            bad += l < 0 || l > 1000 || r > 0 || r < -2000;
            prevL = curL;
            prevR = curR;
        }
        // 止まったときには、もう出すものがない
        for (int f : {0, 1, 2048, 4095}) {
            int32_t l = 1, r = 1;
            mx.between(f, l, r);
            late += l != 0 || r != 0;
        }
        // フレーム 1 つは 625 / 441 サンプルの間に上がり、同じ間に下がる。clock() の値が 0 になった次の step() で止まる
        CHECK_EQ(steps < 100, true);
        CHECK_EQ(refIdleAt > 0 && steps == refIdleAt + 1, true);
        CHECK_EQ(peak > 0 && peak <= 1000, true);
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(late, 0);
    // 鳴らしている途中で reset() すると、覚えた値も 0 に戻る
    x68k::CdMixer mx;
    for (int i = 0; i < 4; i++) mx.push(1000, -2000);
    mx.step();
    mx.step();
    int32_t l = 0, r = 0;
    mx.between(4095, l, r);
    CHECK_EQ(l != 0 && r != 0, true);
    mx.reset();
    CHECK_EQ(mx.idle(), true);
    mx.between(0, l, r);
    CHECK_EQ(l == 0 && r == 0, true);
    mx.between(4095, l, r);
    CHECK_EQ(l == 0 && r == 0, true);
}

// 記録に入れるフレームの形: 前のフレームとの差が左右とも -8192..8191 なら 1 個 (J_CD_DELTA)、そうでなければ 2 個 (J_CD)。
// どちらでも、記録を当てて作った音は、記録を通さないときと同じになる
void testJournalDelta() {
    printf("journal: frames as deltas (1 entry) or as samples (2 entries)\n");
    // CdMixer::delta() の端の値
    {
        x68k::CdMixer mx;
        int dl = 0, dr = 0;
        CHECK_EQ(mx.delta(8191, -8192, dl, dr), true); // 0 から +8191 と -8192
        CHECK_EQ(dl, 8191);
        CHECK_EQ(dr, -8192);
        CHECK_EQ(mx.delta(8191 + 8192, -8192, dl, dr), false); // +8192
        CHECK_EQ(mx.delta(8191 + 8192, -8192 + 8192, dl, dr), false); // 右だけ +8192
        CHECK_EQ(mx.delta(8191 + 8192 - 8193, 0, dl, dr), false); // -8193
        CHECK_EQ(mx.delta(8190 - 8192, 8191, dl, dr), true); // -8192 と +8191
        CHECK_EQ(mx.delta(32767, -32768, dl, dr), false);
        CHECK_EQ(mx.delta(-32768, 32767, dl, dr), false); // -65535 と +65535
        CHECK_EQ(dl, -65535);
        CHECK_EQ(dr, 65535);
        CHECK_EQ(mx.delta(-32768, 32767, dl, dr), true); // 差 0
    }
    // 端の値を並べたフレームを、記録を通す Sound と通さない Sound に渡す
    static const int kSteps[] = {8191, -8192, 8192, -8193, 0, 1, -1, 8190, -8191, 20000, -20000, 65535, -65535, 3};
    const int kCount = (int)(sizeof kSteps / sizeof kSteps[0]);
    static uint32_t ring[1 << 14];
    x68k::VideoJournal journal;
    journal.init(ring, 14);
    x68k::Sound* single = new x68k::Sound;
    x68k::Sound* emu = new x68k::Sound;
    x68k::Sound* synth = new x68k::Sound;
    x68k::CdMixer* direct = new x68k::CdMixer;
    x68k::CdMixer* writer = new x68k::CdMixer;
    single->setOutputRate(48000);
    synth->setOutputRate(48000);
    x68k::SoundReplayer* replayer = new x68k::SoundReplayer(journal, *synth);
    emu->setJournal(&journal);
    int l = 0, r = 0, expect1 = 0, expect2 = 0, got1 = 0, got2 = 0;
    for (int i = 0; i < kCount * kCount; i++) {
        // 左と右の差の組み合わせをすべて (範囲を超えたら 0 に戻す。そのときの差も数える)
        int nl = l + kSteps[i / kCount], nr = r + kSteps[i % kCount];
        if (nl > 32767 || nl < -32768) nl = 0;
        if (nr > 32767 || nr < -32768) nr = 0;
        const int dl = nl - l, dr = nr - r;
        if (dl >= -8192 && dl <= 8191 && dr >= -8192 && dr <= 8191) expect1++;
        else expect2++;
        l = nl;
        r = nr;
        single->cdFrame(direct, (int16_t)l, (int16_t)r);
        emu->cdFrame(writer, (int16_t)l, (int16_t)r);
        single->advance(91, nullptr, nullptr, direct);
        emu->advance(91, nullptr, nullptr, writer);
    }
    single->advance(4000, nullptr, nullptr, direct);
    emu->advance(4000, nullptr, nullptr, writer);
    emu->flushJournal();
    journal.publish();
    const uint32_t* p;
    const unsigned n = journal.available(p);
    for (unsigned i = 0; i < n; i++) {
        if (p[i] >> 28 == x68k::Sound::J_CD_DELTA) got1++;
        if (p[i] >> 28 == x68k::Sound::J_CD) got2++;
    }
    CHECK_EQ(got1, expect1);
    CHECK_EQ(got2, 2 * expect2);
    if (expect1 < 20 || expect2 < 20) {
        printf("  %d frames as deltas, %d as samples\n", expect1, expect2);
        failures++;
    }
    while (replayer->step(1 << 20) != 0) {
    }
    static int16_t pa[2 * 4096], pb[2 * 4096];
    const int na = single->ring().drain(pa, 4096), nb = synth->ring().drain(pb, 4096);
    CHECK_EQ(na, nb);
    int diff = 0, nonzero = 0;
    for (int i = 0; i < 2 * na && i < 2 * nb; i++) {
        diff += pa[i] != pb[i];
        nonzero += pa[i] != 0;
    }
    CHECK_EQ(diff, 0);
    if (na < 200 || nonzero < na) {
        printf("  %d frames, %d samples are not zero\n", na, nonzero);
        failures++;
    }
    delete replayer;
    delete single;
    delete emu;
    delete synth;
    delete direct;
    delete writer;
}

// ---------------------------------------------------------------- 使うチャンクだけを読むイメージ

void testCached() {
    printf("cached image: data and audio wait for the chunks\n");
    makeBin();
    CdToc toc;
    CHECK_EQ(makeToc(toc), true);
    const uint32_t CHUNK = x68k::DiskCache::CHUNK;
    static x68k::DiskCache cache;
    CHECK_EQ(cache.init(sizeof g_bin, 8), true);
    x68k::HardDiskImage img;
    CHECK_EQ(img.loadCached(&cache, sizeof g_bin, 1, true), true);
    int loads = 0;
    auto serve = [&]() {
        unsigned slot;
        uint32_t c, len;
        uint8_t* buf;
        int n = 0;
        while (cache.nextLoad(slot, c, buf, len)) {
            memcpy(buf, g_bin + (size_t)c * CHUNK, len);
            cache.loaded(slot, true);
            n++;
        }
        loads += n;
        return n;
    };
    Spc s;
    s.attach(6, &img, true, &toc);
    initSpc(s);
    // READ(10): 論理ブロック 5 から 20 ブロック (2352 バイト / セクタで 47KB。チャンクをまたぐ)
    CHECK_EQ(select(s, 6), true);
    const uint8_t cdb[10] = {0x28, 0, 0, 0, 0, 5, 0, 0, 20, 0};
    CHECK_EQ(transferOut(s, Spc::PH_COMMAND, cdb, 10), Spc::INTS_COMPLETE);
    s.write(Spc::PCTL, Spc::PH_DATA_IN);
    setTc(s, 20 * 2048);
    s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    int waits = 0, diff = 0;
    for (uint32_t i = 0; i < 20 * 2048;) {
        if (s.read(Spc::SSTS) & Spc::SSTS_EMPTY) {
            waits++;
            if (serve() == 0) {
                printf("  no chunk was requested at byte %u\n", i);
                failures++;
                break;
            }
            continue;
        }
        diff += s.read(Spc::DREG) != dataByte(5 + i / 2048, i % 2048);
        i++;
    }
    CHECK_EQ(diff, 0);
    if (waits == 0) {
        printf("  the transfer never waited for a chunk\n");
        failures++;
    }
    s.write(Spc::INTS, 0xff);
    CHECK_EQ(manualIn(s), 0);
    CHECK_EQ(manualIn(s), 0);

    // 再生: チャンクが読めるまでフレームは出ず、位置も進まない。読めたら続きから出る
    CdDrive& cd = *s.cd();
    CHECK_EQ(runCommand(s, 6, {0x45, 0, 0, 0, 0, 100, 0, 0, 40, 0}), 0);
    static int16_t out[2 * 4096];
    CHECK_EQ(cd.produce(40000, out, 4096), 0);
    CHECK_EQ(cd.position(), 100);
    CHECK_EQ(cd.playing(), true);
    uint32_t lba = 100, fr = 0;
    int total = 0, stalls = 0;
    for (int step = 0; step < 4000 && cd.playing(); step++) {
        const int n = checkFrames(cd, 40000, lba, fr);
        if (n == 0 && cd.playing()) {
            stalls++;
            serve();
        }
        total += n;
    }
    CHECK_EQ(total, 40 * 588);
    CHECK_EQ(cd.audioStatus(), CdDrive::AUDIO_DONE);
    if (stalls == 0) {
        printf("  the play never waited for a chunk\n");
        failures++;
    }
    // 先読みを頼むので、待つ回数はチャンクの数より少ない (40 ブロック = 94KB = 6 チャンク)
    if (stalls > 4) {
        printf("  the play waited %d times (the next chunks were not requested ahead)\n", stalls);
        failures++;
    }

    // 媒体を入れ替える: 同じキャッシュの枠を、別の大きさのイメージ (ISO、40 ブロック) に使い回す (DiskCache::reinit())
    static uint8_t iso[40 * 2048];
    for (uint32_t i = 0; i < sizeof iso; i++) iso[i] = (uint8_t)(i * 13 + (i >> 11));
    s.changeCd(nullptr);
    img.unload();
    CHECK_EQ(cache.reinit(sizeof iso), true);
    CHECK_EQ(cache.slots(), 8u);
    CHECK_EQ(img.loadCached(&cache, sizeof iso, 2048, true), true);
    s.changeCd(&img);
    CHECK_EQ(runCommand(s, 6, {0x00, 0, 0, 0, 0, 0}), 2); // UNIT ATTENTION
    CHECK_EQ(select(s, 6), true);
    const uint8_t cdb2[10] = {0x28, 0, 0, 0, 0, 30, 0, 0, 10, 0};
    CHECK_EQ(transferOut(s, Spc::PH_COMMAND, cdb2, 10), Spc::INTS_COMPLETE);
    s.write(Spc::PCTL, Spc::PH_DATA_IN);
    setTc(s, 10 * 2048);
    s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    diff = 0;
    for (uint32_t i = 0; i < 10 * 2048;) {
        if (s.read(Spc::SSTS) & Spc::SSTS_EMPTY) {
            // 前のイメージのチャンクが残っていれば、頼まずに古い中身を返してしまう
            const unsigned CHUNK2 = x68k::DiskCache::CHUNK;
            unsigned slot;
            uint32_t c, len;
            uint8_t* p;
            int n = 0;
            while (cache.nextLoad(slot, c, p, len)) {
                memcpy(p, iso + (size_t)c * CHUNK2, len);
                cache.loaded(slot, true);
                n++;
            }
            if (n == 0) {
                printf("  no chunk was requested at byte %u\n", i);
                failures++;
                break;
            }
            continue;
        }
        diff += s.read(Spc::DREG) != iso[30 * 2048 + i];
        i++;
    }
    CHECK_EQ(diff, 0);
}

// DiskCache::reinit(): 枠がすべて埋まっているとき、読み込みを頼んだままのときから作り直しても、新しいイメージを
// 最初から最後まで読める (前のイメージの中身を返さない、頼まれたままの枠も使える)
void testCacheReinit() {
    printf("cache reinit: from full / loading slots\n");
    makeBin();
    const uint32_t CHUNK = x68k::DiskCache::CHUNK;
    // 2 つ目のイメージは 1 つ目と同じ大きさ (チャンク → 枠の表が同じ大きさになり、古い表のメモリが使い回されやすい)
    static uint8_t second[sizeof g_bin];
    for (uint32_t i = 0; i < sizeof second; i++) second[i] = (uint8_t)(i * 29 + (i >> 9) + 1);
    const uint32_t chunks = (uint32_t)((sizeof g_bin + CHUNK - 1) / CHUNK);
    for (int loading = 0; loading < 2; loading++) {
        x68k::DiskCache* cache = new x68k::DiskCache;
        CHECK_EQ(cache->init(sizeof g_bin, 8), true);
        x68k::HardDiskImage img;
        CHECK_EQ(img.loadCached(cache, sizeof g_bin, 1, true), true);
        const uint8_t* file = g_bin;
        auto serve = [&]() {
            unsigned slot;
            uint32_t c, len;
            uint8_t* buf;
            int n = 0;
            while (cache->nextLoad(slot, c, buf, len)) {
                memcpy(buf, file + (size_t)c * CHUNK, len);
                cache->loaded(slot, true);
                n++;
            }
            return n;
        };
        // イメージの [off, off + len) を読んで file と比べる。読めなければ (頼んでも来なければ) false
        auto readAll = [&]() {
            int diff = 0;
            for (uint64_t off = 0; off < sizeof g_bin;) {
                uint32_t got = 0;
                bool failed = false;
                uint8_t* p = img.window(off, 4096, got, failed);
                if (!p) {
                    if (failed || serve() == 0) return false;
                    continue;
                }
                diff += memcmp(p, file + off, got) != 0;
                off += got;
            }
            return diff == 0;
        };
        CHECK_EQ(readAll(), true); // 8 個の枠がすべて埋まる
        if (loading) {
            // 8 個の枠すべてに読み込みを頼んだまま (読まない) にする
            bool failed = false;
            for (uint32_t c = 0; c < 8; c++) cache->lookup(c, true, failed);
            CHECK_EQ(cache->loadQueued(), true);
        }
        img.unload();
        CHECK_EQ(cache->reinit(sizeof second), true);
        CHECK_EQ(cache->loadQueued(), false); // 頼まれた読み込みは捨てる
        CHECK_EQ(img.loadCached(cache, sizeof second, 1, true), true);
        file = second;
        CHECK_EQ(readAll(), true);
        // もう一度、後ろから前へ (枠を空けながら読む)
        int diff = 0;
        for (uint32_t c = chunks; c-- > 0;) {
            uint32_t got = 0;
            bool failed = false;
            uint8_t* p = img.window((uint64_t)c * CHUNK, 16, got, failed);
            if (!p) {
                if (failed || serve() == 0) {
                    diff++;
                    continue;
                }
                p = img.window((uint64_t)c * CHUNK, 16, got, failed);
            }
            diff += !p || memcmp(p, second + (size_t)c * CHUNK, 16) != 0;
        }
        CHECK_EQ(diff, 0);
        // 大きいイメージに入れ替える (チャンク → 枠の表を作り直す)。そのあと、小さいイメージに戻す
        static uint8_t big[2 * sizeof g_bin + 5000];
        for (uint32_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 7 + (i >> 10) + 3);
        img.unload();
        CHECK_EQ(cache->reinit(sizeof big), true);
        CHECK_EQ(img.loadCached(cache, sizeof big, 1, true), true);
        file = big;
        diff = 0;
        for (uint64_t off = 0; off < sizeof big;) {
            uint32_t got = 0;
            bool failed = false;
            uint8_t* p = img.window(off, 4096, got, failed);
            if (!p) {
                if (failed || serve() == 0) {
                    diff++;
                    break;
                }
                continue;
            }
            diff += memcmp(p, big + off, got) != 0;
            off += got;
        }
        CHECK_EQ(diff, 0);
        img.unload();
        CHECK_EQ(cache->reinit(sizeof second), true);
        CHECK_EQ(img.loadCached(cache, sizeof second, 1, true), true);
        file = second;
        CHECK_EQ(readAll(), true);
        delete cache;
    }
}

// ---------------------------------------------------------------- Machine の上で: 音の出力とステート

const int64_t kStep = 320000; // 1 回に進める CPU のサイクル (16MHz で 20ms)

// 中身のない IPL ROM (リセットのあと、その場で回り続ける) の Machine
x68k::Machine* makeMachine() {
    static uint8_t ipl[x68k::Machine::IPL_SIZE];
    memset(ipl, 0xff, sizeof ipl);
    const uint8_t vec[] = {0x00, 0x00, 0x20, 0x00, 0x00, 0xff, 0x00, 0x10, 0x60, 0xfe};
    memcpy(ipl + 0x10000, vec, 8);
    memcpy(ipl + 0x10010, vec + 8, 2);
    x68k::Machine* m = new x68k::Machine;
    if (!m->ok() || !m->setIplRom(ipl, sizeof ipl)) return nullptr;
    return m;
}

// 正弦波のイメージ (オーディオのトラック 1 つ、150 セクタ = 2 秒): 左は 1kHz、右は 500Hz、振幅 16000
const uint32_t kToneSectors = 150;
uint8_t g_tone[kToneSectors * 2352];
const double kPi = 3.14159265358979323846;

void makeTone(CdToc& toc, double hzL = 1000.0, double hzR = 500.0) {
    for (uint32_t i = 0; i < kToneSectors * 588; i++) {
        const uint16_t l = (uint16_t)(int16_t)lrint(16000.0 * sin(2 * kPi * hzL * i / 44100.0));
        const uint16_t r = (uint16_t)(int16_t)lrint(16000.0 * sin(2 * kPi * hzR * i / 44100.0));
        uint8_t* p = g_tone + 4 * i;
        p[0] = (uint8_t)l;
        p[1] = (uint8_t)(l >> 8);
        p[2] = (uint8_t)r;
        p[3] = (uint8_t)(r >> 8);
    }
    toc = CdToc();
    toc.count = 1;
    toc.tracks[0].control = 0;
    toc.tracks[0].sector = 2352;
    toc.tracks[0].fileEnd = kToneSectors;
    toc.leadOut = kToneSectors;
}

// 16 ビットの列 (step おき) の、実効値と、負から正へ 0 を横切った回数
void measure(const int16_t* p, int n, int step, double& rms, int& crossings) {
    double sum = 0;
    crossings = 0;
    for (int i = 0; i < n; i++) {
        const int v = p[i * step];
        sum += (double)v * v;
        if (i > 0 && p[(i - 1) * step] < 0 && v >= 0) crossings++;
    }
    rms = n ? sqrt(sum / n) : 0;
}

void testSoundOutput() {
    printf("machine: cd audio in the sound output\n");
    CdToc toc;
    makeTone(toc);
    x68k::HardDiskImage img;
    CHECK_EQ(img.load(g_tone, sizeof g_tone, 1, true), true);
    x68k::Machine* m = makeMachine();
    if (!m) {
        printf("  cannot make the machine\n");
        failures++;
        return;
    }
    m->sound().setOutputRate(48000);
    m->attachScsi(6, &img, true, &toc);
    CHECK_EQ(m->cdDrive() != nullptr, true);
    m->powerOn();
    static int16_t pcm[2 * 65536];
    // 再生していない間は無音
    for (int i = 0; i < 10; i++) m->run(kStep);
    int n = m->sound().ring().drain(pcm, 65536);
    int nonzero = 0;
    for (int i = 0; i < 2 * n; i++) nonzero += pcm[i] != 0;
    CHECK_EQ(nonzero, 0);
    // 1 秒再生する。フレームごとに取り出す (リングは 340ms 分)
    CHECK_EQ(m->cdDrive()->play(0, 75), true);
    n = 0;
    const uint64_t c0 = m->cycles();
    for (int i = 0; i < 65; i++) {
        m->run(kStep);
        n += m->sound().ring().drain(pcm + 2 * n, 65536 - n);
    }
    const double seconds = (double)(m->cycles() - c0) / (double)m->cpuHz();
    CHECK_EQ(m->cdDrive()->audioStatus(), CdDrive::AUDIO_DONE);
    CHECK_EQ(m->cdDrive()->position(), 75);
    if (n < (int)(seconds * 48000) - 100 || n > (int)(seconds * 48000) + 100) {
        printf("  %d output frames in %.3f s\n", n, seconds);
        failures++;
    }
    // 始まりの 50ms を除いた 0.9 秒を調べる。出力は 1/2 にして足すので、振幅は 8000 に
    // 補間の減衰 (1kHz で 1% 未満) を掛けたもの。実効値は 8000 / sqrt(2) = 5657
    const int skip = 2400, len = 43200;
    double rmsL, rmsR;
    int crossL, crossR;
    measure(pcm + 2 * skip, len, 2, rmsL, crossL);
    measure(pcm + 2 * skip + 1, len, 2, rmsR, crossR);
    if (rmsL < 5657 * 0.97 || rmsL > 5657 * 1.01 || rmsR < 5657 * 0.97 || rmsR > 5657 * 1.01) {
        printf("  rms: left %.1f, right %.1f (want about 5657)\n", rmsL, rmsR);
        failures++;
    }
    // 0.9 秒に、左は 900 回、右は 450 回
    if (crossL < 899 || crossL > 901 || crossR < 449 || crossR > 451) {
        printf("  zero crossings: left %d, right %d (want 900 and 450)\n", crossL, crossR);
        failures++;
    }
    // 終わったあとは無音になる (フィルタの残りが消えたあと)
    for (int i = 0; i < 20; i++) m->run(kStep);
    m->sound().ring().drain(pcm, 65536);
    for (int i = 0; i < 5; i++) m->run(kStep);
    n = m->sound().ring().drain(pcm, 65536);
    int peak = 0;
    for (int i = 0; i < 2 * n; i++) peak = pcm[i] > peak ? pcm[i] : -pcm[i] > peak ? -pcm[i] : peak;
    if (n == 0 || peak > 2) {
        printf("  after the play: %d frames, peak %d\n", n, peak);
        failures++;
    }
    delete m;
}

// 16 ビットの列 (step おき、rate Hz) の中の、hz の正弦波の振幅
double toneAmplitude(const int16_t* p, int n, int step, double hz, double rate) {
    double re = 0, im = 0;
    for (int i = 0; i < n; i++) {
        re += p[i * step] * cos(2 * kPi * hz * i / rate);
        im += p[i * step] * sin(2 * kPi * hz * i / rate);
    }
    return n ? 2 * sqrt(re * re + im * im) / n : 0;
}

// 線形補間で周波数を from から直したときの、hz の正弦波の振幅の比 (sinc の 2 乗)
double lerpGain(double hz, double from) {
    const double x = kPi * hz / from;
    return (sin(x) / x) * (sin(x) / x);
}

// CD-ROM 装置のオーディオは X68000 の出力のフィルタ (低域通過フィルタ) を通らない: 10kHz の音の振幅が、
// 周波数を直すときの補間 (44.1kHz -> 62.5kHz -> 48kHz、どちらも線形補間) で決まる値になる。
// フィルタを通すと、10kHz はさらに 0.86 倍ほどになる (1kHz はほとんど変わらない)
void testNoOutputFilter() {
    printf("machine: cd audio does not pass the output filter\n");
    CdToc toc;
    makeTone(toc, 10000.0, 1000.0);
    x68k::HardDiskImage img;
    CHECK_EQ(img.load(g_tone, sizeof g_tone, 1, true), true);
    x68k::Machine* m = makeMachine();
    if (!m) {
        printf("  cannot make the machine\n");
        failures++;
        return;
    }
    m->sound().setOutputRate(48000);
    m->attachScsi(6, &img, true, &toc);
    m->powerOn();
    static int16_t pcm[2 * 65536];
    CHECK_EQ(m->cdDrive()->play(0, 75), true);
    int n = 0;
    for (int i = 0; i < 65; i++) {
        m->run(kStep);
        n += m->sound().ring().drain(pcm + 2 * n, 65536 - n);
    }
    CHECK_EQ(m->cdDrive()->audioStatus(), CdDrive::AUDIO_DONE);
    // 始まりの 50ms を除いた 0.9 秒。左が 10kHz、右が 1kHz
    const int skip = 2400, len = 43200;
    CHECK_EQ(n >= skip + len, true);
    const double high = toneAmplitude(pcm + 2 * skip, len, 2, 10000.0, 48000.0);
    const double low = toneAmplitude(pcm + 2 * skip + 1, len, 2, 1000.0, 48000.0);
    const double wantHigh = 8000 * lerpGain(10000.0, 44100.0) * lerpGain(10000.0, 62500.0); // 6187
    const double wantLow = 8000 * lerpGain(1000.0, 44100.0) * lerpGain(1000.0, 62500.0);    // 7980
    if (high < wantHigh * 0.97 || high > wantHigh * 1.03 || low < wantLow * 0.97 || low > wantLow * 1.03) {
        printf("  amplitude: 10kHz %.1f (want %.1f), 1kHz %.1f (want %.1f)\n", high, wantHigh, low, wantLow);
        failures++;
    }
    const double ratio = low > 0 ? high / low : 0, wantRatio = wantHigh / wantLow; // 0.775
    if (ratio < wantRatio * 0.97 || ratio > wantRatio * 1.03) {
        printf("  10kHz / 1kHz: %.4f (want %.4f)\n", ratio, wantRatio);
        failures++;
    }
    delete m;
}

// 鳴らすものを足して 16 ビットを超えた分は、上限と下限で止める (反対の符号に回らない)。
// CD-ROM 装置のオーディオだけなら 1/2 にして足すので超えない。MIDI の音源の音を足したときに超える
struct LoudMidi : x68k::MidiOut {
    void midiByte(uint8_t) override {}
    void mixFrame(int32_t& l, int32_t& r) override {
        l += 30000;
        r -= 30000;
    }
};

void testClip() {
    printf("sound: cd audio and midi are added after the filter and clipped\n");
    x68k::Sound* snd = new x68k::Sound;
    x68k::CdMixer* mx = new x68k::CdMixer;
    LoudMidi midi;
    snd->setOutputRate(48000);
    static int16_t pcm[2 * 4096];
    // 直流 (左 +32767、右 -32768) を 50ms: CD-ROM 装置のオーディオだけなら、1/2 の値のまま続く
    // (X68000 の出力の直流を切るフィルタを通らない)
    int n = 0;
    for (int i = 0; i < 50; i++) {
        for (int k = 0; k < 45; k++) mx->push(32767, -32768);
        snd->advance(4000, nullptr, nullptr, mx);
        n += snd->ring().drain(pcm + 2 * n, 4096 - n);
    }
    CHECK_EQ(n >= 2300, true);
    int bad = 0;
    for (int i = 1200; i < 2300; i++) bad += pcm[2 * i] != 16383 || pcm[2 * i + 1] != -16384;
    CHECK_EQ(bad, 0);
    // MIDI の音源の音 (左 +30000、右 -30000) を足すと、上限と下限で止まる
    n = 0;
    for (int i = 0; i < 20; i++) {
        for (int k = 0; k < 45; k++) mx->push(32767, -32768);
        snd->advance(4000, nullptr, &midi, mx);
        n += snd->ring().drain(pcm + 2 * n, 4096 - n);
    }
    bad = 0;
    for (int i = 0; i < n; i++) bad += pcm[2 * i] != 32767 || pcm[2 * i + 1] != -32768;
    CHECK_EQ(n > 900, true);
    CHECK_EQ(bad, 0);
    delete mx;
    delete snd;
}

// OPM のチャネル ch に、4 つのオペレータをすべて出力にした大きな音 (アルゴリズム 7、TL = 0) を設定して鳴らす
void loudOpm(x68k::Sound& snd, int ch) {
    auto w = [&](uint8_t reg, uint8_t v) {
        snd.opmWrite(1, reg);
        snd.opmWrite(3, v);
    };
    w((uint8_t)(0x20 + ch), 0xc7); // L / R、CON = 7
    w((uint8_t)(0x28 + ch), 0x4a);
    w((uint8_t)(0x30 + ch), 0);
    for (int s = 0; s < 4; s++) {
        const int base = s * 8 + ch;
        w((uint8_t)(0x40 + base), 1);
        w((uint8_t)(0x60 + base), 0);
        w((uint8_t)(0x80 + base), 31);
        w((uint8_t)(0xa0 + base), 0);
        w((uint8_t)(0xc0 + base), 0);
        w((uint8_t)(0xe0 + base), 15);
    }
    w(0x08, (uint8_t)(0x78 | ch));
}

// MIDI の音源がなくても、X68000 の音 (OPM) と CD-ROM 装置のオーディオを足して 16 ビットを超えた分は、上限と下限で止まる。
// 同じ OPM の音だけを作った出力に、CD-ROM 装置のオーディオの直流 (1/2 にした +16383 / -16384) を足して止めた値と同じになる
void testClipWithoutMidi() {
    printf("sound: opm and cd audio are clipped without midi\n");
    x68k::Sound* both = new x68k::Sound;
    x68k::Sound* only = new x68k::Sound;
    x68k::CdMixer* mx = new x68k::CdMixer;
    both->setOutputRate(48000);
    only->setOutputRate(48000);
    static int16_t pa[2 * 4096], pb[2 * 4096];
    // 先に CD-ROM 装置のオーディオだけを 5ms 流して、値を落ち着かせる
    for (int i = 0; i < 5; i++) {
        for (int k = 0; k < 45; k++) mx->push(32767, -32768);
        both->advance(4000, nullptr, nullptr, mx);
        only->advance(4000);
    }
    both->ring().drain(pa, 4096);
    only->ring().drain(pb, 4096);
    for (int ch = 0; ch < 2; ch++) {
        loudOpm(*both, ch);
        loudOpm(*only, ch);
    }
    int na = 0, nb = 0;
    for (int i = 0; i < 50; i++) {
        for (int k = 0; k < 45; k++) mx->push(32767, -32768);
        both->advance(4000, nullptr, nullptr, mx);
        only->advance(4000);
        na += both->ring().drain(pa + 2 * na, 4096 - na);
        nb += only->ring().drain(pb + 2 * nb, 4096 - nb);
    }
    CHECK_EQ(na, nb);
    CHECK_EQ(na >= 2300, true);
    int bad = 0, top = 0, bottom = 0;
    for (int i = 0; i < na && i < nb; i++) {
        const int32_t l = pb[2 * i] + 16383, r = pb[2 * i + 1] - 16384;
        const int32_t wantL = l > 32767 ? 32767 : l, wantR = r < -32768 ? -32768 : r;
        bad += pa[2 * i] != wantL || pa[2 * i + 1] != wantR;
        top += l > 32767;
        bottom += r < -32768;
    }
    CHECK_EQ(bad, 0);
    // 超えるフレームが実際にある (なければ、この試験は何も確かめていない)
    CHECK_EQ(top > 100, true);
    CHECK_EQ(bottom > 100, true);
    delete mx;
    delete only;
    delete both;
}

// 音の合成を別のコアで行うとき (記録を当てる側) の音が、1 つのコアのときと同じになる
void testJournal() {
    printf("machine: journal replay gives the same samples\n");
    CdToc toc;
    makeTone(toc);
    x68k::HardDiskImage imgA, imgB;
    CHECK_EQ(imgA.load(g_tone, sizeof g_tone, 1, true), true);
    CHECK_EQ(imgB.load(g_tone, sizeof g_tone, 1, true), true);
    x68k::Machine* a = makeMachine();
    x68k::Machine* b = makeMachine();
    if (!a || !b) {
        printf("  cannot make the machines\n");
        failures++;
        return;
    }
    a->sound().setOutputRate(48000);
    a->attachScsi(6, &imgA, true, &toc);
    b->attachScsi(6, &imgB, true, &toc);
    static uint32_t ring[1 << 16];
    x68k::VideoJournal journal;
    journal.init(ring, 16);
    x68k::Sound* synth = new x68k::Sound;
    synth->setOutputRate(48000);
    x68k::SoundReplayer* replayer = new x68k::SoundReplayer(journal, *synth);
    x68k::SoundLink link;
    link.attach(*b, journal);
    a->powerOn();
    b->powerOn();
    static int16_t pa[2 * 65536], pb[2 * 65536];
    int na = 0, nb = 0;
    // ためた記録を、合成用の Sound に当てる
    auto pump = [&]() {
        b->sound().flushJournal();
        journal.publish();
        while (replayer->step(1 << 20) != 0) {
        }
    };
    const size_t cap = x68k::stateSize(*a) + 65536;
    uint8_t* stateA = new uint8_t[cap];
    uint8_t* stateB = new uint8_t[cap];
    size_t sizeA = 0, sizeB = 0;
    for (int i = 0; i < 60; i++) {
        // 再生、ステートの保存、一時停止、再開、ステートの読み込み、止めて別のところから、リセット
        for (x68k::Machine* m : {a, b}) {
            CdDrive& cd = *m->cdDrive();
            if (i == 2) CHECK_EQ(cd.play(10, 120), true);
            if (i == 8) {
                // 保存と読み込みの前後は、state.cpp の先頭の説明のとおりに合成用の Sound と写し合う
                if (m == b) {
                    pump();
                    x68k::copySoundState(*synth, b->sound());
                }
                CHECK_EQ(x68k::saveState(*m, m == a ? stateA : stateB, cap, m == a ? sizeA : sizeB), true);
            }
            if (i == 30) {
                // 再生の途中のステートを読む。そのあとの記録は前のフレームとの差で書くので、記録する側と読み手の
                // 「前のフレーム」がずれると音が変わる
                const char* err = nullptr;
                CHECK_EQ(x68k::loadState(*m, m == a ? stateA : stateB, m == a ? sizeA : sizeB, err), true);
                if (m == b) {
                    pump();
                    x68k::copySoundState(b->sound(), *synth);
                }
                CHECK_EQ(cd.playing(), true);
            }
            if (i == 20) CHECK_EQ(cd.pause(false), true);
            if (i == 25) CHECK_EQ(cd.pause(true), true);
            if (i == 40) CHECK_EQ(cd.play(3, 30), true);
            if (i == 50) m->powerOn();
            if (i == 52) CHECK_EQ(cd.play(100, 150), true);
        }
        a->run(kStep);
        b->run(kStep);
        pump();
        na += a->sound().ring().drain(pa + 2 * na, 65536 - na);
        nb += synth->ring().drain(pb + 2 * nb, 65536 - nb);
    }
    CHECK_EQ(na, nb);
    CHECK_EQ(a->cdDrive()->position(), b->cdDrive()->position());
    int diff = 0, peak = 0;
    for (int i = 0; i < 2 * na && i < 2 * nb; i++) {
        diff += pa[i] != pb[i];
        if (pa[i] > peak) peak = pa[i];
    }
    CHECK_EQ(diff, 0);
    if (na < 48000 || peak < 7000) {
        printf("  %d frames, peak %d\n", na, peak);
        failures++;
    }
    link.detach(*b);
    delete[] stateA;
    delete[] stateB;
    delete replayer;
    delete synth;
    delete a;
    delete b;
}

// ステートに再生の状態が入る。読んだあと、同じところから同じ音が続く
void testState() {
    printf("machine: state keeps the play position\n");
    CdToc toc;
    makeTone(toc);
    x68k::HardDiskImage img;
    CHECK_EQ(img.load(g_tone, sizeof g_tone, 1, true), true);
    x68k::Machine* m = makeMachine();
    if (!m) {
        printf("  cannot make the machine\n");
        failures++;
        return;
    }
    m->sound().setOutputRate(48000);
    m->attachScsi(6, &img, true, &toc);
    m->powerOn();
    CdDrive& cd = *m->cdDrive();
    CHECK_EQ(cd.play(5, 140), true);
    cd.setPrevent(true);
    for (int i = 0; i < 12; i++) m->run(kStep);
    const size_t cap = x68k::stateSize(*m) + 4096; // 版 11 までの並びは、SPC の状態が今より大きい
    uint8_t* state = new uint8_t[cap];
    size_t size = 0;
    CHECK_EQ(x68k::saveState(*m, state, cap, size), true);
    const uint32_t pos = cd.position();
    static int16_t pa[2 * 16384], pb[2 * 16384];
    m->sound().ring().drain(pa, 16384);
    for (int i = 0; i < 6; i++) m->run(kStep);
    const int na = m->sound().ring().drain(pa, 16384);
    const uint32_t posAfter = cd.position();
    // 止めて、禁止を解いてから読む: 再生中に戻り、同じ位置から同じ音が出る
    cd.stop();
    cd.setPrevent(false);
    for (int i = 0; i < 3; i++) m->run(kStep);
    m->sound().ring().drain(pb, 16384);
    const char* err = nullptr;
    CHECK_EQ(x68k::loadState(*m, state, size, err), true);
    CHECK_EQ(cd.playing(), true);
    CHECK_EQ(cd.prevented(), true);
    CHECK_EQ(cd.position(), pos);
    for (int i = 0; i < 6; i++) m->run(kStep);
    const int nb = m->sound().ring().drain(pb, 16384);
    CHECK_EQ(cd.position(), posAfter);
    CHECK_EQ(na, nb);
    // 44.1kHz から直すときの補間の位相と、CdMixer の中のフレームはステートに入らない (環境のもの) ので、音は
    // 1 フレーム (1/44100 秒) 未満ずれることがある。1kHz、振幅 8000 の正弦波では、その差は 1140 まで。
    // 読んだ直後も確かめる: 止まっていたところから鳴らし始めるので、出力の始めの 3 フレームは 0 から上がる途中の値
    // (0 と続きの音の間。雑音にならない)。そのあとは、始めから差が 1140 までに入る
    auto diffFrom = [&](int first) {
        int worst = 0;
        for (int i = 2 * first; i < 2 * na && i < 2 * nb; i++) {
            const int d = pa[i] > pb[i] ? pa[i] - pb[i] : pb[i] - pa[i];
            if (d > worst) worst = d;
        }
        return worst;
    };
    // 出力の始めの count フレームの、絶対値の最大
    auto peakOf = [&](int count) {
        int peak = 0;
        for (int i = 0; i < 2 * count && i < 2 * nb; i++) peak = pb[i] > peak ? pb[i] : -pb[i] > peak ? -pb[i] : peak;
        return peak;
    };
    int worst = diffFrom(3);
    if (na < 4000 || worst > 1200) {
        printf("  %d frames, the largest difference is %d\n", na, worst);
        failures++;
    }
    for (int i = 0; i < 2 * 3; i++) {
        // 0 と、続きの音 (pa) の間 (補間の位相の分の 1200 を足した範囲)
        const int lo = (pa[i] < 0 ? pa[i] : 0) - 1200, hi = (pa[i] > 0 ? pa[i] : 0) + 1200;
        if (pb[i] < lo || pb[i] > hi) {
            printf("  output %d right after the load: %d (the original is %d)\n", i, pb[i], pa[i]);
            failures++;
        }
    }
    // 別のところ (位相の違う音) を再生している途中で読む: CdMixer に残っている前の音のフレーム (2 つまで) を出してから、
    // ステートの位置の音が続く。前の音のフレームが残る分、音は 2 フレーム (1/44100 秒 x 2) までずれるので、差は 2280 まで。
    // 始めの 6 フレーム (前の音から続きの音へ移る間) も、どちらかの音の振幅 (8000) を超えない
    CHECK_EQ(cd.play(41, 140), true);
    for (int i = 0; i < 3; i++) m->run(kStep);
    m->sound().ring().drain(pb, 16384);
    CHECK_EQ(cd.position() != pos, true);
    CHECK_EQ(x68k::loadState(*m, state, size, err), true);
    CHECK_EQ(cd.position(), pos);
    for (int i = 0; i < 6; i++) m->run(kStep);
    CHECK_EQ(m->sound().ring().drain(pb, 16384), na);
    CHECK_EQ(cd.position(), posAfter);
    worst = diffFrom(6);
    const int peak = peakOf(na);
    if (worst > 2400 || peak > 8100) {
        printf("  load during another play: the largest difference is %d, the peak is %d\n", worst, peak);
        failures++;
    }
    // 媒体がないときに読むと、再生は止まる
    m->changeCd(nullptr);
    CHECK_EQ(x68k::loadState(*m, state, size, err), true);
    CHECK_EQ(cd.playing(), false);
    // 前の版 (6 と 7) のステートも読める (CD-ROM 装置の状態は入っていないので、再生は止まり、禁止は解ける)
    m->changeCd(&img, &toc);
    for (uint32_t version : {6u, 7u}) {
        CHECK_EQ(x68k::saveState(*m, state, cap, size, version), true);
        CHECK_EQ(state[8], version);
        CHECK_EQ(cd.play(5, 140), true);
        cd.setPrevent(true);
        CHECK_EQ(x68k::loadState(*m, state, size, err), true);
        CHECK_EQ(cd.playing(), false);
        CHECK_EQ(cd.prevented(), false);
    }
    // 版 8 は、版 7 より SPC 2 つ分の CD-ROM 装置の状態だけ大きい (版 11 までは、装置がなくても入っていた)
    size_t size7 = 0, size8 = 0;
    CHECK_EQ(x68k::saveState(*m, state, cap, size7, 7), true);
    CHECK_EQ(x68k::saveState(*m, state, cap, size8, 8), true);
    CHECK_EQ(state[8], 8);
    CHECK_EQ(size8, size7 + 2 * 1058);
    delete[] state;
    delete m;
}

// データインの途中でステートを保存し、ほかのことをしてから読んで、続きを受け取る。受け取った全体を out に入れ、
// バイト数を返す (2352 バイト / セクタのイメージの READ では、セクタの並びが戻らないと続きが違うデータになる)
int midTransfer(const uint8_t* cdb, uint32_t total, uint32_t cut, uint8_t* out) {
    makeBin();
    CdToc toc;
    makeToc(toc);
    x68k::HardDiskImage img;
    img.load(g_bin, sizeof g_bin, 1, true);
    x68k::Machine* m = makeMachine();
    if (!m) return -1;
    m->attachScsi(6, &img, true, &toc);
    m->powerOn();
    Spc& s = m->spc();
    initSpc(s);
    if (!select(s, 6)) return -1;
    transferOut(s, Spc::PH_COMMAND, cdb, 10);
    s.write(Spc::PCTL, Spc::PH_DATA_IN);
    setTc(s, total);
    s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    uint32_t i = 0;
    for (; i < cut; i++) out[i] = s.read(Spc::DREG);
    const size_t cap = x68k::stateSize(*m) + 4096; // 版 11 までの並びは、SPC の状態が今より大きい
    uint8_t* st = new uint8_t[cap];
    size_t n = 0;
    if (!x68k::saveState(*m, st, cap, n)) return -2;
    // 残りを受け取り、リセットしてから読む
    // (ステータスフェーズの REQ が来ている間も Transfer in Progress は 1 なので、FIFO が空になったらやめる)
    while ((s.read(Spc::SSTS) & (Spc::SSTS_TRIP | Spc::SSTS_EMPTY)) == Spc::SSTS_TRIP) s.read(Spc::DREG);
    m->powerOn();
    initSpc(s);
    const char* err = nullptr;
    if (!x68k::loadState(*m, st, n, err)) return -3;
    while (i < total && (s.read(Spc::SSTS) & Spc::SSTS_TRIP)) {
        if (s.read(Spc::SSTS) & Spc::SSTS_EMPTY) break;
        out[i++] = s.read(Spc::DREG);
    }
    delete[] st;
    delete m;
    return (int)i;
}

void testStateMidTransfer() {
    printf("machine: state in the middle of a read from 2352-byte sectors / read toc\n");
    static uint8_t out[4 * 2048];
    // READ(10): 論理ブロック 5 から 4 ブロック。セクタの中、境目、次のセクタで切る
    const uint8_t rd[10] = {0x28, 0, 0, 0, 0, 5, 0, 0, 4, 0};
    for (uint32_t cut : {1u, 2047u, 2048u, 2049u, 3000u, 8191u}) {
        memset(out, 0xaa, sizeof out);
        CHECK_EQ(midTransfer(rd, 4 * 2048, cut, out), 4 * 2048);
        int diff = 0;
        for (uint32_t i = 0; i < 4 * 2048; i++) diff += out[i] != dataByte(5 + i / 2048, i % 2048);
        CHECK_EQ(diff, 0);
    }
    // READ TOC: 応答は CdDrive の中のバッファにある
    const uint8_t tocCmd[10] = {0x43, 0, 0, 0, 0, 0, 0, 0, 36, 0};
    static uint8_t ref[36];
    CHECK_EQ(midTransfer(tocCmd, 36, 36, ref), 36);
    CHECK_EQ(ref[3], 3);
    for (uint32_t cut : {1u, 10u, 35u}) {
        memset(out, 0xaa, 36);
        CHECK_EQ(midTransfer(tocCmd, 36, cut, out), 36);
        CHECK_EQ(memcmp(out, ref, 36), 0);
    }
}

// CD-ROM 装置の状態を足す前の実装が書いたステート (old_state_data.h) を読む
void testOldState() {
    printf("machine: states written by the implementation before the cd-rom state\n");
    static uint8_t old[65536];
    size_t size = 0;
    for (size_t i = 0; i + 1 < sizeof kOldStateV6; i += 2)
        for (int k = 0; k < kOldStateV6[i] && size < sizeof old; k++) old[size++] = kOldStateV6[i + 1];
    CHECK_EQ(size, 48580);
    static uint8_t iso[4 * 2048];
    x68k::HardDiskImage img;
    CHECK_EQ(img.load(iso, sizeof iso, 2048, true), true);
    x68k::Machine* m = makeMachine();
    if (!m) {
        printf("  cannot make the machine\n");
        failures++;
        return;
    }
    m->attachScsi(6, &img, true);
    m->powerOn();
    CdDrive& cd = *m->cdDrive();
    const size_t cap = x68k::stateSize(*m) + 4096; // 版 11 までの並びは、SPC の状態が今より大きい
    uint8_t* again = new uint8_t[cap];
    for (uint8_t version : {(uint8_t)6, (uint8_t)7}) {
        old[8] = version; // 版 7 のデータは、版の番号だけが違う
        m->run(kStep);
        cd.setPrevent(true);
        const char* err = nullptr;
        CHECK_EQ(x68k::loadState(*m, old, size, err), true);
        if (err) printf("  version %d: %s\n", version, err);
        CHECK_EQ(cd.prevented(), false); // CD-ROM 装置の状態は入っていないので、リセットしたときと同じ
        CHECK_EQ(cd.playing(), false);
        // 読んだ状態を同じ版で保存し直すと、同じ大きさになり、また読める (SASI と SPC の状態は 0 で埋まるので、
        // バイト列は同じにならない: 版 11 までの SASI と SPC の状態は、読み飛ばすだけで持っていない)
        size_t n = 0;
        CHECK_EQ(x68k::saveState(*m, again, cap, n, version), true);
        CHECK_EQ(n, size);
        CHECK_EQ(x68k::loadState(*m, again, n, err), true);
        // そのまま動かせる
        m->run(kStep);
        CHECK_EQ(cd.play(0, 1), false); // データトラックだけ
    }
    // 壊したデータは読まない
    old[100] ^= 1;
    const char* err = nullptr;
    CHECK_EQ(x68k::loadState(*m, old, size, err), false);
    delete[] again;
    delete m;
}

} // namespace

int main() {
    testCue();
    testTocAndRead();
    testModePages();
    testMediumChange();
    testPlay();
    testPlayIntoData();
    testMixer();
    testMixerStep();
    testJournalDelta();
    testCached();
    testCacheReinit();
    testSoundOutput();
    testNoOutputFilter();
    testClip();
    testClipWithoutMidi();
    testJournal();
    testState();
    testStateMidTransfer();
    testOldState();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
