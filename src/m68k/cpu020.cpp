// MC68030 / MC68060 の CPU コア: レジスタ、メモリ、例外、実効アドレス、実行ループ
#include "m68k/cpu020.h"

#include <string.h>
#ifdef PIX68K_UNDO_CHECK
#include <stdio.h>
#include <stdlib.h>
#endif

#include "m68k/fpu.h"
#include "m68k/m68k_internal.h"

#include "m68k/cpu060_cycles.inc"

#define C020_HOT CPU020_INLINE

namespace m68k {

Cpu020::Handler Cpu020::table030_[65536];
Cpu020::Op060 Cpu020::ops060_[65536];
bool Cpu020::tablesReady_ = false;
uint8_t Cpu020::openBusRead_[4096];
uint8_t Cpu020::openBusWrite_[4096];

namespace {
C020_HOT uint16_t ld16(uintptr_t p) {
    const uint8_t* b = (const uint8_t*)p;
    return (uint16_t)((b[0] << 8) | b[1]);
}
// ロングの読み書きは __builtin_memcpy で書く。ベアメタル版 (Circle) は -ffreestanding でビルドするので、memcpy と書くと
// 4 バイトでも Circle の memcpy() の呼び出しになる (128 バイト未満は 1 バイトずつ写すループ)
C020_HOT uint32_t ld32(uintptr_t p) {
    uint32_t v;
    __builtin_memcpy(&v, (const void*)p, 4);
    return __builtin_bswap32(v);
}
C020_HOT void st16(uintptr_t p, uint16_t v) {
    uint8_t* b = (uint8_t*)p;
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}
C020_HOT void st32(uintptr_t p, uint32_t v) {
    v = __builtin_bswap32(v);
    __builtin_memcpy((void*)p, &v, 4);
}
} // namespace

Cpu020::Cpu020(Bus* bus, CpuModel model) : bus_(bus), model_(model) {
    if (!tablesReady_) {
        buildTables();
        buildCycles060();
        memset(openBusRead_, 0xff, sizeof openBusRead_);
        tablesReady_ = true;
    }
    // 68030 は、68000 の 1 サイクルを 3/5 サイクルとする (概算)。68060 は命令ごとの表 (cpu060_cycles.inc) で数える
    baseCost_ = cost_ = SUB * 3 / 5;
    setSpeed(100);
    clearFastPages();
    for (int i = 0; i < 8; i++) d_[i] = a_[i] = 0;
    fpu_ = new Fpu(this);
}

Cpu020::~Cpu020() { delete fpu_; }

// ---------------------------------------------------------------- SR

uint16_t Cpu020::getSR() const {
    return (uint16_t)((trace_ << 14) | (sflag_ ? 0x2000 : 0) | (mflag_ ? 0x1000 : 0) | (imask_ << 8) | (flagX_ << 4) |
                      (flagN_ << 3) | (flagZ_ << 2) | (flagV_ << 1) | flagC_);
}

void Cpu020::setCCR(uint8_t ccr) {
    flagX_ = (ccr >> 4) & 1;
    flagN_ = (ccr >> 3) & 1;
    flagZ_ = (ccr >> 2) & 1;
    flagV_ = (ccr >> 1) & 1;
    flagC_ = ccr & 1;
}

void Cpu020::setSR(uint16_t sr) {
    if (is060()) sr &= 0xa71f; // 68060 には T0 と M がない
    if (sflag_) {
        if (mflag_) msp_ = a_[7];
        else isp_ = a_[7];
    } else {
        usp_ = a_[7];
    }
    bool oldS = sflag_;
    trace_ = (sr >> 14) & 3;
    setSupervisor((sr & 0x2000) != 0);
    mflag_ = (sr & 0x1000) != 0;
    imask_ = (sr >> 8) & 7;
    setCCR((uint8_t)sr);
    a_[7] = sflag_ ? (mflag_ ? msp_ : isp_) : usp_;
    flow_ = true; // SR を書く命令は T0 のトレースの対象
    attn_ = 1;    // 割り込みマスクとトレースが変わる
    if (oldS != sflag_) fetchPage_ = TLB_INVALID;
}

void Cpu020::getRegisters(Registers& r) const {
    for (int i = 0; i < 8; i++) r.d[i] = d_[i];
    for (int i = 0; i < 7; i++) r.a[i] = a_[i];
    if (sflag_) {
        r.ssp = a_[7];
        r.usp = usp_;
    } else {
        r.usp = a_[7];
        r.ssp = mflag_ ? msp_ : isp_;
    }
    r.sr = getSR();
    r.pc = pc_;
    r.prefetch[0] = r.prefetch[1] = 0;
}

void Cpu020::setRegisters(const Registers& r) {
    for (int i = 0; i < 8; i++) d_[i] = r.d[i];
    for (int i = 0; i < 7; i++) a_[i] = r.a[i];
    uint16_t sr = is060() ? (r.sr & 0xa71f) : r.sr;
    trace_ = (sr >> 14) & 3;
    setSupervisor((sr & 0x2000) != 0);
    mflag_ = (sr & 0x1000) != 0;
    imask_ = (sr >> 8) & 7;
    setCCR((uint8_t)sr);
    usp_ = r.usp;
    if (mflag_) msp_ = r.ssp;
    else isp_ = r.ssp;
    a_[7] = sflag_ ? r.ssp : r.usp;
    pc_ = r.pc;
    halted_ = stopped_ = false;
    inException_ = false;
    // 68060 でアドレスエラーのフレームを積んでいる途中に止まったあと (バスエラーで halted_ になり、longjmp で抜けた) は
    // 立ったまま残っている。消さないと、次のアドレスエラーがダブルフォルトになる
    inAddrErr_ = false;
    fetchPage_ = TLB_INVALID;
    attn_ = 1;
}

// ---------------------------------------------------------------- メモリ

void Cpu020::setFastPage(bool supervisor, int page, const uint8_t* read, uint8_t* write, bool flush) {
    page &= FAST_PAGES - 1;
    fastRead_[supervisor ? 1 : 0][page] = read;
    fastWrite_[supervisor ? 1 : 0][page] = write;
    if (flush) flushTlb();
}

void Cpu020::clearFastPages() {
    for (int s = 0; s < 2; s++) {
        for (int i = 0; i < FAST_PAGES; i++) {
            fastRead_[s][i] = nullptr;
            fastWrite_[s][i] = nullptr;
        }
    }
    flushTlb();
}

void Cpu020::setExtMemory(uint32_t start, uint32_t size, uint8_t* mem) {
    extStart_ = start;
    extSize_ = mem ? size : 0;
    ext_ = mem;
    flushTlb();
}

void Cpu020::flushTlb() {
    selectDataTlb();
    flushAtc();
    for (int k = 0; k < 3; k++)
        for (int s = 0; s < 2; s++)
            for (int i = 0; i < TLB_SIZE; i++) tlb_[k][s][i].tag = TLB_INVALID;
    fetchPage_ = TLB_INVALID;
}

// 物理アドレスのページ (4KB) をホストのメモリで直接読み書きできるならそのポインタ
const uint8_t* Cpu020::physPtr(uint32_t pa, bool write, bool super) const {
    if (pa - extStart_ < extSize_) return ext_ + (pa - extStart_);
    if (pa - holeStart_ < holeSize_) return write ? openBusWrite_ : openBusRead_;
    int s = super ? 1 : 0;
    const uint8_t* p = write ? fastWrite_[s][(pa >> 16) & 0xff] : fastRead_[s][(pa >> 16) & 0xff];
    return p ? p + (pa & 0xf000) : nullptr;
}

// TLB にページを入れる。直接読み書きできないページなら false
bool Cpu020::tlbFill(int kind, uint32_t addr, int fc, TlbEntry& e) {
    bool super = (fc & 4) != 0;
    uint32_t pa = translate(addr, fc, kind == TLB_WRITE, kind == TLB_FETCH) & ~0xfffu;
    const uint8_t* p = physPtr(pa, kind == TLB_WRITE, super);
    if (!p) return false;
    e.tag = addr >> 12;
    e.base = (uintptr_t)p - (uintptr_t)(addr & ~0xfffu);
    return true;
}


// TLB に当たれば直接読み書きする。ワードとロングは、ページをまたがないことも要る。またぐかどうかは、アクセスの
// 最後のバイトのページ ((a + 大きさ - 1) >> 12) をタグと比べれば 1 回の比較で分かる: 表の位置は先頭のバイトのページで
// 決めるので、タグが最後のバイトのページと同じなのは、当たっていて、先頭と最後が同じページのときだけ
// (またぐと最後のページは先頭 + 1 で、その位置のタグにはならない。使っていない位置のタグ $FFFFFFFF にもならない)
#define C020_TLB_HIT(kind, s, a, size) (tlb_[kind][s][((a) >> 12) & (TLB_SIZE - 1)].tag == (((a) + ((size) - 1)) >> 12))
#define C020_TLB_BASE(kind, s, a) (tlb_[kind][s][((a) >> 12) & (TLB_SIZE - 1)].base)

C020_HOT uint8_t Cpu020::memRead8(uint32_t a, int fc) {
    const unsigned s = (fc >> 2) & 1;
    if (C020_TLB_HIT(TLB_READ, s, a, 1)) {
        tickData16(a);
        return *(const uint8_t*)(C020_TLB_BASE(TLB_READ, s, a) + a);
    }
    return slowRead8(a, fc);
}

C020_HOT uint16_t Cpu020::memRead16(uint32_t a, int fc) {
    const unsigned s = (fc >> 2) & 1;
    if (C020_TLB_HIT(TLB_READ, s, a, 2)) {
        tickData16(a);
        return ld16(C020_TLB_BASE(TLB_READ, s, a) + a);
    }
    return slowRead16(a, fc);
}

C020_HOT uint32_t Cpu020::memRead32(uint32_t a, int fc) {
    const unsigned s = (fc >> 2) & 1;
    if (C020_TLB_HIT(TLB_READ, s, a, 4)) {
        tickData32(a);
        return ld32(C020_TLB_BASE(TLB_READ, s, a) + a);
    }
    return slowRead32(a, fc);
}

C020_HOT void Cpu020::memWrite8(uint32_t a, uint8_t v, int fc) {
    const unsigned s = (fc >> 2) & 1;
    if (C020_TLB_HIT(TLB_WRITE, s, a, 1)) {
        tickData16(a);
        *(uint8_t*)(C020_TLB_BASE(TLB_WRITE, s, a) + a) = v;
        return;
    }
    slowWrite8(a, v, fc);
}

C020_HOT void Cpu020::memWrite16(uint32_t a, uint16_t v, int fc) {
    const unsigned s = (fc >> 2) & 1;
    if (C020_TLB_HIT(TLB_WRITE, s, a, 2)) {
        tickData16(a);
        st16(C020_TLB_BASE(TLB_WRITE, s, a) + a, v);
        return;
    }
    slowWrite16(a, v, fc);
}

C020_HOT void Cpu020::memWrite32(uint32_t a, uint32_t v, int fc) {
    const unsigned s = (fc >> 2) & 1;
    if (C020_TLB_HIT(TLB_WRITE, s, a, 4)) {
        tickData32(a);
        st32(C020_TLB_BASE(TLB_WRITE, s, a) + a, v);
        return;
    }
    slowWrite32(a, v, fc);
}

// データ空間 (今のモードの FC) の読み書き。memRead8(a, fcData()) などと同じ結果で、
// S が変わったときに選んだ dataReadTlb_ / dataWriteTlb_ を使う。
C020_HOT uint8_t Cpu020::dataRead8(uint32_t a) {
    const TlbEntry& e = dataReadTlb_[(a >> 12) & (TLB_SIZE - 1)];
    if (e.tag == (a >> 12)) {
        tickData16(a);
        return *(const uint8_t*)(e.base + a);
    }
    return slowRead8(a, fcData());
}
C020_HOT uint16_t Cpu020::dataRead16(uint32_t a) {
    const TlbEntry& e = dataReadTlb_[(a >> 12) & (TLB_SIZE - 1)];
    if (e.tag == ((a + 1u) >> 12)) {
        tickData16(a);
        return ld16(e.base + a);
    }
    return slowRead16(a, fcData());
}
C020_HOT uint32_t Cpu020::dataRead32(uint32_t a) {
    const TlbEntry& e = dataReadTlb_[(a >> 12) & (TLB_SIZE - 1)];
    if (e.tag == ((a + 3u) >> 12)) {
        tickData32(a);
        return ld32(e.base + a);
    }
    return slowRead32(a, fcData());
}
C020_HOT void Cpu020::dataWrite8(uint32_t a, uint8_t v) {
    const TlbEntry& e = dataWriteTlb_[(a >> 12) & (TLB_SIZE - 1)];
    if (e.tag == (a >> 12)) {
        tickData16(a);
        *(uint8_t*)(e.base + a) = v;
        return;
    }
    slowWrite8(a, v, fcData());
}
C020_HOT void Cpu020::dataWrite16(uint32_t a, uint16_t v) {
    const TlbEntry& e = dataWriteTlb_[(a >> 12) & (TLB_SIZE - 1)];
    if (e.tag == ((a + 1u) >> 12)) {
        tickData16(a);
        st16(e.base + a, v);
        return;
    }
    slowWrite16(a, v, fcData());
}
C020_HOT void Cpu020::dataWrite32(uint32_t a, uint32_t v) {
    const TlbEntry& e = dataWriteTlb_[(a >> 12) & (TLB_SIZE - 1)];
    if (e.tag == ((a + 3u) >> 12)) {
        tickData32(a);
        st32(e.base + a, v);
        return;
    }
    slowWrite32(a, v, fcData());
}

C020_HOT uint8_t Cpu020::read8(uint32_t a) { return dataRead8(a); }
C020_HOT uint16_t Cpu020::read16(uint32_t a) { return dataRead16(a); }
C020_HOT uint32_t Cpu020::read32(uint32_t a) { return dataRead32(a); }
C020_HOT void Cpu020::write8(uint32_t a, uint32_t v) { dataWrite8(a, (uint8_t)v); }
C020_HOT void Cpu020::write16(uint32_t a, uint32_t v) { dataWrite16(a, (uint16_t)v); }
C020_HOT void Cpu020::write32(uint32_t a, uint32_t v) { dataWrite32(a, v); }

template <int S> C020_HOT uint32_t Cpu020::readS(uint32_t a) {
    if (S == 1) return read8(a);
    if (S == 2) return read16(a);
    return read32(a);
}

template <int S> C020_HOT void Cpu020::writeS(uint32_t a, uint32_t v) {
    if (S == 1) write8(a, v);
    else if (S == 2) write16(a, v);
    else write32(a, v);
}

// Bus に出す物理アドレス。アクセスエラーのフレームのため、論理アドレスを覚えておく
uint32_t Cpu020::busAddr(uint32_t a, int fc, bool write, bool fetch) {
    logical_ = a;
    logicalWrite_ = write;
    return translate(a, fc, write, fetch);
}

// DF を 0 にした RTE のやり直しの間の読み書き (cpu020.h の replaySig_)。済ませたら bit32 を立て、読み出しの値を
// 下位 32 ビットに入れて返す。バスエラーになったアクセスなら、読み出しはデータ入力バッファの値、書き込みは捨てる
// (Bus に出したものとして記録する)。ほかのアクセスは、直接読み書きできるページなら TLB に入れずに読み書きする。
// できなければ 0 を返し、呼び出し側が Bus に出す (TLB には入らない)。呼び出し側の通常の経路を遅くしないよう、
// 別の関数にして値で返す
__attribute__((noinline, cold)) uint64_t Cpu020::replayAccess(uint32_t a, int fc, uint8_t cycle, bool write, uint32_t v) {
    const uint64_t done = 1ull << 32;
    busCycle_ = cycle;
    if (busSig(a, fc, cycle, write) == replaySig_ && replaySeen_++ == replayOrdinal_) {
        // 済ませたので、命令の残りのアクセスは比べない。消さなくても、replaySeen_ が replayOrdinal_ を超えるので
        // 2 度は当たらず、残りのアクセスが TLB に入らないだけで結果は同じ (消してもテストは落ちない)
        replayN_ = 0;
        noteBus(a, fc, cycle, write);
        return done | (write ? 0 : replayData_);
    }
    TlbEntry e;
    if (!tlbFill(write ? TLB_WRITE : TLB_READ, a, fc, e)) return 0;
    const uintptr_t p = e.base + a;
    if (write) {
        if (cycle == BUS_BYTE) *(uint8_t*)p = (uint8_t)v;
        else if (cycle == BUS_WORD) st16(p, (uint16_t)v);
        else st32(p, v);
        return done;
    }
    return done | (cycle == BUS_BYTE ? *(const uint8_t*)p : cycle == BUS_WORD ? ld16(p) : ld32(p));
}

uint8_t Cpu020::slowRead8(uint32_t a, int fc) {
    tickSlow16();
    TlbEntry& e = tlb_[TLB_READ][(fc >> 2) & 1][(a >> 12) & (TLB_SIZE - 1)];
    busCycle_ = BUS_BYTE; // 表を引く途中のアクセスエラーでも使うので、tlbFill() の前に入れる
    if (replayN_) {
        const uint64_t r = replayAccess(a, fc, BUS_BYTE, false, 0);
        if (r >> 32) return (uint8_t)r;
    }
    if (tlbFill(TLB_READ, a, fc, e)) {
        tickMiss(a);
        return *(const uint8_t*)(e.base + a);
    }
    noteBus(a, fc, BUS_BYTE, false);
    return busWaited()->read8(busAddr(a, fc, false), fc);
}

uint16_t Cpu020::slowRead16(uint32_t a, int fc) {
    if ((a & 0xfff) == 0xfff) { // ページをまたぐ
        beginSplit(a, BUS_WORD);
        uint16_t hi = memRead8(a, fc);
        uint16_t v = (uint16_t)((hi << 8) | memRead8(a + 1, fc));
        endSplit();
        return v;
    }
    tickSlow16();
    TlbEntry& e = tlb_[TLB_READ][(fc >> 2) & 1][(a >> 12) & (TLB_SIZE - 1)];
    busCycle_ = BUS_WORD;
    if (replayN_) {
        const uint64_t r = replayAccess(a, fc, BUS_WORD, false, 0);
        if (r >> 32) return (uint16_t)r;
    }
    if (tlbFill(TLB_READ, a, fc, e)) {
        tickMiss(a);
        return ld16(e.base + a);
    }
    noteBus(a, fc, BUS_WORD, false);
    uint32_t pa = busAddr(a, fc, false);
    if (a & 1) {
        uint16_t hi = busWaited()->read8(pa, fc);
        return (uint16_t)((hi << 8) | busWaited()->read8(pa + 1, fc));
    }
    return busWaited()->read16(pa, fc);
}

uint32_t Cpu020::slowRead32(uint32_t a, int fc) {
    if ((a & 0xfff) > 0xffc || (a & 1)) {
        beginSplit(a, BUS_LONG);
        uint32_t v;
        if (a & 1) {
            uint32_t b0 = memRead8(a, fc);
            uint32_t w = memRead16(a + 1, fc);
            v = (b0 << 24) | (w << 8) | memRead8(a + 3, fc);
        } else {
            uint32_t hi = memRead16(a, fc);
            v = (hi << 16) | memRead16(a + 2, fc);
        }
        endSplit();
        return v;
    }
    TlbEntry& e = tlb_[TLB_READ][(fc >> 2) & 1][(a >> 12) & (TLB_SIZE - 1)];
    busCycle_ = BUS_LONG;
    if (replayN_) {
        const uint64_t r = replayAccess(a, fc, BUS_LONG, false, 0);
        if (r >> 32) {
            tickSlow32();
            return (uint32_t)r;
        }
    }
    if (tlbFill(TLB_READ, a, fc, e)) {
        tickSlow32();
        tickMiss(a);
        return ld32(e.base + a);
    }
    tickSlow32();
    noteBus(a, fc, BUS_LONG, false);
    uint32_t pa = busAddr(a, fc, false);
    uint32_t hi = busWaited()->read16(pa, fc);
    return (hi << 16) | busWaited()->read16(pa + 2, fc);
}

void Cpu020::slowWrite8(uint32_t a, uint8_t v, int fc) {
    tickSlow16();
    TlbEntry& e = tlb_[TLB_WRITE][(fc >> 2) & 1][(a >> 12) & (TLB_SIZE - 1)];
    busCycle_ = BUS_BYTE;
    if (replayN_ && (replayAccess(a, fc, BUS_BYTE, true, v) >> 32)) return;
    if (tlbFill(TLB_WRITE, a, fc, e)) {
        tickMiss(a);
        *(uint8_t*)(e.base + a) = v;
        return;
    }
    noteBus(a, fc, BUS_BYTE, true);
    dob_ = v;
    busWaited()->write8(busAddr(a, fc, true), v, fc);
}

void Cpu020::slowWrite16(uint32_t a, uint16_t v, int fc) {
    if ((a & 0xfff) == 0xfff) {
        beginSplit(a, BUS_WORD);
        memWrite8(a, (uint8_t)(v >> 8), fc);
        memWrite8(a + 1, (uint8_t)v, fc);
        endSplit();
        return;
    }
    tickSlow16();
    TlbEntry& e = tlb_[TLB_WRITE][(fc >> 2) & 1][(a >> 12) & (TLB_SIZE - 1)];
    busCycle_ = BUS_WORD;
    if (replayN_ && (replayAccess(a, fc, BUS_WORD, true, v) >> 32)) return;
    if (tlbFill(TLB_WRITE, a, fc, e)) {
        tickMiss(a);
        st16(e.base + a, v);
        return;
    }
    noteBus(a, fc, BUS_WORD, true);
    dob_ = v;
    uint32_t pa = busAddr(a, fc, true);
    if (a & 1) {
        busWaited()->write8(pa, (uint8_t)(v >> 8), fc);
        busWaited()->write8(pa + 1, (uint8_t)v, fc);
        return;
    }
    busWaited()->write16(pa, v, fc);
}

void Cpu020::slowWrite32(uint32_t a, uint32_t v, int fc) {
    if ((a & 0xfff) > 0xffc || (a & 1)) {
        beginSplit(a, BUS_LONG);
        if (a & 1) {
            memWrite8(a, (uint8_t)(v >> 24), fc);
            memWrite16(a + 1, (uint16_t)(v >> 8), fc);
            memWrite8(a + 3, (uint8_t)v, fc);
        } else {
            memWrite16(a, (uint16_t)(v >> 16), fc);
            memWrite16(a + 2, (uint16_t)v, fc);
        }
        endSplit();
        return;
    }
    tickSlow32();
    TlbEntry& e = tlb_[TLB_WRITE][(fc >> 2) & 1][(a >> 12) & (TLB_SIZE - 1)];
    busCycle_ = BUS_LONG;
    if (replayN_ && (replayAccess(a, fc, BUS_LONG, true, v) >> 32)) return;
    if (tlbFill(TLB_WRITE, a, fc, e)) {
        tickMiss(a);
        st32(e.base + a, v);
        return;
    }
    noteBus(a, fc, BUS_LONG, true);
    dob_ = v;
    uint32_t pa = busAddr(a, fc, true);
    busWaited()->write16(pa, (uint16_t)(v >> 16), fc);
    busWaited()->write16(pa + 2, (uint16_t)v, fc);
}

// 命令の語を読む。PC は常に偶数 (奇数への分岐は jumpTo() でアドレスエラーにする)
template <bool M060> C020_HOT uint16_t Cpu020::fetch16() {
    uint32_t a = pc_;
    if ((a >> 12) == fetchPage_) {
        pc_ = a + 2;
        if (!M060) tick(4);
        return ld16(fetchBase_ + a);
    }
    return slowFetch16();
}

uint16_t Cpu020::slowFetch16() {
    uint32_t a = pc_;
    int fc = fcProg();
    TlbEntry& e = tlb_[TLB_FETCH][(fc >> 2) & 1][(a >> 12) & (TLB_SIZE - 1)];
    tick(4);
    busCycle_ = BUS_FETCH;
    if ((e.tag == (a >> 12)) || tlbFill(TLB_FETCH, a, fc, e)) {
        fetchPage_ = a >> 12;
        fetchBase_ = e.base;
        pc_ = a + 2;
        return ld16(e.base + a);
    }
    uint16_t v = busWaited()->read16(busAddr(a, fc, false, true), fc);
    pc_ = a + 2;
    return v;
}

template <bool M060> C020_HOT uint32_t Cpu020::fetch32() {
    const uint32_t a = pc_;
    // 同じ命令ページ内なら、2 語をまとめて読む。境界をまたぐときや Bus に出るときは、
    // 1 語目の PC とサイクル数を進めてから 2 語目を読む (途中の例外も従来どおり)。
    if ((a >> 12) == fetchPage_ && (a & 0xfff) <= 0xffc) {
        pc_ = a + 4;
        if (!M060) tick(8);
        return ld32(fetchBase_ + a);
    }
    uint32_t hi = fetch16<M060>();
    return (hi << 16) | fetch16<M060>();
}

uint8_t Cpu020::xread8(uint32_t a) { return read8(a); }
uint16_t Cpu020::xread16(uint32_t a) { return read16(a); }
uint32_t Cpu020::xread32(uint32_t a) { return read32(a); }
void Cpu020::xwrite8(uint32_t a, uint32_t v) { write8(a, v); }
void Cpu020::xwrite16(uint32_t a, uint32_t v) { write16(a, v); }
void Cpu020::xwrite32(uint32_t a, uint32_t v) { write32(a, v); }
uint32_t Cpu020::xeaAddr(int mode, int reg, int size) {
    return is060() ? eaAddr<true>(mode, reg, size) : eaAddr<false>(mode, reg, size);
}
uint16_t Cpu020::xfetch16() { return is060() ? fetch16<true>() : fetch16<false>(); }
uint32_t Cpu020::xfetch32() { return is060() ? fetch32<true>() : fetch32<false>(); }

// ---------------------------------------------------------------- 例外

void Cpu020::abortInstr() { M68K_LONGJMP(abort_, 1); }

CPU020_INLINE void Cpu020::jumpTo(uint32_t addr) {
    if (__builtin_expect(addr & 1, 0)) addressErrorFetch(addr);
    // 68060 で命令キャッシュが無効のとき: この命令の拡張語までの取り出しの分を、ここで足す (execOne() は、流れを変えた
    // 命令の次では飛び先のロングワードの 1 回だけを足すので、ここで足さないと拡張語の分が抜ける)
    if (__builtin_expect(fetchWaitOn_, 0)) fetchSeq060(pc_ - 2);
    pc_ = addr;
    flow_ = true;
}
void Cpu020::xjumpTo(uint32_t addr) { jumpTo(addr); }

// スーパーバイザモードに入り、例外のスタックを積んでベクタへ飛ぶ。format は 0、2、3、4、9
void Cpu020::exception(int vector, int format, uint32_t pushPc, uint32_t instrAddr, uint32_t ea) {
    stopped_ = false;
    replayN_ = 0; // やり直しの命令の中の例外: スタックに積む書き込みを、やり直しの記録と比べない
    uint16_t old = getSR();
    if (!sflag_) {
        usp_ = a_[7];
        a_[7] = mflag_ ? msp_ : isp_;
        setSupervisor(true);
        fetchPage_ = TLB_INVALID;
    }
    trace_ = 0;
    inException_ = true;
    tick(34);
    tick060(19); // MC68060UM 表 10-26 (例外の種類による 19 から 23 の違いは見ない)
    pairLead_ = 0;
    foldOk060_ = false;
    uint32_t sp = a_[7];
    if (format == 9) {
        // フォーマット $9 (10 語): +$08 命令のアドレス、+$0C〜+$13 内部レジスタ (+$10 に実効アドレス。MC68881UM 6.1.3)
        sp -= 4;
        write32(sp, ea);
        sp -= 4;
        write32(sp, 0);
    }
    if (format == 4) {
        // フォーマット $4 (68060): +$08 にフォールトアドレス (instrAddr)、+$0C に FSLW (ea)
        sp -= 4;
        write32(sp, ea);
    }
    if (format == 2 || format == 3 || format == 4 || format == 9) {
        sp -= 4;
        write32(sp, instrAddr);
    }
    sp -= 2;
    write16(sp, (uint16_t)((format << 12) | (vector << 2)));
    sp -= 4;
    write32(sp, pushPc);
    sp -= 2;
    write16(sp, old);
    a_[7] = sp;
    uint32_t v = read32(vbr_ + (uint32_t)vector * 4);
    inException_ = false;
    // ベクタが奇数でアドレスエラーになったときに積んだフレームを消さないよう、戻す値を今の値にしておく
    if (is060()) saveUndo();
    else clearUndoLog();
    jumpTo(v);
}

void Cpu020::illegal() {
    noTrace_ = true;
    if (onUnimplemented) onUnimplemented(onUnimplementedCtx, pc0_, op_, 4);
    exception(4, 0, pc0_);
}
void Cpu020::lineA() {
    noTrace_ = true;
    exception(10, 0, pc0_);
}
void Cpu020::lineF() {
    noTrace_ = true;
    if (onUnimplemented) onUnimplemented(onUnimplementedCtx, pc0_, op_, 11);
    exception(11, 0, pc0_);
}
void Cpu020::privilege() {
    noTrace_ = true;
    exception(8, 0, pc0_);
}
void Cpu020::trapFormat2(int vector) { exception(vector, 2, pc_, pc0_); }

void Cpu020::interrupt(int level) {
    stopped_ = false;
    uint16_t old = getSR();
    if (!sflag_) {
        usp_ = a_[7];
        a_[7] = mflag_ ? msp_ : isp_;
        setSupervisor(true);
        fetchPage_ = TLB_INVALID;
    }
    trace_ = 0;
    imask_ = level;
    tick(44);
    tick060(23); // MC68060UM 表 10-26
    pairLead_ = 0;
    foldOk060_ = false;
    int vec;
    if (M68K_SETJMP(iackAbort_)) {
        vec = 24;
    } else {
        inIack_ = true;
        vec = bus_->interruptAck(level);
    }
    inIack_ = false;
    if (vec == -1) vec = 24 + level;
    else if (vec < 0) vec = 24;
    inException_ = true;
    uint32_t sp = a_[7];
    sp -= 2;
    write16(sp, (uint16_t)(vec << 2));
    sp -= 4;
    write32(sp, pc_);
    sp -= 2;
    write16(sp, old);
    a_[7] = sp;
    if (mflag_) {
        // M=1 なら、マスタスタックに積んだあと M を 0 にし、割り込みスタックにフォーマット $1 (スローアウェイ) の
        // フレームを積む (MC68030UM 8.1.9)
        msp_ = a_[7];
        mflag_ = false;
        sp = isp_;
        sp -= 2;
        write16(sp, (uint16_t)(0x1000 | (vec << 2)));
        sp -= 4;
        write32(sp, pc_);
        sp -= 2;
        write16(sp, (uint16_t)(old | 0x2000)); // 割り込み前の SR に S を立てたもの (M=1 のまま。RTE でマスタスタックに戻る)
        a_[7] = sp;
    }
    uint32_t v = read32(vbr_ + (uint32_t)vec * 4);
    inException_ = false;
    if (is060()) saveUndo(); // exception() と同じ
    else clearUndoLog();
    jumpTo(v);
}

// 68030 のバスエラー・アドレスエラー (MC68030UM 8.1.2、8.2)
//
// 実機は命令の途中の状態をフォーマット $B のフレームの内部レジスタに積み、RTE で命令の途中から続ける。ここでは
// 命令の途中の状態は持たず、68060 と同じく、命令の途中で動かしたアドレスレジスタ (と一部の命令の CCR) を命令の
// 始めの値に戻してから (undoLog()) フレームを積み、RTE は命令をはじめからやり直す。(An)+ / -(An) などの命令で
// アドレスレジスタが 2 回進むことはない。
// 実機と同じに見えるもの:
// - 積む PC は実行中の命令の先頭 (表 8-6 の「Address of instruction in execution when fault occurred」)
// - SSW の DF が 1 のまま RTE すると、バスエラーになったアクセスをもう一度行う (ここでは命令ごとやり直す)
// - 処理ルーチンがデータの転送を済ませて DF を 0 にした場合 (8.2.2)、RTE はそのアクセスを行わない。読み出しなら
//   データ入力バッファ (+$2C) の値を使い、書き込みなら行ったものとする。ここでは命令をやり直し、そのアクセスだけを
//   Bus に出さない (resumeFaulted())。RM (TAS、CAS、CAS2) で DF が 0 なら、処理ルーチンが命令全体を済ませたとして
//   次の命令から続ける
// 実機と違うもの:
// - 短いフレーム ($A) は積まない。データのバスエラーはいつも長いフレーム ($B)
// - 命令の途中で行ったほかの読み書き (MOVEM の前のほうの語など) は、やり直しでもう一度行う
// - 1 つの命令にバスエラーになるアクセスが 2 つ以上あり、処理ルーチンが毎回 DF を 0 にして RTE すると、命令が
//   終わらない。データ入力バッファで済ませるのは最後のバスエラーのアクセスだけで、やり直しでは前のアクセスを
//   また Bus に出してバスエラーになるため (MOVE.L (A0),(A1) で両方、SUBX.B -(A0),-(A1) で A0 と A1 が同じ番地、
//   奇数番地の MOVE.L をバイト・ワード・バイトに分けたものなど)
// - ロングを 16 ビットずつ 2 回で Bus に出していて 2 回目がバスエラーになったときも、アドレスと大きさはロングの
//   アクセス全体のもの (DF が 0 ならロング全体を行わない。1 回目の書き込みは済んでいる)
// - 命令の読み込みのバスエラーで、処理ルーチンがステージ B / C を埋めて RB / RC を 0 にしても、それは使わず読み直す
// - RTS、RTD、RTR、RTE で A7 を動かしたあとに奇数番地へ飛んだアドレスエラーでは、A7 は戻さない (記録しない。
//   cpu020.h の logA())
// 実機と同じかどうか分からないもの:
// - 処理ルーチンから見えるアドレスレジスタは命令の始めの値 (MOVE.L (A0)+,D0 のバスエラーなら、A0 は進む前)。
//   MC68030UM 8 章は、命令の途中の状態は内部レジスタに積むとだけ書き、処理ルーチンから見える An の値を決めていない。
//   RTE で戻らず、バスエラーになった所を (An)+ の An から知るソフト (メモリの大きさを調べるものなど) では、
//   以前の版 (An を進めたまま積んでいた) と結果が変わりうる
// - RM (TAS、CAS、CAS2) で処理ルーチンが DF を 0 にしたとき、8.2.2 は処理ルーチンが命令全体を済ませ、CCR と
//   (CAS / CAS2 の) レジスタの写しを直すとしている。ここでは An を命令の始めの値に戻したまま次の命令へ進むので、
//   TAS (A0)+ や TAS -(A0) では、処理ルーチンが A0 も直さないと A0 は進まない
// 内部レジスタの領域には、ここで使う値を入れる (処理ルーチンは変えてはいけない場所。8.2.2)。どれも、内部レジスタの
// 領域を 0 で埋める書き込みの中で書く (書き込みの回数は変えない)。データ出力バッファ (+$18) は書き込みの
// バスエラーのときだけ積む (読み出しなら 0):
//   +$14 (ワード) bit0 が 1 ならデータのバスエラー、bit15-8 はそのアクセスと同じもの (アドレス、FC、大きさ、読み書き) が
//   命令の中で前に何回あったか (Cpu020::faultOrdinal())、+$1C (ロング) 次の命令のアドレス (RM で使う)
// ordinal が負ならデータのバスエラーではない
static void pushBusFault(Cpu020* c, int vector, uint32_t addr, uint16_t ssw, uint32_t stageB, int ordinal) {
    const bool dataFault = ordinal >= 0;
    const uint32_t dob = c->dob_; // フレームを積む書き込みで dob_ が変わる前に取っておく
    uint16_t old = c->getSR();
    if (!c->sflag_) {
        c->usp_ = c->a_[7];
        c->a_[7] = c->mflag_ ? c->msp_ : c->isp_;
        c->setSupervisor(true);
        c->fetchPage_ = Cpu020::TLB_INVALID;
    }
    c->trace_ = 0;
    c->tick(50);
    uint32_t sp = c->a_[7] - 92;
    // 内部レジスタの領域は 0 で埋める。データのバスエラーなら、そのとき同じループで +$14、+$18 (書き込みなら DOB)、
    // +$1C を書く (書き込みの回数を増やさない)
    for (int i = 12; i < 92; i += 4) {
        uint32_t v = 0;
        if (dataFault) {
            if (i == 0x14) v = (uint32_t)(ordinal << 8 | 1) << 16;
            else if (i == 0x18 && !(ssw & 0x40)) v = dob;
            else if (i == 0x1c) v = c->pc_;
        }
        c->write32(sp + i, v);
    }
    c->write32(sp + 0x24, stageB);
    c->write32(sp + 16, addr);
    c->write16(sp + 10, ssw);
    c->write16(sp + 8, 0);
    c->write16(sp + 6, (uint16_t)(0xb000 | (vector << 2)));
    c->write32(sp + 2, c->pc0_);
    c->write16(sp, old);
    c->a_[7] = sp;
    uint32_t v = c->read32(c->vbr_ + (uint32_t)vector * 4);
    c->inException_ = false;
    c->clearUndoLog(); // exception() と同じ (ベクタが奇数でアドレスエラーになっても、このフレームを残す)
    // バスエラー / アドレスエラーのベクタが奇数: この例外の処理の途中のアドレスエラーなので、フレームを積んだところで
    // 止まる (ダブルバスフォルト。MC68030UM 8.1.3)。
    // jumpTo() に渡すと addressErrorFetch() からここへ戻り、ベクタ 3 が奇数の間、フレームを積み続ける。
    // 呼び出し元 (busError()、addressErrorFetch()) が、このあと longjmp で命令を打ち切る
    if (v & 1) {
        c->halted_ = true;
        return;
    }
    c->jumpTo(v);
}

// TAS、CAS、CAS2 (読んで書く命令。SSW の RM)
static bool readModifyWrite(uint16_t op) {
    if ((op & 0xffc0) == 0x4ac0) return op != 0x4afc && (op & 0x38) != 0; // TAS <ea> (ILLEGAL と TAS Dn を除く)
    return (op & 0xf9c0) == 0x08c0 && (op & 0x0600) != 0;                   // CAS、CAS2
}

void Cpu020::busError(uint32_t addr, bool write, int fc) {
    if (inIack_) M68K_LONGJMP(iackAbort_, 1);
    if (inException_) {
        halted_ = true;
        M68K_LONGJMP(abort_, 1);
    }
    // フレームを積む書き込みは、やり直しの記録と比べない。消すと、まだ済ませていないやり直しのアクセス (アドレス、
    // FC、大きさ、読み書き、何回目か) とフレームの書き込みの 1 つが一致したときに、その書き込みが捨てられる。
    // そうなるのは、やり直しの命令がフレームを積む場所へスーパーバイザのデータ空間でロングかワードを書き、その前の
    // アクセスがまたバスエラーになるときだけで、そういう命令は見つかっていない (消してもテストは落ちない)。
    // pushBusFault() の最後の clearUndoLog() も 0 にするが、フレームを書いたあと
    replayN_ = 0;
    // 同じアクセスが命令の中で前に何回あったか (undoLog() が記録を消す前に数える)
    const unsigned ordinal = faultOrdinal(busSig(logical_, fc & 7, (uint8_t)(busCycle_ & 3), write));
    if (is060()) {
        // 68060: アクセスエラー (フォーマット $4)。論理アドレスと FSLW を積む。表を引いている途中なら TWE で、
        // アドレス、読み書き、FC、命令の読み込みかは、表を引くもとになったアクセスのもの (tableWalk() が覚えておく)
        if (inTableWalk_) accessFault(walkAddr_, 0x40u, walkWrite_, walkFc_, walkFetch_);
        accessFault(logical_, write ? 0x10u : 0x20u, write, fc, busCycle_ == BUS_FETCH);
    }
    undoLog();
    inException_ = true;
    // SSW (MC68030UM 図 8-9): 命令の読み込みなら FC/RC か FB/RB、データなら DF と RM、RW、SIZE、FC2-0
    uint16_t ssw;
    uint32_t stageB = pc0_ + 4;
    if (busCycle_ == BUS_FETCH) {
        if (addr == pc0_ + 4) ssw = 0x4000 | 0x1000; // FB | RB
        else ssw = 0x8000 | 0x2000;                  // FC | RC
        if (addr > pc0_ + 4) stageB = addr; // 拡張語の途中: その語をステージ B とする
        pushBusFault(this, 2, addr, ssw, stageB, -1);
    } else {
        ssw = (uint16_t)(0x0100 | (readModifyWrite(op_) ? 0x80 : 0) | (write ? 0 : 0x40) | ((busCycle_ & 3) << 4) |
                         (fc & 7));
        // アドレスは Bus に出したアクセス (slowRead / slowWrite の 1 回分) の先頭。Bus が 16 ビットずつ分けたうちの
        // 2 回目でも、分ける前のアドレスにする
        pushBusFault(this, 2, logical_, ssw, stageB, (int)ordinal);
    }
    M68K_LONGJMP(abort_, 1);
}

// DF が 0 のフォーマット $B のフレームからの RTE (opRte() が SR、PC、SP を戻したあと)。PC の命令を 1 つ実行する。
// その中で、バスエラーになったアクセス (アドレス、FC、大きさ、読み書きが同じもののうち ordinal 番目) を Bus に出さない
void Cpu020::resumeFaulted(uint32_t addr, uint16_t ssw, uint32_t dib, unsigned ordinal) {
    const uint8_t cycle = (uint8_t)((ssw >> 4) & 3);
    replaySig_ = busSig(addr, ssw & 7, cycle, !(ssw & 0x40));
    // データ入力バッファは右詰め (8.2.2)
    replayData_ = cycle == BUS_BYTE ? (dib & 0xff) : cycle == BUS_WORD ? (dib & 0xffff) : dib;
    replayOrdinal_ = (uint8_t)ordinal;
    replaySeen_ = 0;
    clearUndoLog();
    // 処理ルーチンがページを直接読み書きできるように変えていても、TLB に当たって slowRead / slowWrite を通らない
    // ことがないよう、TLB を空にする (やり直しの間は TLB に入れない。replayAccess())
    flushTlb();
    replayN_ = 1;
    pc0_ = pc_;
    const int traceMode = trace_;
    flow_ = false;
    noTrace_ = false;
    uint16_t op = fetch16();
    op_ = op;
    handlerOf(op)(this, op);
    replayN_ = 0;
    if (traceMode && !noTrace_ && ((traceMode & 2) || flow_)) exception(9, 2, pc_, pc0_);
    noTrace_ = true; // RTE のトレースは見ない (続けた命令のトレースを上で済ませた)
}

void Cpu020::addressErrorFetch(uint32_t addr) {
    if (inException_) {
        halted_ = true;
        M68K_LONGJMP(abort_, 1);
    }
    if (is060()) {
        // 68060: フォーマット $2。PC はこの命令、アドレスは飛び先の A0 を 0 にしたもの (MC68060UM 8.2.2)。
        // アドレスエラーのベクタも奇数なら、止める (ダブルフォルト)
        if (inAddrErr_) {
            inAddrErr_ = false;
            halted_ = true;
            M68K_LONGJMP(abort_, 1);
        }
        undoAddressRegs();
        noTrace_ = true;
        inAddrErr_ = true;
        exception(3, 2, pc0_, addr & ~1u);
        inAddrErr_ = false;
        M68K_LONGJMP(abort_, 1);
    }
    // 68030: バスエラーと同じく、命令の始めのアドレスレジスタと CCR に戻し、RTE は命令をはじめからやり直す
    replayN_ = 0;
    undoLog();
    inException_ = true;
    // 命令の読み込みのアドレスエラー: RB と RC だけを立てる (MC68030UM 8.2.1)
    pushBusFault(this, 3, addr, 0x3000, pc0_ + 4, -1);
    M68K_LONGJMP(abort_, 1);
}

// ---------------------------------------------------------------- 実行

void Cpu020::reset() {
    attn_ = 1;
    resetSplit();
    halted_ = stopped_ = false;
    inException_ = false;
    inAddrErr_ = false;
    trace_ = 0;
    if (!sflag_) usp_ = a_[7];
    setSupervisor(true);
    mflag_ = false;
    imask_ = 7;
    vbr_ = 0;
    cacr_ = 0;
    clearTiming060(); // 同時実行、折りたたみ、分岐キャッシュ、命令の取り出しの位置
    updateCacheCosts();
    tc030_ = 0;
    tt030_[0] = tt030_[1] = 0;
    // 68060: TC、TTR の E、BUSCR、PCR の書けるビットを 0 にする (MC68060UM 8.2.10)。PCR の上位は ID ($0430) と
    // リビジョン (値に意味は持たせず 6 にしておく)
    tc060_ = 0;
    for (int i = 0; i < 2; i++) {
        itt_[i] &= ~0x8000u;
        dtt_[i] &= ~0x8000u;
    }
    pcr_ = 0x04300600;
    buscr_ = 0;
    updateMmu();
    fpu_->reset();
    flushTlb();
    if (M68K_SETJMP(abort_)) {
        inException_ = false;
        halted_ = true;
        return;
    }
    inException_ = true; // リセット中のバスエラーは停止
    tick(16);
    uint32_t sp = memRead32(0, FC_SUPER_PROG);
    uint32_t pc = memRead32(4, FC_SUPER_PROG);
    a_[7] = sp;
    isp_ = sp;
    jumpTo(pc);
    inException_ = false;
}

template <bool M060> C020_HOT void Cpu020::execOne() {
    if (halted_) {
        if (M060) tick060(4);
        else tick(4);
        return;
    }
    // 68060: 命令をやり直す例外で戻す A0-A7 と CCR を取っておく。割り込みのベクタが奇数でアドレスエラーになる
    // 場合にも使うので、割り込みを受け付ける前に取る (68030 は割り込みのあとで取る。interrupt() も取る)
    if (M060) saveUndo();
    if (ipl_ != lastIpl_) {
        if (ipl_ == 7) nmiPending_ = true;
        lastIpl_ = ipl_;
    }
    if (ipl_ > imask_ || nmiPending_) {
        if (ipl_ == 7) nmiPending_ = false;
        if (ipl_ > imask_ || ipl_ == 7) {
            interrupt(ipl_);
            return;
        }
    }
    if (stopped_) {
        if (M060) tick060(4);
        else tick(4);
        return;
    }
    // トレース (MC68030UM 8.1.7): T1 なら命令ごと、T0 なら流れを変えた命令 (分岐・ジャンプ・トラップ・リターンと
    // SR を書く命令) のあと。命令の開始時の T で決まる
    execInstr<M060>(trace_, false, true);
}

// 命令を 1 つ実行する。execOne() (検査のあと) と runLoop() (検査を省いたとき。traceMode は 0、fast は true) から呼ぶ。
// fetchWait が false なら、68060 の命令キャッシュが無効のときの取り出しの時間を調べない (runLoop() が、無効の間は
// true で呼ぶ。cpu020.h の ATTN_FETCH)。3 つの引数は呼ぶ側で決まっていて、ここは呼ぶ所ごとに展開される
template <bool M060> C020_HOT void Cpu020::execInstr(int traceMode, bool fast, bool fetchWait) {
    pc0_ = pc_;
    if (M060 && fetchWait && __builtin_expect(fetchWaitOn_, 0)) {
        // 命令キャッシュが無効: 命令の並びが新しいロングワードに入るたびに、バスの読み出し 1 回分を足す (updateCacheCosts())。
        // 前の命令が流れを変えていれば (flow_)、同じロングワードでも読み直す。拡張語の分は、次の命令の先頭までに
        // 進んだロングワードの数で数える (流れを変えた命令の拡張語の分は jumpTo() が足す)
        if (flow_) {
            const uint32_t l = pc0_ & ~3u;
            sub_ += (uint64_t)fetchWait_[(l >> 28) & 1];
            fetchLong_ = l;
            fetchValid_ = true;
        } else {
            fetchSeq060(pc0_);
        }
    }
    flow_ = false;
    noTrace_ = false;
    // 命令のやり直しの記録 (cpu020.h の logA()) を空にする。68060 で execOne() から来たときは、saveUndo() の写し
    // (undoFull_) を残す
    if (fast || !M060) clearUndoLog();
    uint16_t op = fetch16<M060>();
    op_ = op;
    if (!M060) {
        table030_[op](this, op);
    } else {
        // サイクル数 (cpu060_cycles.inc)。前の命令と同時に進む命令 (スーパースカラ) は 0 サイクル。
        // 分岐の命令 (T_BRANCH) は、サイクル数も、同時実行と折りたたみの状態も、命令の中で決める。
        // ほかの命令は、表のサイクル数が 0 のもの (命令の中で足す) も含めて、ここで状態を進める: 同時実行の相手に
        // ならない命令は pairLead_ を消し (前の命令の状態が、この命令を越えて残らない)、分岐でない命令を実行したので
        // 次の分岐を折りたためる (fold060())
        const Op060& e = ops060_[op];
        const Handler fn = e.fn;
        const uint64_t meta = e.meta;
        const uint32_t t = (uint32_t)meta & 0xffffu;
        if (__builtin_expect(!(t & T_BRANCH), 1)) {
            // 同時実行の判定は分岐で書く。条件で値を選ぶ形 (分岐なし) も試したが、Pi 5 (GCC 14.2) で OutRun の命令数が
            // 4.5% 増え、分岐予測の外れは減らなかった (#224)。
            // PCR の ESS はビット 0、T_LEAD はビット 11 なので、pcr_ を 11 ビットずらして重ねて調べる
            const uint32_t lead = pairLead_;
            if (lead && (t & T_PAIR) && !((uint32_t)(meta >> 40) & pairDest_) && !(t & lead & T_MEM)) {
                pairLead_ = 0;
            } else {
                sub_ += (uint64_t)((t & T_CYC) * (uint32_t)unit060_);
                if (t & (pcr_ << 11) & T_LEAD) {
                    pairLead_ = t;
                    pairDest_ = (uint32_t)(meta >> 16) & 0xffffffu;
                } else {
                    pairLead_ = 0;
                }
            }
            foldOk060_ = true;
            if (__builtin_expect(!(t & T_CYC), 0)) {
                // サイクル数を命令の中で足す、分岐でない命令 (MOVEM、FPU など)。命令の中で 1 サイクルも足さなかった
                // もの (レジスタの数が 0 の MOVEM など) は、1 サイクルとする。分岐でない命令は必ず時間を進めるので、
                // 0 サイクルに折りたたまれる分岐と組み合わせても、時間が止まるループにならない
                const uint64_t sub060 = sub_;
                fn(this, op);
                if (sub_ == sub060) tick060(1);
                if (traceMode && !noTrace_ && ((traceMode & 2) || flow_)) exception(9, 2, pc_, pc0_);
                return;
            }
        }
        fn(this, op);
    }
    if (traceMode && !noTrace_ && ((traceMode & 2) || flow_)) exception(9, 2, pc_, pc0_);
}

#ifdef PIX68K_UNDO_CHECK
// undoAddressRegs() の結果を、命令ごとに A0-A7 と CCR を写していたときの結果と比べる
void Cpu020::undoCheckCompare(uint32_t a7Before) {
    bool ok = true;
    for (int i = 0; i < 7; i++) ok = ok && a_[i] == chkA_[i];
    ok = ok && a_[7] == ((sflag_ == chkS_ && mflag_ == chkM_) ? chkA_[7] : a7Before);
    ok = ok && flagX_ == chkCcr_[0] && flagN_ == chkCcr_[1] && flagZ_ == chkCcr_[2] && flagV_ == chkCcr_[3] &&
         flagC_ == chkCcr_[4];
    if (ok) return;
    fprintf(stderr, "undo check failed: pc %08x op %04x full %d n %02x\n", pc0_, op_, undoFull_, undoN_);
    for (int i = 0; i < 8; i++) fprintf(stderr, "  a%d now %08x want %08x\n", i, a_[i], chkA_[i]);
    fprintf(stderr, "  xnzvc now %u%u%u%u%u want %u%u%u%u%u\n", flagX_, flagN_, flagZ_, flagV_, flagC_, chkCcr_[0],
            chkCcr_[1], chkCcr_[2], chkCcr_[3], chkCcr_[4]);
    abort();
}
#endif

// データの読み書き 1 回と、命令の取り出し (ロングワード 1 つ) に足すサイクル数を決める。
// 68030: 今までと同じ (16 ビットまでの読み書きは 68000 の 4 サイクル分、32 ビットは 8 サイクル分)。
// 68060: CACR の EDC (データキャッシュ) / EIC (命令キャッシュ) が 1 なら、いつもキャッシュに当たるものとして足さない。
// 0 なら、メモリの読み書きはすべてバスに出るので、1 回ごとに MISS060_MAIN (本体のメモリと ROM。$10000000 より下) か
// MISS060_LOCAL (060turbo のローカルメモリ) を足す (どちらも setClock060() のクロックのサイクル数に直した値)。バスの時間なので、setSpeed() の倍率は掛けない。
// I/O と VRAM (Bus に出す読み書き) はもともとキャッシュに入らないので、EDC によらず setBusWait() の分だけを足す
// (slowRead / slowWrite は、メモリだったときだけ tickMiss() を足す)
// 命令の並びが a の語まで進んだ: 最後に取り出したロングワードから a のロングワードまでの数だけ、取り出しの時間を足す
void Cpu020::fetchSeq060(uint32_t a) {
    const uint32_t l = a & ~3u;
    if (!fetchValid_) {
        // まだ取り出していない (命令キャッシュを切った直後、リセットやステートの読み込みのあと): 新しく読み始める 1 回分
        sub_ += (uint64_t)fetchWait_[(l >> 28) & 1];
        fetchLong_ = l;
        fetchValid_ = true;
        return;
    }
    const uint32_t d = l - fetchLong_;
    if (!d) return;
    // 続きのロングワード (1 命令は最大 22 バイト) なら進んだ数だけ、そうでなければ新しく読み始める 1 回分を足す
    sub_ += (uint64_t)(d <= 24 ? d >> 2 : 1) * (uint32_t)fetchWait_[(l >> 28) & 1];
    fetchLong_ = l;
}

void Cpu020::updateCacheCosts() {
    if (!is060()) {
        dataCost16_[0] = dataCost16_[1] = slowCost16_ = 4 * cost_;
        dataCost32_[0] = dataCost32_[1] = slowCost32_ = 8 * cost_;
        missCost_[0] = missCost_[1] = 0;
        fetchWait_[0] = fetchWait_[1] = 0;
        fetchWaitOn_ = false;
        dataTick_ = true;
        return;
    }
    const bool edc = (cacr_ & 0x80000000u) != 0, eic = (cacr_ & 0x00008000u) != 0;
    dataCost16_[0] = dataCost32_[0] = missCost_[0] = edc ? 0 : missMain060_;
    dataCost16_[1] = dataCost32_[1] = missCost_[1] = edc ? 0 : missLocal060_;
    slowCost16_ = slowCost32_ = 0;
    fetchWait_[0] = eic ? 0 : missMain060_;
    fetchWait_[1] = eic ? 0 : missLocal060_;
    // 命令キャッシュを切ったところ: 最後に取り出した位置は決まっていない (次の命令で読み始める)
    if (!eic && !fetchWaitOn_) fetchValid_ = false;
    fetchWaitOn_ = !eic;
    dataTick_ = !edc;
    // fetchWaitOn_ が変わったかもしれない: 次の命令の前に、attn_ の ATTN_FETCH を決め直す (runLoop())
    attn_ = ATTN_CHECK;
}

void Cpu020::setSpeed(int percent) {
    if (percent > SPEED_MAX) percent = SPEED_MAX;
    if (is060()) {
        // 68060: 1 サイクル (MC68060UM の表の単位) を SUB * 100 / percent にする。tick() と FPU の 68882 の値は使わない
        if (percent < SPEED060_MIN) percent = SPEED060_MIN;
        unit060_ = SUB * 100 / percent;
        if (unit060_ < 1) unit060_ = 1;
        cost_ = 0;
        fpuCost_ = 0;
        speed_ = SUB * 100 / unit060_; // 割り算で丸めた後の、実際の速さ
        updateCacheCosts();
        return;
    }
    if (percent < 100) percent = 100;
    cost_ = baseCost_ * 100 / percent;
    if (cost_ < 1) cost_ = 1;
    updateCacheCosts();
    fpuCost_ = SUB * 100 / percent;
    speed_ = baseCost_ * 100 / cost_; // 割り算で丸めた後の、実際の速さ
}

int Cpu020::step() {
    uint64_t start = sub_;
    attn_ = 1;
    if (M68K_SETJMP(abort_)) {
        inException_ = false;
        resetSplit();
        return (int)((sub_ - start) / SUB);
    }
    resetSplit();
    if (is060()) execOne<true>();
    else execOne<false>();
    return (int)((sub_ - start) / SUB);
}

int64_t Cpu020::run(int64_t cycles) {
    const uint64_t start = sub_;
    const uint64_t end = start + (uint64_t)cycles * SUB;
    // 命令を打ち切ったあと (halted_ を変える所はどれも longjmp でここへ戻る) は、次の命令の前に execOne() の検査を通す。
    // 外から状態を変える所 (setRegisters()、ステートの読み込み、setIPL()) は、それぞれが attn_ を 1 にする
    // endRun_ は setjmp の前に消す。命令が endRun() を呼んだあとに打ち切られて (バスエラーなど) longjmp で戻ったときは
    // 残し、例外のフレームを積んだところで戻る
    endRun_ = 0;
    if (M68K_SETJMP(abort_)) {
        inException_ = false;
        attn_ = 1;
    }
    resetSplit();
    runUntil(end);
    return (int64_t)((sub_ - start) / SUB);
}

// STOP で止まっていて、割り込みを受け付けないと分かっているとき (runLoop() が調べる) に、execOne() を 4 サイクルずつ
// 繰り返すのと同じ所まで 1 回で進める: 4 サイクル分を足して初めて endSub 以上になる値にする (足す回数は切り上げ)。
// 止まっている間は命令を実行しないので、区間の途中で ipl_ や imask_ が変わることはない (setIPL() は区間の切れ目の
// 周辺機器の同期から、または run() の外から呼ばれる)。endSub は区間の終わりで、Machine::runCpu() が nextEvent()、
// nextTimerIrq()、区間の上限で区切ってある。4 サイクル単位で丸めるので、区間の終わりを 4 サイクル未満だけ越える
// ことがある (4 サイクルずつ進めていたときと同じ)。
// 68060: execOne() が毎回呼ぶ saveUndo() は、止まっている間はレジスタが変わらないので、1 回呼べば同じ状態になる
template <bool M060> void Cpu020::stopUntil(uint64_t endSub) {
    if (M060) saveUndo();
    const uint64_t step = M060 ? (uint64_t)(4 * unit060_) : (uint64_t)(4 * cost_);
    if (!step) {
        // 足す値が 0 (起きないはず): 今までどおり 1 回分だけ進めて戻る (runLoop() はそのまま繰り返す)
        if (M060) tick060(4);
        else tick(4);
        return;
    }
    // 足す回数を切り上げで求める。区間の残りが 2^64 に近くても桁あふれしないよう、商と余りで切り上げる
    const uint64_t rest = endSub - sub_;
    sub_ += (rest / step + (rest % step != 0)) * step;
}

// attn_ が 0 の間は、execOne() の始めの検査 (止まっているか、割り込み、トレース) を省いて命令を実行する。
// 省く検査は、attn_ が 0 ならどれも成り立たない (cpu020.h の attn_)。命令の実行そのものは execOne() と同じ
template <bool M060> C020_HOT void Cpu020::runLoop(uint64_t endSub) {
    while (sub_ < endSub) {
        if (__builtin_expect(attn_ != 0, 0)) {
            if (M060 && attn_ == ATTN_FETCH) {
                // 調べることは命令キャッシュが無効なことだけ: 検査を省いた実行に、取り出しの時間を足す。
                // ATTN_CHECK が 0 なので、下の endRun_ と stopped_ も調べなくてよい (どちらも、立てる所が attn_ を
                // ATTN_CHECK にする: endRun()、STOP の setSR())。命令が attn_ を変えたら (setIPL()、MOVEC で CACR を
                // 書く、など)、次の命令は下の経路で決め直す
                undoCheckSave();
                execInstr<M060>(0, true, true);
                continue;
            }
            // endRun(): ここで戻る。attn_ は ATTN_CHECK のまま残し、次の run() の最初の命令の前に検査を通す
            if (endRun_) {
                endRun_ = 0;
                return;
            }
            if (stopped_ && !halted_ && ipl_ == lastIpl_ && ipl_ <= imask_ && !nmiPending_) {
                stopUntil<M060>(endSub);
                continue;
            }
            execOne<M060>();
            // 命令の途中で setIPL() や setSR() が呼ばれていても、今の値で決め直す (endRun() も残す)
            attn_ = halted_ || stopped_ || trace_ != 0 || ipl_ != lastIpl_ || ipl_ > imask_ || nmiPending_ || endRun_;
            if (M060 && fetchWaitOn_) attn_ |= ATTN_FETCH;
            continue;
        }
        // 68060 も、命令の始めに A0-A7 と CCR を写さない。要る命令は、表に入れた関数 (thunk の U) が写す
        if (M060) undoCheckSave();
        execInstr<M060>(0, true, false);
    }
}

__attribute__((noinline)) void Cpu020::runUntil(uint64_t endSub) {
    if (is060()) runLoop<true>(endSub);
    else runLoop<false>(endSub);
}

// ---------------------------------------------------------------- 実効アドレス

// 拡張語のある実効アドレス (モード 6 と (d8,PC,Xn))。よく使う brief 形式 ((d8,An,Xn)) で、拡張語が今の命令のページに
// あるときは、ここで計算する (extAddr() を呼ばない)。結果とサイクル数は extAddr() と同じ (fetch16() の速い経路と同じに
// PC を進めて tick(4)、そのあと tick(2))。フル形式や、拡張語がページの外のときは extAddr() で計算する
template <bool M060> CPU020_INLINE uint32_t Cpu020::indexAddr(uint32_t base, bool pcRel) {
    const uint32_t pc = pc_;
    if ((pc >> 12) == fetchPage_) {
        const uint16_t ext = ld16(fetchBase_ + pc);
        if (!(ext & 0x100)) {
            pc_ = pc + 2;
            if (!M060) tick(4);
            const int r = (ext >> 12) & 15;
            uint32_t x = r < 8 ? d_[r] : a_[r - 8];
            if (!(ext & 0x800)) x = sext16(x);
            x <<= (ext >> 9) & 3;
            if (!M060) tick(2);
            return base + x + sext8(ext);
        }
    }
    return extAddr<M060>(base, pcRel);
}

// (d8,An,Xn) / (bd,An,Xn) / ([bd,An],Xn,od) / ([bd,An,Xn],od)。base は An か拡張語のアドレス (PC 相対)
template <bool M060> uint32_t Cpu020::extAddr(uint32_t base, bool pcRel) {
    (void)pcRel;
    uint16_t ext = fetch16<M060>();
    int r = (ext >> 12) & 15;
    uint32_t x = r < 8 ? d_[r] : a_[r - 8];
    if (!(ext & 0x800)) x = sext16(x);
    x <<= (ext >> 9) & 3;
    if (!(ext & 0x100)) {
        if (!M060) tick(2);
        return base + x + sext8(ext);
    }
    // フルフォーマット
    if (!M060) tick(4);
    tick060((ext & 7) ? 3 : 1); // MC68060UM 表 10-5: (bd,An,Xi) は 1、メモリ間接は 3
    if (ext & 0x80) base = 0; // BS
    if (ext & 0x40) x = 0;    // IS
    uint32_t bd = 0;
    switch ((ext >> 4) & 3) {
    case 2: bd = sext16(fetch16<M060>()); break;
    case 3: bd = fetch32<M060>(); break;
    }
    int iis = ext & 7;
    if (iis == 0) return base + bd + x;
    uint32_t od = 0;
    switch (iis & 3) {
    case 2: od = sext16(fetch16<M060>()); break;
    case 3: od = fetch32<M060>(); break;
    }
    if (iis & 4) return read32(base + bd) + x + od; // ポストインデックス
    return read32(base + bd + x) + od;              // プリインデックス
}

template <bool M060> CPU020_INLINE uint32_t Cpu020::eaAddr(int mode, int reg, int size) {
    switch (mode) {
    case 2:
        return a_[reg];
    case 3: {
        uint32_t a = a_[reg];
        logA(reg);
        a_[reg] += (size == 1 && reg == 7) ? 2 : size;
        return a;
    }
    case 4:
        if (!M060) tick(2);
        logA(reg);
        a_[reg] -= (size == 1 && reg == 7) ? 2 : size;
        return a_[reg];
    case 5: {
        uint32_t b = a_[reg];
        return b + sext16(fetch16<M060>());
    }
    case 6:
        return indexAddr<M060>(a_[reg], false);
    default:
        switch (reg) {
        case 0: return sext16(fetch16<M060>());
        case 1: return fetch32<M060>();
        case 2: {
            uint32_t b = pc_;
            return b + sext16(fetch16<M060>());
        }
        case 3: return indexAddr<M060>(pc_, true);
        }
    }
    return 0;
}

template <int S, bool M060> CPU020_INLINE uint32_t Cpu020::readEa(int mode, int reg) {
    if (mode == 0) return d_[reg] & Mask<S>::v;
    if (mode == 1) return a_[reg] & Mask<S>::v;
    if (mode == 7 && reg == 4) {
        if (S == 1) return fetch16<M060>() & 0xff;
        if (S == 2) return fetch16<M060>();
        return fetch32<M060>();
    }
    return readS<S>(eaAddr<M060>(mode, reg, S));
}

template <int S, bool M060> CPU020_INLINE uint32_t Cpu020::readEaAddr(int mode, int reg, uint32_t& addr) {
    if (mode == 0) return d_[reg] & Mask<S>::v;
    if (mode == 1) return a_[reg] & Mask<S>::v;
    addr = eaAddr<M060>(mode, reg, S);
    return readS<S>(addr);
}

template <int S> CPU020_INLINE void Cpu020::writeEa(int mode, int reg, uint32_t addr, uint32_t v) {
    if (mode == 0) setD<S>(reg, v);
    else if (mode == 1) a_[reg] = v;
    else writeS<S>(addr, v);
}

// 条件 (M68000PRM 表 3-19)。フラグは 0 か 1。条件が決まっている thunk (cpu020.h の SPEC) では、1 つの式だけが残る
CPU020_INLINE bool Cpu020::cond(int cc) const {
    switch (cc & 15) {
    case 0: return true;                                // T
    case 1: return false;                               // F
    case 2: return !(flagC_ | flagZ_);                  // HI
    case 3: return (flagC_ | flagZ_) != 0;              // LS
    case 4: return !flagC_;                             // CC
    case 5: return flagC_ != 0;                         // CS
    case 6: return !flagZ_;                             // NE
    case 7: return flagZ_ != 0;                         // EQ
    case 8: return !flagV_;                             // VC
    case 9: return flagV_ != 0;                         // VS
    case 10: return !flagN_;                            // PL
    case 11: return flagN_ != 0;                        // MI
    case 12: return flagN_ == flagV_;                   // GE
    case 13: return flagN_ != flagV_;                   // LT
    case 14: return !flagZ_ && flagN_ == flagV_;        // GT
    default: return flagZ_ || flagN_ != flagV_;         // LE
    }
}

// ---------------------------------------------------------------- 演算

template <int S> CPU020_INLINE void Cpu020::setNZ(uint32_t v) {
    flagN_ = (v & Msb<S>::v) ? 1 : 0;
    flagZ_ = (v & Mask<S>::v) ? 0 : 1;
}

template <int S> CPU020_INLINE uint32_t Cpu020::add(uint32_t s, uint32_t d, uint32_t x, bool addx) {
    const uint32_t m = Mask<S>::v;
    s &= m;
    d &= m;
    uint64_t wide = (uint64_t)s + d + x;
    uint32_t r = (uint32_t)wide & m;
    flagC_ = flagX_ = (uint32_t)((wide >> (S * 8)) & 1);
    flagV_ = ((s ^ r) & (d ^ r) & Msb<S>::v) ? 1 : 0;
    flagN_ = (r & Msb<S>::v) ? 1 : 0;
    if (addx) {
        if (r) flagZ_ = 0;
    } else {
        flagZ_ = r ? 0 : 1;
    }
    return r;
}

template <int S> CPU020_INLINE uint32_t Cpu020::sub(uint32_t s, uint32_t d, uint32_t x, bool subx) {
    const uint32_t m = Mask<S>::v;
    s &= m;
    d &= m;
    uint32_t r = (d - s - x) & m;
    flagC_ = flagX_ = ((uint64_t)s + x > d) ? 1 : 0;
    flagV_ = ((s ^ d) & (r ^ d) & Msb<S>::v) ? 1 : 0;
    flagN_ = (r & Msb<S>::v) ? 1 : 0;
    if (subx) {
        if (r) flagZ_ = 0;
    } else {
        flagZ_ = r ? 0 : 1;
    }
    return r;
}

template <int S> CPU020_INLINE void Cpu020::cmp(uint32_t s, uint32_t d) {
    const uint32_t m = Mask<S>::v;
    s &= m;
    d &= m;
    uint32_t r = (d - s) & m;
    flagC_ = (s > d) ? 1 : 0;
    flagV_ = ((s ^ d) & (r ^ d) & Msb<S>::v) ? 1 : 0;
    setNZ<S>(r);
}

template <int S> CPU020_INLINE uint32_t Cpu020::logic(uint32_t v) {
    setNZ<S>(v);
    flagV_ = flagC_ = 0;
    return v & Mask<S>::v;
}

// BCD の加減算 (ABCD / SBCD / NBCD で使う)。s がソース、d がデスティネーション。X を加味し、X C Z N V を設定する
// ABCD: 下位の桁の和が 9 を超えたら 6 を足し、上位の桁が 9 を超えたか桁あふれしたら $60 を足す。
// 桁上がりは 2 回目の補正をしたかどうか。N は結果の最上位ビット。68030 では V は常に 0 (instructiontest.x の実機の結果から)
uint8_t Cpu020::abcd(uint8_t s, uint8_t d) {
    int x = flagX_;
    int r = d + s + x;
    if ((d & 0x0f) + (s & 0x0f) + x > 9) r += 0x06;
    int c = r >= 0xa0 ? 1 : 0;
    if (c) r += 0x60;
    flagX_ = flagC_ = c;
    if (r & 0xff) flagZ_ = 0;
    if (!is060()) {
        flagN_ = (r >> 7) & 1;
        flagV_ = 0;
    }
    return (uint8_t)r;
}

// SBCD (NBCD は d=0): 下位の桁で借りが出たら 6 を引き、2 進の引き算全体で借りが出たら $60 を引く。
// 借り (X と C) は、6 を引いたあとの値が負になったかどうか。N は結果の最上位ビット。68030 では V は常に 0
uint8_t Cpu020::sbcd(uint8_t s, uint8_t d) {
    int x = flagX_;
    int u = d - s - x; // 補正前
    int r = u;
    if ((d & 0x0f) - (s & 0x0f) - x < 0) r -= 0x06;
    int c = r < 0 ? 1 : 0;
    if (u < 0) r -= 0x60;
    flagX_ = flagC_ = c;
    if (r & 0xff) flagZ_ = 0;
    if (!is060()) {
        flagN_ = (r >> 7) & 1;
        flagV_ = 0;
    }
    return (uint8_t)r;
}

} // namespace m68k

#include "m68k/cpu020_ops.inc"
#include "m68k/cpu060.inc"
#include "m68k/cpu020_decode.inc"
