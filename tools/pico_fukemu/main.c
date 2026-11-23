// main.c -- ふろっぴU君 (FUK) の偽物の Pico 2 のファーム。USB (CDC 2 本) とフラッシュのイメージを core.c につなぐ
//
// CDC 0 をポート 1、CDC 1 をポート 2 として答える (pix68k は 0x00 を送って 'f' / 'F' で見分ける)。
// イメージと設定はフラッシュの決まった場所 (layout.h) に mkimg.py の UF2 で書く。書き込みもフラッシュに行う。
// 無線 LAN のビルド (pico2_w、PICO_FUKEMU_WIFI_FILE) では、pico_kbd の net.c で Wi-Fi につなぎ、mDNS と ping に答え、
// TCP で状態を返す (命令は app_run)。無線 LAN はコア 1 だけで回し、コア 0 (USB と FDC) が無線 LAN の処理を待たないようにする。
// コア 0 とコア 1 の間は、状態の写し (s_snap_*)、記録のリング (s_nl_*)、フラッシュの書き込みの頼み (s_fw_*) だけでやりとりする。
// フラッシュを書くのは無線 LAN のビルドではコア 1 だけ (コア 0 の書き込みもコア 1 に頼む)。書くあいだはコア 0 を止める
// (flash_safe_execute とマルチコアのロックアウト。止める側をコア 1 に決めておけば、両方から止め合うことがない)。
// OTA のビルド (PICO_FUKEMU_OTA) では、pico_kbd の ota.c (O 命令) でファームを、netwrite.c (W 命令) で設定とイメージを書ける。
//
// 設定とイメージは、フラッシュの物理的な場所 (layout.h) を、アドレスの変換を通らない窓 (XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE)
// から読む。A/B のパーティションから起動すると、ブート ROM が XIP_BASE をそのパーティションに付け替えるため。
#include <stdio.h>
#include <string.h>

#include "core.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"
#include "layout.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "tusb.h"
#if PICO_KBD_WIFI
#include "../pico_kbd/app.h"
#include "hardware/watchdog.h"
#include "netwrite.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#endif
#ifndef PICO_FUKEMU_VERSION
#define PICO_FUKEMU_VERSION "unknown"
#endif

#define P1 0
#define P2 1

static uint64_t __time_critical_func(now_us)(void) { return time_us_64(); }

static void out1(uint8_t b)
{
    tud_cdc_n_write_char(P1, b);
    tud_cdc_n_write_flush(P1);
}

// ポート 2 へ送る。送るバッファに入った分だけ受け付けて、すぐに出す (待たない。残りは core が送り直す)
static size_t __time_critical_func(out2)(const uint8_t *p, size_t n)
{
    // 空いている分だけ書く (TinyUSB は DTR が 0 の間、一杯の FIFO を上書きするので、write の戻り値に頼らない)
    uint32_t room = tud_cdc_n_write_available(P2);
    if (n > room) n = room;
    uint32_t k = n ? tud_cdc_n_write(P2, p, (uint32_t)n) : 0;
    tud_cdc_n_write_flush(P2);
    return k;
}

// フラッシュの物理的な場所 off を読む (キャッシュもアドレスの変換も通らない。書いた直後に読んでも新しい中身が見える)
static const uint8_t *flash_at(uint32_t off) { return (const uint8_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + off); }

// 4KB を消して書く (flash_range_* の場所はフラッシュの物理的な場所)
struct flash_job {
    uint32_t off;
    const uint8_t *data; // NULL なら消すだけ
};
static void erase_program(void *arg)
{
    const struct flash_job *j = arg;
    flash_range_erase(j->off, FLASH_SECTOR_SIZE);
    if (j->data) flash_range_program(j->off, j->data, FLASH_SECTOR_SIZE);
}

