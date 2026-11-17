// ファイルを、書いている途中で止まっても前の内容が残るように書き直す (ベアメタル版の pix68k.cfg とステート)。
//
// 手順: tmp (pix68k.new など) に書き終えてから、path を消して tmp の名前を path に変える
// (FatFs の f_rename は上書きしないので、消してから変える)。読む側は、path がないときだけ tmp を読む。
// 前の置き換えの途中 (path を消したあと、名前を変える前) で止まっていれば、tmp が唯一の写しなので、
// 書く前に名前を path に戻す。戻せたことを確かめられなければ書かない。読む側の手順は readWithFallback()。
//
// ファイルの操作は Fs に任せる (ベアメタル版は FatFs、試験は作り物)。Fs が持つもの:
//   FsResult exists(const char* path)                     ある: OK、ない: NO_FILE、分からない: ERROR
//   FsResult write(const char* path, const void*, unsigned) 作り直して全部を書き、閉じる (書ききれなければ ERROR)
//   FsResult remove(const char* path)                     ないときは NO_FILE
//   FsResult rename(const char* from, const char* to)
//   bool canRename(const char* from, const char* to)      なくてもよい。途中で止まった置き換えを戻す名前の変更の前に聞く
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

namespace platform {

enum class FsResult { OK, NO_FILE, ERROR };

// Fs に bool canRename(const char* from, const char* to) があればそれを呼ぶ。なければ true
template <class Fs>
auto canRecoverRename(Fs& fs, const char* from, const char* to, int) -> decltype(fs.canRename(from, to)) {
    return fs.canRename(from, to);
}
template <class Fs> bool canRecoverRename(Fs&, const char*, const char*, long) { return true; }

template <class Fs>
FsResult replaceFile(Fs& fs, const char* path, const char* tmp, const void* data, unsigned size) {
    const FsResult p = fs.exists(path);
    if (p == FsResult::ERROR) return FsResult::ERROR;
    if (p == FsResult::NO_FILE) {
        const FsResult t = fs.exists(tmp);
        if (t == FsResult::ERROR) return FsResult::ERROR;
        if (t == FsResult::OK) {
            // 前の置き換えの途中で止まった: tmp を path に戻す。戻せなければ、唯一の写しの tmp を書き直さない。
            // Fs に canRename() があれば、名前を変える前に聞く (名前を変えるとディレクトリが伸びることがあるので、
            // 空きが少ないドライブでは断る)。断られたら戻さず、tmp が唯一の写しのまま残る (読む側は tmp を読む)
            if (!canRecoverRename(fs, tmp, path, 0)) return FsResult::ERROR;
            if (fs.rename(tmp, path) != FsResult::OK) return FsResult::ERROR;
            if (fs.exists(path) != FsResult::OK || fs.exists(tmp) != FsResult::NO_FILE) return FsResult::ERROR;
        }
    }
    // ここでは、path が今の内容を持っているか、どちらもない。tmp があっても (path を消す前に止まったもの)
    // path のほうが読まれるので、書き直してよい
    FsResult r = fs.write(tmp, data, size);
    if (r != FsResult::OK) {
        fs.remove(tmp); // path は前のまま
        return FsResult::ERROR;
    }
    r = fs.remove(path);
    if (r != FsResult::OK && r != FsResult::NO_FILE) {
        // path を消せない (読み取り専用など): path は前のまま読まれるので、書いた tmp は消す
        // (残すと、あとで path がなくなったときに古い tmp を読むことになる)
        fs.remove(tmp);
        return FsResult::ERROR;
    }
    // ここで失敗しても、path はなく tmp が新しい内容を持つ (読む側が tmp を読み、次に書くときに path に戻す)
    return fs.rename(tmp, path) == FsResult::OK ? FsResult::OK : FsResult::ERROR;
}

// replaceFile() で書いたファイルを読む。path があれば path を読み (空でも path)、path がないときだけ tmp を読む。
// tmp を読めたら usedTmp を true にする。返すのは、path を読んだ結果か、path がなくて tmp を読んだ結果
// (tmp もなければ path の結果)。結果の型 R は Fs が決める (ベアメタル版は FatFs の FRESULT)。Fs が持つもの:
//   R    read(const char* path)   読む (読んだ中身は Fs が持つ)
//   bool missing(R r)             ファイルがない (FatFs では FR_NO_FILE と FR_NO_PATH)
//   bool ok(R r)                  読めた
template <class Fs>
auto readWithFallback(Fs& fs, const char* path, const char* tmp, bool& usedTmp) -> decltype(fs.read(path)) {
    usedTmp = false;
    auto r = fs.read(path);
    if (fs.missing(r)) {
        auto t = fs.read(tmp);
        if (fs.ok(t)) usedTmp = true;
        if (!fs.missing(t)) r = t;
    }
    return r;
}

} // namespace platform
