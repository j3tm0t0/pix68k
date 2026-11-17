// ディスクのイメージの書き戻し (ベアメタル版)。X68000 が書いたチャンク (x68k/dirty_map.h) を、
// コア 1 (X68000) からコア 0 (SD カード) に渡し、コア 0 がイメージのファイルに書く。
//
// 受け渡し (DiskChannel):
//   コア 1 はフレームの切れ目で serviceCore1() を呼ぶ。コア 0 が書き戻しを頼んでいて (requestFlush())、
//   受け渡しのバッファが空いていれば、 dirty フラグの立ったチャンクを STAGE_CHUNKS 個までバッファに写して dirty フラグを下ろし、
//   バッファをコア 0 に渡す (state_ を FULL にする。release)。
//   コア 0 は FULL を見たら (acquire) ファイルに書き、書けたら返す (RETURNED。release)。書けなければ返さずに
//   間を空けて書き直す (dirty フラグはもう下りているが、写しはバッファに残っている)。
//   書き直すまでの間と、続けて失敗した回数は、ディスクごとに持つ。バッファには何台かのディスクのチャンクが混じるので、
//   書けないディスクがあっても、ほかのディスクのチャンクは書く。ただしバッファは 1 つなので、書けないディスクの
//   チャンクが残っている間はバッファを返せず、ほかのディスクの次のチャンクは写せない (書けるようになるまで待つ)。
//   コア 1 は RETURNED を見たら、書き終えたチャンクを DirtySource に知らせてから (chunkWritten())、次を写す。
//   書いたチャンクをコア 1 が写したあとに X68000 がまた書けば、dirty フラグがまた立ち、次のバッファで書く。
// SRAM の書き戻し (emulator.h) と同じく、コア 1 はファイルにもヒープにも触らず、コア 0 はコア 1 が動いている間は
// イメージのバイト列を読まない (写しだけを読む)。X68000 を止めている間 (設定メニュー) は、コア 0 がコア 1 の
// 代わりに serviceWhilePaused() を呼んでよい。
//
// 書く時期 (DiskSync::service()、コア 0 の主ループ):
//   - dirty フラグの立ったディスクに IDLE_MS の間書き込みがない (Human68k がひと続きの書き込みを終えた)
//   - 最初に dirty フラグが立ってから MAX_AGE_MS たった (書き続けるソフトでも、間を空けて書く)
//   - 読み込みのキャッシュが、書き戻すまで捨てられないチャンクでいっぱいになった (pressure)
//   - 設定メニューでディスクの取り出し、リセット、Restart、設定やステートの保存をするとき (flushPaused())
//
// 書き方:
//   - ハードディスク (HDF、HDS): ファイルの中の位置に、チャンクをそのまま上書きする。ファイルの大きさは変えない
//     (クラスタの並びと FAT は変わらない)。1 回分 (バッファ 1 つ) を書くたびに sync する。
//     数百 MB のイメージを毎回 .new に写して置き換えるのは時間がかかりすぎるため。電源が切れると、書いていた
//     チャンクだけが新旧混じることがある (実機のハードディスクで書き込みの途中に電源が切れたときと同じ程度)
//   - フロッピーディスク (XDF、DIM、D88): コア 0 がファイルの写し (shadow) を持ち、チャンクを写しに当て、
//     dirty フラグがすべて下りたら写し全体を platform/file_replace.h で .new に書いてから置き換える。1.2MB ほどなので
//     時間はかからず、電源が切れても前の内容か新しい内容のどちらかが残る
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stdint.h>
#include <string.h>

#include "platform/file_replace.h"
#include "x68k/dirty_map.h"
#include "x68k/disk_cache.h"

