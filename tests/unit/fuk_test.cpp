// FUK (x68k/fuk.h) の単体テスト。相手は偽物の装置 (fuk_fake.h)
#include <string.h>

#include "fuk_fake.h"
#include "test_check.h"
#include "x68k/fuk.h"

using x68k::Fuk;

namespace {

struct Count : Fuk::Host {
    uint32_t count = 0;
    bool single = true;
    uint32_t fukDmaCount() override { return count; }
    bool fukDmaSingleBlock() override { return single; }
};

// 時間を step ずつ進めながら poll() し、done() になるまでの時間 (us) を返す。limit を超えたら limit
template <typename F> uint64_t runUntil(FakeFuk& dev, Fuk& fuk, F done, uint64_t limit = 20000000, uint64_t step = 100) {
    uint64_t t = 0;
    for (; t < limit; t += step) {
        fuk.poll();
        if (done()) return t;
        dev.advance(step);
    }
    return limit;
}

void command(Fuk& fuk, const uint8_t* bytes, int n) {
    for (int i = 0; i < n; i++) fuk.write(3, bytes[i]);
}

// ステータスが RQM になるまで待ってから 1 バイト書く、ソフトウェアと同じ手順
void sendCommand(FakeFuk& dev, Fuk& fuk, const uint8_t* bytes, int n) {
    for (int i = 0; i < n; i++) {
        CHECK_EQ(runUntil(dev, fuk, [&] { return (fuk.read(1) & 0xc0) == 0x80; }) < 1000000, 1);
        fuk.write(3, bytes[i]);
    }
}

// リザルトを n バイト読む (RQM と DIO を待ってから)
void readResult(FakeFuk& dev, Fuk& fuk, uint8_t* out, int n) {
    for (int i = 0; i < n; i++) {
        CHECK_EQ(runUntil(dev, fuk, [&] { return (fuk.read(1) & 0xc0) == 0xc0; }) < 1000000, 1);
        out[i] = fuk.read(3);
    }
}

void motorOn(FakeFuk& dev, Fuk& fuk) {
    fuk.write(7, 0x80);
    runUntil(dev, fuk, [] { return false; }, 2000);
}

// 1 セクタぶん (1024 バイト) の DMA の Read を出し、DMAC が 32 us ごとに読める分を読むのをまねて、最初のバイトから
// 最後のバイト (TC。DMAC の COC) までの時間 (us) を返す。first には、コマンドを出してから
// 最初のバイトまでの時間を入れる。resultBeforeTc には、TC の時点でリザルトが用意されていたかを入れる
uint64_t timedRead(FakeFuk& dev, Fuk& fuk, Count& host, int unit, int c, int h, int r, int eot, uint64_t* first = nullptr,
                   bool* resultBeforeTc = nullptr) {
    host.count = 1024;
    const uint8_t cmd[] = {0x46, (uint8_t)unit, (uint8_t)c, (uint8_t)h, (uint8_t)r, 3, (uint8_t)eot, 0x35, 0xff};
    sendCommand(dev, fuk, cmd, 9);
    uint8_t buf[1024];
    int got = 0;
    uint64_t t0 = 0, t1 = 0;
    for (uint64_t t = 0; t < 2000000 && got < 1024; t += 32) {
        fuk.poll();
        while (got < 1024 && fuk.dataReady()) {
            const int k = fuk.readData(buf + got, 1024 - got);
            if (k <= 0) break;
            if (got == 0) t0 = t;
            got += k;
            t1 = t;
        }
        dev.advance(32);
    }
    CHECK_EQ(got, 1024);
    CHECK_EQ(memcmp(buf, &(unit == 1 ? dev.image1 : dev.image)[((c * 2 + h) * 8 + (r - 1)) * 1024], 1024), 0);
    fuk.terminalCount();
    if (resultBeforeTc) *resultBeforeTc = (fuk.read(1) & 0xc0) == 0xc0;
    CHECK_EQ(fuk.dataReady(), 0); // TC のあとは渡さない
    uint8_t res[7];
    readResult(dev, fuk, res, 7);
    CHECK_EQ(res[0] & 0xc0, 0);
    if (first) *first = t0;
    return t1 - t0;
}

// 2 つの Read の長さの差 (届いた時刻の間隔が保たれたかを見る。#320)
long sectorDiff(uint64_t t7, uint64_t t8) { return (long)(((long long)t8 - (long long)t7) * 10000 / (long long)((t7 + t8) / 2)); }

// ディスクから読んだ時刻に 1 バイトずつ答える FUK (tools/pico_fukemu の既定: R から EOT まで読む) の Read を、届いた分から
// DMAC に渡す。DMAC の転送は届くのに合わせて進み (最初のバイトはデータが全部届く前に渡る)、1024 バイト目 (TC) は
// リザルトが届く前に来る。装置が読む時間を長くした分が、渡す時刻にも現れる
void testStreamedRead() {
    const double stretches[] = {0.023, 0.0};
    for (double stretch : stretches) {
        FakeFuk dev;
        Count host;
        dev.image1 = dev.image;
        for (int i = 0; i < 1024; i++) dev.image1[(2 * 8 + 6) * 1024 + i] = (uint8_t)(i * 7); // C1 H0 R7
        dev.realtime.push_back({1, 1, 0, 7, 0.0});
        dev.realtime.push_back({1, 1, 0, 8, stretch});
        Fuk fuk(dev, &host);
        CHECK_EQ(fuk.start(1000), 1);
        motorOn(dev, fuk);
        long sum = 0;
        for (int i = 0; i < 3; i++) {
            uint64_t first7 = 0;
            bool res7 = true, res8 = true;
            const uint64_t t7 = timedRead(dev, fuk, host, 1, 1, 0, 7, 8, &first7, &res7); // R7-EOT8
            const uint64_t t8 = timedRead(dev, fuk, host, 1, 1, 0, 8, 8, nullptr, &res8);
            CHECK_EQ(t7 > 15000 && t7 < 18000, 1);
            CHECK_EQ(first7 < 2000, 1); // 最初のバイトは、データが届き始めてすぐに渡る
            CHECK_EQ(res7, 0);          // TC の時点で、セクタ 8 とリザルトはまだ届いていない
            CHECK_EQ(res8, 0);          // R8-EOT8 も、TC はリザルトの前 (リザルトはデータのあとの CRC の分だけ遅れる)
            sum += sectorDiff(t7, t8);
        }
        CHECK_EQ(dev.realtimeReads, 6u);
        CHECK_EQ(fuk.stats().streamedReads >= 6, 1);
        const long score = sum / 3;
        printf("  streamed read: stretch %.1f%% score %ld\n", stretch * 100, score);
        if (stretch > 0) CHECK_EQ(score >= 120 && score <= 350, 1);
        else CHECK_EQ(score < 120, 1);
        // 覚えていたトラックから答えた (速く届いた) Read は、USB で届く速さで渡る
        const uint64_t t = timedRead(dev, fuk, host, 1, 2, 0, 1, 1);
        CHECK_EQ(t < 2000, 1);
    }
}

// 届いている途中の DMA の異常終了: 残りとリザルトが届くのを待ってから、オーバーランのリザルトを返す
void testStreamedReadAbort() {
    FakeFuk dev;
    Count host;
    dev.image1 = dev.image;
    dev.realtime.push_back({1, 1, 0, 7, 0.0});
    Fuk fuk(dev, &host);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    host.count = 1024;
    const uint8_t cmd[] = {0x46, 1, 1, 0, 7, 3, 8, 0x35, 0xff};
    sendCommand(dev, fuk, cmd, 9);
    uint8_t buf[1024];
    int got = 0;
    for (int t = 0; t < 100000 && got < 100; t += 32) {
        fuk.poll();
        if (fuk.dataReady()) got += fuk.readData(buf, 100 - got > 0 ? 100 - got : 0);
        dev.advance(32);
    }
    CHECK_EQ(got >= 100, 1);
    fuk.abortTransfer();
    CHECK_EQ(fuk.dataReady(), 0);
    CHECK_EQ((fuk.read(1) & 0xc0) == 0xc0, 0); // リザルトはまだ
    uint8_t res[7];
    readResult(dev, fuk, res, 7);
    CHECK_EQ(res[0] & 0xc0, 0x40);
    CHECK_EQ(res[1] & 0x10, 0x10);
    CHECK_EQ(fuk.stats().timeouts, 0);
    // 次のコマンドがふつうに動く
    CHECK_EQ(timedRead(dev, fuk, host, 1, 2, 0, 1, 1) < 2000, 1);
}

// 届いている途中で 'R' の残りが来なくなったら、READ_TIMEOUT_US で時間切れにする
void testStreamedReadStall() {
    FakeFuk dev;
    Count host;
    dev.image1 = dev.image;
    dev.realtime.push_back({1, 1, 0, 7, 0.0});
    Fuk fuk(dev, &host);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    host.count = 1024;
    const uint8_t cmd[] = {0x46, 1, 1, 0, 7, 3, 8, 0x35, 0xff};
    sendCommand(dev, fuk, cmd, 9);
    uint8_t buf[1024];
    int got = 0;
    for (int t = 0; t < 100000 && got < 100; t += 32) {
        fuk.poll();
        if (fuk.dataReady()) got += fuk.readData(buf, 100 - got > 0 ? 100 - got : 0);
        dev.advance(32);
    }
    CHECK_EQ(got >= 100, 1);
    dev.unplug();
    dev.plugged = true; // 届く途中のバイトだけを捨てる (同じ装置のまま、残りは来ない)
    const uint64_t waited = runUntil(dev, fuk, [&] { return fuk.irq(); });
    CHECK_EQ(waited >= Fuk::READ_TIMEOUT_US - 100000 && waited <= Fuk::READ_TIMEOUT_US + 100000, 1);
    CHECK_EQ(fuk.stats().timeouts, 1);
    uint8_t res[7];
    readResult(dev, fuk, res, 7);
    CHECK_EQ(res[0] & 0xc8, 0x48); // 異常終了、ノットレディ
}

// X68000 の時間を、偽物の装置の時間 (実時間) と別に進める
struct Burst : Count {
    uint64_t emuNs = 1;
    uint64_t fukEmuNs() override { return emuNs; }
    uint64_t fukFrameNs() override { return 16700000; }
};

// 実機と同じように、X68000 を 1 フレーム分 (16.7 ms) まとめて実時間の burstUs で動かしてから、残りを待つ。poll() は
// 動かしている間だけラスタごとに呼ばれる。1 セクタぶんの DMA の Read を出し、X68000 の時間で最初のバイトから TC までの
// 時間 (us) を返す。delay には、コマンドを出してから最初のバイトを渡すまでの X68000 の時間 (us) を入れる
// emuScale を 1 より大きくすると、X68000 が実時間より遅れていて追いつく場面 (1 フレームを、実時間の 1 フレーム分より
// 長い X68000 の時間ぶん動かす) になる
uint64_t burstRead(FakeFuk& dev, Fuk& fuk, Burst& host, int r, uint32_t burstUs, uint32_t phaseUs, uint64_t* delay = nullptr,
                   unsigned emuScale = 1) {
    host.count = 1024;
    const uint8_t cmd[] = {0x46, 1, 1, 0, (uint8_t)r, 3, 8, 0x35, 0xff};
    for (int i = 0; i < 9; i++) {
        while ((fuk.read(1) & 0xc0) != 0x80) {
            fuk.poll();
            dev.advance(10);
            host.emuNs += 10000;
        }
        fuk.write(3, cmd[i]);
    }
    const uint64_t start = host.emuNs;
    dev.advance(phaseUs); // フレームの切れ目との位置をずらす (X68000 は休んでいる)
    const int RASTERS = 262;
    const uint64_t frameUs = 16700;
    uint8_t buf[1024];
    int got = 0;
    uint64_t t0 = 0, t1 = 0;
    for (int f = 0; f < 200 && got < 1024; f++) {
        uint32_t spent = 0; // このフレームで進めた実時間 (us)
        for (int i = 0; i < RASTERS && got < 1024; i++) {
            host.emuNs += frameUs * 1000 * emuScale / RASTERS;
            const uint32_t next = (uint32_t)((uint64_t)burstUs * (i + 1) / RASTERS); // 割り算の切り捨てをためない
            dev.advance(next - spent);
            spent = next;
            fuk.poll();
            while (got < 1024 && fuk.dataReady()) {
                const int k = fuk.readData(buf + got, 1024 - got);
                if (k <= 0) break;
                if (got == 0) t0 = host.emuNs;
                got += k;
                t1 = host.emuNs;
            }
        }
        dev.advance(frameUs - spent); // 次のフレームまで待つ (poll() は呼ばれない)
    }
    CHECK_EQ(got, 1024);
    fuk.terminalCount();
    for (int i = 0; i < 7; i++) {
        while ((fuk.read(1) & 0xc0) != 0xc0) {
            fuk.poll();
            dev.advance(10);
            host.emuNs += 10000;
        }
        fuk.read(3);
    }
    if (delay) *delay = (t0 - start) / 1000;
    return (t1 - t0) / 1000;
}

// X68000 が 1 フレーム分を実時間の 1 / 4 / 12 ms で動かしてから休んでも (届いたバイトを受け取るのがフレームの初めに
// 寄っても)、届いた時刻の間隔のまま X68000 の時間に写すので、装置が読む時間を長くした分が、渡す時刻にも現れる。
// 装置が 1 バイトずつ送る場合と、64 バイト (USB のフルスピードのパケット 1 つ) ずつまとめて送る場合の両方で確かめる
void testStreamedReadBursty() {
    const uint32_t bursts[] = {1000, 4000, 12000};
    const double stretches[] = {0.023, 0.0};
    const unsigned chunks[] = {0, 64};
    for (unsigned chunk : chunks)
    for (double stretch : stretches)
        for (uint32_t burst : bursts) {
            FakeFuk dev;
            Burst host;
            dev.realtimeChunk = chunk;
            dev.image1 = dev.image;
            dev.realtime.push_back({1, 1, 0, 7, 0.0});
            dev.realtime.push_back({1, 1, 0, 8, stretch});
            Fuk fuk(dev, &host);
            CHECK_EQ(fuk.start(1000), 1);
            motorOn(dev, fuk);
            long sum = 0;
            uint64_t maxDelay = 0;
            for (int i = 0; i < 3; i++) {
                uint64_t d7 = 0, d8 = 0;
                const uint64_t t7 = burstRead(dev, fuk, host, 7, burst, (uint32_t)(i * 5300), &d7);
                const uint64_t t8 = burstRead(dev, fuk, host, 8, burst, (uint32_t)(i * 7100 + 2500), &d8);
                CHECK_EQ(t7 > 15000 && t7 < 18000, 1);
                sum += sectorDiff(t7, t8);
                maxDelay = d7 > maxDelay ? d7 : maxDelay;
                maxDelay = d8 > maxDelay ? d8 : maxDelay;
            }
            const long score = sum / 3;
            printf("  bursty: chunk %2u stretch %.1f%% burst %5u us score %ld (first byte after %llu us at most)\n", chunk,
                   stretch * 100, burst, score, (unsigned long long)maxDelay);
            if (stretch > 0) CHECK_EQ(score >= 120 && score <= 350, 1);
            else CHECK_EQ(score > -60 && score < 60, 1);
            CHECK_EQ(fuk.stats().timedReads >= 6, 1);
            CHECK_EQ(fuk.stats().lateReads, 0u); // L が足りている (渡す時刻より遅れて届いたバイトがない)
            CHECK_EQ(fuk.stats().tcReads, 6u);
        }
}

// X68000 が実時間より遅れていて、1 フレームを実時間の 4 倍の速さで動かして追いつく間は、バイトが渡す時刻より遅れて
// 届く。その Read を late として数える
void testStreamedReadLate() {
    FakeFuk dev;
    Burst host;
    dev.image1 = dev.image;
    dev.realtime.push_back({1, 1, 0, 7, 0.0});
    Fuk fuk(dev, &host);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    burstRead(dev, fuk, host, 7, 16700, 0, nullptr, 4);
    printf("  late: %u reads, %u us at most\n", fuk.stats().lateReads, fuk.stats().lateMaxUs);
    CHECK_EQ(fuk.stats().lateReads, 1u);
    CHECK_EQ(fuk.stats().lateMaxUs > 1000, 1);
}

// Burst のホスト (X68000 の時間が分かる) でも、装置がすぐに答えた Read (トラックを覚えていた) は、L が小さく、短い時間で
// 渡し終える (偽物の装置は、この Read のデータを 1 us ずつの間隔で送る。最初のバイトまでの時間は表示するだけ)
void testBurstFastRead() {
    FakeFuk dev;
    Burst host;
    dev.image1 = dev.image;
    Fuk fuk(dev, &host);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    uint64_t d = 0;
    const uint64_t t = burstRead(dev, fuk, host, 7, 1000, 0, &d);
    printf("  fast read: first byte after %llu us, TC after %llu us, lead %llu us\n", (unsigned long long)d,
           (unsigned long long)t, (unsigned long long)fuk.stats().leadUs);
    CHECK_EQ(fuk.stats().timedReads, 1u);
    CHECK_EQ(t < 2000, 1); // 1 セクタを読む時間 (16 ms) よりずっと短い (この試験の poll() は 1 ラスタに 1 回なので 1 ms ほど)
    CHECK_EQ(fuk.stats().leadUs < 1000, 1);
    CHECK_EQ(fuk.stats().lateReads, 0u);
}

// fuk_lead= で遅れを決めたときは、'R' が早く全部届いても、届き方によらずその遅れで渡し始める。TC の長さの統計も取れる
void testStreamedReadFixedLead() {
    FakeFuk dev;
    Burst host;
    dev.image1 = dev.image;
    dev.realtime.push_back({1, 1, 0, 7, 0.0});
    Fuk fuk(dev, &host);
    fuk.setLeadUs(30000);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    uint64_t d = 0;
    const uint64_t t = burstRead(dev, fuk, host, 7, 1000, 0, &d);
    printf("  fixed lead: first byte after %llu us, TC after %llu us, lead %llu us\n", (unsigned long long)d,
           (unsigned long long)t, (unsigned long long)fuk.stats().leadUs);
    CHECK_EQ(fuk.stats().leadUs, 30000u);
    CHECK_EQ(d >= 30000 && d < 30000 + 16700 + 1000, 1); // 最初のバイトが届くまでの時間 (読み始めの待ちなど) を足した分まで
    CHECK_EQ(t > 15000 && t < 18000, 1);
    CHECK_EQ(fuk.stats().tcReads, 1u);
}

void testIdentify() {
    for (int swap = 0; swap < 2; swap++) {
        FakeFuk dev;
        dev.swapPorts = swap != 0;
        Fuk fuk(dev);
        CHECK_EQ(fuk.start(1000), 1);
        CHECK_EQ(fuk.ready(), 1);
        CHECK_EQ(fuk.stats().links, 1);
        // 初期化 (0x80 0xFE) とリセット (0x40) を、見分けの 0x00 のあとに送っている
        CHECK_EQ(dev.port1Log.size() >= 4, 1);
        CHECK_EQ(dev.port1Log[0], 0x00);
        CHECK_EQ(dev.port1Log[1], 0x80);
        CHECK_EQ(dev.port1Log[2], 0xfe);
        CHECK_EQ(dev.port1Log[3], 0x40);
        CHECK_EQ(fuk.read(1), 0x80);
    }
    {
        FakeFuk dev;
        dev.wrongDevice = true;
        Fuk fuk(dev);
        CHECK_EQ(fuk.start(1000), 0);
        CHECK_EQ(fuk.ready(), 0);
        CHECK_EQ(fuk.stats().linkFailures >= 1, 1);
        CHECK_EQ(fuk.read(1), 0x80); // つながっていない間も、コマンドは受け付ける
    }
    {
        FakeFuk dev;
        dev.silent = true;
        Fuk fuk(dev);
        CHECK_EQ(fuk.start(1000), 0);
    }
    {
        FakeFuk dev;
        dev.plugged = false;
        Fuk fuk(dev);
        CHECK_EQ(fuk.start(100), 0);
        // あとからつなぐと、poll() の中で見分ける
        dev.plug();
        CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.ready(); }) < 1000000, 1);
    }
}

