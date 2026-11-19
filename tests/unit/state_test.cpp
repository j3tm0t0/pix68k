// 実行状態の保存と復元 (x68k/state.h) のテスト。
//
//   state_test                  テスト用の IPL ROM で動かす (ROM なしで動く)
//   state_test --corrupted      そのうち、書き換えたステートを読ませる試験だけ。環境変数 STATE_TEST_ROUNDS で回数 (既定 150)、
//                               STATE_TEST_SEED で乱数の種を変え、STATE_TEST_VERBOSE=1 で書き換えた場所を出す。
//                               ASan / UBSan のビルドで回数を増やして回す
//   state_test [-r DIR] -0 DISK [--cpu 68030 --ipl IPLROM30.DAT]
//                               本物の IPL ROM で Human68k をフロッピーディスクから起動し、起動の途中 (FDC と DMAC が
//                               動いている間) で保存する。起動したあと、machine_bench の場面 (user、gfx、fd) の途中でも保存し、
//                               4 秒ほど動かして比べる。ROM かディスクがなければ 77 を返す (ctest で飛ばす)。
//                               060turbo.sys を組み込んだディスクでは tests/x68k/state_060turbo.sh から動かす
//
// 確かめること: 動かしている Machine A を保存し、A をそのまま M フレーム進めたものと、別の場面を動かしていた
// 新しい Machine B に読んで M フレーム進めたものが同じになる (CPU のサイクル数とレジスタ、RAM、VRAM、合成した画面。
// machine_bench の checksum と同じ作り方)。さらに、そのあとの 2 つの保存が 1 バイトも違わないこと。
// 版、機種、CRC、長さが合わないものを読まず、そのとき Machine を変えないこと。
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <functional>
#include <initializer_list>
#include <new>
#include <vector>

#include "../bench/scene.h"
#include "test_check.h"
#include "x68k/machine.h"
#include "x68k/midi_out.h"
#include "x68k/render.h"
#include "x68k/sasi.h"
#include "x68k/spc.h"
#include "x68k/state.h"
#include "x68k/video_journal.h"

// SCSI のドライバが最初にすること (MB89352 のデータシート): SCTL の RST で SPC をリセットの状態にし、その間に BDID に
// 自分の ID を書く。リセットを解くのは呼んだ側 (SCTL に $11)
static void spcTakeId7(x68k::Spc& spc) {
    spc.write(x68k::Spc::SCTL, x68k::Spc::SCTL_RESET);
    spc.write(x68k::Spc::BDID, 7);
}

// 確保したメモリを、確保のたびに違う値で埋める。ステートに、値の決まっていないバイト (メンバの間の詰め物、
// 初めの値を入れていないメンバ) が入っていると、同じことをした 2 つの Machine のステートが違うものになり、
// 下の比較で見つかる。埋めなければ、確保したメモリがたまたま 0 の環境 (macOS) では見つからない
// noinline: GCC は、operator delete だけをインライン展開すると、new で確保したものを free() していると警告する
// (-Wmismatched-new-delete)
static uint8_t gFill = 0x35;
__attribute__((noinline)) void* operator new(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    memset(p, gFill, n);
    gFill = (uint8_t)(gFill * 5 + 0x4b);
    return p;
}
__attribute__((noinline)) void operator delete(void* p) noexcept { free(p); }
__attribute__((noinline)) void operator delete(void* p, size_t) noexcept { free(p); }

namespace {

// テスト用の IPL ROM: 割り込みのマスクを 0 にして止まる (場面は scene::jumpTo() で始める)
struct Rom {
    uint8_t b[x68k::Machine::IPL_SIZE];
    Rom() {
        memset(b, 0xff, sizeof b);
        put32(0x10000, 0x00002000); // SSP
        put32(0x10004, 0x00fe0100); // PC
        put16(0x0100, 0x46fc);      // move.w #$2000,sr
        put16(0x0102, 0x2000);
        put16(0x0104, 0x60fe);      // bra.s *
    }
    void put16(uint32_t o, uint16_t v) {
        b[o] = (uint8_t)(v >> 8);
        b[o + 1] = (uint8_t)v;
    }
    void put32(uint32_t o, uint32_t v) {
        put16(o, (uint16_t)(v >> 16));
        put16(o + 2, (uint16_t)v);
    }
};

uint32_t peek32(x68k::Machine& m, uint32_t a) {
    bool ok;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = v << 8 | m.peek8(a + i, ok);
    return v;
}

uint32_t fnv(uint32_t h, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

// Machine と、それに付けた Renderer の画面
struct Box {
    x68k::Machine* m;
    x68k::Renderer* r;
    uint32_t* pixels;
    explicit Box(const x68k::Config& c) {
        m = new x68k::Machine(c);
        pixels = new uint32_t[(size_t)x68k::Renderer::MAX_WIDTH * x68k::Renderer::MAX_HEIGHT]();
        r = new x68k::Renderer(m->video());
        r->setTarget(pixels, x68k::Renderer::MAX_WIDTH, x68k::Renderer::MAX_WIDTH, x68k::Renderer::MAX_HEIGHT);
        m->setLineListener(r);
        m->sound().setOutputRate(48000);
    }
    ~Box() {
        m->setLineListener(nullptr);
        delete r;
        delete[] pixels;
        delete m;
    }
    void clearPixels() { memset(pixels, 0, (size_t)x68k::Renderer::MAX_WIDTH * x68k::Renderer::MAX_HEIGHT * 4); }
    void frames(int n) {
        static int16_t audio[2 * 4096];
        for (int i = 0; i < n; i++) {
            m->runFrame();
            while (m->sound().ring().drain(audio, 4096) > 0) {
            }
        }
    }
    // machine_bench の checksum() と同じもの
    uint32_t checksum() {
        uint32_t h = 2166136261u;
        uint64_t c = m->cycles();
        h = fnv(h, (const uint8_t*)&c, sizeof c);
        m68k::Registers reg;
        m->getRegisters(reg);
        h = fnv(h, (const uint8_t*)reg.d, sizeof reg.d);
        h = fnv(h, (const uint8_t*)reg.a, sizeof reg.a);
        uint32_t misc[4] = {reg.usp, reg.ssp, reg.sr, reg.pc};
        h = fnv(h, (const uint8_t*)misc, sizeof misc);
        bool ok;
        for (uint32_t a = 0; a < 0x400000; a++) {
            uint8_t v = m->peek8(a, ok);
            h = fnv(h, &v, 1);
        }
        h = fnv(h, m->video().tvram(), x68k::Video::TVRAM_SIZE);
        h = fnv(h, (const uint8_t*)m->video().gvramWords(), x68k::Video::GVRAM_SIZE);
        h = fnv(h, (const uint8_t*)pixels, (size_t)x68k::Renderer::MAX_WIDTH * x68k::Renderer::MAX_HEIGHT * 4);
        return h;
    }
};

struct Blob {
    uint8_t* data = nullptr;
    size_t size = 0;
    ~Blob() { delete[] data; }
};

bool save(x68k::Machine& m, Blob& out) {
    delete[] out.data;
    size_t cap = x68k::stateSize(m);
    out.data = new uint8_t[cap];
    if (!x68k::saveState(m, out.data, cap, out.size) || out.size != cap) {
        printf("  saveState failed (size %zu, want %zu)\n", out.size, cap);
        failures++;
        return false;
    }
    // 自分で保存したものは、読むときの値の範囲の確かめ (StateIO::CHECK) を必ず通る (確かめが厳しすぎないこと)
    x68k::StateIO check(x68k::StateIO::CHECK, out.data + x68k::STATE_HEADER_SIZE, out.size - x68k::STATE_HEADER_SIZE);
    m.state(check);
    if (!check.ok() || check.pos() != out.size - x68k::STATE_HEADER_SIZE) {
        printf("  a state saved by this machine does not pass the check (stopped at payload offset %zu)\n", check.pos());
        failures++;
        return false;
    }
    return true;
}

// 1 フレームずつ動かし、フレームごとに保存して、読むときの確かめを通るかを見る
void framesChecked(x68k::Machine& m, int n, const std::function<void()>& frame) {
    static Blob scratch;
    for (int i = 0; i < n && failures == 0; i++) {
        frame();
        save(m, scratch);
    }
}

bool same(const Blob& a, const Blob& b) { return a.size == b.size && memcmp(a.data, b.data, a.size) == 0; }

// 保存の中で最初に違うところ (デバッグ用)
void reportDiff(const Blob& a, const Blob& b) {
    if (a.size != b.size) {
        printf("  sizes differ: %zu vs %zu\n", a.size, b.size);
        return;
    }
    // ヘッダの CRC は中身が違えば必ず違うので、中身の最初の違いを出す
    size_t count = 0, first = 0;
    for (size_t i = x68k::STATE_HEADER_SIZE; i < a.size; i++)
        if (a.data[i] != b.data[i] && count++ == 0) first = i;
    if (count) {
        // 違いの手前にある区切りの値 (StateIO::tag() の 4 文字) を探して、どの部分かを出す
        char part[5] = "?";
        for (size_t i = first; i >= x68k::STATE_HEADER_SIZE + 4; i--) {
            const uint8_t* t = a.data + i - 4;
            // CPU と FPU の区切り ("6000"、"6030"、"6882") は、ファイルの中でこの順に並ぶ値で書いてあるので、逆の順で探す
            static const char* const kTags[] = {"MACH", "0006", "0306", "2886", "MEM ", "VID\0", "KEYB", "FDC ", "SND ",
                                                "SCC ", "MOUS", "SASI", "SPC ", "FPUB", "MERC", "NEPT", "MIDI", "TIME"};
            bool found = false;
            for (const char* k : kTags)
                if (t[0] == (uint8_t)k[3] && t[1] == (uint8_t)k[2] && t[2] == (uint8_t)k[1] && t[3] == (uint8_t)k[0]) {
                    memcpy(part, k, 4);
                    found = true;
                }
            if (found) {
                printf("  %zu byte(s) differ, first %zu byte(s) after \"%s\": %02x vs %02x\n", count, first - i, part,
                       a.data[first], b.data[first]);
                if (getenv("STATE_TEST_DIFF")) { // デバッグ用: 違うバイトをすべて出す
                    for (size_t j = first; j < a.size; j++)
                        if (a.data[j] != b.data[j]) printf("    +%zu: %02x vs %02x\n", j - i, a.data[j], b.data[j]);
                }
                return;
            }
        }
        printf("  %zu byte(s) differ, first at payload offset %zu: %02x vs %02x\n", count,
               first - x68k::STATE_HEADER_SIZE, a.data[first], b.data[first]);
        return;
    }
    for (size_t i = 0; i < x68k::STATE_HEADER_SIZE; i++)
        if (a.data[i] != b.data[i]) {
            printf("  header differs at offset %zu\n", i);
            return;
        }
}

// 場面を始めて、キーボードとマウスにも入力を入れる (保存の時点で送っている途中のものができるように)
void startScene(Box& b, bool gfx) {
    if (gfx) CHECK_EQ(scene::startGfx(*b.m), true);
    else CHECK_EQ(scene::jumpTo(*b.m, scene::kCpuLoop, sizeof scene::kCpuLoop / 2), true);
    b.m->input().onKey(0x04, true); // A
    b.m->input().onMouse(30, -12, 1);
}

// A を保存して M フレーム進めたものと、別のことをしていた B に読んで M フレーム進めたものが同じか
void roundTrip(const char* name, const x68k::Config& config) {
    printf("%s\n", name);
    static Rom rom;
    Box a(config);
    if (!a.m->ok()) {
        printf("  out of memory\n");
        failures++;
        return;
    }
    a.m->setIplRom(rom.b, sizeof rom.b);
    a.m->powerOn();
    a.frames(2);
    startScene(a, true);
    a.frames(20);
    Blob saved;
    if (!save(*a.m, saved)) return;
    a.frames(30);
    a.m->input().onKey(0x04, false);
    a.frames(10);
    const uint32_t wantSum = a.checksum();

    Box b(config);
    b.m->setIplRom(rom.b, sizeof rom.b);
    b.m->powerOn();
    b.frames(2);
    startScene(b, false); // A とは違う場面
    b.frames(7);
    const char* err = nullptr;
    CHECK_EQ(x68k::loadState(*b.m, saved.data, saved.size, err), true);
    if (err) printf("  %s\n", err);
    Blob again; // 読んですぐ保存したものは、読んだものと同じ
    save(*b.m, again);
    if (!same(saved, again)) {
        printf("  save after load differs\n");
        reportDiff(saved, again);
        failures++;
    }
    b.frames(30);
    b.m->input().onKey(0x04, false);
    b.frames(10);
    CHECK_EQ(b.checksum(), wantSum);

    // 続けたあとの保存も同じ
    Blob sa, sb;
    save(*a.m, sa);
    save(*b.m, sb);
    if (!same(sa, sb)) {
        printf("  states after %s differ\n", name);
        reportDiff(sa, sb);
        failures++;
    }
}

#ifndef PIX68K_NO_CPU020
// FPU が保留している例外を、保存と読み込みで引き継ぐこと。FDIV のゼロ除算を保留にしたまま、FPU の命令のないループで
// 待っている間に保存する。$310000 に書いてループを抜けると FNOP で保留の例外 (ベクタ 50) が起き、処理ルーチンが
// FSAVE したフレームを $310100 に写して $310008 を 1 増やす。読んだ側でも同じことが起き、同じ結果になるかを見る。
// check は保存した側と読んだ側の両方で、例外のあとに FSAVE したフレームを確かめる
template <class Check>
void fpuPendingRun(const char* name, int cpu, const uint16_t* prog, size_t words, Check check) {
    printf("%s\n", name);
    static Rom rom;
    x68k::Config config;
    config.setCpu(cpu);
    auto release = [](x68k::Machine& m) { scene::poke8(m, 0x310003, 1); };

    Box a(config);
    a.m->setIplRom(rom.b, sizeof rom.b);
    a.m->powerOn();
    a.frames(2);
    CHECK_EQ(scene::jumpTo(*a.m, prog, words), true);
    a.frames(3);
    CHECK_EQ(peek32(*a.m, 0x310008), 0u); // まだ例外は起きていない
    Blob saved;
    if (!save(*a.m, saved)) return;
    release(*a.m);
    a.frames(3);
    CHECK_EQ(peek32(*a.m, 0x310004), 1u);
    CHECK_EQ(peek32(*a.m, 0x310008), 1u);
    check(*a.m);
    const uint32_t wantSum = a.checksum();

    Box b(config);
    b.m->setIplRom(rom.b, sizeof rom.b);
    b.m->powerOn();
    b.frames(2);
    startScene(b, false); // 保留のない別の場面
    b.frames(3);
    const char* err = nullptr;
    CHECK_EQ(x68k::loadState(*b.m, saved.data, saved.size, err), true);
    if (err) printf("  %s\n", err);
    release(*b.m);
    b.frames(3);
    CHECK_EQ(peek32(*b.m, 0x310004), 1u);
    CHECK_EQ(peek32(*b.m, 0x310008), 1u);
    check(*b.m);
    CHECK_EQ(b.checksum(), wantSum);
}

// 68882 (Fpu の excPending_ と pendingFrame_)。処理ルーチンは FSAVE した IDLE のフレーム (BIU フラグの bit27 が 0 で
// 保留を表す) を写し、bit27 を立てて FRESTORE する
void fpuPending() {
    static const uint16_t prog[] = {
        0x41fa, 0x0036,                         //         LEA handler(PC),A0
        0x21c8, 0x00c8,                         //         MOVE.L A0,$C8.W (ベクタ 50: ゼロ除算)
        0xf23c, 0x9000, 0x0000, 0x0400,         //         FMOVE.L #$400,FPCR (ゼロ除算の例外を許す)
        0xf23c, 0x4000, 0x0000, 0x0001,         //         FMOVE.L #1,FP0
        0xf23c, 0x4020, 0x0000, 0x0000,         //         FDIV.L #0,FP0 (例外を保留する)
        0x4ab9, 0x0031, 0x0000,                 // loop:   TST.L $310000
        0x67f8,                                 //         BEQ.S loop
        0xf280, 0x0000,                         //         FNOP (保留の例外が起きる)
        0x23fc, 0x0000, 0x0001, 0x0031, 0x0004, //         MOVE.L #1,$310004
        0x60fe,                                 // done:   BRA.S done
        0xf327,                                 // handler: FSAVE -(A7)
        0x43f9, 0x0031, 0x0100,                 //         LEA $310100,A1
        0x244f,                                 //         MOVEA.L A7,A2
        0x700e,                                 //         MOVEQ #14,D0
        0x22da,                                 // copy:   MOVE.L (A2)+,(A1)+
        0x51c8, 0xfffc,                         //         DBRA D0,copy
        0x08ef, 0x0003, 0x0038,                 //         BSET #3,$38(A7) (BIU フラグの bit27)
        0xf35f,                                 //         FRESTORE (A7)+
        0x52b9, 0x0031, 0x0008,                 //         ADDQ.L #1,$310008
        0x4e73,                                 //         RTE
    };
    fpuPendingRun("68882 pending exception", 68030, prog, sizeof prog / 2, [](x68k::Machine& m) {
        CHECK_EQ(peek32(m, 0x310100 + 0x38) & 0x08000000u, 0u); // FSAVE したフレームは保留を表す
        CHECK_EQ(peek32(m, 0x310100 + 0x04) >> 16, 0x4020u);    // コマンドの語は FDIV
    });
}

// 68060 (Fpu の excp060_ と excpOp_)。FPU は例外の状態 (EXCP) になり、次の FPU の命令の前の例外 (フォーマット $0、
// PC はその命令) として起こす。処理ルーチンは FSAVE した EXCP のフレーム (3 ロング。FSAVE で EXCP は解ける) を写し、
// フレームを捨てて RTE する。戻った先の FNOP は例外を起こさずに進む
void fpuPending060() {
    static const uint16_t prog[] = {
        0x41fa, 0x0036,                         //         LEA handler(PC),A0
        0x21c8, 0x00c8,                         //         MOVE.L A0,$C8.W (ベクタ 50: ゼロ除算)
        0xf23c, 0x9000, 0x0000, 0x0400,         //         FMOVE.L #$400,FPCR (ゼロ除算の例外を許す)
        0xf23c, 0x4000, 0x0000, 0x0001,         //         FMOVE.L #1,FP0
        0xf23c, 0x4020, 0x0000, 0x0000,         //         FDIV.L #0,FP0 (EXCP になる)
        0x4ab9, 0x0031, 0x0000,                 // loop:   TST.L $310000
        0x67f8,                                 //         BEQ.S loop
        0xf280, 0x0000,                         //         FNOP (保留の例外が起きる。戻ってきたらそのまま進む)
        0x23fc, 0x0000, 0x0001, 0x0031, 0x0004, //         MOVE.L #1,$310004
        0x60fe,                                 // done:   BRA.S done
        0xf327,                                 // handler: FSAVE -(A7)
        0x23df, 0x0031, 0x0100,                 //         MOVE.L (A7)+,$310100
        0x23df, 0x0031, 0x0104,                 //         MOVE.L (A7)+,$310104
        0x23df, 0x0031, 0x0108,                 //         MOVE.L (A7)+,$310108
        0x23ef, 0x0002, 0x0031, 0x010c,         //         MOVE.L 2(A7),$31010C (例外のフレームの PC)
        0x52b9, 0x0031, 0x0008,                 //         ADDQ.L #1,$310008
        0x4e73,                                 //         RTE
    };
    fpuPendingRun("68060 pending exception", 68060, prog, sizeof prog / 2, [](x68k::Machine& m) {
        CHECK_EQ(peek32(m, 0x310100) & 0xffffu, 0xe002u); // EXCP、DZ (ベクタ 50)
        CHECK_EQ(peek32(m, 0x31010c), scene::kCode + 0x28); // FNOP の番地
    });
}
#endif

// 保存しない Machine、保存する Machine、保存したものを読む Machine の 3 つで同じプログラムを動かし、終わりの
// チェックサム (CPU のサイクル数、レジスタ、RAM、VRAM、画面) と保存が同じになるかを見る。
//   setup    電源を入れて 2 フレーム動かしたあとに呼ぶ (表を RAM に置く、プログラムへ飛ばすなど)
//   release  保存の時点で呼ぶ (プログラムの待ちを解く)。保存しない Machine でも同じ時点で呼ぶ
//   verify   終わりに 3 つの Machine それぞれで呼ぶ
template <class Setup, class Release, class Verify>
void threeWay(const char* name, const x68k::Config& config, int before, int after, Setup setup, Release release,
              Verify verify) {
    printf("%s\n", name);
    static Rom rom;
    auto start = [&](Box& x) {
        x.m->setIplRom(rom.b, sizeof rom.b);
        x.m->powerOn();
        x.frames(2);
    };
    Box n(config); // 保存しない
    start(n);
    setup(*n.m);
    n.frames(before);
    release(*n.m);
    n.frames(after);
    verify(*n.m);
    const uint32_t wantSum = n.checksum();

    Box a(config); // 保存する
    start(a);
    setup(*a.m);
    a.frames(before);
    Blob saved;
    if (!save(*a.m, saved)) return;
    release(*a.m);
    a.frames(after);
    verify(*a.m);
    CHECK_EQ(a.checksum(), wantSum); // 保存しても変わらない

    Box b(config); // 別の場面を動かしていたものに読む
    start(b);
    startScene(b, false);
    b.frames(5);
    const char* err = nullptr;
    CHECK_EQ(x68k::loadState(*b.m, saved.data, saved.size, err), true);
    if (err) printf("  %s\n", err);
    Blob again;
    save(*b.m, again);
    if (!same(saved, again)) {
        printf("  save after load differs\n");
        reportDiff(saved, again);
        failures++;
    }
    release(*b.m);
    b.frames(after);
    verify(*b.m);
    CHECK_EQ(b.checksum(), wantSum);
    Blob sn, sb;
    save(*n.m, sn);
    save(*b.m, sb);
    if (!same(sn, sb)) {
        printf("  states at the end differ\n");
        reportDiff(sn, sb);
        failures++;
    }
}

void poke32(x68k::Machine& m, uint32_t a, uint32_t v) {
    scene::poke16(m, a, (uint16_t)(v >> 16));
    scene::poke16(m, a + 2, (uint16_t)v);
}

#ifndef PIX68K_NO_CPU020

// 68030 の MMU のレジスタ (TC、TT0、TT1、CRP、SRP、MMUSR) を初期値でない値にして保存する。X68030 はアドレス変換を
// しない (MC68EC030 として扱う) ので、ATC はなく、レジスタの値だけが状態になる。読んだあとに PMOVE でメモリに
// 書き出した値を見る
void mmu030() {
    static const uint16_t prog[] = {
        0x41f9, 0x0031, 0x0100, //         LEA $310100,A0
        0xf010, 0x4000,         //         PMOVE (A0),TC
        0x5088,                 //         ADDQ.L #8,A0
        0xf010, 0x0800,         //         PMOVE (A0),TT0
        0x5088,                 //         ADDQ.L #8,A0
        0xf010, 0x0c00,         //         PMOVE (A0),TT1
        0x5088,                 //         ADDQ.L #8,A0
        0xf010, 0x4c00,         //         PMOVE (A0),CRP
        0x5088,                 //         ADDQ.L #8,A0
        0xf010, 0x4800,         //         PMOVE (A0),SRP
        0x5088,                 //         ADDQ.L #8,A0
        0xf010, 0x6000,         //         PMOVE (A0),MMUSR
        0x4ab9, 0x0031, 0x0000, // wait:   TST.L $310000
        0x67f8,                 //         BEQ.S wait
        0x41f9, 0x0031, 0x0200, //         LEA $310200,A0
        0xf010, 0x4200,         //         PMOVE TC,(A0)
        0x5088,                 //         ADDQ.L #8,A0
        0xf010, 0x0a00,         //         PMOVE TT0,(A0)
        0x5088,                 //         ADDQ.L #8,A0
        0xf010, 0x0e00,         //         PMOVE TT1,(A0)
        0x5088,                 //         ADDQ.L #8,A0
        0xf010, 0x4e00,         //         PMOVE CRP,(A0)
        0x5088,                 //         ADDQ.L #8,A0
        0xf010, 0x4a00,         //         PMOVE SRP,(A0)
        0x5088,                 //         ADDQ.L #8,A0
        0xf010, 0x6200,         //         PMOVE MMUSR,(A0)
        0x23fc, 0x0000, 0x0001, 0x0031, 0x0004, // MOVE.L #1,$310004
        0x60fe,                 // done:   BRA.S done
    };
    static const uint32_t values[] = {
        0x80f08630, 0,          // TC (E = 1)
        0x00ff8507, 0,          // TT0 (E = 1)
        0xc03f8143, 0,          // TT1 (E = 1)
        0x7fff0002, 0x00123450, // CRP
        0x80000003, 0x00234560, // SRP
        0xe2470000, 0,          // MMUSR
    };
    x68k::Config config;
    config.setCpu(68030);
    threeWay(
        "68030 MMU registers (TC, TT0, TT1, CRP, SRP, MMUSR)", config, 3, 3,
        [](x68k::Machine& m) {
            for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) poke32(m, 0x310100 + (uint32_t)i * 4, values[i]);
            CHECK_EQ(scene::jumpTo(m, prog, sizeof prog / 2), true);
        },
        [](x68k::Machine& m) { scene::poke8(m, 0x310003, 1); },
        [](x68k::Machine& m) {
            CHECK_EQ(peek32(m, 0x310004), 1u);
            for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
                // MMUSR は 16 ビット
                const uint32_t want = i == 10 ? values[i] : values[i];
                CHECK_EQ(peek32(m, 0x310200 + (uint32_t)i * 4), want);
            }
        });
}

