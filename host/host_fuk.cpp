#include "host_fuk.h"

// termios.h の ECHO などのマクロが settings.h の名前とぶつかるので、先に読む
#include "platform/settings.h"

// Windows 版は host_fuk_win.cpp (COM ポート)
#ifndef _WIN32

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "platform/fuk_ring.h"

namespace {

uint64_t monotonicUs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

// 開いて、そのままのバイトをやり取りする設定にする。開けなければ -1 (errno に理由)。設定を読めない (tcgetattr() が
// 失敗する) ときは、設定せずに開いたまま返す
int openPort(const char* path) {
    const int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    termios t;
    if (tcgetattr(fd, &t) == 0) { // 設定を読めなければ、そのまま使う
        cfmakeraw(&t);
        t.c_cflag |= CLOCAL | CREAD;
        cfsetspeed(&t, B115200);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        if (tcsetattr(fd, TCSANOW, &t) != 0) {
            const int e = errno;
            close(fd);
            errno = e;
            return -1;
        }
        tcflush(fd, TCIOFLUSH);
    }
    return fd;
}

class HostFukLink final : public x68k::FukLink {
public:
    HostFukLink(const char* a, const char* b) : path_{a, b} { trace_ = getenv("PIX68K_FUK_TRACE") != nullptr; }
    ~HostFukLink() override { closePorts(); }

    bool openPorts(char* err, size_t errSize) {
        for (int i = 0; i < 2; i++) {
            fd_[i] = openPort(path_[i].c_str());
            if (fd_[i] < 0) {
                if (err != nullptr) snprintf(err, errSize, "cannot open %s: %s", path_[i].c_str(), strerror(errno));
                closePorts();
                return false;
            }
        }
        epoch_++;
        // 受け取るスレッドを動かす。届いた時刻を付けて、リングに入れる (#320。X68000 を動かすスレッドは、1 フレームを
        // まとめて動かしては待つので、そちらで読んだ時刻では、届いた時刻が分からない)
        for (int i = 0; i < 2; i++) {
            rx_[i].drop();
            rxRead_[i] = rxWritten_[i];
            lastRxAt_[i] = 0;
        }
        failed_.store(false);
        stop_.store(false);
        reader_ = std::thread([this] { readLoop(); });
        return true;
    }

    bool online() override {
        if (fd_[0] >= 0) return true;
        // 抜けている。間を空けて開き直す
        const uint64_t t = monotonicUs();
        // 消えるのを待つ間は、短い抜き差しを見落とさないよう 0.1 秒ごとに見る
        if (t - lastTry_ < (needUnplug_ ? 100000u : 1000000u)) return false;
        lastTry_ = t;
        if (needUnplug_) {
            // 読み書きに失敗したあとは、ポートがいったん消える (2 本のどちらかの名前が access() で見えなくなる。抜いたとき) まで開き直さない。開き直すたびに epoch() を
            // 変えるので、同じ装置にそのまま開き直すと、送りかけの 'M' のデータの残りを捨てて見分けの 0x00 を送り、
            // それがディスクに書かれることがある。消えたのを待つと、この場合は防げる。ただし、装置の電源が保たれた
            // まま USB だけが切れて戻った (ポートが消えて戻る) ときは、装置が初めの状態に戻ったかを見分けられない
            // (FukLink::epoch() と docs/fuk.md の「既知の制限」)。止まっている間 (設定メニューなど) に消えて戻ると、
            // 消えたのを見逃して、もう一度抜くまで開かない
            if (access(path_[0].c_str(), F_OK) == 0 && access(path_[1].c_str(), F_OK) == 0) return false;
            needUnplug_ = false;
            fprintf(stderr, "fuk: the serial ports are gone; will open them when they come back\n");
            return false;
        }
        return openPorts(nullptr, 0);
    }
    unsigned epoch() override { return epoch_; }
    uint64_t nowUs() override { return monotonicUs(); }

