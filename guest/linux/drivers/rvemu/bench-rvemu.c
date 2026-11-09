// SPDX-License-Identifier: GPL-2.0
/* /dev/rvemu-bench: benchmark points for the rvemu machine's firmware (SBI
 * extension 0x0a000002), which records cycle counts at each point and
 * prints them on its console on RVEMU_BENCH_REPORT (rvemu-ssh.h). No
 * crypto buffers or keys are involved. */
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <asm/sbi.h>
#include "rvemu-ssh.h"
#define EXT 0x0a000002

static long rvemu_bench_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    __u32 tag;
    struct sbiret r;
    if (cmd == RVEMU_BENCH_REPORT) {
        r = sbi_ecall(EXT, 1, 0, 0, 0, 0, 0, 0);
        if (r.error) return r.error == SBI_ERR_NOT_SUPPORTED ? -EOPNOTSUPP : -EIO;
        return r.value;
    }
    if (cmd != RVEMU_BENCH_POINT) return -ENOTTY;
    if (copy_from_user(&tag, (void __user *)arg, sizeof(tag))) return -EFAULT;
    if (tag > 3) return -EINVAL;
    r = sbi_ecall(EXT, 0, tag, 0, 0, 0, 0, 0);
    return r.error ? -EIO : 0;
}
static const struct file_operations rvemu_bench_ops = {
    .owner = THIS_MODULE, .unlocked_ioctl = rvemu_bench_ioctl,
};
static struct miscdevice rvemu_bench_dev = {
    .minor = MISC_DYNAMIC_MINOR, .name = "rvemu-bench",
    .fops = &rvemu_bench_ops, .mode = 0600,
};
static int __init rvemu_bench_init(void)
{
    if (sbi_probe_extension(EXT) <= 0) return -ENODEV;
    return misc_register(&rvemu_bench_dev);
}
device_initcall(rvemu_bench_init);
MODULE_LICENSE("GPL");
