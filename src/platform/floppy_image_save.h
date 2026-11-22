// 実物のフロッピーディスクドライブから読んだディスクを、イメージのファイルに書き出す手順 (#287)。
// あるファイルは上書きしない:
//   1. path があれば書かない (同じ大きさなら「書き出し済み」、違う大きさなら別のもの)。あるかどうかを確かめられなくても書かない
//   2. tmp (<path>.new) に残っていたものを消し、tmp に全部を書く
//   3. tmp を読み戻して CRC-32 を確かめる (名前に入れた CRC-32 と違えば、tmp を消して失敗)
//   4. tmp の名前を path に変える。名前を変える操作は、path があれば失敗するもの (FatFs の f_rename()) を使う。
//      2〜3 の間に path ができていても、消さずに失敗する
// platform::replaceFile() (platform/file_replace.h) は path を消してから名前を変えるので、ここでは使わない。
//
// ファイルの操作は Fs に任せる (ベアメタル版は FatFs、試験は作り物)。Fs が持つもの:
//   FsResult stat(const char* path, unsigned long long& bytes, bool& dir)  ある: OK、ない: NO_FILE、分からない: ERROR
//   FsResult write(const char* path, const void*, unsigned)   その名前がないときだけ作って全部を書き、媒体に書き終えて
//                                                             閉じる (どれかに失敗すれば ERROR。手順は writeNewFile())
//   FsResult remove(const char* path)                         ないときは NO_FILE
//   FsResult rename(const char* from, const char* to)         to があれば ERROR (上書きしない)
//   bool crc32(const char* path, uint32_t& crc)               ファイルの全体を読んだ CRC-32。読めなければ false
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "platform/file_replace.h"

namespace platform {

// CRC-32 (zlib の crc32() と同じ)。分けて読むとき: state を 0xffffffff から始め、最後に ~state
inline uint32_t crc32Update(uint32_t state, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        state ^= data[i];
        for (int b = 0; b < 8; b++) state = (state >> 1) ^ (0xedb88320u & (0u - (state & 1)));
    }
    return state;
}
inline uint32_t crc32Of(const uint8_t* data, size_t size) { return ~crc32Update(0xffffffffu, data, size); }

// 新しいファイルを作って全部を書く手順 (saveFloppyImageFile() の Fs::write() の中身)。File が持つもの:
//   bool openNew()                              その名前のファイルがないときだけ作って開く (FatFs の FA_CREATE_NEW)
//   bool write(const void*, unsigned, unsigned& written)
//   bool sync()                                 媒体に書き終える
//   bool close()
// 書いた長さが足りない (媒体がいっぱい)、sync() か close() が失敗した、のどれでも ERROR。開いたら、失敗しても閉じる
template <class File> FsResult writeNewFile(File& f, const void* data, unsigned size) {
    if (!f.openNew()) return FsResult::ERROR;
    unsigned written = 0;
    bool ok = f.write(data, size, written) && written == size;
    if (ok) ok = f.sync();
    if (!f.close()) ok = false;
    return ok ? FsResult::OK : FsResult::ERROR;
}

// HTTP の GET / HEAD /fd<N>.xdf のパスなら、ドライブの番号 (0 か 1)。そうでなければ -1
inline int floppyImagePathDrive(const char* path) {
    static const char kHead[] = "/fd";
    static const char kTail[] = ".xdf";
    if (path == nullptr) return -1;
    for (int i = 0; i < 3; i++)
        if (path[i] != kHead[i]) return -1;
    if (path[3] != '0' && path[3] != '1') return -1;
    for (int i = 0; i < 5; i++) // 終わりの 0 まで
        if (path[4 + i] != kTail[i]) return -1;
    return path[3] - '0';
}

