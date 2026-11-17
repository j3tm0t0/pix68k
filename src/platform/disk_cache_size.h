// 使うチャンクだけを読むイメージ (x68k/disk_cache.h) に使うヒープの見積もりと、枠の数の決め方 (ベアメタル版)。
// 見積もりには、中身の枠のほかに、Slot の表、チャンク → 枠の表、dirty フラグ (32 チャンクごとの語に切り上げる)、
// DiskCache と HardDiskImage そのものを入れる。ヒープの確保の仕方 (小さいものはバケットの大きさに切り上げ、
// 前にヘッダを置いてそろえる。Circle の lib/heapallocator.cpp) は HeapModel で渡す。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "x68k/dirty_map.h"
#include "x68k/disk_cache.h"
#include "x68k/hdd.h"

namespace platform {

// イメージの全体がメモリに入るなら、すべてのチャンクの枠を用意する (読んだチャンクは空けず、残りは裏で順に読むので、
// しばらくすると全体をメモリに読んだのと同じになる。x68k/disk_cache.h の fillAhead())。枠の中身を置くところ:
//   - 1GB より上のヒープ (Pi 4 / Pi 5 でメモリが 1GB より多いとき) に入れば、そこに置く
//   - なければ 1GB より下のヒープに置く。ここはほかの確保 (060turbo のローカルメモリ、SoundFont、MT-32 / SC-55 の ROM、
//     ステートなど) も使うので、DISK_CACHE_WHOLE_MAX_BYTES 以下のイメージは入るかぎり、それより大きいイメージは
//     使ってよい大きさの半分までに収まるときだけ
constexpr uint64_t DISK_CACHE_WHOLE_MAX_BYTES = 64ull << 20;
// 全体が入らないときの枠の数 (1 つ 16KB。256 個で 4MB)
constexpr unsigned DISK_CACHE_SLOTS = 256;

// ヒープの確保の仕方。buckets は小さい確保を切り上げる大きさ (昇順、0 で終わる。nullptr ならない)、
// header は確保ごとに前に置く大きさ、align はヘッダと中身の合計をそろえる単位 (2 のべき乗)
struct HeapModel {
    const uint32_t* buckets;
    uint32_t header;
    uint32_t align;
};

// size バイトを new したときに減るヒープの大きさ
inline uint64_t heapBlockBytes(uint64_t size, const HeapModel& h) {
    if (h.buckets) {
        for (const uint32_t* b = h.buckets; *b; b++) {
            if (size <= *b) {
                size = *b;
                break;
            }
        }
    }
    const uint64_t a = h.align ? h.align : 1;
    return (h.header + size + a - 1) / a * a;
}

// imageBytes バイトのイメージを slots 個の枠で読むときに減るヒープの大きさ
// (machine_run.cpp の new DiskCache、new HardDiskImage、DiskCache::init()、HardDiskImage::loadCached() の分)。
// イメージのファイルを開く new FIL と、SetupFastSeek() が作るクラスタの並びの表 (FIL::cltbl。64 語から、
// ファイルが断片になっていればそれ以上) は入れていない。見積もりはその分 (断片がなければ 1KB ほど) 実際より少ない
inline uint64_t diskCacheHeapBytes(uint64_t imageBytes, unsigned slots, const HeapModel& h) {
    uint64_t a[x68k::DiskCache::ALLOCATIONS];
    x68k::DiskCache::allocations(imageBytes, slots, a);
    uint64_t total = heapBlockBytes(sizeof(x68k::DiskCache), h) + heapBlockBytes(sizeof(x68k::HardDiskImage), h) +
                     heapBlockBytes(x68k::DirtyMap::allocationBytes(imageBytes), h);
    for (unsigned i = 0; i < x68k::DiskCache::ALLOCATIONS; i++) total += heapBlockBytes(a[i], h);
    return total;
}

// 枠の中身 (slots 個) を確保したときに減るヒープの大きさ
inline uint64_t diskCacheDataBytes(unsigned slots, const HeapModel& h) {
    return heapBlockBytes((uint64_t)slots * x68k::DiskCache::CHUNK, h);
}

// 枠の中身を別のところ (1GB より上のヒープ) に置くときに、1GB より下のヒープが減る大きさ (表、dirty フラグ、オブジェクト)
inline uint64_t diskCacheTableBytes(uint64_t imageBytes, unsigned slots, const HeapModel& h) {
    return diskCacheHeapBytes(imageBytes, slots, h) - diskCacheDataBytes(slots, h);
}

// 枠の数と、枠の中身を置くところ
struct DiskCachePlan {
    unsigned slots;
    bool high; // 枠の中身を 1GB より上のヒープに置く (表などは 1GB より下)
    bool whole; // すべてのチャンクの枠がある
};

// 枠の数を決める。lowRoom / highRoom は、1GB より下 / 上のヒープで使ってよい大きさ (上のヒープがなければ 0)。
// すべてのチャンクの枠 (4 個以上) が入ればその数、入らなければ DISK_CACHE_SLOTS
// (それが lowRoom に入るかは呼んだ側が diskCacheHeapBytes() で調べる。DISK_CACHE_SLOTS 以下で足りるイメージも同じ)
inline DiskCachePlan planDiskCache(uint64_t imageBytes, uint64_t lowRoom, uint64_t highRoom, const HeapModel& h) {
    const uint64_t chunks = (imageBytes + x68k::DiskCache::CHUNK - 1) / x68k::DiskCache::CHUNK;
    if (chunks > x68k::DiskCache::MAX_SLOTS) return {DISK_CACHE_SLOTS, false, false};
    const unsigned whole = chunks > 4 ? (unsigned)chunks : 4u;
    if (whole <= DISK_CACHE_SLOTS) return {whole, false, true};
    if (diskCacheDataBytes(whole, h) <= highRoom && diskCacheTableBytes(imageBytes, whole, h) <= lowRoom)
        return {whole, true, true};
    const uint64_t limit = imageBytes <= DISK_CACHE_WHOLE_MAX_BYTES ? lowRoom : lowRoom / 2;
    if (diskCacheHeapBytes(imageBytes, whole, h) <= limit) return {whole, false, true};
    return {DISK_CACHE_SLOTS, false, false};
}

} // namespace platform
