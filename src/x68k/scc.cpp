#include "x68k/scc.h"

namespace x68k {

// WR2 と WR9 は両チャネルで共通 (wr_[CH_A] と wr_[CH_B] の両方に置く)

void Scc::reset() {
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < 16; i++) wr_[c][i] = 0;
        resetChannel(c);
        chan_[c].rxFirstArmed = true;
    }
    pointer_ = 0;
    ius_ = 0;
}

void Scc::resetChannel(int ch) {
    Chan& c = chan_[ch];
    c.count = 0;
    for (int i = 0; i < FIFO_SIZE; i++) c.fifoErr[i] = false;
    c.overrun = false;
    c.rxLocked = false;
    c.rxLockedRead = false;
    c.rxFirstArmed = true;
    c.rxFirstIp = false;
    c.txIp = false;
    c.extIp = false;
    c.extLatched = false;
    c.extLatch = 0;
    // c.lines は線の今の状態なので残す
    c.txBufFull = false;
    c.shiftBusy = false;
    c.rxNext = 0;
    bool oldRts = rts(ch);
    // チャネルのリセットで WR1 の割り込みの許可、WR3 の受信の許可、WR5 の送信の許可と RTS が 0 になる。
    // WR15 は 11111000 (DCD、同期 / ハント、CTS、送信アンダーラン、ブレークの割り込みを選ぶ)
    wr_[ch][1] &= 0x24;
    wr_[ch][3] &= 0xfe;
    wr_[ch][5] &= 0x61;
    wr_[ch][15] = 0xf8;
    // 応答中の要因は残す (チャネルのリセットでは IUS は変わらない)
    if (oldRts && listener_) listener_->onRts(ch, false);
}

bool Scc::special(int ch) const {
    const Chan& c = chan_[ch];
    switch ((wr_[ch][1] >> 3) & 3) {
    case 0: return false;
    // 10 (すべての文字): 要求は FIFO の先頭の文字に付く。オーバーランの文字を読めば、RR1 のフラグが残っていても下ろす
    // (Zilog のユーザーマニュアル: この方式では IP は文字を FIFO から取り出したときに下ろす。FIFO は止めない)
    case 2: return c.count > 0 && c.fifoErr[0];
    // 01 と 11: Error Reset まで出し続ける
    default: return c.overrun;
    }
}

bool Scc::pending(int src) const {
    int ch = src < SRC_B_RX ? CH_A : CH_B;
    const Chan& c = chan_[ch];
    uint8_t wr1 = wr_[ch][1];
    switch (src % 3) {
    case 0: { // 受信 (と特殊受信条件)
        int mode = (wr1 >> 3) & 3;
        if (mode == 0) return false;
        if (special(ch)) return true;
        if (mode == 2) return c.count > 0 && !c.rxLockedRead;
        if (mode == 1) return c.rxFirstIp;
        return false;
    }
    case 1: // 送信
        return (wr1 & 0x02) && c.txIp;
    default: // 外部 / 状態
        return (wr1 & 0x01) && c.extIp;
    }
}

// 要求 (IP) が立っている要因のうち、優先度の最も高いもの。なければ -1。
// 割り込みを出すかどうか (MIE、応答中の IUS) は見ない: IP は要因ごとの許可だけで立つ
int Scc::highestIp() const {
    // 要因の番号は優先度の高い順 (NUM_SRC 個)。最初に要求の立っているものを返す
    int src = 0;
    while (src < NUM_SRC && !pending(src)) ++src;
    return src == NUM_SRC ? -1 : src;
}

int Scc::highestPending() const {
    if (!(wr_[CH_A][9] & 0x08)) return -1; // MIE
    for (int s = 0; s < NUM_SRC; s++) {
        // 応答中の要因があれば、それと同じか低い要因は出さない
        if (ius_ & (1u << s)) return -1;
        if (pending(s)) return s;
    }
    return -1;
}

