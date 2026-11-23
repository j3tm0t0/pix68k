// platform/config_kernel.h (config.txt の kernel= の値だけを書き換える) のテスト。読み方の例は tools/pix68kcli/update_test.go と同じ
#include "platform/config_kernel.h"
#include "test_check.h"

#include <initializer_list>
#include <stdio.h>
#include <string.h>

using namespace platform;

static const char* const kPi400[] = {"pi4", "pi400", nullptr};
static const char* const kPi3B[] = {"pi3", nullptr};

static void find(const char* cfg, const char* want) {
    const ConfigKernel k = findConfigKernel(cfg, strlen(cfg), kPi400);
    const bool ok = want ? k.error == nullptr && k.length == strlen(want) && memcmp(cfg + k.offset, want, k.length) == 0
                         : k.error != nullptr;
    if (!ok) printf("NG: [%s]\n", cfg);
    CHECK_EQ(ok, true);
}

struct Mem {
    uint8_t s[512];
    bool failRead = false, failWrite = false, failSync = false, drop = false;
    int writes = 0;
    bool read(uint8_t* b) { memcpy(b, s, 512); return !failRead; }
    bool write(const uint8_t* b) {
        writes++;
        if (failWrite) return false;
        if (!drop) memcpy(s, b, 512);
        return true;
    }
    bool sync() { return !failSync; }
};

int main() {
    find("kernel=a.img", "a.img");
    find("kernel=a.img\r\nkernel=b.img\r\n", "b.img");
    find("[pi4]\nkernel=a.img\n[pi400]\nkernel=b.img\n", "b.img");
    find("[pi400]\nkernel=b.img\n[pi5]\nkernel=c.img\n", "b.img");
    find("#kernel=x.img\nkernel=a.img\n", "a.img");
    find("[none]\nkernel=x.img\ninclude y.txt\n[all]\nkernel=a.img\n", "a.img");
    find("kernel=a.img\n[none]\n[pi4]\nkernel=x.img\n", nullptr);
    find("kernel=a.img\n[none]\n[pi3]\nkernel=x.img\n", "a.img");
    find("kernel a.img\n", nullptr);
    find("[none]\n[all]\n[pi4]\nkernel=b.img\n", "b.img");
    find("[gpio4=1]\ngpu_mem=64\n[all]\nkernel=a.img\n", "a.img");
    find("kernel_address=0x80000\nkernel=a.img\n", "a.img");
    for (const char* c : {"[pi5]\nkernel=x.img\n", "[tryboot]\nkernel=x.img\n[all]\nkernel=a.img\n", "[gpio4=1]\n[pi4]\nkernel=x.img\n",
                          "include x.txt\nkernel=a.img\n", "os_prefix=v1/\nkernel=a.img\n", "kernel = a.img\n", " kernel=a.img\n",
                          "kernel=a.img \n", "kernel=sub/a.img\n", "kernel=\n", "kernel\n", "[pi4\nkernel=a.img\n", "", "\n"})
        find(c, nullptr);

    // 名前は機種ごとの 2 つだけで、長さが同じ
    for (unsigned r = 2; r <= 5; r++) {
        const char* a = r == 2 ? "kernel7.img" : r == 3 ? "kernel8.img" : r == 4 ? "kernel8-rpi4.img" : "kernel_2712.img";
        const char* b = otherKernelName(r, a);
        CHECK_EQ(b != nullptr && strlen(a) == strlen(b) && strcmp(a, b) != 0, true);
        CHECK_EQ(strcmp(otherKernelName(r, b), a), 0);
    }
    CHECK_EQ(otherKernelName(3, "kernel7.img") == nullptr, true);
    CHECK_EQ(otherKernelName(3, "x.img") == nullptr, true);

    // 位置: どのずれでも、セクタをまたがなければ値だけが変わり、またげば断る
    for (int pad = 0; pad < 1100; pad++) {
        static char t[2048];
        memset(t, '#', pad);
        int n = pad;
        if (pad) t[n++] = '\n';
        n += snprintf(t + n, sizeof t - n, "kernel=kernel8.img\n#tail\n");
        size_t off = 0;
        const char* err = planKernelSwitch(t, n, kPi3B, 3, "kernel8.img", "pix68k8.img", off);
        const size_t v = (size_t)(strstr(t, "kernel=kernel8") - t) + 7;
        const bool cross = v / 512 != (v + 10) / 512;
        CHECK_EQ(err != nullptr, cross);
        if (err != nullptr) continue;
        CHECK_EQ(off, v);
        Mem m;
        memset(m.s, 0xaa, 512); // ファイルの終わりより後ろ (前の中身が残っていることがある) は変えない
        const size_t start = v / 512 * 512, len = (size_t)n - start < 512 ? n - start : 512;
        memcpy(m.s, t + start, len);
        uint8_t want[512], b1[512], b2[512];
        memcpy(want, m.s, 512);
        memcpy(want + (v - start), "pix68k8.img", 11);
        Mem keep = m;
        CHECK_EQ(switchKernelSector(m, t, n, off, "pix68k8.img", b1, b2) == SwitchResult::OK, true);
        CHECK_EQ(memcmp(m.s, want, 512), 0);
        CHECK_EQ(m.writes, 1);
        // 読んだあとで変わっていれば書かない
        m = keep;
        m.s[0] ^= 1;
        CHECK_EQ(switchKernelSector(m, t, n, off, "pix68k8.img", b1, b2) == SwitchResult::CHANGED, true);
        CHECK_EQ(m.writes, 0);
        m = keep;
        m.failRead = true;
        CHECK_EQ(switchKernelSector(m, t, n, off, "pix68k8.img", b1, b2) == SwitchResult::READ_ERROR, true);
        CHECK_EQ(m.writes, 0);
        m = keep;
        m.failWrite = true;
        CHECK_EQ(switchKernelSector(m, t, n, off, "pix68k8.img", b1, b2) == SwitchResult::WRITE_ERROR, true);
        m = keep;
        m.drop = true;
        CHECK_EQ(switchKernelSector(m, t, n, off, "pix68k8.img", b1, b2) == SwitchResult::VERIFY_ERROR, true);
    }
    // 断るもの
    const char* c = "[pi3]\nkernel=kernel8.img\n";
    size_t off = 0;
    CHECK_EQ(planKernelSwitch(c, strlen(c), kPi3B, 3, "kernel8.img", "pix68k8.img", off) == nullptr, true);
    CHECK_EQ(planKernelSwitch(c, strlen(c), kPi3B, 3, "pix68k8.img", "kernel8.img", off) != nullptr, true); // 今は kernel8
    CHECK_EQ(planKernelSwitch(c, strlen(c), kPi3B, 3, "kernel8.img", "kernel8.img", off) != nullptr, true);
    CHECK_EQ(planKernelSwitch(c, strlen(c), kPi3B, 3, "kernel8.img", "evil....img", off) != nullptr, true);
    CHECK_EQ(planKernelSwitch(c, strlen(c), kPi3B, 4, "kernel8.img", "pix68k8.img", off) != nullptr, true); // ほかの機種
    CHECK_EQ(planKernelSwitch(c, strlen(c), kPi400, 4, "kernel8-rpi4.img", "pix68k8-rpi4.img", off) != nullptr, true);
    return finish();
}
