"""Human68k と 060turbo.sys を入れた、起動できる SCSI のハードディスクのイメージ (HDS、8MB) を作る (試験用)。
  make_scsi_hds.py <リポジトリの tools> <HUMAN302.XDF> <out.hds> <060turbo.sys> [060turbo.sys のオプション]
HUMAN.SYS、COMMAND.X、SCSI の IPL、SCSI のデバイスドライバ (FORMAT.X の中のもの) は HUMAN302.XDF から取り出す。
CONFIG.SYS は DEVICE = A:\\060TURBO.SYS <オプション> と SHELL = A:\\COMMAND.X だけ。
どのファイルもリポジトリに入れない (tests/x68k/boot_060turbo_scsi.sh が一時ディレクトリに作る)。
"""
import struct
import sys

sys.path.insert(0, sys.argv[1])
import make_sasi_hdf as m  # noqa: E402

xdf, out, turbo = sys.argv[2:5]
options = sys.argv[5] if len(sys.argv) > 5 else ''
fd = m.Fat12Disk(open(xdf, 'rb').read())
src = fd.tree()
fmt = m.find_file(src, b'FORMAT  X  ')
body = fmt[0x40:]
# FORMAT.X の中の、SCSI の IPL (メニュー)、SCSI のデバイスドライバ、パーティションの IPL
ipl_at = body.find(b'X68000 SCSI DISK IPL MENU')
ipl = body.rfind(b'\x60\x00\x00\xca', 0, ipl_at)
drv = body.find(b'\xff\xff\xff\xff\x40\x00')
while drv >= 0 and body[drv + 14:drv + 22] != b'\x01SCHDISK':
    drv = body.find(b'\xff\xff\xff\xff\x40\x00', drv + 1)
part_ipl_at = body.find(b'\x60\x24SHARP/KG')
if ipl <= 0 or drv <= 0 or part_ipl_at < 0:
    raise SystemExit('FORMAT.X の中に SCSI の IPL かドライバが見つからない')
scsi_ipl = body[ipl:ipl + 1000]
driver = body[drv:drv + 0x3358]
part_ipl = body[part_ipl_at:part_ipl_at + 0x2c6] + bytes(0x400 - 0x2c6)


def ent(name, attr=0x20):
    e = bytearray(32)
    e[0:11] = name
    e[11] = attr
    struct.pack_into('<HH', e, 22, 0, (2000 - 1980) << 9 | 1 << 5 | 1)
    return bytes(e)


def keep(name):
    for e, b in src:
        if e[0:11] == name:
            return (e, b)
    raise SystemExit(name.decode() + ' が HUMAN302.XDF にない')


cfg = 'FILES = 30\r\nBUFFERS = 20 1024\r\nLASTDRIVE = Z:\r\n'
cfg += 'DEVICE = A:\\060TURBO.SYS %s\r\n' % options
cfg += 'SHELL = A:\\COMMAND.X\r\n'
tree = [keep(b'HUMAN   SYS'), keep(b'COMMAND X  '), (ent(b'CONFIG  SYS'), cfg.encode()),
        (ent(b'060TURBOSYS'), open(turbo, 'rb').read())]

SIZE_KB = 8 * 1024
img = bytearray(SIZE_KB * 1024)
hdr = bytearray(0x400)
hdr[0:8] = b'X68SCSI1'
struct.pack_into('>HI', hdr, 8, 512, SIZE_KB * 2)
hdr[14] = 1
s = b'Human68K SCSI-DISK by Keisoku Giken'
hdr[16:16 + len(s)] = s
img[0:0x400] = hdr
img[0x400:0x400 + len(scsi_ipl)] = scsi_ipl
PSTART_KB = 0x20
plen = SIZE_KB - PSTART_KB
pt = bytearray(0x400)
struct.pack_into('>4sIII', pt, 0, b'X68K', SIZE_KB, SIZE_KB, SIZE_KB)
struct.pack_into('>8sII', pt, 0x10, b'Human68k', PSTART_KB, plen)
img[0x800:0xC00] = pt
img[0xC00:0xC00 + len(driver)] = driver
part = PSTART_KB * 1024
sectors = plen
# FAT 1 本のセクタ数とクラスタの数は、SASI のイメージと同じ決め方にする
fat, clusters = m.fat16_layout(sectors)
p = bytearray(part_ipl)
struct.pack_into('>HBBHHHBBII', p, 0x12, m.SECTOR, 1, 2, 1, m.ROOT_ENTRIES, 0, 0xF7, fat, sectors, PSTART_KB)
img[part:part + 0x400] = p
w = m.Fat16Writer(img, part, sectors, fat, clusters)
w.fat[0] = 0xF7FF
w.fat[1] = 0xFF00
w.write_dir(tree, 0, True)
w.finish()
open(out, 'wb').write(img)
