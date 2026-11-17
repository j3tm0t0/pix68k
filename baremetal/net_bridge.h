// ベアメタル版の Neptune-X のフレームの送受信。コア 0 が Circle のネットワークデバイス (CNetDevice) で生の
// イーサネットのフレームを出し入れし、コア 1 (X68000) とはロックのない 2 つのリング (platform::FrameRing) で受け渡す。
//   有線の Ethernet: Pi 3B+ は LAN7800、Pi 3B は LAN9514 (SMSC951x)、Pi 4 / Pi 400 は GENET (BCM54213)、
//                   Pi 5 は RP1 の MACB。QEMU の raspi3b では -device usb-net (CDC Ethernet)
// X68000 の MAC アドレスは Pi のものと違うので、デバイスを受信のフィルタなし (プロミスキャス) にする。Circle の
// ドライバは自分の MAC アドレスだけを受け取る設定にするので、初期化の後でレジスタを書き換える:
//   GENET  UMAC_CMD の PROMISC。Circle の UpdatePHY() のあとにも書き直す
//   MACB   NCFGR の CAF (すべてのフレームを受け取る)。同上
//   LAN7800  RFE_CTL の UCAST_EN と MCAST_EN (USB のベンダー要求でレジスタを書く)
//   LAN9514  MAC_CR の PRMS (同上)
//   CDC (QEMU)  何もしない (QEMU の usb-net は宛ての MAC アドレスで選ばない)
// neptune_mac=pi なら、Pi のネットワークデバイスの MAC アドレスを X68000 に使い、USB のものはマルチキャストをすべて
// 通す設定だけにする (GENET と MACB はプロミスキャス)。
// Wi-Fi は Circle の WLAN がファームウェアを要し、ほかの MAC アドレスのフレームを通さないので使わない。
// neptune_link=echo なら、ネットワークデバイスを使わず、コア 1 の中で ARP と ping に答える (platform/ether_peer.h)。
#pragma once

#include <circle/logger.h>
#include <circle/netdevice.h>
#include <circle/synchronize.h>
#include <circle/types.h>

#include "platform/frame_ring.h"
#include "x68k/ether_link.h"

class CNetBridge {
public:
    CNetBridge(CLogger& logger) : m_Logger(logger) {}
    ~CNetBridge();

    // コア 1 を動かす前に、コア 0 で呼ぶ。link は neptune_link= の値、macText は neptune_mac= の値。
    // X68000 に使う MAC アドレスを mac に入れる。使えなければ false (ボードはつながない)
    bool Initialize(const char* link, const char* macText, u8 mac[6]);
    // Machine の Neptune に渡す口
    x68k::EtherLink* Link() { return m_pLink; }
    // コア 0 の主ループから呼ぶ (2ms ごと)。デバイスを探し、フレームを受け渡す
    void Service();
    // 試験用 (カーネルの pix68k_net_probe=1): リンクがつながったら、X68000 の MAC アドレスから ARP の問い合わせ
    // (10.0.2.2 は誰か。QEMU の -netdev user のゲートウェイ) を出し、X68000 宛ての答えが届いたらログに出す
    void EnableProbe() { m_bProbe = true; }
    // ネットワークデバイスを使う (neptune_link=eth)。echo なら false
    bool UsesDevice() const { return m_pRing != nullptr; }

private:
    enum TKind { KindNone, KindGenet, KindMacb, KindLan7800, KindSmsc951x, KindCdc, KindUnknown };
    void FindDevice();
    void SendProbe();
    void CheckProbe(const u8* frame, unsigned len);
    void SetPromiscuous();
    bool UsbRegister(u32 reg, u32 set);

    CLogger& m_Logger;
    x68k::EtherLink* m_pLink = nullptr;              // m_pRing か、試験用の口
    platform::RingEtherLink<32>* m_pRing = nullptr; // コア 0 とコア 1 の間のリング (ネットワークデバイスを使うとき)
    CNetDevice* m_pDevice = nullptr;
    CNetDevice* m_pOwned = nullptr; // 自分で作ったデバイス (GENET、MACB)
    TKind m_Kind = KindNone;
    bool m_bUsePiMac = false;
    bool m_bPromiscuous = false; // 受信のフィルタを変えられた
    bool m_bFilterWarned = false;
    bool m_bLinkUp = false;
    bool m_bProbe = false, m_bProbeSent = false, m_bProbeDone = false;
    u8 m_Mac[6] = {};
    u64 m_LastPhy = 0;
    u64 m_LastStats = 0;
    unsigned m_nSent = 0, m_nReceived = 0, m_nLogged = 0;
    DMA_BUFFER(u8, m_Buffer, FRAME_BUFFER_SIZE);
};
