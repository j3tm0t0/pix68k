// MC68881 / MC68882 の計算の部分 (レジスタ、演算、条件、オペランドの形の変換)。CPU に依存しない。
//
// fpu.h の Fpu (68030 / 68060 の F ライン命令) と、x68k/fpu_board.h の数値演算プロセッサボード
// (68000 からコプロセッサインターフェースレジスタを叩いて使う 68881) が共通に使う。
// 68000 だけのビルド (PIX68K_NO_CPU020) でもボードのために入れるので、Cpu020 には触らない。
// 演算の結果とフラグの決め方は fpu.h の説明のとおり (68882 として作ったものを、68881 にもそのまま使う)。
#pragma once

#include <stdint.h>

#include "m68k/fpu_math.h"

namespace m68k {

class FpuCore {
public:
    fp::X fp_[8];
    uint32_t fpcr_ = 0, fpsr_ = 0, fpiar_ = 0;

    // FPSR の条件コードのビット
    enum : uint32_t { CC_N = 1u << 27, CC_Z = 1u << 26, CC_I = 1u << 25, CC_NAN = 1u << 24 };
    static uint32_t ccOf(const fp::X& x);
    // 条件 (6 ビット) が成り立つか (68881 / 68882。bit4 は BSUN を起こすかどうかだけで、条件は下位 4 ビットで決まる)
    bool cond881(int cc) const;
    // FPCR の精度と丸めの方向
    fp::Ctx ctx() const;
    // FPSR の例外ステータスから AEXC を作って足す
    void accrue();
    // FPCR で許可された例外のうち、FPSR に立っているもの (オーバーフローで INEX だけ許可なら INEX)
    uint32_t enabledExceptions() const;
    // 例外のビット (優先順位の高いもの) のベクタ番号 (48-54)
    static int excVector(uint32_t mask);
    // 既定の NaN (演算の結果の NaN)
    static fp::X defaultNan();
    // NaN を既定の NaN に、0 と無限大を正しい形に、ほかを正規化する
    static fp::X canon(const fp::X& x);

    // 演算 (opmode $00-$3F)。結果は fp_[n] に入れ、条件コードを ccOut に返す。false なら定義されていない opmode
    bool arith(int opmode, const fp::X& src, int n, fp::Ctx& c, uint32_t& ccOut, bool& setCc);
    // FMOVECR: ROM の定数 off を fp_[n] に入れ、FPSR の条件コードと例外ステータスを決める
    void fmovecr(int off, int n, fp::Ctx& c);

    // オペランドの形 (命令の語の bit12-10: 0 L、1 S、2 X、3 P、4 W、5 D、6 B) のバイト数
    static int operandSize(int format);
    // メモリに置かれた形 (w は上位の語から並べたもの。B は w[0] の上位 8 ビット、W は上位 16 ビット) を読む。
    // 拡張精度のシグナリング NaN は c.exc に SNAN を立てる。NaN は既定の NaN にする
    static fp::X loadOperand(int format, const uint32_t* w, fp::Ctx& c);
    // x を format の形にして w に書く (k はパック 10 進の k ファクタ)。並びは loadOperand と同じ
    static void storeOperand(const fp::X& x, int format, int k, fp::Ctx& c, uint32_t* w);

    // 演算のサイクル数を足す先 (Fpu が CPU のサイクルを指す。nullptr なら数えない)
    uint64_t* cycleSink_ = nullptr;
    const int* cycleCost_ = nullptr;
    const int* cycleCost060_ = nullptr; // 68060 の内蔵 FPU の 1 サイクル (Cpu020::unit060_。68030 では 0)

protected:
    void resetRegisters();
    // n は 68882 のサイクル数 (68030 のとき)、n060 は 68060 の内蔵 FPU のサイクル数 (MC68060UM 表 10-25。FPn どうしの値)
    void cycles(int n, int n060 = 0) {
        if (!cycleSink_) return;
        *cycleSink_ += (uint64_t)n * *cycleCost_;
        if (cycleCost060_) *cycleSink_ += (uint64_t)n060 * *cycleCost060_;
    }
};

} // namespace m68k
