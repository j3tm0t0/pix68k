// Human68k の実行ファイル (.X) を、CPU のコアと最小限の DOS コールだけで動かす (CPU の試験用)。
//
//   run_x [--cpu 68000|68030|68060] [--limit 命令数] [--expect 文字列]... file.x [引数...]
//
// Human68k や IPL ROM は使わない。68060 の未実装整数命令は、060SP の代わりにその場で実行する。
// 16MB の RAM にプログラムを読み込み、ユーザモードで実行する。
// DOS コール ($FFxx) は命令を実行する前に見つけて、ここで処理する (_EXIT, _WRITE, _PRINT, _PUTCHAR, _SUPER,
// _INTVCS, _INTVCG, _SETBLOCK, _GETDATE, _GETTIM2 など。ファイルは扱わない)。
// XEiJ の instructiontest.x (実機の結果を持つ) / fputest.x (計算で求めた期待値を持つ。FMOVECR だけは実機の値) を
// 動かすのに使う。
// --expect の文字列がすべて出力に現れれば 0、現れなければ 1 で終わる。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

#include "m68k/cpu020.h"
#include "m68k/m68k.h"

namespace {

constexpr uint32_t RAM_SIZE = 16 * 1024 * 1024;
constexpr uint32_t LOAD = 0x10000; // メモリ管理ポインタ (16 バイト) + PSP (240 バイト) の位置
constexpr uint32_t SSP_TOP = 0x8000;

class FlatBus : public m68k::Bus {
public:
    uint8_t* ram;
    FlatBus() { ram = new uint8_t[RAM_SIZE](); }
    ~FlatBus() override { delete[] ram; }
    uint8_t read8(uint32_t a, int) override { return ram[a & (RAM_SIZE - 1)]; }
    uint16_t read16(uint32_t a, int) override {
        a &= RAM_SIZE - 1;
        return (uint16_t)((ram[a] << 8) | ram[(a + 1) & (RAM_SIZE - 1)]);
    }
    void write8(uint32_t a, uint8_t v, int) override { ram[a & (RAM_SIZE - 1)] = v; }
    void write16(uint32_t a, uint16_t v, int) override {
        a &= RAM_SIZE - 1;
        ram[a] = (uint8_t)(v >> 8);
        ram[(a + 1) & (RAM_SIZE - 1)] = (uint8_t)v;
    }
    uint32_t r16(uint32_t a) const { return (uint32_t)((ram[a & (RAM_SIZE - 1)] << 8) | ram[(a + 1) & (RAM_SIZE - 1)]); }
    uint32_t r32(uint32_t a) const { return (r16(a) << 16) | r16(a + 2); }
    void w16(uint32_t a, uint32_t v) {
        ram[a & (RAM_SIZE - 1)] = (uint8_t)(v >> 8);
        ram[(a + 1) & (RAM_SIZE - 1)] = (uint8_t)v;
    }
    void w32(uint32_t a, uint32_t v) {
        w16(a, v >> 16);
        w16(a + 2, v);
    }
};

std::string g_out;
uint32_t g_textEnd = 0xffffffff;

void emit(const char* p, size_t n) {
    fwrite(p, 1, n, stdout);
    fflush(stdout);
    g_out.append(p, n);
}

template <class C> struct Runner {
    C& cpu;
    FlatBus& bus;
    bool exited = false;
    int exitCode = 0;
    uint32_t dosVec[256] = {};
    uint32_t ring[32] = {};
    uint32_t textEnd = 0xffffffff;

    Runner(C& c, FlatBus& b) : cpu(c), bus(b) {}

    void setRegs(m68k::Registers& r) {
        r.prefetch[0] = (uint16_t)bus.r16(r.pc);
        r.prefetch[1] = (uint16_t)bus.r16(r.pc + 2);
        cpu.setRegisters(r);
    }

    static constexpr uint16_t FILE_HANDLE = 5;

    // DOS コール。sp は呼び出し時のスタック (引数の先頭)
    void dos(uint16_t op, m68k::Registers& r) {
        bool super = (r.sr & 0x2000) != 0;
        uint32_t sp = super ? r.ssp : r.usp;
        uint32_t d0 = 0;
        switch (op & 0xff) {
        case 0x00: // _EXIT
            exited = true;
            exitCode = 0;
            return;
        case 0x4c: // _EXIT2
            exited = true;
            exitCode = (int)bus.r16(sp);
            return;
        case 0x02: { // _PUTCHAR
            char c = (char)bus.r16(sp);
            emit(&c, 1);
            break;
        }
        case 0x09: { // _PRINT
            uint32_t p = bus.r32(sp);
            std::string s;
            while (bus.ram[p & (RAM_SIZE - 1)]) s += (char)bus.ram[p++ & (RAM_SIZE - 1)];
            emit(s.data(), s.size());
            break;
        }
        case 0x40: { // _WRITE。ファイル (_OPEN / _CREATE で返したハンドル) への書き込みは捨てる
            uint32_t p = bus.r32(sp + 2), n = bus.r32(sp + 6);
            if (bus.r16(sp) != FILE_HANDLE) {
                std::string s;
                for (uint32_t i = 0; i < n; i++) s += (char)bus.ram[(p + i) & (RAM_SIZE - 1)];
                emit(s.data(), s.size());
            }
            d0 = n;
            break;
        }
        case 0x3c: // _CREATE
        case 0x3d: // _OPEN (fputest.x のログのファイル。中身は捨てる)
            d0 = FILE_HANDLE;
            break;
        case 0x3e: // _CLOSE
        case 0x42: // _SEEK
            d0 = 0;
            break;
        case 0x4a: // _SETBLOCK
            d0 = 0;
            break;
        case 0x20: { // _SUPER
            uint32_t arg = bus.r32(sp);
            if (arg == 0) {
                if (super) {
                    d0 = 0xffffffff; // すでにスーパーバイザ
                } else {
                    d0 = r.ssp;
                    r.ssp = r.usp;
                    r.sr |= 0x2000;
                }
            } else {
                if (super) {
                    r.usp = r.ssp;
                    r.ssp = arg;
                    r.sr &= ~0x2000;
                }
                d0 = 0;
            }
            break;
        }
        case 0x25: { // _INTVCS
            uint32_t n = bus.r16(sp), a = bus.r32(sp + 2);
            if (n < 0x100) {
                d0 = bus.r32(n * 4);
                bus.w32(n * 4, a);
            } else {
                d0 = dosVec[n & 0xff];
                dosVec[n & 0xff] = a;
            }
            break;
        }
        case 0x35: { // _INTVCG
            uint32_t n = bus.r16(sp);
            d0 = n < 0x100 ? bus.r32(n * 4) : dosVec[n & 0xff];
            break;
        }
        case 0x2a: { // _GETDATE: 2000-01-01 (土曜日)
            d0 = (6u << 16) | ((20u << 9) | (1u << 5) | 1u);
            break;
        }
        case 0x27: // _GETTIM2
            d0 = 0;
            break;
        case 0x44: // _IOCTRL
            d0 = 0;
            break;
        case 0x0b: // _KEYSNS
        case 0x10: // _KBHIT など
            d0 = 0;
            break;
        default:
            fprintf(stderr, "run_x: unsupported DOS call $%04x at $%08x\n", op, r.pc);
            d0 = 0xffffffff;
            break;
        }
        r.d[0] = d0;
        r.pc += 2;
        setRegs(r);
    }

    // trap #15 (IOCS)。d0 の番号で分ける
    bool iocs(m68k::Registers& r) {
        uint32_t n = r.d[0] & 0xff;
        if (getenv("RUN_X_IOCS")) fprintf(stderr, "iocs %02x a1=%08x\n", n, r.a[1]);
        switch (n) {
        case 0x21: { // _B_PRINT
            uint32_t p = r.a[1];
            std::string s;
            while (bus.ram[p & (RAM_SIZE - 1)]) s += (char)bus.ram[p++ & (RAM_SIZE - 1)];
            emit(s.data(), s.size());
            r.a[1] = p;
            break;
        }
        case 0x20: { // _B_PUTC
            char c = (char)r.d[1];
            emit(&c, 1);
            break;
        }
        case 0x82: // _B_BPEEK
            r.d[0] = (r.d[0] & 0xffffff00u) | bus.ram[r.a[1] & (RAM_SIZE - 1)];
            r.a[1]++;
            r.pc += 2;
            setRegs(r);
            return true;
        default: // ほかの IOCS コールは何もせず 0 を返す
            break;
        }
        r.d[0] = 0;
        r.pc += 2;
        setRegs(r);
        return true;
    }

    uint64_t run(uint64_t limit) {
        uint64_t n = 0;
        m68k::Registers r;
        while (!exited && n < limit) {
            uint32_t pc = cpu.pc_;
            if (cpu.halted()) {
                fprintf(stderr, "run_x: CPU halted at $%08x\n", pc);
                exitCode = 2;
                break;
            }
            uint16_t op = (uint16_t)bus.r16(pc);
            if ((op & 0xff00) == 0xff00) {
                cpu.getRegisters(r);
                dos(op, r);
                continue;
            }
            if (op == 0x4e4f && bus.r32(0x2f * 4) == 0) {
                cpu.getRegisters(r);
                if (iocs(r)) continue;
            }
            if (pc < 0x8000 || pc >= textEnd || (textEnd != 0xffffffff && op == 0 && bus.r16(pc + 2) == 0)) {
                fprintf(stderr, "run_x: jumped to $%06x (unset vector?)\n", pc);
                break;
            }
            ring[n & 31] = pc;
            cpu.step();
            n++;
        }
        cpu.getRegisters(r);
        if (!exited) {
            fprintf(stderr, "run_x: stopped after %llu instructions. last PCs:", (unsigned long long)n);
            for (int k = 0; k < 32; k++) fprintf(stderr, " %06x", ring[(n + k) & 31]);
            fprintf(stderr, "\n");
            uint32_t lp = ring[(n + 31) & 31];
            fprintf(stderr, "run_x: code at $%06x:", lp - 8);
            for (int k = 0; k < 12; k++) fprintf(stderr, " %04x", bus.r16(lp - 8 + 2 * k));
            fprintf(stderr, "\n");
            fprintf(stderr, "run_x: d0-7 %08x %08x %08x %08x %08x %08x %08x %08x\n", r.d[0], r.d[1], r.d[2], r.d[3], r.d[4], r.d[5], r.d[6], r.d[7]);
            fprintf(stderr, "run_x: a0-6 %08x %08x %08x %08x %08x %08x %08x usp %08x ssp %08x sr %04x\n", r.a[0], r.a[1], r.a[2], r.a[3], r.a[4], r.a[5], r.a[6], r.usp, r.ssp, r.sr);
        }
        return n;
    }
};

bool loadX(FlatBus& bus, const char* path, const std::vector<std::string>& args, m68k::Registers& r) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + got);
    fclose(f);
    if (data.size() < 64 || data[0] != 'H' || data[1] != 'U') return false;
    auto be32 = [&](size_t o) { return (uint32_t)((data[o] << 24) | (data[o + 1] << 16) | (data[o + 2] << 8) | data[o + 3]); };
    uint32_t base = be32(4), entry = be32(8), text = be32(12), dat = be32(16), bss = be32(20), rel = be32(24);
    uint32_t prog = LOAD + 256;
    if (64 + text + dat + rel > data.size()) return false;
    memcpy(bus.ram + prog, data.data() + 64, text + dat);
    memset(bus.ram + prog + text + dat, 0, bss);
    // 再配置
    uint32_t delta = prog - base;
    size_t p = 64 + text + dat, end = p + rel;
    uint32_t at = prog;
    while (p + 1 < end) {
        uint32_t w = (uint32_t)((data[p] << 8) | data[p + 1]);
        p += 2;
        if (w == 1) {
            w = be32(p);
            p += 4;
        }
        if (w & 1) {
            at += w - 1;
            bus.w16(at, bus.r16(at) + delta);
        } else {
            at += w;
            bus.w32(at, bus.r32(at) + delta);
        }
    }
    uint32_t progEnd = prog + text + dat + bss;
    if (getenv("RUN_X_TEXT_CHECK")) g_textEnd = (uint32_t)strtoul(getenv("RUN_X_TEXT_CHECK"), nullptr, 0);
    // コマンドライン (LASCII)
    uint32_t cmd = progEnd + 16;
    std::string line;
    for (size_t i = 0; i < args.size(); i++) {
        if (i) line += ' ';
        line += args[i];
    }
    if (line.size() > 255) line.resize(255);
    bus.ram[cmd] = (uint8_t)line.size();
    memcpy(bus.ram + cmd + 1, line.data(), line.size());
    bus.ram[cmd + 1 + line.size()] = 0;
    uint32_t env = (cmd + 0x110) & ~1u;
    bus.w32(env, 0x100); // 環境の大きさ
    uint32_t stackTop = (env + 0x10000) & ~3u;
    memset(&r, 0, sizeof r);
    r.a[0] = LOAD;
    r.a[1] = progEnd;
    r.a[2] = cmd;
    r.a[3] = env;
    r.a[4] = prog + entry;
    r.usp = stackTop;
    r.ssp = SSP_TOP;
    r.sr = 0x0000;
    r.pc = prog + entry;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    int cpuType = 68030;
    uint64_t limit = 20000000000ull;
    std::vector<std::string> expects;
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--cpu") && i + 1 < argc) cpuType = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = strtoull(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "--expect") && i + 1 < argc) expects.push_back(argv[++i]);
        else break;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: run_x [--cpu 68000|68030|68060] [--limit N] [--expect S]... file.x [args...]\n");
        return 2;
    }
    const char* path = argv[i++];
    std::vector<std::string> args(argv + i, argv + argc);
    FlatBus bus;
    m68k::Registers r;
    if (!loadX(bus, path, args, r)) {
        fprintf(stderr, "run_x: cannot load %s\n", path);
        return 77;
    }
    // 例外ベクタは、何もしないで止まる場所にしておく (未設定の例外が起きたら分かるように)
    for (int v = 2; v < 256; v++) bus.w32(v * 4, 0);
    // IOCS のワークの MPU の種類 ($0CBC) と FPU の有無 ($0CBD)。fputest.x が見る
    bus.ram[0xcbc] = (uint8_t)(cpuType == 68060 ? 6 : cpuType == 68030 ? 3 : 0);
    bus.ram[0xcbd] = cpuType == 68000 ? 0 : 0xff;
    int code;
    clock_t t0 = clock();
    uint64_t n;
    if (cpuType == 68000) {
        m68k::Cpu cpu(&bus);
        for (int p = 0; p < 256; p++) cpu.setFastPage(true, p, bus.ram + p * 65536, bus.ram + p * 65536),
                                     cpu.setFastPage(false, p, bus.ram + p * 65536, bus.ram + p * 65536);
        Runner<m68k::Cpu> run(cpu, bus);
        run.textEnd = g_textEnd;
        run.setRegs(r);
        n = run.run(limit);
        code = run.exitCode;
        if (!run.exited) code = 3;
    } else {
        m68k::Cpu020 cpu(&bus, cpuType == 68060 ? m68k::CpuModel::M68060 : m68k::CpuModel::M68030);
        for (int p = 0; p < 256; p++) cpu.setFastPage(true, p, bus.ram + p * 65536, bus.ram + p * 65536),
                                     cpu.setFastPage(false, p, bus.ram + p * 65536, bus.ram + p * 65536);
        // 68060 の未実装整数命令は、060turbo.sys が組み込む 060SP の代わりにその場で実行する
        cpu.setUnimplementedAssist(m68k::Cpu020::ASSIST_ALWAYS);
        Runner<m68k::Cpu020> run(cpu, bus);
        run.textEnd = g_textEnd;
        run.setRegs(r);
        n = run.run(limit);
        code = run.exitCode;
        if (!run.exited) code = 3;
    }
    double sec = (double)(clock() - t0) / CLOCKS_PER_SEC;
    // 終わったときの RAM の全体のハッシュ (FNV-1a)。命令のサイクル数だけを変える変更の前後で、実行した命令の数と
    // この値が同じなら、命令の結果は変わっていない (run_x には割り込みがないので、サイクル数は実行の流れを変えない)
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t i = 0; i < RAM_SIZE; i++) hash = (hash ^ bus.ram[i]) * 1099511628211ull;
    fprintf(stderr, "run_x: %llu instructions in %.2f s (exit %d), ram hash %016llx\n", (unsigned long long)n, sec, code,
            (unsigned long long)hash);
    for (const std::string& e : expects) {
        if (g_out.find(e) == std::string::npos) {
            fprintf(stderr, "run_x: expected text not found: %s\n", e.c_str());
            return 1;
        }
    }
    return code;
}
