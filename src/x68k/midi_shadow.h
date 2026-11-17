// MIDI ボードから MIDI の音源に送ったバイトから、音源の設定 (チャネルごとのプログラム、コントロールチェンジ、
// ピッチベンド、RPN) と、受け取る側がどこまで読んだか (ランニングステータス、揃っていないデータのバイト) を覚える。
// 音源の設定は X68000 のステートに入らない (音源は音を作るコアにあり、UART の先の実機の音源のこともある) ので、
// 次のときに、覚えた設定を MIDI のバイトとして送り直し、音源を送ったバイトのとおりの状態に戻す (emit())。
// - ステートを読んだとき (読んだステートを保存したときの設定に戻す)
// - 動いている途中で音源をつないだとき (ベアメタル版は SoundFont を X68000 を動かし始めてから読む)
//
// 送り直すもの (emit() の順):
//   1. 最後に受けたリセットのシステムエクスクルーシブ (GM システムオン、GS リセット、XG システムオン)
//   2. チャネルごとに: リセットオールコントローラ (CC 121)、受けたコントロールチェンジ (バンクセレクトを除き、番号の順)、
//      サステイン (受けていなければ 0)、今の音色を選んだときのバンクセレクト、プログラムチェンジ (受けていなければ 0)、
//      今のバンクセレクト (次のプログラムチェンジのためのもの)、RPN 0-2 のデータ (ピッチベンドの幅、ファインチューン、
//      コースチューン。初めの値と違うものだけ)、選んでいる RPN の番号と NRPN を選んでいるか、ピッチベンド
//   3. $F7 (ランニングステータスを消す)、受け取る側の読みかけの状態 (MidiStream::pending())
// 覚えないもの: ノートオン / オフ (鳴っている音は止める)、キープレッシャー、チャネルプレッシャー、リセット以外の
// システムエクスクルーシブ (GS のパートの設定など)、NRPN のデータ、データインクリメント / デクリメント。
// 受けたバイトの扱いは、この PC の SoundFont の音源 (platform/sf2_synth.h) に合わせる。
// - CC 121 で消すもの: 音量 (7, 39)、エクスプレッション (11, 43)、パン (10, 42)、バンク (0, 32)、RPN の選択とデータ
//   (ピッチベンドの幅とチューニングは初めの値)。サステインとピッチベンドは消さない。GM の推奨 (RP-015) とは違う
//   (GM は音量、パン、バンクを消さず、ピッチベンドとペダルを消す) ので、UART の先の実機の音源では、CC 121 のあとの
//   状態が送り直したあとにずれることがある
// - RPN は RPN ごとにデータの MSB と LSB を持つ (音源の多くと同じ)。NRPN を選んでいる間のデータエントリは捨てる
// - バンクセレクトは、受けた順で結果が変わる音源がある (TinySoundFont は MSB だけなら LSB として使い、MSB のあとの LSB で
//   2 つを合わせ、続けての LSB では LSB だけを使う) ので、値ではなく、最後の 2 つのメッセージを覚えて同じ順に送り直す。
//   その 2 つに入っていない MSB / LSB は、最後に受けた値をその前に送る (MSB と LSB を別に覚える音源のため)。
//   2 つに満たないときは、CC 121 か初めの状態 (バンク 0) から受けたものなので、MSB 0、LSB 0 を送ってから送る
// - リセットのシステムエクスクルーシブ (MidiStream::isResetSysex()) は、すべてのチャネルを初めの状態に戻したとみなす
// 合わないことがわかっているもの (音源の仕様による):
// - SoundFont の音源は、SF2 にない音色を選ぶとバンク 0 のプログラム 0 にする (sf2_synth.h)。UART の先の実機の音源で
//   前の音色のままにするものは、送り直したあとに違う音色になることがある
// MT-32 / CM-32L の音源 (MidiOut::MT32。platform/mt32_synth.h) に送り直すときは、次のように変える (emit() の family):
// - リセットのシステムエクスクルーシブ (GM / GS / XG) とバンクセレクトは送らない (MT-32 は受けない)
// - プログラムチェンジは、受けたチャネルだけ送る。MT-32 は、電源を入れたときにパートごとに違う音色を選んでいて、
//   システムエクスクルーシブでパートの音色を書き換える曲もあるので、受けていないチャネルにプログラム 0 を送らない
// - ピッチベンドの幅 (RPN 0) は、最後のプログラムチェンジのあとに RPN 0 のデータエントリを受けたときだけ、その値を
//   (2 でも) 送る。MT-32 の幅の初めの値はパッチの値 (多くは 12) で、プログラムチェンジでパッチの値に戻り、CC 121 では
//   変わらない。この幅は GS の扱いの rpnMsb[0] とは別に覚える (flags の上の 5 ビット。0-24 に収める (Munt と同じ))。
//   RPN 1、2 (チューニング) は送らない (MT-32 は受けない)
// 送り直さないもの (MT-32): システムエクスクルーシブで書いた音色、パッチ、リズムの設定、パートの割り当て、
//   リバーブなど。音源の中に残っているものをそのまま使う (ステートを読んだとき)。途中で音源をつないだとき
//   (ベアメタル版で ROM を読み終えたとき) は、それまでに送ったシステムエクスクルーシブは届かない
// 合わないことがわかっているもの (MT-32):
// - CC 121 は、Munt (実機の MT-32 と同じ) ではモジュレーション、エクスプレッション、ピッチベンド、ホールドペダルを
//   戻し、音量とパンは戻さない。上の CC 121 の扱い (音量とパンも消す) と違うので、CC 121 のあとに音量やパンを
//   送っていない曲では、送り直したあとに前の音量とパンのままになる
// - 受けたプログラムチェンジのあとに、システムエクスクルーシブでそのパートの音色を書き換えた曲では、送り直した
//   プログラムチェンジで書き換える前の音色に戻る
// - Munt は CC 121 で RPN の選択を消さない。上の CC 121 の扱いでは消すので、CC 121 のあとに RPN を選び直さずに
//   データエントリを送る曲では、そのデータエントリを覚えない (送り直したあとの幅が違う)
// - リズムのパート (初めはチャネル 10) では、Munt はプログラムチェンジを受けないので幅が変わらないが、ここでは
//   プログラムチェンジで幅の記録 (RANGE) を消す。幅を送ったあとにプログラムチェンジを送った曲では、送り直したあとの
//   幅が違う (docs/midi.md の例)。リズムのパートのチャネルはシステムエクスクルーシブで変えられるので、ここでは見分けない
// - MT-32 は GM / GS / XG のリセットを受けないが、ここではすべてのチャネルを初めの状態に戻したとみなす。そのあとに
//   プログラムチェンジや幅を送っていないチャネルは、送り直したときにリセットの前の設定に戻らない
// 状態はそのまま書き写せる形にする (ステートは StateIO::raw() で保存する。読んだら sanitize() で取りうる値に収める)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stdint.h>
#include <string.h>

