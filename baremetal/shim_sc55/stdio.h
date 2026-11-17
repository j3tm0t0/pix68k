// Nuked-SC55 (external/nuked-sc55) をベアメタルでビルドするための <stdio.h>。
// Nuked-SC55 は知らない番地を読み書きしたときなどに printf を使うだけ。printf は baremetal/munt_libc.cpp にあり、何も出さない。
// 本物の <stdio.h> と同じく NULL も決める (pcm.cpp が使う)
#pragma once
#include <stddef.h>

extern "C" int printf(const char* format, ...);