#if PICO_KBD_WIFI
// コア 1 で呼ぶ。コア 0 を止めてから書く
static bool flash_job_run(uint32_t off, const uint8_t *data)
{
    struct flash_job j = {off, data};
    return flash_safe_execute(erase_program, &j, 1000) == PICO_OK;
}
// コア 0 の書き込みをコア 1 に頼む
static struct flash_job s_fw_job;
static volatile uint32_t s_fw_req, s_fw_ack;
static volatile bool s_fw_ok;
static void fw_serve(void) // コア 1
{
    uint32_t r = s_fw_req;
    if (r == s_fw_ack) return;
    __dmb();
    s_fw_ok = flash_job_run(s_fw_job.off, s_fw_job.data);
    __dmb();
    s_fw_ack = r;
}
#endif

// フラッシュの 4KB の消去単位ごとに、読んで、直して、消して、書く
static uint8_t sector[FLASH_SECTOR_SIZE];
// sector[] をフラッシュの s に書く (コア 0)
static bool flash_write_sector(uint32_t s)
{
#if PICO_KBD_WIFI
    // コア 1 に頼んで待つ。コア 1 が 5 秒答えなければ失敗 (リザルトは異常終了)。
    // 時間切れの頼みは取り消せないので、それが終わるまでは次を頼まず、sector[] も変えない (write_image が先に見る)
    if (s_fw_ack != s_fw_req) return false;
    uint32_t r = s_fw_req + 1;
    s_fw_job.off = s, s_fw_job.data = sector;
    __dmb();
    s_fw_req = r;
    uint64_t limit = time_us_64() + 5000000;
    while (s_fw_ack != r)
        if (time_us_64() > limit) return false;
    __dmb();
    return s_fw_ok;
#else
    struct flash_job j = {s, sector};
    uint32_t irq = save_and_disable_interrupts();
    erase_program(&j);
    restore_interrupts(irq);
    return true;
#endif
}
static int write_image(int unit, uint32_t off, const uint8_t *p, size_t n)
{
    uint32_t base = FUKEMU_SLOT_OFF(unit) + FUKEMU_SLOT_HDR;
#if PICO_KBD_WIFI
    if (s_fw_ack != s_fw_req) return -1; // 前の頼みが時間切れのまま残っている (sector[] をコア 1 がまだ使うかもしれない)
#endif
    while (n) {
        uint32_t a = base + off, s = a & ~(FLASH_SECTOR_SIZE - 1), o = a - s;
        uint32_t k = FLASH_SECTOR_SIZE - o;
        if (k > n) k = (uint32_t)n;
        memcpy(sector, flash_at(s), FLASH_SECTOR_SIZE);
        if (memcmp(sector + o, p, k) != 0) {
            memcpy(sector + o, p, k);
            if (!flash_write_sector(s)) return -1;
            if (memcmp(flash_at(s), sector, FLASH_SECTOR_SIZE) != 0) return -1; // 読み直して違えば失敗 (リザルトは異常終了)
        }
        off += k, p += k, n -= k;
    }
    return 0;
}

// どちらかのポートを 1200bps で開くと BOOTSEL (UF2 の書き込み) に入る
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *coding)
{
    (void)itf;
    if (coding->bit_rate == 1200) reset_usb_boot(0, 0);
}

