// ホスト版で Nuked-SC55 (external/nuked-sc55) をビルドするときに -include で先に読むヘッダ。
// Nuked-SC55 は知らない番地を読み書きしたときなどに printf で標準出力に書く (SC-55mkII の起動のたびに 6 行出る)。
// pix68k の出力に混ぜないよう、何もしないものにする (ソースは変えない。ベアメタル版の printf は何も出さない)
#pragma once
#include <stdio.h>
#define printf(...) ((void)0)
