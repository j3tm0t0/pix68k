"""Human68k のシステムディスク (2HD の XDF) の中身を、SASI のハードディスクのイメージ (HDF) に移す。そのイメージから起動できる。

  python3 -I tools/make_sasi_hdf.py [--size 10|20|40] [--add ファイル[=名前]]... <システムディスク.xdf> <出力.hdf>

イメージの並び (1 レコードは 256 バイト):
  - レコード 0 から 3: ハードディスクの IPL (起動する領域を選ぶプログラム)
  - レコード 4: 領域の表。領域は 1 つだけで、レコード 33 から始まる
  - レコード 33 から: Human68k の領域。セクタは 1024 バイトで、クラスタは 1 セクタ。
    先頭のセクタが領域の IPL (BPB を含む)、続いて FAT (16 ビット) が 2 本、ルートディレクトリ、データ
2 つの IPL のプログラムはこのファイルに持たず、システムディスクの BIN\\FORMAT.X の中から写す。
システムディスクのファイルとディレクトリは全部写す。HUMAN.SYS はルートディレクトリの先頭に置く。
--add のファイルはルートディレクトリに足す (フロッピーディスクに入らない大きなプログラムを動かすため)。
名前 (=名前 で変えられる) は 8.3 の大文字にし、同じ名前がフロッピーディスクにあれば置き換える。

大きさは、SASI のハードディスクのイメージ (HDF) として広く使われている 10MB / 20MB / 40MB のファイルの大きさ
(10441728 / 20748288 / 41496576 バイト) に合わせる。256 バイトのレコードが 1 トラックに 33、ヘッドが 4 本か 8 本、
シリンダが 309 本か 614 本のドライブにあたる。FORMAT.X の装置初期化が 10M / 20M / 40M で書くレコード数も同じ。
--size を省くと 20MB。

終了コード: 0 = 作った、1 = 作れなかった (理由を標準エラーに出す)、2 = システムディスクのファイルを読めなかった。"""
from struct import pack, pack_into, unpack_from
from sys import argv, stderr
from argparse import ArgumentParser, RawDescriptionHelpFormatter

RECORD_BYTES = 256      # SASI のハードディスクの 1 レコード
SECTOR = 1024           # Human68k の領域の 1 セクタ (= 1 クラスタ)
ROOT_ENTRIES = 512      # ルートディレクトリのエントリの数
ENTRY_BYTES = 32        # ディレクトリのエントリ 1 つ
EXIT_OK, EXIT_FAILED, EXIT_NO_DISK = 0, 1, 2  # 終了コード
RESERVED_SECTORS = 1    # 領域の先頭の、IPL のセクタ
FAT_COPIES = 2
ATTR_DIRECTORY = 0x10
FAT16_LAST = 0xFFFF     # FAT16 で、チェーンの最後のクラスタに入れる値

# Human68k 3.02 の FORMAT.X (2.31) で、空のイメージを装置初期化して、装置全体を 1 つの領域に確保したときに、
# FORMAT.X がレコード 4 ($400) に書いた値: 'X68K'、レコード数 (2 つ)、ランプレコード、'Human68k'、開始レコード、
# 領域のレコード数 (10MB は 40750、20MB は 81000、40MB は 162040)
# 下の表は 容量 (MB): (装置のレコード数, ランプレコード, 領域の開始レコード, 領域のレコード数)
DRIVES = {
    10: (40788, 44880, 33, 40750),
    20: (81048, 87648, 33, 81000),
    40: (162096, 175296, 33, 162040),
}
DEFAULT_MB = 20

# FORMAT.X の中で 2 つの IPL を探す手がかり。
# ハードディスクの IPL は、メニューの題の文字列の手前にある分岐命令 ($6000 $00CA) から 4 レコード分。
# 同じ分岐命令で始まる SCSI 用の IPL も FORMAT.X に入っているので、題の文字列で見分ける。
# 領域の IPL は、分岐命令 ($6020) の次に 'Hudson soft 2.00' がある 1 セクタ分。
DISK_IPL_TITLE = b'X68000 HARD DISK IPL MENU'
DISK_IPL_HEAD = bytes.fromhex('600000ca')
DISK_IPL_BYTES = 4 * RECORD_BYTES
PART_IPL_HEAD = bytes.fromhex('6020') + b'Hudson soft 2.00'

