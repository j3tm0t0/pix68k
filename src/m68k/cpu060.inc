// MC68060 だけの部分: MMU (アドレス変換)、アクセスエラー、未実装整数命令、CINV / CPUSH、PFLUSH / PLPA、MOVE16、
// LPSTOP。cpu020.cpp から include する。根拠は MC68060 User's Manual (MC68060UM) と
// M68000 Family Programmer's Reference Manual (M68000PM)。
//
// MMU (MC68060UM 4 章):
// - TC の E が 1 なら、URP (ユーザ) / SRP (スーパーバイザ) から 3 段の表を引く (ルート 7 ビット、ポインタ 7 ビット、
//   ページ 6 ビット (4KB) か 5 ビット (8KB))。間接のページデスクリプタは 1 段だけ。U と M のビットを立てる
// - ITT0/1 (命令)、DTT0/1 (データ) に当たれば変換しない (書き込み保護だけ見る)。E が 0 なら変換しない
//   (TC の DWO が 1 ならデータの書き込みは保護違反)
// - 変換の結果は TLB (ホストのメモリを直接読み書きするページ) と ATC (それ以外) に入れる。キャッシュの
//   モード (CM) やユーザ属性 (U0 / U1) は見ない。どちらも PFLUSH、MOVEC で TC / TTR / URP / SRP を書いたときに消す
// キャッシュは持たないので、CINV / CPUSH は何もしない (命令の読み込みも常にメモリから読む)。

