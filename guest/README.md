# guest

The Linux guests for the board, built in Docker:

    ./run-build.sh [all|rootfs|kernel|disk|disk-rootfs|disk-kernel]

The first run clones Buildroot 2026.08.1 into `src/buildroot`. Build trees
live in the Docker volume `rvemu-guest-work`, so later builds are
incremental. Logs and outputs are not tracked. The wrapper verifies the Buildroot checkout
is clean at the expected tag, retains `build-STAGE.log`, and writes
`build.json` beside successful outputs. See the
[development guide](../docs/development.md) for provenance and clean-build limits.

Packages that Buildroot doesn't have live in `br-external`, a BR2_EXTERNAL
tree named `RVEMU`: one directory per package under `package/`, and one
`source` line per package in `Config.in`. `package/dropbear` holds hooks on
Buildroot's own `dropbear` instead (no `Config.in`). `build.sh` always passes
`BR2_EXTERNAL` (Buildroot remembers it in the output directory).

What `build.sh` rebuilds by itself, because Buildroot wouldn't:
- a fresh `target/` when the configuration files differ from the last
  build, so an image holds only the configured packages;
- BusyBox, when `busybox-disk.fragment` differs from the last build's;
- an external package whose files changed (a new patch or `.mk`), or
  the files under `guest/` its `extra-sources` lists;
- MicroPython, when the C modules it was built with differ from the
  build's.

## The two guests

Both use the same rv32ima / ilp32 CPU (no F, D or C), musl, Linux 6.12.116
built with the Buildroot toolchain from `tinyconfig` plus `linux.fragment`,
and BusyBox init with a getty on ttyS0. Log in as root, no password.

| | initramfs guest (`out/`) | disk guest (`out-disk/`) |
|---|---|---|
| Stages | `all`, `rootfs`, `kernel` | `disk`, `disk-rootfs`, `disk-kernel` |
| Configs | `buildroot.defconfig`, `post-build.sh`, `linux.fragment` | `buildroot-disk.defconfig`, `busybox-disk.fragment`, `post-build-disk.sh`, `linux.fragment` + `linux-disk.fragment` |
| Root | `rootfs.cpio` initramfs | `rootfs.squashfs` on a read-only virtio-blk disk, tmpfs overlay; `/home` and `/root` on jffs2 in flash |
| Userland | static BusyBox | static BusyBox; shared musl for nano, mg, MicroPython, Lua 5.4, htop, tmux, lrzsz, bc, ncdu, lsof, less, tcc (`cc`), Dropbear (SSH), libgpiod tools, i2c-tools, spidev_test, MicroPython `machine`, the pin examples |
| Kernel | no networking, no block layer | + virtio-mmio blk and net, squashfs, overlayfs, sockets, swap, MTD + jffs2, the pin drivers, the native crypto devices |
| Used by | the boot benchmark (`scripts/pico-linux.sh`), `pico-shell.sh -i` | `scripts/pico-shell.sh` (default) |

Outputs in each directory: `Image`, `vmlinux`, `System.map`, `linux.config`
and the root filesystem.

## Disk guest

- **Root filesystem:** squashfs, LZ4, 16 KB blocks.
- **Boot:** the kernel mounts `/dev/vda` read-only and runs
  `/sbin/overlay-init` (from `post-build-disk.sh`). It mounts a tmpfs
  overlay on top of the squashfs, mounts `/home` (below), pivots into it
  and starts BusyBox init. Changes outside `/home` and `/root` last until
  reboot, and the squashfs stays visible at `/rom`. The firmware's default
  command line for a disk without an initramfs is
  `root=/dev/vda ro rootfstype=squashfs init=/sbin/overlay-init`.
- **BusyBox** is linked statically (`busybox-disk.fragment`). Everything
  else links against the shared musl.
  The `/dev/fd` and `/dev/std*` links are made at build time in
  `/lib/devlinks` and copied by one inittab line.
