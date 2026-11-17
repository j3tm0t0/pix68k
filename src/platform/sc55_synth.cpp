// SC-55mkII の MIDI 音源。sc55_synth.h を参照
#include "platform/sc55_synth.h"

#include <string.h>

#ifdef PIX68K_CIRCLE
// ベアメタル版には libm がない。baremetal/munt_libc.cpp のものを使う
extern "C" double sin(double x);
extern "C" double cos(double x);
#else
#include <math.h>
#endif

#include "mcu.h"
#include "pcm.h"
#include "submcu.h"

// Nuked-SC55 (external/nuked-sc55/src/mcu.cpp) の中のもの
extern uint8_t* rom1;
extern uint8_t* rom2;
extern uint8_t* sram;
void unscramble(uint8_t* src, uint8_t* dst, int len);
void MCU_Init(void);
void MCU_Reset(void);
void MCU_PatchROM(void);
void MCU_Step(void);
void MCU_ResetGlobals(void);
void PCM_Reset(void);

namespace {

platform::Sc55Synth* g_owner = nullptr; // 開いている Sc55Synth (Nuked-SC55 の状態はグローバル変数なので 1 つだけ)

constexpr uint32_t kSizes[platform::Sc55Synth::ROM_COUNT] = {0x8000, 0x80000, 0x1000, 0x200000, 0x100000};
const char* const kFileNames[platform::Sc55Synth::ROM_COUNT] = {"rom1.bin", "rom2.bin", "rom_sm.bin", "waverom1.bin",
                                                                "waverom2.bin"};
const char* const kPartNames[platform::Sc55Synth::ROM_COUNT] = {"R15199858", "R00233567", "R15199880", "R15209359",
                                                                "R15279813"};
constexpr uint32_t kChunk = 0x100000; // 波形の ROM の並べ替えは 1MB ごとに閉じている (unscramble())
constexpr uint32_t kSramSize = 0x8000; // H8/532 の外の SRAM (mcu.cpp の SRAM_SIZE)

bool sameName(const char* a, const char* b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
        if (x == 0) return true;
    }
}

// メモリの上の ROM を読む口
class MemorySource : public platform::Sf2Source {
public:
    MemorySource(const uint8_t* data, unsigned size) : data_(data), size_(size) {}
    int read(void* out, unsigned n) override {
        if (data_ == nullptr) return 0;
        const unsigned k = n < size_ - pos_ ? n : size_ - pos_;
        memcpy(out, data_ + pos_, k);
        pos_ += k;
        return (int)k;
    }
    bool skip(unsigned n) override {
        if (n > size_ - pos_) return false;
        pos_ += n;
        return true;
    }

private:
    const uint8_t* data_;
    unsigned size_;
    unsigned pos_ = 0;
};

inline int16_t clamp16(float v) { return v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int16_t)v; }

} // namespace

// Nuked-SC55 が出した標本 (mcu.cpp の MCU_PostSample() が呼ぶ)
void SC55_PostSample(int left, int right) {
    if (g_owner != nullptr) g_owner->postSample(left, right);
}

// パネルの LCD (pix68k は表示しない)
void LCD_Write(uint32_t, uint8_t) {}
void LCD_Enable(uint32_t) {}