// 動作の記録 (RAM に溜める)。pix68k を閉じてからポート 2 に 'L' を送ると、溜めた記録を送って消す (最後に 0x00)
#if PICO_KBD_WIFI
// 記録をコア 0 からコア 1 に渡すリング (書くのはコア 0 だけ、読むのはコア 1 だけ)。あふれた分は捨てる
static char s_nl_buf[4096];
static volatile uint32_t s_nl_head, s_nl_tail;
static void nl_put(char c)
{
    uint32_t h = s_nl_head;
    if (h - s_nl_tail >= sizeof s_nl_buf) return;
    s_nl_buf[h % sizeof s_nl_buf] = c;
    __dmb();
    s_nl_head = h + 1;
}
#endif
static char logbuf[64 * 1024];
static uint32_t log_head, log_len;
static void log_put(char c)
{
    logbuf[(log_head + log_len) % sizeof logbuf] = c;
    if (log_len < sizeof logbuf) log_len++;
    else log_head = (log_head + 1) % sizeof logbuf;
}
static void log_line(const char *s)
{
    char t[24];
    snprintf(t, sizeof t, "%llu ", (unsigned long long)time_us_64());
    for (const char *q = t; *q; q++) log_put(*q);
    for (const char *q = s; *q; q++) log_put(*q);
    log_put('\n');
#if PICO_KBD_WIFI
    // コア 1 に渡し、認証を通った TCP の相手にも流す (相手がいなければ net.c が捨てる)
    for (const char *q = t; *q; q++) nl_put(*q);
    for (const char *q = s; *q; q++) nl_put(*q);
    nl_put('\n');
#endif
}
// 記録を送り切るまで USB を回す (調べるときだけ使う)。1 秒進まなければやめる
static void dump_all(const uint8_t *p, size_t n)
{
    uint64_t limit = time_us_64() + 1000000;
    while (n && time_us_64() < limit && tud_mounted()) {
        size_t k = out2(p, n);
        p += k, n -= k;
        if (k) limit = time_us_64() + 1000000;
        tud_task();
    }
}
static void dump_log(void)
{
    while (log_len) {
        uint32_t k = sizeof logbuf - log_head;
        if (k > log_len) k = log_len;
        dump_all((const uint8_t *)logbuf + log_head, k);
        log_head = (log_head + k) % sizeof logbuf;
        log_len -= k;
    }
    dump_all((const uint8_t *)"", 1);
}

static const fukemu_io_t io = {now_us, out1, out2, write_image, log_line, dump_log};

// 状態の問い合わせに使う (起動のときに決まる)
static fukemu_config_t s_cfg;
static bool s_cfg_flash, s_img[FUKEMU_UNITS], s_wp[FUKEMU_UNITS];
static uint64_t s_last_rx_us[2]; // ポート 1 と 2 で最後にバイトを受けた時刻 (0 ならまだ)

#if PICO_KBD_WIFI
// 状態の写し: コア 1 が s_snap_req を増やし、コア 0 が主ループで写してから s_snap_ack を合わせる。
// TinyUSB の状態もコア 0 で読む (TinyUSB はコア 0 だけが触る)
typedef struct {
    fukemu_stats_t st;
    uint64_t rx_us[2];
    uint64_t at_us;
    bool mounted, suspended, dtr[2];
} snap_t;
static snap_t s_snap;
static volatile uint32_t s_snap_req, s_snap_ack;
static void snap_serve(void) // コア 0
{
    uint32_t r = s_snap_req;
    if (r == s_snap_ack) return;
    fukemu_get_stats(&s_snap.st);
    s_snap.rx_us[0] = s_last_rx_us[0], s_snap.rx_us[1] = s_last_rx_us[1];
    s_snap.mounted = tud_mounted(), s_snap.suspended = tud_suspended();
    s_snap.dtr[0] = tud_cdc_n_connected(P1), s_snap.dtr[1] = tud_cdc_n_connected(P2);
    s_snap.at_us = time_us_64();
    __dmb();
    s_snap_ack = r;
}
static bool snap_take(snap_t *out) // コア 1。コア 0 が 0.2 秒答えなければ false
{
    uint32_t r = s_snap_req + 1;
    s_snap_req = r;
    uint64_t limit = time_us_64() + 200000;
    while (s_snap_ack != r) {
        fw_serve(); // コア 0 が書き込みを頼んで待っているときに、止め合わないように
        if (time_us_64() > limit) return false;
    }
    __dmb();
    *out = s_snap;
    return true;
}

