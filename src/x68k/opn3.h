// YMF288 (OPN3-L)。まーきゅりーゆにっと V4 (MK-MU1O) に 2 個載っている FM 音源 (x68k/mercury.h)。
//
// FM 6 音 (4 オペレータ)、SSG 3 音とノイズ、リズム 6 音、タイマー A / B、ステータス、BUSY を作る。
// YM2608 (OPNA) から ADPCM、I/O ポート、CSM、プリスケーラを除いたもので、レジスタの並びは YM2608 と同じ。
//
// もとにした資料 (ほかのエミュレータのソースは読んでいない):
// - YMF288 のデータシート (ヤマハ): レジスタの並び、YM2608B 互換モードと YMF288 モード ($20 の NEW)、
//   デバイス識別コード、BUSY の長さ、分周比 (FM 1/6、SSG 1/8、リズム 1/12 に固定)、SSG と FM の音量の比、
//   SSG のトーンの周期が 7 以下のときの出力
// - YM2608 アプリケーションマニュアル (ヤマハ): F-Number、Multiple、Detune の表 (表 2-6)、キースケール (表 2-8)、
//   Rate = 2R + Rks、LFO の周波数と PMS / AMS の深さ、タイマーの式、$29、$10 のフラグの制御、リズムのレジスタ
// - タイマーの 1 つの長さ (8MHz でタイマー A が 18 マイクロ秒、B が 288 マイクロ秒): マニュアルの式を YM2203 と同じ形として読み、
//   PMD (KAJA さん) の説明書のテンポの式で確かめた (opn3.cpp の advanceTimers() の前の説明)
// - 対数の正弦波の表と指数の表の作り方: Matthew Gambrell、Olli Niemitalo「OPLx decapsulated」(2008) にある
//   ヤマハの FM 音源の ROM の式
// - エンベロープの刻み (3 サンプルに 1 回、レートごとの変化量の並び、アタックの式)、フィードバック、
//   SSG 型エンベロープの動き: Nemesis さんが SpritesMind のフォーラムに書いた YM2612 の解析 (OPN 系で共通)
//
// 資料から決まらず、推測で決めたこと:
// - LFO の PM は、マニュアルの深さ (セント) になる正弦波を 32 段で作る (実チップのビット単位の表とは合わせていない)
// - LFO の AM は三角波 (0 から 11.8dB)。位相は PM と合わせていない
// - リズム 1 音の最大の音量は、FM の 1 スロットの最大と同じにした
// - アルゴリズムの中で、あるスロットが前のサンプルの値を使う実チップの順序は作っていない (どのスロットも今のサンプルの値で変調する)
// - BUSY の間の書き込みも受け付ける
// - $20 の STBY (スタンバイモード) は値を覚えるだけで、音もレジスタの読み書きも止めない
//
// 1 サンプルはマスタークロック 144 個 (8MHz なら約 55.6kHz)。ここでは時間を 4MHz のクロック (Sound と同じ) で数えるので、
// 1 サンプルは 72 クロック。タイマーとステータスは音を作らないときも動かす (advanceTimers())。
// リズムの音 (チップの中の ROM のデータ) は持っていない。使う人が用意したデータを setRhythm() で渡す。なければリズムは鳴らない。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stdint.h>

namespace x68k {

class Opn3 {
public:
    static constexpr int CLOCKS_PER_SAMPLE = 72; // 4MHz のクロックで数えた 1 サンプル (マスタークロック 8MHz の 144 個)
    static constexpr int RHYTHMS = 6;            // BD、SD、TOP (シンバル)、HH、TOM、RIM ($10 の bit0 から、$18-$1D の順)

    // リズムの音 (16 ビット、モノラル)。pcm が nullptr か length が 0 の音は鳴らない。
    // 一度渡したら、その中身と指す先は変えないこと (音のコアが読み続ける)
    struct RhythmSet {
        const int16_t* pcm[RHYTHMS];
        uint32_t length[RHYTHMS]; // サンプル数
        uint32_t rate[RHYTHMS];   // サンプリング周波数 (Hz)
    };
    // すべての Opn3 が使うリズムの音を決める (nullptr で外す)。Machine を動かし始める前に呼ぶ
    static void setRhythm(const RhythmSet* set);
    static const RhythmSet* rhythm();

