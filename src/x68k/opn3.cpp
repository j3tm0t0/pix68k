// YMF288 (OPN3-L)。もとにした資料と、推測で決めたことは opn3.h を参照
#include "x68k/opn3.h"

#include <string.h>

namespace x68k {

namespace {

// ---- 表 (最初の Opn3 を作るときに式から作る。ベアメタルには libm がないので、級数で計算する) ----
uint16_t kLogSin[256]; // 1/4 周期の正弦波の対数: -log2(sin((i + 0.5) * pi / 512)) * 256
uint16_t kPow[256];    // 指数: 2^(-(i + 1) / 256) * 2048 を丸めて 4 倍 (最大 8168)
int32_t kPmScale[8][8]; // PM: 周波数に掛ける割合 (16 ビットの小数)。PMS と、1/4 周期の中の位置 (8 段)
uint16_t kSsgVol[32];  // SSG の音量 (1.5dB 刻み)。矩形波の上から下までの幅
uint16_t kRhythmVol[95]; // リズムの音量 (0.75dB 刻みの減衰、12 ビットの小数)
bool tablesReady = false;
const Opn3::RhythmSet* rhythmSet = nullptr;

const double kPi = 3.14159265358979323846;
const double kLn2 = 0.69314718055994530942;

double expSeries(double x) { // e^x (|x| < 1)
    double sum = 1, term = 1;
    for (int n = 1; n < 24; n++) {
        term *= x / n;
        sum += term;
    }
    return sum;
}

double pow2(double x) { // 2^x
    int whole = (int)x;
    if ((double)whole > x) whole--;
    double r = expSeries((x - whole) * kLn2);
    for (; whole > 0; whole--) r *= 2;
    for (; whole < 0; whole++) r /= 2;
    return r;
}

double log2of(double y) { // log2(y)、y > 0
    int e = 0;
    while (y >= 2) {
        y /= 2;
        e++;
    }
    while (y < 1) {
        y *= 2;
        e--;
    }
    // y は [1, 2)。ln(y) = 2 atanh((y - 1) / (y + 1))
    const double z = (y - 1) / (y + 1);
    double sum = 0, term = z;
    for (int n = 1; n < 80; n += 2) {
        sum += term / n;
        term *= z * z;
    }
    return e + 2 * sum / kLn2;
}

double sine(double x) { // sin(x)、0 <= x <= pi/2
    double sum = 0, term = x;
    for (int n = 1; n < 30; n += 2) {
        sum += term;
        term *= -x * x / ((n + 1) * (n + 2));
    }
    return sum;
}

int roundInt(double v) { return (int)(v + 0.5); }

void buildTables() {
    if (tablesReady) return;
    for (int i = 0; i < 256; i++) {
        kLogSin[i] = (uint16_t)roundInt(-log2of(sine((i + 0.5) * kPi / 512)) * 256);
        kPow[i] = (uint16_t)(roundInt(pow2(-(i + 1) / 256.0) * 2048) << 2);
    }
    // PMS の深さ (セント。アプリケーションマニュアルの 2-6)
    static const double cents[8] = {0, 3.4, 6.7, 10, 14, 20, 40, 80};
    for (int p = 0; p < 8; p++)
        for (int q = 0; q < 8; q++)
            kPmScale[p][q] = roundInt((pow2(cents[p] / 1200 * sine((q + 0.5) * kPi / 16)) - 1) * 65536);
    // SSG の 1 チャネルの最大 (矩形波の振れ幅の半分) は、FM の 1 スロットの最大の 0.25 倍 (YMF288 のデータシートの図 4)。
    // この表は矩形波の上から下までの幅 (その 2 倍)。ssgOutput() が 0 を中心に振らせる
    kSsgVol[0] = 0;
    for (int s = 1; s < 32; s++) kSsgVol[s] = (uint16_t)roundInt(8168 * 0.5 * pow2((s - 31) / 4.0));
    for (int n = 0; n < 95; n++) kRhythmVol[n] = (uint16_t)roundInt(4096 * pow2(-n * 0.75 / 6.0206));
    tablesReady = true;
}

// Detune による増分の補正 (アプリケーションマニュアルの表 2-6 の周波数を、8MHz のときの増分 1 = 0.053Hz で割ったもの)。
// キーコード (Block と Note) と Detune の下位 2 ビットで決まる
const uint8_t kDetune[32][4] = {
    {0, 0, 1, 2},   {0, 0, 1, 2},   {0, 0, 1, 2},   {0, 0, 1, 2},   {0, 1, 2, 2},   {0, 1, 2, 3},   {0, 1, 2, 3},
    {0, 1, 2, 3},   {0, 1, 2, 4},   {0, 1, 3, 4},   {0, 1, 3, 4},   {0, 1, 3, 5},   {0, 2, 4, 5},   {0, 2, 4, 6},
    {0, 2, 4, 6},   {0, 2, 5, 7},   {0, 2, 5, 8},   {0, 3, 6, 8},   {0, 3, 6, 9},   {0, 3, 7, 10},  {0, 4, 8, 11},
    {0, 4, 8, 12},  {0, 4, 9, 13},  {0, 5, 10, 14}, {0, 5, 11, 16}, {0, 6, 12, 17}, {0, 6, 13, 19}, {0, 7, 14, 20},
    {0, 8, 16, 22}, {0, 8, 16, 22}, {0, 8, 16, 22}, {0, 8, 16, 22},
};

// LFO の 1 段 (1 周期の 1/128) のサンプル数。マニュアルの周波数 3.98、5.56、6.02、6.37、6.88、9.63、48.1、72.2Hz
// (7.9872MHz のとき) から: 55467 / 128 / 周波数
const uint8_t kLfoPeriod[8] = {109, 78, 72, 68, 63, 45, 9, 6};

// AMS: LFO の AM (0..126、1 = 0.09375dB) を右に寄せる数。深さは 0、1.4、5.9、11.8dB
const uint8_t kAmShift[4] = {8, 3, 1, 0};

const int32_t ENV_MAX = 1023;

// タイマー A / B が 1 つ数える間の、4MHz のクロックの数 (マスタークロック 144 個と 2304 個)
const uint32_t TIMER_A_CLOCKS = 72, TIMER_B_CLOCKS = 1152;

// エンベロープの 1 回の変化量 (rate: 実効レート 0..63、i: 刻みの番号の下位 3 ビット)
//   レート 0-1: 変化しない
//   レート 2-47: 0 か 1。8 回のうち 1 になる回数が、レートの下位 2 ビットで 4、5、6、7 回 (レート 2-5 は 4 回、6-7 は 6 回)
//   レート 48-59: 2^(レート / 4 - 12) と、その 2 倍が、レートの下位 2 ビットで決まる割合で混ざる
//   レート 60-63: いつも 8
int32_t egStep(int rate, uint32_t i) {
    static const uint8_t mix[4] = {0x00, 0x88, 0xaa, 0xee}; // bit i が 1 の回に多いほうの値を使う
    if (rate < 2) return 0;
    if (rate >= 60) return 8;
    if (rate >= 48) return (1 << ((rate >> 2) - 12)) << ((mix[rate & 3] >> i) & 1);
    static const uint8_t ones[4] = {0xaa, 0xba, 0xee, 0xfe};
    const int sel = rate < 6 ? 0 : rate < 8 ? 2 : (rate & 3);
    return (ones[sel] >> i) & 1;
}

// オペレータの出力: 正弦波の位置 (10 ビット) と減衰量 (10 ビット) から。14 ビットの符号付き
inline int32_t opOutput(uint32_t p, int32_t att) {
    if (att >= ENV_MAX) return 0;
    const uint32_t idx = (p & 0x100) ? (~p & 0xff) : (p & 0xff);
    const uint32_t x = kLogSin[idx] + ((uint32_t)att << 2);
    const uint32_t shift = x >> 8;
    if (shift > 13) return 0;
    const int32_t v = kPow[x & 0xff] >> shift;
    return (p & 0x200) ? -v : v;
}

// キーコードの下位 2 ビット (Note)。F-Number の上位 4 ビット F11-F8 から決まる (アプリケーションマニュアルの表 2-4)
inline uint32_t noteOf(uint32_t fnum) {
    const uint32_t f11 = (fnum >> 10) & 1, f10 = (fnum >> 9) & 1, f9 = (fnum >> 8) & 1, f8 = (fnum >> 7) & 1;
    const uint32_t n3 = (f11 & (f10 | f9 | f8)) | ((f11 ^ 1) & f10 & f9 & f8);
    return f11 << 1 | n3;
}

} // namespace

void Opn3::setRhythm(const RhythmSet* set) { rhythmSet = set; }
const Opn3::RhythmSet* Opn3::rhythm() { return rhythmSet; }

Opn3::Opn3() {
    buildTables();
    reset();
}

void Opn3::reset() {
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < 256; i++) reg_[b][i] = 0;
    addr_ = bank_ = 0;
    flags_ = mask_ = 0;
    irqEnable_ = 3; // $29 の D1、D0 の初期値は 1
    reg_[0][0x29] = 3;
    sixCh_ = false;
    ch3Special_ = false;
    fmIdle_ = true;
    timerA_ = timerB_ = 0;
    lfoCount_ = kLfoPeriod[0];
    lfoStep_ = 0;
    lfoOn_ = false;
    lfoAm_ = 0;
    pmDirty_ = false;
    egCounter_ = 0;
    egDivider_ = 0;
    for (int c = 0; c < 6; c++) {
        ch_[c] = Channel();
        reg_[c / 3][0xb4 + c % 3] = 0xc0; // L と R の初期値は 1
        updateChannel(c);
    }
    for (int i = 0; i < 3; i++) {
        fnum3_[i] = 0;
        block3_[i] = latch3_[i] = 0;
        toneCount_[i] = 1;
        toneOut_[i] = 0;
    }
    noiseCount_ = 1;
    noiseLfsr_ = 1;
    noiseOut_ = 0;
    envCount_ = 1;
    envPos_ = 0;
    envAttack_ = envHold_ = false;
    envFixed_ = 0;
    ssgOn_ = rhythmOn_ = false;
    rhythmTotal_ = 0;
    pad0_ = 0;
    for (int i = 0; i < RHYTHMS; i++) rhythm_[i] = Rhythm();
}