# 領域の IPL のオフセット $12 からの BPB (ビッグエンディアン)。
# FORMAT.X の中の IPL では、ここに 1024、1、2、1、512、セクタ数、$F8、FAT のセクタ数、0 (4 バイト) が並んでいる。
# セクタ数と FAT のセクタ数と最後の 4 バイト (領域の開始レコード) を領域に合わせて書き直す。
# 最後の 4 バイトが開始レコードであることは、書き直す前の版が作ったイメージと比べて確かめた。
BPB_AT = 0x12
MAX_DEPTH = 32      # サブディレクトリをたどる深さの上限 (Human68k のパスの長さでは、これより深くならない)
BPB_FORM = '>HBBHHHBBI'
MEDIA_BYTE = 0xF8


class Fat12Disk(object):
    """2HD のフロッピーディスクのイメージ (FAT12) を読む。

    先頭のセクタのオフセット $0B からの BPB (リトルエンディアン。MS-DOS のフロッピーディスクと同じ並び) を使う。
    """

    def __init__(self, image):
        if len(image) < 0x18:
            raise ValueError('ディスクのイメージが短くて BPB を読めない')
        def word(at):   # BPB の 16 ビットの値 (リトルエンディアン)
            return image[at] | image[at + 1] << 8

        sector_bytes = word(0x0B)
        if sector_bytes != SECTOR or image[0x0D] != 1:  # $0D は 1 クラスタのセクタ数
            raise ValueError('1 セクタが 1024 バイトで 1 クラスタが 1 セクタのディスク (2HD) だけを扱える')
        fat_start = word(0x0E) * SECTOR             # 予約セクタのあとに FAT
        fat_length = word(0x16) * SECTOR            # FAT 1 本の大きさ
        self._image = image
        self._root_at = fat_start + image[0x10] * fat_length  # $10 は FAT の本数
        self._root_count = word(0x11)               # ルートディレクトリのエントリの数
        self._data_at = self._root_at + self._root_count * ENTRY_BYTES
        if self._data_at > len(image):
            raise ValueError('ディスクのイメージが短くて FAT とルートディレクトリを読めない')
        # FAT12 は 3 バイトに 2 つの値が入る。先に全部ほどいておく
        packed = image[fat_start:fat_start + fat_length]
        self._links = []
        for at in range(0, len(packed) - 2, 3):
            three = packed[at] | packed[at + 1] << 8 | packed[at + 2] << 16
            self._links += [three & 0xFFF, three >> 12]

    def _contents(self, first):
        """first から始まるクラスタのチェーンの中身をつなげて返す"""
        parts = []
        visited = set()
        cluster = first
        while 2 <= cluster < 0xFF0:
            at = self._data_at + (cluster - 2) * SECTOR
            if cluster in visited or cluster >= len(self._links) or at + SECTOR > len(self._image):
                raise ValueError(f'FAT のチェーンが壊れている (クラスタ {cluster})')
            visited.add(cluster)
            parts.append(self._image[at:at + SECTOR])
            cluster = self._links[cluster]
        return b''.join(parts)

    def _listing(self, table, parents=()):
        """ディレクトリの中身 (バイト列) を (エントリ, 中身) のリストにする。

        parents は、たどってきたディレクトリの最初のクラスタ。壊れたディスクで、子のエントリが自分か祖先を
        指していたら、終わらなくなるので止める。深さも MAX_DEPTH までにする。
        """
        if len(parents) > MAX_DEPTH:
            raise ValueError(f'ディレクトリが深すぎる ({MAX_DEPTH} 段より深い)')
        found = []
        for at in range(0, len(table) - ENTRY_BYTES + 1, ENTRY_BYTES):
            entry = bytes(table[at:at + ENTRY_BYTES])
            if entry[0] == 0x00:    # ここから後ろは使われていない
                break
            if entry[0] == 0xE5:    # 消されたエントリ
                continue
            if entry[0] == 0x2E:    # '.' と '..'
                continue
            first, size = unpack_from('<HI', entry, 26)
            if entry[11] & ATTR_DIRECTORY:
                if first in parents:
                    raise ValueError(f'ディレクトリが自分か上のディレクトリを指している (クラスタ {first})')
                found.append((entry, self._listing(self._contents(first), parents + (first,))))
            else:   # ファイルとボリューム名
                content = self._contents(first)
                if size > len(content):
                    name = entry[0:11].decode('ascii', 'replace')
                    raise ValueError(f'{name}: エントリの大きさ ({size} バイト) の分のクラスタがつながっていない')
                found.append((entry, content[:size]))
        return found

    def tree(self):
        """ディスクの中身を返す。

        (32 バイトのエントリ, 中身) のリストで、並びはディレクトリの中の順。
        中身はファイルなら bytes、ディレクトリなら同じ形のリスト ('.' と '..' は入れない)。
        ボリューム名のエントリも入る (中身は空の bytes)。
        """
        end = self._root_at + self._root_count * ENTRY_BYTES
        return self._listing(self._image[self._root_at:end], (0,))


