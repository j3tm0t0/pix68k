// SingleStepTests (68000) のランナー
//   使い方: sst_runner [-v N] [-c] [-a] <file.sst>...
//     -v N  失敗したテストを最初の N 件まで詳しく表示する
//     -c    サイクル数 (length) も比較する
//     -x    SSP が変わるテストを除外して数える
//     -e F  既知の不一致の一覧 (1 行に「ファイル名<TAB>テスト名<TAB>理由」)。
//           一覧のテストは失敗してよい。一覧にないテストの失敗と、一覧にあるのに合格したテストを数え、
//           どちらも 0 なら終了コード 0 を返す
//     -l    失敗したテストを「ファイル名<TAB>テスト名」で出力する
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <set>
#include <string>
#include <vector>

#include "m68k/m68k.h"

namespace {

struct State {
    uint32_t r[22];
    std::vector<std::pair<uint32_t, uint8_t>> ram;
};

struct Test {
    std::string name;
    State init, fin;
    uint32_t length;
};

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    uint32_t u32() {
        uint32_t v = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
        p += 4;
        return v;
    }
    uint16_t u16() {
        uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
        p += 2;
        return v;
    }
    uint8_t u8() { return *p++; }
};

void readState(Reader& rd, State& s) {
    for (int i = 0; i < 22; i++) s.r[i] = rd.u32();
    uint32_t n = s.r[21];
    s.ram.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = rd.u32();
        s.ram[i] = {a, rd.u8()};
    }
}

bool loadFile(const char* path, std::vector<Test>& tests) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(size);
    if (fread(buf.data(), 1, size, f) != (size_t)size) {
        fclose(f);
        return false;
    }
    fclose(f);
    if (memcmp(buf.data(), "SST1", 4) != 0) return false;
    Reader rd{buf.data() + 4, buf.data() + size};
    uint32_t count = rd.u32();
    tests.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        Test& t = tests[i];
        uint16_t len = rd.u16();
        t.name.assign((const char*)rd.p, len);
        rd.p += len;
        readState(rd, t.init);
        readState(rd, t.fin);
        t.length = rd.u32();
        uint32_t ntr = rd.u32();
        rd.p += ntr * 16;
    }
    return true;
}

class TestBus : public m68k::Bus {
public:
    uint8_t mem[1 << 24];
    std::vector<uint32_t> written;
    uint8_t read8(uint32_t a, int) override { return mem[a]; }
    uint16_t read16(uint32_t a, int) override { return (uint16_t)((mem[a] << 8) | mem[(a + 1) & 0xffffff]); }
    void write8(uint32_t a, uint8_t v, int) override {
        written.push_back(a);
        mem[a] = v;
    }
    void write16(uint32_t a, uint16_t v, int) override {
        written.push_back(a);
        written.push_back((a + 1) & 0xffffff);
        mem[a] = (uint8_t)(v >> 8);
        mem[(a + 1) & 0xffffff] = (uint8_t)v;
    }
};

const char* kRegNames[] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "a0", "a1", "a2",
                           "a3", "a4", "a5", "a6", "usp", "ssp", "sr", "pc", "pf0", "pf1"};

void toRegs(const State& s, m68k::Registers& r) {
    for (int i = 0; i < 8; i++) r.d[i] = s.r[i];
    for (int i = 0; i < 7; i++) r.a[i] = s.r[8 + i];
    r.usp = s.r[15];
    r.ssp = s.r[16];
    r.sr = (uint16_t)s.r[17];
    r.pc = s.r[18];
    r.prefetch[0] = (uint16_t)s.r[19];
    r.prefetch[1] = (uint16_t)s.r[20];
}

void fromRegs(const m68k::Registers& r, uint32_t* out) {
    for (int i = 0; i < 8; i++) out[i] = r.d[i];
    for (int i = 0; i < 7; i++) out[8 + i] = r.a[i];
    out[15] = r.usp;
    out[16] = r.ssp;
    out[17] = r.sr;
    out[18] = r.pc;
    out[19] = r.prefetch[0];
    out[20] = r.prefetch[1];
}

} // namespace