// 要因の符号 (V3V2V1): B 送信 000、B 外部 001、B 受信 010、B 特殊受信 011、A 送信 100、A 外部 101、
// A 受信 110、A 特殊受信 111。要求がないときは 011
uint8_t Scc::vectorFor(int src) const {
    static const uint8_t kCode[NUM_SRC] = {6, 4, 5, 2, 0, 1};
    uint8_t code = 3;
    if (src >= 0) {
        code = kCode[src];
        if (src == SRC_A_RX && special(CH_A)) code = 7;
        if (src == SRC_B_RX && special(CH_B)) code = 3;
    }
    uint8_t v = wr_[CH_A][2];
    if (wr_[CH_A][9] & 0x10) {
        // 状態を上位に置く: V4 = 符号の bit2、V5 = bit1、V6 = bit0
        uint8_t hi = (uint8_t)(((code >> 2) & 1) | (code & 2) | ((code & 1) << 2));
        return (uint8_t)((v & 0x8f) | (hi << 4));
    }
    return (uint8_t)((v & 0xf1) | (code << 1));
}

int Scc::ack() {
    int s = highestPending();
    if (s < 0) return -1;
    ius_ |= (uint8_t)(1u << s);
    // 受信の要求は応答では下ろさない (最初の 1 文字はその文字を読んだとき、特殊受信条件は Error Reset で下ろす)
    if (wr_[CH_A][9] & 0x02) return -1; // NV: ベクタを出さない
    return (wr_[CH_A][9] & 0x01) ? vectorFor(s) : wr_[CH_A][2];
}

// FIFO の先頭が変わった。オーバーランのバイトなら特殊受信条件にし、01 と 11 のモードでは FIFO を止める
void Scc::topChanged(int ch) {
    Chan& c = chan_[ch];
    if (c.count == 0 || !c.fifoErr[0]) return;
    c.overrun = true;
    int mode = (wr_[ch][1] >> 3) & 3;
    if (mode == 1 || mode == 3) c.rxLocked = true;
}

void Scc::armFirst(int ch) {
    Chan& c = chan_[ch];
    if (c.count > 0 && !c.rxLockedRead) {
        // FIFO に文字が残っていれば、すぐ要求を出す
        c.rxFirstIp = true;
        c.rxFirstArmed = false;
    } else {
        c.rxFirstArmed = true;
    }
}

void Scc::receive(int ch, uint8_t data) {
    ch &= 1;
    if (!rxEnabled(ch)) return;
    Chan& c = chan_[ch];
    if (c.count >= FIFO_SIZE) {
        // 最後のバイトを上書きし、そのバイトのオーバーランのフラグを立てる (先頭に来たら特殊受信条件)
        c.fifo[FIFO_SIZE - 1] = data;
        c.fifoErr[FIFO_SIZE - 1] = true;
        return;
    }
    c.fifo[c.count] = data;
    c.fifoErr[c.count] = false;
    c.count++;
    if (c.rxFirstArmed) {
        c.rxFirstArmed = false;
        c.rxFirstIp = true;
    }
}

int Scc::readData(int ch) {
    Chan& c = chan_[ch];
    if (c.count == 0 || c.rxLockedRead) return -1;
    uint8_t v = c.fifo[0];
    for (int i = 1; i < c.count; i++) {
        c.fifo[i - 1] = c.fifo[i];
        c.fifoErr[i - 1] = c.fifoErr[i];
    }
    c.count--;
    c.fifoErr[c.count] = false;
    c.rxFirstIp = false; // 最初の 1 文字の要求は、その文字を読み出したときに下ろす
    if (c.rxLocked) {
        // 特殊受信条件のバイトを読んだ。Error Reset までは次のバイトを見せない
        c.rxLockedRead = true;
        return v;
    }
    topChanged(ch);
    return v;
}

