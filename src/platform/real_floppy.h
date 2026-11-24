// 実物のフロッピーディスクドライブ (USB-FDD など) から、ディスクの全体を読んでイメージ (XDF) にする (#286)。
// 読むだけで、書く処理は持たない (RealFloppySource に書く関数がなく、読んだイメージは書き込み禁止でドライブに入れる)。
//
// ドライブごとの違いは RealFloppySource に閉じ込める。今あるのは、論理ブロックの番号で読む USB-FDD (UFI) の 2 つ:
//   - ホスト版: Linux のブロックデバイス (/dev/sdX) を O_RDONLY で開く (host/main.cpp)
//   - ベアメタル版: Circle の ufd1、ufd2、… (baremetal/machine_run.cpp)
// シリンダ・ヘッド・セクタで読むドライブ (FDC 互換の「ふろっぴU君」など) を足すときは、blockAt() の並び
// (トラック = シリンダ x 2 + ヘッド、その中でセクタ 1〜8 の順。XDF と同じ) で read() を実装すればよい。
//
// 読める形式は X68000 の 2HD (1024 バイト/セクタ、8 セクタ/トラック、77 シリンダ、両面 = 1232 ブロック) だけ。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace platform {

// pix68k.cfg の fd0= / fd1= に書く、実物のドライブの名前 (ファイル名には使えない ':' を入れる)。
//   usb-fdd:    1 台目の USB-FDD (usb-fdd:0 と同じ)
//   usb-fdd:N   N は 0 から数えた番号 (0〜REAL_FLOPPY_UNITS - 1)。usb-fdd:0 が 1 台目、usb-fdd:1 が 2 台目。
//               ベアメタル版では Circle の ufd<N + 1>。Circle は、ディスクが入っていて大きさを読めたドライブにだけ、
//               読めた順に ufd1、ufd2、… と番号を付ける (ディスクのないドライブは数に入らない)
constexpr const char* REAL_FLOPPY_NAME = "usb-fdd:";
constexpr int REAL_FLOPPY_UNITS = 4;

// name が実物のドライブの名前なら、その番号 (0〜REAL_FLOPPY_UNITS - 1)。そうでなければ -1 (大文字と小文字は区別しない)
inline int realFloppyUnit(const char* name) {
    if (name == nullptr) return -1;
    for (const char* want = REAL_FLOPPY_NAME; *want != 0; name++, want++) {
        char c = *name;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != *want) return -1;
    }
    if (name[0] == 0) return 0;
    if (name[0] >= '0' && name[0] < '0' + REAL_FLOPPY_UNITS && name[1] == 0) return name[0] - '0';
    return -1;
}

// experimental_usb_fdd_write= の値。空か off なら -1、usb-fdd:0〜usb-fdd:3 ならその番号、ほかは -2 (番号のない usb-fdd: も受けない)
inline int usbFddWriteUnit(const char* value) {
    if (value == nullptr || value[0] == 0) return -1;
    if ((value[0] | 0x20) == 'o' && (value[1] | 0x20) == 'f' && (value[2] | 0x20) == 'f' && value[3] == 0) return -1;
    const int unit = realFloppyUnit(value);
    const char* e = value;
    while (*e) e++;
    return unit >= 0 && e[-1] != ':' ? unit : -2;
}

inline bool isRealFloppyName(const char* name) { return realFloppyUnit(name) >= 0; }

// 実物のドライブから読んだディスクを、イメージのファイルに書き出すときの名前 (#287)。中身の CRC-32 (zlib の crc32() と
// 同じ値) から "FD-602F7858.XDF" の形にする。同じディスクは同じ名前になるので、書き出し済みかどうかが名前で分かり、
// 違うディスクを上書きすることもない。out は REAL_FLOPPY_IMAGE_NAME_SIZE 以上
// partial: 読めないセクタのある、欠けた控え (#386)。"FD-602F7858-PARTIAL.XDF"
constexpr size_t REAL_FLOPPY_IMAGE_NAME_SIZE = 24;
inline void realFloppyImageName(uint32_t crc, char* out, bool partial = false) {
    static const char kHex[] = "0123456789ABCDEF";
    char* p = out;
    *p++ = 'F';
    *p++ = 'D';
    *p++ = '-';
    for (int shift = 28; shift >= 0; shift -= 4) *p++ = kHex[(crc >> shift) & 15];
    for (const char* e = partial ? "-PARTIAL.XDF" : ".XDF"; *e != 0; e++) *p++ = *e;
    *p = 0;
}

// 実物のドライブ。読むだけ
class RealFloppySource {
public:
    virtual ~RealFloppySource() {}
    // 入っているディスクの、ブロックの大きさ (バイト) とブロックの数。ドライブがない、ディスクが入っていないなら false
    virtual bool geometry(uint32_t& blockBytes, uint32_t& blocks) = 0;
    // ブロック lba から count 個を dst に読む。1 つでも読めなければ false
    virtual bool read(uint32_t lba, uint32_t count, uint8_t* dst) = 0;
};

