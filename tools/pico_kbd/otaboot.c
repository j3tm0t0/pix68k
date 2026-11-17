// RP2040 の無線 LAN 越しの更新: 記録の読み書きと、ブートローダの写し (otaboot.h を参照)
#include "otaboot.h"
#include <stddef.h>
#include <string.h>
#include "core.h"

#define OTAB_MAGIC 0x4b42444fu // "ODBK"

static bool meta_ok(const OtabMeta* m) {
    return m->magic == OTAB_MAGIC && m->check == crc32_update(0, (const uint8_t*)m, offsetof(OtabMeta, check));
}

// 正しい記録のうち新しいほうの場所を返す (なければ 0)
static uint32_t meta_latest(const OtabFlash* f, OtabMeta* out) {
    OtabMeta a, b;
    memcpy(&a, f->read(OTAB_META0), sizeof a);
    memcpy(&b, f->read(OTAB_META1), sizeof b);
    bool okA = meta_ok(&a), okB = meta_ok(&b);
    if (okA && (!okB || (int32_t)(a.seq - b.seq) > 0)) {
        *out = a;
        return OTAB_META0;
    }
    if (okB) {
        *out = b;
        return OTAB_META1;
    }
    memset(out, 0, sizeof *out);
    return 0;
}

void otab_meta_read(const OtabFlash* f, OtabMeta* m) { meta_latest(f, m); }

void otab_meta_write(const OtabFlash* f, const OtabMeta* m) {
    static uint8_t sector[OTAB_SECTOR];
    OtabMeta old, next = *m;
    uint32_t at = meta_latest(f, &old);
    next.magic = OTAB_MAGIC;
    next.seq = at ? old.seq + 1 : 1;
    next.check = crc32_update(0, (const uint8_t*)&next, offsetof(OtabMeta, check));
    memset(sector, 0xff, sizeof sector);
    memcpy(sector, &next, sizeof next);
    // 今の記録でないほうに書く。途中で電源が切れても、今の記録は残る
    uint32_t target = at == OTAB_META0 ? OTAB_META1 : OTAB_META0;
    f->erase(target);
    f->program(target, sector);
}

static bool vectors_valid(const OtabFlash* f, uint32_t at);

static bool all_ff(const uint8_t* p) {
    for (uint32_t i = 0; i < OTAB_SECTOR; i++)
        if (p[i] != 0xff) return false;
    return true;
}

#define TRIES 3 // 消去と書き込みは、読み返して合わなければここまでやり直す

// src から dst に len バイト (4KB 単位に切り上げ) を写す。もう同じ中身の 4KB は触らない。
// 書いた 4KB は読み返して確かめ、合わなければ消す所からやり直す。最後に全体を比べ直し、すべて同じなら true。
// false のとき、dst の中身は当てにできない (src は変えていない)
static bool copy_region(const OtabFlash* f, uint32_t src, uint32_t dst, uint32_t len) {
    static uint8_t sector[OTAB_SECTOR];
    for (uint32_t off = 0; off < len; off += OTAB_SECTOR) {
        for (int i = 0; i < TRIES && memcmp(f->read(src + off), f->read(dst + off), OTAB_SECTOR) != 0; i++) {
            memcpy(sector, f->read(src + off), OTAB_SECTOR); // フラッシュを書いている間は、フラッシュを読めない
            f->erase(dst + off);
            if (!all_ff(sector)) f->program(dst + off, sector);
        }
    }
    for (uint32_t off = 0; off < len; off += OTAB_SECTOR)
        if (memcmp(f->read(src + off), f->read(dst + off), OTAB_SECTOR) != 0) return false;
    return true;
}

static bool image_ok(const OtabFlash* f, uint32_t at, uint32_t size, uint32_t crc) {
    if (size < OTAB_SECTOR || size > OTAB_REGION) return false;
    uint32_t c = 0;
    for (uint32_t off = 0; off < size; off += OTAB_SECTOR)
        c = crc32_update(c, f->read(at + off), size - off < OTAB_SECTOR ? size - off : OTAB_SECTOR);
    return c == crc;
}

