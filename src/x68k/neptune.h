// ネットワークボード Neptune-X。X68000 の拡張スロットに ISA バスをつなぐボードに、NE2000 互換のイーサネットの
// コントローラ (National Semiconductor DP8390 と 16KB のバッファメモリ) を載せたもの。
//
// アドレスと割り込み:
//   $ECE000-$ECE3FF  ボードの範囲 (ISA の I/O ポート $200-$3FF を 2 バイトおきに置く: ポート p は $ECE000 + (p - $200) x 2)。
//                    NE2000 はポート $300-$31F にあるので、偶数番地の $ECE200-$ECE23E に見える:
//                    $ECE200 + r x 2 (r = 0..15)  DP8390 のレジスタ
//                    $ECE220                      データポート (リモート DMA。16 ビットで読み書きする)
//                    $ECE23E                      リセットポート (読むと DP8390 をリセットする。書いても何も起きない)
//                    範囲の中のほかの番地は、読むと $FF、書いても何も起きない。範囲の外 ($ECE400-$ECFFFF) はバスエラー
//   割り込み          拡張スロットの IRQ2 (レベル 2)、ベクタ $F9。DP8390 の INT (ISR & IMR) をそのまま出す
// これは NetBSD/x68k の neptune.c と stand/boot/ne.c (ポートの置き方、偶数番地だけを使うこと、データポートを
// 16 ビットで読み書きしてバイトの並びを入れ替えないこと) と GENERIC の設定 (neptune0 at intio0 addr 0xece000 intr 249。
// 249 = ベクタ $F9) に合わせた (コードは使っていない)。範囲の終わり ($ECE3FF)、割り込みのレベル (IRQ2)、PROM の中身は、
// ボードの資料でも実機でも確かめていない。この値で、ether_ne.sys 0.03 と TCPPACKA の ping が通る (docs/compat.md)。
//
// DP8390 は National Semiconductor の DP8390D のデータシート (NIC: Network Interface Controller) に合わせた:
//   CR (全ページの 0)  bit0 STP、bit1 STA、bit2 TXP、bit5-3 RD (1 リモート読み出し、2 リモート書き込み、3 Send Packet、
//                      4 以上 中止)、bit7-6 ページ
//   ページ 0 (読み)   1-2 CLDA、3 BNRY、4 TSR、5 NCR、6 FIFO、7 ISR、8-9 CRDA、C RSR、D-F CNTR0-2 (読むと 0 になる)
//   ページ 0 (書き)   1 PSTART、2 PSTOP、3 BNRY、4 TPSR、5-6 TBCR、7 ISR (1 を書いたビットを消す)、8-9 RSAR、A-B RBCR、
//                      C RCR、D TCR、E DCR、F IMR
//   ページ 1          1-6 PAR (自分の MAC アドレス)、7 CURR、8-F MAR (マルチキャストのハッシュ)
//   ページ 2 (読み)   1 PSTART、2 PSTOP、3 RNPP、4 TPSR、5 LNPP、6-7 アドレスカウンタ、C RCR、D TCR、E DCR、F IMR
// NE2000 のバッファメモリ (リモート DMA のアドレス): $0000-$001F が PROM (16 バイトの PROM の 1 バイトが 2 バイトずつ
// 並んで見える。先頭 6 バイトが MAC アドレス、14 と 15 が 'W' ($57))、$4000-$7FFF が 16KB の RAM。ほかは読むと $FF。
//
// 受信: フィルタ (RCR の AB / AM / PRO と PAR、MAR) を通ったフレームを、60 バイトに足りなければ 0 で埋め、FCS (CRC-32) を
// 付けて、CURR のページから受信リング (PSTART..PSTOP) に書く。各フレームの前に 4 バイトの見出し
// (受信の状態 RSR、次のフレームのページ、バイト数の下位と上位。バイト数はフレームと FCS で、見出しは含まない) を置く。
// 書くと BNRY のページに届くときは書かずに捨て、ISR の OVW を立て、CNTR2 (取り落とした数) を増やす。
// 送信: TXP を立てると、TPSR のページから TBCR バイトを読んで送る。10Mbps の線で送り終えるまでの時間 (プリアンブル 8 バイト、
// フレーム (60 バイトに満たなければ 60)、FCS 4 バイト、フレームの間の 12 バイト) が過ぎてから、ISR の PTX と TSR の PTX を立て、
// CR の TXP を下ろす (データシートの CR の TXP: 送り終えるか中止すると内部で 0 になる)。送っている間に STP を書いても
// 送り終える (データシートの STP: 送っている途中のものは終えてからリセットの状態に入る)。時間は Machine がラスタごとに
// poll() に渡すラスタの長さで進める。送るとすぐに PTX を立てると、TXP を書いた直後に割り込みが入り、TXP を書いてから
// 送信中のフラグを立てるドライバ (ether_ne.sys 0.03) が、送り終えたことを取りこぼして二度と送らなくなる。
// TCR の LB が 0 でなければ (ループバック)、線には出さずに自分で受ける。受けたフレームはバッファメモリに書かず、
// 最後の 8 バイト (FCS を含む) を FIFO のレジスタから読めるだけ (データシートの Loopback Diagnostics)。
//
// 資料から決められず、推測で決めたこと:
// - 範囲の中の奇数番地は、データポートの下位バイト ($ECE221) を除き、読むと $FF で書いても何も起きない
//   (NetBSD は偶数番地しか使わない。奇数番地に次のレジスタを見せる作りも考えられるが、ISA の 8 ビットのポートの読み書きで
//   上位のバイトレーンがどう見えるかは分からない)
// - リモート DMA は CR の RD の向きと残りのバイト数が 0 でないときだけ転送する。WTS = 1 で奇数のバイト数のときは、
//   最後のワードの 2 バイト目を転送しない。CR を書いても今のアドレスと残りは読み直さない
//   (RSAR と RBCR を書いたときにその値になる)
// - データポートの 16 ビットの読み書きは、上位バイト ($ECE220) と下位バイト ($ECE221) の順に 1 バイトずつ届く
//   (Machine は 16 ビットの読み書きを 2 回のバイトに分ける)。WTS = 1 のとき、上位バイトを読むと 2 バイト転送して
//   下位バイトを覚えておき、下位バイトを読むとそれを返す。書くときは上位バイトを覚えておき、下位バイトで 2 バイト書く。
//   DP8390 の AD0-7 を X68000 の上位バイト (偶数番地) につないでいるとして、BOS = 0 なら先のバイトが上位に来る
//   (NetBSD がバイトの並びを入れ替えずに読み書きしていることと合う)。WTS = 0 のときは上位バイトだけで 1 バイトずつ
// - 受信の時間 (線の速さ、FIFO) は数えない。受信は Machine がラスタごとに EtherLink から 1 フレームずつ取り出す
// - 送信の時間はラスタの始まりごとにまとめて進める。TXP を書いたラスタの分は数えない (TXP を書いてから最初の poll() では
//   時間を進めない) ので、送り終えるのは本当の時間より遅く、その差は 2 ラスタ (約 64us / 128us) より小さい
//   (TXP を書いてから次のラスタの始まりまでは数えず、そのあとはラスタ単位に切り上げるため)。
//   実機より早く PTX を出すことはない
// - 送るフレームは TXP を書いたときにバッファメモリからまとめて読んで EtherLink に渡す (実機は送りながら DMA で読むので、
//   送っている間に同じページへ次のフレームを書くドライバでも、前のフレームは書き換える前の中身で出る)。
//   相手の答えは、PTX を出す前に届くことがある
// - Send Packet コマンド (RD = 3) は作っていない (何もしない)
// - リセットの後の値: CR = $21 (STP、RD = 4)、ISR = $80 (RST)、IMR = 0。ほかのレジスタは変えない
// - 2 枚目のボード ($ECE400)、ほかの割り込みベクタの選び方 (ジャンパ) は作っていない
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stdint.h>

