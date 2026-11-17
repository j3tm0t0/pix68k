// ディスクのイメージの書き戻し (x68k/dirty_map.h、platform/disk_sync.h) と、大きなイメージの読み込みのキャッシュ
// (x68k/disk_cache.h) のテスト。作り物のファイルシステムで、書く時期 (書き込みが止んでから、書き続けているとき)、
// 書いた中身、書き込みと読み込みの失敗 (dirty フラグを下ろしたあとで書けなかった分を落とさない、書き戻す前に枠を空けて古い中身を
// 読み直さない) を調べる。
// 最後に、コア 1 (X68000) とコア 0 (SD カード) を 2 つのスレッドにして受け渡しを動かす
// (-fsanitize=thread でビルドすると、データの競合も調べられる)。
#include "platform/disk_cache_size.h"
#include "platform/disk_sync.h"
#include "test_check.h"
#include "x68k/fdd.h"
#include "x68k/hdd.h"

#include <atomic>
#include <stdlib.h>
#include <string.h>
#include <thread>

// DiskCache::init() と DirtyMap::init() が new[] で確保する大きさを数える (testCacheHeapEstimate)。-1 なら数えない
static size_t g_newBytes[8];
static int g_newCount = -1;
void* operator new[](size_t n) {
    if (g_newCount >= 0 && g_newCount < 8) g_newBytes[g_newCount++] = n;
    void* p = malloc(n ? n : 1);
    if (!p) abort();
    return p;
}
void operator delete[](void* p) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

namespace {

using platform::DiskChannel;
using platform::DiskEvent;
using platform::DiskSync;
using platform::FsResult;
using x68k::DirtyMap;

const uint32_t CHUNK = DirtyMap::CHUNK;

// ファイルは名前で 4 つまで。ハードディスクは File を直接持つ
struct FakeFs {
    struct File {
        bool exists = false;
        char name[16] = {};
        uint8_t* data = nullptr;
        uint32_t size = 0;
        unsigned syncs = 0;
        bool err = false; // FatFs の FIL と同じく、読み書きのエラーは残り、開き直すまで読み書きはすべて失敗する
        int failWrites = 0; // このファイルだけ、この回数だけ writeAt を失敗させる
    };
    File files[8];
    int failWrites = 0;  // この回数だけ writeAt を失敗させる
    int failSyncs = 0;   // sync
    int failRemoves = 0; // remove (フロッピーディスクの置き換え)
    int failReads = 0;   // readAt
    int failRecovers = 0; // recover (開き直し)
    unsigned reads = 0, recovers = 0;
    unsigned writes = 0, written = 0, events[8] = {};
    const char* failWriteName = nullptr; // この名前のファイルだけ、failNamedWrites 回だけ write (全体の書き込み) を失敗させる
    int failNamedWrites = 0;
    // report() の記録 (ディスクごとの回数と、起きた順の一覧)。now は試験が進める時刻
    struct Event {
        int disk;
        DiskEvent e;
        uint32_t value;
        uint64_t at;
    };
    unsigned diskEvents[10][8] = {};
    Event log[256];
    int logLen = 0;
    uint64_t now = 0;
    // ディスク disk の事象 e が起きた時刻を、起きた順に out に入れる (max 個まで)。個数を返す
    int times(int disk, DiskEvent e, uint64_t* out, int max) const {
        int n = 0;
        for (int i = 0; i < logLen && n < max; i++)
            if (log[i].disk == disk && log[i].e == e) out[n++] = log[i].at;
        return n;
    }

    ~FakeFs() {
        for (File& f : files) free(f.data);
    }
    File* make(const char* name, uint32_t size) {
        for (File& f : files)
            if (!f.name[0]) {
                strcpy(f.name, name);
                f.exists = true;
                f.data = (uint8_t*)calloc(size ? size : 1, 1);
                f.size = size;
                return &f;
            }
        return nullptr;
    }
    File* find(const char* name) {
        for (File& f : files)
            if (f.name[0] && !strcmp(f.name, name)) return &f;
        return nullptr;
    }
    bool readAt(File* f, uint64_t off, void* buf, uint32_t len) {
        if (f->err) return false;
        if (failReads > 0) {
            failReads--;
            f->err = true;
            return false;
        }
        reads++;
        if (off + len > f->size) return false;
        memcpy(buf, f->data + off, len);
        return true;
    }
    bool writeAt(File* f, uint64_t off, const void* p, uint32_t len) {
        if (f->err) return false;
        if (failWrites > 0 || f->failWrites > 0) {
            if (failWrites > 0) failWrites--;
            else f->failWrites--;
            f->err = true;
            return false;
        }
        if (off + len > f->size) return false; // 大きさは変えない
        memcpy(f->data + off, p, len);
        writes++;
        written += len;
        return true;
    }
    bool sync(File* f) {
        if (failSyncs > 0) {
            failSyncs--;
            return false;
        }
        f->syncs++;
        return true;
    }
    void recover(File* f) {
        recovers++;
        if (failRecovers > 0) { // 開き直せなかった: エラーは残る
            failRecovers--;
            return;
        }
        f->err = false;
    }
    // platform/file_replace.h
    FsResult exists(const char* n) {
        File* f = find(n);
        return f && f->exists ? FsResult::OK : FsResult::NO_FILE;
    }
    FsResult write(const char* n, const void* d, unsigned size) {
        if (failWriteName && failNamedWrites > 0 && !strcmp(n, failWriteName)) {
            failNamedWrites--;
            return FsResult::ERROR;
        }
        File* f = find(n);
        if (!f) f = make(n, size);
        free(f->data);
        f->data = (uint8_t*)malloc(size ? size : 1);
        memcpy(f->data, d, size);
        f->size = size;
        f->exists = true;
        return FsResult::OK;
    }
    FsResult remove(const char* n) {
        if (failRemoves > 0) {
            failRemoves--;
            return FsResult::ERROR;
        }
        File* f = find(n);
        if (!f || !f->exists) return FsResult::NO_FILE;
        f->exists = false;
        return FsResult::OK;
    }
    FsResult rename(const char* from, const char* to) {
        File* a = find(from);
        File* b = find(to);
        if (!a || !a->exists) return FsResult::NO_FILE;
        if (b && b->exists) return FsResult::ERROR; // FatFs の f_rename は上書きしない
        if (!b) b = make(to, 0);
        free(b->data);
        b->data = a->data;
        b->size = a->size;
        b->exists = true;
        a->data = nullptr;
        a->size = 0;
        a->exists = false;
        return FsResult::OK;
    }
    void report(int disk, DiskEvent e, uint32_t value) {
        events[(int)e]++;
        if (disk >= 0 && disk < 10) diskEvents[disk][(int)e]++;
        if (logLen < 256) log[logLen++] = {disk, e, value, now};
    }
};

void testDirtyMap() {
    printf("dirty map\n");
    DirtyMap m;
    CHECK_EQ(m.init(5 * CHUNK + 100), true);
    CHECK_EQ(m.chunks(), 6);
    CHECK_EQ(m.chunkBytes(5), 100);
    CHECK_EQ(m.chunkBytes(6), 0);
    CHECK_EQ(m.any(), false);
    m.mark(CHUNK * 2 + 7);
    m.mark(CHUNK * 2 + 8); // 同じチャンク
    CHECK_EQ(m.count(), 1);
    CHECK_EQ(m.writes(), 2);
    m.markRange(CHUNK - 1, 2); // チャンク 0 と 1 にまたがる
    CHECK_EQ(m.count(), 3);
    m.mark(6 * CHUNK); // 範囲の外は数えない
    CHECK_EQ(m.count(), 3);
    CHECK_EQ(m.next(0), 0);
    CHECK_EQ(m.next(1), 1);
    CHECK_EQ(m.next(3), (uint64_t)-1);
    m.clear(1);
    CHECK_EQ(m.next(1), 2);
    CHECK_EQ(m.count(), 2);
    m.clearAll();
    CHECK_EQ(m.any(), false);
    // 32 チャンクを超える大きさ (語の境目)
    DirtyMap big;
    big.init(100 * (uint64_t)CHUNK);
    big.mark(70 * (uint64_t)CHUNK);
    CHECK_EQ(big.next(5), 70);
    big.markRange(0, 100 * (uint64_t)CHUNK);
    CHECK_EQ(big.count(), 100);
}

// D88 は、セクタのヘッダ (データの前の 16 バイト) にも dirty フラグを立てる
void testFloppyMarks() {
    printf("floppy marks (D88 header)\n");
    // 1 トラックに 1 セクタ (1024 バイト) だけの D88
    static uint8_t d88[0x2b0 + 16 + 1024];
    memset(d88, 0, sizeof d88);
    const uint32_t total = sizeof d88;
    d88[0x1b] = 0x20; // 2HD
    for (int i = 0; i < 4; i++) d88[0x1c + i] = (uint8_t)(total >> (8 * i));
    d88[0x20] = 0xb0;
    d88[0x21] = 0x02; // トラック 0 は 0x2b0 から
    uint8_t* h = d88 + 0x2b0;
    h[0] = 0; h[1] = 0; h[2] = 1; h[3] = 3; // C H R N
    h[4] = 1;                               // セクタ数
    h[0x0e] = 0x00;
    h[0x0f] = 0x04; // 1024 バイト
    x68k::FloppyImage img;
    CHECK_EQ(img.load(d88, sizeof d88, false, "a.d88"), true);
    const x68k::FloppyImage::Sector* s = img.find(0, 0, 0, 1, 3);
    CHECK_EQ(s != nullptr, true);
    if (!s) return;
    img.setMark(s, true);
    CHECK_EQ(img.dirty(), true);
    CHECK_EQ(img.dirtyMap().count(), 1);
    CHECK_EQ(d88[0x2b0 + 7], 0x10);
}

struct Rig {
    FakeFs fs;
    DiskChannel* ch = new DiskChannel;
    DiskSync<FakeFs> sync{fs, *ch};
    ~Rig() { delete ch; }
    // コア 1 のフレームの切れ目とコア 0 の主ループを 1 回ずつ
    void step(uint64_t now) {
        fs.now = now;
        ch->serviceCore1();
        sync.service(now);
    }
};

void fillPattern(uint8_t* p, uint32_t n, uint32_t seed) {
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)((i * 31 + seed * 7) ^ (i >> 8));
}

