#!/bin/sh
# Build nnn on the device with tcc against the narrow libncurses.
# Curses headers come from ../noice/include (the device ships none).
set -e
cd "$(dirname "$0")"
tcc -O2 -DNORL -DNOX11 -DDIM_FILTERED \
    -Icompat -I../noice/include \
    -o nnn nnn.c -lncurses
ls -l nnn
