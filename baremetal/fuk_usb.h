// ベアメタル版の FUK (x68k/fuk.h。USB でつなぐ実物の FDC と FDD) の USB の口 (#295)。Pi 4 / Pi 5 (xHCI) だけ。
//
// FUK は USB の CDC を 2 本持つ装置 (VID:PID 04D8:E6B1)。Circle はそれぞれを USB シリアル (utty1、utty2) として認識する。
// X68000 を動かすコア 1 と、USB を扱うコア 0 の間は、向きごとのリング (platform/fuk_ring.h) で受け渡す:
//   受信: 受信の転送を出したままにしておき、届いたらコア 0 の USB の割り込みの中でリングに入れる (主ループは通らない)。
//         コア 1 は、ラスタごとと FDC のレジスタを読み書きしたときにリングを見る
//   送信: コア 1 がリングに入れて、コア 0 にコア間の割り込み (IPI) を送る。コア 0 は IPI のハンドラの中で送信の転送を出し、
//         送り終わりの割り込みで残りを続けて送る
// コア 0 の主ループは 1 周に 2ms 以上かかる (待ち、ネットワーク、SD カード) ので、送受信には使わない。
// Circle のドライバには、待たずに送る関数と受信の転送を出したままにする関数を足している (tools/circle_fuk_patch.sh)。
// Circle の USB の関数を呼ぶのはコア 0 だけ (割り込みの中か、割り込みを止めて)。
//
// Pi 3 (DWHCI) は、同じ装置に受信と送信の転送を同時に出せないので、この方法が使えない (fuk=on は無視する)。
#pragma once

#include <circle/types.h>

#include "platform/fuk_ring.h"
#include "x68k/fuk.h"

class CUSBSerialHostDevice;
class CUSBDevice;

class CFukUsb final : public x68k::FukLink {
public:
    static constexpr u16 VENDOR = 0x04d8, PRODUCT = 0xe6b1;
    static constexpr unsigned IPI = 10; // Circle の IPI_USER (CCoreRunner::IPIHandler() が OnIpi() を呼ぶ)

    CFukUsb();
    ~CFukUsb() override;

    // コア 0 から: USB の装置の一覧に FUK (VID と PID が合う USB シリアルが 2 本) があるか。リング (500KB ほど) を持つ
    // このクラスを確保する前に確かめるのに使う
    static bool Present();
    // コア 0 の主ループから: つないでいなければ、USB の装置の一覧から FUK を探してつなぐ。つながっていれば true。
    // つながっている間は、送り残しがあれば送る (IPI を取りこぼしたときの備え)
    bool Scan();
    // コア 0 の IPI のハンドラから
    static void OnIpi();
    // 受信のリングがあふれて捨てたバイト数 (0 でなければならない)
    unsigned RxDropped() const { return __atomic_load_n(&m_RxDropped, __ATOMIC_RELAXED); }
    // 調べるための数 (コア 0 が書く。ほかのコアから読むと少しずれうる)
    struct Counters {
        unsigned txBytes[2] = {0, 0}; // USB に出したバイト数 (ポートは USB のインタフェースの順)
        unsigned rxBytes[2] = {0, 0}; // USB から受け取ったバイト数
        unsigned ipis = 0;            // 処理した IPI
        unsigned sendFailures = 0;    // 送信の転送を出せなかった / 失敗して終わった
        unsigned receiveStops = 0;    // 受信の転送が失敗して止まった
        unsigned removals = 0;        // 装置が消えた
    };
    const Counters& GetCounters() const { return m_Counters; }

    // ---- x68k::FukLink (コア 1 から。コア 1 を動かす前と止めている間は、コア 0 から呼んでよい) ----
    bool online() override { return __atomic_load_n(&m_Online, __ATOMIC_ACQUIRE); }
    unsigned epoch() override { return __atomic_load_n(&m_Epoch, __ATOMIC_ACQUIRE); }
    unsigned send(int port, const uint8_t* data, unsigned n) override;
    unsigned receive(int port, uint8_t* dst, unsigned max) override;
    uint64_t receivedAtUs(int port) override { return m_Port[port].lastRxAt; }
    unsigned droppedStamps() override { return m_Port[0].rxAt.dropped() + m_Port[1].rxAt.dropped(); }
    uint64_t nowUs() override;
    void waitRx(unsigned us) override;
    bool sendIdle(int port) override;

private:
    struct Port {
        CFukUsb* self = nullptr;
        CUSBSerialHostDevice* dev = nullptr; // コア 0 だけが読み書きする
        bool sending = false;                // 書くのはコア 0 だけ。コア 1 も読む (atomic)
        platform::FukRing<131072> rx;        // 書くのはコア 0 (USB の割り込み)、読むのはコア 1
        platform::FukStampRing<4096> rxAt;   // rx に書いたバイトが届いた時刻 (同じく)
        unsigned rxWritten = 0;              // rx に書いたバイト数の合計 (コア 0)
        unsigned rxRead = 0;                 // rx から読んだバイト数の合計 (コア 1)
        uint64_t lastRxAt = 0;               // 最後に読んだバイトが届いた時刻 (コア 1)
        platform::FukRing<131072> tx;        // 書くのはコア 1、読むのはコア 0
        u8 chunk[512];
        unsigned chunkLen = 0;               // chunk の中の、送っている (か、送れずに残っている) バイト数。書くのはコア 0
    };

    static int Find(CUSBSerialHostDevice* found[2]); // 見つけた数 (2 で FUK)
    void Kick();
    void PumpAll();          // コア 0、割り込みを止めて
    void Pump(Port& port);
    void Offline() { __atomic_store_n(&m_Online, false, __ATOMIC_RELEASE); }
    static void ReceiveHandler(const u8* data, unsigned n, void* param);
    static void SentHandler(boolean ok, void* param);
    static void RemovedHandler(void* param);
    static void DeviceRemoving(CUSBDevice* device); // xHCI の装置の破棄が始まる前に呼ばれる (circle_fuk_patch.sh)
    void Detach(Port& port);                        // コア 0、割り込みを止めて

    static CFukUsb* s_pThis;
    Port m_Port[2];
    bool m_Online = false;
    unsigned m_Epoch = 0;
    unsigned m_RxDropped = 0;
    Counters m_Counters;
};
