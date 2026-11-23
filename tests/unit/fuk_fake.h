// 試験用: FUK (x68k/fuk.h) の相手をする偽物の装置。docs/fuk.md の手順どおりに答える、2HD のディスク 1 枚
// (ドライブ 0) の FDC。返事は latencyUs たってから届く (USB の往復の代わり)。時間は試験が進める。
#pragma once

#include <stdint.h>
#include <string.h>

#include <deque>
#include <vector>

#include "x68k/fuk.h"

class FakeFuk : public x68k::FukLink {
public:
    static constexpr uint32_t IMAGE_BYTES = 77 * 2 * 8 * 1024;

    FakeFuk() : image(IMAGE_BYTES) {
        for (uint32_t i = 0; i < IMAGE_BYTES; i++) image[i] = (uint8_t)(i * 7 + (i >> 10) * 13);
    }

    // ---- 試験が変えるもの ----
    std::vector<uint8_t> image;   // ユニット 0 (FD0) のディスク
    std::vector<uint8_t> image1;  // ユニット 1 (FD1) のディスク (空ならディスクなし)
    // ディスクから読んだ時刻に答える Read (#320 の FUK の動き。tools/pico_fukemu と同じ形)。R がこのセクタから始まる
    // Read は、R から EOT まで (realtimeOneSector なら R だけ) をディスクから読んだことにする。セクタ k は、1 バイト 16 us に
    // (1 + そのセクタの stretch) をかけた速さで読む (このリストにないセクタは stretch 0)
    struct Realtime {
        int unit, c, h, r;
        double stretch;
    };
    std::vector<Realtime> realtime;
    bool realtimeStream = true;     // true: データを 1 バイトずつ、ディスクから読んだ時刻に送る。
                                    // false: 'R' と長さとデータを読み始めの時刻にまとめて、リザルトを読み終えた時刻に送る
    bool realtimeOneSector = false; // true: 最初のセクタで終える (リザルトの R もそのセクタ)
    unsigned realtimeChunk = 0;     // 0 でなければ、realtimeStream のデータをこのバイト数ずつまとめ、まとまりの最後のバイトを
                                    // 読んだ時刻に送る (USB のパケットごとに送る装置のまね)
    // 読み出し (x68k/fuk_reader.h) の試験用
    std::vector<uint32_t> badSectors;   // 読むと CRC エラーになるセクタ (先頭からの番号)
    uint8_t badSt2 = 0x20;              // そのときの ST2
    unsigned badTimes = 0xffffffffu;    // badSectors のエラーを返す残りの回数 (それを過ぎると読める)
    bool eotAbnormal = false;           // EOT まで読み終えたら、TC のない FDC と同じく異常終了 (EN) で答える
    uint8_t readIdN = 3;                // READ ID が答えるセクタ長
    unsigned notReadySenses = 0;        // この回数だけ、SENSE DEVICE STATUS にレディでないと答える (モーターの立ち上がり)
    bool stickyInt = false;             // SENSE INTERRUPT STATUS で割り込みを下ろさない (readyInt で立てたとき)
    bool failSeeks = false;             // SEEK / RECALIBRATE が異常終了する
    bool readyInt = false;              // モーターを回すか止めると、レディの変化の割り込みを出す
    unsigned ejectAfterReads = 0;       // 0 でなければ、その回数目の Read のあとでディスクを入れ替える ('D')
    uint8_t lastMotor = 0;              // 最後にドライブ選択に書かれた値
    std::vector<uint8_t> driveControls; // ドライブ制御に書かれた値
    unsigned countWrites = 0;           // 転送バイト数を書かれた回数
    int cylinder() const { return pcn_; }
    bool plugged = true;
    unsigned plugCount = 1;
    bool swapPorts = false;    // true ならリンクのポート 0 が FUK のポート 2
    bool wrongDevice = false;  // 見分けに 'x' を返す
    bool silent = false;       // 何も返さない
    bool noReadFrame = false;  // Read のコマンドに 'R' を返さない
    bool writeProtected = false;
    unsigned sendLimit = 0;    // 0 でなければ、ポート 1 への 1 回の送信で受け付けるバイト数の上限
    unsigned sendLimit2 = 0;   // 同じく、ポート 2
    uint32_t asyncSendUs[2] = {0, 0}; // 0 でなければ、FUK のポート 1 / 2 に送ったバイトが装置に届くまでの時間 (口の中に残る)
    // 0 でなければ、ポート 2 に送ったバイトは口からすぐ出ていく (sendIdle() は true) が、装置が読むのはこの時間のあと
    // (装置の中のバッファにたまり、ポート 1 を先に読むことがある)
    uint32_t deviceDelay2Us = 0;
    unsigned rejectSends = 0;  // 0 でなければ、その回数だけ送信を受け付けない (0 バイトを返す)
    uint32_t latencyUs = 1000;
    uint32_t writeUs = 20000;  // 'M' を受け取ってから 'w' までの時間
    uint32_t seekUs = 3000;
    // 時間の元。nullptr なら試験が advance() で進める。Machine ごと動かす試験は、X68000 の時間を渡す
    uint64_t (*clock)(void*) = nullptr;
    void* clockArg = nullptr;

