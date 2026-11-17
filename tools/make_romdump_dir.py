"""X68000 のエミュレータの ROM ($FE0000-$FFFFFF と $FC0000-$FDFFFF) をファイルに書き出す Human68k の起動ディレクトリを作る。

  python3 -I tools/make_romdump_dir.py <Human68k のシステムディスク.xdf> <出力ディレクトリ>

出力ディレクトリには HUMAN.SYS と COMMAND.X (システムディスクから写す)、CONFIG.SYS、AUTOEXEC.BAT、ROMDUMP.X を置く。
このディレクトリをホストのディレクトリとして起動ドライブにつなぐと (XEiJ なら -boot=hf0 -hf0=<出力ディレクトリ>)、
AUTOEXEC.BAT が ROMDUMP.X を動かし、同じディレクトリに IPLROM.DAT ($FE0000 から 128KB) と ROM30.DAT
($FC0000 から 128KB) を書いて "ROMDUMP OK" と出す。

060turbo の IPL ROM を、XEiJ の IPLROM 1.6 から取り出すために使う (README の「060turbo」を参照)。
ROMDUMP.X の中身 (152 バイト) は下の CODE で、次のアセンブリを組み立てたもの:

    start:  clr.l   -(sp)               ; _SUPER (スーパーバイザモードにする)
            dc.w    $ff20
            addq.l  #4,sp
            lea     name1(pc),a0        ; "IPLROM.DAT" に $FE0000 から
            move.l  #$fe0000,d1
            bsr     dump
            lea     name2(pc),a0        ; "ROM30.DAT" に $FC0000 から
            move.l  #$fc0000,d1
            bsr     dump
            pea     done(pc)            ; _PRINT "ROMDUMP OK"
            dc.w    $ff09
            addq.l  #4,sp
            dc.w    $ff00               ; _EXIT
    dump:   move.l  d1,-(sp)
            move.w  #$20,-(sp)          ; _CREATE (属性 $20)
            move.l  a0,-(sp)
            dc.w    $ff3c
            addq.l  #6,sp
            move.l  (sp)+,d1
            tst.l   d0
            bmi     fail
            move.w  d0,d7
            move.l  #$20000,-(sp)       ; _WRITE 128KB
            move.l  d1,-(sp)
            move.w  d7,-(sp)
            dc.w    $ff40
            lea     10(sp),sp
            move.w  d7,-(sp)            ; _CLOSE
            dc.w    $ff3e
            addq.l  #2,sp
            rts
    fail:   pea     err(pc)             ; _PRINT "ROMDUMP FAILED"
            dc.w    $ff09
            addq.l  #4,sp
            dc.w    $ff00
    name1:  dc.b    'IPLROM.DAT',0
    name2:  dc.b    'ROM30.DAT',0
    done:   dc.b    'ROMDUMP OK',13,10,0
    err:    dc.b    'ROMDUMP FAILED',13,10,0
            even
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_sasi_hdf import Fat12Disk, find_file  # noqa: E402

CODE = bytes.fromhex(
    '42a7ff20588f41fa005c223c00fe00006100001a41fa0059223c00fc00006100000c487a0055ff09588fff002f013f3c'
    '00202f08ff3c5c8f221f4a806b00001c3e002f3c000200002f013f07ff404fef000a3f07ff3e548f4e75487a002aff09'
    '588fff0049504c524f4d2e44415400524f4d33302e44415400524f4d44554d50204f4b0d0a00524f4d44554d50204641'
    '494c45440d0a0000')


def x_file(text):
    """テキストだけの X 形式の実行ファイル (再配置なし)"""
    text += b'\0' * (len(text) & 1)
    return b'HU\0\0' + struct.pack('>IIIIIII', 0, 0, len(text), 0, 0, 0, 0) + b'\0' * 32 + text


def main():
    if len(sys.argv) != 3:
        print(__doc__.split('\n\n')[1], file=sys.stderr)
        return 2
    disk = sys.argv[1]
    try:    # ないファイルも読めないファイルも、引数の誤りと同じ 2 で終わる
        with open(disk, 'rb') as f:
            data = f.read()
    except OSError as why:
        print(why, file=sys.stderr)
        return 2
    tree = Fat12Disk(data).tree()
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for name, key in (('HUMAN.SYS', b'HUMAN   SYS'), ('COMMAND.X', b'COMMAND X  ')):
        body = find_file(tree, key)
        if body is None:
            print(f'{name} がシステムディスクにない', file=sys.stderr)
            return 1
        open(os.path.join(out, name), 'wb').write(body)
    open(os.path.join(out, 'CONFIG.SYS'), 'wb').write(b'FILES = 20\r\nBUFFERS = 10\r\n')
    open(os.path.join(out, 'AUTOEXEC.BAT'), 'wb').write(b'ROMDUMP\r\n')
    open(os.path.join(out, 'ROMDUMP.X'), 'wb').write(x_file(CODE))
    return 0


if __name__ == '__main__':
    sys.exit(main())
