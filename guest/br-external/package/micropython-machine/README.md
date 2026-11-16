# micropython-machine

MicroPython's `machine` and `rp2` modules for the board's pins. Both are
written in Python over Linux devices, so programs written for a Pico mostly
run unchanged.

| File | On the board | What |
|---|---|---|
| `machine.py` | `/usr/lib/micropython/machine.mpy` | adds `Pin`, `I2C`, `SPI`, `PWM` and `ADC` to MicroPython's built-in `machine` |
| `rp2.py` | `/usr/lib/micropython/rp2.mpy` | the rp2 port's `rp2`: `asm_pio`, `PIO`, `StateMachine`, `DMA`, on `/dev/pio1` and `/dev/pio2` |
| `cmod/fcntl` | built into `micropython` | `fcntl.ioctl`, for both modules |
| `cmod/rvmem` | built into `micropython` | `_rvmem`, which maps a device into the program and reads and writes 32-bit registers there |
| `examples/pio_*.py` | `/usr/share/examples/pio` | `rp2` examples |

Both modules are compiled to `.mpy` with `mpy-cross` at build time, so an
import doesn't parse the source. The C modules are compiled into the
MicroPython binary (`USER_C_MODULES`).

## machine

| Class | Linux device | Notes |
|---|---|---|
| `Pin(n, mode, pull, value=)` | `/dev/gpiochip0` (GPIO v2) | `n` is a GPIO number, `"GP15"` or a `Pin`. `Pin.IN`, `OUT`, `OPEN_DRAIN`; `PULL_UP`, `PULL_DOWN`. Two `Pin` objects for one number share the line. `irq(handler, trigger)` runs the handler in a thread of its own for each edge. |
| `I2C(0 or 1)` | `/dev/i2c-0`, `/dev/i2c-1` | `scan`, `readfrom(_into)`, `writeto`, `writevto`, `readfrom_mem(_into)`, `writeto_mem` |
| `SPI(0, baudrate=...)` | `/dev/spidev0.0` | `read`, `readinto`, `write`, `write_readinto` |
| `PWM(Pin, freq=, duty_u16=)` | `/sys/class/pwm/pwmchip0` | `freq`, `duty_u16`, `duty_ns`; pins on one PWM slice share its period |
| `ADC(Pin(40))` ... `ADC(Pin(47))`, `ADC(ADC.CORE_TEMP)` | IIO | `read_u16`, `read_uv`, `read_raw`, and `read_temp()` for the temperature sensor |