    // ---- 試験が見るもの ----
    std::vector<uint8_t> port1Log;       // ポート 1 に届いたバイト
    std::vector<uint8_t> commandLog;     // FDC が受け取ったコマンドの 1 バイト目
    unsigned overlappedReads = 0;        // 返事が届く前に次の読み出しが来た回数 (0 でなければならない)
    unsigned resets = 0;
    unsigned port2Unexpected = 0;        // 書くデータを待っていないときにポート 2 に届いた、0x00 でないバイト (0 でなければならない)
    unsigned readCommands = 0, writeCommands = 0;
    unsigned realtimeReads = 0;          // realtime に当たった Read

    void advance(uint64_t us) { t_ += us; }
    void unplug() {
        plugged = false;
        out_[0].clear();
        out_[1].clear();
        in_[0].clear();
        in_[1].clear();
    }
    // 同じ装置との通信が一度切れて、戻る (USB の転送のエラー)。装置の状態も、届く途中のバイトもそのまま
    void glitch() { plugged = false; }
    void resume() { plugged = true; }
    void plug() {
        plugged = true;
        plugCount++;
        resetDevice();
        p1Pending_ = -1;
        p2Data_ = false;
    }

    // ---- x68k::FukLink ----
    bool online() override { return plugged; }
    unsigned epoch() override { return plugCount; }
    uint64_t nowUs() override { return clock != nullptr ? clock(clockArg) + waited_ : t_; }
    void waitRx(unsigned us) override {
        if (clock == nullptr) t_ += us;
        else waited_ += us; // 待つ間は X68000 の時間が進まないので、待った分を足す
    }
    unsigned send(int port, const uint8_t* data, unsigned n) override {
        if (!plugged) return 0;
        const bool one = (port == 0) != swapPorts;
        if (rejectSends != 0) {
            rejectSends--;
            return 0;
        }
        if (one && sendLimit != 0 && n > sendLimit) n = sendLimit;
        if (!one && sendLimit2 != 0 && n > sendLimit2) n = sendLimit2;
        deliver();
        const int k = one ? 0 : 1;
        for (unsigned i = 0; i < n; i++) {
            if (!one && deviceDelay2Us != 0) in_[k].push_back(Byte{nowUs() + deviceDelay2Us, data[i], false});
            else if (asyncSendUs[k] != 0 || !in_[k].empty()) in_[k].push_back(Byte{nowUs() + asyncSendUs[k], data[i]});
            else if (one) rxPort1(data[i]);
            else rxPort2(data[i]);
        }
        return n;
    }
    bool sendIdle(int port) override {
        deliver();
        for (const Byte& b : in_[(port == 0) != swapPorts ? 0 : 1])
            if (b.host) return false;
        return true;
    }
    unsigned receive(int port, uint8_t* dst, unsigned max) override {
        if (!plugged) return 0;
        deliver();
        work();
        const int k = (port == 0) != swapPorts ? 0 : 1;
        std::deque<Byte>& q = out_[k];
        const uint64_t t = nowUs();
        unsigned n = 0;
        // 届いた時刻が同じバイトだけを 1 回で返す (実物の口も、届いたまとまりごとに時刻を付ける)
        while (n < max && !q.empty() && q.front().due <= t && (n == 0 || q.front().due == lastDue_[k])) {
            lastDue_[k] = q.front().due;
            dst[n++] = q.front().b;
            q.pop_front();
        }
        return n;
    }
    uint64_t receivedAtUs(int port) override { return lastDue_[(port == 0) != swapPorts ? 0 : 1]; }

private:
    struct Byte {
        uint64_t due;
        uint8_t b;
        bool host = true; // 口の中に残っている (false なら、装置の中のバッファにある)
    };
    enum Phase { COMMAND, WRITE_WAIT_M, WRITE_DATA, WRITE_BUSY, RESULT };