namespace m68k {

namespace {
// FSLW (MC68060UM 図 8-6) のビット
enum : uint32_t {
    FSLW_MA = 1u << 27,
    FSLW_IO = 1u << 15,
    FSLW_PTA = 1u << 12,
    FSLW_PTB = 1u << 11,
    FSLW_IL = 1u << 10,
    FSLW_PF = 1u << 9,
    FSLW_SP = 1u << 8,
    FSLW_WP = 1u << 7,
    FSLW_TWE = 1u << 6,
    FSLW_RE = 1u << 5,
    FSLW_WE = 1u << 4,
    FSLW_TTR = 1u << 3,
};
} // namespace

void Cpu020::updateMmu() {
    // 変換するか、TTR / TC で書き込みを保護しているときだけ translateSlow() を通す
    bool on = (tc060_ & 0x8000) || (tc060_ & 0x20);
    for (int i = 0; i < 2; i++) {
        if ((dtt_[i] & 0x8004) == 0x8004) on = true;
    }
    mmuOn_ = is060() && on;
}

void Cpu020::flushAtc() {
    for (int f = 0; f < 2; f++)
        for (int s = 0; s < 2; s++)
            for (int i = 0; i < ATC_SIZE; i++) atc_[f][s][i].tag = TLB_INVALID;
}

// FSLW の、原因によらない部分: IO / MA、RW、SIZE、TM (ファンクションコード)。TT は 0 (普通のアクセス)
uint32_t Cpu020::fslwBase(bool write, int fc, bool fetch) const {
    uint32_t v = (uint32_t)(fc & 7) << 16;
    v |= write ? (1u << 23) : (2u << 23);
    // SIZE: 00 バイト、01 ワード、10 ロング
    switch (busCycle_ & 3) {
    case BUS_BYTE: v |= 0u << 21; break;
    case BUS_WORD: v |= 1u << 21; break;
    default: v |= 2u << 21; break;
    }
    if (fetch) v = (v & ~(3u << 21)) | (1u << 21) | FSLW_IO;
    return v;
}

// アクセスエラー (MC68060UM 8.2.1、8.4.4)。68060 は命令をはじめからやり直すので、積む PC は命令の先頭で、
// 命令の途中で変えたアドレスレジスタは戻す。命令の読み込みなら、フォールトアドレスは命令の最初の語のアドレス
void Cpu020::accessFault(uint32_t addr, uint32_t cause, bool write, int fc, bool fetch) {
    inTableWalk_ = false;
    if (inException_) { // 例外のスタックを積んでいる途中 (ダブルフォルト)
        halted_ = true;
        M68K_LONGJMP(abort_, 1);
    }
    if (splitDepth_ && !fetch) {
        // 分けて行っているアクセスの途中: 元のアクセスのアドレスと大きさを積む。MA を立てるのは 2 回目以降の転送で
        // エラーになったときだけ (MC68060UM 8.4.4.3)。最初の転送は元のアドレスから始まるので、エラーになった転送の
        // アドレスで見分ける
        if (addr != splitAddr_) cause |= FSLW_MA;
        addr = splitAddr_;
        busCycle_ = splitCycle_;
    }
    splitDepth_ = 0;
    uint32_t fslw = fslwBase(write, fc, fetch) | cause;
    if (altAccess_ && !fetch) fslw |= 2u << 19; // TT = 2 (MOVES で FC 0 / 3 / 4 / 7)
    altAccess_ = false;
    uint32_t faultAddr = addr;
    if (fetch) {
        if (addr != pc0_) fslw |= FSLW_MA; // 拡張語の読み込み
        faultAddr = pc0_;
    }
    undoAddressRegs();
    noTrace_ = true;
    if (onAccessFault) onAccessFault(onUnimplementedCtx, pc0_, addr, fslw);
    // 例外処理の途中のアドレスエラー (ベクタが奇数) はダブルフォルトにする (MC68060UM 8.2.2)
    inAddrErr_ = true;
    exception(2, 4, pc0_, faultAddr, fslw);
    inAddrErr_ = false;
    M68K_LONGJMP(abort_, 1);
}

// 表のデスクリプタを読み書きする。busCycle_ は表を引くもとになったアクセスの大きさのまま変えない (アクセスエラーの FSLW に積む)
uint32_t Cpu020::physRead32(uint32_t pa) {
    const uint8_t* p = physPtr(pa & ~0xfffu, false, true);
    if (p && (pa & 0xfff) <= 0xffc) return ld32((uintptr_t)p + (pa & 0xfff));
    uint32_t hi = bus_->read16(pa, FC_SUPER_DATA);
    return (hi << 16) | bus_->read16(pa + 2, FC_SUPER_DATA);
}

void Cpu020::physWrite32(uint32_t pa, uint32_t v) {
    uint8_t* p = (uint8_t*)physPtr(pa & ~0xfffu, true, true);
    if (p && (pa & 0xfff) <= 0xffc) {
        st32((uintptr_t)p + (pa & 0xfff), v);
        return;
    }
    bus_->write16(pa, (uint16_t)(v >> 16), FC_SUPER_DATA);
    bus_->write16(pa + 2, (uint16_t)v, FC_SUPER_DATA);
}

// MOVES で FC 0 / 3 / 4 / 7 を読み書きする。FC と大きさはそのまま Bus と、アクセスエラーの FSLW (TM、TT = 2、SIZE) に渡す。
// 境界にそろっていないときは、普通のアクセスと同じく転送を分け (ロングはワード 2 回、奇数番地はバイト・ワード・バイト、
// ワードはバイト 2 回)、2 回目以降の転送のエラーでは MA を立てる (accessFault)
namespace {
// 転送の区切り。n 個の (先頭からのオフセット) を返す
int altPieces(uint32_t pa, int size, int* starts) {
    if (size == 4 && (pa & 1)) {
        starts[0] = 0, starts[1] = 1, starts[2] = 3;
        return 3;
    }
    if ((size == 4 && (pa & 2)) || (size == 2 && (pa & 1))) {
        starts[0] = 0, starts[1] = size / 2;
        return 2;
    }
    starts[0] = 0;
    return 1;
}
} // namespace

uint32_t Cpu020::altRead(uint32_t pa, int size, int fc) {
    uint32_t v = 0;
    logicalWrite_ = false;
    altAccess_ = true;
    busCycle_ = size == 1 ? BUS_BYTE : size == 2 ? BUS_WORD : BUS_LONG;
    int starts[3];
    const int n = altPieces(pa, size, starts);
    if (n > 1) {
        splitDepth_ = 1;
        splitAddr_ = pa;
        splitCycle_ = busCycle_;
    }
    for (int k = 0; k < n; k++) {
        logical_ = pa + starts[k];
        const int end = k + 1 < n ? starts[k + 1] : size;
        for (int i = starts[k]; i < end; i++) {
            const uint8_t* p = physPtr((pa + i) & ~0xfffu, false, (fc & 4) != 0);
            v = (v << 8) | (p ? p[(pa + i) & 0xfff] : bus_->read8(pa + i, fc));
        }
    }
    splitDepth_ = 0;
    altAccess_ = false;
    tick(4 * size);
    return v;
}

void Cpu020::altWrite(uint32_t pa, uint32_t v, int size, int fc) {
    logicalWrite_ = true;
    altAccess_ = true;
    busCycle_ = size == 1 ? BUS_BYTE : size == 2 ? BUS_WORD : BUS_LONG;
    int starts[3];
    const int n = altPieces(pa, size, starts);
    if (n > 1) {
        splitDepth_ = 1;
        splitAddr_ = pa;
        splitCycle_ = busCycle_;
    }
    for (int k = 0; k < n; k++) {
        logical_ = pa + starts[k];
        const int end = k + 1 < n ? starts[k + 1] : size;
        for (int i = starts[k]; i < end; i++) {
            uint8_t b = (uint8_t)(v >> (8 * (size - 1 - i)));
            uint8_t* p = (uint8_t*)physPtr((pa + i) & ~0xfffu, true, (fc & 4) != 0);
            if (p) p[(pa + i) & 0xfff] = b;
            else bus_->write8(pa + i, b, fc);
        }
    }
    splitDepth_ = 0;
    altAccess_ = false;
    tick(4 * size);
}

// 表を引く (MC68060UM 4.2.1、図 4-8、図 4-9)。writable は、このあと書いてよいか (保護がなく M を立てた)
uint32_t Cpu020::tableWalk(uint32_t addr, bool super, bool write, bool fetch, int fc, bool& writable) {
    inTableWalk_ = true;
    walkAddr_ = addr;
    walkWrite_ = write;
    walkFetch_ = fetch;
    walkFc_ = (uint8_t)fc;
    tick(20);
    // ルート: ルートポインタの上位 23 ビット + RI x 4
    const uint32_t rootAddr = ((super ? srp060_ : urp_) & ~0x1ffu) + ((addr >> 25) << 2);
    const uint32_t rd = physRead32(rootAddr);
    if (!(rd & 2)) accessFault(addr, FSLW_PTA, write, fc, fetch);
    if (!(rd & 8)) physWrite32(rootAddr, rd | 8);
    bool wp = (rd & 4) != 0;
    // ポインタ: ルートのデスクリプタの上位 23 ビット + PI x 4
    const uint32_t ptrAddr = (rd & ~0x1ffu) + (((addr >> 18) & 0x7f) << 2);
    const uint32_t pd = physRead32(ptrAddr);
    if (!(pd & 2)) accessFault(addr, FSLW_PTB, write, fc, fetch);
    if (!(pd & 8)) physWrite32(ptrAddr, pd | 8);
    wp = wp || (pd & 4);
    // ページ: 8KB なら上位 25 ビット + PGI (5 ビット) x 4、4KB なら上位 24 ビット + PGI (6 ビット) x 4
    const bool p8 = (tc060_ & 0x4000) != 0;
    uint32_t pgAddr = p8 ? (pd & ~0x7fu) + (((addr >> 13) & 0x1f) << 2) : (pd & ~0xffu) + (((addr >> 12) & 0x3f) << 2);
    uint32_t d = physRead32(pgAddr);
    if ((d & 3) == 2) { // 間接
        pgAddr = d & ~3u;
        d = physRead32(pgAddr);
        if ((d & 3) == 2) accessFault(addr, FSLW_IL, write, fc, fetch);
    }
    if ((d & 3) == 0) accessFault(addr, FSLW_PF, write, fc, fetch);
    wp = wp || (d & 4);
    if (!super && (d & 0x80)) accessFault(addr, FSLW_SP, write, fc, fetch);
    if (write && wp) accessFault(addr, FSLW_WP, write, fc, fetch);
    uint32_t nd = d | 8;
    if (write) nd |= 0x10;
    if (nd != d) physWrite32(pgAddr, nd);
    inTableWalk_ = false;
    writable = !wp && (nd & 0x10);
    const uint32_t mask = p8 ? 0x1fffu : 0xfffu;
    return (d & ~mask) | (addr & mask);
}

uint32_t Cpu020::translateSlow(uint32_t addr, int fc, bool write, bool fetch) {
    // ファンクションコード 0 / 3 / 4 / 7 は変換しない (MC68060UM 4.2.5)
    if (fc == 0 || fc == 3 || fc == 4 || fc == 7) return addr;
    const bool super = (fc & 4) != 0;
    const uint32_t tag = addr >> 12;
    AtcEntry& e = atc_[fetch ? 1 : 0][super ? 1 : 0][tag & (ATC_SIZE - 1)];
    if (e.tag == tag && (!write || e.writable)) return (e.ppage << 12) | (addr & 0xfff);
    uint32_t pa = addr;
    bool writable = true;
    bool done = false;
    // TTR (MC68060UM 4.1.3): 上位 8 ビットを、マスクのビットを除いて比べる。S が 00 ならユーザ、01 ならスーパーバイザ
    const uint32_t* tt = fetch ? itt_ : dtt_;
    for (int i = 0; i < 2 && !done; i++) {
        const uint32_t t = tt[i];
        if (!(t & 0x8000)) continue;
        const int sf = (t >> 13) & 3;
        if ((sf == 0 && super) || (sf == 1 && !super)) continue;
        if ((((addr >> 24) ^ (t >> 24)) & ~(t >> 16)) & 0xff) continue;
        if (write && (t & 4)) accessFault(addr, FSLW_WP | FSLW_TTR, write, fc, fetch);
        writable = !(t & 4);
        done = true;
    }
    if (!done) {
        if (!(tc060_ & 0x8000)) {
            // 変換しない。データの書き込みは TC の DWO で保護できる
            if (!fetch && (tc060_ & 0x20)) {
                if (write) accessFault(addr, FSLW_WP, write, fc, fetch);
                writable = false;
            }
        } else {
            pa = tableWalk(addr, super, write, fetch, fc, writable);
        }
    }
    e.tag = tag;
    e.ppage = pa >> 12;
    e.writable = writable;
    return pa;
}

// ---------------------------------------------------------------- 命令

// 未実装整数命令 (MC68060UM 8.2.4): フォーマット $0、PC はこの命令
void Cpu020::unimplementedInteger() {
    undoAddressRegs();
    noTrace_ = true;
    if (onUnimplemented) onUnimplemented(onUnimplementedCtx, pc0_, op_, 61);
    exception(61, 0, pc0_);
}

// 68060 にない命令を、ベクタ vector の例外にせずその場で実行するか (setUnimplementedAssist)
bool Cpu020::assist060(int vector) {
    if (unimplAssist_ == ASSIST_ALWAYS) return true;
    if (unimplAssist_ != ASSIST_ROM) return false;
    // IPL ROM は未定義の例外のベクタの最上位バイトにベクタ番号を入れる (本体の 16MB は繰り返し見えるので、
    // 最上位バイトは無視される)
    // ユーザモードの命令でも、ベクタはスーパーバイザのデータ空間から読む (例外の処理と同じ)
    uint32_t v = memRead32(vbr_ + (uint32_t)vector * 4, FC_SUPER_DATA);
    return (v & 0xffffff) >= 0xfe0000 && ((v >> 24) == 0 || (v >> 24) == (uint32_t)vector);
}

bool Cpu020::unimpl060() {
    if (assist060(61)) return false;
    unimplementedInteger();
    return true;
}

// MOVEP、CHK2 / CMP2、CAS2: 肩代わりするときは 68030 の実装で実行する
void Cpu020::opUnimpInt(uint16_t op) {
    if (unimpl060()) return;
    table030_[op](this, op);
}

// CINV / CPUSH (1111 0100 CC P SS RRR)。キャッシュは持たないので何もしない。SS が 00 なら F ライン
void Cpu020::opCache060(uint16_t op) {
    if (!sflag_) {
        privilege();
        return;
    }
    if (((op >> 3) & 3) == 0) {
        lineF();
        return;
    }
    tick(10);
}

// PFLUSH (1111 0101 000 MM RRR) / PLPAW (1111 0101 1000 1RRR) / PLPAR (1111 0101 1100 1RRR)。
// PTEST (68040) は 68060 にないので F ライン
void Cpu020::opMmu060(uint16_t op) {
    const int kind = (op >> 6) & 3;
    const int sub = (op >> 3) & 7;
    const bool isFlush = kind == 0 && sub < 4;
    const bool isPlpa = (kind == 2 || kind == 3) && sub == 1;
    if (!isFlush && !isPlpa) {
        lineF();
        return;
    }
    if (!sflag_) {
        privilege();
        return;
    }
    tick(10);
    if (isFlush) {
        // 指定したページだけでなく全部消す (消しすぎても結果は変わらない)
        flushTlb();
        return;
    }
    // PLPA: An の論理アドレスを、DFC のファンクションコードで物理アドレスにする (MC68060UM 4.7.3)
    const int reg = op & 7;
    const bool write = kind == 2;
    const int fc = (int)dfc_;
    a_[reg] = translate(a_[reg], fc, write, false);
}

// MOVE16 (M68000PM): 16 バイトの境界にそろえた 16 バイトを写す
void Cpu020::opMove16(uint16_t op) {
    const int reg = op & 7;
    uint32_t src, dst;
    if ((op & 0xfff8) == 0xf620) {
        // MOVE16 (Ax)+,(Ay)+
        const uint16_t w = fetch16();
        if ((w & 0x8fff) != 0x8000) {
            lineF();
            return;
        }
        const int ay = (w >> 12) & 7;
        src = a_[reg];
        dst = a_[ay];
        a_[reg] += 16;
        if (ay != reg) a_[ay] += 16;
    } else if ((op & 0xffe0) == 0xf600) {
        const uint32_t abs = fetch32();
        const uint32_t ay = a_[reg];
        switch ((op >> 3) & 3) {
        case 0: src = ay; dst = abs; a_[reg] += 16; break; // (Ay)+,(xxx).L
        case 1: src = abs; dst = ay; a_[reg] += 16; break; // (xxx).L,(Ay)+
        case 2: src = ay; dst = abs; break;                // (Ay),(xxx).L
        default: src = abs; dst = ay; break;               // (xxx).L,(Ay)
        }
    } else {
        lineF();
        return;
    }
    src &= ~15u;
    dst &= ~15u;
    uint32_t v[4];
    for (int i = 0; i < 4; i++) v[i] = read32(src + 4 * i);
    for (int i = 0; i < 4; i++) write32(dst + 4 * i, v[i]);
}

// LPSTOP #imm (F800 01C0 iiii)。STOP と同じに扱う
void Cpu020::opLpstop(uint16_t) {
    const uint16_t w = fetch16();
    if (w != 0x01c0) {
        lineF();
        return;
    }
    opStop(0x4e72);
}

} // namespace m68k
