// USB-FDD (UFI) の X68000 の 2HD のディスクを、書き込みのために読み書きする (#386)。実機の USB には触らず、UfiTransport を通す
// (ベアメタル版はPi 4 / Pi 5 の CUSBFloppyDiskDevice::Pix68kCommand()、試験では偽物)。
// open() のあとで、媒体が変わった、抜かれた、媒体のエラー (書く前の読み出しは除く)、USB の転送が失敗した、のどれかを見たら、そこから先は何も送らない
// (stopped()。同じオブジェクトでは戻せない)。書き直しはしない。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform/real_floppy.h"
#include "platform/ufi_gate.h"

namespace platform {

struct UfiReply {
    enum Phase {
        DONE,    // コマンドが終わった (asc を見る)
        REFUSED, // 送っていない
        COMMAND, // コマンドを送る段階で断られた (次に REQUEST SENSE を送ってよい)
        BROKEN,  // データか状態の転送が失敗した (時間切れ、抜去など)。これから先は送らない
    } phase = BROKEN;
    uint8_t asc = 0, ascq = 0; // 割り込みで届いた 2 バイト (割り込みのない装置は使わない)
    int bytes = 0;             // データの転送で届いた数
};

class UfiTransport {
public:
    virtual ~UfiTransport() {}
    virtual UfiReply command(const uint8_t cdb[12], void* data, size_t len, bool in) = 0;
    // 次の command() で、この範囲の WRITE(10) を 1 回だけ通す (UfiGate::arm())
    virtual bool armWrite(uint32_t lba, uint32_t count) = 0;
    virtual void waitMs(unsigned ms) = 0;
};

class UfiDisk final : public RealFloppySource {
public:
    enum Error {
        NONE,
        NOT_OPEN,
        BAD_RANGE,
        NOT_READY, // ディスクがない、回転が始まらない
        BAD_FORMAT,
        PROTECTED,
        MEDIUM,   // ディスクのその場所を読み書きできない
        CHANGED,  // 媒体が変わったか、装置がリセットされた
        REJECTED, // コマンドを断られた
        HARDWARE, // ほかのセンス
        BROKEN,   // USB の転送の失敗
        NOT_ALLOWED, // 書くことを許していない (設定、確認、ドライブの番号、つないだときの形式)。何も送っていない
    };
    enum Protect { UNKNOWN, WRITABLE, WRITE_PROTECTED };

    static constexpr uint32_t BLOCK_BYTES = UfiGate::BLOCK_BYTES, BLOCKS = UfiGate::BLOCKS;
    static constexpr int OPEN_TRIES = 12;         // 回り出すまで (Y-E DATA の USB-FDU で約 2.7 秒) を十分に待つ
    static constexpr unsigned OPEN_WAIT_MS = 500;

    // 準備ができるまで待ち、X68000 の 2HD かを確かめ、書き込み禁止を調べる。ここまでに見た「媒体が変わった」「電源が入った」
    // (つないだあとの最初のコマンドに付く) は読み捨てる。だめなら false。呼べるのは 1 回だけ
    bool open(UfiTransport* t) {
        if (t_ != nullptr) return false; // 1 回だけ (止まったものを、開き直して使わないように)
        if (t == nullptr) return fail(NOT_OPEN);
        t_ = t;
        bool ready = false;
        for (int i = 0; i < OPEN_TRIES && !ready; i++) {
            uint8_t cdb[12] = {UfiOp::TEST_UNIT_READY};
            ready = run(cdb, nullptr, 0, true);
            if (ready) break;
            if (error_ != CHANGED && error_ != NOT_READY) return false;
            stopped_ = false; // 開く間だけ、読み捨てて続ける
            if (error_ == NOT_READY) {
                if (asc_ == 0x3a) return false; // ディスクが入っていない
                t_->waitMs(OPEN_WAIT_MS);
            }
        }
        if (!ready) return fail(NOT_READY);
        uint8_t cap[8] = {};
        uint8_t cdb[12] = {UfiOp::READ_CAPACITY};
        if (!run(cdb, cap, sizeof cap, true)) return false;
        blocks_ = be32(cap) + 1;
        blockBytes_ = be32(cap + 4);
        if (blockBytes_ != BLOCK_BYTES || blocks_ != BLOCKS) return fail(BAD_FORMAT);
        if (!readProtect()) return false;
        error_ = NONE;
        open_ = true;
        return true;
    }

    bool geometry(uint32_t& blockBytes, uint32_t& blocks) override {
        if (!usable()) return false;
        blockBytes = blockBytes_;
        blocks = blocks_;
        return true;
    }
    bool read(uint32_t lba, uint32_t count, uint8_t* dst) override {
        if (!usable()) return false;
        if (!inRange(lba, count) || dst == nullptr) return fail(BAD_RANGE);
        uint8_t cdb[12];
        makeRw(cdb, UfiOp::READ10, lba, count);
        return run(cdb, dst, (size_t)count * BLOCK_BYTES, true);
    }
    // src の count ブロック (1 トラックまで) を lba から書く。false のとき、その範囲のどこまで書けたかは分からない
    // (error() が PROTECTED、CHANGED、BAD_RANGE、NOT_ALLOWED のときは、ドライブの答えでは書いていない)
    bool write(uint32_t lba, uint32_t count, const uint8_t* src) {
        if (!usable()) return false;
        if (!inRange(lba, count) || src == nullptr) return fail(BAD_RANGE);
        if (protect_ == WRITE_PROTECTED) return fail(PROTECTED);
        writing_ = true;
        uint8_t cdb[12];
        makeRw(cdb, UfiOp::WRITE10, lba, count);
        if (!t_->armWrite(lba, count)) return fail(NOT_ALLOWED);
        return run(cdb, const_cast<uint8_t*>(src), (size_t)count * BLOCK_BYTES, false);
    }
    // 媒体が変わっていないか、準備ができているかを尋ねる
    bool ready() {
        if (!usable()) return false;
        uint8_t cdb[12] = {UfiOp::TEST_UNIT_READY};
        return run(cdb, nullptr, 0, true);
    }

