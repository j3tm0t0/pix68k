// baremetal/ufd_write.inc (Circle の USB-FDD のドライバの終わりに足す、書き込みの関数。#386) のテスト。
// Circle の代わりに、送ったものを数える偽物のクラスを置き、.inc をそのまま取り込んで動かす。
// 門 (platform/ufi_gate.h) が閉じている、番号が違う、確認の前、直前に許していない、のどれでも何も送らないことを確かめる
#include "platform/ufi_gate.h"
#include "test_check.h"

#include <new>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ---- Circle の代わり。メンバの名前は tools/circle_ufd_patch.sh が .h に足すものと同じ
typedef uint8_t u8;
typedef uint32_t u32;
typedef int boolean;
#define TRUE 1
#define FALSE 0
#define assert(x) ((void)(x))
#define PIX68K_LONG_TIMEOUT
enum THeap { HEAP_DMA30 };
void* operator new[](size_t n, THeap) noexcept { return malloc(n); }
enum { REQUEST_OUT = 0, REQUEST_CLASS = 0x20, REQUEST_TO_INTERFACE = 1 };
struct TSetupData {
    u8 bmRequestType, bRequest;
    uint16_t wValue, wIndex, wLength;
};
enum TUSBError { USBErrorUnknown, USBErrorStall };
struct CXHCIEndpoint {
    int resets = 0;
    boolean ResetFromHalted() { return ++resets, TRUE; }
};
struct CUSBEndpoint {
    CXHCIEndpoint x;
    CXHCIEndpoint* GetXHCIEndpoint() { return &x; }
};
struct CUSBRequest {
    u8* data;
    TUSBError error = USBErrorUnknown;
    CUSBRequest(CUSBEndpoint*, void* p, u32, TSetupData*) : data((u8*)p) {}
    TUSBError GetUSBError() const { return error; }
};
struct CUSBHostController {
    int commands = 0, ins = 0, outs = 0, interrupts = 0;
    boolean commandOk = TRUE;
    TUSBError error = USBErrorUnknown;
    int failTransfer = 0; // 1 = データ、2 = 割り込み
    u8 last[12] = {}, out[8192] = {};
    CUSBEndpoint *in = nullptr, *outEp = nullptr;
    boolean SubmitBlockingRequest(CUSBRequest* r) {
        commands++;
        memcpy(last, r->data, 12);
        r->error = error;
        return commandOk;
    }
    int Transfer(CUSBEndpoint* ep, void* p, size_t n) {
        if (ep == in || ep == outEp) {
            (ep == in ? ins : outs)++;
            if (failTransfer == 1) return -1;
            if (ep == outEp) memcpy(out, p, n);
            else memset(p, 0x5a, n);
            return (int)n;
        }
        interrupts++;
        if (failTransfer == 2) return -1;
        memset(p, 0, n);
        return (int)n;
    }
    int sent() const { return commands + ins + outs + interrupts; }
};
class CUSBFloppyDiskDevice {
public:
    boolean Pix68kArmWrite(u32 nBlock, u32 nCount);
    int Pix68kCommand(const u8* pCmdBlk, void* pBuffer, size_t nBufLen, boolean bIn, u8* pStatus, int* pBytes);
    CUSBHostController host;
    CUSBEndpoint ep0, epIn, epOut, epInt;
    int protocol = 0;
    unsigned m_nDeviceNumber = 1;
    unsigned m_nBlockShift = 10;
    CUSBEndpoint *m_pEndpointIn = &epIn, *m_pEndpointOut = &epOut, *m_pEndpointInterrupt = &epInt;
    platform::UfiGate m_Pix68kGate;
    boolean m_bPix68kBroken = FALSE, m_bPix68kArmed = FALSE;
    u8 *m_pPix68kBuffer = nullptr, *m_pPix68kSmall = nullptr;
    CUSBFloppyDiskDevice() {
        host.in = &epIn;
        host.outEp = &epOut;
    }
    ~CUSBFloppyDiskDevice() {
        free(m_pPix68kBuffer);
        free(m_pPix68kSmall);
    }
    int GetInterfaceProtocol() const { return protocol; }
    int GetInterfaceNumber() const { return 0; }
    CUSBHostController* GetHost() { return &host; }
    CUSBEndpoint* GetEndpoint0() { return &ep0; }
};

