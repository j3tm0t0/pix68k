// メモリに入りきらない大きなイメージ (数百 MB の HDS、CD-ROM の ISO) を、使うチャンク (DirtyMap::CHUNK バイト)
// だけ読んでおくキャッシュ。HardDiskImage::loadCached() で使う。
//
// X68000 を動かすスレッド (ベアメタル版のコア 1) は SD カードを読めないので、読むのは環境 (コア 0) に頼む:
//   - コア 1: lookup() がチャンクを探し、なければ空いている枠を選んで「読み込み中」にし、頼みのリング (1 対 1、
//     ロックなし) に入れる。読み込み中はコントローラが REQ を出さずに待つ (x68k/hdd.h の DiskCursor)。
//     prefetch() は、これから読むチャンクを先に頼む
//   - コア 0: nextLoad() で頼みを取り出し、ファイルから枠に読んで loaded() で返す (release)。
//     コア 1 は枠の状態を acquire で読み、読み込めていれば使う
// 読めなかったとき: 先読み (prefetch()。コマンドが待っているとは限らない) の失敗はエラーにせず、次に lookup() したときに
// 読み直しを頼む。コマンドが頼んだ読み込み (lookup() の load) か、その読み直しが失敗したときだけ failed を返す。
// 失敗が続いたら (FAIL_STREAK 回) 先読みをやめ、読めたら戻す (SD カードが壊れているときに読み続けないため)。
// 先読みで読めなかったチャンクは、失敗が続いていても 1 回はコマンドの読み込みとして頼む (先読みだけで failed にしない)。
// 枠がすべてのチャンクに足りるイメージ (whole()) は、読んだチャンクを空けない。コア 1 がフレームの切れ目で fillAhead() を呼び、
// まだ読んでいないチャンクを先頭から順に少しずつ頼むので、しばらくするとイメージの全体がメモリに入る (それからは待たない)。
// コマンドが読みたいチャンクは、これまでどおり lookup() がすぐに頼む。
// 枠を空けるのはコア 1 だけ。読み込み中の枠、X68000 が書いてまだ書き戻していない枠 (DirtyMap の dirty フラグが立っているか、
// 書き戻しの途中のもの)、いまコントローラが読み書きしている枠は空けない。空けられる枠がなければ pressure を立て、
// コア 0 はすぐに書き戻す (platform/disk_sync.h)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "x68k/dirty_map.h"

namespace x68k {

class DiskCache {
public:
    static constexpr uint32_t CHUNK = DirtyMap::CHUNK;
    static constexpr unsigned QUEUE = 64; // 頼みのリングの大きさ (2 のべき乗)
    // 枠の中身は 1 つのブロックに確保する。Circle のヒープのブロックの大きさは 32 ビット (THeapBlockHeader::nSize) なので、
    // 4GB より小さくする (4GB - 16KB まで)
    static constexpr unsigned MAX_SLOTS = (1u << 18) - 1;
    static constexpr unsigned FAIL_STREAK = 4; // 続けてこの回数読めなければ、先読みをやめる

    DiskCache() {}
    ~DiskCache() {
        if (ownData_) delete[] data_;
        else if (release_) release_(data_);
        delete[] slots_;
        delete[] map_;
    }
    DiskCache(const DiskCache&) = delete;
    DiskCache& operator=(const DiskCache&) = delete;

