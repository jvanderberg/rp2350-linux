/* Every edge on a pin, timestamped by PIO1 and stored by DMA: nothing is
 * lost however close the edges are (down to a few clock cycles), where
 * the GPIO character device's events need about 2 ms between them.
 *
 *   cc -o pio-edges pio-edges.c -lpio
 *   ./pio-edges PIN EDGES [TIMEOUT_MS]
 *
 * Prints how many edges came, and the shortest, longest and mean time
 * between them. X counts down one every two clock cycles; each edge
 * pushes it (rises and falls alternate, starting from the pin's level).
 * The count wraps after 2^32 ticks (about 25 s at 348 MHz). */
#include <pio.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *src =
    ".program edges\n"
    "    mov x, ~null\n"
    "low:\n"
    "    jmp pin rose\n"
    "    jmp x-- low\n"
    "rose:\n"
    "    mov isr, x\n"
    "    push noblock\n"
    "high:\n"
    "    jmp pin still_high\n"
    "    mov isr, x\n"
    "    push noblock\n"
    "    jmp low\n"
    "still_high:\n"
    "    jmp x-- high\n";

int main(int argc, char **argv)
{
    char err[128];
    pio_asm_program_t p;
    if (argc < 3) {
        fprintf(stderr, "usage: %s PIN EDGES [TIMEOUT_MS]\n", argv[0]);
        return 1;
    }
    int pin = atoi(argv[1]);
    uint32_t n = strtoul(argv[2], NULL, 0);
    int timeout = argc > 3 ? atoi(argv[3]) : 10000;
    if (n < 1 || n > 16384)
        n = 16384;
    PIO pio = pio_open(1);
    if (!pio) {
        perror("pio_open");
        return 1;
    }
    if (pio_assemble(src, NULL, &p, err, sizeof(err)) < 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    uint32_t *buf = pio_dma_buffer(pio, 65536);
    if (!buf) {
        perror("pio_dma_buffer");
        return 1;
    }
    int off = pio_add_program(pio, &p.program);
    pio_sm_config c = pio_asm_default_config(&p, off);
    sm_config_set_jmp_pin(&c, pin);
    sm_config_set_in_pins(&c, pin);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    if (pio_sm_init(pio, 0, off, &c) < 0) {
        perror("pio_sm_init");
        return 1;
    }
    pio_dma_config d = { 0, true, 4, 0, n, 0, false };
    int ch = pio_dma_start(pio, -1, &d);
    if (ch < 0) {
        perror("pio_dma_start");
        return 1;
    }
    pio_sm_set_enabled(pio, 0, true);
    printf("ready\n");
    fflush(stdout);
    uint32_t left = n;
    if (pio_dma_wait(pio, ch, timeout) < 0 && errno != ETIMEDOUT) {
        perror("pio_dma_wait");
        return 1;
    }
    pio_dma_status(pio, ch, &left, NULL);
    pio_sm_set_enabled(pio, 0, false);
    pio_dma_stop(pio, ch);
    uint32_t got = n - left;
    double tick = 2.0 / pio_clock_hz(pio);
    uint32_t lo = ~0u, hi = 0;
    for (uint32_t i = 1; i < got; i++) {
        uint32_t dt = buf[i - 1] - buf[i];
        if (dt < lo)
            lo = dt;
        if (dt > hi)
            hi = dt;
    }
    printf("%u edges", got);
    if (got > 1)
        printf(", between them %.3f to %.3f us, mean %.3f us", lo * tick * 1e6, hi * tick * 1e6,
               (double)(buf[0] - buf[got - 1]) / (got - 1) * tick * 1e6);
    printf("\n");
    pio_close(pio);
    return got != n;
}