    uint64_t devNow() { return nowUs(); }
    // 口の中に残っていたバイトを、時間が来たものから装置に届ける (ポートごとに順番どおり。ポートの間の順番は時間で決まる)
    void deliver() {
        const uint64_t t = nowUs();
        for (;;) {
            int k = -1;
            for (int i = 0; i < 2; i++)
                if (!in_[i].empty() && in_[i].front().due <= t && (k < 0 || in_[i].front().due < in_[k].front().due)) k = i;
            if (k < 0) return;
            const uint8_t b = in_[k].front().b;
            in_[k].pop_front();
            if (k == 0) rxPort1(b);
            else rxPort2(b);
        }
    }
    void out1(uint8_t b) {
        if (!silent) out_[0].push_back(Byte{devNow() + latencyUs, b});
    }
    void out2At(uint64_t due, uint8_t b) {
        if (!silent) out_[1].push_back(Byte{due, b});
    }
    void out2(uint8_t b) { out2At(devNow() + latencyUs, b); }
    void raise() {
        if (!intLine_) {
            intLine_ = true;
            out2('C');
        }
    }
    void lower() {
        if (intLine_) {
            intLine_ = false;
            out2('c');
        }
    }
    bool ready() const { return forceReady_ || (motor_ && !image.empty()); }
    std::vector<uint8_t>& img(int un) { return un == 1 ? image1 : image; }
    bool hasDisk(int un) const { return ready() && (un == 0 ? !image.empty() : un == 1 ? !image1.empty() : false); }
    static uint32_t offsetOf(int c, int h, int r) { return (uint32_t)((c * 2 + h) * 8 + (r - 1)) * 1024; }
    static bool chsOk(int c, int h, int r, int n) { return n == 3 && c < 77 && h < 2 && r >= 1 && r <= 8; }
    void result7(uint8_t st0, uint8_t st1, uint8_t c, uint8_t h, uint8_t r, uint8_t n) {
        res_[0] = st0, res_[1] = st1, res_[2] = 0, res_[3] = c, res_[4] = h, res_[5] = r, res_[6] = n;
        resLen_ = 7;
        resPos_ = 0;
        phase_ = RESULT;
    }
    void resetDevice() {
        phase_ = COMMAND;
        cmdLen_ = 0;
        motor_ = forceReady_ = false;
        seekPending_ = seekDone_ = false;
        writeDue_ = 0;
        lower();
        resets++;
    }

    // 時間がたって終わる仕事 (シークの終わり、書き込みの終わり)
    void work() {
        const uint64_t t = devNow();
        if (seekPending_ && t >= seekDue_) {
            seekPending_ = false;
            seekDone_ = true;
            raise();
        }
        if (phase_ == WRITE_BUSY && t >= writeDue_) {
            finishWrite();
            out2('w');
        }
    }

