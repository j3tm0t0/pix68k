// ネットワークボード Neptune-X (x68k/neptune.h) のテスト。
// ドライバがする手順 (リセット、PROM からの MAC アドレスの読み出し、初期化、リモート DMA での送信、受信リングからの
// 読み出し、割り込み) を、NetBSD/x68k の stand/boot (ne.c、dp8390.c) と同じ順でレジスタを読み書きしてなぞる。
// 最後に 68000 の命令からボードを読み書きし、IRQ2 (ベクタ $F9) の割り込みを受ける。
#include <stdio.h>
#include <string.h>

#include <deque>
#include <thread>
#include <initializer_list>
#include <vector>

#include "platform/ether_peer.h"
#include "platform/frame_ring.h"
#include "x68k/machine.h"
#include "x68k/neptune.h"
#include "x68k/state.h"
#include "test_check.h"

namespace {

using x68k::Neptune;
using Frame = std::vector<uint8_t>;

// 送ったフレームを覚え、決めたフレームを返す口
struct TestLink : x68k::EtherLink {
    std::vector<Frame> sent;
    std::deque<Frame> inbox;
    void send(const uint8_t* f, int len) override { sent.emplace_back(f, f + len); }
    int receive(uint8_t* buf, int cap) override {
        if (inbox.empty()) return 0;
        Frame f = inbox.front();
        inbox.pop_front();
        if ((int)f.size() > cap) return 0;
        memcpy(buf, f.data(), f.size());
        return (int)f.size();
    }
};

// DP8390 のレジスタの番号
enum {
    CR = 0, PSTART = 1, PSTOP = 2, BNRY = 3, TPSR = 4, TBCR0 = 5, TBCR1 = 6, ISR = 7, RSAR0 = 8, RSAR1 = 9,
    RBCR0 = 10, RBCR1 = 11, RCR = 12, TCR = 13, DCR = 14, IMR = 15, TSR = 4, RSR = 12, CNTR2 = 15, CRDA0 = 8, CRDA1 = 9, FIFO = 6,
    PAR0 = 1, CURR = 7, MAR0 = 8,
};
enum { CR_STP = 0x01, CR_STA = 0x02, CR_TXP = 0x04, CR_RD0 = 0x08, CR_RD1 = 0x10, CR_RD2 = 0x20, CR_PAGE1 = 0x40 };
enum { DCR_WTS = 0x01, DCR_BOS = 0x02, DCR_LS = 0x08, DCR_FT1 = 0x40 };

const uint8_t kMac[6] = {0x02, 0x00, 0x00, 0x12, 0x34, 0x56};
const uint8_t kPeer[6] = {0x02, 0x00, 0x00, 0xaa, 0xbb, 0xcc};

// ドライバから見たボード ($ECE000 からの位置で読み書きする)
struct Drv {
    Neptune& n;
    bool word = true; // データポートを 16 ビットで読み書きする (DCR の WTS)
    uint8_t txPage = 0x40, rxStart = 0x46, rxStop = 0x80, next = 0;

    explicit Drv(Neptune& nic) : n(nic) {}
    void out(int r, uint8_t v) { CHECK_EQ(n.write(0x200 + r * 2, v), true); }
    uint8_t in(int r) { return (uint8_t)n.read(0x200 + r * 2); }
    // 68000 の move.w は上位バイト、下位バイトの順に届く
    void outw(uint16_t w) {
        n.write(0x220, (uint8_t)(w >> 8));
        n.write(0x221, (uint8_t)w);
    }
    uint16_t inw() {
        const int hi = n.read(0x220);
        return (uint16_t)(hi << 8 | n.read(0x221));
    }
    uint8_t dcr() const { return (uint8_t)(DCR_FT1 | DCR_LS | (word ? DCR_WTS : 0)); }

