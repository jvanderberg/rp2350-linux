/* Included by cli-main.c; measurements end before command/channel setup. */
#include <sys/ioctl.h>
#include "rvemu-ssh.h"
#include "session.h"
#include "algo.h"
void rvemu_benchmark(unsigned int tag)
{
    static int fd = -2;
    static unsigned int next = 0;
    __u32 point = tag;
    if (fd == -2) {
        const char *enabled = getenv("RVEMU_SSH_BENCH");
        if (!enabled || strcmp(enabled, "1")) { fd = -1; return; }
        fd = open("/dev/rvemu-bench", O_RDWR | O_CLOEXEC);
        if (fd < 0) dropbear_exit("Cannot open connection benchmark device");
    }
    if (fd < 0 || tag < next) return; /* rekey/repeated authentication */
    if (tag != next || ioctl(fd, RVEMU_BENCH_POINT, &point))
        dropbear_exit("Connection benchmark boundary failed");
    next++;
    /* Report negotiated KEX only AFTER the measured authentication endpoint. */
    if (tag == 3) {
        for (algo_type *a = sshkex; a->name; a++) {
            if (a->data == ses.keys->algo_kex) {
                fprintf(stderr, "SSH CONNECTION KEX %s\n", a->name);
                break;
            }
        }
    }
}
