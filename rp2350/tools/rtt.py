#!/usr/bin/env python3
"""Minimal RTT up-channel reader over OpenOCD's Tcl RPC port.

Raspberry Pi's OpenOCD build only exposes `rtt server`, not `rtt setup` /
`rtt start`, so this polls the SEGGER RTT control block directly: read the
up-channel's WrOff/RdOff, copy out new bytes, write RdOff back. Works while the
Hazard3 core runs (OpenOCD uses system-bus access).

usage: rtt.py CONTROL_BLOCK_ADDR [--until REGEX] [--timeout S] [--port 6666]
"""
import argparse
import re
import socket
import sys
import time

TERM = b"\x1a"


class Ocd:
    def __init__(self, port):
        self.s = socket.create_connection(("localhost", port))

    def cmd(self, c):
        self.s.sendall(c.encode() + TERM)
        buf = b""
        while not buf.endswith(TERM):
            chunk = self.s.recv(65536)
            if not chunk:
                raise ConnectionError("openocd closed the connection")
            buf += chunk
        return buf[:-1].decode()

    def words(self, addr, n):
        out = self.cmd(f"read_memory {addr:#x} 32 {n}").split()
        return [int(x, 0) for x in out]

    def bytes(self, addr, n):
        # Byte-wide read_memory aborts this OpenOCD build on RISC-V, so read
        # aligned words and slice.
        start = addr & ~3
        count = (addr + n - start + 3) // 4
        raw = b"".join(w.to_bytes(4, "little") for w in self.words(start, count))
        return raw[addr - start : addr - start + n]

    def write_word(self, addr, v):
        self.cmd(f"write_memory {addr:#x} 32 {{{v:#x}}}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cb", type=lambda x: int(x, 0))
    ap.add_argument("--until", default=None)
    ap.add_argument("--timeout", type=float, default=120)
    ap.add_argument("--port", type=int, default=6666)
    a = ap.parse_args()

    ocd = Ocd(a.port)
    deadline = time.time() + a.timeout
    while True:
        ident = ocd.bytes(a.cb, 16)
        if ident.startswith(b"SEGGER RTT"):
            break
        if time.time() > deadline:
            sys.exit(f"no RTT control block at {a.cb:#x}")
        time.sleep(0.1)

    # Control block: id[16], max_up, max_down, then up[0] =
    # {name, buffer, size, wroff, rdoff, flags}.
    up0 = a.cb + 24
    _, buf, size, _, _, _ = ocd.words(up0, 6)
    pat = re.compile(a.until) if a.until else None
    text = ""
    while time.time() < deadline:
        wr, rd = ocd.words(up0 + 12, 2)
        if wr == rd:
            time.sleep(0.05)
            continue
        if wr > rd:
            data = ocd.bytes(buf + rd, wr - rd)
        else:
            data = ocd.bytes(buf + rd, size - rd) + ocd.bytes(buf, wr)
        ocd.write_word(up0 + 16, wr)
        s = data.decode(errors="replace")
        sys.stdout.write(s)
        sys.stdout.flush()
        text += s
        if pat and pat.search(text):
            # Drain whatever follows the match for a moment, then stop.
            pat = None
            deadline = time.time() + 0.5
    if pat:
        sys.exit("timeout")


if __name__ == "__main__":
    main()
