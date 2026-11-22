// src/platform/floppy_image_save.h のテスト: USB-FDD から読んだディスクをイメージのファイルに書き出す手順 (#287)。
// あるファイルを上書きしないこと、書いたものを読み戻して確かめること、失敗したときに書きかけを残さないこと
#include "platform/floppy_image_save.h"
#include "test_check.h"

#include <string.h>

namespace {

using namespace platform;

// 名前が 4 つまでの、メモリの中のファイルシステム
struct FakeFs {
    struct File {
        char name[64];
        uint8_t data[64];
        unsigned size;
        bool dir, used;
    } files[4] = {};
    // 失敗させる操作
    bool statError = false, writeError = false, removeError = false, renameError = false, readError = false;
    bool corrupt = false;           // 書いたものの 1 バイトが化ける
    const char* appear = nullptr;   // write() の間に、この名前のファイルができる
    int writes = 0, removes = 0, renames = 0;
    int removeErrorAfter = -1; // この回数だけ remove() が動いたあとは、消せない (-1 なら使わない)
    const void* writtenFrom = nullptr;

    File* find(const char* name) {
        for (File& f : files)
            if (f.used && strcmp(f.name, name) == 0) return &f;
        return nullptr;
    }
    File* put(const char* name, const void* data, unsigned size, bool dir = false) {
        File* f = find(name);
        for (File& g : files)
            if (!f && !g.used) f = &g;
        f->used = true;
        f->dir = dir;
        strcpy(f->name, name);
        f->size = size;
        if (size) memcpy(f->data, data, size);
        return f;
    }
    FsResult stat(const char* path, unsigned long long& bytes, bool& dir) {
        if (statError) return FsResult::ERROR;
        File* f = find(path);
        if (!f) return FsResult::NO_FILE;
        bytes = f->size;
        dir = f->dir;
        return FsResult::OK;
    }
    FsResult write(const char* path, const void* data, unsigned size) {
        writes++;
        writtenFrom = data;
        if (appear) put(appear, "other", 5);
        if (writeError) {
            put(path, data, size / 2); // 途中まで書いて失敗した
            return FsResult::ERROR;
        }
        File* f = put(path, data, size);
        if (corrupt) f->data[3] ^= 0x40;
        return FsResult::OK;
    }
    FsResult remove(const char* path) {
        if (removeErrorAfter >= 0 && removes >= removeErrorAfter) {
            removes++;
            return FsResult::ERROR;
        }
        removes++;
        if (removeError) return FsResult::ERROR;
        File* f = find(path);
        if (!f) return FsResult::NO_FILE;
        f->used = false;
        return FsResult::OK;
    }
    FsResult rename(const char* from, const char* to) {
        renames++;
        File* f = find(from);
        if (renameError || !f || find(to)) return FsResult::ERROR; // to があれば上書きしない
        strcpy(f->name, to);
        return FsResult::OK;
    }
    bool crc32(const char* path, uint32_t& crc) {
        File* f = find(path);
        if (readError || !f) return false;
        crc = crc32Of(f->data, f->size);
        return true;
    }
};

const uint8_t kImage[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const char* const kPath = "FD-0BADF00D.XDF";
const char* const kTmp = "FD-0BADF00D.XDF.new";

FloppyImageSave save(FakeFs& fs) {
    return saveFloppyImageFile(fs, kPath, kTmp, kImage, sizeof kImage, crc32Of(kImage, sizeof kImage));
}

void testCrc() {
    // zlib の crc32() と同じ値
    CHECK_EQ(crc32Of((const uint8_t*)"123456789", 9), 0xcbf43926u);
    CHECK_EQ(crc32Of(nullptr, 0), 0);
    // 分けて読んでも同じ
    uint32_t s = crc32Update(0xffffffffu, (const uint8_t*)"1234", 4);
    s = crc32Update(s, (const uint8_t*)"56789", 5);
    CHECK_EQ(~s, 0xcbf43926u);
}

void testSave() {
    FakeFs fs;
    CHECK_EQ((int)save(fs), (int)FloppyImageSave::SAVED);
    CHECK_EQ(floppyImageSaved(FloppyImageSave::SAVED), true);
    FakeFs::File* f = fs.find(kPath);
    CHECK_EQ(f != nullptr, true);
    if (f) {
        CHECK_EQ(f->size, sizeof kImage);
        CHECK_EQ(memcmp(f->data, kImage, sizeof kImage), 0);
    }
    CHECK_EQ(fs.find(kTmp) == nullptr, true); // 書きかけの名前は残らない
    CHECK_EQ(fs.writes, 1);
    CHECK_EQ(fs.renames, 1);

    // もう一度: 書き出し済み。何も書かない、消さない
    fs.writes = fs.removes = fs.renames = 0;
    CHECK_EQ((int)save(fs), (int)FloppyImageSave::ALREADY);
    CHECK_EQ(floppyImageSaved(FloppyImageSave::ALREADY), true);
    CHECK_EQ(fs.writes + fs.removes + fs.renames, 0);
}

// 同じ名前のものがあるとき、あるかどうか分からないときは、何もしない
void testExisting() {
    {
        FakeFs fs;
        fs.put(kPath, "short", 5);
        CHECK_EQ((int)save(fs), (int)FloppyImageSave::OTHER_SIZE);
        CHECK_EQ(floppyImageSaved(FloppyImageSave::OTHER_SIZE), false);
        CHECK_EQ(fs.writes + fs.removes + fs.renames, 0);
        CHECK_EQ(fs.find(kPath)->size, 5);
    }
    {
        FakeFs fs; // 同じ大きさでも、ディレクトリなら別のもの
        fs.put(kPath, kImage, sizeof kImage, true);
        CHECK_EQ((int)save(fs), (int)FloppyImageSave::OTHER_SIZE);
        CHECK_EQ(fs.writes + fs.removes + fs.renames, 0);
    }
    {
        FakeFs fs;
        fs.statError = true;
        CHECK_EQ((int)save(fs), (int)FloppyImageSave::CHECK_FAILED);
        CHECK_EQ(fs.writes + fs.removes + fs.renames, 0);
    }
}

// 前の書きかけ (.new) が残っていても、それを本来の名前にしない。消して書き直す
void testStaleTmp() {
    FakeFs fs;
    fs.put(kTmp, "stale half", 10);
    CHECK_EQ((int)save(fs), (int)FloppyImageSave::SAVED);
    CHECK_EQ(fs.find(kPath)->size, sizeof kImage);
    CHECK_EQ(memcmp(fs.find(kPath)->data, kImage, sizeof kImage), 0);
    CHECK_EQ(fs.find(kTmp) == nullptr, true);

    // 書きかけを消せなければ、書かない
    FakeFs fs2;
    fs2.put(kTmp, "stale half", 10);
    fs2.removeError = true;
    CHECK_EQ((int)save(fs2), (int)FloppyImageSave::WRITE_FAILED);
    CHECK_EQ(fs2.writes, 0);
    CHECK_EQ(fs2.find(kPath) == nullptr, true);
}

void testFailures() {
    {
        FakeFs fs; // 書けない (いっぱいなど): 書きかけを消す
        fs.writeError = true;
        CHECK_EQ((int)save(fs), (int)FloppyImageSave::WRITE_FAILED);
        CHECK_EQ(fs.find(kPath) == nullptr, true);
        CHECK_EQ(fs.find(kTmp) == nullptr, true);
        CHECK_EQ(fs.renames, 0);
    }
    {
        FakeFs fs; // 書いたものが化けた: 名前を変えずに消す
        fs.corrupt = true;
        CHECK_EQ((int)save(fs), (int)FloppyImageSave::VERIFY_FAILED);
        CHECK_EQ(fs.find(kPath) == nullptr, true);
        CHECK_EQ(fs.find(kTmp) == nullptr, true);
        CHECK_EQ(fs.renames, 0);
    }
    {
        FakeFs fs; // 読み戻せない
        fs.readError = true;
        CHECK_EQ((int)save(fs), (int)FloppyImageSave::VERIFY_FAILED);
        CHECK_EQ(fs.find(kPath) == nullptr, true);
        CHECK_EQ(fs.find(kTmp) == nullptr, true);
    }
    {
        FakeFs fs; // 名前を変えられない
        fs.renameError = true;
        CHECK_EQ((int)save(fs), (int)FloppyImageSave::WRITE_FAILED);
        CHECK_EQ(fs.find(kPath) == nullptr, true);
        CHECK_EQ(fs.find(kTmp) == nullptr, true);
    }
}

// 失敗したあとに書きかけ (.new) を消せたかどうかを、呼んだ側に返す (消せなかったのに「消した」と言わない)
void testTmpLeft() {
    const uint32_t crc = crc32Of(kImage, sizeof kImage);
    bool left = true;
    {
        FakeFs fs; // 成功: 残らない
        CHECK_EQ((int)saveFloppyImageFile(fs, kPath, kTmp, kImage, sizeof kImage, crc, &left), (int)FloppyImageSave::SAVED);
        CHECK_EQ(left, false);
    }
    {
        FakeFs fs; // 読み戻しが合わず、消せた
        fs.corrupt = true;
        left = true;
        CHECK_EQ((int)saveFloppyImageFile(fs, kPath, kTmp, kImage, sizeof kImage, crc, &left),
                 (int)FloppyImageSave::VERIFY_FAILED);
        CHECK_EQ(left, false);
        CHECK_EQ(fs.find(kTmp) == nullptr, true);
    }
    {
        FakeFs fs; // 読み戻しが合わず、消せなかった (最初の「古い書きかけを消す」は通り、そのあとが消せない)
        fs.corrupt = true;
        fs.removeErrorAfter = 1;
        left = false;
        CHECK_EQ((int)saveFloppyImageFile(fs, kPath, kTmp, kImage, sizeof kImage, crc, &left),
                 (int)FloppyImageSave::VERIFY_FAILED);
        CHECK_EQ(left, true);
        CHECK_EQ(fs.find(kTmp) != nullptr, true);
        CHECK_EQ(fs.find(kPath) == nullptr, true);
    }
    {
        FakeFs fs; // 書けず、書きかけも消せなかった
        fs.writeError = true;
        fs.removeErrorAfter = 1;
        left = false;
        CHECK_EQ((int)saveFloppyImageFile(fs, kPath, kTmp, kImage, sizeof kImage, crc, &left),
                 (int)FloppyImageSave::WRITE_FAILED);
        CHECK_EQ(left, true);
    }
    {
        FakeFs fs; // 書いている間に同じ名前ができ、書いたほうを消せなかった
        fs.appear = kPath;
        fs.removeErrorAfter = 1;
        left = false;
        CHECK_EQ((int)saveFloppyImageFile(fs, kPath, kTmp, kImage, sizeof kImage, crc, &left),
                 (int)FloppyImageSave::APPEARED);
        CHECK_EQ(left, true);
        CHECK_EQ(fs.find(kPath)->size, 5);
    }
    {
        FakeFs fs; // 古い書きかけを消せない: 書かない。書きかけは残っている
        fs.put(kTmp, "stale half", 10);
        fs.removeError = true;
        left = false;
        CHECK_EQ((int)saveFloppyImageFile(fs, kPath, kTmp, kImage, sizeof kImage, crc, &left),
                 (int)FloppyImageSave::WRITE_FAILED);
        CHECK_EQ(left, true);
    }
    {
        FakeFs fs; // 書く前に断る場合は、書きかけに触っていない
        fs.statError = true;
        left = true;
        CHECK_EQ((int)saveFloppyImageFile(fs, kPath, kTmp, kImage, sizeof kImage, crc, &left),
                 (int)FloppyImageSave::CHECK_FAILED);
        CHECK_EQ(left, false);
    }
}

// 書いている間に同じ名前のファイルができたら、それを消さない
void testAppeared() {
    FakeFs fs;
    fs.appear = kPath;
    CHECK_EQ((int)save(fs), (int)FloppyImageSave::APPEARED);
    CHECK_EQ(floppyImageSaved(FloppyImageSave::APPEARED), false);
    FakeFs::File* f = fs.find(kPath);
    CHECK_EQ(f != nullptr, true);
    if (f) {
        CHECK_EQ(f->size, 5);
        CHECK_EQ(memcmp(f->data, "other", 5), 0);
    }
    CHECK_EQ(fs.find(kTmp) == nullptr, true);
}

// 新しいファイルを書く手順: どこで失敗しても ERROR で、開いたら必ず閉じる
struct FakeFile {
    bool exists = false;    // 同じ名前がある (openNew() が失敗する)
    unsigned room = 1000;   // 書ける長さ
    bool writeError = false, syncError = false, closeError = false;
    int opens = 0, writes = 0, syncs = 0, closes = 0;
    bool openNew() {
        opens++;
        return !exists;
    }
    bool write(const void*, unsigned size, unsigned& written) {
        writes++;
        if (writeError) return false;
        written = size < room ? size : room;
        return true;
    }
    bool sync() {
        syncs++;
        return !syncError;
    }
    bool close() {
        closes++;
        return !closeError;
    }
};

void testWriteNewFile() {
    {
        FakeFile f;
        CHECK_EQ((int)writeNewFile(f, kImage, sizeof kImage), (int)FsResult::OK);
        CHECK_EQ(f.opens * 1000 + f.writes * 100 + f.syncs * 10 + f.closes, 1111);
    }
    {
        FakeFile f; // 同じ名前がある: 開かず、書かない
        f.exists = true;
        CHECK_EQ((int)writeNewFile(f, kImage, sizeof kImage), (int)FsResult::ERROR);
        CHECK_EQ(f.writes + f.syncs + f.closes, 0);
    }
    {
        FakeFile f; // 途中までしか書けない (媒体がいっぱい)
        f.room = 5;
        CHECK_EQ((int)writeNewFile(f, kImage, sizeof kImage), (int)FsResult::ERROR);
        CHECK_EQ(f.syncs, 0);
        CHECK_EQ(f.closes, 1);
    }
    {
        FakeFile f;
        f.writeError = true;
        CHECK_EQ((int)writeNewFile(f, kImage, sizeof kImage), (int)FsResult::ERROR);
        CHECK_EQ(f.closes, 1);
    }
    {
        FakeFile f;
        f.syncError = true;
        CHECK_EQ((int)writeNewFile(f, kImage, sizeof kImage), (int)FsResult::ERROR);
        CHECK_EQ(f.closes, 1);
    }
    {
        FakeFile f;
        f.closeError = true;
        CHECK_EQ((int)writeNewFile(f, kImage, sizeof kImage), (int)FsResult::ERROR);
    }
}

// HTTP: パスの見分けと、答え方の順 (ないなら何も確保せずに 404、HEAD は写さない、足りなければ 503)
void testHttp() {
    CHECK_EQ(floppyImagePathDrive("/fd0.xdf"), 0);
    CHECK_EQ(floppyImagePathDrive("/fd1.xdf"), 1);
    CHECK_EQ(floppyImagePathDrive("/fd2.xdf"), -1);
    CHECK_EQ(floppyImagePathDrive("/fd0.xdf/"), -1);
    CHECK_EQ(floppyImagePathDrive("/fd0.xd"), -1);
    CHECK_EQ(floppyImagePathDrive("/FD0.XDF"), -1);
    CHECK_EQ(floppyImagePathDrive("/files/pix68k/fd0.xdf"), -1);
    CHECK_EQ(floppyImagePathDrive(""), -1);
    CHECK_EQ(floppyImagePathDrive(nullptr), -1);

    typedef FloppyImageReply R;
    const unsigned n = 1261568;
    // ディスクがない (空のドライブ、ファイルから入れたディスク): バッファがあってもなくても、HEAD でも 404
    for (int head = 0; head < 2; head++)
        for (int have = 0; have < 2; have++)
            for (int can = 0; can < 2; can++)
                CHECK_EQ((int)floppyImageReply(0, head != 0, have != 0, can != 0), (int)R::NOT_FOUND);
    // HEAD: 確保も写しもしない
    CHECK_EQ((int)floppyImageReply(n, true, false, false), (int)R::HEAD_ONLY);
    CHECK_EQ((int)floppyImageReply(n, true, false, true), (int)R::HEAD_ONLY);
    CHECK_EQ((int)floppyImageReply(n, true, true, true), (int)R::HEAD_ONLY);
    // GET: バッファがあれば使う。なければ、空きがあるときだけ確保する
    CHECK_EQ((int)floppyImageReply(n, false, true, false), (int)R::SEND);
    CHECK_EQ((int)floppyImageReply(n, false, false, true), (int)R::ALLOCATE);
    CHECK_EQ((int)floppyImageReply(n, false, false, false), (int)R::NO_MEMORY);
}

} // namespace

int main() {
    testWriteNewFile();
    testHttp();
    testCrc();
    testSave();
    testExisting();
    testStaleTmp();
    testFailures();
    testAppeared();
    testTmpLeft();
    return finish();
}
