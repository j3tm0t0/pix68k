// RP2040 (Pico W) の無線 LAN 越しの更新で、ブートローダ (boot2040/) とファームウェア (ota_rp2040.c) が共に使う部分。
// ハードウェアに依存しないので、ホストでも試せる (test_otaboot_host.c)。
//
// RP2040 のブート ROM には A/B の仕組みがないので、フラッシュの先頭に小さなブートローダを置く。
//
//   0x000000  ブートローダ (64KB。boot2 を含む。更新しない)
//   0x010000  MAIN     動かすファームウェア (640KB。この番地で動くようにリンクする)
//   0x0b0000  STAGING  O 命令で受けた新しいイメージ (640KB)
//   0x150000  BACKUP   入れ替える前の MAIN の写し (640KB)
//   0x1f0000  記録     4KB が 2 つ (交互に書く。書いている途中で電源が切れても、前の記録が残る)
//
// 進み方 (記録の state):
//   NONE      MAIN を起動する
//   PENDING   ファームウェアが STAGING に受け終えた。ブートローダが CRC とベクタ表を確かめ、MAIN を BACKUP に写して COPYING へ
//             (合わなければ MAIN に触らず NONE へ。result = BAD_IMAGE。BACKUP に写せなければ result = BACKUP_FAILED)
//   COPYING   STAGING を MAIN に写して TRIAL へ
//   TRIAL     新しい MAIN を試している。ファームウェアが確定する (NONE に戻す) 前にもう一度ブートローダが動いたら REVERTING へ
//   REVERTING BACKUP を MAIN に写して NONE へ (result = REVERTED)
// どの写しも、途中で電源が切れたら同じ所からやり直せる (写す元は、写し終えるまで変えない)。
// 写しは 4KB ごとに読み返して確かめ、合わなければやり直す。記録も読み返す。次の段に進むのは、写しが元と同じになり、
// その段の記録がフラッシュに入ったのを確かめたあとだけ (BACKUP が MAIN と同じになる前に MAIN を消さない。
// MAIN が BACKUP と同じになる前に「戻し終えた」と記録しない)。
// ブートローダは、TRIAL で起動するときにウォッチドッグを掛ける。新しいファームウェアが固まったら、再起動して前の版に戻る。
#ifndef PICO_KBD_OTABOOT_H
#define PICO_KBD_OTABOOT_H
#include <stdbool.h>
#include <stdint.h>

#define OTAB_SECTOR 4096u
#define OTAB_MAIN 0x010000u
#define OTAB_STAGING 0x0b0000u
#define OTAB_BACKUP 0x150000u
#define OTAB_REGION 0x0a0000u // MAIN / STAGING / BACKUP の大きさ
#define OTAB_META0 0x1f0000u
#define OTAB_META1 0x1f1000u

typedef enum { OTAB_NONE = 0, OTAB_PENDING, OTAB_COPYING, OTAB_TRIAL, OTAB_REVERTING } OtabState;
typedef enum { OTAB_R_NONE = 0, OTAB_R_CONFIRMED, OTAB_R_REVERTED, OTAB_R_BAD_IMAGE, OTAB_R_BACKUP_FAILED } OtabResult;
// otab_boot の結果。STUCK は、写しか記録の書き込みが読み返しと合わず、MAIN を起動してはいけない状態
typedef enum { OTAB_BOOT_MAIN = 0, OTAB_BOOT_TRIAL, OTAB_BOOT_STUCK } OtabBoot;

typedef struct {
    uint32_t magic;
    uint32_t seq;    // 書くたびに増やす。2 つの記録のうち、正しくて seq の大きいほうを使う
    uint32_t state;  // OtabState
    uint32_t result; // OtabResult (最後の更新の結果)
    uint32_t size;   // STAGING のイメージのバイト数
    uint32_t crc;    // その CRC-32
    uint32_t check;  // ここまでの CRC-32
} OtabMeta;

// フラッシュの操作 (offset はフラッシュの先頭からのバイト数。どれも 4KB 単位)
typedef struct {
    const uint8_t* (*read)(uint32_t offset);               // その場所を読むためのポインタ
    void (*erase)(uint32_t offset);                        // 4KB を消す
    void (*program)(uint32_t offset, const uint8_t* data); // 消してある 4KB に書く
} OtabFlash;

// 記録を読む。正しい記録がなければ、すべて 0 (state = NONE)
void otab_meta_read(const OtabFlash* f, OtabMeta* m);
// 記録を書く (seq と check は中で決める)
void otab_meta_write(const OtabFlash* f, const OtabMeta* m);

// ブートローダの仕事: 記録を見て、要る写しを済ませる。
//   OTAB_BOOT_MAIN   MAIN を起動する
//   OTAB_BOOT_TRIAL  MAIN を「試している版」として起動する (呼んだ側がウォッチドッグを掛ける)
//   OTAB_BOOT_STUCK  MAIN を起動しない。記録は途中の段のままなので、もう一度呼べば同じ所からやり直す
OtabBoot otab_boot(const OtabFlash* f);

// MAIN にイメージが入っているように見えるか (boot2 の 256 バイトのあとのベクタ表の、スタックと入り口の番地を見る)
bool otab_main_valid(const OtabFlash* f);

#endif
