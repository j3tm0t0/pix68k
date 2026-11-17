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
#include "x68k/opm.h"

namespace x68k {

namespace {

// ---------------------------------------------------------------- 表
// 起動時に一度だけ式から作る。ベアメタルには libm がないので、sin / log2 / exp2 は級数で計算する
// (表を作るときだけ double を使う)

uint16_t kLogSin[256]; // -log2(sin) を 1/256 オクターブ単位で (正弦波の 1/4 周期、256 点)
uint16_t kPow2[256];   // 2^(-i/256) * 8191 (減衰量から振幅へ)
uint32_t kPhaseStep[768]; // ブロック 7 の位相の増分 (半音を 64 に分けた 12 半音分)
bool tablesReady = false;

const double kPi = 3.14159265358979323846;
const double kLn2 = 0.69314718055994530942;

// e^x (|x| が小さいとき)
double dexpSmall(double x) {
    double sum = 1, term = 1;
    for (int n = 1; n < 30; n++) {
        term *= x / n;
        sum += term;
    }
    return sum;
}

// 2^x
double dexp2(double x) {
    int i = (int)x;
    if ((double)i > x) i--; // 切り捨て
    double r = dexpSmall((x - i) * kLn2);
    for (; i > 0; i--) r *= 2;
    for (; i < 0; i++) r /= 2;
    return r;
}

// log2(y)、y > 0
double dlog2(double y) {
    int e = 0;
    while (y < 0.5) {
        y *= 2;
        e--;
    }
    while (y >= 1) {
        y /= 2;
        e++;
    }
    // y は [0.5, 1)。ln(y) = 2 atanh((y - 1) / (y + 1))
    double z = (y - 1) / (y + 1), z2 = z * z, term = z, sum = 0;
    for (int n = 1; n < 60; n += 2) {
        sum += term / n;
        term *= z2;
    }
    return e + 2 * sum / kLn2;
}

// sin(x)、0 <= x <= pi/2
double dsin(double x) {
    double sum = 0, term = x, x2 = x * x;
    for (int n = 1; n < 40; n += 2) {
        sum += term;
        term *= -x2 / ((n + 1) * (n + 2));
    }
    return sum;
}

int roundToInt(double v) { return (int)(v + 0.5); }

void buildTables() {
    if (tablesReady) return;
    for (int i = 0; i < 256; i++) {
        double s = dsin((2 * i + 1) * kPi / 1024);
        kLogSin[i] = (uint16_t)roundToInt(-dlog2(s) * 256);
        kPow2[i] = (uint16_t)roundToInt(dexp2(-i / 256.0) * 8191);
    }
    // KC = $4A (A、オクターブ 4)、KF = 0 は、クロック 3.579545MHz で 440Hz。
    // 1 サンプルは 64 クロックなので、増分 (20 ビットで 1 周) は 440 * 64 * 2^20 / 3579545。ブロック 7 はその 8 倍
    const double a4 = 440.0 * 64 * 1048576 / 3579545 * 8;
    for (int e = 0; e < 768; e++) kPhaseStep[e] = (uint32_t)roundToInt(a4 * dexp2((e - 512) / 768.0));
    tablesReady = true;
}

// DT1 による増分の補正。キーコード (KC の上位 5 ビット) と DT1 の下位 2 ビットで決まる (データシートの表)
const uint8_t kDetune[32][4] = {
    {0, 0, 1, 2},  {0, 0, 1, 2},  {0, 0, 1, 2},   {0, 0, 1, 2},   {0, 1, 2, 2},   {0, 1, 2, 3},   {0, 1, 2, 3},
    {0, 1, 2, 3},  {0, 1, 2, 4},  {0, 1, 3, 4},   {0, 1, 3, 4},   {0, 1, 3, 5},   {0, 2, 4, 5},   {0, 2, 4, 6},
    {0, 2, 4, 6},  {0, 2, 5, 7},  {0, 2, 5, 8},   {0, 3, 6, 8},   {0, 3, 6, 9},   {0, 3, 7, 10},  {0, 4, 8, 11},
    {0, 4, 8, 12}, {0, 4, 9, 13}, {0, 5, 10, 14}, {0, 5, 11, 16}, {0, 6, 12, 17}, {0, 6, 13, 19}, {0, 7, 14, 20},
    {0, 8, 16, 22}, {0, 8, 16, 22}, {0, 8, 16, 22}, {0, 8, 16, 22},
};

// DT2: 音程を 0、+600、+781、+950 セント上げる (半音の 1/64 単位)
const int32_t kDetune2[4] = {0, (600 * 64 + 50) / 100, (781 * 64 + 50) / 100, (950 * 64 + 50) / 100};

// エンベロープの 1 回の変化量 (rate: 実効レート 0..63、i: EG の刻みの番号の下位 3 ビット)。
// ymfm の s_increment_table と同じ値を、規則から計算する:
//   レート 0-1: 変化しない
//   レート 2-47: 0 か 1。8 回のうち 1 になる回がレートの下位 2 ビットで 4、5、6、7 回 (レート 2-7 は例外的に 4 か 6 回)
//   レート 48-59: 基本の値 2^(レート/4 - 12) と、その 2 倍が、レートの下位 2 ビットで決まる割合で混ざる
//   レート 60-63: いつも 8
int32_t egIncrement(int rate, uint32_t i) {
    static const uint8_t low[4][8] = {
        {0, 1, 0, 1, 0, 1, 0, 1}, {0, 1, 0, 1, 1, 1, 0, 1}, {0, 1, 1, 1, 0, 1, 1, 1}, {0, 1, 1, 1, 1, 1, 1, 1}};
    static const uint8_t high[4][8] = {
        {0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 1, 0, 0, 0, 1}, {0, 1, 0, 1, 0, 1, 0, 1}, {0, 1, 1, 1, 0, 1, 1, 1}};
    if (rate < 2) return 0;
    if (rate < 8) return low[rate >= 6 ? 2 : 0][i]; // 2-5 は 4 回、6-7 は 6 回
    if (rate < 48) return low[rate & 3][i];
    if (rate < 60) return (1 << ((rate >> 2) - 12)) << high[rate & 3][i];
    return 8;
}

const int32_t ENV_MAX = 1023;

// 減衰量 (1/256 オクターブ単位) から振幅 (最大 8191) へ
inline int32_t attenuationToVolume(uint32_t a) {
    if ((a >> 8) > 12) return 0;
    return kPow2[a & 0xff] >> (a >> 8);
}

// オペレータの出力: 位相 (10 ビット) に変調を足し、減衰量 att (10 ビット) を掛ける。14 ビットの符号付き
inline int32_t opOutput(uint32_t phase, int32_t mod, int32_t att) {
    if (att >= ENV_MAX) return 0;
    uint32_t p = ((phase >> 10) + (uint32_t)mod) & 1023;
    uint32_t idx = (p & 0x100) ? (~p & 0xff) : (p & 0xff);
    int32_t v = attenuationToVolume(kLogSin[idx] + ((uint32_t)att << 2));
    return (p & 0x200) ? -v : v;
}

} // namespace

Opm::Opm() {
    buildTables();
    reset();
}

void Opm::reset() {
    for (int i = 0; i < 256; i++) reg_[i] = 0;
    addr_ = 0;
    status_ = 0;
    busy_ = 0;
    timerA_ = timerB_ = 0;
    runA_ = runB_ = false;
    csmPending_ = false;
    lfoCounter_ = 0;
    lfoStep_ = 16;
    lfoPhase_ = 0;
    lfoNoise_ = 0;
    lfoDirty_ = false;
    lfoAm_ = lfoPm_ = lastPm_ = 0;
    amd_ = pmd_ = 0;
    noiseLfsr_ = 1;
    noiseCount_ = 0;
    noiseBit_ = false;
    egCounter_ = 0;
    egDivider_ = 0;
    for (int c = 0; c < 8; c++) {
        ch_[c] = Channel();
        updateChannel(c);
    }
}

uint8_t Opm::read(uint32_t offset) const {
    if ((offset & 3) == 3) return (uint8_t)(status_ | (busy_ ? 0x80 : 0));
    return 0xff;
}

void Opm::write(uint32_t offset, uint8_t v) {
    if ((offset & 3) == 1) {
        addr_ = v;
    } else if ((offset & 3) == 3) {
        writeReg(addr_, v);
        busy_ = CLOCKS_PER_SAMPLE;
    }
}

void Opm::keyOn(Op& op) {
    if (op.key) return;
    op.key = true;
    op.phase = 0;
    op.state = EG_ATTACK;
    if (op.rate[EG_ATTACK] >= 62) op.env = 0;
}

void Opm::keyOff(Op& op) {
    if (!op.key) return;
    op.key = false;
    op.state = EG_RELEASE;
}

void Opm::writeReg(uint8_t r, uint8_t v) {
    reg_[r] = v;
    if (r < 0x20) {
        switch (r) {
        case 0x01: // TEST: bit1 で LFO を止めて 0 に戻す
            if (v & 0x02) lfoCounter_ = 0;
            break;
        case 0x08: { // キーオン: bit3 M1、bit4 C1、bit5 M2、bit6 C2
            Channel& ch = ch_[v & 7];
            static const int slotOfBit[4] = {0, 2, 1, 3};
            for (int b = 0; b < 4; b++) {
                Op& op = ch.op[slotOfBit[b]];
                if (v & (0x08 << b)) {
                    keyOn(op);
                    ch.idle = false;
                }
                else keyOff(op);
            }
            break;
        }
        case 0x14: // タイマの制御
            if (v & 0x10) status_ &= (uint8_t)~1;
            if (v & 0x20) status_ &= (uint8_t)~2;
            if ((v & 0x01) && !runA_) timerA_ = 1024 - (((uint32_t)reg_[0x10] << 2) | (reg_[0x11] & 3));
            if ((v & 0x02) && !runB_) timerB_ = 16 * (256 - (uint32_t)reg_[0x12]);
            runA_ = (v & 0x01) != 0;
            runB_ = (v & 0x02) != 0;
            break;
        case 0x18: // LFRQ: 下位 4 ビットが仮数、上位 4 ビットが指数
            lfoStep_ = (16u + (v & 15)) << (v >> 4);
            break;
        case 0x19:
            if (v & 0x80) pmd_ = v & 0x7f;
            else amd_ = v & 0x7f;
            lfoDirty_ = true; // 次のサンプルで AM / PM の値を計算し直す (ノイズの値は取り直さない)
            break;
        case 0x1b:
            lfoDirty_ = true;
            break;
        }
        return;
    }
    if (r < 0x40) {
        int c = r & 7;
        Channel& ch = ch_[c];
        switch (r & 0x38) {
        case 0x20:
            ch.right = (v & 0x80) != 0;
            ch.left = (v & 0x40) != 0;
            ch.fb = (v >> 3) & 7;
            ch.con = v & 7;
            break;
        case 0x28: ch.kc = v & 0x7f; updateChannel(c); break;
        case 0x30: ch.kf = v >> 2; updateChannel(c); break;
        case 0x38:
            ch.pms = (v >> 4) & 7;
            ch.ams = v & 3;
            ch.pmActive = ch.pms != 0;
            updateChannel(c);
            break;
        }
        return;
    }
    // オペレータのレジスタ: 下位 5 ビットがスロット (チャネル + 8 x {M1, M2, C1, C2})
    int slot = r & 0x1f;
    Channel& ch = ch_[slot & 7];
    Op& op = ch.op[slot >> 3];
    switch (r & 0xe0) {
    case 0x40:
        op.dt1 = (v >> 4) & 7;
        op.mul = v & 15;
        updateOp(ch, op, 0);
        break;
    case 0x60:
        op.tl = v & 0x7f;
        op.tlLevel = op.tl << 3;
        break;
    case 0x80:
        op.ks = v >> 6;
        op.ar = v & 0x1f;
        break;
    case 0xa0:
        op.amEnable = (v & 0x80) != 0;
        op.d1r = v & 0x1f;
        break;
    case 0xc0:
        op.dt2 = v >> 6;
        op.d2r = v & 0x1f;
        updateOp(ch, op, 0);
        break;
    case 0xe0:
        op.d1l = v >> 4;
        op.rr = v & 15;
        op.d1lLevel = (op.d1l == 15 ? 31 : op.d1l) << 5;
        break;
    }
    updateRates(ch, op);
    // PM がかかっているチャネルは、次のサンプルで位相の増分を計算し直す
    if (ch.pmActive) lastPm_ = lfoPm_ + 1;
}

void Opm::updateChannel(int c) {
    Channel& ch = ch_[c];
    for (Op& op : ch.op) {
        updateOp(ch, op, 0);
        updateRates(ch, op);
    }
    if (ch.pmActive) lastPm_ = lfoPm_ + 1;
}

// 実効レート = レート x 2 (RR は x 4 + 2) + キーコード >> (3 - KS)。レートが 0 なら 0
void Opm::updateRates(Channel& ch, Op& op) {
    int ksr = (ch.kc >> 2) >> (3 - op.ks);
    auto eff = [ksr](int raw) -> uint8_t {
        if (raw == 0) return 0;
        int r = raw + ksr;
        return (uint8_t)(r > 63 ? 63 : r);
    };
    op.rate[EG_ATTACK] = eff(op.ar * 2);
    op.rate[EG_DECAY] = eff(op.d1r * 2);
    op.rate[EG_SUSTAIN] = eff(op.d2r * 2);
    op.rate[EG_RELEASE] = eff(op.rr * 4 + 2);
}

// 位相の増分: KC (オクターブ 3 ビット + 音名 4 ビット)、KF (半音の 1/64)、DT2、PM で音程を決め、DT1 と MUL を掛ける
void Opm::updateOp(Channel& ch, Op& op, int pmDelta) {
    int block = (ch.kc >> 4) & 7;
    int code = ch.kc & 15;
    // 音名は 16 個の値に 12 半音が飛び飛びに並ぶので、3/4 にして詰める (3 と 4 は同じ音になる)
    int adj = code - (code >> 2);
    int32_t eff = ((adj << 6) | ch.kf) + kDetune2[op.dt2] + pmDelta;
    while (eff >= 768) {
        eff -= 768;
        block++;
    }
    while (eff < 0) {
        eff += 768;
        block--;
    }
    if (block > 7) {
        block = 7;
        eff = 767;
    } else if (block < 0) {
        block = 0;
        eff = 0;
    }
    int32_t step = (int32_t)(kPhaseStep[eff] >> (7 - block));
    int32_t dt = kDetune[(ch.kc >> 2) & 31][op.dt1 & 3];
    step += (op.dt1 & 4) ? -dt : dt;
    uint32_t s = (uint32_t)step & 0x1ffff;
    op.step = (s * (op.mul ? op.mul * 2u : 1u)) >> 1;
}

// ---------------------------------------------------------------- 1 サンプル

void Opm::clockTimers() {
    if (csmPending_) {
        // CSM のキーオンは 1 サンプルだけ: レジスタでキーオンしていないスロットは離す
        csmPending_ = false;
        for (Channel& ch : ch_)
            for (Op& op : ch.op)
                if (!op.key && op.state != EG_RELEASE) op.state = EG_RELEASE;
    }
    if (runA_ && --timerA_ == 0) {
        timerA_ = 1024 - (((uint32_t)reg_[0x10] << 2) | (reg_[0x11] & 3));
        if (reg_[0x14] & 0x04) status_ |= 1;
        if (reg_[0x14] & 0x80) {
            // CSM: 全スロットをキーオンする
            for (Channel& ch : ch_)
                for (Op& op : ch.op)
                    if (!op.key) {
                        op.phase = 0;
                        op.state = EG_ATTACK;
                        ch.idle = false;
                        if (op.rate[EG_ATTACK] >= 62) op.env = 0;
                    }
            csmPending_ = true;
        }
    }
    if (runB_ && --timerB_ == 0) {
        timerB_ = 16 * (256 - (uint32_t)reg_[0x12]);
        if (reg_[0x14] & 0x08) status_ |= 2;
    }
}

// LFO: 30 ビットのカウンタに LFRQ で決まる値を足し、上位 8 ビットを波形の位置にする
// (LFRQ = $FF で 1 周 1057 サンプル = 59Hz (4MHz)、0 でおよそ 1000 秒)
void Opm::clockLfo() {
    if (reg_[0x01] & 0x02) {
        lfoCounter_ = 0;
    } else {
        lfoCounter_ += lfoStep_;
    }
    uint8_t ph = (uint8_t)(lfoCounter_ >> 22);
    if (ph != lfoPhase_) {
        lfoPhase_ = ph;
        lfoNoise_ = (uint8_t)(noiseLfsr_ >> 17); // LFSR の bit17-24 (ノイズと同じ LFSR)
    } else if (!lfoDirty_) {
        return; // 波形の位置も AMD / PMD / 波形も変わらなければ、AM / PM の値も変わらない
    }
    // AMD / PMD / 波形を書いただけなら、ノイズの値は前のまま (ymfm と同じ)
    lfoDirty_ = false;
    int am, pm;
    switch (reg_[0x1b] & 3) {
    case 0: // のこぎり波
        am = 255 - ph;
        pm = (int8_t)ph;
        break;
    case 1: // 矩形波
        am = ph < 128 ? 255 : 0;
        pm = ph < 128 ? 127 : -128;
        break;
    case 2: // 三角波 (ymfm と同じ形)
        am = ph < 128 ? ((ph ^ 0xff) << 1) & 0xff : (ph << 1) & 0xff;
        pm = (int8_t)(uint8_t)((ph & 64) ? am : ~am);
        break;
    default: // ノイズ
        am = lfoNoise_;
        pm = (int8_t)lfoNoise_;
        break;
    }
    lfoAm_ = (am * amd_) >> 7;
    lfoPm_ = (pm * pmd_) >> 7;
}

// ノイズ (ymfm と同じ): 17 ビットの LFSR は 32 クロックごとに (1 サンプルに 2 回) 進め続け、
// NFRQ で決まる回数 ((NFRQ ^ 31) + 1 回) ごとに bit17 を取り出してノイズの値にする。
// LFSR は左にずらし、下位 8 ビットに最近の 8 ビットの履歴、bit8-24 に 17 ビットの状態が入る
void Opm::clockNoise() {
    uint32_t freq = (reg_[0x0f] & 31) ^ 31;
    for (int rep = 0; rep < 2; rep++) {
        noiseLfsr_ <<= 1;
        noiseLfsr_ |= ((noiseLfsr_ >> 17) ^ (noiseLfsr_ >> 14) ^ 1) & 1;
        if (noiseCount_++ >= freq) {
            noiseCount_ = 0;
            noiseBit_ = ((noiseLfsr_ >> 17) & 1) != 0;
        }
    }
}

// エンベロープを 1 回進める (3 サンプルに 1 回)。レートが高いほど、多く・頻繁に変わる
void Opm::clockEnvelope(Op& op) {
    if (op.state == EG_DECAY && op.env >= op.d1lLevel) op.state = EG_SUSTAIN;
    int rate = op.rate[op.state];
    if (rate < 2) return;
    int shift = rate >> 2;
    uint32_t idx;
    if (shift < 11) {
        uint32_t s = 11 - (uint32_t)shift;
        if (egCounter_ & ((1u << s) - 1)) return;
        idx = (egCounter_ >> s) & 7;
    } else {
        idx = egCounter_ & 7;
    }
    int32_t inc = egIncrement(rate, idx);
    if (op.state == EG_ATTACK) {
        if (rate >= 62) op.env = 0;
        else op.env += ((~op.env) * inc) >> 4;
        if (op.env <= 0) {
            op.env = 0;
            op.state = EG_DECAY;
        }
    } else {
        op.env += inc;
        if (op.env > ENV_MAX) op.env = ENV_MAX;
    }
}

int32_t Opm::channelOutput(Channel& ch, int chIndex) {
    Op& m1 = ch.op[0];
    Op& m2 = ch.op[1];
    Op& c1 = ch.op[2];
    Op& c2 = ch.op[3];
    int32_t att[4];
    int32_t am = ch.ams ? lfoAm_ << (ch.ams - 1) : 0;
    for (int i = 0; i < 4; i++) {
        Op& op = ch.op[i];
        op.phase = (op.phase + op.step) & 0xfffff;
        int32_t a = op.env + op.tlLevel + (op.amEnable ? am : 0);
        att[i] = a > ENV_MAX ? ENV_MAX : a;
    }
    int32_t fbMod = ch.fb ? (ch.fbOut[0] + ch.fbOut[1]) >> (10 - ch.fb) : 0;
    int32_t o1 = opOutput(m1.phase, fbMod, att[0]);
    ch.fbOut[0] = ch.fbOut[1];
    ch.fbOut[1] = o1;
    // チャネル 7 の C2 は、ノイズを有効にするとノイズになる
    bool noise = chIndex == 7 && (reg_[0x0f] & 0x80);
    auto outC2 = [&](int32_t mod) -> int32_t {
        if (!noise) return opOutput(c2.phase, mod, att[3]);
        // ノイズの音量は対数から戻さず、減衰量 (10 ビット) を反転して 1 ビット上げたもの (ymfm と同じ)
        int32_t v = (att[3] ^ 0x3ff) << 1;
        return noiseBit_ ? -v : v;
    };
    int32_t o2, o3, o4; // C1, M2, C2
    switch (ch.con) {
    case 0: // M1 -> C1 -> M2 -> C2
        o2 = opOutput(c1.phase, o1 >> 1, att[2]);
        o3 = opOutput(m2.phase, o2 >> 1, att[1]);
        o4 = outC2(o3 >> 1);
        return o4;
    case 1: // (M1 + C1) -> M2 -> C2
        o2 = opOutput(c1.phase, 0, att[2]);
        o3 = opOutput(m2.phase, (o1 + o2) >> 1, att[1]);
        o4 = outC2(o3 >> 1);
        return o4;
    case 2: // (M1 + (C1 -> M2)) -> C2
        o2 = opOutput(c1.phase, 0, att[2]);
        o3 = opOutput(m2.phase, o2 >> 1, att[1]);
        o4 = outC2((o1 + o3) >> 1);
        return o4;
    case 3: // ((M1 -> C1) + M2) -> C2
        o2 = opOutput(c1.phase, o1 >> 1, att[2]);
        o3 = opOutput(m2.phase, 0, att[1]);
        o4 = outC2((o2 + o3) >> 1);
        return o4;
    case 4: // (M1 -> C1) + (M2 -> C2)
        o2 = opOutput(c1.phase, o1 >> 1, att[2]);
        o3 = opOutput(m2.phase, 0, att[1]);
        o4 = outC2(o3 >> 1);
        return o2 + o4;
    case 5: // M1 -> C1, M2, C2
        o2 = opOutput(c1.phase, o1 >> 1, att[2]);
        o3 = opOutput(m2.phase, o1 >> 1, att[1]);
        o4 = outC2(o1 >> 1);
        return o2 + o3 + o4;
    case 6: // (M1 -> C1) + M2 + C2
        o2 = opOutput(c1.phase, o1 >> 1, att[2]);
        o3 = opOutput(m2.phase, 0, att[1]);
        o4 = outC2(0);
        return o2 + o3 + o4;
    default: // M1 + C1 + M2 + C2
        o2 = opOutput(c1.phase, 0, att[2]);
        o3 = opOutput(m2.phase, 0, att[1]);
        o4 = outC2(0);
        return o1 + o2 + o3 + o4;
    }
}

void Opm::clockSample(int32_t* out) {
    clockTimers();
    if (!out) return;
    clockLfo();
    clockNoise();
    if (++egDivider_ >= 3) {
        egDivider_ = 0;
        egCounter_++;
        for (Channel& ch : ch_) {
            if (ch.idle) continue;
            bool idle = true;
            for (Op& op : ch.op) {
                clockEnvelope(op);
                if (op.state != EG_RELEASE || op.env < ENV_MAX) idle = false;
            }
            ch.idle = idle;
        }
    }
    // PM: LFO の値が変わったら、PM のかかっているチャネルの位相の増分を計算し直す
    if (lfoPm_ != lastPm_) {
        lastPm_ = lfoPm_;
        for (Channel& ch : ch_) {
            if (!ch.pmActive) continue;
            int32_t d = ch.pms < 6 ? lfoPm_ >> (6 - ch.pms) : lfoPm_ * (1 << (ch.pms - 5));
            for (Op& op : ch.op) updateOp(ch, op, d);
        }
    }
    int32_t l = 0, r = 0;
    for (int c = 0; c < 8; c++) {
        Channel& ch = ch_[c];
        // 4 つのオペレータがすべて鳴り終わっていれば計算しない (キーオンで位相は 0 に戻るので、位相を進めなくてよい)
        if (ch.idle) continue;
        int32_t v = channelOutput(ch, c);
        if (ch.left) l += v;
        if (ch.right) r += v;
    }
    out[0] = l;
    out[1] = r;
}

} // namespace x68k