    Error error() const { return error_; }
    Protect protect() const { return protect_; }
    bool stopped() const { return stopped_; }
    uint8_t senseKey() const { return key_; }
    uint8_t asc() const { return asc_; }
    uint8_t ascq() const { return ascq_; }
    uint32_t blockBytes() const { return blockBytes_; } // BAD_FORMAT のときの、ドライブの答え
    uint32_t blocks() const { return blocks_; }

private:
    static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
    static void makeRw(uint8_t* cdb, uint8_t op, uint32_t lba, uint32_t count) {
        memset(cdb, 0, 12);
        cdb[0] = op;
        cdb[2] = (uint8_t)(lba >> 24);
        cdb[3] = (uint8_t)(lba >> 16);
        cdb[4] = (uint8_t)(lba >> 8);
        cdb[5] = (uint8_t)lba;
        cdb[7] = (uint8_t)(count >> 8);
        cdb[8] = (uint8_t)count;
    }
    static bool inRange(uint32_t lba, uint32_t count) {
        return count > 0 && count <= UfiGate::MAX_BLOCKS && lba < BLOCKS && count <= BLOCKS - lba;
    }
    bool usable() {
        if (!open_) return fail(error_ == NONE ? NOT_OPEN : error_);
        return !stopped_; // error_ は止まった理由のまま
    }
    bool fail(Error e) {
        error_ = e;
        return false;
    }
    bool stop(Error e) {
        stopped_ = true;
        return fail(e);
    }
    // MODE SENSE(10) の頭の 8 バイト (3 バイト目の最上位が書き込み禁止)。知らないドライブなら UNKNOWN
    bool readProtect() {
        uint8_t head[8] = {};
        uint8_t cdb[12] = {UfiOp::MODE_SENSE10};
        cdb[2] = 0x3f;
        cdb[8] = sizeof head;
        if (run(cdb, head, sizeof head, true)) {
            protect_ = head[3] & 0x80 ? WRITE_PROTECTED : WRITABLE;
            return true;
        }
        return error_ == REJECTED;
    }
    // 1 つのコマンド。データは len バイトちょうどでなければ失敗
    bool run(const uint8_t* cdb, void* data, size_t len, bool in) {
        key_ = asc_ = ascq_ = 0;
        const UfiReply r = t_->command(cdb, data, len, in);
        switch (r.phase) {
        case UfiReply::DONE:
            if (r.asc == 0 && r.ascq == 0) {
                if (r.bytes != (int)len) return stop(BROKEN);
                error_ = NONE;
                return true;
            }
            break;
        case UfiReply::REFUSED: return fail(REJECTED);
        case UfiReply::COMMAND: break;
        default: return stop(BROKEN);
        }
        uint8_t sense[18] = {};
        uint8_t sc[12] = {UfiOp::REQUEST_SENSE};
        sc[4] = sizeof sense;
        const UfiReply s = t_->command(sc, sense, sizeof sense, true);
        // REQUEST SENSE の割り込みの 2 バイトは、前のコマンドの ASC が残るので見ない
        if (s.phase != UfiReply::DONE || s.bytes != (int)sizeof sense || (sense[0] & 0x7f) != 0x70) return stop(BROKEN);
        key_ = sense[2] & 0x0f;
        asc_ = sense[12];
        ascq_ = sense[13];
        switch (key_) {
        case 1: // ドライブが自分で読み直すなどして成功した
            if (r.phase == UfiReply::DONE && r.bytes == (int)len) {
                error_ = NONE;
                return true;
            }
            return stop(HARDWARE);
        case 2: return stop(NOT_READY);
        // 書き直すには、データの転送路を戻す手順 (HALT の解除) が要り、確かめていないので、ここで止める
        // 書く前の読み出しなら、読めるところを読むために続ける (データの転送路は CUSBFloppyDiskDevice::Pix68kCommand() が戻す)
        case 3: return writing_ ? stop(MEDIUM) : fail(MEDIUM);
        case 5: return fail(REJECTED);
        case 6: return stop(CHANGED);
        case 7:
            protect_ = WRITE_PROTECTED;
            return fail(PROTECTED);
        default: return stop(HARDWARE);
        }
    }

    UfiTransport* t_ = nullptr;
    bool open_ = false, stopped_ = false, writing_ = false;
    Error error_ = NONE;
    Protect protect_ = UNKNOWN;
    uint8_t key_ = 0, asc_ = 0, ascq_ = 0;
    uint32_t blockBytes_ = 0, blocks_ = 0;
};

} // namespace platform
