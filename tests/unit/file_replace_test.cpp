// src/platform/file_replace.h のテスト: 作り物のファイルシステムで、書き込み、名前の変更、削除の失敗を起こし、
// どの失敗でも、読む側 (path があれば path、なければ tmp) が前の内容か新しい内容のどちらかを読めること
#include "platform/file_replace.h"
#include "test_check.h"

#include <string.h>

namespace {

using platform::FsResult;

// ファイルは 2 つ (path と tmp) だけ。失敗させる操作を数で選ぶ
struct FakeFs {
    struct File {
        bool exists = false;
        char data[32] = {};
    } files[2];
    const char* names[2] = {"cfg", "new"};
    // 失敗させるもの
    bool failRename = false, failWrite = false, shortWrite = false, failRemovePath = false, failExists = false;
    bool renameLies = false; // rename は OK を返すが、何もしない

    File* find(const char* n) {
        for (int i = 0; i < 2; i++)
            if (!strcmp(n, names[i])) return &files[i];
        return nullptr;
    }
    FsResult exists(const char* n) {
        if (failExists) return FsResult::ERROR;
        return find(n)->exists ? FsResult::OK : FsResult::NO_FILE;
    }
    FsResult write(const char* n, const void* d, unsigned size) {
        File* f = find(n);
        f->exists = true; // 作り直す (切り詰める)
        memset(f->data, 0, sizeof f->data);
        if (failWrite) return FsResult::ERROR;
        unsigned n2 = shortWrite ? size / 2 : size;
        memcpy(f->data, d, n2);
        return shortWrite ? FsResult::ERROR : FsResult::OK;
    }
    FsResult remove(const char* n) {
        File* f = find(n);
        if (f == &files[0] && failRemovePath) return FsResult::ERROR;
        if (!f->exists) return FsResult::NO_FILE;
        f->exists = false;
        return FsResult::OK;
    }
    FsResult rename(const char* from, const char* to) {
        if (failRename) return FsResult::ERROR;
        if (renameLies) return FsResult::OK;
        File* a = find(from);
        File* b = find(to);
        if (!a->exists || b->exists) return FsResult::ERROR;
        *b = *a;
        a->exists = false;
        return FsResult::OK;
    }
    void put(int i, const char* s) {
        files[i].exists = true;
        memset(files[i].data, 0, sizeof files[i].data);
        strcpy(files[i].data, s);
    }
    // 読む側: path があれば path、なければ tmp
    const char* read() {
        if (files[0].exists) return files[0].data;
        if (files[1].exists) return files[1].data;
        return nullptr;
    }
};

FsResult replace(FakeFs& fs, const char* s) { return platform::replaceFile(fs, "cfg", "new", s, (unsigned)strlen(s)); }

bool reads(FakeFs& fs, const char* want) {
    const char* got = fs.read();
    bool ok = got && !strcmp(got, want);
    if (!ok) printf("  read: %s, want %s\n", got ? got : "(none)", want);
    return ok;
}

void testNormal() {
    printf("normal\n");
    FakeFs fs;
    CHECK_EQ((int)replace(fs, "one"), (int)FsResult::OK); // どちらもない
    CHECK_EQ(reads(fs, "one"), true);
    CHECK_EQ(fs.files[1].exists, false);
    CHECK_EQ((int)replace(fs, "two"), (int)FsResult::OK);
    CHECK_EQ(reads(fs, "two"), true);
    CHECK_EQ(fs.files[1].exists, false);
}

void testWriteFails() {
    printf("write fails\n");
    for (int shortW = 0; shortW < 2; shortW++) {
        FakeFs fs;
        fs.put(0, "old");
        (shortW ? fs.shortWrite : fs.failWrite) = true;
        CHECK_EQ((int)replace(fs, "newvalue"), (int)FsResult::ERROR);
        CHECK_EQ(reads(fs, "old"), true);
        CHECK_EQ(fs.files[1].exists, false); // 半端な tmp は残さない
    }
}

// canRename() を持つ Fs (ベアメタル版の USB メモリのときの空きの判定に当たる)
struct GuardedFs : FakeFs {
    bool allowRename = true;
    int asked = 0;
    bool canRename(const char*, const char*) {
        asked++;
        return allowRename;
    }
};

void testRecovery() {
    printf("recovery\n");
    // path を消したあとで止まり、tmp だけがある: tmp を path に戻してから書く
    FakeFs fs;
    fs.put(1, "only");
    CHECK_EQ((int)replace(fs, "next"), (int)FsResult::OK);
    CHECK_EQ(reads(fs, "next"), true);
    // 戻すときに名前を変えられなければ、唯一の写しを書き直さない
    FakeFs f2;
    f2.put(1, "only");
    f2.failRename = true;
    CHECK_EQ((int)replace(f2, "next"), (int)FsResult::ERROR);
    CHECK_EQ(reads(f2, "only"), true);
    // 名前を変えたと返しても、変わっていなければ書かない
    FakeFs f3;
    f3.put(1, "only");
    f3.renameLies = true;
    CHECK_EQ((int)replace(f3, "next"), (int)FsResult::ERROR);
    CHECK_EQ(reads(f3, "only"), true);
    // 戻したあとで書き込みに失敗しても、戻した内容が残る
    FakeFs f4;
    f4.put(1, "only");
    f4.failWrite = true;
    CHECK_EQ((int)replace(f4, "next"), (int)FsResult::ERROR);
    CHECK_EQ(reads(f4, "only"), true);
    // 名前を変えてよいかを聞かれて断ると (空きが少ないドライブ)、戻さず、書きもしない。tmp が唯一の写しのまま残る
    GuardedFs g;
    g.put(1, "only");
    g.allowRename = false;
    CHECK_EQ((int)platform::replaceFile(g, "cfg", "new", "next", 5), (int)FsResult::ERROR);
    CHECK_EQ(g.asked, 1);
    CHECK_EQ(g.files[0].exists, false);
    CHECK_EQ(g.files[1].exists, true);
    CHECK_EQ(strcmp(g.files[1].data, "only"), 0);
    // 許せば、戻してから書く (聞くのは戻すときの 1 回だけ。書いたあとの名前の変更では聞かない)
    GuardedFs g2;
    g2.put(1, "only");
    CHECK_EQ((int)platform::replaceFile(g2, "cfg", "new", "next", 5), (int)FsResult::OK);
    CHECK_EQ(g2.asked, 1);
    CHECK_EQ(strcmp(g2.files[0].data, "next"), 0);
    CHECK_EQ(g2.files[1].exists, false);
    // 戻すものがなければ聞かない
    GuardedFs g3;
    g3.put(0, "prev");
    g3.allowRename = false;
    CHECK_EQ((int)platform::replaceFile(g3, "cfg", "new", "next", 5), (int)FsResult::OK);
    CHECK_EQ(g3.asked, 0);
    CHECK_EQ(strcmp(g3.files[0].data, "next"), 0);
    // ある / ないが分からなければ何もしない
    FakeFs f5;
    f5.put(1, "only");
    f5.failExists = true;
    CHECK_EQ((int)replace(f5, "next"), (int)FsResult::ERROR);
    CHECK_EQ(reads(f5, "only"), true);
}

void testRemoveAndRenameFail() {
    printf("remove / rename fails\n");
    // path を消せなければ、path は前のまま、書いた tmp は消す
    FakeFs fs;
    fs.put(0, "old");
    fs.failRemovePath = true;
    CHECK_EQ((int)replace(fs, "new1"), (int)FsResult::ERROR);
    CHECK_EQ(reads(fs, "old"), true);
    CHECK_EQ(fs.files[1].exists, false);
    // 最後の名前の変更に失敗: path はなく、tmp が新しい内容を持つ (読む側は tmp を読む)
    FakeFs f2;
    f2.put(0, "old");
    f2.failRename = true;
    CHECK_EQ((int)replace(f2, "new2"), (int)FsResult::ERROR);
    CHECK_EQ(reads(f2, "new2"), true);
    // 次に書くときは、tmp を path に戻してから書く
    f2.failRename = false;
    CHECK_EQ((int)replace(f2, "new3"), (int)FsResult::OK);
    CHECK_EQ(reads(f2, "new3"), true);
    // path と古い tmp が両方ある (path を消す前に止まった): path が読まれるので、tmp を書き直してよい
    FakeFs f3;
    f3.put(0, "cur");
    f3.put(1, "stale");
    CHECK_EQ((int)replace(f3, "new4"), (int)FsResult::OK);
    CHECK_EQ(reads(f3, "new4"), true);
    CHECK_EQ(f3.files[1].exists, false);
}

} // namespace

