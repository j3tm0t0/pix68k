// MC68030 / MC68060 の CPU コア (68020 以降の命令セット)
//
// 68000 のコア (m68k.h の Cpu) とは別のクラスにしている。68000 はバスサイクルの順序まで合わせているが、
// こちらは命令単位で動かす。68030 のサイクル数は概算 (68000 で同じ命令にかかるサイクル数の 3/5)。68060 のサイクル数は
// MC68060UM 10 章の表から作る (cpu060_cycles.inc)。68000 の経路には手を入れないので、68000 の速さとタイミングは変わらない。
//
// 68060 (MC68060 User's Manual、以下 MC68060UM) では、68060 がハードウェアで実行しない命令を例外にし
// (未実装整数命令はベクタ 61、未実装の浮動小数点命令はベクタ 11 など)、68040 / 68060 形式の MMU でアドレスを
// 変換する (cpu060.inc)。
//
// アドレスは 32 ビット。Bus にも 32 ビットのまま渡す (X68030 の本体は 16MB の空間が繰り返し見える)。
// 68000 と違い、データの奇数番地へのワード・ロングのアクセスはアドレスエラーにならない (命令の読み込みだけ)。
//
// C++ の例外、RTTI、ヒープ確保は使わない。命令の途中のバスエラーなどは setjmp/longjmp で打ち切る。
#pragma once

#include <stdint.h>
#include <string.h>

#include "m68k/m68k.h"

namespace x68k {
class StateIO;
}

// 命令ごとに通る小さな関数と、よく使う命令の処理は、表に入れる関数 (Cpu020::thunk) の中に必ず展開する。
// アドレッシングモードなどを決めて作った thunk の中で、モードによる分岐が消える (下の thunk の説明)。
// 定義は cpu020.cpp にあり、ほかのファイルからは呼べない
#define CPU020_INLINE inline __attribute__((always_inline))

namespace m68k {

class Fpu;

enum class CpuModel : int {
    M68000 = 0, // 68000 のコア (Cpu) を使う
    M68030 = 3,
    M68060 = 6,
};

class Cpu020 {
public:
    void state(x68k::StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)
    void stateTiming060(x68k::StateIO& s); // 68060 のサイクル数の数え方の状態 (state.cpp)
    void stateTlb(x68k::StateIO& s); // 68060 の MMU が有効な間の TLB と ATC (fast page を決めたあとに呼ぶ。state.cpp)
    Cpu020(Bus* bus, CpuModel model);
    ~Cpu020();
    Cpu020(const Cpu020&) = delete;
    Cpu020& operator=(const Cpu020&) = delete;

    CpuModel model() const { return model_; }
    bool is060() const { return model_ == CpuModel::M68060; }
    // 命令の処理と、そこから呼ぶ関数のうち、よく使うものは 2 つに分けてコンパイルする (#170):
    //   M060 = true   68060 専用。68060 では結果が決まっている検査 (tick() の cost_、is060() の model_) を消す。
    //                 68060 の表 (ops060_) の thunk からだけ呼ぶ
    //   M060 = false  両方の機種で使える (検査を実行時に行う)。68030 の表のほか、68060 でも、分けていない命令、FPU、
    //                 68060 にない命令の肩代わり (opUnimpInt() が 68030 の表の処理を呼ぶ) から使う
    // 中では tick(n) を if (!M060) tick(n)、is060() を on060<M060>() と書く。結果とサイクル数はどちらも同じ。
    // For<M060> は、命令の処理の 2 つ目の引数の型。表に入れる関数 (thunk) が M060 を渡すのに使い、中身はない
    template <bool M060> struct For {};
    template <bool M060> bool on060() const { return M060 || is060(); }

    void reset();
    int step();
    int64_t run(int64_t cycles);
    void setIPL(int level) {
        // 変わったときだけ、次の命令の前に割り込みを調べる (runUntil())。周辺機器の同期のたびに同じ値で呼ばれる
        if (ipl_ != (level & 7)) {
            ipl_ = level & 7;
            attn_ = 1;
        }
    }

    // 実行中の run() を、今の命令が終わったところで打ち切る (渡したサイクル数より前に戻る)。命令の途中の周辺機器への
    // 書き込みから呼ぶ。Machine が、タイマの設定が変わったときに、周辺機器を次に進める時刻を決め直すのに使う。
    // run() の外で呼んでも何も起きない (run() の始めに消す)
    void endRun() {
        endRun_ = 1;
        attn_ = 1;
    }

    // 68000 のコアと同じ形のレジスタ (ssp はスーパーバイザのスタックポインタ: M=0 なら ISP、M=1 なら MSP)
    void getRegisters(Registers& r) const;
    void setRegisters(const Registers& r);

    // サイクル数 (Machine のクロック)。内部では 1/SUB サイクル単位で数える。
    // SUB は、setSpeed() の速さの刻みを細かくするために大きくしてある (800 の 16 倍。#131)。既定の速さ (100) では、
    // どのコストも 800 のときのちょうど 16 倍なので、サイクル数は 800 のときと同じになる。
    // 800 のときは、68030 の 68000 の 1 サイクルあたりのコスト (baseCost_) が 480 で、速さは 48000 / cost_ の
    // 飛び飛びの値にしかならなかった (1600% の次が 1655%、3000% の次が 3200%)。12800 では 3200% でも刻みが 0.5% より細かい。
    // 64 ビットの sub_ があふれるまでの時間は 1/16 になる (50MHz で約 333 日)。そうなる前に、x68k::Machine::rebaseTime() が
    // フレームの切れ目で時刻を戻す (上限は x68k/state.h の STATE_CPU020_CYCLES_MAX)
    static constexpr int SUB = 12800;
    uint64_t cycles() const { return sub_ / SUB; }
    void addCycles(int n) { sub_ += (uint64_t)n * SUB; }
    // 1/SUB サイクル単位の今の時刻と、その単位で足すもの (Machine が X68030 と 060turbo の本体のバスの待ちを足す)
    uint64_t subCycles() const { return sub_; }
    void addSubCycles(uint64_t n) { sub_ += n; }
    // 時刻を n サイクル戻す (cycles() 以下。サイクルの端数は変わらない。x68k::Machine::shiftTime())
    void shiftTime(uint64_t n) { sub_ -= n * SUB; }
    // 命令の実行にかかるサイクル数を 100/percent 倍にする (100 で既定の速さ、最大 SPEED_MAX)。
    // 周辺機器の時間はそのままなので、CPU だけが percent/100 倍速くなる (クロックを上げたのと同じ)。
    // 周辺機器を待つサイクル (addCycles) は変えない。
    // 68060 の 100 は、命令のサイクル数が MC68060UM 10 章の表どおり (実機の 68060 と同じ) という意味で、
    // 100 より小さい値 (SPEED060_MIN まで) も渡せる (実機より遅くして、ホストの負荷を下げる)。68030 は 100 が下限
    static constexpr int SPEED_MAX = 3200;
    static constexpr int SPEED060_MIN = 5;
    void setSpeed(int percent);
    int speed() const { return speed_; }
    bool halted() const { return halted_; }
    bool stopped() const { return stopped_; }

    [[noreturn]] void busError(uint32_t addr, bool write, int fc);