// 書き込みが止んでから IDLE_MS で書く。書いたチャンクだけを書く
void testIdleFlush() {
    printf("hard disk: flush after idle\n");
    Rig r;
    const uint32_t size = 64 * CHUNK;
    FakeFs::File* f = r.fs.make("hd0", size);
    static uint8_t data[64 * CHUNK];
    memset(data, 0, sizeof data);
    x68k::HardDiskImage img;
    img.load(data, size, 512, false);
    r.sync.addHard(2, &img, f);
    uint64_t now = 1000;
    r.step(now);
    // 2 か所に書く (DiskCursor でコントローラと同じように)
    x68k::DiskCursor cur;
    cur.start(&img, 5 * CHUNK + 512, 1024);
    for (uint32_t i = 0; i < 1024; i++) {
        CHECK_EQ(cur.ready(i), true);
        cur.at(i) = (uint8_t)(i + 1);
        img.markDirty(cur.base() + i);
    }
    memset(img.block(40 * CHUNK / 512), 0xaa, 512);
    img.markDirtyRange(40 * CHUNK, 512);
    CHECK_EQ(img.dirtyMap().count(), 2);
    CHECK_EQ(r.sync.ioPending(), false); // 書くと決めるまでは SD カードに触らない
    // 書き込みのすぐあとは書かない
    for (int i = 0; i < 10; i++) r.step(now += 100);
    CHECK_EQ(r.fs.writes, 0);
    // io = false (コア 0 が写す役を持ったまま) の間は、書くと決めても SD カードには書かない
    for (int i = 0; i < 20; i++) {
        r.ch->serviceCore1();
        r.sync.service(now += 100, false);
    }
    CHECK_EQ(r.fs.writes, 0);
    CHECK_EQ(r.sync.ioPending(), true); // 次の周回で写す役を任せて書く
    for (int i = 0; i < 20; i++) r.step(now += 100);
    CHECK_EQ(r.fs.writes, 2);
    CHECK_EQ(r.fs.written, 2 * CHUNK);
    CHECK_EQ(memcmp(f->data, data, size), 0);
    CHECK_EQ(img.dirty(), false);
    CHECK_EQ(r.sync.busy(), false);
    CHECK_EQ(r.sync.ioPending(), false);
    CHECK_EQ(r.fs.events[(int)DiskEvent::WRITTEN], 1);
    CHECK_EQ(f->syncs >= 1, true);
}

// 書き続けていても MAX_AGE_MS で書く
void testMaxAge() {
    printf("hard disk: flush while writing continuously\n");
    Rig r;
    const uint32_t size = 16 * CHUNK;
    FakeFs::File* f = r.fs.make("hd0", size);
    static uint8_t data[16 * CHUNK];
    memset(data, 0, sizeof data);
    x68k::HardDiskImage img;
    img.load(data, size, 512, false);
    r.sync.addHard(3, &img, f);
    uint64_t now = 1;
    bool flushed = false;
    for (int i = 0; i < 400; i++) { // 100ms ごとに 40 秒書き続ける
        data[(i % 16) * CHUNK] = (uint8_t)i;
        img.markDirty((i % 16) * CHUNK);
        r.step(now += 100);
        if (r.fs.writes && !flushed) {
            flushed = true;
            CHECK_EQ(now >= DiskSync<FakeFs>::MAX_AGE_MS, true);
            CHECK_EQ(now < DiskSync<FakeFs>::MAX_AGE_MS + 1000, true);
        }
    }
    CHECK_EQ(flushed, true);
}

// 書けなかったチャンクは、 dirty フラグが下りていても受け渡しのバッファに残り、間を空けて書き直す
void testWriteFailure() {
    printf("hard disk: write failures are retried, nothing lost\n");
    Rig r;
    const uint32_t size = 32 * CHUNK;
    FakeFs::File* f = r.fs.make("hd0", size);
    static uint8_t data[32 * CHUNK];
    fillPattern(data, size, 1);
    memcpy(f->data, data, size);
    x68k::HardDiskImage img;
    img.load(data, size, 512, false);
    r.sync.addHard(2, &img, f);
    for (int c = 0; c < 10; c++) { // 10 チャンク (受け渡しのバッファ 3 回分)
        fillPattern(data + c * 3 * CHUNK % size, 600, 100 + c);
        img.markDirtyRange(c * 3 * CHUNK % size, 600);
    }
    r.fs.failWrites = 3;
    r.fs.failSyncs = 1;
    uint64_t now = 10;
    for (int i = 0; i < 2000 && (img.dirty() || r.sync.busy()); i++) r.step(now += 50);
    CHECK_EQ(memcmp(f->data, data, size), 0);
    CHECK_EQ(r.fs.events[(int)DiskEvent::WRITE_FAILED], 4);
    CHECK_EQ(r.fs.events[(int)DiskEvent::RECOVERED], 1);
    CHECK_EQ(r.fs.recovers, 4); // 失敗のたびにファイルを開き直した (開き直さなければエラーが残って書けない)
    // 開き直しにも失敗した: 次に書くときも失敗し、また開き直す。開き直せたら書ける
    data[3] ^= 0xff;
    img.markDirty(3);
    r.fs.failWrites = 1;
    r.fs.failRecovers = 2;
    for (int i = 0; i < 4000 && (img.dirty() || r.sync.busy()); i++) r.step(now += 50);
    CHECK_EQ(memcmp(f->data, data, size), 0);
    CHECK_EQ(r.fs.recovers, 4u + 3u);
    CHECK_EQ(r.sync.failures(), 0);
    CHECK_EQ(r.sync.failures(), 0);
    // 止めている間の書き戻し: 書けなければ false を返し、次に書ける
    data[7] ^= 0xff;
    img.markDirty(7);
    r.fs.failWrites = 1;
    CHECK_EQ(r.sync.flushPaused(), false);
    CHECK_EQ(r.sync.flushPaused(), true);
    CHECK_EQ(memcmp(f->data, data, size), 0);
    // 止めている間の主ループ (設定メニューを開いたまま): 待たずに書き、書けなければ間を空けて書き直す
    data[9] ^= 0xff;
    img.markDirty(9);
    r.fs.failWrites = 2;
    const unsigned before = r.fs.writes;
    uint64_t t = 100000;
    r.sync.servicePaused(t); // 1 回目: 失敗 (1 秒あとに書き直す。データは受け渡しのバッファに残る)
    CHECK_EQ(r.sync.busy(), true);
    r.sync.servicePaused(t += 500); // まだ書き直さない
    CHECK_EQ(r.fs.events[(int)DiskEvent::WRITE_FAILED], 9);
    r.sync.servicePaused(t += 600); // 2 回目: 失敗 (2 秒あと)
    r.sync.servicePaused(t += 1000);
    CHECK_EQ(r.fs.events[(int)DiskEvent::WRITE_FAILED], 10);
    r.sync.servicePaused(t += 1100); // 3 回目: 書ける
    CHECK_EQ(img.dirty() || r.sync.busy(), false);
    CHECK_EQ(memcmp(f->data, data, size), 0);
    r.sync.servicePaused(t += 5000); // 書くものがなければ何もしない
    CHECK_EQ(r.fs.writes, before + 1);
}

// フロッピーディスク: 写しに当て、 dirty フラグがすべて下りたら .new に書いて置き換える
void testFloppy() {
    printf("floppy: shadow and replace\n");
    Rig r;
    static uint8_t data[1261568];
    fillPattern(data, sizeof data, 3);
    FakeFs::File* f = r.fs.make("fd0.xdf", sizeof data);
    memcpy(f->data, data, sizeof data);
    static uint8_t shadow[sizeof data];
    memcpy(shadow, data, sizeof data);
    x68k::FloppyImage img;
    CHECK_EQ(img.load(data, sizeof data, false, "fd0.xdf"), true);
    r.sync.addFloppy(0, &img, "fd0.xdf", "fd0.xdf.new", shadow, sizeof data);
    // 2 つのセクタに書く
    const x68k::FloppyImage::Sector* s1 = img.find(0, 0, 0, 1, 3);
    const x68k::FloppyImage::Sector* s2 = img.find(100, 50, 0, 8, 3);
    memset(img.data(s1), 0x11, 1024);
    img.setMark(s1, false);
    memset(img.data(s2), 0x22, 1024);
    img.setMark(s2, false);
    r.fs.failRemoves = 1; // 1 回目の置き換えは失敗させる
    uint64_t now = 5;
    for (int i = 0; i < 400 && (img.dirty() || r.sync.busy() || r.fs.events[(int)DiskEvent::REPLACED] == 0); i++)
        r.step(now += 100);
    FakeFs::File* g = r.fs.find("fd0.xdf");
    CHECK_EQ(g && g->exists && g->size == sizeof data, true);
    if (g && g->data) CHECK_EQ(memcmp(g->data, data, sizeof data), 0);
    CHECK_EQ(r.fs.events[(int)DiskEvent::REPLACE_FAILED], 1);
    CHECK_EQ(r.fs.events[(int)DiskEvent::REPLACED], 1);
    // 取り出す前の書き戻し (止めている間)
    memset(img.data(s1), 0x33, 1024);
    img.setMark(s1, false);
    CHECK_EQ(r.sync.flushPaused(0), true);
    g = r.fs.find("fd0.xdf");
    if (g && g->data) CHECK_EQ(memcmp(g->data, data, sizeof data), 0);
    CHECK_EQ(r.fs.events[(int)DiskEvent::REPLACED], 2);
}