    // imageBytes バイトのイメージのために、slots 個の枠を用意する (コア 0、動かす前)。メモリが足りなければ false。
    // data が nullptr でなければ、枠の中身 (slots * CHUNK バイト) をそこに置く (new とは別のところから確保したとき。
    // ベアメタル版の 1GB より上のヒープ)。release があれば、この DiskCache を捨てるときに release(data) を呼ぶ
    // (init() が false を返したときも、捨てるときに呼ぶ。呼んだ側は data を自分で返さない)
    bool init(uint64_t imageBytes, unsigned slots, uint8_t* data = nullptr, void (*release)(uint8_t*) = nullptr) {
        if (data) {
            data_ = data;
            ownData_ = false;
            release_ = release;
        }
        if (slots < 4 || slots > MAX_SLOTS) return false;
        const uint64_t chunks = (imageBytes + CHUNK - 1) / CHUNK;
        if (chunks == 0 || chunks > 0xffffffffull) return false;
        if (!data) data_ = new uint8_t[(size_t)slots * CHUNK];
        slots_ = new Slot[slots];
        map_ = new uint32_t[(size_t)chunks];
        if (!data_ || !slots_ || !map_) return false;
        for (uint64_t i = 0; i < chunks; i++) map_[i] = 0;
        nslots_ = slots;
        chunks_ = (uint32_t)chunks;
        mapCap_ = (uint32_t)chunks;
        return true;
    }
    // 別のイメージ (imageBytes バイト) のために作り直す (CD-ROM の媒体を入れ替えたとき)。枠はそのまま使い回し、
    // チャンク → 枠の表は、前の大きさに入れば使い回し、入らなければ作り直す。読み込んだ中身と頼まれた読み込みは捨てる。コア 1 を止めていて、コア 0 が
    // 読み込みの途中でないときに呼ぶ。このあと HardDiskImage::loadCached() でつなぎ直すこと。メモリが足りなければ false
    bool reinit(uint64_t imageBytes) {
        const uint64_t chunks = (imageBytes + CHUNK - 1) / CHUNK;
        if (nslots_ == 0 || chunks == 0 || chunks > 0xffffffffull) return false;
        if (chunks > mapCap_) {
            uint32_t* map = new uint32_t[(size_t)chunks];
            if (!map) return false;
            delete[] map_;
            map_ = map;
            mapCap_ = (uint32_t)chunks;
        }
        for (uint64_t i = 0; i < chunks; i++) map_[i] = 0;
        for (unsigned i = 0; i < nslots_; i++) slots_[i] = Slot();
        chunks_ = (uint32_t)chunks;
        dirty_ = nullptr;
        hand_ = 0;
        pinned_ = ~0u;
        failStreak_ = 0;
        __atomic_store_n(&fillNext_, 0u, __ATOMIC_RELAXED);
        fillMisses_ = misses_;
        fillLast_ = NO_CHUNK;
        fillFails_ = 0;
        fillStopped_ = false;
        __atomic_store_n(&okLoads_, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&tail_, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&head_, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&pressure_, 0u, __ATOMIC_RELEASE);
        return true;
    }
    // init(imageBytes, slots) が new で確保する大きさ (バイト)。中身の枠 (data を渡すときは確保しない)、Slot の表、
    // チャンク → 枠の表の順。
    // ベアメタル版がヒープの空きと比べるのに使う (platform/disk_cache_size.h)
    static constexpr unsigned ALLOCATIONS = 3;
    static void allocations(uint64_t imageBytes, unsigned slots, uint64_t out[ALLOCATIONS]) {
        const uint64_t chunks = (imageBytes + CHUNK - 1) / CHUNK;
        out[0] = (uint64_t)slots * CHUNK;
        out[1] = (uint64_t)slots * sizeof(Slot);
        out[2] = chunks * sizeof(uint32_t);
    }
    void setDirtyMap(const DirtyMap* m) { dirty_ = m; }
    unsigned slots() const { return nslots_; }
    // 枠がすべてのチャンクに足りる (読んだチャンクを空けない。fillAhead() が残りを順に頼む)
    bool whole() const { return nslots_ != 0 && nslots_ >= chunks_; }

