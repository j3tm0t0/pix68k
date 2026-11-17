// main.c と net.c の間の呼び出し
#ifndef PICO_KBD_APP_H
#define PICO_KBD_APP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---- main.c が持つ ----
bool app_reply(const char* s);     // "#pico s" を UART と TCP の相手に返す。UART の側に積めなければ false
void app_run(const char* line);    // 命令の 1 行を実行する

void app_flush(void);              // 再起動の前に、たまっている応答を UART と TCP の相手に送り切る

// ---- ota.c が持つ (cmake に PICO_KBD_OTA を付けたビルドだけ) ----
void ota_init(void);
void ota_task(bool net_ready);     // 主ループから毎回呼ぶ。net_ready は、新しい版を確定してよい状態か
void ota_command(const char* args); // O 命令
void ota_run(const char* args, bool net_ready); // 確定してよい状態なら先に確定してから、O 命令を実行する
bool ota_wd_ok(void);               // 常時のウォッチドッグを掛けてよい状態か (新しい版を確定する前は false)

// ---- net.c が持つ (無線 LAN のビルドだけ) ----
void net_init(void);               // 無線のチップを初期化する (数秒かかるので、USB を始める前に呼ぶ)
void net_task(void);               // 主ループから毎回呼ぶ
void net_reply(const char* s);     // 認証を通った相手がいれば "#pico s" を送る
void net_log_byte(char c);         // 試験機のログの 1 バイト (主ループから)
bool net_tx_idle(void);            // 相手に送るものが残っていない (相手がいないときも true)
void net_flush(unsigned ms);       // 再起動の前に、送るものを送り出して (最大 ms ミリ秒待つ) 相手を切る
void net_status(char* buf, size_t size); // N 命令の応答
bool net_is_up(void);              // IP アドレスを取れている
bool net_client_authed(void);      // トークンを通った相手がつないでいる
uint32_t net_down_ms(void);        // IP アドレスを取れていない状態が続いているミリ秒 (取れていれば 0)

#endif
