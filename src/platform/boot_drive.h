// ベアメタル版が ROM、pix68k.cfg、ディスクのイメージなどを読み書きするドライブ (boot ドライブ) の選び方。
//
// ドライブは優先する順に NVMe (Pi 5 だけ)、SD カード、USB メモリ (Circle の umsd1〜umsd3、FatFs の USB: / USB2: / USB3:)。
// USB メモリは、USB の装置を探し終えるまで使えず、探すのに時間がかかるので、NVMe と SD カードで決まらないときだけ探す
// (needUsbDrive())。USB メモリが 2 本以上あっても、同じ規則で 1 本を選ぶ (Linux の入った USB メモリとデータ用の USB
// メモリを挿したときに、/pix68k のあるほうを使うため)。
//
// 選び方 (pickBootDrive()):
//   - deploy (tools/deploy_pi.sh。cmdline.txt の pix68k_id が 0 でない) のときは、/pix68k/id.txt がその値のドライブ。
//     なければ選ばない (ほかのディスクにログを書かないため)
//   - そうでなければ、/pix68k のディレクトリがあるドライブのうち、優先する順で最初のもの
//   - どれにも /pix68k がなければ、マウントできたドライブのうち最初のもの (ROM はルートに置いてもよいので)
// cmdline.txt の pix68k_drive=sd / nvme / usb は、そのドライブだけを候補にする (usb はすべての USB メモリ。既定の auto はすべて)。
// ベアメタルでも使えるよう、例外、RTTI、STL は使わない (ヘッダだけ)。
#pragma once

#include "platform/net_files.h"

namespace platform {

// BOOT_USB から BOOT_USB + BOOT_USB_DRIVES - 1 が USB メモリ (umsd1、umsd2、...)
enum BootDriveIndex { BOOT_NVME = 0, BOOT_SD = 1, BOOT_USB = 2, BOOT_USB_DRIVES = 3, BOOT_DRIVES = BOOT_USB + BOOT_USB_DRIVES };

inline bool isUsbBootDrive(int drive) { return drive >= BOOT_USB && drive < BOOT_DRIVES; }

// cmdline.txt の pix68k_drive の値
enum class BootDriveOption { AUTO, SD, NVME, USB, INVALID };

struct BootDriveState {
    bool mounted = false; // FAT のパーティションをマウントできた
    bool hasDir = false;  // /pix68k のディレクトリがある
    unsigned id = 0;      // /pix68k/id.txt の値 (なければ 0)
};

inline bool bootDriveNameIs(const char* s, const char* name) {
    while (*s != 0 && *s == *name) {
        s++;
        name++;
    }
    return *s == 0 && *name == 0;
}

// s は pix68k_drive の値 (書かれていなければ nullptr)
inline BootDriveOption parseBootDriveOption(const char* s) {
    if (s == nullptr || bootDriveNameIs(s, "auto")) return BootDriveOption::AUTO;
    if (bootDriveNameIs(s, "sd")) return BootDriveOption::SD;
    if (bootDriveNameIs(s, "nvme")) return BootDriveOption::NVME;
    if (bootDriveNameIs(s, "usb")) return BootDriveOption::USB;
    return BootDriveOption::INVALID;
}

// そのドライブを候補にするか (INVALID は AUTO と同じに扱う)
inline bool bootDriveAllowed(BootDriveOption opt, int drive) {
    switch (opt) {
    case BootDriveOption::SD: return drive == BOOT_SD;
    case BootDriveOption::NVME: return drive == BOOT_NVME;
    case BootDriveOption::USB: return isUsbBootDrive(drive);
    default: return drive >= 0 && drive < BOOT_DRIVES;
    }
}

// boot ドライブの番号 (BootDriveIndex) を返す。選べなければ -1。d は BOOT_DRIVES 個
inline int pickBootDrive(const BootDriveState* d, unsigned wantId, BootDriveOption opt) {
    int first = -1;
    for (int i = 0; i < BOOT_DRIVES; i++) {
        if (!d[i].mounted || !bootDriveAllowed(opt, i)) continue;
        if (wantId != 0) {
            if (d[i].id == wantId) return i;
            continue;
        }
        if (d[i].hasDir) return i;
        if (first < 0) first = i;
    }
    return first;
}

// NVMe と SD カードを見たあとで、USB メモリを探す必要があるか。USB メモリを候補にしていて、NVMe と SD カードに
// deploy の値が一致するもの (deploy のとき) も、/pix68k があるもの (そうでないとき) もなければ true
inline bool needUsbDrive(const BootDriveState* d, unsigned wantId, BootDriveOption opt) {
    if (!bootDriveAllowed(opt, BOOT_USB)) return false;
    BootDriveState s[BOOT_DRIVES];
    for (int i = 0; i < BOOT_DRIVES; i++) s[i] = isUsbBootDrive(i) ? BootDriveState{} : d[i];
    const int pick = pickBootDrive(s, wantId, opt);
    if (pick < 0) return true;
    return wantId == 0 && !s[pick].hasDir;
}

// USB メモリを待つ間の、1 つの umsdN の見張り (baremetal/kernel.cpp の FinishBootDrive())。
// Circle は抜かれた USB メモリの番号 (umsd2 など) を次の USB メモリに使い回し、装置のアドレスも使い回すことがある。
// 1 回の UpdatePlugAndPlay() で古い装置の削除と新しい装置の登録が済むと、番号もアドレスも同じに見えるので、
// アドレスを比べるだけでは入れ替わりが分からない。そこで、調べた装置には削除の通知 (CDevice::RegisterRemovedHandler())
// を付け、通知で gone を立てる。update() は今の装置 (GetDevice() の結果。なければ nullptr) を受け取り、
// 前の結果を捨てる (DROP) か、マウントして調べる (PROBE) かを返す。PROBE のときは、呼んだ側が新しい装置に削除の通知を付ける
enum class UsbSlotAction { NONE, PROBE, DROP, DROP_AND_PROBE };

struct UsbSlotWatch {
    const void* device = nullptr; // 調べた装置 (なければ nullptr)
    volatile bool gone = false;   // device が削除された (削除の通知で立てる)

