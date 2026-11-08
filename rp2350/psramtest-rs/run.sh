#!/bin/bash
# Flash the RISC-V build and stream its RTT output with probe-rs.
#
# Needs probe-rs with the RP235x_riscv target and its RISC-V flash algorithm
# (probe-rs PRs #3952 and #4565, on master after v0.32.0). Built into
# ~/git/toolchains/probe-rs-master so the system probe-rs is untouched:
#   cargo install probe-rs-tools --git https://github.com/probe-rs/probe-rs \
#     --locked --root ~/git/toolchains/probe-rs-master
#
# The older OpenOCD route (Raspberry Pi's build + ../tools/rtt.py) is
# run-openocd.sh; OpenOCD has no RTT commands for RISC-V targets.
# usage: run.sh [timeout_s] [elf]
set -u
cd "$(dirname "$0")" || exit 1
P=${PROBE_RS:-$HOME/git/toolchains/probe-rs-master/bin/probe-rs}
TIMEOUT=${1:-120}
E=${2:-target/riscv32imac-unknown-none-elf/release/psramtest}
OUT=$(mktemp)
"$P" run --chip RP235x_riscv --protocol swd --no-location --no-timestamps "$E" > "$OUT" 2>&1 &
PID=$!
tail -f "$OUT" &
TAIL=$!
for _ in $(seq 1 "$TIMEOUT"); do
  sleep 1
  grep -qE "core clock at end|FAIL|panicked" "$OUT" && break
  kill -0 $PID 2>/dev/null || break
done
sleep 0.5
kill $PID $TAIL 2>/dev/null; wait 2>/dev/null
rm -f "$OUT"
