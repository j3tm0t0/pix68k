// ベアメタル版の memcpy() と memset()。Circle (lib/util_fast.S) のものと置き換える。
//
// Circle の AArch64 の memcpy() は、128 バイト未満のときと、dest か src が 8 バイトにそろっていないときは、
// 1 バイトずつ写す。memset() は、32 バイトにそろうまでと、残りの 32 バイト未満を 1 バイトずつ書く。
// 大きさが定数の小さい写しはコンパイラがその場に展開する (builtin_mem.h) が、大きさが変数の写しは
// 関数の呼び出しのまま残る (画面の合成の行の消去、音の合成のバッファ、ディスクやネットワークのバッファなど)。
// ここでは、長さに関係なく 8 バイト (または 4 バイト) ずつ写す。
//
// 守っていること (Circle のものと同じ):
// - 2 バイト以上の読み書きは、その大きさにそろった番地だけで行う (dest と src の両方)。フレームバッファは Device の
//   メモリとして割り当てられ、MMU を有効にする前にも呼ばれる。どちらも、そろっていない読み書きは例外になる。
//   dest と src の番地の下位ビットが違っていて両方をそろえられないときは、そろえられる大きさ (4 バイト) に下げるか、
//   1 バイトずつ写す
// - memcpy() は、1 回分 (多くて 32 バイト) を全部読んでから書き、番地の小さいほうの回から順に進む。Circle の
//   memmove() は、dest が src より小さくて重なるときに memcpy() を呼ぶので、その使い方で正しく写せるようにする。
//   1 回分の中で書く順は決めていない (Cortex-A72 / A76 向けのビルドでは、コンパイラが 32 バイトの後ろの半分を
//   先に書く命令の並びにする)
// - 範囲の外は読まない、書かない (128 バイト以上のときの先読み (prfm) を除く。prfm は例外を起こさない)
// - 浮動小数点 / NEON のレジスタは使わない
// - 128 バイト以上で 8 バイトにそろっているときは、Circle と同じく 32 バイトずつ、先読みをしながら写す
//
// このファイルは -fno-builtin -fno-tree-loop-distribute-patterns -fno-tree-vectorize -mstrict-align でビルドする
// (Makefile。ループを memcpy() / memset() の呼び出しや NEON の命令に置き換えさせない)。
// ホストの試験 (tests/unit/mem_fast_test.cpp) は、PIX68K_MEM_NAME で名前を変え、PIX68K_MEM_ACCESS で読み書きの
// 番地と大きさを調べる。

#ifndef PIX68K_MEM_NAME
#define PIX68K_MEM_NAME(name) name
#endif
// p から size バイトを読む (store が 0) / 書く (store が 1) 前に呼ぶ。試験用
#ifndef PIX68K_MEM_ACCESS
#define PIX68K_MEM_ACCESS(p, size, store) ((void)0)
#endif

// 先読みする。addr は整数 (範囲の終わりを越えるポインタを作らない)。試験は、整数で渡されることと番地を調べる
#ifndef PIX68K_MEM_PREFETCH
#define PIX68K_MEM_PREFETCH(addr) __builtin_prefetch((const void*)(addr), 0, 0)
#endif

namespace {
typedef __SIZE_TYPE__ Size;
typedef __UINTPTR_TYPE__ Addr;
typedef unsigned char B1;
typedef unsigned short B2;
typedef unsigned int B4;
typedef unsigned long long B8;

// may_alias: どの型のメモリでも、この型で読み書きしてよいことをコンパイラに伝える
template <class T> inline __attribute__((always_inline)) T load(const B1* p) {
    typedef T __attribute__((may_alias)) Alias;
    PIX68K_MEM_ACCESS(p, sizeof(T), 0);
    return *(const Alias*)p;
}
template <class T> inline __attribute__((always_inline)) void store(B1* p, T v) {
    typedef T __attribute__((may_alias)) Alias;
    PIX68K_MEM_ACCESS(p, sizeof(T), 1);
    *(Alias*)p = v;
}
// 1 個写して、d、s、n を進める
template <class T> inline __attribute__((always_inline)) void copy(B1*& d, const B1*& s, Size& n) {
    store<T>(d, load<T>(s));
    d += sizeof(T);
    s += sizeof(T);
    n -= sizeof(T);
}
template <class T> inline __attribute__((always_inline)) void fill(B1*& d, Size& n, T v) {
    store<T>(d, v);
    d += sizeof(T);
    n -= sizeof(T);
}
} // namespace

