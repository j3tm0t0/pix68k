#!/bin/sh
# Circle の Pi 4 / Pi 400 の Ethernet (GENET) のドライバ (lib/bcm54213.cpp) を pix68k 用に書き換える (#356)。
#   使い方: tools/circle_genet_patch.sh <元の bcm54213.cpp> <書き出す先>
# ベアメタル版のビルド (tools/build_baremetal.sh) が使う。書き換える行が見つからなければ失敗する (Circle を上げたときに気づく)。
#
# MAC (GENET) と PHY (BCM54213PE) の間は RGMII でつながり、1000Mbps では送信クロックをデータから 2ns ほど遅らせる必要がある。
# 遅らせるのは MAC か PHY のどちらか片方だけ。元の Circle は MAC の遅延を無効にし (EXT_RGMII_OOB_CTRL の bit 16)、
# PHY の遅延も無効のままなので、どちらも遅らせない。Pi 4 (c03111) ではそれでも届くが、Pi 400 (c03130) では送ったフレームの
# 2〜6 割が失われ、DHCP が通らなかった。1 回の起動の中で切り替えて、番号を付けたフレームが届いた数を数えた結果 (150 回ずつ。
# 左から 60 バイト、590 バイト、ブロードキャスト):
#   MAC  PHY   Pi 400          Pi 4
#   なし なし  100/113/56      150/150/150   (元の Circle)
#   あり なし  150/150/149     150/150/145
#   なし あり  150/150/150     150/150/146
#   あり あり  0/0/0           0/0/0
# 届いた数は raspi3 の Wi-Fi で数えた。Wi-Fi のブロードキャストは送り直されないので、ブロードキャストの数個の欠けは Wi-Fi の
# 分を含む。Pi 4 で「なし/なし」と「あり/なし」を 150 回ごとに交互に切り替えて 1500 回ずつ送ると、1500/1500/1473 と
# 1500/1500/1476 で、悪くならない。
# Linux は Pi 4 と Pi 400 のどちらも rgmii-rxid (送信の遅延は MAC、受信の遅延は PHY) で動かす。同じく MAC の遅延を有効にし、
# PHY の送信の遅延は無効にする (両方が有効だと 1 つも届かないので、PHY の側も前の状態に任せずに書く)。
set -eu
[ $# -eq 2 ] || { echo "usage: $0 <bcm54213.cpp> <out.cpp>" >&2; exit 2; }
awk '
  index($0, "int CBcm54213Device::mii_config(bool init)") == 1 { on = 1 }
  on && $0 == "\tu32 id_mode_dis = BIT(16);" {
    print "\tu32 id_mode_dis = BIT(16);\t// pix68k (#356): cleared below, the MAC delays TXC"
    n++; next
  }
  on && $0 == "\treg |= RGMII_MODE_EN | id_mode_dis;" {
    print "\treg &= ~id_mode_dis;"
    print "\treg |= RGMII_MODE_EN;"
    m++; next
  }
  on && $0 == "\treturn 0;" {
    print "\t// pix68k (#356): the PHY must not delay TXC too (shadow register 0x1C, SCR3 (3), GTXCLK delay (bit 9))"
    print "\tmdio_write(0x1C, 3 << 10);"
    print "\tint clk_ctl = mdio_read(0x1C);"
    print "\tif (clk_ctl >= 0)"
    print "\t\tmdio_write(0x1C, 0x8000 | (3 << 10) | (clk_ctl & 0x3FF & ~0x200));"
    print "\telse"
    print "\t\tCLogger::Get ()->Write (FromBcm54213, LogWarning, \"Cannot read the PHY TX clock delay\");"
    print ""
    r++
  }
  on && $0 == "}" { on = 0 }
  { print }
  END { if (n != 1 || m != 1 || r != 1) exit 1 }' "$1" > "$2" ||
  { echo "$1: the lines to patch were not found (mii_config)" >&2; exit 1; }
