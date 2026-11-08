# guest

The Linux guests for rvemu, built in Docker:

    ./run-build.sh [all|rootfs|kernel|disk|disk-rootfs|disk-kernel]

The first run clones Buildroot 2026.08.1 into `src/buildroot`. Build trees
live in the Docker volume `rvemu-guest-work`, so later builds are
incremental. Logs and outputs are not tracked.

There are two guests. Both use the same rv32ima / ilp32 CPU (no F, D or C),
musl, Linux 6.12.116 built with the Buildroot toolchain from `tinyconfig` plus
`linux.fragment`, and BusyBox init with a getty on ttyS0. Log in as root, no
password.

| | initramfs guest (`out/`) | disk guest (`out-disk/`) |
|---|---|---|
| Stages | `all`, `rootfs`, `kernel` | `disk`, `disk-rootfs`, `disk-kernel` |
| Configs | `buildroot.defconfig`, `post-build.sh`, `linux.fragment` | `buildroot-disk.defconfig`, `post-build-disk.sh`, `linux.fragment` + `linux-disk.fragment` |
| Root | `rootfs.cpio` initramfs | `rootfs.squashfs` on a read-only virtio-blk disk, tmpfs overlay; `/home` and `/root` on jffs2 in flash |
| Userland | static BusyBox | shared musl; BusyBox, nano, mg, MicroPython, Lua 5.4, htop, tmux, lrzsz, bc, ncdu, lsof, less |
| Kernel | no networking, no block layer | + virtio-mmio blk and net, squashfs, overlayfs, sockets, MTD + jffs2 |
| Used by | the boot benchmark (`scripts/pico-linux.sh`), `pico-shell.sh -i` | `scripts/pico-shell.sh` (default), `rvemu -d`, `virt.sh -D` |

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
  reboot, and the squashfs stays visible at `/rom`. The
  Mac host's and the firmware's default command line for a disk without an
  initramfs is
  `root=/dev/vda ro rootfstype=squashfs init=/sbin/overlay-init`.
- **`/home`:** jffs2 on the board's 1 MB flash region (the `rvemu,flash`
  device, `mtd0`), mounted `noatime` by overlay-init. `/root` is a bind
  mount of `/home/root`, created from the squashfs's `/root` the first time.
  - jffs2 is log-structured and checksums every node, so a power cut or
    reset loses at most the write in flight. A `write()` is on flash when it
    returns (no write buffer).
  - It formats a blank region itself, and erases flash it can't parse, in
    the background.
  - Its GC thread (`jffs2_gcd_mtd0`) is SCHED_IDLE (build.sh edits
    `fs/jffs2/background.c`): after a mount it CRC-checks every node, and
    that waits for idle time instead of slowing the boot.
  - No compression beyond jffs2's rtime: zlib's deflate workspace is about
    270 KiB of vmalloc.
  - The driver, `linux/rvemu-flash.c`, is copied into the kernel tree by
    build.sh. `/sys/bus/platform/devices/f8000400.flash/stats` reads the
    device's counters: erases, erases skipped (already blank), pages
    programmed, longest stall (us), total stall (ms).
- **`/home` on the board (348 MHz):**

  | | |
  |---|---|
  | Boot to `login:`, steady state | 26.95 s (no `/home`: 26.64 s) |
  | First boot on blank flash / on garbage (formats) | 27.6 s / 35.4 s, once |
  | Guest RAM (perf-gate `mem_adj_kb`) | 40 KiB less than without `/home` |
  | Writing 256 KB in large writes / in 256-byte writes | about 260 KB/s / 13 KB/s |
  | Longest guest stall: page program / sector erase | 0.5 ms / 51 ms |
  | Wear, one day of typical use (300 shell commands, 20 saves of a 5 KB file, 200 log lines, five 50 KB rewrites) | 36 sector erases at 14% full, 252 at 72% full: 0.14 to 1 erase per sector per day against 100K cycles |
- **Network:** `eth0` is virtio-net. `/etc/network/interfaces` runs DHCP in
  the background (`udhcpc -b`), so boot doesn't wait for the Wi-Fi join.
  On the board the NIC is bridged to the Wi-Fi radio. On the Mac, `rvemu -N`
  gives a stub LAN (see `rvemu-rs/README.md`).
- **`LANG=C.UTF-8`** in `/etc/profile`, for tmux and friends.
- **Not included:** vim (its 3.5 MB binary doesn't fit the 8 MiB guest's
  page cache; `vi` is BusyBox's) and strace (Buildroot has none for RV32).

Kernel options of note in `linux-disk.fragment`:

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

## Sizes and boot (Mac host, `-m 8`, kernel text as extra RAM, boot to `login:`)

| | initramfs guest | disk guest |
|---|---|---|
| Image (of which text) | 2.4 MB (1.6 MB) | 4.0 MB (3.0 MB) |
| Root filesystem | 1.2 MB cpio | 4.5 MB squashfs |
| Guest instructions to `login:` | 58.1 M | 147.5 M (with `-N`) |
| Boot on the board, 348 MHz | 8.0 s | 19.7 s |
