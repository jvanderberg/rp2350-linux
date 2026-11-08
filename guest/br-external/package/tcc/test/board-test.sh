#!/usr/bin/env bash
# board-test.sh [selfhost]: copy these tests to the board's /tmp/tcc-test over
# the console (push.py) and run them there
# with run.sh. "selfhost" also copies tcc's source from the disk guest's
# Buildroot build (Docker volume rvemu-guest-work) and has tcc build itself.
# Needs the board lock: rp2350/tools/board-lock run NAME -- ./board-test.sh
set -euo pipefail
cd "$(dirname "$0")"
CMD=../../../../../rp2350/tools/board-cmd.py
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir "$W/tcc-test"
cp ./*.c ./*.h ./*.expect run.sh "$W/tcc-test/"
if [[ ${1:-} == selfhost ]]; then
	docker run --rm -v rvemu-guest-work:/work:ro rvemu-guest bash -c '
		cd /work/br-disk/build/tcc-*/ && tar czf - tcc.c libtcc.c tccpp.c tccgen.c \
		tccdbg.c tccelf.c tccasm.c tccrun.c tcctools.c riscv32-gen.c riscv64-link.c \
		riscv64-asm.c riscv64-tok.h tcc.h tcctok.h libtcc.h elf.h stab.h stab.def \
		dwarf.h tcclib.h -C build-target config.h' > "$W/tcc-test/tcc-src.tgz"
fi
tar czf "$W/t.tgz" -C "$W" tcc-test
start=$(date +%s)
"$CMD" -- 'rm -rf /tmp/tcc-test /tmp/t.tgz'
./push.py "$W/t.tgz" /tmp/t.tgz
"$CMD" -- 'cd /tmp && tar xzf t.tgz && rm t.tgz && ls tcc-test | wc -l'
echo "board-test: copied in $(($(date +%s) - start)) s"
start=$(date +%s)
"$CMD" --timeout 1800 -- "sh /tmp/tcc-test/run.sh ${1:-}"
echo "board-test: ran in $(($(date +%s) - start)) s (wall, from the Mac)"
