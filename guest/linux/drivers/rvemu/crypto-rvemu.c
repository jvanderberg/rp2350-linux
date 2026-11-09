// SPDX-License-Identifier: GPL-2.0
/* /dev/rvemu-crypto: the rvemu machine's native buffer engine, one operation
 * per ioctl (rvemu-crypto.h). Keys and random seeds come from the caller.
 * Operations share the engine's kernel bounce buffers, one at a time; a
 * failed operation copies nothing back, and every buffer used is wiped. */
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/cpumask.h>
#include <asm/sbi.h>
#include <asm/io.h>
#include "rvemu-crypto.h"
#include "rvemu-native.h"
/* Firmware pause protocol: an operation FID with MAY_PEND may return
 * PENDING after a time slice; CONTINUE runs the next slice, ABORT drops it.
 * The firmware runs one operation at a time. */
#define MAY_PEND 0x100
#define CONTINUE 0x200
#define ABORT 0x201
#define PENDING (-100)

/* One operation at a time, so one set of buffers: allocated at boot, while
 * memory is still contiguous, and held from copy-in to copy-out. */
static DEFINE_MUTEX(native_lock);
static struct rvemu_native_buffers native_buffers;

struct rvemu_native_buffers *rvemu_native_get(void)
{
    if (!native_buffers.in) return ERR_PTR(-ENODEV);
    if (mutex_lock_killable(&native_lock)) return ERR_PTR(-EINTR);
    return &native_buffers;
}
void rvemu_native_put(unsigned long in_len, unsigned long out_len, unsigned long plen)
{
    memzero_explicit(native_buffers.in, in_len);
    memzero_explicit(native_buffers.out, out_len);
    memzero_explicit(native_buffers.params, plen);
    mutex_unlock(&native_lock);
}
/* Between slices the guest takes its interrupts and other tasks run; the
 * buffers stay put until the operation is done. The engine is hart 0's (its
 * host core runs it): with several CPUs the caller moves to CPU 0 for the
 * call. */
struct sbiret rvemu_native_call(unsigned long op, unsigned long cap, unsigned long len,
                                unsigned long plen)
{
    struct sbiret r = { .error = RVEMU_NATIVE_INTERRUPTED };
#ifdef CONFIG_SMP
    cpumask_t saved;
    cpumask_copy(&saved, current->cpus_ptr);
    if (set_cpus_allowed_ptr(current, cpumask_of(0))) return r;
#endif
    r = sbi_ecall(RVEMU_NATIVE_EXT, op | MAY_PEND, virt_to_phys(native_buffers.out), cap,
                  virt_to_phys(native_buffers.in), len, virt_to_phys(native_buffers.params), plen);
    while (r.error == PENDING) {
        if (fatal_signal_pending(current)) {
            sbi_ecall(RVEMU_NATIVE_EXT, ABORT, 0, 0, 0, 0, 0, 0);
            r.error = RVEMU_NATIVE_INTERRUPTED;
            break;
        }
        cond_resched();
        r = sbi_ecall(RVEMU_NATIVE_EXT, CONTINUE, 0, 0, 0, 0, 0, 0);
    }
#ifdef CONFIG_SMP
    set_cpus_allowed_ptr(current, &saved);
#endif
    return r;
}
#define MAX_BUFFER RVEMU_NATIVE_MAX_BUFFER
#define MAX_PARAMS RVEMU_NATIVE_MAX_PARAMS
static long crypto_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct rvemu_native_buffers *b;
    struct rvemu_crypto_request r;
    struct sbiret result;
    long ret=0;
    if (cmd!=RVEMU_CRYPTO_RUN) return -ENOTTY;
    if (copy_from_user(&r,(void __user *)arg,sizeof(r))) return -EFAULT;
    if (r.flags || r.output_len) return -EINVAL;
    if (r.source_len>MAX_BUFFER || r.capacity>MAX_BUFFER || r.parameters_len>MAX_PARAMS) return -EMSGSIZE;
    if (r.operation>=MAY_PEND) return -EOPNOTSUPP;
    b=rvemu_native_get();
    if (IS_ERR(b)) return PTR_ERR(b);
    if (copy_from_user(b->in,(void __user *)(unsigned long)r.source,r.source_len) ||
        copy_from_user(b->params,(void __user *)(unsigned long)r.parameters,r.parameters_len)) {
        ret=-EFAULT; goto done;
    }
    result=rvemu_native_call(r.operation,r.capacity,r.source_len,r.parameters_len);
    if (result.error) {
        ret=result.error==-2 ? -EOPNOTSUPP : result.error==-1 ? -EBADMSG :
            result.error==RVEMU_NATIVE_INTERRUPTED ? -EINTR : -EINVAL;
        goto done;
    }
    if (result.value>r.capacity) { ret=-EIO; goto done; }
    r.output_len=result.value;
    if (copy_to_user((void __user *)(unsigned long)r.destination,b->out,r.output_len) ||
        copy_to_user((void __user *)arg,&r,sizeof(r))) ret=-EFAULT;
done:
    rvemu_native_put(r.source_len,r.capacity,r.parameters_len);
    return ret;
}
static const struct file_operations crypto_ops={
    .owner=THIS_MODULE, .open=nonseekable_open, .unlocked_ioctl=crypto_ioctl,
};
static struct miscdevice crypto_dev={.minor=MISC_DYNAMIC_MINOR,.name="rvemu-crypto",.fops=&crypto_ops,.mode=0600};
static int __init crypto_init(void)
{
    struct rvemu_native_buffers b;
    if (sbi_probe_extension(RVEMU_NATIVE_EXT)<=0) return -ENODEV;
    b.in=kmalloc(MAX_BUFFER,GFP_KERNEL); b.out=kmalloc(MAX_BUFFER,GFP_KERNEL);
    b.params=kmalloc(MAX_PARAMS,GFP_KERNEL);
    if (!b.in || !b.out || !b.params) {
        kfree(b.in); kfree(b.out); kfree(b.params);
        return -ENOMEM;
    }
    native_buffers=b;
    return misc_register(&crypto_dev);
}
/* before ssh-rvemu.c's device_initcall */
subsys_initcall(crypto_init);
MODULE_LICENSE("GPL");
