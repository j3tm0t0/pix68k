// ota.c の O 命令を、ブート ROM の代わり (host_stubs/) とメモリの上のフラッシュで試す:
//   cc -std=c11 -Wall -Wextra -Werror -DPICO_KBD_VERSION=7 -Ihost_stubs -I. -o test_ota_host test_ota_host.c core.c && ./test_ota_host
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ota.c"
#include "ota_rp2350.c"

static int s_fail;
#define CHECK(x) do { if (!(x)) { printf("NG %s:%d: %s\n", __FILE__, __LINE__, #x); s_fail++; } } while (0)

// ---- フラッシュ (2MB + 8KB)。A は 0x2000 から 1MB、B はその次の 1MB。動いている側は s_running (0 = A、1 = B) ----
#define PART_A 0x2000u
#define PART_SIZE 0x100000u
#define PART_B (PART_A + PART_SIZE)
static uint8_t s_flash[PART_B + PART_SIZE];
static unsigned s_ops;        // フラッシュの操作の回数
static uint32_t s_lastProgram; // 最後に書いた場所
static int s_failAt = -1;      // この回の操作を失敗させる
static jmp_buf s_rebooted;
static uint32_t s_rebootFlags, s_rebootP0;
static char s_lastReply[128];
static int s_bootTypeNow, s_buyCalls;
static uint8_t s_tbybNow;
static int s_running;      // 動いているパーティション (0 = A、1 = B)。書く先はもう一方
static bool s_ptLoaded = true; // パーティション表を読んであるか
static int s_loadCalls;
static int s_buyErr;       // 0 でなければ、確定はこのエラーを返す
static int s_targetErr;    // 0 でなければ、書く先を聞かれたらこのエラーを返す
static uint32_t part_start(int p) { return p == 0 ? PART_A : PART_B; }

int rom_flash_op(cflash_flags_t flags, uintptr_t addr, uint32_t size, uint8_t* buf) {
    uint32_t op = flags.flags >> CFLASH_OP_LSB;
    uint32_t off = (uint32_t)(addr - XIP_BASE);
    if ((int)s_ops++ == s_failAt) return -1;
    CHECK(size == 4096 && off % 4096 == 0 && off + size <= sizeof s_flash);
    CHECK(off >= part_start(!s_running) && off < part_start(!s_running) + PART_SIZE); // 動いている側には触らない
    if (op == CFLASH_OP_VALUE_ERASE) memset(s_flash + off, 0xff, size);
    else if (op == CFLASH_OP_VALUE_PROGRAM) {
        for (uint32_t i = 0; i < size; i++) s_flash[off + i] &= buf[i]; // 消していない所に書くと化ける
        s_lastProgram = off;
    } else memcpy(buf, s_flash + off, size);
    return 0;
}
int rom_get_uf2_target_partition(uint8_t* w, uint32_t size, uint32_t family, resident_partition_t* out) {
    (void)w;
    CHECK(size >= 4096 && family == RP2350_ARM_S_FAMILY_ID);
    if (s_targetErr) return s_targetErr;
    if (!s_ptLoaded) return BOOTROM_ERROR_PRECONDITION_NOT_MET;
    // 本物と同じく、選んだパーティションの番号を返す (0 でなくても成功)
    uint32_t start = part_start(!s_running);
    out->permissions_and_location = (start / 4096) | ((start + PART_SIZE) / 4096 - 1) << 13;
    return !s_running;
}
int rom_load_partition_table(uint8_t* w, uint32_t size, bool force) {
    (void)w;
    (void)force;
    CHECK(size >= 3264);
    s_loadCalls++;
    s_ptLoaded = true;
    return 0;
}
intptr_t rom_flash_runtime_to_storage_addr(uintptr_t addr) { return (intptr_t)(addr + part_start(s_running)); }
void rom_flash_flush_cache(void) {}
int rom_reboot(uint32_t flags, uint32_t delay_ms, uint32_t p0, uint32_t p1) {
    (void)delay_ms;
    (void)p1;
    s_rebootFlags = flags;
    s_rebootP0 = p0;
    longjmp(s_rebooted, 1);
}
int rom_explicit_buy(uint8_t* b, uint32_t size) {
    (void)b;
    CHECK(size >= 4096);
    s_buyCalls++;
    if (s_buyErr) return s_buyErr;
    s_tbybNow = 0;
    return 0;
}
bool rom_get_boot_info(boot_info_t* info) {
    info->partition = (int8_t)s_running;
    info->tbyb_and_update_info = s_tbybNow;
    return true;
}
int rom_get_last_boot_type(void) { return s_bootTypeNow; }
bool app_reply(const char* s) {
    snprintf(s_lastReply, sizeof s_lastReply, "%s", s);
    return true;
}
void app_flush(void) {}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64(const uint8_t* d, unsigned n, char* out) {
    for (unsigned i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        *out++ = kB64[v >> 18];
        *out++ = kB64[v >> 12 & 63];
        *out++ = i + 1 < n ? kB64[v >> 6 & 63] : '=';
        *out++ = i + 2 < n ? kB64[v & 63] : '=';
    }
    *out = 0;
}

