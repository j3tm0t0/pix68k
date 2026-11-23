// DMAC (HD63450、$E84000-$E840FF、4 チャネル x 64 バイト)。
// X68000 では、チャネル 0 = FDC、1 = SASI (ハードディスク)、2 = 拡張スロット / メモリ間転送 (IOCS の DMAMOVE)、
// 3 = ADPCM。割り込みはレベル 3 で、ベクタは各チャネルの NIV (正常) / EIV (エラー)。
//
// 転送は run() で行う。デバイスからの要求で動くチャネル (OCR の REQG = 10) は、
// Host::dmaReady() が true の間だけ転送する。転送したバイトは Host のバスを通して読み書きする
// (デバイスのレジスタも、メモリと同じくアドレスで読み書きする)。
// ワード / ロングワードのオペランドもバイトに分けて読み書きするが、アドレスの計算は HD63450 と同じにする:
// メモリ側はオペランドの中を MAR, MAR+1, ... の順に、デバイス側は 8 ビットポートなら DAR, DAR+2, ...、
// 16 ビットポートなら DAR, DAR+1, ... の順にアクセスし、次のオペランドへは MAR をオペランドの大きさ、
// DAR をその 2 倍 (8 ビットポート) か同じだけ (16 ビットポート) 増減する。
// バスのファンクションコードは MFC (メモリ)、DFC (デバイス)、BFC (チェーンの表) の値を渡す。
//
// 正常に終わったチャネルは Host::dmaDone() (DONE の信号。コンティニューモードでは、次のブロックに移るときにも呼ぶ)、エラー (バスエラー、アドレスエラー、ソフトウェアアボートなど)
// で止まったチャネルは Host::dmaError() で知らせる。CCR への書き込みで起きたエラーは、次の run() か
// notify() で知らせる (write() は Host を持たないため)。
#pragma once

#include <stdint.h>

namespace x68k {

class StateIO;

class Dmac {
public:
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)
    class Host {
    public:
        virtual ~Host() {}
        // バスエラーなら false。fc はバスのファンクションコード (MFC / DFC / BFC の下位 3 ビット)
        virtual bool dmaRead8(uint32_t addr, uint8_t& v, int fc) = 0;
        virtual bool dmaWrite8(uint32_t addr, uint8_t v, int fc) = 0;
        // デバイスがデータを用意できているか (外部要求のチャネル)
        virtual bool dmaReady(int ch) = 0;
        // 転送が終わった (デバイスの TC に当たる)。コンティニューモードでは、ブロックが終わるたびに呼ぶ
        virtual void dmaDone(int ch) = 0;
        // 転送がエラーで止まった (デバイスは実行中の動作を異常終了する)
        virtual void dmaError(int ch) = 0;
        // チャネル ch のメモリ側のアドレス。mar は MAR の 32 ビットの値。本体のバスにはアドレスが 24 ビットしか出ないので、
        // 既定は下位 24 ビット。Machine は、060turbo (68060) のときのチャネル 0 と 1 だけ、ローカルメモリの
        // アドレスをそのまま返す (x68k/machine.cpp の dmaMemAddr)。返したアドレスが dmaRead8 / dmaWrite8 / dmaReadBlock に渡る
        virtual uint32_t dmaMemAddr(int ch, uint32_t mar) {
            (void)ch;
            return mar & 0xffffff;
        }
        // まとめて転送する (速くするためだけのもの。転送の結果は 1 バイトずつのときと同じでなければならない)。
        // 外部要求のチャネル ch で、デバイスの dar (固定) から 1 バイトずつ読み、メモリの mar から順に書く転送を、
        // n バイトまで進める。dmaReady(ch) を確かめてから dmaRead8(dar) と dmaWrite8(mar + i) をする 1 バイトずつの
        // 転送と、デバイスとメモリの中身が同じになる分だけ転送し、そのバイト数を返す。
        // 途中で止まる (準備ができていない、バスエラーになる) バイトの手前で止める。できなければ 0 (1 バイトずつに戻る)
        virtual int dmaReadBlock(int ch, uint32_t dar, int dfc, uint32_t mar, int mfc, int n) {
            (void)ch, (void)dar, (void)dfc, (void)mar, (void)mfc, (void)n;
            return 0;
        }
        // オートリクエスト (OCR の REQG = 00 / 01) のチャネル ch で、同じ向きの転送をまとめてする
        // (内蔵 SCSI の ROM は、SPC の DREG をこの方法で読む)。
        // dmaReady(ch) を確かめずに dmaRead8(dar) と dmaWrite8(mar + i) をする 1 バイトずつの転送と、デバイスとメモリの
        // 中身が同じになる分だけ転送し、そのバイト数を返す。できなければ 0 (1 バイトずつに戻る)
        virtual int dmaReadBlockAuto(int ch, uint32_t dar, int dfc, uint32_t mar, int mfc, int n) {
            (void)ch, (void)dar, (void)dfc, (void)mar, (void)mfc, (void)n;
            return 0;
        }
    };

