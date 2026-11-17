// ベアメタル版の Neptune-X のフレームの送受信 (説明は net_bridge.h)
#include "net_bridge.h"

#include <circle/memio.h>
#include <circle/string.h>
#include <circle/timer.h>
#include <circle/util.h>
#if RASPPI == 4
#include <circle/bcm2711.h>
#include <circle/bcm54213.h>
#elif RASPPI >= 5
#include <circle/bcm2712.h>
#include <circle/macb.h>
#else
#include <circle/usb/lan7800.h>
#include <circle/usb/smsc951x.h>
#include <circle/usb/usb.h>
#include <circle/usb/usbdevice.h>
#include <circle/usb/usbhostcontroller.h>
#endif

#include "platform/ether_peer.h"
#include "platform/settings.h"
#include "x68k/neptune.h"

static const char FromNet[] = "neptune";

CNetBridge::~CNetBridge() {
    delete m_pRing;
    if (m_pLink != m_pRing) delete m_pLink;
    delete m_pOwned;
}

bool CNetBridge::Initialize(const char* link, const char* macText, u8 mac[6]) {
    const char* arg = "";
    const platform::NeptuneLink kind = platform::parseNeptuneLink(link, arg);
    memcpy(m_Mac, x68k::Neptune::DEFAULT_MAC, 6);
    m_bUsePiMac = strcmp(macText, "pi") == 0;
    if (macText[0] && !m_bUsePiMac && !platform::parseMac(macText, m_Mac)) {
        m_Logger.Write(FromNet, LogWarning, "neptune_mac=%s: not a unicast MAC address (like 02:68:6b:00:00:01)", macText);
        return false;
    }
    if (kind == platform::NeptuneLink::ECHO) {
        // コア 1 の中だけで答える (ネットワークデバイスを使わない)
        m_pLink = new platform::EchoPeer;
    } else if (kind == platform::NeptuneLink::DEFAULT || kind == platform::NeptuneLink::ETH) {
        m_pRing = new platform::RingEtherLink<32>;
        m_pLink = m_pRing;
#if RASPPI == 4
        CBcm54213Device* dev = new CBcm54213Device;
        if (dev != nullptr && dev->Initialize()) {
            m_pOwned = dev;
            m_Kind = KindGenet;
        } else {
            delete dev;
            m_Logger.Write(FromNet, LogWarning, "cannot initialize the Ethernet controller (GENET)");
        }
#elif RASPPI >= 5
        CMACBDevice* dev = new CMACBDevice;
        if (dev != nullptr && dev->Initialize()) {
            m_pOwned = dev;
            m_Kind = KindMacb;
        } else {
            delete dev;
            m_Logger.Write(FromNet, LogWarning, "cannot initialize the Ethernet controller (MACB)");
        }
#endif
        FindDevice();
    } else {
        m_Logger.Write(FromNet, LogWarning, "neptune_link=%s: use eth (default) or echo on the bare metal build", link);
        return false;
    }
    if (m_pLink == nullptr) return false;
    if (m_bUsePiMac) {
        if (m_pDevice == nullptr) {
            m_Logger.Write(FromNet, LogWarning, "neptune_mac=pi: no network device yet; using the default address");
            m_bUsePiMac = false; // 既定のアドレスを使うので、デバイスが見つかったらプロミスキャスにする
        } else {
            m_pDevice->GetMACAddress()->CopyTo(m_Mac);
        }
    }
    memcpy(mac, m_Mac, 6);
    return true;
}

// ネットワークデバイスを探す (USB のものは後からつながることがある)
void CNetBridge::FindDevice() {
    if (m_pRing == nullptr || m_pDevice != nullptr) return;
    m_pDevice = CNetDevice::GetNetDevice(NetDeviceTypeEthernet);
    if (m_pDevice == nullptr) return;
#if RASPPI <= 3
#ifdef PIX68K_QEMU
    m_Kind = KindCdc;
#else
    // USB の Ethernet は、機種ではなく USB の ID で見分ける (eth0 が別の USB の Ethernet のこともある)。
    // Circle の USB の Ethernet (LAN7800、LAN9514、CDC) はどれも「CUSBFunction, CNetDevice」の順に継承するので、
    // CLAN7800Device へのキャストから CUSBFunction を得る処理は、どのクラスでも同じ位置を指す
    CUSBFunction* fn = (CLAN7800Device*)m_pDevice;
    const TUSBDeviceDescriptor* desc = fn->GetDevice()->GetDeviceDescriptor();
    m_Kind = KindUnknown;
    if (desc != nullptr && desc->idVendor == 0x0424) {
        if (desc->idProduct == 0x7800) m_Kind = KindLan7800;
        else if (desc->idProduct == 0xec00) m_Kind = KindSmsc951x; // LAN9512 / LAN9514
    }
#endif
#endif
    static const char* const kNames[] = {"none", "GENET", "MACB", "LAN7800", "LAN9514", "CDC Ethernet", "unknown"};
    CString mac;
    m_pDevice->GetMACAddress()->Format(&mac);
    m_Logger.Write(FromNet, LogNotice, "network device: %s (%s)", kNames[m_Kind], (const char*)mac);
    m_LastPhy = 0; // すぐにリンクを調べる
}

