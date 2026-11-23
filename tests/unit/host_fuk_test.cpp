// ホスト版の FUK のシリアルポートの口 (host/host_fuk.h) の単体テスト。
// 2 本の pty の先に偽物の装置 (fuk_fake.h) を置き、x68k::Fuk が見分けて、読めることを確かめる。
// リング (platform/fuk_ring.h) もここで確かめる。
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <thread>

#include "fuk_fake.h"
#include "host_fuk.h"
#include "platform/fuk_ring.h"
#include "test_check.h"
#include "x68k/fuk.h"

namespace {

uint64_t realUs(void*) {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

int openMaster(char* slave, size_t size) {
    const int fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (fd < 0 || grantpt(fd) != 0 || unlockpt(fd) != 0) return -1;
    const char* name = ptsname(fd);
    if (name == nullptr) return -1;
    snprintf(slave, size, "%s", name);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

void testRing() {
    static platform::FukRing<16> ring;
    uint8_t in[32], out[32];
    for (int i = 0; i < 32; i++) in[i] = (uint8_t)(i + 1);
    CHECK_EQ(ring.write(in, 10), 10);
    CHECK_EQ(ring.used(), 10);
    CHECK_EQ(ring.read(out, 4), 4);
    CHECK_EQ(out[3], 4);
    // 終わりをまたいで書く。入らない分は書かない
    CHECK_EQ(ring.write(in + 10, 20), 10);
    CHECK_EQ(ring.used(), 16);
    CHECK_EQ(ring.read(out, 32), 16);
    for (int i = 0; i < 16; i++) CHECK_EQ(out[i], i + 5);
    CHECK_EQ(ring.read(out, 32), 0);
    ring.write(in, 5);
    ring.drop();
    CHECK_EQ(ring.used(), 0);
    // 届いた時刻の区切り: 0-9 が 100、10-14 が 200、15-29 が 300
    static platform::FukStampRing<4> at;
    at.push(10, 100);
    at.push(15, 200);
    at.push(30, 300);
    CHECK_EQ(at.lookup(4, 0), 100u);  // 4 バイト読んだ: 最後のバイト (3) は 100
    CHECK_EQ(at.lookup(10, 0), 100u); // 9 も 100
    CHECK_EQ(at.lookup(11, 0), 200u);
    CHECK_EQ(at.lookup(30, 0), 300u);
    CHECK_EQ(at.lookup(31, 7), 7u);   // 区切りがない
    at.push(40, 400);
    at.push(50, 500);
    at.push(60, 600);
    at.push(70, 700);
    at.push(80, 800); // 一杯 (入らない)
    CHECK_EQ(at.lookup(75, 0), 0u);
    CHECK_EQ(at.dropped(), 1u);

    // limit(): 同じ時刻のまとまりごとに読む。リングのバイト (data) と区切り (st) を、書き手と同じ順 (区切りが先) で入れる
    static platform::FukRing<64> data;
    static platform::FukStampRing<8> st;
    unsigned written = 0, readN = 0;
    auto put = [&](unsigned n, uint64_t us) {
        st.push(written + n, us);
        written += data.write(in, n);
    };
    auto get = [&](unsigned max, uint64_t* us) {
        const unsigned used = data.used(); // 先にバイト数を見る
        const unsigned n = data.read(out, st.limit(readN, used < max ? used : max));
        readN += n;
        *us = st.lookup(readN, 0);
        return n;
    };
    uint64_t us = 0;
    put(3, 10);
    put(5, 20);
    CHECK_EQ(get(32, &us), 3u);
    CHECK_EQ(us, 10u);
    CHECK_EQ(get(2, &us), 2u); // max で切れても同じまとまり
    CHECK_EQ(us, 20u);
    CHECK_EQ(get(32, &us), 3u);
    CHECK_EQ(us, 20u);
    CHECK_EQ(get(32, &us), 0u);
    // 区切りを入れてからバイトを書くまでの間に読んだ: 区切りの end はまだ書かれていないバイトを指すが、used() で見た
    // バイトしか読まない
    st.push(written + 4, 30);
    CHECK_EQ(get(32, &us), 0u);
    written += data.write(in, 4);
    // used() を見たあとに次のまとまりが届いても、まとめて読まない。読み手の手順 (used()、limit()、read()) の間に
    // 書き手が割り込んだ順をそのまま並べる
    {
        const unsigned used = data.used(); // 4 バイト (時刻 30)
        put(6, 40);                        // used() を見たあとに届いた
        const unsigned n = data.read(out, st.limit(readN, used < 32u ? used : 32u));
        readN += n;
        CHECK_EQ(n, 4u);
        CHECK_EQ(st.lookup(readN, 0), 30u);
    }
    CHECK_EQ(get(32, &us), 6u);
    CHECK_EQ(us, 40u);
    // 何も入っていなければ 0 バイト
    CHECK_EQ(st.limit(readN, data.used()), 0u);
}

void testPty() {
    char slave[2][256];
    int master[2];
    for (int i = 0; i < 2; i++) {
        master[i] = openMaster(slave[i], sizeof slave[i]);
        if (master[i] < 0) {
            printf("  no pty; skipped\n");
            return;
        }
    }
    // 装置の役のスレッド: pty に届いたバイトを偽物の装置に渡し、装置の返事を pty に書く
    static FakeFuk dev;
    dev.clock = realUs;
    dev.latencyUs = 200;
    dev.swapPorts = true; // 1 本目の名前のポートがポート 2
    std::atomic<bool> stop{false};
    std::thread device([&] {
        uint8_t buf[4096];
        while (!stop.load()) {
            pollfd p[2] = {{master[0], POLLIN, 0}, {master[1], POLLIN, 0}};
            poll(p, 2, 1);
            for (int i = 0; i < 2; i++) {
                ssize_t n;
                while ((n = read(master[i], buf, sizeof buf)) > 0) dev.send(i, buf, (unsigned)n);
            }
            for (int i = 0; i < 2; i++) {
                unsigned n;
                while ((n = dev.receive(i, buf, sizeof buf)) != 0) {
                    unsigned done = 0;
                    while (done < n && !stop.load()) {
                        const ssize_t w = write(master[i], buf + done, n - done);
                        if (w > 0) done += (unsigned)w;
                        else usleep(100);
                    }
                }
            }
        }
    });
    char err[256] = {0};
    x68k::FukLink* link = openHostFukLinkAt(slave[0], slave[1], err, sizeof err);
    CHECK_EQ(link != nullptr, 1);
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
    }
    stop.store(true);
    device.join();
    delete link;
    close(master[0]);
    close(master[1]);
    // ないポート
    CHECK_EQ(openHostFukLinkAt("/nonexistent/a", "/nonexistent/b", err, sizeof err) == nullptr, 1);
    CHECK_EQ(err[0] != 0, 1);
    CHECK_EQ(openHostFukLink("off", err, sizeof err) == nullptr, 1);
    CHECK_EQ(err[0], 0);
    CHECK_EQ(openHostFukLink("a,b,c", err, sizeof err) == nullptr, 1);
    CHECK_EQ(err[0] != 0, 1);
}

// 届いた時刻は、受け取るスレッドが付ける (#320)。読むのを遅らせても、届いた時刻のまま
void testArrivalTime() {
    char slave[2][256];
    int master[2];
    for (int i = 0; i < 2; i++) {
        master[i] = openMaster(slave[i], sizeof slave[i]);
        if (master[i] < 0) {
            printf("  no pty; skipped\n");
            return;
        }
    }
    char err[256] = {0};
    x68k::FukLink* link = openHostFukLinkAt(slave[0], slave[1], err, sizeof err);
    CHECK_EQ(link != nullptr, 1);
    if (link != nullptr) {
        const uint8_t msg[] = {'R', 3, 0};
        const uint64_t before = realUs(nullptr);
        CHECK_EQ(write(master[1], msg, sizeof msg), (ssize_t)sizeof msg);
        usleep(60000);
        const uint64_t readAt = realUs(nullptr);
        uint8_t got[8];
        CHECK_EQ(link->receive(1, got, sizeof got), 3u);
        const uint64_t at = link->receivedAtUs(1);
        // 書いたあとに付いた時刻で、読んだ時刻より十分前 (受け取るスレッドの遅れは 20 ms まで許す)
        CHECK_EQ(at >= before && at + 40000 <= readAt, 1);
        CHECK_EQ(link->receive(1, got, sizeof got), 0u);
        delete link;
    }
    close(master[0]);
    close(master[1]);
}

} // namespace

// 読み書きに失敗したら閉じ、ポートの名前が残っている間は開き直さない。名前がいったん消えて戻ったら開き直し、
// epoch() が変わる。ポートの名前は pty を指すシンボリックリンクにして、抜き差しをリンクの削除と作り直しでまねる
void testReopen() {
    char slave[2][256], slave2[2][256], path[2][600], tmp[600];
    int master[2] = {-1, -1}, master2[2] = {-1, -1};
    // 一時ディレクトリは TMPDIR (なければ /tmp) の下に作る
    const char* tmpRoot = getenv("TMPDIR");
    char dir[512];
    snprintf(dir, sizeof dir, "%s/pix68k-fuk-test-XXXXXX", tmpRoot != nullptr && tmpRoot[0] != 0 ? tmpRoot : "/tmp");
    bool haveDir = false;
    x68k::FukLink* link = nullptr;
    auto cleanup = [&] {
        delete link;
        for (int i = 0; i < 2; i++) {
            if (master[i] >= 0) close(master[i]);
            if (master2[i] >= 0) close(master2[i]);
            if (haveDir) unlink(path[i]);
        }
        if (haveDir) {
            unlink(tmp);
            rmdir(dir);
        }
    };
    for (int i = 0; i < 2; i++) {
        master[i] = openMaster(slave[i], sizeof slave[i]);
        master2[i] = openMaster(slave2[i], sizeof slave2[i]); // 挿し直したあとの装置 (先に作っておく)
        if (master[i] < 0 || master2[i] < 0) {
            printf("  no pty; skipped\n");
            cleanup();
            return;
        }
    }
    if (mkdtemp(dir) == nullptr) {
        printf("  no temporary directory; skipped\n");
        cleanup();
        return;
    }
    haveDir = true;
    snprintf(tmp, sizeof tmp, "%s/tmp", dir);
    for (int i = 0; i < 2; i++) {
        snprintf(path[i], sizeof path[i], "%s/port%d", dir, i);
        CHECK_EQ(symlink(slave[i], path[i]), 0);
    }
    char err[256] = {0};
    link = openHostFukLinkAt(path[0], path[1], err, sizeof err);
    CHECK_EQ(link != nullptr, 1);
    if (link == nullptr) {
        cleanup();
        return;
    }
    CHECK_EQ(link->online(), 1);
    const unsigned e0 = link->epoch();
    // 名前を、新しい pty に向け直してから (rename() で入れ替えるので、名前はいつも見える)、古い装置の側を閉じる
    // (読み出しが失敗する)。名前は一度も消えないので、閉じたあとは開き直さないはず
    for (int i = 0; i < 2; i++) {
        unlink(tmp);
        CHECK_EQ(symlink(slave2[i], tmp), 0);
        CHECK_EQ(rename(tmp, path[i]), 0);
    }
    close(master[0]);
    close(master[1]);
    master[0] = master[1] = -1;
    uint8_t buf[64];
    const uint64_t t0 = realUs(nullptr);
    while (link->online() && realUs(nullptr) - t0 < 3000000) {
        link->receive(0, buf, sizeof buf);
        link->receive(1, buf, sizeof buf);
        usleep(1000);
    }
    CHECK_EQ(link->online(), 0);
    // 名前が残っている間は、1 秒を過ぎても開かない (向け直した先は開けるが、消えたのを見ていない)
    for (int k = 0; k < 15; k++) {
        CHECK_EQ(link->online(), 0);
        usleep(100000);
    }
    // 1 本の名前を消す: 消えたのを見る (0.1 秒ごとに見る)。戻ったら開き直す
    unlink(path[0]);
    for (int k = 0; k < 5; k++) {
        CHECK_EQ(link->online(), 0);
        usleep(100000);
    }
    CHECK_EQ(symlink(slave2[0], path[0]), 0);
    bool back = false;
    for (int k = 0; k < 30 && !back; k++) {
        back = link->online();
        if (!back) usleep(100000);
    }
    CHECK_EQ(back, 1);
    CHECK_EQ(link->epoch() != e0, 1);
    cleanup();
}

int main() {
    testRing();
    testPty();
    testArrivalTime();
    testReopen();
    return finish();
}
