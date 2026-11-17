// SASI のハードディスクのインタフェース ($E96001-$E96007) と、その先につながるコントローラとドライブ。
//
// 根拠にした資料:
//   - 「Inside X68000」(桑野雅彦、1992) の SASI の章: バスの信号とフェーズ、X68000 のポート、6 バイトのコマンド、
//     主要コマンド (TEST DRIVE READY、RECALIBRATE、REQUEST SENSE STATUS、FORMAT DRIVE、READ、WRITE)、
//     4 バイトのセンスデータ、ステータスの値 (正常は $00、エラーは $02)、メッセージの値 ($00)
//   - 本にない細かいところ (センスデータのエラーコードの値: クラス 0 ドライブ、クラス 1 データ、クラス 2 コマンド。
//     ステータスのビット 5-6 の論理ユニット番号、FORMAT TRACK、SEEK などのコマンドコード) は、SASI のコントローラで
//     広く使われている割り当てを、担当の知識から書いた (仕様書は確かめていない)
//   - IPL ROM と Human68k (FORMAT.X) を動かして、出てきたコマンドを見た結果 (tests/x68k/boot_ipl.cpp の --sasi-trace)
//
// ポート (Inside X68000 の図 5):
//   $E96001 R/W  データ。読み書きすると、REQ に対する ACK を自動で返す
//   $E96003 R    バスの状態: bit4 MSG、bit3 C/D、bit2 I/O、bit1 BSY、bit0 REQ (1 が有効)
//           W    SEL を無効にする
//   $E96005 W    RST を約 300 マイクロ秒のあいだ有効にする (バスにつながったコントローラがすべてリセットされる)
//   $E96007 W    書いた値をバスに出して SEL を有効にする (ID n のコントローラはビット n)
//
// 流れ: バスフリー → セレクション ($E96007 に ID のビットを書くと、その ID のコントローラが BSY を出す。$E96003 に
// 書いて SEL を戻す) → コマンド (6 バイト) → データ (あれば) → ステータス (1 バイト) → メッセージ (1 バイト) → バスフリー。
//
// DMAC のチャネル 1 の転送要求は REQ そのもの (REQ が有効になると出て、ACK で戻る。Inside X68000)。
// 割り込み (IOC のハードディスク割り込み) がいつ出るかは、資料にも IPL ROM や Human68k の動きにも手がかりがない
// (どちらも割り込みを使わず、ポートを見て待つ)。ここでは、コマンドの処理が終わってステータスフェーズに入ったときに
// 出し、ステータスを読むか、割り込みに応答するか、リセットで消すことにした。
//
// ドライブは ID (0..7) ごとに論理ユニット 0 と 1 の 2 台まで (Inside X68000: 使われる論理ユニット番号は 0 と 1 だけ)。
// 1 ブロックは 256 バイト。イメージの読み込みを待つ間 (x68k/hdd.h の DiskCursor) は REQ を出さない。
// ベアメタルでも使えるよう、例外、RTTI、STL、stdio は使わない。
#pragma once

#include <stdint.h>

#include "x68k/hdd.h"

namespace x68k {

class StateIO;

class Sasi final {
public:
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)

    static constexpr uint32_t BLOCK_SIZE = 256;    // 1 ブロックのバイト数
    static constexpr int UNITS = 16;               // ID x 2 + 論理ユニット番号
    static constexpr uint32_t TRACK_BLOCKS = 33;   // 1 トラックのブロック数 (FORMAT TRACK の単位)

    // $E96003 で読めるバスの状態
    enum : uint8_t { LINE_REQ = 0x01, LINE_BSY = 0x02, LINE_IO = 0x04, LINE_CD = 0x08, LINE_MSG = 0x10 };
    // コマンド (1 バイト目)
    enum Command : uint8_t {
        CMD_TEST_DRIVE_READY = 0x00,
        CMD_RECALIBRATE = 0x01,
        CMD_REQUEST_SENSE = 0x03,
        CMD_FORMAT_DRIVE = 0x04,
        CMD_CHECK_TRACK_FORMAT = 0x05,
        CMD_FORMAT_TRACK = 0x06,
        CMD_FORMAT_BAD_TRACK = 0x07,
        CMD_READ = 0x08,
        CMD_WRITE = 0x0a,
        CMD_SEEK = 0x0b,
        CMD_ASSIGN_DRIVE = 0xc2, // ドライブのパラメータ (10 バイト) を渡す。X68000 の IOCS が装置の初期化で出す
    };
    // センスデータの 1 バイト目のエラーコード (クラス << 4 | コード)。bit7 は「番地が有効」
    enum : uint8_t {
        SENSE_NO_ERROR = 0x00,
        SENSE_WRITE_FAULT = 0x03,
        SENSE_NOT_READY = 0x04,       // クラス 0 (ドライブ): ドライブが準備できていない
        SENSE_DATA_ERROR = 0x11,
        SENSE_INVALID_COMMAND = 0x20, // クラス 2 (コマンド): 知らないコマンド
        SENSE_INVALID_ADDRESS = 0x21, // クラス 2: ブロックの番地が範囲の外
        SENSE_ADDRESS_VALID = 0x80,
    };
    static constexpr uint8_t STATUS_ERROR = 0x02; // ステータスのビット 1。ビット 5-6 に論理ユニット番号

