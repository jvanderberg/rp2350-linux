# rvemu-rs

An RV32IMA + Sv32 Linux system emulator in Rust. It runs on the Mac, under
QEMU, and on the RP2350's Hazard3 core. On the Hazard3, guest RAM is in
PSRAM and translated code is in SRAM.

| Crate | |
|---|---|
| `rvcore/` | `no_std` core: hart state, interpreter, soft TLB and Sv32 walk, built-in SBI, 16550 UART, PLIC, the virtio-mmio devices (`virtio.rs`) and the NOR flash device (`nor.rs`). The `instrument` feature adds host-only statistics (unique-code bitmaps, an XIP cache model) and needs `alloc`. `ram-code` puts the hot paths in SRAM on the board. |
| `rvjit/` | `no_std` block translator, RV32IMA guest code to RV32 host code |
| `rvemu/` | Mac host: loads the guest, generates the device tree (needs `dtc`), terminal console, statistics, RP2350 timing model (`-M`), cache-policy simulator (`-S`) |
| `rvemu-virt/` | bare-metal build for `qemu-system-riscv32 -M virt`: interpreter, translator or lockstep |
| `rvemu-pico/` | the board firmwares (below) |
| `jittest-guest/` | bare-metal S-mode test guest for the translator (separate workspace) |

The emulator is deterministic: guest time is retired instructions plus
skipped idle time, so a run is repeatable instruction for instruction. That
is what lockstep testing and the board-against-Mac comparisons rely on.

## Mac host

    cargo build --release
    ./target/release/rvemu -k ../guest/out-disk/Image -d ../guest/out-disk/rootfs.squashfs -m 8   # Ctrl-A x quits
    ./target/release/rvemu -k ../guest/out/Image -i ../guest/out/rootfs.cpio -m 8 -B -u login:    # batch boot

