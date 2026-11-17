// RS-232C (SCC のチャネル A) の相手を、別のスレッド (コア) で動く送受信の口とつなぐリング。
// X68000 を動かすスレッド (コア) が Scc から send() / receive() / carrier() を呼び、ネットワークの側 (ホスト版は
// host/host_serial.cpp のスレッド、ベアメタル版はコア 0 の TCP のタスク) が pushRx() / popTx() / setCarrier() を呼ぶ。
// 向きごとに書き手と読み手が 1 つずつのロックのないリング。読み書きは __atomic_* で行う。
// ホスト版のネットワークの側は、受信のリングに空きがあるときだけソケットから読む (OS の TCP が相手を待たせる)。
// ベアメタル版は Circle が受信の窓を狭めないので、空きに関係なく読む (pumpRx())。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stdint.h>

#include "x68k/serial_link.h"

namespace platform {

template <unsigned N> // バイト数 (2 のべき乗)
class ByteRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of two");

public:
    // 書き手。満杯なら false (捨てる)
    bool push(uint8_t b) {
        const unsigned head = __atomic_load_n(&head_, __ATOMIC_RELAXED);
        const unsigned tail = __atomic_load_n(&tail_, __ATOMIC_ACQUIRE);
        if (head - tail >= N) return false;
        buf_[head & (N - 1)] = b;
        __atomic_store_n(&head_, head + 1, __ATOMIC_RELEASE);
        return true;
    }
    // 読み手。なければ -1
    int pop() {
        const unsigned tail = __atomic_load_n(&tail_, __ATOMIC_RELAXED);
        const unsigned head = __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
        if (head == tail) return -1;
        const uint8_t b = buf_[tail & (N - 1)];
        __atomic_store_n(&tail_, tail + 1, __ATOMIC_RELEASE);
        return b;
    }
    // 書き手から見た空き
    unsigned space() const {
        const unsigned head = __atomic_load_n(&head_, __ATOMIC_RELAXED);
        const unsigned tail = __atomic_load_n(&tail_, __ATOMIC_ACQUIRE);
        return N - (head - tail);
    }

private:
    uint8_t buf_[N];
    unsigned head_ = 0; // 書き手が進める
    unsigned tail_ = 0; // 読み手が進める
};

// RX / TX: 受信 (ネットワーク → X68000) と送信のリングのバイト数。ホスト版は OS の TCP が相手を待たせるので 4KB で足りる。
// ベアメタル版は Circle が TCP の受信の窓を狭めない (読まなくても相手は送り続ける) ので、受信のリングを大きくする
template <unsigned RX = 4096, unsigned TX = 4096>
class RingSerialLinkT : public x68k::SerialLink {
public:
    static constexpr unsigned SIZE = RX; // 受信のリングのバイト数

    // ---- X68000 の側 (x68k::SerialLink) ----
    void send(uint8_t b) override { tx_.push(b); }
    int receive() override {
        drainIfAsked();
        return rx_.pop();
    }
    // Scc は周辺機器を進めるたびに呼ぶので、ここで受信のリングを捨てる依頼も見る
    bool carrier() override {
        drainIfAsked();
        return __atomic_load_n(&carrier_, __ATOMIC_ACQUIRE);
    }

