/* PIO1, PIO2 and the DMA pool on a real block (the board, or the Mac
 * emulator's -G), through libpio and the driver. Needs no wiring: a block
 * reads every pad, so one block (at GPIOBASE 0, where index N is GPIO N)
 * checks what the other drove, and Linux drives pins from SIO for a block
 * to read. Uses GP15, GP20, GP21, GP31 and GP33, which must be free.
 *
 *   cc -o pio-selftest pio-selftest.c -lpio -lpthread && ./pio-selftest
 *
 * Prints PASS/FAIL per check and "pio-selftest: N of M passed"; exit 0 when
 * all pass. run.sh runs it with the MicroPython one. */
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <pio.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int checks, fails;

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    fails += !ok;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

/* ---- pins driven by Linux (SIO), with the GPIO uAPI */

static int line_out(unsigned n, int v)
{
    struct gpio_v2_line_request r;
    int c = open("/dev/gpiochip0", O_RDWR);
    memset(&r, 0, sizeof(r));
    r.offsets[0] = n;
    r.num_lines = 1;
    strcpy(r.consumer, "pio-selftest");
    r.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    r.config.num_attrs = 1;
    r.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    r.config.attrs[0].attr.values = v;
    r.config.attrs[0].mask = 1;
    if (c < 0 || ioctl(c, GPIO_V2_GET_LINE_IOCTL, &r) < 0) {
        perror("line request");
        r.fd = -1;
    }
    close(c);
    return r.fd;
}

static void line_set(int fd, int v)
{
    struct gpio_v2_line_values lv = { .bits = (unsigned)v, .mask = 1 };
    ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &lv);
}

/* ---- a pad's level as a block sees it: IN pins, 1 then PUSH, run from
 * the CPU on a stopped state machine (no program, nothing claimed) */

static int read_pad(PIO pio, uint sm, uint gpio)
{
    pio_sm_set_in_pins(pio, sm, gpio);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_in(pio_pins, 1));
    pio_sm_exec(pio, sm, pio_encode_push(false, false));
    usleep(1000);
    if (pio_sm_is_rx_fifo_empty(pio, sm))
        return -1;
    return pio_sm_get(pio, sm) != 0;
}

/* PIO1 on GPIOBASE 16 (its config names GP33), driving GP21 by SET from
 * the config's own pins; PIO2 on GPIOBASE 0 reads GP21 */
static void test_high_base_output(void)
{
    char d[96];
    PIO p1 = pio_open(1), p2 = pio_open(2);
    if (!p1 || !p2) {
        check(0, "open both blocks", strerror(errno));
        return;
    }
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_set_pins(&c, 21, 1);
    sm_config_set_jmp_pin(&c, 33);
    int r = pio_sm_set_config(p1, 0, &c);
    snprintf(d, sizeof(d), "set_config %d, GPIOBASE %u, SET_BASE index %u", r, pio_get_gpio_base(p1),
             (unsigned)(p1->sm[0].pinctrl >> 5 & 31));
    check(r == 0 && pio_get_gpio_base(p1) == 16, "PIO1 takes GPIOBASE 16 for GP21 and GP33", d);
    check(pio_gpio_init(p1, 21) == 0, "PIO1 claims GP21", NULL);
    pio_sm_exec(p1, 0, pio_encode_set(pio_pindirs, 1));
    pio_sm_exec(p1, 0, pio_encode_set(pio_pins, 1));
    int hi = read_pad(p2, 0, 21);
    pio_sm_exec(p1, 0, pio_encode_set(pio_pins, 0));
    int lo = read_pad(p2, 0, 21);
    snprintf(d, sizeof(d), "PIO2 read %d after set pins 1, %d after set pins 0", hi, lo);
    check(hi == 1 && lo == 0, "PIO1 at GPIOBASE 16 drives GP21 (index 5)", d);

    /* the 48-pin mask calls, by GPIO number */
    pio_sm_set_pins_with_mask(p1, 0, 1ull << 21, 1ull << 21);
    hi = read_pad(p2, 0, 21);
    pio_sm_set_pins_with_mask(p1, 0, 0, 1ull << 21);
    lo = read_pad(p2, 0, 21);
    snprintf(d, sizeof(d), "PIO2 read %d, then %d", hi, lo);
    check(hi == 1 && lo == 0, "pio_sm_set_pins_with_mask(GP21) at GPIOBASE 16", d);

    /* another block's claim on the pin */
    check(pio_gpio_init(p2, 21) < 0 && errno == EBUSY, "PIO2 can't claim PIO1's GP21", strerror(errno));
    pio_close(p1);
    pio_close(p2);
}

