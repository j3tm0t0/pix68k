// 音の合成 (OPM と ADPCM) を別のスレッドで行ったとき (x68k/sound_replay.h) に、1 つのスレッドで合成したときと
// 同じ音になるかを確かめる。同じ Machine を 2 つ作り、片方は今までどおり、もう片方は音の合成を別のスレッドで行い、
//   Human68k の起動 (900 フレーム)
//   machine_bench の gfx の場面 (OPM の 8 チャネル、4 チャネルは LFO つき。120 フレーム)
//   ADPCM の再生 (8255 で分周と左右を変えながら、$E92003 にデータを書き続ける。約 1 秒)
//   CD-ROM 装置のオーディオの再生 (テスト用に作った PCM を再生し、途中で一時停止と再開をする。約 1 秒)
// を動かして、作った音 (48kHz の 16 ビットのステレオ) の全サンプルのハッシュと数が同じことを調べる。
// 音の量の調整 (setLatency) はしないので、作る音は動かし方によらず決まる。
// ThreadSanitizer でビルドして動かすと、スレッドの間のデータの競合も調べられる。
//   sound_thread_test [-r ROM のディレクトリ] [-0 起動ディスク] [--ring log2] [--frames N] [--no-gfx] [--no-peak-check]
//   --no-peak-check: 音の大きさ (最大値が 1000 以上か) を調べない (音をほとんど出さないディスクで確かめるとき)
// ROM か起動ディスクがなければ 77 (ctest の SKIP_RETURN_CODE)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <thread>

#include "../bench/scene.h"
#include "x68k/machine.h"
#include "x68k/sound_replay.h"

namespace {

uint8_t* loadWhole(const char* path, size_t& size) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = n > 0 ? new uint8_t[(size_t)n] : nullptr;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        delete[] buf;
        buf = nullptr;
    }
    fclose(f);
    size = buf ? (size_t)n : 0;
    return buf;
}

// 取り出した音のハッシュと数、振幅の最大
struct PcmHash {
    uint32_t h = 2166136261u;
    uint64_t frames = 0;
    int peak = 0;
    void add(const int16_t* p, int n) {
        const uint8_t* b = (const uint8_t*)p;
        for (int i = 0; i < n * 4; i++) h = (h ^ b[i]) * 16777619u;
        for (int i = 0; i < n * 2; i++) {
            int v = p[i] < 0 ? -p[i] : p[i];
            if (v > peak) peak = v;
        }
        frames += (uint64_t)n;
    }
    // リングにたまっている分をすべて取り出す
    void drain(x68k::AudioRing& ring) {
        static thread_local int16_t buf[2 * 4096];
        int n;
        while ((n = ring.drain(buf, 4096)) > 0) add(buf, n);
    }
};

struct Setup {
    const uint8_t* ipl;
    size_t iplSize;
    const uint8_t* cg;
    size_t cgSize;
    uint8_t* disk;
    size_t diskSize;
    const char* diskPath;
};

int kBootFrames = 900, kGfxFrames = 120;

// CD-ROM のイメージ: オーディオのトラック 1 つ (150 セクタ = 2 秒)。中身は決まった乱数
const uint32_t kCdSectors = 150;
uint8_t g_cdPcm[kCdSectors * 2352];

void makeCd() {
    uint32_t x = 12345;
    for (uint32_t i = 0; i < sizeof g_cdPcm; i++) {
        x = x * 1664525u + 1013904223u;
        g_cdPcm[i] = (uint8_t)(x >> 24);
    }
}