// io = false (コア 0 が写す役を持ったまま) の service() は、書き直すまでの間 (retry) の期限を過ぎても SD カードに
// 触らず、状態 (写しを書いていない、受け渡しのバッファに残っている) をそのまま残す。そのあと io = true で書き終える
void testServiceWithoutIo() {
    printf("service without io keeps pending work past the retry time\n");
    {
        // 写しを書いていないフロッピーディスク (1 回目の置き換えに失敗した)
        Rig r;
        static uint8_t data[1261568];
        fillPattern(data, sizeof data, 9);
        FakeFs::File* f = r.fs.make("fd1.xdf", sizeof data);
        memcpy(f->data, data, sizeof data);
        static uint8_t shadow[sizeof data];
        memcpy(shadow, data, sizeof data);
        x68k::FloppyImage img;
        CHECK_EQ(img.load(data, sizeof data, false, "fd1.xdf"), true);
        r.sync.addFloppy(1, &img, "fd1.xdf", "fd1.xdf.new", shadow, sizeof data);
        const x68k::FloppyImage::Sector* sec = img.find(10, 5, 0, 3, 3);
        memset(img.data(sec), 0x44, 1024);
        img.setMark(sec, false);
        r.fs.failRemoves = 1;
        uint64_t now = 5;
        for (int i = 0; i < 400 && r.fs.events[(int)DiskEvent::REPLACE_FAILED] == 0; i++) r.step(now += 100);
        CHECK_EQ(r.fs.events[(int)DiskEvent::REPLACE_FAILED], 1);
        CHECK_EQ(img.dirty(), false);
        CHECK_EQ(r.sync.ioPending(), true);
        // 書き直すまでの間 (1 秒) を大きく過ぎても、io = false なら置き換えない
        for (int i = 0; i < 50; i++) {
            r.ch->serviceCore1();
            r.sync.service(now += 100, false);
        }
        CHECK_EQ(r.fs.events[(int)DiskEvent::REPLACE_FAILED], 1);
        CHECK_EQ(r.fs.events[(int)DiskEvent::REPLACED], 0);
        CHECK_EQ(r.sync.ioPending(), true);
        FakeFs::File* g = r.fs.find("fd1.xdf");
        CHECK_EQ(g && g->exists && g->data && memcmp(g->data, data, sizeof data) != 0, true); // まだ前の内容
        // io = true で置き換える
        for (int i = 0; i < 20 && r.sync.ioPending(); i++) r.step(now += 100);
        CHECK_EQ(r.fs.events[(int)DiskEvent::REPLACED], 1);
        CHECK_EQ(r.sync.ioPending(), false);
        g = r.fs.find("fd1.xdf");
        CHECK_EQ(g && g->exists && g->size == sizeof data, true);
        if (g && g->data) CHECK_EQ(memcmp(g->data, data, sizeof data), 0);
    }
    {
        // 書けなかったハードディスクのチャンク (受け渡しのバッファに残っている)
        Rig r;
        const uint32_t size = 16 * CHUNK;
        FakeFs::File* f = r.fs.make("hd0", size);
        static uint8_t data[16 * CHUNK];
        fillPattern(data, size, 11);
        memcpy(f->data, data, size);
        x68k::HardDiskImage img;
        img.load(data, size, 512, false);
        r.sync.addHard(2, &img, f);
        fillPattern(data + 4 * CHUNK, 512, 12);
        img.markDirtyRange(4 * CHUNK, 512);
        r.fs.failWrites = 1;
        uint64_t now = 10;
        for (int i = 0; i < 400 && r.fs.events[(int)DiskEvent::WRITE_FAILED] == 0; i++) r.step(now += 100);
        CHECK_EQ(r.fs.events[(int)DiskEvent::WRITE_FAILED], 1);
        const unsigned writes = r.fs.writes;
        for (int i = 0; i < 50; i++) {
            r.ch->serviceCore1();
            r.sync.service(now += 100, false);
        }
        CHECK_EQ(r.fs.writes, writes);
        CHECK_EQ(r.fs.events[(int)DiskEvent::WRITE_FAILED], 1);
        CHECK_EQ(r.sync.busy(), true);
        CHECK_EQ(r.sync.ioPending(), true);
        for (int i = 0; i < 20 && r.sync.ioPending(); i++) r.step(now += 100);
        CHECK_EQ(r.sync.ioPending(), false);
        CHECK_EQ(r.fs.events[(int)DiskEvent::RECOVERED], 1);
        CHECK_EQ(memcmp(f->data, data, size), 0);
    }
}

// 書き戻せないまま FD を外し (書き戻さずに取り出した)、同じドライブに別の FD を入れても、受け渡しのバッファに
// 残っていた前の FD のチャンクを新しい FD に書かない
void testRemoveDropsRecords() {
    printf("floppy removed with unwritten chunks: not written to the next disk\n");
    Rig r;
    const uint32_t fsize = 1261568;
    static uint8_t a[1261568], b[1261568], shadowA[1261568], shadowB[1261568];
    fillPattern(a, fsize, 21);
    fillPattern(b, fsize, 22);
    FakeFs::File* fa = r.fs.make("a.xdf", fsize);
    memcpy(fa->data, a, fsize);
    FakeFs::File* fb = r.fs.make("b.xdf", fsize);
    memcpy(fb->data, b, fsize);
    static uint8_t bOrig[1261568];
    memcpy(bOrig, b, fsize);
    memcpy(shadowA, a, fsize);
    x68k::FloppyImage imgA;
    CHECK_EQ(imgA.load(a, fsize, false, "a.xdf"), true);
    r.sync.addFloppy(0, &imgA, "a.xdf", "a.xdf.new", shadowA, fsize);
    const uint32_t hsize = 8 * CHUNK;
    FakeFs::File* fh = r.fs.make("hd0", hsize);
    static uint8_t h[8 * CHUNK];
    memset(h, 0, sizeof h);
    x68k::HardDiskImage himg;
    himg.load(h, hsize, 512, false);
    r.sync.addHard(2, &himg, fh);
    // 両方に書き、HD の書き込みを失敗させ続ける (FD0 と HD のチャンクが同じバッファに入ったまま)
    const x68k::FloppyImage::Sector* s1 = imgA.find(0, 0, 0, 1, 3);
    memset(imgA.data(s1), 0x5a, 1024);
    imgA.setMark(s1, false);
    h[10] = 1;
    himg.markDirty(10);
    r.fs.failWrites = 1000000;
    uint64_t now = 100;
    for (int i = 0; i < 100; i++) r.step(now += 100);
    CHECK_EQ(r.sync.busy(), true);
    CHECK_EQ(r.sync.flushPaused(0), false);
    // 書き戻さずに取り出して、B を入れる
    r.sync.remove(0);
    memcpy(shadowB, b, fsize);
    x68k::FloppyImage imgB;
    CHECK_EQ(imgB.load(b, fsize, false, "b.xdf"), true);
    r.sync.addFloppy(0, &imgB, "b.xdf", "b.xdf.new", shadowB, fsize);
    // HD が書けるようになった
    r.fs.failWrites = 0;
    CHECK_EQ(r.sync.flushPaused(), true);
    FakeFs::File* g = r.fs.find("b.xdf");
    if (g && g->data) CHECK_EQ(memcmp(g->data, bOrig, fsize), 0);
    CHECK_EQ(memcmp(shadowB, bOrig, fsize), 0);
    CHECK_EQ(fh->data[10], 1);
}

// コア 1 とコア 0 を別のスレッドで動かす。コア 1 は書いては (コントローラと同じく 1 バイトごとに dirty フラグを立てて)
// フレームの切れ目で serviceCore1() を呼び、コア 0 は主ループを回す。最後に止めて (コア 1 のスレッドを終えて)
// flushPaused() を呼び、ファイルがイメージと同じになることを確かめる
void testThreads() {
    printf("two threads (core 1 writes, core 0 flushes)\n");
    Rig r;
    const uint32_t size = 128 * CHUNK;
    FakeFs::File* f = r.fs.make("hd0", size);
    static uint8_t data[128 * CHUNK];
    fillPattern(data, size, 9);
    memcpy(f->data, data, size);
    static x68k::HardDiskImage img;
    img.load(data, size, 512, false);
    r.sync.addHard(4, &img, f);
    std::atomic<bool> done{false};
    std::atomic<uint64_t> clock{1};
    std::thread core1([&] {
        uint32_t seed = 12345;
        x68k::DiskCursor cur;
        for (int frame = 0; frame < 3000; frame++) {
            // 1 フレームに 0〜3 回、ランダムな位置に 512〜4096 バイト書く (ときどき書かないフレームを続ける)
            seed = seed * 1103515245 + 12345;
            const int n = (frame / 200) % 3 == 2 ? 0 : (int)(seed >> 28) % 4;
            for (int k = 0; k < n; k++) {
                seed = seed * 1103515245 + 12345;
                const uint32_t lba = (seed >> 8) % (size / 512 - 8);
                const uint32_t len = 512 * (1 + (seed & 7));
                cur.start(&img, (uint64_t)lba * 512, len);
                for (uint32_t i = 0; i < len; i++) {
                    if (!cur.ready(i)) break;
                    cur.at(i) = (uint8_t)(seed + i);
                    img.markDirty(cur.base() + i);
                }
            }
            r.ch->serviceCore1();
            clock.fetch_add(17, std::memory_order_relaxed); // 1 フレーム 17ms とする
        }
        done.store(true, std::memory_order_release);
    });
    std::thread core0([&] {
        while (!done.load(std::memory_order_acquire)) r.sync.service(clock.load(std::memory_order_relaxed));
    });
    core1.join();
    core0.join();
    // 止めている (コア 1 のスレッドは終わった)
    CHECK_EQ(r.sync.flushPaused(), true);
    CHECK_EQ(memcmp(f->data, data, size), 0);
    CHECK_EQ(img.dirty(), false);
    printf("  %u writes, %u bytes\n", r.fs.writes, r.fs.written);
}

