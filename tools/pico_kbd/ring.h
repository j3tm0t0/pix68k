// 1 バイト単位のリングバッファ。書く側と読む側がそれぞれ 1 つ (割り込みと主ループ) のときに、ロックなしで使える
#ifndef PICO_KBD_RING_H
#define PICO_KBD_RING_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t* buf;
    uint32_t mask;            // 大きさ - 1 (大きさは 2 のべき乗)
    volatile uint32_t head;   // 読む側だけが進める (読んだ累計)
    volatile uint32_t tail;   // 書く側だけが進める (書いた累計)
} Ring;

#define RING_INIT(storage) {(storage), sizeof(storage) - 1, 0, 0}

// buf への読み書きが head / tail の更新の前後に入れ替わらないようにする (コアは 1 つなのでコンパイラの並べ替えだけ止めればよい)
#define RING_FENCE() __atomic_signal_fence(__ATOMIC_SEQ_CST)

static inline uint32_t ring_count(const Ring* r) { return r->tail - r->head; }
static inline uint32_t ring_space(const Ring* r) { return r->mask + 1 - ring_count(r); }

static inline bool ring_put(Ring* r, uint8_t c) {
    uint32_t t = r->tail;
    if (t - r->head > r->mask) return false;
    r->buf[t & r->mask] = c;
    RING_FENCE();
    r->tail = t + 1;
    return true;
}

static inline bool ring_get(Ring* r, uint8_t* c) {
    uint32_t h = r->head;
    if (h == r->tail) return false;
    RING_FENCE();
    *c = r->buf[h & r->mask];
    RING_FENCE();
    r->head = h + 1;
    return true;
}

#endif
