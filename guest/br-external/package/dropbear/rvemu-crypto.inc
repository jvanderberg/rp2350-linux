/* Adapter only: the device ABI and native primitives have no SSH dependency. */
#include <sys/ioctl.h>
#include "dbutil.h"
#include "rvemu-crypto.h"
static int rvemu_crypto_fd(void)
{
    static int fd=-2;
    if (fd==-2) {
        const char *mode=getenv("RVEMU_SSH_OFFLOAD");
        fd=mode && !strcmp(mode,"0") ? -1 : open("/dev/rvemu-crypto",O_RDWR|O_CLOEXEC);
    }
    return fd;
}
static int rvemu_crypto(unsigned int op, const void *src, unsigned int len,
        void *dst, unsigned int cap, const void *params, unsigned int plen)
{
    struct rvemu_crypto_request r={0};
    int rc, err, fd=rvemu_crypto_fd();
    if (fd<0 || len>32768 || cap>32768 || plen>4096) return 0;
    r.operation=op; r.source=(uintptr_t)src; r.source_len=len;
    r.destination=(uintptr_t)dst; r.capacity=cap;
    r.parameters=(uintptr_t)params; r.parameters_len=plen;
    rc=ioctl(fd,RVEMU_CRYPTO_RUN,&r); err=errno;
    if (!rc) return r.output_len==cap ? 1 : -1;
    if (err==ENOTTY || err==EOPNOTSUPP || err==ENODEV || err==ENOMEM) return 0;
    return -1; /* validation/authentication failures never fall back */
}
