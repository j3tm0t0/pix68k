// 画面の出し方の設定 (pix68k.cfg の video_filter= と video_scale=) の番号と名前。効果の中身は platform/crt_effect.h。
// 設定 (platform/settings.h) と設定メニュー (platform/menu.h) も使うので、番号だけを分けてある。
// video_filter= は効果のビット (CrtPreset) を重ねた数 (0..7)。中の名前は CrtPreset のまま。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

namespace platform {

enum CrtPreset : int {
    CRT_OFF = 0,
    CRT_SCANLINE = 1, // スキャンライン
    CRT_CURVE = 2,    // たる型のゆがみと四隅を暗くする
    CRT_MASK = 4,     // アパーチャーグリル (RGB の縦じま) とにじみ
    CRT_PRESET_COUNT = 8, // 効果の 3 ビットの組み合わせの数 (video_filter= の下の 3 ビット。枠のビットは含まない)
    // モニタの枠 (#230。platform/bezel.h)。どちらか 1 つ。効果のビットとは別に、その上のビットに置く
    CRT_BEZEL_DE = 8,  // cz-600de (グレー)
    CRT_BEZEL_DB = 16, // cz-600db (黒)
    CRT_EFFECT_BITS = CRT_SCANLINE | CRT_CURVE | CRT_MASK,
    CRT_BEZEL_BITS = CRT_BEZEL_DE | CRT_BEZEL_DB,
    // #229 の crt= の組み合わせ (crt= はもう読まない。試験と説明で使う)
    CRT_GRILLE = CRT_SCANLINE | CRT_MASK,
    CRT_GEOM = CRT_SCANLINE | CRT_CURVE,
    CRT_FULL = CRT_SCANLINE | CRT_CURVE | CRT_MASK,
};

// video_filter= に書ける名前と、そのビット (platform/settings.h の video_filter= の項目が使う)。
// 名前を増やすときは、ここに足す。知らない名前は読むときに飛ばし、ログで知らせる。
// 枠の 2 つ (kVideoFilterExclusive) はどちらか 1 つで、両方を書いたらあとに書いたほうを使う (ログで知らせる)
static const char* const kVideoFilterNames[] = {"scanline", "curve", "mask", "cz-600de", "cz-600db", nullptr};
static const int kVideoFilterBits[] = {CRT_SCANLINE, CRT_CURVE, CRT_MASK, CRT_BEZEL_DE, CRT_BEZEL_DB};
static constexpr int kVideoFilterExclusive = CRT_BEZEL_BITS;

// 枠の番号 (0 なら枠なし、1 は cz-600de、2 は cz-600db)
inline int videoBezel(int filter) { return (filter & CRT_BEZEL_BITS) == CRT_BEZEL_DB ? 2 : (filter & CRT_BEZEL_DE) ? 1 : 0; }
// 枠の名前 (video_filter= に書く名前。枠なしなら nullptr)
inline const char* videoBezelName(int filter) {
    const int b = videoBezel(filter);
    return b == 1 ? "cz-600de" : b == 2 ? "cz-600db" : nullptr;
}

// video_scale= の値
enum VideoScale : int {
    VIDEO_SCALE_OFF = 0,     // 1 倍 (拡大しない)
    VIDEO_SCALE_INTEGER = 1, // 整数倍
    VIDEO_SCALE_ASPECT = 2,  // 4:3 を保って高さいっぱい
    VIDEO_SCALE_FULL = 3,    // 画面全体に引き伸ばす
    VIDEO_SCALE_COUNT = 4,
};
// video_scale= を書いていないときの値。ベアメタル版の Pi 2 だけ integer (960x540 のフレームバッファを GPU が 2 倍するので、
// 4:3 に伸ばすと写す量が増え、縦横比も整数倍でなくなる)。ほかは aspect (#365)
#if defined(RASPPI) && RASPPI == 2
inline constexpr int kDefaultVideoScale = VIDEO_SCALE_INTEGER;
#else
inline constexpr int kDefaultVideoScale = VIDEO_SCALE_ASPECT;
#endif

// 表を作って写す効果か (ゆがみかマスクがある。スキャンラインだけなら、整数倍では表を使わない)
inline bool crtNeedsMap(int preset) { return (preset & (CRT_CURVE | CRT_MASK)) != 0; }

// Pi 3 (Cortex-A53) で選べる組み合わせ。写すのは合成するコアで、1 フレームのうちに合成と写しを終える必要がある。
// 実機で測った時間は docs/baremetal.md の「画面の出し方」。ゆがみ (curve) は表示の画素を 1 つずつ表で引くので、どの枠でも
// 収まらない。full (1920x1080 で 207 万画素) と mask (アパーチャーグリルとにじみ) の組も収まらない
// 枠 (cz-600de、cz-600db) は、制限を変えない (枠を出せるかは絵を読んで画面モードが決まるまで分からず、枠なしで
// 写すときに video_scale= のとおりの重さになるため)。ガラスの矩形 (1160x870) は aspect (1440x1080) より小さいので、
// 枠があってもなくても、この制限の中なら収まる
inline bool videoAllowedPi3(int scale, int filter) {
    if (filter & CRT_CURVE) return false;
    return !(scale == VIDEO_SCALE_FULL && (filter & CRT_MASK));
}

// video_filter= に書く形 (「scanline,curve,mask」の順に , でつなぐ。どれもなければ「none」)。ログ用。
// 保存するときは platform/settings.h の formatSetting() が kVideoFilterNames から作る
// 枠があれば、効果のあとに枠の名前を足す (「scanline,curve,cz-600de」。枠だけなら「cz-600de」)
inline const char* videoFilterName(int filter) {
    static const char* const names[3][CRT_PRESET_COUNT] = {
        {"none", "scanline", "curve", "scanline,curve", "mask", "scanline,mask", "curve,mask", "scanline,curve,mask"},
        {"cz-600de", "scanline,cz-600de", "curve,cz-600de", "scanline,curve,cz-600de", "mask,cz-600de",
         "scanline,mask,cz-600de", "curve,mask,cz-600de", "scanline,curve,mask,cz-600de"},
        {"cz-600db", "scanline,cz-600db", "curve,cz-600db", "scanline,curve,cz-600db", "mask,cz-600db",
         "scanline,mask,cz-600db", "curve,mask,cz-600db", "scanline,curve,mask,cz-600db"},
    };
    if (filter < 0 || (filter & ~(CRT_EFFECT_BITS | CRT_BEZEL_BITS)) != 0 ||
        (filter & CRT_BEZEL_BITS) == CRT_BEZEL_BITS)
        return "?";
    return names[videoBezel(filter)][filter & CRT_EFFECT_BITS];
}

// video_scale= に書く名前
inline const char* videoScaleName(int scale) {
    static const char* const names[VIDEO_SCALE_COUNT] = {"off", "integer", "aspect", "full"};
    return scale >= 0 && scale < VIDEO_SCALE_COUNT ? names[scale] : "?";
}

} // namespace platform
