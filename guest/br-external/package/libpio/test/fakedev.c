/* The fake device (fakedev.h). Built without the renaming, so these are
 * the definitions pio.c's renamed calls reach. */
#include "fakedev.h"
#include "rvemu-pio.h"
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>

struct fake_dev fake;

#define FD_BASE 100
static _Alignas(4096) uint8_t regs[2][RVEMU_PIO_MAP_SIZE];
static uint8_t *dma_buf[2];
static uint32_t dma_size[2];

void fake_reset(void)
{
    for (int i = 0; i < 2; i++) {
        free(dma_buf[i]);
        dma_buf[i] = NULL;
        dma_size[i] = 0;
    }
    memset(regs, 0, sizeof(regs));
    memset(&fake, 0, sizeof(fake));
    fake.clk_hz = 348000000;
}

void *fake_regs(int block) { return regs[block - 1]; }

int fake_count(unsigned long cmd, uint32_t arg)
{
    int n = 0;
    for (int i = 0; i < fake.nlog; i++)
        n += fake.log[i].cmd == cmd && fake.log[i].arg == arg;
    return n;
}

int fake_dma_owned(void)
{
    int n = 0;
    for (int i = 0; i < 4; i++)
        n += fake.dma_owner[i] != 0;
    return n;
}

static void note(unsigned long cmd, uint32_t arg)
{
    if (fake.nlog < FAKE_LOG_MAX)
        fake.log[fake.nlog++] = (struct fake_call){ cmd, arg };
}

static int block_of(int fd)
{
    return fd == FD_BASE + 1 || fd == FD_BASE + 2 ? fd - FD_BASE : 0;
}

int fake_open(const char *path, int flags, ...)
{
    (void)flags;
    if (!strcmp(path, "/dev/pio1"))
        return FD_BASE + 1;
    if (!strcmp(path, "/dev/pio2"))
        return FD_BASE + 2;
    errno = ENOENT;
    return -1;
}

int fake_close(int fd) { return block_of(fd) ? 0 : (errno = EBADF, -1); }

void *fake_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    int b = block_of(fd);
    (void)addr, (void)prot, (void)flags;
    if (!b)
        return (errno = EBADF, MAP_FAILED);
    if (off == 0 && len <= RVEMU_PIO_MAP_SIZE)
        return regs[b - 1];
    if (off == RVEMU_PIO_DMA_BUF_OFFSET && dma_buf[b - 1] && len <= dma_size[b - 1])
        return dma_buf[b - 1];
    return (errno = EINVAL, MAP_FAILED);
}

int fake_munmap(void *addr, size_t len) { (void)addr, (void)len; return 0; }

int fake_poll(struct pollfd *p, nfds_t n, int timeout)
{
    (void)timeout;
    for (nfds_t i = 0; i < n; i++)
        p[i].revents = POLLIN;
    return (int)n;
}

ssize_t fake_read(int fd, void *buf, size_t n)
{
    (void)fd;
    memset(buf, 0, n);
    return (ssize_t)n;
}

static int dma(int b, unsigned long cmd, struct rvemu_pio_dma *d)
{
    if (cmd == RVEMU_PIO_DMA_START) {
        fake.starts++;
        note(cmd, d->channel);
        if (fake.start_fails_at && fake.starts == fake.start_fails_at)
            return (errno = EINVAL, -1);
        if (d->channel == RVEMU_PIO_DMA_NEW) {
            uint32_t ch;
            for (ch = 0; ch < 4 && fake.dma_owner[ch]; ch++)
                ;
            if (ch == 4)
                return (errno = EBUSY, -1);
            fake.dma_owner[ch] = b;
            d->channel = ch;
        } else if (d->channel >= 4 || fake.dma_owner[d->channel] != b) {
            return (errno = EINVAL, -1);
        }
        return 0;
    }
    note(cmd, d->channel);
    if (d->channel >= 4 || fake.dma_owner[d->channel] != b)
        return (errno = EINVAL, -1);
    if (cmd == RVEMU_PIO_DMA_STOP)
        fake.dma_owner[d->channel] = 0;
    /* WAIT and STATUS: done at once, no error */
    d->busy = d->error = d->remaining = 0;
    return 0;
}

int fake_ioctl(int fd, unsigned long cmd, ...)
{
    int b = block_of(fd);
    va_list ap;
    void *arg;

    va_start(ap, cmd);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (!b)
        return (errno = EBADF, -1);
    if (cmd == RVEMU_PIO_INFO) {
        struct rvemu_pio_info *i = arg;
        memset(i, 0, sizeof(*i));
        i->block = b;
        i->clk_hz = fake.clk_hz;
        i->valid[0] = 0xffffffff;
        i->valid[1] = 0xffff;
        i->claimed[0] = (uint32_t)fake.claimed[b - 1];
        i->claimed[1] = (uint32_t)(fake.claimed[b - 1] >> 32);
        return 0;
    }
    if (cmd == RVEMU_PIO_PIN_CLAIM || cmd == RVEMU_PIO_PIN_RELEASE) {
        uint32_t pin = *(uint32_t *)arg;
        note(cmd, pin);
        if (pin >= 48)
            return (errno = EINVAL, -1);
        if (cmd == RVEMU_PIO_PIN_RELEASE) {
            fake.claimed[b - 1] &= ~(1ull << pin);
            return 0;
        }
        if (fake.busy >> pin & 1 || fake.claimed[2 - b] >> pin & 1)
            return (errno = EBUSY, -1);
        fake.claimed[b - 1] |= 1ull << pin;
        return 0;
    }
    if (cmd == RVEMU_PIO_DMA_ALLOC) {
        uint32_t n = *(uint32_t *)arg;
        note(cmd, n);
        if (!n || n > RVEMU_PIO_DMA_MAX_BUF || dma_buf[b - 1])
            return (errno = dma_buf[b - 1] ? EBUSY : EINVAL, -1);
        dma_size[b - 1] = (n + 4095) & ~4095u;
        dma_buf[b - 1] = calloc(1, dma_size[b - 1]);
        return 0;
    }
    if (cmd == RVEMU_PIO_DMA_START || cmd == RVEMU_PIO_DMA_STATUS || cmd == RVEMU_PIO_DMA_WAIT ||
        cmd == RVEMU_PIO_DMA_STOP)
        return dma(b, cmd, arg);
    return (errno = ENOTTY, -1);
}