// ドライブを抜く頼み: コア 1 が s_ej_unit と s_ej_req を書き、コア 0 がコマンドの合間に fukemu_eject してから s_ej_ack を合わせる
static volatile int s_ej_unit;
static volatile uint32_t s_ej_req, s_ej_ack;
static volatile bool s_ej_ok;
static void eject_serve(void) // コア 0
{
    uint32_t r = s_ej_req;
    if (r == s_ej_ack) return;
    __dmb();
    int u = s_ej_unit;
    if (!fukemu_eject(u)) return; // コマンドの途中。次の周回でもう一度
    s_img[u] = false;
    log_line(u ? "fd1 ejected (network write)" : "fd0 ejected (network write)");
    s_ej_ok = true;
    __dmb();
    s_ej_ack = r;
}
#if PICO_FUKEMU_OTA
static bool eject_take(int unit) // コア 1。コア 0 が 2 秒で抜けなければ false
{
    s_ej_unit = unit;
    s_ej_ok = false;
    __dmb();
    uint32_t r = s_ej_req + 1;
    s_ej_req = r;
    uint64_t limit = time_us_64() + 2000000;
    while (s_ej_ack != r) {
        fw_serve();
        if (time_us_64() > limit) {
            // 取り消す (コア 0 がこのあと抜くことはある。抜けていれば、それでも困らない)
            s_ej_ack = r;
            return false;
        }
    }
    __dmb();
    return s_ej_ok;
}
#endif

bool app_reply(const char *s)
{
    net_reply(s);
    return true;
}
void app_flush(void) { net_flush(500); }

static unsigned long long ms_of(uint64_t us) { return (unsigned long long)(us / 1000); }

static void status(void) // コア 1
{
    static const char *const kDel[] = {"stream", "head", "end"};
    static const char *const kRule[] = {"none", "pair", "single", "all"};
    char buf[200];
    snap_t sn;
    bool have = snap_take(&sn);
    uint64_t now = have ? sn.at_us : time_us_64();
    snprintf(buf, sizeof buf, "fukemu version=%s up-ms=%llu", PICO_FUKEMU_VERSION, ms_of(now));
    app_reply(buf);
    snprintf(buf, sizeof buf, "config source=%s delivery=%s rule=%s one-sector=%d stretches=%d",
             s_cfg_flash ? "flash" : "default", s_cfg.delivery < 3 ? kDel[s_cfg.delivery] : "?",
             s_cfg.rule < 4 ? kRule[s_cfg.rule] : "?", s_cfg.one_sector, s_cfg.nstretch);
    app_reply(buf);
    for (int i = 0; i < s_cfg.nstretch; i++) { // 1 件ずつ (16 件でも切れないように)
        const fukemu_stretch_t *t = &s_cfg.stretch[i];
        snprintf(buf, sizeof buf, "stretch U%d C%d H%d R%d ppm=%ld", t->unit, t->c, t->h, t->r, (long)t->ppm);
        app_reply(buf);
    }
    snprintf(buf, sizeof buf, "image fd0=%s fd1=%s", s_img[0] ? (s_wp[0] ? "present-wp" : "present") : "empty",
             s_img[1] ? (s_wp[1] ? "present-wp" : "present") : "empty");
    app_reply(buf);
    if (!have) { // コア 0 が止まっている (書き込み中、'L' で記録を送っている途中など)
        app_reply("usb busy");
        return;
    }
    // USB: mounted はホストが設定まで済ませたか、dtr は USB が使える状態でホストが DTR を立てているか (ポートを開くと立つ)。
    // rx は最後にバイトを受けてからの時間
    int n = snprintf(buf, sizeof buf, "usb mounted=%d suspended=%d dtr1=%d dtr2=%d", sn.mounted, sn.suspended,
                     sn.dtr[0], sn.dtr[1]);
    for (int p = 0; p < 2; p++) {
        if (sn.rx_us[p])
            n += snprintf(buf + n, sizeof buf - n, " rx%d-ago-ms=%llu", p + 1, ms_of(now - sn.rx_us[p]));
        else
            n += snprintf(buf + n, sizeof buf - n, " rx%d=none", p + 1);
    }
    app_reply(buf);
    const fukemu_stats_t *st = &sn.st;
    snprintf(buf, sizeof buf, "count reads=%lu rt-reads=%lu writes=%lu resets=%lu", (unsigned long)st->reads,
             (unsigned long)st->rt_reads, (unsigned long)st->writes, (unsigned long)st->resets);
    app_reply(buf);
    if (!st->reads) {
        app_reply("last-read none");
        return;
    }
    // at-ms は起動からの時刻、result-us はコマンドを受けてからリザルトを送る予定まで、queued-us は送るバッファに渡し終えるまで
    n = snprintf(buf, sizeof buf, "last-read U%d C%d H%d R%d-%d st0=%02x rt=%d at-ms=%llu ago-ms=%llu result-us=%llu",
                 st->last_unit, st->last_c, st->last_h, st->last_r, st->last_eot, st->last_st0, st->last_rt,
                 ms_of(st->last_cmd_us), ms_of(now - st->last_cmd_us),
                 (unsigned long long)(st->last_due_us - st->last_cmd_us));
    if (st->last_done_us)
        snprintf(buf + n, sizeof buf - n, " queued-us=%llu", (unsigned long long)(st->last_done_us - st->last_cmd_us));
    else
        snprintf(buf + n, sizeof buf - n, " queued-us=%s", st->last_aborted ? "aborted" : "pending");
    app_reply(buf);
}