    // 直接読み書きするメモリ。X68030 の本体の 16MB (64KB のページ 256 個) は 4GB の空間に繰り返し見えるとし、
    // それとは別に、繰り返さない領域を 1 つ置ける (X68030 のハイメモリ、060turbo のローカルメモリ)
    static constexpr int FAST_PAGES = 256;
    // flush が false なら TLB を消さない (まとめて設定するときは、最後に flushTlb() を呼ぶ)
    void setFastPage(bool supervisor, int page, const uint8_t* read, uint8_t* write, bool flush = true);
    void clearFastPages();
    void setExtMemory(uint32_t start, uint32_t size, uint8_t* mem);
    // 物理アドレスで [start, start + size) のうち、setExtMemory() の領域から外れたところは、何もつながっていない
    // メモリにする: 読むと $FF、書いても変わらない。バスエラーにはならない (060turbo のローカルメモリの、SIMM の
    // ない部分。060turbo.sys は書いた値が読めるかどうかで SIMM の容量を調べる)。size が 0 ならなし
    // Bus に出す読み書き (setFastPage() / setExtMemory() のメモリでないところ。I/O、グラフィック VRAM、テキスト VRAM への
    // 書き込みなど) 1 回 (16 ビット) ごとに、cycles サイクル (Machine のクロック) を足す。0 なら足さない (既定)。
    // setSpeed() の倍率は掛けない (CPU を速くしても、本体のバスの速さは変わらない)
    void setBusWait(int cycles) { busWait_ = (uint64_t)cycles * SUB; }
    // 68060 のクロック (Hz)。キャッシュを切ったときのメモリの読み書きの時間 (MISS060_MAIN / MISS060_LOCAL。50MHz の
    // サイクル数で決めた値) を、時間が同じになるようこのクロックのサイクル数に直す。既定は 50MHz (値はそのまま)
    void setClock060(uint32_t hz) {
        const uint64_t mhz = hz / 1000000 ? hz / 1000000 : 50;
        missMain060_ = (int)(MISS060_MAIN * mhz / 50);
        missLocal060_ = (int)(MISS060_LOCAL * mhz / 50);
        updateCacheCosts();
    }
    void setNoMemory(uint32_t start, uint32_t size) {
        holeStart_ = start;
        holeSize_ = size;
        flushTlb();
    }
    // 68060 の未実装整数命令 (MOVEP など) を、例外にせずその場で実行するか
    //   ASSIST_OFF       いつも例外にする (実機と同じ)
    //   ASSIST_ROM       ベクタ 61 が IPL ROM の中 ($FE0000-$FFFFFF) を指している間だけ実行する。060turbo の ROM の
    //                    代わりに X68030 の IPL ROM を使うためのもの (machine.h)。060turbo.sys が 060SP を組み込めば例外になる
    //   ASSIST_ALWAYS    いつも実行する (060SP を組み込んだのと同じ結果。run_x で使う)
    enum UnimplementedAssist : uint8_t { ASSIST_OFF, ASSIST_ROM, ASSIST_ALWAYS };
    void setUnimplementedAssist(UnimplementedAssist mode) { unimplAssist_ = mode; }

    // 命令のトレース (デバッグ用)。未実装の命令などで例外になったときに呼ばれる
    void (*onUnimplemented)(void* ctx, uint32_t pc, uint16_t op, int vector) = nullptr;
    void* onUnimplementedCtx = nullptr;
    // 68060 のアクセスエラー (デバッグ用)。addr は論理アドレス、fslw は積む FSLW
    void (*onAccessFault)(void* ctx, uint32_t pc, uint32_t addr, uint32_t fslw) = nullptr;

    Fpu* fpu() { return fpu_; }

    // ---- 以下は命令の実装とテストから使う ----
    uint32_t d_[8];
    uint32_t a_[8]; // a_[7] は今のスタックポインタ
    uint32_t usp_ = 0, isp_ = 0, msp_ = 0; // 今使っていないスタックポインタの保存場所
    uint32_t pc_ = 0;  // 次に読む命令の語
    uint32_t pc0_ = 0; // 実行中の命令の先頭
    uint16_t op_ = 0;
    uint32_t flagX_ = 0, flagN_ = 0, flagZ_ = 0, flagV_ = 0, flagC_ = 0; // 0 か 1 (flagZ_ は 1 = ゼロ)
    bool sflag_ = true, mflag_ = false;
    int trace_ = 0; // SR の T1 T0 (bit1 = T1, bit0 = T0)
    int imask_ = 7;

    // 制御レジスタ
    uint32_t vbr_ = 0, sfc_ = 0, dfc_ = 0, cacr_ = 0, caar_ = 0;
    // 68030 の MMU のレジスタ (アドレス変換はしない。XEiJ の説明書 (feature.htm) と同じく X68030 は MC68EC030 として扱う)
    uint32_t tc030_ = 0, tt030_[2] = {0, 0}, mmusr030_ = 0;
    uint64_t crp030_ = 0, srp030_ = 0;
    // 68060 の制御レジスタ
    uint32_t pcr_ = 0, buscr_ = 0, tc060_ = 0, itt_[2] = {0, 0}, dtt_[2] = {0, 0}, urp_ = 0, srp060_ = 0;

    uint16_t getSR() const;
    void setSR(uint16_t sr);
    void setCCR(uint8_t ccr);
    int fcData() const { return sflag_ ? FC_SUPER_DATA : FC_USER_DATA; }
    int fcProg() const { return sflag_ ? FC_SUPER_PROG : FC_USER_PROG; }

    // サイクル数: 68000 で n サイクルかかる分を足す (機種の係数を掛ける)。68060 では係数が 0 で、何も足さない
    // (68060 は下の tick060() と、命令ごとの表 ops060_ で数える)
    // 係数が 0 (68060) なら、sub_ を読み書きしない (足しても変わらない)
    void tick(int n) {
        if (cost_) sub_ += (uint64_t)(n * cost_);
    }
    // 68060 の n サイクル (MC68060UM 10 章の値) を足す。68030 では係数が 0 で、何も足さない
    void tick060(int n) { sub_ += (uint64_t)(n * unit060_); }
    // データの読み書き 1 回分 (16 ビットまで / 32 ビット)。a は読み書きするアドレス (updateCacheCosts())
    // 足す値がどれも 0 (68060 でデータキャッシュが有効) なら、sub_ を読み書きしない (dataTick_。updateCacheCosts())
    void tickData16(uint32_t a) {
        if (dataTick_) sub_ += (uint64_t)dataCost16_[(a >> 28) & 1];
    }
    void tickData32(uint32_t a) {
        if (dataTick_) sub_ += (uint64_t)dataCost32_[(a >> 28) & 1];
    }
    // TLB に当たらなかった読み書き (slowRead / slowWrite) の始めに足す分 (68030 は上と同じ、68060 は 0) と、
    // それがメモリだったときに足す分 (68060 でデータキャッシュが無効のときのバスの時間。68030 は 0)
    void tickSlow16() { sub_ += (uint64_t)slowCost16_; }
    void tickSlow32() { sub_ += (uint64_t)slowCost32_; }
    void tickMiss(uint32_t a) { sub_ += (uint64_t)missCost_[(a >> 28) & 1]; }
    // Bus の読み書きの前に、setBusWait() のサイクル数を足してから bus_ を返す
    Bus* busWaited() {
        sub_ += busWait_;
        return bus_;
    }

