#include "x68k/sram_file.h"

#include <string.h>

namespace x68k {

namespace {
const char kMagic[8] = {'P', 'X', '6', '8', 'S', 'R', 'A', 'M'};

uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
} // namespace

uint32_t SramSlots::crc32(const uint8_t* data, size_t size) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < size; i++) {
        c ^= data[i];
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

bool SramSlots::parse(const uint8_t* data, size_t size, uint32_t& generation) {
    generation = 0;
    if (data == nullptr) return false;
    if (size == SRAM_SIZE) return true;
    if (size != FILE_SIZE) return false;
    const uint8_t* t = data + SRAM_SIZE;
    if (memcmp(t, kMagic, sizeof kMagic) != 0) return false;
    if (get32(t + 12) != crc32(data, SRAM_SIZE + 12)) return false;
    generation = get32(t + 8);
    return true;
}

void SramSlots::build(const uint8_t* sram, uint32_t generation, uint8_t* out) {
    memcpy(out, sram, SRAM_SIZE);
    uint8_t* t = out + SRAM_SIZE;
    memcpy(t, kMagic, sizeof kMagic);
    put32(t + 8, generation);
    put32(t + 12, crc32(out, SRAM_SIZE + 12)); // SRAM、"PX68SRAM"、世代番号の CRC
}

void SramSlots::load(int slot, const uint8_t* data, size_t size) {
    valid_[slot] = parse(data, size, gen_[slot]);
}

int SramSlots::newest() const {
    // 世代番号は 32 ビットで周回するので、差の符号で比べる
    if (valid_[0] && valid_[1]) return (int32_t)(gen_[1] - gen_[0]) > 0 ? 1 : 0;
    if (valid_[0]) return 0;
    if (valid_[1]) return 1;
    return -1;
}

int SramSlots::nextSlot() const {
    int n = newest();
    return n < 0 ? 0 : 1 - n;
}

uint32_t SramSlots::nextGeneration() const {
    int n = newest();
    return n < 0 ? 1 : gen_[n] + 1;
}

void SramSlots::saved(int slot, uint32_t generation) {
    valid_[slot] = true;
    gen_[slot] = generation;
}

} // namespace x68k
