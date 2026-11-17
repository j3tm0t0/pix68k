#!/usr/bin/env python3
# JIS X 0208 の区点 (0x2121-0x7E7E の 2 バイト) と Unicode の対応表 (src/platform/jis_table.h) を作る。
# 画面の左上のメッセージ (platform/osd_message.h) が、X68000 の CGROM の 16x16 の字を引くのに使う。
# 表は Python の文字コードの表から作る (cp932 と euc_jp)。同じ区点に Unicode が 2 つある字 (「～」の U+FF5E と
# U+301C など) は両方入れる (macOS や curl はどちらも送るため)。CGROM にある区 (1-8 区と 16-84 区) だけ。
#   使い方: python3 -I tools/gen_jis_table.py > src/platform/jis_table.h
import sys

ROWS = list(range(0x21, 0x29)) + list(range(0x30, 0x75))
main = {}  # 区点 -> Unicode (cp932 の表。なければ euc_jp)
alt = {}   # もう一方の表だけにある Unicode -> 区点 (「〜」U+301C など)
for row in ROWS:
    for col in range(0x21, 0x7f):
        jis = row << 8 | col
        euc = bytes([row | 0x80, col | 0x80])
        # JIS から Shift_JIS へ
        r, c = row, col
        s1 = ((r + 1) >> 1) + (0x70 if r <= 0x5e else 0xb0)
        s2 = c + (0x1f if r & 1 else 0x7e)
        if r & 1 and c >= 0x60:
            s2 += 1
        sjis = bytes([s1, s2])
        for codec, data in (('cp932', sjis), ('euc_jp', euc)):
            try:
                u = data.decode(codec)
            except UnicodeDecodeError:
                continue
            if len(u) != 1:
                continue
            cp = ord(u)
            if cp < 0x80 or cp > 0xffff:
                continue
            if jis not in main:
                main[jis] = cp
            elif main[jis] != cp:
                alt.setdefault(cp, jis)
used = set(main.values())
alt = {u: j for u, j in alt.items() if u not in used}

out = sys.stdout
out.write('// JIS X 0208 の区点と Unicode (BMP) の対応表。\n')
out.write('// tools/gen_jis_table.py が Python の文字コードの表 (cp932、euc_jp) から作った (手で直さない)。\n')
out.write('// kJisToUnicode は CGROM にある区 (1-8 区と 16-84 区) の順に、区点ごとの Unicode (なければ 0)。1 字 2 バイトで済むように、\n')
out.write('// Unicode の順の表にはしない (引くときは端から探す。platform/osd_message.h の unicodeToJis())。\n')
out.write('// kJisAltTable は、同じ区点に別の Unicode もある字 (「〜」U+301C と「～」U+FF5E など) の、もう一方の Unicode と区点。\n')
out.write('#pragma once\n\n#include <stdint.h>\n\nnamespace platform {\n\n')
out.write('static constexpr int JIS_TABLE_ROWS = %d; // 1-8 区 (0x21-0x28) と 16-84 区 (0x30-0x74)\n' % len(ROWS))
out.write('inline constexpr uint16_t kJisToUnicode[JIS_TABLE_ROWS][94] = {\n')
for row in ROWS:
    vals = [main.get(row << 8 | col, 0) for col in range(0x21, 0x7f)]
    out.write('    {// 0x%02x\n' % row)
    for i in range(0, 94, 12):
        out.write('     ' + ' '.join('0x%04x,' % v for v in vals[i:i + 12]) + '\n')
    out.write('    },\n')
out.write('};\n')
items = sorted(alt.items())
out.write('static constexpr int JIS_ALT_SIZE = %d;\n' % len(items))
out.write('inline constexpr uint16_t kJisAltTable[JIS_ALT_SIZE][2] = {\n')
for i in range(0, len(items), 6):
    out.write('    ' + ' '.join('{0x%04x, 0x%04x},' % kv for kv in items[i:i + 6]) + '\n')
out.write('};\n\n} // namespace platform\n')
