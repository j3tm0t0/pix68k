// ホスト版の RS-232C (SCC のチャネル A) の相手。rs232c= / --rs232c の値で選ぶ (platform::parseRs232c()):
//   tcp:PORT       127.0.0.1 の PORT で待ち受ける
//   tcp:ADDR:PORT  ADDR の PORT で待ち受ける (0.0.0.0 ならすべて。ほかの機械から届くので、信用できる網だけで使う)
// つないできた相手 1 つとバイトをそのままやり取りする (telnet の取り決めはしない。nc や socat でつなぐ)。
// つないでいる間は DCD と CTS を出す。つないでいる間に別の相手が来たら、すぐ切る。
// 受け取りと送りは別のスレッドで行い、platform::RingSerialLink で X68000 のスレッドに渡す。
#pragma once

#include <stddef.h>

#include "x68k/serial_link.h"

// spec は rs232c= の値。開けなければ nullptr を返し、err に理由を入れる。返した口は delete で閉じる
// (スレッドを止めてから消す)。spec が空か off なら nullptr を返し、err は空にする
x68k::SerialLink* openHostSerialLink(const char* spec, char* err, size_t errSize);
// addr (空なら 127.0.0.1) の port で待ち受ける。port が 0 なら OS が空いている番号を選び、boundPort に返す (試験用)
x68k::SerialLink* openHostSerialLinkAt(const char* addr, unsigned port, unsigned* boundPort, char* err, size_t errSize);