    void finishWrite() {
        const int op = cmd_[0] & 0x1f;
        const uint8_t us = cmd_[1] & 7;
        uint8_t c = cmd_[2], h = cmd_[3], r = cmd_[4], n = cmd_[5];
        if (writeProtected) {
            result7((uint8_t)(0x40 | us), 0x02, c, h, r, n);
            return;
        }
        if (op == 0x0d) { // WRITE ID: 4 バイトずつの ID。セクタを D で埋める
            c = h = r = 0;
            n = cmd_[2];
            for (size_t i = 0; i + 4 <= wdata_.size() && i / 4 < cmd_[3]; i += 4) {
                c = wdata_[i], h = wdata_[i + 1], r = wdata_[i + 2], n = wdata_[i + 3];
                if (chsOk(c, h, r, n)) memset(&img(cmd_[1] & 3)[offsetOf(c, h, r)], cmd_[5], 1024);
            }
            result7(us, 0, c, h, r, n);
            return;
        }
        if (op != 0x05 && op != 0x09) { // SCAN: 対応しない
            result7((uint8_t)(0x40 | us), 0, c, h, r, n);
            return;
        }
        if (!chsOk(c, h, r, n) || cmd_[6] < r || cmd_[6] > 8) {
            result7((uint8_t)(0x40 | us), 0x01, c, h, r, n);
            return;
        }
        uint32_t bytes = (uint32_t)wdata_.size();
        const uint32_t room = (uint32_t)(cmd_[6] - r + 1) * 1024;
        if (bytes > room) bytes = room;
        if (bytes != 0) memcpy(&img(cmd_[1] & 3)[offsetOf(c, h, r)], wdata_.data(), bytes);
        const uint32_t sectors = bytes == 0 ? 1 : (bytes + 1023) / 1024;
        result7(us, 0, c, h, (uint8_t)(r + sectors - 1), n);
    }

