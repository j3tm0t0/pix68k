// 単体テストの小さな道具 (CHECK_EQ と失敗の数)
#pragma once

#include <stdio.h>

static int failures = 0;

#define CHECK_EQ(a, b)                                                                                 \
    do {                                                                                               \
        unsigned long long va_ = (unsigned long long)(a), vb_ = (unsigned long long)(b);              \
        if (va_ != vb_) {                                                                              \
            printf("  %s:%d: %s == %s: got %llx, want %llx\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
            failures++;                                                                                \
        }                                                                                              \
    } while (0)

static int finish() {
    if (failures) {
        printf("FAILED: %d\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
