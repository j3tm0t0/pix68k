// core.c -- ふろっぴU君 (FUK) の偽物の FDC (μPD72065) と FDD のモデル。手順は README.md と pix68k の docs/fuk.md
#include "core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ---- トラックの並び (2HD、1 トラック 8 セクタ x 1024 バイト) ----
// インデックスのあと: GAP4a 80、SYNC 12、IAM 4、GAP1 50
// 各セクタ: SYNC 12、IDAM 4、ID 4、CRC 2、GAP2 22、SYNC 12、DAM 4、データ 1024、CRC 2、GAP3 116 (X68000 の 2HD のフォーマットの GPL 0x74)
// 8 セクタで 9762 バイト。残り (1 回転 10416.67 バイトまで) は GAP4b
#define PRE_BYTES (80 + 12 + 4 + 50)
#define ID_TO_DATA (12 + 4 + 4 + 2 + 22 + 12 + 4)
#define SECTOR_BYTES (ID_TO_DATA + FUKEMU_SECSIZE + 2 + 116)
#define CRC_US (2 * FUKEMU_BYTE_US)

uint32_t fukemu_id_offset(int r) { return PRE_BYTES + (uint32_t)(r - 1) * SECTOR_BYTES; }
uint32_t fukemu_data_offset(int r) { return fukemu_id_offset(r) + ID_TO_DATA; }

// セクタ r の ID の先頭の、インデックスからの時間 (前のセクタを長くした分を含む)
static uint32_t id_time(int r, const uint32_t extra_us[FUKEMU_SECS])
{
    uint32_t t = fukemu_id_offset(r) * FUKEMU_BYTE_US;
    for (int k = 0; k < r - 1; k++) t += extra_us[k];
    return t;
}

// 時刻 t に探し始めて、次に頭の下に来るセクタ r の ID の、回転の先頭の時刻
static uint64_t rev_base(uint64_t t, int r, const uint32_t extra_us[FUKEMU_SECS])
{
    uint64_t t3 = t * 3, base3 = t3 - t3 % FUKEMU_REV_3US;
    if (t3 - base3 > (uint64_t)id_time(r, extra_us) * 3) base3 += FUKEMU_REV_3US; // ID の先頭を過ぎていたら次の回転
    return (base3 + 2) / 3; // 1us に切り上げる
}

static void window_at(uint64_t base, int r, const uint32_t extra_us[FUKEMU_SECS], uint64_t *start, uint64_t *end)
{
    *start = base + id_time(r, extra_us) + ID_TO_DATA * FUKEMU_BYTE_US;
    *end = *start + FUKEMU_SECSIZE * FUKEMU_BYTE_US + extra_us[r - 1];
}

void fukemu_sector_window(uint64_t t, int r, const uint32_t extra_us[FUKEMU_SECS], uint64_t *start, uint64_t *end)
{
    window_at(rev_base(t, r, extra_us), r, extra_us, start, end);
}

// ---- 状態 ----
enum phase { PH_CMD, PH_OUT, PH_WRITE_M, PH_WRITE_DATA, PH_RESULT };

static const uint8_t cmd_length[32] = {
    1, 1, 9, 3, 2, 9, 9, 2, 1, 9, 2, 1, 9, 6, 1, 3, 1, 9, 1, 1, 1, 1, 1, 1, 1, 9, 1, 1, 1, 9, 1, 1,
};

static const fukemu_io_t *io;
static fukemu_config_t cfg;
static const uint8_t *img[FUKEMU_UNITS];
static bool wprot[FUKEMU_UNITS];

static enum phase phase;
static uint8_t cmd[9];
static int cmd_len, cmd_need;
static uint8_t res[7];
static int res_len, res_pos;
static bool res_lower_int; // リザルトの 1 バイト目を読んだら FDC の割り込みを下ろす (READ ID)
static bool res_sis;       // SENSE INTERRUPT STATUS のリザルト (ST0 を読んだらユニットのビジーを解く)
static bool int_line;
static bool motor, force_ready;
static int sel_unit = -1;  // $E94005 で最後に選んだユニット
static uint16_t mtc;       // Write の前に知らせてくる DMA の転送バイト数
static int p1_reg = -1;    // ポート 1: 値を待っている書き込みのレジスタ

