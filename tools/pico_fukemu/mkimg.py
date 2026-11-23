#!/usr/bin/env python3
"""FUK の偽物 (pico_fukemu) のフラッシュに書くイメージと設定の UF2 を作る。

  mkimg.py out.uf2 --fd0 A.hdm --fd1 B.hdm --stretch 1:1:0:8:2.3

できた UF2 を、BOOTSEL の Pico 2 のドライブに写す (または picotool load out.uf2)。ファームの UF2 とは別に書く。
ファームを書き直してもイメージと設定は残り、イメージの UF2 を書き直してもファームは残る。
置き場所は layout.h と同じ (変えたら両方を直す)。指定しなかったドライブのイメージには触らない。
"""
import argparse
import struct
import sys

IMAGE_BYTES = 77 * 2 * 8 * 1024
CFG_OFF = 0x0FF000
SLOT_HDR = 0x1000
FLASH_BASE = 0x10000000
FAMILY_ABSOLUTE = 0xE48BFF57  # RP2350 のブート ROM が、パーティション表に関わらず書いた番地に置く
MAX_STRETCH = 16
DEFAULT_STRETCH = (1, 1, 0, 8, 23000)
MAX_EXTRA_US = 9474  # core.h の FUKEMU_MAX_EXTRA_US (1 トラックの合計の上限)
DELIVERY = {"stream": 0, "head": 1, "end": 2}
RULE = {"none": 0, "pair": 1, "single": 2, "all": 3}


def slot_off(unit):
    return 0x100000 + unit * 0x140000


def parse_stretch(s):
    """U:C:H:R:PERCENT (例 1:1:0:8:2.3 は、FD1 の C1 H0 R8 のデータを 2.3% 長くする)"""
    p = s.split(":")
    if len(p) != 5:
        raise argparse.ArgumentTypeError(f"{s}: U:C:H:R:PERCENT の形で書く")
    u, c, h, r = (int(x, 0) for x in p[:4])
    pct = float(p[4])
    if not (0 <= u <= 1 and 0 <= c < 77 and 0 <= h <= 1 and 1 <= r <= 8 and 0 <= pct < 50):
        raise argparse.ArgumentTypeError(f"{s}: 範囲の外 (U 0〜1、C 0〜76、H 0〜1、R 1〜8、0〜50%)")
    return (u, c, h, r, round(pct * 10000))


def cfg_block(delivery, rule, stretch, one_sector=False):
    b = b"FUKECFG1" + struct.pack("<BBBB", DELIVERY[delivery], RULE[rule], len(stretch), 1 if one_sector else 0)
    for u, c, h, r, ppm in stretch:
        b += struct.pack("<BBBBi", u, c, h, r, ppm)
    b += b"\0" * ((MAX_STRETCH - len(stretch)) * 8)
    return b.ljust(0x1000, b"\xff")


def img_header(wp):
    return (b"FUKEIMG1" + struct.pack("<II", IMAGE_BYTES, 1 if wp else 0)).ljust(SLOT_HDR, b"\xff")


def uf2(regions):
    """regions: [(フラッシュの先頭からの位置, バイト列)] を 256 バイトずつの UF2 のブロックにする"""
    chunks = []
    for off, data in regions:
        data = data + b"\xff" * (-len(data) % 256)
        for i in range(0, len(data), 256):
            chunks.append((FLASH_BASE + off + i, data[i:i + 256]))
    out = bytearray()
    for n, (addr, d) in enumerate(chunks):
        hdr = struct.pack("<IIIIIIII", 0x0A324655, 0x9E5D5157, 0x00002000, addr, 256, n, len(chunks), FAMILY_ABSOLUTE)
        out += hdr + d + b"\0" * (476 - 256) + struct.pack("<I", 0x0AB16F30)
    return bytes(out)


