// MC68882 (X68030 の FPU) の命令。計算は fpu_math.h
//
// 根拠は MC68881/MC68882 User's Manual (MC68881UM)。演算の結果とフラグは、XEiJ の fputest.x の期待値に合わせている
// (期待値は計算で求めた値で、実機の結果とは限らない。FMOVECR だけは実機の値)。主な点:
// - NaN の仮数は保たない。演算の結果の NaN は常に 7FFF FFFFFFFF FFFFFFFF (fputest.x の結果)
// - シグナリング NaN は、演算のオペランドになったとき (メモリからでもレジスタからでも) SNAN を立てる。
//   FMOVEM はレジスタの値をそのまま読み書きする
// - 浮動小数点の例外は、FPCR で許可した例外が FPSR に立ったときに起こす。68882 は次の FPU の命令を始めるときに
//   知らせる (保留し、次の FPU の命令の前の例外にする。fpu.cpp の fpException())。FMOVE FPn,<ea> は書いたあとの
//   命令の途中の例外 (フォーマット $9、+$10 に書いた先のアドレス)。SNAN、OPERR、DZ が許可されていれば
//   デスティネーションのレジスタは変えない
// - FSAVE は NULL か IDLE のフレームを書く (68882 の IDLE は $1F38 で 60 バイト)。命令の途中の状態は持たない。
//   FRESTORE は IDLE の BIU フラグの例外の保留 (bit27 が 0) を見て、次の FPU の命令の前に例外を起こす
// - コプロセッサとの並行動作 (FPU が計算している間に CPU が先に進む) はしない。サイクル数は概算
//
// 68060 の FPU (MC68060UM 6 章) では次が違う:
// - 未実装の命令 (表 6-11: 超越関数、FMOD、FREM、FSCALE、FGETEXP、FGETMAN、FMOVECR、FScc、FDBcc、FTRAPcc) は
//   ベクタ 11 (フォーマット $2、PC は次の命令、メモリのオペランドの実効アドレスを積む)。定義されていない命令の語
//   (68882 で別名になっているもの) も未実装として扱う
// - 未実装の実効アドレス (F<op>.X / .P #imm、FMOVEM.L #imm で 2 つ以上の制御レジスタ、FMOVEM.X の動的リスト) は
//   ベクタ 60 (フォーマット $0、PC はこの命令)。パック 10 進のオペランドはベクタ 55 (フォーマット $2)
// - FSxxx / FDxxx (単精度 / 倍精度に丸める演算) がある
// - 非正規化数と非正規化された拡張精度のオペランドは、未実装のデータ型の例外 (ベクタ 55)。FPn に書く命令は
//   命令の前の例外 (フォーマット $0)、FMOVE OUT は命令の後の例外 (フォーマット $3)、パック 10 進はフォーマット $2
// - アンダーフロー (ベクタ 51) と、丸める前にあふれるオーバーフロー (ベクタ 53) は、許可していなくても例外にし、
//   060FPSP が結果を作る
// - FPn に書く命令の例外 (51 / 53 と、許可した SNAN / OPERR / DZ / INEX) は、FPU を例外の状態 (EXCP) にするだけで、
//   次の FPU の命令 (FSAVE を除く) を始めるときに、その命令の前の例外 (フォーマット $0、PC はその命令) として起こす。
//   FMOVE OUT の例外は、すぐに命令の後の例外 (フォーマット $3、PC は次の命令) にする
// - FSAVE のフレームは 3 ロング (NULL $00、IDLE $60、EXCP $E0。MC68060UM 図 6-10、6-11)
// - PCR の DFP が 1 なら、FPU の命令はベクタ 11 (フォーマット $4)
// 実機と違う点: EXCP のフレームの例外のオペランドは、どの例外でもソースのオペランドにする
// 060SP がない環境 (Cpu020::setUnimplementedAssist) では、未実装の命令も 68882 と同じに実行する
#pragma once

