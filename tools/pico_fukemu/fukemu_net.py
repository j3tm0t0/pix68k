#!/usr/bin/env python3
"""無線 LAN の OTA のビルドの pico_fukemu に、設定とディスクイメージを TCP で書く (W 命令)。

  fukemu_net.py pix68k-fukemu.local --fd1 fd1.xdf --stretch 1:1:0:8:2.3

選べるものは mkimg.py と同じ (--fd0/--fd1/--wp0/--wp1/--erase0/--erase1/--config/--delivery/--rule/--stretch ...)。
書き終えたら R 命令で再起動する (--no-reboot で再起動しない。新しい中身は再起動のあとに使われる)。
ファームの更新は tools/pico_kbd/picocmd.py --ota を使う。トークンは表示しない。
"""
import argparse
import base64
import os
import socket
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkimg  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=2323)
    ap.add_argument("--token-file", default=os.path.expanduser("~/.config/pix68k-pico/tcp_token.txt"))
    ap.add_argument("--timeout", type=float, default=10)
    ap.add_argument("--no-reboot", action="store_true", help="書き終えても再起動しない")
    mkimg.add_args(ap)
    args = ap.parse_args()
    regions = mkimg.regions_for(args)
    try:
        with open(args.token_file, "rb") as f:
            token = f.readline().strip()
    except OSError as e:
        sys.exit("fukemu_net: トークンのファイルを読めなかった: %s" % e)

    jobs = []  # (B 命令の引数 or X 命令, データ)
    for off, data in regions:
        if off == mkimg.CFG_OFF:
            jobs.append(("B config %d %08x" % (len(data), zlib.crc32(data)), data))
            continue
        unit = 0 if off == mkimg.slot_off(0) else 1
        if len(data) == mkimg.SLOT_HDR:  # --eraseN
            jobs.append(("X fd%d" % unit, None))
            continue
        img = data[mkimg.SLOT_HDR:]
        wp = data[12] & 1
        jobs.append(("B fd%d %d %08x%s" % (unit, len(img), zlib.crc32(img), " wp" if wp else ""), img))

    try:
        sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
    except OSError as e:
        sys.exit("fukemu_net: つなげなかった (%s:%d): %s" % (args.host, args.port, e))
    pending = b""

    def reply(prefix, wait):
        """prefix で始まる応答を待つ。"#pico write error" なら止める"""
        nonlocal pending
        deadline = time.monotonic() + wait
        while True:
            while b"\n" not in pending:
                left = deadline - time.monotonic()
                if left <= 0:
                    sys.exit("fukemu_net: 応答がない (%s)" % prefix)
                sock.settimeout(left)
                try:
                    d = sock.recv(4096)
                except socket.timeout:
                    continue
                if not d:
                    sys.exit("fukemu_net: 相手が切った (トークンが違うか、ほかの相手がつないでいる)")
                pending += d
            line, pending = pending.split(b"\n", 1)
            line = line.rstrip(b"\r").decode("utf-8", "replace")
            if line.startswith("#pico write error") or line.startswith("#pico error"):
                # "#pico error unknown-command" は OTA なしのビルド (W 命令がない)
                sys.exit("fukemu_net: " + line)
            if line.startswith(prefix):
                return line

    try:
        sock.sendall(token + b"\n")
        reply("#pico auth ok", args.timeout)
        for head, data in jobs:
            sock.sendall(("W " + head + "\n").encode())
            if data is None:
                print(reply("#pico write erased", args.timeout), flush=True)
                continue
            print(reply("#pico write begin", args.timeout), flush=True)
            step = 336
            t0 = time.monotonic()
            for pos in range(0, len(data), step):
                sock.sendall(("W D " + base64.b64encode(data[pos:pos + step]).decode() + "\n").encode())
                reply("#pico write ok", 30)
                if pos // step % 500 == 0:
                    print("write %d / %d (%.0f 秒)" % (pos, len(data), time.monotonic() - t0), flush=True)
            sock.sendall(b"W E\n")
            print(reply("#pico write done", 60), flush=True)
        if not args.no_reboot:
            sock.sendall(b"R\n")
            print(reply("#pico reboot", args.timeout), flush=True)
    except OSError as e:
        sys.exit("fukemu_net: 接続が切れた: %s" % e)
    finally:
        sock.close()


if __name__ == "__main__":
    main()
