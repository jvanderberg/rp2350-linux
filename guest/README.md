# guest

The Linux guests for rvemu, built in Docker:

    ./run-build.sh [all|rootfs|kernel|disk|disk-rootfs|disk-kernel]

The first run clones Buildroot 2026.08.1 into `src/buildroot`. Build trees
live in the Docker volume `rvemu-guest-work`, so later builds are
incremental. Logs and outputs are not tracked.

Packages that Buildroot doesn't have live in `br-external`, a BR2_EXTERNAL
tree named `RVEMU`: one directory per package under `package/`, and one
`source` line per package in `Config.in`. `build.sh` always passes
`BR2_EXTERNAL` (Buildroot remembers it in the shared output directory). It
also dircleans an external package whose files have changed since the last
build in that tree, so a new patch or `.mk` takes effect.

There are two guests. Both use the same rv32ima / ilp32 CPU (no F, D or C),
musl, Linux 6.12.116 built with the Buildroot toolchain from `tinyconfig` plus
`linux.fragment`, and BusyBox init with a getty on ttyS0. Log in as root, no
password.

| | initramfs guest (`out/`) | disk guest (`out-disk/`) |
|---|---|---|
| Stages | `all`, `rootfs`, `kernel` | `disk`, `disk-rootfs`, `disk-kernel` |
| Configs | `buildroot.defconfig`, `post-build.sh`, `linux.fragment` | `buildroot-disk.defconfig`, `post-build-disk.sh`, `linux.fragment` + `linux-disk.fragment` |
| Root | `rootfs.cpio` initramfs | `rootfs.squashfs` on a read-only virtio-blk disk, tmpfs overlay |
| Userland | static BusyBox | shared musl; BusyBox, nano, mg, MicroPython, Lua 5.4, htop, tmux, lrzsz, bc, ncdu, lsof, less, tcc (`cc`) |
| Kernel | no networking, no block layer | + virtio-mmio blk and net, squashfs, overlayfs, sockets |
| Used by | the boot benchmark (`scripts/pico-linux.sh`), `pico-shell.sh -i` | `scripts/pico-shell.sh` (default), `rvemu -d`, `virt.sh -D` |

Outputs in each directory: `Image`, `vmlinux`, `System.map`, `linux.config`
and the root filesystem.

## Disk guest

- **Root filesystem:** squashfs, LZ4, 16 KB blocks. At 8 MiB the kernel
  evicts file pages often, and every read-back decompresses a whole block,
  so the blocks are kept small.
- **Boot:** the kernel mounts `/dev/vda` read-only and runs
  `/sbin/overlay-init` (from `post-build-disk.sh`). It mounts a tmpfs
  overlay on top of the squashfs, pivots into it and starts BusyBox init.
  Changes last until reboot, and the squashfs stays visible at `/rom`. The
  Mac host's and the firmware's default command line for a disk without an
  initramfs is
  `root=/dev/vda ro rootfstype=squashfs init=/sbin/overlay-init`.
- **Network:** `eth0` is virtio-net. `/etc/network/interfaces` runs DHCP in
  the background (`udhcpc -b`), so boot doesn't wait for the Wi-Fi join.
  On the board the NIC is bridged to the Wi-Fi radio. On the Mac, `rvemu -N`
  gives a stub LAN (see `rvemu-rs/README.md`).
