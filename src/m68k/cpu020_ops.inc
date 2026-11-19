// MC68030 / MC68060 の整数命令 (cpu020.cpp から include する)
//
// 命令の開始時、pc_ はオペコードの次の語を指す (オペコードは読み終えている)。
// 未定義のフラグは実機の結果 (instructiontest.x) に合わせる。サイクル数は 68000 での概算 (tick) に機種の係数を掛けたもの。
//
// よく使う命令の処理は template の最後に bool M060 を持ち、引数に For<M060> を取る (cpu020.h の For の説明)。
// 68060 用 (M060 = true) と、両方の機種で使えるもの (false) の 2 つにコンパイルされる。その中では tick(n) を
// if (!M060) tick(n)、is060() を on060<M060>() と書き、fetch16 / fetch32 / eaAddr / readEa / readEaAddr に M060 を渡す (eaAddr から呼ぶ indexAddr / extAddr も同じ)。
// 特権命令や使うことの少ない命令 (MOVEC、RTE、ビットフィールド、BCD など) は分けておらず、今までどおり 1 つだけ

namespace m68k {

#define MODE ((op >> 3) & 7)
#define REG (op & 7)
#define REG9 ((op >> 9) & 7)

// ---------------------------------------------------------------- 算術・論理

template <int S, int OP, bool M060> void Cpu020::opArithEaDn(uint16_t op, For<M060>) {
    int dn = REG9;
    uint32_t s = readEa<S, M060>(MODE, REG);
    uint32_t d = d_[dn] & Mask<S>::v;
    uint32_t r = 0;
    switch (OP) {
    case OP_ADD: r = add<S>(s, d, 0, false); break;
    case OP_SUB: r = sub<S>(s, d, 0, false); break;
    case OP_AND: r = logic<S>(d & s); break;
    case OP_OR: r = logic<S>(d | s); break;
    case OP_CMP: cmp<S>(s, d); break;
    }
    if (OP != OP_CMP) setD<S>(dn, r);
}

template <int S, int OP, bool M060> void Cpu020::opArithDnEa(uint16_t op, For<M060>) {
    int mode = MODE, reg = REG;
    uint32_t s = d_[REG9] & Mask<S>::v;
    if (mode == 0) { // EOR Dn,Dn
        setD<S>(reg, logic<S>(d_[reg] ^ s));
        return;
    }
    uint32_t addr = eaAddr<M060>(mode, reg, S);
    uint32_t d = readS<S>(addr);
    uint32_t r = 0;
    switch (OP) {
    case OP_ADD: r = add<S>(s, d, 0, false); break;
    case OP_SUB: r = sub<S>(s, d, 0, false); break;
    case OP_AND: r = logic<S>(d & s); break;
    case OP_OR: r = logic<S>(d | s); break;
    case OP_EOR: r = logic<S>(d ^ s); break;
    }
    writeS<S>(addr, r);
}

template <int S, bool SUB, bool M060> void Cpu020::opAddxSubx(uint16_t op, For<M060>) {
    int rx = REG9, ry = REG;
    if (!(op & 8)) {
        uint32_t r = SUB ? sub<S>(d_[ry], d_[rx], flagX_, true) : add<S>(d_[ry], d_[rx], flagX_, true);
        setD<S>(rx, r);
        return;
    }
    saveCcrUndo(); // 書き込みのバスエラーでやり直すとき、X と Z を戻す (cpu020.h の logA())
    uint32_t sa = eaAddr<M060>(4, ry, S);
    uint32_t s = readS<S>(sa);
    uint32_t da = eaAddr<M060>(4, rx, S);
    uint32_t d = readS<S>(da);
    uint32_t r = SUB ? sub<S>(s, d, flagX_, true) : add<S>(s, d, flagX_, true);
    writeS<S>(da, r);
}

template <int S, bool SUB, bool M060> void Cpu020::opAdda(uint16_t op, For<M060>) {
    int an = REG9;
    uint32_t s = readEa<S, M060>(MODE, REG);
    if (S == 2) s = sext16(s);
    a_[an] = SUB ? a_[an] - s : a_[an] + s;
}

template <int S, bool M060> void Cpu020::opCmpa(uint16_t op, For<M060>) {
    uint32_t s = readEa<S, M060>(MODE, REG);
    if (S == 2) s = sext16(s);
    cmp<4>(s, a_[REG9]);
}

template <int S, bool M060> void Cpu020::opCmpm(uint16_t op, For<M060>) {
    uint32_t s = readS<S>(eaAddr<M060>(3, REG, S));
    uint32_t d = readS<S>(eaAddr<M060>(3, REG9, S));
    cmp<S>(s, d);
}

template <int S, int OP, bool M060> void Cpu020::opImm(uint16_t op, For<M060>) {
    int mode = MODE, reg = REG;
    uint32_t imm = S == 1 ? (fetch16<M060>() & 0xff) : S == 2 ? fetch16<M060>() : fetch32<M060>();
    uint32_t d, addr = 0;
    if (mode == 0) {
        d = d_[reg] & Mask<S>::v;
    } else {
        addr = eaAddr<M060>(mode, reg, S);
        d = readS<S>(addr);
    }
    uint32_t r = 0;
    switch (OP) {
    case IMM_ORI: r = logic<S>(d | imm); break;
    case IMM_ANDI: r = logic<S>(d & imm); break;
    case IMM_SUBI: r = sub<S>(imm, d, 0, false); break;
    case IMM_ADDI: r = add<S>(imm, d, 0, false); break;
    case IMM_EORI: r = logic<S>(d ^ imm); break;
    case IMM_CMPI: cmp<S>(imm, d); return;
    }
    if (mode == 0) setD<S>(reg, r);
    else writeS<S>(addr, r);
}

template <int OP> void Cpu020::opImmSr(uint16_t op) {
    bool toSr = (op & 0x40) != 0;
    if (toSr && !sflag_) {
        privilege();
        return;
    }
    uint16_t imm = fetch16();
    uint16_t v = getSR();
    switch (OP) {
    case IMM_ORI: v |= imm; break;
    case IMM_ANDI: v &= imm; break;
    case IMM_EORI: v ^= imm; break;
    }
    tick(8);
    if (toSr) setSR(v);
    else setCCR((uint8_t)v);
}

template <int S, bool SUB, bool M060> void Cpu020::opAddqSubq(uint16_t op, For<M060>) {
    int mode = MODE, reg = REG;
    uint32_t q = REG9;
    if (q == 0) q = 8;
    if (mode == 1) {
        a_[reg] = SUB ? a_[reg] - q : a_[reg] + q;
        return;
    }
    if (mode == 0) {
        setD<S>(reg, SUB ? sub<S>(q, d_[reg], 0, false) : add<S>(q, d_[reg], 0, false));
        return;
    }
    uint32_t addr = eaAddr<M060>(mode, reg, S);
    uint32_t d = readS<S>(addr);
    writeS<S>(addr, SUB ? sub<S>(q, d, 0, false) : add<S>(q, d, 0, false));
}

// ---------------------------------------------------------------- 転送

template <int S, bool M060> void Cpu020::opMove(uint16_t op, For<M060>) {
    uint32_t v = readEa<S, M060>(MODE, REG);
    int dmode = (op >> 6) & 7, dreg = REG9;
    setNZ<S>(v);
    flagV_ = flagC_ = 0;
    if (dmode == 0) {
        setD<S>(dreg, v);
        return;
    }
    writeS<S>(eaAddr<M060>(dmode, dreg, S), v);
}

template <int S, bool M060> void Cpu020::opMovea(uint16_t op, For<M060>) {
    uint32_t v = readEa<S, M060>(MODE, REG);
    a_[REG9] = S == 2 ? sext16(v) : v;
}

void Cpu020::opMoveq(uint16_t op) {
    uint32_t v = sext8(op);
    d_[REG9] = v;
    setNZ<4>(v);
    flagV_ = flagC_ = 0;
}

template <int S, int OP, bool M060> void Cpu020::opUnary(uint16_t op, For<M060>) {
    int mode = MODE, reg = REG;
    uint32_t d, addr = 0;
    if (mode == 0) {
        d = d_[reg];
    } else {
        if (OP == UN_NEGX) saveCcrUndo();
        addr = eaAddr<M060>(mode, reg, S);
        d = OP == UN_CLR ? 0 : readS<S>(addr);
    }
    uint32_t r = 0;
    switch (OP) {
    case UN_NEGX: r = sub<S>(d, 0, flagX_, true); break;
    case UN_CLR:
        flagN_ = flagV_ = flagC_ = 0;
        flagZ_ = 1;
        break;
    case UN_NEG: r = sub<S>(d, 0, 0, false); break;
    case UN_NOT: r = logic<S>(~d); break;
    }
    if (mode == 0) setD<S>(reg, r);
    else writeS<S>(addr, r);
}

template <int S, bool M060> void Cpu020::opTst(uint16_t op, For<M060>) { logic<S>(readEa<S, M060>(MODE, REG)); }

template <bool M060> void Cpu020::opTas(uint16_t op, For<M060>) {
    int mode = MODE, reg = REG;
    if (mode == 0) {
        logic<1>(d_[reg]);
        d_[reg] |= 0x80;
        return;
    }
    uint32_t addr = eaAddr<M060>(mode, reg, 1);
    uint8_t v = read8(addr);
    logic<1>(v);
    write8(addr, v | 0x80);
}

template <bool M060> void Cpu020::opScc(uint16_t op, For<M060>) {
    int mode = MODE, reg = REG;
    uint32_t v = cond((op >> 8) & 15) ? 0xff : 0;
    if (mode == 0) {
        setD<1>(reg, v);
        return;
    }
    write8(eaAddr<M060>(mode, reg, 1), v);
}

void Cpu020::opSwap(uint16_t op) {
    uint32_t v = d_[REG];
    v = (v >> 16) | (v << 16);
    d_[REG] = v;
    logic<4>(v);
}

// EXT.W (0x4880)、EXT.L (0x48C0)、EXTB.L (0x49C0)
void Cpu020::opExt(uint16_t op) {
    int reg = REG;
    switch ((op >> 6) & 7) {
    case 2: {
        uint32_t v = sext8(d_[reg]) & 0xffff;
        setD<2>(reg, v);
        logic<2>(v);
        break;
    }
    case 3: d_[reg] = logic<4>(sext16(d_[reg])); break;
    default: d_[reg] = logic<4>(sext8(d_[reg])); break;
    }
}

template <bool M060> void Cpu020::opExg(uint16_t op, For<M060>) {
    int rx = REG9, ry = REG;
    uint32_t t;
    switch ((op >> 3) & 0x1f) {
    case 0x08:
        t = d_[rx];
        d_[rx] = d_[ry];
        d_[ry] = t;
        break;
    case 0x09:
        t = a_[rx];
        a_[rx] = a_[ry];
        a_[ry] = t;
        break;
    case 0x11:
        t = d_[rx];
        d_[rx] = a_[ry];
        a_[ry] = t;
        break;
    }
    if (!M060) tick(2);
}

void Cpu020::opMoveFromSr(uint16_t op) {
    if (!sflag_) { // 68010 以降は特権命令
        privilege();
        return;
    }
    int mode = MODE, reg = REG;
    uint16_t v = getSR();
    if (mode == 0) setD<2>(reg, v);
    else write16(eaAddr(mode, reg, 2), v);
}

void Cpu020::opMoveFromCcr(uint16_t op) {
    int mode = MODE, reg = REG;
    uint16_t v = getSR() & 0xff;
    if (mode == 0) setD<2>(reg, v);
    else write16(eaAddr(mode, reg, 2), v);
}

void Cpu020::opMoveToCcr(uint16_t op) {
    setCCR((uint8_t)readEa<2>(MODE, REG));
    tick(4);
}

void Cpu020::opMoveToSr(uint16_t op) {
    if (!sflag_) {
        privilege();
        return;
    }
    uint16_t v = (uint16_t)readEa<2>(MODE, REG);
    tick(4);
    setSR(v);
}

void Cpu020::opMoveUsp(uint16_t op) {
    if (!sflag_) {
        privilege();
        return;
    }
    if (op & 8) a_[REG] = usp_;
    else usp_ = a_[REG];
}

template <int S, bool M060> void Cpu020::opMovemToMem(uint16_t op, For<M060>) {
    int mode = MODE, reg = REG;
    uint16_t mask = fetch16<M060>();
    tick060(__builtin_popcount(mask)); // MC68060UM 表 10-20: レジスタ 1 つに 1 サイクル
    if (mode == 4) {
        // <list> に An があれば、命令の開始時の An - 大きさを書く (68020 以降)
        uint32_t addr = a_[reg];
        logA(reg);
        if (mask) a_[reg] = addr - S;
        // マスクの立っているビットだけを、下のビットから順にたどる (16 回調べるのと同じ順)
        for (uint32_t m = mask; m; m &= m - 1) {
            int r = 15 - __builtin_ctz(m);
            uint32_t v = r < 8 ? d_[r] : a_[r - 8];
            addr -= S;
            writeS<S>(addr, v);
        }
        a_[reg] = addr;
        return;
    }
    uint32_t addr = eaAddr<M060>(mode, reg, S);
    for (uint32_t m = mask; m; m &= m - 1) {
        int r = __builtin_ctz(m);
        writeS<S>(addr, r < 8 ? d_[r] : a_[r - 8]);
        addr += S;
    }
}

template <int S, bool M060> void Cpu020::opMovemToReg(uint16_t op, For<M060>) {
    int mode = MODE, reg = REG;
    uint16_t mask = fetch16<M060>();
    tick060(__builtin_popcount(mask)); // MC68060UM 表 10-20: レジスタ 1 つに 1 サイクル
    uint32_t addr = mode == 3 ? a_[reg] : eaAddr<M060>(mode, reg, S);
    for (uint32_t m = mask; m; m &= m - 1) {
        int r = __builtin_ctz(m);
        uint32_t v = readS<S>(addr);
        if (S == 2) v = sext16(v);
        if (r < 8) {
            d_[r] = v;
        } else {
            logA(r - 8);
            a_[r - 8] = v;
        }
        addr += S;
    }
    if (mode == 3) a_[reg] = addr;
}

template <int S> void Cpu020::opMovep(uint16_t op) {
    int dx = REG9, ay = REG;
    uint32_t addr = a_[ay] + sext16(fetch16());
    if (!(op & 0x80)) {
        uint32_t v = 0;
        for (int i = 0; i < S; i++) v = (v << 8) | read8(addr + 2 * i);
        setD<S>(dx, v);
    } else {
        uint32_t v = d_[dx];
        for (int i = 0; i < S; i++) write8(addr + 2 * i, v >> (8 * (S - 1 - i)));
    }
}

// ---------------------------------------------------------------- プログラム制御

template <bool M060> void Cpu020::opBcc(uint16_t op, For<M060>) {
    int cc = (op >> 8) & 15;
    uint32_t base = pc_;
    uint32_t disp;
    switch (op & 0xff) {
    case 0: disp = sext16(fetch16<M060>()); break;
    case 0xff: disp = fetch32<M060>(); break;
    default: disp = sext8(op); break;
    }
    uint32_t target = base + disp;
    if (cc == 1) { // BSR
        logA(7);
        a_[7] -= 4;
        write32(a_[7], pc_);
        if (!M060) tick(2);
        if (on060<M060>()) jump060(1);
        jumpTo(target);
        return;
    }
    if (cond(cc)) {
        if (!M060) tick(2);
        if (on060<M060>()) {
            if (cc == 0) jump060(0); // BRA
            else bcc060(true, target < base);
        }
        jumpTo(target);
        return;
    }
    if (!M060) tick(4);
    if (on060<M060>()) bcc060(false, target < base);
}

template <bool M060> void Cpu020::opDbcc(uint16_t op, For<M060>) {
    int reg = REG;
    uint32_t base = pc_;
    uint32_t disp = sext16(fetch16<M060>());
    const bool dbra = ((op >> 8) & 15) == 1;
    if (cond((op >> 8) & 15)) {
        if (!M060) tick(4);
        if (on060<M060>()) dbcc060(false, dbra);
        return;
    }
    uint16_t cnt = (uint16_t)(d_[reg] - 1);
    setD<2>(reg, cnt);
    if (!M060) tick(2);
    if (on060<M060>()) dbcc060(cnt != 0xffff, dbra);
    if (cnt != 0xffff) jumpTo(base + disp);
}

template <bool M060> void Cpu020::opLea(uint16_t op, For<M060>) { a_[REG9] = eaAddr<M060>(MODE, REG, 4); }

template <bool M060> void Cpu020::opPea(uint16_t op, For<M060>) {
    uint32_t addr = eaAddr<M060>(MODE, REG, 4);
    logA(7);
    a_[7] -= 4;
    write32(a_[7], addr);
}

// 68060 の JMP / JSR (MC68060UM 表 10-18): (d16,PC) と (xxx).WL は分岐キャッシュで予測でき、ほかは 5 サイクル。
// base は予測が当たったときのサイクル数 (JMP 0、JSR 1)
void Cpu020::jmp060(uint16_t op, int base) {
    const int mode = MODE, reg = REG;
    if (mode == 7 && reg <= 2) {
        jump060(base);
    } else {
        tick060(5);
        pairLead_ = 0;
        foldOk060_ = false;
    }
}

template <bool M060> void Cpu020::opJmp(uint16_t op, For<M060>) {
    uint32_t t = eaAddr<M060>(MODE, REG, 4);
    if (on060<M060>()) jmp060(op, 0);
    jumpTo(t);
}

template <bool M060> void Cpu020::opJsr(uint16_t op, For<M060>) {
    uint32_t t = eaAddr<M060>(MODE, REG, 4);
    logA(7);
    a_[7] -= 4;
    write32(a_[7], pc_);
    if (on060<M060>()) jmp060(op, 1);
    jumpTo(t);
}

template <bool M060> void Cpu020::opRts(uint16_t, For<M060>) {
    uint32_t t = read32(a_[7]);
    // 68060 で戻り先が奇数: アドレスエラーは A7 を命令の始めの値に戻すので、A7 を進める前に例外にする
    // (68030 は進めたまま積む。cpu020.cpp の busError() の説明)
    if ((t & 1) && on060<M060>()) addressErrorFetch(t);
    a_[7] += 4;
    jumpTo(t);
}

template <bool M060> void Cpu020::opRtd(uint16_t, For<M060>) {
    uint32_t disp = sext16(fetch16<M060>());
    uint32_t t = read32(a_[7]);
    a_[7] += 4 + disp;
    jumpTo(t);
}

void Cpu020::opRtr(uint16_t) {
    uint16_t ccr = read16(a_[7]);
    uint32_t t = read32(a_[7] + 2);
    a_[7] += 6;
    setCCR((uint8_t)ccr);
    jumpTo(t);
}

void Cpu020::opRte(uint16_t) {
    if (!sflag_) {
        privilege();
        return;
    }
    tick(20);
    for (;;) {
        uint32_t sp = a_[7];
        uint16_t sr = read16(sp);
        uint32_t pc = read32(sp + 2);
        int format = read16(sp + 6) >> 12;
        // その機種にない形はフォーマットエラー (ベクタ 14)。フレームはそのまま残し、その下に積む (MC68030UM 8.1.13)
        int size = 0;
        if (is060()) {
            switch (format) {
            case 0x0: size = 8; break;
            case 0x2: case 0x3: size = 12; break;
            case 0x4: size = 16; break;
            }
        } else {
            switch (format) {
            case 0x0: case 0x1: size = 8; break;
            case 0x2: size = 12; break;
            case 0x9: size = 20; break;
            case 0xa: size = 32; break;
            case 0xb: size = 92; break;
            }
        }
        if (!size) {
            noTrace_ = true;
            exception(14, 0, pc0_);
            return;
        }
        // 68060 で PC が奇数: アドレスエラー (フォーマット $2)。フォーマットの検査が先 (上のフォーマットエラーが優先)。
        // MC68060UM 8.4 で決まるのは、積む SR の上位バイト (S、T、割り込みマスク) が RTE を始めたときの値で、PC が
        // RTE の番地であること。フレームを外してから例外にすること (SSP は進む) と、CCR を「新しい SR が 0 なら Z、
        // bit15 が 1 なら N、ほかは 0」にすることはマニュアルに書かれていない。実機の 68060 では確かめておらず、
        // この動きを見るソフトも知らない (tests/unit/cpu060_test.cpp の testRteOddPc() は今の動きを固定するだけ)
        if ((pc & 1) && is060()) {
            uint16_t keep = (uint16_t)(getSR() & 0xff00);
            a_[7] = sp + size;
            setSR((uint16_t)(keep | (sr == 0 ? 0x04 : 0) | (sr & 0x8000 ? 0x08 : 0)));
            saveUndo(); // addressErrorFetch() が A7 と CCR を命令の始めの値に戻さないように、今の値を始めの値にする
            addressErrorFetch(pc);
        }
        // 68030 のバスエラー ($B): 処理ルーチンがデータの転送を済ませて DF を 0 にしていれば、そのアクセスを除いて
        // 続ける (Cpu020::busError() の説明)。DF が 1 か、命令の読み込みのバスエラーなら、命令をはじめからやり直す
        bool resume = false, skip = false;
        uint16_t ssw = 0, internal = 0;
        uint32_t faultAddr = 0, dib = 0, next = 0;
        if (format == 0xb && ((internal = read16(sp + 0x14)) & 1)) {
            ssw = read16(sp + 0x0a);
            if (!(ssw & 0x0100)) {
                faultAddr = read32(sp + 0x10);
                dib = read32(sp + 0x2c);
                next = read32(sp + 0x1c);
                resume = !(ssw & 0x0080);
                skip = !resume; // RM: 処理ルーチンが命令全体を済ませた (8.2.2)
            }
        }
        a_[7] = sp + size;
        setSR(sr);
        if (format == 1) continue; // スローアウェイフレーム: 新しいスタックからもう一度
        jumpTo(skip ? next : pc);
        if (resume) resumeFaulted(faultAddr, ssw, dib, internal >> 8);
        return;
    }
}

void Cpu020::opTrap(uint16_t op) { exception(32 + (op & 15), 0, pc_); }

void Cpu020::opTrapv(uint16_t) {
    if (flagV_) trapFormat2(7);
}

// TRAPcc (0x50FA: .W、0x50FB: .L、0x50FC: オペランドなし)
void Cpu020::opTrapcc(uint16_t op) {
    switch (op & 7) {
    case 2: fetch16(); break;
    case 3: fetch32(); break;
    }
    if (cond((op >> 8) & 15)) trapFormat2(7);
}

template <int S, bool M060> void Cpu020::opLink(uint16_t op, For<M060>) {
    int an = REG;
    uint32_t disp = S == 2 ? sext16(fetch16<M060>()) : fetch32<M060>();
    logA(7);
    a_[7] -= 4;
    write32(a_[7], a_[an]); // LINK A7 は減らしたあとの A7 を積む (MC68030UM の LINK: SP - 4 → SP、An → (SP))
    a_[an] = a_[7];
    a_[7] += disp;
}

void Cpu020::opUnlk(uint16_t op) {
    int an = REG;
    uint32_t v = read32(a_[an]);
    a_[7] = a_[an] + 4;
    a_[an] = v;
}

void Cpu020::opNop(uint16_t) {}

void Cpu020::opReset(uint16_t) {
    if (!sflag_) {
        privilege();
        return;
    }
    bus_->resetDevices();
    tick(128);
}

void Cpu020::opStop(uint16_t) {
    if (!sflag_) {
        privilege();
        return;
    }
    uint16_t imm = fetch16();
    // S を 0 にする値なら特権違反 (M68000PRM の STOP: "If bit S is set to zero by the immediate data ... privilege violation")
    if (!(imm & 0x2000)) {
        privilege();
        return;
    }
    setSR(imm);
    stopped_ = true;
}

void Cpu020::opIllegal(uint16_t) { illegal(); }
void Cpu020::opLineA(uint16_t) { lineA(); }
void Cpu020::opLineF(uint16_t) { lineF(); }

// BKPT: ブレークポイントのアクノリッジに応える装置がないので、不当命令の例外になる
void Cpu020::opBkpt(uint16_t) { illegal(); }

// CALLM / RTM は 68020 だけの命令。68030 と 68060 では不当命令
void Cpu020::opCallm(uint16_t) { illegal(); }

// ---------------------------------------------------------------- ビット操作

template <int OP> static inline uint32_t bitOp020(uint32_t v, int bit) {
    switch (OP) {
    case BIT_CHG: return v ^ (1u << bit);
    case BIT_CLR: return v & ~(1u << bit);
    case BIT_SET: return v | (1u << bit);
    default: return v;
    }
}

template <int OP, bool M060> void Cpu020::opBitDyn(uint16_t op, For<M060>) {
    int mode = MODE, reg = REG;
    uint32_t bitsrc = d_[REG9];
    if (mode == 0) {
        int bit = bitsrc & 31;
        flagZ_ = ((d_[reg] >> bit) & 1) ? 0 : 1;
        d_[reg] = bitOp020<OP>(d_[reg], bit);
        return;
    }
    int bit = bitsrc & 7;
    if (mode == 7 && reg == 4) {
        uint32_t v = fetch16<M060>() & 0xff;
        flagZ_ = ((v >> bit) & 1) ? 0 : 1;
        return;
    }
    uint32_t addr = eaAddr<M060>(mode, reg, 1);
    uint32_t v = read8(addr);
    flagZ_ = ((v >> bit) & 1) ? 0 : 1;
    if (OP != BIT_TST) write8(addr, bitOp020<OP>(v, bit));
}

template <int OP, bool M060> void Cpu020::opBitImm(uint16_t op, For<M060>) {
    int mode = MODE, reg = REG;
    int bitsrc = fetch16<M060>() & 0xff;
    if (mode == 0) {
        int bit = bitsrc & 31;
        flagZ_ = ((d_[reg] >> bit) & 1) ? 0 : 1;
        d_[reg] = bitOp020<OP>(d_[reg], bit);
        return;
    }
    int bit = bitsrc & 7;
    uint32_t addr = eaAddr<M060>(mode, reg, 1);
    uint32_t v = read8(addr);
    flagZ_ = ((v >> bit) & 1) ? 0 : 1;
    if (OP != BIT_TST) write8(addr, bitOp020<OP>(v, bit));
}

// ---------------------------------------------------------------- シフト・ローテート

template <int S, int TYPE, bool LEFT> uint32_t Cpu020::shift(uint32_t v, int count) {
    const uint32_t m = Mask<S>::v, msb = Msb<S>::v;
    const int bits = S * 8;
    v &= m;
    flagV_ = 0;
    if (count == 0) {
        flagC_ = (TYPE == SH_ROX) ? flagX_ : 0;
        setNZ<S>(v);
        return v;
    }
    uint32_t c = 0;
    switch (TYPE) {
    case SH_LS:
        if (count > bits) {
            c = 0;
            v = 0;
        } else if (LEFT) {
            c = (uint32_t)(((uint64_t)v >> (bits - count)) & 1);
            v = (uint32_t)(((uint64_t)v << count) & m);
        } else {
            c = (uint32_t)(((uint64_t)v >> (count - 1)) & 1);
            v = (uint32_t)((uint64_t)v >> count);
        }
        flagX_ = c;
        break;
    case SH_AS:
        if (LEFT) {
            // V: シフトの途中で最上位ビットが一度でも変わったか
            uint64_t wide = (uint64_t)v;
            for (int i = 0; i < count; i++) {
                c = (uint32_t)((wide >> (bits - 1)) & 1);
                uint64_t nv = (wide << 1) & m;
                if ((nv ^ wide) & msb) flagV_ = 1;
                wide = nv;
            }
            v = (uint32_t)wide;
        } else {
            int32_t sv = S == 1 ? (int8_t)v : S == 2 ? (int16_t)v : (int32_t)v;
            if (count >= bits) {
                c = (uint32_t)(sv < 0);
                sv = sv < 0 ? -1 : 0;
            } else {
                c = (uint32_t)((sv >> (count - 1)) & 1);
                sv >>= count;
            }
            v = (uint32_t)sv & m;
        }
        flagX_ = c;
        break;
    case SH_RO: {
        int n = count & (bits - 1);
        if (LEFT) {
            if (n) v = ((v << n) | (v >> (bits - n))) & m;
            c = v & 1;
        } else {
            if (n) v = ((v >> n) | (v << (bits - n))) & m;
            c = (v & msb) ? 1 : 0;
        }
        break;
    }
    case SH_ROX: {
        int n = count % (bits + 1);
        c = flagX_;
        for (int i = 0; i < n; i++) {
            uint32_t nc;
            if (LEFT) {
                nc = (v & msb) ? 1 : 0;
                v = ((v << 1) | c) & m;
            } else {
                nc = v & 1;
                v = (v >> 1) | (c ? msb : 0);
            }
            c = nc;
        }
        flagX_ = c;
        break;
    }
    }
    flagC_ = c;
    setNZ<S>(v);
    return v;
}

template <int S, int TYPE, bool LEFT, bool M060> void Cpu020::opShiftReg(uint16_t op, For<M060>) {
    int reg = REG;
    int count = (op & 0x20) ? (int)(d_[REG9] & 63) : (REG9 ? REG9 : 8);
    setD<S>(reg, shift<S, TYPE, LEFT>(d_[reg], count));
    if (!M060) tick((S == 4 ? 4 : 2) + count);
}

template <int TYPE, bool LEFT, bool M060> void Cpu020::opShiftMem(uint16_t op, For<M060>) {
    if (TYPE == 2) saveCcrUndo(); // ROXL / ROXR は X を使う
    uint32_t addr = eaAddr<M060>(MODE, REG, 2);
    uint32_t v = read16(addr);
    write16(addr, shift<2, TYPE, LEFT>(v, 1));
}

// ---------------------------------------------------------------- 乗除算

template <bool SIGNED, bool M060> void Cpu020::opMulW(uint16_t op, For<M060>) {
    uint32_t s = readEa<2, M060>(MODE, REG);
    int dn = REG9;
    uint32_t r = SIGNED ? (uint32_t)((int32_t)(int16_t)d_[dn] * (int32_t)(int16_t)s) : (d_[dn] & 0xffff) * s;
    d_[dn] = r;
    logic<4>(r);
    if (!M060) tick(28);
}

// DIVU.W / DIVS.W <ea>,Dn
// 未定義のフラグは instructiontest.x の実機の結果に合わせる (DIVU.L / DIVS.L と同じ形):
//   ゼロ除算: 符号なしは N と Z を Dn (32 ビット) から、符号付きは N=0 Z=1 (Z は試験では決まらず、DIVS.L に合わせた)。V と C は 0
//   あふれ (被除数の絶対値の上位 16 ビット >= 除数の絶対値): N と Z を被除数の絶対値 (32 ビット) から
//   あふれ (それ以外。符号付きで商が 16 ビットに収まらない): N は商のビット 7、Z は商の下位 8 ビットが 0 か
// 68060 はゼロ除算で C だけを 0 にし (N、Z、V は変えない)、あふれで V を 1、C を 0 にする (N と Z は変えない)。
// instructiontest.x hard の div* の 14 件が実機と合う。ゼロ除算のときの V は、試験では決まらない。今は前の値を保つ。
// 実機では確かめていない
template <bool SIGNED, bool M060> void Cpu020::opDivW(uint16_t op, For<M060>) {
    uint32_t s = readEa<2, M060>(MODE, REG);
    if (!M060) tick(SIGNED ? 120 : 100);
    int dn = REG9;
    uint32_t d = d_[dn];
    if (s == 0) {
        if (on060<M060>()) {
            flagC_ = 0;
            trapFormat2(5);
            return;
        }
        if (SIGNED) {
            flagN_ = 0;
            flagZ_ = 1;
        } else {
            flagN_ = d >> 31;
            flagZ_ = d == 0;
        }
        flagV_ = flagC_ = 0;
        trapFormat2(5);
        return;
    }
    // 絶対値で割ってから符号を付ける (商は 0 へ丸め、余りは被除数の符号)
    bool nneg = SIGNED && (int32_t)d < 0, dneg = SIGNED && (int16_t)s < 0;
    uint32_t an = nneg ? 0 - d : d, ad = dneg ? (uint32_t)(0 - sext16(s)) : s;
    flagC_ = 0;
    if (on060<M060>() && ((an >> 16) >= ad || (SIGNED && (nneg != dneg ? an / ad > 0x8000u : an / ad > 0x7fffu)))) {
        flagV_ = 1; // あふれ (68060)
        return;
    }
    if ((an >> 16) >= ad) {
        // 商が 16 ビットに収まらない
        flagN_ = an >> 31;
        flagZ_ = an == 0;
        flagV_ = 1;
        return;
    }
    uint32_t qm = an / ad, rm = an % ad;
    bool qneg = nneg != dneg;
    uint32_t q = qneg ? 0 - qm : qm;
    if (SIGNED && (qneg ? qm > 0x8000u : qm > 0x7fffu)) {
        flagN_ = (q >> 7) & 1;
        flagZ_ = (q & 0xff) == 0;
        flagV_ = 1;
        return;
    }
    uint32_t r = nneg ? 0 - rm : rm;
    d_[dn] = (r << 16) | (q & 0xffff);
    flagN_ = (q >> 15) & 1;
    flagZ_ = (q & 0xffff) == 0;
    flagV_ = 0;
}

// MULU.L / MULS.L <ea>,Dl / <ea>,Dh:Dl
template <bool M060> void Cpu020::opMulL(uint16_t op, For<M060>) {
    uint16_t ext = fetch16<M060>();
    bool sign = (ext & 0x800) != 0, quad = (ext & 0x400) != 0;
    // 68060 は 64 ビットの積を作らない (未実装整数命令)
    if (quad && on060<M060>() && unimpl060()) return;
    uint32_t s = readEa<4, M060>(MODE, REG);
    int dl = (ext >> 12) & 7, dh = ext & 7;
    if (!M060) tick(40);
    uint64_t r;
    if (sign) r = (uint64_t)((int64_t)(int32_t)d_[dl] * (int64_t)(int32_t)s);
    else r = (uint64_t)d_[dl] * s;
    flagC_ = 0;
    if (quad) {
        d_[dh] = (uint32_t)(r >> 32);
        d_[dl] = (uint32_t)r;
        flagN_ = (uint32_t)(r >> 63);
        flagZ_ = r == 0;
        flagV_ = 0;
    } else {
        uint32_t lo = (uint32_t)r;
        d_[dl] = lo;
        flagN_ = lo >> 31;
        flagZ_ = lo == 0;
        if (sign) flagV_ = (int64_t)r != (int64_t)(int32_t)lo;
        else flagV_ = (r >> 32) != 0;
    }
}

// DIVU.L / DIVS.L <ea>,Dq / <ea>,Dr:Dq / DIVUL.L / DIVSL.L
// 被除数は 64 ビットのとき Dr:Dq、32 ビットのとき Dq (符号付きなら符号拡張)。Dr と Dq が同じなら商だけを残す。
// 未定義のフラグは instructiontest.x の実機の結果に合わせる:
//   ゼロ除算: 符号なしは N と Z を Dq から、符号付きは N=0 Z=1。V と C は 0
//   あふれ (被除数の絶対値の上位 32 ビット >= 除数の絶対値): N と Z を被除数の絶対値の下位 32 ビットから
//   あふれ (それ以外。符号付きで商が 32 ビットに収まらない): N は商のビット 7、Z は商の下位 8 ビットが 0 か
// 68060 は DIVU.W / DIVS.W と同じく、ゼロ除算で C だけを 0 に、あふれで V を 1、C を 0 にする
template <bool M060> void Cpu020::opDivL(uint16_t op, For<M060>) {
    uint16_t ext = fetch16<M060>();
    bool sign = (ext & 0x800) != 0, quad = (ext & 0x400) != 0;
    // 68060 は 64 ビットの被除数を扱わない (未実装整数命令)
    if (quad && on060<M060>() && unimpl060()) return;
    uint32_t s = readEa<4, M060>(MODE, REG);
    int dq = (ext >> 12) & 7, dr = ext & 7;
    if (!M060) tick(70);
    if (s == 0) {
        if (on060<M060>()) {
            flagC_ = 0;
            trapFormat2(5);
            return;
        }
        if (sign) {
            flagN_ = 0;
            flagZ_ = 1;
        } else {
            flagN_ = d_[dq] >> 31;
            flagZ_ = d_[dq] == 0;
        }
        flagV_ = flagC_ = 0;
        trapFormat2(5);
        return;
    }
    // 絶対値で割ってから符号を付ける (商は 0 へ丸め、余りは被除数の符号)
    bool nneg, dneg;
    uint64_t an, ad;
    if (quad) {
        uint64_t u = ((uint64_t)d_[dr] << 32) | d_[dq];
        nneg = sign && (int64_t)u < 0;
        an = nneg ? 0 - u : u;
    } else {
        nneg = sign && (int32_t)d_[dq] < 0;
        an = nneg ? (uint64_t)(0 - d_[dq]) : d_[dq];
    }
    dneg = sign && (int32_t)s < 0;
    ad = dneg ? (uint32_t)(0 - s) : s;
    flagC_ = 0;
    if (on060<M060>() && ((an >> 32) >= ad || (sign && (nneg != dneg ? an / ad > 0x80000000u : an / ad > 0x7fffffffu)))) {
        flagV_ = 1; // あふれ (68060)
        return;
    }
    if ((an >> 32) >= ad) {
        // 商が 32 ビットに収まらない
        flagN_ = ((uint32_t)an >> 31) & 1;
        flagZ_ = (uint32_t)an == 0;
        flagV_ = 1;
        return;
    }
    uint32_t qm = (uint32_t)(an / ad), rm = (uint32_t)(an % ad);
    bool qneg = nneg != dneg;
    uint32_t q = qneg ? 0 - qm : qm;
    if (sign && (qneg ? qm > 0x80000000u : qm > 0x7fffffffu)) {
        flagN_ = (q >> 7) & 1;
        flagZ_ = (q & 0xff) == 0;
        flagV_ = 1;
        return;
    }
    if (dr != dq) d_[dr] = nneg ? 0 - rm : rm;
    d_[dq] = q;
    flagN_ = q >> 31;
    flagZ_ = q == 0;
    flagV_ = 0;
}

// CHK.W (0x4180) / CHK.L (0x4100)
template <int S> void Cpu020::opChk(uint16_t op) {
    uint32_t x = S == 2 ? readEa<2>(MODE, REG) : readEa<4>(MODE, REG);
    tick(10);
    int32_t r = S == 2 ? (int16_t)d_[REG9] : (int32_t)d_[REG9];
    int32_t ub = S == 2 ? (int16_t)x : (int32_t)x;
    // N は Dn < 0 (マニュアルの定義どおり)。未定義の Z/V/C は実機に合わせる:
    // 68030 は Z が Dn == 0、V と C が上限 - Dn の引き算のもの。68060 は Z、V、C を変えない
    // (XEiJ の説明書 history.htm の CHK の表。instructiontest.x hard の chk.w / chk.l で実機と合う)
    flagN_ = r < 0;
    if (is060()) {
        if (r < 0 || r > ub) trapFormat2(6);
        return;
    }
    constexpr uint32_t msb = S == 2 ? 0x8000u : 0x80000000u, m = S == 2 ? 0xffffu : 0xffffffffu;
    uint32_t ur = (uint32_t)r & m, uu = (uint32_t)ub & m, d = (uu - ur) & m;
    flagZ_ = r == 0;
    flagV_ = (((uu ^ ur) & (uu ^ d)) & msb) != 0;
    flagC_ = uu < ur;
    if (r < 0 || r > ub) trapFormat2(6);
}

// CMP2 / CHK2
template <int S> void Cpu020::opChk2Cmp2(uint16_t op) {
    uint16_t w = fetch16();
    uint32_t a = eaAddr(MODE, REG, S);
    tick(16);
    uint32_t lb = readS<S>(a), ub = readS<S>(a + S), r;
    int rn = (w >> 12) & 7;
    if (w & 0x8000) {
        if (S == 1) lb = sext8(lb), ub = sext8(ub);
        if (S == 2) lb = sext16(lb), ub = sext16(ub);
        r = a_[rn];
    } else {
        r = d_[rn] & Mask<S>::v;
    }
    // Z と C はマニュアルの定義どおり (範囲は境界を含み、下限 > 上限なら回り込む)
    flagZ_ = r == lb || r == ub;
    flagC_ = lb <= ub ? (r < lb || r > ub) : (r > ub && r < lb);
    if (!is060()) {
        chk2Flags<S>(lb, ub, r, (w & 0x8000) != 0);
    } else { // 68060 (060SP を肩代わりするとき): V = 0、N は Rn の符号 (Dn は大きさで符号拡張した値の符号)
        constexpr uint32_t msb = S == 1 ? 0x80u : S == 2 ? 0x8000u : 0x80000000u;
        flagN_ = (w & 0x8000) ? r >> 31 : (r & msb) != 0;
        flagV_ = 0;
    }
    if ((w & 0x800) && flagC_) trapFormat2(6);
}

// CMP2 / CHK2 の未定義の N と V (68030)。
// 下限 L、上限 U、Rn の D を 32 ビットに符号拡張し (An の D はそのまま)、符号なしで比べて、U - D、L - D、D - L の
// どれか 1 つの引き算の N と V にする:
//   L < D かつ L <= U、または D = U なら U - D。そうでなく D < U < L なら L - D。どちらでもなければ D - L
// この引き算の Z と C は、定義どおりの Z と C といつも同じになる (乱数で確かめた)。
// 出典: XEiJ の更新履歴 (history.htm) の MC68EC030 の CHK2 / CMP2 の項 (Y.Sugahara 氏が示した手順を元にしたもの)。
// コードは見ていない。instructiontest.x の hard (実機の 68030 の結果) の chk2 / cmp2 の 12 件がすべて合う。
// 68060 では呼ばない。68060 は CHK2 / CMP2 をハードウェアで実行せず、060turbo.sys などの 060SP (Motorola の isp.s の
// chk2_cmp2_compare) を入れていればそれが実行する (Z と C だけを書き換え、X、N、V は命令の前のまま)。060SP がなく
// pix68k が肩代わりするとき (opUnimpInt()) は、N = (Rn < 0)、V = 0 にする (opChk2Cmp2())。68060 の実機ではこの命令は
// 例外になるので実機の値はなく、根拠になる資料もない。持ち主の決定 (#36) でこの値にした
template <int S> void Cpu020::chk2Flags(uint32_t lb, uint32_t ub, uint32_t r, bool isA) {
    if (!isA) { // Dn: 下限と上限と Rn を符号拡張する。An の下限と上限は呼ぶ前に符号拡張してある
        if (S == 1) lb = sext8(lb), ub = sext8(ub), r = sext8(r);
        if (S == 2) lb = sext16(lb), ub = sext16(ub), r = sext16(r);
    }
    uint32_t x, y;
    if ((lb < r && lb <= ub) || r == ub) x = ub, y = r;
    else if (r < ub && ub < lb) x = lb, y = r;
    else x = r, y = lb;
    uint32_t d = x - y;
    flagN_ = d >> 31;
    flagV_ = ((x ^ y) & (x ^ d)) >> 31;
}

// CAS Dc,Du,<ea>
template <int S> void Cpu020::opCas(uint16_t op) {
    uint16_t w = fetch16();
    int dc = w & 7, du = (w >> 6) & 7;
    uint32_t a = eaAddr(MODE, REG, S);
    // 68060 は境界にそろっていないオペランドの CAS を実行しない (未実装整数命令)
    if (S > 1 && (a & (S - 1)) && is060() && unimpl060()) return;
    uint32_t m = readS<S>(a);
    cmp<S>(d_[dc], m);
    tick(8);
    if (flagZ_) writeS<S>(a, d_[du]);
    else setD<S>(dc, m);
}

// CAS2 Dc1:Dc2,Du1:Du2,(Rn1):(Rn2)
template <int S> void Cpu020::opCas2(uint16_t) {
    uint16_t w1 = fetch16(), w2 = fetch16();
    int r1 = w1 >> 12, r2 = w2 >> 12;
    uint32_t a1 = r1 < 8 ? d_[r1] : a_[r1 - 8];
    uint32_t a2 = r2 < 8 ? d_[r2] : a_[r2 - 8];
    int dc1 = w1 & 7, du1 = (w1 >> 6) & 7, dc2 = w2 & 7, du2 = (w2 >> 6) & 7;
    uint32_t m1 = readS<S>(a1);
    uint32_t m2 = readS<S>(a2);
    tick(16);
    cmp<S>(d_[dc1], m1);
    if (flagZ_) {
        cmp<S>(d_[dc2], m2);
        if (flagZ_) {
            writeS<S>(a1, d_[du1]);
            writeS<S>(a2, d_[du2]);
            return;
        }
    }
    if (S == 2) {
        d_[dc1] = (d_[dc1] & 0xffff0000u) | m1;
        d_[dc2] = (d_[dc2] & 0xffff0000u) | m2;
    } else {
        d_[dc1] = m1;
        d_[dc2] = m2;
    }
}

// MOVES: SFC / DFC のファンクションコードで読み書きする
template <int S> void Cpu020::opMoves(uint16_t op) {
    if (!sflag_) {
        privilege();
        return;
    }
    uint16_t w = fetch16();
    int rn = w >> 12;
    uint32_t a = eaAddr(MODE, REG, S);
    if (is060()) {
        // 68060: 命令空間 (2 / 6) はデータ空間 (1 / 5) として扱い、0 / 3 / 4 / 7 は変換しない物理アドレス
        // (MC68060UM 4.2.5、表 4-2)
        int fc = (int)((w & 0x800) ? dfc_ : sfc_);
        if (fc == 2 || fc == 6) fc--;
        if (fc == 0 || fc == 3 || fc == 4 || fc == 7) {
            if (w & 0x800) {
                altWrite(a, rn < 8 ? d_[rn] : a_[rn - 8], S, fc);
            } else {
                uint32_t v = altRead(a, S, fc);
                if (rn < 8) setD<S>(rn, v);
                else a_[rn - 8] = S == 1 ? sext8(v) : S == 2 ? sext16(v) : v;
            }
            return;
        }
        if (w & 0x800) {
            uint32_t v = rn < 8 ? d_[rn] : a_[rn - 8];
            if (S == 1) memWrite8(a, (uint8_t)v, fc);
            else if (S == 2) memWrite16(a, (uint16_t)v, fc);
            else memWrite32(a, v, fc);
        } else {
            uint32_t v = S == 1 ? memRead8(a, fc) : S == 2 ? memRead16(a, fc) : memRead32(a, fc);
            if (rn < 8) setD<S>(rn, v);
            else a_[rn - 8] = S == 1 ? sext8(v) : S == 2 ? sext16(v) : v;
        }
        return;
    }
    if (w & 0x800) { // Rn → <ea>
        uint32_t v = rn < 8 ? d_[rn] : a_[rn - 8];
        if (S == 1) memWrite8(a, (uint8_t)v, (int)dfc_);
        else if (S == 2) memWrite16(a, (uint16_t)v, (int)dfc_);
        else memWrite32(a, v, (int)dfc_);
    } else {
        uint32_t v = S == 1 ? memRead8(a, (int)sfc_) : S == 2 ? memRead16(a, (int)sfc_) : memRead32(a, (int)sfc_);
        if (rn < 8) setD<S>(rn, v);
        else a_[rn - 8] = S == 1 ? sext8(v) : S == 2 ? sext16(v) : v;
    }
}

void Cpu020::opMovec(uint16_t op) {
    if (!sflag_) {
        privilege();
        return;
    }
    uint16_t w = fetch16();
    int rn = w >> 12;
    uint32_t& reg = rn < 8 ? d_[rn] : a_[rn - 8];
    bool toCr = (op & 1) != 0;
    uint32_t v = toCr ? reg : 0;
    tick(10);
    int cr = w & 0xfff;
    bool m060 = is060();
    // MC68060UM 表 10-24: CACR、TC、ITT / DTT、BUSCR、URP、SRP は +3 (SFC、DFC、USP、VBR、PCR は表 ops060_ の値のまま)
    if (cr == 0x002 || cr == 0x003 || (cr >= 0x004 && cr <= 0x008) || cr == 0x806 || cr == 0x807) tick060(3);
    switch (cr) {
    case 0x000:
        if (toCr) sfc_ = v & 7;
        else v = sfc_;
        break;
    case 0x001:
        if (toCr) dfc_ = v & 7;
        else v = dfc_;
        break;
    case 0x002:
        // 68060 の CACR (MC68060UM 図 5-5): EDC NAD ESB DPI FOC EBC EIC NAI FIC。CABC と CUBC は常に 0 と読める
        if (toCr) {
            if (m060) {
                // CABC (ビット 22) は分岐キャッシュを全部、CUBC (ビット 21) はユーザモードで入れたものを消す。
                // どちらも書いたときだけ働き、読むと 0 (MC68060UM 5.2)
                if (v & 0x00400000u) clearBranchCache060(false);
                else if (v & 0x00200000u) clearBranchCache060(true);
            }
            cacr_ = v & (m060 ? 0xf880e000u : 0x00003f1fu);
            updateCacheCosts();
        }
        else v = cacr_;
        break;
    case 0x800:
        if (toCr) usp_ = v;
        else v = usp_;
        break;
    case 0x801:
        if (toCr) vbr_ = v & ~3u;
        else v = vbr_;
        break;
    case 0x802:
        if (m060) {
            illegal();
            return;
        }
        if (toCr) caar_ = v;
        else v = caar_;
        break;
    case 0x803:
    case 0x804:
        if (m060) {
            illegal();
            return;
        }
        if (cr == 0x803) {
            if (toCr) {
                if (mflag_) a_[7] = v;
                else msp_ = v;
            } else {
                v = mflag_ ? a_[7] : msp_;
            }
        } else {
            if (toCr) {
                if (mflag_) isp_ = v;
                else a_[7] = v;
            } else {
                v = mflag_ ? isp_ : a_[7];
            }
        }
        break;
    default:
        if (!m060) {
            illegal();
            return;
        }
        switch (cr) {
        case 0x003:
            if (toCr) {
                tc060_ = v & 0xfffe;
                updateMmu();
                flushTlb();
            } else {
                v = tc060_;
            }
            break;
        case 0x004: case 0x005: case 0x006: case 0x007: {
            uint32_t& t = cr < 6 ? itt_[cr - 4] : dtt_[cr - 6];
            if (toCr) {
                t = v & 0xffffe364u;
                updateMmu();
                flushTlb();
            } else {
                v = t;
            }
            break;
        }
        case 0x008:
            if (toCr) buscr_ = v & 0xf0000000u;
            else v = buscr_;
            break;
        case 0x806:
            if (toCr) {
                urp_ = v & ~0x1ffu;
                flushTlb();
            } else {
                v = urp_;
            }
            break;
        case 0x807:
            if (toCr) {
                srp060_ = v & ~0x1ffu;
                flushTlb();
            } else {
                v = srp060_;
            }
            break;
        case 0x808:
            if (toCr) pcr_ = (pcr_ & 0xffffff00u) | (v & 0x83);
            else v = pcr_;
            break;
        default:
            illegal();
            return;
        }
    }
    if (!toCr) reg = v;
}

// ---------------------------------------------------------------- BCD

template <bool SUB> void Cpu020::opBcd(uint16_t op) {
    int rx = REG9, ry = REG;
    if (!(op & 8)) {
        uint8_t r = SUB ? sbcd((uint8_t)d_[ry], (uint8_t)d_[rx]) : abcd((uint8_t)d_[ry], (uint8_t)d_[rx]);
        setD<1>(rx, r);
        return;
    }
    saveCcrUndo();
    uint8_t s = read8(eaAddr(4, ry, 1));
    uint32_t da = eaAddr(4, rx, 1);
    uint8_t d = read8(da);
    write8(da, SUB ? sbcd(s, d) : abcd(s, d));
}

void Cpu020::opNbcd(uint16_t op) {
    int mode = MODE, reg = REG;
    if (mode == 0) {
        setD<1>(reg, sbcd((uint8_t)d_[reg], 0));
        return;
    }
    saveCcrUndo();
    uint32_t addr = eaAddr(mode, reg, 1);
    write8(addr, sbcd(read8(addr), 0));
}

// PACK -(Ax),-(Ay),#adj / Dx,Dy,#adj
void Cpu020::opPack(uint16_t op) {
    int rx = REG, ry = REG9;
    uint16_t adj;
    if (!(op & 8)) {
        adj = fetch16();
        uint16_t v = (uint16_t)(d_[rx] + adj);
        setD<1>(ry, ((v >> 4) & 0xf0) | (v & 0x0f));
        return;
    }
    uint32_t a = eaAddr(4, rx, 2);
    // -(Ax) から 2 バイト (下位のバイトから順に) 読む
    uint16_t lo = read8(a + 1);
    uint16_t hi = read8(a);
    adj = fetch16();
    uint16_t v = (uint16_t)(((hi << 8) | lo) + adj);
    write8(eaAddr(4, ry, 1), ((v >> 4) & 0xf0) | (v & 0x0f));
}

// UNPK -(Ax),-(Ay),#adj / Dx,Dy,#adj
void Cpu020::opUnpk(uint16_t op) {
    int rx = REG, ry = REG9;
    if (!(op & 8)) {
        uint16_t adj = fetch16();
        uint32_t s = d_[rx] & 0xff;
        uint16_t v = (uint16_t)((((s << 4) & 0x0f00) | (s & 0x0f)) + adj);
        setD<2>(ry, v);
        return;
    }
    uint32_t s = read8(eaAddr(4, rx, 1));
    uint16_t adj = fetch16();
    uint16_t v = (uint16_t)((((s << 4) & 0x0f00) | (s & 0x0f)) + adj);
    uint32_t a = eaAddr(4, ry, 2);
    write8(a + 1, (uint8_t)v);
    write8(a, (uint8_t)(v >> 8));
}

// ---------------------------------------------------------------- ビットフィールド

// BFTST/BFEXTU/BFCHG/BFEXTS/BFCLR/BFFFO/BFSET/BFINS (0xE8C0-0xEFFF)
void Cpu020::opBitField(uint16_t op) {
    int type = (op >> 8) & 7; // 0 TST, 1 EXTU, 2 CHG, 3 EXTS, 4 CLR, 5 FFO, 6 SET, 7 INS
    uint16_t w = fetch16();
    int mode = MODE, reg = REG;
    int32_t offset = (w & 0x800) ? (int32_t)d_[(w >> 6) & 7] : (int32_t)((w >> 6) & 31);
    int width = (w & 0x20) ? (int)(d_[w & 7] & 31) : (int)(w & 31);
    if (width == 0) width = 32;
    int rn = (w >> 12) & 7;
    tick(12);
    uint32_t mask = width == 32 ? 0xffffffffu : ((1u << width) - 1);
    uint32_t field;
    if (mode == 0) {
        int off = offset & 31;
        uint32_t v = d_[reg];
        // オフセットのビットを最上位に回してから取り出す
        uint32_t rot = off ? ((v << off) | (v >> (32 - off))) : v;
        field = (rot >> (32 - width)) & mask;
        flagN_ = (field >> (width - 1)) & 1;
        flagZ_ = field == 0;
        flagV_ = flagC_ = 0;
        uint32_t nf = field;
        switch (type) {
        case 1: d_[rn] = field; return;
        case 3: d_[rn] = width == 32 ? field : (uint32_t)((int32_t)(field << (32 - width)) >> (32 - width)); return;
        case 5: {
            int i = 0;
            while (i < width && !((field >> (width - 1 - i)) & 1)) i++;
            d_[rn] = (uint32_t)offset + (uint32_t)i; // 符号なしで足す ($7FFFFFFF + 1 があふれないように)
            return;
        }
        case 0: return;
        case 2: nf = ~field & mask; break;
        case 4: nf = 0; break;
        case 6: nf = mask; break;
        case 7:
            nf = d_[rn] & mask;
            flagN_ = (nf >> (width - 1)) & 1;
            flagZ_ = nf == 0;
            break;
        }
        uint32_t fmask = mask << (32 - width);
        uint32_t ins = nf << (32 - width);
        rot = (rot & ~fmask) | ins;
        d_[reg] = off ? ((rot >> off) | (rot << (32 - off))) : rot;
        return;
    }
    uint32_t base = eaAddr(mode, reg, 1);
    uint32_t a = base + (uint32_t)(offset >> 3);
    int bo = offset & 7;
    int nbytes = (bo + width + 7) >> 3; // 1..5
    uint64_t data = 0;
    for (int i = 0; i < nbytes; i++) data = (data << 8) | read8(a + i);
    int total = nbytes * 8;
    int sh = total - bo - width;
    field = (uint32_t)(data >> sh) & mask;
    flagN_ = (field >> (width - 1)) & 1;
    flagZ_ = field == 0;
    flagV_ = flagC_ = 0;
    uint32_t nf = field;
    switch (type) {
    case 1: d_[rn] = field; return;
    case 3: d_[rn] = width == 32 ? field : (uint32_t)((int32_t)(field << (32 - width)) >> (32 - width)); return;
    case 5: {
        int i = 0;
        while (i < width && !((field >> (width - 1 - i)) & 1)) i++;
        d_[rn] = (uint32_t)offset + (uint32_t)i; // 符号なしで足す ($7FFFFFFF + 1 があふれないように)
        return;
    }
    case 0: return;
    case 2: nf = ~field & mask; break;
    case 4: nf = 0; break;
    case 6: nf = mask; break;
    case 7:
        nf = d_[rn] & mask;
        flagN_ = (nf >> (width - 1)) & 1;
        flagZ_ = nf == 0;
        break;
    }
    uint64_t m64 = (uint64_t)mask << sh;
    data = (data & ~m64) | ((uint64_t)nf << sh);
    for (int i = nbytes - 1; i >= 0; i--) {
        write8(a + i, (uint8_t)data);
        data >>= 8;
    }
}

// ---------------------------------------------------------------- 68030 の MMU 命令

// PMOVE / PFLUSH / PTEST / PLOAD (コプロセッサ ID 0)。XEiJ の説明書 (feature.htm) と同じくアドレス変換はせず
// (MC68EC030 として扱う)、PMOVE はレジスタの値を読み書きするだけ、PFLUSH / PTEST / PLOAD は何もしない
void Cpu020::opMmu030(uint16_t op) {
    if (is060()) {
        lineF();
        return;
    }
    if (!sflag_) {
        privilege();
        return;
    }
    uint16_t w = fetch16();
    int mode = MODE, reg = REG;
    int kind = w >> 13;
    if (kind == 0 || kind == 2 || kind == 3) {
        // PMOVE
        bool toMem = (w & 0x200) != 0;
        if (kind == 0) { // TT0 / TT1
            int n = (w >> 10) & 1;
            if (!(w & 0x0800)) {
                lineF();
                return;
            }
            if (toMem) {
                uint32_t a = mode >= 2 ? eaAddr(mode, reg, 4) : 0;
                if (mode == 0) d_[reg] = tt030_[n];
                else if (mode == 1) a_[reg] = tt030_[n];
                else write32(a, tt030_[n]);
            } else {
                tt030_[n] = mode == 0 ? d_[reg] : mode == 1 ? a_[reg] : read32(eaAddr(mode, reg, 4));
            }
            return;
        }
        if (kind == 3) { // MMUSR
            if (toMem) {
                if (mode == 0) setD<2>(reg, mmusr030_);
                else write16(eaAddr(mode, reg, 2), mmusr030_);
            } else {
                mmusr030_ = mode == 0 ? (d_[reg] & 0xffff) : read16(eaAddr(mode, reg, 2));
            }
            return;
        }
        int preg = (w >> 10) & 7; // 0 TC, 2 SRP, 3 CRP
        if (preg == 0) {
            if (toMem) {
                if (mode == 0) d_[reg] = tc030_;
                else write32(eaAddr(mode, reg, 4), tc030_);
            } else {
                tc030_ = mode == 0 ? d_[reg] : read32(eaAddr(mode, reg, 4));
            }
            return;
        }
        if (preg == 2 || preg == 3) {
            uint64_t& rp = preg == 2 ? srp030_ : crp030_;
            uint32_t a = eaAddr(mode, reg, 8);
            if (toMem) {
                write32(a, (uint32_t)(rp >> 32));
                write32(a + 4, (uint32_t)rp);
            } else {
                uint64_t hi = read32(a);
                rp = (hi << 32) | read32(a + 4);
            }
            return;
        }
        lineF();
        return;
    }
    if (kind == 1) { // PLOAD / PFLUSH / PFLUSHA
        if (mode >= 2) eaAddr(mode, reg, 1);
        return;
    }
    if (kind == 4) { // PTEST
        if (mode >= 2) eaAddr(mode, reg, 1);
        mmusr030_ = 0;
        return;
    }
    lineF();
}

#undef MODE
#undef REG
#undef REG9

} // namespace m68k
