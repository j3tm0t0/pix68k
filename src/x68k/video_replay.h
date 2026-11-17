// 画面の合成を別のコア (スレッド) で行う部分 (しくみは video_journal.h)。
//
//   RenderLink   書き手 (X68000 を動かすコア) の側。Machine の Video に記録を付け、LineListener の呼び出しを
//                リングに入れる。attach() したら Machine の LineListener はこれになる
//   VideoReplayer 読み手 (合成するコア) の側。記録を写しの Video に当て、ラスタとフレームの終わりを
//                渡された LineListener (Renderer。写しの Video を読むように作る) に伝える
//
// 使い方:
//   Video shadow;  (大きいのでヒープに置いてよい)
//   Renderer renderer(shadow);
//   VideoJournal journal; journal.init(buf, log2);
//   VideoReplayer replayer(journal, shadow, renderer);
//   RenderLink link; link.attach(machine, journal, shadow, replayer);   // 写しを今の Video と同じにしてからつなぐ
//   合成するコア: for (;;) replayer.step();   書き手: machine.runFrame() ...
//   やめるとき: link.detach(machine) (ためた記録を処理し終えるまで待つ)
#pragma once

#include <stdint.h>

#include "x68k/machine.h"
#include "x68k/video.h"
#include "x68k/video_journal.h"

namespace x68k {

class VideoReplayer;

class RenderLink : public LineListener {
public:
    // 書き手が先に進めるフレームの数。フレームの終わりで、reader がこの数以上のフレームを描き終えていなければ、
    // 描き終えるまで待つ (リングの大きさとは別に、合成の遅れをフレームの単位で抑える)
    static constexpr unsigned MAX_FRAMES_AHEAD = 2;

    // shadow を machine の Video と同じ内容にし、記録を始める。合成するコアが journal を読んでいない間に呼ぶこと。
    // reader はこの journal を読む VideoReplayer (フレームの遅れを見るのに使う)
    void attach(Machine& machine, VideoJournal& journal, Video& shadow, const VideoReplayer& reader);
    // ためた記録を読み手が処理し終えるまで待ち、記録をやめる (Machine の LineListener は nullptr に戻す)
    void detach(Machine& machine);

    void onRaster(int raster) override;
    void onFrameEnd() override;
    // リングに入れたラスタの数 (書き手のスレッドで読む)
    uint64_t rasters() const { return rasters_; }
    // 測るとき: ラスタを入れるたびに、replayer がまだ描いていないラスタの数を見て最大を残す (統計用)
    void setLagProbe(const VideoReplayer* replayer) { probe_ = replayer; }
    unsigned maxLag() const { return maxLag_; }
    void resetMaxLag() { maxLag_ = 0; }
    // フレームの遅れを待った回数 (書き手のスレッドで読む)
    uint64_t frameWaits() const { return frameWaits_; }
    // フレームの遅れを待った時間 (journal の setClock() の時計の単位。時計がなければ 0。書き手のスレッドで読む)
    uint64_t frameWaitTicks() const { return frameWaitTicks_; }

private:
    VideoJournal* journal_ = nullptr;
    const VideoReplayer* reader_ = nullptr;
    uint64_t rasters_ = 0;
    uint64_t frames_ = 0;
    uint64_t frameWaits_ = 0;
    uint64_t frameWaitTicks_ = 0;
    const VideoReplayer* probe_ = nullptr;
    unsigned maxLag_ = 0;
};

class VideoReplayer {
public:
    VideoReplayer(VideoJournal& journal, Video& shadow, LineListener& out)
        : journal_(journal), shadow_(shadow), out_(out) {}
    // たまっている記録を処理する (リングの折り返しまで)。処理した数を返す (0 ならたまっていなかった)
    unsigned step();
    // 描き終えたラスタの数 (どのスレッドからも読める。RenderLink::rasters() との差が合成するコアの遅れ)
    uint64_t rasters() const { return __atomic_load_n(&rasters_, __ATOMIC_RELAXED); }
    // 描き終えたフレームの数 (どのスレッドからも読める)
    uint64_t frames() const { return __atomic_load_n(&frames_, __ATOMIC_ACQUIRE); }

private:
    void apply(uint32_t e);

    VideoJournal& journal_;
    Video& shadow_;
    LineListener& out_;
    uint64_t rasters_ = 0;
    uint64_t frames_ = 0;
};

} // namespace x68k
