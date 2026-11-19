#!/usr/bin/env python3
# モニタの枠の絵 (ガラスが透明な RGBA の PNG) を、ベアメタル版が SD カードの /pix68k/ から読む形 (bezel-<名前>.bin) にする。
#   使い方: tools/bezel_pack.py <枠の絵.png> <出力.bin> [ガラスの x y 幅 高さ]
#   ガラスの矩形の既定は 380 68 1160 870 (tools/bezel_cz600d.py の絵。4:3、x は 4 画素にそろえる)
# 形式は src/platform/bezel.h: "PXBZ"、u16 の幅、高さ、ガラスの x、y、幅、高さ (リトルエンディアン)、そのあと
# 幅 x 高さ の u32 (A << 24 | 黒に重ねた RGB)。
# カーネルは、ガラスの矩形の各行で、左右の端から続く A が 0 でない画素だけを枠として描く。その間に A が 0 でない画素が
# あると描けないので、失敗する。要るもの: numpy、Pillow
import struct
import sys

import numpy as np
from PIL import Image


def main():
    if len(sys.argv) not in (3, 7):
        sys.exit('usage: bezel_pack.py <bezel.png> <out.bin> [gx gy gw gh]')
    src, out = sys.argv[1], sys.argv[2]
    gx, gy, gw, gh = (int(v) for v in sys.argv[3:7]) if len(sys.argv) == 7 else (380, 68, 1160, 870)
    a = np.asarray(Image.open(src).convert('RGBA')).astype(np.uint32)
    h, w = a.shape[:2]
    if gx % 4 != 0:
        sys.exit(f'{src}: glass x ({gx}) must be a multiple of 4')
    if gw < 16 or gh < 16 or gx + gw > w or gy + gh > h:
        sys.exit(f'{src}: glass {gw}x{gh} at ({gx},{gy}) does not fit in {w}x{h}')
    al = a[..., 3]
    # ガラスの矩形の各行: 左右の端から続く枠の画素の間に、枠の画素 (A が 0 でない) がないこと
    glass = al[gy:gy + gh, gx:gx + gw] != 0
    for y in range(gh):
        row = glass[y]
        clear = np.flatnonzero(~row)
        if clear.size and row[clear[0]:clear[-1] + 1].any():
            sys.exit(f'{src}: row {gy + y} has frame pixels inside the glass; the kernel cannot draw them')
    if glass.all(axis=1).all():
        sys.exit(f'{src}: the glass is not transparent')
    # ガラスの矩形の外の透明な画素は、カーネルが黒 (黒に重ねた色) で描く。矩形のまわり 2 画素までは許す (いまの絵は
    # ガラスが矩形より右に 1 画素広い)。それより外にあれば、矩形と絵のガラスがずれている
    outside = al == 0
    outside[max(gy - 2, 0):gy + gh + 2, max(gx - 2, 0):gx + gw + 2] = False
    if outside.any():
        ys, xs = np.nonzero(outside)
        sys.exit(f'{src}: {outside.sum()} transparent pixels outside the glass rect (e.g. at {xs[0]},{ys[0]}); '
                 'the glass rect does not match the image')
    rgb = (a[..., :3] * al[..., None] + 127) // 255
    px = (al << 24) | (rgb[..., 0] << 16) | (rgb[..., 1] << 8) | rgb[..., 2]
    with open(out, 'wb') as f:
        f.write(b'PXBZ' + struct.pack('<6H', w, h, gx, gy, gw, gh))
        f.write(px.astype('<u4').tobytes())
    print(f'{out}: {w}x{h}, glass {gw}x{gh} at ({gx},{gy})')


if __name__ == '__main__':
    main()