void testSeek() {
    FakeFuk dev;
    Fuk fuk(dev);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    const uint8_t seek[] = {0x0f, 0x00, 5};
    sendCommand(dev, fuk, seek, 3);
    // 送った直後は、覚えているステータスが使えないのでビジーを返す (待たない)
    CHECK_EQ(fuk.read(1), 0x10);
    CHECK_EQ(fuk.irq(), 0);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.irq(); }) < 100000, 1);
    const uint8_t sis[] = {0x08};
    sendCommand(dev, fuk, sis, 1);
    CHECK_EQ(fuk.read(1), 0x10); // リザルトがそろうまではビジー
    uint8_t res[2];
    readResult(dev, fuk, res, 2);
    CHECK_EQ(res[0], 0x20);
    CHECK_EQ(res[1], 5);
    CHECK_EQ(runUntil(dev, fuk, [&] { return !fuk.irq() && fuk.read(1) == 0x80; }) < 100000, 1);
    // 引き取る割り込みがないときの SENSE INTERRUPT STATUS は 1 バイト (0x80)
    sendCommand(dev, fuk, sis, 1);
    readResult(dev, fuk, res, 1);
    CHECK_EQ(res[0], 0x80);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.read(1) == 0x80; }) < 100000, 1);
    // SENSE DEVICE STATUS
    const uint8_t sds[] = {0x04, 0x00};
    sendCommand(dev, fuk, sds, 2);
    readResult(dev, fuk, res, 1);
    CHECK_EQ(res[0], 0x28); // レディ、両面。トラック 0 ではない
    // ここまで、X68000 を待たせていない
    CHECK_EQ(fuk.stats().waits, 0);
    CHECK_EQ(dev.overlappedReads, 0);
}