    void reset() {
        const int v = n.read(0x23e);
        n.write(0x23e, (uint8_t)v);
    }
    void readmem(int src, uint8_t* dst, int amount) {
        out(CR, CR_RD2 | CR_STA);
        if (word && (amount & 1)) amount++;
        out(RBCR0, (uint8_t)amount);
        out(RBCR1, (uint8_t)(amount >> 8));
        out(RSAR0, (uint8_t)src);
        out(RSAR1, (uint8_t)(src >> 8));
        out(CR, CR_RD0 | CR_STA);
        if (word) {
            for (int i = 0; i < amount; i += 2) {
                const uint16_t w = inw();
                dst[i] = (uint8_t)(w >> 8);
                dst[i + 1] = (uint8_t)w;
            }
        } else {
            for (int i = 0; i < amount; i++) dst[i] = (uint8_t)n.read(0x220);
        }
    }
    void writemem(const uint8_t* src, int dst, int len) {
        out(CR, CR_RD2 | CR_STA);
        out(ISR, 0x40); // RDC を消す
        out(RBCR0, (uint8_t)len);
        out(RBCR1, (uint8_t)(len >> 8));
        out(RSAR0, (uint8_t)dst);
        out(RSAR1, (uint8_t)(dst >> 8));
        out(CR, CR_RD1 | CR_STA);
        if (word) {
            for (int i = 0; i < len; i += 2) outw((uint16_t)(src[i] << 8 | (i + 1 < len ? src[i + 1] : 0)));
        } else {
            for (int i = 0; i < len; i++) n.write(0x220, src[i]);
        }
        CHECK_EQ(in(ISR) & 0x40, 0x40); // リモート DMA が終わった
    }
    // 初期化 (NetBSD の dp8390_init と同じ順。データシートの手順)
    void init(const uint8_t* mac, uint8_t rcr = 0x04) {
        out(CR, CR_RD2 | CR_STP);
        out(DCR, dcr());
        out(RBCR0, 0);
        out(RBCR1, 0);
        out(RCR, 0x20); // MON
        out(TCR, 0x02); // 内部ループバック
        out(BNRY, rxStart);
        out(PSTART, rxStart);
        out(PSTOP, rxStop);
        out(ISR, 0xff);
        out(IMR, 0);
        out(CR, CR_RD2 | CR_PAGE1 | CR_STP);
        for (int i = 0; i < 6; i++) out(PAR0 + i, mac[i]);
        next = (uint8_t)(rxStart + 1);
        out(CURR, next);
        out(CR, CR_RD2 | CR_STP);
        out(RCR, rcr);
        out(TCR, 0);
        out(CR, CR_RD2 | CR_STA);
    }
    void send(const uint8_t* f, int len) {
        writemem(f, txPage << 8, len);
        out(TPSR, txPage);
        out(TBCR0, (uint8_t)(len < 60 ? 60 : len));
        out(TBCR1, (uint8_t)(len >> 8));
        out(CR, CR_RD2 | CR_TXP | CR_STA);
        n.finishTransmit(); // 送る時間は testTransmitTime で確かめる
    }
    // 受信リングから 1 フレーム読む (NetBSD の EtherReceive と同じ)。なければ空。見出しも返す
    Frame receive(uint8_t hdr[4] = nullptr) {
        out(CR, CR_RD2 | CR_PAGE1 | CR_STA);
        const uint8_t current = in(CURR);
        out(CR, CR_RD2 | CR_STA);
        if (next == current) return Frame();
        uint8_t h[4];
        readmem(next << 8, h, 4);
        if (hdr) memcpy(hdr, h, 4);
        // 見出しのバイト数はフレームと FCS。NetBSD と同じく、そこから 4 を引いた分 (FCS を除いたフレーム) を読む
        int len = (h[2] | h[3] << 8) - 4;
        Frame f((size_t)len + 1);
        int buf = (next << 8) + 4;
        int done = 0;
        if (buf + len > rxStop << 8) { // リングの終わりをまたぐ
            const int first = (rxStop << 8) - buf;
            readmem(buf, f.data(), first);
            done = first;
            buf = rxStart << 8;
        }
        readmem(buf, f.data() + done, len - done);
        f.resize((size_t)len);
        next = h[1];
        uint8_t b = (uint8_t)(next - 1);
        if (b < rxStart) b = (uint8_t)(rxStop - 1);
        out(BNRY, b);
        return f;
    }
};

Frame makeFrame(const uint8_t* dst, const uint8_t* src, int len, uint8_t seed = 0) {
    Frame f((size_t)len);
    memcpy(f.data(), dst, 6);
    memcpy(f.data() + 6, src, 6);
    f[12] = 0x08;
    f[13] = 0x00;
    for (int i = 14; i < len; i++) f[i] = (uint8_t)(i * 7 + seed);
    return f;
}

// FCS (CRC-32) を、ビットごとに計算する (neptune.cpp の表とは別の計算で確かめる)
uint32_t crcBitwise(const uint8_t* p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return c; // 反転する前
}

// マルチキャストのハッシュの番号: 下位から計算した CRC (反転前) の下位 6 ビットを逆順にしたもの
int hashOf(const uint8_t* addr) {
    const uint32_t c = crcBitwise(addr, 6);
    int h = 0;
    for (int i = 0; i < 6; i++) h |= (int)((c >> i) & 1) << (5 - i);
    return h;
}

// ---------------------------------------------------------------- テスト

void testResetAndProm() {
    printf("neptune: reset port, PROM (MAC address)\n");
    Neptune n;
    n.setMac(kMac);
    Drv d(n);
    d.out(CR, CR_RD2 | CR_STA);
    CHECK_EQ(d.in(ISR) & 0x80, 0); // 動かすと RST が消える
    d.reset();
    CHECK_EQ(d.in(CR), 0x21);
    CHECK_EQ(d.in(ISR) & 0x80, 0x80);
    // NetBSD と同じく、STP を書いてから CR と ISR を見る
    d.out(CR, CR_RD2 | CR_STP);
    CHECK_EQ(d.in(CR) & 0x27, 0x21);
    CHECK_EQ(d.in(ISR) & 0x80, 0x80);
    // PROM を 16 ビットで 32 バイト読む: 1 バイトが 2 バイトずつ並ぶ
    d.out(RCR, 0x20);
    d.out(DCR, d.dcr());
    uint8_t rom[32];
    d.readmem(0, rom, 32);
    for (int i = 0; i < 6; i++) {
        CHECK_EQ(rom[i * 2], kMac[i]);
        CHECK_EQ(rom[i * 2 + 1], kMac[i]);
    }
    CHECK_EQ(rom[28], 0x57);
    CHECK_EQ(rom[30], 0x57);
    CHECK_EQ(d.in(ISR) & 0x40, 0x40); // バイト数を読み終えると RDC
    // 8 ビットで読んでも 2 バイトずつ
    d.word = false;
    d.out(DCR, d.dcr());
    d.readmem(0, rom, 4);
    CHECK_EQ(rom[0], kMac[0]);
    CHECK_EQ(rom[1], kMac[0]);
    CHECK_EQ(rom[2], kMac[1]);

    // 範囲の外はバスエラー、範囲の中の使わない番地と奇数番地は $FF
    CHECK_EQ(n.read(0x400), -1);
    CHECK_EQ(n.write(0x400, 0), false);
    CHECK_EQ(n.read(0x000), 0xff);
    CHECK_EQ(n.read(0x201), 0xff);
    CHECK_EQ(n.read(0x300), 0xff);
}

void testTransmit() {
    printf("neptune: transmit through remote DMA\n");
    Neptune n;
    TestLink link;
    n.setLink(&link);
    Drv d(n);
    d.init(kMac);
    const Frame f = makeFrame(kPeer, kMac, 100);
    d.out(ISR, 0xff);
    d.out(IMR, 0x0a); // PTX と TXE
    d.send(f.data(), (int)f.size());
    CHECK_EQ(link.sent.size(), 1);
    if (!link.sent.empty()) {
        CHECK_EQ(link.sent[0].size(), 100);
        CHECK_EQ(link.sent[0] == f, true);
    }
    CHECK_EQ(d.in(ISR) & 0x02, 0x02);
    CHECK_EQ(d.in(TSR) & 0x01, 0x01);
    CHECK_EQ(d.in(CR) & CR_TXP, 0);
    CHECK_EQ(n.irq(), true);
    d.out(ISR, 0x02);
    CHECK_EQ(n.irq(), false);
    // 短いフレームはドライバが 60 バイトにして送る
    const Frame s = makeFrame(kPeer, kMac, 42);
    d.send(s.data(), (int)s.size());
    CHECK_EQ(link.sent.size(), 2);
    if (link.sent.size() == 2) {
        CHECK_EQ(link.sent[1].size(), 60);
        CHECK_EQ(memcmp(link.sent[1].data(), s.data(), s.size()), 0);
    }
    // 奇数の長さ (最後の語の下位バイトは捨てる)
    const Frame o = makeFrame(kPeer, kMac, 101, 3);
    d.send(o.data(), (int)o.size());
    if (link.sent.size() == 3) CHECK_EQ(link.sent[2] == o, true);
    // 止めていると送らない
    d.out(CR, CR_RD2 | CR_STP);
    d.out(CR, CR_RD2 | CR_TXP | CR_STP);
    CHECK_EQ(link.sent.size(), 3);
}

// 送り終えるのは線で送る時間が過ぎてから。それまで CR の TXP は 1 のままで、PTX も割り込みもない
void testTransmitTime() {
    printf("neptune: transmit completes after the wire time\n");
    CHECK_EQ(Neptune::transmitNs(42), (8 + 60 + 4 + 12) * 800u); // 60 バイトに満たないものは 60 バイトとして数える
    CHECK_EQ(Neptune::transmitNs(1514), (8 + 1514 + 4 + 12) * 800u);
    Neptune n;
    TestLink link;
    n.setLink(&link);
    Drv d(n);
    d.init(kMac);
    d.out(ISR, 0xff);
    d.out(IMR, 0x0a);
    const Frame f = makeFrame(kPeer, kMac, 100);
    auto start = [&](const Frame& fr) {
        d.writemem(fr.data(), d.txPage << 8, (int)fr.size());
        d.out(TPSR, d.txPage);
        d.out(TBCR0, (uint8_t)fr.size());
        d.out(TBCR1, (uint8_t)(fr.size() >> 8));
        d.out(CR, CR_RD2 | CR_TXP | CR_STA);
    };
    const uint32_t total = Neptune::transmitNs(100);
    start(f);
    // フレームは TXP を書いたときに送る。PTX を立てて TXP を下ろすのは送り終えるとき
    CHECK_EQ(link.sent.size(), 1);
    if (link.sent.size() == 1) CHECK_EQ(link.sent[0] == f, true);
    CHECK_EQ(n.transmitting(), true);
    CHECK_EQ(d.in(CR) & CR_TXP, CR_TXP);
    CHECK_EQ(d.in(ISR) & 0x02, 0);
    CHECK_EQ(n.irq(), false);
    // 送っている間に同じページへ次のフレームを書いても、送ったものは変わらない
    const Frame g = makeFrame(kPeer, kMac, 100, 7);
    d.writemem(g.data(), d.txPage << 8, (int)g.size());
    // TXP を書いてから最初の poll() は時間を進めない (TXP を書いたラスタの分は数えない)
    n.poll(Neptune::transmitNs(1514));
    CHECK_EQ(d.in(CR) & CR_TXP, CR_TXP);
    // 半分進めてから TXP に 0 を書いても、TXP を書き直しても、送り終える時刻は変わらない
    n.poll(total / 2);
    CHECK_EQ(d.in(CR) & CR_TXP, CR_TXP);
    d.out(CR, CR_RD2 | CR_STA);
    CHECK_EQ(d.in(CR) & CR_TXP, CR_TXP);
    d.out(CR, CR_RD2 | CR_TXP | CR_STA);
    CHECK_EQ(link.sent.size(), 1); // 送っている間の TXP では送らない
    n.poll(total - total / 2 - 1);
    CHECK_EQ(d.in(CR) & CR_TXP, CR_TXP);
    CHECK_EQ(d.in(ISR) & 0x02, 0);
    n.poll(1);
    CHECK_EQ(d.in(CR) & CR_TXP, 0);
    CHECK_EQ(d.in(ISR) & 0x02, 0x02);
    CHECK_EQ(d.in(TSR) & 0x01, 0x01);
    CHECK_EQ(n.irq(), true);
    CHECK_EQ(link.sent.size(), 1);
    if (link.sent.size() == 1) CHECK_EQ(link.sent[0] == f, true);
    d.out(ISR, 0x02);
    n.poll(Neptune::transmitNs(1514));
    CHECK_EQ(n.irq(), false);
    // 送っている間に STP を書いても送り終える
    start(f);
    CHECK_EQ(link.sent.size(), 2);
    d.out(CR, CR_RD2 | CR_STP);
    CHECK_EQ(d.in(CR) & (CR_TXP | CR_STP), CR_TXP | CR_STP);
    n.poll(0);
    n.poll(total);
    CHECK_EQ(d.in(CR) & CR_TXP, 0);
    CHECK_EQ(d.in(ISR) & 0x02, 0x02);
    d.out(ISR, 0x02);
    // リセットポートを読むと中止する (フレームはもう送っている。PTX は立たない)
    d.out(CR, CR_RD2 | CR_STA);
    start(f);
    d.reset();
    CHECK_EQ(n.transmitting(), false);
    n.poll(0);
    n.poll(Neptune::transmitNs(1514));
    CHECK_EQ(link.sent.size(), 3);
    CHECK_EQ(d.in(ISR) & 0x02, 0);

    // ステート: 送っている途中で保存して読み込んだものは、保存せずに続けたものと同じときに送り終える。
    // before が 0 のものは、TXP を書いてから最初の poll() の前に保存する
    for (const uint32_t before : {0u, 1000u, total - 1}) {
        x68k::Machine m;
        CHECK_EQ(m.setNeptune(true), true);
        TestLink linkA;
        m.neptune()->setLink(&linkA);
        Drv dm(*m.neptune());
        dm.init(kMac);
        dm.out(IMR, 0x0a);
        dm.writemem(f.data(), dm.txPage << 8, (int)f.size());
        dm.out(TPSR, dm.txPage);
        dm.out(TBCR0, (uint8_t)f.size());
        dm.out(TBCR1, 0);
        dm.out(CR, CR_RD2 | CR_TXP | CR_STA);
        if (before) {
            m.neptune()->poll(0);
            m.neptune()->poll(before);
        }
        std::vector<uint8_t> buf(x68k::stateSize(m));
        size_t written = 0;
        CHECK_EQ(x68k::saveState(m, buf.data(), buf.size(), written), true);
        x68k::Machine m2;
        m2.setNeptune(true);
        TestLink linkB;
        m2.neptune()->setLink(&linkB);
        const char* err = nullptr;
        CHECK_EQ(x68k::loadState(m2, buf.data(), written, err), true);
        Neptune* const ns[2] = {m.neptune(), m2.neptune()};
        if (!before) for (Neptune* n2 : ns) n2->poll(5000); // 最初の poll() は進めない
        // 残りの 1ns 手前まで進めると、どちらも送っている
        for (Neptune* n2 : ns) n2->poll(total - before - 1);
        for (Neptune* n2 : ns) {
            CHECK_EQ(n2->transmitting(), true);
            CHECK_EQ(n2->irq(), false);
        }
        // あと 1ns で、どちらも送り終えて PTX の割り込みを出す
        for (Neptune* n2 : ns) n2->poll(1);
        for (Neptune* n2 : ns) {
            CHECK_EQ(n2->transmitting(), false);
            CHECK_EQ(n2->isr() & 0x02, 0x02);
            CHECK_EQ(n2->irq(), true);
        }
        // フレームは保存の前に送っているので、読み込んだほうは送り直さない
        CHECK_EQ(linkA.sent.size(), 1);
        CHECK_EQ(linkB.sent.size(), 0);

        // 送っている間に TBCR を書き換えてから保存しても、終わる時刻は保存せずに続けたものと同じ
        // (1000 バイトのフレームを 1000ns 送ったところで TBCR を 60 にする)
        if (before == 1000) {
            const Frame big = makeFrame(kPeer, kMac, 1000);
            const uint32_t bigTotal = Neptune::transmitNs(1000);
            x68k::Machine m5;
            CHECK_EQ(m5.setNeptune(true), true);
            Drv d5(*m5.neptune());
            d5.init(kMac);
            d5.out(IMR, 0x0a);
            d5.writemem(big.data(), d5.txPage << 8, (int)big.size());
            d5.out(TPSR, d5.txPage);
            d5.out(TBCR0, (uint8_t)big.size());
            d5.out(TBCR1, (uint8_t)(big.size() >> 8));
            d5.out(CR, CR_RD2 | CR_TXP | CR_STA);
            m5.neptune()->poll(0);
            m5.neptune()->poll(1000);
            d5.out(TBCR0, 60);
            d5.out(TBCR1, 0);
            std::vector<uint8_t> b5(x68k::stateSize(m5));
            CHECK_EQ(x68k::saveState(m5, b5.data(), b5.size(), written), true);
            x68k::Machine m6;
            m6.setNeptune(true);
            CHECK_EQ(x68k::loadState(m6, b5.data(), written, err), true);
            Neptune* const ns2[2] = {m5.neptune(), m6.neptune()};
            // TBCR が 60 のときの全体の時間 (67200ns) を過ぎても、どちらも送っている
            for (Neptune* n2 : ns2) n2->poll(Neptune::transmitNs(60));
            for (Neptune* n2 : ns2) CHECK_EQ(n2->transmitting(), true);
            for (Neptune* n2 : ns2) n2->poll(bigTotal - 1000 - Neptune::transmitNs(60) - 1);
            for (Neptune* n2 : ns2) {
                CHECK_EQ(n2->transmitting(), true);
                CHECK_EQ(n2->irq(), false);
            }
            for (Neptune* n2 : ns2) n2->poll(1);
            for (Neptune* n2 : ns2) {
                CHECK_EQ(n2->transmitting(), false);
                CHECK_EQ(n2->irq(), true);
            }
        }

        // 版 10 のステート (残りの時間がない) を読むと、残りは 0 で、次の poll() で送り終える
        if (before == 1000) {
            x68k::Machine m3;
            CHECK_EQ(m3.setNeptune(true), true);
            Drv d3(*m3.neptune());
            d3.init(kMac);
            d3.out(IMR, 0x0a);
            d3.writemem(f.data(), d3.txPage << 8, (int)f.size());
            d3.out(TPSR, d3.txPage);
            d3.out(TBCR0, (uint8_t)f.size());
            d3.out(TBCR1, 0);
            d3.out(CR, CR_RD2 | CR_TXP | CR_STA);
            std::vector<uint8_t> old(x68k::stateSize(m3) + 4096); // 今の版の大きさ + 余裕 (版 11 までは SPC の状態が大きい)
            CHECK_EQ(x68k::saveState(m3, old.data(), old.size(), written, 10), true);
            x68k::Machine m4;
            m4.setNeptune(true);
            CHECK_EQ(x68k::loadState(m4, old.data(), written, err), true);
            CHECK_EQ(m4.neptune()->transmitting(), true);
            m4.neptune()->poll(0);
            CHECK_EQ(m4.neptune()->transmitting(), false);
            CHECK_EQ(m4.neptune()->irq(), true);
        }
    }
}

void testReceive() {
    printf("neptune: receive ring, header, FCS, padding\n");
    Neptune n;
    Drv d(n);
    d.init(kMac);
    d.out(IMR, 0x01);
    CHECK_EQ(d.receive().empty(), true);
    const Frame f = makeFrame(kMac, kPeer, 300);
    CHECK_EQ(n.receiveFrame(f.data(), (int)f.size()), true);
    CHECK_EQ(n.irq(), true);
    CHECK_EQ(d.in(ISR) & 0x01, 0x01);
    CHECK_EQ(d.in(RSR) & 0x21, 0x01); // PRX、PHY = 0 (自分宛て)
    uint8_t h[4];
    Frame r = d.receive(h);
    CHECK_EQ(h[0] & 0x01, 0x01);
    CHECK_EQ(h[1], d.rxStart + 1 + 2); // 4 + 300 + 4 バイトは 2 ページ
    CHECK_EQ(h[2] | h[3] << 8, 304);      // フレームと FCS (見出しは含まない)
    CHECK_EQ(r.size(), 300);
    if (r.size() == 300) CHECK_EQ(memcmp(r.data(), f.data(), 300), 0);
    {
        const uint16_t at = (uint16_t)(((d.rxStart + 1) << 8) + 4 + 300); // FCS はフレームのすぐ後ろ
        const uint32_t fcs = ~crcBitwise(f.data(), 300);
        CHECK_EQ(n.memRead(at) | n.memRead(at + 1) << 8 | n.memRead(at + 2) << 16 | (uint32_t)n.memRead(at + 3) << 24,
                 fcs);
    }
    d.out(ISR, 0x01);
    CHECK_EQ(n.irq(), false);
    CHECK_EQ(d.receive().empty(), true);

    // 60 バイトに満たないフレームは 0 で埋める
    const Frame s = makeFrame(kMac, kPeer, 42);
    n.receiveFrame(s.data(), (int)s.size());
    r = d.receive(h);
    CHECK_EQ(h[2] | h[3] << 8, 64);
    CHECK_EQ(r.size(), 60);
    if (r.size() == 60) {
        CHECK_EQ(memcmp(r.data(), s.data(), 42), 0);
        for (int i = 42; i < 60; i++) CHECK_EQ(r[i], 0);
    }

    // 1514 バイト (いちばん長いフレーム)
    const Frame l = makeFrame(kMac, kPeer, 1514, 5);
    CHECK_EQ(n.receiveFrame(l.data(), (int)l.size()), true);
    r = d.receive();
    CHECK_EQ(r.size(), 1514);
    if (r.size() == 1514) CHECK_EQ(memcmp(r.data(), l.data(), 1514), 0);
}

void testFilter() {
    printf("neptune: receive filter (own address, broadcast, multicast hash, promiscuous)\n");
    static const uint8_t kBroadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    static const uint8_t kGroup[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0x01};
    static const uint8_t kOther[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    Neptune n;
    Drv d(n);
    d.init(kMac, 0x00); // 自分宛てだけ
    Frame f = makeFrame(kBroadcast, kPeer, 60);
    CHECK_EQ(n.receiveFrame(f.data(), 60), false);
    f = makeFrame(kOther, kPeer, 60);
    CHECK_EQ(n.receiveFrame(f.data(), 60), false);
    f = makeFrame(kMac, kPeer, 60);
    CHECK_EQ(n.receiveFrame(f.data(), 60), true);
    d.receive();

    d.out(RCR, 0x04); // AB
    f = makeFrame(kBroadcast, kPeer, 60);
    CHECK_EQ(n.receiveFrame(f.data(), 60), true);
    CHECK_EQ(d.in(RSR) & 0x20, 0x20); // PHY = 1 (マルチキャストかブロードキャスト)
    d.receive();

    // マルチキャスト: AM と、ハッシュの番号の MAR のビット
    f = makeFrame(kGroup, kPeer, 60);
    d.out(RCR, 0x08);
    CHECK_EQ(n.receiveFrame(f.data(), 60), false); // MAR が 0
    const int h = hashOf(kGroup);
    d.out(CR, CR_RD2 | CR_PAGE1 | CR_STA);
    d.out(MAR0 + (h >> 3), (uint8_t)(1 << (h & 7)));
    d.out(CR, CR_RD2 | CR_STA);
    CHECK_EQ(n.receiveFrame(f.data(), 60), true);
    d.receive();
    // ほかのビットだけなら受け取らない
    d.out(CR, CR_RD2 | CR_PAGE1 | CR_STA);
    d.out(MAR0 + (h >> 3), (uint8_t)~(1 << (h & 7)));
    for (int i = 0; i < 8; i++)
        if (i != (h >> 3)) d.out(MAR0 + i, 0xff);
    d.out(CR, CR_RD2 | CR_STA);
    CHECK_EQ(n.receiveFrame(f.data(), 60), false);
    d.out(RCR, 0x00);

    // プロミスキャス: ほかの宛ても受け取る
    d.out(RCR, 0x10);
    f = makeFrame(kOther, kPeer, 60);
    CHECK_EQ(n.receiveFrame(f.data(), 60), true);
    d.receive();

    // 止めていると受け取らない。MON では書かずに数える
    d.out(CR, CR_RD2 | CR_STP);
    CHECK_EQ(n.receiveFrame(f.data(), 60), false);
    d.out(CR, CR_RD2 | CR_STA);
    d.out(RCR, 0x30);
    CHECK_EQ(n.receiveFrame(f.data(), 60), false);
    CHECK_EQ(d.in(RSR) & 0x10, 0x10);
    CHECK_EQ(d.in(CNTR2), 1);
    CHECK_EQ(d.in(CNTR2), 0); // 読むと 0 に戻る
}

void testRingWrapAndOverflow() {
    printf("neptune: ring wraps at PSTOP, overflow against BNRY\n");
    Neptune n;
    Drv d(n);
    d.rxStart = 0x46;
    d.rxStop = 0x50; // 10 ページの小さなリング
    d.init(kMac);
    d.out(IMR, 0x11);
    // 3 ページ (4 + 600 + 4) のフレームを順に入れて読む。リングをまたいでも同じに読める
    for (int k = 0; k < 8; k++) {
        const Frame f = makeFrame(kMac, kPeer, 600, (uint8_t)k);
        CHECK_EQ(n.receiveFrame(f.data(), (int)f.size()), true);
        const Frame r = d.receive();
        CHECK_EQ(r.size(), 600);
        if (r.size() == 600) CHECK_EQ(memcmp(r.data(), f.data(), 600), 0);
    }
    CHECK_EQ(d.receive().empty(), true);
    // 読まずに入れ続けると、BNRY に届くところで捨てる (9 ページ空いているので 3 ページのフレームは 2 つまで)
    const uint8_t curr0 = n.curr();
    const Frame f = makeFrame(kMac, kPeer, 600, 9);
    CHECK_EQ(n.receiveFrame(f.data(), 600), true);
    CHECK_EQ(n.receiveFrame(f.data(), 600), true);
    d.out(ISR, 0xff);
    CHECK_EQ(n.receiveFrame(f.data(), 600), false);
    CHECK_EQ(d.in(ISR) & 0x90, 0x90); // OVW と RST
    CHECK_EQ(n.irq(), true);
    CHECK_EQ(d.in(CNTR2), 1);
    CHECK_EQ(n.curr(), (uint8_t)(0x46 + (curr0 - 0x46 + 6) % 10));
    // 取り出すと (BNRY を進めると) RST が消え、また入る
    CHECK_EQ(d.receive().size(), 600);
    CHECK_EQ(d.in(ISR) & 0x80, 0);
    d.out(ISR, 0x10);
    CHECK_EQ(n.receiveFrame(f.data(), 600), true);
    CHECK_EQ(d.receive().size(), 600);
    CHECK_EQ(d.receive().size(), 600);
    CHECK_EQ(d.receive().empty(), true);
}

void testLoopbackAndByteOrder() {
    printf("neptune: loopback, 8-bit transfers, BOS\n");
    Neptune n;
    TestLink link;
    n.setLink(&link);
    Drv d(n);
    d.init(kMac);
    // ループバック: 線には出さず、自分の受信リングに入る
    d.out(TCR, 0x02);
    const Frame f = makeFrame(kMac, kMac, 80);
    d.send(f.data(), (int)f.size());
    CHECK_EQ(link.sent.size(), 0);
    CHECK_EQ(d.in(ISR) & 0x02, 0x02); // PTX (受信リングには書かないので、PRX は見ない)
    // ループバックのフレームは受信リングに書かず、最後の 8 バイト (フレームの最後の 4 バイトと FCS) を FIFO から読む
    Frame r = d.receive();
    CHECK_EQ(r.empty(), true);
    CHECK_EQ(d.in(RSR) & 0x01, 0x01);
    {
        const uint32_t fcs = ~crcBitwise(f.data(), 80);
        const uint8_t want[8] = {f[76], f[77], f[78], f[79], (uint8_t)fcs, (uint8_t)(fcs >> 8), (uint8_t)(fcs >> 16),
                                 (uint8_t)(fcs >> 24)};
        for (int i = 0; i < 8; i++) CHECK_EQ(d.in(FIFO), want[i]);
    }
    // ループバックの間は線からは受け取らない
    CHECK_EQ(n.receiveFrame(f.data(), 80), false);
    d.out(TCR, 0x00);

    // 8 ビットの転送
    d.word = false;
    d.out(DCR, d.dcr());
    const Frame g = makeFrame(kPeer, kMac, 71);
    d.send(g.data(), (int)g.size());
    CHECK_EQ(link.sent.size(), 1);
    if (link.sent.size() == 1) CHECK_EQ(memcmp(link.sent[0].data(), g.data(), 71), 0);
    CHECK_EQ(n.receiveFrame(g.data(), 71), false); // 宛てが違う
    const Frame h = makeFrame(kMac, kPeer, 71);
    n.receiveFrame(h.data(), 71);
    r = d.receive();
    CHECK_EQ(r.size(), 71);
    if (r.size() == 71) CHECK_EQ(memcmp(r.data(), h.data(), 71), 0);

    // BOS = 1 では、16 ビットの読み書きの上位と下位が入れ替わる
    d.word = true;
    d.out(DCR, (uint8_t)(d.dcr() | DCR_BOS));
    d.out(RBCR0, 2);
    d.out(RBCR1, 0);
    d.out(RSAR0, 0x00);
    d.out(RSAR1, 0x40);
    d.out(CR, CR_RD1 | CR_STA);
    d.outw(0x1234);
    CHECK_EQ(n.memRead(0x4000), 0x34);
    CHECK_EQ(n.memRead(0x4001), 0x12);
    d.out(RBCR0, 2);
    d.out(RSAR0, 0x00);
    d.out(RSAR1, 0x40);
    d.out(CR, CR_RD0 | CR_STA);
    CHECK_EQ(d.inw(), 0x1234);
    d.out(DCR, d.dcr());
    d.out(RBCR0, 2);
    d.out(RSAR0, 0x00);
    d.out(RSAR1, 0x40);
    d.out(CR, CR_RD0 | CR_STA);
    CHECK_EQ(d.inw(), 0x3412);
}

void testPoll() {
    printf("neptune: frames from the link that do not fit are dropped with OVW\n");
    Neptune n;
    TestLink link;
    n.setLink(&link);
    Drv d(n);
    d.rxStart = 0x46;
    d.rxStop = 0x4b; // 5 ページ (BNRY の後ろの 4 ページに書ける)
    d.init(kMac);
    for (int k = 0; k < 3; k++) link.inbox.push_back(makeFrame(kMac, kPeer, 600, (uint8_t)k));
    link.inbox.push_back(makeFrame(kPeer, kMac, 60)); // 宛てが違う: 捨てる (OVW にはしない)
    n.poll(0); // 1 つ目が入る (3 ページ)
    CHECK_EQ(d.in(ISR) & 0x11, 0x01);
    n.poll(0); // 2 つ目は入らない (残り 1 ページ): 捨てて OVW
    CHECK_EQ(d.in(ISR) & 0x10, 0x10);
    n.poll(0);
    n.poll(0);
    CHECK_EQ(n.received(), 1);
    CHECK_EQ(n.dropped(), 2);
    CHECK_EQ(link.inbox.size(), 0);
    CHECK_EQ(d.in(CNTR2), 2);
    Frame r = d.receive();
    CHECK_EQ(r.size(), 600);
    if (r.size() == 600) CHECK_EQ(r[14], (uint8_t)(14 * 7));
    CHECK_EQ(d.receive().empty(), true);
    d.out(ISR, 0xff);

    // リング全体より大きいフレーム (1514 バイトは 6 ページ) は捨て、次のフレームは受け取る
    link.inbox.push_back(makeFrame(kMac, kPeer, 1514, 1));
    link.inbox.push_back(makeFrame(kMac, kPeer, 60, 2));
    n.poll(0);
    CHECK_EQ(d.in(ISR) & 0x10, 0x10);
    n.poll(0);
    CHECK_EQ(n.received(), 2);
    CHECK_EQ(n.dropped(), 3);
    r = d.receive();
    CHECK_EQ(r.size(), 60);
    if (r.size() == 60) CHECK_EQ(r[14], (uint8_t)(14 * 7 + 2));
}

// 受信の見出しのバイト数とページの境目。NetBSD の dp8390.c は、バイト数の上位を次のフレームのページから計算し直す
// (古いチップの誤りへの対策)。その計算と見出しの値が合うこと
void testHeaderLength() {
    printf("neptune: receive header byte count at page boundaries\n");
    for (int len = 60; len <= 1514; len++) {
        Neptune n;
        Drv d(n);
        d.init(kMac);
        const Frame f = makeFrame(kMac, kPeer, len);
        CHECK_EQ(n.receiveFrame(f.data(), len), true);
        uint8_t h[4];
        const Frame r = d.receive(h);
        const int count = h[2] | h[3] << 8;
        const int first = d.rxStart + 1;
        int nlen = h[1] - first - 1;
        if ((count & 0xff) + 4 > 256) nlen--;
        const int recalculated = (count & 0xff) | nlen << 8;
        if (count != len + 4 || recalculated != count || r.size() != (size_t)len) {
            CHECK_EQ(count, len + 4);
            CHECK_EQ(recalculated, count);
            CHECK_EQ(r.size(), (size_t)len);
            break;
        }
    }
}

// リモート DMA の終わり、中止、向き違い、同じコマンドの書き直し
void testRemoteDma() {
    printf("neptune: remote DMA stops at the count, on abort and in the wrong direction\n");
    Neptune n;
    Drv d(n);
    d.init(kMac);
    static const uint8_t kData[4] = {0x11, 0x22, 0x33, 0x44};
    d.word = false;
    d.out(DCR, d.dcr());
    d.writemem(kData, 0x4000, 4);
    // RBCR = 1 の書き込みが終わったあとの書き込みは、次のバイトを変えない
    d.out(RBCR0, 1);
    d.out(RBCR1, 0);
    d.out(RSAR0, 0x00);
    d.out(RSAR1, 0x40);
    d.out(CR, CR_RD1 | CR_STA);
    n.write(0x220, 0xaa);
    CHECK_EQ(d.in(ISR) & 0x40, 0x40);
    n.write(0x220, 0xbb);
    CHECK_EQ(n.memRead(0x4000), 0xaa);
    CHECK_EQ(n.memRead(0x4001), 0x22);
    // 中止したあとの書き込みも何もしない
    d.out(RBCR0, 2);
    d.out(RSAR0, 0x01);
    d.out(CR, CR_RD1 | CR_STA);
    d.out(CR, CR_RD2 | CR_STA);
    n.write(0x220, 0xcc);
    CHECK_EQ(n.memRead(0x4001), 0x22);
    // 読み出しの向きのときにデータポートへ書いても何も起きず、書き込みの向きで読むと $FF で進まない
    d.out(RBCR0, 2);
    d.out(RSAR0, 0x01);
    d.out(CR, CR_RD0 | CR_STA);
    n.write(0x220, 0xdd);
    CHECK_EQ(n.memRead(0x4001), 0x22);
    CHECK_EQ(n.read(0x220), 0x22);
    d.out(RBCR0, 2);
    d.out(RSAR0, 0x01);
    d.out(CR, CR_RD1 | CR_STA);
    CHECK_EQ(n.read(0x220), 0xff);
    CHECK_EQ(d.in(CRDA0), 0x01);
    // 4 バイトの読み出しの途中で同じコマンドを書き直しても、今のアドレスは戻らない
    d.out(RBCR0, 4);
    d.out(RSAR0, 0x00);
    d.out(CR, CR_RD0 | CR_STA);
    CHECK_EQ(n.read(0x220), 0xaa);
    CHECK_EQ(n.read(0x220), 0x22);
    d.out(CR, CR_RD0 | CR_STA);
    CHECK_EQ(d.in(CRDA0), 0x02);
    CHECK_EQ(n.read(0x220), 0x33);
    CHECK_EQ(n.read(0x220), 0x44);
    CHECK_EQ(n.read(0x220), 0xff); // 終わった
    // 終わった転送のあとに CR を書き直しても (ページの切り替えを含む)、アドレスは戻らず、RAM も書き換えない
    d.out(RBCR0, 1);
    d.out(RBCR1, 0);
    d.out(RSAR0, 0x00);
    d.out(RSAR1, 0x40);
    n.write(0x200, 0x12); // RD1、STA
    n.write(0x220, 0xe1);
    n.write(0x200, 0x52); // ページ 1 に切り替える
    n.write(0x200, 0x12); // ページ 0 に戻す
    CHECK_EQ(d.in(CRDA0), 0x01);
    CHECK_EQ(d.in(CRDA1), 0x40);
    n.write(0x220, 0xe2);
    CHECK_EQ(n.memRead(0x4000), 0xe1);
    CHECK_EQ(n.memRead(0x4001), 0x22);
    // RD = 0 を書いたあとでも、RBCR と RSAR を書いてから RD1 を書けば、その位置から書く
    n.write(0x200, 0x02);
    d.out(RBCR0, 1);
    d.out(RSAR0, 0x01);
    n.write(0x200, 0x12);
    n.write(0x220, 0xe3);
    CHECK_EQ(n.memRead(0x4001), 0xe3);
    CHECK_EQ(n.memRead(0x4002), 0x33);
    // 1 回のリモート DMA の読み出しでも、PSTOP に来ると PSTART に戻る (受信リングをまたぐフレームを続けて読める)
    {
        static const uint8_t kEnd[2] = {0x91, 0x92}, kStart[2] = {0x93, 0x94};
        d.word = false;
        d.out(DCR, d.dcr());
        d.writemem(kEnd, (d.rxStop << 8) - 2, 2);
        d.writemem(kStart, d.rxStart << 8, 2);
        uint8_t got[4];
        d.readmem((d.rxStop << 8) - 2, got, 4);
        CHECK_EQ(got[0], 0x91);
        CHECK_EQ(got[1], 0x92);
        CHECK_EQ(got[2], 0x93);
        CHECK_EQ(got[3], 0x94);
    }
    // WTS = 1 で奇数のバイト数: 最後のワードの 2 バイト目は書かない
    d.word = true;
    d.out(DCR, d.dcr());
    d.out(ISR, 0x40);
    d.out(RBCR0, 3);
    d.out(RBCR1, 0);
    d.out(RSAR0, 0x00);
    d.out(RSAR1, 0x40);
    d.out(CR, CR_RD1 | CR_STA);
    d.outw(0x5566);
    d.outw(0x7788);
    CHECK_EQ(n.memRead(0x4002), 0x77);
    CHECK_EQ(n.memRead(0x4003), 0x44);
}

// ---------------------------------------------------------------- Machine につないで

void testMachine() {
    printf("neptune: on the machine ($ECE200, IRQ2 vector $F9, state)\n");
    x68k::Machine none;
    bool ok = true;
    none.peek8(0xece200, ok);
    CHECK_EQ(ok, false);

    x68k::Machine m;
    CHECK_EQ(m.setNeptune(true), true);
    Neptune& n = *m.neptune();
    n.setMac(kMac);
    m.read8(0xece23e, m68k::FC_SUPER_DATA); // リセット
    CHECK_EQ(m.read8(0xece200, m68k::FC_SUPER_DATA), 0x21);
    m.write8(0xece21c, 0x49, m68k::FC_SUPER_DATA); // DCR (WTS)
    m.write8(0xece214, 0x10, m68k::FC_SUPER_DATA); // RBCR0
    m.write8(0xece216, 0x00, m68k::FC_SUPER_DATA);
    m.write8(0xece210, 0x00, m68k::FC_SUPER_DATA); // RSAR0
    m.write8(0xece212, 0x00, m68k::FC_SUPER_DATA);
    m.write8(0xece200, 0x0a, m68k::FC_SUPER_DATA); // RD0、STA
    CHECK_EQ(m.read16(0xece220, m68k::FC_SUPER_DATA), kMac[0] << 8 | kMac[0]);
    CHECK_EQ(m.read16(0xece220, m68k::FC_SUPER_DATA), kMac[1] << 8 | kMac[1]);

    // 割り込み: IMR の PRX を立てて受信すると、レベル 2 でベクタ $F9
    Drv d(n);
    d.init(kMac);
    d.out(IMR, 0x01);
    CHECK_EQ(m.interruptAck(2), -2); // 要求がなければスプリアス
    const Frame f = makeFrame(kMac, kPeer, 60);
    n.receiveFrame(f.data(), 60);
    CHECK_EQ(m.interruptAck(2), 0xf9);
    CHECK_EQ(m.interruptAck(2), 0xf9); // ISR を消すまで続く
    m.write8(0xece20e, 0x01, m68k::FC_SUPER_DATA);
    CHECK_EQ(m.interruptAck(2), -2);

    // ステート: ボードの状態が戻る。ボードのない Machine には読まない
    n.receiveFrame(f.data(), 60);
    std::vector<uint8_t> buf(x68k::stateSize(m));
    size_t written = 0;
    CHECK_EQ(x68k::saveState(m, buf.data(), buf.size(), written), true);
    x68k::Machine m2;
    m2.setNeptune(true);
    const char* err = nullptr;
    CHECK_EQ(x68k::loadState(m2, buf.data(), written, err), true);
    CHECK_EQ(m2.neptune()->curr(), n.curr());
    CHECK_EQ(m2.neptune()->isr(), n.isr());
    for (int a = 0x4000; a < 0x8000; a += 0x100) CHECK_EQ(m2.neptune()->memRead((uint16_t)(a + 4)), n.memRead((uint16_t)(a + 4)));
    // Neptune-X 付きの前の版のステートも読める: 版 10 (送信の残りの時間がない)、版 11 (それが入った)、
    // 版 12 (SASI と SPC の書き直し)、版 13 (ADPCM の書き直し。SCC は前の並び)、今の版 (SCC の新しい並び、版 14 から)
    {
        const uint32_t versions[] = {10, 11, 12, 13, x68k::STATE_VERSION};
        size_t sizes[5] = {};
        for (int i = 0; i < 5; i++) {
            std::vector<uint8_t> old(x68k::stateSize(m) + 4096);
            size_t len = 0;
            CHECK_EQ(x68k::saveState(m, old.data(), old.size(), len, versions[i]), true);
            CHECK_EQ(old[8], (uint8_t)versions[i]);
            sizes[i] = len;
            x68k::Machine mo;
            mo.setNeptune(true);
            const char* e = nullptr;
            CHECK_EQ(x68k::loadState(mo, old.data(), len, e), true);
            CHECK_EQ(mo.neptune()->curr(), n.curr());
            CHECK_EQ(mo.neptune()->isr(), n.isr());
        }
        CHECK_EQ(sizes[1] > sizes[0], true); // 版 11 で Neptune-X の送信の残りの時間が増えた
        CHECK_EQ(sizes[2] < sizes[1], true); // 版 12 で SASI と SPC を書き直し、CD-ROM 装置がなければ小さくなった
        CHECK_EQ(sizes[3], sizes[2]);        // 版 13 は ADPCM の中身の並びが変わっただけ
        CHECK_EQ(x68k::STATE_SCC_RS232C, 14u);
        // 版 14 で SCC の並びが変わった: チャネルごとの 16 バイト x 2 が、40 バイト x 2 + ポインタ 4 + 時刻 8 になった
        // 今の版は、さらに版 20 の音のフィルタの履歴 (256 バイト) が増えている
        CHECK_EQ(sizes[4], sizes[3] + 2 * (40 - 16) + 4 + 8 + 2 * 4 * x68k::Sound::RESAMPLE_TAPS);
    }
    // 前の実装が書いた固定データ (tests/data/gen_neptune_state.cpp で作った。同じ実装で書いて読むだけだと、書く側と
    // 読む側が同時にずれても分からないため): 版 10 と版 13 を今の実装で読む
    for (const char* name : {"neptune_v10.state", "neptune_v13.state"}) {
        char path[1024];
        snprintf(path, sizeof path, "%s/tests/data/%s", PIX68K_SOURCE_DIR, name);
        FILE* f = fopen(path, "rb");
        CHECK_EQ(f != nullptr, true);
        if (!f) continue;
        std::vector<uint8_t> data(1 << 20);
        const size_t len = fread(data.data(), 1, data.size(), f);
        fclose(f);
        x68k::Machine mf;
        mf.setNeptune(true);
        const char* e = nullptr;
        CHECK_EQ(x68k::loadState(mf, data.data(), len, e), true);
        CHECK_EQ(mf.neptune()->curr(), 0x47);
        // SCC: チャネル B で WR12 を選んだまま保存した (版 13 までの並びのポインタ)。読むと WR12 の $0E
        CHECK_EQ(mf.read8(0xe98001, m68k::FC_SUPER_DATA), 0x0e);
        // 読んだものを今の版で書き直して読める
        std::vector<uint8_t> again(x68k::stateSize(mf) + 4096);
        size_t n2 = 0;
        CHECK_EQ(x68k::saveState(mf, again.data(), again.size(), n2), true);
        x68k::Machine mg;
        mg.setNeptune(true);
        CHECK_EQ(x68k::loadState(mg, again.data(), n2, e), true);
        CHECK_EQ(mg.neptune()->curr(), 0x47);
    }
    x68k::Machine m3;
    CHECK_EQ(x68k::loadState(m3, buf.data(), written, err), false);
    if (err) CHECK_EQ(strcmp(err, "expansion boards differ"), 0);
    // ボードのない Machine のステートは、ボードのない Machine に読める (前と同じ)
    std::vector<uint8_t> buf3(x68k::stateSize(m3));
    CHECK_EQ(x68k::saveState(m3, buf3.data(), buf3.size(), written), true);
    x68k::Machine m4;
    CHECK_EQ(x68k::loadState(m4, buf3.data(), written, err), true);
}

// 68000 の命令で PROM を読み、受信の割り込みを受ける
struct Rom {
    uint8_t b[x68k::Machine::IPL_SIZE];
    uint32_t pos = 0x100;
    Rom() {
        memset(b, 0xff, sizeof b);
        put32(0x10000, 0x00002000);
        put32(0x10004, 0x00fe0100);
        put16(0x0f00, 0x7e01); // バスエラー: moveq #1,d7
        put16(0x0f02, 0x60fe);
        // 割り込み ($F9) の処理: 数を数え、ISR を読んで消す
        put16(0x0e00, 0x5285);                         // addq.l #1,d5
        put16(0x0e02, 0x1839); put32(0x0e04, 0x00ece20e); // move.b $ece20e,d4
        put16(0x0e08, 0x13c4); put32(0x0e0a, 0x00ece20e); // move.b d4,$ece20e
        put16(0x0e0e, 0x4e73);                         // rte
        code({0x21fc, 0x00fe, 0x0f00, 0x0008});         // move.l #$fe0f00,$8.w
        code({0x21fc, 0x00fe, 0x0e00, 0x03e4});         // move.l #$fe0e00,$3e4.w (ベクタ $F9)
    }
    void put16(uint32_t o, uint16_t v) {
        b[o] = (uint8_t)(v >> 8);
        b[o + 1] = (uint8_t)v;
    }
    void put32(uint32_t o, uint32_t v) {
        put16(o, (uint16_t)(v >> 16));
        put16(o + 2, (uint16_t)v);
    }
    void code(std::initializer_list<uint16_t> words) {
        for (uint16_t w : words) {
            put16(pos, w);
            pos += 2;
        }
    }
    // move.b #v,$ECE200+r*2
    void out(int r, uint8_t v) { code({0x13fc, v, 0x00ec, (uint16_t)(0xe200 + r * 2)}); }
};

void testFrom68000() {
    printf("neptune: driven by 68000 code, interrupt at level 2\n");
    Rom rom;
    rom.code({0x1039, 0x00ec, 0xe23e}); // move.b $ece23e,d0 (リセット)
    rom.out(CR, 0x21);
    rom.out(DCR, 0x49);
    rom.out(RBCR0, 2);
    rom.out(RBCR1, 0);
    rom.out(RSAR0, 0);
    rom.out(RSAR1, 0);
    rom.out(CR, 0x0a);
    rom.code({0x3239, 0x00ec, 0xe220}); // move.w $ece220,d1 (PROM の 1 バイト目が 2 つ)
    rom.out(PSTART, 0x46);
    rom.out(PSTOP, 0x80);
    rom.out(BNRY, 0x46);
    rom.out(CR, 0x61);
    for (int i = 0; i < 6; i++) rom.out(PAR0 + i, kMac[i]);
    rom.out(CURR, 0x47);
    rom.out(CR, 0x21);
    rom.out(RCR, 0x04);
    rom.out(TCR, 0x00);
    rom.out(ISR, 0xff);
    rom.out(IMR, 0x01);
    rom.out(CR, 0x22);
    rom.code({0x7a00, 0x46fc, 0x2000}); // moveq #0,d5; move.w #$2000,sr (割り込みを受け付ける)
    rom.code({0x7c01, 0x60fe});         // moveq #1,d6; bra.s *
    x68k::Machine m;
    m.setNeptune(true);
    m.neptune()->setMac(kMac);
    m.setIplRom(rom.b, sizeof rom.b);
    m.powerOn();
    m.run(20000);
    m68k::Registers r;
    m.getRegisters(r);
    CHECK_EQ(r.d[7], 0);
    CHECK_EQ(r.d[6], 1);
    CHECK_EQ(r.d[1] & 0xffff, kMac[0] << 8 | kMac[0]);
    CHECK_EQ(r.d[5], 0);
    // 線からフレームが届く: Machine がラスタの始まりで取り込み、割り込みが入る
    TestLink link;
    m.neptune()->setLink(&link);
    link.inbox.push_back(makeFrame(kMac, kPeer, 60));
    m.run(20000);
    m.getRegisters(r);
    CHECK_EQ(r.d[5], 1);
    CHECK_EQ(r.d[4] & 0xff, 0x01); // PRX
    CHECK_EQ(m.neptune()->received(), 1);
}

// ---------------------------------------------------------------- 環境の側の口

// Machine につないだとき、送信の時間はラスタごとに進む (Machine::startLine() が poll() にラスタの長さを渡す)
void testTransmitTimeOnMachine() {
    printf("neptune: transmit time advances with the machine\n");
    Rom rom;
    rom.code({0x60fe}); // bra.s *
    x68k::Machine m;
    CHECK_EQ(m.setNeptune(true), true);
    TestLink link;
    m.neptune()->setLink(&link);
    m.setIplRom(rom.b, sizeof rom.b);
    m.powerOn();
    m.run(20000);
    Drv d(*m.neptune());
    d.init(kMac);
    d.out(ISR, 0xff);
    const Frame f = makeFrame(kPeer, kMac, 1000); // 送るのに 819.2us
    d.writemem(f.data(), d.txPage << 8, (int)f.size());
    d.out(TPSR, d.txPage);
    d.out(TBCR0, (uint8_t)f.size());
    d.out(TBCR1, (uint8_t)(f.size() >> 8));
    const int fc = m68k::FC_SUPER_DATA;
    m.write8(0xece200, CR_RD2 | CR_TXP | CR_STA, fc);
    CHECK_EQ(link.sent.size(), 1); // フレームは TXP を書いたときに送る
    // TXP を書いたラスタの分は数えないので、819.2us より前には送り終えない
    m.run(16 * 819); // 819us (16MHz)
    CHECK_EQ(m.neptune()->transmitting(), true);
    CHECK_EQ(m.read8(0xece20e, fc) & 0x02, 0);
    // 遅れは 2 ラスタ (約 128us) より小さい
    m.run(16 * 181); // 合わせて 1000us
    CHECK_EQ(m.neptune()->transmitting(), false);
    CHECK_EQ(link.sent.size(), 1);
    CHECK_EQ(m.read8(0xece20e, fc) & 0x02, 0x02);
}

void testFrameRing() {
    printf("frame ring: single producer / single consumer\n");
    platform::FrameRing<4> ring;
    uint8_t buf[1600];
    CHECK_EQ(ring.pop(buf, sizeof buf), 0);
    for (int i = 0; i < 4; i++) {
        const Frame f = makeFrame(kMac, kPeer, 60 + i, (uint8_t)i);
        CHECK_EQ(ring.push(f.data(), (int)f.size()), true);
    }
    const Frame extra = makeFrame(kMac, kPeer, 60);
    CHECK_EQ(ring.push(extra.data(), 60), false); // 満杯
    CHECK_EQ(ring.overflows(), 1);
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(ring.pop(buf, sizeof buf), 60 + i);
        CHECK_EQ(buf[14], (uint8_t)(14 * 7 + i));
    }
    CHECK_EQ(ring.empty(), true);
    // 2 つのスレッドで、順番と中身が変わらない
    static platform::FrameRing<16> shared;
    const int count = 20000;
    std::thread producer([&] {
        uint8_t f[100];
        for (int i = 0; i < count;) {
            for (int k = 0; k < 100; k++) f[k] = (uint8_t)(i + k);
            if (shared.push(f, 14 + i % 86)) i++;
        }
    });
    int bad = 0;
    for (int i = 0; i < count;) {
        const int n = shared.pop(buf, sizeof buf);
        if (n == 0) continue;
        if (n != 14 + i % 86 || buf[0] != (uint8_t)i || buf[n - 1] != (uint8_t)(i + n - 1)) bad++;
        i++;
    }
    producer.join();
    CHECK_EQ(bad, 0);
}

// IPv4 のヘッダのチェックサムを確かめる (正しければ 0)
uint16_t sum16(const uint8_t* p, int n) {
    uint32_t s = 0;
    for (int i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

Frame arpRequest(const uint8_t* ip, const uint8_t* target) {
    static const uint8_t kBroadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    Frame f(42);
    memcpy(f.data(), kBroadcast, 6);
    memcpy(f.data() + 6, kMac, 6);
    const uint8_t hdr[] = {0x08, 0x06, 0, 1, 0x08, 0x00, 6, 4, 0, 1};
    memcpy(f.data() + 12, hdr, sizeof hdr);
    memcpy(f.data() + 22, kMac, 6);
    memcpy(f.data() + 28, ip, 4);
    memcpy(f.data() + 38, target, 4);
    return f;
}

Frame echoRequest(const uint8_t* dstMac, const uint8_t* ip, const uint8_t* target, int payload, uint16_t seq) {
    Frame f((size_t)(14 + 20 + 8 + payload));
    memcpy(f.data(), dstMac, 6);
    memcpy(f.data() + 6, kMac, 6);
    f[12] = 0x08;
    uint8_t* h = f.data() + 14;
    h[0] = 0x45;
    h[2] = (uint8_t)((28 + payload) >> 8);
    h[3] = (uint8_t)(28 + payload);
    h[8] = 30;
    h[9] = 1;
    memcpy(h + 12, ip, 4);
    memcpy(h + 16, target, 4);
    const uint16_t hs = sum16(h, 20);
    h[10] = (uint8_t)(hs >> 8);
    h[11] = (uint8_t)hs;
    uint8_t* c = h + 20;
    c[0] = 8;
    c[4] = 0x12;
    c[5] = 0x34;
    c[6] = (uint8_t)(seq >> 8);
    c[7] = (uint8_t)seq;
    for (int i = 0; i < payload; i++) c[8 + i] = (uint8_t)i;
    const uint16_t cs = sum16(c, 8 + payload);
    c[2] = (uint8_t)(cs >> 8);
    c[3] = (uint8_t)cs;
    return f;
}

// ドライバの手順で、EchoPeer に ARP を問い合わせて ping する
void testPingThroughEchoPeer() {
    printf("neptune: ARP and ping through the echo peer\n");
    static const uint8_t kIp[4] = {192, 168, 68, 2}, kGw[4] = {192, 168, 68, 1};
    x68k::Machine m;
    m.setNeptune(true);
    Neptune& n = *m.neptune();
    platform::EchoPeer peer;
    n.setLink(&peer);
    n.setMac(kMac);
    Drv d(n);
    d.reset();
    d.init(kMac);
    d.out(IMR, 0x01);
    Frame f = arpRequest(kIp, kGw);
    d.send(f.data(), (int)f.size());
    CHECK_EQ(peer.arpReplies(), 1);
    // Machine はラスタごとに 1 つ取り込む
    m.run(2000);
    CHECK_EQ(n.irq(), true);
    Frame r = d.receive();
    d.out(ISR, 0x01);
    CHECK_EQ(r.size(), 60); // 42 バイトを 60 に埋める
    if (r.size() == 60) {
        CHECK_EQ(memcmp(r.data(), kMac, 6), 0);
        CHECK_EQ(memcmp(r.data() + 6, platform::EchoPeer::PEER_MAC, 6), 0);
        CHECK_EQ(r[21], 2); // ARP の答え
        CHECK_EQ(memcmp(r.data() + 28, kGw, 4), 0);
        CHECK_EQ(memcmp(r.data() + 22, platform::EchoPeer::PEER_MAC, 6), 0);
    }
    // 自分のアドレスの知らせには答えない
    f = arpRequest(kIp, kIp);
    d.send(f.data(), (int)f.size());
    CHECK_EQ(peer.arpReplies(), 1);
    // 送り元が 0.0.0.0 の問い合わせ (アドレスが重なっていないかの確認) にも答えない
    static const uint8_t kZeroIp[4] = {0, 0, 0, 0};
    f = arpRequest(kZeroIp, kGw);
    d.send(f.data(), (int)f.size());
    CHECK_EQ(peer.arpReplies(), 1);
    for (uint16_t seq = 0; seq < 3; seq++) {
        f = echoRequest(platform::EchoPeer::PEER_MAC, kIp, kGw, 56 + seq * 500, seq);
        d.send(f.data(), (int)f.size());
        m.run(2000);
        r = d.receive();
        d.out(ISR, 0x01);
        CHECK_EQ(r.size(), f.size());
        if (r.size() != f.size()) continue;
        const uint8_t* h = r.data() + 14;
        CHECK_EQ(sum16(h, 20), 0);
        CHECK_EQ(memcmp(h + 12, kGw, 4), 0);
        CHECK_EQ(memcmp(h + 16, kIp, 4), 0);
        CHECK_EQ(h[20], 0); // echo reply
        CHECK_EQ(sum16(h + 20, (int)f.size() - 34), 0);
        CHECK_EQ(h[26] << 8 | h[27], seq);
        CHECK_EQ(memcmp(h + 28, f.data() + 42, f.size() - 42), 0);
    }
    CHECK_EQ(peer.echoReplies(), 3);
    CHECK_EQ(d.receive().empty(), true);
}

void testLoopbackLink() {
    printf("neptune: loopback link returns frames to itself\n");
    Neptune n;
    platform::LoopbackLink link;
    n.setLink(&link);
    Drv d(n);
    d.init(kMac);
    const Frame own = makeFrame(kMac, kMac, 90), other = makeFrame(kPeer, kMac, 90);
    d.send(other.data(), 90);
    d.send(own.data(), 90);
    n.poll(0); // 宛てが違う: フィルタで捨てる
    n.poll(0);
    const Frame r = d.receive();
    CHECK_EQ(r.size(), 90);
    if (r.size() == 90) CHECK_EQ(memcmp(r.data(), own.data(), 90), 0);
    CHECK_EQ(d.receive().empty(), true);
}

} // namespace

int main() {
    testResetAndProm();
    testTransmit();
    testTransmitTime();
    testReceive();
    testFilter();
    testRingWrapAndOverflow();
    testLoopbackAndByteOrder();
    testPoll();
    testHeaderLength();
    testRemoteDma();
    testMachine();
    testFrom68000();
    testTransmitTimeOnMachine();
    testFrameRing();
    testPingThroughEchoPeer();
    testLoopbackLink();
    return finish();
}