    void execute() {
        const int op = cmd_[0] & 0x1f;
        const int un = cmd_[1] & 3;
        const uint8_t us = cmd_[1] & 7;
        commandLog.push_back(cmd_[0]);
        cmdLen_ = 0;
        switch (op) {
        case 0x02:
        case 0x06:
        case 0x0c: { // Read: 'R' 長さ データ リザルト。リザルトの段階には入らず、割り込みも出さない
            readCommands++;
            if (noReadFrame) return;
            const uint8_t c = cmd_[2], h = cmd_[3], r = cmd_[4], n = cmd_[5], eot = cmd_[6];
            uint32_t sectors = 0;
            uint8_t res[7] = {us, 0, 0, c, h, r, n};
            if (!hasDisk(un)) {
                res[0] = (uint8_t)(0x48 | us);
            } else if (!chsOk(c, h, r, n) || eot < r || eot > 8) {
                res[0] = (uint8_t)(0x40 | us);
                res[1] = 0x01;
            } else {
                sectors = (uint32_t)(eot - r + 1);
                res[5] = eot;
                pcn_ = c;
            }
            for (uint32_t k = 0; k < sectors; k++) {
                const uint32_t lba = offsetOf(c, h, r) / 1024 + k;
                bool bad = false;
                for (uint32_t b : badSectors) bad = bad || b == lba;
                if (!bad || badTimes == 0) continue;
                badTimes--;
                sectors = k + 1; // 読めなかったセクタのデータも付く
                res[0] = (uint8_t)(0x40 | us);
                res[1] = 0x20;
                res[2] = badSt2;
                res[5] = (uint8_t)(r + k);
                break;
            }
            if (eotAbnormal && sectors != 0 && res[0] == us) res[0] = (uint8_t)(0x40 | us), res[1] = 0x80;
            if (ejectAfterReads != 0 && readCommands == ejectAfterReads) out2('D');
            bool rt = false;
            for (const Realtime& x : realtime)
                if (sectors != 0 && x.unit == un && x.c == c && x.h == h && x.r == r) rt = true;
            if (rt) {
                realtimeReads++;
                if (realtimeOneSector) {
                    sectors = 1;
                    res[5] = r;
                }
                const uint32_t len1 = sectors * 1024 + 7;
                const uint64_t due = devNow() + latencyUs;
                out2At(due, 'R');
                out2At(due, (uint8_t)len1);
                out2At(due, (uint8_t)(len1 >> 8));
                double t = (double)due;
                for (uint32_t k = 0; k < sectors; k++) {
                    double stretch = 0;
                    for (const Realtime& x : realtime)
                        if (x.unit == un && x.c == c && x.h == h && x.r == (int)(r + k)) stretch = x.stretch;
                    const double byteUs = 16.0 * (1.0 + stretch);
                    for (uint32_t i = 0; i < 1024; i++) {
                        uint32_t last = i; // このバイトと一緒に送る、まとまりの最後のバイト
                        if (realtimeChunk != 0) {
                            last = (i / realtimeChunk + 1) * realtimeChunk - 1;
                            if (last > 1023) last = 1023;
                        }
                        out2At(realtimeStream ? (uint64_t)(t + byteUs * (last + 1)) : due, img(un)[offsetOf(c, h, r + k) + i]);
                    }
                    t += byteUs * 1024 + 32 + 50; // データのあとの CRC と、次のセクタの ID までのすき間 (おおよそ)
                }
                const uint64_t end = (uint64_t)t;
                for (int i = 0; i < 7; i++) out2At(end, res[i]);
                return;
            }
            const uint32_t len = sectors * 1024 + 7;
            // USB のフルスピードで運ぶ時間 (1ms に 1KB ほど) をまねて、少しずつ届ける
            uint64_t due = devNow() + latencyUs;
            out2At(due, 'R');
            out2At(due, (uint8_t)len);
            out2At(due, (uint8_t)(len >> 8));
            for (uint32_t i = 0; i < sectors * 1024; i++) out2At(due + i, img(un)[offsetOf(c, h, r) + i]);
            due += sectors * 1024;
            for (int i = 0; i < 7; i++) out2At(due, res[i]);
            return;
        }
        case 0x05:
        case 0x09:
        case 0x0d:
        case 0x11:
        case 0x19:
        case 0x1d:
            writeCommands++;
            if (!hasDisk(un)) { // 書けない: 'W' を飛ばして 'w'
                result7((uint8_t)(0x48 | us), 0, cmd_[2], cmd_[3], cmd_[4], cmd_[5]);
                out2('w');
                return;
            }
            wdata_.clear();
            phase_ = WRITE_WAIT_M;
            out2('W');
            return;
        case 0x03: return;
        case 0x04:
            if (notReadySenses != 0) {
                notReadySenses--;
                res_[0] = (uint8_t)(0x08 | us);
                resLen_ = 1;
                resPos_ = 0;
                phase_ = RESULT;
                return;
            }
            res_[0] = (uint8_t)((hasDisk(un) ? 0x20 : 0) | (hasDisk(un) && pcn_ == 0 ? 0x10 : 0) | 0x08 | us);
            resLen_ = 1;
            resPos_ = 0;
            phase_ = RESULT;
            return;
        case 0x07:
        case 0x0f:
            pcn_ = op == 0x07 ? 0 : cmd_[2];
            seekSt0_ = (uint8_t)(0x20 | us | (hasDisk(un) ? 0 : 0x48) | (failSeeks ? 0x50 : 0));
            seekPending_ = true;
            seekDue_ = devNow() + seekUs;
            return;
        case 0x08:
            lower();
            if (readyPending_) {
                if (stickyInt && !motor_) raise();
                else readyPending_ = false;
                res_[0] = 0xc0;
                res_[1] = pcn_;
                resLen_ = 2;
            } else if (seekDone_) {
                seekDone_ = false;
                res_[0] = seekSt0_;
                res_[1] = pcn_;
                resLen_ = 2;
            } else {
                res_[0] = 0x80;
                resLen_ = 1;
            }
            resPos_ = 0;
            phase_ = RESULT;
            return;
        case 0x0a: // READ ID: リザルトを用意して割り込み。1 バイト目を読まれたら下ろす
            result7((uint8_t)(hasDisk(un) ? us : 0x48 | us), 0, pcn_, (uint8_t)((cmd_[1] >> 2) & 1), 1, readIdN);
            readIdIrq_ = true;
            raise();
            return;
        default:
            res_[0] = 0x80;
            resLen_ = 1;
            resPos_ = 0;
            phase_ = RESULT;
            return;
        }
    }

