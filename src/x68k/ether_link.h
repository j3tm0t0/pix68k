// イーサネットのフレームをやり取りする口。ネットワークボード (x68k/neptune.h) が使い、実際の送受信は環境が受け持つ
// (ベアメタル版は Circle の CNetDevice、ホスト版は試験用の応答役や pcap / tap)。
// フレームは宛先の MAC アドレスから始まり、FCS (CRC) を含まない。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stdint.h>

namespace x68k {

class EtherLink {
public:
    static constexpr int MAX_FRAME = 1514; // FCS を除いた最大の長さ (宛先 6 + 送り元 6 + 種類 2 + データ 1500)
    static constexpr int MIN_FRAME = 14;   // 宛先、送り元、種類だけ

    virtual ~EtherLink() {}
    // フレームを送る (len は MIN_FRAME..MAX_FRAME)。送れなければ捨ててよい (実機の線でも落ちることがある)
    virtual void send(const uint8_t* frame, int len) = 0;
    // 受け取ったフレームがあれば buf (cap バイト。MAX_FRAME 以上) に入れて長さを返す。なければ 0。
    // X68000 を動かしているスレッド (コア) から呼ぶので、待たずに戻ること
    virtual int receive(uint8_t* buf, int cap) = 0;
};

} // namespace x68k
