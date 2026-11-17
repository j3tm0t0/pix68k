// この OPM の実装は ymfm (https://github.com/aaronsgiles/ymfm) をもとにしている。
// 表 (DT2、EG の変化量など) と式 (周波数、エンベロープ、フィードバック、LFO、PMS、ノイズ) は ymfm と同じ。
// ymfm の著作権表示、条件、免責は次のとおり:
//
// BSD 3-Clause License
//
// Copyright (c) 2021, Aaron Giles
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// pix68k としての変更点と、それ以外の部分は GPLv3 (リポジトリの LICENSE)。
//
// OPM (YM2151、$E90001 アドレス、$E90003 データ / 状態)。X68000 ではクロック 4MHz。
//
// 1 サンプルは 64 クロック (4MHz なら 62.5kHz)。clockSample() で 1 サンプルずつ進める。
// タイマ (A / B) と状態のフラグ、割り込み (MFP の GPIP3) は音を作らないときも動かす。
// 音を作るのは clockSample() に出力先を渡したときだけで、8 チャネル x 4 オペレータの FM、
// 8 種類のアルゴリズム、フィードバック、エンベロープ (AR / D1R / D2R / RR / D1L、KS)、DT1 / DT2、MUL、TL、
// LFO (のこぎり波 / 矩形波 / 三角波 / ノイズ) による AM / PM、チャネル 7 のノイズ、L / R の出力を作る。
//
// ymfm (BSD-3-Clause、Copyright (c) 2021, Aaron Giles。全文は上) をもとにした。YM2151 のデータシートも参考にした。
// 正弦波、指数、音程の表は起動時に式から作る (ベアメタルには libm がないため)。DT1 の表はデータシートの値。
// 内側のループは整数だけで計算する。
//
// X68000 では、レジスタ $1B の bit7 (この実装では CT2 と呼ぶ) が ADPCM のクロック (0 = 8MHz、1 = 4MHz)、
// bit6 (CT1) が FDC の強制レディにつながる。IPL ROM の IOCS は、ADPCM の 3.9kHz / 5.2kHz のときだけ bit7 を立て
// ($FF9AA0 付近)、FDC の準備では bit6 を立てて戻す ($FF8B70 付近)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stdint.h>

namespace x68k {

class StateIO;

class Opm {
public:
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)
    static constexpr int CLOCKS_PER_SAMPLE = 64;

    Opm();
    void reset();
    // offset は $E90000 からの位置。奇数番地だけ: 1 = アドレス、3 = データ (読むと状態)
    uint8_t read(uint32_t offset) const;
    void write(uint32_t offset, uint8_t v);
    uint8_t reg(int n) const { return reg_[n & 0xff]; }
    // 割り込みの要求 (タイマのフラグが立っている)。X68000 では MFP の GPIP3 に負論理でつながる
    bool irq() const { return (status_ & 3) != 0; }
    // 今、アドレスとして選んでいるレジスタの番号 (次にデータを書く先)
    uint8_t address() const { return addr_; }
    // タイマが次に割り込みを要求する (状態のフラグを立てる) までのサンプル数 (clockSample() をこの回数だけ呼ぶと立つ)。
    // フラグを立てる設定 ($14 の IRQEN) で動いているタイマだけを見る。もう要求が出ているか、なければ 0
    uint32_t samplesToIrq() const {
        if (status_ & 3) return 0;
        uint32_t n = 0;
        if (runA_ && (reg_[0x14] & 0x04) && timerA_) n = timerA_;
        if (runB_ && (reg_[0x14] & 0x08) && timerB_ && (n == 0 || timerB_ < n)) n = timerB_;
        return n;
    }
    // CT1 / CT2 の出力 ($1B の bit6 / bit7)
    bool ct1() const { return (reg_[0x1b] & 0x40) != 0; }
    bool ct2() const { return (reg_[0x1b] & 0x80) != 0; }

    // クロックが進んだ (書き込みのあとの BUSY の時間を数える)
    void elapse(uint32_t clocks) { busy_ = clocks >= busy_ ? 0 : busy_ - clocks; }
    // 1 サンプル (64 クロック) 進める。out が nullptr でなければ、このサンプルの出力 (L, R) を書く。
    // 1 チャネルの出力は 14 ビット (オペレータ 1 つの最大が +-8191)、out は 8 チャネルの和
    void clockSample(int32_t* out);