    // ---- コア 1 ----
    // チャンク c の中身。読み込み中なら nullptr (load なら、まだ頼んでいなければ頼む)。
    // 読めなかったら failed を立てて nullptr (枠は空ける。次に lookup() すれば読み直す)。ただし先読みで読めなかった
    // チャンクは failed を立てずに nullptr を返す (load なら読み直しを頼む)。
    // 返した枠は、次に lookup() を呼ぶまで空けない
    uint8_t* lookup(uint32_t c, bool load, bool& failed) {
        failed = false;
        if (c >= chunks_) return nullptr;
        if (const uint32_t m = map_[c]) {
            const unsigned s = m - 1u;
            switch (__atomic_load_n(&slots_[s].state, __ATOMIC_ACQUIRE)) {
            case READY:
                if (failStreak_) failStreak_ = 0;
                slots_[s].ref = 1;
                pinned_ = s;
                __atomic_store_n(&hits_, hits_ + 1, __ATOMIC_RELAXED);
                return data_ + (size_t)s * CHUNK;
            case FAILED: {
                map_[c] = 0;
                setState(s, FREE);
                if (failStreak_ < FAIL_STREAK) failStreak_++;
                // 先読みの失敗はエラーにせず、コマンドの読み込みとして読み直す。失敗が続いていても 1 回は頼む
                // (失敗が続いている間は先読みを頼まないが、その前に頼んだ先読みの失敗がここに来ることがある。
                // コマンドの読み込みは失敗が続いていても頼むので、それと同じ扱いにする)
                if (!slots_[s].demand) {
                    if (load) request(c, true);
                    return nullptr;
                }
                failed = true;
                return nullptr;
            }
            default:
                return nullptr; // 読み込み中
            }
        }
        if (load) request(c, true);
        return nullptr;
    }
    // 読み込めていれば中身を返す (頼まない。返した枠を「使っている枠」にもしない)。書き戻しの写しに使う
    const uint8_t* peek(uint32_t c) const {
        if (c >= chunks_ || !map_[c]) return nullptr;
        const unsigned s = map_[c] - 1u;
        return __atomic_load_n(&slots_[s].state, __ATOMIC_ACQUIRE) == READY ? data_ + (size_t)s * CHUNK : nullptr;
    }
    // これから使うチャンクを先に頼む (空けられる枠がなければ頼まない。頼みのリングは半分まで)
    void prefetch(uint32_t c) {
        if (c >= chunks_ || map_[c] || failStreak_ >= FAIL_STREAK) return;
        if (head_ - __atomic_load_n(&tail_, __ATOMIC_ACQUIRE) >= QUEUE / 2) return;
        request(c, false);
    }
    // 枠がすべてのチャンクに足りるイメージ (whole()) で、まだ読んでいないチャンクを先頭から順に max 個まで頼む
    // (フレームの切れ目で呼ぶ)。コマンドの読み込みをその後ろで待たせないよう、頼みが残っている間と、前に呼んでから
    // コマンドが読み込みを頼んだ間は、足さない。読めないことが続いている間 (FAIL_STREAK) も頼まない。
    // 読めなかったチャンクは、先読みと同じく、コマンドが読むときに読み直す。前の回の最後に頼んだチャンクが読めないことが
    // FAIL_STREAK 回続いたら、裏で読むのをやめる (SD カードが壊れているときに、イメージの終わりまで読み続けないため。
    // 作り直す (reinit()) まで戻さない)
    void fillAhead(unsigned max) {
        if (fillStopped_ || fillNext_ >= chunks_ || nslots_ < chunks_ || failStreak_ >= FAIL_STREAK) return;
        const uint32_t m = misses_;
        const bool waited = m != fillMisses_;
        fillMisses_ = m;
        if (waited || head_ != __atomic_load_n(&tail_, __ATOMIC_ACQUIRE)) return;
        if (fillLast_ != NO_CHUNK && map_[fillLast_]) {
            const unsigned st = __atomic_load_n(&slots_[map_[fillLast_] - 1u].state, __ATOMIC_ACQUIRE);
            if (st == LOADING) return; // コア 0 が取り出したが、まだ読み終えていない
            if (st != FAILED) fillFails_ = 0;
            else if (++fillFails_ >= FAIL_STREAK) {
                fillStopped_ = true;
                return;
            }
            fillLast_ = NO_CHUNK;
        }
        // 読んであるチャンクを飛ばす数にも上限を置く (1 回の呼び出しを短くする)
        unsigned n = 0;
        for (unsigned scanned = 0; n < max && fillNext_ < chunks_ && scanned < FILL_SCAN; scanned++) {
            const uint32_t c = fillNext_;
            if (!map_[c]) {
                request(c, false);
                if (!map_[c]) break; // 頼めなかった (次に呼ばれたときにもう一度)
                fillLast_ = c;
                n++;
            }
            __atomic_store_n(&fillNext_, c + 1, __ATOMIC_RELAXED);
        }
    }
    // fillAhead() が頼み終えた (すべてのチャンクを読んだか、読み込みを頼んである。読めたかどうかは allLoaded())
    bool fillDone() const { return __atomic_load_n(&fillNext_, __ATOMIC_RELAXED) >= chunks_; }
    // 書き戻しの途中 (コア 0 に渡したが、まだ書き終えていない) のチャンクは空けない
    void setInFlight(uint32_t c, bool on) {
        if (c < chunks_ && map_[c]) slots_[map_[c] - 1u].inFlight = on;
    }