#include "platform/midi_stream.h"
#include "x68k/midi_out.h"

namespace x68k {

class MidiShadow {
public:
    static constexpr int RESET_SYSEX_MAX = 16;

    MidiShadow() { reset(); }

    // 初めの状態 (何も受けていない) に戻す
    void reset() {
        stream_.reset();
        resetLen_ = 0;
        memset(resetSysex_, 0, sizeof resetSysex_); // ステートに入るので、使わない分も決まった値にしておく
        for (Channel& c : ch_) c.clear();
    }

    // ステートから読んだ値を、配列の範囲に収める (書き換えたステートで配列の外を読み書きしないため)
    void sanitize() {
        stream_.sanitize();
        if (resetLen_ < 0 || resetLen_ > RESET_SYSEX_MAX) resetLen_ = 0;
        for (Channel& c : ch_) {
            c.bank.sanitize();
            c.applied.sanitize();
            c.program &= 0x7f;
            c.flags &= PROGRAM | BEND | RANGE | RANGE_MASK;
            if (c.range() > 24) c.setRange(24);
            c.bend &= 0x3fff;
            for (uint8_t& v : c.cc) v &= 0x7f;
            // control() が cc[] に覚えない番号 (バンクセレクト、データエントリ、データインクリメント / デクリメント、
            // NRPN と RPN の選択) は、受けたことにしない。壊れたステートから、どの RPN を書き換えるか分からないデータエントリを
            // 送り直さないため (MT-32 ではピッチベンドの幅が 24 を超えることがある)
            static const uint8_t kNotKept[] = {0, 6, 32, 38, 96, 97, 98, 99, 100, 101};
            for (uint8_t k : kNotKept) c.unsetCc(k);
            c.selMsb &= 0x7f;
            c.selLsb &= 0x7f;
            c.nrpn = c.nrpn ? 1 : 0;
            for (int r = 0; r < 3; r++) {
                c.rpnMsb[r] &= 0x7f;
                c.rpnLsb[r] &= 0x7f;
            }
        }
    }

    // 音源に送ったバイト
    void feed(uint8_t b) {
        switch (stream_.feed(b)) {
        case platform::MidiStream::NONE: return;
        case platform::MidiStream::SYSEX: sysex(); return;
        case platform::MidiStream::CHANNEL: break;
        }
        Channel& c = ch_[stream_.status() & 0x0f];
        const uint8_t d1 = stream_.data1(), d2 = stream_.data2();
        switch (stream_.status() & 0xf0) {
        case 0xb0: control(c, d1, d2); break;
        case 0xc0:
            c.program = d1;
            c.flags |= PROGRAM;
            c.flags &= (uint8_t)~RANGE; // MT-32 はプログラムチェンジでパッチの幅に戻す
            c.applied = c.bank; // この音色を選んだときのバンク
            break;
        case 0xe0:
            c.bend = (uint16_t)(d1 | d2 << 7);
            c.flags |= BEND;
            break;
        default: break;
        }
    }

