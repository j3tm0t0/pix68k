// 68030 / 68060 のコア (Cpu020) で、命令ごとのサイクル数と結果が変わっていないことを確かめる。
//
// 命令の処理 (cpu020_ops.inc) は、68060 用 (tick() と is060() の検査を消したもの) と、両方の機種で使えるものの 2 つに
// 分けてコンパイルする (#170。cpu020.h の For<M060>)。68030 の表に 68060 用の処理が入ると、68030 のサイクル数が
// 足されなくなり、68060 だけの動き (奇数番地への JSR、DIVU のあふれなど) になる。この試験は、小さな命令の列を
// 68030 と 68060 で動かし、進んだ時間 (1/SUB サイクル単位) と、終わったときのレジスタとメモリから作った値を、
// 分ける前のコアで取った値 (下の golden) と比べる。
//
// 動かし方は 6 通り:
//   0  68030、直接読み書きするページ (fast page) あり、速さ 100
//   1  68030、fast page あり、速さ 200 (setSpeed())
//   2  68030、fast page なし (どの読み書きも Bus に出る)
//   3  68060、fast page あり (リセットのまま。キャッシュは無効)
//   4  68060、fast page なし
//   5  68060、fast page あり、命令キャッシュとデータキャッシュを有効にする
//
// golden の作り直し: 命令の列を変えたときは、cpu020_cycles_test --print の出力で golden の中身を置き換える。
// サイクル数や結果を変える変更をしたときも同じ (変わった行と理由を PR に書くこと)
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "cpu020_env.h"
#include "test_check.h"

using namespace m68k;
using namespace cpu020test;

