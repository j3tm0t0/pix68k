// SoundFont 2 の MIDI 音源。sf2_synth.h を参照
#include "platform/sf2_synth.h"

#include <string.h>

#ifdef PIX68K_CIRCLE
#include <circle/alloc.h>
#else
#include <stdlib.h>
#endif

// ---------------------------------------------------------------- TinySoundFont が使う数学の関数
// ベアメタルには libm がないので自分で用意する。使うのはノートオンとエンベロープとフィルタの設定 (64 フレームに 1 回など)
// なので、速さより簡単さを取る。倍精度で 1e-12 ほどの誤差。
namespace {

constexpr double kLn2 = 0.69314718055994530942;
constexpr double kPi = 3.14159265358979323846;

// 2^i (i は整数)。倍精度の指数の範囲を超えたら 0 か大きな値
double pow2i(int i) {
    if (i < -1022) return 0.0;
    if (i > 1023) return 1e308;
    union {
        double d;
        uint64_t u;
    } v;
    v.u = (uint64_t)(i + 1023) << 52;
    return v.d;
}

// 2^x
double exp2d(double x) {
    if (x < -1100.0) return 0.0;
    if (x > 1100.0) return 1e308;
    int i = (int)x;
    if ((double)i > x) i--;
    const double t = (x - i) * kLn2; // 0 <= t < ln 2
    double sum = 1.0, term = 1.0;
    for (int k = 1; k <= 16; k++) {
        term *= t / k;
        sum += term;
    }
    return sum * pow2i(i);
}

// log2(x)。x <= 0 なら大きな負の値
double log2d(double x) {
    if (!(x > 0.0)) return -1e300;
    union {
        double d;
        uint64_t u;
    } v;
    v.d = x;
    int e = (int)((v.u >> 52) & 0x7ff);
    if (e == 0) { // 非正規化数
        v.d = x * 4503599627370496.0; // 2^52
        e = (int)((v.u >> 52) & 0x7ff) - 52;
    }
    e -= 1023;
    v.u = (v.u & 0x000fffffffffffffull) | 0x3ff0000000000000ull; // 仮数 1 <= m < 2
    double m = v.d;
    if (m > 1.41421356237309504880) {
        m *= 0.5;
        e++;
    }
    // ln m = 2 (s + s^3/3 + s^5/5 + ...)、s = (m - 1) / (m + 1)、|s| < 0.172
    const double s = (m - 1.0) / (m + 1.0), s2 = s * s;
    double sum = 0.0, p = s;
    for (int k = 1; k <= 31; k += 2) {
        sum += p / k;
        p *= s2;
    }
    return e + 2.0 * sum / kLn2;
}

double powd(double a, double b) {
    if (a == 1.0 || b == 0.0) return 1.0;
    if (a <= 0.0) return 0.0; // TinySoundFont は正の底か 0 しか使わない
    return exp2d(b * log2d(a));
}

// sin と cos (|x| <= pi/2 で級数)
double sinSmall(double x) {
    const double x2 = x * x;
    double sum = 0.0, term = x;
    for (int k = 1; k <= 21; k += 2) {
        sum += term;
        term *= -x2 / ((k + 1) * (k + 2));
    }
    return sum;
}
double cosSmall(double x) {
    const double x2 = x * x;
    double sum = 0.0, term = 1.0;
    for (int k = 0; k <= 20; k += 2) {
        sum += term;
        term *= -x2 / ((k + 1) * (k + 2));
    }
    return sum;
}
double tand(double x) {
    // tan は周期 pi。-pi/2 < x <= pi/2 に寄せる
    const double n = (double)(long long)(x / kPi + (x >= 0 ? 0.5 : -0.5));
    x -= n * kPi;
    const double c = cosSmall(x);
    return c == 0.0 ? 1e300 : sinSmall(x) / c;
}

double sqrtd(double x) {
    if (!(x > 0.0)) return 0.0;
    // ニュートン法 (初めの値は指数を半分にしたもの)
    double r = exp2d(log2d(x) * 0.5);
    for (int i = 0; i < 3; i++) r = 0.5 * (r + x / r);
    return r;
}

// ---------------------------------------------------------------- TinySoundFont の確保
// load() の間に確保した量を数え、上限 (LoadLimit) を超える確保は、malloc を呼ぶ前に失敗させる (TinySoundFont は 0 を
// 受けると読むのをやめる)。Circle の malloc は足りないと全体を止めるので、ヒープの空きを上限にして呼ぶ。
// 解放しても数えた量は減らさない (Circle は 512KB を超える解放済みのブロックを使い回さないので、少なめに見積もらないため)。
// 生きているブロックをリストでつなぎ、読めなかったときに TinySoundFont が解放しないもの (tsf_load() の途中で失敗すると
// 残るプリセットなど) も解放する。load() の外 (音を作る間) の確保は、いつも失敗させる (音を作るコアでは確保しない)。
// 確保と解放は 1 つのコアから (load() はベアメタルではコア 0) だけ行う
struct BlockHeader { // 16 バイト (中身を 16 バイトにそろえる)
    BlockHeader* prev;
    BlockHeader* next;
};
static_assert(sizeof(BlockHeader) == 16, "header must keep 16-byte alignment");

struct AllocState {
    bool open = false;            // load() の中
    uint64_t limit = 0;           // 確保してよい量
    uint64_t used = 0;            // 確保した量 (ヘッダと、blockBytes() で見積もった切り上げを含む)
    bool exceeded = false;        // 上限を超えて断った
    uint64_t (*blockBytes)(size_t) = nullptr;
    BlockHeader* live = nullptr;  // 生きているブロック (load() の中で確保したもの)
} g_alloc;

bool charge(size_t n) {
    if (!g_alloc.open) {
        g_alloc.exceeded = true;
        return false;
    }
    const size_t total = n + sizeof(BlockHeader);
    const uint64_t cost = g_alloc.blockBytes ? g_alloc.blockBytes(total) : total;
    if (cost > g_alloc.limit || g_alloc.used > g_alloc.limit - cost) {
        g_alloc.exceeded = true;
        return false;
    }
    g_alloc.used += cost;
    return true;
}

void linkBlock(BlockHeader* h) {
    h->prev = nullptr;
    h->next = g_alloc.live;
    if (h->next) h->next->prev = h;
    g_alloc.live = h;
}

void unlinkBlock(BlockHeader* h) {
    if (h->prev) h->prev->next = h->next;
    else g_alloc.live = h->next;
    if (h->next) h->next->prev = h->prev;
}

void* tsfMalloc(size_t n) {
    if (!charge(n)) return nullptr;
    BlockHeader* h = (BlockHeader*)malloc(n + sizeof(BlockHeader));
    if (!h) return nullptr;
    linkBlock(h);
    return h + 1;
}

void tsfFree(void* p) {
    if (!p) return;
    BlockHeader* h = (BlockHeader*)p - 1;
    unlinkBlock(h);
    free(h);
}

void* tsfRealloc(void* p, size_t n) {
    if (!p) return tsfMalloc(n);
    if (!charge(n)) return nullptr; // 失敗しても p はそのまま (realloc と同じ)
    BlockHeader* h = (BlockHeader*)p - 1;
    unlinkBlock(h);
    BlockHeader* r = (BlockHeader*)realloc(h, n + sizeof(BlockHeader));
    if (!r) {
        linkBlock(h);
        return nullptr;
    }
    linkBlock(r);
    return r + 1;
}

// tsf_stream から Sf2Source を読む。TinySoundFont は標本のほかは read() と skip() の失敗を見ないので、頼んだ量に
// 届かない read() と、失敗した skip() をここで覚え、load() の終わりで読めなかったことにする。
// ただし、RIFF の直下のチャンクの頭 (4 バイトの ID) を読もうとして、ちょうどファイルの終わりで 0 バイトで、RIFF の大きさから
// 見て足りないのがちょうど 8 バイトのときだけは失敗にしない。RIFF の大きさにファイル全体の大きさ (正しい値 + 8) を書く
// 書き出しソフトがあり、TinySoundFont は最後の LIST のあとにまだ 8 バイトあるとみて次の ID を読みに行くため。
// LIST の中で終わったときと、足りないのが 8 バイトでないとき (チャンクが欠けている) は失敗にする。
// どこがチャンクの頭かは、TinySoundFont と同じ順にチャンクの大きさをたどって決める (ID、大きさ、RIFF / LIST なら種類の
// 4 バイトずつを、別々の read() で読む。中身は読むか、全体を skip() する)
struct StreamState {
    platform::Sf2Source* src;
    bool failed;
    uint64_t pos;       // ここまで読んだ (読み飛ばした) バイト数
    uint64_t head;      // 次のチャンクの頭の位置
    int phase;          // 0: head で ID を待つ、1: 大きさを待つ、2: RIFF / LIST の種類を待つ
    bool list;          // 1 で読んだ ID が RIFF か LIST
    uint64_t chunk;     // 1 で読んだ ID の位置
    uint64_t riffEnd;   // RIFF の大きさから決まる終わりの位置 (0 なら RIFF をまだ読んでいない)
    uint64_t listEnd;   // RIFF の直下の、今読んでいる LIST の終わりの位置
    bool short8;        // RIFF の大きさが + 8 のファイルの終わりで、ID を読めなかった
};
int streamRead(void* data, void* ptr, unsigned int size) {
    StreamState* st = (StreamState*)data;
    const uint64_t start = st->pos;
    int n = st->src->read(ptr, size);
    if (n < 0) {
        st->failed = true;
        n = 0;
    }
    st->pos += (unsigned)n;
    const bool full = (unsigned)n == size;
    if (st->phase == 0 && start == st->head && size == 4) { // チャンクの ID
        if (n == 0 && st->riffEnd != 0 && start >= st->listEnd && st->riffEnd == start + 8) {
            st->short8 = true; // RIFF の大きさが + 8 のファイルの終わり (上の説明)
            return 0;          // このあとさらに読もうとすれば、そこで失敗になる
        }
        if (!full) st->failed = true;
        const uint8_t* id = (const uint8_t*)ptr;
        st->list = full && (memcmp(id, "RIFF", 4) == 0 || memcmp(id, "LIST", 4) == 0);
        st->chunk = start;
        st->phase = 1;
        return n;
    }
    if (!full) st->failed = true;
    if (st->phase == 1 && size == 4) { // チャンクの大きさ
        uint32_t len = 0;
        if (full) {
            const uint8_t* p = (const uint8_t*)ptr;
            len = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        }
        const uint64_t end = st->chunk + 8 + len;
        if (st->list) {
            if (st->riffEnd == 0) st->riffEnd = end;
            else if (st->chunk >= st->listEnd) st->listEnd = end; // RIFF の直下の LIST
            st->phase = 2;
        } else {
            st->head = end; // 中身のあと (TinySoundFont は奇数の大きさの埋め草を読み飛ばさない)
            st->phase = 0;
        }
    } else if (st->phase == 2) { // RIFF / LIST の種類。この中のチャンクがすぐあとに続く
        st->head = st->pos;
        st->phase = 0;
    }
    return n;
}
int streamSkip(void* data, unsigned int count) {
    StreamState* st = (StreamState*)data;
    if (st->src->skip(count)) {
        // TinySoundFont が読み飛ばすのは、チャンクか LIST の中身の全体。次はチャンクの頭
        st->pos += count;
        st->head = st->pos;
        st->phase = 0;
        return 1;
    }
    st->failed = true;
    return 0;
}

// メモリの上の SF2
class MemorySource : public platform::Sf2Source {
public:
    MemorySource(const void* data, size_t size) : p_((const uint8_t*)data), left_(size) {}
    int read(void* out, unsigned n) override {
        if (n > left_) n = (unsigned)left_;
        memcpy(out, p_, n);
        p_ += n;
        left_ -= n;
        return (int)n;
    }
    bool skip(unsigned n) override {
        if (n > left_) return false;
        p_ += n;
        left_ -= n;
        return true;
    }

private:
    const uint8_t* p_;
    size_t left_;
};

} // namespace

