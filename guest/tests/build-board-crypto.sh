#!/bin/bash
# The board's native crypto test guest, in /eval (its own Docker volume):
# the disk guest's kernel with the crypto/SSH/benchmark adapters
# (guest/linux/rvemu-{crypto,ssh}.c), and the disk guest's squashfs plus the
# patched Dropbear and the test tools. /work is the disk guest's build volume
# (read-only; guest/run-build.sh disk first), /repo this checkout
# (read-only), /out the output directory (guest/out-crypto), which must
# already hold dropbear.tar.bz2 and crypto-vectors.bin.
set -euo pipefail
CROSS=/work/br-disk/host/bin/riscv32-buildroot-linux-musl-
KVER=6.12.116
DVER=2026.94
[ -x "${CROSS}gcc" ] && [ -f /work/br-disk/images/rootfs.squashfs ] && [ -f /work/linux-disk-$KVER/.config ] ||
  { echo 'build the disk guest first (guest/run-build.sh disk)' >&2; exit 1; }
mkdir -p /eval

# kernel: the disk guest's tree and configuration, plus the adapters
if [ ! -d /eval/linux ]; then cp -a "/work/linux-disk-$KVER" /eval/linux; fi
cd /eval/linux
# the disk tree's sources and configuration as the last disk build left them
rm -rf drivers/rvemu && cp -a "/work/linux-disk-$KVER/drivers/rvemu" drivers/rvemu
cp "/work/linux-disk-$KVER/drivers/mtd/devices/rvemu-flash.c" drivers/mtd/devices/
cp "/work/linux-disk-$KVER/fs/jffs2/background.c" fs/jffs2/
cp "/work/linux-disk-$KVER/.config" .config
cp /repo/guest/linux/rvemu-ssh.c /repo/guest/linux/rvemu-ssh.h drivers/char/
cp /repo/guest/linux/rvemu-crypto.c /repo/guest/linux/rvemu-crypto.h drivers/char/
grep -q rvemu-ssh.o drivers/char/Makefile || echo 'obj-y += rvemu-ssh.o' >> drivers/char/Makefile
grep -q rvemu-crypto.o drivers/char/Makefile || echo 'obj-y += rvemu-crypto.o' >> drivers/char/Makefile
make ARCH=riscv CROSS_COMPILE="$CROSS" olddefconfig
make ARCH=riscv CROSS_COMPILE="$CROSS" -j"$(nproc)" Image vmlinux
cp arch/riscv/boot/Image System.map vmlinux /out/
cp .config /out/linux.config

# Dropbear, as the SSH evaluation guest builds it (guest/tests/build-ssh-eval.sh)
cd /eval
printf '%s  %s\n' e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81 /out/dropbear.tar.bz2 | sha256sum -c -
if [ ! -d "dropbear-$DVER" ]; then tar xf /out/dropbear.tar.bz2; fi
cd "dropbear-$DVER"
cat > localoptions.h <<'H'
#define DROPBEAR_SMALL_CODE 1
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_CLI_PASSWORD_AUTH 0
#define DO_HOST_LOOKUP 0
H
if [ ! -f Makefile ]; then
  ./configure --host=riscv32-buildroot-linux-musl --disable-harden --disable-zlib \
    --enable-bundled-libtom --disable-wtmp --disable-lastlog CC="${CROSS}gcc" CFLAGS='-Os' LDFLAGS='-static'
fi
cp /repo/guest/tests/dropbear/rvemu-offload.inc /repo/guest/linux/rvemu-ssh.h src/
cp /repo/guest/tests/dropbear/rvemu-bench.h /repo/guest/tests/dropbear/rvemu-bench.inc src/
cp /repo/guest/tests/dropbear/rvemu-crypto.inc /repo/guest/linux/rvemu-crypto.h src/
python3 /repo/guest/tests/dropbear/patch.py .
sum=$(cat src/chachapoly.c src/chachapoly.h src/common-session.c src/cli-main.c src/cli-session.c src/cli-auth.c src/common-kex.c src/curve25519.c src/sntrup761.c src/rvemu-*.inc src/rvemu-*.h localoptions.h | sha256sum)
if [ "$(cat .rvemu-source-sum 2>/dev/null || true)" != "$sum" ]; then
  make clean
  echo "$sum" > .rvemu-source-sum
fi
make -j"$(nproc)" MULTI=1 PROGRAMS='dropbear dbclient dropbearkey'

# root filesystem: the disk guest's, unpacked as root so owners and modes
# stay as Buildroot made them, plus Dropbear and the tests
rm -rf /eval/root
/work/br-disk/host/bin/unsquashfs -q -d /eval/root /work/br-disk/images/rootfs.squashfs
cd /eval/root
install -m755 "/eval/dropbear-$DVER/dropbearmulti" usr/sbin/dropbear
"${CROSS}strip" usr/sbin/dropbear
ln -sf ../sbin/dropbear usr/bin/dbclient
ln -sf ../sbin/dropbear usr/bin/dropbearkey
for t in ssh-ioctl-test crypto-ioctl-test crypto-vectors rvemu-bench-report; do
  "${CROSS}gcc" -static -Os -Wall -Wextra -Werror -I/repo/guest/linux "/repo/guest/tests/$t.c" -o "usr/bin/$t"
  "${CROSS}strip" "usr/bin/$t"
done
install -m755 /repo/guest/tests/ssh-board-test usr/bin/ssh-board-test
install -D -m644 /out/crypto-vectors.bin usr/share/rvemu/crypto-vectors.bin
rm -f /out/rootfs.squashfs
# as Buildroot's squashfs (buildroot-disk.defconfig: LZ4 high compression,
# 16 KiB blocks)
/work/br-disk/host/bin/mksquashfs /eval/root /out/rootfs.squashfs -noappend -processors "$(nproc)" -b 16K -comp lz4 -Xhc -quiet
stat -c 'dropbear bytes: %s' usr/sbin/dropbear
ls -la /out