// 1 文字の時間 (CPU サイクル)。スタートビット、データビット、パリティ、ストップビット (1.5 は半ビットで数える)
uint64_t Scc::charCycles(int ch, bool tx) const {
    static const uint8_t kBits[4] = {5, 7, 6, 8};
    static const uint8_t kMul[4] = {1, 16, 32, 64};
    static const uint8_t kStopHalf[4] = {2, 2, 3, 4};
    const uint8_t wr4 = wr_[ch][4];
    const int data = tx ? kBits[(wr_[ch][5] >> 5) & 3] : kBits[(wr_[ch][3] >> 6) & 3];
    const uint64_t half = (uint64_t)(2 * (1 + data + (wr4 & 1)) + kStopHalf[(wr4 >> 2) & 3]);
    const uint64_t tc = (uint64_t)wr_[ch][12] | ((uint64_t)wr_[ch][13] << 8);
    // 1 ビット = 2 x M x (TC + 2) / PCLK 秒。half 個の半ビットで half x M x (TC + 2) / PCLK 秒
    uint64_t n = (uint64_t)cpuHz_ * half * kMul[(wr4 >> 6) & 3] * (tc + 2) / PCLK;
    return n ? n : 1;
}

void Scc::tryLoadTx(int ch, uint64_t t) {
    Chan& c = chan_[ch];
    if (!c.txBufFull || c.shiftBusy) return;
    if (!(wr_[ch][5] & 0x08)) return;                     // 送信の許可
    if ((wr_[ch][3] & 0x20) && !(c.lines & RR0_CTS)) return; // Auto Enables: CTS がなければ送らない
    c.shiftByte = c.txBuf;
    c.txBufFull = false;
    c.shiftBusy = true;
    c.shiftEnd = t + charCycles(ch, true);
    c.txIp = true; // 送信バッファが空いた
}

void Scc::setLines(int ch, uint8_t lines) {
    Chan& c = chan_[ch];
    const uint8_t old = rr0Ext(ch);
    c.lines = lines;
    if (c.extLatched) return;
    uint8_t en = 0;
    if (wr_[ch][15] & 0x08) en |= RR0_DCD;
    if (wr_[ch][15] & 0x20) en |= RR0_CTS;
    // 選んだ線が変わったら、RR0 の値を止めて要求を出す (WR1 bit0 で外部 / 状態の割り込みを許可しているときだけ。
    // 許可していなければ RR0 は今の線の値をそのまま見せる)
    if (((old ^ lines) & en) && (wr_[ch][1] & 0x01)) {
        c.extLatched = true;
        c.extLatch = lines;
        c.extIp = true;
    }
}

void Scc::extReset(int ch) {
    Chan& c = chan_[ch];
    const bool was = c.extLatched;
    const uint8_t old = c.extLatch;
    c.extIp = false;
    c.extLatched = false;
    if (was) {
        // 止めている間に線が変わっていたら、また止めて要求を出す
        uint8_t en = 0;
        if (wr_[ch][15] & 0x08) en |= RR0_DCD;
        if (wr_[ch][15] & 0x20) en |= RR0_CTS;
        if (((old ^ c.lines) & en) && (wr_[ch][1] & 0x01)) {
            c.extLatched = true;
            c.extLatch = c.lines;
            c.extIp = true;
        }
    }
}

