// まーきゅりーゆにっとの YMF288 のリズム音源の音を、使う人が用意した WAV ファイルから作る。
//
// YMF288 のリズムの音 (バスドラム、スネアドラム、シンバル、ハイハット、タム、リムショット) はチップの中の ROM にあり、
// pix68k には入っていない。ほかの OPNA のプレーヤーで使われている 6 つの WAV ファイル (kOpnRhythmFiles の名前) を
// 置いてもらい、ここで 16 ビットのモノラルに直して x68k::Opn3::setRhythm() に渡す。なければリズムは鳴らない。
// 読めるのは、圧縮していない PCM の 16 ビット (モノラルかステレオ。ステレオは左右を足して半分にする)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "x68k/opn3.h"

namespace platform {

// $10 の bit0 から順に: BD、SD、TOP (シンバル)、HH、TOM、RIM
static const char* const kOpnRhythmFiles[x68k::Opn3::RHYTHMS] = {
    "2608_BD.WAV", "2608_SD.WAV", "2608_TOP.WAV", "2608_HH.WAV", "2608_TOM.WAV", "2608_RIM.WAV",
};

// WAV ファイルの中身 (file、size バイト) を、その場で 16 ビットのモノラルに直す。直した音は file の先頭から入る
// (file は 2 バイトの境界にそろっていること)。frames にサンプル数、rate にサンプリング周波数を返す。
// 読めない形 (PCM の 16 ビットでない、チャンクが壊れている) なら false
inline bool opnRhythmFromWav(uint8_t* file, size_t size, uint32_t& frames, uint32_t& rate) {
    const auto le16 = [](const uint8_t* p) { return (uint32_t)(p[0] | p[1] << 8); };
    const auto le32 = [](const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; };
    if (size < 12 || memcmp(file, "RIFF", 4) != 0 || memcmp(file + 8, "WAVE", 4) != 0) return false;
    uint32_t channels = 0, bits = 0, format = 0;
    rate = 0;
    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint32_t len = le32(file + pos + 4);
        const size_t body = pos + 8;
        if (len > size - body) return false;
        if (memcmp(file + pos, "fmt ", 4) == 0) {
            if (len < 16) return false;
            format = le16(file + body);
            channels = le16(file + body + 2);
            rate = le32(file + body + 4);
            bits = le16(file + body + 14);
        } else if (memcmp(file + pos, "data", 4) == 0) {
            if (format != 1 || bits != 16 || (channels != 1 && channels != 2) || rate < 1000 || rate > 192000) return false;
            const uint32_t n = len / (2 * channels);
            // 出力は入力より前にあり、同じか小さいので、前から順に書けば読む前のデータを壊さない
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t* p = file + body + (size_t)i * 2 * channels;
                int32_t v = (int16_t)le16(p);
                if (channels == 2) v = (v + (int16_t)le16(p + 2)) / 2;
                const int16_t s = (int16_t)v;
                memcpy(file + (size_t)i * 2, &s, 2);
            }
            frames = n;
            return n > 0;
        }
        pos = body + len + (len & 1);
    }
    return false;
}

// 6 つの音をまとめて持つ。data[i] は、opnRhythmFromWav() で直したあとのバッファ (持ち主は呼ぶ側。消さないこと)
struct OpnRhythm {
    x68k::Opn3::RhythmSet set = {};
    // i 番目の音を入れる (file は読んだファイルの中身)。読めなければ false で、その音は鳴らない
    bool add(int i, uint8_t* file, size_t size) {
        uint32_t frames = 0, rate = 0;
        if (!file || !opnRhythmFromWav(file, size, frames, rate)) return false;
        set.pcm[i] = (const int16_t*)(const void*)file;
        set.length[i] = frames;
        set.rate[i] = rate;
        return true;
    }
};

} // namespace platform
