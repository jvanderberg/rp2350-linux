#!/usr/bin/env bash
# board-test.sh [selfhost]: copy these tests to the board's /tmp/tcc-test over
# the console (base64 in chunks, through board-cmd.py) and run them there
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
	mkdir "$W/tcc-test/tcc-src"
	docker run --rm -v rvemu-guest-work:/work:ro rvemu-guest bash -c '
		cd /work/br-disk/build/tcc-*/ && tar cf - tcc.c libtcc.c tccpp.c tccgen.c \
		tccdbg.c tccelf.c tccasm.c tccrun.c tcctools.c riscv32-gen.c riscv64-link.c \
		riscv64-asm.c riscv64-tok.h tcc.h tcctok.h libtcc.h elf.h stab.h stab.def \
		dwarf.h tcclib.h -C build-target config.h' | tar xf - -C "$W/tcc-test/tcc-src"
fi
tar czf "$W/t.tgz" -C "$W" tcc-test
{ base64 < "$W/t.tgz" | tr -d '\n' | fold -w 2048; echo; } > "$W/t.b64"
echo "board-test: sending $(wc -c < "$W/t.tgz") bytes in $(wc -l < "$W/t.b64") chunks"
start=$(date +%s)
"$CMD" -- 'rm -rf /tmp/tcc-test /tmp/t.b64'
while read -r line; do
	"$CMD" -- "echo $line >> /tmp/t.b64" > /dev/null
done < "$W/t.b64"
"$CMD" -- 'cd /tmp && base64 -d t.b64 | tar xzf - && rm t.b64 && ls tcc-test | wc -l'
echo "board-test: copied in $(($(date +%s) - start)) s"
start=$(date +%s)
"$CMD" --timeout 1800 -- "sh /tmp/tcc-test/run.sh ${1:-}"
echo "board-test: ran in $(($(date +%s) - start)) s (wall, from the Mac)"
