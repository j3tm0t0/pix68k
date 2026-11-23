// 更新 (#369) の途中で電源が切れたときに、起動に使うファイル (config.txt と、その kernel= のファイル) が残るか。
// Circle の FatFs をホストで動かし、メモリの上の FAT のディスクで、ベアメタル版 (baremetal/host_net.cpp) と同じ順に
//   使っていない名前へカーネルを書く (.part → f_sync → commitUpload) → update.txt を同じように書く → config.txt の 1 セクタを書き換える
// を動かす。セクタの書き込みを 1 回目、2 回目 … で打ち切り (その後の書き込みは届かない)、毎回マウントし直して確かめる。
// セクタの書き込みは「届くか、届かないか」で模す (書いている最中のセクタが壊れる、書き込みの順番が入れ替わる、という SD カードの
// 故障は模していない)
#include <stdlib.h>
#include <string.h>

#include "ff.h" // diskio.h より先に
#include "diskio.h"
#include "platform/config_kernel.h"
#include "platform/net_files.h"
#include "test_check.h"

namespace {
constexpr int kSectors = 72000; // 35MB (FAT32 になる大きさ)
BYTE g_disk[kSectors * 512], g_base[kSectors * 512];
long g_left = -1; // あと何セクタ書けるか (-1 なら制限なし)
bool g_error = false; // 打ち切った後の書き込みをエラーにする (false なら黙って捨てる)
long g_writes = 0;
} // namespace

extern "C" {
DSTATUS disk_initialize(BYTE) { return 0; }
DSTATUS disk_status(BYTE) { return 0; }
DRESULT disk_read(BYTE, BYTE* buf, LBA_t sector, UINT count) {
    memcpy(buf, g_disk + sector * 512, count * 512);
    return RES_OK;
}
DRESULT disk_write(BYTE, const BYTE* buf, LBA_t sector, UINT count) {
    for (UINT i = 0; i < count; i++) {
        g_writes++;
        if (g_left == 0) {
            if (g_error) return RES_ERROR;
            continue;
        }
        if (g_left > 0) g_left--;
        memcpy(g_disk + (sector + i) * 512, buf + i * 512, 512);
    }
    return RES_OK;
}
DRESULT disk_ioctl(BYTE, BYTE cmd, void* buf) {
    if (cmd == GET_SECTOR_COUNT) *(LBA_t*)buf = kSectors;
    if (cmd == GET_SECTOR_SIZE) *(WORD*)buf = 512;
    if (cmd == GET_BLOCK_SIZE) *(DWORD*)buf = 1;
    return RES_OK;
}
void* ff_memalloc(UINT n) { return malloc(n); }
void ff_memfree(void* p) { free(p); }
}