    void reset();
    uint8_t read(uint32_t offset);
    void write(uint32_t offset, uint8_t v);

    // unit = ID x 2 + 論理ユニット番号 (0..15)。イメージは環境が持つ。nullptr で外す
    void attach(int unit, HardDiskImage* disk);
    void detach(int unit) { attach(unit, nullptr); } // 外す
    HardDiskImage* image(int unit) const { return unit >= 0 && unit < UNITS ? image_[unit] : nullptr; }

    // IOC のハードディスク割り込みの要求
    bool irq() const { return interrupt_; }
    void acknowledgeInterrupt() { interrupt_ = false; } // 割り込みに応答した
    // バスを使っている (BSY)
    bool busy() const { return stage_ != BUS_FREE; }
    // DMAC のチャネル 1 の転送要求 (REQ)。イメージの読み込みや FORMAT の続きも、ここで進める
    bool dataReady() { return request(); }
    // データインフェーズで、今すぐ渡せるバイトを dst に n バイトまで写す ($E96001 を続けて読むのと同じ)。
    // 写したバイト数を返す
    int readData(uint8_t* dst, int n);

    // コマンドが終わる (ステータスフェーズに入る) たびに呼ぶ。cmd は 6 バイト、sense はセンスデータの 1 バイト目
    using TraceHook = void (*)(void* user, const uint8_t* cmd, uint8_t status, uint8_t sense);
    void setTrace(TraceHook hook, void* user) {
        traceHook_ = hook;
        traceUser_ = user;
    }

private:
    enum Stage : uint8_t {
        BUS_FREE,
        SELECTED, // SEL が有効で、コントローラが BSY を返している
        COMMAND,
        DATA_IN,
        DATA_OUT,
        WORKING, // FORMAT で書いている間 (REQ を出さない)
        STATUS,
        MESSAGE,
        STAGE_COUNT
    };

    bool request();
    uint8_t lines();
    uint8_t takeByte();
    void putByte(uint8_t v);
    void select(uint8_t idBits);
    void busFree(); // バスフリーに戻る (転送の途中なら捨てる)
    void execute(); // 6 バイトそろったコマンドを実行する
    void startDisk(Stage stage, uint32_t block, uint32_t count);
    void startBuffer(Stage stage, uint32_t len);
    void dataOutDone();
    void work();
    void finish(uint8_t sense, bool addressValid = false);
    HardDiskImage* drive() const;
    uint32_t address() const { return (uint32_t)(cmd_[1] & 0x1f) << 16 | (uint32_t)cmd_[2] << 8 | cmd_[3]; }
    int lun() const { return cmd_[1] >> 5; }

    HardDiskImage* image_[UNITS] = {};
    Stage stage_ = BUS_FREE;
    bool interrupt_ = false;
    bool fromDisk_ = false; // データの相手がイメージ (cursor_) か、buf_ か
    uint8_t id_ = 0;        // セレクションで選ばれたコントローラ
    uint8_t cmd_[6] = {};   // 受け取ったコマンド
    uint8_t cmdLen_ = 0;
    uint8_t status_ = 0;    // ステータスフェーズで返すバイト
    uint8_t sense_[4] = {}; // REQUEST SENSE STATUS で返す 4 バイト
    uint8_t buf_[16] = {};  // センスデータ、ドライブのパラメータ
    uint32_t pos_ = 0, len_ = 0; // データの中の位置と長さ (バイト)
    DiskCursor cursor_;
    TraceHook traceHook_ = nullptr;
    void* traceUser_ = nullptr;
};

} // namespace x68k
