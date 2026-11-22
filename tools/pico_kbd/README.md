# pico_kbd: 実機の試験用の Pico ファームウェア

Raspberry Pi Pico (RP2040) を、試験する Pi (以下、試験機) の USB キーボード、UART のログの中継、リセットのスイッチとして使う。
同じソースを Pico W / Pico WH (RP2040、ボード名 `pico_w`) と Pico 2 W / Pico 2 WH (RP2350、ボード名 `pico2_w`) 向けにもビルドできる。
Pico W と Pico 2 W では、命令を無線 LAN (TCP) でも受けられる (下の「無線 LAN で命令を受ける」)。この場合、Debug Probe も UART の線も要らない。
人がいなくても、別の Linux 機 (以下、操作機) から試験機にキーを打ち、ログを読み、固まったら再起動できる。

## つなぎ方

| Pico | つなぐ先 | 用途 |
| --- | --- | --- |
| micro-USB | 試験機の USB | boot プロトコルの USB キーボードに見える。Pico の電源もここから取る |
| GP0 (1 番ピン、UART0 TX) | Debug Probe の U 端子の RX (黄) | 応答とログを操作機に返す |
| GP1 (2 番ピン、UART0 RX) | Debug Probe の U 端子の TX (オレンジ) | 操作機から命令を受ける |
| GND (3 番ピン) | Debug Probe の U 端子の GND (黒) | |
| GP2 (4 番ピン) | 試験機の RUN 端子 (Pi 3B+ は基板の `RUN` の穴) | `X` 命令で Low にして試験機をリセットする。ふだんは入力 (Hi-Z) |
| GP3 (5 番ピン) | Pi 5 の電源ボタンの端子 (J2) の、GND でない側 | `J` 命令で Low にして電源ボタンを押す。ふだんは Hi-Z。下の「Pi 5 の電源ボタン」を読んでからつなぐ |
| GP4 (6 番ピン、UART1 TX) | 試験機の UART の RX (Pi 3 / 4 は GPIO 15、10 番ピン。Pi 5 は 3 ピンの UART 端子の RX) | `S` / `W` 命令で試験機に文字列を送る。つながなくてもよい |
| GP5 (7 番ピン、UART1 RX) | 試験機の UART の TX (Pi 3 / 4 は GPIO 14、8 番ピン) | 試験機のログを受けて UART0 に流す |
| SWD (端の 3 ピン) | Debug Probe の D 端子 | ファームウェアの書き込み |

Debug Probe は操作機の USB につなぐ。操作機に Debug Probe が複数あるときは、シリアル番号で選ぶ
(`udevadm info -q property -n /dev/ttyACM1 | grep ID_SERIAL_SHORT`)。

無線 LAN なしのビルドは、基板の LED を使わない (Pico W / Pico 2 W の LED は無線のチップの先にあり、TinyUSB の `board_init()` はこれらのボードでは LED を触らない)。

### 操作機の GPIO の UART に直につなぐ (Debug Probe を使わない)

操作機が Raspberry Pi なら、Debug Probe の代わりに操作機の GPIO の UART (`/dev/serial0`、3.3V) につないでよい。

| Pico | 操作機 (Raspberry Pi の 40 ピン) |
| --- | --- |
| GP0 (1 番ピン、TX) | GPIO 15 (10 番ピン、RXD) |
| GP1 (2 番ピン、RX) | GPIO 14 (8 番ピン、TXD) |
| GND (3 番ピン) | GND (6 番ピン) |

命令は `/dev/ttyACM1` の代わりに `/dev/serial0` に送る。この場合、書き込みは SWD でなく uf2 で行う (下の「uf2 で書き込む」)。

### Pi 5 の電源ボタン (J2)

Pi 5 には RUN 端子がない。代わりに、基板の電源ボタンと並列の端子 J2 (2 つの穴。片方は GND) を `J` 命令で Low にする。
Pi 5 が止まっているときに短く押すと起動し、動いているときに長く押し続けると強制的に切れる。
pix68k (ベアメタル) が動いている間の短押しは、何も起こさないはずなので、固まった試験機を起動し直すには `J L` (切る) のあと `J` (入れる) を送る。

- GP3 (5 番ピン) を J2 の GND でない側に、Pico の GND を J2 の GND 側か Pi 5 の GND につなぐ。GP3 は Low を出すか Hi-Z にするだけで、High は出さない。内蔵の引き上げ・引き下げは切ってある。
- Pico の電源を Pi 5 の USB だけから取ると、Pi 5 を切ったときに Pico も止まり、起動のための短押しを送れない
  (実機で確かめた。切れている Pi 5 の USB には 5V が出ない)。
  Pi 5 のピンヘッダの 5V (2 番ピン。Pi 5 を切っても出たまま) か、Pi 5 と別の 5V を Pico の VSYS (39 番ピン) につなぎ、GND を共通にする。
