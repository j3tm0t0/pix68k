// Windows 版の FUK の口 (host_fuk.h)。FUK の 2 本の USB シリアルポート (CDC) は、Windows では COM ポートに見える。
// 動きは host_fuk.cpp (Linux / macOS) と同じにする:
//   - fuk=on は、USB の VID:PID が 04D8:E6B1 の装置の、インタフェース 0 と 2 (MI_00 と MI_02) の COM ポートを探す
//     (Linux の /dev/serial/by-id/usb-TNB_Factory._FDC_emulator-if00 と -if02 と同じもの)
//   - PORT_A,PORT_B には COM3,COM4 のように書く (\\.\ は付けなくてよい。付けてもよい)
//   - 115200bps、8N1、フロー制御なしで、DTR と RTS を上げて開く。設定を読めない (GetCommState() が失敗する) ときは、
//     設定せずに使う (試験で名前付きパイプにつなぐため)
//   - 受け取るスレッドが、届いたバイトを時刻と一緒にリングに入れる。送るバイトは送信待ちのバッファ (queue) にためて、前の送信が終わるたびに送る
//   - 読み書きに失敗したら閉じ、2 本のどちらかの名前がいったん見えなくなって (抜いたとき) から開き直す
#ifdef _WIN32

#include "host_fuk.h"

#include "platform/settings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "platform/fuk_ring.h"

namespace {

uint64_t monotonicUs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string winError(DWORD e) {
    char text[256] = {0};
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e,
                             MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), text, sizeof text, nullptr);
    while (n > 0 && (text[n - 1] == '\r' || text[n - 1] == '\n' || text[n - 1] == ' ' || text[n - 1] == '.')) text[--n] = 0;
    char buf[320];
    if (n > 0) snprintf(buf, sizeof buf, "%s (Windows error %lu)", text, (unsigned long)e);
    else snprintf(buf, sizeof buf, "Windows error %lu", (unsigned long)e);
    return buf;
}

// CreateFile() に渡す名前。COM10 から上は \\.\ が要るので、\\ で始まらなければ付ける
std::string deviceName(const char* name) {
    if (name[0] == '\\' && name[1] == '\\') return name;
    return std::string("\\\\.\\") + name;
}