// readWithFallback(): path があれば (空でも) path を読み、path がないときだけ tmp を読む。結果は FatFs の FRESULT に
// 似せた数で返す (ベアメタル版の CKernel::ReadWithFallback() と同じ使い方)
enum FakeResult { R_OK, R_NO_FILE, R_NO_PATH, R_DISK_ERR };
struct ReadFs {
    FakeResult pathResult = R_OK, tmpResult = R_OK;
    const char* pathData = "path";
    const char* tmpData = "tmp";
    const char* got = nullptr; // 最後に読めた中身
    int reads = 0;
    FakeResult read(const char* n) {
        reads++;
        const bool isPath = strcmp(n, "cfg") == 0;
        const FakeResult r = isPath ? pathResult : tmpResult;
        got = r == R_OK ? (isPath ? pathData : tmpData) : nullptr;
        return r;
    }
    bool missing(FakeResult r) const { return r == R_NO_FILE || r == R_NO_PATH; }
    bool ok(FakeResult r) const { return r == R_OK; }
};

void testReadWithFallback() {
    printf("read with fallback\n");
    struct Case {
        const char* name;
        FakeResult path, tmp;
        FakeResult want;
        const char* got; // 読めた中身 (nullptr なら読めない)
        bool usedTmp;
        int reads;
    };
    static const Case cases[] = {
        {"path exists", R_OK, R_OK, R_OK, "path", false, 1},
        {"no path, tmp exists", R_NO_FILE, R_OK, R_OK, "tmp", true, 2},
        {"no directory, tmp exists", R_NO_PATH, R_OK, R_OK, "tmp", true, 2},
        {"neither exists", R_NO_FILE, R_NO_FILE, R_NO_FILE, nullptr, false, 2},
        {"no path, no directory for tmp", R_NO_FILE, R_NO_PATH, R_NO_FILE, nullptr, false, 2},
        {"no path, tmp cannot be read", R_NO_FILE, R_DISK_ERR, R_DISK_ERR, nullptr, false, 2},
        // path が読めないときは tmp を読まない (path が今の内容を持っているかもしれない)
        {"path cannot be read", R_DISK_ERR, R_OK, R_DISK_ERR, nullptr, false, 1},
    };
    for (const Case& c : cases) {
        ReadFs fs;
        fs.pathResult = c.path;
        fs.tmpResult = c.tmp;
        const char* const cfg = "cfg";
        const char* const tmp = "new";
        const auto from = platform::readWithFallback(fs, cfg, tmp);
        const FakeResult r = from.result;
        const bool usedTmp = from.usedTmp;
        CHECK_EQ(from.path == (c.usedTmp ? tmp : cfg), true); // 読んだファイルのパス
        if (r != c.want || usedTmp != c.usedTmp || fs.reads != c.reads ||
            (c.got ? (fs.got == nullptr || strcmp(fs.got, c.got) != 0) : fs.got != nullptr))
            printf("  %s\n", c.name);
        CHECK_EQ(r, c.want);
        CHECK_EQ(usedTmp, c.usedTmp);
        CHECK_EQ(fs.reads, c.reads);
        CHECK_EQ(c.got ? fs.got != nullptr && strcmp(fs.got, c.got) == 0 : fs.got == nullptr, true);
    }
    // path が空のファイルでも path を読む (空の pix68k.cfg は「設定なし」で、古い tmp は読まない)
    ReadFs fs;
    fs.pathData = "";
    const auto from = platform::readWithFallback(fs, "cfg", "new");
    CHECK_EQ(from.result, R_OK);
    CHECK_EQ(from.usedTmp, false);
    CHECK_EQ(strcmp(from.path, "cfg"), 0);
    CHECK_EQ(strcmp(fs.got, ""), 0);
}

int main() {
    testNormal();
    testWriteFails();
    testRecovery();
    testRemoveAndRenameFail();
    testReadWithFallback();
    return finish();
}