- 直につなぐ前に、J2 の GND でない側の電圧をテスターで測る (未確認)。3.3V 以下なら直につないでよい。
  3.3V を超えるとき、または Pico の電源が落ちることがある配線のときは、直につながず、NPN トランジスタ (2SC1815 など) を挟む:
  コレクタを J2 の GND でない側、エミッタを GND、ベースを 1kΩ〜10kΩ の抵抗を通して GPIO につなぎ、ベースと GND の間に 10kΩ 〜 100kΩ の抵抗を入れる。
  この配線では GP3 が High のときに押すことになるので、cmake に `-DPICO_KBD_PWR_ACTIVE_HIGH=1` を付けてビルドしたファームウェアを使う
  (`J` で GP3 に High を出す。押していない間は Hi-Z。`X` と GP2 は変わらない)。付けずにビルドしたファームウェアは、この配線では何も押さない。
- 長押し (`J L`) の既定は 5000 ミリ秒。実機 (Pi 5) では 5000 ミリ秒で切れ、そのあとの `J` (300 ミリ秒) で起動した。
  8000 ミリ秒だと、押している途中で切れ、切れたあと約 20 秒で自分で起動し直した。長さを変えるには `J 6000` のようにミリ秒で指定する (最大 15000)。

## ビルドと書き込み (操作機で)

pico-sdk 2.1 以降、`gcc-arm-none-eabi`、`libnewlib-arm-none-eabi`、`libstdc++-arm-none-eabi-newlib`、`cmake`、`openocd` (RP2040 に対応した 0.12.0 以降) を使う。
キューと行の読み取りは `core.c` に分けてあり、ホストで試せる:
`cc -std=c11 -Wall -Wextra -Werror -o test_host tools/pico_kbd/test_host.c tools/pico_kbd/core.c && ./test_host`
無線 LAN 越しの更新の `O` 命令 (`ota.c`) も、ブート ROM の代わり (`host_stubs/`) とメモリの上のフラッシュで試せる:
`cd tools/pico_kbd && cc -std=c11 -Wall -Wextra -Werror -DPICO_KBD_VERSION=7 -Ihost_stubs -I. -o test_ota_host test_ota_host.c core.c && ./test_ota_host`
TCP の受け口 (`net.c`。トークンの認証と、命令の受け渡し) も、lwIP と無線のチップの代わり (`host_stubs/net_host.h`) で試せる:
`cd tools/pico_kbd && cc -std=c11 -Wall -Wextra -Werror -Ihost_stubs -I. -o test_net_host test_net_host.c core.c && ./test_net_host`
ホストの試験をまとめて走らせるには `tools/pico_kbd/run_host_tests.sh` (CI の host-linux の job もこれを呼ぶ)。`picocmd.py` と `mkuf2.py` の試験 (`test_tools_host.py`) も一緒に走る。

```
PICO_SDK_PATH=~/pico-sdk cmake -S tools/pico_kbd -B build/pico_kbd -DPICO_BOARD=pico -DPICO_NO_PICOTOOL=1
cmake --build build/pico_kbd -j4
sudo openocd -f interface/cmsis-dap.cfg -c "adapter serial <Debug Probe のシリアル番号>" -f target/rp2040.cfg \
  -c "adapter speed 5000" -c "program build/pico_kbd/pico_kbd.elf verify reset exit"
```

### uf2 で書き込む (Pico W、Pico 2 W)

```
PICO_SDK_PATH=~/pico-sdk cmake -S tools/pico_kbd -B build/pico_w_kbd -DPICO_BOARD=pico_w -DPICO_NO_PICOTOOL=1
PICO_SDK_PATH=~/pico-sdk cmake -S tools/pico_kbd -B build/pico2_w_kbd -DPICO_BOARD=pico2_w -DPICO_NO_PICOTOOL=1
cmake --build build/pico_w_kbd -j2 && cmake --build build/pico2_w_kbd -j2
picotool uf2 convert build/pico_w_kbd/pico_kbd.elf build/pico_w_kbd.uf2 --family rp2040
picotool uf2 convert build/pico2_w_kbd/pico_kbd.elf build/pico2_w_kbd.uf2 --family rp2350-arm-s --abs-block
```