void readTrack(FakeFuk& dev, Fuk& fuk, Count& host, bool block, uint32_t count) {
    host.count = count;
    const uint8_t read[] = {0x46, 0x00, 2, 0, 1, 3, 8, 0x1b, 0xff}; // C2 H0 R1-8
    sendCommand(dev, fuk, read, 9);
    CHECK_EQ(fuk.read(1), 0x10);
    CHECK_EQ(fuk.dataReady(), 0);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.dataReady(); }) < 1000000, 1);
    CHECK_EQ(fuk.irq(), 0);
    static uint8_t got[8192];
    memset(got, 0, sizeof got);
    uint32_t n = 0;
    // データは届いた分から渡る (#320)。届いている途中では、届いた分より先は渡らない
    runUntil(dev, fuk, [&] {
        if (block) {
            while (n < count && fuk.dataReady()) {
                const int k = fuk.readData(got + n, (int)(count - n < 100 ? count - n : 100));
                if (k <= 0) break;
                n += (uint32_t)k;
            }
        } else {
            while (n < count && fuk.dataReady()) got[n++] = fuk.read(3);
        }
        return n >= count;
    }, 1000000, 10);
    CHECK_EQ(n, count);
    fuk.terminalCount();
    CHECK_EQ(memcmp(got, &dev.image[(2 * 2 * 8) * 1024], count), 0);
    CHECK_EQ(fuk.dataReady(), 0);
    // TC のあとは、残りとリザルトが届いてから割り込み
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.irq(); }) < 1000000, 1);
    CHECK_EQ(fuk.read(1), 0xd0);
    uint8_t res[7];
    for (int i = 0; i < 7; i++) res[i] = fuk.read(3);
    CHECK_EQ(res[0], 0x00);
    CHECK_EQ(res[3], 2);
    CHECK_EQ(res[5], 8);
    CHECK_EQ(res[6], 3);
    CHECK_EQ(fuk.irq(), 0); // リザルトを読むと割り込みが消える
}

void testRead() {
    FakeFuk dev;
    Count host;
    Fuk fuk(dev, &host);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    readTrack(dev, fuk, host, true, 8192);
    readTrack(dev, fuk, host, false, 8192);
    readTrack(dev, fuk, host, true, 1024); // DMAC の転送バイト数が少ない: 残りは捨てる
    // Read のあとの SENSE INTERRUPT STATUS は FUK に送らず、0x80 を返す
    const size_t commands = dev.commandLog.size();
    const uint8_t sis[] = {0x08};
    sendCommand(dev, fuk, sis, 1);
    CHECK_EQ(fuk.read(1), 0xd0);
    CHECK_EQ(fuk.read(3), 0x80);
    CHECK_EQ(dev.commandLog.size(), commands);
    CHECK_EQ(fuk.stats().waits, 0);
    CHECK_EQ(dev.overlappedReads, 0);

    // DMA を使わない (転送バイト数が 0): ステータスは 0xF0 で、CPU が 1 バイトずつ読む
    host.count = 0;
    const uint8_t read[] = {0x46, 0x00, 0, 0, 1, 3, 1, 0x1b, 0xff};
    sendCommand(dev, fuk, read, 9);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.read(1) == 0xf0; }) < 1000000, 1);
    for (int i = 0; i < 1024; i++) {
        CHECK_EQ(fuk.read(1), 0xf0);
        CHECK_EQ(fuk.read(3), dev.image[i]);
    }
    CHECK_EQ(fuk.read(1), 0xd0);
    CHECK_EQ(fuk.irq(), 1);
    for (int i = 0; i < 7; i++) fuk.read(3);

    // レディでないドライブ: データなしでリザルトだけが届く
    const uint8_t read1[] = {0x46, 0x01, 0, 0, 1, 3, 1, 0x1b, 0xff};
    sendCommand(dev, fuk, read1, 9);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.read(1) == 0xd0; }) < 1000000, 1);
    CHECK_EQ(fuk.read(3), 0x49);
    for (int i = 1; i < 7; i++) fuk.read(3);
}

