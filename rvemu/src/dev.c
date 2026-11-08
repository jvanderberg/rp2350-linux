/* Devices: 16550-style UART on the host terminal, and a single-context
 * SiFive PLIC (hart 0, S-mode external interrupt). */
#include "dev.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* ---------------------------------------------------------------- console */

static bool interactive;
static bool stdin_open = true;
static bool tty_raw;
static struct termios saved_termios;

static uint8_t rx_buf[256];
static unsigned rx_head, rx_tail; /* ring: head = next read */

static uint8_t out_buf[4096];
static size_t out_len;

static const char *until_str;
static size_t until_len;
static char tail_buf[256];
static size_t tail_len;
static bool ctrl_a;

void console_flush(void)
{
    size_t off = 0;
    while (off < out_len) {
        ssize_t n = write(STDOUT_FILENO, out_buf + off, out_len - off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        off += (size_t) n;
    }
    out_len = 0;
}

void console_putc(cpu_t *c, uint8_t ch)
{
    out_buf[out_len++] = ch;
    if (ch == '\n' || out_len == sizeof(out_buf))
        console_flush();

    if (until_len && !c->stats.until_instret) {
        if (tail_len == sizeof(tail_buf)) {
            memmove(tail_buf, tail_buf + 1, tail_len - 1);
            tail_len--;
        }
        tail_buf[tail_len++] = (char) ch;
        if (tail_len >= until_len &&
            memcmp(tail_buf + tail_len - until_len, until_str, until_len) == 0) {
            c->stats.until_instret = c->instret + 1;
            c->stats.until_unique_insns = c->stats.unique_insns;
            c->halt = HALT_UNTIL;
        }
    }
}

static bool rx_avail(void) { return rx_head != rx_tail; }

int console_getc(cpu_t *c)
{
    (void) c;
    if (!rx_avail())
        return -1;
    uint8_t ch = rx_buf[rx_head];
    rx_head = (rx_head + 1) % sizeof(rx_buf);
    return ch;
}

static void read_host_input(cpu_t *c)
{
    if (!stdin_open)
        return;
    struct pollfd p = {STDIN_FILENO, POLLIN, 0};
    while (((rx_tail + 1) % sizeof(rx_buf)) != rx_head &&
           poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLHUP))) {
        uint8_t ch;
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n <= 0) {
            stdin_open = false;
            return;
        }
        if (ctrl_a) {
            ctrl_a = false;
            if (ch == 'x') {
                c->halt = HALT_USER;
                return;
            }
        } else if (ch == 1) { /* Ctrl-A prefix */
            ctrl_a = true;
            continue;
        }
        rx_buf[rx_tail] = ch;
        rx_tail = (rx_tail + 1) % sizeof(rx_buf);
    }
}

static void restore_tty(void)
{
    if (tty_raw) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
        tty_raw = false;
    }
}

/* ---------------------------------------------------------------- PLIC */

static struct {
    uint32_t prio[32];
    uint32_t pending, enable, threshold;
    uint32_t claimed; /* in service: claimed, not yet completed */
    uint32_t level;   /* device interrupt lines */
} plic;

static void plic_update(cpu_t *c)
{
    plic.pending |= plic.level & ~plic.claimed;
    uint32_t cand = plic.pending & plic.enable;
    bool any = false;
    for (uint32_t i = 1; i < 32 && cand; i++)
        if ((cand & (1u << i)) && plic.prio[i] > plic.threshold) {
            any = true;
            break;
        }
    if (any)
        c->sip |= MIP_SEIP;
    else
        c->sip &= ~MIP_SEIP;
}

static uint32_t plic_claim(void)
{
    uint32_t cand = plic.pending & plic.enable, best = 0, best_prio = 0;
    for (uint32_t i = 1; i < 32; i++)
        if ((cand & (1u << i)) && plic.prio[i] > plic.threshold &&
            plic.prio[i] > best_prio) {
            best = i;
            best_prio = plic.prio[i];
        }
    if (best) {
        plic.pending &= ~(1u << best);
        plic.claimed |= 1u << best;
    }
    return best;
}

static int plic_load(cpu_t *c, uint32_t off, int size, uint32_t *v)
{
    if (size != 4)
        return EXC_LOAD_FAULT;
    if (off < 0x80)
        *v = plic.prio[off >> 2];
    else if (off == 0x1000)
        *v = plic.pending;
    else if (off == 0x2000)
        *v = plic.enable;
    else if (off == 0x200000)
        *v = plic.threshold;
    else if (off == 0x200004) {
        *v = plic_claim();
        plic_update(c);
    } else
        *v = 0;
    return 0;
}

static int plic_store(cpu_t *c, uint32_t off, int size, uint32_t v)
{
    if (size != 4)
        return EXC_STORE_FAULT;
    if (off < 0x80) {
        if (off)
            plic.prio[off >> 2] = v & 7;
    } else if (off == 0x2000) {
        plic.enable = v & ~1u;
    } else if (off == 0x200000) {
        plic.threshold = v & 7;
    } else if (off == 0x200004) {
        if (v < 32)
            plic.claimed &= ~(1u << v);
    }
    plic_update(c);
    return 0;
}

static void plic_set_level(cpu_t *c, uint32_t irq, bool high)
{
    if (high)
        plic.level |= 1u << irq;
    else
        plic.level &= ~(1u << irq);
    plic_update(c);
}

/* ---------------------------------------------------------------- UART */

static struct {
    uint8_t ier, lcr, mcr, dll, dlh, scr;
    bool thre_pending;
} uart;

