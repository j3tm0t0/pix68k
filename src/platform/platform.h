// エミュレータ本体と、動かす環境 (Mac の SDL2 版 / Raspberry Pi のベアメタル版) の境目。
// エミュレータ本体はこのインタフェースだけを使い、環境ごとの実装を差し替える。
// ベアメタル (Circle、C++ 標準ライブラリなし) でも使えるよう、STL は使わない。
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace platform {

// 画面: エミュレータが描いた XRGB8888 の画像を表示する
class Video {
public:
    virtual ~Video() {}
    // pixels は width x height、1 行 pitch 画素
    virtual void present(const uint32_t* pixels, int width, int height, int pitch) = 0;
};

// 入力: キーは USB HID の usage ID (キーボード/キーパッドのページ) で渡す。
// SDL の scancode と Circle の生キーコードはどちらもこの値なので、変換せずに使える。
// マウスとジョイスティックは、受け取らない実装 (キーボードだけのものなど) のために既定では何もしない。
class InputSink {
public:
    // onMouse() の buttons (今押しているボタン)
    enum MouseButton : uint8_t { MOUSE_LEFT = 0x01, MOUSE_RIGHT = 0x02, MOUSE_MIDDLE = 0x04 };
    // onJoystick() の bits (押していれば 1)。A / B は ATARI 規格のトリガ A / B
    enum JoyBit : uint8_t {
        JOY_UP = 0x01, JOY_DOWN = 0x02, JOY_LEFT = 0x04, JOY_RIGHT = 0x08, JOY_A = 0x10, JOY_B = 0x20,
    };

    virtual ~InputSink() {}
    virtual void onKey(uint8_t usage, bool down) = 0;
    // マウスが動いた / ボタンが変わった。dx、dy は前回からの移動量 (右と下が正)
    virtual void onMouse(int dx, int dy, uint8_t buttons) {
        (void)dx;
        (void)dy;
        (void)buttons;
    }
    // ジョイスティックの状態が変わった (port は 0 か 1)
    virtual void onJoystick(int port, uint8_t bits) {
        (void)port;
        (void)bits;
    }
};

// ファイル: ROM やディスクイメージを読む。path は環境ごとの置き場所からの相対パス
// (ホスト版はコマンドラインで指定したディレクトリ、ベアメタル版は SD カードの /pix68k)
class Storage {
public:
    virtual ~Storage() {}
    // ファイルの大きさを返す。ないときは -1
    virtual long fileSize(const char* path) = 0;
    // 先頭から size バイト読む。読めたら true
    virtual bool readFile(const char* path, void* buffer, size_t size) = 0;
    virtual bool writeFile(const char* path, const void* buffer, size_t size) = 0;
};

// 音: エミュレータは int16 のステレオのフレーム (L, R の順に交互に並べたもの) を作ってためておき、
// 音を出す側 (SDL のコールバック、Circle の音のデバイスの割り込みなど) が取り出す。
// エミュレータとは別のスレッドやコアから呼んでよい (読み手は 1 つだけ)
class AudioSource {
public:
    virtual ~AudioSource() {}
    // out に frames フレームを必ず書く。たまっている分が足りなければ、残りは直前の値から無音へ向かう値で埋める
    virtual void readAudio(int16_t* out, int frames) = 0;
};

// 時刻とログ
class System {
public:
    virtual ~System() {}
    virtual uint64_t microseconds() = 0;
    virtual void log(const char* message) = 0;
};

} // namespace platform
