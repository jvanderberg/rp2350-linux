/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Josh Vanderberg
 * The instruction encoders below are BSD-3-Clause (SPDX-Snippet). */
/* libpio: the RP2350's PIO1 and PIO2 from Linux programs on rp2350-linux.
 *
 * The functions are the pico-sdk's hardware_pio ones (pio_add_program,
 * pio_sm_init, sm_config_set_*, pio_sm_put_blocking, pio_encode_*, ...),
 * so most SDK PIO code, including the init functions pioasm copies from a
 * .pio file's `% c-sdk {` block, builds unchanged. The differences:
 *   - a block is opened first: PIO pio = pio_open(1);  (1 or 2; PIO0 is the
 *     board's radio), and pio_close(pio) at the end (exit closes it too).
 *     One program at a time has a block.
 *   - pins are GPIO numbers 0-47; a block reaches 32 consecutive ones
 *     (GPIOBASE 0 or 16), which pio_sm_set_config picks when the state
 *     machine's pins need it. pio_gpio_init(pio, pin) takes the pin from
 *     Linux (it fails, -1 and errno, when something else has it).
 *   - calls that can fail return -1 (errno set) instead of panicking.
 *   - pio_assemble() assembles .pio source at run time (pioasm's syntax),
 *     so a program can carry its PIO code as a string; pioasm(1) turns a
 *     .pio file into a header, as the SDK's does.
 *   - pio_wait_irq() sleeps until the block's IRQ0 output is up;
 *     pio_sm_get_blocking / put_blocking sleep the same way when they wait.
 *
 *   cc prog.c -lpio
 */
#ifndef LIBPIO_PIO_H
#define LIBPIO_PIO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#if !defined(_GNU_SOURCE) && !defined(_BSD_SOURCE) && !defined(_DEFAULT_SOURCE)
typedef unsigned int uint;
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ registers */

typedef struct {
    volatile uint32_t clkdiv;
    volatile uint32_t execctrl;
    volatile uint32_t shiftctrl;
    volatile uint32_t addr;
    volatile uint32_t instr;
    volatile uint32_t pinctrl;
} pio_sm_hw_t;

typedef struct {
    volatile uint32_t inte;
    volatile uint32_t intf;
    volatile uint32_t ints;
} pio_irq_ctrl_hw_t;

typedef struct {
    volatile uint32_t ctrl;
    volatile uint32_t fstat;
    volatile uint32_t fdebug;
    volatile uint32_t flevel;
    volatile uint32_t txf[4];
    volatile uint32_t rxf[4];
    volatile uint32_t irq;
    volatile uint32_t irq_force;
    volatile uint32_t input_sync_bypass;
    volatile uint32_t dbg_padout;
    volatile uint32_t dbg_padoe;
    volatile uint32_t dbg_cfginfo;
    volatile uint32_t instr_mem[32];
    pio_sm_hw_t sm[4];
    volatile uint32_t rxf_putget[4][4];
    volatile uint32_t gpiobase;
    volatile uint32_t intr;
    pio_irq_ctrl_hw_t irq_ctrl[2];
} pio_hw_t;

typedef pio_hw_t *PIO;

/* The register aliases: an atomic XOR, SET or CLR of bits. */
#define hw_xor_alias(a) ((volatile uint32_t *)((uintptr_t)(a) + 0x1000))
#define hw_set_alias(a) ((volatile uint32_t *)((uintptr_t)(a) + 0x2000))
#define hw_clear_alias(a) ((volatile uint32_t *)((uintptr_t)(a) + 0x3000))
#ifndef LIBPIO_PLAIN_REGISTERS
static inline void hw_set_bits(volatile uint32_t *a, uint32_t m) { *hw_set_alias(a) = m; }
static inline void hw_clear_bits(volatile uint32_t *a, uint32_t m) { *hw_clear_alias(a) = m; }
static inline void hw_xor_bits(volatile uint32_t *a, uint32_t m) { *hw_xor_alias(a) = m; }
#else
/* libpio's host tests (test/): registers in plain memory, which has no
 * aliases, so the same changes made directly */
