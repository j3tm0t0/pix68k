// SC-55mkII の MIDI 音源。Nuked-SC55 (external/nuked-sc55、GPL 2 以降) で鳴らす。
// MIDI ボード (x68k/ym3802.h) が送ったバイトを x68k::MidiOut として受け、出力のフレームごとに音を足す。
//
// ROM は利用者が用意する。SC-55mkII の 5 つのファイルを 1 つのディレクトリに置く (romFileName() の名前か、
// 大きさで見分ける。pickRoms())。
//   rom1.bin      R15199858 (H8/532 の中の ROM)      32KB
//   rom2.bin      R00233567 (H8/532 のプログラム)    512KB
//   rom_sm.bin    R15199880 (M37450M2 の中の ROM)    4KB
//   waverom1.bin  R15209359 (波形 16Mbit)            2MB
//   waverom2.bin  R15279813 (波形 8Mbit)             1MB
// 大きさが違うもの、H8/532 のリセットのベクタがプログラムの外を指すものは読まない。
//
// Nuked-SC55 は SC-55mkII の 2 つのマイコン (H8/532 と M37450) と PCM の LSI をエミュレートする。中の状態は
// グローバル変数なので、開ける Sc55Synth は 1 つだけ (2 つ目の load() は IN_USE を返す)。
// MIDI のバイトは本物と同じくサブのマイコンの UART に入れる (Nuked-SC55 の MCU_PostUART())。
//
// 出力の周波数: PCM の LSI は 24MHz / ((スロットの数 + 1) x 25) ごとに 1 つ (オーバーサンプリングのときは 2 つ) の
// 標本を出す (SC-55mkII で 66207Hz)。その周波数を PCM の LSI の設定から求め、窓をかけた sinc の FIR
// (片側 8 タップ、256 の位相) で出力の周波数 (48kHz など) に変える。カットオフは出力の周波数の 0.45 倍。
//
// 重さ: 1 秒の音に H8/532 の 200 万命令 (24MHz / 12) とサブのマイコンと PCM の計算が要り、Munt (MT-32) より
// ずっと重い (tests/bench/sc55_bench.cpp)。音を作る 2 つのやり方:
//   同期 (setAsync(false)、既定): mixFrame() が要るだけその場で作る。ホスト版、--wav、試験。作る音は決まる
//   非同期 (setAsync(true)): mixFrame() は作っておいた音を足すだけ。音は pump() が作る (ベアメタル版はコア 3 が
//     OPM と ADPCM の記録を処理し終えた合間に呼ぶ)。SC-55 が遅れても OPM と ADPCM の音は止まらず、
//     SC-55 の音だけが途切れる。
//     MIDI のバイトには「受けたときに mixFrame() で出したフレームの数 + ASYNC_DELAY」を付け、pump() は
//     そのフレームを作る前に UART に入れる。SC-55 の音は OPM より ASYNC_DELAY (48kHz で 32ms) 遅れて鳴る。
//     pump() は mixFrame() のフレームの数 + ASYNC_DELAY より先は作らない (まだ来ていない MIDI があるため)。
//     mixFrame() が要るフレームがまだできていなければ無音を足し、数える (underruns())。間に合わなかった
//     フレームは作っても捨てる。0.25 秒より遅れたら、出力の位置から ASYNC_DELAY / 2 先まで作らずに飛ばす (skips())。
//     出力の位置までにすると、実時間より遅い Pi では飛ばしたあとも間に合わないままになるので、先に余裕を作る。
//     midiByte() / midiReset() / mixFrame() を呼ぶコアと pump() を呼ぶコアは違ってよい (1 つの書き手と
//     1 つの読み手のロックのないリング。sc55_test の testRealThreads() で、別のスレッドから呼んで確かめる)
//
// 起動: SC-55mkII のプログラムは電源を入れてから MIDI を受けるまでに時間がかかる。load() の中で BOOT_MS だけ
// 進めておく (ベアメタル版はコア 0 が読むときに行う)。
//
// メモリ: load() は heapBytes() (6MB) までを new で確保する (波形の ROM 3 つ (4MB)、並べ替えに使う 1MB、H8/532 の
// ROM と SRAM (576KB) など)。Nuked-SC55 の大きな配列は、ベアメタル版のカーネルの .bss を
// 大きくしないように、確保した領域へのポインタに変えてある (Circle はカーネルの大きさに上限がある)。ベアメタルでは、
// 呼ぶ前にヒープに残っているかを確かめる (Circle の new は足りないと全体を止めるため)。load() のあとは確保しない。
//
// 状態はステートに入れない。ステートを読んだときは、Machine が x68k/midi_shadow.h で GS の音源として設定を送り直す。
// midiReset() は 16 のチャネルにオールサウンドオフ (CC 120) を送る。途中まで受けたメッセージは、そのあと送り直す
// (ランニングステータスを変えないため)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "platform/sf2_synth.h"
#include "x68k/midi_out.h"

