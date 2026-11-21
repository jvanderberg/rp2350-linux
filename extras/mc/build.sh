#!/bin/sh
# Build mc. Each source file is compiled on its own and then linked, which
# keeps tcc's peak memory low enough for the RP2350 board.
#
#   ./build.sh            build with tcc (the board's compiler)
#   CC=cc ./build.sh      build with another compiler (e.g. on a PC)
cd "$(dirname "$0")" || exit 1

CC=${CC:-tcc}
CFLAGS=${CFLAGS:--O2 -Wall}
SRCS="main.c screen.c keys.c panel.c dialog.c menu.c fileops.c run.c util.c
      viewer.c usermenu.c"
TMP=${TMPDIR:-/tmp}/mc-build.$$

mkdir -p "$TMP" || exit 1
objs=""
for src in $SRCS; do
    obj="$TMP/${src%.c}.o"
    $CC $CFLAGS -D_GNU_SOURCE -c "$src" -o "$obj" || { rm -rf "$TMP"; exit 1; }
    objs="$objs $obj"
done
# Link to a temporary name, then rename: safe while the old one runs.
$CC -o mc.new $objs && mv mc.new mc
rc=$?
rm -rf "$TMP"
[ $rc -eq 0 ] && echo "built mc"
exit $rc
