// MB89352 (SPC) と SCSI のハードディスク (x68k/spc.h) のテスト。CPU は使わず、レジスタを直接読み書きする。
// 期待する値は、「Inside X68000」の SCSI の章と MB89352 のデータシート (レジスタのビット、SPC のコマンド、SSTS の状態の表、
// 割り込みの要因)、SCSI-2 の規格 (フェーズの進み方、メッセージ、コマンドの応答、センスデータ) から決めた。
// CD-ROM 装置のコマンドは cdrom_test.cpp。最後に、Machine の上での番地、ROM の置き方、割り込みのベクタ (x68k/machine.h)。
#include <string.h>

#include <initializer_list>

#include "test_check.h"
#include "old_scsi_state_data.h"
#include "x68k/disk_cache.h"
#include "x68k/machine.h"
#include "x68k/spc.h"
#include "x68k/state.h"

namespace {

using x68k::HardDiskImage, x68k::Spc; // C++17 の複数の宣言

const uint32_t kBlocks = 300, kBlockSize = 512;
uint8_t g_disk[kBlocks * kBlockSize];

uint8_t pattern(uint32_t i) { return (uint8_t)(i * 7 + (i >> 9) * 3 + 5); }

void fillDisk() {
    for (uint32_t i = 0; i < sizeof g_disk; i++) g_disk[i] = pattern(i);
}

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

const uint8_t kIdle = Spc::SSTS_INIT;                                  // 結合中で、実行中の動作なし
const uint8_t kReqWaiting = Spc::SSTS_INIT | Spc::SSTS_TRIP;           // REQ が来ているが、転送していない
const uint8_t kHard = Spc::SSTS_INIT | Spc::SSTS_BUSY | Spc::SSTS_TRIP; // ハード転送の実行中

struct Rig {
    HardDiskImage img; // ID 2 のディスク
    Spc s; // ID 7 のイニシエータとして使う
    uint8_t buf[4096];
    uint32_t got = 0;
    explicit Rig(bool writeProtect = false) {
        fillDisk();
        CHECK_EQ(img.load(g_disk, sizeof g_disk, kBlockSize, writeProtect), true);
        s.attach(2, &img);
        init();
    }
    // SCSI のドライバがする初期化: リセットを解き、自分の ID を 7 にして、アービトレーションと割り込みを有効にする
    void init() {
        s.reset(); // 電源を入れたときの状態から
        s.write(Spc::SCTL, Spc::SCTL_RESET); // リセットの状態のまま ID を決める
        s.write(Spc::BDID, 7); // 自分の ID
        s.write(Spc::SCTL, Spc::SCTL_INT | Spc::SCTL_ARBITRATION); // リセットを解く
        s.write(Spc::INTS, 0xff); // 残っている要因があれば消す
    }
    void tc(uint32_t n) {
        for (int k = 0; k < 3; k++) s.write(Spc::TCH + k, (uint8_t)(n >> (16 - 8 * k))); // TCH、TCM、TCL の順
    }
    uint32_t tc() { return (uint32_t)s.read(Spc::TCH) << 16 | (uint32_t)s.read(Spc::TCM) << 8 | s.read(Spc::TCL); }
    uint8_t ints() {
        const uint8_t v = s.read(Spc::INTS);
        s.write(Spc::INTS, v);
        return v;
    }
    uint8_t phase() { return s.read(Spc::PSNS) & 7; }
    // Select: TEMP に自分と相手の ID、TCH / TCM に待つ時間。割り込みの要因を返す
    uint8_t select(int id, bool atn = false) {
        s.write(Spc::PCTL, 0);
        if (atn) s.write(Spc::SCMD, Spc::CMD_SET_ATN);
        s.write(Spc::TEMP, (uint8_t)(1u << 7 | 1u << id)); // 自分 (7) と相手の ID のビット
        tc(0x0f0003); // 応答を待つ時間
        scmd(Spc::CMD_SELECT);
        return ints();
    }
    // ハード転送 (プログラム転送) で送る / 受け取る。割り込みの要因を返す
    uint8_t out(uint8_t ph, const uint8_t* data, uint32_t n) {
        s.write(Spc::PCTL, ph);
        tc(n);
        scmd(Spc::CMD_TRANSFER | Spc::SCMD_PT); // プログラム転送
        for (uint32_t i = 0; i < n && (s.read(Spc::SSTS) & Spc::SSTS_BUSY); i++) s.write(Spc::DREG, data[i]);
        return ints();
    }
    uint8_t in(uint8_t ph, uint32_t n) {
        s.write(Spc::PCTL, ph);
        tc(n);
        scmd(Spc::CMD_TRANSFER | Spc::SCMD_PT);
        got = 0;
        while (!(s.read(Spc::SSTS) & Spc::SSTS_EMPTY) && got < sizeof buf) buf[got++] = s.read(Spc::DREG);
        return ints();
    }
    // マニュアル転送: PSNS で REQ を見て、TEMP を読み、Set ACK、Reset ACK
    void scmd(uint8_t command) { s.write(Spc::SCMD, command); }
    bool req() { return (s.read(Spc::PSNS) & Spc::PSNS_REQ) != 0; }
    void handshake() { // ACK を出して下げる。ターゲットは次のバイトか次のフェーズへ進む
        scmd(Spc::CMD_SET_ACK);
        scmd(Spc::CMD_RESET_ACK);
    }
    uint8_t manualIn() {
        CHECK_EQ(req(), true);
        const uint8_t byte = s.read(Spc::TEMP);
        handshake();
        return byte;
    }
    void manualOut(uint8_t byte) {
        CHECK_EQ(req(), true);
        s.write(Spc::TEMP, byte); // Set ACK のときに TEMP の値がターゲットに渡る
        handshake();
    }
    // ターゲットが phase の情報転送フェーズで REQ を出しているときの PSNS (ATN などほかの信号はない)
    static uint8_t requesting(uint8_t phase) { return uint8_t(phase | Spc::PSNS_BSY | Spc::PSNS_REQ); }
    // ステータスとメッセージ (COMMAND COMPLETE) を受け取り、バスフリーになるのを確かめる。ステータスを返す
    uint8_t end() {
        // PSNS はすべてのビット (ATN を含む) を比べる。ステータスとメッセージインでは ATN は出ていない
        CHECK_EQ(s.read(Spc::PSNS), requesting(Spc::PH_STATUS));
        const uint8_t st = manualIn();
        CHECK_EQ(s.read(Spc::PSNS), requesting(Spc::PH_MSG_IN));
        CHECK_EQ(manualIn(), 0x00);
        // バスフリー: BSY などの信号がなくなり、イニシエータとしての結合も解ける
        CHECK_EQ(s.read(Spc::PSNS), 0x00);
        CHECK_EQ((s.read(Spc::SSTS) & Spc::SSTS_INIT) != 0, false);
        return st; // ステータスのバイト
    }
    // コマンドを 1 つ実行する: セレクション、コマンド、データイン (n バイトまで) かデータアウト、ステータス、メッセージ
    int run(std::initializer_list<uint8_t> cdb, uint32_t n = 0, const uint8_t* data = nullptr, int id = 2) {
        got = 0;
        if (select(id) != Spc::INTS_COMPLETE) return -1;
        uint8_t c[12];
        uint32_t len = 0;
        for (uint8_t b : cdb) c[len++] = b;
        CHECK_EQ(out(Spc::PH_COMMAND, c, len), Spc::INTS_COMPLETE);
        if (n && phase() == Spc::PH_DATA_IN) in(Spc::PH_DATA_IN, n);
        else if (n && data && phase() == Spc::PH_DATA_OUT) out(Spc::PH_DATA_OUT, data, n);
        return end();
    }
    // REQUEST SENSE: センスキー << 16 | 追加センスコード << 8 | クオリファイア
    uint32_t sense() {
        CHECK_EQ(run({0x03, 0, 0, 0, 18, 0}, 18), 0);
        CHECK_EQ(got, 18);
        CHECK_EQ(buf[0], 0x70); // 拡張センスデータ
        CHECK_EQ(buf[7], 10);
        return (uint32_t)buf[2] << 16 | (uint32_t)buf[12] << 8 | buf[13];
    }
};

// ---------------------------------------------------------------- レジスタ

void testRegisters() {
    printf("registers after reset / bdid / tc / ints\n");
    fillDisk();
    Spc s;
    s.reset();
    // リセットのあとは、リセットと切り離しの状態 (SCTL の bit7 = 1)。ほかは 0。SSTS は TC = 0 と FIFO が空
    CHECK_EQ(s.read(Spc::SCTL), 0x80);
    CHECK_EQ(s.read(Spc::INTS), 0);
    CHECK_EQ(s.read(Spc::PSNS), 0); // バスの信号はどれも出ていない
    CHECK_EQ(s.read(Spc::SSTS), Spc::SSTS_TC0 | Spc::SSTS_EMPTY);
    CHECK_EQ(s.read(Spc::SERR), 0);
    CHECK_EQ(s.irq(), false);
    // BDID: 書くのは ID (下位 3 ビット)、読むと ID のビット。$07 を書くと $80 (Inside X68000)
    s.write(Spc::BDID, 7);
    CHECK_EQ(s.read(Spc::BDID), 0x80);
    s.write(Spc::BDID, 0xfb);
    CHECK_EQ(s.read(Spc::BDID), 0x08);
    // 転送カウンタは 24 ビット。MBC は下位 4 ビット
    s.write(Spc::TCH, 0x12);
    s.write(Spc::TCM, 0x34);
    s.write(Spc::TCL, 0x56);
    CHECK_EQ(s.read(Spc::TCH), 0x12);
    CHECK_EQ(s.read(Spc::TCM), 0x34);
    CHECK_EQ(s.read(Spc::TCL), 0x56);
    CHECK_EQ(s.read(Spc::MBC), 0x06);
    CHECK_EQ(s.read(Spc::SSTS) & Spc::SSTS_TC0, 0);
    // PCTL は bit7 と下位 3 ビット。TEMP は書いた値が読める (バスにターゲットのデータがないとき)
    s.write(Spc::PCTL, 0xff);
    CHECK_EQ(s.read(Spc::PCTL), 0x87);
    s.write(Spc::TEMP, 0xa5);
    CHECK_EQ(s.read(Spc::TEMP), 0xa5);
    // リセットの間は、Select も RST も受け付けない
    HardDiskImage img; // ID 0 のディスク
    CHECK_EQ(img.load(g_disk, sizeof g_disk, kBlockSize), true);
    s.attach(0, &img);
    s.write(Spc::TEMP, 0x81);
    s.write(Spc::SCMD, Spc::CMD_SELECT); // リセットの間なので無視される
    CHECK_EQ(s.read(Spc::INTS), 0);
    s.write(Spc::SCMD, Spc::SCMD_RST);
    CHECK_EQ(s.read(Spc::INTS), 0);
    s.write(Spc::SCMD, 0);
    // SCTL の bit7 に 1 を書くと、レジスタがリセットされる (BDID は残る)
    s.write(Spc::SCTL, 0x80);
    CHECK_EQ(s.read(Spc::PCTL), 0);
    CHECK_EQ(s.read(Spc::TCM), 0);
    CHECK_EQ(s.read(Spc::BDID), 0x08);
    // つないでいない番号のレジスタ
    CHECK_EQ(s.read(3), 0xff);
    CHECK_EQ(s.read(15), 0xff);
}

// ---------------------------------------------------------------- セレクション

void testSelect() {
    printf("select: command complete / time out\n");
    Rig r;
    // 相手がいる: Command Complete。イニシエータとして結合し、ターゲットはコマンドを求める
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    CHECK_EQ(r.s.read(Spc::PSNS), Spc::PSNS_REQ | Spc::PSNS_BSY | Spc::PH_COMMAND);
    CHECK_EQ(r.s.read(Spc::SSTS) & 0xf0, kReqWaiting);
    CHECK_EQ(r.s.busy(), true);
    // 結合している間の Select は何もしない
    r.s.write(Spc::SCMD, Spc::CMD_SELECT);
    CHECK_EQ(r.s.read(Spc::INTS), 0);
    r.init();
    CHECK_EQ(r.s.read(Spc::PSNS), 0);

    // 相手がいない: Time Out。SEL を出したままになる。待つ間に転送カウンタは 0 まで減る。
    // SEL を出している間は、SSTS の INIT が 1 (ディスクの SCSI のドライバは、これで打ち切るかやり直すかを決める)
    r.s.write(Spc::PCTL, 0);
    r.s.write(Spc::TEMP, 0x80 | 1 << 5);
    r.tc(0x0f0003);
    r.s.write(Spc::SCMD, Spc::CMD_SELECT);
    CHECK_EQ(r.s.read(Spc::INTS), Spc::INTS_TIMEOUT);
    CHECK_EQ(r.s.read(Spc::PSNS), Spc::PSNS_SEL);
    CHECK_EQ(r.s.read(Spc::SSTS) & 0xf0, Spc::SSTS_INIT);
    CHECK_EQ(r.tc(), 0);
    // TEMP に $00 を書いて ID を下げ、転送カウンタに短い時間を入れて Time Out を消すと、その時間だけ待って
    // もう一度 Time Out になる (IPL ROM のドライバの手順)。転送カウンタが 0 のときに消すと、SEL を戻して終わる
    r.s.write(Spc::TEMP, 0);
    CHECK_EQ(r.s.read(Spc::PSNS), Spc::PSNS_SEL);
    r.tc(0x000258);
    r.s.write(Spc::INTS, Spc::INTS_TIMEOUT);
    CHECK_EQ(r.s.read(Spc::INTS), Spc::INTS_TIMEOUT);
    CHECK_EQ(r.s.read(Spc::PSNS), Spc::PSNS_SEL);
    CHECK_EQ(r.tc(), 0);
    r.s.write(Spc::INTS, Spc::INTS_TIMEOUT);
    CHECK_EQ(r.s.read(Spc::INTS), 0);
    CHECK_EQ(r.s.read(Spc::PSNS), 0);
    CHECK_EQ(r.s.read(Spc::SSTS) & 0xf0, 0);
    // 待つ時間 (TCH / TCM) が 0 なら無限大: 割り込みは出ず、Select を持ったまま。Bus Release で取り消す
    r.s.write(Spc::TEMP, 0x80 | 1 << 5);
    r.tc(0x000003);
    r.s.write(Spc::SCMD, Spc::CMD_SELECT);
    CHECK_EQ(r.s.read(Spc::INTS), 0);
    CHECK_EQ(r.s.read(Spc::SSTS) & 0xf0, Spc::SSTS_INIT | Spc::SSTS_BUSY);
    r.s.write(Spc::SCMD, Spc::CMD_BUS_RELEASE);
    CHECK_EQ(r.s.read(Spc::SSTS) & 0xf0, 0);
    CHECK_EQ(r.s.read(Spc::PSNS), 0);
    // 自分の ID だけ (相手のビットがない)、リセレクション (PCTL の bit0 = 1): 答えるものがない
    r.s.write(Spc::TEMP, 0x80);
    r.tc(0x0f0003);
    r.s.write(Spc::SCMD, Spc::CMD_SELECT);
    CHECK_EQ(r.ints(), Spc::INTS_TIMEOUT);
    r.s.write(Spc::TEMP, 0);
    r.s.write(Spc::PCTL, 1);
    r.s.write(Spc::TEMP, 0x80 | 1 << 2);
    r.tc(0x0f0003);
    r.s.write(Spc::SCMD, Spc::CMD_SELECT);
    CHECK_EQ(r.ints(), Spc::INTS_TIMEOUT);
    r.s.write(Spc::TEMP, 0);

    // 割り込みの要求: 要因があって、SCTL の bit0 が 1 のとき
    CHECK_EQ(r.s.irq(), false);
    r.s.write(Spc::PCTL, 0);
    r.s.write(Spc::TEMP, 0x80 | 1 << 2);
    r.s.write(Spc::SCMD, Spc::CMD_SELECT);
    CHECK_EQ(r.s.irq(), true);
    r.s.write(Spc::SCTL, Spc::SCTL_ARBITRATION);
    CHECK_EQ(r.s.irq(), false);
    CHECK_EQ(r.s.read(Spc::INTS), Spc::INTS_COMPLETE); // 禁止していても、要因は INTS に出る
    r.s.write(Spc::SCTL, Spc::SCTL_ARBITRATION | Spc::SCTL_INT);
    CHECK_EQ(r.s.irq(), true);
    r.s.write(Spc::INTS, Spc::INTS_COMPLETE); // 1 を書いたビットだけ消える
    CHECK_EQ(r.s.read(Spc::INTS), 0);
    CHECK_EQ(r.s.irq(), false);
}

// ---------------------------------------------------------------- マニュアル転送

void testManual() {
    printf("manual transfer: temp, set ack, reset ack\n");
    Rig r;
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    // TEST UNIT READY を 1 バイトずつ。Set ACK で REQ が下がって ACK が上がり、Reset ACK で次の REQ が来る
    for (int i = 0; i < 6; i++) {
        CHECK_EQ(r.s.read(Spc::PSNS), Spc::PSNS_REQ | Spc::PSNS_BSY | Spc::PH_COMMAND);
        r.s.write(Spc::TEMP, 0x00);
        r.s.write(Spc::SCMD, Spc::CMD_SET_ACK);
        CHECK_EQ(r.s.read(Spc::PSNS), Spc::PSNS_ACK | Spc::PSNS_BSY | Spc::PH_COMMAND);
        r.s.write(Spc::SCMD, Spc::CMD_RESET_ACK);
    }
    // ステータスフェーズ。TEMP は、ACK を返すまで何度読んでも同じバイト
    CHECK_EQ(r.s.read(Spc::PSNS), Spc::PSNS_REQ | Spc::PSNS_BSY | Spc::PH_STATUS);
    CHECK_EQ(r.s.read(Spc::TEMP), 0x00);
    CHECK_EQ(r.s.read(Spc::PSNS), Spc::PSNS_REQ | Spc::PSNS_BSY | Spc::PH_STATUS);
    CHECK_EQ(r.end(), 0x00);

    // バスフリーの割り込み (PCTL の bit7): メッセージを受け取ってターゲットがバスを放すと Disconnected
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    for (int i = 0; i < 6; i++) r.manualOut(0);
    CHECK_EQ(r.manualIn(), 0x00);
    r.s.write(Spc::PCTL, Spc::PCTL_BUSFREE_INT | Spc::PH_MSG_IN);
    CHECK_EQ(r.s.read(Spc::INTS), 0);
    CHECK_EQ(r.manualIn(), 0x00);
    CHECK_EQ(r.s.read(Spc::INTS), Spc::INTS_DISCONNECTED);
    CHECK_EQ(r.s.read(Spc::PSNS), 0);
    r.ints();

    // データもマニュアル転送で受け取れる: READ CAPACITY の 8 バイト
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    for (uint8_t b : {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}) r.manualOut(b);
    uint8_t cap[8];
    for (int i = 0; i < 8; i++) {
        CHECK_EQ(r.phase(), Spc::PH_DATA_IN);
        cap[i] = r.manualIn();
    }
    CHECK_EQ(be32(cap), kBlocks - 1);
    CHECK_EQ(be32(cap + 4), kBlockSize);
    CHECK_EQ(r.end(), 0x00);
}

// ---------------------------------------------------------------- ハード転送

void testHardTransfer() {
    printf("hard transfer: fifo, transfer counter, command complete / service required\n");
    Rig r;
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    // コマンドフェーズの転送: 6 バイト。転送カウンタは 1 バイト送るごとに減り、0 になると Command Complete
    const uint8_t rd[6] = {0x08, 0, 0, 4, 1, 0}; // READ(6): ブロック 4 から 1 ブロック
    r.s.write(Spc::PCTL, Spc::PH_COMMAND);
    r.tc(6);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    CHECK_EQ(r.s.read(Spc::SSTS), kHard | Spc::SSTS_EMPTY);
    for (int i = 0; i < 5; i++) r.s.write(Spc::DREG, rd[i]);
    CHECK_EQ(r.tc(), 1);
    CHECK_EQ(r.s.read(Spc::INTS), 0);
    r.s.write(Spc::DREG, rd[5]);
    CHECK_EQ(r.tc(), 0);
    CHECK_EQ(r.ints(), Spc::INTS_COMPLETE);
    // ターゲットはデータインフェーズで REQ を出している。転送はしていない
    CHECK_EQ(r.s.read(Spc::PSNS), Spc::PSNS_REQ | Spc::PSNS_BSY | Spc::PH_DATA_IN);
    CHECK_EQ(r.s.read(Spc::SSTS), kReqWaiting | Spc::SSTS_TC0 | Spc::SSTS_EMPTY);

    // データインの転送: FIFO は 8 バイト。いっぱいになると、読むまでバスから取り込まない
    r.s.write(Spc::PCTL, Spc::PH_DATA_IN);
    r.tc(kBlockSize);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    CHECK_EQ(r.s.read(Spc::SSTS), kHard | Spc::SSTS_FULL);
    CHECK_EQ(r.tc(), kBlockSize - 8);
    CHECK_EQ(r.s.read(Spc::DREG), pattern(4 * kBlockSize));
    CHECK_EQ(r.tc(), kBlockSize - 9); // 1 バイト空いたので、次を取り込んだ
    CHECK_EQ(r.s.read(Spc::SSTS), kHard | Spc::SSTS_FULL);
    int diff = 0;
    for (uint32_t i = 1; i < kBlockSize - 8; i++) diff += r.s.read(Spc::DREG) != pattern(4 * kBlockSize + i);
    CHECK_EQ(diff, 0);
    // 最後の 8 バイトは FIFO の中。バスの上の転送は終わった (ターゲットはステータスフェーズ) が、
    // FIFO を読み終えるまでは実行中のままで、Command Complete もまだ出ない
    CHECK_EQ(r.tc(), 0);
    CHECK_EQ(r.s.read(Spc::INTS), 0);
    CHECK_EQ(r.s.read(Spc::SSTS), kHard | Spc::SSTS_TC0 | Spc::SSTS_FULL);
    CHECK_EQ(r.s.read(Spc::PSNS) & 7, Spc::PH_STATUS);
    for (uint32_t i = kBlockSize - 8; i < kBlockSize; i++) {
        CHECK_EQ(r.s.read(Spc::SSTS) & Spc::SSTS_EMPTY, 0);
        diff += r.s.read(Spc::DREG) != pattern(4 * kBlockSize + i);
    }
    CHECK_EQ(diff, 0);
    CHECK_EQ(r.s.read(Spc::SSTS), kReqWaiting | Spc::SSTS_TC0 | Spc::SSTS_EMPTY);
    CHECK_EQ(r.ints(), Spc::INTS_COMPLETE);
    // ステータスとメッセージもハード転送で。メッセージインの最後のバイトのあとは ACK が出たままで、
    // Reset ACK で戻すとバスフリーになる
    CHECK_EQ(r.in(Spc::PH_STATUS, 1), Spc::INTS_COMPLETE);
    CHECK_EQ(r.got, 1);
    CHECK_EQ(r.buf[0], 0x00);
    CHECK_EQ(r.in(Spc::PH_MSG_IN, 1), Spc::INTS_COMPLETE);
    CHECK_EQ(r.buf[0], 0x00);
    CHECK_EQ(r.s.read(Spc::PSNS), Spc::PSNS_ACK | Spc::PSNS_BSY | Spc::PH_MSG_IN);
    r.s.write(Spc::SCMD, Spc::CMD_RESET_ACK);
    CHECK_EQ(r.s.read(Spc::PSNS), 0);

    // 転送カウンタより先にターゲットがフェーズを変えたら Service Required。受け取った分は FIFO から読める
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    const uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, inq, 6), Spc::INTS_COMPLETE);
    CHECK_EQ(r.in(Spc::PH_DATA_IN, 255), Spc::INTS_SERVICE);
    CHECK_EQ(r.got, 36);
    CHECK_EQ(r.tc(), 255 - 36);
    // PCTL のフェーズがバスと違えば、転送は始まらずに Service Required
    r.s.write(Spc::PCTL, Spc::PH_DATA_IN);
    r.tc(4);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    CHECK_EQ(r.ints(), Spc::INTS_SERVICE);
    CHECK_EQ(r.tc(), 4);
    CHECK_EQ(r.s.read(Spc::SSTS) & 0xf0, kReqWaiting);
    CHECK_EQ(r.end(), 0x00);

    // コマンドを送っている途中でターゲットがフェーズを変えたとき (6 バイトのコマンドに 10 を指定): Service Required
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    const uint8_t tur[10] = {0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, tur, 10), Spc::INTS_SERVICE);
    CHECK_EQ(r.tc(), 4);
    CHECK_EQ(r.end(), 0x00);

    // 結合していないときの Transfer と DREG は何もしない
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    r.s.write(Spc::DREG, 0x55);
    CHECK_EQ(r.s.read(Spc::DREG), 0);
    CHECK_EQ(r.s.read(Spc::INTS), 0);
    CHECK_EQ(r.s.read(Spc::SSTS) & 0xf0, 0);
}

