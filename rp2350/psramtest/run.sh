#!/bin/bash
# Flash an ELF to the target and print its RTT output until the test finishes.
# usage: run.sh build/psramtest.elf [timeout_s]
ELF=$1; T=${2:-60}
OUT=$(mktemp)
probe-rs run --chip RP235x --protocol swd --no-location --no-timestamps "$ELF" > "$OUT" 2>&1 &
PID=$!
for _ in $(seq 1 "$T"); do sleep 1; grep -q "RESULT\|FAIL: no PSRAM" "$OUT" && break; done
sleep 1; kill $PID 2>/dev/null; wait $PID 2>/dev/null
grep -v WARN "$OUT"; rm -f "$OUT"
