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

// ---- G 命令 (ゲームパッド) の引数 ----
#define PAD_MAX_MS 10000
#define PAD_HAT_MAX 8 // 0 は離した状態、1 が上で、時計回りに 8 (左上) まで (TinyUSB の GAMEPAD_HAT_*)
// 空ならすべて離す (buttons 0、hat 0、ms 0)。それ以外は "ボタン (16 進、32 ビット) [ハット 0-8 [ミリ秒]]"。
// ミリ秒を書かなければ押したままにする (ms は 0)。1 から PAD_MAX_MS なら、その時間だけ押して離す。書き方が違えば
// (明示した 0 も) false
bool pad_parse(const char* args, uint32_t* buttons, uint8_t* hat, unsigned* ms);

// ---- ゲームパッドのレポートを送る順番 ----
// 押している時間 (G の ms) は、押したレポートを受け手が読み終えた時刻から数える (前のレポートが読まれるのを待つ間に
// 時間が過ぎて、離したレポートしか送られないことがないように)。時刻はマイクロ秒
typedef struct {
    uint32_t buttons;
    uint8_t hat;
    bool dirty;          // 送っていない状態がある
    bool inFlight;       // 送ったレポートを受け手がまだ読んでいない
    unsigned holdMs;     // 押したレポートが読まれたら、この時間のあとに離す (0 なら離さない)
    bool holdArmed;      // 送ったレポート (inFlight) が、holdMs を数え始めるもの
    bool releasePending; // releaseAt に離す
    uint64_t releaseAt;
} PadState;

void pad_init(PadState* s);   // すべて離した状態を、次に送れるときに 1 回送る
void pad_set(PadState* s, uint32_t buttons, uint8_t hat, unsigned ms);
// 次に送るレポートがあれば true (buttons と hat に入れる)。離す時刻が来ていれば、ここで離した状態にする
bool pad_next(PadState* s, uint64_t now_us, uint32_t* buttons, uint8_t* hat);
void pad_sent(PadState* s);                       // pad_next() のレポートを送れた (受け付けられた)
void pad_complete(PadState* s, uint64_t now_us);  // 受け手が読み終えた
void pad_failed(PadState* s);                     // 送れなかった。同じ状態をもう一度送る

// ---- C 命令 (USB の抜き差しの真似) の引数 ----
#define USBC_MIN_MS 100   // これより短く切り離すと、試験機が抜かれたことに気づかないことがある
#define USBC_MAX_MS 60000
typedef enum { USBC_OFF, USBC_ON, USBC_CYCLE } UsbcAction;
typedef enum { USBC_SAME, USBC_KBD, USBC_KBD_PAD } UsbcMode; // つなぐときの装置 (SAME は今のまま)
// "off"、"on [k|kg]"、"ミリ秒 [k|kg]" (切り離して、ミリ秒 (USBC_MIN_MS から USBC_MAX_MS) のあとにつなぎ直す)。書き方が違えば false
bool usbc_parse(const char* args, UsbcAction* action, unsigned* ms, UsbcMode* mode);

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
