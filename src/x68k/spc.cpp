// MB89352 (SPC) と SCSI の装置 (説明と根拠は spc.h)。
//
// 前半が SPC (レジスタ、セレクション、マニュアル転送、ハード転送と FIFO)、後半がバスの向こうのターゲット
// (フェーズの進め方、メッセージ、ハードディスクと CD-ROM 装置のコマンド)。
#include "x68k/spc.h"

#include <string.h>

namespace x68k {

namespace {

uint32_t be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
uint32_t be24(const uint8_t* p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
void put16(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
void put24(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}
void put32(uint8_t* p, uint32_t v) {
    put16(p, v >> 16);
    put16(p + 2, v & 0xffff);
}

// CD の番地: 論理ブロックのまま (4 バイト) か、分・秒・フレーム (0、M、S、F)。1 秒は 75 フレーム
void putMsf(uint8_t* p, uint32_t frames) {
    p[0] = 0;
    p[1] = (uint8_t)(frames / (60 * 75));
    p[2] = (uint8_t)(frames / 75 % 60);
    p[3] = (uint8_t)(frames % 75);
}
// 絶対位置: MSF では、論理ブロック 0 が 00:02:00 (150 フレーム。SCSI-2 の 14.1.1)
void putAddress(uint8_t* p, uint32_t lba, bool msf) {
    if (msf) putMsf(p, lba + 150);
    else put32(p, lba);
}

// コマンドの長さ: 1 バイト目の上位 3 ビット (グループ) で決まる (SCSI-2 の 7.2.1)
unsigned cdbSize(uint8_t op) {
    switch (op >> 5) {
    case 1:
    case 2: return 10;
    case 5: return 12;
    default: return 6;
    }
}

// 追加センスコード (SCSI-2 の表 71)
enum AdditionalSense : uint8_t {
    ASC_WRITE_ERROR = 0x0c,
    ASC_READ_ERROR = 0x11,
    ASC_INVALID_COMMAND = 0x20, // INVALID COMMAND OPERATION CODE
    ASC_LBA_RANGE = 0x21,
    ASC_BAD_CDB_FIELD = 0x24,   // INVALID FIELD IN CDB
    ASC_NO_LUN = 0x25,
    ASC_PROTECTED = 0x27,       // WRITE PROTECTED
    ASC_MEDIUM_CHANGED = 0x28,
    ASC_SEQUENCE = 0x2c,
    ASC_NO_SAVING = 0x39,
    ASC_NO_MEDIUM = 0x3a,
    ASC_REMOVAL_PREVENTED = 0x53, // クオリファイア 02h
    ASC_ILLEGAL_MODE_FOR_TRACK = 0x64,
};

// コマンドの操作コード。名前は T10 の操作コードの一覧 (https://www.t10.org/lists/op-num.htm) のもの。
// 48h (PLAY AUDIO TRACK INDEX) と A5h (PLAY AUDIO(12)) は SCSI-2 の CD-ROM 装置のコマンド (14 章)
enum : uint8_t {
    OP_TEST_UNIT_READY = 0x00,
    OP_REZERO_UNIT = 0x01,
    OP_REQUEST_SENSE = 0x03,
    OP_FORMAT_UNIT = 0x04,
    OP_READ6 = 0x08,
    OP_WRITE6 = 0x0a,
    OP_SEEK6 = 0x0b,
    OP_INQUIRY = 0x12,
    OP_MODE_SELECT6 = 0x15,
    OP_MODE_SENSE6 = 0x1a,
    OP_START_STOP_UNIT = 0x1b,
    OP_SEND_DIAGNOSTIC = 0x1d,
    OP_PREVENT_ALLOW = 0x1e,
    OP_READ_CAPACITY = 0x25,
    OP_READ10 = 0x28,
    OP_WRITE10 = 0x2a,
    OP_SEEK10 = 0x2b,
    OP_WRITE_VERIFY = 0x2e,
    OP_VERIFY = 0x2f,
    OP_SYNCHRONIZE_CACHE = 0x35,
    OP_READ_SUB_CHANNEL = 0x42,
    OP_READ_TOC = 0x43,
    OP_PLAY_AUDIO10 = 0x45,
    OP_PLAY_AUDIO_MSF = 0x47,
    OP_PLAY_TRACK_INDEX = 0x48,
    OP_PAUSE_RESUME = 0x4b,
    OP_MODE_SELECT10 = 0x55,
    OP_MODE_SENSE10 = 0x5a,
    OP_PLAY_AUDIO12 = 0xa5,
};

} // namespace

Spc::~Spc() { delete cd_; }

// ================================================================ SPC

// X68000 のリセット: SPC はリセットと切り離しの状態 (SCTL の bit7 = 1) になり、バスの装置もリセットされる
void Spc::reset() { // 説明は上
    chipReset();
    sctl_ = SCTL_RESET;
    bdid_ = 0;
    for (auto& s : sense_) memset(s, 0, sizeof s); // どの装置のセンスデータもなし
    if (cd_) cd_->reset();
}

// SCTL の bit7: レジスタと制御回路のリセット。バスから切り離される (BDID と SCTL はそのまま)
void Spc::chipReset() {
    dropBus();
    scmd_ = 0;
    ints_ = 0;
    sdgc_ = 0;
    pctl_ = 0;
    temp_ = 0;
    transferCount_ = 0;
    fifoCount_ = 0;
    endInts_ = 0;
    fifoIn_ = false;
    atnOut_ = false;
    selWait_ = false;
}

// SCSI バスのリセット (RST): 装置はコマンドを捨ててバスフリーになり、センスデータも消える。
// (SCSI-2 の 6.2.2.1 では、リセットのあとの最初のコマンドに UNIT ATTENTION を返す。ここでは返さない:
// 起動の最初のコマンドを失敗させないため。CD-ROM 装置の媒体の入れ替えの UNIT ATTENTION も、リセットで消す)
// SPC は Reset Condition の割り込みを出す (BDID、SCTL、SCMD、PCTL、転送カウンタは変わらない。Inside X68000)
void Spc::busReset() {
    dropBus();
    fifoCount_ = 0;
    endInts_ = 0;
    atnOut_ = false;
    selWait_ = false;
    for (auto& s : sense_) memset(s, 0, sizeof s);
    if (cd_) cd_->reset();
    ints_ |= INTS_RESET;
}

uint8_t Spc::popFifo() {
    const uint8_t head = fifo_[0];
    fifoCount_--;
    memmove(fifo_, fifo_ + 1, fifoCount_);
    if (fifoCount_ == 0 && endInts_) { // 読み終えたので、ハード転送が終わる
        ints_ |= endInts_;
        endInts_ = 0;
    }
    return head;
}

// レジスタを読む (並びとビットは spc.h の表)。表にない番号 (3、15) は $FF
uint8_t Spc::read(int index) {
    if (index == DREG) return readData(); // FIFO から取り出す (前後でバスの転送を進める)
    run(); // ほかのレジスタは、読む前にバスの上の転送を進めておく
    if (index == TEMP) return readTemp();
    if (index == PSNS) return psns();
    if (index == SSTS) return ssts();
    if (index >= TCH && index <= TCL) {
        const int shift = 8 * (TCL - index); // TCH が bit23-16、TCM が bit15-8、TCL が bit7-0
        return (uint8_t)(transferCount_ >> shift);
    }
    if (index == BDID) return (uint8_t)(1u << bdid_); // 自分の ID のビット
    if (index == MBC) return (uint8_t)(transferCount_ & 0x0f);
    if (index == SERR) return 0x00; // パリティなどのエラーは起きない
    // 書いた値をそのまま返すレジスタ
    const uint8_t* held = index == SCTL ? &sctl_ : index == SCMD ? &scmd_ : index == INTS ? &ints_ : index == PCTL ? &pctl_ : nullptr;
    return held ? *held : 0xff;
}

uint8_t Spc::readData() {
    run();
    const uint8_t byte = fifoIn_ && fifoCount_ ? popFifo() : 0; // 入力の向きで FIFO にあるときだけ
    run(); // 空いた分を、バスから取り込む
    return byte;
}

// マニュアル転送: ターゲットが出しているバスのデータ (ACK を返したあとも、戻すまでは同じバイト。
// IPL ROM のドライバは Set ACK のあとに読む)。出していなければ、書いた値。
// ACK を出している間は、Set ACK のときに覚えたバイトを返す (ステートを戻した直後は、イメージが
// まだ読み込まれていないことがある)
uint8_t Spc::readTemp() {
    const bool targetDrives = connected_ && (tphase_ & PSNS_IO); // 入力のフェーズのターゲットがバスにデータを出す
    if (!targetDrives) return temp_;
    if (ack_) return ackValid_ ? ackByte_ : temp_;
    return req() ? busIn() : temp_;
}

// レジスタに書く。表にない番号と、読むだけのレジスタ (MBC、SSTS、SERR) は、書いても何も起きない
void Spc::write(int index, uint8_t value) {
    switch (index) {
    case BDID: bdid_ = value & 7; return;
    case SCTL: writeControl(value); return;
    case SCMD: writeCommand(value); return;
    case INTS: clearInterrupts(value); return;
    case SDGC: sdgc_ = value; return;
    case PCTL: pctl_ = value & (PCTL_BUSFREE_INT | 7); return;
    case DREG: pushFifo(value); return;
    case TEMP: temp_ = value; return; // タイムアウトのあとは $00 を書いて、バスに出している ID を下げる (Inside X68000)
    case TCH: setCounterByte(16, value); return;
    case TCM: setCounterByte(8, value); return;
    case TCL: setCounterByte(0, value); return;
    }
}

// SCTL。bit7 はレジスタと制御回路のリセット、bit6 は転送の制御回路だけのリセット (バスとの結合は変わらない)
void Spc::writeControl(uint8_t value) {
    sctl_ = value;
    if (value & SCTL_RESET) {
        chipReset();
        return;
    }
    if (value & SCTL_CONTROL_RESET) {
        xfer_ = pad_ = false;
        fifoCount_ = 0;
        endInts_ = 0;
    }
}

// SCMD。リセットの間 (SCTL の bit7) は、RST もコマンドも受け付けない。
// RST (bit4) を 0 から 1 にしたときにバスがリセットされる。RST が 1 の間はコマンドを実行しない
void Spc::writeCommand(uint8_t value) {
    const bool rstWasOn = (scmd_ & SCMD_RST) != 0;
    scmd_ = value;
    if (sctl_ & SCTL_RESET) return;
    if (!(value & SCMD_RST)) command(value);
    else if (!rstWasOn) busReset();
}

// INTS に書く: 1 のビットの要因を消す。
// 答えのなかったセレクションのあと (SEL を出したまま): Time Out を消したとき、転送カウンタが 0 なら信号をすべて
// 戻して、結合していない状態になる。新しい待ち時間を転送カウンタに入れてから消すと、その時間だけもう一度応答を
// 待ち、また Time Out になる。Time Out が出たとき、転送カウンタは 0 になっている (MB89352 のデータシートの
// Select コマンドの説明。富士通 1989 SCSI Product Guide)。
// IPL ROM の SCSI のドライバは、Time Out のあと TEMP に $00、転送カウンタに短い時間を書いてから Time Out を消し、
// 次の割り込みを待つ
void Spc::clearInterrupts(uint8_t bits) {
    ints_ &= (uint8_t)~bits;
    if (!(bits & INTS_TIMEOUT) || !selWait_) return;
    if (transferCount_ == 0) {
        selWait_ = false;
    } else {
        transferCount_ = 0;
        ints_ |= INTS_TIMEOUT; // 待ち直した時間が過ぎた
    }
}

// DREG に書く: データアウトのハード転送の間だけ、FIFO に空きがあれば入れる
void Spc::pushFifo(uint8_t value) {
    if (xfer_ && !fifoIn_ && fifoCount_ < FIFO_SIZE) fifo_[fifoCount_++] = value;
    run();
}

// 転送カウンタ (24 ビット) の 1 バイト。shift は TCH が 16、TCM が 8、TCL が 0
void Spc::setCounterByte(unsigned shift, uint8_t value) {
    transferCount_ = (transferCount_ & ~(0xffu << shift)) | (uint32_t)value << shift;
}

uint8_t Spc::psns() {
    uint8_t v = 0;
    if (connected_) {
        v |= PSNS_BSY | tphase_;
        if (req()) v |= PSNS_REQ;
        if (atnOut_) v |= PSNS_ATN;
    }
    if (ack_) v |= PSNS_ACK;
    if (selWait_) v |= PSNS_SEL;
    return v;
}

// SSTS。bit7-4 は「SSTS の状態の表」(Inside X68000 の図 18): イニシエータとして結合していて、
//   実行中の動作がなければ 1000、REQ が来ているが転送していなければ 1001、ハード転送の実行中は 1011。
// FIFO に受け取ったデータが残っている間は、ハード転送は終わっていない (Service Required の説明)。
// 答えのないセレクションで SEL を出したまま待っている間も、イニシエータとして動いているので bit7 (INIT) を 1 にする。
// FORMAT.X が SCSI のディスクに書くドライバ (68000 の機種で使う) は、Time Out のあとに INIT が 1 なら
// Time Out のあとの手順 (TEMP に $00、転送カウンタに短い時間を入れて待ち直す) に進み、最後に INIT が 0 なら
// 「装置がない」として返る。最初の Time Out のあとに INIT が 0 だと、セレクションをやり直す
// (0 だと、装置のない ID でやり直し続ける)
uint8_t Spc::ssts() {
    uint8_t v = 0;
    if (connected_ || selWait_) v |= SSTS_INIT;
    if (xfer_ || fifoCount_) v |= SSTS_BUSY | SSTS_TRIP;
    else if (connected_ && req()) v |= SSTS_TRIP;
    if (selWait_ && !(ints_ & INTS_TIMEOUT)) v |= SSTS_BUSY; // 待つ時間が無限大のセレクション
    if (scmd_ & SCMD_RST) v |= SSTS_RST;
    if (transferCount_ == 0) v |= SSTS_TC0;
    if (fifoCount_ == FIFO_SIZE) v |= SSTS_FULL;
    if (fifoCount_ == 0) v |= SSTS_EMPTY;
    return v;
}

void Spc::command(uint8_t v) {
    switch (v & CMD_MASK) {
    case CMD_BUS_RELEASE: selWait_ = false; break; // 待っている Select の取り消し (ターゲットのときの動きは作っていない)
    case CMD_SELECT: select(); break;
    case CMD_RESET_ATN: atnOut_ = false; break; // ATN を戻す
    case CMD_SET_ATN: atnOut_ = true; break;    // ATN を出す (セレクションの前に出すと、ATN つきのセレクションになる)
    case CMD_TRANSFER: startTransfer(); break;
    case CMD_TRANSFER_PAUSE: break; // ターゲットのときだけのコマンド
    case CMD_SET_ACK:
        // マニュアル転送: REQ に ACK で答える。バスのバイトはここで受け渡す (入力はターゲットが出しているバイトを
        // 覚え、出力は TEMP の値をターゲットが受け取る)。データアウトのバイトは、ここでイメージかバッファに入れる
        if (ack_ || xfer_) break;
        ackValid_ = req();
        if (ackValid_) {
            ackByte_ = tphase_ & PSNS_IO ? busIn() : temp_;
            if (tphase_ == PH_DATA_OUT) store(ackByte_);
        }
        ack_ = true;
        break;
    case CMD_RESET_ACK: releaseAck(); break;
    }
}

// Reset ACK/REQ: ACK を戻す。それが REQ に答えた ACK なら、ターゲットは次のバイトか次のフェーズへ進む。
// ACK を出していなければ何もしない
void Spc::releaseAck() {
    if (!ack_) return;
    const bool answered = ackValid_ && connected_;
    ack_ = ackValid_ = false;
    if (!answered) return;
    if (tphase_ & PSNS_IO) took();
    else if (tphase_ == PH_DATA_OUT) stored(); // バイトは Set ACK のときに入れてある
    else gave(ackByte_);
}

// Select: TEMP に自分と相手の ID のビット、TCH / TCM に応答を待つ時間。PCTL の bit0 が 1 ならリセレクション。
// 相手がいれば Command Complete、いなければ Time Out (待つ時間が 0 なら無限大で、割り込みは出ない)。
// 答えがなければ SEL を出したままになる。アービトレーションは、ほかにイニシエータがいないのでいつも勝つ
void Spc::select() {
    if (connected_) return;
    selWait_ = false;
    int id = -1; // 選ぶ相手 (いなければ -1)
    if (!(pctl_ & PSNS_IO)) {
        const uint8_t others = temp_ & (uint8_t)~(1u << bdid_);
        for (int i = 7; i >= 0 && id < 0; i--)
            if (others & (1u << i)) id = i;
    }
    if (!present(id)) {
        atnOut_ = false;
        selWait_ = true;
        if (transferCount_ >> 8) { // 待つ間に転送カウンタは 0 まで減る
            transferCount_ = 0;
            ints_ |= INTS_TIMEOUT;
        }
        return;
    }
    connected_ = true;
    target_ = (uint8_t)id;
    lun_ = 0; // IDENTIFY が来るまでは 0
    identified_ = false;
    cdbLen_ = 0;
    msgExt_ = 0;
    ack_ = ackValid_ = false;
    src_ = SRC_NONE;
    // ATN を出して選ばれたターゲットは、コマンドの前にメッセージアウトフェーズにする (SCSI-2 の 6.2.1)
    tphase_ = atnOut_ ? PH_MSG_OUT : PH_COMMAND;
    ints_ |= INTS_COMPLETE;
}

// Transfer: PCTL のフェーズで、転送カウンタのバイト数をハード転送する。
// 転送カウンタが 0 で、ターミネーションモードが 1 のデータのフェーズなら、パディング転送
// (ターゲットがフェーズを変えるまで、受け取ったデータは捨て、送るデータは $00)
void Spc::startTransfer() {
    if (!connected_) return;
    xfer_ = true;
    pad_ = false;
    fifoCount_ = 0;
    endInts_ = 0;
    fifoIn_ = (pctl_ & PSNS_IO) != 0;
    if (transferCount_ == 0) {
        // 数が 0 で、データフェーズでパディングを指示されたときだけ転送を続ける。ほかはすぐ終わる
        const bool padding = (scmd_ & SCMD_TERM) && (pctl_ & 6) == 0;
        if (!padding) {
            endTransfer(INTS_COMPLETE);
            return;
        }
        pad_ = true;
    }
    run();
}

// バスの上の転送が終わった。受け取ったデータが FIFO に残っていれば、割り込みは FIFO を読み終えたときに出す:
// 「データ入力時には、内部バッファのデータがすべて引き取られるまで転送動作は終結しない」(Inside X68000 の
// Service Required の説明)。IPL ROM のドライバも、割り込みが出たあとは DREG を読まない (動かして見た結果)
void Spc::endTransfer(uint8_t ints) {
    xfer_ = pad_ = false;
    if (fifoIn_ && fifoCount_) {
        endInts_ |= ints;
        return;
    }
    ints_ |= ints;
    if (!fifoIn_) fifoCount_ = 0;
}

// ハード転送を、進められるところまで進める: FIFO とバスの間で、ターゲットが REQ を出しているバイトを受け渡す。
//   - 転送カウンタが 0 になったら Command Complete
//   - ターゲットが PCTL と違うフェーズにしたら Service Required
//   - メッセージアウトの最後のバイトの前に ATN を戻す。メッセージインの最後のバイトでは ACK を出したままにする
//     (Reset ACK/REQ コマンドで戻す)。どちらも Inside X68000 の SPC のコマンドの説明
void Spc::run() {
    checkMedia();
    while (xfer_ && connected_) {
        const uint8_t want = pctl_ & 7;
        if (tphase_ != want) {
            if (!ack_) endTransfer(INTS_SERVICE);
            return;
        }
        if (!pad_ && transferCount_ == 0) { // 転送の途中で転送カウンタに 0 を書いた: そこで終わる (ターゲットの REQ を待たない)
            endTransfer(INTS_COMPLETE);
            return;
        }
        if (!req()) {
            checkMedia();
            if (tphase_ == want && !req()) return;
            continue;
        }
        if (pad_) {
            if (want & PSNS_IO) took();
            else gave(0);
            continue;
        }
        if (want & PSNS_IO) {
            if (fifoCount_ == FIFO_SIZE) return;
            const uint8_t v = busIn();
            fifo_[fifoCount_++] = v;
            transferCount_--;
            if (want == PH_MSG_IN && transferCount_ == 0) {
                ack_ = ackValid_ = true;
                ackByte_ = v;
            }
            else took();
        } else {
            if (fifoCount_ == 0) return;
            const uint8_t v = popFifo();
            if (want == PH_MSG_OUT && transferCount_ == 1) atnOut_ = false;
            transferCount_--;
            gave(v);
        }
        if (transferCount_ == 0) {
            endTransfer(INTS_COMPLETE);
            return;
        }
    }
}

bool Spc::dataReady() {
    run();
    if (scmd_ & SCMD_PT) return false;
    if (fifoIn_) return fifoCount_ > 0;
    return xfer_ && fifoCount_ < FIFO_SIZE;
}

int Spc::readData(uint8_t* dst, int n, bool needRequest) {
    run();
    if (!fifoIn_ || (pctl_ & 7) != PH_DATA_IN) return 0;
    if (needRequest && (scmd_ & SCMD_PT)) return 0;
    int done = 0;
    // FIFO に取り込んである分
    while (done < n && fifoCount_) dst[done++] = popFifo();
    // その続きを、FIFO を通さずにターゲットから写す (1 バイトずつ FIFO を通したのと同じ順で、同じ数になる)
    while (done < n && xfer_ && !pad_ && connected_ && tphase_ == PH_DATA_IN && req()) {
        uint32_t k = src_ == SRC_DISK ? cursor_.avail(pos_) : len_ - pos_;
        if (k > len_ - pos_) k = len_ - pos_;
        if (k > (uint32_t)(n - done)) k = (uint32_t)(n - done);
        if (k > transferCount_) k = transferCount_;
        if (k == 0) break;
        memcpy(dst + done, sourceAt(pos_), k);
        done += (int)k;
        transferCount_ -= k;
        pos_ += k - 1;
        took(); // 最後の 1 バイトで、データの終わりならステータスフェーズへ
        if (transferCount_ == 0) endTransfer(INTS_COMPLETE);
    }
    run();
    return done;
}

// ================================================================ 装置をつなぐ

void Spc::attach(int id, HardDiskImage* image, bool cdrom, const CdToc* toc) {
    if (id < 0 || id > 7) return;
    if (connected_ && target_ == id) detach(); // コマンドの途中の装置を替えたら、コマンドは捨てる
    memset(sense_[id], 0, sizeof sense_[id]);
    disk_[id] = nullptr;
    if (cd_ && (cdrom || cd_->id() == id)) {
        if (connected_ && target_ == cd_->id()) detach();
        delete cd_;
        cd_ = nullptr;
    }
    HardDiskImage* loaded = image && image->loaded() ? image : nullptr;
    if (!cdrom) {
        disk_[id] = loaded;
        return;
    }
    cd_ = new CdDrive;
    if (!cd_) return;
    cd_->setId(id);
    cd_->setMedium(loaded, toc, false);
}

bool Spc::changeCd(HardDiskImage* image, const CdToc* toc) {
    if (!cd_) return false;
    if (connected_ && target_ == cd_->id()) detach();
    cd_->setMedium(image && image->loaded() ? image : nullptr, toc, true);
    return true;
}

// ================================================================ ターゲット

// ターゲットがバスを放す (バスフリー、リセット、つなぎ直し): 割り込みは出さない。
// バスフリーになったら、イニシエータが出していた ATN も戻す (次のセレクションは、Set ATN をし直さなければ ATN なし)
void Spc::dropBus() {
    connected_ = false;
    atnOut_ = false;
    xfer_ = pad_ = false;
    ack_ = ackValid_ = false;
    cursor_.stop();
    src_ = SRC_NONE;
    writing_ = false;
    pos_ = len_ = 0;
}

// ターゲットがバスを放す (コマンドの終わり)。PCTL の bit7 が 1 なら Disconnected の割り込み
void Spc::busFree() {
    dropBus();
    if (pctl_ & PCTL_BUSFREE_INT) ints_ |= INTS_DISCONNECTED;
}

// イメージが読めなかったら (x68k/disk_cache.h)、MEDIUM ERROR で終わる
void Spc::checkMedia() {
    if (!connected_ || src_ != SRC_DISK || (tphase_ != PH_DATA_IN && tphase_ != PH_DATA_OUT)) return;
    if (!cursor_.ready(pos_) && cursor_.failed()) check(KEY_MEDIUM_ERROR, writing_ ? ASC_WRITE_ERROR : ASC_READ_ERROR);
}

// コマンドの途中の装置を外した、媒体を替えた: コマンドを捨て、FIFO に残っているその装置のデータと、
// FIFO を読み終えたら出すはずだった割り込みも捨てる (外した装置のデータを DMA に渡さない)
void Spc::detach() {
    dropBus();
    fifoCount_ = 0;
    endInts_ = 0;
}

// ターゲットが REQ を出しているか。ACK が出ている間と、イメージの読み込みを待つ間は出さない
bool Spc::req() {
    if (!connected_ || ack_) return false;
    if (src_ == SRC_DISK && (tphase_ == PH_DATA_IN || tphase_ == PH_DATA_OUT)) return cursor_.ready(pos_);
    return true;
}

uint8_t* Spc::sourceAt(uint32_t pos) {
    switch (src_) {
    case SRC_CD: return cd_->buf() + pos;
    case SRC_DISK: return &cursor_.at(pos);
    default: return buf_ + pos;
    }
}

// 入力のフェーズで、ターゲットがバスに出しているバイト
uint8_t Spc::busIn() {
    switch (tphase_) {
    case PH_DATA_IN: return *sourceAt(pos_);
    case PH_STATUS: return status_;
    case PH_MSG_IN: return msgIn_;
    default: return 0;
    }
}

// 入力のフェーズのバイトを受け取った: 次のバイトか、次のフェーズへ。
// ステータスの次は COMMAND COMPLETE のメッセージ、そのあとバスフリー (SCSI-2 の 6.6.5)
void Spc::took() {
    switch (tphase_) {
    case PH_DATA_IN:
        if (++pos_ >= len_) good();
        break;
    case PH_STATUS:
        tphase_ = PH_MSG_IN;
        msgIn_ = 0x00;
        msgThenFree_ = true;
        break;
    case PH_MSG_IN:
        if (msgThenFree_) {
            busFree();
        } else {
            tphase_ = PH_COMMAND;
            cdbLen_ = 0;
        }
        break;
    default: break;
    }
}

// 出力のフェーズのバイトを受け取った
void Spc::gave(uint8_t v) {
    switch (tphase_) {
    case PH_MSG_OUT: message(v); break;
    case PH_COMMAND:
        if (cdbLen_ < sizeof cdb_) cdb_[cdbLen_++] = v;
        if (cdbLen_ >= cdbSize(cdb_[0])) execute();
        break;
    case PH_DATA_OUT: // マニュアル転送のデータアウト
        store(v);
        stored();
        break;
    default: break;
    }
}

// データアウトのバイトを、イメージかバッファに入れる (REQ を出しているとき = イメージが読み込まれているとき)
void Spc::store(uint8_t v) {
    if (src_ == SRC_DISK) {
        cursor_.at(pos_) = v;
        disk_[target_]->markDirty(cursor_.base() + pos_);
    } else if (pos_ < sizeof buf_) {
        buf_[pos_] = v; // MODE SELECT のパラメータ (使わない。256 バイトを超えた分は捨てる)
    }
}

// データアウトのバイトを入れ終えた: 次のバイトか、ステータスへ
void Spc::stored() {
    if (++pos_ >= len_) good();
}

// メッセージアウト (SCSI-2 の 6.1.9.2、6.6)。ATN が出ている間はメッセージを受け取り続け、戻ったらコマンドフェーズへ。
//   IDENTIFY ($80-$FF): 論理ユニット番号を覚える (ディスコネクトの許可は使わない)
//   ABORT ($06)、BUS DEVICE RESET ($0C): バスフリーにする
//   拡張メッセージ ($01、長さ、中身。同期転送の相談など): 受け取ってから MESSAGE REJECT ($07) を返す
//   ほか (NO OPERATION など) は何もしない
void Spc::message(uint8_t v) {
    if (msgExt_ == 1) {
        msgExt_ = (uint16_t)((v ? v : 256) + 1); // 長さ 0 は 256 バイト (SCSI-2 の 6.5 の表 11)
        return;
    }
    if (msgExt_ > 1) {
        if (--msgExt_ > 1) return;
        msgExt_ = 0;
        tphase_ = PH_MSG_IN;
        msgIn_ = 0x07;
        msgThenFree_ = false;
        return;
    }
    if (v & 0x80) {
        lun_ = v & 7;
        identified_ = true;
    } else if (v == 0x01) {
        msgExt_ = 1;
        return;
    } else if (v == 0x06 || v == 0x0c) {
        if (v == 0x0c) memset(sense_[target_], 0, sizeof sense_[target_]);
        busFree();
        return;
    }
    if (!atnOut_) {
        tphase_ = PH_COMMAND;
        cdbLen_ = 0;
    }
}

void Spc::toStatus(uint8_t status) {
    cursor_.stop();
    src_ = SRC_NONE;
    writing_ = false;
    status_ = status;
    tphase_ = PH_STATUS;
    if (traceHook_) traceHook_(traceUser_, target_, cdb_, status);
}

// CHECK CONDITION で終わり、センスデータを残す (REQUEST SENSE で引き取る)。code は $70 (今のエラー) か $71 (遅れて知らせるエラー)
void Spc::check(uint8_t key, uint8_t asc, uint8_t ascq, uint8_t code) {
    uint8_t* s = sense_[target_];
    s[0] = code;
    s[1] = key;
    s[2] = asc;
    s[3] = ascq;
    toStatus(ST_CHECK);
}

void Spc::dataIn(Source src, uint32_t len) {
    if (!len) { // 転送するバイトがなければ、すぐにステータスへ
        good();
        return;
    }
    src_ = src;
    pos_ = 0;
    len_ = len;
    tphase_ = PH_DATA_IN;
}

void Spc::dataOut(Source src, uint32_t len) {
    if (!len) {
        good();
        return;
    }
    src_ = src;
    writing_ = src == SRC_DISK;
    pos_ = 0;
    len_ = len;
    tphase_ = PH_DATA_OUT;
}

// コマンドがそろった。REQUEST SENSE と INQUIRY は、どの状態でも実行する (SCSI-2 の 7.9、8.2.5、8.2.14)。
// ほかのコマンドは、前のセンスデータを消してから実行する。論理ユニットは 0 だけ: ほかの番号では、INQUIRY は装置の種類 $7F、
// REQUEST SENSE は LOGICAL UNIT NOT SUPPORTED、ほかのコマンドは CHECK CONDITION (SCSI-2 の 7.5.3)。
// IDENTIFY を受け取っていれば、コマンドの中の論理ユニット番号は見ない (7.2.2)
void Spc::execute() {
    const uint8_t op = cdb_[0];
    if (op == OP_REQUEST_SENSE) {
        requestSense();
        return;
    }
    memset(sense_[target_], 0, sizeof sense_[target_]);
    if (op == OP_INQUIRY) {
        inquiry();
        return;
    }
    if (!lunZero()) {
        check(KEY_ILLEGAL, ASC_NO_LUN);
        return;
    }
    if (isCdrom(target_)) executeCd();
    else executeDisk();
}

// REQUEST SENSE: 拡張センスデータ (Inside X68000 の図 35、SCSI-2 の表 65)。18 バイトのうち割り当て長まで。
// 割り当て長 0 は 4 バイト (SCSI-1 の決まり。SCSI-2 の 8.2.1 の注 62 も、SCSI-1 / CCS として動く装置は 4 バイトを返すとしている)
void Spc::requestSense() {
    uint8_t* s = sense_[target_];
    uint8_t code = s[0], key = s[1], asc = s[2], ascq = s[3];
    const bool cd = isCdrom(target_);
    if (!lunZero()) {
        code = 0x70;
        key = KEY_ILLEGAL;
        asc = ASC_NO_LUN;
        ascq = 0;
    } else if (!code && cd) {
        if (cd_->takeAttention()) {
            // 媒体を入れ替えたあとの最初のコマンドが REQUEST SENSE なら、ここで UNIT ATTENTION を知らせる
            key = KEY_ATTENTION;
            asc = ASC_MEDIUM_CHANGED;
        } else if (cd_->audioStatus() == CdDrive::AUDIO_PLAY || cd_->audioStatus() == CdDrive::AUDIO_PAUSED) {
            // 再生の状態: NO SENSE で、クオリファイアがオーディオステータス (SCSI-2 の表 71 の 00h 11h、00h 12h)
            ascq = cd_->audioStatus();
        }
    }
    memset(buf_, 0, 18);
    buf_[0] = code ? code : 0x70;
    buf_[2] = key;
    buf_[7] = 10;
    buf_[12] = asc;
    buf_[13] = ascq;
    memset(s, 0, sizeof sense_[target_]);
    const uint32_t alloc = cdb_[4] ? cdb_[4] : 4;
    dataIn(SRC_BUF, alloc < 18 ? alloc : 18);
}

// INQUIRY (Inside X68000 の図 26、SCSI-2 の表 45): 装置の種類 ($00 ダイレクトアクセス、$05 CD-ROM)、取り外しできるか、
// 準拠する規格、ベンダと製品の名前。論理ユニットがなければ、種類は $7F
void Spc::inquiry() {
    const bool cd = isCdrom(target_);
    memset(buf_, 0, 36);
    if (!lunZero()) {
        buf_[0] = 0x7f;
    } else {
        buf_[0] = cd ? 0x05 : 0x00;
        buf_[1] = cd ? 0x80 : 0x00;
    }
    buf_[2] = 2; // ANSI の版: SCSI-2 (コマンドとセンスデータを SCSI-2 に沿って作っている)
    buf_[3] = 2; // 応答データの形式: SCSI-2
    buf_[4] = 31;
    memcpy(buf_ + 8, "PIX68K  ", 8);
    memcpy(buf_ + 16, cd ? "CD-ROM DRIVE    " : "HARD DISK       ", 16);
    memcpy(buf_ + 32, "1.00", 4);
    dataIn(SRC_BUF, cdb_[4] < 36 ? cdb_[4] : 36);
}

// MODE SENSE(6) / (10) (Inside X68000 の図 27、28。SCSI-2 の 8.2.10、8.2.11)。
// ヘッダ、ブロックディスクリプタ (DBD = 1 なら省く)、ページ。CD-ROM 装置は、ページ 01h (読み出しのエラー回復)、
// 0Dh (CD-ROM のパラメータ)、0Eh (オーディオの制御) を持つ (SCSI-2 の 14.3.3、表 268、271、273)。
// ハードディスクは、ページ 03h (フォーマットのパラメータ。SCSI-2 の 9.3.3.3、表 163) と 04h (ドライブの形。9.3.3.7、表 171) を持つ。
// イメージに形はないので、1 シリンダを 8 ヘッド × 32 セクタとして、シリンダ数をブロック数から決める (端数は切り捨て)。
// PC = 1 (変えられるビット) は全部 0、PC = 2 (既定の値) は今の値と同じ、PC = 3 (保存した値) はない
void Spc::modeSense(bool cd) {
    const bool ten = cdb_[0] == OP_MODE_SENSE10;
    const uint8_t pc = cdb_[2] >> 6, page = cdb_[2] & 0x3f;
    const bool dbd = (cdb_[1] & 0x08) != 0;
    const uint32_t alloc = ten ? be16(cdb_ + 7) : cdb_[4];
    if (pc == 3) {
        check(KEY_ILLEGAL, ASC_NO_SAVING);
        return;
    }
    static const uint8_t kCdPages[][16] = {
        {0x01, 0x06, 0, 0, 0, 0, 0, 0},
        {0x0d, 0x06, 0, 0x0d, 0, 60, 0, 75},                               // 1 分 = 60 秒、1 秒 = 75 フレーム
        {0x0e, 0x0e, 0x04, 0, 0, 0, 0, 0, 0x01, 0xff, 0x02, 0xff, 0, 0, 0, 0}, // Immed、出力 0 / 1 はチャネル 0 / 1、音量は最大
    };
    HardDiskImage* img = disk_[target_];
    uint8_t diskPages[2][24] = {};
    if (!cd) {
        const uint32_t kHeads = 8, kSectors = 32;
        const uint32_t cylinders = img->blocks() / (kHeads * kSectors);
        uint8_t* f = diskPages[0];
        f[0] = 0x03;
        f[1] = 0x16;
        put16(f + 2, kHeads);            // 1 ゾーンのトラック数 (1 シリンダ)
        put16(f + 10, kSectors);         // 1 トラックのセクタ数
        put16(f + 12, img->blockSize()); // 物理セクタのバイト数
        put16(f + 14, 1);                // インタリーブ
        f[20] = 0x40;                    // HSEC: ハードセクタ
        uint8_t* g = diskPages[1];
        g[0] = 0x04;
        g[1] = 0x16;
        put24(g + 2, cylinders > 0xffffff ? 0xffffff : cylinders ? cylinders : 1);
        g[5] = (uint8_t)kHeads;
        put16(g + 20, 3600); // 回転数 (rpm)
    }
    const int pages = cd ? 3 : 2;
    auto pageAt = [&](int i) { return cd ? kCdPages[i] : diskPages[i]; };
    bool found = page == 0x3f || page == 0x00;
    for (int i = 0; i < pages; i++) found = found || pageAt(i)[0] == page;
    if (!found) {
        check(KEY_ILLEGAL, ASC_BAD_CDB_FIELD);
        return;
    }
    memset(buf_, 0, sizeof buf_);
    uint32_t n = ten ? 8 : 4;
    if (!dbd) {
        uint8_t* b = buf_ + n;
        const uint32_t blocks = cd ? cd_->blocks() : img->blocks();
        b[0] = cd ? 0x01 : 0x00; // 密度コード
        put24(b + 1, blocks > 0xffffff ? 0xffffff : blocks);
        put24(b + 5, cd ? 2048 : img->blockSize());
        n += 8;
    }
    for (int i = 0; i < pages; i++) {
        const uint8_t* p = pageAt(i);
        if (page != 0x3f && page != p[0]) continue;
        const uint32_t size = 2u + p[1];
        memcpy(buf_ + n, p, pc == 1 ? 2 : size);
        n += size;
    }
    const uint8_t wp = !cd && img->writeProtected() ? 0x80 : 0x00;
    if (ten) {
        put16(buf_, n - 2);
        buf_[3] = wp;
        put16(buf_ + 6, dbd ? 0 : 8);
    } else {
        buf_[0] = (uint8_t)(n - 1);
        buf_[2] = wp;
        buf_[3] = dbd ? 0 : 8;
    }
    dataIn(SRC_BUF, alloc < n ? alloc : n);
}

void Spc::readWrite(bool write, uint32_t lba, uint32_t count) {
    HardDiskImage* img = disk_[target_];
    const uint32_t blocks = img->blocks(), size = img->blockSize();
    if (lba > blocks || count > blocks - lba) {
        check(KEY_ILLEGAL, ASC_LBA_RANGE);
        return;
    }
    if (write && img->writeProtected()) {
        check(KEY_PROTECT, ASC_PROTECTED);
        return;
    }
    if (count == 0) {
        good();
        return;
    }
    cursor_.start(img, (uint64_t)lba * size, count * size);
    if (write) dataOut(SRC_DISK, count * size);
    else dataIn(SRC_DISK, count * size);
}

// ハードディスク (ダイレクトアクセス装置) のコマンド。Inside X68000 の図 24 の、SCSI のドライバが求める
// コマンドと、X3.131-1986 / SCSI-2 のダイレクトアクセス装置のコマンドのうち、データを変えずに答えられるもの
void Spc::executeDisk() {
    HardDiskImage* img = disk_[target_];
    switch (cdb_[0]) {
    case OP_TEST_UNIT_READY:
    case OP_REZERO_UNIT:
    case OP_START_STOP_UNIT:
    case OP_SEND_DIAGNOSTIC: // SEND DIAGNOSTIC
    case OP_PREVENT_ALLOW: // PREVENT ALLOW MEDIUM REMOVAL
    case OP_SYNCHRONIZE_CACHE: // SYNCHRONIZE CACHE
        good();
        break;
    case OP_FORMAT_UNIT: // FORMAT UNIT: 中身はそのまま (初期化のあとの中身は決まっていない)
        if (img->writeProtected()) check(KEY_PROTECT, ASC_PROTECTED);
        else good();
        break;
    case OP_READ6: // READ(6): 番地は 21 ビット、ブロック数 0 は 256
    case OP_WRITE6: // WRITE(6)
        readWrite(cdb_[0] == OP_WRITE6, be24(cdb_ + 1) & 0x1fffff, cdb_[4] ? cdb_[4] : 256);
        break;
    case OP_READ10: // READ(10): ブロック数 0 は、何も転送しない
    case OP_WRITE10: // WRITE(10)
    case OP_WRITE_VERIFY: // WRITE AND VERIFY
        readWrite(cdb_[0] != OP_READ10, be32(cdb_ + 2), be16(cdb_ + 7));
        break;
    case OP_SEEK6:
    case OP_SEEK10:
    case OP_VERIFY: {
        // VERIFY: データは転送しない。長さ 0 はエラーではない (SCSI-2 の 9.2.19)。範囲の見方は READ(10) と同じ。
        // SEEK は、ないブロックの番地ならエラー
        const bool verify = cdb_[0] == OP_VERIFY;
        const uint32_t lba = cdb_[0] == OP_SEEK6 ? be24(cdb_ + 1) & 0x1fffff : be32(cdb_ + 2);
        const uint32_t count = verify ? be16(cdb_ + 7) : 1;
        if (lba > img->blocks() || count > img->blocks() - lba) check(KEY_ILLEGAL, ASC_LBA_RANGE);
        else good();
        break;
    }
    case OP_MODE_SELECT6: dataOut(SRC_BUF, cdb_[4]); break;           // MODE SELECT(6): パラメータは受け取るだけ
    case OP_MODE_SELECT10: dataOut(SRC_BUF, be16(cdb_ + 7)); break;    // MODE SELECT(10)
    case OP_MODE_SENSE6:
    case OP_MODE_SENSE10: modeSense(false); break;
    case OP_READ_CAPACITY: // READ CAPACITY: 最後の論理ブロックの番地と、ブロックの長さ (Inside X68000 の図 31)
        put32(buf_, img->blocks() - 1);
        put32(buf_ + 4, img->blockSize());
        dataIn(SRC_BUF, 8);
        break;
    default: check(KEY_ILLEGAL, ASC_INVALID_COMMAND); break;
    }
}

// ---------------------------------------------------------------- CD-ROM 装置 (SCSI-2 の 14 章)

// READ: データトラックの中の、イメージに入っている範囲だけ読める。1 ブロックは 2048 バイト。
// 2352 バイト / セクタなどのイメージでは、セクタの中のユーザデータだけを渡す (x68k/cd_drive.h の CdTrack)。
// オーディオのトラックにかかれば BLANK CHECK / ILLEGAL MODE FOR THIS TRACK (SCSI-2 の表 71 の 64h)
void Spc::cdRead(uint32_t lba, uint32_t count) {
    const CdToc& toc = cd_->toc();
    if (lba >= toc.leadOut || count > toc.leadOut - lba) {
        check(KEY_ILLEGAL, ASC_LBA_RANGE);
        return;
    }
    const int t = toc.find(lba);
    if (t < 0 || toc.tracks[t].audio() || lba + count > toc.end(t)) {
        check(KEY_BLANK, ASC_ILLEGAL_MODE_FOR_TRACK);
        return;
    }
    const CdTrack& tr = toc.tracks[t];
    if (lba < tr.fileLba || lba + count > tr.fileEnd) {
        check(KEY_ILLEGAL, ASC_LBA_RANGE);
        return;
    }
    cd_->stop(); // 読むと再生は止まる
    cd_->setPosition(lba + count);
    if (count == 0) {
        good();
        return;
    }
    const uint64_t base = tr.offset + (uint64_t)(lba - tr.fileLba) * tr.sector + tr.dataOffset;
    if (tr.sector == 2048) cursor_.start(cd_->image(), base, count * 2048);
    else cursor_.start(cd_->image(), base, count * 2048, tr.sector, 2048);
    dataIn(SRC_DISK, count * 2048);
}

// READ TOC (SCSI-2 の 14.2.11、表 260、261): ヘッダ (長さ、最初と最後のトラック) と、開始トラックからのトラックごとの
// 8 バイト (ADR とコントロール、トラック番号、番地)、最後にリードアウト (トラック番号 AAh)
void Spc::cdReadToc() {
    const CdToc& toc = cd_->toc();
    const bool msf = (cdb_[1] & 0x02) != 0;
    const uint8_t start = cdb_[6];
    const uint32_t alloc = be16(cdb_ + 7);
    const uint8_t first = toc.count ? toc.tracks[0].number : 0, last = toc.count ? toc.tracks[toc.count - 1].number : 0;
    if (start > last && start != 0xaa) {
        check(KEY_ILLEGAL, ASC_BAD_CDB_FIELD);
        return;
    }
    uint8_t* b = cd_->buf();
    uint32_t n = 4;
    b[2] = first;
    b[3] = last;
    for (int i = 0; i <= toc.count; i++) {
        const bool leadOut = i == toc.count;
        const CdTrack& tr = toc.tracks[leadOut && toc.count ? toc.count - 1 : leadOut ? 0 : i]; // トラックがなくても配列の中
        if (!leadOut && tr.number < start) continue;
        uint8_t* e = b + n;
        e[0] = 0;
        e[1] = (uint8_t)(0x10 | (tr.control & 0x0f));
        e[2] = leadOut ? 0xaa : tr.number;
        e[3] = 0;
        putAddress(e + 4, leadOut ? toc.leadOut : tr.start, msf);
        n += 8;
    }
    put16(b, n - 2);
    dataIn(SRC_CD, alloc < n ? alloc : n);
}

// READ SUB-CHANNEL (SCSI-2 の 14.2.10、表 251 から 259): ヘッダ (オーディオステータス、長さ) と、SubQ = 1 なら形式ごとのデータ。
//   形式 00h: 今の位置と、媒体のカタログ番号、ISRC (どちらも持っていない)。48 バイト
//   形式 01h: 今の位置 (ADR とコントロール、トラック、インデックス、絶対位置、トラックの中の位置)。16 バイト
//   形式 02h: 媒体のカタログ番号、形式 03h: ISRC。24 バイト
void Spc::cdSubChannel() {
    const bool msf = (cdb_[1] & 0x02) != 0, subq = (cdb_[2] & 0x40) != 0;
    const uint8_t format = cdb_[3];
    const uint32_t alloc = be16(cdb_ + 7);
    uint32_t n = 4;
    if (subq) {
        if (format > 3) {
            check(KEY_ILLEGAL, ASC_BAD_CDB_FIELD);
            return;
        }
        n = format == 0 ? 48 : format == 1 ? 16 : 24;
    }
    memset(buf_, 0, 48);
    buf_[1] = cd_->audioStatus();
    put16(buf_ + 2, n - 4);
    if (subq) {
        const CdToc& toc = cd_->toc();
        buf_[4] = format;
        if (format <= 1) {
            // リードアウトまで進んだあとは、最後のブロックを今の位置として返す
            uint32_t pos = cd_->position();
            if (pos >= toc.leadOut && toc.leadOut) pos = toc.leadOut - 1;
            const int t = toc.find(pos);
            if (t >= 0) {
                const CdTrack& tr = toc.tracks[t];
                buf_[5] = (uint8_t)(0x10 | (tr.control & 0x0f));
                buf_[6] = tr.number;
                buf_[7] = pos < tr.start ? 0 : 1; // INDEX 01 の前 (プリギャップ) はインデックス 0
                putAddress(buf_ + 8, pos, msf);
                // トラックの中の位置は INDEX 01 から。プリギャップでは、LBA は負、MSF は INDEX 01 までの残り
                const uint32_t rel = pos - tr.start;
                if (msf) putMsf(buf_ + 12, pos < tr.start ? tr.start - pos : rel);
                else put32(buf_ + 12, rel);
            }
        } else if (format == 3) {
            buf_[6] = cdb_[6];
            for (int i = 0; i < toc.count; i++)
                if (toc.tracks[i].number == cdb_[6]) buf_[5] = (uint8_t)(0x10 | (toc.tracks[i].control & 0x0f));
        }
    }
    cd_->audioStatusRead(); // 「終わった」「エラーで止まった」は 1 回だけ返す
    dataIn(SRC_BUF, alloc < n ? alloc : n);
}

// [lba, end) を再生する。始まりがリードアウトより後ろなら LBA OUT OF RANGE、オーディオのトラックでなければ
// ILLEGAL MODE FOR THIS TRACK。終わりがリードアウトより後ろなら、リードアウトまで。
// エラーにしたときは、今の再生を止めない
void Spc::cdPlay(uint32_t lba, uint32_t end) {
    const CdToc& toc = cd_->toc();
    if (lba >= toc.leadOut) {
        check(KEY_ILLEGAL, ASC_LBA_RANGE);
        return;
    }
    if (!cd_->play(lba, end < toc.leadOut ? end : toc.leadOut)) {
        check(KEY_BLANK, ASC_ILLEGAL_MODE_FOR_TRACK);
        return;
    }
    good();
}

void Spc::executeCd() {
    const uint8_t op = cdb_[0];
    // 媒体を入れ替えたあとの最初のコマンドは UNIT ATTENTION (SCSI-2 の 7.9)。再生がエラーで止まっていたら、
    // 次のコマンドに遅れて知らせる (エラーコード 71h。SCSI-2 の 14.1.3)
    if (cd_->takeAttention()) {
        check(KEY_ATTENTION, ASC_MEDIUM_CHANGED);
        return;
    }
    uint8_t key = 0, asc = 0;
    if (cd_->takeDeferred(key, asc)) {
        check(key, asc, 0, 0x71);
        return;
    }
    // 媒体がなくても答えるコマンド
    switch (op) {
    case OP_MODE_SENSE6:
    case OP_MODE_SENSE10: modeSense(true); return;
    case OP_MODE_SELECT6: dataOut(SRC_BUF, cdb_[4]); return;        // MODE SELECT(6): パラメータは受け取るだけ
    case OP_MODE_SELECT10: dataOut(SRC_BUF, be16(cdb_ + 7)); return; // MODE SELECT(10)
    case OP_SEND_DIAGNOSTIC: good(); return;                           // SEND DIAGNOSTIC
    case OP_PREVENT_ALLOW: // Prevent (bit0) で取り出しを禁止する
        cd_->setPrevent((cdb_[4] & 1) != 0);
        good();
        return;
    case OP_START_STOP_UNIT: { // START STOP UNIT: LoEj (bit1) があればトレイを動かす。なければ、Start = 0 で再生を止める
        const bool loej = (cdb_[4] & 2) != 0, start = (cdb_[4] & 1) != 0;
        if (loej && !start && cd_->prevented()) {
            check(KEY_ILLEGAL, ASC_REMOVAL_PREVENTED, 0x02);
            return;
        }
        if (loej) cd_->setTray(!start);
        else if (!start) cd_->stop();
        good();
        return;
    }
    case OP_TEST_UNIT_READY:
    case OP_REZERO_UNIT:
    case OP_READ6:
    case OP_SEEK6:
    case OP_READ_CAPACITY:
    case OP_READ10:
    case OP_SEEK10:
    case OP_VERIFY:
    case OP_READ_SUB_CHANNEL:
    case OP_READ_TOC:
    case OP_PLAY_AUDIO10:
    case OP_PLAY_AUDIO_MSF:
    case OP_PLAY_TRACK_INDEX:
    case OP_PAUSE_RESUME:
    case OP_PLAY_AUDIO12: break;
    default: check(KEY_ILLEGAL, ASC_INVALID_COMMAND); return;
    }
    if (!cd_->ready()) {
        check(KEY_NOT_READY, ASC_NO_MEDIUM);
        return;
    }
    const CdToc& toc = cd_->toc();
    switch (op) {
    case OP_TEST_UNIT_READY: good(); break; // TEST UNIT READY
    case OP_REZERO_UNIT:                // REZERO UNIT
        cd_->stop();
        cd_->setPosition(0);
        good();
        break;
    case OP_READ6: cdRead(be24(cdb_ + 1) & 0x1fffff, cdb_[4] ? cdb_[4] : 256); break; // READ(6)
    case OP_READ10: cdRead(be32(cdb_ + 2), be16(cdb_ + 7)); break;                     // READ(10)
    case OP_SEEK6: // SEEK(6)
    case OP_SEEK10: { // SEEK(10): 再生は止まる
        const uint32_t lba = op == OP_SEEK6 ? be24(cdb_ + 1) & 0x1fffff : be32(cdb_ + 2);
        if (lba >= toc.leadOut) {
            check(KEY_ILLEGAL, ASC_LBA_RANGE);
            break;
        }
        cd_->stop();
        cd_->setPosition(lba);
        good();
        break;
    }
    case OP_VERIFY: { // VERIFY: 再生は止まる。長さ 0 はエラーではない (SCSI-2 の 9.2.19)
        const uint32_t lba = be32(cdb_ + 2), count = be16(cdb_ + 7);
        if (lba > toc.leadOut || count > toc.leadOut - lba) {
            check(KEY_ILLEGAL, ASC_LBA_RANGE);
            break;
        }
        cd_->stop();
        good();
        break;
    }
    case OP_READ_CAPACITY: // READ CAPACITY: 最後の論理ブロックと 2048
        put32(buf_, toc.leadOut - 1);
        put32(buf_ + 4, 2048);
        dataIn(SRC_BUF, 8);
        break;
    case OP_READ_SUB_CHANNEL: cdSubChannel(); break;
    case OP_READ_TOC: cdReadToc(); break;
    case OP_PLAY_AUDIO10:   // PLAY AUDIO(10): 始まりの論理ブロックと長さ (16 ビット)
    case OP_PLAY_AUDIO12: { // PLAY AUDIO(12): 長さが 32 ビット
        // 始まりが FFFFFFFFh なら今の位置から。長さ 0 は何もしない (エラーではない)。SCSI-2 の 14.2.2
        uint32_t lba = be32(cdb_ + 2);
        const uint32_t len = op == OP_PLAY_AUDIO10 ? be16(cdb_ + 7) : be32(cdb_ + 6);
        if (len == 0) {
            good();
            break;
        }
        if (lba == 0xffffffffu) lba = cd_->position();
        cdPlay(lba, len > 0xffffffffu - lba ? 0xffffffffu : lba + len);
        break;
    }
    case OP_PLAY_AUDIO_MSF: { // PLAY AUDIO MSF: 始まりと終わりを分・秒・フレームで。始まりが FF:FF:FF なら今の位置から
        const bool current = cdb_[3] == 0xff && cdb_[4] == 0xff && cdb_[5] == 0xff;
        const uint32_t s = (uint32_t)cdb_[3] * 60 * 75 + (uint32_t)cdb_[4] * 75 + cdb_[5];
        const uint32_t e = (uint32_t)cdb_[6] * 60 * 75 + (uint32_t)cdb_[7] * 75 + cdb_[8];
        if ((!current && s < 150) || e < 150) {
            check(KEY_ILLEGAL, ASC_LBA_RANGE);
            break;
        }
        const uint32_t lba = current ? cd_->position() : s - 150, end = e - 150;
        if (lba == end) { // 始まりと終わりが同じなら何もしない
            good();
            break;
        }
        if (lba > end) {
            check(KEY_ILLEGAL, ASC_BAD_CDB_FIELD);
            break;
        }
        cdPlay(lba, end);
        break;
    }
    case OP_PLAY_TRACK_INDEX: {
        // PLAY AUDIO TRACK INDEX (SCSI-2 の 14.2.5): 始まりのトラックとインデックスから、終わりのトラックの
        // 終わりのインデックスの最後のブロックまで。トラックとインデックスの正しい値は 1 から 99。
        // イメージから分かるインデックスは 0 (プリギャップ。14.1.1) と 1 だけなので、
        //   始まりのインデックス 0: プリギャップの始まりから (規格は 0 のときを決めていない)
        //   始まりのインデックス 1: プリギャップのあとの最初のブロックから
        //   始まりのインデックス 2 以上: トラックにないインデックス。SOTC = 0 なので、次のトラックの始まりから
        //     (エラーではない)。終わりが同じトラックか、次のトラックがなければ、鳴らすところがないので、
        //     何もせずに正常に終わる (PLAY AUDIO MSF の、始まりと終わりが同じときと同じ)
        //   終わりのインデックス: 0 でも 1 以上でも、終わりのトラックの終わりまで (0 は正しい値ではないが、
        //     0 のまま送るドライバでも鳴るように、受け付ける)
        // 終わりのトラックが最後より後ろなら、最後のトラックまで
        int first = -1, last = -1;
        for (int i = 0; i < toc.count; i++) {
            if (toc.tracks[i].number == cdb_[4]) first = i;
            if (toc.tracks[i].number <= cdb_[7]) last = i;
        }
        if (first < 0 || last < first) {
            check(KEY_ILLEGAL, ASC_BAD_CDB_FIELD);
            break;
        }
        const CdTrack& t = toc.tracks[first];
        const uint32_t lba = cdb_[5] == 0 ? t.begin : cdb_[5] == 1 ? t.start : toc.end(first);
        const uint32_t end = toc.end(last);
        if (lba >= end) { // ないインデックスから、同じトラックの終わりまで
            good();
            break;
        }
        cdPlay(lba, end);
        break;
    }
    case OP_PAUSE_RESUME: // PAUSE RESUME: 再生中でも一時停止中でもなければ COMMAND SEQUENCE ERROR
        if (cd_->pause((cdb_[8] & 1) != 0)) good();
        else check(KEY_ILLEGAL, ASC_SEQUENCE);
        break;
    default: check(KEY_ILLEGAL, ASC_INVALID_COMMAND); break;
    }
}

} // namespace x68k
