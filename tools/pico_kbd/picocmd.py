#!/usr/bin/env python3
"""無線 LAN のビルドの pico_kbd に、TCP で命令を送る。

使い方: picocmd.py [--port 2323] [--token-file ファイル] [--wait 秒] [--log] ホスト 命令...
  例: picocmd.py pix68k-pico P
      picocmd.py --wait 10 pix68k-pico 'J L'       # "power done" まで読む
      picocmd.py --log --wait 30 pix68k-pico       # 命令を送らず、試験機のログを 30 秒読む
      picocmd.py --ota pico2_w_kbd_ota.bin pix68k-pico   # 無線 LAN 越しに更新する (OTA のビルドだけ)
命令を 1 つ送るたびに、応答 (#pico ...) を 1 行待って表示する。--wait は、最後の応答のあとも読み続ける秒数。
--log を付けると、試験機のログ ("> " で始まる行) も表示する。トークンは表示しない。
"""
import argparse
import base64
import os
import socket
import sys
import time
import zlib


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=2323)
    ap.add_argument("--token-file", default=os.path.expanduser("~/pico-token.txt"))
    ap.add_argument("--wait", type=float, default=0)
    ap.add_argument("--timeout", type=float, default=5, help="応答を待つ秒数")
    ap.add_argument("--log", action="store_true")
    ap.add_argument("--ota", metavar="BIN", help="このイメージ (.bin) を O 命令で送って更新する")
    ap.add_argument("--ota-force", action="store_true", help="TBYB の付いていないイメージでも入れる (O E F)")
    ap.add_argument("host")
    ap.add_argument("commands", nargs="*")
    args = ap.parse_args()
    # ファイルは、つなぐ前に読んでおく (読めなかったことを、接続の失敗と取り違えないように)
    try:
        with open(args.token_file, "rb") as f:
            token = f.readline().strip()
    except OSError as e:
        sys.exit("picocmd: トークンのファイルを読めなかった: %s" % e)
    image = None
    if args.ota:
        try:
            with open(args.ota, "rb") as f:
                image = f.read()
        except OSError as e:
            sys.exit("picocmd: イメージを読めなかった: %s" % e)

    try:
        sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
    except OSError as e:
        sys.exit("picocmd: つなげなかった (%s:%d): %s" % (args.host, args.port, e))
    pending = b""

    def read_line(deadline):
        """1 行を返す。deadline までに来なければ None。相手が切ったら EOFError"""
        nonlocal pending
        while b"\n" not in pending:
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            sock.settimeout(left)
            try:
                data = sock.recv(4096)
            except socket.timeout:
                return None
            if not data:
                raise EOFError
            pending += data
        line, pending = pending.split(b"\n", 1)
        return line.rstrip(b"\r").decode("utf-8", "replace")

    def read_reply(deadline):
        """応答 (#pico ...) を 1 行待つ。ログの行は --log のときだけ表示する"""
        while True:
            line = read_line(deadline)
            if line is None:
                return None
            if line.startswith("#pico "):
                return line
            if args.log:
                print(line, flush=True)

    try:
        sock.sendall(token + b"\n")
        if read_reply(time.monotonic() + args.timeout) != "#pico auth ok":
            sys.exit("picocmd: トークンが通らなかった")
        for cmd in args.commands:
            sock.sendall(cmd.encode() + b"\n")
            reply = read_reply(time.monotonic() + args.timeout)
            if reply is None:
                sys.exit("picocmd: 応答がない: " + cmd)
            print(reply, flush=True)
        if image is not None:

            def ota(cmd, wait=args.timeout, head="#pico ota "):
                """O 命令を送り、"#pico ota ..." の応答を待つ (ほかの応答は表示して読み飛ばす)"""
                sock.sendall(cmd.encode() + b"\n")
                deadline = time.monotonic() + wait
                while True:
                    reply = read_reply(deadline)
                    if reply is None:
                        sys.exit("picocmd: 応答がない: " + cmd[:20])
                    if reply.startswith("#pico ota error"):
                        sys.exit("picocmd: " + reply)
                    if reply.startswith(head):
                        return reply
                    print(reply, flush=True)

            print(ota("O", head="#pico ota version="), flush=True)
            print(ota("O B %d %08x" % (len(image), zlib.crc32(image))), flush=True)
            step = 336
            for pos in range(0, len(image), step):
                ota("O D " + base64.b64encode(image[pos:pos + step]).decode(), wait=30)
                if pos // step % 100 == 0:
                    print("ota %d / %d" % (pos, len(image)), flush=True)
            print(ota("O E F" if args.ota_force else "O E", wait=30), flush=True)
            # 新しい版は、トークンを通った相手が来たときに確定する。起動から約 16.7 秒以内につなぎ直す
            sock.close()
            print("送り終えた。Pico が再起動するので、つなぎ直す", flush=True)
            pending = b""
            give_up = time.monotonic() + 120  # RP2040 は、ブートローダの写しに 30 秒ほどかかる
            while True:
                time.sleep(1)
                try:
                    sock = socket.create_connection((args.host, args.port), timeout=1)
                    sock.sendall(token + b"\n")
                    if read_reply(time.monotonic() + 3) == "#pico auth ok":
                        break
                    sock.close()
                except (OSError, EOFError):
                    pass
                if time.monotonic() > give_up:
                    sys.exit("picocmd: 再起動のあと、つなぎ直せなかった")
            time.sleep(1)  # 確定 (フラッシュの書き換え) が終わるのを待つ
            print(ota("O", head="#pico ota version="), flush=True)
            print("version が送った版の番号で、tbyb=00 なら確定済み。前の番号のままなら、前の版に戻っている", flush=True)
        end = time.monotonic() + args.wait
        while True:
            reply = read_reply(end)
            if reply is None:
                break
            print(reply, flush=True)
    except EOFError:
        sys.exit("picocmd: 相手が切った (トークンが違うか、ほかの相手がつないでいる)")
    except OSError as e:
        # 受け付けを断られたとき (ほかの相手がつないでいる、違うトークンが来たあとの 1 秒の間) は、ここに来ることが多い
        sys.exit("picocmd: 接続が切れた (ほかの相手がつないでいるか、違うトークンのあとの 1 秒の間か、Pico が再起動した): %s" % e)
    finally:
        sock.close()


if __name__ == "__main__":
    main()
