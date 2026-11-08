# rp2350-linux

Linux 6.12 (rv32ima, Sv32 MMU) on an RP2350B board. The kernel runs in an
emulated hart, and the emulator runs on one of the chip's own Hazard3 RISC-V
cores; the other does the console and network I/O. The guest gets the board's 8 MB PSRAM as RAM, plus its kernel text read in
place from flash. Its root filesystem is a squashfs in flash. Its console is
the board's USB serial port, and its Ethernet is the RM2 (CYW43439) Wi-Fi
module. Hot guest code goes through a block translator into SRAM, so most of
it runs as native Hazard3 code.

```
rp2350 login: root
# uname -a
Linux rp2350 6.12.116 #3 Sun Nov  8 04:03:07 UTC 2026 riscv32 GNU/Linux
```

## Layout

| Path | What |
|---|---|
| `rvemu-rs/rvcore` | `no_std` CPU: interpreter, soft TLB and Sv32 walk, built-in SBI, UART, PLIC, virtio-mmio block and network devices |
| `rvemu-rs/rvjit` | `no_std` block translator, RV32IMA guest to RV32 host code |
| `rvemu-rs/rvemu` | Mac host: interactive or batch boots, device tree generation, statistics, timing model, cache-policy simulator |
| `rvemu-rs/rvemu-virt` | the same core bare-metal under `qemu-system-riscv32 -M virt` (interpreter, translator, lockstep) |
| `rvemu-rs/rvemu-pico` | board firmwares: `rvemu-pico-shell` (interactive, USB console, Wi-Fi; the I/O on the second core), `rvemu-pico-linux` (boot benchmark over RTT), `rvemu-pico` (jittest) |
| `rvemu-rs/jittest-guest` | bare-metal test guest for the translator |
| `rvemu-rs/scripts` | build, load and compare scripts (`pico-shell.sh`, `pico-wifi.sh`, `pico-linux.sh`, `virt.sh`, ...) |
| `rp2350/` | board crate (clocks, PSRAM, BOOTSEL), PSRAM bring-up tests, host tools (`pack-guest.py`, `board-cmd.py`, RTT, profiler) |
| `guest/` | Docker + Buildroot builds of the kernel and the two root filesystems |
| `rvemu/` | the original C interpreter, kept as the reference the Rust version is compared against |

Not in the repository: `guest/src/buildroot` (`guest/run-build.sh` clones
it), `guest/out*` (build outputs) and `ref/` (third-party checkouts such as
semu's prebuilt images, which `rvemu/` and `scripts/compare.sh` use by
default).

Each directory has its own README with the details: [rvemu-rs](rvemu-rs/README.md),
[rp2350](rp2350/README.md), [guest](guest/README.md), [rvemu](rvemu/README.md).

## Hardware

- An RP2350B board with 8 MB APS6404L PSRAM on QMI CS1 (GP0) and 16 MB of
  flash. The firmwares run the chip at 348 MHz and 1.20 V, with the PSRAM at
  116 MHz.
- An RM2 (CYW43439) Wi-Fi module on the Pico 2 W pins: GP23 WL_ON, GP24
  data/IRQ, GP25 CS, GP29 CLK. Optional: without it the guest's `eth0` stays
  down.
- The target's USB to the Mac, for loading (BOOTSEL + picotool) and the
  console.
- Optional: an SWD probe (a second RP2350 running debugprobe) for RTT and
  for recovery.

## Prerequisites (macOS)

- Rust stable with the `riscv32imac-unknown-none-elf` and
  `riscv32im-unknown-none-elf` targets.
- Docker, for the guest build.
- `dtc`, `picotool` and `screen`.
- probe-rs from master, in `~/git/toolchains/probe-rs-master` (override with
  `PROBE_RS=`). Only needed for RTT, for `pico-linux.sh`, and as a fallback
  when the board can't be put into BOOTSEL over USB. See
  [rp2350/README.md](rp2350/README.md).
- `qemu-system-riscv32`, for `scripts/virt.sh`.

## Quickstart

