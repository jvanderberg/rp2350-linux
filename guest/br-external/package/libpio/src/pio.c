/* libpio: the PIO blocks through /dev/pio1 and /dev/pio2 (pio.h). */
#define _GNU_SOURCE
#include "pio.h"
#include "rvemu-pio.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* SHIFTCTRL, EXECCTRL, PINCTRL fields */
#define SC_FJOIN_RX (1u << 31)
#define SC_FJOIN_TX (1u << 30)
#define SC_JOIN_MASK (3u << 30 | 3u << 14)
#define EC_OUT_STICKY (1u << 17)
#define EC_STALLED (1u << 31)

struct block {
    PIO pio;
    int fd;
    uint32_t clk_hz;
    uint32_t used;      /* instruction memory */
    uint8_t claimed_sm; /* pio_sm_claim */
    void *dma;          /* the DMA buffer, mapped */
    size_t dma_size;
};

static struct block blocks[2];
static uint32_t last_clk_hz;

static struct block *blk(PIO pio)
{
    for (int i = 0; i < 2; i++)
        if (blocks[i].pio == pio && pio)
            return &blocks[i];
    return NULL;
}

PIO pio_open(int n)
{
    char path[16];
    struct rvemu_pio_info info;
    struct block *b;
    void *m;
    int fd;

    if (n != 1 && n != 2) {
        errno = EINVAL;
        return NULL;
    }
    b = &blocks[n - 1];
    if (b->pio)
        return b->pio;
    snprintf(path, sizeof(path), "/dev/pio%d", n);
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    if (ioctl(fd, RVEMU_PIO_INFO, &info) < 0)
        goto fail;
    m = mmap(NULL, RVEMU_PIO_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED)
        goto fail;
    b->pio = m;
    b->fd = fd;
    b->clk_hz = info.clk_hz;
    b->used = 0;
    b->claimed_sm = 0;
    last_clk_hz = info.clk_hz;
    return b->pio;
fail:
    n = errno;
    close(fd);
    errno = n;
    return NULL;
}

void pio_close(PIO pio)
{
    struct block *b = blk(pio);

    if (!b)
        return;
    munmap((void *)b->pio, RVEMU_PIO_MAP_SIZE);
    if (b->dma)
        munmap(b->dma, b->dma_size);
    close(b->fd);
    memset(b, 0, sizeof(*b));
}

uint pio_get_index(PIO pio)
{
    struct block *b = blk(pio);
    return b ? (uint)(b - blocks) + 1 : 0;
}

uint32_t pio_clock_hz(PIO pio)
{
    struct block *b = blk(pio);
    return b ? b->clk_hz : 0;
}

uint32_t clock_get_hz(enum clock_index clk)
{
    (void)clk;
    return last_clk_hz;
}

int pio_fd(PIO pio)
{
    struct block *b = blk(pio);
    return b ? b->fd : -1;
}

int pio_gpio_init(PIO pio, uint pin)
{
    struct block *b = blk(pio);
    uint32_t n = pin;

    if (!b) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(b->fd, RVEMU_PIO_PIN_CLAIM, &n) < 0 ? -1 : 0;
}

int pio_gpio_release(PIO pio, uint pin)
{
    struct block *b = blk(pio);
    uint32_t n = pin;

    if (!b) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(b->fd, RVEMU_PIO_PIN_RELEASE, &n) < 0 ? -1 : 0;
}

int pio_set_gpio_base(PIO pio, uint base)
{
    if (base != 0 && base != 16) {
        errno = EINVAL;
        return -1;
    }
    pio->gpiobase = base;
    return 0;
}

uint pio_get_gpio_base(PIO pio)
{
    return pio->gpiobase & 16;
}

/* ------------------------------------------------------------ programs */

static uint32_t prog_mask(const pio_program_t *p, uint offset)
{
    uint32_t m = p->length >= 32 ? ~0u : (1u << p->length) - 1;
    return m << offset;
}

