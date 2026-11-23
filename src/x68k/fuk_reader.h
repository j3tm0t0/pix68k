// FUK (x68k/fuk.h) につないだ FDD のディスクを、XDF のイメージに読み出す (#381)。
//
// 読むのは 2HD (77 シリンダ、2 面、8 セクタ、1 セクタ 1024 バイト) だけ。ほかの書式と読めないセクタは、読み直したあと、
// 位置を示して失敗する (欠けたイメージを成功として渡さない)。
// ディスクには何も書かない: コマンドは sendCommand() の 1 か所から送り、allowed() の一覧にあるものだけを通す。
// さらに、使っている間は Fuk が書き込みのコマンドを受け付けない (Fuk::beginReader())。排出 (ドライブ制御) も送らない。
// X68000 の FDC の操作と混ざらないよう、X68000 を止めている間 (か、動かす前) だけ使う。poll() は待たない
// (Fuk がステータスの返事を少し待つことはある)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "x68k/fuk.h"

namespace x68k {

class FukReader {
public:
    static constexpr int CYLINDERS = 77, HEADS = 2, SECTORS = 8, SECTOR_BYTES = 1024;
    static constexpr uint32_t TRACK_BYTES = SECTORS * SECTOR_BYTES;
    static constexpr uint32_t IMAGE_BYTES = CYLINDERS * HEADS * TRACK_BYTES;
    static constexpr int TRACKS = CYLINDERS * HEADS;
    static constexpr int RETRIES = 3;                   // トラック (とセクタ) を読む回数
    static constexpr uint32_t BUSY_US = 1000000;        // FUK の返事や送り残しを待つ時間 (X68000 が FDC を使っている途中なら、待たずに断る)
    static constexpr uint32_t READY_US = 5000000;       // ドライブがレディになるのを待つ時間
    static constexpr uint32_t SETTLE_US = 500000;       // モーターを戻したあと、レディの変化の割り込みを待つ時間
    static constexpr uint32_t STEP_US = 5000000;        // コマンド 1 つの時間切れ (Fuk の時間切れより長い)

    enum State : uint8_t { IDLE, RUNNING, DONE, FAILED };
    enum Error : uint8_t {
        NONE,
        BUSY,         // X68000 が FDC を使っている途中
        NO_DEVICE,    // FUK がつながっていない
        NOT_READY,    // ディスクが入っていない、レディにならない
        LINK_LOST,    // 途中で FUK が抜かれた、答えない、FDC がリセットされた
        DISK_CHANGED, // 途中でディスクが出し入れされた
        SEEK_FAILED,
        BAD_FORMAT,   // 2HD 1024 バイト × 8 セクタではない
        BAD_SECTOR,   // 読めないセクタ
        TIMED_OUT,
        CANCELLED,
    };
    struct Result {
        Error error = NONE;
        int c = -1, h = -1, r = -1; // 失敗した位置 (-1 は決まらない)
        uint8_t st[7] = {};         // そのときの FDC のリザルト (ST0、ST1、ST2、C、H、R、N)
        bool hasStatus = false;
        bool restored = false;      // ヘッドとモーターを、始める前の状態に戻せた
    };

    explicit FukReader(Fuk& fuk) : fuk_(fuk) {}
    ~FukReader() { fuk_.endReader(head()); }

    // 始める。image は IMAGE_BYTES バイト (終わるまで呼び手が持つ)。specify は、X68000 を動かさずに使うとき true
    // (SPECIFY を送る。X68000 を動かしているときは、IPL が送った値を変えないように送らない)
    void start(int unit, uint8_t* image, bool specify);
    // 進める。RUNNING の間、短い間隔で呼ぶ
    State poll();
    // やめる。実行中のコマンドが終わってから、ヘッドとモーターを戻して FAILED (CANCELLED) になる
    void cancel() { cancel_ = true; }
    State state() const { return state_; }
    int tracksDone() const { return tracks_; }
    unsigned retries() const { return retryCount_; }
    const Result& result() const { return result_; }
    // 結果を英文で書く (改行なし)
    void describe(char* out, size_t size) const;
    // 送ってよいコマンド (1 バイト目) か。ディスクに書くものはない
    static bool allowed(uint8_t command);

private:
    enum Step : uint8_t { WAIT_IDLE, SPECIFY, READY, DRAIN, RECAL, SEEK, READ_ID, READ, RESTORE, RESTORE_RECAL, MOTOR, SETTLE, SETTLE_SENSE };

    void enter(Step s);
    void fail(Error e, int r = -1);
    void finish();
    bool sendCommand(const uint8_t* bytes, int n);
    int stepCommand(const uint8_t* bytes, int n, int resultBytes);
    int stepSeek(const uint8_t* bytes, int n);
    void pollReady();
    void pollReadId();
    void pollRead();
    void retry(Error e, int r);
    Fuk::ReaderHead head() const {
        return !moved_ ? Fuk::ReaderHead::UNMOVED : restoredHead_ ? Fuk::ReaderHead::RESTORED : Fuk::ReaderHead::LOST;
    }
    uint8_t headUnit() const { return (uint8_t)(h_ << 2 | unit_); }

    Fuk& fuk_;
    State state_ = IDLE;
    Step step_ = WAIT_IDLE;
    uint8_t unit_ = 0;
    uint8_t* image_ = nullptr;
    bool specify_ = false, cancel_ = false, began_ = false, touched_ = false;
    bool moved_ = false, restoredHead_ = false; // ヘッドを動かすコマンドを送った、戻せた
    Result result_;
    unsigned resetCount_ = 0, fddEvents_ = 0, retryCount_ = 0;
    uint64_t stepSince_ = 0, commandSince_ = 0, readySince_ = 0;
    int tracks_ = 0;
    int c_ = 0, h_ = 0;
    int first_ = 1, last_ = SECTORS; // いま読むセクタの範囲 (1 セクタずつ読むときは同じ)
    int tries_ = 0, restoreTries_ = 0;
    uint64_t settleSince_ = 0;
    bool settleWait_ = false;
    // コマンド
    uint8_t sub_ = 0;                // 0 送る前、1 リザルト待ち、2 SEEK の終わり待ち、3 SENSE INTERRUPT STATUS のリザルト待ち、
                                     // 4 SENSE INTERRUPT STATUS を送る前
    uint8_t res_[7] = {};
    int resGot_ = 0;
    uint32_t dataGot_ = 0;
    bool tooLong_ = false;
};

} // namespace x68k
