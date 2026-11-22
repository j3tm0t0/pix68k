// ホスト版: Linux のブロックデバイス (USB-FDD の /dev/sdX) を、実物のフロッピーディスクドライブとして読む (#286)。
// デバイスは O_RDONLY で開き、読むだけ。Linux 以外では isBlockDevice() がいつも false を返し、open() は失敗する。
// (<linux/fs.h> の BLOCK_SIZE がほかのヘッダの名前と重なるので、中身は real_floppy_dev.cpp に置く)
#pragma once

#include <stdint.h>

#include "platform/real_floppy.h"

namespace host {

bool isBlockDevice(const char* path);

class BlockDeviceFloppy final : public platform::RealFloppySource {
public:
    ~BlockDeviceFloppy() override;
    // 開けなければ false (errno に理由)
    bool open(const char* path);
    bool geometry(uint32_t& blockBytes, uint32_t& blocks) override;
    bool read(uint32_t lba, uint32_t count, uint8_t* dst) override;
    // 開いているファイル記述子 (開いていなければ -1)。読み取り専用で開いていることを試験で確かめるため
    int descriptor() const { return fd_; }

private:
    int fd_ = -1;
    uint32_t blockBytes_ = 0;
};

} // namespace host
