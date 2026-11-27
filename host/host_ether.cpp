// ホスト版の Neptune-X のフレームの送受信の口。説明は host_ether.h
#include "host_ether.h"

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <thread>

#include "platform/ether_peer.h"
#include "platform/frame_ring.h"
#include "platform/settings.h"

#ifdef PIX68K_HAVE_PCAP
#ifdef _WIN32
#include "npcap_win.h" // Npcap の wpcap.dll を実行時に読む
#else
#include <pcap.h>
#include <poll.h>
#endif
#endif

#ifdef __linux__
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {

#ifdef PIX68K_HAVE_PCAP
// libpcap。受け取るハンドルと送るハンドルを分ける (1 つのハンドルを 2 つのスレッドから使わない)
class PcapLink : public x68k::EtherLink {
public:
    ~PcapLink() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (rx_) pcap_close(rx_);
        if (tx_) pcap_close(tx_);
    }
    bool open(const char* dev, const uint8_t mac[6], char* err, size_t errSize) {
        char ebuf[PCAP_ERRBUF_SIZE] = {};
#ifdef _WIN32
        if (!loadNpcap(err, errSize)) return false;
#endif
        rx_ = pcap_create(dev, ebuf);
        if (!rx_) return fail(err, errSize, ebuf);
        pcap_set_snaplen(rx_, 2048);
        pcap_set_promisc(rx_, 1);
        pcap_set_timeout(rx_, 10);
        pcap_set_immediate_mode(rx_, 1);
        if (pcap_activate(rx_) < 0) return fail(err, errSize, pcap_geterr(rx_));
        if (pcap_datalink(rx_) != DLT_EN10MB) return fail(err, errSize, "not an Ethernet interface");
        // 自分が送ったフレームは受け取らない。できない環境 (Npcap など) では、送り元が自分の MAC のものをフィルタで落とす
        const bool inOnly = pcap_setdirection(rx_, PCAP_D_IN) == 0;
        char filter[192];
        int at = 0;
        if (!inOnly)
            at = snprintf(filter, sizeof filter, "not ether src %02x:%02x:%02x:%02x:%02x:%02x and (", mac[0], mac[1],
                          mac[2], mac[3], mac[4], mac[5]);
        snprintf(filter + at, sizeof filter - at,
                 "ether dst %02x:%02x:%02x:%02x:%02x:%02x or ether broadcast or ether multicast%s", mac[0], mac[1],
                 mac[2], mac[3], mac[4], mac[5], inOnly ? "" : ")");
        bpf_program prog;
        if (pcap_compile(rx_, &prog, filter, 1, PCAP_NETMASK_UNKNOWN) == 0) {
            pcap_setfilter(rx_, &prog);
            pcap_freecode(&prog);
        }
        // 受け取るスレッドは待たずに読み、poll() で 100ms までフレームを待つ (止めるときに pcap_next_ex() で
        // ずっと待たないように)
        if (pcap_setnonblock(rx_, 1, ebuf) < 0) return fail(err, errSize, ebuf);
#ifdef _WIN32
        // Windows は poll() の代わりに、フレームが届くと合図になるイベントを待つ
        event_ = pcap_getevent(rx_);
        if (!event_) return fail(err, errSize, "cannot wait for frames on this interface");
#else
        fd_ = pcap_get_selectable_fd(rx_);
        if (fd_ < 0) return fail(err, errSize, "cannot wait for frames on this interface");
#endif
        tx_ = pcap_open_live(dev, 64, 0, 10, ebuf);
        if (!tx_) return fail(err, errSize, ebuf);
        thread_ = std::thread([this] { run(); });
        return true;
    }
    void send(const uint8_t* f, int len) override { pcap_inject(tx_, f, (size_t)len); }
    int receive(uint8_t* buf, int cap) override { return ring_.pop(buf, cap); }

private:
    static bool fail(char* err, size_t errSize, const char* msg) {
        snprintf(err, errSize, "pcap: %s", msg);
        return false;
    }
    void run() {
        while (!stop_) {
            // 待たずに読み、なかったときだけ poll() で 100ms まで待つ (libpcap の中に残っているものを先に読む)
            pcap_pkthdr* h;
            const u_char* data;
            const int r = pcap_next_ex(rx_, &h, &data);
            if (r < 0) return;
            if (r == 0) {
#ifdef _WIN32
                WaitForSingleObject(event_, 100);
#else
                pollfd p = {fd_, POLLIN, 0};
                poll(&p, 1, 100);
#endif
                continue;
            }
            if (h->caplen == h->len && h->caplen >= (unsigned)MIN_FRAME && h->caplen <= (unsigned)MAX_FRAME)
                ring_.push(data, (int)h->caplen);
        }
    }

    pcap_t* rx_ = nullptr;
    pcap_t* tx_ = nullptr;
#ifdef _WIN32
    HANDLE event_ = nullptr; // pcap_getevent() のもの (pcap_close() が閉じる)
#else
    int fd_ = -1;
#endif
    std::thread thread_;
    std::atomic<bool> stop_{false};
    platform::FrameRing<64> ring_;
};
#endif

