#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Build xwolf on the board with tcc. Each file is compiled on its own to keep
# tcc's memory low. Everything is rebuilt every time: without a real-time
# clock, the board's file times can't say what changed.
cd "$(dirname "$0")" || exit 1
mkdir -p obj
objs=""
for src in *.c; do
    obj="obj/${src%.c}.o"
    echo "cc $src"
    cc -DXWOLF -I. -I../demos -c "$src" -o "$obj" || exit 1
    objs="$objs $obj"
done
cc -o xwolf.new $objs -lm && mv xwolf.new xwolf || exit 1
rm -rf obj
echo "built xwolf"