namespace platform {

class DiskChannel {
public:
    // ディスクの番号: 0-1 フロッピーディスク、2 SASI ID 0、3-9 SCSI ID 0-6
    static constexpr int MAX_DISKS = 10;
    static constexpr uint32_t CHUNK = x68k::DirtyMap::CHUNK;
    // 1 回に写すチャンクの数。コア 1 がフレームの切れ目で写す量 (64KB、Pi 3 で 50us ほど)
    static constexpr int STAGE_CHUNKS = 4;
    // 全体がメモリに入るイメージで、まだ読んでいないチャンクを 1 フレームに頼む数 (DiskCache::fillAhead())。
    // 128KB。コア 0 が次のフレームまでに読み終えていれば、1 秒に 7MB ほど進む (55 フレーム / 秒)
    static constexpr unsigned FILL_CHUNKS = 8;
    static constexpr uint8_t NO_DISK = 0xff; // 外したディスクのレコード (書かない)
    struct Record {
        uint8_t disk;
        uint32_t chunk;
        uint32_t len;
    };

    // ---- コア 0 (コア 1 を動かす前か、止めている間) ----
    void setSource(int disk, x68k::DirtySource* src) {
        if (disk < 0 || disk >= MAX_DISKS) return;
        sources_[disk] = src;
        scan_[disk] = 0;
    }
    x68k::DirtySource* source(int disk) const { return disk >= 0 && disk < MAX_DISKS ? sources_[disk] : nullptr; }
    // 読み込みのキャッシュ。全体がメモリに入るものは、コア 1 がフレームの切れ目で残りのチャンクを頼む
    void setCache(int disk, x68k::DiskCache* cache) {
        if (disk >= 0 && disk < MAX_DISKS) caches_[disk] = cache;
    }

    // ---- コア 1 (フレームの切れ目) ----
    void serviceCore1() {
        service(-1, false);
        for (x68k::DiskCache* c : caches_)
            if (c) c->fillAhead(FILL_CHUNKS);
    }

    // ---- コア 0 ----
    // 書き戻しを頼む / やめる
    void requestFlush(bool on) { __atomic_store_n(&flushReq_, on ? 1u : 0u, __ATOMIC_RELEASE); }
    bool flushRequested() const { return __atomic_load_n(&flushReq_, __ATOMIC_RELAXED) != 0; }
    // コア 1 が写したバッファがあるか (あれば records() 以下を読んでよい)
    bool full() const { return __atomic_load_n(&state_, __ATOMIC_ACQUIRE) == FULL; }
    int records() const { return n_; }
    const Record& record(int i) const { return rec_[i]; }
    const uint8_t* data(int i) const { return stage_[i]; }
    // バッファを書き終えた。コア 1 に返す
    void returnBatch() { __atomic_store_n(&state_, (unsigned)RETURNED, __ATOMIC_RELEASE); }
    // 止めている間か、コア 1 を動かす前: バッファに残っている disk のレコードを書かないようにする (ディスクを外したとき。
    // 同じ番号にあとでつないだ別のディスクに、前のディスクのチャンクを書かないように)
    void dropDisk(int disk) {
        for (int i = 0; i < n_; i++)
            if (rec_[i].disk == disk) rec_[i].disk = NO_DISK;
    }
    // 止めている間: コア 1 の代わりに、返したバッファを片付けて次を写す。only >= 0 ならそのディスクだけ写す
    void serviceWhilePaused(int only = -1) { service(only, true); }

    // 読み込みのキャッシュが、書き戻すまで捨てられないチャンクでいっぱいになった (コア 1 が知らせ、コア 0 が消す)
    void signalPressure() { __atomic_store_n(&pressure_, 1u, __ATOMIC_RELEASE); }
    bool takePressure() { return __atomic_exchange_n(&pressure_, 0u, __ATOMIC_ACQ_REL) != 0; }

private:
    enum : unsigned { EMPTY = 0, FULL = 1, RETURNED = 2 };

