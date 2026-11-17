// test_ota_host.c が ota.c をホストでビルドするための、ブート ROM の代わり。中身は test_ota_host.c にある
#ifndef HOST_STUB_PICO_BOOTROM_H
#define HOST_STUB_PICO_BOOTROM_H
#include <stdbool.h>
#include <stdint.h>

#define RP2350_ARM_S_FAMILY_ID 0xe48bff59u
#define PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB 0
#define PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS 0x00001fffu
#define PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB 13
#define PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS 0x03ffe000u
#define REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE 0x4
#define REBOOT2_FLAG_NO_RETURN_ON_SUCCESS 0x100
#define BOOT_TYPE_FLASH_UPDATE 4
#define BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING 0x1
#define CFLASH_OP_LSB 16
#define CFLASH_OP_VALUE_ERASE 0
#define CFLASH_OP_VALUE_PROGRAM 1
#define CFLASH_OP_VALUE_READ 2
#define CFLASH_SECLEVEL_LSB 8
#define CFLASH_SECLEVEL_VALUE_SECURE 1
#define CFLASH_ASPACE_LSB 0
#define CFLASH_ASPACE_VALUE_STORAGE 0

typedef struct cflash_flags { uint32_t flags; } cflash_flags_t;
typedef struct { uint32_t permissions_and_location, permissions_and_flags; } resident_partition_t;
typedef struct {
    int8_t diagnostic_partition_index;
    uint8_t boot_type;
    int8_t partition;
    uint8_t tbyb_and_update_info;
    uint32_t boot_diagnostic;
    uint32_t reboot_params[2];
} boot_info_t;

#define BOOTROM_ERROR_PRECONDITION_NOT_MET (-14)
#define BOOTROM_ERROR_NOT_FOUND (-17)
#define PARTITION_TABLE_NO_PARTITION_INDEX 0xff
int rom_load_partition_table(uint8_t* workarea, uint32_t size, bool force_reload);
int rom_flash_op(cflash_flags_t flags, uintptr_t addr, uint32_t size_bytes, uint8_t* buf);
int rom_get_uf2_target_partition(uint8_t* workarea, uint32_t size, uint32_t family, resident_partition_t* out);
intptr_t rom_flash_runtime_to_storage_addr(uintptr_t addr);
void rom_flash_flush_cache(void);
int rom_reboot(uint32_t flags, uint32_t delay_ms, uint32_t p0, uint32_t p1);
int rom_explicit_buy(uint8_t* buffer, uint32_t size);
bool rom_get_boot_info(boot_info_t* info);
int rom_get_last_boot_type(void);
#endif
