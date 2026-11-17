// 68030 のコア (Cpu020) と Musashi の 68030 を、ランダムな命令 1 つずつで比べる (差分テスト)。
//
//   musashi_diff [-n 回数] [-s 種] [-v 例の数] [-u] [-e 既知の違いのファイル]
//
// -e を付けると、ファイルに書いた命令の名前ごとの違い (知っていて直していない違い) と比べ、違いの出た命令の集まりが
// 変わったら終了コード 1 を返す (CI で使う。種と回数を変えると変わる)。ファイルの形は 1 行に「名前 数 ハッシュ」で、
// # から後はコメント。ハッシュは、その命令の名前で違いが出たテストの番号 (何回目のランダムな命令か) の並びの
// FNV-1a で、数が同じでも、既知の違いが 1 つ消えて別の違いが 1 つ出れば変わる。違いが出た命令のこの 3 つは、
// 結果の表の「expect:」の行に出る (ファイルを直すときはそれを写す)
//
// 命令の語と拡張語、レジスタ、CCR、メモリをランダムに作り、両方で 1 命令だけ実行して、レジスタ (D、A、USP、ISP、
// MSP)、PC、SR、書き込んだメモリを比べる。違いは命令の名前 (Musashi の逆アセンブラ) ごとに数えて表示する。
// Musashi は XEiJ ほど実機に忠実ではない (未定義のフラグ、BCD、ビットフィールドの端など) ので、違いは
// XEiJ の instructiontest.x (実機の結果) で確かめてから直すこと。FPU (コプロセッサ ID 1) と MMU の命令は比べない。
// Musashi はリポジトリに入れない。CMake の MUSASHI_DIR に、m68kmake で m68kops.c を作った Musashi を置く。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "m68k/cpu020.h"

extern "C" {
#include "m68k.h"
}

namespace {

constexpr uint32_t RAM_SIZE = 1 << 20;
constexpr uint32_t MASK = RAM_SIZE - 1;
constexpr uint32_t CODE = 0x1000;
constexpr uint32_t ISP = 0x7f000, MSP = 0x7e000, USP = 0x7d000;

uint8_t pristine[RAM_SIZE];
uint8_t memM[RAM_SIZE], memO[RAM_SIZE];
std::vector<uint32_t> writesM, writesO;

uint64_t rngState = 1;
uint32_t rnd() {
    rngState ^= rngState << 13;
    rngState ^= rngState >> 7;
    rngState ^= rngState << 17;
    return (uint32_t)(rngState >> 16);
}

class DiffBus : public m68k::Bus {
public:
    uint8_t read8(uint32_t a, int) override { return memO[a & MASK]; }
    uint16_t read16(uint32_t a, int) override { return (uint16_t)((memO[a & MASK] << 8) | memO[(a + 1) & MASK]); }
    void write8(uint32_t a, uint8_t v, int) override {
        memO[a & MASK] = v;
        writesO.push_back(a & MASK);
    }
    void write16(uint32_t a, uint16_t v, int) override {
        write8(a, (uint8_t)(v >> 8), 0);
        write8(a + 1, (uint8_t)v, 0);
    }
};

uint32_t randomData() {
    switch (rnd() % 8) {
    case 0: return 0;
    case 1: return 0xffffffffu;
    case 2: return 0x80000000u;
    case 3: return 0x7fffffffu;
    case 4: return rnd() & 0xff;
    case 5: return rnd() & 0xffff;
    default: return rnd() << 16 ^ rnd();
    }
}

bool skipOpcode(uint16_t op) {
    if ((op & 0xf000) == 0xf000) return true; // コプロセッサ (FPU、MMU) と F ライン
    if (op == 0x4e70 || op == 0x4e72) return true; // RESET、STOP
    if ((op & 0xfff8) == 0x4848) return true;      // BKPT
    return false;
}

struct State {
    uint32_t d[8], a[8], usp, isp, msp, pc, sr;
};

} // namespace

