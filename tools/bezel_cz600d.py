#!/usr/bin/env python3
# video_filter の cz-600de (グレー) と cz-600db (黒) で使う、ブラウン管のモニタの枠の絵を作る (#230)。
#
#   python3 bezel_cz600d.py <出力フォルダ>
#
# <出力フォルダ>/bezel-cz-600de.png と bezel-cz-600db.png を書く。1920x1080 の RGBA で、
# アルファは乗算しない。ガラスの部分はアルファ 0 (RGB も 0)。
# 絵はすべてこのスクリプトが手続きで描く。写真やほかの画像は使わず、ファイルも読まない (実物の写真は形と寸法の参考にしただけ)。
# 文字とロゴも描かない。
# 使うのは numpy と Pillow だけ。乱数の種は固定なので、同じ環境なら何度作っても同じ絵になる。numpy や Pillow の版が違うと、
# 小数の丸めの違いで数画素の色が 1 だけ違うことがある (Mac の numpy 2.5.1 / Pillow 12.3.0 と、ubuntu-latest の apt の版で
# 1920x1080 のうち 4 画素。アルファ (枠の形) は同じ)。
#
# 寸法は「基準の単位」(写真のモニタの幅を約 1200 とした単位) で書き、出力の画素に直す。
# ガラスの外 (外枠と斜面) は倍率 k で細くし、下の部分 (フタ、LED、スイッチ) は別の倍率 BAND_B で描く。
import os
import sys

import numpy as np
from PIL import Image

W, H = 1920, 1080
yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)

GLASS_W = 1120                # 細くする前の、ガラスの幅 (出力の画素)
CAB = (62, 100, 1145, 1122)   # 本体の外形
CAB_RT, CAB_RB = 72, 26       # 本体の角の丸み (上、下)
CAB_ROLL = 20                 # 本体の前面の縁の丸み
INNER = (112, 174, 1098, 966) # 斜面の外側の縁。下の辺 (966) から下が、フタなどの並ぶ部分
INNER_RT, INNER_RB = 34, 18
SLOPE = (148, 186, 1050, 906) # 斜面の幅を決めるための枠 (ガラスとの差が斜面の幅)
GLASS = (165, 228, 1030, 880) # 細くする前のガラス
GLASS_R = 34                  # ガラスの角の丸み
LID = (176, 994, 908, 1092)   # フタ (下は本体の下の端より外まで延ばす)
LEDWIN = (914, 994, 1012, 1092)
IR = (104, 1044, 156, 1078)   # 受光窓
SWSLOT = (1028, 1034, 1114, 1088)
BAND_B = 0.78                 # 下の部分の倍率 (出力の画素 / 基準の単位)
SEED = 11

PAL = {
    'gray': dict(base=(0.595, 0.605, 0.598), kd=0.80, amb=0.36, ks=0.10, shin=22, kr=0.025,
                 grain=0.11, gasket=(0.05, 0.05, 0.05), bg=(0.085, 0.085, 0.09),
                 ir=(0.06, 0.06, 0.065), ledwin=(0.30, 0.30, 0.29), lid=(0.585, 0.595, 0.59),
                 led_off=(0.16, 0.20, 0.15)),
    'black': dict(base=(0.125, 0.125, 0.155), kd=0.75, amb=0.40, ks=0.20, shin=30, kr=0.07,
                  grain=0.09, gasket=(0.015, 0.015, 0.018), bg=(0.10, 0.10, 0.11),
                  ir=(0.03, 0.03, 0.035), ledwin=(0.05, 0.05, 0.06), lid=(0.115, 0.115, 0.14),
                  led_off=(0.07, 0.09, 0.07)),
}
OUTPUTS = (('cz-600de', 'gray'), ('cz-600db', 'black'))


def box_blur(a, r, axis):
    pad = [(0, 0)] * a.ndim
    pad[axis] = (r + 1, r)
    c = np.cumsum(np.pad(a, pad, mode='edge'), axis=axis, dtype=np.float64)
    n = a.shape[axis]
    hi = np.take(c, np.arange(2 * r + 1, 2 * r + 1 + n), axis=axis)
    lo = np.take(c, np.arange(0, n), axis=axis)
    return ((hi - lo) / (2 * r + 1)).astype(np.float32)


