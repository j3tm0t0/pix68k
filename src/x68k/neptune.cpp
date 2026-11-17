// ネットワークボード Neptune-X (NE2000 互換、DP8390)。説明は neptune.h
#include "x68k/neptune.h"

#include <string.h>

namespace x68k {

constexpr uint8_t Neptune::DEFAULT_MAC[6];

namespace {

// FCS に使う CRC-32 (IEEE 802.3。ビットを下位から送る)
uint32_t fcs32(const uint8_t* p, int n) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xffffffffu;
    for (int i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

// マルチキャストのハッシュの番号 (0..63)。DP8390 は宛先の 6 バイトの CRC を、上位のビットから多項式 $04C11DB7 で
// 計算し (データのビットは下位から入れる)、上位 6 ビットで MAR の 64 ビットの 1 つを選ぶ
int multicastHash(const uint8_t* addr) {
    uint32_t crc = 0xffffffffu;
    for (int i = 0; i < 6; i++) {
        uint8_t b = addr[i];
        for (int k = 0; k < 8; k++, b >>= 1) {
            const uint32_t carry = (crc >> 31) ^ (b & 1u);
            crc <<= 1;
            if (carry) crc ^= 0x04c11db7u;
        }
    }
    return (int)(crc >> 26);
}

} // namespace

Neptune::Neptune() {
    setMac(DEFAULT_MAC);
    reset();
}

void Neptune::setMac(const uint8_t mac[6]) {
    memset(prom_, 0, sizeof prom_);
    memcpy(prom_, mac, 6);
    prom_[14] = prom_[15] = 0x57; // 'W'。NE2000 か NE1000 かを、ドライバがこの値で見分けることがある
}

void Neptune::reset() {
    cr_ = CR_STP | 0x20; // 止まっていて、リモート DMA は中止 (送っているフレームも中止)
    txRemainNs_ = 0;
    txFirstPoll_ = false;
    isr_ = ISR_RST;
    imr_ = 0;
    overflowRst_ = false;
    dmaCount_ = 0;
}

// ---------------------------------------------------------------- CPU からの読み書き

int Neptune::read(uint32_t off) {
    if (off >= WINDOW) return -1;
    if (off < NIC_OFFSET || off >= NIC_OFFSET + 0x40) return 0xff; // NE2000 のないポート
    if (off == DATA_OFFSET + 1) return dataLatch_;
    if (off & 1) return 0xff;
    const int port = (int)(off - NIC_OFFSET) >> 1;
    if (port < 0x10) return regRead(port);
    if (port == 0x10) {
        // データポート。WTS = 1 なら 2 バイト転送し、下位バイトを覚えておく
        if (dcr_ & DCR_WTS) {
            const uint8_t b0 = dmaReadByte(), b1 = dmaReadByte();
            const bool bos = (dcr_ & DCR_BOS) != 0;
            dataLatch_ = bos ? b0 : b1;
            return bos ? b1 : b0;
        }
        return dmaReadByte();
    }
    if (port == 0x1f) { // リセットポート: 読むと DP8390 をリセットする
        reset();
        return 0x00;
    }
    return 0xff;
}

bool Neptune::write(uint32_t off, uint8_t v) {
    if (off >= WINDOW) return false;
    if (off < NIC_OFFSET || off >= NIC_OFFSET + 0x40) return true;
    if (off == DATA_OFFSET + 1) {
        // データポートの下位バイト。WTS = 1 なら、覚えておいた上位バイトと合わせて 2 バイト書く
        if (dcr_ & DCR_WTS) {
            const bool bos = (dcr_ & DCR_BOS) != 0;
            dmaWriteByte(bos ? v : dataLatch_);
            dmaWriteByte(bos ? dataLatch_ : v);
        }
        return true;
    }
    if (off & 1) return true;
    const int port = (int)(off - NIC_OFFSET) >> 1;
    if (port < 0x10) {
        regWrite(port, v);
    } else if (port == 0x10) {
        if (dcr_ & DCR_WTS) dataLatch_ = v; // 下位バイトを書いたときに 2 バイト書く
        else dmaWriteByte(v);
    }
    return true; // リセットポートとほかのポートは書いても何も起きない
}

uint8_t Neptune::regRead(int r) {
    if (r == 0) return cr_;
    switch (cr_ >> 6) {
    case 0:
        switch (r) {
        case 0x01: return 0;           // CLDA0 (受信を書いている途中はないので、ページの先頭)
        case 0x02: return curr_;       // CLDA1
        case 0x03: return bnry_;
        case 0x04: return tsr_;
        case 0x05: return ncr_;
        case 0x06: {                   // FIFO (ループバックで受けたフレームの最後の 8 バイトを順に読む)
            const uint8_t v = fifo_[fifoPos_ & 7];
            fifoPos_++;
            return v;
        }
        case 0x07: return isr_;
        case 0x08: return (uint8_t)dmaAddr_;
        case 0x09: return (uint8_t)(dmaAddr_ >> 8);
        case 0x0c: return rsr_;
        case 0x0d: case 0x0e: case 0x0f: { // 数を読むと 0 に戻る
            const uint8_t v = cntr_[r - 0x0d];
            cntr_[r - 0x0d] = 0;
            return v;
        }
        default: return 0xff; // $0A、$0B は使わない番号
        }
    case 1:
        if (r <= 0x06) return par_[r - 1];
        if (r == 0x07) return curr_;
        return mar_[r - 8];
    case 2:
        switch (r) {
        case 0x01: return pstart_;
        case 0x02: return pstop_;
        case 0x03: return rnpp_;
        case 0x04: return tpsr_;
        case 0x05: return lnpp_;
        case 0x06: return (uint8_t)(dmaAddr_ >> 8);
        case 0x07: return (uint8_t)dmaAddr_;
        case 0x0c: return rcr_ | 0xc0;
        case 0x0d: return tcr_ | 0xe0;
        case 0x0e: return dcr_ | 0x80;
        case 0x0f: return imr_ | 0x80;
        default: return 0xff;
        }
    default: return 0xff; // ページ 3 は試験用で、使わない
    }
}

void Neptune::regWrite(int r, uint8_t v) {
    if (r == 0) {
        writeCommand(v);
        return;
    }
    switch (cr_ >> 6) {
    case 0:
        switch (r) {
        case 0x01: pstart_ = v; break;
        case 0x02: pstop_ = v; break;
        case 0x03:
            bnry_ = v;
            // あふれて立てた RST は、ドライバがリングからフレームを取り出す (BNRY を進める) と消える
            if (overflowRst_) {
                overflowRst_ = false;
                if (!(cr_ & CR_STP)) isr_ &= (uint8_t)~ISR_RST;
            }
            break;
        case 0x04: tpsr_ = v; break;
        case 0x05: tbcr_ = (uint16_t)((tbcr_ & 0xff00) | v); break;
        case 0x06: tbcr_ = (uint16_t)((tbcr_ & 0x00ff) | v << 8); break;
        case 0x07: isr_ &= (uint8_t)~(v & 0x7f); break; // 1 を書いたビットを消す (RST は書いても変わらない)
        // リモート DMA の開始アドレスとバイト数は、書いたときに今の値にもする (RD を書く前でも後でも同じに働く)
        case 0x08: rsar_ = (uint16_t)((rsar_ & 0xff00) | v); dmaAddr_ = rsar_; break;
        case 0x09: rsar_ = (uint16_t)((rsar_ & 0x00ff) | v << 8); dmaAddr_ = rsar_; break;
        case 0x0a: rbcr_ = (uint16_t)((rbcr_ & 0xff00) | v); dmaCount_ = rbcr_; break;
        case 0x0b: rbcr_ = (uint16_t)((rbcr_ & 0x00ff) | v << 8); dmaCount_ = rbcr_; break;
        case 0x0c: rcr_ = v & 0x3f; break;
        case 0x0d: tcr_ = v & 0x1f; break;
        case 0x0e: dcr_ = v & 0x7f; break;
        case 0x0f: imr_ = v & 0x7f; break;
        }
        break;
    case 1:
        if (r <= 0x06) par_[r - 1] = v;
        else if (r == 0x07) curr_ = v;
        else mar_[r - 8] = v;
        break;
    case 2:
        // 試験用のレジスタ。次のフレームのページだけ覚える (ほかは書いても何も起きない)
        if (r == 0x03) rnpp_ = v;
        else if (r == 0x05) lnpp_ = v;
        break;
    default: break;
    }
}

void Neptune::writeCommand(uint8_t v) {
    const int rd = (v >> 3) & 7;
    const bool busy = (cr_ & CR_TXP) != 0;
    // TXP は送り終えるまで 1 のまま (0 を書いても送るのは止まらない)
    cr_ = (uint8_t)((v & ~CR_TXP) | (busy ? CR_TXP : 0));
    if (v & CR_STP) {
        cr_ &= (uint8_t)~CR_STA;
        isr_ |= ISR_RST; // 止めると RST が立つ
    } else if (v & CR_STA) {
        if (!overflowRst_) isr_ &= (uint8_t)~ISR_RST;
    }
    // RD0 / RD1 を書いても、今のアドレスと残りのバイト数は読み直さない。どちらも RSAR と RBCR を書いたときに
    // その値になり (CRDA は RSAR とは別のレジスタで、転送で進むのは CRDA と残りだけ)、ページを切り替えるための
    // CR の書き込みや、終わった転送のあとの書き直しで戻らないようにする
    if (rd >= 4) {
        dmaCount_ = 0; // 中止
    }
    if ((v & CR_TXP) && !busy && !(cr_ & CR_STP)) startTransmit();
}

// ---------------------------------------------------------------- バッファメモリとリモート DMA

uint8_t Neptune::memRead(uint16_t addr) const {
    if (addr < 0x20) return prom_[addr >> 1];
    if (addr >= RAM_START && addr < RAM_START + RAM_SIZE) return ram_[addr - RAM_START];
    return 0xff;
}

void Neptune::memWrite(uint16_t addr, uint8_t v) {
    if (addr >= RAM_START && addr < RAM_START + RAM_SIZE) ram_[addr - RAM_START] = v;
}

void Neptune::dmaStep() {
    dmaAddr_++;
    // 受信リングの終わりに来たら先頭に戻る (リングをまたぐフレームを続けて読める)
    if (pstop_ > pstart_ && dmaAddr_ == (uint16_t)(pstop_ << 8)) dmaAddr_ = (uint16_t)(pstart_ << 8);
    if (dmaCount_ > 0 && --dmaCount_ == 0) isr_ |= ISR_RDC;
}

// リモート DMA は、CR の RD がその向きで、残りのバイト数が 0 でないときだけ転送する。終わったあと、中止したあと、
// 向きが違うときの読み書きは何もしない (読むと $FF)。WTS = 1 で奇数のバイト数のときは、最後の 1 バイトだけを転送する
uint8_t Neptune::dmaReadByte() {
    if (((cr_ >> 3) & 7) != 1 || dmaCount_ == 0) return 0xff;
    const uint8_t v = memRead(dmaAddr_);
    dmaStep();
    return v;
}

void Neptune::dmaWriteByte(uint8_t v) {
    if (((cr_ >> 3) & 7) != 2 || dmaCount_ == 0) return;
    memWrite(dmaAddr_, v);
    dmaStep();
}

// ---------------------------------------------------------------- 送信と受信

uint32_t Neptune::transmitNs(uint16_t bytes) {
    const uint32_t n = bytes < 60 ? 60 : bytes;
    return (8 + n + 4 + 12) * 800; // 1 バイト 0.8us
}

// フレームは TXP を書いたときに読んで送り、PTX を立てて TXP を下ろすのは送り終えるとき (finishTransmit())
void Neptune::startTransmit() {
    cr_ |= CR_TXP;
    txRemainNs_ = transmitNs(tbcr_);
    txFirstPoll_ = true;
    int len = tbcr_;
    if (len > EtherLink::MAX_FRAME) len = EtherLink::MAX_FRAME;
    uint8_t frame[EtherLink::MAX_FRAME];
    for (int i = 0; i < len; i++) frame[i] = memRead((uint16_t)((tpsr_ << 8) + i));
    if (tcr_ & 0x06) {
        // ループバック: 線には出さず、自分の受信に回す
        receive(frame, len, true);
    } else if (len >= EtherLink::MIN_FRAME) {
        if (link_) link_->send(frame, len);
        sent_++;
    }
}

void Neptune::finishTransmit() {
    if (!(cr_ & CR_TXP)) return;
    cr_ &= (uint8_t)~CR_TXP;
    txRemainNs_ = 0;
    txFirstPoll_ = false;
    tsr_ = 0x01; // PTX
    ncr_ = 0;
    isr_ |= ISR_PTX;
}

bool Neptune::accept(const uint8_t* dest) const {
    if (dest[0] & 1) {
        static const uint8_t kBroadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        if (memcmp(dest, kBroadcast, 6) == 0) return (rcr_ & RCR_AB) != 0;
        if (!(rcr_ & RCR_AM)) return false;
        const int h = multicastHash(dest);
        return (mar_[h >> 3] >> (h & 7)) & 1;
    }
    return (rcr_ & RCR_PRO) || memcmp(dest, par_, 6) == 0;
}

int Neptune::freePages() const {
    const int ring = pstop_ - pstart_;
    if (ring <= 0) return 0;
    if (curr_ == bnry_) return ring;
    if (bnry_ > curr_) return bnry_ - curr_;
    return ring - (curr_ - bnry_);
}

void Neptune::countMissed() {
    if (cntr_[2] != 0xff) cntr_[2]++;
    if (cntr_[2] & 0x80) isr_ |= ISR_CNT;
}

bool Neptune::receive(const uint8_t* frame, int len, bool loopback) {
    // 止めているとき、ループバックの設定で線から届いたときは受け取らない
    if ((cr_ & CR_STP) || len < 6 || (!loopback && (tcr_ & 0x06))) return false;
    if (len > EtherLink::MAX_FRAME) len = EtherLink::MAX_FRAME;
    if (!accept(frame)) return false;
    const uint8_t phy = (frame[0] & 1) ? RSR_PHY : 0;
    if (rcr_ & RCR_MON) { // 調べるだけで、メモリには書かない
        rsr_ = RSR_MPA | phy;
        countMissed();
        return false;
    }
    // 60 バイトに満たなければ 0 で埋め、FCS を付ける
    uint8_t buf[EtherLink::MAX_FRAME + 4];
    memcpy(buf, frame, (size_t)len);
    int n = len;
    if (n < 60) {
        memset(buf + n, 0, (size_t)(60 - n));
        n = 60;
    }
    const uint32_t fcs = fcs32(buf, n);
    for (int i = 0; i < 4; i++) buf[n++] = (uint8_t)(fcs >> (8 * i));
    if (loopback) {
        // ループバックのフレームはバッファメモリに書かない。最後の 8 バイトを FIFO から読めるだけ
        // (DP8390D のデータシートの Loopback Diagnostics、AN-0858、AN-0937)
        memcpy(fifo_, buf + n - 8, 8);
        fifoPos_ = 0;
        rsr_ = RSR_PRX | phy;
        return false;
    }
    // 見出しのバイト数はフレームと FCS (見出しは含まない)。ページ数は見出しの 4 バイトを足して数える
    // (NetBSD の dp8390.c が次のフレームのページからバイト数を計算し直す式と合う)
    const int pages = (n + 4 + 255) >> 8;
    if (pages >= freePages()) {
        // 書くと BNRY のページに届く: 捨てる
        rsr_ = RSR_MPA | phy;
        isr_ |= ISR_OVW | ISR_RST;
        overflowRst_ = true;
        countMissed();
        dropped_++;
        return false;
    }
    const int ring = pstop_ - pstart_;
    int next = curr_ + pages;
    if (next >= pstop_) next -= ring;
    rsr_ = RSR_PRX | phy;
    uint16_t addr = (uint16_t)(curr_ << 8);
    const uint16_t wrapAt = (uint16_t)(pstop_ << 8), wrapTo = (uint16_t)(pstart_ << 8);
    auto put = [&](uint8_t v) {
        memWrite(addr, v);
        if (++addr == wrapAt) addr = wrapTo;
    };
    put(rsr_);
    put((uint8_t)next);
    put((uint8_t)n);
    put((uint8_t)(n >> 8));
    for (int i = 0; i < n; i++) put(buf[i]);
    curr_ = (uint8_t)next;
    isr_ |= ISR_PRX;
    received_++;
    return true;
}

void Neptune::poll(uint32_t elapsedNs) {
    if (cr_ & CR_TXP) {
        // TXP を書いたラスタの分は数えない (書いたのはラスタの途中なので、全部引くと実機より早く送り終える)
        if (txFirstPoll_) txFirstPoll_ = false;
        else if (elapsedNs >= txRemainNs_) finishTransmit();
        else txRemainNs_ -= elapsedNs;
    }
    if (!link_) return;
    // 受信リングに入らないフレームは、実機と同じく捨てて OVW を立てる (receive())
    const int n = link_->receive(rxBuf_, (int)sizeof rxBuf_);
    if (n >= EtherLink::MIN_FRAME) receive(rxBuf_, n, false);
}

} // namespace x68k