    // ---- メモリ ----
    // 今のモードのデータ空間の読み書き (cpu020.cpp の dataRead8() など)
    uint8_t dataRead8(uint32_t a);
    uint16_t dataRead16(uint32_t a);
    uint32_t dataRead32(uint32_t a);
    void dataWrite8(uint32_t a, uint8_t v);
    void dataWrite16(uint32_t a, uint16_t v);
    void dataWrite32(uint32_t a, uint32_t v);
    uint8_t read8(uint32_t a);
    uint16_t read16(uint32_t a);
    uint32_t read32(uint32_t a);
    void write8(uint32_t a, uint32_t v);
    void write16(uint32_t a, uint32_t v);
    void write32(uint32_t a, uint32_t v);
    template <int S> uint32_t readS(uint32_t a);
    template <int S> void writeS(uint32_t a, uint32_t v);
    uint8_t memRead8(uint32_t a, int fc);
    uint16_t memRead16(uint32_t a, int fc);
    uint32_t memRead32(uint32_t a, int fc);
    void memWrite8(uint32_t a, uint8_t v, int fc);
    void memWrite16(uint32_t a, uint16_t v, int fc);
    void memWrite32(uint32_t a, uint32_t v, int fc);
    // M060 が true なら 68060 専用 (ゼロになる tick(4) の検査も省く)。既定は両機種で使える。
    template <bool M060 = false> uint16_t fetch16();
    template <bool M060 = false> uint32_t fetch32();
    void flushTlb();
    // 上の関数の、インライン展開しない版 (FPU など、cpu020.cpp の外から使う)
    uint8_t xread8(uint32_t a);
    uint16_t xread16(uint32_t a);
    uint32_t xread32(uint32_t a);
    void xwrite8(uint32_t a, uint32_t v);
    void xwrite16(uint32_t a, uint32_t v);
    void xwrite32(uint32_t a, uint32_t v);
    uint16_t xfetch16();
    uint32_t xfetch32();

    // ---- 例外 ----
    // format: 0 / 2・3 (+$08 に instrAddr を積む) / 4 (68060。+$08 に instrAddr、+$0C に ea) /
    // 9 (コプロセッサの命令の途中。instrAddr と、+$10 に ea を積む)。pushPc は積む PC
    void exception(int vector, int format, uint32_t pushPc, uint32_t instrAddr = 0, uint32_t ea = 0);
    void illegal();     // 不当命令 (ベクタ 4、PC は命令の先頭)
    void lineA();
    void lineF();
    void privilege();
    void trapFormat2(int vector); // CHK、ゼロ除算、TRAPV、TRAPcc など (PC は次の命令、命令のアドレスを積む)
    void interrupt(int level);
    CPU020_INLINE void jumpTo(uint32_t addr);
    void xjumpTo(uint32_t addr); // jumpTo() の、展開しない版 (FPU から使う)
    [[noreturn]] void addressErrorFetch(uint32_t addr);
    [[noreturn]] void abortInstr();

    // ---- 実効アドレス ----
    // メモリを指すモード。(An)+ / -(An) は An を進める
    template <bool M060 = false> CPU020_INLINE uint32_t eaAddr(int mode, int reg, int size);
    uint32_t xeaAddr(int mode, int reg, int size); // eaAddr() の、展開しない版 (FPU から使う)
    template <bool M060 = false> uint32_t extAddr(uint32_t base, bool pcRel); // 拡張語 (brief / full) で決まるアドレス
    // brief 形式を展開した extAddr()
    template <bool M060 = false> CPU020_INLINE uint32_t indexAddr(uint32_t base, bool pcRel);
    template <int S, bool M060 = false> CPU020_INLINE uint32_t readEa(int mode, int reg);
    template <int S, bool M060 = false> CPU020_INLINE uint32_t readEaAddr(int mode, int reg, uint32_t& addr);
    template <int S> CPU020_INLINE void writeEa(int mode, int reg, uint32_t addr, uint32_t v);
    template <int S> void setD(int r, uint32_t v) {
        if (S == 4) d_[r] = v;
        else if (S == 2) d_[r] = (d_[r] & 0xffff0000u) | (v & 0xffff);
        else d_[r] = (d_[r] & 0xffffff00u) | (v & 0xff);
    }
    CPU020_INLINE bool cond(int cc) const;

    // ---- 演算 ----
    template <int S> CPU020_INLINE uint32_t add(uint32_t s, uint32_t d, uint32_t x, bool addx);
    template <int S> CPU020_INLINE uint32_t sub(uint32_t s, uint32_t d, uint32_t x, bool subx);
    template <int S> CPU020_INLINE void cmp(uint32_t s, uint32_t d);
    template <int S> CPU020_INLINE void setNZ(uint32_t v);
    template <int S> CPU020_INLINE uint32_t logic(uint32_t v);
    uint8_t abcd(uint8_t s, uint8_t d);
    uint8_t sbcd(uint8_t s, uint8_t d);

