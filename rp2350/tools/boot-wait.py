#!/usr/bin/env python3
"""Wait for `login:` on rvemu-pico-shell's console; print seconds since START.

usage: boot-wait.py START_EPOCH [--port PORT] [--timeout S]

START_EPOCH is when the firmware was started (pico-shell.sh writes it to
$RP2350_LOCK_HOME/last-launch). Prints the boot time in seconds on stdout,
or exits 1 on timeout. Plain termios, never 1200 baud.
"""
import argparse
import glob
import os
import select
import sys
import termios
import time
import tty


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("start", type=float)
    ap.add_argument("--port", default=None)
    ap.add_argument("--timeout", type=float, default=180.0)
    a = ap.parse_args()
    end = time.time() + a.timeout
    port = a.port
    while not port:
        ports = sorted(glob.glob("/dev/cu.usbmodemrvemu*"))
        if ports:
            port = ports[0]
        elif time.time() > end:
            sys.exit("boot-wait: no console port")
        else:
            time.sleep(0.05)
    fd = None
    while fd is None:
        try:
            fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
        except OSError:
            if time.time() > end:
                raise
            time.sleep(0.05)
    tty.setraw(fd, termios.TCSANOW)
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    buf = b""
    log = bytearray()
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if not r:
            continue
        data = os.read(fd, 4096)
        if not data:
            break
        log += data
        buf = (buf + data)[-64:]
        if b"login:" in buf:
            t = time.time() - a.start
            os.close(fd)
            print(f"{t:.2f}")
            return 0
        if b"Kernel panic" in log[-2048:]:
            break
    sys.stderr.write(log[-1500:].decode(errors="replace"))
    sys.exit("boot-wait: no login: (timeout or panic)")


if __name__ == "__main__":
    sys.exit(main())
