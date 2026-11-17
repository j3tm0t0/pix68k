// Homebrew の aarch64-elf-gcc は C ライブラリ (newlib) のヘッダを持たない。
// FatFs などが <string.h> を読むので、Circle の同名の関数に置き換える。
#pragma once
#include <circle/util.h>
