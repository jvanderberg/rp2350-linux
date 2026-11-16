# rp2350: board support, bring-up tests, tools

Target: an RP2350B with 8 MB AP Memory APS6404L PSRAM on QMI CS1 (GP0), 16 MB
W25Q128 flash, and an RM2 (CYW43439) Wi-Fi module on the Pico 2 W pins (GP23
WL_ON, GP24 data/IRQ, GP25 CS, GP29 CLK). Debug probe: a second RP2350 board
running debugprobe (CMSIS-DAP) over SWD.

## board/ (`rp2350-board` crate)

Shared by the bring-up tests and the emulator firmwares:

- **Clocks.** The default feature `overclock` gives 348 MHz at 1.20 V;
  `--no-default-features` keeps the stock 150 MHz.
- **PSRAM init.** QMI CS1 at 116 MHz (divider 3 at 348 MHz), mapped in the
  cached XIP window at 0x11000000.
- **Hazard3 counters.** `mcycle64` and friends; `TIMER0.DBGPAUSE` is cleared
  so a halted core can't skew timings.
- **BOOTSEL.** `reboot_to_bootsel`, and `bootsel_if_requested`: a debugger
  writes `0xb0075e1c` to watchdog SCRATCH0 (0x400d800c) and resets the chip.
- **Linker scripts.** `memory.x` and `rp235x_riscv.x`, which adds the RP2350
  boot blocks and then includes riscv-rt's `link.x`.

## Linux guest in flash

