// SPDX-License-Identifier: GPL-2.0
/*
 * squashfs's LZ4 blocks decoded by the rvemu machine's native engine (SBI
 * extension RVEMU_NATIVE_EXT, operation 0: a raw LZ4 block), on guest
 * physical memory: one call per block, which the firmware runs directly
 * (not on its workspace) and to completion. See rvemu-lz4.h.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <asm/sbi.h>
#include "rvemu-native.h"
#include "rvemu-lz4.h"

#define OP_LZ4_DECODE 0

static bool rl_engine;

int rvemu_lz4_decode(void *dst, unsigned int cap, const void *src, unsigned int len)
{
	struct sbiret r;

	if (!rl_engine || !virt_addr_valid(dst) || !virt_addr_valid(src) ||
	    len > RVEMU_NATIVE_MAX_BUFFER)
		return -EBUSY;
	cap = min_t(unsigned int, cap, RVEMU_NATIVE_MAX_BUFFER);
	r = sbi_ecall(RVEMU_NATIVE_EXT, OP_LZ4_DECODE, virt_to_phys(dst), cap, virt_to_phys((void *)src), len, 0, 0);
	/* the engine's -1: the block is bad, or doesn't fit in cap */
	if (r.error == -1)
		return -EIO;
	if (r.error || r.value > cap)
		return -EBUSY;
	return r.value;
}

static int __init rl_init(void)
{
	rl_engine = sbi_probe_extension(RVEMU_NATIVE_EXT) > 0;
	return 0;
}
/* before the root filesystem is mounted (after every initcall) */
device_initcall(rl_init);