static struct {
    uint8_t pcn;
    bool busy;      // シーク中 (MSR の DnB)。SENSE INTERRUPT STATUS で引き取るまで
    bool seek_run;  // シークを進めている
    uint64_t seek_due;
    bool seek_end;  // シークの終わりを SENSE INTERRUPT STATUS で引き取るのを待つ
    uint8_t seek_st0;
} u[4];

// 直前の Read がセクタ 7 の単独読み (実時間) だったか (FUKEMU_RT_PAIR)
static struct {
    bool valid;
    uint8_t unit, c, h;
} last7;

// Write のデータ
static uint8_t wbuf[16384];
static uint32_t w_need, w_got;

// ポート 2 へ送る予定。順に送る。dur_us が 0 なら due に全部、そうでなければ due から dur_us かけて 1 バイトずつ
#define MAX_SEGS (FUKEMU_SECS + 2)
static struct seg {
    uint64_t due;
    uint32_t dur_us;
    const uint8_t *p;
    uint32_t len, sent;
    bool timed; // 実時間の送信 (記録に残す)
} segs[MAX_SEGS];
static int nsegs, seg_pos;
static uint8_t out_hdr[3], out_res[7];
static fukemu_stats_t stats;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    if (!io->log) return;
    char s[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    io->log(s);
}

// 通知のバイト ('C'、'c'、'W'、'w'、'F')。送るバッファが一杯でも待たずに溜め、poll で送る
// ファームでは、送る経路 (fukemu_poll から USB の送るバッファに渡すまで) を RAM に置く。
// コア 1 の無線 LAN の処理と XIP のキャッシュを取り合って、送る時刻が揺れないようにするため (ホストの試験では何もしない)
#if PICO_FUKEMU_FIRMWARE
#include "pico/platform/sections.h"
#define RAM_FUNC(f) __time_critical_func(f)
#else
#define RAM_FUNC(f) f
#endif

static uint8_t ctl[64];
static unsigned ctl_head, ctl_len;
static bool line_sent; // 最後に送り出した 'C' / 'c' (ホストが知っている割り込みの線)
static void RAM_FUNC(flush_ctl)(void)
{
    while (ctl_len) {
        unsigned k = ctl_head + ctl_len <= sizeof ctl ? ctl_len : (unsigned)sizeof ctl - ctl_head;
        size_t n = io->out2(ctl + ctl_head, k);
        for (size_t i = 0; i < n; i++)
            if (ctl[ctl_head + i] == 'C' || ctl[ctl_head + i] == 'c') line_sent = ctl[ctl_head + i] == 'C';
        ctl_head = (ctl_head + (unsigned)n) % sizeof ctl;
        ctl_len -= (unsigned)n;
        if (n < k) return;
    }
}
static void out2c(uint8_t c)
{
    // 'C' と 'c' は割り込みの線の今の状態を知らせるだけなので、まだ送っていない逆の通知があれば、足さずに消す。
    // 詰まっている間に割り込みが何度上下しても、溜まるのは 1 つまで
    if (ctl_len && (c == 'C' || c == 'c')) {
        unsigned last = (ctl_head + ctl_len - 1) % sizeof ctl;
        if (ctl[last] == (c == 'C' ? 'c' : 'C')) {
            ctl_len--;
            flush_ctl();
            return;
        }
    }
    if (ctl_len < sizeof ctl) ctl[(ctl_head + ctl_len++) % sizeof ctl] = c;
    flush_ctl();
}
static void raise_int(void)
{
    if (!int_line) {
        int_line = true;
        out2c('C');
    }
}
static void lower_int(void)
{
    if (int_line) {
        int_line = false;
        out2c('c');
    }
}

