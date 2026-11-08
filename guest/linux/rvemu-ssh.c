// SPDX-License-Identifier: GPL-2.0
/* Experimental SSH packet offload. Per-open contiguous buffers avoid
 * allocating/wiping whole slabs for each packet. Never pass user pointers
 * or vmapped kernel-stack addresses to firmware. */
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <asm/sbi.h>
#include <asm/io.h>
#include "rvemu-ssh.h"
#define EXT 0x0a000001
#define MAX_BYTES 32768

struct rvemu_ssh_context {
    struct mutex lock;
    unsigned char *input, *output, *params;
};
static void free_context(struct rvemu_ssh_context *c)
{
    if (!c) return;
    kfree_sensitive(c->params);
    kfree_sensitive(c->output);
    kfree_sensitive(c->input);
    kfree_sensitive(c);
}
static int rvemu_ssh_open(struct inode *inode, struct file *file)
{
    struct rvemu_ssh_context *c = kzalloc(sizeof(*c), GFP_KERNEL);
    if (!c) return -ENOMEM;
    c->input = kmalloc(MAX_BYTES, GFP_KERNEL);
    c->output = kmalloc(MAX_BYTES, GFP_KERNEL);
    c->params = kmalloc(72, GFP_KERNEL);
    if (!c->input || !c->output || !c->params) {
        free_context(c);
        return -ENOMEM;
    }
    mutex_init(&c->lock);
    file->private_data = c;
    return nonseekable_open(inode, file);
}
static int rvemu_ssh_release(struct inode *inode, struct file *file)
{
    free_context(file->private_data);
    return 0;
}
static long rvemu_ssh_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct rvemu_ssh_context *c = file->private_data;
    struct rvemu_ssh_request r = {0};
    struct sbiret result;
    unsigned int outlen, i;
    long ret = -EINVAL;
    if (cmd != RVEMU_SSH_CRYPT) return -ENOTTY;
    if (copy_from_user(&r, (void __user *)arg, sizeof(r))) {
        ret = -EFAULT;
        goto done;
    }
    if (r.reserved || (r.operation != 4 && r.operation != 5)) goto done;
    if (r.length > MAX_BYTES || r.length < (r.operation == 4 ? 4 : 20)) {
        ret = -EMSGSIZE;
        goto done;
    }
    outlen = r.operation == 4 ? r.length + 16 : r.length - 16;
    if (outlen > MAX_BYTES) { ret = -EMSGSIZE; goto done; }
    /* A descriptor can be shared across threads/fork: serialize its buffers. */
    mutex_lock(&c->lock);
    if (copy_from_user(c->input, (void __user *)(unsigned long)r.source, r.length)) {
        ret = -EFAULT;
        goto unlock;
    }
    memcpy(c->params, r.key, 64);
    memset(c->params + 64, 0, 4);
    for (i = 0; i < 4; i++) c->params[68+i] = r.sequence >> (24-8*i);
    result = sbi_ecall(EXT, r.operation, virt_to_phys(c->output), outlen,
                      virt_to_phys(c->input), r.length, virt_to_phys(c->params), 72);
    if (result.error) {
        ret = result.error == -2 ? -EOPNOTSUPP :
              result.error == -1 ? -EBADMSG : -EINVAL;
        goto unlock;
    }
    if (result.value != outlen) { ret = -EIO; goto unlock; }
    ret = copy_to_user((void __user *)(unsigned long)r.destination, c->output, outlen) ? -EFAULT : 0;
unlock:
    memzero_explicit(c->params, 72);
    if (ret) {
        memzero_explicit(c->input, r.length);
        memzero_explicit(c->output, outlen);
    }
    mutex_unlock(&c->lock);
done:
    memzero_explicit(&r, sizeof(r));
    return ret;
}
static const struct file_operations rvemu_ssh_ops = {
    .owner = THIS_MODULE,
    .open = rvemu_ssh_open,
    .release = rvemu_ssh_release,
    .unlocked_ioctl = rvemu_ssh_ioctl,
};
static struct miscdevice rvemu_ssh_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "rvemu-ssh",
    .fops = &rvemu_ssh_ops,
    .mode = 0600,
};
static int __init rvemu_ssh_init(void)
{
    if (sbi_probe_extension(EXT) <= 0) return -ENODEV;
    return misc_register(&rvemu_ssh_dev);
}
device_initcall(rvemu_ssh_init);

/* Measurement is a separate device so software-only firmware can benchmark
 * the same connection boundaries. No crypto buffers or keys are involved. */
static long rvemu_bench_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    __u32 tag;
    struct sbiret r;
    if (cmd != RVEMU_BENCH_POINT) return -ENOTTY;
    if (copy_from_user(&tag, (void __user *)arg, sizeof(tag))) return -EFAULT;
    if (tag > 3) return -EINVAL;
    r = sbi_ecall(0x0a000002, 0, tag, 0, 0, 0, 0, 0);
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
    if (sbi_probe_extension(0x0a000002) <= 0) return -ENODEV;
    return misc_register(&rvemu_bench_dev);
}
device_initcall(rvemu_bench_init);
MODULE_LICENSE("GPL");