- `pico_w` と `pico2_w` の configure では、SDK に無線のドライバ (`lib/cyw43-driver` など) が入っていないと警告が出る。無線は使わないので、そのままでよい。
- picotool (2.x) がビルドする機械にあれば、`-DPICO_NO_PICOTOOL=1` を外すと uf2 もビルドで作られる。上の例は、picotool のない機械で ELF まで作り、別の機械で uf2 に変える手順。
- 書き込み: Pico の BOOTSEL ボタンを押したまま USB をパソコンにつなぐ。出てきたドライブ (Pico W は `RPI-RP2`、Pico 2 W は `RP2350`) に uf2 を写すと、書き込んで再起動する。
  ファームウェアが動いていれば、`B` 命令でも同じ書き込みモードに入る。

Pico を書き換えると USB のつながりが一度切れる。試験機 (Pi 3B+) では、書き換えたあとの Pico を pix68k が見つけ直さなかったので、試験機を再起動する (`X` 命令でよい)。

## 無線 LAN で命令を受ける (Pico W、Pico 2 W)

cmake に SSID とパスワードのファイルを渡してビルドすると、Pico が無線 LAN につなぎ、TCP のポート 2323 で UART と同じ命令を受ける。
UART でも今までどおり受ける。配線は、USB (試験機へ)、GP5 (試験機のログ)、GP2 か GP3 (リセットか電源ボタン) と GND だけになる。

### ビルド

pico-sdk に `lib/cyw43-driver` と `lib/lwip` のサブモジュールが入っていること (`git submodule update --init`)。

```
umask 077
printf '%s\n%s\n' 'SSID' 'パスワード' > ~/pico-wifi.txt          # 1 行目が SSID、2 行目がパスワード (2.4GHz)
openssl rand -hex 8 > ~/pico-token.txt                          # TCP の相手に求めるトークン
PICO_SDK_PATH=~/pico-sdk cmake -S tools/pico_kbd -B build/pico2_w_wifi -DPICO_BOARD=pico2_w -DPICO_NO_PICOTOOL=1 \
  -DPICO_KBD_WIFI_FILE=$HOME/pico-wifi.txt -DPICO_KBD_TOKEN_FILE=$HOME/pico-token.txt -DPICO_KBD_HOSTNAME=pix68k-pico
cmake --build build/pico2_w_wifi -j4
picotool uf2 convert build/pico2_w_wifi/pico_kbd.elf build/pico2_w_kbd_wifi.uf2 --family rp2350-arm-s --abs-block
```

- SSID、パスワード、トークンは、cmake の変数やコマンドの引数には入れず、`gen_secrets.py` がビルド先の `secrets/pico_kbd_secrets.h` に変える。
  このヘッダと、できた elf / bin / uf2 には中身が入っているので、コミットしたり配ったりしない。
  uf2 や bin は `build/` の下に作る (`build/` と、`tools/pico_kbd` の下の `*.uf2`、`*.bin`、`*.elf` は .gitignore に入れてある)。
- `PICO_KBD_HOSTNAME` は DHCP で名乗る名前 (既定 `pix68k-pico`)、`PICO_KBD_TCP_PORT` はポート (既定 2323)。
- 国の設定は日本 (`CYW43_COUNTRY_JAPAN`) に固定してある。

### 使い方

```
host=pix68k-pico.local                  # mDNS で引ける (引けないときは、ルーターの DHCP の一覧で IP アドレスを探す)
tools/pico_kbd/picocmd.py $host P                    # 命令を送り、応答を 1 行待って終わる (トークンは ~/pico-token.txt から読む)
tools/pico_kbd/picocmd.py --wait 10 $host 'J L'      # 応答のあとも 10 秒読む ("power done" まで)
tools/pico_kbd/picocmd.py --log --wait 30 $host      # 試験機のログを 30 秒読む
{ cat ~/pico-token.txt; cat; } | nc $host 2323       # nc でつないだまま、打った行を送る (Ctrl + C で終わる)
```

- `picocmd.py` は Python 3 だけで動く。トークンのファイルは `--token-file` で変えられる。
  nc で 1 つだけ送って終わらせる書き方は、試験機がログを流している間は nc が終わらないので使わない (macOS の nc で確かめた)。

