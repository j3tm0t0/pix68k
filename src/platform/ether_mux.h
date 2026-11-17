// 1 つの Ethernet を、Pi の TCP/IP (ネットワークの口。baremetal/host_net.h) と Neptune-X (baremetal/net_bridge.h) で
// 分け合うときの、受け取ったフレームの振り分け。宛先の MAC アドレスで決める:
//   Pi の MAC アドレス宛て → TCP/IP、X68000 の MAC アドレス宛て → Neptune-X、ブロードキャストとマルチキャスト → 両方、
//   ほか (プロミスキャスで届いた、ほかの機械宛て) → 捨てる
// 2 つの MAC アドレスは違うものにする (neptune_mac=pi は使えない)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stdint.h>
#include <string.h>

namespace platform {

enum EtherRoute : unsigned { ROUTE_DROP = 0, ROUTE_STACK = 1, ROUTE_BRIDGE = 2, ROUTE_BOTH = 3 };

inline unsigned routeFrame(const uint8_t* frame, unsigned len, const uint8_t stackMac[6], const uint8_t bridgeMac[6]) {
    if (len < 14) return ROUTE_DROP;
    if (frame[0] & 1) return ROUTE_BOTH; // ブロードキャストとマルチキャスト
    unsigned r = ROUTE_DROP;
    if (memcmp(frame, stackMac, 6) == 0) r |= ROUTE_STACK;
    if (memcmp(frame, bridgeMac, 6) == 0) r |= ROUTE_BRIDGE;
    return r;
}

// フレームを N 個まで溜める (コア 0 だけが使う。満杯なら新しいものを捨てる)
template <unsigned N, unsigned SIZE> class FrameQueue {
public:
    bool push(const void* data, unsigned len) {
        if (count_ == N || len > SIZE) {
            dropped_++;
            return false;
        }
        Slot& s = slots_[(head_ + count_) % N];
        memcpy(s.data, data, len);
        s.len = len;
        count_++;
        return true;
    }
    // 取り出して長さを返す (なければ 0)。out は SIZE バイト
    unsigned pop(void* out) {
        if (count_ == 0) return 0;
        Slot& s = slots_[head_];
        memcpy(out, s.data, s.len);
        head_ = (head_ + 1) % N;
        count_--;
        return s.len;
    }
    unsigned size() const { return count_; }
    unsigned dropped() const { return dropped_; }

private:
    struct Slot {
        uint8_t data[SIZE];
        unsigned len;
    };
    Slot slots_[N];
    unsigned head_ = 0, count_ = 0, dropped_ = 0;
};

} // namespace platform
