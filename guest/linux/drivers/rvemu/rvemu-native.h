/* Kernel-internal: one operation of the rvemu machine's native buffer engine
 * (SBI extension RVEMU_NATIVE_EXT), shared by crypto-rvemu.c and
 * ssh-rvemu.c. */
#ifndef RVEMU_NATIVE_H
#define RVEMU_NATIVE_H
#include <asm/sbi.h>
#define RVEMU_NATIVE_EXT 0x0a000001
/* The operation was abandoned for a fatal signal. */
#define RVEMU_NATIVE_INTERRUPTED (-101)
/* Runs the operation to completion, in time slices with the guest scheduling
 * between them; buffers are physical addresses of kernel memory. */
struct sbiret rvemu_native_call(unsigned long op, phys_addr_t out, unsigned long cap,
                                phys_addr_t in, unsigned long len,
                                phys_addr_t params, unsigned long plen);
#endif
