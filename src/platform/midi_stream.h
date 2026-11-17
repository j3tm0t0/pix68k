// MIDI のバイト列をメッセージに分ける。音源 (sf2_synth.h) が、MIDI ボードから 1 バイトずつ受け取って使う。
//
// - チャネルメッセージ ($80-$EF): データのバイトが揃ったら CHANNEL を返す。ランニングステータス (同じステータスの
//   2 つ目からステータスのバイトを省いたもの) も受ける
// - システムエクスクルーシブ ($F0 ... $F7): $F7 か、次のステータスのバイトで終わり、SYSEX を返す。
//   SYSEX_MAX バイトを超えた分は捨て、sysexTruncated() が true になる。$F7 なしに次の $F0 が来たら、前のものを
//   SYSEX で返し (中身はその次の feed() まで読める)、次のバイトから新しいものとして読む
// - システムコモン ($F1-$F6): データのバイトを読み飛ばす。ランニングステータスは消える
// - リアルタイム ($F8-$FF): メッセージの途中でも、何もしないで読み飛ばす (ランニングステータスは消さない)
// - ステータスのない、宙に浮いたデータのバイトは捨てる
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stdint.h>

namespace platform {

class MidiStream {
public:
    static constexpr int SYSEX_MAX = 64;
    enum Result { NONE, CHANNEL, SYSEX };

    void reset() {
        status_ = 0;
        need_ = have_ = 0;
        skip_ = 0;
        restart_ = 0;
        inSysex_ = 0;
        sysexLen_ = 0;
        truncated_ = 0;
    }

    Result feed(uint8_t b) {
        if (b >= 0xf8) return NONE; // リアルタイム
        if (restart_) { // 前の feed() で、終わっていないシステムエクスクルーシブのあとに $F0 が来た
            restart_ = 0;
            inSysex_ = 1;
            sysexLen_ = 0;
            truncated_ = 0;
        }
        if (b & 0x80) {
            Result r = NONE;
            if (inSysex_) { // 次のステータスのバイトでシステムエクスクルーシブが終わる
                inSysex_ = 0;
                r = SYSEX;
            }
            if (b == 0xf7) {
                status_ = 0;
                return r;
            }
            if (b == 0xf0) {
                status_ = 0;
                skip_ = 0;
                if (r == SYSEX) { // 前のものの中身を返してから、新しいものを読み始める
                    restart_ = 1;
                    return r;
                }
                inSysex_ = 1;
                sysexLen_ = 0;
                truncated_ = 0;
                return r;
            }
            if (b >= 0xf0) { // システムコモン
                status_ = 0;
                skip_ = (b == 0xf2) ? 2 : (b == 0xf1 || b == 0xf3) ? 1 : 0;
                return r;
            }
            status_ = b;
            need_ = dataBytes(b);
            have_ = 0;
            skip_ = 0;
            return r;
        }
        // データのバイト
        if (inSysex_) {
            if (sysexLen_ < SYSEX_MAX) sysex_[sysexLen_++] = b;
            else truncated_ = 1;
            return NONE;
        }
        if (skip_) {
            skip_--;
            return NONE;
        }
        if (status_ == 0) return NONE;
        data_[have_++] = b;
        if (have_ < need_) return NONE;
        have_ = 0; // 次はランニングステータス
        return CHANNEL;
    }

    // CHANNEL を返したあとに読む
    uint8_t status() const { return status_; }
    uint8_t data1() const { return data_[0]; }
    uint8_t data2() const { return need_ > 1 ? data_[1] : 0; }
    // SYSEX を返したあとに読む ($F0 と $F7 を除いた中身)
    const uint8_t* sysex() const { return sysex_; }
    int sysexLength() const { return sysexLen_; }
    bool sysexTruncated() const { return truncated_ != 0; }