// 大きなイメージ: 読み込みを待つ間は ready() が false。コア 0 が読めば読める
void testCachedRead() {
    printf("cached image: read waits for core 0\n");
    Rig r;
    const uint32_t size = 40 * CHUNK + 1024; // 最後のチャンクは短い
    FakeFs::File* f = r.fs.make("big.hds", size);
    fillPattern(f->data, size, 5);
    x68k::DiskCache cache;
    CHECK_EQ(cache.init(size, 8), true);
    x68k::HardDiskImage img;
    CHECK_EQ(img.loadCached(&cache, size, 512, true), true);
    CHECK_EQ(img.block(0) == nullptr, true);
    r.sync.addHard(5, nullptr, f, &cache);
    // 3 チャンクにまたがる読み込み
    x68k::DiskCursor cur;
    const uint32_t base = 3 * CHUNK - 512, len = 2 * CHUNK + 1024;
    CHECK_EQ(r.sync.ioPending(), false);
    cur.start(&img, base, len);
    CHECK_EQ(cur.ready(0), false); // まだ読んでいない
    CHECK_EQ(r.sync.ioPending(), true); // 読み込みを頼まれた
    uint32_t pos = 0;
    int rounds = 0;
    bool ok = true;
    while (pos < len && rounds < 100) {
        if (!cur.ready(pos)) {
            r.sync.serviceLoads(2); // コア 0 が 2 チャンクずつ読む
            rounds++;
            continue;
        }
        ok = ok && cur.at(pos) == f->data[base + pos];
        pos++;
    }
    CHECK_EQ(pos, len);
    CHECK_EQ(ok, true);
    // 最後の短いチャンク
    cur.start(&img, 40 * CHUNK, 1024);
    while (!cur.ready(0)) r.sync.serviceLoads(4);
    CHECK_EQ(cur.avail(0), 1024);
    CHECK_EQ(cur.at(1023), f->data[40 * CHUNK + 1023]);
    // 枠 (8 個) より多くのチャンクを順に読む (古い枠を空けて使い回す)
    for (uint32_t c = 0; c < 40; c++) {
        cur.start(&img, (uint64_t)c * CHUNK + 100, 200);
        int guard = 0;
        while (!cur.ready(0) && guard++ < 10) r.sync.serviceLoads(1);
        CHECK_EQ(cur.ready(0), true);
        if (cur.ready(0)) CHECK_EQ(cur.at(199), f->data[c * CHUNK + 299]);
    }
    // 先読み (start() が頼む) で読めなかったチャンクは、エラーにせずに読み直す (先に頼んであった読み込みは済ませておく)
    while (r.sync.serviceLoads(8) != 0) {
    }
    cur.start(&img, 10 * CHUNK + 5, 10);
    r.fs.failReads = 1;
    int guard = 0;
    while (!cur.ready(0) && !cur.failed() && guard++ < 100) r.sync.serviceLoads(1);
    CHECK_EQ(cur.failed(), false);
    CHECK_EQ(cur.ready(0), true);
    if (cur.ready(0)) CHECK_EQ(cur.at(0), f->data[10 * CHUNK + 5]);
    CHECK_EQ(r.fs.events[(int)DiskEvent::READ_FAILED], 1);
    CHECK_EQ(r.fs.events[(int)DiskEvent::READ_RECOVERED], 1);
    // 読み直しも読めなければ failed になる。次のコマンドではまた読み直す
    while (r.sync.serviceLoads(8) != 0) {
    }
    CHECK_EQ(r.sync.ioPending(), false);
    cur.start(&img, 20 * CHUNK + 5, 10);
    r.fs.failReads = 100;
    guard = 0;
    while (!cur.ready(0) && !cur.failed() && guard++ < 100) r.sync.serviceLoads(1);
    CHECK_EQ(cur.failed(), true);
    // 読んだのは先読みの 3 チャンクと読み直しの 1 回だけ。ログは続けて失敗した 1 回目だけ出す
    CHECK_EQ(100 - r.fs.failReads, 4);
    CHECK_EQ(r.fs.events[(int)DiskEvent::READ_FAILED], 2);
    r.fs.failReads = 0;
    cur.start(&img, 20 * CHUNK + 5, 10);
    guard = 0;
    while (!cur.ready(0) && !cur.failed() && guard++ < 100) r.sync.serviceLoads(1);
    CHECK_EQ(cur.ready(0), true); // 読めなかったあとにファイルを開き直したので、読み直せる
    if (cur.ready(0)) CHECK_EQ(cur.at(0), f->data[20 * CHUNK + 5]);
    CHECK_EQ(r.fs.events[(int)DiskEvent::READ_RECOVERED], 2);
}

// 先読みとコマンドの読み込みの失敗 (x68k/disk_cache.h): 先読みの失敗は次に使うときに読み直す。読み直しが失敗したら failed。
// 続けて FAIL_STREAK 回読めなければ、先読みも読み直しもしない (壊れた SD カードを読み続けない)。読めたら戻す
void testCacheReadFailures() {
    printf("cache: prefetch failures are retried once, with a limit\n");
    const uint32_t chunks = 64;
    x68k::DiskCache cache;
    CHECK_EQ(cache.init((uint64_t)chunks * CHUNK, 16), true);
    int okReads = 0, badReads = 0;
    bool fail = false;
    auto serve = [&]() {
        unsigned slot;
        uint32_t c, len;
        uint8_t* buf;
        unsigned n = 0;
        while (cache.nextLoad(slot, c, buf, len)) {
            if (!fail) memset(buf, (int)c, len);
            cache.loaded(slot, !fail);
            (fail ? badReads : okReads)++;
            n++;
        }
        return n;
    };
    bool failed = false;
    // 先読みが失敗: lookup() は failed を立てず、読み直しを頼む
    cache.prefetch(5);
    fail = true;
    CHECK_EQ(serve(), 1u);
    fail = false;
    CHECK_EQ(cache.lookup(5, true, failed) == nullptr, true);
    CHECK_EQ(failed, false);
    CHECK_EQ(serve(), 1u);
    uint8_t* p = cache.lookup(5, true, failed);
    CHECK_EQ(p != nullptr && p[0] == 5, true);
    // load なしで見たときは読み直しを頼まない (failed も立てない。次に load で見れば頼む)
    cache.prefetch(6);
    fail = true;
    serve();
    CHECK_EQ(cache.lookup(6, false, failed) == nullptr, true);
    CHECK_EQ(failed, false);
    fail = false;
    CHECK_EQ(serve(), 0u);
    CHECK_EQ(cache.lookup(6, true, failed) == nullptr, true);
    CHECK_EQ(serve(), 1u);
    p = cache.lookup(6, true, failed);
    CHECK_EQ(p != nullptr && p[0] == 6, true);
    // 先読みも読み直しも失敗: failed
    cache.prefetch(7);
    fail = true;
    serve();
    CHECK_EQ(cache.lookup(7, true, failed) == nullptr, true);
    CHECK_EQ(failed, false);
    CHECK_EQ(serve(), 1u);
    CHECK_EQ(cache.lookup(7, true, failed) == nullptr, true);
    CHECK_EQ(failed, true);
    // コマンドの読み込み (lookup の load) の失敗はすぐ failed
    fail = false;
    CHECK_EQ(cache.lookup(8, true, failed) == nullptr, true);
    fail = true;
    serve();
    CHECK_EQ(cache.lookup(8, true, failed) == nullptr, true);
    CHECK_EQ(failed, true);
    // 続けて失敗している: 先読みは頼まない
    cache.prefetch(30); // 失敗が続く前に頼んだ先読み
    fail = true;
    CHECK_EQ(serve(), 1u);
    for (uint32_t c = 10; c < 14; c++) {
        cache.lookup(c, true, failed);
        serve();
        cache.lookup(c, true, failed);
    }
    cache.prefetch(20);
    CHECK_EQ(serve(), 0u);
    // 失敗が続いたあとでも、先読みで読めなかったチャンクは 1 回はコマンドの読み込みとして頼む
    CHECK_EQ(cache.lookup(30, true, failed) == nullptr, true);
    CHECK_EQ(failed, false);
    CHECK_EQ(serve(), 1u);
    CHECK_EQ(cache.lookup(30, true, failed) == nullptr, true);
    CHECK_EQ(failed, true); // その読み込みも読めなければ failed
    // コマンドの読み込みは頼む。読めたら先読みも戻る
    fail = false;
    CHECK_EQ(cache.lookup(21, true, failed) == nullptr, true);
    CHECK_EQ(serve(), 1u);
    p = cache.lookup(21, true, failed);
    CHECK_EQ(p != nullptr && p[0] == 21, true);
    cache.prefetch(22);
    CHECK_EQ(serve(), 1u);
}

