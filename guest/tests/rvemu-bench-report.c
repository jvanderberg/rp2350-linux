/* Ask the firmware to print its benchmark points and diagnostics (native
 * operation counts and stack peaks, firmware stack use) on the console. */
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include "rvemu-ssh.h"
int main(void)
{
    int fd = open("/dev/rvemu-bench", O_RDWR), n;
    if (fd < 0) { perror("/dev/rvemu-bench"); return 1; }
    fflush(stdout);
    n = ioctl(fd, RVEMU_BENCH_REPORT);
    if (n < 0) { perror("benchmark report"); return 1; }
    printf("BENCH REPORT %d points\n", n);
    return 0;
}