    unsigned send(int port, const uint8_t* data, unsigned n) override {
        if (fd_[port] < 0) return 0;
        const ssize_t k = write(fd_[port], data, n);
        if (k > 0 && trace_) trace('>', port, data, (unsigned)k);
        if (k >= 0) return (unsigned)k;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        lost(strerror(errno));
        return 0;
    }

    unsigned receive(int port, uint8_t* dst, unsigned max) override {
        if (fd_[port] < 0) return 0;
        // 同じ時刻に届いたまとまりごとに返す (receivedAtUs() がそのまとまりの時刻になるように)
        // 先に入っているバイト数を見てから区切りを引く (そのあとに届いたバイトを、違う時刻のまま一緒に読まない)
        const unsigned used = rx_[port].used();
        const unsigned n = rx_[port].read(dst, rxAt_[port].limit(rxRead_[port], used < max ? used : max));
        if (n != 0) {
            rxRead_[port] += n;
            lastRxAt_[port] = rxAt_[port].lookup(rxRead_[port], monotonicUs());
            return n;
        }
        if (failed_.load(std::memory_order_acquire) && rx_[port].used() == 0) {
            lost(failErrno_ != 0 ? strerror(failErrno_) : "hang up");
        }
        return 0;
    }
    uint64_t receivedAtUs(int port) override { return lastRxAt_[port]; }
    unsigned droppedStamps() override { return rxAt_[0].dropped() + rxAt_[1].dropped(); }

    bool sendIdle(int port) override {
        if (fd_[port] < 0) return true;
        int queued = 0;
        // OS の送信のバッファに残っているバイト数。調べられなければ、残っていないことにする
        if (ioctl(fd_[port], TIOCOUTQ, &queued) != 0) return true;
        return queued == 0;
    }