// 大きなイメージへの書き込み: 書いたチャンクは書き戻すまで空けない (空けて読み直すと古い中身になる)。
// 書き戻しに失敗している間も同じ
void testCachedWrite() {
    printf("cached image: written chunks stay until written back\n");
    Rig r;
    const uint32_t size = 32 * CHUNK;
    FakeFs::File* f = r.fs.make("big.hds", size);
    fillPattern(f->data, size, 6);
    static uint8_t ref[32 * CHUNK];
    memcpy(ref, f->data, size);
    x68k::DiskCache cache;
    cache.init(size, 4); // 4 枠しかない
    x68k::HardDiskImage img;
    img.loadCached(&cache, size, 512, false);
    r.sync.addHard(3, &img, f, &cache);
    uint64_t now = 1;
    x68k::DiskCursor cur;
    // 6 チャンクに書く (枠より多い)。書き戻しは最初の 3 回失敗させる
    r.fs.failWrites = 3;
    for (uint32_t c = 0; c < 6; c++) {
        const uint32_t base = c * 5 * CHUNK + 1000;
        cur.start(&img, base, 512);
        uint32_t pos = 0;
        int guard = 0;
        while (pos < 512 && guard < 100000) {
            if (!cur.ready(pos)) {
                // 待つ: フレームの切れ目とコア 0 の主ループ
                r.ch->serviceCore1();
                r.sync.serviceLoads(4);
                r.sync.service(now += 20);
                guard++;
                continue;
            }
            cur.at(pos) = (uint8_t)(c * 7 + pos);
            ref[base + pos] = (uint8_t)(c * 7 + pos);
            img.markDirty(base + pos);
            pos++;
        }
        CHECK_EQ(pos, 512);
    }
    // 書いた内容を、ほかのチャンクを読んで枠を回しながら読み直す
    for (int round = 0; round < 3; round++) {
        for (uint32_t c = 0; c < 32; c++) {
            cur.start(&img, (uint64_t)c * CHUNK, CHUNK);
            uint32_t pos = 0;
            int guard = 0;
            bool same = true;
            while (pos < CHUNK && guard < 100000) {
                if (!cur.ready(pos)) {
                    r.ch->serviceCore1();
                    r.sync.serviceLoads(4);
                    r.sync.service(now += 20);
                    guard++;
                    continue;
                }
                same = same && cur.at(pos) == ref[c * CHUNK + pos];
                pos++;
            }
            CHECK_EQ(pos, CHUNK);
            CHECK_EQ(same, true);
        }
    }
    CHECK_EQ(r.fs.events[(int)DiskEvent::WRITE_FAILED], 3);
    CHECK_EQ(r.sync.flushPaused(), true);
    CHECK_EQ(memcmp(f->data, ref, size), 0);
}

// 大きなイメージを 2 つのスレッドで: コア 1 はランダムに読み書きし (読み込みを待つ間はフレームを進める)、読んだ中身が
// 書いたとおりかを確かめる。コア 0 は読み込みと書き戻しをする
void testCachedThreads() {
    printf("cached image, two threads\n");
    Rig r;
    const uint32_t size = 256 * CHUNK;
    FakeFs::File* f = r.fs.make("big.hds", size);
    fillPattern(f->data, size, 11);
    static uint8_t ref[256 * CHUNK];
    memcpy(ref, f->data, size);
    static x68k::DiskCache cache;
    cache.init(size, 16);
    static x68k::HardDiskImage img;
    img.loadCached(&cache, size, 512, false);
    r.sync.addHard(6, &img, f, &cache);
    std::atomic<bool> done{false};
    std::atomic<uint64_t> clock{1};
    std::atomic<int> bad{0};
    std::thread core1([&] {
        uint32_t seed = 777;
        x68k::DiskCursor cur;
        for (int op = 0; op < 3000; op++) {
            seed = seed * 1103515245 + 12345;
            const bool write = (seed >> 30) == 0; // 1/4 は書き込み
            const uint32_t lba = (seed >> 6) % (size / 512 - 64);
            const uint32_t len = 512 * (1 + ((seed >> 3) & 31));
            cur.start(&img, (uint64_t)lba * 512, len);
            for (uint32_t pos = 0; pos < len;) {
                if (!cur.ready(pos)) {
                    r.ch->serviceCore1(); // 待つ間もフレームは進む
                    clock.fetch_add(17, std::memory_order_relaxed);
                    continue;
                }
                const uint32_t off = lba * 512 + pos;
                if (write) {
                    const uint8_t v = (uint8_t)(seed ^ pos);
                    cur.at(pos) = v;
                    ref[off] = v;
                    img.markDirty(off);
                } else if (cur.at(pos) != ref[off]) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                }
                pos++;
            }
            if ((op & 7) == 0) {
                r.ch->serviceCore1();
                clock.fetch_add(17, std::memory_order_relaxed);
            }
        }
        done.store(true, std::memory_order_release);
    });
    std::thread core0([&] {
        while (!done.load(std::memory_order_acquire)) {
            r.sync.serviceLoads(4);
            r.sync.service(clock.load(std::memory_order_relaxed));
        }
    });
    core1.join();
    core0.join();
    CHECK_EQ(bad.load(), 0);
    CHECK_EQ(r.sync.flushPaused(), true);
    CHECK_EQ(memcmp(f->data, ref, size), 0);
    printf("  %u reads, %u writes, cache hits %u, misses %u\n", r.fs.reads, r.fs.writes, cache.hits(), cache.misses());
}

// コントローラが使っている枠 (最後に lookup() したチャンク) は、ほかのチャンクを先に読んでも追い出さない
void testCachePinned() {
    printf("cache: the slot in use is not evicted\n");
    const uint32_t size = 32 * CHUNK;
    static uint8_t file[32 * CHUNK];
    fillPattern(file, size, 9);
    x68k::DiskCache cache;
    cache.init(size, 4);
    auto serve = [&]() {
        unsigned slot;
        uint32_t c, len;
        uint8_t* buf;
        while (cache.nextLoad(slot, c, buf, len)) {
            memcpy(buf, file + (size_t)c * CHUNK, len);
            cache.loaded(slot, true);
        }
    };
    bool failed = false;
    CHECK_EQ(cache.lookup(0, true, failed) == nullptr, true);
    serve();
    uint8_t* p = cache.lookup(0, true, failed);
    CHECK_EQ(p != nullptr, true);
    for (uint32_t c = 1; c < 20; c++) { // 枠 (4 個) より多くを先に読む
        cache.prefetch(c);
        serve();
    }
    CHECK_EQ(cache.peek(0) == p, true);
    if (p) CHECK_EQ(memcmp(p, file, CHUNK), 0);
}

// イメージ全体の数の枠を用意したキャッシュ (ベアメタル版で 64MB 以下のイメージに使う) は、一度読んだチャンクを空けない。
// 全部のチャンクを読んだあとは、もう一度読んでも SD カードを読まない (全体をメモリに読んだのと同じ)
void testWholeImageCache() {
    printf("cache: with a slot for every chunk, nothing is read twice\n");
    const uint32_t chunks = 37;
    const uint32_t size = chunks * CHUNK - 300; // 最後のチャンクは短い
    static uint8_t file[37 * CHUNK];
    fillPattern(file, size, 11);
    x68k::DiskCache cache;
    CHECK_EQ(cache.init(size, chunks), true);
    unsigned loads = 0;
    auto serve = [&]() {
        unsigned slot;
        uint32_t c, len;
        uint8_t* buf;
        while (cache.nextLoad(slot, c, buf, len)) {
            memcpy(buf, file + (size_t)c * CHUNK, len);
            cache.loaded(slot, true);
            loads++;
        }
    };
    bool failed = false;
    for (int round = 0; round < 3; round++) {
        // 順と逆順を混ぜ、先読みも使う
        for (uint32_t i = 0; i < chunks; i++) {
            const uint32_t c = round == 1 ? chunks - 1 - i : (i * 7) % chunks;
            uint8_t* p = cache.lookup(c, true, failed);
            if (p == nullptr) {
                cache.prefetch((c + 1) % chunks);
                serve();
                p = cache.lookup(c, true, failed);
            }
            CHECK_EQ(p != nullptr, true);
            if (p) CHECK_EQ(p[CHUNK / 2 - 1], file[(size_t)c * CHUNK + CHUNK / 2 - 1]);
        }
        CHECK_EQ(loads, chunks); // 2 回目からは読まない
    }
}

