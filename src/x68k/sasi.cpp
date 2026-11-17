// SASI のハードディスク (説明と根拠は sasi.h)。
#include "x68k/sasi.h"

#include <string.h>

namespace x68k {

// RST ($E96005) と X68000 のリセット: どのコントローラもコマンドを捨ててバスフリーになり、
// センスデータと割り込みの要求も消える
void Sasi::reset() {
    for (uint8_t& b : sense_) b = 0; // 前のコマンドのエラーも残さない
    interrupt_ = false;
    busFree(); // 転送中のコマンドとデータも捨てる (busFree() は割り込みとセンスデータに触らない)
}

void Sasi::busFree() {
    stage_ = BUS_FREE;
    cursor_.stop();
    fromDisk_ = false;
    cmdLen_ = 0;
    pos_ = len_ = 0;
}

void Sasi::attach(int unit, HardDiskImage* disk) {
    if ((unsigned)unit >= (unsigned)UNITS) return;
    // 転送の途中のドライブをつなぎ直したら、同じイメージでも転送は捨てる (古いイメージの位置を持ったままにしない。
    // SPC と同じ)
    if (stage_ != BUS_FREE && cmdLen_ == sizeof cmd_ && unit == id_ * 2 + lun()) busFree();
    image_[unit] = disk;
}

HardDiskImage* Sasi::drive() const {
    if (lun() > 1) return nullptr;
    HardDiskImage* img = image_[id_ * 2 + lun()];
    return img && img->loaded() && img->blockSize() == BLOCK_SIZE ? img : nullptr;
}

// REQ が有効か。データのフェーズでは、イメージの読み込みを待つ間は出さない
bool Sasi::request() {
    switch (stage_) {
    case COMMAND:
    case STATUS:
    case MESSAGE: return true;
    case DATA_IN:
    case DATA_OUT:
        if (!fromDisk_) return true;
        if (cursor_.ready(pos_)) return true;
        if (cursor_.failed()) finish(SENSE_DATA_ERROR, true); // イメージが読めなかった
        return stage_ == STATUS;
    case WORKING:
        work();
        return stage_ == STATUS;
    default: return false;
    }
}

// バスの状態。I/O、C/D、MSG の組み合わせは Inside X68000 の「SASI のバス動作」:
// コマンドは C/D、データはどちらも無効で I/O が向き、ステータスは C/D と I/O、メッセージは MSG も
uint8_t Sasi::lines() {
    const bool req = request();
    uint8_t v = 0;
    switch (stage_) {
    case BUS_FREE: return 0;
    case SELECTED:
    case WORKING:
    case DATA_OUT: break;
    case COMMAND: v = LINE_CD; break;
    case DATA_IN: v = LINE_IO; break;
    case STATUS: v = LINE_CD | LINE_IO; break;
    case MESSAGE: v = LINE_MSG | LINE_CD | LINE_IO; break;
    default: break;
    }
    return (uint8_t)(v | LINE_BSY | (req ? LINE_REQ : 0));
}

// ポートの読み出し。番地は 8 バイトごとに同じものが見える。$E96001 はデータ、$E96003 はバスの状態、ほかは $FF
uint8_t Sasi::read(uint32_t addr) {
    const uint32_t port = addr & 7;
    if (port == 1) return takeByte();
    if (port == 3) return lines();
    return 0xff;
}

// ポートへの書き込み (sasi.h の表)
void Sasi::write(uint32_t addr, uint8_t value) {
    const uint32_t port = addr & 7;
    if (port == 1) {
        putByte(value);
    } else if (port == 3) {
        // SEL を戻す: 選ばれたコントローラがコマンドを求める
        if (stage_ != SELECTED) return;
        stage_ = COMMAND;
        cmdLen_ = 0;
    } else if (port == 5) {
        reset(); // RST
    } else if (port == 7) {
        select(value);
    }
}

// セレクション: バスフリーのときに、ドライブがつながっている ID のビットが立っていれば、そのコントローラが BSY を返す。
// どのコントローラも答えなければバスフリーのまま (ホストは BSY を待ってあきらめる)
void Sasi::select(uint8_t idBits) {
    if (stage_ != BUS_FREE && stage_ != SELECTED) return;
    stage_ = BUS_FREE;
    for (int id = 0; id < 8; id++) {
        if (!(idBits & (1 << id))) continue;
        if (!image_[id * 2] && !image_[id * 2 + 1]) continue;
        id_ = (uint8_t)id;
        stage_ = SELECTED;
        return;
    }
}

uint8_t Sasi::takeByte() {
    switch (stage_) {
    case DATA_IN: {
        if (!request() || stage_ != DATA_IN) return 0;
        const uint8_t byte = fromDisk_ ? cursor_.at(pos_) : buf_[pos_];
        if (++pos_ >= len_) finish(SENSE_NO_ERROR);
        return byte;
    }
    case STATUS:
        stage_ = MESSAGE;
        interrupt_ = false;
        return status_;
    case MESSAGE:
        busFree();
        return 0x00; // コマンドコンプリート
    default: return 0;
    }
}

int Sasi::readData(uint8_t* dst, int n) {
    int done = 0;
    while (done < n && stage_ == DATA_IN && request() && stage_ == DATA_IN) {
        uint32_t k = fromDisk_ ? cursor_.avail(pos_) : len_ - pos_;
        if (k > (uint32_t)(n - done)) k = (uint32_t)(n - done);
        if (k > len_ - pos_) k = len_ - pos_;
        if (k == 0) break;
        memcpy(dst + done, fromDisk_ ? &cursor_.at(pos_) : buf_ + pos_, k);
        done += (int)k;
        pos_ += k;
        if (pos_ >= len_) finish(SENSE_NO_ERROR);
    }
    return done;
}

void Sasi::putByte(uint8_t v) {
    switch (stage_) {
    case COMMAND:
        if (cmdLen_ < sizeof cmd_) cmd_[cmdLen_++] = v; // 6 バイトそろったら実行する
        if (cmdLen_ == sizeof cmd_) execute();
        break;
    case DATA_OUT:
        if (!request() || stage_ != DATA_OUT) return;
        if (fromDisk_) {
            cursor_.at(pos_) = v;
            image_[id_ * 2 + lun()]->markDirty(cursor_.base() + pos_);
        } else {
            buf_[pos_] = v;
        }
        if (++pos_ >= len_) dataOutDone();
        break;
    default: break;
    }
}

void Sasi::dataOutDone() { finish(SENSE_NO_ERROR); }

// コマンドの結果を決めて、ステータスフェーズに入る。エラーなら、ステータスのビット 1 を立て、センスデータを残す
// (Inside X68000: エラーのあと REQUEST SENSE STATUS で 4 バイトを引き取る。1 バイト目の bit7 は番地が有効)
void Sasi::finish(uint8_t sense, bool addressValid) {
    cursor_.stop();
    fromDisk_ = false;
    const uint8_t unit = (uint8_t)(lun() << 5);
    status_ = sense == SENSE_NO_ERROR ? 0 : (uint8_t)(STATUS_ERROR | unit);
    if (cmd_[0] != CMD_REQUEST_SENSE || sense != SENSE_NO_ERROR) {
        sense_[0] = (uint8_t)(sense | (addressValid ? SENSE_ADDRESS_VALID : 0));
        sense_[1] = (uint8_t)(unit | (addressValid ? cmd_[1] & 0x1f : 0));
        sense_[2] = addressValid ? cmd_[2] : 0;
        sense_[3] = addressValid ? cmd_[3] : 0;
    } else {
        memset(sense_, 0, sizeof sense_); // 引き取ったら消える
    }
    stage_ = STATUS;
    interrupt_ = true;
    if (traceHook_) traceHook_(traceUser_, cmd_, status_, sense);
}

void Sasi::startDisk(Stage stage, uint32_t block, uint32_t count) {
    HardDiskImage* img = drive();
    stage_ = stage;
    fromDisk_ = true;
    pos_ = 0;
    len_ = count * BLOCK_SIZE;
    cursor_.start(img, (uint64_t)block * BLOCK_SIZE, len_);
}

void Sasi::startBuffer(Stage stage, uint32_t len) {
    stage_ = stage;
    fromDisk_ = false;
    pos_ = 0;
    len_ = len;
}

// FORMAT: 範囲を 0 で埋める。イメージの読み込みを待つことがあるので、少しずつ進める
void Sasi::work() {
    HardDiskImage* img = image_[id_ * 2 + lun()];
    while (pos_ < len_) {
        if (!cursor_.ready(pos_)) {
            if (cursor_.failed()) finish(SENSE_DATA_ERROR, true);
            return;
        }
        uint32_t k = cursor_.avail(pos_);
        if (k > len_ - pos_) k = len_ - pos_;
        memset(&cursor_.at(pos_), 0, k);
        img->markDirtyRange(cursor_.base() + pos_, k);
        pos_ += k;
    }
    finish(SENSE_NO_ERROR);
}

void Sasi::execute() { // コマンドの意味は sasi.h の表
    HardDiskImage* img = drive();
    const uint32_t blocks = img ? img->blocks() : 0;
    const uint32_t addr = address();
    const uint32_t count = cmd_[4] ? cmd_[4] : 256; // 0 は 256 ブロック
    const uint8_t op = cmd_[0];

    if (op == CMD_REQUEST_SENSE) {
        memcpy(buf_, sense_, sizeof sense_);
        startBuffer(DATA_IN, sizeof sense_);
        return;
    }
    if (op == CMD_ASSIGN_DRIVE) {
        // ドライブのパラメータ (10 バイト) を受け取る。大きさはイメージで決まっているので、中身は使わない。
        // 10 バイトは IPL ROM が送る数 (9 バイトや 11 バイトで終わることにすると、IPL ROM の起動が進まない)
        startBuffer(DATA_OUT, 10);
        return;
    }
    switch (op) {
    case CMD_TEST_DRIVE_READY:
    case CMD_RECALIBRATE:
    case CMD_SEEK:
    case CMD_CHECK_TRACK_FORMAT:
    case CMD_FORMAT_BAD_TRACK:
    case CMD_FORMAT_DRIVE:
    case CMD_FORMAT_TRACK:
    case CMD_READ:
    case CMD_WRITE: break;
    default: finish(SENSE_INVALID_COMMAND); return;
    }
    if (!img) {
        finish(SENSE_NOT_READY);
        return;
    }
    switch (op) {
    case CMD_TEST_DRIVE_READY:
    case CMD_RECALIBRATE: finish(SENSE_NO_ERROR); return;
    case CMD_SEEK:
    case CMD_CHECK_TRACK_FORMAT:
    case CMD_FORMAT_BAD_TRACK:
        if (addr >= blocks) finish(SENSE_INVALID_ADDRESS, true);
        else finish(SENSE_NO_ERROR);
        return;
    case CMD_READ:
    case CMD_WRITE:
        if (addr >= blocks || count > blocks - addr) {
            finish(SENSE_INVALID_ADDRESS, true);
            return;
        }
        if (op == CMD_WRITE && img->writeProtected()) {
            finish(SENSE_WRITE_FAULT);
            return;
        }
        startDisk(op == CMD_WRITE ? DATA_OUT : DATA_IN, addr, count);
        return;
    default: break;
    }
    // FORMAT DRIVE は指定の番地からドライブの終わりまで、FORMAT TRACK は番地を含む 1 トラック
    if (addr >= blocks) {
        finish(SENSE_INVALID_ADDRESS, true);
        return;
    }
    if (img->writeProtected()) {
        finish(SENSE_WRITE_FAULT);
        return;
    }
    uint32_t first = addr, n = blocks - addr;
    if (op == CMD_FORMAT_TRACK) {
        first = addr - addr % TRACK_BLOCKS;
        n = blocks - first < TRACK_BLOCKS ? blocks - first : TRACK_BLOCKS;
    }
    startDisk(WORKING, first, n);
    work();
}

} // namespace x68k
