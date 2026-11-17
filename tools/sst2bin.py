"""SingleStepTests の JSON (gzip) を、テストランナーが読む単純なバイナリに変換する。

形式 (すべてリトルエンディアン):
  ファイル先頭: "SST1" と u32 テスト数
  テストごと:
    u16 名前の長さ, 名前 (UTF-8)
    状態 x2 (initial, final):
      u32 x 22: d0-d7, a0-a6, usp, ssp, sr, pc, prefetch0, prefetch1, RAM 項目数 n
      n 項目: u32 アドレス, u8 値 (各 5 バイト)
    u32 サイクル数 (length)
    u32 トランザクション数 m
    m 項目: u8 種別 ('r','w','t','n'), u8 fc, u8 サイズ (1=byte 2=word 0=idle), u8 予備,
            u32 サイクル数, u32 アドレス, u32 値
"""
import gzip
import json
import struct
import sys

REGS = ["d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
        "a0", "a1", "a2", "a3", "a4", "a5", "a6", "usp", "ssp", "sr", "pc"]


def state(s):
    ram = s["ram"]
    out = struct.pack("<19I", *[s[r] & 0xFFFFFFFF for r in REGS])
    out += struct.pack("<3I", s["prefetch"][0], s["prefetch"][1], len(ram))
    for addr, val in ram:
        out += struct.pack("<IB", addr, val)
    return out


def main(src, dst):
    tests = json.load(gzip.open(src))
    with open(dst, "wb") as f:
        f.write(b"SST1" + struct.pack("<I", len(tests)))
        for t in tests:
            name = t["name"].encode()
            f.write(struct.pack("<H", len(name)) + name)
            f.write(state(t["initial"]))
            f.write(state(t["final"]))
            tr = t.get("transactions", [])
            f.write(struct.pack("<II", t["length"], len(tr)))
            for x in tr:
                if x[0] == "n":
                    f.write(struct.pack("<BBBBIII", ord("n"), 0, 0, 0, x[1], 0, 0))
                else:
                    size = 1 if x[4] == ".b" else 2
                    f.write(struct.pack("<BBBBIII", ord(x[0]), x[2], size, 0, x[1], x[3], x[5]))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
