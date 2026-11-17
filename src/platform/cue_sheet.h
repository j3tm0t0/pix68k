// CUE シート (CD-ROM のイメージのトラックの並びを書いたテキスト) を読み、CD-ROM 装置のトラックの並び (x68k::CdToc) を作る。
//
// 読む行: FILE "名前" BINARY | MOTOROLA | WAVE、TRACK nn 種類、INDEX nn mm:ss:ff、PREGAP mm:ss:ff、POSTGAP mm:ss:ff、
// FLAGS (DCP、4CH、PRE)。ほかの行 (REM、TITLE、PERFORMER、CATALOG、ISRC など) は読み飛ばす。
// トラックの種類: AUDIO (2352 バイト / セクタ)、MODE1/2048、MODE1/2352、MODE2/2336、MODE2/2352。
// CDG と CDI、MP3 と AIFF のファイルは扱わない (parse() が false を返す)。
//
// 使い方:
//   CueSheet cue;
//   if (!cue.parse(text, size)) ... cue.error()
//   ファイルごとに (cue.fileCount()、cue.fileName())、イメージの中のデータの位置と大きさを決める。
//   WAVE のファイルは wavData() でヘッダを読み、PCM のデータの位置と大きさを使う。
//   ファイルが複数なら、環境がつなげて 1 つのイメージにする (つなげた中での位置を渡す)
//   if (!cue.layout(base, bytes, toc)) ... cue.error()
//
// 位置の決め方:
//   - INDEX の時刻は、そのファイルの先頭からのフレーム (1 フレーム = 1 セクタ、1 秒 = 75 フレーム)
//   - 論理ブロック 0 は最初のファイルの先頭。ファイルが複数なら、前のファイルの終わりに続ける
//   - INDEX 00 から INDEX 01 の前まではプリギャップ (そのトラックに属し、ファイルに入っている)。
//     PREGAP はファイルに入っていないプリギャップ、POSTGAP はファイルに入っていないポストギャップ (どちらも無音。
//     その分だけ後ろの位置がずれる)。最初のトラックの PREGAP は読み飛ばす (論理ブロック 0 の前の 2 秒のこと)
//   - TOC に出すトラックの始まりは INDEX 01
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "x68k/cd_drive.h"

namespace platform {

class CueSheet {
public:
    static constexpr int MAX_FILES = 99;
    static constexpr int MAX_TRACKS = x68k::CdToc::MAX_TRACKS;
    enum FileType : uint8_t { BINARY, MOTOROLA, WAVE };

    // text (size バイト) を読む。text は layout() と fileName() を使い終わるまで残しておくこと
    bool parse(const char* text, size_t size) {
        files_ = tracks_ = 0;
        error_ = nullptr;
        size_t pos = 0;
        if (size >= 3 && (uint8_t)text[0] == 0xef && (uint8_t)text[1] == 0xbb && (uint8_t)text[2] == 0xbf) pos = 3; // BOM
        while (pos < size) {
            size_t end = pos;
            while (end < size && text[end] != '\n' && text[end] != '\r') end++;
            if (!line(text + pos, end - pos)) return false;
            pos = end;
            while (pos < size && (text[pos] == '\n' || text[pos] == '\r')) pos++;
        }
        if (tracks_ == 0) return fail("no TRACK");
        for (int i = 0; i < tracks_; i++)
            if (track_[i].index1 < 0) return fail("a TRACK has no INDEX 01");
        return true;
    }
    const char* error() const { return error_ ? error_ : ""; }
    int fileCount() const { return files_; }
    FileType fileType(int i) const { return file_[i].type; }
    // ファイルの名前を out (cap バイト) に写す。入りきらなければ false
    bool fileName(int i, char* out, size_t cap) const {
        if (i < 0 || i >= files_ || file_[i].len + 1 > cap) return false;
        memcpy(out, file_[i].name, file_[i].len);
        out[file_[i].len] = 0;
        return true;
    }
    int trackCount() const { return tracks_; }

