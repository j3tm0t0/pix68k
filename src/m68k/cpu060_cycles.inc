// 68060 の命令ごとのサイクル数の表と、2 命令の同時実行 (スーパースカラ) の分類。cpu020.cpp から include する。
//
// 出どころは M68060 User's Manual (M68060UM/AD) の 10 章 Instruction Execution Timing。使った表:
//   10.1.2 表 10-1 / 10-2 / 10-3  同時実行の分類 (pOEP|sOEP、pOEP-only、pOEP-until-last、pOEP-but-allows-sOEP)
//   10.1.3 - 10.1.6               同時実行の条件 (sOEP で使えないアドレッシングモード、メモリの読み書きは 2 命令で 1 つ、
//                                 前の命令の結果のレジスタを後の命令が使わない)
//   10.4 表 10-5                  実効アドレス ((bd,An,Xi) は +1、メモリ間接は +3。Cpu020::extAddr で足す)
//   10.5 表 10-6 / 10-7 / 10-8    MOVE、MOVE16
//   10.6 表 10-9                  ADD、AND、CMP、DIVS、DIVU、EOR、MULS、MULU、OR、SUB
//   10.7 表 10-10                 ADDI、ADDQ、ANDI、CMPI、EORI、MOVEQ、ORI、SUBI、SUBQ
//   10.8 表 10-11 / 10-12         CAS、NBCD、NEG、NEGX、NOT、Scc、TAS、TST、CLR
//   10.9 表 10-13                 シフト、ローテート
//   10.10 表 10-14 / 10-15 / 10-16 ビット操作、ビットフィールド
//   10.11 表 10-17 / 10-18 / 10-19 Bcc、BRA、BSR、DBcc、JMP、JSR、RTD、RTE、RTR、RTS (分岐は Cpu020::branch060 など)
//   10.12 表 10-20                LEA、PEA、MOVEM (レジスタの数の分は opMovem* で足す)
//   10.13 表 10-21                ADDX、CMPM、SUBX、ABCD、SBCD
//   10.14 表 10-22 / 10-23 / 10-24 SR の命令、MOVES、そのほか
//   10.15 表 10-25                FPU (fpu.cpp、fpu_core.cpp の cycles() の 2 つめの引数)
//   10.16 表 10-26                例外 (Cpu020::exception と interrupt)
//
// 表の値は、10.2 の前提 (命令キャッシュ、データキャッシュ、ATC に当たる。パイプラインが止まらない) のときのもの。
// CACR で命令キャッシュ (EIC) / データキャッシュ (EDC) を切ったときは、命令の取り出しとデータの読み書きがバスに出る分を
// 足す (10.3。Cpu020::updateCacheCosts() と execOne()。値は cpu020.h の MISS060_MAIN / MISS060_LOCAL)。
// キャッシュが有効なら、いつも当たるものとして扱う (キャッシュの中身は持たない)。
// 次のものは数えていない:
//   - ATC のミス (表を引く時間)、整列していないアクセスの +1 / +2
//   - レジスタを書いた直後にアドレスの計算に使うときの待ち (10.2 の 3)
//   - 「<= k」と書かれた命令 (DIVS.W / DIVU.W、CINV、CPUSH) の、値による違い (DIV は最大の値、CINV / CPUSH は 1 ラインの値)
//   - ビットフィールドが 5 バイトにまたがるときの追加、FPU の整数 / 拡張精度のオペランドの +3 / +2
//
// 同時実行は、表 10-2 の分類と、10.1.3 から 10.1.6 の条件のうちオペコードの 1 語めだけで決まるものを見る
// (インデックスレジスタと、フルフォーマットの拡張語かどうかは見ない)。PCR の ESS が 1 のときだけ数える。

