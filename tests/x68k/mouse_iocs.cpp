// マウスが本物の IOCS で使えるかを確かめる。Human68k を起動してプロンプトが出たら、RAM に小さな
// プログラムを書いて CPU をそこに飛ばし、IOCS の _MS_INIT ($70) を呼んだあと、_MS_GETDT ($74) と
// _MS_CURGT ($75) を繰り返し呼んで結果を RAM に書かせる。ホストからマウスを動かし、ボタンを押して、
// IOCS の返す値が変わるかを見る。
//   mouse_iocs [-r ROM のディレクトリ] -0 起動ディスク
// ROM か起動ディスクがなければ 77 (ctest の SKIP_RETURN_CODE)。
//
// _MS_GETDT の d0: bit31-24 X の移動量、bit23-16 Y の移動量、bit15-8 左ボタン、bit7-0 右ボタン (押すと $FF)
// _MS_CURGT の d0: bit31-16 カーソルの X 座標、bit15-0 Y 座標
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "x68k/machine.h"

static uint8_t* loadWhole(const char* path, size_t& size) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = n > 0 ? new uint8_t[(size_t)n] : nullptr;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        delete[] buf;
        buf = nullptr;
    }
    fclose(f);
    size = buf ? (size_t)n : 0;
    return buf;
}

static bool loadFile(const char* path, uint8_t* buf, size_t size) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, size, f);
    int extra = fgetc(f);
    fclose(f);
    return n == size && extra == EOF;
}

namespace {

const uint32_t kCode = 0x300000;  // プログラムを置く場所 (起動直後の Human68k は使っていない)
const uint32_t kStack = 0x2ff000; // プログラムのスーパーバイザスタック
const uint32_t kGetdt = 0x300100, kCurgt = 0x300104, kCount = 0x300108;

// 68000 のプログラム
const uint16_t kProgram[] = {
    0x7070,                         // moveq #$70,d0     _MS_INIT
    0x4e4f,                         // trap #15
    // loop:
    0x7074,                         // moveq #$74,d0     _MS_GETDT
    0x4e4f,                         // trap #15
    0x23c0, 0x0030, 0x0100,         // move.l d0,$300100
    0x7075,                         // moveq #$75,d0     _MS_CURGT
    0x4e4f,                         // trap #15
    0x23c0, 0x0030, 0x0104,         // move.l d0,$300104
    0x52b9, 0x0030, 0x0108,         // addq.l #1,$300108
    0x60e4,                         // bra.s loop (-28)
};

x68k::Machine* gm;

uint32_t peek32(uint32_t a) {
    bool ok;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | gm->peek8(a + i, ok);
    return v;
}

void poke16(uint32_t a, uint16_t v) {
    gm->write8(a, (uint8_t)(v >> 8), m68k::FC_SUPER_DATA);
    gm->write8(a + 1, (uint8_t)v, m68k::FC_SUPER_DATA);
}

void frames(int n) {
    for (int i = 0; i < n; i++) gm->runFrame();
}

int failures = 0;

void check(bool ok, const char* what, uint32_t value) {
    printf("%s: %s (%08x)\n", what, ok ? "ok" : "FAILED", value);
    if (!ok) failures++;
}

} // namespace