#if PICO_FUKEMU_OTA
// W 命令 (netwrite.c) のフラッシュの読み書き (コア 1)
static uint8_t s_nw_buf[FLASH_SECTOR_SIZE];
static bool nw_write(uint32_t off, const uint8_t *p)
{
    memcpy(s_nw_buf, p, FLASH_SECTOR_SIZE);
    return flash_job_run(off, s_nw_buf) && memcmp(flash_at(off), s_nw_buf, FLASH_SECTOR_SIZE) == 0;
}
static bool nw_erase(uint32_t off) { return flash_job_run(off, NULL); }
static void nw_reply(const char *s) { app_reply(s); }
static const nw_io_t s_nw_io = {nw_write, nw_erase, flash_at, nw_reply, eject_take};

// ota.c のフラッシュの書き込み (rom_flash_op) と確定 (rom_explicit_buy) は、SDK の中で flash_safe_execute を通るので、
// コア 1 から呼べば、そのあいだコア 0 は止まる
#endif

static void reboot(void)
{
    app_reply("reboot");
    app_flush();
    watchdog_reboot(0, 0, 10);
    for (;;) tight_loop_contents();
}

// コア 1 のスタック (8KB)。lwIP、mDNS、cyw43、snprintf、W 命令の 336 バイトの作業域、ブート ROM の呼び出しを載せる。
// SDK の既定 (SCRATCH_X の 2KB) では足りないので、普通の RAM に取る (CMake で PICO_CORE1_STACK_SIZE=0 にして
// SCRATCH_X には取らない。コア 0 のスタック (SCRATCH_Y) があふれても、すぐ下の SCRATCH_X は空いている)
static uint32_t __attribute__((aligned(16))) s_core1_stack[0x2000 / sizeof(uint32_t)];

// コア 1: 無線 LAN だけを回す
static void core1_main(void)
{
#if PICO_FUKEMU_OTA
    ota_init();
    nw_init(&s_nw_io);
    bool bought = false;
#endif
    net_init();
    for (;;) {
        fw_serve();
        uint32_t t = s_nl_tail;
        while (t != s_nl_head) {
            __dmb();
            net_log_byte(s_nl_buf[t % sizeof s_nl_buf]);
            t++;
            __dmb();
            s_nl_tail = t;
        }
        net_task();
#if PICO_FUKEMU_OTA
        // 新しい版を試しているときは、トークンを通った相手が来たら確定する (1 回だけ)
        if (!bought && net_client_authed()) {
            bought = true;
            ota_task(true);
        }
#endif
    }
}

void app_run(const char *line)
{
    while (*line == ' ') line++;
    char buf[200];
    switch (line[0]) {
    case 'S':
        status();
        app_reply("ok");
        break;
    case 'N':
        net_status(buf, sizeof buf);
        app_reply(buf);
        break;
#if PICO_FUKEMU_OTA
    case 'O':
        ota_run(line[1] == ' ' ? line + 2 : line + 1, net_client_authed());
        break;
    case 'W':
        nw_command(line[1] == ' ' ? line + 2 : line + 1);
        break;
#endif
    case 'R':
        reboot();
        break;
    case 'H':
        app_reply("S: 状態 (版、設定、イメージ、USB、数、最後の Read)  N: 無線 LAN の状態  R: 再起動  H: この一覧");
#if PICO_FUKEMU_OTA
        app_reply("O: ファームの更新 (pico_kbd/picocmd.py --ota)  W: 設定とイメージを書く (fukemu_net.py)");
#endif
        break;
    case '\0':
        break;
    default:
        app_reply("error unknown-command (H で一覧)");
        break;
    }
}
#endif

