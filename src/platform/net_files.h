// ネットワークの口 (baremetal/host_net.cpp) から SD カードのファイルを書き換えるときの、置き換えの手順と、
// 断るファイルの判定。
//
// 置き換え: 受け取った内容を <名前>.part に書き終えてから (長さが Content-Length と合ったときだけ)、
//   1. <名前> があれば、<名前>.old を消し、<名前> を <名前>.old に変える
//   2. <名前>.part を <名前> に変える (できなければ <名前>.old を <名前> に戻す)
//   3. <名前>.old を消す
// <名前> がない時間は 1 と 2 の間だけ (名前を変えるだけなので短い。大きなファイルを消す 3 はそのあと)。
// その間に電源が切れたら、<名前>.old が前の内容、<名前>.part が新しい内容を持つ。
// 一時ファイルに .new を使わないのは、pix68k.cfg とフロッピーディスクのイメージを読む側 (ReadWithFallback()) が、
// <名前> がないときに .new を読むため (書きかけを読ませない)。
//
// ファイルの操作は Fs に任せる (platform/file_replace.h と同じ形。exists、remove、rename。それに isDirectory)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform/file_replace.h"

namespace platform {

// path に suffix を付けた名前を out に入れる。入らなければ false
inline bool withSuffix(const char* path, const char* suffix, char* out, size_t size) {
    const size_t a = strlen(path), b = strlen(suffix);
    if (a + b + 1 > size) return false;
    memcpy(out, path, a);
    memcpy(out + a, suffix, b + 1);
    return true;
}

// part を path に置き換える (手順は上)。OK なら path が新しい内容を持つ。ERROR のときも、path か path.old の
// どちらかに前の内容が残る (part は消す)。path がディレクトリなら何も変えずに ERROR (ディレクトリごと
// <名前>.old に改名して、中のファイルを見つからなくしないように)。fs.isDirectory(パス) は、あってディレクトリなら true
template <class Fs> FsResult commitUpload(Fs& fs, const char* path, const char* part, const char* old) {
    if (fs.isDirectory(path)) {
        fs.remove(part);
        return FsResult::ERROR;
    }
    const FsResult p = fs.exists(path);
    if (p == FsResult::ERROR) {
        fs.remove(part);
        return FsResult::ERROR;
    }
    bool moved = false;
    if (p == FsResult::OK) {
        const FsResult r = fs.remove(old);
        if (r == FsResult::ERROR || fs.rename(path, old) != FsResult::OK) {
            fs.remove(part);
            return FsResult::ERROR;
        }
        moved = true;
    }
    if (fs.rename(part, path) != FsResult::OK) {
        if (moved) fs.rename(old, path);
        fs.remove(part);
        return FsResult::ERROR;
    }
    // 前の内容を消す (前の置き換えの途中で止まって残っていたものも)。消せなくても新しい内容は path にある
    fs.remove(old);
    return FsResult::OK;
}

// 大文字と小文字を区別せずに比べる (FAT のファイル名)
inline bool samePathNoCase(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z') y = (char)(y - 'a' + 'A');
        if (x != y) return false;
    }
    return *a == *b;
}

// 比べるためのパスの形を作る。ドライブ名 ("SD:") を除き、区切り (/ と \) を 1 つの / にまとめ、"." を除き、".." で
// 1 つ上に戻り、名前の後ろの "." と空白を除く (FAT はそれを無視する)。upper なら大文字にする (FAT は大文字と小文字を
// 区別しない)。結果は / で始まる (根は "/")。入らなければ false
inline bool normalizePath(const char* in, char* out, size_t size, bool upper) {
    if (size < 2) return false;
    const char* colon = strchr(in, ':');
    const char* p = colon ? colon + 1 : in;
    size_t n = 0;
    while (*p) {
        while (*p == '/' || *p == '\\') p++;
        if (*p == 0) break;
        const char* start = p;
        while (*p && *p != '/' && *p != '\\') p++;
        size_t len = (size_t)(p - start);
        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            while (n > 0 && out[n - 1] != '/') n--; // 名前を消す
            if (n > 0) n--;                         // その前の / を消す
            continue;
        }
        while (len > 0 && (start[len - 1] == '.' || start[len - 1] == ' ')) len--;
        if (len == 0) continue;
        if (n + 1 + len + 1 > size) return false;
        out[n++] = '/';
        for (size_t i = 0; i < len; i++) {
            char c = start[i];
            if (upper && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            out[n++] = c;
        }
    }
    if (n == 0) out[n++] = '/';
    out[n] = 0;
    return true;
}

// fs.longName() の答え
enum class NameLookup {
    FOUND,   // ある (長い名前を返した)
    MISSING, // ない (FatFs の FR_NO_FILE、FR_NO_PATH)
    ERROR,   // 読めなかった (SD カードの読み出しの失敗など。あるかないか分からない)
};