int main(int argc, char** argv) {
    char romDir[1024];
    const char* home = getenv("HOME");
    snprintf(romDir, sizeof romDir, "%s/.cache/pix68k/rom", home ? home : ".");
    const char* disk = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) snprintf(romDir, sizeof romDir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "-0") && i + 1 < argc) disk = argv[++i];
        else {
            fprintf(stderr, "usage: %s [-r romdir] -0 bootdisk\n", argv[0]);
            return 2;
        }
    }
    static uint8_t ipl[x68k::Machine::IPL_SIZE];
    char path[1100];
    snprintf(path, sizeof path, "%s/IPLROM.DAT", romDir);
    if (!loadFile(path, ipl, sizeof ipl)) {
        fprintf(stderr, "cannot read %s; skipped\n", path);
        return 77;
    }
    size_t size = 0;
    uint8_t* data = disk ? loadWhole(disk, size) : nullptr;
    if (!data) {
        fprintf(stderr, "cannot read the boot disk %s; skipped\n", disk ? disk : "(none)");
        return 77;
    }
    static x68k::Machine m;
    gm = &m;
    static x68k::FloppyImage image;
    if (!m.ok() || !image.load(data, size, false, disk)) {
        fprintf(stderr, "cannot set up the machine\n");
        return 1;
    }
    m.setIplRom(ipl, sizeof ipl);
    m.insertDisk(0, &image);
    m.setTime((int64_t)time(nullptr));
    m.powerOn();
    frames(900); // Human68k のプロンプトまで

    // 割り込みを受け付けている (マスク 0) ときに飛ばす (割り込みの処理の途中で横取りしない)
    m68k::Registers r;
    for (int i = 0; i < 200; i++) {
        m.cpu().getRegisters(r);
        if ((r.sr & 0x0700) == 0) break;
        m.run(100);
    }
    if (r.sr & 0x0700) {
        printf("the interrupt mask never became 0 (sr=%04x)\n", r.sr);
        return 1;
    }
    for (size_t i = 0; i < sizeof kProgram / 2; i++) poke16(kCode + (uint32_t)i * 2, kProgram[i]);
    for (uint32_t a = kGetdt; a < kCount + 4; a += 2) poke16(a, 0);
    r.pc = kCode;
    r.sr = 0x2000; // スーパーバイザ、割り込みはすべて受け付ける
    r.ssp = kStack;
    r.prefetch[0] = kProgram[0];
    r.prefetch[1] = kProgram[1];
    m.cpu().setRegisters(r);

    frames(30);
    uint32_t count = peek32(kCount);
    check(count > 10, "the program is running (loop count)", count);
    check(m.mouse().packets() > 0, "IOCS asked the mouse for data (packets)", m.mouse().packets());
    uint32_t d = peek32(kGetdt);
    check((d & 0xffff) == 0, "_MS_GETDT with no buttons", d);
    uint32_t pos0 = peek32(kCurgt);

    // 左ボタン
    m.input().onMouse(0, 0, platform::InputSink::MOUSE_LEFT);
    frames(10);
    d = peek32(kGetdt);
    check((d & 0xff00) != 0 && (d & 0x00ff) == 0, "_MS_GETDT reports the left button", d);
    m.input().onMouse(0, 0, 0);
    frames(10);
    d = peek32(kGetdt);
    check((d & 0xffff) == 0, "_MS_GETDT after releasing the left button", d);

    // 右ボタン
    m.input().onMouse(0, 0, platform::InputSink::MOUSE_RIGHT);
    frames(10);
    d = peek32(kGetdt);
    check((d & 0xff00) == 0 && (d & 0x00ff) != 0, "_MS_GETDT reports the right button", d);
    m.input().onMouse(0, 0, 0);
    frames(10);

    // 押して、求められる前に離したクリックも届く
    m.input().onMouse(0, 0, platform::InputSink::MOUSE_LEFT);
    m.input().onMouse(0, 0, 0);
    bool seen = false;
    for (int i = 0; i < 10 && !seen; i++) {
        m.run(m.cpuHz() / 1000);
        seen = (peek32(kGetdt) & 0xff00) != 0;
    }
    check(seen, "a short click is not lost", peek32(kGetdt));
    frames(10);

    // 右下へ動かす: カーソルの座標が増える
    for (int i = 0; i < 10; i++) {
        m.input().onMouse(6, 4, 0);
        frames(1);
    }
    frames(10);
    uint32_t pos1 = peek32(kCurgt);
    int x0 = (int)(pos0 >> 16), y0 = (int)(pos0 & 0xffff), x1 = (int)(pos1 >> 16), y1 = (int)(pos1 & 0xffff);
    printf("cursor (%d,%d) -> (%d,%d)\n", x0, y0, x1, y1);
    check(x1 > x0, "moving right increases X", pos1);
    check(y1 > y0, "moving down increases Y", pos1);
    // 左上へ戻す
    for (int i = 0; i < 10; i++) {
        m.input().onMouse(-6, -4, 0);
        frames(1);
    }
    frames(10);
    uint32_t pos2 = peek32(kCurgt);
    int x2 = (int)(pos2 >> 16), y2 = (int)(pos2 & 0xffff);
    printf("cursor -> (%d,%d)\n", x2, y2);
    check(x2 < x1 && y2 < y1, "moving left/up decreases X/Y", pos2);

    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
