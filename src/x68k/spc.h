// SCSI プロトコルコントローラ MB89352 (SPC) と、その先の SCSI バスにつながる装置 (ハードディスク、CD-ROM 装置)。
// 内蔵 SCSI ($E96021-$E9603F) と外付け SCSI ボード ($EA0001-$EA001F) で、同じものを 1 つずつ使う。
//
// 根拠にした資料:
//   - 「Inside X68000」(桑野雅彦、1992) の SCSI の章と、富士通 MB89352 のデータシート (1989 SCSI Product Guide):
//     レジスタの並びとビット、SPC のコマンド (Bus Release、Select、Set ATN、Reset ATN、Transfer、Transfer Pause、
//     Reset ACK/REQ、Set ACK/REQ)、転送モード (マニュアル転送は TEMP、ハード転送は DREG と 8 バイトの FIFO)、
//     SSTS の状態の表、割り込みの要因、セレクションタイムアウトのあとの手順
//   - SCSI-2 の規格 (X3T9.2/375R revision 10L。節と表の番号はこの版のもの): バスのフェーズ (6 章)、メッセージ、
//     コマンドとステータス (7 章)、どの装置にもあるコマンドとセンスデータ (8 章)、CD-ROM 装置のコマンドと
//     モードページ (14 章)
//   - IPL ROM の SCSI のドライバと Human68k を動かして、出てきたコマンドとレジスタの使い方を見た結果
//
// レジスタ (reg は番地の並びの順。奇数番地に 1 つずつ):
//    0 BDID  自分の ID (書くのは下位 3 ビット、読むと ID のビット)
//    1 SCTL  bit7 リセットと切り離し、bit6 転送の制御のリセット、bit5 診断、bit4 アービトレーション、bit3 パリティ、
//            bit2 セレクションに答える、bit1 リセレクションに答える、bit0 割り込みを出す
//    2 SCMD  bit7-5 コマンド、bit4 RST を出す、bit3 インターセプト、bit2 プログラム転送、bit0 ターミネーションモード
//    4 INTS  bit7 Selected、bit6 Reselected、bit5 Disconnected、bit4 Command Complete、bit3 Service Required、
//            bit2 Time Out、bit1 SPC Hard Error、bit0 Reset Condition。1 を書いたビットが消える
//    5 PSNS  (読む) bit7 REQ、bit6 ACK、bit5 ATN、bit4 SEL、bit3 BSY、bit2 MSG、bit1 C/D、bit0 I/O。(書く) SDGC
//    6 SSTS  bit7 イニシエータとして結合、bit6 ターゲットとして結合、bit5 SPC Busy、bit4 Transfer in Progress、
//            bit3 RST、bit2 TC = 0、bit1 FIFO がいっぱい、bit0 FIFO が空
//    7 SERR  エラー (ここでは起きないので 0)
//    8 PCTL  bit7 バスフリーの割り込みを出す、bit2-0 転送するフェーズ (MSG、C/D、I/O)
//    9 MBC   (読む) 転送カウンタの下位 4 ビット
//   10 DREG  ハード転送のデータ (FIFO)
//   11 TEMP  マニュアル転送のデータ、セレクションで出す ID のビット
//   12-14 TCH、TCM、TCL  転送カウンタ (24 ビット)。セレクションでは応答を待つ時間
//
// X68000 (イニシエータ) としての動きだけを作る。ターゲットとして選ばれること、リセレクション (ディスコネクトした
// ターゲットからの再接続) はない: つないだ装置はディスコネクトしない。
// 時間はかけない: セレクション、装置の処理、バスの上のバイトの受け渡しは、レジスタを読み書きしたその場で進む。
// ただし、イメージの読み込みを待つ間 (x68k/hdd.h の DiskCursor) は、装置が REQ を出さず、FIFO にデータが来ない。
//
// 内蔵 SCSI では、SPC の転送要求を DMAC のチャネル 1 につなぐ (dataReady()、readData())。
// 割り込みは、内蔵 SCSI がレベル 1 (ベクタ $6C)、外付け SCSI ボードがレベル 2 (ベクタ $F6)。Machine が扱う。
// ベアメタルでも使えるよう、例外、RTTI、STL、stdio は使わない。
#pragma once

#include <stdint.h>

#include "x68k/cd_drive.h"
#include "x68k/hdd.h"

namespace x68k {

class StateIO;

class Spc final {
public:
    Spc() {}
    ~Spc();
    Spc(const Spc&) = delete;
    Spc& operator=(const Spc&) = delete;
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)

