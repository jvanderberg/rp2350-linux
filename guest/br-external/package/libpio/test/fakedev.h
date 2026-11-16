/* A fake /dev/pio1 and /dev/pio2 for libpio's host tests.
 *
 * The Makefile compiles pio.c with its system calls renamed (-Dopen=fake_open
 * and so on), so pio.c runs unchanged against these: the register block is
 * plain memory, and the driver's ioctls are answered here and logged. */
#ifndef FAKEDEV_H
#define FAKEDEV_H
#include <stdint.h>

#define FAKE_LOG_MAX 64

/* one driver call, as the fake saw it */
struct fake_call {
    unsigned long cmd;
    uint32_t arg;     /* the pin, or the DMA channel */
};

struct fake_dev {
    uint32_t clk_hz;
    uint64_t claimed[2];  /* per block */
    uint64_t busy;        /* pins whose claim gets EBUSY */
    int dma_owner[4];     /* block that has the channel, or 0 */
    int start_fails_at;   /* the Nth DMA_START (1-based) fails with EINVAL; 0 never */
    int starts;
    int nlog;
    struct fake_call log[FAKE_LOG_MAX];
};

extern struct fake_dev fake;

/* back to a fresh device (registers zeroed, nothing claimed or logged) */
void fake_reset(void);
/* the block's register memory, as pio.c maps it */
void *fake_regs(int block);
/* the number of logged calls of `cmd` with `arg` */
int fake_count(unsigned long cmd, uint32_t arg);
/* DMA channels some block still has */
int fake_dma_owned(void);
#endif
