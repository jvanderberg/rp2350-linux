# rvemu

The C reference interpreter: RV32IMA + Sv32, boots MMU Linux on the Mac.
`../rvemu-rs` is the emulator in use (Rust, Mac + QEMU + RP2350, with the
translator). `../rvemu-rs/scripts/compare.sh` requires this one and the Rust
interpreter to produce identical console output and counters on a full boot,
using a shared device tree and subtracting the Rust host's one setup TLB flush.

## Build and run

    make
    make run      # interactive boot of guest/out (Ctrl-A x quits)
    make bench    # deterministic boot-to-login with the RP2350 timing model

Options: `./rvemu -h`. `-m` sets the guest RAM size and the emulator generates
a matching device tree (`-D` prints it), so a different kernel/rootfs is a
drop-in: `./rvemu -k ../guest/out/Image -i ../guest/out/rootfs.cpio -m 8`.
`make run` / `make bench` use `../guest/out`; build it with
`(cd ../guest && ./run-build.sh all)`. Override with `make run GUEST=/path/to/guest`.

## Design

- **No M-mode.** The kernel runs in S-mode; SBI (base, time, IPI, rfence,
  HSM, reset, legacy console) is implemented in C.
- **Soft TLB** on every access: separate R/W/X tables and separate sets for
  U, S and S+SUM, so privilege changes and SUM toggles don't flush. An entry is
  `{page tag, addend}` with host = va + addend. Misses walk Sv32 and set A/D
  at fill time.
- **Deterministic time.** One timer tick per retired instruction
  (timebase 25 MHz), and WFI skips ahead to the next timer deadline. Runs are
  repeatable instruction for instruction.
- **Devices:** 16550 UART on the terminal, single-context PLIC (no virtio:
  initramfs guests only).

## Timing model (`-M`)

Estimates, not measurements. The instruction mix, unique code and XIP-cache
misses come from the run. The per-class cycle costs and the 100-cycle PSRAM
miss are guesses (constants at the top of `report()` in `src/main.c`). The
XIP cache is modeled as 16 KiB, 2-way, 8-byte lines; check those parameters
against the RP2350 datasheet before trusting the miss counts.