uint8_t Opn3::readData() const {
    if (!newMode()) {
        // YM2608B 互換モード: 読めるのは SSG のレジスタだけ。ほかは、読む前にバスにあった値 (ここではアドレス) が見える
        if (addr_ == 0xff) return 0x01;
        if (addr_ == 0x0e || addr_ == 0x0f) return 0xff;
        return bank_ == 0 && addr_ < 0x0e ? reg_[0][addr_] : addr_;
    }
    if (addr_ == 0xff) return 0x02; // デバイス識別コード
    if (bank_ == 0 && (addr_ == 0x0e || addr_ == 0x0f)) return 0xff; // I/O ポートはない (ハイインピーダンス)
    return reg_[bank_][addr_];
}

uint32_t Opn3::busyClocks(bool data, int bank, uint8_t reg) const {
    if (!newMode()) return data ? 192 : 0; // YM2608B 互換モード: データを書いたあとだけ
    if (!data || bank != 0) return 15;
    return reg == 0x10 ? 180 : reg == 0x28 ? 192 : 15;
}

uint32_t Opn3::clocksToIrq() const {
    if (irq()) return 0;
    const uint8_t ctl = reg_[0][0x27];
    uint32_t n = 0;
    if (timerA_ && (ctl & 0x04) && (irqEnable_ & ~mask_ & ~flags_ & 1)) n = timerA_;
    if (timerB_ && (ctl & 0x08) && (irqEnable_ & ~mask_ & ~flags_ & 2) && (n == 0 || timerB_ < n)) n = timerB_;
    return n;
}