// 口が少しずつしか受け付けないとき (Linux の USB シリアル) も、送り残しを続けて送る
void testPartialSend() {
    FakeFuk dev;
    Count host;
    Fuk fuk(dev, &host);
    // 見分けと初期化 (0x80 0xFE 0x40) のときから、少しずつしか受け付けない。初めの何回かは 1 バイトも受け付けない
    dev.sendLimit = 1;
    dev.rejectSends = 5;
    CHECK_EQ(fuk.start(1000), 1);
    CHECK_EQ(dev.port1Log.size() >= 4, 1);
    CHECK_EQ(dev.port1Log[0], 0x00);
    CHECK_EQ(dev.port1Log[1], 0x80);
    CHECK_EQ(dev.port1Log[2], 0xfe);
    CHECK_EQ(dev.port1Log[3], 0x40);
    CHECK_EQ(fuk.stats().linkFailures, 0);
    motorOn(dev, fuk);
    for (int i = 0; i < 40; i++) fuk.write(5, (uint8_t)(i & 1)); // 続けて書く
    readTrack(dev, fuk, host, true, 8192);
    fuk.reset();
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.read(1) == 0x80; }) < 1000000, 1);
    runUntil(dev, fuk, [] { return false; }, 20000);
    motorOn(dev, fuk);
    readTrack(dev, fuk, host, true, 8192);
    CHECK_EQ(fuk.stats().timeouts, 0);
    CHECK_EQ(fuk.stats().links, 1);
    CHECK_EQ(dev.overlappedReads, 0);
}

// 読み出しの返事を待っている間に書き込みがたまっても、1 つも落とさない (空くのを待つ)
// 'M' とデータを送り切る前に時間切れになっても、残りを捨てて見分けに進まない (見分けの 0x00 がディスクに書かれる)
// 口が受け付けたバイトが、装置に届くのが遅い (ベアメタル版のリング、OS のバッファ)。'M' とデータを口に渡し終えても、
// 装置へ出きるまではリセットを送らない
void testWriteTailAsync() {
    FakeFuk dev;
    Count host;
    Fuk fuk(dev, &host);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    dev.writeUs = 0;
    dev.asyncSendUs[1] = 30000; // ポート 2 に送ったものは、30 ms たってから装置に届く
    host.count = 2048;
    const uint8_t write[] = {0x45, 0x00, 8, 0, 1, 3, 8, 0x1b, 0xff};
    sendCommand(dev, fuk, write, 9);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.dataReady(); }) < 1000000, 1);
    for (int i = 0; i < 2048; i++) fuk.write(3, (uint8_t)(0x80 | i));
    const unsigned resets = dev.resets;
    fuk.reset();
    runUntil(dev, fuk, [] { return false; }, 10000);
    CHECK_EQ(dev.resets, resets); // データが出きるまで、リセットは届かない
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.read(1) == 0x80; }, 2000000) < 2000000, 1);
    CHECK_EQ(dev.resets, resets + 1);
    CHECK_EQ(dev.port2Unexpected, 0);
    const uint32_t at = 8 * 2 * 8 * 1024;
    CHECK_EQ(dev.image[at], 0x80);
    CHECK_EQ(dev.image[at + 2047], 0xff);
    CHECK_EQ(fuk.stats().timeouts, 0);
    CHECK_EQ(dev.overlappedReads, 0);
}

// 'M' とデータは口から出ていったが、装置の中のバッファに残っていて、装置がポート 1 を先に読む。
// 'w' が届くまでリセットを送らない (先に届くと、残りのデータが通知用の入力として読まれる)
void testWriteTailInDevice() {
    FakeFuk dev;
    Count host;
    Fuk fuk(dev, &host);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    dev.deviceDelay2Us = 30000; // ポート 2 に送ったものは、口からすぐ出るが、装置が読むのは 30 ms たってから
    host.count = 1024;
    const uint8_t write[] = {0x45, 0x00, 10, 0, 1, 3, 1, 0x1b, 0xff};
    sendCommand(dev, fuk, write, 9);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.dataReady(); }) < 1000000, 1);
    for (int i = 0; i < 1024; i++) fuk.write(3, (uint8_t)(0x20 + (i & 0x5f)));
    runUntil(dev, fuk, [] { return false; }, 2000); // 口からは出きった
    const unsigned resets = dev.resets;
    fuk.reset();
    runUntil(dev, fuk, [] { return false; }, 40000); // 装置はデータを読み終えたが、まだ書いている (writeUs = 20 ms)
    CHECK_EQ(dev.resets, resets);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.read(1) == 0x80; }, 2000000) < 2000000, 1);
    CHECK_EQ(dev.resets, resets + 1);
    CHECK_EQ(dev.port2Unexpected, 0);
    const uint32_t at = (10 * 2 + 0) * 8 * 1024;
    bool all = true;
    for (int i = 0; i < 1024; i++) all = all && dev.image[at + (uint32_t)i] == (uint8_t)(0x20 + (i & 0x5f));
    CHECK_EQ(all, 1);
    CHECK_EQ(fuk.stats().timeouts, 0);
}

// 'w' が来ないまま止まった装置は、WRITE_TIMEOUT_US で諦めてリセットを送る
void testWriteDoneTimeout() {
    FakeFuk dev;
    Count host;
    Fuk fuk(dev, &host);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    dev.writeUs = 60000000; // 'w' は来ない
    host.count = 1024;
    const uint8_t write[] = {0x45, 0x00, 11, 0, 1, 3, 1, 0x1b, 0xff};
    sendCommand(dev, fuk, write, 9);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.dataReady(); }) < 1000000, 1);
    for (int i = 0; i < 1024; i++) fuk.write(3, (uint8_t)i);
    runUntil(dev, fuk, [] { return false; }, 2000);
    const unsigned resets = dev.resets;
    fuk.reset();
    runUntil(dev, fuk, [] { return false; }, 1000000);
    CHECK_EQ(dev.resets, resets);
    CHECK_EQ(runUntil(dev, fuk, [&] { return dev.resets != resets; }, 12000000) < 12000000, 1);
    CHECK_EQ(fuk.stats().timeouts, 0); // 待つのをやめただけ (X68000 のリセットで、コマンドはもう終わっている)
}

// リセットを挟まずに 'w' が来ないときは、データが出きってから WRITE_TIMEOUT_US で 1 回だけ時間切れにして、すぐに見分け直す
void testWriteDoneTimeoutBusy() {
    FakeFuk dev;
    Count host;
    Fuk fuk(dev, &host);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    dev.writeUs = 60000000;
    host.count = 1024;
    const uint8_t write[] = {0x45, 0x00, 12, 0, 1, 3, 1, 0x1b, 0xff};
    sendCommand(dev, fuk, write, 9);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.dataReady(); }) < 1000000, 1);
    for (int i = 0; i < 1024; i++) fuk.write(3, (uint8_t)i);
    const uint64_t t = runUntil(dev, fuk, [&] { return fuk.stats().timeouts != 0; }, 30000000);
    CHECK_EQ(t >= Fuk::WRITE_TIMEOUT_US - 100000 && t < Fuk::WRITE_TIMEOUT_US + 100000, 1);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.ready() && fuk.stats().links == 2; }, 2000000) < 2000000, 1);
    runUntil(dev, fuk, [] { return false; }, 12000000);
    CHECK_EQ(fuk.stats().timeouts, 1);
}

// 'M' とデータを送っている途中で、同じ装置との通信が一度切れて戻った (USB の転送のエラー)。送り残しを捨てない
void testWriteTailGlitch() {
    FakeFuk dev;
    Count host;
    Fuk fuk(dev, &host);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    dev.writeUs = 0;
    dev.sendLimit2 = 8;
    host.count = 2048;
    const uint8_t write[] = {0x45, 0x00, 9, 1, 1, 3, 8, 0x1b, 0xff};
    sendCommand(dev, fuk, write, 9);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.dataReady(); }) < 1000000, 1);
    for (int i = 0; i < 2048; i++) fuk.write(3, (uint8_t)(0x40 | (i & 0x3f)));
    runUntil(dev, fuk, [] { return false; }, 1000); // 少しだけ送ったところで切れる
    dev.glitch();
    runUntil(dev, fuk, [] { return false; }, 50000);
    CHECK_EQ(fuk.ready(), 0);
    dev.resume();
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.ready() && fuk.stats().links == 2; }, 5000000) < 5000000, 1);
    CHECK_EQ(dev.port2Unexpected, 0);
    const uint32_t at = (9 * 2 + 1) * 8 * 1024;
    bool all = true;
    for (int i = 0; i < 2048; i++) all = all && dev.image[at + (uint32_t)i] == (uint8_t)(0x40 | (i & 0x3f));
    CHECK_EQ(all, 1);
}