    UsbSlotAction update(const void* present) {
        const bool drop = device != nullptr && (gone || present != device);
        if (drop) device = nullptr;
        gone = false;
        const bool probe = present != nullptr && device == nullptr;
        if (probe) device = present;
        if (drop) return probe ? UsbSlotAction::DROP_AND_PROBE : UsbSlotAction::DROP;
        return probe ? UsbSlotAction::PROBE : UsbSlotAction::NONE;
    }
};

// USB メモリを待つのをやめるか。chosen: 規則で決まる USB メモリ (/pix68k がある、deploy なら id.txt が一致する) が
// つながっている。any: USB メモリがつながっている。changed: 最後に装置の構成が変わった時刻、scanned: 探し終えた時刻。
// 規則で決まるものがあればすぐ、なければ構成が settle の間変わらなくなったら、どちらでもなければ wait でやめる
inline bool usbWaitDone(bool chosen, bool any, unsigned long long now, unsigned long long changed,
                        unsigned long long scanned, unsigned long long settle, unsigned long long wait) {
    return chosen || (any && now - changed >= settle) || now - scanned >= wait;
}

// USB メモリを boot ドライブにしたときに書いてよいパスか。USB メモリの FAT のパーティションは Linux の起動ファイルの
// 置き場所 (/boot/firmware) と同じことがあるので、pix68k が書くのは /pix68k の下だけにする。
// upperPath は platform/net_files.h の normalizePath() の大文字の形 ("/PIX68K/HUMAN.HDS"。".." は片づけてある)
inline bool underPix68kDir(const char* upperPath) {
    const char* dir = "/PIX68K/";
    while (*dir != 0 && *upperPath == *dir) {
        upperPath++;
        dir++;
    }
    return *dir == 0 && *upperPath != 0;
}

// pathInDrive はドライブの中のパス ("/pix68k/HUMAN.HDS"。ドライブ名は付けない)。
// 0x20 より小さい文字、0x7F、":" を含むパスは断る。FatFs はパスを 0x20 より小さい文字で終わりにするので
// ("/kernel8.img<TAB>/../pix68k/x" は "/kernel8.img" を開く)、normalizePath() の結果と開くファイルが食い違う。
// ":" は normalizePath() がドライブ名の区切りとして前を捨てる
inline bool usbPathWritable(const char* pathInDrive) {
    for (const char* p = pathInDrive; *p != 0; p++)
        if ((unsigned char)*p < 0x20 || *p == 0x7f || *p == ':') return false;
    char norm[300];
    return normalizePath(pathInDrive, norm, sizeof norm, true) && underPix68kDir(norm);
}

// USB メモリに新しいファイルを書く前に、空きを確かめる。書いたあとも reserve だけ空きが残るなら true
// (Linux がカーネルを更新するときなどに使う分を残す)
inline bool driveHasRoom(unsigned long long freeBytes, unsigned long long writeBytes, unsigned long long reserve) {
    return freeBytes >= reserve && freeBytes - reserve >= writeBytes;
}

// ネットワークの口とステートの書き込みを、boot ドライブが USB メモリのときに断る理由
enum class UsbWriteCheck {
    OK,
    OUTSIDE, // /pix68k の下ではない
    NO_ROOM, // 書いたあとに reserve の空きが残らない (空きを読めなかったときも)
};

// bytes をクラスタの倍数に切り上げて out に入れる (FAT はクラスタの単位でしか割り当てない)。
// clusterBytes が 0 か 1 ならそのまま。unsigned long long で表せなければ false
inline bool clusterRoundUp(unsigned long long bytes, unsigned long long clusterBytes, unsigned long long& out) {
    out = bytes;
    if (clusterBytes <= 1) return true;
    const unsigned long long rem = bytes % clusterBytes;
    if (rem == 0) return true;
    const unsigned long long add = clusterBytes - rem;
    if (bytes > ~0ull - add) return false;
    out = bytes + add;
    return true;
}

// USB メモリに書く操作の種類
enum class UsbWriteKind {
    REMOVE,   // 消すだけ (空きは見ない)
    NEW_FILE, // 新しい名前のファイルを書く (ステート、SRAM、.new、.part、0 バイトのファイルも)
    RENAME,   // 名前を変えるだけ (途中で止まった置き換えを戻すとき)
    NEW_DIR,  // ディレクトリを作る
};

// 新しい名前を作る操作 (新しいファイル、名前の変更、ディレクトリの作成) で、ディレクトリが伸びる分として数える大きさ。
// FAT は、ディレクトリに空きのエントリがなければ、名前を足すときにディレクトリを 1 クラスタずつ伸ばす。
// 名前の変更も、新しい名前のエントリを取ってから古いエントリを消す。長い名前は最大 255 文字で、
// 13 文字ごとの 19 エントリと短い名前の 1 エントリで 20 エントリ = 640 バイトを使う。1 回の保存は、新しいファイルの
// 作成 (.new や .part) と名前の変更 1、2 回なので、多くても 2KB ほど。クラスタが 512 バイトでも数クラスタで済む。
// 伸びるかどうかを細かく数えず、固定の 64KiB (クラスタに切り上げる。クラスタが大きいときは最低 4 クラスタ) を
// いつも足して判定する。表せなければ false
const unsigned long long kUsbDirMargin = 64 * 1024;

inline bool usbDirMargin(unsigned long long clusterBytes, unsigned long long& out) {
    const unsigned long long c = clusterBytes <= 1 ? 1 : clusterBytes;
    if (c > ~0ull / 4) return false;
    if (!clusterRoundUp(kUsbDirMargin, c, out)) return false;
    if (out < 4 * c) out = 4 * c;
    return true;
}

// その操作が使う大きさを out に入れる。表せなければ false。
//   NEW_FILE: 中身 (クラスタに切り上げる) とディレクトリの余裕 (usbDirMargin())。0 バイトでも余裕の分を数える
//   RENAME:   ディレクトリの余裕 (bytes は見ない)
//   NEW_DIR:  新しいディレクトリの 1 クラスタとディレクトリの余裕 (bytes は見ない)
inline bool usbWriteNeed(UsbWriteKind kind, unsigned long long bytes, unsigned long long clusterBytes,
                         unsigned long long& out) {
    const unsigned long long c = clusterBytes <= 1 ? 1 : clusterBytes;
    out = 0;
    if (kind == UsbWriteKind::REMOVE) return true;
    unsigned long long margin = 0, body = 0;
    if (!usbDirMargin(c, margin)) return false;
    if (kind == UsbWriteKind::NEW_DIR) body = c;
    else if (kind == UsbWriteKind::NEW_FILE && !clusterRoundUp(bytes, c, body)) return false;
    if (body > ~0ull - margin) return false;
    out = body + margin;
    return true;
}

// freeKnown は空きを読めたか。clusterBytes はクラスタの大きさ。使う大きさは usbWriteNeed() で数える。
// pending は、ネットワークの口から受け取っている途中のファイルが、これから使う大きさ (UsbWriteBudget)。
// 受け取りは主ループに譲りながら進むので、その間にほかの保存 (ステートなど) が空きを使うと、両方が書き終わったあとの
// 空きが reserve を切る。残りを先に取ってあるものとして、空きから引いて判定する。
// boot ドライブが USB メモリでなければ (NVMe、SD カード)、前と同じく何も断らない
inline UsbWriteCheck checkUsbWrite(bool bootOnUsb, const char* pathInDrive, UsbWriteKind kind, unsigned long long bytes,
                                   bool freeKnown, unsigned long long freeBytes, unsigned long long reserve,
                                   unsigned long long pending = 0, unsigned long long clusterBytes = 1) {
    if (!bootOnUsb) return UsbWriteCheck::OK;
    if (!usbPathWritable(pathInDrive)) return UsbWriteCheck::OUTSIDE;
    if (kind == UsbWriteKind::REMOVE) return UsbWriteCheck::OK;
    unsigned long long need = 0;
    if (!usbWriteNeed(kind, bytes, clusterBytes, need)) return UsbWriteCheck::NO_ROOM;
    if (!freeKnown || freeBytes < pending || !driveHasRoom(freeBytes - pending, need, reserve))
        return UsbWriteCheck::NO_ROOM;
    return UsbWriteCheck::OK;
}

// ネットワークの口から受け取っている途中のファイルが、これから使う大きさの合計 (複数の受け取りの合計)。
// 受け取り始めるときに begin()、書くたびに (と、やめたときに残りを) end() で減らす。大きさは UploadPlan が数える
struct UsbWriteBudget {
    unsigned long long pending = 0;
    void begin(unsigned long long bytes) { pending += bytes; }
    void end(unsigned long long bytes) { pending -= bytes < pending ? bytes : pending; }
};

// 1 つの受け取りが、これから使う大きさを数える。始めは usbWriteNeed(NEW_FILE) の値 (中身をクラスタに切り上げたものと、
// ディレクトリの余裕。UsbWriteBudget::begin() に渡すのと同じ値。表せない大きさなら 0)。
// wrote() は、書いたことで減った分を返し、finish() は残りを返して 0 にする (どちらも UsbWriteBudget::end() に渡す)。
// FAT は、クラスタの最初の 1 バイトを書いたときにそのクラスタを割り当てるので、書いた大きさもクラスタに切り上げて数える。
// ディレクトリの余裕は、名前を変え終わるまで (finish() まで) 持つ。
// 返した値の合計は、いつも始めの値と同じになる
struct UploadPlan {
    unsigned long long total, cluster, written = 0, body = 0, margin = 0, left = 0;
    UploadPlan(unsigned long long totalBytes, unsigned long long clusterBytes)
        : total(totalBytes), cluster(clusterBytes <= 1 ? 1 : clusterBytes) {
        if (usbWriteNeed(UsbWriteKind::NEW_FILE, total, cluster, left) && usbDirMargin(cluster, margin)) {
            body = left - margin;
        } else {
            total = 0;
            left = 0;
        }
    }
    unsigned long long wrote(unsigned long long n) {
        if (left == 0) return 0;
        written += n < total - written ? n : total - written;
        unsigned long long used = 0;
        clusterRoundUp(written, cluster, used); // written は total 以下なので、表せる
        const unsigned long long now = body - used + margin;
        const unsigned long long freed = left - now;
        left = now;
        return freed;
    }
    unsigned long long finish() {
        const unsigned long long freed = left;
        left = 0;
        return freed;
    }
};

// ネットワークの口が、消す前と、ファイルを受け取り始める前に呼ぶ判定。bytes が負なら消すだけ (空きは見ず、何も
// 取らない)。0 以上なら bytes バイトのファイルを受け取る (0 バイトでもディレクトリの余裕を数える)。OK なら、使う大きさ
// (UploadPlan の始めの値) を budget に足す。呼んだ側は、同じ引数で作った UploadPlan の wrote() と finish() の値を
// budget.end() に返す。boot ドライブが USB メモリでなければ、何も断らず、何も取らない
inline UsbWriteCheck beginNetWrite(UsbWriteBudget& budget, bool bootOnUsb, const char* pathInDrive, long long bytes,
                                   bool freeKnown, unsigned long long freeBytes, unsigned long long reserve,
                                   unsigned long long clusterBytes) {
    const UsbWriteKind kind = bytes < 0 ? UsbWriteKind::REMOVE : UsbWriteKind::NEW_FILE;
    const unsigned long long n = bytes < 0 ? 0 : (unsigned long long)bytes;
    const UsbWriteCheck r =
        checkUsbWrite(bootOnUsb, pathInDrive, kind, n, freeKnown, freeBytes, reserve, budget.pending, clusterBytes);
    if (r == UsbWriteCheck::OK && bootOnUsb && bytes >= 0) budget.begin(UploadPlan(n, clusterBytes).left);
    return r;
}

// コア 0 の主ループなどから、ディスクの書き戻しと大きなイメージの読み込み (platform/disk_sync.h の DiskSync) を進める。
// io が false なら読み書きせず、書き戻すかどうかを決めるだけにする。メニューを開いている間 (menuActive) は、
// X68000 を止めていて (menuPaused) io が true のときだけ、書いていない分を書く。読んだチャンクの数を返す
template <class Sync>
unsigned serviceDiskSync(Sync& sync, unsigned long long nowMs, bool io, bool menuActive, bool menuPaused) {
    const unsigned loads = io ? sync.serviceLoads(4) : 0;
    if (!menuActive) sync.service(nowMs, io);
    else if (io && menuPaused) sync.servicePaused(nowMs);
    return loads;
}

// コア 0 が、USB の転送の中の待ち時間 (CTimer::MsDelay()) にディスクのイメージを読み書きしてよいか。
// boot ドライブが USB メモリなら、ほかの USB の転送の途中に USB メモリの転送を始めることになるので、しない
inline bool diskIoAllowed(bool bootOnUsb, bool insideUsbTransfer) { return !(bootOnUsb && insideUsbTransfer); }

} // namespace platform
