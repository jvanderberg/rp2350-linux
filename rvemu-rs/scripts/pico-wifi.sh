#!/usr/bin/env bash
# Store the Wi-Fi network for rvemu-pico-shell's radio in the board's flash
# (the 4 KB at 0x103FF000, below the guest blob), then restart the board. Asks for the passphrase
# without echoing it; nothing is written to disk on the Mac.
#   scripts/pico-wifi.sh SSID          (empty passphrase: an open network)
#   scripts/pico-wifi.sh --forget      (erase the stored network)
# Format: "WIFI", SSID length, passphrase length (u32 LE), SSID[32], passphrase[64].
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/pico-common.sh
[ $# = 1 ] || { echo "usage: $0 SSID | --forget"; exit 2; }
ELF=target/riscv32imac-unknown-none-elf/release/rvemu-pico-shell
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
if [ "$1" = --forget ]; then
  head -c 4096 /dev/zero > "$T/wifi.bin"
else
  read -rsp "passphrase for $1 (blank for an open network): " PASS; echo
  [ ${#1} -le 32 ] || { echo "SSID longer than 32 bytes"; exit 1; }
  [ ${#PASS} = 0 ] || { [ ${#PASS} -ge 8 ] && [ ${#PASS} -le 63 ]; } || { echo "WPA passphrases are 8..63 characters"; exit 1; }
  # passed on stdin, not argv, so it doesn't show in ps
  printf '%s\n%s' "$1" "$PASS" | python3 -c '
import struct, sys
ssid, pw = sys.stdin.buffer.read().split(b"\n", 1)
blk = struct.pack("<4sII32s64s", b"WIFI", len(ssid), len(pw), ssid, pw)
open(sys.argv[1], "wb").write(blk.ljust(4096, b"\0"))' "$T/wifi.bin"
  unset PASS
fi
ensure_bootsel "$ELF"
picotool load -v -t bin "$T/wifi.bin" -o "$WIFI_ADDR"
picotool reboot
echo "pico-wifi.sh: stored; the board is restarting (status on RTT: [wifi] lines)"