// タイマー A は 1 サンプル (マスタークロック 144 個) ごとに 1 つ数え、(1024 - NA) サンプルであふれる。タイマー B はその 16 倍の
// 2304 クロックごとに数え、(256 - NB) 回であふれる。8MHz ではタイマー A の 1 つが 18 マイクロ秒、タイマー B が 288 マイクロ秒。
// YM2608 のアプリケーションマニュアルの式 tA = 72 x (1024 - NA) / fM、tB = 1152 x (256 - NB) / fM は YM2203 のものと同じ形で、
// YM2203 の標準の 4MHz を入れるとこの値になる (YM2608 の FM は YM2203 の 2 倍のクロックを同じサンプリング周波数に分周して動くので、
// 8MHz では係数が 2 倍になる)。マニュアルの例は 8MHz を入れて 9 マイクロ秒と書いているが、それだと PC-9801 用に作られた曲の
// テンポが 2 倍になる。PMD (KAJA さんの PC-9801 用の音楽のドライバ) の説明書のテンポの式 (テンポ = $112C / (256 - タイマー B)、
// 2 分音符が 48 クロック) は、タイマー B の 1 つが 0.284 ミリ秒のときに合う (7.9872MHz の 2304 クロックは 0.288 ミリ秒)
void Opn3::advanceTimers(uint32_t clocks) {
    const uint8_t ctl = reg_[0][0x27];
    if (timerA_) {
        if (clocks >= timerA_) {
            const uint32_t period = TIMER_A_CLOCKS * (1024 - ((uint32_t)reg_[0][0x24] << 2 | (reg_[0][0x25] & 3)));
            if (ctl & 0x04) flags_ |= 1;
            timerA_ = period - (clocks - timerA_) % period;
        } else {
            timerA_ -= clocks;
        }
    }
    if (timerB_) {
        if (clocks >= timerB_) {
            const uint32_t period = TIMER_B_CLOCKS * (256 - (uint32_t)reg_[0][0x26]);
            if (ctl & 0x08) flags_ |= 2;
            timerB_ = period - (clocks - timerB_) % period;
        } else {
            timerB_ -= clocks;
        }
    }
}

void Opn3::writeReg(int bank, uint8_t r, uint8_t v) {
    bank &= 1;
    if (r >= 0x30) {
        if (r <= 0xb6) writeFm(bank, r, v);
        return;
    }
    if (bank == 1) {
        if (r == 0x10) { // フラグの制御
            if (v & 0x80) flags_ = 0; // IRQ RESET: マスクは変えない
            else reg_[1][0x10] = mask_ = v & 3;
        }
        return;
    }
    if (r < 0x10) {
        writeSsg(r, v);
        return;
    }
    if (r < 0x20) {
        writeRhythm(r, v);
        return;
    }
    switch (r) {
    case 0x20: reg_[0][r] = v & 3; break; // NEW、STBY
    case 0x22: {
        reg_[0][r] = v & 0x0f;
        const bool on = (v & 0x08) != 0;
        if (!on) {
            lfoStep_ = 0;
            lfoAm_ = 0;
            if (lfoOn_) pmDirty_ = true;
        }
        if (on != lfoOn_) lfoCount_ = kLfoPeriod[v & 7];
        lfoOn_ = on;
        break;
    }
    case 0x24: reg_[0][r] = v; break;
    case 0x25: reg_[0][r] = v & 3; break;
    case 0x26: reg_[0][r] = v; break;
    case 0x27: {
        reg_[0][r] = v & 0x4f; // リセットのビットは 0 に読める。bit7 (CSM) はない
        if (v & 0x10) flags_ &= (uint8_t)~1;
        if (v & 0x20) flags_ &= (uint8_t)~2;
        if (!(v & 0x01)) timerA_ = 0;
        else if (!timerA_) timerA_ = TIMER_A_CLOCKS * (1024 - ((uint32_t)reg_[0][0x24] << 2 | (reg_[0][0x25] & 3)));
        if (!(v & 0x02)) timerB_ = 0;
        else if (!timerB_) timerB_ = TIMER_B_CLOCKS * (256 - (uint32_t)reg_[0][0x26]);
        const bool special = (v & 0x40) != 0;
        if (special != ch3Special_) {
            ch3Special_ = special;
            updateChannel(2);
        }
        break;
    }
    case 0x28: { // キーオン / オフ: bit4-7 が S1-S4、bit2-0 がチャネル (0-2 と 4-6)
        if ((v & 3) == 3) break;
        Channel& ch = ch_[(v & 3) + ((v & 4) ? 3 : 0)];
        for (int s = 0; s < 4; s++) {
            if (v & (0x10 << s)) keyOn(ch, ch.op[s]);
            else keyOff(ch.op[s]);
        }
        break;
    }
    case 0x29:
        reg_[0][r] = v & 0x83;
        sixCh_ = (v & 0x80) != 0;
        irqEnable_ = v & 3;
        break;
    default: break; // $21、$2A-$2C はテスト用。$2D-$2F のプリスケーラはない
    }
}

