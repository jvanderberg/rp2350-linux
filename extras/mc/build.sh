#!/bin/sh
# Build mc, and xmc (mc in an X11 window; it needs ../demos/x11.h and the
# terminal from ../tm for its terminal tabs). Each
# source file is compiled on its own and then linked, which keeps tcc's
# peak memory low enough for the RP2350 board.
#
#   ./build.sh            build mc with tcc (the board's compiler)
#   ./build.sh xmc        build xmc
#   ./build.sh mc xmc     build both
#   CC=cc ./build.sh      build with another compiler (e.g. on a PC)
cd "$(dirname "$0")" || exit 1

CC=${CC:-tcc}
CFLAGS=${CFLAGS:--O2 -Wall}
MC_SRCS="main.c screen.c keys.c panel.c panel_draw.c dialog.c menu.c fileops.c run.c util.c
         viewer.c usermenu.c"
XMC_SRCS="xmc.c panel.c fileops.c usermenu.c util.c ../tm/vt.c ../tm/sb.c ../tm/lz.c ../tm/util.c"

# build NAME SOURCES...
build() {
    name=$1
    shift
    tmp=${TMPDIR:-/tmp}/mc-build.$$
    mkdir -p "$tmp" || return 1
    objs=""
    for src in "$@"; do
        base=${src##*/}
        flags=
        case $src in
            # tm's util.c has an xmalloc too: give its own another name
            ../tm/*) obj="$tmp/tm_${base%.c}.o"; flags="-Dxmalloc=tm_xmalloc" ;;
            *)       obj="$tmp/${base%.c}.o" ;;
        esac
        $CC $CFLAGS $flags -D_GNU_SOURCE -I../demos -c "$src" -o "$obj" || { rm -rf "$tmp"; return 1; }
        objs="$objs $obj"
    done
    # Link to a temporary name, then rename: safe while the old one runs.
    $CC -o "$name.new" $objs && mv "$name.new" "$name"
    rc=$?
    rm -rf "$tmp"
    [ $rc -eq 0 ] && echo "built $name"
    return $rc
}

status=0
for target in ${*:-mc}; do
    case $target in
        mc)  build mc $MC_SRCS || status=1 ;;
        xmc) build xmc $XMC_SRCS || status=1 ;;
        *)   echo "unknown target: $target (mc or xmc)" >&2; status=1 ;;
    esac
done
exit $status
