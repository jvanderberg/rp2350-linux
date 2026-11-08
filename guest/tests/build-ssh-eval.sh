#!/bin/bash
# Isolated evaluation build in /eval. /work is the READ-ONLY existing guest
# build volume; /repo is this checkout (read-only); /out holds artifacts.
set -euo pipefail
CROSS=/work/br/host/bin/riscv32-buildroot-linux-musl-
KVER=6.12.116
DVER=2026.94
[ -x "${CROSS}gcc" ] || { echo 'Build the initramfs guest toolchain first (guest/run-build.sh all)' >&2; exit 1; }
mkdir -p /eval
if [ ! -d /eval/linux ]; then cp -a "/work/linux-$KVER" /eval/linux; fi
cd /eval/linux
cp /repo/guest/linux/rvemu-ssh.c /repo/guest/linux/rvemu-ssh.h drivers/char/
cp /repo/guest/linux/rvemu-crypto.c /repo/guest/linux/rvemu-crypto.h /repo/guest/linux/rvemu-native.h drivers/char/
grep -q rvemu-ssh.o drivers/char/Makefile || echo 'obj-y += rvemu-ssh.o' >> drivers/char/Makefile
grep -q rvemu-crypto.o drivers/char/Makefile || echo 'obj-y += rvemu-crypto.o' >> drivers/char/Makefile
scripts/config --enable NET --enable INET --enable UNIX
make ARCH=riscv CROSS_COMPILE="$CROSS" olddefconfig
make ARCH=riscv CROSS_COMPILE="$CROSS" -j"$(nproc)" Image
cp arch/riscv/boot/Image /out/Image
cp System.map /out/System.map
cp .config /out/linux.config
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
mkdir -p /eval/root
# Always reconstruct the private root to avoid retaining test keys/state.
rm -rf /eval/root
mkdir /eval/root
cd /eval/root
cpio -idmu < /work/br/images/rootfs.cpio
mkdir -p usr/sbin usr/bin proc sys dev tmp root
install -m755 "/eval/dropbear-$DVER/dropbearmulti" usr/sbin/dropbear
"${CROSS}strip" usr/sbin/dropbear
ln -sf ../sbin/dropbear usr/bin/dbclient
ln -sf ../sbin/dropbear usr/bin/dropbearkey
"${CROSS}gcc" -static -Os -Wall -Wextra -I/repo/guest/linux /repo/guest/tests/ssh-ioctl-test.c -o usr/bin/ssh-ioctl-test
"${CROSS}strip" usr/bin/ssh-ioctl-test
"${CROSS}gcc" -static -Os -Wall -Wextra -I/repo/guest/linux /repo/guest/tests/crypto-ioctl-test.c -o usr/bin/crypto-ioctl-test
"${CROSS}strip" usr/bin/crypto-ioctl-test
install -m755 /repo/guest/tests/ssh-init init
find . -print0 | cpio --null -o -H newc > /out/rootfs.cpio
stat -c 'dropbear bytes: %s' usr/sbin/dropbear
