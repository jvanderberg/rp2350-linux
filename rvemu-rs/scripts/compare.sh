#!/usr/bin/env bash
# Run the C reference (../rvemu) and the Rust emulator on the same
# deterministic boot and require identical console output and statistics.
# Usage: scripts/compare.sh [Image] [initrd] [until-string]
set -euo pipefail
cd "$(dirname "$0")/.."
K=${1:-../ref/prebuilt/Image}
I=${2:-../ref/prebuilt/rootfs.cpio}
U=${3:-login:}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
make -s -C ../rvemu
cargo build -q --release
../rvemu/rvemu -B -M -k "$K" -i "$I" -u "$U" >"$T/c.out" 2>"$T/c.err"
./target/release/rvemu -B -M -k "$K" -i "$I" -u "$U" >"$T/rs.out" 2>"$T/rs.err"
fail=0
cmp -s "$T/c.out" "$T/rs.out" || { echo "console output differs"; fail=1; }
diff <(grep -v "host time" "$T/c.err") <(grep -v "host time" "$T/rs.err") || fail=1
grep "host time" "$T/c.err" | sed 's/^/C    /'
grep "host time" "$T/rs.err" | sed 's/^/Rust /'
[ $fail = 0 ] && echo "identical: $(grep 'guest instructions' "$T/rs.err")"
exit $fail
