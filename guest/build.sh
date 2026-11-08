#!/usr/bin/env bash
# Runs inside the container. /src = guest/src (bind), /out = guest/out (bind),
# /work = docker volume for build trees.
set -euo pipefail
KVER=${KVER:-6.12.116}
JOBS=$(nproc)
export BR2_DL_DIR=/work/dl

stage=${1:-all}

# Buildroot saves BR2_EXTERNAL in the (shared) output directory, so always
# pass it: the external tree, or empty when this checkout has none.
EXT=; [[ -f /guest/br-external/external.desc ]] && EXT=/guest/br-external

# The /work volume is shared by every checkout. Buildroot never removes files
# from target/, so when the configuration (defconfig, post-build script, overlay
# or external tree under /guest) differs from the last one built in this tree,
# reinstall every package into a fresh target/ (no recompiling) so the image
# holds only this checkout's packages.
fresh_target() {   # BR_OUT FILES...
  local o=$1; shift
  local sum; sum=$(cd /guest && { find "$@" -type f 2>/dev/null || true; } | sort | xargs cat 2>/dev/null | sha256sum | cut -d' ' -f1)
  if [[ -d $o/target && "$(cat "$o/.config-sum" 2>/dev/null)" != "$sum" ]]; then
    echo "build.sh: configuration changed since the last build in $o: fresh target/"
    rm -rf "$o/target"
    find "$o/build" -name .stamp_target_installed -delete 2>/dev/null || true
    rm -f "$o"/build/*/.stamp_images_installed
    # host-gcc-final puts libgcc_s & co. into target/ from its host install
    rm -f "$o"/build/host-gcc-final-*/.stamp_host_installed
  fi
  echo "$sum" > "$o/.config-sum"
}

# disk / disk-rootfs / disk-kernel: the squashfs-root variant, into /out-disk
# (buildroot-disk.defconfig, linux.fragment + linux-disk.fragment)
if [[ $stage == disk* ]]; then
  if [[ $stage == disk || $stage == disk-rootfs ]]; then
    mkdir -p /work/br-disk
    cp /guest/buildroot-disk.defconfig /work/br-disk/defconfig
    fresh_target /work/br-disk buildroot-disk.defconfig post-build-disk.sh overlay-disk br-external
    make -C /src/buildroot O=/work/br-disk BR2_EXTERNAL="$EXT" BR2_DEFCONFIG=/work/br-disk/defconfig defconfig
    make -C /work/br-disk -j"$JOBS"
    cp /work/br-disk/images/rootfs.squashfs /out-disk/rootfs.squashfs
  fi
  if [[ $stage == disk || $stage == disk-kernel ]]; then
    CROSS=/work/br-disk/host/bin/riscv32-buildroot-linux-musl-
    # its own source tree (the initramfs kernel builds in linux-$KVER, and
    # kbuild refuses out-of-tree builds from a tree built in place)
    K=/work/linux-disk-$KVER
    if [[ ! -d $K ]]; then
      mkdir -p "$K" && curl -fsSL "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KVER.tar.xz" | tar xJ -C "$K" --strip-components=1
    fi
    cd "$K"
    # the rvemu NOR flash driver for /home (guest/linux/rvemu-flash.c); the
    # tree is shared, and builds without CONFIG_MTD_RVEMU_FLASH ignore it
    cmp -s /guest/linux/rvemu-flash.c drivers/mtd/devices/rvemu-flash.c || cp /guest/linux/rvemu-flash.c drivers/mtd/devices/
    grep -q MTD_RVEMU_FLASH drivers/mtd/devices/Kconfig ||
      sed -i '$i config MTD_RVEMU_FLASH\n\ttristate "rvemu NOR flash device"\n\tdepends on HAS_IOMEM \&\& OF\n' drivers/mtd/devices/Kconfig
    grep -q MTD_RVEMU_FLASH drivers/mtd/devices/Makefile ||
      echo 'obj-$(CONFIG_MTD_RVEMU_FLASH) += rvemu-flash.o' >> drivers/mtd/devices/Makefile
    # jffs2's GC thread CRC-checks every node after a mount: idle time only
    # (SCHED_IDLE, not nice 10), so a full /home doesn't hold up the boot. A
    # writer that runs out of space collects garbage itself.
    sed -i 's|^\tset_user_nice(current, 10);$|\tsched_setscheduler_nocheck(current, SCHED_IDLE, \&(struct sched_param){ 0 });|' fs/jffs2/background.c
    make ARCH=riscv CROSS_COMPILE=$CROSS tinyconfig
    scripts/kconfig/merge_config.sh -m .config /guest/linux.fragment /guest/linux-disk.fragment
    make ARCH=riscv CROSS_COMPILE=$CROSS olddefconfig
    make ARCH=riscv CROSS_COMPILE=$CROSS -j"$JOBS" Image vmlinux
    cp arch/riscv/boot/Image vmlinux System.map /out-disk/
    cp .config /out-disk/linux.config
  fi
  ls -la /out-disk
  exit 0
fi

if [[ $stage == all || $stage == rootfs ]]; then
  mkdir -p /work/br
  cp /guest/buildroot.defconfig /work/br/defconfig
  fresh_target /work/br buildroot.defconfig post-build.sh overlay br-external
  make -C /src/buildroot O=/work/br BR2_EXTERNAL="$EXT" BR2_DEFCONFIG=/work/br/defconfig defconfig
  make -C /work/br -j"$JOBS"
  cp /work/br/images/rootfs.cpio /out/rootfs.cpio
fi

if [[ $stage == all || $stage == kernel ]]; then
  CROSS=/work/br/host/bin/riscv32-buildroot-linux-musl-
  cd /work
  if [[ ! -d linux-$KVER ]]; then
    curl -fsSL "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KVER.tar.xz" | tar xJ
  fi
  cd "linux-$KVER"
  make ARCH=riscv CROSS_COMPILE=$CROSS tinyconfig
  scripts/kconfig/merge_config.sh -m .config /guest/linux.fragment
  make ARCH=riscv CROSS_COMPILE=$CROSS olddefconfig
  make ARCH=riscv CROSS_COMPILE=$CROSS -j"$JOBS" Image vmlinux
  cp arch/riscv/boot/Image /out/Image
  cp vmlinux System.map .config /out/
  mv /out/.config /out/linux.config
fi
ls -la /out
