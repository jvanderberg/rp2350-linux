/* A square wave on a pin from PIO1, its program assembled at run time.
 *
 *   cc -o squarewave squarewave.c -lpio
 *   ./squarewave PIN HZ [SECONDS]      (no SECONDS: until Ctrl-C)
 *
 * The program is two SET instructions, each held for D+1 cycles: the
 * delay D is picked so the clock divider can make HZ. */
#include <pio.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static void on_signal(int s) { (void)s; stop = 1; }

int main(int argc, char **argv)
{
    char src[256], err[128];
    pio_asm_program_t prog;
    if (argc < 3) {
        fprintf(stderr, "usage: %s PIN HZ [SECONDS]\n", argv[0]);
        return 1;
    }
    int pin = atoi(argv[1]);
    double hz = atof(argv[2]);
    int secs = argc > 3 ? atoi(argv[3]) : -1;
    PIO pio = pio_open(1);
    if (!pio) {
        perror("pio_open");
        return 1;
    }
    double clk = pio_clock_hz(pio);
    /* the longest delay that still leaves a divider of 1 or more */
    int d = 31;
    while (d > 0 && clk / (hz * 2 * (d + 1)) < 1)
        d--;
    double div = clk / (hz * 2 * (d + 1));
    if (div < 1 || div > 65536) {
        fprintf(stderr, "%g Hz is out of reach (clock %g Hz)\n", hz, clk);
        return 1;
    }
    snprintf(src, sizeof(src),
             ".program square\n"
             "    set pindirs, 1\n"
             ".wrap_target\n"
             "    set pins, 1 [%d]\n"
             "    set pins, 0 [%d]\n"
             ".wrap\n", d, d);
    if (pio_assemble(src, NULL, &prog, err, sizeof(err)) < 0) {
        fprintf(stderr, "pio_assemble: %s\n", err);
        return 1;
    }
    int off = pio_add_program(pio, &prog.program);
    int sm = pio_claim_unused_sm(pio, true);
    if (off < 0 || pio_gpio_init(pio, pin) < 0) {
        perror("pin");
        return 1;
    }
    pio_sm_config c = pio_asm_default_config(&prog, off);
    sm_config_set_set_pins(&c, pin, 1);
    sm_config_set_clkdiv(&c, (float)div);
    if (pio_sm_init(pio, sm, off, &c) < 0) {
        perror("pio_sm_init");
        return 1;
    }
    pio_sm_set_enabled(pio, sm, true);
    printf("GP%d: %.3f Hz (clock %.0f Hz, divider %.4f, delay %d)\n", pin,
           clk / ((uint32_t)(div * 256) / 256.0) / (2 * (d + 1)), clk, div, d);
    fflush(stdout);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    while (!stop && secs != 0) {
        sleep(1);
        if (secs > 0)
            secs--;
    }
    pio_close(pio);
    return 0;
}
