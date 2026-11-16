/* WS2812 LEDs on a pin: COUNT of them set to one colour.
 *
 *   pioasm ws2812.pio ws2812.pio.h
 *   cc -o ws2812 ws2812.c -lpio
 *   ./ws2812 PIN COUNT RRGGBB
 */
#include <pio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "ws2812.pio.h"

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s PIN COUNT RRGGBB\n", argv[0]);
        return 1;
    }
    int pin = atoi(argv[1]), count = atoi(argv[2]);
    uint32_t rgb = strtoul(argv[3], NULL, 16);
    /* the LEDs take green, red, blue, most significant bit first */
    uint32_t grb = ((rgb >> 8 & 0xff) << 16 | (rgb >> 16 & 0xff) << 8 | (rgb & 0xff)) << 8;
    PIO pio = pio_open(1);
    if (!pio) {
        perror("pio_open");
        return 1;
    }
    int off = pio_add_program(pio, &ws2812_program);
    if (off < 0 || ws2812_program_init(pio, 0, off, pin, 800000, false) < 0) {
        perror("ws2812");
        return 1;
    }
    for (int i = 0; i < count; i++)
        pio_sm_put_blocking(pio, 0, grb);
    /* the last bits out of the shift register, then the reset gap */
    while (!pio_sm_is_tx_fifo_empty(pio, 0))
        ;
    usleep(1000);
    pio_close(pio);
    return 0;
}
