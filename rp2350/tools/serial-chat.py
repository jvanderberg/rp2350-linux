#!/usr/bin/env python3
"""Script a serial console: wait for a pattern, send a line, repeat.

usage: serial-chat.py PORT [--timeout S] EXPECT [SEND EXPECT]...
Prints everything received, with the elapsed time at each match. No pyserial:
plain termios, raw mode.
"""
import argparse
import os
import select
import sys
import termios
import time
import tty


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("steps", nargs="+")
    a = ap.parse_args()
    fd = os.open(a.port, os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd, termios.TCSANOW)  # the default TCSAFLUSH drops early output
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    t0 = time.time()
    buf = b""
    steps = a.steps
    i = 0
    while i < len(steps):
        want = steps[i].encode()
        deadline = time.time() + a.timeout
        while want not in buf:
            left = deadline - time.time()
            if left <= 0:
                sys.stdout.write(f"\n[serial-chat] timeout waiting for {steps[i]!r}\n")
                return 1
            r, _, _ = select.select([fd], [], [], min(left, 0.5))
            if r:
                data = os.read(fd, 4096)
                sys.stdout.write(data.decode(errors="replace"))
                sys.stdout.flush()
                buf += data
        sys.stdout.write(f"\n[serial-chat] {time.time() - t0:.1f} s: got {steps[i]!r}\n")
        buf = buf[buf.index(want) + len(want):]
        if i + 1 < len(steps):
            os.write(fd, steps[i + 1].encode() + b"\r")
        i += 2
    os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
