// baremetal/mem_fast.cpp (ベアメタル版の memcpy() / memset()) の試験。
// 同じソースを、名前を変えてホストでビルドする。読み書きのたびに呼ばれる PIX68K_MEM_ACCESS で、番地がその大きさに
// そろっていること、範囲の外を読み書きしないこと、番地の小さいほうから順に、すきまも重なりもなく書くことを調べる。
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <vector>

namespace {
struct Access {
    const uint8_t* src = nullptr; // 読んでよい範囲 (memset では使わない)
    const uint8_t* dst = nullptr; // 書いてよい範囲
    size_t n = 0;
    const uint8_t* nextStore = nullptr; // 次に書くはずの番地
    const uint8_t* loaded = nullptr;    // ここまで読んだ (src の側)
    int errors = 0;
    size_t maxAccess = 0; // 1 回の読み書きの大きさの最大
    int prefetches = 0;   // 先読みの回数
} g;

void fail(const char* what, const void* p, size_t size) {
    if (g.errors++ < 10)
        fprintf(stderr, "  %s: p=%p size=%zu (src=%p dst=%p n=%zu)\n", what, p, size, (const void*)g.src, (const void*)g.dst, g.n);
}

void access(const void* pv, size_t size, int store) {
    const uint8_t* p = (const uint8_t*)pv;
    if (((uintptr_t)p & (size - 1)) != 0) fail("そろっていない読み書き", p, size);
    if (size > g.maxAccess) g.maxAccess = size;
    if (store) {
        if (p < g.dst || p + size > g.dst + g.n) fail("範囲の外への書き込み", p, size);
        if (p != g.nextStore) fail("順でない書き込み", p, size);
        g.nextStore = p + size;
    } else {
        if (p < g.src || p + size > g.src + g.n) fail("範囲の外の読み込み", p, size);
        if (p + size > g.loaded) g.loaded = p + size;
    }
}

// 先読みの番地。mem_fast.cpp が整数で渡すこと (ポインタの演算で範囲の終わりを越えた番地を作ると未定義の動作に
// なるので、ポインタを渡すようになったらビルドできないようにする) と、読む範囲の終わりから 64 バイトの中に
// あることを調べる (範囲の終わりを越えるのはよい。先読みは例外を起こさない)
template <class T> void prefetch(T addr) {
    static_assert(std::is_integral<T>::value, "先読みの番地は整数で計算する");
    const uintptr_t a = (uintptr_t)addr;
    g.prefetches++;
    if (a < (uintptr_t)g.src || a > (uintptr_t)g.src + g.n + 64) fail("範囲から離れた先読み", (const void*)a, 0);
}
} // namespace

#define PIX68K_MEM_PREFETCH(addr) prefetch(addr)
#define PIX68K_MEM_NAME(name) pix68k_test_##name
#define PIX68K_MEM_ACCESS(p, size, store) access(p, size, store)
#include "../../baremetal/mem_fast.cpp"

