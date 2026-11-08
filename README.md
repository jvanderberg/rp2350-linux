# rp2350-linux

Linux 6.12 (rv32ima, Sv32 MMU) on an RP2350B board. The RP2350's Hazard3
RISC-V cores have no MMU, so they can't run Linux themselves: one of them
runs an emulated RISC-V CPU that has one, and the kernel runs on that. The
other core does the console and network I/O. The guest gets
the board's 8 MB PSRAM as RAM, plus its kernel text read in place from
flash. Its root filesystem is a squashfs in flash, and `/home` is a jffs2
filesystem in flash. Its console is the board's USB serial port, and its
Ethernet is the RM2 (CYW43439) Wi-Fi module. Hot guest code goes through a
block translator into SRAM, so most of it runs as native Hazard3 code.

```
rp2350 login: root
# uname -a
Linux rp2350 6.12.116 #29 Sun Nov  8 12:56:58 UTC 2026 riscv32 GNU/Linux
# free
              total        used        free      shared  buff/cache   available
Mem:           7004        1948        3636          12        1420        4020
```

Boot to `login:` takes 10.9 s on the board.

## Features

| Feature | On by default? | Turn it on / use it | Main cost | Section |
|---|---|---|---|---|
| Fast boot: static BusyBox, short init, DHCP in the background, TRNG seed, translator speedups | yes | nothing to do | +128 KiB of flash | [Boot time](#boot-time) |
| I/O on core1: USB console, Wi-Fi radio, the radio side of virtio-net | yes | nothing to do | 58 KiB of SRAM the code cache could otherwise use (about 1.5 s on a cold `md5sum` pass) | [Two cores](#two-cores-io-on-core1) |
| `/home` (and `/root`) on flash, jffs2, 1 MB | yes | nothing to do | 40 KiB of guest RAM | [/home on flash](#home-on-flash) |
| C compiler: TinyCC as `cc` | yes (disk guest) | `cc hello.c`, `tcc -run`, `tcc -ar` | 704 KiB of squashfs | [C compiler](#c-compiler) |
| Swap on flash, 1788 KiB | no | `scripts/pico-shell.sh -w` | about 32 KiB of guest RAM and 0.5 s of boot | [Swap](#swap-on-flash) |
| Pins: GPIO, I2C, SPI, PWM, ADC | no | `PINS=1 guest/run-build.sh disk` | +112 KiB of kernel text, about 72 KiB of RAM, file reads 5-10% slower | [Pins](#pins) |

All numbers are measured on the board at 348 MHz. Every example below was run on the board: lines starting with `#`
are in the board's shell, lines starting with `$` on the Mac it is plugged
into.

## Contents

- [Layout](#layout), [Hardware](#hardware), [Prerequisites](#prerequisites-macos), [Quickstart](#quickstart)
- Features: [Boot time](#boot-time), [Two cores](#two-cores-io-on-core1),
  [/home on flash](#home-on-flash), [C compiler](#c-compiler),
  [Swap](#swap-on-flash), [Pins](#pins)
- [Flash map](#flash-map-16-mb), [Sharing the board](#sharing-the-board),
  [Performance gate](#performance-gate), [Results](#results-on-the-board-348-mhz)

## Layout

| Path | What |
|---|---|
| `rvemu-rs/` | the emulator and block translator that run Linux on the board, and the board firmwares (`rvemu-rs/rvemu-pico`: `rvemu-pico-shell`, the one you use; `rvemu-pico-linux`, the boot benchmark; `rvemu-pico`, jittest), plus the build, load and test scripts in `rvemu-rs/scripts` (`pico-shell.sh`, `pico-wifi.sh`, `perf-gate.sh`, ...). |
| `rp2350/` | board crate (clocks, PSRAM, BOOTSEL), PSRAM bring-up tests, host tools (`board-lock`, `board-cmd.py`, `boot-wait.py`, `pack-guest.py`, RTT, profiler) |
| `guest/` | Docker + Buildroot builds of the kernel and the two root filesystems, the guest's own kernel drivers (`guest/linux`) and packages (`guest/br-external`) |
| `rvemu/` | the original C interpreter, kept as the reference the Rust version is compared against |

Not in the repository: `guest/src/buildroot` (`guest/run-build.sh` clones
it), `guest/out*` (build outputs) and `ref/` (third-party checkouts such as
semu's prebuilt images, which `rvemu/` and `scripts/compare.sh` use by
default).

Each directory has its own README with the details: [rvemu-rs](rvemu-rs/README.md)
(the emulator, the devices and the firmware), [guest](guest/README.md) (what
is in the guests and how they are built), [rp2350](rp2350/README.md),
[rvemu](rvemu/README.md).

## Hardware

- An RP2350B board with 8 MB APS6404L PSRAM on QMI CS1 (GP0) and 16 MB of
  flash. The firmwares run the chip at 348 MHz and 1.20 V, with the PSRAM at
  116 MHz.
- An RM2 (CYW43439) Wi-Fi module on the Pico 2 W pins: GP23 WL_ON, GP24
  data/IRQ, GP25 CS, GP29 CLK. Optional: without it the guest's `eth0` stays
  down.
- The target's USB to the Mac, for loading (BOOTSEL + picotool) and the
  console.
- The free GPIOs, both I2C buses, SPI0, PWM and the ADC are the guest's,
  in a `PINS=1` build (see [Pins](#pins)).
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

## Quickstart

```sh
# Every script that touches the board or the guest-build volume takes its
# lock in your name (see "Sharing the board").
export BOARD_LOCK_OWNER=$USER

# 1. Build the guest: kernel + squashfs root into guest/out-disk (Docker; the
#    first Buildroot build takes a while). `./run-build.sh all` builds the
#    small initramfs guest in guest/out.
cd guest && ./run-build.sh disk && cd ..

# 2. Take the board.
cd rvemu-rs
../rp2350/tools/board-lock acquire $USER -l 120

# 3. Store the Wi-Fi network in the board's flash (asks for the passphrase).
scripts/pico-wifi.sh "My Network"

# 4. Load the guest and rvemu-pico-shell over USB (only what changed) and boot.
#    -b rebuilds the guest first, -i uses the initramfs guest instead, -w adds swap.
scripts/pico-shell.sh

# 5. Connect. Log in as root, no password. Ctrl-A k quits screen.
screen /dev/cu.usbmodemrvemu1 115200

# Or run one command without a terminal (screen must be closed):
../rp2350/tools/board-cmd.py 'free; ifconfig eth0'

# 6. Done: let the next user have the board.
../rp2350/tools/board-lock release $USER
```

`pico-shell.sh` gets the board into BOOTSEL with a 1200-baud touch on its
console port, or failing that over SWD. It refuses while another program
(screen, say) has the port open.

## Boot time

The disk guest boots to `login:` in about **10.9 s**. Nothing needs
turning on. What keeps it short:

- **Static BusyBox** (`guest/busybox-disk.fragment`). Every init script and
  every shell command is a fork+exec, and a static BusyBox skips musl's
  dynamic loader each time (about 0.4 M guest instructions per exec).
  The other programs link against the shared musl.
- **Short init.** The only init scripts are `S02sysctl`, `S02swap` and
  `S40network`. There is no syslogd, klogd, crond, seedrng or modules
  script (`dmesg` has the kernel log), and the `/dev/fd` and `/dev/std*`
  symlinks are one `cp -a` in inittab.
- **DHCP in the background.** `eth0` runs `udhcpc -b`, so boot doesn't
  wait for the Wi-Fi join or the lease.
- **rng-seed.** The device tree's `/chosen` carries 32 bytes of `rng-seed`,
  which `rvemu-pico-shell` fills from the RP2350's TRNG before the guest
  starts, so the kernel's CRNG is ready at once.
- **Translator.** LR/SC, `sstatus` writes and TLB refills stay in
  translated code, block descriptors live in the code buffer, and a
  jump-cache miss walks the hash chain itself
  ([rvemu-rs/README.md](rvemu-rs/README.md#translator-rvjit)).

What the performance gate measures:

| | |
|---|---|
| Boot to `login:`, disk guest | 10.9 s |
| Benchmark boot, initramfs guest | 5.2 s |
| First runs of htop, tmux, MicroPython, Lua and nano | 6.6 s |
| 200 fork+exec of `/bin/true` | 18.2 s |
| MicroPython, sum of 30,000 squares | 2.96 s |
| md5sum of 58 files, cold / warm | 15.1 / 14.3 s |
| MemAvailable after boot | about 4,000 KiB |

**Costs and limits.** The static BusyBox takes 128 KiB more flash than a
shared one. Boot doesn't touch libc, so the first dynamic program after
boot pages it in.

**Example:** boot.

```
$ scripts/pico-shell.sh
pico-shell.sh: Linux is booting; console: screen /dev/cu.usbmodemrvemu1 115200
$ ../rp2350/tools/boot-wait.py "$(cat ~/.cache/rp2350-board/last-launch)"
10.91
# dmesg | grep -i "crng init"
[    0.000000] random: crng init done
# ps | grep -c "[u]dhcpc -b"
1
# ls /etc/init.d
S02swap     S02sysctl   S40network  rcK         rcS
```

## Two cores: I/O on core1

`rvemu-pico-shell` runs only the guest on core0. core1 runs the USB
console, the Wi-Fi driver and the radio side of the guest's virtio-net, and
sleeps in `wfi` between interrupts. The cores talk through lock-free rings
in SRAM. Nothing needs turning on.

On the board:

| | |
|---|---|
| Typed-key echo, round trip | median 1.23 ms (min 1.08, max 5.72, n = 40) |
| `ping` to the Wi-Fi gateway | avg 1.05 ms (0.92-1.31), 0% loss |
| Console output (`seq`) | 7.3 KiB/s, exact |
| `wget` 1 MiB from a server on the LAN | 0.99 s, md5 correct |

**Costs and limits.**
- SRAM: core1's code (about 56 KB, in `.core1_text`), its 7 KiB service
  slot, 5 KiB stack and 16 KiB console ring come out of the translator's
  SRAM the translator's code cache would otherwise have: the cache is
  326 KiB (`CODE_KIB`). 58 KiB more cache would make a cold `md5sum` pass
  about 1.5 s faster.
- Everything core1 runs must be in SRAM, because both cores share the QMI
  and the XIP cache with guest RAM (see the rvemu-rs README for how that's
  checked).
- A flash write (`/home`, swap) parks core1 for the length of each erase
  or program.
- core0 rings core1 on console output and network transmit. Each way has
  only 2 frame slots: a frame the guest sends while core1 is two behind is
  dropped (TCP resends it).

**Example:** the network through the radio.

```
# ifconfig eth0 | grep "inet "
          inet addr:192.168.4.171  Bcast:192.168.7.255  Mask:255.255.252.0
# ping -c 10 192.168.4.1 | tail -2
10 packets transmitted, 10 packets received, 0% packet loss
round-trip min/avg/max = 0.915/1.054/1.311 ms
# cd /tmp && time wget -q -O 1m.bin http://192.168.4.47:8771/1m.bin; md5sum 1m.bin
real	0m 0.99s
5c2b2733524d557484177d013844e229  1m.bin
```

Details: [rvemu-rs/README.md](rvemu-rs/README.md#core1-rvemu-picosrccore1rs).

## /home on flash

The disk guest's `/home` is a jffs2 filesystem on the 1 MB flash region at
0x10100000, and `/root` is a bind mount of `/home/root`. Files there
survive reboots, reloads with `pico-shell.sh` and power cuts. Everything
else outside `/home` is a tmpfs overlay and is gone at reboot.

- **Always on.** A blank region formats itself on the first boot. If the
  region holds something that isn't jffs2, `overlay-init` formats it.
- **Power cuts.** jffs2 checksums every node, so a cut loses at most the
  write in flight. A reset in the middle of a burst of writes and deletes
  leaves a clean mount, the older files intact and the burst's files
  present up to the reset.
- **Counters:** `/sys/bus/platform/devices/f8000400.flash/stats` reads
  erases, erases skipped (already blank), pages programmed, the longest
  stall (us) and the total stall (ms).

**Costs and limits:**

| | |
|---|---|
| Guest RAM | 40 KiB |
| Boot | +0.3 s; the first boot on blank flash +1 s, on garbage +8.7 s (it erases what it can't parse) |
| Size | 1 MB; writes fail with ENOSPC at about 98% full |
| Writes | about 260 KB/s in large writes, 13 KB/s in 256-byte writes (one jffs2 node each) |
| Guest stalls | at most 0.5 ms per page program, 47-51 ms per sector erase |
| Wear | 0.14 to 1 erase per sector per day of typical use, against 100K cycles |

BusyBox reports any write error on standard output, ENOSPC included, as
"I/O error". A full `/home` (or `/tmp`) shows up as `head: standard
output: I/O error`, not "No space left on device".

**Example** (board): a file survives a reboot.

```
# head -c 65536 /dev/urandom > /home/docs-test.bin; md5sum /home/docs-test.bin > /home/docs-test.md5; sync
$ scripts/pico-shell.sh          # reboot the board (boot to login: 10.95 s)
# md5sum -c /home/docs-test.md5
/home/docs-test.bin: OK
```

Details: [guest/README.md](guest/README.md#home-on-flash) (the filesystem),
[rvemu-rs/README.md](rvemu-rs/README.md#flash-writes-rvemu-picosrcflashrs)
(how the firmware writes flash).

## C compiler

The disk guest has TinyCC as `cc` and `tcc`, with musl's headers and the
kernel's device headers. It compiles, assembles and links on the board.

- `cc hello.c && ./a.out`, `cc -c`, `cc -o prog a.c b.c`
- `tcc -run file.c ARGS`, or `#!/usr/bin/tcc -run` as a script's first line
- `tcc -ar rcs libfoo.a a.o b.o` for static libraries

**Costs and limits.**
- 704 KiB of squashfs; no RAM until it runs.
- A compile takes 0.9-2.8 MB, against about 4 MB available after boot.
  After every 128 kB it allocates, tcc checks MemAvailable, and below
  512 kB it stops with "memory full", so it can't stall the guest by
  evicting the page cache. Build larger programs a file at a time
  (`cc -c`).
- Dynamic linking only: there is no `libc.a`, so no `-static`.
- `long double` is computed in double precision.
- There is no `make` in the guest.

**Example:**

```
# printf '#include <stdio.h>\nint main(void) { printf("hello from tcc\\n"); return 0; }\n' > hello.c
# time cc hello.c && ./a.out
real	0m 1.19s
hello from tcc
# time tcc -run hello.c
hello from tcc
real	0m 0.20s
```

**Example:** a static library from two files.

```
# printf 'int sq(int x) { return x * x; }\n' > sq.c
# printf 'int cube(int x) { return x * x * x; }\n' > cube.c
# printf '#include <stdio.h>\nint sq(int), cube(int);\nint main(void) { printf("%%d %%d\\n", sq(7), cube(3)); return 0; }\n' > main.c
# cc -c sq.c cube.c && tcc -ar rcs libm2.a sq.o cube.o && tcc -ar t libm2.a
sq.o
cube.o
# cc -o calc main.c -L. -lm2 && ./calc
49 27
```

**Example:** when memory runs short, tcc stops instead of stalling the
guest. Here `/tmp` (RAM) holds 3.4 MB, and the file has 3,000 one-line
functions.

```
# awk 'BEGIN{print "#include <stdio.h>"; for(i=0;i<3000;i++) printf "int f%d(int x) { return x * %d + %d; }\n", i, i, i; print "int main(void) { printf(\"%d\\n\", f2999(1)); return 0; }"}' > big.c
# head -c 3400000 /dev/zero > /tmp/fill; grep MemAvailable /proc/meminfo
MemAvailable:        820 kB
# cc -o big big.c
tcc: memory full (508 kB left for the system)
# rm /tmp/fill; cc -o big big.c && ./big
5998
```

Details: [guest/README.md](guest/README.md#c-compiler).

## Swap on flash

`scripts/pico-shell.sh -w` gives the guest a swap device, `/dev/vdb`, in
the flash region 0x10200000-0x103FEFFF, and the guest's init enables it:
1788 KiB of swap. The swap is empty at every boot. Without `-w` the guest
has no swap device and pays nothing. There is no zram or zswap: with about 4 MB
free, anything that keeps swapped pages in RAM takes it from the page
cache that holds program text, and the guest thrashes on squashfs reads
instead.

`flashswap-stats` in the guest prints the device's counters and flash
timings.

Workload: tmux with four panes and htop; Lua holding a 1.7 MB table
(idle); MicroPython building a 15,000-string list and a 5,000-entry dict
in a 1 MB heap; then a Lua loop:

| Swap | Finishes | Wall time | Echo latency p50 / p95 / max |
|---|---|---|---|
| none | no: hung, over 240 s | | 17 ms / 7.7 s / 44 s |
| flash (`-w`) | yes | 149 s | 24 / 161 / 688 ms |
| zram, lz4, 4 MB | no: hung, over 300 s | | 26 ms / 98 ms / 28 s |
| zswap in front of flash | no: hung, over 300 s | | 27 ms / 99 ms / 29 s |

- **More memory.** With tmux and htop running, a Lua table of 40,000
  strings (2.3 MB) builds with flash swap. Without swap, 20,000 (1.2 MB)
  is the most that builds.
- **Latency.** Typed-key echo while swapping: p50 21-26 ms, p95 150-177 ms,
  max about 0.8 s. While idle erases run: 15-39 ms. One request stalls the
  guest for at most one erase and its writes (about 50 ms).
- **Wear.** One erase per page written, plus 1 in 8 for wear levelling.
  Continuous thrashing (4.5 erases/s) would wear the region out in 131
  days; an hour of it a day, in about 9 years.

**Costs and limits.**
- About 32 KiB of guest RAM with `-w` (the device's slab and virtqueue,
  plus the reserved page that holds the swap map), and about 0.5 s of boot
  for mkswap and swapon. That's why it's opt-in.
- Wear levelling is in RAM only (a random start each boot, no erase
  counts kept), and Linux sends no discards, so freed slots are found only
  when they are overwritten.
- Under thrashing with no idle time the erases are synchronous, about
  52 ms per 4 KiB written.

**Example:**

```
$ scripts/pico-shell.sh -w
# cat /proc/swaps
Filename				Type		Size		Used		Priority
/dev/vdb                                partition	1788		0		-2
# cat /sys/block/vdb/serial
rvemu-flash-swap
# time lua -e 't={} for i=1,40000 do t[i]=string.rep("x",40)..i end print(#t)'; free
40000
real	0m 13.50s
              total        used        free      shared  buff/cache   available
Mem:           7000        2112        3016           0        1872        3864
Swap:          1788         116        1672
# flashswap-stats
pages 448
sectors 511
live 385
...
erases_sync 225
erases_idle 5
...
erase_us_max 48811
program_us_max 564
```

`flashswap-stats` counts 4 KiB pages and sectors. `live` sectors hold a
page the device still maps; Linux sends no discards, so that includes pages
it has since freed (here 385 live against 116 KiB in use). `erases_sync`
erases made a write wait, `erases_idle` ran while the guest idled, and the
`*_us` fields are flash times in microseconds.

Details: [rvemu-rs/README.md](rvemu-rs/README.md#flash-swap-rvcoresrcswapdevrs-rvemu-picosrcswap_flashrs).

## Pins

A `PINS=1` build of the disk guest gets the board's pins through the
standard Linux interfaces. It is opt-in because the kernel subsystems cost
RAM and kernel text even when no pin is in use:

```
cd guest && PINS=1 ./run-build.sh disk    # the default build has none of it
```

`scripts/pico-shell.sh` adds the pin devices to the guest's device tree by
itself when the kernel has their drivers (`CONFIG_RVEMU_GPIO=y` in
`guest/out-disk/linux.config`).

| Linux | Board |
|---|---|
| `/dev/gpiochip0` ("rp2350-gpio", lines 0-47 = GP0-GP47) | free pins GP1-GP3, GP8-GP15, GP20-GP22, GP26-GP28, GP30-GP39; pull-up/pull-down/none, rising/falling edge events |
| `/dev/i2c-0`, `/dev/i2c-1` | I2C0 on GP4 (SDA) / GP5 (SCL), I2C1 on GP6 / GP7; 100 kHz, internal pull-ups |
| `/dev/spidev0.0` | SPI0: GP16 MISO, GP17 CS, GP18 SCK, GP19 MOSI; 100 kHz to 50 MHz, modes 0-3, 8-bit |
| `/sys/class/pwm/pwmchip0` (channel N = GPN) | any free GPIO below 40; pins on one slice share its period |
| IIO `rp2350-adc` (`/sys/bus/iio/devices/iio:device0`) | `in_voltage0..7` = GP40-GP47 (mV = raw x `in_voltage_scale`), `in_temp` (the chip's sensor, m°C = (raw + offset) x scale) |

GP0 (the PSRAM's chip select) and GP23/24/25/29 (the radio) are not offered
to the guest; `gpioinfo` names every line. The pins build has:

- libgpiod 2 tools (`gpiodetect`, `gpioinfo`, `gpioget`, `gpioset`,
  `gpiomon`), i2c-tools (`i2cdetect`, `i2cget`, `i2cset`, `i2ctransfer`)
  and `spidev_test`;
- MicroPython's `machine` module with `Pin` (and `Pin.irq()`), `I2C`,
  `SPI`, `PWM` and `ADC` (`ADC(Pin(40))`, `ADC(ADC.CORE_TEMP).read_temp()`);
- C examples, built and as source, in `/usr/share/examples/pins`:
  `gpio-blink`, `gpio-edge`, `i2c-scan`, `spi-xfer`, `adc-read`. They
  compile on the board with `cc -o gpio-blink gpio-blink.c` (the
  directory's Makefile is for cross builds: the guest has no `make`).

**Costs and limits.**
- The `PINS=1` guest: kernel text +112 KiB (read from flash on the
  board), rootfs +256 KiB, about 72 KiB of guest RAM, and md5sum over the
  fixed file list 5% slower cold and 9.5% warm. The
  default guest pays none of it.
- The firmware, in either mode: +22 KB of flash and +1.2 KB of SRAM.
- I2C runs at 100 kHz only. A stuck bus fails the transfer with
  `ETIMEDOUT` after 5 ms per byte; it can't hang the guest.
- SPI's clock has a 100 kHz floor, so the longest transfer (4 KiB) takes
  at most about 0.3 s. A slower rate is refused.
- Pins on one PWM slice share its period, and a slice's period can't
  change while another of its pins runs.

**Example** (`PINS=1` guest, nothing wired to the pins):

```
# cd /usr/share/examples/pins
# ./gpio-blink 15 3 100 | head -2
GP15 set 1, pad reads 1
GP15 set 0, pad reads 0
# ./gpio-edge 14 4
pull-up: rising edge on GP14 (seqno 1)
pull-down: falling edge on GP14 (seqno 2)
pull-up: rising edge on GP14 (seqno 3)
pull-down: falling edge on GP14 (seqno 4)
4 of 4 edges
# ./adc-read | tail -1
temp raw  877  26.7 C
# micropython -c "from machine import ADC; print(ADC(ADC.CORE_TEMP).read_temp())"
26.67292789499999
# cd /sys/class/pwm/pwmchip0 && echo 15 > export && cd pwm15 &&
  echo 1000000 > period && echo 250000 > duty_cycle && echo 1 > enable   # 1 kHz, 25% on GP15
# mkdir /tmp/b && cd /tmp/b && cp /usr/share/examples/pins/gpio-blink.c . &&
  time cc -o gpio-blink gpio-blink.c && ./gpio-blink 15 2 50
real	0m 1.29s
GP15 set 1, pad reads 1
GP15 set 0, pad reads 0
GP15 set 1, pad reads 1
GP15 set 0, pad reads 0
```

Details: [guest/README.md](guest/README.md#pins) (the build, drivers and
packages), [rvemu-rs/README.md](rvemu-rs/README.md#pins-rvemu-picosrcpinsrs)
(the devices and the firmware side).

## Flash map (16 MB)

| Address | Contents | Written by |
|---|---|---|
| 0x10000000 | firmware (`rvemu-pico-shell` or `rvemu-pico-linux`) | `picotool load -x` |
| 0x10100000 | the guest's `/home`, 1 MB (jffs2) | the firmware (`rvemu-pico/src/flash.rs`) |
| 0x10200000 | swap, 511 sectors up to 0x103FEFFF (contents never persist) | `rvemu-pico-shell`, only with `pico-shell.sh -w` |
| 0x103FF000 | Wi-Fi settings, 4 KB (`"WIFI"`, lengths, SSID, passphrase) | `scripts/pico-wifi.sh` only |
| 0x10400000 | guest blob, at most 2 MB: `RVLY` header, the kernel after its text, initramfs, DTB | `pack-guest.py` via `pico-common.sh` |
| 0x10600000 | root filesystem (squashfs) | `pico-common.sh` `load_disk` |
| 0x11000000 − text_len | kernel text, ending exactly at the end of flash | `pico-common.sh` `load_text` |

- **The Wi-Fi sector.** The firmware's flash writer refuses everything
  outside the `/home` and swap regions, so the guest can't reach the Wi-Fi
  settings. Don't write 0x103FF000 with picotool or SWD either: it holds the
  network's passphrase, and the next user's Wi-Fi.
- **Kernel text.** At most 3.5 MiB (`TEXT_MAX` in `guest_blob.rs`;
  `pack-guest.py` refuses a longer one). The default kernel's text is
  2.92 MiB and the `PINS=1` kernel's 3.04 MiB. So the squashfs may reach
  0x10C80000: 6.5 MB.
- **Contiguous guest RAM.** Flash ends at 0x11000000, which is exactly where
  the PSRAM window starts. With the kernel's `.text` in the last bytes of
  flash, guest RAM is one contiguous block of host addresses: read-only
  kernel text from flash, then 8 MiB of PSRAM. With the default kernel the
  guest has 11,184 KiB of RAM (MemTotal 7,004 KiB, after the kernel's own).
- **Change stamps.** Each load script keeps a hash of what it wrote, in
  `~/.cache/rp2350-board/stamps`, and skips writes when nothing changed
  (`FORCE_BLOB=1` forces them).

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
  `pico-*.sh`, `perf-gate.sh`, `run-riscv.sh` and `board-cmd.py` refuse to
  run unless `BOARD_LOCK_OWNER` holds it (`board-lock run` sets it for its
  command).
- A lease (20 min by default) that isn't renewed expires, and the next
  waiter gets the lock. `run` renews it every 30 s while its command runs.
  Every grant and release goes to `~/.cache/rp2350-board/history.log`.
- `guest/run-build.sh` takes the `guest-build` lock itself for the Docker
  run, and refuses to start while you hold the board. When the
  configuration differs from the last one built in the volume, it reinstalls
  every package into a fresh `target/`, so an image never holds another
  checkout's packages.
- `rp2350/tools/board-cmd.py COMMAND` runs one command in the board's shell
  over the USB console, logging in as root if needed, prints its output and
  exits with its status. `rp2350/tools/boot-wait.py` waits for `login:` and
  prints the boot time (`--log FILE` keeps the console).

**Example:** a whole board session as one locked command.

```
$ rp2350/tools/board-lock run docs -- bash -c '
    cd rvemu-rs && scripts/pico-shell.sh &&
    ../rp2350/tools/boot-wait.py "$(cat ~/.cache/rp2350-board/last-launch)" &&
    ../rp2350/tools/board-cmd.py "uname -r; cat /proc/swaps"'
...
pico-shell.sh: Linux is booting; console: screen /dev/cu.usbmodemrvemu1 115200
10.92
6.12.116
Filename				Type		Size		Used		Priority
```

## Performance gate

`rvemu-rs/scripts/perf-gate.sh` is the on-device performance and stability
gate. Every change is measured on the board with it before it lands. It
runs:

1. the benchmark boot: `rvemu-pico-linux` boots the initramfs guest to
   `login:`, and its instruction count and console hash must match a
   reference run;
2. `rvemu-pico-shell` with the disk guest: the wall time from firmware
   start to `login:` (the boot console is kept in
   `~/.cache/rp2350-board/boot-logs/`);
3. a fixed workload over the console: the first runs of five programs, a
   MicroPython loop, a Lua loop and 200 fork+exec;
4. MemAvailable plus the pages on the per-CPU free lists (`mem_adj_kb`;
   the lists alone swing MemAvailable by hundreds of KiB between boots);
5. md5sum of a fixed list of files (`~/.cache/rp2350-board/md5-files.list`,
   58 files), cold and warm, so adding files to the image doesn't read as
   slower I/O; then of every file under `/usr /lib /bin /sbin`. All must
   match the squashfs image's own checksums: one flipped bit fails the gate.

```sh
rp2350/tools/board-lock run NAME -- rvemu-rs/scripts/perf-gate.sh -n "my change"
```

It needs `guest/out` and `guest/out-disk` built in the checkout. Results go
to `~/.cache/rp2350-board/perf.log`, one JSON line per run, and are
compared with `perf-baseline.json` there. A slowdown fails only past both a
percentage (3% for boot, 5% for the workload) and the run-to-run noise
measured on unchanged code (0.3 s; fork+exec 0.5 s; md5 0.8 / 1.5 s). So do
128 KiB less memory and any checksum mismatch. `-B` skips the benchmark
boot; `-s` makes the run the new baseline. One gate takes about 4 minutes.

## Results (on the board, 348 MHz)

| | |
|---|---|
| Boot to `login:`, disk guest (squashfs root, `/home` on flash, Wi-Fi) | 10.9 s |
| Boot to `login:`, initramfs guest (benchmark) | 5.2 s |
| Typed-key echo / `ping` to the gateway | 1.2 ms / 1.05 ms |
| Guest RAM (2.9 MiB flash-backed kernel text + 8 MiB PSRAM) | 11,184 KiB (MemTotal 7,004 KiB) |
| Translator vs the interpreter, jittest | 40.1 vs 1.90 MIPS (21x) |
