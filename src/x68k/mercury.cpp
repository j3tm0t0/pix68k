// まーきゅりーゆにっと。mercury.h を参照
#include "x68k/mercury.h"

namespace x68k {

namespace {

// PCM (48kHz、44.1kHz、32kHz とその半分) のサンプルを、Sound の 62.5kHz のサンプルに直すフィルタの表
// (tools/gen_resample_table.py pcm が作る)。kPcmInterp[p][j] は、出力の位置が標本 i + p / PCM_PHASES のときに、
// 標本 i - 15 + j に掛ける値 (2^14 倍)。どの p でも合計は 2^14
constexpr int PCM_PHASES = 512;
const int16_t kPcmInterp[PCM_PHASES + 1][Mercury::PCM_TAPS] = {
#include "x68k/pcm_interp_table.inc"
};

// YMF288 の約 55.6kHz のサンプルを、Sound の 62.5kHz のサンプルに直す FIR。出力 9 サンプルが入力 8 サンプルにあたるので、
// 出力の位置は入力のサンプルの間の 9 通り (0/9 から 8/9) しかない。その 9 通りについて、カイザー窓 (beta = 8) を掛けた sinc の
// 16 タップを持つ (14 ビットの小数。相ごとに和が 1 になるようにそろえる)。ベアメタルには libm がないので級数で計算する
int16_t kOpnFir[9][Mercury::OPN_TAPS];
bool opnFirReady = false;

double sinPi(double f) { // sin(pi * f)、0 <= f <= 0.5
    const double x = 3.14159265358979323846 * f;
    double sum = 0, term = x;
    for (int n = 1; n < 30; n += 2) {
        sum += term;
        term *= -x * x / ((n + 1) * (n + 2));
    }
    return sum;
}

double besselI0(double z2) { // 0 次の変形ベッセル関数。z2 は引数の 2 乗
    double sum = 1, term = 1;
    for (int k = 1; k < 40; k++) {
        term *= z2 / 4 / ((double)k * k);
        sum += term;
    }
    return sum;
}

void buildOpnFir() {
    if (opnFirReady) return;
    const int half = Mercury::OPN_TAPS / 2;
    const double beta = 8;
    for (int p = 0; p < 9; p++) {
        double h[Mercury::OPN_TAPS], sum = 0;
        for (int k = 0; k < Mercury::OPN_TAPS; k++) {
            // k 個前のサンプルから見た、出力の位置 (入力のサンプルを単位に。-half から half の間)
            const double t = k - half + p / 9.0;
            double a = t < 0 ? -t : t;
            double sinc = 1;
            if (a > 1e-9) {
                const int whole = (int)a;
                double f = a - whole;
                if (f > 0.5) f = 1 - f;
                sinc = sinPi(f) / (3.14159265358979323846 * a);
                if (whole & 1) sinc = -sinc;
            }
            const double u = t / half;
            h[k] = sinc * besselI0(beta * beta * (1 - u * u)) / besselI0(beta * beta);
            sum += h[k];
        }
        int total = 0, biggest = 0;
        for (int k = 0; k < Mercury::OPN_TAPS; k++) {
            const double v = h[k] / sum * 16384;
            kOpnFir[p][k] = (int16_t)(v < 0 ? v - 0.5 : v + 0.5);
            total += kOpnFir[p][k];
            if (kOpnFir[p][k] > kOpnFir[p][biggest]) biggest = k;
        }
        kOpnFir[p][biggest] = (int16_t)(kOpnFir[p][biggest] + 16384 - total); // 丸めの分を足して、和をちょうど 1 にする
    }
    opnFirReady = true;
}

} // namespace

Mercury::Mercury() {
    buildOpnFir();
    reset();
}

// YMF288 の今の出力 (side は 0 が L、1 が R)。まだどちらのチップにも書いていなければ 0
int32_t Mercury::opnOutput(int side) const {
    if (!opnUsed_[0] && !opnUsed_[1]) return 0;
    const int16_t* const h = kOpnFir[opnAcc_ / 8]; // opnAcc_ は 8 の倍数 (64 クロックずつ進め、72 で折り返す)
    const int32_t* const x = opnHist_[side];
    int64_t sum = 0;
    for (int k = 0; k < OPN_TAPS; k++) sum += (int64_t)h[k] * x[(opnPos_ - (uint32_t)k) & (OPN_TAPS - 1)];
    return (int32_t)(sum >> 14);
}

void Mercury::reset() {
    cmd_ = mode_ = high_ = 0;
    leftNext_ = true;
    head_ = count_ = 0;
    acc_ = 0;
    outL_ = outR_ = 0;
    memset(pcmHist_, 0, sizeof pcmHist_);
    pcmPos_ = 0;
    pcmQuiet_ = PCM_TAPS;
    for (int i = 0; i < 2; i++) {
        opn_[i].reset();
        opnBusy_[i] = 0;
        opnUsed_[i] = false;
        for (int k = 0; k < OPN_TAPS; k++) opnHist_[i][k] = 0;
    }
    opnAcc_ = 0;
    opnPos_ = 0;
    vector_ = 0xff;
    irqLine_ = irqLatched_ = false;
}

uint32_t Mercury::rate() const {
    static const uint32_t kRates[4] = {44100, 32000, 44100, 48000};
    const uint32_t hz = kRates[(cmd_ >> 4) & 3];
    return (cmd_ & 0x80) ? hz : hz / 2;
}

uint8_t Mercury::read(uint32_t off, uint32_t pendingClocks4M) const {
    switch (off & 0x1fff) {
    case 0x90: {
        // PCS は DMAC の PCS に出す EXPCL と同じ値にする (推測で決めたこと。mercury.h)
        const bool pcs = expcl(pendingClocks4M);
        return (uint8_t)(0xf0 | (pcs ? 0x08 : 0) | (drq() ? 0 : 0x04) | (mode_ & 3));
    }
    case 0x91: return cmd_;
    case 0xa1: return (uint8_t)((cmd_ & 0xf0) | 0x0f);
    default: break;
    }
    off &= 0x1fff;
    if (!(off & 1)) return 0xff; // YMF288 とベクタは下位バイトだけ
    if (off >= 0xb0 && off < 0xc0) return vector_;
    if (off >= 0xc0 && off < 0xd0) {
        const int chip = (off >> 3) & 1;
        const Opn3& opn = opn_[chip];
        switch ((off >> 1) & 3) {
        case 0: case 2: // ステータス 0 / 1。YMF288 では ADPCM のフラグがないので、どちらも BUSY とタイマーのフラグ
            return (uint8_t)(opn.status() | (opnBusy_[chip] > pendingClocks4M * 2 ? 0x80 : 0));
        case 1: return opn.readData();
        default: return 0xff; // A1 = A0 = 1 の読み出しは何も出ない
        }
    }
    return 0xff;
}

void Mercury::opnWriteAddress(int chip, int port, uint8_t v, uint32_t pendingClocks4M) {
    chip &= 1;
    opn_[chip].writeAddress(port, v);
    const uint32_t busy = opn_[chip].busyClocks(false, port & 1, v);
    opnBusy_[chip] = busy ? pendingClocks4M * 2 + busy : 0;
}

void Mercury::opnWriteData(int chip, uint8_t v, uint32_t pendingClocks4M, int& bank, uint8_t& reg) {
    chip &= 1;
    bank = opn_[chip].bank();
    reg = opn_[chip].address();
    opnBusy_[chip] = pendingClocks4M * 2 + opn_[chip].busyClocks(true, bank, reg);
    opnWriteReg(chip, bank, reg, v);
}

void Mercury::opnWriteReg(int chip, int bank, uint8_t reg, uint8_t v) {
    chip &= 1;
    opn_[chip].writeReg(bank, reg, v);
    opnUsed_[chip] = true;
    if (chip == 0) updateOpnIrq();
}

// マスターの /IRQ が出た変わり目で、割り込みの要求を覚える
void Mercury::updateOpnIrq() {
    const bool line = opn_[0].irq();
    if (line && !irqLine_) irqLatched_ = true;
    irqLine_ = line;
}

void Mercury::advanceOpn(uint32_t clocks4M, bool synth) {
    for (int i = 0; i < 2; i++) {
        if (opnUsed_[i]) opn_[i].advanceTimers(clocks4M);
        opnBusy_[i] = opnBusy_[i] > clocks4M * 2 ? opnBusy_[i] - clocks4M * 2 : 0;
    }
    updateOpnIrq();
    opnAcc_ += clocks4M;
    while (opnAcc_ >= (uint32_t)Opn3::CLOCKS_PER_SAMPLE) {
        opnAcc_ -= Opn3::CLOCKS_PER_SAMPLE;
        if (!synth) continue;
        int32_t out[2] = {0, 0};
        for (int i = 0; i < 2; i++)
            if (opnUsed_[i]) opn_[i].clockSample(out);
        opnPos_ = (opnPos_ + 1) & (OPN_TAPS - 1);
        opnHist_[0][opnPos_] = out[0];
        opnHist_[1][opnPos_] = out[1];
    }
}

void Mercury::copyOpnSynthFrom(const Mercury& synth) {
    for (int i = 0; i < 2; i++) {
        const uint8_t addr = opn_[i].address();
        const int bank = opn_[i].bank();
        opn_[i] = synth.opn_[i];
        opn_[i].writeAddress(bank, addr);
        for (int k = 0; k < OPN_TAPS; k++) opnHist_[i][k] = synth.opnHist_[i][k];
    }
    opnPos_ = synth.opnPos_;
}

void Mercury::writeData(uint16_t w) {
    if (count_ >= FIFO) return; // あふれた分は捨てる (L / R の順も進めない。進めると以後 L と R が入れ替わる)
    if (cmd_ & 0x02) leftNext_ = !leftNext_;
    fifo_[(head_ + count_) % FIFO] = w;
    count_++;
}

// 1 サンプル分を取り出す (ステレオは L と R の 2 語、モノラルは 1 語)。足りなければ、前の値をもう一度出す
void Mercury::take() {
    if (cmd_ & 0x02) {
        if (count_ >= 2) {
            outL_ = (int16_t)fifo_[head_];
            outR_ = (int16_t)fifo_[(head_ + 1) % FIFO];
            head_ = (head_ + 2) % FIFO;
            count_ -= 2;
        }
    } else if (count_ >= 1) {
        outL_ = outR_ = (int16_t)fifo_[head_];
        head_ = (head_ + 1) % FIFO;
        count_--;
    }
    pushPcm();
}

// 今の出力 (outL_、outR_) を、62.5kHz に直すフィルタの履歴に入れる
void Mercury::pushPcm() {
    pcmHist_[0][pcmPos_] = pcmHist_[0][pcmPos_ + PCM_TAPS] = (int16_t)outL_;
    pcmHist_[1][pcmPos_] = pcmHist_[1][pcmPos_ + PCM_TAPS] = (int16_t)outR_;
    pcmPos_ = (pcmPos_ + 1) % PCM_TAPS;
    if (outL_ | outR_) pcmQuiet_ = 0;
    else if (pcmQuiet_ < (uint32_t)PCM_TAPS) pcmQuiet_++;
}

// PCM のサンプル (48kHz など) を、今の時点 (Sound の 62.5kHz のサンプル) の値に直す。前は、取り出した値をそのまま
// 出していた (前の値を保つだけ) ので、PCM の周波数と 62.5kHz のずれが、元の音と関係のない周波数の成分になっていた
// (48kHz の PCM の 5kHz の正弦波で -18dB。#296)。窓をかけた sinc の多相フィルタ (32 タップ、位置は 1/512 サンプル刻み) で、
// PCM の周波数の半分より上にできる写しを落として、間の値を作る。フィルタの分、音は PCM の 16 サンプル遅れる
// (48kHz で約 0.33ms。取り出したサンプルが履歴の真ん中 (16 番目) に来たときに、その値が出る)。出す値は 16 ビットを少し超えることがある (段差のところ)。止めるのは Sound
int32_t Mercury::pcmOutput(int side) const {
    if (pcmQuiet_ >= (uint32_t)PCM_TAPS) return 0;
    // acc_ / 4000000 が、最後に取り出してから進んだ時間 (PCM の 1 サンプルを 1 として)。いちばん近い表の行を使う
    const int16_t* const h = kPcmInterp[((uint64_t)acc_ * PCM_PHASES + 2000000) / 4000000];
    const int16_t* const x = pcmHist_[side] + pcmPos_;
    int64_t sum = 0;
    for (int k = 0; k < PCM_TAPS; k++) sum += (int32_t)h[k] * x[k];
    return (int32_t)((sum + (1 << 13)) >> 14);
}

} // namespace x68k