// すべてのチャンクの枠があるキャッシュは、フレームの切れ目の fillAhead() で残りのチャンクを順に読み、全体がメモリに入る。
// コマンドの読み込みが待っている間は足さず、枠の足りないキャッシュでは何もしない
void testFillAhead() {
    printf("cache: the whole image is read in the background\n");
    const uint32_t chunks = 100;
    const uint32_t size = chunks * CHUNK - 300;
    static uint8_t file[100 * CHUNK];
    fillPattern(file, size, 23);
    static uint8_t data[100 * CHUNK]; // 枠の中身を外から渡す (ベアメタル版の 1GB より上のヒープの代わり)
    static int released;
    released = 0;
    unsigned loads = 0;
    bool fail = false;
    {
        x68k::DiskCache cache;
        CHECK_EQ(cache.init(size, chunks, data, [](uint8_t* p) { released += p == data; }), true);
        CHECK_EQ(cache.whole(), true);
        auto serve = [&]() {
            unsigned slot, n = 0;
            uint32_t c, len;
            uint8_t* buf;
            while (cache.nextLoad(slot, c, buf, len)) {
                if (!fail) memcpy(buf, file + (size_t)c * CHUNK, len);
                cache.loaded(slot, !fail);
                loads++;
                n++;
            }
            return n;
        };
        bool failed = false;
        // 1 回に 8 チャンクまで。頼みが残っている間は足さない
        cache.fillAhead(8);
        cache.fillAhead(8);
        CHECK_EQ(serve(), 8u);
        CHECK_EQ(cache.fillDone(), false);
        // コマンドが読み込みを頼んだ (待っている) 直後の回は足さない。その次からは続ける
        CHECK_EQ(cache.lookup(50, true, failed) == nullptr, true);
        CHECK_EQ(serve(), 1u);
        cache.fillAhead(8);
        CHECK_EQ(serve(), 0u);
        CHECK_EQ(cache.lookup(50, true, failed) != nullptr, true);
        cache.fillAhead(8);
        CHECK_EQ(serve(), 8u);
        // 読めなかったチャンクは、あとでコマンドが読むときに読み直す (先読みと同じ)
        fail = true;
        cache.fillAhead(4);
        CHECK_EQ(serve(), 4u); // チャンク 16-19
        fail = false;
        for (int i = 0; i < 40 && !cache.fillDone(); i++) {
            cache.fillAhead(8);
            serve();
        }
        CHECK_EQ(cache.fillDone(), true);
        CHECK_EQ(cache.allLoaded(), false); // 頼み終えたが、読めなかったチャンクが 4 つ残っている
        CHECK_EQ(loads, chunks); // 読んであるチャンク (50) は飛ばし、同じチャンクを 2 回読まない
        cache.fillAhead(8);
        CHECK_EQ(serve(), 0u);
        // 全体がメモリにある: 読めなかった 4 つのほかは、もう頼まない
        unsigned waits = 0;
        for (uint32_t c = 0; c < chunks; c++) {
            uint8_t* p = cache.lookup(c, true, failed);
            if (p == nullptr) {
                waits++;
                CHECK_EQ(c >= 16 && c < 20, true);
                CHECK_EQ(failed, false);
                serve();
                p = cache.lookup(c, true, failed);
            }
            CHECK_EQ(p != nullptr, true);
            if (p) CHECK_EQ(memcmp(p, file + (size_t)c * CHUNK, c == chunks - 1 ? CHUNK - 300 : CHUNK), 0);
            if (p) CHECK_EQ(p >= data && p < data + sizeof data, true);
        }
        CHECK_EQ(waits, 4u);
        CHECK_EQ(loads, chunks + 4);
        CHECK_EQ(cache.allLoaded(), true); // 読み直して、すべて読めた
        // 読めなかったチャンクを読み直すときも、読めているチャンクの枠は空けない (もう一度見ても読み込みを頼まない)
        for (uint32_t c = 0; c < chunks; c++) CHECK_EQ(cache.lookup(c, true, failed) != nullptr, true);
        CHECK_EQ(serve(), 0u);
        // 別のイメージに作り直すと、最初から読み直す
        CHECK_EQ(cache.reinit(40 * CHUNK), true);
        CHECK_EQ(cache.fillDone(), false);
        CHECK_EQ(cache.allLoaded(), false);
        cache.fillAhead(8);
        CHECK_EQ(serve(), 8u);
        // 枠より大きいイメージに作り直すと、裏では読まない
        CHECK_EQ(cache.reinit(101 * CHUNK), true);
        CHECK_EQ(cache.whole(), false);
        cache.fillAhead(8);
        CHECK_EQ(serve(), 0u);
        CHECK_EQ(released, 0);
    }
    CHECK_EQ(released, 1); // 外から渡した枠の中身は、捨てるときに返す
    // 読めないことが続いている間は頼まない
    {
        x68k::DiskCache cache;
        CHECK_EQ(cache.init(size, chunks), true);
        bool failed = false;
        for (unsigned i = 0; i < x68k::DiskCache::FAIL_STREAK; i++) {
            CHECK_EQ(cache.lookup(i, true, failed) == nullptr, true);
            unsigned slot;
            uint32_t c, len;
            uint8_t* buf;
            while (cache.nextLoad(slot, c, buf, len)) cache.loaded(slot, false);
            CHECK_EQ(cache.lookup(i, true, failed) == nullptr, true);
            CHECK_EQ(failed, true);
        }
        cache.fillAhead(8);
        cache.fillAhead(8);
        CHECK_EQ(cache.loadQueued(), false);
    }
    // 裏の読み込みが FAIL_STREAK 回続けて読めなければ、やめる (読めるようになっても、作り直すまで戻さない)
    {
        x68k::DiskCache cache;
        CHECK_EQ(cache.init(size, chunks), true);
        auto serveAll = [&](bool ok) {
            unsigned slot, n = 0;
            uint32_t c, len;
            uint8_t* buf;
            while (cache.nextLoad(slot, c, buf, len)) {
                cache.loaded(slot, ok);
                n++;
            }
            return n;
        };
        cache.fillAhead(2);
        cache.fillAhead(2); // コア 0 がまだ読んでいない間は足さない
        CHECK_EQ(serveAll(false), 2u);
        cache.fillAhead(2);
        CHECK_EQ(serveAll(true), 2u); // 読めたら数え直す
        for (unsigned i = 0; i < x68k::DiskCache::FAIL_STREAK; i++) {
            cache.fillAhead(2);
            CHECK_EQ(serveAll(false), 2u);
        }
        cache.fillAhead(2);
        CHECK_EQ(serveAll(true), 0u);
        CHECK_EQ(cache.fillDone(), false);
        CHECK_EQ(cache.reinit(size), true);
        cache.fillAhead(2);
        CHECK_EQ(serveAll(true), 2u);
    }
    // DiskSync につないだキャッシュは、コア 1 のフレームの切れ目 (serviceCore1()) で進む。外すと進まない
    {
        FakeFs fs;
        platform::DiskChannel ch;
        platform::DiskSync<FakeFs> sync(fs, ch);
        FakeFs::File f;
        f.data = file;
        f.size = sizeof file; // イメージをつないでいないキャッシュは、最後のチャンクも 16KB で読む
        x68k::DiskCache cache;
        CHECK_EQ(cache.init(size, chunks), true);
        sync.addHard(3, nullptr, &f, &cache);
        unsigned total = 0;
        for (int frame = 0; frame < 30; frame++) {
            ch.serviceCore1();
            CHECK_EQ(sync.ioPending(), !cache.fillDone() || cache.loadQueued());
            unsigned n;
            while ((n = sync.serviceLoads(4)) != 0) total += n;
        }
        CHECK_EQ(total, chunks);
        CHECK_EQ(cache.fillDone(), true);
        CHECK_EQ(cache.allLoaded(), true);
        bool failed = false;
        const uint8_t* p = cache.lookup(77, true, failed);
        CHECK_EQ(p != nullptr && memcmp(p, file + 77 * (size_t)CHUNK, CHUNK) == 0, true);
        x68k::DiskCache other;
        CHECK_EQ(other.init(size, chunks), true);
        sync.addHard(4, nullptr, &f, &other);
        sync.remove(4);
        ch.serviceCore1();
        CHECK_EQ(other.loadQueued(), false);
    }
}

// 書けなかったあとは、間 (1 秒) を空けるまで書き直さない
void testRetryWait() {
    printf("hard disk: retry waits after a failure\n");
    Rig r;
    const uint32_t size = 8 * CHUNK;
    FakeFs::File* f = r.fs.make("hd0", size);
    static uint8_t data[8 * CHUNK];
    memset(data, 0, sizeof data);
    x68k::HardDiskImage img;
    img.load(data, size, 512, false);
    r.sync.addHard(2, &img, f);
    data[100] = 1;
    img.markDirty(100);
    r.fs.failWrites = 1;
    uint64_t now = 10000;
    for (int i = 0; i < 100 && r.fs.events[(int)DiskEvent::WRITE_FAILED] == 0; i++) r.step(now += 50);
    CHECK_EQ(r.fs.events[(int)DiskEvent::WRITE_FAILED], 1);
    const uint64_t failedAt = now;
    while (now < failedAt + 900) {
        r.step(now += 50);
        CHECK_EQ(r.fs.writes, 0u); // まだ書き直さない
    }
    for (int i = 0; i < 10; i++) r.step(now += 50);
    CHECK_EQ(r.fs.writes, 1u);
    CHECK_EQ(f->data[100], 1);
}

