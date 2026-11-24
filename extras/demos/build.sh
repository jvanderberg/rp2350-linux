#!/bin/sh
# Build the demos. Each source file is compiled on its own and then linked,
# which keeps tcc's peak memory low enough for this board.
# Usage: ./build.sh [name ...]   (default: all demos)
cd "$(dirname "$0")" || exit 1

build() {
    name=$1; shift
    objs=""
    for src in "$@"; do
        obj="/tmp/${src%.c}.o"
        cc -Wall -c "$src" -o "$obj" || { rm -f $objs; return 1; }
        objs="$objs $obj"
    done
    # Link to a temporary name, then rename: safe while the old one runs.
    cc -o "$name.new" $objs && mv "$name.new" "$name"
    rc=$?
    rm -f $objs
    [ $rc -eq 0 ] && echo "built $name"
    return $rc
}

targets=${*:-"stackfall rain pulse asteroids bogomips life maze"}
status=0
for t in $targets; do
    case $t in
        stackfall) build stackfall stackfall.c || status=1 ;;
        rain)      build rain rain.c || status=1 ;;
        pulse)     build pulse pulse.c pulse_draw.c pulse_data.c || status=1 ;;
        asteroids) build asteroids asteroids.c || status=1 ;;
        bogomips)  build bogomips bogomips.c || status=1 ;;
        life)      build life life.c || status=1 ;;
        maze)      build maze maze.c || status=1 ;;
        *)         echo "unknown demo: $t"; status=1 ;;
    esac
done
exit $status