bool pio_can_add_program_at_offset(PIO pio, const pio_program_t *p, uint offset)
{
    struct block *b = blk(pio);

    if (!b || !p->length || p->length > 32 || offset + p->length > 32)
        return false;
    if (p->origin >= 0 && (uint)p->origin != offset)
        return false;
    return !(b->used & prog_mask(p, offset));
}

static int find_offset(PIO pio, const pio_program_t *p)
{
    if (p->origin >= 0)
        return pio_can_add_program_at_offset(pio, p, p->origin) ? p->origin : -1;
    /* from the top, as the SDK does */
    for (int off = 32 - p->length; off >= 0; off--)
        if (pio_can_add_program_at_offset(pio, p, off))
            return off;
    return -1;
}

bool pio_can_add_program(PIO pio, const pio_program_t *p)
{
    return find_offset(pio, p) >= 0;
}

int pio_add_program_at_offset(PIO pio, const pio_program_t *p, uint offset)
{
    struct block *b = blk(pio);

    if (!pio_can_add_program_at_offset(pio, p, offset)) {
        errno = ENOSPC;
        return -1;
    }
    for (uint i = 0; i < p->length; i++) {
        uint16_t ins = p->instructions[i];
        /* JMPs are relative to the program's start */
        if (ins >> 13 == 0)
            ins += offset;
        pio->instr_mem[offset + i] = ins;
    }
    b->used |= prog_mask(p, offset);
    return offset;
}

int pio_add_program(PIO pio, const pio_program_t *p)
{
    int off = find_offset(pio, p);

    if (off < 0) {
        errno = ENOSPC;
        return -1;
    }
    return pio_add_program_at_offset(pio, p, off);
}

void pio_remove_program(PIO pio, const pio_program_t *p, uint offset)
{
    struct block *b = blk(pio);

    if (b && offset < 32)
        b->used &= ~prog_mask(p, offset);
}

void pio_clear_instruction_memory(PIO pio)
{
    struct block *b = blk(pio);

    for (int i = 0; i < 32; i++)
        pio->instr_mem[i] = pio_encode_jmp(i);
    if (b)
        b->used = 0;
}

/* ------------------------------------------------------------ config */

pio_sm_config pio_get_default_sm_config(void)
{
    pio_sm_config c;

    memset(&c, 0, sizeof(c));
    sm_config_set_clkdiv_int_frac8(&c, 1, 0);
    sm_config_set_wrap(&c, 0, 31);
    sm_config_set_in_shift(&c, true, false, 32);
    sm_config_set_out_shift(&c, true, false, 32);
    return c;
}

void sm_config_set_out_pin_base(pio_sm_config *c, uint base)
{
    c->out_base = base;
    c->pins_set |= 1;
    c->pinctrl = (c->pinctrl & ~0x1fu) | (base & 31);
}

void sm_config_set_out_pin_count(pio_sm_config *c, uint count)
{
    c->pinctrl = (c->pinctrl & ~(0x3fu << 20)) | (count & 0x3f) << 20;
}

void sm_config_set_out_pins(pio_sm_config *c, uint base, uint count)
{
    sm_config_set_out_pin_base(c, base);
    sm_config_set_out_pin_count(c, count);
}

void sm_config_set_set_pin_base(pio_sm_config *c, uint base)
{
    c->set_base = base;
    c->pins_set |= 2;
    c->pinctrl = (c->pinctrl & ~(0x1fu << 5)) | (base & 31) << 5;
}

void sm_config_set_set_pin_count(pio_sm_config *c, uint count)
{
    c->pinctrl = (c->pinctrl & ~(7u << 26)) | (count & 7) << 26;
}

void sm_config_set_set_pins(pio_sm_config *c, uint base, uint count)
{
    sm_config_set_set_pin_base(c, base);
    sm_config_set_set_pin_count(c, count);
}

