// 画面や音を合成するコア (書き手) が、合成に使った時間をフレームごとに足し、ログを出すコア (読み手) が
// 区間ごとに受け取るための統計。合計の時間、フレームの数、最大の 3 つを、同じ区間のものとして受け取る。
//
//   書き手: 1 フレーム (か、何フレームか) を終えるたびに addFrames() を呼ぶ
//   読み手: take() を呼ぶ。初めて呼ぶと区切りを頼んで false を返し、書き手が次の addFrames() で区切ったあとに
//           呼ぶと、その区間を返して true (platform/interval_handoff.h と同じ)
//
// 区間を区切るのは書き手なので、読み手が 3 つの値を別々のときに読んで食い違うことや、読み手が最大を 0 に戻して
// 書き手が足したばかりの値を消すことがない。ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)
#pragma once

#include <stdint.h>

#include "platform/interval_handoff.h"

namespace platform {

struct CoreWork {
    uint64_t totalUs = 0; // 使った時間の合計
    uint32_t frames = 0;  // フレームの数
    uint32_t maxUs = 0;   // 1 フレームに使った時間の最大 (まとめて数えた分は割った値。addFrames() の説明を参照)
    uint32_t averageUs() const { return frames != 0 ? (uint32_t)(totalUs / frames) : 0; }
};

class CoreWorkMeter {
public:
    // ---- 書き手 ----
    // frames フレーム (1 以上) の間に busyUs 使った。何フレームかまとめて数えたとき (合成するコアが遅れて、
    // フレームの区切りを 1 度に 2 つ以上見つけたとき) は、busyUs を frames で割った値を 1 フレームの時間として
    // 最大と比べる。そのフレームのうちのどれかが実際に使った時間の最大ではない (それより小さくなる)
    void addFrames(uint64_t busyUs, uint32_t frames) {
        CoreWork& w = handoff_.current();
        w.totalUs += busyUs;
        w.frames += frames;
        const uint64_t per = busyUs / frames;
        if (per > w.maxUs) w.maxUs = per > 0xffffffffu ? 0xffffffffu : (uint32_t)per;
        handoff_.endSample();
    }

    // ---- 読み手 ----
    bool take(CoreWork& out) { return handoff_.take(out); }

private:
    IntervalHandoff<CoreWork> handoff_;
};

} // namespace platform
