// MIDI ボード (ym3802.h) が送ったバイトの行き先 (音源)。SoundFont の音源 (platform/sf2_synth.h) や、実機の音源への
// UART の出力が受ける。
//
// バイトは Sound を通して届く (sound.h の J_MIDI)。音の合成を別のコアで行うときは、その記録を当てるコアで呼ばれる。
// 記録の中の位置が送り終えた時刻なので、音を作る音源は、そのとき作っている出力のフレームの位置でバイトを受け取る。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stdint.h>

namespace x68k {

class MidiOut {
public:
    virtual ~MidiOut() {}
    // MIDI のバイトを 1 つ受け取る
    virtual void midiByte(uint8_t b) = 0;
    // 鳴っている音をすべて止める (X68000 のリセット、ステートを読んだとき)。チャネルの設定とランニングステータスは
    // 変えない (ステートを読んだときは、このあと Machine が x68k/midi_shadow.h で設定を送り直す)
    virtual void midiReset() {}
    // 音源の種類。ステートを読んだときなどに、覚えた設定を送り直すやり方を決める (x68k/midi_shadow.h の emit())。
    // GENERAL は GM / GS の音源 (SoundFont、UART の先の音源)、MT32 は MT-32 / CM-32L (platform/mt32_synth.h)
    enum Family { GENERAL, MT32 };
    virtual Family midiFamily() const { return GENERAL; }
    // 出力の 1 フレームを作って l と r に足す (16 ビットの範囲。音を作らない行き先は何もしない)。
    // Sound が出力のフレームごとに呼ぶ
    virtual void mixFrame(int32_t& l, int32_t& r) {
        (void)l;
        (void)r;
    }
    // 合成のコアが記録を処理し終えて空いたときに呼ぶ (SoundReplayer::idleMidi())。音を作るのが重い音源
    // (platform/sc55_synth.h) が、ここで先に音を作っておく。少し (1ms ほどまで) 仕事をしたら true を返す。
    // 何もしなかったら false (合成のコアは次の記録が来るまで眠る)
    virtual bool idleWork() { return false; }
};

} // namespace x68k