// ポートの名前が見えているか (抜いたのを見るため)。COM ポートなどの DOS のデバイスの名前は QueryDosDevice() で、
// 名前付きパイプは WaitNamedPipe() で見る (パイプに CreateFile() すると、つないでしまうため)
bool portExists(const std::string& dev) {
    const char* p = dev.c_str();
    if (strncmp(p, "\\\\.\\", 4) == 0) p += 4;
    if (strncmp(p, "pipe\\", 5) == 0) {
        if (WaitNamedPipeA(dev.c_str(), 1)) return true;
        return GetLastError() != ERROR_FILE_NOT_FOUND;
    }
    if (strchr(p, '\\') == nullptr) {
        char target[1024];
        return QueryDosDeviceA(p, target, sizeof target) != 0;
    }
    return GetFileAttributesA(dev.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 開いて、そのままのバイトをやり取りする設定にする。開けなければ INVALID_HANDLE_VALUE (GetLastError() に理由)
HANDLE openPort(const std::string& dev) {
    HANDLE h = CreateFileA(dev.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return h;
    DCB d;
    memset(&d, 0, sizeof d);
    d.DCBlength = sizeof d;
    if (GetCommState(h, &d)) { // 設定を読めなければ、そのまま使う
        d.BaudRate = CBR_115200;
        d.ByteSize = 8;
        d.Parity = NOPARITY;
        d.StopBits = ONESTOPBIT;
        d.fBinary = TRUE;
        d.fParity = FALSE;
        d.fOutxCtsFlow = FALSE;
        d.fOutxDsrFlow = FALSE;
        d.fDtrControl = DTR_CONTROL_ENABLE;
        d.fDsrSensitivity = FALSE;
        d.fTXContinueOnXoff = TRUE;
        d.fOutX = FALSE;
        d.fInX = FALSE;
        d.fErrorChar = FALSE;
        d.fNull = FALSE;
        d.fRtsControl = RTS_CONTROL_ENABLE;
        d.fAbortOnError = FALSE;
        // 読み出しは、届いているバイトがあればすぐ戻り、なければ 1 バイト届くまで (10 ms まで) 待つ。書き込みは待ち切る
        COMMTIMEOUTS t = {MAXDWORD, MAXDWORD, 10, 0, 0};
        if (!SetCommState(h, &d) || !SetCommTimeouts(h, &t)) {
            const DWORD e = GetLastError();
            CloseHandle(h);
            SetLastError(e);
            return INVALID_HANDLE_VALUE;
        }
        PurgeComm(h, PURGE_RXABORT | PURGE_RXCLEAR | PURGE_TXABORT | PURGE_TXCLEAR);
    }
    return h;
}

// fuk=on: VID:PID が 04D8:E6B1 の装置の、インタフェース 0 と 2 の COM ポートの名前を探す
bool findFukPorts(char* a, char* b, size_t size) {
    // 「ポート (COM と LPT)」のクラス
    static const GUID kPorts = {0x4d36e978, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    a[0] = b[0] = 0;
    HDEVINFO set = SetupDiGetClassDevsA(&kPorts, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;
    SP_DEVINFO_DATA info;
    info.cbSize = sizeof info;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); i++) {
        char id[512];
        if (!SetupDiGetDeviceInstanceIdA(set, &info, id, sizeof id, nullptr)) continue;
        for (char* c = id; *c; c++) *c = (char)toupper((unsigned char)*c);
        if (strstr(id, "VID_04D8&PID_E6B1") == nullptr) continue;
        char* out = strstr(id, "&MI_00") != nullptr ? a : strstr(id, "&MI_02") != nullptr ? b : nullptr;
        if (out == nullptr) continue;
        HKEY key = SetupDiOpenDevRegKey(set, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;
        char name[64] = {0};
        DWORD len = sizeof name - 1, type = 0;
        if (RegQueryValueExA(key, "PortName", nullptr, &type, (BYTE*)name, &len) == ERROR_SUCCESS && type == REG_SZ)
            snprintf(out, size, "%s", name);
        RegCloseKey(key);
    }
    SetupDiDestroyDeviceInfoList(set);
    return a[0] != 0 && b[0] != 0;
}

class WinFukLink final : public x68k::FukLink {
public:
    WinFukLink(const char* a, const char* b) : name_{a, b}, dev_{deviceName(a), deviceName(b)} {
        trace_ = getenv("PIX68K_FUK_TRACE") != nullptr;
        stopEvent_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        for (int i = 0; i < 2; i++) tx_[i].ev = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    }
    ~WinFukLink() override {
        closePorts();
        CloseHandle(stopEvent_);
        for (int i = 0; i < 2; i++) CloseHandle(tx_[i].ev);
    }

    bool openPorts(char* err, size_t errSize) {
        for (int i = 0; i < 2; i++) {
            h_[i] = openPort(dev_[i]);
            if (h_[i] == INVALID_HANDLE_VALUE) {
                const DWORD e = GetLastError();
                if (err != nullptr) snprintf(err, errSize, "cannot open %s: %s", name_[i].c_str(), winError(e).c_str());
                closePorts();
                return false;
            }
        }
        epoch_++;
        for (int i = 0; i < 2; i++) {
            rx_[i].drop();
            rxRead_[i] = rxWritten_[i];
            lastRxAt_[i] = 0;
        }
        failed_.store(false);
        stop_.store(false);
        ResetEvent(stopEvent_);
        reader_ = std::thread([this] { readLoop(); });
        return true;
    }

    bool online() override {
        if (h_[0] != INVALID_HANDLE_VALUE) return true;
        const uint64_t t = monotonicUs();
        if (t - lastTry_ < (needUnplug_ ? 100000u : 1000000u)) return false;
        lastTry_ = t;
        if (needUnplug_) {
            // 読み書きに失敗したあとは、ポートがいったん消える (2 本のどちらかの名前が見えなくなる。抜いたとき) まで
            // 開き直さない (理由は host_fuk.cpp の online())
            if (portExists(dev_[0]) && portExists(dev_[1])) return false;
            needUnplug_ = false;
            fprintf(stderr, "fuk: the serial ports are gone; will open them when they come back\n");
            return false;
        }
        return openPorts(nullptr, 0);
    }
    unsigned epoch() override { return epoch_; }
    uint64_t nowUs() override { return monotonicUs(); }

    unsigned send(int port, const uint8_t* data, unsigned n) override {
        if (h_[port] == INVALID_HANDLE_VALUE) return 0;
        Tx& t = tx_[port];
        DWORD e = 0;
        unsigned k = 0;
        {
            std::lock_guard<std::mutex> lock(t.m);
            e = reap(port);
            if (e == 0) {
                const size_t room = TX_BYTES - t.queue.size();
                k = n < room ? n : (unsigned)room;
                t.queue.insert(t.queue.end(), data, data + k);
                if (!t.busy && !t.queue.empty()) e = start(port);
            }
        }
        if (e != 0) {
            lost(winError(e).c_str());
            return 0;
        }
        if (k > 0 && trace_) trace('>', port, data, k);
        return k;
    }

    unsigned receive(int port, uint8_t* dst, unsigned max) override {
        if (h_[port] == INVALID_HANDLE_VALUE) return 0;
        const unsigned used = rx_[port].used();
        const unsigned n = rx_[port].read(dst, rxAt_[port].limit(rxRead_[port], used < max ? used : max));
        if (n != 0) {
            rxRead_[port] += n;
            lastRxAt_[port] = rxAt_[port].lookup(rxRead_[port], monotonicUs());
            return n;
        }
        if (failed_.load(std::memory_order_acquire) && rx_[port].used() == 0) {
            const DWORD e = failError_.load();
            lost(e != 0 ? winError(e).c_str() : "hang up");
        }
        return 0;
    }
    uint64_t receivedAtUs(int port) override { return lastRxAt_[port]; }
    unsigned droppedStamps() override { return rxAt_[0].dropped() + rxAt_[1].dropped(); }

    // 送信待ちのバッファ (queue) のバイトも、送りかけのバイト (inflight) も残っていない (送信が終わった) か
    bool sendIdle(int port) override {
        if (h_[port] == INVALID_HANDLE_VALUE) return true;
        Tx& t = tx_[port];
        std::lock_guard<std::mutex> lock(t.m);
        const DWORD e = reap(port);
        if (e != 0) {
            fail(e);
            return true;
        }
        return !t.busy && t.queue.empty();
    }

    void waitRx(unsigned us) override {
        if (h_[0] == INVALID_HANDLE_VALUE) {
            std::this_thread::sleep_for(std::chrono::microseconds(us));
            return;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::microseconds(us),
                     [this] { return rx_[0].used() != 0 || rx_[1].used() != 0 || failed_.load(std::memory_order_acquire); });
    }

private:
    // 送る側: queue にためたバイトを inflight に移して送る。送っている間 (busy) に来たバイトは queue にためる
    struct Tx {
        std::mutex m;
        OVERLAPPED ov;
        HANDLE ev = nullptr;
        std::vector<uint8_t> inflight, queue;
        bool busy = false;
    };
    static constexpr size_t TX_BYTES = 65536;

    // t.m を持って呼ぶ。queue を送り始める。失敗したら Windows のエラーの番号を返す
    DWORD start(int port) {
        Tx& t = tx_[port];
        t.inflight.swap(t.queue);
        t.queue.clear();
        ResetEvent(t.ev);
        memset(&t.ov, 0, sizeof t.ov);
        t.ov.hEvent = t.ev;
        if (!WriteFile(h_[port], t.inflight.data(), (DWORD)t.inflight.size(), nullptr, &t.ov)) {
            const DWORD e = GetLastError();
            if (e != ERROR_IO_PENDING) return e;
        }
        t.busy = true;
        return 0;
    }
    // t.m を持って呼ぶ。送り終わっていたら、残り (送り切れなかった分と、ためた分) を送り始める
    DWORD reap(int port) {
        Tx& t = tx_[port];
        if (!t.busy) return 0;
        DWORD k = 0;
        if (!GetOverlappedResult(h_[port], &t.ov, &k, FALSE)) {
            const DWORD e = GetLastError();
            if (e == ERROR_IO_INCOMPLETE) return 0;
            t.busy = false;
            return e;
        }
        t.busy = false;
        if (k < t.inflight.size()) t.queue.insert(t.queue.begin(), t.inflight.begin() + k, t.inflight.end());
        return t.queue.empty() ? 0 : start(port);
    }

    void trace(char dir, int port, const uint8_t* p, unsigned n) {
        fprintf(stderr, "fuk %10llu %c%c %4u:", (unsigned long long)(monotonicUs() % 10000000000ull), "AB"[port], dir, n);
        for (unsigned i = 0; i < n && i < 24; i++) fprintf(stderr, " %02x", p[i]);
        fprintf(stderr, n > 24 ? " ...\n" : "\n");
    }
    void lost(const char* why) {
        fprintf(stderr, "fuk: the serial ports were closed (%s); will reopen them after the device is unplugged and plugged in again\n",
                why);
        closePorts();
        needUnplug_ = true;
        lastTry_ = monotonicUs();
    }
    // 受け取るスレッド。両方のポートに読み出しを出しておき、届いたバイトを時刻と一緒にリングに入れる。
    // 送り終わったポートには、ためたバイトを送る。読み書きに失敗したら failed_ を立てて終わる (閉じるのは receive() が lost() で)
    void readLoop() {
        static constexpr DWORD BUF = 4096;
        std::vector<uint8_t> buf[2] = {std::vector<uint8_t>(BUF), std::vector<uint8_t>(BUF)};
        OVERLAPPED ov[2];
        HANDLE ev[2];
        bool pending[2] = {false, false};
        for (int i = 0; i < 2; i++) ev[i] = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        DWORD error = 0;
        while (error == 0 && !stop_.load(std::memory_order_acquire)) {
            bool full = false;
            for (int i = 0; i < 2 && error == 0; i++) {
                if (pending[i]) continue;
                if (rx_[i].used() + BUF > RX_BYTES) { // 入りきらない分は読まずに待つ
                    full = true;
                    continue;
                }
                ResetEvent(ev[i]);
                memset(&ov[i], 0, sizeof ov[i]);
                ov[i].hEvent = ev[i];
                if (!ReadFile(h_[i], buf[i].data(), BUF, nullptr, &ov[i]) && GetLastError() != ERROR_IO_PENDING) {
                    error = GetLastError();
                    break;
                }
                pending[i] = true; // すぐ終わったときも、イベントが立っているので下で受け取る
            }
            if (error != 0) break;
            HANDLE w[5];
            DWORD nw = 0;
            for (int i = 0; i < 2; i++)
                if (pending[i]) w[nw++] = ev[i];
            for (int i = 0; i < 2; i++) {
                std::lock_guard<std::mutex> lock(tx_[i].m);
                if (tx_[i].busy) w[nw++] = tx_[i].ev;
            }
            w[nw++] = stopEvent_;
            if (WaitForMultipleObjects(nw, w, FALSE, full ? 1 : 10) == WAIT_FAILED) {
                error = GetLastError();
                break;
            }
            const uint64_t at = monotonicUs();
            bool got = false;
            for (int i = 0; i < 2; i++) {
                if (!pending[i]) continue;
                DWORD k = 0;
                if (!GetOverlappedResult(h_[i], &ov[i], &k, FALSE)) {
                    const DWORD e = GetLastError();
                    if (e == ERROR_IO_INCOMPLETE) continue;
                    pending[i] = false;
                    error = e;
                    break;
                }
                pending[i] = false;
                if (k == 0) continue; // 時間切れ (何も届いていない)
                if (trace_) trace('<', i, buf[i].data(), k);
                rxAt_[i].push(rxWritten_[i] + k, at); // 時刻はバイトより先に入れる
                rxWritten_[i] += rx_[i].write(buf[i].data(), k);
                got = true;
            }
            for (int i = 0; i < 2 && error == 0; i++) {
                std::lock_guard<std::mutex> lock(tx_[i].m);
                error = reap(i);
            }
            if (got) {
                std::lock_guard<std::mutex> lock(mutex_);
                cv_.notify_all();
            }
        }
        for (int i = 0; i < 2; i++) {
            if (pending[i]) {
                DWORD k = 0;
                CancelIoEx(h_[i], &ov[i]);
                GetOverlappedResult(h_[i], &ov[i], &k, TRUE);
            }
            CloseHandle(ev[i]);
        }
        if (error != 0) fail(error);
    }
    void fail(DWORD e) {
        failError_.store(e);
        failed_.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex_);
        cv_.notify_all();
    }
    void closePorts() {
        if (reader_.joinable()) {
            stop_.store(true, std::memory_order_release);
            SetEvent(stopEvent_);
            reader_.join();
        }
        for (int i = 0; i < 2; i++) {
            if (h_[i] == INVALID_HANDLE_VALUE) continue;
            {
                Tx& t = tx_[i];
                std::lock_guard<std::mutex> lock(t.m);
                if (t.busy) {
                    DWORD k = 0;
                    CancelIoEx(h_[i], &t.ov);
                    GetOverlappedResult(h_[i], &t.ov, &k, TRUE);
                    t.busy = false;
                }
                t.queue.clear();
                t.inflight.clear();
            }
            // DTR と RTS を下ろしてから閉じる
            EscapeCommFunction(h_[i], CLRDTR);
            EscapeCommFunction(h_[i], CLRRTS);
            CloseHandle(h_[i]);
            h_[i] = INVALID_HANDLE_VALUE;
        }
    }

    std::string name_[2]; // 書かれた名前 (エラーの表示用)
    std::string dev_[2];  // CreateFile() に渡す名前
    HANDLE h_[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    unsigned epoch_ = 0;
    uint64_t lastTry_ = 0;
    bool needUnplug_ = false;
    bool trace_ = false;
    Tx tx_[2];

    static constexpr unsigned RX_BYTES = 131072;
    std::thread reader_;
    HANDLE stopEvent_ = nullptr;
    std::atomic<bool> stop_{false}, failed_{false};
    std::atomic<DWORD> failError_{0};
    platform::FukRing<RX_BYTES> rx_[2];
    platform::FukStampRing<4096> rxAt_[2];
    unsigned rxWritten_[2] = {0, 0};
    unsigned rxRead_[2] = {0, 0};
    uint64_t lastRxAt_[2] = {0, 0};
    std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace

x68k::FukLink* openHostFukLinkAt(const char* portA, const char* portB, char* err, size_t errSize) {
    WinFukLink* link = new WinFukLink(portA, portB);
    if (!link->openPorts(err, errSize)) {
        delete link;
        return nullptr;
    }
    return link;
}

x68k::FukLink* openHostFukLink(const char* spec, char* err, size_t errSize) {
    if (errSize) err[0] = 0;
    char a[platform::Settings::PATH_SIZE], b[platform::Settings::PATH_SIZE];
    switch (platform::parseFuk(spec, a, b, sizeof a)) {
    case platform::FukPorts::OFF: return nullptr;
    case platform::FukPorts::BAD:
        snprintf(err, errSize, "bad value '%s' (off, on or PORT_A,PORT_B)", spec);
        return nullptr;
    case platform::FukPorts::AUTO:
        if (!findFukPorts(a, b, sizeof a)) {
            snprintf(err, errSize, "cannot find the COM ports of the FUK (USB 04D8:E6B1); is it plugged in? "
                                   "(or write the two COM ports as PORT_A,PORT_B)");
            return nullptr;
        }
        break;
    case platform::FukPorts::PATHS: break;
    }
    return openHostFukLinkAt(a, b, err, errSize);
}

#endif // _WIN32