// パディング転送: 転送カウンタ 0、ターミネーションモード 1 のデータのフェーズ。ターゲットがフェーズを変えるまで、
// 受け取ったデータは捨て、送るデータは $00 (Inside X68000 の SCMD の説明)
void testPadding() {
    printf("padding transfer\n");
    Rig r;
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    const uint8_t rd[6] = {0x08, 0, 0, 4, 2, 0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, rd, 6), Spc::INTS_COMPLETE);
    r.s.write(Spc::PCTL, Spc::PH_DATA_IN);
    r.tc(0);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT | Spc::SCMD_TERM);
    CHECK_EQ(r.ints(), Spc::INTS_SERVICE);
    CHECK_EQ(r.s.read(Spc::SSTS) & Spc::SSTS_EMPTY, Spc::SSTS_EMPTY);
    CHECK_EQ(r.end(), 0x00);
    // 書き込み: ブロック 7 が $00 で埋まる
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    const uint8_t wr[6] = {0x0a, 0, 0, 7, 1, 0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, wr, 6), Spc::INTS_COMPLETE);
    r.s.write(Spc::PCTL, Spc::PH_DATA_OUT);
    r.tc(0);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT | Spc::SCMD_TERM);
    CHECK_EQ(r.ints(), Spc::INTS_SERVICE);
    CHECK_EQ(r.end(), 0x00);
    int nonzero = 0;
    for (uint32_t i = 7 * kBlockSize; i < 8 * kBlockSize; i++) nonzero += g_disk[i] != 0;
    CHECK_EQ(nonzero, 0);
    CHECK_EQ(g_disk[8 * kBlockSize], pattern(8 * kBlockSize));
    // ターミネーションモードが 0 なら、転送カウンタ 0 の Transfer はすぐ終わる
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    r.s.write(Spc::PCTL, Spc::PH_COMMAND);
    r.tc(0);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    CHECK_EQ(r.ints(), Spc::INTS_COMPLETE);
    CHECK_EQ(r.phase(), Spc::PH_COMMAND);
}

// ---------------------------------------------------------------- メッセージ

