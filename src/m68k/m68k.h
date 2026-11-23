// MC68000 CPU コア
//
// Raspberry Pi のベアメタル (Circle) と Mac のホスト版の両方でビルドするため、
// C++ の例外、RTTI、iostream、ヒープ確保をコアの中では使わない。
// 命令の途中でアドレスエラーやバスエラーが起きたときは setjmp/longjmp で命令を打ち切る。
#pragma once

#include <stdint.h>

#ifdef PIX68K_CIRCLE
// Circle (libc なし) は自前の setjmp/longjmp を持っている
#include <circle/setjmp.h>
#else
#include <setjmp.h>
#endif

// POSIX の setjmp はシグナルマスクを保存するためシステムコールが入り遅い。_setjmp を使う
#if defined(__APPLE__) || defined(__unix__)
#define M68K_SETJMP(b) _setjmp(b)
#define M68K_LONGJMP(b, v) _longjmp(b, v)
#else
#define M68K_SETJMP(b) setjmp(b)
#define M68K_LONGJMP(b, v) longjmp(b, v)
#endif

namespace x68k {
class StateIO;
}

namespace m68k {

// ファンクションコード (FC2..FC0)
enum : int {
    FC_USER_DATA = 1,
    FC_USER_PROG = 2,
    FC_SUPER_DATA = 5,
    FC_SUPER_PROG = 6,
    FC_IACK = 7,
};

// CPU から見たバス。アドレスは 24 ビットにマスクして渡す。
// アクセス 1 回につき CPU は 4 サイクル進める。ウェイトを入れたいときは
// Cpu::addCycles() で足す。バスエラーにしたいときは Cpu::busError() を呼ぶ (戻ってこない)。
// 注意:
// - busError() は Cpu::step() / run() / reset() から呼ばれたバスアクセスの中でだけ呼べる
//   (CPU の外からメモリを読むデバッガなどで呼ぶと、戻り済みの関数に longjmp する)
// - busError() は longjmp で Bus の関数を途中で抜ける。ロックやデストラクタのある
//   ローカル変数を持ったまま呼ばないこと
// - run() の実行中に Bus のコールバックから step() / run() / reset() を呼ばないこと
class Bus {
public:
    virtual ~Bus() {}
    virtual uint8_t read8(uint32_t addr, int fc) = 0;
    virtual uint16_t read16(uint32_t addr, int fc) = 0;
    virtual void write8(uint32_t addr, uint8_t value, int fc) = 0;
    virtual void write16(uint32_t addr, uint16_t value, int fc) = 0;
    // TAS の読み出し・変更・書き込みサイクル。既定では read8/write8 を続けて呼ぶ。
    virtual uint8_t tasRead(uint32_t addr, int fc) { return read8(addr, fc); }
    virtual void tasWrite(uint32_t addr, uint8_t value, int fc) { write8(addr, value, fc); }
    // 割り込みアクノリッジ。ベクタ番号を返す。-1 ならオートベクタ、-2 ならスプリアス割り込み (ベクタ 24)。
    // ここで Cpu::busError() を呼んでもスプリアス割り込みになる。
    virtual int interruptAck(int level) { (void)level; return -1; }
    // RESET 命令で RESET 信号が出たとき
    virtual void resetDevices() {}
};

struct Registers {
    uint32_t d[8];
    uint32_t a[7];
    uint32_t usp, ssp;
    uint16_t sr;
    uint32_t pc;          // 次に実行する命令のアドレス
    uint16_t prefetch[2]; // [0]=IRD (pc の語), [1]=IRC (pc+2 の語)
};

class Cpu {
public:
    void state(x68k::StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)
    explicit Cpu(Bus* bus);

    // 電源投入・リセット: ベクタ 0/1 から SSP と PC を読む
    void reset();
    // 1 命令 (または例外処理 1 回) を実行し、使ったサイクル数を返す
    int step();
    // 指定サイクル数以上進むまで実行し、実際に使ったサイクル数を返す
    int64_t run(int64_t cycles);

    // 割り込み要求レベル (0..7)。周辺機器が変化のたびに設定する。
    // レベル 7 (NMI) の立ち上がりは命令の境目でしか見ないので、命令の間に 7→0 と戻すと取りこぼす
    void setIPL(int level) { ipl_ = level & 7; }

    void getRegisters(Registers& r) const;
    void setRegisters(const Registers& r);

