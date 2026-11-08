#!/usr/bin/env bash
# Run the C reference (../rvemu) and the Rust emulator on the same
# deterministic boot and require identical console output and statistics.
# Usage: scripts/compare.sh [Image] [initrd] [until-string]
set -euo pipefail
cd "$(dirname "$0")/.."
K=${1:-../guest/out/Image}
I=${2:-../guest/out/rootfs.cpio}
U=${3:-login:}
. ../scripts/preflight.sh
for tool in cargo make cc dtc python3; do need_tool "$tool" 'See docs/development.md.'; done
need_file "$K" 'Run: (cd ../guest && ./run-build.sh all)'
need_file "$I" 'Run: (cd ../guest && ./run-build.sh all)'
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
make -s -C ../rvemu
cargo build --locked -q --release
# Use identical machine inputs, including the deterministic entropy seed.
# The C reference's built-in device-tree generator predates that seed.
./target/release/rvemu -D -m 64 -i "$I" > "$T/guest.dts"
dtc -q -I dts -O dtb -o "$T/guest.dtb" "$T/guest.dts"
../rvemu/rvemu -B -M -m 64 -b "$T/guest.dtb" -k "$K" -i "$I" -u "$U" >"$T/c.out" 2>"$T/c.err"
./target/release/rvemu -B -M -m 64 -b "$T/guest.dtb" -k "$K" -i "$I" -u "$U" >"$T/rs.out" 2>"$T/rs.err"
# Rust's host calls set_readonly_prefix(0) after Cpu::new, adding exactly
# one setup TLB flush. The C host has no such call. Normalize only that
# known startup operation; retain every guest counter and any other difference.
python3 - "$T/rs.err" "$T/rs.normalized" <<'P'
import pathlib, re, sys
text = pathlib.Path(sys.argv[1]).read_text()
def setup_flush(match):
    count = int(match[2])
    if count < 1:
        raise SystemExit("Rust setup TLB flush counter missing")
    return match[1] + str(count - 1) + match[3]
text, matches = re.subn(r"^(tlb misses .*?  flushes )(\d+)(  page flushes )",
                        setup_flush, text, flags=re.MULTILINE)
if matches != 1:
    raise SystemExit("Expected exactly one Rust TLB statistics line")
pathlib.Path(sys.argv[2]).write_text(text)
P
fail=0
for impl in c rs; do
  grep -q 'to --until match [1-9][0-9]*' "$T/$impl.err" || {
    echo "$impl did not reach the requested console marker: $U"; fail=1;
  }
done
diff -u "$T/c.out" "$T/rs.out" || { echo "console output differs"; fail=1; }
diff <(grep -v "host time" "$T/c.err") <(grep -v "host time" "$T/rs.normalized") || fail=1
grep "host time" "$T/c.err" | sed 's/^/C    /'
grep "host time" "$T/rs.err" | sed 's/^/Rust /'
[ $fail = 0 ] && echo "identical (excluding one Rust host setup TLB flush): $(grep 'guest instructions' "$T/rs.err")"
exit $fail