/* Linux drives GP33; PIO1, on GPIOBASE 16, reads it as index 17 */
static void test_high_base_input(void)
{
    char d[96];
    int fd = line_out(33, 1);
    PIO p1 = pio_open(1);
    if (fd < 0 || !p1) {
        check(0, "GP33 from Linux, PIO1", strerror(errno));
        return;
    }
    check(pio_gpio_init(p1, 33) < 0 && errno == EBUSY, "PIO1 can't claim GP33 while Linux has it",
          strerror(errno));
    pio_set_gpio_base(p1, 16);
    int hi = read_pad(p1, 1, 33);
    line_set(fd, 0);
    int lo = read_pad(p1, 1, 33);
    snprintf(d, sizeof(d), "read %d with GP33 high, %d low; IN_BASE index %u", hi, lo,
             (unsigned)(p1->sm[1].pinctrl >> 15 & 31));
    check(hi == 1 && lo == 0, "PIO1 at GPIOBASE 16 reads GP33", d);
    close(fd);
    pio_close(p1);
}

/* WAIT GPIO's index on GPIOBASE 16: GP31 (from the base) and GP15 (if it
 * were absolute) held opposite, so only one reading lets the wait finish */
static void test_wait_gpio_index(void)
{
    char err[128], d[128];
    pio_asm_program_t prog;
    int lo31 = line_out(31, 0), hi15 = line_out(15, 1);
    PIO p1 = pio_open(1);
    if (lo31 < 0 || hi15 < 0 || !p1) {
        check(0, "GP15, GP31 from Linux, PIO1", strerror(errno));
        return;
    }
    pio_assemble(".program idle\ntop:\n    jmp top\n", "idle", &prog, err, sizeof(err));
    uint off = pio_add_program(p1, &prog.program);
    pio_set_gpio_base(p1, 16);
    pio_sm_config c = pio_asm_default_config(&prog, off);
    sm_config_set_jmp_pin(&c, 33);
    pio_sm_init(p1, 2, off, &c);
    pio_sm_set_enabled(p1, 2, true);
    pio_sm_exec(p1, 2, pio_encode_wait_gpio(false, 15)); /* wait 0 gpio 15 */
    usleep(2000);
    int stalled0 = pio_sm_is_exec_stalled(p1, 2);
    pio_sm_restart(p1, 2);
    line_set(lo31, 1);
    line_set(hi15, 0);
    pio_sm_exec(p1, 2, pio_encode_wait_gpio(true, 15)); /* wait 1 gpio 15 */
    usleep(2000);
    int stalled1 = pio_sm_is_exec_stalled(p1, 2);
    pio_sm_set_enabled(p1, 2, false);
    pio_sm_restart(p1, 2);
    snprintf(d, sizeof(d), "wait 0 with GP31 low/GP15 high: %s; wait 1 with GP31 high/GP15 low: %s",
             stalled0 ? "stalled" : "done", stalled1 ? "stalled" : "done");
    check(!stalled0 && !stalled1, "WAIT GPIO 15 at GPIOBASE 16 is GP31", d);
    close(lo31);
    close(hi15);
    pio_close(p1);
}

/* a block's pin outputs don't outlive the program that set them */
static void test_reset_between_programs(void)
{
    char d[64];
    PIO p1 = pio_open(1), p2 = pio_open(2);
    pio_gpio_init(p1, 20);
    pio_sm_set_consecutive_pindirs(p1, 0, 20, 1, true);
    pio_sm_set_pins_with_mask(p1, 0, 1ull << 20, 1ull << 20);
    int before = read_pad(p2, 0, 20);
    pio_close(p1);
    p1 = pio_open(1);
    pio_gpio_init(p1, 20);
    pio_sm_set_consecutive_pindirs(p1, 0, 20, 1, true);
    int after = read_pad(p2, 0, 20);
    snprintf(d, sizeof(d), "GP20 %d before close, %d once an output again", before, after);
    check(before == 1 && after == 0, "a reopened block drives its pins low", d);
    pio_close(p1);
    pio_close(p2);
}

static void test_driver_refusals(void)
{
    PIO p1 = pio_open(1);
    int fd = open("/dev/pio1", O_RDWR);
    check(fd < 0 && errno == EBUSY, "a second open of /dev/pio1 is EBUSY", strerror(errno));
    if (fd >= 0)
        close(fd);
    check(pio_gpio_init(p1, 0) < 0 && errno == EINVAL, "GP0 (PSRAM) can't be claimed", strerror(errno));
    check(pio_gpio_init(p1, 48) < 0, "GP48 can't be claimed", strerror(errno));
    void *m = mmap(NULL, 0x5000, PROT_READ, MAP_SHARED, pio_fd(p1), 0);
    check(m == MAP_FAILED, "registers can't be mapped past 16 KiB", NULL);
    /* 0x10000: RVEMU_PIO_DMA_BUF_OFFSET, the DMA buffer's mmap offset */
    m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, pio_fd(p1), 0x10000);
    check(m == MAP_FAILED, "no DMA buffer to map before one is made", NULL);

    check(pio_dma_buffer(p1, 8192) != NULL, "a DMA buffer", NULL);
    static const struct {
        const char *what;
        pio_dma_config c;
    } bad[] = {
        { "4 transfers past the buffer", { 0, false, 4, 8192 - 8, 4, 0, false } },
        { "a 3-byte transfer", { 0, false, 3, 0, 4, 0, false } },
        { "a misaligned word", { 0, false, 4, 2, 4, 0, false } },
        { "no transfers", { 0, false, 4, 0, 0, 0, false } },
        { "a count with the chip's mode bits", { 0, false, 4, 0, 0xf0000001u, 0, false } },
        { "endless without a ring", { 0, true, 4, 0, 1, 0, true } },
        { "a ring past 4 KiB", { 0, true, 4, 0, 1, 13, true } },
        { "a misaligned ring", { 0, true, 4, 64, 1, 8, true } },
        { "state machine 4", { 4, false, 4, 0, 1, 0, false } },
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char w[80];
        int ch = pio_dma_start(p1, -1, &bad[i].c);
        snprintf(w, sizeof(w), "DMA refuses %s", bad[i].what);
        check(ch < 0 && errno == EINVAL, w, ch < 0 ? strerror(errno) : "started");
        if (ch >= 0)
            pio_dma_stop(p1, ch);
    }
    pio_close(p1);
}

