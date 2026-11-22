// ホスト版: Linux のブロックデバイスを、実物のフロッピーディスクドライブとして読む (real_floppy_dev.h)
#include "real_floppy_dev.h"

#include <errno.h>

#ifdef __linux__
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace host {

#ifdef __linux__
bool isBlockDevice(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISBLK(st.st_mode);
}

BlockDeviceFloppy::~BlockDeviceFloppy() {
    if (fd_ >= 0) close(fd_);
}

bool BlockDeviceFloppy::open(const char* path) {
    if (fd_ >= 0) close(fd_);
    fd_ = ::open(path, O_RDONLY | O_CLOEXEC);
    return fd_ >= 0;
}

bool BlockDeviceFloppy::geometry(uint32_t& blockBytes, uint32_t& blocks) {
    int ss = 0;
    uint64_t bytes = 0;
    if (fd_ < 0 || ioctl(fd_, BLKSSZGET, &ss) != 0 || ioctl(fd_, BLKGETSIZE64, &bytes) != 0 || ss <= 0) return false;
    blockBytes_ = (uint32_t)ss;
    blockBytes = blockBytes_;
    blocks = (uint32_t)(bytes / blockBytes_);
    return bytes != 0;
}

bool BlockDeviceFloppy::read(uint32_t lba, uint32_t count, uint8_t* dst) {
    if (fd_ < 0 || blockBytes_ == 0) return false;
    size_t left = (size_t)count * blockBytes_;
    off_t at = (off_t)lba * blockBytes_;
    while (left > 0) {
        const ssize_t n = pread(fd_, dst, left, at);
        if (n <= 0) return false;
        dst += n;
        at += n;
        left -= (size_t)n;
    }
    return true;
}
#else
bool isBlockDevice(const char*) { return false; }
BlockDeviceFloppy::~BlockDeviceFloppy() {}
bool BlockDeviceFloppy::open(const char*) {
    errno = ENOTSUP;
    return false;
}
bool BlockDeviceFloppy::geometry(uint32_t&, uint32_t&) { return blockBytes_ != 0; } // いつも false
bool BlockDeviceFloppy::read(uint32_t, uint32_t, uint8_t*) { return false; }
#endif

} // namespace host