    // base[i] はファイル i のデータの、イメージの中の位置 (バイト)、bytes[i] はそのデータの大きさ。
    // トラックの並びを toc に作る。作れなければ false (error())
    bool layout(const uint64_t* base, const uint64_t* bytes, x68k::CdToc& toc) {
        toc.count = 0;
        uint64_t p = 0; // 今のファイルの先頭の論理ブロック
        int t = 0;
        for (int f = 0; f < files_; f++) {
            const int a = t;
            while (t < tracks_ && track_[t].file == f) t++;
            if (t == a) continue; // トラックのないファイル
            const uint32_t sector = track_[a].sector;
            for (int i = a; i < t; i++)
                if (track_[i].sector != sector) return fail("tracks with different sector sizes in one FILE");
            const uint64_t frames = bytes[f] / sector;
            uint64_t start = p;
            for (int i = a; i < t; i++) {
                const Track& c = track_[i];
                // ファイルの最初のトラックは、ファイルの先頭から (INDEX 01 の前はプリギャップ)
                const uint32_t idx0 = i == a ? 0 : first(c);
                const uint64_t next = i + 1 < t ? first(track_[i + 1]) : frames;
                if (idx0 > (uint32_t)c.index1 || (uint64_t)c.index1 > next || next > frames)
                    return fail("an INDEX is out of order or beyond the end of the FILE");
                x68k::CdTrack& o = toc.tracks[toc.count];
                o = x68k::CdTrack();
                o.number = c.number;
                o.control = c.control;
                o.dataOffset = c.dataOffset;
                o.sector = (uint16_t)sector;
                o.swap = file_[f].type == MOTOROLA && (c.control & 0x04) == 0;
                o.begin = (uint32_t)(start + idx0);
                if (toc.count > 0) start += c.pregap;
                o.fileLba = (uint32_t)(start + idx0);
                o.start = (uint32_t)(start + (uint32_t)c.index1);
                o.fileEnd = (uint32_t)(start + next);
                o.offset = base[f] + (uint64_t)idx0 * sector;
                start += c.postgap;
                toc.count++;
            }
            p = start + frames;
            if (p > 0xffffff) return fail("the image is too long");
        }
        if (toc.count == 0 || p == 0) return fail("no data");
        toc.leadOut = (uint32_t)p;
        return true;
    }

private:
    struct File {
        const char* name = nullptr;
        size_t len = 0;
        FileType type = BINARY;
    };
    struct Track {
        uint8_t number = 0;
        uint8_t file = 0;
        uint8_t control = 0;
        uint8_t dataOffset = 0;
        uint16_t sector = 2352;
        int32_t index0 = -1, index1 = -1; // ファイルの先頭からのフレーム (なければ -1)
        uint32_t pregap = 0, postgap = 0;
    };
    // そのトラックの、ファイルに入っている最初のフレーム
    static uint32_t first(const Track& c) { return (uint32_t)(c.index0 >= 0 ? c.index0 : c.index1); }
    bool fail(const char* e) {
        error_ = e;
        return false;
    }
    static char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c; }
    static bool space(char c) { return c == ' ' || c == '\t'; }
    // 次の語 (引用符で囲んだものは中身) を取り出す。なければ false
    static bool word(const char*& p, const char* end, const char*& w, size_t& n) {
        while (p < end && space(*p)) p++;
        if (p >= end) return false;
        if (*p == '"') {
            w = ++p;
            while (p < end && *p != '"') p++;
            n = (size_t)(p - w);
            if (p < end) p++;
            return true;
        }
        w = p;
        while (p < end && !space(*p)) p++;
        n = (size_t)(p - w);
        return true;
    }
    static bool is(const char* w, size_t n, const char* key) {
        size_t i = 0;
        for (; i < n && key[i]; i++)
            if (upper(w[i]) != key[i]) return false;
        return i == n && key[i] == 0;
    }
    static bool number(const char* w, size_t n, uint32_t& v) {
        if (n == 0 || n > 6) return false;
        v = 0;
        for (size_t i = 0; i < n; i++) {
            if (w[i] < '0' || w[i] > '9') return false;
            v = v * 10 + (uint32_t)(w[i] - '0');
        }
        return true;
    }
    // mm:ss:ff をフレームにする
    static bool msf(const char* w, size_t n, uint32_t& frames) {
        uint32_t v[3];
        int k = 0;
        size_t a = 0;
        for (size_t i = 0; i <= n; i++) {
            if (i < n && w[i] != ':') continue;
            if (k >= 3 || !number(w + a, i - a, v[k])) return false;
            k++;
            a = i + 1;
        }
        if (k != 3 || v[1] >= 60 || v[2] >= 75) return false;
        // 分は 6 桁まで書けるので、64 ビットで計算して、論理ブロックに使える範囲 (24 ビット) かを確かめる
        const uint64_t total = ((uint64_t)v[0] * 60 + v[1]) * 75 + v[2];
        if (total > 0xffffff) return false;
        frames = (uint32_t)total;
        return true;
    }
    bool line(const char* p, size_t len) {
        const char* end = p + len;
        const char *w, *a;
        size_t n, an;
        if (!word(p, end, w, n)) return true; // 空の行
        if (is(w, n, "FILE")) {
            if (files_ >= MAX_FILES) return fail("too many FILEs");
            File& f = file_[files_];
            while (p < end && space(*p)) p++;
            if (p < end && *p == '"') {
                if (!word(p, end, f.name, f.len) || !word(p, end, a, an)) return fail("FILE without a name or a type");
            } else {
                // 引用符のない名前: 行の最後の語が種類、その前までが名前 (名前に空白があってもよい)
                const char* e = end;
                while (e > p && space(e[-1])) e--;
                const char* typeEnd = e;
                while (e > p && !space(e[-1])) e--;
                a = e;
                an = (size_t)(typeEnd - e);
                while (e > p && space(e[-1])) e--;
                f.name = p;
                f.len = (size_t)(e - p);
            }
            if (f.len == 0) return fail("FILE without a name or a type");
            if (is(a, an, "BINARY")) f.type = BINARY;
            else if (is(a, an, "MOTOROLA")) f.type = MOTOROLA;
            else if (is(a, an, "WAVE")) f.type = WAVE;
            else return fail("unsupported FILE type (BINARY, MOTOROLA or WAVE)");
            files_++;
            return true;
        }
        if (is(w, n, "TRACK")) {
            if (files_ == 0) return fail("TRACK before FILE");
            if (tracks_ >= MAX_TRACKS) return fail("too many TRACKs");
            Track& t = track_[tracks_];
            t = Track();
            uint32_t num;
            if (!word(p, end, w, n) || !number(w, n, num) || num < 1 || num > 99) return fail("bad TRACK number");
            if (tracks_ > 0 && num <= track_[tracks_ - 1].number) return fail("TRACK numbers are not ascending");
            t.number = (uint8_t)num;
            t.file = (uint8_t)(files_ - 1);
            if (!word(p, end, w, n)) return fail("TRACK without a type");
            if (is(w, n, "AUDIO")) {
                t.sector = 2352;
            } else {
                t.control = 0x04; // データトラック
                if (is(w, n, "MODE1/2048") || is(w, n, "MODE2/2048")) {
                    t.sector = 2048;
                } else if (is(w, n, "MODE1/2352")) {
                    t.sector = 2352;
                    t.dataOffset = 16; // 同期 12 + ヘッダ 4
                } else if (is(w, n, "MODE2/2336")) {
                    t.sector = 2336;
                    t.dataOffset = 8; // サブヘッダ 8
                } else if (is(w, n, "MODE2/2352")) {
                    t.sector = 2352;
                    t.dataOffset = 24; // 同期 12 + ヘッダ 4 + サブヘッダ 8
                } else {
                    return fail("unsupported TRACK type");
                }
            }
            tracks_++;
            return true;
        }
        if (is(w, n, "INDEX") || is(w, n, "PREGAP") || is(w, n, "POSTGAP") || is(w, n, "FLAGS")) {
            if (tracks_ == 0) return fail("INDEX, PREGAP, POSTGAP or FLAGS before TRACK");
            Track& t = track_[tracks_ - 1];
            if (is(w, n, "FLAGS")) {
                while (word(p, end, a, an)) {
                    if (is(a, an, "PRE")) t.control |= 0x01;
                    else if (is(a, an, "DCP")) t.control |= 0x02;
                    else if (is(a, an, "4CH")) t.control |= 0x08;
                }
                return true;
            }
            uint32_t idx = 0, frames;
            const bool index = is(w, n, "INDEX"), pre = is(w, n, "PREGAP");
            if (index && (!word(p, end, a, an) || !number(a, an, idx))) return fail("bad INDEX number");
            if (!word(p, end, a, an) || !msf(a, an, frames)) return fail("bad time (mm:ss:ff)");
            if (!index) (pre ? t.pregap : t.postgap) = frames;
            else if (idx == 0) t.index0 = (int32_t)frames;
            else if (idx == 1) t.index1 = (int32_t)frames; // INDEX 02 から後ろは使わない
            return true;
        }
        return true; // ほかの行は読み飛ばす
    }