static uint8_t uart_iir(void)
{
    if ((uart.ier & 1) && rx_avail())
        return 0x04; /* received data available */
    if ((uart.ier & 2) && uart.thre_pending)
        return 0x02; /* transmitter holding register empty */
    return 0x01;     /* no interrupt pending */
}

static void uart_update(cpu_t *c)
{
    plic_set_level(c, UART_IRQ, uart_iir() != 0x01);
}

static int uart_load(cpu_t *c, uint32_t off, int size, uint32_t *v)
{
    if (size != 1)
        return EXC_LOAD_FAULT;
    bool dlab = uart.lcr & 0x80;
    switch (off) {
    case 0:
        if (dlab) {
            *v = uart.dll;
        } else {
            int ch = console_getc(c);
            *v = ch < 0 ? 0 : (uint32_t) ch;
        }
        break;
    case 1: *v = dlab ? uart.dlh : uart.ier; break;
    case 2: {
        uint8_t iir = uart_iir();
        if (iir == 0x02)
            uart.thre_pending = false;
        *v = iir;
        break;
    }
    case 3: *v = uart.lcr; break;
    case 4: *v = uart.mcr; break;
    case 5: *v = 0x60u | (rx_avail() ? 1u : 0u); break; /* THRE|TEMT|DR */
    case 6: *v = 0xB0; break;                           /* DCD|DSR|CTS */
    case 7: *v = uart.scr; break;
    default: *v = 0; break;
    }
    uart_update(c);
    return 0;
}

static int uart_store(cpu_t *c, uint32_t off, int size, uint32_t v)
{
    if (size != 1)
        return EXC_STORE_FAULT;
    bool dlab = uart.lcr & 0x80;
    uint8_t b = (uint8_t) v;
    switch (off) {
    case 0:
        if (dlab) {
            uart.dll = b;
        } else {
            console_putc(c, b);
            uart.thre_pending = true; /* transmits instantly */
        }
        break;
    case 1:
        if (dlab) {
            uart.dlh = b;
        } else {
            /* enabling THRE interrupt with an empty THR raises it */
            if (!(uart.ier & 2) && (b & 2))
                uart.thre_pending = true;
            uart.ier = b & 0x0F;
        }
        break;
    case 3: uart.lcr = b; break;
    case 4: uart.mcr = b; break;
    case 7: uart.scr = b; break;
    default: break; /* FCR and others ignored */
    }
    uart_update(c);
    return 0;
}

/* ---------------------------------------------------------------- bus */

int mmio_load(cpu_t *c, uint32_t pa, int size, uint32_t *v)
{
    if (pa - UART_BASE < UART_SIZE)
        return uart_load(c, pa - UART_BASE, size, v);
    if (pa - PLIC_BASE < PLIC_SIZE)
        return plic_load(c, pa - PLIC_BASE, size, v);
    return EXC_LOAD_FAULT;
}

int mmio_store(cpu_t *c, uint32_t pa, int size, uint32_t v)
{
    if (pa - UART_BASE < UART_SIZE)
        return uart_store(c, pa - UART_BASE, size, v);
    if (pa - PLIC_BASE < PLIC_SIZE)
        return plic_store(c, pa - PLIC_BASE, size, v);
    return EXC_STORE_FAULT;
}

void dev_poll(cpu_t *c)
{
    read_host_input(c);
    uart_update(c);
}

/* Guest executed WFI with nothing to do. Guest time is instruction-driven,
 * so idle time is skipped straight to the next timer deadline. In
 * interactive mode we first wait (in real time, bounded) for keyboard input
 * so an idle shell doesn't spin the host CPU. */
void dev_wfi(cpu_t *c)
{
    dev_poll(c);
    if (c->sip & c->sie)
        return;
    uint64_t now = cpu_time(c);
    if (c->stimecmp <= now)
        return;

    if (interactive && stdin_open) {
        uint64_t delta = c->stimecmp - now;
        uint64_t ms64 = delta / (TIMEBASE_HZ / 1000);
        int ms = ms64 > 100 ? 100 : (int) ms64;
        struct pollfd p = {STDIN_FILENO, POLLIN, 0};
        console_flush();
        if (poll(&p, 1, ms) > 0) {
            dev_poll(c);
            if (c->sip & c->sie)
                return;
        }
        if (c->stimecmp == UINT64_MAX)
            return; /* no timer armed: keep waiting for input */
    } else if (c->stimecmp == UINT64_MAX) {
        fprintf(stderr, "\n[rvemu] guest idle with no timer and no input\n");
        c->halt = HALT_IDLE;
        return;
    }
    uint64_t skip = c->stimecmp - now;
    c->time_skip += skip;
    c->stats.idle_ticks += skip;
}

void dev_init(cpu_t *c, bool is_interactive, const char *until)
{
    (void) c;
    interactive = is_interactive;
    until_str = until;
    until_len = until ? strlen(until) : 0;
    if (until_len > sizeof(tail_buf))
        until_len = sizeof(tail_buf);
    if (interactive && isatty(STDIN_FILENO) &&
        tcgetattr(STDIN_FILENO, &saved_termios) == 0) {
        struct termios raw = saved_termios;
        raw.c_lflag &= ~(tcflag_t) (ICANON | ECHO | ISIG);
        raw.c_iflag &= ~(tcflag_t) (ICRNL | IXON);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
            tty_raw = true;
            atexit(restore_tty);
        }
    }
    if (!interactive)
        stdin_open = false;
}

void dev_shutdown(void)
{
    console_flush();
    restore_tty();
}