- 最初の行はトークン。合えば `#pico auth ok` が返り、そのあとは命令を 1 行ずつ送れる。違うトークンは何も返さずに切る。10 秒たってもトークンが来なければ切る。
- 相手は同時に 1 つ。認証を通った相手がつないでいる間、ほかの接続は断る。相手が何も言わずに消えたときは、十数秒で気づいて次の相手を待つ。
- 応答 (`#pico ...`) は UART と TCP の相手の両方に返る。試験機のログ (GP5) は、TCP の相手には行の先頭に `> ` を付けて送る
  (0.2 秒たっても行が終わらないときは、そこまでを 1 行として送る)。相手が受け取らずにたまったログは捨てる。
- `N` 命令で、無線 LAN の様子 (IP アドレス、つないでいる相手、捨てたログの行数) が返る。
- 無線 LAN につながったときは `#pico wifi up ip=... port=2323 host=...` を、相手がトークンを通ったときは `#pico net client <相手の IP>` を UART に出す。
- `X` と `J` は、応答が相手に届いてから (最大 1 秒待つ) 端子を動かす。Pico の電源を試験機の USB から取っていると、
  `X` や `J L` で Pico も止まって接続が切れずに残ることがあるので、送る側は `nc -w` などで待つ時間を区切る。
- トークンは平文で流れる。同じ LAN の中の、たまたまつないだ相手を止めるためのもので、盗み聞きは防げない。
  違うトークンが来たら、そのあと 1 秒はだれも受け付けない (正しいトークンの相手も、その間は接続を断られる。
  違うトークンを送り続ける相手がいると、だれもつなげなくなる。無線 LAN 越しの更新の確定待ちの間 (Pico 2 W は約 16.7 秒、
  Pico W は 60 秒) に続けられると、確定できずに前の版に戻る)。
- トークンを知っている相手は、`O` 命令でファームウェアを書き換えられる (OTA のビルド)。イメージに署名はなく、CRC-32 は壊れていないかを見るだけ。
  信用できない相手のいる LAN では使わない。
- `B` (BOOTSEL) を TCP から送ると、USB の書き込みモードに入ったままになり、無線 LAN からは戻せない (USB を挿し直すか、UF2 を書く)。
- 無線 LAN につながらないまま 10 分たつと、再起動して無線のチップを初期化し直す (`#pico wifi down reboot`)。
  再起動の間は UART の命令も受けられず、USB のキーボードも試験機から一度抜ける (押したままのキーは離れる)。
  試験機 (Pi 3B+) は、つながり直した Pico を見つけ直さなかった (上の「ビルドと書き込み」の注意)。アクセスポイントが 10 分以上止まったあとは、
  `X` で試験機をリセットするまでキーが届かなくなることがある。
  `X` / `J` で端子を動かしている間は、終わるまで再起動を待つ。
  アクセスポイントのない所で UART だけで使うときは、cmake に `-DPICO_KBD_WIFI_REBOOT_MIN=0` を付ける。

### 無線 LAN 越しの更新 (Pico 2 W)

RP2350 のブート ROM の A/B パーティションと「試してから確定」(TBYB) を使い、USB につなぎ直さずにファームウェアを入れ替える。
仕組みと `O` 命令は `ota.c` の先頭に書いてある。RP2040 (Pico W) にはこの仕組みがないので、別の方法を使う (下の「無線 LAN 越しの更新 (Pico W)」)。

- 新しいイメージは、動いていない側のパーティションに書く。送っている途中で切れても、動いている版はそのまま動き続ける。
- 書き終えると再起動して新しい版を 1 回だけ試す。新しい版は、TCP でトークンを通った相手が来たときに確定する
  (`picocmd.py --ota` が、送り終えたあとにつなぎ直して `O` を送る)。
  約 16.7 秒たっても確定しなければ、ブート ROM が再起動して前の版に戻す (無線 LAN につながらない版や、TCP を受けられない版を送ってしまっても、送り直せる)。
- 確定したあとで見つかった不具合 (たとえば `O` 命令だけが動かない) からは戻れない。そのときは BOOTSEL で書き直す。
- 書く先は、更新のたびに A と B を行き来する。`O B` を断られたときの `ota error no-partition N` の N は、
  ブート ROM に書く先を聞いたときのエラーの番号 (負の数。-17 は書ける先がない)。
- `-DPICO_KBD_OTA_NEVER_BUY=1` を付けたビルドは確定しない。送ると、16.7 秒後に前の版に戻る (戻る動きを確かめるための版)。

ビルド (無線 LAN のビルドに足す):