void Opn3::keyOn(Channel& ch, Op& op) {
    if (op.key) return;
    op.key = true;
    op.phase = 0;
    op.ssgInv = false;
    op.state = EG_ATTACK;
    if (op.rate[EG_ATTACK] >= 62) op.env = 0;
    ch.idle = false;
    fmIdle_ = false;
}

void Opn3::keyOff(Op& op) {
    if (!op.key) return;
    op.key = false;
    // SSG 型エンベロープで出力を裏返していたら、今の出力と同じ大きさになる減衰量に直してからリリースに入る
    if ((op.ssg & 0x08) && (op.ssgInv != ((op.ssg & 0x04) != 0))) op.env = (0x200 - op.env) & ENV_MAX;
    op.state = EG_RELEASE;
}

void Opn3::writeFm(int bank, uint8_t r, uint8_t v) {
    const int n = r & 3;
    if (n == 3) return;
    const int c = n + bank * 3;
    Channel& ch = ch_[c];
    if (r >= 0xa0) {
        switch (r & 0xfc) {
        case 0xa0: // F-Number の下位。$A4 に書いておいた上位と一緒に入る
            reg_[bank][r] = v;
            ch.fnum = (uint16_t)((ch.latch & 7) << 8 | v);
            ch.block = (ch.latch >> 3) & 7;
            updateChannel(c);
            break;
        case 0xa4:
            reg_[bank][r] = ch.latch = v & 0x3f;
            break;
        case 0xa8: // チャネル 3 のスロットごとの F-Number ($A8 が S3、$A9 が S1、$AA が S2)
            if (bank == 0) {
                static const uint8_t slotOf[3] = {2, 0, 1};
                const int s = slotOf[n];
                reg_[0][r] = v;
                fnum3_[s] = (uint16_t)((latch3_[s] & 7) << 8 | v);
                block3_[s] = (latch3_[s] >> 3) & 7;
                if (ch3Special_) updateChannel(2);
            }
            break;
        case 0xac:
            if (bank == 0) {
                static const uint8_t slotOf[3] = {2, 0, 1};
                reg_[0][r] = latch3_[slotOf[n]] = v & 0x3f;
            }
            break;
        case 0xb0:
            reg_[bank][r] = v & 0x3f;
            ch.fb = (v >> 3) & 7;
            ch.alg = v & 7;
            break;
        case 0xb4:
            reg_[bank][r] = v & 0xf7;
            ch.left = (v & 0x80) != 0;
            ch.right = (v & 0x40) != 0;
            ch.ams = (v >> 4) & 3;
            ch.pms = v & 7;
            updateChannel(c);
            break;
        }
        return;
    }
    // オペレータのレジスタ: bit3-2 がスロット (0 S1、1 S3、2 S2、3 S4)
    static const uint8_t slotOf[4] = {0, 2, 1, 3};
    Op& op = ch.op[slotOf[(r >> 2) & 3]];
    switch (r & 0xf0) {
    case 0x30:
        reg_[bank][r] = v & 0x7f;
        op.dt = (v >> 4) & 7;
        op.mul = v & 15;
        break;
    case 0x40:
        reg_[bank][r] = v & 0x7f;
        op.tl = v & 0x7f;
        op.tlLevel = op.tl << 3;
        return;
    case 0x50:
        reg_[bank][r] = v & 0xdf;
        op.ks = v >> 6;
        op.ar = v & 0x1f;
        break;
    case 0x60:
        reg_[bank][r] = v & 0x9f;
        op.amEnable = (v & 0x80) != 0;
        op.dr = v & 0x1f;
        break;
    case 0x70:
        reg_[bank][r] = v & 0x1f;
        op.sr = v & 0x1f;
        break;
    case 0x80:
        reg_[bank][r] = v;
        op.sl = v >> 4;
        op.rr = v & 15;
        op.slLevel = (op.sl == 15 ? 31 : op.sl) << 5;
        break;
    case 0x90:
        reg_[bank][r] = v & 0x0f;
        op.ssg = v & 0x0f;
        ch.ssgEg = ((ch.op[0].ssg | ch.op[1].ssg | ch.op[2].ssg | ch.op[3].ssg) & 0x08) != 0;
        return;
    }
    updateChannel(c);
}