    void service(int only, bool force) {
        unsigned st = __atomic_load_n(&state_, __ATOMIC_ACQUIRE);
        if (st == RETURNED) {
            for (int i = 0; i < n_; i++)
                if (rec_[i].disk != NO_DISK)
                    if (x68k::DirtySource* s = sources_[rec_[i].disk]) s->chunkWritten(rec_[i].chunk);
            n_ = 0;
            __atomic_store_n(&state_, (unsigned)EMPTY, __ATOMIC_RELAXED);
            st = EMPTY;
        }
        if (st != EMPTY) return;
        if (!force && !__atomic_load_n(&flushReq_, __ATOMIC_ACQUIRE)) return;
        fill(only);
        if (n_ > 0) __atomic_store_n(&state_, (unsigned)FULL, __ATOMIC_RELEASE);
    }

    void fill(int only) {
        for (int k = 0; k < MAX_DISKS && n_ < STAGE_CHUNKS; k++) {
            const int d = only >= 0 ? only : (next_ + k) % MAX_DISKS;
            if (only >= 0 && k > 0) break;
            x68k::DirtySource* src = sources_[d];
            if (!src) continue;
            x68k::DirtyMap& map = src->dirtyMap();
            while (n_ < STAGE_CHUNKS && map.count() != 0) {
                int64_t c = map.next(scan_[d]);
                if (c < 0) c = map.next(0); // 前に見た位置より前に dirty フラグが立った
                if (c < 0) break;
                uint32_t len = 0;
                const uint8_t* p = src->chunkData((uint32_t)c, len);
                map.clear((uint32_t)c);
                scan_[d] = (uint32_t)c + 1;
                if (!p || len == 0) continue;
                memcpy(stage_[n_], p, len);
                rec_[n_].disk = (uint8_t)d;
                rec_[n_].chunk = (uint32_t)c;
                rec_[n_].len = len;
                n_++;
                src->chunkInFlight((uint32_t)c);
            }
        }
        if (only < 0) next_ = (next_ + 1) % MAX_DISKS; // ディスクを順に先にする (1 つのディスクが占めないように)
    }

    x68k::DirtySource* sources_[MAX_DISKS] = {};
    x68k::DiskCache* caches_[MAX_DISKS] = {};
    uint32_t scan_[MAX_DISKS] = {}; // 次に探し始めるチャンク (コア 1)
    int next_ = 0;
    uint8_t stage_[STAGE_CHUNKS][CHUNK];
    Record rec_[STAGE_CHUNKS] = {};
    int n_ = 0;
    unsigned state_ = EMPTY;
    unsigned flushReq_ = 0;
    unsigned pressure_ = 0;
};

// コア 0 の側。Fs はファイルの操作 (ベアメタル版は FatFs、試験は作り物):
//   typedef ... File;                                         開いたままのイメージのファイル
//   bool readAt(File*, uint64_t off, void* buf, uint32_t len)  読む (読み込みのキャッシュ)
//   bool writeAt(File*, uint64_t off, const void* p, uint32_t len)  上書きする
//   bool sync(File*)
//   void recover(File*)                                       読み書きに失敗したあとに呼ぶ。開き直す (FatFs は f_read /
//                                                             f_write / f_lseek のエラーを FIL に残し、以後の読み書きが
//                                                             すべて失敗するため)
//   exists / write / remove / rename                           platform/file_replace.h のもの
//   void report(int disk, DiskEvent e, uint32_t value)         ログ用
enum class DiskEvent {
    WRITTEN,       // 書き戻しが一区切りついた。value バイトを書いた (フロッピーディスクは写しに当てたバイト数)
    WRITE_FAILED,  // 書けなかった。間を空けて書き直す
    REPLACED,      // フロッピーディスクのファイルを置き換えた (value バイト)
    REPLACE_FAILED,
    RECOVERED,     // 書けなかったあとで書けた (そのディスクが。value は続けて書けなかった回数)
    READ_FAILED,   // 読み込みのキャッシュのチャンク (value) を読めなかった。続けて失敗したときは、1 回目と 16 回ごとに出す
    READ_RECOVERED, // 読めなかったあとで読めた。value は続けて読めなかった回数
};

template <class Fs>
class DiskSync {
public:
    static constexpr uint32_t IDLE_MS = 2000;     // この間書き込みがなければ書く
    static constexpr uint32_t MAX_AGE_MS = 30000; // 書き続けていても、この間隔では書く
    // 書けなかったときに書き直すまでの間 (そのディスクの失敗が続くと 2 倍ずつ、最大 30 秒)
    static constexpr uint32_t RETRY_MS = 1000;