| Option | |
|---|---|
| `-k FILE` | kernel Image, loaded at the start of RAM |
| `-i FILE` | initramfs (cpio) |
| `-d FILE` | disk image for the read-only virtio-blk device. Without `-i`, the default command line roots on it: `root=/dev/vda ro rootfstype=squashfs init=/sbin/overlay-init`. |
| `-F FILE` | the NOR flash device (the board's 1 MB `/home` region), backed by FILE, which is created blank (0xFF) if missing and written through, so `/home` persists across runs. `RVEMU_NOR_CUT=N` simulates a power cut: the N-th program or erase is applied halfway and the emulator exits. |
| `-N` | virtio-net on a stub LAN (`stubnet.rs`): a gateway at 10.0.2.2 that answers ARP and ping. In the guest: `ifconfig eth0 10.0.2.15 up && ping -c 3 10.0.2.2`. |
| `-X BYTES` | the kernel's first BYTES (its text) become extra, read-only guest RAM, as on the board |
| `-m MB` | guest RAM (default 64) |
| `-a ARGS` | kernel command line (default `earlycon console=ttyS0`) |
| `-u STRING` / `-n COUNT` | stop when the console prints STRING, or after COUNT instructions |
| `-B` | batch: no keyboard, idle time skipped. Also stops on a guest kernel panic. |
| `-M` | RP2350 timing model |
| `-D` | print the generated device tree and exit (the board scripts use this) |
| `-S FILE -J OUT [-C CORPUS]` | cache-policy simulator (below) |

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

- **No M-mode.** The kernel runs in S-mode, and SBI is implemented in the
  emulator.
- **virtio** (`rvcore/src/virtio.rs`): one virtio-mmio v2 transport (split
  virtqueues, no indirect descriptors) shared by both devices. Requests
  complete synchronously.
  - **Block device:** read-only, with one 16-entry queue. Backends implement
    `Disk`: `MemDisk` is a byte range, such as the squashfs in flash.
  - **Network device:** rx queue of 16, tx queue of 32. Linux stops a tx
    queue while fewer than 19 (`MAX_SKB_FRAGS + 2`) descriptors are free.
    Backends implement `NetLink`: the stub LAN on the Mac, the Wi-Fi radio
    on the board.
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

- **Registers.** Guest `xN` lives in host `xN`. Five host registers are
  reserved: x3 (state pointer), x4/x7/x23 (scratch) and x24 (helper link).
  These are the five least-used guest registers in a boot (about 1.5% of
  operand uses); their guest values live in `cpu.x[]`.
- **Exact timing.** Every block starts with `budget -= N; if < 0 exit`. The
  budget is the number of instructions until the next timer deadline, so
  `instret` and interrupt timing are identical to the interpreter's.
- **Memory.** Loads and stores call a shared soft-TLB helper
  (`addi; jal helper; .word meta; ld/st`) that skips the metadata word on a
  hit. A miss, MMIO, a fault, a page-crossing access or a misaligned access
  exits, and `Cpu::step()` executes that one instruction. AMOs use the host
  AMO on the translated address, or load/op/store when `Jit::native_amo` is
  off. LR/SC, CSRs, ecall, sret and wfi go to the interpreter.
- **Linking.** Blocks are keyed by (virtual pc, physical pc) and never cross a
  page. Same-page exits are chained by patching a `jal`. Other jumps use an
  inline jump cache, validated against the fetch TLB.
- **Code cache.** When the code buffer is full it is either flushed
  (`regions = 1`) or used as a ring of equal regions. Moving on to the next
  region evicts the blocks in it, which are the oldest. Chains from a newer
  region into an older one are logged, and reset when that older region is
  evicted.
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

With the disk guest, lockstep matches through boot to `login:` (2,517,290
events). Translation is about half of host time, at 31 host instructions per
guest instruction under `-icount`.

## On the RP2350 (`rvemu-pico/`)

All three firmwares run at 348 MHz (`rp2350-board`). Each one builds the CPU
in place in a static (`Cpu::init`, about 20 KB) rather than on the stack.

| Binary | Guest | Console | Script |
|---|---|---|---|
| `rvemu-pico-shell` | Linux from the flash blob; root on the squashfs in flash or the initramfs | USB CDC-ACM (`/dev/cu.usbmodemrvemu*`), status on RTT | `scripts/pico-shell.sh` |
| `rvemu-pico-linux` | the initramfs guest to `login:`, timed | RTT | `scripts/pico-linux.sh` |
| `rvemu-pico` | jittest: translator, lockstep and interpreter runs | RTT | `scripts/pico.sh` |

### rvemu-pico-shell

- **Memory.** Guest RAM is the 8 MB PSRAM, preceded by the kernel text in
  flash (`guest_blob.rs`; flash map in the top-level README). The translator
  has a 320 KiB code cache in SRAM: 4,096 blocks in a ring of 8 regions. It
  has no PSRAM backing store, because that traffic would share the 16 KB XIP
  cache with guest RAM.
- **Console.** Output waits in a 16 KB ring (a static) while no terminal is
  open, so a late `screen` shows the end of the boot log. Input goes into a
  256-byte ring.
- **Idle.** While the guest idles in WFI, the firmware polls USB and the
  radio in real time until the next timer deadline (at most 100 ms per
  wait), then skips guest time ahead.
- **BOOTSEL.** Opening the port at 1200 baud and closing it reboots into
  BOOTSEL. So does the SWD request in `rp2350-board::bootsel_if_requested`.
- **Disk.** The squashfs is read through flash's uncached, non-allocating
  XIP alias, so disk reads don't evict guest RAM from the XIP cache.

### Flash writes (`rvemu-pico/src/flash.rs`)

The one module that erases or programs flash at run time: the `/home`
device uses it, and so does swap.
- **Regions.** Only `STORAGE` (0x10100000-0x101FFFFF) and `SWAP`
  (0x10200000-0x103FEFFF) are writable. Anything else is refused: the
  firmware, the Wi-Fi sector, the blob, the squashfs and the kernel text.
- **API.** `erase_sector` (4 KB; a sector that is already blank is not
  erased), `program_page` (256 bytes from SRAM), `program` (any range in one
  region, from anywhere, PSRAM included; copied a page at a time into SRAM),
  `read` and `is_erased` through the uncached alias, and `stats`.
- **Stopping XIP.** Each sector erase or page program is one critical
  section: interrupts masked, core 1 parked (`park_core1`), then an SRAM
  function saves the QMI window settings (the 348 MHz flash timing and the
  PSRAM's QPI setup), the QSPI pads and XIP_CTRL, calls the bootrom's
  `connect_internal_flash`, `flash_exit_xip` and erase or program, and
  restores them all.
- **XIP cache.** Only the changed flash lines are invalidated, by address.
  The bootrom's `flash_flush_cache` would also discard guest RAM's dirty
  PSRAM lines, which share the 16 KB cache.
- **Stalls.** The guest is paused while the flash is busy: about 0.7 ms per
  page and 45 ms per sector (400 ms at most). Guest time doesn't advance
  meanwhile, and USB is polled again as soon as the store returns.

### Wi-Fi (`rvemu-pico/src/wifi.rs`)

- **Radio.** The RM2 module's CYW43439, wired as on the Pico 2 W: GP23
  WL_ON, GP24 data/IRQ, GP25 CS, GP29 CLK. Firmware blobs and their licence
  are in `rvemu-pico/firmware/`.
- **Driver.** The `cyw43` 0.7 crate, without an executor: the emulator's
  device poll polls its futures with a no-op waker, and `embassy-time` reads
  the core's cycle counter.
- **Bus.** A port of `cyw43-pio`'s PIO gSPI program to rp235x-hal. The CPU
  feeds the FIFOs where the original uses DMA. The divider keeps the PIO clock
  at or below 50 MHz (a 25 MHz gSPI clock) at any system clock. A transfer that takes more
  than 20 ms is abandoned, so a missing radio can't hang the emulator.
- **Network.** Ethernet frames are bridged to the guest's virtio-net. The
  guest gets the radio's MAC, and its link state follows the join.
- **Credentials.** `scripts/pico-wifi.sh SSID` asks for the passphrase and
  writes it to the 4 KB at 0x103FF000. The firmware joins in the background
  and retries every 3 s. `--forget` erases the network.

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
| RP2350 translator | 56c7e73f | 9,540,070 | 36.8 MIPS, 9.45 cycles/insn |
| RP2350 lockstep | 56c7e73f | 9,540,070 | 3,503 events matched |

## Scripts

| Script | |
|---|---|
| `scripts/pico-shell.sh [-i] [-b] [-m MB]` | load the guest and `rvemu-pico-shell` over USB BOOTSEL, then print the console port |
| `scripts/pico-wifi.sh SSID \| --forget` | store or erase the Wi-Fi network |
| `scripts/pico-linux.sh [TIMEOUT]` | boot benchmark: board against a Mac reference run, same instruction count and console hash |
| `scripts/pico.sh` | jittest on the board against the Mac |
| `scripts/virt.sh` | the bare-metal build under QEMU |
| `scripts/compare.sh` | C reference against Rust |
| `scripts/sim.py` | board time for cache policies simulated by `rvemu -S` |
| `scripts/pico-common.sh` | shared by the `pico-*` scripts: BOOTSEL, `load_at` with change stamps, `pack_guest`, the flash addresses |

`../rp2350/tools/board-cmd.py COMMAND` runs a command in the board's shell
over the USB console and prints its output.

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
