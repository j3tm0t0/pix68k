#!/bin/sh
# 実物のフロッピーディスクに書かないための仕組み (#286) が外れていないことを確かめる。
#   使い方: tests/tools/real_floppy_guard_test.sh <リポジトリの場所>
#   1. Circle の USB-FDD のドライバの書き換え (tools/circle_ufd_patch.sh): 書き換えたあとの Write() / TryWrite() / Reset() が
#      何も送らずに戻ること、Command() が決まった 5 つのコマンドだけを通すこと。ベアメタル版は ctest でビルドできないので、
#      書き換えた結果の文字列を見る (submodule の external/circle がなければ、ここは飛ばす)
#   2. 書き換える行がなくなったとき、関数の "{" が名前と同じ行に移ったときに、スクリプトが失敗すること
#      (気づかずに、次の関数に行を足したままビルドしない)
#   3. ホスト版 (host/main.cpp) が、実物のドライブから読んだディスクを書き込み禁止で入れ、書き戻さないこと (ソースを見る)
set -u
ROOT=$1
fail=0
ok() { echo "ok: $1"; }
ng() { echo "NG: $1"; fail=1; }

# $1 のファイルの、$2 で始まる行から、次の "}" だけの行までを出す
body() {
  START=$2 awk '
    !on && index($0, ENVIRON["START"]) == 1 { on = 1 }
    on { print }
    on && $0 == "}" { exit }' "$1"
}
# $1: 説明、$2: ファイル、$3: 関数の始まり、$4: 中にあるはずの文字列
check() {
  if body "$2" "$3" | grep -qF -- "$4"; then ok "$1"; else ng "$1: $2 の $3 に '$4' がない"; fi
}

MAIN=$ROOT/host/main.cpp
check 'ホスト版: ブロックデバイスは実物のドライブとして読む' "$MAIN" 'bool loadDisk(Disk& d, const char* path, bool readOnly) {' \
  'd.real = host::isBlockDevice(path);'
check 'ホスト版: 実物のドライブから読んだディスクは書き込み禁止で入れる' "$MAIN" 'bool loadDisk(Disk& d, const char* path, bool readOnly) {' \
  'readOnly = true;'
check 'ホスト版: 実物のドライブから読んだディスクは書き戻さない' "$MAIN" 'void unloadDisk(x68k::Machine& m, int drive, Disk& d, bool writeBack) {' \
  'if (d.image.dirty() && !d.real) {'
check 'ホスト版: デバイスは読み取り専用で開く' "$ROOT/host/real_floppy_dev.cpp" 'bool BlockDeviceFloppy::open(const char* path) {' \
  'fd_ = ::open(path, O_RDONLY | O_CLOEXEC);'
RUN=$ROOT/baremetal/machine_run.cpp
check 'ベアメタル版: 読んだイメージは書き込み禁止で入れる' "$RUN" 'bool CKernel::LoadRealFloppy(int drive, int unit) {' \
  'image->load(data, Reader::IMAGE_BYTES, true)'
if body "$RUN" 'bool CKernel::LoadRealFloppy(int drive, int unit) {' | grep -qE 'addFloppy|->Write *\('; then
  ng 'ベアメタル版: LoadRealFloppy() が書き戻しの対象に入れているか、ドライブに書いている'
else
  ok 'ベアメタル版: LoadRealFloppy() は書き戻しの対象に入れず、ドライブに書かない'
fi

# 転送を待つ間にウォッチドッグを延ばす関数は、USB の装置を探す前に登録し、外さない (USB-FDD の Configure() は
# fd0= / fd1= に書いていなくても呼ばれるので、読む間だけの登録では足りない)
check 'ベアメタル版: 待ちの間の関数を、USB を探す前に登録する' "$ROOT/baremetal/kernel.cpp" 'void CKernel::ScanUsb(bool serviceMachine) {' \
  'g_pPix68kXHCIWaitHook = &CKernel::RealFloppyWaitHook;'
