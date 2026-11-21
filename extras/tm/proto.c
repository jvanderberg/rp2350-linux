/*
 * proto.c - framing for the client/server socket. See proto.h.
 */
#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>

#include "proto.h"

static int write_all(int fd, const unsigned char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);

        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Header and data go in one writev(), so the other side wakes once per
 * message rather than once for each part: on a slow board every wakeup
 * shows up as typing latency. */
int msg_send(int fd, int type, const void *data, size_t n)
{
    unsigned char hdr[3];
    struct iovec iov[2];
    ssize_t w;

    if (n > MSG_MAX)
        n = MSG_MAX;
    hdr[0] = (unsigned char)type;
    hdr[1] = (unsigned char)(n >> 8);
    hdr[2] = (unsigned char)(n & 0xFF);
    iov[0].iov_base = hdr;
    iov[0].iov_len = 3;
    iov[1].iov_base = (void *)data;
    iov[1].iov_len = n;
    do
        w = writev(fd, iov, 2);
    while (w < 0 && errno == EINTR);
    if (w < 0)
        return -1;
    if ((size_t)w >= 3)             /* finish a short write */
        return write_all(fd, (const unsigned char *)data + (w - 3),
                         n - (size_t)(w - 3));
    if (write_all(fd, hdr + w, 3 - (size_t)w) != 0)
        return -1;
    return write_all(fd, data, n);
}

int msg_send_all(int fd, int type, const void *data, size_t n)
{
    const unsigned char *p = data;

    while (n > 0) {
        size_t k = n > MSG_MAX ? MSG_MAX : n;

        if (msg_send(fd, type, p, k) != 0)
            return -1;
        p += k;
        n -= k;
    }
    return 0;
}

size_t msg_parse(const unsigned char *buf, size_t len, int *type,
                 const unsigned char **data, size_t *n)
{
    size_t size;

    if (len < 3)
        return 0;
    size = (size_t)buf[1] << 8 | buf[2];
    if (len < 3 + size)
        return 0;
    *type = buf[0];
    *data = buf + 3;
    *n = size;
    return 3 + size;
}
