#include "x68k/textscreen.h"

#include <string.h>

namespace x68k {

namespace {
const uint32_t kFont8x16 = 0x3a800; // CGROM の 8x16 の半角 ($F3A800)
}

void readTextScreen(const Video& video, const uint8_t* cg, char out[TEXT_ROWS][TEXT_COLS + 1]) {
    const uint8_t* t = video.tvram();
    for (int r = 0; r < TEXT_ROWS; r++) {
        for (int c = 0; c < TEXT_COLS; c++) {
            uint8_t cell[16];
            bool blank = true;
            for (int y = 0; y < 16; y++) {
                uint32_t o = (uint32_t)(r * 16 + y) * 128 + (uint32_t)c;
                cell[y] = (uint8_t)(t[o] | t[0x20000 + o] | t[0x40000 + o] | t[0x60000 + o]);
                if (cell[y]) blank = false;
            }
            char ch = blank ? ' ' : '?';
            for (int g = 0x21; !blank && g < 0x7f; g++) {
                if (!memcmp(cell, cg + kFont8x16 + g * 16, 16)) {
                    ch = (char)g;
                    break;
                }
            }
            out[r][c] = ch;
        }
        int end = TEXT_COLS;
        while (end > 0 && out[r][end - 1] == ' ') end--;
        out[r][end] = 0;
    }
}

} // namespace x68k
