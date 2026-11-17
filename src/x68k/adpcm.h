// ADPCM (OKI MSM6258V、MPU インタフェース版。$E92001 コマンド / ステータス、$E92003 データ)。再生だけを作る。
//
// 根拠にした資料:
//   - OKI の 1987 年版 Voice Synthesis LSI Data Book の MSM6258/MSM6258V の章 (ピンの説明、
//     「HOW TO USE THE DATA BUS (MPU I/F Version)」) と、同じ本の ADPCM の方式の説明
//   - 量子化幅の 49 段の表と、符号ごとの段の動かし方 (-1 -1 -1 -1 +2 +4 +6 +8) は、同じ方式を説明した
//     Dialogic の「Dialogic ADPCM Algorithm」。表の値は 16 x 1.1^n の切り捨てになっている
//   - X68000 でのつなぎ方: Data Crystal の X68k:IOMAP と、IPL ROM (IPLROM.DAT) の IOCS _ADPCMOUT / _ADPCMMOD の
//     逆アセンブル
//
// レジスタ (データシートの「HOW TO USE THE DATA BUS」):
//   コマンド ($E92001 に書く): D0 = SP (止める)、D1 = PLAY ST (再生を始める)、D2 = REC ST (録音を始める。作っていない)
//   ステータス ($E92001 を読む): 再生中に $40、止まっていれば $C0 を返す。bit7 は、データシートでは録音中に H、
//     再生中に L。X68k:IOMAP では 0 = 再生中、1 = 録音中 / スタンバイ中で、図の bit6 は 1。止まっている間の bit7 は
//     データシートに書いていないので X68k:IOMAP に従う。bit5-0 は資料に書いていないので 0 を返す。
//     実機では確かめていない。IPL ROM の IOCS はこの値を読まない
//   データ ($E92003): 1 バイトに 4 ビットの ADPCM が 2 つ。D0-D3 が先のサンプル、D4-D7 が次のサンプル。
//     各 4 ビットの最上位が符号 (1 で波形が下がる)
//
// クロックと速さ:
//   標本化の周波数は、発振の 1/1024、1/768、1/512 (データシートの SAM1 / SAM2 の表)。X68000 では 8255 のポート C の
//   bit3-2 で選ぶ (%00、%01、%10)。%11 はデータシートでは無効、X68k:IOMAP では未使用で、ここでは 1/512 として扱う。
//   発振は 8MHz で、OPM の CT2 (レジスタ $1B の bit7) が 1 なら 4MHz (X68k:IOMAP。IPL ROM の _ADPCMOUT も、
//   3.9kHz と 5.2kHz のときに bit7 を立てている)。
//   データの要求 (DMAC のチャネル 3 の REQ) は 2 サンプルに 1 回 (データシートの MCK は標本化の周波数の 1/2)。
//
// 左右: 8255 のポート C の bit0 が 1 で右を切り、bit1 が 1 で左を切る。IPL ROM の _ADPCMOUT は、出力の指定
// (1 = 左、2 = 右、3 = 左右。PCM8A の PCM8ATEC.DOC にも同じ指定がある) の 1 と 2 をそのまま、3 を 0 に、0 を 3 にして
// bit1-0 に書く。つまり %01 が左だけ、%10 が右だけ。X68k:IOMAP の表 (%01 = 右のみ ON) は、これと逆になっている。
//
// 止めたときと始めたとき (データシートの DAOUT の説明): MPU 版は、止めるコマンドで止まったときの出力の電圧を保ち、
// 始めるコマンドで 1/2 VDD (波形の中心) に戻る。ここでも、止めたら最後の値を出し続け、PLAY ST で値を 0、
// 量子化幅を最小に戻して、最初のバイトから始める。
// 再生中の PLAY ST も同じに始め直す。PCM8A (PCM8A102.HAS) は、音が途切れたあとの出し始めに、止めずに PLAY ST だけを
// 書き (DMASA1)、自分の ADPCM の変換の状態も最初に戻している (INT040 の「出力ｵﾌｾｯﾄ初期化」)。再生中の PLAY ST を
// 無視すると、PCM8A の変換と復号の状態がずれて、波形に直流が乗る。
//
// 資料に書いていない所 (推定で作った):
//   - 再生していない間に $E92003 に書いたバイトは使わない (PLAY ST のあとに来たバイトから復号する)
//   - データが間に合わなかったとき (DMA が止まった、PCM8A が音のない間 DMA を止めている、など) は、次のバイトが
//     来るまで出力を保ち、要求を出したまま待つ。実機が、最後に書かれたバイトを復号し続けるのかどうかは分からない
//
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stdint.h>

namespace x68k {

class StateIO;

class Adpcm {
public:
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)
    void reset();

    // offset はデバイスの窓の先頭からの位置 (下位 2 ビットを見る)。1 がコマンド / ステータス、3 がデータ
    uint8_t read(uint32_t offset) const;
    void write(uint32_t offset, uint8_t v);

    // 発振の選択 (OPM の CT2。true で 4MHz)
    void setClock4M(bool on) { clock4M_ = on; }
    // 8255 のポート C の bit1-0 (左右) と bit3-2 (標本化の周波数)
    void setPanRate(uint8_t pan, uint8_t rate) {
        pan_ = pan & 3;
        rate_ = rate & 3;
    }
    // 上の 3 つをまとめた値 (bit4 = 4MHz、bit3-2 = 周波数、bit1-0 = 左右。Sound の記録と同じ並び)
    uint32_t lines() const { return (clock4M_ ? 0x10u : 0u) | (uint32_t)rate_ << 2 | pan_; }

    // 時間を進める。clocks は 4MHz (OPM のクロック) の数
    void advance(uint32_t clocks);

    // DMAC への転送の要求
    bool drq() const { return drq_; }
    bool playing() const { return playing_; }
    // 今の出力 (12 ビット、-2048..2047)。切っている側は 0
    int32_t left() const { return (pan_ & 2) ? 0 : signal_; }
    int32_t right() const { return (pan_ & 1) ? 0 : signal_; }

    // 1 サンプルの長さ (8MHz のクロックの数)
    uint32_t period() const;

    // 4 ビットの ADPCM を 1 つ復号する。signal は波形の値 (12 ビット)、step は量子化幅の段 (0..48)。
    // どちらも書き換え、新しい波形の値を返す
    static int32_t decode(uint8_t nibble, int32_t& signal, int32_t& step);

    static constexpr int STEP_MAX = 48;

private:
    void sample(); // 1 サンプル分進める

    bool playing_ = false;
    bool clock4M_ = false;
    bool drq_ = false;       // 次のバイトを求めている
    bool latched_ = false;   // latch_ に、まだ使っていないバイトがある
    bool second_ = false;    // current_ の上位 4 ビット (2 つ目のサンプル) を次に復号する
    uint8_t pan_ = 0, rate_ = 0;
    uint8_t latch_ = 0;      // CPU / DMAC が書いたバイト
    uint8_t current_ = 0;    // 復号している途中のバイト
    int32_t signal_ = 0;     // 波形の値 (-2048..2047)
    int32_t step_ = 0;       // 量子化幅の段 (0..48)
    uint32_t elapsed_ = 0;   // 前のサンプルから過ぎた時間 (8MHz のクロックの数。period() より小さい)
};

} // namespace x68k
