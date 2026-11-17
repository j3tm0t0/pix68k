// ディスクのイメージのうち、X68000 が書いた部分の dirty フラグ (チャンク = CHUNK バイトごとに 1 ビット)。
// 環境 (ベアメタル版のコア 0) が、 dirty フラグの立ったチャンクだけをイメージのファイルに書き戻すのに使う
// (platform/disk_sync.h)。
//
// dirty フラグを立てる (mark) のは X68000 を動かしているスレッド (ベアメタル版のコア 1) だけで、 dirty フラグを読んで消すのも
// 同じスレッド (フレームの切れ目) か、X68000 を止めている間のコア 0 だけ。ほかのスレッドが動いている間に
// 読んでよいのは count() と writes() だけ (atomic に読み書きする)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace x68k {

class DirtyMap {
public:
    static constexpr unsigned CHUNK_SHIFT = 14;
    static constexpr uint32_t CHUNK = 1u << CHUNK_SHIFT; // 16KB

    DirtyMap() {}
    ~DirtyMap() { delete[] bits_; }
    DirtyMap(const DirtyMap&) = delete;
    DirtyMap& operator=(const DirtyMap&) = delete;

    // bytes バイトのイメージの分の dirty フラグを用意する (すべて消えた状態)。前の大きさ以下なら確保し直さない
    bool init(uint64_t bytes) {
        const uint64_t chunks = (bytes + CHUNK - 1) >> CHUNK_SHIFT;
        if (chunks > 0xffffffffull) return false;
        const uint32_t words = (uint32_t)((chunks + 31) / 32);
        if (words > capacity_) {
            delete[] bits_;
            bits_ = new uint32_t[words ? words : 1];
            capacity_ = bits_ ? words : 0;
            if (!bits_) {
                bytes_ = 0;
                chunks_ = 0;
                return false;
            }
        }
        bytes_ = bytes;
        chunks_ = (uint32_t)chunks;
        for (uint32_t i = 0; i < words; i++) bits_[i] = 0;
        setCount(0);
        return true;
    }

    // init(bytes) が new で確保する大きさ (バイト。32 チャンクごとの語に切り上げる)
    static uint64_t allocationBytes(uint64_t bytes) {
        const uint64_t chunks = (bytes + CHUNK - 1) >> CHUNK_SHIFT;
        const uint64_t words = (chunks + 31) / 32;
        return (words ? words : 1) * sizeof(uint32_t);
    }

    uint64_t bytes() const { return bytes_; }
    uint32_t chunks() const { return chunks_; }
    // チャンク c のバイト数 (最後のチャンクは短いことがある)
    uint32_t chunkBytes(uint32_t c) const {
        const uint64_t start = (uint64_t)c << CHUNK_SHIFT;
        if (start >= bytes_) return 0;
        return bytes_ - start < CHUNK ? (uint32_t)(bytes_ - start) : CHUNK;
    }

    // イメージの off バイト目を書いた (X68000 を動かしているスレッド。1 バイトごとに呼んでよい)
    void mark(uint64_t off) {
        bumpWrites();
        if (off >= bytes_) return;
        const uint32_t c = (uint32_t)(off >> CHUNK_SHIFT);
        uint32_t& w = bits_[c >> 5];
        const uint32_t m = 1u << (c & 31);
        if (!(w & m)) {
            w |= m;
            setCount(count_ + 1);
        }
    }
    // [off, off + len) を書いた
    void markRange(uint64_t off, uint64_t len) {
        bumpWrites();
        if (len == 0 || off >= bytes_) return;
        if (len > bytes_ - off) len = bytes_ - off;
        const uint32_t first = (uint32_t)(off >> CHUNK_SHIFT), last = (uint32_t)((off + len - 1) >> CHUNK_SHIFT);
        uint32_t n = count_;
        for (uint32_t c = first; c <= last; c++) {
            uint32_t& w = bits_[c >> 5];
            const uint32_t m = 1u << (c & 31);
            if (!(w & m)) {
                w |= m;
                n++;
            }
        }
        setCount(n);
    }

    bool test(uint32_t c) const { return c < chunks_ && (bits_[c >> 5] >> (c & 31) & 1); }
    // チャンク c の dirty フラグを下ろす
    void clear(uint32_t c) {
        if (!test(c)) return;
        bits_[c >> 5] &= ~(1u << (c & 31));
        setCount(count_ - 1);
    }
    void clearAll() {
        for (uint32_t i = 0; i < (chunks_ + 31) / 32; i++) bits_[i] = 0;
        setCount(0);
    }
    // from 以上で最初の dirty フラグの立ったチャンク。なければ -1
    int64_t next(uint32_t from) const {
        if (count_ == 0) return -1;
        for (uint32_t c = from; c < chunks_;) {
            const uint32_t w = bits_[c >> 5] >> (c & 31);
            if (w == 0) {
                c = (c | 31) + 1;
                continue;
            }
            uint32_t k = 0;
            while (!(w >> k & 1)) k++;
            return c + k < chunks_ ? (int64_t)(c + k) : -1;
        }
        return -1;
    }

    // ---- ほかのスレッドからも読める ----
    // dirty フラグの立ったチャンクの数
    uint32_t count() const { return __atomic_load_n(&count_, __ATOMIC_RELAXED); }
    bool any() const { return count() != 0; }
    // 書き込みの回数 (一周する)。コア 0 が、しばらく書かれていないかを見るのに使う
    uint32_t writes() const { return __atomic_load_n(&writes_, __ATOMIC_RELAXED); }

private:
    void setCount(uint32_t n) { __atomic_store_n(&count_, n, __ATOMIC_RELAXED); }
    void bumpWrites() { __atomic_store_n(&writes_, writes_ + 1, __ATOMIC_RELAXED); }

    uint32_t* bits_ = nullptr;
    uint32_t capacity_ = 0; // bits_ の語の数
    uint64_t bytes_ = 0;
    uint32_t chunks_ = 0;
    uint32_t count_ = 0;  // 書くのは dirty フラグを立てるスレッドだけ
    uint32_t writes_ = 0; // 同じ
};

// 書き戻すディスクのイメージ (FloppyImage と HardDiskImage)。platform/disk_sync.h が使う
class DirtySource {
public:
    virtual DirtyMap& dirtyMap() = 0;
    // チャンク c の中身 (dirty フラグの立ったチャンクは必ず読める)。len にバイト数を入れる。
    // X68000 を動かしているスレッドか、止めている間のコア 0 から呼ぶ
    virtual const uint8_t* chunkData(uint32_t c, uint32_t& len) = 0;
    // チャンク c を書き戻した (ファイルに書き終えた)。必要に応じて受け取る (読み込みのキャッシュが、
    // 書き戻すまでチャンクを捨てないようにする)
    virtual void chunkWritten(uint32_t c) { (void)c; }
    // 書き戻し中 (コア 0 に渡したが、まだ書き終えていない)。chunkWritten() で終わる
    virtual void chunkInFlight(uint32_t c) { (void)c; }

protected:
    ~DirtySource() {}
};

} // namespace x68k