// FUK の答えを待っている間に DMA が異常終了したら、答えが届いたところでオーバーランにして終わる
void testAbortWhileWaiting() {
    FakeFuk dev;
    Count host;
    Fuk fuk(dev, &host);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    uint8_t res[7];
    host.count = 1024;
    const uint8_t read[] = {0x46, 0x00, 0, 0, 1, 3, 1, 0x1b, 0xff};
    sendCommand(dev, fuk, read, 9);
    fuk.abortTransfer(); // 'R' が届く前
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.irq(); }) < 1000000, 1);
    CHECK_EQ(fuk.dataReady(), 0);
    CHECK_EQ(fuk.read(1), 0xd0);
    for (int i = 0; i < 7; i++) res[i] = fuk.read(3);
    CHECK_EQ(res[0] & 0xc0, 0x40);
    CHECK_EQ(res[1] & 0x10, 0x10);
    // 次の Read はふつうに読める
    readTrack(dev, fuk, host, true, 8192);
    // Write: 'W' が届く前に異常終了 → 'W' が届いたところで書き込みをやめ (データは送らない)、オーバーラン
    host.count = 1024;
    const uint8_t write[] = {0x45, 0x00, 11, 0, 1, 3, 1, 0x1b, 0xff};
    const std::vector<uint8_t> before = dev.image;
    sendCommand(dev, fuk, write, 9);
    fuk.abortTransfer();
    const uint64_t t = runUntil(dev, fuk, [&] { return fuk.irq(); });
    CHECK_EQ(t < Fuk::DMA_TIMEOUT_US / 2, 1);
    for (int i = 0; i < 7; i++) res[i] = fuk.read(3);
    CHECK_EQ(res[0] & 0xc0, 0x40);
    CHECK_EQ(res[1] & 0x10, 0x10);
    CHECK_EQ(fuk.stats().cancelledWrites, 1);
    runUntil(dev, fuk, [] { return false; }, 100000);
    CHECK_EQ(dev.image == before, 1); // ディスクには何も書いていない
    CHECK_EQ(dev.port2Unexpected, 0);
}

void testWriteTailSlow() {
    FakeFuk dev;
    Count host;
    Fuk fuk(dev, &host);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    dev.sendLimit2 = 1; // ポート 2 は 1 回に 1 バイトしか受け付けない
    dev.writeUs = 0;
    host.count = 8192;
    const uint8_t write[] = {0x45, 0x00, 7, 0, 1, 3, 8, 0x1b, 0xff};
    sendCommand(dev, fuk, write, 9);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.dataReady(); }) < 1000000, 1);
    for (int i = 0; i < 8192; i++) fuk.write(3, 0xaa);
    // 2 ms に 1 回しか送れない: 送り切るのに 16 秒かかり、その前に 'w' を待つ時間 (10 秒) が切れる
    runUntil(dev, fuk, [&] { return false; }, 30000000, 2000);
    CHECK_EQ(fuk.stats().timeouts, 1);
    CHECK_EQ(dev.port2Unexpected, 0);
    const uint32_t at = 7 * 2 * 8 * 1024;
    bool all = true;
    for (int i = 0; i < 8192; i++) all = all && dev.image[at + (uint32_t)i] == 0xaa;
    CHECK_EQ(all, 1);
    // X68000 には異常終了を返していて、送り切ったあとはまた使える
    CHECK_EQ(fuk.read(1), 0xd0);
    CHECK_EQ(fuk.read(3), 0x48);
    for (int i = 1; i < 7; i++) fuk.read(3);
    dev.sendLimit2 = 0;
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.ready() && fuk.read(1) == 0x80; }, 5000000) < 5000000, 1);
    CHECK_EQ(fuk.stats().links, 2);
    CHECK_EQ(dev.overlappedReads, 0);
}

// SEEK の途中で抜いたら、こちらで異常終了のシークの終わりを作る (X68000 が割り込みを待ち続けない)
void testSeekUnplug() {
    FakeFuk dev;
    Fuk fuk(dev);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    dev.seekUs = 500000;
    const uint8_t seek[] = {0x0f, 0x00, 30};
    const uint8_t sis[] = {0x08};
    sendCommand(dev, fuk, seek, 3);
    runUntil(dev, fuk, [] { return false; }, 5000);
    CHECK_EQ(fuk.irq(), 0);
    dev.unplug();
    fuk.poll();
    CHECK_EQ(fuk.irq(), 1);
    // 挿し直して、つながったあとに引き取っても、こちらで作った結果を返す
    dev.plug();
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.ready() && fuk.read(1) == 0x80; }) < 1000000, 1);
    CHECK_EQ(fuk.irq(), 1);
    const size_t commands = dev.commandLog.size();
    command(fuk, sis, 1);
    CHECK_EQ(fuk.read(1), 0xd0);
    CHECK_EQ(fuk.read(3), 0x68);
    CHECK_EQ(fuk.read(3), 0x00);
    CHECK_EQ(fuk.irq(), 0);
    CHECK_EQ(dev.commandLog.size(), commands); // FUK には送っていない
    // 返事が来なくなったとき (時間切れ → つなぎ直し) も同じ
    motorOn(dev, fuk);
    sendCommand(dev, fuk, seek, 3);
    runUntil(dev, fuk, [] { return false; }, 5000);
    dev.silent = true;
    const uint8_t sds[] = {0x04, 0x00};
    command(fuk, sds, 2);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.irq(); }) < 1000000, 1);
    dev.silent = false;
    CHECK_EQ(fuk.read(3), 0x00); // SENSE DEVICE STATUS の、こちらで作った結果
    command(fuk, sis, 1);
    CHECK_EQ(fuk.read(3), 0x68);
    CHECK_EQ(fuk.read(3), 0x00);
}

void testQueueFull() {
    FakeFuk dev;
    Fuk fuk(dev);
    CHECK_EQ(fuk.start(1000), 1);
    dev.latencyUs = 20000; // 返事が遅い
    size_t before = 0;
    for (uint8_t b : dev.port1Log) before += b == 0x85;
    fuk.write(5, 0x01); // 状態の読み出しも送る → その返事が届くまで、次の書き込みは並ぶ
    for (int i = 0; i < 1000; i++) fuk.write(5, (uint8_t)(i & 1 ? 0x01 : 0x00));
    runUntil(dev, fuk, [] { return false; }, 200000);
    size_t writes = 0;
    for (size_t i = 0; i + 1 < dev.port1Log.size(); i++) writes += dev.port1Log[i] == 0x85;
    CHECK_EQ(writes - before >= 1001, 1); // 値の 0x85 はないので、数は書き込みの数 (読み出しの 0x05 は数えない)
    CHECK_EQ(fuk.stats().queueWaits != 0, 1);
    CHECK_EQ(fuk.stats().timeouts, 0);
    CHECK_EQ(fuk.stats().links, 1);
    CHECK_EQ(dev.overlappedReads, 0);
    // 返事がまったく来ないときは、待つ上限 (1 回) で諦めて、見分けからやり直す (書き込みごとに待ち直さない)
    dev.silent = true;
    const uint64_t waited = fuk.stats().waitUs;
    for (int i = 0; i < 2000; i++) fuk.write(5, (uint8_t)(i & 1));
    CHECK_EQ(fuk.stats().timeouts != 0, 1);
    CHECK_EQ(fuk.stats().waitUs - waited <= 2 * Fuk::WAIT_US, 1);
    dev.silent = false;
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.ready() && fuk.read(1) == 0x80; }, 5000000) < 5000000, 1);
}

