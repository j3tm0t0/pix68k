// ota.c の RP2040 の部分: 受けたイメージを STAGING に書き、ブートローダ (boot2040/) に入れ替えを頼む (otaboot.h を参照)。
// 入れ替えたあとの起動 (TRIAL) では、ブートローダが掛けたウォッチドッグを主ループで延ばし続ける。
// 確定 (トークンを通った相手が来た) したら記録を NONE に戻す。ウォッチドッグは止めず、main.c が延ばし続ける。
// 60 秒たっても確定しなければ、再起動する (ブートローダが前の版に戻す)。固まったときは、ウォッチドッグが再起動する。
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "hardware/flash.h"
#include "hardware/structs/watchdog.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "ota_backend.h"
#include "otaboot.h"
#include "pico/stdlib.h"

#define TRIAL_MS 60000

static OtabMeta s_meta;
static bool s_trial;
static absolute_time_t s_trialEnd;

static const uint8_t* f_read(uint32_t offset) { return (const uint8_t*)(XIP_BASE + offset); }
static void f_erase(uint32_t offset) {
    uint32_t irq = save_and_disable_interrupts(); // フラッシュを書く間は、フラッシュの上の割り込みの処理を動かせない
    flash_range_erase(offset, OTAB_SECTOR);
    restore_interrupts(irq);
}
static void f_program(uint32_t offset, const uint8_t* data) {
    uint32_t irq = save_and_disable_interrupts();
    flash_range_program(offset, data, OTAB_SECTOR);
    restore_interrupts(irq);
}
static const OtabFlash kFlash = {f_read, f_erase, f_program};

// 書く先は STAGING だけ
static bool in_staging(uint32_t offset) { return offset >= OTAB_STAGING && offset < OTAB_STAGING + OTAB_REGION; }

bool otab_erase_sector(uint32_t offset) {
    if (!in_staging(offset)) return false;
    f_erase(offset);
    return true;
}

bool otab_write_sector(uint32_t offset, uint8_t* data) {
    if (!in_staging(offset)) return false;
    f_erase(offset);
    f_program(offset, data);
    return memcmp(f_read(offset), data, OTAB_SECTOR) == 0;
}

const char* otab_target(uint32_t* start, uint32_t* size) {
    if (s_trial) return "trial"; // 試している版を確定してから、次を受ける
    *start = OTAB_STAGING;
    *size = OTAB_REGION;
    return NULL;
}

// 受けたイメージが MAIN で動くかは、ブートローダが入れ替える前に確かめる (otaboot.c)
const char* otab_check_first(const uint8_t* first, bool force) {
    (void)first;
    (void)force;
    return NULL;
}

bool otab_commit(uint32_t start, uint32_t size, uint32_t crc) {
    (void)start;
    OtabMeta m = s_meta;
    m.state = OTAB_PENDING;
    m.size = size;
    m.crc = crc;
    otab_meta_write(&kFlash, &m);
    otab_meta_read(&kFlash, &s_meta);
    return s_meta.state == OTAB_PENDING && s_meta.size == size && s_meta.crc == crc;
}

void otab_reboot(uint32_t start) {
    (void)start;
    watchdog_reboot(0, 0, 100);
    for (;;) tight_loop_contents();
}

void otab_status(char* buf, size_t size) {
    // state と result は otaboot.h の OtabState と OtabResult (result: 1 = 確定した、2 = 前の版に戻した、3 = 受けたイメージが壊れていた、4 = 今の版を BACKUP に写せなかった)
    snprintf(buf, size, "state=%lu result=%lu trial=%d", (unsigned long)s_meta.state, (unsigned long)s_meta.result, s_trial);
}

void otab_init(void) {
    otab_meta_read(&kFlash, &s_meta);
    s_trial = s_meta.state == OTAB_TRIAL;
    if (s_trial) {
        watchdog_enable(8000, true); // ブートローダが掛けたものを掛け直す (watchdog_update() が延ばす長さを決めるため)
        s_trialEnd = make_timeout_time_ms(TRIAL_MS);
    }
}

// 常時のウォッチドッグは、いつ掛けてもよい (試している間は、ブートローダが掛けたものと同じ 8 秒)
bool otab_wd_ok(void) { return true; }

void otab_task(bool ready) {
    if (!s_trial) return;
    if (ready) {
        s_meta.state = OTAB_NONE;
        s_meta.result = OTAB_R_CONFIRMED;
        otab_meta_write(&kFlash, &s_meta);
        otab_meta_read(&kFlash, &s_meta);
        if (s_meta.state == OTAB_NONE) {
            // ウォッチドッグは止めない (確定のあとも、固まったら再起動するように main.c が延ばし続ける)
            s_trial = false;
            app_reply("ota confirmed");
            return;
        }
    }
    if (time_reached(s_trialEnd)) {
        app_reply("ota not-confirmed reboot");
        app_flush();
        watchdog_reboot(0, 0, 10);
        for (;;) tight_loop_contents();
    }
    watchdog_update();
}
