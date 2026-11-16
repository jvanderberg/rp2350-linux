/* /dev/pio1, /dev/pio2 (pio-rvemu.c): the RP2350's PIO1 and PIO2.
 *
 * One process at a time has a block (a second open gets EBUSY); it starts
 * reset (state machines stopped, FIFOs empty, instruction memory cleared)
 * and is reset again, its pins given back, when the last reference goes.
 *
 * mmap offset 0, up to RVEMU_PIO_MAP_SIZE: the block's registers in the
 * chip's layout, with the XOR, SET and CLR aliases at +0x1000, +0x2000 and
 * +0x3000 (the pico-sdk's pio_hw_t). A block drives only the pins it has
 * claimed (RVEMU_PIO_PIN_CLAIM); its inputs see every pin.
 *
 * read() of 4 bytes waits until the block's IRQ0 output is up (IRQ0_INTS
 * not 0) and returns IRQ0_INTS; poll() says POLLIN while it is up. The
 * program clears the source (IRQ flags, FIFO levels) itself.
 *
 * Fixed-width RV32 ABI. */
#ifndef RVEMU_PIO_H
#define RVEMU_PIO_H
#include <linux/types.h>
#include <linux/ioctl.h>

#define RVEMU_PIO_MAP_SIZE 0x4000

struct rvemu_pio_info {
    __u32 block;     /* 1 or 2: PIO1, PIO2 */
    __u32 clk_hz;    /* the block's clock (clk_sys) */
    __u32 valid[2];  /* pins it may claim (bit N = GPN; 0-31, 32-47) */
    __u32 claimed[2];/* pins it has */
};

#define RVEMU_PIO_INFO _IOR('R', 0x50, struct rvemu_pio_info)
/* Give pin N (0-47) to the block: EBUSY when gpiolib or the other block
 * has it, EINVAL when the board uses it for something else. */
#define RVEMU_PIO_PIN_CLAIM _IOW('R', 0x51, __u32)
/* Give it back to its GPIO line, as gpiolib last set it up. */
#define RVEMU_PIO_PIN_RELEASE _IOW('R', 0x52, __u32)

/* DMA between a buffer and a state machine's FIFO, on the board's small
 * pool of channels (EBUSY when they are all in use).
 *
 * The buffer: one per open, a page multiple up to RVEMU_PIO_DMA_MAX_BUF,
 * zeroed, mapped with mmap at offset RVEMU_PIO_DMA_BUF_OFFSET. */
#define RVEMU_PIO_DMA_BUF_OFFSET 0x10000
#define RVEMU_PIO_DMA_MAX_BUF 0x10000
#define RVEMU_PIO_DMA_ALLOC _IOW('R', 0x53, __u32)

#define RVEMU_PIO_DMA_FROM_SM 1  /* RX FIFO to the buffer (else buffer to TX FIFO) */
#define RVEMU_PIO_DMA_ENDLESS 2  /* never ends: with a ring only */
#define RVEMU_PIO_DMA_NEW 0xffffffffu

struct rvemu_pio_dma {
    __u32 channel;    /* START: RVEMU_PIO_DMA_NEW or one of ours; set */
    __u32 sm;         /* 0-3 */
    __u32 flags;
    __u32 size;       /* bytes a transfer: 1, 2 or 4 */
    __u32 offset;     /* where in the buffer */
    __u32 count;      /* transfers */
    __u32 ring_bits;  /* 0, or the memory side wraps in 1 << ring_bits
                       * bytes (at most 4096; offset aligned to it) */
    __u32 timeout_ms; /* WAIT: 0 none */
    /* STATUS, WAIT */
    __u32 busy;
    __u32 error;
    __u32 remaining;  /* transfers left */
    __u32 pos;        /* bytes past offset the memory side is at */
};

#define RVEMU_PIO_DMA_START _IOWR('R', 0x54, struct rvemu_pio_dma)
#define RVEMU_PIO_DMA_STATUS _IOWR('R', 0x55, struct rvemu_pio_dma)
/* Until the channel is done (ETIMEDOUT after timeout_ms). */
#define RVEMU_PIO_DMA_WAIT _IOWR('R', 0x56, struct rvemu_pio_dma)
/* Stop it and give the channel back. */
#define RVEMU_PIO_DMA_STOP _IOW('R', 0x57, struct rvemu_pio_dma)
#endif