void testMessages() {
    printf("messages: identify, extended message, abort\n");
    Rig r;
    // ATN を出してセレクションすると、ターゲットはメッセージアウトフェーズにする
    CHECK_EQ(r.select(2, true), Spc::INTS_COMPLETE);
    CHECK_EQ(r.s.read(Spc::PSNS), Spc::PSNS_REQ | Spc::PSNS_ATN | Spc::PSNS_BSY | Spc::PH_MSG_OUT);
    // ハード転送でメッセージを送ると、最後のバイトで ATN が戻り、コマンドフェーズになる
    const uint8_t identify[1] = {0x80};
    CHECK_EQ(r.out(Spc::PH_MSG_OUT, identify, 1), Spc::INTS_COMPLETE);
    CHECK_EQ(r.s.read(Spc::PSNS), Spc::PSNS_REQ | Spc::PSNS_BSY | Spc::PH_COMMAND);
    const uint8_t tur[6] = {0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, tur, 6), Spc::INTS_COMPLETE);
    CHECK_EQ(r.end(), 0x00);

    // マニュアル転送では、ATN を出している間はメッセージアウトが続く。Reset ATN のあとの 1 バイトで終わる
    CHECK_EQ(r.select(2, true), Spc::INTS_COMPLETE);
    r.manualOut(0x08); // NO OPERATION
    CHECK_EQ(r.phase(), Spc::PH_MSG_OUT);
    r.s.write(Spc::SCMD, Spc::CMD_RESET_ATN);
    r.manualOut(0x80);
    CHECK_EQ(r.phase(), Spc::PH_COMMAND);
    for (int i = 0; i < 6; i++) r.manualOut(0);
    CHECK_EQ(r.end(), 0x00);

    // IDENTIFY の論理ユニット番号が 0 でない: INQUIRY は装置の種類 $7F を返し、ほかのコマンドは CHECK CONDITION
    // (ILLEGAL REQUEST、LOGICAL UNIT NOT SUPPORTED)
    const uint8_t identify1[1] = {0x81};
    CHECK_EQ(r.select(2, true), Spc::INTS_COMPLETE);
    CHECK_EQ(r.out(Spc::PH_MSG_OUT, identify1, 1), Spc::INTS_COMPLETE);
    const uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, inq, 6), Spc::INTS_COMPLETE);
    r.in(Spc::PH_DATA_IN, 36);
    CHECK_EQ(r.buf[0], 0x7f);
    CHECK_EQ(r.end(), 0x00);
    CHECK_EQ(r.select(2, true), Spc::INTS_COMPLETE);
    CHECK_EQ(r.out(Spc::PH_MSG_OUT, identify1, 1), Spc::INTS_COMPLETE);
    CHECK_EQ(r.out(Spc::PH_COMMAND, tur, 6), Spc::INTS_COMPLETE);
    CHECK_EQ(r.end(), 0x02);
    // コマンドの 2 バイト目の論理ユニット番号でも同じ
    CHECK_EQ(r.run({0x00, 0x20, 0, 0, 0, 0}), 0x02);
    CHECK_EQ(r.sense(), 0x052500);
    // IDENTIFY を受け取っていれば、コマンドの中の論理ユニット番号は見ない (SCSI-2 の 7.2.2)
    CHECK_EQ(r.select(2, true), Spc::INTS_COMPLETE);
    CHECK_EQ(r.out(Spc::PH_MSG_OUT, identify, 1), Spc::INTS_COMPLETE);
    const uint8_t tur1[6] = {0x00, 0x20, 0, 0, 0, 0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, tur1, 6), Spc::INTS_COMPLETE);
    CHECK_EQ(r.end(), 0x00);

    // 拡張メッセージ (同期転送の相談: 01h、長さ 3、01h、周期、オフセット) には MESSAGE REJECT (07h) を返し、
    // そのあとコマンドフェーズになる
    CHECK_EQ(r.select(2, true), Spc::INTS_COMPLETE);
    const uint8_t sdtr[6] = {0x80, 0x01, 0x03, 0x01, 0x32, 0x08};
    CHECK_EQ(r.out(Spc::PH_MSG_OUT, sdtr, 6), Spc::INTS_COMPLETE);
    CHECK_EQ(r.phase(), Spc::PH_MSG_IN);
    CHECK_EQ(r.manualIn(), 0x07);
    CHECK_EQ(r.phase(), Spc::PH_COMMAND);
    CHECK_EQ(r.out(Spc::PH_COMMAND, tur, 6), Spc::INTS_COMPLETE);
    CHECK_EQ(r.end(), 0x00);

    // 拡張メッセージの長さ 0 は 256 バイト、FFh は 255 バイト (SCSI-2 の 6.5 の表 11)。中身のバイトは、
    // メッセージとして読まない (06h は ABORT ではない)
    for (int lenByte : {0x00, 0xff, 0xfe, 0x01}) {
        const int body = lenByte ? lenByte : 256;
        CHECK_EQ(r.select(2, true), Spc::INTS_COMPLETE);
        r.manualOut(0x01);
        r.manualOut((uint8_t)lenByte);
        for (int i = 0; i < body - 1; i++) r.manualOut(0x06);
        CHECK_EQ(r.s.read(Spc::PSNS), Spc::PSNS_REQ | Spc::PSNS_ATN | Spc::PSNS_BSY | Spc::PH_MSG_OUT);
        r.s.write(Spc::SCMD, Spc::CMD_RESET_ATN);
        r.manualOut(0x06);
        CHECK_EQ(r.phase(), Spc::PH_MSG_IN);
        // MESSAGE REJECT をハード転送で受け取る: 最後のバイトで ACK を出したままになり、その間の TEMP は
        // 受け取ったバイト。Reset ACK でコマンドフェーズへ
        r.s.write(Spc::PCTL, Spc::PH_MSG_IN);
        r.tc(1);
        r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
        CHECK_EQ(r.s.read(Spc::DREG), 0x07);
        CHECK_EQ(r.ints(), Spc::INTS_COMPLETE);
        CHECK_EQ(r.s.read(Spc::PSNS) & Spc::PSNS_ACK, Spc::PSNS_ACK);
        CHECK_EQ(r.s.read(Spc::TEMP), 0x07);
        r.s.write(Spc::SCMD, Spc::CMD_RESET_ACK);
        CHECK_EQ(r.phase(), Spc::PH_COMMAND);
        CHECK_EQ(r.out(Spc::PH_COMMAND, tur, 6), Spc::INTS_COMPLETE);
        CHECK_EQ(r.end(), 0x00);
    }

    // ABORT (06h): ターゲットはバスフリーにする
    CHECK_EQ(r.select(2, true), Spc::INTS_COMPLETE);
    const uint8_t abortMsg[1] = {0x06};
    CHECK_EQ(r.out(Spc::PH_MSG_OUT, abortMsg, 1), Spc::INTS_COMPLETE);
    CHECK_EQ(r.s.read(Spc::PSNS), 0);
    // セレクションの前の Set ATN は、相手がいなければ捨てる
    CHECK_EQ(r.select(5, true), Spc::INTS_TIMEOUT);
    r.s.write(Spc::TEMP, 0);
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    CHECK_EQ(r.phase(), Spc::PH_COMMAND);
}

// ---------------------------------------------------------------- ハードディスクのコマンド

// ABORT のメッセージでターゲットがバスを放したら、ATN も戻る。次に Set ATN をせずにセレクションすると、
// メッセージアウトではなくコマンドフェーズになる (ATN を出したままにすると、ターゲットはまたメッセージを待つ)
void testAbortDropsAtn() {
    printf("abort releases ATN\n");
    Rig r;
    CHECK_EQ(r.select(2, true), Spc::INTS_COMPLETE);
    CHECK_EQ(r.s.read(Spc::PSNS), Spc::PSNS_REQ | Spc::PSNS_ATN | Spc::PSNS_BSY | Spc::PH_MSG_OUT);
    r.manualOut(0x06); // ABORT (ATN は出したまま)
    CHECK_EQ(r.s.read(Spc::PSNS), 0x00); // バスフリー。ATN もない
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    CHECK_EQ(r.s.read(Spc::PSNS), Spc::PSNS_REQ | Spc::PSNS_BSY | Spc::PH_COMMAND);
    const uint8_t tur[6] = {0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, tur, 6), Spc::INTS_COMPLETE);
    CHECK_EQ(r.end(), 0x00);
}

