// otaboot.c (RP2040 のブートローダの写しと記録) を、メモリの上のフラッシュで試す:
//   cc -std=c11 -Wall -Wextra -Werror -o test_otaboot_host test_otaboot_host.c otaboot.c core.c && ./test_otaboot_host
// フラッシュの操作のたびに「そこで電源が切れた」ことにして起動し直し、MAIN がいつも
// 前の版か新しい版のどちらかにそろうこと、試して確定しなければ前の版に戻ることを確かめる。
// 同じく、操作のたびに「消去や書き込みが、何も言わずに失敗した」ことにして (その 1 回だけ / その 4KB がずっと)、
//   - BACKUP が元の MAIN と同じになる前に、MAIN を消したり書いたりしない
//   - MAIN が BACKUP と同じになる前に、「戻し終えた」と記録しない
//   - MAIN を起動するのは、MAIN が前の版か新しい版と 1 バイトも違わないときだけ
// を確かめる。
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core.h"
#include "otaboot.h"

static int s_fail;
#define CHECK(x) do { if (!(x)) { printf("NG %s:%d: %s\n", __FILE__, __LINE__, #x); s_fail++; } } while (0)

static uint8_t s_flash[0x200000];
static uint8_t s_oldRegion[OTAB_REGION]; // 更新の前の MAIN の全体
static long s_ops, s_cutAt = -1; // s_cutAt 回目の操作の途中で電源が切れる
static jmp_buf s_powerLost;
// 何も言わない失敗: s_badAt 回目の操作が失敗する。s_badSticky なら、その 4KB はそのあともずっと失敗する
static long s_badAt = -1;
static bool s_badSticky;
static uint32_t s_badSector; // ずっと失敗する 4KB (0 ならなし)
static bool s_backupKnownGood;

static bool in_region(uint32_t off, uint32_t base) { return off >= base && off < base + OTAB_REGION; }
static bool region_eq(uint32_t a, uint32_t b) { return memcmp(s_flash + a, s_flash + b, OTAB_REGION) == 0; }

// 操作の前に呼ぶ。true なら、この操作は失敗させる
static bool before_op(uint32_t off, const uint8_t* d) {
    CHECK(off % OTAB_SECTOR == 0 && off >= OTAB_MAIN); // ブートローダ自身には触らない
    CHECK(!in_region(off, OTAB_STAGING));               // ブートローダは STAGING を書かない
    if (in_region(off, OTAB_BACKUP)) s_backupKnownGood = false;
    if (in_region(off, OTAB_MAIN)) {
        // MAIN に触るのは、BACKUP が元の MAIN と同じになったあとだけ
        if (!s_backupKnownGood) {
            CHECK(memcmp(s_flash + OTAB_BACKUP, s_oldRegion, OTAB_REGION) == 0);
            s_backupKnownGood = true;
        }
    }
    if (d != NULL && off >= OTAB_META0) {
        // 「戻し終えた」と記録するのは、MAIN が BACKUP と同じになったあとだけ
        OtabMeta m;
        memcpy(&m, d, sizeof m);
        if (m.state == OTAB_NONE && m.result == OTAB_R_REVERTED) CHECK(region_eq(OTAB_MAIN, OTAB_BACKUP));
    }
    bool bad = s_ops == s_badAt || (s_badSector != 0 && off == s_badSector);
    if (s_ops == s_badAt && s_badSticky) s_badSector = off;
    return bad;
}
static const uint8_t* f_read(uint32_t off) { return s_flash + off; }
static void f_erase(uint32_t off) {
    bool bad = before_op(off, NULL);
    if (s_ops++ == s_cutAt) { // 半分だけ消えた
        memset(s_flash + off, 0xff, OTAB_SECTOR / 2);
        longjmp(s_powerLost, 1);
    }
    if (bad) return; // 消えなかった
    memset(s_flash + off, 0xff, OTAB_SECTOR);
}
static void f_program(uint32_t off, const uint8_t* d) {
    bool bad = before_op(off, d);
    uint32_t n = OTAB_SECTOR;
    bool cut = s_ops++ == s_cutAt;
    if (cut) n /= 2; // 半分だけ書けた
    for (uint32_t i = 0; i < n; i++) s_flash[off + i] &= bad && i % 7 == 3 ? (uint8_t)(d[i] & 0x5a) : d[i]; // 失敗なら、ところどころ化ける
    if (cut) longjmp(s_powerLost, 1);
}
static const OtabFlash kFlash = {f_read, f_erase, f_program};

