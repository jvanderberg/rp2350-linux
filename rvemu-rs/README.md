# rvemu-rs

An RV32IMA + Sv32 Linux system emulator in Rust. It runs on the Mac, under
QEMU, and on the RP2350's Hazard3 core. On the Hazard3, guest RAM is in
PSRAM and translated code is in SRAM.

| Crate | |
|---|---|
| `rvcore/` | `no_std` core: hart state, interpreter, soft TLB and Sv32 walk, built-in SBI, 16550 UART, PLIC, the virtio-mmio devices (`virtio.rs`), the NOR flash device (`nor.rs`), the pin devices (`periph.rs`) and the flash swap backend (`swapdev.rs`). The `instrument` feature adds host-only statistics (unique-code bitmaps, an XIP cache model) and needs `alloc`. `ram-code` puts the hot paths in SRAM on the board. |
| `rvjit/` | `no_std` block translator, RV32IMA guest code to RV32 host code |
| `rvemu/` | Mac host: loads the guest, generates the device tree (needs `dtc`), terminal console, statistics, RP2350 timing model (`-M`), cache-policy simulator (`-S`), and stand-ins for the board's flash (`norfile.rs`), swap flash and pins (`stubpins.rs`) |
| `rvemu-virt/` | bare-metal build for `qemu-system-riscv32 -M virt`: interpreter, translator or lockstep |
| `rvemu-pico/` | the board firmwares (below) |
| `jittest-guest/` | bare-metal S-mode test guest for the translator (separate workspace) |

The emulator is deterministic: guest time is retired instructions plus
skipped idle time, so a run is repeatable instruction for instruction. That
is what lockstep testing and the board-against-Mac comparisons rely on.

## Mac host

The Mac host runs the interpreter (rvjit emits RV32 code, so the
translator runs only on the board and under QEMU, below). It boots the disk
guest to `login:` in under a second.

    cargo build --release
    ./target/release/rvemu -k ../guest/out-disk/Image -d ../guest/out-disk/rootfs.squashfs -m 8   # Ctrl-A x quits
    ./target/release/rvemu -k ../guest/out/Image -i ../guest/out/rootfs.cpio -m 8 -B -u login:    # batch boot

