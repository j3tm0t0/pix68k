#!/bin/sh
# ベアメタル版 (baremetal/) が、USB メモリを boot ドライブにしたときの制限 (src/platform/boot_drive.h。中身は ctest の
# boot_drive_test で確かめる) を、決まった入口から呼んでいることを確かめる。ベアメタル版は ctest でビルドできないので、
# ソースの関数の中に呼び出しの行があるかを見る (行を消すと失敗する)。
#   使い方: tests/tools/usb_guard_test.sh <リポジトリの場所>
set -u
ROOT=$1
fail=0

# $1 のファイルの、$2 (正規表現ではなく、そのままの文字列) で始まる行から、$3 と同じ行 (関数を閉じる行) までを出す。
# 始まりの行が " }" で終わる 1 行の関数なら、その行だけ
body() {
  START=$2 END=$3 awk '
    !on && index($0, ENVIRON["START"]) == 1 { on = 1; if ($0 ~ / }$/) { print; exit } }
    on { print }
    on && $0 == ENVIRON["END"] { exit }' "$ROOT/$1"
}

# $1: 説明、$2: ファイル、$3: 関数の始まり、$4: 関数の終わり、$5: 中にあるはずの文字列
check() {
  if body "$2" "$3" "$4" | grep -qF -- "$5"; then
    echo "ok: $1"
  else
    echo "NG: $1: $2 の $3 に '$5' がない"
    fail=1
  fi
}

TOP='}'
check 'イメージの書き込み禁止の判定が /pix68k の外を見る' baremetal/machine_run.cpp \
  'bool CKernel::DiskReadOnly(' "$TOP" '!UsbPathWritable(path)'
check 'UsbPathWritable() が platform の判定を呼ぶ' baremetal/kernel.cpp \
  'bool CKernel::UsbPathWritable(' "$TOP" 'platform::usbPathWritable(path + dn)'
check 'UsbPathWritable() は USB メモリのときだけ断る' baremetal/kernel.cpp \
  'bool CKernel::UsbPathWritable(' "$TOP" 'if (!m_BootOnUsb) return true;'
check 'DELETE が場所を確かめる' baremetal/host_net.cpp \
  'void CHttpTask::DeleteFile(' "$TOP" 'if (!m_Env.netCanWrite(path, -1, why)) {'
check 'PUT が場所を確かめる' baremetal/host_net.cpp \
  'void CHttpTask::PutFile(' "$TOP" 'if (!m_Env.netCanWrite(path, -1, refused)) {'
check 'PUT が空きを確かめて、受け取る大きさを覚える' baremetal/host_net.cpp \
  'void CHttpTask::PutFile(' "$TOP" 'if (!m_Env.netCanWrite(path, m_Req.contentLength, refused)) {'
check 'PUT が、受け取る大きさとクラスタの大きさから残りを数える' baremetal/host_net.cpp \
  'void CHttpTask::PutFile(' "$TOP" \
  'planned{m_Env, platform::UploadPlan((unsigned long long)m_Req.contentLength, m_Env.netClusterBytes())};'
check 'PUT が書いて減った分を返す' baremetal/host_net.cpp \
  'void CHttpTask::PutFile(' "$TOP" 'm_Env.netWriteDone(planned.plan.wrote(fill));'
check 'PUT が出るときに残りを返す' baremetal/host_net.cpp \
  'void CHttpTask::PutFile(' "$TOP" '~Planned() { env.netWriteDone(plan.finish()); }'
check 'netCanWrite() が platform の判定と予約を呼ぶ' baremetal/kernel.cpp \
  'bool CKernel::netCanWrite(' "$TOP" \
  'switch (platform::beginNetWrite(m_UsbBudget, m_BootOnUsb, path, bytes, known, freeBytes, kUsbDriveReserve,'
check 'netCanWrite() が空きを読む' baremetal/kernel.cpp \
  'bool CKernel::netCanWrite(' "$TOP" 'const bool known = !m_BootOnUsb || bytes < 0 || UsbFreeBytes(freeBytes);'
check 'netWriteDone() が残りを減らす' baremetal/kernel.cpp \
  'void CKernel::netWriteDone(' "$TOP" 'm_UsbBudget.end(bytes);'
check 'CheckUsbWrite() が platform の判定を呼ぶ' baremetal/kernel.cpp \
  'platform::UsbWriteCheck CKernel::CheckUsbWrite(' "$TOP" \
  'return platform::checkUsbWrite(true, pathInDrive, kind, bytes, known, freeBytes, kUsbDriveReserve,'
check 'CheckUsbWrite() が受け取りの途中の残りとクラスタの大きさを渡す' baremetal/kernel.cpp \
  'platform::UsbWriteCheck CKernel::CheckUsbWrite(' "$TOP" '                                   m_UsbBudget.pending, netClusterBytes());'
check 'CheckUsbWrite() は USB メモリのときだけ調べる' baremetal/kernel.cpp \
  'platform::UsbWriteCheck CKernel::CheckUsbWrite(' "$TOP" 'if (!m_BootOnUsb) return platform::UsbWriteCheck::OK;'
check 'CanWriteNewFile() が CheckUsbWrite() を呼ぶ' baremetal/kernel.cpp \
  'bool CKernel::CanWriteNewFile(' "$TOP" 'return strncmp(path, k->m_BootDrive, dn) == 0 &&'