void sm_config_set_in_pin_base(pio_sm_config *c, uint base)
{
    c->in_base = base;
    c->pins_set |= 4;
    c->pinctrl = (c->pinctrl & ~(0x1fu << 15)) | (base & 31) << 15;
}

void sm_config_set_in_pins(pio_sm_config *c, uint base)
{
    sm_config_set_in_pin_base(c, base);
}

void sm_config_set_in_pin_count(pio_sm_config *c, uint count)
{
    c->shiftctrl = (c->shiftctrl & ~0x1fu) | (count & 0x1f);
}

void sm_config_set_sideset_pin_base(pio_sm_config *c, uint base)
{
    c->sideset_base = base;
    c->pins_set |= 8;
    c->pinctrl = (c->pinctrl & ~(0x1fu << 10)) | (base & 31) << 10;
}

void sm_config_set_sideset_pins(pio_sm_config *c, uint base)
{
    sm_config_set_sideset_pin_base(c, base);
}

void sm_config_set_sideset(pio_sm_config *c, uint bits, bool optional, bool pindirs)
{
    c->pinctrl = (c->pinctrl & ~(7u << 29)) | (bits & 7) << 29;
    c->execctrl = (c->execctrl & ~(3u << 29)) | (uint32_t)optional << 30 | (uint32_t)pindirs << 29;
}

void sm_config_set_clkdiv_int_frac8(pio_sm_config *c, uint32_t div_int, uint8_t div_frac8)
{
    c->clkdiv = (div_int & 0xffff) << 16 | (uint32_t)div_frac8 << 8;
}

void sm_config_set_clkdiv_int_frac(pio_sm_config *c, uint16_t div_int, uint8_t div_frac)
{
    sm_config_set_clkdiv_int_frac8(c, div_int, div_frac);
}

static void div_to_int_frac(float div, uint32_t *i, uint8_t *f)
{
    if (div < 1.0f)
        div = 1.0f;
    if (div >= 65536.0f) {
        *i = 0; /* 65536 */
        *f = 0;
        return;
    }
    *i = (uint32_t)div;
    *f = (uint8_t)((div - (float)*i) * 256.0f);
}

void sm_config_set_clkdiv(pio_sm_config *c, float div)
{
    uint32_t i;
    uint8_t f;

    div_to_int_frac(div, &i, &f);
    sm_config_set_clkdiv_int_frac8(c, i, f);
}

void sm_config_set_wrap(pio_sm_config *c, uint target, uint wrap)
{
    c->execctrl = (c->execctrl & ~(0x1fu << 12 | 0x1fu << 7)) | (wrap & 31) << 12 | (target & 31) << 7;
}

void sm_config_set_jmp_pin(pio_sm_config *c, uint pin)
{
    c->jmp_pin = pin;
    c->pins_set |= 16;
    c->execctrl = (c->execctrl & ~(0x1fu << 24)) | (pin & 31) << 24;
}

void sm_config_set_in_shift(pio_sm_config *c, bool right, bool autopush, uint thresh)
{
    c->shiftctrl = (c->shiftctrl & ~(1u << 18 | 1u << 16 | 0x1fu << 20)) |
                   (uint32_t)right << 18 | (uint32_t)autopush << 16 | (thresh & 0x1f) << 20;
}

void sm_config_set_out_shift(pio_sm_config *c, bool right, bool autopull, uint thresh)
{
    c->shiftctrl = (c->shiftctrl & ~(1u << 19 | 1u << 17 | 0x1fu << 25)) |
                   (uint32_t)right << 19 | (uint32_t)autopull << 17 | (thresh & 0x1f) << 25;
}

void sm_config_set_fifo_join(pio_sm_config *c, enum pio_fifo_join join)
{
    c->shiftctrl = (c->shiftctrl & ~SC_JOIN_MASK) | (uint32_t)(join & 3) << 30 | (uint32_t)(join >> 2) << 14;
}