static bool has_disk(int n) { return n >= 0 && n < FUKEMU_UNITS && img[n] != NULL; }
static bool ready(int n) { return force_ready || (motor && has_disk(n)); }

static uint8_t msr(void)
{
    uint8_t b = 0;
    for (int n = 0; n < 4; n++)
        if (u[n].busy) b |= (uint8_t)(1 << n);
    switch (phase) {
    case PH_CMD: return (uint8_t)((cmd_len ? 0x90 : 0x80) | b);
    case PH_RESULT: return (uint8_t)(0xD0 | b);
    default: return (uint8_t)(0x10 | b);
    }
}

// コマンドを待つ段階に戻る。シークの終わりを引き取っていないユニットがあれば割り込みを立て直す
static void back_to_cmd(void)
{
    phase = PH_CMD;
    cmd_len = 0;
    res_sis = false;
    for (int n = 0; n < 4; n++)
        if (u[n].seek_end) raise_int();
}

static void to_result(int len, bool lower_on_first)
{
    res_len = len;
    res_pos = 0;
    res_lower_int = lower_on_first;
    phase = PH_RESULT;
}

static void result7(uint8_t st0, uint8_t st1, uint8_t st2, uint8_t c, uint8_t h, uint8_t r, uint8_t n)
{
    res[0] = st0, res[1] = st1, res[2] = st2, res[3] = c, res[4] = h, res[5] = r, res[6] = n;
}

static uint32_t offset(int c, int h, int r)
{
    return ((uint32_t)(c * FUKEMU_HEADS + h) * FUKEMU_SECS + (uint32_t)(r - 1)) * FUKEMU_SECSIZE;
}

static bool chs_ok(int c, int h, int r, int n)
{
    return n == 3 && c < FUKEMU_CYLS && h < FUKEMU_HEADS && r >= 1 && r <= FUKEMU_SECS;
}

static void add_seg(uint64_t due, uint32_t dur_us, const uint8_t *p, uint32_t len, bool timed)
{
    struct seg *s = &segs[nsegs++];
    s->due = due, s->dur_us = dur_us, s->p = p, s->len = len, s->sent = 0, s->timed = timed;
}

// そのトラックの各セクタを長くした分 (マイクロ秒)
// 合計が FUKEMU_MAX_EXTRA_US を超える分は足さない (トラックが 1 回転に収まるように)
static void track_extra(int un, int c, int h, uint32_t extra[FUKEMU_SECS])
{
    memset(extra, 0, FUKEMU_SECS * sizeof extra[0]);
    for (int i = 0; i < cfg.nstretch; i++) { // 同じセクタを何度も書いたら、最後のもの
        const fukemu_stretch_t *s = &cfg.stretch[i];
        if (s->unit == un && s->c == c && s->h == h && s->r >= 1 && s->r <= FUKEMU_SECS)
            extra[s->r - 1] =
                s->ppm > 0 ? (uint32_t)((uint64_t)FUKEMU_SECSIZE * FUKEMU_BYTE_US * (uint32_t)s->ppm / 1000000u) : 0;
    }
    uint32_t sum = 0;
    for (int k = 0; k < FUKEMU_SECS; k++) { // セクタの順に足し、上限を超える分は落とす
        if (extra[k] > FUKEMU_MAX_EXTRA_US - sum) extra[k] = FUKEMU_MAX_EXTRA_US - sum;
        sum += extra[k];
    }
}

// PAIR: R=7 から始まる Read と、その直後の同じトラックの R=8 EOT=8 を実時間にする
static bool realtime(int un, int c, int h, int r, int eot)
{
    bool single = r == eot;
    switch (cfg.rule) {
    case FUKEMU_RT_ALL: return true;
    case FUKEMU_RT_SINGLE: return single;
    case FUKEMU_RT_PAIR: {
        bool after7 = last7.valid && last7.unit == un && last7.c == c && last7.h == h;
        last7.valid = false;
        if (r == 7) {
            last7.valid = true, last7.unit = (uint8_t)un, last7.c = (uint8_t)c, last7.h = (uint8_t)h;
            return true;
        }
        return single && r == 8 && after7;
    }
    default: return false;
    }
}

