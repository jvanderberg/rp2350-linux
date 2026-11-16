# libpio

The RP2350's PIO1 and PIO2 for C programs on the board. PIO0 drives the
Wi-Fi radio and is not offered.

| On the board | What |
|---|---|
| `/usr/include/pio.h` | the API: the pico-sdk's `hardware/pio.h` functions, plus `pio_open`, the run-time assembler, `pio_wait_irq` and DMA |
| `/usr/lib/libpio.so` | the library (`cc ... -lpio`) |
| `/usr/bin/pioasm` | a `.pio` file to a C header, as the pico-sdk's `pioasm` does it |
| `/usr/share/examples/pio` | `squarewave.c`, `pio-fifo.c`, `pio-dma.c`, `pio-edges.c`, `ws2812.c` with `ws2812.pio`, and MicroPython's `pio_*.py` ([MicroPython `rp2`](../micropython-machine/README.md)) |

libpio sits on the kernel driver `pio-rvemu.c` (`/dev/pio1`, `/dev/pio2`,
uapi `rvemu-pio.h`, both in `guest/linux/drivers/rvemu`). Opening a block
maps its registers into the program, so a register access is one load or
store, with no system call. The emulator passes it to the chip's PIO1 or
PIO2, which run at the chip's speed. The block's own code runs there; only
the program's register accesses go through the emulator.

## Getting started

Everything here runs on the board, with tcc (`cc`). Lines starting with `#`
are in the board's shell.

**1. A program assembled at run time, and the FIFOs.** This state machine
reads a word from its TX FIFO, adds one to it and pushes the result to its
RX FIFO. No pins are involved.

```c
/* add-one.c */
#include <pio.h>
#include <stdio.h>

static const char *src =
    ".program add_one\n"
    "    pull\n"
    "    mov x, ~osr\n"   /* x = ~v */
    "    jmp x-- done\n"  /* x = ~v - 1 = ~(v + 1) */
    "done:\n"
    "    mov isr, ~x\n"   /* v + 1 */
    "    push\n";

int main(void)
{
    char err[128];
    pio_asm_program_t p;
    PIO pio = pio_open(1);
    if (!pio) {
        perror("pio_open");
        return 1;
    }
    if (pio_assemble(src, NULL, &p, err, sizeof err) < 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    int off = pio_add_program(pio, &p.program);
    pio_sm_config c = pio_asm_default_config(&p, off);
    pio_sm_init(pio, 0, off, &c);
    pio_sm_set_enabled(pio, 0, true);
    for (uint32_t v = 41; v < 44; v++) {
        pio_sm_put_blocking(pio, 0, v);
        printf("%u -> %u\n", v, pio_sm_get_blocking(pio, 0));
    }
    pio_close(pio);
    return 0;
}
```

```
# cc -o add-one add-one.c -lpio && ./add-one
41 -> 42
42 -> 43
43 -> 44
```

**2. A pin.** `squarewave` makes a square wave from two `set` instructions,
so the PIO clock divider gives the frequency. The program claims the pin
with `pio_gpio_init`. Until it exits, `gpioinfo` shows that line as used
by `pio`.

```
# mkdir /tmp/p && cd /tmp/p && cp /usr/share/examples/pio/* .
# cc -o squarewave squarewave.c -lpio
# ./squarewave 15 100000 5          # GP15, 100 kHz, 5 s
GP15: 100000.000 Hz (clock 348000000 Hz, divider 54.3750, delay 31)
```

On the board `rp2350/tools/pin-test.py` measured this example within
0.001% of 1 kHz, 100 kHz and 1 MHz.