/* ---- DMA: an echo, a stop that ends a wait, starts racing stops */

static PIO wp;
static int wch;
static void *waiter(void *x)
{
    *(volatile int *)x = pio_dma_wait(wp, wch, 0);
    return NULL;
}
static void *stopper(void *x)
{
    for (int i = 0; i < 2000; i++)
        pio_dma_stop(wp, i & 3);
    return x;
}

static void test_dma(void)
{
    char err[128], d[96];
    pio_asm_program_t prog;
    static uint32_t src[1024], dst[1024];
    PIO p = wp = pio_open(2);
    pio_assemble(".program echo\n.wrap_target\n    pull\n    mov isr, ~osr\n    push\n.wrap\n", "echo", &prog, err,
                 sizeof(err));
    uint off = pio_add_program(p, &prog.program);
    pio_sm_config c = pio_asm_default_config(&prog, off);
    pio_sm_init(p, 1, off, &c);
    pio_sm_set_enabled(p, 1, true);
    for (int i = 0; i < 1024; i++)
        src[i] = 0x9e3779b9u * (i + 1);
    pio_dma_buffer(p, 16384);
    /* RX into the second half while TX feeds from the first */
    char *buf = pio_dma_buffer(p, 16384);
    memcpy(buf, src, sizeof(src));
    pio_dma_config rx = { 1, true, 4, 8192, 1024, 0, false }, tx = { 1, false, 4, 0, 1024, 0, false };
    int rc = pio_dma_start(p, -1, &rx), tc = pio_dma_start(p, -1, &tx);
    int ok = rc >= 0 && tc >= 0 && pio_dma_wait(p, tc, 2000) == 0 && pio_dma_wait(p, rc, 2000) == 0;
    memcpy(dst, buf + 8192, sizeof(dst));
    int wrong = 0;
    for (int i = 0; i < 1024; i++)
        wrong += dst[i] != ~src[i];
    snprintf(d, sizeof(d), "%d of 1024 wrong", wrong);
    check(ok && !wrong, "DMA echo through a state machine", d);
    pio_dma_stop(p, rc);
    pio_dma_stop(p, tc);
    pio_sm_set_enabled(p, 1, false);

    /* RX from a stopped machine never finishes; a stop from another thread
     * must end a wait with no time limit */
    pthread_t t;
    volatile int wr = -2;
    wch = pio_dma_start(p, -1, &rx);
    pthread_create(&t, NULL, waiter, (void *)&wr);
    usleep(200000);
    pio_dma_stop(p, wch);
    for (int i = 0; i < 30 && wr == -2; i++)
        usleep(100000);
    check(wr != -2, "a stop from another thread ends DMA_WAIT", wr == -2 ? "still waiting after 3 s" : NULL);
    if (wr == -2)
        pthread_detach(t); /* stuck in the driver: exit takes it */
    else
        pthread_join(t, NULL);

    pthread_create(&t, NULL, stopper, NULL);
    int starts = 0;
    for (int i = 0; i < 2000; i++)
        starts += pio_dma_start(p, -1, &rx) >= 0;
    pthread_join(t, NULL);
    for (int ch = 0; ch < 4; ch++)
        pio_dma_stop(p, ch);
    pio_dma_config one = { 1, false, 4, 0, 1, 0, false };
    int ch = pio_dma_start(p, -1, &one);
    snprintf(d, sizeof(d), "%d starts; a channel after: %d", starts, ch);
    check(ch >= 0, "starts racing stops leave the pool usable", d);
    pio_dma_stop(p, ch);
    pio_close(p);
}

int main(void)
{
    test_high_base_output();
    test_high_base_input();
    test_wait_gpio_index();
    test_reset_between_programs();
    test_driver_refusals();
    test_dma();
    printf("pio-selftest: %d of %d passed\n", checks - fails, checks);
    return fails != 0;
}