void Opn3::updateOp(Op& op, uint32_t fnum, uint32_t block, int pm) {
    const uint32_t kc = block << 2 | noteOf(fnum);
    int32_t base = (int32_t)((fnum << block) >> 1);
    if (pm) base += (int32_t)(((int64_t)base * pm) >> 16);
    const int32_t det = kDetune[kc][op.dt & 3];
    base = (op.dt & 4) ? base - det : base + det;
    const uint32_t b = (uint32_t)base & 0x1ffff;
    op.step = (op.mul ? b * op.mul : b >> 1) & 0xfffff;
    // Rate = 2R + Rks (R = 0 なら 0)。リリースは R = RR x 2 + 1。Rks はキーコードを KS で決まる数だけ右に寄せたもの
    const uint32_t rks = kc >> (3 - op.ks);
    const uint32_t r[4] = {op.ar, op.dr, op.sr, (uint32_t)op.rr * 2 + 1};
    for (int i = 0; i < 4; i++) {
        const uint32_t rate = r[i] ? 2 * r[i] + rks : 0;
        op.rate[i] = (uint8_t)(rate > 63 ? 63 : rate);
    }
}

void Opn3::updateChannel(int c) {
    Channel& ch = ch_[c];
    int pm = 0;
    if (lfoOn_ && ch.pms) {
        const uint32_t i = lfoStep_ >> 2; // 32 段の正弦波
        const uint32_t q = (i & 8) ? 7 - (i & 7) : (i & 7);
        pm = kPmScale[ch.pms][q];
        if (i & 16) pm = -pm;
    }
    for (int s = 0; s < 4; s++) {
        if (c == 2 && ch3Special_ && s < 3) updateOp(ch.op[s], fnum3_[s], block3_[s], pm);
        else updateOp(ch.op[s], ch.fnum, ch.block, pm);
    }
}

void Opn3::clockLfo() {
    if (!lfoOn_) return;
    if (--lfoCount_) return;
    lfoCount_ = kLfoPeriod[reg_[0][0x22] & 7];
    lfoStep_ = (lfoStep_ + 1) & 127;
    lfoAm_ = (uint8_t)((lfoStep_ < 64 ? lfoStep_ : 127 - lfoStep_) * 2);
    if ((lfoStep_ & 3) == 0) pmDirty_ = true;
}

// SSG 型エンベロープ ($90 の bit3 が 1): 減衰量が $200 に届いたところで、繰り返す、裏返す、止める
void Opn3::clockSsgEg(Op& op) {
    if (!(op.ssg & 0x08) || op.env < 0x200) return;
    const bool hold = (op.ssg & 0x01) != 0, alternate = (op.ssg & 0x02) != 0, attack = (op.ssg & 0x04) != 0;
    if (alternate && (!hold || !op.ssgInv)) op.ssgInv = !op.ssgInv;
    if (!alternate && !hold) op.phase = 0;
    if (op.state == EG_ATTACK) return;
    if (op.state != EG_RELEASE && !hold) {
        op.state = EG_ATTACK; // 繰り返す
        if (op.rate[EG_ATTACK] >= 62) op.env = 0;
    } else if (op.state == EG_RELEASE || op.ssgInv == attack) {
        op.env = ENV_MAX;
    }
}

void Opn3::clockEnvelope(Op& op) {
    if (op.state == EG_ATTACK && op.env == 0) op.state = EG_DECAY;
    if (op.state == EG_DECAY && op.env >= op.slLevel) op.state = EG_SUSTAIN;
    const int rate = op.rate[op.state];
    const int shift = rate < 48 ? 11 - (rate >> 2) : 0;
    if (egCounter_ & ((1u << shift) - 1)) return;
    const int32_t inc = egStep(rate, (egCounter_ >> shift) & 7);
    if (!inc) return;
    if (op.state == EG_ATTACK) {
        if (rate < 62) op.env += (~op.env * inc) >> 4;
        if (op.env <= 0) {
            op.env = 0;
            op.state = EG_DECAY;
        }
        return;
    }
    if (op.ssg & 0x08) {
        if (op.env < 0x200) op.env += 4 * inc; // SSG 型は 4 倍の速さで、$200 まで
    } else {
        op.env += inc;
    }
    if (op.env > ENV_MAX) op.env = ENV_MAX;
    if (op.state == EG_DECAY && op.env >= op.slLevel) op.state = EG_SUSTAIN;
}