check 'CanWriteNewFile() が新しいファイルとして判定する' baremetal/kernel.cpp \
  'bool CKernel::CanWriteNewFile(' "$TOP" 'k->CheckUsbWrite(path + dn, platform::UsbWriteKind::NEW_FILE, size) == platform::UsbWriteCheck::OK;'
check '新しいファイルの書き込み (ステート、pix68k.cfg、フロッピーの .new) が空きを確かめる' baremetal/machine_run.cpp \
  '    platform::FsResult write(const char* path, const void* data, unsigned size) {' '    }' \
  'if (!CKernel::CanWriteNewFile(path, size)) {'
check 'SRAM の書き込みが空きを確かめる' baremetal/machine_run.cpp \
  'bool CKernel::SaveSram(' "$TOP" 'CanWriteNewFile(path, x68k::SramSlots::FILE_SIZE) ?'
check 'USB の転送の中の待ち時間が、転送の途中だと伝える' baremetal/kernel.cpp \
  'void CKernel::UsbScanDelay(' "$TOP" 'k->ServiceDuringUsbScan(true);'
check 'ServiceDuringUsbScan() が読み書きしてよいかを platform の判定で決める' baremetal/machine_run.cpp \
  'void CKernel::ServiceDuringUsbScan(' "$TOP" 'ServiceDisks(now, platform::diskIoAllowed(m_BootOnUsb, insideUsbTransfer));'
check 'ServiceDisks() が platform の serviceDiskSync() を呼ぶ' baremetal/machine_run.cpp \
  'unsigned CKernel::ServiceDisks(' "$TOP" 'return platform::serviceDiskSync(m_pDiskWriter->sync, now / (CLOCKHZ / 1000), io, m_MenuActive,'
check 'ServiceDisks() が、メニューを開いて X68000 を止めているかを渡す' baremetal/machine_run.cpp \
  'unsigned CKernel::ServiceDisks(' "$TOP" '                                     m_Menu.isOpen() && m_pEmulator->paused());'
check 'deploy のログが空きを確かめる' baremetal/kernel.cpp \
  'void CKernel::FinishDeploy(' "$TOP" 'if (!CanWriteNewFile(path, LOGGER_BUFSIZE)) {'
check '/pix68k のディレクトリを作る前に空きを確かめる' baremetal/kernel.cpp \
  'void CKernel::MakeBootDir(' "$TOP" 'if (CheckUsbWrite("/pix68k/x", platform::UsbWriteKind::NEW_DIR, 0) != platform::UsbWriteCheck::OK) return;'
check '途中で止まった置き換えを戻す名前の変更の前に空きを確かめる' baremetal/machine_run.cpp \
  '    bool canRename(const char*, const char* to)' '    }' 'return CKernel::CanRenameFile(to);'
check 'CanRenameFile() が名前の変更として判定する' baremetal/kernel.cpp \
  'bool CKernel::CanRenameFile(' "$TOP" \
  'k->CheckUsbWrite(path + dn, platform::UsbWriteKind::RENAME, 0) == platform::UsbWriteCheck::OK;'
check 'replaceFile() が、戻す名前の変更の前に聞く' src/platform/file_replace.h \
  'FsResult replaceFile(' "$TOP" 'if (!canRecoverRename(fs, tmp, path, 0)) return FsResult::ERROR;'
# USB メモリが 2 本以上のとき (#116): 空きとクラスタの大きさは選んだ USB メモリのものを読み、選ばなかったものは外す
check 'UsbFreeBytes() が、選んだ boot ドライブの空きを読む' baremetal/kernel.cpp \
  'bool CKernel::UsbFreeBytes(' "$TOP" \
  'if (m_BootDrive == nullptr || f_getfree(m_BootDrive, &clusters, &fs) != FR_OK || fs == nullptr) return false;'
check 'netClusterBytes() が、選んだ USB メモリのクラスタの大きさを返す' baremetal/kernel.cpp \
  'unsigned long long CKernel::netClusterBytes(' "$TOP" \
  'return m_BootUsbFs != nullptr ? (unsigned long long)m_BootUsbFs->csize * FF_MIN_SS : 1;'
check 'SelectBootDrive() が、選んだ USB メモリの FATFS を覚える' baremetal/kernel.cpp \
  'void CKernel::SelectBootDrive(' "$TOP" \
  'm_BootUsbFs = m_BootOnUsb ? &m_UsbFs[pick - platform::BOOT_USB] : nullptr;'
check 'FinishBootDrive() が、選ばなかった USB メモリをアンマウントする' baremetal/kernel.cpp \
  'void CKernel::FinishBootDrive(' "$TOP" \
  'if (m_Drives[drive].mounted && m_BootUsbFs != &m_UsbFs[k]) f_unmount(kBootDriveNames[drive]);'
check 'FinishBootDrive() が、調べた USB メモリに削除の通知を付ける' baremetal/kernel.cpp \
  'void CKernel::FinishBootDrive(' "$TOP" \
  'removedHandle[k] = dev->RegisterRemovedHandler(UsbDriveRemoved, &m_UsbWatch[k]);'
check '削除の通知が、見張りに抜かれたことを伝える' baremetal/kernel.cpp \
  'static void UsbDriveRemoved(' "$TOP" 'static_cast<platform::UsbSlotWatch*>(context)->gone = true;'
exit $fail
