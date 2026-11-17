// 音の合成 (OPM と ADPCM) を別のコア (スレッド) で行う部分。しくみは sound.h の説明と video_journal.h のリング。
//
//   SoundLink     書き手 (X68000 を動かすコア) の側。Machine の Sound に記録を付ける
//   SoundReplayer 読み手 (合成するコア) の側。記録を合成用の Sound に当てて音を作る。
//                 音は合成用の Sound の ring() (AudioRing) にたまる
//
// 使い方:
//   Sound synth; synth.setOutputRate(48000);   (Machine の Sound は出力の周波数を 0 のままにする)
//   VideoJournal journal; journal.init(buf, log2);
//   SoundReplayer replayer(journal, synth);
//   SoundLink link; link.attach(machine, journal);   // powerOn() の前に呼ぶ (2 つの Sound が同じ状態から始まるように)
//   合成するコア: for (;;) replayer.step();   音を出す側: synth.ring() から取り出す
//   やめるとき: link.detach(machine)
#pragma once

#include <stdint.h>

#include "x68k/machine.h"
#include "x68k/sound.h"
#include "x68k/video_journal.h"

namespace x68k {

class SoundLink {
public:
    // machine.powerOn() の前に呼ぶ
    void attach(Machine& machine, VideoJournal& journal);
    // ためた記録を読み手が処理し終えるまで待ち、記録をやめる
    void detach(Machine& machine);

private:
    VideoJournal* journal_ = nullptr;
};

class SoundReplayer {
public:
    SoundReplayer(VideoJournal& journal, Sound& synth) : journal_(journal), synth_(synth) {}
    ~SoundReplayer() { delete mercury_; }
    SoundReplayer(const SoundReplayer&) = delete;
    SoundReplayer& operator=(const SoundReplayer&) = delete;
    // たまっている記録を max 個まで処理する。処理した数を返す (0 ならたまっていなかった)。
    // 進めたクロック数の記録 1 個で最大 1ms 分 (48kHz で 48 フレーム) の音ができる。音のリング (AudioRing、
    // 340ms) を取り出す前にあふれさせないよう、遅れがたまっているときは max を小さくして取り出しの間に呼ぶ
    unsigned step(unsigned max = 64);
    // 記録に合わせて置いたまーきゅりーゆにっと (書き手の Machine につないでいなければ nullptr)
    Mercury* mercury() { return mercury_; }
    // MIDI ボードが送ったバイトの行き先 (音源)。nullptr ならバイトは捨てる。合成している間にほかのコアから
    // 設定してよい (次の step() から使う)。設定したあとは midi を消さないこと
    void setMidiOut(MidiOut* midi) { __atomic_store_n(&midi_, midi, __ATOMIC_RELEASE); }
    // 記録がなくて空いているときに呼ぶ。音源の MidiOut::idleWork() を呼び、仕事をしたら true
    bool idleMidi() {
        MidiOut* midi = __atomic_load_n(&midi_, __ATOMIC_ACQUIRE);
        return midi != nullptr && midi->idleWork();
    }

private:
    VideoJournal& journal_;
    Sound& synth_;
    Mercury* mercury_ = nullptr;
    MidiOut* midi_ = nullptr;
    CdMixer cd_; // CD-ROM 装置のオーディオ (記録の J_CD のフレームを受け取る)
};

} // namespace x68k
