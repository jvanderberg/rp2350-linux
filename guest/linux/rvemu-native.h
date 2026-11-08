/* Kernel-internal: one native buffer operation (SBI 0x0a000001), shared by
 * rvemu-crypto.c and rvemu-ssh.c. */
#ifndef RVEMU_NATIVE_H
#define RVEMU_NATIVE_H
#include <asm/sbi.h>
/* The firmware paused the operation, or it was abandoned for a fatal signal. */
#define RVEMU_NATIVE_INTERRUPTED (-101)
struct sbiret rvemu_native_call(unsigned long op, phys_addr_t out, unsigned long cap,
                                phys_addr_t in, unsigned long len,
                                phys_addr_t params, unsigned long plen);
#endif
