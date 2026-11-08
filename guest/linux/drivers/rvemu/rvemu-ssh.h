/* /dev/rvemu-ssh (ssh-rvemu.c): SSH packets, and /dev/rvemu-bench
 * (bench-rvemu.c): benchmark points. Fixed-width RV32 ABI. */
#ifndef RVEMU_SSH_H
#define RVEMU_SSH_H
#include <linux/types.h>
#include <linux/ioctl.h>
struct rvemu_ssh_request {
    __u32 operation; /* 4 seal, 5 authenticate/open */
    __u32 source;
    __u32 destination;
    __u32 length; /* input bytes, including tag for open */
    __u8 key[64]; /* payload key, header key */
    __u32 sequence;
    __u32 reserved;
};
#define RVEMU_SSH_CRYPT _IOW('R', 0x40, struct rvemu_ssh_request)
#define RVEMU_BENCH_POINT _IOW('R', 0x41, __u32)
/* Firmware prints its recorded points and diagnostics on the console and
 * clears them; returns the number of points printed. */
#define RVEMU_BENCH_REPORT _IO('R', 0x43)
#endif