int32_t Opn3::channelOutput(Channel& ch) {
    const int32_t am = lfoAm_ >> kAmShift[ch.ams];
    int32_t att[4];
    for (int s = 0; s < 4; s++) {
        const Op& op = ch.op[s];
        att[s] = op.env + op.tlLevel + (op.amEnable ? am : 0);
    }
    if (ch.ssgEg) { // SSG 型エンベロープで裏返しているオペレータは、$200 から引いた減衰量にする
        for (int s = 0; s < 4; s++) {
            const Op& op = ch.op[s];
            if ((op.ssg & 0x08) && op.state != EG_RELEASE && (op.ssgInv != ((op.ssg & 0x04) != 0)))
                att[s] += ((0x200 - op.env) & ENV_MAX) - op.env;
        }
    }
    Op* const op = ch.op;
#define OPN3_OP(s, mod) opOutput((op[s].phase >> 10) + (uint32_t)(mod), att[s])
    const int32_t fb = ch.fb ? (ch.fbOut[0] + ch.fbOut[1]) >> (10 - ch.fb) : 0;
    const int32_t o1 = OPN3_OP(0, fb);
    ch.fbOut[1] = ch.fbOut[0];
    ch.fbOut[0] = o1;
    int32_t out, o2, o3;
    switch (ch.alg) {
    case 0: // S1 → S2 → S3 → S4
        o2 = OPN3_OP(1, o1 >> 1);
        o3 = OPN3_OP(2, o2 >> 1);
        out = OPN3_OP(3, o3 >> 1);
        break;
    case 1: // (S1 + S2) → S3 → S4
        o2 = OPN3_OP(1, 0);
        o3 = OPN3_OP(2, (o1 + o2) >> 1);
        out = OPN3_OP(3, o3 >> 1);
        break;
    case 2: // (S1 + (S2 → S3)) → S4
        o2 = OPN3_OP(1, 0);
        o3 = OPN3_OP(2, o2 >> 1);
        out = OPN3_OP(3, (o1 + o3) >> 1);
        break;
    case 3: // ((S1 → S2) + S3) → S4
        o2 = OPN3_OP(1, o1 >> 1);
        o3 = OPN3_OP(2, 0);
        out = OPN3_OP(3, (o2 + o3) >> 1);
        break;
    case 4: // (S1 → S2) + (S3 → S4)
        o2 = OPN3_OP(1, o1 >> 1);
        o3 = OPN3_OP(2, 0);
        out = o2 + OPN3_OP(3, o3 >> 1);
        break;
    case 5: // S1 → (S2 + S3 + S4)
        out = OPN3_OP(1, o1 >> 1) + OPN3_OP(2, o1 >> 1) + OPN3_OP(3, o1 >> 1);
        break;
    case 6: // (S1 → S2) + S3 + S4
        out = OPN3_OP(1, o1 >> 1) + OPN3_OP(2, 0) + OPN3_OP(3, 0);
        break;
    default: // S1 + S2 + S3 + S4
        out = o1 + OPN3_OP(1, 0) + OPN3_OP(2, 0) + OPN3_OP(3, 0);
        break;
    }
#undef OPN3_OP
    for (int s = 0; s < 4; s++) op[s].phase = (op[s].phase + op[s].step) & 0xfffff;
    return out;
}

// ---- SSG ----

void Opn3::updateSsgOn() { ssgOn_ = ((reg_[0][8] | reg_[0][9] | reg_[0][10]) & 0x1f) != 0; }

void Opn3::writeSsg(uint8_t r, uint8_t v) {
    switch (r) {
    case 1: case 3: case 5: reg_[0][r] = v & 0x0f; break;
    case 6: reg_[0][r] = v & 0x1f; break;
    case 8: case 9: case 10:
        reg_[0][r] = v & 0x1f;
        updateSsgOn();
        break;
    case 13: // エンベロープの形。書くと最初から始まる
        reg_[0][r] = v & 0x0f;
        envPos_ = 0;
        envHold_ = false;
        envAttack_ = (v & 0x04) != 0;
        envCount_ = 4 * (((uint32_t)reg_[0][12] << 8 | reg_[0][11]) ? ((uint32_t)reg_[0][12] << 8 | reg_[0][11]) : 1);
        break;
    case 14: case 15: break; // I/O ポートはない
    default: reg_[0][r] = v; break;
    }
}

// 音量がすべて 0 の間も、矩形波、ノイズ、エンベロープのカウンタは進める (音量を上げたときの位相が、実チップと同じになるように)。
// 出力は計算しない。1 サンプルは 18 刻みで、矩形波の半周期は 32 刻み以上なので、反転は多くて 1 回
void Opn3::ssgAdvanceSilent() {
    const uint8_t* const r = reg_[0];
    for (int c = 0; c < 3; c++) {
        const uint32_t tp = (uint32_t)r[c * 2 + 1] << 8 | r[c * 2];
        if (tp < 8) continue;
        const uint32_t period = 4 * tp;
        if (toneCount_[c] > period) toneCount_[c] = period;
        if (toneCount_[c] > 18) {
            toneCount_[c] -= 18;
        } else {
            toneOut_[c] ^= 1;
            toneCount_[c] = period - (18 - toneCount_[c]);
        }
    }
    const uint32_t noisePeriod = 8 * (r[6] ? r[6] : 1);
    if (noiseCount_ > noisePeriod) noiseCount_ = noisePeriod;
    for (uint32_t left = 18; left;) {
        const uint32_t seg = left < noiseCount_ ? left : noiseCount_;
        left -= seg;
        if ((noiseCount_ -= seg) == 0) {
            noiseCount_ = noisePeriod;
            noiseLfsr_ = noiseLfsr_ >> 1 | ((noiseLfsr_ ^ noiseLfsr_ >> 3) & 1) << 16;
            noiseOut_ = noiseLfsr_ & 1;
        }
    }
    ssgEnvelope();
}

// エンベロープを 1 サンプル (18 刻み) 進めて、今の音量 (0..31) を返す。32 段で 1 周、1 段は エンベロープの周期の設定 x 4 刻み
uint32_t Opn3::ssgEnvelope() {
    if (envHold_) return envFixed_;
    const uint8_t* const r = reg_[0];
    const uint32_t ep = (uint32_t)r[12] << 8 | r[11];
    const uint32_t step = 4 * (ep ? ep : 1);
    uint32_t t = 18;
    while (!envHold_ && t >= envCount_) {
        t -= envCount_;
        envCount_ = step;
        if (++envPos_ < 32) continue;
        envPos_ = 0;
        const uint8_t shape = r[13];
        if (!(shape & 0x08)) { // 1 回で終わり、0 で止まる
            envHold_ = true;
            envFixed_ = 0;
        } else if (shape & 0x01) { // 止まる (ALT なら反対の端で)
            envHold_ = true;
            envFixed_ = (((shape & 0x04) != 0) != ((shape & 0x02) != 0)) ? 31 : 0;
        } else {
            if (shape & 0x02) envAttack_ = !envAttack_;
        }
    }
    if (envHold_) return envFixed_;
    envCount_ -= t;
    return envAttack_ ? envPos_ : 31u - envPos_;
}