namespace platform {

class Sc55Synth : public x68k::MidiOut {
public:
    enum Rom { ROM1, ROM2, ROM_SM, WAVE1, WAVE2, ROM_COUNT };
    static constexpr uint32_t MCU_HZ = 24000000; // H8/532 のクロック (Nuked-SC55 の mcu.cycles の単位)
    static constexpr int ASYNC_DELAY = 1536;      // 非同期のときの遅れ (出力のフレーム。48kHz で 32ms)
    static constexpr int FIFO_FRAMES = 2048;      // 非同期のときの作った音のリング (ASYNC_DELAY より大きい 2 のべき乗)
    // MIDI のバイトの待ち行列 (2 のべき乗)。ステートを読んだときの設定の送り直し (x68k/midi_shadow.h の emit()) は
    // 最大 6549 バイトを一度に送り、それを UART に入れるには MIDI の速さで 2.1 秒ほどかかる。その間に届くバイトを
    // 合わせても入るように 16384 にする (sc55_test で emit() の最大の量と比べる)。
    // 入らないとき: ステートを続けて 3 回読み、1 回目から 3 回目までが 1 秒より短いと、送り直し (最大の量なら
    // 6.4KB x 3) が待ち行列を超え、超えた分のバイトを落とす (droppedBytes() に数える。落ちた送り直しは途中が抜けるので、
    // 設定が一部しか戻らない)。2 回までなら続けても入る。sc55_test の testRealResendBurst() で確かめている
    static constexpr int QUEUE_SIZE = 16384;
    static constexpr int BOOT_MS = 5000;          // load() の中で進めておく時間
    static constexpr uint32_t UART_CYCLES = MCU_HZ / 3125; // MIDI の 1 バイト (31250bps で 10 ビット) の時間
    static constexpr int TAPS = 16;               // 周波数を変える FIR のタップの数
    static constexpr int PHASES = 256;            // その位相の数

    enum Result {
        OK,
        BAD_SIZE,    // ファイルの大きさが違う
        READ_FAILED, // 読めなかった
        BAD_ROM,     // H8/532 のリセットのベクタがおかしい (SC-55mkII の ROM でない)
        IN_USE,      // ほかの Sc55Synth が開いている
        BAD_RATE,    // 出力の周波数が 0 以下か大きすぎる
        NO_SOUND,    // 起動しても PCM の LSI が音を出し始めない
    };
    static const char* resultText(Result r);

    static uint32_t romSize(int rom);
    static const char* romFileName(int rom); // Nuked-SC55 の名前 ("rom1.bin" など)
    static const char* romPartName(int rom); // Roland の部品の番号 ("R15199858" など)
    // ディレクトリの中のファイル (名前と大きさ n 個) から、5 つの ROM をどれにするかを決めて index[ROM_COUNT] に書く。
    // romFileName() と同じ名前 (大文字と小文字は区別しない) のものを先に選び、なければ大きさが同じものを選ぶ
    // (大きさが同じものが 2 つ以上あれば選ばない)。'.' で始まる名前は見ない (macOS が FAT に作る ._ のファイル)。
    // 選べなかった ROM があれば、その番号を返す (すべて選べたら -1)
    static int pickRoms(const char* const* names, const uint64_t* sizes, int n, int index[ROM_COUNT]);
    // load() の中で確保する量の上限 (バイト)。ベアメタルの確保の切り上げを含めても超えないよう、多めにしてある
    static uint64_t heapBytes() { return 6u << 20; }