    uint64_t cycles() const { return cycles_; }
    void addCycles(int n) { cycles_ += n; }
    // サイクル数を置き換える (Machine が 68000 の速さを変えて動かすとき、走ったサイクル数を X68000 の時間に直すのに使う)
    void setCycles(uint64_t c) { cycles_ = c; }
    bool halted() const { return halted_; }
    bool stopped() const { return stopped_; }
    // STOP で止まっていて、今の割り込み要求では起きない (次の step() は 4 サイクル待つだけ) か。
    // execOne() の割り込みの受け付けと同じ条件: レベルがマスクより高いか、レベル 7 の立ち上がり (NMI) があれば起きる
    bool waitingInStop() const {
        return stopped_ && !halted_ && !(ipl_ > imask_ || (ipl_ == 7 && (nmiPending_ || lastIpl_ != 7)));
    }

    [[noreturn]] void busError(uint32_t addr, bool write, int fc);

    // 速く読み書きするためのメモリの表。24 ビットの空間を 64KB のページに分け、ページごとに
    // ホストのメモリ (ページの先頭のアドレスのバイト。68000 と同じく上位バイトが先) を指しておくと、
    // CPU はそのページを Bus を呼ばずに直接読み書きする。nullptr のページ (既定) は Bus を呼ぶ。
    // 読み書きしても副作用がなく、バスエラーにならないメモリ (RAM、ROM の読み出し) だけに使う。
    // supervisor: スーパーバイザ (FC 5, 6) とユーザ (FC 1, 2) で別の表を持つ
    // 表を変えるのは Bus のコールバックの中 (命令の途中) でもよい
    static constexpr int FAST_PAGES = 256;
    void setFastPage(bool supervisor, int page, const uint8_t* read, uint8_t* write);
    void clearFastPages();

    // 以下は命令実装から使う内部状態。テストのため public にしている。
    uint32_t d_[8];
    uint32_t a_[8]; // a_[7] は現在の SP
    uint32_t inactiveSp_; // S=1 なら USP、S=0 なら SSP を保持
    uint32_t pc_;
    uint16_t ird_, irc_;
    // SR を分解して持つ
    uint32_t flagX_, flagN_, flagZ_, flagV_, flagC_; // 各 0 or 1 (flagZ_ は 1=ゼロ)
    bool sflag_, tflag_;
    int imask_;

    // 命令の実装を呼ぶ関数。メンバ関数へのポインタより呼び出しが軽く、表も半分の大きさになる
    using Handler = void (*)(Cpu*, uint16_t);
    template <void (Cpu::*M)(uint16_t)> static void thunk(Cpu* c, uint16_t op) { (c->*M)(op); }

public: // 命令実装・デコーダ・テストから使う
    Bus* bus_;
    uint64_t cycles_ = 0;
    int ipl_ = 0;
    int lastIpl_ = 0;
    bool nmiPending_ = false;
    bool halted_ = false;
    bool stopped_ = false;
    bool inGroup0_ = false;
    static constexpr uint32_t NO_FAULT_PC = 0xffffffff;
    uint32_t faultPc_ = NO_FAULT_PC; // グループ 0 例外で積む PC (既定は pc_)
    bool faultNotInstr_ = false;
    bool noTrace_ = false;
    bool inIack_ = false; // 割り込みアクノリッジ中 (ここでのバスエラーはスプリアス割り込み)
    jmp_buf iackAbort_;
    uint16_t curOp_ = 0; // 実行中の命令のオペコード (グループ 0 例外のフレームに積む)
    bool traceAtStart_ = false;
    jmp_buf abort_;

    static Handler table_[65536];
    static bool tableReady_;
    static void buildTable();
    static Handler decode(uint16_t op);
    void execOne();
    void runUntil(uint64_t end);

    // ---- SR ----
public:
    uint16_t getSR() const;
    void setSR(uint16_t sr);
    void setCCR(uint8_t ccr);
public: // 命令実装・デコーダ・テストから使う
    void setSupervisor(bool s);
    int fcData() const { return sflag_ ? FC_SUPER_DATA : FC_USER_DATA; }
    int fcProg() const { return sflag_ ? FC_SUPER_PROG : FC_USER_PROG; }

    // ---- バスアクセス ----
    void idle(int n) { cycles_ += n; }
    const uint8_t* fastRead_[2][FAST_PAGES]; // [0] ユーザ、[1] スーパーバイザ
    uint8_t* fastWrite_[2][FAST_PAGES];
    uint16_t readWordRaw(uint32_t addr, int fc);
    void writeWordRaw(uint32_t addr, uint16_t v, int fc);
    uint8_t readByteRaw(uint32_t addr, int fc);
    void writeByteRaw(uint32_t addr, uint8_t v, int fc);
    template <int S> uint32_t read(uint32_t addr, bool prog = false);
    template <int S> void write(uint32_t addr, uint32_t v);
    template <int S> void writeLongLowFirst(uint32_t addr, uint32_t v);
    uint16_t fetch(uint32_t addr);
    uint16_t readExt();
    uint32_t readExtLong();
    void prefetch();
    void refetch();
    void jump(uint32_t addr, int gap = 0);
    void push16(uint16_t v);
    void push32(uint32_t v);
    uint32_t pop32();
    uint32_t readPredecLong(int reg);