void testDiskCommands() {
    printf("disk commands: inquiry, read capacity, read / write, mode sense, request sense\n");
    Rig r;
    // INQUIRY: ダイレクトアクセス装置 ($00)、取り外しできない、追加データ長 31
    CHECK_EQ(r.run({0x12, 0, 0, 0, 36, 0}, 36), 0);
    CHECK_EQ(r.got, 36);
    CHECK_EQ(r.buf[0], 0x00);
    CHECK_EQ(r.buf[1] & 0x80, 0);
    CHECK_EQ(r.buf[2], 2); // SCSI-2
    CHECK_EQ(r.buf[3], 2);
    CHECK_EQ(r.buf[4], 31);
    CHECK_EQ(r.run({0x12, 0, 0, 0, 5, 0}, 36), 0); // 割り当て長で切る
    CHECK_EQ(r.got, 5);
    // READ CAPACITY: 最後のブロックの番地とブロックの長さ
    CHECK_EQ(r.run({0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 8), 0);
    CHECK_EQ(be32(r.buf), kBlocks - 1);
    CHECK_EQ(be32(r.buf + 4), kBlockSize);
    // TEST UNIT READY、REZERO UNIT、SEEK、START STOP UNIT
    CHECK_EQ(r.run({0x00, 0, 0, 0, 0, 0}), 0);
    CHECK_EQ(r.run({0x01, 0, 0, 0, 0, 0}), 0);
    CHECK_EQ(r.run({0x0b, 0, 0, 10, 0, 0}), 0);
    CHECK_EQ(r.run({0x1b, 0, 0, 0, 1, 0}), 0);

    // READ(6) と READ(10)
    CHECK_EQ(r.run({0x08, 0, 0, 3, 2, 0}, 2 * kBlockSize), 0);
    CHECK_EQ(r.got, 2 * kBlockSize);
    int diff = 0;
    for (uint32_t i = 0; i < 2 * kBlockSize; i++) diff += r.buf[i] != pattern(3 * kBlockSize + i);
    CHECK_EQ(diff, 0);
    CHECK_EQ(r.run({0x28, 0, 0, 0, 1, 0x2b, 0, 0, 1, 0}, kBlockSize), 0); // 最後のブロック (299)
    CHECK_EQ(r.got, kBlockSize);
    CHECK_EQ(r.buf[0], pattern(299 * kBlockSize));
    // READ(10) のブロック数 0 は、何も転送しない
    CHECK_EQ(r.run({0x28, 0, 0, 0, 0, 5, 0, 0, 0, 0}, kBlockSize), 0);
    CHECK_EQ(r.got, 0);
    // WRITE(6) と WRITE(10)
    static uint8_t data[2 * kBlockSize];
    for (uint32_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i ^ 0xc3);
    CHECK_EQ(r.run({0x0a, 0, 0, 20, 1, 0}, kBlockSize, data), 0);
    CHECK_EQ(memcmp(g_disk + 20 * kBlockSize, data, kBlockSize), 0);
    CHECK_EQ(g_disk[21 * kBlockSize], pattern(21 * kBlockSize));
    CHECK_EQ(r.img.dirty(), true);
    CHECK_EQ(r.run({0x2a, 0, 0, 0, 0, 30, 0, 0, 2, 0}, 2 * kBlockSize, data), 0);
    CHECK_EQ(memcmp(g_disk + 30 * kBlockSize, data, 2 * kBlockSize), 0);
    CHECK_EQ(g_disk[30 * kBlockSize - 1], pattern(30 * kBlockSize - 1));

    // 範囲の外: CHECK CONDITION、ILLEGAL REQUEST、LOGICAL BLOCK ADDRESS OUT OF RANGE。データフェーズにはならない
    CHECK_EQ(r.run({0x28, 0, 0, 0, 1, 0x2b, 0, 0, 2, 0}, kBlockSize), 0x02);
    CHECK_EQ(r.got, 0);
    CHECK_EQ(r.sense(), 0x052100);
    // センスデータは、引き取るか、次のコマンドで消える
    CHECK_EQ(r.sense(), 0x000000);
    CHECK_EQ(r.run({0x0a, 0, 0xff, 0, 1, 0}, kBlockSize, data), 0x02);
    CHECK_EQ(r.run({0x00, 0, 0, 0, 0, 0}), 0);
    CHECK_EQ(r.sense(), 0x000000);
    // 知らないコマンド: INVALID COMMAND OPERATION CODE
    CHECK_EQ(r.run({0x09, 0, 0, 0, 0, 0}), 0x02);
    CHECK_EQ(r.sense(), 0x052000);
    // REQUEST SENSE の割り当て長 0 は 4 バイト (X3.131-1986)
    CHECK_EQ(r.run({0x03, 0, 0, 0, 0, 0}, 18), 0);
    CHECK_EQ(r.got, 4);

    // MODE SENSE(6): ヘッダ 4 バイトとブロックディスクリプタ 8 バイト (ブロック数とブロックの長さ)、
    // ページ 03h と 04h (24 バイトずつ)
    CHECK_EQ(r.run({0x1a, 0, 0x3f, 0, 255, 0}, 255), 0);
    CHECK_EQ(r.got, 60);
    CHECK_EQ(r.buf[0], 59);
    CHECK_EQ(r.buf[12], 0x03);
    CHECK_EQ(r.buf[36], 0x04);
    CHECK_EQ(r.buf[2], 0x00); // 書き込み禁止ではない
    CHECK_EQ(r.buf[3], 8);
    CHECK_EQ(be32(r.buf + 4) & 0xffffff, kBlocks);
    CHECK_EQ(be32(r.buf + 8) & 0xffffff, kBlockSize);
    // ページ 03h (フォーマットのパラメータ): 1 トラック 32 セクタ、1 セクタ 512 バイト、ハードセクタ
    CHECK_EQ(r.run({0x1a, 0, 0x03, 0, 255, 0}, 255), 0);
    CHECK_EQ(r.got, 36);
    CHECK_EQ(r.buf[12], 0x03);
    CHECK_EQ(r.buf[13], 0x16);
    CHECK_EQ(be32(r.buf + 12 + 8) & 0xffff, 32);
    CHECK_EQ(be32(r.buf + 12 + 10) & 0xffff, kBlockSize);
    CHECK_EQ(r.buf[12 + 20], 0x40);
    // ページ 04h (ドライブの形): 8 ヘッド。300 ブロックは 1 シリンダ (8 × 32 = 256 ブロック) と端数
    CHECK_EQ(r.run({0x1a, 0, 0x04, 0, 255, 0}, 255), 0);
    CHECK_EQ(r.got, 36);
    CHECK_EQ(r.buf[12], 0x04);
    CHECK_EQ(r.buf[13], 0x16);
    CHECK_EQ(be32(r.buf + 12 + 1) & 0xffffff, 1);
    CHECK_EQ(r.buf[12 + 5], 8);
    // PC = 1 (変えられるビット): ページの中身は全部 0
    CHECK_EQ(r.run({0x1a, 0, 0x44, 0, 255, 0}, 255), 0);
    CHECK_EQ(r.buf[12], 0x04);
    CHECK_EQ(r.buf[12 + 5], 0);
    // DBD = 1 ならブロックディスクリプタを省く。MODE SENSE(10) はヘッダが 8 バイト
    CHECK_EQ(r.run({0x1a, 0x08, 0x03, 0, 255, 0}, 255), 0);
    CHECK_EQ(r.got, 28);
    CHECK_EQ(r.buf[4], 0x03);
    CHECK_EQ(r.run({0x5a, 0, 0x3f, 0, 0, 0, 0, 0, 255, 0}, 255), 0);
    CHECK_EQ(r.got, 64);
    CHECK_EQ(be32(r.buf) >> 16, 62);
    CHECK_EQ(r.buf[16], 0x03);
    // ないページ: INVALID FIELD IN CDB
    CHECK_EQ(r.run({0x1a, 0, 0x05, 0, 255, 0}, 255), 0x02);
    CHECK_EQ(r.sense(), 0x052400);
    // MODE SELECT: パラメータを受け取って、正常に終わる
    CHECK_EQ(r.run({0x15, 0, 0, 0, 12, 0}, 12, data), 0);

    // READ(6) のブロック数 0 は 256 ブロック (SCSI-2 の 9.2.5)
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    const uint8_t rd256[6] = {0x08, 0, 0, 10, 0, 0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, rd256, 6), Spc::INTS_COMPLETE);
    r.s.write(Spc::PCTL, Spc::PH_DATA_IN);
    r.tc(0x30000);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    uint32_t count = 0;
    uint8_t lastByte = 0;
    while (!(r.s.read(Spc::SSTS) & Spc::SSTS_EMPTY) && count < 0x30000) {
        lastByte = r.s.read(Spc::DREG);
        count++;
    }
    CHECK_EQ(count, 256 * kBlockSize);
    CHECK_EQ(lastByte, pattern(266 * kBlockSize - 1));
    CHECK_EQ(r.ints(), Spc::INTS_SERVICE); // 転送カウンタが残っているうちに、ステータスフェーズになった
    CHECK_EQ(r.end(), 0x00);
    // READ(6) と SEEK(6) の番地は 21 ビット。上の 3 ビットは論理ユニット番号で、IDENTIFY を受け取っていれば見ない
    const uint8_t id0[1] = {0x80};
    CHECK_EQ(r.select(2, true), Spc::INTS_COMPLETE);
    CHECK_EQ(r.out(Spc::PH_MSG_OUT, id0, 1), Spc::INTS_COMPLETE);
    const uint8_t rdLun[6] = {0x08, 0xe0, 0, 3, 1, 0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, rdLun, 6), Spc::INTS_COMPLETE);
    CHECK_EQ(r.phase(), Spc::PH_DATA_IN);
    r.in(Spc::PH_DATA_IN, kBlockSize);
    CHECK_EQ(r.got, kBlockSize);
    CHECK_EQ(r.buf[0], pattern(3 * kBlockSize));
    CHECK_EQ(r.end(), 0x00);
    CHECK_EQ(r.select(2, true), Spc::INTS_COMPLETE);
    CHECK_EQ(r.out(Spc::PH_MSG_OUT, id0, 1), Spc::INTS_COMPLETE);
    const uint8_t seekLun[6] = {0x0b, 0xe0, 0, 10, 0, 0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, seekLun, 6), Spc::INTS_COMPLETE);
    CHECK_EQ(r.end(), 0x00);
    // VERIFY: 範囲の中なら正常に終わり (データは転送しない)、外なら LOGICAL BLOCK ADDRESS OUT OF RANGE
    CHECK_EQ(r.run({0x2f, 0, 0, 0, 1, 0x2b, 0, 0, 1, 0}), 0);
    CHECK_EQ(r.run({0x2f, 0, 0, 0, 0, 0, 0, 1, 0x2c, 0}), 0); // 0 から 300 ブロック
    CHECK_EQ(r.run({0x2f, 0, 0, 0, 1, 0x2b, 0, 0, 2, 0}), 0x02);
    CHECK_EQ(r.sense(), 0x052100);
    CHECK_EQ(r.run({0x2f, 0, 0, 0, 0, 0, 0, 1, 0x2d, 0}), 0x02);
    CHECK_EQ(r.sense(), 0x052100);
    CHECK_EQ(r.run({0x2f, 0, 0, 0, 1, 0x2c, 0, 0, 0, 0}), 0); // 長さ 0 はエラーではない (最後のブロックの次でも)
    CHECK_EQ(r.run({0x2f, 0, 0, 0, 1, 0x2d, 0, 0, 0, 0}), 0x02);
    CHECK_EQ(r.sense(), 0x052100);
    CHECK_EQ(r.run({0x0b, 0, 1, 0x2c, 0, 0}), 0x02); // SEEK(6) も同じ
    CHECK_EQ(r.sense(), 0x052100);

    // ハード転送の途中で転送カウンタに 0 を書くと、そこで転送が終わる (カウンタは 0 のまま)
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    const uint8_t rd2[6] = {0x08, 0, 0, 3, 2, 0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, rd2, 6), Spc::INTS_COMPLETE);
    r.s.write(Spc::PCTL, Spc::PH_DATA_IN);
    r.tc(2 * kBlockSize);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    for (int i = 0; i < 10; i++) r.s.read(Spc::DREG);
    r.tc(0);
    count = 0;
    while (!(r.s.read(Spc::SSTS) & Spc::SSTS_EMPTY) && count < 100) {
        r.s.read(Spc::DREG);
        count++;
    }
    CHECK_EQ(count <= 8, true); // FIFO に残っていた分だけ
    CHECK_EQ(r.tc(), 0);
    CHECK_EQ(r.ints(), Spc::INTS_COMPLETE);
    {
        static uint8_t st[4096];
        x68k::StateIO out(x68k::StateIO::SAVE, st, sizeof st);
        r.s.state(out);
        x68k::StateIO check(x68k::StateIO::CHECK, st, out.pos());
        r.s.state(check);
        CHECK_EQ(check.ok(), true);
    }
    r.init();

    // 書き込み禁止のイメージ: WRITE は DATA PROTECT、WRITE PROTECTED。MODE SENSE の WP ビットが 1
    Rig p(true);
    CHECK_EQ(p.run({0x0a, 0, 0, 20, 1, 0}, kBlockSize, data), 0x02);
    CHECK_EQ(p.sense(), 0x072700);
    CHECK_EQ(g_disk[20 * kBlockSize], pattern(20 * kBlockSize));
    CHECK_EQ(p.run({0x1a, 0, 0, 0, 255, 0}, 255), 0);
    CHECK_EQ(p.buf[2], 0x80);
    CHECK_EQ(p.img.dirty(), false);

    // コマンドが終わるたびに、ID、コマンド、ステータスを知らせる (setTrace())
    struct Seen {
        int count = 0, id = -1;
        uint8_t cdb[10] = {};
        uint8_t status = 0xff;
    } seen;
    r.s.setTrace(
        [](void* ctx, int id, const uint8_t* cdb, uint8_t status) {
            Seen* q = (Seen*)ctx;
            q->count++;
            q->id = id;
            memcpy(q->cdb, cdb, 10);
            q->status = status;
        },
        &seen);
    CHECK_EQ(r.run({0x28, 0, 0, 0, 9, 9, 0, 0, 1, 0}, kBlockSize), 0x02);
    CHECK_EQ(seen.count, 1);
    CHECK_EQ(seen.id, 2);
    CHECK_EQ(seen.cdb[0], 0x28);
    CHECK_EQ(seen.cdb[4], 9);
    CHECK_EQ(seen.status, 0x02);
}

// ---------------------------------------------------------------- バスのリセット、つなぎ直し

void testReset() {
    printf("scsi bus reset / control reset / attach\n");
    Rig r;
    // RST (SCMD の bit4): 転送の途中でもバスフリーになり、Reset Condition。出している間は SSTS の bit3 が 1。
    // この割り込みは、SCTL の bit0 が 0 でも出る
    CHECK_EQ(r.run({0x28, 0, 0, 0, 1, 0x2b, 0, 0, 2, 0}), 0x02); // センスデータを残す
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    const uint8_t rd[6] = {0x08, 0, 0, 4, 1, 0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, rd, 6), Spc::INTS_COMPLETE);
    r.s.write(Spc::SCTL, Spc::SCTL_ARBITRATION);
    r.s.write(Spc::SCMD, Spc::SCMD_RST);
    CHECK_EQ(r.s.read(Spc::INTS), Spc::INTS_RESET);
    CHECK_EQ(r.s.irq(), true);
    CHECK_EQ(r.s.read(Spc::SSTS) & Spc::SSTS_RST, Spc::SSTS_RST);
    CHECK_EQ(r.s.read(Spc::PSNS), 0);
    r.s.write(Spc::SCMD, 0);
    CHECK_EQ(r.s.read(Spc::SSTS) & Spc::SSTS_RST, 0);
    r.s.write(Spc::INTS, Spc::INTS_RESET);
    CHECK_EQ(r.s.irq(), false);
    CHECK_EQ(r.s.read(Spc::BDID), 0x80); // BDID は変わらない
    r.s.write(Spc::SCTL, Spc::SCTL_ARBITRATION | Spc::SCTL_INT);
    CHECK_EQ(r.sense(), 0x000000); // 装置のセンスデータも消える

    // 転送の制御回路のリセット (SCTL の bit6): ハード転送をやめる。バスとの結合はそのまま
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    r.s.write(Spc::PCTL, Spc::PH_COMMAND);
    r.tc(6);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    r.s.write(Spc::DREG, 0);
    r.s.write(Spc::SCTL, Spc::SCTL_CONTROL_RESET | Spc::SCTL_ARBITRATION | Spc::SCTL_INT);
    r.s.write(Spc::SCTL, Spc::SCTL_ARBITRATION | Spc::SCTL_INT);
    CHECK_EQ(r.s.read(Spc::SSTS) & 0xf0, kReqWaiting);
    CHECK_EQ(r.phase(), Spc::PH_COMMAND);
    for (int i = 0; i < 5; i++) r.manualOut(0);
    CHECK_EQ(r.end(), 0x00);

    // コマンドの途中の装置を外すと、コマンドは捨てる。外した ID は答えない
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    r.s.attach(2, nullptr);
    CHECK_EQ(r.s.read(Spc::PSNS), 0);
    CHECK_EQ(r.s.image(2) == nullptr, true);
    CHECK_EQ(r.select(2), Spc::INTS_TIMEOUT);
    r.s.write(Spc::TEMP, 0);
    // 読み込んでいないイメージは、つながない
    x68k::HardDiskImage empty;
    r.s.attach(3, &empty);
    CHECK_EQ(r.s.image(3) == nullptr, true);
    // ID ごとに別の装置
    r.s.attach(0, &r.img);
    CHECK_EQ(r.run({0x00, 0, 0, 0, 0, 0}, 0, nullptr, 0), 0);
    CHECK_EQ(r.s.isCdrom(0), false);
    CHECK_EQ(r.s.cd() == nullptr, true);
}

// ---------------------------------------------------------------- DMA 転送