    using Handler = void (*)(Cpu020*, uint16_t);
    // 68060 で、命令をやり直す例外 (アクセスエラーなど) のために命令の始めに取っておくもの (下の saveUndo() の説明):
    //   U_NONE  何も取らない。CCR を変えたあとに例外になることがなく、An は変える前に logA() で記録する命令
    //   U_CCR   CCR だけ取る。フラグを決めてからメモリに書く命令 (An は logA() で記録する)
    //   U_FULL  A0-A7、CCR、S、M をすべて取る (命令ごとに確かめていないものは、すべてこれ)
    enum : int { U_NONE = 0, U_CCR = 1, U_FULL = 2 };
    // thunk の SPEC: オペコードのうち、表を作るときに決まっているビット。0 でなければ、その thunk を入れるオペコードは
    // どれもこの値を持つので、コンパイラにそう伝える (specAssume())。命令の処理 (opMove() など) は thunk の中に
    // 展開されるので、アドレッシングモードや条件による分岐がコンパイルのときに決まり、命令ごとに選ばなくてよくなる。
    // 命令の処理そのもの (結果とサイクル数) は変わらない
    //   bit 3-0   実効アドレス (bit 5-0) の種類 + 1。種類は 0-6 がモード、7 以上は 7 + レジスタ (モード 7)
    //   bit 7-4   MOVE の書く先 (bit 11-6) の種類 + 1
    //   bit 12-8  条件 (bit 11-8) + 1
    //   bit 14-13 下位バイトの分類 (Bcc の変位): 1 = $00 (16 ビット)、2 = $FF (32 ビット)、3 = そのほか (8 ビット)
    static constexpr unsigned specEa(int kind) { return (unsigned)(kind + 1); }
    static constexpr unsigned specDst(int kind) { return (unsigned)(kind + 1) << 4; }
    static constexpr unsigned specCc(int cc) { return (unsigned)(cc + 1) << 8; }
    static constexpr unsigned specLow(int cls) { return (unsigned)cls << 13; }
    // 伝え方は 2 つある。
    //   - 「このビットがこの値でなければ、ここには来ない」と書く (__builtin_unreachable())。命令は 1 つも増えない
    //   - 決まっているビットをその値に置き換えたオペコードを、命令の処理に渡す (CPU020_SPEC_BY_VALUE)
    // 1 つ目だけで分岐が消えるのは、確かめた中では GCC 16.2 と Apple clang 21 だった。GCC 14.2 と GCC 15.2 は
    // 分岐を消さず、thunk の 1 つ 1 つに、すべてのアドレッシングモードの処理が入る (GCC 15.2 の Pi 5 のベアメタル版は
    // cpu020.o の .text が 1.34MB のはずが 3.55MB になり、カーネルが大きさの上限の手前まで来た。#204)。
    // そこで GCC 16 より前では 2 つ目も使う (Pi 5 の Linux の GCC 14.2 のホスト版で、060turbo 75MHz の起動の
    // サイクル数が 8% 減った)。GCC 16 以降と clang は 1 つ目だけにして、機械語を変えない。実機に入れるカーネル
    // (aarch64-elf-gcc 16.2) に 2 つ目を足すと、レジスタの割り当てが変わり、大きさも少し増える。実機で速さを
    // 比べていないので、足していない
#ifndef CPU020_SPEC_BY_VALUE
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 16
#define CPU020_SPEC_BY_VALUE 1
#else
#define CPU020_SPEC_BY_VALUE 0
#endif
#endif
    static CPU020_INLINE uint16_t specAssume(unsigned spec, uint16_t op) {
        const int ea = (int)(spec & 15) - 1, dst = (int)((spec >> 4) & 15) - 1, cc = (int)((spec >> 8) & 31) - 1;
        const int low = (int)((spec >> 13) & 3);
        // 命令の処理が使うのと同じ式で伝える (MODE、REG、REG9、(op >> 6) & 7、(op >> 8) & 15、op & 0xff)
        if (ea >= 0) {
            if (((op >> 3) & 7) != (ea < 7 ? ea : 7)) __builtin_unreachable();
            if (ea >= 7 && (op & 7) != ea - 7) __builtin_unreachable();
        }
        if (dst >= 0) {
            if (((op >> 6) & 7) != (dst < 7 ? dst : 7)) __builtin_unreachable();
            if (dst >= 7 && ((op >> 9) & 7) != dst - 7) __builtin_unreachable();
        }
        if (cc >= 0 && ((op >> 8) & 15) != cc) __builtin_unreachable();
        if (low == 1 && (op & 0xff) != 0) __builtin_unreachable();
        if (low == 2 && (op & 0xff) != 0xff) __builtin_unreachable();
        if (low == 3 && ((op & 0xff) == 0 || (op & 0xff) == 0xff)) __builtin_unreachable();
#if CPU020_SPEC_BY_VALUE
        // 決まっているビットを、その値に置き換えたオペコードを返す (上の条件から、値は変わらない)
        unsigned mask = 0, bits = 0;
        if (ea >= 0) {
            mask |= 7u << 3, bits |= (unsigned)(ea < 7 ? ea : 7) << 3;
            if (ea >= 7) mask |= 7u, bits |= (unsigned)(ea - 7);
        }
        if (dst >= 0) {
            mask |= 7u << 6, bits |= (unsigned)(dst < 7 ? dst : 7) << 6;
            if (dst >= 7) mask |= 7u << 9, bits |= (unsigned)(dst - 7) << 9;
        }
        if (cc >= 0) mask |= 15u << 8, bits |= (unsigned)cc << 8;
        if (low == 1) mask |= 0xffu;
        if (low == 2) mask |= 0xffu, bits |= 0xffu;
        return (uint16_t)((op & ~mask) | bits);
#else
        return op;
#endif
    }
    // 表 (table030_ / ops060_) に入れる関数。M060 は 68060 の表に入れるもの。
    // 2 つ目は、68060 用に分けてコンパイルする命令の処理 (引数に For<M060> がある) のもので、M060 を処理に渡す
    template <void (Cpu020::*M)(uint16_t), int U = U_FULL, bool M060 = false, unsigned SPEC = 0>
    static void thunk(Cpu020* c, uint16_t op) {
        op = specAssume(SPEC, op);
        if (M060) {
            if (U == U_FULL) c->saveUndo();
            else if (U == U_CCR) c->saveCcrUndo();
        }
        (c->*M)(op);
    }
    template <bool M060, void (Cpu020::*M)(uint16_t, For<M060>), int U = U_FULL, unsigned SPEC = 0>
    static void thunkFor(Cpu020* c, uint16_t op) {
        op = specAssume(SPEC, op);
        if (M060) {
            if (U == U_FULL) c->saveUndo();
            else if (U == U_CCR) c->saveCcrUndo();
        }
        (c->*M)(op, For<M060>{});
    }

    // ---- 命令 ----
    template <int S, int OP, bool M060> CPU020_INLINE void opArithEaDn(uint16_t op, For<M060>);
    template <int S, int OP, bool M060> CPU020_INLINE void opArithDnEa(uint16_t op, For<M060>);
    template <int S, bool SUB, bool M060> void opAddxSubx(uint16_t op, For<M060>);
    template <int S, bool SUB, bool M060> CPU020_INLINE void opAdda(uint16_t op, For<M060>);
    template <int S, bool M060> CPU020_INLINE void opCmpa(uint16_t op, For<M060>);
    template <int S, bool M060> CPU020_INLINE void opCmpm(uint16_t op, For<M060>);
    template <int S, int OP, bool M060> CPU020_INLINE void opImm(uint16_t op, For<M060>);
    template <int OP> void opImmSr(uint16_t op);
    template <int S, bool SUB, bool M060> CPU020_INLINE void opAddqSubq(uint16_t op, For<M060>);
    template <int S, bool M060> CPU020_INLINE void opMove(uint16_t op, For<M060>);
    template <int S, bool M060> CPU020_INLINE void opMovea(uint16_t op, For<M060>);
    CPU020_INLINE void opMoveq(uint16_t op);
    template <int S, int OP, bool M060> CPU020_INLINE void opUnary(uint16_t op, For<M060>);
    template <int S, bool M060> CPU020_INLINE void opTst(uint16_t op, For<M060>);
    template <bool M060> void opTas(uint16_t op, For<M060>);
    template <bool M060> CPU020_INLINE void opScc(uint16_t op, For<M060>);
    template <bool M060> CPU020_INLINE void opDbcc(uint16_t op, For<M060>);
    template <bool M060> CPU020_INLINE void opBcc(uint16_t op, For<M060>);
    template <bool M060> CPU020_INLINE void opLea(uint16_t op, For<M060>);
    template <bool M060> CPU020_INLINE void opPea(uint16_t op, For<M060>);
    template <bool M060> CPU020_INLINE void opJmp(uint16_t op, For<M060>);
    template <bool M060> CPU020_INLINE void opJsr(uint16_t op, For<M060>);
    template <bool M060> CPU020_INLINE void opRts(uint16_t op, For<M060>);
    void opRtr(uint16_t op);
    template <bool M060> void opRtd(uint16_t op, For<M060>);
    void opRte(uint16_t op);
    void opTrap(uint16_t op);
    void opTrapv(uint16_t op);
    void opTrapcc(uint16_t op);
    template <int S, bool M060> CPU020_INLINE void opLink(uint16_t op, For<M060>);
    CPU020_INLINE void opUnlk(uint16_t op);
    void opMoveUsp(uint16_t op);
    CPU020_INLINE void opNop(uint16_t op);
    void opReset(uint16_t op);
    void opStop(uint16_t op);
    void opIllegal(uint16_t op);
    void opLineA(uint16_t op);
    void opLineF(uint16_t op);
    void opBkpt(uint16_t op);
    CPU020_INLINE void opSwap(uint16_t op);
    CPU020_INLINE void opExt(uint16_t op);
    template <bool M060> void opExg(uint16_t op, For<M060>);
    void opMoveFromSr(uint16_t op);
    void opMoveFromCcr(uint16_t op);
    void opMoveToCcr(uint16_t op);
    void opMoveToSr(uint16_t op);
    template <int S, bool M060> CPU020_INLINE void opMovemToMem(uint16_t op, For<M060>);
    template <int S, bool M060> CPU020_INLINE void opMovemToReg(uint16_t op, For<M060>);
    template <int S> void opMovep(uint16_t op);
    template <int OP, bool M060> CPU020_INLINE void opBitDyn(uint16_t op, For<M060>);
    template <int OP, bool M060> CPU020_INLINE void opBitImm(uint16_t op, For<M060>);
    template <int S, int TYPE, bool LEFT, bool M060> CPU020_INLINE void opShiftReg(uint16_t op, For<M060>);
    template <int TYPE, bool LEFT, bool M060> void opShiftMem(uint16_t op, For<M060>);
    template <int S, int TYPE, bool LEFT> uint32_t shift(uint32_t v, int count);
    template <bool SIGNED, bool M060> CPU020_INLINE void opMulW(uint16_t op, For<M060>);
    template <bool SIGNED, bool M060> void opDivW(uint16_t op, For<M060>);
    template <bool M060> void opMulL(uint16_t op, For<M060>);
    template <bool M060> void opDivL(uint16_t op, For<M060>);
    template <int S> void opChk(uint16_t op);
    template <int S> void opChk2Cmp2(uint16_t op);
    template <int S> void chk2Flags(uint32_t lb, uint32_t ub, uint32_t r, bool isA); // CMP2 / CHK2 の未定義の N と V
    template <int S> void opCas(uint16_t op);
    template <int S> void opCas2(uint16_t op);
    template <int S> void opMoves(uint16_t op);
    void opMovec(uint16_t op);
    template <bool SUB> void opBcd(uint16_t op);
    void opNbcd(uint16_t op);
    void opPack(uint16_t op);
    void opUnpk(uint16_t op);
    void opBitField(uint16_t op);
    void opCallm(uint16_t op);
    void opMmu030(uint16_t op);
    void opFpu(uint16_t op);
    // 68060 (cpu060.inc)
    void opUnimpInt(uint16_t op);  // 未実装整数命令 (MOVEP、CHK2 / CMP2、CAS2)
    void unimplementedInteger();   // ベクタ 61 (フォーマット $0、PC はこの命令)
    // 68060 でこの命令を未実装整数命令の例外にしたら true (setUnimplementedAssist で肩代わりするなら false)
    bool unimpl060();
    bool assist060(int vector); // 68060 にない命令を、ベクタ vector の例外にせずその場で実行するなら true
    void opCache060(uint16_t op);  // CINV / CPUSH
    void opMmu060(uint16_t op);    // PFLUSH / PLPA
    void opMove16(uint16_t op);
    void opLpstop(uint16_t op);

