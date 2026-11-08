#!/usr/bin/env python3
"""Print '<guest insns> <FNV-1a of console bytes>' for a Mac rvemu run.

usage: fnv.py CONSOLE_OUT RVEMU_STDERR
(the same hash rvemu-pico's RTT console keeps)
"""
import re
import sys


def main():
    h = 0x811C9DC5
    with open(sys.argv[1], "rb") as f:
        for b in f.read():
            h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    with open(sys.argv[2]) as f:
        n = re.search(r"guest instructions (\d+)", f.read()).group(1)
    print(n, f"{h:08x}")


if __name__ == "__main__":
    main()