    // ---- ネットワークの側 ----
    bool pushRx(uint8_t b) { return rx_.push(b); }
    // 受信のリングの空き。捨てる依頼を X68000 の側がまだ済ませていなければ 0 (前の相手のバイトと混ぜない)
    unsigned rxSpace() const { return rxDrained() ? rx_.space() : 0; }
    int popTx() { return tx_.pop(); }
    void setCarrier(bool on) { __atomic_store_n(&carrier_, on, __ATOMIC_RELEASE); }
    // 次の相手がつながった。前の相手から届いて受信のリングに残っているバイトは、読み手である X68000 の側に捨ててもらう
    // (書き手のこちらから読み進めると、書き手と読み手が 1 つずつという前提が崩れる)
    void dropRx() { __atomic_add_fetch(&drainAsk_, 1u, __ATOMIC_RELEASE); }
    bool rxDrained() const {
        return __atomic_load_n(&drainDone_, __ATOMIC_ACQUIRE) == __atomic_load_n(&drainAsk_, __ATOMIC_RELAXED);
    }
    // 受信のリングの空きと、捨てる依頼が済んでいるかを、そろえて取る。済んでいるかを先に見てから空きを取るので、
    // 取ったあとに X68000 の側が捨て終えても、「済んでいないので空きは 0」と「済んでいて空きはこれだけ」の
    // どちらかになる (空きが 0 なのに済んでいる、と食い違うことはない)
    struct RxRoom {
        bool drained;
        unsigned space;
    };
    RxRoom rxRoom() const {
        return rxRoomWith([] {});
    }
    // between は 2 つの値を読む間に呼ぶ (試験で、ここで X68000 の側に捨て終えさせる。ふだんは何もしない)
    template <class F>
    RxRoom rxRoomWith(F between) const {
        RxRoom r;
        r.drained = rxDrained();
        between();
        r.space = r.drained ? rx_.space() : 0;
        return r;
    }

private:
    void drainIfAsked() {
        const unsigned ask = __atomic_load_n(&drainAsk_, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&drainDone_, __ATOMIC_RELAXED) == ask) return;
        while (rx_.pop() >= 0) {
        }
        __atomic_store_n(&drainDone_, ask, __ATOMIC_RELEASE);
    }
    ByteRing<RX> rx_; // ネットワーク → X68000
    ByteRing<TX> tx_; // X68000 → ネットワーク
    bool carrier_ = false;
    unsigned drainAsk_ = 0;  // ネットワークの側が増やす
    unsigned drainDone_ = 0; // X68000 の側が drainAsk_ に追いつかせる
};

using RingSerialLink = RingSerialLinkT<>;

// 相手が閉じた (hangup) ときに、ネットワークの側 (ホスト版) がすること。room は rxRoom() で一度に取った値。
//   READ:       空きがあるので読む (読み残しを読み切れば recv() が 0 を返して切る)。閉じていなくても READ
//   WAIT_DRAIN: 前の相手の読み残しを X68000 の側がまだ捨てていないだけ。切らずに待つ
//   DROP:       X68000 が受信を止めていてリングが満ちている。切る
enum class HangupAction { READ, WAIT_DRAIN, DROP };
template <class Room>
HangupAction onHangup(const Room& room, bool hangup) {
    if (!hangup || room.space > 0) return HangupAction::READ;
    return room.drained ? HangupAction::DROP : HangupAction::WAIT_DRAIN;
}

// ネットワークの側の 1 回分の受信。リングの空きに関係なく読む (相手が切れたことを見逃さないため)。入らないバイトは
// 捨てて dropped に足す。前の相手の読み残しを X68000 の側がまだ捨てていなければ読まない (混ぜない)。
// recv(buf, size) は読んだバイト数 (なければ 0)、切れたら負を返す。返り値: 読んだバイト数、切れたら -1
template <class Link, class Recv>
int pumpRx(Link& link, uint8_t* buf, unsigned size, Recv recv, unsigned& dropped) {
    if (!link.rxDrained()) return 0;
    const int n = recv(buf, size);
    if (n < 0) return -1;
    for (int i = 0; i < n; i++)
        if (!link.pushRx(buf[i])) dropped++;
    return n;
}

// ネットワークの側の 1 回分の送信。txReady (相手に送れる。送信待ちが上限より少ない) のときだけ、リングから最大 size バイト
// 取り出して send(buf, len) に渡す。send は切れたら負を返す。返り値: 渡したバイト数、切れたら -1
template <class Link, class Send>
int pumpTx(Link& link, uint8_t* buf, unsigned size, bool txReady, Send send) {
    if (!txReady) return 0;
    unsigned len = 0;
    int b;
    while (len < size && (b = link.popTx()) >= 0) buf[len++] = (uint8_t)b;
    if (len == 0) return 0;
    return send(buf, len) < 0 ? -1 : (int)len;
}

} // namespace platform