    // ---- 内部 ----
    struct TlbEntry {
        uint32_t tag; // 論理ページ (アドレス >> 12)。使っていなければ INVALID
        uintptr_t base; // ホストのポインタ - ページの論理アドレス
    };
    static constexpr int TLB_BITS = 9;
    static constexpr int TLB_SIZE = 1 << TLB_BITS;
    static constexpr uint32_t TLB_INVALID = 0xffffffffu;
    enum { TLB_READ = 0, TLB_WRITE = 1, TLB_FETCH = 2 };
    // 今の S ビットのデータ TLB。アクセスごとに [種類][S] の位置を計算しない。
    // ステートには保存せず、S を変える所と flushTlb() で選び直す。
    TlbEntry* dataReadTlb_ = tlb_[TLB_READ][1];
    TlbEntry* dataWriteTlb_ = tlb_[TLB_WRITE][1];
    void selectDataTlb() {
        dataReadTlb_ = tlb_[TLB_READ][sflag_];
        dataWriteTlb_ = tlb_[TLB_WRITE][sflag_];
    }
    void setSupervisor(bool s) {
        sflag_ = s;
        selectDataTlb();
    }
    uint32_t fetchPage_ = TLB_INVALID; // 今の命令のページ (TLB_FETCH の写し)
    uintptr_t fetchBase_ = 0;
    bool tlbFill(int kind, uint32_t addr, int fc, TlbEntry& e);
    // ---- 68060 の MMU (cpu060.inc) ----
    // 論理アドレスを物理アドレスにする。変換できなければアクセスエラー (戻ってこない)
    uint32_t translate(uint32_t addr, int fc, bool write, bool fetch) {
        return mmuOn_ ? translateSlow(addr, fc, write, fetch) : addr;
    }
    uint32_t translateSlow(uint32_t addr, int fc, bool write, bool fetch);
    uint32_t tableWalk(uint32_t addr, bool super, bool write, bool fetch, int fc, bool& writable);
    uint32_t physRead32(uint32_t pa);
    // MOVES で、変換しない空間 (ファンクションコード 0 / 3 / 4 / 7) を読み書きする。size は 1 / 2 / 4
    uint32_t altRead(uint32_t pa, int size, int fc);
    void altWrite(uint32_t pa, uint32_t v, int size, int fc);
    void physWrite32(uint32_t pa, uint32_t v);
    void updateMmu();
    // アクセスエラー (フォーマット $4)。fslw は FSLW (MC68060UM 図 8-6) の原因のビット
    [[noreturn]] void accessFault(uint32_t addr, uint32_t fslw, bool write, int fc, bool fetch);
    uint32_t fslwBase(bool write, int fc, bool fetch) const;
    // ATC (変換の結果を覚えておく。ホストのメモリでない場所の読み書きで、毎回表を引かないため)
    struct AtcEntry {
        uint32_t tag;   // 論理ページ (4KB)
        uint32_t ppage; // 物理ページ
        bool writable;  // 書いてよい (書き込み保護がなく、M ビットを立て済み)
    };
    static constexpr int ATC_SIZE = 256;
    void flushAtc();
    bool mmuOn_ = false; // 68060 で、変換か書き込み保護をする設定になっている
    bool inTableWalk_ = false;
    // 表を引くもとになったアクセス (表を引く途中のバスエラーで積む)
    uint32_t walkAddr_ = 0;
    bool walkWrite_ = false;
    bool walkFetch_ = false;
    uint8_t walkFc_ = 0;
    uint32_t logical_ = 0; // Bus に出したアクセスの論理アドレス (アクセスエラーのフレームに積む)
    bool logicalWrite_ = false;
    // 68060 は命令をはじめからやり直すので、アクセスエラーなどではアドレスレジスタを命令の始めの値に戻す。
    // (An)+ / -(An) のほか、PEA、JSR、BSR、LINK、MOVEM -(An)、MOVE16 などが命令の途中で動かした An も戻す。
    // A7 は、命令の途中で S / M が変わっていれば別のスタックなので戻さない。
    // CCR も戻す (ADDX -(Ay),-(Ax) などは、書き込みでアクセスエラーになっても X を変えたあとなので、やり直すと結果が変わる)。
    // 戻し方は 2 通りあり、命令ごとに表に入れる関数 (thunk の U) で決まる:
    // - saveUndo() で A0-A7、CCR、S、M を写しておき (undoFull_ が 1)、それに戻す。割り込みを受け付ける前、例外の
    //   フレームを積み終えたあと、U_FULL の命令の始めに写す
    // - 写さない命令 (U_NONE、U_CCR) は、68030 と同じ記録 (logA()、saveCcrUndo()) から戻す。A7 を変えたあとに S / M が
    //   変わる命令は U_FULL にする
    // 以前は命令ごとに saveUndo() を呼んでいた。PIX68K_UNDO_CHECK を定義してビルドすると、以前と同じ写しも取り、
    // undoAddressRegs() の結果が以前と同じかを確かめる (違えば止める)
    uint32_t undoA_[8] = {};
    uint32_t undoCcr_[5] = {};
    bool undoS_ = false, undoM_ = false;
#ifdef PIX68K_UNDO_CHECK
    uint32_t chkA_[8] = {};
    uint32_t chkCcr_[5] = {};
    bool chkS_ = false, chkM_ = false;
    void undoCheckSave() {
        for (int i = 0; i < 8; i++) chkA_[i] = a_[i];
        chkCcr_[0] = flagX_, chkCcr_[1] = flagN_, chkCcr_[2] = flagZ_, chkCcr_[3] = flagV_, chkCcr_[4] = flagC_;
        chkS_ = sflag_, chkM_ = mflag_;
    }
    void undoCheckCompare(uint32_t a7Before);
#else
    void undoCheckSave() {}
#endif
    // 命令ごとに呼ぶので、memcpy ではなく __builtin_memcpy で写す。ベアメタル版 (Circle) は -ffreestanding で
    // ビルドするので、memcpy と書くと Circle の memcpy() の呼び出しになり、32 バイトを 1 バイトずつ写す
    void saveUndo() {
        __builtin_memcpy(undoA_, a_, sizeof undoA_);
        undoCcr_[0] = flagX_;
        undoCcr_[1] = flagN_;
        undoCcr_[2] = flagZ_;
        undoCcr_[3] = flagV_;
        undoCcr_[4] = flagC_;
        undoS_ = sflag_;
        undoM_ = mflag_;
        undoFull_ = 1;
        splitDepth_ = 0;
        altAccess_ = false;
        undoCheckSave();
    }
    void undoAddressRegs() {
#ifdef PIX68K_UNDO_CHECK
        const uint32_t a7Before = a_[7];
#endif
        if (undoFull_) {
            for (int i = 0; i < 7; i++) a_[i] = undoA_[i];
            if (sflag_ == undoS_ && mflag_ == undoM_) a_[7] = undoA_[7];
            flagX_ = undoCcr_[0];
            flagN_ = undoCcr_[1];
            flagZ_ = undoCcr_[2];
            flagV_ = undoCcr_[3];
            flagC_ = undoCcr_[4];
        } else {
            // 記録は消さない (このあと例外のフレームを積み終えたところで saveUndo() が呼ばれる)
            const int n = undoN_ & ~UNDO_CCR;
            for (int i = (n < UNDO_LOG ? n : UNDO_LOG) - 1; i >= 0; i--)
                a_[undoLog_[i] & 7] = (uint32_t)(undoLog_[i] >> 8);
            if (undoN_ & UNDO_CCR) {
                flagX_ = undoCcr_[0];
                flagN_ = undoCcr_[1];
                flagZ_ = undoCcr_[2];
                flagV_ = undoCcr_[3];
                flagC_ = undoCcr_[4];
            }
        }
#ifdef PIX68K_UNDO_CHECK
        undoCheckCompare(a7Before);
#endif
    }
    // 68060 で、ページをまたぐ・境界にそろっていないアクセスを分けて行っている間は、アクセスエラーに元のアクセスの
    // アドレスと大きさを積み、FSLW の MA を立てる (MC68060UM 8.4.4.2、8.4.4.3)
    uint8_t splitDepth_ = 0;
    uint8_t splitCycle_ = 0;
    uint32_t splitAddr_ = 0;
    void beginSplit(uint32_t a, uint8_t cycle) {
        if (is060() && splitDepth_++ == 0) {
            splitAddr_ = a;
            splitCycle_ = cycle;
        }
    }
    void endSplit() {
        if (is060()) splitDepth_--;
    }
    // 命令を打ち切ったあと (longjmp) に、分けて行っていたアクセスの記録を消す。68060 だけ (68030 では使わない)。
    // 以前は saveUndo() が命令ごとに消していた。命令が終わりまで進めばどちらも元の値に戻っているので、消すのは
    // 打ち切ったあとと、run() / step() の始め (ステートを読んだあと) でよい
    void resetSplit() {
        if (is060()) {
            splitDepth_ = 0;
            altAccess_ = false;
        }
    }
    // MOVES で FC 0 / 3 / 4 / 7 を読み書きしている (FSLW の TT は 2、表 2-2)
    bool altAccess_ = false;
    bool inAddrErr_ = false; // 68060 のアドレスエラー・アクセスエラーの例外を積んでいる途中 (ここでのアドレスエラーはダブルフォルト)
    UnimplementedAssist unimplAssist_ = ASSIST_OFF;
    uint32_t holeStart_ = 0, holeSize_ = 0;
    static uint8_t openBusRead_[4096];  // 何もつながっていない場所を読んだ値 ($FF)
    static uint8_t openBusWrite_[4096]; // 何もつながっていない場所に書いた値の捨て場所
    uint16_t slowRead16(uint32_t a, int fc);
    uint32_t slowRead32(uint32_t a, int fc);
    uint8_t slowRead8(uint32_t a, int fc);
    void slowWrite8(uint32_t a, uint8_t v, int fc);
    void slowWrite16(uint32_t a, uint16_t v, int fc);
    void slowWrite32(uint32_t a, uint32_t v, int fc);
    uint16_t slowFetch16();
    uint32_t busAddr(uint32_t a, int fc, bool write, bool fetch = false);
    const uint8_t* physPtr(uint32_t pa, bool write, bool super) const;

