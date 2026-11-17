// 8255 (src/x68k/ppi.cpp) のテスト: ジョイスティックのビット、ポート C の書き込みとビットセット / リセット、
// モードの設定、トリガを L にするポート C の bit6 / 7、ADPCM のビット、Machine の入力からの経路
#include "test_check.h"
#include "x68k/machine.h"
#include "x68k/ppi.h"

namespace {

using x68k::Ppi;

void testJoystickBits() {
    printf("joystick bits\n");
    Ppi p;
    p.reset();
    CHECK_EQ(p.read(1), 0xff);
    CHECK_EQ(p.read(3), 0xff);
    p.setJoystick(0, Ppi::JOY_UP);
    CHECK_EQ(p.read(1), 0xfe);
    p.setJoystick(0, Ppi::JOY_DOWN | Ppi::JOY_RIGHT);
    CHECK_EQ(p.read(1), 0xf5);
    p.setJoystick(0, Ppi::JOY_LEFT | Ppi::JOY_A);
    CHECK_EQ(p.read(1), 0xdb);
    p.setJoystick(1, Ppi::JOY_B);
    CHECK_EQ(p.read(3), 0xbf);
    CHECK_EQ(p.read(1), 0xdb); // ポート A は変わらない
    // 上と下を同時には押せない
    p.setJoystick(0, Ppi::JOY_UP | Ppi::JOY_DOWN | Ppi::JOY_A);
    CHECK_EQ(p.read(1), 0xdf);
    p.setJoystick(2, 0xff); // ないポートは無視
    CHECK_EQ(Ppi::portValue(0), 0xff);
    CHECK_EQ(Ppi::portValue(0x3f), 0x90);
}

void testPortC() {
    printf("port c\n");
    Ppi p;
    p.reset();
    p.write(5, 0x0b);
    CHECK_EQ(p.read(5), 0x0b);
    CHECK_EQ(p.adpcmPan(), 3);
    CHECK_EQ(p.adpcmRate(), 2);
    // ビットセット / リセット ($E9A007)
    p.write(7, 0x00); // bit0 を 0
    CHECK_EQ(p.portC(), 0x0a);
    p.write(7, 0x09); // bit4 を 1
    CHECK_EQ(p.portC(), 0x1a);
    p.write(7, 0x0f); // bit7 を 1
    CHECK_EQ(p.portC(), 0x9a);
    // bit6 / bit7: ジョイスティック 1 のトリガ A / B を L にする
    p.setJoystick(0, 0);
    CHECK_EQ(p.read(1), 0xbf); // bit7 → トリガ B
    p.write(7, 0x0d);          // bit6 を 1
    CHECK_EQ(p.read(1), 0x9f);
    CHECK_EQ(p.read(3), 0xff); // ジョイスティック 2 には効かない
    // ポート A / B への書き込みは何もしない
    p.write(1, 0x00);
    CHECK_EQ(p.read(1), 0x9f);
    // モードの設定で出力は 0 になる
    p.write(7, 0x92);
    CHECK_EQ(p.control(), 0x92);
    CHECK_EQ(p.portC(), 0x00);
    CHECK_EQ(p.read(1), 0xff);
}

void testMachineInput() {
    printf("machine input path\n");
    static x68k::Machine m;
    m.powerOn();
    m.input().onJoystick(0, platform::InputSink::JOY_RIGHT | platform::InputSink::JOY_B);
    CHECK_EQ(m.read8(0xe9a001, m68k::FC_SUPER_DATA), 0xb7);
    m.input().onJoystick(1, platform::InputSink::JOY_UP);
    CHECK_EQ(m.read8(0xe9a003, m68k::FC_SUPER_DATA), 0xfe);
    // 8255 への書き込み (ビットセット) がポート C に入る
    m.write8(0xe9a007, 0x03, m68k::FC_SUPER_DATA);
    CHECK_EQ(m.read8(0xe9a005, m68k::FC_SUPER_DATA) & 0x02, 0x02);
    // マウスの入力は Mouse に溜まる
    m.input().onMouse(3, -4, 0);
    CHECK_EQ(m.mouse().pendingDx(), 3);
    CHECK_EQ(m.mouse().pendingDy(), -4);
    // 定数が platform と x68k で同じ
    CHECK_EQ(platform::InputSink::JOY_A, Ppi::JOY_A);
    CHECK_EQ(platform::InputSink::JOY_RIGHT, Ppi::JOY_RIGHT);
    CHECK_EQ(platform::InputSink::MOUSE_RIGHT, x68k::Mouse::BTN_RIGHT);
}

} // namespace

int main() {
    testJoystickBits();
    testPortC();
    testMachineInput();
    return finish();
}