#include "x68k/ether_link.h"

namespace x68k {

class StateIO;

class Neptune {
public:
    static constexpr uint32_t BASE = 0xece000;   // ボードの範囲の先頭
    static constexpr uint32_t WINDOW = 0x400;    // ボードの範囲の大きさ
    static constexpr uint32_t NIC_OFFSET = 0x200; // DP8390 のレジスタ (ISA のポート $300)
    static constexpr uint32_t DATA_OFFSET = NIC_OFFSET + 0x10 * 2;  // データポート ($ECE220)
    static constexpr uint32_t RESET_OFFSET = NIC_OFFSET + 0x1f * 2; // リセットポート ($ECE23E)
    static constexpr int INTERRUPT_LEVEL = 2; // IRQ_LEVEL は Circle のマクロと重なる
    static constexpr int VECTOR = 0xf9;
    static constexpr uint32_t RAM_START = 0x4000; // バッファメモリ (リモート DMA のアドレス)
    static constexpr uint32_t RAM_SIZE = 0x4000;
    static constexpr int PROM_SIZE = 16;
    static constexpr uint8_t DEFAULT_MAC[6] = {0x02, 0x68, 0x6b, 0x00, 0x00, 0x01}; // ローカル管理のアドレス

    // ISR と IMR のビット
    enum : uint8_t {
        ISR_PRX = 0x01, ISR_PTX = 0x02, ISR_RXE = 0x04, ISR_TXE = 0x08,
        ISR_OVW = 0x10, ISR_CNT = 0x20, ISR_RDC = 0x40, ISR_RST = 0x80,
    };
    // CR のビット
    enum : uint8_t { CR_STP = 0x01, CR_STA = 0x02, CR_TXP = 0x04 };
    // RCR のビット
    enum : uint8_t { RCR_SEP = 0x01, RCR_AR = 0x02, RCR_AB = 0x04, RCR_AM = 0x08, RCR_PRO = 0x10, RCR_MON = 0x20 };
    // RSR のビット
    enum : uint8_t { RSR_PRX = 0x01, RSR_MPA = 0x10, RSR_PHY = 0x20, RSR_DIS = 0x40 };
    // DCR のビット
    enum : uint8_t { DCR_WTS = 0x01, DCR_BOS = 0x02 };

