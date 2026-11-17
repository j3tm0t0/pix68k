// munt_libc_test 用: Munt の表 (external/munt/mt32emu/src/Tables.cpp) を、ベアメタル版と同じく baremetal/shim_munt の
// <cmath> と baremetal/munt_libc.cpp の数学の関数でビルドする。ホストの libm で作ったもの (mt32emu のライブラリ) と
// 同時に置けるよう、名前空間を MT32EmuShim に付け替える (このファイルは shim_munt を -I で先に読むようにビルドする)
#include "munt_libc_rename.h"

#define MT32Emu MT32EmuShim
#include "Tables.cpp"

extern "C" const void* pix68kShimTables() { return &MT32EmuShim::Tables::getInstance(); }