- **`LANG=C.UTF-8`** in `/etc/profile`, for tmux and friends.
- **Not included:** vim (its 3.5 MB binary doesn't fit the 8 MiB guest's
  page cache; `vi` is BusyBox's) and strace (Buildroot has none for RV32).

## C compiler

`cc` is TinyCC (`br-external/package/tcc`). It compiles, assembles and
links on the guest: `cc hello.c && ./a.out`, `cc -run hello.c`, `cc -c`,
`cc -ar` for static libraries.

- **Target:** upstream tinycc (70adbb12) has no 32-bit RISC-V backend.
  The package's patches add one (`riscv32-gen.c`) for rv32ima and the ilp32
  ABI, and fix the shared RISC-V assembler and linker for 32 bits. It never
  emits compressed instructions. `long long` lives in register pairs.
  `float` and `double` live in integer registers and go through libgcc's
  soft-float routines. `long double` is the ABI's 128-bit quad in memory and
  in calls, but is computed in double precision. Objects mix with gcc's both
  ways.
- **Files:** `/usr/bin/tcc` and `cc`; `/usr/lib/tcc` holds `libtcc1.a`,
  libgcc.a (without the quad, half-float and _BitInt members), `runmain.o`,
  musl's empty libm/libpthread/... archives and tcc's own headers.
  `/usr/include` holds musl's headers and the kernel UAPI headers for
  devices: `linux/gpio.h`, `i2c.h`, `i2c-dev.h`, `spi/spidev.h`, input,
  uinput, iio, watchdog, rtc, serial, fb and net. The list is in
  `install-headers.sh`, which copies the headers they include as well.
  Buildroot deletes `/usr/include` and static libraries from `target/`, so
  the package keeps them in its own `devroot/` and copies them into the
  image in a `ROOTFS_PRE_CMD_HOOK`.
- **Linking:** dynamic, against `/lib/ld-musl-riscv32-sf.so.1`. There is
  no `libc.a`, so no `-static`.
- **Memory:** compiling a file takes 0.9–2.8 MB, against about 3.7 MB
  available after login. With no swap, a compile that took the last of it
  would evict the page cache that holds every program's code and stall the
  guest for minutes. So after every 128 kB it allocates, tcc checks
  MemAvailable, and below 512 kB it stops with "memory full". Larger
  programs build one file at a time (`cc -c`).
- **Size:** 704 KiB of squashfs.
- **Tests:** `br-external/package/tcc/test/board-test.sh` copies the tests
  to the board over the console (`push.py`) and runs them (`run.sh`). The
  tests cover long long, float and double, structs by value, varargs,
  function pointers, a static library, kernel headers and an arithmetic
  hash that must match a native build. On the board, each compiles in
  0.2–3.1 s. `run.sh selfhost` adds a stress case for the emulator only
  (QEMU and the translator, `-m 11`). tcc builds itself from its sources
  one file at a time, then builds itself again with the result, and the
  two binaries must be identical.

Kernel options of note in `linux-disk.fragment`:

| Option | Why |
|---|---|
| `CONFIG_MISC_FILESYSTEMS=y` | the menu squashfs sits under; tinyconfig turns it off |
| `CONFIG_SQUASHFS_LZ4`, `SQUASHFS_DECOMP_SINGLE` | matches the image; one decompressor buffer |
| `CONFIG_OVERLAY_FS`, `CONFIG_TMPFS_XATTR` | the writable overlay (overlayfs wants xattrs on its upper layer) |
| `CONFIG_BASE_SMALL=y` | smaller kernel tables, for 8 MiB |
| `CONFIG_INET_TABLE_PERTURB_ORDER=8` | 1 KiB instead of 256 KiB for RFC 6056 port perturbation |
| `CONFIG_VIRTIO_NET`, `CONFIG_NET`, `UNIX`, `INET`, `PACKET` | Ethernet over virtio; tmux needs AF_UNIX |
| `EPOLL`, `EVENTFD`, `TIMERFD`, `SIGNALFD`, `INOTIFY_USER`, `SYSVIPC`, `FILE_LOCKING` | what the packages expect |

## Configuration notes

- `CONFIG_EFI` defaults to y and *selects* `RISCV_ISA_C`, putting compressed
  instructions back in. It's disabled in the fragment.
- `CONFIG_LEGACY_PTYS` creates 512 `/dev` nodes. At 8 MiB, devtmpfs gets only
  about 548 inodes, so `/dev/ttyS0` never appears and getty loops forever.
  It's disabled.
- Misaligned-access probing at boot is off
  (`RISCV_PROBE_UNALIGNED_ACCESS`): every access in that loop is a
  translator exit.
- No self-patching code (jump labels, ftrace, kprobes, modules), which
  matters for the translator.
- The disk kernel builds in its own source tree (`/work/linux-disk-*`):
  kbuild refuses out-of-tree builds from a tree that was built in place.

## Sizes and boot (Mac host, `-m 8`, kernel text as extra RAM, boot to `login:`)

| | initramfs guest | disk guest |
|---|---|---|
| Image (of which text) | 2.4 MB (1.6 MB) | 4.0 MB (3.0 MB) |
| Root filesystem | 1.2 MB cpio | 5.2 MB squashfs |
| Guest instructions to `login:` | 58.1 M | 147.5 M (with `-N`) |
| Boot on the board, 348 MHz | 8.0 s | 19.7 s |
