#!/usr/bin/env bash
# Build the jittest guest, run it on the Mac interpreter for the reference
# output, then flash rvemu-pico to the RP2350 (Hazard3) and check that the
# board prints exactly the same jittest lines in every run (translator,
# lockstep, interpreter).
#   scripts/pico.sh [TIMEOUT_S]
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=$HOME/.rustup/toolchains/stable-aarch64-apple-darwin/lib/rustlib/aarch64-apple-darwin/bin
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

(cd jittest-guest && cargo build -q --release)
"$BIN/llvm-objcopy" -O binary jittest-guest/target/riscv32im-unknown-none-elf/release/jittest-guest jittest-guest/jittest.bin
cargo build -q --release -p rvemu
./target/release/rvemu -k jittest-guest/jittest.bin -m 4 -B > "$T/mac.out" 2>&1
cargo build -q --release -p rvemu-pico --target riscv32imac-unknown-none-elf
../rp2350/tools/run-riscv.sh target/riscv32imac-unknown-none-elf/release/rvemu-pico "${1:-180}" | tee "$T/board.out"

pick() { grep -aE '^(jittest|uart:|SELFCHECK|PANIC|unexpected|[a-z]+ +[0-9a-f]{8} )' | tr -d '\r'; }
pick < "$T/mac.out" > "$T/mac.lines"
fail=0
for mode in translator lockstep interpreter; do
  # the run's section: from its "--- mode ---" header to the next header
  awk -v m="--- $mode ---" '$0==m{on=1;next} /^(--- |=== summary)/{on=0} on' "$T/board.out" | pick > "$T/$mode.lines"
  if [ ! -s "$T/$mode.lines" ]; then
    echo "pico.sh: $mode: no output"; fail=1
  elif diff -u "$T/mac.lines" "$T/$mode.lines" > "$T/diff"; then
    echo "pico.sh: $mode matches the Mac reference ($(wc -l < "$T/mac.lines" | tr -d ' ') lines)"
  else
    echo "pico.sh: $mode MISMATCH against the Mac reference"; cat "$T/diff"; fail=1
  fi
done
grep -q "all runs AGREE" "$T/board.out" || { echo "pico.sh: board runs do not agree"; fail=1; }
exit $fail
