#include "x68k/fdc.h"

#include <string.h>

namespace x68k {

// コマンドのバイト数 (1 バイト目を含む)。0 は未定義のコマンド
static bool isScan(int c) { return c == 17 || c == 25 || c == 29; }

static const uint8_t kCmdLen[32] = {0, 0, 9, 3, 2, 9, 9, 2, 1, 9, 2, 0, 9, 6, 0, 3,
                                    0, 9, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 9, 0, 0};

enum : uint8_t {
    ST0_ABNORMAL = 0x40,
    ST0_INVALID = 0x80,
    ST0_SEEK_END = 0x20,
    ST0_EQUIP_CHECK = 0x10,
    ST0_NOT_READY = 0x08,
    ST1_EN = 0x80, // エンドオブシリンダ
    ST1_DE = 0x20, // CRC エラー
    ST1_OR = 0x10, // オーバーラン (DMA が止まった)
    ST1_ND = 0x04, // セクタが見つからない
    ST1_NW = 0x02, // 書き込み禁止
    ST1_MA = 0x01, // アドレスマークがない
    ST2_CM = 0x40, // デリーテッドデータ (READ DATA のとき) / 通常のデータ (READ DELETED のとき)
    ST2_DD = 0x20, // データの CRC エラー
    ST2_SH = 0x08, // スキャンの条件を満たした (SCAN EQUAL では一致した)
    ST2_SN = 0x04, // スキャンの条件を EOT まで満たさなかった
    ST2_MD = 0x01, // データのアドレスマークがない
};

void Fdc::reset() {
    phase_ = COMMAND;
    cmdLen_ = cmdNeed_ = 0;
    resultLen_ = resultPos_ = 0;
    st0_ = st1_ = st2_ = 0;
    for (int i = 0; i < DRIVES; i++) {
        seekSt0_[i] = 0;
        cylinder_[i] = 0;
        idIndex_[i] = 0;
    }
    irq_ = false;
    forceReady_ = false;
    nonDma_ = false;
    driveCtrl_ = 0;
    ejectMask_ = 0;
    access_ = -1;
    sector_ = nullptr;
    bufLen_ = bufPos_ = 0;
    sectorDone_ = false;
}

void Fdc::insert(int drive, FloppyImage* image) {
    if (drive < 0 || drive >= DRIVES) return;
    // 転送の途中のセクタは前のイメージを指しているので、書き込みを前のイメージ (解放されているかもしれない) に戻さない
    if (drive_[drive] != image && drive == us()) sector_ = nullptr;
    drive_[drive] = image;
    inserted_[drive] = image && image->loaded();
    ejectReq_ &= (uint8_t)~(1 << drive); // 前のディスクへの排出の指示は、新しいディスクには効かない
}

void Fdc::eject(int drive) {
    if (drive < 0 || drive >= DRIVES) return;
    if (drive == us()) sector_ = nullptr; // 取り出したイメージは解放されることがある
    drive_[drive] = nullptr;
    inserted_[drive] = false;
}

bool Fdc::takeEjectRequest(int drive) {
    bool r = (ejectReq_ >> drive) & 1;
    ejectReq_ &= (uint8_t)~(1 << drive);
    return r;
}

bool Fdc::seekPending() const {
    for (int d = 0; d < DRIVES; d++)
        if (seekSt0_[d]) return true;
    return false;
}

bool Fdc::ready(int drive) const {
    if (drive < 0 || drive >= DRIVES) return false;
    return inserted_[drive] || forceReady_;
}

// ---------------------------------------------------------------- レジスタ

uint8_t Fdc::read(uint32_t offset) {
    switch (offset & 0x07) {
    case 0x1: { // 状態: bit7 RQM、bit6 DIO (FDC→CPU)、bit5 EXM (DMA なしの実行中)、bit4 CB
        switch (phase_) {
        case RESULT: return 0xd0;
        case EXEC_READ:
            if (nonDma_) return dataReady() ? 0xf0 : 0x30;
            return 0x10;
        case EXEC_WRITE:
        case EXEC_WRITE_ID:
            if (nonDma_) return dataReady() ? 0xb0 : 0x30;
            return 0x10;
        default: return cmdLen_ > 0 ? 0x90 : 0x80;
        }
    }
    case 0x3: // データ
        if (phase_ == RESULT) {
            // 結果を読むと割り込みが消える。ただし SENSE INTERRUPT STATUS で返していないシークの終わりが残っていれば続く
            irq_ = seekPending();
            uint8_t v = result_[resultPos_++];
            if (resultPos_ >= resultLen_) phase_ = COMMAND;
            return v;
        }
        if (phase_ == EXEC_READ) {
            if (sectorDone_) advance();
            if (phase_ == EXEC_READ && bufPos_ < bufLen_) {
                uint8_t v = buf_[bufPos_++];
                if (bufPos_ >= bufLen_) sectorDone_ = true;
                return v;
            }
        }
        return 0x00;
    case 0x5: { // ドライブの状態: bit7 = 制御レジスタで選んだドライブにディスクが入っている
        uint8_t v = 0;
        for (int d = 0; d < 4; d++)
            if ((driveCtrl_ >> d) & 1 && inserted_[d]) v |= 0x80;
        return v;
    }
    default:
        return 0xff;
    }
}

void Fdc::write(uint32_t offset, uint8_t v) {
    switch (offset & 0x07) {
    case 0x3:
        if (phase_ == EXEC_WRITE || phase_ == EXEC_WRITE_ID) {
            if (sectorDone_) advance();
            if ((phase_ == EXEC_WRITE || phase_ == EXEC_WRITE_ID) && bufPos_ < bufLen_) {
                buf_[bufPos_++] = v;
                if (bufPos_ >= bufLen_) {
                    commitWrite();
                    sectorDone_ = true;
                }
            }
            return;
        }
        if (phase_ != COMMAND) return; // 結果を読み終える前のコマンドは受け付けない
        if (cmdLen_ == 0) {
            cmdNeed_ = kCmdLen[v & 0x1f];
            if (cmdNeed_ == 0) { // 未定義のコマンド: ST0 = 0x80 を返す
                result_[0] = ST0_INVALID;
                setResult(1, false);
                return;
            }
        }
        cmd_[cmdLen_++] = v;
        if (cmdLen_ >= cmdNeed_) {
            cmdLen_ = 0;
            execute();
        }
        break;
    case 0x5: {
        // ドライブ制御: bit0-3 ドライブの選択、bit5 排出、bit6 排出禁止、bit7 LED 点滅。
        // 選択のビットが 1 → 0 になったとき、そのドライブに対して実行される
        for (int d = 0; d < 4; d++) {
            if ((driveCtrl_ >> d) & 1 && !((v >> d) & 1)) {
                if ((driveCtrl_ & 0x20) && inserted_[d]) ejectReq_ |= (uint8_t)(1 << d);
                if (driveCtrl_ & 0x40) ejectMask_ |= (uint8_t)(1 << d);
                else ejectMask_ &= (uint8_t)~(1 << d);
            }
        }
        driveCtrl_ = v;
        break;
    }
    case 0x7:
        // アクセスするドライブ: bit7 モーター、bit1-0 ドライブ
        access_ = (v & 0x80) ? (v & 3) : -1;
        break;
    }
}

void Fdc::setResult(int n, bool interrupt) {
    resultLen_ = n;
    resultPos_ = 0;
    phase_ = n > 0 ? RESULT : COMMAND;
    if (interrupt) irq_ = true;
}

// ---------------------------------------------------------------- コマンド

void Fdc::execute() {
    int c = cmd_[0] & 0x1f;
    int u = us();
    st0_ = (uint8_t)(cmd_[1] & 7);
    st1_ = st2_ = 0;
    switch (c) {
    case 3: // SPECIFY
        nonDma_ = cmd_[2] & 1;
        setResult(0, false);
        break;
    case 4: { // SENSE DEVICE STATUS → ST3
        uint8_t st3 = (uint8_t)(cmd_[1] & 7);
        st3 |= 0x08; // 両面
        if (cylinder_[u] == 0) st3 |= 0x10;
        if (ready(u)) st3 |= 0x20;
        if (drive_[u] && drive_[u]->writeProtected()) st3 |= 0x40;
        result_[0] = st3;
        setResult(1, false);
        break;
    }
    case 7: // RECALIBRATE
        cylinder_[u] = 0;
        seekSt0_[u] = (uint8_t)(u | (ready(u) ? ST0_SEEK_END : (ST0_SEEK_END | ST0_ABNORMAL | ST0_NOT_READY)));
        if (u >= 2) seekSt0_[u] = (uint8_t)(u | ST0_SEEK_END | ST0_ABNORMAL | ST0_EQUIP_CHECK);
        setResult(0, true);
        break;
    case 15: // SEEK
        if (ready(u)) {
            cylinder_[u] = cmd_[2];
            seekSt0_[u] = (uint8_t)((cmd_[1] & 7) | ST0_SEEK_END);
        } else {
            seekSt0_[u] = (uint8_t)((cmd_[1] & 7) | ST0_SEEK_END | ST0_ABNORMAL | ST0_NOT_READY);
        }
        setResult(0, true);
        break;
    case 8: { // SENSE INTERRUPT STATUS: シークを終えたドライブを 1 台ずつ返す
        bool found = false;
        for (int d = 0; d < DRIVES && !found; d++) {
            if (!seekSt0_[d]) continue;
            result_[0] = seekSt0_[d];
            result_[1] = cylinder_[d];
            seekSt0_[d] = 0;
            found = true;
        }
        irq_ = seekPending(); // ほかのドライブの終わりが残っていれば割り込みが続く
        if (found) {
            setResult(2, false);
        } else {
            result_[0] = ST0_INVALID;
            setResult(1, false);
        }
        break;
    }
    case 10: { // READ ID
        int t = track();
        const FloppyImage* img = drive_[u];
        bool ok = false;
        if (!ready(u)) {
            st0_ |= ST0_ABNORMAL | ST0_NOT_READY;
        } else if (!img || img->sectorCount(t) == 0) {
            st0_ |= ST0_ABNORMAL;
            st1_ |= ST1_MA;
        } else {
            ok = true;
            int n = img->sectorCount(t);
            const FloppyImage::Sector* s = img->sector(t, idIndex_[u] % n);
            idIndex_[u] = (uint8_t)((idIndex_[u] + 1) % n);
            cmd_[2] = s->c;
            cmd_[3] = s->h;
            cmd_[4] = s->r;
            cmd_[5] = s->n;
        }
        if (!ok) {
            // 読めなかったときの ID は決まっていないので、今のシリンダとヘッドを返す
            // (前のコマンドのバイトを返さない)
            cmd_[2] = cylinder_[u];
            cmd_[3] = (uint8_t)hd();
            cmd_[4] = 0;
            cmd_[5] = 0;
        }
        result_[0] = st0_;
        result_[1] = st1_;
        result_[2] = st2_;
        result_[3] = cmd_[2];
        result_[4] = cmd_[3];
        result_[5] = cmd_[4];
        result_[6] = cmd_[5];
        setResult(7, true);
        break;
    }
    case 2:  // READ DIAGNOSTIC
    case 5:  // WRITE DATA
    case 6:  // READ DATA
    case 9:  // WRITE DELETED DATA
    case 12: // READ DELETED DATA
    case 13: // WRITE ID (フォーマット)
    case 17: // SCAN EQUAL
    case 25: // SCAN LOW OR EQUAL
    case 29: // SCAN HIGH OR EQUAL
        startReadWrite();
        break;
    default: // 未定義のコマンド (ここには来ない)
        st0_ |= ST0_ABNORMAL;
        endRw(false);
        break;
    }
}

void Fdc::startReadWrite() {
    int c = cmd_[0] & 0x1f;
    int u = us();
    sectorDone_ = false;
    lastSector_ = false;
    bufPos_ = bufLen_ = 0;
    if (!ready(u) || !drive_[u]) {
        st0_ |= ST0_ABNORMAL | ST0_NOT_READY;
        endRw(false);
        return;
    }
    if ((c == 5 || c == 9 || c == 13) && drive_[u]->writeProtected()) {
        st0_ |= ST0_ABNORMAL;
        st1_ |= ST1_NW;
        endRw(false);
        return;
    }
    if (c == 13) {
        // WRITE ID: N、SC (セクタ数)、GPL、D (埋める値)。1 セクタにつき C H R N の 4 バイトを受け取る
        int sc = cmd_[3];
        bufLen_ = sc * 4;
        if (bufLen_ <= 0 || bufLen_ > (int)sizeof buf_) {
            st0_ |= ST0_ABNORMAL;
            endRw(false);
            return;
        }
        phase_ = EXEC_WRITE_ID;
        return;
    }
    if (c == 2) idIndex_[u] = 0;
    if (!loadSector()) return;
    // SCAN 系は書き込みと同じく CPU (DMA) からデータを受け取り、セクタごとにディスクのデータと比べる
    phase_ = (c == 5 || c == 9 || isScan(c)) ? EXEC_WRITE : EXEC_READ;
}

// R を次のセクタへ進める (uPD765A のデータシートの「FUNCTIONAL DESCRIPTION OF COMMANDS」)。
// 読み書きは R + 1、SCAN 系は R + STP (「SCAN COMMANDS」: R + STP → R)。
// EOT のセクタのあとは、MT が 1 でヘッド 0 ならヘッド 1 の R = 1 へ移る。そうでなければシリンダの終わりで false。
// SCAN 系で STP = 2 のときに EOT を飛び越したら、EOT を読めないままインデックスホールが来るので false
// (データシートの例: 1〜26 のセクタを 21 から STP = 2 で読むと異常終了する)
bool Fdc::stepSector() {
    const int eot = cmd_[6];
    const int step = isScan(cmd_[0] & 0x1f) && cmd_[8] == 2 ? 2 : 1;
    if (cmd_[4] == eot) {
        if (!(cmd_[0] & 0x80) || hd() == 1) return false;
        cmd_[1] |= 0x04; // ヘッド 1 へ。ST0 の HD も変わる
        st0_ |= 0x04;
        cmd_[3] ^= 1;
        cmd_[4] = 1;
        return true;
    }
    if (cmd_[4] < eot && cmd_[4] + step > eot) return false;
    cmd_[4] = (uint8_t)(cmd_[4] + step);
    return true;
}

bool Fdc::loadSector() {
    int c = cmd_[0] & 0x1f;
    int u = us();
    FloppyImage* img = drive_[u];
    if (!img) {
        st0_ |= ST0_ABNORMAL | ST0_NOT_READY;
        endRw(false);
        return false;
    }
    const FloppyImage::Sector* s;
    for (;;) {
        int t = track(); // SK で飛ばして MT でヘッド 1 へ移ったら、トラックも変わる
        if (c == 2) { // READ DIAGNOSTIC: ID に関係なく先頭から順に読む
            s = img->sector(t, idIndex_[u]++);
            if (s && (s->c != cmd_[2] || s->h != cmd_[3] || s->r != cmd_[4] || s->n != cmd_[5])) st1_ |= ST1_ND;
        } else {
            s = img->find(t, cmd_[2], cmd_[3], cmd_[4], cmd_[5]);
        }
        if (!s) {
            st0_ |= ST0_ABNORMAL;
            st1_ |= img->sectorCount(t) == 0 ? ST1_MA : ST1_ND;
            endRw(false);
            return false;
        }
        // データのマークがコマンドと合わないセクタ: READ DATA と SCAN 系ではデリーテッドデータ、READ DELETED DATA では
        // 通常のデータ (データシートの READ DATA / READ DELETED DATA / SCAN COMMANDS の SK の説明)
        const bool mismatch = (c == 6 || isScan(c)) ? s->deleted : c == 12 ? !s->deleted : false;
        if (!mismatch) break;
        st2_ |= ST2_CM;
        if (!(cmd_[0] & 0x20)) {
            st0_ |= ST0_ABNORMAL;
            lastSector_ = true;
            break;
        }
        if (!stepSector()) {
            st0_ |= ST0_ABNORMAL;
            st1_ |= ST1_EN;
            endRw(false);
            return false;
        }
    }
    // 転送する長さ: N が 0 のときは DTL。SCAN 系の 9 バイト目は DTL ではなく STP なので、N = 0 なら 128 バイト。
    // データシートの表にある N は 0 から 7 まで。8 以上は下位 3 ビットで決める (N = 8 は 128 バイト。前からの動き)
    int len = cmd_[5] ? 128 << (cmd_[5] & 7) : isScan(c) ? 128 : cmd_[8];
    if (len > (int)s->size) len = (int)s->size;
    if (len > (int)sizeof buf_) len = (int)sizeof buf_;
    if (len <= 0) {
        // データのないセクタ (D88 で大きさが 0) や DTL = 0: 転送するものがないので、データのマークがないとして終える
        st0_ |= ST0_ABNORMAL;
        st1_ |= ST1_MA;
        st2_ |= ST2_MD;
        endRw(false);
        return false;
    }
    sector_ = s;
    bufLen_ = len;
    bufPos_ = 0;
    sectorDone_ = false;
    if (isScan(c)) memcpy(scanBuf_, img->data(s), (size_t)len);
    // ディスクのデータを読むコマンド (読み込みと SCAN 系) は、セクタの CRC エラーを結果に入れる
    if (c != 5 && c != 9) {
        if (!isScan(c)) memcpy(buf_, img->data(s), (size_t)len);
        if (s->status == 0xb0) { // D88: データの CRC エラー
            st0_ |= ST0_ABNORMAL;
            st1_ |= ST1_DE;
            st2_ |= ST2_DD;
            lastSector_ = true;
        } else if (s->status != 0 && s->status != 0x10) { // ID の CRC エラーなど
            st0_ |= ST0_ABNORMAL;
            st1_ |= ST1_DE;
            lastSector_ = true;
        }
    }
    return true;
}

void Fdc::commitWrite() {
    int u = us();
    FloppyImage* img = drive_[u];
    if (!img) return;
    if (phase_ == EXEC_WRITE_ID) {
        // フォーマット: 受け取った ID のセクタがイメージにあれば D の値で埋める (配置は変えられない)
        int t = track();
        for (int i = 0; i + 4 <= bufLen_; i += 4) {
            const FloppyImage::Sector* s = img->find(t, buf_[i], buf_[i + 1], buf_[i + 2], buf_[i + 3]);
            if (!s) {
                st0_ |= ST0_ABNORMAL;
                st1_ |= ST1_NW;
                continue;
            }
            img->noteWrite(s, nullptr, s->size, cmd_[5]);
            memset(img->data(s), cmd_[5], s->size);
            // フォーマットしたセクタは通常のデータのマークで、CRC も正しい: 前のデリーテッドデータのフラグと
            // CRC エラーなどの状態を消す (dirty フラグも setMark() が立てる)
            img->setMark(s, false);
        }
        return;
    }
    if (!sector_) return;
    if (isScan(cmd_[0] & 0x1f)) {
        compareScan();
        return;
    }
    img->noteWrite(sector_, buf_, (uint32_t)bufLen_);
    memcpy(img->data(sector_), buf_, (size_t)bufLen_);
    img->setMark(sector_, (cmd_[0] & 0x1f) == 9); // WRITE DELETED DATA ならデリーテッドデータ
}

// SCAN 系: CPU から受け取った 1 セクタ分 (buf_) を、ディスクのデータ (scanBuf_) とバイトごとに比べる
// (uPD765A のデータシートの「SCAN COMMANDS」と表 4)。
//   - どちらかが $FF のバイトは、いつも条件を満たす (マスク)
//   - セクタが条件を満たすのは、すべてのバイトが条件 (SCAN EQUAL は =、LOW OR EQUAL は ディスク <= CPU、
//     HIGH OR EQUAL は ディスク >= CPU) を満たすとき。データシートは「バイトごとに比べる」としか書いていないので、
//     バイトごとに条件を確かめると読んだ
//   - 満たしたら終わる。すべてのバイトが等しければ SH = 1、SN = 0。等しくないバイトがあれば SH = 0、SN = 0
//   - 満たさなければ SN = 1 で、R + STP のセクタへ進む。EOT のセクタでも満たさなければ、SN = 1 のまま終わる。
//     TC で終わるときも、満たしたセクタがなければ SN = 1 のままにする (SN は「条件を満たすセクタが見つからない」)
// 結果の C/H/R は、最後に比べたセクタのまま (データシートに SCAN 系の結果の R の決まりはない)
void Fdc::compareScan() {
    const int c = cmd_[0] & 0x1f;
    bool equal = true, met = true;
    for (int i = 0; i < bufLen_ && met; i++) {
        const uint8_t disk = scanBuf_[i], cpu = buf_[i];
        if (disk == 0xff || cpu == 0xff || disk == cpu) continue;
        equal = false;
        if (c == 17 || (c == 25 && disk > cpu) || (c == 29 && disk < cpu)) met = false;
    }
    if (met) {
        st2_ &= (uint8_t)~(ST2_SN | ST2_SH);
        if (equal) st2_ |= ST2_SH;
        lastSector_ = true;
        return;
    }
    st2_ |= ST2_SN; // 条件を満たすセクタがない
    // シリンダの最後のセクタ (EOT。MT が 1 でヘッド 0 なら、ヘッド 1 が残っている) まで比べた
    if (cmd_[4] == cmd_[6] && (!(cmd_[0] & 0x80) || hd() == 1)) lastSector_ = true;
}

// セクタを読み終えた (書き終えた) のに TC が来ないとき: 次のセクタへ進む
void Fdc::advance() {
    sectorDone_ = false;
    if (phase_ == EXEC_WRITE_ID || lastSector_) {
        endRw(phase_ == EXEC_WRITE_ID && !(st0_ & ST0_ABNORMAL));
        return;
    }
    // R が EOT に達したら、MT ならヘッド 1 の R=1 へ、そうでなければエンドオブシリンダで終わる
    if (!stepSector()) {
        st0_ |= ST0_ABNORMAL;
        st1_ |= ST1_EN;
        endRw(false);
        return;
    }
    loadSector();
}

bool Fdc::dataReady() {
    if (phase_ == EXEC_READ) {
        if (sectorDone_) advance();
        return phase_ == EXEC_READ && bufPos_ < bufLen_;
    }
    if (phase_ == EXEC_WRITE || phase_ == EXEC_WRITE_ID) {
        if (sectorDone_) advance();
        return (phase_ == EXEC_WRITE || phase_ == EXEC_WRITE_ID) && bufPos_ < bufLen_;
    }
    return false;
}

int Fdc::readData(uint8_t* dst, int n) {
    int done = 0;
    // dataReady() は読み終えたセクタのあとに次のセクタを用意する (EOT やエラーなら実行フェーズを終える)
    while (done < n && phase_ == EXEC_READ && dataReady()) {
        int k = bufLen_ - bufPos_;
        if (k > n - done) k = n - done;
        memcpy(dst + done, buf_ + bufPos_, (size_t)k);
        bufPos_ += k;
        done += k;
        if (bufPos_ >= bufLen_) sectorDone_ = true;
    }
    return done;
}

void Fdc::terminalCount() {
    if (phase_ != EXEC_READ && phase_ != EXEC_WRITE && phase_ != EXEC_WRITE_ID) return;
    if (phase_ == EXEC_WRITE_ID) {
        endRw(!(st0_ & ST0_ABNORMAL));
        return;
    }
    // 最後のセクタの途中で TC が来ても、そのセクタの終わりで止まる (書き込みは書いた分を残す)
    if (phase_ == EXEC_WRITE && bufPos_ > 0 && bufPos_ < bufLen_) {
        if (isScan(cmd_[0] & 0x1f)) {
            // SCAN 系: 受け取った分だけを比べる
            bufLen_ = bufPos_;
        } else {
            memset(buf_ + bufPos_, 0, (size_t)(bufLen_ - bufPos_));
        }
        commitWrite();
    }
    // このセクタで終わると決まっている (デリーテッドデータ、CRC エラー) ときと SCAN 系では、結果の C/H/R を進めない。
    // SCAN 系は、TC が来たら比べている途中のバイトまでで終わる (データシートの「SCAN COMMANDS」)
    if (isScan(cmd_[0] & 0x1f) || lastSector_) {
        endRw(!(st0_ & ST0_ABNORMAL));
        return;
    }
    // 結果の C/H/R は「次のセクタ」を指す。EOT で終わったときは R = 1 で、
    // MT なら H の最下位ビットを反転し、ヘッド 1 で終わったときは C + 1 (ヘッド 0 なら C はそのまま)
    if (cmd_[4] == cmd_[6]) {
        bool mt = (cmd_[0] & 0x80) != 0;
        if (!mt || hd() == 1) cmd_[2]++;
        if (mt) cmd_[3] ^= 1;
        cmd_[4] = 1;
    } else {
        cmd_[4]++;
    }
    endRw(!(st0_ & ST0_ABNORMAL));
}

// DMA がバスエラーやソフトウェアアボートで止まった: 実行フェーズをオーバーランとして異常終了する。
// 書き込みは途中のセクタを捨てる (イメージを書き換えない)。結果の C/H/R は今のセクタを指す
void Fdc::abortTransfer() {
    if (phase_ != EXEC_READ && phase_ != EXEC_WRITE && phase_ != EXEC_WRITE_ID) return;
    st0_ |= ST0_ABNORMAL;
    st1_ |= ST1_OR;
    endRw(false);
}

void Fdc::endRw(bool normal) {
    if (normal) st0_ &= (uint8_t)~0xc0;
    result_[0] = st0_;
    result_[1] = st1_;
    result_[2] = st2_;
    result_[3] = cmd_[2];
    result_[4] = cmd_[3];
    result_[5] = cmd_[4];
    result_[6] = cmd_[5];
    sector_ = nullptr;
    bufLen_ = bufPos_ = 0;
    sectorDone_ = false;
    if (!(st0_ & 0xc0)) transfersOk_++;
    setResult(7, true);
}

} // namespace x68k