void testDma() {
    printf("dma: data request and block read\n");
    Rig r;
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    const uint8_t rd[6] = {0x08, 0, 0, 4, 2, 0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, rd, 6), Spc::INTS_COMPLETE);
    CHECK_EQ(r.s.dataReady(), false); // ハード転送をしていない
    static uint8_t buf[2 * kBlockSize];
    CHECK_EQ(r.s.readData(buf, 100, false), 0);
    // プログラム転送 (SCMD の bit2 = 1) では転送要求を出さない
    r.s.write(Spc::PCTL, Spc::PH_DATA_IN);
    r.tc(2 * kBlockSize);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    CHECK_EQ(r.s.dataReady(), false);
    CHECK_EQ(r.s.readData(buf, 100, true), 0);
    // 転送要求を確かめない読み方では、プログラム転送でも写す (DREG を続けて読むのと同じ)
    CHECK_EQ(r.s.readData(buf, 3, false), 3);
    buf[3] = r.s.read(Spc::DREG);
    CHECK_EQ(r.s.readData(buf + 4, 2000, false), 2 * (int)kBlockSize - 4);
    CHECK_EQ(memcmp(buf, g_disk + 4 * kBlockSize, sizeof buf), 0);
    CHECK_EQ(r.ints(), Spc::INTS_COMPLETE);
    CHECK_EQ(r.end(), 0x00);

    // DMA 転送 (bit2 = 0): FIFO にデータがあれば転送要求が出る
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    CHECK_EQ(r.out(Spc::PH_COMMAND, rd, 6), Spc::INTS_COMPLETE);
    r.s.write(Spc::PCTL, Spc::PH_DATA_IN);
    r.tc(2 * kBlockSize);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER);
    CHECK_EQ(r.s.dataReady(), true);
    memset(buf, 0, sizeof buf);
    CHECK_EQ(r.s.readData(buf, 700, true), 700);
    CHECK_EQ(r.tc() <= 2 * kBlockSize - 700, true);
    CHECK_EQ(r.s.dataReady(), true);
    CHECK_EQ(r.s.readData(buf + 700, 2000, true), 2 * (int)kBlockSize - 700);
    CHECK_EQ(memcmp(buf, g_disk + 4 * kBlockSize, sizeof buf), 0);
    CHECK_EQ(r.s.dataReady(), false);
    CHECK_EQ(r.ints(), Spc::INTS_COMPLETE);
    CHECK_EQ(r.end(), 0x00);

    // 書き込みの DMA 転送: FIFO が空いていれば転送要求が出る
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    const uint8_t wr[6] = {0x0a, 0, 0, 9, 1, 0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, wr, 6), Spc::INTS_COMPLETE);
    r.s.write(Spc::PCTL, Spc::PH_DATA_OUT);
    r.tc(kBlockSize);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER);
    for (uint32_t i = 0; i < kBlockSize; i++) {
        CHECK_EQ(r.s.dataReady(), true);
        r.s.write(Spc::DREG, (uint8_t)(i * 3));
    }
    CHECK_EQ(r.s.dataReady(), false);
    CHECK_EQ(r.ints(), Spc::INTS_COMPLETE);
    CHECK_EQ(r.end(), 0x00);
    CHECK_EQ(g_disk[9 * kBlockSize + 100], (uint8_t)(100 * 3));
}

// ---------------------------------------------------------------- 使うチャンクだけを読むイメージ

void testCached() {
    printf("cached image: the fifo waits for the chunks\n");
    const uint32_t CHUNK = x68k::DiskCache::CHUNK;
    static uint8_t file[8 * 16384];
    for (uint32_t i = 0; i < sizeof file; i++) file[i] = pattern(i * 5);
    static x68k::DiskCache cache;
    CHECK_EQ(cache.init(sizeof file, 4), true);
    Rig r;
    static HardDiskImage img; // チャンクのキャッシュを通すディスク
    CHECK_EQ(img.loadCached(&cache, sizeof file, kBlockSize, false), true);
    r.s.attach(1, &img);
    auto serve = [&]() {
        unsigned slot;
        uint32_t c, len;
        uint8_t* buf;
        int n = 0;
        while (cache.nextLoad(slot, c, buf, len)) {
            memcpy(buf, file + (size_t)c * CHUNK, len);
            cache.loaded(slot, true);
            n++;
        }
        return n;
    };
    // WRITE(6): ブロック 31 から 2 ブロック (チャンクの境目 16384 = ブロック 32 をまたぐ)。
    // チャンクが読めるまでターゲットは REQ を出さず、送ったバイトは FIFO にたまる (8 バイトでいっぱい)
    CHECK_EQ(r.select(1), Spc::INTS_COMPLETE);
    const uint8_t wr[6] = {0x0a, 0, 0, 31, 2, 0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, wr, 6), Spc::INTS_COMPLETE);
    CHECK_EQ(r.s.read(Spc::PSNS), Spc::PSNS_BSY | Spc::PH_DATA_OUT);
    r.s.write(Spc::PCTL, Spc::PH_DATA_OUT);
    r.tc(2 * kBlockSize);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    int waits = 0;
    for (uint32_t i = 0; i < 2 * kBlockSize;) {
        if (r.s.read(Spc::SSTS) & Spc::SSTS_FULL) {
            waits++;
            if (serve() == 0) {
                printf("  no chunk was requested at byte %u\n", i);
                failures++;
                break;
            }
            continue;
        }
        r.s.write(Spc::DREG, (uint8_t)(i * 11 + 1));
        i++;
    }
    CHECK_EQ(waits > 0, true);
    CHECK_EQ(r.ints(), Spc::INTS_COMPLETE);
    CHECK_EQ(r.end(), 0x00);
    // 読み直して確かめる
    CHECK_EQ(r.select(1), Spc::INTS_COMPLETE);
    const uint8_t rd[6] = {0x08, 0, 0, 31, 2, 0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, rd, 6), Spc::INTS_COMPLETE);
    r.s.write(Spc::PCTL, Spc::PH_DATA_IN);
    r.tc(2 * kBlockSize);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    int diff = 0;
    for (uint32_t i = 0; i < 2 * kBlockSize;) {
        if (r.s.read(Spc::SSTS) & Spc::SSTS_EMPTY) {
            if (serve() == 0) {
                failures++;
                break;
            }
            continue;
        }
        diff += r.s.read(Spc::DREG) != (uint8_t)(i * 11 + 1);
        i++;
    }
    CHECK_EQ(diff, 0);
    r.ints();
    CHECK_EQ(r.end(), 0x00);

    // チャンクの読み込みを待っている間 (ターゲットが REQ を出さない) でも、転送カウンタに 0 を書けば転送は終わる
    static x68k::DiskCache cache2;
    CHECK_EQ(cache2.init(sizeof file, 4), true);
    x68k::HardDiskImage img2;
    CHECK_EQ(img2.loadCached(&cache2, sizeof file, kBlockSize, false), true);
    r.s.attach(1, &img2);
    CHECK_EQ(r.select(1), Spc::INTS_COMPLETE);
    CHECK_EQ(r.out(Spc::PH_COMMAND, rd, 6), Spc::INTS_COMPLETE);
    r.s.write(Spc::PCTL, Spc::PH_DATA_IN);
    r.tc(2 * kBlockSize);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    CHECK_EQ(r.s.read(Spc::SSTS) & (Spc::SSTS_BUSY | Spc::SSTS_EMPTY), Spc::SSTS_BUSY | Spc::SSTS_EMPTY);
    CHECK_EQ(r.s.read(Spc::INTS), 0);
    r.tc(0);
    CHECK_EQ(r.s.read(Spc::SSTS) & Spc::SSTS_BUSY, 0);
    CHECK_EQ(r.ints(), Spc::INTS_COMPLETE);
    r.init();
    r.s.attach(1, nullptr);
}

// ---------------------------------------------------------------- ステート

void testState() {
    printf("state: in the middle of a transfer\n");
    Rig r;
    static uint8_t st[8192];
    auto save = [&](Spc& s) {
        x68k::StateIO io(x68k::StateIO::SAVE, st, sizeof st);
        s.state(io);
        CHECK_EQ(io.ok(), true);
        return io.pos();
    };
    auto load = [&](Spc& s, size_t n) {
        x68k::StateIO check(x68k::StateIO::CHECK, st, n);
        s.state(check);
        if (!check.ok() || check.pos() != n) return false;
        x68k::StateIO io(x68k::StateIO::LOAD, st, n);
        s.state(io);
        return io.ok();
    };
    // READ の 100 バイト目 (FIFO に 8 バイト入っている) で保存し、リセットしてから読んで、続きを受け取る
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    const uint8_t rd[6] = {0x08, 0, 0, 4, 2, 0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, rd, 6), Spc::INTS_COMPLETE);
    r.s.write(Spc::PCTL, Spc::PH_DATA_IN);
    r.tc(2 * kBlockSize);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    for (int i = 0; i < 100; i++) r.s.read(Spc::DREG);
    size_t n = save(r.s);
    r.init();
    CHECK_EQ(r.s.read(Spc::PSNS), 0);
    CHECK_EQ(load(r.s, n), true);
    int diff = 0;
    for (uint32_t i = 100; i < 2 * kBlockSize; i++) diff += r.s.read(Spc::DREG) != pattern(4 * kBlockSize + i);
    CHECK_EQ(diff, 0);
    CHECK_EQ(r.ints(), Spc::INTS_COMPLETE);
    CHECK_EQ(r.end(), 0x00);
    // WRITE の途中
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    const uint8_t wr[6] = {0x0a, 0, 0, 9, 1, 0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, wr, 6), Spc::INTS_COMPLETE);
    r.s.write(Spc::PCTL, Spc::PH_DATA_OUT);
    r.tc(kBlockSize);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    for (uint32_t i = 0; i < 200; i++) r.s.write(Spc::DREG, (uint8_t)(i + 1));
    n = save(r.s);
    r.init();
    CHECK_EQ(load(r.s, n), true);
    for (uint32_t i = 200; i < kBlockSize; i++) r.s.write(Spc::DREG, (uint8_t)(i + 1));
    CHECK_EQ(r.ints(), Spc::INTS_COMPLETE);
    CHECK_EQ(r.end(), 0x00);
    diff = 0;
    for (uint32_t i = 0; i < kBlockSize; i++) diff += g_disk[9 * kBlockSize + i] != (uint8_t)(i + 1);
    CHECK_EQ(diff, 0);
    // 装置をつないでいない SPC に読むと、コマンドを捨ててバスフリーになる (レジスタは戻る)
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    CHECK_EQ(r.out(Spc::PH_COMMAND, rd, 6), Spc::INTS_COMPLETE);
    n = save(r.s);
    Spc other;
    other.reset();
    CHECK_EQ(load(other, n), true);
    CHECK_EQ(other.read(Spc::PSNS), 0);
    CHECK_EQ(other.read(Spc::BDID), 0x80);
    // 壊れた値 (FIFO の数が 8 を超える) は読まない: 区切り 4 バイト、BDID から TEMP の 7 バイトの次が FIFO の数
    st[11] = 9;
    CHECK_EQ(load(r.s, n), false);

    // READ の途中のステートで、イメージの中のセクタの並び (CD-ROM の 2352 バイト / セクタなど) が、
    // CD-ROM の読み出しが作らない値: 読むが、コマンドを捨ててバスフリーにする。
    // 並びは、センスデータ (32) と応答のバッファ (256) のあとの、位置 8、セクタの大きさ 4、ユーザデータの大きさ 4、
    // イメージの大きさ 8、装置の種類 1、CD-ROM 装置があるか 1
    r.init();
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    CHECK_EQ(r.out(Spc::PH_COMMAND, rd, 6), Spc::INTS_COMPLETE);
    n = save(r.s);
    CHECK_EQ(n, 380);
    const size_t kBase = 354, kStride = 362, kPayload = 366, kImage = 370;
    auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; i++) st[at + (size_t)i] = (uint8_t)(v >> (8 * i));
    };
    static const uint32_t kBad[][2] = {{0x10001, 0x10000}, {2352, 0}, {0, 2048}, {2048, 2048}, {2352, 2352}, {0x12930, 2048}};
    for (const uint32_t* v : kBad) {
        put32(kStride, v[0]);
        put32(kPayload, v[1]);
        CHECK_EQ(load(r.s, n), true);
        CHECK_EQ(r.s.read(Spc::PSNS), 0);
        r.s.read(Spc::DREG);
    }
    put32(kStride, 0);
    put32(kPayload, 0);
    CHECK_EQ(load(r.s, n), true); // 書き換えなければ続きができる
    CHECK_EQ(r.phase(), Spc::PH_DATA_IN);
    r.init();

    // イメージの大きさが保存したときと違うか、転送の範囲 (位置から 2 ブロック = 1024 バイト) がイメージからはみ出す
    // ステートは、読むが、コマンドを捨ててバスフリーにする (イメージの外を読み書きしない)
    {
        static uint8_t orig[sizeof st];
        memcpy(orig, st, n);
        auto put64 = [&](size_t at, uint64_t v) {
            for (int i = 0; i < 8; i++) st[at + (size_t)i] = (uint8_t)(v >> (8 * i));
        };
        const uint64_t kBytes = (uint64_t)kBlocks * kBlockSize;
        const struct {
            uint64_t base, image;
            bool keep;
        } kCases[] = {
            {4 * kBlockSize, kBytes, true},            // 保存したまま
            {4 * kBlockSize, kBytes + 1, false},       // イメージの大きさが違う
            {4 * kBlockSize, kBytes - kBlockSize, false},
            {kBytes - 1024, kBytes, true},             // ちょうど最後まで
            {kBytes - 1023, kBytes, false},            // 最後の 1 バイトがはみ出す
            {kBytes, kBytes, false},
            {kBytes + 4096, kBytes, false},
            {0xffffffffffffff00ull, kBytes, false},    // 足すとあふれる
            {0xfffffffffffffc00ull, 0xffffffffffffffffull, false},
        };
        for (const auto& c : kCases) {
            memcpy(st, orig, n);
            put64(kBase, c.base);
            put64(kImage, c.image);
            r.init();
            CHECK_EQ(load(r.s, n), true);
            CHECK_EQ(r.s.read(Spc::PSNS) != 0, c.keep);
            if (c.keep) {
                CHECK_EQ(r.phase(), Spc::PH_DATA_IN);
                r.in(Spc::PH_DATA_IN, 1024);
                CHECK_EQ(r.got, 1024);
                CHECK_EQ(r.buf[0], pattern((uint32_t)c.base));
                CHECK_EQ(r.buf[1023], pattern((uint32_t)c.base + 1023));
                CHECK_EQ(r.end(), 0x00);
            } else {
                r.s.read(Spc::DREG); // 読んでも落ちない
            }
        }
        // 大きさの違うイメージをつないだ SPC に読んでも、コマンドを捨てる
        memcpy(st, orig, n);
        static uint8_t small[8 * kBlockSize];
        x68k::HardDiskImage smallImg;
        CHECK_EQ(smallImg.load(small, sizeof small, kBlockSize, false), true);
        Spc other2;
        other2.attach(2, &smallImg);
        other2.reset();
        CHECK_EQ(load(other2, n), true);
        CHECK_EQ(other2.read(Spc::PSNS), 0);
        r.init();
    }

    // 応答のバッファ (256 バイト) から送るデータインの長さは、バッファに収まること (収まらないステートは読まない)。
    // 並びの 58 バイト目からが長さ、62 バイト目からが位置
    {
        CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
        const uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0};
        CHECK_EQ(r.out(Spc::PH_COMMAND, inq, 6), Spc::INTS_COMPLETE);
        CHECK_EQ(r.phase(), Spc::PH_DATA_IN);
        n = save(r.s);
        const size_t kLen = 58;
        CHECK_EQ(st[kLen] | st[kLen + 1] << 8 | st[kLen + 2] << 16 | st[kLen + 3] << 24, 36);
        for (uint32_t len : {256u, 257u, 0xffffu, 0x10000u, 0xffffffffu}) {
            put32(kLen, len);
            r.init();
            CHECK_EQ(load(r.s, n), len <= 256);
        }
        put32(kLen, 36);
        r.init();
        CHECK_EQ(load(r.s, n), true);
        r.in(Spc::PH_DATA_IN, 36);
        CHECK_EQ(r.got, 36);
        CHECK_EQ(r.end(), 0x00);
        r.init();
    }

    // メッセージアウトの途中 (拡張メッセージの中身を受け取っている) の残りの数は 257 まで (並びの 49 バイト目から 16 ビット)
    {
        CHECK_EQ(r.select(2, true), Spc::INTS_COMPLETE);
        r.manualOut(0x01);
        r.manualOut(0x00); // 256 バイト
        n = save(r.s);
        const size_t kMsgExt = 49;
        CHECK_EQ(st[kMsgExt] | st[kMsgExt + 1] << 8, 257);
        st[kMsgExt] = 0x02; // 258
        r.init();
        CHECK_EQ(load(r.s, n), false);
        st[kMsgExt + 1] = 0xff;
        CHECK_EQ(load(r.s, n), false);
        st[kMsgExt] = 0x01;
        st[kMsgExt + 1] = 0x01;
        CHECK_EQ(load(r.s, n), true);
        for (int i = 0; i < 255; i++) r.manualOut(0x06);
        CHECK_EQ(r.phase(), Spc::PH_MSG_OUT);
        r.s.write(Spc::SCMD, Spc::CMD_RESET_ATN);
        r.manualOut(0x06);
        CHECK_EQ(r.phase(), Spc::PH_MSG_IN);
        r.init();
    }

    // バスの上の転送が終わり、FIFO を読み終えたら出す割り込み (ステータスを 1 バイト受け取ったところ) も保存する
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    const uint8_t tur[6] = {0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, tur, 6), Spc::INTS_COMPLETE);
    r.s.write(Spc::PCTL, Spc::PH_STATUS);
    r.tc(1);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    CHECK_EQ(r.s.read(Spc::INTS), 0); // まだ FIFO に残っている
    n = save(r.s);
    r.init();
    CHECK_EQ(load(r.s, n), true);
    CHECK_EQ(r.s.read(Spc::INTS), 0);
    CHECK_EQ(r.s.read(Spc::DREG), 0x00);
    CHECK_EQ(r.ints(), Spc::INTS_COMPLETE);
    CHECK_EQ(r.manualIn(), 0x00); // COMMAND COMPLETE
    CHECK_EQ(r.s.read(Spc::PSNS), 0);
    r.init();

    // 版 11 までの並び (前の実装のもの) は、レジスタだけを戻し、バスと転送の状態は読み飛ばす。
    // 長さは、版 4 から 7 が 473 バイト、版 8 からは CD-ROM 装置の状態 (1050 バイト) を含めて 1531 バイト
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE); // 結合している状態で書く
    r.s.write(Spc::BDID, 5);
    r.s.write(Spc::PCTL, Spc::PCTL_BUSFREE_INT | Spc::PH_STATUS);
    r.s.write(Spc::TEMP, 0x5a);
    r.tc(0x123456);
    for (uint32_t version : {3u, 7u, 11u}) {
        x68k::StateIO out(x68k::StateIO::SAVE, st, sizeof st, version);
        r.s.state(out);
        CHECK_EQ(out.ok(), true);
        CHECK_EQ(out.pos(), version < 4 ? 472u : version < 8 ? 473u : 1531u);
        Spc fresh;
        fresh.attach(2, &r.img);
        fresh.reset();
        x68k::StateIO in(x68k::StateIO::LOAD, st, out.pos(), version);
        fresh.state(in);
        CHECK_EQ(in.ok(), true);
        CHECK_EQ(in.pos(), out.pos());
        CHECK_EQ(fresh.read(Spc::BDID), 0x20);
        CHECK_EQ(fresh.read(Spc::SCTL), Spc::SCTL_ARBITRATION | Spc::SCTL_INT);
        CHECK_EQ(fresh.read(Spc::PCTL), Spc::PCTL_BUSFREE_INT | Spc::PH_STATUS);
        CHECK_EQ(fresh.read(Spc::TEMP), 0x5a);
        CHECK_EQ(fresh.read(Spc::TCH), 0x12);
        CHECK_EQ(fresh.read(Spc::TCM), 0x34);
        CHECK_EQ(fresh.read(Spc::TCL), 0x56);
        CHECK_EQ(fresh.read(Spc::PSNS), 0); // バスフリーから
        CHECK_EQ(fresh.read(Spc::INTS), 0);
        // 前の実装がターゲットと結合している途中で保存したもの (区切りから 23 バイト目からの 32 ビットが、-1 でなく
        // ターゲットの ID) は読まない
        CHECK_EQ(st[23] & st[24] & st[25] & st[26], 0xff);
        st[23] = 2;
        st[24] = st[25] = st[26] = 0;
        x68k::StateIO busy(x68k::StateIO::CHECK, st, out.pos(), version);
        fresh.state(busy);
        CHECK_EQ(busy.ok(), false);
        CHECK_EQ(busy.why() != nullptr, true);
    }
}

