# rvemu-rs

An RV32IMA + Sv32 Linux system emulator in Rust. It runs on the Mac, under
QEMU, and on the RP2350's Hazard3 core. On the Hazard3, guest RAM is in
PSRAM and translated code is in SRAM.

| Crate | |
|---|---|
| `rvcore/` | `no_std` core: hart state, interpreter, soft TLB and Sv32 walk, built-in SBI, 16550 UART, PLIC and the virtio-mmio devices (`virtio.rs`). The `instrument` feature adds host-only statistics (unique-code bitmaps, an XIP cache model) and needs `alloc`. `ram-code` puts the hot paths in SRAM on the board. |
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

- **No M-mode.** The kernel runs in S-mode, and SBI is implemented in the
  emulator.
- **Entropy.** The device tree's `/chosen` has a 32-byte `rng-seed`, so the
  kernel's CRNG is ready at boot. Without it the CRNG never initializes and
  every `get_random_*` (several per exec) re-extracts its pool. Batch runs,
  `-D` and the benchmark firmware keep the fixed bytes (`DTB_RNG_SEED`), so
  they stay deterministic. The interactive Mac host fills them from
  `/dev/urandom`, and `rvemu-pico-shell` from the RP2350's TRNG.
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
  build time size the tables. `384`, `1024`, `8` and `1024` match
  rvemu-pico.

With the disk guest, lockstep matches through boot to `login:` (2,517,290
events). With rvemu-pico's table sizes, that boot takes 20.9 host
instructions per guest instruction under `-icount`, and translation is 7.8 of
them.

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
  USB console and the Wi-Fi radio (below).
- **Memory.** Guest RAM is the 8 MB PSRAM, preceded by the kernel text in
  flash (`guest_blob.rs`; flash map in the top-level README). The translator
  has a `CODE_KIB` code cache in SRAM (330 KiB; core1 takes the rest), in a
  ring of 8 regions. It
  has no PSRAM backing store, because that traffic would share the 16 KB XIP
  cache with guest RAM.
- **Console** (`usbcon.rs`). Output waits in a 16 KB ring while no terminal
  is open, keeping the newest bytes, so a late `screen` shows the end of the
  boot log. With a terminal open nothing is lost: when the ring is full the
  guest waits for the terminal. Input goes into a 256-byte ring, and USB is
  read only while it has room.
- **Idle.** While the guest idles in WFI, core0 waits in real time until the
  next timer deadline (at most 100 ms per wait), then skips guest time ahead.
  Console input or a received frame ends the wait at once.
- **BOOTSEL.** Opening the port at 1200 baud and closing it reboots into
  BOOTSEL. So does the SWD request in `rp2350-board::bootsel_if_requested`.
- **Disk.** The squashfs is read through flash's uncached, non-allocating
  XIP alias, so disk reads don't evict guest RAM from the XIP cache.
- **Entropy.** The DTB's `rng-seed` is overwritten with 32 bytes from the
  TRNG before the guest starts.

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
  `core1::park()` holds core1 in an SRAM loop until `core1::unpark()`; the
  pair fits `flash::Parker`.
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
| `scripts/pico-shell.sh [-i] [-b] [-m MB]` | load the guest and `rvemu-pico-shell` over USB BOOTSEL, then print the console port |
| `scripts/pico-wifi.sh SSID \| --forget` | store or erase the Wi-Fi network |
| `scripts/pico-linux.sh [TIMEOUT]` | boot benchmark: board against a Mac reference run, same instruction count and console hash |
| `scripts/pico.sh` | jittest on the board against the Mac |
| `scripts/virt.sh` | the bare-metal build under QEMU |
| `scripts/compare.sh` | C reference against Rust |
| `scripts/sim.py` | board time for cache policies simulated by `rvemu -S` |
| `scripts/check-core1.py [ELF]` | references from core1's SRAM code into XIP flash (fails on any outside cold paths) |
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
