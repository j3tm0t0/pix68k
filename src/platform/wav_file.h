// 16 ビットのステレオの WAV ファイルを書く (ホスト版とテスト用。stdio を使うのでベアメタルでは使わない)
#pragma once

#include <stdint.h>
#include <stdio.h>

namespace platform {

class WavFile {
public:
    ~WavFile() { close(); }
    bool open(const char* path, int rate) {
        f_ = fopen(path, "wb");
        if (!f_) return false;
        rate_ = rate;
        frames_ = 0;
        writeHeader();
        return true;
    }
    void write(const int16_t* frames, int count) {
        if (!f_ || count <= 0) return;
        // WAV はリトルエンディアン
        for (int i = 0; i < count * 2; i++) {
            uint16_t v = (uint16_t)frames[i];
            uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
            fwrite(b, 1, 2, f_);
        }
        frames_ += (uint32_t)count;
    }
    void close() {
        if (!f_) return;
        fseek(f_, 0, SEEK_SET);
        writeHeader(); // 大きさを書き直す
        fclose(f_);
        f_ = nullptr;
    }
    uint32_t frames() const { return frames_; }

private:
    void put32(uint32_t v) {
        uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
        fwrite(b, 1, 4, f_);
    }
    void put16(uint16_t v) {
        uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
        fwrite(b, 1, 2, f_);
    }
    void writeHeader() {
        uint32_t data = frames_ * 4;
        fwrite("RIFF", 1, 4, f_);
        put32(36 + data);
        fwrite("WAVEfmt ", 1, 8, f_);
        put32(16);
        put16(1); // PCM
        put16(2);
        put32((uint32_t)rate_);
        put32((uint32_t)rate_ * 4);
        put16(4);
        put16(16);
        fwrite("data", 1, 4, f_);
        put32(data);
    }

    FILE* f_ = nullptr;
    int rate_ = 48000;
    uint32_t frames_ = 0;
};

} // namespace platform