// 1 サンプル (18 刻み) の間の SSG の出力。矩形波とノイズが変わるところで区切り、1 だった時間の割合を音量に掛ける
int32_t Opn3::ssgOutput() {
    const uint8_t* const r = reg_[0];
    uint32_t period[3];
    for (int c = 0; c < 3; c++) {
        const uint32_t tp = (uint32_t)r[c * 2 + 1] << 8 | r[c * 2];
        period[c] = tp >= 8 ? 4 * tp : 0; // 周期が 7 以下なら矩形波は出ない (1 のまま)
        if (period[c] && toneCount_[c] > period[c]) toneCount_[c] = period[c];
    }
    const uint32_t noisePeriod = 8 * (r[6] ? r[6] : 1);
    if (noiseCount_ > noisePeriod) noiseCount_ = noisePeriod;
    const uint8_t mixer = r[7];
    uint32_t high[3] = {0, 0, 0};
    uint32_t left = 18;
    while (left) {
        uint32_t seg = left < noiseCount_ ? left : noiseCount_;
        for (int c = 0; c < 3; c++)
            if (period[c] && toneCount_[c] < seg) seg = toneCount_[c];
        for (int c = 0; c < 3; c++) {
            const bool tone = period[c] == 0 || toneOut_[c] || (mixer >> c & 1);
            const bool noise = noiseOut_ || (mixer >> (c + 3) & 1);
            if (tone && noise) high[c] += seg;
            if (period[c] && (toneCount_[c] -= seg) == 0) {
                toneOut_[c] ^= 1;
                toneCount_[c] = period[c];
            }
        }
        if ((noiseCount_ -= seg) == 0) {
            noiseCount_ = noisePeriod;
            noiseLfsr_ = noiseLfsr_ >> 1 | ((noiseLfsr_ ^ noiseLfsr_ >> 3) & 1) << 16; // 17 ビット
            noiseOut_ = noiseLfsr_ & 1;
        }
        left -= seg;
    }
    const uint32_t envLevel = ssgEnvelope();
    // 矩形波は 0 を中心に、音量の半分ずつ上下に振らせる (1 だった時間の割合から)。
    // トーンもノイズも止めたチャネルは、音量をそのまま出す (音量のレジスタを書き換えて PCM を鳴らす使い方。データシートに、
    // このときの出力は、止めていないときの 2 倍とある)
    int32_t sum = 0;
    for (int c = 0; c < 3; c++) {
        const uint8_t level = r[8 + c];
        const int32_t vol = (level & 0x10) ? kSsgVol[envLevel] : (level & 0x0f) ? kSsgVol[(level & 0x0f) * 2 + 1] : 0;
        if ((mixer >> c & 1) && (mixer >> (c + 3) & 1)) sum += vol * 36;
        else sum += vol * (2 * (int32_t)high[c] - 18);
    }
    return sum / 36;
}

// ---- リズム ----

void Opn3::writeRhythm(uint8_t r, uint8_t v) {
    switch (r) {
    case 0x10: // bit7 が 1 なら止める (ダンプ)、0 なら鳴らす
        for (int i = 0; i < RHYTHMS; i++) {
            if (!(v >> i & 1)) continue;
            Rhythm& rh = rhythm_[i];
            const RhythmSet* const set = rhythmSet;
            if ((v & 0x80) || !set || !set->pcm[i] || !set->length[i]) {
                rh.on = false;
                continue;
            }
            rh.pos = rh.frac = 0;
            // 1 サンプル (マスタークロック 144 個、8MHz) で進む量
            rh.step = (uint32_t)((uint64_t)set->rate[i] * 65536 * 144 / 8000000);
            rh.on = true;
        }
        rhythmOn_ = false;
        for (int i = 0; i < RHYTHMS; i++) rhythmOn_ = rhythmOn_ || rhythm_[i].on;
        break;
    case 0x11: reg_[0][r] = rhythmTotal_ = v & 0x3f; break;
    case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: {
        Rhythm& rh = rhythm_[r - 0x18];
        reg_[0][r] = v & 0xdf;
        rh.left = (v & 0x80) != 0;
        rh.right = (v & 0x40) != 0;
        rh.level = v & 0x1f;
        break;
    }
    default: break; // $12 はテスト用
    }
}

void Opn3::rhythmOutput(int32_t* out) {
    const RhythmSet* const set = rhythmSet;
    bool any = false;
    for (int i = 0; i < RHYTHMS; i++) {
        Rhythm& rh = rhythm_[i];
        if (!rh.on) continue;
        if (!set || !set->pcm[i] || rh.pos >= set->length[i]) {
            rh.on = false;
            continue;
        }
        any = true;
        // 音量: RTL と IL の減衰 (どちらも 0.75dB 刻み) を足す。16 ビットの音を FM の 1 スロットの最大に合わせる
        const int32_t vol = kRhythmVol[(63 - rhythmTotal_) + (31 - rh.level)];
        const int32_t v = (set->pcm[i][rh.pos] * vol) >> 14;
        if (rh.left) out[0] += v;
        if (rh.right) out[1] += v;
        rh.frac += rh.step;
        rh.pos += rh.frac >> 16;
        rh.frac &= 0xffff;
    }
    rhythmOn_ = any;
}

