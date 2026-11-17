// MIDI ボードが送ったバイトを、UART (31.25kbps) から実機の MIDI 音源に送る (midi_out=uart)。
// ここは機種によらない部分: バイトを溜めるリング、UART に送る速さの配分、鳴っている音を止めるバイトの差し込み。
// UART に 1 バイト書く put() と、今の時刻を返す nowUs() は、使う側 (ベアメタル版は baremetal/kernel.h) が作る。
//
// 速さの配分: UART の送信 FIFO にバイトが何個残っているかを時刻から計算し (1 バイト 320us)、fifoBytes 個までしか
// 入れない。FIFO が満杯になる前にやめるので、put() は待たない。入らなかったバイトはリング (RING_SIZE 個) に残し、
// 次に pump() を呼んだときに送る。MIDI ボード (YM3802) も 31.25kbps で送るので、ふだんは溜まった量は増え続けない。
// 増えるのは、送り直し (x68k/midi_shadow.h の emit()。最大 6.4KB ほど) と、記録をまとめて処理したとき。
// リングがあふれたら、そのバイトは捨てて数える (dropped())。待たない。捨てたあとは、次のステータスのバイトまで
// データのバイトも捨てる (メッセージの途中から送らないため)。システムエクスクルーシブの途中で捨てたときは、その $F7 も
// 捨てる (中身の欠けたものを $F7 で閉じて送らないため)。音源には $F0 と途中までのデータが届いたままになり、次の
// ステータスのバイトで終わる ($F7 のないシステムエクスクルーシブとして捨てる音源が多い。Roland の音源はチェックサムが
// 合わなければ受けない)。
//
// 鳴っている音を止める (midiReset() と requestSilence()): 16 のチャネルに、gs ではオールサウンドオフ (CC 120) と
// オールノートオフ (CC 123)、mt32 ではホールドペダルを離す (CC 64 = 0) とオールノートオフを送る (MT-32 は CC 120 を
// 受けず、ホールドペダルを踏んだままではオールノートオフで音が止まらないため)。
//   - システムエクスクルーシブの途中なら、先に $F7 で閉じ、残りのデータのバイトは次のステータスのバイトまで捨てる
//   - チャネルメッセージの途中なら、止めるバイトのあとに、そのステータスと受けたデータのバイトを送り直す
//     (ランニングステータスを変えないため)
//   - 前に止めてからチャネルメッセージを 1 つも送っていなければ、何も送らない (メニューを開くたびに 80 バイト送らない)
// midiReset() は、それまでに受けたバイトの後ろに並べる (リセットやステートを読んだときは順番が大事)。
// requestSilence() は、ほかのコアから呼んでよい (一時停止、再起動の前)。頼んだときにリングに入っていたバイト (リングの
// 書き込み位置を区切りとして覚える) を送り終えたところで差し込む (先に止めると、溜まっていたノートオンがそのあとに
// 届いて鳴り続けるため)。頼んだあとに入ったバイト (メニューを閉じて再開したあとの演奏) は、止めるバイトのあとに送る。
// 期限 (deadlineUs) を付けたときは、区切りまでのバイトと止めるバイトを期限までに送り終えられなければ、区切りまでの
// バイトを読み手の側で捨てて (flushed())、止めるバイトを先に送る (再起動の前。31.25kbps で 300ms に送れるのは
// 937 バイトなので、止めるバイト (最大 84) を除いて 850 バイトほどより多く溜まっていれば捨てる。捨てたあとに残る途中の
// メッセージは、システムエクスクルーシブなら $F7 で閉じ、チャネルメッセージなら送り直さずに捨てる)。
// 読み手は、頼みの番号、区切り、リングの書き込み位置の順に acquire で読む (頼む側は書き込み位置を読んでから区切りを、
// 区切りを書いてから番号を release で書くので、番号が見えれば区切りまでのバイトも見える)。
// 頼みには世代の番号を付ける: requestSilence() が返した番号を silenced() に渡すと、その頼み (とそれより前の頼み) の
// 止めるバイトを FIFO に入れ終えたかが分かる。入れ終えたことは、最後のバイトを put() して drainedAt() を更新したあとに
// 見せるので、silenced() が true になったあとの drainedAt() を過ぎれば、止めるバイトは UART から出終えている。
//
// コアの分け方: 書き手 (midiByte() / midiReset()) は 1 つのコア、読み手 (pump()) も 1 つのコアで、同じでも違ってもよい
// (ロックのないリング)。setPumper(SELF) なら midiByte() と idleWork() の中で pump() を呼ぶ (書き手と読み手が同じコア)。
// idleWork() は、送るものが残っている間は true を返す (合成のコアが眠らずに呼び続け、FIFO が空になる前に次を入れる)。
// OTHER なら、ほかのコアが pump() を呼ぶ。NONE (初め) なら誰も送らず、リングに溜める。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stdint.h>

