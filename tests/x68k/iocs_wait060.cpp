// 060turbo (68060) で X68030 の IPL ROM (IPLROM30.DAT) を使うとき、IPL ROM が CACR に書く 68030 の値が、68060 の CACR の
// 値 (EIC = ビット 15、EDC = ビット 31) に読み替えられているかを確かめる (x68k/machine.cpp の patchIplFor060)。
// 見るのは次の 4 つ (括弧の中は、書き換える IPL ROM の場所):
// - 起動時 ($FF014E): 電源を入れてから CACR が初めて 0 でなくなったときの値が $80008000
//   (このあと IPL ROM は SRAM の $ED0090 に従って _SYS_STAT の設定を通り、CACR を書き直す。空の SRAM では同じ値になるので、
//   起動が終わったあとの値では $FF014E の書き換えを確かめられない。そのため、初めて書かれた値を見る)
// - IOCS _SYS_STAT ($AC) の設定 (d1 = 4、d2 = 0 から 3。$FFC7CC): 呼んだあとの CACR が $00000000 / $00008000 / $80000000 / $80008000
// - IOCS _SYS_STAT の取得 (d1 = 1。$FFC78C): 上の設定のあとで d2 と同じ値を返す。CACR を直接 $A0808000 (すべて有効) にすると 3、
//   $20800000 (ストアバッファと分岐キャッシュだけ) にすると 0
// - IOCS _MS_ONTM ($79) の待ちのループ ($FFAC82): 回っている間の CACR が $80008000 で、戻ったあとは呼ぶ前の値
// _MS_ONTM は、マウスのボタンが押されるのを d2 単位まで待つ。1 単位は、IOCS のワークの $0CB8 (ROM の上の dbra のループの
// 1ms あたりの回数) x 100 / 834 周のループで、X68030 では約 1ms。060turbo では、patchIplFor060 が 834 を 583 に書き換えて
// 1 単位を 1ms にする (ループの 1 周のサイクル数が違うため)。待ちの長さは表示し、--min / --max があれば範囲を確かめる
// (ctest は 950 から 1050)。
// IPL ROM がディスクを待っている間に、RAM に小さなプログラムを書いて CPU をそこに飛ばす。
//   iocs_wait060 [--ipl IPLROM30.DAT] [--min ms] [--max ms] [--clock 50|75]
//                [--any-rom [--rom30 ROM30_060.DAT] [--loop-cacr hex]]
// --any-rom: X68030 の IPL ROM 1.3 でなくても動かす (IPLROM060.DAT など。patchIplFor060 は何もしない)。CACR の読み替えと
// $0CB8 の値は確かめず、$0CB8 と _MS_ONTM の待ちの長さと、待ちのループの間の CACR を出す (--min / --max は確かめる)。
// --rom30 は $FC0000 からの ROM。--loop-cacr を付けると、待ちのループの間の CACR がその値か (と、ループの中で 100 回より
// 多く見られたか) も確かめる
// --min / --max は、1000 単位あたりの長さ (ミリ秒) の許す範囲 (既定は確かめない)。
// ROM (--rom30 を付けたときはそれも) がなければ 77 (ctest の SKIP_RETURN_CODE)。--any-rom がなく、X68030 の IPL ROM 1.3 で
// なければ 77。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "x68k/machine.h"

