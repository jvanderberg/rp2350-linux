#!/usr/bin/env python3
"""Run the PIO tests (run.sh) in the disk guest on the Mac emulator's
simulated board (rvemu -G, software PIO in rvemu/src/piosim.rs).

    guest/tests/pio/emu-test.py        (needs rvemu-rs/target/release/rvemu
                                         and guest/out-disk: run-build.sh disk)

Boots the guest, copies this directory's files in, runs run.sh, prints its
output; exit 0 when it ends "pio-tests: PASS". The board runs the same
files through rp2350/tools/pin-test.py --only pio-self.
"""
import base64
import os
import select
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "../../.."))
FILES = ["run.sh", "pio-selftest.c", "rp2_selftest.py"]
PROMPT = b"EMU# "


def main():
    rvemu = os.path.join(ROOT, "rvemu-rs/target/release/rvemu")
    out = os.path.join(ROOT, "guest/out-disk")
    for f in (rvemu, os.path.join(out, "Image"), os.path.join(out, "rootfs.squashfs")):
        if not os.path.exists(f):
            sys.exit(f"emu-test: no {f} (cargo build --release -p rvemu; guest/run-build.sh disk)")
    p = subprocess.Popen([rvemu, "-k", f"{out}/Image", "-d", f"{out}/rootfs.squashfs", "-G", "-m", "16"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    buf = b""

    def until(pat, timeout):
        nonlocal buf
        end = time.time() + timeout
        while pat not in buf:
            if time.time() > end:
                return False
            if select.select([p.stdout], [], [], 0.5)[0]:
                d = os.read(p.stdout.fileno(), 65536)
                if not d:
                    return False
                buf += d
        return True

    def run(cmd, timeout):
        nonlocal buf
        buf = b""
        p.stdin.write(cmd.encode() + b"\n")
        p.stdin.flush()
        ok = until(PROMPT, timeout)
        return ok, buf.decode(errors="replace").replace(PROMPT.decode(), "")

    try:
        if not until(b"login:", 300):
            sys.exit("emu-test: the guest never got to its login prompt\n" + buf.decode(errors="replace")[-2000:])
        p.stdin.write(b"root\n")
        p.stdin.flush()
        until(b"# ", 30)
        # a prompt the commands' own echo can't contain
        run("stty -echo; PS1='EM''U# '", 30)
        run("mkdir -p /tmp/pio-tests && cd /tmp/pio-tests", 30)
        for name in FILES:
            data = base64.b64encode(open(os.path.join(HERE, name), "rb").read()).decode()
            run(f": > {name}.b64", 30)
            for i in range(0, len(data), 512):
                run(f"echo {data[i:i + 512]} >> {name}.b64", 30)
            ok, o = run(f"base64 -d {name}.b64 > {name} && rm {name}.b64 && echo copied", 30)
            if "copied" not in o:
                sys.exit(f"emu-test: copying {name} failed: {o}")
        ok, o = run("sh run.sh 2>&1", 1800)
        print(o.strip())
        if not ok:
            sys.exit("emu-test: run.sh timed out")
        return 0 if "pio-tests: PASS" in o else 1
    finally:
        p.kill()


if __name__ == "__main__":
    sys.exit(main())
