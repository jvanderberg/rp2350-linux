#!/bin/bash
# Alternative to run.sh: flash and stream RTT through Raspberry Pi's OpenOCD
# (rp2350-riscv.cfg) instead of probe-rs. RTT is read by ../tools/rtt.py over
# OpenOCD's Tcl port, because OpenOCD registers its rtt commands only for
# Cortex-M/HLA/ARC targets, not RISC-V. Memory access is forced to the system
# bus: OpenOCD's default program-buffer access halts the hart for every read.
# usage: run-openocd.sh [timeout_s] [elf]
set -u
cd "$(dirname "$0")" || exit 1
T=${OPENOCD_RPI:-$HOME/git/toolchains/openocd-rpi}
NM=$HOME/.rustup/toolchains/stable-aarch64-apple-darwin/lib/rustlib/aarch64-apple-darwin/bin/llvm-nm
TIMEOUT=${1:-120}
E=${2:-target/riscv32imac-unknown-none-elf/release/psramtest}
RTT=$("$NM" "$E" | awk '/ _SEGGER_RTT$/{print "0x"$1}')
LOG=$(mktemp)
"$T/openocd" -s "$T/scripts" -f interface/cmsis-dap.cfg -f target/rp2350-riscv.cfg \
  -c "adapter speed 5000" \
  -c "rp2350.rv0 riscv set_mem_access sysbus" -c "rp2350.rv1 riscv set_mem_access sysbus" \
  -c "program $E verify" -c "reset run" > "$LOG" 2>&1 &
OCD=$!
until grep -qE "Verified OK|Error" "$LOG" || ! kill -0 $OCD 2>/dev/null; do sleep 0.2; done
grep -E "Programming Finished|Verified|Error" "$LOG"
sleep 0.5
python3 -I ../tools/rtt.py "$RTT" --until 'core clock at end|FAIL|panicked' --timeout "$TIMEOUT"
kill $OCD 2>/dev/null; wait 2>/dev/null
rm -f "$LOG"
