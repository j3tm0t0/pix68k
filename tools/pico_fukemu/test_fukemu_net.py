#!/usr/bin/env python3
"""fukemu_net.py を nwsim (netwrite.c を TCP で受けるもの) に向けて走らせ、書かれたフラッシュを mkimg.py の中身と比べる"""
import os
import subprocess
import sys
import tempfile

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import mkimg  # noqa: E402

nwsim = sys.argv[1]
with tempfile.TemporaryDirectory() as d:
    img = bytes((i * 13 + (i >> 10)) & 0xFF for i in range(mkimg.IMAGE_BYTES))
    with open(os.path.join(d, "a.xdf"), "wb") as f:
        f.write(img)
    with open(os.path.join(d, "token"), "w") as f:
        f.write("testtoken99\n")
    out = os.path.join(d, "flash.bin")
    port = "23%03d" % (os.getpid() % 1000)
    sim = subprocess.Popen([nwsim, port, "testtoken99", out], stdout=subprocess.PIPE, text=True)
    assert sim.stdout.readline().strip() == "ready"
    r = subprocess.run([sys.executable, os.path.join(here, "fukemu_net.py"), "127.0.0.1", "--port", port,
                        "--token-file", os.path.join(d, "token"), "--fd1", os.path.join(d, "a.xdf"), "--wp1",
                        "--stretch", "1:1:0:8:2.3"], capture_output=True, text=True, timeout=120)
    sim.wait(timeout=10)
    if r.returncode != 0:
        sys.exit("test_fukemu_net: FAIL\n" + r.stdout + r.stderr)
    flash = open(out, "rb").read()

    class A:
        pass
    a = A()
    for k in ("fd0", "erase0", "erase1", "wp0", "no_stretch", "one_sector", "config"):
        setattr(a, k, None)
    a.fd1, a.wp1, a.delivery, a.rule, a.stretch = os.path.join(d, "a.xdf"), True, None, None, [(1, 1, 0, 8, 23000)]
    for off, data in mkimg.regions_for(a):
        if flash[off:off + len(data)] != data:
            sys.exit("test_fukemu_net: FAIL (0x%x の中身が mkimg.py と違う)" % off)
    assert flash[mkimg.slot_off(0):mkimg.slot_off(0) + 8] == b"\xff" * 8
print("test_fukemu_net: ok")
