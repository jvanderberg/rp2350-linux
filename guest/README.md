# guest

The Linux guests for the board, built in Docker:

    BOARD_LOCK_OWNER=NAME ./run-build.sh [all|rootfs|kernel|disk|disk-rootfs|disk-kernel]
    BOARD_LOCK_OWNER=NAME PINS=1 ./run-build.sh disk     # with the board's pins

The first run clones Buildroot 2026.08.1 into `src/buildroot`. Build trees
live in the Docker volume `rvemu-guest-work`, so later builds are
incremental. That volume is shared by every checkout, so `run-build.sh`
waits for the `guest-build` lock (`../rp2350/tools/board-lock`) and needs
`BOARD_LOCK_OWNER`. It refuses to run while you hold the board lock. Logs
and outputs are not tracked.

Packages that Buildroot doesn't have live in `br-external`, a BR2_EXTERNAL
tree named `RVEMU`: one directory per package under `package/`, and one
`source` line per package in `Config.in`. `build.sh` always passes
`BR2_EXTERNAL` (Buildroot remembers it in the shared output directory).

What `build.sh` rebuilds by itself, because Buildroot wouldn't:
- a fresh `target/` when the configuration files differ from the last
  build in that tree, so an image never holds another checkout's packages;
- BusyBox, when `busybox-disk.fragment` differs from the last build's;
- an external package whose files changed (a new patch or `.mk`);
- MicroPython, when the C modules it was built with (none without
  `PINS=1`) differ from the build's.

## The two guests

Both use the same rv32ima / ilp32 CPU (no F, D or C), musl, Linux 6.12.116
built with the Buildroot toolchain from `tinyconfig` plus `linux.fragment`,
and BusyBox init with a getty on ttyS0. Log in as root, no password.

| | initramfs guest (`out/`) | disk guest (`out-disk/`) |
|---|---|---|
| Stages | `all`, `rootfs`, `kernel` | `disk`, `disk-rootfs`, `disk-kernel` |
| Configs | `buildroot.defconfig`, `post-build.sh`, `linux.fragment` | `buildroot-disk.defconfig`, `busybox-disk.fragment`, `post-build-disk.sh`, `linux.fragment` + `linux-disk.fragment` (`PINS=1`: + `buildroot-pins.fragment`, `linux-pins.fragment`) |
| Root | `rootfs.cpio` initramfs | `rootfs.squashfs` on a read-only virtio-blk disk, tmpfs overlay; `/home` and `/root` on jffs2 in flash |
| Userland | static BusyBox | static BusyBox; shared musl for nano, mg, MicroPython, Lua 5.4, htop, tmux, lrzsz, bc, ncdu, lsof, less, tcc (`cc`) (`PINS=1`: + libgpiod tools, i2c-tools, spidev_test, MicroPython `machine`, the pin examples) |
| Kernel | no networking, no block layer | + virtio-mmio blk and net, squashfs, overlayfs, sockets, swap, MTD + jffs2 (`PINS=1`: + the pin drivers) |
| Used by | the boot benchmark (`scripts/pico-linux.sh`), `pico-shell.sh -i` | `scripts/pico-shell.sh` (default) |

Outputs in each directory: `Image`, `vmlinux`, `System.map`, `linux.config`
and the root filesystem.

## Disk guest

- **Root filesystem:** squashfs, LZ4, 16 KB blocks. At 8 MiB the kernel
  evicts file pages often, and every read-back decompresses a whole block,
  so the blocks are kept small.
- **Boot:** the kernel mounts `/dev/vda` read-only and runs
  `/sbin/overlay-init` (from `post-build-disk.sh`). It mounts a tmpfs
  overlay on top of the squashfs, mounts `/home` (below), pivots into it
  and starts BusyBox init. Changes outside `/home` and `/root` last until
  reboot, and the squashfs stays visible at `/rom`. The firmware's default
  command line for a disk without an initramfs is
  `root=/dev/vda ro rootfstype=squashfs init=/sbin/overlay-init`.
- **BusyBox** is linked statically (`busybox-disk.fragment`), so an applet
  exec skips musl's dynamic loader (about 0.4 M guest instructions per
  fork+exec of `/bin/true`). Everything else links against the shared musl.
  The `/dev/fd` and `/dev/std*` links are made at build time in
  `/lib/devlinks` and copied by one inittab line.
