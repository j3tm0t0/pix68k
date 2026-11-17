// 実時間に合わせて音のサンプルを取り出すときの数を決める (ベアメタル版の pix68k_sound=null が使う)。
// 前に呼んだ時刻から計算すると、1 サンプルの時間 (48kHz、1MHz のタイマで 20.83) の端数を切り捨てて時刻を
// 進めるので、実時間より多く取り出してしまう。始めた時刻と、それから取り出した総数から計算する。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)
#pragma once

#include <stdint.h>

namespace platform {

class SampleClock {
public:
    // ticksHz: 時刻の単位 (1 秒の数)、rate: サンプリング周波数
    SampleClock(uint32_t ticksHz, uint32_t rate) : ticksHz_(ticksHz), rate_(rate) {}
    void start(uint64_t now) {
        start_ = now;
        total_ = 0;
    }
    // now までに取り出すはずの数のうち、まだ取り出していない数を返す (返した数は取り出したことにする)。
    // maxDue を超えるほど遅れていたら (長く止まっていたら)、超えた分は取り出さずに捨てたことにする
    uint64_t due(uint64_t now, uint64_t maxDue) {
        const uint64_t should = (now - start_) * rate_ / ticksHz_;
        if (should <= total_) return 0;
        uint64_t d = should - total_;
        if (d > maxDue) {
            total_ += d - maxDue;
            d = maxDue;
        }
        total_ += d;
        return d;
    }
    uint64_t total() const { return total_; }

private:
    uint32_t ticksHz_, rate_;
    uint64_t start_ = 0;
    uint64_t total_ = 0; // 始めてから取り出した (捨てたものも含む) 数
};

} // namespace platform