x68k::Machine* makeMachine(const Setup& s, x68k::FloppyImage& image) {
    x68k::Machine* m = new x68k::Machine();
    uint8_t* copy = new uint8_t[s.diskSize];
    memcpy(copy, s.disk, s.diskSize);
    if (!m->ok() || !m->setIplRom(s.ipl, s.iplSize) || !image.load(copy, s.diskSize, false, s.diskPath)) return nullptr;
    if (s.cg) m->setCgRom(s.cg, s.cgSize);
    m->insertDisk(0, &image);
    m->setTime(1767225600);
    // CD-ROM 装置。SCSI の ROM は渡さないので X68000 からは見えない (起動は変わらない)。再生は CdDrive を直接操作する
    x68k::HardDiskImage* cd = new x68k::HardDiskImage;
    x68k::CdToc* toc = new x68k::CdToc;
    toc->count = 1;
    toc->tracks[0].control = 0;
    toc->tracks[0].sector = 2352;
    toc->tracks[0].fileEnd = kCdSectors;
    toc->leadOut = kCdSectors;
    if (!cd->load(g_cdPcm, sizeof g_cdPcm, 1, true)) return nullptr;
    m->attachScsi(6, cd, true, toc);
    delete toc;
    return m;
}

// 起動、gfx の場面、ADPCM の再生を動かす。afterFrame はフレームごと (ADPCM の再生では 2000 サイクルごと) に呼ぶ
template <typename F>
bool runScenes(x68k::Machine& m, F afterFrame) {
    m.powerOn();
    for (int i = 0; i < kBootFrames; i++) {
        m.runFrame();
        afterFrame();
    }
    if (kGfxFrames == 0) return true;
    if (!scene::startGfx(m)) return false;
    for (int i = 0; i < kGfxFrames; i++) {
        m.runFrame();
        afterFrame();
    }
    // ADPCM: 8255 のポート C ($E9A005) で分周と左右を変えながら、CPU の代わりにデータを書き続ける
    scene::Rng rnd;
    scene::poke8(m, 0xe92001, 0x02); // 再生開始
    for (int i = 0; i < 8000; i++) {
        if (i % 1000 == 0) scene::poke8(m, 0xe9a005, (uint8_t)((i / 1000) & 0x0f));
        if (i == 4000) scene::opm(m, 0x1b, 0x80); // CT2: ADPCM のクロックを 4MHz にする
        scene::poke8(m, 0xe92003, (uint8_t)rnd());
        m.run(2000);
        if (i % 256 == 0) afterFrame();
    }
    scene::poke8(m, 0xe92001, 0x01); // 停止
    for (int i = 0; i < 30; i++) {
        m.runFrame();
        afterFrame();
    }
    // CD-ROM 装置のオーディオ: 再生し、途中で一時停止と再開をして、最後は止める
    x68k::CdDrive& cd = *m.cdDrive();
    if (!cd.play(10, 140)) return false;
    for (int i = 0; i < 70; i++) {
        if (i == 20) cd.pause(false);
        if (i == 26) cd.pause(true);
        if (i == 60) cd.stop();
        m.runFrame();
        afterFrame();
    }
    return true;
}

unsigned quitFlag = 0;

void relax() { std::this_thread::yield(); }

} // namespace

