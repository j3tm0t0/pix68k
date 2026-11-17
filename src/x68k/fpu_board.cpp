// 数値演算プロセッサボード (CZ-6BP1 相当)。fpu_board.h を参照
#include "x68k/fpu_board.h"

#include <string.h>

namespace x68k {

using m68k::FpuCore;
using m68k::fp::Ctx;
using m68k::fp::X;
namespace fp = m68k::fp;

namespace {

// 応答プリミティブ (MC68881UM 表 7-7)
enum : uint16_t {
    R_DONE = 0x0802,       // ヌル: 待機 (CA = 0、PF = 1)
    R_RELEASE = 0x0900,    // ヌル: CPU を放す (IA = 1)
    R_WAIT = 0x8900,       // ヌル: もう一度読め (CA = 1、IA = 1)
    R_COND = 0x0800,       // ヌル: 条件の結果 (TF)
    R_ILLEGAL = 0x5c0b,    // 命令の前の例外: F ライン (定義されていない命令。PC = 1、MC68881UM 7.4.2.5)
    R_PRE_EXC = 0x1c00,    // 命令の前の例外 | ベクタ番号 ($1C31-$1C36)
    R_BSUN = 0x5c30,       // 命令の前の例外: BSUN (PC = 1)
    R_MID_EXC = 0x1d00,    // 命令の途中の例外 | ベクタ番号 ($1D31-$1D36)
    R_PROTOCOL = 0x1d0d,   // 命令の途中の例外: コプロセッサのプロトコル違反
    R_MAINREG = 0x8c00,    // メインプロセッサのレジスタ (Dn) を送れ | n
    R_MOVEM_IN = 0x810c,   // 複数のレジスタをメモリから FPCP へ
    R_MOVEM_OUT = 0xa10c,  // 複数のレジスタを FPCP からメモリへ
};

// FSAVE / FRESTORE のフォーマット語 (MC68881UM 表 6-4。版の番号 $1F と大きさは Inside X68000 の値)
enum : uint16_t { F_NULL = 0x0018, F_ILLEGAL = 0x0218, F_IDLE = 0x1f18, F_BUSY = 0x1fb4 };
const int kIdleLongs = 6, kBusyLongs = 45;
// IDLE のフレームの BIU フラグ (保留なし。fpu.cpp の kBiuIdle と同じ)。bit27 が 0 なら例外の保留あり
const uint32_t kBiuIdle = 0x7c00ffffu, kBiuNoPending = 0x08000000u;

// 実効アドレスを評価してデータを送れ (DR = 0 は CPU から FPCP へ、1 は FPCP から CPU へ)。CA = 1
uint16_t evaluate(int bytes, bool toCpu) {
    if (bytes <= 4) return (uint16_t)((toCpu ? 0xb100 : 0x9500) | bytes);
    return (uint16_t)((toCpu ? 0xb200 : 0x9600) | bytes);
}

void toBytes(const uint32_t* w, int bytes, uint8_t* out) {
    for (int i = 0; i < bytes; i++) out[i] = (uint8_t)(w[i >> 2] >> (24 - 8 * (i & 3)));
}

void toWords(const uint8_t* in, int bytes, uint32_t* w, int words) {
    for (int i = 0; i < words; i++) w[i] = 0;
    for (int i = 0; i < bytes; i++) w[i >> 2] |= (uint32_t)in[i] << (24 - 8 * (i & 3));
}

int bitCount(unsigned v) {
    int n = 0;
    for (; v; v &= v - 1) n++;
    return n;
}

} // namespace

void FpuBoard::reset() {
    for (X& r : core_.fp_) r = FpuCore::defaultNan();
    core_.fpcr_ = core_.fpsr_ = core_.fpiar_ = 0;
    nullState_ = true;
    excPending_ = false;
    excReported_ = false;
    memset(pendFrame_, 0, sizeof pendFrame_);
    abort();
    restoreEcho_ = 0xffff;
    fpiarHi_ = 0;
}

// 制御 CIR への書き込み (68881 はアボートと例外の受け付けを区別しない): 手順をやめ、保留している例外を消す
void FpuBoard::abort() {
    phase_ = P_IDLE;
    len_ = pos_ = 0;
    excPending_ = false;
    setResponse(R_DONE);
}

// ---------------------------------------------------------------- CIR の読み書き

uint16_t FpuBoard::read16(uint32_t off) {
    switch (off & 0x1e) {
    case 0x00: { // 応答
        const uint16_t v = resp_;
        if (next_) setResponse(next_, next2_);
        return v;
    }
    case 0x04: return save();
    case 0x06: return restoreEcho_;
    case 0x10:
    case 0x12: { // オペランド
        uint8_t hi, lo = 0xff;
        if (!operandOut(hi)) return 0xffff;
        if (phase_ == P_OUT || phase_ == P_SAVE) operandOut(lo); // 1 バイトだけ残っていたとき (バイトのオペランド) は $FF
        return (uint16_t)(hi << 8 | lo);
    }
    case 0x14: // レジスタ選択
        if (phase_ != P_REGSEL) {
            violation();
            return 0xffff;
        }
        startMovem();
        return (uint16_t)(list_ << 8); // 下位バイトは 0 (MC68881UM 7.4.2.4)
    default: return 0xffff; // 書き込み専用と予約
    }
}

void FpuBoard::write16(uint32_t off, uint16_t v) {
    switch (off & 0x1e) {
    case 0x02: abort(); return;
    case 0x06: restoreFormat(v); return;
    case 0x0a: command(v); return;
    case 0x0e: condition(v); return;
    case 0x10:
    case 0x12:
        operandIn((uint8_t)(v >> 8));
        if (phase_ == P_IN || phase_ == P_MAINREG || phase_ == P_RESTORE) operandIn((uint8_t)v);
        return;
    case 0x18: fpiarHi_ = v; return;
    case 0x1a: core_.fpiar_ = fpiarHi_ << 16 | v; return;
    default: return; // 読み出し専用、命令語、オペランドアドレス、予約
    }
}

uint8_t FpuBoard::read8(uint32_t off) {
    off &= 0x1f;
    if ((off & 0x1c) == 0x10) { // オペランド
        uint8_t b;
        return operandOut(b) ? b : 0xff;
    }
    if (off & 1) { // 下位バイト: 状態は変えずに今の値を見せる
        switch (off & 0x1e) {
        case 0x00: return (uint8_t)resp_;
        case 0x06: return (uint8_t)restoreEcho_;
        case 0x14: return 0x00; // レジスタ選択の下位バイト
        default: return 0xff;
        }
    }
    return (uint8_t)(read16(off) >> 8);
}

void FpuBoard::write8(uint32_t off, uint8_t v) {
    off &= 0x1f;
    if ((off & 0x1c) == 0x10) {
        operandIn(v);
        return;
    }
    write16(off, (uint16_t)(v << 8 | v));
}

// ---------------------------------------------------------------- オペランドの転送

void FpuBoard::startIn(InKind kind, int bytes) {
    phase_ = P_IN;
    inKind_ = kind;
    len_ = bytes;
    pos_ = 0;
}

void FpuBoard::startOut(const uint32_t* w, int bytes) {
    toBytes(w, bytes, buf_);
    phase_ = P_OUT;
    len_ = bytes;
    pos_ = 0;
}

// 待っていないときのオペランドとレジスタ選択の読み書き: プロトコル違反 (命令の途中の例外)
void FpuBoard::violation() {
    phase_ = P_IDLE;
    len_ = pos_ = 0;
    setResponse(R_PROTOCOL);
}

void FpuBoard::operandIn(uint8_t b) {
    if (phase_ != P_IN && phase_ != P_MAINREG && phase_ != P_RESTORE) {
        violation();
        return;
    }
    buf_[pos_++] = b;
    if (pos_ < len_) return;
    if (phase_ == P_MAINREG) finishMainReg();
    else if (phase_ == P_IN) finishIn();
    else { // P_RESTORE
        uint32_t w[kBusyLongs];
        toWords(buf_, len_, w, len_ / 4);
        phase_ = P_IDLE;
        nullState_ = false;
        if (len_ == kIdleLongs * 4) {
            memcpy(pendFrame_, w, sizeof pendFrame_);
            // BIU フラグの bit27 が 0 なら例外が保留されている。次のコマンドか条件で知らせる (MC68881UM 6.4.2.2)
            excPending_ = !(w[kIdleLongs - 1] & kBiuNoPending);
            excReported_ = false;
            setResponse(R_DONE);
        } else {
            unpackBusy(w);
        }
    }
}

bool FpuBoard::operandOut(uint8_t& b) {
    if (phase_ != P_OUT && phase_ != P_SAVE) {
        violation();
        return false;
    }
    b = buf_[pos_++];
    if (pos_ < len_) return true;
    phase_ = P_IDLE;
    finishOut();
    return true;
}

// ---------------------------------------------------------------- コマンド (cpGEN の 2 語目)

void FpuBoard::command(uint16_t w) {
    // 保留している例外は、次の演算か FMOVE FPn,<ea> を始めるときに知らせる (命令は実行しない)。制御レジスタの
    // 転送 (opclass 100、101) と FMOVEM (110、111) は、知らせずに実行する (MC68881UM 6.4.2.2、7.5.4.1)。
    // 例外ハンドラが FMOVEM で FPSR や FPn を保存できるようにするため
    if ((w >> 13) < 4) {
        if (takePending()) return;
        excReported_ = false; // 新しい演算を始める: 知らせた例外のフレームは捨てる
    }
    phase_ = P_IDLE;
    cmd_ = w;
    const int cls = w >> 13, m = (w >> 10) & 7, n = (w >> 7) & 7, opmode = w & 0x7f;
    switch (cls) {
    case 0: // FPm,FPn (MC68881UM 7.5.1.1)
        if (opmode >= 0x40) break;
        nullState_ = false;
        arith(&core_.fp_[m], nullptr);
        setResponse(R_RELEASE | pcBit(), R_DONE);
        return;
    case 2: // <ea>,FPn (7.5.1.2)。FMOVECR はレジスタどうしと同じ手順
        if (opmode >= 0x40) break;
        nullState_ = false;
        if (m == 7) {
            core_.fpsr_ &= 0x00ff00ff;
            Ctx c = core_.ctx();
            core_.fmovecr(opmode, n, c);
            core_.accrue();
            if (core_.enabledExceptions()) raisePending(core_.fp_[n]);
            setResponse(R_RELEASE | pcBit(), R_DONE);
            return;
        }
        startIn(IN_SOURCE, FpuCore::operandSize(m));
        setResponse((uint16_t)(evaluate(len_, false) | pcBit()), R_WAIT);
        return;
    case 3: // FMOVE FPn,<ea> (7.5.1.3)。動的な k ファクタなら、先に Dn を送らせる
        nullState_ = false;
        if (m == 7) {
            phase_ = P_MAINREG;
            len_ = 4;
            pos_ = 0;
            setResponse((uint16_t)(R_MAINREG | pcBit() | ((w >> 4) & 7)), R_WAIT);
            return;
        }
        fmoveOut(w & 0x7f);
        // 変換している間の $8900 (PC を求めるなら $C900)、次に評価と転送のプリミティブ、それを読んだら $8900
        setResponse(R_WAIT | pcBit(), evaluate(len_, true), R_WAIT);
        return;
    case 4:   // <ea>,FPCR/FPSR/FPIAR (7.5.1.4)
    case 5: { // FPCR/FPSR/FPIAR,<ea>
        int list = (w >> 10) & 7;
        if (list == 0) list = 1; // fpu.cpp と同じく、0 は FPIAR として扱う
        list_ = (uint8_t)list;
        const int count = bitCount((unsigned)list);
        // 1 個のとき: FPIAR は $9704 / $B304 (実効アドレスに An を使える)、FPCR と FPSR は $9504 / $B104
        uint16_t prim = evaluate(4 * count, cls == 5);
        if (count == 1 && list == 1) prim = (uint16_t)(prim | 0x0200);
        nullState_ = false;
        if (cls == 4) {
            startIn(IN_CONTROL, 4 * count);
        } else {
            const uint32_t* regs[3] = {&core_.fpcr_, &core_.fpsr_, &core_.fpiar_};
            uint32_t out[3];
            int k = 0;
            for (int i = 0; i < 3; i++)
                if (list & (4 >> i)) out[k++] = *regs[i];
            startOut(out, 4 * count);
            outDone_ = R_DONE;
        }
        setResponse(prim, R_WAIT); // 制御レジスタの転送は例外を起こさないので PC は求めない
        return;
    }
    case 6:   // FMOVEM <ea>,FPn のリスト (7.5.1.5)
    case 7: { // FMOVEM FPn のリスト,<ea>
        nullState_ = false;
        if (w & 0x0800) { // 動的なリスト: Dn を送らせる
            phase_ = P_MAINREG;
            len_ = 4;
            pos_ = 0;
            setResponse((uint16_t)(R_MAINREG | ((w >> 4) & 7)), R_WAIT);
            return;
        }
        list_ = (uint8_t)w;
        phase_ = P_REGSEL;
        setResponse(cls == 6 ? R_MOVEM_IN : R_MOVEM_OUT, R_WAIT);
        return;
    }
    default: break;
    }
    // 定義されていない命令 (opclass 001、opmode $40 以上)
    setResponse(R_ILLEGAL);
}

// 動的なリストか k ファクタの Dn を受け取った
void FpuBoard::finishMainReg() {
    const uint32_t v = (uint32_t)buf_[0] << 24 | (uint32_t)buf_[1] << 16 | (uint32_t)buf_[2] << 8 | buf_[3];
    phase_ = P_IDLE;
    if ((cmd_ >> 13) == 3) { // FMOVE.P FPn,<ea>{Dn}
        fmoveOut((int)(v & 0x7f));
        setResponse(evaluate(len_, true), R_WAIT);
        return;
    }
    list_ = (uint8_t)v;
    phase_ = P_REGSEL;
    setResponse((cmd_ >> 13) == 6 ? R_MOVEM_IN : R_MOVEM_OUT, R_WAIT);
}

// レジスタ選択を読んだ: FMOVEM のデータを送る (リストが空なら終わり)
void FpuBoard::startMovem() {
    const int count = bitCount(list_);
    if (count == 0) {
        phase_ = P_IDLE;
        setResponse(R_DONE);
        return;
    }
    // リストの bit7 から順に送る。プレデクリメント (cmd_ の bit12 が 0) では bit7 が FP7、ほかは bit7 が FP0
    // (fpu.cpp の movemData と同じ並び)
    const bool predec = !(cmd_ & 0x1000);
    if ((cmd_ >> 13) == 6) {
        startIn(IN_MOVEM, 12 * count);
    } else {
        uint32_t w[24];
        int k = 0;
        for (int b = 7; b >= 0; b--) {
            if (!(list_ & (1 << b))) continue;
            const X& x = core_.fp_[predec ? b : 7 - b];
            w[k++] = (uint32_t)x.se << 16;
            w[k++] = (uint32_t)(x.m >> 32);
            w[k++] = (uint32_t)x.m;
        }
        startOut(w, 12 * count);
        outDone_ = R_DONE;
    }
    setResponse(R_WAIT);
}

void FpuBoard::finishIn() {
    phase_ = P_IDLE;
    uint32_t w[24] = {}; // 受け取った長さが用途より短くても、決まった値を読む
    toWords(buf_, len_, w, (len_ + 3) / 4);
    switch (inKind_) {
    case IN_SOURCE:
        arith(nullptr, w);
        setResponse(R_RELEASE, R_DONE);
        return;
    case IN_CONTROL: {
        uint32_t* regs[3] = {&core_.fpcr_, &core_.fpsr_, &core_.fpiar_};
        const uint32_t masks[3] = {0x0000fff0u, 0x0ffffff8u, 0xffffffffu};
        int k = 0;
        for (int i = 0; i < 3; i++)
            if (list_ & (4 >> i)) *regs[i] = w[k++] & masks[i];
        setResponse(R_DONE);
        return;
    }
    case IN_MOVEM: {
        const bool predec = !(cmd_ & 0x1000);
        int k = 0;
        for (int b = 7; b >= 0; b--) {
            if (!(list_ & (1 << b))) continue;
            // そのまま入れる (シグナリング NaN や非正規化数も保つ。fpu.cpp と同じ)
            core_.fp_[predec ? b : 7 - b] = X{(uint16_t)(w[k] >> 16), (uint64_t)w[k + 1] << 32 | w[k + 2]};
            k += 3;
        }
        setResponse(R_DONE);
        return;
    }
    }
}

void FpuBoard::finishOut() { setResponse(outDone_); }

// ---------------------------------------------------------------- 計算

// FPm,FPn と <ea>,FPn の演算 (fpu.cpp の Fpu::general の 68882 の部分と同じ)
void FpuBoard::arith(const X* srcReg, const uint32_t* srcMem) {
    const int m = (cmd_ >> 10) & 7, n = (cmd_ >> 7) & 7, opmode = cmd_ & 0x7f;
    Ctx c = core_.ctx();
    // 例外で結果を捨てるときに戻す条件コード (消す前に取っておく)
    const uint32_t keepCc = core_.fpsr_ & 0xff000000u;
    core_.fpsr_ &= 0x00ff00ff;
    const X src = srcReg ? *srcReg : FpuCore::loadOperand(m, srcMem, c);
    // SNAN、OPERR、DZ の例外が許可されていれば、レジスタは変えない (MC68881UM 6.1.2、6.1.3、6.1.6)
    const X keepDst = core_.fp_[n], keepCos = core_.fp_[opmode & 7];
    uint32_t cc = 0;
    bool setCc;
    if (!core_.arith(opmode, src, n, c, cc, setCc)) {
        core_.fp_[n] = keepDst;
        core_.fpsr_ = (core_.fpsr_ & 0x00ffffff) | keepCc;
        return;
    }
    core_.fpsr_ = (core_.fpsr_ & 0x00ffffff) | cc | c.exc;
    if (core_.fpcr_ & c.exc & (fp::EXC_SNAN | fp::EXC_OPERR | fp::EXC_DZ)) {
        core_.fp_[n] = keepDst;
        if (opmode >= 0x30 && opmode <= 0x37) core_.fp_[opmode & 7] = keepCos;
        core_.fpsr_ = (core_.fpsr_ & 0x00ffffff) | keepCc;
    }
    core_.accrue();
    if (core_.enabledExceptions()) raisePending(src);
}

// FMOVE FPn,<ea>: 変換して送る用意をする。許可された例外が起きたら、送り終えたときに命令の途中の例外を返す
void FpuBoard::fmoveOut(int k) {
    const int m = (cmd_ >> 10) & 7, n = (cmd_ >> 7) & 7;
    core_.fpsr_ &= 0xffff00ff;
    Ctx c = core_.ctx();
    uint32_t w[3];
    FpuCore::storeOperand(core_.fp_[n], m, k, c, w);
    core_.fpsr_ |= c.exc;
    core_.accrue();
    const uint32_t en = core_.enabledExceptions();
    startOut(w, FpuCore::operandSize(m));
    outDone_ = en ? (uint16_t)(R_MID_EXC | FpuCore::excVector(en)) : (uint16_t)R_DONE;
}

// 許可された例外が起きた: 保留し、FSAVE で見えるフレームを作る
void FpuBoard::raisePending(const X& operand) {
    excPending_ = true;
    pendFrame_[0] = (uint32_t)cmd_ << 16;
    pendFrame_[1] = (uint32_t)operand.se << 16;
    pendFrame_[2] = (uint32_t)(operand.m >> 32);
    pendFrame_[3] = (uint32_t)operand.m;
    pendFrame_[4] = 0;
    pendFrame_[5] = kBiuIdle & ~kBiuNoPending;
}

// 保留している例外があれば、命令の前の例外 ($1C3x) を返す。例外の種類は、今の FPCR の ENABLE と FPSR の EXC
// から決める (FRESTORE で戻したときは、そのあとに戻した FPCR と FPSR の値)。どちらも立っていなければ知らせない
bool FpuBoard::takePending() {
    if (!excPending_) return false;
    excPending_ = false;
    const uint32_t en = core_.enabledExceptions();
    if (!en) return false;
    excReported_ = true;
    phase_ = P_IDLE;
    setResponse((uint16_t)(R_PRE_EXC | FpuCore::excVector(en)));
    return true;
}

// ---------------------------------------------------------------- 条件 (FBcc、FScc、FDBcc、FTRAPcc。MC68881UM 7.5.2)

void FpuBoard::condition(uint16_t v) {
    if (takePending()) return;
    excReported_ = false;
    phase_ = P_IDLE;
    const int cc = v & 0x3f;
    if (cc >= 0x20) {
        setResponse(R_ILLEGAL);
        return;
    }
    nullState_ = false;
    // bit4 の立った条件は、NAN なら BSUN を立てる。BSUN が許可されていれば命令の前の例外
    if ((cc & 0x10) && (core_.fpsr_ & FpuCore::CC_NAN)) {
        core_.fpsr_ |= fp::EXC_BSUN;
        core_.accrue();
        if (core_.fpcr_ & fp::EXC_BSUN) {
            setResponse(R_BSUN);
            return;
        }
    }
    // TF = 1 が成立 (MC68881UM 表 7-3)。Inside X68000 は逆に書いているが、Motorola の資料に従う
    setResponse((uint16_t)(R_COND | (core_.cond881(cc) ? 1 : 0)), R_DONE);
}

// ---------------------------------------------------------------- FSAVE / FRESTORE (MC68881UM 6.4)

uint16_t FpuBoard::save() {
    if (phase_ == P_SAVE || phase_ == P_RESTORE) return F_ILLEGAL; // セーブかリストアの途中 (入れ子)
    if (nullState_) return F_NULL;
    uint32_t w[kBusyLongs];
    int longs;
    uint16_t format;
    if (phase_ == P_IDLE) {
        format = F_IDLE;
        longs = kIdleLongs;
        if (excPending_ || excReported_) {
            memcpy(w, pendFrame_, sizeof pendFrame_);
        } else {
            memset(w, 0, sizeof w);
            w[kIdleLongs - 1] = kBiuIdle;
        }
    } else {
        format = F_BUSY;
        longs = kBusyLongs;
        packBusy(w);
    }
    // フレームは高いアドレスから送る (CPU はフレームの最後から書いていく)
    uint32_t rev[kBusyLongs];
    for (int i = 0; i < longs; i++) rev[i] = w[longs - 1 - i];
    toBytes(rev, longs * 4, buf_);
    phase_ = P_SAVE;
    len_ = longs * 4;
    pos_ = 0;
    excPending_ = false; // 保留はフレームに移る
    excReported_ = false;
    outDone_ = R_DONE;
    setResponse(R_DONE);
    return format;
}

void FpuBoard::restoreFormat(uint16_t f) {
    phase_ = P_IDLE;
    excPending_ = false;
    excReported_ = false;
    setResponse(R_DONE);
    restoreEcho_ = f;
    if ((f >> 8) == 0x00) { // NULL: リセットする
        for (X& r : core_.fp_) r = FpuCore::defaultNan();
        core_.fpcr_ = core_.fpsr_ = core_.fpiar_ = 0;
        nullState_ = true;
        return;
    }
    if (f != F_IDLE && f != F_BUSY) {
        restoreEcho_ = F_ILLEGAL;
        return;
    }
    // フレームの残りは低いアドレスから受け取る
    phase_ = P_RESTORE;
    len_ = (f == F_IDLE ? kIdleLongs : kBusyLongs) * 4;
    pos_ = 0;
}

// BUSY のフレーム: 手順の途中の状態をそのまま詰める (実機の中身とは違う)
void FpuBoard::packBusy(uint32_t* w) const {
    memset(w, 0, kBusyLongs * 4);
    w[0] = (uint32_t)cmd_ << 16 | (uint32_t)phase_ << 8 | inKind_;
    w[1] = (uint32_t)list_ << 16 | outDone_;
    w[2] = (uint32_t)resp_ << 16 | next_;
    w[3] = (uint32_t)len_ << 16 | (uint32_t)pos_;
    toWords(buf_, 96, w + 4, 24);
    w[28] = next2_;
    // 保留している例外 (制御レジスタの転送と FMOVEM は保留したまま始められる): w[29]-w[34] に IDLE のフレームの
    // 中身、w[35] の bit0 に知らせたあとか (excReported_)。保留していれば、最後の語 (BIU フラグ) の bit27 を 0 にする
    memcpy(w + 29, pendFrame_, sizeof pendFrame_);
    w[35] = excReported_ ? 1u : 0u;
    w[kBusyLongs - 1] = excPending_ ? (kBiuIdle & ~kBiuNoPending) : kBiuIdle;
}

void FpuBoard::unpackBusy(const uint32_t* w) {
    cmd_ = (uint16_t)(w[0] >> 16);
    phase_ = (uint8_t)(w[0] >> 8);
    inKind_ = (uint8_t)w[0];
    list_ = (uint8_t)(w[1] >> 16);
    outDone_ = (uint16_t)w[1];
    resp_ = (uint16_t)(w[2] >> 16);
    next_ = (uint16_t)w[2];
    len_ = (int)(w[3] >> 16);
    pos_ = (int)(w[3] & 0xffff);
    toBytes(w + 4, 96, buf_);
    next2_ = (uint16_t)w[28];
    memcpy(pendFrame_, w + 29, sizeof pendFrame_);
    excReported_ = (w[35] & 1) != 0;
    excPending_ = !(w[kBusyLongs - 1] & kBiuNoPending);
    // 壊れたフレームで範囲の外を読み書きしないように
    // (受け渡しの途中なら pos_ < len_。buf_[pos_] を読み書きしてから数を比べる)
    const bool transfer = phase_ == P_IN || phase_ == P_OUT || phase_ == P_MAINREG;
    if (phase_ > P_REGSEL || inKind_ > IN_MOVEM || len_ > 96 || pos_ > len_ || (transfer && pos_ >= len_)) {
        phase_ = P_IDLE;
        len_ = pos_ = 0;
        setResponse(R_DONE);
    }
}

} // namespace x68k
