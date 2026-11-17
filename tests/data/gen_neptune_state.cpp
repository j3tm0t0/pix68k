// tests/data/neptune_v10.state と neptune_v13.state を作ったプログラム。ROM のない空の Machine に Neptune-X をつなぎ、
// レジスタを少し書いて (CURR = $47、SCC のチャネル B で WR12 を選んだまま) ステートを書く。
// 前の実装で作るため、そのコミットの src と libx68k.a / libm68k.a でビルドして動かした:
//   neptune_v10.state: a005875 (版 10。Neptune-X の送信の残りの時間がない)
//   neptune_v13.state: ecc77f3 (main。版 13。SCC は前の並び)
//   c++ -std=c++17 -I<tree>/src gen_neptune_state.cpp <tree>/build/libx68k.a <tree>/build/libm68k.a -o gen && ./gen out.state
// ビルドには入れない (ctest では neptune_test がこの 2 つを読む)
#include <stdio.h>
#include <vector>
#include "x68k/machine.h"
#include "x68k/neptune.h"
#include "x68k/state.h"
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    static const uint8_t mac[6] = {0x00, 0x00, 0xf4, 0x12, 0x34, 0x56};
    x68k::Machine m;
    if (!m.setNeptune(true)) return 3;
    m.neptune()->setMac(mac);
    const int fc = m68k::FC_SUPER_DATA;
    m.read8(0xece23e, fc);        // reset
    m.write8(0xece21c, 0x49, fc); // DCR
    m.write8(0xece21a, 0x04, fc); // TCR
    m.write8(0xece200, 0x62, fc); // page 1, STP? (CR: PS0=1)
    m.write8(0xece20e, 0x47, fc); // CURR = $47 (page 1)
    m.write8(0xece200, 0x22, fc); // page 0, STA
    m.write8(0xece21e, 0x15, fc); // IMR
    // SCC: select WR12 on channel B and leave the pointer there
    m.write8(0xe98001, 12, fc);
    m.write8(0xe98001, 0x0e, fc);
    m.write8(0xe98001, 12, fc);
    std::vector<uint8_t> buf(x68k::stateSize(m) + 4096);
    size_t n = 0;
    if (!x68k::saveState(m, buf.data(), buf.size(), n)) return 4;
    FILE* f = fopen(argv[1], "wb");
    fwrite(buf.data(), 1, n, f);
    fclose(f);
    printf("version %u, %zu bytes, curr %u, isr %u\n", (unsigned)x68k::STATE_VERSION, n, (unsigned)m.neptune()->curr(), (unsigned)m.neptune()->isr());
    return 0;
}
