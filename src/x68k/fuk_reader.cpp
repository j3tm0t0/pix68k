#include "x68k/fuk_reader.h"

#include <string.h>

namespace x68k {

enum : uint8_t {
    OP_SPECIFY = 0x03,
    OP_SENSE_DEVICE = 0x04,
    OP_READ_DATA = 0x06,
    OP_RECALIBRATE = 0x07,
    OP_SENSE_INTERRUPT = 0x08,
    OP_READ_ID = 0x0a,
    OP_SEEK = 0x0f,
    MFM = 0x40,
};

bool FukReader::allowed(uint8_t command) {
    switch (command) {
    case OP_SPECIFY:
    case OP_SENSE_DEVICE:
    case OP_RECALIBRATE:
    case OP_SENSE_INTERRUPT:
    case OP_SEEK:
    case MFM | OP_READ_DATA:
    case MFM | OP_READ_ID:
        return true;
    default:
        return false;
    }
}

void FukReader::start(int unit, uint8_t* image, bool specify) {
    unit_ = (uint8_t)(unit & 3);
    image_ = image;
    specify_ = specify;
    cancel_ = began_ = touched_ = moved_ = restoredHead_ = false;
    result_ = Result();
    tracks_ = 0;
    retryCount_ = 0;
    c_ = h_ = 0;
    first_ = 1;
    last_ = SECTORS;
    tries_ = restoreTries_ = 0;
    state_ = RUNNING;
    enter(WAIT_IDLE);
}

void FukReader::enter(Step s) {
    step_ = s;
    sub_ = 0;
    stepSince_ = commandSince_ = fuk_.nowUs();
}

// 失敗した。ヘッドとモーターを戻してから終える (FUK が使えなくなったときは、戻せないのでそのまま終える)
void FukReader::fail(Error e, int r) {
    if (step_ >= RESTORE) { // 戻せなかった。読んだ結果は変えず、restored だけ false にする
        result_.restored = false;
        finish();
        return;
    }
    if (result_.error == NONE) {
        result_.error = e;
        if (e == BAD_SECTOR || e == BAD_FORMAT || e == SEEK_FAILED || (e == NOT_READY && (step_ == READ || step_ == READ_ID))) {
            result_.c = c_;
            if (e != SEEK_FAILED) result_.h = h_;
            result_.r = r;
            memcpy(result_.st, res_, sizeof result_.st);
            result_.hasStatus = e != SEEK_FAILED;
        }
    }
    if (!began_ || e == LINK_LOST || e == TIMED_OUT) finish();
    else if (moved_) enter(RESTORE);
    else { // ヘッドは動かしていない
        result_.restored = true;
        enter(MOTOR);
    }
}

void FukReader::finish() {
    // 最後のトラックを読み終えてから、ここまでの間に出し入れされた (戻すのに失敗したときも見る)
    if (began_ && result_.error == NONE && fuk_.fddEvents() != fddEvents_) result_.error = DISK_CHANGED;
    if (began_) fuk_.endReader(head());
    began_ = false;
    state_ = result_.error == NONE ? DONE : FAILED;
}

// コマンドはここからだけ送る
bool FukReader::sendCommand(const uint8_t* bytes, int n) {
    if (!allowed(bytes[0])) return false;
    for (int i = 0; i < n; i++) fuk_.write(3, bytes[i]);
    dataGot_ = 0;
    resGot_ = 0;
    tooLong_ = false;
    return true;
}

// コマンドを送り、リザルトを res_ に読む。終わったら 1、まだなら 0、送れなければ -1
int FukReader::stepCommand(const uint8_t* bytes, int n, int resultBytes) {
    const uint8_t msr = fuk_.read(1);
    if (sub_ == 0) {
        if ((msr & 0xd0) != 0x80) return 0; // コマンドを受け付けるのを待つ
        if (!sendCommand(bytes, n)) return -1;
        commandSince_ = fuk_.nowUs();
        sub_ = 1;
        return resultBytes == 0 ? 1 : 0;
    }
    if (fuk_.dataReady()) { // Read のデータ (リザルトより先に、全部届いてから渡される)
        const uint32_t want = (uint32_t)(last_ - first_ + 1) * SECTOR_BYTES;
        uint8_t* dst = image_ + (uint32_t)(c_ * HEADS + h_) * TRACK_BYTES + (uint32_t)(first_ - 1) * SECTOR_BYTES;
        while (fuk_.dataReady() && dataGot_ < want) {
            const int k = fuk_.readData(dst + dataGot_, (int)(want - dataGot_));
            if (k <= 0) break;
            dataGot_ += (uint32_t)k;
        }
        if (fuk_.dataReady()) { // 頼んだより多い
            tooLong_ = true;
            fuk_.terminalCount();
        }
        return 0;
    }
    // ステータスを読んだあとにリザルトがそろうことがあるので、読み直す
    while (resGot_ < resultBytes && (fuk_.read(1) & 0xf0) == 0xd0) res_[resGot_++] = fuk_.read(3);
    // SENSE INTERRUPT STATUS は、引き取る割り込みがなければ 1 バイトだけ
    if (bytes[0] == OP_SENSE_INTERRUPT && resGot_ == 1 && res_[0] == 0x80) return 1;
    return resGot_ >= resultBytes ? 1 : 0;
}

// SEEK / RECALIBRATE を送り、終わりの割り込みを引き取る。終わったら 1 (res_[0] が ST0、res_[1] がシリンダ)、
// まだなら 0、送れなければ -1
int FukReader::stepSeek(const uint8_t* bytes, int n) {
    static const uint8_t kSense[1] = {OP_SENSE_INTERRUPT};
    if (sub_ == 0) {
        const int k = stepCommand(bytes, n, 0);
        if (k == 1) {
            sub_ = 2;
            moved_ = true;
        }
        return k < 0 ? -1 : 0;
    }
    if (sub_ == 2) {
        if (!fuk_.irq()) return 0;
        sub_ = 4;
        return 0;
    }
    if (sub_ == 4) {
        if ((fuk_.read(1) & 0xd0) != 0x80) return 0;
        if (!sendCommand(kSense, 1)) return -1;
        sub_ = 3;
        return 0;
    }
    if (stepCommand(kSense, 1, 2) != 1) return 0;
    // ほかの割り込み (レディの変化など) なら、続けて待つ
    if (res_[0] == 0x80 || !(res_[0] & 0x20) || (res_[0] & 3) != unit_) {
        sub_ = 2;
        return 0;
    }
    return 1;
}

void FukReader::pollReady() {
    if (sub_ == 0 && fuk_.irq()) { // モーターを回して起きたレディの変化を、先に引き取る
        enter(DRAIN);
        return;
    }
    const uint8_t cmd[2] = {OP_SENSE_DEVICE, unit_};
    const int k = stepCommand(cmd, 2, 1);
    if (k == 0) return;
    if (k > 0 && (res_[0] & 0x20)) enter(RECAL);
    else if (fuk_.nowUs() - readySince_ >= READY_US) fail(NOT_READY);
    else enter(READY);
}

// 読み直す。ヘッドを動かし直してから (RECAL → SEEK)。だめなら、トラックをまとめて読むのをやめて 1 セクタずつ読み、
// それでもだめなら失敗する
void FukReader::retry(Error e, int r) {
    retryCount_++;
    if (++tries_ >= RETRIES) {
        if (e == BAD_SECTOR && step_ == READ && first_ != last_) {
            first_ = last_ = 1;
            tries_ = 0;
        } else {
            fail(e, r);
            return;
        }
    }
    enter(RECAL);
}

void FukReader::pollReadId() {
    const uint8_t cmd[2] = {MFM | OP_READ_ID, headUnit()};
    if (stepCommand(cmd, 2, 7) != 1) return;
    if (res_[0] & 0x08) fail(NOT_READY);
    else if ((res_[0] & 0xc0) != 0 || res_[3] != c_ || res_[4] != h_ || res_[6] != 3 || res_[5] < 1 || res_[5] > SECTORS)
        retry(BAD_FORMAT, -1);
    else enter(READ);
}

void FukReader::pollRead() {
    const uint8_t cmd[9] = {MFM | OP_READ_DATA, headUnit(),    (uint8_t)c_, (uint8_t)h_, (uint8_t)first_, 3,
                            (uint8_t)last_,     0x35,          0xff};
    if (stepCommand(cmd, 9, 7) != 1) return;
    if (res_[0] & 0x08) {
        fail(NOT_READY);
        return;
    }
    const bool normal = (res_[0] & 0xc0) == 0 && res_[1] == 0;
    // TC なしで EOT まで読み終えた FDC は、異常終了 (シリンダの終わり) と答える。データは全部読めている
    const bool endOfTrack = (res_[0] & 0xc0) == 0x40 && res_[1] == 0x80;
    const uint32_t want = (uint32_t)(last_ - first_ + 1) * SECTOR_BYTES;
    if (!(normal || endOfTrack) || res_[2] != 0 || tooLong_ || dataGot_ != want) {
        // どのセクタかは、1 セクタずつ読むときに決まる
        retry(BAD_SECTOR, first_);
        return;
    }
    tries_ = 0;
    if (first_ == last_ && last_ < SECTORS) {
        first_ = ++last_;
        enter(READ);
        return;
    }
    first_ = 1;
    last_ = SECTORS;
    tracks_++;
    if (tracks_ >= TRACKS) {
        enter(RESTORE);
        return;
    }
    if (++h_ >= HEADS) {
        h_ = 0;
        c_++;
        enter(SEEK);
    } else {
        enter(READ_ID);
    }
}

FukReader::State FukReader::poll() {
    if (state_ != RUNNING) return state_;
    // X68000 が FDC を使っている途中なら、Fuk を動かさずに断る (動かすと、X68000 が止まっているために DMAC が渡して
    // こない書き込みを、時間切れと取り違えてやめてしまう)
    if (!began_ && !cancel_ && fuk_.busyForReader()) {
        fail(BUSY);
        return state_;
    }
    fuk_.poll();
    const uint64_t t = fuk_.nowUs();
    if (!began_) {
        if (cancel_) fail(CANCELLED);
        else if (!fuk_.ready()) fail(NO_DEVICE);
        else if (fuk_.idleForReader()) {
            resetCount_ = fuk_.resetCount();
            fddEvents_ = fuk_.fddEvents();
            fuk_.beginReader(unit_);
            began_ = touched_ = true;
            readySince_ = t;
            enter(specify_ ? SPECIFY : READY);
        } else if (t - stepSince_ >= BUSY_US) fail(BUSY);
        return state_;
    }
    if (!fuk_.ready() || fuk_.resetCount() != resetCount_) {
        fail(LINK_LOST);
        return state_;
    }
    const bool restoring = step_ >= RESTORE;
    // コマンドの切れ目 (sub_ が 0) でだけ、やめる、ディスクの交換を見る
    if (!restoring && sub_ == 0) {
        if (fuk_.fddEvents() != fddEvents_) {
            fail(DISK_CHANGED);
            return state_;
        }
        if (cancel_) {
            fail(CANCELLED);
            return state_;
        }
    }
    if ((step_ != SETTLE && t - commandSince_ >= STEP_US) || (step_ == DRAIN && t - readySince_ >= READY_US)) {
        fail(TIMED_OUT);
        return state_;
    }
    switch (step_) {
    case SPECIFY: { // IPL と同じ値 (SRT 3 ms、HUT、HLT、DMA あり)
        const uint8_t cmd[3] = {OP_SPECIFY, 0xd0, 0x10};
        if (stepCommand(cmd, 3, 0) == 1) enter(READY);
        break;
    }
    case READY: pollReady(); break;
    case DRAIN: {
        const uint8_t cmd[1] = {OP_SENSE_INTERRUPT};
        if (stepCommand(cmd, 1, 2) == 1) enter(READY);
        break;
    }
    case RECAL: {
        const uint8_t cmd[2] = {OP_RECALIBRATE, unit_};
        if (stepSeek(cmd, 2) != 1) break;
        if (res_[0] & 0x08) fail(NOT_READY);
        else if ((res_[0] & 0xc0) != 0 || res_[1] != 0) {
            // 1 回の RECALIBRATE で届かないことがある (77 ステップまで)
            if (++tries_ >= RETRIES) fail(SEEK_FAILED);
            else enter(RECAL);
        } else enter(SEEK);
        break;
    }
    case SEEK: {
        const uint8_t cmd[3] = {OP_SEEK, headUnit(), (uint8_t)c_};
        if (stepSeek(cmd, 3) != 1) break;
        if (res_[0] & 0x08) fail(NOT_READY);
        else if ((res_[0] & 0xc0) != 0 || res_[1] != c_) retry(SEEK_FAILED, -1);
        else enter(READ_ID);
        break;
    }
    case READ_ID: pollReadId(); break;
    case READ: pollRead(); break;
    case RESTORE: { // X68000 が最後に頼んだシリンダに戻す。分からなければ 0。できなくても結果は変えない
        const int c = fuk_.readerCylinder();
        const uint8_t seek[3] = {OP_SEEK, unit_, (uint8_t)c};
        const uint8_t recal[2] = {OP_RECALIBRATE, unit_};
        if (stepSeek(c > 0 ? seek : recal, c > 0 ? 3 : 2) != 1) break;
        if ((res_[0] & 0xc8) == 0 && res_[1] == (c > 0 ? c : 0)) {
            result_.restored = restoredHead_ = true;
            enter(MOTOR);
        } else if (++restoreTries_ < RETRIES) {
            enter(RESTORE_RECAL);
        } else {
            enter(MOTOR);
        }
        break;
    }
    case RESTORE_RECAL: {
        const uint8_t recal[2] = {OP_RECALIBRATE, unit_};
        if (stepSeek(recal, 2) == 1) enter(RESTORE);
        break;
    }
    case MOTOR:
        // 戻したモーターで起きるレディの変化の割り込みを、X68000 に見せずに引き取る
        settleWait_ = fuk_.restoreReaderMotor();
        enter(SETTLE);
        settleSince_ = t;
        break;
    case SETTLE:
        if (fuk_.irq()) {
            if (t - settleSince_ < 2 * SETTLE_US) {
                enter(SETTLE_SENSE);
                break;
            }
            result_.restored = false; // 割り込みが下りない
        } else if (settleWait_ && t - settleSince_ < SETTLE_US) {
            break;
        }
        finish();
        break;
    case SETTLE_SENSE: {
        const uint8_t cmd[1] = {OP_SENSE_INTERRUPT};
        if (stepCommand(cmd, 1, 2) == 1) enter(SETTLE);
        break;
    }
    default: break;
    }
    return state_;
}

namespace {
// 入る分だけつなげる (ベアメタル版には snprintf() がない)
struct Text {
    char* out;
    size_t size, n = 0;
    Text(char* o, size_t s) : out(o), size(s) {
        if (size) out[0] = 0;
    }
    void put(char c) {
        if (n + 1 >= size) return;
        out[n++] = c;
        out[n] = 0;
    }
    void add(const char* s) {
        while (*s) put(*s++);
    }
    void dec(int v) {
        if (v >= 10) dec(v / 10);
        put((char)('0' + v % 10));
    }
    void hex(uint8_t v) { put("0123456789abcdef"[v >> 4]), put("0123456789abcdef"[v & 15]); }
};
} // namespace

void FukReader::describe(char* out, size_t size) const {
    static const char* const kText[] = {"done",
                                        "the X68000 is using the floppy drive",
                                        "FUK is not connected",
                                        "the drive is not ready (no disk?)",
                                        "FUK was unplugged, did not answer or was reset",
                                        "the disk was changed",
                                        "seek failed",
                                        "not a 2HD disk with 8 sectors of 1024 bytes, or the track cannot be read",
                                        "a sector cannot be read",
                                        "timed out",
                                        "cancelled"};
    const Result& r = result_;
    Text t{out, size};
    t.add(state_ == RUNNING ? "reading" : state_ == IDLE ? "idle" : kText[r.error]);
    if (r.c >= 0) t.add(" at cylinder "), t.dec(r.c);
    if (r.c >= 0 && r.h >= 0) t.add(" head "), t.dec(r.h);
    if (r.c >= 0 && r.r >= 0) t.add(" sector "), t.dec(r.r);
    if (r.hasStatus) {
        t.add(" (result");
        for (uint8_t b : r.st) t.add(" "), t.hex(b);
        t.add(")");
    }
    if ((state_ == DONE || state_ == FAILED) && touched_ && !r.restored) t.add("; the head position could not be restored");
}

} // namespace x68k
