/* Kernel-internal: one raw LZ4 block decoded by the rvemu machine's native
 * engine (lz4-rvemu.c), for squashfs. */
#ifndef RVEMU_LZ4_H
#define RVEMU_LZ4_H
/* Decode the block of @len bytes at @src into at most @cap bytes at @dst
 * (both physically contiguous, linearly mapped). The decoded length, -EIO
 * for a bad block or one that doesn't fit, or -EBUSY when the engine can't
 * take it now (absent, busy, or not this CPU's): decode it in software. */
int rvemu_lz4_decode(void *dst, unsigned int cap, const void *src, unsigned int len);
#endif
