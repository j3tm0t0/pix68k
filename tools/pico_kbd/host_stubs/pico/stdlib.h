// test_ota_host.c が ota.c をホストでビルドするための、最小の代わりのヘッダ
#ifndef HOST_STUB_PICO_STDLIB_H
#define HOST_STUB_PICO_STDLIB_H
#include <stdbool.h>
#include <stdint.h>
#define XIP_BASE 0x10000000u
static inline void tight_loop_contents(void) {}
#endif
