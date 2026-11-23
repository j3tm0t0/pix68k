#include "fuk_read.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include "host_fuk.h"
#include "platform/settings.h"
#include "x68k/fuk_reader.h"

namespace {

volatile sig_atomic_t interrupted = 0;
void onInterrupt(int) { interrupted = 1; }

// path がないときだけ作って全部を書く。書けなければ消す
bool saveNew(const char* path, const std::vector<uint8_t>& data) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        fprintf(stderr, "fuk-read: cannot create %s: %s\n", path, strerror(errno));
        return false;
    }
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t k = write(fd, data.data() + done, data.size() - done);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        done += (size_t)k;
    }
    bool ok = done == data.size() && fsync(fd) == 0;
    const int e = errno;
    if (close(fd) != 0) ok = false;
    if (ok) return true;
    fprintf(stderr, "fuk-read: cannot write %s: %s\n", path, strerror(e));
    unlink(path);
    return false;
}

} // namespace

int readFukDisk(const char* fukSpec, int drive, const char* path) {
    char a[platform::Settings::PATH_SIZE], b[platform::Settings::PATH_SIZE];
    const platform::FukPorts kind = platform::parseFuk(fukSpec, a, b, sizeof a);
    if (kind == platform::FukPorts::OFF || kind == platform::FukPorts::BAD) {
        fprintf(stderr, "fuk-read: needs --fuk on or --fuk PORT_A,PORT_B\n");
        return 2;
    }
    struct stat st;
    if (lstat(path, &st) == 0) {
        fprintf(stderr, "fuk-read: %s already exists; not overwritten\n", path);
        return 2;
    }
    char err[256] = {0};
    std::unique_ptr<x68k::FukLink> link(openHostFukLink(fukSpec, err, sizeof err));
    if (!link) {
        fprintf(stderr, "fuk-read: %s\n", err[0] ? err : "cannot open FUK");
        return 1;
    }
    auto fuk = std::make_unique<x68k::Fuk>(*link);
    if (!fuk->start(3000)) {
        fprintf(stderr, "fuk-read: no answer from the device on %s\n", fukSpec);
        return 1;
    }
    std::vector<uint8_t> image(x68k::FukReader::IMAGE_BYTES);
    struct sigaction sa = {}, oldInt, oldTerm;
    sa.sa_handler = onInterrupt;
    sigaction(SIGINT, &sa, &oldInt);
    sigaction(SIGTERM, &sa, &oldTerm);
    bool ok;
    {
        x68k::FukReader reader(*fuk);
        reader.start(drive, image.data(), true);
        int shown = -1;
        while (reader.poll() == x68k::FukReader::RUNNING) {
            if (interrupted) reader.cancel();
            if (reader.tracksDone() != shown) {
                shown = reader.tracksDone();
                fprintf(stderr, "\rfuk-read: %d / %d tracks, %u retries ", shown, x68k::FukReader::TRACKS, reader.retries());
                fflush(stderr);
            }
            link->waitRx(500);
        }
        char text[200];
        reader.describe(text, sizeof text);
        ok = reader.state() == x68k::FukReader::DONE;
        fprintf(stderr, "\rfuk-read: %d / %d tracks, %u retries\nfuk-read: %s\n", reader.tracksDone(),
                x68k::FukReader::TRACKS, reader.retries(), text);
    }
    // モーターを止める書き込みなど、送り残しを送り切る
    const uint64_t t0 = link->nowUs();
    while (fuk->ready() && !fuk->idleForReader() && link->nowUs() - t0 < 1000000) {
        fuk->poll();
        link->waitRx(500);
    }
    if (ok) ok = saveNew(path, image);
    if (ok) fprintf(stderr, "fuk-read: wrote %s (%u bytes)\n", path, (unsigned)image.size());
    sigaction(SIGINT, &oldInt, nullptr);
    sigaction(SIGTERM, &oldTerm, nullptr);
    return ok ? 0 : 1;
}
