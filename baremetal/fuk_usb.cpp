#include "fuk_usb.h"

#include <circle/multicore.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#include <circle/usb/usbdevice.h>
#include <circle/usb/usbserialhost.h>

CFukUsb* CFukUsb::s_pThis = nullptr;

#if RASPPI >= 4
// Circle の xHCI のドライバ (tools/circle_fuk_patch.sh で書き換えたもの) が、USB の装置を破棄する最初に呼ぶ
extern void (*volatile g_pPix68kXHCIDeviceRemoving)(CUSBDevice* pDevice);
#endif

CFukUsb::CFukUsb() {
    for (int i = 0; i < 2; i++) m_Port[i].self = this;
    s_pThis = this;
#if RASPPI >= 4
    g_pPix68kXHCIDeviceRemoving = DeviceRemoving;
#endif
}

CFukUsb::~CFukUsb() {
    EnterCritical();
#if RASPPI >= 4
    g_pPix68kXHCIDeviceRemoving = nullptr;
#endif
    Offline();
    for (int i = 0; i < 2; i++) Detach(m_Port[i]);
    s_pThis = nullptr;
    LeaveCritical();
}

// ---------------------------------------------------------------- コア 0

// VID と PID が合う USB シリアルを探す (Configure() が済んだものだけが一覧にある)。インタフェース番号の順に並べる
int CFukUsb::Find(CUSBSerialHostDevice* found[2]) {
    found[0] = found[1] = nullptr;
    int n = 0;
    for (unsigned i = 0; i < CUSBSerialHostDevice::Pix68kMaxDevices; i++) {
        CUSBSerialHostDevice* dev = CUSBSerialHostDevice::Pix68kGet(i);
        if (dev == nullptr) continue;
        const TUSBDeviceDescriptor* desc = dev->GetDevice()->GetDeviceDescriptor();
        if (desc == nullptr || desc->idVendor != VENDOR || desc->idProduct != PRODUCT) continue;
        if (n < 2) found[n] = dev;
        n++;
    }
    if (n == 2 && found[0]->GetInterfaceNumber() > found[1]->GetInterfaceNumber()) {
        CUSBSerialHostDevice* t = found[0];
        found[0] = found[1];
        found[1] = t;
    }
    return n;
}

bool CFukUsb::Present() {
    CUSBSerialHostDevice* found[2];
    return Find(found) == 2;
}

bool CFukUsb::Scan() {
    if (online()) {
        EnterCritical();
        PumpAll();
        LeaveCritical();
        return true;
    }
    CUSBSerialHostDevice* found[2];
    if (Find(found) != 2) return false;
    // USB の転送が失敗して止まった装置は、抜いて挿し直すまで使わない。コントローラは失敗したエンドポイントを止めて
    // いて、立て直すには装置の側のデータの順番 (トグル) とそろえ直す必要があり、失敗した転送のうち装置に届いた分も
    // 分からない。続けると、'M' のデータが重なったり欠けたりしてディスクを壊しうる。装置を抜くと、Circle が
    // ドライバを消して (DeviceRemoving())、挿し直したときに別の装置として見つかる。電源が保たれたまま USB だけが
    // 切れて戻ったときも別の装置として見つかり、初めの状態に戻ったかは分からない (FukLink::epoch())
    if (m_Port[0].dev == found[0] && m_Port[1].dev == found[1]) return false;
    EnterCritical();
    bool ok = true;
    for (int i = 0; i < 2; i++) {
        Port& p = m_Port[i];
        p.dev = found[i];
        __atomic_store_n(&p.sending, false, __ATOMIC_RELEASE);
        p.chunkLen = 0;
        p.tx.drop(); // 前の装置への送り残し (読み手はコア 0)
        p.dev->Pix68kSetHandlers(ReceiveHandler, SentHandler, RemovedHandler, &p);
        if (!p.dev->Pix68kReceiving() && !p.dev->Pix68kStartReceive()) ok = false;
    }
    if (ok) {
        __atomic_add_fetch(&m_Epoch, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&m_Online, true, __ATOMIC_RELEASE);
        PumpAll();
    } else {
        // 受信を始められなかった。次に呼ばれたときにやり直せるよう、切り離しておく
        for (int i = 0; i < 2; i++) Detach(m_Port[i]);
    }
    LeaveCritical();
    return ok;
}

void CFukUsb::OnIpi() {
    if (s_pThis == nullptr) return;
    s_pThis->m_Counters.ipis++;
    s_pThis->PumpAll();
}

void CFukUsb::PumpAll() {
    Pump(m_Port[0]);
    Pump(m_Port[1]);
}

void CFukUsb::Pump(Port& port) {
    if (port.dev == nullptr || port.sending || !online()) return;
    // リングから取り出す前に「送信中」にする (コア 1 の sendIdle() が、リングは空で送信中でもない一瞬を見ないように)
    __atomic_store_n(&port.sending, true, __ATOMIC_RELEASE);
    port.chunkLen = port.tx.read(port.chunk, sizeof port.chunk);
    if (port.chunkLen == 0) {
        __atomic_store_n(&port.sending, false, __ATOMIC_RELEASE);
        return;
    }
    if (!port.dev->Pix68kSend(port.chunk, port.chunkLen)) {
        __atomic_store_n(&port.sending, false, __ATOMIC_RELEASE);
        m_Counters.sendFailures++;
        Offline();
    }
}

