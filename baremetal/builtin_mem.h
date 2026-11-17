// ベアメタル版で、memcpy / memset / memmove / memcmp / strlen をコンパイラの組み込み関数として扱わせる。
// Makefile が、pix68k と同梱のライブラリ (Munt、Nuked-SC55、TinySoundFont) のすべてのソースに -include で読ませる
// (Circle のライブラリと addon のビルドには渡らない)。
//
// Circle の Rules.mk は -ffreestanding を付ける。このとき GCC はこれらを普通の関数として扱い、大きさが定数の
// 4 バイトや 8 バイトの写しも Circle の関数の呼び出しにする (Circle の memcpy() は 128 バイト未満を 1 バイトずつ写す)。
// ここで __builtin_ の付いた名前を呼ぶインライン関数を先に定義しておくと、大きさが定数の小さい写しや比較は
// その場のロードとストアに展開され、展開されないもの (大きさが変数のもの、大きいもの) は今までと同じ名前の関数の
// 呼び出しになる (gnu_inline なので、この定義そのものが関数として出力されることはない)。
//
// -fbuiltin を付ける方法は使わない。printf が putchar の呼び出しに書き換えられてリンクできなくなる (Munt の
// Synth.cpp で起きた)。数学の関数 (munt_libc.cpp が定義している) も組み込み関数として扱われるようになる
// (結果が変わるかどうかは確かめていない)。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define PIX68K_BUILTIN_MEM extern inline __attribute__((gnu_inline, always_inline, artificial))

PIX68K_BUILTIN_MEM void* memcpy(void* dest, const void* src, __SIZE_TYPE__ n) { return __builtin_memcpy(dest, src, n); }
PIX68K_BUILTIN_MEM void* memset(void* dest, int value, __SIZE_TYPE__ n) { return __builtin_memset(dest, value, n); }
PIX68K_BUILTIN_MEM void* memmove(void* dest, const void* src, __SIZE_TYPE__ n) { return __builtin_memmove(dest, src, n); }
PIX68K_BUILTIN_MEM int memcmp(const void* a, const void* b, __SIZE_TYPE__ n) { return __builtin_memcmp(a, b, n); }
PIX68K_BUILTIN_MEM __SIZE_TYPE__ strlen(const char* s) { return __builtin_strlen(s); }

#undef PIX68K_BUILTIN_MEM

#ifdef __cplusplus
}
#endif