    // 覚えた設定を MIDI のバイトとして out(uint8_t) に渡す (上の説明の順)。family は送り先の音源の種類
    template <class F> void emit(F out, MidiOut::Family family = MidiOut::GENERAL) const {
        const bool mt32 = family == MidiOut::MT32;
        if (resetLen_ > 0 && !mt32) {
            out(0xf0);
            for (int i = 0; i < resetLen_; i++) out(resetSysex_[i]);
            out(0xf7);
        }
        for (int n = 0; n < 16; n++) {
            const Channel& c = ch_[n];
            const uint8_t cc = (uint8_t)(0xb0 | n);
            auto control = [&](uint8_t num, uint8_t v) {
                out(cc);
                out(num);
                out(v);
            };
            control(121, 0);
            for (int k = 0; k < 120; k++)
                if (k != 0 && k != 32 && k != 64 && c.ccSet(k)) control((uint8_t)k, c.cc[k]);
            control(64, c.ccSet(64) ? c.cc[64] : 0);
            if (!mt32) c.applied.emit(control);
            if (!mt32 || (c.flags & PROGRAM)) {
                out((uint8_t)(0xc0 | n));
                out((c.flags & PROGRAM) ? c.program : 0);
            }
            if (!mt32 && (c.applied.n != 0 || c.bank.n != 0)) {
                if (c.bank.n < 2) { // CC 121 か初めの状態 (バンク 0) から受けたもの (上の説明)
                    control(0, 0);
                    control(32, 0);
                }
                c.bank.emit(control);
            }
            bool rpnSent = false;
            if (mt32 && (c.flags & RANGE)) {
                control(101, 0);
                control(100, 0);
                control(6, c.range());
                rpnSent = true;
            }
            for (int r = 0; r < 3 && !mt32; r++) {
                if (c.rpnMsb[r] == RPN_MSB0[r] && c.rpnLsb[r] == 0) continue;
                control(101, 0);
                control(100, (uint8_t)r);
                control(6, c.rpnMsb[r]);
                control(38, c.rpnLsb[r]);
                rpnSent = true;
            }
            // 選んでいる RPN の番号 (初めは RPN ヌル (127, 127))。NRPN を選んでいたら、NRPN のヌル (127, 127) を選ぶ
            if (rpnSent || c.selMsb != 127 || c.selLsb != 127) {
                control(101, c.selMsb);
                control(100, c.selLsb);
            }
            if (c.nrpn) {
                control(99, 127);
                control(98, 127);
            }
            const uint16_t bend = (c.flags & BEND) ? c.bend : 8192;
            out((uint8_t)(0xe0 | n));
            out((uint8_t)(bend & 0x7f));
            out((uint8_t)(bend >> 7));
        }
        // 最後のピッチベンドのランニングステータスを消してから ($F7 だけでは何も起きない)、読みかけの状態に戻す
        out(0xf7);
        uint8_t p[platform::MidiStream::PENDING_MAX];
        const int n = stream_.pending(p);
        for (int i = 0; i < n; i++) out(p[i]);
    }

    // 試験用
    int program(int n) const { return (ch_[n].flags & PROGRAM) ? ch_[n].program : -1; }
    int control(int n, int k) const { return ch_[n].ccSet(k) ? ch_[n].cc[k] : -1; }

private:
    // flags: PROGRAM、BEND、RANGE と、上の 5 ビット (RANGE_MASK) に MT-32 のピッチベンドの幅
    enum : uint8_t { PROGRAM = 1, BEND = 2, RANGE = 4, RANGE_SHIFT = 3, RANGE_MASK = 0xf8 };
    static constexpr uint8_t RPN_MSB0[3] = {2, 64, 64}; // RPN 0-2 のデータの MSB の初めの値 (LSB は 0)

    // バンクセレクトの最後の 2 つのメッセージと、最後に受けた MSB / LSB の値
    struct Bank {
        uint8_t n;      // msg に入っている数 (0-2)
        uint8_t num[2]; // 0 (MSB) か 32 (LSB)。古い順
        uint8_t val[2];
        uint8_t msb, lsb;
        uint8_t has; // bit0: msb を受けた、bit1: lsb を受けた
        void clear() { memset(this, 0, sizeof *this); }
        void add(uint8_t k, uint8_t v) {
            if (n == 2) {
                num[0] = num[1];
                val[0] = val[1];
                n = 1;
            }
            num[n] = k;
            val[n] = v;
            n++;
            if (k == 0) {
                msb = v;
                has |= 1;
            } else {
                lsb = v;
                has |= 2;
            }
        }
        template <class C> void emit(C& control) const {
            bool msbIn = false, lsbIn = false;
            for (int i = 0; i < n; i++) (num[i] == 0 ? msbIn : lsbIn) = true;
            if ((has & 1) && !msbIn) control(0, msb);
            if ((has & 2) && !lsbIn) control(32, lsb);
            for (int i = 0; i < n; i++) control(num[i], val[i]);
        }
        void sanitize() {
            if (n > 2) n = 0;
            for (int i = 0; i < 2; i++) {
                num[i] = num[i] == 0 ? 0 : 32;
                val[i] &= 0x7f;
            }
            msb &= 0x7f;
            lsb &= 0x7f;
            has &= 3;
        }
    };