// USB のネットワークデバイスのレジスタ reg のビット set を立てる (LAN7800 と LAN9514 は同じベンダー要求)
bool CNetBridge::UsbRegister(u32 reg, u32 set) {
#if RASPPI <= 3
    CUSBFunction* fn = nullptr;
    // CNetDevice は private の基底なので、C 形式のキャストで実際のクラスに戻す (m_Kind は FindDevice() が USB の ID で決めた)
    if (m_Kind == KindLan7800) fn = (CLAN7800Device*)m_pDevice;
    else if (m_Kind == KindSmsc951x) fn = (CSMSC951xDevice*)m_pDevice;
    if (fn == nullptr) return false;
    DMA_BUFFER(u32, value, 1);
    if (fn->GetHost()->ControlMessage(fn->GetEndpoint0(), REQUEST_IN | REQUEST_VENDOR, 0xa1, 0, reg, value, 4) != 4)
        return false;
    value[0] |= set;
    return fn->GetHost()->ControlMessage(fn->GetEndpoint0(), REQUEST_OUT | REQUEST_VENDOR, 0xa0, 0, reg, value, 4) >= 0;
#else
    (void)reg;
    (void)set;
    return false;
#endif
}

// 受信のフィルタを変える。X68000 に別の MAC アドレスを使うときはプロミスキャスにする。neptune_mac=pi のときも、
// X68000 のドライバが設定するマルチキャストを通すため、USB のものはマルチキャストをすべて通す設定にする
// (GENET と MACB はマルチキャストだけを通す簡単な設定がないので、プロミスキャスにする。余分なフレームは Neptune-X の
// フィルタが捨てる)。USB のレジスタを書けなかったときは m_bPromiscuous を立てず、Service() が 2 秒ごとに書き直す
void CNetBridge::SetPromiscuous() {
    bool ok = true;
    switch (m_Kind) {
#if RASPPI == 4
    case KindGenet: { // UMAC (GENET の $800) の CMD ($008) の bit4 (PROMISC)
        const uintptr reg = ARM_BCM54213_BASE + 0x800 + 0x008;
        write32(reg, read32(reg) | (1u << 4));
        // Circle は宛先のフィルタ (MDF) にブロードキャストと Pi の MAC アドレスを入れて有効にしている。
        // Linux の bcmgenet もプロミスキャスのときは MDF_CTRL ($650) を 0 にする
        write32(ARM_BCM54213_BASE + 0x800 + 0x650, 0);
        break;
    }
#elif RASPPI >= 5
    case KindMacb: { // NCFGR ($004) の bit4 (CAF)
        const uintptr reg = ARM_MACB_BASE + 0x0004;
        write32(reg, read32(reg) | (1u << 4));
        break;
    }
#endif
    // RFE_CTL: UCAST_EN ($100、宛先を見ずにユニキャストを通す)、MCAST_EN ($200、マルチキャストをすべて通す)
    case KindLan7800: ok = UsbRegister(0x0b0, m_bUsePiMac ? 0x200 : 0x300); break;
    // MAC_CR: PRMS ($40000、プロミスキャス)、MCPAS ($80000、マルチキャストをすべて通す)
    case KindSmsc951x: ok = UsbRegister(0x100, m_bUsePiMac ? 0x80000 : 0x40000); break;
    case KindCdc: break;
    default: ok = false; break;
    }
    if (ok) {
        if (!m_bPromiscuous)
            m_Logger.Write(FromNet, LogNotice,
                           m_bUsePiMac ? "receiving all multicast frames (neptune_mac=pi)"
                                       : "receiving frames for the X68000's own MAC address (promiscuous)");
        m_bPromiscuous = true;
    } else if (!m_bFilterWarned) {
        m_Logger.Write(FromNet, LogWarning,
                       m_bUsePiMac ? "cannot change the receive filter; multicast frames may not arrive (retrying)"
                                   : "cannot disable the MAC address filter (retrying); use neptune_mac=pi");
        m_bFilterWarned = true;
    }
}