    DiskSync(Fs& fs, DiskChannel& ch) : fs_(fs), ch_(ch) {}

    // ---- コア 1 を動かす前か、止めている間 ----
    // フロッピーディスクをつなぐ。shadow (size バイト) はファイルの今の中身の写し (呼んだ側が用意し、
    // 外すまで持つ)。path と tmp は外すまで持つこと
    void addFloppy(int disk, x68k::DirtySource* src, const char* path, const char* tmp, uint8_t* shadow, uint32_t size) {
        Disk& d = disks_[disk];
        d = Disk();
        d.kind = FLOPPY;
        d.path = path;
        d.tmp = tmp;
        d.shadow = shadow;
        d.size = size;
        d.lastWrites = src->dirtyMap().writes();
        ch_.setSource(disk, src);
    }
    // ハードディスクをつなぐ。file は外すまで開いておく。src が nullptr なら書き戻さない (読み込みのキャッシュのためだけ)。
    // cache があれば、コア 1 が頼んだチャンクを読む (x68k/disk_cache.h)
    void addHard(int disk, x68k::DirtySource* src, typename Fs::File* file, x68k::DiskCache* cache = nullptr) {
        Disk& d = disks_[disk];
        d = Disk();
        d.kind = src ? HARD : READER;
        d.file = file;
        d.cache = cache;
        if (src) d.lastWrites = src->dirtyMap().writes();
        ch_.setSource(disk, src);
        ch_.setCache(disk, cache);
    }
    // 外す (flushPaused() で書いてから。書いていない分は捨てる)
    // ディスクを外す (止めている間)。受け渡しのバッファに残っているそのディスクのチャンクは書かない
    void remove(int disk) {
        disks_[disk] = Disk();
        ch_.setSource(disk, nullptr);
        ch_.setCache(disk, nullptr);
        ch_.dropDisk(disk);
        pendingBytes_[disk] = 0;
        readFailures_[disk] = 0;
        failures_[disk] = 0;
        retryAt_[disk] = 0;
    }
    // つないである読み込みのキャッシュ (なければ nullptr)
    const x68k::DiskCache* cache(int disk) const { return disks_[disk].cache; }
    // 書き戻すディスクか
    bool attached(int disk) const { return disks_[disk].kind == FLOPPY || disks_[disk].kind == HARD; }

    // ---- コア 0 の主ループ ----
    // 読み込みのキャッシュに頼まれたチャンクを、全部のディスクで合わせて maxLoads 個まで読む。読んだ数を返す
    unsigned serviceLoads(unsigned maxLoads) {
        unsigned n = 0;
        for (int i = 0; i < DiskChannel::MAX_DISKS && n < maxLoads; i++) {
            Disk& d = disks_[i];
            if (!d.cache) continue;
            unsigned slot;
            uint32_t c, len;
            uint8_t* buf;
            while (n < maxLoads && d.cache->nextLoad(slot, c, buf, len)) {
                const bool ok = fs_.readAt(d.file, (uint64_t)c * DiskChannel::CHUNK, buf, len);
                if (!ok) {
                    if (readFailures_[i] % READ_REPORT_EVERY == 0) fs_.report(i, DiskEvent::READ_FAILED, c);
                    readFailures_[i]++;
                    fs_.recover(d.file); // 次の読み込みのために開き直す (FatFs はエラーを FIL に残す)
                } else if (readFailures_[i]) {
                    fs_.report(i, DiskEvent::READ_RECOVERED, readFailures_[i]);
                    readFailures_[i] = 0;
                }
                d.cache->loaded(slot, ok);
                loads_++;
                n++;
            }
        }
        return n;
    }

