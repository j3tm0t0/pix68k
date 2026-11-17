// ベアメタル版の USB の入力 (マウス、ゲームパッド) を platform::InputSink の形に直す道具。
// Circle に依存しないので、ホストの単体テスト (tests/unit/input_map_test.cpp) で確かめられる。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stdint.h>

#include "platform/platform.h"

namespace platform {

// USB マウスのレポートを溜め、送れるときに送る。送り先 (コア間のキュー) が満杯でも、移動量は足し合わせて
// 次に送り、押したボタンは少なくとも 1 回は押したと送る (押してすぐ離したクリックを落とさない)。
// emit(dx, dy, buttons) は送れたら true を返す。1 回に送る移動量は ±limit まで (残りは次に送る)
class MouseTracker {
public:
    explicit MouseTracker(int limit = 2047) : limit_(limit) {}
    void reset() {
        dx_ = dy_ = 0;
        buttons_ = latched_ = 0;
        dirty_ = false;
    }
    // レポートを受け取り、送れるだけ送る
    template <typename Emit>
    bool report(int dx, int dy, uint8_t buttons, Emit&& emit) {
        // 足す前に切り詰める (int の範囲を超えて足さないように)
        dx_ = clampAcc(dx_ + clampAcc(dx));
        dy_ = clampAcc(dy_ + clampAcc(dy));
        latched_ |= (uint8_t)(buttons & ~buttons_);
        buttons_ = buttons;
        dirty_ = true;
        return flush(emit);
    }
    // ボタンをすべて離したことにする (マウスが外れたとき)
    template <typename Emit>
    bool releaseAll(Emit&& emit) {
        return report(0, 0, 0, emit);
    }
    // まだ送れていないものを送る。すべて送れたら true
    template <typename Emit>
    bool flush(Emit&& emit) {
        for (int guard = 0; dirty_ && guard < 64; guard++) {
            int sx = clamp(dx_), sy = clamp(dy_);
            uint8_t b = (uint8_t)(buttons_ | latched_);
            if (!emit(sx, sy, b)) return false;
            dx_ -= sx;
            dy_ -= sy;
            bool sentLatched = latched_ != 0 && b != buttons_;
            latched_ = 0;
            // 押したことにして送ったボタンをもう離していれば、離したことも送る
            dirty_ = sentLatched || dx_ != 0 || dy_ != 0;
        }
        return !dirty_;
    }
    bool pending() const { return dirty_; }

private:
    int clamp(int v) const { return v > limit_ ? limit_ : v < -limit_ ? -limit_ : v; }
    static int clampAcc(int v) { return v > (1 << 20) ? (1 << 20) : v < -(1 << 20) ? -(1 << 20) : v; }
    int limit_;
    int dx_ = 0, dy_ = 0;
    uint8_t buttons_ = 0, latched_ = 0;
    bool dirty_ = false;
};

// USB ゲームパッドの状態 → ジョイスティックのビット (InputSink::JOY_*)
struct GamePadInput {
    // known: Circle が配置を知っているゲームパッド (GamePadPropertyIsKnown)。そのときの buttons のビットは
    // Circle の TGamePadButton (usbgamepad.h): Y bit7、B bit8、A bit9、X bit10、上 bit15、右 bit16、下 bit17、左 bit18
    bool known = false;
    unsigned buttons = 0;
    int nhats = 0;
    int hat = 8;   // 最初のハット: 0 上、1 右上、2 右 ... 7 左上 (時計回り)。それ以外は離している
    int naxes = 0; // 最初の 2 本 (X、Y) だけ見る
    int value[2] = {0, 0}, minimum[2] = {0, 0}, maximum[2] = {255, 255};
};

// USB のコンフィグレーション記述子 (全体) から、インタフェース iface の HID 記述子 (0x21) を探し、
// レポート記述子 (種類 0x22) の種類と長さを読む。HID 記述子には bNumDescriptors (5 バイト目) 個の
// 「種類 1 バイト + 長さ 2 バイト」が並ぶので、その中からレポート記述子を探す (bLength に収まる分だけ見る)。
// 見つかれば true。インタフェースの HID 記述子に 0x22 がなければ、その場で false
inline bool hidReportDescriptorInfo(const uint8_t* config, unsigned total, int iface, uint8_t& type, unsigned& length) {
    bool inIface = false;
    for (unsigned i = 0; i + 2 <= total;) {
        const unsigned len = config[i];
        const uint8_t kind = config[i + 1];
        if (len < 2 || i + len > total) return false;
        if (kind == 0x04 && len >= 9) { // インタフェース記述子 (代替設定 0 だけ見る)
            inIface = config[i + 2] == iface && config[i + 3] == 0;
        } else if (kind == 0x21 && inIface && len >= 6) { // HID 記述子
            const unsigned count = config[i + 5];
            for (unsigned k = 0; k < count && 6 + 3 * k + 3 <= len; k++) {
                const unsigned at = i + 6 + 3 * k;
                if (config[at] != 0x22) continue;
                type = config[at];
                length = (unsigned)config[at + 1] | (unsigned)config[at + 2] << 8;
                return length != 0;
            }
            return false;
        }
        i += len;
    }
    return false;
}

// USB のコンフィグレーション記述子 (全体) から、インタフェース iface の HID 記述子の bCountryCode を読む。
// 見つかれば true (値が 0 なら、キーボードが国を示していない)
inline bool hidCountryCode(const uint8_t* config, unsigned total, int iface, uint8_t& country) {
    bool inIface = false;
    for (unsigned i = 0; i + 2 <= total;) {
        const unsigned len = config[i];
        const uint8_t kind = config[i + 1];
        if (len < 2 || i + len > total) return false;
        if (kind == 0x04 && len >= 9) {
            inIface = config[i + 2] == iface && config[i + 3] == 0;
        } else if (kind == 0x21 && inIface && len >= 6) {
            country = config[i + 4];
            return true;
        }
        i += len;
    }
    return false;
}

// USB HID のレポート記述子から、ゲームパッドの最初のハットの論理最小値と論理最大値を読む。
// Circle の CUSBGamePadStandardDevice::DecodeReport() と同じ順に項目を見る (ジョイスティックかゲームパッドの
// 用途のあとに出てくる最初のハット。ボタンの Usage Page のあとの用途はハットとみなさない)。
// さらに Usage Page が Generic Desktop のときだけハットとみなし、論理範囲が 8 方向 (最大 - 最小 = 7) で
// なければ使わない。見つかれば true
inline bool hidHatLogicalRange(const uint8_t* desc, unsigned len, int& lmin, int& lmax) {
    enum { NONE, PAD, BUTTON, HAT, AXIS } state = NONE;
    const int UNDEF = -123456789;
    int page = 0, mn = UNDEF, mx = UNDEF, pmn = UNDEF, pmx = UNDEF;
    unsigned i = 0;
    while (i < len) {
        const uint8_t item = desc[i++];
        if (item == 0xfe) return false; // 長い項目 (ゲームパッドでは使わない)
        static const unsigned kSize[4] = {0, 1, 2, 4};
        const unsigned n = kSize[item & 3];
        if (i + n > len) return false;
        int32_t arg = 0;
        if (n == 1) arg = (int8_t)desc[i];
        else if (n == 2) arg = (int16_t)(desc[i] | desc[i + 1] << 8);
        else if (n == 4) arg = (int32_t)((uint32_t)desc[i] | (uint32_t)desc[i + 1] << 8 | (uint32_t)desc[i + 2] << 16 |
                                         (uint32_t)desc[i + 3] << 24);
        i += n;
        switch (item & 0xfc) {
        case 0x04: // Usage Page
            page = arg & 0xffff;
            if (page == 0x09 && state == PAD) state = BUTTON;
            break;
        case 0x08: // Usage
            if (page != 0x01) break;
            if (arg == 0x04 || arg == 0x05) state = PAD;                       // Joystick、Game Pad
            else if (arg == 0x39 && state == PAD) state = HAT;                   // Hat switch
            else if (arg >= 0x30 && arg <= 0x36 && state == PAD) state = AXIS; // 軸
            break;
        case 0x14: mn = arg; break;  // Logical Minimum
        case 0x24: mx = arg; break;  // Logical Maximum
        case 0x34: pmn = arg; break; // Physical Minimum
        case 0x44: pmx = arg; break; // Physical Maximum
        case 0x80: // Input
            if ((arg & 3) == 2) {
                if (state == HAT) {
                    lmin = mn != UNDEF ? mn : pmn;
                    lmax = mx != UNDEF ? mx : pmx;
                    // 差は 64 ビットで計算する (4 バイトの値で int があふれないように)
                    return lmin != UNDEF && lmax != UNDEF && (int64_t)lmax - (int64_t)lmin == 7;
                }
                if (state == AXIS || state == BUTTON) state = PAD;
            }
            break;
        default:
            break;
        }
    }
    return false;
}

// USB ゲームパッドのハットの値を GamePadInput::hat の形 (0 上 ... 7 左上、8 離している) に直す。
// Circle (usbgamepadstandard.cpp) はハットの値から論理最小値を引かずに渡すので、論理範囲が 1-8 の機種は
// 離していると 0 を返し、そのままでは「上」に見える (押した向きも 45 度ずれる)。
// 論理最小値が分かっていれば (レポート記述子を読めた、設定で決めた、Circle が配置を知っている機種) reset(min) で決める。
// 分からなければ (reset(-1)) 値から決める:
//   - 0 と 8 のうち先に来たほうを離した値とみなす: 8 なら 0-7 の機種、0 なら 1-8 の機種
//     (見誤るのは、押したままつないだとき。それまでの 1-7 は 0-7 の機種として読む)
//   - 9 以上の値が来たら 0-7 の機種に直す (1-8 の機種は 0-8 しか返さない)
class HatDecoder {
public:
    // base: 論理最小値 (0 か 1)。-1 なら値から決める
    void reset(int base = -1) {
        fixed_ = base == 0 || base == 1;
        base_ = fixed_ ? base : -1;
    }
    int decode(int raw) {
        if (!fixed_) {
            if (raw >= 9) base_ = 0;
            else if (base_ < 0 && raw == 8) base_ = 0;
            else if (base_ < 0 && raw == 0) base_ = 1;
        }
        int v = raw - (base_ < 0 ? 0 : base_);
        return v >= 0 && v <= 7 ? v : 8;
    }
    int base() const { return base_; } // -1 はまだ決まっていない

private:
    bool fixed_ = false;
    int base_ = -1;
};

inline uint8_t joystickBits(const GamePadInput& in) {
    typedef InputSink S;
    uint8_t v = 0;
    if (in.known) {
        if (in.buttons & (1u << 15)) v |= S::JOY_UP;
        if (in.buttons & (1u << 16)) v |= S::JOY_RIGHT;
        if (in.buttons & (1u << 17)) v |= S::JOY_DOWN;
        if (in.buttons & (1u << 18)) v |= S::JOY_LEFT;
        if (in.buttons & ((1u << 9) | (1u << 10))) v |= S::JOY_A; // A、X
        if (in.buttons & ((1u << 8) | (1u << 7))) v |= S::JOY_B;  // B、Y
    } else {
        // 配置が分からないときは、1 番目のボタンをトリガ A、2 番目を B にする
        if (in.buttons & 1u) v |= S::JOY_A;
        if (in.buttons & 2u) v |= S::JOY_B;
    }
    if (in.nhats > 0 && in.hat >= 0 && in.hat <= 7) {
        static const uint8_t kHat[8] = {
            S::JOY_UP, S::JOY_UP | S::JOY_RIGHT, S::JOY_RIGHT, S::JOY_RIGHT | S::JOY_DOWN,
            S::JOY_DOWN, S::JOY_DOWN | S::JOY_LEFT, S::JOY_LEFT, S::JOY_LEFT | S::JOY_UP,
        };
        v |= kHat[in.hat];
    }
    // アナログの軸: 真ん中から振れ幅の 40% までは遊び
    for (int a = 0; a < 2 && a < in.naxes; a++) {
        // 64 ビットで計算する (最小値と最大値が int の端に近い機種でもあふれないように)
        const int64_t lo = in.minimum[a], hi = in.maximum[a];
        if (hi <= lo) continue;
        const int64_t center = lo + (hi - lo) / 2;
        const int64_t dead = (hi - lo) * 2 / 10; // 半分の幅の 40%
        const int64_t d = (int64_t)in.value[a] - center;
        if (d < -dead) v |= a == 0 ? S::JOY_LEFT : S::JOY_UP;
        if (d > dead) v |= a == 0 ? S::JOY_RIGHT : S::JOY_DOWN;
    }
    return v;
}

// USB キーボードの LED を送る。送れなかったら、間を空けて (1 回目は retryTicks、そのあと 2 倍ずつ)
// MAX_TRIES 回まで送り直す。LED の値が変わったら、すぐに送る。now と retryTicks の単位は呼ぶ側が決める
class LedSender {
public:
    static constexpr int MAX_TRIES = 6; // 最初の 1 回を含む