extern "C" {
unsigned int m68k_read_memory_8(unsigned int a) { return memM[a & MASK]; }
unsigned int m68k_read_memory_16(unsigned int a) { return (memM[a & MASK] << 8) | memM[(a + 1) & MASK]; }
unsigned int m68k_read_memory_32(unsigned int a) { return (m68k_read_memory_16(a) << 16) | m68k_read_memory_16(a + 2); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return m68k_read_memory_8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) {
    memM[a & MASK] = (uint8_t)v;
    writesM.push_back(a & MASK);
}
void m68k_write_memory_16(unsigned int a, unsigned int v) {
    m68k_write_memory_8(a, v >> 8);
    m68k_write_memory_8(a + 1, v);
}
void m68k_write_memory_32(unsigned int a, unsigned int v) {
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16(a + 2, v);
}
}

int main(int argc, char** argv) {
    long count = 200000, examples = 2;
    bool userToo = false;
    const char* expectedPath = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) count = atol(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) rngState = strtoull(argv[++i], nullptr, 0) | 1;
        else if (!strcmp(argv[i], "-v") && i + 1 < argc) examples = atol(argv[++i]);
        else if (!strcmp(argv[i], "-u")) userToo = true;
        else if (!strcmp(argv[i], "-e") && i + 1 < argc) expectedPath = argv[++i];
        else {
            fprintf(stderr, "usage: %s [-n count] [-s seed] [-v examples] [-u] [-e expected]\n", argv[0]);
            return 2;
        }
    }
    for (uint32_t i = 0; i < RAM_SIZE; i++) pristine[i] = (uint8_t)rnd();
    // ベクタ: n 番は $40000 + n * 16 (偶数)
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t v = 0x40000 + n * 16;
        pristine[n * 4] = (uint8_t)(v >> 24);
        pristine[n * 4 + 1] = (uint8_t)(v >> 16);
        pristine[n * 4 + 2] = (uint8_t)(v >> 8);
        pristine[n * 4 + 3] = (uint8_t)v;
    }
    pristine[0] = 0, pristine[1] = 0x07, pristine[2] = 0xf0, pristine[3] = 0; // リセットの SSP
    memcpy(memM, pristine, RAM_SIZE);
    memcpy(memO, pristine, RAM_SIZE);

    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68030);
    m68k_pulse_reset();
    // m68k_pulse_reset() はリセットの例外のサイクル数を RESET_CYCLES に入れ、次の m68k_execute() はそれを使い切るだけで
    // 命令を実行せずに戻る (m68kcpu.c の m68k_execute())。ここで使い切っておかないと、最初のテストで Musashi が
    // 命令を実行せず、違いに数えてしまう (前の musashi_expected.txt の sub.l の 1 件)
    m68k_execute(1);
    DiffBus bus;
    m68k::Cpu020 cpu(&bus, m68k::CpuModel::M68030);
    cpu.reset();

    struct Stat {
        long total = 0, bad = 0;
        uint32_t hash = 2166136261u; // 違いの出たテストの番号の FNV-1a
        std::vector<std::string> ex;
    };
    std::map<std::string, Stat> stats;
    long tested = 0, bad = 0, skippedAddressError = 0;
    for (long t = 0; t < count; t++) {
        uint16_t words[6];
        do words[0] = (uint16_t)rnd();
        while (skipOpcode(words[0]));
        for (int i = 1; i < 6; i++) words[i] = (uint16_t)rnd();
        for (int i = 0; i < 6; i++) {
            memM[CODE + i * 2] = memO[CODE + i * 2] = (uint8_t)(words[i] >> 8);
            memM[CODE + i * 2 + 1] = memO[CODE + i * 2 + 1] = (uint8_t)words[i];
        }
        State in;
        for (int i = 0; i < 8; i++) in.d[i] = randomData();
        for (int i = 0; i < 7; i++) in.a[i] = (rnd() % 4 == 0) ? randomData() : (0x10000 + (rnd() % 0x60000)) & ~1u;
        bool user = userToo && rnd() % 4 == 0;
        bool master = !user && rnd() % 8 == 0;
        in.sr = (user ? 0 : 0x2000) | (master ? 0x1000 : 0) | ((rnd() % 8) << 8) | (rnd() & 0x1f);
        in.usp = USP;
        in.isp = ISP;
        in.msp = MSP;
        in.pc = CODE;

        char dis[256];
        m68k_disassemble(dis, CODE, M68K_CPU_TYPE_68030);
        std::string name(dis);
        size_t sp = name.find(' ');
        if (sp != std::string::npos) name.resize(sp);

        // Musashi
        m68k_set_reg(M68K_REG_SR, in.sr);
        m68k_set_reg(M68K_REG_USP, in.usp);
        m68k_set_reg(M68K_REG_ISP, in.isp);
        m68k_set_reg(M68K_REG_MSP, in.msp);
        m68k_set_reg(M68K_REG_VBR, 0);
        for (int i = 0; i < 8; i++) m68k_set_reg((m68k_register_t)(M68K_REG_D0 + i), in.d[i]);
        for (int i = 0; i < 7; i++) m68k_set_reg((m68k_register_t)(M68K_REG_A0 + i), in.a[i]);
        m68k_set_reg(M68K_REG_PC, in.pc);
        m68k_execute(1);
        State om;
        for (int i = 0; i < 8; i++) om.d[i] = m68k_get_reg(nullptr, (m68k_register_t)(M68K_REG_D0 + i));
        for (int i = 0; i < 8; i++) om.a[i] = m68k_get_reg(nullptr, (m68k_register_t)(M68K_REG_A0 + i));
        om.usp = m68k_get_reg(nullptr, M68K_REG_USP);
        om.isp = m68k_get_reg(nullptr, M68K_REG_ISP);
        om.msp = m68k_get_reg(nullptr, M68K_REG_MSP);
        om.pc = m68k_get_reg(nullptr, M68K_REG_PC);
        om.sr = m68k_get_reg(nullptr, M68K_REG_SR);

        // Cpu020
        cpu.setSR((uint16_t)in.sr);
        for (int i = 0; i < 8; i++) cpu.d_[i] = in.d[i];
        for (int i = 0; i < 7; i++) cpu.a_[i] = in.a[i];
        cpu.usp_ = in.usp;
        cpu.isp_ = in.isp;
        cpu.msp_ = in.msp;
        cpu.a_[7] = user ? in.usp : master ? in.msp : in.isp;
        cpu.vbr_ = 0;
        cpu.pc_ = in.pc;
        cpu.step();
        State oo;
        for (int i = 0; i < 8; i++) oo.d[i] = cpu.d_[i];
        for (int i = 0; i < 8; i++) oo.a[i] = cpu.a_[i];
        oo.usp = cpu.sflag_ ? cpu.usp_ : cpu.a_[7];
        oo.isp = cpu.sflag_ && !cpu.mflag_ ? cpu.a_[7] : cpu.isp_;
        oo.msp = cpu.sflag_ && cpu.mflag_ ? cpu.a_[7] : cpu.msp_;
        oo.pc = cpu.pc_;
        oo.sr = cpu.getSR();

        // Musashi はアドレスエラー (奇数番地への分岐など) を扱わないので比べない
        const bool addressError = oo.pc == 0x40000 + 3 * 16;
        std::string diff;
        char buf[128];
        for (int i = 0; i < 8; i++)
            if (om.d[i] != oo.d[i]) snprintf(buf, sizeof buf, " D%d:%08x/%08x", i, om.d[i], oo.d[i]), diff += buf;
        for (int i = 0; i < 8; i++)
            if (om.a[i] != oo.a[i]) snprintf(buf, sizeof buf, " A%d:%08x/%08x", i, om.a[i], oo.a[i]), diff += buf;
        if (om.usp != oo.usp) snprintf(buf, sizeof buf, " USP:%08x/%08x", om.usp, oo.usp), diff += buf;
        if (om.isp != oo.isp) snprintf(buf, sizeof buf, " ISP:%08x/%08x", om.isp, oo.isp), diff += buf;
        if (om.msp != oo.msp) snprintf(buf, sizeof buf, " MSP:%08x/%08x", om.msp, oo.msp), diff += buf;
        if (om.pc != oo.pc) snprintf(buf, sizeof buf, " PC:%08x/%08x", om.pc, oo.pc), diff += buf;
        if (om.sr != oo.sr) snprintf(buf, sizeof buf, " SR:%04x/%04x", om.sr, oo.sr), diff += buf;
        int memDiff = 0;
        for (uint32_t a : writesM)
            if (memM[a] != memO[a] && memDiff++ < 4) snprintf(buf, sizeof buf, " [%05x]:%02x/%02x", a, memM[a], memO[a]), diff += buf;
        for (uint32_t a : writesO)
            if (memM[a] != memO[a] && memDiff++ < 4) snprintf(buf, sizeof buf, " [%05x]:%02x/%02x", a, memM[a], memO[a]), diff += buf;
        for (uint32_t a : writesM) memM[a] = memO[a] = pristine[a];
        for (uint32_t a : writesO) memM[a] = memO[a] = pristine[a];
        writesM.clear();
        writesO.clear();

        if (addressError) {
            skippedAddressError++;
            continue;
        }
        Stat& s = stats[name];
        s.total++;
        tested++;
        if (!diff.empty()) {
            s.bad++;
            bad++;
            for (int i = 0; i < 4; i++) s.hash = (s.hash ^ (uint8_t)(t >> (i * 8))) * 16777619u;
            if ((long)s.ex.size() < examples) {
                char head[512];
                snprintf(head, sizeof head, "    %-40s %04x %04x %04x %04x sr=%04x d=%08x,%08x a=%08x,%08x:", dis, words[0],
                         words[1], words[2], words[3], in.sr, in.d[0], in.d[1], in.a[0], in.a[1]);
                s.ex.push_back(std::string(head) + diff + "  (Musashi/ours)");
            }
        }
    }
    std::vector<std::pair<long, std::string>> order;
    for (auto& kv : stats)
        if (kv.second.bad) order.push_back({kv.second.bad, kv.first});
    std::sort(order.begin(), order.end(), [](auto& x, auto& y) { return x.first > y.first; });
    for (auto& o : order) {
        Stat& s = stats[o.second];
        printf("%-10s %6ld / %6ld differ   expect: %s %ld %08x\n", o.second.c_str(), s.bad, s.total, o.second.c_str(), s.bad,
               s.hash);
        for (auto& e : s.ex) printf("%s\n", e.c_str());
    }
    printf("musashi_diff: %ld instructions, %ld differ (%.2f%%), %ld address errors not compared\n", tested, bad,
           tested ? 100.0 * bad / tested : 0.0, skippedAddressError);
    if (!expectedPath) return 0;
    FILE* f = fopen(expectedPath, "r");
    if (!f) {
        fprintf(stderr, "musashi_diff: %s を開けない\n", expectedPath);
        return 2;
    }
    std::map<std::string, std::pair<long, uint32_t>> expected;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (char* hash = strchr(line, '#')) *hash = 0;
        char name[64];
        long n;
        unsigned h;
        if (sscanf(line, "%63s %ld %x", name, &n, &h) == 3) expected[name] = {n, h};
    }
    fclose(f);
    int failures = 0;
    for (auto& kv : stats) {
        if (!kv.second.bad) continue;
        auto it = expected.find(kv.first);
        if (it == expected.end()) {
            printf("NEW: %s %ld differ\n", kv.first.c_str(), kv.second.bad);
            failures++;
        } else if (it->second.first != kv.second.bad || it->second.second != kv.second.hash) {
            printf("CHANGED: %s %ld differ %08x (expected %ld %08x)\n", kv.first.c_str(), kv.second.bad, kv.second.hash,
                   it->second.first, it->second.second);
            failures++;
        }
    }
    for (auto& kv : expected) {
        if (stats.count(kv.first) && stats[kv.first].bad) continue;
        printf("GONE: %s (expected %ld differ)\n", kv.first.c_str(), kv.second.first);
        failures++;
    }
    if (failures) return 1;
    printf("musashi_diff: no differences beyond %s\n", expectedPath);
    return 0;
}