def blur(a, sigma):
    """箱型のぼかしを 3 回掛けて、ガウスぼかしの代わりにする"""
    r = max(1, int(round(sigma * 0.9)))
    for _ in range(3):
        a = box_blur(a, r, 0)
        a = box_blur(a, r, 1)
    return a


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def cover(d, aa=0.75):
    """符号付き距離 d (内側が負) から、縁をなめらかにした塗りの割合 (内側で 1) を出す"""
    return np.clip(0.5 - d / (2 * aa), 0, 1)


def sd_box(x0, y0, x1, y1, tl, tr, br, bl):
    """出力の画素で書いた角丸の四角の、符号付き距離。角ごとに丸みを変えられる"""
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    hw, hh = (x1 - x0) / 2, (y1 - y0) / 2
    rad = np.where(yy < cy, np.where(xx < cx, tl, tr), np.where(xx < cx, bl, br))
    qx = np.abs(xx - cx) - (hw - rad)
    qy = np.abs(yy - cy) - (hh - rad)
    return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - rad


class Map:
    """基準の単位から出力の画素へ。ガラスの外は k 倍に縮める"""

    def __init__(self, s, k):
        self.s, self.k, self.ox, self.oy = s, k, 0.0, 0.0
        self.u = s * k

    def _f(self, v, a, b):
        if v < a:
            return a - self.k * (a - v)
        if v > b:
            return b + self.k * (v - b)
        return v

    def X(self, x):
        return self.ox + self.s * self._f(x, GLASS[0], GLASS[2])

    def Y(self, y):
        return self.oy + self.s * self._f(y, GLASS[1], GLASS[3])

    def sd(self, r, rt, rb=None):
        rb = rt if rb is None else rb
        return sd_box(self.X(r[0]), self.Y(r[1]), self.X(r[2]), self.Y(r[3]),
                      rt * self.u, rt * self.u, rb * self.u, rb * self.u)


class BandMap(Map):
    """下の部分は倍率 b でそろえる。SPLIT より左は本体の左端から、右は右端から測るので、伸び縮みするのはフタの幅だけ"""
    SPLIT = 545

    def __init__(self, b, xl, xr, ytop):
        self.b, self.xl, self.xr, self.ytop = b, xl, xr, ytop
        self.u = b

    def X(self, x):
        if x < self.SPLIT:
            return self.xl + self.b * (x - CAB[0])
        return self.xr - self.b * (CAB[2] - x)

    def Y(self, y):
        return self.ytop + self.b * (y - INNER[3])