def find_file(tree, name11):
    """tree() の結果から、エントリの名前 (11 バイト) が name11 のものを探して中身を返す。なければ None を返す (関数の終わりまで進む)。

    並びの順に見て、ディレクトリがあればその中を先に探してから次へ進む。
    名前が合えば、ファイルでなくても (ディレクトリならリストを) 返す。
    """
    for entry, content in tree:
        if entry[0:11] == name11:
            return content
        if isinstance(content, list):
            hit = find_file(content, name11)
            if hit is not None:
                return hit


def dos_name(path):
    """ファイル名を 8.3 の大文字のディレクトリのエントリの名前 (11 バイト) にする"""
    base = path.replace('\\', '/').rsplit('/', 1)[-1].upper()
    stem, _, ext = base.rpartition('.') if '.' in base else (base, '', '')
    if not stem or len(stem) > 8 or len(ext) > 3:
        raise ValueError(f'{base}: 8.3 の名前にできない')
    return (stem.ljust(8) + ext.ljust(3)).encode('ascii')


def fat16_layout(sectors):
    """sectors セクタの領域の、FAT 1 本のセクタ数とクラスタの数を返す。

    FAT が 1 本につき 1 セクタしかないとしたときのクラスタの数 (これより多くはならない) に、
    FAT の先頭の 2 つを足した数の値が入るセクタ数を、FAT 1 本の大きさにする。
    クラスタの数は、残りのセクタの数。
    """
    root_sectors = ROOT_ENTRIES * ENTRY_BYTES // SECTOR
    after_root = sectors - RESERVED_SECTORS - root_sectors
    values = after_root - FAT_COPIES + 2
    fat_sectors = max(1, (values * 2 + SECTOR - 1) // SECTOR)
    return fat_sectors, after_root - FAT_COPIES * fat_sectors


class Fat16Writer(object):
    """Human68k のハードディスクの領域に、FAT16 とディレクトリとファイルを書く。

    img (bytearray) の part_offset バイト目から領域が始まる。先頭の 1 セクタ (IPL) には触らない。
    fat は FAT の値のリスト。先頭の 2 つは $F8FF と $FFFF にしてあり、呼び出し側が書き換えてよい。
    FAT の値はビッグエンディアンで書く (Human68k のハードディスクの FAT16 の並び)。
    """

    def __init__(self, img, part_offset, sectors, fat_sectors, clusters):
        self._img = img
        self._fat_at = part_offset + RESERVED_SECTORS * SECTOR
        self._fat_bytes = fat_sectors * SECTOR
        self._root_at = self._fat_at + FAT_COPIES * self._fat_bytes
        self._data_at = self._root_at + ROOT_ENTRIES * ENTRY_BYTES
        if self._data_at + clusters * SECTOR > part_offset + sectors * SECTOR:
            raise ValueError('FAT とクラスタが領域に入らない')
        self.fat = [MEDIA_BYTE << 8 | 0xFF, FAT16_LAST] + [0] * clusters
        self._unused = 2    # まだ使っていない、いちばん若いクラスタ

    def _take(self, count):
        """続きの count 個のクラスタを 1 つのチェーンにして確保し、最初のクラスタの番号を返す"""
        head = self._unused
        if head + count > len(self.fat):
            raise ValueError('領域の空きクラスタが足りない')
        for cluster in range(head, head + count - 1):
            self.fat[cluster] = cluster + 1
        self.fat[head + count - 1] = FAT16_LAST
        self._unused = head + count
        return head

    def _put(self, first, content):
        at = self._data_at + (first - 2) * SECTOR
        self._img[at:at + len(content)] = content

    def write_dir(self, tree, cluster, root_flag):
        """tree (Fat12Disk.tree() と同じ形のリスト) を 1 つのディレクトリとして書く。

        root_flag が真ならルートディレクトリに書いて 0 を返す。
        偽なら新しいクラスタに書いて、その最初のクラスタの番号を返す。cluster は親のディレクトリのクラスタ
        (親がルートなら 0) で、'..' のエントリに入れる。
        クラスタは、ディレクトリ自身、その中のファイルとディレクトリ (並びの順) の順に、若い番号から使う。
        エントリは先頭の 26 バイトをそのまま写し、クラスタの番号と大きさを入れ直す。
        大きさが 0 のファイルとボリューム名にはクラスタを割り当てない。
        """
        table = bytearray()
        if root_flag:
            if len(tree) > ROOT_ENTRIES:
                raise ValueError(f'ルートディレクトリのエントリは {ROOT_ENTRIES} 個まで')
            here = 0
        else:   # サブディレクトリ。先頭に '.' と '..' を置く
            here = self._take(((len(tree) + 2) * ENTRY_BYTES + SECTOR - 1) // SECTOR)
            for dots, target in ((b'.', here), (b'..', cluster)):
                table += pack('<11sB14xHI', dots.ljust(11), ATTR_DIRECTORY, target, 0)
        for entry, content in tree:
            if isinstance(content, list):
                first, size = self.write_dir(content, here, False), 0
            else:   # ファイルとボリューム名
                size = len(content)
                first = self._take((size + SECTOR - 1) // SECTOR) if size else 0
                if size:
                    self._put(first, content)
            table += entry[0:26] + pack('<HI', first, size)
        if root_flag:
            self._img[self._root_at:self._root_at + len(table)] = table
        else:   # 最初に確保したクラスタに書く
            self._put(here, table)
        return here

    def finish(self) -> None:
        """FAT を 2 本とも img に書く。write_dir() を全部済ませてから呼ぶ"""
        packed = pack(f'>{len(self.fat)}H', *self.fat)
        for copy in range(FAT_COPIES):
            at = self._fat_at + copy * self._fat_bytes
            self._img[at:at + len(packed)] = packed

    def used_clusters(self):
        return self._unused - 2


def take_ipls(format_x):
    """FORMAT.X の中身から、ハードディスクの IPL (4 レコード) と領域の IPL (1 セクタ) を取り出す"""
    title_at = format_x.find(DISK_IPL_TITLE)
    disk_at = format_x.rfind(DISK_IPL_HEAD, 0, max(title_at, 0))
    part_at = format_x.find(PART_IPL_HEAD)
    if disk_at < 0 or part_at < 0:
        raise ValueError('FORMAT.X の中に SASI のハードディスクの IPL が見つからない')
    for start, length in ((disk_at, DISK_IPL_BYTES), (part_at, SECTOR)):
        if start + length > len(format_x):
            raise ValueError(f'FORMAT.X が IPL の途中 (${start:X} から {length} バイト) で終わっている')
    return format_x[disk_at:disk_at + DISK_IPL_BYTES], format_x[part_at:part_at + SECTOR]


def added_files(tree, specs):
    """--add の指定 (ファイル[=名前] のリスト) のファイルを、ルートディレクトリのリスト tree に足したものを返す"""
    for arg in specs:
        path, _, rename = arg.partition('=')
        try:
            with open(path, 'rb') as f:
                body = f.read()
        except OSError as e:
            raise ValueError(str(e))
        name = dos_name(rename or path)
        tree = [t for t in tree if t[0][0:11] != name]
        e = bytearray(32)
        e[0:11] = name
        e[11] = 0x20
        pack_into('<HH', e, 22, 0, (2000 - 1980) << 9 | 1 << 5 | 1)  # 2000-01-01 00:00:00
        tree.append((bytes(e), body))
    return tree


def build_image(floppy, megabytes, specs):
    """システムディスクの中身 floppy から HDF の中身を作る。(イメージ, ルートのエントリの数, 使ったクラスタの数, クラスタの数) を返す"""
    records, lamp_record, first_record, part_records = DRIVES[megabytes]
    tree = Fat12Disk(floppy).tree()
    format_x = find_file(tree, b'FORMAT  X  ')
    if format_x is None:
        raise ValueError('システムディスクに FORMAT.X が入っていない')
    disk_ipl, part_ipl = take_ipls(format_x)
    tree = added_files(tree, specs)
    # HUMAN.SYS をルートディレクトリの先頭に移す。ほかの並びは変えない (書き直す前の版が作ったイメージと同じ並び)
    first = [item for item in tree if item[0][0:11] == b'HUMAN   SYS']
    tree = first + [item for item in tree if item[0][0:11] != b'HUMAN   SYS']

    img = bytearray(records * RECORD_BYTES)
    img[0:DISK_IPL_BYTES] = disk_ipl
    pack_into('>4s3I8s2I', img, 4 * RECORD_BYTES,
                     b'X68K', records, records, lamp_record, b'Human68k', first_record, part_records)

    part_at = first_record * RECORD_BYTES
    sectors = part_records * RECORD_BYTES // SECTOR
    fat_sectors, clusters = fat16_layout(sectors)
    boot = bytearray(part_ipl)
    pack_into(BPB_FORM, boot, BPB_AT, SECTOR, 1, FAT_COPIES, RESERVED_SECTORS, ROOT_ENTRIES,
                     sectors, MEDIA_BYTE, fat_sectors, first_record)
    img[part_at:part_at + SECTOR] = boot
    writer = Fat16Writer(img, part_at, sectors, fat_sectors, clusters)
    writer.write_dir(tree, 0, True)
    writer.finish()
    return img, len(tree), writer.used_clusters(), clusters


def run(argv):
    parser = ArgumentParser(description=__doc__, formatter_class=RawDescriptionHelpFormatter)
    # オプション: 容量 (MB) と、足すファイル (何度でも)。そのあとに入力と出力のファイル
    for flag, settings in (('--size', {'type': int, 'choices': sorted(DRIVES), 'default': DEFAULT_MB,
                                       'metavar': '|'.join(str(mb) for mb in sorted(DRIVES))}),
                           ('--add', {'action': 'append', 'default': [], 'metavar': 'FILE[=NAME]'})):
        parser.add_argument(flag, **settings)
    for name, shown in (('xdf', 'システムディスク.xdf'), ('hdf', '出力.hdf')):
        parser.add_argument(name, metavar=shown)
    opts = parser.parse_args(argv)

    try:
        with open(opts.xdf, 'rb') as src:
            floppy = src.read()
    except OSError as why:
        print(why, file=stderr)
        return EXIT_NO_DISK
    try:
        img, root_count, used, clusters = build_image(floppy, opts.size, opts.add)
        with open(opts.hdf, 'wb') as dst:
            dst.write(img)
    except (ValueError, OSError) as why:
        print(f'{opts.xdf}: {why}', file=stderr)
        return EXIT_FAILED
    # 書き直す前の版と同じ 1 行を標準出力に出す
    print(f'{opts.hdf}: {opts.size}MB, {root_count} entries in root, {used}/{clusters} clusters used')
    return EXIT_OK


if __name__ == "__main__":
    raise SystemExit(run(argv[1:]))