namespace {
constexpr UINT kSize = 70000; // カーネルの代わり
BYTE g_old[kSize], g_new[kSize], g_stale[kSize], g_buf[kSize + 16];
// 2 つ目は、kernel= が 2 番目のセクタにある (配布している config.txt の Pi 4 / Pi 5 と同じ)
#define CONFIG_HEAD "# 0123456789012345678901234567890123456789012345678901234567890\n"
#define CONFIG_TAIL "# comment\narm_64bit=1\n[pi4]\nkernel=kernel8-rpi4.img\n[pi3]\nkernel=kernel8.img\ngpu_mem=64\n[all]\n"
const char kConfig1[] = CONFIG_TAIL;
const char kConfig2[] = CONFIG_HEAD CONFIG_HEAD CONFIG_HEAD CONFIG_HEAD CONFIG_HEAD CONFIG_HEAD CONFIG_HEAD CONFIG_HEAD CONFIG_TAIL;
const char* g_config = kConfig1;
UINT g_configSize = sizeof kConfig1 - 1;
const char* const kPi3B[] = {"pi3", nullptr};

struct Fs {
    platform::FsResult conv(FRESULT r) {
        if (r == FR_OK) return platform::FsResult::OK;
        return r == FR_NO_FILE || r == FR_NO_PATH ? platform::FsResult::NO_FILE : platform::FsResult::ERROR;
    }
    platform::FsResult exists(const char* p) { FILINFO fi; return conv(f_stat(p, &fi)); }
    bool isDirectory(const char* p) { FILINFO fi; return f_stat(p, &fi) == FR_OK && (fi.fattrib & AM_DIR) != 0; }
    platform::FsResult remove(const char* p) { return conv(f_unlink(p)); }
    platform::FsResult rename(const char* a, const char* b) {
        const char* colon = strchr(b, ':');
        return conv(f_rename(a, colon ? colon + 1 : b));
    }
};

bool put(const char* path, const void* data, UINT n) {
    char part[64], old[64];
    platform::withSuffix(path, ".part", part, sizeof part);
    platform::withSuffix(path, ".old", old, sizeof old);
    FIL f;
    if (f_open(&f, part, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    for (UINT i = 0; i < n;) {
        UINT k = n - i > 32768 ? 32768 : n - i, done = 0;
        if (f_write(&f, (const BYTE*)data + i, k, &done) != FR_OK || done != k) return false;
        i += k;
    }
    const FRESULT s = f_sync(&f), c = f_close(&f);
    if (s != FR_OK || c != FR_OK) return false;
    Fs fs;
    return platform::commitUpload(fs, path, part, old) == platform::FsResult::OK;
}

bool load(const char* path, void* data, UINT max, UINT& n) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    const bool ok = f_read(&f, data, max, &n) == FR_OK;
    f_close(&f);
    return ok;
}

struct Sector {
    BYTE pdrv;
    LBA_t sect;
    bool read(uint8_t* b) { return disk_read(pdrv, b, sect, 1) == RES_OK; }
    bool write(const uint8_t* b) { return disk_write(pdrv, b, sect, 1) == RES_OK; }
    bool sync() { return disk_ioctl(pdrv, CTRL_SYNC, nullptr) == RES_OK; }
};

bool switchTo(const char* from, const char* to) {
    FIL f;
    static char text[1024];
    UINT n = 0, one = 0;
    uint8_t b1[512], b2[512];
    if (f_open(&f, "0:/config.txt", FA_READ) != FR_OK) return false;
    if (f_read(&f, text, sizeof text, &n) != FR_OK) return false;
    size_t off = 0;
    if (platform::planKernelSwitch(text, n, kPi3B, 3, from, to, off) != nullptr) return false;
    BYTE c;
    if (f_lseek(&f, off) != FR_OK || f_read(&f, &c, 1, &one) != FR_OK || one != 1) return false;
    Sector d{f.obj.fs->pdrv, f.sect};
    const bool ok = platform::switchKernelSector(d, text, n, off, to, b1, b2) == platform::SwitchResult::OK;
    f_close(&f);
    return ok;
}

// 更新の手順。途中で失敗したら (cli と同じく) そこで止める。書き込みを捨てているときは、読み戻しの確認で止まる
bool update() {
    UINT n = 0;
    if (!put("0:/pix68k8.img", g_new, kSize)) return false;
    if (!load("0:/pix68k8.img", g_buf, sizeof g_buf, n) || n != kSize || memcmp(g_buf, g_new, kSize) != 0) return false;
    if (!put("0:/pix68k/update.txt", "previous=kernel8.img\n", 21)) return false;
    return switchTo("kernel8.img", "pix68k8.img");
}

// 起動に使うファイルが残っているか: config.txt が前か後のどちらかで、その kernel= のファイルが完全。
// 0 = 前の版、1 = 新しい版、-1 = 起動できない
int bootable() {
    static FATFS fs;
    if (f_mount(&fs, "0:", 1) != FR_OK) return -1;
    static char text[1024];
    UINT n = 0;
    if (!load("0:/config.txt", text, sizeof text, n) || n != g_configSize) return -1;
    const platform::ConfigKernel k = platform::findConfigKernel(text, n, kPi3B);
    if (k.error != nullptr || k.length != 11) return -1;
    char path[32] = "0:/";
    memcpy(path + 3, text + k.offset, 11);
    path[14] = 0;
    const bool isNew = strcmp(path, "0:/pix68k8.img") == 0;
    // 値のほかは変わっていない
    memcpy(text + k.offset, "kernel8.img", 11);
    if (memcmp(text, g_config, n) != 0) return -1;
    if (!load(path, g_buf, sizeof g_buf, n) || n != kSize) return -1;
    return memcmp(g_buf, isNew ? g_new : g_old, kSize) == 0 ? isNew : -1;
}
} // namespace

int main() {
    for (UINT i = 0; i < kSize; i++) {
        g_old[i] = (BYTE)(i * 7 + 1);
        g_new[i] = (BYTE)(i * 13 + 5);
        g_stale[i] = (BYTE)(i * 29 + 9);
    }
    for (int second = 0; second < 2; second++) {
    if (second) {
        g_config = kConfig2;
        g_configSize = sizeof kConfig2 - 1;
        CHECK_EQ(platform::findConfigKernel(kConfig2, g_configSize, kPi3B).offset / 512, 1u);
    }
    // FAT16 と FAT32、前の更新の残り (使っていない名前の古い版) がある / ない
    for (int fmt = 0; fmt < 2; fmt++) {
        for (int stale = 0; stale < 2; stale++) {
            static BYTE work[4096];
            static FATFS fs;
            g_left = -1;
            memset(g_disk, 0, sizeof g_disk);
            const MKFS_PARM opt = {(BYTE)(fmt == 0 ? FM_FAT : FM_FAT32), 0, 0, 0, fmt == 0 ? 2048u : 512u};
            CHECK_EQ(f_mkfs("0:", &opt, work, sizeof work), FR_OK);
            CHECK_EQ(f_mount(&fs, "0:", 1), FR_OK);
            CHECK_EQ(fs.fs_type, fmt == 0 ? FS_FAT16 : FS_FAT32);
            CHECK_EQ(f_mkdir("0:/pix68k"), FR_OK);
            CHECK_EQ(put("0:/config.txt", g_config, g_configSize), true);
            CHECK_EQ(put("0:/kernel8.img", g_old, kSize), true);
            if (stale) {
                CHECK_EQ(put("0:/pix68k8.img", g_stale, kSize), true);
                CHECK_EQ(put("0:/pix68k/update.txt", "previous=x\n", 11), true);
            }
            f_unmount("0:");
            memcpy(g_base, g_disk, sizeof g_disk);
            // 切らなければ新しい版になる
            CHECK_EQ(bootable(), 0);
            g_writes = 0;
            CHECK_EQ(f_mount(&fs, "0:", 1), FR_OK);
            CHECK_EQ(update(), true);
            const long total = g_writes;
            CHECK_EQ(bootable(), 1);
            CHECK_EQ(total > 20, true);
            // 前の版のファイルは残り、戻すこともできる
            CHECK_EQ(switchTo("pix68k8.img", "kernel8.img"), true);
            CHECK_EQ(bootable(), 0);
            for (int error = 0; error < 2; error++) {
                int bad = 0, newCount = 0;
                for (long cut = 0; cut <= total; cut++) {
                    memcpy(g_disk, g_base, sizeof g_disk);
                    g_error = error != 0;
                    g_left = cut;
                    if (f_mount(&fs, "0:", 1) == FR_OK) update();
                    f_unmount("0:");
                    g_left = -1;
                    const int b = bootable();
                    if (b < 0) {
                        bad++;
                        printf("NG: fmt %d stale %d error %d: cut at %ld / %ld\n", fmt, stale, error, cut, total);
                    }
                    newCount += b == 1;
                }
                CHECK_EQ(bad, 0);
                CHECK_EQ(newCount, 1); // 新しい版になるのは、最後の 1 セクタまで書けたときだけ
                printf("second %d fmt %d stale %d error %d: %ld cuts, bad %d\n", second, fmt, stale, error, total + 1, bad);
            }
        }
    }
    }
    return finish();
}