// 転送の途中の装置を外す、媒体を替える: FIFO に残っているその装置のデータと、保留の割り込みも捨てる
void testDetach() {
    printf("detach in the middle of a transfer: the fifo is emptied\n");
    Rig r;
    const uint8_t rd[6] = {0x08, 0, 0, 3, 2, 0};
    // プログラム転送: FIFO に 8 バイト入ったところで外す
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    CHECK_EQ(r.out(Spc::PH_COMMAND, rd, 6), Spc::INTS_COMPLETE);
    r.s.write(Spc::PCTL, Spc::PH_DATA_IN);
    r.tc(2 * kBlockSize);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    CHECK_EQ(r.s.read(Spc::SSTS) & Spc::SSTS_FULL, Spc::SSTS_FULL);
    r.s.attach(2, nullptr);
    CHECK_EQ(r.s.read(Spc::PSNS), 0);
    CHECK_EQ(r.s.read(Spc::SSTS) & (Spc::SSTS_INIT | Spc::SSTS_BUSY | Spc::SSTS_FULL | Spc::SSTS_EMPTY), Spc::SSTS_EMPTY);
    CHECK_EQ(r.s.read(Spc::INTS), 0);
    // DMA 転送: DMAC にデータを渡せる状態で外すと、渡せなくなる
    r.s.attach(2, &r.img);
    r.init();
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    CHECK_EQ(r.out(Spc::PH_COMMAND, rd, 6), Spc::INTS_COMPLETE);
    r.s.write(Spc::PCTL, Spc::PH_DATA_IN);
    r.tc(2 * kBlockSize);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER);
    CHECK_EQ(r.s.dataReady(), true);
    r.s.attach(2, nullptr);
    CHECK_EQ(r.s.dataReady(), false);
    uint8_t tmp[16];
    CHECK_EQ(r.s.readData(tmp, sizeof tmp, true), 0);
    // ステータスを 1 バイト受け取って FIFO に残っている (割り込みは読み終えてから) ところで外すと、割り込みも出ない
    r.s.attach(2, &r.img);
    r.init();
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    const uint8_t tur[6] = {0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, tur, 6), Spc::INTS_COMPLETE);
    r.s.write(Spc::PCTL, Spc::PH_STATUS);
    r.tc(1);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    CHECK_EQ(r.s.read(Spc::SSTS) & Spc::SSTS_EMPTY, 0);
    r.s.attach(2, nullptr);
    CHECK_EQ(r.s.read(Spc::SSTS) & Spc::SSTS_EMPTY, Spc::SSTS_EMPTY);
    r.s.read(Spc::DREG);
    CHECK_EQ(r.s.read(Spc::INTS), 0);

    // CD-ROM 装置の媒体を替える (changeCd()) ときも同じ
    static uint8_t iso[16 * 2048];
    for (uint32_t i = 0; i < sizeof iso; i++) iso[i] = pattern(i * 9);
    x68k::HardDiskImage cdImg;
    CHECK_EQ(cdImg.load(iso, sizeof iso, 2048, true), true);
    r.s.attach(3, &cdImg, true);
    r.init();
    CHECK_EQ(r.select(3), Spc::INTS_COMPLETE);
    const uint8_t rdCd[6] = {0x08, 0, 0, 1, 1, 0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, rdCd, 6), Spc::INTS_COMPLETE);
    CHECK_EQ(r.phase(), Spc::PH_DATA_IN);
    r.s.write(Spc::PCTL, Spc::PH_DATA_IN);
    r.tc(2048);
    r.s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    CHECK_EQ(r.s.read(Spc::DREG), pattern(2048 * 9));
    CHECK_EQ(r.s.read(Spc::SSTS) & Spc::SSTS_FULL, Spc::SSTS_FULL);
    CHECK_EQ(r.s.changeCd(nullptr, nullptr), true);
    CHECK_EQ(r.s.read(Spc::PSNS), 0);
    CHECK_EQ(r.s.read(Spc::SSTS) & (Spc::SSTS_INIT | Spc::SSTS_BUSY | Spc::SSTS_FULL | Spc::SSTS_EMPTY), Spc::SSTS_EMPTY);
    r.s.attach(3, nullptr);
}