static bool reply_is(const char* head) { return strncmp(s_lastReply, head, strlen(head)) == 0; }

// イメージを送る。true なら再起動まで進んだ
static bool send(const uint8_t* image, uint32_t size, uint32_t crc, uint32_t stopAfter) {
    char line[600];
    snprintf(line, sizeof line, "B %u %08x", (unsigned)size, (unsigned)crc);
    ota_command(line);
    if (!reply_is("ota begin")) return false;
    for (uint32_t pos = 0; pos < size && pos < stopAfter; pos += 336) {
        unsigned n = size - pos < 336 ? size - pos : 336;
        strcpy(line, "D ");
        b64(image + pos, n, line + 2);
        ota_command(line);
        if (!reply_is("ota ok")) return false;
    }
    if (setjmp(s_rebooted)) return true;
    ota_command(stopAfter == UINT32_MAX - 1 ? "E F" : "E");
    return false;
}

static void fill_flash(void) { memset(s_flash, 0x5a, sizeof s_flash); }

// ---- ブート ROM の起動のしかたの代わり ----
// ふつうの起動 (電源を入れた、R、ウォッチドッグ): 確定していない版があれば、もう一方 (前の版) で起動する
static void boot_normal(void) {
    if (s_tbybNow & BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING) s_running = !s_running;
    s_tbybNow = 0;
    s_bootTypeNow = 0;
    ota_init();
}
// 更新のあとの起動: 書いた側で 1 回だけ起動する。確定待ち
static void boot_update(void) {
    CHECK(s_rebootFlags & REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE);
    CHECK(s_rebootP0 == XIP_BASE + part_start(!s_running));
    s_running = !s_running;
    s_tbybNow = BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING;
    s_bootTypeNow = BOOT_TYPE_FLASH_UPDATE;
    ota_init();
}
// 1 回の更新: 送る → 更新のあとの起動 → 確定。from で動いていて、もう一方に入る
static void update_once(const uint8_t* image, uint32_t size, int from) {
    CHECK(s_running == from);
    uint32_t to = part_start(!from), keep = part_start(from);
    memset(s_flash + keep, 0x5a, PART_SIZE);
    int buys = s_buyCalls;
    CHECK(send(image, size, crc32_update(0, image, size), UINT32_MAX));
    CHECK(reply_is("ota done"));
    CHECK(memcmp(s_flash + to, image, size) == 0 && s_lastProgram == to);
    for (uint32_t i = 0; i < PART_SIZE; i++) if (s_flash[keep + i] != 0x5a) { CHECK(!"動いている側が変わった"); break; }
    boot_update();
    CHECK(s_running == !from);
    CHECK(!ota_wd_ok()); // 確定の前は、常時のウォッチドッグを掛けない
    ota_task(false);
    CHECK(s_buyCalls == buys && !ota_wd_ok());
    ota_command("B 8192 0"); // 確定の前は受けない
    CHECK(reply_is("ota error trial"));
    ota_run("B 8192 0", false); // トークンを通った相手がいなければ、ota_run でも同じ
    CHECK(reply_is("ota error trial") && s_buyCalls == buys);
    if (from == 1) {
        // トークンと O B が同じ受信データで来た: 先に確定してから受ける
        ota_run("B 8192 0", true);
        CHECK(s_buyCalls == buys + 1 && reply_is("ota begin") && ota_wd_ok());
        ota_command("A");
        return;
    }
    ota_task(true);
    CHECK(s_buyCalls == buys + 1 && reply_is("ota bought rc=0 tbyb=00") && ota_wd_ok());
}