#define TSF_POW(a, b) powd((a), (b))
#define TSF_POWF(a, b) ((float)powd((a), (b)))
#define TSF_EXPF(x) ((float)exp2d((x) / kLn2))
#define TSF_LOG(x) (log2d(x) * kLn2)
#define TSF_TAN(x) tand(x)
#define TSF_LOG10(x) (log2d(x) * 0.30102999566398119521)
#define TSF_SQRT(x) sqrtd(x)
#define TSF_SQRTF(x) ((float)sqrtd(x))
#define TSF_MALLOC tsfMalloc
#define TSF_REALLOC tsfRealloc
#define TSF_FREE tsfFree
#define TSF_MEMCPY memcpy
#define TSF_MEMSET memset
#define TSF_NO_STDIO
#define TSF_STATIC
#define TSF_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wunknown-warning-option"
#pragma GCC diagnostic ignored "-Wnull-pointer-subtraction"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "../../external/tinysoundfont/tsf.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace platform {

Sf2Synth::~Sf2Synth() {
    if (f_) tsf_close(f_);
}

bool Sf2Synth::load(const void* data, size_t size, int rate, int maxVoices, const LoadLimit& limit) {
    if (size > 0x7fffffff) {
        loadBytes_ = 0;
        overLimit_ = false;
        return false;
    }
    MemorySource src(data, size);
    return load(src, rate, maxVoices, limit);
}