static inline void hw_set_bits(volatile uint32_t *a, uint32_t m) { *a |= m; }
static inline void hw_clear_bits(volatile uint32_t *a, uint32_t m) { *a &= ~m; }
static inline void hw_xor_bits(volatile uint32_t *a, uint32_t m) { *a ^= m; }
#endif
static inline void hw_write_masked(volatile uint32_t *a, uint32_t v, uint32_t m)
{
    hw_xor_bits(a, (*a ^ v) & m);
}

#define NUM_PIO_STATE_MACHINES 4
#define PIO_INSTRUCTION_COUNT 32

/* ------------------------------------------------------------ blocks */

/* Open PIO block 1 or 2: its registers, reset. NULL (errno) on failure:
 * EBUSY when another program has it. */
PIO pio_open(int block);
void pio_close(PIO pio);
/* 1 or 2 */
uint pio_get_index(PIO pio);
/* The block's clock (clk_sys), Hz. */
uint32_t pio_clock_hz(PIO pio);
/* The SDK's clock_get_hz(clk_sys): the clock of the last block opened. */
enum clock_index { clk_ref = 4, clk_sys = 5 };
uint32_t clock_get_hz(enum clock_index clk);

/* Take pin `pin` (0-47) from Linux for this block; give it back. */
int pio_gpio_init(PIO pio, uint pin);
int pio_gpio_release(PIO pio, uint pin);
/* 0 or 16: the first of the 32 pins the block reaches. */
int pio_set_gpio_base(PIO pio, uint base);
uint pio_get_gpio_base(PIO pio);

/* ------------------------------------------------------------ programs */

typedef struct pio_program {
    const uint16_t *instructions;
    uint8_t length;
    int8_t origin; /* -1: anywhere */
    uint8_t pio_version;
} pio_program_t;

bool pio_can_add_program(PIO pio, const pio_program_t *program);
bool pio_can_add_program_at_offset(PIO pio, const pio_program_t *program, uint offset);
/* The offset it went to, or -1 (no room: ENOSPC). */
int pio_add_program(PIO pio, const pio_program_t *program);
int pio_add_program_at_offset(PIO pio, const pio_program_t *program, uint offset);
void pio_remove_program(PIO pio, const pio_program_t *program, uint loaded_offset);
void pio_clear_instruction_memory(PIO pio);

/* ------------------------------------------------------------ state machine config */

typedef struct {
    uint32_t clkdiv;
    uint32_t execctrl;
    uint32_t shiftctrl;
    uint32_t pinctrl;
    /* GPIO numbers (pinctrl holds them modulo 32), and which were set:
     * bit 0 out, 1 set, 2 in, 3 side-set, 4 jmp pin */
    uint8_t out_base, set_base, in_base, sideset_base, jmp_pin;
    uint8_t pins_set;
} pio_sm_config;

enum pio_fifo_join {
    PIO_FIFO_JOIN_NONE = 0,
    PIO_FIFO_JOIN_TX = 1,
    PIO_FIFO_JOIN_RX = 2,
    PIO_FIFO_JOIN_TXGET = 4,
    PIO_FIFO_JOIN_TXPUT = 8,
    PIO_FIFO_JOIN_PUTGET = 12,
};

enum pio_mov_status_type { STATUS_TX_LESSTHAN = 0, STATUS_RX_LESSTHAN = 1, STATUS_IRQ_SET = 2 };

