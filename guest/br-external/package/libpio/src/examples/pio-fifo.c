/* PIO1's FIFOs, no pins: a state machine sends back each word it gets,
 * inverted. Checks every word and reports the round trips per second
 * through the emulator.
 *
 *   cc -o pio-fifo pio-fifo.c -lpio
 *   ./pio-fifo [WORDS]
 */
#include <pio.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *echo =
    ".program echo\n"
    ".wrap_target\n"
    "    pull\n"
    "    mov isr, ~osr\n"
    "    push\n"
    ".wrap\n";

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 10000, bad = 0;
    char err[128];
    pio_asm_program_t prog;
    PIO pio = pio_open(1);
    if (!pio) {
        perror("pio_open");
        return 1;
    }
    if (pio_assemble(echo, "echo", &prog, err, sizeof(err)) < 0) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    int off = pio_add_program(pio, &prog.program);
    pio_sm_config c = pio_asm_default_config(&prog, off);
    pio_sm_init(pio, 0, off, &c);
    pio_sm_set_enabled(pio, 0, true);
    double t0 = now();
    uint32_t x = 1;
    for (int i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        pio_sm_put_blocking(pio, 0, x);
        uint32_t y = pio_sm_get_blocking(pio, 0);
        if (y != ~x && bad++ < 5)
            fprintf(stderr, "word %d: sent %08x, got %08x\n", i, x, y);
    }
    double t = now() - t0;
    /* and the interrupt path: an empty RX FIFO, then one word */
    pio_set_irq0_source_enabled(pio, pis_sm0_rx_fifo_not_empty, true);
    int64_t before = pio_wait_irq(pio, 0);
    pio_sm_put(pio, 0, 0x12345678);
    int64_t ints = pio_wait_irq(pio, 1000);
    uint32_t y = pio_sm_get(pio, 0);
    printf("%d words, %d wrong, %.0f round trips/s; irq before %lld after %#llx, word %08x\n", n, bad, n / t,
           (long long)before, (long long)ints, y);
    pio_close(pio);
    return bad || before != 0 || ints != 1 || y != ~0x12345678u;
}