The emulator firmwares (`../rvemu-rs/rvemu-pico`) read the guest from flash.
The flash map is in the [top-level README](../README.md#flash-map-16-mb).

- **Kernel text in place.** Flash ends at 0x11000000, exactly where the
  PSRAM window begins. The kernel's text and read-only data go in the last
  `text_len` bytes of flash, so guest RAM is one contiguous block of host
  addresses: guest PA 0 is the text, read in place from flash, and the
  PSRAM follows.
  - The emulator makes the prefix read-only: stores fault
    (`Cpu::set_readonly_prefix`) and DMA writes are refused (`GuestMem::ro`).
  - `text_len` is `__init_begin - _start` from `System.map` when the kernel
    puts its read-only data right after its text
    (`guest/linux/rodata-in-flash.patch`): 3502080 bytes for the disk
    kernel. For a kernel without the patch it is `_etext - _start`, rounded
    down to a page (that page also holds `.init`).
  - Guest RAM is `text_len` plus the PSRAM part, 8128 KiB by default
    (`pico-shell.sh`'s `M`; the native engine has the top 64 KiB of the
    8 MiB): 11825152 bytes with the disk kernel.
- **Why not `XIP_KERNEL`.** Linux's own RISC-V `XIP_KERNEL` doesn't fit
  here: on RV32 `KERNEL_LINK_ADDR == PAGE_OFFSET`, so the text would collide
  with RAM's linear map.
- **Guest blob** (`tools/pack-guest.py`, read by
  `rvemu-pico/src/guest_blob.rs`): ten little-endian u32s.
  - Magic `"RVLY"`.
  - Guest RAM size (text + PSRAM part).
  - Offset and length of the kernel after its text, of the initramfs, and of
    the DTB. Each part is 4 KiB aligned, inside 2 MB.
  - `text_len`, and an FNV-1a hash of the text, which the firmware checks so
    a stale text image is caught.
- **Loading.** `rvemu-rs/scripts/pico-common.sh` packs and loads the blob,
  the text, the squashfs and the firmware. It keeps a hash of each write and
  skips the parts that haven't changed.

## tools/

| Tool | |
|---|---|
| `boot-wait.py START [--log FILE]` | wait for `login:` on the console and print the seconds since START (`pico-shell.sh` writes it to `rvemu-rs/target/pico-last-launch`) |
| `pack-guest.py IMAGE INITRD DTB RAM_MB TEXT_LEN BLOB TEXT` | split an Image into the flash text and the guest blob |
| `board-cmd.py [--port P] [--timeout S] COMMAND...` | run a command in the board's shell over the USB console (logs in as root if needed), print its output, exit with its status |
| `pin-test.py [--host ADDR] [--only NAME,...]` | test the Linux board's GPIO, edges, pull-up, PWM, I2C, SPI, MicroPython `machine`, and PIO and DMA from C and MicroPython `rp2`, against a second board running `pintester/` ([Pin tests](#pin-tests)) |
| `serial-chat.py PORT EXPECT [SEND EXPECT]...` | script a serial console: wait for a pattern, send a line, repeat |
| `run-riscv.sh ELF [TIMEOUT] [STOP_REGEX]` | flash a Hazard3 ELF with probe-rs and stream RTT; `ATTACH=1` attaches to a running program instead |
| `rtt.py CB_ADDR` | RTT reader over OpenOCD's Tcl port |
| `pcsample.py ELF` | statistical PC profiler over OpenOCD |
| `fnv.py OUT ERR` | instruction count and console hash of a Mac run, as the benchmark firmware reports them |

`board-cmd.py` sends `echo RVB""EGIN_<tag>; COMMAND; echo RVE""ND_<tag> $?`.
The echoed command line keeps the quotes, so only real output matches the
markers, and the random tag rules out stale ones. The port takes one program
at a time, so close `screen` first.

## Pin tests

`tools/pin-test.py` tests the Linux board's pins against a second RP2350B
board running `pintester/`, which drives and measures them. It ends
with `N of M passed` and exits with 0 only when every check passed. On the
board it passes 44 of 44.

### pintester/ (RISC-V, Rust, rp235x-hal)

Firmware for the second board: GPIO levels, pulses, frequency and duty
measurement, and an I2C target, driven over USB serial
(`/dev/cu.usbmodempintest1`). Each command is one line, and every reply
ends with a line `ok` or `err WHY`.

| Command | Does |
|---|---|
| `id` | prints `pintester` |
| `out P V` | GP`P` an output at `V` (0 or 1) |
| `in P [u/d/n]` | GP`P` an input with a pull-up, a pull-down or neither (the default); prints `P=V` |
| `get P` | GP`P`'s level, `P=V` |
| `pulse P N US` | toggles output GP`P` `N` times, `US` microseconds apart |
| `meas P MS` | watches GP`P` for `MS` ms: `rises=R falls=F hz=H duty=D` (duty in %) |
| `regs` | the I2C target's first 16 registers |
| `i2c` | the I2C target's counters |
| `bootsel` | reboots into USB BOOTSEL |

The I2C target is always on: I2C1 on GP14 (SDA) and GP15 (SCL), address
0x42, internal pull-ups. It is a 256-byte register file like a 24C02: a
write's first byte sets the register pointer and the rest are stored from
there, and a read returns bytes from the pointer. Opening the port at 1200
baud and closing it also reboots into BOOTSEL.

```sh
cd pintester && cargo build --release
picotool load --ser SERIAL -v -x -t elf target/riscv32imac-unknown-none-elf/release/pintester
```

`debugprobe-v2.3.1-backup.uf2` is the firmware that board had before
(Raspberry Pi's debugprobe 2.3.1, read back with picotool); put it back with
`picotool load --ser SERIAL -v -x debugprobe-v2.3.1-backup.uf2`.
It is Raspberry Pi's code and others' (MIT, BSD-3-Clause, Apache-2.0), not
this project's: see `pintester/debugprobe-v2.3.1-backup.NOTICE.txt`.

### Wiring

The two boards share a ground. The tester's pin for each wire is in the
header of `tools/pin-test.py`, which is the reference.

| Linux board | Goes to | Used for |
|---|---|---|
| GP13 | a tester input | GPIO output; PIO output (square waves, ws2812, MicroPython blink) |
| GP14 | a tester output | GPIO input and edge events; PIO edge capture |
| GP15 | a tester input | PWM (sysfs and MicroPython) |
| GP3 | a tester pin | the pull-up test |
| GP4, GP5 | tester GP14, GP15 | I2C0 SDA, SCL (the tester is the target at 0x42) |
| GP19 | GP16, on the Linux board | SPI0 loopback, MOSI to MISO (a jumper) |

### Running

```
$ rp2350/tools/pin-test.py [--host ADDR] [--only NAME,...]
```

- The Linux board is reached by `ssh root@ADDR`, with key login (see the
  top-level README's [SSH](../README.md#ssh)). Without `--host`, the
  script asks the board's console for `eth0`'s address with
  `board-cmd.py`, so the console port must be free (close `screen`).
- The tester is the first `/dev/cu.usbmodempintest*`.
- First it checks that the tester answers, and builds two helpers on the
  board with tcc (a GPIO v2 edge counter and toggler, and an SPI loopback
  test). Then it runs the sections, all of them or the ones `--only`
  names, and prints a `PASS` or `FAIL` line for each check (`INFO` lines
  are reported only).

| Section | Checks |
|---|---|
| `gpio` | GP13 driven 1/0/1/0 with `gpioset`, read by the tester; GP14 driven by the tester, read with `gpioget` |
| `pull` | GP3's pull-up (`gpiomon -b pull-up`) raises the line once the tester lets go of it |
| `edges` | GP14 edge events through the GPIO v2 uAPI: all 40 at 20 ms and all 100 at 5 ms, rising and falling in turn. 200 at 2 ms and at 1 ms, and GP13 toggled every 25 ms, are reported only (`INFO`). |
| `pwm` | GP15 by sysfs at 1 kHz 25%, 10 kHz 50%, 100 Hz 75% and 1 MHz 50%, measured within 1% and 2 points of duty; then off |
| `i2c` | `i2cdetect` finds 0x42; `i2cset`/`i2cget` bytes and words; `i2ctransfer` with a repeated start; an absent address fails; MicroPython `I2C` |
| `spi` | SPI0 loopback in modes 0-3, at 100 kHz to 50 MHz, 1 to 4096 bytes; MicroPython `SPI` at 10 MHz |
| `micropython` | `Pin` output, `Pin.irq` catching 20 edges at 20 ms, `PWM` at 2 kHz 25% |
| `pio-self` | `guest/tests/pio` on the board, with no wiring of its own (it uses free pins GP15, GP20, GP21, GP31, GP33): libpio and MicroPython `rp2` at GPIOBASE 0 and 16, read back by the other block or driven from Linux, `WAIT GPIO`'s index, outputs reset between programs, the driver's refusals, and DMA (echo, a stop ending a wait, starts racing stops). The same files run in the Mac emulator by `scripts/check.sh emu`. |
| `pio` | builds the PIO examples on the board (tcc and `pioasm`); `squarewave` on GP13 at 1 kHz, 100 kHz and 1 MHz, within 1% and 50 ± 3% duty; `ws2812` sends 240 bits for 10 LEDs; `pio-edges` catches all of 200 edges at 1 ms and of 2000 at 20 µs and at 5 µs on GP14, with the right mean spacing; `pio-fifo`; `pio-dma` (echo and ring); MicroPython `pio_blink.py` at 23.7 Hz, `pio_echo.py`, `pio_ring.py` |

Pull-downs aren't tested: on the RP2350 a line that has been high stays
near 2 V against the internal pull-down (erratum RP2350-E9).

On the board, the whole run passed 44 of 44 before `pio-self` was added,
and the PIO section alone
(`--only pio`, 16 checks with the three common ones) passed 16 of 16
three more times in a row. The PIO section measured:

- the square waves within 0.001% at 1 kHz, 100 kHz and 1 MHz
- every edge captured down to 5 µs apart
- the DMA echo at about 18M words a second

## psramtest/ (Arm, C, Pico SDK)

PSRAM bring-up, an 8 MB pattern test, and timing on the Cortex-M33 cores.

```sh
cd psramtest
PICO_SDK_PATH=~/pico/pico-sdk PICO_TOOLCHAIN_PATH=~/git/toolchains/arm-gcc \
  cmake -S . -B build && cmake --build build -j8
./run.sh build/psramtest348_div3.elf      # probe-rs run --chip RP235x + RTT
```

Targets:
- `psramtest`: 150 MHz.
- `psramtest300`: 300 MHz, 1.20 V.
- `psramtest348_{1200,1300}`: 348 MHz.
- `psramtest348_div3`: 348 MHz, PSRAM divider 3 = 116 MHz.
- `psramsoak348_div3`: a 60 s soak.

## psramtest-rs/ (RISC-V, Rust, rp235x-hal)

The same test on the Hazard3 cores. The default feature `overclock` gives
348 MHz at 1.20 V with the PSRAM at 116 MHz; `--no-default-features` gives
stock 150 MHz.

```sh
cd psramtest-rs
cargo build --release
./run.sh            # flash + RTT via probe-rs (RP235x_riscv)
./run-openocd.sh    # same via Raspberry Pi OpenOCD + ../tools/rtt.py
```

### Results (8 MB, 0 errors in every run)

| Core | Clock | PSRAM SCK | Seq write (uncached) | Seq read | Random miss | Soak |
|---|---|---|---|---|---|---|
| M33 | 150 MHz | 75 MHz | 23.7 MB/s | ~18 MB/s | ~500 ns | 1 round |
| M33 | 300 MHz | 100 MHz | 37.0 MB/s | ~29 MB/s | 363 ns | 5 rounds |
| M33 | 348 MHz | 87 MHz | 35.2 MB/s | ~28 MB/s | 402 ns | 5 rounds |
| M33 | 348 MHz | 116 MHz | 43.0 MB/s | ~34 MB/s | 316 ns | 5 rounds |
| Hazard3 | 150 MHz | 75 MHz | 24.1 MB/s | ~21 MB/s | 513 ns | 60 s, 504 MB |
| Hazard3 | 348 MHz | 116 MHz | (not recorded) | | 321 ns | 60 s, 888 MB |

## Debug notes

- **Choosing the probe-rs chip.** Once the bootrom boots a RISC-V image, the
  Arm APs return FAULT, so `--chip RP235x` stops working. Use
  `--chip RP235x_riscv`.
- **probe-rs from master.** The RISC-V-mode flash algorithm (#4565) is only
  on master, so build it separately:
  `cargo install probe-rs-tools --git https://github.com/probe-rs/probe-rs --locked --root ~/git/toolchains/probe-rs-master`.
  The scripts use that build (override with `PROBE_RS=`).
- **SWD is unreliable.** probe-rs sometimes fails with "Error during
  transport", and retrying usually works. SWD flash writes to the RISC-V
  target are slow (about 13 KB/s at `--speed 10000`), which is why guests
  load over USB.
- **OpenOCD has no RTT for RISC-V.** OpenOCD (upstream and Raspberry Pi's
  fork) registers its `rtt` commands only for Cortex-M, HLA and ARC targets,
  so `tools/rtt.py` polls the RTT control block over the Tcl port instead.
  - Force `riscv set_mem_access sysbus`: the default program-buffer access
    halts the hart for every read.
  - Byte-wide `read_memory` crashes that build.
- **QMI and SRAM code.** Code that touches QMI direct mode or M0 timing runs
  from SRAM (`.data.ram_func`), and must read the register back before
  returning. Hazard3's instruction fetch can reach the QMI ahead of the
  store that leaves direct mode, and that fetch faults.
- **A wedged hart.** If the hart wedges (for example on a bus access that
  never completes), the RISC-V debug module times out ("Timeout during DMI
  access"), and neither OpenOCD nor probe-rs can halt it. To recover:
  1. Reset through the RP-AP with OpenOCD's `-f target/rp2350-rescue.cfg`.
  2. Flash through the Arm side (`probe-rs download --chip RP235x ...`,
     `probe-rs reset --chip RP235x`).
  3. The bootrom then boots the RISC-V image, and `--chip RP235x_riscv`
     works again.

  If SWD stops answering altogether, only a power cycle helps.

### Loading over USB (BOOTSEL + picotool)

The `rvemu-rs/scripts/pico-*.sh` scripts load over the target's USB. That
runs at about 70 KB/s, bound by flash writes.

- **Entering BOOTSEL.** The bootrom's BOOTSEL mode has its own USB stack.
  - `rvemu-pico-shell` enters it on a 1200-baud touch of its console port.
  - `rvemu-pico-linux` reboots into it when it finishes.
  - Any rp2350-board firmware enters it on the SWD request above.
- **picotool types.** `picotool load` needs `-t bin` for raw images with
  unknown extensions, and `-t elf` for Cargo's extension-less ELFs.
- **RTT after a USB load.** Read it with `probe-rs attach`
  (`ATTACH=1 tools/run-riscv.sh`), but only once the debug module answers.
  Right after the reboot it times out ("Timeout during DMI access"), as it
  does while the chip sits in BOOTSEL.
- **USB data lines.** The target's USB must reach the Mac's data lines. A
  link with VBUS but no host traffic shows in USBCTRL SIE_STATUS
  (0x50110050) as VBUS_DETECTED + SUSPENDED, never CONNECTED. macOS 15 also
  holds a new accessory until it is allowed.
