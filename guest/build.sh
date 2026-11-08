#!/usr/bin/env bash
# Runs inside the container. /src = guest/src (bind), /out = guest/out (bind),
# /work = docker volume for build trees.
set -euo pipefail
KVER=${KVER:-6.12.116}
JOBS=$(nproc)
export BR2_DL_DIR=/work/dl

stage=${1:-all}

# disk / disk-rootfs / disk-kernel: the squashfs-root variant, into /out-disk
# (buildroot-disk.defconfig, linux.fragment + linux-disk.fragment)
if [[ $stage == disk* ]]; then
  if [[ $stage == disk || $stage == disk-rootfs ]]; then
    mkdir -p /work/br-disk
    cp /guest/buildroot-disk.defconfig /work/br-disk/defconfig
    make -C /src/buildroot O=/work/br-disk BR2_DEFCONFIG=/work/br-disk/defconfig defconfig
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
  make -C /src/buildroot O=/work/br BR2_DEFCONFIG=/work/br/defconfig defconfig
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