// Read: 'R' 長さ (2 バイト、下位が先) データ リザルト (7)。R から EOT まで読み、リザルトの R は EOT にする。
// リザルトの段階には入らず、割り込みも出さない (pix68k が作る)
static void start_read(void)
{
    int un = cmd[1] & 3, hd = (cmd[1] >> 2) & 1;
    uint8_t us = cmd[1] & 7;
    uint8_t c = cmd[2], h = cmd[3], r = cmd[4], n = cmd[5], eot = cmd[6];
    uint32_t nsec = 0;
    bool rt = false;
    (void)hd;
    if (!ready(un) || !has_disk(un)) {
        result7((uint8_t)(0x48 | us), 0, 0, c, h, r, n);
    } else if (!chs_ok(c, h, r, n) || eot < r || eot > FUKEMU_SECS) {
        result7((uint8_t)(0x40 | us), 0x01, 0, c, h, r, n); // ID が見つからない (MA)
    } else {
        rt = realtime(un, c, h, r, eot);
        // one_sector: 実時間の Read は、最初のセクタで終える (1 セクタで TC が来たときの FDC と同じ)
        if (rt && cfg.one_sector) eot = r;
        nsec = (uint32_t)(eot - r + 1);
        result7(us, 0, 0, c, h, eot, n);
        u[un].pcn = c;
    }
    uint32_t len = nsec * FUKEMU_SECSIZE + 7;
    out_hdr[0] = 'R', out_hdr[1] = (uint8_t)len, out_hdr[2] = (uint8_t)(len >> 8);
    memcpy(out_res, res, 7);
    nsegs = seg_pos = 0;
    uint64_t now = io->now_us();
    if (rt) {
        uint32_t extra[FUKEMU_SECS];
        track_extra(un, c, h, extra);
        uint64_t base = rev_base(now, r, extra), s0, e0, s, e = 0;
        window_at(base, r, extra, &s0, &e0);
        uint64_t fin = 0;
        for (int k = r; k <= eot; k++) {
            window_at(base, k, extra, &s, &e);
            fin = e + CRC_US;
        }
        const uint8_t *data = img[un] + offset(c, h, r);
        switch (cfg.delivery) {
        case FUKEMU_HEAD:
            add_seg(s0, 0, out_hdr, 3, true);
            add_seg(s0, 0, data, nsec * FUKEMU_SECSIZE, true);
            break;
        case FUKEMU_END:
            add_seg(fin, 0, out_hdr, 3, true);
            add_seg(fin, 0, data, nsec * FUKEMU_SECSIZE, true);
            break;
        default: // FUKEMU_STREAM: セクタごとに、データが流れる時刻に合わせて
            add_seg(s0, 0, out_hdr, 3, true);
            for (int k = r; k <= eot; k++) {
                window_at(base, k, extra, &s, &e);
                add_seg(s, (uint32_t)(e - s), data + (uint32_t)(k - r) * FUKEMU_SECSIZE, FUKEMU_SECSIZE, true);
            }
            break;
        }
        add_seg(fin, 0, out_res, 7, true);
        stats.rt_reads++;
        logf_("rt read U%d C%d H%d R%d-%d at %llu: data %llu-%llu result %llu (mode %d)", un, c, h, r, eot,
              (unsigned long long)now, (unsigned long long)s0, (unsigned long long)e, (unsigned long long)fin,
              cfg.delivery);
    } else {
        if (!nsec) last7.valid = false;
        add_seg(now, 0, out_hdr, 3, false);
        if (nsec) add_seg(now, 0, img[un] + offset(c, h, r), nsec * FUKEMU_SECSIZE, false);
        add_seg(now, 0, out_res, 7, false);
        logf_("read U%d C%d H%d R%d-%d st0 %02x", un, c, h, r, eot, res[0]);
    }
    stats.reads++;
    stats.last_unit = (uint8_t)un, stats.last_c = c, stats.last_h = h, stats.last_r = r, stats.last_eot = eot;
    stats.last_st0 = res[0], stats.last_rt = rt, stats.last_cmd_us = now;
    stats.last_due_us = segs[nsegs - 1].due, stats.last_done_us = 0, stats.last_aborted = false;
    phase = PH_OUT;
}

