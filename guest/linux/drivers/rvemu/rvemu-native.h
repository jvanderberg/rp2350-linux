/* Kernel-internal: one operation of the rvemu machine's native buffer engine
 * (SBI extension RVEMU_NATIVE_EXT), shared by crypto-rvemu.c and
 * ssh-rvemu.c. */
#ifndef RVEMU_NATIVE_H
#define RVEMU_NATIVE_H
#include <asm/sbi.h>
#include "rvemu-crypto.h"
#define RVEMU_NATIVE_EXT 0x0a000001
/* The operation was abandoned for a fatal signal. */
#define RVEMU_NATIVE_INTERRUPTED (-101)
#define RVEMU_NATIVE_MAX_BUFFER RVEMU_CRYPTO_MAX_BUFFER
#define RVEMU_NATIVE_MAX_PARAMS RVEMU_CRYPTO_MAX_PARAMS
/* The engine's bounce buffers: one set, allocated at boot, which the
 * firmware reads and writes by physical address. */
struct rvemu_native_buffers {
    unsigned char *in, *out; /* RVEMU_NATIVE_MAX_BUFFER bytes each */
    unsigned char *params;   /* RVEMU_NATIVE_MAX_PARAMS bytes */
};
/* Takes the buffers, waiting while another operation has them; ERR_PTR
 * -EINTR on a fatal signal, -ENODEV when the engine is absent. */
struct rvemu_native_buffers *rvemu_native_get(void);
/* Wipes the bytes used of each buffer and gives them back. */
void rvemu_native_put(unsigned long in_len, unsigned long out_len, unsigned long plen);
/* Runs the operation on the buffers (the caller holds them) to completion,
 * in time slices with the guest scheduling between them: len bytes of in
 * and plen of params, at most cap bytes into out. */
struct sbiret rvemu_native_call(unsigned long op, unsigned long cap, unsigned long len,
                                unsigned long plen);
#endif
