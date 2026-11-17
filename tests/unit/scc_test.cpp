// SCC (src/x68k/scc.cpp) のテスト: レジスタの選択と読み書き、RR0 / RR3、受信の FIFO とオーバーラン、
// 受信割り込みのモード、MIE、ベクタ (状態で書き換える / 書き換えない、上位 / 下位)、IUS、RTS の通知、
// 2 つのチャネルで共通のレジスタの選択、RS-232C (チャネル A) のボーレートと送受信、特殊受信条件、外部 / 状態の割り込み、
// 相手を外したときの DCD / CTS、RingSerialLink の受信のリングの読み残しを捨てる依頼
#include "test_check.h"
#include "platform/serial_ring.h"
#include "x68k/scc.h"
#include "x68k/state.h"

#include <deque>
#include <vector>

namespace {

using x68k::Scc;

// コマンドのポート: B は offset 1、A は 5
void wr(Scc& s, int ch, int reg, uint8_t v) {
    uint32_t cmd = ch == Scc::CH_A ? 5 : 1;
    if (reg >= 8) s.write(cmd, (uint8_t)(0x08 | (reg & 7))); // Point High
    else if (reg != 0) s.write(cmd, (uint8_t)reg);
    s.write(cmd, v);
}

uint8_t rr(Scc& s, int ch, int reg) {
    uint32_t cmd = ch == Scc::CH_A ? 5 : 1;
    if (reg >= 8) s.write(cmd, (uint8_t)(0x08 | (reg & 7)));
    else if (reg != 0) s.write(cmd, (uint8_t)reg);
    return s.read(cmd);
}

struct RtsLog : Scc::Listener {
    int changes = 0;
    bool last = false;
    int lastCh = -1;
    void onRts(int ch, bool asserted) override {
        changes++;
        last = asserted;
        lastCh = ch;
    }
};

// IPL ROM と同じ設定にする (トレースで見た値)
void setupLikeIpl(Scc& s) {
    s.reset();
    wr(s, Scc::CH_A, 9, 0xc0);
    wr(s, Scc::CH_A, 2, 0x50);
    wr(s, Scc::CH_B, 9, 0x40);
    wr(s, Scc::CH_B, 4, 0x4c);
    wr(s, Scc::CH_B, 3, 0xc1);
    wr(s, Scc::CH_B, 5, 0x60);
    wr(s, Scc::CH_B, 1, 0x10);
    wr(s, Scc::CH_B, 9, 0x09);
}

void testRegisters() {
    printf("registers\n");
    Scc s;
    s.reset();
    wr(s, Scc::CH_B, 12, 0x1f);
    wr(s, Scc::CH_B, 13, 0x00);
    CHECK_EQ(rr(s, Scc::CH_B, 12), 0x1f);
    CHECK_EQ(s.wr(Scc::CH_B, 12), 0x1f);
    CHECK_EQ(s.wr(Scc::CH_A, 12), 0x00);
    // WR2 と WR9 は両チャネルで共通
    wr(s, Scc::CH_B, 2, 0x50);
    CHECK_EQ(s.wr(Scc::CH_A, 2), 0x50);
    CHECK_EQ(rr(s, Scc::CH_A, 2), 0x50);
    // ポインタは 1 回の読み書きで 0 に戻る: 次のコマンドポートの読みは RR0
    s.write(1, 0x0c);
    s.read(1);
    CHECK_EQ(s.read(1) & 0x04, 0x04); // RR0: 送信バッファ空
    // 受信がなければ RR0 bit0 = 0
    CHECK_EQ(rr(s, Scc::CH_B, 0) & 0x01, 0);
    // 偶数番地はつながっていない
    CHECK_EQ(s.read(0), 0xff);
}

void testReceive() {
    printf("receive fifo\n");
    Scc s;
    setupLikeIpl(s);
    CHECK_EQ(s.rxEnabled(Scc::CH_B), 1);
    s.receive(Scc::CH_B, 0x11);
    s.receive(Scc::CH_B, 0x22);
    CHECK_EQ(rr(s, Scc::CH_B, 0) & 0x01, 1);
    CHECK_EQ(s.read(3), 0x11);
    CHECK_EQ(rr(s, Scc::CH_B, 8), 0x22); // RR8 も受信データ
    CHECK_EQ(rr(s, Scc::CH_B, 0) & 0x01, 0);
    // 4 バイト目はオーバーラン (最後を上書きし、そのバイトをオーバーランとして記録する)。そのバイトが先頭に来ると RR1 bit5
    for (int i = 0; i < 4; i++) s.receive(Scc::CH_B, (uint8_t)(0x30 + i));
    CHECK_EQ(rr(s, Scc::CH_B, 1) & 0x20, 0);
    CHECK_EQ(s.read(3), 0x30);
    CHECK_EQ(s.read(3), 0x31);
    CHECK_EQ(rr(s, Scc::CH_B, 1) & 0x20, 0x20);
    CHECK_EQ(s.read(3), 0x33);
    CHECK_EQ(rr(s, Scc::CH_B, 1) & 0x20, 0x20); // Error Reset まで残る
    wr(s, Scc::CH_B, 0, 0x30); // Error Reset
    CHECK_EQ(rr(s, Scc::CH_B, 1) & 0x20, 0);
    // 受信が許可されていなければ捨てる
    wr(s, Scc::CH_B, 3, 0xc0);
    s.receive(Scc::CH_B, 0x55);
    CHECK_EQ(s.rxCount(Scc::CH_B), 0);
}

void testInterrupts() {
    printf("interrupts and vectors\n");
    Scc s;
    setupLikeIpl(s);
    CHECK_EQ(s.irq(), 0);
    s.receive(Scc::CH_B, 0x01);
    CHECK_EQ(s.irq(), 1);
    CHECK_EQ(rr(s, Scc::CH_A, 3), 0x04); // RR3: B の受信
    CHECK_EQ(rr(s, Scc::CH_B, 3), 0x00); // RR3 は A だけ
    CHECK_EQ(rr(s, Scc::CH_B, 2), 0x54); // B 受信の符号 010 を bit3-1 に
    CHECK_EQ(s.ack(), 0x54);
    // 応答したので IUS が立ち、同じ要因はもう出ない
    s.receive(Scc::CH_B, 0x02);
    CHECK_EQ(s.irq(), 0);
    s.read(3);
    s.read(3);
    wr(s, Scc::CH_A, 0, 0x38); // Reset Highest IUS
    CHECK_EQ(s.irq(), 0);      // FIFO は空
    s.receive(Scc::CH_B, 0x03);
    CHECK_EQ(s.irq(), 1);
    // MIE を落とすと出ない
    wr(s, Scc::CH_A, 9, 0x01);
    CHECK_EQ(s.irq(), 0);
    // VIS なし: WR2 そのもの
    wr(s, Scc::CH_A, 9, 0x08);
    CHECK_EQ(s.ack(), 0x50);
    wr(s, Scc::CH_A, 0, 0x38);
    // 状態を上位に (bit4): 符号 010 は bit6-4 で 010
    wr(s, Scc::CH_A, 9, 0x19);
    CHECK_EQ(s.ack(), (0x50 & 0x8f) | 0x20); // bit6-4 は置き換わる
    wr(s, Scc::CH_A, 0, 0x38);
    // NV: ベクタを出さない
    wr(s, Scc::CH_A, 9, 0x0b);
    CHECK_EQ(s.ack() < 0, 1);
    wr(s, Scc::CH_A, 0, 0x38);
    s.read(3);
    // 要求がないときの RR2 (B) は符号 011
    wr(s, Scc::CH_A, 9, 0x09);
    CHECK_EQ(rr(s, Scc::CH_B, 2), 0x56);
}

// RR2 (Z8530 Technical Manual 7.2.3): チャネル A は WR2 のまま。チャネル B は、VIS に関係なく、優先度の最も高い
// 要求 (IP) の状態で書き換えたベクタ。IP は MIE や IUS では変わらないので、割り込みを出していない間も同じ値が読める
void testRr2Status() {
    printf("rr2 of channel b\n");
    Scc s;
    setupLikeIpl(s); // WR2 = $50、B の受信割り込み、MIE と VIS
    CHECK_EQ(rr(s, Scc::CH_A, 2), 0x50);
    CHECK_EQ(rr(s, Scc::CH_B, 2), 0x56); // 要求なし: 011
    s.receive(Scc::CH_B, 0x11);
    CHECK_EQ(rr(s, Scc::CH_B, 2), 0x54); // B 受信: 010
    CHECK_EQ(rr(s, Scc::CH_A, 2), 0x50); // A は書き換えない
    // VIS = 0 でも B の RR2 は書き換わる (応答で出るベクタは WR2 のまま)
    wr(s, Scc::CH_A, 9, 0x08);
    CHECK_EQ(rr(s, Scc::CH_B, 2), 0x54); // VIS = 0 でも B 受信の符号
    // MIE = 0: 割り込みは出ないが、IP は立ったまま
    wr(s, Scc::CH_A, 9, 0x01);
    CHECK_EQ(s.irq(), 0);
    CHECK_EQ(rr(s, Scc::CH_B, 2), 0x54); // MIE = 0 でも IP は残る
    // 応答して IUS が立っている間 (同じ要因の割り込みは出ない) も、IP があれば読める
    wr(s, Scc::CH_A, 9, 0x09);
    CHECK_EQ(s.ack(), 0x54);
    CHECK_EQ(s.irq(), 0); // 応答中の要因と同じ要因は出さない
    CHECK_EQ(rr(s, Scc::CH_B, 2), 0x54); // それでも RR2 には出る
    // 優先度の高い要求 (A の送信: 100) があれば、そちらの状態になる。チャネル A は送信を許可して (WR5 bit3) 書く
    // (バイトがすぐシフトレジスタに移り、送信バッファが空いて要求が出る)
    wr(s, Scc::CH_A, 1, 0x02);
    wr(s, Scc::CH_A, 5, 0x08);
    s.write(7, 0x00);
    CHECK_EQ(rr(s, Scc::CH_B, 2), 0x58);
    // 状態を上位に置く (WR9 bit4): V4 V5 V6 = 符号の bit2、bit1、bit0。A の送信 100 は bit4 だけが 1
    wr(s, Scc::CH_A, 9, 0x19);
    CHECK_EQ(rr(s, Scc::CH_B, 2), 0x10);
    CHECK_EQ(rr(s, Scc::CH_A, 2), 0x50);
    // 要求を全部下ろすと、上位に置いたときの「要求なし」は 110
    wr(s, Scc::CH_A, 0, 0x28); // Reset Tx Int Pending
    s.read(3); // B のデータを読んで、B の受信の要求も下ろす
    CHECK_EQ(rr(s, Scc::CH_B, 2), 0x60);
}

void testStatusHighCodes() {
    printf("status high vector codes\n");
    Scc s;
    setupLikeIpl(s);
    wr(s, Scc::CH_A, 2, 0x00);
    wr(s, Scc::CH_B, 1, 0x12);
    wr(s, Scc::CH_A, 9, 0x19);
    s.write(3, 0x00); // B 送信: 符号 000 → 000
    CHECK_EQ(s.ack(), 0x00);
    wr(s, Scc::CH_A, 0, 0x38);
    wr(s, Scc::CH_B, 0, 0x28);
    // A の受信 (符号 110) は上位では 011 (bit4 と bit5)
    wr(s, Scc::CH_A, 3, 0xc1);
    wr(s, Scc::CH_A, 1, 0x10);
    s.receive(Scc::CH_A, 0x01);
    CHECK_EQ(s.ack(), 0x30);
    wr(s, Scc::CH_A, 0, 0x38);
    // 下位では 110 を bit3-1 に
    wr(s, Scc::CH_A, 9, 0x09);
    CHECK_EQ(s.ack(), 0x0c);
}

void testFirstCharMode() {
    printf("rx interrupt on first character\n");
    Scc s;
    setupLikeIpl(s);
    wr(s, Scc::CH_B, 1, 0x08); // 最初の 1 文字
    s.receive(Scc::CH_B, 0x01);
    CHECK_EQ(s.irq(), 1);
    CHECK_EQ(s.ack(), 0x54);
    wr(s, Scc::CH_A, 0, 0x38);
    // 応答しても要求は残り、その文字を読み出したときに下ろす
    CHECK_EQ(s.irq(), 1);
    CHECK_EQ(s.read(3), 0x01);
    CHECK_EQ(s.irq(), 0);
    s.receive(Scc::CH_B, 0x02);
    CHECK_EQ(s.irq(), 0); // 2 文字目では出ない
    // Enable Int on Next Rx Character: FIFO に文字が残っていれば、すぐ出す
    wr(s, Scc::CH_B, 0, 0x20);
    CHECK_EQ(s.irq(), 1);
    CHECK_EQ(s.read(3), 0x02);
    CHECK_EQ(s.irq(), 0);
    wr(s, Scc::CH_B, 0, 0x20); // Enable Int on Next Rx Character
    s.receive(Scc::CH_B, 0x03);
    CHECK_EQ(s.irq(), 1);
}

void testTxAndPriority() {
    printf("tx interrupt and priority\n");
    Scc s;
    setupLikeIpl(s);
    wr(s, Scc::CH_B, 1, 0x12); // すべての文字 + 送信
    s.write(3, 0x41);           // 送信データ → 送信バッファ空の要求
    CHECK_EQ(rr(s, Scc::CH_A, 3), 0x02);
    s.receive(Scc::CH_B, 0x01);
    // 受信のほうが優先
    CHECK_EQ(s.ack(), 0x54);
    // 受信の IUS 中は、優先度の低い送信も出ない
    CHECK_EQ(s.irq(), 0);
    s.read(3);
    wr(s, Scc::CH_A, 0, 0x38);
    CHECK_EQ(s.ack(), 0x50); // B 送信の符号は 000
    wr(s, Scc::CH_A, 0, 0x38);
    wr(s, Scc::CH_B, 0, 0x28); // Reset Tx Int Pending
    CHECK_EQ(s.irq(), 0);
}

void testRts() {
    printf("rts\n");
    Scc s;
    RtsLog log;
    s.setListener(&log);
    setupLikeIpl(s);
    log.changes = 0;
    wr(s, Scc::CH_B, 5, 0x62);
    CHECK_EQ(log.changes, 1);
    CHECK_EQ(log.last, 1);
    CHECK_EQ(log.lastCh, Scc::CH_B);
    wr(s, Scc::CH_B, 5, 0x62); // 変わらなければ知らせない
    CHECK_EQ(log.changes, 1);
    wr(s, Scc::CH_B, 5, 0x60);
    CHECK_EQ(log.changes, 2);
    CHECK_EQ(log.last, 0);
    // チャネルのリセットで RTS は落ちる
    wr(s, Scc::CH_B, 5, 0x62);
    wr(s, Scc::CH_B, 9, 0x40);
    CHECK_EQ(log.last, 0);
    CHECK_EQ(s.rts(Scc::CH_B), 0);
    // ハードウェアリセットでも WR2 は残る
    CHECK_EQ(s.wr(Scc::CH_A, 2), 0x50);
    wr(s, Scc::CH_A, 9, 0xc0);
    CHECK_EQ(s.wr(Scc::CH_A, 2), 0x50);
    CHECK_EQ(s.wr(Scc::CH_A, 9), 0x00);
}

// RS-232C の相手の代わり
struct FakeLink : x68k::SerialLink {
    std::deque<uint8_t> in;
    std::vector<uint8_t> out;
    bool up = true;
    void send(uint8_t b) override { out.push_back(b); }
    int receive() override {
        if (in.empty()) return -1;
        int b = in.front();
        in.pop_front();
        return b;
    }
    bool carrier() override { return up; }
};

// IOCS の RS-232C と同じような設定: 9600bps (x16、TC=14)、8 ビット、1 ストップビット、パリティなし
void setupRs232c(Scc& s) {
    setupLikeIpl(s);
    wr(s, Scc::CH_A, 4, 0x44);
    wr(s, Scc::CH_A, 12, 14);
    wr(s, Scc::CH_A, 13, 0);
    wr(s, Scc::CH_A, 14, 0x03);
    wr(s, Scc::CH_A, 3, 0xc1);
    wr(s, Scc::CH_A, 5, 0xea); // DTR、8 ビット、送信の許可、RTS
}

void testSharedPointer() {
    printf("register pointer is shared by both channels\n");
    Scc s;
    s.reset();
    wr(s, Scc::CH_B, 12, 0x11);
    wr(s, Scc::CH_A, 12, 0x22);
    // A のコマンドのポートで WR12 を選び、B のコマンドのポートで読むと B の RR12
    s.write(5, 0x0c);
    CHECK_EQ(s.pointer(), 12);
    CHECK_EQ(s.read(1), 0x11);
    CHECK_EQ(s.pointer(), 0);
    // B で選んで A に書く
    s.write(1, 0x0d);
    s.write(5, 0x5a);
    CHECK_EQ(s.wr(Scc::CH_A, 13), 0x5a);
    CHECK_EQ(s.wr(Scc::CH_B, 13), 0x00);
    // データのポートはポインタを使わず、変えない
    s.write(5, 0x0c);
    s.read(7);
    s.write(3, 0x00);
    CHECK_EQ(s.pointer(), 12);
    CHECK_EQ(s.read(5), 0x22);
}

void testBaudAndTx() {
    printf("rs-232c baud rate and transmit\n");
    Scc s(16000000);
    FakeLink link;
    s.setLink(&link);
    setupRs232c(s);
    // 5MHz / (2 x 16 x 16) = 9765.6bps、10 ビットで 1.024ms = 16384 サイクル
    CHECK_EQ(s.txCharCycles(Scc::CH_A), 16384);
    // 2 ストップビット、パリティあり、7 ビット: 1 + 7 + 1 + 2 = 11 ビット
    wr(s, Scc::CH_A, 4, 0x4d);
    wr(s, Scc::CH_A, 5, 0xaa);
    CHECK_EQ(s.txCharCycles(Scc::CH_A), 16384 * 11 / 10);
    wr(s, Scc::CH_A, 4, 0x44);
    wr(s, Scc::CH_A, 5, 0xea);
    wr(s, Scc::CH_A, 1, 0x02); // 送信の割り込み
    uint64_t t = 1000;
    s.update(t);
    s.write(7, 'A');
    // シフトレジスタが空いているので、すぐバッファが空く (要求が出る)。送り終えるのは 1 文字の時間のあと
    CHECK_EQ(rr(s, Scc::CH_A, 0) & 0x04, 0x04);
    CHECK_EQ(rr(s, Scc::CH_A, 3), 0x10);
    CHECK_EQ(rr(s, Scc::CH_A, 1) & 0x01, 0);
    wr(s, Scc::CH_A, 0, 0x28); // Reset Tx Int Pending
    s.write(7, 'B');
    CHECK_EQ(rr(s, Scc::CH_A, 0) & 0x04, 0); // B はバッファで待つ
    CHECK_EQ(rr(s, Scc::CH_A, 3), 0x00);
    s.update(t + 16383);
    CHECK_EQ(link.out.size(), 0u);
    s.update(t + 16384);
    CHECK_EQ(link.out.size(), 1u);
    CHECK_EQ(link.out[0], 'A');
    CHECK_EQ(rr(s, Scc::CH_A, 0) & 0x04, 0x04);
    CHECK_EQ(rr(s, Scc::CH_A, 3), 0x10);
    // 少し遅れて呼んでも、2 文字目は 1 文字目を送り終えた時刻から数える
    s.update(t + 16384 * 2 + 100);
    CHECK_EQ(link.out.size(), 2u);
    CHECK_EQ(link.out[1], 'B');
    CHECK_EQ(rr(s, Scc::CH_A, 1) & 0x01, 0x01); // 全部送信済み
    // 送信を許可していなければ送らない
    wr(s, Scc::CH_A, 5, 0xe2);
    s.write(7, 'C');
    s.update(t + 16384 * 10);
    CHECK_EQ(link.out.size(), 2u);
    CHECK_EQ(rr(s, Scc::CH_A, 0) & 0x04, 0);
    wr(s, Scc::CH_A, 5, 0xea);
    s.update(t + 16384 * 11);
    CHECK_EQ(link.out.size(), 3u);
    // Auto Enables: CTS がなければ送らない
    link.up = false;
    s.update(t + 16384 * 12);
    wr(s, Scc::CH_A, 3, 0xe1);
    s.write(7, 'D');
    s.update(t + 16384 * 14);
    CHECK_EQ(link.out.size(), 3u);
    link.up = true;
    s.update(t + 16384 * 15);
    s.update(t + 16384 * 16);
    CHECK_EQ(link.out.size(), 4u);
    CHECK_EQ(link.out[3], 'D');
    // チャネル B は前と同じく、書いたらすぐ送り終えたことにする
    CHECK_EQ(rr(s, Scc::CH_B, 1) & 0x01, 0x01);
    s.write(3, 0x00);
    CHECK_EQ(rr(s, Scc::CH_B, 0) & 0x04, 0x04);
}

void testRxPacing() {
    printf("rs-232c receive pacing and rts\n");
    Scc s(16000000);
    FakeLink link;
    s.setLink(&link);
    setupRs232c(s);
    wr(s, Scc::CH_A, 1, 0x10); // すべての文字
    for (int i = 0; i < 5; i++) link.in.push_back((uint8_t)('a' + i));
    uint64_t t = 100000;
    s.update(t);
    CHECK_EQ(s.rxCount(Scc::CH_A), 1); // 1 文字の時間に 1 バイトまで
    CHECK_EQ(s.irq(), 1);
    CHECK_EQ(s.ack(), 0x5c); // A 受信の符号 110
    s.update(t + 16383);
    CHECK_EQ(s.rxCount(Scc::CH_A), 1);
    s.update(t + 16384);
    CHECK_EQ(s.rxCount(Scc::CH_A), 2);
    CHECK_EQ(s.read(7), 'a');
    CHECK_EQ(s.read(7), 'b');
    wr(s, Scc::CH_A, 0, 0x38);
    // RTS を下ろすと相手は送らない (バイトは相手の側に残る)
    wr(s, Scc::CH_A, 5, 0xe8);
    s.update(t + 16384 * 5);
    CHECK_EQ(s.rxCount(Scc::CH_A), 0);
    CHECK_EQ(link.in.size(), 3u);
    wr(s, Scc::CH_A, 5, 0xea);
    s.update(t + 16384 * 6);
    CHECK_EQ(s.rxCount(Scc::CH_A), 1);
    CHECK_EQ(s.read(7), 'c');
    // 読まずにいると FIFO があふれ、オーバーランの特殊受信条件になる (ベクタの符号 111)
    link.in.clear();
    for (int i = 0; i < 4; i++) link.in.push_back((uint8_t)('0' + i));
    for (int i = 7; i < 12; i++) s.update(t + 16384 * (uint64_t)i);
    CHECK_EQ(s.rxCount(Scc::CH_A), 3);
    CHECK_EQ(s.read(7), '0');
    CHECK_EQ(s.read(7), '1');
    CHECK_EQ(rr(s, Scc::CH_A, 1) & 0x20, 0x20);
    CHECK_EQ(rr(s, Scc::CH_A, 2), 0x50);     // A で読む RR2 は WR2 そのもの
    CHECK_EQ(rr(s, Scc::CH_B, 2), 0x5e);     // 特殊受信条件 (A) の符号 111
    CHECK_EQ(s.ack(), 0x5e);
    CHECK_EQ(s.read(7), '3');
    wr(s, Scc::CH_A, 0, 0x30); // Error Reset
    wr(s, Scc::CH_A, 0, 0x38);
    CHECK_EQ(s.irq(), 0);
    // つながっていなければ何も受け取らない
    s.setLink(nullptr);
    s.update(t + 16384 * 20);
    CHECK_EQ(s.rxCount(Scc::CH_A), 0);
}

// 01 (最初の 1 文字か特殊受信条件) のモードでは、特殊受信条件のバイトで FIFO が止まり、Error Reset で進む
void testSpecialLock() {
    printf("special receive condition locks the fifo\n");
    Scc s;
    setupLikeIpl(s);
    wr(s, Scc::CH_A, 3, 0xc1);
    wr(s, Scc::CH_A, 1, 0x08);
    for (int i = 0; i < 5; i++) s.receive(Scc::CH_A, (uint8_t)(0x40 + i)); // 40 41 44 (44 はオーバーラン)
    CHECK_EQ(s.ack(), 0x5c);   // 最初の 1 文字
    wr(s, Scc::CH_A, 0, 0x38);
    CHECK_EQ(s.read(7), 0x40);
    CHECK_EQ(s.irq(), 0);
    CHECK_EQ(s.read(7), 0x41);
    // オーバーランのバイトが先頭に来た: 特殊受信条件
    CHECK_EQ(s.irq(), 1);
    CHECK_EQ(rr(s, Scc::CH_A, 1) & 0x20, 0x20);
    CHECK_EQ(s.ack(), 0x5e);
    CHECK_EQ(s.read(7), 0x44);
    // 止まっている間は、届いたバイトも見せない
    s.receive(Scc::CH_A, 0x50);
    CHECK_EQ(rr(s, Scc::CH_A, 0) & 0x01, 0);
    CHECK_EQ(s.read(7), 0x00);
    wr(s, Scc::CH_A, 0, 0x30); // Error Reset
    wr(s, Scc::CH_A, 0, 0x38);
    CHECK_EQ(rr(s, Scc::CH_A, 0) & 0x01, 0x01);
    CHECK_EQ(s.irq(), 0); // 最初の 1 文字のモードなので、次の文字では出ない
    CHECK_EQ(s.read(7), 0x50);
    // 11 (特殊受信条件だけ): ふつうの文字では出ない
    wr(s, Scc::CH_A, 1, 0x18);
    s.receive(Scc::CH_A, 0x60);
    CHECK_EQ(s.irq(), 0);
}

void testExtStatus() {
    printf("external/status interrupts (dcd, cts)\n");
    Scc s;
    FakeLink link;
    link.up = false;
    s.setLink(&link);
    setupRs232c(s);
    s.update(10);
    CHECK_EQ(rr(s, Scc::CH_A, 0) & 0x28, 0x00);
    // 割り込みを許可していなければ、RR0 は線の値をそのまま見せる
    link.up = true;
    s.update(20);
    CHECK_EQ(rr(s, Scc::CH_A, 0) & 0x28, 0x28);
    CHECK_EQ(s.irq(), 0);
    // WR15 の既定 (リセットで $F8) は DCD と CTS を選んでいる
    CHECK_EQ(rr(s, Scc::CH_A, 15), 0xf8);
    wr(s, Scc::CH_A, 1, 0x01);
    link.up = false;
    s.update(30);
    CHECK_EQ(s.irq(), 1);
    CHECK_EQ(rr(s, Scc::CH_A, 3), 0x08);
    CHECK_EQ(s.ack(), 0x5a); // A 外部の符号 101
    // RR0 は止めた値のまま (線がまた変わっても)
    link.up = true;
    s.update(40);
    CHECK_EQ(rr(s, Scc::CH_A, 0) & 0x28, 0x00);
    // Reset Ext/Status: 止めている間に変わっていたので、また止めて要求を出す
    wr(s, Scc::CH_A, 0, 0x10);
    wr(s, Scc::CH_A, 0, 0x38);
    CHECK_EQ(rr(s, Scc::CH_A, 0) & 0x28, 0x28);
    CHECK_EQ(s.irq(), 1);
    wr(s, Scc::CH_A, 0, 0x10);
    CHECK_EQ(s.irq(), 0);
    // WR15 で選んでいなければ出ない
    wr(s, Scc::CH_A, 15, 0x00);
    link.up = false;
    s.update(50);
    CHECK_EQ(s.irq(), 0);
    CHECK_EQ(rr(s, Scc::CH_A, 0) & 0x28, 0x00);
    // チャネル B の DCD / CTS は 0 のまま
    CHECK_EQ(rr(s, Scc::CH_B, 0) & 0x28, 0x00);
}

// 状態の保存と復元: 送信の途中と受信の FIFO、レジスタの選択のポインタ。版 13 (前の並び) でも書いて読める
void testState() {
    printf("state\n");
    Scc a(16000000);
    FakeLink la;
    a.setLink(&la);
    setupRs232c(a);
    a.update(5000);
    a.write(7, 'X');
    a.write(7, 'Y'); // X は送っている途中、Y はバッファ
    a.receive(Scc::CH_A, 0x31);
    a.receive(Scc::CH_A, 0x32);
    a.write(1, 0x0c); // B のコマンドのポートで WR12 を選んだまま
    std::vector<uint8_t> buf(4096);
    x68k::StateIO save(x68k::StateIO::SAVE, buf.data(), buf.size());
    a.state(save);
    CHECK_EQ(save.ok(), 1);
    Scc b(16000000);
    FakeLink lb;
    b.setLink(&lb);
    b.reset();
    x68k::StateIO load(x68k::StateIO::LOAD, buf.data(), save.pos());
    b.state(load);
    CHECK_EQ(load.ok(), 1);
    CHECK_EQ(b.pointer(), 12);
    CHECK_EQ(b.rxCount(Scc::CH_A), 2);
    CHECK_EQ(b.txBusy(Scc::CH_A), 1);
    b.update(5000 + 16384);
    CHECK_EQ(lb.out.size(), 1u);
    CHECK_EQ(lb.out[0], 'X');
    b.update(5000 + 16384 * 2);
    CHECK_EQ(lb.out.size(), 2u);
    CHECK_EQ(lb.out[1], 'Y');
    CHECK_EQ(b.read(1), a.wr(Scc::CH_B, 12));
    // 版 13 (前の並び): 受信の FIFO と、選んでいたポインタは残る (送信の途中は残らない)
    std::vector<uint8_t> old(4096);
    x68k::StateIO saveOld(x68k::StateIO::SAVE, old.data(), old.size(), x68k::STATE_SCC_RS232C - 1);
    a.state(saveOld);
    CHECK_EQ(saveOld.ok(), 1);
    CHECK_EQ(saveOld.pos() < save.pos(), 1);
    Scc c;
    c.reset();
    x68k::StateIO loadOld(x68k::StateIO::LOAD, old.data(), saveOld.pos(), x68k::STATE_SCC_RS232C - 1);
    c.state(loadOld);
    CHECK_EQ(loadOld.ok(), 1);
    CHECK_EQ(c.pointer(), 12);
    CHECK_EQ(c.rxCount(Scc::CH_A), 2);
    CHECK_EQ(c.read(7), 0x31);
}

// 10 (すべての文字) のモードの特殊受信条件: そのバイトを読めば要求を下ろす。チャネル B のベクタの符号は 011
void testSpecialAllChars() {
    printf("special receive condition in the all-characters mode\n");
    Scc s;
    setupLikeIpl(s); // チャネル B は WR1 = $10 (すべての文字)、WR9 = $09 (MIE、ベクタを状態で書き換える、下位)
    for (int i = 0; i < 4; i++) s.receive(Scc::CH_B, (uint8_t)(0x40 + i)); // 40 41 43 (43 はオーバーラン)
    CHECK_EQ(s.ack(), 0x54); // B 受信 (010)
    wr(s, Scc::CH_A, 0, 0x38);
    CHECK_EQ(s.read(3), 0x40);
    CHECK_EQ(s.read(3), 0x41);
    CHECK_EQ(s.irq(), 1);
    CHECK_EQ(rr(s, Scc::CH_B, 1) & 0x20, 0x20);
    CHECK_EQ(rr(s, Scc::CH_B, 2), 0x56); // B 特殊受信条件 (011)
    CHECK_EQ(s.ack(), 0x56);
    wr(s, Scc::CH_A, 0, 0x38);
    CHECK_EQ(s.read(3), 0x43);
    // FIFO が空になれば要求はない。RR1 のオーバーランのフラグは Error Reset まで残る
    CHECK_EQ(s.irq(), 0);
    CHECK_EQ(rr(s, Scc::CH_B, 1) & 0x20, 0x20);
    // 止めないので、次のバイトはふつうの受信の要求になる
    s.receive(Scc::CH_B, 0x50);
    CHECK_EQ(rr(s, Scc::CH_B, 2), 0x54);
    CHECK_EQ(s.read(3), 0x50);
    wr(s, Scc::CH_B, 0, 0x30); // Error Reset
    CHECK_EQ(rr(s, Scc::CH_B, 1) & 0x20, 0x00);
    CHECK_EQ(s.irq(), 0);
}

// 相手を外したとき、相手のいないままステートを読んだときに、DCD / CTS を下ろして外部 / 状態の要求を出す
void testLinkRemoved() {
    printf("dcd and cts drop when the link is removed\n");
    Scc s;
    FakeLink link;
    s.setLink(&link);
    setupRs232c(s);
    s.update(10);
    CHECK_EQ(rr(s, Scc::CH_A, 0) & 0x28, 0x28);
    wr(s, Scc::CH_A, 1, 0x01);
    std::vector<uint8_t> buf(4096);
    x68k::StateIO save(x68k::StateIO::SAVE, buf.data(), buf.size());
    s.state(save);
    CHECK_EQ(save.ok(), 1);
    s.setLink(nullptr);
    s.update(20);
    CHECK_EQ(s.irq(), 1);
    CHECK_EQ(rr(s, Scc::CH_A, 3), 0x08);
    wr(s, Scc::CH_A, 0, 0x10);
    CHECK_EQ(rr(s, Scc::CH_A, 0) & 0x28, 0x00);
    // 線が 1 のときのステートを、相手なしで読む
    Scc b;
    b.reset();
    x68k::StateIO load(x68k::StateIO::LOAD, buf.data(), save.pos());
    b.state(load);
    CHECK_EQ(load.ok(), 1);
    b.update(30);
    CHECK_EQ(b.irq(), 1);
    wr(b, Scc::CH_A, 0, 0x10);
    CHECK_EQ(rr(b, Scc::CH_A, 0) & 0x28, 0x00);
}

// RingSerialLink: 相手が切れても受信のバイトは残る。次の相手がつながったら (dropRx())、読み残しは X68000 の側 (読み手) が
// 捨てる。捨て終えるまで書き手には空きを見せない
void testRingDrop() {
    printf("ring serial link drops unread bytes when the next peer connects\n");
    platform::RingSerialLink r;
    r.setCarrier(true);
    CHECK_EQ(r.rxSpace(), platform::RingSerialLink::SIZE);
    r.pushRx('a');
    r.pushRx('b');
    r.pushRx('c');
    r.setCarrier(false);
    CHECK_EQ(r.carrier(), 0);
    CHECK_EQ(r.receive(), 'a'); // 切れたあとも読める
    r.dropRx();                 // 次の相手がつながった
    CHECK_EQ(r.rxDrained(), 0);
    CHECK_EQ(r.rxSpace(), 0u);
    CHECK_EQ(r.carrier(), 0); // ここで捨てる
    CHECK_EQ(r.rxDrained(), 1);
    CHECK_EQ(r.rxSpace(), platform::RingSerialLink::SIZE);
    r.setCarrier(true);
    r.pushRx('d');
    CHECK_EQ(r.receive(), 'd');
    CHECK_EQ(r.receive(), -1);
}

// ネットワークの側の受信と送信の 1 回分 (ベアメタル版の CSerialTask が使う)。受信のリングが満ちていても読み、相手が
// 切れたことを見つける。入らないバイトは捨てて数える。前の相手の読み残しを捨て終えるまでは読まない
void testPump() {
    printf("network side pumps (bare metal rs232c)\n");
    platform::RingSerialLinkT<16, 16> r;
    uint8_t buf[8];
    unsigned dropped = 0;
    int calls = 0;
    auto data = [&calls](uint8_t* b, unsigned size) {
        calls++;
        for (unsigned i = 0; i < size; i++) b[i] = (uint8_t)i;
        return (int)size;
    };
    CHECK_EQ(platform::pumpRx(r, buf, 8, data, dropped), 8);
    CHECK_EQ(platform::pumpRx(r, buf, 8, data, dropped), 8);
    CHECK_EQ(r.rxSpace(), 0u);
    // リングが満ちていても読む (入らない分は捨てる)
    CHECK_EQ(platform::pumpRx(r, buf, 8, data, dropped), 8);
    CHECK_EQ(dropped, 8u);
    // リングが満ちたまま相手が切れた: 切れたと分かる
    auto closed = [](uint8_t*, unsigned) { return -1; };
    CHECK_EQ(platform::pumpRx(r, buf, 8, closed, dropped), -1);
    CHECK_EQ(r.receive(), 0); // リングの中身は残る
    // 次の相手: 読み残しを X68000 の側が捨てるまでは読まない
    r.dropRx();
    calls = 0;
    CHECK_EQ(platform::pumpRx(r, buf, 8, data, dropped), 0);
    CHECK_EQ(calls, 0);
    r.carrier();
    CHECK_EQ(platform::pumpRx(r, buf, 8, data, dropped), 8);
    CHECK_EQ(calls, 1);
    CHECK_EQ(r.receive(), 0);
    // 送信: 送れないときはリングから取り出さない
    for (int i = 0; i < 5; i++) r.send((uint8_t)(0x30 + i));
    unsigned sent = 0;
    auto out = [&sent](uint8_t*, unsigned len) {
        sent += len;
        return (int)len;
    };
    CHECK_EQ(platform::pumpTx(r, buf, 8, false, out), 0);
    CHECK_EQ(sent, 0u);
    CHECK_EQ(platform::pumpTx(r, buf, 8, true, out), 5);
    CHECK_EQ(sent, 5u);
    CHECK_EQ(buf[0], 0x30);
    r.send(1);
    CHECK_EQ(platform::pumpTx(r, buf, 8, true, [](uint8_t*, unsigned) { return -1; }), -1);
}

// 相手が閉じたときの判定 (ホスト版)。空きと「捨てる依頼が済んだか」は rxRoom() でそろえて取る。取ったあと、判定の前に
// X68000 の側が捨て終えても、取った値では「済んでいない」なので待つ (切って、届いたバイトを捨てない)
void testHangupDecision() {
    printf("hangup decision uses one snapshot of the receive ring\n");
    platform::RingSerialLinkT<16, 16> r;
    for (int i = 0; i < 16; i++) r.pushRx((uint8_t)i);
    r.dropRx(); // 次の相手がつながった (前の相手の 16 バイトが残っている)
    const auto room = r.rxRoom();
    CHECK_EQ(room.drained, 0);
    CHECK_EQ(room.space, 0u);
    r.carrier(); // ここで X68000 の側が捨て終える (空きを取ったあと、判定の前)
    CHECK_EQ(r.rxDrained(), 1);
    CHECK_EQ(platform::onHangup(room, true) == platform::HangupAction::WAIT_DRAIN, 1);
    // rxRoom() が 2 つの値を読む間に捨て終えても、そろった値になる (先に「済んだか」を読むので、済んでいない・空き 0)。
    // 空きを先に読む順だと、空き 0 のまま「済んだ」になり、DROP (届いたバイトを捨てて切る) になってしまう
    for (int i = 0; i < 16; i++) r.pushRx((uint8_t)i);
    r.dropRx();
    const auto mid = r.rxRoomWith([&r] { r.carrier(); });
    CHECK_EQ(r.rxDrained(), 1);
    CHECK_EQ(mid.drained == 0 || mid.space > 0, 1);
    CHECK_EQ(platform::onHangup(mid, true) == platform::HangupAction::DROP, 0);
    // 次に取り直せば空きがあるので読む
    const auto room2 = r.rxRoom();
    CHECK_EQ(room2.space, 16u);
    CHECK_EQ(platform::onHangup(room2, true) == platform::HangupAction::READ, 1);
    // 本当に満ちているときだけ切る
    for (int i = 0; i < 16; i++) r.pushRx((uint8_t)i);
    CHECK_EQ(platform::onHangup(r.rxRoom(), true) == platform::HangupAction::DROP, 1);
    CHECK_EQ(platform::onHangup(r.rxRoom(), false) == platform::HangupAction::READ, 1);
}

} // namespace

int main() {
    testRegisters();
    testReceive();
    testInterrupts();
    testRr2Status();
    testStatusHighCodes();
    testFirstCharMode();
    testTxAndPriority();
    testRts();
    testSharedPointer();
    testBaudAndTx();
    testRxPacing();
    testSpecialLock();
    testExtStatus();
    testState();
    testSpecialAllChars();
    testLinkRemoved();
    testRingDrop();
    testPump();
    testHangupDecision();
    return finish();
}