#include <stdint.h>

#include "m68k/fpu_core.h"

namespace x68k {
class StateIO;
}

namespace m68k {

class Cpu020;

// レジスタと演算は FpuCore (fpu_core.h)。ここは Cpu020 の命令としての実行 (オペランドの読み書き、例外、FSAVE / FRESTORE)
class Fpu : public FpuCore {
public:
    void state(x68k::StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)
    explicit Fpu(Cpu020* cpu);
    void reset();
    // F ライン (コプロセッサ ID 1) の命令。op は命令の最初の語
    void execute(uint16_t op);

    bool nullState_ = true; // リセット後、まだ FPU の命令を実行していない (FSAVE は NULL のフレームを書く)
    bool excPending_ = false; // FRESTORE した IDLE のフレームで例外が保留されていた
    uint32_t pendingFrame_[14] = {}; // そのときの IDLE のフレームのヘッダより後 (FSAVE でそのまま書き戻す)
    // 68060: 例外の状態 (EXCP)。0 ならなし、そうでなければ 0x80 | 例外の種類 (MC68060UM 図 6-11 の V2-V0)
    uint8_t excp060_ = 0;
    fp::X excpOp_ = {0, 0}; // EXCP のフレームの例外のオペランド

    // 条件 (6 ビット) が成り立つか (68882。68060 では表 6-9 の式)
    bool cond(int cc) const;

private:
    void general(uint16_t op);
    void scc(uint16_t op);
    void bcc(uint16_t op, bool longDisp);
    void fsave(uint16_t op);
    void frestore(uint16_t op);

    // ソースのオペランドを読む (format は命令の語の bit12-10)。addr にメモリのアドレスを返す
    bool readSource(uint16_t op, int format, fp::X& out, fp::Ctx& c, uint32_t& addr);
    void writeDest(uint16_t op, uint16_t w, int format, int n, fp::Ctx& c, uint32_t& addr);
    void moveControl(uint16_t op, uint16_t w, bool toMem);
    void movemData(uint16_t op, uint16_t w, bool toMem);
    bool checkBsun(int cc);
    // 許可された例外を起こしたら true (68882 の FPn に書く命令の例外は保留するだけで false)。mid は FMOVE FPn,<ea> の
    // 命令の途中の例外 (フォーマット $9、ea を積む)。cmd と operand は保留したときのフレームの中身
    bool fpException(bool mid, uint32_t ea, uint16_t cmd, const fp::X& operand);
    bool takePending();
    // EA: モード 7 の 4 (イミディエイト) は、命令の後ろにあるデータを指す (size バイト進める)
    uint32_t eaData(int mode, int reg, int size);
    // 68060 (fpu.h の先頭)
    bool m060() const;
    bool fpDisabled060(uint16_t op);
    bool takeExcp060();
    bool unimplemented060(uint16_t op, int vector, int format, int eaSize, bool out = false);
    uint32_t frameEa060(int mode, int reg, int size, bool out);
    uint32_t foutEa060(uint16_t op, int format, uint32_t addr);
    bool unimpEa060();
    void fpException060(uint32_t mask, bool post, uint32_t ea);
    void fsave060(int mode, int reg);
    void frestore060(int mode, int reg);
    fp::X lastSrc_ = {0, 0}; // 直前の演算のソースのオペランド (EXCP のフレームに入れる)
    // readSource() が読んだソースのオペランドを、68060 が EXCP のフレームに入れる形にしたもの (単精度・倍精度は
    // 指数のバイアスを付け替えただけの形) と、68060 が扱わない形 (非正規化数、非正規化の拡張精度) だったか
    fp::X frameSrc_ = {0, 0};
    bool srcUnsupported_ = false;
    static bool unsupportedX(const fp::X& x);
    void singleFrame(uint32_t v);
    bool unsupported060(int vector, uint32_t ea, int format, const fp::X& operand);

    Cpu020* cpu_;
};

} // namespace m68k
