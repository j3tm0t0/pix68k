// ホスト版の RS-232C の TCP (host/host_serial.cpp) のテスト: 送ってすぐ閉じる相手のバイトが最後まで X68000 の側に届くこと、
// 前の相手の読み残しが次の相手のバイトと混ざらないこと
#include "host_serial.h"
#include "test_check.h"

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
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <chrono>
#include <string>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void closeSocket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
void closeSocket(Socket s) { close(s); }
#endif

Socket connectTo(unsigned port) {
    const Socket fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kNoSocket) return kNoSocket;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (sockaddr*)&a, sizeof a) < 0) {
        closeSocket(fd);
        return kNoSocket;
    }
    return fd;
}

// 送った長さ (切れたら負)
int sendBytes(Socket fd, const char* p, size_t n) { return (int)send(fd, p, (int)n, 0); }

// carrier() が want になるまで待つ (X68000 の側は周辺機器を進めるたびに carrier() を呼ぶ)
bool waitCarrier(x68k::SerialLink* l, bool want) {
    const auto end = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < end) {
        if (l->carrier() == want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// n バイト受け取るか、届かなくなって 2 秒たつまで読む (負荷の高い機械でも落ちないよう長めにする)
std::string readFor(x68k::SerialLink* l, size_t n) {
    std::string got;
    auto last = Clock::now();
    while (got.size() < n && Clock::now() - last < std::chrono::seconds(2)) {
        l->carrier();
        const int b = l->receive();
        if (b < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        got += (char)b;
        last = Clock::now();
    }
    return got;
}

} // namespace

int main() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }
#endif
    // 待ち受けの番号は OS に選ばせる (ほかの試験やプロセスとぶつからない)
    unsigned port = 0;
    char err[128];
    x68k::SerialLink* l = openHostSerialLinkAt("127.0.0.1", 0, &port, err, sizeof err);
    CHECK_EQ(port != 0, 1);
    CHECK_EQ(l != nullptr, 1);
    if (!l) {
        printf("open: %s\n", err);
        return finish();
    }

    printf("bytes sent just before closing reach the X68000 side\n");
    Socket fd = connectTo(port);
    CHECK_EQ(fd != kNoSocket, 1);
    CHECK_EQ(waitCarrier(l, true), 1);
    CHECK_EQ(sendBytes(fd, "hello", 5), 5);
    closeSocket(fd);
    CHECK_EQ(waitCarrier(l, false), 1);
    CHECK_EQ(readFor(l, 5) == "hello", 1);

    printf("unread bytes of the previous peer are dropped when the next one connects\n");
    fd = connectTo(port);
    CHECK_EQ(waitCarrier(l, true), 1);
    CHECK_EQ(sendBytes(fd, "old", 3), 3);
    closeSocket(fd);
    CHECK_EQ(waitCarrier(l, false), 1);
    // old は読まない (切れる前に受け取ったバイトはリングに入っている。受け取ってから切るので)
    fd = connectTo(port);
    CHECK_EQ(waitCarrier(l, true), 1);
    CHECK_EQ(sendBytes(fd, "new", 3), 3);
    const std::string got = readFor(l, 3);
    printf("  got '%s'\n", got.c_str());
    CHECK_EQ(got == "new", 1);
    closeSocket(fd);
    CHECK_EQ(waitCarrier(l, false), 1);

    // つないだ直後 (前の相手の読み残しを X68000 の側がまだ捨てていない間) に、送ってすぐ閉じる相手。
    // carrier() を呼ばずに送って閉じ、ネットワークの側が閉じたのを見つけてから X68000 の側で読む: 切らずに待って全部届く
    printf("a peer that sends and closes before the previous bytes are dropped\n");
    fd = connectTo(port);
    CHECK_EQ(fd != kNoSocket, 1);
    CHECK_EQ(sendBytes(fd, "abc", 3), 3);
    closeSocket(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_EQ(readFor(l, 3) == "abc", 1);
    CHECK_EQ(waitCarrier(l, false), 1);

    printf("unread bytes are dropped again on the next connection (twice in a row)\n");
    for (int round = 0; round < 2; round++) {
        fd = connectTo(port);
        CHECK_EQ(waitCarrier(l, true), 1);
        CHECK_EQ(sendBytes(fd, "zz", 2), 2);
        closeSocket(fd);
        CHECK_EQ(waitCarrier(l, false), 1);
    }
    fd = connectTo(port);
    CHECK_EQ(waitCarrier(l, true), 1);
    CHECK_EQ(sendBytes(fd, "ok", 2), 2);
    CHECK_EQ(readFor(l, 2) == "ok", 1);
    closeSocket(fd);
    CHECK_EQ(waitCarrier(l, false), 1);

    // X68000 が受信を止めている (読まない) 間にリング (4KB) より多く届き、相手が切れた: 切断を見つけて DCD / CTS を下ろし、
    // 次の相手も受ける。リングに入った分は残り、次の相手がつながったら捨てる
    printf("a hangup is noticed while the receive ring is full\n");
    fd = connectTo(port);
    CHECK_EQ(waitCarrier(l, true), 1);
    std::string big(16384, 'x');
    size_t off = 0;
    while (off < big.size()) {
        const int w = sendBytes(fd, big.data() + off, big.size() - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    CHECK_EQ(off, big.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // リングが満ちるまで待つ (読まない)
    closeSocket(fd);
    CHECK_EQ(waitCarrier(l, false), 1);
    CHECK_EQ(readFor(l, 4096).size(), 4096u);
    // もう一度満たしてから次の相手: 読み残しは捨て、次の相手のバイトだけが届く
    fd = connectTo(port);
    CHECK_EQ(waitCarrier(l, true), 1);
    off = 0;
    while (off < big.size()) {
        const int w = sendBytes(fd, big.data() + off, big.size() - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    closeSocket(fd);
    CHECK_EQ(waitCarrier(l, false), 1);
    fd = connectTo(port);
    CHECK_EQ(waitCarrier(l, true), 1);
    CHECK_EQ(sendBytes(fd, "fresh", 5), 5);
    CHECK_EQ(readFor(l, 5) == "fresh", 1);
    closeSocket(fd);
    CHECK_EQ(waitCarrier(l, false), 1);

    delete l;
    return finish();
}
