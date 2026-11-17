// 試験用の小さな SoundFont 2 (SF2) をメモリの上に作る。SF2 のファイルはリポジトリに入れないので、テストとベンチマークは
// これを使う (ベンチマークは、SF2 のファイルを渡せばそちらを使う)。
// 中身: 正弦波の標本 1 つ (44.1kHz で 441Hz、44 周期、全体をループ、元の音は A4 = 69) を使う楽器 1 つと、
// それを使うプリセット 2 つ (バンク 0 のプログラム 0 と、ドラムのバンク 128 のプログラム 0)。
#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <vector>

namespace sf2test {

class Writer {
public:
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) {
        u8((uint8_t)v);
        u8((uint8_t)(v >> 8));
    }
    void u32(uint32_t v) {
        u16((uint16_t)v);
        u16((uint16_t)(v >> 16));
    }
    void id(const char* s) { b.insert(b.end(), s, s + 4); }
    void name(const char* s) {
        char n[20] = {};
        strncpy(n, s, 19);
        b.insert(b.end(), n, n + 20);
    }
    // 大きさを後で書く chunk の始まり
    size_t begin(const char* tag) {
        id(tag);
        u32(0);
        return b.size();
    }
    void end(size_t start) {
        uint32_t n = (uint32_t)(b.size() - start);
        memcpy(&b[start - 4], &n, 4); // リトルエンディアンのホストで使う
        if (n & 1) u8(0);
    }
    size_t list(const char* type) {
        size_t s = begin("LIST");
        id(type);
        return s;
    }
};

inline std::vector<uint8_t> makeSineSf2() {
    const uint32_t kLen = 4400; // 44 周期 (ループの継ぎ目で途切れない)
    Writer w;
    size_t riff = w.begin("RIFF");
    w.id("sfbk");
    size_t info = w.list("INFO");
    size_t c = w.begin("ifil");
    w.u16(2);
    w.u16(1);
    w.end(c);
    c = w.begin("isng");
    w.b.insert(w.b.end(), {'E', 'M', 'U', '8', '0', '0', '0', 0});
    w.end(c);
    c = w.begin("INAM");
    w.b.insert(w.b.end(), {'t', 'e', 's', 't', 0, 0});
    w.end(c);
    w.end(info);
    size_t sdta = w.list("sdta");
    c = w.begin("smpl");
    for (uint32_t i = 0; i < kLen + 46; i++) {
        double v = i < kLen ? sin(2 * 3.14159265358979323846 * i / 100.0) : 0.0;
        w.u16((uint16_t)(int16_t)lrint(v * 16000));
    }
    w.end(c);
    w.end(sdta);
    size_t pdta = w.list("pdta");
    // プリセット: 0 = (バンク 0, プログラム 0)、1 = (バンク 128, プログラム 0)、EOP
    c = w.begin("phdr");
    const uint16_t banks[2] = {0, 128};
    for (int i = 0; i < 2; i++) {
        w.name(i ? "Drum" : "Sine");
        w.u16(0);
        w.u16(banks[i]);
        w.u16((uint16_t)i);
        w.u32(0);
        w.u32(0);
        w.u32(0);
    }
    w.name("EOP");
    w.u16(0);
    w.u16(0);
    w.u16(2);
    w.u32(0);
    w.u32(0);
    w.u32(0);
    w.end(c);
    c = w.begin("pbag"); // それぞれ 1 つの区域 (生成子 1 つ: 楽器)
    for (int i = 0; i < 3; i++) {
        w.u16((uint16_t)i);
        w.u16(0);
    }
    w.end(c);
    c = w.begin("pmod");
    for (int i = 0; i < 10; i++) w.u8(0);
    w.end(c);
    c = w.begin("pgen");
    for (int i = 0; i < 2; i++) {
        w.u16(41); // instrument
        w.u16(0);
    }
    w.u16(0);
    w.u16(0);
    w.end(c);
    c = w.begin("inst");
    w.name("SineInst");
    w.u16(0);
    w.name("EOI");
    w.u16(1);
    w.end(c);
    c = w.begin("ibag");
    w.u16(0);
    w.u16(0);
    w.u16(2);
    w.u16(0);
    w.end(c);
    c = w.begin("imod");
    for (int i = 0; i < 10; i++) w.u8(0);
    w.end(c);
    c = w.begin("igen");
    w.u16(54); // sampleModes = 1 (ループ)
    w.u16(1);
    w.u16(53); // sampleID = 0
    w.u16(0);
    w.u16(0);
    w.u16(0);
    w.end(c);
    c = w.begin("shdr");
    w.name("Sine");
    w.u32(0);
    w.u32(kLen);
    w.u32(0);
    w.u32(kLen);
    w.u32(44100);
    w.u8(69);
    w.u8(0);
    w.u16(0);
    w.u16(1); // mono
    w.name("EOS");
    for (int i = 0; i < 5; i++) w.u32(0);
    w.u8(0);
    w.u8(0);
    w.u16(0);
    w.u16(0);
    w.end(c);
    w.end(pdta);
    w.end(riff);
    return w.b;
}