// 68060 の MMU を有効にし、表を引いた結果が ATC と TLB に入っている状態で保存する。
//   - 論理アドレス $40000000 からの 64 ページを、表 (ルート $320000、ポインタ $320200、ページ $320400) で
//     物理アドレス $330000 からに割り当てる。命令は ITT0、$00000000-$3FFFFFFF のデータは DTT0 で変換しない
//   - 保存の前に 16 ページを読み書きする (表を引く。1 回ごとに時間がかかり、U と M のビットを書く)。
//     そのあと、ページ 3 のデスクリプタを PFLUSH せずに書き換える (ATC に残っている前の割り当てが使われ続ける)
//   - 保存のあとに 32 ページを読み書きする。前の 16 ページは表を引かず、ページ 3 は前の割り当てのままになる
// 読んだ側が ATC と TLB を空にして始めると、表を引き直す分だけサイクル数がずれ、ページ 3 は新しい割り当てに書く。
// キャッシュの制御レジスタ (CACR)、PCR、BUSCR も初期値でない値にし、終わりにメモリへ書き出す
const uint16_t kMmu060Prog[] = {
        0x203c, 0x0032, 0x0000, //         MOVE.L #$320000,D0
        0x4e7b, 0x0807,         //         MOVEC D0,SRP
        0x4e7b, 0x0806,         //         MOVEC D0,URP
        0x203c, 0x003f, 0xc000, //         MOVE.L #$003FC000,D0
        0x4e7b, 0x0006,         //         MOVEC D0,DTT0
        0x203c, 0x00ff, 0xc000, //         MOVE.L #$00FFC000,D0
        0x4e7b, 0x0004,         //         MOVEC D0,ITT0
        0x203c, 0xa080, 0x8000, //         MOVE.L #$A0808000,D0
        0x4e7b, 0x0002,         //         MOVEC D0,CACR
        0x203c, 0x0000, 0x0001, //         MOVE.L #1,D0
        0x4e7b, 0x0808,         //         MOVEC D0,PCR
        0x203c, 0xa000, 0x0000, //         MOVE.L #$A0000000,D0
        0x4e7b, 0x0008,         //         MOVEC D0,BUSCR
        0x203c, 0x0000, 0x8000, //         MOVE.L #$8000,D0
        0x4e7b, 0x0003,         //         MOVEC D0,TC
        0x207c, 0x4000, 0x0000, //         MOVEA.L #$40000000,A0
        0x700f,                 //         MOVEQ #15,D0
        0x5290,                 // l1:     ADDQ.L #1,(A0)
        0xd1fc, 0x0000, 0x1000, //         ADDA.L #$1000,A0
        0x51c8, 0xfff6,         //         DBRA D0,l1
        0x23fc, 0x0035, 0x0001, 0x0032, 0x040c, // MOVE.L #$00350001,$32040C (ページ 3 を $350000 に)
        0x4ab9, 0x0031, 0x0000, // wait:   TST.L $310000
        0x67f8,                 //         BEQ.S wait
        0x207c, 0x4000, 0x0000, //         MOVEA.L #$40000000,A0
        0x701f,                 //         MOVEQ #31,D0
        0x5290,                 // l2:     ADDQ.L #1,(A0)
        0xd1fc, 0x0000, 0x1000, //         ADDA.L #$1000,A0
        0x51c8, 0xfff6,         //         DBRA D0,l2
        0x41f9, 0x0031, 0x0200, //         LEA $310200,A0
        0x4e7a, 0x0002, 0x20c0, //         MOVEC CACR,D0 / MOVE.L D0,(A0)+
        0x4e7a, 0x0808, 0x20c0, //         MOVEC PCR,D0 / MOVE.L D0,(A0)+
        0x4e7a, 0x0008, 0x20c0, //         MOVEC BUSCR,D0 / MOVE.L D0,(A0)+
        0x4e7a, 0x0003, 0x20c0, //         MOVEC TC,D0 / MOVE.L D0,(A0)+
        0x4e7a, 0x0006, 0x20c0, //         MOVEC DTT0,D0 / MOVE.L D0,(A0)+
        0x4e7a, 0x0004, 0x20c0, //         MOVEC ITT0,D0 / MOVE.L D0,(A0)+
        0x4e7a, 0x0807, 0x20c0, //         MOVEC SRP,D0 / MOVE.L D0,(A0)+
        0x23fc, 0x0000, 0x0001, 0x0031, 0x0004, // MOVE.L #1,$310004
        0x60fe,                 // done:   BRA.S done
};

// 表を RAM に置いて、上のプログラムへ飛ばす
void startMmu060(x68k::Machine& m) {
    poke32(m, 0x320000 + 0x20 * 4, 0x320200 | 3); // ルート: $40000000-$41FFFFFF
    poke32(m, 0x320200, 0x320400 | 3);            // ポインタ: $40000000-$4003FFFF
    for (uint32_t i = 0; i < 64; i++) poke32(m, 0x320400 + i * 4, (0x330000 + i * 0x1000) | 1);
    CHECK_EQ(scene::jumpTo(m, kMmu060Prog, sizeof kMmu060Prog / 2), true);
}

void mmu060() {
    x68k::Config config;
    config.setCpu(68060);
    threeWay(
        "68060 MMU (TC enabled, ATC and TLB filled, stale entry)", config, 3, 3, startMmu060,
        [](x68k::Machine& m) { scene::poke8(m, 0x310003, 1); },
        [](x68k::Machine& m) {
            CHECK_EQ(peek32(m, 0x310004), 1u);
            CHECK_EQ(peek32(m, 0x330000), 2u); // ページ 0: 保存の前と後に 1 回ずつ
            CHECK_EQ(peek32(m, 0x333000), 2u); // ページ 3: 前の割り当てのまま 2 回
            CHECK_EQ(peek32(m, 0x340000), 1u); // ページ 16: 後に 1 回
            // ページ 32 は使っていないので、$350000 は 0 のまま (ページ 3 を新しい割り当てで書くと 1 になる)
            CHECK_EQ(peek32(m, 0x350000), 0u);
            CHECK_EQ(peek32(m, 0x310200), 0xa0808000u); // CACR
            CHECK_EQ(peek32(m, 0x310204) & 0xff, 0x01u); // PCR の ESS
            CHECK_EQ(peek32(m, 0x310208), 0xa0000000u); // BUSCR
            CHECK_EQ(peek32(m, 0x31020c), 0x8000u);     // TC
            CHECK_EQ(peek32(m, 0x310210), 0x003fc000u); // DTT0
            CHECK_EQ(peek32(m, 0x310214), 0x00ffc000u); // ITT0
            CHECK_EQ(peek32(m, 0x310218), 0x320000u);   // SRP
        });
    // ページ 5 を、ローカルメモリの SIMM のない番地 ($10200000。読むと $FF、書いても変わらない) に割り当てる。
    // 読んだあとも同じ「何もつながっていない」ページを指すこと (SIMM のある番地に戻すと、ローカルメモリに書いてしまう。
    // ローカルメモリの中身は終わりのステートで比べる)
    x68k::Config hole;
    hole.setCpu(68060);
    hole.localMemSize = 1u << 20;
    threeWay(
        "68060 MMU (a page mapped to local memory without SIMM)", hole, 3, 3,
        [](x68k::Machine& m) {
            startMmu060(m);
            poke32(m, 0x320400 + 5 * 4, 0x10200000 | 1);
        },
        [](x68k::Machine& m) { scene::poke8(m, 0x310003, 1); },
        [](x68k::Machine& m) {
            CHECK_EQ(peek32(m, 0x310004), 1u);
            CHECK_EQ(peek32(m, 0x335000), 0u); // ページ 5 は RAM に書いていない
            CHECK_EQ(peek32(m, 0x334000), 2u);
        });
    // 版 6、7、8 の並び (TLB と ATC がない。版 8 は CD-ROM 装置の状態がある) で書いたものも読める。TLB と ATC は
    // 空から始まるので、表を引き直す (サイクル数は保存しなかったときと変わる)。プログラムは最後まで進み、ページ 3 は
    // 書き換えたあとの割り当てで書く
    static Rom rom;
    Box a(config);
    a.m->setIplRom(rom.b, sizeof rom.b);
    a.m->powerOn();
    a.frames(2);
    startMmu060(*a.m);
    a.frames(3);
    size_t prevSize = 0;
    for (uint32_t version : {6u, 7u, 8u}) {
        Box b(config);
        b.m->setIplRom(rom.b, sizeof rom.b);
        b.m->powerOn();
        b.frames(2);
        std::vector<uint8_t> old(x68k::stateSize(*a.m) + 4096); // 版 11 までの並びは、SPC の状態が今より大きい
        size_t n = 0;
        CHECK_EQ(x68k::saveState(*a.m, old.data(), old.size(), n, version), true);
        if (version == 7) CHECK_EQ(n, prevSize); // 版 6 と版 7 は同じ並び
        if (version == 8) CHECK_EQ(n > prevSize, true);
        prevSize = n;
        const char* err = nullptr;
        CHECK_EQ(x68k::loadState(*b.m, old.data(), n, err), true);
        scene::poke8(*b.m, 0x310003, 1);
        b.frames(3);
        CHECK_EQ(peek32(*b.m, 0x310004), 1u);
        CHECK_EQ(peek32(*b.m, 0x330000), 2u);
        CHECK_EQ(peek32(*b.m, 0x333000), 1u);
        CHECK_EQ(peek32(*b.m, 0x350000), 1u);
    }
}
#endif

// 合わないものを読まないこと。読まなかったときに Machine を変えないこと
#ifndef PIX68K_NO_CPU020
// ステートの保存、大きさを数えるだけ (stateSize())、読めないステートの読み込み (並びの確認で止まる) では、68060 の
// サイクル数の数え方の状態 (分岐キャッシュ、命令の取り出しの位置) を変えない。途中で何度呼んでも、あとのサイクル数が同じ
void sizeKeepsTiming060() {
    printf("68060: saving, stateSize() and a rejected load keep the cycle counting state\n");
    static Rom rom;
    x68k::Config config;
    config.setCpu(68060);
    uint32_t sums[2];
    for (int k = 0; k < 2; k++) {
        Box a(config);
        a.m->setIplRom(rom.b, sizeof rom.b);
        a.m->powerOn();
        a.frames(2);
        startScene(a, true);
        // CPU の状態だけを書いたもの (下の CHECK で読ませる)
        static uint8_t cpuState[1 << 16];
        x68k::StateIO sv(x68k::StateIO::SAVE, cpuState, sizeof cpuState);
        a.m->cpu020()->state(sv);
        CHECK_EQ(sv.ok(), true);
        for (int f = 0; f < 20; f++) {
            a.m->run(12345); // フレームの途中で呼ぶ
            if (k == 1) {
                Blob whole; // ステートの全体を保存する
                save(*a.m, whole);
                x68k::stateSize(*a.m);
                // ゼロで埋めたものは、ヘッダの検査で断られる
                static uint8_t junk[64];
                const char* err = nullptr;
                CHECK_EQ(x68k::loadState(*a.m, junk, sizeof junk, err), false);
                // StateIO::SIZE と StateIO::CHECK を、CPU の state() に直接渡す
                x68k::StateIO sz(x68k::StateIO::SIZE, nullptr, 0);
                a.m->cpu020()->state(sz);
                x68k::StateIO ck(x68k::StateIO::CHECK, cpuState, sv.pos());
                a.m->cpu020()->state(ck);
                CHECK_EQ(ck.ok(), true);
                CHECK_EQ(ck.pos(), sv.pos());
            }
            a.frames(1);
        }
        sums[k] = a.checksum();
    }
    CHECK_EQ(sums[1], sums[0]);
}
#endif

#ifndef PIX68K_NO_CPU020
// 68030 / 68060 のコアは、動いている間、命令の前の検査 (止まっているか、割り込み、トレース) を省く (m68k/cpu020.h の
// attn_)。ステートを読んだあとは、読んだ状態で検査をやり直す: STOP で止まっている機械のステートを、ループを回っている
// (検査を省いている) 機械に読むと、止まったままになり、STOP の先の命令へ進まない
void loadStoppedCpu(int cpu) {
    printf("%d: a state saved in STOP keeps the CPU stopped on a machine that was running\n", cpu);
    static Rom rom;
    x68k::Config config;
    config.setCpu(cpu);
    // STOP #$2700 の先に、$310000 のロングを増やし続けるループを置く
    static const uint16_t prog[] = {0x4e72, 0x2700, 0x52b9, 0x0031, 0x0000, 0x60f8};
    Box a(config);
    a.m->setIplRom(rom.b, sizeof rom.b);
    a.m->powerOn();
    a.frames(2);
    for (uint32_t i = 0; i < 4; i++) scene::poke8(*a.m, 0x310000 + i, 0);
    CHECK_EQ(scene::jumpTo(*a.m, prog, sizeof prog / 2), true);
    a.frames(1);
    CHECK_EQ(a.m->cpu020()->stopped(), true);
    Blob saved;
    if (!save(*a.m, saved)) return;

    Box b(config);
    b.m->setIplRom(rom.b, sizeof rom.b);
    b.m->powerOn();
    b.frames(2);
    CHECK_EQ(scene::jumpTo(*b.m, scene::kCpuLoop, sizeof scene::kCpuLoop / 2), true);
    b.frames(1);
    b.m->run(1000);
    CHECK_EQ(b.m->cpu020()->attn_ & m68k::Cpu020::ATTN_CHECK, 0); // 検査を省いている
    const char* err = nullptr;
    CHECK_EQ(x68k::loadState(*b.m, saved.data, saved.size, err), true);
    b.frames(2);
    CHECK_EQ(b.m->cpu020()->stopped(), true);
    CHECK_EQ(peek32(*b.m, 0x310000), 0u);
}
#endif

#ifndef PIX68K_NO_CPU020
// 68030 / 68060 のコアは、STOP で止まっている間、区間の終わりまで 1 回で進める (Cpu020::stopUntil())。
// MFP のタイマ D (ディレイモード) の割り込みで STOP から起きるプログラムを、STOP の途中を含む所で保存し、読み込んだ
// 機械が、保存した機械と 1/SUB サイクルまで同じ時刻に起き、同じ所まで進むことを確かめる
void stopTimerWake(int cpu) {
    printf("%d: STOP woken by MFP timer D, saved and loaded while stopped\n", cpu);
    static Rom rom;
    x68k::Config config;
    config.setCpu(cpu);
    // STOP #$2000; ADDQ.L #1,$310000; BRA.S STOP。処理ルーチン ($311000) は ADDQ.L #1,$310004; RTE
    static const uint16_t prog[] = {0x4e72, 0x2000, 0x52b9, 0x0031, 0x0000, 0x60f4};
    static const uint16_t handler[] = {0x52b9, 0x0031, 0x0004, 0x4e73};
    auto start = [&](Box& b) {
        b.m->setIplRom(rom.b, sizeof rom.b);
        b.m->powerOn();
        b.frames(2);
        for (uint32_t i = 0; i < 8; i++) scene::poke8(*b.m, 0x310000 + i, 0);
        for (size_t i = 0; i < sizeof handler / 2; i++) scene::poke16(*b.m, 0x311000 + (uint32_t)i * 2, handler[i]);
        // MFP: ベクタ $40 (自動 EOI)、タイマ D だけ許可、1/4 分周で 250 カウント (約 250us ごと)
        poke32(*b.m, 0x44 * 4, 0x311000);
        scene::poke8(*b.m, 0xe88017, 0x40); // VR
        scene::poke8(*b.m, 0xe88007, 0x00); // IERA
        scene::poke8(*b.m, 0xe88013, 0x00); // IMRA
        scene::poke8(*b.m, 0xe88009, 0x10); // IERB
        scene::poke8(*b.m, 0xe88015, 0x10); // IMRB
        scene::poke8(*b.m, 0xe88025, 250);  // TDDR
        scene::poke8(*b.m, 0xe8801d, 0x01); // TCDCR (タイマ D を 1/4 分周)
    };
    Box a(config);
    start(a);
    CHECK_EQ(scene::jumpTo(*a.m, prog, sizeof prog / 2), true);
    a.frames(2);
    // 止まっている途中で保存する (区間の途中で終わる長さで進め、止まっているところで止める)
    a.m->run(37);
    for (int i = 0; i < 1000 && !a.m->cpu020()->stopped(); i++) a.m->run(37);
    CHECK_EQ(a.m->cpu020()->stopped(), true);
    Blob saved;
    if (!save(*a.m, saved)) return;
    a.m->run(12345);
    a.frames(3);

    Box b(config);
    start(b);
    const char* err = nullptr;
    CHECK_EQ(x68k::loadState(*b.m, saved.data, saved.size, err), true);
    b.m->run(12345);
    b.frames(3);
    CHECK_EQ(b.m->cpu020()->sub_, a.m->cpu020()->sub_);
    CHECK_EQ(b.checksum(), a.checksum());
    CHECK_EQ(peek32(*b.m, 0x310000), peek32(*a.m, 0x310000));
    CHECK_EQ(peek32(*b.m, 0x310004), peek32(*a.m, 0x310004));
    // 起きた回数の期待値 (タイマ D の周期から決まる数) とは比べない。この試験は、保存した機械と読み込んだ機械が同じに
    // 進むことだけを見る。下は、起きてはまた止まることを繰り返していること (タイマ D の割り込みの数だけループを回る)
    CHECK_EQ(peek32(*a.m, 0x310004) > 100, true);
    CHECK_EQ(peek32(*a.m, 0x310000) + 1 >= peek32(*a.m, 0x310004), true);
    CHECK_EQ(peek32(*a.m, 0x310000) <= peek32(*a.m, 0x310004), true);
}
#endif

#ifndef PIX68K_NO_CPU020
// 68060 のサイクル数の数え方の状態 (分岐キャッシュなど) は、版 10 のステートに入っている。版 9 のステートには入って
// いないので、読み込むと空から始まる (読み込む前に覚えていたものも消える)
void timingState060() {
    printf("68060: the cycle counting state is in version 10; a version 9 state starts with it empty\n");
    static Rom rom;
    x68k::Config config;
    config.setCpu(68060);
    using m68k::Cpu020;
    // cacr と ess (PCR の ESS) を決めて場面を動かし、n サイクル進めたあと、同時実行の相手が決まっている所
    // (pairLead_ と pairDest_ が 0 でない) まで 1 サイクルずつ進める (ess のとき)
    auto start = [&](Box& x, uint32_t cacr, bool ess, int n) {
        x.m->setIplRom(rom.b, sizeof rom.b);
        x.m->powerOn();
        x.frames(2);
        startScene(x, true);
        Cpu020& c = *x.m->cpu020();
        c.cacr_ = cacr;
        c.updateCacheCosts();
        if (ess) c.pcr_ |= 1;
        x.frames(5);
        x.m->run(n); // フレームの途中
        for (int i = 0; ess && i < 200000 && !(c.pairLead_ && c.pairDest_); i++) x.m->run(1);
    };
    auto entries = [](const Cpu020& c) {
        int n = 0;
        for (uint8_t st : c.bcacheState060_) n += st != 0;
        return n;
    };
    const uint32_t EBC = 0x00800000u, EDC = 0x80000000u, EIC = 0x00008000u;
    // 保存する機械: 分岐キャッシュとスーパースカラが有効で、命令キャッシュは無効 (命令の取り出しの位置を持っている)
    Box a(config);
    start(a, EBC | EDC, true, 4321);
    const Cpu020& ca = *a.m->cpu020();
    CHECK_EQ(entries(ca) > 0, true);
    CHECK_EQ(ca.pairLead_ != 0, true);
    CHECK_EQ(ca.pairDest_ != 0, true);
    CHECK_EQ(ca.fetchValid_, true);
    const size_t cap = x68k::stateSize(*a.m) + 4096; // 版 9 の並びは、SPC の状態が今より大きい
    std::vector<uint8_t> v9(cap), v10(cap);
    size_t n9 = 0, n10 = 0;
    CHECK_EQ(x68k::saveState(*a.m, v9.data(), cap, n9, 9), true);
    CHECK_EQ(x68k::saveState(*a.m, v10.data(), cap, n10, 10), true);
    // 版 10 で増えたのは: 区切り 4 + pairLead_ 4 + pairDest_ 4 + foldOk060_ 1 + fetchValid_ 1 + fetchLong_ 4 +
    // 分岐キャッシュのアドレス 4 x 256 + 状態 256 (今の版は、SASI と SPC の状態の並びも違うので、版 10 の並びで比べる)
    {
        std::vector<uint8_t> old10(cap);
        size_t n10old = 0;
        CHECK_EQ(x68k::saveState(*a.m, old10.data(), cap, n10old, 10), true);
        CHECK_EQ(n10old, n9 + 4 + 4 + 4 + 1 + 1 + 4 + 4 * 256 + 256);
    }
    const char* err = nullptr;
    {
        // 版 9: 数え方の状態を持っている機械 (分岐を覚え、同時実行の相手と取り出しの位置が決まっている) に読むと、
        // どれも空になる
        Box b(config);
        start(b, EBC | EDC, true, 9876);
        const Cpu020& cb = *b.m->cpu020();
        CHECK_EQ(entries(cb) > 0, true);
        CHECK_EQ(cb.pairLead_ != 0, true);
        CHECK_EQ(cb.pairDest_ != 0, true);
        CHECK_EQ(cb.fetchValid_, true);
        CHECK_EQ(x68k::loadState(*b.m, v9.data(), n9, err), true);
        CHECK_EQ(entries(cb), 0);
        CHECK_EQ(cb.pairLead_, 0u);
        CHECK_EQ(cb.pairDest_, 0u);
        CHECK_EQ(cb.foldOk060_, false);
        CHECK_EQ(cb.fetchValid_, false);
        CHECK_EQ(cb.cacr_, ca.cacr_);
        CHECK_EQ(cb.pcr_, ca.pcr_);
        // 続けて動く
        b.frames(3);
        CHECK_EQ(b.m->halted(), false);
    }
    {
        // 版 10: 読み込む前は保存した機械と違う状態 (スーパースカラなし、命令キャッシュ有効、数え方の状態は空) にしておく。
        // 読むと保存した機械と同じ状態になり、続きのサイクル数も同じ
        Box c(config);
        start(c, EBC | EDC | EIC, false, 777);
        c.frames(3);
        c.m->cpu020()->clearTiming060();
        const Cpu020& cc = *c.m->cpu020();
        CHECK_EQ(cc.pairLead_, 0u);
        CHECK_EQ(cc.pairDest_, 0u);
        CHECK_EQ(cc.fetchValid_, false);
        CHECK_EQ(entries(cc), 0);
        CHECK_EQ(cc.pcr_ & 1, 0u);
        CHECK_EQ(x68k::loadState(*c.m, v10.data(), n10, err), true);
        CHECK_EQ(memcmp(cc.bcachePc060_, ca.bcachePc060_, sizeof ca.bcachePc060_), 0);
        CHECK_EQ(memcmp(cc.bcacheState060_, ca.bcacheState060_, sizeof ca.bcacheState060_), 0);
        CHECK_EQ(cc.pairLead_, ca.pairLead_);
        CHECK_EQ(cc.pairDest_, ca.pairDest_);
        CHECK_EQ(cc.foldOk060_, ca.foldOk060_);
        CHECK_EQ(cc.fetchValid_, ca.fetchValid_);
        CHECK_EQ(cc.fetchLong_, ca.fetchLong_);
        a.frames(10);
        c.frames(10);
        CHECK_EQ(c.m->cycles(), a.m->cycles());
        CHECK_EQ(c.checksum(), a.checksum());
    }
}
#endif