namespace {

const uint32_t kCode = 0x0ff000;
const uint32_t kStack = 0x0ff000;
// 結果: +0 待ちを始めたら 1、+4 _MS_ONTM の d0、+8 終わったら 1、+12 呼ぶ前の CACR、+16 呼んだあとの CACR、
// +$20 + 8n 設定 (d2 = n) のあとの CACR、+$24 + 8n そのあとの取得の d0、+$40 CACR = $A0808000 での取得の d0、
// +$44 CACR = $20800000 での取得の d0
const uint32_t kResult = 0x100100;
const int kUnits = 500;
const uint32_t kCacrBeforeWait = 0xa0808000; // 待ちの前の CACR (待ちの間の $80008000 と違う値にする)

uint16_t g_program[160];
size_t g_len = 0;

void w(uint16_t v) { g_program[g_len++] = v; }
void l(uint32_t v) {
    w((uint16_t)(v >> 16));
    w((uint16_t)v);
}
void storeD0(uint32_t addr) { // move.l d0,addr
    w(0x23c0);
    l(addr);
}
void setCacr(uint32_t v) { // move.l #v,d0 / movec d0,cacr
    w(0x203c);
    l(v);
    w(0x4e7b), w(0x0002);
}
void sysStat(int d1, int d2) { // move.l #$ac,d0 / moveq #d1,d1 / moveq #d2,d2 / trap #15
    w(0x203c);
    l(0xac);
    w((uint16_t)(0x7200 | d1));
    w((uint16_t)(0x7400 | d2));
    w(0x4e4f);
}

void makeProgram() {
    for (int n = 0; n < 4; n++) {
        sysStat(4, n);             // キャッシュの設定
        w(0x4e7a), w(0x0002);      // movec cacr,d0
        storeD0(kResult + 0x20 + 8 * (uint32_t)n);
        sysStat(1, 0);             // キャッシュの状態の取得
        storeD0(kResult + 0x24 + 8 * (uint32_t)n);
    }
    setCacr(0xa0808000);
    sysStat(1, 0);
    storeD0(kResult + 0x40);
    setCacr(0x20800000);
    sysStat(1, 0);
    storeD0(kResult + 0x44);
    setCacr(kCacrBeforeWait);
    w(0x4e7a), w(0x0002);          // movec cacr,d0
    storeD0(kResult + 12);
    w(0x23fc), l(1), l(kResult);   // move.l #1,結果 + 0
    w(0x7079);                     // moveq #$79,d0      _MS_ONTM
    w(0x7200);                     // moveq #0,d1        左ボタン
    w(0x243c), l((uint32_t)kUnits); // move.l #500,d2    待つ長さ
    w(0x4e4f);                     // trap #15
    storeD0(kResult + 4);
    w(0x23fc), l(1), l(kResult + 8); // move.l #1,結果 + 8
    w(0x4e7a), w(0x0002);          // movec cacr,d0
    storeD0(kResult + 16);
    w(0x60fe);                     // bra.s *
}

x68k::Machine* gm;

uint32_t peek32(uint32_t a) {
    uint32_t v = 0;
    bool ok;
    for (int i = 0; i < 4; i++) v = (v << 8) | gm->peek8(a + i, ok);
    return v;
}

void poke16(uint32_t a, uint16_t v) {
    gm->write8(a, (uint8_t)(v >> 8), m68k::FC_SUPER_DATA);
    gm->write8(a + 1, (uint8_t)v, m68k::FC_SUPER_DATA);
}

int failures = 0;

void check(bool ok, const char* what, uint32_t value) {
    printf("%s: %s (%08x)\n", what, ok ? "ok" : "FAILED", value);
    if (!ok) failures++;
}

} // namespace

