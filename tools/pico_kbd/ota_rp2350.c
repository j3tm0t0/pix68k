// ota.c の RP2350 の部分: ブート ROM の A/B パーティションと TBYB を使う (仕組みは ota.c の先頭を参照)
#include "pico/stdlib.h" // boot/ のヘッダより先に読む
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "boot/picobin.h"
#include "boot/picoboot.h"
#include "boot/uf2.h"
#include "core.h"
#include "ota_backend.h"
#include "pico/bootrom.h"

static __attribute__((aligned(4))) uint8_t s_work[OTA_SECTOR]; // ブート ROM の作業用と、書いたあとの読み返し

static boot_info_t s_boot;
static int s_bootType;
static bool s_buyDone;
static int s_buyRc = 1; // 確定 (rom_explicit_buy) の戻り値。まだ呼んでいなければ 1

// 今動いている版が確定済みか (確定の要らない起動か、確定に成功したあと)
static bool settled(void) { return s_buyDone && (s_buyRc == 0 || s_buyRc == 1); }

static int flash_op(uint32_t op, uint32_t offset, uint8_t* buf) {
    cflash_flags_t flags = {(op << CFLASH_OP_LSB) | (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
                            (CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB)};
    return rom_flash_op(flags, XIP_BASE + offset, OTA_SECTOR, buf);
}

bool otab_erase_sector(uint32_t offset) { return flash_op(CFLASH_OP_VALUE_ERASE, offset, NULL) == 0; }

bool otab_write_sector(uint32_t offset, uint8_t* data) {
    if (flash_op(CFLASH_OP_VALUE_ERASE, offset, NULL) != 0) return false;
    if (flash_op(CFLASH_OP_VALUE_PROGRAM, offset, data) != 0) return false;
    if (flash_op(CFLASH_OP_VALUE_READ, offset, s_work) != 0) return false;
    return memcmp(s_work, data, OTA_SECTOR) == 0;
}

const char* otab_target(uint32_t* start, uint32_t* size) {
    // UF2 を写したときにブート ROM が選ぶのと同じパーティション (動いていない側) を聞く
    // 戻り値は、選んだパーティションの番号 (0 以上。A なら 0、B なら 1) か、負のエラー。0 でなくても成功である
    // (B で動いているときは A = 0 が返り、A で動いているときは B = 1 が返る)
    static char why[32];
    // 試している (まだ確定していない) 版は、次のイメージを受けない。受けると、書く先 = 戻り先の版の先頭を消すので、
    // そのあと確定できなかったときに起動できる版がなくなる
    if (!settled()) return "trial";
    resident_partition_t part;
    rom_flash_flush_cache();
    int rc = rom_get_uf2_target_partition(s_work, sizeof s_work, RP2350_ARM_S_FAMILY_ID, &part);
    if (rc == BOOTROM_ERROR_PRECONDITION_NOT_MET) {
        // パーティション表がまだ読まれていない起動のしかただった。読ませてから聞き直す
        rom_load_partition_table(s_work, sizeof s_work, false);
        rc = rom_get_uf2_target_partition(s_work, sizeof s_work, RP2350_ARM_S_FAMILY_ID, &part);
    }
    if (rc < 0 || rc == PARTITION_TABLE_NO_PARTITION_INDEX) {
        snprintf(why, sizeof why, "no-partition %d", rc);
        return why;
    }
    uint32_t first = (part.permissions_and_location & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >>
                     PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
    uint32_t last = (part.permissions_and_location & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >>
                    PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
    *start = first * OTA_SECTOR;
    *size = (last + 1 - first) * OTA_SECTOR;
    // 動いているイメージに重なる先には書かない (パーティション表がおかしいとき)
    intptr_t self = rom_flash_runtime_to_storage_addr(XIP_BASE);
    if (self >= 0 && (uint32_t)self >= XIP_BASE + *start && (uint32_t)self < XIP_BASE + *start + *size) return "target-is-running";
    return NULL;
}

// ブート ROM は、書いたイメージの中の IMAGE_DEF (版の番号と、確定待ちかどうか) を見て起動するので、記録するものはない
bool otab_commit(uint32_t start, uint32_t size, uint32_t crc) {
    (void)start;
    (void)size;
    (void)crc;
    return true;
}

void otab_reboot(uint32_t start) {
    // フラッシュを更新したことをブート ROM に伝えて再起動する。書いたパーティションを 1 回だけ優先して起動する
    rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, 100, XIP_BASE + start, 0);
    for (;;) tight_loop_contents();
}

void otab_status(char* buf, size_t size) {
    // part は起動したパーティション (0 = A、1 = B、-1 = パーティション表なし)、tbyb と diag はブート ROM の起動の記録
    // (boot_info_t の tbyb_and_update_info と boot_diagnostic)、buy-rc は確定の結果 (呼んでいなければ 1)
    snprintf(buf, size, "part=%d boot-type=%d tbyb=%02x diag=%08lx buy-rc=%d", s_boot.partition, s_bootType,
             s_boot.tbyb_and_update_info, (unsigned long)s_boot.boot_diagnostic, s_buyRc);
}

void otab_init(void) {
    memset(&s_boot, 0, sizeof s_boot);
    s_boot.partition = -1;
    rom_get_boot_info(&s_boot);
    s_bootType = rom_get_last_boot_type();
    s_buyRc = 1;
    // 確定が要るのは、フラッシュの更新のあとの起動だけ
    s_buyDone = !(s_bootType == BOOT_TYPE_FLASH_UPDATE || (s_boot.tbyb_and_update_info & BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING));
}

// 常時のウォッチドッグを掛けてよいか。確定の前は、ブート ROM が掛けた 16.7 秒のウォッチドッグ (確定しなければ前の版に戻すためのもの)
// に触らない。確定に失敗したときも触らず、ブート ROM のウォッチドッグに再起動させる
bool otab_wd_ok(void) { return settled(); }

// 確定を待たない (TBYB の付いていない) イメージは、無線 LAN につながらない版でも前の版に戻らないので、既定では受けない
const char* otab_check_first(const uint8_t* first, bool force) {
    if (force) return NULL;
    int tbyb = picobin_tbyb(first, OTA_SECTOR);
    return tbyb < 0 ? "no-image-def" : tbyb == 0 ? "not-tbyb" : NULL;
}

void otab_task(bool ready) {
    if (s_buyDone || !ready) return;
    s_buyDone = true;
    s_buyRc = rom_explicit_buy(s_work, sizeof s_work);
    rom_get_boot_info(&s_boot);
    char buf[64];
    snprintf(buf, sizeof buf, "ota bought rc=%d tbyb=%02x", s_buyRc, s_boot.tbyb_and_update_info);
    app_reply(buf);
}
