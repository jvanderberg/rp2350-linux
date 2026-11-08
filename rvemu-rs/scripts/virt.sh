#!/usr/bin/env bash
# Run the bare-metal emulator under qemu-system-riscv32 -M virt.
#   scripts/virt.sh MODE [options]
#   MODE: interp | jit | lockstep
#   -k Image -i initrd|none -d disk.img -m MB -u STRING -n MILLION_INSNS -I (interactive) -c (count host insns via -icount)
#   -D: the disk guest (guest/out-disk: Image, root on rootfs.squashfs)
set -euo pipefail
cd "$(dirname "$0")/.."
MODE=${1:?mode}; shift
K=../guest/out/Image; I=../guest/out/rootfs.cpio; D=; M=8; U="login:"; A=; N=0; BATCH=1; ICOUNT=()
while getopts "k:i:d:Dm:u:n:a:Ic" o; do case $o in
  a) A=$OPTARG;; k) K=$OPTARG;; i) I=$OPTARG;; d) D=$OPTARG;; m) M=$OPTARG;; u) U=$OPTARG;; n) N=$OPTARG;; I) BATCH=0;;
  D) K=../guest/out-disk/Image; I=none; D=../guest/out-disk/rootfs.squashfs;;
  c) ICOUNT=(-icount "shift=0,align=off,sleep=off");; *) exit 2;; esac; done
case $MODE in interp) MN=0;; jit) MN=1;; lockstep) MN=2;; *) echo "bad mode"; exit 2;; esac

# The frontend feature also reserves native workspace memory.
RVEMU_FEATURES=${RVEMU_FEATURES:-}
RVEMU_FEATURES=${RVEMU_FEATURES//rvcore\/pv-buffers/pv-buffers}
FEATURES=(); [ -n "${RVEMU_FEATURES:-}" ] && FEATURES=(--features "$RVEMU_FEATURES")
cargo build -q --release -p rvemu "${FEATURES[@]}"
cargo build -q --release -p rvemu-virt --target riscv32imac-unknown-none-elf "${FEATURES[@]}"
ELF=target/riscv32imac-unknown-none-elf/release/rvemu-virt
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

# same device tree the Mac host generates for these inputs
INITRD_ARGS=(); [ "$I" != none ] && INITRD_ARGS=(-i "$I")
[ -n "$D" ] && INITRD_ARGS+=(-d "$D")
[ -n "$A" ] && INITRD_ARGS+=(-a "$A")
./target/release/rvemu -D -m "$M" "${INITRD_ARGS[@]}" > "$T/guest.dts"
DLEN=0; [ -n "$D" ] && DLEN=$(stat -f%z "$D")
dtc -q -I dts -O dtb -o "$T/guest.dtb" "$T/guest.dts"
python3 - "$T/info.bin" "$MN" "$M" "$N" "$BATCH" "$U" "$DLEN" <<'P'
import struct, sys
out, mode, mb, n, batch, until, dlen = sys.argv[1:]
open(out, "wb").write(struct.pack("<IIIII60sI", 0x4D455652, int(mode), int(mb) << 20, int(n), int(batch), until.encode(), int(dlen)))
P
RAM=$((M << 20)); BASE=$((0x81000000))
DTB_ADDR=$((RAM - 0x10000))
LOADERS=()
if [ "$I" != none ]; then
  ISZ=$(stat -f%z "$I")
  INITRD_ADDR=$(( (DTB_ADDR - ISZ) & ~0xFFF ))
fi
hex() { printf "0x%x" "$1"; }
[ "$I" != none ] && LOADERS=(-device "loader,file=$I,addr=$(hex $((BASE + INITRD_ADDR))),force-raw=on")
[ -n "$D" ] && LOADERS+=(-device "loader,file=$D,addr=0x8a000000,force-raw=on")

exec qemu-system-riscv32 -machine virt -bios none -m 256M -nographic -monitor none -serial stdio \
  "${ICOUNT[@]}" \
  -kernel "$ELF" \
  -device "loader,file=$K,addr=$(hex $BASE),force-raw=on" \
  "${LOADERS[@]}" \
  -device "loader,file=$T/guest.dtb,addr=$(hex $((BASE + DTB_ADDR))),force-raw=on" \
  -device "loader,file=$T/info.bin,addr=0x80f00000,force-raw=on"
