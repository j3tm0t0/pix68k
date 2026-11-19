#include "x68k/dmac.h"

namespace x68k {

// エラーコード (CER)
enum : uint8_t {
    ERR_CONFIG = 0x01,
    ERR_TIMING = 0x02,
    ERR_ADDR_MEMORY = 0x05,
    ERR_ADDR_DEVICE = 0x06,
    ERR_ADDR_BASE = 0x07,
    ERR_BUS_MEMORY = 0x09,
    ERR_BUS_DEVICE = 0x0a,
    ERR_BUS_BASE = 0x0b,
    ERR_COUNT_MEMORY = 0x0d,
    ERR_COUNT_BASE = 0x0f,
    ERR_SOFT_ABORT = 0x11,
};

void Dmac::reset() {
    for (int ch = 0; ch < 4; ch++) {
        for (int i = 0; i < 64; i++) reg_[ch][i] = 0;
        // NIV / EIV のリセット値は 0x0F (未初期化割り込みのベクタ)
        reg_[ch][NIV] = 0x0f;
        reg_[ch][EIV] = 0x0f;
        pendingChain_[ch] = false;
        pendingError_[ch] = false;
        firstOperand_[ch] = false;
    }
    lastAck_ = 3;
    lastRun_ = 3;
}

uint8_t Dmac::read(uint32_t offset) {
    int ch = (int)((offset >> 6) & 3);
    int r = (int)(offset & 0x3f);
    if (r == GCR && ch != 3) return 0x00;
    return reg_[ch][r];
}

void Dmac::write(uint32_t offset, uint8_t v) {
    int ch = (int)((offset >> 6) & 3);
    int r = (int)(offset & 0x3f);
    switch (r) {
    case CSR: // 1 を書いたビットが消える (ACT と PCS は消えない)。ERR を消すと CER も 0 になる
        reg_[ch][CSR] &= (uint8_t)~(v & ~0x09);
        if (v & CSR_ERR) reg_[ch][CER] = 0;
        break;
    case CER: // 読み出し専用
        break;
    case CCR: {
        uint8_t old = reg_[ch][CCR];
        // STR は書いても 0 に戻せない (止めるのはソフトウェアアボート)
        reg_[ch][CCR] = (uint8_t)((v & ~CCR_SAB) | (old & CCR_STR));
        if (v & CCR_SAB) {
            if (active(ch)) error(ch, ERR_SOFT_ABORT);
            break;
        }
        if ((v & CCR_CNT) && (reg_[ch][OCR] & 0x08)) {
            // チェーンモードではコンティニューは使えない
            error(ch, ERR_CONFIG);
            break;
        }
        if ((v & CCR_STR) && !active(ch)) {
            start(ch);
        } else if ((v & CCR_CNT) && !active(ch)) {
            // 動いていないチャネルに STR なしで CNT を立てた
            error(ch, ERR_TIMING);
        }
        break;
    }
    case GCR:
        if (ch == 3) reg_[ch][r] = v & 0x0f;
        break;
    default:
        reg_[ch][r] = v;
        break;
    }
}

void Dmac::start(int ch) {
    if (reg_[ch][CSR] & (CSR_COC | CSR_BTC | CSR_NDT | CSR_ERR)) {
        error(ch, ERR_TIMING);
        return;
    }
    reg_[ch][CER] = 0;
    reg_[ch][CSR] |= CSR_ACT;
    firstOperand_[ch] = true;
    if (reg_[ch][OCR] & 0x08) {
        // チェーン: 最初の表の読み込みは notify() か run() でする (メモリを読むのに Host が要るため)
        pendingChain_[ch] = true;
        return;
    }
    if (get16(ch, MTC) == 0) error(ch, ERR_COUNT_MEMORY);
}

void Dmac::error(int ch, uint8_t code) {
    // 動いていたチャネルなら、デバイスに異常終了を知らせる
    if (active(ch)) pendingError_[ch] = true;
    reg_[ch][CER] = code;
    reg_[ch][CSR] = (uint8_t)((reg_[ch][CSR] | CSR_COC | CSR_ERR) & ~CSR_ACT);
    reg_[ch][CCR] &= (uint8_t)~(CCR_STR | CCR_CNT);
    pendingChain_[ch] = false;
}

void Dmac::finish(int ch) {
    reg_[ch][CSR] = (uint8_t)((reg_[ch][CSR] | CSR_COC) & ~CSR_ACT);
    reg_[ch][CCR] &= (uint8_t)~(CCR_STR | CCR_CNT);
}

void Dmac::notify(Host& host) {
    // チェーンで開始したチャネルは、転送の要求を待たずに最初の表を読む (開始の直後に CPU が MAR / MTC / BAR を
    // 読んでも、表から読んだ値が見える)
    for (int ch = 0; ch < 4; ch++)
        if (pendingChain_[ch]) startChain(ch, host);
    for (int ch = 0; ch < 4; ch++) {
        if (!pendingError_[ch]) continue;
        pendingError_[ch] = false;
        host.dmaError(ch);
    }
}

// チェーンのブロックを 1 つ読む (アレイ: BAR から 6 バイト、リンク: 10 バイト)。失敗したら false
bool Dmac::loadChain(int ch, Host& host) {
    uint32_t bar = get32(ch, BAR) & 0xffffff;
    if (bar & 1) { // 表はワードで読むので、奇数番地はアドレスエラー
        error(ch, ERR_ADDR_BASE);
        return false;
    }
    int fc = reg_[ch][BFC] & 7;
    uint8_t b[10];
    int n = (reg_[ch][OCR] & 0x04) ? 10 : 6;
    for (int i = 0; i < n; i++) {
        if (!host.dmaRead8(bar + (uint32_t)i, b[i], fc)) {
            error(ch, ERR_BUS_BASE);
            return false;
        }
    }
    set32(ch, MAR, (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]);
    set16(ch, MTC, (uint32_t)(b[4] << 8 | b[5]));
    // 表の転送数が 0 なら、BAR も BTC も新しくせずにカウントエラーで止まる。
    //   BAR を進めない: MC68450 のデータシートの 5.6.3.1.4 (チェーンで MTCR に 0 が入ったら、BAR と BTCR を新しくする前に
    //     止まる) にある。HD63450 のデータシート (エラーのあとのレジスタの説明) は「表を読もうとして止まったときの
    //     BAR は、読んでいた表を指す」としていて、同じ結果になる (カウントエラーもこの場合に入ると読んだ)。
    //   BTC を減らさない: 書いてあるのは MC68450 の 5.6.3.1.4 だけ。HD63450 のデータシートには BTC についての記述が
    //     ないので、MC68450 と同じとした (推定)
    if (get16(ch, MTC) == 0) {
        error(ch, ERR_COUNT_MEMORY);
        return false;
    }
    if (n == 10) {
        set32(ch, BAR, (uint32_t)b[6] << 24 | (uint32_t)b[7] << 16 | (uint32_t)b[8] << 8 | b[9]);
    } else {
        // アレイチェーンは、表を 1 つ読むたびに BTC を 1 減らし、BAR を 6 増やす (HD63450 のデータシートの
        // Array Chaining の節、MC68450 の 5.5.2)。
        // ブロックの転送中に読む BTC は、まだ読んでいない表の数になる
        set32(ch, BAR, bar + 6);
        set16(ch, BTC, get16(ch, BTC) - 1);
    }
    return true;
}

// 1 オペランド (バイト / ワード / ロングワード) を転送する。エラーで止まったら false
bool Dmac::transferOne(int ch, Host& host) {
    uint8_t ocr = reg_[ch][OCR], scr = reg_[ch][SCR];
    bool toMemory = (ocr & 0x80) != 0;
    int size = (ocr >> 4) & 3;
    bool port16 = (reg_[ch][DCR] & 0x08) != 0;
    int bytes = size == 1 ? 2 : size == 2 ? 4 : 1;
    // 8 ビットのデバイスは奇数番地か偶数番地の片方だけにつながるので、デバイス側のアドレスは 2 ずつ進む
    uint32_t devStride = port16 ? 1 : 2;
    // メモリ側のアドレスは Host が決める (既定は下位 24 ビット)。転送の途中で 24 ビットを超えても折り返さないよう、
    // 1 バイトごとに求める
    const uint32_t mar32 = get32(ch, MAR);
    uint32_t mar = mar32 & 0xffffff, dar = get32(ch, DAR) & 0xffffff;
    int mfc = reg_[ch][MFC] & 7, dfc = reg_[ch][DFC] & 7;
    if (bytes > 1) {
        // ワード / ロングワードは偶数番地でなければならない (8 ビットポートのデバイス側はバイトで読み書きする)
        if (mar & 1) {
            error(ch, ERR_ADDR_MEMORY);
            return false;
        }
        if (port16 && (dar & 1)) {
            error(ch, ERR_ADDR_DEVICE);
            return false;
        }
    }
    uint8_t data[4];
    for (int i = 0; i < bytes; i++) {
        bool ok = toMemory ? host.dmaRead8((dar + (uint32_t)i * devStride) & 0xffffff, data[i], dfc)
                           : host.dmaRead8(host.dmaMemAddr(ch, mar32 + (uint32_t)i), data[i], mfc);
        if (!ok) {
            error(ch, toMemory ? ERR_BUS_DEVICE : ERR_BUS_MEMORY);
            return false;
        }
    }
    for (int i = 0; i < bytes; i++) {
        bool ok = toMemory ? host.dmaWrite8(host.dmaMemAddr(ch, mar32 + (uint32_t)i), data[i], mfc)
                           : host.dmaWrite8((dar + (uint32_t)i * devStride) & 0xffffff, data[i], dfc);
        if (!ok) {
            error(ch, toMemory ? ERR_BUS_MEMORY : ERR_BUS_DEVICE);
            return false;
        }
    }
    // MAC / DAC: 01 = 増やす、10 = 減らす
    uint32_t memStep = (uint32_t)bytes, devStep = (uint32_t)bytes * devStride;
    mar = get32(ch, MAR);
    dar = get32(ch, DAR);
    if ((scr & 0x0c) == 0x04) mar += memStep;
    else if ((scr & 0x0c) == 0x08) mar -= memStep;
    if ((scr & 0x03) == 0x01) dar += devStep;
    else if ((scr & 0x03) == 0x02) dar -= devStep;
    set32(ch, MAR, mar);
    set32(ch, DAR, dar);
    return true;
}

// チェーンで開始したチャネルの最初の表を読む。エラーで止まったら false
bool Dmac::startChain(int ch, Host& host) {
    pendingChain_[ch] = false;
    if (!(reg_[ch][OCR] & 0x04) && get16(ch, BTC) == 0) { // アレイチェーンのブロック数が 0
        error(ch, ERR_COUNT_BASE);
        return false;
    }
    return loadChain(ch, host);
}

void Dmac::runChannel(int ch, Host& host, int maxBytes, LinkGuard* guard) {
    if (pendingChain_[ch] && !startChain(ch, host)) return;
    int budget = maxBytes;
    // runBusHolders() から呼ばれたときは、転送が DMAC のレジスタを書いたら、そのオペランドの後始末 (MTC、ブロックの
    // 終わり) まで済ませたところで戻る
    const bool* wrote = guard ? guard->wrote : nullptr;
    while (active(ch) && !(reg_[ch][CCR] & CCR_HLT) && budget > 0 && !(wrote && *wrote)) {
        int reqg = reg_[ch][OCR] & 3;
        // REQG = 11 は最初の 1 オペランドだけオートリクエストで転送し、そのあとは外部要求を待つ
        bool external = reqg == 2 || (reqg == 3 && !firstOperand_[ch]);
        if (external && !host.dmaReady(ch)) break;
        // デバイス (固定の番地) からメモリ (増やす) へのバイトの転送は、まとめて転送できるか Host に聞く
        // (FDC の読み込みで 1 フレームに数十 KB、ハードディスクの読み込みで数百 KB を 1 バイトずつ転送すると重いため)。
        // 外部要求のチャネルは dmaReadBlock()、オートリクエストのチャネル (REQG = 00 / 01) は dmaReadBlockAuto() で聞く。
        // REQG = 11 の最初の 1 オペランド (オートリクエスト) は聞かない: 次のバイトからは外部要求を待つので、
        // 続けて転送してはいけない
        // ブロックの最後のバイトは下の 1 バイトずつの処理に任せ、ブロックの終わり (チェーン、コンティニュー、終了) は変えない
        const uint8_t ocr0 = reg_[ch][OCR];
        const int size0 = (ocr0 >> 4) & 3; // 0 (8 ビット、パック) と 3 (8 ビット、パックなし) は 1 バイトずつ
        if ((external || reqg < 2) && (ocr0 & 0x80) && (size0 == 0 || size0 == 3) && (reg_[ch][SCR] & 0x0f) == 0x04) {
            const uint32_t left = get16(ch, MTC);
            const int n = left > 1 ? ((int)left - 1 < budget ? (int)left - 1 : budget) : 0;
            // メモリ側のアドレスは Host が決める (既定は下位 24 ビット。x68k/machine.cpp の dmaMemAddr)
            const uint32_t dar = get32(ch, DAR) & 0xffffff, mar = host.dmaMemAddr(ch, get32(ch, MAR));
            const int dfc = reg_[ch][DFC] & 7, mfc = reg_[ch][MFC] & 7;
            const int k = n <= 0 ? 0
                          : external ? host.dmaReadBlock(ch, dar, dfc, mar, mfc, n)
                                     : host.dmaReadBlockAuto(ch, dar, dfc, mar, mfc, n);
            if (k > 0) {
                set32(ch, MAR, get32(ch, MAR) + (uint32_t)k);
                set16(ch, MTC, left - (uint32_t)k);
                firstOperand_[ch] = false;
                budget -= k;
                continue;
            }
        }
        if (!transferOne(ch, host)) break; // エラー: dmaDone は呼ばず、dmaError で知らせる
        firstOperand_[ch] = false;
        budget--;
        uint32_t mtc = get16(ch, MTC) - 1;
        set16(ch, MTC, mtc);
        if (mtc != 0) continue;
        // ブロックの終わり
        uint8_t ocr = reg_[ch][OCR];
        if ((ocr & 0x0c) == 0x08) { // アレイチェーン: BTC は表を読んだときに減らしてある (loadChain())
            if (get16(ch, BTC) != 0) {
                if (loadChain(ch, host)) continue;
                return;
            }
        } else if ((ocr & 0x0c) == 0x0c) { // リンクチェーン
            const uint32_t next = get32(ch, BAR) & 0xffffff;
            if (next != 0) {
                if (!loadChain(ch, host)) return;
                if (guard) {
                    // 前に読んだ表の番地にもう一度来たら輪。表は読んであるので、ここで戻っても run() が続きを進められる
                    if (guard->power != 0 && next == guard->saved) {
                        guard->ring = true;
                        return;
                    }
                    if (guard->power == 0 || ++guard->count == guard->power) {
                        guard->saved = next;
                        guard->power = guard->power ? guard->power * 2 : 1;
                        guard->count = 0;
                    }
                }
                continue;
            }
        } else if (reg_[ch][CCR] & CCR_CNT) { // コンティニュー: 次のブロックは BAR / BTC / BFC
            reg_[ch][CSR] |= CSR_BTC;
            reg_[ch][CCR] &= (uint8_t)~CCR_CNT;
            set32(ch, MAR, get32(ch, BAR));
            set16(ch, MTC, get16(ch, BTC));
            reg_[ch][MFC] = reg_[ch][BFC];
            // コンティニューでは、DONE はブロックごとに出る (HD63450 のデータシートの DONE の説明、MC68450 の 5.5.1)。
            // DONE は前のブロックの最後の転送に伴って出るので、次のブロックの転送数が 0 (カウントエラー) でも出す
            // (エラーを見つける時機はデータシートに書かれておらず、ここは推定)。チャネル 0 では FDC の TC になり、
            // FDC のコマンドは 1 つ目のブロックの終わりで終わる
            host.dmaDone(ch);
            if (get16(ch, MTC) != 0) continue;
            error(ch, ERR_COUNT_MEMORY); // 次のブロックの転送数が 0
            return;
        }
        finish(ch);
        host.dmaDone(ch);
    }
}

void Dmac::run(Host& host, int maxBytes) {
    notify(host);
    // CPR の優先度の高い順に転送する。同じ優先度のチャネルは、前に転送したチャネルの次から順に
    for (int pri = 0; pri < 4; pri++) {
        int first = (lastRun_ + 1) & 3;
        for (int i = 0; i < 4; i++) {
            int ch = (first + i) & 3;
            if (!active(ch) || priority(ch) != pri) continue;
            runChannel(ch, host, maxBytes);
            lastRun_ = ch;
        }
    }
    notify(host);
}

void Dmac::runBusHolders(Host& host, const bool* wrote) {
    notify(host);
    auto selfWrite = [wrote] { return wrote && *wrote; };
    // 全部のチャネルを合わせて runChannel() を 65,536 回 (2 の 32 乗オペランド) まで
    uint32_t calls = 0;
    for (int pri = 0; pri < 4 && !selfWrite(); pri++) {
        for (int ch = 0; ch < 4 && !selfWrite(); ch++) {
            if (!holdsBus(ch) || priority(ch) != pri) continue;
            // オートリクエストの最大速度は要求を待たないので、runChannel() は 1 回ごとに必ず進む
            // (転送するか、終わるか、エラーで止まる)。終わらない転送は 2 種類で、どちらも気づいたところで戻る:
            // - リンクチェーンの輪 (guard.ring)
            // - 転送が DMAC のレジスタを書く (*wrote)。自分の MTC を書き戻す、CCR に CNT を立て直す、
            //   BTC を書き戻すなど。レジスタは write() でしか外から変わらないので、書き込みがなければ
            //   上の 2 つのほかに終わらない転送はない。書き込みがあったら、終わる転送かどうかは調べずに戻る
            //   (ほかのチャネルのレジスタでも、まだ転送していないチャネルが残っていても)
            // 念のため、合わせたオペランドの数にも上限を置く
            LinkGuard guard;
            guard.wrote = wrote;
            while (holdsBus(ch) && !guard.ring && !selfWrite() && calls < 65536) {
                runChannel(ch, host, 65536, &guard);
                calls++;
            }
        }
    }
    notify(host);
}

int Dmac::ack() {
    // CPR の優先度の最も高いチャネルが応答する。同じ優先度のチャネルは順番に応答する
    for (int pri = 0; pri < 4; pri++) {
        for (int i = 0; i < 4; i++) {
            int ch = (lastAck_ + 1 + i) & 3;
            if (priority(ch) != pri || !requesting(ch)) continue;
            lastAck_ = ch;
            return (reg_[ch][CSR] & CSR_ERR) ? reg_[ch][EIV] : reg_[ch][NIV];
        }
    }
    return -1;
}

} // namespace x68k