pio_sm_config pio_get_default_sm_config(void);
void sm_config_set_out_pins(pio_sm_config *c, uint out_base, uint out_count);
void sm_config_set_out_pin_base(pio_sm_config *c, uint out_base);
void sm_config_set_out_pin_count(pio_sm_config *c, uint out_count);
void sm_config_set_set_pins(pio_sm_config *c, uint set_base, uint set_count);
void sm_config_set_set_pin_base(pio_sm_config *c, uint set_base);
void sm_config_set_set_pin_count(pio_sm_config *c, uint set_count);
void sm_config_set_in_pins(pio_sm_config *c, uint in_base);
void sm_config_set_in_pin_base(pio_sm_config *c, uint in_base);
void sm_config_set_in_pin_count(pio_sm_config *c, uint in_count);
void sm_config_set_sideset_pins(pio_sm_config *c, uint sideset_base);
void sm_config_set_sideset_pin_base(pio_sm_config *c, uint sideset_base);
void sm_config_set_sideset(pio_sm_config *c, uint bit_count, bool optional, bool pindirs);
void sm_config_set_clkdiv_int_frac8(pio_sm_config *c, uint32_t div_int, uint8_t div_frac8);
void sm_config_set_clkdiv_int_frac(pio_sm_config *c, uint16_t div_int, uint8_t div_frac);
void sm_config_set_clkdiv(pio_sm_config *c, float div);
void sm_config_set_wrap(pio_sm_config *c, uint wrap_target, uint wrap);
void sm_config_set_jmp_pin(pio_sm_config *c, uint pin);
void sm_config_set_in_shift(pio_sm_config *c, bool shift_right, bool autopush, uint push_threshold);
void sm_config_set_out_shift(pio_sm_config *c, bool shift_right, bool autopull, uint pull_threshold);
void sm_config_set_fifo_join(pio_sm_config *c, enum pio_fifo_join join);
void sm_config_set_out_special(pio_sm_config *c, bool sticky, bool has_enable_pin, uint enable_pin_index);
void sm_config_set_mov_status(pio_sm_config *c, enum pio_mov_status_type status_sel, uint status_n);

/* ------------------------------------------------------------ state machines */

/* Apply `config`: -1 (EINVAL) when its pins don't fit in 32 from one base,
 * or need a base other than the block's while it has machines running. */
int pio_sm_set_config(PIO pio, uint sm, const pio_sm_config *config);
/* Stop, configure (NULL: the default), empty the FIFOs, restart, and jump
 * to `initial_pc`. */
int pio_sm_init(PIO pio, uint sm, uint initial_pc, const pio_sm_config *config);
void pio_sm_set_enabled(PIO pio, uint sm, bool enabled);
void pio_set_sm_mask_enabled(PIO pio, uint32_t mask, bool enabled);
void pio_sm_restart(PIO pio, uint sm);
void pio_restart_sm_mask(PIO pio, uint32_t mask);
void pio_sm_clkdiv_restart(PIO pio, uint sm);
void pio_clkdiv_restart_sm_mask(PIO pio, uint32_t mask);
void pio_enable_sm_mask_in_sync(PIO pio, uint32_t mask);
uint8_t pio_sm_get_pc(PIO pio, uint sm);
void pio_sm_exec(PIO pio, uint sm, uint instr);
bool pio_sm_is_exec_stalled(PIO pio, uint sm);
void pio_sm_exec_wait_blocking(PIO pio, uint sm, uint instr);
void pio_sm_set_wrap(PIO pio, uint sm, uint wrap_target, uint wrap);
void pio_sm_set_out_pins(PIO pio, uint sm, uint out_base, uint out_count);
void pio_sm_set_set_pins(PIO pio, uint sm, uint set_base, uint set_count);
void pio_sm_set_in_pins(PIO pio, uint sm, uint in_base);
void pio_sm_set_sideset_pins(PIO pio, uint sm, uint sideset_base);
void pio_sm_set_jmp_pin(PIO pio, uint sm, uint pin);
void pio_sm_set_clkdiv_int_frac8(PIO pio, uint sm, uint32_t div_int, uint8_t div_frac8);
void pio_sm_set_clkdiv_int_frac(PIO pio, uint sm, uint16_t div_int, uint8_t div_frac);
void pio_sm_set_clkdiv(PIO pio, uint sm, float div);
/* Drive pins from the state machine (by running SET instructions on it;
 * it should be stopped): values, directions (1 out), a run of directions. */
void pio_sm_set_pins(PIO pio, uint sm, uint64_t pin_values);
void pio_sm_set_pins_with_mask(PIO pio, uint sm, uint64_t pin_values, uint64_t pin_mask);
void pio_sm_set_pindirs_with_mask(PIO pio, uint sm, uint64_t pin_dirs, uint64_t pin_mask);
int pio_sm_set_consecutive_pindirs(PIO pio, uint sm, uint pin_base, uint pin_count, bool is_out);