void Scc::updateA() {
    Chan& c = chan_[CH_A];
    const uint8_t lines = (link_ && link_->carrier()) ? RR0_EXT_MASK : 0;
    if (lines != c.lines) setLines(CH_A, lines);
    // 送信: 送り終えたバイトを渡し、次のバイトをシフトレジスタに移す
    while (c.shiftBusy && now_ >= c.shiftEnd) {
        c.shiftBusy = false;
        if (link_) link_->send(c.shiftByte);
        tryLoadTx(CH_A, c.shiftEnd);
    }
    if (!c.shiftBusy) tryLoadTx(CH_A, now_); // 送信の許可や CTS が変わった
    // 受信: 1 文字の時間ごとに 1 バイトまで
    if (!link_ || !rxEnabled(CH_A) || !rts(CH_A)) return;
    if ((wr_[CH_A][3] & 0x20) && !(c.lines & RR0_DCD)) return; // Auto Enables: DCD がなければ受け取らない
    if (c.rxNext > now_) return;
    uint64_t charTime = 0; // 届いたバイトがあるときだけ数える (割り算を毎回しない)
    for (;;) {
        const int b = link_->receive();
        if (b < 0) {
            c.rxNext = now_; // 次に届いたバイトは、届いたときに受け取る
            break;
        }
        if (!charTime) {
            charTime = charCycles(CH_A, false);
            if (c.rxNext + charTime < now_) c.rxNext = now_; // しばらく何も届いていなければ、今から数える
        }
        receive(CH_A, (uint8_t)b);
        c.rxNext += charTime;
        if (c.rxNext > now_) break;
    }
}

uint8_t Scc::readReg(int ch, int reg) {
    const Chan& c = chan_[ch];
    switch (reg) {
    case 0: { // RR0: bit0 受信あり、bit2 送信バッファ空、bit3 DCD、bit5 CTS、bit6 送信アンダーラン
        const bool rx = c.count > 0 && !c.rxLockedRead;
        const bool txEmpty = ch == CH_B || !c.txBufFull;
        return (uint8_t)((rx ? 0x01 : 0x00) | (txEmpty ? 0x04 : 0x00) | rr0Ext(ch) | 0x40);
    }
    case 1: { // RR1: bit0 全部送信済み、bit2-1 残りビットの符号 (8 ビットで 011)、bit5 オーバーラン
        const bool allSent = ch == CH_B || (!c.txBufFull && !c.shiftBusy);
        return (uint8_t)((allSent ? 0x01 : 0x00) | 0x06 | (c.overrun ? 0x20 : 0x00));
    }
    case 2: // RR2 (Z8530 Technical Manual 7.2.3): A は WR2 に書いたベクタそのまま。B は、VIS (WR9 bit0) に関係なく、
            // 優先度の最も高い IP の状態で書き換えたベクタ (表 7-4)。要求がなければ状態は 011 (上位に置くなら 110)
        if (ch == CH_A) return wr_[CH_A][2];
        {
            const int top = highestIp(); // 要求がなければ -1。vectorFor(-1) は「要求なし」の符号を入れる
            return vectorFor(top);
        }
    case 3: { // RR3 (A だけ): 割り込みの要求。bit5 A 受信、4 A 送信、3 A 外部、2 B 受信、1 B 送信、0 B 外部
        if (ch != CH_A) return 0;
        uint8_t v = 0;
        static const uint8_t kBit[NUM_SRC] = {0x20, 0x10, 0x08, 0x04, 0x02, 0x01};
        for (int s = 0; s < NUM_SRC; s++)
            if (pending(s)) v |= kBit[s];
        return v;
    }
    case 8: { // 受信データ (データのポートと同じ)
        int d = readData(ch);
        return d < 0 ? 0 : (uint8_t)d;
    }
    case 10: return 0x00;
    case 12: return wr_[ch][12];
    case 13: return wr_[ch][13];
    case 15: return wr_[ch][15];
    default: return 0x00;
    }
}

void Scc::command(int ch, uint8_t v) {
    Chan& c = chan_[ch];
    switch ((v >> 3) & 7) {
    case 2: // Reset Ext/Status Interrupts
        extReset(ch);
        break;
    case 4: // Enable Int on Next Rx Character
        armFirst(ch);
        break;
    case 5: // Reset Tx Int Pending
        c.txIp = false;
        break;
    case 6: { // Error Reset
        c.overrun = false;
        const bool wasRead = c.rxLockedRead;
        c.rxLocked = false;
        c.rxLockedRead = false;
        // 止めていたバイトを読んでいなければ、そのバイトは先頭に残る (オーバーランのフラグは下ろす)
        if (!wasRead && c.count > 0) c.fifoErr[0] = false;
        topChanged(ch);
        break;
    }
    case 7: // Reset Highest IUS
        for (int s = 0; s < NUM_SRC; s++) {
            if (ius_ & (1u << s)) {
                ius_ &= (uint8_t)~(1u << s);
                break;
            }
        }
        break;
    default: // Null、Point High (呼び出し側で処理)、Send Abort
        break;
    }
}