int main(void)
{
    fukemu_config_t cfg = {0};
    const fukemu_flash_cfg_t *fc = (const fukemu_flash_cfg_t *)flash_at(FUKEMU_CFG_OFF);
    // 見出しと値の範囲が合うときだけ使う (書いている途中で電源が切れた設定を、なるべく使わない)
    bool cfg_ok = memcmp(fc->magic, FUKEMU_CFG_MAGIC, 8) == 0 && fc->delivery <= FUKEMU_END && fc->rule <= FUKEMU_RT_ALL &&
                  fc->nstretch <= FUKEMU_MAX_STRETCH && fc->one_sector <= 1;
    if (cfg_ok) {
        cfg.delivery = fc->delivery;
        cfg.rule = fc->rule;
        cfg.one_sector = fc->one_sector == 1;
        cfg.nstretch = fc->nstretch <= FUKEMU_MAX_STRETCH ? fc->nstretch : FUKEMU_MAX_STRETCH;
        for (int i = 0; i < cfg.nstretch; i++) {
            cfg.stretch[i].unit = fc->stretch[i].unit;
            cfg.stretch[i].c = fc->stretch[i].c;
            cfg.stretch[i].h = fc->stretch[i].h;
            cfg.stretch[i].r = fc->stretch[i].r;
            cfg.stretch[i].ppm = fc->stretch[i].ppm;
        }
    } else {
        fukemu_default_config(&cfg);
    }
    const uint8_t *images[FUKEMU_UNITS];
    bool wp[FUKEMU_UNITS];
    for (int u = 0; u < FUKEMU_UNITS; u++) {
        const fukemu_flash_img_t *h = (const fukemu_flash_img_t *)flash_at(FUKEMU_SLOT_OFF(u));
        bool ok = memcmp(h->magic, FUKEMU_IMG_MAGIC, 8) == 0 && h->size == FUKEMU_IMAGE_BYTES;
        images[u] = ok ? flash_at(FUKEMU_SLOT_OFF(u) + FUKEMU_SLOT_HDR) : NULL;
        wp[u] = ok && (h->flags & 1);
        s_img[u] = ok, s_wp[u] = wp[u];
    }
    s_cfg = cfg;
    s_cfg_flash = cfg_ok;

    tusb_init();
    fukemu_init(&io, &cfg, images, wp);
#if PICO_KBD_WIFI
    // コア 1 がフラッシュを書くときに、コア 0 を止められるようにしてから、コア 1 を始める (s_cfg なども決めてから)
    flash_safe_execute_core_init();
    multicore_launch_core1_with_stack(core1_main, s_core1_stack, sizeof s_core1_stack);
#endif
    log_line(images[0] ? "fd0 present" : "fd0 empty");
    log_line(images[1] ? "fd1 present" : "fd1 empty");
    for (;;) {
        tud_task();
        uint8_t b[64];
        while (tud_cdc_n_available(P1)) {
            uint32_t k = tud_cdc_n_read(P1, b, sizeof b);
            if (k) s_last_rx_us[0] = time_us_64();
            for (uint32_t i = 0; i < k; i++) fukemu_port1_rx(b[i]);
        }
        while (tud_cdc_n_available(P2)) {
            uint32_t k = tud_cdc_n_read(P2, b, sizeof b);
            if (k) s_last_rx_us[1] = time_us_64();
            for (uint32_t i = 0; i < k; i++) fukemu_port2_rx(b[i]);
        }
        fukemu_poll();
#if PICO_KBD_WIFI
        snap_serve();
        eject_serve();
#endif
    }
}
