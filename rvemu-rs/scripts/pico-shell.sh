#!/usr/bin/env bash
# Interactive Linux on the RP2350: load the guest (if changed) and
# rvemu-pico-shell over USB BOOTSEL, then print the console port. Connect with
#   screen /dev/cu.usbmodemrvemu1 115200        (Ctrl-A k to quit)
# The board goes back to BOOTSEL on `stty -f PORT 1200`, which this script
# also uses to replace a running shell.
#   scripts/pico-shell.sh [-m RAM] [-i] [-b] [-w] [-d DIR]
# Default: guest/out-disk, root on a squashfs in flash (nano, tmux,
# MicroPython, ...; guest/run-build.sh disk). -i: the small initramfs guest in
# guest/out. -b: (re)build that guest in Docker first (guest/run-build.sh;
# incremental once the first Buildroot build is done). -w: give the guest
# the flash swap device in 0x10200000-0x103FEFFF (/dev/vdb, or /dev/vda with
# -i; 1788 KiB of swap). -d DIR: a disk guest other than guest/out-disk
# (DIR has its Image, System.map, linux.config and rootfs.squashfs). -m RAM:
# guest RAM in MiB, or KiB as NK (default 8128K: the PSRAM's 8 MiB less the
# native engine's 64 KiB).
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/pico-common.sh
M=8128K; INITRAMFS=0; BUILD=0; SWAP=0; GUEST=../guest/out-disk
while getopts "m:ibwd:" o; do case $o in m) M=$OPTARG;; i) INITRAMFS=1;; b) BUILD=1;; w) SWAP=1;; d) GUEST=$OPTARG;; *) exit 2;; esac; done
if [ $BUILD = 1 ]; then
  if [ $INITRAMFS = 1 ]; then ../guest/run-build.sh all; else ../guest/run-build.sh disk; fi
fi
if [ $INITRAMFS = 1 ]; then
  K=../guest/out/Image; I=../guest/out/rootfs.cpio
else
  K=$GUEST/Image; I=-; DISK=$GUEST/rootfs.squashfs
  [ -f "$DISK" ] || { echo "pico-shell.sh: no $DISK (cd guest && ./run-build.sh disk), or use -i"; exit 1; }
fi
ELF=target/riscv32imac-unknown-none-elf/release/rvemu-pico-shell
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

cargo build --locked -q --release -p rvemu
cargo build --locked -q --release -p rvemu-pico --target riscv32imac-unknown-none-elf
# the pin devices only for a kernel with their drivers (the disk guest's)
IO=0; grep -q '^CONFIG_RVEMU_GPIO=y' "$(dirname "$K")/linux.config" 2>/dev/null && IO=1
NET=1 NOR=1 SWAP=$SWAP IO=$IO pack_guest "$K" "$I" "$M" "$T/blob.bin" "$T/text.bin"
ensure_bootsel "$ELF"
load_text "$T/text.bin"
load_blob "$T/blob.bin"
[ $INITRAMFS = 1 ] || load_disk "$DISK"
picotool load -v -x -t elf "$ELF"
# when the firmware started: scripts/perf-gate.sh times boot to login: from it
python3 -c 'import time; print(time.time())' > "$LAUNCH"
for _ in $(seq 1 20); do [ -n "$(shell_port)" ] && break; sleep 0.5; done
PORT=$(shell_port)
[ -n "$PORT" ] || { echo "pico-shell.sh: console port did not appear"; exit 1; }
echo "pico-shell.sh: Linux is booting; console: screen $PORT 115200"