void rejects() {
    printf("rejects\n");
    static Rom rom;
    x68k::Config config;
    Box a(config);
    a.m->setIplRom(rom.b, sizeof rom.b);
    a.m->powerOn();
    a.frames(2);
    startScene(a, true);
    a.frames(5);
    Blob good;
    save(*a.m, good);
    Blob before;
    save(*a.m, before);

    auto expectReject = [&](const char* what, const uint8_t* data, size_t size, const char* want) {
        const char* err = nullptr;
        bool ok = x68k::loadState(*a.m, data, size, err);
        if (ok || !err || strcmp(err, want) != 0) {
            printf("  %s: loaded=%d error=%s (want %s)\n", what, ok, err ? err : "(none)", want);
            failures++;
        }
    };
    uint8_t* bad = new uint8_t[good.size];
    memcpy(bad, good.data, good.size);
    bad[8] = (uint8_t)(x68k::STATE_VERSION + 1); // 知らない版
    expectReject("version", bad, good.size, "unsupported state version");
    memcpy(bad, good.data, good.size);
    bad[0] = 'X';
    expectReject("magic", bad, good.size, "not a pix68k state file");
    memcpy(bad, good.data, good.size);
    bad[good.size / 2] ^= 0x40; // 中身
    expectReject("crc", bad, good.size, "state file is broken (CRC mismatch)");
    expectReject("truncated", good.data, good.size - 1, "state file is truncated");
    expectReject("short", good.data, 10, "not a pix68k state file");
    delete[] bad;

    Blob after;
    save(*a.m, after);
    if (!same(before, after)) {
        printf("  machine changed by a rejected load\n");
        reportDiff(before, after);
        failures++;
    }

    // 機種の違い: メイン RAM、クロック、IPL ROM
    x68k::Config ram2 = config;
    ram2.ramSize = 2u << 20;
    Box c(ram2);
    c.m->setIplRom(rom.b, sizeof rom.b);
    const char* err = nullptr;
    CHECK_EQ(x68k::loadState(*c.m, good.data, good.size, err), false);
    CHECK_EQ(err && !strcmp(err, "RAM size differs"), true);
    x68k::Config clk = config;
    clk.setClockMHz(10);
    Box d(clk);
    d.m->setIplRom(rom.b, sizeof rom.b);
    CHECK_EQ(x68k::loadState(*d.m, good.data, good.size, err), false);
    CHECK_EQ(err && !strcmp(err, "CPU clock differs"), true);
    Box e(config);
    Rom other;
    other.put16(0x0106, 0x4e71);
    e.m->setIplRom(other.b, sizeof other.b);
    CHECK_EQ(x68k::loadState(*e.m, good.data, good.size, err), false);
    CHECK_EQ(err && !strcmp(err, "IPL ROM differs"), true);
#ifndef PIX68K_NO_CPU020
    x68k::Config c030;
    c030.setCpu(68030);
    Box f(c030);
    f.m->setIplRom(rom.b, sizeof rom.b);
    CHECK_EQ(x68k::loadState(*f.m, good.data, good.size, err), false);
    CHECK_EQ(err && !strcmp(err, "CPU model differs"), true);
    {
        // 060turbo の 50MHz と 75MHz のステートは互いに読まない (ヘッダの CPU のクロック)
        x68k::Config c50;
        c50.setCpu(68060);
        Box g50(c50);
        g50.m->setIplRom(rom.b, sizeof rom.b);
        g50.m->powerOn();
        g50.m->runFrame();
        Blob s060;
        save(*g50.m, s060);
        x68k::Config c75 = c50;
        CHECK_EQ(c75.setClockMHz(75), true);
        Box g75(c75);
        g75.m->setIplRom(rom.b, sizeof rom.b);
        CHECK_EQ(x68k::loadState(*g75.m, s060.data, s060.size, err), false);
        CHECK_EQ(err && !strcmp(err, "CPU clock differs"), true);
        Box g50b(c50);
        g50b.m->setIplRom(rom.b, sizeof rom.b);
        CHECK_EQ(x68k::loadState(*g50b.m, s060.data, s060.size, err), true);
    }
#endif
}

// Sound の写し: 音の状態は写り、出力の周波数は写らない
void soundCopy() {
    printf("copySoundState\n");
    x68k::Sound a, b;
    a.setOutputRate(48000);
    b.setOutputRate(0);
    a.opmWrite(1, 0x14);
    a.opmWrite(3, 0x2a); // タイマを動かす
    a.advance(100000);
    x68k::copySoundState(a, b);
    CHECK_EQ(b.outputRate(), 0);
    CHECK_EQ(b.opm().reg(0x14), 0x2a);
    CHECK_EQ(b.opm().read(3), a.opm().read(3));
}

// ステートを読んだあと、ADPCM のクロック・左右・分周を読む前の値に戻すと、合成する側 (記録を当てる Sound) にも届く
void soundLinesAfterLoad() {
    printf("sound lines after load\n");
    static uint32_t ring[1u << 12];
    x68k::VideoJournal j;
    j.init(ring, 12);
    x68k::Sound m, r; // m は記録する側 (Machine の Sound)、r は記録を当てる側 (コア 3)
    m.setJournal(&j);
    x68k::Mercury* unit = nullptr; // まーきゅりーゆにっとはつながない
    auto drain = [&]() {
        m.flushJournal();
        const uint32_t* e;
        while (unsigned n = j.available(e)) {
            for (unsigned i = 0; i < n; i++) r.replay(e[i], unit);
            j.consume(n);
        }
    };
    m.setAdpcmLines(true, 3, 2); // S
    drain();
    x68k::StateIO size(x68k::StateIO::SIZE, nullptr, 0);
    m.state(size);
    uint8_t* buf = new uint8_t[size.pos()];
    x68k::StateIO save(x68k::StateIO::SAVE, buf, size.pos());
    m.state(save);
    CHECK_EQ(save.ok(), true);
    m.setAdpcmLines(false, 1, 0); // L
    drain();
    CHECK_EQ(r.adpcm().lines(), m.adpcm().lines());
    x68k::StateIO load(x68k::StateIO::LOAD, buf, size.pos());
    m.state(load); // S に戻る
    CHECK_EQ(load.ok(), true);
    x68k::copySoundState(m, r);
    CHECK_EQ(r.adpcm().lines(), m.adpcm().lines());
    m.setAdpcmLines(false, 1, 0); // 読む前の値 L に戻す
    drain();
    CHECK_EQ(m.adpcm().lines(), 0x01u);
    CHECK_EQ(r.adpcm().lines(), m.adpcm().lines());
    m.setJournal(nullptr);
    delete[] buf;
}

// 外付け SCSI ボード (CZ-6BS1 相当) の SPC で、CD-ROM (ID 6) から 1 ブロックを読んでいる途中に保存する。
// ボードの ROM はテスト用の値で埋めたもの (ドライバは動かさず、SPC のレジスタを直接触る)
struct ScsiRig {
    static constexpr uint32_t HD_BLOCKS = 64, CD_BLOCKS = 16;
    uint8_t hd[HD_BLOCKS * 512];
    uint8_t cd[CD_BLOCKS * 2048];
    x68k::HardDiskImage hdImg, cdImg;
    ScsiRig() {
        for (uint32_t i = 0; i < sizeof hd; i++) hd[i] = (uint8_t)(i * 7 + 1);
        for (uint32_t i = 0; i < sizeof cd; i++) cd[i] = (uint8_t)(i * 13 + (i >> 11));
        hdImg.load(hd, sizeof hd, 512, false);
        cdImg.load(cd, sizeof cd, 2048, true);
    }
};

uint8_t g_boardRom[x68k::Machine::SCSI_EX_ROM_SIZE];

void setupBoard(Box& b, ScsiRig& rig) {
    static Rom rom;
    for (uint32_t i = 0; i < sizeof g_boardRom; i++) g_boardRom[i] = (uint8_t)(i ^ 0x5a);
    b.m->setIplRom(rom.b, sizeof rom.b);
    CHECK_EQ(b.m->setScsiExRom(g_boardRom, sizeof g_boardRom), true);
    b.m->attachScsiEx(0, &rig.hdImg);
    b.m->attachScsiEx(6, &rig.cdImg, true);
    b.m->powerOn();
}

void spcTc(x68k::Spc& s, uint32_t n) {
    s.write(x68k::Spc::TCH, (uint8_t)(n >> 16));
    s.write(x68k::Spc::TCM, (uint8_t)(n >> 8));
    s.write(x68k::Spc::TCL, (uint8_t)n);
}

// 転送中のデータを n バイトまで受け取る。受け取った数を返す
uint32_t spcRead(x68k::Spc& s, uint8_t* out, uint32_t n) {
    uint32_t i = 0;
    while (i < n && (s.read(x68k::Spc::SSTS) & x68k::Spc::SSTS_TRIP) && !(s.read(x68k::Spc::SSTS) & x68k::Spc::SSTS_EMPTY))
        out[i++] = s.read(x68k::Spc::DREG);
    return i;
}

void scsiBoard() {
    printf("SCSI board (CD-ROM read in progress)\n");
    using x68k::Spc;
    x68k::Config config;
    ScsiRig rigA, rigB;
    Box a(config);
    setupBoard(a, rigA);
    a.frames(2);
    Spc& s = a.m->spcEx();
    spcTakeId7(s); // リセットの状態にして、自分の ID を 7 にする (続けて SCTL でリセットを解く)
    s.write(Spc::SCTL, 0x11);
    s.write(Spc::INTS, 0xff);
    // ID 6 を選ぶ
    s.write(Spc::PCTL, 0);
    s.write(Spc::TEMP, (uint8_t)(0x80 | 1 << 6));
    spcTc(s, 0x2000);
    s.write(Spc::SCMD, Spc::CMD_SELECT);
    CHECK_EQ(s.read(Spc::INTS), Spc::INTS_COMPLETE);
    s.write(Spc::INTS, 0xff);
    // READ(6): LBA 3 から 1 ブロック
    const uint8_t cdb[6] = {0x08, 0, 0, 3, 1, 0};
    s.write(Spc::PCTL, Spc::PH_COMMAND);
    spcTc(s, 6);
    s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    for (int i = 0; i < 6; i++) s.write(Spc::DREG, cdb[i]);
    CHECK_EQ(s.read(Spc::INTS), Spc::INTS_COMPLETE);
    s.write(Spc::INTS, 0xff);
    CHECK_EQ(s.read(Spc::PSNS) & 7, Spc::PH_DATA_IN);
    s.write(Spc::PCTL, Spc::PH_DATA_IN);
    spcTc(s, 2048);
    s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
    static uint8_t got[2048];
    uint32_t n = spcRead(s, got, 700);
    CHECK_EQ(n, 700);
    a.frames(1);
    Blob saved;
    if (!save(*a.m, saved)) return;
    static uint8_t restA[2048], restB[2048];
    uint32_t na = spcRead(s, restA, sizeof restA);
    CHECK_EQ(na, 2048 - 700);
    CHECK_EQ(memcmp(got, rigA.cd + 3 * 2048, 700), 0);
    CHECK_EQ(memcmp(restA, rigA.cd + 3 * 2048 + 700, na), 0);
    a.frames(3);

    Box b(config);
    setupBoard(b, rigB);
    b.frames(5);
    const char* err = nullptr;
    CHECK_EQ(x68k::loadState(*b.m, saved.data, saved.size, err), true);
    if (err) printf("  %s\n", err);
    uint32_t nb = spcRead(b.m->spcEx(), restB, sizeof restB);
    CHECK_EQ(nb, na);
    CHECK_EQ(memcmp(restA, restB, na), 0);
    b.frames(3);
    CHECK_EQ(b.checksum(), a.checksum());
    Blob sa, sb;
    save(*a.m, sa);
    save(*b.m, sb);
    if (!same(sa, sb)) {
        printf("  states after the SCSI read differ\n");
        reportDiff(sa, sb);
        failures++;
    }

    // ボードのない Machine と、ROM の違うボードには読まない
    Box c(config);
    static Rom rom;
    c.m->setIplRom(rom.b, sizeof rom.b);
    CHECK_EQ(x68k::loadState(*c.m, saved.data, saved.size, err), false);
    CHECK_EQ(err && !strcmp(err, "SCSI board ROM differs"), true);
    static uint8_t other[x68k::Machine::SCSI_EX_ROM_SIZE];
    memset(other, 0x33, sizeof other);
    c.m->setScsiExRom(other, sizeof other);
    CHECK_EQ(x68k::loadState(*c.m, saved.data, saved.size, err), false);
    CHECK_EQ(err && !strcmp(err, "SCSI board ROM differs"), true);

    // 違う種類の装置 (ID 6 にハードディスク) につないで読むと、転送の途中の状態は捨てる (読めるが、続きは来ない)
    ScsiRig rigC;
    Box d(config);
    static Rom rom2;
    d.m->setIplRom(rom2.b, sizeof rom2.b);
    d.m->setScsiExRom(g_boardRom, sizeof g_boardRom);
    d.m->attachScsiEx(6, &rigC.hdImg);
    d.m->powerOn();
    CHECK_EQ(x68k::loadState(*d.m, saved.data, saved.size, err), true);
    static uint8_t restD[2048];
    CHECK_EQ(spcRead(d.m->spcEx(), restD, sizeof restD), 0);
}

// ---------------------------------------------------------------- 周辺機器の動作の途中で保存する
//
// 周辺機器のレジスタを外から読み書きする手順を、短い区切り (ステップ) の並びにして書く。
//   N: 保存せずに、はじめから終わりまで動かす
//   A: ステップの境目ごとに保存しながら、はじめから終わりまで動かす (保存しても結果が変わらないこと)
//   B: 境目 k の保存を読み、k 番目から終わりまで動かす。これをすべての k について行う (1 つの Machine に次々に読むので、
//      読む前の状態は毎回違う)
// 終わりのチェックサム、保存、ディスクのイメージが N と同じになるかを見る。ディスクのイメージはステートに入らないので、
// B には A の境目 k の時点のイメージの写しを渡す。読んだ値は、ステップごとに決めた RAM の番地に書き残す (チェックサムに入る)
struct Env {
    static constexpr size_t FD_SIZE = 77 * 2 * 8 * 1024; // 2HD の XDF
    static constexpr size_t HD_SIZE = 512 * 256;         // SASI のイメージ (512 レコード)
    Box box;
    uint8_t* fd;
    uint8_t* hd;
    x68k::FloppyImage fdImg;
    x68k::HardDiskImage hdImg;
    ScsiRig scsi;
    Env(const x68k::Config& config, bool midi, bool board, bool fpu = false, bool mercury = false, bool cd = false)
        : box(config) {
        static Rom rom;
        fd = new uint8_t[FD_SIZE];
        hd = new uint8_t[HD_SIZE];
        for (size_t i = 0; i < FD_SIZE; i++) fd[i] = (uint8_t)(i * 5 + (i >> 10) + 3);
        for (size_t i = 0; i < HD_SIZE; i++) hd[i] = (uint8_t)(i * 11 + (i >> 8) + 1);
        CHECK_EQ(fdImg.load(fd, FD_SIZE, false, "test.xdf"), true);
        CHECK_EQ(hdImg.load(hd, HD_SIZE, 256, false), true);
        x68k::Machine& m = *box.m;
        m.setIplRom(rom.b, sizeof rom.b);
        if (midi) CHECK_EQ(m.setMidiBoard(true), true);
        if (fpu) CHECK_EQ(m.setFpuBoard(true), true);
        if (mercury) CHECK_EQ(m.setMercury(true), true);
        if (board) {
            for (uint32_t i = 0; i < sizeof g_boardRom; i++) g_boardRom[i] = (uint8_t)(i ^ 0x5a);
            CHECK_EQ(m.setScsiExRom(g_boardRom, sizeof g_boardRom), true);
            m.attachScsiEx(0, &scsi.hdImg);
            if (cd) m.attachScsiEx(6, &scsi.cdImg, true); // CD-ROM 装置
        }
        m.attachSasi(0, &hdImg);
        m.insertDisk(0, &fdImg);
        m.powerOn();
        box.frames(2);
    }
    ~Env() {
        box.m->ejectDisk(0);
        box.m->attachSasi(0, nullptr);
        fdImg.unload();
        delete[] fd;
        delete[] hd;
    }
    void out(uint32_t a, uint8_t v) { box.m->write8(a, v, m68k::FC_SUPER_DATA); }
    uint8_t in(uint32_t a) { return box.m->read8(a, m68k::FC_SUPER_DATA); }
    void out32(uint32_t a, uint32_t v) {
        for (int i = 0; i < 4; i++) out(a + (uint32_t)i, (uint8_t)(v >> (24 - 8 * i)));
    }
    // 時間を進める (音のリングはためない)
    void run(int64_t cycles) {
        static int16_t audio[2 * 4096];
        box.m->run(cycles);
        while (box.m->sound().ring().drain(audio, 4096) > 0) {
        }
    }
    // DMAC のチャネル ch を始める (割り込みは使わない)。チェーンなら mar は表の番地、count はブロックの数
    void dma(int ch, uint8_t ocr, uint32_t mar, uint32_t dar, uint16_t count) {
        const uint32_t b = 0xe84000 + (uint32_t)ch * 0x40;
        out(b + 0x00, 0xff); // CSR の終わりのビットを消す
        out(b + 0x04, 0x80); // DCR
        out(b + 0x05, ocr);
        out(b + 0x06, 0x04); // SCR: メモリの番地を増やす
        const bool chain = (ocr & 0x08) != 0;
        out(b + (chain ? 0x1a : 0x0a), (uint8_t)(count >> 8)); // BTC / MTC
        out(b + (chain ? 0x1b : 0x0b), (uint8_t)count);
        out32(b + (chain ? 0x1c : 0x0c), mar); // BAR / MAR
        out32(b + 0x14, dar);
        out(b + 0x29, 5); // MFC
        out(b + 0x31, 5); // DFC
        out(b + 0x39, 5); // BFC
        out(b + 0x07, 0x80); // CCR: STR
    }
};

struct Disks {
    std::vector<uint8_t> fd, hd, scsi;
    void take(const Env& e) {
        fd.assign(e.fd, e.fd + Env::FD_SIZE);
        hd.assign(e.hd, e.hd + Env::HD_SIZE);
        scsi.assign(e.scsi.hd, e.scsi.hd + sizeof e.scsi.hd);
    }
    void give(Env& e) const {
        memcpy(e.fd, fd.data(), fd.size());
        memcpy(e.hd, hd.data(), hd.size());
        memcpy(e.scsi.hd, scsi.data(), scsi.size());
    }
    bool same(const Disks& o) const { return fd == o.fd && hd == o.hd && scsi == o.scsi; }
};

// MIDI の音源の代わりに、届いたバイトを覚える
struct MidiLog : x68k::MidiOut {
    std::vector<uint8_t> bytes;
    void midiByte(uint8_t b) override { bytes.push_back(b); }
    void midiReset() override {}
};

// ステップ。log は、読んだ値を書き残してよい RAM の番地 (ステップごとに 256 バイト)
using Step = std::function<void(Env&, uint32_t log)>;
const uint32_t kLog = 0x3c0000;

void runScript(const char* name, const x68k::Config& config, bool midi, bool board, const std::vector<Step>& steps,
               const std::function<void(Env&)>& verify) {
    printf("%s (%zu save points)\n", name, steps.size() + 1);
    const size_t count = steps.size();
    auto prepare = [&](Env& e) {
        CHECK_EQ(scene::jumpTo(*e.box.m, scene::kCpuLoop, sizeof scene::kCpuLoop / 2), true);
        e.box.frames(1);
    };
    // N
    Env n(config, midi, board);
    MidiLog midiN;
    n.box.m->setMidiOut(&midiN);
    prepare(n);
    for (size_t i = 0; i < count; i++) steps[i](n, kLog + (uint32_t)i * 256);
    const uint32_t wantSum = n.box.checksum();
    Blob wantState;
    save(*n.box.m, wantState);
    Disks wantDisks;
    wantDisks.take(n);
    verify(n); // レジスタを読むと時間が進むので、比べる値を取ったあとに呼ぶ

    // A
    std::vector<Blob> saved(count + 1);
    std::vector<Disks> disks(count + 1);
    std::vector<size_t> midiAt(count + 1);
    {
        Env a(config, midi, board);
        MidiLog midiA;
        a.box.m->setMidiOut(&midiA);
        prepare(a);
        for (size_t i = 0; i <= count; i++) {
            if (!save(*a.box.m, saved[i])) return;
            disks[i].take(a);
            midiAt[i] = midiA.bytes.size();
            if (i < count) steps[i](a, kLog + (uint32_t)i * 256);
        }
        CHECK_EQ(a.box.checksum(), wantSum);
        Disks d;
        d.take(a);
        CHECK_EQ(d.same(wantDisks), true);
        CHECK_EQ(midiA.bytes == midiN.bytes, true);
        if (!same(saved[count], wantState)) {
            printf("  saving changed the result\n");
            reportDiff(wantState, saved[count]);
            failures++;
        }
    }

    // B
    Env b(config, midi, board);
    MidiLog midiB;
    b.box.m->setMidiOut(&midiB);
    startScene(b.box, true); // A とは違う場面
    b.box.frames(3);
    int bad = 0;
    for (size_t k = 0; k <= count && bad < 3; k++) {
        const int before = failures;
        disks[k].give(b);
        const char* err = nullptr;
        CHECK_EQ(x68k::loadState(*b.box.m, saved[k].data, saved[k].size, err), true);
        if (err) printf("  %s\n", err);
        Blob again;
        save(*b.box.m, again);
        if (!same(saved[k], again)) {
            printf("  save after load differs\n");
            reportDiff(saved[k], again);
            failures++;
        }
        // 読んだときに音源へ送り直す分 (覚えている設定) は比べない。そのあとに届くバイトが、保存しなかったときの続きと同じ
        midiB.bytes.clear();
        for (size_t i = k; i < count; i++) steps[i](b, kLog + (uint32_t)i * 256);
        CHECK_EQ(b.box.checksum(), wantSum);
        Blob end;
        save(*b.box.m, end);
        if (!same(end, wantState)) {
            printf("  states at the end differ\n");
            reportDiff(wantState, end);
            failures++;
        }
        Disks d;
        d.take(b);
        CHECK_EQ(d.same(wantDisks), true);
        const std::vector<uint8_t> rest(midiN.bytes.begin() + (long)midiAt[k], midiN.bytes.end());
        CHECK_EQ(midiB.bytes == rest, true);
        if (failures != before) {
            printf("  ^ loaded at save point %zu of %zu\n", k, count);
            bad++;
        }
    }
}

