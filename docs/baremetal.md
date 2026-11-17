# ベアメタル版 (Raspberry Pi)

ベアメタル版のビルド、config.txt、コアの割り当て、音の出力、ファン、ログ、画面の拡大と OSD、I2S の DAC、SD カードと USB メモリ、起動にかかる時間。

文中の #番号 は、開発用の非公開リポジトリの issue と PR の番号。

低レベル部分 (起動、USB、HDMI、SD カード、割り込み) は [Circle](https://github.com/rsta2/circle) (Step51.1.1、`external/circle` の submodule) を使う。

```sh
git submodule update --init
brew install aarch64-elf-gcc qemu mtools
tools/build_baremetal.sh 3 --qemu   # QEMU 用の Pi 3 版 → build/baremetal-raspi3-qemu/app/kernel8.img
tools/qemu_smoke.sh                 # QEMU raspi3b で起動し、SD カード・USB 入力・マルチコアの動作をログで確かめる
tools/qemu_human68k.sh              # QEMU raspi3b で Human68k を起動し、DIR を打って一覧が出るか、USB マウスの動きが届くかを確かめる (ROM がなければ飛ばす)
RENDER_CORE=off tools/qemu_human68k.sh  # 同じことを、画面の合成をコア 1 で行う設定 (render_core=off) で行う
SOUND_CORE=off tools/qemu_human68k.sh   # 同じことを、音の合成をコア 1 で行う設定 (sound_core=off) で行う
FAIL_CORE=2 tools/qemu_human68k.sh  # 同じことを、コア 2 以降が起動しなかったことにして (pix68k_fail_core=2) 行う
FAIL_CORE=3 tools/qemu_human68k.sh  # 同じことを、コア 3 が起動しなかったことにして (pix68k_fail_core=3) 行う
SOUND_OUT=off tools/qemu_human68k.sh  # 同じことを、音を出さない設定 (pix68k_sound=off。コア 3 を使わない) で行う
BOOT=hd tools/qemu_human68k.sh      # 同じことを、起動ディスクから作った SASI のイメージ (hd0=) で行う
WRITEBACK=1 tools/qemu_human68k.sh  # DIR のあとにファイルを作り、イメージのファイルに書き戻されたか (SD カードのイメージの中と、もう一度起動して TYPE) を確かめる
BOOT=hd STREAM=1 WRITEBACK=1 tools/qemu_human68k.sh  # 同じことを、SASI のイメージを全体を読まずにチャンクだけ読む設定 (pix68k_stream_min=0) で行う
WRITEBACK=1 REBOOTKEY=1 tools/qemu_human68k.sh  # ファイルを作ったあと書き戻しを待たずに Ctrl + Alt + Delete を送り、書き戻してから再起動するかを確かめる
MENU=1 tools/qemu_human68k.sh       # DIR のあとに設定メニューを開き、設定の保存、ステートの保存と読み込み、FD0 の入れ替え、リセット、Restart を確かめる
WFE=0 tools/qemu_human68k.sh       # 同じことを、待つ間を wfe で眠らせずに空回りで待つ設定 (pix68k_wfe=0) で行う
HOSTNET=1 tools/qemu_human68k.sh    # ネットワークの口 (net=on) を動かし、DHCP、curl でのファイルの読み書き、/log、/screen.ppm、/type、/key、syslog、/reboot を確かめる
tools/build_baremetal.sh 3          # 実機の Pi 3 版 → build/baremetal-raspi3/app/kernel8.img
tools/build_baremetal.sh 4          # 実機の Pi 4 / Pi 400 版 → build/baremetal-raspi4/app/kernel8-rpi4.img
tools/build_baremetal.sh 5          # 実機の Pi 5 版 → build/baremetal-raspi5/app/kernel_2712.img
```

Linux では、Ubuntu のパッケージの aarch64-linux-gnu-gcc でビルドできる (CI の `baremetal` の job で使っている。実機では確かめていない。
QEMU の `tools/qemu_smoke.sh` は通る)。接頭辞を `PREFIX64` で渡す。

```sh
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu qemu-system-arm mtools rsync
PREFIX64=aarch64-linux-gnu- tools/build_baremetal.sh 3 --qemu
tools/qemu_smoke.sh
```

`tools/build_baremetal.sh` は、コンパイラが Linux 向け (`gcc -dumpmachine` の答えに `-linux-` がある) なら、`Config.mk` に
`-mno-outline-atomics` を足す。Linux 向けの GCC は、LSE のない CPU (Raspberry Pi 3、Raspberry Pi 4) ではアトミックな操作を libgcc の関数の呼び出しにし、
その関数が `__getauxval()` を使うので、足さないと Raspberry Pi 3 / Raspberry Pi 4 版がリンクできない (Circle のライブラリに渡すため。アプリの部分には、
どのコンパイラでも `baremetal/Makefile` が同じオプションを付けていて、重なっても問題ない)。aarch64-elf-gcc では `Config.mk` に足さず、
Circle のライブラリのビルドは変わらない。

カーネルの大きさには上限がある。Circle は、.bss まで含めたカーネルの終わり (リンカの `_end`) が `0x80000 + KERNEL_MAX_SIZE`
を超えていると、起動の直後に何も表示せずに止まる。Circle の既定は 2MB で、Raspberry Pi 3 / Raspberry Pi 4 / Raspberry Pi 5 版は 68030 / 68060 の命令表 (512KB ずつ)
などで 2MB を超えた (Pi 4 / Pi 5 版の `_end` は 3MB 余り。今の値はビルドの最後に出る) ので、`tools/build_baremetal.sh` で 8MB に
している (Circle の `configure --kernel-max-size 8` で渡す。`Config.mk` に `-DKERNEL_MAX_SIZE=0x800000` が入り、Circle のライブラリと
アプリの C++ とアセンブラのすべてに渡る。Circle のソースは変えない)。そのぶん、スタックとヒープの始まりが 6MB 上に動く。
`tools/build_baremetal.sh` はビルドの最後に `_end` と上限を比べ、超えていればカーネルのイメージを消して失敗する (CI の baremetal
の job でも同じ)。ELF がない、2 つ以上ある、`nm` が失敗した、`_end` がないときも、確かめられないのでイメージを消して失敗する。
前のビルドのイメージはビルドの始めに消すので、途中のどこで失敗しても (Circle の設定の確かめ、ライブラリ、アプリ)、古いイメージは残らない。
成功したときは `_end 0x...... (上限 0x880000、残り .... KB)` の形で残りを出す。

`memcpy` / `memset` とアトミックな操作 (ベアメタル版の速さ): Circle は `-ffreestanding` でビルドするので、そのままでは GCC が
`memcpy` などを組み込み関数として扱わず、大きさが定数の 4 バイトや 8 バイトの写しも Circle の関数の呼び出しになる。Circle の
AArch64 の `memcpy()` (`lib/util_fast.S`) は、128 バイト未満のときと、どちらかの番地が 8 バイトにそろっていないときは 1 バイトずつ写す。
`baremetal/Makefile` は次のことを行う (Circle のライブラリと addon のビルドは変えない)。

- `baremetal/builtin_mem.h` を pix68k と同梱のライブラリ (Munt、Nuked-SC55、TinySoundFont) のすべてのソースに `-include` で読ませ、
  `memcpy` / `memset` / `memmove` / `memcmp` / `strlen` を組み込み関数として扱わせる。大きさが定数の小さいものは、その場のロードと
  ストアになる。`-fbuiltin` は使わない (`printf` が `putchar` の呼び出しに書き換えられてリンクできない。数学の関数も組み込み関数として
  扱われるようになるが、結果が変わるかどうかは確かめていない)
- 大きさが変数のものは関数の呼び出しのまま残るので、`memcpy()` と `memset()` は `baremetal/mem_fast.cpp` のものを使う
  (リンクのときに Circle のものより先に使われる)。長さに関係なく 8 バイトずつ (dest と src の番地の下位ビットが 4 バイトだけ
  ずれていれば 4 バイトずつ) 写す。2 バイト以上の読み書きは、その大きさにそろった番地だけで行う (フレームバッファは Device の
  メモリで、そろっていない読み書きは例外になる。Circle のものと同じ)。128 バイト以上の `memcpy()` は Circle と同じ 32 バイトずつのループ。
  `memcpy()` は 1 回分 (多くて 32 バイト) を全部読んでから書き、番地の小さいほうの回から順に進む (Circle の `memmove()` が、dest が src より
  小さくて重なるときに `memcpy()` を呼ぶため)。`memmove()` と `memcmp()` は Circle のまま (呼び出しが少ない)。ホストでは `mem_fast_test` が同じソースを試験する
- 組み込み関数の `memcpy` などは引数がヌルでないものとして扱われ、写したあとのヌルの検査を GCC が消すことがあるので、
  `-fno-delete-null-pointer-checks` を付ける
- Pi 3 / Pi 4 (LSE のない Cortex-A53 / A72) 向けでは、GCC がアトミックな操作 (`__atomic_exchange_n` など) を libgcc の関数
  (`__aarch64_cas4_acq_rel` など) の呼び出しにするので、`-mno-outline-atomics` でその場の命令 (LL/SC) にする

オブジェクトファイルの中の呼び出しの数は `tools/baremetal_memcalls.py build/baremetal-raspi5/app` で数えられる (`--detail` でファイルごと)。
Pi 5 版のアプリの部分で、`memcpy` / `memset` / `memmove` / `memcmp` / `strlen` の呼び出しは 704 か所から 344 か所になり、大きさが定数で
128 バイト未満のものは 301 か所から 1 か所になった (残る 1 か所は HTTP の処理の中の、長さが 0 になる分岐)。
Pi 3 / Pi 4 版のカーネルのアトミックの関数の呼び出しは 53 か所から 2 か所になった (残りは Circle と libsupc++ の中。
アプリの部分は、同じ道具の `atomic` の列で 0 か所)。
`tools/build_baremetal.sh` は、ビルドの最後にカーネルの逆アセンブルを調べ (`tools/baremetal_hot_mem.sh`)、次のどれかがあれば失敗する:
名前に `m68k::` か `x68k::` がある関数の中の、大きさが 128 バイト未満の定数の `memcpy()` の呼び出し。`memcpy()` / `memset()` の中の、
ほかの関数の呼び出し、NEON / 浮動小数点のレジスタ、スタックの使用。Circle の `util_fast.o` のリンク。
Pi 5 の実機 (PR #91 のコメントの測定) では、テキスト画面が主の場面で、コア 2 が 1 フレームを合成する時間が 2162us から 1351us になった。
ゲームの場面 (悪魔城ドラキュラ) のコア 2 と、コア 1 (CPU のコアの `memcpy` は PR #84 でなくなっている)、コア 3 は変わらなかった。Pi 3 / Pi 4 の実機では測っていない。

実機で動かすには、SD カードの FAT のパーティションに Raspberry Pi のファームウェア (Circle の `boot/` で取得できる) とカーネルのイメージを置く。
`config.txt` は Circle の `boot/config64.txt` をもとにする (`arm_64bit=1`。Raspberry Pi 4 と Raspberry Pi 400 は同じカーネル `kernel8-rpi4.img` で、
`[pi4]` の節で選ぶ)。`[pi4]` の節の `armstub=armstub8-rpi4.bin` の行は消す。この armstub は FIQ を使うときだけ要り、pix68k は使わない
(消さずに使うなら、Circle で `make armstub64` を実行してできたファイルを置く。Pi 400 での起動はまだ実機で確かめていない。Pi 4 は [compat.md](compat.md) の #105 の行で確かめた)。
Pi 5 では、`[pi5]` の節に `framebuffer_depth=32` と `framebuffer_ignore_alpha=1` を必ず書く (`config64.txt` ではコメントになっている)。
pix68k は 32 ビットのフレームバッファを使うので、この 2 行がないと Circle が画面を作れず、何も表示せずに止まる
(ネットワークも出ない。UART には `pix68k: cannot create the 32-bit framebuffer; ...` の 1 行だけを出す)。
Circle のソースや文書に理由が書いてあるわけではなく、Pi 5 8GB の実機で確かめた結果。2 行のどちらか片方で足りるかは確かめていない。

```
[pi5]
kernel=kernel_2712.img
framebuffer_depth=32
framebuffer_ignore_alpha=1
```

UART のログ (115200bps) は、Pi 3 と Pi 4 / Pi 400 では GPIO 14/15、Pi 5 では HDMI 端子の間にある 3 ピンの UART 端子に出る。
USB は、Pi 3 は SoC の DWHCI、Pi 4 / Pi 400 は PCIe の先の VL805 (xHCI)、Pi 5 は RP1 の xHCI を Circle が使う。
キーボード、マウス、ゲームパッドはどの機種でも Circle の同じドライバ (`ukbd1`〜、`mouse1`、`upad1`〜) から受け取る。
boot パーティションの `/pix68k/` (ROM はルートでもよい) に次のファイルを置くと、X68000 をコア 1 で実時間で動かし、画面 (32 ビットのフレームバッファの真ん中) と USB キーボードをつなぐ。
画面の合成はコア 2 で行う (コア 1 は VRAM などへの書き込みとラスタの区切りをリングに入れ、コア 2 はそれを Video の写しに当てながら 1 ラスタずつ描く。
コア 1 だけで合成したときと同じ画面になる。リングが満杯か、コア 2 が 2 フレーム以上遅れたらコア 1 が待ち、フレームは飛ばさない)。
音 (OPM と ADPCM) の合成はコア 3 で行う (コア 1 は OPM と ADPCM への書き込みと進めたクロック数をリングに入れ、コア 3 がそれを当てて音を作る。
OPM のタイマや割り込み、ADPCM の転送の要求など CPU から見える状態はコア 1 が計算する。コア 1 だけで作ったときと同じ音になる)。
コア 2 / 3 が起動しなければ、その仕事はコア 1 が行う。コア 1 が起動しなければ止まる。
コア 0 は USB、SD カード、ログ、画面への転送、音の出力を受け持つ。

SD カードなどの `/pix68k/` に置くファイルの一覧 (ROM、ディスクのイメージ、`pix68k.cfg`、SRAM) は、[config-reference.md](config-reference.md) の表にある。


音は HDMI に出す。`cmdline.txt` の `pix68k_sound=pwm` で PWM (Raspberry Pi 3 と Raspberry Pi 4 Model B は 3.5mm のジャック、Raspberry Pi 5 は GPIO 12 / 13。Raspberry Pi 400 にはジャックがない)、
`pix68k_sound=i2s` で I2S の DAC (下の「I2S の DAC」を参照)、`pix68k_sound=off` で出さない。QEMU 用のビルドでは既定で出さない (QEMU の raspi3b は HDMI の音を持たない)。
`pix68k_sound=null` は試験用で、音を作って、コア 0 が実時間に合わせて取り出して捨てる (`tools/qemu_human68k.sh` が使う)。

ARM のクロックは最大にする (Circle の既定は低いクロックで、Raspberry Pi 3 Model B+ では 1.4GHz (Model B では 1.2GHz) ではなく 600MHz になる。最大は Raspberry Pi 4 Model B で 1.5GHz (後期の版の基板は `config.txt` に `arm_boost=1` を書くと 1.8GHz。Raspberry Pi OS の `config.txt` には書いてあるが、Circle の `config64.txt` にはない)、Raspberry Pi 400 で 1.8GHz、Raspberry Pi 5 で 2.4GHz)。温度を見てクロックを下げること (Circle の `CCPUThrottle::Update()`) はしていない。熱くなったときはファームウェアがクロックを下げるはず (Raspberry Pi 3 Model B+ の Linux では 60 度で 1.2GHz になった。ベアメタルではまだ確かめていない)。ファームウェアが電圧不足や温度でクロックを下げると、10 秒ごとのログに `cpu: throttled ...` の警告を出す。

Pi 5 では、ファンのコネクタ (Active Cooler、Case Fan) のファンを回す。Linux では pwm-fan ドライバが回すが、ベアメタルでは誰も回さず、4 つのコアを使い続けると温度が上がってファームウェアがクロックを下げるため。つなぎ方と速さは Raspberry Pi の Linux の device tree ([bcm2712-rpi-5-b.dts](https://github.com/raspberrypi/linux/blob/rpi-6.12.y/arch/arm64/boot/dts/broadcom/bcm2712-rpi-5-b.dts) の `cooling_fan`、`rp1_pwm1`、`thermal_trips`) に合わせた:

- ファンの PWM は RP1 の GPIO45 (alt0、PWM1 のチャネル 3。チャネルは 0 から 3 の 4 本)。出力は反転で、GPIO45 が Low の間が「回す」。周期は 41566ns (約 24kHz)
- 速さは温度で 4 段階に変える。50 / 60 / 67.5 / 75 度以上で段階 1 / 2 / 3 / 4 (速さは 255 を全開として 75 / 125 / 175 / 250) に上げ、上げた温度より 5 度下がったら 1 つ下げる。50 度に届くまでは止めている。温度は 2 秒ごとに読む。温度が読めないときは段階 4 にする
- 起動してから主ループに入るまで (数秒) は全開で回す
- PWM1 のクロックを動かせなかったときは、GPIO45 をふつうの出力にして、回す (全開) か止めるかだけにする (ログに `fan: PWM1 clock failed; on/off only`)
- `cmdline.txt` に `pix68k_fan=full` を書くといつも全開、`pix68k_fan=off` を書くとファンに触らない (既定は `pix68k_fan=auto`。ファンのコネクタがない基板や、GPIO45 をほかのことに使う基板では `off` にする)。Circle の `gpiofanpin=` を書いたときも触らない
- 10 秒ごとの `cpu:` の行の最後に、段階、速さ (255 が全開)、RP1 が測った回転数を出す (`cpu: 2400 MHz set, 2400 MHz measured, throttled 0, 52 C, fan level 1 (75/255), 2890 rpm, mem 8192 MB, heap low 150/958 MB used, high 0/7168 MB used`。数字は例)。起動のときには、ファームウェアが残した PWM1 の設定を `fan: PWM1 before: ...` の行に出す
X68000 が動き始めたら、ログは UART だけに出す。10 秒ごとに速さと、その 10 秒のフレームごとの時間
(`frames:` の行。1 フレームの CPU サイクル数、処理時間の平均と最悪、次のフレームまでの残り時間の最小値、遅れたフレームの数、起動してからコア 1 がコア 2 の合成を待った回数)、
ARM のクロック (要求した値と実測の値)、ファームウェアがクロックを下げているか (`PROPTAG_GET_THROTTLED` の値)、温度、メモリとヒープの使った量を出す (`cpu:` の行。Pi 5 はファンの段階と回転数も出す)。
ヒープは、`mem` がメモリ全体、`heap low 使った量/大きさ` が 1GB より下のヒープ (`new` と `malloc` が使う)、`high` が 1GB より上のヒープ (Pi 4 / Pi 5 でメモリが 1GB より多いときだけ。大きなディスクのイメージのキャッシュに使う)。
Circle のヒープは解放したブロックを大きさごとに取っておいて使い回すだけなので、「使った量」は今使っている量ではなく、起動してから一度でも切り出した量 (いちばん多く使ったときに近い)。残りは一度も使っていない部分で、解放した分はここに入らない。
X68000 のメモリ、ディスクのイメージ、MIDI の音源の ROM と SoundFont を読み終えて主ループに入る前に、そのときの量を `mem: after loading: ...` の行に 1 回出す (それぞれの大きさは読んだときの行に出る)。続けて `mem: framebuffer 0x...-0x... (... KB, pitch ...), ARM memory 0x0-0x... (... MB; total ... MB), heap low 0x...-0x...; framebuffer outside the ARM memory` の行に、フレームバッファの番地、ARM のメモリ (キャッシュを通る、1GB より下) とヒープの範囲を出す。フレームバッファが ARM のメモリの中にあれば `INSIDE`、ヒープと重なれば `OVERLAPS the heap` と出る (ふつうはファームウェアが ARM のメモリの外に置く)。
Pi 5 の 2GB のモデルで足りるかは、`heap low` の使った量で見当が付く (`heap low` の大きさはメモリの量によらず 1GB より少し小さい)。`high` は 2GB のモデルでは約 1GB になる。`high` に置くディスクのイメージのキャッシュは空きに合わせて大きさを決めるので、8GB のモデルの `high` の使った量は 2GB のモデルとそのまま比べられない。
`cmdline.txt` に `pix68k_textdump=1` を書くと、1 秒ごとにテキスト画面を読み、変わっていれば `text|...` の行でログに出す (`tools/qemu_human68k.sh` が使う)。`pix68k_fail_core=N` は試験用で、N 番以降のコアを起動しなかったものとして扱う。`pix68k_wfe=0` を書くと、コア 0 とコア 1 が待つ間を wfe で眠らせずに空回りで待つ (前の動きと温度や速さを比べるため。既定は wfe で眠る)。`tools/qemu_human68k.sh` では `WFE=0` を付けると渡す。`pix68k_gpu_scale=` と `pix68k_fb_rows=0` は下の画面の説明を参照 (`tools/qemu_human68k.sh` では `GPU_SCALE=`、`FB_ROWS=0`)。
USB マウスは X68000 のマウス (SCC につながるシリアルマウス)、USB ゲームパッド (2 台まで) はジョイスティック 1、2 になる。
Circle が配置を知っているゲームパッドは十字キーが上下左右、A / X がトリガ A、B / Y がトリガ B。知らないものは最初のハットと最初の 2 本の軸 (遊び 40%) が上下左右、ボタン 1 / 2 がトリガ A / B。
ハットの値が 0-7 の機種と 1-8 の機種がある (Circle は論理最小値を引かずに渡す)。Circle が配置を知っている機種は 0-7 として扱い、知らない機種はつないだときに HID のレポート記述子を読み直して論理最小値を調べる (HID 記述子に書かれた正確な長さで頼む)。読めなければ値から決める (`platform::HatDecoder`): 0 と 8 のうち先に来たほうを離した値とみなし、8 なら 0-7、0 なら 1-8 の機種とする。9 以上の値が来たら 0-7 に直す。見誤るのは押したままつないだときだけ。0-7 の機種で上を押したままつないだときは、離したときに 15 などを返す機種なら直るが、離したときに 8 を返す機種では離しても左上が入ったままになる。1-8 の機種で左上を押したままつなぐと、離しても上が入ったままになる。どちらも、つなぎ直すか `hat=` で固定すると直る。`pix68k.cfg` の `hat=0` (0-7) / `hat=1` (1-8) で固定できる。
X68000 のキーボードの LED は、つないでいるすべての USB キーボードの LED に出す (CAPS → Caps Lock、かな → Scroll Lock、INS → Num Lock。ほかは出さない。Pi 400 では INS を Num Lock に出さない。[keyboard.md](keyboard.md) を参照)。
フレームバッファの大きさは HDMI の解像度のまま (`cmdline.txt` の `width=768 height=512` で変えられる)。X68000 の画面は、コア 0 (USB を探している間は画面を合成するコア) がフレームバッファに写すときに整数倍に拡大し、真ん中に出す (`pix68k.cfg` の `scale=auto`、既定。`src/platform/frame_scale.h`)。
倍率の決め方: 768x512 を縦横同じ整数倍 (2 倍まで) で拡大して、フレームバッファに入る最大の大きさを枠とする。各画面モードの横と縦の倍率は、それぞれ枠に入る最大の整数にする。
1920x1080 では 768x512 が 2x2、512x512 が 3x2、256x256 が 6x4 で、どれも 1536x1024 になる (横と縦の倍率が違うのは、X68000 の 512 ドットや 256 ドットのモードの画素が横に長いため)。
1280x720 では、768x512 と 512x512 は拡大せず、256x256 は 3x2 で 768x512 になる。拡大は最近傍 (同じ画素を並べる) で、ぼかさない。
フレームバッファに枠がまるごと入らないときは、小さい倍率にする (はみ出す分を切り出して大きくはしない。`config.txt` の `disable_overscan=1` を参照)。3840x2160 でも 2 倍まで (1536x1024) にする (3 倍、4 倍では写す量が 9 倍、16 倍になるため)。大きく出したいときは `cmdline.txt` で `width=1920 height=1080` などにすると、ファームウェアが HDMI の解像度に引き伸ばす (縦横比と、ぼけ方は確かめていない)。
`scale=off` で拡大しない (今までどおり 1 倍で真ん中に出す)。X68000 の画面の外は黒で塗る。10 秒ごとのログに、1 フレームを写すのにかかった時間 (平均と最大、`screen: copy to the framebuffer ...`) を出す。 コア 2 で画面を合成しているときは、コア 2 が 1 フレームを合成するのに使った時間 (`screen: core 2 composed a frame in ... us on average, ... us at most`) を、コア 3 で音を合成しているときは、コア 3 が 1 フレーム分の音を合成するのに使った時間 (`sound: core 3 synthesized a frame of sound in ...`。MIDI の音源が音を作る時間を含む) も出す。記録を処理した時間だけを足したもので、待っている時間は入らない。 合成するコアが遅れて何フレーム分かの記録をまとめて処理し、フレームの区切りを 1 度に 2 つ以上見つけたときは、その間の時間をフレームの数で割った値を 1 フレームの時間とする。そのときの `at most` は、フレームごとの最大ではなく、この割った値になる。
写すときは、前に写したときの X68000 の画面の写しをふつうのメモリに持ち、1 行ずつ比べて、変わった行だけをフレームバッファに書く (`platform::FrameRowCache`。起動の間やディスクを読む間など、画面がほとんど変わらないときに写す時間が減る)。ログの `copy to the framebuffer` の行の終わりに、変わっていないので書かなかった行の割合を出す。すべての行が変わるフレーム (スクロールするゲームなど) では、フレームバッファに書く量は前と同じで、比べる時間と写しを作る時間 (768x512 で 1.5MB の読み書き) が足される。`cmdline.txt` に `pix68k_fb_rows=0` を書くと、前と同じく毎回すべての行を書く (比べるため)。
`pix68k.cfg` の `scanline=on` (既定は `off`。ベアメタル版だけ) で、縦 2 倍以上に拡大したとき、拡大した各行の最後の 1 行を半分の明るさにして、ブラウン管のスキャンラインのように見せる (1920x1080 の 768x512 なら 2 行に 1 行、256x256 なら 4 行に 1 行)。設定メニューでは Scale の行で `off`、`auto (integer)`、`auto + scanlines` から選び、すぐに反映する (主のページの行は増やせないため、Scale の行にまとめた。`scale=off` で拡大しないときは暗くしない。`off` を選んでも `scanline=` は変えない)。
縦の倍率が 1 のとき (`scale=off`、1024 ラインの画面、`pix68k_gpu_scale=2` でフレームバッファの中の倍率が 1 になるとき) は何もしない。ホスト版 (SDL2) は `scale=` と同じく使わない (ウィンドウへの拡大は SDL2 に任せているため)。
暗くした行は、拡大した 1 行と同じくふつうのメモリで作ってから書くので、フレームバッファに書く量と、読み戻しのないことは変わらない。増えるのは 1 行を暗くする計算 (1920x1080 の 768x512 で 1 フレーム 1536x512 画素) で、10 秒ごとのログの `copy to the framebuffer` の行の終わりに `; scanlines` を付けるので、あり / なしで比べられる。画面の右上の数字 (`osd=`) を X68000 の画面に重ねるときも、数字は暗くしない。
暗くする行はフレームバッファの行に合わせて入れるので、ファームウェアがフレームバッファを整数でない倍率で表示に引き伸ばすとき (`cmdline.txt` の `width=` `height=` を HDMI の解像度より小さくしたときなど) は、暗い行の間隔がそろわず、縞 (モアレ) が出ることがある。
`cmdline.txt` に `pix68k_gpu_scale=2` を書くと、フレームバッファを表示の大きさの縦横 1/2 (1920x1080 なら 960x540) にし、表示への 2 倍の拡大はファームウェア (GPU) に任せる。倍率は表示の大きさで決めるので、768x512、256x256、384x256 の画面は今と同じ 1536x1024 に出て、フレームバッファに書く量は 1/4 になる。512x512 の画面は、フレームバッファの中で 1.5 倍にはできないので 1 倍にし、1024x1024 に出る。フレームバッファより大きい画面 (1024x1024 のモードなど) は、今は 1920x1080 にそのまま入るが、960x540 からはみ出す分は切れる (真ん中を出す)。`pix68k_gpu_scale=1x2` なら縦だけ GPU が 2 倍にする (1920x540。512x512 も 1536x1024 に出て、書く量は 1/2)。表示の大きさが割り切れないか、768x512 が入らなくなるとき (オーバースキャンで 1824x984 になっているときなど) は使わず、ログに出す。既定は使わない。
GPU の拡大の補間は `config.txt` の `scaling_kernel` で決まり、何も書かないとドットがにじむことがある。`scaling_kernel=8` で最近傍になるはず (実機ではまだ確かめていない)。`pix68k_gpu_scale=1x2` で、ファームウェアがフレームバッファを縦横比を変えて表示いっぱいに引き伸ばすかも、実機ではまだ確かめていない。`tools/qemu_human68k.sh` では `GPU_SCALE=2 FB_SIZE=1920x1080` で試せる (QEMU は拡大せず、960x540 のまま出す)。
X68000 の画面の右の余白 (16:9 のモニタで左右に余る黒いところ) に、直近 1 秒の数字を出す (`pix68k.cfg` の `osd=`。既定は `on`。設定メニューの OSD でも切り替えられる)。
1 項目ずつ縦に並べ、上から次の順に出す:

```
RPi5 2.4GHz    Raspberry Pi のモデルと ARM のクロック (10 秒ごとのログの cpu: の行で測った値。測れなければ要求した値)
68030 25MHz    MPU とクロック (68000 10MHz / 16MHz、68030 25MHz、68060 50MHz など。pix68k.cfg の cpu= と clock= で決まる値)
55.5/55.4/0    直近 1 秒に合成したフレームの fps / 画面に写したフレームの fps (0.1 まで) / 写す前に次のフレームに置き換わった数
       100%    X68000 の速さ (実時間の何 %)
   HEAP 52M    1GB より下のヒープの使った量 (MB。上の cpu: の行の説明の heap low と同じ値で、一度でも切り出した量)
      52.3C    SoC の温度 (℃、0.1 度まで。127.9 度まで出す)
FAN 2200rpm    ファンの回転数 (止まっていれば FAN 0rpm)。Pi 5 でファンを動かしているときだけ。段階はログの cpu: の行と /thermal で見る
  THROTTLED    ファームウェアが ARM のクロックを下げたか、電圧が足りないとき (10 秒ごとのログの cpu: の行で読んだ値)
```

- fps は、コア 1 が X68000 のフレームを終えるたびに記録するフレームの数と時刻から求める (X68000 の 55.46Hz なら 55.5)。写した fps は、合成した fps に「写した数 / 合成した数」を掛けた値。fps と置き換わった数で 11 文字を超えるとき (`55.5/55.4/12`、100fps 以上など) は、fps を四捨五入した整数で `56/55/12` のように出す。fps は 999.9、置き換わった数と速さは 999 まで
- 回転数は 100rpm に丸め、前に出した値から 100rpm 以上変わったときだけ変える (回転数は毎秒少しずつ変わるので、そのたびに描き直さないため)。10000rpm 以上は `FAN 12.3k` のように 0.1k (100rpm) 単位で出す (20000rpm まで)
- Raspberry Pi のモデルの名前は Circle の `CMachineInfo` から決める: `RPi3` (3B)、`RPi3+` (3B+)、`RPi3A+` (3A+)、`Zero2` (Zero 2 W)、`CM3`、`CM3+`、`RPi4` (4B)、`Pi400`、`CM4`、`CM4S`、`RPi5`、`Pi500`、`CM5`、`CM5L` (CM5 Lite)、ほかは `Pi`。クロックは 1GHz 以上なら 0.1GHz 単位 (`2.4GHz`)、より下なら `700MHz`。11 文字に入らなければ、空白を詰め (`RPi3+1.4GHz`。名前が数字で終わるときは詰めない)、それでも入らなければ `GHz` を `G` にする (`Pi400 1.8G`、`RPi3A+ 1.4G`)
- MPU とクロックの行は、3 桁の MHz なら `68060 100M` のように縮める。値は起動したときと設定を変えたときにしか変わらないので、この行のために描き直すことはない (描き直すのは、ほかの数字が変わったときの 1 秒に 1 回まで)
- 最高速モード (`max_speed=on`) のときは、MPU の行のクロックを実効の値 (クロック × 今の速さの % / 100) にして `~` を付ける (`68060 ~210M`、`68000 ~20M`、`68000 ~9MHz`、1000MHz 以上は `68060 ~1.2G`)。上 2 桁に丸め (100MHz より下は 1MHz 単位)、前に出した値から、丸めの単位、出した値の 5% (MHz に四捨五入。52MHz なら 3MHz、89MHz なら 4MHz、210MHz なら 11MHz)、2MHz のいちばん大きい幅以上変わったときだけ変える (40MHz より下は 2MHz。10MHz の 204% と 205% を行き来しても 20 のまま)。丸めるのはクロック × % を kHz で求めたあとの 1 回だけ (75MHz の 286% は 214.5MHz なので 210) (速さは 0.25 秒ごとに少しずつ変わるので、丸めの境目で数字が行き来しないため)。060turbo の値は、選んだクロックの実機の 68060 で何 MHz にあたるか。ホスト版は横 1 行で MPU の行を出さないので、変わらない

- 文字の大きさは余白の幅で決める。1 行は 11 文字 (88 ドット) で、右に 8 ドット空けるので、余白が 96 ドットあれば 1 倍、192 ドットあれば 2 倍、288 ドットあれば 3 倍 (上限) の文字にする。1920x1080 で X68000 の画面を 2 倍にしたときは、右の余白が 192 ドットなので 2 倍の文字になる
- 右に余白がないとき (X68000 の画面がモニタの横いっぱいのとき): 上に余白があれば、そこに横 1 行 (`55.5/55.4/0 100% 52.3C THR`。MPU、ヒープ、ファンは幅を取りすぎるので出さない) で出す。上にもなければ、`osd=on` では出さない。`osd=fps` では、右上に横 1 行で X68000 の画面に重ねる。横 1 行の幅は 23 文字 (184 ドット。いちばん長い `999/999/999 999% 127.9C` が入る) で、`THR` は出すときだけ、その分 (4 文字) 広げる
- `osd=off` で出さない
- 既定の `osd=on` で出るかは、モニタの大きさと X68000 の画面モードで決まる (`tests/unit/osd_test.cpp` の `testCoverage()` が表にする)。16:9 のモニタ (1280x720 以上。`pix68k_gpu_scale=2` / `1x2` の 1920x1080 も) では、どの画面モードでも縦に並べて出る (`pix68k_gpu_scale=2` の 1024x1024 の画面を除く)。1920x1200、1280x800、1280x1024 でも縦に並ぶ。1600x1200 の 768x512 などは上の余白に横 1 行で出る。出ないのは、余白が左右とも 96 ドットより狭く、上下も 20 ドットより狭いとき (1680x1050 で 768x512 を 2 倍にしたとき、640x480 のモニタなど) で、そのときは `osd=fps` にすれば X68000 の画面に重ねて出る
- 描くのは、X68000 の画面をフレームバッファに写すところ (`src/platform/osd.h`)。X68000 の VRAM や合成した画面には描かないので、`/screen.ppm`、ステート、エミュレーションの結果は変わらない。余白に出すときは、数字が変わったとき (1 秒に 1 回) だけ描く

数字はコア 0 が 1 秒ごとに数え、変わったときだけ描き直す。重ねるときは、X68000 の画面をフレームバッファに 1 行ずつ写すときに、数字と重なる部分だけを数字の絵に差し替える (X68000 の画面のデータは変えない)。

ホスト版でも同じ設定で出せる (ウィンドウの右上に横 1 行。`osd=on` では X68000 の画面に重なるときは出さない。合成したフレームはすべて出すので置き換わった数は 0 で、温度は出さない)。

ROM がないとき (M0 の動作) は、boot パーティションのファイル一覧と `/pix68k/hello.txt` の中身、コア 1 での CPU コアの速度計測、USB キーボード・マウス・ゲームパッドの入力をログに出し、画面の右上にカラーバーを描く。

Linux の入った実機 (ssh で入れるもの) では、`tools/deploy_pi.sh <3|4|5> <ホスト名> <秒数>` でベアメタル版を 1 回だけ起動できる。
ベアメタル版は指定の秒数動いたら、ログを boot パーティションの `/pix68k/log.txt` に書いて再起動し、Linux に戻る。スクリプトはそのログを取ってきて表示する。
ファームウェアの tryboot (次の 1 回だけ `tryboot.txt` で起動する機能、全モデル対応) を使うので、ベアメタル版が止まっても次の再起動で Linux に戻る。止まったときはウォッチドッグ (15 秒) で再起動する。

Homebrew の `aarch64-elf-gcc` は C ライブラリのヘッダを持たないので、FatFs が使う `<string.h>` は `baremetal/shim/` で Circle の関数に置き換えている。

## I2S の DAC

`pix68k_sound=i2s` で、GPIO につないだ I2S の DAC に 48kHz の音を出す (Pi 3 / Pi 4 / Pi 5。Pi 5 は RP1 の I2S で、ピンは同じ)。
Pi がビットクロックと LR クロックを出し、DAC はそれに従う。実機ではまだ確かめていない (#107)。

| DAC の端子 | Pi の GPIO | ヘッダのピン |
| --- | --- | --- |
| BCK (BCLK) | GPIO 18 | 12 |
| LRCK (LCK、WS) | GPIO 19 | 35 |
| DIN (DATA) | GPIO 21 | 40 |
| SDA (I2C で設定する DAC だけ) | GPIO 2 | 3 |
| SCL (同上) | GPIO 3 | 5 |
| VIN / GND | 3.3V か 5V (基板に合わせる) / GND | 1 か 2 / 6 など |

GPIO 20 (ヘッダの 38 番ピン) も I2S の入力として割り当てるので、ほかの用途に使わない。

`pix68k_i2s_dac=` で DAC の設定のしかたを選ぶ。

- `auto` (既定): I2C (GPIO 2 / 3) で PCM512x (PCM5122 など、番地 0x4C / 0x4D) と WM8960 (0x1A) を探し、見つかれば
  設定する。どれもなければ、設定の要らない DAC として音を送る。
- `none`: I2C を使わない (GPIO 2 / 3 に触らない)。PCM5102A のような、設定の要らない DAC 向け。
- `0x4c` のような I2C の番地: その番地の PCM512x か WM8960 を設定する。見つからなければ音を出さない。

PCM5102A の基板は、SCK を GND につなぐ (基板の SCK のジャンパーかはんだのパッド) と、BCK から内部でクロックを作る。
ほかの端子は FLT = L、DEMP = L、XSMT = H (L だと消音のまま)、FMT = L (I2S) にする (裏のはんだのパッドが開いたままの基板もある)。WM8960 の基板は MCLK の水晶を持つもの
(Waveshare の WM8960 Audio HAT など) を想定している。起動時のログの `sound:` の行に、I2C で DAC を見つけて設定したか
(`sound: i2s DAC configured over I2C`)、設定せずに送っているか (`sound: i2s without DAC control ...`) が出る。
QEMU (raspi3b) では、`pix68k_sound=i2s` も `pwm` も音のデバイスを動かすところで止まり、X68000 が動き出さない。

## SD カードの用意

1. SD カードを FAT32 でフォーマットする (MBR の第 1 パーティション)
2. Raspberry Pi のファームウェア (`bootcode.bin` (Pi 3 だけ)、`start*.elf`、`fixup*.dat`、Pi 4 / Pi 400 / Pi 5 は `bcm27*.dtb` も。Circle の `boot/` で `make` を実行するとダウンロードする) と `config.txt` (上を参照) をルートに置く
3. `tools/build_baremetal.sh <3|4|5>` で作ったカーネル (`kernel8.img`、`kernel8-rpi4.img`、`kernel_2712.img`) をルートに置く
4. `/pix68k/` を作り、ROM (`IPLROM.DAT`、`CGROM.DAT`。X68030 は `IPLROM30.DAT`) と、ディスクのイメージを置く。ROM はルートに置いてもよい
5. 必要なら `/pix68k/pix68k.cfg` を書く (下の例と、[config-reference.md](config-reference.md) の表の `pix68k.cfg` の行を参照)

```
# /pix68k/pix68k.cfg (1 行に 1 つ「キー=値」。# から行末までは注釈)
fd0 = HUMAN302.XDF
fd1 = GAME.XDF
clock = 16
ram = 4
```

キーの書き方の決まりは [config-reference.md](config-reference.md) にある。

## USB メモリから読み書きする

SD カード (Pi 5 は NVMe も) がないか、その boot パーティションに `/pix68k` のディレクトリがないときは、USB メモリの
最初の FAT のパーティションの `/pix68k/` を、SD カードのときと同じように使う (ROM、`pix68k.cfg`、ディスクのイメージ、SRAM、
ステートの読み書き。置くファイルは [config-reference.md](config-reference.md) の表と同じ)。Linux を入れた USB メモリなら、1 つ目のパーティション
(Linux では `/boot/firmware`) がそれに当たるので、Linux から `/boot/firmware/pix68k/` にファイルを置ける
(カーネルを LAN から起動し、SD カードを挿していない Pi 4 など)。

- 選ぶ順は NVMe (Pi 5)、SD カード、USB メモリ。`/pix68k` のディレクトリがある最初のドライブを使う。どれにもなければ、
  マウントできた最初のドライブを使う (`src/platform/boot_drive.h`、ctest の `boot_drive_test`)。
  NVMe か SD カードに `/pix68k` があれば USB メモリは探さず、起動の流れは前と変わらない
- `cmdline.txt` の `pix68k_drive=usb` / `sd` / `nvme` で、使うドライブを 1 つに決められる (ほかのドライブは初期化しない。既定は `auto`)
- 使うドライブはログの `boot drive: USB:` の行に出る。USB メモリを探したときは、その前に
  `USB drive: mounted 1234 ms after the USB scan started (/pix68k found)` が出る (2 本目からは
  `USB drive: USB2: (umsd2) mounted 1500 ms after the USB scan started (/pix68k found)` の形。`/pix68k/id.txt` があれば、その値も出る)
- USB メモリは Circle の USB 大容量記憶のドライバ (`umsd1`〜`umsd3`) と FatFs の `USB:`、`USB2:`、`USB3:` のボリュームで
  読み書きする。FatFs はパーティション表 (MBR) の 4 つのうち、最初の FAT のパーティションをマウントする (2 つ目以降の
  ext4 は読まない。GPT のパーティション表と exFAT には対応していない)
- USB メモリは 3 本まで調べ、上と同じ規則で 1 本を選ぶ: `/pix68k` のある最初のもの (`cmdline.txt` に `pix68k_id` があれば、
  `/pix68k/id.txt` の値が一致するもの)、どれにもなければマウントできた最初のもの。Linux の入った USB メモリと、データ用の
  USB メモリを両方挿していても、`/pix68k` を置いたほうを使う (どちらが `umsd1` になるかは USB のポートとつなぐ順で決まる)。
  選ばなかった USB メモリはアンマウントし、読み書きしない
- USB の装置は、ふだんは X68000 を動かし始めてから探すが、USB メモリを使うときは ROM を読む前に探すので、
  最初の画面が出るのはその分遅くなる。探し終えたあとも、規則で決まる USB メモリ (`/pix68k` があるか、`pix68k_id` の値が
  一致する) が出てくるか、USB メモリが出てきて装置の構成が 0.5 秒変わらなくなるまで待つ (ハブの先の USB メモリは、直結の
  ものより遅れて出てくるため)。長くても `cmdline.txt` の `pix68k_usb_wait=ミリ秒` (既定 2000) まで
- USB メモリを使っている間は、ほかの USB の転送 (キーボードやハブをつないだり外したりしたときの処理、キーボードの LED) の
  途中では、ディスクのイメージを読み書きしない (転送から戻ってから行う)。SD カードのときは、前と同じく途中でも読み書きする
- USB メモリの FAT のパーティションには Linux の起動ファイルが入っていることがあるので、USB メモリでは `/pix68k/` の下だけに書く
  (`pix68k.cfg`、SRAM、ステート、ディスクの書き戻し、deploy のログ)。`pix68k.cfg` に `/` で始まる名前で書いた `/pix68k/` の外の
  イメージは、読めるが書き込み禁止になる。ネットワークの口 (`net=on`) からの `/pix68k/` の外への書き込みと削除は断る (403)。
  SD カードと NVMe のときは前と同じで、制限しない
- ハードディスクと SCSI のイメージの書き戻しは、ファイルの大きさを変えずに中身を上書きするので、空きを使わない。
  フロッピーディスクのイメージ、`pix68k.cfg`、ステートは、別の名前 (`.new`) に書き終えてから置き換えるので、空きが足りずに
  失敗しても前のファイルが残り、書きかけのファイルは消す。USB メモリでは、新しく書くファイル (ステート、`pix68k.cfg`、SRAM、
  フロッピーディスクのイメージの `.new`、ネットワークの口からのファイル) は、書いたあとに 32MB の空きが残るときだけ書く。
  deploy のログ (`log.txt`) と `/pix68k` のディレクトリを作るときも同じ。書く大きさは FAT のクラスタの倍数に切り上げ、
  新しい名前を作る操作 (新しいファイル、`.new` / `.part`、名前の変更、途中で止まった置き換えを戻す名前の変更、
  ディレクトリの作成) には、ディレクトリが伸びる分の余裕として 64KiB (クラスタに切り上げ、最低 4 クラスタ) を足して数える。
  FAT は、ディレクトリのエントリが埋まっていると名前を足すときにディレクトリを伸ばす。長い名前は最大 255 文字で
  20 エントリ (640 バイト) なので、1 回の保存の作成と名前の変更を合わせても 64KiB で足りる (伸びるかどうかは調べず、
  いつも足す)。0 バイトのファイルも同じ。余裕が残らなければ、途中で止まった置き換えも戻さず、そのままにする。
  ネットワークの口から受け取っている途中のファイルは、まだ書いていない残りも使ったものとして数える
  (受け取りの途中でステートを保存しても、両方が終わったあとに 32MB が残る)
- 空きは FatFs の `f_getfree()` で読む。FAT32 では FSInfo に書かれた空きの数をそのまま使うので、Linux が正しく
  アンマウントせずに終わったあとなどは、実際の空きとずれていることがある
- SD カードか NVMe をマウントできたが `/pix68k` のディレクトリがないとき (ROM をルートに置いているとき) も、ROM を読む前に
  USB の装置を探し、USB メモリを待つ。このときは、キーボードやマウスの登録が、USB の装置を探した直後ではなく、
  X68000 を動かし始めたあとの主ループの最初の周回になる
- 動かしている間は USB メモリを抜かない (開いているディスクのイメージを読み書きできなくなる)

## 起動にかかる時間

ベアメタル版は、起動の各段階の時刻をログに出す (`boot time: ... at N ms since power-on (+M ms)`。+M は、時刻がすぐ前の段階からの時間。
`first frame shown` は USB の装置を探し終えてから出すので、ログの順と時刻の順は違う)。
時刻は Circle が読む ARM のカウンタの値で、電源を入れたときから数えているはずだが、ファームウェアの時間がどこまで入るかは実機で確かめていない。
段階は次のとおり。

| ログ | 中身 |
| --- | --- |
| `kernel started` | カーネルに入った (ファームウェアの時間がここに入るかは未確認) |
| `screen, serial and timer ready` | HDMI の画面、UART、割り込み、タイマを用意した |
| `boot drive mounted` | SD カード (Pi 5 は NVMe も) を初期化してマウントした |
| `reading pix68k.cfg` | 設定を読み始める |
| `ROMs read` | IPL ROM と CGROM を読み終えた |
| `X68000 set up` | X68000 を用意し、音の出力を始めた |
| `disk images read` | フロッピーディスクのイメージを読み、ハードディスクのイメージを開いた |
| `core 1 started` | コア 1 で X68000 が動き始めた (IPL ROM が動き出す) |
| `first frame shown` | X68000 の最初の画面を出した |
| `USB devices scanned` | USB の装置を探し終えた |
| `keyboard attached` | 最初のキーボードが見つかった |

起動を速くするためにしていること:

- USB の装置は、X68000 を動かし始めてから探す。Circle は装置を探すときにハブの電源を入れて 510ms 待つなど決まった時間を待つので、
  Raspberry Pi 3 Model B+ (ハブが 2 段と LAN7800) では 2 秒ほどかかる見込み (QEMU では 2.6〜2.9 秒)。探している間もコア 1 は X68000 を動かす。
  コア 0 は探し終えるまで USB の転送で止まるので、その間は画面を合成するコア (コア 2。`render_core=off` のときと、コア 2 が
  起動しなかったときはコア 1) が、合成したフレームを自分でフレームバッファに写す。コア 0 は Circle の待ち (`CTimer::MsDelay()`) の間に、
  ハードディスクのイメージのチャンクを読み、書き戻す (`tools/build_baremetal.sh` が Circle の写しの `timer.h` を書き換え、
  `MsDelay()` から pix68k の関数を呼ぶ)。探し終えたあとも、装置をつないだり外したりしたあとの処理 (`UpdatePlugAndPlay()`。QEMU で
  0.1 秒ほど) と、キーボードの LED を送る転送 (QEMU で 30ms ほど) の間は、同じように合成するコアが写す。設定メニューを開く途中と
  開いている間は、コア 0 がメニューを重ねて描くので任せない。写す役の受け渡しの順序は `src/platform/frame_handoff.h` に書いた。
  QEMU では、最初の 10 秒に表示する前に置き換わったフレームは 0 (ログの `screen: ... replaced before shown`。コア 0 が待ちの間にだけ写して
  いたときは、探している 3 秒ほどの間に 35〜90 フレーム)。コア 1 が写すときは、1 フレームにつき画面を 1 回写す分だけ X68000 を動かす時間が減る
  (実機では測っていない)。
  キーボードなどは探し終えてから使える。あとからつないだキーボードも使える (QEMU で、何もつながっていないときと、ハブとマウスだけのときに確かめた)。
  キーボードが見つかるのは IPL ROM が動き出してから 2〜3 秒後なので、IPL ROM が電源を入れた直後に見るキー (OPT.1 を押したまま
  起動するなど) は X68000 に届かない (実機では確かめていない)。
  Pi 3 で `neptune=on` のときだけは、有線の Ethernet が USB につながっているので、X68000 を動かす前に探す。
  Pi 4 の Ethernet (GENET) は USB を通らない。Pi 5 の Ethernet (MACB) と PWM / I2S の音 (`pix68k_sound=pwm` / `i2s`) は RP1 を使うが、
  RP1 (Circle の `CSouthbridge`) は Circle の `sysinit()` が `main()` の前に初期化するので、USB より前に使ってよい
  (`CUSBSubSystem` の中の `CSouthbridge` は、初期化済みかどうかを返すだけ)
- ハードディスクと CD-ROM のイメージは全体を読まず、使うチャンクだけを読む ([disks-and-scsi.md](disks-and-scsi.md) の「ディスクの書き戻しと大きなイメージ (ベアメタル版)」を参照)
- 起動のときに ACT LED を 3 回点滅させるのをやめた (Circle の `CActLED::Blink()` は点滅の間 2.1 秒待つ)
- Circle の遅延ループの較正 (`CALIBRATE_DELAY`) をしない (QEMU で 0.2 秒、Pi 3 の 600MHz では 0.5 秒ほどの見込み。
  較正の結果を使う `nsDelay()` は Circle も pix68k も使っていない)。ログの `timer: SpeedFactor is ...` は出なくなった

QEMU (raspi3b) で、Human68k 3.02 のフロッピーディスクとハードディスク (20MB の SASI のイメージ) から起動したときの時間 (ms。3 回ずつ):

| | core 1 started | first frame shown | 「A>」が出る |
| --- | --- | --- | --- |
| 変える前、FD | 5480〜5560 | 5590〜5630 | 12500〜12600 |
| 変えたあと、FD | 250〜300 | 270〜330 | 7000〜7200 |
| 変える前、HD | 9670〜10080 | 9750〜10160 | 19200〜20300 |
| 変えたあと、HD | 310〜420 | 360〜490 | 11700〜14100 |

測ったマシンはほかの作業で負荷が高く (ロードアベレージ 20 前後)、QEMU の速さがばらついた。HD の行は、変える前と後を交互に起動して測った。
「A>」はテキスト画面を 1 秒ごとに読んで見ているので、さらに 1 秒ほどの幅がある。QEMU の値は目安で、実機の値はまだ測っていない。

ファームウェアの設定 (実機では確かめていないので、推奨にとどめる):

- `config.txt` の `disable_overscan=1`: ファームウェアのオーバースキャン (画面の上下左右を空けて、フレームバッファをその分小さくする) をやめる。
  書かないと、1920x1080 の HDMI のモードではフレームバッファが 1824x984 などになり、768x512 の 2 倍 (1536x1024) が入らないので、X68000 の画面を拡大しない
  (Raspberry Pi 3 Model B+ と 1920x1080 のキャプチャ装置で、書いていないときに 1 倍になった)。起動時のログの `screen: overscan ...` でオーバースキャンが有効かが分かる
- `config.txt` の `disable_splash=1`: 虹色の画面を出さない
- `config.txt` の `boot_delay=0`: カーネルを読む前に待たない。Raspberry Pi 3 Model B+ の Raspberry Pi OS で `vcgencmd get_config boot_delay` は、
  書いていなくても 0 だったので、いまのファームウェアでは書かなくても同じかもしれない
- Pi 4 / Pi 400 / Pi 5 は、EEPROM の `BOOT_ORDER` で SD カード (または NVMe) を先に探すようにする (ネットワークや USB から起動しようとして待たない)
- 試す価値はあるが、確かめていないもの: `config.txt` の `initial_turbo=N` (起動から N 秒、クロックを上げる。Circle の `config64.txt` は
  `initial_turbo=0` にしている)、`cmdline.txt` の `usbpowerdelay=N` (Circle がハブの電源を入れてから待つ時間 (既定 510ms。Pi 4 / Pi 5 の
  xHCI のポートは 300ms) を N ms にする。短すぎると見つからない装置があるかもしれない。いまは X68000 が動き出すまでの時間には入らず、
  キーボードが使えるようになるまでの時間だけが変わる)

参考: Raspberry Pi 3 Model B+ の Raspberry Pi OS の起動 (`sudo vclog -m` のファームウェアのログ) では、start.elf が 1.27 秒から動き、
10MB の Linux のカーネルを 1.7 秒かけて読み、5.95 秒で ARM を動かしていた。pix68k のカーネルは 0.7MB で、initramfs もないので、もっと短いはず。
