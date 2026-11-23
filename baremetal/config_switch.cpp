#include "config_switch.h"

#include <circle/string.h>
#include <circle/util.h>
#include <fatfs/ff.h>
#include <fatfs/diskio.h>

// FatFs の下のセクタの読み書き (platform::switchKernelSector() の disk)
namespace {
struct CSectorDisk {
    BYTE pdrv;
    LBA_t sector;
    bool read(uint8_t* b) { return disk_read(pdrv, b, sector, 1) == RES_OK; }
    bool write(const uint8_t* b) { return disk_write(pdrv, b, sector, 1) == RES_OK; }
    bool sync() { return disk_ioctl(pdrv, CTRL_SYNC, nullptr) == RES_OK; }
};
} // namespace

bool SwitchConfigKernel(const char* drive, const char* const* sections, const char* from, const char* to, u8* buf,
                        int& status, const char*& error) {
    error = nullptr;
    status = 500;
    // SD カードだけ。USB メモリには /pix68k/ の外を書けず、NVMe は書いたものが装置のキャッシュに残ることがある。
    // Circle の diskio.cpp の CTRL_SYNC は、どの装置でも Flush の結果を返さないので (sync() はいつも成功)、失敗に気づけない
    if (strcmp(drive, "SD:") != 0) {
        error = "only for the SD card";
        status = 403;
        return false;
    }
    char* const text = (char*)buf;
    u8* const before = buf + platform::CONFIG_TXT_MAX;
    u8* const after = before + platform::CONFIG_SECTOR;
    CString full;
    full.Format("%s/config.txt", drive);
    FIL file;
    if (f_open(&file, full, FA_READ | FA_OPEN_EXISTING) != FR_OK) {
        error = "cannot open config.txt";
        status = 404;
        return false;
    }
    const FSIZE_t n = f_size(&file);
    UINT got = 0;
    size_t offset = 0;
    if (n > platform::CONFIG_TXT_MAX) {
        error = "config.txt is too large";
        status = 409;
    } else if (f_read(&file, text, (UINT)n, &got) != FR_OK || got != n) {
        error = "cannot read config.txt; not written";
    } else if ((error = platform::planKernelSwitch(text, got, sections, (unsigned)RASPPI, from, to, offset)) != nullptr) {
        status = 409;
    } else {
        // 値の 1 バイト目を読むと、FIL の sect がそのセクタの番号になる (FF_FS_TINY は 0)
        u8 c = 0;
        UINT one = 0;
        if (f_lseek(&file, (FSIZE_t)offset) != FR_OK || f_read(&file, &c, 1, &one) != FR_OK || one != 1 || file.sect == 0) {
            error = "cannot find the sector of config.txt; not written";
        } else {
            CSectorDisk disk = {file.obj.fs->pdrv, file.sect};
            // FatFs を通さないので、FatFs がほかのタスクでこのドライブを使わないようにしておく
            const int volume = file.obj.fs->ldrv;
            ff_mutex_take(volume);
            const platform::SwitchResult result = platform::switchKernelSector(disk, text, got, offset, to, before, after);
            ff_mutex_give(volume);
            switch (result) {
            case platform::SwitchResult::OK: break;
            case platform::SwitchResult::READ_ERROR: error = "cannot read the sector; not written"; break;
            case platform::SwitchResult::CHANGED: error = "the sector differs from config.txt; not written"; break;
            case platform::SwitchResult::WRITE_ERROR: error = "WRITE ERROR: check config.txt before rebooting"; break;
            case platform::SwitchResult::VERIFY_ERROR: error = "VERIFY ERROR: check config.txt before rebooting"; break;
            }
        }
    }
    f_close(&file);
    return error == nullptr;
}
