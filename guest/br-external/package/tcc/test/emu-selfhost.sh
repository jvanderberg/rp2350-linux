#!/usr/bin/env bash
# emu-selfhost.sh: the tcc tests plus the self-host stress case (run.sh
# selfhost) on the Mac, in the disk guest under QEMU with the translator and
# 11 MB of guest RAM (about the board's MemTotal). The image is
# guest/out-disk/rootfs.squashfs with these tests and tcc's source added at
# /t, and an init script that runs them. While they run, a monitor prints
# the guest's free memory every 10 s and how long each 10 s step took (a
# long step means the guest stalled). Needs guest/run-build.sh disk first.
set -euo pipefail
cd "$(dirname "$0")"
TEST=$PWD
G=$(cd ../../../.. && pwd)          # guest/
RS=$(cd "$G/../rvemu-rs" && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/t/tcc-test"
cp ./*.c ./*.h ./*.expect run.sh "$W/t/tcc-test/"
cat > "$W/t/go.sh" <<'EOF'
cp -a /t/tcc-test /tmp/ && cd /tmp/tcc-test
sh run.sh selfhost > log 2>&1 < /dev/null &
p=$!
prev=$(cut -d' ' -f1 /proc/uptime)
while kill -0 $p 2>/dev/null; do
	sleep 10
	now=$(cut -d' ' -f1 /proc/uptime)
	echo "monitor: up $now step $(echo "$now $prev" | awk '{printf "%.1f", $1-$2}') s, $(free | awk '/Mem:/{print "free", $4, "shared", $5, "available", $7}') kB"
	prev=$now
done
cat log
dmesg | grep -i -A3 "out of memory" | tail -12
EOF
cat > "$W/S99ztest" <<'EOF'
#!/bin/sh
case "$1" in start) sh /t/go.sh 2>&1; echo TESTS-""DONE ;; esac
EOF
docker run --rm -v rvemu-guest-work:/work:ro -v "$G/out-disk:/o:ro" -v "$W:/w" rvemu-guest bash -c '
set -e
H=/work/br-disk/host; export LD_LIBRARY_PATH=$H/lib
cd /work/br-disk/build/tcc-*/ && tar czf /w/t/tcc-test/tcc-src.tgz tcc.c libtcc.c \
	tccpp.c tccgen.c tccdbg.c tccelf.c tccasm.c tccrun.c tcctools.c riscv32-gen.c \
	riscv64-link.c riscv64-asm.c riscv64-tok.h tcc.h tcctok.h libtcc.h elf.h stab.h \
	stab.def dwarf.h tcclib.h -C build-target config.h
R=/tmp/r; $H/bin/unsquashfs -q -d $R /o/rootfs.squashfs >/dev/null
cp -a /w/t $R/t
install -m 755 /w/S99ztest $R/etc/init.d/S99ztest
$H/bin/mksquashfs $R /w/test.sqfs -comp lz4 -Xhc -b 16384 -noappend -all-root -quiet >/dev/null'
cd "$RS"
scripts/virt.sh jit -m 11 -k ../guest/out-disk/Image -i none -d "$W/test.sqfs" -u TESTS-DONE |
	tr -d '\r' | sed -n '/^monitor: /,/^TESTS-DONE/p'
echo "emu-selfhost: tests in $TEST"
