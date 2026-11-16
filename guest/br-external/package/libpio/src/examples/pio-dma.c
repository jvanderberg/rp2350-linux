/* DMA between PIO1 and memory, checked:
 *  1. echo: a state machine sends back each word inverted; one channel
 *     feeds it WORDS words from the DMA buffer, another takes the answers
 *     into it, with no CPU per word. Reports words/s.
 *  2. ring: a state machine counts down as fast as its clock allows and
 *     pushes every count; an endless channel keeps the last 4 KiB of them
 *     in a ring. Checks the ring holds consecutive counts.
 *
 *   cc -o pio-dma pio-dma.c -lpio
 *   ./pio-dma [WORDS]
 */
#include <pio.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int load(PIO pio, uint sm, const char *src, float div)
{
    char err[128];
    pio_asm_program_t p;
    if (pio_assemble(src, NULL, &p, err, sizeof(err)) < 0) {
        fprintf(stderr, "%s\n", err);
        exit(1);
    }
    int off = pio_add_program(pio, &p.program);
    pio_sm_config c = pio_asm_default_config(&p, off);
    sm_config_set_clkdiv(&c, div);
    pio_sm_init(pio, sm, off, &c);
    return off;
}

int main(int argc, char **argv)
{
    uint32_t n = argc > 1 ? strtoul(argv[1], NULL, 0) : 4096, bad = 0;
    PIO pio = pio_open(1);
    if (!pio) {
        perror("pio_open");
        return 1;
    }
    if (n < 1 || n > 4096)
        n = 4096;
    uint32_t *buf = pio_dma_buffer(pio, 65536);
    if (!buf) {
        perror("pio_dma_buffer");
        return 1;
    }

    /* 1: echo, words out of buf[0..n), answers into buf[8192..) */
    load(pio, 0, ".wrap_target\n pull\n mov isr, ~osr\n push\n.wrap\n", 1);
    uint32_t x = 12345;
    for (uint32_t i = 0; i < n; i++)
        buf[i] = x = x * 1664525u + 1013904223u;
    pio_sm_set_enabled(pio, 0, true);
    pio_dma_config rx = { 0, true, 4, 8192 * 4, n, 0, false };
    pio_dma_config tx = { 0, false, 4, 0, n, 0, false };
    double t0 = now();
    int crx = pio_dma_start(pio, -1, &rx);
    int ctx = pio_dma_start(pio, -1, &tx);
    if (crx < 0 || ctx < 0) {
        perror("pio_dma_start");
        return 1;
    }
    if (pio_dma_wait(pio, ctx, 5000) < 0 || pio_dma_wait(pio, crx, 5000) < 0) {
        uint32_t left = 0;
        pio_dma_status(pio, crx, &left, NULL);
        fprintf(stderr, "echo: %s (%u words not back)\n", strerror(errno), left);
        return 1;
    }
    double t = now() - t0;
    for (uint32_t i = 0; i < n; i++)
        if (buf[8192 + i] != ~buf[i] && bad++ < 5)
            fprintf(stderr, "word %u: sent %08x, got %08x\n", i, buf[i], buf[8192 + i]);
    printf("echo: %u words by DMA, %u wrong, %.0f words/s\n", n, bad, n / t);
    pio_dma_stop(pio, ctx);
    pio_dma_stop(pio, crx);
    pio_sm_set_enabled(pio, 0, false);

    /* 2: the ring, 1024 counts at buf[4096..) */
    load(pio, 1, "top:\n mov isr, x\n push\n jmp x-- top\n jmp top\n", 64);
    pio_dma_config ring = { 1, true, 4, 4096 * 4, 1u << 27, 12, true };
    int cring = pio_dma_start(pio, -1, &ring);
    if (cring < 0) {
        perror("ring");
        return 1;
    }
    pio_sm_set_enabled(pio, 1, true);
    usleep(300000);
    uint32_t pos0, pos1;
    pio_dma_status(pio, cring, NULL, &pos0);
    usleep(300000);
    int busy = pio_dma_status(pio, cring, NULL, &pos1);
    /* stop the counter, then read the ring: one break at most, where the
     * newest count meets the oldest */
    pio_sm_set_enabled(pio, 1, false);
    usleep(10000);
    pio_dma_stop(pio, cring);
    int breaks = 0;
    for (int i = 0; i < 1024; i++) {
        uint32_t a = buf[4096 + i], b = buf[4096 + (i + 1) % 1024];
        if (b != a - 1)
            breaks++;
    }
    printf("ring: busy %d, position %u then %u, %d break%s in 1024 counts (newest %08x)\n", busy, pos0, pos1,
           breaks, breaks == 1 ? "" : "s", buf[4096 + (pos1 / 4 + 1023) % 1024]);
    pio_close(pio);
    return bad || breaks > 1 || busy != 1;
}