```
# 最初に UF2 で書くイメージ
cmake ... -DPICO_BOARD=pico2_w -DPICO_KBD_WIFI_FILE=... -DPICO_KBD_TOKEN_FILE=... -DPICO_KBD_OTA=1
picotool uf2 convert build/.../pico_kbd.elf build/pico2_w_kbd_ota_first.uf2 --family rp2350-arm-s --abs-block
picotool partition create tools/pico_kbd/pt.json build/pico2_w_kbd_pt.uf2
# O 命令で送るイメージ (ビルド先を分ける)。送るのは elf と一緒にできる pico_kbd.bin
cmake ... -DPICO_BOARD=pico2_w -DPICO_KBD_WIFI_FILE=... -DPICO_KBD_TOKEN_FILE=... -DPICO_KBD_OTA=1 -DPICO_KBD_OTA_TBYB=1
```

- 版の番号は、configure したときの時刻 (2026-01-01 からの分の数) から作る。`-DPICO_KBD_VERSION=数` で決めてもよい。
  configure し直さずにビルドだけやり直すと、同じ番号のままになる。送るイメージを作るたびに、`cmake -DPICO_KBD_VERSION=数 ビルド先` で番号を上げる。
  あとから送るイメージほど大きい番号にする (ブート ROM は、確定済みのイメージのうち番号が大きいほうを起動する)。
- `PICO_KBD_OTA_TBYB=1` のイメージは、`O` 命令で送ったときだけ起動する。UF2 で最初に書くイメージには付けない。
  付けないイメージは、確定を待たないので、無線 LAN につながらない版でも前の版に戻らない。`O E` は `ota error not-tbyb` で断る
  (それでも入れるなら `picocmd.py --ota-force`)。
- 新しい版を試している間 (確定の前) は、次のイメージを受けない (`ota error trial`)。受けると、戻り先の版を消してしまうため。

最初の書き込み (1 回だけ、BOOTSEL で):

1. BOOTSEL ボタンを押したまま USB をつなぎ、`RP2350` のドライブに `pico2_w_kbd_pt.uf2` (パーティション表) を写す。
   再起動して、もう一度 `RP2350` のドライブが出る (起動できるイメージがまだないため)。
2. そのドライブに `pico2_w_kbd_ota_first.uf2` を写す。A のパーティションに入り、再起動して動き始める。

picotool があれば、`picotool load pico2_w_kbd_pt.uf2 && picotool reboot -u && picotool load -x pico2_w_kbd_ota_first.uf2` でも同じ。

2 回目からの更新:

```
tools/pico_kbd/picocmd.py --ota build/.../pico_kbd.bin $host     # 30 秒ほどで送り終え、再起動のあとつなぎ直して O の応答を表示する
```

最後に表示される `O` の応答の `version` が送った版の番号で、`tbyb=00` なら確定済み。`version` が前のままなら、新しい版は確定されずに戻されている。

### 無線 LAN 越しの更新 (Pico W)

RP2040 のブート ROM には A/B の仕組みがないので、フラッシュの先頭に小さなブートローダ (`boot2040/`、約 7KB) を置く。
フラッシュの割り方と進み方は `otaboot.h` に書いてある。送る側の手順 (`picocmd.py --ota`) と `O` 命令は Pico 2 W と同じ。

- ファームウェアは、受けたイメージを STAGING に書き、「入れ替えを頼む」と記録して再起動する。
- ブートローダは、STAGING の CRC を確かめ、今の版を BACKUP に写してから、新しい版を MAIN に写して起動する (写しに 30 秒ほどかかる)。
- 新しい版は、TCP でトークンを通った相手が来たときに確定する。確定の前にもう一度ブートローダが動いたら
  (固まってウォッチドッグが再起動した、60 秒たっても確定できずに自分で再起動した、電源が切れた)、BACKUP から前の版に戻す。
- 写した 4KB は読み返して確かめ、合わなければやり直す。BACKUP が今の版と同じになるまでは MAIN を消さず、
  MAIN が BACKUP と同じになるまでは「戻し終えた」と記録しない。やり直しても合わないときは、どこで合わなかったかで分かれる:
  - 今の版を BACKUP に写せなかった: MAIN にはまだ触っていないので、更新をやめて今の版を起動する (`O` の応答が `result=4`)。
  - MAIN に新しい版を写せなかった: BACKUP から前の版に戻す。
  - 次のどれかのときは、MAIN を起動せずに BOOTSEL に入る (記録は途中の段のまま残るので、次に電源を入れたときに同じ所からやり直す):
    - 戻すこともできなかった (MAIN が BACKUP と同じにならない、BACKUP が起動できるイメージに見えない)。
    - 新しい版を MAIN に写し終えたが、「試している」と記録できなかった (試している版として扱えず、だめな版でも戻せなくなるため)。
    - 「前の版に戻す」と記録できなかった。
  - 記録を書けなくても MAIN を起動する場合が 2 つある。どちらも MAIN は前の版 (今の版) にそろっている:
    - MAIN を書き換え始める前 (STAGING を確かめた結果や、BACKUP に写し終えたことの記録) に失敗した。MAIN には触っていないので、今の版を起動する。
    - 前の版に戻し終えたあと、「戻し終えた」の記録に失敗した。MAIN は BACKUP と同じになっているので起動し、次の起動で記録だけやり直す。
