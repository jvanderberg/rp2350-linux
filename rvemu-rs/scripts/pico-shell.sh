#!/usr/bin/env bash
# Interactive Linux on the RP2350: load the guest (if changed) and
# rvemu-pico-shell over USB BOOTSEL, then print the console port. Connect with
#   screen /dev/cu.usbmodemrvemu1 115200        (Ctrl-A k to quit)
# The board goes back to BOOTSEL on `stty -f PORT 1200`, which this script
# also uses to replace a running shell.
#   scripts/pico-shell.sh [-m RAM_MB] [-i] [-b]
# Default: guest/out-disk, root on a squashfs in flash (nano, tmux,
# MicroPython, ...; guest/run-build.sh disk). -i: the small initramfs guest in
# guest/out. -b: (re)build that guest in Docker first (guest/run-build.sh;
# incremental once the first Buildroot build is done).
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/pico-common.sh
M=8; INITRAMFS=0; BUILD=0
while getopts "m:ib" o; do case $o in m) M=$OPTARG;; i) INITRAMFS=1;; b) BUILD=1;; *) exit 2;; esac; done
if [ $BUILD = 1 ]; then
  if [ $INITRAMFS = 1 ]; then ../guest/run-build.sh all; else ../guest/run-build.sh disk; fi
fi
if [ $INITRAMFS = 1 ]; then
  K=../guest/out/Image; I=../guest/out/rootfs.cpio
else
  K=../guest/out-disk/Image; I=-; DISK=../guest/out-disk/rootfs.squashfs
  [ -f "$DISK" ] || { echo "pico-shell.sh: no $DISK (cd guest && ./run-build.sh disk), or use -i"; exit 1; }
fi
ELF=target/riscv32imac-unknown-none-elf/release/rvemu-pico-shell
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

cargo build -q --release -p rvemu
cargo build -q --release -p rvemu-pico --target riscv32imac-unknown-none-elf
# the pin devices only for a kernel with their drivers (PINS=1 guest build)
IO=0; grep -q '^CONFIG_RVEMU_GPIO=y' "$(dirname "$K")/linux.config" 2>/dev/null && IO=1
NET=1 IO=$IO pack_guest "$K" "$I" "$M" "$T/blob.bin" "$T/text.bin"
ensure_bootsel "$ELF"
keep_wifi
load_text "$T/text.bin"
load_blob "$T/blob.bin"
[ $INITRAMFS = 1 ] || load_disk "$DISK"
picotool load -v -x -t elf "$ELF"
# when the firmware started: scripts/perf-gate.sh times boot to login: from it
python3 -c 'import time; print(time.time())' > "$LOCK_HOME/last-launch"
for _ in $(seq 1 20); do [ -n "$(shell_port)" ] && break; sleep 0.5; done
PORT=$(shell_port)
[ -n "$PORT" ] || { echo "pico-shell.sh: console port did not appear"; exit 1; }
echo "pico-shell.sh: Linux is booting; console: screen $PORT 115200"