void Opn3::clockSample(int32_t* out) {
    clockLfo();
    if (pmDirty_) {
        pmDirty_ = false;
        for (int c = 0; c < 6; c++)
            if (ch_[c].pms) updateChannel(c);
    }
    bool egTick = false;
    if (++egDivider_ >= 3) {
        egDivider_ = 0;
        egCounter_ = (egCounter_ + 1) & 0xfff;
        egTick = true;
    }
    if (!fmIdle_) {
        bool allIdle = true;
        for (int c = 0; c < 6; c++) {
            Channel& ch = ch_[c];
            if (ch.idle) continue;
            if (ch.ssgEg)
                for (Op& op : ch.op) clockSsgEg(op);
            if (egTick) {
                bool idle = true;
                for (Op& op : ch.op) {
                    clockEnvelope(op);
                    if (op.state != EG_RELEASE || op.env < ENV_MAX) idle = false;
                }
                if (idle) { // 鳴り終わった。次のキーオンで位相は 0 に戻るので、位相も進めなくてよい
                    ch.idle = true;
                    ch.fbOut[0] = ch.fbOut[1] = 0;
                    continue;
                }
            }
            const int32_t v = channelOutput(ch);
            allIdle = false;
            if (c >= 3 && !sixCh_) continue; // $29 の SCH が 0 なら、チャネル 4-6 は鳴らない
            if (ch.left) out[0] += v;
            if (ch.right) out[1] += v;
        }
        fmIdle_ = allIdle;
    }
    if (ssgOn_) {
        const int32_t v = ssgOutput();
        out[0] += v;
        out[1] += v;
    } else {
        ssgAdvanceSilent();
    }
    if (rhythmOn_) rhythmOutput(out);
}

bool Opn3::stateOk() const {
    // bool のバイトは 0 か 1 (ほかの値を bool として読むと未定義の動作になるので、バイトで見る)
    const auto flag = [](const bool& b) {
        uint8_t v;
        memcpy(&v, &b, 1);
        return v <= 1;
    };
    if (bank_ > 1 || flags_ > 3 || mask_ > 3 || irqEnable_ > 3 || !flag(sixCh_) || !flag(ch3Special_) || !flag(fmIdle_))
        return false;
    // タイマーの残りは、書いたときに入れる値まで (大きな値だと、X68000 のソフトが待ち続ける)
    if (timerA_ > TIMER_A_CLOCKS * 1024 || timerB_ > TIMER_B_CLOCKS * 256) return false;
    if (lfoCount_ < 1 || lfoCount_ > 109 || lfoStep_ > 127 || lfoAm_ > 126 || !flag(lfoOn_) || !flag(pmDirty_)) return false;
    if (egCounter_ > 0xfff || egDivider_ < 0 || egDivider_ > 2) return false;
    for (const Channel& ch : ch_) {
        if (ch.fnum > 2047 || ch.block > 7 || ch.fb > 7 || ch.alg > 7 || ch.pms > 7 || ch.ams > 3 || ch.latch > 0x3f)
            return false;
        if (!flag(ch.left) || !flag(ch.right) || !flag(ch.idle) || !flag(ch.ssgEg)) return false;
        if (ch.fbOut[0] < -8192 || ch.fbOut[0] > 8192 || ch.fbOut[1] < -8192 || ch.fbOut[1] > 8192) return false;
        for (const Op& op : ch.op) {
            if (op.state > EG_RELEASE || !flag(op.key) || !flag(op.amEnable) || !flag(op.ssgInv)) return false;
            if (op.dt > 7 || op.mul > 15 || op.tl > 127 || op.ks > 3 || op.ar > 31 || op.dr > 31 || op.sr > 31 ||
                op.rr > 15 || op.sl > 15 || op.ssg > 15)
                return false;
            for (uint8_t r : op.rate)
                if (r > 63) return false;
            if (op.env < 0 || op.env > ENV_MAX || op.slLevel < 0 || op.slLevel > ENV_MAX || op.tlLevel < 0 ||
                op.tlLevel > ENV_MAX || op.phase > 0xfffff || op.step > 0xfffff)
                return false;
        }
    }
    for (int i = 0; i < 3; i++) {
        if (fnum3_[i] > 2047 || block3_[i] > 7 || latch3_[i] > 0x3f) return false;
        if (toneCount_[i] < 1 || toneCount_[i] > 4 * 4095 || toneOut_[i] > 1) return false;
    }
    if (noiseCount_ < 1 || noiseCount_ > 8 * 31 || noiseLfsr_ > 0x1ffff || noiseOut_ > 1) return false;
    if (envCount_ < 1 || envCount_ > 4 * 65535 || envPos_ > 31 || envFixed_ > 31 || !flag(envAttack_) || !flag(envHold_))
        return false;
    if (!flag(ssgOn_) || !flag(rhythmOn_) || rhythmTotal_ > 63) return false;
    for (const Rhythm& rh : rhythm_) {
        if (rh.frac > 0xffff || rh.level > 31 || !flag(rh.on) || !flag(rh.left) || !flag(rh.right)) return false;
    }
    return true;
}

} // namespace x68k