    // ---- コア 0 ----
    // 頼まれた読み込みを 1 つ取り出す。buf (len バイト) にファイルの c * CHUNK から読み、loaded(slot, ok) で返す
    bool nextLoad(unsigned& slot, uint32_t& c, uint8_t*& buf, uint32_t& len) {
        const unsigned t = tail_;
        if (t == __atomic_load_n(&head_, __ATOMIC_ACQUIRE)) return false;
        const Request r = queue_[t & (QUEUE - 1)];
        __atomic_store_n(&tail_, t + 1, __ATOMIC_RELEASE);
        slot = r.slot;
        c = r.chunk;
        buf = data_ + (size_t)r.slot * CHUNK;
        len = dirty_ ? dirty_->chunkBytes(c) : CHUNK;
        return true;
    }
    void loaded(unsigned slot, bool ok) {
        if (ok) __atomic_store_n(&okLoads_, okLoads_ + 1, __ATOMIC_RELAXED);
        setState(slot, ok ? READY : FAILED);
    }
    // すべてのチャンクが読めて、メモリにある (whole() のキャッシュは読めたチャンクを空けないので、読めた数で分かる)
    bool allLoaded() const { return whole() && __atomic_load_n(&okLoads_, __ATOMIC_RELAXED) >= chunks_; }
    // 読み込みを頼まれている (nextLoad() で取り出すものがある)
    bool loadQueued() const { return tail_ != __atomic_load_n(&head_, __ATOMIC_ACQUIRE); }
    // 空けられる枠がなかった (書き戻すまで待っている)。読むと消える
    bool takePressure() { return __atomic_exchange_n(&pressure_, 0u, __ATOMIC_ACQ_REL) != 0; }
    // 統計 (コア 1 が書き、コア 0 がログに出す。おおよその値)
    uint32_t hits() const { return __atomic_load_n(&hits_, __ATOMIC_RELAXED); }
    uint32_t misses() const { return __atomic_load_n(&misses_, __ATOMIC_RELAXED); }

private:
    enum : unsigned { FREE = 0, LOADING = 1, READY = 2, FAILED = 3 };
    static constexpr unsigned FILL_SCAN = 4096;
    static constexpr uint32_t NO_CHUNK = 0xffffffffu;
    struct Slot {
        uint32_t chunk = 0;
        unsigned state = FREE; // コア 1 が FREE と LOADING を、コア 0 が READY と FAILED を書く
        uint8_t ref = 0;       // 最近使った (時計の針で空けるときに 1 回見逃す)
        bool inFlight = false;
        bool demand = false; // コマンドが頼んだ読み込み (lookup() の load)。false なら先読み
    };
    struct Request {
        uint32_t chunk;
        unsigned slot;
    };

    void setState(unsigned s, unsigned st) { __atomic_store_n(&slots_[s].state, st, __ATOMIC_RELEASE); }

