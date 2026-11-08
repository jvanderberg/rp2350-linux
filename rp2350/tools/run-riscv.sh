#!/bin/bash
# Flash a Hazard3 (RISC-V) ELF and stream its RTT output with probe-rs
# master (RP235x_riscv target; see ../README.md). Stops when a line matches
# STOP_REGEX or after TIMEOUT seconds.
# usage: run-riscv.sh ELF [TIMEOUT_S] [STOP_REGEX]
# ATTACH=1: don't flash, attach to the running program (loaded e.g. by picotool).
set -u
E=$1
TIMEOUT=${2:-120}
STOP=${3:-'\] done|HOST TRAP|panicked'}
P=${PROBE_RS:-$HOME/git/toolchains/probe-rs-master/bin/probe-rs}
"$(dirname "$0")/board-lock" check "${BOARD_LOCK_OWNER:-nobody}" || exit 1
OUT=$(mktemp)
CMD=run; [ "${ATTACH:-0}" = 1 ] && CMD=attach
"$P" "$CMD" --chip RP235x_riscv --protocol swd --no-location --no-timestamps "$E" > "$OUT" 2>&1 &
PID=$!
tail -f "$OUT" &
TAIL=$!
for _ in $(seq 1 "$TIMEOUT"); do
  sleep 1
  grep -qE "$STOP" "$OUT" && break
  kill -0 $PID 2>/dev/null || break
done
sleep 0.5
kill $PID $TAIL 2>/dev/null; wait 2>/dev/null
rm -f "$OUT"