// TinySoundFont が読むと大きく広がる SF2 (小さいファイルで、確保の上限を確かめるため)。presets 個のプリセットが、
// zones 個の区域を持つ同じ楽器を使う。TinySoundFont はプリセットごとに楽器の区域を写すので、区域の数は
// presets x zones になる (1024 x 1024 で 55,938 バイトのファイルから、170MB ほどを確保しようとする)
// first を渡すと、プリセットの番号 (バンク x 128 + プログラム) を first から始める
inline std::vector<uint8_t> makeWideSf2(int presets, int zones, int first = 0) {
    const uint32_t kLen = 100;
    Writer w;
    size_t riff = w.begin("RIFF");
    w.id("sfbk");
    size_t info = w.list("INFO");
    size_t c = w.begin("ifil");
    w.u16(2);
    w.u16(1);
    w.end(c);
    w.end(info);
    size_t sdta = w.list("sdta");
    c = w.begin("smpl");
    for (uint32_t i = 0; i < kLen + 46; i++) w.u16(0);
    w.end(c);
    w.end(sdta);
    size_t pdta = w.list("pdta");
    c = w.begin("phdr");
    for (int i = 0; i <= presets; i++) {
        w.name(i < presets ? "P" : "EOP");
        w.u16((uint16_t)(i < presets ? (first + i) % 128 : 0));
        w.u16((uint16_t)(i < presets ? (first + i) / 128 : 0));
        w.u16((uint16_t)i); // 区域の番号 (1 つずつ)
        w.u32(0);
        w.u32(0);
        w.u32(0);
    }
    w.end(c);
    c = w.begin("pbag");
    for (int i = 0; i <= presets; i++) {
        w.u16((uint16_t)i); // 生成子 1 つずつ
        w.u16(0);
    }
    w.end(c);
    c = w.begin("pmod");
    for (int i = 0; i < 10; i++) w.u8(0);
    w.end(c);
    c = w.begin("pgen");
    for (int i = 0; i < presets; i++) {
        w.u16(41); // instrument 0
        w.u16(0);
    }
    w.u16(0);
    w.u16(0);
    w.end(c);
    c = w.begin("inst");
    w.name("Wide");
    w.u16(0);
    w.name("EOI");
    w.u16((uint16_t)zones);
    w.end(c);
    c = w.begin("ibag");
    for (int i = 0; i <= zones; i++) {
        w.u16((uint16_t)i); // 生成子 1 つずつ (sampleID)
        w.u16(0);
    }
    w.end(c);
    c = w.begin("imod");
    for (int i = 0; i < 10; i++) w.u8(0);
    w.end(c);
    c = w.begin("igen");
    for (int i = 0; i < zones; i++) {
        w.u16(53); // sampleID = 0
        w.u16(0);
    }
    w.u16(0);
    w.u16(0);
    w.end(c);
    c = w.begin("shdr");
    w.name("S");
    w.u32(0);
    w.u32(kLen);
    w.u32(0);
    w.u32(kLen);
    w.u32(44100);
    w.u8(69);
    w.u8(0);
    w.u16(0);
    w.u16(1);
    w.name("EOS");
    for (int i = 0; i < 5; i++) w.u32(0);
    w.u8(0);
    w.u8(0);
    w.u16(0);
    w.u16(0);
    w.end(c);
    w.end(pdta);
    w.end(riff);
    return w.b;
}

} // namespace sf2test
