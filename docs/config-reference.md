# 設定の一覧

SD カードに置くファイル、pix68k.cfg のキー、cmdline.txt のオプションの詳しい説明。キーの短い一覧は [README](../README.md#pix68kcfg) の表にある。

## SD カードに置くファイル

ベアメタル版は、boot パーティション (か USB メモリ) の `/pix68k/` (ROM はルートでもよい) から次のファイルを読む。`pix68k.cfg` の行は、キーの説明をまとめて書いたもの。

| ファイル | 内容 |
| --- | --- |
| `IPLROM.DAT` | IPL ROM (128KB)。なければ X68000 は動かさず、[baremetal.md](baremetal.md) の M0 の動作をする |
| `IPLROM30.DAT` | X68030 の IPL ROM (128KB)。`pix68k.cfg` で `cpu=68030` のとき、`cpu=68060` で `IPLROM060.DAT` がないときに読む |
| `IPLROM060.DAT` | 060turbo の IPL ROM (128KB、なくてよい)。`cpu=68060` のときに先に探す ([060turbo.md](060turbo.md) を参照) |
| `CGROM.DAT` | CGROM (768KB)。なければ文字が出ない |
| `fd0.xdf`、`fd1.xdf` | フロッピーディスクのイメージ (`.dim`、`.d88` も可)。書き込みはファイルに書き戻す ([disks-and-scsi.md](disks-and-scsi.md) の「ディスクの書き戻しと大きなイメージ (ベアメタル版)」を参照) |
| `hd0.hdf` | SASI ID 0 のハードディスクのイメージ (HDF)。48MB まで (SASI の 40MB のドライブが入る)。書き込みはファイルに書き戻す |
| `SCSIEXROM.DAT` | 外付け SCSI ボードの ROM (なくてよい)。`pix68k.cfg` の `scsi_ex=on` のときに読む ([disks-and-scsi.md](disks-and-scsi.md) の「増設メモリ、外付け SCSI ボード、CD-ROM」を参照) |
| `pix68k.cfg` | 設定 (なくてよい)。`fd0=HUMAN302.XDF`、`hd0=HUMAN302.HDF` のように、ディスクのイメージの名前を指定する (`/` で始まらなければ `/pix68k` から)。`clock=10` で CPU を 10MHz にする (既定は `clock=16`、XVI)。`cpu=68030` で X68030 にする (IPL ROM は `IPLROM30.DAT`、SRAM は `SRAM30.DAT` / `SRAM30.BAK` を使う。`high_mem=16` で 16MB のハイメモリを置く)。`cpu=68060` で 060turbo にする (SRAM は `SRAM060.DAT` / `SRAM060.BAK`、`local_mem=` でローカルメモリの容量 (MB、0 から 256、既定 128)、`clock=75` で 68060 を 75MHz にする (既定は 50MHz。[060turbo.md](060turbo.md) の「060turbo (68060)」を参照))。`max_speed=on` で最高速モード (68000 でも使える。[060turbo.md](060turbo.md) の「最高速モード」を参照)。`speed060=` で 060turbo の命令の速さ (選んだクロックの実機の 68060 を 100 とした %、既定 100)、`max_speed_limit=` で 060turbo の最高速モードの上限 ([060turbo.md](060turbo.md) の「68060 のサイクル数」と「最高速モード」を参照)。`sram_memsize=keep` で SRAM に書かれたメインメモリの容量をそのまま使う (既定の `fix` では `ram=` の容量に合わせる)。`ram=8` で本体のメモリを 8MB にする (1 から 12)。`fpu_board=on` で数値演算プロセッサボード (`fpu_board=2` で 2 枚)、`mercury=on` でまーきゅりーゆにっとをつなぐ ([expansion.md](expansion.md) の「数値演算プロセッサボード、まーきゅりーゆにっと」を参照)。`neptune=on` でネットワークボード Neptune-X をつなぐ (`neptune_mac=`、`neptune_link=`。[expansion.md](expansion.md) の「ネットワークボード Neptune-X」を参照)。`rs232c=tcp:2324` で RS-232C を TCP の 2324 番につなぐ (`net=on` が要る。[expansion.md](expansion.md) の「RS-232C (SCC のチャネル A)」を参照)。`midi=on` で MIDI ボード CZ-6BM1 をつなぐ (`midi_sf2=` で SoundFont、`midi_out=`、`midi_voices=`、`midi_out=mt32` のときの `midi_mt32_ctrl=` と `midi_mt32_pcm=`、`midi_out=sc55` のときの `midi_sc55=`、`midi_out=uart` のときの `midi_uart=` と `midi_uart_family=`。[midi.md](midi.md) の「MIDI ボード CZ-6BM1 (YM3802)」を参照)。`net=on` で Pi の Ethernet に HTTP のサーバと syslog の送信を動かす (`net_ip=`、`net_token=`、`syslog=` など。[network-api.md](network-api.md) を参照)。`scsi_ex=on` で外付け SCSI ボードをつなぎ (ROM は `SCSIEXROM.DAT`。`scsi_ex_rom=` でファイル名を変えられる)、`sc0=HUMAN.HDS` から `sc6=` で SCSI のハードディスク (HDS)、`cd=GAME.ISO` か `cd=GAME.CUE` で CD-ROM (`cd_id=` で ID、既定 6) をつなぐ (外付け SCSI ボードがなければ X68030 / 060turbo の内蔵 SCSI。イメージは使うチャンクだけを読む。書き込みはファイルに書き戻す。[disks-and-scsi.md](disks-and-scsi.md) の「増設メモリ、外付け SCSI ボード、CD-ROM」と「ディスクの書き戻しと大きなイメージ (ベアメタル版)」を参照)。`write_back=off` でディスクの書き戻しをやめ、`write_protect=fd1,sc0` で書き込み禁止にするディスクを並べる。`keyboard=` でキーボードの種類を選ぶ (既定の `auto` ではつないだキーボードの VID:PID から決める。`usb` は普通の USB キーボード、`pi400us` / `pi400jp` は Raspberry Pi 400 の英語版 / 日本語版。[keyboard.md](keyboard.md) の「Pi 400 のキーボード」を参照)。`hat=0` / `hat=1` でゲームパッドのハットの値の範囲を 0-7 / 1-8 に固定する (既定の `auto` ではレポート記述子か値から決める。[baremetal.md](baremetal.md) を参照)。`scale=off` で X68000 の画面を拡大しない (既定の `auto` では整数倍に拡大する。[baremetal.md](baremetal.md) を参照)。`scanline=on` で縦 2 倍以上に拡大したときにブラウン管のスキャンラインのように行を暗くする (既定は `off`。[baremetal.md](baremetal.md) を参照)。画面の右の余白にフレームの数、速さ、温度などを出す (既定の `osd=on`。`osd=fps` なら余白がないときに X68000 の画面に重ね、`osd=off` で出さない。[baremetal.md](baremetal.md) を参照)。`render_core=off` で画面の合成も、`sound_core=off` で音の合成もコア 1 で行う (既定はどちらも `on`。コア 2 / コア 3 が 1 秒以内に動き出さなければ、`on` でもコア 1 で行う。音を出さない設定なら、`sound_core=on` でもコア 3 は使わない) |
| `SRAM.DAT`、`SRAM.BAK` | SRAM。起動時に読み、書き換えられたら書き戻す (5 秒に 1 回まで)。電源投入のたびに、SRAM に書かれたメインメモリの容量 ($ED0008) をエミュレータのメモリの容量に合わせる (XEiJ の説明書 parameter.htm の modifymemorysize と同じ働き。`pix68k.cfg` の `sram_memsize=keep` で止められる)。書いている途中で電源が切れても前の内容が残るよう、2 つのファイルに世代番号と CRC を付けて交互に書き、正しくて新しいほうを読む (形式は `src/x68k/sram_file.h`。先頭の 16KB は SRAM そのもので、16KB だけの SRAM.DAT も読める) |

## pix68k.cfg の書き方

pix68k.cfg のキーは `src/platform/settings.h` の `kSettingKeys` の表に 1 か所にまとめてある (ベアメタル版、ホスト版の `--config`、設定メニューが同じ表を使う)。
fd0 / fd1 / hd0 / sc0〜sc6 / cd / scsi_ex_rom はファイル名 (`/` で始まらなければ `/pix68k/` から)。
clock (68000 では 10、16。68060 では 50、75)、cpu (68000、68030、68060)、ram (1〜12)、high_mem (0〜16)、local_mem (0〜256)、cd_id (0〜6)、speed060 (5〜3200)、max_speed_limit (0〜3200) は数。
max_speed、render_core、sound_core、scsi_ex、write_back は on / off。write_protect はディスクの名前 (fd0 fd1 hd0 sc0〜sc6) を「,」か空白で区切って並べる。sram_memsize は fix / keep、hat は auto / 0 / 1、keyboard は auto / usb / pi400us / pi400jp。
知らないキーと正しくない値は、ログに警告を出して無視する。数のあとに文字が付いた値 (`high_mem=8MB` など) も正しくない値として無視する (前は数の部分だけを読んでいた)。
`fd0=` / `fd1=` を空で書くと、ドライブを空にする (`fd0.xdf` などを探さない。メニューで取り出してから「Save settings」を選ぶとこう書く)。ほかのファイル名のキー (`hd0=`、`sc0=` など) は、空なら指定しなかったのと同じ。

## cmdline.txt のオプション

boot パーティションのルートの `cmdline.txt` (1 行) に、空白で区切って書く。Circle が読むもの (`width=` など) と、pix68k が読む `pix68k_` で始まるものがある。

| オプション | 内容 | 既定 | 詳しい説明 |
| --- | --- | --- | --- |
| `width=768 height=512` | フレームバッファの大きさ (Circle のオプション) | HDMI の解像度 | [baremetal.md](baremetal.md) |
| `pix68k_sound=` | 音の出し先。`pwm` (3.5mm のジャック、Raspberry Pi 5 は GPIO 12 / 13)、`i2s` (I2S の DAC)、`off`。`null` は試験用 | HDMI (QEMU 用のビルドは `off`) | [baremetal.md](baremetal.md) |
| `pix68k_i2s_dac=` | I2S の DAC の設定のしかた。`auto`、`none`、I2C の番地 (`0x4c` など) | `auto` | [baremetal.md](baremetal.md) の「I2S の DAC」 |
| `pix68k_fan=` | Pi 5 のファン。`auto` (温度で 4 段階)、`full` (いつも全開)、`off` (触らない) | `auto` | [baremetal.md](baremetal.md) |
| `pix68k_drive=` | `/pix68k` を読むドライブ。`auto`、`usb`、`sd`、`nvme` | `auto` | [baremetal.md](baremetal.md) の「USB メモリから読み書きする」 |
| `pix68k_id=` | USB メモリを選ぶとき、`/pix68k/id.txt` の値がこれと同じものを使う | なし | 同上 |
| `pix68k_usb_wait=` | USB メモリが出てくるのを待つ上限 (ミリ秒) | `2000` | 同上 |
| `pix68k_gpu_scale=` | `2` でフレームバッファを縦横 1/2 にして、2 倍の拡大を GPU に任せる。`1x2` で縦だけ | 使わない | [baremetal.md](baremetal.md) |
| `pix68k_fb_rows=0` | 試験用。変わっていない行も毎回フレームバッファに書く (書く行を減らす前の動きと比べるため) | | [baremetal.md](baremetal.md) |
| `pix68k_wfe=0` | 試験用。コア 0 とコア 1 が待つ間、wfe で眠らせずに空回りで待つ (温度や速さを比べるため) | | [baremetal.md](baremetal.md) |
| `pix68k_net=` などの `pix68k_` + ネットワークのキー | `pix68k.cfg` の `net`、`net_ip`、`net_gw`、`net_dns`、`net_name`、`net_allow`、`net_token`、`syslog` と同じ。`cmdline.txt` のほうを使う (`rs232c` は読まない) | | [network-api.md](network-api.md) |
| `pix68k_textdump=N` | 試験用。N 秒ごとにテキスト画面を読み、変わっていればログに出す | | [baremetal.md](baremetal.md) |
| `pix68k_fail_core=N` | 試験用。N 番以降のコアが起動しなかったことにする | | [baremetal.md](baremetal.md) |
| `pix68k_net_probe=1` | 試験用。Neptune-X (`neptune_link=eth`) のリンクがつながったら、X68000 の MAC アドレスから ARP の問い合わせを出し、答えが届いたらログに出す (`baremetal/net_bridge.h`) | | |
| `pix68k_stream_min=0` | 試験用。SASI のイメージを全体を読まずにチャンクだけ読む (`tools/qemu_human68k.sh` の `STREAM=1`) | | [baremetal.md](baremetal.md) |
| `pix68k_deploy=秒数` | `tools/deploy_pi.sh` が使う。指定の秒数動いたらログを書いて再起動する | | [disks-and-scsi.md](disks-and-scsi.md)、[baremetal.md](baremetal.md) |

## ホスト版のコマンドラインのオプション

ホスト版の `pix68k` のオプションの一覧は `host/main.cpp` の先頭の注釈にある。よく使うものは [README](../README.md#ホスト版のオプション) に、
音、RTC の時刻、入力のオプションは [build-and-test.md](build-and-test.md) にある。`--config FILE` で `pix68k.cfg` と同じ形式の設定ファイルを読める
(コマンドラインのほうが優先する。README の表の「ホスト版」の列が「-」のキーは、ホスト版では黙って無視する)。