    // 空ける枠を選ぶ (時計の針)。なければ -1
    int victim() {
        const bool keep = nslots_ >= chunks_; // 枠がすべてのチャンクに足りる: 読めたチャンクは空けない (空いた枠が必ずある)
        for (unsigned k = 0; k < 2 * nslots_; k++) {
            const unsigned s = hand_;
            hand_ = (hand_ + 1) % nslots_;
            Slot& sl = slots_[s];
            const unsigned st = __atomic_load_n(&sl.state, __ATOMIC_ACQUIRE);
            if (st == FREE) return (int)s;
            if (keep && st == READY) continue;
            if (st == LOADING || s == pinned_ || sl.inFlight) continue;
            if (st == READY && dirty_ && dirty_->test(sl.chunk)) continue; // 書き戻していない
            if (st == READY && sl.ref) {
                sl.ref = 0;
                continue;
            }
            return (int)s; // READY (使っていない) か FAILED
        }
        return -1;
    }

    void request(uint32_t c, bool demand) {
        if (head_ - __atomic_load_n(&tail_, __ATOMIC_ACQUIRE) >= QUEUE) return; // 頼みのリングがいっぱい
        const int v = victim();
        if (v < 0) {
            if (demand) __atomic_store_n(&pressure_, 1u, __ATOMIC_RELEASE);
            return;
        }
        Slot& sl = slots_[v];
        if (__atomic_load_n(&sl.state, __ATOMIC_RELAXED) != FREE && map_[sl.chunk] == (uint32_t)(v + 1))
            map_[sl.chunk] = 0;
        sl.chunk = c;
        sl.ref = 1;
        sl.inFlight = false;
        sl.demand = demand;
        setState((unsigned)v, LOADING);
        map_[c] = (uint32_t)(v + 1);
        queue_[head_ & (QUEUE - 1)] = Request{c, (unsigned)v};
        __atomic_store_n(&head_, head_ + 1, __ATOMIC_RELEASE);
        if (demand) __atomic_store_n(&misses_, misses_ + 1, __ATOMIC_RELAXED);
    }

    uint8_t* data_ = nullptr;
    Slot* slots_ = nullptr;
    bool ownData_ = true;                 // data_ は init() が new で確保した
    void (*release_)(uint8_t*) = nullptr; // data_ を外から渡されたときの後始末 (なければ何もしない)
    uint32_t* map_ = nullptr; // チャンク → 枠の番号 + 1 (0 はない)。コア 1 だけが読み書きする
    unsigned nslots_ = 0;
    uint32_t chunks_ = 0;
    uint32_t mapCap_ = 0; // map_ に確保してある個数 (reinit() で使い回す)
    const DirtyMap* dirty_ = nullptr;
    unsigned hand_ = 0;
    unsigned pinned_ = ~0u;
    // 続けて読めなかった回数。lookup() が FAILED の枠を見たときに増やし、READY の枠を見たときに 0 に戻す
    // (コア 0 が読めたときではなく、コア 1 が lookup() でそれを見たときに戻る)
    unsigned failStreak_ = 0;
    uint32_t fillNext_ = 0;   // fillAhead() が次に見るチャンク (コア 1 が書く)
    uint32_t fillMisses_ = 0; // 前に fillAhead() を呼んだときの misses_
    uint32_t fillLast_ = NO_CHUNK; // fillAhead() が最後に頼んだチャンク (結果を次の回に見る)
    unsigned fillFails_ = 0;       // それが続けて読めなかった回数
    bool fillStopped_ = false;     // 読めないことが続いたので、裏で読むのをやめた
    Request queue_[QUEUE] = {};
    unsigned head_ = 0; // コア 1 が書く
    unsigned tail_ = 0; // コア 0 が書く
    unsigned pressure_ = 0;
    uint32_t hits_ = 0, misses_ = 0;
    uint32_t okLoads_ = 0; // 読めたチャンクの数 (コア 0 が書く。reinit() で 0 に戻す)
};

} // namespace x68k
