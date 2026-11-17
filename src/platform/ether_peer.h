// 試験用のフレームの送受信の口 (x68k::EtherLink)。本物のネットワークにつながずに、Neptune-X のドライバと TCP/IP を動かす。
//   EchoPeer      線の向こうに 1 台だけ機械がいるものとして、ARP の問い合わせ (どの IP アドレスでも) と
//                 ICMP の echo request (ping) に答える。答えの送り元の MAC アドレスは PEER_MAC
//   LoopbackLink  送ったフレームをそのまま返す (宛ての MAC アドレスが自分でなければ、ボードのフィルタで捨てられる)
// どちらも X68000 を動かすスレッド (コア) の中だけで使う。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stdint.h>
#include <string.h>

#include "platform/frame_ring.h"
#include "x68k/ether_link.h"

namespace platform {

class EchoPeer : public x68k::EtherLink {
public:
    static constexpr uint8_t PEER_MAC[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0xfe};

    void send(const uint8_t* f, int len) override {
        if (len < 14) return;
        const unsigned type = (unsigned)f[12] << 8 | f[13];
        if (type == 0x0806) arp(f, len);
        else if (type == 0x0800) ipv4(f, len);
    }
    int receive(uint8_t* buf, int cap) override { return replies_.pop(buf, cap); }
    unsigned arpReplies() const { return arpReplies_; }
    unsigned echoReplies() const { return echoReplies_; }

private:
    static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
    static void put16(uint8_t* p, unsigned v) {
        p[0] = (uint8_t)(v >> 8);
        p[1] = (uint8_t)v;
    }
    // インターネットのチェックサム (16 ビットの 1 の補数の和)
    static uint16_t checksum(const uint8_t* p, int n) {
        uint32_t sum = 0;
        for (int i = 0; i + 1 < n; i += 2) sum += get16(p + i);
        if (n & 1) sum += (uint32_t)p[n - 1] << 8;
        while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
        return (uint16_t)~sum;
    }

    // ARP の問い合わせ (イーサネット、IPv4) に、問い合わせた IP アドレスの持ち主として答える
    void arp(const uint8_t* f, int len) {
        if (len < 42) return;
        const uint8_t* a = f + 14;
        if (get16(a) != 1 || get16(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4 || get16(a + 6) != 1) return;
        if (memcmp(a + 14, a + 24, 4) == 0) return; // 自分のアドレスの知らせ (gratuitous ARP)
        static const uint8_t kZero[4] = {0, 0, 0, 0};
        if (memcmp(a + 14, kZero, 4) == 0) return; // 送り元が 0.0.0.0: アドレスが重なっていないかの確認 (ARP probe)
        uint8_t r[42];
        memcpy(r, f + 6, 6);
        memcpy(r + 6, PEER_MAC, 6);
        put16(r + 12, 0x0806);
        uint8_t* b = r + 14;
        put16(b, 1);
        put16(b + 2, 0x0800);
        b[4] = 6;
        b[5] = 4;
        put16(b + 6, 2);
        memcpy(b + 8, PEER_MAC, 6); // 送り元のハードウェアアドレス
        memcpy(b + 14, a + 24, 4);  // 送り元の IP アドレス = 問い合わせられたアドレス
        memcpy(b + 18, a + 8, 6);   // 宛てのハードウェアアドレス = 問い合わせた機械
        memcpy(b + 24, a + 14, 4);
        if (replies_.push(r, (int)sizeof r)) arpReplies_++;
    }

    // ICMP の echo request に echo reply を返す
    void ipv4(const uint8_t* f, int len) {
        const uint8_t* ip = f + 14;
        if (len < 34 || (ip[0] >> 4) != 4) return;
        const int ihl = (ip[0] & 0x0f) * 4;
        const int total = get16(ip + 2);
        if (ihl < 20 || total < ihl + 8 || 14 + total > len || ip[9] != 1) return;
        if (get16(ip + 6) & 0x3fff) return; // 分割したパケットは扱わない
        if (ip[ihl] != 8) return;           // echo request だけ
        uint8_t r[x68k::EtherLink::MAX_FRAME];
        const int n = 14 + total;
        memcpy(r, f + 6, 6);
        memcpy(r + 6, PEER_MAC, 6);
        memcpy(r + 12, f + 12, (size_t)(n - 12));
        uint8_t* rip = r + 14;
        memcpy(rip + 12, ip + 16, 4); // 送り元と宛てを入れ替える
        memcpy(rip + 16, ip + 12, 4);
        rip[8] = 64; // TTL
        put16(rip + 10, 0);
        put16(rip + 10, checksum(rip, ihl));
        uint8_t* icmp = rip + ihl;
        icmp[0] = 0; // echo reply
        put16(icmp + 2, 0);
        put16(icmp + 2, checksum(icmp, total - ihl));
        if (replies_.push(r, n)) echoReplies_++;
    }

    FrameRing<8> replies_;
    unsigned arpReplies_ = 0, echoReplies_ = 0;
};

class LoopbackLink : public x68k::EtherLink {
public:
    void send(const uint8_t* f, int len) override { frames_.push(f, len); }
    int receive(uint8_t* buf, int cap) override { return frames_.pop(buf, cap); }

private:
    FrameRing<8> frames_;
};

} // namespace platform