namespace m68k {

namespace {

// cyc060() の 1 個 (ops060_ の meta の下位 16 ビット): 下位 10 ビットがサイクル数、上位が下のフラグ
enum : uint16_t {
    T_CYC = 0x03ff,
    T_PAIR = 0x0400, // sOEP に入れる (pOEP|sOEP の分類で、sOEP で使えないモードでない)
    T_LEAD = 0x0800, // この命令の (最後の) サイクルに、次の命令を sOEP に入れられる
    T_MEM = 0x1000,  // オペランドのメモリの読み書きがある
    // 分岐の命令 (Bcc / BRA / BSR、DBcc、JMP、JSR)。サイクル数と、同時実行と折りたたみの状態は、命令の中で決める
    // (bcc060()、jump060()、dbcc060()、jmp060())。ほかの命令は、表のサイクル数が 0 でも (MOVEM、FPU、例外になる命令。
    // サイクル数は命令の中で足す) execOne() が状態を進める
    T_BRANCH = 0x2000,
};
// use060() の 1 個: 命令が使うレジスタ。ビット 0-15 は演算に使うレジスタ (D0-D7、A0-A7)、16-23 はアドレスの計算に使う An。
// wr060() の 1 個: 命令が書くレジスタを、同じ並びにしたもの。前の命令の wr060() と後の命令の use060() に同じビットがあれば、
// 2 つは同時に進まない (10.1.5、10.1.6)。結果を書くレジスタは、演算に使う側とアドレスに使う側の両方に立てる。
// MOVE.L <ea>,Rx の Rx は、演算に使う側には立てない (10.1.6 の例外 1)。(An)+ / -(An) で動かす An も両方に立てる

struct Timing060 {
    int c = 0;   // サイクル数
    int cls = 0; // 0 pOEP-only、1 pOEP|sOEP、2 pOEP-until-last、3 pOEP-but-allows-sOEP
    bool mem = false, pcrel = false, branch = false;
    uint32_t iee = 0, agu = 0; // 使うレジスタ (use060())
    uint32_t wr = 0;           // 書くレジスタ (wr060())

    // 結果を書くレジスタ (0-7 は Dn、8-15 は An)。fwd は MOVE.L <ea>,Rx
    void dest(int r, bool fwd = false) {
        if (!fwd) wr |= 1u << r;
        if (r >= 8) wr |= 0x10000u << (r - 8);
    }

