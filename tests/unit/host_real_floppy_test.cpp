// host/real_floppy_dev.cpp のテスト: 実物のドライブ (Linux のブロックデバイス) を、読み取り専用で開くこと (#286)。
// ブロックデバイスは試験では用意できないので、ふつうのファイルを開かせて、開き方だけを確かめる
// (ブロックデバイスではないので、geometry() は失敗する)。Linux 以外では open() がいつも失敗するので、何も確かめない
#include "real_floppy_dev.h"
#include "test_check.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main() {
#ifdef _WIN32
    // Windows には /tmp がないので、TEMP の下に作る
    char path[1024];
    const char* tmp = getenv("TEMP");
    snprintf(path, sizeof path, "%s/pix68k_real_floppy_XXXXXX", tmp && *tmp ? tmp : ".");
#else
    char path[] = "/tmp/pix68k_real_floppy_XXXXXX";
#endif
    const int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }
    static const char kData[] = "0123456789abcdef";
    CHECK_EQ(write(fd, kData, sizeof kData), (ssize_t)sizeof kData);
    close(fd);

    CHECK_EQ(host::isBlockDevice(path), false);
    CHECK_EQ(host::isBlockDevice("/nonexistent/device"), false);
    host::BlockDeviceFloppy dev;
    CHECK_EQ(dev.descriptor(), -1);
    uint8_t buf[16];
    CHECK_EQ(dev.read(0, 1, buf), false); // 開く前
#ifdef __linux__
    CHECK_EQ(dev.open(path), true);
    const int d = dev.descriptor();
    CHECK_EQ(d >= 0, true);
    // 読み取り専用で開いている。書こうとしても書けない
    CHECK_EQ(fcntl(d, F_GETFL) & O_ACCMODE, O_RDONLY);
    errno = 0;
    CHECK_EQ(write(d, "x", 1), (ssize_t)-1);
    CHECK_EQ(errno, EBADF);
    // ブロックデバイスではないので、大きさは答えない (読みもしない)
    uint32_t blockBytes = 0, blocks = 0;
    CHECK_EQ(dev.geometry(blockBytes, blocks), false);
    CHECK_EQ(dev.read(0, 1, buf), false);
    // ファイルの中身は変わっていない
    char back[sizeof kData] = {0};
    FILE* f = fopen(path, "rb");
    CHECK_EQ(f != nullptr, true);
    if (f) {
        CHECK_EQ(fread(back, 1, sizeof back, f), sizeof back);
        fclose(f);
    }
    CHECK_EQ(memcmp(back, kData, sizeof kData), 0);
#else
    CHECK_EQ(dev.open(path), false);
    printf("not Linux: nothing to check\n");
#endif
    unlink(path);
    return finish();
}
