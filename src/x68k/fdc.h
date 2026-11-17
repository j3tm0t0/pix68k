// FDC (uPD72065、$E94001 状態、$E94003 データ) とドライブ制御 ($E94005、$E94007)。
//
// データの受け渡しは DMAC のチャネル 0 が $E94003 を読み書きして行う (X68000 は常に DMA)。
// DMAC は dataReady() でデータが用意できているか確かめ、転送が終わると terminalCount() を呼ぶ
// (実機では DMAC の DONE が FDC の TC につながっている)。DMA が異常終了したときは abortTransfer() を呼ぶ。
// 時間はかけず、コマンドを受け取った時点でディスクのデータを用意する。
#pragma once

#include <stdint.h>

#include "x68k/fdd.h"

namespace x68k {

class StateIO;

class Fdc {
public:
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)
    static constexpr int DRIVES = 4;

    void reset();
    uint8_t read(uint32_t offset);
    void write(uint32_t offset, uint8_t v);
    bool irq() const { return irq_; }
    // OPM のレジスタ $1B の bit7 (CT2): FDD を強制レディにする
    void setForceReady(bool on) { forceReady_ = on; }

    // ドライブにイメージを入れる / 出す (イメージは環境が持ち、ここは指すだけ)
    void insert(int drive, FloppyImage* image);
    void eject(int drive);
    FloppyImage* image(int drive) const { return (drive >= 0 && drive < DRIVES) ? drive_[drive] : nullptr; }
    // ソフトウェアが排出を指示したか (環境がイメージを書き戻して外す)。読むと消える
    bool takeEjectRequest(int drive);
    // ソフトウェアが LED の点滅や排出禁止を指示しているか (表示用)
    bool ejectMasked(int drive) const { return (ejectMask_ >> drive) & 1; }
    bool accessLed(int drive) const { return access_ >= 0 && access_ == drive; }
    // 読み書き系のコマンド (READ DATA、WRITE DATA、WRITE ID、SCAN など) が正常に終わって (ST0 の IC が 00) 結果を
    // 用意した回数。動作には使わない (ベンチマークが読み込みの回数を数える。状態の保存にも入れない)
    uint32_t transfersOk() const { return transfersOk_; }

    // ---- DMAC から ----
    bool dataReady();
    // DMA の読み込みをまとめてする: dataReady() を確かめてからデータのレジスタを読むことを、n バイトまで
    // (データがなくなるまで) 繰り返したのと同じことをして、dst に書いたバイト数を返す
    int readData(uint8_t* dst, int n);
    void terminalCount();
    // DMA が異常終了した (バスエラー、ソフトウェアアボート)
    void abortTransfer();

private:
    enum Phase { COMMAND, EXEC_READ, EXEC_WRITE, EXEC_WRITE_ID, RESULT };

    bool ready(int drive) const;
    bool seekPending() const; // SENSE INTERRUPT STATUS で返していないシークの終わりがある
    void execute();
    void startReadWrite();
    bool stepSector();       // R を次のセクタへ進める。EOT を過ぎたら false
    bool loadSector();       // 今の C/H/R/N のセクタを用意する。できなければ結果を作って false
    void advance();          // セクタを読み終えた / 書き終えたあとの処理
    void commitWrite();
    void compareScan();      // SCAN 系: 受け取ったセクタをディスクのデータと比べる
    void endRw(bool normal);  // 読み書き系のコマンドの終わり (結果 7 バイト + 割り込み)
    void setResult(int n, bool interrupt);
    int us() const { return cmd_[1] & 3; }
    int hd() const { return (cmd_[1] >> 2) & 1; }
    int track() const { return cylinder_[us()] * 2 + hd(); }

    FloppyImage* drive_[DRIVES] = {};
    bool inserted_[DRIVES] = {};
    uint8_t ejectReq_ = 0, ejectMask_ = 0;
    int access_ = -1;

    Phase phase_ = COMMAND;
    uint8_t cmd_[9] = {};
    int cmdLen_ = 0, cmdNeed_ = 0;
    uint8_t result_[7] = {};
    int resultLen_ = 0, resultPos_ = 0;

    uint8_t st0_ = 0, st1_ = 0, st2_ = 0;
    uint8_t seekSt0_[DRIVES] = {};  // SENSE INTERRUPT STATUS で返すシーク結果 (0 = なし)
    uint8_t cylinder_[DRIVES] = {};
    uint8_t idIndex_[DRIVES] = {};  // READ ID / READ DIAGNOSTIC で次に読むセクタ
    bool irq_ = false;
    uint32_t transfersOk_ = 0;
    bool forceReady_ = false;
    bool nonDma_ = false;           // SPECIFY の ND
    uint8_t driveCtrl_ = 0;

    // 読み書きの途中の状態
    const FloppyImage::Sector* sector_ = nullptr;
    int bufLen_ = 0, bufPos_ = 0;
    bool sectorDone_ = false;
    bool lastSector_ = false; // デリーテッドデータなどで、このセクタで終わる
    uint8_t buf_[8192] = {};  // ステートに入るので、使う前の中身も決まった値にしておく
    uint8_t scanBuf_[8192] = {}; // SCAN 系: ディスクのデータ (buf_ には CPU からのデータが入る)
};

} // namespace x68k