// 書き込みのデータを受け取っている途中で DMA が異常終了したとき、抜いたとき
void testWriteAbort() {
    FakeFuk dev;
    Count host;
    Fuk fuk(dev, &host);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    host.count = 1024;
    const uint8_t write[] = {0x45, 0x00, 6, 0, 1, 3, 1, 0x1b, 0xff};
    uint8_t res[7];
    // DMA の異常終了 (バスエラー、ソフトウェアアボート): 書き込みをやめる。そろっていないデータは送らず、
    // FUK の FDC をリセットして、リザルトはオーバーラン。ディスクの中身は変えない
    const std::vector<uint8_t> before = dev.image;
    const unsigned resets = dev.resets;
    sendCommand(dev, fuk, write, 9);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.dataReady(); }) < 1000000, 1);
    for (int i = 0; i < 100; i++) fuk.write(3, 0x99);
    fuk.abortTransfer();
    CHECK_EQ(fuk.dataReady(), 0);
    CHECK_EQ(fuk.irq(), 1);
    CHECK_EQ(fuk.read(1), 0xd0);
    for (int i = 0; i < 7; i++) res[i] = fuk.read(3);
    CHECK_EQ(res[0] & 0xc0, 0x40);
    CHECK_EQ(res[1] & 0x10, 0x10);
    const uint32_t at = 6 * 2 * 8 * 1024;
    runUntil(dev, fuk, [] { return false; }, 100000);
    CHECK_EQ(dev.image == before, 1);
    CHECK_EQ(dev.resets, resets + 1);
    CHECK_EQ(fuk.stats().cancelledWrites, 1);
    CHECK_EQ(dev.port2Unexpected, 0);
    // 転送が、知らせたバイト数より少ないところで終わったとき (TC) も、書かせない
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.read(1) == 0x80; }) < 1000000, 1);
    sendCommand(dev, fuk, write, 9);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.dataReady(); }) < 1000000, 1);
    for (int i = 0; i < 512; i++) fuk.write(3, 0x77);
    fuk.terminalCount();
    for (int i = 0; i < 7; i++) res[i] = fuk.read(3);
    CHECK_EQ(res[0] & 0xc0, 0x40);
    runUntil(dev, fuk, [] { return false; }, 100000);
    CHECK_EQ(dev.image == before, 1);
    CHECK_EQ(fuk.stats().cancelledWrites, 2);
    // やめたあとは、モーターを回し直さなくても書ける (リセットのあとに、ドライブ選択を送り直している)
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.read(1) == 0x80; }) < 1000000, 1);
    sendCommand(dev, fuk, write, 9);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.dataReady(); }) < 1000000, 1);
    for (int i = 0; i < 1024; i++) fuk.write(3, 0x22);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.irq(); }) < 1000000, 1);
    for (int i = 0; i < 7; i++) res[i] = fuk.read(3);
    CHECK_EQ(res[0], 0x00);
    CHECK_EQ(dev.image[at + 1023], 0x22);
    // データを受け取っている途中で抜く: すぐに異常終了を返し、挿し直せばまた書ける
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.read(1) == 0x80; }) < 1000000, 1);
    sendCommand(dev, fuk, write, 9);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.dataReady(); }) < 1000000, 1);
    for (int i = 0; i < 100; i++) fuk.write(3, 0x55);
    dev.unplug();
    fuk.poll();
    CHECK_EQ(fuk.dataReady(), 0);
    CHECK_EQ(fuk.read(1), 0xd0);
    CHECK_EQ(fuk.read(3), 0x48);
    for (int i = 1; i < 7; i++) fuk.read(3);
    dev.plug();
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.ready() && fuk.read(1) == 0x80; }) < 1000000, 1);
    motorOn(dev, fuk);
    sendCommand(dev, fuk, write, 9);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.dataReady(); }) < 1000000, 1);
    for (int i = 0; i < 1024; i++) fuk.write(3, 0x66);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.irq(); }) < 1000000, 1);
    for (int i = 0; i < 7; i++) res[i] = fuk.read(3);
    CHECK_EQ(res[0], 0x00);
    CHECK_EQ(dev.image[at + 1023], 0x66);
    // ほかのスレッドに見せる数 (snapshot()) は、poll() のあとで今の数と同じになる
    for (int i = 0; i < 200; i++) fuk.poll();
    CHECK_EQ(fuk.snapshot().writeFrames, fuk.stats().writeFrames);
    CHECK_EQ(fuk.snapshot().links, 2);
    CHECK_EQ(fuk.readyShared(), 1);
}

void testWrite() {
    FakeFuk dev;
    Count host;
    Fuk fuk(dev, &host);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    host.count = 2048;
    const uint8_t write[] = {0x45, 0x04, 3, 1, 2, 3, 8, 0x1b, 0xff}; // C3 H1 R2 から
    sendCommand(dev, fuk, write, 9);
    CHECK_EQ(fuk.dataReady(), 0);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.dataReady(); }) < 1000000, 1);
    for (int i = 0; i < 2048; i++) fuk.write(3, (uint8_t)(i ^ 0x5a));
    fuk.terminalCount();
    CHECK_EQ(fuk.dataReady(), 0);
    CHECK_EQ(fuk.read(1), 0x10);
    CHECK_EQ(fuk.irq(), 0);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.irq(); }) < 1000000, 1);
    CHECK_EQ(fuk.read(1), 0xd0);
    uint8_t res[7];
    for (int i = 0; i < 7; i++) res[i] = fuk.read(3);
    CHECK_EQ(res[0], 0x04);
    CHECK_EQ(res[5], 3);
    const uint32_t at = ((3 * 2 + 1) * 8 + 1) * 1024;
    for (int i = 0; i < 2048; i++) CHECK_EQ(dev.image[at + (uint32_t)i], (uint8_t)(i ^ 0x5a));
    // 転送バイト数を、コマンドの前に 0xB0 / 0xB1 で知らせている
    bool told = false;
    for (size_t i = 0; i + 3 < dev.port1Log.size(); i++)
        if (dev.port1Log[i] == 0xb0 && dev.port1Log[i + 1] == 0x00 && dev.port1Log[i + 2] == 0xb1 && dev.port1Log[i + 3] == 0x08)
            told = true;
    CHECK_EQ(told, 1);

    // WRITE ID: セクタの ID を 4 バイトずつ送る
    host.count = 32;
    const uint8_t format[] = {0x4d, 0x00, 3, 8, 0x54, 0xe5};
    sendCommand(dev, fuk, format, 6);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.dataReady(); }) < 1000000, 1);
    for (int r = 1; r <= 8; r++) {
        fuk.write(3, 10);
        fuk.write(3, 0);
        fuk.write(3, (uint8_t)r);
        fuk.write(3, 3);
    }
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.irq(); }) < 1000000, 1);
    for (int i = 0; i < 7; i++) res[i] = fuk.read(3);
    CHECK_EQ(res[0], 0x00);
    CHECK_EQ(dev.image[(10 * 2 * 8) * 1024 + 100], 0xe5);

    // 書き込み禁止: リザルトは FUK のものをそのまま返す
    dev.writeProtected = true;
    host.count = 1024;
    sendCommand(dev, fuk, write, 9);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.dataReady(); }) < 1000000, 1);
    for (int i = 0; i < 1024; i++) fuk.write(3, 0);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.irq(); }) < 1000000, 1);
    for (int i = 0; i < 7; i++) res[i] = fuk.read(3);
    CHECK_EQ(res[0], 0x44);
    CHECK_EQ(res[1], 0x02);
    dev.writeProtected = false;

    // レディでないドライブ: 'W' なしで 'w' が届く
    const uint8_t write1[] = {0x45, 0x01, 0, 0, 1, 3, 8, 0x1b, 0xff};
    sendCommand(dev, fuk, write1, 9);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.irq(); }) < 1000000, 1);
    CHECK_EQ(fuk.read(3), 0x49);
    for (int i = 1; i < 7; i++) fuk.read(3);

    // X68000 を止めていた間 (poll() が呼ばれない) は、時間切れを数えない
    host.count = 1024;
    {
        const uint8_t write9[] = {0x45, 0x00, 9, 0, 1, 3, 1, 0x1b, 0xff};
        sendCommand(dev, fuk, write9, 9);
        CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.dataReady(); }) < 1000000, 1);
        dev.advance(30000000);
        fuk.poll();
        CHECK_EQ(fuk.dataReady(), 1);
        CHECK_EQ(fuk.ready(), 1);
        for (int i = 0; i < 1024; i++) fuk.write(3, 0x77);
        CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.irq(); }) < 1000000, 1);
        for (int i = 0; i < 7; i++) res[i] = fuk.read(3);
        CHECK_EQ(res[0], 0x00);
        CHECK_EQ(dev.image[9 * 2 * 8 * 1024 + 1023], 0x77);
        CHECK_EQ(fuk.stats().cancelledWrites, 0);
    }
    // Read を待っている間に止められても、時間切れにしない (止められていた時間は数えない)
    {
        host.count = 1024;
        const uint8_t read9[] = {0x46, 0x00, 9, 0, 1, 3, 1, 0x1b, 0xff};
        sendCommand(dev, fuk, read9, 9);
        dev.advance(5000000);
        CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.dataReady(); }) < 1000000, 1);
        static uint8_t got[1024];
        CHECK_EQ(fuk.readData(got, 1024), 1024);
        CHECK_EQ(got[1023], 0x77);
        for (int i = 0; i < 7; i++) res[i] = fuk.read(3);
        CHECK_EQ(res[0], 0x00);
        CHECK_EQ(fuk.stats().timeouts, 0);
        CHECK_EQ(fuk.stats().links, 1);
    }
    // DMAC がデータを渡してこない: DMA_TIMEOUT_US で書き込みをやめ、リザルトはオーバーランにする。ディスクには書かない
    host.count = 1024;
    const uint8_t write0[] = {0x45, 0x00, 0, 0, 1, 3, 1, 0x1b, 0xff};
    const std::vector<uint8_t> before0 = dev.image;
    sendCommand(dev, fuk, write0, 9);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.dataReady(); }) < 1000000, 1);
    fuk.write(3, 0xaa);
    const uint64_t t = runUntil(dev, fuk, [&] { return fuk.irq(); });
    CHECK_EQ(t >= Fuk::DMA_TIMEOUT_US && t < Fuk::DMA_TIMEOUT_US + 200000, 1);
    for (int i = 0; i < 7; i++) res[i] = fuk.read(3);
    CHECK_EQ(res[0] & 0xc0, 0x40);
    CHECK_EQ(res[1] & 0x10, 0x10);
    CHECK_EQ(fuk.stats().cancelledWrites, 1);
    runUntil(dev, fuk, [] { return false; }, 100000);
    CHECK_EQ(dev.image == before0, 1);
    CHECK_EQ(fuk.stats().waits, 0);
    CHECK_EQ(dev.overlappedReads, 0);
}