int main(int argc, char** argv) {
    const char* iplPath = nullptr;
    double minMs = -1, maxMs = -1;
    int clockMHz = 50;
    bool anyRom = false;
    bool wantLoopCacr = false;
    uint32_t loopCacrWant = 0;
    const char* rom30Path = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ipl") && i + 1 < argc) iplPath = argv[++i];
        else if (!strcmp(argv[i], "--min") && i + 1 < argc) minMs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) maxMs = atof(argv[++i]);
        else if (!strcmp(argv[i], "--clock") && i + 1 < argc) clockMHz = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--any-rom")) anyRom = true;
        else if (!strcmp(argv[i], "--loop-cacr") && i + 1 < argc) {
            wantLoopCacr = true;
            loopCacrWant = (uint32_t)strtoul(argv[++i], nullptr, 16);
        }
        else if (!strcmp(argv[i], "--rom30") && i + 1 < argc) rom30Path = argv[++i];
        else {
            fprintf(stderr,
                    "usage: %s [--ipl IPLROM30.DAT] [--min ms] [--max ms] [--clock 50|75] [--any-rom [--rom30 file] [--loop-cacr hex]]\n",
                    argv[0]);
            return 2;
        }
    }
    static uint8_t ipl[x68k::Machine::IPL_SIZE];
    FILE* f = iplPath ? fopen(iplPath, "rb") : nullptr;
    const bool got = f && fread(ipl, 1, sizeof ipl, f) == sizeof ipl;
    if (f) fclose(f);
    if (!got) {
        fprintf(stderr, "cannot read %s; skipped\n", iplPath ? iplPath : "(no --ipl)");
        return 77;
    }
    static const uint8_t kRomver30[] = {0x20, 0x3c, 0x13, 0x92, 0x11, 0x27, 0x4e, 0x75};
    if (!anyRom && memcmp(ipl + 0x10030, kRomver30, sizeof kRomver30) != 0) {
        fprintf(stderr, "%s is not the IPL ROM 1.3 of the X68030; skipped\n", iplPath);
        return 77;
    }
    x68k::Config config;
    if (!config.setCpu(68060)) {
        fprintf(stderr, "no 68060 core in this build; skipped\n");
        return 77;
    }
    if (!config.setClockMHz(clockMHz)) {
        fprintf(stderr, "--clock: 50 or 75\n");
        return 2;
    }
    config.localMemSize = 16u << 20;
    static x68k::Machine m(config);
    gm = &m;
    if (!m.ok() || !m.setIplRom(ipl, sizeof ipl)) {
        fprintf(stderr, "cannot set up the machine\n");
        return 1;
    }
    if (rom30Path) {
        static uint8_t rom30[x68k::Machine::SCSI_ROM_SIZE];
        FILE* g = fopen(rom30Path, "rb");
        const size_t n = g ? fread(rom30, 1, sizeof rom30, g) : 0;
        if (g) fclose(g);
        if (n != sizeof rom30 || !m.setScsiRom(rom30, sizeof rom30)) {
            fprintf(stderr, "cannot read %s; skipped\n", rom30Path);
            return 77;
        }
    }
    m.setTime(1767225600);
    m.powerOn();
    // 起動時: CACR が初めて 0 でなくなったときの値 (IPL ROM が $FF014E で書く $2101 を読み替えたもの)
    uint32_t firstCacr = 0;
    for (int i = 0; i < 200000 && !firstCacr; i++) {
        m.run(200);
        firstCacr = m.cpu020()->cacr_;
    }
    if (!anyRom)
        check(firstCacr == 0x80008000, "the first CACR written at boot ($2101 of the 68030) is EIC + EDC", firstCacr);
    for (int i = 0; i < 400; i++) m.runFrame(); // IPL ROM がディスクを待つところまで

    m68k::Registers r;
    for (int i = 0; i < 200; i++) {
        m.cpu020()->getRegisters(r);
        if ((r.sr & 0x0700) == 0) break;
        m.run(100);
    }
    if (r.sr & 0x0700) {
        printf("the interrupt mask never became 0 (sr=%04x)\n", r.sr);
        return 1;
    }
    makeProgram();
    if (kCode + g_len * 2 > kResult) {
        printf("the program is too long\n");
        return 1;
    }
    for (size_t i = 0; i < g_len; i++) poke16(kCode + (uint32_t)i * 2, g_program[i]);
    for (uint32_t a = kResult; a < kResult + 0x48; a += 2) poke16(a, a < kResult + 0x20 ? 0 : 0xeeee);
    r.pc = kCode;
    r.sr = 0x2000;
    r.ssp = kStack;
    m.cpu020()->setRegisters(r);
    // 細かく進めて、始めと終わりのサイクル数を取る (1 回に 2000 サイクル = 50MHz で 40 マイクロ秒、75MHz で 27 マイクロ秒)。
    // 待ちのループ ($FFAC86 から $FFACE8 の手前まで) を実行している間の CACR も見る
    uint64_t start = 0, end = 0;
    int inLoop = 0, wrong = 0;
    uint32_t wrongCacr = 0, loopCacr = 0, otherCacr = 0;
    int cacrOther = 0; // --loop-cacr: 最初に見た値と違う CACR を見た回数
    for (int i = 0; i < 2000000 && !end; i++) {
        m.run(2000);
        const uint64_t now = m.cpu020()->cycles();
        if (!start && peek32(kResult) == 1) start = now;
        if (peek32(kResult + 8) == 1) end = now;
        if (start && !end) {
            m.cpu020()->getRegisters(r);
            if (r.pc >= 0xffac86 && r.pc < 0xfface8) {
                inLoop++;
                if (inLoop > 1 && m.cpu020()->cacr_ != loopCacr) {
                    cacrOther++;
                    otherCacr = m.cpu020()->cacr_;
                }
                loopCacr = m.cpu020()->cacr_;
                if (m.cpu020()->cacr_ != 0x80008000) {
                    wrong++;
                    wrongCacr = m.cpu020()->cacr_;
                }
            }
        }
    }
    if (!start || !end) {
        printf("_MS_ONTM did not return\n");
        return 1;
    }
    for (int i = 0; i < 10; i++) m.run(2000); // 呼んだあとの CACR を書くところまで

    static const uint32_t kWant[4] = {0x00000000, 0x00008000, 0x80000000, 0x80008000};
    for (int n = 0; n < 4 && !anyRom; n++) {
        char what[96];
        snprintf(what, sizeof what, "_SYS_STAT set (d2 = %d): CACR", n);
        check(peek32(kResult + 0x20 + 8 * (uint32_t)n) == kWant[n], what, peek32(kResult + 0x20 + 8 * (uint32_t)n));
        snprintf(what, sizeof what, "_SYS_STAT get after set (d2 = %d) returns %d", n, n);
        check(peek32(kResult + 0x24 + 8 * (uint32_t)n) == (uint32_t)n, what, peek32(kResult + 0x24 + 8 * (uint32_t)n));
    }
    // --any-rom では、待ちのループの間の CACR を出す (IPL ROM 1.6 は命令キャッシュだけを有効にして回る)。--loop-cacr が
    // あれば、どの見た値もその値か確かめる
    if (anyRom) {
        printf("CACR inside the wait loop: %08x (%d samples)\n", loopCacr, inLoop);
        if (wantLoopCacr) {
            check(inLoop > 100, "samples taken inside the wait loop", (uint32_t)inLoop);
            char what[96];
            snprintf(what, sizeof what, "CACR inside the wait loop is $%08X", loopCacrWant);
            check(cacrOther == 0 && loopCacr == loopCacrWant, what, cacrOther ? otherCacr : loopCacr);
        }
    }
    if (!anyRom) { // X68030 の IPL ROM 1.3 の読み替え (patchIplFor060)
        check(peek32(kResult + 0x40) == 3, "_SYS_STAT get with CACR = $A0808000 returns 3", peek32(kResult + 0x40));
        check(peek32(kResult + 0x44) == 0, "_SYS_STAT get with CACR = $20800000 returns 0", peek32(kResult + 0x44));
        check(inLoop > 100, "samples taken inside the wait loop", (uint32_t)inLoop);
        check(wrong == 0, "CACR inside the wait loop is $80008000 (EIC + EDC)", wrong ? wrongCacr : 0x80008000);
        check(peek32(kResult + 12) == kCacrBeforeWait, "CACR before _MS_ONTM", peek32(kResult + 12));
        check(peek32(kResult + 16) == kCacrBeforeWait, "CACR after _MS_ONTM is restored", peek32(kResult + 16));
    }
    check(peek32(kResult + 4) == 0xffff, "_MS_ONTM timed out (d0 = $FFFF)", peek32(kResult + 4));

    const double ms = (double)(end - start) / (double)m.cpuHz() * 1000.0;
    const double per1000 = ms * 1000.0 / kUnits;
    printf("$0CB8 = %u\n", peek32(0xcb8) >> 16);
    // $0CB8 は選んだクロックに比例させる (50MHz で 8333、75MHz で 12500。x68k/machine.cpp の patchIplFor060)
    if (!anyRom)
        check((peek32(0xcb8) >> 16) == m.cpuHz() / 6000, "IOCS work $0CB8 is cpuHz / 6000", peek32(0xcb8) >> 16);
    printf("_MS_ONTM (d2 = %d) took %.1f ms: %.1f ms per 1000 units\n", kUnits, ms, per1000);
    if ((minMs >= 0 && per1000 < minMs) || (maxMs >= 0 && per1000 > maxMs)) {
        printf("FAILED: want %.0f to %.0f ms per 1000 units\n", minMs, maxMs);
        failures++;
    }
    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