    // io が false なら SD カードには書かず、書くかどうかを決めるだけにする。コア 0 が画面を写す役を持ったまま
    // 呼ぶとき用で、ioPending() を見てから呼ぶまでの間にコア 1 が書き戻しの準備を進めても、ここでは書かない
    // (次の周回で ioPending() が true になり、写す役を任せてから書く)
    void service(uint64_t nowMs, bool io = true) {
        now_ = nowMs;
        if (io && ch_.full()) writeBatch(false);
        bool want = ch_.takePressure();
        bool pending = ch_.full();
        for (int i = 0; i < DiskChannel::MAX_DISKS; i++) {
            Disk& d = disks_[i];
            if (d.cache && d.cache->takePressure()) want = true;
            if (d.kind != FLOPPY && d.kind != HARD) continue;
            x68k::DirtySource* src = ch_.source(i);
            const uint32_t w = src->dirtyMap().writes(), c = src->dirtyMap().count();
            if (w != d.lastWrites) {
                d.lastWrites = w;
                d.lastWriteMs = nowMs;
                if (d.dirtySinceMs == 0) d.dirtySinceMs = nowMs ? nowMs : 1;
            }
            if (c != 0) {
                pending = true;
                if (nowMs - d.lastWriteMs >= IDLE_MS || (d.dirtySinceMs && nowMs - d.dirtySinceMs >= MAX_AGE_MS))
                    want = true;
            } else if (!staged(i)) {
                // dirty フラグがすべて下り、受け渡しのバッファにもこのディスクのチャンクが残っていない:
                // フロッピーディスクは写しをファイルにする
                if (io && d.shadowDirty && nowMs >= retryAt_[i]) writeShadow(i);
                if (!d.shadowDirty) d.dirtySinceMs = 0;
            }
            if (d.shadowDirty) pending = true;
        }
        if (want) ch_.requestFlush(true);
        else if (!pending && ch_.flushRequested()) {
            ch_.requestFlush(false);
            reportWritten();
        }
    }
    // 書き戻しの途中か
    bool busy() const { return ch_.flushRequested() || ch_.full(); }
    // 次の service() / serviceLoads() で SD カードを読み書きするかもしれない (書き戻しの途中、フロッピーディスクの
    // 写しを書いていない、読み込みを頼まれている)。コア 0 が長く止まる間、画面を写す役を任せるかを決めるのに使う
    bool ioPending() const {
        if (busy()) return true;
        for (int i = 0; i < DiskChannel::MAX_DISKS; i++) {
            const Disk& d = disks_[i];
            if (d.shadowDirty || (d.cache && d.cache->loadQueued())) return true;
        }
        return false;
    }
    uint64_t loads() const { return loads_; }