// 記録を書き、読み返して、書いたとおりに入ったときだけ true。次の段に進むかどうかは、フラッシュに入った記録で決める
// (記録が入っていないのに先へ進むと、次の起動で前の段をやり直して、写しの元を壊す)
static bool commit(const OtabFlash* f, const OtabMeta* m) {
    for (int i = 0; i < TRIES; i++) {
        OtabMeta got;
        otab_meta_write(f, m);
        otab_meta_read(f, &got);
        if (got.state == m->state && got.result == m->result && got.size == m->size && got.crc == m->crc) return true;
    }
    return false;
}

OtabBoot otab_boot(const OtabFlash* f) {
    OtabMeta m;
    otab_meta_read(f, &m);
    if (m.state == OTAB_PENDING) {
        // ここでは MAIN に触らないので、どこで失敗しても MAIN (今の版) を起動してよい
        // CRC が合っていても、MAIN で動くイメージでなければ入れ替えない (ほかの基板用のイメージを送ってしまったときなど)
        if (!image_ok(f, OTAB_STAGING, m.size, m.crc) || !vectors_valid(f, OTAB_STAGING)) {
            m.state = OTAB_NONE;
            m.result = OTAB_R_BAD_IMAGE;
            commit(f, &m);
            return OTAB_BOOT_MAIN;
        }
        // BACKUP が MAIN と同じになったのを確かめてからでなければ、MAIN を書き換える段 (COPYING) に進まない
        if (!copy_region(f, OTAB_MAIN, OTAB_BACKUP, OTAB_REGION)) {
            m.state = OTAB_NONE;
            m.result = OTAB_R_BACKUP_FAILED;
            commit(f, &m);
            return OTAB_BOOT_MAIN;
        }
        m.state = OTAB_COPYING;
        if (!commit(f, &m)) return OTAB_BOOT_MAIN; // 記録は PENDING のまま。次の起動でやり直す
    }
    if (m.state == OTAB_COPYING) {
        if (copy_region(f, OTAB_STAGING, OTAB_MAIN, m.size) && image_ok(f, OTAB_MAIN, m.size, m.crc)) {
            m.state = OTAB_TRIAL;
            // TRIAL と記録できなければ起動しない (試している版として扱えず、だめな版でも戻せなくなる)
            return commit(f, &m) ? OTAB_BOOT_TRIAL : OTAB_BOOT_STUCK;
        }
        m.state = OTAB_REVERTING; // 書けていない。前の版に戻す
        if (!commit(f, &m)) return OTAB_BOOT_STUCK;
    } else if (m.state == OTAB_TRIAL) { // 確定されないまま、もう一度起動した
        m.state = OTAB_REVERTING;
        if (!commit(f, &m)) return OTAB_BOOT_STUCK;
    }
    if (m.state == OTAB_REVERTING) {
        // BACKUP が起動できるイメージに見えなければ、MAIN を消さない
        if (!vectors_valid(f, OTAB_BACKUP)) return OTAB_BOOT_STUCK;
        // MAIN が BACKUP と同じになったのを確かめてからでなければ、戻し終えた (NONE) と記録しない
        if (!copy_region(f, OTAB_BACKUP, OTAB_MAIN, OTAB_REGION)) return OTAB_BOOT_STUCK;
        m.state = OTAB_NONE;
        m.result = OTAB_R_REVERTED;
        commit(f, &m); // 記録できなくても、MAIN は前の版にそろっている。次の起動で、何も写さずに記録だけやり直す
    }
    return OTAB_BOOT_MAIN;
}

bool otab_main_valid(const OtabFlash* f) { return vectors_valid(f, OTAB_MAIN); }

// at に、MAIN で動くようにリンクしたイメージが入っているように見えるか
static bool vectors_valid(const OtabFlash* f, uint32_t at) {
    uint32_t v[2];
    memcpy(v, f->read(at) + 256, sizeof v);
    // スタックは RAM (0x20000000 から 0x20042000)、入り口は MAIN の中 (Thumb なので奇数)
    return v[0] > 0x20000000u && v[0] <= 0x20042000u && (v[1] & 1) && v[1] >= 0x10000000u + OTAB_MAIN &&
           v[1] < 0x10000000u + OTAB_MAIN + OTAB_REGION;
}
