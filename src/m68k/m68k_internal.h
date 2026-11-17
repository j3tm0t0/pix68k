// コア内部で使う小さな定義
#pragma once

#include <stdint.h>

namespace m68k {

template <int S> struct Mask;
template <> struct Mask<1> { static constexpr uint32_t v = 0xff; };
template <> struct Mask<2> { static constexpr uint32_t v = 0xffff; };
template <> struct Mask<4> { static constexpr uint32_t v = 0xffffffff; };

template <int S> struct Msb;
template <> struct Msb<1> { static constexpr uint32_t v = 0x80; };
template <> struct Msb<2> { static constexpr uint32_t v = 0x8000; };
template <> struct Msb<4> { static constexpr uint32_t v = 0x80000000; };

inline uint32_t sext8(uint32_t v) { return (uint32_t)(int32_t)(int8_t)v; }
inline uint32_t sext16(uint32_t v) { return (uint32_t)(int32_t)(int16_t)v; }

inline int popcount16(uint32_t v) {
    v &= 0xffff;
    int n = 0;
    while (v) {
        v &= v - 1;
        n++;
    }
    return n;
}

// 命令の種類 (テンプレート引数)
enum { OP_ADD, OP_SUB, OP_AND, OP_OR, OP_EOR, OP_CMP };
enum { IMM_ORI, IMM_ANDI, IMM_SUBI, IMM_ADDI, IMM_EORI, IMM_CMPI };
enum { UN_NEGX, UN_CLR, UN_NEG, UN_NOT };
enum { BIT_TST, BIT_CHG, BIT_CLR, BIT_SET };
enum { SH_AS, SH_LS, SH_ROX, SH_RO };

} // namespace m68k