// マニュアル転送で ACK を出している間に保存し、何も読み込んでいないキャッシュのイメージに戻す
// (ベアメタル版の大きなイメージ。戻した直後は、転送中のチャンクがまだ読み込まれていない)
void testStateAck() {
    printf("state: saved while ACK is asserted, restored onto an empty cache\n");
    const uint32_t CHUNK = x68k::DiskCache::CHUNK;
    static uint8_t file[8 * 16384];
    for (uint32_t i = 0; i < sizeof file; i++) file[i] = pattern(i * 3 + 1);
    static uint8_t st[8192];
    auto serve = [&](x68k::DiskCache& cache) {
        unsigned slot;
        uint32_t c, len;
        uint8_t* buf;
        int n = 0;
        while (cache.nextLoad(slot, c, buf, len)) {
            memcpy(buf, file + (size_t)c * CHUNK, len);
            cache.loaded(slot, true);
            n++;
        }
        return n;
    };
    auto save = [&](Spc& s) {
        x68k::StateIO io(x68k::StateIO::SAVE, st, sizeof st);
        s.state(io);
        CHECK_EQ(io.ok(), true);
        return io.pos();
    };
    auto load = [&](Spc& s, size_t n) {
        x68k::StateIO check(x68k::StateIO::CHECK, st, n);
        s.state(check);
        if (!check.ok() || check.pos() != n) return false;
        x68k::StateIO io(x68k::StateIO::LOAD, st, n);
        s.state(io);
        return io.ok();
    };
    static x68k::DiskCache cacheA, cacheB, cacheC;
    CHECK_EQ(cacheA.init(sizeof file, 4), true);
    CHECK_EQ(cacheB.init(sizeof file, 4), true);
    CHECK_EQ(cacheC.init(sizeof file, 4), true);
    x68k::HardDiskImage imgA, imgB, imgC;
    CHECK_EQ(imgA.loadCached(&cacheA, sizeof file, kBlockSize, false), true);
    CHECK_EQ(imgB.loadCached(&cacheB, sizeof file, kBlockSize, false), true);
    CHECK_EQ(imgC.loadCached(&cacheC, sizeof file, kBlockSize, false), true);
    Rig r;
    r.s.attach(1, &imgA);
    const uint32_t base = 40 * kBlockSize;

    // READ(6) のブロック 40 から 1 ブロック: 3 バイト目で Set ACK を出したところで保存する
    CHECK_EQ(r.select(1), Spc::INTS_COMPLETE);
    const uint8_t rd[6] = {0x08, 0, 0, 40, 1, 0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, rd, 6), Spc::INTS_COMPLETE);
    CHECK_EQ(serve(cacheA) > 0, true);
    r.s.write(Spc::PCTL, Spc::PH_DATA_IN);
    CHECK_EQ(r.manualIn(), file[base]);
    CHECK_EQ(r.manualIn(), file[base + 1]);
    CHECK_EQ(r.s.read(Spc::PSNS) & Spc::PSNS_REQ, Spc::PSNS_REQ);
    r.s.write(Spc::SCMD, Spc::CMD_SET_ACK);
    CHECK_EQ(r.s.read(Spc::TEMP), file[base + 2]);
    size_t n = save(r.s);
    {
        Spc b;
        b.attach(1, &imgB);
        b.reset();
        CHECK_EQ(load(b, n), true);
        // ACK を出している間のバイトは、イメージが読み込まれていなくても読める
        CHECK_EQ(b.read(Spc::TEMP), file[base + 2]);
        CHECK_EQ(b.read(Spc::PSNS) & Spc::PSNS_REQ, 0);
        b.write(Spc::SCMD, Spc::CMD_RESET_ACK);
        CHECK_EQ(b.read(Spc::PSNS) & Spc::PSNS_REQ, 0); // チャンクが読み込まれるまで、次の REQ は出ない
        CHECK_EQ(b.read(Spc::TEMP), b.read(Spc::TEMP)); // REQ がない間の TEMP は、書いた値 (読んでも落ちない)
        CHECK_EQ(serve(cacheB) > 0, true);
        CHECK_EQ(b.read(Spc::PSNS) & Spc::PSNS_REQ, Spc::PSNS_REQ);
        CHECK_EQ(b.read(Spc::TEMP), file[base + 3]);
    }
    r.init();

    // WRITE(6) のブロック 41 から 1 ブロック: 2 バイト目で Set ACK を出したところで保存する。
    // バイトは Set ACK のときにイメージに入っているので、戻したあとの Reset ACK はイメージに触らない
    CHECK_EQ(r.select(1), Spc::INTS_COMPLETE);
    const uint8_t wr[6] = {0x0a, 0, 0, 41, 1, 0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, wr, 6), Spc::INTS_COMPLETE);
    serve(cacheA); // READ と同じチャンクなので、もう読み込まれている
    r.s.write(Spc::PCTL, Spc::PH_DATA_OUT);
    r.manualOut(0xa1);
    CHECK_EQ(r.s.read(Spc::PSNS) & Spc::PSNS_REQ, Spc::PSNS_REQ);
    r.s.write(Spc::TEMP, 0xb2);
    r.s.write(Spc::SCMD, Spc::CMD_SET_ACK);
    n = save(r.s);
    {
        Spc c;
        c.attach(1, &imgC);
        c.reset();
        CHECK_EQ(load(c, n), true);
        c.write(Spc::SCMD, Spc::CMD_RESET_ACK);
        CHECK_EQ(c.read(Spc::PSNS) & Spc::PSNS_REQ, 0);
        CHECK_EQ(serve(cacheC) > 0, true);
        CHECK_EQ(c.read(Spc::PSNS), Spc::PSNS_REQ | Spc::PSNS_BSY | Spc::PH_DATA_OUT);
        // 3 バイト目から続ける
        c.write(Spc::TEMP, 0xc3);
        c.write(Spc::SCMD, Spc::CMD_SET_ACK);
        c.write(Spc::SCMD, Spc::CMD_RESET_ACK);
        uint32_t got = 0;
        bool failed = false;
        const uint8_t* p = imgC.window(41 * kBlockSize, 3, got, failed);
        CHECK_EQ(p != nullptr && got >= 3, true);
        if (p && got >= 3) CHECK_EQ(p[2], 0xc3);
    }
    // 保存した側は、そのまま続けられる
    r.s.write(Spc::SCMD, Spc::CMD_RESET_ACK);
    {
        uint32_t got = 0;
        bool failed = false;
        const uint8_t* p = imgA.window(41 * kBlockSize, 2, got, failed);
        CHECK_EQ(p != nullptr && got >= 2, true);
        if (p && got >= 2) {
            CHECK_EQ(p[0], 0xa1);
            CHECK_EQ(p[1], 0xb2);
        }
    }
    r.init();
    r.s.attach(1, nullptr);

    // MODE SELECT(10) のパラメータが 512 バイト (256 バイトを超えた分は捨てる): 300 バイト目で保存して、戻せる
    CHECK_EQ(r.select(2), Spc::INTS_COMPLETE);
    const uint8_t ms[10] = {0x55, 0x10, 0, 0, 0, 0, 0, 2, 0, 0};
    CHECK_EQ(r.out(Spc::PH_COMMAND, ms, 10), Spc::INTS_COMPLETE);
    CHECK_EQ(r.phase(), Spc::PH_DATA_OUT);
    r.s.write(Spc::PCTL, Spc::PH_DATA_OUT);
    for (int i = 0; i < 300; i++) r.manualOut(0);
    n = save(r.s);
    r.init();
    CHECK_EQ(load(r.s, n), true);
    CHECK_EQ(r.s.read(Spc::PSNS), Spc::PSNS_REQ | Spc::PSNS_BSY | Spc::PH_DATA_OUT);
    for (int i = 300; i < 512; i++) r.manualOut(0);
    CHECK_EQ(r.end(), 0x00);
}

// ---------------------------------------------------------------- Machine の上で: 番地、ROM の置き方、割り込み

// 中身のない IPL ROM (リセットのあと、その場で回り続ける) の Machine
x68k::Machine* makeMachine() {
    static uint8_t ipl[x68k::Machine::IPL_SIZE];
    memset(ipl, 0xff, sizeof ipl);
    const uint8_t vec[] = {0x00, 0x00, 0x20, 0x00, 0x00, 0xff, 0x00, 0x10, 0x60, 0xfe};
    memcpy(ipl + 0x10000, vec, 8);
    memcpy(ipl + 0x10010, vec + 8, 2);
    x68k::Machine* m = new x68k::Machine;
    if (!m->ok() || !m->setIplRom(ipl, sizeof ipl)) return nullptr;
    return m;
}

// 書き直す前の実装のビルドが書いたステート (old_scsi_state_data.h) を読む: SASI と SPC のところを正しい長さだけ
// 読み飛ばし (ずれていれば、後ろの装置の検査か全体の長さで落ちる)、SPC のレジスタと CD-ROM 装置の状態を戻す
void testOldStateData() {
    printf("machine: states written by the implementation before the rewrite\n");
    static uint8_t old[65536];
    static uint8_t iso[4 * 2048];
    static HardDiskImage img; // CD-ROM の媒体
    CHECK_EQ(img.load(iso, sizeof iso, 2048, true), true);
    struct Data {
        const uint8_t* rle;
        size_t rleSize, size;
        uint8_t versions[4];
    };
    const Data data[] = {
        {kOldScsiStateV3, sizeof kOldScsiStateV3, 48562, {2, 3, 0, 0}},
        {kOldScsiStateV4, sizeof kOldScsiStateV4, 48564, {4, 0, 0, 0}},
        {kOldScsiStateV5, sizeof kOldScsiStateV5, 48572, {5, 0, 0, 0}},
        {kOldScsiStateV6, sizeof kOldScsiStateV6, 48580, {6, 7, 0, 0}},
        {kOldScsiStateV11, sizeof kOldScsiStateV11, 50696, {8, 9, 10, 11}},
    };
    for (const Data& d : data) {
        size_t size = 0;
        for (size_t i = 0; i + 1 < d.rleSize; i += 2)
            for (int k = 0; k < d.rle[i] && size < sizeof old; k++) old[size++] = d.rle[i + 1];
        CHECK_EQ(size, d.size);
        for (uint8_t version : d.versions) {
            if (!version) continue;
            x68k::Machine* m = makeMachine();
            if (!m) {
                printf("  cannot make the machine\n");
                failures++;
                return;
            }
            m->attachScsi(6, &img, true);
            m->powerOn();
            m->run(1000);
            old[8] = version; // 同じデータで、版の番号だけが違う
            const char* err = nullptr;
            CHECK_EQ(x68k::loadState(*m, old, size, err), true);
            if (err) printf("  version %d: %s\n", version, err);
            Spc& s = m->spc();
            CHECK_EQ(s.read(Spc::BDID), 0x20);
            CHECK_EQ(s.read(Spc::SCTL), Spc::SCTL_ARBITRATION | Spc::SCTL_INT);
            CHECK_EQ(s.read(Spc::PCTL), Spc::PCTL_BUSFREE_INT | Spc::PH_STATUS);
            CHECK_EQ(s.read(Spc::TEMP), 0x5a);
            CHECK_EQ(s.read(Spc::TCH), 0x12);
            CHECK_EQ(s.read(Spc::TCM), 0x34);
            CHECK_EQ(s.read(Spc::TCL), 0x56);
            CHECK_EQ(s.read(Spc::PSNS), 0);
            // CD-ROM 装置の状態は、版 8 から入っている
            CHECK_EQ(m->cdDrive()->prevented(), version >= 8);
            CHECK_EQ(m->cdDrive()->position(), version >= 8 ? 3u : 0u);
            m->run(100000); // そのまま動かせる
            // 壊したデータは読まない
            old[100] ^= 1;
            CHECK_EQ(x68k::loadState(*m, old, size, err), false);
            old[100] ^= 1;
            delete m;
        }
    }
}

// Machine の番地で SPC を読み書きする (base は reg 0 の番地。レジスタは奇数番地に 2 バイトおき)
struct Port {
    x68k::Machine& m;
    uint32_t base;
    uint8_t rd(int reg) { return m.read8(base + 2 * (uint32_t)reg, m68k::FC_SUPER_DATA); }
    void wr(int reg, uint8_t v) { m.write8(base + 2 * (uint32_t)reg, v, m68k::FC_SUPER_DATA); }
    // リセットを解いて ID 0 を選び、Command Complete を残す
    void selectId0() {
        wr(Spc::SCTL, Spc::SCTL_RESET);
        wr(Spc::BDID, 7);
        wr(Spc::SCTL, Spc::SCTL_ARBITRATION | Spc::SCTL_INT);
        wr(Spc::PCTL, 0);
        wr(Spc::TEMP, 0x81);
        wr(Spc::TCH, 0x0f);
        wr(Spc::SCMD, Spc::CMD_SELECT);
    }
};