- 写しの途中で電源が切れても、次の起動で同じ所からやり直す。記録は 2 か所に交互に書くので、書いている途中で切れても前の記録が残る。
  この進み方は、ホストで、フラッシュの操作を 1 回ずつ「そこで電源が切れた」「消去や書き込みが何も言わずに失敗した」ことにして試してある:
  `cd tools/pico_kbd && cc -std=c11 -O2 -Wall -Wextra -Werror -o test_otaboot_host test_otaboot_host.c otaboot.c core.c && ./test_otaboot_host`
- ブートローダ自身は、無線 LAN 越しには更新できない。

ビルド (pico-sdk 2.3 以降。2.2.0 では `pico_add_linker_script_override_path` の所で止まる。リンクの番地を変えるのに `pico_add_linker_script_override_path` を使う):

```
cmake -S tools/pico_kbd/boot2040 -B build/boot2040 -DPICO_BOARD=pico_w -DPICO_NO_PICOTOOL=1 -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build/boot2040
cmake -S tools/pico_kbd -B build/pico_w_ota -DPICO_BOARD=pico_w -DPICO_NO_PICOTOOL=1 \
  -DPICO_KBD_WIFI_FILE=... -DPICO_KBD_TOKEN_FILE=... -DPICO_KBD_HOSTNAME=pix68k-pico -DPICO_KBD_OTA=1
cmake --build build/pico_w_ota
# 最初に BOOTSEL で書く UF2 (ブートローダとファームウェアを 1 つにまとめる)
tools/pico_kbd/mkuf2.py build/pico_w_kbd_ota_first.uf2 0x10000000:build/boot2040/pico_kbd_boot2040.bin 0x10010000:build/pico_w_ota/pico_kbd.bin \
  0x101f0000:erase:8192
# 2 回目からは、ファームウェアの bin を送る
tools/pico_kbd/picocmd.py --ota build/pico_w_ota/pico_kbd.bin pix68k-pico.local
```

- `PICO_KBD_OTA=1` のファームウェアは 0x10010000 で動くようにリンクされるので、単独の UF2 にして書いても起動しない。必ずブートローダと一緒に書く。
- ドライブ (RPI-RP2) に写して書くときは、UF2 の中で各 4KB の 16 ブロックが、16 の倍数の番目から続けて並んでいないといけない。ずれていると、4KB ごとの先頭のブロックが消え、ブートローダが MAIN を空と見て BOOTSEL に戻る。`mkuf2.py` は bin を 4KB まで 0xff で埋めて、そろえる (`picotool load` ではずれていても書ける)。
- `0x101f0000:erase:8192` は、更新の記録 (2 つの 4KB) を消す。付けないと、前の更新の途中の記録が残っているときに (試している間に書き直した、
  BOOTSEL に入ったので書き直した、など)、次の起動でブートローダが古い写しの続きをして、書いたばかりの MAIN を上書きする。
- `O` の応答は `ota version=V state=S result=R trial=T`。`result` は最後の更新の結果 (1 = 確定した、2 = 前の版に戻した、3 = 受けたイメージが壊れていた、4 = 今の版を BACKUP に写せなかった)。
- 版の番号は表示のためだけに使う (RP2040 では、起動するイメージを番号で選ばない)。

### 基板の LED

| LED | 様子 |
| --- | --- |
| 消えたまま | 無線のチップの初期化に失敗した (またはファームウェアが動いていない) |
| 1 秒に 1 回の点滅 | アクセスポイントにつなごうとしている (見つからない、IP アドレスが取れない間もこの点滅) |
| 速い点滅 (1 秒に 5 回) | パスワードが違うと言われた (つなぎ直しは続ける) |
| 点いたまま | IP アドレスを取れた。TCP の相手を待っている |
| 1 秒ごとに短く消える | トークンを通った相手がつないでいる |