// 書き直しの待ちと失敗の回数はディスクごと: 書けないハードディスクがあっても、同じ受け渡しのバッファに入った
// ほかのディスクのチャンクは書く。書けないディスクの待ちは、ほかのディスクが書けても 1 秒に戻らず、2 倍ずつ延びる
void testRetryPerDiskHard() {
    printf("retry wait and failure count are per disk (two hard disks)\n");
    Rig r;
    const uint32_t size = 8 * CHUNK;
    FakeFs::File* fa = r.fs.make("hdA", size);
    FakeFs::File* fb = r.fs.make("hdB", size);
    static uint8_t a[8 * CHUNK], b[8 * CHUNK];
    fillPattern(a, size, 21);
    fillPattern(b, size, 22);
    memcpy(fa->data, a, size);
    memcpy(fb->data, b, size);
    x68k::HardDiskImage ia, ib;
    ia.load(a, size, 512, false);
    ib.load(b, size, 512, false);
    r.sync.addHard(2, &ia, fa);
    r.sync.addHard(3, &ib, fb);
    a[CHUNK + 5] ^= 0xff;
    ia.markDirty(CHUNK + 5);
    b[2 * CHUNK + 7] ^= 0xff;
    ib.markDirty(2 * CHUNK + 7);
    fa->failWrites = 1000000; // A は書けないまま
    uint64_t now = 0;
    while (now < 12000) r.step(now += 50);
    // B は、A と同じバッファに入っていても書けている
    CHECK_EQ(memcmp(fb->data, b, size), 0);
    CHECK_EQ(memcmp(fa->data, a, size) != 0, true);
    CHECK_EQ(r.fs.diskEvents[3][(int)DiskEvent::WRITE_FAILED], 0);
    CHECK_EQ(r.sync.failures(3), 0);
    // A の書き直しの間は 1 秒、2 秒、4 秒 (12 秒の間に 4 回失敗する)
    uint64_t at[8];
    const int n = r.fs.times(2, DiskEvent::WRITE_FAILED, at, 8);
    CHECK_EQ(n, 4);
    if (n == 4) {
        CHECK_EQ(at[1] - at[0], 1000);
        CHECK_EQ(at[2] - at[1], 2000);
        CHECK_EQ(at[3] - at[2], 4000);
    }
    CHECK_EQ(r.sync.failures(2), 4);
    CHECK_EQ(r.sync.failures(), 4);
    CHECK_EQ(r.fs.events[(int)DiskEvent::RECOVERED], 0); // B が書けても「書けるようになった」とは知らせない
    CHECK_EQ(r.sync.busy(), true);                       // A のチャンクは受け渡しのバッファに残っている
    // A が書けるようになった: 次の書き直し (8 秒あと) で書け、A についてだけ知らせる。B は 2 度書かない
    fa->failWrites = 0;
    const unsigned writes = r.fs.writes;
    while (now < 25000 && (ia.dirty() || r.sync.busy())) r.step(now += 50);
    CHECK_EQ(memcmp(fa->data, a, size), 0);
    CHECK_EQ(r.fs.writes, writes + 1);
    CHECK_EQ(r.fs.events[(int)DiskEvent::RECOVERED], 1);
    CHECK_EQ(r.fs.diskEvents[2][(int)DiskEvent::RECOVERED], 1);
    CHECK_EQ(r.sync.failures(), 0);
    // 書いたバイト数は、ディスクごとに 1 チャンクずつ (B を 2 度数えない)
    CHECK_EQ(r.fs.diskEvents[2][(int)DiskEvent::WRITTEN], 1);
    CHECK_EQ(r.fs.diskEvents[3][(int)DiskEvent::WRITTEN], 1);
    for (int i = 0; i < r.fs.logLen; i++)
        if (r.fs.log[i].e == DiskEvent::WRITTEN) CHECK_EQ(r.fs.log[i].value, CHUNK);
    CHECK_EQ(r.sync.bytesWritten(), 2 * CHUNK);
}

// 書けないハードディスクのチャンクが受け渡しのバッファに残っている間も、同じバッファに入っていたフロッピーディスクの
// チャンクは写しに当て、ファイルを置き換える (バッファを返すまで待たない)
void testFloppyReplacedWhileHardStuck() {
    printf("floppy is replaced while a hard disk chunk is stuck in the buffer\n");
    Rig r;
    const uint32_t hsize = 8 * CHUNK;
    FakeFs::File* fh = r.fs.make("hd0", hsize);
    static uint8_t hd[8 * CHUNK];
    fillPattern(hd, hsize, 41);
    memcpy(fh->data, hd, hsize);
    x68k::HardDiskImage ih;
    ih.load(hd, hsize, 512, false);
    r.sync.addHard(2, &ih, fh);
    const uint32_t fsize = 1261568;
    static uint8_t fd[1261568], shadow[1261568];
    fillPattern(fd, fsize, 42);
    memcpy(shadow, fd, fsize);
    memcpy(r.fs.make("fd0.xdf", fsize)->data, fd, fsize);
    x68k::FloppyImage ifd;
    CHECK_EQ(ifd.load(fd, fsize, false, "fd0.xdf"), true);
    r.sync.addFloppy(0, &ifd, "fd0.xdf", "fd0.xdf.new", shadow, fsize);
    // 両方に書く (同じ受け渡しのバッファに入る)。ハードディスクは書けないまま
    hd[CHUNK + 3] ^= 0xff;
    ih.markDirty(CHUNK + 3);
    const x68k::FloppyImage::Sector* s = ifd.find(0, 0, 0, 2, 3);
    memset(ifd.data(s), 0x77, 1024);
    ifd.setMark(s, false);
    fh->failWrites = 1000000;
    uint64_t now = 0;
    while (now < 6000) r.step(now += 50);
    CHECK_EQ(r.fs.diskEvents[2][(int)DiskEvent::WRITE_FAILED] >= 2, true);
    CHECK_EQ(r.sync.busy(), true); // ハードディスクのチャンクはバッファに残っている
    CHECK_EQ(r.fs.diskEvents[0][(int)DiskEvent::REPLACED], 1);
    FakeFs::File* g = r.fs.find("fd0.xdf");
    CHECK_EQ(g && g->exists && g->size == fsize && memcmp(g->data, fd, fsize) == 0, true);
    // ハードディスクが書けるようになれば、残りも終わる。フロッピーディスクは置き換え直さない
    fh->failWrites = 0;
    while (now < 20000 && (ih.dirty() || r.sync.busy())) r.step(now += 50);
    CHECK_EQ(memcmp(fh->data, hd, hsize), 0);
    CHECK_EQ(r.sync.ioPending(), false);
    CHECK_EQ(r.fs.diskEvents[0][(int)DiskEvent::REPLACED], 1);
}

// 置き換えられないフロッピーディスクがあっても、ハードディスクとほかのフロッピーディスクの書き戻しは待たない。
// 置き換えられないディスクの待ちは、ほかのディスクが書けても 2 倍ずつ延びる
void testRetryPerDiskFloppy() {
    printf("retry wait and failure count are per disk (floppy that cannot be replaced)\n");
    Rig r;
    const uint32_t fsize = 1261568;
    static uint8_t fd[1261568], fd2[1261568], shadow[1261568], shadow2[1261568];
    fillPattern(fd, fsize, 31);
    fillPattern(fd2, fsize, 32);
    memcpy(shadow, fd, fsize);
    memcpy(shadow2, fd2, fsize);
    memcpy(r.fs.make("fdA.xdf", fsize)->data, fd, fsize);
    memcpy(r.fs.make("fdB.xdf", fsize)->data, fd2, fsize);
    x68k::FloppyImage ia, ib;
    CHECK_EQ(ia.load(fd, fsize, false, "fdA.xdf"), true);
    CHECK_EQ(ib.load(fd2, fsize, false, "fdB.xdf"), true);
    r.sync.addFloppy(0, &ia, "fdA.xdf", "fdA.xdf.new", shadow, fsize);
    r.sync.addFloppy(1, &ib, "fdB.xdf", "fdB.xdf.new", shadow2, fsize);
    const uint32_t hsize = 8 * CHUNK;
    FakeFs::File* fh = r.fs.make("hd0", hsize);
    static uint8_t hd[8 * CHUNK];
    fillPattern(hd, hsize, 33);
    memcpy(fh->data, hd, hsize);
    x68k::HardDiskImage ih;
    ih.load(hd, hsize, 512, false);
    r.sync.addHard(2, &ih, fh);
    // A に書く。A の置き換えは失敗し続ける
    const x68k::FloppyImage::Sector* sa = ia.find(0, 0, 0, 1, 3);
    memset(ia.data(sa), 0x55, 1024);
    ia.setMark(sa, false);
    r.fs.failWriteName = "fdA.xdf.new";
    r.fs.failNamedWrites = 1000000;
    uint64_t now = 0;
    while (r.fs.diskEvents[0][(int)DiskEvent::REPLACE_FAILED] == 0 && now < 5000) r.step(now += 50);
    CHECK_EQ(r.fs.diskEvents[0][(int)DiskEvent::REPLACE_FAILED], 1);
    const uint64_t failedAt = now;
    // A が 3 回失敗して 4 秒待っている間 (最初の失敗の 3 秒あとから 7 秒あとまで) に、ハードディスクと B に書く:
    // A の次の書き直しを待たずに書き戻す
    while (now < failedAt + 3500) r.step(now += 50);
    hd[3 * CHUNK + 9] ^= 0xff;
    ih.markDirty(3 * CHUNK + 9);
    const x68k::FloppyImage::Sector* sb = ib.find(4, 2, 0, 3, 3);
    memset(ib.data(sb), 0x66, 1024);
    ib.setMark(sb, false);
    const uint64_t wroteAt = now;
    uint64_t hardAt = 0, floppyAt = 0;
    while (now < failedAt + 15000) {
        r.step(now += 50);
        if (!hardAt && memcmp(fh->data, hd, hsize) == 0) hardAt = now;
        if (!floppyAt && r.fs.diskEvents[1][(int)DiskEvent::REPLACED]) floppyAt = now;
    }
    const uint64_t idle = DiskSync<FakeFs>::IDLE_MS;
    CHECK_EQ(hardAt != 0 && hardAt <= wroteAt + idle + 200, true);
    CHECK_EQ(floppyAt != 0 && floppyAt <= wroteAt + idle + 200, true);
    FakeFs::File* gb = r.fs.find("fdB.xdf");
    CHECK_EQ(gb && gb->exists && gb->size == fsize && memcmp(gb->data, fd2, fsize) == 0, true);
    // A の書き直しの間は、ほかのディスクが書けても 1 秒、2 秒、4 秒、8 秒
    uint64_t at[8];
    const int n = r.fs.times(0, DiskEvent::REPLACE_FAILED, at, 8);
    CHECK_EQ(n, 5);
    if (n == 5) {
        CHECK_EQ(at[1] - at[0], 1000);
        CHECK_EQ(at[2] - at[1], 2000);
        CHECK_EQ(at[3] - at[2], 4000);
        CHECK_EQ(at[4] - at[3], 8000);
    }
    CHECK_EQ(r.fs.events[(int)DiskEvent::RECOVERED], 0);
    CHECK_EQ(r.sync.failures(0), 5);
    CHECK_EQ(r.sync.failures(1), 0);
    CHECK_EQ(r.sync.failures(2), 0);
    // A の前の内容はそのまま残っている
    FakeFs::File* ga = r.fs.find("fdA.xdf");
    CHECK_EQ(ga && ga->exists && ga->size == fsize && memcmp(ga->data, fd, fsize) != 0, true);
    // 止めている間の主ループ (設定メニュー) も同じ: A が待っている間でも、ハードディスクはすぐ書く
    hd[5 * CHUNK + 1] ^= 0xff;
    ih.markDirty(5 * CHUNK + 1);
    r.fs.now = now += 50;
    r.sync.servicePaused(now);
    CHECK_EQ(memcmp(fh->data, hd, hsize), 0);
    CHECK_EQ(r.fs.diskEvents[0][(int)DiskEvent::REPLACE_FAILED], 5); // A はまだ書き直さない
    // A が置き換えられるようになった: 次の書き直しで置き換わり、A についてだけ知らせる
    r.fs.failNamedWrites = 0;
    while (now < failedAt + 40000 && r.sync.ioPending()) r.step(now += 50);
    ga = r.fs.find("fdA.xdf");
    CHECK_EQ(ga && ga->exists && ga->size == fsize && memcmp(ga->data, fd, fsize) == 0, true);
    CHECK_EQ(r.fs.events[(int)DiskEvent::RECOVERED], 1);
    CHECK_EQ(r.fs.diskEvents[0][(int)DiskEvent::RECOVERED], 1);
    CHECK_EQ(r.sync.failures(), 0);
}