    Sc55Synth();
    ~Sc55Synth() override;
    Sc55Synth(const Sc55Synth&) = delete;
    Sc55Synth& operator=(const Sc55Synth&) = delete;

    // ROM を読み、SC-55mkII を起動する (BOOT_MS だけ進める)。src[i] は ROM i のファイル (romSize(i) バイト)。
    // rate は出力のサンプリング周波数。idle が nullptr でなければ、読む合間と起動の間におおよそ 20ms ごとに呼ぶ
    // (ベアメタル版がコア 0 のほかの仕事をするため)
    Result load(Sf2Source* const src[ROM_COUNT], int rate, void (*idle)(void*) = nullptr, void* idleArg = nullptr);
    // 試験と、ホスト版用: メモリの上の ROM から読む (data[i] は romSize(i) バイト)
    Result load(const uint8_t* const data[ROM_COUNT], int rate);
    bool loaded() const { return loaded_; }
    // 音量 (1.0 で Nuked-SC55 の出力のまま)
    void setGain(float gain) { gain_ = gain; }
    // 音の作り方 (上の説明)。load() のあと、音を鳴らし始める前に決める
    void setAsync(bool on) { async_ = on; }
    bool async() const { return async_; }
    // 非同期のとき、音を作る。作ったフレームの数を返す (0 なら作れるところまで作ってある)。
    // 1 回で最大 maxFrames フレーム (48kHz で 16 フレームは 0.33ms 分)
    int pump(int maxFrames = 16);
    // PCM の LSI が出している標本の周波数 (Hz。まだ出していなければ 0)
    double sourceRate() const { return srcRate_; }
    uint32_t underruns() const { return __atomic_load_n(&underruns_, __ATOMIC_RELAXED); }
    uint32_t skips() const { return __atomic_load_n(&skips_, __ATOMIC_RELAXED); }
    uint32_t droppedBytes() const { return __atomic_load_n(&droppedBytes_, __ATOMIC_RELAXED); }
    // 試験用: H8/532 が進んだクロック数
    uint64_t mcuCycles() const;
    // 試験用: 待ち行列に残っているバイトと、UART に入れるのを待っているバイトの数。
    // UART に入れるのを待っているバイトの位置 (oHead_、oTail_) は音を作る側 (非同期なら pump() を呼ぶコア、同期なら
    // mixFrame() を呼ぶコア) だけが読み書きするので、そのコアから呼ぶこと。ベアメタル版で別のコア (画面に出すなど)
    // から読むなら、その 2 つを __atomic で読み書きするように変える必要がある (今のままでは古い値や競合になる)
    uint32_t pendingBytes() const {
        return (__atomic_load_n(&qHead_, __ATOMIC_ACQUIRE) - __atomic_load_n(&qTail_, __ATOMIC_ACQUIRE)) +
               (oHead_ - oTail_);
    }
    // 試験用: 周波数の変換で数える標本の番号 (srcCount_ と pos_) を n 進める (長く動かしたときの試験)。
    // n は SRC_RING の倍数にする
    void advanceSourceCount(uint64_t n) {
        srcCount_ += n;
        pos_ += n << 32;
    }
    uint64_t sourceCount() const { return srcCount_; }
    // pos_ は整数部が 32 ビットなので、2^32 標本 (66kHz で 18 時間) で一周する。その半分の SRC_REBASE (2^31。
    // 9 時間) を srcCount_ が超えたら、srcCount_ と pos_ から SRC_REBASE / 2 (SRC_RING の倍数) を引く。
    // srcCount_ は 2^31 より少し大きいところまでしか増えず、2^32 には届かない
    static constexpr uint64_t SRC_REBASE = 1ull << 31;