| Option | |
|---|---|
| `-k FILE` | kernel Image, loaded at the start of RAM |
| `-i FILE` | initramfs (cpio) |
| `-d FILE` | disk image for the read-only virtio-blk device. Without `-i`, the default command line roots on it: `root=/dev/vda ro rootfstype=squashfs init=/sbin/overlay-init`. |
| `-F FILE` | the NOR flash device (the board's 1 MB `/home` region), backed by FILE, which is created blank (0xFF) if missing and written through, so `/home` persists across runs. `RVEMU_NOR_CUT=N` simulates a power cut: the N-th program or erase is applied halfway and the emulator exits. |
| `-W` | the swap device (below) on a modelled copy of the board's swap flash (511 sectors), with datasheet-typical erase and program times in its statistics. The guest's init enables it as `/dev/vdb`. |
| `-G` | the pin devices (GPIO, I2C, SPI, PWM, ADC) on a simulated board (`stubpins.rs`): an output reads back its level, an input its pull, or the line it is jumpered to (`RVEMU_JUMPERS=14-15,...`); a 256-byte EEPROM at 0x50 on I2C bus 0; SPI MISO wired to MOSI; the ADC reads 27 °C and 400 x N + 100 on GP40+N. `RVEMU_IO=gpio,i2c0,i2c1,spi,pwm,adc` picks which of them go in the device tree. |
| `-N` | virtio-net on a stub LAN (`stubnet.rs`): a gateway at 10.0.2.2 that answers ARP and ping. In the guest: `ifconfig eth0 10.0.2.15 up && ping -c 3 10.0.2.2`. |
| `-X BYTES` | the kernel's first BYTES (its text) become extra, read-only guest RAM, as on the board. The board's value is `text_len` in `scripts/pico-common.sh`: `_etext - _start` from `System.map`, rounded down to a page (2998272 for the disk kernel). |
| `-b FILE` | use this DTB instead of generating one |
| `-m MB` | guest RAM (default 64) |
| `-a ARGS` | kernel command line (default `earlycon console=ttyS0`) |
| `-u STRING` / `-n COUNT` | stop when the console prints STRING, or after COUNT instructions |
| `-B` | batch: no keyboard, idle time skipped. Also stops on a guest kernel panic. |
| `-M` | RP2350 timing model |
| `-D` | print the generated device tree and exit (the board scripts use this) |
| `-S FILE -J OUT [-C CORPUS]` | cache-policy simulator (below) |

### The board's devices on the Mac

`-F`, `-W` and `-G` stand in for the board's flash, swap and pins, so the
guest side of those features can be tried and tested without the board.
`-X 2998272` is the disk kernel's text. The guest's own
examples, on the board, are in the top-level README.

Boot cost, batch to `login:` (72.9 M guest instructions):

```
$ ./target/release/rvemu -k ../guest/out-disk/Image -d ../guest/out-disk/rootfs.squashfs \
    -m 8 -X 2998272 -N -B -u login: >/dev/null
...
guest instructions 72926879
```

`/home` across a power cut (`-F`, `RVEMU_NOR_CUT`): write a file, then cut
the power during a burst of writes, then check.

```
$ R="./target/release/rvemu -k ../guest/out-disk/Image -d ../guest/out-disk/rootfs.squashfs -m 8 -F /tmp/home.img"
$ $R          # head -c 50000 /dev/urandom > /home/keep; md5sum /home/keep > /home/keep.md5; sync
$ RVEMU_NOR_CUT=300 $R
# for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do head -c 8000 /dev/urandom > /home/f$i; done
[rvemu] RVEMU_NOR_CUT: power cut at flash operation 300
$ $R
# dmesg | grep jffs2
[    0.268287] jffs2: version 2.2. © 2001-2006 Red Hat, Inc.
[    1.757180] jffs2: jffs2_scan_inode_node(): CRC failed on node at 0x000e0f44: Read 0xffffffff, calculated 0xd3998a4c
# ls /home; md5sum -c /home/keep.md5
f1        f2        f4        f6        f8        keep      root
f10       f3        f5        f7        f9        keep.md5
/home/keep: OK
```

The half-written node fails its CRC and is skipped; everything written
before it is there, and new writes work.

Swap (`-W`): the guest's init finds the device by its serial and enables it.

```
$ ./target/release/rvemu -k ../guest/out-disk/Image -d ../guest/out-disk/rootfs.squashfs -m 8 -X 2998272 -W
# cat /proc/swaps
Filename				Type		Size		Used		Priority
/dev/vdb                                partition	1788		0		-2
# flashswap-stats | grep -E "erase_us_max|program_us_max"
erase_us_max 45000
program_us_max 700
```

Pins (`-G`, GP14 jumpered to GP15):

```
$ RVEMU_JUMPERS=14-15 ./target/release/rvemu -k ../guest/out-disk/Image \
    -d ../guest/out-disk/rootfs.squashfs -m 8 -X 2998272 -N -G
# gpioset -c gpiochip0 -t0 GP15=1; gpioget -c gpiochip0 GP14
"GP14"=active
# i2cset -y 0 0x50 0x10 0xab; i2cget -y 0 0x50 0x10
0xab
# cd /usr/share/examples/pins; ./gpio-edge 20 4 | tail -1; ./spi-xfer 12 34 56
4 of 4 edges
tx: 12 34 56
rx: 12 34 56
# micropython -c "from machine import I2C; i=I2C(0); print([hex(a) for a in i.scan()]); i.writeto_mem(0x50, 0x20, b'abc'); print(i.readfrom_mem(0x50, 0x20, 3))"
['0x50']
b'abc'
```

An input jumpered to a line that is still an output reads that output,
even after the program that drove it exits (the line keeps its
direction): `gpio-edge 14` then sees no edges. Use a free line.

tcc's memory check (no swap, `/tmp` holding 3.4 MB):

```
# grep MemAvailable /proc/meminfo
MemAvailable:        820 kB
# cc -o big big.c          # 3,000 one-line functions
tcc: memory full (508 kB left for the system)
```

`scripts/compare.sh [Image] [initrd]` boots the C reference (`../rvemu`) and
this emulator on the same guest, and requires identical console output and
counters.

## Machine

| Device | Address | IRQ |
|---|---|---|
| RAM | 0x00000000 | |
| PLIC | 0xF0000000 | |
| UART (16550) | 0xF4000000 | 1 |
| virtio-blk | 0xF8000000 | 2 |
| virtio-net | 0xF8000200 | 3 |
| NOR flash (`rvemu,flash`) | 0xF8000400 | |
| virtio-blk, swap (only with `-W` / `pico-shell.sh -w`) | 0xF8000600 | 5 |
| GPIO | 0xF8001000 | 8 |
| I2C0, I2C1 | 0xF8001200, 0xF8001400 | |
| SPI0 | 0xF8001600 | |
| PWM | 0xF8001800 | |
| ADC | 0xF8001A00 | |

- **No M-mode.** The kernel runs in S-mode, and SBI is implemented in the
  emulator.
- **Entropy.** The device tree's `/chosen` has a 32-byte `rng-seed`, so the
  kernel's CRNG is ready at boot. Without it the CRNG never initializes and
  every `get_random_*` (several per exec) re-extracts its pool. Batch runs,
  `-D` and the benchmark firmware keep the fixed bytes (`DTB_RNG_SEED`), so
  they stay deterministic. The interactive Mac host fills them from
  `/dev/urandom`, and `rvemu-pico-shell` from the RP2350's TRNG.
- **virtio** (`rvcore/src/virtio.rs`): one virtio-mmio v2 transport (split
  virtqueues, no indirect descriptors) shared by the devices. Requests
  complete synchronously.
  - **Block device:** one 16-entry queue. Backends implement `Disk`:
    `MemDisk` is a read-only byte range, such as the squashfs in flash. A
    backend can be writable and can offer a larger logical block size
    (`VIRTIO_BLK_F_BLK_SIZE`) and its own device ID (GET_ID, the guest's
    `/sys/block/vdX/serial`).
  - **Swap device:** a second, writable block device whose backend is
    `swapdev::FlashSwap` (below).
  - **Network device:** rx queue of 16, tx queue of 32. Linux stops a tx
    queue while fewer than 19 (`MAX_SKB_FRAGS + 2`) descriptors are free.
    Backends implement `NetLink`: the stub LAN on the Mac, the Wi-Fi radio
    on the board.
- **Pins** (`rvcore/src/periph.rs`, `-G`): small synchronous MMIO
  devices with their own Linux drivers (`guest/linux/drivers/rvemu`), over a
  `Pins` backend: `StubPins` on the Mac, `BoardPins` on the board.
  - **GPIO:** 48 lines, one control word each (output, output enable,
    pull-up, pull-down, rising/falling edge interrupt; the pad level reads
    back in bit 8), set/clear/input/pending words for 32 lines at a time.
    Lines the backend doesn't offer are invalid in the gpiochip. Edges come
    from the backend's latched state, checked when the device poll runs;
    there is no per-instruction cost.
  - **I2C:** the driver writes a table of up to 16 messages (address,
    read flag, length, buffer) and its count. The device runs the whole
    transfer before that store returns, and leaves the messages done (and
    a NACK flag) in a result register. Consecutive messages to one address
    are one transaction with repeated starts.
  - **SPI:** one transfer per write of GO: TX and RX buffers (guest
    physical), length, clock rate and mode; chip select is a register of
    its own. The Linux driver is an unqueued controller, so the SPI core
    runs no message-pump thread.
  - **PWM:** channel N is GPIO N. The backend answers with the period and
    duty cycle it achieved, which `get_state` reports.
  - **ADC:** one 12-bit result register per channel: 0-7 GP40-GP47, 8 the
    temperature sensor (bit 31: conversion error).
- **DMA and the translator:** each guest RAM range a device writes is
  reported separately (`take_dma_write`). The translator then drops only the
  code pages that were actually written.
- **NOR flash** (`rvcore/src/nor.rs`): the guest's `/home`. A command
  (read, program or erase, with an offset, a guest buffer and a length) is
  one register store, and it completes before the store returns, so there
  is no interrupt. Programs AND into the flash, as on NOR. The guest sees
  16 KB erase blocks (four 4 KB flash sectors): with 4 KB blocks jffs2 needs
  about 80 KiB more RAM for its per-block tables. Backends implement
  `NorFlash`: a file on the Mac, `flash::STORAGE` on the board. The guest's
  driver is `guest/linux/rvemu-flash.c`.
- **Read-only prefix:** `Cpu::set_readonly_prefix(n)` makes stores to the
  first `n` bytes of guest RAM fault. `GuestMem::ro` makes devices refuse DMA
  writes there. That is how the board maps the kernel's text straight from
  flash.

## Translator (`rvjit/`)

- **Registers.** Guest `xN` lives in host `xN`. Six host registers are
  reserved: x3 (state pointer), x4/x7/x23 (scratch), x22 (instruction
  budget) and x24 (helper link). These are the six least-used guest
  registers in a boot (about 2% of operand uses); their guest values live in
  `cpu.x[]`.
- **Exact timing.** A block is its guest pc, `jal budget_exit`, then the
  entry: `addi x22, x22, -N; blt x22, zero, <the jal>`. The budget is the
  number of instructions until the next timer deadline, so `instret` and
  interrupt timing are identical to the interpreter's.
- **Memory.** Loads and stores call a shared soft-TLB helper
  (`addi; jal helper; ld/st`). On a TLB miss the helper saves the
  caller-saved guest registers, calls `Cpu::tlb_refill()` on the host stack
  and carries on. MMIO, a fault, a write to a code page or a misaligned
  access exits with the call site, and `Cpu::step()` executes that one
  instruction. The dispatcher finds the block by scanning
  back to its `jal budget_exit`, and the instruction by counting helper
  calls, so no per-access metadata is stored. AMOs use the host
  AMO on the translated address, or load/op/store when `Jit::native_amo` is
  off. LR.W and SC.W keep the interpreter's reservation in `Cpu`
  (`res_valid`, `res_addr`). Ecall, sret and wfi go to the interpreter.
- **CSRs.** In S mode, every access to `sscratch`, `sepc`, `scause` and
  `stval` is a load and store on `Cpu`. So are reads of `sstatus` and SIE
  set or clear. Other `sstatus` writes call `Cpu::sstatus_op()` the same way
  as a TLB refill. Setting SIE exits if an enabled interrupt was already
  pending when the run began; the pending set can't change during a run.
  Other CSRs go to the interpreter.
- **Linking.** Blocks are keyed by (virtual pc, physical pc) and never cross a
  page. Same-page exits are chained by patching a `jal`. Other jumps use an
  inline jump cache, validated against the fetch TLB. On a jump-cache miss
  the same routine walks the target's hash chain in the code buffer and
  refills the entry, so only untranslated targets go back to the
  dispatcher.
- **Code cache.** Each block's descriptor (hash-chain link, physical and
  virtual pc) is a header in front of its code, so the code buffer is the
  only limit on how many blocks are live. When the buffer is full it is
  either flushed (`regions = 1`) or used as a ring of equal regions. Moving
  on to the next region evicts the blocks in it, which are the oldest: they
  are the tail of every hash chain, so each chain is cut at the first one.
  Chains from a newer region into an older one are logged, and reset when
  that older region is evicted. Blocks whose first instruction needs the
  interpreter are not kept.