- **Init scripts:** only `S02sysctl`, `S02swap` (which does nothing without
  a swap device), `S40network` and `S50dropbear`. There is no syslogd, klogd, crond,
  seedrng (the device tree's `rng-seed` seeds the CRNG) or modules script.
  `dmesg` has the kernel log.
- **Network:** `eth0` is virtio-net, bridged to the board's Wi-Fi radio.
  `/etc/network/interfaces` starts `udhcpc -b` in the background.
- **`LANG=C.UTF-8`** in `/etc/profile`, for tmux and friends.
- **Not included:** vim (`vi` is BusyBox's), strace and make.

- **`claude-remote`** (`claude-remote/`): runs Claude Code on another host
  over ssh while it reads, edits and runs commands on this guest
  ([how to use it](../README.md#claude-code)). The tools are
  `claude-guest-mcp`, an MCP server over stdio: a static no_std Rust
  binary on raw system calls, so it costs the guest almost no memory.
  `run-build.sh` builds it on the host with Cargo for
  `riscv32im-unknown-none-elf` (the build container has no Rust) and
  `post-build-disk.sh` installs it in `/usr/bin`.

## /home on flash

jffs2 on the board's 1 MB flash region (the `rvemu,flash` device, `mtd0`),
mounted `noatime` by overlay-init. `/root` is a bind mount of `/home/root`,
created from the squashfs's `/root` the first time. How to use it is in
the top-level README.

- jffs2 is log-structured and checksums every node, so a power cut or
  reset loses at most the write in flight. A `write()` is on flash when it
  returns (no write buffer).
- It formats a blank region itself, and erases flash it can't parse, in
  the background. If the region holds no jffs2 node at all, the mount
  fails, and overlay-init formats it through the driver (`echo 1 >
  /sys/bus/platform/devices/f8000400.flash/format`).
- Its GC thread (`jffs2_gcd_mtd0`) runs SCHED_IDLE (build.sh edits
  `fs/jffs2/background.c`).
- Compression: rtime only.
- The device shows 16 KB erase blocks (four 4 KB flash sectors).
- The driver, `linux/rvemu-flash.c`, is copied into the kernel tree by
  build.sh. `/sys/bus/platform/devices/f8000400.flash/stats` reads the
  device's counters: erases, erases skipped (already blank), pages
  programmed, longest stall (us), total stall (ms).
- Without the device, overlay-init says "no flash storage; /home is in
  RAM" and `/home` is part of the overlay.

## Swap

`/etc/init.d/S02swap` runs mkswap and swapon on the virtio disk whose
serial is `rvemu-flash-swap`, when the firmware offers one
(`pico-shell.sh -w`). The device is 449 pages: 448 pages
(1792 KiB) of swap area, which leaves 1788 KiB of swap after mkswap's
header, and a last page that holds the device's statistics, which
`flashswap-stats` prints. The kernel has `CONFIG_SWAP` (on by default with
`CONFIG_BLOCK`). There is no zram or zswap.

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
- **Memory:** after every 128 kB it allocates, tcc checks MemAvailable,
  and below 512 kB it stops with "memory full" (patch 0007).
- **Tests:** `br-external/package/tcc/test/board-test.sh` copies the tests
  to the board over the console (`push.py`) and runs them (`run.sh`). The
  tests cover long long, float and double, structs by value, varargs,
  function pointers, a static library, kernel headers and an arithmetic
  hash that must match a native build. (The self-host stress test, `test/emu-selfhost.sh`, runs
  in the emulator, not on the board: see `../rvemu-rs/README.md`.)

## Pins

The disk guest has the board's GPIO, I2C, SPI, PWM and ADC. The pin map,
the tools and examples are in the top-level README; the devices are in
`../rvemu-rs/README.md`.

- **Drivers:** `linux/drivers/rvemu/` holds the machine's own drivers,
  which `build.sh` copies into the kernel tree (`drivers/rvemu`):
  `gpio-rvemu.c` (gpiochip with an irqchip, bias, edge events),
  `i2c-rvemu.c`, `spi-rvemu.c`, `pwm-rvemu.c` and `adc-rvemu.c` (IIO).
  Each one is off unless its `CONFIG_RVEMU_*` symbol is set, and binds to an
  `rvemu,*` node in the guest's device tree (`pico-shell.sh` adds the
  nodes when the kernel has the drivers).
- **Kernel options** (`linux-disk.fragment`): `GPIO_CDEV` (v2 only),
  `I2C_CHARDEV`, `SPI_SPIDEV`, `PWM`, `IIO` without buffers or triggers;
  `MMC_SPI`, `MMC_BLOCK` and `VFAT_FS` for the SD card on SPI1, which
  `S03sd` mounts at `/mnt/sd`.
- **Packages** (`buildroot-disk.defconfig`): libgpiod 2 tools (`gpiodetect`,
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

| Option | For |
|---|---|
| `CONFIG_MISC_FILESYSTEMS=y` | the menu squashfs sits under; tinyconfig turns it off |
| `CONFIG_SQUASHFS_LZ4`, `SQUASHFS_DECOMP_SINGLE` | matches the image; one decompressor buffer |
| `CONFIG_MTD`, `MTD_RVEMU_FLASH`, `JFFS2_FS` (no write buffer, zlib or lzo) | `/home` on the flash device |
| `CONFIG_OVERLAY_FS`, `CONFIG_TMPFS_XATTR` | the writable overlay (overlayfs wants xattrs on its upper layer) |
| `CONFIG_BASE_SMALL=y` | smaller kernel tables, for 8 MiB |
| `CONFIG_INET_TABLE_PERTURB_ORDER=8` | a 1 KiB RFC 6056 port perturbation table |
| `CONFIG_VIRTIO_NET`, `CONFIG_NET`, `UNIX`, `INET`, `PACKET` | Ethernet over virtio; tmux needs AF_UNIX |
| `EPOLL`, `EVENTFD`, `TIMERFD`, `SIGNALFD`, `INOTIFY_USER`, `SYSVIPC`, `FILE_LOCKING` | what the packages expect |
| `GPIO_CDEV`, `I2C_CHARDEV`, `SPI_SPIDEV`, `PWM`, `IIO`, `RVEMU_GPIO` & co. | the board's pins |
| `RVEMU_CRYPTO`, `RVEMU_SSH`, `RVEMU_BENCH` | the native crypto devices |
| no `VT`, `INPUT`, `SERIO`, `HID_SUPPORT` | the board has only the serial console |

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

## SSH

Dropbear 2026.94 (Buildroot's package, with `BR2_PACKAGE_DROPBEAR_CLIENT`):
`dropbear`, and `ssh`/`dbclient`, `scp`, `dropbearkey` and
`dropbearconvert` as links to it. How to log in is in the top-level README.

- **Server:** `S50dropbear` starts it on port 22 at boot. Password logins
  are compiled out (`br-external/package/dropbear/localoptions.h`), so root
  logs in with a key in `/root/.ssh/authorized_keys`. No reverse DNS. No SFTP
  server: OpenSSH's `scp` needs `-O`.
- **Host keys:** `/etc/dropbear` is a link to `/home/dropbear`
  (`post-build-disk.sh`), which `/etc/default/dropbear` creates.
  `dropbear -R` makes each key type's host key the first time a client asks
  for it, so they last across reboots.
- **Native crypto:** `br-external/package/dropbear` is not a package of
  its own: `dropbear-rvemu.mk` adds a post-patch hook to Buildroot's
  `dropbear`, which copies in the `rvemu-*.inc` adapters and the device
  headers and runs `patch.py` (exact edits that fail on any other Dropbear
  version). X25519, Ed25519 (keys, signing, verification), sntrup761 and
  `chacha20-poly1305@openssh.com` packets then go through `/dev/rvemu-crypto`
  and `/dev/rvemu-ssh`. The algorithms and wire format are Dropbear's. Where
  a device is missing or a process can't open it (they are root-only),
  Dropbear uses its own code; a failed authentication never falls back.
  `RVEMU_SSH_OFFLOAD=0` in a process's environment makes it use its own code.
- **Benchmark points:** with `RVEMU_SSH_BENCH=1`, `dbclient` marks the
  connect, the TCP connection, the first key exchange and authentication on
  `/dev/rvemu-bench`.
- `patch.py` also makes `dbclient` run its loop handler as soon as the
  last channel closes inside `channelio()`.

## Native crypto devices

The firmware's native engine (rvemu-rs/README.md) runs X25519, Ed25519,
ChaCha20-Poly1305, sntrup761 and LZ4 outside the emulated CPU. The kernel
drivers are in `linux/drivers/rvemu` (`CONFIG_RVEMU_CRYPTO`, `RVEMU_SSH`,
`RVEMU_BENCH` in `linux-disk.fragment`). Each device appears only when the
firmware has the engine, and is root-only (mode 0600).

**`/dev/rvemu-crypto`** (`crypto-rvemu.c`, ABI in `rvemu-crypto.h`): one
operation per `RVEMU_CRYPTO_RUN` ioctl. The request holds the operation,
the input pointer and length, the output pointer and capacity, the
parameters pointer and length, `output_len` (zero; set to the output's
length on success) and `flags` (zero). Input and output are at most 32 KiB,
parameters 4 KiB, and the buffers may overlap. Keys and random seeds come
from the caller; the engine keeps no state between operations.

| Op | Operation | Input | Parameters | Output |
|---:|---|---|---|---|
| 0 / 1 | raw LZ4 block decode / encode | block | none | bytes |
| 2 / 3 | ChaCha20-Poly1305 (RFC 8439) seal / open | plaintext / ciphertext + tag | key32, nonce12, AAD (up to 1024) | ciphertext + tag / plaintext |
| 4 / 5 | `chacha20-poly1305@openssh.com` packet seal / open | packet / packet + tag | payload key32, header key32, sequence (64-bit big-endian) | packet + tag / packet |
| 6 | X25519 | peer public key32 (the basepoint for a public key) | private scalar32 | shared secret32 / public key32 |
| 7 | Ed25519 public key | none | seed32 | public key32 |
| 8 | Ed25519 sign | message | seed32 | signature64 |
| 9 | Ed25519 verify (strict) | message | public key32, signature64 | none |
| 10 | sntrup761 keypair | none | random seed32 | public key1158, secret key1763 |
| 11 | sntrup761 encapsulate | public key1158 | random seed32 | ciphertext1039, shared secret32 |
| 12 | sntrup761 decapsulate | ciphertext1039 | secret key1763 | shared secret32 |

Errors: `EOPNOTSUPP` for an unknown operation, `EBADMSG` when
authentication or a signature fails, a low-order X25519 point or bad LZ4,
`EINVAL` for wrong lengths, `EMSGSIZE` for oversized buffers, `EINTR` when
a fatal signal ends the wait. On any error the output buffer is untouched,
or partly written on `EFAULT`; discard it. sntrup761 decapsulation of a bad
ciphertext returns the implicit-rejection secret, not an error. The device
serializes its callers; the kernel's buffers are wiped after every
operation.

A long operation doesn't stop the guest: the firmware runs it in 2 ms
slices, the guest takes its interrupts between them, and the driver lets
the scheduler run another task there (`cond_resched()`).

**`/dev/rvemu-ssh`** (`ssh-rvemu.c`, `rvemu-ssh.h`): `RVEMU_SSH_CRYPT`
seals (4) or opens (5) one `chacha20-poly1305@openssh.com` packet; the
request carries the 64-byte key and the sequence number.

**`/dev/rvemu-bench`** (`bench-rvemu.c`, `rvemu-ssh.h`):
`RVEMU_BENCH_POINT` records a point (tag 0-3) with the firmware's cycle,
instruction and XIP cache counters; `RVEMU_BENCH_REPORT` prints the points
on the console and clears them, with the native operation counts and stack
peaks since boot.

The test programs in `tests/` (`crypto-ioctl-test.c`, `crypto-vectors.c`,
`ssh-ioctl-test.c`, `ssh-board-test`, `rvemu-bench-report.c`) are not in
the image; [docs/development.md](../docs/development.md) has the test
images that add them.
