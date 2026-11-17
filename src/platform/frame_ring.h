// イーサネットのフレームを、1 つの書き手から 1 つの読み手へ渡すロックのないリング (コア間、スレッド間)。
// ベアメタル版はコア 0 (Circle のネットワークデバイス) とコア 1 (X68000) の間で、送る向きと受け取る向きに 1 つずつ使う。
// ホスト版は pcap / tap を読むスレッドから X68000 を動かすスレッドへ渡すのに使う。
// 満杯なら書き手のフレームを捨てる (線の上で落ちたのと同じ)。読み書きは __atomic_* で行う。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stdint.h>
#include <string.h>

#include "x68k/ether_link.h"

namespace platform {

template <unsigned N> // フレームの数 (2 のべき乗)
class FrameRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of two");

public:
    static constexpr int SLOT = 1536; // 1 フレームの場所 (EtherLink::MAX_FRAME 以上)

    // 書き手。満杯か長さが合わなければ false (捨てる)
    bool push(const uint8_t* frame, int len) {
        if (len <= 0 || len > SLOT) return false;
        const unsigned head = __atomic_load_n(&head_, __ATOMIC_RELAXED);
        const unsigned tail = __atomic_load_n(&tail_, __ATOMIC_ACQUIRE);
        if (head - tail >= N) {
            __atomic_fetch_add(&overflows_, 1u, __ATOMIC_RELAXED);
            return false;
        }
        Slot& s = slots_[head & (N - 1)];
        memcpy(s.data, frame, (size_t)len);
        s.len = len;
        __atomic_store_n(&head_, head + 1, __ATOMIC_RELEASE);
        return true;
    }
    // 読み手。なければ 0。buf は cap バイト (足りなければそのフレームを捨てて 0)
    int pop(uint8_t* buf, int cap) {
        const unsigned tail = __atomic_load_n(&tail_, __ATOMIC_RELAXED);
        const unsigned head = __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
        if (head == tail) return 0;
        const Slot& s = slots_[tail & (N - 1)];
        int len = s.len;
        if (len > cap) len = 0;
        else memcpy(buf, s.data, (size_t)len);
        __atomic_store_n(&tail_, tail + 1, __ATOMIC_RELEASE);
        return len;
    }
    bool empty() const {
        return __atomic_load_n(&head_, __ATOMIC_ACQUIRE) == __atomic_load_n(&tail_, __ATOMIC_ACQUIRE);
    }
    // 満杯で捨てた数
    unsigned overflows() const { return __atomic_load_n(&overflows_, __ATOMIC_RELAXED); }

private:
    // len は 0 で始める。pop() は head_ != tail_ のとき (push() が書いたあと) にしか読まないが、GCC の
    // -Wmaybe-uninitialized はそれを追えず、初期化していない値を読むかもしれないと警告するため
    struct Slot {
        int len = 0;
        uint8_t data[SLOT];
    };
    Slot slots_[N];
    // head_ と tail_ は別のキャッシュライン (64 バイト) に置く。alignas はベアメタルの new が揃えないので使わず、
    // 間を 64 バイト空ける
    unsigned head_ = 0;      // 書き手だけが書く
    unsigned overflows_ = 0; // 書き手だけが増やす
    char pad_[64] = {};
    unsigned tail_ = 0;      // 読み手だけが書く
};

// X68000 の側 (EtherLink) から見た 2 つのリング: send() は tx に入れ、receive() は rx から取り出す
template <unsigned N> class RingEtherLink : public x68k::EtherLink {
public:
    FrameRing<N> tx; // X68000 → 環境
    FrameRing<N> rx; // 環境 → X68000
    void send(const uint8_t* frame, int len) override { tx.push(frame, len); }
    int receive(uint8_t* buf, int cap) override { return rx.pop(buf, cap); }
};

} // namespace platform
