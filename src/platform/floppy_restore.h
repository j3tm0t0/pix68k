// XDF のイメージを 1 枚まるごと、実物のディスク (USB-FDD) に書き戻す手順 (#386、experimental)。
// pix68k.cfg の experimental_usb_fdd_write= で許したドライブだけで使う (platform/ufi_gate.h)。
//   1. prepare(): ディスクを開き (UfiDisk)、今の中身を全部読む。読めないセクタ (媒体のエラー) があれば、1 つずつ読み直して
//      読めた分だけを控える (partial())。ほかの失敗ならやめる (書いていない)
//   2. 呼ぶ側が、今の中身を控えて backupDone() を呼び (控えられなければ、そのあとは書けない)、書き込み元と今の中身の CRC を見せて、書いてよいかを尋ねる。
//      partial() なら、控えが欠けたまま書いてよいかを別に尋ね、よければ acceptPartial() を呼ぶ
//   3. write(): 尋ねている間に入れ替わっていないかを、飛び飛びのトラックを読んで確かめ (媒体の変化を知らせないドライブのため)、
//      トラック 0 を 0 で埋め、トラック 1〜153、最後にトラック 0 を書く。途中で止まったディスクが、前の中身や書きかけのまま
//      起動しないようにするため
//   4. verify(): 全部を、書いた順 (トラック 0 は最後) に読み、全バイトを比べる。違えば止まる (書き直しはしない。もう一度最初から行う)
// 読み書きの失敗や媒体の変化があれば、そこで止まり、そのあとは何も送らない (UfiDisk)。
// 入れ替えたディスクの中身が、調べたトラックで前のディスクと同じなら、入れ替えは分からない。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform/floppy_image_save.h"
#include "platform/real_floppy.h"
#include "platform/ufi_disk.h"

namespace platform {

// 進みを知らせる。false を返すと、読む段階 (prepare()) はやめる。書き始めたあとは返り値を見ない
class FloppyRestoreProgress {
public:
    enum Phase { READING, CHECKING, WRITING, VERIFYING };
    virtual ~FloppyRestoreProgress() {}
    virtual bool progress(Phase phase, uint32_t done, uint32_t total) = 0;
};

class FloppyRestore {
public:
    static constexpr uint32_t TRACKS = RealFloppyReader::TRACKS, TRACK_BLOCKS = RealFloppyReader::TRACK_BLOCKS;
    static constexpr size_t TRACK_BYTES = (size_t)TRACK_BLOCKS * RealFloppyReader::BLOCK_BYTES;
    static constexpr size_t IMAGE_BYTES = RealFloppyReader::IMAGE_BYTES;
    static constexpr uint32_t CHECK_STEP = 11; // 入れ替わっていないかを見るトラックの間隔 (14 トラック)

    enum State { IDLE, READY, WRITTEN, VERIFIED, FAILED };
    enum Error {
        NONE,
        CANCELED,
        CANNOT_OPEN, // disk().error() に理由
        PROTECTED,
        READ_ERROR,  // track()。disk().error()
        SWAPPED,     // 読んだときと中身が違う
        WRITE_ERROR, // track()。disk().error()
        MISMATCH,    // 読み戻すと違う。track() と block()
        BAD_STATE,
        UNREADABLE,  // 全部を読めるトラックが 1 つもない (フォーマットされていないなど)
        NO_BACKUP,   // 控えがない
        NOT_ACCEPTED, // 控えが欠けているのに、認めていない
    };
    static constexpr uint32_t BLOCKS = UfiDisk::BLOCKS;

