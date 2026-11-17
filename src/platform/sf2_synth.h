// SoundFont 2 (SF2) の MIDI 音源。TinySoundFont (external/tinysoundfont/tsf.h、MIT) で鳴らす。
// MIDI ボード (x68k/ym3802.h) が送ったバイトを x68k::MidiOut として受け、出力のフレームごとに音を足す。
//
// 受けるメッセージ: ノートオン / オフ、コントロールチェンジ (TinySoundFont が受けるもの: バンクセレクト、ボリューム、
// エクスプレッション、パン、サステイン、オールサウンドオフ、オールノートオフ、リセットオールコントローラ。RPN は下)、
// プログラムチェンジ、ピッチベンド。チャネル 10 (0 から数えて 9) はドラム (バンク 128)。
// RPN (0: ピッチベンドの幅、1: ファインチューン、2: コースチューン) は、TinySoundFont に渡さずにここで扱う。RPN ごとに
// データの MSB と LSB を持ち、データエントリ (6, 38) で選んでいる RPN のものを書き換えて、ピッチベンドの幅 (MSB 半音 +
// LSB セント) とチューニング (コース (MSB - 64) 半音 + ファイン (14 ビット - 8192) / 8192 半音) を決め直す。
// NRPN (98, 99) を選んだあとのデータエントリは捨てる。リセットオールコントローラで RPN の選択とデータを初めに戻す。
// 初めの RPN の選択は、101 と 100 のどちらも 127 (RPN ヌル)。100 だけ、101 だけを送ったときは、送らなかった側が
// 127 のままなので、そのあとのデータエントリは捨てる (実物の GS 音源と同じ。TinySoundFont は送らなかった側を 0 とみなす)。
// (TinySoundFont はデータを RPN の間で 1 つだけ持ち、チューニングを今の値の整数の部分から計算するので、受けた順で
// 結果が変わり、送り直しても同じ状態に戻せない)
// SF2 にないプログラムを選んだときは、TinySoundFont は前の音色のままにするが、ここではバンク 0 のプログラム 0
// (なければ最初の音色) にする (前のプログラムチェンジによらず、受けたバンクとプログラムだけで音色が決まるように)。
// システムエクスクルーシブは GM システムオン、GS リセット、XG システムオンだけを受け、すべてのチャネルを初めの状態に戻す。
// ポリフォニックキープレッシャー、チャネルプレッシャー、ほかのシステムエクスクルーシブは無視する。
//
// 音は TSF_RENDER_EFFECTSAMPLEBLOCK (64) フレームずつまとめて作る。受けたメッセージは、次のまとまりの初めから反映される
// (48kHz で最大 1.3ms 遅れる)。
//
// ベアメタルでは、合成するコア (コア 3) はヒープを使えない。load() でボイスとチャネルを先に確保し、その後は確保しない。
// ボイスが足りないときは、TinySoundFont はリリース (鍵盤を離したあとの減衰) に入ったボイスのうち、いちばん進んだものを
// 止めて使う。リリースに入ったものがなければ、新しい音は鳴らさない。
// load() の中の確保は量を数え、LoadLimit の上限を超えるなら確保せずに読むのをやめる (sf2_synth.cpp)。
// 数学の関数は libm を使わず、このファイルの中のもの (sf2_synth.cpp) を使う (ベアメタルには libm がない。ホストでも同じ
// ものを使い、どちらでも同じ音にする)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "platform/midi_stream.h"
#include "x68k/midi_out.h"

struct tsf;

namespace platform {

// SF2 のファイルを読む口 (ファイルから少しずつ読むため)。read() は読んだバイト数、skip() は進めたら true
class Sf2Source {
public:
    virtual int read(void* out, unsigned n) = 0;
    virtual bool skip(unsigned n) = 0;

protected:
    ~Sf2Source() = default;
};

// load() で確保してよい量。blockBytes は n バイトを確保したときに実際に減るヒープの大きさ (nullptr なら n)
struct LoadLimit {
    uint64_t maxBytes = UINT64_MAX;
    uint64_t (*blockBytes)(size_t) = nullptr;
};

class Sf2Synth : public x68k::MidiOut {
public:
    static constexpr int BLOCK = 64; // TSF_RENDER_EFFECTSAMPLEBLOCK
    static constexpr int DEFAULT_VOICES = 64;