namespace {
int failures = 0;

void check(bool ok, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void check(bool ok, const char* fmt, ...) {
    if (ok) return;
    if (failures++ < 20) {
        va_list ap;
        va_start(ap, fmt);
        fprintf(stderr, "FAIL: ");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
        va_end(ap);
    }
}

uint32_t rngState = 0x12345678;
uint8_t rnd() {
    rngState = rngState * 1664525u + 1013904223u;
    return (uint8_t)(rngState >> 24);
}

void begin(const uint8_t* src, const uint8_t* dst, size_t n) {
    g.src = src;
    g.dst = dst;
    g.n = n;
    g.nextStore = dst;
    g.loaded = src;
    g.errors = 0;
    g.maxAccess = 0;
    g.prefetches = 0;
}

// 書いた範囲がちょうど [dst, dst + n) だったか
bool wroteAll() { return g.errors == 0 && g.nextStore == g.dst + g.n; }

const size_t GUARD = 64;

// 重ならない写し: 長さ、dest と src のそろい方のすべての組み合わせ
void testCopy(size_t maxLen) {
    std::vector<uint8_t> srcBuf(maxLen + 2 * GUARD + 32), dstBuf(maxLen + 2 * GUARD + 32), want(dstBuf.size());
    // 番地の下位ビットを決めるため、バッファの先頭を 16 バイトにそろえた位置から数える
    uint8_t* sBase = srcBuf.data() + (16 - ((uintptr_t)srcBuf.data() & 15)) % 16;
    uint8_t* dBase = dstBuf.data() + (16 - ((uintptr_t)dstBuf.data() & 15)) % 16;
    for (auto& b : srcBuf) b = rnd();
    for (size_t n = 0; n <= maxLen; n++) {
        for (size_t so = 0; so < 16; so++) {
            for (size_t dof = 0; dof < 16; dof++) {
                uint8_t* s = sBase + GUARD / 2 + so;
                uint8_t* d = dBase + GUARD / 2 + dof;
                memset(dstBuf.data(), 0xee, dstBuf.size());
                memset(want.data(), 0xee, want.size());
                memcpy(want.data() + (d - dstBuf.data()), s, n);
                begin(s, d, n);
                void* r = pix68k_test_memcpy(d, s, n);
                check(r == d, "memcpy の戻り値 n=%zu so=%zu do=%zu", n, so, dof);
                check(wroteAll(), "memcpy の読み書き n=%zu so=%zu do=%zu (誤り %d)", n, so, dof, g.errors);
                check(memcmp(dstBuf.data(), want.data(), dstBuf.size()) == 0, "memcpy の結果 n=%zu so=%zu do=%zu", n, so, dof);
                // dest と src のそろい方が同じなら、8 バイト以上は 8 バイトずつ写していること (速さの確かめ)
                if (((so ^ dof) & 7) == 0 && n >= 16) check(g.maxAccess == 8, "memcpy が 8 バイトずつ写していない n=%zu so=%zu do=%zu", n, so, dof);
                // 128 バイト以上 (そろえたあと) なら 32 バイトごとに先読みする。127 バイト以下ではしない
                if (((so ^ dof) & 7) == 0 && n >= 135) check(g.prefetches >= 3, "memcpy が先読みしていない n=%zu so=%zu do=%zu", n, so, dof);
                if (n < 128) check(g.prefetches == 0, "小さい memcpy が先読みした n=%zu so=%zu do=%zu", n, so, dof);
                if (((so ^ dof) & 7) == 4 && n >= 8) check(g.maxAccess == 4, "memcpy が 4 バイトずつ写していない n=%zu so=%zu do=%zu", n, so, dof);
            }
        }
    }
}

// dest が src より小さくて重なる写し (Circle の memmove() がこの形で memcpy() を呼ぶ)。memmove() と同じ結果になること
void testCopyOverlap(size_t maxLen) {
    static const size_t dists[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 15, 16, 17, 24, 31, 32, 33, 40, 63, 64, 65, 127, 128, 129, 200};
    std::vector<uint8_t> buf(maxLen + 300 + 2 * GUARD), want(buf.size()), init(buf.size());
    for (auto& b : init) b = rnd();
    for (size_t n = 0; n <= maxLen; n++) {
        for (size_t dist : dists) {
            for (size_t dof = 0; dof < 8; dof++) {
                memcpy(buf.data(), init.data(), buf.size());
                memcpy(want.data(), init.data(), want.size());
                uint8_t* d = buf.data() + GUARD + dof;
                uint8_t* s = d + dist;
                memmove(want.data() + GUARD + dof, want.data() + GUARD + dof + dist, n);
                begin(s, d, n);
                void* r = pix68k_test_memcpy(d, s, n);
                check(r == d, "重なる memcpy の戻り値 n=%zu dist=%zu do=%zu", n, dist, dof);
                check(wroteAll(), "重なる memcpy の読み書き n=%zu dist=%zu do=%zu (誤り %d)", n, dist, dof, g.errors);
                check(memcmp(buf.data(), want.data(), buf.size()) == 0, "重なる memcpy の結果 n=%zu dist=%zu do=%zu", n, dist, dof);
            }
        }
    }
}

void testSet(size_t maxLen) {
    static const int values[] = {0, 0xa5, 0xff, 0x1234, -1, 0x80};
    std::vector<uint8_t> buf(maxLen + 2 * GUARD + 64), want(buf.size());
    uint8_t* base = buf.data() + (32 - ((uintptr_t)buf.data() & 31)) % 32;
    int vi = 0;
    for (size_t n = 0; n <= maxLen; n++) {
        for (size_t dof = 0; dof < 32; dof++) {
            const int value = values[vi++ % (sizeof values / sizeof values[0])];
            uint8_t* d = base + GUARD / 2 + dof;
            memset(buf.data(), 0x11, buf.size());
            memset(want.data(), 0x11, want.size());
            memset(want.data() + (d - buf.data()), value, n);
            begin(nullptr, d, n);
            void* r = pix68k_test_memset(d, value, n);
            check(r == d, "memset の戻り値 n=%zu do=%zu", n, dof);
            check(wroteAll(), "memset の読み書き n=%zu do=%zu (誤り %d)", n, dof, g.errors);
            check(memcmp(buf.data(), want.data(), buf.size()) == 0, "memset の結果 n=%zu do=%zu value=%d", n, dof, value);
            if (n >= 16) check(g.maxAccess == 8, "memset が 8 バイトずつ書いていない n=%zu do=%zu", n, dof);
        }
    }
}

// 大きい写し (フレームやディスクのイメージの大きさ)
void testLarge() {
    static const size_t sizes[] = {4096, 65536 + 3, 768 * 512 * 4, 1 << 20};
    for (size_t n : sizes) {
        std::vector<uint8_t> src(n + 64), dst(n + 64), want(n + 64);
        for (auto& b : src) b = rnd();
        for (size_t off = 0; off < 9; off += 4) { // 0、4、8
            for (size_t rel = 0; rel < 2; rel++) { // dest と src のそろい方が同じ / 1 バイト違う
                memset(dst.data(), 0x77, dst.size());
                memset(want.data(), 0x77, want.size());
                const uint8_t* s = src.data() + off + rel;
                uint8_t* d = dst.data() + off;
                memcpy(want.data() + off, s, n);
                begin(s, d, n);
                pix68k_test_memcpy(d, s, n);
                check(wroteAll() && memcmp(dst.data(), want.data(), dst.size()) == 0, "大きい memcpy n=%zu off=%zu rel=%zu", n, off, rel);
            }
            memset(want.data() + off, 0x5a, n);
            begin(nullptr, dst.data() + off, n);
            pix68k_test_memset(dst.data() + off, 0x5a, n);
            check(wroteAll() && memcmp(dst.data(), want.data(), dst.size()) == 0, "大きい memset n=%zu off=%zu", n, off);
        }
    }
}
} // namespace

int main() {
    testCopy(300);
    testCopyOverlap(300);
    testSet(300);
    testLarge();
    if (failures != 0) {
        fprintf(stderr, "mem_fast_test: %d failures\n", failures);
        return 1;
    }
    printf("mem_fast_test: ok\n");
    return 0;
}
