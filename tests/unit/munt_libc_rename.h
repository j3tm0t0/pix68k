// munt_libc_test 用: baremetal/munt_libc.cpp の関数を、ホストの libc / libm と名前がぶつからないように付け替える
#pragma once
#define sin pix68k_sin
#define cos pix68k_cos
#define exp pix68k_exp
#define log pix68k_log
#define log10 pix68k_log10
#define fmod pix68k_fmod
#define sqrt pix68k_sqrt
#define floor pix68k_floor
#define ceil pix68k_ceil
#define fabs pix68k_fabs
#define rand pix68k_rand
#define printf pix68k_printf
#define vprintf pix68k_vprintf
#define sprintf pix68k_sprintf
#define snprintf pix68k_snprintf
#define vsnprintf pix68k_vsnprintf