// Write の要求: 書けないときは 'W' を飛ばして 'w' (リザルトは pix68k がポート 1 で読む)
static void start_write(void)
{
    int un = cmd[1] & 3;
    uint8_t us = cmd[1] & 7;
    if (!ready(un) || !has_disk(un)) {
        result7((uint8_t)(0x48 | us), 0, 0, cmd[2], cmd[3], cmd[4], cmd[5]);
        to_result(7, false);
        out2c('w');
        return;
    }
    w_need = mtc;
    w_got = 0;
    phase = PH_WRITE_M;
    out2c('W');
}

static int write_img(int un, uint32_t off, const uint8_t *p, size_t n)
{
    if (off + n > FUKEMU_IMAGE_BYTES) return -1;
    return io->write_image(un, off, p, n);
}

static void finish_write(void)
{
    stats.writes++;
    int op = cmd[0] & 0x1F, un = cmd[1] & 3;
    uint8_t us = cmd[1] & 7;
    uint32_t got = w_got < sizeof wbuf ? w_got : (uint32_t)sizeof wbuf;
    if (wprot[un]) {
        result7((uint8_t)(0x40 | us), 0x02, 0, cmd[2], cmd[3], cmd[4], cmd[5]); // 書き込み禁止 (NW)
    } else if (op == 0x0D) { // WRITE ID: ID (C, H, R, N) が SC 個、4 バイトずつ。セクタを D (cmd[5]) で埋める
        uint32_t sc = cmd[3];
        bool ok = sc > 0 && got >= sc * 4;
        for (uint32_t i = 0; ok && i < sc; i++)
            ok = chs_ok(wbuf[i * 4], wbuf[i * 4 + 1], wbuf[i * 4 + 2], wbuf[i * 4 + 3]);
        if (!ok) { // このイメージの形 (2HD、1024 バイト x 8) でないフォーマットは、書かずに異常終了
            result7((uint8_t)(0x40 | us), 0x04, 0, cmd[2], cmd[3], cmd[4], cmd[5]);
            logf_("write id U%d: %u ids, not this format", un, (unsigned)(got / 4));
        } else {
            static uint8_t fill[FUKEMU_SECS * FUKEMU_SECSIZE];
            memset(fill, cmd[5], sizeof fill);
            int e = 0;
            // イメージの上で続いているセクタはまとめて書く (フラッシュの 4KB を何度も消さないように)
            for (uint32_t i = 0; i < sc;) {
                uint32_t off = offset(wbuf[i * 4], wbuf[i * 4 + 1], wbuf[i * 4 + 2]), n = 1;
                while (i + n < sc && n < FUKEMU_SECS &&
                       offset(wbuf[(i + n) * 4], wbuf[(i + n) * 4 + 1], wbuf[(i + n) * 4 + 2]) == off + n * FUKEMU_SECSIZE)
                    n++;
                if (write_img(un, off, fill, n * FUKEMU_SECSIZE)) e = 1;
                i += n;
            }
            const uint8_t *last = &wbuf[(sc - 1) * 4];
            result7(e ? (uint8_t)(0x40 | us) : us, e ? 0x10 : 0, 0, last[0], last[1], last[2], last[3]);
            u[un].pcn = last[0];
            logf_("write id U%d C%d H%d %u ids -> %d", un, last[0], last[1], (unsigned)sc, e);
        }
    } else if (op == 0x05 || op == 0x09) {
        uint8_t c = cmd[2], h = cmd[3], r = cmd[4], n = cmd[5], eot = cmd[6];
        if (!chs_ok(c, h, r, n) || eot < r || eot > FUKEMU_SECS) {
            result7((uint8_t)(0x40 | us), 0x01, 0, c, h, r, n);
        } else {
            uint32_t room = (uint32_t)(eot - r + 1) * FUKEMU_SECSIZE;
            uint32_t bytes = got < room ? got : room;
            uint32_t nsec = bytes ? (bytes + FUKEMU_SECSIZE - 1) / FUKEMU_SECSIZE : 1;
            int e = bytes ? write_img(un, offset(c, h, r), wbuf, bytes) : 0;
            uint8_t last = (uint8_t)(r + nsec - 1);
            result7(e ? (uint8_t)(0x40 | us) : us, e ? 0x10 : 0, 0, c, h, last, n);
            u[un].pcn = c;
            logf_("write U%d C%d H%d R%d-%d %u bytes -> %d", un, c, h, r, last, (unsigned)bytes, e);
        }
    } else { // SCAN は扱わない (失敗を返す)
        result7((uint8_t)(0x40 | us), 0, 0, cmd[2], cmd[3], cmd[4], cmd[5]);
    }
    to_result(7, false);
    out2c('w');
}

