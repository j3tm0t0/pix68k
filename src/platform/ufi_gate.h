// USB-FDD (UFI) へ送ってよいコマンドの判定 (#386)。Pi 4 / Pi 5 だけで使う。
// 通常のビルドに入るが、既定では閉じている。pix68k.cfg に experimental_usb_fdd_write=usb-fdd:N と書いたときだけ enable(N) を呼び、
// その番号のドライブにだけコマンドを通す。WRITE(10) は、さらに設定メニューで確かめたあと (confirm(true)) でなければ通さない。
// tools/build_baremetal.sh が Circle の写しに pix68k_ufi_gate.h の名前で写し、ドライバの入口 (baremetal/ufd_write.inc) が使う。
// WRITE(10) は、直前に arm() で許した 1 回だけ、ブロックの番号と数が同じときだけ通す。
// ほかに通すのは TEST UNIT READY、REQUEST SENSE、READ CAPACITY、READ(10)、MODE SENSE(10) だけ (フォーマットや自己診断は通さない)
// このファイルは Circle の写しにも置くので、#pragma once では二重の取り込みを防げない
#ifndef PIX68K_UFI_GATE_H
#define PIX68K_UFI_GATE_H

#include <stddef.h>
#include <stdint.h>

namespace platform {

struct UfiOp {
    enum : uint8_t {
        TEST_UNIT_READY = 0x00,
        REQUEST_SENSE = 0x03,
        READ_CAPACITY = 0x25,
        READ10 = 0x28,
        WRITE10 = 0x2a,
        MODE_SENSE10 = 0x5a,
    };
};

class UfiGate {
public:
    static constexpr uint32_t BLOCK_BYTES = 1024, BLOCKS = 1232, MAX_BLOCKS = 8; // X68000 の 2HD、1 トラックまで

    // pix68k.cfg を読んだあとに 1 回だけ呼ぶ。unit が 0〜3 でなければ閉じる
    static void enable(int unit) {
        state().unit = unit >= 0 && unit < 4 ? unit : -1;
        state().confirmed = false;
    }
    static bool enabled(int unit) { return unit >= 0 && state().unit == unit; }
    static int enabledUnit() { return state().unit; }
    // 設定メニューで「書く」と答えてから、書き終わるまで true
    static void confirm(bool on) { state().confirmed = on && state().unit >= 0; }

    // unit は、このドライブの番号 (Circle の ufd<unit + 1>)。次の allow() で、lba から count ブロックの WRITE(10) を 1 回だけ許す
    bool arm(int unit, uint32_t lba, uint32_t count) {
        armed_ = false;
        if (!enabled(unit) || !state().confirmed) return false;
        if (count == 0 || count > MAX_BLOCKS || lba >= BLOCKS || count > BLOCKS - lba) return false;
        lba_ = lba;
        count_ = count;
        armed_ = true;
        return true;
    }
    // コマンド cdb (12 バイト) と len バイトのデータ (in なら装置から受ける) を送ってよいか。許しは、どのコマンドでも消える
    bool allow(int unit, const uint8_t* cdb, size_t len, bool in) {
        const bool armed = armed_;
        armed_ = false;
        if (!enabled(unit)) return false;
        if (cdb == nullptr || cdb[1] != 0) return false; // LUN 0 だけ
        switch (cdb[0]) {
        case UfiOp::TEST_UNIT_READY: return len == 0;
        case UfiOp::REQUEST_SENSE:
        case UfiOp::READ_CAPACITY:
        case UfiOp::READ10:
        case UfiOp::MODE_SENSE10: return in && len > 0;
        case UfiOp::WRITE10: {
            const uint32_t lba = (uint32_t)cdb[2] << 24 | (uint32_t)cdb[3] << 16 | (uint32_t)cdb[4] << 8 | cdb[5];
            const uint32_t count = (uint32_t)cdb[7] << 8 | cdb[8];
            return armed && state().confirmed && !in && lba == lba_ && count == count_ && len == (size_t)count * BLOCK_BYTES;
        }
        default: return false;
        }
    }

private:
    struct State {
        int unit;
        bool confirmed;
    };
    static State& state() {
        static State s = {-1, false};
        return s;
    }
    bool armed_ = false;
    uint32_t lba_ = 0, count_ = 0;
};

} // namespace platform

#endif
