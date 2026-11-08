/* Included by chachapoly.c in the experimental test build. */
#include <sys/ioctl.h>
#include "rvemu-ssh.h"

/* Return 1 on success, 0 to use software, -1 on a fatal/authentication error.
 * An authentication failure NEVER falls back. The environment selects a
 * controlled baseline; absence of the device permits ordinary deployments. */
static int rvemu_ssh_packet(unsigned int seq, const unsigned char *in,
        unsigned char *out, unsigned long len, const unsigned char key[64], int direction)
{
    static int fd = -2;
    struct rvemu_ssh_request r;
    int rc, saved_errno;
    if (fd == -2) {
        const char *mode = getenv("RVEMU_SSH_OFFLOAD");
        fd = mode && !strcmp(mode, "0") ? -1 : open("/dev/rvemu-ssh", O_RDWR | O_CLOEXEC);
    }
    if (fd < 0 || len > 32768 - 16) return 0;
    memset(&r, 0, sizeof(r));
    r.operation = direction == LTC_ENCRYPT ? 4 : 5;
    r.source = (uint32_t)(uintptr_t)in;
    r.destination = (uint32_t)(uintptr_t)out;
    r.length = len + (direction == LTC_DECRYPT ? 16 : 0);
    r.sequence = seq;
    memcpy(r.key, key, 64);
    rc = ioctl(fd, RVEMU_SSH_CRYPT, &r);
    saved_errno = errno;
    m_burn(&r, sizeof(r));
    if (!rc) return 1;
    if (saved_errno == ENODEV || saved_errno == ENOTTY || saved_errno == EOPNOTSUPP ||
        saved_errno == EMSGSIZE || saved_errno == ENOMEM) return 0;
    return -1;
}
