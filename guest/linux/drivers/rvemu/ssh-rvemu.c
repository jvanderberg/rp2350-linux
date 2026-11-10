// SPDX-License-Identifier: GPL-2.0
/* /dev/rvemu-ssh: one SSH chacha20-poly1305@openssh.com packet per ioctl
 * (rvemu-ssh.h), sealed or opened by the rvemu machine's native engine.
 * Packets go through the engine's kernel bounce buffers (rvemu-native.h),
 * which the firmware reads and writes by physical address; user pointers
 * never reach it. */
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/err.h>
#include <asm/sbi.h>
#include "rvemu-ssh.h"
#include "rvemu-native.h"
#define MAX_BYTES RVEMU_NATIVE_MAX_BUFFER

static long rvemu_ssh_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct rvemu_native_buffers *b;
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
    b = rvemu_native_get();
    if (IS_ERR(b)) { ret = PTR_ERR(b); goto done; }
    if (copy_from_user(b->in, (void __user *)(unsigned long)r.source, r.length)) {
        ret = -EFAULT;
        goto unlock;
    }
    memcpy(b->params, r.key, 64);
    memset(b->params + 64, 0, 4);
    for (i = 0; i < 4; i++) b->params[68+i] = r.sequence >> (24-8*i);
    result = rvemu_native_call(r.operation, outlen, r.length, 72);
    if (result.error) {
        ret = result.error == -2 ? -EOPNOTSUPP :
              result.error == -1 ? -EBADMSG :
              result.error == RVEMU_NATIVE_INTERRUPTED ? -EINTR : -EINVAL;
        goto unlock;
    }
    if (result.value != outlen) { ret = -EIO; goto unlock; }
    ret = copy_to_user((void __user *)(unsigned long)r.destination, b->out, outlen) ? -EFAULT : 0;
unlock:
    rvemu_native_put(r.length, outlen, 72);
done:
    memzero_explicit(&r, sizeof(r));
    return ret;
}
static const struct file_operations rvemu_ssh_ops = {
    .owner = THIS_MODULE,
    .open = nonseekable_open,
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
    if (sbi_probe_extension(RVEMU_NATIVE_EXT) <= 0) return -ENODEV;
    return misc_register(&rvemu_ssh_dev);
}
device_initcall(rvemu_ssh_init);

MODULE_LICENSE("GPL");