```sh
# 1. Build the guest: kernel + squashfs root into guest/out-disk (Docker; the
#    first Buildroot build takes a while). `./run-build.sh all` builds the
#    small initramfs guest in guest/out.
cd guest && ./run-build.sh disk && cd ..

# 2. Optional: try it on the Mac first (Ctrl-A x quits).
cd rvemu-rs && cargo build --release
./target/release/rvemu -k ../guest/out-disk/Image -d ../guest/out-disk/rootfs.squashfs -m 8

# 3. Take the board (see "Sharing the board" below); every script that
#    touches it checks the lock.
export BOARD_LOCK_OWNER=$USER
../rp2350/tools/board-lock acquire $USER -l 120

# 4. Store the Wi-Fi network in the board's flash (asks for the passphrase).
scripts/pico-wifi.sh "My Network"

# 5. Load the guest and rvemu-pico-shell over USB (only what changed) and boot.
#    -b rebuilds the guest first, -i uses the initramfs guest instead.
scripts/pico-shell.sh

# 6. Connect. Log in as root, no password. Ctrl-A k quits screen.
screen /dev/cu.usbmodemrvemu1 115200

# Or run one command without a terminal (screen must be closed):
../rp2350/tools/board-cmd.py 'free; ifconfig eth0'
```

`pico-shell.sh` gets the board into BOOTSEL with a 1200-baud touch on its
console port, or failing that over SWD. It refuses while another program
(screen, say) has the port open.

## Sharing the board

There is one board and one guest-build Docker volume, and several checkouts
can use them, so both sit behind `rp2350/tools/board-lock` (state in
`~/.cache/rp2350-board`, outside every checkout):

```sh
rp2350/tools/board-lock status                    # who holds what, who waits
rp2350/tools/board-lock run NAME -- CMD...        # wait (FIFO), run, release
rp2350/tools/board-lock acquire NAME [-l MIN] [-w MIN]   # hold across commands
rp2350/tools/board-lock renew NAME; rp2350/tools/board-lock release NAME
```

- The board lock covers BOOTSEL/picotool, the console port and SWD.
  `pico-*.sh`, `run-riscv.sh` and `board-cmd.py` refuse to run unless
  `BOARD_LOCK_OWNER` holds it (`board-lock run` sets it for its command).
- A lease (20 min by default) that isn't renewed expires, and the next
  waiter gets the lock.
- `guest/run-build.sh` takes the `guest-build` lock itself for the Docker
  run, and refuses to start while you hold the board. When the
  configuration differs from the last one built in the volume, it reinstalls
  every package into a fresh `target/`, so an image never holds another
  checkout's packages.
- What each region of flash holds is recorded in
  `~/.cache/rp2350-board/stamps`, so a checkout reloads a region another
  checkout overwrote.

`scripts/perf-gate.sh` is the on-device performance and stability gate:
- the benchmark boot;
- the disk guest's wall time to `login:`;
- a fixed workload: first runs, MicroPython, Lua, fork+exec;
- MemAvailable;
- checksums of every file in the root filesystem against the image's own.

It compares the results with `~/.cache/rp2350-board/perf-baseline.json` and
fails on any slowdown over 3% (boot) or 5% (workload), on 128 KiB less
MemAvailable, or on a checksum mismatch.

## Flash map (16 MB)

| Address | Contents | Written by |
|---|---|---|
| 0x10000000 | firmware (`rvemu-pico-shell` or `rvemu-pico-linux`) | `picotool load -x` |
| 0x103FF000 | Wi-Fi settings, 4 KB (`"WIFI"`, lengths, SSID, passphrase) | `scripts/pico-wifi.sh` |
| 0x10400000 | guest blob, at most 2 MB: `RVLY` header, the kernel after its text, initramfs, DTB | `pack-guest.py` via `pico-common.sh` |
| 0x10600000 | root filesystem (squashfs) | `pico-common.sh` `load_disk` |
| 0x11000000 − text_len | kernel text, ending exactly at the end of flash | `pico-common.sh` `load_text` |

Flash ends at 0x11000000, which is exactly where the PSRAM window starts. With
the kernel's `.text` in the last bytes of flash, guest RAM is one contiguous
block of host addresses: read-only kernel text from flash, then 8 MiB of
PSRAM. The guest sees 11.1 MB of RAM. Each load script keeps a hash of what
it wrote under `rvemu-rs/target/` and skips writes when nothing changed
(`FORCE_BLOB=1` forces them).

## Results (on the board, 348 MHz)

| | |
|---|---|
| Boot to `login:`, disk guest (squashfs root, shared musl, Wi-Fi) | 19.7 s |
| Boot to `login:`, initramfs guest (static BusyBox) | 8.0 s |
| `vi` opens | 0.4 s |
| Guest RAM (2.9 MB flash-backed kernel text + 8 MiB PSRAM) | 11,100 KiB |
| Translator vs the interpreter, jittest | 36.8 vs 1.90 MIPS (19.4x) |
| QEMU lockstep, disk guest (translator vs interpreter, every event) | 2,517,290 events matched |

Every board run of the benchmark firmware matches the Mac's instruction
count and console bytes exactly. The emulator is deterministic: guest time
is retired instructions plus skipped idle time.