    Bus* bus_;
    CpuModel model_;
    Fpu* fpu_ = nullptr;
    int baseCost_ = SUB * 3 / 5; // 68000 の 1 サイクルあたりの 1/SUB サイクル数 (68030 は 3/5 サイクル)
    int cost_ = SUB * 3 / 5; // baseCost_ に setSpeed() の倍率を掛けたもの
    int fpuCost_ = SUB; // FPU の 1 サイクルあたりの 1/SUB サイクル数 (68060 では 0)
    int unit060_ = 0;   // 68060 の 1 サイクルあたりの 1/SUB サイクル数に setSpeed() の倍率を掛けたもの (68030 では 0)
    // 68060 でキャッシュが無効のとき、バスに出る読み書き 1 回にかかるサイクル数 (1/SUB サイクル単位)。
    // 本体のメモリ: 実機の 060turbo で命令 / データキャッシュを切ったときの si.r の processor performance (410.38%、
    // 413.36%) から決めた。ループ (addq.l / tst.b (a0) / bne、1 周に命令の取り出しが 2 回とデータの読み出しが 1 回) が
    // 1 周 約 32.4 サイクルなので、1 + 3 x 10.5 = 32.5 になる 10.5 サイクル。
    // ローカルメモリ: 実機の値は見つかっていない。MC68060UM 10.3.4 の「2 + w」で、60ns の DRAM の読み出しを w = 4
    // クロック (80ns) とした仮の値
    static constexpr int MISS060_MAIN = SUB * 21 / 2;
    static constexpr int MISS060_LOCAL = SUB * 6;
    // 上の 2 つを setClock060() のクロックのサイクル数に直したもの (1/SUB サイクル単位)。
    // どちらも、時間 (本体のメモリ 210ns、ローカルメモリ 120ns) はクロックによらず同じとした。本体のメモリは本体のバス
    // (25MHz) の時間なので、68060 のクロックを上げても変わらない。ローカルメモリは、実機でクロックを上げたときの値が
    // 見つかっておらず、DRAM の時間で決まるものとした (推定。75MHz で 9 サイクル)
    int missMain060_ = MISS060_MAIN;
    int missLocal060_ = MISS060_LOCAL;
    int dataCost16_[2] = {}, dataCost32_[2] = {}; // [0] $10000000 より下、[1] ローカルメモリ
    int slowCost16_ = 0, slowCost32_ = 0, missCost_[2] = {};
    int fetchWait_[2] = {};
    bool fetchWaitOn_ = false;
    bool dataTick_ = true; // dataCost16_ / dataCost32_ のどれかが 0 でない
    uint32_t fetchLong_ = 0;  // 最後に取り出した命令のロングワードのアドレス (fetchValid_ のとき)
    bool fetchValid_ = false; // fetchLong_ が決まっている
    void updateCacheCosts();
    uint64_t busWait_ = 0; // setBusWait() (1/SUB サイクル単位)
    int speed_ = 100;
    uint64_t sub_ = 0;
    int ipl_ = 0, lastIpl_ = 0;
    bool nmiPending_ = false;
    bool halted_ = false, stopped_ = false;
    // 0 でなければ、次の命令の前に、止まっているか (halted_、stopped_)、割り込みを受け付けるか (ipl_、lastIpl_、
    // imask_、nmiPending_)、トレースするか (trace_) を調べる。0 の間は、どれも前に調べたときから変わっておらず、
    // 調べなくても結果が同じなので、命令ごとの検査を省く (runUntil())。これらを変える所 (setIPL()、setSR()、reset()、
    // setRegisters()、ステートの読み込み)、step() の始め、命令を打ち切ったあと (longjmp) で 1 にする。ステートには入れない。
    // 上の検査は ATTN_CHECK (ビット 0。「1 にする」と書いた所はこのビット)。
    // ATTN_FETCH (ビット 1) は、68060 の命令キャッシュが無効 (fetchWaitOn_) の間だけ立つ。命令ごとに fetchWaitOn_ を
    // 調べるかわりに、その間は runLoop() が取り出しの時間を足す経路で実行する (検査は省いたまま)。ATTN_CHECK を
    // 決め直すときに一緒に決める (runLoop())。fetchWaitOn_ を変える updateCacheCosts() は attn_ を ATTN_CHECK にして、
    // 次の命令の前に決め直させる
    static constexpr uint8_t ATTN_CHECK = 1, ATTN_FETCH = 2;
    uint8_t attn_ = 1;
    // run() を、今の命令が終わったところで打ち切る (endRun())。run() の中でだけ立ち、ステートには入れない
    uint8_t endRun_ = 0;
    bool inException_ = false; // 例外のスタックを積んでいる途中 (ここでのバスエラーはダブルフォルト)
    bool inIack_ = false;
    // Bus に出したアクセスの種類 (バスエラーの SSW の SIZE と、命令の読み込みかどうか)。下位 2 ビットが SSW の SIZE
    enum : uint8_t { BUS_LONG = 0, BUS_BYTE = 1, BUS_WORD = 2, BUS_FETCH = 4 };
    uint8_t busCycle_ = BUS_LONG;
    jmp_buf iackAbort_;
    jmp_buf abort_;
    bool flow_ = false; // 実行中の命令が PC か SR を書いた (T0 のトレース)
    bool noTrace_ = false;
    // 68030 の命令のやり直しの記録 (logA()、saveCcrUndo()、noteBus())。flow_ と noTrace_ の隣に置き、命令の始めに
    // 8 バイトをまとめて 0 にする
    uint8_t undoN_ = 0; // logA() の記録の数。bit7 (UNDO_CCR) は saveCcrUndo() で CCR を取っておいた
    uint8_t busN_ = 0;  // この命令で Bus に出したデータのアクセスの数 (busSig_)
    uint8_t replayN_ = 0; // DF を 0 にした RTE のやり直しの間 1 (replaySig_ のアクセスをまだ済ませていない)
    uint8_t undoFull_ = 0; // 68060: この命令で saveUndo() を呼んだ (undoAddressRegs() は写しに戻す)
    uint8_t undoPad_[2] = {}; // 8 バイトにして、clearUndoLog() を 1 回の書き込みにする