static void execute(void)
{
    int op = cmd[0] & 0x1F, un = cmd[1] & 3, hd = (cmd[1] >> 2) & 1;
    uint8_t us = cmd[1] & 7;
    // セクタ 7 の読みと 8 の読みの間に、ステータスを見るコマンド (SENSE INTERRUPT STATUS、SENSE DEVICE STATUS、
    // SPECIFY) と、同じシリンダへの SEEK 以外が挟まったら、続けて読んだとはみなさない。
    bool same_seek = op == 0x0F && last7.valid && un == last7.unit && cmd[2] == last7.c;
    if (op != 0x02 && op != 0x06 && op != 0x0C && op != 0x08 && op != 0x04 && op != 0x03 && !same_seek)
        last7.valid = false;
    switch (op) {
    case 0x02: case 0x06: case 0x0C: // READ DIAGNOSTIC / READ DATA / READ DELETED DATA
        start_read();
        return;
    case 0x05: case 0x09: case 0x0D: // WRITE DATA / WRITE DELETED DATA / WRITE ID
    case 0x11: case 0x19: case 0x1D: // SCAN (データは Write と同じ手順で届く)
        start_write();
        return;
    case 0x03: // SPECIFY
        back_to_cmd();
        return;
    case 0x04: { // SENSE DEVICE STATUS: ST3
        bool r = ready(un);
        res[0] = (uint8_t)((has_disk(un) && wprot[un] ? 0x40 : 0) | (r ? 0x20 : 0) | (r && u[un].pcn == 0 ? 0x10 : 0) |
                           0x08 | us);
        to_result(1, false);
        return;
    }
    case 0x07: case 0x0F: { // RECALIBRATE / SEEK。終わったら割り込み ('C')
        uint8_t ncn = op == 0x07 ? 0 : cmd[2];
        u[un].busy = true;
        u[un].seek_run = true;
        u[un].seek_due = io->now_us();
        u[un].pcn = ncn;
        u[un].seek_st0 = (uint8_t)(0x20 | us | (ready(un) ? 0 : 0x48));
        back_to_cmd();
        return;
    }
    case 0x08: { // SENSE INTERRUPT STATUS
        lower_int();
        int len = 1;
        res[0] = 0x80;
        for (int n = 0; n < 4; n++) {
            if (u[n].seek_end) {
                u[n].seek_end = false;
                res[0] = u[n].seek_st0;
                res[1] = u[n].pcn;
                len = 2;
                break;
            }
        }
        res_sis = true;
        to_result(len, false);
        return;
    }
    case 0x0A: // READ ID: リザルトを用意して割り込み。1 バイト目を読まれたら下ろす
        result7(ready(un) && has_disk(un) ? us : (uint8_t)(0x48 | us), 0, 0, u[un].pcn, (uint8_t)hd, 1, 3);
        to_result(7, true);
        raise_int();
        return;
    case 0x14: case 0x15: case 0x16: // RESET STANDBY / SET STANDBY / SOFTWARE RESET
        back_to_cmd();
        return;
    default: // 不正なコマンド
        res[0] = 0x80;
        to_result(1, false);
        return;
    }
}