void sm_config_set_out_special(pio_sm_config *c, bool sticky, bool has_enable_pin, uint enable_pin_index)
{
    c->execctrl = (c->execctrl & ~(1u << 17 | 1u << 18 | 0x1fu << 19)) | (uint32_t)sticky << 17 |
                  (uint32_t)has_enable_pin << 18 | (enable_pin_index & 0x1f) << 19;
}

void sm_config_set_mov_status(pio_sm_config *c, enum pio_mov_status_type sel, uint n)
{
    c->execctrl = (c->execctrl & ~(3u << 5 | 0x1fu)) | (uint32_t)(sel & 3) << 5 | (n & 0x1f);
}

/* ------------------------------------------------------------ state machines */

/* The GPIOBASE `c`'s pins need (bit 0: 0 works, bit 1: 16 works). */
static int bases_for(const pio_sm_config *c)
{
    uint8_t pins[5] = { c->out_base, c->set_base, c->in_base, c->sideset_base, c->jmp_pin };
    int ok = 3;

    for (int i = 0; i < 5; i++) {
        if (!(c->pins_set >> i & 1))
            continue;
        if (pins[i] >= 32)
            ok &= ~1;
        if (pins[i] < 16)
            ok &= ~2;
    }
    return ok;
}

int pio_sm_set_config(PIO pio, uint sm, const pio_sm_config *c)
{
    int ok = bases_for(c);
    uint base = pio->gpiobase & 16;

    if (sm > 3 || !ok) {
        errno = EINVAL;
        return -1;
    }
    if (!(ok >> (base ? 1 : 0) & 1)) {
        /* the other base: only while nothing runs on this one */
        if (pio->ctrl & 0xf) {
            errno = EINVAL;
            return -1;
        }
        pio->gpiobase = base ? 0 : 16;
    }
    pio->sm[sm].clkdiv = c->clkdiv;
    pio->sm[sm].execctrl = c->execctrl;
    pio->sm[sm].shiftctrl = c->shiftctrl;
    pio->sm[sm].pinctrl = c->pinctrl;
    return 0;
}

void pio_sm_set_enabled(PIO pio, uint sm, bool en)
{
    if (en)
        hw_set_bits(&pio->ctrl, 1u << sm);
    else
        hw_clear_bits(&pio->ctrl, 1u << sm);
}

void pio_set_sm_mask_enabled(PIO pio, uint32_t mask, bool en)
{
    if (en)
        hw_set_bits(&pio->ctrl, mask & 0xf);
    else
        hw_clear_bits(&pio->ctrl, mask & 0xf);
}

void pio_sm_restart(PIO pio, uint sm) { hw_set_bits(&pio->ctrl, 1u << (4 + sm)); }
void pio_restart_sm_mask(PIO pio, uint32_t mask) { hw_set_bits(&pio->ctrl, (mask & 0xf) << 4); }
void pio_sm_clkdiv_restart(PIO pio, uint sm) { hw_set_bits(&pio->ctrl, 1u << (8 + sm)); }
void pio_clkdiv_restart_sm_mask(PIO pio, uint32_t mask) { hw_set_bits(&pio->ctrl, (mask & 0xf) << 8); }

void pio_enable_sm_mask_in_sync(PIO pio, uint32_t mask)
{
    hw_set_bits(&pio->ctrl, (mask & 0xf) | (mask & 0xf) << 8);
}

uint8_t pio_sm_get_pc(PIO pio, uint sm) { return pio->sm[sm].addr & 0x1f; }
void pio_sm_exec(PIO pio, uint sm, uint instr) { pio->sm[sm].instr = instr; }
bool pio_sm_is_exec_stalled(PIO pio, uint sm) { return pio->sm[sm].execctrl & EC_STALLED; }