    // テスト用: オペレータのエンベロープの減衰量 (0 = 最大音量、1023 = 無音)。ch 0..7、slot 0..3 は M1 M2 C1 C2
    int envelope(int ch, int slot) const { return ch_[ch & 7].op[slot & 3].env; }

private:
    enum EgState : uint8_t { EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };

    struct Op {
        uint32_t phase = 0; // 10.10 の固定小数点 (上位 10 ビットが正弦波の位置)
        uint32_t step = 0;  // 1 サンプルの位相の増分 (PM を含まない)
        int32_t env = 1023; // エンベロープの減衰量 (10 ビット、1 = 0.09375dB)
        int32_t out = 0;    // このサンプルの出力 (変調に使う)
        EgState state = EG_RELEASE;
        bool key = false;
        // レジスタから計算した値
        uint8_t dt1 = 0, mul = 0, tl = 0, ks = 0, ar = 0, d1r = 0, d2r = 0, rr = 0, d1l = 0, dt2 = 0;
        bool amEnable = false;
        uint8_t rate[4] = {}; // 今の KC で決まる実効レート (EG の状態ごと)
        uint8_t pad[3] = {};  // 詰め物 (ステートに入るので 0 にしておく。x68k/state.h。下の pad も同じ)
        int32_t d1lLevel = 0; // D1L を減衰量に直したもの
        int32_t tlLevel = 0;  // TL を減衰量に直したもの
    };
    struct Channel {
        Op op[4]; // M1 M2 C1 C2 (レジスタの順)
        uint8_t kc = 0, kf = 0, fb = 0, con = 0, pms = 0, ams = 0;
        bool left = false, right = false;
        int32_t fbOut[2] = {}; // M1 の前の 2 サンプルの出力 (フィードバック)
        bool pmActive = false;
        bool idle = true; // 4 つのオペレータがすべて鳴り終わっている (エンベロープも出力も計算しない)
        uint8_t pad[2] = {};
    };

    void writeReg(uint8_t r, uint8_t v);
    void keyOn(Op& op);
    void keyOff(Op& op);
    void updateChannel(int c);           // KC / KF / DT / MUL / KS が変わった
    void updateOp(Channel& ch, Op& op, int pmDelta);
    void updateRates(Channel& ch, Op& op);
    void clockTimers();
    void clockLfo();
    void clockNoise();
    void clockEnvelope(Op& op);
    int32_t channelOutput(Channel& ch, int chIndex);

    uint8_t reg_[256] = {};
    uint8_t addr_ = 0;
    uint8_t status_ = 0; // bit1 タイマ B、bit0 タイマ A
    uint8_t pad0_[2] = {};
    uint32_t busy_ = 0;  // BUSY が消えるまでのクロック数

    // タイマ (サンプル単位で数える)
    uint32_t timerA_ = 0, timerB_ = 0; // 次のオーバーフローまでのサンプル数
    bool runA_ = false, runB_ = false;
    bool csmPending_ = false; // CSM: タイマ A のオーバーフローで全スロットをキーオンした (次のサンプルで離す)
    uint8_t pad1_ = 0;

    // LFO
    uint32_t lfoCounter_ = 0;
    uint32_t lfoStep_ = 0;
    uint8_t lfoPhase_ = 0;
    uint8_t lfoNoise_ = 0;
    bool lfoDirty_ = false; // AMD / PMD / 波形を書いたので、AM / PM の値を計算し直す
    uint8_t pad2_ = 0;
    int32_t lfoAm_ = 0; // 0..254
    int32_t lfoPm_ = 0; // -128..127 に PMD を掛けたもの
    int32_t lastPm_ = 0;
    uint8_t amd_ = 0, pmd_ = 0;
    uint8_t pad3_[2] = {};

    // ノイズ
    uint32_t noiseLfsr_ = 1;
    uint32_t noiseCount_ = 0;
    bool noiseBit_ = false;
    uint8_t pad4_[3] = {};

    // エンベロープ (3 サンプルに 1 回)
    uint32_t egCounter_ = 0;
    int egDivider_ = 0;

    Channel ch_[8];
};

} // namespace x68k