    File file_[MAX_FILES];
    Track track_[MAX_TRACKS];
    int files_ = 0, tracks_ = 0;
    const char* error_ = nullptr;
};

// WAV のファイルの先頭 (n バイト。ふつうは 44 バイトのヘッダだが、ほかのチャンクがあることもあるので多めに読んで渡す) から、
// PCM のデータの位置 (offset) と大きさ (bytes) を探す。fileSize はファイルの大きさ。
// 44.1kHz、16 ビット、ステレオの PCM でなければ false (err に理由)
inline bool wavData(const uint8_t* head, size_t n, uint64_t fileSize, uint32_t& offset, uint64_t& bytes, const char*& err) {
    auto le32 = [&](size_t p) { return (uint32_t)head[p] | (uint32_t)head[p + 1] << 8 | (uint32_t)head[p + 2] << 16 | (uint32_t)head[p + 3] << 24; };
    auto le16 = [&](size_t p) { return (uint32_t)head[p] | (uint32_t)head[p + 1] << 8; };
    err = "not a WAV file";
    if (n < 12 || memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "WAVE", 4) != 0) return false;
    bool fmt = false;
    size_t p = 12;
    while (p + 8 <= n) {
        const uint32_t len = le32(p + 4);
        if (memcmp(head + p, "fmt ", 4) == 0) {
            if (len < 16 || p + 24 > n) return false;
            err = "the WAV file is not 44.1kHz 16-bit stereo PCM";
            if (le16(p + 8) != 1 || le16(p + 10) != 2 || le32(p + 12) != 44100 || le16(p + 22) != 16) return false;
            fmt = true;
        } else if (memcmp(head + p, "data", 4) == 0) {
            if (!fmt) return false;
            offset = (uint32_t)(p + 8);
            bytes = len;
            if (offset > fileSize) return false;
            if (bytes > fileSize - offset) bytes = fileSize - offset; // 長さが合わないファイルは、あるだけ
            return true;
        }
        p += 8 + (size_t)len + (len & 1);
    }
    err = "no data chunk near the start of the WAV file";
    return false;
}

} // namespace platform
