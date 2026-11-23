// 更新 (#369) で残した前の版に、設定メニューと起動メニューから戻す。戻せるかの判定は platform/update_rollback.h、
// config.txt の書き換えは SwitchConfigKernel() (POST /config/kernel と同じ) だけ
#include <circle/string.h>
#include <circle/util.h>

#include "build_info.h" // tools/build_baremetal.sh が作る
#include "config_switch.h"
#include "kernel.h"
#include "platform/sha256.h"

static const char FromUpdate[] = "update";

namespace {
// path を buf に max バイトまで読む。読めない、大きすぎるなら false
bool ReadSmallFile(const char* path, u8* buf, size_t max, size_t& size) {
    FIL f;
    if (f_open(&f, path, FA_READ | FA_OPEN_EXISTING) != FR_OK) return false;
    const FSIZE_t n = f_size(&f);
    UINT got = 0;
    const bool ok = n <= max && f_read(&f, buf, (UINT)n, &got) == FR_OK && got == n;
    f_close(&f);
    size = got;
    return ok;
}
} // namespace

// 戻せる版があるかを SD カードから読み直す (メニューを開くとき)。書かない
void CKernel::FindRollback() {
    m_HasRollback = false;
    if (m_BootDrive == nullptr || strcmp(m_BootDrive, "SD:") != 0) return;
    if (m_pRollbackBuf == nullptr) m_pRollbackBuf = new u8[platform::CONFIG_TXT_MAX + platform::UPDATE_TXT_MAX + CONFIG_SWITCH_BUF];
    u8* const config = m_pRollbackBuf;
    u8* const update = config + platform::CONFIG_TXT_MAX;
    size_t updateSize = 0, configSize = 0;
    if (!ReadSmallFile("SD:/pix68k/update.txt", update, platform::UPDATE_TXT_MAX, updateSize) ||
        !ReadSmallFile("SD:/config.txt", config, platform::CONFIG_TXT_MAX, configSize))
        return;
    const char* why = platform::planRollback((const char*)config, configSize, (const char*)update, updateSize,
                                             netConfigSections(), (unsigned)RASPPI, PIX68K_VERSION, PIX68K_COMMIT, m_Rollback);
    if (why != nullptr) return; // 更新していない (update.txt がない) のと同じく、項目を出さない
    m_HasRollback = true;
}

// 前の版に戻すように config.txt を書き換える。再起動は呼ぶ側
bool CKernel::RollBack(platform::MenuText& msg, bool cancelChoice) {
    FindRollback(); // メニューを開いてから変わっていないか
    if (!m_HasRollback) {
        msg.add("no previous version");
        return false;
    }
    // ファイルが、更新したときの前の版と同じか
    CString path;
    path.Format("SD:/%s", m_Rollback.to);
    char hex[65] = "";
    FIL f;
    const FRESULT opened = f_open(&f, path, FA_READ | FA_OPEN_EXISTING);
    if (opened == FR_OK) {
        platform::Sha256 hash;
        const FSIZE_t size = f_size(&f);
        FSIZE_t done = 0;
        while (done < size) {
            UINT got = 0;
            if (f_read(&f, m_pRollbackBuf, platform::CONFIG_TXT_MAX, &got) != FR_OK || got == 0) break;
            hash.update(m_pRollbackBuf, got);
            done += got;
            KeepAlive();
        }
        f_close(&f);
        if (size > 0 && done == size) hash.finishHex(hex);
    }
    if (hex[0] == 0) {
        const bool missing = opened == FR_NO_FILE || opened == FR_NO_PATH;
        m_Logger.Write(FromUpdate, LogWarning, "%s: %s; not written", (const char*)path, missing ? "not found" : "cannot read");
        msg.add(missing ? "no " : "cannot read ").add(m_Rollback.to).add("; not rolled back");
        return false;
    }
    if (strcmp(hex, m_Rollback.sha256) != 0) {
        m_Logger.Write(FromUpdate, LogWarning, "%s is not the previous version; not written", (const char*)path);
        msg.add(m_Rollback.to).add(" has changed; not rolled back");
        return false;
    }
    // 断った POST /reboot?profile= の取り消しは、戻せると分かってから、config.txt を替える前に済ませる (取り消せなければ再起動できない)
    if (cancelChoice && !CancelProfileChoiceForRestart("menu Roll back")) {
        msg.add("cannot cancel /reboot request; not rolled back");
        return false;
    }
    int status = 0;
    const char* error = nullptr;
    u8* const buf = m_pRollbackBuf + platform::CONFIG_TXT_MAX + platform::UPDATE_TXT_MAX;
    if (!SwitchConfigKernel("SD:", netConfigSections(), m_Rollback.from, m_Rollback.to, buf, status, error)) {
        m_Logger.Write(FromUpdate, LogWarning, "config.txt kernel=%s -> %s: %s", m_Rollback.from, m_Rollback.to, error);
        msg.add(error);
        return false;
    }
    m_Logger.Write(FromUpdate, LogNotice, "config.txt: kernel=%s -> %s (rolled back to %s)", m_Rollback.from, m_Rollback.to,
                   m_Rollback.version);
    m_HasRollback = false;
    msg.add("rolled back to ").add(m_Rollback.version);
    return true;
}

// 設定メニューの「Roll back」。X68000 は止まっている
bool CKernel::rollBack(platform::MenuText& msg) {
    // Restart と同じく、書き戻せなければ 1 回は断り、続けてもう一度選んだら、書き戻さずに進む
    if (!FlushDisks()) {
        if (!m_Discard.confirmed()) {
            msg.add("cannot write back; Roll back again to discard");
            m_Discard.arm(platform::MENU_ACTION_ROLLBACK);
            return false;
        }
        m_Logger.Write(FromUpdate, LogWarning, "rolling back without writing back disks");
    }
    return RollBack(msg, true);
}
