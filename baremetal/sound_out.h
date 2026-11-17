// ベアメタル版の音の出力。Circle の音のデバイスが DMA の割り込み (コア 0) で GetChunk() を呼ぶので、
// そこで X68000 (コア 1) のリングバッファ (platform::AudioSource、ロックなし) から取り出して変換する。
//   HDMI: 24 ビットの IEC958 (S/PDIF) の形式。HDMI の画面がつながっていれば音も出る (Pi 3 / Pi 4 / Pi 5)
//   PWM:  Pi 3 と Pi 4B は 3.5mm のジャック (Pi 400 にはジャックがない)。Pi 5 はジャックがなく、GPIO 12 / 13 に出る (RP1)
//   I2S:  GPIO 18 (BCK) / 19 (LRCK) / 21 (DOUT) につないだ DAC。Pi が BCK と LRCK を出す (DAC はクロックの従側)。
//         Pi 3 / Pi 4 は BCM283x / BCM2711 の PCM、Pi 5 は RP1 の I2S0 (Circle が同じピンに割り当てる)
#pragma once

#include <circle/sound/hdmisoundbasedevice.h>
#include <circle/sound/pwmsoundbasedevice.h>
#include <circle/sound/i2ssoundbasedevice.h>

#include "platform/platform.h"

namespace pix68k_sound {

// 1 回の GetChunk() で扱うフレーム数の上限 (下のチャンクの大きさに合わせる)
static constexpr unsigned MAX_FRAMES = 1024;
// HDMI: 384 ワード (192 フレーム) の倍数。768 フレーム = 16ms
static constexpr unsigned HDMI_CHUNK = 384 * 4;
// PWM: 1024 ワード = 512 フレーム = 11ms
static constexpr unsigned PWM_CHUNK = 1024;
// I2S: 1024 ワード = 512 フレーム = 11ms
static constexpr unsigned I2S_CHUNK = 1024;
static constexpr unsigned RATE = 48000;

} // namespace pix68k_sound

class CHdmiSoundOut : public CHDMISoundBaseDevice {
public:
    CHdmiSoundOut(CInterruptSystem* interrupt, platform::AudioSource* source)
        : CHDMISoundBaseDevice(interrupt, pix68k_sound::RATE, pix68k_sound::HDMI_CHUNK), m_pSource(source) {}

protected:
    unsigned GetChunk(u32* buffer, unsigned chunkSize) override {
        unsigned frames = chunkSize / 2;
        if (frames > pix68k_sound::MAX_FRAMES) frames = pix68k_sound::MAX_FRAMES;
        m_pSource->readAudio(m_Samples, (int)frames);
        // チャンクは 192 フレーム (IEC958 のブロック) の倍数なので、ブロックの中の位置はフレームの番号から決まる
        for (unsigned f = 0; f < frames; f++) {
            unsigned n = f % IEC958_FRAMES_PER_BLOCK;
            for (unsigned ch = 0; ch < 2; ch++) { // 左、右
                // IEC958 のサンプルは 24 ビット。16 ビットの値を上位 16 ビットに置く (256 倍)
                const s32 sample24 = (s32)m_Samples[2 * f + ch] * 256;
                buffer[2 * f + ch] = ConvertIEC958Sample((u32)sample24, n);
            }
        }
        return frames * 2;
    }

private:
    platform::AudioSource* m_pSource;
    s16 m_Samples[2 * pix68k_sound::MAX_FRAMES];
};

class CPwmSoundOut : public CPWMSoundBaseDevice {
public:
    CPwmSoundOut(CInterruptSystem* interrupt, platform::AudioSource* source)
        : CPWMSoundBaseDevice(interrupt, pix68k_sound::RATE, pix68k_sound::PWM_CHUNK), m_pSource(source) {}

protected:
    unsigned GetChunk(u32* buffer, unsigned chunkSize) override {
        unsigned frames = chunkSize / 2;
        if (frames > pix68k_sound::MAX_FRAMES) frames = pix68k_sound::MAX_FRAMES;
        m_pSource->readAudio(m_Samples, (int)frames);
        // -32768..32767 を 0..GetRangeMax() に直す
        const u32 range = (u32)GetRangeMax();
        for (unsigned i = 0; i < frames * 2; i++) buffer[i] = (u32)(((s32)m_Samples[i] + 32768) * (u64)range >> 16);
        return frames * 2;
    }

private:
    platform::AudioSource* m_pSource;
    s16 m_Samples[2 * pix68k_sound::MAX_FRAMES];
};

// I2S の DAC。pI2CMaster が nullptr でなければ、Circle が I2C で DAC を探して設定する (address が 0 なら PCM512x を
// 0x4C / 0x4D、WM8960 を 0x1A で探し、どれもなければ設定の要らない DAC (PCM5102A など) として動かす。
// 0 でなければその番地の PCM512x か WM8960 で、見つからなければ Start() が失敗する)
class CI2sSoundOut : public CI2SSoundBaseDevice {
public:
    CI2sSoundOut(CInterruptSystem* interrupt, platform::AudioSource* source, CI2CMaster* i2c, u8 address)
        : CI2SSoundBaseDevice(interrupt, pix68k_sound::RATE, pix68k_sound::I2S_CHUNK, FALSE, i2c, address),
          m_pSource(source) {}

protected:
    unsigned GetChunk(u32* buffer, unsigned chunkSize) override {
        // 少なく返すと Circle が送るのをやめるので、いつも chunkSize の分を埋める
        unsigned frames = chunkSize / 2;
        if (frames > pix68k_sound::MAX_FRAMES) frames = pix68k_sound::MAX_FRAMES;
        m_pSource->readAudio(m_Samples, (int)frames);
        // 32 ビットのワードの中の符号付き 24 ビット (GetRangeMin() から GetRangeMax())。16 ビットを 256 倍する
        // (負の数の左シフトは C++17 では未定義なので掛け算にする)
        for (unsigned i = 0; i < frames * 2; i++) buffer[i] = (u32)((s32)m_Samples[i] * 256);
        return frames * 2;
    }

private:
    platform::AudioSource* m_pSource;
    s16 m_Samples[2 * pix68k_sound::MAX_FRAMES];
};
