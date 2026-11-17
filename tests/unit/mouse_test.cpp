// マウス (src/x68k/mouse.cpp) のテスト: 3 バイトの形、±127 を超えたときの切り詰めとオーバーフローのビット、
// RTS (MSCTRL) で送り始めること、4800bps の間隔、溜めた移動量、短いクリックを落とさないこと
#include <limits.h>

#include "test_check.h"
#include "x68k/mouse.h"

namespace {

using x68k::Mouse;
using x68k::Scc;

void wr(Scc& s, int reg, uint8_t v) {
    if (reg != 0) s.write(1, (uint8_t)reg);
    s.write(1, v);
}

void setupScc(Scc& s, Mouse& m) {
    s.setListener(&m);
    s.reset();
    wr(s, 3, 0xc1); // 受信を許可
    wr(s, 5, 0x60);
}

void request(Scc& s) {
    wr(s, 5, 0x60);
    wr(s, 5, 0x62); // RTS を出す
}

void testPacket() {
    printf("packet format\n");
    uint8_t p[3];
    Mouse::makePacket(5, -3, Mouse::ST_LEFT, p);
    CHECK_EQ(p[0], 0x01);
    CHECK_EQ(p[1], 0x05);
    CHECK_EQ(p[2], 0xfd);
    Mouse::makePacket(127, -128, Mouse::ST_RIGHT, p);
    CHECK_EQ(p[0], 0x02);
    CHECK_EQ(p[1], 0x7f);
    CHECK_EQ(p[2], 0x80);
    Mouse::makePacket(200, 0, 0, p);
    CHECK_EQ(p[0], 0x10);
    CHECK_EQ(p[1], 0x7f);
    Mouse::makePacket(-200, 0, 0, p);
    CHECK_EQ(p[0], 0x20);
    CHECK_EQ(p[1], 0x80);
    Mouse::makePacket(0, 300, 0, p);
    CHECK_EQ(p[0], 0x40);
    CHECK_EQ(p[2], 0x7f);
    Mouse::makePacket(0, -129, 3, p);
    CHECK_EQ(p[0], 0x83);
    CHECK_EQ(p[2], 0x80);
}

void testRequest() {
    printf("request by rts\n");
    const uint32_t hz = 16000000;
    Scc s;
    Mouse m(hz);
    setupScc(s, m);
    CHECK_EQ(m.byteCycles(), (uint64_t)hz * 11 / 4800);
    m.onMouse(10, 0, 0);
    m.onMouse(5, -2, Mouse::BTN_RIGHT);
    // RTS を出す前は送らない
    const uint64_t t = 1000;
    m.update(t, s);
    CHECK_EQ(s.rxCount(Scc::CH_B), 0);
    request(s);
    CHECK_EQ(m.busy(), 1);
    CHECK_EQ(m.pendingDx(), 0);
    m.update(t + m.byteCycles() - 1, s);
    CHECK_EQ(s.rxCount(Scc::CH_B), 0);
    m.update(t + m.byteCycles(), s);
    CHECK_EQ(s.rxCount(Scc::CH_B), 1);
    // 送っている間の要求は無視する
    request(s);
    CHECK_EQ(m.packets(), 1);
    m.update(t + 3 * m.byteCycles(), s);
    CHECK_EQ(s.rxCount(Scc::CH_B), 3);
    CHECK_EQ(s.read(3), 0x02); // 右ボタン
    CHECK_EQ(s.read(3), 15);
    CHECK_EQ(s.read(3), 0xfe);
    CHECK_EQ(m.busy(), 0);
    // 動いていなくても送る
    request(s);
    m.update(t + 10 * m.byteCycles(), s);
    CHECK_EQ(s.read(3), 0x02);
    CHECK_EQ(s.read(3), 0);
    CHECK_EQ(s.read(3), 0);
    // RTS を下ろすだけでは送らない
    uint32_t n = m.packets();
    wr(s, 5, 0x60);
    CHECK_EQ(m.packets(), n);
    // チャネル A の RTS では送らない
    s.write(5, 5);
    s.write(5, 0x62);
    CHECK_EQ(m.packets(), n);
}

void testClickAndOverflow() {
    printf("short click and overflow\n");
    Scc s;
    Mouse m;
    setupScc(s, m);
    // 求められる前に押して離した
    m.onMouse(0, 0, Mouse::BTN_LEFT);
    m.onMouse(0, 0, 0);
    request(s);
    m.update(1u << 20, s);
    CHECK_EQ(s.read(3), 0x01);
    s.read(3);
    s.read(3);
    // 次のパケットでは離している
    request(s);
    m.update(1u << 21, s);
    CHECK_EQ(s.read(3), 0x00);
    s.read(3);
    s.read(3);
    // 大きく動いたら切り詰め、超えた分は捨てる
    m.onMouse(-500, 400, 0);
    request(s);
    m.update(1u << 22, s);
    CHECK_EQ(s.read(3), 0x60);
    CHECK_EQ(s.read(3), 0x80);
    CHECK_EQ(s.read(3), 0x7f);
    CHECK_EQ(m.pendingDx(), 0);
    CHECK_EQ(m.pendingDy(), 0);
    // 中ボタンは X68000 のマウスにない
    m.onMouse(0, 0, Mouse::BTN_MIDDLE);
    request(s);
    m.update(1u << 23, s);
    CHECK_EQ(s.read(3), 0x00);
}

// 溜めた移動量に足すとき、int の範囲を超える値が来てもあふれない (切り詰めてから足す)
void testHugeMove() {
    printf("huge move does not overflow\n");
    Mouse m;
    m.onMouse(1000, -1000, 0);
    m.onMouse(INT_MAX, INT_MIN, 0);
    CHECK_EQ(m.pendingDx(), 1 << 20);
    CHECK_EQ(m.pendingDy(), -(1 << 20));
    m.onMouse(INT_MAX, INT_MIN, 0);
    CHECK_EQ(m.pendingDx(), 1 << 20);
    CHECK_EQ(m.pendingDy(), -(1 << 20));
}

} // namespace

int main() {
    testPacket();
    testRequest();
    testClickAndOverflow();
    testHugeMove();
    return finish();
}
