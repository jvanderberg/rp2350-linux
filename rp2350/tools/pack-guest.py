#!/usr/bin/env python3
"""Pack a Linux guest for the RP2350 firmwares (rvemu-rs/rvemu-pico/src/guest_blob.rs).

usage: pack-guest.py IMAGE INITRD DTB RAM_MB TEXT_LEN BLOB_OUT TEXT_OUT

The Image's first TEXT_LEN bytes (its text, page aligned) go to TEXT_OUT,
which belongs at 0x11000000 - TEXT_LEN in flash: the firmware maps it as
read-only guest RAM straight from flash. The rest goes in the blob.

Blob header (10 little-endian u32): magic "RVLY", guest RAM bytes
(TEXT_LEN + RAM_MB MiB), then offset and length of the kernel remainder,
the initramfs and the DTB (each 4 KiB aligned), TEXT_LEN, FNV-1a of the text.
"""
import struct
import sys


def fnv1a(b):
    h = 0x811C9DC5
    for x in b:
        h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
    return h


def read(path):
    with open(path, "rb") as f:
        return f.read()


def write(path, data):
    with open(path, "wb") as f:
        f.write(data)


def main():
    k, i, d = (read(p) for p in sys.argv[1:4])
    mb, tlen = int(sys.argv[4]), int(sys.argv[5], 0)
    blob_out, text_out = sys.argv[6], sys.argv[7]
    assert tlen % 4096 == 0 and tlen <= len(k), "TEXT_LEN must be a page multiple inside the Image"
    text, rest = k[:tlen], k[tlen:]

    def al(x):
        return (x + 4095) & ~4095

    ko = 4096
    io = al(ko + len(rest))
    do = al(io + len(i))
    blob = bytearray(do + len(d))
    struct.pack_into("<10I", blob, 0, 0x594C5652, (mb << 20) + tlen, ko, len(rest), io, len(i), do, len(d),
                     tlen, fnv1a(text))
    blob[ko:ko + len(rest)] = rest
    blob[io:io + len(i)] = i
    blob[do:do + len(d)] = d
    write(blob_out, blob)
    write(text_out, text)


if __name__ == "__main__":
    main()
