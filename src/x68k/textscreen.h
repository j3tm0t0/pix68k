// テキスト画面を ASCII の文字列として読む (テストとデバッグ用)。
// 8x16 の升目ごとに、4 プレーンの OR を字の形とみなして CGROM の半角の字形 (8x16) と比べる。
// 読めない升目は '?'、何もない升目は ' '。スクロールは考えない (テキスト VRAM の左上から読む)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stdint.h>

#include "x68k/video.h"

namespace x68k {

static constexpr int TEXT_COLS = 96, TEXT_ROWS = 32;

// cgrom は CGROM の全体 (768KB)。out の各行は行末の空白を除いて 0 で終わる
void readTextScreen(const Video& video, const uint8_t* cgrom, char out[TEXT_ROWS][TEXT_COLS + 1]);

} // namespace x68k
