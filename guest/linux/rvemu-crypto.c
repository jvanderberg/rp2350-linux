// SPDX-License-Identifier: GPL-2.0
/* Generic synchronous buffer accelerator; keys/coins belong to the caller.
 * Each open owns physical bounce buffers; a mutex permits shared descriptors.
 * Firmware errors never copy output back; all used buffers are wiped. */
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <asm/sbi.h>
#include <asm/io.h>
#include "rvemu-crypto.h"
#include "rvemu-native.h"
#define EXT 0x0a000001
/* Firmware pause protocol: an operation FID with MAY_PEND may return
 * PENDING after a time slice; CONTINUE runs the next slice, ABORT drops it.
 * The firmware runs one operation at a time. */
#define MAY_PEND 0x100
#define CONTINUE 0x200
#define ABORT 0x201
#define PENDING (-100)

static DEFINE_MUTEX(native_lock);

/* Between slices the guest takes its interrupts and other tasks run; the
 * caller's buffers (kernel-owned) stay put until the operation is done. */
struct sbiret rvemu_native_call(unsigned long op, phys_addr_t out, unsigned long cap,
                                phys_addr_t in, unsigned long len,
                                phys_addr_t params, unsigned long plen)
{
    struct sbiret r = { .error = RVEMU_NATIVE_INTERRUPTED };
    if (mutex_lock_killable(&native_lock)) return r;
    r = sbi_ecall(EXT, op | MAY_PEND, out, cap, in, len, params, plen);
    while (r.error == PENDING) {
        if (fatal_signal_pending(current)) {
            sbi_ecall(EXT, ABORT, 0, 0, 0, 0, 0, 0);
            r.error = RVEMU_NATIVE_INTERRUPTED;
            break;
        }
        cond_resched();
        r = sbi_ecall(EXT, CONTINUE, 0, 0, 0, 0, 0, 0);
    }
    mutex_unlock(&native_lock);
    return r;
}
#define MAX_BUFFER RVEMU_CRYPTO_MAX_BUFFER
#define MAX_PARAMS RVEMU_CRYPTO_MAX_PARAMS
struct crypto_context { struct mutex lock; unsigned char *in, *out, *params; };
static void destroy(struct crypto_context *c)
{
    if (!c) return;
    kfree_sensitive(c->in); kfree_sensitive(c->out); kfree_sensitive(c->params); kfree_sensitive(c);
}
static int crypto_open(struct inode *inode, struct file *file)
{
    struct crypto_context *c = kzalloc(sizeof(*c), GFP_KERNEL);
    if (!c) return -ENOMEM;
    c->in=kmalloc(MAX_BUFFER,GFP_KERNEL); c->out=kmalloc(MAX_BUFFER,GFP_KERNEL);
    c->params=kmalloc(MAX_PARAMS,GFP_KERNEL);
    if (!c->in || !c->out || !c->params) { destroy(c); return -ENOMEM; }
    mutex_init(&c->lock); file->private_data=c;
    return nonseekable_open(inode,file);
}
static int crypto_release(struct inode *inode, struct file *file) { destroy(file->private_data); return 0; }
static long crypto_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct crypto_context *c=file->private_data;
    struct rvemu_crypto_request r;
    struct sbiret result;
    long ret=0;
    if (cmd!=RVEMU_CRYPTO_RUN) return -ENOTTY;
    if (copy_from_user(&r,(void __user *)arg,sizeof(r))) return -EFAULT;
    if (r.flags || r.output_len) return -EINVAL;
    if (r.source_len>MAX_BUFFER || r.capacity>MAX_BUFFER || r.parameters_len>MAX_PARAMS) return -EMSGSIZE;
    mutex_lock(&c->lock);
    if (copy_from_user(c->in,(void __user *)(unsigned long)r.source,r.source_len) ||
        copy_from_user(c->params,(void __user *)(unsigned long)r.parameters,r.parameters_len)) {
        ret=-EFAULT; goto done;
    }
    if (r.operation>=MAY_PEND) { ret=-EOPNOTSUPP; goto done; }
    result=rvemu_native_call(r.operation,virt_to_phys(c->out),r.capacity,
        virt_to_phys(c->in),r.source_len,virt_to_phys(c->params),r.parameters_len);
    if (result.error) {
        ret=result.error==-2 ? -EOPNOTSUPP : result.error==-1 ? -EBADMSG :
            result.error==RVEMU_NATIVE_INTERRUPTED ? -EINTR : -EINVAL;
        goto done;
    }
    if (result.value>r.capacity) { ret=-EIO; goto done; }
    r.output_len=result.value;
    if (copy_to_user((void __user *)(unsigned long)r.destination,c->out,r.output_len) ||
        copy_to_user((void __user *)arg,&r,sizeof(r))) ret=-EFAULT;
done:
    memzero_explicit(c->in,r.source_len); memzero_explicit(c->out,r.capacity);
    memzero_explicit(c->params,r.parameters_len);
    mutex_unlock(&c->lock);
    return ret;
}
static const struct file_operations crypto_ops={
    .owner=THIS_MODULE, .open=crypto_open, .release=crypto_release, .unlocked_ioctl=crypto_ioctl,
};
static struct miscdevice crypto_dev={.minor=MISC_DYNAMIC_MINOR,.name="rvemu-crypto",.fops=&crypto_ops,.mode=0600};
static int __init crypto_init(void)
{
    if (sbi_probe_extension(EXT)<=0) return -ENODEV;
    return misc_register(&crypto_dev);
}
device_initcall(crypto_init);
MODULE_LICENSE("GPL");
