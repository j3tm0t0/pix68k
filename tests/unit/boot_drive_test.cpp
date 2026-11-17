// src/platform/boot_drive.h のテスト: ベアメタル版が読み書きするドライブ (NVMe、SD カード、USB メモリ) の選び方
#include "platform/boot_drive.h"
#include "test_check.h"

#include <string.h>

namespace {

using namespace platform;

struct Drives {
    BootDriveState d[BOOT_DRIVES];
    Drives& set(int i, bool hasDir, unsigned id = 0) {
        d[i].mounted = true;
        d[i].hasDir = hasDir;
        d[i].id = id;
        return *this;
    }
};

void testOption() {
    CHECK_EQ((int)parseBootDriveOption(nullptr), (int)BootDriveOption::AUTO);
    CHECK_EQ((int)parseBootDriveOption("auto"), (int)BootDriveOption::AUTO);
    CHECK_EQ((int)parseBootDriveOption("sd"), (int)BootDriveOption::SD);
    CHECK_EQ((int)parseBootDriveOption("nvme"), (int)BootDriveOption::NVME);
    CHECK_EQ((int)parseBootDriveOption("usb"), (int)BootDriveOption::USB);
    CHECK_EQ((int)parseBootDriveOption("usb1"), (int)BootDriveOption::INVALID);
    CHECK_EQ((int)parseBootDriveOption("us"), (int)BootDriveOption::INVALID);
    CHECK_EQ((int)parseBootDriveOption(""), (int)BootDriveOption::INVALID);
    // INVALID は AUTO と同じく、すべてを候補にする
    for (int i = 0; i < BOOT_DRIVES; i++) CHECK_EQ(bootDriveAllowed(BootDriveOption::INVALID, i), true);
    CHECK_EQ(bootDriveAllowed(BootDriveOption::USB, BOOT_SD), false);
    CHECK_EQ(bootDriveAllowed(BootDriveOption::SD, BOOT_SD), true);
}

// SD カードに /pix68k があれば、USB メモリは探さず、SD カードを使う (Pi 3 / Pi 5 の今の動き)
void testSdFirst() {
    Drives a;
    a.set(BOOT_SD, true);
    CHECK_EQ(needUsbDrive(a.d, 0, BootDriveOption::AUTO), false);
    CHECK_EQ(pickBootDrive(a.d, 0, BootDriveOption::AUTO), BOOT_SD);
    // USB メモリにも /pix68k があっても、SD カードが先
    a.set(BOOT_USB, true);
    CHECK_EQ(pickBootDrive(a.d, 0, BootDriveOption::AUTO), BOOT_SD);
    // NVMe と SD カードの両方に /pix68k があれば NVMe
    a.set(BOOT_NVME, true);
    CHECK_EQ(pickBootDrive(a.d, 0, BootDriveOption::AUTO), BOOT_NVME);
    // NVMe に /pix68k がなく、SD カードにあれば SD カード
    Drives b;
    b.set(BOOT_NVME, false).set(BOOT_SD, true);
    CHECK_EQ(needUsbDrive(b.d, 0, BootDriveOption::AUTO), false);
    CHECK_EQ(pickBootDrive(b.d, 0, BootDriveOption::AUTO), BOOT_SD);
}

// SD カードがないか、/pix68k がなければ USB メモリを探す
void testUsbFallback() {
    Drives none;
    CHECK_EQ(needUsbDrive(none.d, 0, BootDriveOption::AUTO), true);
    CHECK_EQ(pickBootDrive(none.d, 0, BootDriveOption::AUTO), -1);
    none.set(BOOT_USB, true);
    CHECK_EQ(pickBootDrive(none.d, 0, BootDriveOption::AUTO), BOOT_USB);

    Drives sd;
    sd.set(BOOT_SD, false);
    CHECK_EQ(needUsbDrive(sd.d, 0, BootDriveOption::AUTO), true);
    CHECK_EQ(pickBootDrive(sd.d, 0, BootDriveOption::AUTO), BOOT_SD); // USB メモリがなければ SD カードのまま
    sd.set(BOOT_USB, true);
    CHECK_EQ(pickBootDrive(sd.d, 0, BootDriveOption::AUTO), BOOT_USB);
    // どちらにも /pix68k がなければ、先に見た SD カード
    Drives both;
    both.set(BOOT_SD, false).set(BOOT_USB, false);
    CHECK_EQ(pickBootDrive(both.d, 0, BootDriveOption::AUTO), BOOT_SD);
    // USB メモリだけがあり、/pix68k がなくても USB メモリ (ROM はルートでもよい)
    Drives usb;
    usb.set(BOOT_USB, false);
    CHECK_EQ(pickBootDrive(usb.d, 0, BootDriveOption::AUTO), BOOT_USB);
    // needUsbDrive() は、渡された USB メモリの状態を見ない
    CHECK_EQ(needUsbDrive(usb.d, 0, BootDriveOption::AUTO), true);
}

// pix68k_drive= で 1 つに決める
void testExplicit() {
    Drives a;
    a.set(BOOT_SD, true).set(BOOT_USB, true).set(BOOT_NVME, true);
    CHECK_EQ(pickBootDrive(a.d, 0, BootDriveOption::USB), BOOT_USB);
    CHECK_EQ(pickBootDrive(a.d, 0, BootDriveOption::SD), BOOT_SD);
    CHECK_EQ(pickBootDrive(a.d, 0, BootDriveOption::NVME), BOOT_NVME);
    CHECK_EQ(needUsbDrive(a.d, 0, BootDriveOption::USB), true);
    CHECK_EQ(needUsbDrive(a.d, 0, BootDriveOption::SD), false);
    CHECK_EQ(needUsbDrive(a.d, 0, BootDriveOption::NVME), false);
    // sd と決めたら、SD カードがなくても USB メモリは探さず、使わない
    Drives b;
    b.set(BOOT_USB, true);
    CHECK_EQ(needUsbDrive(b.d, 0, BootDriveOption::SD), false);
    CHECK_EQ(pickBootDrive(b.d, 0, BootDriveOption::SD), -1);
}

// deploy: /pix68k/id.txt が一致するドライブだけ
void testDeploy() {
    Drives a;
    a.set(BOOT_SD, true, 1234);
    CHECK_EQ(needUsbDrive(a.d, 1234, BootDriveOption::AUTO), false);
    CHECK_EQ(pickBootDrive(a.d, 1234, BootDriveOption::AUTO), BOOT_SD);
    // 一致しなければ USB メモリを探し、そこにもなければ選ばない
    CHECK_EQ(needUsbDrive(a.d, 99, BootDriveOption::AUTO), true);
    CHECK_EQ(pickBootDrive(a.d, 99, BootDriveOption::AUTO), -1);
    a.set(BOOT_USB, true, 7);
    CHECK_EQ(pickBootDrive(a.d, 99, BootDriveOption::AUTO), -1);
    a.set(BOOT_USB, true, 99);
    CHECK_EQ(pickBootDrive(a.d, 99, BootDriveOption::AUTO), BOOT_USB);
    // NVMe と SD カードの両方が一致すれば NVMe
    Drives b;
    b.set(BOOT_NVME, true, 5).set(BOOT_SD, true, 5);
    CHECK_EQ(pickBootDrive(b.d, 5, BootDriveOption::AUTO), BOOT_NVME);
}

// USB メモリが 2 本以上 (umsd1〜umsd3): 同じ規則で 1 本を選ぶ (#116)
void testMultipleUsb() {
    const int usb1 = BOOT_USB, usb2 = BOOT_USB + 1, usb3 = BOOT_USB + 2;
    CHECK_EQ(BOOT_DRIVES, BOOT_USB + BOOT_USB_DRIVES);
    CHECK_EQ(isUsbBootDrive(BOOT_SD), false);
    CHECK_EQ(isUsbBootDrive(BOOT_NVME), false);
    CHECK_EQ(isUsbBootDrive(usb1), true);
    CHECK_EQ(isUsbBootDrive(usb3), true);
    CHECK_EQ(isUsbBootDrive(BOOT_DRIVES), false);
    CHECK_EQ(isUsbBootDrive(-1), false);
    // pix68k_drive=usb はすべての USB メモリを候補にする
    for (int i = usb1; i <= usb3; i++) CHECK_EQ(bootDriveAllowed(BootDriveOption::USB, i), true);
    CHECK_EQ(bootDriveAllowed(BootDriveOption::SD, usb2), false);
    CHECK_EQ(bootDriveAllowed(BootDriveOption::AUTO, usb3), true);
    // umsd1 が Linux の USB メモリ (/pix68k なし)、umsd2 がデータ用 (/pix68k あり) なら umsd2
    Drives a;
    a.set(usb1, false).set(usb2, true);
    CHECK_EQ(pickBootDrive(a.d, 0, BootDriveOption::AUTO), usb2);
    CHECK_EQ(pickBootDrive(a.d, 0, BootDriveOption::USB), usb2);
    // 両方にあれば先のもの
    a.set(usb1, true);
    CHECK_EQ(pickBootDrive(a.d, 0, BootDriveOption::AUTO), usb1);
    // どれにもなければ、マウントできた最初のもの
    Drives b;
    b.set(usb2, false).set(usb3, false);
    CHECK_EQ(pickBootDrive(b.d, 0, BootDriveOption::AUTO), usb2);
    // SD カードにだけ /pix68k がなくても、USB メモリのどれかにあればそれ
    Drives c;
    c.set(BOOT_SD, false).set(usb1, false).set(usb3, true);
    CHECK_EQ(pickBootDrive(c.d, 0, BootDriveOption::AUTO), usb3);
    // deploy (pix68k_id): id.txt が一致するもの。/pix68k があっても一致しなければ選ばない
    Drives d;
    d.set(usb1, true, 7).set(usb2, true, 6868);
    CHECK_EQ(pickBootDrive(d.d, 6868, BootDriveOption::AUTO), usb2);
    CHECK_EQ(pickBootDrive(d.d, 7, BootDriveOption::AUTO), usb1);
    CHECK_EQ(pickBootDrive(d.d, 99, BootDriveOption::AUTO), -1);
    // needUsbDrive() は、どの USB メモリの状態も見ない
    CHECK_EQ(needUsbDrive(d.d, 6868, BootDriveOption::AUTO), true);
    CHECK_EQ(needUsbDrive(c.d, 0, BootDriveOption::AUTO), true);
    Drives e;
    e.set(BOOT_SD, true).set(usb2, true);
    CHECK_EQ(needUsbDrive(e.d, 0, BootDriveOption::AUTO), false);
    CHECK_EQ(pickBootDrive(e.d, 0, BootDriveOption::AUTO), BOOT_SD);
}

// USB メモリを待つ間の umsdN の見張り: 抜去、番号とアドレスの使い回し (#116)
void testUsbSlotWatch() {
    int a = 0, b = 0;
    UsbSlotWatch w;
    CHECK_EQ((int)w.update(nullptr), (int)UsbSlotAction::NONE);
    // 出てきたら調べる。同じ装置のままなら何もしない
    CHECK_EQ((int)w.update(&a), (int)UsbSlotAction::PROBE);
    CHECK_EQ((int)w.update(&a), (int)UsbSlotAction::NONE);
    // 抜かれたら (削除の通知が来て、装置もなくなる) 捨てる
    w.gone = true;
    CHECK_EQ((int)w.update(nullptr), (int)UsbSlotAction::DROP);
    CHECK_EQ(w.device == nullptr, true);
    CHECK_EQ((int)w.update(nullptr), (int)UsbSlotAction::NONE);
    // また出てきたら調べ直す
    CHECK_EQ((int)w.update(&a), (int)UsbSlotAction::PROBE);
    // 1 回の UpdatePlugAndPlay() で削除と登録が済み、同じ番号・同じアドレスで別の USB メモリが出てきたとき:
    // アドレスは同じでも、削除の通知が来ていれば捨てて調べ直す
    w.gone = true;
    CHECK_EQ((int)w.update(&a), (int)UsbSlotAction::DROP_AND_PROBE);
    CHECK_EQ(w.device == &a, true);
    CHECK_EQ(w.gone, false);
    CHECK_EQ((int)w.update(&a), (int)UsbSlotAction::NONE);
    // 通知がなくてもアドレスが違えば別の装置
    CHECK_EQ((int)w.update(&b), (int)UsbSlotAction::DROP_AND_PROBE);
    // 装置がなくなれば、通知がなくても捨てる
    CHECK_EQ((int)w.update(nullptr), (int)UsbSlotAction::DROP);
}

// USB メモリを待つのをやめるか (#116)
void testUsbWaitDone() {
    const unsigned long long settle = 500, wait = 2000;
    // 規則で決まる USB メモリがあれば、すぐやめる
    CHECK_EQ(usbWaitDone(true, true, 100, 100, 0, settle, wait), true);
    // USB メモリがあっても、構成が変わってから settle たつまでは待つ (ハブの先のものが遅れて出てくる)
    CHECK_EQ(usbWaitDone(false, true, 400, 0, 0, settle, wait), false);
    CHECK_EQ(usbWaitDone(false, true, 500, 0, 0, settle, wait), true);
    // 構成が変わったら (抜去や、2 本目が出てきたら) 数え直す
    CHECK_EQ(usbWaitDone(false, true, 700, 300, 0, settle, wait), false);
    CHECK_EQ(usbWaitDone(false, true, 800, 300, 0, settle, wait), true);
    // USB メモリがなければ、wait まで待つ
    CHECK_EQ(usbWaitDone(false, false, 1999, 0, 0, settle, wait), false);
    CHECK_EQ(usbWaitDone(false, false, 2000, 0, 0, settle, wait), true);
    // 構成が変わり続けても、wait でやめる
    CHECK_EQ(usbWaitDone(false, true, 2100, 2000, 100, settle, wait), true);
}

// USB メモリでは /pix68k の下だけに書く
void testWritablePath() {
    CHECK_EQ(usbPathWritable("/pix68k/pix68k.cfg"), true);
    CHECK_EQ(usbPathWritable("/PIX68K/sub/HUMAN.HDS"), true);
    CHECK_EQ(usbPathWritable("//Pix68k//state1.p68"), true);
    CHECK_EQ(usbPathWritable("\\pix68k\\a.xdf"), true);
    CHECK_EQ(usbPathWritable("/pix68k"), false);
    CHECK_EQ(usbPathWritable("/pix68k/"), false);
    CHECK_EQ(usbPathWritable("/"), false);
    CHECK_EQ(usbPathWritable(""), false);
    CHECK_EQ(usbPathWritable("/kernel8.img"), false);
    CHECK_EQ(usbPathWritable("/pix68k2/a.xdf"), false);
    CHECK_EQ(usbPathWritable("/pix68k/../config.txt"), false);
    CHECK_EQ(usbPathWritable("/pix68k/sub/../../cmdline.txt"), false);
    CHECK_EQ(usbPathWritable("/overlays/pix68k/a"), false);
    CHECK_EQ(usbPathWritable("/pix68k./a"), true); // FAT は名前の後ろの "." を無視するので、/pix68k と同じ
    // FatFs は 0x20 より小さい文字でパスを終わりにする: "/kernel8.img" を開くので断る
    CHECK_EQ(usbPathWritable("/kernel8.img\t/../pix68k/x"), false);
    CHECK_EQ(usbPathWritable("/kernel8.img\x01/../pix68k/x"), false);
    CHECK_EQ(usbPathWritable("/pix68k/a\rb"), false);
    CHECK_EQ(usbPathWritable("/pix68k/a\x7f"), false);
    // ":" の前は normalizePath() が捨てる
    CHECK_EQ(usbPathWritable("/kernel8.img:/pix68k/x"), false);
    CHECK_EQ(usbPathWritable("USB:/pix68k/x"), false); // ドライブ名は付けずに渡す
}

// ネットワークの口とステートの書き込みの判定 (クラスタの大きさは 1 として、中身 + 1 を使うと数える)
const UsbWriteKind NEW = UsbWriteKind::NEW_FILE, REMOVE = UsbWriteKind::REMOVE, NEW_DIR = UsbWriteKind::NEW_DIR,
                   RENAME = UsbWriteKind::RENAME;

void testUsbWrite() {
    const unsigned long long MB = 1 << 20;
    // NVMe と SD カードでは何も断らない
    CHECK_EQ((int)checkUsbWrite(false, "/kernel8.img", NEW, 500 * MB, false, 0, 32 * MB), (int)UsbWriteCheck::OK);
    CHECK_EQ((int)checkUsbWrite(true, "/kernel8.img", NEW, 1, true, 400 * MB, 32 * MB), (int)UsbWriteCheck::OUTSIDE);
    CHECK_EQ((int)checkUsbWrite(true, "/kernel8.img", REMOVE, 0, true, 400 * MB, 32 * MB), (int)UsbWriteCheck::OUTSIDE);
    CHECK_EQ((int)checkUsbWrite(true, "/pix68k/a.hds", NEW, 100 * MB, true, 400 * MB, 32 * MB), (int)UsbWriteCheck::OK);
    CHECK_EQ((int)checkUsbWrite(true, "/pix68k/a.hds", NEW, 390 * MB, true, 400 * MB, 32 * MB), (int)UsbWriteCheck::NO_ROOM);
    CHECK_EQ((int)checkUsbWrite(true, "/pix68k/a.hds", NEW, 1, false, 400 * MB, 32 * MB), (int)UsbWriteCheck::NO_ROOM);
    // 消すだけなら空きは見ない
    CHECK_EQ((int)checkUsbWrite(true, "/pix68k/a.hds", REMOVE, 0, false, 0, 32 * MB), (int)UsbWriteCheck::OK);
}

// 受け取っている途中のファイルの残りを、ほかの保存の判定から引く
void testBudget() {
    const unsigned long long MB = 1 << 20;
    UsbWriteBudget b;
    // 空き 65MB で 32MB の受け取りを始める
    CHECK_EQ((int)checkUsbWrite(true, "/pix68k/a.hds", NEW, 32 * MB, true, 65 * MB, 32 * MB, b.pending), (int)UsbWriteCheck::OK);
    b.begin(32 * MB);
    // 途中のステート (12MB): 残りを引くと 32MB が残らないので断る (引かなければ通ってしまう)
    CHECK_EQ((int)checkUsbWrite(true, "/pix68k/state1.p68", NEW, 12 * MB, true, 65 * MB, 32 * MB, 0), (int)UsbWriteCheck::OK);
    CHECK_EQ((int)checkUsbWrite(true, "/pix68k/state1.p68", NEW, 12 * MB, true, 65 * MB, 32 * MB, b.pending),
             (int)UsbWriteCheck::NO_ROOM);
    // 16MB 書いたあと (空き 49MB、残り 16MB) も同じ
    b.end(16 * MB);
    CHECK_EQ(b.pending, 16 * MB);
    CHECK_EQ((int)checkUsbWrite(true, "/pix68k/state1.p68", NEW, 2 * MB, true, 49 * MB, 32 * MB, b.pending), (int)UsbWriteCheck::NO_ROOM);
    // もう 1 つの受け取りも断る
    CHECK_EQ((int)checkUsbWrite(true, "/pix68k/b.hds", NEW, 2 * MB, true, 49 * MB, 32 * MB, b.pending), (int)UsbWriteCheck::NO_ROOM);
    // 書き終えれば残りは 0
    b.end(16 * MB);
    CHECK_EQ(b.pending, 0);
    // 空きが多ければ、受け取りの途中でも書ける
    b.begin(32 * MB);
    CHECK_EQ((int)checkUsbWrite(true, "/pix68k/state1.p68", NEW, 12 * MB, true, 400 * MB, 32 * MB, b.pending), (int)UsbWriteCheck::OK);
    // やめたときは残りを返す。多く返しても 0 より下にならない
    b.end(100 * MB);
    CHECK_EQ(b.pending, 0);
    // 2 回の begin() は足し合わせる
    b.begin(3);
    b.begin(4);
    CHECK_EQ(b.pending, 7);
    b.end(7);
    // 残りが空きより多い (空きの値がずれている) ときは断る
    CHECK_EQ((int)checkUsbWrite(true, "/pix68k/a", NEW, 1, true, 10 * MB, 0, 20 * MB), (int)UsbWriteCheck::NO_ROOM);
    // 消すだけなら残りは見ない。USB メモリでなければ何も見ない
    CHECK_EQ((int)checkUsbWrite(true, "/pix68k/a", REMOVE, 0, true, 0, 32 * MB, 20 * MB), (int)UsbWriteCheck::OK);
    CHECK_EQ((int)checkUsbWrite(false, "/a", NEW, 100 * MB, true, 0, 32 * MB, 20 * MB), (int)UsbWriteCheck::OK);
}

unsigned long long roundUp(unsigned long long bytes, unsigned long long c) {
    unsigned long long out = 12345;
    CHECK_EQ(clusterRoundUp(bytes, c, out), true);
    return out;
}

// クラスタの単位で数える: 1 バイトのファイルも 1 クラスタを使い、名前を足すとディレクトリが 1 クラスタ伸びることがある
void testClusters() {
    const unsigned long long MB = 1 << 20, C = 32768, MAX = ~0ull;
    CHECK_EQ(roundUp(0, C), 0);
    CHECK_EQ(roundUp(1, C), C);
    CHECK_EQ(roundUp(C, C), C);
    CHECK_EQ(roundUp(C + 1, C), 2 * C);
    CHECK_EQ(roundUp(16400, C), C);
    CHECK_EQ(roundUp(5, 0), 5);
    CHECK_EQ(roundUp(5, 1), 5);
    // 表せる境界と、表せないとき (前は 0 に戻っていた)
    unsigned long long out = 0;
    CHECK_EQ(roundUp(MAX - 4095, 4096), MAX - 4095); // 4096 の倍数で最大のもの
    CHECK_EQ(clusterRoundUp(MAX - 4094, 4096, out), false);
    CHECK_EQ(clusterRoundUp(MAX, 4096, out), false);
    CHECK_EQ(roundUp(MAX, 1), MAX);
    // ディレクトリの余裕: 64KiB をクラスタに切り上げる。クラスタが大きいときは最低 4 クラスタ
    const unsigned long long K = 1024, M64 = 64 * K;
    CHECK_EQ(usbDirMargin(512, out), true);
    CHECK_EQ(out, M64);
    CHECK_EQ(usbDirMargin(4096, out), true);
    CHECK_EQ(out, M64);
    CHECK_EQ(usbDirMargin(16384, out), true);
    CHECK_EQ(out, M64);
    CHECK_EQ(usbDirMargin(C, out), true); // 32KiB のクラスタ: 4 クラスタ
    CHECK_EQ(out, 4 * C);
    CHECK_EQ(usbDirMargin(65536, out), true);
    CHECK_EQ(out, 4 * 65536);
    CHECK_EQ(usbDirMargin(3000, out), true); // 2 の累乗でなくても切り上げる
    CHECK_EQ(out, 66000);
    CHECK_EQ(usbDirMargin(0, out), true);
    CHECK_EQ(out, M64);
    CHECK_EQ(usbDirMargin(MAX / 4 + 1, out), false);
    // 使う大きさ: 中身 + ディレクトリの余裕。0 バイトのファイルも余裕の分。ディレクトリは 1 クラスタ + 余裕
    CHECK_EQ(usbWriteNeed(NEW, 0, 4096, out), true);
    CHECK_EQ(out, M64);
    CHECK_EQ(usbWriteNeed(NEW, 1, 4096, out), true);
    CHECK_EQ(out, M64 + 4096);
    CHECK_EQ(usbWriteNeed(NEW, 4097, 4096, out), true);
    CHECK_EQ(out, M64 + 2 * 4096);
    CHECK_EQ(usbWriteNeed(NEW, 1, C, out), true);
    CHECK_EQ(out, 5 * C);
    CHECK_EQ(usbWriteNeed(RENAME, 999, 4096, out), true);
    CHECK_EQ(out, M64);
    CHECK_EQ(usbWriteNeed(NEW_DIR, 999, 4096, out), true);
    CHECK_EQ(out, M64 + 4096);
    CHECK_EQ(usbWriteNeed(REMOVE, 999, 4096, out), true);
    CHECK_EQ(out, 0);
    CHECK_EQ(usbWriteNeed(NEW, MAX - 4095, 4096, out), false); // 中身は表せるが、ディレクトリの余裕を足せない
    CHECK_EQ(usbWriteNeed(NEW, MAX - 4095 - M64, 4096, out), true);
    CHECK_EQ(out, MAX - 4095);
    CHECK_EQ(usbWriteNeed(NEW, MAX, 4096, out), false);
    CHECK_EQ(usbWriteNeed(NEW_DIR, 0, MAX / 4 + 1, out), false);
    // 表せない大きさは断る
    CHECK_EQ((int)checkUsbWrite(true, "/pix68k/a", NEW, MAX, true, MAX, 0, 0, 4096), (int)UsbWriteCheck::NO_ROOM);
    CHECK_EQ((int)checkUsbWrite(true, "/pix68k/a", NEW, MAX - 4095, true, MAX, 0, 0, 4096), (int)UsbWriteCheck::NO_ROOM);

    // 空き 32MB + 余裕 2 つ分 + 1 クラスタで 1 バイトの受け取りを始める (中身の 1 クラスタと余裕を取る)
    const unsigned long long free0 = 32 * MB + 9 * C;
    UsbWriteBudget b;
    CHECK_EQ((int)checkUsbWrite(true, "/pix68k/a", NEW, 1, true, free0, 32 * MB, b.pending, C), (int)UsbWriteCheck::OK);
    UploadPlan up(1, C);
    CHECK_EQ(up.left, 5 * C);
    b.begin(up.left);
    // 受信を待つ間の 16400 バイトの SRAM (1 クラスタ + 余裕の 4 クラスタが要る): 残りは 4 クラスタなので断る
    CHECK_EQ((int)checkUsbWrite(true, "/pix68k/SRAM.DAT", NEW, 16400, true, free0, 32 * MB, b.pending, C), (int)UsbWriteCheck::NO_ROOM);
    // 受け取りの分を引かなければ通ってしまう
    CHECK_EQ((int)checkUsbWrite(true, "/pix68k/SRAM.DAT", NEW, 16400, true, free0, 32 * MB, 0, C), (int)UsbWriteCheck::OK);
}

// 512 バイトのクラスタで、親のディレクトリが埋まっているとき。200 文字の名前 (長い名前のエントリ 16 個 + 短い名前 1 個 =
// 544 バイト) の 0 バイトのファイルを .part で作り、名前を変えると、ディレクトリが 2〜3 クラスタ伸びる。
// 空きが 32MiB + 512 バイトしかなければ断る (前は、ディレクトリの分を 1 クラスタと数えて通していた)
void testFullDirectory() {
    const unsigned long long MiB = 1 << 20, C = 512, M64 = 64 * 1024;
    char path[230] = "/pix68k/";
    size_t n = strlen(path);
    for (int i = 0; i < 200; i++) path[n++] = (char)('a' + i % 26);
    path[n] = 0;
    CHECK_EQ(usbPathWritable(path), true);
    // 0 バイトの PUT
    UsbWriteBudget b;
    CHECK_EQ((int)beginNetWrite(b, true, path, 0, true, 32 * MiB + C, 32 * MiB, C), (int)UsbWriteCheck::NO_ROOM);
    CHECK_EQ(b.pending, 0);
    CHECK_EQ((int)beginNetWrite(b, true, path, 0, true, 32 * MiB + 3 * C, 32 * MiB, C), (int)UsbWriteCheck::NO_ROOM);
    CHECK_EQ((int)beginNetWrite(b, true, path, 0, true, 32 * MiB + M64 - C, 32 * MiB, C), (int)UsbWriteCheck::NO_ROOM);
    CHECK_EQ(b.pending, 0);
    // 余裕の 64KiB があれば通る。ディレクトリが 3 クラスタ (1536 バイト) 伸びても、32MiB は残る
    CHECK_EQ((int)beginNetWrite(b, true, path, 0, true, 32 * MiB + M64, 32 * MiB, C), (int)UsbWriteCheck::OK);
    CHECK_EQ(b.pending, M64);
    CHECK_EQ(32 * MiB + M64 - 3 * C >= 32 * MiB, true);
    // 途中で止まった置き換えを戻す名前の変更と、新しいファイル、ディレクトリの作成も同じ
    CHECK_EQ((int)checkUsbWrite(true, path, RENAME, 0, true, 32 * MiB + C, 32 * MiB, 0, C), (int)UsbWriteCheck::NO_ROOM);
    CHECK_EQ((int)checkUsbWrite(true, path, RENAME, 0, true, 32 * MiB + M64, 32 * MiB, 0, C), (int)UsbWriteCheck::OK);
    CHECK_EQ((int)checkUsbWrite(true, path, RENAME, 0, false, 400 * MiB, 32 * MiB, 0, C), (int)UsbWriteCheck::NO_ROOM);
    CHECK_EQ((int)checkUsbWrite(true, "/kernel8.img", RENAME, 0, true, 400 * MiB, 32 * MiB, 0, C), (int)UsbWriteCheck::OUTSIDE);
    CHECK_EQ((int)checkUsbWrite(false, "/kernel8.img", RENAME, 0, false, 0, 32 * MiB, 0, C), (int)UsbWriteCheck::OK);
    CHECK_EQ((int)checkUsbWrite(true, path, NEW, 0, true, 32 * MiB + C, 32 * MiB, 0, C), (int)UsbWriteCheck::NO_ROOM);
    CHECK_EQ((int)checkUsbWrite(true, path, NEW, 1, true, 32 * MiB + M64, 32 * MiB, 0, C), (int)UsbWriteCheck::NO_ROOM);
    CHECK_EQ((int)checkUsbWrite(true, path, NEW, 1, true, 32 * MiB + M64 + C, 32 * MiB, 0, C), (int)UsbWriteCheck::OK);
    CHECK_EQ((int)checkUsbWrite(true, "/pix68k/x", NEW_DIR, 0, true, 32 * MiB + M64, 32 * MiB, 0, C), (int)UsbWriteCheck::NO_ROOM);
    CHECK_EQ((int)checkUsbWrite(true, "/pix68k/x", NEW_DIR, 0, true, 32 * MiB + M64 + C, 32 * MiB, 0, C), (int)UsbWriteCheck::OK);
}

// ネットワークの口の判定と予約: 消すだけ (負の値) では取らず、0 バイトでも取り、終わりで返すと 0 に戻る
void testNetWrite() {
    const unsigned long long MiB = 1 << 20, C = 4096, M64 = 64 * 1024;
    UsbWriteBudget b;
    // 消すだけ: 空きを読めなくても通り、何も取らない
    CHECK_EQ((int)beginNetWrite(b, true, "/pix68k/a", -1, false, 0, 32 * MiB, C), (int)UsbWriteCheck::OK);
    CHECK_EQ(b.pending, 0);
    CHECK_EQ((int)beginNetWrite(b, true, "/kernel8.img", -1, true, 400 * MiB, 32 * MiB, C), (int)UsbWriteCheck::OUTSIDE);
    CHECK_EQ(b.pending, 0);
    // 0 バイト: 余裕の分を取る。終わりで返す
    CHECK_EQ((int)beginNetWrite(b, true, "/pix68k/a", 0, true, 400 * MiB, 32 * MiB, C), (int)UsbWriteCheck::OK);
    CHECK_EQ(b.pending, M64);
    {
        UploadPlan up(0, C);
        CHECK_EQ(up.left, M64);
        b.end(up.wrote(0));
        CHECK_EQ(b.pending, M64);
        b.end(up.finish());
    }
    CHECK_EQ(b.pending, 0);
    // 10000 バイト: 3 クラスタ + 余裕。書くたびに減り、終わりで 0
    CHECK_EQ((int)beginNetWrite(b, true, "/pix68k/a", 10000, true, 400 * MiB, 32 * MiB, C), (int)UsbWriteCheck::OK);
    CHECK_EQ(b.pending, 3 * C + M64);
    {
        UploadPlan up(10000, C);
        b.end(up.wrote(8192));
        CHECK_EQ(b.pending, C + M64);
        b.end(up.wrote(1808));
        CHECK_EQ(b.pending, M64);
        b.end(up.finish());
    }
    CHECK_EQ(b.pending, 0);
    // 断ったとき (場所、空き、空きを読めない) は取らない
    CHECK_EQ((int)beginNetWrite(b, true, "/kernel8.img", 10, true, 400 * MiB, 32 * MiB, C), (int)UsbWriteCheck::OUTSIDE);
    CHECK_EQ((int)beginNetWrite(b, true, "/pix68k/a", 10, true, 32 * MiB, 32 * MiB, C), (int)UsbWriteCheck::NO_ROOM);
    CHECK_EQ((int)beginNetWrite(b, true, "/pix68k/a", 10, false, 400 * MiB, 32 * MiB, C), (int)UsbWriteCheck::NO_ROOM);
    CHECK_EQ(b.pending, 0);
    // 2 つ目の受け取りは、1 つ目が取った分を引いて判定する
    const unsigned long long free1 = 32 * MiB + 2 * M64 + 2 * C;
    CHECK_EQ((int)beginNetWrite(b, true, "/pix68k/a", 1, true, free1, 32 * MiB, C), (int)UsbWriteCheck::OK);
    CHECK_EQ((int)beginNetWrite(b, true, "/pix68k/b", C + 1, true, free1, 32 * MiB, C), (int)UsbWriteCheck::NO_ROOM);
    CHECK_EQ((int)beginNetWrite(b, true, "/pix68k/b", C, true, free1, 32 * MiB, C), (int)UsbWriteCheck::OK);
    CHECK_EQ(b.pending, 2 * M64 + 2 * C);
    b.end(UploadPlan(1, C).finish());
    b.end(UploadPlan(C, C).finish());
    CHECK_EQ(b.pending, 0);
    // NVMe と SD カード: 何も断らず、何も取らない
    CHECK_EQ((int)beginNetWrite(b, false, "/kernel8.img", 100 * MiB, false, 0, 32 * MiB, 1), (int)UsbWriteCheck::OK);
    CHECK_EQ((int)beginNetWrite(b, false, "/kernel8.img", -1, false, 0, 32 * MiB, 1), (int)UsbWriteCheck::OK);
    CHECK_EQ(b.pending, 0);
}

// 受け取りの残りの返し方: どの終わり方でも、返した合計が始めに取った大きさと同じで、最後の残りが 0 になる。
// 始めに取るのは、中身 (クラスタに切り上げる) とディレクトリの余裕 (64KiB)。余裕は finish() で返す
void testUploadPlan() {
    const unsigned long long C = 4096, M = 64 * 1024;
    // 正常に終わる: 10000 バイト (3 クラスタ) を 3000 バイトずつ書く
    {
        UsbWriteBudget b;
        UploadPlan up(10000, C);
        CHECK_EQ(up.left, 3 * C + M);
        b.begin(up.left);
        unsigned long long freed = 0, sum = 0;
        freed = up.wrote(3000); // 1 つ目のクラスタを使い始めた
        CHECK_EQ(freed, C);
        b.end(freed), sum += freed;
        CHECK_EQ(b.pending, 2 * C + M);
        freed = up.wrote(3000); // 6000: 2 つ目
        CHECK_EQ(freed, C);
        b.end(freed), sum += freed;
        freed = up.wrote(3000); // 9000: 3 つ目
        CHECK_EQ(freed, C);
        b.end(freed), sum += freed;
        CHECK_EQ(b.pending, M); // ディレクトリの余裕は、名前を変え終わるまで持つ
        freed = up.wrote(1000); // 10000: 同じクラスタの中
        CHECK_EQ(freed, 0);
        b.end(freed), sum += freed;
        freed = up.finish();
        CHECK_EQ(freed, M);
        b.end(freed), sum += freed;
        CHECK_EQ(sum, 3 * C + M);
        CHECK_EQ(b.pending, 0);
    }
    // 途中の書き込みの失敗か切断: 1 回書いたあとでやめる
    {
        UsbWriteBudget b;
        UploadPlan up(10000, C);
        b.begin(up.left);
        unsigned long long sum = 0, freed = up.wrote(5000); // 2 クラスタ
        CHECK_EQ(freed, 2 * C);
        b.end(freed), sum += freed;
        CHECK_EQ(b.pending, C + M);
        freed = up.finish();
        CHECK_EQ(freed, C + M);
        b.end(freed), sum += freed;
        CHECK_EQ(sum, 3 * C + M);
        CHECK_EQ(b.pending, 0);
        CHECK_EQ(up.finish(), 0); // 2 回返さない
        CHECK_EQ(up.wrote(100), 0);
    }
    // 始めた直後の中止: 何も書かずにやめる
    {
        UsbWriteBudget b;
        UploadPlan up(10000, C);
        b.begin(up.left);
        const unsigned long long freed = up.finish();
        CHECK_EQ(freed, 3 * C + M);
        b.end(freed);
        CHECK_EQ(b.pending, 0);
    }
    // 全体より多く書いたと言われても、取った分より多くは返さない
    {
        UploadPlan up(100, C);
        CHECK_EQ(up.left, C + M);
        CHECK_EQ(up.wrote(500), C);
        CHECK_EQ(up.wrote(500), 0);
        CHECK_EQ(up.finish(), M);
    }
    // 0 バイトの受け取りも、ディレクトリの余裕を取る
    {
        UploadPlan empty(0, C);
        CHECK_EQ(empty.left, M);
        CHECK_EQ(empty.wrote(0), 0);
        CHECK_EQ(empty.finish(), M);
    }
    // 表せない大きさは何も取らない (判定で断られるので、受け取りは始まらない)
    {
        UploadPlan huge(~0ull, C);
        CHECK_EQ(huge.left, 0);
        CHECK_EQ(huge.wrote(100), 0);
        CHECK_EQ(huge.finish(), 0);
    }
    // 2 つの受け取りが重なる: 合計を持ち、片方が終わってももう片方の分は残る
    {
        UsbWriteBudget b;
        UploadPlan a(10000, C), c(5000, C);
        b.begin(a.left);
        b.begin(c.left);
        CHECK_EQ(b.pending, 5 * C + 2 * M);
        b.end(a.wrote(10000));
        b.end(a.finish());
        CHECK_EQ(b.pending, 2 * C + M);
        b.end(c.finish());
        CHECK_EQ(b.pending, 0);
    }
}

// 読み書きの回数を数える作り物の DiskSync
struct FakeSync {
    int loads = 0, ioServices = 0, decideOnly = 0, paused = 0;
    unsigned serviceLoads(int) {
        loads++;
        return 1;
    }
    void service(unsigned long long, bool io) { (io ? ioServices : decideOnly)++; }
    void servicePaused(unsigned long long) { paused++; }
    int ios() const { return loads + ioServices + paused; }
};

// USB の転送の途中 (insideUsbTransfer) では、boot ドライブが USB メモリのときだけ読み書きしない
void testServiceDisks() {
    for (int usb = 0; usb < 2; usb++)
        for (int inside = 0; inside < 2; inside++)
            for (int menu = 0; menu < 3; menu++) { // 0: 閉じている、1: 開いたが止めていない、2: 開いて止めている
                FakeSync s;
                const bool io = diskIoAllowed(usb != 0, inside != 0);
                const unsigned loads = serviceDiskSync(s, 0, io, menu != 0, menu == 2);
                const bool blocked = usb != 0 && inside != 0;
                CHECK_EQ(loads, blocked ? 0u : 1u);
                CHECK_EQ(s.loads, blocked ? 0 : 1);
                // 読み書きの回数: 止めたら 0。そうでなければ、チャンクの読み込みと、書き戻し (メニューを開いて
                // 止めていないときは書き戻さない)
                CHECK_EQ(s.ios(), blocked ? 0 : menu == 1 ? 1 : 2);
                // 書き戻すかどうかを決めるだけの呼び出しは、メニューを閉じているときに残る
                CHECK_EQ(s.decideOnly, blocked && menu == 0 ? 1 : 0);
                CHECK_EQ(s.paused, !blocked && menu == 2 ? 1 : 0);
            }
}

// USB の転送の中の待ち時間のディスクの読み書き
void testDiskIo() {
    CHECK_EQ(diskIoAllowed(false, false), true);
    CHECK_EQ(diskIoAllowed(false, true), true); // SD カードは前と同じ
    CHECK_EQ(diskIoAllowed(true, false), true); // 主ループと、ROM や SoundFont を読む合間
    CHECK_EQ(diskIoAllowed(true, true), false);
}

void testRoom() {
    const unsigned long long MB = 1 << 20;
    CHECK_EQ(driveHasRoom(428 * MB, 12 * MB, 32 * MB), true);
    CHECK_EQ(driveHasRoom(44 * MB, 12 * MB, 32 * MB), true);
    CHECK_EQ(driveHasRoom(44 * MB - 1, 12 * MB, 32 * MB), false);
    CHECK_EQ(driveHasRoom(31 * MB, 0, 32 * MB), false);
    CHECK_EQ(driveHasRoom(32 * MB, 0, 32 * MB), true);
    CHECK_EQ(driveHasRoom(0, 1, 0), false);
}

} // namespace

int main() {
    testWritablePath();
    testUsbWrite();
    testDiskIo();
    testBudget();
    testClusters();
    testUploadPlan();
    testFullDirectory();
    testNetWrite();
    testServiceDisks();
    testRoom();
    testOption();
    testSdFirst();
    testUsbFallback();
    testExplicit();
    testDeploy();
    testMultipleUsb();
    testUsbSlotWatch();
    testUsbWaitDone();
    return finish();
}
