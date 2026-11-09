#!/bin/bash
# The board's native crypto test guest: the disk guest (guest/out-disk) with
# the test tools added to its root filesystem, into /out (guest/out-crypto).
# /work is the disk guest's build volume (read-only; guest/run-build.sh disk
# first), /repo this checkout (read-only), and /out must already hold
# crypto-vectors.bin (rvemu-rs/scripts/board-crypto-vectors.py).
set -euo pipefail
CROSS=/work/br-disk/host/bin/riscv32-buildroot-linux-musl-
DISK=/repo/guest/out-disk
[ -x "${CROSS}gcc" ] && [ -f "$DISK/rootfs.squashfs" ] && [ -f "$DISK/Image" ] ||
  { echo 'build the disk guest first (guest/run-build.sh disk)' >&2; exit 1; }
[ -f /out/crypto-vectors.bin ] || { echo 'no /out/crypto-vectors.bin' >&2; exit 1; }
cp "$DISK/Image" "$DISK/System.map" "$DISK/linux.config" /out/

# root filesystem: the disk guest's, unpacked as root so owners and modes
# stay as Buildroot made them, plus the tests
R=$(mktemp -d)
trap 'rm -rf "$R"' EXIT
/work/br-disk/host/bin/unsquashfs -q -f -d "$R" "$DISK/rootfs.squashfs"
for t in ssh-ioctl-test crypto-ioctl-test crypto-vectors rvemu-bench-report; do
  "${CROSS}gcc" -static -Os -Wall -Wextra -Werror -I/repo/guest/linux/drivers/rvemu "/repo/guest/tests/$t.c" -o "$R/usr/bin/$t"
  "${CROSS}strip" "$R/usr/bin/$t"
done
install -m755 /repo/guest/tests/ssh-board-test "$R/usr/bin/ssh-board-test"
install -D -m644 /out/crypto-vectors.bin "$R/usr/share/rvemu/crypto-vectors.bin"
rm -f /out/rootfs.squashfs
# as Buildroot's squashfs (buildroot-disk.defconfig: LZ4 high compression,
# 16 KiB blocks)
/work/br-disk/host/bin/mksquashfs "$R" /out/rootfs.squashfs -noappend -processors "$(nproc)" -b 16K -comp lz4 -Xhc -quiet
ls -la /out