- **Init scripts:** only `S02sysctl`, `S02swap` (which does nothing without
  a swap device) and `S40network`. Every script and command at boot is a
  fork+exec, so there is no syslogd, klogd, crond, seedrng (the device
  tree's `rng-seed` seeds the CRNG) or modules script. `dmesg` has the
  kernel log.
- **Network:** `eth0` is virtio-net, bridged to the board's Wi-Fi radio.
  `/etc/network/interfaces` starts `udhcpc -b`
  in the background, so boot doesn't wait for a lease or the Wi-Fi join.
- **`LANG=C.UTF-8`** in `/etc/profile`, for tmux and friends.
- **Not included:** vim (its 3.5 MB binary doesn't fit the 8 MiB guest's
  page cache; `vi` is BusyBox's), strace (Buildroot has none for RV32) and
  make.

## /home on flash

jffs2 on the board's 1 MB flash region (the `rvemu,flash` device, `mtd0`),
mounted `noatime` by overlay-init. `/root` is a bind mount of `/home/root`,
created from the squashfs's `/root` the first time. How to use it, its
costs and examples are in the top-level README.

- jffs2 is log-structured and checksums every node, so a power cut or
  reset loses at most the write in flight. A `write()` is on flash when it
  returns (no write buffer).
- It formats a blank region itself, and erases flash it can't parse, in
  the background. If the region holds no jffs2 node at all, the mount
  fails, and overlay-init formats it through the driver (`echo 1 >
  /sys/bus/platform/devices/f8000400.flash/format`).
- Its GC thread (`jffs2_gcd_mtd0`) is SCHED_IDLE (build.sh edits
  `fs/jffs2/background.c`): after a mount it CRC-checks every node, and
  that waits for idle time instead of slowing the boot.
- No compression beyond jffs2's rtime: zlib's deflate workspace is about
  270 KiB of vmalloc.
- The device shows 16 KB erase blocks (four 4 KB flash sectors): with
  4 KB blocks jffs2 needs about 80 KiB more RAM for its per-block tables.
- The driver, `linux/rvemu-flash.c`, is copied into the kernel tree by
  build.sh. `/sys/bus/platform/devices/f8000400.flash/stats` reads the
  device's counters: erases, erases skipped (already blank), pages
  programmed, longest stall (us), total stall (ms).
- Without the device, overlay-init says "no flash storage; /home is in
  RAM" and `/home` is part of the overlay.

Measured on the board:

| | |
|---|---|
| Boot to `login:`, steady state | +0.3 s |
| First boot on blank flash / on garbage (formats) | +1 s / +8.7 s, once |
| Guest RAM (perf-gate `mem_adj_kb`) | 40 KiB |
| Writing 256 KB in large writes / in 256-byte writes | about 260 KB/s / 13 KB/s |
| Longest guest stall: page program / sector erase | 0.5 ms / 51 ms |
| Wear, one day of typical use (300 shell commands, 20 saves of a 5 KB file, 200 log lines, five 50 KB rewrites) | 36 sector erases at 14% full, 252 at 72% full: 0.14 to 1 erase per sector per day against 100K cycles |

## Swap

`/etc/init.d/S02swap` runs mkswap and swapon on the virtio disk whose
serial is `rvemu-flash-swap`, when the firmware offers one
(`pico-shell.sh -w`). The device is 449 pages: 448 pages
(1792 KiB) of swap area, which leaves 1788 KiB of swap after mkswap's
header, and a last page that holds the device's statistics, which
`flashswap-stats` prints. The kernel has `CONFIG_SWAP` (on by default with
`CONFIG_BLOCK`). zram and zswap are left out (see the top-level README).

## C compiler

`cc` is TinyCC (`br-external/package/tcc`). It compiles, assembles and
links on the guest: `cc hello.c && ./a.out`, `tcc -run hello.c`, `cc -c`,
`tcc -ar` for static libraries. Examples are in the top-level README.

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
  image in a `ROOTFS_PRE_CMD_HOOK` (only when `BR2_PACKAGE_TCC` is set).
- **Linking:** dynamic, against `/lib/ld-musl-riscv32-sf.so.1`. There is
  no `libc.a`, so no `-static`.
- **Memory:** compiling a file takes 0.9–2.8 MB, against about 4 MB
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
  0.2–3.1 s. (The self-host stress test, `test/emu-selfhost.sh`, runs
  in the emulator, not on the board: see `../rvemu-rs/README.md`.)

## Pins

`PINS=1 ./run-build.sh disk` (or `disk-rootfs`, `disk-kernel`) builds the
disk guest with the board's GPIO, I2C, SPI, PWM and ADC. The pin map, the
tools, the costs and examples are in the top-level README; the devices are
in `../rvemu-rs/README.md`. It adds `linux-pins.fragment` to the kernel and
`buildroot-pins.fragment` to the root filesystem. Without it,
`linux-disk.fragment` and `buildroot-disk.defconfig` give a guest with
none of them, which pays nothing for them. Switching between the two modes
gives a fresh `target/`.

- **Drivers:** `linux/drivers/rvemu/` holds the machine's own drivers,
  which `build.sh` copies into the kernel tree (`drivers/rvemu`):
  `gpio-rvemu.c` (gpiochip with an irqchip, bias, edge events),
  `i2c-rvemu.c`, `spi-rvemu.c`, `pwm-rvemu.c` and `adc-rvemu.c` (IIO).
  Each one is off unless its `CONFIG_RVEMU_*` symbol is set, and binds to an
  `rvemu,*` node in the guest's device tree (`pico-shell.sh` adds the
  nodes when the kernel has the drivers).
- **Kernel options** (`linux-pins.fragment`): `GPIO_CDEV` (v2 only),
  `I2C_CHARDEV`, `SPI_SPIDEV`, `PWM`, `IIO` without buffers or triggers.
- **Packages** (`buildroot-pins.fragment`): libgpiod 2 tools (`gpiodetect`,
  `gpioinfo`, `gpioget`, `gpioset`, `gpiomon`, ...), i2c-tools,
  `spidev_test`, and two of this repository's (`br-external/package/`):
  - `micropython-machine`: `machine.py` (compiled to
    `/usr/lib/micropython/machine.mpy`), which extends MicroPython's
    built-in `machine` with `Pin`, `I2C`, `SPI`, `PWM` and `ADC` over
    `/dev/gpiochip0`, `/dev/i2c-N`, `/dev/spidev0.0`, PWM sysfs and IIO,
    and a small C module (`cmod/fcntl`) that MicroPython is built with, for
    `fcntl.ioctl`. `Pin.irq(handler)` runs the handler in a thread of its
    own for each edge.
  - `pins-examples`: `gpio-blink`, `gpio-edge`, `i2c-scan`, `spi-xfer` and
    `adc-read`, built and as source in `/usr/share/examples/pins`. On the
    board, build one with `cc -o gpio-blink gpio-blink.c`. The directory's
    `Makefile` is for cross builds (`make CC=...`), since the guest has no
    `make`.

## Kernel options

Of note in `linux-disk.fragment`:

| Option | Why |
|---|---|
| `CONFIG_MISC_FILESYSTEMS=y` | the menu squashfs sits under; tinyconfig turns it off |
| `CONFIG_SQUASHFS_LZ4`, `SQUASHFS_DECOMP_SINGLE` | matches the image; one decompressor buffer |
| `CONFIG_MTD`, `MTD_RVEMU_FLASH`, `JFFS2_FS` (no write buffer, zlib or lzo) | `/home` on the flash device |
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

## Sizes and boot

| | initramfs guest | disk guest | disk guest, `PINS=1` |
|---|---|---|---|
| Image (of which text, read from flash on the board) | 2.4 MB (1.6 MB) | 4.1 MB (3.1 MB) | 4.2 MB (3.2 MB) |
| Root filesystem | 1.2 MB cpio | 5.4 MB squashfs | 5.6 MB squashfs |
| Boot to `login:` on the board, 348 MHz | 5.2 s (the benchmark) | 10.9 s | 11.2 s |
| MemTotal after boot | | 7,004 KiB | 6,972 KiB |
