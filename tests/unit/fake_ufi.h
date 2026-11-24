// UFI の USB-FDD の偽物 (ufi_disk_test、floppy_restore_test)
#pragma once

#include <stdlib.h>
#include <string.h>

#include "platform/ufi_disk.h"

using namespace platform;

struct FakeUfi : UfiTransport {
    static constexpr size_t BYTES = (size_t)UfiDisk::BLOCKS * 1024;
    uint8_t* disk = (uint8_t*)malloc(BYTES);
    uint8_t* other = (uint8_t*)malloc(BYTES); // 入れ替えたあとのディスク
    UfiGate gate;
    int unit = 0; // Circle の ufd<unit + 1>
    uint32_t blockBytes = 1024, blocks = 1232;
    bool wp = false, modeSense = true, stallCommand = false, dead = false, shortData = false;
    int wpFromWrite = -1;       // この回数目の WRITE(10) から書き込み禁止 (MODE SENSE では分からないドライブ)
    uint8_t key = 0, asc = 0, ascq = 0; // 今のセンス
    uint8_t attention = 0;      // 次のコマンドを 06/xx で断る
    int notReady = 0;           // 残りの回数だけ 02/04 で断る
    bool noDisk = false, badSense = false;
    int corruptWrites = 0;      // 残りの回数だけ、書いた中身が 1 バイト化ける (成功と答える)
    uint32_t badBlock = ~0u;    // 読めず、書けないブロック
    bool unreadable[UfiDisk::BLOCKS] = {}; // 読めないブロック (書けば読めるようになる)
    bool stallData = false;     // 読めないことを、データの転送の STALL で知らせる
    bool tornBad = false;       // 転送が切れたとき、書きかけのブロックが読めなくなる
    int writes = 0, commands = 0, waits = 0, breakAt = -1, changeAt = -1;
    uint32_t tornBlocks = 0;    // 転送が切れたときに書けているブロック
    FakeUfi() {
        for (size_t i = 0; i < BYTES; i++) {
            disk[i] = (uint8_t)(i * 31 >> 5);
            other[i] = (uint8_t)(i * 17 >> 4);
        }
    }
    ~FakeUfi() {
        free(disk);
        free(other);
    }
    void waitMs(unsigned) override { waits++; }
    bool armWrite(uint32_t lba, uint32_t count) override { return gate.arm(unit, lba, count); }
    UfiReply reply(uint8_t k, uint8_t a, uint8_t q) {
        key = k;
        asc = a;
        ascq = q;
        UfiReply r;
        r.phase = stallCommand ? UfiReply::COMMAND : UfiReply::DONE;
        if (!stallCommand) {
            r.asc = a;
            r.ascq = q;
        }
        return r;
    }
    UfiReply done(int bytes) {
        UfiReply r;
        r.phase = UfiReply::DONE;
        r.bytes = shortData && bytes > 1 ? bytes - 1 : bytes;
        return r;
    }
    UfiReply command(const uint8_t cdb[12], void* data, size_t len, bool in) override {
        if (!gate.allow(unit, cdb, len, in)) {
            UfiReply r;
            r.phase = UfiReply::REFUSED;
            return r;
        }
        const int at = commands++;
        if (at == changeAt) {
            uint8_t* x = disk;
            disk = other;
            other = x;
            attention = 0x28;
        }
        if (dead) return UfiReply();
        uint8_t* p = (uint8_t*)data;
        const uint32_t lba = (uint32_t)cdb[2] << 24 | (uint32_t)cdb[3] << 16 | (uint32_t)cdb[4] << 8 | cdb[5];
        const uint32_t count = (uint32_t)cdb[7] << 8 | cdb[8];
        if (cdb[0] == UfiOp::REQUEST_SENSE) {
            memset(p, 0, len);
            p[0] = badSense ? 0 : 0x70;
            p[2] = key;
            p[7] = 10;
            p[12] = asc;
            p[13] = ascq;
            UfiReply r = done((int)len);
            r.asc = asc; // 前のコマンドの ASC が残るドライブ
            r.ascq = ascq;
            key = 0;
            return r;
        }
        if (attention) {
            const uint8_t a = attention;
            attention = 0;
            return reply(6, a, 0);
        }
        if (noDisk) return reply(2, 0x3a, 0);
        if (notReady > 0) {
            notReady--;
            return reply(2, 0x04, 0x01);
        }
        if (at == breakAt) {
            dead = true;
            if (cdb[0] == UfiOp::WRITE10) {
                memcpy(disk + (size_t)lba * 1024, data, (size_t)tornBlocks * 1024);
                if (tornBad) unreadable[lba + tornBlocks] = true;
            }
            return UfiReply();
        }
        switch (cdb[0]) {
        case UfiOp::TEST_UNIT_READY: return done(0);
        case UfiOp::READ_CAPACITY: {
            const uint32_t v[2] = {blocks - 1, blockBytes};
            for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v[i / 4] >> (24 - 8 * (i % 4)));
            return done(8);
        }
        case UfiOp::MODE_SENSE10:
            if (!modeSense) return reply(5, 0x20, 0);
            memset(p, 0, len);
            p[2] = 0x93;
            p[3] = wp ? 0x80 : 0;
            return done((int)len);
        case UfiOp::READ10:
            if (badBlock >= lba && badBlock < lba + count) return reply(3, 0x11, 0);
            for (uint32_t b = lba; b < lba + count; b++)
                if (unreadable[b]) {
                    UfiReply r = reply(3, 0x11, 0);
                    if (stallData) r.phase = UfiReply::COMMAND;
                    return r;
                }
            memcpy(p, disk + (size_t)lba * 1024, len);
            return done((int)len);
        case UfiOp::WRITE10:
            if (wp || (wpFromWrite >= 0 && writes >= wpFromWrite)) return reply(7, 0x27, 0);
            writes++;
            if (badBlock >= lba && badBlock < lba + count) return reply(3, 0x03, 0);
            memcpy(disk + (size_t)lba * 1024, p, len);
            for (uint32_t b = lba; b < lba + count; b++) unreadable[b] = false;
            if (corruptWrites > 0) {
                corruptWrites--;
                disk[(size_t)lba * 1024 + 2100] ^= 1;
            }
            return done((int)len);
        }
        return UfiReply();
    }
};