/* FIFOs */
static inline void pio_sm_put(PIO pio, uint sm, uint32_t data) { pio->txf[sm] = data; }
static inline uint32_t pio_sm_get(PIO pio, uint sm) { return pio->rxf[sm]; }
static inline bool pio_sm_is_rx_fifo_full(PIO pio, uint sm) { return pio->fstat >> sm & 1; }
static inline bool pio_sm_is_rx_fifo_empty(PIO pio, uint sm) { return pio->fstat >> (8 + sm) & 1; }
static inline bool pio_sm_is_tx_fifo_full(PIO pio, uint sm) { return pio->fstat >> (16 + sm) & 1; }
static inline bool pio_sm_is_tx_fifo_empty(PIO pio, uint sm) { return pio->fstat >> (24 + sm) & 1; }
static inline uint pio_sm_get_rx_fifo_level(PIO pio, uint sm) { return pio->flevel >> (8 * sm + 4) & 0xf; }
static inline uint pio_sm_get_tx_fifo_level(PIO pio, uint sm) { return pio->flevel >> (8 * sm) & 0xf; }
void pio_sm_put_blocking(PIO pio, uint sm, uint32_t data);
uint32_t pio_sm_get_blocking(PIO pio, uint sm);
void pio_sm_clear_fifos(PIO pio, uint sm);
void pio_sm_drain_tx_fifo(PIO pio, uint sm);

/* claiming state machines (within this program) */
void pio_sm_claim(PIO pio, uint sm);
void pio_claim_sm_mask(PIO pio, uint sm_mask);
void pio_sm_unclaim(PIO pio, uint sm);
/* The state machine, or -1 (none free; required: exit with a message). */
int pio_claim_unused_sm(PIO pio, bool required);
bool pio_sm_is_claimed(PIO pio, uint sm);

/* ------------------------------------------------------------ interrupts */

enum pio_interrupt_source {
    pis_sm0_rx_fifo_not_empty = 0,
    pis_sm1_rx_fifo_not_empty = 1,
    pis_sm2_rx_fifo_not_empty = 2,
    pis_sm3_rx_fifo_not_empty = 3,
    pis_sm0_tx_fifo_not_full = 4,
    pis_sm1_tx_fifo_not_full = 5,
    pis_sm2_tx_fifo_not_full = 6,
    pis_sm3_tx_fifo_not_full = 7,
    pis_interrupt0 = 8,
    pis_interrupt1 = 9,
    pis_interrupt2 = 10,
    pis_interrupt3 = 11,
    pis_interrupt4 = 12,
    pis_interrupt5 = 13,
    pis_interrupt6 = 14,
    pis_interrupt7 = 15,
};

static inline bool pio_interrupt_get(PIO pio, uint n) { return pio->irq >> n & 1; }
static inline void pio_interrupt_clear(PIO pio, uint n) { pio->irq = 1u << n; }
void pio_set_irq0_source_enabled(PIO pio, enum pio_interrupt_source source, bool enabled);
void pio_set_irq0_source_mask_enabled(PIO pio, uint32_t mask, bool enabled);
/* Sleep until IRQ0_INTS is not 0, at most timeout_ms (-1: no limit);
 * IRQ0_INTS, 0 on timeout, -1 on error. */
int64_t pio_wait_irq(PIO pio, int timeout_ms);
/* The block's file descriptor (poll(2) says POLLIN while IRQ0 is up). */
int pio_fd(PIO pio);

/* ------------------------------------------------------------ DMA */

/* The board has a few DMA channels for PIO (4): each moves data between
 * this program's DMA buffer and one state machine's FIFO, at the FIFO's
 * pace, with no CPU. */

/* The block's DMA buffer, mapped: at least `size` bytes (a page multiple,
 * at most 64 KiB), zeroed. One per block: later calls give the same one
 * (EINVAL when it is smaller than asked). NULL (errno) on failure. */
void *pio_dma_buffer(PIO pio, size_t size);

