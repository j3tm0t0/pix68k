// platform/floppy_restore.h (XDF を実物のディスクに書き戻す手順。#386) のテスト。ドライブは偽物 (fake_ufi.h)
#include "platform/floppy_restore.h"
#include "fake_ufi.h"
#include "test_check.h"

namespace {

uint8_t g_old[FloppyRestore::IMAGE_BYTES], g_image[FloppyRestore::IMAGE_BYTES], g_start[FloppyRestore::IMAGE_BYTES];
uint8_t g_work[FloppyRestore::TRACK_BYTES];

struct Progress : FloppyRestoreProgress {
    int cancelAt = -1, calls = 0;
    bool progress(Phase, uint32_t done, uint32_t total) override {
        CHECK_EQ(done <= total, true);
        return calls++ != cancelAt;
    }
};

bool oneTrackTorn(const uint8_t* disk, const uint8_t* a, const uint8_t* b) {
    // どのブロックも、前の中身か、新しい中身か、(トラック 0 だけ) 0 のどれか
    for (uint32_t i = 0; i < UfiDisk::BLOCKS; i++) {
        const uint8_t* d = disk + (size_t)i * 1024;
        bool zero = i < 8;
        for (int k = 0; k < 1024 && zero; k++) zero = d[k] == 0;
        if (!zero && memcmp(d, a + (size_t)i * 1024, 1024) != 0 && memcmp(d, b + (size_t)i * 1024, 1024) != 0) return false;
    }
    return true;
}

// 控えを残してから、入れ替わっていないかを見る
bool same(FloppyRestore& r, Progress* p) { return r.backupDone(true, r.oldCrc()) && r.checkSame(p); }

void testOk() {
    FakeUfi f;
    memcpy(g_start, f.disk, sizeof g_start);
    FloppyRestore r;
    Progress p;
    CHECK_EQ(r.write(g_image, &p), false); // 順番どおりでなければ断る
    FloppyRestore r2;
    CHECK_EQ(r2.prepare(&f, g_old, g_work, &p), true);
    CHECK_EQ(memcmp(g_old, g_start, sizeof g_old), 0);
    CHECK_EQ(r2.oldCrc(), crc32Of(g_start, sizeof g_start));
    CHECK_EQ(f.writes, 0);
    FloppyRestore r3;
    FakeUfi f3;
    CHECK_EQ(r3.prepare(&f3, g_old, g_work, &p), true);
    CHECK_EQ(r3.write(g_image, &p), false); // checkSame() を飛ばした
    CHECK_EQ(f3.writes, 0);
    CHECK_EQ(same(r2, &p), true);
    CHECK_EQ(r2.touched(), false);
    CHECK_EQ(r2.write(g_image, &p), true);
    CHECK_EQ(r2.touched(), true);
    CHECK_EQ(r2.verify(&p), true);
    CHECK_EQ(memcmp(f.disk, g_image, sizeof g_image), 0);
    CHECK_EQ(f.writes, 155); // トラック 0 は 2 回
    CHECK_EQ(r2.state(), FloppyRestore::VERIFIED);
}

void testRefuse() {
    Progress p;
    {
        FakeUfi f;
        f.wp = true;
        FloppyRestore r;
        CHECK_EQ(r.prepare(&f, g_old, g_work, &p), false);
        CHECK_EQ(r.error(), FloppyRestore::PROTECTED);
        CHECK_EQ(f.writes, 0);
    }
    {
        FakeUfi f;
        f.blockBytes = 512;
        FloppyRestore r;
        CHECK_EQ(r.prepare(&f, g_old, g_work, &p), false);
        CHECK_EQ(r.error(), FloppyRestore::CANNOT_OPEN);
    }
    // 読めないセクタのほかの失敗なら、読むのをやめる
    {
        FakeUfi f;
        f.breakAt = 20;
        FloppyRestore r;
        CHECK_EQ(r.prepare(&f, g_old, g_work, &p), false);
        CHECK_EQ(r.error(), FloppyRestore::READ_ERROR);
        CHECK_EQ(r.partial(), false);
        CHECK_EQ(same(r, &p), false);
        CHECK_EQ(f.writes, 0);
    }
    // 控えを残せない、読み戻すと違う (同じ名前の別のファイルがある)、控えを確かめていない
    for (int c = 0; c < 3; c++) {
        FakeUfi f;
        FloppyRestore r;
        CHECK_EQ(r.prepare(&f, g_old, g_work, &p), true);
        if (c < 2) CHECK_EQ(r.backupDone(c == 1, r.oldCrc() ^ (c == 1 ? 1 : 0)), false);
        CHECK_EQ(r.checkSame(&p), false);
        if (c == 2) CHECK_EQ(r.error(), FloppyRestore::NO_BACKUP);
        CHECK_EQ(r.write(g_image, &p), false);
        CHECK_EQ(r.touched(), false);
        CHECK_EQ(f.writes, 0);
    }
    {
        FakeUfi f;
        FloppyRestore r;
        Progress c;
        c.cancelAt = 5;
        CHECK_EQ(r.prepare(&f, g_old, g_work, &c), false);
        CHECK_EQ(r.error(), FloppyRestore::CANCELED);
        CHECK_EQ(r.write(g_image, &c), false);
        CHECK_EQ(f.writes, 0);
    }
    // 尋ねている間の入れ替え: 知らせるドライブと、知らせないドライブ
    for (int tells = 0; tells < 2; tells++) {
        FakeUfi f;
        FloppyRestore r;
        CHECK_EQ(r.prepare(&f, g_old, g_work, &p), true);
        uint8_t* x = f.disk;
        f.disk = f.other;
        f.other = x;
        if (tells) f.attention = 0x28;
        CHECK_EQ(same(r, &p), false);
        CHECK_EQ(r.error(), FloppyRestore::SWAPPED);
        CHECK_EQ(r.write(g_image, &p), false);
        CHECK_EQ(r.touched(), false);
        CHECK_EQ(f.writes, 0);
    }
    // 書いてみたら書き込み禁止
    {
        FakeUfi f;
        f.modeSense = false;
        f.wp = true;
        FloppyRestore r;
        CHECK_EQ(r.prepare(&f, g_old, g_work, &p), true);
        CHECK_EQ(same(r, &p), true);
        CHECK_EQ(r.write(g_image, &p), false);
        CHECK_EQ(r.error(), FloppyRestore::PROTECTED);
        CHECK_EQ(r.touched(), false);
    }
}

// 読めないセクタのあるディスク: 読めた分を控え、欠けたままでよいと認めたときだけ書ける
void testPartial() {
    Progress p;
    for (int c = 0; c < 4; c++) {
        FakeUfi f;
        f.stallData = c & 1;
        f.unreadable[0] = f.unreadable[500] = f.unreadable[503] = f.unreadable[1231] = true;
        FloppyRestore r;
        CHECK_EQ(r.prepare(&f, g_old, g_work, &p), true);
        CHECK_EQ(r.partial(), true);
        CHECK_EQ(r.badBlocks(), 4u);
        CHECK_EQ(r.badTracks(), 3u);
        int marked = 0;
        for (uint32_t b = 0; b < FloppyRestore::BLOCKS; b++) {
            const uint8_t* o = g_old + (size_t)b * 1024;
            CHECK_EQ(r.badBlock(b), f.unreadable[b]);
            if (r.badBlock(b)) marked += memcmp(o, "pix68k: cannot read this sector\npix68k", 38) == 0;
            else CHECK_EQ(memcmp(o, f.disk + (size_t)b * 1024, 1024), 0);
        }
        CHECK_EQ(marked, 4);
        CHECK_EQ(r.badTrack(0) && r.badTrack(62) && r.badTrack(153) && !r.badTrack(1), true);
        if (c >= 2) r.acceptPartial();
        CHECK_EQ(same(r, &p), c >= 2);
        if (c < 2) CHECK_EQ(r.error(), FloppyRestore::NOT_ACCEPTED);
        CHECK_EQ(r.write(g_image, &p), c >= 2);
        if (c < 2) {
            CHECK_EQ(f.writes, 0);
            continue;
        }
        CHECK_EQ(r.verify(&p), true);
        CHECK_EQ(memcmp(f.disk, g_image, sizeof g_image), 0);
    }
    // 入れ替わりは、読めたトラックで見つける
    {
        FakeUfi f;
        for (uint32_t b = 0; b < 1224; b++) f.unreadable[b] = true; // トラック 153 だけ読める
        FloppyRestore r;
        CHECK_EQ(r.prepare(&f, g_old, g_work, &p), true);
        r.acceptPartial();
        f.disk[1230 * 1024] ^= 1;
        CHECK_EQ(same(r, &p), false);
        CHECK_EQ(r.error(), FloppyRestore::SWAPPED);
        CHECK_EQ(f.writes, 0);
    }
    // 読めるトラックがない (入れ替わりを見分けられない。物理フォーマットもないかもしれない)
    {
        FakeUfi f;
        for (uint32_t b = 0; b < 1232; b += 8) f.unreadable[b] = true;
        FloppyRestore r;
        CHECK_EQ(r.prepare(&f, g_old, g_work, &p), false);
        CHECK_EQ(r.error(), FloppyRestore::UNREADABLE);
        CHECK_EQ(f.writes, 0);
    }
    // 読めず、書けないセクタ (物理フォーマットが崩れている): 書く途中で止まる
    {
        FakeUfi f;
        f.badBlock = 500;
        FloppyRestore r;
        CHECK_EQ(r.prepare(&f, g_old, g_work, &p), true);
        CHECK_EQ(r.badBlocks(), 1u);
        r.acceptPartial();
        CHECK_EQ(same(r, &p), true);
        CHECK_EQ(r.write(g_image, &p), false);
        CHECK_EQ(r.error(), FloppyRestore::WRITE_ERROR);
        CHECK_EQ(r.track(), 62u);
        CHECK_EQ(r.touched(), true);
    }
    // 読めないセクタを読んでいる間も、Esc でやめられる
    {
        FakeUfi f;
        f.unreadable[500] = true;
        FloppyRestore r;
        Progress c;
        c.cancelAt = 65;
        CHECK_EQ(r.prepare(&f, g_old, g_work, &c), false);
        CHECK_EQ(r.error(), FloppyRestore::CANCELED);
    }
    // 最初の 1 回が断られたら、書いていない。書いた結果が分からなければ、書き始めたとみなす
    for (int c = 0; c < 3; c++) {
        FakeUfi f;
        FloppyRestore r;
        CHECK_EQ(r.prepare(&f, g_old, g_work, &p) && same(r, &p), true);
        if (c == 0) f.attention = 0x28;
        if (c == 1) f.notReady = 1;
        if (c == 2) f.breakAt = f.commands;
        CHECK_EQ(r.write(g_image, &p), false);
        CHECK_EQ(r.touched(), c != 0);
    }
}

// 3 つおきのコマンドで抜く。書き始める前なら中身は変わらない
void testCut() {
    FakeUfi whole;
    Progress p;
    FloppyRestore w;
    CHECK_EQ(w.prepare(&whole, g_old, g_work, &p) && same(w, &p) && w.write(g_image, &p) && w.verify(&p), true);
    const int total = whole.commands;
    for (int torn = 0; torn < 2; torn++)
        for (int at = torn; at < total; at += 3) {
            FakeUfi f;
            memcpy(g_start, f.disk, sizeof g_start);
            f.breakAt = at;
            f.tornBlocks = torn ? 5 : 0;
            f.tornBad = torn != 0;
            FloppyRestore r;
            const bool ok = r.prepare(&f, g_old, g_work, &p) && same(r, &p) && r.write(g_image, &p) && r.verify(&p);
            CHECK_EQ(ok, false);
            CHECK_EQ(r.state(), FloppyRestore::FAILED);
            const int c = f.commands;
            if (!r.touched()) CHECK_EQ(memcmp(f.disk, g_start, sizeof g_start), 0);
            CHECK_EQ(oneTrackTorn(f.disk, g_start, g_image), true);
            f.dead = false;
            r.verify(&p);
            r.write(g_image, &p);
            CHECK_EQ(f.commands, c);
            // 最後の書き込み (トラック 0) が届く前に切れたら、先頭のトラックは 0 のまま (最初の 1 回が届いていれば)
            if (f.writes > 0 && f.writes < 154 + 1 && !(f.writes == 154 && torn)) {
                bool zero = true;
                for (size_t k = 0; k < 8192; k++) zero = zero && f.disk[k] == 0;
                CHECK_EQ(zero, true);
            }
            // 書きかけのセクタが読めなくなっても、やり直せる
            if (f.writes > 0 && at % 60 < 3) {
                f.breakAt = -1;
                FloppyRestore again;
                CHECK_EQ(again.prepare(&f, g_old, g_work, &p), true);
                if (again.partial()) again.acceptPartial();
                CHECK_EQ(same(again, &p) && again.write(g_image, &p) && again.verify(&p), true);
                CHECK_EQ(memcmp(f.disk, g_image, sizeof g_image), 0);
                continue;
            }
            // 書きかけのディスクの先頭は、前のブートセクタのままではない (最初の 1 回が届いていれば)
            if (f.writes > 0) CHECK_EQ(memcmp(f.disk, g_start, 1024) != 0, true);
        }
}

// どのコマンドで入れ替えられても (知らせるドライブ)、入れ替えたあとのディスクには書かない
void testSwap() {
    FakeUfi whole;
    Progress p;
    FloppyRestore w;
    w.prepare(&whole, g_old, g_work, &p) && same(w, &p) && w.write(g_image, &p) && w.verify(&p);
    const int total = whole.commands;
    for (int at = 3; at < total; at += 2) { // 開き終えたあと
        FakeUfi f;
        memcpy(g_start, f.other, sizeof g_start);
        f.changeAt = at;
        FloppyRestore r;
        const bool ok = r.prepare(&f, g_old, g_work, &p) && same(r, &p) && r.write(g_image, &p) && r.verify(&p);
        CHECK_EQ(ok, false);
        CHECK_EQ(memcmp(f.disk, g_start, sizeof g_start), 0);
    }
}

void testVerify() {
    Progress p;
    FakeUfi f;
    FloppyRestore r;
    CHECK_EQ(r.prepare(&f, g_old, g_work, &p), true);
    CHECK_EQ(same(r, &p), true);
    CHECK_EQ(r.write(g_image, &p), true);
    const int writes = f.writes;
    f.disk[3 * 8192 + 2100] ^= 1; // トラック 3 の R = 3 が違う
    CHECK_EQ(r.verify(&p), false);
    CHECK_EQ(r.error(), FloppyRestore::MISMATCH);
    CHECK_EQ(r.track(), 3u);
    CHECK_EQ(r.block(), 2u);
    CHECK_EQ(f.writes, writes); // 書き直さない
    // 先頭のトラックの最初と最後、最後のトラックの最後、真ん中のトラックの R = 8 のどれが違っても見つける
    static const uint32_t kBad[] = {0, 7, 8, 15, 616 + 7, 1231};
    for (uint32_t b : kBad)
        for (int end = 0; end < 2; end++) {
            FakeUfi x;
            FloppyRestore q;
            CHECK_EQ(q.prepare(&x, g_old, g_work, &p) && same(q, &p) && q.write(g_image, &p), true);
            x.disk[(size_t)b * 1024 + (end ? 1023 : 0)] ^= 0x80;
            CHECK_EQ(q.verify(&p), false);
            CHECK_EQ(q.error(), FloppyRestore::MISMATCH);
            CHECK_EQ(q.track(), b / 8);
            CHECK_EQ(q.block(), b % 8);
        }
    // 読み戻す順番は 1〜153、最後に 0 (2 か所が違えば、先に見つかるほう)
    for (uint32_t t = 1; t < 154; t += 152) {
        FakeUfi x;
        FloppyRestore q;
        CHECK_EQ(q.prepare(&x, g_old, g_work, &p) && same(q, &p) && q.write(g_image, &p), true);
        x.disk[0] ^= 1;
        x.disk[(size_t)t * 8192] ^= 1;
        CHECK_EQ(q.verify(&p), false);
        CHECK_EQ(q.track(), t);
    }
    // 控えのバッファを書かせない
    {
        FakeUfi x;
        FloppyRestore q;
        CHECK_EQ(q.prepare(&x, g_old, g_work, &p) && same(q, &p), true);
        CHECK_EQ(q.write(g_old, &p), false);
        CHECK_EQ(x.writes, 0);
    }
    // 尋ねている間の入れ替え: 途中のトラックだけが違う別のディスクも、見るトラック (11 おき) なら見つける
    for (uint32_t t = 0; t < FloppyRestore::TRACKS; t += FloppyRestore::CHECK_STEP) {
        CHECK_EQ(FloppyRestore::CHECK_STEP, 11u);
        FakeUfi x;
        FloppyRestore q;
        CHECK_EQ(q.prepare(&x, g_old, g_work, &p), true);
        x.disk[(size_t)t * 8192 + 5000] ^= 1;
        CHECK_EQ(same(q, &p), false);
        CHECK_EQ(q.error(), FloppyRestore::SWAPPED);
    }
    // 書いたものが化けても、成功と答えるドライブ
    FakeUfi f2;
    f2.corruptWrites = 1000;
    FloppyRestore r2;
    CHECK_EQ(r2.prepare(&f2, g_old, g_work, &p) && same(r2, &p) && r2.write(g_image, &p), true);
    CHECK_EQ(r2.verify(&p), false);
    CHECK_EQ(r2.error(), FloppyRestore::MISMATCH);
    // 確認 (設定メニュー) の前は、手順を進めても 1 つも書けない
    UfiGate::confirm(false);
    FakeUfi f3;
    FloppyRestore r3;
    CHECK_EQ(r3.prepare(&f3, g_old, g_work, &p) && same(r3, &p), true);
    CHECK_EQ(r3.write(g_image, &p), false);
    CHECK_EQ(f3.writes, 0);
    UfiGate::confirm(true);
}

} // namespace

int main() {
    UfiGate::enable(0);
    UfiGate::confirm(true);
    for (size_t i = 0; i < sizeof g_image; i++) g_image[i] = (uint8_t)(i * 5 + (i >> 9) + 1);
    testOk();
    testRefuse();
    testPartial();
    testCut();
    testSwap();
    testVerify();
    return finish();
}