void pio_sm_exec_wait_blocking(PIO pio, uint sm, uint instr)
{
    pio_sm_exec(pio, sm, instr);
    while (pio_sm_is_exec_stalled(pio, sm))
        ;
}

void pio_sm_clear_fifos(PIO pio, uint sm)
{
    /* changing the join empties them: twice puts it back */
    hw_xor_bits(&pio->sm[sm].shiftctrl, SC_FJOIN_RX);
    hw_xor_bits(&pio->sm[sm].shiftctrl, SC_FJOIN_RX);
}

int pio_sm_init(PIO pio, uint sm, uint initial_pc, const pio_sm_config *config)
{
    pio_sm_config def;

    if (sm > 3) {
        errno = EINVAL;
        return -1;
    }
    pio_sm_set_enabled(pio, sm, false);
    if (!config) {
        def = pio_get_default_sm_config();
        config = &def;
    }
    if (pio_sm_set_config(pio, sm, config) < 0)
        return -1;
    pio_sm_clear_fifos(pio, sm);
    /* the FIFO debug flags of this machine */
    pio->fdebug = (1u << 24 | 1u << 16 | 1u << 8 | 1u) << sm;
    pio_sm_restart(pio, sm);
    pio_sm_clkdiv_restart(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_jmp(initial_pc));
    return 0;
}

void pio_sm_set_wrap(PIO pio, uint sm, uint target, uint wrap)
{
    hw_write_masked(&pio->sm[sm].execctrl, (wrap & 31) << 12 | (target & 31) << 7, 0x1fu << 12 | 0x1fu << 7);
}

void pio_sm_set_out_pins(PIO pio, uint sm, uint base, uint count)
{
    hw_write_masked(&pio->sm[sm].pinctrl, (base & 31) | (count & 0x3f) << 20, 0x1fu | 0x3fu << 20);
}

void pio_sm_set_set_pins(PIO pio, uint sm, uint base, uint count)
{
    hw_write_masked(&pio->sm[sm].pinctrl, (base & 31) << 5 | (count & 7) << 26, 0x1fu << 5 | 7u << 26);
}

void pio_sm_set_in_pins(PIO pio, uint sm, uint base)
{
    hw_write_masked(&pio->sm[sm].pinctrl, (base & 31) << 15, 0x1fu << 15);
}

void pio_sm_set_sideset_pins(PIO pio, uint sm, uint base)
{
    hw_write_masked(&pio->sm[sm].pinctrl, (base & 31) << 10, 0x1fu << 10);
}

void pio_sm_set_jmp_pin(PIO pio, uint sm, uint pin)
{
    hw_write_masked(&pio->sm[sm].execctrl, (pin & 31) << 24, 0x1fu << 24);
}

void pio_sm_set_clkdiv_int_frac8(PIO pio, uint sm, uint32_t div_int, uint8_t div_frac8)
{
    pio->sm[sm].clkdiv = (div_int & 0xffff) << 16 | (uint32_t)div_frac8 << 8;
}

void pio_sm_set_clkdiv_int_frac(PIO pio, uint sm, uint16_t div_int, uint8_t div_frac)
{
    pio_sm_set_clkdiv_int_frac8(pio, sm, div_int, div_frac);
}

void pio_sm_set_clkdiv(PIO pio, uint sm, float div)
{
    uint32_t i;
    uint8_t f;

    div_to_int_frac(div, &i, &f);
    pio_sm_set_clkdiv_int_frac8(pio, sm, i, f);
}

/* SET pins / pindirs one pin at a time on `sm`, as the SDK does. */
static void set_each(PIO pio, uint sm, uint64_t values, uint64_t mask, bool dirs)
{
    uint32_t pinctrl = pio->sm[sm].pinctrl, execctrl = pio->sm[sm].execctrl;
    uint base = pio->gpiobase & 16;

    hw_clear_bits(&pio->sm[sm].execctrl, EC_OUT_STICKY);
    for (uint n = 0; n < 48; n++) {
        if (!(mask >> n & 1) || n < base || n >= base + 32)
            continue;
        pio->sm[sm].pinctrl = 1u << 26 | ((n - base) & 31) << 5;
        pio_sm_exec(pio, sm, pio_encode_set(dirs ? pio_pindirs : pio_pins, values >> n & 1));
    }
    pio->sm[sm].pinctrl = pinctrl;
    pio->sm[sm].execctrl = execctrl;
}

