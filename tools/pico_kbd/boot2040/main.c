// RP2040 (Pico W) の無線 LAN 越しの更新のためのブートローダ。フラッシュの先頭 (64KB) に置く。
// 記録を見て要る写しを済ませ (../otaboot.c)、MAIN のファームウェアに飛ぶ。無線も USB も使わない。
// MAIN に何も入っていなければ、USB の書き込みモード (BOOTSEL) に入る。
#include <string.h>
#include "hardware/flash.h"
#include "hardware/structs/scb.h"
#include "hardware/sync.h"
#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "otaboot.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"

static const uint8_t* f_read(uint32_t offset) { return (const uint8_t*)(XIP_BASE + offset); }
static void f_erase(uint32_t offset) {
    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(offset, OTAB_SECTOR);
    restore_interrupts(irq);
}
static void f_program(uint32_t offset, const uint8_t* data) {
    uint32_t irq = save_and_disable_interrupts();
    flash_range_program(offset, data, OTAB_SECTOR);
    restore_interrupts(irq);
}
static const OtabFlash kFlash = {f_read, f_erase, f_program};

// MAIN のファームウェアは、先頭に (使わない) boot2 の 256 バイトがあり、そのあとにベクタ表がある
static void __attribute__((noreturn)) jump_to_main(void) {
    const uint32_t* vectors = (const uint32_t*)(XIP_BASE + OTAB_MAIN + 0x100);
    (void)save_and_disable_interrupts();
    *(io_rw_32*)(PPB_BASE + M0PLUS_NVIC_ICER_OFFSET) = 0xffffffffu; // 割り込みをすべて止め、たまっている分を消す
    *(io_rw_32*)(PPB_BASE + M0PLUS_NVIC_ICPR_OFFSET) = 0xffffffffu;
    *(io_rw_32*)(PPB_BASE + M0PLUS_SYST_CSR_OFFSET) = 0;
    scb_hw->vtor = (uint32_t)vectors;
    uint32_t sp = vectors[0], pc = vectors[1];
    // 割り込みは、ファームウェアの起動の処理が自分の用意を済ませてから受けられるように、ここで戻しておく
    // (リセットの直後と同じ状態にする)
    __asm volatile("msr msp, %0\n cpsie i\n bx %1\n" : : "r"(sp), "r"(pc));
    __builtin_unreachable();
}

int main(void) {
    OtabBoot how = otab_boot(&kFlash);
    // 入れ替えた MAIN が起動できそうにないなら、もう一度呼んで前の版に戻す (記録が TRIAL なので、戻す動きになる)
    if (how == OTAB_BOOT_TRIAL && !otab_main_valid(&kFlash)) how = otab_boot(&kFlash);
    // 写しか記録が読み返しと合わなかったら、同じ所から何度かやり直す
    for (int i = 0; i < 3 && how == OTAB_BOOT_STUCK; i++) how = otab_boot(&kFlash);
    // それでもだめなとき (MAIN の中身を当てにできない) と、MAIN に何も入っていないときは、書き込みモードに入る
    if (how == OTAB_BOOT_STUCK || !otab_main_valid(&kFlash)) {
        reset_usb_boot(0, 0);
        for (;;) tight_loop_contents();
    }
    // 試している版が固まったら、再起動して前の版に戻す。ファームウェアは主ループでこれを延ばし続ける
    if (how == OTAB_BOOT_TRIAL) {
        // watchdog_enable() は「ウォッチドッグの時間切れで再起動したか」を見分ける値を scratch[4] に書く。
        // この起動が時間切れによるものでなければ消しておく (ファームウェアが、更新のあとの起動を時間切れと取り違えないように)。
        // pico-sdk の中身に合わせている (2.3.1 の hardware_watchdog/watchdog.c で確かめた): watchdog_enable() が
        // scratch[4] に 0x6ab73121 を書き、watchdog_enable_caused_reboot() がその値を見る。SDK を上げたら、ここを見直す
        bool timedOut = watchdog_enable_caused_reboot();
        watchdog_enable(8000, true);
        if (!timedOut) watchdog_hw->scratch[4] = 0;
    }
    jump_to_main();
}