bool Sf2Synth::load(Sf2Source& src, int rate, int maxVoices, const LoadLimit& limit) {
    if (f_) {
        tsf_close(f_);
        f_ = nullptr;
    }
    loadBytes_ = 0;
    overLimit_ = false;
    if (rate <= 0 || maxVoices <= 0) return false;
    g_alloc.open = true;
    g_alloc.limit = limit.maxBytes;
    g_alloc.used = 0;
    g_alloc.exceeded = false;
    g_alloc.blockBytes = limit.blockBytes;
    BlockHeader* const before = g_alloc.live; // ほかの Sf2Synth が持っているブロック
    StreamState st = {&src, false, 0, 0, 0, false, 0, 0, 0, false};
    tsf_stream stream = {&st, streamRead, streamSkip};
    tsf* f = tsf_load(&stream);
    // ここでボイスと 16 チャネルを確保しておく (合成するコアで確保しないため)
    // 途中で読めなかったら、読めたところまでで作ったものも捨てる。RIFF の終わりまで読んでいなければ (足りないのが
    // 8 バイトより少なく、TinySoundFont が次のチャンクを読みに行かなかったときも)、チャンクが欠けているので捨てる
    const bool whole = st.pos == st.riffEnd || (st.short8 && st.pos + 8 == st.riffEnd);
    bool ok = f != nullptr && !st.failed && whole;
    if (ok) {
        tsf_set_output(f, TSF_STEREO_INTERLEAVED, rate, 0.0f);
        ok = tsf_set_max_voices(f, maxVoices) && tsf_channel_set_bank(f, 15, 0);
    }
    if (!ok) {
        if (f) tsf_close(f);
        // TinySoundFont が途中で失敗して解放しなかったものを解放する (このあいだに確保したものは、リストの先頭から
        // before の手前まで)
        while (g_alloc.live && g_alloc.live != before) tsfFree(g_alloc.live + 1);
    }
    loadBytes_ = g_alloc.used;
    overLimit_ = g_alloc.exceeded;
    g_alloc.open = false;
    if (!ok) return false;
    f_ = f;
    resetChannels();
    stream_.reset();
    pos_ = BLOCK;
    return true;
}

