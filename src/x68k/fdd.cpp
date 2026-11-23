#include "x68k/fdd.h"

#include <string.h>

namespace x68k {

FloppyImage::~FloppyImage() { unload(); }

void FloppyImage::unload() {
    delete[] sectors_;
    sectors_ = nullptr;
    capacity_ = used_ = 0;
    format_ = NONE;
    data_ = nullptr;
    size_ = 0;
    dirty_.clearAll();
    __atomic_store_n(&bootChanged_, 0u, __ATOMIC_RELAXED);
    for (int t = 0; t < MAX_TRACKS; t++) {
        trackFirst_[t] = 0;
        trackCount_[t] = 0;
    }
}

bool FloppyImage::allocate(int sectors) {
    sectors_ = new Sector[sectors];
    if (!sectors_) return false;
    capacity_ = sectors;
    used_ = 0;
    return true;
}

void FloppyImage::addTrack(int track, int first, int count) {
    trackFirst_[track] = (int16_t)first;
    trackCount_[track] = (uint8_t)count;
}

static bool hasExtension(const char* name, const char* ext) {
    if (!name) return false;
    size_t n = strlen(name), e = strlen(ext);
    if (n < e) return false;
    for (size_t i = 0; i < e; i++) {
        char a = name[n - e + i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

bool FloppyImage::load(uint8_t* data, size_t size, bool writeProtect, const char* name) {
    unload();
    data_ = data;
    size_ = size;
    writeProtect_ = writeProtect;
    bool ok = false;
    if (hasExtension(name, ".d88") || hasExtension(name, ".d8u") || hasExtension(name, ".88d")) {
        ok = loadD88();
    } else if (hasExtension(name, ".dim")) {
        ok = loadDim();
    } else if (size == 1261568) {
        ok = loadXdf();
    } else {
        // 拡張子が分からないときは D88 (ヘッダの大きさが一致する)、DIM の順に試す
        ok = loadD88() || loadDim();
    }
    if (!ok || !dirty_.init(size)) {
        unload();
        return false;
    }
    return true;
}


// XDF: 2HD (77 シリンダ x 2 ヘッド x 8 セクタ x 1024 バイト) をそのまま並べたもの
bool FloppyImage::loadXdf() {
    if (size_ != 1261568) return false;
    if (!allocate(154 * 8)) return false;
    for (int t = 0; t < 154; t++) {
        addTrack(t, used_, 8);
        for (int r = 1; r <= 8; r++) {
            Sector& s = sectors_[used_++];
            s.c = (uint8_t)(t >> 1);
            s.h = (uint8_t)(t & 1);
            s.r = (uint8_t)r;
            s.n = 3;
            s.status = 0;
            s.deleted = false;
            s.offset = (uint32_t)((t * 8 + r - 1) * 1024);
            s.size = 1024;
        }
    }
    format_ = XDF;
    return true;
}

// DIM: 256 バイトのヘッダ (先頭が種類、続く 170 バイトがトラックの有無) のあと、
// 「有」のトラックのデータだけが並ぶ (0xFF の overtrack に関係なくフラグに従う。DIM の仕様書は見ておらず、試験は自分で作ったイメージだけ)。
// 「無」のトラックはセクタのないトラックとして扱い、読み書きは ST1 の MA (アドレスマークがない) で失敗する
bool FloppyImage::loadDim() {
    if (size_ < 256) return false;
    int type = data_[0];
    int spt, n;
    switch (type) {
    case 0: spt = 8; n = 3; break;  // 2HD
    case 1: spt = 9; n = 3; break;  // 2HS
    case 2: spt = 15; n = 2; break; // 2HC
    case 3: spt = 9; n = 3; break;  // 2HDE
    case 9: spt = 18; n = 2; break; // 2HQ
    default: return false;
    }
    const size_t len = (size_t)spt * (size_t)(128 << n);
    const uint8_t* flags = data_ + 1;
    int tracks = 0;
    for (int t = 0; t < 170 && t < MAX_TRACKS; t++)
        if (flags[t]) tracks++;
    if (tracks == 0 || !allocate(tracks * spt)) return false;
    size_t pos = 256;
    for (int t = 0; t < 170 && t < MAX_TRACKS; t++) {
        if (!flags[t]) continue;
        if (size_ - pos < len) break; // 途中で切れたイメージ: 残りのトラックはないものとする
        int c = t >> 1, h = t & 1;
        addTrack(t, used_, spt);
        for (int r = 1; r <= spt; r++) {
            Sector& s = sectors_[used_++];
            s.c = (uint8_t)c;
            s.h = (uint8_t)h;
            s.r = (uint8_t)r;
            // 2HS は先頭のセクタ以外の R が 10 から、2HDE は先頭のセクタ以外の H に 0x80 が立つ
            if (type == 1 && (c || h || r != 1)) s.r = (uint8_t)(r + 9);
            if (type == 3 && (c || h || r != 1)) s.h = (uint8_t)(h | 0x80);
            s.n = (uint8_t)n;
            s.status = 0;
            s.deleted = false;
            s.offset = (uint32_t)(pos + (size_t)(r - 1) * (size_t)(128 << n));
            s.size = (uint32_t)(128 << n);
        }
        pos += len;
    }
    format_ = DIM;
    return used_ > 0;
}

static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// D88: 0x2B0 バイトのヘッダ (0x1A 書き込み禁止、0x1C 全体の大きさ、0x20 から 164 トラック分の位置)。
// 古いツールが作った 0x2A0 バイトのヘッダ (トラックの表が 160 個) もある。先頭のトラックの位置で見分ける。
// 各セクタは 16 バイトの見出し (C H R N、トラックのセクタ数、密度、削除、状態、データの大きさ) とデータ
static constexpr uint32_t D88_SECTOR_HEADER = 16;

// トラックのセクタの数。見出しかデータがイメージ (limit まで) からはみ出すトラックや、
// セクタのないトラックは 0 (なかったことにする)
static int scanD88Track(const uint8_t* data, uint32_t off, uint32_t limit) {
    if (off >= limit || limit - off < D88_SECTOR_HEADER) return 0;
    int nsec = le16(data + off + 4);
    if (nsec <= 0 || nsec > 64) return 0;
    uint32_t p = off;
    for (int i = 0; i < nsec; i++) {
        if (p >= limit || limit - p < D88_SECTOR_HEADER) return 0;
        uint32_t size = le16(data + p + 0x0e);
        if (limit - p - D88_SECTOR_HEADER < size) return 0;
        p += D88_SECTOR_HEADER + size;
    }
    return nsec;
}

bool FloppyImage::loadD88() {
    if (size_ < 0x2a0) return false;
    uint32_t total = le32(data_ + 0x1c);
    // 0x1C の大きさより後ろに余分なデータがあってもよい (その部分は使わない)
    if (total > size_) return false;
    // 0 でない最初のトラックの位置で見分ける (トラック 0 がないイメージもある)
    uint32_t header = 0x2b0;
    for (int t = 0; t < 160; t++) {
        uint32_t off = le32(data_ + 0x20 + t * 4);
        if (off == 0) continue;
        if (off == 0x2a0) header = 0x2a0;
        break;
    }
    if (total < header) return false;
    int tables = (int)((header - 0x20) / 4);
    int count = 0;
    for (int t = 0; t < tables && t < MAX_TRACKS; t++) {
        uint32_t off = le32(data_ + 0x20 + t * 4);
        if (off < header) continue; // 0 (トラックなし) や、ヘッダの中を指すトラックは飛ばす
        count += scanD88Track(data_, off, total);
    }
    if (count == 0 || !allocate(count)) return false;
    if (data_[0x1a] & 0x10) writeProtect_ = true;
    for (int t = 0; t < tables && t < MAX_TRACKS; t++) {
        uint32_t off = le32(data_ + 0x20 + t * 4);
        if (off < header) continue;
        int nsec = scanD88Track(data_, off, total);
        if (nsec == 0) continue;
        addTrack(t, used_, nsec);
        uint32_t p = off;
        for (int i = 0; i < nsec; i++) {
            Sector& s = sectors_[used_++];
            s.c = data_[p];
            s.h = data_[p + 1];
            s.r = data_[p + 2];
            s.n = data_[p + 3];
            s.deleted = data_[p + 7] != 0;
            s.status = data_[p + 8];
            s.size = le16(data_ + p + 0x0e);
            s.offset = p + D88_SECTOR_HEADER;
            p += D88_SECTOR_HEADER + s.size;
        }
    }
    format_ = D88;
    return true;
}

void FloppyImage::markDirty(const Sector* s) {
    if (!s) return;
    // D88 はデータの前にセクタのヘッダ (デリーテッドデータのフラグと状態) がある
    const uint32_t head = format_ == D88 ? D88_SECTOR_HEADER : 0;
    dirty_.markRange(s->offset - head, (uint64_t)s->size + head);
}

// 書き込みでデータのマークと状態を新しくする。XDF / DIM はデリーテッドデータを表せないので、表の上だけ変える
void FloppyImage::setMark(const Sector* sector, bool deleted) {
    if (!sector || sector < sectors_ || sector >= sectors_ + used_) return;
    Sector& s = sectors_[sector - sectors_];
    s.deleted = deleted;
    s.status = 0;
    if (format_ == D88) {
        uint8_t* h = data_ + s.offset - D88_SECTOR_HEADER;
        h[7] = deleted ? 0x10 : 0x00;
        h[8] = 0x00;
    }
    markDirty(&s);
}

void FloppyImage::noteWrite(const Sector* s, const uint8_t* bytes, uint32_t len, uint8_t fill) {
    // 先頭のトラックの R = 1 のセクタだけを見る。1 セクタ 512 バイトの FAT のディスクでは R = 2 が FAT の先頭のことがあり、
    // R = 2 まで見るとファイルを書くだけで知らせてしまう
    const int count = sectorCount(0);
    if (count == 0 || s < sectors_ + trackFirst_[0] || s >= sectors_ + trackFirst_[0] + count || s->r != 1) return;
    if (len > s->size) len = s->size;
    if (len > 1024) len = 1024;
    const uint8_t* now = data(s);
    for (uint32_t i = 0; i < len; i++) {
        if (now[i] == (bytes ? bytes[i] : fill)) continue;
        __atomic_store_n(&bootChanged_, 1u, __ATOMIC_RELEASE);
        return;
    }
}

int FloppyImage::sectorCount(int track) const {
    if (format_ == NONE || track < 0 || track >= MAX_TRACKS) return 0;
    return trackCount_[track];
}

const FloppyImage::Sector* FloppyImage::sector(int track, int i) const {
    if (i < 0 || i >= sectorCount(track)) return nullptr;
    return &sectors_[trackFirst_[track] + i];
}

const FloppyImage::Sector* FloppyImage::find(int track, uint8_t c, uint8_t h, uint8_t r, uint8_t n) const {
    int cnt = sectorCount(track);
    for (int i = 0; i < cnt; i++) {
        const Sector& s = sectors_[trackFirst_[track] + i];
        if (s.c == c && s.h == h && s.r == r && s.n == n) return &s;
    }
    return nullptr;
}

} // namespace x68k