#include "x68k/midi_out.h"

namespace platform {

class UartMidi : public x68k::MidiOut {
public:
    static constexpr uint32_t BYTE_US = 320;   // 31.25kbps で 1 バイト (スタートビット、8 ビット、ストップビット)
    static constexpr uint32_t RING_SIZE = 16384; // 2 のべき乗
    static constexpr int DEFAULT_FIFO = 12;      // PL011 の FIFO は 16 バイト。時刻のずれの分だけ少なくする
    enum Pumper { NONE, SELF, OTHER };

    // fifoBytes が 0 以下なら速さを配分しない (pump() で溜まった分をすべて送る。ホスト版でファイルに書くとき)
    explicit UartMidi(Family family = GENERAL, int fifoBytes = DEFAULT_FIFO) : family_(family), fifo_(fifoBytes) {}

    // ---- 書き手 ----
    void midiByte(uint8_t b) override {
        // 捨てている間は、データと、システムエクスクルーシブを閉じる $F7 も捨てる。$F7 を捨てても、音源の側では
        // システムエクスクルーシブが開いたまま (欠けたまま) なので、次のステータスのバイトがリングに入るまで捨て続ける
        // (そのステータスで音源の側のシステムエクスクルーシブが終わる)
        if (dropping_ && (b < 0x80 || (b == 0xf7 && (inSysex_w_ || dropSysex_)))) {
            countDrop();
            if (b == 0xf7) dropSysex_ = false;
        } else if (push(b)) {
            if (b >= 0x80 && b < 0xf8) {
                dropping_ = false;
                dropSysex_ = false;
                inSysex_w_ = b == 0xf0;
            }
        } else {
            countDrop();
            if (b < 0xf8) {
                dropping_ = true;
                // 捨てたステータスは音源に届いていないので、inSysex_w_ (音源の側の状態) は変えない
                if (b == 0xf0) dropSysex_ = true; // そのシステムエクスクルーシブの $F7 も捨てる
            }
        }
        if (pumper() == SELF) pump(nowUs());
    }
    void midiReset() override {
        // リングが満杯なら、順番は守れないが、止めることだけは頼む
        if (!push(RESET_ENTRY)) (void)requestSilence();
        if (pumper() == SELF) pump(nowUs());
    }
    Family midiFamily() const override { return family_; }
    bool idleWork() override {
        if (pumper() != SELF) return false;
        return pump(nowUs()) != 0 || !idle();
    }