// USB の割り込みの中 (コア 0)。n が 0 なら、受信の転送が失敗して止まった
void CFukUsb::ReceiveHandler(const u8* data, unsigned n, void* param) {
    Port* p = static_cast<Port*>(param);
    if (n == 0) {
        p->self->m_Counters.receiveStops++;
        p->self->Offline();
        return;
    }
    p->self->m_Counters.rxBytes[p - p->self->m_Port] += n;
    // 時刻はバイトより先に入れる (コア 1 が、時刻のないバイトを読まないように)
    p->rxAt.push(p->rxWritten + n, CTimer::GetClockTicks64() * (1000000 / CLOCKHZ));
    const unsigned w = p->rx.write(data, n);
    p->rxWritten += w;
    if (w != n) __atomic_add_fetch(&p->self->m_RxDropped, n - w, __ATOMIC_RELAXED);
}

void CFukUsb::SentHandler(boolean ok, void* param) {
    Port* p = static_cast<Port*>(param);
    if (!ok) {
        // 転送が失敗した。この装置は、抜いて挿し直すまで使わない (Scan())
        p->chunkLen = 0;
        __atomic_store_n(&p->sending, false, __ATOMIC_RELEASE);
        p->self->m_Counters.sendFailures++;
        p->self->Offline();
    } else {
        p->self->m_Counters.txBytes[p - p->self->m_Port] += p->chunkLen;
        p->chunkLen = 0;
        __atomic_store_n(&p->sending, false, __ATOMIC_RELEASE);
        p->self->Pump(*p);
    }
}

// ポートを装置から切り離す: これ以上転送を出さず、あとから終わる転送のハンドラも呼ばせない
void CFukUsb::Detach(Port& port) {
    if (port.dev != nullptr) port.dev->Pix68kStop();
    port.dev = nullptr;
    port.chunkLen = 0;
    __atomic_store_n(&port.sending, false, __ATOMIC_RELEASE);
}

// USB の装置の破棄が始まる (抜いた)。Circle のプラグアンドプレイの処理 (コア 0 の主ループ) の中から、xHCI のドライバが
// スロットを無効にする前に呼ぶ。そのあと USB シリアルのドライバが消えるまでの間に、コア 1 からの IPI で送信の転送を
// 出すと、無効なスロットに出すことになる (Circle の assert で止まる)。ここで、割り込みを止めて (IPI のハンドラと
// 転送の完了のハンドラはコア 0 の割り込みの中で動くので、途中に入らない) 切り離す
void CFukUsb::DeviceRemoving(CUSBDevice* device) {
    CFukUsb* self = s_pThis;
    if (self == nullptr) return;
    EnterCritical();
    for (int i = 0; i < 2; i++) {
        Port& p = self->m_Port[i];
        if (p.dev == nullptr || p.dev->GetDevice() != device) continue;
        self->Detach(p);
        self->m_Counters.removals++;
        self->Offline();
    }
    LeaveCritical();
}

// USB シリアルのドライバが消える。ふつうは DeviceRemoving() が先に切り離している (Configure() に失敗して消えるときなど、
// 装置ごとではなくドライバだけが消えるときの備え)
void CFukUsb::RemovedHandler(void* param) {
    Port* p = static_cast<Port*>(param);
    EnterCritical();
    if (p->dev != nullptr) {
        p->self->Detach(*p);
        p->self->m_Counters.removals++;
        p->self->Offline();
    }
    LeaveCritical();
}

// ---------------------------------------------------------------- コア 1

void CFukUsb::Kick() {
    if (CMultiCoreSupport::ThisCore() == 0) {
        EnterCritical();
        PumpAll();
        LeaveCritical();
    } else {
        CMultiCoreSupport::SendIPI(0, IPI);
    }
}

unsigned CFukUsb::send(int port, const uint8_t* data, unsigned n) {
    if (!online()) return 0;
    const unsigned w = m_Port[port].tx.write(data, n);
    Kick();
    return w;
}

unsigned CFukUsb::receive(int port, uint8_t* dst, unsigned max) {
    Port& p = m_Port[port];
    // 同じ時刻に届いたまとまり (USB の転送 1 回分) ごとに返す (receivedAtUs() がそのまとまりの時刻になるように)
    // 先に入っているバイト数を見てから区切りを引く (そのあとに届いたバイトを、違う時刻のまま一緒に読まない)
    const unsigned used = p.rx.used();
    const unsigned n = p.rx.read(dst, p.rxAt.limit(p.rxRead, used < max ? used : max));
    if (n != 0) {
        p.rxRead += n;
        // 届いた時刻は、コア 0 の割り込みの中で付けたもの (コア 1 が読むのが遅れても、届いた時刻のまま)
        p.lastRxAt = p.rxAt.lookup(p.rxRead, nowUs());
    }
    return n;
}

// リングが空で、USB の送信の転送も終わっている (つながっていないときも、リングに残っていれば false)
bool CFukUsb::sendIdle(int port) {
    Port& p = m_Port[port];
    return p.tx.used() == 0 && !__atomic_load_n(&p.sending, __ATOMIC_ACQUIRE) && __atomic_load_n(&p.chunkLen, __ATOMIC_ACQUIRE) == 0;
}

uint64_t CFukUsb::nowUs() { return CTimer::GetClockTicks64() * (1000000 / CLOCKHZ); }

void CFukUsb::waitRx(unsigned us) {
    const u64 end = CTimer::GetClockTicks64() + (u64)us * (CLOCKHZ / 1000000);
    while (CTimer::GetClockTicks64() < end && m_Port[0].rx.used() == 0 && m_Port[1].rx.used() == 0 && online()) {
        asm volatile("yield");
    }
}
