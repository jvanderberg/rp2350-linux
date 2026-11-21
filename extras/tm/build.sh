#!/bin/sh
# Build tm. Compiles one file at a time so it fits on a small board.
# Always rebuilds everything: on the board's FAT card every file has the
# same timestamp, so "is it newer" checks can't be trusted.
#   ./build.sh            with tcc (the board)
#   CC=cc ./build.sh      with any other C compiler
set -e
cd "$(dirname "$0")"
CC=${CC:-tcc}
CFLAGS=${CFLAGS:--O2}
SRCS="main client server copy vt render sb lz keys proto util"
OBJS=
for f in $SRCS; do
    echo "  CC $f.c"
    $CC $CFLAGS -D_GNU_SOURCE -c "$f.c" -o "$f.o"
    OBJS="$OBJS $f.o"
done
echo "  LD tm"
$CC -o tm $OBJS