void pio_sm_set_pins(PIO pio, uint sm, uint64_t values)
{
    set_each(pio, sm, values, ~0ull, false);
}

void pio_sm_set_pins_with_mask(PIO pio, uint sm, uint64_t values, uint64_t mask)
{
    set_each(pio, sm, values, mask, false);
}

void pio_sm_set_pindirs_with_mask(PIO pio, uint sm, uint64_t dirs, uint64_t mask)
{
    set_each(pio, sm, dirs, mask, true);
}

int pio_sm_set_consecutive_pindirs(PIO pio, uint sm, uint pin, uint count, bool out)
{
    uint64_t m;

    if (pin + count > 48 || count > 32) {
        errno = EINVAL;
        return -1;
    }
    m = (count >= 64 ? ~0ull : (1ull << count) - 1) << pin;
    set_each(pio, sm, out ? m : 0, m, true);
    return 0;
}

/* ------------------------------------------------------------ waiting */

int64_t pio_wait_irq(PIO pio, int timeout_ms)
{
    struct block *b = blk(pio);
    struct pollfd p;
    uint32_t ints;
    int r;

    if (!b) {
        errno = EINVAL;
        return -1;
    }
    p.fd = b->fd;
    p.events = POLLIN;
    r = poll(&p, 1, timeout_ms);
    if (r <= 0)
        return r;
    if (read(b->fd, &ints, sizeof(ints)) != sizeof(ints))
        return errno == EAGAIN ? 0 : -1;
    return ints;
}

void pio_set_irq0_source_enabled(PIO pio, enum pio_interrupt_source s, bool en)
{
    if (en)
        hw_set_bits(&pio->irq_ctrl[0].inte, 1u << s);
    else
        hw_clear_bits(&pio->irq_ctrl[0].inte, 1u << s);
}

void pio_set_irq0_source_mask_enabled(PIO pio, uint32_t mask, bool en)
{
    if (en)
        hw_set_bits(&pio->irq_ctrl[0].inte, mask);
    else
        hw_clear_bits(&pio->irq_ctrl[0].inte, mask);
}

/* Spin a little, then sleep on IRQ0 with only `source` added to it (each
 * register access is a trip through the emulator). */
static void wait_for(PIO pio, uint32_t fstat_bit, bool want, uint source)
{
    for (int i = 0; i < 64; i++)
        if (!!(pio->fstat & fstat_bit) == want)
            return;
    while (!!(pio->fstat & fstat_bit) != want) {
        bool had = pio->irq_ctrl[0].inte >> source & 1;
        if (!had)
            hw_set_bits(&pio->irq_ctrl[0].inte, 1u << source);
        if (!!(pio->fstat & fstat_bit) != want)
            pio_wait_irq(pio, 100);
        if (!had)
            hw_clear_bits(&pio->irq_ctrl[0].inte, 1u << source);
    }
}

void pio_sm_put_blocking(PIO pio, uint sm, uint32_t data)
{
    /* TXFULL clear; wakes on TXNFULL */
    wait_for(pio, 1u << (16 + sm), false, pis_sm0_tx_fifo_not_full + sm);
    pio_sm_put(pio, sm, data);
}

uint32_t pio_sm_get_blocking(PIO pio, uint sm)
{
    /* RXEMPTY clear; wakes on RXNEMPTY */
    wait_for(pio, 1u << (8 + sm), false, pis_sm0_rx_fifo_not_empty + sm);
    return pio_sm_get(pio, sm);
}

