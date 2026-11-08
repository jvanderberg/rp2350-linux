/* Negative tests against the actual kernel boundary in the disposable guest. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "rvemu-ssh.h"
static int expect(int fd, unsigned long op, void *arg, int error) {
    int rc = ioctl(fd, op, arg);
    if (rc != -1 || errno != error) {
        fprintf(stderr, "ioctl probe: rc=%d errno=%d expected=%d\n", rc, errno, error);
        return 1;
    }
    return 0;
}
int main(void) {
    unsigned char input[20] = {0}, output[20];
    struct rvemu_ssh_request r = {0};
    int fd = open("/dev/rvemu-ssh", O_RDWR), failed = 0;
    if (fd < 0) return 1;
    failed |= expect(fd, 0, &r, ENOTTY);
    failed |= expect(fd, RVEMU_SSH_CRYPT, NULL, EFAULT);
    r.operation = 99;
    failed |= expect(fd, RVEMU_SSH_CRYPT, &r, EINVAL);
    r.operation = 4; r.length = 32769;
    failed |= expect(fd, RVEMU_SSH_CRYPT, &r, EMSGSIZE);
    r.length = 4;
    failed |= expect(fd, RVEMU_SSH_CRYPT, &r, EFAULT);
    r.operation = 5; r.length = 20;
    r.source = (uintptr_t)input; r.destination = (uintptr_t)output;
    memset(output, 0xa5, sizeof(output));
    failed |= expect(fd, RVEMU_SSH_CRYPT, &r, EBADMSG);
    for (unsigned int i=0; i<sizeof(output); i++) if (output[i] != 0xa5) failed=1;
    r.operation = 4; r.length = 4; r.destination = 0;
    failed |= expect(fd, RVEMU_SSH_CRYPT, &r, EFAULT);
    close(fd);
    if (!failed) puts("SSH IOCTL BOUNDS AND AUTH PASS");
    return failed;
}