    // 実効アドレスの 1 つを足す。use: レジスタ直接のとき、その値を演算に使う
    void ea(int mode, int reg, bool use = true) {
        switch (mode) {
        case 0:
            if (use) iee |= 1u << reg;
            break;
        case 1:
            if (use) iee |= 1u << (8 + reg);
            break;
        case 3:
        case 4: dest(8 + reg); [[fallthrough]];
        case 2:
        case 5:
        case 6:
            agu |= 1u << reg;
            mem = true;
            break;
        default:
            if (reg <= 3) mem = true;
            if (reg == 2 || reg == 3) pcrel = true;
            break;
        }
    }
};

bool simpleMem060(int mode) { return mode >= 2 && mode <= 4; } // (An)、(An)+、-(An)
// (d8,An,Xi)、(xxx).W、(xxx).L、(d8,PC,Xi): 表で +1 になるモード
bool plusOne060(int mode, int reg) { return mode == 6 || (mode == 7 && (reg == 0 || reg == 1 || reg == 3)); }

// ADD / SUB / AND / OR の <ea>,Dn と Dn,<ea>
void arith060(Timing060& t, uint16_t op) {
    const int mode = (op >> 3) & 7, reg = op & 7, dn = (op >> 9) & 7;
    t.c = 1;
    t.cls = 1;
    t.iee |= 1u << dn;
    t.ea(mode, reg);
    if (!(op & 0x100)) t.dest(dn);
}

Timing060 timing060(uint16_t op) {
    Timing060 t;
    const int mode = (op >> 3) & 7, reg = op & 7, r9 = (op >> 9) & 7, size = (op >> 6) & 3;
    switch (op >> 12) {
    case 0x0:
        if (op & 0x100) {
            if (mode == 1) { // MOVEP (68060 にない。その場で実行するときの概算)
                t.c = 2;
                break;
            }
            t.c = 1; // BTST / BCHG / BCLR / BSET Dn,<ea> (表 10-14)
            t.ea(mode, reg);
            break;
        }
        if (r9 == 4) { // BTST / BCHG / BCLR / BSET #<imm>,<ea> (表 10-15)
            t.c = (mode == 0 || simpleMem060(mode)) ? 1 : 2;
            t.cls = 2;
            t.ea(mode, reg);
            if (mode == 0 && size != 0) t.dest(reg); // BCHG / BCLR / BSET は Dn を書く
            break;
        }
        if (r9 == 7 && size != 3) { // MOVES (表 10-23)
            t.c = (simpleMem060(mode) || mode == 5) ? 1 : 2;
            break;
        }
        if (size == 3) {
            if (r9 >= 5 && (op & 0x3f) != 0x3c) t.c = (simpleMem060(mode) || mode == 5) ? 19 : 20; // CAS (表 10-11)
            else t.c = 2; // CHK2 / CMP2 / CAS2 (68060 にない)
            break;
        }
        if ((op & 0x3f) == 0x3c) { // to CCR / to SR (表 10-24、10-22)
            if (size == 0) t.c = 1;
            else t.c = r9 == 0 ? 5 : 12;
            break;
        }
        // ORI / ANDI / SUBI / ADDI / EORI / CMPI (表 10-10)
        t.ea(mode, reg);
        if (mode == 0 || simpleMem060(mode)) {
            t.c = 1;
            t.cls = 1;
            if (mode == 0 && r9 != 6) t.dest(reg);
        } else {
            t.c = 2;
            t.cls = 2;
        }
        break;
    case 0x1:
    case 0x2:
    case 0x3: { // MOVE / MOVEA (表 10-6、10-7)
        const int dmode = (op >> 6) & 7, dreg = r9;
        const bool srcImm = mode == 7 && reg == 4, srcReg = mode <= 1;
        t.ea(mode, reg);
        if (dmode <= 1) {
            t.c = 1;
            t.cls = 1;
            t.dest(dmode * 8 + dreg, (op >> 12) == 2);
        } else {
            t.ea(dmode, dreg);
            if (srcReg) {
                t.c = 1;
                t.cls = 1;
            } else if (srcImm) {
                t.c = simpleMem060(dmode) ? 1 : 2;
                t.cls = 2;
            } else {
                t.c = 2;
                t.cls = 2;
            }
        }
        break;
    }
    case 0x4:
        if ((op & 0xfff8) == 0x49c0) { // EXTB.L
            t.c = 1;
            t.cls = 1;
            t.iee = 1u << reg;
            t.dest(reg);
            break;
        }
        if ((op & 0x1c0) == 0x1c0) { // LEA (表 10-20)
            t.c = 1;
            t.cls = 1;
            t.ea(mode, reg);
            t.mem = false;
            t.dest(8 + r9);
            break;
        }
        if ((op & 0x1c0) == 0x180 || (op & 0x1c0) == 0x100) { // CHK (表 10-24)
            t.c = 2;
            break;
        }
        switch ((op >> 8) & 0xf) {
        case 0x0: // NEGX、MOVE from SR
            t.c = 1;
            break;
        case 0x2: // CLR (表 10-12)、MOVE from CCR
            t.c = 1;
            if (size != 3) {
                t.cls = 1;
                t.ea(mode, reg, false);
                if (mode == 0) t.dest(reg);
            }
            break;
        case 0x4: // NEG、MOVE to CCR
        case 0x6: // NOT、MOVE to SR
            if (size == 3 && (op & 0x200)) {
                t.c = 12;
                break;
            }
            t.c = 1;
            t.cls = 1;
            t.ea(mode, reg);
            if (mode == 0 && size != 3) t.dest(reg);
            break;
        case 0x8:
            if (size == 0) { // NBCD、LINK.L
                t.c = mode == 1 ? 2 : 1;
                if (mode == 1) {
                    t.cls = 2;
                    t.mem = true;
                    t.dest(8 + reg);
                    t.dest(15);
                }
            } else if (size == 1) { // SWAP、BKPT、PEA (表 10-20)
                if (mode == 0) t.c = 1;
                else if (mode == 1) t.c = 0;
                else t.c = (mode == 5 || mode == 6 || (mode == 7 && reg == 3)) ? 2 : 1;
            } else if (mode == 0) { // EXT
                t.c = 1;
                t.cls = 1;
                t.iee = 1u << reg;
                t.dest(reg);
            } else { // MOVEM レジスタ → メモリ (レジスタの数の分は命令の中で足す)
                t.c = plusOne060(mode, reg) ? 1 : 0;
            }
            break;
        case 0xa:
            if (op == 0x4afc) break; // ILLEGAL
            if (size == 3) { // TAS (表 10-11)
                t.c = mode == 0 ? 1 : 17;
                break;
            }
            t.c = 1; // TST
            t.cls = 1;
            t.ea(mode, reg);
            break;
        case 0xc:
            if (size <= 1) { // MULS.L / MULU.L、DIVS.L / DIVU.L (表 10-9 と注 3)
                const bool plain = mode <= 5 || (mode == 7 && reg == 2);
                t.c = (size == 0 ? 2 : 38) + (plain ? 0 : 1);
            } else { // MOVEM メモリ → レジスタ
                t.c = plusOne060(mode, reg) ? 1 : 0;
            }
            break;
        case 0xe:
            if (op >= 0x4e80) {                           // JSR、JMP (命令の中で数える)
                t.branch = true;
                break;
            }
            if ((op & 0xfff0) == 0x4e40) break;           // TRAP (例外で数える)
            if ((op & 0xfff8) == 0x4e50) {                // LINK.W
                t.c = 2;
                t.cls = 2;
                t.mem = true;
                t.dest(8 + reg);
                t.dest(15);
            } else if ((op & 0xfff8) == 0x4e58) t.c = 1;  // UNLK
            else if ((op & 0xfff8) == 0x4e60) t.c = 2;    // MOVE to USP
            else if ((op & 0xfff8) == 0x4e68) t.c = 1;    // MOVE from USP
            else {
                switch (op) {
                case 0x4e70: t.c = 520; break; // RESET
                case 0x4e71: t.c = 9; break;   // NOP (パイプラインを空にする)
                case 0x4e72: t.c = 8; break;   // STOP
                case 0x4e73: t.c = 17; break;  // RTE
                case 0x4e74: t.c = 7; break;   // RTD
                case 0x4e75: t.c = 7; break;   // RTS
                case 0x4e76: t.c = 1; break;   // TRAPV
                case 0x4e77: t.c = 8; break;   // RTR
                case 0x4e7a: t.c = 11; break;  // MOVEC Rc,Rn (CACR などの +3 は opMovec で足す)
                case 0x4e7b: t.c = 12; break;  // MOVEC Rn,Rc
                }
            }
            break;
        }
        break;
    case 0x5:
        if (size == 3) {
            if (mode == 1) { // DBcc (命令の中で数える)
                t.branch = true;
                break;
            }
            t.c = 1;
            if (mode == 7 && reg >= 2) { // TRAPcc (TRAPF は pOEP|sOEP)
                if (((op >> 8) & 15) == 1) t.cls = 1;
            } else { // Scc
                t.cls = 3;
                t.ea(mode, reg, false);
                if (mode == 0) t.dest(reg);
            }
            break;
        }
        t.c = 1; // ADDQ / SUBQ
        t.cls = 1;
        t.ea(mode, reg);
        if (mode <= 1) t.dest(mode * 8 + reg);
        break;
    case 0x6: // Bcc、BRA、BSR (命令の中で数える)
        t.branch = true;
        break;
    case 0x7:        // MOVEQ
        t.c = 1;
        t.cls = 1;
        t.dest(r9);
        break;
    case 0x8:
    case 0xc:
        if (size == 3) { // DIVU.W / DIVS.W (最大 22)、MULU.W / MULS.W
            t.c = (op >> 12) == 8 ? 22 : 2;
        } else if ((op & 0x1f0) == 0x100) { // SBCD、ABCD (表 10-21)
            t.c = (op & 8) ? 2 : 1;
        } else if ((op >> 12) == 8 && ((op & 0x1f0) == 0x140 || (op & 0x1f0) == 0x180)) { // PACK、UNPK
            t.c = 2;
        } else if ((op >> 12) == 0xc && ((op & 0x1f8) == 0x140 || (op & 0x1f8) == 0x148 || (op & 0x1f8) == 0x188)) {
            t.c = 1; // EXG
        } else {
            arith060(t, op); // OR、AND
        }
        break;
    case 0x9:
    case 0xd:
        if (size == 3) { // SUBA / ADDA (表 10-9 の注 1: (Ay)+ / -(Ay) で Ay が結果の An なら +1)
            t.c = 1;
            t.cls = 1;
            t.ea(mode, reg);
            t.iee |= 1u << (8 + r9);
            t.dest(8 + r9);
            if ((mode == 3 || mode == 4) && reg == r9) {
                t.c = 2;
                t.cls = 0;
            }
        } else if ((op & 0x130) == 0x100) { // SUBX / ADDX (表 10-21)
            t.c = (op & 8) ? 2 : 1;
        } else {
            arith060(t, op);
        }
        break;
    case 0xa: break; // ライン A (例外で数える)
    case 0xb:
        if (size == 3 || !(op & 0x100)) { // CMPA、CMP
            t.c = 1;
            t.cls = 1;
            t.ea(mode, reg);
            t.iee |= 1u << ((size == 3 ? 8 : 0) + r9);
        } else if (mode == 1) { // CMPM (Ay)+,(Ax)+
            t.c = 2;
            t.cls = 2;
            t.ea(3, reg);
            t.ea(3, r9);
        } else { // EOR Dn,<ea>
            t.c = 1;
            t.cls = 1;
            t.iee |= 1u << r9;
            t.ea(mode, reg);
            if (mode == 0) t.dest(reg);
        }
        break;
    case 0xe:
        if (size == 3) {
            if (op & 0x800) { // ビットフィールド (表 10-16。5 バイト未満の値)
                static const uint8_t kBf[8] = {6, 6, 8, 6, 8, 9, 8, 6}; // BFTST EXTU CHG EXTS CLR FFO SET INS
                t.c = kBf[(op >> 8) & 7] + (plusOne060(mode, reg) ? 1 : 0);
            } else { // メモリのシフト (表 10-13)
                t.c = 1;
                t.cls = ((op >> 9) & 3) == 2 ? 0 : 1; // ROXL / ROXR は pOEP-only
                t.ea(mode, reg);
            }
            break;
        }
        t.c = 1; // レジスタのシフト
        t.cls = ((op >> 3) & 3) == 2 ? 0 : 1;
        t.iee = 1u << reg;
        if (op & 0x20) t.iee |= 1u << r9;
        t.dest(reg);
        break;
    case 0xf:
        switch ((op >> 8) & 0xf) {
        case 0x4: t.c = (op & 0x20) ? 26 : 17; break; // CPUSH (1 ライン)、CINV
        case 0x5:
            if ((op & 0xffc0) == 0xf500) t.c = (op & 0x10) ? 33 : 18; // PFLUSHA(N) / PFLUSH(N)
            else t.c = 15;                                            // PLPA (ATC に当たるとき)
            break;
        case 0x6: t.c = 11; break; // MOVE16 (読み出しがキャッシュに当たるとき)
        case 0x8: t.c = 15; break; // LPSTOP
        default: break;            // FPU (fpu.cpp で数える)
        }
        break;
    }
    return t;
}

} // namespace

void Cpu020::buildCycles060() {
    for (int i = 0; i < 65536; i++) {
        const Timing060 t = timing060((uint16_t)i);
        uint16_t v = (uint16_t)(t.c & T_CYC);
        if (t.c) {
            if (t.cls == 1 && t.c == 1 && !t.pcrel) v |= T_PAIR;
            if (t.cls != 0) v |= T_LEAD;
            if (t.mem) v |= T_MEM;
        }
        if (t.branch) v |= T_BRANCH;
        const uint32_t use = (t.iee & 0xffff) | ((t.agu & 0xff) << 16);
        ops060_[i].meta = (uint64_t)v | ((uint64_t)(t.wr & 0xffffffu) << 16) | ((uint64_t)use << 40);
    }
}

// 分岐キャッシュ (MC68060UM 5.11。256 個、過去の向きから 5 つの状態で次の向きを決める。状態の移り方は書かれて
// いないので、ここでは「入っていない」と 2 ビットの飽和カウンタの 4 つにする) の概算。分岐したときに入れる。
// CACR に CABC を書くと全部、CUBC を書くとユーザモードで入れたものを消す (opMovec)。CINV / CPUSH が命令キャッシュを
// 無効にするときに一緒に消えること (5.11) は入れていない。
// 戻り値: 0 予測なし、1 予測が当たった、2 予測が外れた。CACR の EBC が 0 なら、いつも予測なし
CPU020_INLINE int Cpu020::predict060(bool taken) {
    if (!(cacr_ & 0x00800000u)) return 0;
    const unsigned i = (pc0_ >> 1) & (BCACHE060 - 1);
    uint8_t& e = bcacheState060_[i];
    if (!(e & BC_VALID) || bcachePc060_[i] != pc0_) {
        // 入っていない (空か、別のアドレスの分岐が入っている): 分岐したときに入れる
        if (taken) {
            bcachePc060_[i] = pc0_;
            e = (uint8_t)(BC_VALID | (sflag_ ? 0 : BC_USER) | 2);
        }
        return 0;
    }
    int st = e & 3;
    const bool pred = st >= 2;
    st = taken ? (st < 3 ? st + 1 : 3) : (st > 0 ? st - 1 : 0);
    e = (uint8_t)((e & ~3) | st);
    return pred == taken ? 1 : 2;
}

// 予測が当たって分岐する Bcc / BRA / JMP の 0 サイクル (表 10-17、10-18) は、分岐がほかの命令と同じサイクルに
// 折りたたまれる場合の値 (10.1.2: 「instruction folding techniques allow one or two instructions to be
// simultaneously executed with a predicted taken Bcc (also for BRA and JMP instructions)」)。1 サイクルに進むのは
// 1 つか 2 つの命令と、それに折りたたまれる分岐 1 つまでなので、折りたたむ相手になる分岐でない命令が、前の分岐の
// あとに実行されていないとき (foldOk060_ が false) は、1 サイクルかかるものとする。命令の取り出しの側でも、分岐
// キャッシュは取り出すアドレスごとに引くので (5.11)、1 サイクルに向きを変えられるのは 1 回。
// どの分岐の命令 (Bcc / BRA / BSR / DBcc / JMP / JSR) もここを通って foldOk060_ を false にするので、分岐だけが
// 続く間は、どの組み合わせでも 0 サイクルの分岐が出ない。
// 戻り値は、この分岐を折りたたむときのサイクル数 (0 か 1)
CPU020_INLINE int Cpu020::fold060() {
    const int c = foldOk060_ ? 0 : 1;
    foldOk060_ = false;
    return c;
}

// Bcc (表 10-17)。予測が当たって分岐しない Bcc は、前の 1 サイクルの命令と同時に進む (10.1.2)
CPU020_INLINE void Cpu020::bcc060(bool taken, bool backward) {
    const int r = predict060(taken);
    const int fold = fold060();
    if (r == 1 && taken) {
        tick060(fold);
        pairLead_ = 0;
        return;
    }
    int c;
    if (r == 1) c = 1;
    else if (r == 2) c = 7;
    else c = taken ? (backward ? 3 : 7) : (backward ? 7 : 1);
    if (!taken && c == 1) {
        // 分岐しないと予測した Bcc と、予測のない前向きの Bcc は pOEP-but-allows-sOEP (表 10-2 の注 1)
        if (r == 1 && pairLead_) {
            pairLead_ = 0;
            return;
        }
        tick060(1);
        pairLead_ = (pcr_ & 1) ? T_LEAD : 0;
        pairDest_ = 0;
        return;
    }
    tick060(c);
    pairLead_ = 0;
}

// BRA、JMP (d16,PC) / (xxx).WL: 予測が当たれば predicted、なければ 3。BSR、JSR は predicted が 1 (表 10-17、10-18)
void Cpu020::jump060(int predicted) {
    pairLead_ = 0;
    const bool hit = predict060(true) == 1;
    const int fold = fold060();
    tick060(!hit ? 3 : predicted ? predicted : fold);
}

// DBcc (表 10-17)。dbra は条件が F (DBRA の列)
void Cpu020::dbcc060(bool taken, bool dbra) {
    const int r = predict060(taken);
    int c;
    if (r == 1) c = 2;
    else if (r == 2) c = 8;
    else c = taken ? 3 : 8;
    if (dbra && c != 3) c--;
    tick060(c);
    pairLead_ = 0;
    foldOk060_ = false;
}

} // namespace m68k