    Sf2Synth() {
        for (Rpn& r : rpn_) r.reset();
    }
    ~Sf2Synth() override;
    Sf2Synth(const Sf2Synth&) = delete;
    Sf2Synth& operator=(const Sf2Synth&) = delete;

    // SF2 のファイルの中身を読む (読み終えたら data は捨ててよい)。rate は出力のサンプリング周波数、
    // maxVoices は同時に鳴らすボイスの数。読めなければ (形が違う、確保が limit を超える) false。
    // 読めなかったときは、読む間に確保したものをすべて解放する
    bool load(const void* data, size_t size, int rate, int maxVoices = DEFAULT_VOICES, const LoadLimit& limit = {});
    // 同じく、src から順に読む (読み飛ばしは skip())。標本は float に直して持つ (ファイルの標本の 2 倍の大きさ)
    bool load(Sf2Source& src, int rate, int maxVoices = DEFAULT_VOICES, const LoadLimit& limit = {});
    // 前の load() で確保した量 (見積もり) と、上限を超えて読むのをやめたか
    uint64_t loadBytes() const { return loadBytes_; }
    bool overLimit() const { return overLimit_; }
    // 試験用: TinySoundFont が確保して、まだ解放していないブロックの数 (すべての Sf2Synth の合計)
    static int liveBlocks();
    // 試験用: チャネルの設定 (TinySoundFont の中の値)
    struct ChannelInfo {
        int presetIndex, bank, wheel;
        float volume, pan, range, tuning;
        int rawBank, sustain;                  // バンクセレクトの状態 (TinySoundFont の値)、サステイン
        int midiVolume, midiExpression, midiPan; // 受けた音量、エクスプレッション、パン (14 ビット)
        int rpnMsb, rpnLsb, nrpn;              // 選んでいる RPN、NRPN を選んでいるか
        int dataMsb[3], dataLsb[3];            // RPN 0-2 のデータ
    };
    ChannelInfo channelInfo(int ch) const;
    bool loaded() const { return f_ != nullptr; }
    // 音量 (1.0 で、ボイス 1 つの最大がおおよそ 16 ビットの半分)
    void setGain(float gain) { gain_ = gain; }
    int presetCount() const;
    int activeVoices() const;

    // ---- x68k::MidiOut ----
    void midiByte(uint8_t b) override;
    void midiReset() override;
    void mixFrame(int32_t& l, int32_t& r) override {
        if (!f_) return;
        if (pos_ >= BLOCK) render();
        l += buf_[2 * pos_];
        r += buf_[2 * pos_ + 1];
        pos_++;
    }

    // 試験用: n フレームの音を作って out (ステレオ、16 ビット) に書く (mixFrame() を n 回呼ぶのと同じ)
    void renderFrames(int16_t* out, int n);

private:
    void render();
    void resetChannels();
    void sysex(const uint8_t* p, int n);
    void program(int ch, int p);
    void control(int ch, int k, int v);
    void applyRpn(int ch, int r);

    // チャネルごとの RPN (上の説明)
    struct Rpn {
        uint8_t selMsb, selLsb, nrpn;
        uint8_t msb[3], lsb[3];
        void reset() {
            selMsb = selLsb = 127;
            nrpn = 0;
            msb[0] = 2;
            msb[1] = msb[2] = 64;
            lsb[0] = lsb[1] = lsb[2] = 0;
        }
    };
    Rpn rpn_[16];

    tsf* f_ = nullptr;
    MidiStream stream_;
    uint64_t loadBytes_ = 0;
    bool overLimit_ = false;
    float gain_ = 1.0f;
    int pos_ = BLOCK;
    float fbuf_[2 * BLOCK] = {};
    int32_t buf_[2 * BLOCK] = {};
};

} // namespace platform