namespace {

struct Case {
    const char* name;
    std::vector<uint16_t> words; // $1000 から置く
    int steps;
    void (*setup)(Env&); // 命令の列のほかに要る準備 (なければ nullptr)
};

constexpr int RUNS = 6;
struct Golden {
    const char* name;
    uint64_t sub[RUNS];
    uint32_t hash[RUNS];
};

// スタックに戻り先を置く (RTS / RTD / RTR)
void stackEven(Env& e) {
    e.cpu.a_[7] = 0x8ff0;
    e.bus.w16(0x8ff0, 0x001f); // RTR の CCR
    e.bus.w32(0x8ff2, 0x00002000);
    e.bus.w32(0x8ff6, 0x00002000);
}
void stackRts(Env& e) {
    e.cpu.a_[7] = 0x8ff0;
    e.bus.w32(0x8ff0, 0x00002000);
}
void stackRtsOdd(Env& e) {
    e.cpu.a_[7] = 0x8ff0;
    e.bus.w32(0x8ff0, 0x00002001);
}
void rtsAt3000(Env& e) { e.bus.w16(0x3000, 0x4e75); }
void rteFrame(Env& e) {
    e.cpu.a_[7] = 0x8ff0;
    e.bus.w16(0x8ff0, 0x2000);
    e.bus.w32(0x8ff2, 0x00002000);
    e.bus.w16(0x8ff6, 0x0000);
}
void zeroLowD3(Env& e) { e.cpu.d_[3] = 0x80000000; }

const Case cases[] = {
    // ---- 実効アドレスと命令の語の取り出し ----
    {"move.l #imm,d0", {0x203c, 0x1234, 0x5678}, 1, nullptr},
    {"move.w (d16,a0),d1", {0x3228, 0x0010}, 1, nullptr},
    {"move.w d0,-(a1)", {0x3300}, 1, nullptr},
    {"move.l (a0)+,d2", {0x2418}, 1, nullptr},
    {"move.b (a7)+,d2", {0x141f}, 1, nullptr},
    {"move.b (8,a0,d1.w),d2", {0x1430, 0x1008}, 1, nullptr},
    {"move.l (8,a0,d1.l*4),d2", {0x2430, 0x1c08}, 1, nullptr},
    {"move.l ([$10,a0],d1.w*4,2),d2", {0x2430, 0x1526, 0x0010, 0x0002}, 1, nullptr},
    {"move.l ([0,a0,d1.w*4],2),d2", {0x2430, 0x1522, 0x0000, 0x0002}, 1, nullptr},
    {"move.l ($10.l,a0,d1.w),d2", {0x2430, 0x1130, 0x0000, 0x0010}, 1, nullptr},
    {"move.w (d16,pc),d0", {0x303a, 0x0100}, 1, nullptr},
    {"move.w (d8,pc,d1.w),d0", {0x303b, 0x1010}, 1, nullptr},
    {"move.l ([$10,pc],d1.w,2),d0", {0x203b, 0x1126, 0x0010, 0x0002}, 1, nullptr},
    {"move.l (xxx).w,d0", {0x2038, 0x3000}, 1, nullptr},
    {"move.l (xxx).l,d0", {0x2039, 0x0000, 0x3004}, 1, nullptr},
    {"move.w d0,(xxx).l", {0x33c0, 0x0000, 0x3008}, 1, nullptr},
    {"move.l (a0)+,(a1)+", {0x22d8}, 1, nullptr},
    {"move.b #imm,(d16,a1)", {0x137c, 0x00ab, 0x0004}, 1, nullptr},
    {"move.l (8,a0,d1.w),(4,a1,d4.w)", {0x23b0, 0x1008, 0x4004}, 1, nullptr},
    {"movea.w (a0),a2", {0x3450}, 1, nullptr},
    {"movea.l #imm,a2", {0x247c, 0x0000, 0x3100}, 1, nullptr},
    {"moveq #$42,d0", {0x7042}, 1, nullptr},
    {"lea (d16,a0),a1", {0x43e8, 0x0020}, 1, nullptr},
    {"lea (8,a0,d1.w),a1", {0x43f0, 0x1008}, 1, nullptr},
    {"pea (a0)", {0x4850}, 1, nullptr},
    {"pea (xxx).l", {0x4879, 0x0000, 0x1234}, 1, nullptr},
    // ---- 演算 ----
    {"add.l (a0)+,d0", {0xd098}, 1, nullptr},
    {"add.w d0,(a0)", {0xd150}, 1, nullptr},
    {"add.l d0,-(a1)", {0xd1a1}, 1, nullptr},
    {"sub.b (d16,a0),d0", {0x9028, 0x0003}, 1, nullptr},
    {"and.l d0,(d16,a0)", {0xc1a8, 0x0004}, 1, nullptr},
    {"cmp.w (8,a0,d1.w),d0", {0xb070, 0x1008}, 1, nullptr},
    {"ori.w #imm,(a0)", {0x0050, 0x00f0}, 1, nullptr},
    {"addi.l #imm,d0", {0x0680, 0x0001, 0x0000}, 1, nullptr},
    {"subi.b #1,(a0)+", {0x0418, 0x0001}, 1, nullptr},
    {"cmpi.l #imm,(a0)", {0x0c90, 0x1234, 0x5678}, 1, nullptr},
    {"cmpi.w #imm,d0", {0x0c40, 0x0203}, 1, nullptr},
    {"eori.l #imm,-(a1)", {0x0aa1, 0xffff, 0x0000}, 1, nullptr},
    {"addq.l #1,(a0)", {0x5290}, 1, nullptr},
    {"addq.w #8,a0", {0x5048}, 1, nullptr},
    {"subq.b #3,d0", {0x5700}, 1, nullptr},
    {"adda.w (a0),a1", {0xd2d0}, 1, nullptr},
    {"suba.l #imm,a1", {0x93fc, 0x0000, 0x0010}, 1, nullptr},
    {"cmpa.l (a0),a1", {0xb3d0}, 1, nullptr},
    {"cmpm.w (a0)+,(a1)+", {0xb348}, 1, nullptr},
    {"addx.l -(a0),-(a1)", {0xd388}, 1, nullptr},
    {"subx.w d0,d1", {0x9340}, 1, nullptr},
    {"neg.w (a0)", {0x4450}, 1, nullptr},
    {"clr.l (a0)+", {0x4298}, 1, nullptr},
    {"not.b d0", {0x4600}, 1, nullptr},
    {"tst.l (a0)", {0x4a90}, 1, nullptr},
    {"tst.w (d16,a0)", {0x4a68, 0x0002}, 1, nullptr},
    {"tas (a0)", {0x4ad0}, 1, nullptr},
    {"st (a0)", {0x50d0}, 1, nullptr},
    {"sne d0", {0x56c0}, 1, nullptr},
    {"exg d0,a1", {0xc189}, 1, nullptr},
    {"swap d0", {0x4840}, 1, nullptr},
    {"ext.l d0", {0x48c0}, 1, nullptr},
    // ---- ビットとシフト ----
    {"btst d1,(a0)", {0x0310}, 1, nullptr},
    {"bset d1,d0", {0x03c0}, 1, nullptr},
    {"bclr #3,(a0)", {0x0890, 0x0003}, 1, nullptr},
    {"btst #7,(d16,a0)", {0x0828, 0x0007, 0x0002}, 1, nullptr},
    {"btst d1,#imm", {0x033c, 0x00ff}, 1, nullptr},
    {"lsl.l #8,d0", {0xe188}, 1, nullptr},
    {"asr.w d1,d0", {0xe260}, 1, nullptr},
    {"roxl.l #1,d0", {0xe390}, 1, nullptr},
    {"ror.b d4,d0", {0xe838}, 1, nullptr},
    {"lsl.w (a0)", {0xe3d0}, 1, nullptr},
    {"asr.w (d16,a0)", {0xe0e8, 0x0002}, 1, nullptr},
    // ---- 乗算と除算 ----
    {"mulu.w (a0),d0", {0xc0d0}, 1, nullptr},
    {"muls.w d1,d0", {0xc1c1}, 1, nullptr},
    {"divu.w d4,d0", {0x80c4}, 1, nullptr},
    {"divs.w (a0),d2", {0x85d0}, 1, nullptr},
    {"divu.w d4,d7 (overflow)", {0x8ec4}, 1, nullptr},
    {"divs.w d4,d7 (overflow)", {0x8fc4}, 1, nullptr},
    {"divu.w #0,d0", {0x80fc, 0x0000}, 1, nullptr},
    {"mulu.l d1,d0", {0x4c01, 0x0000}, 1, nullptr},
    {"muls.l (a0),d1:d0", {0x4c10, 0x0c01}, 1, nullptr},
    {"divu.l d4,d0", {0x4c44, 0x0000}, 1, nullptr},
    {"divs.l d4,d7", {0x4c44, 0x7807}, 1, nullptr},
    {"divu.l d4,d1:d0", {0x4c44, 0x0401}, 1, nullptr},
    {"divu.l d4,d7:d0 (overflow)", {0x4c44, 0x0407}, 1, nullptr},
    // ---- 分岐 ----
    {"bra.s", {0x6004}, 1, nullptr},
    {"bra.w", {0x6000, 0x0010}, 1, nullptr},
    {"bra.l", {0x60ff, 0x0000, 0x0010}, 1, nullptr},
    {"bra.s (odd)", {0x6003}, 1, nullptr},
    {"beq.s (not taken)", {0x6704}, 1, nullptr},
    {"bne.s (taken)", {0x6604}, 1, nullptr},
    {"bne.s (backward)", {0x66fc}, 1, nullptr},
    {"bne.w (taken)", {0x6600, 0x0010}, 1, nullptr},
    {"beq.w (not taken)", {0x6700, 0x0010}, 1, nullptr},
    {"beq.l (not taken)", {0x67ff, 0x0000, 0x0010}, 1, nullptr},
    {"bne.s x4 (loop)", {0x4e71, 0x4e71, 0x66fa}, 9, nullptr},
    {"bsr.s", {0x6104}, 1, nullptr},
    {"bsr.w", {0x6100, 0x0010}, 1, nullptr},
    {"dbra d4 (loop)", {0x51cc, 0xfffe}, 7, nullptr},
    {"dbra d3 (expire)", {0x51cb, 0x0010}, 1, zeroLowD3},
    {"cmp;dbeq (true)", {0xb040, 0x57cc, 0x0010}, 2, nullptr},
    {"dbne d4 (true)", {0x56cc, 0x0010}, 1, nullptr},
    {"jmp (a0)", {0x4ed0}, 1, nullptr},
    {"jmp (d16,pc)", {0x4efa, 0x0010}, 1, nullptr},
    {"jmp (xxx).l", {0x4ef9, 0x0000, 0x2000}, 1, nullptr},
    {"jmp (xxx).l (odd)", {0x4ef9, 0x0000, 0x2001}, 1, nullptr},
    {"jsr (d16,a0)", {0x4ea8, 0x0010}, 1, nullptr},
    {"jsr (xxx).w", {0x4eb8, 0x2000}, 1, nullptr},
    {"jsr (xxx).l (odd)", {0x4eb9, 0x0000, 0x2001}, 1, nullptr},
    {"jsr (a0);rts", {0x4e90}, 2, rtsAt3000},
    {"rts", {0x4e75}, 1, stackRts},
    {"rts (odd)", {0x4e75}, 1, stackRtsOdd},
    {"rtd #8", {0x4e74, 0x0008}, 1, stackRts},
    {"rtr", {0x4e77}, 1, stackEven},
    {"link a6,#-8", {0x4e56, 0xfff8}, 1, nullptr},
    {"link.l a6,#-8", {0x480e, 0xffff, 0xfff8}, 1, nullptr},
    {"unlk a6", {0x4e5e}, 1, nullptr},
    {"nop x3", {0x4e71, 0x4e71, 0x4e71}, 3, nullptr},
    // ---- MOVEM ----
    {"movem.l d0-d7/a0-a6,-(a7)", {0x48e7, 0xfffe}, 1, nullptr},
    {"movem.l (a7)+,d0-d3", {0x4cdf, 0x000f}, 1, stackEven},
    {"movem.w (d16,a0),d0-d3/a1", {0x4ca8, 0x020f, 0x0010}, 1, nullptr},
    {"movem.l d0-d3,(a0)", {0x48d0, 0x000f}, 1, nullptr},
    {"movem.l d0-d1,(8,a0,d4.w)", {0x48f0, 0x0003, 0x4008}, 1, nullptr},
    {"movem.l (none),(a0)", {0x48d0, 0x0000}, 1, nullptr},
    // ---- 分けた命令の -(An) と (d16,An) ----
    // (An) と Dn では、68060 用の処理と両方の機種用の処理が同じ時間になる命令がある (eaAddr() の (An) は tick() を
    // 呼ばない)。-(An) は eaAddr() の tick(2)、(d16,An) は拡張語の fetch16() の tick(4) で違いが出る
    {"tas -(a0)", {0x4ae0}, 1, nullptr},
    {"tas (d16,a0)", {0x4ae8, 0x0004}, 1, nullptr},
    {"lsl.w -(a0)", {0xe3e0}, 1, nullptr},
    {"lsl.w (d16,a0)", {0xe3e8, 0x0004}, 1, nullptr},
    {"asr.w -(a0)", {0xe0e0}, 1, nullptr},
    {"roxr.w -(a0)", {0xe4e0}, 1, nullptr},
    {"ror.w (d16,a0)", {0xe6e8, 0x0004}, 1, nullptr},
    {"cmpa.l -(a0),a1", {0xb3e0}, 1, nullptr},
    {"cmpa.w (d16,a0),a1", {0xb2e8, 0x0004}, 1, nullptr},
    {"addq.l #1,-(a0)", {0x52a0}, 1, nullptr},
    {"addq.w #2,(d16,a0)", {0x5468, 0x0004}, 1, nullptr},
    {"subq.b #3,-(a0)", {0x5720}, 1, nullptr},
    {"subq.l #1,(d16,a0)", {0x53a8, 0x0004}, 1, nullptr},
    {"neg.w -(a0)", {0x4460}, 1, nullptr},
    {"neg.l (d16,a0)", {0x44a8, 0x0004}, 1, nullptr},
    {"clr.w -(a0)", {0x4260}, 1, nullptr},
    {"clr.b (d16,a0)", {0x4228, 0x0004}, 1, nullptr},
    {"not.l -(a0)", {0x46a0}, 1, nullptr},
    {"not.w (d16,a0)", {0x4668, 0x0004}, 1, nullptr},
    {"st -(a0)", {0x50e0}, 1, nullptr},
    {"sne (d16,a0)", {0x56e8, 0x0004}, 1, nullptr},
    // ---- 分けていない命令 (両方の機種で使える処理のまま) ----
    {"ori #0,sr", {0x007c, 0x0000}, 1, nullptr},
    {"move sr,d0", {0x40c0}, 1, nullptr},
    {"move (a0),ccr", {0x44d0}, 1, nullptr},
    {"move d0,sr", {0x46c0}, 1, nullptr},
    {"movec vbr,d0", {0x4e7a, 0x0801}, 1, nullptr},
    {"bfextu (a0){4:8},d0", {0xe9d0, 0x0108}, 1, nullptr},
    {"chk.w d4,d0", {0x4184}, 1, nullptr},
    {"abcd d0,d1", {0xc300}, 1, nullptr},
    {"pack -(a0),-(a1),#1", {0x8348, 0x0001}, 1, nullptr},
    {"cas.w d1,d2,(a0)", {0x0cd0, 0x0081}, 1, nullptr},
    {"trap #0", {0x4e40}, 1, nullptr},
    {"trap #0;handler", {0x4e40, 0x4e71}, 4, nullptr},
    {"rte", {0x4e73}, 1, rteFrame},
    {"illegal", {0x4afc}, 1, nullptr},
    // 68060 にない命令 (68060 では例外。肩代わりするときは 68030 の表の処理を呼ぶ)
    {"movep.w d0,(4,a0)", {0x0188, 0x0004}, 1, nullptr},
    {"cmp2.w (a0),d0", {0x02d0, 0x0000}, 1, nullptr},
    // ---- FPU (fpu.cpp が xeaAddr() / xfetch16() / xfetch32() を使う) ----
    {"fmove.l (d16,a0),fp0", {0xf228, 0x4000, 0x0010}, 1, nullptr},
    {"fmove.l #imm,fp0", {0xf23c, 0x4000, 0x0000, 0x0005}, 1, nullptr},
    {"fadd.d (8,a0,d1.w),fp0", {0xf230, 0x5422, 0x1008}, 1, nullptr},
    {"fmove.l #5,fp0;fmove.x fp0,-(a1)", {0xf23c, 0x4000, 0x0000, 0x0005, 0xf221, 0x6800}, 2, nullptr},
    // FBcc: 変位を xfetch16() / xfetch32() で読む
    {"fbt.w", {0xf28f, 0x0010}, 1, nullptr},
    {"fbt.l", {0xf2cf, 0x0000, 0x0010}, 1, nullptr},
    {"fbf.l", {0xf2c0, 0x0000, 0x0010}, 1, nullptr},
    {"fmove.l #0,fp0;fbeq.l", {0xf23c, 0x4000, 0x0000, 0x0000, 0xf2c1, 0x0000, 0x0010}, 2, nullptr},
};
constexpr int CASES = (int)(sizeof cases / sizeof cases[0]);

struct Result {
    uint64_t sub;
    uint32_t hash;
};

uint32_t fnv(uint32_t h, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        h = (h ^ (v & 0xff)) * 16777619u;
        v >>= 8;
    }
    return h;
}

