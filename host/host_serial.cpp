#include "host_serial.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
// <windows.h> の古いマクロ。x68k のヘッダの引数名 (far) とぶつかる
#undef far
#undef near
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#ifdef __APPLE__
#include <netinet/tcp_fsm.h>
#endif
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <thread>

#include "platform/serial_ring.h"
#include "platform/settings.h"

namespace {

// ソケットの OS ごとの違い。Windows は Winsock (ソケットは SOCKET、poll は WSAPoll、エラーは WSAGetLastError())
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void closeSocket(Socket s) { closesocket(s); }
void setNonBlocking(Socket s) {
    u_long one = 1;
    ioctlsocket(s, FIONBIO, &one);
}
int pollSockets(pollfd* p, unsigned n, int ms) { return WSAPoll(p, (ULONG)n, ms); }
int socketError() { return WSAGetLastError(); }
bool wouldBlock(int e) { return e == WSAEWOULDBLOCK || e == WSAEINTR; }
// FormatMessage の文はコンソールのコードページで化けるので、番号だけ出す
const char* socketErrorText(int e) {
    static thread_local char buf[32];
    snprintf(buf, sizeof buf, "WSA error %d", e);
    return buf;
}
// Winsock を使う前に 1 回だけ呼ぶ (WSACleanup はしない。プロセスが終わるまで使う)
bool startSockets(char* err, size_t errSize) {
    static const int r = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d);
    }();
    if (r != 0) snprintf(err, errSize, "WSAStartup: error %d", r);
    return r == 0;
}
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
void closeSocket(Socket s) { close(s); }
void setNonBlocking(Socket s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK); }
int pollSockets(pollfd* p, unsigned n, int ms) { return poll(p, (nfds_t)n, ms); }
int socketError() { return errno; }
bool wouldBlock(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINTR; }
const char* socketErrorText(int e) { return strerror(e); }
bool startSockets(char*, size_t) { return true; }
#endif

#ifdef POLLRDHUP
constexpr short kHangupEvents = POLLRDHUP;
#else
constexpr short kHangupEvents = 0;
#endif

// 相手が閉じたか (FIN を受け取ったか)。読み残しがあっても分かる。Linux は poll の POLLRDHUP で分かるので使わない。
// macOS の poll は、読み残しがあると相手が閉じても POLLHUP を返さないので、TCP の状態を見る
bool peerClosed(Socket fd) {
#if defined(__APPLE__) && defined(TCP_CONNECTION_INFO)
    tcp_connection_info info;
    socklen_t len = sizeof info;
    if (getsockopt(fd, IPPROTO_TCP, TCP_CONNECTION_INFO, &info, &len) < 0) return false;
    return TCPS_HAVERCVDFIN2(info.tcpi_state);
#else
    (void)fd;
    return false;
#endif
}

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL; // Linux: 切れた相手に送っても SIGPIPE を出さない
#else
constexpr int kSendFlags = 0; // macOS: ソケットの SO_NOSIGPIPE で止める (Windows は SIGPIPE がない)
#endif

class TcpSerialLink : public platform::RingSerialLink {
public:
    ~TcpSerialLink() override {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (client_ != kNoSocket) closeSocket(client_);
        if (listen_ != kNoSocket) closeSocket(listen_);
    }

    bool open(const char* addr, unsigned port, char* err, size_t errSize) {
        if (!startSockets(err, errSize)) return false;
        listen_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_ == kNoSocket) {
            snprintf(err, errSize, "socket: %s", socketErrorText(socketError()));
            return false;
        }
        int one = 1;
#ifdef _WIN32
        // Windows の SO_REUSEADDR は、ほかのプロセスが使っている番号にも重ねて bind できてしまうので、逆に独り占めにする
        // (Windows は TIME_WAIT の番号にも、これなしで bind できる)
        setsockopt(listen_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof one);
#else
        setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
        sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)port);
        const char* host = addr[0] ? addr : "127.0.0.1";
        if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
            snprintf(err, errSize, "bad address %s (use an IPv4 address like 127.0.0.1)", host);
            return false;
        }
        if (bind(listen_, (sockaddr*)&sa, sizeof sa) != 0 || listen(listen_, 1) != 0) {
            snprintf(err, errSize, "cannot listen on %s:%u: %s", host, port, socketErrorText(socketError()));
            return false;
        }
        setNonBlocking(listen_);
        // port が 0 なら OS が選んだ番号 (試験で使う)
        socklen_t len = sizeof sa;
        if (getsockname(listen_, (sockaddr*)&sa, &len) == 0) port_ = ntohs(sa.sin_port);
        snprintf(name_, sizeof name_, "%s:%u", host, port_);
        thread_ = std::thread([this] { run(); });
        return true;
    }
    const char* name() const { return name_; }
    unsigned port() const { return port_; }