void pio_sm_drain_tx_fifo(PIO pio, uint sm)
{
    uint ins = (pio->sm[sm].shiftctrl >> 17 & 1) ? pio_encode_out(pio_null, 32) : pio_encode_pull(false, false);

    while (!pio_sm_is_tx_fifo_empty(pio, sm))
        pio_sm_exec(pio, sm, ins);
}

/* ------------------------------------------------------------ claiming */

void pio_sm_claim(PIO pio, uint sm)
{
    struct block *b = blk(pio);
    if (b && sm < 4)
        b->claimed_sm |= 1u << sm;
}

void pio_claim_sm_mask(PIO pio, uint mask)
{
    struct block *b = blk(pio);
    if (b)
        b->claimed_sm |= mask & 0xf;
}

void pio_sm_unclaim(PIO pio, uint sm)
{
    struct block *b = blk(pio);
    if (b && sm < 4)
        b->claimed_sm &= ~(1u << sm);
}

bool pio_sm_is_claimed(PIO pio, uint sm)
{
    struct block *b = blk(pio);
    return b && sm < 4 && b->claimed_sm >> sm & 1;
}

int pio_claim_unused_sm(PIO pio, bool required)
{
    struct block *b = blk(pio);

    for (int k = 0; b && k < 4; k++)
        if (!(b->claimed_sm >> k & 1)) {
            b->claimed_sm |= 1u << k;
            return k;
        }
    if (required) {
        fprintf(stderr, "pio: no free state machine on PIO%u\n", pio_get_index(pio));
        exit(1);
    }
    errno = EBUSY;
    return -1;
}

/* ------------------------------------------------------------ assembler results */

pio_sm_config pio_asm_default_config(const pio_asm_program_t *p, uint offset)
{
    pio_sm_config c = pio_get_default_sm_config();

    sm_config_set_wrap(&c, offset + p->wrap_target, offset + p->wrap);
    if (p->sideset_bits || p->sideset_opt)
        sm_config_set_sideset(&c, p->sideset_bits + p->sideset_opt, p->sideset_opt, p->sideset_pindirs);
    if (p->has & 1) {
        sm_config_set_in_pin_count(&c, p->in_count);
        sm_config_set_in_shift(&c, p->in_right, p->in_auto, p->in_threshold);
    }
    if (p->has & 2) {
        sm_config_set_out_pin_count(&c, p->out_count);
        sm_config_set_out_shift(&c, p->out_right, p->out_auto, p->out_threshold);
    }
    if (p->has & 4)
        sm_config_set_set_pin_count(&c, p->set_count);
    if (p->has & 8)
        sm_config_set_fifo_join(&c, (enum pio_fifo_join)p->fifo);
    if (p->has & 16)
        sm_config_set_mov_status(&c, (enum pio_mov_status_type)p->mov_status_type, p->mov_status_n);
    if (p->has & 32)
        sm_config_set_clkdiv(&c, p->clock_div);
    return c;
}

bool pio_asm_define(const pio_asm_program_t *p, const char *name, int *value)
{
    for (int i = 0; i < p->ndefines; i++)
        if (!strcmp(p->defines[i].name, name)) {
            *value = p->defines[i].value;
            return true;
        }
    return false;
}

/* ------------------------------------------------------------ DMA */

void *pio_dma_buffer(PIO pio, size_t size)
{
    struct block *b = blk(pio);
    uint32_t n;
    void *m;

    if (!b || !size || size > RVEMU_PIO_DMA_MAX_BUF) {
        errno = EINVAL;
        return NULL;
    }
    if (b->dma) {
        if (size > b->dma_size) {
            errno = EINVAL;
            return NULL;
        }
        return b->dma;
    }
    n = (size + 4095) & ~4095u;
    if (ioctl(b->fd, RVEMU_PIO_DMA_ALLOC, &n) < 0)
        return NULL;
    m = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, RVEMU_PIO_DMA_BUF_OFFSET);
    if (m == MAP_FAILED)
        return NULL;
    b->dma = m;
    b->dma_size = n;
    return m;
}