typedef struct {
    uint sm;
    bool from_sm;     /* its RX FIFO into the buffer; else the buffer to its TX FIFO */
    uint size;        /* bytes a transfer: 1, 2 or 4 (narrow writes to a TX FIFO
                       * are copied across the word, as on the chip) */
    size_t offset;    /* where in the buffer */
    uint32_t count;   /* transfers */
    uint ring_bits;   /* 0, or the buffer side wraps within 1 << ring_bits
                       * bytes (at most 4096; offset aligned to it) */
    bool endless;     /* never ends (with a ring): a capture or a loop */
} pio_dma_config;

/* Start a transfer on a new channel (channel -1) or one of ours again: the
 * channel, or -1 (errno: EBUSY, all in use; EINVAL, a bad config). */
int pio_dma_start(PIO pio, int channel, const pio_dma_config *c);
/* Wait until it is done: 0, or -1 (errno ETIMEDOUT after timeout_ms; 0:
 * no limit). */
int pio_dma_wait(PIO pio, int channel, int timeout_ms);
/* 1 still going, 0 done, -1 (errno EIO) stopped on a bus error; transfers
 * left and where the buffer side is (bytes past offset), either NULL. */
int pio_dma_status(PIO pio, int channel, uint32_t *remaining, uint32_t *pos);
/* Stop it and give the channel back. */
int pio_dma_stop(PIO pio, int channel);
/* Send `bytes` of `data` to state machine sm's TX FIFO, or receive into
 * it from the RX FIFO, `size` bytes a transfer, through the DMA buffer
 * (16 KiB unless one was made), waiting until done: 0 or -1. */
int pio_sm_dma_write(PIO pio, uint sm, const void *data, size_t bytes, uint size);
int pio_sm_dma_read(PIO pio, uint sm, void *data, size_t bytes, uint size);

/* ------------------------------------------------------------ instructions */

/* From here to SPDX-SnippetEnd follows the pico-sdk's
 * hardware/pio_instructions.h (https://github.com/raspberrypi/pico-sdk):
 * enum pio_src_dest is the SDK's, its values the SDK's own flag bits
 * (_PIO_INVALID_*_SRC/DEST, PICO_PIO_VERSION 0) written out; the
 * pio_encode_* functions are the SDK's names and signatures, and
 * pio_encode_sideset, pio_encode_sideset_opt and pio_encode_nop the same
 * expressions.
 *
 * SPDX-SnippetBegin
 * SPDX-SnippetCopyrightText: Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
 *    disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
enum pio_src_dest {
    pio_pins = 0u,
    pio_x = 1u,
    pio_y = 2u,
    pio_null = 3u | 0x20u | 0x80u,
    pio_pindirs = 4u | 0x08u | 0x40u | 0x80u,
    pio_exec_mov = 4u | 0x08u | 0x10u | 0x20u | 0x40u,
    pio_status = 5u | 0x08u | 0x10u | 0x20u | 0x80u,
    pio_pc = 5u | 0x08u | 0x20u | 0x40u,
    pio_isr = 6u | 0x20u,
    pio_osr = 7u | 0x10u | 0x20u,
    pio_exec_out = 7u | 0x08u | 0x20u | 0x40u | 0x80u,
};

static inline uint pio_encode_delay(uint cycles) { return cycles << 8; }
static inline uint pio_encode_sideset(uint sideset_bit_count, uint value)
{
    return value << (13 - sideset_bit_count);
}
static inline uint pio_encode_sideset_opt(uint sideset_bit_count, uint value)
{
    return 0x1000u | value << (12 - sideset_bit_count);
}
static inline uint _pio_encode(uint op, uint a, uint b) { return op << 13 | (a & 7) << 5 | (b & 0x1f); }
static inline uint pio_encode_jmp(uint addr) { return _pio_encode(0, 0, addr); }
static inline uint pio_encode_jmp_not_x(uint addr) { return _pio_encode(0, 1, addr); }
static inline uint pio_encode_jmp_x_dec(uint addr) { return _pio_encode(0, 2, addr); }
static inline uint pio_encode_jmp_not_y(uint addr) { return _pio_encode(0, 3, addr); }
static inline uint pio_encode_jmp_y_dec(uint addr) { return _pio_encode(0, 4, addr); }
static inline uint pio_encode_jmp_x_ne_y(uint addr) { return _pio_encode(0, 5, addr); }
static inline uint pio_encode_jmp_pin(uint addr) { return _pio_encode(0, 6, addr); }
static inline uint pio_encode_jmp_not_osre(uint addr) { return _pio_encode(0, 7, addr); }
static inline uint pio_encode_wait_gpio(bool polarity, uint gpio) { return _pio_encode(1, polarity << 2, gpio); }
static inline uint pio_encode_wait_pin(bool polarity, uint pin) { return _pio_encode(1, polarity << 2 | 1, pin); }
static inline uint pio_encode_wait_irq(bool polarity, bool relative, uint irq)
{
    return _pio_encode(1, polarity << 2 | 2, (relative ? 0x10 : 0) | irq);
}
static inline uint pio_encode_in(enum pio_src_dest src, uint count) { return _pio_encode(2, src, count); }
static inline uint pio_encode_out(enum pio_src_dest dest, uint count) { return _pio_encode(3, dest, count); }
static inline uint pio_encode_push(bool if_full, bool block) { return _pio_encode(4, if_full << 1 | block, 0); }
static inline uint pio_encode_pull(bool if_empty, bool block) { return _pio_encode(4, 4 | if_empty << 1 | block, 0); }
static inline uint pio_encode_mov(enum pio_src_dest dest, enum pio_src_dest src) { return _pio_encode(5, dest, src & 7); }
static inline uint pio_encode_mov_not(enum pio_src_dest dest, enum pio_src_dest src)
{
    return _pio_encode(5, dest, 0x08 | (src & 7));
}
static inline uint pio_encode_mov_reverse(enum pio_src_dest dest, enum pio_src_dest src)
{
    return _pio_encode(5, dest, 0x10 | (src & 7));
}
static inline uint pio_encode_irq_set(bool relative, uint irq) { return _pio_encode(6, 0, (relative ? 0x10 : 0) | irq); }
static inline uint pio_encode_irq_wait(bool relative, uint irq) { return _pio_encode(6, 1, (relative ? 0x10 : 0) | irq); }
static inline uint pio_encode_irq_clear(bool relative, uint irq) { return _pio_encode(6, 2, (relative ? 0x10 : 0) | irq); }
static inline uint pio_encode_set(enum pio_src_dest dest, uint value) { return _pio_encode(7, dest, value); }
static inline uint pio_encode_nop(void) { return pio_encode_mov(pio_y, pio_y); }
/* SPDX-SnippetEnd */