// DMA を使わない書き込みは FUK に送らず、こちらで異常終了にする (データなしで実物のディスクに書かせない)
void testNonDmaWrite() {
    FakeFuk dev;
    Count host;
    Fuk fuk(dev, &host);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    const std::vector<uint8_t> before = dev.image;
    const uint8_t write[] = {0x45, 0x00, 0, 0, 1, 3, 1, 0x1b, 0xff};
    const uint8_t format[] = {0x4d, 0x00, 3, 8, 0x54, 0xe5};
    // DMAC の転送バイト数が 0
    host.count = 0;
    command(fuk, write, 9);
    CHECK_EQ(fuk.read(1), 0xd0);
    CHECK_EQ(fuk.irq(), 1);
    uint8_t res[7];
    for (int i = 0; i < 7; i++) res[i] = fuk.read(3);
    CHECK_EQ(res[0], 0x40);
    CHECK_EQ(res[1], 0x10);
    CHECK_EQ(res[5], 1);
    command(fuk, format, 6);
    CHECK_EQ(fuk.read(1), 0xd0);
    for (int i = 0; i < 7; i++) res[i] = fuk.read(3);
    CHECK_EQ(res[0], 0x40);
    // SPECIFY の ND = 1 (DMAC に前の転送のバイト数が残っていても)
    const uint8_t specifyNd[] = {0x03, 0xd0, 0x11};
    const uint8_t specify0[] = {0x03, 0xd0, 0x10};
    sendCommand(dev, fuk, specifyNd, 3);
    host.count = 1024;
    command(fuk, write, 9);
    CHECK_EQ(fuk.read(1), 0xd0);
    for (int i = 0; i < 7; i++) res[i] = fuk.read(3);
    CHECK_EQ(res[0], 0x40);
    CHECK_EQ(res[1], 0x10);
    runUntil(dev, fuk, [] { return false; }, 50000);
    CHECK_EQ(dev.writeCommands, 0); // FUK には届いていない
    CHECK_EQ(dev.image == before, 1);
    CHECK_EQ(fuk.stats().refusedWrites, 3);
    // DMAC がチェーンかコンティニューで動いている (転送バイト数は最初のブロックの分)
    sendCommand(dev, fuk, specify0, 3);
    host.single = false;
    command(fuk, write, 9);
    CHECK_EQ(fuk.read(1), 0xd0);
    for (int i = 0; i < 7; i++) res[i] = fuk.read(3);
    CHECK_EQ(res[0], 0x40);
    CHECK_EQ(res[1], 0x10);
    host.single = true;
    CHECK_EQ(fuk.stats().refusedWrites, 4);
    CHECK_EQ(fuk.stats().cancelledWrites, 0);
    // ND を戻せば書ける
    const uint8_t specify[] = {0x03, 0xd0, 0x10};
    sendCommand(dev, fuk, specify, 3);
    sendCommand(dev, fuk, write, 9);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.dataReady(); }) < 1000000, 1);
    for (int i = 0; i < 1024; i++) fuk.write(3, 0x33);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.irq(); }) < 1000000, 1);
    for (int i = 0; i < 7; i++) res[i] = fuk.read(3);
    CHECK_EQ(res[0], 0x00);
    CHECK_EQ(dev.image[1023], 0x33);
    CHECK_EQ(dev.port2Unexpected, 0);
}

// 'M' とデータを送っている途中でリセットしても、つなぎ直しになっても、最後まで送ってから次へ進む
// (残りを送らないと、FUK はそのあとポート 2 に届いたものをデータとして読む)
void testWriteTail() {
    for (int how = 0; how < 2; how++) {
        FakeFuk dev;
        Count host;
        Fuk fuk(dev, &host);
        CHECK_EQ(fuk.start(1000), 1);
        motorOn(dev, fuk);
        dev.sendLimit2 = 16; // ポート 2 は少しずつしか受け付けない
        dev.writeUs = 0;     // データがそろったらすぐ書く (Pico のファームと同じ)
        host.count = 2048;
        const uint8_t write[] = {0x45, 0x00, 5, 0, 1, 3, 8, 0x1b, 0xff};
        sendCommand(dev, fuk, write, 9);
        CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.dataReady(); }) < 1000000, 1);
        for (int i = 0; i < 2048; i++) fuk.write(3, (uint8_t)(0x80 | i));
        const unsigned resets = dev.resets;
        if (how == 0) {
            fuk.reset(); // X68000 のリセット
        } else {
            dev.silent = true; // 'w' が来ない → 時間切れ → つなぎ直し
            dev.sendLimit2 = 1;
            dev.writeUs = 60000000;
        }
        if (how == 1) {
            runUntil(dev, fuk, [&] { return fuk.stats().timeouts != 0; }, 20000000, 1000);
            dev.silent = false;
            // やめた書き込みは、こちらで作った異常終了で終わる
            CHECK_EQ(fuk.read(1), 0xd0);
            CHECK_EQ(fuk.read(3), 0x48);
            for (int i = 1; i < 7; i++) fuk.read(3);
        }
        CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.ready() && fuk.read(1) == 0x80; }, 20000000) < 20000000, 1);
        CHECK_EQ(dev.port2Unexpected, 0);
        if (how == 0) {
            CHECK_EQ(dev.resets, resets + 1);
            // データは全部届いていて、書かれている
            const uint32_t at = 5 * 2 * 8 * 1024;
            CHECK_EQ(dev.image[at], 0x80);
            CHECK_EQ(dev.image[at + 2047], 0xff);
        }
        CHECK_EQ(dev.overlappedReads, 0);
    }
}

void testReadId() {
    FakeFuk dev;
    Fuk fuk(dev);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    const uint8_t readId[] = {0x4a, 0x00};
    sendCommand(dev, fuk, readId, 2);
    CHECK_EQ(fuk.read(1), 0x10);
    // 割り込みは、リザルトがそろってから立てる (FUK の 'C' は、こちらがリザルトを読むと下りる)
    const uint64_t t = runUntil(dev, fuk, [&] { return fuk.irq(); });
    CHECK_EQ(t < 100000, 1);
    CHECK_EQ(fuk.read(1), 0xd0);
    uint8_t res[7];
    for (int i = 0; i < 7; i++) {
        CHECK_EQ(fuk.irq(), i == 0);
        res[i] = fuk.read(3);
    }
    CHECK_EQ(res[0], 0x00);
    CHECK_EQ(res[5], 1);
    CHECK_EQ(res[6], 3);
    CHECK_EQ(runUntil(dev, fuk, [&] { return !fuk.irq() && fuk.read(1) == 0x80; }) < 100000, 1);
    // 分からないコマンド: ステータスを見ながら読む (ST0 = 0x80 の 1 バイト)
    const uint8_t bad[] = {0x1f};
    sendCommand(dev, fuk, bad, 1);
    readResult(dev, fuk, res, 1);
    CHECK_EQ(res[0], 0x80);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.read(1) == 0x80; }) < 100000, 1);
    CHECK_EQ(fuk.stats().waits, 0);
    CHECK_EQ(dev.overlappedReads, 0);
}

void testDrive() {
    FakeFuk dev;
    Fuk fuk(dev);
    CHECK_EQ(fuk.start(1000), 1);
    // ドライブ制御を書いてすぐに読むと、覚えた値がないので返事を待つ (1 回だけ)
    fuk.write(5, 0x01);
    CHECK_EQ(fuk.read(5), 0x80);
    CHECK_EQ(fuk.stats().waits, 1);
    // 続けて読んでも、時間がたってから読んでも、覚えた値を返す (古ければ、返しつつ読み直す)
    fuk.write(5, 0x00);
    fuk.write(5, 0x01);
    CHECK_EQ(fuk.read(5), 0x80);
    runUntil(dev, fuk, [] { return false; }, 200000);
    const uint32_t reads = fuk.stats().reads;
    dev.image.clear(); // ディスクを抜いた (通知なし)
    fuk.write(5, 0x01);
    CHECK_EQ(fuk.read(5), 0x80);
    CHECK_EQ(fuk.stats().reads, reads + 1);
    runUntil(dev, fuk, [] { return false; }, 5000);
    CHECK_EQ(fuk.read(5), 0x00);
    CHECK_EQ(fuk.stats().waits, 1);
    // 別のドライブを選んで、少したってから読む: 先に送った読み出しの返事が届いていて、待たない
    fuk.write(5, 0x02);
    runUntil(dev, fuk, [] { return false; }, 5000);
    CHECK_EQ(fuk.read(5), 0x00);
    CHECK_EQ(fuk.stats().waits, 1);
    CHECK_EQ(fuk.read(5 + 8), 0x00); // アドレスは 8 バイトごとに繰り返す
    CHECK_EQ(dev.overlappedReads, 0);
}