暗号の方式は、失敗するたびに WPA2、WPA3 / WPA2、WPA2 / WPA の順に変えてつなぎ直す。

## 命令

Debug Probe の UART (115200bps) か、無線 LAN の TCP に 1 行ずつ送る。命令の一覧は `main.c` の先頭にある。

```
d=/dev/ttyACM1; stty -F $d 115200 raw -echo -ixon -crtscts
cat $d &                          # 応答 (#pico ...) と試験機のログ
printf 'P\n' > $d                 # 生きているか。USB がつながっていれば "mounted"
printf 'T basic\\n\n' > $d        # JIS 配列のキーとして打つ (\n は Enter)
printf 'D 4c 05\n' > $d; printf 'U\n' > $d   # Ctrl + Alt + Delete (pix68k は書き戻してから Pi を再起動する)
printf 'X\n' > $d                 # RUN 端子で試験機をリセットする (固まったとき)
printf 'J\n' > $d                 # Pi 5 の電源ボタンを短く押す (300 ミリ秒。止まっている Pi 5 を起動する)
printf 'J L\n' > $d               # Pi 5 の電源ボタンを長く押す (5000 ミリ秒。強制的に切る)
printf 'S dir\n' > $d             # 試験機の UART に "dir" と改行 (\n) を送る (GP4 を試験機の UART の RX につないだとき)
printf 'C 3000 kg\n' > $d         # USB を切り離し、3 秒後にキーボードとゲームパッドの複合装置としてつなぎ直す
printf 'G 1 0 200\n' > $d         # ゲームパッドのボタン 1 を 200 ミリ秒押す
printf 'G 0 3\n' > $d; printf 'G\n' > $d    # ハットを右に倒したままにし、離す
```

- 応答 (`#pico ...`) を待ってから次の命令を送る。
- キー (`T` / `K` / `D`) は `#pico mounted` を見てから送る。USB がつながる前に送ったキーは入れずに `dropped` を返す (`T` は `dropped=` に数える)。
- `X` のあとの応答は、`#pico reset done`、`#pico started` (2 回)、`#pico mounted` の順に来る (Pi 3B+ の試験機で見た並び)。待つのは `#pico mounted` まで。そのあとはキーを送れる。
- `J` の応答は `#pico power 300 ms`、押し終えたら `#pico power done`。`J 5000` のようにミリ秒 (1〜15000) でも指定できる。押している間の 2 つ目の `J` は `power busy`。
- `S 文字列` は、試験機の UART に文字列と改行 (`\n`) を送る。`W 文字列` は改行を付けない。どちらも `T` と同じ `\n`、`\r`、`\t`、`\e`、`\b`、`\\` が書ける
  (CR で終わらせたいときは `W 文字列\r`)。応答は `sent N`。送る分が待ち行列 (1023 バイト) に入りきらないときは、何も送らずに `send dropped`。
- ゲームパッド: 起動したときの Pico はキーボードだけを見せる。`C on kg` か `C ミリ秒 kg` でつなぎ直すと、インターフェース 1 に HID のゲームパッド
  (TinyUSB の雛形の並び。軸 6 本はいつも 0、ハット 1 つ、ボタン 32 個) を足した複合装置になる。`C on k` でキーボードだけに戻る。
  VID:PID はキーボードと同じ cafe:4068 なので、pix68k はボタンの並びを知らないゲームパッドとして扱う (方向はハットで、ボタンは番号の順)。
  試験機にほかのゲームパッドが挿してあると、ジョイスティックの枠を先に取ることがあるので、使い終わったら `C on k` に戻す。
- `G ボタン [ハット [ミリ秒]]`: ボタンは 16 進 (bit0 がボタン 1)、ハットは 0 (離す)、1 (上) から時計回りに 8 (左上) まで。ミリ秒 (1〜10000。0 は書けない) を書くと、その時間だけ押して離す (押したレポートを試験機が読み終えた時刻から数える)。
  引数なしの `G` はすべて離す。応答は `pad`。ゲームパッドを見せていないときは `pad none`、つながっていないときは `pad dropped`。