static uint8_t read_result(void)
{
    if (phase != PH_RESULT) return 0;
    uint8_t b = res[res_pos++];
    if (res_pos == 1) {
        if (res_lower_int) lower_int();
        if (res_sis && b != 0x80) u[b & 3].busy = false;
    }
    if (res_pos >= res_len) back_to_cmd();
    return b;
}

static void reset(void)
{
    stats.resets++;
    if (phase == PH_OUT && stats.reads && !stats.last_done_us) stats.last_aborted = true;
    phase = PH_CMD;
    cmd_len = 0;
    mtc = 0;
    nsegs = seg_pos = 0;
    for (int n = 0; n < 4; n++) u[n].busy = u[n].seek_run = u[n].seek_end = false;
    // モータとレディも初めに戻す (pix68k の起動ごとに、モータを回したところから始まる)
    motor = force_ready = false;
    last7.valid = false;
    // まだ送れていない通知は捨てる (ホストが閉じている間に溜まった古い 'C' や 'W' を、リセットのあとに届けない)。
    // 割り込みの線は、ホストが最後に受け取った状態から下げ直す
    ctl_len = 0;
    int_line = line_sent;
    lower_int();
}

static void reg_write(int reg, uint8_t v)
{
    switch (reg) {
    case 0x03:
        if (phase != PH_CMD) return;
        if (cmd_len == 0) {
            cmd_need = cmd_length[v & 0x1F];
            memset(cmd, 0, sizeof cmd);
        }
        cmd[cmd_len++] = v;
        if (cmd_len >= cmd_need) execute();
        return;
    case 0x05: // ドライブ制御: bit0-3 ユニットの選択、bit5 排出 (無視)、bit6 排出禁止、bit7 点滅
        sel_unit = v & 15 ? __builtin_ctz(v & 15) : -1;
        return;
    case 0x07: // ドライブ選択: bit7 モータ。レディが変わっても割り込みは出さない (出すと IPL が止まる)
        motor = (v & 0x80) != 0;
        return;
    case 0x23: force_ready = (v & 0x40) != 0; return;
    case 0x30: mtc = (uint16_t)((mtc & 0xFF00) | v); return;
    case 0x31: mtc = (uint16_t)((mtc & 0x00FF) | v << 8); return;
    default: return; // 0x00 (初期化の 0xFE)、0x10〜0x1F (I/O 割り込み)
    }
}

static uint8_t reg_read(int reg)
{
    switch (reg) {
    case 0x00: return 'f';
    case 0x01: return msr();
    case 0x03: return read_result();
    case 0x05: return has_disk(sel_unit) ? 0x80 : 0x00;
    default:
        if (reg >= 0x10 && reg <= 0x1F) return int_line ? 0x80 : 0x00;
        return 0xFF;
    }
}

void fukemu_port1_rx(uint8_t b)
{
    if (p1_reg >= 0) {
        int r = p1_reg;
        p1_reg = -1;
        reg_write(r, b);
        return;
    }
    if (b & 0x80) {
        p1_reg = b & 0x7F;
        return;
    }
    if (b == 0x40) {
        logf_("reset");
        reset();
        return;
    }
    fukemu_poll(); // 終わったシークを、ステータスに出す前に進める
    io->out1(reg_read(b));
}