#include "../../baremetal/ufd_write.inc"

using namespace platform;

namespace {

const u8 kWrite[12] = {0x2a, 0, 0, 0, 0, 8, 0, 0, 8};
const u8 kRead[12] = {0x28, 0, 0, 0, 0, 8, 0, 0, 8};
const u8 kReady[12] = {};
u8 g_data[16384];

int command(CUSBFloppyDiskDevice& d, const u8* cdb, size_t len, boolean in) {
    u8 st[2] = {9, 9};
    int bytes = -1;
    const int r = d.Pix68kCommand(cdb, g_data, len, in, st, &bytes);
    if (r == 1 || r == 2) CHECK_EQ(bytes, 0); // 3 は、転送した数が入ることがある
    return r;
}

// 門が閉じている、番号が違う: 何も送らない
void testClosed() {
    for (int c = 0; c < 4; c++) {
        CUSBFloppyDiskDevice d;
        UfiGate::enable(c == 0 ? -1 : c == 1 ? 1 : c == 2 ? 4 : 0);
        UfiGate::confirm(true);
        if (c == 3) d.m_nDeviceNumber = 2; // 許した番号は ufd1
        CHECK_EQ(d.Pix68kArmWrite(8, 8), FALSE);
        CHECK_EQ(d.m_bPix68kArmed, FALSE);
        CHECK_EQ(command(d, kWrite, 8192, FALSE), 1);
        CHECK_EQ(command(d, kRead, 8192, TRUE), 1);
        CHECK_EQ(command(d, kReady, 0, TRUE), 1);
        CHECK_EQ(d.host.sent(), 0);
    }
    // 門を呼ぶ前に、同じ装置で許してあっても、あとで閉じれば送らない
    CUSBFloppyDiskDevice d;
    UfiGate::enable(0);
    UfiGate::confirm(true);
    CHECK_EQ(d.Pix68kArmWrite(8, 8), TRUE);
    UfiGate::enable(-1);
    CHECK_EQ(command(d, kWrite, 8192, FALSE), 1);
    CHECK_EQ(d.host.sent(), 0);
}

// 許した番号でも、確認の前、直前に許していない、違う範囲や大きさの WRITE(10)、知らないコマンドは送らない
void testNotArmed() {
    UfiGate::enable(0);
    {
        CUSBFloppyDiskDevice d;
        CHECK_EQ(d.Pix68kArmWrite(8, 8), FALSE); // 確認の前
        CHECK_EQ(command(d, kWrite, 8192, FALSE), 1);
        CHECK_EQ(d.host.sent(), 0);
        CHECK_EQ(command(d, kRead, 8192, TRUE), 0); // 読むのはよい
        CHECK_EQ(d.host.outs, 0);
    }
    UfiGate::confirm(true);
    CUSBFloppyDiskDevice d;
    CHECK_EQ(command(d, kWrite, 8192, FALSE), 1); // 許していない
    for (int c = 0; c < 4; c++) {
        CHECK_EQ(d.Pix68kArmWrite(c == 0 ? 0 : 8, c == 1 ? 7 : 8), TRUE);
        CHECK_EQ(command(d, kWrite, c == 2 ? 4096 : 8192, c == 3 ? TRUE : FALSE), 1);
        CHECK_EQ(command(d, kWrite, 8192, FALSE), 1); // 許しは消えている
    }
    for (int op = 0; op < 256; op++) {
        if (op == 0 || op == 0x03 || op == 0x25 || op == 0x28 || op == 0x5a) continue;
        u8 cdb[12] = {(u8)op, 0, 0, 0, 0, 8, 0, 0, 8};
        CHECK_EQ(command(d, cdb, 8192, FALSE), 1);
        CHECK_EQ(command(d, cdb, 0, TRUE), 1);
    }
    CHECK_EQ(d.Pix68kArmWrite(1232, 1), FALSE);
    CHECK_EQ(d.Pix68kArmWrite(0, 9), FALSE);
    CHECK_EQ(command(d, kRead, 8193, TRUE), 1); // バッファより大きい
    CHECK_EQ(d.host.sent(), 0);
    // 2HD でない、CBI でない、割り込みがない装置
    d.m_nBlockShift = 9;
    CHECK_EQ(d.Pix68kArmWrite(8, 8), FALSE);
    d.m_nBlockShift = 10;
    d.protocol = 1;
    CHECK_EQ(d.Pix68kArmWrite(8, 8), FALSE);
    CHECK_EQ(command(d, kReady, 0, TRUE), 1);
    d.protocol = 0;
    d.m_pEndpointInterrupt = nullptr;
    CHECK_EQ(d.Pix68kArmWrite(8, 8), TRUE);
    CHECK_EQ(command(d, kWrite, 8192, FALSE), 1);
    CHECK_EQ(d.host.sent(), 0);
    UfiGate::confirm(false);
}

// 許したとおりの 1 回だけ送る
void testWrite() {
    UfiGate::enable(0);
    UfiGate::confirm(true);
    CUSBFloppyDiskDevice d;
    for (int i = 0; i < 8192; i++) g_data[i] = (u8)(i * 7);
    CHECK_EQ(d.Pix68kArmWrite(8, 8), TRUE);
    CHECK_EQ(command(d, kWrite, 8192, FALSE), 0);
    CHECK_EQ(d.host.commands, 1);
    CHECK_EQ(d.host.outs, 1);
    CHECK_EQ(d.host.interrupts, 1);
    CHECK_EQ(memcmp(d.host.last, kWrite, 12), 0);
    CHECK_EQ(memcmp(d.host.out, g_data, 8192), 0);
    CHECK_EQ(command(d, kWrite, 8192, FALSE), 1); // 2 回目は許していない
    CHECK_EQ(d.host.commands, 1);
    CHECK_EQ(d.Pix68kArmWrite(8, 8), TRUE);
    UfiGate::confirm(false); // 許したあとで確認を取り消す
    CHECK_EQ(command(d, kWrite, 8192, FALSE), 1);
    CHECK_EQ(d.host.commands, 1);
}

// 転送が失敗したら、そのあとは何も送らない
void testBroken() {
    UfiGate::enable(0);
    UfiGate::confirm(true);
    for (int c = 0; c < 4; c++) {
        CUSBFloppyDiskDevice d;
        d.host.failTransfer = c == 0 ? 1 : c == 1 ? 2 : 0;
        if (c >= 2) d.host.commandOk = FALSE;
        if (c == 3) d.host.error = USBErrorStall; // 書き込みを許したあとの STALL
        CHECK_EQ(d.Pix68kArmWrite(8, 8), TRUE);
        CHECK_EQ(command(d, kWrite, 8192, FALSE), 3);
        CHECK_EQ(d.ep0.x.resets, 0);
        d.host.failTransfer = 0;
        d.host.commandOk = TRUE;
        const int sent = d.host.sent();
        CHECK_EQ(d.Pix68kArmWrite(8, 8), TRUE);
        CHECK_EQ(command(d, kWrite, 8192, FALSE), 1);
        CHECK_EQ(command(d, kReady, 0, TRUE), 1);
        CHECK_EQ(d.host.sent(), sent);
    }
    // 書き込みを許す前の STALL は、入口を戻して続ける
    CUSBFloppyDiskDevice d;
    d.host.commandOk = FALSE;
    d.host.error = USBErrorStall;
    CHECK_EQ(command(d, kReady, 0, TRUE), 2);
    CHECK_EQ(d.ep0.x.resets, 1);
    d.host.commandOk = TRUE;
    CHECK_EQ(command(d, kReady, 0, TRUE), 0);
}

} // namespace

int main() {
    CHECK_EQ(UfiGate::enabledUnit(), -1);
    testClosed();
    testNotArmed();
    testWrite();
    testBroken();
    return finish();
}