extern "C" void* PIX68K_MEM_NAME(memcpy)(void* dest, const void* src, Size n) {
    B1* d = (B1*)dest;
    const B1* s = (const B1*)src;
    const Addr diff = (Addr)d ^ (Addr)s;
    if ((diff & 7) == 0 && n >= 8) {
        // 8 バイトにそろうまで (多くて 7 バイト。n は 8 以上なので足りる)
        if ((Addr)d & 1) copy<B1>(d, s, n);
        if ((Addr)d & 2) copy<B2>(d, s, n);
        if ((Addr)d & 4) copy<B4>(d, s, n);
        if (n >= 128) {
            // Circle と同じ形: 32 バイトを読んでから書き、その先を先読みする
            do {
                const B8 a = load<B8>(s), b = load<B8>(s + 8), c = load<B8>(s + 16), e = load<B8>(s + 24);
                store<B8>(d, a);
                store<B8>(d + 8, b);
                store<B8>(d + 16, c);
                store<B8>(d + 24, e);
                d += 32;
                s += 32;
                n -= 32;
                // 先読みの番地は範囲の終わりを越えることがあるので、ポインタではなく整数で計算する
                PIX68K_MEM_PREFETCH((Addr)s + 64);
            } while (n >= 32);
        }
        while (n >= 16) {
            const B8 a = load<B8>(s), b = load<B8>(s + 8);
            store<B8>(d, a);
            store<B8>(d + 8, b);
            d += 16;
            s += 16;
            n -= 16;
        }
        if (n & 8) copy<B8>(d, s, n);
        if (n & 4) copy<B4>(d, s, n);
        if (n & 2) copy<B2>(d, s, n);
        if (n & 1) copy<B1>(d, s, n);
        return dest;
    }
    if ((diff & 3) == 0 && n >= 4) {
        if ((Addr)d & 1) copy<B1>(d, s, n);
        if ((Addr)d & 2) copy<B2>(d, s, n);
        while (n >= 4) copy<B4>(d, s, n);
        if (n & 2) copy<B2>(d, s, n);
        if (n & 1) copy<B1>(d, s, n);
        return dest;
    }
    while (n != 0) copy<B1>(d, s, n);
    return dest;
}

extern "C" void* PIX68K_MEM_NAME(memset)(void* dest, int value, Size n) {
    B1* d = (B1*)dest;
    const B1 v1 = (B1)value;
    if (n >= 8) {
        const B8 v8 = v1 * 0x0101010101010101ULL;
        if ((Addr)d & 1) fill<B1>(d, n, v1);
        if ((Addr)d & 2) fill<B2>(d, n, (B2)v8);
        if ((Addr)d & 4) fill<B4>(d, n, (B4)v8);
        // 16 バイトにそろえる (16 バイトずつの書き込みがキャッシュの行をまたがないようにする。Circle は 32 バイトにそろえる)
        if (((Addr)d & 8) && n >= 8) fill<B8>(d, n, v8);
        while (n >= 32) {
            store<B8>(d, v8);
            store<B8>(d + 8, v8);
            store<B8>(d + 16, v8);
            store<B8>(d + 24, v8);
            d += 32;
            n -= 32;
        }
        if (n & 16) {
            store<B8>(d, v8);
            store<B8>(d + 8, v8);
            d += 16;
            n -= 16;
        }
        if (n & 8) fill<B8>(d, n, v8);
        if (n & 4) fill<B4>(d, n, (B4)v8);
        if (n & 2) fill<B2>(d, n, (B2)v8);
        if (n & 1) fill<B1>(d, n, v1);
        return dest;
    }
    while (n != 0) fill<B1>(d, n, v1);
    return dest;
}
