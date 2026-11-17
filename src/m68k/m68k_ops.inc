// MC68000 命令の実装 (m68k.cpp から include する)
//
// 命令の開始時、pc_ は命令の先頭、ird_ はオペコード、irc_ は pc_+2 の語。
// サイクル数はバスアクセス 1 回 4 サイクルと、idle() で足す内部サイクルの合計になる。

namespace m68k {

#define MODE ((op >> 3) & 7)
#define REG (op & 7)
#define REG9 ((op >> 9) & 7)

static inline bool isRegOrImm(int mode, int reg) { return mode <= 1 || (mode == 7 && reg == 4); }

// ---------------------------------------------------------------- 算術・論理

template <int S, int OP> void Cpu::opArithEaDn(uint16_t op) {
    int mode = MODE, reg = REG, dn = REG9;
    uint32_t addr = 0;
    uint32_t s = readOperand<S>(mode, reg, addr);
    uint32_t d = d_[dn] & Mask<S>::v;
    uint32_t r = 0;
    switch (OP) {
    case OP_ADD: r = add<S>(s, d, 0, false); break;
    case OP_SUB: r = sub<S>(s, d, 0, false); break;
    case OP_AND: r = logic<S>(d & s); break;
    case OP_OR: r = logic<S>(d | s); break;
    case OP_CMP: cmp<S>(s, d); break;
    }
    prefetch();
    if (S == 4) idle(OP == OP_CMP ? 2 : (isRegOrImm(mode, reg) ? 4 : 2));
    if (OP != OP_CMP) setD<S>(dn, r);
}

template <int S, int OP> void Cpu::opArithDnEa(uint16_t op) {
    int mode = MODE, reg = REG, dn = REG9;
    uint32_t s = d_[dn] & Mask<S>::v;
    if (mode == 0) { // EOR Dn,Dn
        uint32_t r = logic<S>(d_[reg] ^ s);
        prefetch();
        if (S == 4) idle(4);
        setD<S>(reg, r);
        return;
    }
    uint32_t addr = ea<S>(mode, reg);
    uint32_t d = read<S>(addr);
    uint32_t r = 0;
    switch (OP) {
    case OP_ADD: r = add<S>(s, d, 0, false); break;
    case OP_SUB: r = sub<S>(s, d, 0, false); break;
    case OP_AND: r = logic<S>(d & s); break;
    case OP_OR: r = logic<S>(d | s); break;
    case OP_EOR: r = logic<S>(d ^ s); break;
    }
    prefetch();
    write<S>(addr, r);
}

template <int S, bool SUB> void Cpu::opAddxSubx(uint16_t op) {
    int rx = REG9, ry = REG;
    if (!(op & 8)) {
        uint32_t r = SUB ? sub<S>(d_[ry], d_[rx], flagX_, true) : add<S>(d_[ry], d_[rx], flagX_, true);
        prefetch();
        if (S == 4) idle(4);
        setD<S>(rx, r);
        return;
    }
    idle(2);
    uint32_t s, d;
    if (S == 4) {
        // -(An) のロングは下位語、上位語の順に 2 バイトずつ減らしながら読む
        s = readPredecLong(ry);
        d = readPredecLong(rx);
    } else {
        a_[ry] -= (S == 1 && ry == 7) ? 2 : S;
        s = read<S>(a_[ry]);
        a_[rx] -= (S == 1 && rx == 7) ? 2 : S;
        d = read<S>(a_[rx]);
    }
    uint32_t r = SUB ? sub<S>(s, d, flagX_, true) : add<S>(s, d, flagX_, true);
    prefetch();
    write<S>(a_[rx], r);
}

template <int S, bool SUB> void Cpu::opAdda(uint16_t op) {
    int mode = MODE, reg = REG, an = REG9;
    uint32_t addr = 0;
    uint32_t s = readOperand<S>(mode, reg, addr);
    if (S == 2) s = sext16(s);
    a_[an] = SUB ? a_[an] - s : a_[an] + s;
    prefetch();
    idle(S == 2 ? 4 : (isRegOrImm(mode, reg) ? 4 : 2));
}

template <int S> void Cpu::opCmpa(uint16_t op) {
    int mode = MODE, reg = REG, an = REG9;
    uint32_t addr = 0;
    uint32_t s = readOperand<S>(mode, reg, addr);
    if (S == 2) s = sext16(s);
    cmp<4>(s, a_[an]);
    prefetch();
    idle(2);
}

template <int S> void Cpu::opCmpm(uint16_t op) {
    int ax = REG9, ay = REG;
    uint32_t s = read<S>(ea<S>(3, ay));
    uint32_t d = read<S>(ea<S>(3, ax));
    cmp<S>(s, d);
    prefetch();
}

template <int S, int OP> void Cpu::opImm(uint16_t op) {
    int mode = MODE, reg = REG;
    uint32_t imm = readImm<S>();
    uint32_t d, addr = 0;
    if (mode == 0)
        d = d_[reg] & Mask<S>::v;
    else {
        addr = ea<S>(mode, reg);
        d = read<S>(addr);
    }
    uint32_t r = 0;
    switch (OP) {
    case IMM_ORI: r = logic<S>(d | imm); break;
    case IMM_ANDI: r = logic<S>(d & imm); break;
    case IMM_SUBI: r = sub<S>(imm, d, 0, false); break;
    case IMM_ADDI: r = add<S>(imm, d, 0, false); break;
    case IMM_EORI: r = logic<S>(d ^ imm); break;
    case IMM_CMPI: cmp<S>(imm, d); break;
    }
    prefetch();
    if (mode == 0) {
        if (S == 4) idle(OP == IMM_CMPI ? 2 : 4);
        if (OP != IMM_CMPI) setD<S>(reg, r);
    } else if (OP != IMM_CMPI) {
        write<S>(addr, r);
    }
}

template <int OP> void Cpu::opImmSr(uint16_t op) {
    bool toSr = (op & 0x40) != 0;
    if (toSr && !sflag_) {
        privilegeViolation();
        return;
    }
    uint16_t imm = readExt();
    uint16_t v = getSR();
    switch (OP) {
    case IMM_ORI: v |= imm; break;
    case IMM_ANDI: v &= imm; break;
    case IMM_EORI: v ^= imm; break;
    }
    if (toSr)
        setSR(v);
    else
        setCCR((uint8_t)v);
    idle(8);
    refetch();
}

template <int S, bool SUB> void Cpu::opAddqSubq(uint16_t op) {
    int mode = MODE, reg = REG;
    uint32_t q = REG9;
    if (q == 0) q = 8;
    if (mode == 1) {
        a_[reg] = SUB ? a_[reg] - q : a_[reg] + q;
        prefetch();
        idle(4); // .w/.l とも 8 サイクル (SingleStepTests の .l = 6 は WinUAE と合わない)
        return;
    }
    if (mode == 0) {
        uint32_t r = SUB ? sub<S>(q, d_[reg], 0, false) : add<S>(q, d_[reg], 0, false);
        prefetch();
        if (S == 4) idle(4);
        setD<S>(reg, r);
        return;
    }
    uint32_t addr = ea<S>(mode, reg);
    uint32_t d = read<S>(addr);
    uint32_t r = SUB ? sub<S>(q, d, 0, false) : add<S>(q, d, 0, false);
    prefetch();
    write<S>(addr, r);
}

// ---------------------------------------------------------------- 転送

template <int S> void Cpu::opMove(uint16_t op) {
    int smode = MODE, sreg = REG;
    int dmode = (op >> 6) & 7, dreg = REG9;
    uint32_t addr = 0;
    uint32_t v = readOperand<S>(smode, sreg, addr);
    if (dmode == 0) {
        setNZ<S>(v);
        flagV_ = flagC_ = 0;
        prefetch();
        setD<S>(dreg, v);
        return;
    }
    if (dmode == 7 && dreg == 1 && !isRegOrImm(smode, sreg)) {
        // 転送元がメモリで転送先が絶対ロング: 下位語は IRC のまま書き込み、そのあとで詰め直す
        uint32_t hi = readExt();
        uint32_t daddr = (hi << 16) | irc_;
        setNZ<S>(v);
        flagV_ = flagC_ = 0;
        write<S>(daddr, v);
        readExt();
        prefetch();
        return;
    }
    if (dmode == 3) {
        // (An)+: 書き込みが終わってから An を進める
        setNZ<S>(v);
        flagV_ = flagC_ = 0;
        write<S>(a_[dreg], v);
        a_[dreg] += (S == 1 && dreg == 7) ? 2 : S;
        prefetch();
        return;
    }
    if (dmode == 4 && S == 4) {
        // -(An) のロング: 下位語の分だけ減らした時点でアドレスエラーを判定する
        setNZ<S>(v);
        flagV_ = flagC_ = 0;
        prefetch();
        a_[dreg] -= 2;
        if (a_[dreg] & 1) addressError(a_[dreg], true, false);
        a_[dreg] -= 2;
        writeLongLowFirst<4>(a_[dreg], v);
        return;
    }
    uint32_t daddr = ea<S>(dmode, dreg, 2, 0);
    setNZ<S>(v);
    flagV_ = flagC_ = 0;
    if (dmode == 4) {
        prefetch();
        if (S == 4)
            writeLongLowFirst<4>(daddr, v);
        else
            write<S>(daddr, v);
    } else {
        write<S>(daddr, v);
        prefetch();
    }
}

template <int S> void Cpu::opMovea(uint16_t op) {
    int smode = MODE, sreg = REG;
    uint32_t addr = 0;
    uint32_t v = readOperand<S>(smode, sreg, addr);
    if (S == 2) v = sext16(v);
    a_[REG9] = v;
    prefetch();
}

void Cpu::opMoveq(uint16_t op) {
    uint32_t v = sext8(op);
    d_[REG9] = v;
    setNZ<4>(v);
    flagV_ = flagC_ = 0;
    prefetch();
}

template <int S, int OP> void Cpu::opUnary(uint16_t op) {
    int mode = MODE, reg = REG;
    uint32_t d, addr = 0;
    if (mode == 0)
        d = d_[reg];
    else {
        addr = ea<S>(mode, reg);
        d = read<S>(addr);
    }
    uint32_t r = 0;
    switch (OP) {
    case UN_NEGX: r = sub<S>(d, 0, flagX_, true); break;
    case UN_CLR:
        r = 0;
        flagN_ = flagV_ = flagC_ = 0;
        flagZ_ = 1;
        break;
    case UN_NEG: r = sub<S>(d, 0, 0, false); break;
    case UN_NOT: r = logic<S>(~d); break;
    }
    prefetch();
    if (mode == 0) {
        if (S == 4) idle(2);
        setD<S>(reg, r);
    } else {
        write<S>(addr, r);
    }
}

template <int S> void Cpu::opTst(uint16_t op) {
    uint32_t addr = 0;
    uint32_t v = readOperand<S>(MODE, REG, addr);
    logic<S>(v);
    prefetch();
}

void Cpu::opTas(uint16_t op) {
    int mode = MODE, reg = REG;
    if (mode == 0) {
        logic<1>(d_[reg]);
        d_[reg] |= 0x80;
        prefetch();
        return;
    }
    uint32_t addr = ea<1>(mode, reg);
    cycles_ += 4;
    uint8_t v = bus_->tasRead(addr & 0xffffff, fcData());
    logic<1>(v);
    idle(2);
    cycles_ += 4;
    bus_->tasWrite(addr & 0xffffff, v | 0x80, fcData());
    prefetch();
}

void Cpu::opScc(uint16_t op) {
    int mode = MODE, reg = REG;
    bool t = cond((op >> 8) & 15);
    uint32_t v = t ? 0xff : 0;
    if (mode == 0) {
        prefetch();
        if (t) idle(2);
        setD<1>(reg, v);
        return;
    }
    uint32_t addr = ea<1>(mode, reg);
    read<1>(addr);
    prefetch();
    write<1>(addr, v);
}

void Cpu::opSwap(uint16_t op) {
    uint32_t v = d_[REG];
    v = (v >> 16) | (v << 16);
    d_[REG] = v;
    logic<4>(v);
    prefetch();
}

template <int S> void Cpu::opExt(uint16_t op) {
    int reg = REG;
    if (S == 2) {
        uint32_t v = sext8(d_[reg]) & 0xffff;
        setD<2>(reg, v);
        logic<2>(v);
    } else {
        uint32_t v = sext16(d_[reg]);
        d_[reg] = v;
        logic<4>(v);
    }
    prefetch();
}

void Cpu::opExg(uint16_t op) {
    int rx = REG9, ry = REG;
    uint32_t t;
    switch ((op >> 3) & 0x1f) {
    case 0x08: // Dx,Dy
        t = d_[rx];
        d_[rx] = d_[ry];
        d_[ry] = t;
        break;
    case 0x09: // Ax,Ay
        t = a_[rx];
        a_[rx] = a_[ry];
        a_[ry] = t;
        break;
    case 0x11: // Dx,Ay
        t = d_[rx];
        d_[rx] = a_[ry];
        a_[ry] = t;
        break;
    }
    prefetch();
    idle(2);
}

void Cpu::opMoveFromSr(uint16_t op) {
    int mode = MODE, reg = REG;
    uint16_t v = getSR();
    if (mode == 0) {
        prefetch();
        idle(2);
        setD<2>(reg, v);
        return;
    }
    uint32_t addr = ea<2>(mode, reg);
    read<2>(addr);
    prefetch();
    write<2>(addr, v);
}

void Cpu::opMoveToCcr(uint16_t op) {
    uint32_t addr = 0;
    uint32_t v = readOperand<2>(MODE, REG, addr);
    setCCR((uint8_t)v);
    idle(4);
    refetch();
}

void Cpu::opMoveToSr(uint16_t op) {
    if (!sflag_) {
        privilegeViolation();
        return;
    }
    uint32_t addr = 0;
    uint32_t v = readOperand<2>(MODE, REG, addr);
    setSR((uint16_t)v);
    idle(4);
    refetch();
}

void Cpu::opMoveUsp(uint16_t op) {
    if (!sflag_) {
        privilegeViolation();
        return;
    }
    if (op & 8)
        a_[REG] = inactiveSp_;
    else
        inactiveSp_ = a_[REG];
    prefetch();
}

template <int S> void Cpu::opMovemToMem(uint16_t op) {
    int mode = MODE, reg = REG;
    uint16_t mask = readExt();
    if (mode == 4) {
        uint32_t addr = a_[reg];
        for (int i = 0; i < 16; i++) {
            if (!(mask & (1 << i))) continue;
            int r = 15 - i;
            uint32_t v = r < 8 ? d_[r] : a_[r - 8];
            addr -= S;
            if (S == 4)
                writeLongLowFirst<4>(addr, v);
            else
                write<2>(addr, v);
        }
        a_[reg] = addr;
    } else {
        uint32_t addr = ea<S>(mode, reg);
        for (int r = 0; r < 16; r++) {
            if (!(mask & (1 << r))) continue;
            uint32_t v = r < 8 ? d_[r] : a_[r - 8];
            write<S>(addr, v);
            addr += S;
        }
    }
    prefetch();
}

template <int S> void Cpu::opMovemToReg(uint16_t op) {
    int mode = MODE, reg = REG;
    uint16_t mask = readExt();
    uint32_t addr = (mode == 3) ? a_[reg] : ea<S>(mode, reg);
    const bool pcRel = mode == 7 && (reg == 2 || reg == 3);
    for (int r = 0; r < 16; r++) {
        if (!(mask & (1 << r))) continue;
        if (mode == 3) a_[reg] = addr + 2; // アドレスエラー時に見える An の値
        uint32_t v = read<S>(addr, pcRel);
        if (S == 2) v = sext16(v);
        if (r < 8)
            d_[r] = v;
        else
            a_[r - 8] = v;
        addr += S;
    }
    read<2>(addr, pcRel);
    if (mode == 3) a_[reg] = addr;
    prefetch();
}

template <int S> void Cpu::opMovep(uint16_t op) {
    int dx = REG9, ay = REG;
    uint32_t addr = a_[ay] + sext16(readExt());
    if (!(op & 0x80)) { // メモリ → レジスタ
        uint32_t v = 0;
        for (int i = 0; i < S; i++) v = (v << 8) | read<1>(addr + 2 * i);
        setD<S>(dx, v);
    } else {
        uint32_t v = d_[dx];
        for (int i = 0; i < S; i++) write<1>(addr + 2 * i, v >> (8 * (S - 1 - i)));
    }
    prefetch();
}

// ---------------------------------------------------------------- プログラム制御

void Cpu::opBcc(uint16_t op) {
    int cc = (op >> 8) & 15;
    uint32_t disp = sext8(op);
    bool word = (op & 0xff) == 0;
    if (word) disp = sext16(irc_);
    uint32_t target = pc_ + 2 + disp;
    if (cc == 1) { // BSR
        uint32_t ret = pc_ + (word ? 4 : 2);
        idle(2);
        push32(ret);
        jump(target);
        return;
    }
    if (cond(cc)) {
        idle(2);
        jump(target);
        return;
    }
    idle(4);
    if (word) readExt();
    prefetch();
}

void Cpu::opDbcc(uint16_t op) {
    int reg = REG;
    if (cond((op >> 8) & 15)) {
        idle(4);
        readExt();
        prefetch();
        return;
    }
    uint32_t target = pc_ + 2 + sext16(irc_);
    uint16_t cnt = (uint16_t)(d_[reg] - 1);
    setD<2>(reg, cnt);
    if (cnt != 0xffff) {
        idle(2);
        jump(target);
        return;
    }
    idle(2);
    fetch(target);
    readExt();
    prefetch();
}

void Cpu::opLea(uint16_t op) {
    a_[REG9] = ea<4>(MODE, REG, 4);
    prefetch();
}

void Cpu::opPea(uint16_t op) {
    uint32_t addr = ea<4>(MODE, REG, 4);
    prefetch();
    push32(addr);
}

void Cpu::opJmp(uint16_t op) {
    uint32_t t = eaControlJump(MODE, REG);
    jump(t);
}

static inline int jumpExtWords(int mode, int reg) {
    if (mode == 2) return 0;
    if (mode == 7 && reg == 1) return 2;
    return 1;
}

void Cpu::opJsr(uint16_t op) {
    int mode = MODE, reg = REG;
    uint32_t t = eaControlJump(mode, reg);
    uint32_t ret = pc_ + 2 + 2 * jumpExtWords(mode, reg);
    if (t & 1) jump(t); // 分岐先が奇数ならリターンアドレスを積む前にアドレスエラー
    push32(ret);
    jump(t);
}

void Cpu::opRts(uint16_t) {
    uint32_t t = pop32();
    jump(t);
}

void Cpu::opRtr(uint16_t) {
    uint16_t ccr = (uint16_t)read<2>(a_[7]);
    a_[7] += 2;
    uint32_t t = pop32();
    setCCR((uint8_t)ccr);
    jump(t);
}

void Cpu::opRte(uint16_t) {
    if (!sflag_) {
        privilegeViolation();
        return;
    }
    uint32_t sp = a_[7];
    uint16_t sr = (uint16_t)read<2>(sp);
    uint32_t pc = read<4>(sp + 2);
    a_[7] = sp + 6;
    setSR(sr);
    jump(pc);
}

void Cpu::opTrap(uint16_t op) { exception(32 + (op & 15), pc_ + 2, 4); }

void Cpu::opTrapv(uint16_t) {
    prefetch();
    if (flagV_) exception(7, pc_, 0);
}

void Cpu::opLink(uint16_t op) {
    int an = REG;
    uint32_t disp = sext16(readExt());
    a_[7] -= 4;
    writeLongLowFirst<4>(a_[7], a_[an]);
    a_[an] = a_[7];
    a_[7] += disp;
    prefetch();
}

void Cpu::opUnlk(uint16_t op) {
    int an = REG;
    a_[7] = a_[an];
    uint32_t v = read<4>(a_[7]);
    a_[7] += 4;
    a_[an] = v;
    prefetch();
}

void Cpu::opNop(uint16_t) { prefetch(); }

void Cpu::opReset(uint16_t) {
    if (!sflag_) {
        privilegeViolation();
        return;
    }
    bus_->resetDevices();
    idle(128);
    prefetch();
}

void Cpu::opStop(uint16_t) {
    if (!sflag_) {
        privilegeViolation();
        return;
    }
    uint16_t imm = readExt();
    setSR(imm);
    prefetch();
    stopped_ = true;
}

// 実行されなかった命令 (不当命令、A/F ライン、特権違反) のあとはトレース例外を起こさない
void Cpu::opIllegal(uint16_t op) {
    noTrace_ = true;
    illegal(op);
}
void Cpu::opLineA(uint16_t) {
    noTrace_ = true;
    exception(10, pc_, 4);
}
void Cpu::opLineF(uint16_t) {
    noTrace_ = true;
    exception(11, pc_, 4);
}

// ---------------------------------------------------------------- ビット操作

template <int OP> static inline uint32_t bitOp(uint32_t v, int bit) {
    switch (OP) {
    case BIT_CHG: return v ^ (1u << bit);
    case BIT_CLR: return v & ~(1u << bit);
    case BIT_SET: return v | (1u << bit);
    default: return v;
    }
}

static inline int bitRegIdle(int op, int bit) {
    if (op == BIT_TST) return 2;
    int n = bit < 16 ? 2 : 4;
    return op == BIT_CLR ? n + 2 : n;
}

template <int OP> void Cpu::opBitDyn(uint16_t op) {
    int mode = MODE, reg = REG;
    uint32_t bitsrc = d_[REG9];
    if (mode == 0) {
        int bit = bitsrc & 31;
        flagZ_ = ((d_[reg] >> bit) & 1) ? 0 : 1;
        prefetch();
        idle(bitRegIdle(OP, bit));
        d_[reg] = bitOp<OP>(d_[reg], bit);
        return;
    }
    int bit = bitsrc & 7;
    uint32_t addr = 0;
    uint32_t v = readOperand<1>(mode, reg, addr);
    flagZ_ = ((v >> bit) & 1) ? 0 : 1;
    prefetch();
    if (OP == BIT_TST) {
        if (mode == 7 && reg == 4) idle(2);
        return;
    }
    write<1>(addr, bitOp<OP>(v, bit));
}

template <int OP> void Cpu::opBitImm(uint16_t op) {
    int mode = MODE, reg = REG;
    int bitsrc = readExt() & 0xff;
    if (mode == 0) {
        int bit = bitsrc & 31;
        flagZ_ = ((d_[reg] >> bit) & 1) ? 0 : 1;
        prefetch();
        idle(bitRegIdle(OP, bit));
        d_[reg] = bitOp<OP>(d_[reg], bit);
        return;
    }
    int bit = bitsrc & 7;
    uint32_t addr = ea<1>(mode, reg);
    uint32_t v = read<1>(addr, mode == 7 && (reg == 2 || reg == 3));
    flagZ_ = ((v >> bit) & 1) ? 0 : 1;
    prefetch();
    if (OP != BIT_TST) write<1>(addr, bitOp<OP>(v, bit));
}

// ---------------------------------------------------------------- シフト・ローテート

template <int S, int TYPE, bool LEFT> uint32_t Cpu::shift(uint32_t v, int count) {
    const uint32_t m = Mask<S>::v, msb = Msb<S>::v;
    v &= m;
    flagV_ = 0;
    if (count == 0) {
        flagC_ = (TYPE == SH_ROX) ? flagX_ : 0;
        setNZ<S>(v);
        return v;
    }
    uint32_t c = 0;
    for (int i = 0; i < count; i++) {
        if (LEFT) {
            c = (v & msb) ? 1 : 0;
            uint32_t nv = (v << 1) & m;
            if (TYPE == SH_ROX) nv |= flagX_;
            if (TYPE == SH_RO) nv |= c;
            if (TYPE == SH_AS && ((nv ^ v) & msb)) flagV_ = 1;
            v = nv;
        } else {
            c = v & 1;
            uint32_t nv = v >> 1;
            if (TYPE == SH_AS) nv |= v & msb;
            if (TYPE == SH_ROX && flagX_) nv |= msb;
            if (TYPE == SH_RO && c) nv |= msb;
            v = nv;
        }
        if (TYPE == SH_ROX) flagX_ = c;
    }
    flagC_ = c;
    if (TYPE == SH_AS || TYPE == SH_LS) flagX_ = c;
    setNZ<S>(v);
    return v;
}

template <int S, int TYPE, bool LEFT> void Cpu::opShiftReg(uint16_t op) {
    int reg = REG;
    int count = (op & 0x20) ? (int)(d_[REG9] & 63) : (REG9 ? REG9 : 8);
    uint32_t r = shift<S, TYPE, LEFT>(d_[reg], count);
    prefetch();
    idle((S == 4 ? 4 : 2) + 2 * count);
    setD<S>(reg, r);
}

template <int TYPE, bool LEFT> void Cpu::opShiftMem(uint16_t op) {
    uint32_t addr = ea<2>(MODE, REG);
    uint32_t v = read<2>(addr);
    uint32_t r = shift<2, TYPE, LEFT>(v, 1);
    prefetch();
    write<2>(addr, r);
}

// ---------------------------------------------------------------- 乗除算

template <bool SIGNED> void Cpu::opMul(uint16_t op) {
    uint32_t addr = 0;
    uint32_t s = readOperand<2>(MODE, REG, addr);
    int dn = REG9;
    uint32_t r;
    int n;
    if (SIGNED) {
        r = (uint32_t)((int32_t)(int16_t)d_[dn] * (int32_t)(int16_t)s);
        n = popcount16(s ^ (s << 1));
    } else {
        r = (d_[dn] & 0xffff) * s;
        n = popcount16(s);
    }
    d_[dn] = r;
    logic<4>(r);
    prefetch();
    idle(34 + 2 * n);
}

static int divuCycles(uint32_t dividend, uint16_t divisor) {
    if ((dividend >> 16) >= divisor) return 10;
    int mcycles = 38;
    uint32_t hdivisor = (uint32_t)divisor << 16;
    for (int i = 0; i < 15; i++) {
        uint32_t temp = dividend;
        dividend <<= 1;
        if ((int32_t)temp < 0) {
            dividend -= hdivisor;
        } else {
            mcycles += 2;
            if (dividend >= hdivisor) {
                dividend -= hdivisor;
                mcycles--;
            }
        }
    }
    return mcycles * 2;
}

static int divsCycles(int32_t dividend, int16_t divisor) {
    int mcycles = 6;
    if (dividend < 0) mcycles++;
    uint32_t adividend = dividend < 0 ? (uint32_t)-(int64_t)dividend : (uint32_t)dividend;
    uint32_t adivisor = divisor < 0 ? (uint32_t)-(int32_t)divisor : (uint32_t)divisor;
    if ((adividend >> 16) >= adivisor) return (mcycles + 2) * 2;
    mcycles += 55;
    if (divisor >= 0) {
        if (dividend >= 0)
            mcycles--;
        else
            mcycles++;
    }
    uint32_t aquot = adividend / adivisor;
    for (int i = 0; i < 15; i++) {
        if ((int16_t)aquot >= 0) mcycles++;
        aquot <<= 1;
    }
    return mcycles * 2;
}

template <bool SIGNED> void Cpu::opDiv(uint16_t op) {
    uint32_t addr = 0;
    uint32_t s = readOperand<2>(MODE, REG, addr);
    int dn = REG9;
    uint32_t d = d_[dn];
    if (s == 0) {
        // ゼロ除算の未定義フラグ (WinUAE による 68000 の実機の値):
        // DIVU は N=上位語<0, Z=上位語==0, V=C=0。DIVS は Z=1, N=V=C=0
        flagV_ = flagC_ = 0;
        if (SIGNED) {
            flagN_ = 0;
            flagZ_ = 1;
        } else {
            flagN_ = (d & 0x80000000) ? 1 : 0;
            flagZ_ = (d >> 16) == 0;
        }
        exception(5, pc_ + 2, 8);
        return;
    }
    if (!SIGNED) {
        uint32_t q = d / s, rem = d % s;
        idle(divuCycles(d, (uint16_t)s) - 4);
        if (q > 0xffff) {
            // オーバーフロー: V=1, N=1, Z=0, C=0 (WinUAE)
            flagV_ = flagN_ = 1;
            flagZ_ = flagC_ = 0;
        } else {
            d_[dn] = (rem << 16) | q;
            flagN_ = (q & 0x8000) ? 1 : 0;
            flagZ_ = q ? 0 : 1;
            flagV_ = flagC_ = 0;
        }
    } else {
        int32_t dividend = (int32_t)d;
        int32_t divisor = (int16_t)s;
        idle(divsCycles(dividend, (int16_t)divisor) - 4);
        int64_t q = (int64_t)dividend / divisor;
        int64_t rem = (int64_t)dividend % divisor;
        if (q < -32768 || q > 32767) {
            flagV_ = flagN_ = 1;
            flagZ_ = flagC_ = 0;
        } else {
            d_[dn] = ((uint32_t)(rem & 0xffff) << 16) | ((uint32_t)q & 0xffff);
            flagN_ = (q & 0x8000) ? 1 : 0;
            flagZ_ = (q & 0xffff) ? 0 : 1;
            flagV_ = flagC_ = 0;
        }
    }
    prefetch();
}

void Cpu::opChk(uint16_t op) {
    uint32_t addr = 0;
    int16_t bound = (int16_t)readOperand<2>(MODE, REG, addr);
    int16_t v = (int16_t)d_[REG9];
    // 未定義フラグは SingleStepTests に合わせる: Z/V/C は 0 (Z は Dn=0 のとき 1)、
    // トラップしないとき N は変えない
    flagZ_ = v == 0;
    flagV_ = flagC_ = 0;
    if (v > bound) { // 上限の比較が先
        flagN_ = v < 0;
        exception(6, pc_ + 2, 8);
        return;
    }
    if (v < 0) {
        flagN_ = 1;
        exception(6, pc_ + 2, 10);
        return;
    }
    prefetch();
    idle(6);
}

// ---------------------------------------------------------------- BCD

template <bool SUB> void Cpu::opBcd(uint16_t op) {
    int rx = REG9, ry = REG;
    if (!(op & 8)) {
        uint8_t r = SUB ? sbcd((uint8_t)d_[ry], (uint8_t)d_[rx]) : abcd((uint8_t)d_[ry], (uint8_t)d_[rx]);
        prefetch();
        idle(2);
        setD<1>(rx, r);
        return;
    }
    idle(2);
    a_[ry] -= (ry == 7) ? 2 : 1;
    uint8_t s = (uint8_t)read<1>(a_[ry]);
    a_[rx] -= (rx == 7) ? 2 : 1;
    uint8_t d = (uint8_t)read<1>(a_[rx]);
    uint8_t r = SUB ? sbcd(s, d) : abcd(s, d);
    prefetch();
    write<1>(a_[rx], r);
}

void Cpu::opNbcd(uint16_t op) {
    int mode = MODE, reg = REG;
    if (mode == 0) {
        uint8_t r = sbcd((uint8_t)d_[reg], 0);
        prefetch();
        idle(2);
        setD<1>(reg, r);
        return;
    }
    uint32_t addr = ea<1>(mode, reg);
    uint8_t r = sbcd((uint8_t)read<1>(addr), 0);
    prefetch();
    write<1>(addr, r);
}

#undef MODE
#undef REG
#undef REG9

} // namespace m68k