// SASI: WRITE (CPU からの書き込みと DMAC のチャネル 1 を混ぜる)、FORMAT BLOCK、REQUEST SENSE、ASSIGN DRIVE、READ の
// それぞれの途中 (セレクション、コマンドの途中、データの途中、ステータス、メッセージ)
void sasiScript() {
    const uint32_t DATA = 0xe96001, STATUS = 0xe96003, SELECT = 0xe96007;
    std::vector<Step> st;
    auto command = [&](std::initializer_list<uint8_t> cmd, size_t split) {
        st.push_back([=](Env& e, uint32_t log) {
            e.out(SELECT, 1);
            scene::poke8(*e.box.m, log, e.in(STATUS));
        });
        st.push_back([=](Env& e, uint32_t) { e.out(STATUS, 0); });
        const std::vector<uint8_t> bytes(cmd);
        st.push_back([=](Env& e, uint32_t) {
            for (size_t i = 0; i < split; i++) e.out(DATA, bytes[i]);
        });
        st.push_back([=](Env& e, uint32_t log) {
            for (size_t i = split; i < bytes.size(); i++) e.out(DATA, bytes[i]);
            scene::poke8(*e.box.m, log, e.in(STATUS));
        });
    };
    auto finish = [&]() {
        st.push_back([=](Env& e, uint32_t log) {
            scene::poke8(*e.box.m, log, e.in(STATUS));
            scene::poke8(*e.box.m, log + 1, e.in(DATA)); // ステータス
        });
        st.push_back([=](Env& e, uint32_t log) {
            scene::poke8(*e.box.m, log, e.in(STATUS));
            scene::poke8(*e.box.m, log + 1, e.in(DATA)); // メッセージ
            scene::poke8(*e.box.m, log + 2, e.in(STATUS));
        });
    };
    auto frame = [&]() { st.push_back([](Env& e, uint32_t) { e.box.frames(1); }); };
    // WRITE: レコード 5 から 2 レコード。100 バイトを CPU から、200 バイトを DMAC で、残りを CPU から
    st.push_back([](Env& e, uint32_t) {
        for (uint32_t i = 0; i < 512; i++) scene::poke8(*e.box.m, 0x360000 + i, (uint8_t)(i * 7 + 9));
    });
    command({0x0a, 0, 0, 5, 2, 0}, 3);
    st.push_back([=](Env& e, uint32_t) {
        for (uint32_t i = 0; i < 100; i++) e.out(DATA, (uint8_t)(i * 7 + 9));
    });
    frame();
    st.push_back([=](Env& e, uint32_t) { e.dma(1, 0x32, 0x360000 + 100, DATA, 200); });
    st.push_back([](Env& e, uint32_t) { e.run(3000); });
    st.push_back([=](Env& e, uint32_t log) {
        scene::poke8(*e.box.m, log, e.in(0xe84040)); // DMAC の CSR
        for (uint32_t i = 300; i < 512; i++) e.out(DATA, (uint8_t)(i * 7 + 9));
    });
    finish();
    // FORMAT BLOCK: レコード 66 から 33 レコード
    command({0x06, 0, 0, 66, 1, 0}, 5);
    finish();
    // 範囲の外の READ (エラー) のあとの REQUEST SENSE: 4 バイトのうち 2 バイトで区切る
    command({0x08, 0, 0xff, 0, 1, 0}, 2);
    finish();
    command({0x03, 0, 0, 0, 0, 0}, 1);
    const size_t senseStep = st.size();
    st.push_back([=](Env& e, uint32_t log) {
        for (uint32_t i = 0; i < 2; i++) scene::poke8(*e.box.m, log + i, e.in(DATA));
    });
    st.push_back([=](Env& e, uint32_t log) {
        for (uint32_t i = 0; i < 2; i++) scene::poke8(*e.box.m, log + i, e.in(DATA));
    });
    finish();
    // ASSIGN DRIVE: パラメータ 10 バイトのうち 4 バイトで区切る
    command({0xc2, 0, 0, 0, 0, 0}, 4);
    st.push_back([=](Env& e, uint32_t) {
        for (uint32_t i = 0; i < 4; i++) e.out(DATA, (uint8_t)(0x10 + i));
    });
    st.push_back([=](Env& e, uint32_t) {
        for (uint32_t i = 4; i < 10; i++) e.out(DATA, (uint8_t)(0x10 + i));
    });
    finish();
    // READ: レコード 5 から 2 レコード。150 バイトを CPU から、残りを DMAC で $370000 へ
    command({0x08, 0, 0, 5, 2, 0}, 5);
    st.push_back([=](Env& e, uint32_t log) {
        for (uint32_t i = 0; i < 150; i++) scene::poke8(*e.box.m, log + i, e.in(DATA));
    });
    frame();
    st.push_back([=](Env& e, uint32_t) { e.dma(1, 0xb2, 0x370000, DATA, 362); });
    st.push_back([](Env& e, uint32_t) { e.run(3000); });
    finish();
    st.push_back([](Env& e, uint32_t) { e.box.frames(3); });
    x68k::Config config;
    runScript("SASI: WRITE / FORMAT BLOCK / REQUEST SENSE / ASSIGN DRIVE / READ", config, false, false, st, [=](Env& e) {
        bool wrote = true, zero = true, read = true, kept = true;
        for (uint32_t i = 0; i < 512; i++) wrote = wrote && e.hd[5 * 256 + i] == (uint8_t)(i * 7 + 9);
        for (uint32_t i = 0; i < 33 * 256; i++) zero = zero && e.hd[66 * 256 + i] == 0;
        for (uint32_t i = 150; i < 512; i++) read = read && peek32(*e.box.m, 0x370000 + i - 150) >> 24 == (uint8_t)(i * 7 + 9);
        for (size_t i = 99 * 256; i < Env::HD_SIZE; i++) kept = kept && e.hd[i] == (uint8_t)(i * 11 + (i >> 8) + 1);
        CHECK_EQ(wrote, true);
        CHECK_EQ(zero, true);
        CHECK_EQ(read, true);
        CHECK_EQ(kept, true);
        // REQUEST SENSE の 1 バイト目: 番地が範囲の外
        // (bit7 は「番地が有効」)
        CHECK_EQ(peek32(*e.box.m, kLog + 256 * (uint32_t)senseStep) >> 24,
                 (uint32_t)(x68k::Sasi::SENSE_ADDRESS_VALID | x68k::Sasi::SENSE_INVALID_ADDRESS));
    });
}

// FDC: コマンドの途中、DMAC の転送の前と後、結果の途中、DMA を使わない読み書きのセクタの途中、フォーマット (WRITE ID) の途中
void fdcScript() {
    const uint32_t FSTAT = 0xe94001, FDATA = 0xe94003;
    std::vector<Step> st;
    auto command = [&](std::initializer_list<uint8_t> cmd, size_t split) {
        const std::vector<uint8_t> bytes(cmd);
        if (split)
            st.push_back([=](Env& e, uint32_t) {
                for (size_t i = 0; i < split; i++) e.out(FDATA, bytes[i]);
            });
        st.push_back([=](Env& e, uint32_t log) {
            for (size_t i = split; i < bytes.size(); i++) e.out(FDATA, bytes[i]);
            scene::poke8(*e.box.m, log, e.in(FSTAT));
        });
    };
    // 結果を読む。先に状態を読む (DMA を使わない転送は、状態を読んだときに結果のフェーズに移る)
    auto result = [&](uint32_t n, uint32_t split) {
        st.push_back([=](Env& e, uint32_t log) {
            scene::poke8(*e.box.m, log + 17, e.in(FSTAT));
            for (uint32_t i = 0; i < split; i++) scene::poke8(*e.box.m, log + i, e.in(FDATA));
        });
        st.push_back([=](Env& e, uint32_t log) {
            for (uint32_t i = split; i < n; i++) scene::poke8(*e.box.m, log + i, e.in(FDATA));
            scene::poke8(*e.box.m, log + 16, e.in(FSTAT));
        });
    };
    auto seek = [&](uint8_t cyl) {
        command({0x0f, 0x00, cyl}, 2);
        st.push_back([](Env& e, uint32_t) { e.run(2000); });
        command({0x08}, 0);
        result(2, 1);
    };
    // フォーマットで書く ID (C、H、R、N) の i バイト目
    auto id = [](uint32_t i) { return (uint8_t)(i % 4 == 0 ? 4 : i % 4 == 1 ? 0 : i % 4 == 2 ? i / 4 + 1 : 3); };
    st.push_back([](Env& e, uint32_t) {
        e.out(0xe94007, 0x80); // ドライブ 0 のモーター
        for (uint32_t i = 0; i < 4096; i++) scene::poke8(*e.box.m, 0x360000 + i, (uint8_t)(i * 13 + 5));
    });
    command({0x03, 0xdf, 0x02}, 1); // SPECIFY (DMA)
    command({0x07, 0x00}, 1);       // RECALIBRATE
    command({0x08}, 0);
    result(2, 1);
    // READ DATA (DMA): シリンダ 2 のセクタ 1 から。DMAC は 1500 バイトで終わる (2 つ目のセクタの途中)
    seek(2);
    command({0x46, 0x00, 2, 0, 1, 3, 8, 0x1b, 0xff}, 4);
    st.push_back([=](Env& e, uint32_t) { e.dma(0, 0xb2, 0x370000, FDATA, 1500); });
    st.push_back([](Env& e, uint32_t) { e.run(3000); });
    result(7, 3);
    // READ DATA (DMA を使わない): 2 セクタを、700 バイト、900 バイト、残りに分けて読む
    command({0x03, 0xdf, 0x03}, 0); // SPECIFY (ND)
    command({0x46, 0x00, 2, 0, 5, 3, 6, 0x1b, 0xff}, 8);
    for (uint32_t from : {0u, 700u, 1600u}) {
        const uint32_t to = from == 0 ? 700 : from == 700 ? 1600 : 2048;
        st.push_back([=](Env& e, uint32_t) {
            for (uint32_t i = from; i < to; i++) scene::poke8(*e.box.m, 0x380000 + i, e.in(FDATA));
        });
    }
    result(7, 0);
    // WRITE DATA (DMA): シリンダ 3 のセクタ 3 から 1324 バイト (2 つ目のセクタの途中で終わる)
    command({0x03, 0xdf, 0x02}, 0);
    seek(3);
    command({0x45, 0x00, 3, 0, 3, 3, 8, 0x1b, 0xff}, 6);
    st.push_back([](Env& e, uint32_t) { e.box.frames(1); });
    st.push_back([=](Env& e, uint32_t) { e.dma(0, 0x32, 0x360000, FDATA, 1324); });
    st.push_back([](Env& e, uint32_t) { e.run(3000); });
    result(7, 4);
    // WRITE DATA (DMA を使わない): シリンダ 3 の裏のセクタ 1 から 2 セクタを、500 バイトと残りに分けて書く
    command({0x03, 0xdf, 0x03}, 0);
    command({0x45, 0x04, 3, 1, 1, 3, 2, 0x1b, 0xff}, 1);
    st.push_back([=](Env& e, uint32_t) {
        for (uint32_t i = 0; i < 500; i++) e.out(FDATA, (uint8_t)(i * 3 + 1));
    });
    st.push_back([=](Env& e, uint32_t) {
        for (uint32_t i = 500; i < 2048; i++) e.out(FDATA, (uint8_t)(i * 3 + 1));
    });
    result(7, 0);
    // WRITE ID (フォーマット): シリンダ 4 の 8 セクタの ID (4 バイトずつ) を、10 バイトと残りに分けて書く
    seek(4);
    command({0x4d, 0x00, 3, 8, 0x1b, 0xe5}, 3);
    st.push_back([=](Env& e, uint32_t) {
        for (uint32_t i = 0; i < 10; i++) e.out(FDATA, id(i));
    });
    st.push_back([=](Env& e, uint32_t) {
        for (uint32_t i = 10; i < 32; i++) e.out(FDATA, id(i));
    });
    result(7, 2);
    st.push_back([](Env& e, uint32_t) { e.box.frames(3); });
    x68k::Config config;
    runScript("FDC: READ DATA / WRITE DATA / WRITE ID, with and without DMA", config, false, false, st, [](Env& e) {
        auto at = [](int cyl, int head, int r) { return (size_t)((cyl * 2 + head) * 8 + r - 1) * 1024; };
        auto orig = [](size_t i) { return (uint8_t)(i * 5 + (i >> 10) + 3); };
        bool dmaRead = true, pioRead = true, dmaWrite = true, pioWrite = true, format = true;
        for (uint32_t i = 0; i < 1500; i++) dmaRead = dmaRead && peek32(*e.box.m, 0x370000 + i) >> 24 == orig(at(2, 0, 1) + i);
        for (uint32_t i = 0; i < 2048; i++) pioRead = pioRead && peek32(*e.box.m, 0x380000 + i) >> 24 == orig(at(2, 0, 5) + i);
        for (uint32_t i = 0; i < 1024; i++) dmaWrite = dmaWrite && e.fd[at(3, 0, 3) + i] == (uint8_t)(i * 13 + 5);
        for (uint32_t i = 0; i < 2048; i++) pioWrite = pioWrite && e.fd[at(3, 1, 1) + i] == (uint8_t)(i * 3 + 1);
        for (uint32_t i = 0; i < 8192; i++) format = format && e.fd[at(4, 0, 1) + i] == 0xe5;
        CHECK_EQ(dmaRead, true);
        CHECK_EQ(pioRead, true);
        CHECK_EQ(dmaWrite, true);
        CHECK_EQ(pioWrite, true);
        CHECK_EQ(format, true);
    });
}

// ADPCM を DMAC のチャネル 3 (アレイチェーン 2 ブロック) で再生している途中と、OPM の音とタイマが動いている途中
void adpcmScript() {
    std::vector<Step> st;
    st.push_back([](Env& e, uint32_t) {
        x68k::Machine& m = *e.box.m;
        // アレイチェーンの表: $360000 から 300 バイト、$361000 から 400 バイト
        poke32(m, 0x320000, 0x00330000);
        scene::poke16(m, 0x320004, 300);
        poke32(m, 0x320006, 0x00331000);
        scene::poke16(m, 0x32000a, 400);
        for (uint32_t i = 0; i < 0x2000; i++) scene::poke8(m, 0x360000 + i, (uint8_t)(i * 37 + (i >> 3)));
        auto opm = [&](uint8_t r, uint8_t v) {
            e.out(0xe90001, r);
            e.out(0xe90003, v);
            e.run(200); // BUSY が消えるのを待つ
        };
        opm(0x20, 0xc7); // チャネル 0: 左右、CON 7
        opm(0x28, 0x4a);
        opm(0x18, 0xc0); // LFO
        opm(0x19, 0x7f);
        opm(0x38, 0x33); // PMS / AMS
        for (uint8_t op = 0; op < 4; op++) {
            opm((uint8_t)(0x40 + op * 8), 0x01);
            opm((uint8_t)(0x60 + op * 8), 0x10);
            opm((uint8_t)(0x80 + op * 8), 0x14);
            opm((uint8_t)(0xa0 + op * 8), 0x85);
            opm((uint8_t)(0xe0 + op * 8), 0x24);
        }
        opm(0x08, 0x78); // キーオン
        opm(0x10, 0xf0); // タイマ A
        opm(0x11, 0x02);
        opm(0x12, 0xfa); // タイマ B
        opm(0x14, 0x3f); // タイマを動かす
        e.out(0xe9a007, 0x92); // 8255: ポート C を出力に
        e.out(0xe9a005, 0x08); // ADPCM: 分周 512、左右とも出す
    });
    st.push_back([](Env& e, uint32_t) { e.dma(3, 0x3a, 0x320000, 0xe92003, 2); });
    st.push_back([](Env& e, uint32_t) { e.out(0xe92001, 0x02); }); // 再生開始
    for (int i = 0; i < 3; i++) st.push_back([](Env& e, uint32_t) { e.box.frames(1); });
    const size_t midStep = st.size();
    st.push_back([](Env& e, uint32_t log) {
        e.run(12345);
        scene::poke8(*e.box.m, log, e.in(0xe840c0));     // DMAC の CSR
        scene::poke8(*e.box.m, log + 1, e.in(0xe840cb)); // MTC の下位
        scene::poke8(*e.box.m, log + 2, e.in(0xe90003)); // OPM の状態
        scene::poke8(*e.box.m, log + 3, e.in(0xe92001)); // ADPCM の状態
    });
    for (int i = 0; i < 4; i++)
        st.push_back([](Env& e, uint32_t log) {
            e.box.frames(1);
            scene::poke8(*e.box.m, log, e.in(0xe840c0));
            scene::poke8(*e.box.m, log + 1, e.in(0xe840cb));
            scene::poke8(*e.box.m, log + 2, e.in(0xe90003));
        });
    st.push_back([](Env& e, uint32_t) { e.out(0xe92001, 0x01); }); // 停止 (残った値を 0 に近づけていく)
    st.push_back([](Env& e, uint32_t) { e.run(40000); });
    st.push_back([](Env& e, uint32_t log) {
        e.out(0xe90001, 0x08);
        e.out(0xe90003, 0x00); // キーオフ
        e.box.frames(3);
        scene::poke8(*e.box.m, log, e.in(0xe840c0));
    });
    x68k::Config config;
    runScript("ADPCM through DMAC channel 3 (array chain) with OPM voice and timers", config, false, false, st, [=](Env& e) {
        // 途中の境目では DMAC が動いていて、ADPCM は再生中 (ステータスは $40)
        CHECK_EQ(peek32(*e.box.m, kLog + 256 * (uint32_t)midStep) >> 24 & 0x08, 0x08u);
        CHECK_EQ(peek32(*e.box.m, kLog + 256 * (uint32_t)midStep + 3) >> 24, 0x40u);
        CHECK_EQ(e.in(0xe840c0) & 0x88, 0x80u); // 終わりには転送が終わっている
    });
}

// MIDI ボードで送信している途中 (FIFO にたまっているバイト、シフトレジスタのバイト、汎用タイマ)
void midiScript() {
    const uint32_t R1 = 0xeafa03, R4 = 0xeafa09, R5 = 0xeafa0b, R6 = 0xeafa0d;
    static const uint8_t song[] = {0xc0, 0x05, 0xb0, 0x07, 0x40, 0x90, 0x45, 0x7f,
                                   0x48, 0x7f, 0x80, 0x45, 0x40, 0xe0, 0x00, 0x50};
    std::vector<Step> st;
    st.push_back([=](Env& e, uint32_t) {
        e.out(R1, 0x80); // リセット
        e.out(R1, 4);
        e.out(R4, 0x08); // TRR: 31.25kbps
        e.out(R1, 8);
        e.out(R4, 0x40); // GTR
        e.out(R5, 0x01);
        e.out(R1, 5);
        e.out(R5, 0x01); // TCR: 送信する
        for (int i = 0; i < 10; i++) e.out(R6, song[i]);
    });
    for (int i = 0; i < 8; i++)
        st.push_back([=](Env& e, uint32_t log) {
            e.run(2100); // 1 バイト (320us、16MHz で 5120 サイクル) より短い
            e.out(R1, 5);
            scene::poke8(*e.box.m, log, e.in(R4)); // TSR
        });
    st.push_back([=](Env& e, uint32_t) {
        e.out(R1, 5);
        for (int i = 10; i < 16; i++) e.out(R6, song[i]);
    });
    for (int i = 0; i < 6; i++)
        st.push_back([=](Env& e, uint32_t log) {
            e.run(7000);
            e.out(R1, 5);
            scene::poke8(*e.box.m, log, e.in(R4));
        });
    st.push_back([](Env& e, uint32_t) { e.box.frames(3); });
    x68k::Config config;
    runScript("MIDI board: bytes in the FIFO and in the shift register", config, true, false, st, [](Env& e) {
        CHECK_EQ(peek32(*e.box.m, kLog + 256 * 1) >> 24 & 0x01, 0x01u); // 1 つ目の境目では送信中
        e.out(0xeafa03, 5);
        CHECK_EQ(e.in(0xeafa09) & 0x81, 0x80u); // 終わりには送り終えている
    });
}

// 外付け SCSI ボードの SPC で、WRITE(6) と READ(6) のコマンドの途中、データの途中、ステータスとメッセージの間
void scsiScript() {
    using x68k::Spc;
    std::vector<Step> st;
    auto note = [](Env& e, uint32_t a, uint8_t v) { scene::poke8(*e.box.m, a, v); };
    auto select = [&]() {
        st.push_back([=](Env& e, uint32_t log) {
            Spc& s = e.box.m->spcEx();
            s.write(Spc::INTS, 0xff);
            s.write(Spc::PCTL, 0);
            s.write(Spc::TEMP, 0x81);
            spcTc(s, 0x2000);
            s.write(Spc::SCMD, Spc::CMD_SELECT);
            note(e, log, s.read(Spc::INTS));
            s.write(Spc::INTS, 0xff);
        });
    };
    auto command = [&](std::initializer_list<uint8_t> cdb) {
        const std::vector<uint8_t> bytes(cdb);
        st.push_back([=](Env& e, uint32_t) {
            Spc& s = e.box.m->spcEx();
            s.write(Spc::PCTL, Spc::PH_COMMAND);
            spcTc(s, 6);
            s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
            for (int i = 0; i < 3; i++) s.write(Spc::DREG, bytes[(size_t)i]);
        });
        st.push_back([=](Env& e, uint32_t log) {
            Spc& s = e.box.m->spcEx();
            for (int i = 3; i < 6; i++) s.write(Spc::DREG, bytes[(size_t)i]);
            note(e, log, s.read(Spc::INTS));
            s.write(Spc::INTS, 0xff);
            note(e, log + 1, s.read(Spc::PSNS));
        });
    };
    auto finish = [&]() {
        for (int k = 0; k < 2; k++)
            st.push_back([=](Env& e, uint32_t log) {
                Spc& s = e.box.m->spcEx();
                note(e, log, s.read(Spc::PSNS));
                note(e, log + 1, s.read(Spc::TEMP));
                s.write(Spc::SCMD, Spc::CMD_SET_ACK);
                s.write(Spc::SCMD, Spc::CMD_RESET_ACK);
                note(e, log + 2, s.read(Spc::PSNS));
            });
    };
    st.push_back([](Env& e, uint32_t) {
        Spc& s = e.box.m->spcEx();
        spcTakeId7(s); // リセットの状態にして、自分の ID を 7 にする (続けて SCTL でリセットを解く)
        s.write(Spc::SCTL, 0x11);
        s.write(Spc::INTS, 0xff);
    });
    // WRITE(6): ブロック 4 から 2 ブロックを、300 バイトと残りに分けて書く
    select();
    command({0x0a, 0, 0, 4, 2, 0});
    st.push_back([=](Env& e, uint32_t) {
        Spc& s = e.box.m->spcEx();
        s.write(Spc::PCTL, Spc::PH_DATA_OUT);
        spcTc(s, 1024);
        s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
        for (uint32_t i = 0; i < 300; i++) s.write(Spc::DREG, (uint8_t)(i * 9 + 2));
    });
    st.push_back([](Env& e, uint32_t) { e.box.frames(1); });
    st.push_back([=](Env& e, uint32_t log) {
        Spc& s = e.box.m->spcEx();
        for (uint32_t i = 300; i < 1024; i++) s.write(Spc::DREG, (uint8_t)(i * 9 + 2));
        note(e, log, s.read(Spc::INTS));
        s.write(Spc::INTS, 0xff);
    });
    finish();
    // READ(6): 同じ 2 ブロックを、500 バイトと残りに分けて読む
    select();
    command({0x08, 0, 0, 4, 2, 0});
    st.push_back([=](Env& e, uint32_t) {
        Spc& s = e.box.m->spcEx();
        s.write(Spc::PCTL, Spc::PH_DATA_IN);
        spcTc(s, 1024);
        s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
        static uint8_t buf[500];
        const uint32_t n = spcRead(s, buf, 500);
        for (uint32_t i = 0; i < n; i++) scene::poke8(*e.box.m, 0x370000 + i, buf[i]);
    });
    st.push_back([](Env& e, uint32_t) { e.run(5000); });
    st.push_back([=](Env& e, uint32_t log) {
        Spc& s = e.box.m->spcEx();
        static uint8_t buf[524];
        const uint32_t n = spcRead(s, buf, 524);
        for (uint32_t i = 0; i < n; i++) scene::poke8(*e.box.m, 0x370000 + 500 + i, buf[i]);
        note(e, log, s.read(Spc::INTS));
        s.write(Spc::INTS, 0xff);
    });
    finish();
    st.push_back([](Env& e, uint32_t) { e.box.frames(3); });
    x68k::Config config;
    runScript("SCSI board: WRITE(6) / READ(6)", config, false, true, st, [](Env& e) {
        bool wrote = true, read = true;
        for (uint32_t i = 0; i < 1024; i++) {
            wrote = wrote && e.scsi.hd[4 * 512 + i] == (uint8_t)(i * 9 + 2);
            read = read && peek32(*e.box.m, 0x370000 + i) >> 24 == (uint8_t)(i * 9 + 2);
        }
        CHECK_EQ(wrote, true);
        CHECK_EQ(read, true);
    });
}

