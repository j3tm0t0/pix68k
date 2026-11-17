#include "x68k/adpcm.h"

namespace x68k {

namespace {

// 量子化幅 (49 段)。16 x 1.1^n の切り捨て (adpcm_test で確かめる)
const int16_t kStepSize[Adpcm::STEP_MAX + 1] = {
    16,  17,  19,  21,  23,  25,  28,  31,  34,  37,  41,  45,   50,   55,   60,   66,  73,
    80,  88,  97,  107, 118, 130, 143, 157, 173, 190, 209, 230,  253,  279,  307,  337, 371,
    408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
};

// 符号の下位 3 ビット (大きさ) ごとの、段の動かし方
const int8_t kStepMove[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

} // namespace

// OKI の方式の復号 (データブックの「ADPCM reproduction」):
//   差 = (-1)^B3 x (4Δ x B2 + 2Δ x B1 + Δ x B0 + Δ / 2)、波形 = 前の波形 + 差、次の Δ = Δ x M(符号)
// 表の量子化幅は 4Δ に当たるので、差 = 幅 x B2 + 幅 / 2 x B1 + 幅 / 4 x B0 + 幅 / 8 (それぞれ切り捨て)。
// 波形は 12 ビットで、あふれないように止める (MSM6258 の説明の「bit overflow protection」)
int32_t Adpcm::decode(uint8_t nibble, int32_t& signal, int32_t& step) {
    if (step < 0) step = 0;
    if (step > STEP_MAX) step = STEP_MAX;
    const int32_t size = kStepSize[step];
    int32_t diff = size >> 3;
    if (nibble & 4) diff += size;
    if (nibble & 2) diff += size >> 1;
    if (nibble & 1) diff += size >> 2;
    signal += (nibble & 8) ? -diff : diff;
    if (signal > 2047) signal = 2047;
    if (signal < -2048) signal = -2048;
    step += kStepMove[nibble & 7];
    if (step < 0) step = 0;
    if (step > STEP_MAX) step = STEP_MAX;
    return signal;
}

// 電源投入 / リセット。発振と左右、周波数の選択 (ほかの LSI の出力) は変えない
void Adpcm::reset() {
    playing_ = false;
    drq_ = false;
    latched_ = false;
    second_ = false;
    latch_ = 0;
    current_ = 0;
    signal_ = 0;
    step_ = 0;
    elapsed_ = 0;
}

uint32_t Adpcm::period() const {
    static const uint16_t kDivide[4] = {1024, 768, 512, 512};
    return (uint32_t)kDivide[rate_] * (clock4M_ ? 2u : 1u);
}

uint8_t Adpcm::read(uint32_t offset) const {
    switch (offset & 3) {
    case 1: // ステータス: 再生中に $40、止まっていれば $C0 (bit7 は再生中に 0。adpcm.h の説明)
        return playing_ ? 0x40 : 0xc0;
    case 3: // データ (録音したデータ。録音は作っていない)
        return 0x00;
    default: // 偶数番地には何もつながっていない
        return 0xff;
    }
}

void Adpcm::write(uint32_t offset, uint8_t v) {
    switch (offset & 3) {
    case 1: // コマンド
        if (v & 0x01) {
            // SP: 止める。出力は止まったときの値を保つ
            playing_ = false;
            drq_ = false;
            latched_ = false;
        } else if (v & 0x02) {
            // PLAY ST: 再生を始める。出力は波形の中心に戻り、最初のバイトを求める。再生中でも始め直す (adpcm.h)
            playing_ = true;
            signal_ = 0; // 波形の中心
            step_ = 0;
            second_ = false;
            latched_ = false;
            elapsed_ = 0;
            drq_ = true;
        } else if (v & 0x04) {
            // REC ST: 録音は作っていない。再生は止まる
            playing_ = false;
            drq_ = false;
            latched_ = false;
        }
        break;
    case 3: // データ: 再生中だけ受け取る
        if (!playing_) break;
        latch_ = v;
        latched_ = true;
        drq_ = false;
        break;
    default:
        break;
    }
}

// 1 サンプル。バイトの 1 つ目のサンプルのところで書かれたバイトを取り込み、次のバイトを求める
void Adpcm::sample() {
    if (!second_) {
        if (!latched_) {
            // データが来ていない: 出力を保って待つ
            drq_ = true;
            return;
        }
        current_ = latch_;
        latched_ = false;
        drq_ = true;
        decode(current_ & 0x0f, signal_, step_);
        second_ = true;
    } else {
        decode(current_ >> 4, signal_, step_);
        second_ = false;
    }
}

void Adpcm::advance(uint32_t clocks) {
    if (!playing_) {
        elapsed_ = 0;
        return;
    }
    elapsed_ += clocks * 2; // 4MHz のクロックを 8MHz の数に直す
    const uint32_t p = period();
    while (elapsed_ >= p && playing_) {
        elapsed_ -= p;
        sample();
    }
}

} // namespace x68k
