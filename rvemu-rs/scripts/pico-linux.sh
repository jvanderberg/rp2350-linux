#!/usr/bin/env bash
# Boot the Linux guest on the RP2350 (Hazard3). Loads go over the target's
# USB (bootrom BOOTSEL + picotool): Image + initramfs + DTB packed into a blob
# at 0x10400000 (only when it changed), then rvemu-pico-linux; RTT is read by
# attaching probe-rs. The firmware ends by rebooting into BOOTSEL, so runs
# chain without touching the board. The board runs the translator (and the
# interpreter if the firmware's RUN_INTERPRETER is set) to `login:`; every
# run must match a Mac reference run (same console bytes, same guest
# instruction count).
#   scripts/pico-linux.sh [TIMEOUT_S]      (env: FORCE_BLOB=1 reloads the blob)
set -euo pipefail
cd "$(dirname "$0")/.."
K=../guest/out/Image; I=../guest/out/rootfs.cpio; M=8; U="login:"
ELF=target/riscv32imac-unknown-none-elf/release/rvemu-pico-linux
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
. scripts/pico-common.sh

cargo build -q --release -p rvemu
./target/release/rvemu -k "$K" -i "$I" -m "$M" -X "$(text_len "$K")" -u "$U" -B > "$T/mac.out" 2> "$T/mac.err"
python3 ../rp2350/tools/fnv.py "$T/mac.out" "$T/mac.err" > "$T/mac.ref"
read -r REF_N REF_H < "$T/mac.ref"
echo "pico-linux.sh: Mac reference: $REF_N guest insns, console hash $REF_H"

pack_guest "$K" "$I" "$M" "$T/blob.bin" "$T/text.bin"
cargo build -q --release -p rvemu-pico --target riscv32imac-unknown-none-elf
ensure_bootsel "$ELF"
keep_wifi
load_text "$T/text.bin"
load_blob "$T/blob.bin"
picotool load -v -x -t elf "$ELF"
# the debug module answers once the bootrom has handed over to the app
for _ in $(seq 1 20); do
  "$P" read --chip RP235x_riscv --protocol swd b32 0x400d800c 1 >/dev/null 2>&1 && break
  sleep 0.5
done
ATTACH=1 ../rp2350/tools/run-riscv.sh "$ELF" "${1:-300}" '\] done|HOST TRAP|panicked|no valid guest' | tee "$T/board.out"

fail=0
grep -aq "^RESULT translator " "$T/board.out" || { echo "pico-linux.sh: translator: no result"; fail=1; }
for mode in translator interpreter; do
  line=$(grep -a "^RESULT $mode " "$T/board.out" || true)
  [ -n "$line" ] || continue
  n=$(sed -E 's/.* instret ([0-9]+) .*/\1/' <<<"$line"); h=$(sed -E 's/.* hash ([0-9a-f]+).*/\1/' <<<"$line")
  if [ "$n" = "$REF_N" ] && [ "$h" = "$REF_H" ]; then
    echo "pico-linux.sh: $mode matches the Mac reference ($n insns, console $h)"
  else
    echo "pico-linux.sh: $mode MISMATCH: $n insns / console $h, Mac $REF_N / $REF_H"; fail=1
  fi
done
exit $fail