private:
    void drop() {
        closeSocket(client_);
        client_ = kNoSocket;
        pendingLen_ = pendingPos_ = 0;
        setCarrier(false);
        printf("rs232c: %s: disconnected\n", name_);
        fflush(stdout);
    }

    void acceptClient() {
        sockaddr_in peer;
        socklen_t len = sizeof peer;
        const Socket fd = accept(listen_, (sockaddr*)&peer, &len);
        if (fd == kNoSocket) return;
        if (client_ != kNoSocket) { // すでに相手がいる
            closeSocket(fd);
            return;
        }
        setNonBlocking(fd);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        // つながる前に X68000 が送ったバイトは捨てる (相手のいない線に送ったのと同じ)
        while (popTx() >= 0) {
        }
        // 前の相手から届いて、まだ X68000 が読んでいないバイトも捨てる (次の相手のバイトと混ぜない)。
        // 切れたときではなく今捨てるのは、送ってすぐ閉じる相手 (printf hello | nc -N ...) の最後まで X68000 に渡すため
        dropRx();
        client_ = fd;
        setCarrier(true);
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        printf("rs232c: %s: connected from %s:%u\n", name_, ip, (unsigned)ntohs(peer.sin_port));
        fflush(stdout);
    }

    void run() {
        while (!stop_) {
            pollfd p[2];
            int n = 0;
            p[n++] = {listen_, POLLIN, 0};
            if (client_ != kNoSocket) {
                // 受信のリングが満ちていても、相手が切れたことは知りたい。Linux は POLLRDHUP を頼む (macOS は peerClosed()。
                // Windows の WSAPoll は、読み残しがあっても相手が閉じれば POLLHUP を返す)
                short ev = kHangupEvents;
                if (rxSpace() > 0) ev |= POLLIN;
                if (pendingPos_ < pendingLen_) ev |= POLLOUT;
                p[n++] = {client_, ev, 0};
            }
            // 送るバイトは X68000 のスレッドがリングに入れるので、短い間隔で見る (9600bps で 1 バイトは約 1ms)
            pollSockets(p, (unsigned)n, 2);
            if (p[0].revents & POLLIN) acceptClient();
            if (client_ == kNoSocket) {
                while (popTx() >= 0) { // 相手がいなければ捨てる
                }
                continue;
            }
            // 待ち受けで今つないだ相手は、まだ poll していない
            const short rev = n > 1 ? p[1].revents : 0;
            if (rev & (POLLERR | POLLNVAL)) {
                drop();
                continue;
            }
            // 受け取る: リングの空きの分だけ読む (読まなければ TCP の流れの制御で相手が待つ)。
            // 空きと捨てる依頼が済んでいるかは rxRoom() でそろえて取り、あとの判定はこの値だけで行う
            const auto room = rxRoom();
            const unsigned space = room.space;
            const bool hangup = (rev & (POLLHUP | kHangupEvents)) || (space == 0 && peerClosed(client_));
            switch (platform::onHangup(room, hangup)) {
            case platform::HangupAction::WAIT_DRAIN:
                // 相手が閉じたが、前の相手の読み残しを X68000 の側がまだ捨てていない (つないだ直後)。リングが満ちている
                // わけではないので切らずに待ち、捨て終えたら読む (切ると、この相手が送ったバイトを全部捨ててしまう)。
                // poll がすぐ戻り続けるので、少し待つ
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            case platform::HangupAction::DROP:
                // 相手が閉じたが、X68000 が受信を止めていて (RTS を下ろしたまま) リングに空きがない。待つと DCD / CTS が
                // 1 のまま残り、次の相手も受けられないので、ここで切る。リングに入っている分は X68000 が読むまで残し
                // (次の相手がつながったときに捨てる)、OS の受信バッファに残っている分は捨てる
                drop();
                continue;
            case platform::HangupAction::READ:
                break;
            }
            if (space > 0) {
                uint8_t buf[1024];
                const int want = (int)(space < sizeof buf ? space : sizeof buf);
                const int r = (int)::recv(client_, (char*)buf, want, 0);
                if (r == 0 || (r < 0 && !wouldBlock(socketError()))) {
                    drop();
                    continue;
                }
                for (int i = 0; i < r; i++) pushRx(buf[i]);
            }
            // 送る
            if (pendingPos_ == pendingLen_) {
                pendingPos_ = pendingLen_ = 0;
                int b;
                while (pendingLen_ < (int)sizeof pending_ && (b = popTx()) >= 0) pending_[pendingLen_++] = (uint8_t)b;
            }
            if (pendingPos_ < pendingLen_) {
                const int w = (int)::send(client_, (const char*)pending_ + pendingPos_, pendingLen_ - pendingPos_, kSendFlags);
                if (w > 0) {
                    pendingPos_ += w;
                } else if (w < 0 && !wouldBlock(socketError())) {
                    drop();
                }
            }
        }
    }

    Socket listen_ = kNoSocket;
    Socket client_ = kNoSocket;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    uint8_t pending_[1024];
    int pendingLen_ = 0, pendingPos_ = 0;
    char name_[64] = {0};
    unsigned port_ = 0;
};

} // namespace

x68k::SerialLink* openHostSerialLink(const char* spec, char* err, size_t errSize) {
    if (errSize) err[0] = 0;
    unsigned port;
    char addr[64];
    switch (platform::parseRs232c(spec, port, addr, sizeof addr)) {
    case platform::Rs232cLink::OFF:
        return nullptr;
    case platform::Rs232cLink::BAD:
        snprintf(err, errSize, "bad value '%s' (off, tcp:PORT or tcp:ADDR:PORT)", spec);
        return nullptr;
    case platform::Rs232cLink::TCP:
        break;
    }
    return openHostSerialLinkAt(addr, port, nullptr, err, errSize);
}

x68k::SerialLink* openHostSerialLinkAt(const char* addr, unsigned port, unsigned* boundPort, char* err, size_t errSize) {
    if (errSize) err[0] = 0;
    TcpSerialLink* link = new TcpSerialLink;
    if (!link->open(addr, port, err, errSize)) {
        delete link;
        return nullptr;
    }
    if (boundPort) *boundPort = link->port();
    printf("rs232c: listening on %s (connect with nc or socat)\n", link->name());
    return link;
}
