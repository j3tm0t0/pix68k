#!/usr/bin/env python3
# 設定メニューの文字 (src/platform/menu_font.h) を、Circle の 8x16 のフォント (external/circle/lib/font8x16.cpp、
# Linux の kbd の lat1-16。GPLv2 以降) の ASCII の部分 (0x21-0x7E) から作る。0x20 (空白) は全部 0。
#   使い方: python3 -I tools/gen_menu_font.py > src/platform/menu_font.h
import os
import re
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(root, 'external', 'circle', 'lib', 'font8x16.cpp')
glyphs = {}
for line in open(src, encoding='latin-1'):
    m = re.match(r'\s*((?:0x[0-9A-Fa-f]{2},\s*){16})//\s*0x([0-9A-Fa-f]{2})', line)
    if m:
        code = int(m.group(2), 16)
        glyphs[code] = [int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]{2})', m.group(1))]
out = sys.stdout
out.write('// 設定メニューの文字 (ASCII の 0x20-0x7E、8x16 ドット、1 バイトが 1 行で bit7 が左)。\n')
out.write('// tools/gen_menu_font.py が Circle の external/circle/lib/font8x16.cpp から作った (手で直さない)。\n')
out.write('// 元は Linux の kbd パッケージの lat1-16 (Andries Brouwer ほか、GPLv2 以降)。\n')
out.write('#pragma once\n\n#include <stdint.h>\n\nnamespace platform {\n\n')
out.write('static constexpr int MENU_FONT_FIRST = 0x20, MENU_FONT_LAST = 0x7e;\n')
out.write('static const uint8_t kMenuFont[MENU_FONT_LAST - MENU_FONT_FIRST + 1][16] = {\n')
for code in range(0x20, 0x7f):
    if code != 0x20 and code not in glyphs:
        sys.exit('missing glyph 0x%02X' % code)
    g = glyphs.get(code, [0] * 16)
    note = {0x20: 'space', 0x5c: 'backslash'}.get(code, chr(code))
    out.write('    {%s}, // %s\n' % (', '.join('0x%02x' % v for v in g), note))
out.write('};\n\n} // namespace platform\n')
