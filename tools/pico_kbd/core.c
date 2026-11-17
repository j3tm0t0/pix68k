// pico_kbd の、ハードウェアに依存しない部分
#include "core.h"
#include <string.h>

static Step s_queue[KEYQ_SIZE];
static unsigned s_head, s_tail; // s_head から取り出し、s_tail に入れる
static uint8_t s_heldKeys[6];
static uint8_t s_heldMods;

void keyq_init(void) {
    s_head = s_tail = 0;
    memset(s_heldKeys, 0, sizeof s_heldKeys);
    s_heldMods = 0;
}

unsigned keyq_free(void) { return KEYQ_SIZE - 1 - ((s_tail - s_head) % KEYQ_SIZE); }

// 空きを確かめてから呼ぶ
static void push(uint8_t mods, const uint8_t* keys, unsigned nkeys, uint32_t wait_us) {
    Step* s = &s_queue[s_tail];
    memset(s, 0, sizeof *s);
    s->report[0] = mods;
    for (unsigned i = 0; i < nkeys && i < 6; i++) s->report[2 + i] = keys[i];
    s->wait_us = wait_us;
    s_tail = (s_tail + 1) % KEYQ_SIZE;
}

static unsigned held_list(const uint8_t* held, uint8_t* keys) {
    unsigned n = 0;
    for (unsigned i = 0; i < 6; i++)
        if (held[i]) keys[n++] = held[i];
    return n;
}

bool keyq_tap(uint8_t usage, uint8_t mods, unsigned ms) {
    if (keyq_free() < 2 + 1) return false;
    uint8_t keys[6];
    unsigned n = held_list(s_heldKeys, keys);
    unsigned n0 = n;
    if (usage != 0) {
        bool have = false;
        for (unsigned i = 0; i < n; i++) have |= keys[i] == usage;
        if (!have) {
            if (n >= 6) return false; // 押したままのキーが 6 個あると足せない
            keys[n++] = usage;
        }
    }
    push(s_heldMods | mods, keys, n, ms * 1000u);
    push(s_heldMods, keys, n0, 40 * 1000u);
    return true;
}

bool keyq_down(uint8_t usage, uint8_t mods) {
    if (keyq_free() < 1 + 1) return false;
    uint8_t held[6];
    memcpy(held, s_heldKeys, sizeof held);
    if (usage != 0) {
        bool have = false;
        for (unsigned i = 0; i < 6; i++) have |= held[i] == usage;
        for (unsigned i = 0; i < 6 && !have; i++)
            if (held[i] == 0) { held[i] = usage; have = true; }
        if (!have) return false; // 押したままのキーが 6 個あると足せない
    }
    memcpy(s_heldKeys, held, sizeof held);
    s_heldMods |= mods;
    uint8_t keys[6];
    unsigned n = held_list(s_heldKeys, keys);
    push(s_heldMods, keys, n, 20 * 1000u);
    return true;
}

void keyq_up(void) {
    memset(s_heldKeys, 0, sizeof s_heldKeys);
    s_heldMods = 0;
    // 空きが 0 になるのは U で最後の 1 枠を使ったときだけ。そのあと何も入っていないので、最後の項目はすべて離したレポート
    if (keyq_free() == 0) return;
    push(0, NULL, 0, 20 * 1000u);
}

const Step* keyq_peek(void) { return s_head == s_tail ? NULL : &s_queue[s_head]; }

void keyq_pop(void) {
    if (s_head != s_tail) s_head = (s_head + 1) % KEYQ_SIZE;
}

bool jis_key(char c, uint8_t* usage, bool* shift) {
    static const char kShiftDigits[] = "!\"#$%&'()";
    static const struct {
        char c;
        uint8_t usage;
        bool shift;
    } kOthers[] = {
        {' ', 0x2C, false}, {'\n', 0x28, false}, {'\t', 0x2B, false}, {'\x1b', 0x29, false}, {'\b', 0x2A, false},
        {'-', 0x2D, false}, {'=', 0x2D, true},   {'^', 0x2E, false},  {'~', 0x2E, true},    {'@', 0x2F, false},
        {'`', 0x2F, true},  {'[', 0x30, false},  {'{', 0x30, true},   {']', 0x32, false},   {'}', 0x32, true},
        {';', 0x33, false}, {'+', 0x33, true},   {':', 0x34, false},  {'*', 0x34, true},    {',', 0x36, false},
        {'<', 0x36, true},  {'.', 0x37, false},  {'>', 0x37, true},   {'/', 0x38, false},   {'?', 0x38, true},
        {'\\', 0x89, false}, {'|', 0x89, true},  {'_', 0x87, true},
    };
    if (c >= 'a' && c <= 'z') { *usage = 0x04 + (c - 'a'); *shift = false; return true; }
    if (c >= 'A' && c <= 'Z') { *usage = 0x04 + (c - 'A'); *shift = true; return true; }
    if (c >= '1' && c <= '9') { *usage = 0x1E + (c - '1'); *shift = false; return true; }
    if (c == '0') { *usage = 0x27; *shift = false; return true; }
    const char* p = strchr(kShiftDigits, c);
    if (c != 0 && p != NULL) { *usage = 0x1E + (uint8_t)(p - kShiftDigits); *shift = true; return true; }
    for (unsigned i = 0; i < sizeof kOthers / sizeof kOthers[0]; i++)
        if (kOthers[i].c == c) { *usage = kOthers[i].usage; *shift = kOthers[i].shift; return true; }
    return false;
}