def regions_for(args):
    """書く場所と中身の一覧 [(フラッシュの場所, バイト列)] (fukemu_net.py も使う)"""
    regions = []
    if args.config or args.stretch or args.delivery or args.rule or args.one_sector or args.no_stretch:
        # --stretch を省くと、ファームの既定と同じ FD1 の C1 H0 R8 +2.3%
        stretch = [] if args.no_stretch else (args.stretch or [DEFAULT_STRETCH])
        args.stretch = stretch
        if len(args.stretch) > MAX_STRETCH:
            sys.exit(f"mkimg: --stretch は {MAX_STRETCH} 個まで")
        per_track = {}
        if len({x[:4] for x in args.stretch}) != len(args.stretch):
            sys.exit("mkimg: 同じセクタの --stretch が 2 つある")
        for u, c, h, r, ppm in args.stretch:
            per_track.setdefault((u, c, h), {})[r] = 1024 * 16 * ppm // 1000000
        for (u, c, h), xs in per_track.items():
            if sum(xs.values()) > MAX_EXTRA_US:
                sys.exit(f"mkimg: FD{u} C{c} H{h} を長くした分が合わせて {sum(xs.values())}us。"
                         f"1 回転に収まるのは {MAX_EXTRA_US}us まで")
        regions.append((CFG_OFF, cfg_block(args.delivery or "stream", args.rule or "pair", stretch, args.one_sector)))
    for unit, path, wp, erase in ((0, args.fd0, args.wp0, args.erase0), (1, args.fd1, args.wp1, args.erase1)):
        if erase:
            regions.append((slot_off(unit), b"\xff" * SLOT_HDR))  # 見出しを消すと、ディスクなし
            continue
        if not path:
            continue
        with open(path, "rb") as f:
            img = f.read()
        if len(img) != IMAGE_BYTES:
            sys.exit(f"mkimg: {path} は {len(img)} バイト。2HD のイメージ ({IMAGE_BYTES} バイト) でない")
        regions.append((slot_off(unit), img_header(wp) + img))
    if not regions:
        sys.exit("mkimg: 書くものがない (--fd0、--fd1、--stretch などを付ける)")
    return regions


def build(args):
    return uf2(regions_for(args))


def add_args(ap):
    """書くものを選ぶ引数 (fukemu_net.py も使う)"""
    ap.add_argument("--fd0", help="FD0 のイメージ (2HD、1261568 バイトの XDF / HDM)")
    ap.add_argument("--fd1", help="FD1 のイメージ")
    ap.add_argument("--wp0", action="store_true", help="FD0 を書き込み禁止にする")
    ap.add_argument("--wp1", action="store_true", help="FD1 を書き込み禁止にする")
    ap.add_argument("--erase0", action="store_true", help="FD0 をディスクなしにする")
    ap.add_argument("--erase1", action="store_true", help="FD1 をディスクなしにする")
    ap.add_argument("--config", action="store_true", help="設定も書く (--stretch などを付けたときは自動で書く)")
    ap.add_argument("--delivery", choices=DELIVERY, help="実時間の Read の送り方 (既定 stream)")
    ap.add_argument("--rule", choices=RULE, help="どの Read を実時間で答えるか (既定 pair)")
    ap.add_argument("--no-stretch", action="store_true", help="どのセクタも長くしない")
    ap.add_argument("--one-sector", action="store_true",
                    help="実時間の Read は最初のセクタだけを送って終える (既定は EOT まで。R=7 EOT=8 でもセクタ 7 だけになる)")
    ap.add_argument("--stretch", type=parse_stretch, action="append", default=[],
                    help="U:C:H:R:PERCENT。そのセクタのデータを PERCENT %% 長くする (何度でも。省くと 1:1:0:8:2.3)")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", help="作る UF2")
    add_args(ap)
    args = ap.parse_args(argv)
    data = build(args)
    with open(args.out, "wb") as f:
        f.write(data)
    print(f"{args.out}: {len(data) // 512} ブロック")


if __name__ == "__main__":
    main()
