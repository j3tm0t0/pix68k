// ホスト版の FUK (x68k/fuk.h。USB でつなぐ実物の FDC と FDD) の口。FUK の 2 本の USB シリアルポートを開く。
// fuk= / --fuk の値 (platform::parseFuk()):
//   on             ポートを名前で探す。Linux の /dev/serial/by-id/usb-TNB_Factory._FDC_emulator-if00 と -if02 だけ
//                  (ほかの OS では探せないので、ポートの名前を書く)
//   PORT_A,PORT_B  2 本のポートの名前 (macOS なら /dev/cu.usbmodemXXXX)。順番はどちらでもよい (x68k::Fuk が見分ける)
// 名前を書いた (か、上の名前で見つけた) ポート以外には何も送らない。
// 速さは 115200bps に設定する (速さそのものは USB では意味を持たないが、1200bps にすると FUK の代わりをする Pico の
// ファームが書き込みモードに入るため、設定する。OS が設定を読ませないときは、設定せずに使う)。
// 読み書きは待たない設定で開き、X68000 を動かすスレッドがそのまま読み書きする (別のスレッドは作らない)。
// 読み書きに失敗したら閉じ、2 本のどちらかの名前がいったん見えなくなって (抜いたとき) から開き直す。開けなかったときは 1 秒ごとに試す。
#pragma once

#include <stddef.h>

#include "x68k/fuk.h"

// spec は fuk= の値。開けなければ nullptr を返し、err に理由を入れる。返した口は delete で閉じる
x68k::FukLink* openHostFukLink(const char* spec, char* err, size_t errSize);
// 2 本のポートを名前で開く (試験用。pty でもよい)
x68k::FukLink* openHostFukLinkAt(const char* portA, const char* portB, char* err, size_t errSize);
