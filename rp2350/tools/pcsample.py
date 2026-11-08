#!/usr/bin/env python3
"""Statistical PC profiler for a Hazard3 program, over OpenOCD's Tcl port.

Halts rv0, reads pc, resumes, N times; keeps the RTT up-channel drained (the
programs use BlockIfFull) and prints it; then maps samples to symbols with
llvm-nm. Start OpenOCD first (see psramtest-rs/run-openocd.sh for the
arguments; force `riscv set_mem_access sysbus`).

usage: pcsample.py ELF [--samples N] [--interval S] [--start-after REGEX]
"""
import argparse
import bisect
import collections
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rtt import Ocd  # noqa: E402

NM = os.path.expanduser(
    "~/.rustup/toolchains/stable-aarch64-apple-darwin/lib/rustlib/aarch64-apple-darwin/bin/llvm-nm"
)


def symbols(elf):
    out = subprocess.run([NM, "-C", "-n", "--defined-only", elf], capture_output=True, text=True, check=True).stdout
    addrs, names, rtt = [], [], None
    for ln in out.splitlines():
        p = ln.split(" ", 2)
        if len(p) == 3 and p[1] in "tTwW":
            addrs.append(int(p[0], 16))
            names.append(p[2])
        if len(p) == 3 and p[2] == "_SEGGER_RTT":
            rtt = int(p[0], 16)
    return addrs, names, rtt


class Rtt:
    def __init__(self, ocd, cb):
        self.ocd, self.up0 = ocd, cb + 24
        _, self.buf, self.size, _, _, _ = ocd.words(self.up0, 6)
        self.text = ""

    def drain(self):
        wr, rd = self.ocd.words(self.up0 + 12, 2)
        if wr == rd:
            return
        if wr > rd:
            data = self.ocd.bytes(self.buf + rd, wr - rd)
        else:
            data = self.ocd.bytes(self.buf + rd, self.size - rd) + self.ocd.bytes(self.buf, wr)
        self.ocd.write_word(self.up0 + 16, wr)
        s = data.decode(errors="replace")
        self.text += s
        sys.stdout.write(s)
        sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--samples", type=int, default=400)
    ap.add_argument("--interval", type=float, default=0.01)
    ap.add_argument("--start-after", default=None, help="begin sampling once RTT output matches")
    ap.add_argument("--port", type=int, default=6666)
    a = ap.parse_args()

    addrs, names, cb = symbols(a.elf)
    ocd = Ocd(a.port)
    ocd.cmd("targets rp2350.rv0")
    while not ocd.bytes(cb, 16).startswith(b"SEGGER RTT"):
        time.sleep(0.05)
    rtt = Rtt(ocd, cb)
    if a.start_after:
        pat = re.compile(a.start_after)
        while not pat.search(rtt.text):
            rtt.drain()
            time.sleep(0.05)
    hits = collections.Counter()
    for _ in range(a.samples):
        ocd.cmd("halt")
        m = re.search(r"0x[0-9a-f]+", ocd.cmd("reg pc"))
        ocd.cmd("resume")
        if m:
            pc = int(m.group(0), 16)
            i = bisect.bisect_right(addrs, pc) - 1
            hits[(names[i] if i >= 0 else "?", "SRAM" if pc >= 0x2000_0000 else "flash")] += 1
        rtt.drain()
        time.sleep(a.interval)
    total = sum(hits.values())
    print(f"\n---- {total} samples ----")
    for (name, where), c in hits.most_common(25):
        print(f"{100 * c / total:5.1f}%  {where:5}  {name[:110]}")


if __name__ == "__main__":
    main()