bool pwr_parse(const char* args, unsigned* ms) {
    while (*args == ' ') args++;
    unsigned v = 0;
    if (*args == 0) {
        v = PWR_SHORT_MS;
    } else if (*args == 'S' || *args == 'L') {
        v = *args == 'S' ? PWR_SHORT_MS : PWR_LONG_MS;
        args++;
    } else {
        unsigned n = 0;
        for (; *args >= '0' && *args <= '9'; args++, n++) {
            v = v * 10 + (unsigned)(*args - '0');
            if (v > PWR_MAX_MS) return false; // ここで止めるので、桁があふれることはない
        }
        if (n == 0 || v == 0) return false;
    }
    while (*args == ' ') args++;
    if (*args != 0) return false;
    *ms = v;
    return true;
}

bool token_eq(const char* line, const char* token) {
    size_t n = strlen(line), m = strlen(token);
    unsigned diff = (unsigned)(n ^ m);
    for (size_t i = 0; i < m; i++) diff |= (unsigned)((unsigned char)(i < n ? line[i] : 0) ^ (unsigned char)token[i]);
    return m > 0 && diff == 0;
}

bool logline_feed(LogLine* l, char c) {
    if (l->ready) {
        l->len = 0;
        l->ready = false;
    }
    if (c == '\r') return false;
    if (c != '\n') {
        l->buf[l->len++] = c;
        if (l->len < LOGLINE_MAX) return false;
    }
    l->buf[l->len] = 0;
    l->ready = true;
    return true;
}

bool logline_flush(LogLine* l) {
    if (l->ready || l->len == 0) return false;
    l->buf[l->len] = 0;
    l->ready = true;
    return true;
}

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int base64_decode(const char* s, uint8_t* out, unsigned cap) {
    size_t n = strlen(s);
    if (n % 4 != 0) return -1;
    unsigned len = 0;
    for (size_t i = 0; i < n; i += 4) {
        // = は最後の 4 文字の、3 文字目と 4 文字目にだけ書ける
        unsigned pad = 0;
        if (s[i + 3] == '=') pad = s[i + 2] == '=' ? 2 : 1;
        if (pad > 0 && i + 4 != n) return -1;
        uint32_t v = 0;
        for (unsigned k = 0; k < 4 - pad; k++) {
            int d = b64_value(s[i + k]);
            if (d < 0) return -1;
            v |= (uint32_t)d << (18 - 6 * k);
        }
        unsigned bytes = 3 - pad;
        if (len + bytes > cap) return -1;
        for (unsigned k = 0; k < bytes; k++) out[len++] = (uint8_t)(v >> (16 - 8 * k));
    }
    return (int)len;
}

uint32_t crc32_update(uint32_t crc, const uint8_t* data, unsigned len) {
    crc = ~crc;
    for (unsigned i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

LineResult line_feed(LineBuf* lb, char c) {
    if (c == '\r') return LINE_NONE;
    if (c == '\n') {
        bool over = lb->overflow;
        lb->buf[lb->len] = 0;
        lb->len = 0;
        lb->overflow = false;
        return over ? LINE_TOO_LONG : LINE_READY;
    }
    if (lb->overflow) return LINE_NONE;
    if (lb->len >= LINE_MAX) {
        lb->overflow = true;
        return LINE_NONE;
    }
    lb->buf[lb->len++] = c;
    return LINE_NONE;
}

int picobin_tbyb(const uint8_t* p, unsigned n) {
    // pico-sdk の boot/picobin.h と crt0 の embedded_start_block.inc.S の並び:
    //   0xffffded3 (ブロックの始まり)、0x42 (IMAGE_TYPE の項目)、0x01 (1 ワード)、16 ビットのフラグ (0x8000 が TBYB)
    for (unsigned i = 0; i + 8 <= n; i += 4) {
        if (p[i] != 0xd3 || p[i + 1] != 0xde || p[i + 2] != 0xff || p[i + 3] != 0xff) continue;
        if (p[i + 4] != 0x42 || p[i + 5] != 0x01) continue;
        return (p[i + 7] & 0x80) ? 1 : 0;
    }
    return -1;
}
