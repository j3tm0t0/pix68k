// オペコード → 命令実装の表 (68030 / 68060)。cpu020.cpp から include する
// 不当なアドレッシングモードの組み合わせは ILLEGAL / LINE-A / LINE-F の例外にする。

namespace m68k {

namespace {

enum : int {
    E_DN = 1 << 0,
    E_AN = 1 << 1,
    E_AI = 1 << 2,
    E_PI = 1 << 3,
    E_PD = 1 << 4,
    E_DI = 1 << 5,
    E_IX = 1 << 6,
    E_AW = 1 << 7,
    E_AL = 1 << 8,
    E_PCD = 1 << 9,
    E_PCX = 1 << 10,
    E_IM = 1 << 11,
    E_ALL = 0xfff,
    E_DATA = E_ALL & ~E_AN,
    E_MEM = E_ALL & ~(E_DN | E_AN),
    E_ALT = E_DN | E_AN | E_AI | E_PI | E_PD | E_DI | E_IX | E_AW | E_AL,
    E_DALT = E_ALT & ~E_AN,
    E_MALT = E_DALT & ~E_DN,
    E_CTRL = E_AI | E_DI | E_IX | E_AW | E_AL | E_PCD | E_PCX,
    E_CALT = E_AI | E_DI | E_IX | E_AW | E_AL,
};

int eBit(int mode, int reg) {
    if (mode < 7) return 1 << mode;
    if (reg <= 4) return 1 << (7 + reg);
    return 0;
}

bool eOk(uint16_t op, int set) { return (eBit((op >> 3) & 7, op & 7) & set) != 0; }

using H2 = Cpu020::Handler;
// 68060 の表を作っている間 true (Cpu020::decode())。68060 の表には、命令をやり直す例外のために命令の始めの値を
// 取っておく関数を入れる (cpu020.h の thunk と saveUndo() の説明)。
//   T2  A0-A7、CCR、S、M をすべて取る (U_FULL)。下の TN / TC にしていない命令はすべてこれ
//   TN  何も取らない (U_NONE)。次の 2 つを命令ごとに確かめたものだけ:
//       - CCR を変えたあとに、メモリの読み書き、命令の語の読み込み、奇数番地への分岐がない
//       - An を変えたあとにそれらがあるなら、変える前に logA() で記録している
//   TC  CCR だけ取る (U_CCR)。フラグを決めてからメモリに書く命令。An は TN と同じ
bool g060 = false;
// 表に入れる関数 (thunk) を選ぶ。K は 68060 の表に入れるもの。
// 68060 用に分けてコンパイルする命令の処理 (引数に Cpu020::For<K> がある。cpu020.h) は 2 つ目で、68060 の表には
// U_NONE でも 68060 用の処理を入れる。分けていない処理は 1 つ目で、U_NONE の thunk は 68030 と 68060 で同じものを使う
template <bool K, void (Cpu020::*M)(uint16_t), int U, unsigned SPEC = 0> H2 mk() {
    if constexpr (K && U != Cpu020::U_NONE) return &Cpu020::thunk<M, U, true, SPEC>;
    else return &Cpu020::thunk<M, U, false, SPEC>;
}
template <bool K, void (Cpu020::*M)(uint16_t, Cpu020::For<K>), int U, unsigned SPEC = 0> H2 mk() {
    return &Cpu020::thunkFor<K, M, U, SPEC>;
}
// 命令の処理の名前は、68060 用と両方の機種用のどちらにもなるので、2 回書く (PICK、TH、EAS)
#define PICK(U, SPEC, ...)                                                                                             \
    (g060 ? mk<true, &Cpu020::__VA_ARGS__, U, SPEC>() : mk<false, &Cpu020::__VA_ARGS__, U, SPEC>())
#define TH(U, ...) PICK(Cpu020::U, 0, __VA_ARGS__)
#define T2(...) TH(U_FULL, __VA_ARGS__)
#define TC(...) TH(U_CCR, __VA_ARGS__)
#define TN(...) TH(U_NONE, __VA_ARGS__)

// ---- アドレッシングモードなどを決めた thunk (cpu020.h の thunk の SPEC) ----
// 実効アドレスの種類: 0-6 はモード、7 以上は 7 + レジスタ (7 (xxx).W、8 (xxx).L、9 (d16,PC)、10 (d8,PC,Xn)、11 #imm)
int eaKind(uint16_t op) {
    const int mode = (op >> 3) & 7;
    return mode < 7 ? mode : 7 + (op & 7);
}
int dstKind(uint16_t op) { // MOVE の書く先 (bit 11-6)
    const int mode = (op >> 6) & 7;
    return mode < 7 ? mode : 7 + ((op >> 9) & 7);
}
// 実効アドレス (bit 5-0) の種類ごとの thunk。SET はその命令で使える種類 (E_xxx。使えない種類の thunk は作らない)。
// UR は Dn / An / PC 相対 / 即値のとき、UM は書けるメモリのときの U。K は 68060 の表に入れるもの (mk())
template <bool K, void (Cpu020::*M)(uint16_t, Cpu020::For<K>), int SET, int UR, int UM, unsigned EXTRA = 0>
H2 eaSpec(uint16_t op) {
#define EA_CASE(k, U)                                                                                                  \
    case k:                                                                                                            \
        if constexpr ((SET >> k) & 1) return mk<K, M, U, Cpu020::specEa(k) | EXTRA>();                                 \
        break;
    switch (eaKind(op)) {
        EA_CASE(0, UR)
        EA_CASE(1, UR)
        EA_CASE(2, UM)
        EA_CASE(3, UM)
        EA_CASE(4, UM)
        EA_CASE(5, UM)
        EA_CASE(6, UM)
        EA_CASE(7, UM)
        EA_CASE(8, UM)
        EA_CASE(9, UR)
        EA_CASE(10, UR)
        EA_CASE(11, UR)
    }
#undef EA_CASE
    return nullptr;
}
// EN: 実効アドレスの種類ごとの thunk で、何も取らない (TN と同じ条件)。
// EC: 同じく、実効アドレスがメモリなら CCR を取り (TC)、Dn / An なら何も取らない (TN)。
// どちらも、はじめの引数はその命令で使える種類 (E_xxx)
#define EAS(set, UR, UM, EXTRA, ...)                                                                                   \
    (g060 ? eaSpec<true, &Cpu020::__VA_ARGS__, set, Cpu020::UR, Cpu020::UM, EXTRA>(op)                                 \
          : eaSpec<false, &Cpu020::__VA_ARGS__, set, Cpu020::UR, Cpu020::UM, EXTRA>(op))
#define EN(set, ...) EAS(set, U_NONE, U_NONE, 0, __VA_ARGS__)
#define EC(set, ...) EAS(set, U_NONE, U_CCR, 0, __VA_ARGS__)

#define SIZED3(T, name, ...)                                                                                           \
    switch (sz) {                                                                                                      \
    case 0: return T(name<1 __VA_ARGS__>);                                                                             \
    case 1: return T(name<2 __VA_ARGS__>);                                                                             \
    case 2: return T(name<4 __VA_ARGS__>);                                                                             \
    }                                                                                                                  \
    return nullptr;
// 実効アドレスの種類ごとの thunk (EN / EC) で、大きさごと
#define SIZED3E(T, set, name, ...)                                                                                     \
    switch (sz) {                                                                                                      \
    case 0: return T(set, name<1 __VA_ARGS__>);                                                                        \
    case 1: return T(set, name<2 __VA_ARGS__>);                                                                        \
    case 2: return T(set, name<4 __VA_ARGS__>);                                                                        \
    }                                                                                                                  \
    return nullptr;

// <ea>,Dn: 読んでからフラグと Dn を決める
template <int OP> H2 arithEaDn2(int sz, uint16_t op) { SIZED3E(EN, E_ALL, opArithEaDn, , OP) }
// Dn,<ea>: メモリなら、読んで、フラグを決めて、書く
template <int OP> H2 arithDnEa2(int sz, uint16_t op) { SIZED3E(EC, E_DALT, opArithDnEa, , OP) }
// ADDX / SUBX: メモリの形は命令の中で CCR を取っておく (saveCcrUndo())
template <bool SUB> H2 addxSubx2(int sz) { SIZED3(TN, opAddxSubx, , SUB) }
template <int OP> H2 imm2(int sz, uint16_t op) {
    if (OP == IMM_CMPI) { SIZED3E(EN, E_DATA & ~E_IM, opImm, , OP) }
    SIZED3E(EC, E_DALT, opImm, , OP)
}
template <int OP> H2 unary2(int sz, uint16_t op) { SIZED3E(EC, E_DALT, opUnary, , OP) }
template <bool SUB> H2 addqSubq2(int sz, uint16_t op) { SIZED3E(EC, E_ALT, opAddqSubq, , SUB) }
H2 tst2(int sz, uint16_t op) { SIZED3E(EN, E_ALL, opTst) }
H2 cmpm2(int sz) { SIZED3(TN, opCmpm) }
template <int TYPE, bool LEFT> H2 shiftReg2(int sz) { SIZED3(TN, opShiftReg, , TYPE, LEFT) }

// MOVE: 読む側と書く先の種類ごとの thunk。読んで、フラグを決めて、書く。書く先が Dn なら、フラグを決めたあとに
// 例外はない (U_NONE)。メモリなら CCR を取る (U_CCR)
template <int S, int SK> H2 moveDst(uint16_t op) {
    constexpr unsigned src = Cpu020::specEa(SK);
    switch (dstKind(op)) {
    case 0: return PICK(Cpu020::U_NONE, src | Cpu020::specDst(0), opMove<S>);
    case 2: return PICK(Cpu020::U_CCR, src | Cpu020::specDst(2), opMove<S>);
    case 3: return PICK(Cpu020::U_CCR, src | Cpu020::specDst(3), opMove<S>);
    case 4: return PICK(Cpu020::U_CCR, src | Cpu020::specDst(4), opMove<S>);
    case 5: return PICK(Cpu020::U_CCR, src | Cpu020::specDst(5), opMove<S>);
    case 6: return PICK(Cpu020::U_CCR, src | Cpu020::specDst(6), opMove<S>);
    case 7: return PICK(Cpu020::U_CCR, src | Cpu020::specDst(7), opMove<S>);
    case 8: return PICK(Cpu020::U_CCR, src | Cpu020::specDst(8), opMove<S>);
    }
    return nullptr;
}
template <int S> H2 moveSpec(uint16_t op) {
    switch (eaKind(op)) {
    case 0: return moveDst<S, 0>(op);
    case 1:
        if constexpr (S != 1) return moveDst<S, 1>(op);
        break;
    case 2: return moveDst<S, 2>(op);
    case 3: return moveDst<S, 3>(op);
    case 4: return moveDst<S, 4>(op);
    case 5: return moveDst<S, 5>(op);
    case 6: return moveDst<S, 6>(op);
    case 7: return moveDst<S, 7>(op);
    case 8: return moveDst<S, 8>(op);
    case 9: return moveDst<S, 9>(op);
    case 10: return moveDst<S, 10>(op);
    case 11: return moveDst<S, 11>(op);
    }
    return nullptr;
}

// 条件 (bit 11-8) ごとの thunk
#define CC16(F)                                                                                                        \
    switch ((op >> 8) & 15) {                                                                                          \
    case 0: return F(0);                                                                                               \
    case 1: return F(1);                                                                                               \
    case 2: return F(2);                                                                                               \
    case 3: return F(3);                                                                                               \
    case 4: return F(4);                                                                                               \
    case 5: return F(5);                                                                                               \
    case 6: return F(6);                                                                                               \
    case 7: return F(7);                                                                                               \
    case 8: return F(8);                                                                                               \
    case 9: return F(9);                                                                                               \
    case 10: return F(10);                                                                                             \
    case 11: return F(11);                                                                                             \
    case 12: return F(12);                                                                                             \
    case 13: return F(13);                                                                                             \
    case 14: return F(14);                                                                                             \
    case 15: return F(15);                                                                                             \
    }                                                                                                                  \
    return nullptr;
// Bcc / BRA / BSR: 条件と変位の大きさ (下位バイトが $00 なら 16 ビット、$FF なら 32 ビット、ほかは 8 ビット) ごと
template <int CC> H2 bccLow(uint16_t op) {
    constexpr unsigned cc = Cpu020::specCc(CC);
    switch (op & 0xff) {
    case 0x00: return PICK(Cpu020::U_NONE, cc | Cpu020::specLow(1), opBcc);
    case 0xff: return PICK(Cpu020::U_NONE, cc | Cpu020::specLow(2), opBcc);
    }
    return PICK(Cpu020::U_NONE, cc | Cpu020::specLow(3), opBcc);
}
H2 bccSpec(uint16_t op) {
#define F(n) bccLow<n>(op)
    CC16(F)
#undef F
}
H2 dbccSpec(uint16_t op) {
#define F(n) PICK(Cpu020::U_NONE, Cpu020::specCc(n), opDbcc)
    CC16(F)
#undef F
}
H2 sccSpec(uint16_t op) {
#define F(n) EAS(E_DALT, U_NONE, U_NONE, Cpu020::specCc(n), opScc)
    CC16(F)
#undef F
}

template <bool LEFT> H2 shiftRegType2(int type, int sz) {
    switch (type) {
    case SH_AS: return shiftReg2<SH_AS, LEFT>(sz);
    case SH_LS: return shiftReg2<SH_LS, LEFT>(sz);
    case SH_ROX: return shiftReg2<SH_ROX, LEFT>(sz);
    case SH_RO: return shiftReg2<SH_RO, LEFT>(sz);
    }
    return nullptr;
}
template <bool LEFT> H2 shiftMemType2(int type) {
    switch (type) {
    case SH_AS: return TC(opShiftMem<SH_AS, LEFT>);
    case SH_LS: return TC(opShiftMem<SH_LS, LEFT>);
    case SH_ROX: return TC(opShiftMem<SH_ROX, LEFT>);
    case SH_RO: return TC(opShiftMem<SH_RO, LEFT>);
    }
    return nullptr;
}
// BTST は読むだけ。BCHG / BCLR / BSET は、メモリなら Z を決めてから書く
H2 bitDyn2(int t, uint16_t op) {
    switch (t) {
    case 0: return EN(E_DATA, opBitDyn<BIT_TST>);
    case 1: return EC(E_DALT, opBitDyn<BIT_CHG>);
    case 2: return EC(E_DALT, opBitDyn<BIT_CLR>);
    case 3: return EC(E_DALT, opBitDyn<BIT_SET>);
    }
    return nullptr;
}
H2 bitImm2(int t, uint16_t op) {
    switch (t) {
    case 0: return EN(E_DATA, opBitImm<BIT_TST>);
    case 1: return EC(E_DALT, opBitImm<BIT_CHG>);
    case 2: return EC(E_DALT, opBitImm<BIT_CLR>);
    case 3: return EC(E_DALT, opBitImm<BIT_SET>);
    }
    return nullptr;
}

template <int OP> H2 arith2(uint16_t op, int eaDnSet) {
    int opmode = (op >> 6) & 7;
    int sz = opmode & 3;
    if (opmode < 3) {
        int set = eaDnSet;
        if (sz == 0) set &= ~E_AN;
        return eOk(op, set) ? arithEaDn2<OP>(sz, op) : nullptr;
    }
    if (opmode >= 4 && opmode <= 6) return eOk(op, E_MALT) ? arithDnEa2<OP>(sz, op) : nullptr;
    return nullptr;
}

H2 decodeLine0_2(uint16_t op, CpuModel model) {
    // 68060 がハードウェアで実行しない命令 (MOVEP、CHK2 / CMP2、CAS2) は未実装整数命令 (MC68060UM 8.2.4)
    const bool m060 = model == CpuModel::M68060;
    int sz = (op >> 6) & 3;
    if ((op & 0x0138) == 0x0108) {
        if (m060) return T2(opUnimpInt);
        return (op & 0x40) ? T2(opMovep<4>) : T2(opMovep<2>);
    }
    if (op & 0x0100) {
        int t = (op >> 6) & 3;
        return eOk(op, t == 0 ? E_DATA : E_DALT) ? bitDyn2(t, op) : nullptr;
    }
    if ((op & 0x0f00) == 0x0800) {
        int t = (op >> 6) & 3;
        return eOk(op, t == 0 ? (E_DATA & ~E_IM) : E_DALT) ? bitImm2(t, op) : nullptr;
    }
    // CMP2 / CHK2
    if ((op & 0xf9c0) == 0x00c0) {
        if (!eOk(op, E_CTRL)) return nullptr;
        if (m060 && ((op >> 9) & 3) != 3) return T2(opUnimpInt);
        switch ((op >> 9) & 3) {
        case 0: return T2(opChk2Cmp2<1>);
        case 1: return T2(opChk2Cmp2<2>);
        case 2: return T2(opChk2Cmp2<4>);
        }
        return nullptr;
    }
    // CAS2
    if (op == 0x0cfc) return m060 ? T2(opUnimpInt) : T2(opCas2<2>);
    if (op == 0x0efc) return m060 ? T2(opUnimpInt) : T2(opCas2<4>);
    // CAS
    if ((op & 0xf9c0) == 0x08c0 && (op & 0x0600)) {
        if (!eOk(op, E_MALT)) return nullptr;
        switch ((op >> 9) & 3) {
        case 1: return T2(opCas<1>);
        case 2: return T2(opCas<2>);
        case 3: return T2(opCas<4>);
        }
        return nullptr;
    }
    // CALLM / RTM (68020 だけ)
    if ((op & 0xffc0) == 0x06c0) return T2(opCallm);
    // MOVES
    if ((op & 0xff00) == 0x0e00 && sz != 3) {
        if (!eOk(op, E_MALT)) return nullptr;
        switch (sz) {
        case 0: return T2(opMoves<1>);
        case 1: return T2(opMoves<2>);
        case 2: return T2(opMoves<4>);
        }
    }
    int k = (op >> 9) & 7;
    if ((op & 0xff) == 0x3c || (op & 0xff) == 0x7c) {
        switch (k) {
        case 0: return T2(opImmSr<IMM_ORI>);
        case 1: return T2(opImmSr<IMM_ANDI>);
        case 5: return T2(opImmSr<IMM_EORI>);
        }
        return nullptr;
    }
    if (sz == 3) return nullptr;
    if (k == 6) return eOk(op, E_DATA & ~E_IM) ? imm2<IMM_CMPI>(sz, op) : nullptr; // 68020 以降は PC 相対も可
    if (!eOk(op, E_DALT)) return nullptr;
    switch (k) {
    case 0: return imm2<IMM_ORI>(sz, op);
    case 1: return imm2<IMM_ANDI>(sz, op);
    case 2: return imm2<IMM_SUBI>(sz, op);
    case 3: return imm2<IMM_ADDI>(sz, op);
    case 5: return imm2<IMM_EORI>(sz, op);
    }
    return nullptr;
}

H2 decodeMove2(uint16_t op) {
    int line = op >> 12;
    int S = line == 1 ? 1 : line == 3 ? 2 : 4;
    int dm = (op >> 6) & 7, dr = (op >> 9) & 7;
    int srcSet = E_ALL;
    if (S == 1) srcSet &= ~E_AN;
    if (!eOk(op, srcSet)) return nullptr;
    if (dm == 1) {
        if (S == 1) return nullptr;
        return S == 2 ? EN(E_ALL, opMovea<2>) : EN(E_ALL, opMovea<4>); // 読んでから An を決める
    }
    if (!(eBit(dm, dr) & E_DALT)) return nullptr;
    switch (S) {
    case 1: return moveSpec<1>(op);
    case 2: return moveSpec<2>(op);
    default: return moveSpec<4>(op);
    }
}

H2 decodeLine4_2(uint16_t op) {
    int sz = (op >> 6) & 3;
    int mode = (op >> 3) & 7;
    switch (op) {
    case 0x4afc: return T2(opIllegal);
    case 0x4e70: return T2(opReset);
    case 0x4e71: return TN(opNop);
    case 0x4e72: return T2(opStop);
    case 0x4e73: return T2(opRte);
    case 0x4e74: return T2(opRtd);
    case 0x4e75: return TN(opRts); // 戻り先が奇数なら、A7 を進める前に例外にする (opRts())
    case 0x4e76: return T2(opTrapv);
    case 0x4e77: return T2(opRtr);
    case 0x4e7a:
    case 0x4e7b: return T2(opMovec);
    }
    if ((op & 0xfff0) == 0x4e40) return T2(opTrap);
    if ((op & 0xfff8) == 0x4e50) return TN(opLink<2>);
    if ((op & 0xfff8) == 0x4808) return TN(opLink<4>);
    if ((op & 0xfff8) == 0x4848) return T2(opBkpt);
    if ((op & 0xfff8) == 0x4e58) return TN(opUnlk);
    if ((op & 0xfff0) == 0x4e60) return T2(opMoveUsp);
    if ((op & 0xffc0) == 0x4e80) return eOk(op, E_CTRL) ? EN(E_CTRL, opJsr) : nullptr;
    if ((op & 0xffc0) == 0x4ec0) return eOk(op, E_CTRL) ? EN(E_CTRL, opJmp) : nullptr;
    if ((op & 0xfff8) == 0x49c0) return TN(opExt); // EXTB.L
    if ((op & 0xf1c0) == 0x41c0) return eOk(op, E_CTRL) ? EN(E_CTRL, opLea) : nullptr;
    if ((op & 0xf1c0) == 0x4180) return eOk(op, E_DATA) ? T2(opChk<2>) : nullptr;
    if ((op & 0xf1c0) == 0x4100) return eOk(op, E_DATA) ? T2(opChk<4>) : nullptr;
    if ((op & 0xffc0) == 0x40c0) return eOk(op, E_DALT) ? T2(opMoveFromSr) : nullptr;
    if ((op & 0xffc0) == 0x42c0) return eOk(op, E_DALT) ? T2(opMoveFromCcr) : nullptr;
    if ((op & 0xffc0) == 0x44c0) return eOk(op, E_DATA) ? T2(opMoveToCcr) : nullptr;
    if ((op & 0xffc0) == 0x46c0) return eOk(op, E_DATA) ? T2(opMoveToSr) : nullptr;
    if ((op & 0xff00) == 0x4000 && sz != 3) return eOk(op, E_DALT) ? unary2<UN_NEGX>(sz, op) : nullptr;
    if ((op & 0xff00) == 0x4200 && sz != 3) return eOk(op, E_DALT) ? unary2<UN_CLR>(sz, op) : nullptr;
    if ((op & 0xff00) == 0x4400 && sz != 3) return eOk(op, E_DALT) ? unary2<UN_NEG>(sz, op) : nullptr;
    if ((op & 0xff00) == 0x4600 && sz != 3) return eOk(op, E_DALT) ? unary2<UN_NOT>(sz, op) : nullptr;
    if ((op & 0xffc0) == 0x4800) return eOk(op, E_DALT) ? T2(opNbcd) : nullptr;
    if ((op & 0xfff8) == 0x4840) return TN(opSwap);
    if ((op & 0xffc0) == 0x4840) return eOk(op, E_CTRL) ? EN(E_CTRL, opPea) : nullptr;
    if ((op & 0xfff8) == 0x4880) return TN(opExt);
    if ((op & 0xfff8) == 0x48c0) return TN(opExt);
    if ((op & 0xff80) == 0x4880 && mode != 0) {
        if (!eOk(op, E_AI | E_PD | E_DI | E_IX | E_AW | E_AL)) return nullptr;
        return (op & 0x40) ? EN(E_CALT | E_PD, opMovemToMem<4>) : EN(E_CALT | E_PD, opMovemToMem<2>);
    }
    if ((op & 0xff80) == 0x4c80) {
        if (!eOk(op, E_AI | E_PI | E_DI | E_IX | E_AW | E_AL | E_PCD | E_PCX)) return nullptr;
        return (op & 0x40) ? EN(E_CTRL | E_PI, opMovemToReg<4>) : EN(E_CTRL | E_PI, opMovemToReg<2>); // An は 1 つずつ記録する
    }
    if ((op & 0xffc0) == 0x4c00) return eOk(op, E_DATA) ? T2(opMulL) : nullptr;
    if ((op & 0xffc0) == 0x4c40) return eOk(op, E_DATA) ? T2(opDivL) : nullptr;
    if ((op & 0xffc0) == 0x4ac0) return eOk(op, E_DALT) ? T2(opTas) : nullptr;
    if ((op & 0xff00) == 0x4a00 && sz != 3) {
        int set = sz == 0 ? (E_ALL & ~E_AN) : E_ALL;
        return eOk(op, set) ? tst2(sz, op) : nullptr;
    }
    return nullptr;
}

H2 decodeLine5_2(uint16_t op) {
    int sz = (op >> 6) & 3;
    int mode = (op >> 3) & 7;
    if (sz == 3) {
        if (mode == 1) return dbccSpec(op);
        if (mode == 7 && (op & 7) >= 2 && (op & 7) <= 4) return T2(opTrapcc);
        return eOk(op, E_DALT) ? sccSpec(op) : nullptr;
    }
    int set = E_ALT;
    if (sz == 0) set &= ~E_AN;
    if (!eOk(op, set)) return nullptr;
    return (op & 0x100) ? addqSubq2<true>(sz, op) : addqSubq2<false>(sz, op);
}

H2 decodeLine8_2(uint16_t op) {
    int opmode = (op >> 6) & 7;
    int mode = (op >> 3) & 7;
    if (opmode == 3) return eOk(op, E_DATA) ? TN(opDivW<false>) : nullptr;
    if (opmode == 7) return eOk(op, E_DATA) ? TN(opDivW<true>) : nullptr;
    if (opmode == 4 && mode <= 1) return T2(opBcd<true>);
    if (opmode == 5 && mode <= 1) return T2(opPack);
    if (opmode == 6 && mode <= 1) return T2(opUnpk);
    return arith2<OP_OR>(op, E_DATA);
}

H2 decodeAddSub2(uint16_t op, bool sub) {
    int opmode = (op >> 6) & 7;
    int mode = (op >> 3) & 7;
    if (opmode == 3) {
        if (!eOk(op, E_ALL)) return nullptr;
        return sub ? EN(E_ALL, opAdda<2, true>) : EN(E_ALL, opAdda<2, false>);
    }
    if (opmode == 7) {
        if (!eOk(op, E_ALL)) return nullptr;
        return sub ? EN(E_ALL, opAdda<4, true>) : EN(E_ALL, opAdda<4, false>);
    }
    if (opmode >= 4 && mode <= 1) return sub ? addxSubx2<true>(opmode & 3) : addxSubx2<false>(opmode & 3);
    return sub ? arith2<OP_SUB>(op, E_ALL) : arith2<OP_ADD>(op, E_ALL);
}

H2 decodeLineB_2(uint16_t op) {
    int opmode = (op >> 6) & 7;
    int mode = (op >> 3) & 7;
    if (opmode == 3) return eOk(op, E_ALL) ? EN(E_ALL, opCmpa<2>) : nullptr;
    if (opmode == 7) return eOk(op, E_ALL) ? EN(E_ALL, opCmpa<4>) : nullptr;
    if (opmode < 3) {
        int set = E_ALL;
        if (opmode == 0) set &= ~E_AN;
        return eOk(op, set) ? arithEaDn2<OP_CMP>(opmode, op) : nullptr;
    }
    if (mode == 1) return cmpm2(opmode & 3);
    return eOk(op, E_DALT) ? arithDnEa2<OP_EOR>(opmode & 3, op) : nullptr;
}

H2 decodeLineC_2(uint16_t op) {
    int opmode = (op >> 6) & 7;
    int mode = (op >> 3) & 7;
    if (opmode == 3) return eOk(op, E_DATA) ? EN(E_DATA, opMulW<false>) : nullptr;
    if (opmode == 7) return eOk(op, E_DATA) ? EN(E_DATA, opMulW<true>) : nullptr;
    if ((op & 0xf1f8) == 0xc140 || (op & 0xf1f8) == 0xc148 || (op & 0xf1f8) == 0xc188) return TN(opExg);
    if (opmode == 4 && mode <= 1) return T2(opBcd<false>);
    return arith2<OP_AND>(op, E_DATA);
}

H2 decodeLineE_2(uint16_t op) {
    int sz = (op >> 6) & 3;
    bool left = (op & 0x100) != 0;
    if (sz == 3) {
        if (op & 0x0800) {
            // ビットフィールド
            int type = (op >> 8) & 7;
            bool modify = type == 2 || type == 4 || type == 6 || type == 7;
            return eOk(op, E_DN | (modify ? E_CALT : E_CTRL)) ? T2(opBitField) : nullptr;
        }
        if (!eOk(op, E_MALT)) return nullptr;
        int type = (op >> 9) & 3;
        return left ? shiftMemType2<true>(type) : shiftMemType2<false>(type);
    }
    int type = (op >> 3) & 3;
    return left ? shiftRegType2<true>(type, sz) : shiftRegType2<false>(type, sz);
}

H2 decodeLineF_2(uint16_t op, CpuModel model) {
    int cpid = (op >> 9) & 7;
    if (cpid == 0 && model == CpuModel::M68030) return T2(opMmu030);
    if (model == CpuModel::M68060) {
        if ((op & 0xff00) == 0xf400) return T2(opCache060);
        if ((op & 0xff00) == 0xf500) return T2(opMmu060);
        if ((op & 0xffc0) == 0xf600) return T2(opMove16);
        if (op == 0xf800) return T2(opLpstop);
    }
    if (cpid == 1) return T2(opFpu);
    return T2(opLineF);
}

H2 decodeOp(uint16_t op, CpuModel model) {
    g060 = model == CpuModel::M68060;
    H2 h = nullptr;
    switch (op >> 12) {
    case 0x0: h = decodeLine0_2(op, model); break;
    case 0x1:
    case 0x2:
    case 0x3: h = decodeMove2(op); break;
    case 0x4: h = decodeLine4_2(op); break;
    case 0x5: h = decodeLine5_2(op); break;
    case 0x6: h = bccSpec(op); break;
    case 0x7: h = (op & 0x100) ? nullptr : TN(opMoveq); break;
    case 0x8: h = decodeLine8_2(op); break;
    case 0x9: h = decodeAddSub2(op, true); break;
    case 0xa: return T2(opLineA);
    case 0xb: h = decodeLineB_2(op); break;
    case 0xc: h = decodeLineC_2(op); break;
    case 0xd: h = decodeAddSub2(op, false); break;
    case 0xe: h = decodeLineE_2(op); break;
    case 0xf: return decodeLineF_2(op, model);
    }
    return h ? h : T2(opIllegal);
}

#undef SIZED3
#undef SIZED3E
#undef T2
#undef TC
#undef TN
#undef TH
#undef PICK
#undef EAS
#undef EN
#undef EC
#undef CC16

} // namespace

Cpu020::Handler Cpu020::decode(uint16_t op, CpuModel model) { return decodeOp(op, model); }

void Cpu020::buildTables() {
    for (int op = 0; op < 65536; op++) {
        table030_[op] = decode((uint16_t)op, CpuModel::M68030);
        table060_[op] = decode((uint16_t)op, CpuModel::M68060);
    }
}

} // namespace m68k
