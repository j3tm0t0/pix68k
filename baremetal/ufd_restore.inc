// 設定メニューの「Write XDF」: XDF のイメージを実物のディスク (USB-FDD) に書き戻す (#386、experimental)。machine_run.cpp が、
// Pi 4 / Pi 5 のときだけ取り込む。使えるのは pix68k.cfg の experimental_usb_fdd_write= で許したドライブだけ。手順は platform/floppy_restore.h、送ってよいコマンドは platform/ufi_gate.h
// Circle の USB-FDD に UFI のコマンドを送る。抜かれたら (装置のオブジェクトが消える前に知らせが来る)、そのあとは送らない
class CUfiUsb final : public platform::UfiTransport {
public:
    explicit CUfiUsb(CUSBFloppyDiskDevice* dev) : m_pDev(dev) { m_Handle = dev->RegisterRemovedHandler(OnRemoved, this); }
    ~CUfiUsb() {
        if (m_pDev != nullptr) m_pDev->UnregisterRemovedHandler(m_Handle);
    }
    platform::UfiReply command(const uint8_t cdb[12], void* data, size_t len, bool in) override {
        platform::UfiReply r; // BROKEN
        if (m_pDev == nullptr) return r;
        u8 status[2];
        int bytes = 0;
        switch (m_pDev->Pix68kCommand(cdb, data, len, in, status, &bytes)) {
        case 0: r.phase = platform::UfiReply::DONE; break;
        case 1: r.phase = platform::UfiReply::REFUSED; break;
        case 2: r.phase = platform::UfiReply::COMMAND; break;
        default: break;
        }
        r.asc = status[0];
        r.ascq = status[1];
        r.bytes = bytes;
        return r;
    }
    bool armWrite(uint32_t lba, uint32_t count) override { return m_pDev != nullptr && m_pDev->Pix68kArmWrite(lba, count); }
    void waitMs(unsigned ms) override { CTimer::Get()->MsDelay(ms); }

private:
    static void OnRemoved(CDevice*, void* context) {
        static_cast<CUfiUsb*>(context)->m_pDev = nullptr;
        platform::UfiGate::confirm(false); // 別のドライブが同じ番号になっても書かないように
    }
    CUSBFloppyDiskDevice* m_pDev;
    CDevice::TRegistrationHandle m_Handle;
};

namespace {
// 書き込みの間だけ置いておくファイル。残っていれば、前の書き込みが終わっていない
const char kUfdMarkName[] = "/pix68k/ufd-write.txt";
} // namespace

struct CKernel::TUfdWrite {
    CUfiUsb usb;
    platform::FloppyRestore restore;
    char name[platform::Settings::PATH_SIZE] = {};
    char backup[platform::REAL_FLOPPY_IMAGE_NAME_SIZE] = {};
    uint32_t crc = 0;
    explicit TUfdWrite(CUSBFloppyDiskDevice* dev) : usb(dev) {}
};

bool CKernel::ufdProgress(int phase, unsigned done, unsigned total) {
    using P = platform::FloppyRestoreProgress;
    KeepAlive();
    char buf[platform::Menu::LINE_BYTES];
    platform::MenuText t(buf, sizeof buf);
    platform::floppy_write_text::progress(t, (P::Phase)phase, done, total);
    m_Menu.showProgress(buf);
    DrawMenu();
    // 読んでいる間だけ、Esc でやめられる
    bool go = true;
    for (int u; (u = PopMenuKey()) >= 0;)
        if (u == platform::menu_key::ESC && phase == P::READING) go = false;
    return go;
}

namespace {
struct CUfdProgress final : platform::FloppyRestoreProgress {
    CKernel* kernel;
    explicit CUfdProgress(CKernel* k) : kernel(k) {}
    bool progress(Phase phase, uint32_t done, uint32_t total) override {
        return kernel->ufdProgress((int)phase, done, total);
    }
};
} // namespace

