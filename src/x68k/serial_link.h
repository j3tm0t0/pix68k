// RS-232C (SCC のチャネル A) の相手。Scc が送り終えたバイトを渡し、届いたバイトを受け取る。実際の送受信は環境が
// 受け持つ (ホスト版は TCP の待ち受け、ベアメタル版は Circle の TCP。どちらも platform/serial_ring.h のリングでつなぐ)。
// 線の速さ (ボーレート) は Scc が数える。ここは速さを持たないバイトの出入り口。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stdint.h>

namespace x68k {

class SerialLink {
public:
    virtual ~SerialLink() {}
    // 送り終えた 1 バイト。渡せなければ捨ててよい (相手がいない線に送ったのと同じ)
    virtual void send(uint8_t b) = 0;
    // 届いたバイトがあれば 1 つ取り出して返す。なければ -1。X68000 を動かすスレッド (コア) から呼ぶので、待たずに戻ること
    virtual int receive() = 0;
    // 相手がつながっている (DCD と CTS を出している)。TCP なら接続している間
    virtual bool carrier() = 0;
};

} // namespace x68k
