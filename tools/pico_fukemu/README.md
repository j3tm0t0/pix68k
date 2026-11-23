# pico_fukemu: ふろっぴU君 (FUK) の偽物の Pico 2 のファーム

TNB製作所の「ふろっぴU君」(以下 FUK) は、X68000 の FDD を USB でエミュレータにつなぐ装置です。pix68k は FUK を使えます (#295、`docs/fuk.md`)。このファームは、Raspberry Pi Pico 2 (RP2350) を FUK に見せかけます。本物の FDD の代わりに、Pico 2 のフラッシュに置いた 2HD のディスクイメージ (FD0 と FD1 の 2 枚) を読み書きします。

作った目的は #320 (Read のデータを、届いた時刻の間隔を保って渡すこと) の試験です。このファームは、実際の挙動に忠実にすることを目指して、設定で選んだ Read を、FDD の回転から計算した時刻に答えます (実物ではまだ確かめていません)。ほかの Read にはすぐに答えます。

既定 (`stream`) では、データをディスクから流れてくる速さで 1 バイトずつ送ります。pix68k は、DMA を始めてから、届いたデータに合わせて DMAC の転送バイト数 (MTC) を減らします。`--stretch` は、セクタのデータが流れる時間を、指定した割合だけ長くする試験用の設定です。

## USB の見え方

本物の FUK と同じにしています。pix68k は、本物のときと同じ経路で使います (`fuk=on`、ホスト版の `--fuk`)。pix68k の FUK への対応は `docs/fuk.md` に書きました。

| 項目 | 値 |
|---|---|
| VID:PID | `04D8:E6B1` |
| 製造者名 / 製品名 | `TNB Factory.` / `FDC emulator` (シリアル番号なし) |
| インターフェース | CDC を 2 本 (0/1 と 2/3)。CDC 0 がポート 1、CDC 1 がポート 2 |

ポートのバイトの形式とレジスタの対応は `docs/fuk.md` の「FUK とのやり取り」と同じです。そのほか、次のことをします。

- どちらかのポートを 1200 bps で開くと、BOOTSEL (UF2 の書き込み) に入ります。2 回目からは、ボタンを押さずにファームやイメージを書き直せます。
- pix68k を閉じてからポート 2 に `'L'` を送ると、RAM に溜めた動作の記録 (実時間の Read の予定の時刻と、USB の送るバッファに渡し終えた時刻など) を返します。終わりに `0x00` が付きます。

## ビルド

pico-sdk 2.1 以降を使います (無線 LAN なしのビルド。OTA のビルドは 2.3.1 以降で、cmake が確かめる。試したのは 2.3.1 だけ)。Mac では Homebrew の `arm-none-eabi-gcc` に C ライブラリが入っていないので、Arm GNU Toolchain を `PICO_TOOLCHAIN_PATH` で指します。

```
export PICO_SDK_PATH=~/pico/pico-sdk
export PICO_TOOLCHAIN_PATH=~/opt/arm-gnu-toolchain-14.3.rel1-darwin-arm64-arm-none-eabi
cmake -S tools/pico_fukemu -B build/pico_fukemu -DPICO_BOARD=pico2
cmake --build build/pico_fukemu -j8          # build/pico_fukemu/pico_fukemu.uf2
```

イメージはファームに入れません。無線 LAN なしのファームの UF2 は、配っても困るものを含みません。

## 無線 LAN (電源が入っているかの確認と状態の問い合わせ)

Pico 2 W で、Wi-Fi につないで `pix68k-fukemu.local` (mDNS) と ping に答えるようにできます。pix68k の側で FUK が USB に見えないとき、Pico が動いているか、USB がホストにつながっているか、最後の Read がいつだったかを、ネットワークから確かめるために使います。

```
cmake -S tools/pico_fukemu -B build/pico_fukemu_w -DPICO_BOARD=pico2_w \
      -DPICO_FUKEMU_WIFI_FILE=<SSID とパスワードのファイル> -DPICO_FUKEMU_TOKEN_FILE=<トークンのファイル>
cmake --build build/pico_fukemu_w -j8        # build/pico_fukemu_w/pico_fukemu.uf2
```

- Wi-Fi のファイルは 1 行目が SSID、2 行目がパスワード、トークンのファイルは 1 行目がトークン (8 文字以上) です。書式とつなぎ方は `tools/pico_kbd` と同じで、`tools/pico_kbd/net.c` をそのまま使います。
- SSID・パスワード・トークンは、ビルド先の生成物 (`secrets/pico_kbd_secrets.h`、`.o`、ELF、BIN、UF2) に入ります。ビルド先と、この UF2 は人に渡さず、コミットもしないでください。
- TCP は暗号化していません。トークンも、OTA で送る `.bin` (SSID・パスワード・トークンが入っている) も、LAN の上を平文で流れます。信用できない相手のいる LAN では使わないでください。
- ホスト名は `PICO_FUKEMU_HOSTNAME` (既定 `pix68k-fukemu`)、TCP のポートは `PICO_FUKEMU_TCP_PORT` (既定 2323) で変えられます。
- 無線 LAN の処理 (無線のチップの初期化、`cyw43_arch_poll`、lwIP) はコア 1 だけで回します。USB と FDC はコア 0 で、コア 0 が無線 LAN の処理の終わりを待つことはありません (フラッシュの読み出しやバスは両方のコアで共用します)。
- コア 0 とコア 1 の間は、`S` の数と時刻と USB の状態の写し (コア 0 が主ループで写す)、動作の記録を渡すリング、フラッシュの書き込みの頼みだけでやりとりします。
- 例外: コア 1 の `sleep_ms` など (無線のチップの初期化、つなぎ直し) のタイマーの割り込みは、SDK の既定のアラームプールがコア 0 にあるので、コア 0 に入ります (1 回数 µs)。
- 送る時刻を決める自前の関数 (`fukemu_poll`、`flush_ctl`、`fukemu_next_due`、`out2`、`now_us`) は RAM に置きます (`__time_critical_func`)。コア 1 の無線 LAN の処理と XIP のキャッシュを取り合って送る時刻が揺れるのを、減らすためです。主ループと TinyUSB はフラッシュにあるので、揺れがなくなるわけではありません (下の「実機で最初に確かめること」で測る)。
- コア 1 のスタックは 8KB を普通の RAM に取ります (`s_core1_stack`。SDK の既定の 2KB では lwIP などに足りないため)。
- 無線 LAN のビルドでは、フラッシュを書くのはコア 1 だけです。X68000 からの書き込み (WRITE DATA、WRITE ID) も、コア 0 がコア 1 に頼んで待ちます。書くあいだは、コア 1 が `flash_safe_execute` でコア 0 を止めます (止める側をコア 1 に決めておくと、両方のコアが止め合うことがありません)。コア 1 が 5 秒答えないと、X68000 には異常終了を返しますが、頼んだ書き込みは取り消せないので、あとでフラッシュに入ることがあります (その間、次の書き込みは断ります)。
- 設定とイメージは、フラッシュの物理的な場所を、アドレスの変換を通らない窓 (`XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE`、キャッシュなし) から読みます。下の OTA で A/B のパーティションから起動すると、ブート ROM が `XIP_BASE` をそのパーティションに付け替えるためです。

TCP の使い方は `nc pix68k-fukemu.local 2323` でつなぎ、1 行目にトークンを送ります。`#pico auth ok` が返ったら、1 行ずつ命令を送ります。

| 命令 | 返すもの |
|---|---|
| `S` | 版 (ビルドしたときの git のコミット)、設定、イメージの有無、USB の状態、Read と書き込みの数、最後の Read |
| `N` | Wi-Fi の状態 (IP アドレス、つないだ時刻など) |
| `R` | 再起動する |
| `H` | 命令の一覧 |
| `O ...` | ファームを書き換える (OTA のビルドだけ。下の「無線 LAN 越しの更新」) |
| `W ...` | 設定とディスクイメージを書く (OTA のビルドだけ) |

`S` の例:

```
#pico fukemu version=1234abc up-ms=81234
#pico config source=flash delivery=stream rule=pair one-sector=0 stretches=1
#pico stretch U1 C1 H0 R8 ppm=23000
#pico image fd0=present fd1=present
#pico usb mounted=1 suspended=0 dtr1=1 dtr2=1 rx1-ago-ms=120 rx2-ago-ms=118
#pico count reads=37 rt-reads=6 writes=0 resets=1
#pico last-read U1 C1 H0 R8-8 st0=01 rt=1 at-ms=80012 ago-ms=1222 result-us=17533 queued-us=17534
#pico ok
```

- `usb mounted` はホストが USB の設定まで済ませたか、`dtr1` / `dtr2` は USB が使える状態でホストが DTR を立てているか (ポートを開くと立つ。サスペンド中は 0)、`rx1-ago-ms` / `rx2-ago-ms` は最後にバイトを受けてからの時間です。
- `last-read` の `at-ms` は起動からの時刻、`result-us` はコマンドを受けてからリザルトを送る予定の時刻まで、`queued-us` はリザルトを USB の送るバッファに渡し終えるまでの時間です。`rt=1` は実時間で答えた Read です。リザルトを送る前にリセットされた Read は `queued-us=aborted` になります。
- コア 0 が 0.2 秒答えないとき (`'L'` で記録を送っている途中など) は、`usb busy` を返して USB の状態、数、最後の Read を省きます。
- 認証を通ったあとは、動作の記録 (`'L'` で返すものと同じ) も、行の先頭に `> ` を付けて流します。送りきれない分は捨てます。

## 無線 LAN 越しの更新 (OTA)

Pico 2 W を USB につなぎ直さずに、ファーム、設定、ディスクイメージを書き換えます。どれもトークンを通った TCP の相手だけが使えます。

- ファーム: `tools/pico_kbd` の仕組み (`ota.c`、`ota_rp2350.c`、`picocmd.py --ota`) をそのまま使います。RP2350 のブート ROM の A/B パーティションと「試してから確定」(TBYB) で、新しいファームは動いていない側のパーティションに書きます。送っている途中で切れても、今の版がそのまま動きます。書き終えると再起動して新しい版を試し、TCP でトークンを通った相手が来たときに確定します。約 16.7 秒たっても確定しなければ、ブート ROM が前の版に戻します。仕組みの詳しいことは `tools/pico_kbd/ota.c` の先頭と `tools/pico_kbd/README.md` にあります。
- 設定とディスクイメージ: `W` 命令 (`netwrite.c`) と `fukemu_net.py` で書きます。書ける場所は `layout.h` の設定の場所と FD0 / FD1 の場所だけです。イメージは、まず動いている FDC からそのドライブを抜いて (ディスクなし。FDC のコマンドの途中なら、終わるまで待つ)、フラッシュの見出しを消し、データを書いて読み返し、CRC が合ったら最後に見出しを書きます。途中で切れたときは、そのドライブがディスクなしのままになります (`fukemu_net.py` で送り直すか、`mkimg.py` の UF2 を BOOTSEL で書けば戻ります)。設定は 4KB をためてから 1 回で書きます。ただし、消して書く数十ミリ秒のあいだに電源が切れると、設定がなくなるか、壊れることがあります (設定に CRC は付けていません)。起動のときは見出し (magic) と値の範囲だけを確かめ、合わなければ既定の設定 (stream、pair、FD1 の C1 H0 R8 +2.3%) で動きます。見出しと範囲が合ったまま値だけ壊れた設定は、そのまま使われます。`S` の `config` で確かめ、おかしければ送り直してください。新しい設定とイメージが使われるのは再起動のあとです (`fukemu_net.py` は書き終えると `R` で再起動します)。
- フラッシュの割り方 (`pt.json`):

| 場所 | 中身 |
|---|---|
| 0x000000〜0x001FFF | パーティション表 |
| 0x002000〜0x07DFFF | パーティション A (496KB) |
| 0x07E000〜0x0F9FFF | パーティション B (496KB) |
| 0x0FF000 | 設定 (OTA なしと同じ場所) |
| 0x100000 / 0x240000 | FD0 / FD1 (OTA なしと同じ場所) |

  設定とイメージの場所はパーティションの外 (`absolute` の UF2 を受ける場所) なので、`mkimg.py` の UF2 はパーティション表を書いたあとも BOOTSEL で書けます。OTA のビルドのファームは 496KB に収まることをビルドのときに確かめます (今は約 410KB)。
- 書き込み中は、コア 1 がフラッシュを書くたびにコア 0 (USB と FDC) が止まります (4KB ごとに数十ミリ秒)。更新は pix68k で FUK を使っていないときにします。
- イメージに署名はなく、CRC-32 は壊れていないかを見るだけです。トークンを知っている相手は、ファームもイメージも書き換えられます。
- 確定したあとで見つかった不具合 (たとえば `O` 命令が動かない) からは、無線 LAN では戻れません。そのときは BOOTSEL で書き直します。ポートを 1200 bps で開けば BOOTSEL に入るので、ボタンは要りません。

ビルド (無線 LAN のビルドに足す):

```
# 最初に USB で書くイメージ (TBYB なし)
cmake -S tools/pico_fukemu -B build/pico_fukemu_ota -DPICO_BOARD=pico2_w -DPICO_FUKEMU_OTA=1 \
      -DPICO_FUKEMU_WIFI_FILE=<Wi-Fi のファイル> -DPICO_FUKEMU_TOKEN_FILE=<トークンのファイル>
cmake --build build/pico_fukemu_ota -j8
picotool uf2 convert build/pico_fukemu_ota/pico_fukemu.elf build/fukemu_ota_first.uf2 --family rp2350-arm-s --abs-block
picotool partition create tools/pico_fukemu/pt.json build/fukemu_pt.uf2
# O 命令で送るイメージ (TBYB あり。ビルド先を分ける)。送るのは pico_fukemu.bin
cmake -S tools/pico_fukemu -B build/pico_fukemu_otat -DPICO_BOARD=pico2_w -DPICO_FUKEMU_OTA=1 -DPICO_FUKEMU_OTA_TBYB=1 \
      -DPICO_FUKEMU_WIFI_FILE=<Wi-Fi のファイル> -DPICO_FUKEMU_TOKEN_FILE=<トークンのファイル>
cmake --build build/pico_fukemu_otat -j8
```

- 版の番号は configure したときの時刻 (2026-01-01 からの分の数) から作ります。`-DPICO_FUKEMU_OTA_VERSION=数` で決めてもよい。ブート ROM は確定済みのうち番号の大きいほうを起動するので、送るイメージは前より大きい番号にします (ビルドだけやり直すと番号は変わらないので、cmake を走らせ直す)。
- `O` の応答の `version` はこの番号です。`S` の `version` は git のコミットです。

実機で最初に確かめること (まだ実機で動かしていない):

- パーティションから起動した状態で、X68000 からの書き込み (`flash_range_erase` / `flash_range_program`、物理の場所) のあとも動き続けるか (アドレスの変換 (ATRANS) が保たれるか)。
- コア 0 が動いたまま `O B` を送ってよいか (`rom_get_uf2_target_partition` と `rom_load_partition_table` は `flash_safe_execute` を通らない)。
- OTA の確定のあと、16.7 秒たっても再起動しないか (`S` の `up-ms` が伸び続けるか)。`pico_kbd` と違い、自前のウォッチドッグはありません。
- 送る時刻の揺れ: `'L'` の記録の `rt seg ... due ... queued ...` を、TCP の相手がいない時と、認証を通った相手がいて記録を流している時とで比べる (コア 1 が忙しいと XIP のキャッシュの取り合いで遅れる)。
- `W E` の読み返し (1.2MB をキャッシュなしで読む) の時間。そのあいだコア 1 は無線 LAN を回しません。
- 元のファームに戻す手順 (この節の最後)。

最初の書き込み (1 回だけ、USB で。Pi 4 のベアメタルでは picotool を使えないので Mac で):

1. BOOTSEL に入れる (今のファームなら、どちらかのポートを 1200 bps で開く。例 `stty -f /dev/cu.usbmodemXXXX 1200`)。
2. `picotool load build/fukemu_pt.uf2` (パーティション表)。`picotool reboot -u` で BOOTSEL に戻る。
3. `picotool load -x build/fukemu_ota_first.uf2` (パーティション A に入って起動する)。
   ドライブに写すなら、`RP2350` のドライブに `fukemu_pt.uf2` を写し、もう一度出たドライブに `fukemu_ota_first.uf2` を写す。
4. 設定とイメージはそのまま残ります (パーティションの外にあるため)。`S` の `image` で確かめます。

2 回目からの更新:

```
tools/pico_kbd/picocmd.py --token-file <トークンのファイル> --ota build/pico_fukemu_otat/pico_fukemu.bin pix68k-fukemu.local
tools/pico_fukemu/fukemu_net.py --token-file <トークンのファイル> pix68k-fukemu.local --fd1 fd1.xdf --stretch 1:1:0:8:2.3
```

`picocmd.py` の最後の `O` の応答の `version` が送った版の番号で、`tbyb=00` なら確定済みです。`fukemu_net.py` の引数は `mkimg.py` と同じです (`--fd0`、`--fd1`、`--wp0`、`--wp1`、`--erase0`、`--erase1`、`--config`、`--delivery`、`--rule`、`--stretch`、`--no-stretch`、`--one-sector`)。

パーティション表を書いたあとで、OTA のない版や別のファーム (本物の FUK のファームなど) に戻すときは、先にパーティション表を消します。`picotool erase -r 0x10000000 0x10002000` のあと、`picotool reboot -u` で BOOTSEL に入り直し (ブート ROM が覚えているパーティション表を読み直させる)、戻すファームを書きます。パーティション表が残っていると、パーティションの場所に `absolute` の UF2 を書けなかったり、書いたファームを起動しなかったりするおそれがあります (実機ではまだ確かめていません)。

## イメージと設定を置く

`mkimg.py` で、フラッシュのイメージの場所に書く UF2 を作ります。ファームの UF2 とは別に、BOOTSEL のドライブに写します (`picotool load out.uf2` でもよい)。

```
tools/pico_fukemu/mkimg.py image.uf2 --fd0 fd0.hdm --fd1 fd1.hdm --config
```

- イメージは 2HD (77 シリンダ x 2 面 x 8 セクタ x 1024 バイト = 1261568 バイト) の XDF / HDM です。
- 同じセクタの `--stretch` を 2 つ書くと断ります。
- 指定しなかったドライブには触りません。`--erase0`、`--erase1` でディスクなしにします。`--wp0`、`--wp1` で書き込み禁止にします。
- X68000 から書いたデータは、フラッシュのイメージに直接書きます (4KB ごとに読んで、消して、書いて、読み直して確かめる)。書いている途中で電源が切れると、その 4KB (セクタ 4 つ分) が壊れることがあります。UF2 を書き直すと、元に戻ります。
- ファームを書き直しても、イメージと設定は残ります。

フラッシュの置き場所 (`layout.h`):

| 位置 | 中身 |
|---|---|
| `0x000000`〜`0x0FEFFF` | ファーム (ビルドのときに、ここを超えないことを確かめる) |
| `0x0FF000` | 設定 (4KB) |
| `0x100000` | FD0: 見出し 4KB + イメージ |
| `0x240000` | FD1: 見出し 4KB + イメージ |

## 実時間で答える Read

### どの Read か (`--rule`)

| 値 | 実時間で答える Read |
|---|---|
| `pair` (既定) | R = 7 から始まる Read と、その直後の R = EOT = 8 の Read (同じユニット、C、H) |
| `single` | 1 セクタだけの Read (R = EOT) すべて |
| `all` | すべての Read |
| `none` | なし。すべてすぐに答える |

ほかの Read は、コマンドを受け取ってすぐに `'R'`、長さ、データ、リザルトを送ります。

既定では、実時間で答える Read も、FDC と同じく EOT まで読んでからリザルトを送ります (FUK には DMA の転送バイト数が届かず、TC もないため)。`--one-sector` (hostsim は `-1`) を付けると、最初のセクタだけを送って終えます (R = 7、EOT = 8 でもセクタ 7 だけを送り、リザルトの R は 7)。

`pair` で「直後」とみなすのは、間に SENSE INTERRUPT STATUS、SENSE DEVICE STATUS、SPECIFY、同じシリンダへの SEEK しか挟まらないときです。別のシリンダへの SEEK や Write などが挟まれば、セクタ 8 はすぐに答えます。

`pair` では、Human68k がファイルを読むときにも、セクタ 7 から始まる Read と、続くセクタ 8 の単独読みが実時間になります。セクタ 7 は、頭の下に来るまで最大 1 回転 (166.7 ms) 待ちます。セクタ 8 は、セクタ 7 のリザルトから 1.86 ms 以内にコマンドが届かないと、1 回転待ちます (セクタ 7 のデータの終わりから、セクタ 8 の ID までが CRC と GAP3 の 118 バイトしかないため)。

### 時刻の計算

FDD は X68000 の 2HD と同じ 360 rpm (1 回転 166.67 ms)、MFM で 500 kbps (1 バイト 16 µs、1 回転 10416.67 バイト) とし、トラックは次の並びとします。

| 部分 | バイト数 |
|---|---|
| インデックスのあと (GAP4a 80、SYNC 12、IAM 4、GAP1 50) | 146 |
| 各セクタ: SYNC 12、IDAM 4、ID 4、CRC 2、GAP2 22、SYNC 12、DAM 4 | 60 |
| 各セクタ: データ | 1024 |
| 各セクタ: CRC 2、GAP3 116 (X68000 の 2HD のフォーマットの GPL `0x74`) | 118 |
| 残り (GAP4b) | 654.67 |

インデックスは、Pico が動き始めてからの時刻を 166.67 ms で割り切れるところとします (端数をためないよう、1/3 µs を単位に数えます)。コマンドを受け取った時刻のあとで、セクタ R の ID の先頭が最初に頭の下に来る回転を選び、データの先頭と終わりの時刻を決めます。リザルトはデータのあとの CRC (2 バイト、32 µs) のあとに送ります。R から EOT まで読むときは、同じ回転で続くセクタを読みます。

### セクタを長くする (`--stretch U:C:H:R:PERCENT`)

セクタのデータが流れる時間を、指定した割合だけ長くする試験用の設定です。`1:1:0:8:2.3` は、FD1 の C1 H0 R8 のデータが流れる時間を 2.3% 長くします (16.384 ms → 16.760 ms)。後ろのセクタは、その分だけ後ろにずれます。16 個まで書けます。1 トラックで長くした分の合計は、トラックが 1 回転に収まる 9474 µs まで (GAP4b のうち 1 ms をインデックスの前に残す) で、`mkimg.py` は超える設定を断り、ファームは超えた分を足しません。

既定 (設定を書かないとき、または `mkimg.py` で `--stretch` を省いたとき) は、FD1 の C1 H0 R8 を 2.3% 長くします。`--no-stretch` (hostsim は `-s none`) で、どのセクタも長くしません。ほかの既定は `--rule pair`、`--delivery stream`、EOT まで読む (`--one-sector` なし) です。

### 送り方 (`--delivery`)

本物の FUK がポート 2 にどう送るかは分かっていません。切り替えられるようにしています。

| 値 | `'R'` と長さ | データ | リザルト |
|---|---|---|---|
| `stream` (既定) | データの先頭の時刻 | 1 バイトずつ、流れ終わる時刻 (先頭 + 16 µs x n) に | CRC のあと |
| `head` | データの先頭の時刻 | `'R'` と一緒にまとめて | CRC のあと |
| `end` | CRC のあと | まとめて | まとめて |

`head` と `stream` で `'R'` をコマンドを受け取った時刻でなく、データの先頭の時刻に送るのは、セクタが頭の下に来るまでの回転待ち (0〜166.7 ms) を、データを送る時間に入れないためです。

セクタを長くすると、`stream` ではバイトの間隔が伸び、`head` ではリザルトを送る時刻が遅れます。

`end` では、pix68k から見て最初のバイトとリザルトが同時に届きます。

## 試験

```
tools/pico_fukemu/run_host_tests.sh      # Pico も pico-sdk も要らない
```

- `test_host.c`: `core.c` (FDC のモデル) の単体試験。ポートの見分け、SEEK と SENSE INTERRUPT STATUS、Read (ユニット 0 と 1、ノットレディ、範囲の外)、Write、書き込み禁止、WRITE ID、READ ID、回転の計算、実時間で答える Read の決まり、送り方と `--stretch` ごとのデータとリザルトの時刻。
- `test_mkimg.py`: `mkimg.py` の UF2 を読み戻し、置き場所が `layout.h` と合うことと、中身を確かめます。
- `test_netwrite.c`: `W` 命令 (`netwrite.c`) をメモリの上のフラッシュで試します。見出しを最初に消して最後に書くこと、CRC 違い・読み返しの違い・足りない・多すぎるときに見出しを書かないこと、設定の中身の確かめ、書ける場所の外に触らないこと。
- `test_fukemu_net.py`: `fukemu_net.py` を `nwsim` (`netwrite.c` を TCP で受けるもの) に向けて走らせ、書かれた中身が `mkimg.py` の UF2 と同じことを確かめます。
- ポート 2 の送るバッファが一杯のときは、待たずに主ループに戻ります (ポート 1 とリセットには答え続ける)。送れなかった分は、空いたら続きを送ります。その分、実時間の Read のバイトは遅れます (記録の `queued` の時刻で分かる)。
- CI はホストの試験だけを走らせます。Pico 2 向けのビルド (`main.c`、`usb_descriptors.c`、大きさの確かめ) は、CI の機械に pico-sdk がないので走らせません。

### Pico を書かずに pix68k につなぐ (`hostsim`)

`hostsim.c` は `core.c` を Mac / Linux で動かし、2 本の pty を FUK のポート 1 と 2 として見せます。

```
cc -std=c11 -O2 -I tools/pico_fukemu -o /tmp/hostsim tools/pico_fukemu/hostsim.c tools/pico_fukemu/core.c
/tmp/hostsim -v -t 600 A.hdm B.hdm 2> sim.log    # 1 行目に "/dev/ttysNN,/dev/ttysMM" を出す
pix68k --fuk /dev/ttysNN,/dev/ttysMM
```

`-d` と `-r` は `mkimg.py` の `--delivery` と `--rule` と同じです。`-t` の秒数で終わります。`-v` で、実時間の Read の予定と、pty に書き終えた時刻を標準エラーに出します。`-w` を付けると、終わるときに書き込みをイメージのファイルに書き戻します。

## 手順の出どころ

FUK とのやり取りの手順は、pix68k の `docs/fuk.md` (#295) と、FUK を再現する Pico 2 のファーム (j3tm0t0/picoZloader の `fuk` ブランチ) の `fuk/HOST.md` の表から書きました。同じファームの `fdc.c` は写していません。

## 本物の FUK と違うところ

- ディスクは 2HD のイメージだけです。2DD、ディスクの入れ替え (`'D'`)、排出はしません。ドライブ 2 と 3 はディスクなしです。
- モータを回したときに、レディの変化の割り込みを出しません。
- モータは全ドライブで 1 つとして扱います (`$E94007` の bit7。どのドライブを選んで書いても同じ)。pix68k の試験用の偽物 (`tests/unit/fuk_fake.h`) と同じです。
- SCAN は常に失敗を返します。MT ビットは見ません。
- 実時間で答える Read 以外は、回転を待たずにすぐに答えます。シークも、すぐに終わります。モータが回り始めるまでの時間も待ちません。
- READ ID のリザルトの R は常に 1 です。
- Read のリザルトの R は EOT です。