int Sf2Synth::liveBlocks() {
    int n = 0;
    for (BlockHeader* h = g_alloc.live; h; h = h->next) n++;
    return n;
}

Sf2Synth::ChannelInfo Sf2Synth::channelInfo(int ch) const {
    ChannelInfo i = {};
    if (!f_) return i;
    i.presetIndex = tsf_channel_get_preset_index(f_, ch);
    i.bank = tsf_channel_get_preset_bank(f_, ch);
    i.wheel = tsf_channel_get_pitchwheel(f_, ch);
    i.volume = tsf_channel_get_volume(f_, ch);
    i.pan = tsf_channel_get_pan(f_, ch);
    i.range = tsf_channel_get_pitchrange(f_, ch);
    i.tuning = tsf_channel_get_tuning(f_, ch);
    const tsf_channel& c = f_->channels->channels[ch];
    i.rawBank = c.bank;
    i.sustain = c.sustain;
    i.midiVolume = c.midiVolume;
    i.midiExpression = c.midiExpression;
    i.midiPan = c.midiPan;
    const Rpn& r = rpn_[ch];
    i.rpnMsb = r.selMsb;
    i.rpnLsb = r.selLsb;
    i.nrpn = r.nrpn;
    for (int k = 0; k < 3; k++) {
        i.dataMsb[k] = r.msb[k];
        i.dataLsb[k] = r.lsb[k];
    }
    return i;
}

int Sf2Synth::presetCount() const { return f_ ? tsf_get_presetcount(f_) : 0; }
int Sf2Synth::activeVoices() const { return f_ ? tsf_active_voice_count(f_) : 0; }

// すべてのチャネルを初めの状態にする (音を止め、コントローラを戻し、プログラム 0。チャネル 10 はドラム)
void Sf2Synth::resetChannels() {
    for (int ch = 0; ch < 16; ch++) {
        tsf_channel_sounds_off_all(f_, ch);
        control(ch, 121, 0); // リセットオールコントローラ (バンク 0、ピッチベンドの幅 2、RPN)
        tsf_channel_set_pitchwheel(f_, ch, 8192);
        tsf_channel_set_sustain(f_, ch, 0);
        program(ch, 0);
    }
}