    void reset();
    uint8_t read(uint32_t offset);
    void write(uint32_t offset, uint8_t v);

    // 動いているチャネルの転送を進める。1 回に転送する量は maxBytes まで
    void run(Host& host, int maxBytes = 65536);
    // write() のあとに呼ぶ: チェーンで開始したチャネルの最初の表を読み、write() で起きたエラー
    // (ソフトウェアアボートなど) を Host に知らせる
    void notify(Host& host);
    bool active(int ch) const { return (reg_[ch][CSR] & CSR_ACT) != 0; }
    // チャネル ch の転送バイト数 (MTC) の今の値 (FUK に、書き込むバイト数を先に知らせるために読む。x68k/fuk.h)
    uint32_t count(int ch) const { return get16(ch, MTC); }
    // チャネル ch が、MTC のバイト数で終わる 1 ブロックだけの転送をしているか (動いていて、アレイチェーンでも
    // リンクチェーンでもなく、コンティニューでもない)。そうでなければ、MTC は転送全体のバイト数ではない
    bool singleBlock(int ch) const { return active(ch) && (reg_[ch][OCR] & 0x0c) == 0 && !(reg_[ch][CCR] & CCR_CNT); }
    // オートリクエストの最大速度 (OCR の REQG = 01) で動いているチャネルがあるか。このチャネルは、転送が終わるまで
    // バスを CPU に返さない (Inside X68000 の PDF の 36、38、53 ページ)
    bool holdsBus(int ch) const { return active(ch) && (reg_[ch][OCR] & 3) == 1 && !(reg_[ch][CCR] & CCR_HLT); }
    bool holdsBus() const {
        for (int ch = 0; ch < 4; ch++)
            if (holdsBus(ch)) return true;
        return false;
    }
    // holdsBus() のチャネルだけ、転送を進める (ほかのチャネルは動かさない)。CPU が開始を書いたところで呼ぶ。
    // 終わりのある転送 (チェーンなし、コンティニュー、アレイチェーン、輪になっていないリンクチェーン) は、長さによらず
    // 最後まで転送する (途中で戻ると、CPU が次の命令で転送元やレジスタを書き換えられる)。
    // 終わらない転送 (実機では CPU が止まったままになる) は、次のところで戻り、残りは run() が少しずつ進める:
    // - リンクチェーンの輪: 同じ番地の表を 2 度読んだところ (輪を数周してから気づくこともある)
    // - 転送が DMAC のレジスタ ($E84000-$E840FF) を書いた: そのオペランドを終えたところ (自分の MTC や CCR を
    //   書き戻し続ける転送。終わる転送でも、どのチャネルのレジスタでも、書いたら戻る)。
    //   書いたことは Host が *wrote に立てて知らせる (Dmac はステートに丸ごと入るので、ここには置かない。
    //   Machine::ioWrite())。wrote が nullptr なら見ない
    // - 合わせて 2 の 32 乗オペランドを転送した (念のための上限。チェーンなし、コンティニュー、アレイチェーンは
    //   最大でもこれより短い。これを超えるリンクチェーンは途中で戻る)
    void runBusHolders(Host& host, const bool* wrote = nullptr);
    // run() でする仕事がない (動いているチャネルも、知らせていないエラーもない)
    bool idle() const {
        for (int ch = 0; ch < 4; ch++)
            if (active(ch) || pendingError_[ch]) return false;
        return true;
    }