static int dma_op(PIO pio, unsigned long cmd, struct rvemu_pio_dma *d)
{
    struct block *b = blk(pio);

    if (!b) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(b->fd, cmd, d);
}

int pio_dma_start(PIO pio, int channel, const pio_dma_config *c)
{
    struct rvemu_pio_dma d;

    memset(&d, 0, sizeof(d));
    d.channel = channel < 0 ? RVEMU_PIO_DMA_NEW : (uint32_t)channel;
    d.sm = c->sm;
    d.flags = (c->from_sm ? RVEMU_PIO_DMA_FROM_SM : 0) | (c->endless ? RVEMU_PIO_DMA_ENDLESS : 0);
    d.size = c->size;
    d.offset = c->offset;
    d.count = c->count;
    d.ring_bits = c->ring_bits;
    if (dma_op(pio, RVEMU_PIO_DMA_START, &d) < 0)
        return -1;
    return d.channel;
}

int pio_dma_wait(PIO pio, int channel, int timeout_ms)
{
    struct rvemu_pio_dma d;

    memset(&d, 0, sizeof(d));
    d.channel = channel;
    d.timeout_ms = timeout_ms;
    if (dma_op(pio, RVEMU_PIO_DMA_WAIT, &d) < 0)
        return -1;
    if (d.error) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int pio_dma_status(PIO pio, int channel, uint32_t *remaining, uint32_t *pos)
{
    struct rvemu_pio_dma d;

    memset(&d, 0, sizeof(d));
    d.channel = channel;
    if (dma_op(pio, RVEMU_PIO_DMA_STATUS, &d) < 0)
        return -1;
    if (remaining)
        *remaining = d.remaining;
    if (pos)
        *pos = d.pos;
    if (d.error) {
        errno = EIO;
        return -1;
    }
    return d.busy;
}

int pio_dma_stop(PIO pio, int channel)
{
    struct rvemu_pio_dma d;

    memset(&d, 0, sizeof(d));
    d.channel = channel;
    return dma_op(pio, RVEMU_PIO_DMA_STOP, &d) < 0 ? -1 : 0;
}

/* Through the buffer, a buffer's worth at a time. */
static int dma_copy(PIO pio, uint sm, void *data, size_t bytes, uint size, bool from_sm)
{
    struct block *b = blk(pio);
    uint8_t *p = data;
    int ch = -1, ret = 0;

    if (!b || (size != 1 && size != 2 && size != 4) || bytes % size) {
        errno = EINVAL;
        return -1;
    }
    if (!b->dma && !pio_dma_buffer(pio, 16384))
        return -1;
    while (bytes) {
        size_t n = bytes < b->dma_size ? bytes : b->dma_size;
        pio_dma_config c = { sm, from_sm, size, 0, (uint32_t)(n / size), 0, false };
        if (!from_sm)
            memcpy(b->dma, p, n);
        if ((ch = pio_dma_start(pio, ch, &c)) < 0 || pio_dma_wait(pio, ch, 0) < 0) {
            ret = -1;
            break;
        }
        if (from_sm)
            memcpy(p, b->dma, n);
        p += n;
        bytes -= n;
    }
    if (ch >= 0) {
        int e = errno;
        pio_dma_stop(pio, ch);
        errno = e;
    }
    return ret;
}

int pio_sm_dma_write(PIO pio, uint sm, const void *data, size_t bytes, uint size)
{
    return dma_copy(pio, sm, (void *)data, bytes, size, false);
}

int pio_sm_dma_read(PIO pio, uint sm, void *data, size_t bytes, uint size)
{
    return dma_copy(pio, sm, data, bytes, size, true);
}