    // ---- 止めている間 ----
    // X68000 を止めている間 (設定メニュー) の主ループ: 書いていない分があれば、待たずに書く。
    // 書けなければ、動いているときと同じく、そのディスクだけ間を延ばしながら書き直す
    void servicePaused(uint64_t nowMs) {
        now_ = nowMs;
        for (int guard = 0; guard < 1 << 20; guard++) {
            if (ch_.full() && !writeBatch(false)) break;
            ch_.serviceWhilePaused();
            if (!ch_.full()) break;
        }
        bool idle = !ch_.full();
        for (int i = 0; i < DiskChannel::MAX_DISKS; i++) {
            Disk& d = disks_[i];
            if (d.kind != FLOPPY && d.kind != HARD) continue;
            if (ch_.source(i)->dirtyMap().count() != 0 || staged(i)) {
                idle = false;
                continue;
            }
            if (d.shadowDirty && (nowMs < retryAt_[i] || !writeShadow(i))) {
                idle = false;
                continue;
            }
            d.dirtySinceMs = 0;
        }
        if (idle) reportWritten();
    }
    // dirty フラグの立ったチャンクを書き、フロッピーディスクは写しをファイルにする。only >= 0 ならそのディスクだけ
    // (ほかのディスクのチャンクが受け渡しのバッファにあれば、それも書く)。書き直すまでの間は待たずに書く。
    // 書けなければ false (dirty フラグと写しは残る)
    bool flushPaused(int only = -1) {
        for (int guard = 0; guard < 1 << 20; guard++) {
            if (ch_.full() && !writeBatch(true)) return false;
            ch_.serviceWhilePaused(only);
            if (!ch_.full()) break;
        }
        for (int i = 0; i < DiskChannel::MAX_DISKS; i++) {
            if (only >= 0 && i != only) continue;
            Disk& d = disks_[i];
            if (d.kind != FLOPPY && d.kind != HARD) continue;
            if (d.shadowDirty && !writeShadow(i)) return false;
            if (ch_.source(i)->dirtyMap().count() != 0) return false;
            d.dirtySinceMs = 0;
        }
        reportWritten();
        return true;
    }
    // 続けて書けなかった回数 (ディスクごと)。disk を省くと、最も多いディスクの回数
    unsigned failures(int disk) const { return failures_[disk]; }
    unsigned failures() const {
        unsigned n = 0;
        for (int i = 0; i < DiskChannel::MAX_DISKS; i++)
            if (failures_[i] > n) n = failures_[i];
        return n;
    }
    uint64_t bytesWritten() const { return bytesWritten_; }

private:
    enum Kind { NONE, FLOPPY, HARD, READER };
    struct Disk {
        Kind kind = NONE;
        typename Fs::File* file = nullptr;
        x68k::DiskCache* cache = nullptr;
        const char* path = nullptr;
        const char* tmp = nullptr;
        uint8_t* shadow = nullptr;
        uint32_t size = 0;
        bool shadowDirty = false;
        uint32_t lastWrites = 0;
        uint64_t lastWriteMs = 0, dirtySinceMs = 0;
    };

    // 受け渡しのバッファに、書き終えていない disk のチャンクが残っているか
    bool staged(int disk) const {
        if (!ch_.full()) return false;
        for (int i = 0; i < ch_.records(); i++)
            if (!done_[i] && ch_.record(i).disk == disk) return true;
        return false;
    }

    // レコード i を書き終えた (同じレコードを 2 度数えない)
    void finishRecord(int i) {
        const DiskChannel::Record& r = ch_.record(i);
        done_[i] = true;
        if (r.disk == DiskChannel::NO_DISK) return; // 外したディスク
        pendingBytes_[r.disk] += r.len;
        bytesWritten_ += r.len;
    }