    // ---- どのコアからでも ----
    // 鳴っている音を止めるよう頼む。deadlineUs (0 なら期限なし) までに止めるバイトを UART から出し終えられるよう、
    // 間に合わなければ溜まったバイトを捨てる。返した番号を silenced() に渡す
    uint32_t requestSilence(uint64_t deadlineUs = 0) {
        // 区切り (今のリングの書き込み位置) と期限は番号より先に書く (読み手は番号を acquire で読んでから読む)。
        // 前の頼みをまだ済ませていなければ、区切りは新しいほうになる (前の頼みもそこで済ませる。二つのコアから同時に
        // 頼んでも先へ進んだほうを残す)。期限は近いほうを残す。
        // 区切りは release で書く: 読み手が区切りを acquire で読んでからリングの書き込み位置を読めば、区切りを書く前に
        // ここで読んだ位置 h より前の位置は読まない (読み手の位置 head >= 区切り。まだ書いていない所を読まない)。
        // 位置は 64 ビットなので、一周して比べ方が壊れることはない (最大の速さで 1 億年ほど)
        const uint64_t h = __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
        uint64_t m = __atomic_load_n(&silenceMark_, __ATOMIC_RELAXED);
        while (h > m && !__atomic_compare_exchange_n(&silenceMark_, &m, h, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
        }
        uint64_t d = __atomic_load_n(&silenceDeadline_, __ATOMIC_RELAXED);
        while (deadlineUs != 0 && (d == 0 || deadlineUs < d) &&
               !__atomic_compare_exchange_n(&silenceDeadline_, &d, deadlineUs, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
        }
        return __atomic_add_fetch(&silenceReq_, 1u, __ATOMIC_ACQ_REL);
    }
    // 番号 gen の頼みの止めるバイトを FIFO に入れ終えた
    bool silenced(uint32_t gen) const { return (int32_t)(__atomic_load_n(&silenceDone_, __ATOMIC_ACQUIRE) - gen) >= 0; }
    void setPumper(Pumper p) { __atomic_store_n(&pumper_, (unsigned)p, __ATOMIC_RELEASE); }
    Pumper pumper() const { return (Pumper)__atomic_load_n(&pumper_, __ATOMIC_ACQUIRE); }
    // リングに溜まっている数 (止めるために差し込むバイトは入らない)
    uint32_t backlog() const {
        return (uint32_t)(__atomic_load_n(&head_, __ATOMIC_ACQUIRE) - __atomic_load_n(&tail_, __ATOMIC_ACQUIRE));
    }
    // 送るものがない (止める頼みを済ませ、差し込むバイトもなく、リングも空)
    bool idle() const {
        const uint32_t req = __atomic_load_n(&silenceReq_, __ATOMIC_ACQUIRE);
        if ((int32_t)(__atomic_load_n(&silenceDone_, __ATOMIC_ACQUIRE) - req) < 0) return false;
        // リングの残りを先に読む (読み手は RESET_ENTRY を読むと injDone_ を false にしてから tail_ を書くので、
        // この順なら、差し込みを始めたところを空と見誤らない)
        if (backlog() != 0) return false;
        return __atomic_load_n(&injDone_, __ATOMIC_ACQUIRE);
    }
    // FIFO に入れたバイトを線に送り終える時刻 (us)。idle() になったあと、これを過ぎれば線も空いている
    uint64_t drainedAt() const { return __atomic_load_n(&busyUntil_, __ATOMIC_ACQUIRE); }
    uint32_t sent() const { return __atomic_load_n(&sent_, __ATOMIC_RELAXED); }
    uint32_t dropped() const { return __atomic_load_n(&dropped_, __ATOMIC_RELAXED); } // リングがあふれて捨てた
    uint32_t flushed() const { return __atomic_load_n(&flushed_, __ATOMIC_RELAXED); } // 期限に間に合わず捨てた
    uint32_t maxBacklog() const { return __atomic_load_n(&maxBacklog_, __ATOMIC_RELAXED); }
    uint32_t resets() const { return __atomic_load_n(&resets_, __ATOMIC_RELAXED); }

    // ---- 読み手 ----
    // 時刻 now (us) に、FIFO に入るだけ送る。送ったバイトの数を返す
    int pump(uint64_t now) {
        int room = space(now);
        int n = 0;
        uint64_t tail = tail_;
        // 番号、区切り、リングの書き込み位置の順に、どれも acquire で読む。頼む側は書き込み位置を読んでから区切りを、
        // 区切りを書いてから番号を release で書くので、番号が見えれば区切りも見え、区切りが見えれば書き込み位置は
        // 区切りより前にならない (区切りまでのバイトは書き終えている)
        const uint32_t req = __atomic_load_n(&silenceReq_, __ATOMIC_ACQUIRE);
        bool pending = req != silenceSeen_;
        uint64_t mark = tail;
        if (pending) mark = __atomic_load_n(&silenceMark_, __ATOMIC_ACQUIRE);
        betweenReads();
        const uint64_t head = __atomic_load_n(&head_, __ATOMIC_ACQUIRE);
        if (mark < tail) mark = tail; // もう送った位置 (前に済ませた頼みの区切り)
        if (mark > head) mark = head; // 上のとおり起きないが、まだ書いていない所は読まない
        if (pending && mark != tail) {
            // 期限のある頼み: 区切りまでのバイト、止めるバイト、FIFO に残っている分を期限までに送れなければ、
            // 区切りまでのバイトを捨てる
            const uint64_t deadline = __atomic_load_n(&silenceDeadline_, __ATOMIC_ACQUIRE);
            const uint64_t busy = busyUntil_ > now ? busyUntil_ - now : 0;
            const uint64_t need = busy + (mark - tail + (injLen_ - injPos_) + sizeof inj_) * BYTE_US;
            if (deadline != 0 && now + need > deadline) {
                __atomic_store_n(&flushed_, flushed_ + (uint32_t)(mark - tail), __ATOMIC_RELAXED);
                tail = mark;
                // 送っていたチャネルメッセージの続きは捨てたので、止めたあとに送り直さない (ランニングステータスも
                // 消し、次のステータスのバイトまでデータは捨てる)。システムエクスクルーシブとシステムコモンは閉じる
                status_ = 0;
                have_ = 0;
            }
        }
        while (room > 0) {
            uint8_t b;
            bool last = false; // 差し込むバイトの最後
            if (injPos_ < injLen_) {
                b = inj_[injPos_++];
                last = injPos_ == injLen_;
            } else if (pending && tail == mark) {
                // 区切りまで送った (requestSilence())。req までの頼みをまとめて済ませる
                pending = false;
                silenceSeen_ = req;
                // 期限を消す。req を読んだあとに別の頼みが期限を書いていたら、その期限も消える (その頼みは次の
                // pump() で期限なしとして扱う。今は期限つきで頼むのは再起動の前だけで、そのときほかの頼みは来ない)
                __atomic_store_n(&silenceDeadline_, 0, __ATOMIC_RELAXED);
                startReset();
                injGen_ = req;
                if (injLen_ == 0) finishInjection(); // 止めるものがない
                continue;
            } else if (tail != head) {
                const uint16_t e = ring_[tail & (RING_SIZE - 1)];
                tail++;
                if (e == RESET_ENTRY) {
                    startReset();
                    continue;
                }
                b = (uint8_t)e;
                if (!accept(b)) continue;
            } else {
                break;
            }
            put(b);
            __atomic_store_n(&busyUntil_, (busyUntil_ > now ? busyUntil_ : now) + BYTE_US, __ATOMIC_RELEASE);
            if (last) finishInjection(); // put() と busyUntil_ のあとに見せる
            room--;
            n++;
        }
        __atomic_store_n(&tail_, tail, __ATOMIC_RELEASE);
        if (n) __atomic_store_n(&sent_, sent_ + (uint32_t)n, __ATOMIC_RELAXED);
        return n;
    }

protected:
    // UART に 1 バイト書く (FIFO に空きがあるときだけ呼ぶ)
    virtual void put(uint8_t b) = 0;
    // 今の時刻 (us)。setPumper(SELF) のときに使う
    virtual uint64_t nowUs() = 0;
    // 試験用: pump() が頼みの番号を読んだあと、リングの書き込み位置を読む前に呼ぶ (読む順番の試験で、その間に書き手が
    // バイトと頼みを入れる)
    virtual void betweenReads() {}
    // 試験用: リングが空のときに、読み書きの位置を pos にする (長く動かしたあとを真似る)
    void setPositionForTest(uint64_t pos) { head_ = tail_ = pos; }

private:
    static constexpr uint16_t RESET_ENTRY = 0x100;

    bool push(uint16_t e) {
        const uint64_t head = head_;
        const uint32_t used = (uint32_t)(head - __atomic_load_n(&tail_, __ATOMIC_ACQUIRE));
        if (used >= RING_SIZE) return false;
        ring_[head & (RING_SIZE - 1)] = e;
        __atomic_store_n(&head_, head + 1, __ATOMIC_RELEASE);
        if (used + 1 > maxBacklog_) __atomic_store_n(&maxBacklog_, used + 1, __ATOMIC_RELAXED);
        return true;
    }
    void countDrop() { __atomic_store_n(&dropped_, dropped_ + 1, __ATOMIC_RELAXED); }
    // 差し込むバイトを FIFO に入れ終えた (止める頼みのものなら、その番号を済ませたことにする。midiReset() のものなら
    // injGen_ は済ませた番号のままなので変わらない)
    void finishInjection() {
        __atomic_store_n(&silenceDone_, injGen_, __ATOMIC_RELEASE);
        __atomic_store_n(&injDone_, true, __ATOMIC_RELEASE);
    }

    // FIFO にあと何バイト入れてよいか
    int space(uint64_t now) const {
        if (fifo_ <= 0) return 1 << 30;
        if (busyUntil_ <= now) return fifo_;
        const uint64_t left = (busyUntil_ - now + BYTE_US - 1) / BYTE_US; // まだ送り終えていないバイト
        return left >= (uint64_t)fifo_ ? 0 : fifo_ - (int)left;
    }

    // X68000 が送ったバイトを UART に送るか決め、受け取る側がどこまで読んだかを覚える
    bool accept(uint8_t b) {
        if (b >= 0xf8) return true; // リアルタイム
        if (b & 0x80) {
            if (skip_) {
                skip_ = false;
                if (b == 0xf7) return false; // 閉じたシステムエクスクルーシブの終わり
            }
            inSysex_ = b == 0xf0;
            common_ = 0;
            if (b >= 0xf0) {
                status_ = 0; // システムのメッセージはランニングステータスを消す
                if (b == 0xf2) common_ = 2;
                else if (b == 0xf1 || b == 0xf3) common_ = 1;
            } else {
                status_ = b;
                need_ = ((b & 0xe0) == 0xc0) ? 1 : 2; // $Cx と $Dx はデータが 1 バイト
                have_ = 0;
                voice_ = true;
            }
            return true;
        }
        if (skip_) return false;
        if (inSysex_) return true;
        if (common_ > 0) {
            common_--;
            return true;
        }
        // ステータスのないデータのバイトは捨てる (止めるバイトを送ったあと、受け取る側には $Bx が残っているため)
        if (status_ == 0) return false;
        data_[have_++] = b;
        if (have_ >= need_) have_ = 0;
        voice_ = true;
        return true;
    }

    // 鳴っている音を止めるバイトを作る (送るのは pump())
    void startReset() {
        int n = 0;
        if (inSysex_ || common_ > 0) {
            if (inSysex_) inj_[n++] = 0xf7;
            inSysex_ = false;
            common_ = 0;
            skip_ = true;
        }
        if (voice_) {
            for (uint8_t ch = 0; ch < 16; ch++) {
                inj_[n++] = (uint8_t)(0xb0 | ch);
                inj_[n++] = family_ == MT32 ? 64 : 120;
                inj_[n++] = 0;
                inj_[n++] = 123;
                inj_[n++] = 0;
            }
            if (status_ != 0) {
                inj_[n++] = status_;
                for (int i = 0; i < have_; i++) inj_[n++] = data_[i];
            }
            voice_ = false;
            __atomic_store_n(&resets_, resets_ + 1, __ATOMIC_RELAXED);
        }
        injPos_ = 0;
        injLen_ = n;
        injGen_ = silenceDone_; // midiReset() のものなら、済ませた番号は変えない (requestSilence() のものは呼び手が置き換える)
        __atomic_store_n(&injDone_, n == 0, __ATOMIC_RELEASE);
    }

    const Family family_;
    const int fifo_;
    unsigned pumper_ = NONE;
    // 止める頼み: 書くのは requestSilence() (silenceReq_、silenceDeadline_) と読み手 (silenceDone_、silenceSeen_)
    uint32_t silenceReq_ = 0, silenceDone_ = 0, silenceSeen_ = 0, injGen_ = 0;
    uint64_t silenceDeadline_ = 0;
    uint64_t silenceMark_ = 0; // 頼んだときのリングの書き込み位置 (ここまで送ったら止めるバイトを入れる)
    // リング (書き手が head_、読み手が tail_ を進める)
    uint16_t ring_[RING_SIZE];
    uint64_t head_ = 0, tail_ = 0; // 一周しないよう 64 ビット
    bool dropping_ = false; // 書き手: 捨てたあと、次のステータスのバイトまでデータを捨てる
    bool dropSysex_ = false; // 書き手: $F0 を捨てた (そのシステムエクスクルーシブの $F7 も捨てる)
    bool inSysex_w_ = false; // 書き手: 最後にリングに入れたステータスが $F0
    // 読み手の状態
    uint64_t busyUntil_ = 0; // FIFO に入れたバイトを送り終える時刻 (us)
    uint8_t status_ = 0, need_ = 0, have_ = 0, data_[2] = {};
    uint8_t common_ = 0; // システムコモンの残りのデータのバイト
    bool inSysex_ = false, skip_ = false;
    bool voice_ = true; // 前に止めてからチャネルメッセージを送った (初めは、前に何が鳴っていたか分からないので true)
    uint8_t inj_[1 + 16 * 5 + 3];
    int injPos_ = 0, injLen_ = 0;
    bool injDone_ = true;
    // 数 (読むのはほかのコア)
    uint32_t sent_ = 0, dropped_ = 0, flushed_ = 0, maxBacklog_ = 0, resets_ = 0;
};

} // namespace platform
