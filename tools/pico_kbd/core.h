// pico_kbd の、ハードウェアに依存しない部分 (ホストでも cc でビルドして試せる。test_host.c を参照)
#ifndef PICO_KBD_CORE_H
#define PICO_KBD_CORE_H
#include <stdbool.h>
#include <stdint.h>

// ---- 送るレポートの列 ----
// 1 つの項目は「この 8 バイトを送り、そのあと wait_us 待つ」
typedef struct {
    uint8_t report[8];
    uint32_t wait_us;
} Step;

#define KEYQ_SIZE 1024 // 入るのは KEYQ_SIZE - 1 項目

// 最後の 1 枠は U (すべて離す) のために空けておく。T、K、D は 1 枠を残せないときは入れずに false を返す。
// U は必ず入る (最後の 1 枠が埋まっているなら、そこに入っているのは前の U のレポートなので、入れなくてよい)
void keyq_init(void);
unsigned keyq_free(void);
bool keyq_tap(uint8_t usage, uint8_t mods, unsigned ms);      // 押したままのキーに usage を足して ms ミリ秒押し、離す
bool keyq_down(uint8_t usage, uint8_t mods);                  // 押したままにする。入れられたときだけ押したままのキーを変える
void keyq_up(void);                                           // すべて離す
const Step* keyq_peek(void);                                  // 次に送る項目 (空なら NULL)
void keyq_pop(void);                                          // 送れたときに呼ぶ

// ---- JIS 配列: 文字 → usage と SHIFT ----
bool jis_key(char c, uint8_t* usage, bool* shift);

// ---- J 命令 (Pi 5 の電源ボタン) の引数 → 押すミリ秒 ----
#define PWR_SHORT_MS 300  // 短押し
#define PWR_LONG_MS 5000  // 長押し (強制的に切る。8000 だと Pi 5 は切れたあと、残りの押しで起動し直した)
#define PWR_MAX_MS 15000
// 空か "S" は短押し、"L" は長押し、10 進の数は 1 から PWR_MAX_MS のミリ秒 (前後の空白は読み飛ばす)。それ以外は false
bool pwr_parse(const char* args, unsigned* ms);

// ---- TCP の相手の認証: 最初の行がトークンと同じか ----
// 違いが何文字目にあっても、比べるのにかかる時間が変わらないようにしてある
bool token_eq(const char* line, const char* token);

// ---- 試験機のログを 1 行ずつに切る (TCP の相手には、行の先頭に "> " を付けて送る) ----
#define LOGLINE_MAX 255 // これより長い行は、途中で切って次の行に続ける

typedef struct {
    char buf[LOGLINE_MAX + 1];
    unsigned len;
    bool ready; // 前の呼び出しで 1 行を返した。次のバイトで空にする
} LogLine;

// 1 バイトを足す。true なら buf に NUL で終わる 1 行が入っている (次に呼ぶまで有効)。\r は捨てる
bool logline_feed(LogLine* l, char c);
// 改行が来ないまま残っている分を 1 行として返す (残りがなければ false)
bool logline_flush(LogLine* l);

// ---- 無線 LAN 越しの更新 (O 命令) で使う ----
// base64 (= の詰めもの付き、空白なし) を戻す。戻したバイト数を返す。書き方が違うか cap を超えるなら -1
int base64_decode(const char* s, uint8_t* out, unsigned cap);
// CRC-32 (zlib の crc32 と同じ値)。最初は crc に 0 を渡す
uint32_t crc32_update(uint32_t crc, const uint8_t* data, unsigned len);

// RP2350 のイメージの先頭 (p から n バイト) にある IMAGE_DEF を探し、TBYB (試してから確定) が付いていれば 1、
// 付いていなければ 0、IMAGE_DEF が見つからなければ -1
int picobin_tbyb(const uint8_t* p, unsigned n);

// ---- 命令の 1 行を組み立てる ----
#define LINE_MAX 511 // 改行を除いた 1 行の最大の長さ

typedef enum { LINE_NONE, LINE_READY, LINE_TOO_LONG } LineResult;

typedef struct {
    char buf[LINE_MAX + 1];
    unsigned len;
    bool overflow; // LINE_MAX を超えた。改行まで捨てる
} LineBuf;

// 1 バイトを足す。LINE_READY なら buf に NUL で終わる 1 行が入っている (次に呼ぶまで有効)
LineResult line_feed(LineBuf* lb, char c);

#endif
