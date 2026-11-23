// オペコード → 命令実装の表を作る (m68k.cpp から include する)
// 68000 で不当なアドレッシングモードの組み合わせは、ILLEGAL / LINE-A / LINE-F の例外にする。

namespace m68k {

namespace {

enum : int {
    EA_DN = 1 << 0,
    EA_AN = 1 << 1,
    EA_AI = 1 << 2,
    EA_PI = 1 << 3,
    EA_PD = 1 << 4,
    EA_DI = 1 << 5,
    EA_IX = 1 << 6,
    EA_AW = 1 << 7,
    EA_AL = 1 << 8,
    EA_PCD = 1 << 9,
    EA_PCX = 1 << 10,
    EA_IM = 1 << 11,
    EA_ALL = 0xfff,
    EA_DATA = EA_ALL & ~EA_AN,
    EA_MEM = EA_ALL & ~(EA_DN | EA_AN),
    EA_ALT = EA_DN | EA_AN | EA_AI | EA_PI | EA_PD | EA_DI | EA_IX | EA_AW | EA_AL,
    EA_DALT = EA_ALT & ~EA_AN,
    EA_MALT = EA_DALT & ~EA_DN,
    EA_CTRL = EA_AI | EA_DI | EA_IX | EA_AW | EA_AL | EA_PCD | EA_PCX,
};

int eaBit(int mode, int reg) {
    if (mode < 7) return 1 << mode;
    if (reg <= 4) return 1 << (7 + reg);
    return 0;
}

bool eaOk(uint16_t op, int set) { return (eaBit((op >> 3) & 7, op & 7) & set) != 0; }

using H = Cpu::Handler;

template <int OP> H arithEaDn(int sz) {
    switch (sz) {
    case 0: return &Cpu::thunk<&Cpu::opArithEaDn<1, OP>>;
    case 1: return &Cpu::thunk<&Cpu::opArithEaDn<2, OP>>;
    case 2: return &Cpu::thunk<&Cpu::opArithEaDn<4, OP>>;
    }
    return nullptr;
}
template <int OP> H arithDnEa(int sz) {
    switch (sz) {
    case 0: return &Cpu::thunk<&Cpu::opArithDnEa<1, OP>>;
    case 1: return &Cpu::thunk<&Cpu::opArithDnEa<2, OP>>;
    case 2: return &Cpu::thunk<&Cpu::opArithDnEa<4, OP>>;
    }
    return nullptr;
}
template <bool SUB> H addxSubx(int sz) {
    switch (sz) {
    case 0: return &Cpu::thunk<&Cpu::opAddxSubx<1, SUB>>;
    case 1: return &Cpu::thunk<&Cpu::opAddxSubx<2, SUB>>;
    case 2: return &Cpu::thunk<&Cpu::opAddxSubx<4, SUB>>;
    }
    return nullptr;
}
template <int OP> H imm(int sz) {
    switch (sz) {
    case 0: return &Cpu::thunk<&Cpu::opImm<1, OP>>;
    case 1: return &Cpu::thunk<&Cpu::opImm<2, OP>>;
    case 2: return &Cpu::thunk<&Cpu::opImm<4, OP>>;
    }
    return nullptr;
}
template <int OP> H unary(int sz) {
    switch (sz) {
    case 0: return &Cpu::thunk<&Cpu::opUnary<1, OP>>;
    case 1: return &Cpu::thunk<&Cpu::opUnary<2, OP>>;
    case 2: return &Cpu::thunk<&Cpu::opUnary<4, OP>>;
    }
    return nullptr;
}
H addq(int sz) {
    switch (sz) {
    case 0: return &Cpu::thunk<&Cpu::opAddq<1>>;
    case 1: return &Cpu::thunk<&Cpu::opAddq<2>>;
    case 2: return &Cpu::thunk<&Cpu::opAddq<4>>;
    }
    return nullptr;
}
H subq(int sz) {
    switch (sz) {
    case 0: return &Cpu::thunk<&Cpu::opSubq<1>>;
    case 1: return &Cpu::thunk<&Cpu::opSubq<2>>;
    case 2: return &Cpu::thunk<&Cpu::opSubq<4>>;
    }
    return nullptr;
}
H tst(int sz) {
    switch (sz) {
    case 0: return &Cpu::thunk<&Cpu::opTst<1>>;
    case 1: return &Cpu::thunk<&Cpu::opTst<2>>;
    case 2: return &Cpu::thunk<&Cpu::opTst<4>>;
    }
    return nullptr;
}
H cmpm(int sz) {
    switch (sz) {
    case 0: return &Cpu::thunk<&Cpu::opCmpm<1>>;
    case 1: return &Cpu::thunk<&Cpu::opCmpm<2>>;
    case 2: return &Cpu::thunk<&Cpu::opCmpm<4>>;
    }
    return nullptr;
}
template <int TYPE, bool LEFT> H shiftReg(int sz) {
    switch (sz) {
    case 0: return &Cpu::thunk<&Cpu::opShiftReg<1, TYPE, LEFT>>;
    case 1: return &Cpu::thunk<&Cpu::opShiftReg<2, TYPE, LEFT>>;
    case 2: return &Cpu::thunk<&Cpu::opShiftReg<4, TYPE, LEFT>>;
    }
    return nullptr;
}
template <bool LEFT> H shiftRegType(int type, int sz) {
    switch (type) {
    case SH_AS: return shiftReg<SH_AS, LEFT>(sz);
    case SH_LS: return shiftReg<SH_LS, LEFT>(sz);
    case SH_ROX: return shiftReg<SH_ROX, LEFT>(sz);
    case SH_RO: return shiftReg<SH_RO, LEFT>(sz);
    }
    return nullptr;
}
template <bool LEFT> H shiftMemType(int type) {
    switch (type) {
    case SH_AS: return &Cpu::thunk<&Cpu::opShiftMem<SH_AS, LEFT>>;
    case SH_LS: return &Cpu::thunk<&Cpu::opShiftMem<SH_LS, LEFT>>;
    case SH_ROX: return &Cpu::thunk<&Cpu::opShiftMem<SH_ROX, LEFT>>;
    case SH_RO: return &Cpu::thunk<&Cpu::opShiftMem<SH_RO, LEFT>>;
    }
    return nullptr;
}
H bitDyn(int t) {
    switch (t) {
    case 0: return &Cpu::thunk<&Cpu::opBitDyn<BIT_TST>>;
    case 1: return &Cpu::thunk<&Cpu::opBitDyn<BIT_CHG>>;
    case 2: return &Cpu::thunk<&Cpu::opBitDyn<BIT_CLR>>;
    case 3: return &Cpu::thunk<&Cpu::opBitDyn<BIT_SET>>;
    }
    return nullptr;
}
H bitImm(int t) {
    switch (t) {
    case 0: return &Cpu::thunk<&Cpu::opBitImm<BIT_TST>>;
    case 1: return &Cpu::thunk<&Cpu::opBitImm<BIT_CHG>>;
    case 2: return &Cpu::thunk<&Cpu::opBitImm<BIT_CLR>>;
    case 3: return &Cpu::thunk<&Cpu::opBitImm<BIT_SET>>;
    }
    return nullptr;
}

// 加減算・論理演算 (0x8, 0x9, 0xB, 0xC, 0xD 系の共通部分)
// OP_EA_DN: <ea>,Dn  OP_DN_EA: Dn,<ea>
template <int OP> H arith(uint16_t op, int eaDnSet) {
    int opmode = (op >> 6) & 7;
    int sz = opmode & 3;
    if (opmode < 3) {
        int set = eaDnSet;
        if (sz == 0) set &= ~EA_AN;
        return eaOk(op, set) ? arithEaDn<OP>(sz) : nullptr;
    }
    if (opmode >= 4 && opmode <= 6) return eaOk(op, EA_MALT) ? arithDnEa<OP>(sz) : nullptr;
    return nullptr;
}

H decodeLine0(uint16_t op) {
    int sz = (op >> 6) & 3;
    if ((op & 0x0138) == 0x0108) return (op & 0x40) ? &Cpu::thunk<&Cpu::opMovep<4>> : &Cpu::thunk<&Cpu::opMovep<2>>;
    if (op & 0x0100) {
        int t = (op >> 6) & 3;
        return eaOk(op, t == 0 ? EA_DATA : EA_DALT) ? bitDyn(t) : nullptr;
    }
    if ((op & 0x0f00) == 0x0800) {
        int t = (op >> 6) & 3;
        return eaOk(op, t == 0 ? (EA_DATA & ~EA_IM) : EA_DALT) ? bitImm(t) : nullptr;
    }
    int k = (op >> 9) & 7;
    if ((op & 0xff) == 0x3c || (op & 0xff) == 0x7c) {
        switch (k) {
        case 0: return &Cpu::thunk<&Cpu::opImmSr<IMM_ORI>>;
        case 1: return &Cpu::thunk<&Cpu::opImmSr<IMM_ANDI>>;
        case 5: return &Cpu::thunk<&Cpu::opImmSr<IMM_EORI>>;
        }
        return nullptr;
    }
    if (sz == 3 || !eaOk(op, EA_DALT)) return nullptr;
    switch (k) {
    case 0: return imm<IMM_ORI>(sz);
    case 1: return imm<IMM_ANDI>(sz);
    case 2: return imm<IMM_SUBI>(sz);
    case 3: return imm<IMM_ADDI>(sz);
    case 5: return imm<IMM_EORI>(sz);
    case 6: return imm<IMM_CMPI>(sz);
    }
    return nullptr;
}

H decodeMove(uint16_t op) {
    int line = op >> 12;
    int S = line == 1 ? 1 : line == 3 ? 2 : 4;
    int dm = (op >> 6) & 7, dr = (op >> 9) & 7;
    int srcSet = EA_ALL;
    if (S == 1) srcSet &= ~EA_AN;
    if (!eaOk(op, srcSet)) return nullptr;
    if (dm == 1) {
        if (S == 1) return nullptr;
        return S == 2 ? &Cpu::thunk<&Cpu::opMovea<2>> : &Cpu::thunk<&Cpu::opMovea<4>>;
    }
    if (!(eaBit(dm, dr) & EA_DALT)) return nullptr;
    switch (S) {
    case 1: return &Cpu::thunk<&Cpu::opMove<1>>;
    case 2: return &Cpu::thunk<&Cpu::opMove<2>>;
    default: return &Cpu::thunk<&Cpu::opMove<4>>;
    }
}

H decodeLine4(uint16_t op) {
    int sz = (op >> 6) & 3;
    int mode = (op >> 3) & 7;
    switch (op) {
    case 0x4afc: return &Cpu::thunk<&Cpu::opIllegal>;
    case 0x4e70: return &Cpu::thunk<&Cpu::opReset>;
    case 0x4e71: return &Cpu::thunk<&Cpu::opNop>;
    case 0x4e72: return &Cpu::thunk<&Cpu::opStop>;
    case 0x4e73: return &Cpu::thunk<&Cpu::opRte>;
    case 0x4e75: return &Cpu::thunk<&Cpu::opRts>;
    case 0x4e76: return &Cpu::thunk<&Cpu::opTrapv>;
    case 0x4e77: return &Cpu::thunk<&Cpu::opRtr>;
    }
    if ((op & 0xfff0) == 0x4e40) return &Cpu::thunk<&Cpu::opTrap>;
    if ((op & 0xfff8) == 0x4e50) return &Cpu::thunk<&Cpu::opLink>;
    if ((op & 0xfff8) == 0x4e58) return &Cpu::thunk<&Cpu::opUnlk>;
    if ((op & 0xfff0) == 0x4e60) return &Cpu::thunk<&Cpu::opMoveUsp>;
    if ((op & 0xffc0) == 0x4e80) return eaOk(op, EA_CTRL) ? &Cpu::thunk<&Cpu::opJsr> : nullptr;
    if ((op & 0xffc0) == 0x4ec0) return eaOk(op, EA_CTRL) ? &Cpu::thunk<&Cpu::opJmp> : nullptr;
    if ((op & 0xf1c0) == 0x41c0) return eaOk(op, EA_CTRL) ? &Cpu::thunk<&Cpu::opLea> : nullptr;
    if ((op & 0xf1c0) == 0x4180) return eaOk(op, EA_DATA) ? &Cpu::thunk<&Cpu::opChk> : nullptr;
    if ((op & 0xffc0) == 0x40c0) return eaOk(op, EA_DALT) ? &Cpu::thunk<&Cpu::opMoveFromSr> : nullptr;
    if ((op & 0xffc0) == 0x44c0) return eaOk(op, EA_DATA) ? &Cpu::thunk<&Cpu::opMoveToCcr> : nullptr;
    if ((op & 0xffc0) == 0x46c0) return eaOk(op, EA_DATA) ? &Cpu::thunk<&Cpu::opMoveToSr> : nullptr;
    if ((op & 0xff00) == 0x4000 && sz != 3) return eaOk(op, EA_DALT) ? unary<UN_NEGX>(sz) : nullptr;
    if ((op & 0xff00) == 0x4200 && sz != 3) return eaOk(op, EA_DALT) ? unary<UN_CLR>(sz) : nullptr;
    if ((op & 0xff00) == 0x4400 && sz != 3) return eaOk(op, EA_DALT) ? unary<UN_NEG>(sz) : nullptr;
    if ((op & 0xff00) == 0x4600 && sz != 3) return eaOk(op, EA_DALT) ? unary<UN_NOT>(sz) : nullptr;
    if ((op & 0xffc0) == 0x4800) return eaOk(op, EA_DALT) ? &Cpu::thunk<&Cpu::opNbcd> : nullptr;
    if ((op & 0xfff8) == 0x4840) return &Cpu::thunk<&Cpu::opSwap>;
    if ((op & 0xffc0) == 0x4840) return eaOk(op, EA_CTRL) ? &Cpu::thunk<&Cpu::opPea> : nullptr;
    if ((op & 0xfff8) == 0x4880) return &Cpu::thunk<&Cpu::opExt<2>>;
    if ((op & 0xfff8) == 0x48c0) return &Cpu::thunk<&Cpu::opExt<4>>;
    if ((op & 0xff80) == 0x4880 && mode != 0) {
        if (!eaOk(op, EA_AI | EA_PD | EA_DI | EA_IX | EA_AW | EA_AL)) return nullptr;
        return (op & 0x40) ? &Cpu::thunk<&Cpu::opMovemToMem<4>> : &Cpu::thunk<&Cpu::opMovemToMem<2>>;
    }
    if ((op & 0xff80) == 0x4c80) {
        if (!eaOk(op, EA_AI | EA_PI | EA_DI | EA_IX | EA_AW | EA_AL | EA_PCD | EA_PCX)) return nullptr;
        return (op & 0x40) ? &Cpu::thunk<&Cpu::opMovemToReg<4>> : &Cpu::thunk<&Cpu::opMovemToReg<2>>;
    }
    if ((op & 0xffc0) == 0x4ac0) return eaOk(op, EA_DALT) ? &Cpu::thunk<&Cpu::opTas> : nullptr;
    if ((op & 0xff00) == 0x4a00 && sz != 3) return eaOk(op, EA_DALT) ? tst(sz) : nullptr;
    return nullptr;
}

H decodeLine5(uint16_t op) {
    int sz = (op >> 6) & 3;
    int mode = (op >> 3) & 7;
    if (sz == 3) {
        if (mode == 1) return &Cpu::thunk<&Cpu::opDbcc>;
        return eaOk(op, EA_DALT) ? &Cpu::thunk<&Cpu::opScc> : nullptr;
    }
    int set = EA_ALT;
    if (sz == 0) set &= ~EA_AN;
    if (!eaOk(op, set)) return nullptr;
    return (op & 0x100) ? subq(sz) : addq(sz);
}

H decodeLine8(uint16_t op) {
    int opmode = (op >> 6) & 7;
    int mode = (op >> 3) & 7;
    if (opmode == 3) return eaOk(op, EA_DATA) ? &Cpu::thunk<&Cpu::opDiv<false>> : nullptr;
    if (opmode == 7) return eaOk(op, EA_DATA) ? &Cpu::thunk<&Cpu::opDiv<true>> : nullptr;
    if (opmode == 4 && mode <= 1) return &Cpu::thunk<&Cpu::opBcd<true>>;
    return arith<OP_OR>(op, EA_DATA);
}

H decodeAddSub(uint16_t op, bool sub) {
    int opmode = (op >> 6) & 7;
    int mode = (op >> 3) & 7;
    if (opmode == 3) {
        if (!eaOk(op, EA_ALL)) return nullptr;
        return sub ? &Cpu::thunk<&Cpu::opAdda<2, true>> : &Cpu::thunk<&Cpu::opAdda<2, false>>;
    }
    if (opmode == 7) {
        if (!eaOk(op, EA_ALL)) return nullptr;
        return sub ? &Cpu::thunk<&Cpu::opAdda<4, true>> : &Cpu::thunk<&Cpu::opAdda<4, false>>;
    }
    if (opmode >= 4 && mode <= 1) return sub ? addxSubx<true>(opmode & 3) : addxSubx<false>(opmode & 3);
    return sub ? arith<OP_SUB>(op, EA_ALL) : arith<OP_ADD>(op, EA_ALL);
}

H decodeLineB(uint16_t op) {
    int opmode = (op >> 6) & 7;
    int mode = (op >> 3) & 7;
    if (opmode == 3) return eaOk(op, EA_ALL) ? &Cpu::thunk<&Cpu::opCmpa<2>> : nullptr;
    if (opmode == 7) return eaOk(op, EA_ALL) ? &Cpu::thunk<&Cpu::opCmpa<4>> : nullptr;
    if (opmode < 3) {
        int set = EA_ALL;
        if (opmode == 0) set &= ~EA_AN;
        return eaOk(op, set) ? arithEaDn<OP_CMP>(opmode) : nullptr;
    }
    if (mode == 1) return cmpm(opmode & 3);
    return eaOk(op, EA_DALT) ? arithDnEa<OP_EOR>(opmode & 3) : nullptr;
}

H decodeLineC(uint16_t op) {
    int opmode = (op >> 6) & 7;
    int mode = (op >> 3) & 7;
    if (opmode == 3) return eaOk(op, EA_DATA) ? &Cpu::thunk<&Cpu::opMul<false>> : nullptr;
    if (opmode == 7) return eaOk(op, EA_DATA) ? &Cpu::thunk<&Cpu::opMul<true>> : nullptr;
    if ((op & 0xf1f8) == 0xc140 || (op & 0xf1f8) == 0xc148 || (op & 0xf1f8) == 0xc188) return &Cpu::thunk<&Cpu::opExg>;
    if (opmode == 4 && mode <= 1) return &Cpu::thunk<&Cpu::opBcd<false>>;
    return arith<OP_AND>(op, EA_DATA);
}

H decodeLineE(uint16_t op) {
    int sz = (op >> 6) & 3;
    bool left = (op & 0x100) != 0;
    if (sz == 3) {
        if (op & 0x0800) return nullptr;
        if (!eaOk(op, EA_MALT)) return nullptr;
        int type = (op >> 9) & 3;
        return left ? shiftMemType<true>(type) : shiftMemType<false>(type);
    }
    int type = (op >> 3) & 3;
    return left ? shiftRegType<true>(type, sz) : shiftRegType<false>(type, sz);
}

} // namespace

Cpu::Handler Cpu::decode(uint16_t op) {
    H h = nullptr;
    switch (op >> 12) {
    case 0x0: h = decodeLine0(op); break;
    case 0x1:
    case 0x2:
    case 0x3: h = decodeMove(op); break;
    case 0x4: h = decodeLine4(op); break;
    case 0x5: h = decodeLine5(op); break;
    case 0x6: h = &Cpu::thunk<&Cpu::opBcc>; break;
    case 0x7: h = (op & 0x100) ? nullptr : &Cpu::thunk<&Cpu::opMoveq>; break;
    case 0x8: h = decodeLine8(op); break;
    case 0x9: h = decodeAddSub(op, true); break;
    case 0xa: return &Cpu::thunk<&Cpu::opLineA>;
    case 0xb: h = decodeLineB(op); break;
    case 0xc: h = decodeLineC(op); break;
    case 0xd: h = decodeAddSub(op, false); break;
    case 0xe: h = decodeLineE(op); break;
    case 0xf: return &Cpu::thunk<&Cpu::opLineF>;
    }
    return h ? h : &Cpu::thunk<&Cpu::opIllegal>;
}

void Cpu::buildTable() {
    for (int op = 0; op < 65536; op++) table_[op] = decode((uint16_t)op);
}

} // namespace m68k