    uint8_t regRead(int reg) {
        work();
        switch (reg) {
        case 0x00: return wrongDevice ? 'x' : 'f';
        case 0x01:
            if (phase_ == RESULT) return 0xd0;
            if (phase_ != COMMAND) return 0x10;
            return (uint8_t)((cmdLen_ ? 0x90 : 0x80) | (seekPending_ || seekDone_ ? 0x01 : 0));
        case 0x03: {
            if (phase_ != RESULT) return 0;
            const uint8_t b = res_[resPos_++];
            if (resPos_ == 1 && readIdIrq_) {
                readIdIrq_ = false;
                lower();
            }
            if (resPos_ >= resLen_) {
                phase_ = COMMAND;
                if (seekDone_) raise();
            }
            return b;
        }
        case 0x05: return (driveSel_ == 1 && !image.empty()) || (driveSel_ == 2 && !image1.empty()) ? 0x80 : 0x00;
        default: return 0xff;
        }
    }

    void regWrite(int reg, uint8_t v) {
        work();
        switch (reg) {
        case 0x03:
            if (phase_ != COMMAND) return;
            if (cmdLen_ == 0) {
                static const uint8_t kLen[32] = {1, 1, 9, 3, 2, 9, 9, 2, 1, 9, 2, 1, 9, 6, 1, 3,
                                                 1, 9, 1, 1, 1, 1, 1, 1, 1, 9, 1, 1, 1, 9, 1, 1};
                cmdNeed_ = kLen[v & 0x1f];
                memset(cmd_, 0, sizeof cmd_);
            }
            cmd_[cmdLen_++] = v;
            if (cmdLen_ >= cmdNeed_) execute();
            return;
        case 0x05:
            driveSel_ = v & 15;
            driveControls.push_back(v);
            return;
        case 0x07:
            if (readyInt && ((v & 0x80) != 0) != motor_) {
                readyPending_ = true;
                raise();
            }
            motor_ = (v & 0x80) != 0;
            lastMotor = v;
            return;
        case 0x23: forceReady_ = (v & 0x40) != 0; return;
        case 0x30:
            countWrites++;
            count_ = (uint16_t)((count_ & 0xff00) | v); return;
        case 0x31:
            countWrites++;
            count_ = (uint16_t)((count_ & 0x00ff) | v << 8); return;
        default: return;
        }
    }

    void rxPort1(uint8_t b) {
        port1Log.push_back(b);
        work();
        if (p1Pending_ >= 0) {
            const int reg = p1Pending_;
            p1Pending_ = -1;
            regWrite(reg, b);
            return;
        }
        if (b & 0x80) {
            p1Pending_ = b & 0x7f;
            return;
        }
        if (b == 0x40) {
            resetDevice();
            return;
        }
        if (!out_[0].empty() && b != 0x00) overlappedReads++;
        out1(regRead(b));
    }

    void rxPort2(uint8_t b) {
        if (phase_ == WRITE_DATA) {
            wdata_.push_back(b);
            if (wdata_.size() >= count_) startWrite();
            return;
        }
        if (phase_ == WRITE_WAIT_M && b == 'M') {
            phase_ = WRITE_DATA;
            if (count_ == 0) startWrite();
            return;
        }
        if (b == 0x00) out2(wrongDevice ? 'x' : 'F');
        else port2Unexpected++;
    }
    void startWrite() {
        phase_ = WRITE_BUSY;
        writeDue_ = devNow() + writeUs;
    }

    uint64_t t_ = 1000000, waited_ = 0;
    std::deque<Byte> out_[2];
    uint64_t lastDue_[2] = {0, 0};
    std::deque<Byte> in_[2];
    Phase phase_ = COMMAND;
    uint8_t cmd_[9] = {};
    int cmdLen_ = 0, cmdNeed_ = 0;
    uint8_t res_[7] = {};
    int resLen_ = 0, resPos_ = 0;
    int p1Pending_ = -1;
    bool p2Data_ = false;
    bool motor_ = false, forceReady_ = false, intLine_ = false, readIdIrq_ = false;
    bool readyPending_ = false;
    bool seekPending_ = false, seekDone_ = false;
    uint64_t seekDue_ = 0, writeDue_ = 0;
    uint8_t seekSt0_ = 0, pcn_ = 0, driveSel_ = 0;
    uint16_t count_ = 0;
    std::vector<uint8_t> wdata_;
};