int main(int argc, char** argv) {
    int verbose = 0;
    bool checkCycles = false, excludeExc = false, list = false;
    const char* expectedPath = nullptr;
    std::vector<const char*> files;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") && i + 1 < argc)
            verbose = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c"))
            checkCycles = true;
        else if (!strcmp(argv[i], "-x"))
            excludeExc = true;
        else if (!strcmp(argv[i], "-l"))
            list = true;
        else if (!strcmp(argv[i], "-e") && i + 1 < argc)
            expectedPath = argv[++i];
        else
            files.push_back(argv[i]);
    }
    std::set<std::string> expected;
    if (expectedPath) {
        FILE* f = fopen(expectedPath, "r");
        if (!f) {
            fprintf(stderr, "cannot open %s\n", expectedPath);
            return 2;
        }
        char line[512];
        while (fgets(line, sizeof line, f)) {
            if (line[0] == '#' || line[0] == '\n') continue;
            char* t1 = strchr(line, '\t');
            if (!t1) continue;
            char* t2 = strchr(t1 + 1, '\t');
            if (!t2) t2 = strchr(t1 + 1, '\n');
            if (!t2) continue;
            expected.insert(std::string(line, t1 - line) + "\t" + std::string(t1 + 1, t2 - t1 - 1));
        }
        fclose(f);
    }
    long unexpectedFail = 0, unexpectedPass = 0;
    static TestBus bus;
    memset(bus.mem, 0, sizeof bus.mem);
    m68k::Cpu cpu(&bus);
    long totalPass = 0, totalRun = 0, totalCycleFail = 0;
    int shown = 0;
    for (const char* path : files) {
        std::vector<Test> tests;
        if (!loadFile(path, tests)) {
            fprintf(stderr, "cannot load %s\n", path);
            return 2;
        }
        long pass = 0, run = 0, cycleFail = 0;
        for (const Test& t : tests) {
            bool exc = t.init.r[16] != t.fin.r[16] || t.init.r[15] != t.fin.r[15];
            if (excludeExc && exc) continue;
            for (auto& m : t.init.ram) bus.mem[m.first & 0xffffff] = m.second;
            m68k::Registers r;
            toRegs(t.init, r);
            cpu.setRegisters(r);
            uint64_t c0 = cpu.cycles();
            cpu.step();
            uint32_t used = (uint32_t)(cpu.cycles() - c0);
            m68k::Registers out;
            cpu.getRegisters(out);
            uint32_t got[21];
            fromRegs(out, got);
            bool ok = true;
            for (int i = 0; i < 21; i++)
                if (got[i] != t.fin.r[i]) ok = false;
            for (auto& m : t.fin.ram)
                if (bus.mem[m.first & 0xffffff] != m.second) ok = false;
            bool cyc = used == t.length;
            if (!cyc) cycleFail++;
            if (checkCycles && !cyc) ok = false;
            run++;
            if (ok) pass++;
            const char* base0 = strrchr(path, '/');
            std::string key = std::string(base0 ? base0 + 1 : path) + "\t" + t.name;
            bool isExpected = expected.count(key) != 0;
            if (expectedPath) {
                if (!ok && !isExpected) unexpectedFail++;
                if (ok && isExpected) {
                    unexpectedPass++;
                    printf("UNEXPECTED PASS %s\n", key.c_str());
                }
            }
            if (!ok && list) printf("%s\n", key.c_str());
            if (!isExpected && !ok && shown < verbose) {
                shown++;
                printf("FAIL %s: %s\n", path, t.name.c_str());
                for (int i = 0; i < 21; i++)
                    if (got[i] != t.fin.r[i])
                        printf("  %-4s init=%08x want=%08x got=%08x\n", kRegNames[i], t.init.r[i], t.fin.r[i], got[i]);
                for (auto& m : t.fin.ram)
                    if (bus.mem[m.first & 0xffffff] != m.second)
                        printf("  ram[%06x] want=%02x got=%02x\n", m.first, m.second, bus.mem[m.first & 0xffffff]);
                if (!cyc) printf("  cycles want=%u got=%u\n", t.length, used);
            }
            // 後始末: 初期化したメモリと CPU が書き込んだメモリを 0 に戻す (既知の不一致のテストでも必ず行う)
            for (auto& m : t.init.ram) bus.mem[m.first & 0xffffff] = 0;
            for (auto& m : t.fin.ram) bus.mem[m.first & 0xffffff] = 0;
            for (uint32_t a : bus.written) bus.mem[a] = 0;
            bus.written.clear();
        }
        const char* base = strrchr(path, '/');
        printf("%-22s %5ld/%5ld %s cyc-mismatch=%ld\n", base ? base + 1 : path, pass, run,
               pass == run ? "OK  " : "FAIL", cycleFail);
        totalPass += pass;
        totalRun += run;
        totalCycleFail += cycleFail;
    }
    printf("TOTAL %ld/%ld (%.3f%%) cyc-mismatch=%ld\n", totalPass, totalRun,
           totalRun ? 100.0 * totalPass / totalRun : 0.0, totalCycleFail);
    if (expectedPath) {
        printf("known discrepancies=%zu unexpected-fail=%ld unexpected-pass=%ld\n", expected.size(), unexpectedFail,
               unexpectedPass);
        return unexpectedFail == 0 && unexpectedPass == 0 ? 0 : 1;
    }
    return totalPass == totalRun ? 0 : 1;
}
