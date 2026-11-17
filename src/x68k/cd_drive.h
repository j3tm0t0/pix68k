// SCSI の CD-ROM 装置の、媒体 (トラックの並び) とオーディオの再生。コマンドの処理は spc.cpp の Spc::executeCdrom()。
//
// 媒体は HardDiskImage (イメージのバイト列) と CdToc (トラックの並び) で表す。CdToc がなければ、イメージの全体を
// 2048 バイト / セクタのデータトラック 1 つとして扱う (ISO)。CUE シートのイメージは、環境が platform/cue_sheet.h で
// CdToc を作って渡す。トラックごとに、イメージの中の位置、1 セクタのバイト数 (2048 / 2336 / 2352)、セクタの中の
// ユーザデータの位置を持つ。オーディオのトラックは 2352 バイト / セクタ (16 ビットのステレオ、44.1kHz、588 フレーム)。
//
// 装置と媒体は別に持つ: 媒体がなくても (取り出したあとも) 装置はセレクションに答え、媒体の要るコマンドには
// NOT READY (MEDIUM NOT PRESENT) を返す。環境が媒体を入れ替えたら (Spc::changeCd())、次のコマンドに 1 回だけ
// UNIT ATTENTION (NOT READY TO READY TRANSITION, MEDIUM MAY HAVE CHANGED) を返す。
//
// オーディオの再生は X68000 の時間で進める: Machine が進めたクロック (4MHz) を produce() に渡し、その間のフレーム
// (44.1kHz) をイメージから読んで受け取る。イメージを読むのは X68000 を動かすコアだけ (x68k/disk_cache.h) なので、
// 音の合成を別のコアで行うときも、フレームはここで読んで Sound の記録に入れる (sound.h の J_CD)。
// 大きなイメージのチャンクを読み込んでいる間は、再生の位置を進めずに待つ (その間は無音)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stdint.h>

#include "x68k/hdd.h"

namespace x68k {

class StateIO;

struct CdTrack {
    uint8_t number = 1;     // トラック番号 (1..99)
    uint8_t control = 0x04; // Q サブチャネルのコントロール (bit2 = データトラック)
    uint8_t dataOffset = 0; // セクタの中のユーザデータ (2048 バイト) の位置。オーディオは 0
    bool swap = false;      // オーディオのサンプルがビッグエンディアン (CUE の FILE ... MOTOROLA)
    uint16_t sector = 2048; // イメージの中の 1 セクタのバイト数
    uint32_t begin = 0;     // このトラックの最初の論理ブロック (プリギャップを含む)
    uint32_t start = 0;     // INDEX 01 の論理ブロック (TOC に出す位置)
    uint32_t fileLba = 0;   // イメージに入っている最初のセクタの論理ブロック (begin 以上。その前は無音)
    uint32_t fileEnd = 0;   // イメージに入っている最後のセクタの次の論理ブロック (その後ろは無音)
    uint64_t offset = 0;    // fileLba のセクタの、イメージの中の位置 (バイト)
    bool audio() const { return (control & 0x04) == 0; }
};

struct CdToc {
    static constexpr int MAX_TRACKS = 99;
    int count = 0;
    uint32_t leadOut = 0; // リードアウトの論理ブロック (最後のトラックの終わり)
    CdTrack tracks[MAX_TRACKS];