The pin map and examples are in the
[top-level README](../../../../README.md#gpio-i2c-spi-pwm-adc).

## rp2

The rp2 port's `rp2` module on PIO1 and PIO2: the same assembler
(`@rp2.asm_pio`), `rp2.PIO`, `rp2.StateMachine` and `rp2.DMA`. PIO0 drives
the board's Wi-Fi radio and is not offered. `rp2.py` maps a block's
registers into the program with `_rvmem` and drives them directly, as
the port does. The unix port's `machine.mem32` can't do this: it goes
through `/dev/mem`, which this kernel doesn't have.

```python
import rp2
from machine import Pin

@rp2.asm_pio(set_init=rp2.PIO.OUT_LOW)
def blink():
    wrap_target()
    set(pins, 1) [31]
    set(pins, 0) [31]
    wrap()

sm = rp2.StateMachine(4, blink, freq=100_000, set_base=Pin(15))   # PIO1, machine 0
sm.active(1)
```

### State machines 4-11

State machines are numbered as on the Pico 2, where each block has four:

| Number | Block | Same as |
|---|---|---|
| 0-3 | PIO0: refused (`ValueError: PIO0 is the board's radio: use PIO 1 or 2`) | |
| 4-7 | PIO1, machines 0-3 | `rp2.PIO(1).state_machine(0)` ... `(3)` |
| 8-11 | PIO2, machines 0-3 | `rp2.PIO(2).state_machine(0)` ... `(3)` |

A Pico program that uses state machines 0-3 needs them renumbered to 4-7.
`rp2.PIO(0)` is refused the same way.

### Pins

- **Which pins a StateMachine takes.** `StateMachine(...)` (or `init`)
  hands the block the pins the program sets up: the `out_init`,
  `set_init` and `sideset_init` pins of `@rp2.asm_pio`, counted from
  `out_base`, `set_base` and `sideset_base`. It drives each one to the
  level and direction its `PIO.OUT_LOW` / `OUT_HIGH` / `IN_LOW` /
  `IN_HIGH` gives. `in_base` and `jmp_pin` are only read, and a block
  reads every pin without taking it.
- **A pin handed to a StateMachine leaves `machine.Pin`.** As on the
  rp2 port, the pin stops being a GPIO. `rp2` closes the GPIO line a `Pin`
  object held for it, so that object stops working, and a new
  `Pin(n)` for it fails with `OSError` (`EBUSY`). The pin stays with the
  block until the program exits.
- **Pins in use elsewhere.** A pin another program holds (a `gpioset`, a
  C program, the other block) makes the StateMachine fail with `OSError`
  (`EBUSY`). The board's own pins, such as the SD card's and the radio's,
  fail with `EINVAL`.
- **GPIO base.** Pins are GPIO numbers 0-47, and `rp2` writes each as its
  index from the block's base. One state machine's pins must all be in
  0-31 or all in 16-47 (`ValueError` otherwise). The block's four machines
  share the base, so it can't change while another of them is running or
  has pins set for the current base (`ValueError`); a machine with no pins
  leaves it alone. `PIO.gpio_base()` reads it, and `PIO.gpio_base(16)`
  sets it. In the assembler, `wait(1, gpio, N)` takes an index from the
  base, not a GPIO number (at base 16, `N` 15 is GP31); `wait(1, pin, N)`
  counts from `in_base`.

### Differences from the rp2 port

- **State machines are 4-11**, as above, and `PIO(1)`, `PIO(2)` only.
- **One program per block.** The first `PIO` or `StateMachine` to use a
  block opens it, and another program that has it open makes that fail
  with `OSError` (`EBUSY`). The block is reset when the program opens it
  and again when it exits: state machines stopped, instruction memory
  cleared, pins given back.
- **`get()` and `put()`** check the FIFO a few times, then sleep on the
  block's interrupt until there is data or room, instead of spinning.
- **IRQ handlers run in a thread.** `PIO.irq(handler, trigger)` calls
  `handler(pio)` for the IRQ flags in `trigger` (`PIO.IRQ_SM0` ...
  `IRQ_SM3`, flag N is `0x100 << N`; default all four).
  `StateMachine.irq(handler)` calls `handler(sm)` for flag N, where N is
  the machine's number within its block (the flag `irq(rel(0))` raises).
  `hard=` is accepted and ignored. The handler runs in a thread of
  `rp2`'s, and `rp2` clears the flag before calling it.
- **`freq`** is converted to a clock divider of the block's 348 MHz
  clock, so it must be between 348 MHz / 65536 (about 5.3 kHz) and
  348 MHz. Anything else raises `ValueError`. `freq=-1`, the default,
  runs at full speed.
- **The assembler is the port's own**, which knows the RP2040's
  instructions, plus the RP2350's `mov(pindirs, src)`. The other RP2350
  additions (`wait jmppin`, `irq prev`/`next`, `mov rxfifo[...]`) aren't in
  it. Emit them with `word()`, or use C and [libpio](../libpio/README.md),
  whose assembler has them.
- **DMA** moves data only between a buffer and a state machine, through
  a buffer the kernel gave the program (below).
- **Not here:** `rp2.Flash`, `rp2.bootsel_button`, `rp2.country`, and
  `DMA.registers`.

### DMA

`rp2.DMA` moves data between a buffer and a state machine's FIFO with
none of the program's time, on the board's pool of four DMA channels.
The pool is shared by both blocks and every program.

```python
rx = rp2.DMA()
rx.config(read=sm, write=buf, count=n, ctrl=rx.pack_ctrl(size=2), trigger=True)   # sm's RX FIFO into buf
tx = rp2.DMA()
tx.config(read=buf2, write=sm, count=n, ctrl=tx.pack_ctrl(size=2), trigger=True)  # buf2 into sm's TX FIFO
tx.wait(2000)
rx.wait(2000)
rx.close()
tx.close()
```

| Call | Does |
|---|---|
| `DMA()` | a channel object. The channel itself is taken on the first `active(1)`. |
| `config(read=, write=, count=, ctrl=, trigger=False)` | one of `read`/`write` is a `StateMachine` and the other a buffer (`bytearray`, `array`, ...): `read=sm` moves its RX FIFO into the buffer, and `write=sm` moves the buffer into its TX FIFO. `count` is the number of transfers. `trigger=True` starts it. |
| `pack_ctrl(size=2, ring_size=0)` | `size` 0, 1 or 2: bytes, halfwords or words. `ring_size` N (up to 12): the buffer side wraps within 2^N bytes. The port's other fields (`inc_read`, `inc_write`, `treq_sel`, `ring_sel`, ...) are accepted and ignored: the buffer side always moves, and the FIFO side and its pace are the state machine's. |
| `unpack_ctrl(ctrl)` | the fields as a dict |
| `active(1)` / `active(0)` / `active()` | start it (again), stop it, or whether it is still going |
| `wait(timeout_ms=0)` | wait until it is done (0: no limit). The rp2 port doesn't have this. |
| `irq(handler)` | after `active(1)`: `handler(dma)` runs in a thread once the transfer is done |
| `close()` | stop it, give the channel back, free its part of the buffer |

- **Data goes through the kernel's buffer.** A block's DMA buffer is
  64 KiB the kernel allocates at the program's first transfer on that
  block. Each `DMA` object takes a 4 KiB-aligned part of it (`OSError`
  `ENOMEM` once it is full). A buffer to send is copied in when the
  transfer starts. Received data is copied into your buffer when `rp2`
  sees the transfer done: when `active()` returns `False`, in `wait()`,
  or in `active(0)` and `close()`. Until then your buffer doesn't change.
- **Endless.** As on the chip, a `count` with 0xF in its top four bits
  (`0xF000_0000`) is TRANS_COUNT's endless mode. It needs a ring
  (`ring_size`). The channel runs until `active(0)`, which also copies the
  ring into your buffer.
- **Channels.** `active(1)` fails with `OSError` (`EBUSY`) when all four
  are in use. A finished transfer keeps its channel, so `active(1)`
  starts it again. `close()` gives it back, and so does the program's
  exit.
- **Checks.** The kernel and the firmware check each transfer, so a
  channel can reach only the program's buffer and its block's FIFOs. A
  setup they refuse (`count` 0, a `ring_size` above 12, endless without a
  ring) raises `OSError` (`EINVAL`), and a bus error raises `OSError`
  (`EIO`).

### Examples

In `/usr/share/examples/pio`. On the board,
`rp2350/tools/pin-test.py` runs all three against a second board.

`test/test_rp2_asm.py` checks the assembler against libpio's, in CPython
on the host (`scripts/check.sh host`), and `guest/tests/pio/rp2_selftest.py`
checks `rp2` on a real block (`scripts/check.sh emu`, and
`pin-test.py --only pio-self` on the board).

| Example | Run | Does |
|---|---|---|
| `pio_blink.py` | `micropython pio_blink.py [PIN]` | blinks PIN (default 14) for 3 s, its timing all in PIO1: each half period is 2112 cycles of a 100 kHz state machine clock, about 23.7 Hz |
| `pio_echo.py` | `micropython pio_echo.py` | no pins: a state machine sends each word back inverted, by `put`/`get` and then 256 words by DMA |
| `pio_ring.py` | `micropython pio_ring.py` | a state machine pushes a count-down, and an endless DMA channel keeps the last 16 values in a 64-byte ring |

```
# micropython /usr/share/examples/pio/pio_echo.py
put/get: 4 of 4 right
DMA: 256 of 256 right
# micropython /usr/share/examples/pio/pio_ring.py
running after 0.5 s: True
ring: 15 of 16 steps of 1
```

The ring holds 15 steps of one because the oldest and newest values meet
at one place in it.

### On the Mac

The emulator's `-G` simulates both blocks and the DMA pool
(`rvemu-rs/rvemu/src/piosim.rs`). There the blocks report a 150 MHz clock,
so `freq` sets the divider for 150 MHz, but they run `RVEMU_PIO_HZ` cycles
a second (default 10 MHz). Programs behave as on the board, only more
slowly.

## _rvmem

A small C module, for `rp2.py`:

| Function | Does |
|---|---|
| `_rvmem.mmap(fd, length, offset=0)` | `mmap` a device file read-write and shared; returns the address |
| `_rvmem.munmap(address, length)` | unmap it |
| `_rvmem.read32(address)`, `_rvmem.write32(address, value)` | one aligned 32-bit load or store (`ValueError` if unaligned) |
