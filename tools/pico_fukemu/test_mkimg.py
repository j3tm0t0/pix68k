#!/usr/bin/env python3
"""mkimg.py の試験: UF2 を読み戻して、置き場所が layout.h と合い、中身が正しいことを確かめる"""
import os
import re
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkimg  # noqa: E402

here = os.path.dirname(os.path.abspath(__file__))


def layout_values():
    s = open(os.path.join(here, "layout.h"), encoding="utf-8").read()
    v = {k: int(x, 16) for k, x in re.findall(r"#define (FUKEMU_\w+) (0x[0-9A-Fa-f]+)u", s)}
    m = re.search(r"#define FUKEMU_SLOT_OFF\(u\) \((0x[0-9A-Fa-f]+)u \+ \(uint32_t\)\(u\) \* (0x[0-9A-Fa-f]+)u\)", s)
    v["SLOT_BASE"], v["SLOT_STEP"] = int(m.group(1), 16), int(m.group(2), 16)
    return v


def read_uf2(data):
    mem = {}
    n = len(data) // 512
    for i in range(n):
        b = data[i * 512:(i + 1) * 512]
        m0, m1, flags, addr, size, no, total, fam = struct.unpack("<IIIIIIII", b[:32])
        assert (m0, m1) == (0x0A324655, 0x9E5D5157) and struct.unpack("<I", b[508:])[0] == 0x0AB16F30
        assert flags == 0x2000 and fam == 0xE48BFF57 and size == 256 and no == i and total == n
        assert addr not in mem
        mem[addr] = b[32:32 + 256]
    return mem


def flash(mem, off, n):
    out = b""
    for a in range(off, off + n, 256):
        out += mem[0x10000000 + a]
    return out[:n]


def main():
    v = layout_values()
    assert v["FUKEMU_CFG_OFF"] == mkimg.CFG_OFF and v["FUKEMU_SLOT_HDR"] == mkimg.SLOT_HDR
    assert all(mkimg.slot_off(u) == v["SLOT_BASE"] + u * v["SLOT_STEP"] for u in (0, 1))
    # イメージはスロットに収まり、次のスロットやフラッシュの終わり (4MB) を越えない
    assert mkimg.SLOT_HDR + mkimg.IMAGE_BYTES <= v["SLOT_STEP"]
    assert mkimg.slot_off(1) + mkimg.SLOT_HDR + mkimg.IMAGE_BYTES <= 4 * 1024 * 1024
    assert v["FUKEMU_FW_MAX"] <= mkimg.CFG_OFF

    with tempfile.TemporaryDirectory() as d:
        img = bytes((i * 7 + 3) & 0xFF for i in range(mkimg.IMAGE_BYTES))
        p = os.path.join(d, "b.hdm")
        open(p, "wb").write(img)
        out = os.path.join(d, "x.uf2")
        mkimg.main([out, "--fd1", p, "--wp1", "--stretch", "1:1:0:8:2.3", "--delivery", "head"])
        mem = read_uf2(open(out, "rb").read())
        hdr = flash(mem, mkimg.slot_off(1), 16)
        assert hdr == b"FUKEIMG1" + struct.pack("<II", mkimg.IMAGE_BYTES, 1)
        assert flash(mem, mkimg.slot_off(1) + mkimg.SLOT_HDR, mkimg.IMAGE_BYTES) == img
        assert 0x10000000 + mkimg.slot_off(0) not in mem  # FD0 には触らない
        cfg = flash(mem, mkimg.CFG_OFF, 20)
        assert cfg[:8] == b"FUKECFG1" and cfg[8:12] == bytes([1, 1, 1, 0])
        assert struct.unpack("<BBBBi", cfg[12:20]) == (1, 1, 0, 8, 23000)
        # --stretch を省くと FD1 の C1 H0 R8 +2.3%、既定の送り方は head
        mkimg.main([out, "--config"])
        cfg = flash(read_uf2(open(out, "rb").read()), mkimg.CFG_OFF, 20)
        assert cfg[8:12] == bytes([0, 1, 1, 0]) and struct.unpack("<BBBBi", cfg[12:20]) == (1, 1, 0, 8, 23000)
        mkimg.main([out, "--no-stretch", "--one-sector"])
        cfg = flash(read_uf2(open(out, "rb").read()), mkimg.CFG_OFF, 12)
        assert cfg[8:12] == bytes([0, 1, 0, 1])
        # 消す
        mkimg.main([out, "--erase0"])
        mem = read_uf2(open(out, "rb").read())
        assert flash(mem, mkimg.slot_off(0), 8) == b"\xff" * 8 and len(mem) == 16
        # 1 トラックの合計が 1 回転に収まらない設定は断る
        try:
            mkimg.main([out, "--stretch", "0:0:0:7:40", "--stretch", "0:0:0:8:40"])
            raise AssertionError("長すぎる設定を断らなかった")
        except SystemExit as e:
            assert "1 回転" in str(e)
        core_h = open(os.path.join(here, "core.h"), encoding="utf-8").read()
        assert f"#define FUKEMU_MAX_EXTRA_US {mkimg.MAX_EXTRA_US}u" in core_h
        # 大きさの違うイメージは断る
        open(p, "wb").write(img[:-1])
        try:
            mkimg.main([out, "--fd0", p])
            raise AssertionError("短いイメージを断らなかった")
        except SystemExit as e:
            assert "2HD" in str(e)
    check_partitions(layout_values())
    print("test_mkimg: ok")


def size_of(x):
    """pt.json の "8K" / "496K" / "1M" / 数を、バイト数にする"""
    if isinstance(x, int):
        return x
    x = str(x).strip().upper()
    mul = {"K": 1024, "M": 1024 * 1024}.get(x[-1], 1)
    return int(x[:-1] if x[-1] in "KM" else x, 0) * mul


def check_partitions(v):
    """OTA の pt.json の A / B が、設定とイメージの場所に重ならないこと (A と B も重ならない)"""
    import json
    pt = json.load(open(os.path.join(here, "pt.json"), encoding="utf-8"))
    spans = []
    for part in pt["partitions"]:
        start, size = size_of(part["start"]), size_of(part["size"])
        assert start % 4096 == 0 and size % 4096 == 0, part["name"]
        assert start >= 0x2000, "%s がパーティション表 (先頭 8KB) に重なる" % part["name"]
        spans.append((start, start + size, part["name"]))
    spans.sort()
    for a, b in zip(spans, spans[1:]):
        assert a[1] <= b[0], "%s と %s が重なる" % (a[2], b[2])
    assert spans[-1][1] <= v["FUKEMU_CFG_OFF"], "%s が設定の場所 (0x%x) に届く" % (spans[-1][2], v["FUKEMU_CFG_OFF"])
    # ファームの大きさの上限 (check_size.cmake に渡す 496KB) は、パーティションより大きくしない
    cm = open(os.path.join(here, "CMakeLists.txt"), encoding="utf-8").read()
    limit = int(re.search(r"set\(fukemu_size_limit (\d+)\) # 496KB", cm).group(1))
    assert all(limit <= e - s for s, e, _ in spans), "fukemu_size_limit がパーティションより大きい"


if __name__ == "__main__":
    main()