static uint8_t s_old[300000], s_new[345678];

static void make_image(uint8_t* img, uint32_t size, uint8_t seed) {
    for (uint32_t i = 0; i < size; i++) img[i] = (uint8_t)(i * 13 + seed + (i >> 9));
    uint32_t v[2] = {0x20042000u, 0x10000000u + OTAB_MAIN + 0x201};
    memcpy(img + 256, v, sizeof v);
}

static bool main_is(const uint8_t* img, uint32_t size) { return memcmp(s_flash + OTAB_MAIN, img, size) == 0; }

// 古い版が MAIN で動いていて、新しい版を STAGING に受け終え、PENDING を記録した状態を作る
static void setup(uint32_t crc) {
    memset(s_flash, 0xff, sizeof s_flash);
    memset(s_flash, 0x11, OTAB_MAIN); // ブートローダ
    memcpy(s_flash + OTAB_MAIN, s_old, sizeof s_old);
    memset(s_flash + OTAB_BACKUP, 0x22, 8192); // 前の更新の残り
    memcpy(s_flash + OTAB_STAGING, s_new, sizeof s_new);
    memcpy(s_oldRegion, s_flash + OTAB_MAIN, OTAB_REGION);
    s_cutAt = -1;
    s_badAt = -1;
    s_badSticky = false;
    s_badSector = 0;
    s_backupKnownGood = false;
    OtabMeta m = {0};
    m.state = OTAB_PENDING;
    m.size = sizeof s_new;
    m.crc = crc;
    otab_meta_write(&kFlash, &m);
}

static bool main_is_old(void) { return memcmp(s_flash + OTAB_MAIN, s_oldRegion, OTAB_REGION) == 0; }

// 何も言わない失敗を badAt 回目の操作に入れて、1 回起動する。起動してよいと答えたなら、MAIN は前の版か新しい版にそろっている
static OtabBoot boot_with_fault(long badAt, bool sticky) {
    s_ops = 0;
    s_cutAt = -1;
    s_badAt = badAt;
    s_badSticky = sticky;
    OtabBoot how = otab_boot(&kFlash);
    s_badAt = -1;
    // 起動してよいと答えたなら、記録もそのとおりにフラッシュに入っている
    // (TRIAL と記録できていないのに試している版として起動すると、だめな版でも戻せない)
    OtabMeta got;
    otab_meta_read(&kFlash, &got);
    if (how == OTAB_BOOT_TRIAL) CHECK(main_is(s_new, sizeof s_new) && got.state == OTAB_TRIAL);
    if (how == OTAB_BOOT_MAIN) {
        CHECK(main_is_old() || main_is(s_new, sizeof s_new));
        CHECK(got.state != OTAB_TRIAL && got.state != OTAB_COPYING);
        if (!main_is_old()) CHECK(got.state == OTAB_NONE && got.result == OTAB_R_CONFIRMED);
    }
    return how;
}

// 電源が切れなくなるまで起動し直す。最後の otab_boot の戻り値 (試している版として起動するか) を返す
static bool boot_until_done(long cutAt) {
    volatile long cut = cutAt;
    volatile int boots = 0;
    for (;;) {
        s_ops = 0;
        s_cutAt = cut;
        if (setjmp(s_powerLost) == 0) {
            OtabBoot how = otab_boot(&kFlash);
            s_cutAt = -1;
            CHECK(how != OTAB_BOOT_STUCK);
            return how == OTAB_BOOT_TRIAL;
        }
        // 電源が切れた。次は切れない
        cut = -1;
        boots = boots + 1;
        CHECK(boots < 3);
    }
}

