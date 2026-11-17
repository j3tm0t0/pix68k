#!/usr/bin/env python3
"""picocmd.py と mkuf2.py を、手元の偽のサーバと一時ファイルで試す (run_host_tests.sh から呼ぶ)。

使い方: test_tools_host.py
"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
TOKEN = b"test-token-0123"
fails = 0


def check(ok, what):
    global fails
    if not ok:
        fails += 1
        print("NG " + what)


def run(script, *args):
    r = subprocess.run([sys.executable, os.path.join(HERE, script)] + list(args), capture_output=True, text=True, timeout=30)
    return r.returncode, r.stdout, r.stderr


def serve(handler):
    """1 回だけ接続を受ける偽の Pico。ポートを返す"""
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    s.listen(1)

    def loop():
        c, _ = s.accept()
        try:
            handler(c)
        finally:
            c.close()
            s.close()

    threading.Thread(target=loop, daemon=True).start()
    return s.getsockname()[1]


def read_line(c):
    buf = b""
    while not buf.endswith(b"\n"):
        d = c.recv(1)
        if not d:
            break
        buf += d
    return buf.strip()


def good(c):  # トークンが合えば、P に答える
    if read_line(c) != TOKEN:
        return
    c.sendall(b"#pico auth ok\n")
    if read_line(c) == b"P":
        c.sendall(b"> log line\n#pico alive mounted\n")
    read_line(c)


def reset(c):  # つないですぐ、RST で切る (受け付けを断るときの Pico と同じ)
    c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))


def close_after_token(c):  # 違うトークン: 何も返さずに閉じる
    read_line(c)


def test_picocmd(tmp):
    tok = os.path.join(tmp, "token.txt")
    with open(tok, "wb") as f:
        f.write(TOKEN + b"\n")
    no_trace = lambda err: "Traceback" not in err

    rc, out, err = run("picocmd.py", "--token-file", tok, "--port", str(serve(good)), "127.0.0.1", "P")
    check(rc == 0 and out == "#pico alive mounted\n", "picocmd: 命令と応答 (%r %r %r)" % (rc, out, err))

    # 待っている相手がいないポート
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    rc, out, err = run("picocmd.py", "--token-file", tok, "--port", str(port), "127.0.0.1", "P")
    check(rc == 1 and "つなげなかった" in err and no_trace(err), "picocmd: つなげない (%r %r)" % (rc, err))

    rc, out, err = run("picocmd.py", "--token-file", tok, "--port", str(serve(reset)), "127.0.0.1", "P")
    check(rc == 1 and ("接続が切れた" in err or "相手が切った" in err) and no_trace(err), "picocmd: すぐ切られる (%r %r)" % (rc, err))

    rc, out, err = run("picocmd.py", "--token-file", tok, "--port", str(serve(close_after_token)), "127.0.0.1", "P")
    check(rc == 1 and "相手が切った" in err and no_trace(err), "picocmd: トークンのあと閉じられる (%r %r)" % (rc, err))

    # ファイルを読めないのは、接続の失敗とは別の文にする (つなぐ前に分かる)
    rc, out, err = run("picocmd.py", "--token-file", tok, "--port", str(port), "--ota", os.path.join(tmp, "none.bin"), "127.0.0.1")
    check(rc == 1 and "イメージを読めなかった" in err and "接続" not in err and no_trace(err), "picocmd: イメージがない (%r %r)" % (rc, err))
    rc, out, err = run("picocmd.py", "--token-file", os.path.join(tmp, "none.txt"), "--port", str(port), "127.0.0.1", "P")
    check(rc == 1 and "トークンのファイルを読めなかった" in err and no_trace(err), "picocmd: トークンのファイルがない (%r %r)" % (rc, err))
    # トークンは表示しない
    check(TOKEN.decode() not in out + err, "picocmd: トークンを表示しない")


def blocks(path):
    with open(path, "rb") as f:
        d = f.read()
    out = []
    for i in range(0, len(d), 512):
        m0, m1, flags, addr, size, no, total, family = struct.unpack("<8I", d[i:i + 32])
        end, = struct.unpack("<I", d[i + 508:i + 512])
        out.append((m0, m1, flags, addr, size, no, total, family, d[i + 32:i + 32 + size], end))
    return len(d), out


def test_mkuf2(tmp):
    a = os.path.join(tmp, "a.bin")
    with open(a, "wb") as f:
        f.write(bytes(range(256)) * 3 + b"xyz")  # 771 バイト → 4KB まで 0xff で埋めて 16 ブロック
    uf2 = os.path.join(tmp, "a.uf2")
    rc, out, err = run("mkuf2.py", uf2, "0x10000000:" + a, "0x101f0000:erase:8192")
    check(rc == 0, "mkuf2: 作れる (%r %r)" % (rc, err))
    size, bl = blocks(uf2)
    check(size == 512 * 48 and len(bl) == 48, "mkuf2: ブロックの数")
    for i, b in enumerate(bl):
        check(b[0] == 0x0A324655 and b[1] == 0x9E5D5157 and b[9] == 0x0AB16F30, "mkuf2: ブロック %d の目印" % i)
        check(b[2] == 0x2000 and b[4] == 256 and b[5] == i and b[6] == 48 and b[7] == 0xE48BFF56, "mkuf2: ブロック %d の頭" % i)
    check([b[3] for b in bl[:16]] == [0x10000000 + 256 * i for i in range(16)], "mkuf2: bin の番地")
    check(bl[0][8] == bytes(range(256)) and bl[3][8] == b"xyz" + b"\xff" * 253, "mkuf2: bin の中身")
    check(all(b[8] == b"\xff" * 256 for b in bl[4:16]), "mkuf2: bin の 4KB の残りは 0xff")
    # 消す範囲: 0x101f0000 から 8192 バイトが、すき間なく 0xff
    check([b[3] for b in bl[16:]] == [0x101F0000 + 256 * i for i in range(32)], "mkuf2: erase の番地")
    check(all(b[8] == b"\xff" * 256 for b in bl[16:]), "mkuf2: erase の中身は 0xff")
    # 4KB の境目に合っていない範囲、重なる範囲は断る 
    for bad in ("0x101f0100:erase:4096", "0x101f0000:erase:256", "0x101f0000:erase:0", "0x10000000:erase:4096",
                "0x10000100:" + a):
        rc, out, err = run("mkuf2.py", uf2, "0x10000000:" + a, bad)
        check(rc != 0 and "Traceback" not in err, "mkuf2: %s を断る (%r %r)" % (bad, rc, err))
    big = os.path.join(tmp, "big.bin")
    with open(big, "wb") as f:
        f.write(b"\x33" * 4097)  # 4KB と 1 バイト → 0x10000000 から 8KB を使う
    rc, out, err = run("mkuf2.py", uf2, "0x10000000:" + big, "0x10001000:" + a)
    check(rc != 0 and "Traceback" not in err, "mkuf2: 埋めた 4KB の残りに重なる bin を断る (%r %r)" % (rc, err))

    # README の RP2040 の最初の UF2 と同じ形 (29 ブロックのブートローダ + 0x10010000 のファームウェア + 記録の消去)。
    # どの 4KB も、ファイルの中で 16 の倍数の番目から 16 ブロック続けて並ぶ (ドライブに写したとき、4KB の先頭が消えないように)
    boot = os.path.join(tmp, "boot.bin")
    fw = os.path.join(tmp, "fw.bin")
    with open(boot, "wb") as f:
        f.write(b"\x11" * (29 * 256 - 92))
    with open(fw, "wb") as f:
        f.write(b"\x22" * (1401 * 256 - 100))
    rc, out, err = run("mkuf2.py", uf2, "0x10000000:" + boot, "0x10010000:" + fw, "0x101f0000:erase:8192")
    check(rc == 0, "mkuf2: 最初の UF2 の形を作れる (%r %r)" % (rc, err))
    size, bl = blocks(uf2)
    addrs = [b[3] for b in bl]
    sectors_ok = len(bl) % 16 == 0 and all(
        addrs[i] % 4096 == 0 and addrs[i:i + 16] == [addrs[i] + 256 * k for k in range(16)] for i in range(0, len(bl), 16))
    check(sectors_ok, "mkuf2: どの 4KB も 16 の倍数の番目から続けて並ぶ")
    check(len(bl) == 16 * (2 + 88 + 2), "mkuf2: 最初の UF2 の形のブロックの数 (%d)" % len(bl))
    check(bl[28][8][-92:] == b"\xff" * 92 and bl[29][8] == b"\xff" * 256, "mkuf2: ブートローダの後ろは 0xff で埋める")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        test_picocmd(tmp)
        test_mkuf2(tmp)
    if fails:
        sys.exit("%d failed" % fails)
    print("ok")


if __name__ == "__main__":
    main()