    explicit LedSender(uint64_t retryTicks) : retryTicks_(retryTicks) {}
    void reset() {
        sent_ = -1;
        want_ = -1;
        tries_ = 0;
        next_ = 0;
    }
    enum Result { IDLE, SENT, FAILED };
    // send(leds) は送れたら true を返す。送ろうとしなければ IDLE
    template <typename Send>
    Result update(int want, uint64_t now, Send&& send) {
        if (want != want_) {
            // 頼まれた値が変わった: 失敗した回数を数え直し、すぐ送る
            want_ = want;
            tries_ = 0;
            next_ = now;
        }
        if (want == sent_) return IDLE;
        if (tries_ >= MAX_TRIES || now < next_) return IDLE;
        if (send(want)) {
            sent_ = want;
            tries_ = 0;
            return SENT;
        }
        next_ = now + (retryTicks_ << (tries_ < 16 ? tries_ : 16));
        tries_++;
        return FAILED;
    }
    // 今の値を送ろうとして失敗した回数 (1 なら初めて失敗した、MAX_TRIES ならもう送らない)
    int tries() const { return tries_; }

private:
    uint64_t retryTicks_;
    int sent_ = -1; // 送れた値 (-1 はまだ送っていない)
    int want_ = -1; // 前に呼ばれたときに頼まれた値
    int tries_ = 0;
    uint64_t next_ = 0;
};

} // namespace platform
