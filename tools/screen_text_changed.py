#!/usr/bin/env python3
# /type で打った文字が /screen.ppm に写っているかを見る (tools/qemu_human68k.sh の HOSTNET=1 で使う)。
#   使い方: python3 -I tools/screen_text_changed.py 打つ前.ppm 打ったあと.ppm serial.txt 'echo hostnet'
# serial.txt (UART のログ) の最後のテキスト画面のダンプ (pix68k_textdump=1 の textdump begin ... end の間の text|... の行。
# 1 行目が画面のいちばん上の行) から、文字列を含む最後の行と桁を探す。その文字の 8x16 の升目 (768x512 の画面の
# テキストは 96 桁 x 32 行) を 2 つの PPM で比べ、空白でない文字の升目が、打ったあとはすべて黒だけでなく、
# 打つ前とは 1 つを除いてすべて違うことを確かめる (打つ前のカーソルの升目が、たまたま打った文字と同じ絵になることを考えて
# 1 つは見逃す)。カーソルの点滅で変わるのは 1 つの升目だけなので、それだけでは通らない。
# テキスト画面のダンプはスクロールを考えない (テキスト VRAM の左上から読む) ので、Human68k のようにスクロールで
# VRAM の中身を動かすときだけ使える。合えば 0、合わなければ 1 で終わる
import sys

CELL_W, CELL_H = 8, 16


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    # ヘッダ: P6、幅、高さ、最大値 (空白か改行で区切る。# からの行は注釈)
    fields, pos = [], 0
    while len(fields) < 4:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            while pos < len(data) and data[pos:pos + 1] != b'\n':
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        if start == pos:
            raise ValueError(f'{path}: short header')
        fields.append(data[start:pos])
    if fields[0] != b'P6' or int(fields[3]) != 255:
        raise ValueError(f'{path}: not a P6 PPM with maxval 255')
    w, h = int(fields[1]), int(fields[2])
    pixels = data[pos + 1:]
    if len(pixels) < w * h * 3:
        raise ValueError(f'{path}: {len(pixels)} bytes of pixels, want {w * h * 3}')
    return w, h, pixels


def last_text_dump(path):
    rows, block = None, None
    with open(path, 'rb') as f:
        for raw in f:
            line = raw.decode('latin-1').rstrip('\r\n')
            if 'textdump begin' in line:
                block = []
            elif 'textdump end' in line:
                if block is not None:
                    rows = block
                block = None
            elif block is not None:
                i = line.find('text|')
                if i >= 0:
                    block.append(line[i + 5:].rstrip('\r'))
    return rows


def cell(img, col, row):
    w, _, pixels = img
    out = bytearray()
    for y in range(row * CELL_H, (row + 1) * CELL_H):
        p = (y * w + col * CELL_W) * 3
        out += pixels[p:p + CELL_W * 3]
    return bytes(out)


def main():
    if len(sys.argv) != 5:
        print('usage: screen_text_changed.py before.ppm after.ppm serial.txt text', file=sys.stderr)
        return 2
    before, after = read_ppm(sys.argv[1]), read_ppm(sys.argv[2])
    text = sys.argv[4]
    if before[:2] != after[:2]:
        print(f'screen size changed: {before[0]}x{before[1]} -> {after[0]}x{after[1]}')
        return 1
    rows = last_text_dump(sys.argv[3])
    if not rows:
        print('no text dump in the log')
        return 1
    found = [(r, line.find(text)) for r, line in enumerate(rows) if text in line]
    if not found:
        print(f'"{text}" is not in the last text dump')
        return 1
    row, col = found[-1]
    w, h, _ = after
    if (col + len(text)) * CELL_W > w or (row + 1) * CELL_H > h:
        print(f'row {row}, column {col} is outside the {w}x{h} screen')
        return 1
    want = changed = drawn = 0
    for k, ch in enumerate(text):
        if ch == ' ':
            continue
        want += 1
        b, a = cell(before, col + k, row), cell(after, col + k, row)
        if a != b:
            changed += 1
        if any(a):
            drawn += 1
    print(f'row {row}, columns {col}-{col + len(text) - 1}: {changed} of {want} cells changed, {drawn} not black')
    return 0 if changed >= want - 1 and drawn == want and want >= 2 else 1


if __name__ == '__main__':
    sys.exit(main())