    // 外から読んだ値 (ステート) を、配列の範囲と取りうる値に収める
    void sanitize() {
        if (status_ != 0 && (status_ < 0x80 || status_ >= 0xf0)) status_ = 0;
        need_ = status_ ? dataBytes(status_) : 0;
        if (have_ < 0 || have_ >= need_) have_ = 0;
        if (skip_ < 0 || skip_ > 2) skip_ = 0;
        if (sysexLen_ < 0 || sysexLen_ > SYSEX_MAX) sysexLen_ = 0;
        // bool のかわりに uint8_t で持つ (ステートの 0/1 でない値を bool として読まないため)
        inSysex_ = inSysex_ ? 1 : 0;
        truncated_ = truncated_ ? 1 : 0;
        restart_ = restart_ ? 1 : 0;
        if (restart_) inSysex_ = 0;
        if (!inSysex_) truncated_ = 0;
        if (truncated_ && sysexLen_ < SYSEX_MAX) truncated_ = 0;
        for (uint8_t& v : data_) v &= 0x7f;
        for (uint8_t& v : sysex_) v &= 0x7f;
    }

    // 新しく作った MidiStream に、ここで返すバイトを順に渡すと、この MidiStream と同じところまで読んだ状態になる
    // (ランニングステータスと、揃っていないデータのバイト、終わっていないシステムエクスクルーシブ)。返すのはバイトの数。
    // SYSEX_MAX バイトを超えて捨てているシステムエクスクルーシブは、1 バイト足して同じく超えた状態にする。
    // システムコモンの読み飛ばしの途中なら何も返さない (どちらでも、続くデータのバイトは捨てられる)。
    // out は PENDING_MAX バイト
    static constexpr int PENDING_MAX = 2 + SYSEX_MAX;
    int pending(uint8_t* out) const {
        int n = 0;
        if (restart_) {
            out[n++] = 0xf0;
            return n;
        }
        if (inSysex_) {
            out[n++] = 0xf0;
            for (int i = 0; i < sysexLen_; i++) out[n++] = sysex_[i];
            if (truncated_) out[n++] = 0;
            return n;
        }
        if (status_ == 0) return 0;
        out[n++] = status_;
        for (int i = 0; i < have_; i++) out[n++] = data_[i];
        return n;
    }

    // システムエクスクルーシブの中身 ($F0 と $F7 を除く) が、すべてのチャネルを初めに戻すリセットか。音源 (sf2_synth.h) と、
    // 送った設定を覚える側 (x68k/midi_shadow.h) で同じものを使う。
    // GM システムオン: 7E dd 09 01、GS リセット: 41 dd 42 12 40 00 7F 00 cs、XG システムオン: 43 1n 4C 00 00 7E 00
    // (このあとに続くバイトは見ない)
    static bool isResetSysex(const uint8_t* p, int n) {
        const bool gm = n >= 4 && p[0] == 0x7e && p[2] == 0x09 && p[3] == 0x01;
        const bool gs = n >= 8 && p[0] == 0x41 && p[2] == 0x42 && p[3] == 0x12 && p[4] == 0x40 && p[5] == 0x00 &&
                        p[6] == 0x7f && p[7] == 0x00;
        const bool xg = n >= 7 && p[0] == 0x43 && (p[1] & 0xf0) == 0x10 && p[2] == 0x4c && p[3] == 0x00 &&
                        p[4] == 0x00 && p[5] == 0x7e && p[6] == 0x00;
        return gm || gs || xg;
    }

    static int dataBytes(uint8_t status) {
        const uint8_t hi = status & 0xf0;
        return (hi == 0xc0 || hi == 0xd0) ? 1 : 2;
    }

private:
    // pad0_ から pad2_ は詰め物。この並びのままステートに入る (x68k/midi_shadow.h) ので、0 にしておく
    uint8_t status_ = 0;
    [[maybe_unused]] uint8_t pad0_[3] = {};
    int need_ = 0, have_ = 0;
    int skip_ = 0;
    uint8_t data_[2] = {};
    uint8_t inSysex_ = 0; // 0 か 1
    uint8_t sysex_[SYSEX_MAX] = {};
    [[maybe_unused]] uint8_t pad1_ = 0;
    int sysexLen_ = 0;
    uint8_t truncated_ = 0; // 0 か 1
    uint8_t restart_ = 0;   // 0 か 1
    [[maybe_unused]] uint8_t pad2_[2] = {};
};

} // namespace platform