- USB の抜き差しの真似: `C off` で切り離し (`tud_disconnect`。試験機からは抜いたように見える)、`C on` でつなぐ。`C ミリ秒` は切り離して、ミリ秒 (100〜60000) 後につなぎ直す
  (応答は `usb off N ms`、つなぎ直したら `usb on`、試験機が設定を終えたら `mounted`)。キーボードとゲームパッドは 1 つの USB 装置なので、一緒に抜き差しされる。
  切り離している間の `T` / `K` / `D` / `G` / `U` は入れずに `dropped` を返す。TCP の命令とログの中継は続く。
  `C off` のあとすぐ `C on` を送ると、切り離していた時間が短すぎて試験機が抜かれたことに気づかず、`mounted` が来ないことがある
  (Pico は、試験機が設定し直すまで `not-mounted` と答える)。`C off` と `C on` の間は 100 ミリ秒以上空けるか、`C ミリ秒` を使う。
  `picocmd.py` は命令ごとに応答を 1 行しか待たないので、`usb on` と `mounted` まで読むには `--wait` を付ける (例: `picocmd.py --wait 8 ホスト 'C 3000 kg'`)。
- 主ループが 8 秒回らなければ、ウォッチドッグが Pico を再起動する (固まったときに、人が挿し直さなくても戻るように)。
  無線のチップの初期化の間も掛けてある (この起動がウォッチドッグの時間切れによるものなら、初期化が済んでから掛ける)。
  無線 LAN 越しの更新で新しい版を試している間は、確定が済んでから掛ける (確定しない試験用の版では掛けない)。
  Pico 2 W では、TBYB の付いていないイメージを書いた直後の起動 (BOOTSEL で UF2 を書いた直後、`--ota-force`) も、
  トークンを通った相手が 1 回つなぐまでは掛からない。
  `N` の応答の終わりの `wd=1 wd-boot=0` の `wd` は掛かっているか、`wd-boot` はこの起動がウォッチドッグの時間切れによるものか
  (そのときは起動の応答も `#pico started wd` になる)。`Z` は、わざと固まって約 8 秒後に再起動させる (試験用。応答は `hang`)。
- 試験機が止まったとき (USB の suspend)、つながり直したとき、つながりが切れたときは、まだ送っていないキーを捨てる (suspend のときは、すべて離したレポートを 1 つだけ残し、resume したら送る)。RP2040 の TinyUSB は VBUS を見ないので、試験機のリセットだけでは「切れた」と分からないため。
- GP2 は RUN に直につないでいる。Pico の電源が落ちている間は RP2040 の端子が保護ダイオードで電源側に引かれるので、RUN の引き上げが弱まるおそれがある。Pi 3B+ の試験機では問題になっていないが、確実にするなら GP2 と RUN の間にダイオード (カソードを GP2 側) か小信号のトランジスタを入れる。

## 試験機を LAN から起動する (Pi 3B+)

カーネルを入れ替えるたびに SD カードを抜き差ししなくて済むよう、試験機は操作機の TFTP から起動する。

- 操作機に dnsmasq を入れ、proxy DHCP (アドレスは割り当てず、起動に要る情報だけを返す) と TFTP を動かす。
  ```
  # /etc/dnsmasq.d/pix68k-netboot.conf
  port=0
  interface=wlan0
  bind-interfaces
  dhcp-range=192.168.1.0,proxy
  pxe-service=0,"Raspberry Pi Boot"
  enable-tftp
  tftp-root=/srv/tftp/pix68k
  ```
  `interface=` と `dhcp-range=` は操作機のネットワークに合わせる (例は、試験機の有線 LAN と操作機の無線 LAN が同じネットワークにある構成)。書いたら `sudo systemctl restart dnsmasq`。
- `/srv/tftp/pix68k/` に `bootcode.bin`、`start.elf`、`fixup.dat`、`config.txt`、`cmdline.txt`、`kernel8.img` を置く。
  `config.txt` はリポジトリの README の SD カードの作り方と同じものを使う。Pi 3B+ に要るのは最低限次の行:
  ```
  arm_64bit=1
  kernel_address=0x80000
  initial_turbo=0
  disable_overscan=1
  kernel=kernel8.img
  ```
  `arm_64bit=1` がないと 32 ビットで起動しようとして `kernel8.img` を読まない。
  `disable_overscan=1` がないとフレームバッファが削られ、画面が 2 倍に拡大されない。
- 試験機の SD カードには `bootcode.bin`、空のファイル `timeout`、`/pix68k/` (ROM とディスクのイメージ) だけを置く。
  SD カードの `bootcode.bin` が LAN からの起動に進み、pix68k は SD カードから ROM とイメージを読む。
  SD カードがなくても Pi 3B+ は LAN から起動するが、始まるまでに時間がかかる (約 15 秒。SD カードの `bootcode.bin` からなら約 6 秒)。
- 新しいカーネルを試すときは `/srv/tftp/pix68k/kernel8.img` を置き換えて、Ctrl + Alt + Delete か `X` を送る。
