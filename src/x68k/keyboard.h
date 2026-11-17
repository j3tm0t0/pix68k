// X68000 のキーボード。USB HID のキー (usage ID) を X68000 のキーコードに変え、
// MFP の USART に 1 バイトずつ送る。
//
// キーコード: 押したときは code ($01-$7F)、離したときは code | $80。
// 送る速さは本物と同じ 2400bps (1 バイト = スタート、データ 8 ビット、ストップの 10 ビット、約 4.2ms)。
// MFP の受信バッファが空き、受信が許可 (RSR bit0) されているときだけ次のバイトを渡す。
// キーリピートはキーボード側でする (本物と同じく、最後に押したキーだけ)。
//
// 本体からキーボードへのコマンド (MFP の送信データ):
//   $40-$47  マウスの制御信号 (MSCTRL)。無視する
//   $48/$49  キーデータの送出を禁止 / 許可
//   $50-$5F  X1 互換モード、LED の明るさ、テレビの制御など。無視する
//   $60-$6F  リピートが始まるまでの時間 = 200 + 100 * n ms (既定 500ms)
//   $70-$7F  リピートの間隔 = 30 + 5 * n * n ms (既定 110ms)
//   $80-$FF  LED (bit0 かな、1 ローマ字、2 コード入力、3 CAPS、4 INS、5 ひらがな、6 全角。0 で点灯)。
//            leds() で読める。USB キーボードの LED には hidLeds() で直して出す
//
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stdint.h>

#include "platform/platform.h"
#include "x68k/mfp.h"

namespace x68k {

class StateIO;


class Keyboard : public platform::InputSink {
public:
    void state(StateIO& s); // 状態の保存と復元 (x68k/state.h、state.cpp)
    // キューの大きさ (2 のべき乗)
    static constexpr int QUEUE_SIZE = 64;
    // 受信バッファが読まれないとき、上書きするまで待つ時間
    static constexpr uint32_t OVERRUN_MS = 100;

    // X68000 のキーコード。メーカーの資料では確かめていない。文字と SHIFT と RETURN は ctest (x68k_boot_type_ascii1 / 2、
    // x68k_boot_type_dir) で、本物の IPL ROM と Human68k にそのキーとして届くことを確かめている。CTRL、ESC、↑、↓ は
    // boot_ipl.cpp の --type で打てるが、結果を調べる試験はない。ほかのキーは確かめていない
    enum Code : uint8_t {
        K_ESC = 0x01, K_BS = 0x0f, K_TAB = 0x10, K_RETURN = 0x1d, K_SPACE = 0x35,
        K_CAPS = 0x5d, K_SHIFT = 0x70, K_CTRL = 0x71, K_OPT1 = 0x72, K_OPT2 = 0x73,
    };

    explicit Keyboard(uint32_t cpuHz = 16000000) { setClock(cpuHz); reset(); }
    // 時間の単位 (CPU のクロック)
    void setClock(uint32_t cpuHz);
    // 電源投入の状態: キュー、押しているキー、リピートの設定を初期化する
    void reset();

    // USB HID の usage ID を X68000 のキーコードに変える。対応するキーがなければ 0
    static uint8_t fromHid(uint8_t usage);

    // ---- platform::InputSink ----
    // ホストのキーが押された / 離された (usage は USB HID の usage ID)
    void onKey(uint8_t usage, bool down) override;

    // X68000 のキーコードで押す / 離す (同じコードを複数の物理キーが共有してもよい。
    // 押している数を数え、最初の押下で make、最後の解放で break を送る)
    void press(uint8_t code, bool down);

    // ASCII の 1 文字を打つ (JIS 配列。必要なら SHIFT も押す)。打てない文字なら false
    bool typeAscii(char c);
    // ASCII の文字を X68000 のキーコードに変える。shift には SHIFT が要るかを返す。打てなければ 0
    static uint8_t fromAscii(char c, bool& shift);

    // now (CPU サイクル) まで進める: 本体からのコマンドを受け、リピートを起こし、
    // 送れる時刻で受信バッファが空いていれば次のバイトを MFP に渡す。
    // 周辺機器を進めるたびに呼ぶので、することがなければ時刻を覚えるだけで戻る (この判断は updateSlow() と同じ)
    void update(uint64_t now, Mfp& mfp) {
        lastNow_ = now;
        if (!mfp.txPending() && !anyPending_ && !(repeatCode_ && now >= nextRepeat_) &&
            (now < nextSend_ || queued() == 0 || !enabled_))
            return;
        updateSlow(now, mfp);
    }

    // 状態 (テスト、表示用)
    int queued() const { return (int)((wp_ - rp_) & (QUEUE_SIZE - 1)); }
    bool enabled() const { return enabled_; }
    // LED の状態 (本体から受けた $80-$FF の値。0 のビットが点灯)
    uint8_t leds() const { return leds_; }
    // LED のビット (leds() で 0 なら点灯)
    enum Led : uint8_t {
        LED_KANA = 0x01, LED_ROMAJI = 0x02, LED_CODE = 0x04, LED_CAPS = 0x08, LED_INS = 0x10,
        LED_HIRAGANA = 0x20, LED_ZENKAKU = 0x40,
    };
    // USB キーボードの LED (HID の LED レポート: bit0 Num Lock、bit1 Caps Lock、bit2 Scroll Lock) に直す。
    // CAPS → Caps Lock、かな → Scroll Lock、INS → Num Lock。ほかの LED は USB キーボードにないので出さない
    static uint8_t hidLeds(uint8_t x68kLeds);
    uint32_t repeatDelayMs() const { return delayMs_; }
    uint32_t repeatIntervalMs() const { return intervalMs_; }
    // 1 バイトを送る間隔 (CPU サイクル)
    uint64_t byteCycles() const { return byteCycles_; }
    // 本体からのコマンド 1 バイトを処理する
    void command(uint8_t v);

private:
    bool push(uint8_t v);
    void flushPendingBreaks();
    int freeSlots() const { return QUEUE_SIZE - 1 - queued(); }
    bool repeatable(uint8_t code) const;
    void updateSlow(uint64_t now, Mfp& mfp); // update() の中身
    uint64_t msToCycles(uint32_t ms) const { return (uint64_t)cpuHz_ * ms / 1000; }

    uint32_t cpuHz_ = 16000000;
    uint64_t byteCycles_ = 0;
    uint8_t queue_[QUEUE_SIZE];
    uint32_t wp_ = 0, rp_ = 0;
    uint8_t hidDown_[32];     // ホストで押しているキー (usage ごとに 1 ビット)
    uint8_t count_[128];      // X68000 のキーごとの、押している物理キーの数
    uint8_t pendingBreak_[16]; // キューが満ちて送れなかった break (コードごとに 1 ビット)
    bool anyPending_ = false;
    bool enabled_ = true;
    uint8_t leds_ = 0xff;
    uint32_t delayMs_ = 500, intervalMs_ = 110;
    uint8_t repeatCode_ = 0;   // リピートするキー (0 ならなし)
    uint64_t nextRepeat_ = 0;
    uint64_t nextSend_ = 0;
    uint64_t lastNow_ = 0;
};

} // namespace x68k