    // old と work は、それぞれ IMAGE_BYTES と TRACK_BYTES の作業用
    bool prepare(UfiTransport* t, uint8_t* old, uint8_t* work, FloppyRestoreProgress* p) {
        if (state_ != IDLE || t == nullptr || old == nullptr || work == nullptr) return fail(BAD_STATE);
        old_ = old;
        work_ = work;
        if (!disk_.open(t)) return fail(CANNOT_OPEN);
        if (disk_.protect() == UfiDisk::WRITE_PROTECTED) return fail(PROTECTED);
        for (uint32_t i = 0; i < TRACKS; i++) {
            track_ = i;
            if (!tell(p, FloppyRestoreProgress::READING, i, TRACKS)) return fail(CANCELED);
            if (disk_.read(i * TRACK_BLOCKS, TRACK_BLOCKS, old_ + i * TRACK_BYTES)) continue;
            // 読めないセクタがある。1 つずつ読む
            if (disk_.error() != UfiDisk::MEDIUM || disk_.stopped()) return fail(READ_ERROR);
            bool bad = false;
            for (uint32_t b = i * TRACK_BLOCKS; b < (i + 1) * TRACK_BLOCKS; b++) {
                if (!tell(p, FloppyRestoreProgress::READING, i, TRACKS)) return fail(CANCELED);
                if (disk_.read(b, 1, old_ + (size_t)b * RealFloppyReader::BLOCK_BYTES)) continue;
                if (disk_.error() != UfiDisk::MEDIUM || disk_.stopped()) return fail(READ_ERROR);
                markBad(b);
                bad = true;
            }
            if (bad) badTracks_++;
        }
        if (badTracks_ == TRACKS) return fail(UNREADABLE);
        oldCrc_ = crc32Of(old_, IMAGE_BYTES);
        state_ = READY;
        return true;
    }
    uint32_t oldCrc() const { return oldCrc_; }
    // 読めないセクタがあり、控えは欠けている (そのセクタには "pix68k: cannot read this sector" を並べてある)
    bool partial() const { return badBlocks_ != 0; }
    uint32_t badBlocks() const { return badBlocks_; }
    uint32_t badTracks() const { return badTracks_; }
    bool badBlock(uint32_t b) const { return b < BLOCKS && (bad_[b / 8] >> (b % 8) & 1) != 0; }
    bool badTrack(uint32_t t) const {
        for (uint32_t b = 0; b < TRACK_BLOCKS; b++)
            if (badBlock(t * TRACK_BLOCKS + b)) return true;
        return false;
    }

    // 今の中身の控えをファイルに残した (saved) か。crc は、残したファイルを読み戻した CRC-32。だめなら、もう書けない
    bool backupDone(bool saved, uint32_t crc) {
        if (state_ != READY) return fail(BAD_STATE);
        if (!saved || crc != oldCrc_) return fail(NO_BACKUP);
        backedUp_ = true;
        return true;
    }
    // 控えが欠けたまま書くことを認める
    void acceptPartial() { partialOk_ = true; }

    // 入れ替わっていないかだけを確かめる (write() の前に呼び、書き込み中の記録を残してから write() を呼ぶ)
    bool checkSame(FloppyRestoreProgress* p) {
        if (state_ != READY) return fail(BAD_STATE);
        if (!backedUp_) return fail(NO_BACKUP);
        if (partial() && !partialOk_) return fail(NOT_ACCEPTED);
        if (!disk_.ready()) return fail(SWAPPED);
        for (uint32_t from = 0; from < TRACKS; from += CHECK_STEP) {
            uint32_t i = from; // その範囲の、全部を読めた最初のトラック
            while (i < TRACKS && i < from + CHECK_STEP && badTrack(i)) i++;
            if (i == TRACKS || i == from + CHECK_STEP) continue;
            track_ = i;
            tell(p, FloppyRestoreProgress::CHECKING, i, TRACKS);
            if (!disk_.read(i * TRACK_BLOCKS, TRACK_BLOCKS, work_)) return fail(READ_ERROR);
            if (memcmp(work_, old_ + i * TRACK_BYTES, TRACK_BYTES) != 0) return fail(SWAPPED);
        }
        checked_ = true;
        return true;
    }

    // image (IMAGE_BYTES) を書く。checkSame() のすぐあとに呼ぶ
    bool write(const uint8_t* image, FloppyRestoreProgress* p) {
        if (state_ != READY || !checked_ || image == nullptr || image == old_) return fail(BAD_STATE);
        image_ = image;
        memset(work_, 0, TRACK_BYTES);
        track_ = 0;
        tell(p, FloppyRestoreProgress::WRITING, 0, TRACKS);
        touched_ = true;
        if (!disk_.write(0, TRACK_BLOCKS, work_)) return writeFailed(true);
        for (uint32_t n = 1; n <= TRACKS; n++) {
            track_ = n % TRACKS; // 1〜153、最後に 0
            tell(p, FloppyRestoreProgress::WRITING, n, TRACKS);
            if (!writeTrack(track_)) return writeFailed(false);
        }
        state_ = WRITTEN;
        return true;
    }