// 鳴っている音だけを止める。チャネルの設定と、どこまで読んだか (ランニングステータス) はそのまま
// (X68000 のリセットでは外の音源はリセットされない。ステートを読んだときは、このあと Machine が設定を送り直す)
void Sf2Synth::midiReset() {
    if (!f_) return;
    for (int ch = 0; ch < 16; ch++) tsf_channel_sounds_off_all(f_, ch);
}

void Sf2Synth::sysex(const uint8_t* p, int n) {
    if (MidiStream::isResetSysex(p, n)) resetChannels();
}

void Sf2Synth::midiByte(uint8_t b) {
    if (!f_) return;
    switch (stream_.feed(b)) {
    case MidiStream::NONE: return;
    case MidiStream::SYSEX:
        if (!stream_.sysexTruncated()) sysex(stream_.sysex(), stream_.sysexLength());
        // システムエクスクルーシブを終えたバイトがチャネルメッセージの始まりなら、そのまま次を待つ
        return;
    case MidiStream::CHANNEL: break;
    }
    const int ch = stream_.status() & 0x0f;
    const int d1 = stream_.data1(), d2 = stream_.data2();
    switch (stream_.status() & 0xf0) {
    case 0x80: tsf_channel_note_off(f_, ch, d1); break;
    case 0x90:
        if (d2 == 0) tsf_channel_note_off(f_, ch, d1);
        else tsf_channel_note_on(f_, ch, d1, d2 / 127.0f);
        break;
    case 0xb0: control(ch, d1, d2); break;
    case 0xc0: program(ch, d1); break;
    case 0xe0: tsf_channel_set_pitchwheel(f_, ch, d1 | d2 << 7); break;
    default: break; // キープレッシャー、チャネルプレッシャー
    }
}

// プログラムチェンジ。SF2 にないプログラムなら、バンク 0 のプログラム 0 (なければ最初の音色) にする (sf2_synth.h の説明)
void Sf2Synth::program(int ch, int p) {
    if (tsf_channel_set_presetnumber(f_, ch, p, ch == 9)) return;
    const int i = tsf_get_presetindex(f_, 0, 0);
    tsf_channel_set_presetindex(f_, ch, i < 0 ? 0 : i);
}

// コントロールチェンジ。RPN はここで扱い、ほかは TinySoundFont に渡す (sf2_synth.h の説明)
void Sf2Synth::control(int ch, int k, int v) {
    Rpn& r = rpn_[ch];
    switch (k) {
    case 6:
    case 38:
        if (r.nrpn || r.selMsb != 0 || r.selLsb > 2) return; // RPN 0-2 のほか (NRPN、RPN ヌルなど) は捨てる
        (k == 6 ? r.msb : r.lsb)[r.selLsb] = (uint8_t)v;
        applyRpn(ch, r.selLsb);
        return;
    case 96:
    case 97: return; // データインクリメント / デクリメントは受けない
    case 98:
    case 99: r.nrpn = 1; return;
    case 100:
        r.selLsb = (uint8_t)v;
        r.nrpn = 0;
        return;
    case 101:
        r.selMsb = (uint8_t)v;
        r.nrpn = 0;
        return;
    case 121: // TinySoundFont がピッチベンドの幅を 2、チューニングを 0 に戻す。RPN のデータもそれに合わせる
        tsf_channel_midi_control(f_, ch, 121, 0);
        r.reset();
        return;
    default: tsf_channel_midi_control(f_, ch, k, v); return;
    }
}

void Sf2Synth::applyRpn(int ch, int n) {
    const Rpn& r = rpn_[ch];
    if (n == 0) tsf_channel_set_pitchrange(f_, ch, r.msb[0] + 0.01f * r.lsb[0]);
    else tsf_channel_set_tuning(f_, ch, (r.msb[2] - 64) + ((r.msb[1] << 7 | r.lsb[1]) - 8192) / 8192.0f);
}

void Sf2Synth::render() {
    tsf_render_float(f_, fbuf_, BLOCK, 0);
    const float scale = 16384.0f * gain_;
    for (int i = 0; i < 2 * BLOCK; i++) {
        float v = fbuf_[i] * scale;
        v = v > 1e9f ? 1e9f : v < -1e9f ? -1e9f : v;
        buf_[i] = (int32_t)v;
    }
    pos_ = 0;
}

void Sf2Synth::renderFrames(int16_t* out, int n) {
    for (int i = 0; i < n; i++) {
        int32_t l = 0, r = 0;
        mixFrame(l, r);
        out[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
}

} // namespace platform