// ---------------------------------------------------------------- 壊れたステート
//
// ファイルが途中で切れた、ビットが変わった、という壊れ方は、長さと CRC で読む前に断る (rejects())。
// ここでは、中身を書き換えたうえで CRC を合わせ直したもの (別の版のプログラムが書いたファイルや、わざと作ったファイル)
// を読ませる。並びが合わなくなるもの、範囲の外の値になるものは断り (そのとき Machine は変わらない)、読んだものは
// そのあと動かしても範囲の外を読み書きしないこと。ASan / UBSan のビルドで動かして確かめる
uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t crc = ~0u;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc & 1) ? 0xedb88320u ^ (crc >> 1) : crc >> 1;
    }
    return ~crc;
}

void put32le(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

// ヘッダの中身の大きさと CRC を、今の中身に合わせる
void fixHeader(uint8_t* data, size_t size) {
    put32le(data + 48, (uint32_t)(size - x68k::STATE_HEADER_SIZE));
    put32le(data + 52, 0);
    put32le(data + 56, crc32(data + x68k::STATE_HEADER_SIZE, size - x68k::STATE_HEADER_SIZE));
}

// 区切りの値 (StateIO::tag() の 4 文字) の位置。なければ 0
size_t findTag(const Blob& b, const char* tag, size_t from = x68k::STATE_HEADER_SIZE) {
    for (size_t i = from; i + 4 <= b.size; i++)
        if (b.data[i] == (uint8_t)tag[3] && b.data[i + 1] == (uint8_t)tag[2] && b.data[i + 2] == (uint8_t)tag[1] &&
            b.data[i + 3] == (uint8_t)tag[0])
            return i;
    return 0;
}

// いくつもの周辺機器を動作の途中にする (SASI の書き込み、FDC の読み込み、ADPCM の再生、OPM、MIDI の送信、SCSI ボードの読み込み)。
// frame が false なら、最後の 1 フレームを runFrame() でなく run() で進める (runFrame() は上限を超えた時刻を戻すので、
// 上限を超えたまま動かす試験で使う。Machine::rebaseTime())
void makeBusy(Env& e, bool midi, bool board, bool cd = false, bool frame = true) {
    x68k::Machine& m = *e.box.m;
    // SASI: WRITE の 100 バイト目まで
    e.out(0xe96007, 1);
    e.out(0xe96003, 0);
    for (uint8_t b : {0x0a, 0, 0, 5, 2, 0}) e.out(0xe96001, b);
    for (uint32_t i = 0; i < 100; i++) e.out(0xe96001, (uint8_t)i);
    // FDC: DMA を使わない READ DATA の 300 バイト目まで
    e.out(0xe94007, 0x80);
    for (uint8_t b : {0x03, 0xdf, 0x03}) e.out(0xe94003, b);
    for (uint8_t b : {0x46, 0x00, 0, 0, 1, 3, 8, 0x1b, 0xff}) e.out(0xe94003, b);
    for (uint32_t i = 0; i < 300; i++) e.in(0xe94003);
    // ADPCM を DMAC のチャネル 3 (アレイチェーン) で再生する。OPM の音とタイマ
    poke32(m, 0x320000, 0x00360000);
    scene::poke16(m, 0x320004, 3000);
    poke32(m, 0x320006, 0x00361000);
    scene::poke16(m, 0x32000a, 3000);
    for (uint32_t i = 0; i < 0x2000; i++) scene::poke8(m, 0x360000 + i, (uint8_t)(i * 37 + (i >> 3)));
    static const uint8_t kOpm[][2] = {{0x20, 0xc7}, {0x28, 0x4a}, {0x18, 0xc0}, {0x19, 0x7f}, {0x38, 0x33}, {0x80, 0x14},
                                      {0xe0, 0x24}, {0x08, 0x78}, {0x10, 0xf0}, {0x12, 0xfa}, {0x14, 0x3f}};
    for (const auto& rv : kOpm) {
        e.out(0xe90001, rv[0]);
        e.out(0xe90003, rv[1]);
        e.run(200); // BUSY が消えるのを待つ
    }
    e.out(0xe9a007, 0x92);
    e.out(0xe9a005, 0x08);
    e.dma(3, 0x3a, 0x320000, 0xe92003, 2);
    e.out(0xe92001, 0x02);
    if (midi) {
        e.out(0xeafa03, 0x80);
        e.out(0xeafa03, 4);
        e.out(0xeafa09, 0x08);
        e.out(0xeafa03, 8);
        e.out(0xeafa09, 0x40);
        e.out(0xeafa0b, 0x01);
        e.out(0xeafa03, 5);
        e.out(0xeafa0b, 0x01);
        for (uint8_t b : {0xc0, 0x05, 0xb0, 0x07, 0x40, 0x90, 0x45, 0x7f, 0x48, 0x7f}) e.out(0xeafa0d, b);
    }
    if (board) {
        using x68k::Spc;
        Spc& s = m.spcEx();
        spcTakeId7(s); // リセットの状態にして、自分の ID を 7 にする (続けて SCTL でリセットを解く)
        s.write(Spc::SCTL, 0x11);
        s.write(Spc::INTS, 0xff);
        s.write(Spc::PCTL, 0);
        s.write(Spc::TEMP, cd ? 0xc0 : 0x81); // CD-ROM 装置は ID 6
        spcTc(s, 0x2000);
        s.write(Spc::SCMD, Spc::CMD_SELECT);
        s.write(Spc::INTS, 0xff);
        s.write(Spc::PCTL, Spc::PH_COMMAND);
        static uint8_t buf[200];
        if (cd) {
            // READ TOC の応答 (CD-ROM 装置の中のバッファ) を 3 バイト読んだところ
            spcTc(s, 10);
            s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
            for (uint8_t b : {0x43, 0, 0, 0, 0, 0, 0, 0, 20, 0}) s.write(Spc::DREG, b);
            s.write(Spc::INTS, 0xff);
            s.write(Spc::PCTL, Spc::PH_DATA_IN);
            spcTc(s, 20);
            s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
            spcRead(s, buf, 3);
        } else {
            spcTc(s, 6);
            s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
            for (uint8_t b : {0x08, 0, 0, 4, 2, 0}) s.write(Spc::DREG, b);
            s.write(Spc::INTS, 0xff);
            s.write(Spc::PCTL, Spc::PH_DATA_IN);
            spcTc(s, 1024);
            s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
            spcRead(s, buf, sizeof buf);
        }
    }
    m.input().onKey(0x04, true);
    m.input().onKey(0x05, true);
    m.input().onMouse(30, -12, 1);
    if (frame) e.box.frames(1);
    else e.run((int64_t)m.cpuHz() / 55);
    m.input().onKey(0x04, false);
    e.run(30000);
}

// 読んだあとに、周辺機器のレジスタを読み書きしてから動かす
void exercise(Env& e, bool midi, bool board) {
    x68k::Machine& m = *e.box.m;
    e.in(0xe94001);
    for (int i = 0; i < 3; i++) e.in(0xe94003);
    e.out(0xe94003, 0x08);
    e.in(0xe96003);
    for (int i = 0; i < 3; i++) e.out(0xe96001, (uint8_t)(0x55 + i));
    e.in(0xe96001);
    for (uint32_t ch = 0; ch < 4; ch++) e.in(0xe84000 + ch * 0x40);
    e.in(0xe90003);
    e.in(0xe92001);
    e.in(0xe8a001); // RTC
    e.in(0xe98005); // SCC
    e.in(0xe9a001); // 8255
    for (uint32_t r = 1; r < 0x30; r += 2) e.in(0xe88000 + r); // MFP
    if (midi) {
        for (uint8_t g = 0; g < 10; g++) {
            e.out(0xeafa03, g);
            for (uint32_t r = 4; r < 8; r++) e.in(0xeafa01 + r * 2);
        }
        e.out(0xeafa03, 5);
        e.out(0xeafa0d, 0x90);
    }
    if (board) {
        using x68k::Spc;
        Spc& s = m.spcEx();
        for (int r = 0; r < 16; r++) s.read(r);
        s.write(Spc::DREG, 0x12);
        s.write(Spc::SCMD, Spc::CMD_SET_ACK);
        s.write(Spc::SCMD, Spc::CMD_RESET_ACK);
        s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
        for (int i = 0; i < 4; i++) s.read(Spc::DREG);
    }
    m.input().onKey(0x06, true);
    m.input().onMouse(-5, 9, 2);
    e.box.frames(2);
    m.input().onKey(0x06, false);
    e.box.frames(1);
}

volatile int g_round = 0;
void onAlarm(int) {
    char msg[64];
    const int n = snprintf(msg, sizeof msg, "  round %d does not finish\n", g_round);
    if (n > 0 && write(1, msg, (size_t)n) < 0) {
    }
    _exit(1);
}

// start は、Env を作ったあとに呼ぶ (場面を始める)。rounds 回、書き換えたステートを読ませる
void corrupted(const char* name, const x68k::Config& config, bool midi, bool board,
               const std::function<void(Env&)>& start, uint32_t seed, bool fpu = false, bool mercury = false,
               bool cd = false) {
    int rounds = 150;
    if (const char* r = getenv("STATE_TEST_ROUNDS")) rounds = atoi(r);
    const bool verbose = getenv("STATE_TEST_VERBOSE") != nullptr;
    signal(SIGALRM, onAlarm);
    Env src(config, midi, board, fpu, mercury, cd);
    start(src);
    makeBusy(src, midi, board, cd);
    if (cd) {
        // CD-ROM 装置の中のバッファの転送の途中になっていること (なっていなければ、その部分を試していない)
        using x68k::Spc;
        CHECK_EQ(src.box.m->spcEx().read(Spc::PSNS) & (Spc::PSNS_BSY | 7), Spc::PSNS_BSY | Spc::PH_DATA_IN);
    }
    Blob good;
    if (!save(*src.box.m, good)) return;
    Disks disks;
    disks.take(src);
    // findTag() は 4 文字を逆の順で探す。CPU と FPU の区切り ("6000"、"6030"、"6882") は、ファイルの中でこの順に並ぶ値で
    // 書いてあるので、逆に書く ("0006"、"0306"、"2886")
    static const char* const kTags[] = {"MACH", "0006", "0306", "2886", "VID\0", "KEYB", "FDC ", "SND ", "SCC ", "MOUS",
                                        "SASI", "SPC ", "MIDI", "TIME", "TLB ", "6881", "MERC", "CD  "};
    std::vector<size_t> tagAt;
    for (const char* t : kTags) {
        for (size_t at = findTag(good, t); at; at = findTag(good, t, at + 4)) tagAt.push_back(at);
    }
    // ねらう区切りが見つかっていること (見つからなければ、その部分を重点的に書き換えていない)
    {
        const bool cpu020 = config.cpuModel != m68k::CpuModel::M68000;
        std::vector<const char*> need = {"MACH", "0006", "VID\0", "KEYB", "FDC ", "SND ", "SCC ", "MOUS", "SASI", "SPC ", "TIME"};
        if (cpu020) need.insert(need.end(), {"0306", "2886", "TLB "});
        if (midi) need.push_back("MIDI");
        if (fpu) need.push_back("6881");
        if (mercury) need.push_back("MERC");
        if (cd) need.push_back("CD  "); // CD-ROM 装置の状態は、装置があるときだけ入る
        for (const char* t : need) {
            if (!findTag(good, t)) {
                printf("  tag %.4s not found in the state\n", t);
                failures++;
            }
        }
    }
    if (verbose) { // 書き換えた場所がどの部分かを調べるための、区切りの位置
        for (const char* t : kTags)
            for (size_t at = findTag(good, t); at; at = findTag(good, t, at + 4)) printf("  tag %.4s at %zu\n", t, at);
    }
    // VID のあとは VRAM が続くので、次の区切り (KEYB) の手前 (パレット、MFP) も狙う
    const size_t keyb = findTag(good, "KEYB");

    Env dst(config, midi, board, fpu, mercury, cd);
    start(dst);
    scene::Rng rng;
    rng.s = seed;
    if (const char* x = getenv("STATE_TEST_SEED")) rng.s ^= (uint32_t)strtoul(x, nullptr, 0) * 2654435761u;
    if (rng.s == 0) rng.s = 1;
    std::vector<uint8_t> bad(good.size);
    int loaded = 0, rejected = 0;
    for (int round = 0; round < rounds; round++) {
        memcpy(bad.data(), good.data, good.size);
        const int n = 1 + (int)(rng() % 4);
        for (int k = 0; k < n; k++) {
            if (rng() % 5 < 2) {
                // 複数バイトの値をまとめて書く (1 / 2 / 4 / 8 バイト。大きな数、符号の境目、桁の境目、乱数)。
                // 場所は、区切りのあと 6000 バイトまでか、KEYB の手前 12000 バイト (パレット、MFP など)
                static const uint64_t kValues[] = {0xffffffffffffffffull, 0x7fffffffull, 0x80000000ull, 0x10000ull, 0x100ull};
                const size_t width = (size_t)1 << (rng() % 4);
                const uint32_t pick = rng() % 6;
                const uint64_t value = pick < 5 ? kValues[pick] : (uint64_t)rng() << 32 | rng();
                size_t pos;
                if (rng() % 3 && !tagAt.empty()) pos = tagAt[rng() % tagAt.size()] + 4 + rng() % 6000;
                else if (keyb > 12000 + x68k::STATE_HEADER_SIZE) pos = keyb - 1 - rng() % 12000;
                else pos = x68k::STATE_HEADER_SIZE + rng() % (good.size - x68k::STATE_HEADER_SIZE);
                if (pos + width > good.size) pos = good.size - width;
                if (verbose) printf("  round %d: [%zu] %zu bytes <- %llx\n", round, pos, width, (unsigned long long)value);
                for (size_t i = 0; i < width; i++) bad[pos + i] = (uint8_t)(value >> (8 * i));
                continue;
            }
            size_t pos;
            const uint32_t where = rng() % 10;
            if (where < 6 && !tagAt.empty()) {
                pos = tagAt[rng() % tagAt.size()] + 4 + rng() % 400;
            } else if (where < 8 && keyb > 6000) {
                pos = keyb - 1 - rng() % 6000;
            } else {
                pos = x68k::STATE_HEADER_SIZE + rng() % (good.size - x68k::STATE_HEADER_SIZE);
            }
            if (pos >= good.size) pos = good.size - 1;
            uint8_t v;
            switch (rng() % 5) {
            case 0: v = (uint8_t)rng(); break;
            case 1: v = 0xff; break;
            case 2: v = (uint8_t)(bad[pos] ^ (1u << (rng() % 8))); break;
            case 3: v = 0x80; break;
            default: v = (uint8_t)(bad[pos] + 1); break;
            }
            if (verbose) printf("  round %d: [%zu] %02x -> %02x\n", round, pos, bad[pos], v);
            bad[pos] = v;
        }
        if (verbose) fflush(stdout);
        g_round = round;
        alarm(60); // 読んだあとに動かしたとき、終わらなくなっていないか
        fixHeader(bad.data(), bad.size());
        disks.give(dst);
        Blob before;
        save(*dst.box.m, before);
        const char* err = nullptr;
        if (x68k::loadState(*dst.box.m, bad.data(), bad.size(), err)) {
            loaded++;
            // 読めたステートを動かすのにかかる時間に上限を置く (範囲の外の値で、周辺機器を進める繰り返しが
            // 長くなっていないか)。ふつうは 3 フレームで数ミリ秒 (ASan のビルドで 1 秒以下)
            const clock_t t0 = clock();
            exercise(dst, midi, board);
            const double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
            if (secs > 5.0) {
                printf("  round %d: running the loaded state took %.1f s\n", round, secs);
                failures++;
            }
            if (fpu) {
                x68k::FpuBoard& f = *dst.box.m->fpuBoard();
                for (int i = 0; i < 8; i++) f.write16(0x10, (uint16_t)(0x4000 + i));
                for (int i = 0; i < 8; i++) f.read16(0x10);
                f.read16(0x14);
                f.read16(0x04);
                for (int i = 0; i < 8; i++) f.read16(0x10);
                f.write16(0x0a, 0x4000);
            }
            if (mercury) {
                for (uint32_t r : {0x90u, 0x91u, 0xa1u}) dst.in(0xecc000 + r);
                dst.out(0xecc080, 0x12);
                dst.out(0xecc081, 0x34);
                dst.box.frames(1);
            }
        } else {
            rejected++;
            Blob after;
            save(*dst.box.m, after);
            if (!same(before, after)) {
                printf("  round %d: machine changed by a rejected load (%s)\n", round, err ? err : "?");
                reportDiff(before, after);
                failures++;
            }
        }
    }
    alarm(0);
    // 最後に、書き換えていないものを読めば、もとの続きと同じに動く
    disks.give(dst);
    const char* err = nullptr;
    CHECK_EQ(x68k::loadState(*dst.box.m, good.data, good.size, err), true);
    src.box.frames(3);
    dst.box.frames(3);
    CHECK_EQ(dst.box.checksum(), src.box.checksum());
    Blob sa, sb;
    save(*src.box.m, sa);
    save(*dst.box.m, sb);
    if (!same(sa, sb)) {
        printf("  states differ after loading the unmodified state\n");
        reportDiff(sa, sb);
        failures++;
    }
    printf("%s: %d rounds, %d loaded, %d rejected\n", name, rounds, loaded, rejected);
}

void corruptedAll() {
    auto cpuLoop = [](Env& e) {
        CHECK_EQ(scene::jumpTo(*e.box.m, scene::kCpuLoop, sizeof scene::kCpuLoop / 2), true);
        e.box.frames(1);
    };
    x68k::Config c68000;
    corrupted("corrupted state (68000, MIDI board, SCSI board)", c68000, true, true, cpuLoop, 20261113);
    // 外付け SCSI ボードに CD-ROM 装置 (READ TOC の応答を読んでいる途中)
    corrupted("corrupted state (68000, SCSI board with CD-ROM)", c68000, false, true, cpuLoop, 4390, false, false, true);
    // 数値演算プロセッサボード (オペランドを待っている途中) とまーきゅりーゆにっと (FIFO にデータがある)
    corrupted("corrupted state (68000, FPU board, Mercury unit)", c68000, false, false,
              [&](Env& e) {
                  cpuLoop(e);
                  e.box.m->fpuBoard()->write16(0x0a, 0x4000);
                  e.box.m->fpuBoard()->write16(0x10, 0x0001);
                  e.out(0xecc091, 0xbe); // 等倍、48kHz、L と R、ステレオ
                  for (uint32_t i = 0; i < 6; i++) { // PCM のデータを 3 語
                      e.out(0xecc080, (uint8_t)(0x10 + i));
                      e.out(0xecc081, (uint8_t)(0x80 + i));
                  }
              },
              6881, true, true);
#ifndef PIX68K_NO_CPU020
    x68k::Config c030;
    c030.setCpu(68030);
    c030.highMemSize = 1u << 20;
    corrupted("corrupted state (68030)", c030, false, false, cpuLoop, 68030);
    x68k::Config c060;
    c060.setCpu(68060);
    c060.localMemSize = 1u << 20;
    corrupted("corrupted state (68060, MMU enabled)", c060, false, false,
              [](Env& e) {
                  startMmu060(*e.box.m);
                  e.box.frames(2);
              },
              68060);
#endif
}

// 読めないステートを断ること (rejects() の続き): 途中で切れたファイル、ヘッダの大きさの違い、中身の大きさの違い、
// 拡張ボードやメモリの大きさの違い、CRC は合っているが並びや値が合わないもの。断ったあとの Machine は、
// 読もうとしなかった Machine と同じ保存になり、同じように動き続ける
void rejectsMore() {
    printf("rejects (truncated files, sizes, layout, values out of range)\n");
    static Rom rom;
    x68k::Config config;
    auto start = [&](Box& x) {
        x.m->setIplRom(rom.b, sizeof rom.b);
        x.m->powerOn();
        x.frames(2);
        startScene(x, true);
        x.frames(5);
    };
    Box a(config), twin(config);
    start(a);
    start(twin);
    Blob good;
    save(*a.m, good);
    Blob before;
    save(*a.m, before);
    std::vector<uint8_t> bad(good.size + 8);
    auto fresh = [&]() { memcpy(bad.data(), good.data, good.size); };
    auto expectReject = [&](const char* what, size_t size, const char* want) {
        const char* err = nullptr;
        // ちょうどの大きさのバッファに写して渡す (範囲の外を読めば ASan が見つける)
        std::vector<uint8_t> exact(bad.begin(), bad.begin() + (long)size);
        const bool ok = x68k::loadState(*a.m, exact.data(), size, err);
        if (ok || !err || strcmp(err, want) != 0) {
            printf("  %s: loaded=%d error=%s (want %s)\n", what, ok, err ? err : "(none)", want);
            failures++;
        }
        Blob after;
        save(*a.m, after);
        if (!same(before, after)) {
            printf("  %s: machine changed by a rejected load\n", what);
            reportDiff(before, after);
            failures++;
        }
    };
    // 途中で切れたファイル
    fresh();
    for (size_t n : {(size_t)0, (size_t)1, (size_t)8, (size_t)63}) expectReject("cut in the header", n, "not a pix68k state file");
    for (size_t n : {(size_t)64, (size_t)65, (size_t)4096, good.size / 2, good.size - 4, good.size - 1})
        expectReject("cut in the payload", n, "state file is truncated");
    expectReject("longer than the header says", good.size + 1, "state file is truncated");
    // ヘッダ
    fresh();
    bad[8] = 1;
    expectReject("version 1", good.size, "unsupported state version");
    fresh();
    bad[11] = 0x80;
    expectReject("huge version", good.size, "unsupported state version");
    fresh();
    bad[12] = 68;
    expectReject("header size", good.size, "unsupported state version");
    fresh();
    bad[48] ^= 1;
    expectReject("payload size", good.size, "state file is truncated");
    fresh();
    bad[52] = 1; // 中身の大きさの上位 32 ビット
    expectReject("payload size (high word)", good.size, "state file is truncated");
    fresh();
    bad[55] = 0xff;
    expectReject("payload size (top byte)", good.size, "state file is truncated");
    fresh();
    bad[56] ^= 0x10;
    expectReject("crc field", good.size, "state file is broken (CRC mismatch)");
    fresh();
    bad[60] = 1; // 数値演算プロセッサボードがあることにする
    expectReject("boards", good.size, "expansion boards differ");
    fresh();
    bad[24] = 0x10; // ハイメモリがあることにする
    expectReject("high memory", good.size, "high memory size differs");
    fresh();
    bad[30] = 0x10; // ローカルメモリ
    expectReject("local memory", good.size, "local memory size differs");
    fresh();
    bad[40] = 1; // 内蔵 SCSI の ROM
    expectReject("SCSI ROM", good.size, "built-in SCSI ROM differs");
    // CRC は合っているが、並びが合わない
    const char* const kLayout = "state file layout does not match";
    fresh();
    bad[x68k::STATE_HEADER_SIZE] ^= 1; // 最初の区切りの値
    fixHeader(bad.data(), good.size);
    expectReject("first tag", good.size, kLayout);
    const size_t mem = findTag(good, "MEM ");
    CHECK_EQ(mem != 0, true);
    fresh();
    bad[mem + 4] = 2; // RAM の最初のページの有無 (0 か 1)
    fixHeader(bad.data(), good.size);
    expectReject("page flag", good.size, kLayout);
    fresh();
    bad[mem + 4] ^= 1; // ページの有無を逆にすると、そのあとの並びがずれる
    fixHeader(bad.data(), good.size);
    expectReject("page flag flipped", good.size, kLayout);
    fresh();
    fixHeader(bad.data(), good.size - 4); // 最後の区切りがない
    expectReject("no end tag", good.size - 4, kLayout);
    fresh();
    fixHeader(bad.data(), good.size + 4); // 終わりのあとに余り
    expectReject("bytes after the end", good.size + 4, kLayout);
    fresh();
    fixHeader(bad.data(), x68k::STATE_HEADER_SIZE); // 中身が空
    expectReject("empty payload", x68k::STATE_HEADER_SIZE, kLayout);
    // CRC も並びも合っているが、値が範囲の外
    const size_t keyb = findTag(good, "KEYB"), fdc = findTag(good, "FDC "), sasi = findTag(good, "SASI"),
                 time = findTag(good, "TIME");
    CHECK_EQ(keyb && fdc && sasi && time, true);
    fresh();
    bad[keyb + 4 + 64] = 64; // キーボードのキューの書き込み位置 (0-63)
    fixHeader(bad.data(), good.size);
    expectReject("keyboard queue index", good.size, kLayout);
    fresh();
    bad[fdc + 4 + 2 + 4] = 5; // FDC のフェーズ (0-4)
    fixHeader(bad.data(), good.size);
    expectReject("FDC phase", good.size, kLayout);
    fresh();
    bad[sasi + 4] = 8; // SASI のフェーズ (0-7)
    fixHeader(bad.data(), good.size);
    expectReject("SASI phase", good.size, kLayout);
    fresh();
    bad[sasi + 4 + 4 + 6] = 7; // SASI のコマンドのバイト数 (0-6)。区切り 4、段階 / 割り込み / データの相手 / ID の 4、コマンド 6 の次
    fixHeader(bad.data(), good.size);
    expectReject("SASI command length", good.size, kLayout);
    fresh();
    bad[time + 4 + 8 + 7] = 0x40; // MFP のクロックへの換算の端数 (64 ビットの最上位バイト)
    fixHeader(bad.data(), good.size);
    expectReject("clock fraction", good.size, kLayout);
    // 古い版の並びで書いたものは読める (版 6、7 と、CD-ROM 装置の状態が入った版 8)
    bad.resize(good.size + 4096); // 版 11 までの並びは、SPC の状態が今より大きい
    for (uint32_t version : {6u, 7u, 8u}) {
        size_t n = 0;
        CHECK_EQ(x68k::saveState(*a.m, bad.data(), bad.size(), n, version), true);
        const char* err = nullptr;
        CHECK_EQ(x68k::loadState(*a.m, bad.data(), n, err), true);
        Blob after;
        save(*a.m, after);
        CHECK_EQ(same(before, after), true);
    }
    // 断られたあとも、読もうとしなかった Machine と同じに動く
    a.frames(20);
    twin.frames(20);
    CHECK_EQ(a.checksum(), twin.checksum());
    Blob sa, st;
    save(*a.m, sa);
    save(*twin.m, st);
    if (!same(sa, st)) {
        printf("  the machine runs differently after rejected loads\n");
        reportDiff(st, sa);
        failures++;
    }
#ifndef PIX68K_NO_CPU020
    // メモリの大きさが違う Machine には読まない (ハイメモリ、ローカルメモリ)
    auto sizes = [&](int cpu, uint32_t high, uint32_t local, uint32_t high2, uint32_t local2, const char* want) {
        x68k::Config c1, c2;
        c1.setCpu(cpu);
        c1.highMemSize = high;
        c1.localMemSize = local;
        c2.setCpu(cpu);
        c2.highMemSize = high2;
        c2.localMemSize = local2;
        Box x(c1), y(c2);
        x.m->setIplRom(rom.b, sizeof rom.b);
        x.m->powerOn();
        x.frames(2);
        y.m->setIplRom(rom.b, sizeof rom.b);
        y.m->powerOn();
        y.frames(2);
        Blob s, y0, y1;
        save(*x.m, s);
        save(*y.m, y0);
        const char* err = nullptr;
        CHECK_EQ(x68k::loadState(*y.m, s.data, s.size, err), false);
        CHECK_EQ(err && !strcmp(err, want), true);
        save(*y.m, y1);
        CHECK_EQ(same(y0, y1), true);
    };
    sizes(68030, 2u << 20, 0, 1u << 20, 0, "high memory size differs");
    sizes(68030, 0, 0, 1u << 20, 0, "high memory size differs");
    sizes(68060, 0, 2u << 20, 0, 1u << 20, "local memory size differs");
    sizes(68060, 0, 2u << 20, 0, 0, "local memory size differs");
#endif
}

uint8_t* readFile(const char* path, size_t& size) {
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

// 本物の IPL ROM で Human68k を起動し、フロッピーディスクを読んでいる途中で保存する
// 保存に、68060 の MMU が有効な間の TLB と ATC が入っているか
bool hasTlb(const Blob& b) {
    const size_t at = findTag(b, "TLB ");
    return at && at + 4 < b.size && b.data[at + 4] == 1;
}

// 乱数の試験では当たらなかった、決め打ちの壊れたステート (レビューで見つかったもの)。
// どれも、直す前は ASan / UBSan のビルドで止まる
void craftedStates() {
    printf("crafted states (SPC not connected, mouse, FPU board, uninitialized ATC)\n");
    x68k::Config config;
    auto get32 = [](const uint8_t* p) {
        return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    };
    const char* const kLayout = "state file layout does not match";
    // 書き換えたものを読ませる。want が nullptr なら読めること、そうでなければその理由で断ること
    auto tryLoad = [&](const char* what, Env& e, const std::vector<uint8_t>& data, const char* want) {
        std::vector<uint8_t> bad(data);
        fixHeader(bad.data(), bad.size());
        const char* err = nullptr;
        const bool ok = x68k::loadState(*e.box.m, bad.data(), bad.size(), err);
        if (ok != (want == nullptr) || (want && (!err || strcmp(err, want) != 0))) {
            printf("  %s: loaded=%d error=%s\n", what, ok, err ? err : "(none)");
            failures++;
        }
        return ok;
    };
    // SPC の部分の並び (x68k/state.cpp の Spc::state()): 区切り 4、BDID から TEMP の 7、FIFO の数 1、あとで出す割り込み 1、
    // FIFO 8、転送カウンタ 4、結合中 / ハード転送 / FIFO の向き / パディング / ACK / ACK が有効 / ATN / SEL (1 ずつ)、
    // ターゲット 1、フェーズ 1、論理ユニット 1、コマンド 12、コマンドのバイト数 1、… (8 バイト)、データの長さ 4、位置 4
    const size_t kConnected = 25, kAck = 29, kAckValid = 30, kPhase = 34, kCdbLen = 48, kLen = 58, kPos = 62;
    // 外付け SCSI ボードの SPC: 結合していないのに、コマンドのフェーズで ACK を返して最後のバイトを受け渡し終えた、
    // という状態。読んだあと Reset ACK を書いても、つながっていない装置のコマンドを実行しない
    {
        using x68k::Spc;
        Env e(config, false, true);
        {
            Spc& s = e.box.m->spcEx();
            s.write(Spc::SCTL, 0x80);
            s.write(Spc::BDID, 7);
            s.write(Spc::SCTL, 0x11); // リセットを解く (解かないと SCMD を受け付けない)
            s.write(Spc::INTS, 0xff);
        }
        Blob good;
        save(*e.box.m, good);
        size_t spc = findTag(good, "SPC ");
        spc = findTag(good, "SPC ", spc + 4); // 2 つ目が外付けのボード
        CHECK_EQ(spc != 0, true);
        CHECK_EQ(good.data[spc + kConnected], 0);
        std::vector<uint8_t> bad(good.data, good.data + good.size);
        bad[spc + kPhase] = Spc::PH_COMMAND;
        bad[spc + kAck] = 1;
        bad[spc + kAckValid] = 1;
        CHECK_EQ(tryLoad("SPC not connected with ACK", e, bad, nullptr), true);
        Spc& s = e.box.m->spcEx();
        s.write(Spc::SCMD, Spc::CMD_RESET_ACK);
        s.write(Spc::SCMD, Spc::CMD_SET_ACK);
        s.write(Spc::SCMD, Spc::CMD_RESET_ACK);
        CHECK_EQ(s.read(Spc::PSNS) & (Spc::PSNS_REQ | Spc::PSNS_BSY | Spc::PSNS_ACK), 0);
        e.box.frames(1);
    }
    // SPC: コマンドを受け取っている途中で、受け取ったバイト数がコマンドの配列 (12 バイト) の終わり。
    // 読むが、続きのバイトを配列の外に書かない。配列より大きい数は読まない
    {
        using x68k::Spc;
        Env e(config, false, true);
        Spc& s = e.box.m->spcEx();
        spcTakeId7(s); // リセットの状態にして、自分の ID を 7 にする (続けて SCTL でリセットを解く)
        s.write(Spc::SCTL, 0x11);
        s.write(Spc::INTS, 0xff);
        s.write(Spc::PCTL, 0);
        s.write(Spc::TEMP, 0x81);
        spcTc(s, 0x2000);
        s.write(Spc::SCMD, Spc::CMD_SELECT);
        s.write(Spc::INTS, 0xff);
        CHECK_EQ(s.read(Spc::PSNS) & 7, Spc::PH_COMMAND);
        Blob good;
        save(*e.box.m, good);
        size_t spc = findTag(good, "SPC ");
        spc = findTag(good, "SPC ", spc + 4);
        CHECK_EQ(spc != 0, true);
        CHECK_EQ(good.data[spc + kConnected], 1);
        CHECK_EQ(good.data[spc + kPhase], Spc::PH_COMMAND);
        CHECK_EQ(good.data[spc + kCdbLen], 0);
        for (uint8_t len : {(uint8_t)12, (uint8_t)11, (uint8_t)5}) {
            std::vector<uint8_t> bad(good.data, good.data + good.size);
            bad[spc + kCdbLen] = len;
            CHECK_EQ(tryLoad("SPC command length", e, bad, nullptr), true);
            Spc& t = e.box.m->spcEx();
            t.write(Spc::PCTL, Spc::PH_COMMAND);
            spcTc(t, 12);
            t.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
            for (int i = 0; i < 12; i++) t.write(Spc::DREG, 0xa8);
        }
        std::vector<uint8_t> over(good.data, good.data + good.size);
        over[spc + kCdbLen] = 13;
        tryLoad("SPC command length over the array", e, over, kLayout);
        std::vector<uint8_t> same(good.data, good.data + good.size); // 書き換えなければ、コマンドの続きを受け取る
        CHECK_EQ(tryLoad("SPC command length (unchanged)", e, same, nullptr), true);
        CHECK_EQ(e.box.m->spcEx().read(Spc::PSNS) & 7, Spc::PH_COMMAND);
    }
    // CD-ROM 装置の中のバッファ (READ TOC の応答) の転送の途中で、データの長さが応答のバッファ (1024 バイト) に
    // 収まらなければ読まない。位置が長さ以上なら、読むが、コマンドを捨ててバスフリーにする
    {
        using x68k::Spc;
        Env e(config, false, true, false, false, true);
        makeBusy(e, false, true, true);
        CHECK_EQ(e.box.m->spcEx().read(Spc::PSNS) & (Spc::PSNS_BSY | 7), Spc::PSNS_BSY | Spc::PH_DATA_IN);
        Blob good;
        save(*e.box.m, good);
        size_t spc = findTag(good, "SPC ");
        spc = findTag(good, "SPC ", spc + 4);
        const size_t cdTag = findTag(good, "CD  ");
        CHECK_EQ(spc != 0 && cdTag > spc, true);
        const uint32_t len = get32(good.data + spc + kLen);
        CHECK_EQ(len > 0 && len <= 1024, true);
        for (uint32_t v : {1025u, 0x7fffffffu, 0xfffffff0u}) {
            std::vector<uint8_t> bad(good.data, good.data + good.size);
            put32le(bad.data() + spc + kLen, v);
            tryLoad("CD-ROM response length", e, bad, kLayout);
        }
        for (uint32_t v : {len, len + 1, 1024u, 0x7fffffffu, 0xfffffff0u}) {
            std::vector<uint8_t> bad(good.data, good.data + good.size);
            put32le(bad.data() + spc + kPos, v);
            CHECK_EQ(tryLoad("CD-ROM response position", e, bad, nullptr), true);
            Spc& t = e.box.m->spcEx();
            CHECK_EQ(t.read(Spc::PSNS) & Spc::PSNS_BSY, 0);
            static uint8_t buf[64];
            spcRead(t, buf, sizeof buf);
        }
        std::vector<uint8_t> same(good.data, good.data + good.size); // 書き換えなければ続きを読める
        CHECK_EQ(tryLoad("CD-ROM response buffer (unchanged)", e, same, nullptr), true);
        CHECK_EQ(e.box.m->spcEx().read(Spc::PSNS) & (Spc::PSNS_BSY | 7), Spc::PSNS_BSY | Spc::PH_DATA_IN);
        // 再生のクロックの端数 (40000 で割った余り): CD-ROM 装置の部分 (CdDrive::state()) は、区切り 4、
        // トレイ / 禁止 / 知らせ / 再生の状態 / エラー 2 つ (6)、位置 4、終わり 4、フレーム 4、端数 4、応答 1024
        const size_t acc = cdTag + 4 + 6 + 4 + 4 + 4;
        std::vector<uint8_t> bad(good.data, good.data + good.size);
        put32le(bad.data() + acc, 40000);
        tryLoad("CD-ROM clock fraction", e, bad, kLayout);
        put32le(bad.data() + acc, 39999);
        CHECK_EQ(tryLoad("CD-ROM clock fraction at the limit", e, bad, nullptr), true);
    }
    // MFP のプリスケーラの残りが大きい (タイマを進める繰り返しが、その数だけ回る)。音のフィルタの補間の前の値が
    // 32 ビットの下限 (次のサンプルとの引き算があふれる)
    {
        Env e(config, false, false);
        Blob good;
        save(*e.box.m, good);
        const size_t keyb = findTag(good, "KEYB"), scc = findTag(good, "SCC ");
        CHECK_EQ(keyb && scc, true);
        // MFP は KEYB の直前の 56 バイト。+32 からプリスケーラの残り 4 つ、最後の 4 バイトは送信データ (なければ -1)
        CHECK_EQ(sizeof(x68k::Mfp), 56);
        CHECK_EQ(get32(good.data + keyb - 4), 0xffffffffu);
        for (uint32_t v : {0x7ffffff0u, 0xffffffffu, 256u}) {
            for (int t = 0; t < 4; t++) {
                std::vector<uint8_t> bad(good.data, good.data + good.size);
                put32le(bad.data() + keyb - 24 + (size_t)t * 4, v);
                tryLoad("MFP prescaler", e, bad, kLayout);
            }
        }
        std::vector<uint8_t> edge(good.data, good.data + good.size);
        put32le(edge.data() + keyb - 24, 199);
        CHECK_EQ(tryLoad("MFP prescaler at the limit", e, edge, nullptr), true);
        e.box.frames(1);
        // 音の部分の終わり (SCC の手前は DMAC と、DMA の途中か (1)) の 8 バイトが、補間の前のサンプル (左、右)
        const size_t prev = scc - 1 - sizeof(x68k::Dmac) - 8;
        for (int c = 0; c < 2; c++) {
            for (uint32_t v : {0x80000000u, 0x7fffffffu, 32768u, 0u - 32769u}) {
                std::vector<uint8_t> bad(good.data, good.data + good.size);
                put32le(bad.data() + prev + (size_t)c * 4, v);
                tryLoad("sound interpolation sample", e, bad, kLayout);
            }
        }
        std::vector<uint8_t> ok(good.data, good.data + good.size);
        put32le(ok.data() + prev, 0u - 32768u);
        put32le(ok.data() + prev + 4, 32767);
        CHECK_EQ(tryLoad("sound interpolation sample at the limit", e, ok, nullptr), true);
        e.box.frames(1);
    }
#ifndef PIX68K_NO_CPU020
    // 68030 の SFC / DFC (0 から 7)
    {
        x68k::Config c030;
        c030.setCpu(68030);
        Env e(c030, false, false);
        Blob good;
        save(*e.box.m, good);
        const size_t cpu = findTag(good, "0306");
        CHECK_EQ(cpu != 0, true);
        // +112 トレース、+116 割り込みのマスク、+120 VBR、+124 SFC、+128 DFC
        CHECK_EQ(get32(good.data + cpu + 116), 0u); // テスト用の ROM がマスクを 0 にしている
        for (size_t at : {(size_t)124, (size_t)128}) {
            for (uint32_t v : {8u, 0x80000000u, 0xffffffffu}) {
                std::vector<uint8_t> bad(good.data, good.data + good.size);
                put32le(bad.data() + cpu + at, v);
                tryLoad("SFC / DFC", e, bad, kLayout);
            }
            std::vector<uint8_t> ok(good.data, good.data + good.size);
            put32le(ok.data() + cpu + at, 7);
            CHECK_EQ(tryLoad("SFC / DFC at the limit", e, ok, nullptr), true);
        }
        e.box.frames(1);
    }
#endif
    // 値の範囲の端: 範囲の外は断り、端の値は読める (OPM のタイマと BUSY の残り、ADPCM の波形の値と量子化幅の段、音のフィルタ、
    // 高速クリアの残りフレーム数、まーきゅりーゆにっとの出力)
    {
        Env e(config, false, false, false, true);
        // OPM のタイマ A を動かす (残りは 1024 - 962 = 62 から減っていく)
        e.out(0xe90001, 0x10);
        e.out(0xe90003, 0xf0);
        e.run(200);
        e.out(0xe90001, 0x11);
        e.out(0xe90003, 0x02);
        e.run(200);
        e.out(0xe90001, 0x14);
        e.out(0xe90003, 0x01);
        Blob good;
        save(*e.box.m, good);
        const size_t snd = findTag(good, "SND "), scc = findTag(good, "SCC "), vid = findTag(good, "VID\0"),
                     merc = findTag(good, "MERC");
        CHECK_EQ(snd && scc && vid && merc, true);
        struct Case {
            const char* what;
            size_t at;      // ファイルの中の位置
            int width;      // 1、4、8 バイト
            uint64_t value;
            bool loads;
        };
        // OPM: 区切りのあとにレジスタ 256、番地、状態、(詰め物)、+260 BUSY の残り、+264 タイマ A、+268 タイマ B、
        // +272 タイマ A が動いている、+273 タイマ B が動いている
        const size_t opm = snd + 4;
        CHECK_EQ(good.data[opm + 272], 1);
        CHECK_EQ(get32(good.data + opm + 264) >= 1 && get32(good.data + opm + 264) <= 62, true);
        CHECK_EQ(good.data[opm + 273], 0);
        // 音の部分の終わり (SCC の手前は DMAC と、DMA の途中か (1)) から逆に: 補間の前のサンプル 8、直流を切るフィルタの
        // 前の入力 8、その出力 16、低域通過フィルタ 2 段 8 + 8、サンプルの中のクロック 4、その前が ADPCM
        // (32 バイト: bool 5、左右、周波数、バイト 2 つ、+9 波形の値、+13 量子化幅の段、+17 時間の端数、+21 から 0 が 11)
        const size_t prev = scc - 1 - sizeof(x68k::Dmac) - 8;
        const size_t hpIn = prev - 8, hpOut = hpIn - 16, lp2 = hpOut - 8, lp1 = lp2 - 8, adpcm = lp1 - 4 - 32;
        CHECK_EQ(adpcm, opm + sizeof(x68k::Opm));
        // 画面: 区切りのあとに CRTC 48、動作ポート 1、高速クリアの予約 1、+50 残りフレーム数
        const size_t clear = vid + 4 + 50;
        // まーきゅりーゆにっと: 区切りのあとの +32 と +36 が左右の出力
        CHECK_EQ(sizeof(x68k::Mercury), 40);
        const size_t outL = merc + 4 + 32, outR = merc + 4 + 36;
        const uint64_t m1 = 0xffffffffull; // 32 ビットの -1
        for (const Case& c : {
                 Case{"OPM timer A", opm + 264, 4, 1025, false}, Case{"OPM timer A", opm + 264, 4, 1024, true},
                 Case{"OPM timer A running at 0", opm + 264, 4, 0, false},
                 Case{"OPM timer B", opm + 268, 4, 4097, false}, Case{"OPM timer B", opm + 268, 4, 4096, true},
                 Case{"OPM busy", opm + 260, 4, 65, false}, Case{"OPM busy", opm + 260, 4, 64, true},
                 Case{"ADPCM signal", adpcm + 9, 4, 2048, false}, Case{"ADPCM signal", adpcm + 9, 4, m1 - 2048, false},
                 Case{"ADPCM signal", adpcm + 9, 4, 2047, true}, Case{"ADPCM signal", adpcm + 9, 4, m1 - 2047, true},
                 Case{"ADPCM step", adpcm + 13, 4, 49, false}, Case{"ADPCM step", adpcm + 13, 4, m1, false},
                 Case{"ADPCM step", adpcm + 13, 4, 48, true},
                 Case{"ADPCM time", adpcm + 17, 4, 2048, false}, Case{"ADPCM time", adpcm + 17, 4, 2047, true},
                 // 左右 (+5) と分周の選択 (+6) は 0..3 (分周は表の添字になる)
                 Case{"ADPCM pan", adpcm + 5, 1, 4, false}, Case{"ADPCM pan", adpcm + 5, 1, 0xff, false},
                 Case{"ADPCM pan", adpcm + 5, 1, 3, true},
                 Case{"ADPCM rate", adpcm + 6, 1, 4, false}, Case{"ADPCM rate", adpcm + 6, 1, 0xff, false},
                 Case{"ADPCM rate", adpcm + 6, 1, 3, true},
                 Case{"low-pass 1", lp1, 4, (1u << 22) + 1, false}, Case{"low-pass 1", lp1 + 4, 4, m1 - (1u << 22), false},
                 Case{"low-pass 1", lp1, 4, 1u << 22, true},
                 Case{"low-pass 2", lp2 + 4, 4, (1u << 22) + 1, false}, Case{"low-pass 2", lp2, 4, m1 - (1u << 22) + 1, true},
                 Case{"high-pass output", hpOut, 8, (1ull << 41) + 1, false},
                 Case{"high-pass output", hpOut + 8, 8, 0ull - (1ull << 41) - 1, false},
                 Case{"high-pass output", hpOut, 8, 1ull << 41, true}, Case{"high-pass output", hpOut + 8, 8, 0ull - (1ull << 41), true},
                 Case{"high-pass input", hpIn, 4, (1u << 20) + 1, false}, Case{"high-pass input", hpIn + 4, 4, 1u << 20, true},
                 Case{"clear frames", clear, 4, 3, false}, Case{"clear frames", clear, 4, m1, false},
                 Case{"clear frames", clear, 4, 2, true},
                 Case{"Mercury output", outL, 4, 32768, false}, Case{"Mercury output", outR, 4, m1 - 32768, false},
                 Case{"Mercury output", outL, 4, 32767, true}, Case{"Mercury output", outR, 4, m1 - 32767, true}}) {
            std::vector<uint8_t> bad(good.data, good.data + good.size);
            for (int i = 0; i < c.width; i++) bad[c.at + (size_t)i] = (uint8_t)(c.value >> (8 * i));
            if (tryLoad(c.what, e, bad, c.loads ? nullptr : kLayout)) e.box.frames(1);
        }
    }
#ifndef PIX68K_NO_CPU020
    // 68030 / 68060 の PC が奇数 (命令の語を偶数の前提で 2 バイトずつ読むので、ページの終わりで 1 バイト外を読む)
    for (int cpu : {68030, 68060}) {
        x68k::Config c;
        c.setCpu(cpu);
        Env e(c, false, false);
        Blob good;
        save(*e.box.m, good);
        const size_t at = findTag(good, "0306");
        CHECK_EQ(at != 0, true);
        const size_t pc = at + 4 + 32 + 32 + 12; // D0-D7、A0-A7、USP / ISP / MSP のあと
        CHECK_EQ(get32(good.data + pc) & 0xfffffff1u, 0x00fe0100u & 0xfffffff1u); // テスト用の ROM のループの中
        for (uint32_t v : {0xffffffffu, 0x00ffffffu, get32(good.data + pc) | 1, 1u}) {
            std::vector<uint8_t> bad(good.data, good.data + good.size);
            put32le(bad.data() + pc, v);
            tryLoad("odd PC", e, bad, kLayout);
        }
        std::vector<uint8_t> ok(good.data, good.data + good.size); // 偶数なら、ROM の最後の語でも読める
        put32le(ok.data() + pc, 0x00fffffeu);
        CHECK_EQ(tryLoad("PC at the last word of the ROM", e, ok, nullptr), true);
        e.box.frames(1);
    }
#endif
    // 68000 は、命令の語を読むたびに番地が偶数かを確かめるので、PC が奇数のステートを読んでもアドレスエラーの例外になる
    {
        Env e(config, false, false);
        Blob good;
        save(*e.box.m, good);
        const size_t at = findTag(good, "0006");
        CHECK_EQ(at != 0, true);
        const size_t pc = at + 4 + 32 + 32 + 4; // D0-D7、A0-A7、使っていないほうの SP のあと
        CHECK_EQ(get32(good.data + pc) & 0xffffff00u, 0x00fe0100u);
        for (uint32_t v : {0x00ffffffu, 0xffffffffu, 0x00003fffu}) {
            std::vector<uint8_t> bad(good.data, good.data + good.size);
            put32le(bad.data() + pc, v);
            CHECK_EQ(tryLoad("odd PC on 68000", e, bad, nullptr), true);
            e.box.frames(2);
        }
    }
    // マウスの移動量が範囲の外 (次に移動量を足すとあふれる)
    {
        Env e(config, false, false);
        e.box.m->input().onMouse(5, -7, 0);
        Blob good;
        save(*e.box.m, good);
        const size_t mouse = findTag(good, "MOUS");
        CHECK_EQ(mouse != 0, true);
        CHECK_EQ(get32(good.data + mouse + 4), 5u); // dx
        for (int which = 0; which < 2; which++) {
            for (uint32_t v : {0x7fffffffu, 0x80000000u, (1u << 20) + 1, 0u - (1u << 20) - 1}) {
                std::vector<uint8_t> bad(good.data, good.data + good.size);
                put32le(bad.data() + mouse + 4 + (size_t)which * 4, v);
                tryLoad("mouse movement", e, bad, kLayout);
            }
        }
        std::vector<uint8_t> edge(good.data, good.data + good.size); // 範囲の端は読める
        put32le(edge.data() + mouse + 4, 1u << 20);
        put32le(edge.data() + mouse + 8, 0u - (1u << 20));
        CHECK_EQ(tryLoad("mouse movement at the limit", e, edge, nullptr), true);
        e.box.m->input().onMouse(1, -1, 0);
        e.box.frames(1);
    }
#ifndef PIX68K_NO_CPU020
    // 68060 の ATC の使っていない項目は、物理ページと書き込み可の値が決まっていない。保存と、読むときの確かめで
    // それを読まないこと。Cpu020 を確保する前に、同じ大きさの領域を $A5 で埋めて解放しておく (同じ場所が使われやすい)。
    // ASan のビルドでは解放した場所をすぐには使い回さないので、ASAN_OPTIONS=max_malloc_fill_size=100000000 を付けて
    // 動かす (確保した領域が $BE で埋まる)
    {
        for (int i = 0; i < 4; i++) {
            void* p = malloc(sizeof(m68k::Cpu020));
            if (p) memset(p, 0xa5, sizeof(m68k::Cpu020));
            free(p);
        }
        x68k::Config c060;
        c060.setCpu(68060);
        Env a(c060, false, false), b(c060, false, false);
        startMmu060(*a.box.m);
        startMmu060(*b.box.m);
        a.box.frames(3);
        b.box.frames(1);
        Blob saved;
        save(*a.box.m, saved);
        CHECK_EQ(hasTlb(saved), true);
        const char* err = nullptr;
        CHECK_EQ(x68k::loadState(*b.box.m, saved.data, saved.size, err), true);
        Blob again;
        save(*b.box.m, again);
        CHECK_EQ(same(saved, again), true);
    }
#endif
    // 数値演算プロセッサボード: オペランドを受け取る途中で、長さが作業用の配列 (24 語) を超えている
    {
        Env e(config, false, false, true, true);
        x68k::FpuBoard& f = *e.box.m->fpuBoard();
        f.write16(0x0a, 0x4000); // FMOVE.L <ea>,FP0: 4 バイトのオペランドを待つ
        Blob good;
        save(*e.box.m, good);
        const size_t at = findTag(good, "6881");
        CHECK_EQ(at != 0, true);
        // +179 フェーズ、+180 受け取ったオペランドの用途、+186 長さ、+190 位置
        CHECK_EQ(good.data[at + 179], 1); // P_IN
        CHECK_EQ(get32(good.data + at + 186), 4u);
        CHECK_EQ(get32(good.data + at + 190), 0u);
        struct Bad {
            const char* what;
            uint8_t phase, kind;
            uint32_t len, pos;
            bool loads;
        };
        for (const Bad& b : {Bad{"operand longer than 96 bytes", 1, 0, 180, 179, false}, Bad{"operand of 97 bytes", 1, 2, 97, 0, false},
                             Bad{"output longer than 96 bytes", 2, 0, 100, 0, false}, Bad{"register longer than 96 bytes", 3, 0, 180, 0, false},
                             Bad{"frame of a wrong size", 5, 0, 100, 0, false}, Bad{"frame of a wrong size", 6, 0, 96, 3, false},
                             Bad{"unknown phase", 7, 0, 4, 0, false}, Bad{"unknown use", 1, 3, 4, 0, false},
                             Bad{"length past the buffer", 0, 0, 181, 0, false}, Bad{"position past the length", 1, 0, 4, 5, false},
                             Bad{"FMOVEM of 8 registers", 1, 2, 96, 95, true}, Bad{"BUSY frame", 6, 0, 180, 179, true},
                             Bad{"IDLE frame", 5, 0, 24, 23, true}, Bad{"position at the end", 1, 0, 4, 4, true}}) {
            std::vector<uint8_t> bad(good.data, good.data + good.size);
            bad[at + 179] = b.phase;
            bad[at + 180] = b.kind;
            put32le(bad.data() + at + 186, b.len);
            put32le(bad.data() + at + 190, b.pos);
            if (!tryLoad(b.what, e, bad, b.loads ? nullptr : kLayout)) continue;
            // 読めたものは、オペランドを読み書きしても範囲の外に出ない
            for (int i = 0; i < 100; i++) f.write16(0x10, (uint16_t)(0x1234 + i));
            for (int i = 0; i < 100; i++) f.read16(0x10);
        }
        // X68000 のソフトが FRESTORE する BUSY のフレーム (ステートとは別の経路) でも同じ。フレームを FSAVE で取り出し、
        // フェーズ、長さ、位置を書き換えて戻す。範囲の外の組み合わせなら、待機に戻る (オペランドを書くと手順の違反)
        struct Frame {
            uint8_t phase;
            uint32_t len, pos;
            bool kept;
        };
        for (const Frame& fr : {Frame{1, 180, 179, false}, Frame{1, 97, 0, false}, Frame{1, 96, 96, false}, Frame{2, 4, 4, false},
                                Frame{5, 180, 0, false}, Frame{6, 180, 0, false}, Frame{1, 0xffff, 0xfffe, false},
                                Frame{1, 4, 2, true}}) {
            f.write16(0x02, 0);      // 手順を止める
            f.write16(0x0a, 0x4000); // オペランドを待つ状態にする
            const uint16_t format = f.read16(0x04); // FSAVE: BUSY のフレーム
            uint32_t w[45];
            for (int i = 44; i >= 0; i--) { // 高いアドレスから来る
                const uint32_t hi = f.read16(0x10);
                w[i] = hi << 16 | f.read16(0x10);
            }
            CHECK_EQ((w[0] >> 8) & 0xff, 1u); // P_IN
            CHECK_EQ(w[3], 4u << 16);         // 長さ 4、位置 0
            w[0] = (w[0] & 0xffff00ffu) | (uint32_t)fr.phase << 8;
            w[3] = fr.len << 16 | fr.pos;
            f.write16(0x06, format); // FRESTORE
            for (int i = 0; i < 45; i++) {
                f.write16(0x10, (uint16_t)(w[i] >> 16));
                f.write16(0x10, (uint16_t)w[i]);
            }
            // 待機に戻っていれば、FSAVE は IDLE のフレームを返す (BUSY のままなら BUSY)
            const uint16_t after = f.read16(0x04);
            if ((after == format) != fr.kept) {
                printf("  FRESTORE of phase %d len %u pos %u: format %04x (BUSY is %04x)\n", fr.phase, fr.len, fr.pos, after, format);
                failures++;
            }
            for (int i = 0; i < 100; i++) f.read16(0x10);
            for (int i = 0; i < 100; i++) f.write16(0x10, (uint16_t)i);
        }
    }
    // CD-ROM 装置から媒体を取り出してある (装置はあるが、イメージがない)。装置はセレクションに答え、INQUIRY や
    // REQUEST SENSE を実行するので、コマンドの受け取りや応答の転送の途中のステートは、読んだあとも続きができる
    {
        using x68k::Spc;
        // セレクションする (コマンドのフェーズになる)
        auto select = [](Spc& s, uint8_t temp) {
            spcTakeId7(s); // リセットの状態にして、自分の ID を 7 にする (続けて SCTL でリセットを解く)
            s.write(Spc::SCTL, 0x11);
            s.write(Spc::INTS, 0xff);
            s.write(Spc::PCTL, 0);
            s.write(Spc::TEMP, temp);
            spcTc(s, 0x2000);
            s.write(Spc::SCMD, Spc::CMD_SELECT);
            s.write(Spc::INTS, 0xff);
        };
        // コマンドのフェーズから、コマンドを送る
        auto command = [](Spc& s, std::initializer_list<uint8_t> cdb) {
            s.write(Spc::PCTL, Spc::PH_COMMAND);
            spcTc(s, (uint32_t)cdb.size());
            s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
            for (uint8_t b : cdb)
                if (s.read(Spc::SSTS) & Spc::SSTS_TRIP) s.write(Spc::DREG, b);
            s.write(Spc::INTS, 0xff);
        };
        // データインのフェーズから、n バイトまで受け取る
        auto dataIn = [](Spc& s, uint8_t* data, uint32_t n) {
            s.write(Spc::PCTL, Spc::PH_DATA_IN);
            spcTc(s, n);
            s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
            const uint32_t got = spcRead(s, data, n);
            s.write(Spc::INTS, 0xff);
            return got;
        };
        // ステータスとメッセージを受け取る。ステータスを返す (ステータスのフェーズでなければ -1)
        auto status = [](Spc& s) {
            if (s.read(Spc::PSNS) != (Spc::PSNS_REQ | Spc::PSNS_BSY | Spc::PH_STATUS)) return -1;
            const int st = s.read(Spc::TEMP);
            for (int i = 0; i < 2; i++) {
                s.write(Spc::SCMD, Spc::CMD_SET_ACK);
                s.write(Spc::SCMD, Spc::CMD_RESET_ACK);
            }
            return st;
        };
        const uint8_t kSelected = Spc::PSNS_REQ | Spc::PSNS_BSY | Spc::PH_COMMAND; // 0x8a
        static uint8_t buf[64];
        // セレクションの直後 (コマンドの受け取りを待っている)
        {
            Env e(config, false, true, false, false, true);
            CHECK_EQ(e.box.m->changeCd(nullptr), true);
            Spc& s = e.box.m->spcEx();
            CHECK_EQ(s.isCdrom(6) && !s.image(6), true);
            select(s, 0xc0);
            CHECK_EQ(s.read(Spc::PSNS), kSelected);
            Blob good;
            save(*e.box.m, good);
            std::vector<uint8_t> same(good.data, good.data + good.size);
            CHECK_EQ(tryLoad("CD-ROM drive without a medium, selected", e, same, nullptr), true);
            CHECK_EQ(s.read(Spc::PSNS), kSelected);
            // 続きのコマンドは INQUIRY (媒体がなくても答える)
            command(s, {0x12, 0, 0, 0, 36, 0});
            CHECK_EQ(s.read(Spc::PSNS) & 7, Spc::PH_DATA_IN);
            CHECK_EQ(dataIn(s, buf, 36), 36u);
            CHECK_EQ(buf[0], 0x05); // CD-ROM 装置
            CHECK_EQ(status(s), 0);
            CHECK_EQ(s.read(Spc::PSNS), 0);
            // TEST UNIT READY は CHECK CONDITION。セレクションのあとに保存して読み、それから送っても同じ
            select(s, 0xc0);
            save(*e.box.m, good);
            same.assign(good.data, good.data + good.size);
            CHECK_EQ(tryLoad("CD-ROM drive without a medium, selected again", e, same, nullptr), true);
            command(s, {0x00, 0, 0, 0, 0, 0});
            CHECK_EQ(status(s), 2);
            // REQUEST SENSE の応答 (NOT READY、MEDIUM NOT PRESENT) を 4 バイト受け取ったところで保存し、読んで残りを受け取る
            select(s, 0xc0);
            command(s, {0x03, 0, 0, 0, 18, 0});
            CHECK_EQ(s.read(Spc::PSNS) & 7, Spc::PH_DATA_IN);
            s.write(Spc::PCTL, Spc::PH_DATA_IN);
            spcTc(s, 18);
            s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
            CHECK_EQ(spcRead(s, buf, 4), 4u);
            save(*e.box.m, good);
            same.assign(good.data, good.data + good.size);
            CHECK_EQ(tryLoad("CD-ROM drive without a medium, REQUEST SENSE", e, same, nullptr), true);
            CHECK_EQ(s.read(Spc::PSNS) & (Spc::PSNS_BSY | 7), Spc::PSNS_BSY | Spc::PH_DATA_IN);
            CHECK_EQ(spcRead(s, buf + 4, 14), 14u);
            s.write(Spc::INTS, 0xff);
            CHECK_EQ((uint32_t)(buf[2] & 15) << 16 | (uint32_t)buf[12] << 8 | buf[13], 0x023a00u);
            CHECK_EQ(status(s), 0);
            CHECK_EQ(s.read(Spc::PSNS), 0);
            e.box.frames(1);
        }
        // 装置がなくなった ID (ハードディスクを外した) につながっているステートは、バスフリーに戻す
        {
            Env e(config, false, true);
            Spc& s = e.box.m->spcEx();
            select(s, 0x81);
            CHECK_EQ(s.read(Spc::PSNS), kSelected);
            Blob good;
            save(*e.box.m, good);
            e.box.m->attachScsiEx(0, nullptr);
            std::vector<uint8_t> same(good.data, good.data + good.size);
            CHECK_EQ(tryLoad("SPC connected to a removed disk", e, same, nullptr), true);
            CHECK_EQ(s.read(Spc::PSNS) & Spc::PSNS_BSY, 0);
            command(s, {0x08, 0, 0, 0, 1, 0});
            CHECK_EQ(status(s), -1);
            e.box.frames(1);
        }
        // 媒体のない CD-ROM 装置につながっているステートの、SPC の部分を書き換えて読ませる (つながっているのにイメージが
        // ない状態は、乱数の試験ではここだけ)。読めたら SPC を動かす (イメージを使う転送の途中にはなっていないこと)
        {
            Env e(config, false, true, false, false, true);
            CHECK_EQ(e.box.m->changeCd(nullptr), true);
            Spc& s = e.box.m->spcEx();
            select(s, 0xc0);
            command(s, {0x12, 0, 0, 0, 36, 0});
            s.write(Spc::PCTL, Spc::PH_DATA_IN);
            spcTc(s, 36);
            s.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
            CHECK_EQ(spcRead(s, buf, 3), 3u);
            Blob good;
            save(*e.box.m, good);
            const size_t time = findTag(good, "TIME");
            size_t spc = 0; // 外付け SCSI ボードの SPC (TIME の手前の最後の "SPC ")
            for (size_t at = findTag(good, "SPC "); at && at < time; at = findTag(good, "SPC ", at + 4)) spc = at;
            CHECK_EQ(spc != 0 && time > spc + 400, true);
            // 書き換える場所: レジスタからデータの位置まで (区切りのあと 60 バイト) と、センスデータと応答のバッファのあとの、
            // イメージの中の位置 8、セクタの並び 4 + 4、イメージの大きさ 8、装置の種類 1、CD-ROM 装置があるか 1 (26 バイト)
            const size_t tail = spc + 354;
            scene::Rng rng;
            rng.s = 6258;
            if (const char* x = getenv("STATE_TEST_SEED")) rng.s ^= (uint32_t)strtoul(x, nullptr, 0) * 2654435761u;
            if (rng.s == 0) rng.s = 1;
            int rounds = 400, loaded = 0;
            signal(SIGALRM, onAlarm);
            if (const char* r = getenv("STATE_TEST_ROUNDS")) rounds = atoi(r);
            for (int round = 0; round < rounds; round++) {
                std::vector<uint8_t> bad(good.data, good.data + good.size);
                const int n = 1 + (int)(rng() % 4);
                for (int k = 0; k < n; k++) {
                    const size_t pos = rng() % 2 ? spc + 4 + rng() % 60 : tail + rng() % 26;
                    static const uint8_t kValues[] = {0, 1, 2, 3, 6, 7, 0x80, 0xff};
                    bad[pos] = rng() % 4 ? kValues[rng() % sizeof kValues] : (uint8_t)rng();
                }
                fixHeader(bad.data(), bad.size());
                const char* err = nullptr;
                g_round = round;
                alarm(60); // 読んだあとに動かしたとき、終わらなくなっていないか
                if (!x68k::loadState(*e.box.m, bad.data(), bad.size(), err)) continue;
                loaded++;
                // 読めたステートを動かすのにかかる時間に上限を置く (corrupted() と同じ)
                const clock_t t0 = clock();
                Spc& t = e.box.m->spcEx();
                t.read(Spc::TEMP);
                spcRead(t, buf, 8);
                for (int i = 0; i < 8; i++) t.write(Spc::DREG, (uint8_t)(0x12 + i));
                for (int i = 0; i < 14; i++) {
                    t.read(Spc::TEMP);
                    t.write(Spc::SCMD, Spc::CMD_SET_ACK);
                    t.write(Spc::SCMD, Spc::CMD_RESET_ACK);
                }
                for (uint8_t ph : {Spc::PH_DATA_OUT, Spc::PH_DATA_IN, Spc::PH_COMMAND}) {
                    t.write(Spc::PCTL, ph);
                    spcTc(t, 12);
                    t.write(Spc::SCMD, Spc::CMD_TRANSFER | Spc::SCMD_PT);
                    for (int i = 0; i < 12; i++) t.write(Spc::DREG, 0x2a);
                    spcRead(t, buf, 12);
                    t.write(Spc::INTS, 0xff);
                }
                e.box.frames(1);
                const double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
                if (secs > 5.0) {
                    printf("  round %d: running the loaded state took %.1f s\n", round, secs);
                    failures++;
                }
            }
            alarm(0);
            printf("  CD-ROM drive without a medium: %d rounds, %d loaded\n", rounds, loaded);
            CHECK_EQ(rounds < 40 || loaded > rounds / 4, true);
        }
    }
    // CPU の時刻 (サイクル数) が 64 ビットの上限に近い。68030 / 68060 は、進める先の時刻の足し算があふれて CPU が
    // 進まなくなり、Machine::run() が戻らない。68000 は Machine の側の足し算があふれて、進まずに戻る。
    // 上限 (STATE_CPU_CYCLES_MAX、STATE_CPU020_CYCLES_MAX) を超える時刻は、読むときに小さい値に直す
    // (断らない)。上限ちょうどはそのまま読めて、そこから進み、進んだあとの保存も読める。時刻を直しても、時刻で持っている
    // 値どうしの差は変わらず、読まずに続けたものと同じに動く
    for (int cpu : {68000, 68030, 68060}) {
#ifdef PIX68K_NO_CPU020
        if (cpu != 68000) continue;
#endif
        x68k::Config c;
        c.setCpu(cpu);
        Env e(c, true, false); // MIDI ボードつき
        Blob good;
        save(*e.box.m, good);
        auto get64 = [&](const uint8_t* p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; };
        // 時刻の位置は、CPU の部分の中で、今のサイクル数になる 8 バイトを探す (68030 / 68060 は 1/Cpu020::SUB サイクルの単位)
#ifndef PIX68K_NO_CPU020
        const uint64_t unit = cpu == 68000 ? 1 : (uint64_t)m68k::Cpu020::SUB;
#else
        const uint64_t unit = 1;
#endif
        auto timeAt = [&](const Blob& b, uint64_t cycles) -> size_t {
            const size_t tag = findTag(b, cpu == 68000 ? "0006" : "0306");
            size_t at = 0;
            int found = 0;
            for (size_t i = tag + 4; tag && i < tag + 400 && i + 8 <= b.size; i++) {
                if (get64(b.data + i) / unit == cycles) {
                    if (!found) at = i;
                    found++;
                }
            }
            CHECK_EQ(found, 1);
            return found == 1 ? at : 0;
        };
        const size_t at = timeAt(good, e.box.m->cycles());
        if (!at) continue;
        auto with = [&](uint64_t v) {
            std::vector<uint8_t> bad(good.data, good.data + good.size);
            put32le(bad.data() + at, (uint32_t)v);
            put32le(bad.data() + at + 4, (uint32_t)(v >> 32));
            return bad;
        };
        const uint64_t max = cpu == 68000 ? x68k::STATE_CPU_CYCLES_MAX : x68k::STATE_CPU020_CYCLES_MAX;
        const uint64_t limit = max * unit;
#ifndef PIX68K_NO_CPU020
        if (cpu != 68000) {
            // 版 14 までは 1/800 サイクルの単位 (STATE_CPU020_SUB_OLD) で書く。読むと今の単位 (Cpu020::SUB) に直り、
            // 既定の速さの時刻 (端数が 16 の倍数) はそのまま戻る。古い上限 (2^53 サイクル) も今の上限 (2^49 サイクル) も
            // 超える時刻は、今の上限で直り、端数は 16 倍になる
            const uint64_t old = x68k::STATE_CPU020_SUB_OLD, k = m68k::Cpu020::SUB / old;
            const uint64_t sub = e.box.m->cpu020()->sub_;
            CHECK_EQ(sub % k, 0u);
            Blob b14;
            b14.size = good.size + 4096;
            b14.data = new uint8_t[b14.size];
            size_t n14 = 0;
            CHECK_EQ(x68k::saveState(*e.box.m, b14.data, b14.size, n14, x68k::STATE_CPU020_SUB - 1), true);
            b14.size = n14;
            std::vector<uint8_t> v14(b14.data, b14.data + n14);
            const size_t tag = findTag(b14, "0306");
            size_t at14 = 0;
            int found = 0;
            for (size_t i = tag + 4; tag && i < tag + 400 && i + 8 <= v14.size(); i++) {
                if (get64(v14.data() + i) == sub / k) {
                    if (!found) at14 = i;
                    found++;
                }
            }
            CHECK_EQ(found, 1);
            if (found == 1) {
                CHECK_EQ(tryLoad("version 14 CPU time", e, v14, nullptr), true);
                CHECK_EQ(e.box.m->cpu020()->sub_, sub);
                const uint64_t c = ((uint64_t)1 << 53) + 12345;
                const uint64_t v = c * old + 7;
                put32le(v14.data() + at14, (uint32_t)v);
                put32le(v14.data() + at14 + 4, (uint32_t)(v >> 32));
                CHECK_EQ(tryLoad("version 14 CPU time past the limit", e, v14, nullptr), true);
                CHECK_EQ(e.box.m->cycles(), x68k::STATE_CPU_TIME_PAST + c % (max / 2));
                CHECK_EQ(e.box.m->cpu020()->sub_ % m68k::Cpu020::SUB, 7 * k);
            }
        }
#endif
        signal(SIGALRM, onAlarm);
        g_round = cpu;
        // 上限を超える時刻: 読めて、時刻は小さい値 (余裕 + 上限の半分で割った余り) になり、そこから進む
        // (戻らなければ 60 秒で止める)。進んだあとの保存は、読むときの確かめを通る (save() が確かめる)
        const uint64_t past = x68k::STATE_CPU_TIME_PAST;
        const uint64_t kPast[] = {UINT64_MAX - 100, UINT64_MAX, limit + 1, limit + unit, limit + limit / 2, (uint64_t)1 << 63};
        for (uint64_t v : kPast) {
            if (!tryLoad("CPU time past the limit", e, with(v), nullptr)) continue;
            const uint64_t t0 = e.box.m->cycles();
            CHECK_EQ(t0, past + v % (limit / 2) / unit);
            alarm(60);
            e.run(1000);
            e.box.frames(3);
            alarm(0);
            CHECK_EQ(e.box.m->cycles() >= t0 + 1000 && e.box.m->cycles() < t0 + 1000 + e.box.m->cpuHz(), true);
            Blob after;
            save(*e.box.m, after);
        }
        // Machine の時刻 (TIME の区切りのあと): 進め終えた時刻 8、端数 8 x 2、ラスタの始まり 8、次のラスタの始まり 8
        auto lineTimes = [&](x68k::Machine& m, uint64_t& sinceStart, uint64_t& untilNext) {
            Blob b;
            save(m, b);
            const size_t time = findTag(b, "TIME");
            CHECK_EQ(time != 0, true);
            sinceStart = m.cycles() - get64(b.data + time + 4 + 24);
            untilNext = get64(b.data + time + 4 + 32) - m.cycles();
        };
        Env e2(c, true, false);
        Disks disks;
        // 上限ちょうどと、上限の 100 サイクル手前: そのまま読める。そこから上限を 1 サイクル超えるまで進めて保存したもの
        // (時刻は上限を超え、ラスタの始まりは上限ちょうどか、上限より前) を読んでも、ラスタの始まりからの時間と、
        // 次のラスタまでの残りは変わらない。続けて少しずつ進めても同じ
        for (uint64_t before : {(uint64_t)0, (uint64_t)100}) {
            CHECK_EQ(tryLoad("CPU time at the limit", e, with(limit - before * unit), nullptr), true);
            CHECK_EQ(e.box.m->cycles(), max - before);
            bool first = true;
            for (int64_t step : {(int64_t)before + 1, (int64_t)1, (int64_t)100, (int64_t)3}) {
                e.run(step);
                const uint64_t t1 = e.box.m->cycles();
                CHECK_EQ(t1 > max, true);
                Blob over;
                save(*e.box.m, over);
                uint64_t since1 = 0, until1 = 0, since2 = 1, until2 = 1;
                lineTimes(*e.box.m, since1, until1);
                if (first) CHECK_EQ(t1 - since1, max - before); // ラスタの始まりは、読んだときの時刻のまま
                first = false;
                disks.take(e);
                disks.give(e2);
                const std::vector<uint8_t> copy(over.data, over.data + over.size);
                CHECK_EQ(tryLoad("CPU time just past the limit", e2, copy, nullptr), true);
                CHECK_EQ(e2.box.m->cycles(), past + t1 - max);
                lineTimes(*e2.box.m, since2, until2);
                CHECK_EQ(since2, since1);
                CHECK_EQ(until2, until1);
                CHECK_EQ(until1 > 0 && until1 < e.box.m->cpuHz() / 1000, true); // 1 ラスタは 1ms より短い
            }
        }
        // 長く動かしても戻る。周辺機器を動作の途中にして (SASI、FDC、ADPCM と DMAC、OPM、MIDI の送信、キー、マウスの
        // パケットの送信の途中) 保存し、別の Machine に読んで、同じ入力で続ける。読まずに続けたものと、マウスから
        // 受け取るバイト、進むサイクル数、レジスタが同じ。終わりのステートも、読まずに続けたものの保存を読んで
        // 時刻を直したものと、すべて同じ (時刻で持っている値に、戻し忘れがあれば合わない)
        // (runFrame() は上限を超えた時刻を戻すので、ここでは run() で進める。Machine::rebaseTime())
        alarm(120);
        e.run(1000);
        e.run((int64_t)e.box.m->cpuHz() * 2);
        e.run(5000000);
        makeBusy(e, true, false, false, false);
        // マウス: SCC のチャネル B で受信を許可し、RTS を出してパケットの送信を始める
        auto sccB = [](Env& x, uint8_t reg, uint8_t v) {
            x.out(0xe98001, reg);
            x.out(0xe98001, v);
        };
        sccB(e, 3, 0xc1);
        sccB(e, 5, 0x60);
        e.box.m->input().onMouse(10, -3, 1);
        sccB(e, 5, 0x62);
        e.run(3000); // 1 バイト目を送っている途中 (4800bps の 1 バイトは 2ms)
        alarm(0);
        const uint64_t t1 = e.box.m->cycles();
        CHECK_EQ(t1 >= max + 1000 + 5000000, true);
        Blob over;
        save(*e.box.m, over);
        const size_t overAt = timeAt(over, t1);
        CHECK_EQ(overAt && get64(over.data + overAt) > limit, true);
        {
            disks.take(e);
            disks.give(e2);
            const std::vector<uint8_t> copy(over.data, over.data + over.size);
            CHECK_EQ(tryLoad("CPU time past the limit after running", e2, copy, nullptr), true);
            const uint64_t t2 = e2.box.m->cycles();
            CHECK_EQ(t2, past + t1 - max);
            alarm(120);
            int bytes = 0;
            for (int i = 0; i < 40; i++) {
                for (Env* x : {&e, &e2}) {
                    if (i == 3) x->box.m->input().onKey(0x04, false);
                    if (i == 5) x->box.m->input().onMouse(-7, 20, 2);
                    if (i == 32) sccB(*x, 5, 0x60);
                    if (i == 33) sccB(*x, 5, 0x62);
                    if (i == 9) x->out(0xeafa0d, 0x91); // MIDI の送信の続き
                    if (i == 10) x->out(0xeafa0d, 0x3c);
                    if (i == 20) x->box.m->input().onKey(0x05, false);
                    x->run(i < 30 ? 9000 : 300000);
                }
                // SCC のチャネル B: RR0 (受信したバイトがあるか) と、受け取ったバイト
                const uint8_t ra = e.in(0xe98001), rb = e2.in(0xe98001);
                CHECK_EQ(rb, ra);
                if (ra & 1) {
                    CHECK_EQ(e2.in(0xe98003), e.in(0xe98003));
                    bytes++;
                }
                CHECK_EQ(e2.box.m->cycles() - t2, e.box.m->cycles() - t1);
            }
            alarm(0);
            CHECK_EQ(bytes >= 6, true); // マウスのパケット 2 つ
            m68k::Registers ra, rb;
            e.box.m->getRegisters(ra);
            e2.box.m->getRegisters(rb);
            CHECK_EQ(rb.pc, ra.pc);
            CHECK_EQ(rb.sr, ra.sr);
            // 終わりのステート: e2 の保存と、e の保存 (時刻は上限を超えている) を読んで保存し直したものが同じ
            Blob sa, sb, sa2;
            save(*e.box.m, sa);
            save(*e2.box.m, sb);
            const uint64_t t3 = e2.box.m->cycles();
            disks.take(e);
            disks.give(e2);
            const std::vector<uint8_t> fromA(sa.data, sa.data + sa.size);
            CHECK_EQ(tryLoad("CPU time past the limit at the end", e2, fromA, nullptr), true);
            CHECK_EQ(e2.box.m->cycles(), t3);
            save(*e2.box.m, sa2);
            if (!same(sa2, sb)) {
                printf("  %d: states differ after continuing from the state with the time brought back\n", cpu);
                reportDiff(sa2, sb);
                failures++;
            }
            // もう一度読んでも、同じ時刻のまま (上限以下なので直さない)
            const std::vector<uint8_t> again(sb.data, sb.data + sb.size);
            CHECK_EQ(tryLoad("CPU time after it was brought back", e2, again, nullptr), true);
            CHECK_EQ(e2.box.m->cycles(), t3);
        }
        // 続けて動かして上限を超えたら、フレームの切れ目で時刻を戻す (Machine::rebaseTime()。#165)。上限の少し手前
        // (1/4 秒前) の時刻で、周辺機器を動作の途中にして (SASI、FDC、ADPCM と DMAC、OPM、MIDI の送信、キー、
        // マウスのパケットの送信の途中) 保存し、e と e2 に読む。e2 だけ、先に shiftTime() で (上限 - 2^40) サイクル
        // 戻す。e は runFrame() で上限を越え、次のフレームの初めに同じだけ戻る。どちらも止まらずに進み、同じ入力で
        // 続けると、戻ったあとはステートがすべて同じ (戻し忘れた時刻があれば合わない)。フロントエンドが実時間と
        // 合わせるのに使う totalCycles() は、戻しても途切れない
        {
            disks.take(e2);
            disks.give(e);
            CHECK_EQ(tryLoad("CPU time before the limit", e, with(limit - (uint64_t)(e.box.m->cpuHz() / 4) * unit), nullptr),
                     true);
            makeBusy(e, true, false);
            sccB(e, 3, 0xc1);
            sccB(e, 5, 0x60);
            e.box.m->input().onKey(0x06, true);
            e.box.m->input().onMouse(10, -3, 1);
            sccB(e, 5, 0x62);
            e.run(3000);
            CHECK_EQ(e.box.m->cycles() < max, true);
            CHECK_EQ(e.box.m->timeShifted(), 0u);
            Blob s0;
            save(*e.box.m, s0);
            disks.take(e);
            disks.give(e2);
            CHECK_EQ(tryLoad("CPU time before the limit (copy)", e2, std::vector<uint8_t>(s0.data, s0.data + s0.size), nullptr),
                     true);
            const uint64_t shift = max - past;
            const uint64_t shifted0 = e2.box.m->timeShifted();
            e2.box.m->shiftTime(shift);
            CHECK_EQ(e2.box.m->timeShifted() - shifted0, shift);
            CHECK_EQ(e2.box.m->cycles(), e.box.m->cycles() - shift);
            const uint64_t total0 = e.box.m->totalCycles(), total2 = e2.box.m->totalCycles();
            alarm(120);
            bool crossed = false;
            int bytes = 0;
#ifndef PIX68K_NO_CPU020
            // 68030 / 68060 では、戻す量は 240 サイクルの倍数で、戻す前後で時刻を 240 サイクルで割った余り (端数も含む) が
            // 変わらない。X68030 と 060turbo の本体の I/O の読み書きをそろえる 1.6 マイクロ秒の区切り (X68030 の 25MHz の
            // 40 サイクル、060turbo の 50MHz の 80 サイクル、75MHz の 120 サイクル。Config::busArea の period) の位置が、時刻を戻しても動かない (#163)
            const uint64_t phaseUnit = x68k::STATE_TIME_SHIFT_UNIT * (uint64_t)m68k::Cpu020::SUB;
            bool phaseChecked = false;
            if (cpu != 68000) CHECK_EQ(shift % x68k::STATE_TIME_SHIFT_UNIT, 0u);
#endif
            for (int i = 0; i < 40; i++) {
#ifndef PIX68K_NO_CPU020
                if (cpu != 68000 && e.box.m->cpu020()) {
                    // runFrame() の初めと同じ rebaseTime() を先に呼び、戻ったフレームで余りを比べる
                    const uint64_t before = e.box.m->cpu020()->subCycles(), shifted = e.box.m->timeShifted();
                    e.box.m->rebaseTime();
                    if (e.box.m->timeShifted() != shifted) {
                        const uint64_t after = e.box.m->cpu020()->subCycles();
                        CHECK_EQ(after < before, true);
                        CHECK_EQ(after % phaseUnit, before % phaseUnit);
                        // 周期は 68030 の 25MHz で 40 サイクル、68060 の 50MHz で 80 サイクル、75MHz で 120 サイクル (どれも 240 の約数)
                        for (uint64_t period : {(uint64_t)40, (uint64_t)80, (uint64_t)120})
                            CHECK_EQ(after % (period * m68k::Cpu020::SUB), before % (period * m68k::Cpu020::SUB));
                        phaseChecked = true;
                    }
                }
#endif
                for (Env* x : {&e, &e2}) {
                    if (i == 2) x->box.m->input().onKey(0x06, false);
                    if (i == 5) x->box.m->input().onMouse(-7, 20, 2);
                    if (i == 9) x->out(0xeafa0d, 0x91); // MIDI の送信の続き
                    if (i == 10) x->out(0xeafa0d, 0x3c);
                    if (i == 20) sccB(*x, 5, 0x60);
                    if (i == 21) sccB(*x, 5, 0x62);
                    if (i == 22) x->box.m->input().onMouse(3, 4, 0);
                    x->box.frames(1);
                }
                const uint8_t ra = e.in(0xe98001), rb = e2.in(0xe98001);
                CHECK_EQ(rb, ra);
                if (ra & 1) {
                    CHECK_EQ(e2.in(0xe98003), e.in(0xe98003));
                    bytes++;
                }
                CHECK_EQ(e.box.m->totalCycles() - total0, e2.box.m->totalCycles() - total2);
                if (e.box.m->timeShifted()) {
                    crossed = true;
                    CHECK_EQ(e.box.m->timeShifted(), shift);
                    CHECK_EQ(e.box.m->cycles(), e2.box.m->cycles());
                } else {
                    CHECK_EQ(e.box.m->cycles(), e2.box.m->cycles() + shift);
                }
            }
            alarm(0);
            CHECK_EQ(crossed, true);
#ifndef PIX68K_NO_CPU020
            if (cpu != 68000) CHECK_EQ(phaseChecked, true);
#endif
            CHECK_EQ(bytes >= 6, true); // マウスのパケット 2 つ
            CHECK_EQ(e.box.m->cycles() < max, true);
            Blob sa, sb;
            save(*e.box.m, sa);
            save(*e2.box.m, sb);
            if (!same(sa, sb)) {
                printf("  %d: states differ after the time was brought back at a frame boundary\n", cpu);
                reportDiff(sa, sb);
                failures++;
            }
            Disks da, db;
            da.take(e);
            db.take(e2);
            CHECK_EQ(da.same(db), true);
        }
        disks.take(e2);
        disks.give(e);
        CHECK_EQ(tryLoad("CPU time (unchanged)", e, with(get64(good.data + at)), nullptr), true);
        e.box.frames(1);
    }
}

int human68k(const char* romDir, const char* diskPath, const char* iplPath, int cpu) {
    char path[1200];
    size_t iplSize = 0, cgSize = 0, diskSize = 0;
    snprintf(path, sizeof path, "%s/IPLROM.DAT", romDir);
    uint8_t* ipl = readFile(iplPath ? iplPath : path, iplSize);
    snprintf(path, sizeof path, "%s/CGROM.DAT", romDir);
    uint8_t* cg = readFile(path, cgSize);
    uint8_t* disk = readFile(diskPath, diskSize);
    if (!ipl || iplSize != x68k::Machine::IPL_SIZE || !cg || cgSize != x68k::Machine::CG_SIZE || !disk) {
        printf("no ROM or disk; skipped\n");
        delete[] ipl;
        delete[] cg;
        delete[] disk;
        return 77;
    }
    // 68060 で動く IPL ROM (IPLROM060.DAT、X68030 の IPL ROM 1.3 でないもの) は、同じディレクトリの ROM30_060.DAT を
    // $FC0000 からの ROM として組で使う (x68k_boot、ホスト版と同じ)。IPL ROM 1.6 は $FC0000 に SCSIINROM 16 がないと
    // 「SCSIINROM 16 is required」で止まり Human68k が起動しないので、ROM30_060.DAT がなければ飛ばす (#125)
    uint8_t* rom30 = nullptr;
    size_t rom30Size = 0;
    static const uint8_t kRomver13[] = {0x20, 0x3c, 0x13, 0x92, 0x11, 0x27, 0x4e, 0x75};
    if (cpu == 68060 && iplPath && memcmp(ipl + 0x10030, kRomver13, sizeof kRomver13) != 0) {
        const char* slash = strrchr(iplPath, '/');
        snprintf(path, sizeof path, "%.*sROM30_060.DAT", slash ? (int)(slash - iplPath + 1) : 0, iplPath);
        rom30 = readFile(path, rom30Size);
        if (!rom30) {
            printf("no %s; skipped\n", path);
            delete[] ipl;
            delete[] cg;
            delete[] disk;
            return 77;
        }
        if (rom30Size == 0 || rom30Size > x68k::Machine::SCSI_ROM_SIZE) { // setScsiRom() が断る大きさ
            printf("%s: %zu bytes (want 1 to %u)\n", path, rom30Size, (unsigned)x68k::Machine::SCSI_ROM_SIZE);
            delete[] rom30;
            delete[] ipl;
            delete[] cg;
            delete[] disk;
            return 1;
        }
        printf("$FC0000 ROM: %s\n", path);
    }
    auto setRoms = [&](x68k::Machine& m) {
        CHECK_EQ(m.setIplRom(ipl, iplSize), true);
        if (rom30) CHECK_EQ(m.setScsiRom(rom30, rom30Size), true);
    };
    printf("human68k on %d (saved while booting from %s)\n", cpu, diskPath);
    x68k::Config config;
    if (!config.setCpu(cpu)) {
        printf("no %d in this build; skipped\n", cpu);
        delete[] ipl;
        delete[] rom30;
        delete[] cg;
        delete[] disk;
        return 77;
    }
    uint8_t* diskA = new uint8_t[diskSize];
    memcpy(diskA, disk, diskSize);
    {
        x68k::FloppyImage imgA;
        imgA.load(diskA, diskSize, false, diskPath);
        Box a(config);
        setRoms(*a.m);
        a.m->setCgRom(cg, cgSize);
        a.m->setTime(1767225600);
        a.m->insertDisk(0, &imgA);
        a.m->powerOn();
        // 保存する時点を少しずつずらし、FDC の読み込みの途中に当たるようにする
        const int kPoints = 6;
        int saveAt[kPoints] = {100, 150, 200, 260, 333, 450};
        int done = 0;
        Blob saved[kPoints];
        int withTlb = 0;
        for (int k = 0; k < kPoints; k++) {
            framesChecked(*a.m, saveAt[k] - done, [&]() { a.frames(1); });
            done = saveAt[k];
            save(*a.m, saved[k]);
            withTlb += hasTlb(saved[k]);
        }
        framesChecked(*a.m, 900 - done, [&]() { a.frames(1); });
        const uint32_t wantSum = a.checksum();
        Blob sa;
        save(*a.m, sa);
        for (int k = 0; k < kPoints; k++) {
            Box c(config);
            setRoms(*c.m);
            c.m->setCgRom(cg, cgSize);
            x68k::FloppyImage img;
            uint8_t* d = new uint8_t[diskSize];
            memcpy(d, disk, diskSize);
            img.load(d, diskSize, false, diskPath);
            c.m->setTime(1767225600);
            c.m->powerOn();
            // ディスクを入れずに別の時点まで動かし (FDC と DMAC は A と違う状態になる)、入れてから読む
            c.frames(50 + k * 17);
            c.m->insertDisk(0, &img);
            const char* err = nullptr;
            CHECK_EQ(x68k::loadState(*c.m, saved[k].data, saved[k].size, err), true);
            if (err) printf("  %s\n", err);
            Blob again;
            save(*c.m, again);
            if (!same(saved[k], again)) {
                printf("  save after load differs (frame %d)\n", saveAt[k]);
                reportDiff(saved[k], again);
                failures++;
            }
            c.frames(900 - saveAt[k]);
            CHECK_EQ(c.checksum(), wantSum);
            Blob sc;
            save(*c.m, sc);
            if (!same(sa, sc)) {
                printf("  states after loading at frame %d differ\n", saveAt[k]);
                reportDiff(sa, sc);
                failures++;
            }
            c.m->ejectDisk(0);
            img.unload();
            delete[] d;
        }
        // 起動したあとの場面 (machine_bench の場面と同じもの) の途中で保存し、数秒動かす: ユーザモードの計算のループ、
        // グラフィックと OPM、フロッピーディスクを読み続けるループ (FDC から RAM への DMA が動き続ける)
        struct Scene {
            const char* name;
            bool (*start)(x68k::Machine&);
        };
        static const Scene kScenes[] = {
            {"user", [](x68k::Machine& m) { return scene::jumpTo(m, scene::kCpuLoop, sizeof scene::kCpuLoop / 2, true); }},
            {"gfx", [](x68k::Machine& m) { return scene::startGfx(m); }},
            {"fd", [](x68k::Machine& m) { return scene::jumpTo(m, scene::kFdLoop, sizeof scene::kFdLoop / 2); }},
        };
        const int kBefore = 47, kAfter = 220; // 保存の前と後のフレーム数 (後は 4 秒ほど)
        for (const Scene& sc : kScenes) {
            CHECK_EQ(sc.start(*a.m), true);
            a.frames(kBefore);
            Blob saved;
            save(*a.m, saved);
            withTlb += hasTlb(saved);
            std::vector<uint8_t> diskAt(diskA, diskA + diskSize); // 保存の時点のディスク (起動のあとは書かないが、念のため)
            // 画面の外 (今の画面の大きさより右と下) には前の場面の絵が残っている。ステートには入らないものなので、
            // 保存した側と読んだ側の両方で消してから比べる
            a.clearPixels();
            framesChecked(*a.m, kAfter, [&]() { a.frames(1); });
            const uint32_t want = a.checksum();
            Blob sa2;
            save(*a.m, sa2);
            Box c(config);
            setRoms(*c.m);
            c.m->setCgRom(cg, cgSize);
            x68k::FloppyImage img;
            std::vector<uint8_t> d(diskAt);
            img.load(d.data(), diskSize, false, diskPath);
            c.m->setTime(1767225600);
            c.m->powerOn();
            c.frames(120); // ディスクなしで IPL ROM が待っている状態
            c.m->insertDisk(0, &img);
            const char* err = nullptr;
            CHECK_EQ(x68k::loadState(*c.m, saved.data, saved.size, err), true);
            if (err) printf("  %s\n", err);
            c.clearPixels();
            c.frames(kAfter);
            CHECK_EQ(c.checksum(), want);
            Blob sc2;
            save(*c.m, sc2);
            if (!same(sa2, sc2)) {
                printf("  states after the %s scene differ\n", sc.name);
                reportDiff(sa2, sc2);
                failures++;
            }
            c.m->ejectDisk(0);
            img.unload();
        }
        // 68060 の MMU が有効な間に保存したものの数 (TLB と ATC が入る)。IPL ROM 1.3 での Human68k と 060turbo.sys の起動では
        // 0、IPLROM060.DAT と ROM30_060.DAT では 9 のうち 7 だった
        printf("  %d of %d saved states hold the 68060 TLB and ATC\n", withTlb,
               kPoints + (int)(sizeof kScenes / sizeof kScenes[0]));
        a.m->ejectDisk(0);
        imgA.unload();
    }
    delete[] diskA;
    delete[] ipl;
    delete[] rom30;
    delete[] cg;
    delete[] disk;
    return finish();
}

} // namespace

int main(int argc, char** argv) {
    const char* romDir = nullptr;
    const char* disk = nullptr;
    const char* ipl = nullptr;
    int cpu = 68000;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "-r")) romDir = argv[i + 1];
        else if (!strcmp(argv[i], "-0")) disk = argv[i + 1];
        else if (!strcmp(argv[i], "--ipl")) ipl = argv[i + 1];
        else if (!strcmp(argv[i], "--cpu")) cpu = atoi(argv[i + 1]);
    }
    if (disk) {
        char def[1100];
        if (!romDir) {
            const char* home = getenv("HOME");
            snprintf(def, sizeof def, "%s/.cache/pix68k/rom", home ? home : ".");
            romDir = def;
        }
        return human68k(romDir, disk, ipl, cpu);
    }
    if (argc == 2 && !strcmp(argv[1], "--corrupted")) { // 壊れたステートの試験だけ (STATE_TEST_ROUNDS で回数を増やして回す)
        craftedStates();
        corruptedAll();
        return finish();
    }
    x68k::Config c68000;
    roundTrip("68000 (gfx scene)", c68000);
    x68k::Config c10;
    c10.setClockMHz(10);
    roundTrip("68000 10MHz", c10);
#ifndef PIX68K_NO_CPU020
    x68k::Config c030;
    c030.setCpu(68030);
    c030.highMemSize = 2u << 20;
    roundTrip("68030 (gfx scene, 2MB high memory)", c030);
    x68k::Config c060;
    c060.setCpu(68060);
    c060.localMemSize = 16u << 20;
    roundTrip("68060 (gfx scene, 16MB local memory)", c060);
    fpuPending();
    fpuPending060();
    sizeKeepsTiming060();
    timingState060();
    loadStoppedCpu(68030);
    loadStoppedCpu(68060);
    stopTimerWake(68030);
    stopTimerWake(68060);
    mmu030();
    mmu060();
#endif
    rejects();
    rejectsMore();
    scsiBoard();
    sasiScript();
    fdcScript();
    adpcmScript();
    midiScript();
    scsiScript();
    corruptedAll();
    craftedStates();
    soundCopy();
    soundLinesAfterLoad();
    return finish();
}