void CKernel::cancelFloppyWrite() {
    platform::UfiGate::confirm(false);
    if (m_pUfdWrite == nullptr) return;
    delete m_pUfdWrite;
    m_pUfdWrite = nullptr;
    m_MenuKeyTail = m_MenuKeyHead; // 待っている間に押したキーは捨てる
}

bool CKernel::prepareFloppyWrite(const char* name, FloppyWriteInfo& info, platform::MenuText& msg) {
    using R = platform::FloppyRestore;
    cancelFloppyWrite();
    const int unit = platform::UfiGate::enabledUnit();
    if (unit < 0) return false;
    const size_t len = strlen(name);
    if (m_BootDrive == nullptr || len < 5 || len >= platform::Settings::PATH_SIZE || strcasecmp(name + len - 4, ".xdf") != 0 ||
        strchr(name, '/') != nullptr || platform::isReplaceTempName(name)) {
        msg.add("choose an .XDF file");
        return false;
    }
    for (int d = 0; d < 2; d++)
        if (m_RealFloppy[d]) {
            msg.add("eject the USB-FDD disk from FD").add((long)d).add(" first");
            return false;
        }
    if (m_FukReading || m_FukReadRequest >= 0) {
        msg.add("the FUK is being read; not written");
        return false;
    }
    if (!FlushDisks()) {
        msg.add("cannot write back the disks; not written");
        return false;
    }
    // 書き込み元 (最初に全部読む)、今の中身、1 トラック。一度だけ確保して使い回す
    if (m_pUfdBuf == nullptr) {
        const size_t need = 2 * R::IMAGE_BYTES + R::TRACK_BYTES;
        size_t freeBytes = 0;
        if (HeapHasRoom(need, freeBytes)) m_pUfdBuf = new u8[need];
        if (m_pUfdBuf == nullptr) {
            msg.add("not enough memory");
            return false;
        }
    }
    u8* const image = m_pUfdBuf;
    u8* const old = image + R::IMAGE_BYTES;
    u8* const work = old + R::IMAGE_BYTES;
    CString path;
    path.Format("%s/pix68k/%s", m_BootDrive, name);
    {
        FIL f;
        if (f_open(&f, path, FA_READ | FA_OPEN_EXISTING) != FR_OK) {
            msg.add("cannot read the image");
            return false;
        }
        UINT got = 0;
        const bool size = f_size(&f) == R::IMAGE_BYTES;
        const bool ok = size && f_read(&f, image, R::IMAGE_BYTES, &got) == FR_OK && got == R::IMAGE_BYTES;
        f_close(&f);
        if (!ok) {
            msg.add(size ? "cannot read the image" : "not a 1261568-byte XDF");
            return false;
        }
    }
    CString devName;
    devName.Format("ufd%d", unit + 1);
    CDevice* dev = m_UsbOk ? m_DeviceNameService.GetDevice(devName, TRUE) : nullptr;
    if (dev == nullptr) {
        msg.add("usb-fdd:").add((long)unit).add(" not found (insert before power-on)");
        return false;
    }
    m_pUfdWrite = new TUfdWrite(static_cast<CUSBFloppyDiskDevice*>(dev));
    TUfdWrite& w = *m_pUfdWrite;
    platform::copyString(w.name, sizeof w.name, name);
    w.crc = platform::crc32Of(image, R::IMAGE_BYTES);
    CUfdProgress progress(this);
    m_Logger.Write(FromMachine, LogNotice, "usb-fdd:%d: reading the disk before writing %s to it", unit, name);
    const bool prepared = w.restore.prepare(&w.usb, old, work, &progress);
    msg.clear(); // 進みの表示
    if (!prepared) {
        const platform::UfiDisk& d = w.restore.disk();
        m_Logger.Write(FromMachine, LogWarning, "usb-fdd:%d: not written: error %d, %s (sense %x/%02x/%02x), track %u", unit,
                       (int)w.restore.error(), platform::floppy_write_text::ufiErrorName(d.error()), d.senseKey(), d.asc(), d.ascq(),
                       (unsigned)w.restore.track());
        platform::floppy_write_text::prepareError(msg, w.restore.error(), d.error(), w.restore.track());
        cancelFloppyWrite();
        return false;
    }
    // 今の中身の控え。同じ名前があれば、中身の CRC も確かめる。読めないセクタがあれば、その一覧 (.TXT) も残す
    platform::realFloppyImageName(w.restore.oldCrc(), w.backup, w.restore.partial());
    CString save, tmp;
    save.Format("%s/pix68k/%s", m_BootDrive, w.backup);
    tmp.Format("%s.new", (const char*)save);
    CFloppySaveFs fs;
    const platform::FloppyImageSave r =
        platform::saveFloppyImageFile(fs, save, tmp, old, (unsigned)R::IMAGE_BYTES, w.restore.oldCrc());
    uint32_t crc = 0;
    bool saved = platform::floppyImageSaved(r) && fs.crc32(save, crc);
    if (saved && w.restore.partial()) {
        CString list, txt, txtTmp, s;
        list.Format("%s: %u sectors in %u tracks of the disk could not be read\n", w.backup, (unsigned)w.restore.badBlocks(),
                    (unsigned)w.restore.badTracks());
        for (u32 b = 0; b < R::BLOCKS; b++)
            if (w.restore.badBlock(b)) {
                s.Format("C%u H%u R%u\n", (unsigned)(b / 16), (unsigned)(b / 8 % 2), (unsigned)(b % 8 + 1));
                list.Append(s);
            }
        char stem[sizeof w.backup];
        platform::copyString(stem, sizeof stem, w.backup);
        stem[strlen(stem) - 4] = 0; // .XDF
        txt.Format("%s/pix68k/%s.TXT", m_BootDrive, stem);
        txtTmp.Format("%s.new", (const char*)txt);
        const uint32_t want = platform::crc32Of((const u8*)(const char*)list, list.GetLength());
        uint32_t got = 0;
        saved = platform::floppyImageSaved(platform::saveFloppyImageFile(fs, txt, txtTmp, (const u8*)(const char*)list,
                                                                        (unsigned)list.GetLength(), want)) &&
                fs.crc32(txt, got) && got == want;
        m_Logger.Write(FromMachine, LogWarning, "usb-fdd:%d: %u sectors could not be read; see %s", unit,
                       (unsigned)w.restore.badBlocks(), (const char*)txt);
    }
    if (!w.restore.backupDone(saved, crc)) {
        m_Logger.Write(FromMachine, LogWarning, "usb-fdd:%d: cannot save the backup %s (%d, FRESULT %d); not written", unit,
                       (const char*)save, (int)r, (int)fs.last);
        msg.add("cannot save the backup; not written");
        cancelFloppyWrite();
        return false;
    }
    platform::floppy_write_text::confirmInfo(info, unit, name, w.crc, w.restore.oldCrc(), w.backup, memcmp(image, old, 1024) == 0,
                                             w.restore.disk().protect() == platform::UfiDisk::UNKNOWN);
    if (w.restore.partial()) platform::floppy_write_text::partialInfo(info, w.restore);
    m_MenuKeyTail = m_MenuKeyHead; // 待っている間に押したキーで答えないように
    return true;
}