    // 周波数を変える FIR の表 (上の説明) を filter ([PHASES + 1][TAPS]) に書く。filter[p][j] は、出力の位置が
    // 標本 i + p / PHASES のときに標本 i - (TAPS / 2 - 1) + j に掛ける値 (隣の位相の値と線形に補間して使う)。
    // srcRate は PCM の LSI の標本の周波数、rate は出力の周波数。試験 (通過域と折り返しの減衰) でも使う
    static void buildFilter(float* filter, double srcRate, int rate);

    // ---- x68k::MidiOut ----
    void midiByte(uint8_t b) override { pushQueue(b); }
    void midiReset() override { pushQueue(RESET_ENTRY); }
    void mixFrame(int32_t& l, int32_t& r) override;
    bool idleWork() override { return pump() != 0; }

    // 試験用: n フレームの音を作って out (ステレオ、16 ビット) に書く (mixFrame() を n 回呼ぶのと同じ)
    void renderFrames(int16_t* out, int n);

    // Nuked-SC55 が標本を 1 つ出した (mcu.cpp の MCU_PostSample() から呼ばれる)
    void postSample(int l, int r);

private:
    static constexpr uint32_t RESET_ENTRY = 0x100;
    struct Entry {
        uint32_t frame;
        uint32_t value; // bit7-0 MIDI のバイト、RESET_ENTRY なら音を止める
    };
    struct Frame {
        uint32_t index;
        int16_t l, r;
    };
    Result loadCommon(Sf2Source* const src[ROM_COUNT], void (*idle)(void*), void* idleArg);
    void freeAll();
    void pushQueue(uint32_t value);
    void feedMidi(uint32_t frame);
    void feedByte(uint8_t b);
    void postUart(uint8_t b);
    void drainUart();
    void resetSound();
    void produceFrame(int16_t& l, int16_t& r);
    void updateRate();

    bool loaded_ = false;
    bool async_ = false;
    int rate_ = 0;
    float gain_ = 1.0f;
    uint8_t* wave_[3] = {}; // waverom1、waverom2、waverom3 (使わないので 0)
    uint8_t* rom1_ = nullptr;
    uint8_t* rom2_ = nullptr;
    uint8_t* sram_ = nullptr;

    // 周波数の変換。src_ は PCM の LSI の標本 (L, R) のリング。srcCount_ はこれまでに受けた数、
    // pos_ は次の出力のフレームの位置 (標本の番号、32 ビットの小数部)
    static constexpr int SRC_RING = 64;
    float src_[SRC_RING][2] = {};
    uint64_t srcCount_ = 0;
    uint64_t pos_ = 0;
    uint64_t step_ = 0;
    double srcRate_ = 0;
    uint8_t cfg3c_ = 0, cfg3d_ = 0;
    float* filter_ = nullptr; // [PHASES + 1][TAPS]

    // MIDI のバイトのリング (書き手は midiByte() / midiReset()、読み手は音を作る側)
    Entry queue_[QUEUE_SIZE];
    uint32_t qHead_ = 0, qTail_ = 0;
    // 出力のフレームの数 (mixFrame() の側が数える) と、作ったフレームの数 (音を作る側)
    uint32_t outFrames_ = 0;
    uint32_t made_ = 0;
    // 非同期のときの作った音のリング (書き手は pump()、読み手は mixFrame())
    Frame fifo_[FIFO_FRAMES];
    uint32_t fHead_ = 0, fTail_ = 0;
    uint32_t underruns_ = 0, skips_ = 0, droppedBytes_ = 0;
    // UART に入れたバイトの読み方 (midiReset() のあとで途中のメッセージを送り直すため)
    uint8_t status_ = 0, need_ = 0, have_ = 0;
    uint8_t data_[2] = {};
    bool inSysex_ = false;
    // UART に入れるのを待つバイト (MIDI の速さで入れる。drainUart())
    static constexpr int OUT_RING = 256;
    uint8_t out_[OUT_RING] = {};
    uint32_t oHead_ = 0, oTail_ = 0;
    uint64_t uartNext_ = 0; // 次のバイトを入れてよい H8/532 のクロック
};

} // namespace platform
