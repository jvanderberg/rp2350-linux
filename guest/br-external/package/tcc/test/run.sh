#!/bin/sh
# run.sh [selfhost]: run in the guest, from a copy of this directory.
# Compiles and runs each test with the guest's cc and compares the output
# with NAME.expect (made by a native compiler). With "selfhost", tcc's
# source in ./tcc-src is built by cc, and then by that tcc, and the two
# binaries must be identical. Prints wall times and peak memory (VmHWM).
cd "$(dirname "$0")" || exit 1
pass=0 fail=0

# peak NAME CMD...: run CMD, then print its wall time and VmHWM
peak() {
	name=$1; shift
	t0=$(cut -d' ' -f1 /proc/uptime)
	"$@" &
	p=$! m=0
	# until it exits (a zombie's status has no VmHWM line)
	while v=$(sed -n 's/^VmHWM:[^0-9]*\([0-9]*\).*/\1/p' "/proc/$p/status" 2>/dev/null) &&
		[ -n "$v" ]; do
		m=$v
		usleep 100000
	done
	wait "$p"
	rc=$?
	t1=$(cut -d' ' -f1 /proc/uptime)
	echo "time $name $(echo "$t1 $t0" | awk '{printf "%.2f", $1 - $2}') s, peak $m kB, rc $rc"
	return $rc
}

check() {	# NAME: NAME.out against NAME.expect
	if cmp -s "$1.out" "$1.expect"; then
		pass=$((pass + 1)); echo "PASS $1"
	else
		fail=$((fail + 1)); echo "FAIL $1"; diff "$1.expect" "$1.out" | head -20
	fi
}

for t in hello ll fp misc arith gpio; do
	peak "cc-$t" cc -o "$t" "$t.c" -lm && "./$t" > "$t.out" 2>&1
	check "$t"
done

# multi-file build with a static library made by tcc -ar
peak cc-c-vec-stats cc -c vec.c stats.c &&
	peak tcc-ar cc -ar rcs libvec.a vec.o stats.o &&
	peak cc-link-lib cc -o lib main.c -L. -lvec && ./lib > lib.out 2>&1
check lib
echo "libvec.a: $(cc -ar t libvec.a | tr '\n' ' ')"

# tcc -run and the classic one-liner
cc -run hello.c > run.out 2>&1; cp hello.expect run.expect; check run
rm -f a.out; cc hello.c && ./a.out > aout.out 2>&1; cp hello.expect aout.expect; check aout

if [ "$1" = selfhost ]; then
	# tcc's source comes as tcc-src.tgz; each file is unpacked only while it
	# is compiled, and the first build's objects go once tcc2 is linked:
	# /tmp is RAM, and the compiler needs 2-2.6 MB of its own
	rm -f hello ll fp misc arith gpio lib a.out ./*.o libvec.a
	cp arith.expect arith3.expect
	mkdir -p tcc-src && cd tcc-src || exit 1
	# src FILE...: unpack FILEs from tcc-src.tgz (busybox tar has no -z)
	src() { gunzip -c ../tcc-src.tgz | tar xf - "$@"; }
	# build CC OUT: tcc one file at a time, as its Makefile does (as one
	# unit, tcc needs about 3 MB more than the guest has)
	build() {
		b0=$(cut -d' ' -f1 /proc/uptime)
		# the headers, and the target files tcc.h includes for their defines
		src config.h tcc.h tcctok.h libtcc.h elf.h stab.h stab.def dwarf.h \
			tcclib.h riscv64-tok.h riscv32-gen.c riscv64-link.c riscv64-asm.c
		for u in tcc libtcc tccpp tccgen tccdbg tccelf tccasm tccrun \
			riscv32-gen riscv64-link riscv64-asm; do
			case $u in
			riscv*) ;;
			tcc) src tcc.c tcctools.c || return 1 ;;
			*) src "$u.c" || return 1 ;;
			esac
			peak "$1-$u.c" "$1" -DONE_SOURCE=0 -c "$u.c" -o "$u.o" || return 1
			case $u in riscv*) ;; *) rm -f "$u.c" tcctools.c ;; esac
		done
		# the link needs ~2 MB of its own: no sources in /tmp meanwhile
		rm -f ./*.h ./*.c stab.def
		if [ "$2" = tcc3 ]; then rm -f ../tcc-src.tgz; fi
		peak "$1-link" "$1" -o "$2" tcc.o libtcc.o tccpp.o tccgen.o tccdbg.o \
			tccelf.o tccasm.o tccrun.o riscv32-gen.o riscv64-link.o \
			riscv64-asm.o -lm -ldl -lpthread || return 1
		rm -f ./*.o
		b1=$(cut -d' ' -f1 /proc/uptime)
		echo "time $1-builds-$2 $(echo "$b1 $b0" | awk '{printf "%.2f", $1 - $2}') s"
	}
	build cc tcc2 && build ./tcc2 tcc3
	ls -l tcc2 tcc3
	if cmp tcc2 tcc3; then
		pass=$((pass + 1)); echo "PASS selfhost (tcc2 and tcc3 identical)"
	else
		fail=$((fail + 1)); echo "FAIL selfhost"
	fi
	./tcc3 -o ../arith3 ../arith.c && ../arith3 > ../arith3.out
	cd .. && check arith3
fi

free
echo "tcc tests: $pass passed, $fail failed"
[ "$fail" = 0 ]