// 書き始める前の大きさに戻す (自分の行だけを消す。ほかのディスクの記録は残す)
static void UnmarkUfdWrite(const char* mark, FSIZE_t size) {
    if (size == 0) {
        f_unlink(mark);
        return;
    }
    FIL f;
    if (f_open(&f, mark, FA_WRITE | FA_OPEN_EXISTING) != FR_OK) return;
    if (f_size(&f) > size && f_lseek(&f, size) == FR_OK) f_truncate(&f);
    f_close(&f);
}

bool CKernel::runFloppyWrite(bool acceptPartial, platform::MenuText& msg) {
    if (m_pUfdWrite == nullptr) return false;
    TUfdWrite& w = *m_pUfdWrite;
    CUfdProgress progress(this);
    CString mark, text;
    mark.Format("%s%s", m_BootDrive, kUfdMarkName);
    FILINFO fi;
    const FRESULT markStat = f_stat(mark, &fi);
    if (markStat != FR_OK && markStat != FR_NO_FILE && markStat != FR_NO_PATH) { // 前の記録があるか分からない
        msg.add("cannot read the boot drive; not written");
        cancelFloppyWrite();
        return false;
    }
    const FSIZE_t markWas = markStat == FR_OK ? fi.fsize : 0;
    if (acceptPartial) w.restore.acceptPartial(); // 控えが欠けていることを、別に確かめた
    bool ok = w.restore.checkSame(&progress);
    msg.clear();
    if (ok) {
        // 前の記録があれば残す (前の控えの名前が分かるように)。
        // 終わらなかったときに、次の起動で知らせる。書けなければ始めない
        text.Format("file=%s\nbackup=%s\n", w.name, w.backup);
        FIL f;
        UINT n = 0;
        bool marked = f_open(&f, mark, FA_WRITE | FA_OPEN_APPEND) == FR_OK;
        if (marked) {
            marked = f_write(&f, (const char*)text, text.GetLength(), &n) == FR_OK && n == text.GetLength() && f_sync(&f) == FR_OK;
            if (f_close(&f) != FR_OK) marked = false;
        }
        if (!marked) {
            msg.add("cannot write to the boot drive; not written");
            UnmarkUfdWrite(mark, markWas);
            cancelFloppyWrite();
            return false;
        }
        m_Logger.Write(FromMachine, LogNotice, "usb-fdd: writing %s (CRC-32 %08x); the old contents are in %s", w.name,
                       (unsigned)w.crc, w.backup);
        platform::UfiGate::confirm(true); // 設定メニューで「Yes」を選んだ
        ok = w.restore.write(m_pUfdBuf, &progress);
        platform::UfiGate::confirm(false);
        ok = ok && w.restore.verify(&progress);
        msg.clear();
        if (ok || !w.restore.touched()) UnmarkUfdWrite(mark, markWas);
    }
    const unsigned c = w.restore.track() / 2, h = w.restore.track() % 2;
    const platform::UfiDisk& d = w.restore.disk();
    if (ok) {
        m_Logger.Write(FromMachine, LogNotice, "usb-fdd: %s written and verified", w.name);
        platform::floppy_write_text::done(msg, markWas != 0);
    } else {
        m_Logger.Write(FromMachine, LogWarning, "usb-fdd: error %d, %s (sense %x/%02x/%02x), cylinder %u, head %u, sector %u; %s; backup %s",
                       (int)w.restore.error(), platform::floppy_write_text::ufiErrorName(d.error()), d.senseKey(), d.asc(), d.ascq(), c, h,
                       (unsigned)w.restore.block() + 1, w.restore.touched() ? "THE DISK IS INCOMPLETE" : "not written", w.backup);
        platform::floppy_write_text::writeError(msg, w.restore.error(), d.error(), w.restore.track(), w.restore.touched(),
                                                w.restore.partial(), w.backup);
    }
    cancelFloppyWrite();
    return ok;
}

// 書き込みを許していること (experimental) と、前の書き込みが終わっていないことを、起動のあとで知らせる
void CKernel::WarnUfdWrite() {
    if (platform::UfiGate::enabledUnit() >= 0) {
        static const char kEnabled[] = "experimental:FDへの書き込みが有効です";
        netMessage(kEnabled, sizeof kEnabled - 1, 15);
    }
    if (m_BootDrive == nullptr) return;
    CString mark;
    mark.Format("%s%s", m_BootDrive, kUfdMarkName);
    FILINFO fi;
    if (f_stat(mark, &fi) != FR_OK) return;
    m_Logger.Write(FromMachine, LogWarning, "usb-fdd: the last write did not finish; see %s for the backup", (const char*)mark);
    static const char kText[] = "FDへの書き込みが終わっていません";
    netMessage(kText, sizeof kText - 1, 15);
}