class RealFloppyReader {
public:
    // X68000 の 2HD
    static constexpr uint32_t BLOCK_BYTES = 1024;
    static constexpr uint32_t TRACK_BLOCKS = 8;
    static constexpr uint32_t TRACKS = 77 * 2;
    static constexpr uint32_t BLOCKS = TRACKS * TRACK_BLOCKS;      // 1232
    static constexpr size_t IMAGE_BYTES = (size_t)BLOCKS * BLOCK_BYTES; // 1,261,568
    // トラックをまとめて読めなかったときに、ブロックを 1 つずつ読み直す回数
    static constexpr int RETRIES = 3;

    enum Status { IDLE, READING, DONE, FAILED };
    enum Error {
        NONE,
        NO_DISK,     // geometry() が false (ドライブがない、ディスクが入っていない)
        BAD_FORMAT,  // X68000 の 2HD ではない (blockBytes() と blocks() に、ドライブの答えが入っている)
        SMALL_BUFFER,
        READ_ERROR,  // badBlock() が読めなかった
    };

    // トラック track (シリンダ x 2 + ヘッド) のセクタ番号 r (1〜8) の、ブロックの番号
    static constexpr uint32_t blockAt(uint32_t track, uint32_t r) { return track * TRACK_BLOCKS + (r - 1); }

    // ドライブがあり、読める形式のディスクが入っているかだけを確かめる (バッファを用意する前に呼べる)。
    // だめなら false (error() が NO_DISK か BAD_FORMAT)。よければ true で、status() は IDLE のまま
    bool check(RealFloppySource* src) {
        src_ = src;
        buf_ = nullptr;
        track_ = 0;
        badBlock_ = 0;
        blockBytes_ = blocks_ = 0;
        if (src == nullptr || !src->geometry(blockBytes_, blocks_)) return fail(NO_DISK);
        if (blockBytes_ != BLOCK_BYTES || blocks_ != BLOCKS) return fail(BAD_FORMAT);
        status_ = IDLE;
        error_ = NONE;
        return true;
    }

    // 読み始める。buf は IMAGE_BYTES 以上。読めそうになければ false (error() に理由)
    bool begin(RealFloppySource* src, uint8_t* buf, size_t bufBytes) {
        if (!check(src)) return false;
        if (buf == nullptr || bufBytes < IMAGE_BYTES) return fail(SMALL_BUFFER);
        buf_ = buf;
        status_ = READING;
        return true;
    }

    // 1 トラック読む。まだ続きがあれば true。終わった (DONE) か失敗した (FAILED) ら false
    bool step() {
        if (status_ != READING) return false;
        const uint32_t first = track_ * TRACK_BLOCKS;
        uint8_t* dst = buf_ + (size_t)first * BLOCK_BYTES;
        if (!src_->read(first, TRACK_BLOCKS, dst)) {
            // どのブロックが読めないのかを知るために、1 つずつ読み直す
            for (uint32_t i = 0; i < TRACK_BLOCKS; i++) {
                bool ok = false;
                for (int t = 0; t < RETRIES && !ok; t++) ok = src_->read(first + i, 1, dst + (size_t)i * BLOCK_BYTES);
                if (!ok) {
                    badBlock_ = first + i;
                    return fail(READ_ERROR);
                }
            }
        }
        if (++track_ == TRACKS) {
            status_ = DONE;
            return false;
        }
        return true;
    }

    // 最後まで読む
    bool readAll() {
        while (step()) {}
        return status_ == DONE;
    }

    Status status() const { return status_; }
    Error error() const { return error_; }
    uint32_t track() const { return track_; } // 読み終えたトラックの数
    uint32_t blockBytes() const { return blockBytes_; }
    uint32_t blocks() const { return blocks_; }
    // READ_ERROR のとき、読めなかったブロックと、そのシリンダ、ヘッド、セクタ番号
    uint32_t badBlock() const { return badBlock_; }
    uint32_t badCylinder() const { return badBlock_ / (TRACK_BLOCKS * 2); }
    uint32_t badHead() const { return badBlock_ / TRACK_BLOCKS % 2; }
    uint32_t badSector() const { return badBlock_ % TRACK_BLOCKS + 1; }

private:
    bool fail(Error e) {
        status_ = FAILED;
        error_ = e;
        return false;
    }

    RealFloppySource* src_ = nullptr;
    uint8_t* buf_ = nullptr;
    Status status_ = IDLE;
    Error error_ = NONE;
    uint32_t track_ = 0;
    uint32_t badBlock_ = 0;
    uint32_t blockBytes_ = 0, blocks_ = 0;
};

} // namespace platform
