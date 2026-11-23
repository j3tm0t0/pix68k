// FUK (x68k/fuk.h) の USB の送受信を、X68000 を動かすコアと USB を扱うコアの間で受け渡すバイトのリング (ベアメタル版)。
// 書き手と読み手が 1 つずつのロックのないリングで、まとめて読み書きする (platform/serial_ring.h の ByteRing は
// 1 バイトずつ。'R' のデータは 8KB ほどあるので、まとめて写す)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stdint.h>
#include <string.h>

namespace platform {

template <unsigned N> // バイト数 (2 のべき乗)
class FukRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of two");

public:
    // 書き手: 入るだけ書いて、書いたバイト数を返す
    unsigned write(const uint8_t* data, unsigned n) {
        const unsigned head = __atomic_load_n(&head_, __ATOMIC_RELAXED);
        const unsigned tail = __atomic_load_n(&tail_, __ATOMIC_ACQUIRE);
        const unsigned room = N - (head - tail);
        if (n > room) n = room;
        const unsigned at = head & (N - 1);
        const unsigned first = n < N - at ? n : N - at;
        memcpy(buf_ + at, data, first);
        memcpy(buf_, data + first, n - first);
        __atomic_store_n(&head_, head + n, __ATOMIC_RELEASE);
        return n;
    }
    // 読み手: max バイトまで読んで、読んだバイト数を返す
    unsigned read(uint8_t* dst, unsigned max) {
        const unsigned tail = __atomic_load_n(&tail_, __ATOMIC_RELAXED);
        const unsigned head = __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
        unsigned n = head - tail;
        if (n > max) n = max;
        const unsigned at = tail & (N - 1);
        const unsigned first = n < N - at ? n : N - at;
        memcpy(dst, buf_ + at, first);
        memcpy(dst + first, buf_, n - first);
        __atomic_store_n(&tail_, tail + n, __ATOMIC_RELEASE);
        return n;
    }
    // 入っているバイト数 (どちらから呼んでもよい。呼んだあとに変わりうる)
    unsigned used() const { return __atomic_load_n(&head_, __ATOMIC_ACQUIRE) - __atomic_load_n(&tail_, __ATOMIC_ACQUIRE); }
    // 読み手: 入っているものを捨てる
    void drop() { __atomic_store_n(&tail_, __atomic_load_n(&head_, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE); }

private:
    uint8_t buf_[N];
    unsigned head_ = 0; // 書き手が進める
    unsigned tail_ = 0; // 読み手が進める
};

// FukRing に書いたバイトが届いた時刻 (#320)。書き手は、書いたあとの書いたバイト数の合計 (end) と時刻を入れる。
// 読み手は、読んだバイト数の合計を渡して、最後に読んだバイトの時刻を引く。一杯なら入れない (読み手は、あとの区切りの
// 時刻を使う。遅いほうの時刻になる)
template <unsigned M> // 区切りの数 (2 のべき乗)
class FukStampRing {
    static_assert(M >= 2 && (M & (M - 1)) == 0, "M must be a power of two");

public:
    // 書き手
    void push(unsigned end, uint64_t us) {
        const unsigned head = __atomic_load_n(&head_, __ATOMIC_RELAXED);
        const unsigned tail = __atomic_load_n(&tail_, __ATOMIC_ACQUIRE);
        if (head - tail >= M) {
            __atomic_store_n(&dropped_, dropped_ + 1, __ATOMIC_RELAXED); // 書き手だけが増やす
            return;
        }
        e_[head & (M - 1)] = Entry{end, us};
        __atomic_store_n(&head_, head + 1, __ATOMIC_RELEASE);
    }
    // 読み手: 読んだバイト数の合計が read のとき、次のバイト (read 番目) と同じ区切りに入っているバイト数 (同じ時刻に
    // 届いたもの)。区切りが分からなければ max (一杯で入れなかった区切りは、あとの区切りとまとめて読む)。
    // max には、この呼び出しより前に FukRing::used() で見たバイト数 (を上限にした値) を渡す。書き手はバイトより先に
    // 時刻を入れるので、そのバイトの区切りはここで必ず見える。あとから届いたバイトまで読んで、違う時刻のまとまりを
    // 1 回で返すことはない
    unsigned limit(unsigned read, unsigned max) {
        unsigned tail = __atomic_load_n(&tail_, __ATOMIC_RELAXED);
        const unsigned head = __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
        while (tail != head && (int)(e_[tail & (M - 1)].end - read) <= 0) tail++;
        if (tail == head) return max;
        const unsigned n = e_[tail & (M - 1)].end - read;
        return n < max ? n : max;
    }
    // 一杯で入れなかった区切りの数 (どちらから呼んでもよい)
    unsigned dropped() const { return __atomic_load_n(&dropped_, __ATOMIC_RELAXED); }
    // 読み手: 読んだバイト数の合計が read のとき、最後に読んだバイト (read - 1 番目) の時刻。分からなければ fallback
    uint64_t lookup(unsigned read, uint64_t fallback) {
        unsigned tail = __atomic_load_n(&tail_, __ATOMIC_RELAXED);
        const unsigned head = __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
        // 読み終えた区切り (end < read) を捨てる。区切りは end の前までのバイト
        while (tail != head && (int)(e_[tail & (M - 1)].end - read) < 0) tail++;
        __atomic_store_n(&tail_, tail, __ATOMIC_RELEASE);
        return tail != head ? e_[tail & (M - 1)].us : fallback;
    }

private:
    struct Entry {
        unsigned end;
        uint64_t us;
    };
    Entry e_[M];
    unsigned head_ = 0;
    unsigned tail_ = 0;
    unsigned dropped_ = 0;
};

} // namespace platform