    // 受け渡しのバッファを書く。全部のレコードを書き終えたらコア 1 に返して true。
    // 書けなかったディスクと、書き直すまでの間を待っているディスク (force なら待たない) のレコードは残し、
    // ほかのディスクのレコードは書く。書き終えたレコードは done_ に覚えておき、次に呼ばれたときには書かない
    bool writeBatch(bool force) {
        bool touched[DiskChannel::MAX_DISKS] = {}; // 今回書いて、まだ sync していないディスク
        bool bad[DiskChannel::MAX_DISKS] = {};     // 今回書けなかったディスク
        bool wrote[DiskChannel::STAGE_CHUNKS] = {};
        for (int i = 0; i < ch_.records(); i++) {
            if (done_[i]) continue;
            const DiskChannel::Record& r = ch_.record(i);
            if (r.disk == DiskChannel::NO_DISK) {
                finishRecord(i);
                continue;
            }
            Disk& d = disks_[r.disk];
            const uint64_t off = (uint64_t)r.chunk * DiskChannel::CHUNK;
            if (d.kind != HARD) {
                // フロッピーディスクは写しに当てるだけ (失敗しない。ファイルにするのは writeShadow())
                if (d.kind == FLOPPY && off < d.size) {
                    memcpy(d.shadow + off, ch_.data(i), off + r.len <= d.size ? r.len : (uint32_t)(d.size - off));
                    d.shadowDirty = true;
                }
                finishRecord(i);
                continue;
            }
            if (bad[r.disk] || (!force && now_ < retryAt_[r.disk])) continue;
            if (!fs_.writeAt(d.file, off, ch_.data(i), r.len)) {
                fs_.recover(d.file); // FatFs はエラーを FIL に残すので、開き直してから書き直す
                fail(r.disk, DiskEvent::WRITE_FAILED);
                bad[r.disk] = true;
                continue;
            }
            touched[r.disk] = true;
            wrote[i] = true;
        }
        for (int i = 0; i < DiskChannel::MAX_DISKS; i++) {
            // 途中で書けなくなったディスクは sync しない (今回書いた分も残し、次にまとめて書き直す)
            if (!touched[i] || bad[i]) continue;
            if (fs_.sync(disks_[i].file)) {
                succeed(i);
            } else {
                fs_.recover(disks_[i].file);
                fail(i, DiskEvent::WRITE_FAILED);
                bad[i] = true;
            }
        }
        bool all = true;
        for (int i = 0; i < ch_.records(); i++) {
            if (wrote[i] && !bad[ch_.record(i).disk]) finishRecord(i);
            if (!done_[i]) all = false;
        }
        if (!all) return false;
        for (int i = 0; i < DiskChannel::STAGE_CHUNKS; i++) done_[i] = false;
        ch_.returnBatch();
        return true;
    }

    void reportWritten() {
        for (int i = 0; i < DiskChannel::MAX_DISKS; i++) {
            if (pendingBytes_[i] == 0) continue;
            fs_.report(i, DiskEvent::WRITTEN, pendingBytes_[i]);
            pendingBytes_[i] = 0;
        }
    }

    bool writeShadow(int i) {
        Disk& d = disks_[i];
        if (replaceFile(fs_, d.path, d.tmp, d.shadow, d.size) != FsResult::OK) {
            fail(i, DiskEvent::REPLACE_FAILED);
            return false;
        }
        d.shadowDirty = false;
        fs_.report(i, DiskEvent::REPLACED, d.size);
        succeed(i);
        return true;
    }

    // disk が書けなかった: そのディスクだけ、書き直すまでの間を延ばす
    void fail(int disk, DiskEvent e) {
        fs_.report(disk, e, failures_[disk]);
        uint64_t wait = RETRY_MS;
        for (unsigned k = 0; k < failures_[disk] && wait < 30000; k++) wait *= 2;
        if (wait > 30000) wait = 30000;
        retryAt_[disk] = now_ + wait;
        failures_[disk]++;
    }
    // disk が書けた (ハードディスクは sync まで、フロッピーディスクはファイルの置き換えまで)
    void succeed(int disk) {
        if (failures_[disk] != 0) fs_.report(disk, DiskEvent::RECOVERED, failures_[disk]);
        failures_[disk] = 0;
        retryAt_[disk] = 0;
    }

    Fs& fs_;
    DiskChannel& ch_;
    Disk disks_[DiskChannel::MAX_DISKS];
    uint64_t now_ = 0;
    uint64_t retryAt_[DiskChannel::MAX_DISKS] = {}; // ディスクごとの、次に書き直してよい時刻 (0 なら待っていない)
    unsigned failures_[DiskChannel::MAX_DISKS] = {}; // ディスクごとの、続けて書けなかった回数
    bool done_[DiskChannel::STAGE_CHUNKS] = {};      // 受け渡しのバッファのレコードのうち、書き終えたもの
    uint64_t bytesWritten_ = 0, loads_ = 0;
    static constexpr uint32_t READ_REPORT_EVERY = 16;
    uint32_t readFailures_[DiskChannel::MAX_DISKS] = {}; // ディスクごとの、続けて読めなかった回数
    uint32_t pendingBytes_[DiskChannel::MAX_DISKS] = {}; // 書き戻しの一区切りまでに書いたバイト数
};

} // namespace platform
