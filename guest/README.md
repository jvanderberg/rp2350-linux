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
`source` line per package in `Config.in`. `build.sh` always passes
`BR2_EXTERNAL` (Buildroot remembers it in the output directory).

What `build.sh` rebuilds by itself, because Buildroot wouldn't:
- a fresh `target/` when the configuration files differ from the last
  build, so an image holds only the configured packages;
- BusyBox, when `busybox-disk.fragment` differs from the last build's;
- an external package whose files changed (a new patch or `.mk`);
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
| Userland | static BusyBox | static BusyBox; shared musl for nano, mg, MicroPython, Lua 5.4, htop, tmux, lrzsz, bc, ncdu, lsof, less, tcc (`cc`), libgpiod tools, i2c-tools, spidev_test, MicroPython `machine`, the pin examples |
| Kernel | no networking, no block layer | + virtio-mmio blk and net, squashfs, overlayfs, sockets, swap, MTD + jffs2, the pin drivers |
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
  a swap device) and `S40network`. There is no syslogd, klogd, crond,
  seedrng (the device tree's `rng-seed` seeds the CRNG) or modules script.
  `dmesg` has the kernel log.
- **Network:** `eth0` is virtio-net, bridged to the board's Wi-Fi radio.
  `/etc/network/interfaces` starts `udhcpc -b` in the background.
- **`LANG=C.UTF-8`** in `/etc/profile`, for tmux and friends.
- **Not included:** vim (`vi` is BusyBox's), strace and make.

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
  `I2C_CHARDEV`, `SPI_SPIDEV`, `PWM`, `IIO` without buffers or triggers.
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
| `GPIO_CDEV`, `I2C_CHARDEV`, `SPI_SPIDEV`, `PWM`, `IIO`, `RVEMU_*` | the board's pins |
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

## General-purpose native crypto interface

The engine is a stateless primitive service, usable without SSH or Linux.
`rvaccel::run()` is the Rust entry point and SBI `0x0a000001` is the physical
buffer interface. The reusable Linux driver is `linux/rvemu-crypto.c`; its
fixed-width RV32 userspace ABI is `linux/rvemu-crypto.h`. Any adapted program
can open `/dev/rvemu-crypto` and issue `RVEMU_CRYPTO_RUN`. The Dropbear patch
is a consumer of that interface, not part of the engine.

Each request supplies operation, input pointer/length, output pointer/capacity,
parameter pointer/length, initially-zero output length and zero flags.
Successful calls return the actual output length in the request. The device
is root-only by default. It uses independent physical input/output/parameter
buffers per descriptor, serializes shared-descriptor requests, and wipes all
used buffers after every request. User input/output buffers may overlap.
Limits are 32 KiB input/output and 4 KiB parameters; unknown operations return
`EOPNOTSUPP`. Failed signature validation or low-order X25519 inputs return
`EBADMSG` without copying output back. As with other copy-to-user interfaces,
a bad destination pointer can cause partial copying on `EFAULT`; callers
must discard output on any error.

| FID | Operation | Input | Parameters | Output |
|---:|---|---|---|---|
| 0 / 1 | Raw LZ4 decode / encode | block | empty | transformed bytes |
| 2 / 3 | IETF ChaCha20-Poly1305 seal / open | plaintext / ciphertext + tag | key32 + nonce12 + AAD (up to 1024) | ciphertext + tag / plaintext |
| 4 / 5 | Existing SSH packet seal / open | packet / packet + tag | payload key32 + header key32 + sequence BE64 | transformed packet |
| 6 | X25519 | peer public key32 (or basepoint) | private scalar32 | shared secret/public key32 |
| 7 | Ed25519 public-key derivation | empty | private seed32 | public key32 |
| 8 | Ed25519 sign | arbitrary message | private seed32 | signature64 |
| 9 | Ed25519 strict verify | arbitrary message | public key32 + signature64 | empty on success |
| 10 | sntrup761 keypair | empty | fresh random seed32 | public key1158 + secret key1763 |
| 11 | sntrup761 encapsulate | public key1158 | fresh random seed32 | ciphertext1039 + shared secret32 |
| 12 | sntrup761 decapsulate | ciphertext1039 | secret key1763 | shared secret32 |

Long operations do not hold up the guest. The kernel driver calls an
operation with SBI FID `0x100 | FID`, which allows the firmware to pause it:
on the board, core0's timer stops it after a 2 ms slice and the call returns
error `-100`. The driver then lets other tasks and interrupts run
(`cond_resched()`) and calls FID `0x200` to run the next slice, until the
operation finishes. A fatal signal ends the wait: the driver calls FID
`0x201`, which drops the operation and erases its stack and scratch, and the
ioctl fails with `EINTR`. The firmware runs one operation at a time; a new
one while another is paused fails with error `-7`, and the driver serializes
all callers. FIDs 0–12 still run an operation to completion in one call.
Native time advances the guest's clock.

No keys, identities or SSH sessions are retained in the engine. Callers own
nonce uniqueness and randomness. The KEM consumes a freshly sampled 32-byte
CSPRNG seed for each keypair/encapsulation; its seeded ChaCha20 generator
expands that entropy inside the native implementation. Dropbear obtains the
seeds from its existing `genrandom()`. KEM decapsulation preserves implicit
rejection: malformed ciphertext produces a fallback secret, not a ciphertext
validity error. Protocol key confirmation must still reject the mismatch.
Ed25519 uses pure Ed25519, not Ed25519ph, and strict verification rejects weak
keys/noncanonical signatures. These are explicit algorithm contracts, not
protocol policy.

The native implementation pins `x25519-dalek` 2.0.1 and `ed25519-dalek` 2.2.0.
`vendor/sntrup761` contains [sntrup761 0.4.0](https://github.com/mikelodder7/sntrup761),
with licenses/archive hash retained. Its local workspace refactor moves large
scalar temporary arrays into caller-owned storage without changing the
arithmetic, serialization, sampling or implicit-rejection algorithms.
Its manifest disables dependency default features so the advertised
allocation-free `no_std` implementation builds for bare-metal RV32. See its
`PROVENANCE` and original manifest; tests compare 16 seeded transcripts with
the untouched upstream implementation. All new runtime dependencies build for
`riscv32imac-unknown-none-elf` without a C compiler or host OS RNG.

Validation is independent of SSH: `tests/crypto-ioctl-test.c` signs a firmware
manifest string, performs a KEM exchange, exercises overlapping buffers and
checks malformed ioctl requests and rejection behavior. `rvaccel` tests use
[RFC 7748](https://www.rfc-editor.org/rfc/rfc7748.html),
[RFC 8032](https://www.rfc-editor.org/rfc/rfc8032.html), and
[sntrup761 draft vectors](https://www.ietf.org/archive/id/draft-josefsson-ntruprime-streamlined-00.html).
`python3 scripts/native-crypto-eval.py` (from `rvemu-rs/`) runs all new native
operations, known vectors and negative cases in RV32 translator/interpreter
lockstep, followed by the existing translator regression. Real SSH with one
native and one original-software endpoint checks wire interoperability in
both directions.

Applications need an adapter to use this API. This is not yet a Linux Crypto
API/AF_ALG provider, an OpenSSL provider, a key vault, or a complete TLS engine.
The old `/dev/rvemu-ssh` packet adapter remains compatible; the new generic
interface makes the handshake primitives available to other consumers.

## SSH with native packet offload

The separate SSH evaluation guest now exercises the complete path:

```
Dropbear adapters -> /dev/rvemu-crypto (generic primitives)
                  -> /dev/rvemu-ssh (existing SSH packet adapter)
                  -> Linux-owned physical buffers -> SBI -> native crypto engine
```

The ordinary disk defconfig does not yet install Dropbear. This test builds
Dropbear 2026.94, the kernel bridge, and a private initramfs; it does not flash
a board. Both endpoints run inside the emulated guest on loopback, so the
host network stub needs no TCP forwarding.

**The crypto backend is native emulator code.** The buffer SBI handler calls
the native crypto libraries directly, outside the Linux guest's interpreter/translator. On
RP2350 it executes directly on the physical CPU. For translator measurements,
QEMU runs the whole RV32 firmware as a stand-in for that physical CPU; the
native crypto does not go through the inner guest translator. The macOS host
runner instead compiles the backend for the Mac CPU. This is a software
crypto engine in the emulator, not an added hardware cipher peripheral.

From `rvemu-rs/`:

```sh
# Build the private kernel/rootfs and run native SSH.
python3 scripts/ssh-eval.py
# The same firmware, but Dropbear uses its original software cipher.
python3 scripts/ssh-eval.py --skip-build --offload off
# Interoperability: enable only one endpoint.
python3 scripts/ssh-eval.py --skip-build --offload client
python3 scripts/ssh-eval.py --skip-build --offload server
# Prove automatic software fallback when firmware has no extension.
python3 scripts/ssh-eval.py --skip-build --without-extension
# Board-like RAM allocation; kernel text modeled as flash (host only).
python3 scripts/ssh-eval.py --skip-build --memory 8 --xip
# Actual RV32 translator and host instruction accounting.
python3 scripts/ssh-eval.py --skip-build --engine jit --offload on --timeout 600
python3 scripts/ssh-eval.py --skip-build --engine jit --offload off --timeout 600
# Connection setup ONLY: fresh boot, one TCP connect through authentication.
python3 scripts/ssh-eval.py --test connection --engine jit --offload on
python3 scripts/ssh-eval.py --skip-build --test connection --engine jit --offload off
```

Prerequisites: Docker's `rvemu-guest` image and `rvemu-guest-work` volume
containing `/work/br`'s musl cross-toolchain, `/work/br/images/rootfs.cpio`,
and `/work/linux-6.12.116` (produced by `guest/run-build.sh all`). Host runs
also need Cargo and `dtc`; translator runs need QEMU and the
`riscv32imac-unknown-none-elf` Rust target. The toolchain/base initramfs are
cached inputs, not reproduced by this script.

The builder mounts the original build volume read-only and uses a separate
`rvemu-ssh-eval-work` volume. It enables networking in the private kernel,
links `guest/linux/rvemu-ssh.c`, verifies Dropbear's tarball against the
Buildroot 2026.08.1 SHA-256, applies exact edits from
`guest/tests/dropbear/patch.py`, and assembles the rootfs. Logs, images and
JSON results live in `rvemu-rs/target/ssh-eval/`; `--skip-build` reuses images.
The `--offload` setting applies at runtime, keeping the firmware identical
for on/off comparisons. `--without-extension` deliberately builds different
firmware for the fallback test only.

The guest generates disposable Ed25519 host/client keys, listens on **guest
127.0.0.1:2222**, and forces `chacha20-poly1305@openssh.com`. Password auth is
compiled out. It checks authentication, remote command execution and exit
status 7, a byte-exact 64 KiB upload/download, a SHA-256-verified 1 MiB stream
in each direction, and rejection of an unrelated key. `--bulk-mib N` adjusts
the stream size (1–16); the stream reuses a 64 KiB buffer rather than storing
megabytes in RAM. Host-key acceptance is automatic for this disposable
loopback test. No keys are installed in the regular image or host, and no
host port is opened. It does not test Wi-Fi, interactive PTYs or SFTP.

The harness requires every success marker, the authentication-rejection
message, and emulator counters. Software mode must make zero native packet
calls. Native/mixed modes must make more than ten encrypt and decrypt calls,
so the two negative-interface probes alone cannot produce a false pass.
The JSON includes guest instruction count, QEMU host instruction count when
available, simulated idle time, and host wall time. The harness also requires
less than 60 simulated seconds of idle time; successful bytes alone must not
hide a connection stalled until rekey. Wall time is a test diagnostic, not RP2350
latency. Native work is omitted from guest instruction counts, so use QEMU
**host** counts to compare total execution cost.

### Interface and fallback

`rvemu-ssh.h` defines a fixed-width RV32 ioctl request with source/destination
user pointers, operation, length, the 64-byte session key and 32-bit packet
sequence. The root-only misc device validates requests and allocates separate
contiguous kernel buffers per open descriptor. A mutex serializes requests
sharing a descriptor. It never hands userspace virtual pointers to SBI.
The source is copied in, firmware is called, and output is copied to userspace
only after success. Parameter keys are wiped after each request; data buffers are wiped on errors
and on close. Successful calls reuse the buffers, avoiding allocation and
whole-slab wiping for every packet. Buffers
are limited to 32 KiB; overlapping user buffers work because the kernel owns
separate input/output copies. Separate opens have separate buffers, and every request supplies its key
and sequence; sessions share no implicit cipher state.

The existing buffer extension gains FID 4 (SSH seal) and FID 5 (SSH open).
Parameters are `payload key[32] || header key[32] || sequence[8, big-endian]`.
The operation implements SSH's separate encrypted length, body counter 1,
and Poly1305 over encrypted length plus body. The native implementation uses
RustCrypto's ChaCha20Legacy and Poly1305 primitives, and verifies the complete
tag in constant time before writing plaintext. It matches the independent
[SSH specification's worked example](https://www.ietf.org/archive/id/draft-ietf-sshm-chacha20-poly1305-04.html#appendix-A).
This is a distinct operation from the IETF AEAD hook, which remains available.

Dropbear retains its original cipher state and software implementation. Its
packet-length lookup stays in software, avoiding an ioctl for four bytes.
Missing/inaccessible device, unsupported operation, oversized packet or
allocation failure falls back to software; authentication and unexpected
interface errors fail the packet. Authentication failure never triggers a
fallback retry. `RVEMU_SSH_OFFLOAD=0` selects software explicitly; the harness
sets server and client environments separately for interoperability checks.

Validation includes malformed ioctl commands/pointers/lengths and failed
packet authentication without modifying the userspace destination, the
independent wire vector and every-byte ciphertext/tag tampering, synthetic
SSH packets in translator/interpreter lockstep, and the existing translator
regression. Both mixed-endpoint configurations and automatic firmware-absent
fallback have passed real SSH tests. Native SSH plus the 1 MiB streams also
passes with 8 MiB and flash-backed kernel text. That uses a small test rootfs,
not the full disk image with every application installed.

### Connection-only benchmark

`--test connection` takes emulator snapshots immediately before Dropbear
calls `connect_remote()` and when the client accepts authentication success.
It excludes boot, persistent host/client key generation, client startup
before the connect call, remote command execution, file transfers and
teardown. Ephemeral key generation during the handshake is included. Both
client and server run in the guest, so the measured cost includes both ends.
A subsequent `true` command verifies the connection but runs after the
measurement endpoint.

Four checkpoints record connection initiation, TCP completion notification,
initial NEWKEYS processing, and authentication success. Dropbear starts
speculative key-exchange computation before processing asynchronous TCP
completion, so the report deliberately groups setup and key exchange; it
does **not** label the first interval as pure network/TCP cost.

The independent, root-only `/dev/rvemu-bench` device forwards tags through
SBI extension `0x0a000002`. Host and QEMU runners snapshot guest instructions,
native-operation counters and idle ticks at that boundary; QEMU also reads
the native host instruction counter. Snapshots are printed only after the
run, outside the measured region. The service is available even without
`pv-buffers`, allowing software-only firmware to use identical boundaries.
Other firmware buses do not advertise this measurement service.

Use `connection_samples` and `connection_medians` in the JSON. Boot/session
totals are separated under `whole_run_diagnostics`. The parser requires all
four ordered boundaries, monotonic counters, the negotiated KEX name, and
native seal/open and the expected handshake primitives inside the measured
window when offload is selected (with endpoint-specific expectations for
mixed client/server tests).
Missing markers or a stalled connection fail the benchmark. Host-run wall
times are diagnostic; QEMU host instruction deltas include native crypto,
kernel bridge costs and translator execution, but are not RP2350 cycle times.

The default is one connection per fresh boot. `--repeats N` supports up to
eight within one boot, but later samples retain translator-cache state;
their median must not be interpreted as a clean crypto-only speed comparison.

Measured first-connection results (32 MiB, identical firmware/rootfs, runtime
offload toggle, negotiated `sntrup761x25519-sha512`, Ed25519 authentication):

| Measured interval | Guest software host instructions | Native engine host instructions |
|---|---:|---:|
| Setup and key exchange | 3,047,195,599 | 362,422,399 |
| Authentication | 721,723,839 | 21,516,424 |
| Connection total | 3,768,919,438 | 383,938,823 |

The general-purpose handshake primitives reduce the connection's total host
instruction count by **89.8% (9.82× fewer instructions)** in this paired run.
Guest instructions fall from 650,199,005 to 14,258,409. Both connections have
zero simulated idle ticks inside the measured window. Native counters prove
four X25519 operations, two Ed25519 signatures, two verifications, one
sntrup761 keypair, one encapsulation and one decapsulation, in addition to
nine packet seals and nine opens. Persistent Ed25519 key generation occurs
outside the connection window.

This measures the combined benefit of native execution and different native
library implementations; it is not a pure translator-overhead comparison.
The original packet-only experiment did not improve connection setup because
its key exchange and signatures remained guest code. Logs/results use the
`connection-r1` suffix; no network algorithm has been downgraded to obtain
these savings.

The connection test also passes with firmware lacking the native extension
and with an 8 MiB XIP-modeled guest. Boundary ABI tests cover unsupported
hosts/functions, invalid tags, bounded recording and userspace privilege;
parser tests reject missing/duplicate/reversed markers and incorrect crypto
path counters (`python3 scripts/test-ssh-measurements.py` from `rvemu-rs/`).

### Event-loop regression found by the test

The original native streaming run passed its byte checks but spent almost
eight simulated hours idle. A guest PC sample showed timer handling rather
than cipher work, and a temporary Dropbear timeout diagnostic localized the
stall to connection shutdown. `channelio()` could drain the last buffered
output and remove the final client channel **after** `cli_sessionloop()` had
checked for completion. The following `select()` then waited until the
eight-hour rekey deadline before checking completion again.

The source patch now invokes the existing client loop handler immediately
when `channelio()` removes the last channel. This fix applies equally to
native and software modes. The native test's idle time drops from about
28,511 seconds to one second. No keepalive or shortened rekey timeout masks
the problem. The harness's idle-time bound guards against its recurrence.

Native-call counters count attempts, including deliberate boundary-test
errors outside the connection measurement window. The full integration test
continues to exercise transfers, failed authentication and shutdown behavior;
its boot/session totals are not used as connection measurements.

### Native memory and stack budget

Firmware uses `rvaccel::Workspace::run`, with exclusively owned reusable
storage per emulated CPU. The RV32 workspace is **53,248 bytes (52 KiB)**:
28,620 bytes of KEM scratch, a 24 KiB execution stack, counters and alignment.
Large scalar reciprocal, polynomial-product, sampling and encoding arrays
live in scratch. Separate operation frames prevent small crypto calls from
inheriting larger operations' local buffers. LZ4's 8 KiB table and remaining
library temporaries use the dedicated stack. No allocation occurs per call.
The hosted convenience `rvaccel::run` still allocates scratch on its caller's
stack; small-stack firmware must use the workspace entry point.

The engine runs each operation on the workspace stack as a coroutine of the
emulator: a switch saves the callee-saved registers on one stack and resumes
the other. A preemptible operation runs with machine interrupts enabled;
core0's timer interrupt is taken on the operation's own stack, and its
handler switches back to the emulator, which returns the pause to the guest.
Resuming switches back into the handler, which returns into the operation.
Between operations the execution stack holds a paint pattern; after each call the first word that differs from it is the
call's exact high-water mark, and the stack is repainted from there up, which
erases everything the call left on it. KEM scratch is erased by the KEM
operations. Every operation records its stack watermark and checks a 256-byte
low-water guard, after every slice and when it finishes. That guard detects budget violations;
it is not a hardware barrier and does not prevent an overflow. The regression
suite requires every tested operation to stay below 20 KiB, leaving at least
4 KiB of the 24 KiB allocation unused.

Release RV32 measurements from `scripts/native-crypto-eval.py` (bytes,
including dispatch/callback frames, repeated calls and rejection paths):

| Operation | Native execution stack high-water |
|---|---:|
| LZ4 decode / encode | 668 / 9,020 |
| IETF ChaCha20-Poly1305 seal / open | 1,420 / 1,420 |
| SSH packet seal / open | 972 / 972 |
| X25519 | 3,484 |
| Ed25519 public / sign / verify | 5,020 / 5,020 / 8,460 |
| sntrup761 keypair / encapsulate / decapsulate | 14,764 / 11,996 / 17,740 |

On the board a paused operation's stack also holds the timer interrupt's
frame; the board's largest peak is 17,888 bytes (decapsulation). These are
observed high-water marks for this compiler/profile and test corpus, not a
formal bound for all builds or inputs.

QEMU reserves independent workspaces for the translator and reference CPU.
The Pico **shell** feature `pv-buffers` reserves the top **64 KiB of PSRAM**,
outside both guest RAM and the optional translator backing store; a compile-time
size check and guest-header bounds check enforce the reservation. Native calls
use only a small wrapper on core0's normal SRAM stack (64 bytes plus the
16-byte trampoline frame in the inspected release build). Other bare-metal
frontends that do not provision a workspace report the extension unavailable.
The feature remains off by default.

Build without flashing:

```sh
cd rvemu-rs
cargo build --release -p rvemu-pico --bin rvemu-pico-shell \
  --target riscv32imac-unknown-none-elf --features pv-buffers
python3 scripts/native-crypto-eval.py
```

`scripts/pico-shell.sh -C` enables the feature on the board and limits the
guest to 7 MiB. It loads whatever guest it is given; the kernel adapters and
Dropbear are in the board crypto guest below.

### On the board

`tests/build-board-crypto.sh` builds the board crypto guest into
`guest/out-crypto`: the disk guest's kernel with `rvemu-crypto`, `rvemu-ssh`
and `rvemu-bench`, and the disk guest's squashfs plus Dropbear and the test
tools (`crypto-vectors`, `crypto-ioctl-test`, `ssh-ioctl-test`,
`ssh-board-test`, `rvemu-bench-report`). It needs the disk guest's build
(`./run-build.sh disk`). From the repository root:

```sh
mkdir -p guest/out-crypto
curl -fsSL -o guest/out-crypto/dropbear.tar.bz2 \
  https://matt.ucc.asn.au/dropbear/releases/dropbear-2026.94.tar.bz2
python3 rvemu-rs/scripts/board-crypto-vectors.py guest/out-crypto/crypto-vectors.bin
docker volume create rvemu-board-crypto-work
docker run --rm --user 0 -v rvemu-guest-work:/work:ro -v rvemu-board-crypto-work:/eval \
  -v "$PWD:/repo:ro" -v "$PWD/guest/out-crypto:/out" rvemu-guest \
  bash /repo/guest/tests/build-board-crypto.sh
```

`rvemu-rs/scripts/pico-crypto-test.py` loads it with
`pico-shell.sh -C -d ../guest/out-crypto`, logs in on the USB console and
runs the tests on the board. Close any terminal on the console port first.
Results and the console transcript go to `rvemu-rs/target/pico-crypto/`.

| Command | Runs on the board |
|---|---|
| `pico-crypto-test.py vectors` | the ABI tests, every vector three times on one descriptor, and each operation's cycle cost |
| `pico-crypto-test.py ssh` | `ssh-board-test integration` with both endpoints native, both software, and each one alone |
| `pico-crypto-test.py connection -n 3` | one SSH connection per fresh boot, alternately native and software, timed from connect to authentication |
| `pico-crypto-test.py responsive` | ping over Wi-Fi and console round trips while native operations run in a loop |
| `pico-crypto-test.py remote` | the Mac's OpenSSH logging in to Dropbear on the board over Wi-Fi, native and software |

`ssh-board-test` keeps its keys and authorized_keys in `/run` and writes
nothing to flash. On the board, `rvemu-bench-report` prints the benchmark
points (core0 cycles, instructions, XIP cache counters), the native call
counts, the native stack peaks, whether the native stack is erased, and
both cores' stack depth on the console.