Result runCase(const Case& k, int run) {
    const bool is060 = run >= 3;
    const bool fast = run != 2 && run != 4;
    Env e(fast, is060 ? CpuModel::M68060 : CpuModel::M68030);
    Cpu020& c = e.cpu;
    if (run == 1) c.setSpeed(200);
    if (run == 5) {
        c.cacr_ = 0x80008000u; // EDC と EIC
        c.updateCacheCosts();
    }
    // $2000-$6FFF を決まった模様で埋め、メモリ間接で読むポインタを置く
    for (uint32_t a = 0x2000; a < 0x7000; a++) e.bus.mem[a] = (uint8_t)(a * 7 + 3 + (a >> 8));
    e.bus.w32(0x3010, 0x00004020);
    e.bus.w32(0x3020, 0x00004040);
    e.bus.w32(0x1012, 0x00004060); // ([$10,pc]) で読むポインタ
    static const uint32_t d[8] = {0x00010203, 0x00000004, 0x11223344, 0x80000001, 5, 0xffff0003, 0x0000ffff, 0x12345678};
    static const uint32_t a[7] = {0x3000, 0x4000, 0x3100, 0x4100, 0x5000, 0x5800, 0x6000};
    for (int i = 0; i < 8; i++) c.d_[i] = d[i];
    for (int i = 0; i < 7; i++) c.a_[i] = a[i];
    uint32_t p = 0x1000;
    for (uint16_t w : k.words) {
        e.bus.w16(p, w);
        p += 2;
    }
    if (k.setup) k.setup(e);
    const uint64_t start = c.sub_;
    e.steps(k.steps);
    Result r;
    r.sub = c.sub_ - start;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 8; i++) h = fnv(h, c.d_[i]);
    for (int i = 0; i < 8; i++) h = fnv(h, c.a_[i]);
    h = fnv(h, c.pc_);
    h = fnv(h, c.getSR());
    h = fnv(h, c.usp_);
    h = fnv(h, c.isp_);
    h = fnv(h, c.msp_);
    for (uint32_t m = 0x2000; m < 0x9000; m += 4) h = fnv(h, e.bus.r32(m));
    for (uint32_t m = COUNTS; m < COUNTS + 0x400; m += 4) h = fnv(h, e.bus.r32(m));
    r.hash = h;
    return r;
}