    bool verify(FloppyRestoreProgress* p) {
        if (state_ != WRITTEN) return fail(BAD_STATE);
        for (uint32_t n = 1; n <= TRACKS; n++) {
            // 1〜153、最後に 0 (最後に書いたトラックを、ドライブが溜めたデータから答えないように)
            const uint32_t i = n % TRACKS;
            track_ = i;
            tell(p, FloppyRestoreProgress::VERIFYING, n, TRACKS);
            bool same = false;
            if (!compare(i, same)) return fail(READ_ERROR);
            if (!same) return fail(MISMATCH);
        }
        state_ = VERIFIED;
        return true;
    }

    State state() const { return state_; }
    Error error() const { return error_; }
    // ディスクに書き始めた (失敗していれば、前の中身と新しい中身が混ざっているかもしれない)
    bool touched() const { return touched_; }
    uint32_t track() const { return track_; } // シリンダ x 2 + ヘッド
    uint32_t block() const { return block_; } // MISMATCH のときの、トラックの中のブロック (R - 1)
    const UfiDisk& disk() const { return disk_; }

private:
    static bool tell(FloppyRestoreProgress* p, FloppyRestoreProgress::Phase f, uint32_t done, uint32_t total) {
        return p == nullptr || p->progress(f, done, total);
    }
    bool fail(Error e) {
        state_ = FAILED;
        error_ = e;
        return false;
    }
    bool writeFailed(bool first) {
        // 最初の 1 回が送られなかったか、ドライブが書く前に断ったなら、何も書いていない
        // (NOT_READY は、書いている途中の失敗のこともあるので、書き始めたとみなす)
        if (first) {
            switch (disk_.error()) {
            case UfiDisk::NOT_ALLOWED:
            case UfiDisk::BAD_RANGE:
            case UfiDisk::REJECTED:
            case UfiDisk::PROTECTED:
            case UfiDisk::CHANGED: touched_ = false; break;
            default: break;
            }
        }
        return fail(disk_.error() == UfiDisk::PROTECTED ? PROTECTED : WRITE_ERROR);
    }
    void markBad(uint32_t b) {
        bad_[b / 8] = (uint8_t)(bad_[b / 8] | 1u << (b % 8));
        badBlocks_++;
        static const char kText[] = "pix68k: cannot read this sector\n"; // 32 バイト
        for (size_t k = 0; k < RealFloppyReader::BLOCK_BYTES; k += 32)
            memcpy(old_ + (size_t)b * RealFloppyReader::BLOCK_BYTES + k, kText, 32);
    }
    bool writeTrack(uint32_t i) { return disk_.write(i * TRACK_BLOCKS, TRACK_BLOCKS, image_ + i * TRACK_BYTES); }
    bool compare(uint32_t i, bool& same) {
        if (!disk_.read(i * TRACK_BLOCKS, TRACK_BLOCKS, work_)) return false;
        same = true;
        for (uint32_t b = 0; b < TRACK_BLOCKS && same; b++) {
            const size_t off = (size_t)b * RealFloppyReader::BLOCK_BYTES;
            if (memcmp(work_ + off, image_ + i * TRACK_BYTES + off, RealFloppyReader::BLOCK_BYTES) != 0) {
                same = false;
                block_ = b;
            }
        }
        return true;
    }

    UfiDisk disk_;
    State state_ = IDLE;
    Error error_ = NONE;
    bool touched_ = false, checked_ = false, backedUp_ = false, partialOk_ = false;
    uint8_t bad_[(BLOCKS + 7) / 8] = {};
    uint32_t badBlocks_ = 0, badTracks_ = 0;
    uint8_t* old_ = nullptr;
    uint8_t* work_ = nullptr;
    const uint8_t* image_ = nullptr;
    uint32_t oldCrc_ = 0, track_ = 0, block_ = 0;
};

} // namespace platform