void testForceReady() {
    FakeFuk dev;
    Fuk fuk(dev);
    CHECK_EQ(fuk.start(1000), 1);
    const size_t before = dev.port1Log.size();
    fuk.setForceReady(true);
    fuk.setForceReady(true);
    runUntil(dev, fuk, [] { return false; }, 5000);
    CHECK_EQ(dev.port1Log.size(), before); // 立ったときは送らない
    const uint8_t recal[] = {0x07, 0x00};
    sendCommand(dev, fuk, recal, 2);
    runUntil(dev, fuk, [] { return false; }, 5000);
    // RECALIBRATE の直前に 0xA3 0x40
    bool sent = false;
    for (size_t i = before; i + 3 < dev.port1Log.size(); i++)
        if (dev.port1Log[i] == 0xa3 && dev.port1Log[i + 1] == 0x40 && dev.port1Log[i + 2] == 0x83 && dev.port1Log[i + 3] == 0x07)
            sent = true;
    CHECK_EQ(sent, 1);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.irq(); }) < 100000, 1);
    const uint8_t sis[] = {0x08};
    sendCommand(dev, fuk, sis, 1);
    uint8_t res[2];
    readResult(dev, fuk, res, 2);
    CHECK_EQ(res[0], 0x20); // モーターを回していなくても、強制レディでシークできる
    const size_t mid = dev.port1Log.size();
    fuk.setForceReady(false);
    runUntil(dev, fuk, [] { return false; }, 5000);
    bool off = false;
    for (size_t i = mid; i + 1 < dev.port1Log.size(); i++)
        if (dev.port1Log[i] == 0xa3 && dev.port1Log[i + 1] == 0x00) off = true;
    CHECK_EQ(off, 1);
    // I/O 割り込みのレジスタへの書き込みも伝える
    fuk.writeIoc(1, 0x04);
    fuk.writeIoc(3, 0x60);
    fuk.writeIoc(2, 0xff); // 偶数番地にはレジスタがない
    runUntil(dev, fuk, [] { return false; }, 5000);
    const size_t n = dev.port1Log.size();
    CHECK_EQ(dev.port1Log[n - 4], 0x91);
    CHECK_EQ(dev.port1Log[n - 3], 0x04);
    CHECK_EQ(dev.port1Log[n - 2], 0x93);
    CHECK_EQ(dev.port1Log[n - 1], 0x60);
}

void testTimeoutAndUnplug() {
    FakeFuk dev;
    Count host;
    Fuk fuk(dev, &host);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    // 'R' が来ない: READ_TIMEOUT_US たったら、こちらで異常終了 (ノットレディ) のリザルトを作る
    dev.noReadFrame = true;
    host.count = 1024;
    const uint8_t read[] = {0x46, 0x00, 0, 0, 1, 3, 1, 0x1b, 0xff};
    sendCommand(dev, fuk, read, 9);
    const uint64_t t = runUntil(dev, fuk, [&] { return fuk.irq(); });
    CHECK_EQ(t >= Fuk::READ_TIMEOUT_US && t < Fuk::READ_TIMEOUT_US + 100000, 1);
    CHECK_EQ(fuk.read(1), 0xd0);
    uint8_t res[7];
    for (int i = 0; i < 7; i++) res[i] = fuk.read(3);
    CHECK_EQ(res[0], 0x48);
    CHECK_EQ(res[5], 1);
    CHECK_EQ(fuk.stats().timeouts, 1);
    // 見分けからやり直して、また使える
    dev.noReadFrame = false;
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.ready() && fuk.read(1) == 0x80; }) < 1000000, 1);
    CHECK_EQ(fuk.stats().links, 2);
    motorOn(dev, fuk);
    readTrack(dev, fuk, host, true, 8192);

    // ポート 1 の返事が来ない
    dev.silent = true;
    const uint8_t sis[] = {0x08};
    const uint8_t sds[] = {0x04, 0x00};
    command(fuk, sds, 2);
    // ステータスを読むたびに少し待つ (yieldForReply()) ので、装置の時計で測る
    const uint64_t from2 = dev.nowUs();
    runUntil(dev, fuk, [&] { return fuk.read(1) == 0xd0; });
    const uint64_t t2 = dev.nowUs() - from2;
    CHECK_EQ(t2 >= Fuk::REPLY_TIMEOUT_US && t2 < Fuk::REPLY_TIMEOUT_US + 100000, 1);
    CHECK_EQ(fuk.read(3), 0x00);
    dev.silent = false;
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.ready() && fuk.read(1) == 0x80; }) < 2000000, 1);

    // Read を待っている間に抜く: すぐに異常終了を返す
    motorOn(dev, fuk);
    sendCommand(dev, fuk, read, 9);
    dev.unplug();
    fuk.poll();
    CHECK_EQ(fuk.ready(), 0);
    CHECK_EQ(fuk.irq(), 1);
    CHECK_EQ(fuk.read(1), 0xd0);
    for (int i = 0; i < 7; i++) res[i] = fuk.read(3);
    CHECK_EQ(res[0], 0x48);
    // つながっていない間: ドライブがレディにならない FDC
    CHECK_EQ(fuk.read(1), 0x80);
    const uint8_t recal[] = {0x07, 0x00};
    command(fuk, recal, 2);
    CHECK_EQ(fuk.irq(), 1);
    command(fuk, sis, 1);
    CHECK_EQ(fuk.read(1), 0xd0);
    CHECK_EQ(fuk.read(3), 0x68);
    CHECK_EQ(fuk.read(3), 0x00);
    CHECK_EQ(fuk.irq(), 0);
    command(fuk, read, 9);
    CHECK_EQ(fuk.read(1), 0xd0);
    CHECK_EQ(fuk.read(3), 0x48);
    for (int i = 1; i < 7; i++) fuk.read(3);
    fuk.write(5, 0x01);
    CHECK_EQ(fuk.read(5), 0x00);
    // 挿し直す
    dev.plug();
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.ready() && fuk.read(1) == 0x80; }) < 1000000, 1);
    motorOn(dev, fuk);
    readTrack(dev, fuk, host, true, 8192);
    CHECK_EQ(dev.overlappedReads, 0);
}

void testReset() {
    FakeFuk dev;
    Count host;
    Fuk fuk(dev, &host);
    CHECK_EQ(fuk.start(1000), 1);
    motorOn(dev, fuk);
    const unsigned resets = dev.resets;
    // Read の途中と、読み出しの返事を待っている途中でリセットする
    host.count = 8192;
    const uint8_t read[] = {0x46, 0x00, 0, 0, 1, 3, 8, 0x1b, 0xff};
    sendCommand(dev, fuk, read, 9);
    runUntil(dev, fuk, [] { return false; }, 3000);
    fuk.reset();
    CHECK_EQ(fuk.irq(), 0);
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.read(1) == 0x80; }) < 1000000, 1);
    CHECK_EQ(dev.resets, resets + 1);
    const uint8_t sis[] = {0x08};
    command(fuk, sis, 1);
    fuk.reset();
    CHECK_EQ(runUntil(dev, fuk, [&] { return fuk.read(1) == 0x80; }) < 1000000, 1);
    // リセットのあとも読める (前の 'R' の残りや返事を取り違えない)
    runUntil(dev, fuk, [] { return false; }, 20000);
    motorOn(dev, fuk);
    readTrack(dev, fuk, host, true, 8192);
    CHECK_EQ(fuk.stats().timeouts, 0);
    CHECK_EQ(dev.overlappedReads, 0);
}

} // namespace

int main() {
    testIdentify();
    testSeek();
    testRead();
    testPartialSend();
    testWrite();
    testNonDmaWrite();
    testWriteTail();
    testWriteTailAsync();
    testStreamedRead();
    testStreamedReadAbort();
    testStreamedReadStall();
    testStreamedReadBursty();
    testStreamedReadFixedLead();
    testStreamedReadLate();
    testBurstFastRead();
    testWriteTailGlitch();
    testWriteTailInDevice();
    testWriteDoneTimeout();
    testWriteDoneTimeoutBusy();
    testAbortWhileWaiting();
    testWriteTailSlow();
    testSeekUnplug();
    testQueueFull();
    testWriteAbort();
    testReadId();
    testDrive();
    testForceReady();
    testTimeoutAndUnplug();
    testReset();
    return finish();
}
