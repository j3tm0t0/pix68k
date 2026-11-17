#include "x68k/mfp.h"

namespace x68k {

namespace {
enum Reg : int {
    GPIP = 0, AER, DDR, IERA, IERB, IPRA, IPRB, ISRA, ISRB, IMRA, IMRB, VR,
    TACR, TBCR, TCDCR, TADR, TBDR, TCDR, TDDR, SCR, UCR, RSR, TSR, UDR,
};
// ディレイモードのプリスケーラ (制御値 1..7)
const uint16_t kPrescale[8] = {0, 4, 10, 16, 50, 64, 100, 200};

// 割り込み要因 src が A/B どちらのレジスタの何ビット目か
inline int bank(int src) { return src >= 8 ? 0 : 1; } // 0: A、1: B
inline uint8_t bit(int src) { return (uint8_t)(1u << (src & 7)); }

// 動いているタイマの分周比を変えたときのプリスケーラの数えかけ。MC68901 のデータシート (Timers、Delay Mode):
// 「タイマが動いている間に分周比を変えると、最初のタイムアウトは 1 から 200 クロックの間の決まらない時刻になり、
// そのあとは正しい間隔になる」。数えかけが新しい分周比に届いていなければそのまま数え続け、
// すでに超えていれば 0 から数え直す (どちらも最初のカウントは 200 クロック以内に来る)。
// ctl はディレイモードの制御値 (1..7)。それ以外 (止まっている、イベントカウント、パルス幅) では何もしない
inline void fitPrescaler(uint32_t& count, int ctl) {
    if (ctl >= 1 && ctl <= 7 && count >= kPrescale[ctl]) count = 0;
}
} // namespace

bool Mfp::rxEnabled() const { return (reg_[RSR] & 0x01) != 0; }

void Mfp::reset() {
    for (int i = 0; i < 24; i++) reg_[i] = 0;
    reg_[TSR] = 0x80; // 送信バッファは空
    for (int i = 0; i < 4; i++) {
        counter_[i] = 0;
        prescale_[i] = 0;
    }
    rxFull_ = false;
    txData_ = -1;
}

void Mfp::request(int src) {
    int b = bank(src);
    if (reg_[IERA + b] & bit(src)) reg_[IPRA + b] |= bit(src);
}

int Mfp::ack() {
    // mfp.h の highestPending() が使うレジスタの番号 (メンバの中で確かめる。private なので)
    static_assert((int)R_IPRA == (int)IPRA && (int)R_IPRB == (int)IPRB && (int)R_ISRA == (int)ISRA &&
                      (int)R_ISRB == (int)ISRB && (int)R_IMRA == (int)IMRA && (int)R_IMRB == (int)IMRB &&
                      (int)R_VR == (int)VR,
                  "the register numbers in mfp.h must match Reg");
    int src = highestPending();
    if (src < 0) return -1;
    int b = bank(src);
    reg_[IPRA + b] &= (uint8_t)~bit(src);
    if (reg_[VR] & 0x08) reg_[ISRA + b] |= bit(src);
    return (reg_[VR] & 0xf0) | src;
}

uint8_t Mfp::read(int reg) {
    switch (reg) {
    case GPIP: {
        return (uint8_t)((gpipIn_ & ~reg_[DDR]) | (reg_[GPIP] & reg_[DDR]));
    }
    case TADR:
    case TBDR:
    case TCDR:
    case TDDR:
        return counter_[reg - TADR];
    case RSR:
        return (uint8_t)((reg_[RSR] & 0x7f) | (rxFull_ ? 0x80 : 0));
    case UDR:
        rxFull_ = false;
        return rxData_;
    default:
        return reg_[reg];
    }
}

void Mfp::write(int reg, uint8_t v) {
    switch (reg) {
    case IERA:
    case IERB:
        reg_[reg] = v;
        reg_[reg + 2] &= v; // 禁止した要因の保留は消える
        break;
    case IPRA:
    case IPRB:
    case ISRA:
    case ISRB:
        reg_[reg] &= v; // 0 を書いたビットだけ消える
        break;
    case TACR:
    case TBCR:
        reg_[reg] = v;
        // 止めたタイマはプリスケーラの途中の値を捨てる
        if (!(v & 0x0f)) prescale_[reg - TACR] = 0;
        // 動いているタイマのプリスケーラの分周比を変えた (下の fitPrescaler())
        fitPrescaler(prescale_[reg - TACR], v & 0x0f);
        break;
    case TCDCR:
        reg_[reg] = v;
        if (!(v & 0x70)) prescale_[2] = 0;
        if (!(v & 0x07)) prescale_[3] = 0;
        fitPrescaler(prescale_[2], (v >> 4) & 0x07);
        fitPrescaler(prescale_[3], v & 0x07);
        break;
    case TADR:
    case TBDR:
    case TCDR:
    case TDDR: {
        int t = reg - TADR;
        reg_[reg] = v;
        // 止まっているタイマはカウンタにもすぐ入る
        bool running;
        if (t == 0) running = (reg_[TACR] & 0x0f) != 0;
        else if (t == 1) running = (reg_[TBCR] & 0x0f) != 0;
        else if (t == 2) running = (reg_[TCDCR] & 0x70) != 0;
        else running = (reg_[TCDCR] & 0x07) != 0;
        if (!running) counter_[t] = v;
        break;
    }
    case TSR:
        reg_[TSR] = (uint8_t)(v | 0x80);
        break;
    case UDR:
        txData_ = v;
        break;
    case GPIP:
        reg_[GPIP] = v;
        break;
    default:
        reg_[reg] = v;
        break;
    }
}

void Mfp::setGpip(int b, bool level) {
    uint8_t m = (uint8_t)(1u << b);
    bool old = (gpipIn_ & m) != 0;
    if (old == level) return;
    if (level) gpipIn_ |= m;
    else gpipIn_ &= (uint8_t)~m;
    // AER のビットが 1 なら 0→1、0 なら 1→0 の変化で反応する
    bool active = ((reg_[AER] & m) != 0) == level;
    if (!active) return;
    static const int8_t kSrc[8] = {SRC_GPIP0, SRC_GPIP1, SRC_GPIP2, SRC_GPIP3,
                                   SRC_GPIP4, SRC_GPIP5, SRC_GPIP6, SRC_GPIP7};
    request(kSrc[b]);
    // タイマ A のイベントカウントモードは GPIP4 (TAI) を数える
    if (b == GPIP_VDISP && (reg_[TACR] & 0x0f) == 0x08) timerEvent(0);
}

bool Mfp::timerEvent(int t) {
    static const int8_t kSrc[4] = {SRC_TIMER_A, SRC_TIMER_B, SRC_TIMER_C, SRC_TIMER_D};
    if (counter_[t] == 1) {
        counter_[t] = reg_[TADR + t];
        request(kSrc[t]);
        return true;
    }
    counter_[t]--; // 0 は 256 として数える
    return false;
}

void Mfp::advance(uint32_t ticks) {
    for (int t = 0; t < 4; t++) {
        int ctl;
        if (t == 0) ctl = reg_[TACR] & 0x0f;
        else if (t == 1) ctl = reg_[TBCR] & 0x0f;
        else if (t == 2) ctl = (reg_[TCDCR] >> 4) & 0x07;
        else ctl = reg_[TCDCR] & 0x07;
        // ディレイモード (1..7) だけ時間で進める。8 はイベントカウント、9 以上はパルス幅測定 (未実装)
        if (ctl == 0 || ctl > 7) continue;
        uint32_t p = kPrescale[ctl];
        uint32_t acc = prescale_[t] + ticks;
        // プリスケーラが一周しなければカウンタは変わらない (周辺機器はほぼ CPU の命令ごとに進めるので、多くはこちら)
        if (acc < p) {
            prescale_[t] = acc;
            continue;
        }
        uint32_t events = acc / p;
        prescale_[t] = acc % p;
        // カウンタが 0 になるたびに再ロードして割り込みを立てる
        while (events > 0) {
            uint32_t c = counter_[t] ? counter_[t] : 256;
            if (events < c) {
                counter_[t] = (uint8_t)(c - events);
                break;
            }
            events -= c;
            counter_[t] = 1;
            timerEvent(t);
        }
    }
}

bool Mfp::sourceOpen(int src) const {
    const int b = bank(src);
    return (reg_[IERA + b] & reg_[IMRA + b] & bit(src)) != 0;
}

uint32_t Mfp::clocksToTimerIrq() const {
    static const int8_t kSrc[4] = {SRC_TIMER_A, SRC_TIMER_B, SRC_TIMER_C, SRC_TIMER_D};
    uint32_t best = NO_TIMER_IRQ;
    for (int t = 0; t < 4; t++) {
        int ctl;
        if (t == 0) ctl = reg_[TACR] & 0x0f;
        else if (t == 1) ctl = reg_[TBCR] & 0x0f;
        else if (t == 2) ctl = (reg_[TCDCR] >> 4) & 0x07;
        else ctl = reg_[TCDCR] & 0x07;
        if (ctl == 0 || ctl > 7 || !sourceOpen(kSrc[t])) continue; // advance() と同じく、ディレイモードだけ
        // advance() の数え方: プリスケーラが p に届くたびにカウンタを 1 減らし、1 から減らすときに要求を出す (0 は 256)
        const uint32_t p = kPrescale[ctl];
        const uint32_t first = prescale_[t] < p ? p - prescale_[t] : 1;
        const uint32_t count = counter_[t] ? counter_[t] : 256;
        const uint32_t clocks = first + (count - 1) * p;
        if (clocks < best) best = clocks;
    }
    return best;
}

void Mfp::receive(uint8_t data) {
    rxData_ = data;
    rxFull_ = true;
    request(SRC_RX_FULL);
}

int Mfp::takeTransmit() {
    int v = txData_;
    txData_ = -1;
    return v;
}

} // namespace x68k