    const uint8_t* fastRead_[2][FAST_PAGES];
    uint8_t* fastWrite_[2][FAST_PAGES];
    uint32_t extStart_ = 0, extSize_ = 0;
    uint8_t* ext_ = nullptr;

    static Handler table030_[65536];
    // 68060 の表。命令ごとに引くものを 1 項目 (16 バイト) にまとめ、1 回の読み込みで済ませる (#224)。
    // meta は下位から: ビット 0-15 サイクル数と同時実行の分類 (cyc060())、16-39 命令が書くレジスタ (wr060())、
    // 40-63 命令が使うレジスタ (use060())。値の意味は cpu060_cycles.inc
    struct Op060 {
        Handler fn;
        uint64_t meta;
    };
    static Op060 ops060_[65536];
    static uint16_t cyc060(uint16_t op) { return (uint16_t)ops060_[op].meta; }
    static uint32_t wr060(uint16_t op) { return (uint32_t)(ops060_[op].meta >> 16) & 0xffffffu; }
    static uint32_t use060(uint16_t op) { return (uint32_t)(ops060_[op].meta >> 40); }
    Handler handlerOf(uint16_t op) const { return is060() ? ops060_[op].fn : table030_[op]; }
    static bool tablesReady_;
    static void buildTables();

    // ---- 68060 のサイクル数 (cpu060_cycles.inc) ----
    static void buildCycles060();
    // 前の命令が、次の命令を sOEP に入れられるなら 0 でない (その命令の cyc060() の値)。pairDest_ は、前の命令の
    // 結果のレジスタを use060() の並びにしたもの (後の命令がこれを使うなら同時に進まない)
    uint32_t pairLead_ = 0, pairDest_ = 0;
    static constexpr int BCACHE060 = 256;
    // 分岐キャッシュ: 命令のアドレス (全部のビット) と、状態 (ビット 0-1 は 2 ビットの飽和カウンタ、BC_VALID は
    // 入っている、BC_USER はユーザモードで入れた)。状態が 0 の項目は空
    uint32_t bcachePc060_[BCACHE060] = {};
    uint8_t bcacheState060_[BCACHE060] = {};
    static constexpr uint8_t BC_VALID = 4, BC_USER = 8;
    // 分岐キャッシュを消す (CACR の CABC は全部、CUBC はユーザモードで入れたものだけ)
    void clearBranchCache060(bool userOnly) {
        for (uint8_t& st : bcacheState060_)
            if (!userOnly || (st & BC_USER)) st = 0;
    }
    // predict060()、fold060()、bcc060() は、呼ぶ所に展開する (#224)。Bcc の関数 (opBcc()) は条件と変位の大きさごとに
    // 分けてコンパイルされていて、分岐するかどうか (taken) が呼ぶ所で決まっているので、展開すると場合分けが消える。
    // 定義は cpu060_cycles.inc (cpu020.cpp が、使う所より前に読む)
    CPU020_INLINE int predict060(bool taken);
    CPU020_INLINE void bcc060(bool taken, bool backward);
    void jump060(int predicted);
    CPU020_INLINE int fold060();
    // 分岐でない命令を実行してから、まだ分岐 (Bcc / BRA / BSR / DBcc / JMP / JSR) を実行していない。予測が当たって分岐する
    // Bcc / BRA / JMP を 0 サイクルに折りたためるのは、このときだけ (fold060())
    bool foldOk060_ = false;
    void fetchSeq060(uint32_t a);
    void jmp060(uint16_t op, int base);
    void dbcc060(bool taken, bool dbra);
    // リセットと、ステートを読み込んだとき (古い版のステートには入っていない) に、数え方の状態を空にする
    void clearTiming060() {
        pairLead_ = pairDest_ = 0;
        foldOk060_ = false;
        fetchValid_ = false;
        clearBranchCache060(false);
    }
    static Handler decode(uint16_t op, CpuModel model);
    template <bool M060> void execOne();
    template <bool M060> void execInstr(int traceMode, bool fast, bool fetchWait);
    template <bool M060> void stopUntil(uint64_t endSub);
    template <bool M060> void runLoop(uint64_t endSub);
    void runUntil(uint64_t endSub);

