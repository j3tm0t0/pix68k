// コントラスト ($E8E001) の試験のうち、Machine を通すもの: 書き込みと読み戻し、RESET 命令、ステートの新旧、
// 合成するコアへの引き渡し (RenderLink)、書き込みとラスタの順序。画面の色は video_test の testContrast()
#include <stdio.h>
#include <string.h>

#include <vector>

#include "test_check.h"
#include "x68k/machine.h"
#include "x68k/render.h"
#include "x68k/state.h"
#include "x68k/video_journal.h"
#include "x68k/video_replay.h"

namespace {

const uint32_t PORT = 0xe8e001;

// $E8E000 のワードに書いた時刻を覚える Machine
struct Probe : x68k::Machine {
    uint64_t wroteAt = 0;
    void write16(uint32_t addr, uint16_t value, int fc) override {
        if (addr == PORT - 1) wroteAt = cycles();
        x68k::Machine::write16(addr, value, fc);
    }
};

// プログラムを $FF0010 に置いた IPL ROM で起動する
void boot(x68k::Machine& m, const std::vector<uint16_t>& program) {
    static uint8_t ipl[x68k::Machine::IPL_SIZE];
    memset(ipl, 0xff, sizeof ipl);
    const uint8_t vectors[8] = {0x00, 0x00, 0x20, 0x00, 0x00, 0xff, 0x00, 0x10};
    memcpy(ipl + 0x10000, vectors, sizeof vectors);
    size_t at = 0x10010;
    for (uint16_t w : program) {
        ipl[at++] = (uint8_t)(w >> 8);
        ipl[at++] = (uint8_t)w;
    }
    ipl[at++] = 0x60; // bra.s *
    ipl[at++] = 0xfe;
    m.setIplRom(ipl, sizeof ipl);
    m.powerOn();
}

uint8_t port(x68k::Machine& m) { return m.read8(PORT, m68k::FC_SUPER_DATA); }
void setPort(x68k::Machine& m, uint8_t v) { m.write8(PORT, v, m68k::FC_SUPER_DATA); }

void testPort() {
    printf("write, read back, RESET instruction, power on\n");
    auto* m = new x68k::Machine;
    CHECK_EQ(port(*m), 0xfe); // 電源を入れる前も、SysPort と Video は同じ
    CHECK_EQ(m->video().contrast(), 14);
    // move.b #3,$e8e001 / reset
    boot(*m, {0x13fc, 0x0003, 0x00e8, 0xe001, 0x4e70});
    CHECK_EQ(port(*m), 0xfe);
    CHECK_EQ(m->video().contrast(), 14);
    // システムポートのほかのレジスタは RESET 命令で戻る ($E8E005 = 0 になる)
    m->write8(0xe8e005, 0x1f, m68k::FC_SUPER_DATA);
    m->run(2000);
    CHECK_EQ(m->read8(0xe8e005, m68k::FC_SUPER_DATA), 0xe0);
    CHECK_EQ(port(*m), 0xf3); // コントラストは残る
    CHECK_EQ(m->video().contrast(), 3);
    setPort(*m, 0xa5); // 上の 4 ビットは捨てる
    CHECK_EQ(port(*m), 0xf5);
    CHECK_EQ(m->video().contrast(), 5);
    m->write16(PORT - 1, 0x0009, m68k::FC_SUPER_DATA);
    CHECK_EQ(m->video().contrast(), 9);
    m->write8(0xe8e003, 0x0b, m68k::FC_SUPER_DATA); // 隣のレジスタ
    CHECK_EQ(m->video().contrast(), 9);
    setPort(*m, 0);
    m->powerOn(); // リセットボタンと電源の入れ直し
    CHECK_EQ(port(*m), 0xfe);
    CHECK_EQ(m->video().contrast(), 14);
    delete m;
}

std::vector<uint8_t> save(x68k::Machine& m, uint32_t version) {
    std::vector<uint8_t> buf(x68k::stateSize(m) + 4096);
    size_t n = 0;
    CHECK_EQ(x68k::saveState(m, buf.data(), buf.size(), n, version), true);
    buf.resize(n);
    return buf;
}

void testState() {
    printf("states, old and new\n");
    auto* a = new x68k::Machine;
    auto* b = new x68k::Machine;
    boot(*a, {});
    boot(*b, {});
    const uint32_t now = x68k::STATE_VERSION, old = x68k::STATE_CONTRAST - 1;
    const struct {
        uint8_t value, before;
        uint32_t version;
        uint8_t want;
    } cases[] = {
        {0, 14, now, 0},  {0, 14, old, 14}, {0, 5, old, 14}, {5, 14, now, 5}, {5, 14, old, 5},
        {14, 0, now, 14}, {14, 0, old, 14}, {15, 0, now, 15}, {15, 0, old, 15},
    };
    for (const auto& c : cases) {
        setPort(*a, c.value);
        const std::vector<uint8_t> data = save(*a, c.version);
        setPort(*b, c.before);
        const char* err = nullptr;
        CHECK_EQ(x68k::loadState(*b, data.data(), data.size(), err), true);
        CHECK_EQ(port(*b), 0xf0 | c.want);
        CHECK_EQ(b->video().contrast(), c.want);
    }
    // 読んだあとの書き込みも画面に届く
    setPort(*b, 6);
    CHECK_EQ(b->video().contrast(), 6);
    // 読めないステートは何も変えない
    std::vector<uint8_t> bad = save(*a, now);
    bad[bad.size() - 1] ^= 1;
    const char* err = nullptr;
    CHECK_EQ(x68k::loadState(*b, bad.data(), bad.size(), err), false);
    CHECK_EQ(port(*b), 0xf6);
    CHECK_EQ(b->video().contrast(), 6);
    // 反映する前の実装が書いた固定データ (中身は 0)
    for (const char* name : {"neptune_v10.state", "neptune_v13.state"}) {
        char path[1024];
        snprintf(path, sizeof path, "%s/tests/data/%s", PIX68K_SOURCE_DIR, name);
        FILE* f = fopen(path, "rb");
        CHECK_EQ(f != nullptr, true);
        if (!f) continue;
        std::vector<uint8_t> data(1 << 20);
        const size_t len = fread(data.data(), 1, data.size(), f);
        fclose(f);
        auto* m = new x68k::Machine;
        m->setNeptune(true);
        setPort(*m, 3);
        CHECK_EQ(x68k::loadState(*m, data.data(), len, err), true);
        CHECK_EQ(port(*m), 0xfe);
        CHECK_EQ(m->video().contrast(), 14);
        delete m;
    }
    delete a;
    delete b;
}

void testRenderLink() {
    printf("RenderLink: attach, writes, RESET, state load\n");
    auto* m = new x68k::Machine;
    boot(*m, {0x4e70}); // reset
    setPort(*m, 0);
    const std::vector<uint8_t> black = save(*m, x68k::STATE_VERSION);
    setPort(*m, 5);
    auto* shadow = new x68k::Video;
    auto* renderer = new x68k::Renderer(*shadow);
    static uint32_t ring[1u << 14];
    x68k::VideoJournal journal;
    journal.init(ring, 14);
    x68k::VideoReplayer replayer(journal, *shadow, *renderer);
    x68k::RenderLink link;
    auto drain = [&] {
        journal.publish();
        while (replayer.step()) {
        }
    };
    link.attach(*m, journal, *shadow, replayer);
    CHECK_EQ(shadow->contrast(), 5); // つないだときの値が写る
    m->run(1000);                    // RESET 命令
    drain();
    CHECK_EQ(shadow->contrast(), 5);
    setPort(*m, 9);
    drain();
    CHECK_EQ(shadow->contrast(), 9);
    const char* err = nullptr;
    CHECK_EQ(x68k::loadState(*m, black.data(), black.size(), err), true);
    drain();
    CHECK_EQ(shadow->contrast(), 0);
    m->powerOn();
    drain();
    CHECK_EQ(shadow->contrast(), 14);
    link.detach(*m);
    delete m;
    delete renderer;
    delete shadow;
}

// ラスタを描く時刻と、そのときのコントラスト
struct Rasters : x68k::LineListener {
    x68k::Machine& m;
    std::vector<uint64_t> at;
    std::vector<uint8_t> contrast;
    explicit Rasters(x68k::Machine& machine) : m(machine) {}
    void onRaster(int) override {
        at.push_back(m.cycles());
        contrast.push_back(m.video().contrast());
    }
    void onFrameEnd() override {}
};

// 長い命令 (MOVEM。書き終わるまで 120 サイクルほど) の最後にコントラストを書く。命令の途中で水平表示期間が始まった
// ラスタは、書く前の明るさで描く
void testOrder() {
    printf("rasters before the write keep the old contrast\n");
    auto program = [](int nops, bool write) {
        std::vector<uint16_t> p = {
            0x41f9, 0x00e8, 0xdfc6, // lea $e8dfc6,a0 (プリンタのポート。MOVEM の最後のワードが $E8E000 に入る)
            0x2c7c, 0x0000, 0x0005, // movea.l #5,a6 (最後のワードが 5)
        };
        for (int i = 0; i < nops; i++) p.push_back(0x4e71);
        if (write) {
            p.push_back(0x48d0); // movem.l d0-d7/a0-a6,(a0)
            p.push_back(0x7fff);
        }
        return p;
    };
    // ラスタの時刻 (NOP だけを走らせるので、遅れは 1 命令まで)
    static const int RUN = 40000, SLACK = 8;
    std::vector<uint64_t> when;
    const auto setup = [](x68k::Machine& m) {
        const uint16_t regs[9] = {91, 9, 17, 81, 567, 0, 0, 567, 27}; // 電源を入れてすぐに表示が始まる
        for (int i = 0; i < 9; i++) m.write16(0xe80000 + (uint32_t)i * 2, regs[i], m68k::FC_SUPER_DATA);
    };
    {
        auto* m = new x68k::Machine;
        boot(*m, program(RUN / 4, false));
        setup(*m);
        Rasters r(*m);
        m->setLineListener(&r);
        m->run(RUN - 200);
        when = r.at;
        m->setLineListener(nullptr);
        delete m;
    }
    CHECK_EQ(when.size() > 20, true);
    int inside = 0;
    for (int nops = 1500; nops < 1800; nops += 3) {
        auto* m = new Probe;
        boot(*m, program(nops, true));
        setup(*m);
        Rasters r(*m);
        m->setLineListener(&r);
        m->run(RUN - 200);
        CHECK_EQ(port(*m), 0xf5);
        int wrong = 0;
        for (size_t i = 0; i < when.size() && i < r.at.size(); i++) {
            if (when[i] + SLACK < m->wroteAt) {
                wrong += r.contrast[i] != 14;
                inside += when[i] + 100 > m->wroteAt;
            } else if (when[i] > m->wroteAt + SLACK) {
                wrong += r.contrast[i] != 5;
            }
        }
        CHECK_EQ(wrong, 0);
        m->setLineListener(nullptr);
        delete m;
    }
    CHECK_EQ(inside > 3, true); // 命令の途中でラスタが始まる場合を通った
}

} // namespace

int main() {
    testPort();
    testState();
    testRenderLink();
    testOrder();
    return finish();
}
