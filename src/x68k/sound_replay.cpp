#include "x68k/sound_replay.h"

namespace x68k {

void SoundLink::attach(Machine& machine, VideoJournal& journal) {
    journal_ = &journal;
    machine.sound().setJournal(&journal);
    if (machine.mercury()) machine.sound().mercuryAttached(true); // 読み手にも置かせる
    // CD-ROM 装置のオーディオの記録は前のフレームとの差で書くので、読み手 (作ったばかり) と同じところから始める
    if (machine.cdDrive()) machine.cdDrive()->mixer().reset();
}

void SoundLink::detach(Machine& machine) {
    if (!journal_) return;
    machine.sound().flushJournal();
    journal_->flush();
    machine.sound().setJournal(nullptr);
    journal_ = nullptr;
}

unsigned SoundReplayer::step(unsigned max) {
    const uint32_t* p;
    unsigned n = journal_.available(p);
    if (n == 0) return 0;
    if (n > max) n = max;
    MidiOut* const midi = __atomic_load_n(&midi_, __ATOMIC_ACQUIRE);
    for (unsigned i = 0; i < n; i++) synth_.replay(p[i], mercury_, midi, &cd_);
    journal_.consume(n);
    return n;
}

} // namespace x68k