/* ------------------------------------------------------------ the assembler */

#define PIO_ASM_MAX_DEFINES 16

/* One assembled program, with what pioasm puts in its header. */
typedef struct {
    char name[32];
    uint16_t instructions[PIO_INSTRUCTION_COUNT];
    pio_program_t program; /* points at instructions */
    uint8_t wrap_target, wrap;
    /* .side_set: bits (without the opt bit), opt, pindirs */
    uint8_t sideset_bits;
    bool sideset_opt, sideset_pindirs;
    /* .in / .out / .set / .fifo / .mov_status / .clock_div, when given
     * (bit 0 in, 1 out, 2 set, 3 fifo, 4 mov_status, 5 clock_div) */
    uint8_t has;
    uint8_t in_count, in_right, in_auto, in_threshold;
    uint8_t out_count, out_right, out_auto, out_threshold;
    uint8_t set_count;
    uint8_t fifo; /* enum pio_fifo_join */
    uint8_t mov_status_type, mov_status_n;
    float clock_div;
    /* .define public */
    int ndefines;
    struct { char name[32]; int value; } defines[PIO_ASM_MAX_DEFINES];
} pio_asm_program_t;

/* Assemble program `name` of `source` (NULL: the first; a source without
 * .program is one program). 0, or -1 with a message ("line N: ...") in
 * err. */
int pio_assemble(const char *source, const char *name, pio_asm_program_t *out, char *err, size_t errlen);
/* The default state machine config for it loaded at `offset`, as pioasm's
 * NAME_program_get_default_config. */
pio_sm_config pio_asm_default_config(const pio_asm_program_t *p, uint offset);
/* A `.define public` value; false when there is none. */
bool pio_asm_define(const pio_asm_program_t *p, const char *name, int *value);

#ifdef __cplusplus
}
#endif
#endif
