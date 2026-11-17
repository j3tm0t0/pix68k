// ホスト版の RS-232C の TCP (host/host_serial.cpp) のテスト: 送ってすぐ閉じる相手のバイトが最後まで X68000 の側に届くこと、
// 前の相手の読み残しが次の相手のバイトと混ざらないこと
#include "host_serial.h"
#include "test_check.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

int connectTo(unsigned port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (sockaddr*)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

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
    int fd = connectTo(port);
    CHECK_EQ(fd >= 0, 1);
    CHECK_EQ(waitCarrier(l, true), 1);
    CHECK_EQ((int)send(fd, "hello", 5, 0), 5);
    close(fd);
    CHECK_EQ(waitCarrier(l, false), 1);
    CHECK_EQ(readFor(l, 5) == "hello", 1);

    printf("unread bytes of the previous peer are dropped when the next one connects\n");
    fd = connectTo(port);
    CHECK_EQ(waitCarrier(l, true), 1);
    CHECK_EQ((int)send(fd, "old", 3, 0), 3);
    close(fd);
    CHECK_EQ(waitCarrier(l, false), 1);
    // old は読まない (切れる前に受け取ったバイトはリングに入っている。受け取ってから切るので)
    fd = connectTo(port);
    CHECK_EQ(waitCarrier(l, true), 1);
    CHECK_EQ((int)send(fd, "new", 3, 0), 3);
    const std::string got = readFor(l, 3);
    printf("  got '%s'\n", got.c_str());
    CHECK_EQ(got == "new", 1);
    close(fd);
    CHECK_EQ(waitCarrier(l, false), 1);

    // つないだ直後 (前の相手の読み残しを X68000 の側がまだ捨てていない間) に、送ってすぐ閉じる相手。
    // carrier() を呼ばずに送って閉じ、ネットワークの側が閉じたのを見つけてから X68000 の側で読む: 切らずに待って全部届く
    printf("a peer that sends and closes before the previous bytes are dropped\n");
    fd = connectTo(port);
    CHECK_EQ(fd >= 0, 1);
    CHECK_EQ((int)send(fd, "abc", 3, 0), 3);
    close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_EQ(readFor(l, 3) == "abc", 1);
    CHECK_EQ(waitCarrier(l, false), 1);

    printf("unread bytes are dropped again on the next connection (twice in a row)\n");
    for (int round = 0; round < 2; round++) {
        fd = connectTo(port);
        CHECK_EQ(waitCarrier(l, true), 1);
        CHECK_EQ((int)send(fd, "zz", 2, 0), 2);
        close(fd);
        CHECK_EQ(waitCarrier(l, false), 1);
    }
    fd = connectTo(port);
    CHECK_EQ(waitCarrier(l, true), 1);
    CHECK_EQ((int)send(fd, "ok", 2, 0), 2);
    CHECK_EQ(readFor(l, 2) == "ok", 1);
    close(fd);
    CHECK_EQ(waitCarrier(l, false), 1);

    // X68000 が受信を止めている (読まない) 間にリング (4KB) より多く届き、相手が切れた: 切断を見つけて DCD / CTS を下ろし、
    // 次の相手も受ける。リングに入った分は残り、次の相手がつながったら捨てる
    printf("a hangup is noticed while the receive ring is full\n");
    fd = connectTo(port);
    CHECK_EQ(waitCarrier(l, true), 1);
    std::string big(16384, 'x');
    size_t off = 0;
    while (off < big.size()) {
        const ssize_t w = send(fd, big.data() + off, big.size() - off, 0);
        if (w <= 0) break;
        off += (size_t)w;
    }
    CHECK_EQ(off, big.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // リングが満ちるまで待つ (読まない)
    close(fd);
    CHECK_EQ(waitCarrier(l, false), 1);
    CHECK_EQ(readFor(l, 4096).size(), 4096u);
    // もう一度満たしてから次の相手: 読み残しは捨て、次の相手のバイトだけが届く
    fd = connectTo(port);
    CHECK_EQ(waitCarrier(l, true), 1);
    off = 0;
    while (off < big.size()) {
        const ssize_t w = send(fd, big.data() + off, big.size() - off, 0);
        if (w <= 0) break;
        off += (size_t)w;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    close(fd);
    CHECK_EQ(waitCarrier(l, false), 1);
    fd = connectTo(port);
    CHECK_EQ(waitCarrier(l, true), 1);
    CHECK_EQ((int)send(fd, "fresh", 5, 0), 5);
    CHECK_EQ(readFor(l, 5) == "fresh", 1);
    close(fd);
    CHECK_EQ(waitCarrier(l, false), 1);

    delete l;
    return finish();
}