    Neptune();
    // 電源を入れたときとリセットポートを読んだとき (DP8390 のリセット)。バッファメモリと PAR などは変えない
    void reset();
    // PROM の MAC アドレス (ドライバが読んで PAR に書く)
    void setMac(const uint8_t mac[6]);
    const uint8_t* mac() const { return prom_; }
    // フレームの送受信の口 (nullptr なら送ったフレームは捨て、何も受け取らない)
    void setLink(EtherLink* link) { link_ = link; }
    EtherLink* link() const { return link_; }

    // CPU からの読み書き。off は BASE からの位置。範囲の外なら read は -1、write は false (バスエラー)
    int read(uint32_t off);
    bool write(uint32_t off, uint8_t v);
    // 割り込みの要求
    bool irq() const { return (isr_ & imr_ & 0x7f) != 0; }

    // EtherLink から 1 フレーム取り出して受信する (Machine がラスタごとに呼ぶ)。受信リングに入らないフレームは
    // 実機と同じく捨て、ISR の OVW を立てる。elapsedNs は前に呼んでから過ぎた時間 (ns) で、送っているフレームの残りの
    // 時間を進める
    void poll(uint32_t elapsedNs);
    // 送っているフレームをすぐに送り終える (試験用)
    void finishTransmit();
    bool transmitting() const { return (cr_ & CR_TXP) != 0; }
    // 10Mbps の線で TBCR バイトのフレームを送る時間 (ns)
    static uint32_t transmitNs(uint16_t bytes);
    // 線から 1 フレーム届いた (FCS なし)。フィルタを通ってリングに書いたら true。試験からも呼ぶ
    bool receiveFrame(const uint8_t* frame, int len) { return receive(frame, len, false); }

    // 試験用
    uint8_t memRead(uint16_t addr) const;
    uint8_t isr() const { return isr_; }
    uint8_t curr() const { return curr_; }
    uint8_t bnry() const { return bnry_; }
    uint32_t sent() const { return sent_; }
    uint32_t received() const { return received_; }
    uint32_t dropped() const { return dropped_; }

    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)

private:
    uint8_t regRead(int r);
    void regWrite(int r, uint8_t v);
    void writeCommand(uint8_t v);
    void startTransmit();
    bool receive(const uint8_t* frame, int len, bool loopback);
    bool accept(const uint8_t* dest) const;
    int freePages() const;
    void memWrite(uint16_t addr, uint8_t v);
    uint8_t dmaReadByte();
    void dmaWriteByte(uint8_t v);
    void dmaStep();
    void countMissed();

    // DP8390 のレジスタ
    uint8_t cr_ = 0x21, isr_ = ISR_RST, imr_ = 0;
    uint8_t pstart_ = 0, pstop_ = 0, bnry_ = 0, curr_ = 0, tpsr_ = 0;
    uint16_t tbcr_ = 0, rsar_ = 0, rbcr_ = 0;
    uint16_t dmaAddr_ = 0, dmaCount_ = 0; // リモート DMA の今のアドレスと残りのバイト数
    uint8_t rcr_ = 0, tcr_ = 0, dcr_ = 0;
    uint8_t tsr_ = 0, rsr_ = 0, ncr_ = 0;
    uint8_t cntr_[3] = {};
    uint8_t par_[6] = {}, mar_[8] = {};
    uint8_t rnpp_ = 0, lnpp_ = 0;
    uint8_t dataLatch_ = 0;      // データポートの下位バイト (読んだ値 / 書く値)
    bool overflowRst_ = false;   // 受信リングがあふれて ISR の RST を立てている

    uint8_t prom_[PROM_SIZE] = {};
    uint8_t ram_[RAM_SIZE] = {};

    // 送っているフレーム (CR の TXP が 1 の間) の残りの時間 (ns) と、TXP を書いてからまだ poll() を呼んでいないか
    // (ステートに入れる。版 11 から)
    uint32_t txRemainNs_ = 0;
    bool txFirstPoll_ = false;

    // 保存しないもの
    EtherLink* link_ = nullptr;
    uint8_t rxBuf_[EtherLink::MAX_FRAME] = {}; // EtherLink から取り出したフレーム
    uint8_t fifo_[8] = {};                     // ループバックで受けたフレームの最後の 8 バイト (試験用なので保存しない)
    uint8_t fifoPos_ = 0;
    uint32_t sent_ = 0, received_ = 0, dropped_ = 0;
};

} // namespace x68k