    // lba を含むトラックの番号 (tracks[] の位置)。範囲の外なら -1
    int find(uint32_t lba) const {
        if (lba >= leadOut) return -1;
        int t = -1;
        for (int i = 0; i < count && tracks[i].begin <= lba; i++) t = i;
        return t;
    }
    uint32_t end(int t) const { return t + 1 < count ? tracks[t + 1].begin : leadOut; }
};

// オーディオのフレーム (44.1kHz) を OPM のサンプル (62.5kHz) の刻みに直す。Sound が OPM の 1 サンプルごとに
// clock() を呼ぶ。62500 : 44100 = 625 : 441 なので、1 サンプルごとに位相を 441 進め、625 を超えたら次のフレームに移る
// (線形補間)。フレームは Machine (記録を当てるコアでは SoundReplayer) が push() で入れる。足りなければ無音にする
class CdMixer {
public:
    static constexpr unsigned SIZE = 2048; // フレーム数 (2 のべき乗)
    void reset() {
        head_ = count_ = 0;
        phase_ = 0;
        prev_[0] = prev_[1] = cur_[0] = cur_[1] = 0;
        out_[0] = out_[1] = outPrev_[0] = outPrev_[1] = 0;
        last_[0] = last_[1] = 0;
    }
    // いっぱいなら捨てる (音を作らない設定で、取り出さないとき)。
    // 鳴らすものがなかったところに入れるときは、位相を 0 に戻す: 前の再生の位相が残っていると、次のフレームが届く前に
    // 取り出して、無音のフレームを 1 つはさむことがある
    void push(int16_t l, int16_t r) {
        if (idle()) phase_ = 0;
        if (count_ >= SIZE) return;
        buf_[(head_ + count_) & (SIZE - 1)] = (uint32_t)(uint16_t)l | (uint32_t)(uint16_t)r << 16;
        count_++;
    }
    // 左と右を別々に受け取るとき (Sound の記録の J_CD): 左を覚えておき、右が来たら 1 フレームにする
    void holdLeft(int16_t l) { left_ = l; }
    void pushRight(int16_t r) {
        last_[0] = left_;
        last_[1] = r;
        push(left_, r);
    }
    // 前のフレームとの差で受け取るとき (Sound の記録の J_CD_DELTA)
    void pushDelta(int dl, int dr) {
        last_[0] = (int16_t)(last_[0] + dl);
        last_[1] = (int16_t)(last_[1] + dr);
        push(last_[0], last_[1]);
    }
    // 記録する側: 前に記録したフレームとの差 (dl、dr) が 14 ビットに入るかを返し、このフレームを覚える。
    // 読み手の pushRight() / pushDelta() と同じ順で呼ばれるので、覚えている値は読み手と同じになる
    bool delta(int16_t l, int16_t r, int& dl, int& dr) {
        dl = l - last_[0];
        dr = r - last_[1];
        last_[0] = l;
        last_[1] = r;
        return dl >= -8192 && dl < 8192 && dr >= -8192 && dr < 8192;
    }
    // 鳴らすものがない (clock() と step() を呼ばなくてよい)。step() で覚えた値が 0 に戻るまでは、鳴らすものがあるとする
    // (再生の終わりの値から 0 までを、出力のフレームに補間して出し切るため)
    bool idle() const {
        return count_ == 0 && (prev_[0] | prev_[1] | cur_[0] | cur_[1] | out_[0] | out_[1] | outPrev_[0] | outPrev_[1]) == 0;
    }
    // OPM の 1 サンプル分進め、その時点の値 (16 ビット) を l と r に足す
    void clock(int32_t& l, int32_t& r) {
        phase_ += 441;
        if (phase_ >= 625) {
            phase_ -= 625;
            prev_[0] = cur_[0];
            prev_[1] = cur_[1];
            if (count_) {
                const uint32_t v = buf_[head_];
                head_ = (head_ + 1) & (SIZE - 1);
                count_--;
                cur_[0] = (int16_t)(uint16_t)v;
                cur_[1] = (int16_t)(uint16_t)(v >> 16);
            } else {
                cur_[0] = cur_[1] = 0;
            }
        }
        l += prev_[0] + (cur_[0] - prev_[0]) * (int32_t)phase_ / 625;
        r += prev_[1] + (cur_[1] - prev_[1]) * (int32_t)phase_ / 625;
    }
    // 出力のフレームに直接足すとき (Sound。X68000 の出力のフィルタを通さない): OPM の 1 サンプル分進め、
    // その時点の値と、1 つ前の時点の値を覚える
    void step() {
        outPrev_[0] = out_[0];
        outPrev_[1] = out_[1];
        out_[0] = out_[1] = 0;
        clock(out_[0], out_[1]);
    }
    // step() の 1 つ前の時点から今の時点までの間の値 (16 ビット)。f は間の位置 (0 から 4095。0 が 1 つ前の時点)
    void between(int32_t f, int32_t& l, int32_t& r) const {
        l = outPrev_[0] + (((out_[0] - outPrev_[0]) * f) >> 12);
        r = outPrev_[1] + (((out_[1] - outPrev_[1]) * f) >> 12);
    }

private:
    uint32_t buf_[SIZE] = {}; // 下位 16 ビットが L、上位が R
    unsigned head_ = 0, count_ = 0;
    uint32_t phase_ = 0;
    int32_t prev_[2] = {}, cur_[2] = {};
    int32_t out_[2] = {}, outPrev_[2] = {}; // step() の時点の値と、その 1 つ前の時点の値
    int16_t left_ = 0;
    int16_t last_[2] = {}; // 前に記録した / 記録から受け取ったフレーム (J_CD_DELTA の差のもと)
};

class CdDrive {
public:
    // オーディオの状態 (READ SUB-CHANNEL のオーディオステータス。SCSI-2 の表 252)
    enum : uint8_t {
        AUDIO_PLAY = 0x11,
        AUDIO_PAUSED = 0x12,
        AUDIO_DONE = 0x13,  // 最後まで再生した
        AUDIO_ERROR = 0x14, // エラーで止まった
        AUDIO_NONE = 0x15,  // 知らせる状態がない
    };
    static constexpr uint32_t FRAMES_PER_SECTOR = 588; // 2352 / 4
    static constexpr uint32_t BUF_SIZE = 1024;         // 99 トラックの TOC (4 + 8 * 100 = 804 バイト) が入る

    void state(StateIO& s); // 状態の保存と復元 (state.cpp)。媒体 (イメージと CdToc) は環境のものなので入れない

    int id() const { return id_; }
    void setId(int id) { id_ = id; }