void CNetBridge::SendProbe() {
    static const u8 kIp[4] = {10, 0, 2, 15}, kTarget[4] = {10, 0, 2, 2};
    u8 f[60] = {};
    memset(f, 0xff, 6);
    memcpy(f + 6, m_Mac, 6);
    static const u8 kHdr[] = {0x08, 0x06, 0, 1, 0x08, 0x00, 6, 4, 0, 1};
    memcpy(f + 12, kHdr, sizeof kHdr);
    memcpy(f + 22, m_Mac, 6);
    memcpy(f + 28, kIp, 4);
    memcpy(f + 38, kTarget, 4);
    memcpy(m_Buffer, f, sizeof f);
    const bool ok = m_pDevice->SendFrame(m_Buffer, sizeof f);
    m_Logger.Write(FromNet, LogNotice, "probe: ARP request for 10.0.2.2 %s", ok ? "sent" : "not sent");
    m_bProbeSent = true;
}

void CNetBridge::CheckProbe(const u8* f, unsigned len) {
    if (len < 42 || memcmp(f, m_Mac, 6) != 0 || f[12] != 0x08 || f[13] != 0x06 || f[21] != 2) return;
    m_Logger.Write(FromNet, LogNotice, "probe: ARP reply to the X68000 from %u.%u.%u.%u (%02x:%02x:%02x:%02x:%02x:%02x)",
                   f[28], f[29], f[30], f[31], f[22], f[23], f[24], f[25], f[26], f[27]);
    m_bProbeDone = true;
}

void CNetBridge::Service() {
    if (m_pRing == nullptr) return;
    FindDevice();
    if (m_pDevice == nullptr) return;
    const u64 now = CTimer::GetClockTicks64();
    // リンクの状態を 2 秒ごとに調べる (Circle のネットワークの PHY のタスクの代わり)
    if (m_LastPhy == 0 || now - m_LastPhy >= 2 * (u64)CLOCKHZ) {
        m_LastPhy = now;
        m_pDevice->UpdatePHY();
        const bool up = m_pDevice->IsLinkUp();
        if (up != m_bLinkUp) {
            m_Logger.Write(FromNet, LogNotice, "link %s", up ? "up" : "down");
            m_bLinkUp = up;
        }
        // GENET と MACB は UpdatePHY() でフィルタを書き直すことがあるので、毎回書く。USB のものは書けるまで
        if (!m_bPromiscuous || m_Kind == KindGenet || m_Kind == KindMacb) SetPromiscuous();
    }
    if (m_bProbe && !m_bProbeSent && m_bLinkUp) SendProbe();
    // X68000 が送ったフレームを出す
    for (int i = 0; i < 32; i++) {
        if (!m_pDevice->IsSendFrameAdvisable()) break;
        const int n = m_pRing->tx.pop(m_Buffer, FRAME_BUFFER_SIZE);
        if (n <= 0) break;
        if (m_pDevice->SendFrame(m_Buffer, (unsigned)n)) m_nSent++;
    }
    // 受け取ったフレームを X68000 に渡す (リングが満杯なら捨てる)
    for (int i = 0; i < 32; i++) {
        unsigned n = 0;
        if (!m_pDevice->ReceiveFrame(m_Buffer, &n)) break;
        if (m_bProbeSent && !m_bProbeDone) CheckProbe(m_Buffer, n);
        if (n >= (unsigned)x68k::EtherLink::MIN_FRAME && n <= (unsigned)x68k::EtherLink::MAX_FRAME &&
            m_pRing->rx.push(m_Buffer, (int)n))
            m_nReceived++;
    }
    // 10 秒ごとに、数が変わっていればログに出す
    if (now - m_LastStats >= 10 * (u64)CLOCKHZ) {
        m_LastStats = now;
        const unsigned total = m_nSent + m_nReceived + m_pRing->rx.overflows() + m_pRing->tx.overflows();
        if (total != m_nLogged)
            m_Logger.Write(FromNet, LogNotice, "frames: sent %u, received %u, dropped (ring full) rx %u tx %u", m_nSent,
                           m_nReceived, m_pRing->rx.overflows(), m_pRing->tx.overflows());
        m_nLogged = total;
    }
}
