// Windows 版の FUK の口 (host/host_fuk_win.cpp) の単体テスト。host_fuk_test.cpp (pty を使う) の Windows 版。
// 2 本の名前付きパイプの先に偽物の装置 (fuk_fake.h) を置き、x68k::Fuk が見分けて、読めることを確かめる
// (パイプは GetCommState() が失敗するので、口は設定せずに使う)。抜き差しは、パイプの名前を消して作り直してまねる。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "fuk_fake.h"
#include "host_fuk.h"
#include "test_check.h"
#include "x68k/fuk.h"

namespace {

uint64_t realUs(void*) {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void sleepMs(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// 装置の側のパイプを作る (同じ名前で何本でも作れる。口がつなぐと、そのうちの 1 本につながる)
HANDLE makePipe(const char* name) {
    return CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                            PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr);
}

void pipeName(char* out, size_t size, const char* what, int i) {
    snprintf(out, size, "\\\\.\\pipe\\pix68k-fuk-test-%lu-%s%d", (unsigned long)GetCurrentProcessId(), what, i);
}

// 届いているバイトを読む (届いていなければ待たずに 0)
unsigned readAvail(HANDLE h, uint8_t* buf, unsigned size) {
    DWORD avail = 0;
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) return 0;
    DWORD n = 0;
    if (!ReadFile(h, buf, avail < size ? avail : size, &n, nullptr)) return 0;
    return n;
}

bool writeAll(HANDLE h, const uint8_t* p, unsigned n) {
    DWORD w = 0;
    return WriteFile(h, p, n, &w, nullptr) && w == n;
}

void testPipe() {
    char name[2][128];
    HANDLE pipe[2];
    for (int i = 0; i < 2; i++) {
        pipeName(name[i], sizeof name[i], "dev", i);
        pipe[i] = makePipe(name[i]);
        CHECK_EQ(pipe[i] != INVALID_HANDLE_VALUE, 1);
        if (pipe[i] == INVALID_HANDLE_VALUE) return;
    }
    char err[256] = {0};
    x68k::FukLink* link = openHostFukLinkAt(name[0], name[1], err, sizeof err);
    if (link == nullptr) printf("  %s\n", err);
    CHECK_EQ(link != nullptr, 1);
    // 装置の役のスレッド: パイプに届いたバイトを偽物の装置に渡し、装置の返事をパイプに書く
    static FakeFuk dev;
    dev.clock = realUs;
    dev.latencyUs = 200;
    dev.swapPorts = true; // 1 本目の名前のポートがポート 2
    std::atomic<bool> stop{false};
    std::thread device([&] {
        uint8_t buf[4096];
        while (!stop.load()) {
            bool idle = true;
            for (int i = 0; i < 2; i++) {
                unsigned n;
                while ((n = readAvail(pipe[i], buf, sizeof buf)) > 0) {
                    dev.send(i, buf, n);
                    idle = false;
                }
            }
            for (int i = 0; i < 2; i++) {
                unsigned n;
                while ((n = dev.receive(i, buf, sizeof buf)) != 0) {
                    writeAll(pipe[i], buf, n);
                    idle = false;
                }
            }
            if (idle) std::this_thread::yield();
        }
    });
    if (link != nullptr) {
        x68k::Fuk fuk(*link);
        CHECK_EQ(fuk.start(3000), 1);
        auto until = [&](auto done) {
            const uint64_t t0 = realUs(nullptr);
            while (realUs(nullptr) - t0 < 3000000) {
                fuk.poll();
                if (done()) return true;
                link->waitRx(200);
            }
            return false;
        };
        fuk.write(7, 0x80); // モーター
        CHECK_EQ(until([&] { return fuk.read(1) == 0x80; }), 1);
        const uint8_t read[] = {0x46, 0x00, 1, 0, 1, 3, 8, 0x1b, 0xff};
        for (uint8_t b : read) fuk.write(3, b);
        CHECK_EQ(until([&] { return fuk.dataReady(); }), 1);
        static uint8_t got[8192];
        CHECK_EQ(fuk.readData(got, sizeof got), 8192);
        CHECK_EQ(memcmp(got, &dev.image[2 * 8 * 1024], sizeof got), 0);
        CHECK_EQ(fuk.read(1), 0xd0);
        CHECK_EQ(fuk.read(3), 0x00);
        CHECK_EQ(fuk.stats().timeouts, 0);
        CHECK_EQ(link->sendIdle(0) && link->sendIdle(1), 1);
    }
    stop.store(true);
    device.join();
    delete link;
    CloseHandle(pipe[0]);
    CloseHandle(pipe[1]);
    // ないポート
    CHECK_EQ(openHostFukLinkAt("COM250", "COM251", err, sizeof err) == nullptr, 1);
    CHECK_EQ(err[0] != 0, 1);
    CHECK_EQ(openHostFukLink("off", err, sizeof err) == nullptr, 1);
    CHECK_EQ(err[0], 0);
    CHECK_EQ(openHostFukLink("a,b,c", err, sizeof err) == nullptr, 1);
    CHECK_EQ(err[0] != 0, 1);
}

// 送るバイトは送信待ちのバッファにためて送る。大きなまとまりを送っても、順番どおりに全部届く
void testSendBulk() {
    char name[2][128];
    HANDLE pipe[2];
    for (int i = 0; i < 2; i++) {
        pipeName(name[i], sizeof name[i], "bulk", i);
        pipe[i] = makePipe(name[i]);
    }
    char err[256] = {0};
    x68k::FukLink* link = openHostFukLinkAt(name[0], name[1], err, sizeof err);
    CHECK_EQ(link != nullptr, 1);
    if (link != nullptr) {
        static uint8_t data[300000], got[300000];
        for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 31 + (i >> 9));
        unsigned sent = 0, recv = 0;
        const uint64_t t0 = realUs(nullptr);
        while (recv < sizeof data && realUs(nullptr) - t0 < 5000000) {
            if (sent < sizeof data) sent += link->send(1, data + sent, sizeof data - sent);
            recv += readAvail(pipe[1], got + recv, sizeof got - recv);
            std::this_thread::yield();
        }
        CHECK_EQ(recv, (unsigned)sizeof data);
        CHECK_EQ(memcmp(got, data, sizeof data), 0);
        bool idle = false;
        for (int k = 0; k < 100 && !idle; k++) {
            idle = link->sendIdle(1);
            if (!idle) sleepMs(1);
        }
        CHECK_EQ(idle, 1);
        delete link;
    }
    CloseHandle(pipe[0]);
    CloseHandle(pipe[1]);
}

