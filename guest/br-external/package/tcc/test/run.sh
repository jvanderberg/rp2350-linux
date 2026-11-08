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
	while v=$(sed -n 's/^VmHWM: *\([0-9]*\).*/\1/p' "/proc/$p/status" 2>/dev/null) &&
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
	cd tcc-src || exit 1
	peak "cc-builds-tcc" cc -o tcc2 tcc.c -lm -ldl -lpthread
	peak "tcc2-builds-tcc" ./tcc2 -o tcc3 tcc.c -lm -ldl -lpthread
	ls -l tcc2 tcc3
	if cmp tcc2 tcc3; then
		pass=$((pass + 1)); echo "PASS selfhost (tcc2 and tcc3 identical)"
	else
		fail=$((fail + 1)); echo "FAIL selfhost"
	fi
	./tcc3 -o ../arith3 ../arith.c && ../arith3 > ../arith3.out
	cd .. && cp arith.expect arith3.expect && check arith3
fi

free
echo "tcc tests: $pass passed, $fail failed"
[ "$fail" = 0 ]