if grep -h 'g_pPix68kXHCIWaitHook *=' "$ROOT"/baremetal/*.cpp | grep -v '&CKernel::RealFloppyWaitHook;' | grep -q .; then
  ng 'ベアメタル版: 待ちの間の関数の登録を外しているところがある'
else
  ok 'ベアメタル版: 待ちの間の関数の登録を外さない'
fi
check 'ベアメタル版: 待ちの間の関数がウォッチドッグを延ばす' "$RUN" 'void CKernel::RealFloppyWaitHook() {' 's_pThis->KeepAlive();'

# 読んだディスクの書き出しと HTTP (#287)
check '書き出し: 一時ファイルは、ないときだけ作る (FA_CREATE_NEW)' "$RUN" 'struct CFloppySaveFs : CFatOps {' \
  'bool openNew() { return note(f_open(&f, path, FA_WRITE | FA_CREATE_NEW)); }'
check '書き出し: USB メモリの空きを確かめてから書く' "$RUN" 'struct CFloppySaveFs : CFatOps {' \
  'if (!CKernel::CanWriteNewFile(path, size)) {'
check '書き出し: 書く手順は platform::writeNewFile()' "$RUN" 'struct CFloppySaveFs : CFatOps {' \
  'platform::writeNewFile(file, data, size);'
if body "$RUN" 'bool CKernel::saveFloppyImage(' | grep -qE 'WriteFileReplacing|replaceFile|f_unlink|FA_CREATE_ALWAYS'; then
  ng '書き出し: saveFloppyImage() が、あるファイルを置き換える関数を使っている'
else
  ok '書き出し: saveFloppyImage() は、あるファイルを置き換える関数を使わない'
fi
check '書き出し: 手順は platform::saveFloppyImageFile()' "$RUN" 'bool CKernel::saveFloppyImage(' 'platform::saveFloppyImageFile(fs, path, tmp, m_pDiskData[drive], size, crc, &tmpLeft);'
NET=$ROOT/baremetal/host_net.cpp
check 'HTTP: /fd<N>.xdf は、ファイルを読むのと同じ扱い (トークン)' "$NET" 'void CHttpTask::Handle(' ': files || fdImage >= 0 ? platform::NetOp::READ_FILES'
check 'HTTP: 答え方は platform::floppyImageReply() で決める' "$NET" 'void CHttpTask::GetFloppyImage(' 'platform::floppyImageReply(bytes, head,'
# バッファを確保するのは、答え方が ALLOCATE に決まったあとだけ
alloc=$(body "$NET" 'void CHttpTask::GetFloppyImage(' | awk '/case Reply::ALLOCATE:/ { a = NR } /new u8\[/ { n++; where = NR } END { print (n == 1 && a && where == a + 1) ? "ok" : "ng" }')
if [ "$alloc" = ok ]; then ok 'HTTP: バッファの確保は ALLOCATE のときだけ'; else ng 'HTTP: バッファの確保が ALLOCATE の外にある'; fi
check 'HTTP: 送るのは写し' "$NET" 'void CHttpTask::GetFloppyImage(' 'SendAll(m_pFloppy + off,'

C=$ROOT/external/circle
CPP=$C/lib/usb/usbfloppydevice.cpp
H=$C/include/circle/usb/usbfloppydevice.h
X=$C/lib/usb/xhciendpoint.cpp
if [ ! -f "$CPP" ]; then
  echo "skip: external/circle がない (git submodule update --init で取る)"
  [ $fail -eq 0 ] && echo "all passed"
  exit $fail
fi
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
PATCH=$ROOT/tools/circle_ufd_patch.sh
if sh "$PATCH" "$CPP" "$T/o.cpp" "$H" "$T/o.h" "$X" "$T/x.cpp" 2>"$T/err"; then
  ok 'Circle の今の版に書き換えが当たる'
else
  ng "Circle の今の版に書き換えが当たらない: $(cat "$T/err")"
fi
# 関数の最初の文 (空行と注釈を除く) が $3 であること
first() {
  if body "$T/o.cpp" "$2" | awk 'NR > 2 && $0 !~ /^[[:space:]]*$/ { print; exit }' | grep -qF -- "$3"; then
    ok "$1"
  else
    ng "$1: $2 の最初の文が '$3' でない"
  fi
}
first 'Write() は何も送らずに -1 を返す' 'int CUSBFloppyDiskDevice::Write (' 'return -1;'
first 'TryWrite() は何も送らずに -1 を返す' 'int CUSBFloppyDiskDevice::TryWrite (' 'return -1;'
first 'Reset() は SEND DIAGNOSTIC を送らずに 0 を返す' 'int CUSBFloppyDiskDevice::Reset (' 'return 0;'
# Command() は、入口で 5 つのコマンド以外とデータアウトを断ってから、装置に送る (ControlMessage)
CMD=$(body "$T/o.cpp" 'int CUSBFloppyDiskDevice::Command (')
allow=$(printf '%s\n' "$CMD" | awk '/switch \(\*\(const u8 \*\) pCmdBlk\)/ { on = 1 } on && /^\tcase / { print $2 } on && /default:/ { exit }' | sort | tr '\n' ' ')
want='SCSI_OP_INQUIRY: SCSI_OP_READ: SCSI_OP_READ_CAPACITY: SCSI_OP_TEST_UNIT_READY: SCSI_REQUEST_SENSE: '
if [ "$allow" = "$want" ]; then ok 'Command() が通すのは 5 つのコマンドだけ'; else ng "Command() が通すコマンド: $allow"; fi
order=$(printf '%s\n' "$CMD" | awk '/default:/ { d = NR } /if \(nBufLen > 0 && !bIn\)/ { o = NR } /ControlMessage/ && !c { c = NR } END { print (d && o && c && d < c && o < c) ? "ok" : "ng" }')
if [ "$order" = ok ]; then ok 'Command() は、断ってから装置に送る'; else ng 'Command() の制限が、装置に送る前にない'; fi
# 書き込みのコマンドの番号 (0x2A) を作るのは TryWrite() の中だけ
if [ "$(grep -c 'SCSI_OP_WRITE;' "$T/o.cpp")" = 1 ] && body "$T/o.cpp" 'int CUSBFloppyDiskDevice::TryWrite (' | grep -q 'SCSI_OP_WRITE;'; then
  ok 'WRITE(10) を作るのは TryWrite() の中だけ'
else
  ng 'WRITE(10) を TryWrite() の外で作っている'
fi
grep -q 'bPix68kLong ? 30\*HZ : 3\*HZ' "$T/x.cpp" && ok 'xHCI: 30 秒まで待つのは USB-FDD への転送だけ' || ng 'xHCI の時間切れの書き換えがない'

# 書き換えが当たらない形の入力では失敗すること。$1: 説明、$2: usbfloppydevice.cpp に掛ける sed の式
broken() {
  sed -e "$2" "$CPP" > "$T/in.cpp"
  if cmp -s "$T/in.cpp" "$CPP"; then ng "$1: 試験の入力が元と同じ (sed の式が当たっていない)"; return; fi
  if sh "$PATCH" "$T/in.cpp" "$T/b.cpp" "$H" "$T/b.h" "$X" "$T/bx.cpp" 2>/dev/null; then
    ng "$1: スクリプトが失敗しない"
  else
    ok "$1: スクリプトが失敗する"
  fi
}
broken 'ブロックの大きさの確かめの行がない' '/if (nBlockSize != BLOCK_SIZE)/d'
broken '割り込みの 2 バイトの確かめの行がない' 's/|| Status\[1\] != STATUS_PASS)/|| Status[1] != 0)/'
# 関数の "{" が名前と同じ行にある (次の行は "{" ではない)
for fn in 'boolean CUSBFloppyDiskDevice::Configure (void)' 'int CUSBFloppyDiskDevice::Read (void \*pBuffer, size_t nCount)' \
          'int CUSBFloppyDiskDevice::Write (const void \*pBuffer, size_t nCount)' \
          'int CUSBFloppyDiskDevice::TryWrite (const void \*pBuffer, size_t nCount)' 'int CUSBFloppyDiskDevice::Reset (void)'; do
  broken "$(printf '%s' "$fn" | sed 's/ (.*//; s/.*:://')() の { が名前と同じ行にある" "/^$fn\$/{N;s/\\n{/ {/;}"
done
broken 'Command() の { が引数と同じ行にある' '/^\t\t\t\t   void \*pBuffer, size_t nBufLen, boolean bIn)$/{N;s/\n{/ {/;}'
broken 'Write() がない' 's/^int CUSBFloppyDiskDevice::Write (/int CUSBFloppyDiskDevice::Write2 (/'
# xHCI の時間切れの行がない
sed -e 's/>= 3\*HZ)/>= 4*HZ)/' "$X" > "$T/inx.cpp"
if sh "$PATCH" "$CPP" "$T/b.cpp" "$H" "$T/b.h" "$T/inx.cpp" "$T/bx.cpp" 2>/dev/null; then
  ng 'xHCI の時間切れの行がない: スクリプトが失敗しない'
else
  ok 'xHCI の時間切れの行がない: スクリプトが失敗する'
fi
[ $fail -eq 0 ] && echo "all passed"
exit $fail