    struct Channel {
        uint8_t cc[128];
        uint8_t set[16]; // cc[] を受けたかのビット (バンクセレクトは bank で覚える)
        Bank bank;       // 受けたバンクセレクト
        Bank applied;    // 今の音色を選んだとき (プログラムチェンジ) の bank
        uint8_t program;
        uint8_t flags; // PROGRAM、BEND、RANGE と MT-32 のピッチベンドの幅 (上の 5 ビット)
        uint16_t bend;
        uint8_t selMsb, selLsb; // 選んでいる RPN の番号 (101, 100)
        uint8_t nrpn;           // 1 なら NRPN を選んでいる (0 か 1)
        uint8_t rpnMsb[3];      // RPN 0-2 のデータ
        uint8_t rpnLsb[3];
        uint8_t pad; // 詰め物 (ステートに入る。clear() で 0 にする)
        void clear() {
            memset(this, 0, sizeof *this);
            bend = 8192;
            resetRpn();
        }
        void resetRpn() {
            selMsb = selLsb = 127;
            nrpn = 0;
            for (int r = 0; r < 3; r++) {
                rpnMsb[r] = RPN_MSB0[r];
                rpnLsb[r] = 0;
            }
        }
        uint8_t range() const { return (uint8_t)(flags >> RANGE_SHIFT); }
        void setRange(uint8_t v) { flags = (uint8_t)((flags & ~RANGE_MASK) | (v << RANGE_SHIFT)); }
        bool ccSet(int k) const { return (set[k >> 3] >> (k & 7)) & 1; }
        void setCc(int k, uint8_t v) {
            cc[k] = v;
            set[k >> 3] |= (uint8_t)(1 << (k & 7));
        }
        void unsetCc(int k) { set[k >> 3] &= (uint8_t)~(1 << (k & 7)); }
    };

    void control(Channel& c, uint8_t k, uint8_t v) {
        switch (k) {
        case 0:
        case 32: c.bank.add(k, v); return;
        case 6:
        case 38:
            if (c.nrpn || c.selMsb != 0 || c.selLsb > 2) return; // RPN 0-2 のほか (NRPN、RPN ヌルなど) は捨てる
            (k == 6 ? c.rpnMsb : c.rpnLsb)[c.selLsb] = v;
            if (k == 6 && c.selLsb == 0) { // MT-32 のピッチベンドの幅 (上の説明。CC 121 では消さない)
                c.flags |= RANGE;
                c.setRange(v > 24 ? 24 : v);
            }
            return;
        case 96:
        case 97: return; // データインクリメント / デクリメントは覚えない
        case 98:
        case 99: c.nrpn = 1; return; // NRPN を選んだ (NRPN のデータは覚えない)
        case 100:
            c.selLsb = v;
            c.nrpn = 0;
            return;
        case 101:
            c.selMsb = v;
            c.nrpn = 0;
            return;
        case 121: { // リセットオールコントローラ (音源の動きに合わせる。上の説明)
            static const uint8_t kReset[] = {7, 39, 11, 43, 10, 42};
            for (uint8_t n : kReset) c.unsetCc(n);
            c.bank.clear();
            c.resetRpn();
            return;
        }
        default:
            if (k < 120) c.setCc(k, v); // 120 から先はチャネルモードのメッセージ (設定ではない)
            return;
        }
    }

    void sysex() {
        if (stream_.sysexTruncated()) return;
        const uint8_t* p = stream_.sysex();
        const int n = stream_.sysexLength();
        if (!platform::MidiStream::isResetSysex(p, n)) return;
        for (Channel& c : ch_) c.clear();
        // 送り直すのは初めの RESET_SYSEX_MAX バイト (リセットかどうかはその中で決まる)
        resetLen_ = n < RESET_SYSEX_MAX ? n : RESET_SYSEX_MAX;
        memcpy(resetSysex_, p, (size_t)resetLen_);
    }

    platform::MidiStream stream_;
    Channel ch_[16];
    uint8_t resetSysex_[RESET_SYSEX_MAX];
    int resetLen_;
};

} // namespace x68k