int main(void) {
    static uint8_t image[300000];
    for (unsigned i = 0; i < sizeof image; i++) image[i] = (uint8_t)(i * 7 + (i >> 8));
    // 本物と同じ IMAGE_DEF の始まり (TBYB つき): 0xffffded3、0x42、0x01、フラグ 0x9021
    static const uint8_t kImageDef[] = {0xd3, 0xde, 0xff, 0xff, 0x42, 0x01, 0x21, 0x90};
    memcpy(image + 0x138, kImageDef, sizeof kImageDef);
    // 確定の済んでいない版 (起動してから ota_init がまだ、も含む) は、次のイメージを受けない
    ota_command("B 8192 0");
    CHECK(reply_is("ota error trial"));
    s_bootTypeNow = 0;
    ota_init();
    static const uint32_t kSizes[] = {4096, 4097, 8192, 12345, 299999, 300000};
    for (unsigned k = 0; k < sizeof kSizes / sizeof kSizes[0]; k++) {
        uint32_t size = kSizes[k];
        fill_flash();
        s_ops = 0;
        CHECK(send(image, size, crc32_update(0, image, size), UINT32_MAX));
        CHECK(reply_is("ota done"));
        CHECK(memcmp(s_flash + PART_B, image, size) == 0);
        CHECK(s_lastProgram == PART_B); // 先頭の 4KB を最後に書く
        CHECK(s_rebootP0 == XIP_BASE + PART_B && (s_rebootFlags & REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE));
        for (uint32_t i = 0; i < PART_A + PART_SIZE; i++) if (s_flash[i] != 0x5a) { CHECK(!"A が変わった"); break; }
    }
    // CRC が違う、途中でやめる、足りないまま終える: 再起動せず、書く先の先頭は消えたまま
    fill_flash();
    CHECK(!send(image, 20000, 0x12345678, UINT32_MAX) && reply_is("ota error crc"));
    CHECK(s_flash[PART_B] == 0xff && s_flash[PART_B + 4095] == 0xff);
    fill_flash();
    CHECK(!send(image, 20000, crc32_update(0, image, 20000), 10000) && reply_is("ota error short"));
    CHECK(s_flash[PART_B] == 0xff);
    ota_command("D QUJD");
    CHECK(reply_is("ota error not-started"));
    ota_command("E");
    CHECK(reply_is("ota error not-started"));
    // 大きすぎる、小さすぎる、書き方が違う
    ota_command("B 1048577 0");
    CHECK(reply_is("ota error too-big"));
    ota_command("B 100 0");
    CHECK(reply_is("ota error bad-args"));
    ota_command("B 5000 0 x");
    CHECK(reply_is("ota error bad-args"));
    ota_command("B 5000"); // CRC がない
    CHECK(reply_is("ota error bad-args"));
    ota_command("B 5000 ");
    CHECK(reply_is("ota error bad-args"));
    ota_command("Z");
    CHECK(reply_is("ota error bad-args"));
    // 宣言した大きさを超えるデータは受けない
    ota_command("B 4096 0");
    CHECK(reply_is("ota begin"));
    char line[600] = "D ";
    for (int i = 0; i < 13; i++) {
        b64(image, 336, line + 2);
        ota_command(line);
    }
    CHECK(reply_is("ota error bad-data"));
    // フラッシュの操作が失敗したら、そこでやめる
    for (int at = 0; at < 12; at++) {
        fill_flash();
        s_ops = 0;
        s_failAt = at;
        bool done = send(image, 12345, crc32_update(0, image, 12345), UINT32_MAX);
        s_failAt = -1;
        CHECK(!done && reply_is("ota error"));
    }
    // TBYB の付いていないイメージ、IMAGE_DEF のないイメージは断る (書く先の先頭は消えたまま)。O E F なら入れる
    {
        static uint8_t plain[20000];
        memcpy(plain, image, sizeof plain);
        plain[0x138 + 7] = 0x10; // TBYB なし
        uint32_t c = crc32_update(0, plain, sizeof plain);
        fill_flash();
        CHECK(!send(plain, sizeof plain, c, UINT32_MAX) && reply_is("ota error not-tbyb"));
        CHECK(s_flash[PART_B] == 0xff);
        plain[0x138] = 0; // IMAGE_DEF がない
        fill_flash();
        CHECK(!send(plain, sizeof plain, crc32_update(0, plain, sizeof plain), UINT32_MAX) && reply_is("ota error no-image-def"));
        CHECK(s_flash[PART_B] == 0xff);
        plain[0x138] = 0xd3;
        fill_flash();
        CHECK(!send(plain, sizeof plain, c, UINT32_MAX)); // 断られて、もう受け付けていない
        CHECK(send(plain, sizeof plain, c, UINT32_MAX - 1)); // (stopAfter が UINT32_MAX - 1 なら O E F で終える)
        CHECK(reply_is("ota done") && memcmp(s_flash + PART_B, plain, sizeof plain) == 0);
        ota_command("E x");
        CHECK(reply_is("ota error bad-args"));
    }
    // やめる
    ota_command("B 8192 0");
    ota_command("A");
    CHECK(reply_is("ota aborted") && s_flash[PART_B] == 0xff);
    // 確定: フラッシュの更新のあとの起動で、無線 LAN につながったときに 1 回だけ
    s_bootTypeNow = BOOT_TYPE_FLASH_UPDATE;
    s_tbybNow = BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING;
    ota_init();
    ota_task(false);
    CHECK(s_buyCalls == 0);
    // 確定の前は、次のイメージを受けない (戻り先の版の先頭を消さない)
    fill_flash();
    ota_command("B 8192 0");
    CHECK(reply_is("ota error trial") && s_flash[PART_A] == 0x5a && s_flash[PART_B] == 0x5a);
    ota_task(true);
    ota_task(true);
    CHECK(s_buyCalls == 1 && reply_is("ota bought rc=0 tbyb=00"));
    // ふつうの起動では確定を呼ばない
    s_bootTypeNow = 1;
    ota_init();
    ota_task(true);
    CHECK(s_buyCalls == 1);
    // ---- 続けて更新する: ふつうの起動 → 更新 → 確定 → ふつうの起動 → 更新 → 確定 → ... ----
    // 書く先は A と B を行き来する。B に書くとき、ブート ROM は 1 (B の番号) を返す (これをエラーと取っていた)
    s_running = 0;
    boot_normal();
    CHECK(ota_wd_ok());
    for (int round = 0; round < 6; round++) {
        int from = round % 2;
        update_once(image, 20000 + (uint32_t)round * 4097, from);
        if (round % 3 != 2) { // ふつうの起動を挟む回と、確定したまま続ける回の両方
            boot_normal();
            CHECK(s_running == !from && ota_wd_ok());
            ota_task(true);
        }
    }
    // 確定しなかった版は、ふつうの起動で前の版に戻る。そのあとも更新できる (A からも B からも)
    for (int from = 0; from < 2; from++) {
        s_running = from;
        boot_normal();
        int buys = s_buyCalls;
        CHECK(send(image, 30000, crc32_update(0, image, 30000), UINT32_MAX));
        boot_update();
        ota_task(false);
        CHECK(!ota_wd_ok());
        ota_command("B 8192 0");
        CHECK(reply_is("ota error trial"));
        boot_normal(); // 16.7 秒のウォッチドッグ
        CHECK(s_running == from && s_buyCalls == buys && ota_wd_ok());
        update_once(image, 40000, from);
    }
    // 確定に失敗したら、ウォッチドッグも掛けず、次のイメージも受けない (ブート ROM が再起動して前の版に戻すのを待つ)
    boot_normal();
    CHECK(send(image, 30000, crc32_update(0, image, 30000), UINT32_MAX));
    boot_update();
    s_buyErr = -12;
    ota_task(true);
    s_buyErr = 0;
    CHECK(!ota_wd_ok());
    ota_command("B 8192 0");
    CHECK(reply_is("ota error trial"));
    // パーティション表が読まれていない起動 (PRECONDITION_NOT_MET) なら、読ませてから聞き直す
    boot_normal();
    s_ptLoaded = false;
    s_loadCalls = 0;
    ota_command("B 8192 0");
    CHECK(reply_is("ota begin") && s_loadCalls == 1);
    ota_command("A");
    // 書く先がない (負のエラー、パーティションなしの 0xff) なら、番号を付けて断る
    s_targetErr = BOOTROM_ERROR_NOT_FOUND;
    ota_command("B 8192 0");
    CHECK(reply_is("ota error no-partition -17"));
    s_targetErr = PARTITION_TABLE_NO_PARTITION_INDEX;
    ota_command("B 8192 0");
    CHECK(reply_is("ota error no-partition 255"));
    s_targetErr = 0;
    if (s_fail) { printf("%d failed\n", s_fail); return 1; }
    printf("ok\n");
    return 0;
}