    // ---- 例外 ----
    void exception(int vector, uint32_t pushPc, int idleBefore);
    [[noreturn]] void addressError(uint32_t addr, bool write, bool prog);
    [[noreturn]] void group0(int vector, uint32_t addr, bool write, int fc);
    void interrupt(int level);
    void privilegeViolation();
    void illegal(uint16_t op);

    // ---- 実効アドレス ----
    uint32_t indexValue(uint16_t ext);
    template <int S> uint32_t ea(int mode, int reg, int idxIdle = 2, int pdIdle = 2);
    uint32_t eaControlJump(int mode, int reg);
    template <int S> uint32_t readOperand(int mode, int reg, uint32_t& addr);
    template <int S> void writeOperand(int mode, int reg, uint32_t addr, uint32_t v);
    template <int S> uint32_t readImm();
    template <int S> void setD(int r, uint32_t v);
    bool cond(int cc) const;
    template <int S> void setNZ(uint32_t v);

    // ---- 演算 ----
    template <int S> uint32_t add(uint32_t s, uint32_t d, uint32_t x, bool addx);
    template <int S> uint32_t sub(uint32_t s, uint32_t d, uint32_t x, bool subx);
    template <int S> void cmp(uint32_t s, uint32_t d);
    template <int S> uint32_t logic(uint32_t v);

    // ---- 命令 ----
    template <int S, int OP> void opArithEaDn(uint16_t op);
    template <int S, int OP> void opArithDnEa(uint16_t op);
    template <int S, bool SUB> void opAddxSubx(uint16_t op);
    template <int S, bool SUB> void opAdda(uint16_t op);
    template <int S> void opCmpa(uint16_t op);
    template <int S> void opCmpm(uint16_t op);
    template <int S, int OP> void opImm(uint16_t op);
    template <int OP> void opImmSr(uint16_t op);
    template <int S> void opAddq(uint16_t op);
    template <int S> void opSubq(uint16_t op);
    template <int S> void opMove(uint16_t op);
    template <int S> void opMovea(uint16_t op);
    void opMoveq(uint16_t op);
    template <int S, int OP> void opUnary(uint16_t op);
    template <int S> void opTst(uint16_t op);
    void opTas(uint16_t op);
    void opScc(uint16_t op);
    void opDbcc(uint16_t op);
    void opBcc(uint16_t op);
    void opLea(uint16_t op);
    void opPea(uint16_t op);
    void opJmp(uint16_t op);
    void opJsr(uint16_t op);
    void opRts(uint16_t op);
    void opRtr(uint16_t op);
    void opRte(uint16_t op);
    void opTrap(uint16_t op);
    void opTrapv(uint16_t op);
    void opLink(uint16_t op);
    void opUnlk(uint16_t op);
    void opMoveUsp(uint16_t op);
    void opNop(uint16_t op);
    void opReset(uint16_t op);
    void opStop(uint16_t op);
    void opIllegal(uint16_t op);
    void opLineA(uint16_t op);
    void opLineF(uint16_t op);
    void opSwap(uint16_t op);
    template <int S> void opExt(uint16_t op);
    void opExg(uint16_t op);
    void opMoveFromSr(uint16_t op);
    void opMoveToCcr(uint16_t op);
    void opMoveToSr(uint16_t op);
    template <int S> void opMovemToMem(uint16_t op);
    template <int S> void opMovemToReg(uint16_t op);
    template <int S> void opMovep(uint16_t op);
    template <int OP> void opBitDyn(uint16_t op);
    template <int OP> void opBitImm(uint16_t op);
    template <int S, int TYPE, bool LEFT> void opShiftReg(uint16_t op);
    template <int TYPE, bool LEFT> void opShiftMem(uint16_t op);
    template <int S, int TYPE, bool LEFT> uint32_t shift(uint32_t v, int count);
    template <bool SIGNED> void opMul(uint16_t op);
    template <bool SIGNED> void opDiv(uint16_t op);
    void opChk(uint16_t op);
    template <bool SUB> void opBcd(uint16_t op);
    void opNbcd(uint16_t op);
    uint8_t abcd(uint8_t s, uint8_t d);
    uint8_t sbcd(uint8_t s, uint8_t d);
};

} // namespace m68k