    Opn3();
    void reset();
    // ステートから読んだ中身が、取りうる範囲にあるか (表の添字やシフトの数に使う値、bool のバイト。state.cpp が呼ぶ)
    bool stateOk() const;

    // ---- CPU から見えるもの (書き手の側だけが使う) ----
    // アドレスの書き込み (port は 0 か 1。A1)。あとのデータの書き込みは、どちらのデータのポートに書いても、ここで選んだレジスタに入る
    void writeAddress(int port, uint8_t v) {
        addr_ = v;
        bank_ = (uint8_t)(port & 1);
    }
    uint8_t address() const { return addr_; }
    int bank() const { return bank_; }
    // ステータス 0 (port 0) / ステータス 1 (port 1)。bit7 の BUSY は入っていない (呼ぶ側が busy() で足す)
    uint8_t status() const { return (uint8_t)(flags_ & ~mask_ & 3); }
    // データの読み出し (選んでいるレジスタ)
    uint8_t readData() const;
    // 書いたあとの BUSY の長さ (マスタークロックの数)。アドレスを書いたあとは data = false
    uint32_t busyClocks(bool data, int bank, uint8_t reg) const;
    // 割り込みの要求 (/IRQ)。タイマーのフラグが立っていて、$29 で許可されている
    bool irq() const { return (flags_ & ~mask_ & irqEnable_ & 3) != 0; }
    // タイマーが次に /IRQ を出すまでの 4MHz のクロック数。もう出ているか、出る見込みがなければ 0
    uint32_t clocksToIrq() const;

    // ---- 書き手と読み手の両方が使うもの ----
    // レジスタに書く (bank は 0 か 1)
    void writeReg(int bank, uint8_t reg, uint8_t v);
    // タイマーを 4MHz のクロックで進める
    void advanceTimers(uint32_t clocks4M);
    // 1 サンプル進めて、出力 (L, R) を out に足す。FM の 1 スロットの最大が +-8168
    void clockSample(int32_t* out);
    // 今は音が出ていない (FM もリズムも鳴り終わり、SSG の音量がすべて 0)。このときも clockSample() は呼び続けること:
    // LFO とエンベロープの刻み、SSG の矩形波とノイズとエンベロープのカウンタは、音が出ていない間も進む (試験用)
    bool silent() const { return fmIdle_ && !rhythmOn_ && !ssgOn_; }

    // テスト用
    uint8_t reg(int bank, int n) const { return reg_[bank & 1][n & 0xff]; }
    int envelope(int ch, int slot) const { return ch_[ch % 6].op[slot & 3].env; }
    bool newMode() const { return (reg_[0][0x20] & 0x02) != 0; }

private:
    enum EgState : uint8_t { EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };

    struct Op {
        uint32_t phase = 0; // 20 ビット (上位 10 ビットが正弦波の位置)
        uint32_t step = 0;  // 1 サンプルの位相の増分 (PM を含む)
        int32_t env = 1023; // エンベロープの減衰量 (10 ビット、1 = 0.09375dB)
        int32_t slLevel = 0, tlLevel = 0;
        EgState state = EG_RELEASE;
        bool key = false;
        bool amEnable = false;
        bool ssgInv = false; // SSG 型エンベロープで、出力を裏返している
        uint8_t dt = 0, mul = 0, tl = 0, ks = 0, ar = 0, dr = 0, sr = 0, rr = 0, sl = 0, ssg = 0;
        uint8_t rate[4] = {}; // 今のキーコードで決まる実効レート (EG の状態ごと)
        uint8_t pad[2] = {};  // 詰め物 (ステートに入るので 0 にしておく。下の pad も同じ)
    };
    struct Channel {
        Op op[4]; // S1 S2 S3 S4
        uint16_t fnum = 0;
        uint8_t block = 0, fb = 0, alg = 0, pms = 0, ams = 0, latch = 0; // latch: $A4 に書いて、まだ $A0 を書いていない値
        bool left = true, right = true;
        bool idle = true; // 4 つのオペレータがすべて鳴り終わっている (エンベロープも出力も計算しない)
        bool ssgEg = false; // SSG 型エンベロープを使うオペレータがある
        int32_t fbOut[2] = {}; // S1 の前の 2 サンプルの出力 (フィードバック)
    };
    struct Rhythm {
        uint32_t pos = 0;  // 再生している位置 (サンプル)
        uint32_t frac = 0; // その端数 (16 ビット)
        uint32_t step = 0; // 1 サンプルで進む量 (16.16)
        bool on = false;
        bool left = false, right = false;
        uint8_t level = 0; // IL (5 ビット)
    };