// GET / HEAD /fd<N>.xdf にどう答えるか。確かめる順: ディスクがあるか (なければ 404。何も確保しない) → HEAD なら
// 長さだけ → 写しのバッファがなければ、ヒープの空きを確かめてから確保する (足りなければ 503)
//   imageBytes: そのドライブの、USB-FDD から読んだディスクのイメージの大きさ (なければ 0)
//   haveBuffer: 写しのバッファを確保済み、canAllocate: いま確保してよい (ヒープに空きがある)
enum class FloppyImageReply {
    NOT_FOUND, // 404
    HEAD_ONLY, // 200 と長さだけ (写さない、確保しない)
    NO_MEMORY, // 503 (確保しない)
    SEND,      // 確保済みのバッファに写して送る
    ALLOCATE,  // バッファを確保してから、写して送る
};
inline FloppyImageReply floppyImageReply(unsigned imageBytes, bool head, bool haveBuffer, bool canAllocate) {
    if (imageBytes == 0) return FloppyImageReply::NOT_FOUND;
    if (head) return FloppyImageReply::HEAD_ONLY;
    if (haveBuffer) return FloppyImageReply::SEND;
    return canAllocate ? FloppyImageReply::ALLOCATE : FloppyImageReply::NO_MEMORY;
}

enum class FloppyImageSave {
    SAVED,         // 書き出した
    ALREADY,       // 同じ名前で同じ大きさのファイルがある (書き出し済み。書かない)
    OTHER_SIZE,    // 同じ名前で大きさの違うファイルかディレクトリがある (書かない)
    CHECK_FAILED,  // path があるかどうかを確かめられなかった (書かない)
    WRITE_FAILED,  // tmp に書けなかった、tmp の古いものを消せなかった、名前を変えられなかった
    VERIFY_FAILED, // 書いたものを読み戻したら CRC-32 が違った (tmp は消した)
    APPEARED,      // 書いている間に path ができた (path はそのまま。tmp は消した)
};

// 書き出したか、書き出し済みか
inline bool floppyImageSaved(FloppyImageSave r) { return r == FloppyImageSave::SAVED || r == FloppyImageSave::ALREADY; }

// data (size バイト、CRC-32 は crc) を path に書き出す。tmp は書いている間の名前 (<path>.new)。
// tmpLeft (nullptr でよい) には、失敗したあとに tmp が残っているかもしれないかを返す (消そうとして消せなかった。
// 書きかけか、読み戻しの合わないファイルが <path>.new として残る。次に書き出すときに、もう一度消そうとする)
template <class Fs>
FloppyImageSave saveFloppyImageFile(Fs& fs, const char* path, const char* tmp, const void* data, unsigned size,
                                    uint32_t crc, bool* tmpLeft = nullptr) {
    bool left = false;
    struct Report {
        bool* out;
        const bool& left;
        ~Report() {
            if (out) *out = left;
        }
    } report{tmpLeft, left};
    unsigned long long bytes = 0;
    bool dir = false;
    FsResult r = fs.stat(path, bytes, dir);
    if (r == FsResult::ERROR) return FloppyImageSave::CHECK_FAILED;
    if (r == FsResult::OK) return !dir && bytes == size ? FloppyImageSave::ALREADY : FloppyImageSave::OTHER_SIZE;
    // 前に途中で止まった書きかけ。消せなければ、その上には書かない
    r = fs.remove(tmp);
    if (r == FsResult::ERROR) {
        left = true;
        return FloppyImageSave::WRITE_FAILED;
    }
    if (fs.write(tmp, data, size) != FsResult::OK) {
        left = fs.remove(tmp) == FsResult::ERROR;
        return FloppyImageSave::WRITE_FAILED;
    }
    uint32_t got = 0;
    if (!fs.crc32(tmp, got) || got != crc) {
        left = fs.remove(tmp) == FsResult::ERROR;
        return FloppyImageSave::VERIFY_FAILED;
    }
    if (fs.rename(tmp, path) != FsResult::OK) {
        // path ができていたのか、ほかの理由かを見分ける。どちらでも path には触らず、tmp を消す
        const FsResult now = fs.stat(path, bytes, dir);
        left = fs.remove(tmp) == FsResult::ERROR;
        return now == FsResult::OK ? FloppyImageSave::APPEARED : FloppyImageSave::WRITE_FAILED;
    }
    return FloppyImageSave::SAVED;
}

} // namespace platform