    // ---- 68030 のバスエラーからの復帰 (busError()、opRte()) ----
    // 68030 も、バスエラー・アドレスエラーの RTE は命令をはじめからやり直す (busError() の説明) ので、命令の途中で
    // 動かしたアドレスレジスタを戻す。68060 のように命令ごとに A0-A7 を写すと 68030 の経路が遅くなるので、命令の途中で
    // An を変えたあとにまだバスエラーになりうる場所 (eaAddr() の (An)+ / -(An)、PEA、JSR、BSR、LINK、MOVEM) でだけ、
    // 変える前の値を記録する (logA())。記録の数 (undoN_) は命令の始めに 0 にする (flow_ と同じ所)。
    // CCR は、入ってくる X や Z で結果が変わり、フラグを変えてから書く命令 (ADDX、SUBX、ABCD、SBCD、NBCD、NEGX、
    // ROXL / ROXR のメモリ) でだけ取っておく (saveCcrUndo())。ほかの命令はやり直しても同じフラグになる
    static constexpr int UNDO_LOG = 16;
    static constexpr uint8_t UNDO_CCR = 0x80;
    uint64_t undoLog_[UNDO_LOG] = {}; // 変える前の値 << 8 | レジスタの番号
    void logA(int r) {
        const unsigned n = undoN_;
        undoLog_[n & (UNDO_LOG - 1)] = ((uint64_t)a_[r] << 8) | (unsigned)r;
        undoN_ = (uint8_t)(n + 1);
    }
    void saveCcrUndo() {
        undoCcr_[0] = flagX_;
        undoCcr_[1] = flagN_;
        undoCcr_[2] = flagZ_;
        undoCcr_[3] = flagV_;
        undoCcr_[4] = flagC_;
        undoN_ |= UNDO_CCR;
    }
    void clearUndoLog() {
        undoN_ = 0;
        busN_ = 0;
        replayN_ = 0;
        undoFull_ = 0;
        undoPad_[0] = undoPad_[1] = 0;
    }
    void undoLog() {
        const int n = undoN_ & ~UNDO_CCR;
        for (int i = (n < UNDO_LOG ? n : UNDO_LOG) - 1; i >= 0; i--)
            a_[undoLog_[i] & 7] = (uint32_t)(undoLog_[i] >> 8);
        if (undoN_ & UNDO_CCR) {
            flagX_ = undoCcr_[0];
            flagN_ = undoCcr_[1];
            flagZ_ = undoCcr_[2];
            flagV_ = undoCcr_[3];
            flagC_ = undoCcr_[4];
        }
        clearUndoLog();
    }
    // Bus に出した書き込みの値 (フォーマット $B のフレームのデータ出力バッファに積む)
    uint32_t dob_ = 0;
    // 処理ルーチンが DF を 0 にして RTE したとき、やり直す命令の中で、バスエラーになったアクセスだけは Bus に出さない。
    // 読み出しならフレームのデータ入力バッファの値を返し、書き込みなら捨てる。アドレス、FC、大きさ (BUS_xxx)、
    // 読み書きが同じアクセス (下の busSig()) のうち、命令の中で replayOrdinal_ 番目 (0 から) のもの。SUBX.B -(A0),-(A1)
    // で A0 と A1 が同じ番地など、同じ所を何回も読み書きする命令があるので、バスエラーのときに、同じアクセスが前に
    // 何回あったかを数えておく (busSig_)。
    // 記録するのは Bus に出したアクセスと、やり直しでデータ入力バッファで済ませたアクセス (直接読み書きしたものは
    // 数えない)。やり直しは同じアクセスをすべて数えるが、今の 68030 の実装 (変換をする mmuOn_ は 68060 だけ) では
    // 数は同じになる。アドレス、FC、読み書きが同じなら、直接読み書きできるかどうか (tlbFill()) は setFastPage() などで
    // 表を変えない限り同じで、バスエラーになったアクセスと同じものは、前のものもすべて Bus に出ているため。FC を
    // キーに入れないと、MOVES でユーザーの空間を読むときに、同じ番地をスーパーバイザで直接読んだもの (間接参照の
    // ポインタなど) と取り違える。
    // やり直しの間は TLB に入れず (replayAccess())、TLB を空にしてから始めるので、処理ルーチンがページを
    // ホストのメモリで直接読み書きできるように変えていても、どのアクセスも slowRead / slowWrite を通る
    uint64_t replaySig_ = 0;     // busSig()
    uint32_t replayData_ = 0;    // 読み出しなら返す値 (右詰め)
    uint8_t replayOrdinal_ = 0;  // 同じアクセスのうち何番目か
    uint8_t replaySeen_ = 0;     // やり直しの中で、同じアクセスがここまでに何回あったか
    uint64_t replayAccess(uint32_t a, int fc, uint8_t cycle, bool write, uint32_t v);
    // Bus に出すデータのアクセスを、命令の始めから順に記録する。1 つの命令で Bus に出るデータのアクセスは、
    // 多くて 68882 の BUSY のフレーム (ロング 54 個) を奇数番地から FRESTORE したとき (1 回をバイト・ワード・バイトの
    // 3 回に分ける) と、メモリ間接のポインタ (3 回) で 165 回なので、256 あれば足りる (FMOVEM.X は 75 回、MOVEM.L は
    // 51 回)。例外は RTE で、スローアウェイフレーム ($1) を続けて読むと 256 回を超えうる。busN_ は 8 ビットで、
    // 256 回目で 0 に戻る。そのときも添字は表の中に収め、何回目かを取り違えるだけ (RTE のスタックの読み出しを
    // DF を 0 にして済ませたときだけ問題になる)
    static constexpr int BUS_SIGS = 256;
    static uint64_t busSig(uint32_t a, int fc, uint8_t cycle, bool write) {
        return ((uint64_t)a << 8) | ((unsigned)fc << 4) | ((unsigned)cycle << 1) | (write ? 1u : 0u); // fc は 0 - 7
    }
    void noteBus(uint32_t a, int fc, uint8_t cycle, bool write) {
        const unsigned n = busN_;
        busSig_[n & (BUS_SIGS - 1)] = busSig(a, fc, cycle, write);
        busN_ = (uint8_t)(n + 1);
    }
    // 今バスエラーになったアクセス (sig) と同じものが、この命令で前に何回あったか。最後に記録したものが sig でなければ
    // (表を引く途中のバスエラーなど) 0
    unsigned faultOrdinal(uint64_t sig) const {
        const unsigned n = busN_;
        if (!n || busSig_[n - 1] != sig) return 0;
        unsigned k = 0;
        for (unsigned i = 0; i + 1 < n; i++) k += busSig_[i] == sig;
        return k;
    }
    // フォーマット $B のフレームで DF が 0 になっていた: PC の命令を、バスエラーになったアクセスを除いて実行する
    void resumeFaulted(uint32_t addr, uint16_t ssw, uint32_t dib, unsigned ordinal);
    // 大きな表は最後に置く (よく使うメンバのオフセットを小さく保ち、ARM64 のロード・ストアの即値に収める)
    TlbEntry tlb_[3][2][TLB_SIZE]; // [種類][スーパーバイザ][ページ]
    AtcEntry atc_[2][2][ATC_SIZE]; // [命令][スーパーバイザ]
    uint64_t busSig_[BUS_SIGS] = {}; // noteBus()

    friend class Fpu;
};

} // namespace m68k
