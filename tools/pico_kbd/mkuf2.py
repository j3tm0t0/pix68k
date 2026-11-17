#!/usr/bin/env python3
"""いくつかの bin を、番地を決めて 1 つの UF2 にまとめる (RP2040 のブートローダとファームウェアを 1 回で書くため)。

使い方: mkuf2.py 出力.uf2 番地:ファイル.bin [番地:ファイル.bin ...] [番地:erase:バイト数 ...]
  例: mkuf2.py pico_w_first.uf2 0x10000000:boot2040.bin 0x10010000:pico_kbd.bin 0x101f0000:erase:8192
「番地:erase:バイト数」は、その範囲を消した状態 (0xff) にする (前の更新の記録を、書き直すときに消すため)。
ファミリーは RP2040 (0xe48bff56)。番地は 4096 の倍数、erase のバイト数も 4096 の倍数。
bin は 4096 の倍数まで 0xff で埋める。ファイルの中で、どの 4KB の 16 ブロックも、16 の倍数の番目から続けて並ぶようにするため。
(ずれていると、ドライブ (RPI-RP2) に写したとき、4KB ごとの先頭のいくつかのブロックが書かれずに消えた。
 picotool load では起きない。29 ブロックのブートローダのあとにファームウェアを置いたら、ファームウェアの 4KB ごとの先頭 3 ブロックが消えた)
"""
import struct
import sys

RP2040_FAMILY = 0xE48BFF56


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    chunks = []
    for arg in sys.argv[2:]:
        addr, path = arg.split(":", 1)
        addr = int(addr, 0)
        if addr % 4096:
            sys.exit("mkuf2.py: 番地は 4096 の倍数にする")
        if path.startswith("erase:"):
            size = int(path[6:], 0)
            # ブート ROM は 4KB ごとに消してから書くので、4KB の境目に合わせる (合っていないと、同じ 4KB の残りも消える)
            if size <= 0 or size % 4096:
                sys.exit("mkuf2.py: 消す範囲のバイト数は 4096 の倍数にする")
            data = b"\xff" * size
        else:
            with open(path, "rb") as f:
                data = f.read()
        data += b"\xff" * (-len(data) % 4096)  # 4KB の終わりまで埋める (上の説明)
        chunks += [(addr + i, data[i:i + 256]) for i in range(0, len(data), 256)]
    addrs = [a for a, _ in chunks]
    if len(set(addrs)) != len(addrs):
        sys.exit("mkuf2.py: 番地が重なっている")
    with open(sys.argv[1], "wb") as f:
        for no, (addr, data) in enumerate(chunks):
            head = struct.pack("<8I", 0x0A324655, 0x9E5D5157, 0x2000, addr, 256, no, len(chunks), RP2040_FAMILY)
            f.write(head + data.ljust(476, b"\0") + struct.pack("<I", 0x0AB16F30))


if __name__ == "__main__":
    main()