void Scc::writeReg(int ch, int reg, uint8_t v) {
    switch (reg) {
    case 0:
        command(ch, v);
        return;
    case 1: {
        const int oldMode = (wr_[ch][1] >> 3) & 3;
        wr_[ch][1] = v;
        // 「最初の 1 文字」のモードにしたら、次の文字 (FIFO に残っていればその文字) で要求を出す
        if (((v >> 3) & 3) == 1 && oldMode != 1) armFirst(ch);
        return;
    }
    case 2:
    case 9:
        if (reg == 9) {
            // bit7-6: 11 ハードウェアリセット、10 チャネル A のリセット、01 チャネル B のリセット
            int r = (v >> 6) & 3;
            if (r == 3) {
                // WR2 (ベクタ) はリセットで変わらない
                Listener* l = listener_;
                bool rtsB = rts(CH_B), rtsA = rts(CH_A);
                uint8_t vec = wr_[CH_A][2];
                listener_ = nullptr;
                reset();
                listener_ = l;
                wr_[CH_A][2] = wr_[CH_B][2] = vec;
                if (rtsA && l) l->onRts(CH_A, false);
                if (rtsB && l) l->onRts(CH_B, false);
                return;
            }
            if (r == 2) resetChannel(CH_A);
            if (r == 1) resetChannel(CH_B);
            v &= 0x3f;
        }
        wr_[CH_A][reg] = wr_[CH_B][reg] = v;
        return;
    case 3:
        wr_[ch][3] = v;
        if (ch == CH_A) tryLoadTx(ch, now_); // Auto Enables が変わった
        return;
    case 5: {
        bool old = rts(ch);
        wr_[ch][5] = v;
        bool now = rts(ch);
        if (old != now && listener_) listener_->onRts(ch, now);
        if (ch == CH_A) tryLoadTx(ch, now_); // 送信の許可が変わった
        return;
    }
    case 8: { // 送信データ
        Chan& c = chan_[ch];
        if (ch == CH_B) {
            // マウスの側は送るものがないので、すぐ送り終えたことにする
            c.txIp = true;
            return;
        }
        // バッファが空いていなければ上書きする (実物も前のバイトは失われる)
        c.txBuf = v;
        c.txBufFull = true;
        c.txIp = false;
        tryLoadTx(ch, now_);
        return;
    }
    default:
        wr_[ch][reg] = v;
        return;
    }
}

// offset 1: チャネル B コマンド、3: B データ、5: A コマンド、7: A データ (偶数番地はつながっていない)
uint8_t Scc::read(uint32_t offset) {
    if (!(offset & 1)) return 0xff;
    int ch = (offset & 4) ? CH_A : CH_B;
    if (offset & 2) {
        int d = readData(ch);
        return d < 0 ? 0 : (uint8_t)d;
    }
    int p = pointer_;
    pointer_ = 0;
    return readReg(ch, p);
}

void Scc::write(uint32_t offset, uint8_t v) {
    if (!(offset & 1)) return;
    int ch = (offset & 4) ? CH_A : CH_B;
    if (offset & 2) {
        writeReg(ch, 8, v);
        return;
    }
    if (pointer_ == 0) {
        // WR0: 下位 3 ビットがレジスタの番号。コマンド 1 (Point High) なら 8 を足す
        int p = v & 7;
        if (((v >> 3) & 7) == 1) p |= 8;
        command(ch, v);
        pointer_ = p;
        return;
    }
    int p = pointer_;
    pointer_ = 0;
    writeReg(ch, p, v);
}

} // namespace x68k