- **Self-modifying code.** Pages with translations never get write-TLB
  entries. A store to one reaches the interpreter, and that page's blocks
  are dropped.

### Under QEMU

    scripts/virt.sh interp|jit|lockstep [-D] [-c] [-I] [-m MB] [-u STRING]

- `lockstep` checks every translated block against a second CPU that runs
  the interpreter on its own copy of RAM.
- `-D` boots the disk guest.
- `-c` uses `-icount` for exact host-instruction counts.
- `-I` takes keyboard input.
- `RVEMU_CODE_KB`, `RVEMU_NJC`, `RVEMU_REGIONS` and `RVEMU_NBUCKETS` at
  build time size the tables (the default buckets: 16384). For board-like
  numbers use `RVEMU_CODE_KB=326 RVEMU_NJC=1024 RVEMU_REGIONS=8
  RVEMU_NBUCKETS=1024` with `-c`.

With the disk guest, lockstep matches every event through boot to
`login:`.

tcc's self-host stress test runs here too:
`../guest/br-external/package/tcc/test/emu-selfhost.sh` boots the disk
guest under QEMU with the translator and `-m 11` (about the board's
MemTotal) and runs tcc's tests plus a stress case: tcc builds itself from
its sources one file at a time, then builds itself again with the result,
and the two binaries must be identical. With the sources and objects in
`/tmp` taking about 2 MB of RAM, the first build stops at "memory full"
after about 80 s, and the guest stays responsive while it runs (the
longest 10 s monitor step is about 16 s).