namespace platform {

const char* Sc55Synth::resultText(Result r) {
    switch (r) {
    case OK: return "ok";
    case BAD_SIZE: return "a ROM file has a wrong size";
    case READ_FAILED: return "cannot read a ROM file";
    case BAD_ROM: return "not SC-55mkII ROMs (bad reset vector)";
    case IN_USE: return "another SC-55 is open";
    case BAD_RATE: return "the output sample rate is not supported";
    case NO_SOUND: return "the SC-55mkII did not start (no sound from the PCM chip)";
    }
    return "?";
}

uint32_t Sc55Synth::romSize(int rom) { return rom >= 0 && rom < ROM_COUNT ? kSizes[rom] : 0; }
const char* Sc55Synth::romFileName(int rom) { return rom >= 0 && rom < ROM_COUNT ? kFileNames[rom] : ""; }
const char* Sc55Synth::romPartName(int rom) { return rom >= 0 && rom < ROM_COUNT ? kPartNames[rom] : ""; }

int Sc55Synth::pickRoms(const char* const* names, const uint64_t* sizes, int n, int index[ROM_COUNT]) {
    int missing = -1;
    for (int rom = 0; rom < ROM_COUNT; rom++) {
        index[rom] = -1;
        for (int i = 0; i < n && index[rom] < 0; i++)
            if (names[i][0] != '.' && sizes[i] == kSizes[rom] && sameName(names[i], kFileNames[rom])) index[rom] = i;
        if (index[rom] < 0) {
            int found = -1, count = 0;
            for (int i = 0; i < n; i++)
                if (names[i][0] != '.' && sizes[i] == kSizes[rom]) {
                    found = i;
                    count++;
                }
            if (count == 1) index[rom] = found;
        }
        if (index[rom] < 0 && missing < 0) missing = rom;
    }
    return missing;
}

Sc55Synth::Sc55Synth() = default;

Sc55Synth::~Sc55Synth() { freeAll(); }

void Sc55Synth::freeAll() {
    if (g_owner == this) {
        g_owner = nullptr;
        waverom1 = waverom2 = waverom3 = nullptr;
        rom1 = rom2 = sram = nullptr;
    }
    delete[] rom1_;
    delete[] rom2_;
    delete[] sram_;
    rom1_ = rom2_ = sram_ = nullptr;
    for (int i = 0; i < 3; i++) {
        delete[] wave_[i];
        wave_[i] = nullptr;
    }
    delete[] filter_;
    filter_ = nullptr;
    loaded_ = false;
}

uint64_t Sc55Synth::mcuCycles() const { return g_owner == this ? mcu.cycles : 0; }

Sc55Synth::Result Sc55Synth::load(const uint8_t* const data[ROM_COUNT], int rate) {
    MemorySource s0(data[0], kSizes[0]), s1(data[1], kSizes[1]), s2(data[2], kSizes[2]), s3(data[3], kSizes[3]),
        s4(data[4], kSizes[4]);
    Sf2Source* const src[ROM_COUNT] = {&s0, &s1, &s2, &s3, &s4};
    return load(src, rate);
}

Sc55Synth::Result Sc55Synth::load(Sf2Source* const src[ROM_COUNT], int rate, void (*idle)(void*), void* idleArg) {
    freeAll();
    if (g_owner != nullptr) return IN_USE;
    if (rate <= 0 || rate > 192000) return BAD_RATE;
    rate_ = rate;
    g_owner = this;
    const Result r = loadCommon(src, idle, idleArg);
    if (r != OK) freeAll();
    return r;
}

Sc55Synth::Result Sc55Synth::loadCommon(Sf2Source* const src[ROM_COUNT], void (*idle)(void*), void* idleArg) {
    // H8/532 の中の ROM を先に読み、リセットのベクタ (先頭の 4 バイト。Nuked-SC55 の MCU_Reset() はページと番地として
    // 読む) が ROM の中のベクタの表 ($00-$E3) のあとを指すかを見る。違えば、ほかは確保しない
    rom1_ = new uint8_t[kSizes[ROM1]];
    if (rom1_ == nullptr) return READ_FAILED;
    if (src[ROM1]->read(rom1_, kSizes[ROM1]) != (int)kSizes[ROM1]) return READ_FAILED;
    const uint32_t reset = (uint32_t)rom1_[0] << 24 | (uint32_t)rom1_[1] << 16 | (uint32_t)rom1_[2] << 8 | rom1_[3];
    if (reset < 0xe4 || reset >= kSizes[ROM1]) return BAD_ROM;
    // Nuked-SC55 の ROM と SRAM (32KB) の領域。波形の ROM は 1MB ずつ並べ替えて入れる
    rom2_ = new uint8_t[kSizes[ROM2]];
    sram_ = new uint8_t[kSramSize];
    wave_[0] = new uint8_t[kSizes[WAVE1]];
    wave_[1] = new uint8_t[kSizes[WAVE2]];
    wave_[2] = new uint8_t[kChunk];
    filter_ = new float[(PHASES + 1) * TAPS];
    uint8_t* tmp = new uint8_t[kChunk];
    if (rom2_ == nullptr || sram_ == nullptr || wave_[0] == nullptr || wave_[1] == nullptr || wave_[2] == nullptr ||
        filter_ == nullptr || tmp == nullptr) {
        delete[] tmp;
        return READ_FAILED;
    }
    rom1 = rom1_;
    rom2 = rom2_;
    sram = sram_;
    MCU_ResetGlobals();
    if (src[ROM2]->read(rom2_, kSizes[ROM2]) != (int)kSizes[ROM2] ||
        src[ROM_SM]->read(sm_rom, kSizes[ROM_SM]) != (int)kSizes[ROM_SM]) {
        delete[] tmp;
        return READ_FAILED;
    }
    memset(wave_[2], 0, kChunk);
    Result result = OK;
    for (int w = 0; w < 2 && result == OK; w++) {
        for (uint32_t off = 0; off < kSizes[WAVE1 + w]; off += kChunk) {
            if (src[WAVE1 + w]->read(tmp, kChunk) != (int)kChunk) {
                result = READ_FAILED;
                break;
            }
            unscramble(tmp, wave_[w] + off, (int)kChunk);
            if (idle) idle(idleArg);
        }
    }
    delete[] tmp;
    if (result != OK) return result;
    waverom1 = wave_[0];
    waverom2 = wave_[1];
    waverom3 = wave_[2];
    waverom_card = waverom_exp = nullptr; // JV-880 だけが使う

    // Nuked-SC55 の main() と同じ順で初期化する (romset は SC-55mkII のまま)
    MCU_Init();
    MCU_PatchROM();
    MCU_Reset();
    SM_Reset();
    PCM_Reset();

    srcCount_ = 0;
    pos_ = 0;
    step_ = 0;
    srcRate_ = 0;
    cfg3c_ = cfg3d_ = 0xff; // 最初の標本で周波数を求める
    memset(src_, 0, sizeof src_);
    qHead_ = qTail_ = 0;
    outFrames_ = made_ = 0;
    fHead_ = fTail_ = 0;
    underruns_ = skips_ = droppedBytes_ = 0;
    status_ = need_ = have_ = 0;
    inSysex_ = false;
    oHead_ = oTail_ = 0;
    uartNext_ = 0;

    // 起動 (sc55_synth.h)。BOOT_MS だけ進めて音は捨てる
    const uint64_t bootCycles = (uint64_t)MCU_HZ / 1000 * BOOT_MS;
    uint64_t nextIdle = 0;
    while (mcu.cycles < bootCycles) {
        MCU_Step();
        if (idle && mcu.cycles >= nextIdle) {
            idle(idleArg);
            nextIdle = mcu.cycles + MCU_HZ / 100;
        }
    }
    if (srcRate_ < 16000 || srcRate_ > 100000) return NO_SOUND;
    // 出力のフレームの位置を今に合わせる (起動の間の標本は使わない)
    pos_ = srcCount_ << 32;
    loaded_ = true;
    return OK;
}

// PCM の LSI の設定から標本の周波数を求める (pcm.cpp の PCM_Update(): 1 周 ((レジスタ $3D の下位 5 ビット) + 2) x 25
// クロックで、オーバーサンプリング ($3C の bit6) なら 1 周に 2 つ出す)
void Sc55Synth::updateRate() {
    cfg3c_ = pcm.config_reg_3c;
    cfg3d_ = pcm.config_reg_3d;
    const uint32_t cycles = (uint32_t)((cfg3d_ & 31) + 2) * 25;
    srcRate_ = (double)MCU_HZ / cycles * ((cfg3c_ & 0x40) ? 2 : 1);
    step_ = (uint64_t)(srcRate_ / rate_ * 4294967296.0);
    buildFilter(filter_, srcRate_, rate_);
    // 新しい周波数の最初の標本から数え直す (前の標本は捨てる)
    memset(src_, 0, sizeof src_);
    pos_ = srcCount_ << 32;
}

// 窓をかけた sinc の表 (sc55_synth.h)。filter[p][j] は、出力の位置が標本 i + p / PHASES のときに標本 i - 7 + j に掛ける値
void Sc55Synth::buildFilter(float* filter, double srcRate, int rate) {
    const double pi = 3.14159265358979323846;
    double cutoff = 0.45 * rate;
    if (cutoff > 0.45 * srcRate) cutoff = 0.45 * srcRate;
    const double fc = cutoff / srcRate; // 標本の周波数に対する比
    const int half = TAPS / 2;
    for (int p = 0; p <= PHASES; p++) {
        double h[TAPS];
        double sum = 0;
        for (int j = 0; j < TAPS; j++) {
            const double d = (double)(j - (half - 1)) - (double)p / PHASES;
            const double x = 2 * fc * d;
            const double sinc = d == 0 ? 1.0 : sin(pi * x) / (pi * x);
            const double w = d <= -half || d >= half ? 0.0 : 0.42 + 0.5 * cos(pi * d / half) + 0.08 * cos(2 * pi * d / half);
            h[j] = 2 * fc * sinc * w;
            sum += h[j];
        }
        for (int j = 0; j < TAPS; j++) filter[p * TAPS + j] = (float)(sum != 0 ? h[j] / sum : 0);
    }
}

void Sc55Synth::postSample(int l, int r) {
    if (pcm.config_reg_3c != cfg3c_ || pcm.config_reg_3d != cfg3d_) updateRate();
    float* s = src_[srcCount_ % SRC_RING];
    s[0] = (float)l;
    s[1] = (float)r;
    srcCount_++;
}

void Sc55Synth::produceFrame(int16_t& l, int16_t& r) {
    feedMidi(made_);
    drainUart();
    const int half = TAPS / 2;
    for (;;) {
        const uint64_t i0 = pos_ >> 32;
        if (srcCount_ > i0 + half) break;
        MCU_Step();
    }
    const uint64_t i0 = pos_ >> 32;
    const uint32_t frac = (uint32_t)pos_;
    const int p = (int)(frac >> 24);
    const float t = (float)(frac & 0xffffff) * (1.0f / 16777216.0f);
    const float* h0 = filter_ + p * TAPS;
    const float* h1 = h0 + TAPS;
    float accL = 0, accR = 0;
    for (int j = 0; j < TAPS; j++) {
        const float h = h0[j] + (h1[j] - h0[j]) * t;
        const float* s = src_[(i0 - (half - 1) + j) % SRC_RING];
        accL += s[0] * h;
        accR += s[1] * h;
    }
    l = clamp16(accL * gain_);
    r = clamp16(accR * gain_);
    pos_ += step_;
    made_++;
    if (srcCount_ >= SRC_REBASE) {
        static_assert(SRC_REBASE / 2 % SRC_RING == 0, "src_ のリングの位置を変えない量を引く");
        srcCount_ -= SRC_REBASE / 2;
        pos_ -= (SRC_REBASE / 2) << 32;
    }
}

void Sc55Synth::pushQueue(uint32_t value) {
    if (!loaded_) return;
    const uint32_t head = qHead_;
    if (head - __atomic_load_n(&qTail_, __ATOMIC_ACQUIRE) >= (uint32_t)QUEUE_SIZE) {
        __atomic_store_n(&droppedBytes_, droppedBytes_ + 1, __ATOMIC_RELAXED);
        return;
    }
    Entry& e = queue_[head & (QUEUE_SIZE - 1)];
    e.frame = outFrames_ + (async_ ? (uint32_t)ASYNC_DELAY : 0);
    e.value = value;
    __atomic_store_n(&qHead_, head + 1, __ATOMIC_RELEASE);
}

// UART に入れるバイトを、待つバイトのリングに置く (feedMidi() が 64 バイト以上空いているときだけ呼ぶ)
void Sc55Synth::postUart(uint8_t b) {
    out_[oHead_ % OUT_RING] = b;
    oHead_++;
}

// 待つバイトを MIDI の速さ (UART_CYCLES ごとに 1 バイト) で Nuked-SC55 の UART に入れる。MIDI ボードが送るバイトは
// もとからこの速さで来るが、設定の送り直し (x68k/midi_shadow.h) と midiReset() のバイトはまとめて来る。
// Nuked-SC55 のサブのマイコンは UART のバッファ (8KB、あふれを調べない) から本物より速く読むので、ここで合わせる
void Sc55Synth::drainUart() {
    while (oTail_ != oHead_ && mcu.cycles >= uartNext_) {
        const uint32_t used = (uart_write_ptr - uart_read_ptr + uart_buffer_size) % uart_buffer_size;
        if (used >= uart_buffer_size - 1) break;
        // しばらく空いていたら今から数える (まとめて入れない)
        if (uartNext_ + UART_CYCLES < mcu.cycles) uartNext_ = mcu.cycles;
        MCU_PostUART(out_[oTail_ % OUT_RING]);
        oTail_++;
        uartNext_ += UART_CYCLES;
    }
}

// frame までに受けたバイトを、待つバイトのリングに移す。64 バイト (midiReset() で入れる量) より空きが少なければ
// 次のフレームまで待つ
void Sc55Synth::feedMidi(uint32_t frame) {
    uint32_t tail = qTail_;
    const uint32_t head = __atomic_load_n(&qHead_, __ATOMIC_ACQUIRE);
    while (tail != head) {
        const Entry& e = queue_[tail & (QUEUE_SIZE - 1)];
        if ((int32_t)(e.frame - frame) > 0) break;
        if (OUT_RING - (oHead_ - oTail_) < 64) break;
        if (e.value == RESET_ENTRY) resetSound();
        else feedByte((uint8_t)e.value);
        tail++;
    }
    __atomic_store_n(&qTail_, tail, __ATOMIC_RELEASE);
}

// バイトを UART に入れ、メッセージの途中かを覚える (resetSound() のため)
void Sc55Synth::feedByte(uint8_t b) {
    if (b < 0xf8) {
        if (b & 0x80) {
            inSysex_ = b == 0xf0;
            if (b >= 0xf0) {
                status_ = 0; // システムのメッセージはランニングステータスを消す
            } else {
                status_ = b;
                need_ = ((b & 0xe0) == 0xc0) ? 1 : 2; // $Cx と $Dx はデータが 1 バイト
                have_ = 0;
            }
        } else if (!inSysex_ && status_ != 0) {
            data_[have_++] = b;
            if (have_ >= need_) have_ = 0;
        }
    }
    postUart(b);
}

// 鳴っている音を止める (sc55_synth.h)
void Sc55Synth::resetSound() {
    if (inSysex_) postUart(0xf7);
    inSysex_ = false;
    for (uint8_t ch = 0; ch < 16; ch++) {
        postUart((uint8_t)(0xb0 | ch));
        postUart(120);
        postUart(0);
    }
    if (status_ != 0) {
        postUart(status_);
        for (int i = 0; i < have_; i++) postUart(data_[i]);
    }
}

int Sc55Synth::pump(int maxFrames) {
    if (!loaded_ || !async_) return 0;
    const uint32_t out = __atomic_load_n(&outFrames_, __ATOMIC_ACQUIRE);
    // 大きく遅れたら、そこまでのフレームは作らずに飛ばす (sc55_synth.h)
    if ((int32_t)(out - made_) > rate_ / 4) {
        made_ = out + (uint32_t)ASYNC_DELAY / 2;
        __atomic_store_n(&skips_, skips_ + 1, __ATOMIC_RELAXED);
    }
    int n = 0;
    while (n < maxFrames && (int32_t)(made_ - (out + (uint32_t)ASYNC_DELAY)) < 0) {
        const uint32_t index = made_;
        int16_t l, r;
        produceFrame(l, r);
        n++;
        // 間に合わなかったフレームは捨てる。リングは ASYNC_DELAY フレームより多く入るので、ここではあふれない
        if ((int32_t)(index - __atomic_load_n(&outFrames_, __ATOMIC_ACQUIRE)) < 0) continue;
        const uint32_t head = fHead_;
        if (head - __atomic_load_n(&fTail_, __ATOMIC_ACQUIRE) >= (uint32_t)FIFO_FRAMES) continue;
        Frame& f = fifo_[head & (FIFO_FRAMES - 1)];
        f.index = index;
        f.l = l;
        f.r = r;
        __atomic_store_n(&fHead_, head + 1, __ATOMIC_RELEASE);
    }
    return n;
}

void Sc55Synth::mixFrame(int32_t& l, int32_t& r) {
    if (!loaded_) return;
    const uint32_t index = outFrames_;
    if (!async_) {
        int16_t a, b;
        produceFrame(a, b);
        l += a;
        r += b;
        outFrames_ = index + 1;
        return;
    }
    // 古いフレーム (前に足りなかったときに作りかけていたもの) は捨て、このフレームがあれば足す
    uint32_t tail = fTail_;
    const uint32_t head = __atomic_load_n(&fHead_, __ATOMIC_ACQUIRE);
    while (tail != head && (int32_t)(fifo_[tail & (FIFO_FRAMES - 1)].index - index) < 0) tail++;
    if (tail != head && fifo_[tail & (FIFO_FRAMES - 1)].index == index) {
        const Frame& f = fifo_[tail & (FIFO_FRAMES - 1)];
        l += f.l;
        r += f.r;
        tail++;
    } else {
        __atomic_store_n(&underruns_, underruns_ + 1, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&fTail_, tail, __ATOMIC_RELEASE);
    __atomic_store_n(&outFrames_, index + 1, __ATOMIC_RELEASE);
}

void Sc55Synth::renderFrames(int16_t* out, int n) {
    for (int i = 0; i < n; i++) {
        int32_t l = 0, r = 0;
        mixFrame(l, r);
        out[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
}

} // namespace platform