int main(int argc, char** argv) {
    char romDir[1024];
    const char* home = getenv("HOME");
    snprintf(romDir, sizeof romDir, "%s/.cache/pix68k/rom", home ? home : ".");
    char diskPath[1100];
    snprintf(diskPath, sizeof diskPath, "%s/.cache/pix68k/disk/HUMAN302.XDF", home ? home : ".");
    const char* disk = diskPath;
    bool peakCheck = true;
    unsigned ringLog2 = 12;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) snprintf(romDir, sizeof romDir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "-0") && i + 1 < argc) disk = argv[++i];
        else if (!strcmp(argv[i], "--ring") && i + 1 < argc) ringLog2 = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) kBootFrames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-gfx")) kGfxFrames = 0;
        else if (!strcmp(argv[i], "--no-peak-check")) peakCheck = false;
        else {
            fprintf(stderr, "usage: %s [-r romdir] [-0 bootdisk] [--ring log2] [--frames N] [--no-gfx] [--no-peak-check]\n",
                    argv[0]);
            return 2;
        }
    }
    makeCd();
    char path[1100];
    Setup s = {};
    s.diskPath = disk;
    snprintf(path, sizeof path, "%s/IPLROM.DAT", romDir);
    s.ipl = loadWhole(path, s.iplSize);
    snprintf(path, sizeof path, "%s/CGROM.DAT", romDir);
    s.cg = loadWhole(path, s.cgSize);
    s.disk = loadWhole(disk, s.diskSize);
    if (!s.ipl || !s.disk) {
        fprintf(stderr, "skip: cannot read %s/IPLROM.DAT or %s\n", romDir, disk);
        return 77;
    }

    // 1 つのスレッドで音を作る
    static x68k::FloppyImage imageA, imageB;
    x68k::Machine* a = makeMachine(s, imageA);
    if (!a) return 1;
    a->sound().setOutputRate(48000);
    PcmHash ha;
    if (!runScenes(*a, [&] { ha.drain(a->sound().ring()); })) {
        fprintf(stderr, "the interrupt mask never became 0\n");
        return 1;
    }
    ha.drain(a->sound().ring());

    // 音の合成を別のスレッドで行う。音はそのスレッドが取り出す (リングが満杯になって音を捨てないように)
    x68k::Machine* b = makeMachine(s, imageB);
    if (!b) return 1;
    x68k::Sound* synth = new x68k::Sound;
    synth->setOutputRate(48000);
    uint32_t* ring = new uint32_t[1u << ringLog2];
    x68k::VideoJournal journal;
    journal.init(ring, ringLog2);
    journal.setRelax(relax);
    x68k::SoundReplayer replayer(journal, *synth);
    x68k::SoundLink link;
    link.attach(*b, journal);
    PcmHash hb;
    std::thread thread([&] {
        for (;;) {
            unsigned n = replayer.step();
            hb.drain(synth->ring());
            if (n == 0) {
                if (__atomic_load_n(&quitFlag, __ATOMIC_ACQUIRE)) break;
                std::this_thread::yield();
            }
        }
    });
    // ベアメタル版と同じく、フレームの終わりでまとめているクロック数を見せる (区切り方が変わっても音は同じはず)
    bool ok = runScenes(*b, [&] { b->sound().flushJournal(); });
    link.detach(*b); // 合成するスレッドが追いつくまで待つ
    __atomic_store_n(&quitFlag, 1u, __ATOMIC_RELEASE);
    thread.join();
    hb.drain(synth->ring());
    if (!ok) {
        fprintf(stderr, "the interrupt mask never became 0\n");
        return 1;
    }

    printf("single: %llu frames, hash %08x, peak %d; threaded: %llu frames, hash %08x, peak %d; "
           "ring %u entries, %u pushed, full %llu times\n",
           (unsigned long long)ha.frames, ha.h, ha.peak, (unsigned long long)hb.frames, hb.h, hb.peak,
           journal.capacity(), journal.pushed(), (unsigned long long)journal.stats().waits);
    // X68000 の CPU から見える状態が変わっていないこと (音の合成を移しても CPU は同じように動く)
    m68k::Registers ra, rb;
    a->cpu().getRegisters(ra);
    b->cpu().getRegisters(rb);
    int failures = 0;
    if (a->cpu().cycles() != b->cpu().cycles() || memcmp(ra.d, rb.d, sizeof ra.d) || memcmp(ra.a, rb.a, sizeof ra.a) ||
        ra.pc != rb.pc) {
        fprintf(stderr, "FAIL: CPU state differs\n");
        failures++;
    }
    if (ha.frames != hb.frames || ha.h != hb.h) {
        fprintf(stderr, "FAIL: sound differs\n");
        failures++;
    }
    if (peakCheck && ha.peak < 1000) {
        fprintf(stderr, "FAIL: too quiet (peak %d); the scenes did not make sound\n", ha.peak);
        failures++;
    }
    if (failures) return 1;
    printf("OK: identical sound\n");
    return 0;
}
