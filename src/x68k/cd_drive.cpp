#include "x68k/cd_drive.h"

namespace x68k {

void CdDrive::setMedium(HardDiskImage* image, const CdToc* toc, bool changed) {
    stop();
    pos_ = 0;
    image_ = image;
    trayOpen_ = false;
    attention_ = image != nullptr && changed;
    toc_.count = 0;
    toc_.leadOut = 0;
    if (!image) return;
    if (!toc) {
        // イメージの全体がデータトラック 1 つ (ISO)
        CdTrack& t = toc_.tracks[0];
        t = CdTrack();
        t.sector = (uint16_t)image->blockSize();
        t.fileEnd = image->blocks();
        toc_.count = 1;
        toc_.leadOut = image->blocks();
        return;
    }
    toc_ = *toc;
    if (toc_.count > CdToc::MAX_TRACKS) toc_.count = CdToc::MAX_TRACKS;
    // イメージの外を指すトラックは、入っている分だけにする (その後ろは無音、データなら読めない)
    const uint64_t bytes = image->bytes();
    for (int i = 0; i < toc_.count; i++) {
        CdTrack& t = toc_.tracks[i];
        if (t.sector == 0) t.sector = 2048;
        const uint64_t have = t.offset < bytes ? (bytes - t.offset) / t.sector : 0;
        if (t.fileEnd < t.fileLba) t.fileEnd = t.fileLba;
        if (t.fileEnd - t.fileLba > have) t.fileEnd = t.fileLba + (uint32_t)have;
    }
}

void CdDrive::setTray(bool open) {
    if (open == trayOpen_) return;
    stop();
    trayOpen_ = open;
    if (!open && image_) attention_ = true;
}

void CdDrive::reset() {
    stop();
    prevent_ = false;
    trayOpen_ = false;
    attention_ = false; // 入れ替えたあとにリセットしたら、起動の最初のコマンドに UNIT ATTENTION を返さない
}

bool CdDrive::play(uint32_t lba, uint32_t end) {
    const int t = toc_.find(lba);
    if (!ready() || t < 0 || !toc_.tracks[t].audio()) return false;
    stop();
    pos_ = lba;
    end_ = end < toc_.leadOut ? end : toc_.leadOut;
    audio_ = pos_ < end_ ? AUDIO_PLAY : AUDIO_DONE;
    return true;
}

void CdDrive::stop() {
    audio_ = AUDIO_NONE;
    deferKey_ = deferAsc_ = 0;
    frame_ = acc_ = owed_ = 0;
    cursor_.stop();
    runLba_ = runEnd_ = 0;
}

bool CdDrive::pause(bool resume) {
    if (audio_ != AUDIO_PLAY && audio_ != AUDIO_PAUSED) return false;
    audio_ = resume ? AUDIO_PLAY : AUDIO_PAUSED;
    if (!resume) acc_ = owed_ = 0;
    return true;
}

// 今の位置から続けて読める範囲を決める。オーディオのトラックでなければ false
bool CdDrive::openRun() {
    cursor_.stop();
    const int ti = toc_.find(pos_);
    if (ti < 0 || !image_) return false;
    const CdTrack& t = toc_.tracks[ti];
    if (!t.audio() || t.sector != FRAMES_PER_SECTOR * 4) return false;
    runLba_ = pos_;
    runSwap_ = t.swap;
    if (pos_ < t.fileLba) { // プリギャップ (イメージにない)
        runSilent_ = true;
        runEnd_ = t.fileLba;
    } else if (pos_ < t.fileEnd) {
        runSilent_ = false;
        runEnd_ = t.fileEnd;
        cursor_.start(image_, t.offset + (uint64_t)(pos_ - t.fileLba) * t.sector, (runEnd_ - runLba_) * (uint32_t)t.sector);
    } else { // トラックの終わりまでイメージにない
        runSilent_ = true;
        runEnd_ = toc_.end(ti);
    }
    if (runEnd_ > toc_.end(ti)) runEnd_ = toc_.end(ti);
    return runEnd_ > runLba_;
}

int CdDrive::produce(uint32_t clocks, int16_t* out, int max) {
    if (audio_ != AUDIO_PLAY) return 0;
    // 4MHz のクロック 40000 個が 441 フレーム (44.1kHz)
    uint64_t acc = (uint64_t)clocks * 441 + acc_;
    owed_ += (uint32_t)(acc / 40000);
    acc_ = (uint32_t)(acc % 40000);
    int n = 0;
    while (owed_ && n < max) {
        if (pos_ >= end_) {
            stop();
            audio_ = AUDIO_DONE;
            break;
        }
        if (pos_ < runLba_ || pos_ >= runEnd_ || (!runSilent_ && !cursor_.active())) {
            if (!openRun()) {
                // データトラックに入った: BLANK CHECK、END OF USER AREA ENCOUNTERED ON THIS TRACK (SCSI-2 の 14.1.7)
                fail(0x08, 0x63);
                break;
            }
        }
        int16_t l = 0, r = 0;
        if (!runSilent_) {
            const uint32_t p = (pos_ - runLba_) * (FRAMES_PER_SECTOR * 4) + frame_ * 4;
            uint8_t b[4];
            int i = 0;
            // チャンクの境目をまたぐことがあるので、1 バイトずつ確かめて写す
            for (; i < 4 && cursor_.ready(p + (uint32_t)i); i++) b[i] = cursor_.at(p + (uint32_t)i);
            if (i < 4) {
                if (cursor_.failed()) { // イメージから読めなかった: MEDIUM ERROR、UNRECOVERED READ ERROR
                    fail(0x03, 0x11);
                } else {
                    owed_ = 0; // 読み込みを待つ (位置は進めない)
                }
                break;
            }
            if (runSwap_) {
                l = (int16_t)(uint16_t)(b[0] << 8 | b[1]);
                r = (int16_t)(uint16_t)(b[2] << 8 | b[3]);
            } else {
                l = (int16_t)(uint16_t)(b[1] << 8 | b[0]);
                r = (int16_t)(uint16_t)(b[3] << 8 | b[2]);
            }
        }
        out[2 * n] = l;
        out[2 * n + 1] = r;
        n++;
        owed_--;
        if (++frame_ >= FRAMES_PER_SECTOR) {
            frame_ = 0;
            pos_++;
        }
    }
    return n;
}

} // namespace x68k