void fukemu_port2_rx(uint8_t b)
{
    if (phase == PH_WRITE_DATA) {
        if (w_got < sizeof wbuf) wbuf[w_got] = b;
        if (++w_got >= w_need) finish_write();
        return;
    }
    if (phase == PH_WRITE_M && b == 'M') {
        phase = PH_WRITE_DATA;
        if (w_need == 0) finish_write();
        return;
    }
    if (b == 0x00) out2c('F');
    else if (b == 'L' && phase == PH_CMD && io->dump_log) io->dump_log();
}

static void RAM_FUNC(send_segs)(uint64_t now)
{
    while (seg_pos < nsegs) {
        struct seg *s = &segs[seg_pos];
        if (now < s->due) return;
        uint32_t upto = s->len;
        if (s->dur_us) { // バイト j は due + (j + 1) * dur / len に読み終わる
            uint64_t k = (now - s->due) * s->len / s->dur_us;
            if (k < upto) upto = (uint32_t)k;
        }
        if (upto > s->sent) s->sent += (uint32_t)io->out2(s->p + s->sent, upto - s->sent);
        if (s->sent < s->len) return;
        // queued は送るバッファに渡し終えた時刻 (USB で送り終えた時刻ではない)
        if (s->timed) logf_("rt seg %d (%u bytes) due %llu queued %llu", seg_pos, (unsigned)s->len,
                            (unsigned long long)(s->due + s->dur_us), (unsigned long long)io->now_us());
        seg_pos++;
    }
    if (phase == PH_OUT) {
        stats.last_done_us = io->now_us();
        back_to_cmd();
    }
}

void RAM_FUNC(fukemu_poll)(void)
{
    uint64_t now = io->now_us();
    flush_ctl();
    if (phase == PH_OUT && !ctl_len) send_segs(now);
    for (int n = 0; n < 4; n++) {
        if (u[n].seek_run && now >= u[n].seek_due) {
            u[n].seek_run = false;
            u[n].seek_end = true;
            if (phase == PH_CMD) raise_int(); // ほかの段階なら、戻ったときに立てる
        }
    }
}

void fukemu_get_stats(fukemu_stats_t *st) { *st = stats; }

bool fukemu_eject(int unit)
{
    if (unit < 0 || unit >= FUKEMU_UNITS || phase != PH_CMD) return false;
    img[unit] = NULL;
    return true;
}

uint64_t RAM_FUNC(fukemu_next_due)(void)
{
    uint64_t t = UINT64_MAX;
    if (ctl_len) return 0;
    if (phase == PH_OUT && seg_pos < nsegs) {
        const struct seg *s = &segs[seg_pos];
        t = s->dur_us ? s->due + ((uint64_t)(s->sent + 1) * s->dur_us + s->len - 1) / s->len : s->due;
    }
    for (int n = 0; n < 4; n++)
        if (u[n].seek_run && u[n].seek_due < t) t = u[n].seek_due;
    return t;
}

void fukemu_default_config(fukemu_config_t *c)
{
    memset(c, 0, sizeof *c);
    c->delivery = FUKEMU_STREAM;
    c->rule = FUKEMU_RT_PAIR;
    c->one_sector = 0;
    c->nstretch = 1;
    c->stretch[0] = (fukemu_stretch_t){1, 1, 0, 8, 23000};
}

void fukemu_init(const fukemu_io_t *io_, const fukemu_config_t *cfg_, const uint8_t *const images[FUKEMU_UNITS],
                 const bool wp[FUKEMU_UNITS])
{
    io = io_;
    memset(&cfg, 0, sizeof cfg);
    if (cfg_) cfg = *cfg_;
    if (cfg.nstretch > FUKEMU_MAX_STRETCH) cfg.nstretch = FUKEMU_MAX_STRETCH;
    for (int n = 0; n < FUKEMU_UNITS; n++) {
        img[n] = images ? images[n] : NULL;
        wprot[n] = wp ? wp[n] : false;
    }
    memset(u, 0, sizeof u);
    memset(&stats, 0, sizeof stats);
    int_line = line_sent = false;
    ctl_len = ctl_head = 0;
    sel_unit = -1;
    p1_reg = -1;
    reset();
}