// FAT のパスを、実際のディレクトリエントリの名前 (長い名前) に直してから normalizePath() の大文字の形にする。
// 短い名前 (HUMAN~1.XDF) や大文字と小文字の違いで同じファイルを指していても、同じ文字列になる。
// fs.longName(パス, out, size) は、そのパスのファイルかディレクトリを引き、あれば長い名前を out に入れる (FatFs の
// f_stat() の fname)。ないところから先は、書いた名前のまま。読めなかった (ERROR) か、out に入らなければ false
// (あるかないか分からないので、同じファイルかどうか決められない)
template <class Fs> bool canonicalPath(Fs& fs, const char* in, char* out, size_t size) {
    char norm[300];
    if (!normalizePath(in, norm, sizeof norm, false)) return false;
    char built[300] = "";
    size_t bn = 0;
    bool exists = true;
    const char* p = norm;
    while (*p == '/') {
        p++;
        const char* start = p;
        while (*p && *p != '/') p++;
        const size_t len = (size_t)(p - start);
        if (len == 0) break;
        if (bn + 1 + len + 1 > sizeof built) return false;
        built[bn++] = '/';
        memcpy(built + bn, start, len);
        built[bn + len] = 0;
        char name[260];
        const NameLookup found = exists ? fs.longName(built, name, sizeof name) : NameLookup::MISSING;
        if (found == NameLookup::ERROR) return false;
        if (found == NameLookup::FOUND) {
            const size_t ln = strlen(name);
            if (bn + ln + 1 > sizeof built) return false;
            memcpy(built + bn, name, ln + 1);
            bn += ln;
        } else {
            exists = false;
            bn += len;
        }
    }
    if (bn == 0) built[bn++] = '/', built[bn] = 0;
    return normalizePath(built, out, size, true);
}

// touchesImage() の答え
enum class ImageCheck {
    FREE,    // どれも使っているイメージと重ならない
    IN_USE,  // どれかが重なる (409)
    UNKNOWN, // ファイルシステムを読めず、決められない (書き換えも削除も断る。503)
};

// X68000 が使っているイメージ inUse を書き換えるときに使う名前。書き戻しは、フロッピーディスクを <名前>.new に
// 書いてから置き換え (ハードディスクはそのまま上書き)、ネットワークの口は <名前>.part と <名前>.old を使う
static const char* const kImageSuffixes[] = {"", ".new", ".part", ".old"};

// paths (ドライブの中のパス。書き換える、消す、置き換えに使うファイルをすべて渡す) のどれかが、X68000 が使っている
// イメージ inUse (ドライブ名付きでよい) か、その置き換えに使う名前と同じファイルか。
// 置き換えの名前は、書き戻す側と同じく、inUse に書いたとおりの名前 (短い名前ならそのまま) に接尾辞を付けてから
// 長い名前に直す。念のため、長い名前に直した inUse に接尾辞を付けたものとも比べる
template <class Fs> ImageCheck touchesImage(Fs& fs, const char* const* paths, int count, const char* inUse) {
    char want[4][2][320];
    char img[300];
    if (!canonicalPath(fs, inUse, img, sizeof img)) return ImageCheck::UNKNOWN;
    for (int k = 0; k < 4; k++) {
        char raw[320];
        if (!withSuffix(inUse, kImageSuffixes[k], raw, sizeof raw)) return ImageCheck::UNKNOWN;
        if (!canonicalPath(fs, raw, want[k][0], sizeof want[k][0])) return ImageCheck::UNKNOWN;
        if (!withSuffix(img, kImageSuffixes[k], raw, sizeof raw) ||
            !normalizePath(raw, want[k][1], sizeof want[k][1], true))
            return ImageCheck::UNKNOWN;
    }
    for (int i = 0; i < count; i++) {
        char c[300];
        if (!canonicalPath(fs, paths[i], c, sizeof c)) return ImageCheck::UNKNOWN;
        for (int k = 0; k < 4; k++)
            if (strcmp(c, want[k][0]) == 0 || strcmp(c, want[k][1]) == 0) return ImageCheck::IN_USE;
    }
    return ImageCheck::FREE;
}

// 名前の最後が、置き換えに使う接尾辞 (.new、.part、.old) か。設定メニューでは、こうしたファイルをつながない
// (ネットワークの口が書いている途中の <名前>.part などを X68000 が使わないように)
inline bool isReplaceTempName(const char* name) {
    const size_t n = strlen(name);
    for (int k = 1; k < 4; k++) {
        const size_t sn = strlen(kImageSuffixes[k]);
        if (n >= sn && samePathNoCase(name + n - sn, kImageSuffixes[k])) return true;
    }
    return false;
}

// ファイルシステムを見ずに比べる版 (長い名前と短い名前の違いは見分けられない)。ホストのテスト用
struct NoFsNames {
    NameLookup longName(const char*, char*, size_t) { return NameLookup::MISSING; }
};
inline bool pathTouchesImage(const char* path, const char* inUse) {
    NoFsNames fs;
    const char* const paths[] = {path};
    return touchesImage(fs, paths, 1, inUse) != ImageCheck::FREE;
}

// 画面の取り込み (PPM、P6)。頭を out に書き、長さを返す
inline int ppmHeader(char* out, size_t size, int width, int height) {
    char tmp[48];
    int n = 0;
    auto put = [&](const char* s) {
        while (*s && n + 1 < (int)sizeof tmp) tmp[n++] = *s++;
    };
    auto putNum = [&](int v) {
        char d[12];
        int k = 0;
        do d[k++] = (char)('0' + v % 10), v /= 10;
        while (v > 0 && k < 11);
        while (k > 0 && n + 1 < (int)sizeof tmp) tmp[n++] = d[--k];
    };
    put("P6\n");
    putNum(width);
    put(" ");
    putNum(height);
    put("\n255\n");
    if ((size_t)n + 1 > size) return 0;
    memcpy(out, tmp, (size_t)n);
    out[n] = 0;
    return n;
}

// XRGB8888 の画素を R、G、B のバイトにする
inline void xrgbToRgb(const uint32_t* src, uint8_t* dst, int count) {
    for (int i = 0; i < count; i++) {
        const uint32_t p = src[i];
        dst[i * 3] = (uint8_t)(p >> 16);
        dst[i * 3 + 1] = (uint8_t)(p >> 8);
        dst[i * 3 + 2] = (uint8_t)p;
    }
}

} // namespace platform