    void waitRx(unsigned us) override {
        if (fd_[0] < 0) {
            usleep(us);
            return;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::microseconds(us),
                     [this] { return rx_[0].used() != 0 || rx_[1].used() != 0 || failed_.load(std::memory_order_acquire); });
    }

private:
    // PIX68K_FUK_TRACE を設定すると、送受信したバイトを時刻 (us) 付きで stderr に出す (調べるとき用。A / B は名前を書いた順)
    void trace(char dir, int port, const uint8_t* p, unsigned n) {
        fprintf(stderr, "fuk %10llu %c%c %4u:", (unsigned long long)(monotonicUs() % 10000000000ull), "AB"[port], dir, n);
        for (unsigned i = 0; i < n && i < 24; i++) fprintf(stderr, " %02x", p[i]);
        fprintf(stderr, n > 24 ? " ...\n" : "\n");
    }
    void lost(const char* why) {
        fprintf(stderr, "fuk: the serial ports were closed (%s); will reopen them after the device is unplugged and plugged in again\n",
                why);
        closePorts();
        needUnplug_ = true;
        lastTry_ = monotonicUs();
    }
    // 受け取るスレッド。両方のポートを見て、届いたバイトを時刻 (読めるようになった直後の時刻) と一緒にリングに入れる。
    // 読み書きに失敗したら failed_ を立てて終わる (閉じるのは receive() が lost() で)
    void readLoop() {
        uint8_t buf[4096];
        while (!stop_.load(std::memory_order_acquire)) {
            pollfd p[2] = {{fd_[0], POLLIN, 0}, {fd_[1], POLLIN, 0}};
            bool full = false;
            for (int i = 0; i < 2; i++)
                if (rx_[i].used() + sizeof buf > RX_BYTES) { // 入りきらない分は読まずに待つ
                    p[i].events = 0;
                    full = true;
                }
            if (poll(p, 2, full ? 1 : 10) < 0 && errno != EINTR) {
                fail(errno);
                return;
            }
            const uint64_t at = monotonicUs();
            bool got = false;
            for (int i = 0; i < 2; i++) {
                if (p[i].revents & POLLNVAL) {
                    fail(EBADF);
                    return;
                }
                if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                const ssize_t k = read(fd_[i], buf, sizeof buf);
                if (k > 0) {
                    if (trace_) trace('<', i, buf, (unsigned)k);
                    rxAt_[i].push(rxWritten_[i] + (unsigned)k, at); // 時刻はバイトより先に入れる
                    rxWritten_[i] += rx_[i].write(buf, (unsigned)k);
                    got = true;
                } else if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                    continue;
                } else if (k < 0 || (p[i].revents & (POLLHUP | POLLERR))) {
                    // 待たない設定の端末は、届いていないときも 0 を返すので、0 は抜けたしるしがあるときだけ失敗とみなす
                    fail(k < 0 ? errno : EIO);
                    return;
                }
            }
            if (got) {
                std::lock_guard<std::mutex> lock(mutex_);
                cv_.notify_all();
            }
        }
    }
    void fail(int e) {
        failErrno_ = e;
        failed_.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex_);
        cv_.notify_all();
    }
    void closePorts() {
        if (reader_.joinable()) {
            stop_.store(true, std::memory_order_release);
            reader_.join();
        }
        for (int i = 0; i < 2; i++) {
            if (fd_[i] < 0) continue;
            // DTR と RTS を下ろしてから閉じる
            int lines = TIOCM_DTR | TIOCM_RTS;
            ioctl(fd_[i], TIOCMBIC, &lines);
            close(fd_[i]);
            fd_[i] = -1;
        }
    }

    std::string path_[2];
    int fd_[2] = {-1, -1};
    unsigned epoch_ = 0;
    uint64_t lastTry_ = 0;
    bool needUnplug_ = false; // 読み書きに失敗した。ポートが消えるまで開き直さない
    bool trace_ = false;

    // 受け取るスレッドとの受け渡し
    static constexpr unsigned RX_BYTES = 131072;
    std::thread reader_;
    std::atomic<bool> stop_{false}, failed_{false};
    int failErrno_ = 0; // failed_ を立てる前に書く
    platform::FukRing<RX_BYTES> rx_[2];
    platform::FukStampRing<4096> rxAt_[2];
    unsigned rxWritten_[2] = {0, 0}; // 受け取るスレッドが書く (動いていない間は、こちらも読み書きする)
    unsigned rxRead_[2] = {0, 0};
    uint64_t lastRxAt_[2] = {0, 0};
    std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace

x68k::FukLink* openHostFukLinkAt(const char* portA, const char* portB, char* err, size_t errSize) {
    HostFukLink* link = new HostFukLink(portA, portB);
    if (!link->openPorts(err, errSize)) {
        delete link;
        return nullptr;
    }
    return link;
}

x68k::FukLink* openHostFukLink(const char* spec, char* err, size_t errSize) {
    if (errSize) err[0] = 0;
    char a[platform::Settings::PATH_SIZE], b[platform::Settings::PATH_SIZE];
    switch (platform::parseFuk(spec, a, b, sizeof a)) {
    case platform::FukPorts::OFF: return nullptr;
    case platform::FukPorts::BAD:
        snprintf(err, errSize, "bad value '%s' (off, on or PORT_A,PORT_B)", spec);
        return nullptr;
    case platform::FukPorts::AUTO:
#if defined(__linux__)
        snprintf(a, sizeof a, "/dev/serial/by-id/usb-TNB_Factory._FDC_emulator-if00");
        snprintf(b, sizeof b, "/dev/serial/by-id/usb-TNB_Factory._FDC_emulator-if02");
        break;
#else
        snprintf(err, errSize, "'on' can find the ports only on Linux; write the two serial ports as PORT_A,PORT_B");
        return nullptr;
#endif
    case platform::FukPorts::PATHS: break;
    }
    return openHostFukLinkAt(a, b, err, errSize);
}

#endif // _WIN32