#ifdef __linux__
// Linux の tap デバイス
class TapLink : public x68k::EtherLink {
public:
    ~TapLink() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) close(fd_);
    }
    bool open(const char* name, char* err, size_t errSize) {
        fd_ = ::open("/dev/net/tun", O_RDWR);
        if (fd_ < 0) {
            snprintf(err, errSize, "tap: cannot open /dev/net/tun");
            return false;
        }
        ifreq ifr;
        memset(&ifr, 0, sizeof ifr);
        ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
        snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", name);
        if (ioctl(fd_, TUNSETIFF, &ifr) < 0) {
            snprintf(err, errSize, "tap: cannot attach to %s (create it with ip tuntap and allow your user)", name);
            return false;
        }
        thread_ = std::thread([this] { run(); });
        return true;
    }
    void send(const uint8_t* f, int len) override {
        if (write(fd_, f, (size_t)len) < 0) {
            // 送れなければ捨てる
        }
    }
    int receive(uint8_t* buf, int cap) override { return ring_.pop(buf, cap); }

private:
    void run() {
        uint8_t buf[2048];
        while (!stop_) {
            pollfd p = {fd_, POLLIN, 0};
            if (poll(&p, 1, 10) <= 0) continue;
            const ssize_t n = read(fd_, buf, sizeof buf);
            if (n >= MIN_FRAME && n <= MAX_FRAME) ring_.push(buf, (int)n);
        }
    }

    int fd_ = -1;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    platform::FrameRing<64> ring_;
};
#endif

} // namespace

x68k::EtherLink* openHostEtherLink(const char* spec, const uint8_t mac[6], char* err, size_t errSize) {
    const char* arg = "";
    switch (platform::parseNeptuneLink(spec, arg)) {
    case platform::NeptuneLink::DEFAULT:
    case platform::NeptuneLink::ECHO: return new platform::EchoPeer;
    case platform::NeptuneLink::LOOPBACK: return new platform::LoopbackLink;
    case platform::NeptuneLink::PCAP: {
#ifdef PIX68K_HAVE_PCAP
        PcapLink* l = new PcapLink;
        if (l->open(arg, mac, err, errSize)) return l;
        delete l;
#ifdef _WIN32
        // 名前 (\Device\NPF_{...}) は分かりにくいので、Npcap が読めていれば使えるインタフェースを並べる
        char loaded[8];
        if (loadNpcap(loaded, sizeof loaded)) listNpcapDevices(stderr);
#endif
#else
        (void)mac;
        snprintf(err, errSize, "pcap: this build has no libpcap");
#endif
        return nullptr;
    }
    case platform::NeptuneLink::TAP: {
#ifdef __linux__
        TapLink* l = new TapLink;
        if (l->open(arg, err, errSize)) return l;
        delete l;
#else
        snprintf(err, errSize, "tap: only on Linux");
#endif
        return nullptr;
    }
    case platform::NeptuneLink::ETH:
        // ベアメタル版と同じ設定ファイルを使えるよう、ホスト版では echo にする (警告は main() が出す)
        return new platform::EchoPeer;
    default:
        snprintf(err, errSize, "unknown link '%s' (echo, loopback, pcap:IF, tap:NAME)", spec);
        return nullptr;
    }
}
