#!/usr/bin/env python3
"""ベアメタル版のオブジェクトファイルにある memcpy / memset / memmove / memcmp / strlen の呼び出しと、
アトミックな操作の libgcc の関数 (__aarch64_cas4_acq_rel など。outline atomics) の呼び出しを数える。

  使い方: tools/baremetal_memcalls.py [--prefix aarch64-elf-] [--detail] <.o か .a か ディレクトリ>...

objdump -dr の出力から、これらの関数への再配置 (bl / b) を探す。呼び出しの前の命令をさかのぼり、
長さの引数 (memcpy / memset / memmove / memcmp の x2) に即値を入れる mov があれば「定数」、
なければ「変数」と数える。前の命令を 16 個までしか見ないので、定数を離れた所で入れている呼び出しは
「変数」に入る (目安の数)。
"""
import collections
import os
import re
import subprocess
import sys

FUNCS = ("memcpy", "memset", "memmove", "memcmp", "strlen")
RELOC = re.compile(r"R_AARCH64_(?:CALL26|JUMP26)\s+(\w+)\s*$")
ATOMIC = "atomic"  # __aarch64_* をまとめて数える列
INSN = re.compile(r"^\s*[0-9a-f]+:\s+(\S+)\s*(.*)$")
MOVI = re.compile(r"^[wx]2, #(0x[0-9a-f]+|\d+)")
READ_ONLY = ("cmp", "tst", "cbz", "cbnz", "str", "strb", "strh", "stp", "tbz", "tbnz", "ccmp")


def length_of(prev):
    """前の命令の並び (新しい順) から、x2 に入れた即値を返す。分からなければ None"""
    for op, args in prev:
        if op in ("bl", "blr", "ret", "b", "br"):
            return None
        # x2 / w2 に書く命令か (最初のオペランド。ldp は 2 つ目のオペランドにも書く)
        if not re.match(r"^[wx]2\b", args) and not (op.startswith("ldp") and re.match(r"^[wx]\d+, [wx]2,", args)):
            continue
        m = MOVI.match(args)
        if op == "mov" and m:
            return int(m.group(1), 0)
        if op in READ_ONLY:
            continue  # x2 を読むだけ
        return None
    return None


def scan(path, prefix):
    out = subprocess.run([prefix + "objdump", "-dr", "--no-show-raw-insn", path],
                         capture_output=True, text=True, check=True).stdout
    calls = []
    prev = collections.deque(maxlen=17)
    member = os.path.basename(path)
    for line in out.splitlines():
        m = re.match(r"^(\S+\.o):\s+file format", line)
        if m:
            member = os.path.basename(m.group(1))
            continue
        if line.endswith(":") and line[:1] in "0123456789abcdef":
            prev.clear()
            continue
        m = RELOC.search(line)
        if m:
            if m.group(1).startswith("__aarch64_"):
                calls.append((member, ATOMIC, None))
            elif m.group(1) in FUNCS:
                p = list(prev)[::-1][1:]  # 直前の bl / b そのものを除く
                n = length_of(p) if m.group(1) != "strlen" else None
                calls.append((member, m.group(1), n))
            continue
        m = INSN.match(line)
        if m:
            prev.append((m.group(1), m.group(2)))
    return calls


def main():
    args = sys.argv[1:]
    prefix = "aarch64-elf-"
    detail = False
    paths = []
    while args:
        a = args.pop(0)
        if a == "--prefix":
            prefix = args.pop(0)
        elif a == "--detail":
            detail = True
        else:
            paths.append(a)
    if not paths:
        sys.exit(__doc__)
    files = []
    for p in paths:
        if os.path.isdir(p):
            files += sorted(os.path.join(p, f) for f in os.listdir(p) if f.endswith((".o", ".a")))
        else:
            files.append(p)
    table = collections.OrderedDict()
    sizes = collections.defaultdict(collections.Counter)
    for f in files:
        for member, func, n in scan(f, prefix):
            row = table.setdefault(member, collections.Counter())
            row[(func, "const" if n is not None else "var")] += 1
            if n is not None:
                sizes[func][n] += 1
    cols = [(f, k) for f in FUNCS for k in (("const", "var") if f != "strlen" else ("var",))]
    cols.append((ATOMIC, "var"))
    head = "".join("%10s" % (f if f == ATOMIC else "%s:%s" % (f[3:] if f.startswith("mem") else f, "定" if k == "const" else "変")) for f, k in cols)
    print("%-28s" % "object" + head + "%8s" % "total")
    total = collections.Counter()
    for member, row in table.items():
        total.update(row)
        if detail:
            print("%-28s" % member + "".join("%11d" % row[c] for c in cols) + "%8d" % sum(row.values()))
    print("%-28s" % "TOTAL" + "".join("%11d" % total[c] for c in cols) + "%8d" % sum(total.values()))
    for f in FUNCS:
        if sizes[f]:
            small = sum(c for n, c in sizes[f].items() if n < 128)
            print("%s の定数の長さ: 128 未満 %d か所、128 以上 %d か所 (長さ:か所 %s)" % (
                f, small, sum(sizes[f].values()) - small,
                ", ".join("%d:%d" % (n, c) for n, c in sorted(sizes[f].items()))))


if __name__ == "__main__":
    main()
