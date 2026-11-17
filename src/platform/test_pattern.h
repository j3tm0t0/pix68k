// M0 の動作確認用: 画面にカラーバーを描き、最後に押されたキーの usage ID を左上に 16 進で表示する。
#pragma once

#include <stdint.h>

namespace platform {

// 768x512 (X68000 の 31kHz 標準の最大解像度) の XRGB8888 画像
class TestPattern {
public:
    static constexpr int kWidth = 768;
    static constexpr int kHeight = 512;
    uint32_t pixels[kWidth * kHeight];

    void draw(uint8_t lastKey, int frame) {
        for (int y = 0; y < kHeight; y++) {
            for (int x = 0; x < kWidth; x++) {
                uint32_t c;
                int band = x * 8 / kWidth; // 縦 8 本のカラーバー
                if (y < kHeight * 3 / 4) {
                    static const uint32_t bars[8] = {0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
                                                     0xff00ff, 0xff0000, 0x0000ff, 0x000000};
                    c = bars[band];
                } else {
                    c = (uint32_t)((x + frame) & 0xff) * 0x010101; // 下は流れるグレースケール
                }
                pixels[y * kWidth + x] = c;
            }
        }
        // 最後に押されたキーの usage ID を 16 進 2 桁で表示する
        drawHex(16, 16, lastKey);
    }

private:
    // 5x7 ドットの 16 進数字を 4 倍に拡大して描く
    void drawHex(int x0, int y0, uint8_t v) {
        static const uint8_t font[16][7] = {
            {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}, {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},
            {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}, {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e},
            {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}, {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e},
            {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}, {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
            {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}, {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c},
            {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e},
            {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}, {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e},
            {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}, {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10},
        };
        for (int d = 0; d < 2; d++) {
            const uint8_t* g = font[d == 0 ? v >> 4 : v & 15];
            for (int row = 0; row < 7; row++)
                for (int col = 0; col < 5; col++) {
                    uint32_t c = (g[row] >> (4 - col)) & 1 ? 0x000000 : 0xffffff;
                    for (int dy = 0; dy < 4; dy++)
                        for (int dx = 0; dx < 4; dx++)
                            pixels[(y0 + row * 4 + dy) * kWidth + x0 + d * 24 + col * 4 + dx] = c;
                }
        }
    }
};

} // namespace platform