    // 媒体を入れる (image が nullptr なら、媒体なし)。toc が nullptr なら、イメージの全体をデータトラック 1 つにする。
    // toc は写して持つ。イメージの外を指すトラックは、イメージに入っている分だけにする。
    // changed なら、次のコマンドに UNIT ATTENTION を返す (媒体があるときだけ)
    void setMedium(HardDiskImage* image, const CdToc* toc, bool changed);
    HardDiskImage* image() const { return image_; }
    // 媒体があって、トレイが閉じている
    bool ready() const { return image_ != nullptr && !trayOpen_; }
    const CdToc& toc() const { return toc_; }
    uint32_t blocks() const { return toc_.leadOut; }

    bool trayOpen() const { return trayOpen_; }
    bool prevented() const { return prevent_; }
    void setPrevent(bool on) { prevent_ = on; }
    // トレイを開ける / 閉じる (START STOP UNIT の LoEj)。閉じて媒体があれば、次のコマンドに UNIT ATTENTION を返す
    void setTray(bool open);
    // 次のコマンドに UNIT ATTENTION を返すなら true を返して、消す
    bool takeAttention() {
        const bool a = attention_;
        attention_ = false;
        return a;
    }
    // X68000 のリセット、SCSI バスのリセット: 再生を止め、取り出しの禁止を解き、トレイを閉じ、知らせていない
    // UNIT ATTENTION を消す
    // (CdMixer は Machine が Sound と同じときにリセットする)
    void reset();

    // ---- オーディオ ----
    // [lba, end) を再生する。始まりがオーディオのトラックでなければ false (何も変えない)
    bool play(uint32_t lba, uint32_t end);
    // 再生を止める (状態は AUDIO_NONE)
    void stop();
    // 一時停止 / 再開。再生中でも一時停止中でもなければ false
    bool pause(bool resume);
    bool playing() const { return audio_ == AUDIO_PLAY; }
    uint8_t audioStatus() const { return audio_; }
    // READ SUB-CHANNEL が状態を返したあと: 終わった / エラーで止まった、は 1 回だけ返す
    void audioStatusRead() {
        if (audio_ == AUDIO_DONE || audio_ == AUDIO_ERROR) audio_ = AUDIO_NONE;
    }
    // 再生がエラーで止まったら、次のコマンド (REQUEST SENSE と INQUIRY のほか) に 1 回だけ CHECK CONDITION を返す
    // (SCSI-2 の 14.1.3)。そのセンスキーと追加センスコードを返して、消す。なければ false
    bool takeDeferred(uint8_t& key, uint8_t& asc) {
        if (!deferKey_) return false;
        key = deferKey_;
        asc = deferAsc_;
        deferKey_ = deferAsc_ = 0;
        return true;
    }
    // 今の位置 (論理ブロック)。再生していないときは、最後に読んだ / シークした位置
    uint32_t position() const { return pos_; }
    void setPosition(uint32_t lba) { pos_ = lba; }
    // 4MHz のクロックで clocks だけ進め、その間のフレームを out (L、R の順に max フレームまで) に入れて数を返す。
    // max フレームを超える分は次に呼んだときに返す (clocks = 0 で続きを取り出せる)。再生していなければ 0
    int produce(uint32_t clocks, int16_t* out, int max);

    // TOC などの応答を置く (Spc の small_ に入らない大きさのもの)
    uint8_t* buf() { return buf_; }
    CdMixer& mixer() { return mixer_; }

private:
    bool openRun();
    void fail(uint8_t key, uint8_t asc) {
        stop();
        audio_ = AUDIO_ERROR;
        deferKey_ = key;
        deferAsc_ = asc;
    }

    int id_ = -1;
    HardDiskImage* image_ = nullptr;
    CdToc toc_;
    bool trayOpen_ = false;
    bool prevent_ = false;
    bool attention_ = false;
    uint8_t audio_ = AUDIO_NONE;
    uint8_t deferKey_ = 0, deferAsc_ = 0; // 再生が止まったエラー (まだ知らせていない)
    uint32_t pos_ = 0;    // 今の論理ブロック
    uint32_t end_ = 0;    // 再生の終わり (この論理ブロックの前まで)
    uint32_t frame_ = 0;  // 今の論理ブロックの中のフレーム (0..587)
    uint32_t acc_ = 0;    // クロックの端数 (4MHz のクロック x 441 を 40000 で割った余り)
    uint32_t owed_ = 0;   // 進めたがまだ取り出していないフレーム数
    // 今読んでいる続きの範囲 (トラックの中の、イメージに入っている部分): [runLba_, runEnd_)
    DiskCursor cursor_;
    uint32_t runLba_ = 0, runEnd_ = 0;
    bool runSilent_ = false; // この範囲はイメージにない (無音)
    bool runSwap_ = false;
    uint8_t buf_[BUF_SIZE] = {};
    CdMixer mixer_;
};

} // namespace x68k