// 外付け SCSI ボード (CZ-6BS1 相当): $EA0001-$EA001F の奇数番地に SPC、$EA0020-$EA1FFF に ROM。割り込みはレベル 2、ベクタ $F6
void testBoard() {
    printf("machine: scsi board address, rom placement, interrupt vector\n");
    static HardDiskImage img; // 外付け SCSI の ID 0
    fillDisk();
    constexpr auto fc = m68k::FC_SUPER_DATA; // 機械のバスはスーパーバイザのデータで読み書きする
    CHECK_EQ(img.load(g_disk, sizeof g_disk, kBlockSize), true);
    x68k::Machine* m = makeMachine();
    if (!m) {
        printf("  cannot make the machine\n");
        failures++;
        return;
    }
    m->powerOn();
    // ROM を読む (バスエラーの場所は -1)
    auto romAt = [&](uint32_t addr) {
        bool ok = false;
        const uint8_t v = m->peek8(addr, ok);
        return ok ? (int)v : -1;
    };
    // ボードをつながなければ、$EA0000 からはバスエラー
    CHECK_EQ(m->hasScsiEx(), false);
    CHECK_EQ(romAt(0xea0020), -1);
    CHECK_EQ(romAt(0xea1fff), -1);
    // ROM のファイルが $EA0000 からの 8192 バイト: そのまま置く
    static uint8_t rom[x68k::Machine::SCSI_EX_ROM_SIZE];
    for (uint32_t i = 0; i < sizeof rom; i++) rom[i] = (uint8_t)(i * 3 + 1);
    CHECK_EQ(m->setScsiExRom(rom, 0), false);
    CHECK_EQ(m->setScsiExRom(rom, sizeof rom + 1), false);
    CHECK_EQ(m->hasScsiEx(), false);
    CHECK_EQ(m->setScsiExRom(rom, sizeof rom), true);
    CHECK_EQ(m->hasScsiEx(), true);
    CHECK_EQ(romAt(0xea0020), rom[0x20]);
    CHECK_EQ(romAt(0xea1fff), rom[0x1fff]);
    CHECK_EQ(m->read8(0xea0020, fc), rom[0x20]);
    // $EA0020 からの 8160 バイト: $EA0020 から置く
    CHECK_EQ(m->setScsiExRom(rom, sizeof rom - 0x20), true);
    CHECK_EQ(romAt(0xea0020), rom[0]);
    CHECK_EQ(romAt(0xea1fff), rom[0x1fdf]);
    // ほかの大きさ: 'SCSIEX' が $EA0044 に来るように置く (ファイルの $24 にあれば $EA0020 から、なければ $EA0000 から)
    static uint8_t small[0x100];
    memset(small, 0x11, sizeof small);
    memcpy(small + 0x24, "SCSIEX", 6);
    CHECK_EQ(m->setScsiExRom(small, sizeof small), true);
    CHECK_EQ(romAt(0xea0044), 'S');
    CHECK_EQ(romAt(0xea0049), 'X');
    CHECK_EQ(romAt(0xea0120), 0xff); // ファイルの後ろは $FF
    memset(small, 0x22, sizeof small);
    memcpy(small + 0x44, "SCSIEX", 6);
    CHECK_EQ(m->setScsiExRom(small, sizeof small), true);
    CHECK_EQ(romAt(0xea0044), 'S');
    CHECK_EQ(romAt(0xea0020), 0x22);

    // SPC は奇数番地。偶数番地は $FF
    Port ex{*m, 0xea0001};
    CHECK_EQ(ex.rd(Spc::SCTL), 0x80); // リセットのあと
    CHECK_EQ(m->read8(0xea0002, fc), 0xff);
    ex.wr(Spc::BDID, 7);
    CHECK_EQ(ex.rd(Spc::BDID), 0x80);
    CHECK_EQ(m->spcEx().read(Spc::BDID), 0x80);
    CHECK_EQ(m->spc().read(Spc::BDID), 0x01); // 内蔵 SCSI の SPC は別のもの
    // 割り込み: レベル 2、ベクタ $F6。要因を消すまで続き、消すと応答するものがなくなる
    m->attachScsiEx(0, &img);
    CHECK_EQ(m->spcEx().image(0) == &img, true);
    ex.selectId0();
    CHECK_EQ(ex.rd(Spc::INTS), Spc::INTS_COMPLETE);
    CHECK_EQ(ex.rd(Spc::PSNS), Spc::PSNS_REQ | Spc::PSNS_BSY | Spc::PH_COMMAND);
    CHECK_EQ(m->interruptAck(2), 0xf6);
    CHECK_EQ(m->interruptAck(2), 0xf6);
    ex.wr(Spc::INTS, 0xff);
    CHECK_EQ(m->interruptAck(2) < 0, true);
    m->attachScsiEx(0, nullptr);
    delete m;

    // 内蔵 SCSI: SCSI の ROM を渡した機種では、$E96021-$E9603F の奇数番地に SPC。割り込みはレベル 1、ベクタ $6C
    m = makeMachine();
    if (!m) return;
    static uint8_t inRom[0x100];
    memset(inRom, 0xff, sizeof inRom);
    CHECK_EQ(m->setScsiRom(inRom, sizeof inRom), true);
    m->powerOn();
    Port in{*m, 0xe96021};
    CHECK_EQ(in.rd(Spc::SCTL), 0x80);
    CHECK_EQ(m->read8(0xe96022, fc), 0xff);
    CHECK_EQ(m->hasScsiEx(), false);
    m->attachScsi(0, &img);
    in.selectId0();
    CHECK_EQ(in.rd(Spc::INTS), Spc::INTS_COMPLETE);
    CHECK_EQ(m->interruptAck(1), 0x6c);
    in.wr(Spc::INTS, 0xff);
    CHECK_EQ(m->interruptAck(1) < 0, true);
    // SASI のポート ($E96001-$E96007) は、内蔵 SCSI の機種でもそのまま
    CHECK_EQ(m->read8(0xe96003, fc), 0x00);

    // DMAC のチャネル 1 で DREG ($E96035) を読み書きする (DMA 転送のハード転送。転送要求が出ている間だけ進む)。
    // READ(6): ブロック 4 から 2 ブロックを $010000 へ
    auto dma = [&](uint8_t ocr, uint32_t mar, uint16_t count) {
        const uint32_t b = 0xe84040;
        auto out8 = [&](uint32_t a, uint8_t v) { m->write8(a, v, fc); };
        out8(b + 0x00, 0xff); // CSR を消す
        out8(b + 0x04, 0x80); // DCR
        out8(b + 0x05, ocr);  // OCR: bit7 デバイスからメモリへ、8 ビット、外部要求
        out8(b + 0x06, 0x04); // SCR: メモリの番地を増やす
        out8(b + 0x0a, (uint8_t)(count >> 8));
        out8(b + 0x0b, (uint8_t)count);
        for (int i = 0; i < 4; i++) out8(b + 0x0c + (uint32_t)i, (uint8_t)(mar >> (24 - 8 * i)));
        const uint32_t dar = 0xe96021 + 2 * Spc::DREG;
        for (int i = 0; i < 4; i++) out8(b + 0x14 + (uint32_t)i, (uint8_t)(dar >> (24 - 8 * i)));
        out8(b + 0x29, 5);
        out8(b + 0x31, 5);
        out8(b + 0x07, 0x80); // CCR: 始める
    };
    auto command = [&](std::initializer_list<uint8_t> cdb, uint8_t phase, uint32_t n) {
        in.wr(Spc::INTS, 0xff);
        in.wr(Spc::PCTL, 0);
        in.wr(Spc::TEMP, 0x81);
        in.wr(Spc::TCH, 0x0f);
        in.wr(Spc::SCMD, Spc::CMD_SELECT);
        in.wr(Spc::INTS, 0xff);
        in.wr(Spc::PCTL, Spc::PH_COMMAND);
        in.wr(Spc::TCH, 0);
        in.wr(Spc::TCM, 0);
        in.wr(Spc::TCL, (uint8_t)cdb.size());
        in.wr(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
        for (uint8_t c : cdb) in.wr(Spc::DREG, c);
        in.wr(Spc::INTS, 0xff);
        in.wr(Spc::PCTL, phase);
        in.wr(Spc::TCH, (uint8_t)(n >> 16));
        in.wr(Spc::TCM, (uint8_t)(n >> 8));
        in.wr(Spc::TCL, (uint8_t)n);
    };
    auto finish = [&]() {
        CHECK_EQ(in.rd(Spc::INTS), Spc::INTS_COMPLETE);
        CHECK_EQ(m->read8(0xe84040, fc) & 0x80, 0x80); // DMAC の CSR: 転送が終わった
        in.wr(Spc::INTS, 0xff);
        CHECK_EQ(in.rd(Spc::PSNS) & 7, Spc::PH_STATUS);
        for (int i = 0; i < 2; i++) {
            CHECK_EQ(in.rd(Spc::TEMP), 0x00);
            in.wr(Spc::SCMD, Spc::CMD_SET_ACK);
            in.wr(Spc::SCMD, Spc::CMD_RESET_ACK);
        }
        CHECK_EQ(in.rd(Spc::PSNS), 0);
    };
    for (uint32_t i = 0; i < 2 * kBlockSize; i++) m->write8(0x010000 + i, 0xee, fc);
    command({0x08, 0, 0, 4, 2, 0}, Spc::PH_DATA_IN, 2 * kBlockSize);
    dma(0xb2, 0x010000, 2 * kBlockSize);
    m->run(2000);
    CHECK_EQ(m->read8(0x010000, fc), 0xee); // Transfer コマンドを出すまでは、転送要求が出ない
    in.wr(Spc::SCMD, Spc::CMD_TRANSFER);
    for (int i = 0; i < 20; i++) m->run(20000);
    int diff = 0;
    for (uint32_t i = 0; i < 2 * kBlockSize; i++) diff += m->read8(0x010000 + i, fc) != pattern(4 * kBlockSize + i);
    CHECK_EQ(diff, 0);
    finish();
    // WRITE(6): $010000 からの 1 ブロックをブロック 9 へ
    for (uint32_t i = 0; i < kBlockSize; i++) m->write8(0x010000 + i, (uint8_t)(i * 5 + 2), fc);
    command({0x0a, 0, 0, 9, 1, 0}, Spc::PH_DATA_OUT, kBlockSize);
    in.wr(Spc::SCMD, Spc::CMD_TRANSFER);
    dma(0x32, 0x010000, kBlockSize);
    for (int i = 0; i < 20; i++) m->run(20000);
    diff = 0;
    for (uint32_t i = 0; i < kBlockSize; i++) diff += g_disk[9 * kBlockSize + i] != (uint8_t)(i * 5 + 2);
    CHECK_EQ(diff, 0);
    finish();
    m->attachScsi(0, nullptr);
    delete m;
}

// 内蔵 SCSI の DMA (DMAC のチャネル 1) の MAR がローカルメモリの範囲 ($1xxxxxxx) を指すとき: 68060 ではローカルメモリへ
// 届ける (Machine::dmaMemAddr。フロッピーディスクのチャネル 0 と同じ互換の処理。#167)。68030 では MAR の下位 24 ビット
// (本体のメモリ) に入る。まとめて転送する経路 (Machine::dmaBlockCopy) と 1 バイトずつの経路の両方で確かめる。
// ローカルメモリは CPU から読まず、READ(6) でローカルメモリに読んだものを、WRITE(6) で同じ番地からディスクの別の
// ブロックへ書き、ディスクの中身で比べる
void testDmaLocal() {
    printf("machine: scsi dma (channel 1) to local memory on 68060\n");
    const auto fc = m68k::FC_SUPER_DATA; // DMAC のレジスタはスーパーバイザのデータとして書く
    static uint8_t ipl[x68k::Machine::IPL_SIZE];
    memset(ipl, 0xff, sizeof ipl);
    const uint8_t vec[] = {0x00, 0x00, 0x20, 0x00, 0x00, 0xff, 0x00, 0x10, 0x60, 0xfe};
    memcpy(ipl + 0x10000, vec, 8);
    memcpy(ipl + 0x10010, vec + 8, 2);
    static uint8_t inRom[0x100];
    memset(inRom, 0xff, sizeof inRom);
    for (int cpu : {68030, 68060}) {
        for (bool block : {true, false}) {
            fillDisk();
            static HardDiskImage img;
            CHECK_EQ(img.load(g_disk, sizeof g_disk, kBlockSize), true);
            x68k::Config config;
            CHECK_EQ(config.setCpu(cpu), true);
            if (cpu == 68060) config.localMemSize = 16u << 20;
            x68k::Machine m(config);
            CHECK_EQ(m.ok(), true);
            CHECK_EQ(m.setIplRom(ipl, sizeof ipl), true);
            CHECK_EQ(m.setScsiRom(inRom, sizeof inRom), true);
            m.setDmaBlock(block);
            // まとめて転送する経路を通ったかを、Profile::dmaBlockBytes で見る
            x68k::Profile prof;
            prof.clock = []() -> uint64_t { return 0; };
            m.setProfile(&prof);
            m.powerOn();
            m.attachScsi(0, &img);
            Port in{m, 0xe96021};
            in.selectId0();
            in.wr(Spc::INTS, 0xff);
            auto dma = [&](uint8_t ocr, uint32_t mar, uint16_t count) {
                const uint32_t b = 0xe84040;
                auto out8 = [&](uint32_t a, uint8_t v) { m.write8(a, v, fc); };
                out8(b + 0x00, 0xff);
                out8(b + 0x04, 0x80);
                out8(b + 0x05, ocr);
                out8(b + 0x06, 0x04);
                out8(b + 0x0a, (uint8_t)(count >> 8));
                out8(b + 0x0b, (uint8_t)count);
                for (int i = 0; i < 4; i++) out8(b + 0x0c + (uint32_t)i, (uint8_t)(mar >> (24 - 8 * i)));
                const uint32_t dar = 0xe96021 + 2 * Spc::DREG;
                for (int i = 0; i < 4; i++) out8(b + 0x14 + (uint32_t)i, (uint8_t)(dar >> (24 - 8 * i)));
                out8(b + 0x29, 5);
                out8(b + 0x31, 5);
                out8(b + 0x07, 0x80);
            };
            // READ(6) / WRITE(6) を DMA で 1 回。cdb は 6 バイト、n は転送のバイト数
            auto transfer = [&](std::initializer_list<uint8_t> cdb, bool read, uint32_t mar, uint32_t n) {
                in.wr(Spc::INTS, 0xff);
                in.wr(Spc::PCTL, 0);
                in.wr(Spc::TEMP, 0x81);
                in.wr(Spc::TCH, 0x0f);
                in.wr(Spc::SCMD, Spc::CMD_SELECT);
                in.wr(Spc::INTS, 0xff);
                in.wr(Spc::PCTL, Spc::PH_COMMAND);
                in.wr(Spc::TCH, 0);
                in.wr(Spc::TCM, 0);
                in.wr(Spc::TCL, (uint8_t)cdb.size());
                in.wr(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
                for (uint8_t c : cdb) in.wr(Spc::DREG, c);
                in.wr(Spc::INTS, 0xff);
                in.wr(Spc::PCTL, read ? Spc::PH_DATA_IN : Spc::PH_DATA_OUT);
                in.wr(Spc::TCH, (uint8_t)(n >> 16));
                in.wr(Spc::TCM, (uint8_t)(n >> 8));
                in.wr(Spc::TCL, (uint8_t)n);
                in.wr(Spc::SCMD, Spc::CMD_TRANSFER);
                dma(read ? 0xb2 : 0x32, mar, (uint16_t)n);
                for (int i = 0; i < 20; i++) m.run(20000);
                CHECK_EQ(in.rd(Spc::INTS), Spc::INTS_COMPLETE);
                CHECK_EQ(m.read8(0xe84040, fc) & 0x80, 0x80); // DMAC の CSR: 転送が終わった
                in.wr(Spc::INTS, 0xff);
                for (int i = 0; i < 2; i++) { // ステータスとメッセージ
                    in.rd(Spc::TEMP);
                    in.wr(Spc::SCMD, Spc::CMD_SET_ACK);
                    in.wr(Spc::SCMD, Spc::CMD_RESET_ACK);
                }
                CHECK_EQ(in.rd(Spc::PSNS), 0);
            };
            for (uint32_t i = 0; i < 2 * kBlockSize; i++) m.write8(0x010000 + i, 0xee, fc);
            // ブロック 4 と 5 を $10010000 へ読み、同じ番地からブロック 20 と 21 へ書く
            transfer({0x08, 0, 0, 4, 2, 0}, true, 0x10010000, 2 * kBlockSize);
            CHECK_EQ(prof.dmaBlockBytes > 0, block);
            transfer({0x0a, 0, 0, 20, 2, 0}, false, 0x10010000, 2 * kBlockSize);
            m.setProfile(nullptr);
            int diff = 0, kept = 0, inMain = 0;
            for (uint32_t i = 0; i < 2 * kBlockSize; i++) {
                diff += g_disk[20 * kBlockSize + i] != pattern(4 * kBlockSize + i);
                const uint8_t v = m.read8(0x010000 + i, fc);
                kept += v == 0xee;
                inMain += v == pattern(4 * kBlockSize + i);
            }
            CHECK_EQ(diff, 0);
            // 68060 は本体の $010000 を書かない。68030 は本体の $010000 に入る
            if (cpu == 68060) CHECK_EQ(kept, (int)(2 * kBlockSize));
            else CHECK_EQ(inMain, (int)(2 * kBlockSize));
            m.attachScsi(0, nullptr);
        }
    }
}

} // namespace

int main() { // 試験の並びは、レジスタ、セレクション、転送、コマンド、状態の保存、Machine の順
    testRegisters();
    testSelect();
    testManual();
    testHardTransfer();
    testPadding();
    testMessages();
    testAbortDropsAtn();
    testDiskCommands();
    testReset();
    testDma(); // チャネル 1 の DMA
    testCached();
    testState();
    testStateAck();
    testDetach();
    testBoard();
    testDmaLocal();
    testOldStateData();
    return finish();
}