    enum : int { BDID = 0, SCTL = 1, SCMD = 2, INTS = 4, PSNS = 5, SDGC = 5, SSTS = 6, SERR = 7, PCTL = 8, MBC = 9, DREG = 10, TEMP = 11, TCH = 12, TCM = 13, TCL = 14 };
    // SCTL のビット
    enum SctlBit : uint8_t {
        SCTL_RESET = 0x80,
        SCTL_CONTROL_RESET = 0x40,
        SCTL_DIAG = 0x20,
        SCTL_ARBITRATION = 0x10,
        SCTL_PARITY = 0x08,
        SCTL_SELECT = 0x04,
        SCTL_RESELECT = 0x02,
        SCTL_INT = 0x01,
    };
    // SCMD のコマンド (bit7-5) とほかのビット
    enum ScmdBit : uint8_t {
        CMD_BUS_RELEASE = 0x00,
        CMD_SELECT = 0x20,
        CMD_RESET_ATN = 0x40,
        CMD_SET_ATN = 0x60,
        CMD_TRANSFER = 0x80,
        CMD_TRANSFER_PAUSE = 0xa0,
        CMD_RESET_ACK = 0xc0,
        CMD_SET_ACK = 0xe0,
        CMD_MASK = 0xe0,
        SCMD_RST = 0x10,
        SCMD_INTERCEPT = 0x08,
        SCMD_PT = 0x04, // プログラム転送 (転送要求を出さない)
        SCMD_TERM = 0x01,
    };
    // INTS のビット (割り込みの要因)
    enum IntsBit : uint8_t {
        INTS_SELECTED = 0x80,
        INTS_RESELECTED = 0x40,
        INTS_DISCONNECTED = 0x20,
        INTS_COMPLETE = 0x10,
        INTS_SERVICE = 0x08,
        INTS_TIMEOUT = 0x04,
        INTS_HARD_ERROR = 0x02,
        INTS_RESET = 0x01,
    };
    enum : uint8_t { PSNS_REQ = 0x80, PSNS_ACK = 0x40, PSNS_ATN = 0x20, PSNS_SEL = 0x10, PSNS_BSY = 0x08, PSNS_MSG = 0x04, PSNS_CD = 0x02, PSNS_IO = 0x01 };
    // SSTS のビット
    enum SstsBit : uint8_t {
        SSTS_INIT = 0x80,
        SSTS_TARG = 0x40,
        SSTS_BUSY = 0x20,
        SSTS_TRIP = 0x10, // Transfer in Progress
        SSTS_RST = 0x08,
        SSTS_TC0 = 0x04,
        SSTS_FULL = 0x02,
        SSTS_EMPTY = 0x01,
    };
    enum : uint8_t { PCTL_BUSFREE_INT = 0x80 };
    // フェーズ (MSG、C/D、I/O。PSNS と PCTL の下位 3 ビット)
    enum : uint8_t { PH_DATA_OUT = 0, PH_DATA_IN = 1, PH_COMMAND = 2, PH_STATUS = 3, PH_MSG_OUT = 6, PH_MSG_IN = 7 };
    // ステータスとセンスキー
    enum : uint8_t { ST_GOOD = 0x00, ST_CHECK = 0x02 };
    enum SenseKey : uint8_t {
        KEY_NONE = 0x0,
        KEY_NOT_READY = 0x2,
        KEY_MEDIUM_ERROR = 0x3,
        KEY_ILLEGAL = 0x5,
        KEY_ATTENTION = 0x6,
        KEY_PROTECT = 0x7,
        KEY_BLANK = 0x8,
    };
    static constexpr int FIFO_SIZE = 8;

    void reset(); // X68000 のリセット
    // レジスタの読み書き。index は上の BDID..TCL (番地の並びの順)
    uint8_t read(int index);
    void write(int index, uint8_t value);

    // 装置をつなぐ (id は 0..7。image が nullptr なら外す)。イメージは環境が持つ。
    // cdrom なら CD-ROM 装置 (1 台まで。前の CD-ROM 装置は外れる)。image が nullptr か読み込んでいなければ、媒体なしでつなぐ。
    // toc は x68k/cd_drive.h。CD-ROM 装置は、つなぐたびに作り直す
    void attach(int id, HardDiskImage* image, bool cdrom = false, const CdToc* toc = nullptr);
    // CD-ROM 装置の媒体を入れ替える (image が nullptr なら取り出す)。装置は作り直さない。CD-ROM 装置がなければ false
    bool changeCd(HardDiskImage* image, const CdToc* toc = nullptr);
    HardDiskImage* image(int id) const { return id < 0 || id > 7 ? nullptr : isCdrom(id) ? cd_->image() : disk_[id]; }
    bool isCdrom(int id) const { return cd_ != nullptr && cd_->id() == id; }
    CdDrive* cd() const { return cd_; }

    bool irq() const { return !(sctl_ & SCTL_RESET) && ((ints_ & INTS_RESET) || (ints_ && (sctl_ & SCTL_INT))); }
    bool busy() const { return connected_ || xfer_; }
    // 転送要求 (DMA 転送のハード転送で、FIFO から読める / FIFO に書けるとき)。イメージの読み込みの続きも、ここで進める
    bool dataReady();
    // データインのハード転送で、今すぐ渡せるバイトを dst に n バイトまで写す (DREG を続けて読むのと同じ)。
    // needRequest なら、転送要求が出ているとき (プログラム転送でないとき) だけ。写したバイト数を返す
    int readData(uint8_t* dst, int n, bool needRequest);