// 使うチャンクだけを読むイメージのヒープの見積もり (platform/disk_cache_size.h)。中身の枠のほかに Slot の表、
// チャンク → 枠の表、dirty フラグ (32 チャンクごとに切り上げ) を入れ、64MB のイメージですべての枠を持つかを正しく決める
void testCacheHeapEstimate() {
    printf("cache: heap estimate and the number of slots\n");
    const platform::HeapModel exact = {nullptr, 0, 1}; // 確保した大きさそのまま
    const uint64_t mb64 = 64ull << 20;
    // 見積もりに入れた大きさが、実際に new[] する大きさと同じ
    {
        uint64_t a[x68k::DiskCache::ALLOCATIONS];
        x68k::DiskCache::allocations(mb64, 4096, a);
        x68k::DiskCache* cache = new x68k::DiskCache;
        x68k::HardDiskImage* img = new x68k::HardDiskImage;
        g_newCount = 0;
        CHECK_EQ(cache->init(mb64, 4096), true);
        CHECK_EQ(img->loadCached(cache, mb64, 256, false), true);
        const int n = g_newCount;
        g_newCount = -1;
        CHECK_EQ(n, 4);
        CHECK_EQ(g_newBytes[0], a[0]);
        CHECK_EQ(g_newBytes[1], a[1]);
        CHECK_EQ(g_newBytes[2], a[2]);
        CHECK_EQ(g_newBytes[3], DirtyMap::allocationBytes(mb64));
        delete img;
        delete cache;
    }
    // 64MB、4096 枠: 中身 64MB、Slot の表 4096 x 12、チャンク → 枠の表 4096 x 4、dirty フラグ 128 語
    const uint64_t objects = sizeof(x68k::DiskCache) + sizeof(x68k::HardDiskImage);
    CHECK_EQ(platform::diskCacheHeapBytes(mb64, 4096, exact), mb64 + 4096 * 12 + 4096 * 4 + 128 * 4 + objects);
    // 前の見積もり (枠の中身、チャンク → 枠の表、dirty フラグ) より Slot の表の分 (48KB) だけ多い
    const uint64_t old = mb64 + (mb64 / CHUNK + 1) * 2 + mb64 / 8 / CHUNK;
    CHECK_EQ(platform::diskCacheHeapBytes(mb64, 4096, exact) - old >= 48 * 1024, true);
    // チャンクの倍数でない大きさと、32 チャンクより小さいイメージ (dirty フラグは 1 語、表は切り上げたチャンクの数)
    CHECK_EQ(platform::diskCacheHeapBytes(5 * CHUNK + 1, 6, exact), 6 * CHUNK + 6 * 12 + 6 * 4 + 4 + objects);
    CHECK_EQ(DirtyMap::allocationBytes(33 * CHUNK), 8u);
    CHECK_EQ(DirtyMap::allocationBytes(1), 4u);

    // Circle のヒープ (バケットへの切り上げと 64 バイトのヘッダ)
    static const uint32_t buckets[] = {0x40, 0x400, 0x1000, 0x4000, 0x10000, 0x40000, 0x80000, 0};
    const platform::HeapModel circle = {buckets, 64, 64};
    CHECK_EQ(platform::heapBlockBytes(1, circle), 128u);
    CHECK_EQ(platform::heapBlockBytes(4096 * 12, circle), 64u + 0x10000);
    CHECK_EQ(platform::heapBlockBytes(0x80001, circle), 64u + 0x80040);
    CHECK_EQ(platform::heapBlockBytes(mb64, circle), mb64 + 64);

    // 枠の数: すべての枠が 1GB より下のヒープにちょうど入ればすべて、1 バイト足りなければ 256 個
    const unsigned small = platform::DISK_CACHE_SLOTS;
    auto plan = [&](uint64_t bytes, uint64_t low, uint64_t high) { return platform::planDiskCache(bytes, low, high, circle); };
    const uint64_t need = platform::diskCacheHeapBytes(mb64, 4096, circle);
    CHECK_EQ(plan(mb64, need, 0).slots, 4096u);
    CHECK_EQ(plan(mb64, need, 0).high, false);
    CHECK_EQ(plan(mb64, need, 0).whole, true);
    CHECK_EQ(plan(mb64, need - 1, 0).slots, small);
    CHECK_EQ(plan(mb64, need - 1, 0).whole, false);
    CHECK_EQ(plan(mb64, old, 0).slots, small); // 前の見積もりでは足りない
    // 64MB より大きいイメージは、1GB より下のヒープの半分までに収まるときだけ
    const uint64_t mb65 = mb64 + CHUNK;
    const uint64_t need65 = platform::diskCacheHeapBytes(mb65, 4097, circle);
    CHECK_EQ(plan(mb65, need65 * 2 - 1, 0).slots, small);
    CHECK_EQ(plan(mb65, need65 * 2, 0).slots, 4097u);
    CHECK_EQ(plan(mb65, need65 * 2, 0).high, false);
    // 1GB より上のヒープに枠の中身が入れば、そこに置く (1GB より下には表などだけ)
    const uint64_t data65 = platform::diskCacheDataBytes(4097, circle), table65 = platform::diskCacheTableBytes(mb65, 4097, circle);
    CHECK_EQ(data65, 4097ull * CHUNK + 64);
    CHECK_EQ(data65 + table65, need65);
    CHECK_EQ(plan(mb65, table65, data65).slots, 4097u);
    CHECK_EQ(plan(mb65, table65, data65).high, true);
    CHECK_EQ(plan(mb65, table65, data65 - 1).slots, small); // 上に入らず、下の半分にも入らない
    CHECK_EQ(plan(mb65, table65 - 1, data65).slots, small); // 表が下に入らない
    CHECK_EQ(plan(mb65, need65 * 2, data65 - 1).high, false); // 上に入らなければ下に
    CHECK_EQ(plan(mb65, need65 * 2, data65 - 1).slots, 4097u);
    // 2GB のイメージ (チャンク → 枠の表は 32 ビット)
    const uint64_t gb2 = 2ull << 30;
    CHECK_EQ(plan(gb2, 16ull << 20, 3ull << 30).slots, (unsigned)(gb2 / CHUNK));
    CHECK_EQ(plan(gb2, 16ull << 20, 3ull << 30).high, true);
    CHECK_EQ(plan(gb2, 900ull << 20, 0).slots, small);
    // 枠の数の上限 (4GB - 16KB) までは全体、超えるイメージは 256 個
    CHECK_EQ((uint64_t)x68k::DiskCache::MAX_SLOTS * CHUNK + 64 < (1ull << 32), true);
    CHECK_EQ(plan((uint64_t)x68k::DiskCache::MAX_SLOTS * CHUNK, ~0ull, ~0ull).slots, x68k::DiskCache::MAX_SLOTS);
    CHECK_EQ(plan((uint64_t)(x68k::DiskCache::MAX_SLOTS + 1) * CHUNK, ~0ull, ~0ull).slots, small);
    // 端数のあるイメージはチャンクの数を切り上げる
    CHECK_EQ(plan(mb64 - CHUNK + 1, ~0ull, 0).slots, 4096u);
    // 256 個以下ならすべての枠 (4 個以上)。入るかは呼んだ側が調べる
    CHECK_EQ(plan(100 * CHUNK, 0, 0).slots, 100u);
    CHECK_EQ(plan(100 * CHUNK, 0, 0).whole, true);
    CHECK_EQ(plan(3 * CHUNK, 0, 0).slots, 4u);
}

} // namespace

int main() {
    testCacheHeapEstimate();
    testDirtyMap();
    testFloppyMarks();
    testIdleFlush();
    testMaxAge();
    testWriteFailure();
    testFloppy();
    testServiceWithoutIo();
    testThreads();
    testRemoveDropsRecords();
    testCachePinned();
    testWholeImageCache();
    testFillAhead();
    testRetryWait();
    testRetryPerDiskHard();
    testFloppyReplacedWhileHardStuck();
    testRetryPerDiskFloppy();
    testCachedRead();
    testCacheReadFailures();
    testCachedWrite();
    testCachedThreads();
    return finish();
}
