#include "x68k/fuk.h"

#include <string.h>

namespace x68k {

// コマンドのバイト数 (1 バイト目を含む)。表にないコマンドは 1 バイト
static const uint8_t kCmdLen[32] = {1, 1, 9, 3, 2, 9, 9, 2, 1, 9, 2, 1, 9, 6, 1, 3,
                                    1, 9, 1, 1, 1, 1, 1, 1, 1, 9, 1, 1, 1, 9, 1, 1};

enum : uint8_t {
    CMD_READ_DIAGNOSTIC = 0x02,
    CMD_SPECIFY = 0x03,
    CMD_SENSE_DEVICE = 0x04,
    CMD_WRITE_DATA = 0x05,
    CMD_READ_DATA = 0x06,
    CMD_RECALIBRATE = 0x07,
    CMD_SENSE_INTERRUPT = 0x08,
    CMD_WRITE_DELETED = 0x09,
    CMD_READ_ID = 0x0a,
    CMD_READ_DELETED = 0x0c,
    CMD_WRITE_ID = 0x0d,
    CMD_SEEK = 0x0f,
};

enum : uint8_t {
    REG_STATUS = 0x01,
    REG_DATA = 0x03,
    REG_DRIVE_CONTROL = 0x05,
    REG_DRIVE_SELECT = 0x07,
    REG_IOC = 0x10,
    REG_FORCE_READY = 0x23,
    REG_COUNT_LOW = 0x30,
    REG_COUNT_HIGH = 0x31,
    BYTE_RESET = 0x40,
};

enum : uint8_t {
    ST0_ABNORMAL = 0x40,
    ST0_INVALID = 0x80,
    ST0_SEEK_END = 0x20,
    ST0_NOT_READY = 0x08,
    ST1_OVERRUN = 0x10,
    MSR_RQM = 0x80,
    MSR_DIO = 0x40,
    MSR_EXM = 0x20,
    MSR_CB = 0x10,
};

static bool isReadType(int op) { return op == CMD_READ_DATA || op == CMD_READ_DELETED || op == CMD_READ_DIAGNOSTIC; }
// データを FUK に送るコマンド (SCAN もデータを送る)
static bool isWriteType(int op) {
    return op == CMD_WRITE_DATA || op == CMD_WRITE_DELETED || op == CMD_WRITE_ID || op == 0x11 || op == 0x19 || op == 0x1d;
}

Fuk::Fuk(FukLink& link, Host* host) : link_(link), host_(host) {
    data_ = new uint8_t[BUF];
    wbuf_ = new uint8_t[BUF];
    dataAt_ = new uint32_t[BUF];
    memset(data_, 0, BUF);
    memset(wbuf_, 0, BUF);
}

Fuk::~Fuk() {
    delete[] data_;
    delete[] wbuf_;
    delete[] dataAt_;
}

// ---------------------------------------------------------------- ポート 1 に送るもの

bool Fuk::push(uint8_t kind, uint8_t reg, uint8_t val) {
    if (qHead_ - qTail_ >= QUEUE) {
        // 入らない。1 つでも落とすと FUK と状態がずれるので、次の service() で見分けからやり直す
        queueOverflow_ = true;
        return false;
    }
    q_[qHead_++ & (QUEUE - 1)] = Op{kind, reg, val};
    return true;
}

// X68000 の書き込みを並べる。返事を待っている間に書き込みがたまって残りが少なくなったら、空くのを待つ
// (こちらが送る読み出しの分は空けておく)。WAIT_US 待っても空かなければ、push() が見分けのやり直しを頼む
void Fuk::pushWrite(uint8_t reg, uint8_t v) {
    if (queueOverflow_) return; // もう見分けからやり直すと決めている (次の service() で)。待ち直さない
    if (qHead_ - qTail_ >= QUEUE - QUEUE_RESERVE) {
        stats_.queueWaits++;
        if (!waitFor([this] { return qHead_ - qTail_ < QUEUE - QUEUE_RESERVE; })) {
            // 待っても空かない。書き込みごとに待ち直すと X68000 が長く止まるので、1 回で諦める
            queueOverflow_ = true;
            return;
        }
    }
    push(OP_WRITE, reg, v);
}

// 'M' とデータを、まだ装置が書き終えていない。口 (ベアメタル版はリング、ホスト版は OS のバッファ) が空になっても、
// 装置の中のバッファに残っていて、装置がポート 1 を先に読むことがある。そこにリセットが届くと、残りのデータが
// 通知用の入力として読まれ、書き込みが失われる。そのため 'w' (書き込みの終わり) か 't' (中止) が届くまで「残っている」
// とみなす。データが口から出きってから WRITE_TIMEOUT_US たっても来なければ、装置が止まっているとみなして諦める
bool Fuk::tailPending() {
    if (wSent_ < wTotal_) return true;
    if (wFlushing_) {
        if (!link_.sendIdle(p2_)) return true;
        wFlushing_ = false;
        wSince_ = now(); // 'w' を待つ時間は、データが口から出きったときから数える
    }
    if (wAwaitDone_ && now() - wSince_ >= WRITE_TIMEOUT_US) wAwaitDone_ = false; // 時間切れは checkTimeouts() が数える
    return wAwaitDone_;
}

void Fuk::scanDone(const uint8_t* p, unsigned n) {
    if (!wAwaitDone_) return;
    for (unsigned i = 0; i < n; i++)
        if (p[i] == 'w' || p[i] == 't') wAwaitDone_ = false;
}

void Fuk::pushRead(uint8_t reg, uint8_t tag) {
    if (push(OP_READ, reg, tag)) stats_.reads++;
}

// 送れるところまで送る。読み出しを送ったら、その返事が届くまで次は送らない (返事は送った順にしか来ない)。
// 口が一度に受け付けなかった分は out1_ に残し、次に呼ばれたときに続きを送る (Linux の USB シリアルは、
// 送信中の書き込みが 16 個たまると、それ以上を受け付けない)
void Fuk::pump() {
    if (!ready()) return;
    // やめた書き込みの 'M' とデータを装置が書き終える ('w') までは、ポート 1 に何も送らない (リセットを先に届けない)。
    // 'w' の代わりに 't' が来たときと、WRITE_TIMEOUT_US たっても 'w' が来ないときは待つのをやめる (tailPending())
    if (phase_ != P_WRITE_BUSY && tailPending()) return;
    for (;;) {
        if (out1Pos_ < out1Len_) {
            out1Pos_ += link_.send(p1_, out1_ + out1Pos_, out1Len_ - out1Pos_);
            if (out1Pos_ < out1Len_) return;
        }
        out1Pos_ = out1Len_ = 0;
        if (awaiting_ || ls_ == L_SETTLE) return;
        while (qHead_ != qTail_ && out1Len_ + 2 <= sizeof out1_) {
            const Op op = q_[qTail_++ & (QUEUE - 1)];
            if (op.kind == OP_WRITE) {
                out1_[out1Len_++] = (uint8_t)(0x80 | op.reg);
                out1_[out1Len_++] = op.val;
            } else if (op.kind == OP_RESET) {
                out1_[out1Len_++] = BYTE_RESET;
            } else {
                out1_[out1Len_++] = op.reg;
                awaiting_ = true;
                awaitTag_ = op.val;
                awaitSince_ = awaitSent_ = now();
                break;
            }
        }
        if (out1Len_ == 0) return;
    }
}

void Fuk::requestMsr() {
    if (!ready() || msrPending_) return;
    msrPending_ = true;
    msrRequery_ = false;
    pushRead(REG_STATUS, T_MSR);
}

// 覚えているステータスを捨てて、読み直す。問い合わせを送ったあとなら、その返事は古いので使わない
void Fuk::invalidateMsr() {
    msrKnown_ = false;
    if (msrPending_) msrRequery_ = true;
    else requestMsr();
}

// ---------------------------------------------------------------- つながり

void Fuk::clearCommand() {
    phase_ = P_IDLE;
    phaseSince_ = now();
    cmdLen_ = cmdNeed_ = 0;
    cmdLocal_ = false;
    dmaAborted_ = false;
    resLen_ = resPos_ = resWant_ = 0;
    resIrq_ = resFetching_ = maskDevIrq_ = false;
    dataLen_ = dataPos_ = 0;
    streaming_ = readTc_ = readAbort_ = timed_ = false;
}

void Fuk::setPhase(Phase p) {
    phase_ = p;
    phaseSince_ = now();
    if (p == P_IDLE) maskDevIrq_ = false;
    if (p != P_READ_DATA) streaming_ = readTc_ = readAbort_ = timed_ = false;
    if (p == P_WRITE_DATA || p == P_IDLE) firstEmu_ = 0; // TC の長さは、Read のデータとリザルトの間だけ測る
}

// ポート 1 に送るものと、返事を待っている状態を捨てる
void Fuk::dropQueue() {
    qHead_ = qTail_ = 0;
    queueOverflow_ = false;
    out1Pos_ = out1Len_ = 0;
    awaiting_ = false;
    msrPending_ = msrRequery_ = msrKnown_ = false;
    resFetching_ = false;
    memset(drivePending_, 0, sizeof drivePending_);
    memset(driveKnown_, 0, sizeof driveKnown_);
    rx2State_ = 0;
    devIrq_ = fddIrq_ = false;
    seekBusy_ = 0;
    sisLocal_ = false;
    forceReadySent_ = false;
    // FDC のシリンダの覚えが消える (見分けからやり直すか、リセットする)
    memset(lastCyl_, -1, sizeof lastCyl_);
    resetCount_++;
}

// SEEK / RECALIBRATE を送ったまま、終わりの通知 ('C') を受け取れなくなった。X68000 は割り込みを待っているので、
// こちらで異常終了 (ノットレディ) のシークの終わりを作る (SENSE INTERRUPT STATUS で引き取れる)
void Fuk::failSeeks() {
    for (int d = 0; d < 4; d++) {
        if (!((seekBusy_ >> d) & 1)) continue;
        localSeek_[d] = (uint8_t)(ST0_SEEK_END | ST0_ABNORMAL | ST0_NOT_READY | d);
        localIrq_ = true;
    }
    seekBusy_ = 0;
}

bool Fuk::localSeekPending() const { return (localSeek_[0] | localSeek_[1] | localSeek_[2] | localSeek_[3]) != 0; }

void Fuk::goOffline(int reason) {
    const bool was = ready();
    if (was && reason >= 0) noteLinkLost((uint8_t)reason);
    failCommand();
    failSeeks();
    dropQueue();
    ls_ = L_OFFLINE;
    retryAt_ = now() + RETRY_US;
    if (was) linkChanged();
}

// 見分けからやり直す (つないだとき、返事が来なくなったとき)
void Fuk::relink(int reason) {
    const bool was = ready();
    if (was && reason >= 0) noteLinkLost((uint8_t)reason);
    failCommand();
    failSeeks();
    dropQueue();
    ls_ = L_DRAIN;
    lsSince_ = lastRx_ = now();
    if (was) linkChanged();
}

void Fuk::serviceLink() {
    if (!link_.online()) {
        if (ls_ != L_OFFLINE) goOffline(Event::UNPLUGGED);
        wasOnline_ = false;
        return;
    }
    const unsigned epoch = link_.epoch();
    if (!wasOnline_ || epoch != epoch_) {
        if (epoch != epoch_) {
            // 装置が USB からいったん消えて戻った。前の 'M' とデータの送り残しは送らない (装置が初めの状態に
            // 戻ったとみなす。電源が保たれたままなら違うことがある。FukLink::epoch() を参照)。同じ装置との通信が
            // 一度切れて戻っただけ (epoch が同じ) なら、残りを送り切ってから見分ける
            wSent_ = wTotal_ = 0;
            wFlushing_ = wAwaitDone_ = false;
        }
        wasOnline_ = true;
        epoch_ = epoch;
        probeFailed_ = false;
        relink(Event::NEW_DEVICE);
    }
    const uint64_t t = now();
    uint8_t buf[64];
    switch (ls_) {
    case L_OFFLINE:
        if (t >= retryAt_) relink();
        break;
    case L_DRAIN: {
        // 前のやり取りの残りを捨てる。しばらく何も届かなくなってから (DRAIN_MAX_US たったら届き続けていても)、両方のポートに 0x00 を送る
        for (int port = 0; port < 2; port++) {
            unsigned n;
            while ((n = link_.receive(port, buf, sizeof buf)) != 0) {
                lastRx_ = t;
                if (port == p2_) scanDone(buf, n);
            }
        }
        // 'M' とデータの送り残しがある間は、見分けに進まない。FUK は決めたバイト数を受け取るまでポート 2 を
        // データとして読むので、残りを捨てて 0x00 を送ると、それがディスクに書かれる。送り切れるまで
        // (か、装置が抜かれるまで) 待つ。その間、X68000 にはつながっていないときと同じに答える
        if (tailPending()) {
            lsSince_ = lastRx_ = t; // 送り切ってから、静かになるのを待ち直す
            break;
        }
        if (t - lastRx_ < DRAIN_QUIET_US && t - lsSince_ < DRAIN_MAX_US) break;
        probe_[0] = probe_[1] = -1;
        probeSent_[0] = probeSent_[1] = false;
        ls_ = L_PROBE;
        lsSince_ = t;
        break;
    }
    case L_PROBE: {
        // 両方に 0x00 を送る (口が受け付けなければ、次に呼ばれたときに送り直す)。
        // 'f' を返したほうがポート 1 (レジスタ)、'F' を返したほうがポート 2 (通知とデータ)
        const uint8_t zero = 0;
        for (int port = 0; port < 2; port++) {
            if (!probeSent_[port]) probeSent_[port] = link_.send(port, &zero, 1) == 1;
            if (probeSent_[port] && probe_[port] < 0 && link_.receive(port, buf, 1) == 1) probe_[port] = buf[0];
        }
        const bool both = probe_[0] >= 0 && probe_[1] >= 0;
        if (!both && t - lsSince_ < PROBE_TIMEOUT_US) break;
        if (both && ((probe_[0] == 'f' && probe_[1] == 'F') || (probe_[0] == 'F' && probe_[1] == 'f'))) {
            p1_ = probe_[0] == 'f' ? 0 : 1;
            p2_ = 1 - p1_;
            // 初期化とリセット。そのあとステータスを 1 回読み、その返事が届くまでに届いた通知は捨てる。
            // 口が一度に受け付けなくても続きを送れるよう、ほかの送信と同じ置き場 (out1_) から送る
            // (2 バイトの書き込みの途中で切ると、FUK は次に届いたバイトを値として読む)
            out1_[0] = 0x80;
            out1_[1] = 0xfe;
            out1_[2] = BYTE_RESET;
            out1Len_ = 3;
            out1Pos_ = 0;
            ls_ = L_SYNC;
            lsSince_ = t;
            pushRead(REG_STATUS, T_SYNC);
            probeFailed_ = false;
            stats_.links++;
            linkChanged();
        } else {
            stats_.linkFailures++;
            ls_ = L_OFFLINE;
            retryAt_ = t + RETRY_US;
            if (!probeFailed_) linkChanged(); // 続けて失敗している間は、1 回だけ知らせる
            probeFailed_ = true;
        }
        break;
    }
    case L_SETTLE:
        if (t - lastRx_ >= SETTLE_QUIET_US) ls_ = L_READY;
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- 受信

void Fuk::receivePort1() {
    uint8_t buf[16];
    unsigned n;
    while (ready() && (n = link_.receive(p1_, buf, sizeof buf)) != 0) {
        for (unsigned i = 0; i < n && ready(); i++) {
            if (!awaiting_) {
                noteStray(Event::S_PORT1, buf[i]);
                continue;
            }
            if (out1Pos_ < out1Len_) noteOdd(Event::ODD_EARLY, buf[i]); // 読み出しの要求は out1_ の最後にある
            awaiting_ = false;
            const uint64_t took = now() - awaitSent_; // awaitSince_ は、止められていた時間を足すので使わない
            stats_.replies++;
            stats_.replyUs += took;
            if (took > stats_.replyMaxUs) stats_.replyMaxUs = (uint32_t)took;
            reply(replyTag_ = awaitTag_, buf[i]);
            replyAt_ = now();
        }
    }
}

void Fuk::reply(uint8_t tag, uint8_t v) {
    if (tag >= T_DRIVE) {
        const int sel = tag & 15;
        if (drivePending_[sel] != 0) drivePending_[sel]--;
        driveStat_[sel] = v;
        driveKnown_[sel] = true;
        driveAt_[sel] = now();
        return;
    }
    switch (tag) {
    case T_SYNC:
        // リセットが済んだ。リセットの前の 'R' の残りを捨てるため、ポート 2 に SETTLE_QUIET_US 何も届かなくなるまで
        // 捨て続ける (serviceLink()。静かになったら届き終わったとみなす)
        ls_ = L_SETTLE;
        lastRx_ = now();
        if ((v & (MSR_RQM | MSR_DIO)) != MSR_RQM) noteOdd(Event::ODD_SYNC, v);
        msr_ = v;
        msrKnown_ = true;
        msrAt_ = lastRx_;
        return;
    case T_STALE:
        return;
    case T_DIRECT:
        directDone_ = true;
        directValue_ = v;
        return;
    case T_MSR:
        msrPending_ = false;
        if (msrRequery_) { // 問い合わせたあとにコマンドを送った。この値は古い
            requestMsr();
            return;
        }
        msr_ = v;
        msrKnown_ = true;
        msrAt_ = now();
        if (phase_ == P_IDLE && cmdLen_ == 0 && (v & (MSR_RQM | MSR_DIO)) == (MSR_RQM | MSR_DIO)) noteOdd(Event::ODD_STATUS, v);
        if (phase_ != P_RESULT_WAIT || resFetching_) return;
        if ((v & (MSR_RQM | MSR_DIO)) == (MSR_RQM | MSR_DIO)) {
            // リザルトが用意できた。数が分かっているコマンドは、続けてその数だけ読む
            if (resWant_ > 0) setPhase(P_RESULT_FETCH);
            resFetching_ = true;
            pushRead(REG_DATA, T_RESULT);
        } else if ((v & (MSR_RQM | MSR_DIO)) == MSR_RQM) {
            // コマンドを待っている: リザルトは (もう) ない
            if (resLen_ > 0) enterResult(resIrq_);
            else setPhase(P_IDLE);
        }
        return;
    case T_RESULT:
        resFetching_ = false;
        if (phase_ != P_RESULT_FETCH && phase_ != P_RESULT_WAIT) {
            noteStray(Event::S_RESULT, v);
            return;
        }
        if (resLen_ < (int)sizeof res_) res_[resLen_++] = v;
        if (phase_ == P_RESULT_FETCH) {
            if ((cmd_[0] & 0x1f) == CMD_SENSE_INTERRUPT && resLen_ == 1) {
                // ST0 が「不正なコマンド」(引き取る割り込みがない) なら 1 バイトだけ。そうでなければ次に PCN が続く
                if ((v & 0xc0) == ST0_INVALID) resWant_ = 1;
                else if (v & ST0_SEEK_END) seekBusy_ &= (uint8_t)~(1u << (v & 3));
            }
            if (resLen_ >= resWant_) {
                checkResult();
                enterResult(resIrq_);
            } else {
                resFetching_ = true;
                pushRead(REG_DATA, T_RESULT);
            }
        } else if (resLen_ >= (int)sizeof res_) {
            enterResult(resIrq_);
        } else {
            invalidateMsr(); // まだ続きがあるかを、ステータスで確かめる
        }
        return;
    }
}

void Fuk::receivePort2() {
    uint8_t buf[256];
    for (;;) {
        if (!ready()) return;
        if (ls_ != L_READY) { // リセットの前の通知とデータは捨てる (書き込みの終わりは拾う)
            const unsigned n = link_.receive(p2_, buf, sizeof buf);
            if (n == 0) return;
            scanDone(buf, n);
            lastRx_ = now();
            continue;
        }
        if (rx2State_ == 3) {
            // 'R' のデータは、置き場にそのまま受け取る
            const unsigned n = link_.receive(p2_, data_ + frameGot_, frameLen_ - frameGot_);
            if (n == 0) return;
            // 届いた時刻 (このまとまりの最後のバイトのもの) を、'R' からの us で覚える
            const uint64_t at = link_.receivedAtUs(p2_);
            const uint64_t rel = at > frameAt_ ? at - frameAt_ : 0;
            const uint32_t rel32 = rel < 0xffffffffu ? (uint32_t)rel : 0xffffffffu;
            for (unsigned i = 0; i < n; i++) dataAt_[frameGot_ + i] = rel32;
            if (timed_ && leadFixed_ && phase_ == P_READ_DATA && frameGot_ < dataLen_) {
                // 渡す時刻を過ぎてから届いた (遅れが足りなかった)。X68000 から見た間隔が、その分ずれる
                const uint64_t due = anchorEmu_ + leadNs_ + (uint64_t)(rel32 > dataAt_[0] ? rel32 - dataAt_[0] : 0) * 1000;
                const uint64_t t = host_->fukEmuNs();
                if (t > due + 64000) { // 1 ラスタ (64 us) までは数えない
                    if (!late_) stats_.lateReads++;
                    late_ = true;
                    const uint64_t us = (t - due) / 1000;
                    if (us > stats_.lateMaxUs) stats_.lateMaxUs = us > 0xffffffffu ? 0xffffffffu : (uint32_t)us;
                }
            }
            frameGot_ += n;
            if (frameGot_ >= frameLen_) frameDone();
            else if (phase_ == P_READ_WAIT) startStream();
            continue;
        }
        if (link_.receive(p2_, buf, 1) == 0) return;
        port2Byte(buf[0]);
    }
}

// 'R' のデータが届き始めた。DMA の Read なら、届いた分から DMAC に渡し始める (#320)。DMAC の転送バイト数 (MTC) は、
// FUK から届いたのに合わせて減る。DMA を使わない Read (CPU が 1 バイトずつ読む) は、今までどおり全部届いてから渡す
void Fuk::startStream() {
    if (dmaAborted_ || nonDma_ || noDma_ || frameLen_ <= 7) return;
    dataLen_ = frameLen_ - 7;
    dataPos_ = 0;
    setPhase(P_READ_DATA);
    streaming_ = true;
    stats_.streamedReads++;
    beginRead();
}

// DMA の Read のデータを渡し始める。X68000 は 1 フレーム分をまとめて実時間より速く動かしてから休むので、受け取った
// ときにすぐ渡すと、休んでいる間に届いたバイトが次のフレームの初めにまとめて渡り、X68000 から見た間隔がフレームの
// 単位に丸められる。そこで i バイト目を、X68000 の時間で「最初のバイトを受け取った時刻 + 遅れ + (i バイト目が届いた
// 時刻 - 最初のバイトが届いた時刻)」に渡す。届いた時刻の間隔をそのまま写すだけで、伸ばしたり詰めたりはしない。
// 遅れは、まだ届いていないバイトの渡す時刻が、届くより前にならないためのもの。X68000 は実時間より多くて 1 フレーム
// 先に進むので、遅れは多くて「1 フレーム + 余裕 (lead) - 最初のバイトが届いてから今までの時間」(leadMax_)。それより
// 前に 'R' が全部届いたら、すべての届いた時刻が分かっているので、そこで遅れを決めて渡し始める (dataAvail())。
// 遅れは Read ごとに 1 回だけかかる。fuk_lead= (setLeadUs()) を決めたときは、届き方によらず、いつもその値にする
void Fuk::beginRead() {
    timed_ = false;
    firstEmu_ = 0;
    availPos_ = 0;
    if (host_ == nullptr || nonDma_ || noDma_) return;
    anchorEmu_ = host_->fukEmuNs();
    if (anchorEmu_ == 0) return;
    if (leadUs_ != 0) {
        // fuk_lead= で決めた値: 届き方によらず、いつもこの遅れ
        leadMax_ = (uint64_t)leadUs_ * 1000;
    } else {
        const uint64_t frame = host_->fukFrameNs() != 0 ? host_->fukFrameNs() : DEFAULT_FRAME_NS;
        const uint64_t lead = frame + frame / 8;
        const uint64_t firstAt = frameAt_ + dataAt_[0], t = link_.nowUs();
        const uint64_t since = t > firstAt ? (t - firstAt) * 1000 : 0; // 最初のバイトが届いてから (ns)
        leadMax_ = since >= lead ? 0 : lead - since;
    }
    leadNs_ = 0;
    leadFixed_ = late_ = false;
    timed_ = true;
    stats_.timedReads++;
}

void Fuk::frameDone() {
    rx2State_ = 0;
    stats_.readFrames++;
    if (timed_ && phase_ == P_READ_DATA && frameLen_ > 7) stats_.spanUs += dataAt_[frameLen_ - 8] - dataAt_[0];
    if (streaming_ && phase_ == P_READ_DATA) {
        // 届いている途中から渡していた Read の終わり。データのあとの 7 バイトがリザルト
        streaming_ = false;
        memcpy(res_, data_ + dataLen_, 7);
        resLen_ = 7;
        sisLocal_ = true;
        if (readAbort_) {
            resSource_ = Event::DMA_ABORT;
            res_[0] = (uint8_t)((res_[0] & 0x3f) | ST0_ABNORMAL);
            res_[1] |= ST1_OVERRUN;
        }
        // TC か DMA の異常終了のあとか、DMAC がデータを全部読み終えていれば、リザルトにする。そうでなければ、
        // 残りを渡し終えたとき (readData()) にリザルトにする
        if (readTc_ || readAbort_ || dataPos_ >= dataLen_) enterResult(true);
        return;
    }
    if (phase_ != P_READ_WAIT) { // 待っていない (時間切れのあとに届いたなど)
        noteStray(Event::S_R, frameLen_);
        return;
    }
    if (frameLen_ < 7) {
        failCommand();
        return;
    }
    // データのあとの 7 バイトがリザルト
    dataLen_ = frameLen_ - 7;
    dataPos_ = 0;
    memcpy(res_, data_ + dataLen_, 7);
    resLen_ = 7;
    sisLocal_ = true;
    if (dmaAborted_ && dataLen_ != 0) {
        // 'R' を待っている間に DMA が異常終了している。データを渡す相手がいないので、オーバーランで終わる
        resSource_ = Event::DMA_ABORT;
        res_[0] = (uint8_t)((res_[0] & 0x3f) | ST0_ABNORMAL);
        res_[1] |= ST1_OVERRUN;
        enterResult(true);
    } else if (dataLen_ != 0) {
        setPhase(P_READ_DATA);
        beginRead();
        if (timed_) stats_.spanUs += dataAt_[dataLen_ - 1] - dataAt_[0];
    } else {
        enterResult(true);
    }
}

void Fuk::port2Byte(uint8_t b) {
    if (rx2State_ == 1) {
        frameLen_ = b;
        rx2State_ = 2;
        return;
    }
    if (rx2State_ == 2) {
        frameLen_ |= (uint32_t)b << 8;
        frameGot_ = 0;
        rx2State_ = 3;
        if (frameLen_ == 0) frameDone();
        return;
    }
    switch (b) {
    case 'C': // FDC の割り込みが立った (シークの終わり、レディの変化、READ ID のリザルト)
        devIrq_ = true;
        if (phase_ == P_IDLE && cmdLen_ == 0) invalidateMsr(); // シーク中のビットが変わる
        else if (phase_ == P_RESULT_WAIT) resultPollAt_ = 0;
        break;
    case 'c':
        devIrq_ = false;
        break;
    case 'D': // ディスクの出し入れ
        fddIrq_ = true;
        fddEvents_++;
        memset(driveKnown_, 0, sizeof driveKnown_);
        break;
    case 'd':
        fddIrq_ = false;
        break;
    case 'R':
        rx2State_ = 1;
        frameAt_ = link_.receivedAtUs(p2_);
        break;
    case 'W': // 書くデータを求めている
        if (phase_ != P_WRITE_WAIT) {
            noteStray(Event::S_W, b);
            break;
        }
        wGot_ = 0;
        setPhase(P_WRITE_DATA);
        if (dmaAborted_) cancelWrite(); // 'W' を待っている間に DMA が異常終了している。データは来ない
        else if (wNeed_ == 0) sendWriteData();
        break;
    case 'w': // 書き込みが終わった (書けなかったときは 'W' なしで来る)。リザルト 7 バイトを読む
        wAwaitDone_ = false;
        if (phase_ == P_WRITE_WAIT || phase_ == P_WRITE_DATA || phase_ == P_WRITE_BUSY) startFetch(7, true);
        else noteStray(Event::S_W_DONE, b);
        break;
    case 't': // 中止
        wAwaitDone_ = false;
        if (phase_ != P_IDLE && phase_ != P_RESULT) failCommand();
        break;
    default:
        noteStray(Event::S_PORT2, b);
        break;
    }
}

// ---------------------------------------------------------------- 時間切れ

void Fuk::checkTimeouts() {
    if (!ready()) return;
    const uint64_t t = now();
    bool lost = awaiting_ && t - awaitSince_ >= REPLY_TIMEOUT_US;
    const bool noReply = lost;
    const uint64_t in = t - phaseSince_;
    switch (phase_) {
    case P_READ_WAIT: lost = lost || in >= READ_TIMEOUT_US; break;
    case P_WRITE_WAIT: lost = lost || in >= WRITE_TIMEOUT_US; break;
    case P_WRITE_BUSY:
        // データが口から出きっていれば、'w' を待つ時間はそのときから数える (tailPending() と同じ)
        if (wSent_ >= wTotal_ && !wFlushing_) lost = lost || t - wSince_ >= WRITE_TIMEOUT_US;
        else lost = lost || in >= WRITE_TIMEOUT_US;
        break;
    case P_RESULT_WAIT: lost = lost || in >= RESULT_TIMEOUT_US; break;
    case P_READ_DATA: lost = lost || (streaming_ && in >= READ_TIMEOUT_US); break; // 'R' の残りが届かない
    case P_WRITE_DATA:
        // FUK はデータを待っているのに、DMAC が渡してこない。書き込みをやめる
        if (in >= DMA_TIMEOUT_US) cancelWrite();
        break;
    default: break;
    }
    if (lost) {
        // 返事が来ない。あとから届く返事を次の読み出しのものと取り違えないよう、見分けからやり直す
        stats_.timeouts++;
        relink(noReply                  ? Event::NO_REPLY
               : phase_ == P_READ_WAIT  ? Event::NO_R
               : phase_ == P_READ_DATA  ? Event::NO_R_REST
               : phase_ == P_WRITE_WAIT ? Event::NO_W
               : phase_ == P_WRITE_BUSY ? Event::NO_W_DONE
                                        : Event::NO_RESULT);
    }
}

void Fuk::service() {
    // ラスタごとに呼ばれるはずなのに間が空いたら、X68000 が止められていた (設定メニューなど)。その間は時間切れを
    // 数えない (止めている間に DMAC が動けなかったのを、データを渡してこないと取り違えて 0 を書き込まないように)
    const uint64_t t = now();
    if (t - lastService_ >= PAUSE_GAP_US) {
        const uint64_t gap = t - lastService_;
        // このあと t との差を取るので、t を超えないようにする (この前の service() の途中で付けた時刻は、
        // lastService_ より少しあとになっている)
        phaseSince_ = phaseSince_ + gap > t ? t : phaseSince_ + gap;
        awaitSince_ = awaitSince_ + gap > t ? t : awaitSince_ + gap;
        wSince_ = wSince_ + gap > t ? t : wSince_ + gap;
    }
    lastService_ = t;
    serviceLink();
    // 'M' とデータは、送り始めたら最後まで送る。途中でリセットされたり、同じ装置とつなぎ直しになったり (epoch が
    // 変わらない) しても送り切る (FUK は決めたバイト数を受け取るまで、ポート 2 に届いたものをデータとして読むため)。
    // epoch が変わったときは serviceLink() が送り残しを捨てる。見分けの途中と、装置がつながっていない間は送らない
    if (wSent_ < wTotal_ && ls_ != L_OFFLINE && ls_ != L_PROBE)
        wSent_ += link_.send(p2_, wbuf_ + wSent_, wTotal_ - wSent_);
    // データが口から出きった時刻を、出きったらすぐに記録する ('w' を待つ時間の始まり)
    if (wSent_ >= wTotal_ && (wFlushing_ || wAwaitDone_)) tailPending();
    if (!ready()) return;
    receivePort1();
    receivePort2();
    checkTimeouts();
    if (queueOverflow_) {
        queueOverflow_ = false;
        if (ready()) {
            stats_.timeouts++;
            relink(Event::QUEUE_FULL);
        }
    }
    if (!ready()) return;
    pump();
}

// ほかのスレッド (コア) が読む数の写しを作る (snapshot() と対)。写しは sharedLock_ を取っている間だけ読み書きする。
// 読み手が読んでいる最中なら、今回は写さない (次の回に写す。X68000 を動かすスレッドは待たない)
void Fuk::publish() {
    __atomic_store_n(&sharedReady_, ready(), __ATOMIC_RELEASE);
    if (__atomic_test_and_set(&sharedLock_, __ATOMIC_ACQUIRE)) return;
    sharedStats_ = stats_;
    __atomic_clear(&sharedLock_, __ATOMIC_RELEASE);
}

Fuk::Stats Fuk::snapshot() {
    while (__atomic_test_and_set(&sharedLock_, __ATOMIC_ACQUIRE)) {
        // 書き手が写している (構造体を 1 つ写すだけなので、すぐ終わる)
    }
    const Stats s = sharedStats_;
    __atomic_clear(&sharedLock_, __ATOMIC_RELEASE);
    return s;
}

void Fuk::poll() {
    service();
    if ((++publishCount_ & 127) == 0) { // ラスタごとに呼ばれるので、4 ms に 1 回ほど
        publish();
        serviceTrace();
    }
    // リザルトの用意を待っている間は、ステータスを間を空けて読む
    if (ready() && phase_ == P_RESULT_WAIT && !msrPending_ && !resFetching_ && now() >= resultPollAt_) {
        resultPollAt_ = now() + RESULT_POLL_US;
        invalidateMsr();
        pump();
    }
}

template <typename F> bool Fuk::waitFor(F done) {
    stats_.waits++;
    const uint64_t t0 = now();
    bool ok = false;
    for (;;) {
        service();
        if (done()) {
            ok = true;
            break;
        }
        if (!ready()) break;
        if (now() - t0 >= WAIT_US) {
            stats_.waitTimeouts++;
            break;
        }
        link_.waitRx(100);
    }
    stats_.waitUs += now() - t0;
    return ok;
}

bool Fuk::start(unsigned waitMs) {
    const uint64_t t0 = now();
    for (;;) {
        service();
        if (ls_ == L_READY) return true;
        if (now() - t0 >= (uint64_t)waitMs * 1000) return false;
        link_.waitRx(1000);
    }
}

// ---------------------------------------------------------------- 異常終了の記録 (#400)

bool Fuk::takeEvent(Event& e) {
    const unsigned tail = __atomic_load_n(&eventTail_, __ATOMIC_RELAXED);
    if (tail == __atomic_load_n(&eventHead_, __ATOMIC_ACQUIRE)) return false;
    e = events_[tail & (EVENTS - 1)];
    __atomic_store_n(&eventTail_, tail + 1, __ATOMIC_RELEASE);
    return true;
}

void Fuk::pushEvent(const Event& e) {
    const unsigned head = __atomic_load_n(&eventHead_, __ATOMIC_RELAXED);
    if (head - __atomic_load_n(&eventTail_, __ATOMIC_ACQUIRE) >= EVENTS) {
        __atomic_add_fetch(&eventsDropped_, 1u, __ATOMIC_RELAXED);
        return;
    }
    events_[head & (EVENTS - 1)] = e;
    __atomic_store_n(&eventHead_, head + 1, __ATOMIC_RELEASE);
}

void Fuk::fillEvent(Event& e, uint8_t kind) {
    memset(&e, 0, sizeof e);
    e.atUs = now();
    e.kind = (Event::Kind)kind;
    memcpy(e.cmd, cmd_, sizeof e.cmd);
    // 実行中ならコマンド全部、受け取っている途中ならそこまで
    e.cmdLen = (uint8_t)(phase_ != P_IDLE ? kCmdLen[cmd_[0] & 0x1f] : cmdLen_);
    e.phase = (uint8_t)phase_;
    e.select = lastSelect_;
    e.control = rawControl_;
    e.reader = reader_;
    e.ms = (uint32_t)((e.atUs - phaseSince_) / 1000);
    e.stray = stats_.stray - strayAtCmd_;
    e.dmaCount = dmaAtCmd_;
    e.dmaGot = wGot_;
    e.dmaChain = chainAtCmd_;
    e.writes = stats_.writeFrames;
}

void Fuk::noteLinkLost(uint8_t reason) {
    Event e;
    fillEvent(e, Event::LINK_LOST);
    // SEEK と RECALIBRATE は、送ったあとも段階は P_IDLE
    const int op = cmd_[0] & 0x1f;
    if (e.cmdLen == 0 && seekBusy_ != 0 && (op == CMD_SEEK || op == CMD_RECALIBRATE)) e.cmdLen = kCmdLen[op];
    e.reason = reason;
    e.replyTag = awaitTag_;
    if (reason == Event::NO_REPLY) e.ms = (uint32_t)((e.atUs - awaitSince_) / 1000);
    pushEvent(e);
}

bool Fuk::eventAllowed(bool write) {
    const uint64_t t = now();
    if (t - eventWindow_ >= EVENT_WINDOW_US) {
        eventWindow_ = t;
        eventUsed_[0] = eventUsed_[1] = 0;
    }
    if (eventUsed_[write ? 0 : 1] < (write ? EVENT_WRITES : EVENT_OTHERS)) {
        eventUsed_[write ? 0 : 1]++;
        return true;
    }
    __atomic_add_fetch(&eventsDropped_, 1u, __ATOMIC_RELAXED);
    return false;
}

// res_ がそろった (X68000 に見せる前)。異常終了なら記録する。普段から出るもの (ディスクがないときの SENSE DEVICE STATUS、
// 引き取る割り込みのない SENSE INTERRUPT STATUS、レディの変化、EOT まで読んだ Read の異常終了 (ST1 が EN だけ)) は記録しない
void Fuk::noteResult(uint8_t source) {
    const int op = cmd_[0] & 0x1f;
    const bool write = isWriteType(op);
    if (reader_) return; // ディスクの読み出し (#381) のコマンド
    traceResult(source);
    if (op == CMD_SENSE_DEVICE) return;
    const uint8_t ic = (uint8_t)(res_[0] & 0xc0);
    if (op == CMD_SENSE_INTERRUPT) {
        if (ic != ST0_ABNORMAL) return;
    } else if (ic == 0) {
        return;
    }
    if (isReadType(op) && source == Event::FROM_FUK && ic == ST0_ABNORMAL && resLen_ == 7 && res_[1] == 0x80 && res_[2] == 0) return;
    if (!eventAllowed(write)) return;
    Event e;
    fillEvent(e, Event::FAILED);
    e.cmdLen = kCmdLen[op];
    e.source = source;
    e.resLen = (uint8_t)resLen_;
    memcpy(e.res, res_, sizeof e.res);
    pushEvent(e);
}

// 実行中のコマンドがなければ、その前のコマンドを入れる
void Fuk::fillLastCommand(Event& e, uint8_t kind, uint8_t reason, uint32_t value) {
    fillEvent(e, kind);
    if (e.cmdLen == 0 && cmdAt_ != 0) e.cmdLen = kCmdLen[cmd_[0] & 0x1f];
    e.reason = reason;
    e.value = value;
    e.replyTag = replyTag_;
    const uint64_t since = replyAt_ != 0 ? e.atUs - replyAt_ : 0;
    e.sinceUs = since < 0xffffffffu ? (uint32_t)since : 0xffffffffu;
}

void Fuk::noteStray(uint8_t kind, uint32_t value) {
    stats_.stray++;
    stats_.strays[kind]++;
    if (now() - strayWindow_ >= TRACE_WINDOW_US) resetStrayQuota();
    if (strayUsed_[kind] >= STRAY_FIRST) return;
    strayUsed_[kind]++;
    Event e;
    fillLastCommand(e, Event::STRAY, kind, value);
    e.number = stats_.strays[kind];
    holdEvent(e);
}

// 返事がずれたかもしれない (記録するだけで、動作は変えない)
void Fuk::noteOdd(uint8_t kind, uint32_t value) {
    stats_.odd[kind]++;
    if (now() - strayWindow_ >= TRACE_WINDOW_US) resetStrayQuota();
    if (oddUsed_[kind] >= STRAY_FIRST) return;
    oddUsed_[kind]++;
    Event e;
    fillLastCommand(e, Event::ODD, kind, value);
    e.number = stats_.odd[kind];
    e.resLen = (uint8_t)resLen_;
    memcpy(e.res, res_, sizeof e.res);
    holdEvent(e);
}

// 決まった数だけ読んだリザルトが、コマンドと合うか。前に余分なバイトがあると、1 つずつずれる
void Fuk::checkResult() {
    const int op = cmd_[0] & 0x1f;
    if (resLen_ == 7) {
        const bool n = (op == CMD_WRITE_ID && res_[6] != cmd_[2]) || (isWriteType(op) && op != CMD_WRITE_ID && res_[6] != cmd_[5]);
        if (((res_[0] ^ cmd_[1]) & 3) != 0 || n) noteOdd(Event::ODD_RESULT, res_[0]);
    } else if (op == CMD_SENSE_INTERRUPT && resLen_ == 2 && (res_[0] & (0xc0 | ST0_SEEK_END)) == ST0_SEEK_END) {
        const int want = lastCyl_[res_[0] & 3];
        if (want >= 0 && res_[1] != want) noteOdd(Event::ODD_SIS, res_[1]);
    }
}

// コマンドの 1 バイト目: WRITE ID から、ひと続きの書き込みの記録を始める
void Fuk::traceCommand(int op) {
    if (reader_ || !isWriteType(op)) return;
    const uint64_t t = now();
    if (op == CMD_WRITE_ID) traceIdAt_ = t;
    else if (traceIdAt_ == 0 || t - traceIdAt_ >= TRACE_AGAIN_US) return;
    traceAt_ = t;
    if (tracing_) return;
    tracing_ = true;
    if (heldCount_ != 0 || traceCount_ != 0) return; // 前の記録を移し終えていない: 続きとして扱う (番号も続ける)
    if (traceRuns_ == 0 || t - traceWindow_ >= TRACE_WINDOW_US) {
        traceWindow_ = t;
        traceRuns_ = 0;
    }
    traceRecord_ = traceRuns_ < TRACE_RUNS;
    if (!traceRecord_) {
        __atomic_add_fetch(&eventsDropped_, 1u, __ATOMIC_RELAXED);
        return;
    }
    traceRuns_++;
    traceNumber_ = 0;
    resetStrayQuota();
}

void Fuk::resetStrayQuota() {
    memset(strayUsed_, 0, sizeof strayUsed_);
    memset(oddUsed_, 0, sizeof oddUsed_);
    strayWindow_ = now();
}

// 書き込みの間は、ログに出すほうの負荷を増やさないよう、書き込みが落ち着くまでためる
void Fuk::holdEvent(const Event& e) {
    if (!tracing_ && heldCount_ == 0) {
        pushEvent(e);
        return;
    }
    if (heldCount_ == HELD) {
        __atomic_add_fetch(&eventsDropped_, 1u, __ATOMIC_RELAXED);
        return;
    }
    held_[(heldPos_ + heldCount_++) % HELD] = e;
}

void Fuk::traceResult(uint8_t source) {
    const int op = cmd_[0] & 0x1f;
    if (!tracing_) return;
    if (!isWriteType(op) && !(op == CMD_SENSE_INTERRUPT && resLen_ == 2)) return;
    if (isWriteType(op)) traceAt_ = now();
    if (!traceRecord_) return;
    Event e;
    fillEvent(e, Event::TRACE);
    e.cmdLen = kCmdLen[op];
    e.source = source;
    e.resLen = (uint8_t)resLen_;
    memcpy(e.res, res_, sizeof e.res);
    e.ms = (uint32_t)((e.atUs - cmdAt_) / 1000);
    e.number = ++traceNumber_;
    if (e.number <= TRACE_FIRST) {
        holdEvent(e);
        return;
    }
    if (traceCount_ == TRACE_LAST) popTrace(); // いちばん古いものを捨てる (番号が飛ぶので分かる)
    trace_[(tracePos_ + traceCount_++) % TRACE_LAST] = e;
}

// ためた記録のいちばん古いものを外す
void Fuk::popTrace() {
    tracePos_ = (tracePos_ + 1) % TRACE_LAST;
    traceCount_--;
}

// ためた記録を 1 つリングに移す (先頭のものと捨てたバイトなど、そのあと最後のもの)
bool Fuk::flushOne() {
    if (heldCount_ != 0) {
        pushEvent(held_[heldPos_]);
        heldPos_ = (heldPos_ + 1) % HELD;
        heldCount_--;
        return true;
    }
    if (traceCount_ == 0) return false;
    pushEvent(trace_[tracePos_]);
    popTrace();
    return true;
}

// 書き込みが TRACE_QUIET_US なければ終わりとみて、ためた記録を、1 回に 1 つずつリングに移す (リングが半分より埋まっていれば
// 待つ)。移し終える前に次の書き込みが始まったら、続きとして扱い、それが終わるまで待つ
void Fuk::serviceTrace() {
    // リザルトを読まないまま止まっても出す (ほかの段階は、時間切れで P_RESULT になる)
    if (tracing_ && (phase_ == P_IDLE || phase_ == P_RESULT) && now() - traceAt_ >= TRACE_QUIET_US) tracing_ = false;
    if (tracing_ || (heldCount_ == 0 && traceCount_ == 0)) return;
    if (__atomic_load_n(&eventHead_, __ATOMIC_RELAXED) - __atomic_load_n(&eventTail_, __ATOMIC_ACQUIRE) >= EVENTS / 2) return;
    flushOne();
}

bool Fuk::flushEvents() {
    tracing_ = false;
    while (__atomic_load_n(&eventHead_, __ATOMIC_RELAXED) - __atomic_load_n(&eventTail_, __ATOMIC_ACQUIRE) < EVENTS)
        if (!flushOne()) return false;
    return heldCount_ != 0 || traceCount_ != 0;
}

namespace {
// 文字列を作る (ベアメタルでも使えるよう、printf を使わない)
struct Text {
    char* p;
    unsigned left;
    void ch(char c) {
        if (left <= 1) return;
        *p++ = c;
        left--;
    }
    void str(const char* s) {
        while (*s) ch(*s++);
    }
    void hex2(uint8_t v) {
        static const char k[] = "0123456789abcdef";
        ch(k[v >> 4]);
        ch(k[v & 15]);
    }
    void bytes(const uint8_t* b, unsigned n) {
        for (unsigned i = 0; i < n; i++) {
            if (i) ch(' ');
            hex2(b[i]);
        }
    }
    void num(uint64_t v, unsigned digits = 1) {
        char t[20];
        unsigned n = 0;
        do {
            t[n++] = (char)('0' + v % 10);
            v /= 10;
        } while (v != 0 && n < sizeof t);
        while (n < digits) t[n++] = '0';
        while (n) ch(t[--n]);
    }
};
} // namespace

static const char* commandName(int op) {
    switch (op) {
    case CMD_READ_DIAGNOSTIC: return "READ DIAGNOSTIC";
    case CMD_SPECIFY: return "SPECIFY";
    case CMD_SENSE_DEVICE: return "SENSE DEVICE STATUS";
    case CMD_WRITE_DATA: return "WRITE DATA";
    case CMD_READ_DATA: return "READ DATA";
    case CMD_RECALIBRATE: return "RECALIBRATE";
    case CMD_SENSE_INTERRUPT: return "SENSE INTERRUPT STATUS";
    case CMD_WRITE_DELETED: return "WRITE DELETED DATA";
    case CMD_READ_ID: return "READ ID";
    case CMD_READ_DELETED: return "READ DELETED DATA";
    case CMD_WRITE_ID: return "WRITE ID";
    case CMD_SEEK: return "SEEK";
    case 0x11:
    case 0x19:
    case 0x1d: return "SCAN";
    default: return "command";
    }
}

const char* Fuk::tagName(uint8_t tag) {
    return tag >= T_DRIVE ? "drive status" : tag == T_RESULT ? "result" : tag == T_MSR ? "status" : tag == T_SYNC ? "sync" : "other";
}

void Fuk::formatEvent(const Event& e, char* out, unsigned size) {
    if (size == 0) return;
    static const char* const kSource[] = {"from the device", "from the device, changed by a DMA error", "made by failCommand",
                                          "made by setResult7, write cancelled", "made by setResult7, write refused",
                                          "made by setResult7, not connected"};
    static const char* const kReason[] = {"no reply to a register read", "no 'R'", "the rest of 'R' did not come", "no 'W' or 'w' after the command",
                                          "no 'w' after the data", "no result", "the send queue is full", "unplugged or the port closed",
                                          "the device came back as new"};
    static const char* const kPhase[] = {"idle", "waiting for 'R'", "read data", "waiting for 'W'", "write data", "waiting for 'w'",
                                         "waiting for the result", "reading the result", "result"};
    static const char* const kStray[] = {"port 1 byte not asked for", "late result byte", "unknown port 2 byte",
                                         "'W' not waited for", "'w' not waited for", "'R' not waited for, length"};
    static const char* const kOdd[] = {"reply before the request was sent", "result does not match the command",
                                       "PCN is not the seek target", "status shows a result while idle", "status after a reset"};
    Text t = {out, size};
    const int op = e.cmd[0] & 0x1f;
    const unsigned cmdLen = e.cmdLen <= sizeof e.cmd ? e.cmdLen : sizeof e.cmd;
    t.str("at ");
    t.num(e.atUs / 1000000);
    t.ch('.');
    t.num(e.atUs / 1000 % 1000, 3);
    t.str(" s: ");
    if (e.kind == Event::LINK_LOST) {
        t.str("link lost: ");
        t.str(kReason[e.reason < sizeof kReason / sizeof kReason[0] ? e.reason : 0]);
        if (e.reason == Event::NO_REPLY) {
            t.str(" (");
            t.str(tagName(e.replyTag));
            t.ch(')');
        }
        t.str(", ");
        t.num(e.ms);
        t.str(" ms, ");
        t.str(kPhase[e.phase < sizeof kPhase / sizeof kPhase[0] ? e.phase : 0]);
        t.str(", command ");
        if (e.cmdLen == 0) t.str("none");
        else {
            t.str(commandName(op));
            t.str(" [");
            t.bytes(e.cmd, cmdLen);
            t.ch(']');
        }
    } else if (e.kind == Event::STRAY || e.kind == Event::ODD) {
        const bool odd = e.kind == Event::ODD;
        t.str(odd ? "odd reply: " : "stray: ");
        t.str(odd ? kOdd[e.reason < sizeof kOdd / sizeof kOdd[0] ? e.reason : 0] : kStray[e.reason < sizeof kStray / sizeof kStray[0] ? e.reason : 0]);
        t.ch(' ');
        if (!odd && e.reason == Event::S_R) t.num(e.value); // 長さ
        else t.hex2((uint8_t)e.value);
        t.str(" (#");
        t.num(e.number);
        t.str("), ");
        t.num(e.sinceUs);
        t.str(" us after the last reply (");
        t.str(tagName(e.replyTag));
        t.str("), ");
        t.str(kPhase[e.phase < sizeof kPhase / sizeof kPhase[0] ? e.phase : 0]);
        t.str(", last command [");
        t.bytes(e.cmd, cmdLen);
        t.ch(']');
        if (odd) {
            t.str(", result [");
            t.bytes(e.res, e.resLen <= 7 ? e.resLen : 7);
            t.ch(']');
        }
    } else {
        if (e.kind == Event::TRACE) {
            t.str("write trace #");
            t.num(e.number);
            t.str(", ");
            t.num(e.ms);
            t.str(" ms: ");
        }
        t.str(commandName(op));
        t.str(" [");
        t.bytes(e.cmd, cmdLen);
        t.str(e.kind == Event::TRACE ? "], result [" : "] failed, result [");
        t.bytes(e.res, e.resLen <= 7 ? e.resLen : 7);
        t.str("] ");
        t.str(kSource[e.source < sizeof kSource / sizeof kSource[0] ? e.source : 0]);
    }
    if (e.reader) t.str(", reading a disk");
    t.str(", select ");
    t.hex2(e.select);
    t.str(", control ");
    t.hex2(e.control);
    if (e.cmdLen != 0 && isWriteType(op)) {
        t.str(", DMA ");
        t.num(e.dmaGot);
        t.ch('/');
        t.num(e.dmaCount);
        if (e.dmaChain) t.str(" (chained or stopped)");
    }
    t.str(", ");
    t.num(e.stray);
    t.str(" stray, ");
    t.num(e.writes);
    t.str(" write frames");
    *t.p = 0;
}

// ---------------------------------------------------------------- コマンド

void Fuk::setResult7(uint8_t st0, uint8_t st1) {
    res_[0] = st0;
    res_[1] = st1;
    res_[2] = 0;
    res_[3] = cmd_[2];
    res_[4] = cmd_[3];
    res_[5] = cmd_[4];
    res_[6] = cmd_[5];
    resLen_ = 7;
}

void Fuk::startFetch(int want, bool irq) {
    resLen_ = 0;
    resWant_ = want;
    resIrq_ = irq;
    setPhase(P_RESULT_FETCH);
    resFetching_ = true;
    pushRead(REG_DATA, T_RESULT);
}

void Fuk::enterResult(bool irq) {
    if (resLen_ <= 0) {
        res_[0] = ST0_INVALID;
        resLen_ = 1;
    }
    resPos_ = 0;
    noteResult(resSource_);
    setPhase(P_RESULT);
    if (irq) localIrq_ = true;
    // X68000 がリザルトを読んでいる間に、そのあとのステータスを読んでおく
    invalidateMsr();
}

// 実行中のコマンドを、こちらで作った異常終了 (ノットレディ) にする
void Fuk::failCommand() {
    if (phase_ == P_RESULT) return;
    if (phase_ == P_IDLE) {
        // コマンドのバイトを受け取っている途中なら、残りは FUK に送らずに受け取って、こちらで答える
        if (cmdLen_ > 0) cmdLocal_ = true;
        return;
    }
    const int op = cmd_[0] & 0x1f;
    const uint8_t us = cmd_[1] & 7;
    resFetching_ = false;
    resSource_ = Event::FAIL_COMMAND;
    if (op == CMD_SENSE_INTERRUPT) {
        res_[0] = ST0_INVALID;
        resLen_ = 1;
        enterResult(false);
    } else if (op == CMD_SENSE_DEVICE) {
        res_[0] = us; // レディでない
        resLen_ = 1;
        enterResult(false);
    } else if (kCmdLen[op] >= 6 || op == CMD_READ_ID) {
        setResult7((uint8_t)(ST0_ABNORMAL | ST0_NOT_READY | us), 0);
        enterResult(true);
    } else {
        res_[0] = ST0_INVALID;
        resLen_ = 1;
        enterResult(false);
    }
}

// そろったデータを送る (wGot_ == wNeed_ のときだけ呼ぶ)
void Fuk::sendWriteData() {
    wbuf_[0] = 'M';
    wTotal_ = 1 + wNeed_;
    wSent_ = 0;
    wFlushing_ = true;
    wAwaitDone_ = true;
    wSince_ = now();
    stats_.writeFrames++;
    setPhase(P_WRITE_BUSY);
}

// コマンドの最後のバイトを FUK に送った
void Fuk::dispatch() {
    const int op = cmd_[0] & 0x1f;
    sisLocal_ = false;
    dmaAborted_ = false;
    resSource_ = Event::FROM_FUK;
    if (isReadType(op)) {
        setPhase(P_READ_WAIT);
    } else if (isWriteType(op)) {
        setPhase(P_WRITE_WAIT);
    } else if (op == CMD_SPECIFY) {
        nonDma_ = (cmd_[2] & 1) != 0;
        invalidateMsr();
    } else if (op == CMD_SEEK || op == CMD_RECALIBRATE) {
        // 終わると 'C' が届き、X68000 が SENSE INTERRUPT STATUS で引き取る
        seekBusy_ |= (uint8_t)(1u << unit());
        headLost_ &= (uint8_t)~(1u << unit());
        lastCyl_[unit()] = op == CMD_SEEK && cmd_[2] < 0x80 ? (int8_t)cmd_[2] : op == CMD_SEEK ? -1 : 0;
        invalidateMsr();
    } else if (op == CMD_SENSE_INTERRUPT) {
        startFetch(2, false);
    } else if (op == CMD_SENSE_DEVICE) {
        startFetch(1, false);
    } else {
        // READ ID はディスクが回るのを待つので、ステータスでリザルトの用意を確かめてから 7 バイト読む。
        // ほかのコマンドはリザルトがあるか分からないので、ステータスを見ながら 1 バイトずつ読む
        resLen_ = 0;
        resWant_ = op == CMD_READ_ID ? 7 : 0;
        resIrq_ = op == CMD_READ_ID;
        // READ ID の割り込みは、リザルトがそろったときにこちらで立てる (FUK の 'C' は、こちらが先に読むと下りる)
        maskDevIrq_ = op == CMD_READ_ID;
        phase_ = P_RESULT_WAIT;
        phaseSince_ = now();
        resultPollAt_ = phaseSince_ + RESULT_POLL_US;
        invalidateMsr();
    }
}

// つながっていない間: ドライブが 1 台もレディにならない FDC として答える
void Fuk::localExecute() {
    const int op = cmd_[0] & 0x1f;
    const uint8_t us = cmd_[1] & 7;
    if (op == CMD_SPECIFY) {
        nonDma_ = (cmd_[2] & 1) != 0;
    } else if (op == CMD_SEEK || op == CMD_RECALIBRATE) {
        localSeek_[us & 3] = (uint8_t)(ST0_SEEK_END | ST0_ABNORMAL | ST0_NOT_READY | (us & 3));
        localIrq_ = true;
    } else if (op == CMD_SENSE_INTERRUPT) {
        res_[0] = ST0_INVALID;
        resLen_ = 1;
        for (int d = 0; d < 4; d++) {
            if (localSeek_[d] == 0) continue;
            res_[0] = localSeek_[d];
            res_[1] = 0;
            resLen_ = 2;
            localSeek_[d] = 0;
            break;
        }
        resPos_ = 0;
        setPhase(P_RESULT);
        localIrq_ = false;
        for (int d = 0; d < 4; d++)
            if (localSeek_[d] != 0) localIrq_ = true;
    } else if (op == CMD_SENSE_DEVICE) {
        res_[0] = us;
        resLen_ = 1;
        resPos_ = 0;
        setPhase(P_RESULT);
    } else if (kCmdLen[op] >= 6 || op == CMD_READ_ID) {
        if (cmdRefused_) setResult7((uint8_t)(ST0_ABNORMAL | us), ST1_OVERRUN); // DMA を使わない書き込み
        else setResult7((uint8_t)(ST0_ABNORMAL | ST0_NOT_READY | us), 0);
        resPos_ = 0;
        noteResult(cmdRefused_ ? Event::REFUSED : Event::OFFLINE);
        setPhase(P_RESULT);
        localIrq_ = true;
    } else {
        res_[0] = ST0_INVALID;
        resLen_ = 1;
        resPos_ = 0;
        setPhase(P_RESULT);
    }
}

// ---------------------------------------------------------------- Machine から

void Fuk::reset() {
    clearCommand();
    localIrq_ = devIrq_ = fddIrq_ = false;
    seekBusy_ = 0;
    sisLocal_ = false;
    nonDma_ = noDma_ = false;
    forceReady_ = forceReadySent_ = false;
    driveSel_ = 0;
    memset(localSeek_, 0, sizeof localSeek_);
    lastSelect_ = lastControl_ = rawControl_ = 0;
    if (!ready()) return;
    // 'W' のあと 'M' を送り始める前 (DMAC からデータを受け取っている途中) なら、'M' は送らない。そろっていない
    // データを 0 で埋めて送ると、ディスクに書かれる。Pico のファームは、リセットで 'M' を待つのをやめる
    // (実物の FUK がやめるかは確かめていない。docs/fuk.md)
    resetDevice();
}

// FUK の FDC をリセットする。リセットのあとにステータスを 1 回読み、その返事が届くまでの通知は捨てる。
// 前に送った読み出しの返事を待っているなら、それが届いてから送る (届いた値は捨てる)
void Fuk::resetDevice() {
    const bool stale = awaiting_;
    const uint64_t since = awaitSince_;
    // 口に渡す途中のバイトは捨てない (2 バイトの書き込みの 1 バイト目だけが届いていると、次のバイトが値として読まれる)
    const unsigned pos = out1Pos_, len = out1Len_;
    dropQueue();
    out1Pos_ = pos;
    out1Len_ = len;
    if (stale) {
        awaiting_ = true;
        awaitTag_ = T_STALE;
        awaitSince_ = since;
    }
    push(OP_RESET, 0, 0);
    pushRead(REG_STATUS, T_SYNC);
    ls_ = L_SYNC;
    lsSince_ = now();
    pump();
}

// 書き込みを、データを FUK に送る前にやめる (DMAC がデータを渡してこない、DMA が異常終了した、決めたバイト数より
// 少ないところで転送が終わった)。そろっていないデータを 0 で埋めて送ると、ディスクの中身を壊す。'M' を送らずに
// FUK の FDC をリセットして (Pico のファームは、リセットで 'M' を待つのをやめる)、X68000 にはオーバーランの
// 異常終了を返す。リセットで FUK のモーターとドライブの選択も初めに戻るので、X68000 が最後に書いた値を送り直す
void Fuk::cancelWrite() {
    stats_.cancelledWrites++;
    resSource_ = Event::CANCELLED;
    setResult7((uint8_t)(ST0_ABNORMAL | (cmd_[1] & 7)), ST1_OVERRUN);
    enterResult(true);
    if (!ready()) return;
    failSeeks();
    resetDevice();
    push(OP_WRITE, REG_DRIVE_SELECT, lastSelect_);
    push(OP_WRITE, REG_DRIVE_CONTROL, lastControl_);
}

// ---------------------------------------------------------------- ディスクの読み出し (fuk_reader.h)

bool Fuk::busyForReader() {
    return reader_ || phase_ != P_IDLE || cmdLen_ != 0 || seekBusy_ != 0 || localSeekPending() || irq() || forceReady_ ||
           forceReadySent_ || (host_ != nullptr && host_->fukDmaCount() != 0);
}

bool Fuk::idleForReader() {
    return !busyForReader() && ls_ == L_READY && qHead_ == qTail_ && out1Pos_ >= out1Len_ && !awaiting_ && rx2State_ == 0 &&
           !queueOverflow_ && !tailPending();
}

void Fuk::beginReader(int unit) {
    reader_ = readerMotor_ = true;
    readerUnit_ = unit & 3;
    readerHost_ = host_;
    readerSis_ = sisLocal_;
    readerNoDma_ = noDma_;
    readerNonDma_ = nonDma_;
    readerReset_ = resetCount_;
    memcpy(readerCyl_, lastCyl_, sizeof readerCyl_);
    host_ = nullptr;
    sisLocal_ = false;
    // write(7) は使わない (X68000 が最後に書いた値と、FDD の割り込みを変えないように)
    pushWrite(REG_DRIVE_SELECT, (uint8_t)(0x80 | readerUnit_));
    pump();
}

bool Fuk::restoreReaderMotor() {
    if (!reader_ || !readerMotor_) return false;
    readerMotor_ = false;
    pushWrite(REG_DRIVE_SELECT, lastSelect_);
    pump();
    return lastSelect_ != (uint8_t)(0x80 | readerUnit_);
}

void Fuk::endReader(ReaderHead head) {
    if (!reader_) return;
    const bool motor = readerMotor_;
    readerMotor_ = false;
    reader_ = false;
    host_ = readerHost_;
    readerHost_ = nullptr;
    if (head == ReaderHead::LOST) headLost_ |= (uint8_t)(1u << readerUnit_);
    const bool lost = resetCount_ != readerReset_ || !ready();
    const bool stuck = phase_ != P_IDLE || cmdLen_ != 0 || seekBusy_ != 0 || irq();
    if (lost || stuck) {
        // 途中で終わった。読み出しのコマンドのリザルトと、こちらで作ったシークの終わりを、X68000 に見せない
        clearCommand();
        seekBusy_ = 0;
        memset(localSeek_, 0, sizeof localSeek_);
        localIrq_ = false;
        sisLocal_ = false;
        noDma_ = readerNoDma_;
        nonDma_ = lost ? nonDma_ : readerNonDma_;
        if (lost) return;
        // FUK が答えない (シークの終わりが来ない、割り込みが下りない)。遅れて届くものを X68000 に見せないよう、リセットする
        const bool fdd = fddIrq_; // ディスクの出し入れは X68000 に残す
        resetDevice();
        fddIrq_ = fdd;
        push(OP_WRITE, REG_DRIVE_SELECT, lastSelect_);
        push(OP_WRITE, REG_DRIVE_CONTROL, lastControl_); // 排出のビットは落としてある
        pump();
        return;
    }
    sisLocal_ = readerSis_;
    noDma_ = readerNoDma_;
    nonDma_ = readerNonDma_;
    memcpy(lastCyl_, readerCyl_, sizeof lastCyl_);
    if (head == ReaderHead::LOST) lastCyl_[readerUnit_] = -1;
    else if (head == ReaderHead::RESTORED && readerCyl_[readerUnit_] < 0) lastCyl_[readerUnit_] = 0; // 分からなかったときは、シリンダ 0 に戻してある
    if (motor) pushWrite(REG_DRIVE_SELECT, lastSelect_);
    pump();
}

bool Fuk::irq() const { return localIrq_ || (devIrq_ && !maskDevIrq_); }

void Fuk::setForceReady(bool on) {
    if (on == forceReady_) return;
    forceReady_ = on;
    // 立ったときは覚えるだけで、次の RECALIBRATE の直前に伝える。下りたときは、伝えていた場合だけ伝える
    if (!on && forceReadySent_) {
        forceReadySent_ = false;
        if (ready()) {
            pushWrite(REG_FORCE_READY, 0x00);
            pump();
        }
    }
}

void Fuk::writeIoc(uint32_t offset, uint8_t v) {
    if (!(offset & 1) || !ready()) return;
    pushWrite((uint8_t)(REG_IOC | (offset & 0x0f)), v);
    pump();
}

// ソフトウェアがステータスを読みながら FUK の返事を待っているとき: 返事はすぐ届くはずなので、少しだけ待つ。
// X68000 は 1 フレーム分を実時間より速く動かし、残りの時間は休んでいる。待たずに返すと、返事が届く前にその
// フレームを動かし終え、返事を受け取るのが次のフレームになる (Pi 5 で、読み出し 1 回が平均 5 ms になった)。
// YIELD_WINDOW_US の間に待った合計が YIELD_BUDGET_US に達したら、その間は待たない (X68000 が実時間に追いつけなく
// ならないように。待った時間はあとから足すので、最後の 1 回の分だけ超えることがある)
void Fuk::yieldForReply() {
    const uint64_t t = now();
    if (t - yieldWindow_ >= YIELD_WINDOW_US) {
        yieldWindow_ = t;
        yieldUsed_ = 0;
    }
    if (yieldUsed_ >= YIELD_BUDGET_US) return;
    link_.waitRx(YIELD_STEP_US);
    const uint64_t took = now() - t;
    yieldUsed_ += (uint32_t)took;
    stats_.yieldUs += took;
    stats_.yields++;
    service();
}

uint8_t Fuk::read(uint32_t offset) {
    service();
    switch (offset & 0x07) {
    case 0x1: {
        if (awaiting_ && ls_ == L_READY &&
            (phase_ == P_RESULT_FETCH || phase_ == P_RESULT_WAIT || (phase_ == P_IDLE && cmdLen_ == 0 && !msrKnown_)))
            yieldForReply();
        switch (phase_) {
        case P_RESULT: return MSR_RQM | MSR_DIO | MSR_CB;
        case P_READ_DATA: return (nonDma_ || noDma_) ? (MSR_RQM | MSR_DIO | MSR_EXM | MSR_CB) : MSR_CB;
        case P_WRITE_DATA: return MSR_CB; // 書き込みは DMA だけ (DMA を使わない書き込みは受け付けない)
        case P_IDLE: break;
        default: return MSR_CB; // FUK の答えを待っている
        }
        if (cmdLen_ > 0) return MSR_RQM | MSR_CB;
        if (!ready()) return MSR_RQM;
        if (!msrKnown_) {
            requestMsr();
            pump();
            return MSR_CB;
        }
        if (now() - msrAt_ >= MSR_FRESH_US) {
            requestMsr();
            pump();
        }
        // コマンドを全部送り終えているので、「コマンドの途中」(RQM と CB が 1、DIO が 0) にはならない。
        // FUK がそう答えても CB は落として返す (ソフトウェアが CB の下りるのを待ち続けないように)
        if ((msr_ & (MSR_RQM | MSR_DIO)) == MSR_RQM) return (uint8_t)(msr_ & ~MSR_CB);
        return msr_;
    }
    case 0x3:
        if (phase_ == P_RESULT_FETCH || phase_ == P_RESULT_WAIT) {
            // リザルトがそろう前に読まれた。そろうのを待つ (上限つき)
            waitFor([this] { return phase_ == P_RESULT; });
        }
        if (phase_ == P_RESULT) {
            const uint8_t v = res_[resPos_++];
            localIrq_ = false; // リザルトを読むと割り込みが消える
            if (resPos_ >= resLen_) {
                setPhase(P_IDLE);
                if (localSeekPending()) localIrq_ = true; // こちらで作ったシークの終わりが、まだ残っている
            }
            return v;
        }
        if (phase_ == P_READ_DATA) { // DMAC が 1 バイトずつ読むときと、DMA を使わないとき
            if (dataPos_ >= dataAvail() || readTc_ || readAbort_) return 0; // まだ届いていない (DMAC は dataReady() を見て読む)
            if (timed_ && dataPos_ == 0) firstEmu_ = host_->fukEmuNs();
            const uint8_t v = data_[dataPos_++];
            if (dataPos_ >= dataLen_ && !streaming_) enterResult(true);
            return v;
        }
        if (phase_ == P_IDLE && cmdLen_ == 0 && ready() && !(msrKnown_ && !(msr_ & MSR_DIO))) {
            // こちらが知らないリザルトがあるかもしれない。FUK から読んで返す (返事を待つ)
            directDone_ = false;
            pushRead(REG_DATA, T_DIRECT);
            waitFor([this] { return directDone_; });
            invalidateMsr();
            pump();
            return directDone_ ? directValue_ : 0x00;
        }
        return 0x00;
    case 0x5: { // ドライブの状態: bit7 = ドライブ制御で選んだドライブにディスクが入っている
        const int sel = driveSel_ & 15;
        if (sel == 0 || !ready()) return 0x00;
        // 覚えた値が古ければ、返しつつ読み直す (ディスクの出し入れは 'D' で届き、そのときは覚えた値を捨てている)。
        // 覚えた値がないときだけ、返事を待つ
        if (drivePending_[sel] == 0 && !(driveKnown_[sel] && now() - driveAt_[sel] < DRIVE_FRESH_US)) {
            drivePending_[sel]++;
            pushRead(REG_DRIVE_CONTROL, (uint8_t)(T_DRIVE | sel));
            pump();
        }
        if (driveKnown_[sel]) return driveStat_[sel];
        waitFor([this, sel] { return drivePending_[sel] == 0; });
        return driveKnown_[sel] ? driveStat_[sel] : 0x00;
    }
    default:
        return 0xff;
    }
}

void Fuk::write(uint32_t offset, uint8_t v) {
    service();
    switch (offset & 0x07) {
    case 0x1:
        if (ready()) pushWrite(REG_STATUS, v);
        break;
    case 0x3: {
        if (phase_ == P_WRITE_DATA) {
            if (wGot_ < wNeed_) wbuf_[1 + wGot_++] = v;
            if (wGot_ >= wNeed_) sendWriteData();
            break;
        }
        if (phase_ != P_IDLE) return; // リザルトを読み終える前のコマンドは受け付けない
        if (cmdLen_ == 0) {
            const int op = v & 0x1f;
            cmdNeed_ = kCmdLen[op];
            cmdLocal_ = !ready();
            cmdRefused_ = cmdHold_ = false;
            // こちらで作ったシークの終わり (つながっていなかった間のもの) が残っていれば、つながったあとでも
            // SENSE INTERRUPT STATUS はこちらで答える
            if (op == CMD_SENSE_INTERRUPT && localSeekPending()) cmdLocal_ = true;
            memset(cmd_, 0, sizeof cmd_);
            strayAtCmd_ = stats_.stray;
            cmdAt_ = now();
            traceCommand(op);
            wGot_ = 0;
            dmaAtCmd_ = 0;
            chainAtCmd_ = false;
            if (!cmdLocal_) {
                if (op == CMD_SENSE_INTERRUPT && sisLocal_ && !devIrq_ && seekBusy_ == 0) {
                    // Read の割り込みはこちらで作ったもので、FUK には引き取る割り込みがない。FUK には送らず、
                    // 「引き取る割り込みがない」と答える
                    cmd_[0] = v;
                    res_[0] = ST0_INVALID;
                    resLen_ = 1;
                    resPos_ = 0;
                    setPhase(P_RESULT);
                    return;
                }
                const uint32_t count = host_ != nullptr ? host_->fukDmaCount() : 0;
                noDma_ = isReadType(op) && count == 0;
                dmaAtCmd_ = count;
                chainAtCmd_ = host_ != nullptr && isWriteType(op) && !host_->fukDmaSingleBlock();
                if (isWriteType(op) && (count == 0 || nonDma_ || !host_->fukDmaSingleBlock())) {
                    // DMA を使わない書き込みと、DMAC がチェーンかコンティニューで動いている書き込み (転送バイト数は
                    // 最初のブロックの分で、全体のバイト数ではない) は受け付けない。FUK には、書くバイト数をコマンドの前に知らせる決まりで、
                    // その数は DMAC の転送バイト数から取る。DMAC を使わないと数が分からず、0 バイトや前の転送の
                    // 残りの数を知らせて、データなしで実物のディスクに書かせてしまう。FUK には送らず、こちらで
                    // 異常終了 (オーバーラン) にする
                    cmdLocal_ = cmdRefused_ = true;
                    stats_.refusedWrites++;
                } else if (isWriteType(op)) {
                    // 'W' のあとに送るバイト数を、コマンドの前に知らせる
                    wNeed_ = count & 0xffff;
                    pushWrite(REG_COUNT_LOW, (uint8_t)wNeed_);
                    pushWrite(REG_COUNT_HIGH, (uint8_t)(wNeed_ >> 8));
                }
                if (!cmdLocal_ && op == CMD_RECALIBRATE && forceReady_ && !forceReadySent_) {
                    forceReadySent_ = true;
                    pushWrite(REG_FORCE_READY, 0x40);
                }
            }
        }
        if (cmdLen_ == 0 && !cmdLocal_ && headLost_ != 0 && (v & 0x1f) == CMD_WRITE_ID) cmdHold_ = true;
        cmd_[cmdLen_++] = v;
        if (cmdHold_) {
            // ヘッドの位置が分からないドライブの WRITE ID は、FUK に送らずに異常終了にする
            if (cmdLen_ < 2) break;
            cmdHold_ = false;
            if ((headLost_ >> unit()) & 1) {
                cmdLocal_ = cmdRefused_ = true;
                stats_.refusedWrites++;
            } else if (!cmdLocal_) {
                pushWrite(REG_DATA, cmd_[0]);
                pushWrite(REG_DATA, v);
            }
        } else if (!cmdLocal_) pushWrite(REG_DATA, v);
        if (cmdLen_ >= cmdNeed_) {
            cmdLen_ = 0;
            if (cmdLocal_) localExecute();
            else dispatch();
            cmdLocal_ = false;
        }
        break;
    }
    case 0x5: {
        // ドライブ制御: bit0-3 ドライブの選択、bit5 排出、bit6 排出禁止、bit7 LED の点滅
        fddIrq_ = false;
        driveSel_ = v & 15;
        rawControl_ = v;
        lastControl_ = (uint8_t)(v & ~0x20); // 送り直すときに、排出はやり直さない
        if (!ready()) break;
        pushWrite(REG_DRIVE_CONTROL, v);
        if (v & 0x20) memset(driveKnown_, 0, sizeof driveKnown_); // 排出: 覚えた状態は使えない
        const int sel = driveSel_;
        // すぐあとに状態を読まれることが多いので、読み出しも送っておく
        if (sel != 0 && drivePending_[sel] == 0 && !(driveKnown_[sel] && now() - driveAt_[sel] < DRIVE_FRESH_US)) {
            drivePending_[sel]++;
            pushRead(REG_DRIVE_CONTROL, (uint8_t)(T_DRIVE | sel));
        }
        break;
    }
    case 0x7: // アクセスするドライブ: bit7 モーター、bit1-0 ドライブ
        fddIrq_ = false;
        lastSelect_ = v;
        if (ready()) pushWrite(REG_DRIVE_SELECT, v);
        break;
    }
    pump();
}

// ---------------------------------------------------------------- DMAC から

// 今 DMAC に渡せるバイト数 (先頭から)。届いている途中なら、届いた分だけ
uint32_t Fuk::dataAvail() {
    const uint32_t got = !streaming_ ? dataLen_ : frameGot_ < dataLen_ ? frameGot_ : dataLen_;
    if (!timed_) return got;
    // 届いた時刻の間隔のまま、X68000 の時間に写す (beginRead())
    const uint64_t t = host_->fukEmuNs();
    const uint64_t in = t > anchorEmu_ ? t - anchorEmu_ : 0;
    if (!leadFixed_) {
        // 'R' が全部届いたか、遅れの上限に達したら、遅れを決めて渡し始める
        if (!streaming_ && leadUs_ == 0) leadNs_ = in < leadMax_ ? in : leadMax_;
        else if (in >= leadMax_) leadNs_ = leadMax_;
        else return 0;
        leadFixed_ = true;
        stats_.leadUs += leadNs_ / 1000;
    }
    const uint32_t first = dataAt_[0];
    while (availPos_ < got) {
        const uint32_t d = dataAt_[availPos_] > first ? dataAt_[availPos_] - first : 0;
        if ((uint64_t)d * 1000 + leadNs_ > in) break;
        availPos_++;
    }
    return availPos_;
}

bool Fuk::dataReady() {
    if (phase_ == P_READ_DATA) return !readTc_ && !readAbort_ && dataPos_ < dataAvail();
    if (phase_ == P_WRITE_DATA) return wGot_ < wNeed_;
    return false;
}

int Fuk::readData(uint8_t* dst, int n) {
    if (phase_ != P_READ_DATA || n <= 0) return 0;
    if (readTc_ || readAbort_) return 0;
    const uint32_t avail = dataAvail();
    if (dataPos_ >= avail) return 0;
    uint32_t k = avail - dataPos_;
    if (k > (uint32_t)n) k = (uint32_t)n;
    memcpy(dst, data_ + dataPos_, k);
    if (timed_ && dataPos_ == 0) firstEmu_ = host_->fukEmuNs();
    dataPos_ += k;
    // FUK が読んだ分を渡し終えた (EOT まで読んだか、エラーで止まった)。届いている途中なら、リザルトが届いたときに
    if (dataPos_ >= dataLen_ && !streaming_) enterResult(true);
    return (int)k;
}

void Fuk::terminalCount() {
    // 最初のバイトを渡してから TC までの長さ (統計)。全部渡し終えてリザルトに移ったあとの TC も数える
    if (firstEmu_ != 0) {
        stats_.tcUs[stats_.tcReads % 6] = (uint32_t)((host_->fukEmuNs() - firstEmu_) / 1000);
        stats_.tcReads++;
        firstEmu_ = 0;
    }
    if (phase_ == P_READ_DATA) {
        // 残りは捨てる。届いている途中なら、残りとリザルトが届いてからリザルトにする (frameDone())
        if (streaming_) readTc_ = true;
        else enterResult(true);
    } else if (phase_ == P_WRITE_DATA) {
        // DMAC が、FUK に知らせたバイト数より少ないところで転送を終えた。足りないデータの 'M' は送らず、FUK の FDC を
        // リセットする (リセットで 'M' 待ちをやめない装置には、この書き込みを止められない。docs/fuk.md)
        if (wGot_ < wNeed_) cancelWrite();
        else sendWriteData();
        pump();
    }
}

// DMA が異常終了した (バスエラー、ソフトウェアアボート)
void Fuk::abortTransfer() {
    if (phase_ == P_READ_DATA && streaming_) {
        readAbort_ = true; // リザルトが届いてから、オーバーランにして返す (frameDone())
    } else if (phase_ == P_READ_DATA) {
        resSource_ = Event::DMA_ABORT;
        res_[0] = (uint8_t)((res_[0] & 0x3f) | ST0_ABNORMAL);
        res_[1] |= ST1_OVERRUN;
        enterResult(true);
    } else if (phase_ == P_WRITE_DATA) {
        cancelWrite();
    } else if (phase_ == P_READ_WAIT || phase_ == P_WRITE_WAIT) {
        dmaAborted_ = true; // FUK の答えが届いたときに終わらせる (frameDone()、'W')
    }
}

} // namespace x68k