    // コマンドが終わる (ステータスフェーズに入る) たびに呼ぶ。cdb はコマンド (6、10、12 バイト)
    using TraceHook = void (*)(void* user, int id, const uint8_t* cdb, uint8_t status);
    void setTrace(TraceHook hook, void* user) {
        traceHook_ = hook;
        traceUser_ = user;
    }

private:
    enum Source : uint8_t { SRC_NONE, SRC_BUF, SRC_CD, SRC_DISK, SRC_COUNT };

    // ---- SPC ----
    void chipReset();
    void command(uint8_t v);
    void writeControl(uint8_t value);
    void writeCommand(uint8_t value);
    void clearInterrupts(uint8_t bits);
    void pushFifo(uint8_t value);
    void setCounterByte(unsigned shift, uint8_t value);
    void releaseAck();
    void select();
    void startTransfer();
    void endTransfer(uint8_t ints);
    void run();
    void busReset();
    uint8_t ssts();
    uint8_t psns();
    uint8_t popFifo();
    uint8_t readData(); // DREG
    uint8_t readTemp(); // TEMP

    // ---- バスの向こう (ターゲット) ----
    bool present(int id) const { return id >= 0 && id < 8 && (disk_[id] != nullptr || isCdrom(id)); }
    void dropBus();
    void detach();
    void busFree(); // ターゲットがバスを放す
    void checkMedia();
    bool req();
    uint8_t busIn();
    void took();
    void gave(uint8_t v);
    void store(uint8_t v);
    void stored();
    void message(uint8_t v);
    void execute(); // そろったコマンドを実行する
    bool lunZero() const { return (identified_ ? lun_ : cdb_[1] >> 5) == 0; }
    void executeDisk();
    void executeCd();
    void requestSense();
    void inquiry();
    void modeSense(bool cd);
    void readWrite(bool write, uint32_t lba, uint32_t count);
    void cdRead(uint32_t lba, uint32_t count);
    void cdReadToc();
    void cdSubChannel();
    void cdPlay(uint32_t lba, uint32_t end);
    void toStatus(uint8_t status);
    void good() { toStatus(ST_GOOD); }
    void check(uint8_t key, uint8_t asc, uint8_t ascq = 0, uint8_t code = 0x70);
    void dataIn(Source src, uint32_t len);
    void dataOut(Source src, uint32_t len);
    uint8_t* sourceAt(uint32_t pos);

    // SPC のレジスタと転送の状態
    uint8_t bdid_ = 0;
    uint8_t sctl_ = SCTL_RESET;
    uint8_t scmd_ = 0;
    uint8_t ints_ = 0;
    uint8_t sdgc_ = 0;
    uint8_t pctl_ = 0;
    uint8_t temp_ = 0;
    uint8_t fifoCount_ = 0;
    uint8_t endInts_ = 0; // バスの上の転送は終わったが、FIFO を読み終えてから出す割り込みの要因
    uint8_t fifo_[FIFO_SIZE] = {};
    uint32_t transferCount_ = 0; // 転送カウンタ (24 ビット)
    bool connected_ = false; // イニシエータとしてターゲットと結合している (ターゲットが BSY を出している)
    bool xfer_ = false;      // ハード転送がバスの上で続いている
    bool fifoIn_ = false;    // FIFO の向き (ターゲットから)
    bool pad_ = false;       // パディング転送
    bool ack_ = false;       // ACK を出している
    bool ackValid_ = false;  // その ACK は REQ に答えたもの (戻したときにターゲットが次へ進む)
    bool atnOut_ = false;    // ATN を出している
    bool selWait_ = false;   // セレクションに答えがなく、SEL を出したまま

    // ターゲットの状態
    uint8_t target_ = 0;
    uint8_t tphase_ = PH_COMMAND;
    uint8_t lun_ = 0;
    uint8_t cdb_[12] = {};
    uint8_t cdbLen_ = 0;
    uint16_t msgExt_ = 0;      // 拡張メッセージの残り (0 なし、1 長さを待つ、2 以上は中身の残り + 1。257 まで)
    bool msgThenFree_ = false; // メッセージインのあとバスフリーにする (そうでなければコマンドフェーズへ)
    bool identified_ = false;  // IDENTIFY を受け取った (論理ユニット番号は lun_。コマンドの中の番号は見ない)
    uint8_t status_ = 0;
    uint8_t msgIn_ = 0;
    uint8_t ackByte_ = 0; // ACK を出している間のバスのバイト (マニュアル転送)
    Source src_ = SRC_NONE;
    bool writing_ = false; // データアウトの先がイメージ
    uint32_t pos_ = 0, len_ = 0;
    uint8_t sense_[8][4] = {}; // 装置ごとのセンスデータ: エラーコード (0 ならなし)、センスキー、追加センスコード、クオリファイア
    uint8_t buf_[256] = {};
    DiskCursor cursor_;

    HardDiskImage* disk_[8] = {};
    CdDrive* cd_ = nullptr;
    TraceHook traceHook_ = nullptr;
    void* traceUser_ = nullptr;
};

} // namespace x68k