    void writeFm(int bank, uint8_t reg, uint8_t v);
    void writeSsg(uint8_t reg, uint8_t v);
    void writeRhythm(uint8_t reg, uint8_t v);
    void keyOn(Channel& ch, Op& op);
    void keyOff(Op& op);
    void updateChannel(int c); // F-Number / Block / DT / MUL / KS / PM が変わった
    void updateOp(Op& op, uint32_t fnum, uint32_t block, int pms);
    void clockLfo();
    void clockEnvelope(Op& op);
    void clockSsgEg(Op& op);
    int32_t channelOutput(Channel& ch);
    int32_t ssgOutput();
    void ssgAdvanceSilent();
    uint32_t ssgEnvelope();
    void rhythmOutput(int32_t* out);
    void updateSsgOn();
    void updateFlagsIdle();

    // ここから下のメンバは、Mercury ごとステートに丸ごと入る (state.cpp の Mercury::state())。足したり並べ替えたりしたら、
    // ステートの版 (state.h の STATE_VERSION) を上げ、古い版の読み方を足すこと。メンバの間に詰め物ができないようにすること
    uint8_t reg_[2][256] = {};
    uint8_t addr_ = 0, bank_ = 0;
    uint8_t flags_ = 0;       // bit1 タイマー B、bit0 タイマー A
    uint8_t mask_ = 0;        // $110 のマスク (bit1 B、bit0 A)
    uint8_t irqEnable_ = 3;   // $29 の EN_TB、EN_TA
    bool sixCh_ = false;      // $29 の SCH
    bool ch3Special_ = false; // $27 の bit6: チャネル 3 のスロットごとに F-Number を決める
    bool fmIdle_ = true;      // 6 チャネルがすべて idle

    // タイマー (4MHz のクロックで数える)
    uint32_t timerA_ = 0, timerB_ = 0; // 次にあふれるまでのクロック数 (止まっていれば 0)。A は 72 x (1024 - NA)、B は 1152 x (256 - NB)

    // LFO
    uint32_t lfoCount_ = 0; // 次の段までのサンプル数
    uint8_t lfoStep_ = 0;   // 0..127
    bool lfoOn_ = false;
    uint8_t lfoAm_ = 0;     // 0..126
    bool pmDirty_ = false;  // PM のかかったチャネルの増分を計算し直す

    // エンベロープ (3 サンプルに 1 回)
    uint32_t egCounter_ = 0;
    int32_t egDivider_ = 0;

    Channel ch_[6];
    // チャネル 3 のスロットごとの F-Number ($A8-$AA、$AC-$AE)。添字は S1 S2 S3
    uint16_t fnum3_[3] = {};
    uint8_t block3_[3] = {};
    uint8_t latch3_[3] = {};

    // SSG (時間は マスタークロック / 8 の刻みで数える。1 サンプルは 18 刻み)
    uint32_t toneCount_[3] = {}; // 次に矩形波が反転するまでの刻み
    uint32_t noiseCount_ = 0;
    uint32_t noiseLfsr_ = 1;
    uint32_t envCount_ = 0;
    uint8_t toneOut_[3] = {};
    uint8_t noiseOut_ = 0;
    uint8_t envPos_ = 0;   // 周期の中の位置 (0..31)
    bool envAttack_ = false;
    bool envHold_ = false;
    uint8_t envFixed_ = 0; // 止まったあとの音量 (0..31)
    bool ssgOn_ = false;   // 音量が 0 でないチャネルがある
    bool rhythmOn_ = false;
    uint8_t rhythmTotal_ = 0; // RTL (6 ビット)
    uint8_t pad0_ = 0;

    Rhythm rhythm_[RHYTHMS];
};

} // namespace x68k
