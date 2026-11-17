// ネットワークで画面の写し (/screen.ppm) を返すときに、フレームを待つ時間の数え方 (ベアメタル版の
// CKernel::WaitSnapshotFrames() が使う)。待つ時間の上限 (budget) に数えるのは、フレームを待っている間
// (platform::FrameHandoff::takeWhileHanded() の wait() を呼んでいる間) だけにする。合成するコアが写し終えるのを
// 待つ間 (end())、受け取ったフレームを写す時間、区切ったあとの SD カードの読み書き (ServiceDisks()) の時間は数えない。
// ディスクの読み書きが残っていれば (ioPending)、1 回の区間 (slice) を待ってもフレームが来ないときに区切る。
// 時刻の単位は呼び手が決める (同じ単位で渡す)。ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stdint.h>

namespace platform {

class SnapshotWait {
public:
    SnapshotWait(uint64_t budget, uint64_t slice) : budget_(budget), slice_(slice) {}
    // takeWhileHanded() を呼ぶ前に呼ぶ (区間を始める)
    void beginSlice() { started_ = false; }
    // フレームを待つ間 (takeWhileHanded() の wait()) に呼ぶ。待ち続けるなら true。
    // 上限を使い切ったら false。区間の長さ以上待って、ディスクの読み書きが残っていれば false (区切る)
    bool keepWaiting(uint64_t now, bool ioPending) {
        if (!started_) {
            started_ = true;
            start_ = now;
        }
        const uint64_t elapsed = now - start_;
        if (used_ + elapsed >= budget_) return false;
        return !(elapsed >= slice_ && ioPending);
    }
    // takeWhileHanded() から戻ったら呼ぶ (この区間でフレームを待った時間を、使った時間に足す)
    void endSlice(uint64_t now) {
        if (started_) used_ += now - start_;
        started_ = false;
    }
    // 上限を使い切った (フレームを待った時間の合計が budget 以上)
    bool expired() const { return used_ >= budget_; }
    uint64_t used() const { return used_; }

private:
    uint64_t budget_, slice_;
    uint64_t used_ = 0, start_ = 0;
    bool started_ = false;
};

} // namespace platform
