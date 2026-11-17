// MC68000 CPU コア: レジスタ、バスアクセス、例外、実効アドレス、実行ループ
#include "m68k.h"

#include "m68k_internal.h"

// よく使う小さな関数は、命令の実装の中に必ず展開させる (呼び出しの手間が命令の実行時間の大きな部分を占めるため)
#define M68K_HOT inline __attribute__((always_inline))

namespace m68k {

Cpu::Handler Cpu::table_[65536];
bool Cpu::tableReady_ = false;

Cpu::Cpu(Bus* bus) : bus_(bus) {
    clearFastPages();
    if (!tableReady_) {
        buildTable();
        tableReady_ = true;
    }
    for (int i = 0; i < 8; i++) d_[i] = a_[i] = 0;
    inactiveSp_ = 0;
    pc_ = 0;
    ird_ = irc_ = 0;
    flagX_ = flagN_ = flagZ_ = flagV_ = flagC_ = 0;
    sflag_ = true;
    tflag_ = false;
    imask_ = 7;
}

// ---------------------------------------------------------------- SR

uint16_t Cpu::getSR() const {
    return (uint16_t)((tflag_ ? 0x8000 : 0) | (sflag_ ? 0x2000 : 0) | (imask_ << 8) | (flagX_ << 4) |
                      (flagN_ << 3) | (flagZ_ << 2) | (flagV_ << 1) | flagC_);
}

void Cpu::setCCR(uint8_t ccr) {
    flagX_ = (ccr >> 4) & 1;
    flagN_ = (ccr >> 3) & 1;
    flagZ_ = (ccr >> 2) & 1;
    flagV_ = (ccr >> 1) & 1;
    flagC_ = ccr & 1;
}

void Cpu::setSR(uint16_t sr) {
    tflag_ = (sr & 0x8000) != 0;
    setSupervisor((sr & 0x2000) != 0);
    imask_ = (sr >> 8) & 7;
    setCCR((uint8_t)sr);
}

void Cpu::setSupervisor(bool s) {
    if (s != sflag_) {
        uint32_t t = a_[7];
        a_[7] = inactiveSp_;
        inactiveSp_ = t;
        sflag_ = s;
    }
}

void Cpu::getRegisters(Registers& r) const {
    for (int i = 0; i < 8; i++) r.d[i] = d_[i];
    for (int i = 0; i < 7; i++) r.a[i] = a_[i];
    if (sflag_) {
        r.ssp = a_[7];
        r.usp = inactiveSp_;
    } else {
        r.usp = a_[7];
        r.ssp = inactiveSp_;
    }
    r.sr = getSR();
    r.pc = pc_;
    r.prefetch[0] = ird_;
    r.prefetch[1] = irc_;
}

void Cpu::setRegisters(const Registers& r) {
    for (int i = 0; i < 8; i++) d_[i] = r.d[i];
    for (int i = 0; i < 7; i++) a_[i] = r.a[i];
    tflag_ = (r.sr & 0x8000) != 0;
    sflag_ = (r.sr & 0x2000) != 0;
    imask_ = (r.sr >> 8) & 7;
    setCCR((uint8_t)r.sr);
    if (sflag_) {
        a_[7] = r.ssp;
        inactiveSp_ = r.usp;
    } else {
        a_[7] = r.usp;
        inactiveSp_ = r.ssp;
    }
    pc_ = r.pc;
    ird_ = r.prefetch[0];
    irc_ = r.prefetch[1];
    halted_ = stopped_ = false;
    inGroup0_ = false;
    faultPc_ = NO_FAULT_PC;
    faultNotInstr_ = false;
}

// ---------------------------------------------------------------- バスアクセス

// 表 (setFastPage) にあるページは Bus を呼ばずにホストのメモリを直接読み書きする。
// fc の bit2 がスーパーバイザ (5, 6) かユーザ (1, 2) かを表す
M68K_HOT uint16_t Cpu::readWordRaw(uint32_t addr, int fc) {
    cycles_ += 4;
    addr &= 0xffffff;
    if (const uint8_t* p = fastRead_[(fc >> 2) & 1][addr >> 16]) {
        p += addr & 0xffff;
        return (uint16_t)((p[0] << 8) | p[1]);
    }
    return bus_->read16(addr, fc);
}

M68K_HOT void Cpu::writeWordRaw(uint32_t addr, uint16_t v, int fc) {
    cycles_ += 4;
    addr &= 0xffffff;
    if (uint8_t* p = fastWrite_[(fc >> 2) & 1][addr >> 16]) {
        p += addr & 0xffff;
        p[0] = (uint8_t)(v >> 8);
        p[1] = (uint8_t)v;
        return;
    }
    bus_->write16(addr, v, fc);
}

M68K_HOT uint8_t Cpu::readByteRaw(uint32_t addr, int fc) {
    cycles_ += 4;
    addr &= 0xffffff;
    if (const uint8_t* p = fastRead_[(fc >> 2) & 1][addr >> 16]) return p[addr & 0xffff];
    return bus_->read8(addr, fc);
}

M68K_HOT void Cpu::writeByteRaw(uint32_t addr, uint8_t v, int fc) {
    cycles_ += 4;
    addr &= 0xffffff;
    if (uint8_t* p = fastWrite_[(fc >> 2) & 1][addr >> 16]) {
        p[addr & 0xffff] = v;
        return;
    }
    bus_->write8(addr, v, fc);
}

void Cpu::setFastPage(bool supervisor, int page, const uint8_t* read, uint8_t* write) {
    page &= FAST_PAGES - 1;
    fastRead_[supervisor ? 1 : 0][page] = read;
    fastWrite_[supervisor ? 1 : 0][page] = write;
}

void Cpu::clearFastPages() {
    for (int s = 0; s < 2; s++) {
        for (int i = 0; i < FAST_PAGES; i++) {
            fastRead_[s][i] = nullptr;
            fastWrite_[s][i] = nullptr;
        }
    }
}

// prog=true は PC 相対のオペランド読み出し (プログラム空間の FC を出す)
template <int S> M68K_HOT uint32_t Cpu::read(uint32_t addr, bool prog) {
    int fc = prog ? fcProg() : fcData();
    if (S == 1) return readByteRaw(addr, fc);
    if (addr & 1) addressError(addr, false, prog);
    if (S == 2) return readWordRaw(addr, fc);
    uint32_t hi = readWordRaw(addr, fc);
    return (hi << 16) | readWordRaw(addr + 2, fc);
}

template <int S> M68K_HOT void Cpu::write(uint32_t addr, uint32_t v) {
    if (S == 1) {
        writeByteRaw(addr, (uint8_t)v, fcData());
        return;
    }
    if (addr & 1) addressError(addr, true, false);
    if (S == 2) {
        writeWordRaw(addr, (uint16_t)v, fcData());
        return;
    }
    writeWordRaw(addr, (uint16_t)(v >> 16), fcData());
    writeWordRaw(addr + 2, (uint16_t)v, fcData());
}

template <int S> void Cpu::writeLongLowFirst(uint32_t addr, uint32_t v) {
    if (addr & 1) addressError(addr + 2, true, false); // 先に書く下位語のアドレスで報告する
    writeWordRaw(addr + 2, (uint16_t)v, fcData());
    writeWordRaw(addr, (uint16_t)(v >> 16), fcData());
}

M68K_HOT uint16_t Cpu::fetch(uint32_t addr) {
    if (addr & 1) addressError(addr, false, true);
    return readWordRaw(addr, fcProg());
}

// IRC にある拡張語を取り出し、次の語を IRC に読み込む
M68K_HOT uint16_t Cpu::readExt() {
    uint16_t v = irc_;
    pc_ += 2;
    irc_ = fetch(pc_ + 2);
    return v;
}

uint32_t Cpu::readExtLong() {
    uint32_t hi = readExt();
    return (hi << 16) | readExt();
}

// 命令の最後のプリフェッチ: 次の命令を IRD に、その次の語を IRC に置く
M68K_HOT void Cpu::prefetch() {
    pc_ += 2;
    ird_ = irc_;
    irc_ = fetch(pc_ + 2);
}

// SR を変えた命令などで、次の命令から読み直す
void Cpu::refetch() {
    pc_ += 2;
    ird_ = fetch(pc_);
    irc_ = fetch(pc_ + 2);
}

void Cpu::jump(uint32_t addr, int gap) {
    if (addr & 1) {
        // 分岐先が奇数: 積む PC は分岐先 - 4、I/N ビットは 1 (命令外)
        faultPc_ = addr - 4;
        faultNotInstr_ = true;
        addressError(addr, false, true);
    }
    pc_ = addr;
    ird_ = fetch(addr);
    idle(gap);
    irc_ = fetch(addr + 2);
}

void Cpu::push16(uint16_t v) {
    a_[7] -= 2;
    write<2>(a_[7], v);
}

void Cpu::push32(uint32_t v) {
    a_[7] -= 4;
    writeLongLowFirst<4>(a_[7], v);
}

uint32_t Cpu::readPredecLong(int reg) {
    a_[reg] -= 2;
    uint32_t lo = read<2>(a_[reg]);
    a_[reg] -= 2;
    uint32_t hi = read<2>(a_[reg]);
    return (hi << 16) | lo;
}

uint32_t Cpu::pop32() {
    uint32_t v = read<4>(a_[7]);
    a_[7] += 4;
    return v;
}

// ---------------------------------------------------------------- 例外

// グループ 1/2 の例外 (トラップ、不当命令、割り込み以外)
void Cpu::exception(int vector, uint32_t pushPc, int idleBefore) {
    stopped_ = false; // STOP 中のトレースなど、例外処理に入ったら停止状態から抜ける
    uint16_t old = getSR();
    setSupervisor(true);
    tflag_ = false;
    idle(idleBefore);
    a_[7] -= 6;
    write<2>(a_[7] + 4, pushPc & 0xffff);
    write<2>(a_[7], old);
    write<2>(a_[7] + 2, pushPc >> 16);
    uint32_t v = read<4>(vector * 4);
    jump(v, 2);
}

void Cpu::privilegeViolation() {
    noTrace_ = true;
    exception(8, pc_, 4);
}
void Cpu::illegal(uint16_t) { exception(4, pc_, 4); }

void Cpu::interrupt(int level) {
    uint16_t old = getSR();
    setSupervisor(true);
    tflag_ = false;
    imask_ = level;
    idle(6);
    a_[7] -= 6;
    write<2>(a_[7] + 4, pc_ & 0xffff);
    cycles_ += 4; // IACK サイクル
    int vec;
    if (M68K_SETJMP(iackAbort_)) {
        vec = 24; // IACK 中のバスエラー: スプリアス割り込み
    } else {
        inIack_ = true;
        vec = bus_->interruptAck(level);
    }
    inIack_ = false;
    if (vec == -1) {
        vec = 24 + level;
        idle(4); // VPA 同期 (概算)
    } else if (vec < 0) {
        vec = 24;
    }
    write<2>(a_[7], old);
    write<2>(a_[7] + 2, pc_ >> 16);
    uint32_t v = read<4>(vec * 4);
    jump(v, 2);
}

// グループ 0 (バスエラー、アドレスエラー)。命令を打ち切って longjmp する
void Cpu::group0(int vector, uint32_t addr, bool write, int fc) {
    if (inGroup0_) {
        // 例外処理中の再発生はダブルフォルト: CPU は停止する
        halted_ = true;
        faultPc_ = NO_FAULT_PC;
        faultNotInstr_ = false;
        M68K_LONGJMP(abort_, 1);
    }
    inGroup0_ = true;
    uint16_t old = getSR();
    uint16_t status = (uint16_t)((curOp_ & 0xffe0) | (write ? 0 : 0x10) | (faultNotInstr_ ? 0x08 : 0) | fc);
    uint32_t pushPc = faultPc_ != NO_FAULT_PC ? faultPc_ : pc_;
    faultPc_ = NO_FAULT_PC;
    faultNotInstr_ = false;
    setSupervisor(true);
    tflag_ = false;
    idle(4);
    a_[7] -= 14;
    this->write<2>(a_[7] + 12, pushPc & 0xffff);
    this->write<2>(a_[7] + 8, old);
    this->write<2>(a_[7] + 10, pushPc >> 16);
    this->write<2>(a_[7] + 6, curOp_);
    this->write<2>(a_[7] + 4, addr & 0xffff);
    this->write<2>(a_[7] + 0, status);
    this->write<2>(a_[7] + 2, addr >> 16);
    uint32_t v = read<4>(vector * 4);
    jump(v, 2);
    inGroup0_ = false;
    M68K_LONGJMP(abort_, 1);
}

void Cpu::addressError(uint32_t addr, bool write, bool prog) {
    group0(3, addr, write, prog ? fcProg() : fcData());
}

void Cpu::busError(uint32_t addr, bool write, int fc) {
    if (inIack_) M68K_LONGJMP(iackAbort_, 1);
    group0(2, addr, write, fc);
}

// ---------------------------------------------------------------- 実行

void Cpu::reset() {
    halted_ = stopped_ = false;
    inGroup0_ = false;
    faultPc_ = NO_FAULT_PC;
    faultNotInstr_ = false;
    tflag_ = false;
    setSupervisor(true);
    imask_ = 7;
    if (M68K_SETJMP(abort_)) {
        inGroup0_ = false;
        halted_ = true;
        return;
    }
    // リセット中のバスエラー・アドレスエラーはスタックを積まずに停止する (ダブルフォルトと同じ扱い)
    inGroup0_ = true;
    idle(16);
    uint32_t hi = readWordRaw(0, FC_SUPER_PROG);
    uint32_t sp = (hi << 16) | readWordRaw(2, FC_SUPER_PROG);
    hi = readWordRaw(4, FC_SUPER_PROG);
    uint32_t pc = (hi << 16) | readWordRaw(6, FC_SUPER_PROG);
    a_[7] = sp;
    jump(pc, 0);
    inGroup0_ = false;
}

// 1 命令 (または例外処理 1 回) を実行する。setjmp は呼び出し側で済ませておく
inline void Cpu::execOne() {
    if (halted_) {
        cycles_ += 4;
        return;
    }
    // 割り込み: レベル 7 はエッジで受け付ける (NMI)
    if (ipl_ != lastIpl_) {
        if (ipl_ == 7) nmiPending_ = true;
        lastIpl_ = ipl_;
    }
    if (ipl_ > imask_ || nmiPending_) {
        if (ipl_ == 7) nmiPending_ = false;
        if (ipl_ > imask_ || ipl_ == 7) {
            stopped_ = false;
            interrupt(ipl_);
            return;
        }
    }
    if (stopped_) {
        idle(4);
        return;
    }
    bool trace = tflag_;
    uint16_t op = ird_;
    curOp_ = op;
    noTrace_ = false;
    table_[op](this, op);
    if (trace && !noTrace_) exception(9, pc_, 4); // トレース例外は 34 サイクル
}

int Cpu::step() {
    uint64_t start = cycles_;
    if (M68K_SETJMP(abort_)) {
        inGroup0_ = false;
        return (int)(cycles_ - start);
    }
    execOne();
    return (int)(cycles_ - start);
}

int64_t Cpu::run(int64_t cycles) {
    const uint64_t start = cycles_;
    const uint64_t end = start + (uint64_t)cycles;
    // 命令がグループ 0 例外で打ち切られたら、ここに戻ってきて続きを実行する
    if (M68K_SETJMP(abort_)) inGroup0_ = false;
    runUntil(end);
    return (int64_t)(cycles_ - start);
}

// run() の命令のループ。setjmp と同じ関数に置くと、this などをレジスタに置けず毎回メモリから読み直すので分ける
__attribute__((noinline)) void Cpu::runUntil(uint64_t end) {
    while (cycles_ < end) execOne();
}

// ---------------------------------------------------------------- 実効アドレス

uint32_t Cpu::indexValue(uint16_t ext) {
    int r = (ext >> 12) & 7;
    uint32_t x = (ext & 0x8000) ? a_[r] : d_[r];
    if (!(ext & 0x0800)) x = (uint32_t)(int32_t)(int16_t)x;
    return x + (uint32_t)(int32_t)(int8_t)(ext & 0xff);
}

template <int S> uint32_t Cpu::ea(int mode, int reg, int idxIdle, int pdIdle) {
    switch (mode) {
    case 2:
        return a_[reg];
    case 3: {
        uint32_t a = a_[reg];
        a_[reg] += (S == 1 && reg == 7) ? 2 : S;
        return a;
    }
    case 4:
        idle(pdIdle);
        a_[reg] -= (S == 1 && reg == 7) ? 2 : S;
        return a_[reg];
    case 5: {
        uint32_t base = a_[reg];
        return base + (uint32_t)(int32_t)(int16_t)readExt();
    }
    case 6: {
        idle(idxIdle);
        uint32_t base = a_[reg];
        return base + indexValue(readExt());
    }
    case 7:
        switch (reg) {
        case 0:
            return (uint32_t)(int32_t)(int16_t)readExt();
        case 1:
            return readExtLong();
        case 2: {
            uint32_t base = pc_ + 2;
            return base + (uint32_t)(int32_t)(int16_t)readExt();
        }
        case 3: {
            idle(idxIdle);
            uint32_t base = pc_ + 2;
            return base + indexValue(readExt());
        }
        }
    }
    return 0;
}

// JMP/JSR: 拡張語は IRC から読み、詰め直しの読み込みをしない
uint32_t Cpu::eaControlJump(int mode, int reg) {
    switch (mode) {
    case 2:
        return a_[reg];
    case 5:
        idle(2);
        return a_[reg] + (uint32_t)(int32_t)(int16_t)irc_;
    case 6:
        idle(6);
        return a_[reg] + indexValue(irc_);
    case 7:
        switch (reg) {
        case 0:
            idle(2);
            return (uint32_t)(int32_t)(int16_t)irc_;
        case 1: {
            uint32_t hi = irc_;
            return (hi << 16) | fetch(pc_ + 4);
        }
        case 2:
            idle(2);
            return pc_ + 2 + (uint32_t)(int32_t)(int16_t)irc_;
        case 3:
            idle(6);
            return pc_ + 2 + indexValue(irc_);
        }
    }
    return 0;
}

template <int S> uint32_t Cpu::readImm() {
    if (S == 1) return readExt() & 0xff;
    if (S == 2) return readExt();
    return readExtLong();
}

template <int S> uint32_t Cpu::readOperand(int mode, int reg, uint32_t& addr) {
    if (mode == 0) return d_[reg] & Mask<S>::v;
    if (mode == 1) return a_[reg] & Mask<S>::v;
    if (mode == 7 && reg == 4) return readImm<S>();
    addr = ea<S>(mode, reg);
    return read<S>(addr, mode == 7 && (reg == 2 || reg == 3));
}

template <int S> void Cpu::writeOperand(int mode, int reg, uint32_t addr, uint32_t v) {
    if (mode == 0)
        setD<S>(reg, v);
    else if (mode == 1)
        a_[reg] = v;
    else
        write<S>(addr, v);
}

template <int S> void Cpu::setD(int r, uint32_t v) {
    d_[r] = (d_[r] & ~Mask<S>::v) | (v & Mask<S>::v);
}

// 条件 cc が成り立つか。cc ごとに、NZVC の 16 通りのどれで成り立つかを 16 ビットの表にしてある
bool Cpu::cond(int cc) const {
    static const uint16_t kTable[16] = {
        0xffff, 0x0000, // T, F
        0x0505, 0xfafa, // HI (!C && !Z), LS (C || Z)
        0x5555, 0xaaaa, // CC, CS
        0x0f0f, 0xf0f0, // NE, EQ
        0x3333, 0xcccc, // VC, VS
        0x00ff, 0xff00, // PL, MI
        0xcc33, 0x33cc, // GE (N == V), LT
        0x0c03, 0xf3fc, // GT (!Z && N == V), LE
    };
    uint32_t nzvc = (flagN_ << 3) | (flagZ_ << 2) | (flagV_ << 1) | flagC_;
    return (kTable[cc & 15] >> nzvc) & 1;
}

template <int S> void Cpu::setNZ(uint32_t v) {
    flagN_ = (v & Msb<S>::v) ? 1 : 0;
    flagZ_ = (v & Mask<S>::v) ? 0 : 1;
}

// ---------------------------------------------------------------- 演算

template <int S> uint32_t Cpu::add(uint32_t s, uint32_t d, uint32_t x, bool addx) {
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

template <int S> uint32_t Cpu::sub(uint32_t s, uint32_t d, uint32_t x, bool subx) {
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

template <int S> void Cpu::cmp(uint32_t s, uint32_t d) {
    const uint32_t m = Mask<S>::v;
    s &= m;
    d &= m;
    uint32_t r = (d - s) & m;
    flagC_ = (s > d) ? 1 : 0;
    flagV_ = ((s ^ d) & (r ^ d) & Msb<S>::v) ? 1 : 0;
    setNZ<S>(r);
}

template <int S> uint32_t Cpu::logic(uint32_t v) {
    setNZ<S>(v);
    flagV_ = flagC_ = 0;
    return v & Mask<S>::v;
}

uint8_t Cpu::abcd(uint8_t yy, uint8_t xx) {
    uint32_t ss = (uint32_t)xx + yy + flagX_;
    uint32_t bc = ((xx & yy) | (~ss & (xx | yy))) & 0x88;
    uint32_t dc = (((ss + 0x66) ^ ss) & 0x110) >> 1;
    uint32_t corf = (bc | dc) - ((bc | dc) >> 2);
    uint32_t rr = ss + corf;
    flagX_ = flagC_ = ((bc | (ss & ~rr)) >> 7) & 1;
    flagV_ = ((~ss & rr) >> 7) & 1;
    rr &= 0xff;
    if (rr) flagZ_ = 0;
    flagN_ = rr >> 7;
    return (uint8_t)rr;
}

uint8_t Cpu::sbcd(uint8_t yy, uint8_t xx) {
    uint32_t dd = (uint32_t)xx - yy - flagX_;
    uint32_t bc = ((~(uint32_t)xx & yy) | (dd & ~((uint32_t)xx ^ yy))) & 0x88;
    uint32_t corf = bc - (bc >> 2);
    uint32_t rr = dd - corf;
    flagX_ = flagC_ = ((bc | (~dd & rr)) >> 7) & 1;
    flagV_ = ((dd & ~rr) >> 7) & 1;
    rr &= 0xff;
    if (rr) flagZ_ = 0;
    flagN_ = rr >> 7;
    return (uint8_t)rr;
}

} // namespace m68k

#include "m68k_ops.inc"
#include "m68k_decode.inc"