**3. A `.pio` file.** `pioasm` turns a `.pio` file into a header in the
pico-sdk's format, including the file's `% c-sdk { ... %}` block, so a
pico-sdk example builds with only the changes listed under
[Differences from the pico-sdk](#differences-from-the-pico-sdk):

```
# pioasm ws2812.pio ws2812.pio.h
# cc -o ws2812 ws2812.c -lpio
# ./ws2812 15 8 ff0000               # 8 LEDs on GP15, all red
```

## Blocks and pins

| Call | Does |
|---|---|
| `PIO pio_open(int block)` | open PIO1 or PIO2 (`block` 1 or 2) and map its registers. The block starts reset. NULL with `errno`: `EBUSY` when another program has it, `EINVAL` for another number. In the same program, a second call returns the same `PIO`. |
| `void pio_close(PIO)` | stop its DMA, reset the block, give its pins back and unmap it. Exiting does the same. |
| `uint pio_get_index(PIO)` | 1 or 2 |
| `uint32_t pio_clock_hz(PIO)` | the block's clock: clk_sys, 348 MHz on the board |
| `uint32_t clock_get_hz(clk_sys)` | the same, for SDK code: the clock of the last block opened |
| `int pio_gpio_init(PIO, uint pin)` | take GPIO `pin` (0-47) from Linux for this block. 0, or -1 with `errno`: `EBUSY` (a GPIO user or the other block has it), `EINVAL` (the board uses that pin for something else, such as the SD card or the radio). A pin running PWM is taken from PWM, which stops. |
| `int pio_gpio_release(PIO, uint pin)` | give it back to its GPIO line |
| `int pio_set_gpio_base(PIO, uint base)` / `uint pio_get_gpio_base(PIO)` | GPIOBASE, 0 or 16: the first of the 32 GPIOs the block reaches |
| `int pio_fd(PIO)` | the block's file descriptor, for `poll(2)` |

- **One program per block.** Opening is exclusive. Opening and closing
  reset the block: its state machines are stopped, its FIFOs emptied and
  its instruction memory cleared. Its pins' output levels and directions
  are cleared too, so a program never starts out driving a pin the last
  one left high.
- **Pins.** The block can drive only the pins it has claimed. Its inputs
  (`in pins`, `wait gpio`, `jmp pin`) see every pin, claimed or not. The
  free pins are the ones `gpioinfo` names `GPn`, as listed in the
  [top-level README](../../../../README.md#gpio-i2c-spi-pwm-adc).
- **GPIOBASE.** Pins are GPIO numbers. A block reaches 32 consecutive
  GPIOs: 0-31 (GPIOBASE 0) or 16-47 (GPIOBASE 16). `pio_sm_set_config` and
  `pio_sm_init` pick the base a configuration's pins need. They fail with
  `EINVAL` when the pins don't fit in one window, or when the block would
  have to change base while one of its state machines is running.

## Programs

| Call | Does |
|---|---|
| `int pio_add_program(PIO, const pio_program_t *)` | load a program where it fits, searching from the top of instruction memory as the SDK does, or at its `origin`. Returns the offset, or -1 (`ENOSPC`). The program's JMP targets are moved by the offset. |
| `int pio_add_program_at_offset(PIO, prog, uint offset)` | at that offset, or -1 (`ENOSPC`) |
| `bool pio_can_add_program(PIO, prog)`, `pio_can_add_program_at_offset(...)` | whether it fits |
| `void pio_remove_program(PIO, prog, uint offset)` | mark its instructions free (they stay in memory until overwritten) |
| `void pio_clear_instruction_memory(PIO)` | every slot `jmp` to itself, all free |
| `int pio_assemble(const char *src, const char *name, pio_asm_program_t *out, char *err, size_t errlen)` | assemble program `name` of `src` (NULL: the first one). 0, or -1 with a message such as `line 2: unknown symbol 'nowhere'` in `err`. [The assembler](#the-assembler) |
| `pio_sm_config pio_asm_default_config(const pio_asm_program_t *, uint offset)` | the program's default configuration at `offset`: what pioasm's `NAME_program_get_default_config` returns |
| `bool pio_asm_define(const pio_asm_program_t *, const char *name, int *value)` | a `.define public` value, or a public label's offset as `offset_LABEL` |

`pio_asm_program_t` holds the instructions, a `pio_program_t` (`.program`)
pointing at them, the wrap points, side-set settings, the `.in`, `.out`,
`.set`, `.fifo`, `.mov_status` and `.clock_div` settings, and up to 16
public defines.

## State machine configuration

These are the SDK's functions, with the same arguments. Pin arguments are
GPIO numbers (see GPIOBASE above).

| Area | Calls |
|---|---|
| defaults | `pio_get_default_sm_config()`: clock divider 1, wrap 0-31, both shifts right with no autopush or autopull, thresholds 32 |
| pins | `sm_config_set_out_pins(c, base, count)`, `sm_config_set_set_pins(c, base, count)`, `sm_config_set_in_pins(c, base)`, `sm_config_set_sideset_pins(c, base)`, `sm_config_set_jmp_pin(c, pin)`; the `_pin_base` / `_pin_count` variants of each, and `sm_config_set_in_pin_count` |
| side-set | `sm_config_set_sideset(c, bit_count, optional, pindirs)` (`bit_count` includes the enable bit) |
| clock | `sm_config_set_clkdiv(c, float div)` (1 to 65536), `sm_config_set_clkdiv_int_frac8(c, int, frac8)`, `sm_config_set_clkdiv_int_frac(c, int, frac)` |
| wrap | `sm_config_set_wrap(c, wrap_target, wrap)` |
| shifts | `sm_config_set_in_shift(c, shift_right, autopush, threshold)`, `sm_config_set_out_shift(c, shift_right, autopull, threshold)` |
| FIFOs | `sm_config_set_fifo_join(c, PIO_FIFO_JOIN_NONE / _TX / _RX / _TXGET / _TXPUT / _PUTGET)` |
| other | `sm_config_set_out_special(c, sticky, has_enable_pin, enable_pin_index)`, `sm_config_set_mov_status(c, STATUS_TX_LESSTHAN / STATUS_RX_LESSTHAN / STATUS_IRQ_SET, n)` |

The slowest state machine clock is 348 MHz / 65536, about 5.3 kHz.

## State machines

| Area | Calls |
|---|---|
| setup | `int pio_sm_init(PIO, sm, initial_pc, const pio_sm_config *)`: stop the machine, apply the configuration (NULL: the default), empty its FIFOs, clear its FIFO debug flags, restart it and its clock divider, and jump to `initial_pc`. It doesn't start it. `int pio_sm_set_config(PIO, sm, const pio_sm_config *)` applies a configuration alone. Both return 0 or -1. |
| run | `pio_sm_set_enabled(PIO, sm, bool)`, `pio_set_sm_mask_enabled(PIO, mask, bool)`, `pio_enable_sm_mask_in_sync(PIO, mask)`, `pio_sm_restart`, `pio_restart_sm_mask`, `pio_sm_clkdiv_restart`, `pio_clkdiv_restart_sm_mask` |
| change while running | `pio_sm_set_wrap`, `pio_sm_set_out_pins`, `pio_sm_set_set_pins`, `pio_sm_set_in_pins`, `pio_sm_set_sideset_pins`, `pio_sm_set_jmp_pin`, `pio_sm_set_clkdiv`, `pio_sm_set_clkdiv_int_frac8`, `pio_sm_set_clkdiv_int_frac` |
| instructions | `pio_sm_exec(PIO, sm, instr)`, `pio_sm_exec_wait_blocking`, `pio_sm_is_exec_stalled`, `pio_sm_get_pc`, and the `pio_encode_*` functions (`pio_encode_jmp`, `pio_encode_set(pio_pins, 1)`, `pio_encode_mov`, `pio_encode_sideset`, `pio_encode_delay`, ...) |
| pin levels and directions | `pio_sm_set_pins(PIO, sm, uint64_t values)`, `pio_sm_set_pins_with_mask(PIO, sm, values, mask)`, `pio_sm_set_pindirs_with_mask(PIO, sm, dirs, mask)`, `int pio_sm_set_consecutive_pindirs(PIO, sm, pin, count, is_out)`. They run `set` instructions on the state machine, one pin at a time, as the SDK does, so stop it first. Masks are 64-bit: bit N is GPN. Only pins in the block's current GPIO window are set. |
| FIFOs | `pio_sm_put`, `pio_sm_get` (no wait), `pio_sm_put_blocking`, `pio_sm_get_blocking`, `pio_sm_is_tx_fifo_full` / `_empty`, `pio_sm_is_rx_fifo_full` / `_empty`, `pio_sm_get_tx_fifo_level`, `pio_sm_get_rx_fifo_level`, `pio_sm_clear_fifos`, `pio_sm_drain_tx_fifo` |
| claiming | `pio_sm_claim`, `pio_claim_sm_mask`, `pio_sm_unclaim`, `pio_sm_is_claimed`, `pio_claim_unused_sm(PIO, required)`, which returns the machine or -1. With `required`, it prints a message and exits if none is free. Claims are kept within the program. |

`pio_sm_put_blocking` and `pio_sm_get_blocking` check the FIFO a few times,
then sleep on the block's interrupt until there is room or data, instead
of spinning. They enable that FIFO's interrupt source only while they
wait.

## Interrupts

Linux sees the block's IRQ0 output, not IRQ1.

| Call | Does |
|---|---|
| `pio_set_irq0_source_enabled(PIO, source, bool)`, `pio_set_irq0_source_mask_enabled(PIO, mask, bool)` | choose what raises IRQ0: `pis_sm0_rx_fifo_not_empty` ... `pis_sm3_tx_fifo_not_full`, `pis_interrupt0` ... `pis_interrupt7` |
| `int64_t pio_wait_irq(PIO, int timeout_ms)` | sleep until IRQ0 is up and return IRQ0_INTS: 0 on timeout, -1 on error. `timeout_ms` -1 waits without limit; 0 only looks. |
| `pio_interrupt_get(PIO, n)`, `pio_interrupt_clear(PIO, n)` | the block's IRQ flags 0-7 |
| `poll(2)` on `pio_fd(PIO)` | `POLLIN` while IRQ0 is up |

IRQ0 is a level. The program clears its cause (`pio_interrupt_clear`, or
reading the FIFO) itself, or `pio_wait_irq` returns again at once.

## DMA

The board keeps four of the chip's DMA channels for PIO: channels 12-15,
shared by both blocks and every program. Each channel moves data between
the program's DMA buffer and one state machine's FIFO, paced by that FIFO,
with no CPU involved. Before a transfer starts, the kernel checks the
request and the firmware checks it again: the buffer has to be the
program's, and the FIFO has to belong to its block.

| Call | Does |
|---|---|
| `void *pio_dma_buffer(PIO, size_t size)` | the block's DMA buffer, mapped: `size` rounded up to a page, at most 64 KiB, zeroed. There is one buffer per open block: later calls return the same buffer, or fail with `EINVAL` if they ask for more. NULL with `errno` on failure. |
| `int pio_dma_start(PIO, int channel, const pio_dma_config *)` | start a transfer on a new channel (`channel` -1) or on one of yours again. Returns the channel (0-3), or -1: `EBUSY` when all four are in use, `EINVAL` for a bad configuration, `ENOMEM` when there is no buffer yet. |
| `int pio_dma_wait(PIO, int channel, int timeout_ms)` | wait until it is done (`timeout_ms` 0: no limit). 0, or -1: `ETIMEDOUT`, or `EIO` when it stopped on a bus error |
| `int pio_dma_status(PIO, int channel, uint32_t *remaining, uint32_t *pos)` | 1 still going, 0 done, -1 (`EIO`) bus error; the transfers left, and how far past `offset` the buffer side is, in bytes (either pointer may be NULL) |
| `int pio_dma_stop(PIO, int channel)` | stop it and give the channel back |
| `int pio_sm_dma_write(PIO, sm, const void *data, size_t bytes, uint size)` | copy `data` to the state machine's TX FIFO by DMA, `size` bytes a transfer, through the DMA buffer, and wait until it is done. 0 or -1. |
| `int pio_sm_dma_read(PIO, sm, void *data, size_t bytes, uint size)` | the same from its RX FIFO into `data` |

```c
typedef struct {
    uint sm;          /* 0-3, of this block */
    bool from_sm;     /* RX FIFO into the buffer; else the buffer to the TX FIFO */
    uint size;        /* bytes a transfer: 1, 2 or 4 */
    size_t offset;    /* where in the buffer: a multiple of size (and of the ring) */
    uint32_t count;   /* transfers: 1 to 2^28 - 1 */
    uint ring_bits;   /* 0, or the buffer side wraps within 1 << ring_bits bytes */
    bool endless;     /* never ends (ring only) */
} pio_dma_config;
```

- **A channel is yours until you stop it.** A finished transfer still
  holds its channel. Start it again with `pio_dma_start(pio, ch, &c)`, and
  give it back with `pio_dma_stop`. `pio_close` and exit stop and give
  back every channel the block has.
- **Limits.** Transfers of 1, 2 or 4 bytes, from 1 to 2^28 - 1 of them. A
  plain transfer has to fit in the buffer: `offset + count * size` at most
  the buffer's size. A narrow write to a TX FIFO is copied across the
  word, as on the chip.
- **Rings.** With `ring_bits` N (from log2 of `size` up to 12), the
  buffer side wraps within 2^N bytes, at most 4 KiB, starting at `offset`.
  `offset` has to be a multiple of 2^N, and the ring has to fit in the
  buffer. The FIFO side doesn't move. `count` still counts transfers, so a
  ring can go round many times.
- **Endless.** `endless` (with a ring) never finishes: a capture that
  keeps the latest 2^N bytes, or a pattern sent over and over. It runs until
  `pio_dma_stop`. `pos` from `pio_dma_status` is where the next word goes
  (or comes from), so the newest word of a capture is the one just before
  `pos`, wrapping round the ring.
- **`pio_sm_dma_write` and `pio_sm_dma_read`** use the start of the
  block's buffer, 16 KiB of it unless the program made a buffer first, and
  copy through it a buffer's worth at a time. Don't mix them with your own
  transfers at offset 0.
- **Memory.** The buffer comes out of the guest's 8 MiB of RAM, so ask
  for what you need.
- **Flash writes.** A flash erase or program (a write to `/home`, swap on
  flash) stops the XIP bus that guest RAM is on. The firmware pauses the
  channels for that time and resumes them afterwards, so a FIFO can fill
  or run dry meanwhile.

**Example:** an echo, then an endless ring (`pio-dma.c`, cut down).

```c
uint32_t *buf = pio_dma_buffer(pio, 65536);
/* the state machine sends each word back inverted (".wrap_target pull
 * mov isr, ~osr push .wrap"): n words out of buf[0..n), answers into
 * buf[8192..) */
pio_dma_config rx = { .sm = 0, .from_sm = true,  .size = 4, .offset = 8192 * 4, .count = n };
pio_dma_config tx = { .sm = 0, .from_sm = false, .size = 4, .offset = 0,        .count = n };
int crx = pio_dma_start(pio, -1, &rx);   /* the receiver first */
int ctx = pio_dma_start(pio, -1, &tx);
pio_dma_wait(pio, ctx, 5000);
pio_dma_wait(pio, crx, 5000);
pio_dma_stop(pio, ctx);
pio_dma_stop(pio, crx);

/* state machine 1 pushes words as fast as it can; the last 4 KiB of them
 * stay in buf[4096..5119] */
pio_dma_config ring = { .sm = 1, .from_sm = true, .size = 4, .offset = 4096 * 4,
                        .count = 1u << 27, .ring_bits = 12, .endless = true };
int cr = pio_dma_start(pio, -1, &ring);
/* ... */
uint32_t pos;
pio_dma_status(pio, cr, NULL, &pos);
uint32_t newest = buf[4096 + (pos / 4 + 1023) % 1024];
pio_dma_stop(pio, cr);
```

On the board the echo ran at about 18M words a second.

## The assembler

`pio_assemble()` and `pioasm` share one assembler. It takes the pico-sdk
pioasm's language, RP2350 (PIO version 1) instructions included.

```
pioasm [-o c-sdk] [-v VERSION] input.pio [output.h]
```

`-` or no input reads standard input, and no output writes standard
output. `-o` accepts only `c-sdk`. `-v` raises the PIO version written in
the header, never lowers it. Errors name the file and the line:
`blink.pio: line 3: set value 32 is out of range (0 to 31)`.

The header has the pico-sdk's names: `NAME_program_instructions`,
`NAME_program` (a `struct pio_program`), `NAME_wrap_target`, `NAME_wrap`,
`NAME_pio_version`, `NAME_program_get_default_config(offset)`, a
`NAME_DEFINE` for each `.define public` and a `NAME_offset_LABEL` for each
`public` label. The file's `% c-sdk { ... %}` text comes after them. It
includes `<pio.h>` in place of `hardware/pio.h`. For `ws2812.pio`:

```
#define ws2812_wrap_target 0
#define ws2812_wrap 3
#define ws2812_pio_version 0

#define ws2812_T1 2
#define ws2812_T2 5
#define ws2812_T3 3

static const uint16_t ws2812_program_instructions[] = {
            //     .wrap_target
    0x6221, //  0: out x, 1       side 0 [T3 - 1]
    0x1123, //  1: jmp !x do_zero side 1 [T1 - 1]
    0x1400, //  2: jmp  bitloop   side 1 [T2 - 1]
    0xa442, //  3: nop            side 0 [T2 - 1]
            //     .wrap
};
```

**Instructions.** Commas between operands are optional, and keywords are
case-insensitive.

| Instruction | Forms |
|---|---|
| `jmp` | `jmp TARGET`, `jmp !x`, `x--`, `!y`, `y--`, `x!=y`, `pin`, `!osre`, `TARGET` |
| `wait` | `wait 0/1 gpio N`, `pin N`, `irq N [rel]`, `irq prev N`, `irq next N`, `jmppin [+ N]`. With no polarity, 1. `gpio` is 0-31 of the block's GPIO window. |
| `in` | `in pins/x/y/null/isr/osr, 1-32` |
| `out` | `out pins/x/y/null/pindirs/pc/isr/exec, 1-32` |
| `push`, `pull` | `[iffull]` / `[ifempty]`, `[block]` (the default) / `[noblock]` |
| `mov` | `mov DEST, [!/~/::]SRC`. DEST: `pins x y pindirs exec pc isr osr`; SRC: `pins x y null status isr osr`. `!` and `~` invert, `::` reverses the bits. Also `mov rxfifo[N], isr`, `mov rxfifo[y], isr` and `mov osr, rxfifo[N]` / `rxfifo[y]` |
| `irq` | `irq [set/nowait/wait/clear] N [rel]`, `irq [prev/next] ... N` |
| `set` | `set pins/x/y/pindirs, 0-31` |
| `nop` | `mov y, y` |

After the operands, any instruction takes `side N` (or `sideset`,
`side_set`) and a delay `[N]`, in either order. A label is `NAME:` or
`public NAME:` at the start of a line. `mov pindirs`, `wait jmppin`,
`irq prev/next`, the `rxfifo` forms and `.fifo txput/txget/putget` are
RP2350-only. A program that uses them is version 1, and naming
`.pio_version 0` for it is an error.

**Directives.**

| Directive | |
|---|---|
| `.program NAME` | starts a program. A file without one is a single program called `program`. |
| `.define [public] NAME EXPR` | a symbol. Before the first `.program` it is the file's, otherwise the program's. `public`: in the header, and in `pio_asm_define` |
| `.origin N` | load only at this offset |
| `.side_set N [opt] [pindirs]` | side-set bits (N plus `opt` at most 5), before the first instruction |
| `.wrap_target`, `.wrap` | the wrap points. Without `.wrap`, the program wraps after its last instruction. |
| `.pio_version 0/1/rp2040/rp2350` | the PIO version the program needs |
| `.in N [left/right] [auto/manual] [THRESHOLD]` | IN pin count (1-32: anything but 32 needs version 1) and shift settings, for the default config |
| `.out N [left/right] [auto/manual] [THRESHOLD]` | OUT pin count (0-32) and shift settings |
| `.set N` | SET pin count (0-5) |
| `.fifo txrx/tx/rx/txput/txget/putget` | FIFO join |
| `.mov_status rxfifo < N`, `txfifo < N`, `irq [prev/next] set N` | what `mov x, status` reads |
| `.clock_div DIV` | clock divider for the default config (1 to 65536) |
| `.word VALUE` | a raw 16-bit instruction |
| `.lang_opt ...` | accepted and ignored |
| `% c-sdk {` ... `%}` | copied into the header after the program. Other languages' blocks (`% python {`, ...) are dropped. |

**Expressions** (operands, delays, `.define`): decimal, `0x` hex and `0b`
binary numbers, symbols and labels, `( )`, unary `-`, `~` (NOT) and `::`
(bit reverse), and `* /`, `+ -`, `<< >>`, `&`, `^`, `|`, from tightest to
loosest. Comments are `;`, `//` and `/* */`.

**Limits.** A program has at most 32 instructions. A file has at most 64
programs and 128 symbols. Names are cut to 31 characters. A program keeps
16 public defines (labels included); any more are left out without an
error. An expression nests at most 64 deep.

## Differences from the pico-sdk

- **Open a block first.** There are no `pio0`/`pio1`/`pio2` globals:
  `PIO pio = pio_open(1);` (or 2), and `pio_close(pio)` at the end. A `PIO`
  still points at the block's registers (`pio_hw_t`), so `pio->txf[sm]`,
  `hw_set_bits(&pio->ctrl, ...)` and the like work.
- **Errors are returned.** Where the SDK panics, libpio returns -1 with
  `errno`: `pio_add_program`, `pio_sm_init`, `pio_sm_set_config`,
  `pio_gpio_init`, `pio_set_gpio_base`, `pio_sm_set_consecutive_pindirs`.
  SDK code that ignores the results still builds. `ws2812.pio`'s init
  function checks them, and returns -1 itself.
- **Pins are claimed from Linux** by `pio_gpio_init`, and only free pins
  can be claimed. Pins are GPIO numbers 0-47, and the library chooses the
  GPIO base.
- **Clocks.** `clock_get_hz(clk_sys)` is the block's clock, from the
  kernel. There is no `hardware/clocks.h`, `hardware/gpio.h`, `sleep_ms` or
  `stdio_init_all`: use the C library.
- **Interrupts.** There are no IRQ handlers (`irq_set_exclusive_handler`,
  `pio_set_irq1_source_enabled`, ...). Wait with `pio_wait_irq` or `poll`
  on IRQ0 instead.
- **DMA.** There is no `hardware/dma.h`. Use the `pio_dma_*` calls, which
  move data only between the block's DMA buffer and its FIFOs.
- **State machine claims** last only as long as the program.

## The examples

All of them are in `/usr/share/examples/pio` as source. Copy them
somewhere writable and build them with `cc -o NAME NAME.c -lpio`. Each one
uses PIO1.

| Example | Run | Does |
|---|---|---|
| `squarewave.c` | `./squarewave PIN HZ [SECONDS]` | a square wave from two `set` instructions assembled at run time. The delay and divider are worked out for HZ. Prints the frequency it made. Without SECONDS it runs until Ctrl-C. |
| `pio-fifo.c` | `./pio-fifo [WORDS]` | no pins: a state machine sends each word back inverted. Checks WORDS words (default 10000) by `put_blocking`/`get_blocking`, prints the round trips per second, then checks the interrupt path (`pio_wait_irq` on RX-not-empty) |
| `pio-dma.c` | `./pio-dma [WORDS]` | the same echo by DMA (up to 4096 words, prints words/s), then an endless 4 KiB ring of a count-down, which it checks holds consecutive counts |
| `pio-edges.c` | `./pio-edges PIN EDGES [TIMEOUT_MS]` | times every edge on PIN with a state machine counter, and stores the times by DMA (up to 16384). Prints `ready`, then the edge count and the shortest, longest and mean spacing. The pin isn't claimed: inputs see every pin. |
| `ws2812.c`, `ws2812.pio` | `pioasm ws2812.pio ws2812.pio.h && cc -o ws2812 ws2812.c -lpio`, then `./ws2812 PIN COUNT RRGGBB` | WS2812 LEDs at 800 kHz, through the header `pioasm` writes |

On the board, `rp2350/tools/pin-test.py` builds all of them with tcc and
checks them against a second board:

- `squarewave` at 1 kHz, 100 kHz and 1 MHz measured within 0.001%.
- `ws2812` sent exactly 240 bits for 10 LEDs.
- `pio-edges` caught every edge at 1 ms, 20 µs and 5 µs spacing. The
  GPIO character device's edge events need about 2 ms between edges.
- `pio-dma` echoed at about 18M words a second and kept its ring.

## On the Mac

The emulator's `-G` runs both blocks in software (`rvemu-rs/rvemu/src/piosim.rs`),
with the DMA pool, so libpio programs can be tried without the board:

```
$ ./target/release/rvemu -k ../guest/out-disk/Image -d ../guest/out-disk/rootfs.squashfs -G -m 16
```

The simulated blocks run `RVEMU_PIO_HZ` cycles a second (default 10 MHz)
in step with wall time, while `pio_clock_hz` reports 150 MHz. A program
therefore behaves as on the board, but more slowly. `irq` and `wait irq`
on neighbouring blocks (`prev`/`next`) aren't simulated. See
[rvemu-rs/README.md](../../../../rvemu-rs/README.md).