## On the RP2350 (`rvemu-pico/`)

All three firmwares run at 348 MHz (`rp2350-board`). Each one builds the CPU
in place in a static (`Cpu::init`, about 20 KB) rather than on the stack.

| Binary | Guest | Console | Script |
|---|---|---|---|
| `rvemu-pico-shell` | Linux from the flash blob; root on the squashfs in flash or the initramfs | USB CDC-ACM (`/dev/cu.usbmodemrvemu*`), status on RTT | `scripts/pico-shell.sh` |
| `rvemu-pico-linux` | the initramfs guest to `login:`, timed | RTT | `scripts/pico-linux.sh` |
| `rvemu-pico` | jittest: translator, lockstep and interpreter runs | RTT | `scripts/pico.sh` |

### rvemu-pico-shell

- **Two cores.** core0 runs only the guest. core1 does all the I/O: the
  USB console and the Wi-Fi radio ([core1](#core1-rvemu-picosrccore1rs)).
  `main` only runs the guest: a non-inlined `setup()` builds the devices
  in place in statics (`BUS`, `JIT`), so their construction doesn't sit
  on core0's stack.
- **Memory.** Guest RAM is the 8 MB PSRAM, preceded by the kernel text in
  flash (`guest_blob.rs`; flash map in the top-level README). The translator
  has a `CODE_KIB` code cache in SRAM (326 KiB; core1's code, slot, stack
  and rings take most of the rest), in a ring of 8 regions. It has no PSRAM
  backing store, because that traffic would share the 16 KB XIP
  cache with guest RAM.
- **Console** (`usbcon.rs`, on core1). Output waits in a 16 KB ring while no terminal
  is open, keeping the newest bytes, so a late `screen` shows the end of the
  boot log. With a terminal open nothing is lost: when the ring is full the
  guest waits for the terminal. Input goes into a 256-byte ring, and USB is
  read only while it has room.
- **Idle.** While the guest idles in WFI, core0 waits in real time until the
  next timer deadline (at most 100 ms per wait), then skips guest time ahead.
  Console input or a received frame (core1's event word) or a pin edge ends
  the wait at once. With swap on, an idle wait first erases one dirty swap
  sector if the guest won't wake within 2 ms.
- **BOOTSEL.** Opening the port at 1200 baud and closing it reboots into
  BOOTSEL. So does the SWD request in `rp2350-board::bootsel_if_requested`.
- **Disk.** The squashfs is read through flash's uncached, non-allocating
  XIP alias, so disk reads don't evict guest RAM from the XIP cache.
- **Entropy.** The DTB's `rng-seed` is overwritten with 32 bytes from the
  TRNG before the guest starts (health-checked, von Neumann debiased; RTT
  says so). The patch looks for the fixed `DTB_RNG_SEED` bytes, so a DTB
  made without `rvemu -D` keeps whatever seed it had.

### Flash writes (`rvemu-pico/src/flash.rs`)

The one module that erases or programs flash at run time: the `/home`
device (`BoardNor` in `shell.rs`) uses it, and so does swap
(`swap_flash.rs`).
- **Regions.** Only `STORAGE` (0x10100000-0x101FFFFF) and `SWAP`
  (0x10200000-0x103FEFFF) are writable. Anything else is refused: the
  firmware, the Wi-Fi sector, the blob, the squashfs and the kernel text.
- **API.** `erase_sector` (4 KB; a sector that is already blank is not
  erased), `program_page` (256 bytes from SRAM), `program` (any range in one
  region, from anywhere, PSRAM included; copied a page at a time into SRAM),
  `read` and `is_erased` through the uncached alias, and `stats`.
- **Stopping XIP.** Each sector erase or page program is one critical
  section: interrupts masked, core 1 parked (`core1::park()`), then an SRAM
  function saves the QMI window settings (the 348 MHz flash timing and the
  PSRAM's QPI setup), the QSPI pads and XIP_CTRL, calls the bootrom's
  `connect_internal_flash`, `flash_exit_xip` and erase or program, and
  restores them all.
- **XIP cache.** Only the changed flash lines are invalidated, by address.
  The bootrom's `flash_flush_cache` would also discard guest RAM's dirty
  PSRAM lines, which share the 16 KB cache.
- **Stalls.** The guest is paused while the flash is busy, and guest time
  doesn't advance meanwhile. Measured: at most 0.5 ms for a page and 47-51
  ms for a sector erase (400 ms by the datasheet). USB isn't serviced during
  a stall (the controller NAKs the host), and the console stays up.

### Flash swap (`rvcore/src/swapdev.rs`, `rvemu-pico/src/swap_flash.rs`)

With `pico-shell.sh -w` the guest gets `/dev/vdb` (`/dev/vda` with `-i`,
which has no root disk) in the flash region 0x10200000-0x103FEFFF, and its
init (`/etc/init.d/S02swap`) runs mkswap and swapon: 1788 KiB of swap. Without `-w` the DTB has no node: the guest pays
nothing, and the firmware never erases or writes the region. With it the
guest gives up 4 KiB of RAM plus the device's own kernel cost.

- **Layout.** A guest page is 4 KiB, the size of a flash erase sector, so
  each page lives in a sector of its own: 511 sectors, 448 pages of swap
  and 63 spare. A write goes to the next erased sector after a moving head
  and the sector it replaces becomes dirty. Nothing is copied, so write
  amplification is 1.
- **Erases wait for idle.** The firmware erases one dirty sector per guest
  WFI when the guest won't wake within 2 ms. A write erases synchronously
  only when no erased sector is left.
- **Wear levelling.** The head starts at a different sector every boot.
  Every 8th write also moves the live page nearest ahead of the head, so
  sectors that hold cold pages keep taking their turn at erases.
- **Nothing persists.** Swap is empty at every boot, so the map (2.2 KB)
  lives only in the top page of guest RAM, which the `-w` DTB reserves
  (`no-map`), and there's no on-flash metadata. Without `-w` the firmware
  finds no swap node in the DTB and sets up no device: no SRAM, no guest
  RAM. Sectors start in an unknown state; blank ones are found by reading
  them (about 0.1 ms).
- **Zero pages** take no flash. Data is stored inverted, so zero runs are
  erased flash and their 256-byte program operations are skipped.
- **Flash access.** Erases and page programs go through `flash.rs`
  (above), which parks core1 and invalidates only the lines it changed.
  Reads use the uncached XIP alias, so no cache maintenance is needed.
  Everything stalls during an erase or program: the firmware and the kernel
  text run from flash, and guest RAM is PSRAM on the same QMI.
- **Statistics.** The device's last page reads back as text: counters,
  erase/program/request times. `flashswap-stats` in the guest prints it.

Measured on the board (W25Q128, 348 MHz):

| | |
|---|---|
| 256-byte page program | 0.42 ms average, 0.56 ms max (5,826 programs) |
| 4 KB sector erase | 44.6 ms average, 48.8 ms max (230 erases) |
| 4 KiB swap write, pre-erased sector | about 7 ms: about 550 KB/s |
| 4 KiB swap write with its erase | about 52 ms, 55.9 ms max: about 80 KB/s |
| Reading back | 128 pages in 0.08 s |

### Pins (`rvemu-pico/src/pins.rs`)

`BoardPins` puts the guest's pin devices on rp235x-hal's drivers; the pin
map is in the top-level README. `pico-shell.sh` puts the devices in the
guest's device tree when the kernel has their drivers (the disk guest's
kernel). Nothing on the emulator's hot path looks at them: the pins are
polled for edges only in the firmware's periodic poll (`Platform::poll_pins`,
every 256 dispatches), and only while a line has an edge interrupt on.

- **GPIO.** Free pins switch between SIO input and output (and PWM) as the
  guest configures them; pulls and levels go straight to the pad. Edges
  latch in IO_BANK0's raw interrupt registers. The device poll reads them
  only when the IO_IRQ_BANK0 line is pending (one Xh3irq CSR read); that
  interrupt is never taken.
- **I2C.** 100 kHz with the pads' pull-ups. `hal::I2C` sets the
  controllers up; the transfers run on their registers (one byte in flight,
  as the pico-sdk does), because the HAL's waits have no time limit. Each
  byte and the STOP get 5 ms. Past that the controller is reset (it can't
  finish an abort on a held line) and set up again, a device still holding
  SDA low gets up to 9 SCL pulses and a STOP, and the guest's transfer
  fails with `-ETIMEDOUT`: a stuck bus can't stop the emulator.
  The controller can't send an address without data, so a zero-length
  write (i2cdetect's probe) becomes a one-byte read.
- **SPI.** `hal::spi::Spi`, 8-bit frames; the clock rate (100 kHz to
  50 MHz) and mode change per transfer. The controller makes the clock, so
  no device can hold a transfer up; the 100 kHz floor bounds the longest
  one (4 KiB, spidev's buffer) to about 0.3 s. Chip select GP17 is a GPIO, held for the whole message.
- **PWM.** The slice and channel that reach the pin, with the divider and
  TOP worked out from the period. A slice's period can't change while
  another of its pins is running.
- **ADC.** rp235x-hal 0.4's ADC knows only the QFN-60 channels, so the
  firmware selects the RP2350B's channels itself (AINSEL, START_ONCE).

### Wi-Fi (`rvemu-pico/src/wifi.rs`)

- **Radio.** The RM2 module's CYW43439, wired as on the Pico 2 W: GP23
  WL_ON, GP24 data/IRQ, GP25 CS, GP29 CLK. Firmware blobs and their licence
  are in `rvemu-pico/firmware/`.
- **Driver.** The `cyw43` 0.7 crate, without an executor: core1's service
  loop polls its futures with a no-op waker, and `embassy-time` reads the
  SIO RISC-V timer. The radio comes up on core1 while the guest boots. The
  guest waits for its MAC (at most 5 s) when it probes the NIC.
- **Bus.** A port of `cyw43-pio`'s PIO gSPI program to rp235x-hal. The CPU
  feeds the FIFOs where the original uses DMA. The divider keeps the PIO clock
  at or below 50 MHz (a 25 MHz gSPI clock) at any system clock. A transfer that takes more
  than 20 ms is abandoned, so a missing radio can't hang core1. The
  firmware blobs are read through flash's uncached alias, so uploading them
  doesn't sweep the guest's data out of the XIP cache.
- **Network.** Ethernet frames are bridged to the guest's virtio-net through
  two frame rings (2 slots each way; the driver buffers 4 more received
  frames). A frame the guest sends while core1 is two behind is dropped. The
  guest gets the radio's MAC, and its link state follows the join.
- **Credentials.** `scripts/pico-wifi.sh SSID` asks for the passphrase and
  writes it to the 4 KB at 0x103FF000. The firmware joins in the background
  and retries every 3 s. `--forget` erases the network.

### core1 (`rvemu-pico/src/core1.rs`)

- **Service loop.** core1 runs one `core1::Service` (`fn poll(&mut self)`),
  built by `core1::launch` in a static slot (`SERVICE_BYTES`, checked at
  compile time) so it never moves and may hold pinned futures. In
  `rvemu-pico-shell` it is `Io` in `shell.rs`: the console, the radio and an
  RTT status line (stack depths, every 10 s). To add a service, add a field
  to `Io` and call it from `Io::poll`. A pass must not block for long, since
  the other services wait.
- **Sharing.** The cores share only lock-free single-producer
  single-consumer rings (`ByteRing`, `FrameRing`) and single-writer words, in
  main SRAM or SRAM8/9 (`#[unsafe(link_section = ".sram89.NAME")]`, zeroed
  at startup). core1 bumps an event word (`notify_core0`) when it hands the
  guest something; core0's device poll is one load and a compare until then.
  core0 rings a doorbell (`notify_core1`) when it queues work.
- **Sleep.** Between passes core1 sleeps in `wfi` until a USB or radio
  (PIO) interrupt, a doorbell, or a 1 ms timer. Interrupts are never taken:
  they only end the `wfi`. core1 keeps time with the SIO RISC-V timer
  (`core1::now()`, at the system clock).
- **SRAM only.** Everything core1 runs is in SRAM, because the cores share
  one QMI and one 16 KB XIP cache with the guest's RAM. `core1.x` links the
  crates and modules it uses into `.core1_text` (copied from flash in
  riscv-rt's `pre_init`), as well as any function marked
  `#[unsafe(link_section = ".data.core1")]`. `scripts/check-core1.py` lists
  every call or address from there into flash, and fails on any that isn't
  a cold path (panics, RTT formatting, BOOTSEL, the radio firmware upload).
- **Flash writes.** Erasing or programming flash stalls XIP.
  `core1::park()` holds core1 in an SRAM loop until `core1::unpark()`;
  `flash.rs` calls the pair around every erase and program.
- **Checking it.** After any change to what core1 calls:

  ```
  $ cargo build --release -p rvemu-pico --target riscv32imac-unknown-none-elf
  $ scripts/check-core1.py target/riscv32imac-unknown-none-elf/release/rvemu-pico-shell
  ...
  check-core1: 274 flash targets, 0 not known to be cold
  ```
- **Memory.** core1's code is about 55 KB in `.core1_text`. Its service
  slot is 7 KiB, its stack 5 KiB, the console rings 16 KiB, and the frame
  rings 6 KiB in SRAM8/9.

### Hazard3 constraints

- RP2350 only supports AMOs on main SRAM (datasheet 2.1.6.2), so with guest
  RAM in PSRAM the firmwares set `Jit::native_amo = false`.
- Hazard3 has no misaligned loads or stores, and a misaligned access to the
  PSRAM window can wedge the hart, so the TLB helpers send misaligned
  accesses to the interpreter.
- Code inlined into a flash caller runs from flash, so the run loops are
  SRAM functions of their own (`.data.rvemu_hot`, with `ram-code`).

### jittest

`jittest-guest/` exercises what the translator treats specially:
- ALU and M-extension edge cases, every load/store width, misaligned
  accesses;
- branchy, recursive and indirect-call code;
- AMOs and LR/SC;
- exact `instret`/`time` reads;
- self-modifying code, traps and MMIO;
- Sv32 paging: demand faults, remapping after `sfence.vma`, SUM, U-mode, and
  one physical page at two virtual addresses;
- timer interrupts and WFI.

Each test prints a hash and its exact instruction count. `scripts/pico.sh`
rebuilds it (the firmware embeds `jittest-guest/jittest.bin`), runs it on the
Mac for the reference, and compares every board run.

| Run | Signature | Guest insns | Speed |
|---|---|---|---|
| Mac interpreter | 56c7e73f | 9,540,070 | reference |
| RP2350 interpreter | 56c7e73f | 9,540,070 | 1.90 MIPS, 183 cycles/insn |
| RP2350 translator | 56c7e73f | 9,540,070 | 40.1 MIPS, 8.68 cycles/insn |
| RP2350 lockstep | 56c7e73f | 9,540,070 | 2,477 events matched |

## Scripts

| Script | |
|---|---|
| `scripts/pico-shell.sh [-i] [-b] [-w] [-m MB]` | load the guest and `rvemu-pico-shell` over USB BOOTSEL, then print the console port; `-w` adds the flash swap device |
| `scripts/pico-wifi.sh SSID \| --forget` | store or erase the Wi-Fi network |
| `scripts/pico-linux.sh [TIMEOUT]` | boot benchmark: board against a Mac reference run, same instruction count and console hash |
| `scripts/pico.sh` | jittest on the board against the Mac |
| `scripts/virt.sh` | the bare-metal build under QEMU |
| `scripts/compare.sh` | C reference against Rust |
| `scripts/sim.py` | board time for cache policies simulated by `rvemu -S` |
| `scripts/check-core1.py [ELF]` | references from core1's SRAM code into XIP flash (fails on any outside cold paths); run it after any change to what core1 calls |
| `scripts/perf-gate.sh [-n LABEL] [-B] [-s]` | the board's performance and stability gate: boots the board, times a fixed workload, checks the memory left and checksums every file in the root filesystem, and fails on anything slower, smaller or wrong against `~/.cache/rp2350-board/perf-baseline.json`. Run it as `../rp2350/tools/board-lock run NAME -- scripts/perf-gate.sh -n LABEL`; it needs `guest/out` and `guest/out-disk`. `-B` skips the benchmark boot, `-s` sets the baseline. |
| `scripts/pico-common.sh` | shared by the `pico-*` scripts: BOOTSEL, `load_at` with change stamps, `pack_guest`, the flash addresses |

`../rp2350/tools/board-cmd.py COMMAND` runs a command in the board's shell
over the USB console and prints its output, and `../rp2350/tools/boot-wait.py`
waits for `login:`. Every board script needs the board lock
(`../rp2350/tools/board-lock`, top-level README).

Notes:
- picotool needs `-t bin` for files with unknown extensions, and `-t elf`
  for Cargo's extension-less ELFs.
- `request_bootsel` refuses to run while another program holds the console
  port, because the firmware acts on the 1200-baud touch only once the port
  is closed.

## Cache-policy simulator (`rvemu -S`)

This shows how a boot behaves with different code-cache policies, without
porting each one to the board. The Mac interpreter runs the guest once. Each
policy in the spec file mirrors the dispatcher using rvjit's own block shapes
and sizes (`Jit::block_code`):
- lookups and translations;
- flushes and evictions;
- chain slots and the jump cache;
- budget and TLB-helper exits;
- self-modifying-code invalidation.

The policy syntax is in `rvemu/src/sim.rs`.

    ./target/release/rvemu -k ../guest/out/Image -i ../guest/out/rootfs.cpio -m 8 \
        -u login: -B -S policies.txt -J sim.json [-C corpus.bin]
    scripts/sim.py sim.json [--detail NAME]

The simulated event counts match the real translator under QEMU: translated
blocks, guest instructions, flushes and SMC invalidations exactly, and
entries, exits and chains within 0.003%. Host-instruction costs are fitted to
QEMU `-icount` runs:

| Component | Host insns | Fit residual |
|---|---|---|
| Translated code | 7.54 per guest insn + 101 per entry | < 0.04% |
| Dispatcher | 158 per `run_one` + 149 per chain request | < 0.25% |
| Interpreter fallback | 333 per step | |
| Translation | 358 per guest insn (+ flush) | 5% at 1024 KiB |

`sim.py` converts these to board cycles with constants fitted to the board:
- CPI 1.12;
- 137 cycles per XIP miss, and 0.61 misses per translated guest instruction
  (the translator reads guest code from PSRAM);
- copies at 41 cycles per word of sequential PSRAM read, 32 per word
  written and 46 per word of flash.

The fitted model reproduces measured board boots to within 1.2%.

The model accounts for the current design:
- **Running translations in place from flash or PSRAM is a loss.**
  Translated code misses the 16 KB XIP cache about every two instructions,
  and it evicts guest data that shares that cache.
- **A hot-threshold policy barely helps.** The cost is thrashing of hot
  code, not translating cold code once.
- **A ring of regions beats flush-all** at the same size.
