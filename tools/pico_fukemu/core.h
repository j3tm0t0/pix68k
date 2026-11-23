// core.h -- ふろっぴU君 (FUK) の偽物の、USB に依らない部分。ホストの PC でも試験できる (test_host.c)
//
// FUK の手順 (2 本のポート、バイトの形式、レジスタの対応) は pix68k の docs/fuk.md と同じ。
// 本物の FUK は本物の FDD を読むが、この偽物は 2HD のイメージ (FD0 と FD1) を読み書きする。
// ふだんの Read はすぐに答える。設定で選んだ Read だけは (enum fukemu_rule。実物の動きはまだ確かめていない)、
// FDD の回転 (360rpm、500kbps) から計算した時刻に答える (#320 の試験用)。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FUKEMU_CYLS 77
#define FUKEMU_HEADS 2
#define FUKEMU_SECS 8
#define FUKEMU_SECSIZE 1024
#define FUKEMU_IMAGE_BYTES (FUKEMU_CYLS * FUKEMU_HEADS * FUKEMU_SECS * FUKEMU_SECSIZE) // 1261568
#define FUKEMU_UNITS 2      // イメージを置けるのは FD0 と FD1。ユニット 2 と 3 はディスクなし
#define FUKEMU_MAX_STRETCH 16

// 実時間で答える Read の、ポート 2 への出し方
enum fukemu_delivery {
    FUKEMU_STREAM = 0, // 'R' と長さをデータの先頭の時刻に、データを 1 バイトずつ流れる時刻に、リザルトを読み終わりに送る (既定)
    FUKEMU_HEAD = 1,   // 'R'、長さ、データをデータの先頭の時刻にまとめて、リザルトだけを読み終わりに送る
    FUKEMU_END = 2,    // 全部を読み終わりの時刻にまとめて送る
};

// どの Read を実時間で答えるか
enum fukemu_rule {
    FUKEMU_RT_NONE = 0,   // すべてすぐに答える
    FUKEMU_RT_PAIR = 1,   // R=7 から始まる Read と、その直後の R=8 EOT=8 (同じユニット、C、H) だけ
    FUKEMU_RT_SINGLE = 2, // 1 セクタだけの Read (R == EOT) はすべて
    FUKEMU_RT_ALL = 3,    // すべての Read
};

// セクタの記録を長くする (試験用)。そのセクタのデータが流れる時間を (1 + ppm / 1e6) 倍にする
typedef struct {
    uint8_t unit, c, h, r;
    int32_t ppm;
} fukemu_stretch_t;

typedef struct {
    uint8_t delivery; // enum fukemu_delivery
    uint8_t rule;     // enum fukemu_rule
    uint8_t nstretch;
    uint8_t one_sector; // 真なら、実時間の Read は最初のセクタだけを送って終える (R=7 EOT=8 でもセクタ 7 だけ)
    fukemu_stretch_t stretch[FUKEMU_MAX_STRETCH];
} fukemu_config_t;

typedef struct {
    uint64_t (*now_us)(void);                            // 単調に進む時刻 (マイクロ秒)
    void (*out1)(uint8_t b);                             // ポート 1 へ 1 バイト送る
    // ポート 2 へ送る。受け付けたバイト数を返す (送るバッファが一杯なら n より少ない。残りは core が次の poll で送り直す)
    size_t (*out2)(const uint8_t *p, size_t n);
    int (*write_image)(int unit, uint32_t off, const uint8_t *p, size_t n); // イメージに書く。0 で成功
    void (*log)(const char *s);                          // 無くてよい
    void (*dump_log)(void); // ポート 2 に 'L' が来たら、溜めた記録を送る (無くてよい。pix68k を閉じてから使う)
} fukemu_io_t;

// 設定を書かないときの既定: stream、pair、EOT まで読む、FD1 の C1 H0 R8 を 2.3% 長くする
void fukemu_default_config(fukemu_config_t *cfg);

// images[u] は FUKEMU_IMAGE_BYTES バイトのイメージ (NULL ならディスクなし)。wp[u] が真なら書き込み禁止
void fukemu_init(const fukemu_io_t *io, const fukemu_config_t *cfg, const uint8_t *const images[FUKEMU_UNITS],
                 const bool wp[FUKEMU_UNITS]);
void fukemu_port1_rx(uint8_t b);
void fukemu_port2_rx(uint8_t b);
void fukemu_poll(void); // 時刻が来た送信と、終わった仕事を進める。主ループで何度も呼ぶ
// 次に fukemu_poll() で送るものがある時刻 (無ければ UINT64_MAX)。ホストの試験と、待つ長さを決めるのに使う
uint64_t fukemu_next_due(void);

// 動作の数と、最後の Read (状態を問い合わせたときに返す。時刻は now_us の値)
typedef struct {
    uint32_t reads, rt_reads, writes, resets;
    uint8_t last_unit, last_c, last_h, last_r, last_eot, last_st0;
    bool last_rt;
    bool last_aborted;       // リザルトを送り終える前にリセットで取りやめた
    uint64_t last_cmd_us;    // コマンドを受け取った時刻 (0 ならまだ Read がない)
    uint64_t last_due_us;    // リザルトを送る予定の時刻
    uint64_t last_done_us;   // リザルトを送るバッファに渡し終えた時刻 (0 ならまだ)
} fukemu_stats_t;
void fukemu_get_stats(fukemu_stats_t *st);
// ドライブ unit をディスクなしにする (無線 LAN でイメージを書き始めるとき)。データの送受信やリザルトの途中
// (コマンドを受け終えてから、リザルトを読み終えるまで) なら何もせず false
bool fukemu_eject(int unit);

// ---- FDD の回転の計算 (試験から直接使う) ----
// X68000 の 2HD は 360rpm (1 回転 166666.67us)。端数を落とさないよう、1/3 us を単位にして数える
#define FUKEMU_REV_3US 500000u
#define FUKEMU_BYTE_US 16u // 500kbps の MFM で 1 バイト (1 回転 10416.67 バイト)
// 1 トラックのセクタを長くした分の合計の上限。GAP4b (654 バイト = 10474us) のうち、1ms をインデックスの前に残す
#define FUKEMU_MAX_EXTRA_US 9474u
// トラックの先頭 (インデックス) から、セクタ r (1〜8) の ID の SYNC の先頭までと、データの先頭までのバイト数
uint32_t fukemu_id_offset(int r);
uint32_t fukemu_data_offset(int r);
// 時刻 t にコマンドを受け取ったとき、セクタ r のデータが流れ始める時刻と流れ終わる時刻 (CRC の前)。
// extra_us[k] はセクタ k+1 のデータを長くした分 (後ろのセクタはその分ずれる)
void fukemu_sector_window(uint64_t t, int r, const uint32_t extra_us[FUKEMU_SECS], uint64_t *start, uint64_t *end);

#ifdef __cplusplus
}
#endif