int main(void) {
    make_image(s_old, sizeof s_old, 1);
    make_image(s_new, sizeof s_new, 77);
    uint32_t crc = crc32_update(0, s_new, sizeof s_new);
    OtabMeta m;

    // 記録がなければ何もしない
    memset(s_flash, 0xff, sizeof s_flash);
    memcpy(s_flash + OTAB_MAIN, s_old, sizeof s_old);
    s_ops = 0;
    CHECK(otab_boot(&kFlash) == OTAB_BOOT_MAIN && s_ops == 0 && main_is(s_old, sizeof s_old) && otab_main_valid(&kFlash));
    memset(s_flash + OTAB_MAIN, 0xff, 4096);
    CHECK(!otab_main_valid(&kFlash));

    // ふつうの更新: 新しい版を試し、確定する
    setup(crc);
    CHECK(boot_until_done(-1));
    long opsUpdate = s_ops;
    CHECK(main_is(s_new, sizeof s_new) && memcmp(s_flash + OTAB_BACKUP, s_old, sizeof s_old) == 0);
    otab_meta_read(&kFlash, &m);
    CHECK(m.state == OTAB_TRIAL);
    m.state = OTAB_NONE; // ファームウェアが確定する
    m.result = OTAB_R_CONFIRMED;
    otab_meta_write(&kFlash, &m);
    CHECK(!boot_until_done(-1) && main_is(s_new, sizeof s_new));
    otab_meta_read(&kFlash, &m);
    CHECK(m.state == OTAB_NONE && m.result == OTAB_R_CONFIRMED);

    // 確定しないまま起動し直すと、前の版に戻る
    setup(crc);
    CHECK(boot_until_done(-1));
    CHECK(!boot_until_done(-1));
    long opsRevert = s_ops;
    CHECK(main_is(s_old, sizeof s_old) && otab_main_valid(&kFlash));
    for (uint32_t i = sizeof s_old; i < OTAB_REGION; i++) if (s_flash[OTAB_MAIN + i] != 0xff) { CHECK(!"MAIN の残りが戻っていない"); break; }
    otab_meta_read(&kFlash, &m);
    CHECK(m.state == OTAB_NONE && m.result == OTAB_R_REVERTED);
    CHECK(!boot_until_done(-1) && s_ops == 0); // そのあとは何もしない

    // STAGING の CRC が違えば、MAIN に触らない
    setup(crc ^ 1);
    CHECK(!boot_until_done(-1) && main_is(s_old, sizeof s_old));
    otab_meta_read(&kFlash, &m);
    CHECK(m.state == OTAB_NONE && m.result == OTAB_R_BAD_IMAGE);

    // CRC は合うが、MAIN で動くイメージでない (ほかの基板用や、番地の違うイメージを送ってしまった): MAIN に触らない
    {
        uint32_t wrong[2] = {0x20082000u, 0x10000201u}; // RP2350 のスタックと、0x10000000 でリンクした入り口
        memcpy(s_new + 256, wrong, sizeof wrong);
        setup(crc32_update(0, s_new, sizeof s_new));
        CHECK(!boot_until_done(-1) && main_is(s_old, sizeof s_old));
        otab_meta_read(&kFlash, &m);
        CHECK(m.state == OTAB_NONE && m.result == OTAB_R_BAD_IMAGE);
        make_image(s_new, sizeof s_new, 77);
    }

    // 更新の途中の、どの操作で電源が切れても、起動し直せば新しい版を試す所まで進む
    // (最初の記録の書き込みが切れたときだけは、更新がなかったことになり、前の版のまま)
    printf("更新の操作 %ld 回、戻す操作 %ld 回のそれぞれで電源を切る\n", opsUpdate, opsRevert);
    for (long cut = 0; cut < opsUpdate; cut++) {
        setup(crc);
        bool trial = boot_until_done(cut);
        if (trial) CHECK(main_is(s_new, sizeof s_new));
        else CHECK(main_is(s_old, sizeof s_old));
        otab_meta_read(&kFlash, &m);
        CHECK(trial ? m.state == OTAB_TRIAL : m.state == OTAB_NONE);
        if (!trial) continue;
        // そのあと確定しなければ、前の版に戻る
        CHECK(!boot_until_done(-1) && main_is(s_old, sizeof s_old));
    }
    // 戻す途中のどの操作で電源が切れても、起動し直せば前の版に戻る
    for (long cut = 0; cut < opsRevert; cut++) {
        setup(crc);
        CHECK(boot_until_done(-1));
        CHECK(!boot_until_done(cut));
        CHECK(main_is(s_old, sizeof s_old));
        otab_meta_read(&kFlash, &m);
        CHECK(m.state == OTAB_NONE);
        if (m.state != OTAB_NONE) break;
    }
    // ---- 何も言わない失敗 ----
    // その 1 回だけの失敗: 読み返して気づき、やり直すので、失敗がないときと同じ結果になる
    for (long bad = 0; bad < opsUpdate; bad++) {
        setup(crc);
        CHECK(boot_with_fault(bad, false) == OTAB_BOOT_TRIAL);
        otab_meta_read(&kFlash, &m);
        CHECK(m.state == OTAB_TRIAL && memcmp(s_flash + OTAB_BACKUP, s_oldRegion, OTAB_REGION) == 0);
    }
    for (long bad = 0; bad < opsRevert; bad++) {
        setup(crc);
        CHECK(boot_with_fault(-1, false) == OTAB_BOOT_TRIAL);
        CHECK(boot_with_fault(bad, false) == OTAB_BOOT_MAIN && main_is_old());
        otab_meta_read(&kFlash, &m);
        CHECK(m.state == OTAB_NONE && m.result == OTAB_R_REVERTED);
    }
    // その 4KB がずっと失敗する: 起動してよいと答えるのは MAIN がそろっているときだけ (boot_with_fault の中で見る)。
    // 直ったあと (失敗をやめたあと) は、起動し直せば前の版か新しい版にそろい、確定しなければ前の版に戻る
    long stuck = 0, backupFailed = 0;
    for (long bad = 0; bad < opsUpdate; bad++) {
        setup(crc);
        OtabBoot how = boot_with_fault(bad, true);
        uint32_t where = s_badSector;
        otab_meta_read(&kFlash, &m);
        if (how == OTAB_BOOT_STUCK) stuck++;
        if (in_region(where, OTAB_BACKUP)) {
            // BACKUP に写せなかった: MAIN には触らず、今の版を起動する
            CHECK(how == OTAB_BOOT_MAIN && main_is_old() && m.state == OTAB_NONE && m.result == OTAB_R_BACKUP_FAILED);
            backupFailed++;
        }
        if (in_region(where, OTAB_MAIN)) CHECK(how == OTAB_BOOT_STUCK); // MAIN に写せず、戻すこともできない
        // 失敗が続く間は、何度起動し直しても同じ (壊れた MAIN を起動しない)
        for (int i = 0; i < 2; i++) {
            OtabBoot again = boot_with_fault(-1, false);
            if (in_region(where, OTAB_MAIN)) CHECK(again == OTAB_BOOT_STUCK);
        }
        // 直ったあと
        s_badSector = 0;
        how = boot_with_fault(-1, false);
        CHECK(how != OTAB_BOOT_STUCK);
        if (how == OTAB_BOOT_TRIAL) CHECK(boot_with_fault(-1, false) == OTAB_BOOT_MAIN); // 確定しなければ戻る
        CHECK(main_is_old());
        otab_meta_read(&kFlash, &m);
        CHECK(m.state == OTAB_NONE);
    }
    CHECK(stuck > 0 && backupFailed > 0);
    for (long bad = 0; bad < opsRevert; bad++) {
        setup(crc);
        CHECK(boot_with_fault(-1, false) == OTAB_BOOT_TRIAL);
        OtabBoot how = boot_with_fault(bad, true);
        uint32_t where = s_badSector;
        if (in_region(where, OTAB_MAIN)) {
            // MAIN を BACKUP と同じにできない: 起動しない。「戻し終えた」とも記録しない
            otab_meta_read(&kFlash, &m);
            CHECK(how == OTAB_BOOT_STUCK && m.state == OTAB_REVERTING);
        }
        s_badSector = 0;
        how = boot_with_fault(-1, false);
        if (how == OTAB_BOOT_STUCK) how = boot_with_fault(-1, false);
        CHECK(how == OTAB_BOOT_MAIN && main_is_old());
        otab_meta_read(&kFlash, &m);
        CHECK(m.state == OTAB_NONE && m.result == OTAB_R_REVERTED);
    }
    // BACKUP が起動できるイメージでなくなっていたら (写したあとに壊れた)、戻すときに MAIN を消さない
    setup(crc);
    CHECK(boot_with_fault(-1, false) == OTAB_BOOT_TRIAL);
    memset(s_flash + OTAB_BACKUP, 0, OTAB_SECTOR);
    memset(s_oldRegion, 0, OTAB_SECTOR); // (試験の「BACKUP は元の MAIN と同じ」の見張りを、壊したあとの中身に合わせる)
    s_ops = 0;
    CHECK(otab_boot(&kFlash) == OTAB_BOOT_STUCK && main_is(s_new, sizeof s_new));
    if (s_fail) { printf("%d failed\n", s_fail); return 1; }
    printf("ok\n");
    return 0;
}