    // 割り込みを要求しているチャネルがあるか (周辺機器を進めるたびに見るので、ヘッダに置いてインライン展開させる)
    bool irq() const {
        for (int ch = 0; ch < 4; ch++)
            if (requesting(ch)) return true;
        return false;
    }
    int ack(); // ベクタ番号。要求がなければ -1

private:
    // チャネルのレジスタの位置
    enum : int {
        CSR = 0x00, CER = 0x01, DCR = 0x04, OCR = 0x05, SCR = 0x06, CCR = 0x07,
        MTC = 0x0a, MAR = 0x0c, DAR = 0x14, BTC = 0x1a, BAR = 0x1c,
        NIV = 0x25, EIV = 0x27, MFC = 0x29, CPR = 0x2d, DFC = 0x31, BFC = 0x39, GCR = 0x3f,
    };
    enum : uint8_t {
        CSR_COC = 0x80, CSR_BTC = 0x40, CSR_NDT = 0x20, CSR_ERR = 0x10, CSR_ACT = 0x08,
        CCR_STR = 0x80, CCR_CNT = 0x40, CCR_HLT = 0x20, CCR_SAB = 0x10, CCR_INT = 0x08,
    };

    uint32_t get16(int ch, int r) const { return (uint32_t)(reg_[ch][r] << 8 | reg_[ch][r + 1]); }
    uint32_t get32(int ch, int r) const {
        return (uint32_t)reg_[ch][r] << 24 | (uint32_t)reg_[ch][r + 1] << 16 | (uint32_t)reg_[ch][r + 2] << 8 |
               reg_[ch][r + 3];
    }
    void set16(int ch, int r, uint32_t v) {
        reg_[ch][r] = (uint8_t)(v >> 8);
        reg_[ch][r + 1] = (uint8_t)v;
    }
    void set32(int ch, int r, uint32_t v) {
        reg_[ch][r] = (uint8_t)(v >> 24);
        reg_[ch][r + 1] = (uint8_t)(v >> 16);
        reg_[ch][r + 2] = (uint8_t)(v >> 8);
        reg_[ch][r + 3] = (uint8_t)v;
    }

    void start(int ch);
    void error(int ch, uint8_t code);
    void finish(int ch);
    bool loadChain(int ch, Host& host);
    bool startChain(int ch, Host& host);
    bool transferOne(int ch, Host& host);
    // リンクチェーンの輪を見つけるための記録 (Brent の方法: 覚えておく表の番地は 1 つ)
    struct LinkGuard {
        uint32_t saved = 0; // 比べる相手の表の番地
        uint32_t count = 0, power = 0; // saved を覚えてから読んだ表の数と、覚え直すまでの数 (0 = まだ覚えていない)
        bool ring = false;
        const bool* wrote = nullptr; // runBusHolders() の wrote
    };
    // guard があれば、リンクチェーンで同じ番地の表を 2 度読んだところで guard->ring を立てて戻る
    void runChannel(int ch, Host& host, int maxBytes, LinkGuard* guard = nullptr);
    bool requesting(int ch) const {
        return (reg_[ch][CCR] & CCR_INT) && (reg_[ch][CSR] & (CSR_COC | CSR_BTC | CSR_NDT | CSR_ERR));
    }
    int priority(int ch) const { return reg_[ch][CPR] & 3; } // 0 が最も高い

    uint8_t reg_[4][64] = {};
    bool pendingChain_[4] = {}; // 開始時のチェーンの表の読み込みを、次の notify() か run() でする
    bool pendingError_[4] = {}; // Host にまだ知らせていないエラー
    bool firstOperand_[4] = {}; // 開始してからまだ 1 オペランドも転送していない (REQG = 11 用)
    int lastAck_ = 3;           // 同じ優先度のチャネルの中で、最後に応答したチャネル
    int lastRun_ = 3;           // 同じ優先度のチャネルの中で、最後に転送したチャネル
};

} // namespace x68k