def render(color):
    """枠の絵 (RGB、0..1) と、ガラスの塗りの割合 (1 がガラス) を返す"""
    P = PAL[color]
    rng = np.random.default_rng(SEED)
    gx0, gy0, gx1, gy1 = GLASS
    s = GLASS_W / (gx1 - gx0)
    b = BAND_B
    band_h = b * (CAB[3] - INNER[3])
    # 本体の高さが 1080 になるように、ガラスの外の倍率 k を決める
    k = ((H - band_h) / s - (gy1 - gy0)) / ((gy0 - CAB[1]) + (INNER[3] - gy1))
    m = Map(s, k)
    m.ox = W / 2 - (m.X(CAB[0]) + m.X(CAB[2])) / 2
    m.oy = -m.Y(CAB[1])
    mb = BandMap(b, m.X(CAB[0]), m.X(CAB[2]), m.Y(INNER[3]))
    s = m.u  # ここから下の高さや幅は、細くした枠の単位で書く

    # ---- 高さの地図 ----
    dc = sd_box(m.X(CAB[0]), 0, m.X(CAB[2]), H, CAB_RT * s, CAB_RT * s, CAB_RB * b, CAB_RB * b) / s
    roll = np.clip(dc + CAB_ROLL, 0, CAB_ROLL) / CAB_ROLL
    h = np.where(dc < 0, (np.sqrt(np.clip(1 - roll ** 2, 0, 1)) - 1) * CAB_ROLL, -60)
    cab_mask = cover(dc * s)

    # 斜面は INNER の縁から始まり、ガラスへ直接落ちる。ガラスは 4:3 にして中央に置く
    ix0, iy0, ix1, iy1 = m.X(INNER[0]), m.Y(INNER[1]), m.X(INNER[2]), m.Y(INNER[3])
    G0 = ix0 + m.X(GLASS[0]) - m.X(SLOPE[0])
    G1 = iy0 + m.Y(GLASS[1]) - m.Y(SLOPE[1])
    G2 = ix1 - (m.X(SLOPE[2]) - m.X(GLASS[2]))
    G3 = iy1 - (m.Y(SLOPE[3]) - m.Y(GLASS[3]))
    gw, gh = G2 - G0, G3 - G1
    if gw * 3 > gh * 4:
        c = (G0 + G2) / 2; gw = gh * 4 / 3; G0, G2 = c - gw / 2, c + gw / 2
    else:
        c = (G1 + G3) / 2; gh = gw * 3 / 4; G1, G3 = c - gh / 2, c + gh / 2
    do = sd_box(ix0, iy0, ix1, iy1, INNER_RT * s, INNER_RT * s, INNER_RB * s, INNER_RB * s) / s
    dg = sd_box(G0, G1, G2, G3, GLASS_R * s, GLASS_R * s, GLASS_R * s, GLASS_R * s) / s
    t = np.clip(-do / np.maximum(-do + dg, 1e-3), 0, 1)
    slope_depth = 32
    h -= np.where(do < 0, slope_depth * t, 0)
    h = np.where(dg < 0, -slope_depth, h)

    # フタ: 少し奥に下がった面と、まわりの細い溝。下は本体の外まで延ばすので、下の溝は出ない
    dl = mb.sd((LID[0], LID[1], LID[2], CAB[3] + 40), 11, 4) / b
    h -= 1.2 * smooth(1, -1, dl)
    h -= 2.5 * np.exp(-(dl / 0.9) ** 2)
    # フタの上の中央の、押して開ける所の小さな出っ張り (文字は描かない)
    lx = (mb.X(LID[0]) + mb.X(LID[2])) / 2
    dmark = sd_box(lx - 9 * b, mb.Y(LID[1] + 10), lx + 9 * b, mb.Y(LID[1] + 13), *([1.5 * b] * 4)) / b
    h += 0.7 * smooth(0.6, -0.6, dmark)
    # LED の窓 (右上の角だけ丸い)
    dlw = sd_box(mb.X(LEDWIN[0]), mb.Y(LEDWIN[1]), mb.X(LEDWIN[2]), mb.Y(LEDWIN[3]),
                 3 * b, 11 * b, 3 * b, 3 * b) / b
    h -= 1.6 * smooth(1, -1, dlw)
    h -= 2.5 * np.exp(-(dlw / 0.9) ** 2)
    dir_ = mb.sd(IR, 2) / b
    h -= 1.5 * smooth(0.8, -0.8, dir_)
    # スライドスイッチのくぼみと、細く長いつまみ 2 本 (左が黒、右がオレンジ)
    dsl = mb.sd(SWSLOT, 22) / b
    h -= 7 * smooth(1.5, -2.5, dsl)
    levers = []
    for (x0, c) in ((1052, 'k'), (1080, 'o')):
        dlv = mb.sd((x0, 1036.5, x0 + 10, 1085.5), 2.5) / b
        rib = 0.5 * np.cos((yy - mb.Y(1036.5)) / b * 2 * np.pi / 4.0) * (dlv < -1.5)
        h = np.where(dlv < 0, -7 + 5.5 * smooth(0, -2, dlv) + rib, h)
        levers.append((cover(dlv * b), c))

    # 樹脂の細かいざらつき
    grain = blur(rng.standard_normal((H, W)).astype(np.float32), 1.1)
    hs = h * s + grain * P['grain'] * s / 1.5

    # ---- 陰影 ----
    n = np.stack([-np.gradient(hs, axis=1), -np.gradient(hs, axis=0), np.ones_like(hs)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    L = np.array([-0.30, -0.80, 0.52], np.float32); L /= np.linalg.norm(L)    # 左上からの光
    L2 = np.array([0.55, -0.2, 0.8], np.float32); L2 /= np.linalg.norm(L2)    # 弱い補助の光
    Hv = L + np.array([0, 0, 1], np.float32); Hv /= np.linalg.norm(Hv)
    diff = np.minimum(np.clip(n @ L, 0, 1) / float(L[2]), 1.25)
    fill = np.clip(n @ L2, 0, 1) / float(L2[2])
    spec = np.clip(n @ Hv, 0, 1) ** P['shin']
    spec_wide = np.clip(n @ Hv, 0, 1) ** 4
    # 上を向いた面は明るい天井を、下を向いた面は暗い床を映す
    env = np.clip(-n[..., 1] * 2.2, -1, 1) * (1 - n[..., 2]) * 4
    ao = 1 - np.clip((blur(hs, 9 * s) - hs) * 0.02 / s, 0, 0.5)
    fall = 1.04 - 0.10 * (yy / H)

    alb = np.broadcast_to(np.array(P['base'], np.float32), (H, W, 3)).copy()
    alb *= (1 + 0.03 * blur(rng.standard_normal((H, W)).astype(np.float32), 25))[..., None]

    def paint(mask, c):
        nonlocal alb
        alb = alb * (1 - mask[..., None]) + np.array(c, np.float32) * mask[..., None]

    paint(cover(dl * b), P['lid'])
    paint(cover(dlw * b), P['ledwin'])
    paint(cover(dir_ * b), P['ir'])
    paint(cover(dsl * b) * 0.6, (0.02, 0.02, 0.025))
    for mk, c in levers:
        paint(mk, (0.06, 0.06, 0.065) if c == 'k' else (0.85, 0.36, 0.10))
    paint(cover(dg * s + 2.5 * s), P['gasket'])

    light = (P['amb'] + P['kd'] * (0.85 * diff + 0.15 * fill)) * ao * fall
    col = alb * light[..., None] + (P['ks'] * spec + 0.03 * spec_wide + P['kr'] * env)[..., None]

    # ---- ガラス ----
    # 中はアルファ 0 にするが、縁の半透明の画素の色のために、絵のないガラスの色 (黒の浮き、上の縁の影、映り込み) を塗る
    dgc = dg * s + 2.5 * s
    glass = cover(dgc, 1.0)
    cx = (G0 + G2) / 2
    nyy = (yy - (G1 + G3) / 2) / ((G3 - G1) / 2)
    shadow_top = 1 - 0.6 * smooth(-30 * s / 1.5, 0, dgc) * np.clip(0.55 - nyy * 0.6, 0, 1)
    sc = np.broadcast_to(np.array([0.045, 0.05, 0.048], np.float32), (H, W, 3)) * shadow_top[..., None]
    refl = np.exp(-(((xx - (cx - (G2 - G0) * 0.22)) / ((G2 - G0) * 0.38)) ** 2
                    + ((yy - (G1 + (G3 - G1) * 0.18)) / ((G3 - G1) * 0.22)) ** 2))
    sc = sc + (0.06 * refl)[..., None]
    sc = sc + (0.05 * np.exp(-((dgc + 4) / 3) ** 2) * np.clip(nyy, 0, 1))[..., None]
    col = col * (1 - glass[..., None]) + sc * glass[..., None]

    # ---- LED (2 列 x 3 段。左上と右下が緑で点く) ----
    for ci, lx in enumerate((934, 986)):
        for ri, ly in enumerate((1016, 1043, 1070)):
            on = (ci == 0 and ri == 0) or (ci == 1 and ri == 2)
            px, py = mb.X(lx), mb.Y(ly)
            r = 3.2 * b
            dist = np.hypot(xx - px, yy - py)
            mk = cover(dist - r)
            if on:
                c = np.array([0.35, 1.0, 0.40], np.float32)
                core = np.exp(-((xx - px) ** 2 + (yy - py) ** 2) / (2 * (r * 0.7) ** 2))
                col = col * (1 - mk[..., None]) + c * (0.6 + 0.4 * core)[..., None] * mk[..., None]
                col += c * (0.6 * core + 0.16 * np.exp(-dist / (5 * b)) * (1 - mk))[..., None]
            else:
                col = col * (1 - mk[..., None]) + np.array(P['led_off'], np.float32) * mk[..., None]
                col += (0.12 * np.exp(-(((xx - px + r * 0.3) ** 2 + (yy - py + r * 0.35) ** 2)
                                        / (2 * (r * 0.3) ** 2))))[..., None]

    # ---- 本体の外の暗い背景 ----
    bg = np.array(P['bg'], np.float32) * (0.7 + 0.3 * np.exp(-((xx - W / 2) / 900) ** 2))[..., None]
    bg *= (0.5 + 0.5 * blur(1 - cab_mask, 14))[..., None]
    col = col * cab_mask[..., None] + bg * (1 - cab_mask[..., None])

    col += (rng.random((H, W, 3)).astype(np.float32) - 0.5) / 255  # 段差が出ないように散らす
    return col, glass


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: bezel_cz600d.py <出力フォルダ>')
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, color in OUTPUTS:
        col, glass = render(color)
        rgb = (np.clip(col, 0, 1) * 255 + 0.5).astype(np.uint8).astype(np.float32)
        a = 1 - glass
        rgb[a <= 0] = 0
        rgba = np.dstack([rgb, a * 255])
        path = os.path.join(out, f'bezel-{name}.png')
        Image.fromarray((rgba + 0.5).clip(0, 255).astype(np.uint8), 'RGBA').save(path)
        print(path)


if __name__ == '__main__':
    main()