// 分ける前のコアで取った値 (はじめの 141 行は b7cc8c5、あとから足した行は 4f9c119。どちらも #170 より前)。cpu020_cycles_test --print の出力
const Golden golden[] = {
#include "cpu020_cycles_golden.inc"
};
constexpr int GOLDENS = (int)(sizeof golden / sizeof golden[0]);

} // namespace

int main(int argc, char** argv) {
    const bool print = argc > 1 && !strcmp(argv[1], "--print");
    if (print) {
        for (const Case& k : cases) {
            Result r[RUNS];
            for (int run = 0; run < RUNS; run++) r[run] = runCase(k, run);
            printf("{\"%s\",\n {", k.name);
            for (int run = 0; run < RUNS; run++) printf("%s%llu", run ? ", " : "", (unsigned long long)r[run].sub);
            printf("},\n {");
            for (int run = 0; run < RUNS; run++) printf("%s0x%08x", run ? ", " : "", r[run].hash);
            printf("}},\n");
        }
        return 0;
    }
    CHECK_EQ(GOLDENS, CASES);
    int bad = 0;
    for (int i = 0; i < CASES && i < GOLDENS; i++) {
        const Case& k = cases[i];
        const Golden& g = golden[i];
        if (strcmp(k.name, g.name)) {
            printf("  golden %d は \"%s\" (命令の列は \"%s\")\n", i, g.name, k.name);
            failures++;
            continue;
        }
        for (int run = 0; run < RUNS; run++) {
            const Result r = runCase(k, run);
            if (r.sub == g.sub[run] && r.hash == g.hash[run]) continue;
            if (bad++ < 40)
                printf("  %s (動かし方 %d): サイクル数 %llu (golden %llu)、結果 %08x (golden %08x)\n", k.name, run,
                       (unsigned long long)r.sub, (unsigned long long)g.sub[run], r.hash, g.hash[run]);
        }
    }
    CHECK_EQ(bad, 0);
    // 68030 の速さ 100 では、どの命令も時間が進む (tick() が消えていれば 0 になる命令がある)
    for (int i = 0; i < GOLDENS; i++) CHECK_EQ(golden[i].sub[0] != 0, true);
    return finish();
}