// 届いた時刻は、受け取るスレッドが付ける (#320)。読むのを遅らせても、届いた時刻のまま
void testArrivalTime() {
    char name[2][128];
    HANDLE pipe[2];
    for (int i = 0; i < 2; i++) {
        pipeName(name[i], sizeof name[i], "time", i);
        pipe[i] = makePipe(name[i]);
    }
    char err[256] = {0};
    x68k::FukLink* link = openHostFukLinkAt(name[0], name[1], err, sizeof err);
    CHECK_EQ(link != nullptr, 1);
    if (link != nullptr) {
        const uint8_t msg[] = {'R', 3, 0};
        const uint64_t before = realUs(nullptr);
        CHECK_EQ(writeAll(pipe[1], msg, sizeof msg), 1);
        sleepMs(60);
        const uint64_t readAt = realUs(nullptr);
        uint8_t got[8];
        CHECK_EQ(link->receive(1, got, sizeof got), 3u);
        const uint64_t at = link->receivedAtUs(1);
        // 書いたあとに付いた時刻で、読んだ時刻より十分前 (受け取るスレッドの遅れは 20 ms まで許す)
        CHECK_EQ(at >= before && at + 40000 <= readAt, 1);
        CHECK_EQ(link->receive(1, got, sizeof got), 0u);
        delete link;
    }
    CloseHandle(pipe[0]);
    CloseHandle(pipe[1]);
}

// 読み書きに失敗したら閉じ、ポートの名前が残っている間は開き直さない。名前がいったん消えて戻ったら開き直し、
// epoch() が変わる。名前が残るのは、同じ名前のパイプをもう 1 本作っておいてまねる
void testReopen() {
    char name[2][128];
    HANDLE pipe[2], pipe2[2];
    for (int i = 0; i < 2; i++) {
        pipeName(name[i], sizeof name[i], "reopen", i);
        pipe[i] = makePipe(name[i]);
    }
    char err[256] = {0};
    x68k::FukLink* link = openHostFukLinkAt(name[0], name[1], err, sizeof err);
    CHECK_EQ(link != nullptr, 1);
    if (link == nullptr) {
        CloseHandle(pipe[0]);
        CloseHandle(pipe[1]);
        return;
    }
    CHECK_EQ(link->online(), 1);
    const unsigned e0 = link->epoch();
    // 挿し直したあとの装置 (同じ名前) を作ってから、古い装置の側を閉じる (読み出しが失敗する)。
    // 名前は一度も消えないので、閉じたあとは開き直さないはず
    for (int i = 0; i < 2; i++) pipe2[i] = makePipe(name[i]);
    CloseHandle(pipe[0]);
    CloseHandle(pipe[1]);
    uint8_t buf[64];
    const uint64_t t0 = realUs(nullptr);
    while (link->online() && realUs(nullptr) - t0 < 3000000) {
        link->receive(0, buf, sizeof buf);
        link->receive(1, buf, sizeof buf);
        sleepMs(1);
    }
    CHECK_EQ(link->online(), 0);
    for (int k = 0; k < 15; k++) {
        CHECK_EQ(link->online(), 0);
        sleepMs(100);
    }
    // 1 本の名前を消す: 消えたのを見る (0.1 秒ごとに見る)。戻ったら開き直す
    CloseHandle(pipe2[0]);
    for (int k = 0; k < 5; k++) {
        CHECK_EQ(link->online(), 0);
        sleepMs(100);
    }
    pipe2[0] = makePipe(name[0]);
    bool back = false;
    for (int k = 0; k < 30 && !back; k++) {
        back = link->online();
        if (!back) sleepMs(100);
    }
    CHECK_EQ(back, 1);
    CHECK_EQ(link->epoch() != e0, 1);
    delete link;
    CloseHandle(pipe2[0]);
    CloseHandle(pipe2[1]);
}

} // namespace

int main() {
    testPipe();
    testSendBulk();
    testArrivalTime();
    testReopen();
    return finish();
}
