// SPDX-License-Identifier: GPL-2.0
/*
 * /dev/pio1 and /dev/pio2: the rvemu machine's PIO blocks, the RP2350's
 * PIO1 and PIO2 themselves (rvemu-rs/rvcore/src/periph.rs). A program maps
 * a block's registers and runs it as on the chip; this driver hands out
 * pins (through gpiolib, so a pin is either a GPIO line or a PIO pin),
 * waits for the block's interrupt, and resets the block between owners.
 * The interface is in rvemu-pio.h.
 *
 * The firmware lets a block drive only the free pins given to it here:
 * PIOCTL, the pin function registers both blocks share, is not mapped
 * into programs.
 *
 * DMA (`rvemu,pio-dma`): a few of the chip's channels, each between a
 * buffer this driver allocated for the program and one state machine's
 * FIFO. Programs name the state machine and a part of their buffer; the
 * physical address comes from here, and the firmware checks it again.
 */
#include <linux/bitops.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include "rvemu-pio.h"

/* the block's registers */
#define PIO_CTRL		0x000
#define PIO_IRQ			0x030
#define PIO_INPUT_SYNC_BYPASS	0x038
#define PIO_INSTR_MEM(n)	(0x048 + 4 * (n))
#define PIO_SM_CLKDIV(n)	(0x0c8 + 0x18 * (n))
#define PIO_SM_EXECCTRL(n)	(0x0cc + 0x18 * (n))
#define PIO_SM_SHIFTCTRL(n)	(0x0d0 + 0x18 * (n))
#define PIO_SM_INSTR(n)		(0x0d8 + 0x18 * (n))
#define PIO_SM_PINCTRL(n)	(0x0dc + 0x18 * (n))
#define PIO_GPIOBASE		0x168
#define PIO_MOV_PINS_NULL	0xa003	/* mov pins, null */
#define PIO_MOV_PINDIRS_NULL	0xa063	/* mov pindirs, null (RP2350) */
#define PIO_IRQ0_INTE		0x170
#define PIO_IRQ0_INTF		0x174
#define PIO_IRQ0_INTS		0x178
#define PIO_IRQ1_INTE		0x17c
#define PIO_IRQ1_INTF		0x180
#define SHIFTCTRL_FJOIN_RX	BIT(31)

/* PIOCTL: the pin functions */
#define CTL_SEL			0x00
#define CTL_FUNC		0x04
#define CTL_VALID(w)		(0x08 + 4 * (w))
#define CTL_CLK			0x10

#define NPINS			48

/* the DMA device */
#define DMA_CH(n)		(0x40 * (n))
#define DMA_BUF			0x00
#define DMA_COUNT		0x04
#define DMA_CTRL		0x08
#define  DMA_CTRL_FROM_FIFO	BIT(2)
#define  DMA_CTRL_ENDLESS	BIT(3)
#define DMA_GO			0x0c
#define DMA_STATUS		0x10
#define  DMA_ST_BUSY		BIT(0)
#define  DMA_ST_ERROR		BIT(1)
#define  DMA_ST_REJECTED	BIT(2)
#define DMA_REMAIN		0x14
#define DMA_POS			0x18
#define DMA_IRQ			0x100
#define DMA_IRQ_EN		0x104
#define DMA_NCHAN		0x108
#define DMA_MAX_CH		4

struct rvemu_pio;

static struct rp_dma {
	void __iomem *regs;
	unsigned int nchan;
	spinlock_t lock;		/* owner, IRQ_EN */
	struct rvemu_pio *owner[DMA_MAX_CH];
	wait_queue_head_t wq;
} *rp_dma;

struct rvemu_pio {
	struct miscdevice misc;
	char name[8];
	struct device *dev;
	void __iomem *regs;
	void __iomem *ctl;
	phys_addr_t phys;
	unsigned int block;
	int irq;
	struct mutex lock;	/* open, pins */
	bool open;
	struct gpio_desc *pins[NPINS];
	wait_queue_head_t wq;
	spinlock_t irq_lock;
	bool irq_off;
	/* the program's DMA buffer */
	struct page *dma_page;
	size_t dma_size;
};

/* SEL then FUNC: one pin at a time for both blocks */
static DEFINE_MUTEX(rp_ctl_lock);

static u64 rp_valid(struct rvemu_pio *p)
{
	return readl(p->ctl + CTL_VALID(0)) |
	       (u64)readl(p->ctl + CTL_VALID(1)) << 32;
}

static u32 rp_func(struct rvemu_pio *p, unsigned int n, u32 func)
{
	u32 got;

	mutex_lock(&rp_ctl_lock);
	writel(n, p->ctl + CTL_SEL);
	writel(func, p->ctl + CTL_FUNC);
	got = readl(p->ctl + CTL_FUNC);
	mutex_unlock(&rp_ctl_lock);
	return got;
}

static int rp_claim(struct rvemu_pio *p, u32 n)
{
	struct gpio_desc *d;

	if (n >= NPINS || !(rp_valid(p) >> n & 1))
		return -EINVAL;
	if (p->pins[n])
		return 0;
	d = gpiod_get_index(p->dev, "pio", n, GPIOD_ASIS);
	if (IS_ERR(d))
		return PTR_ERR(d) == -EPROBE_DEFER ? -EAGAIN : -EBUSY;
	if (rp_func(p, n, p->block) != p->block) {
		gpiod_put(d);
		return -EIO;
	}
	p->pins[n] = d;
	return 0;
}

static void rp_release_pin(struct rvemu_pio *p, unsigned int n)
{
	if (!p->pins[n])
		return;
	rp_func(p, n, 0);
	gpiod_put(p->pins[n]);
	p->pins[n] = NULL;
}

/* Back to the state the chip resets to, with every state machine at 0. */
static void rp_reset(struct rvemu_pio *p)
{
	int k;

	writel(0, p->regs + PIO_IRQ0_INTE);
	writel(0, p->regs + PIO_IRQ1_INTE);
	writel(0, p->regs + PIO_IRQ0_INTF);
	writel(0, p->regs + PIO_IRQ1_INTF);
	/* stop them all, restart their state and clock dividers */
	writel(0xff0, p->regs + PIO_CTRL);
	for (k = 0; k < 4; k++) {
		writel(0x00010000, p->regs + PIO_SM_CLKDIV(k));
		writel(0x0001f000, p->regs + PIO_SM_EXECCTRL(k));
		/* a join change empties both FIFOs */
		writel(0x000c0000 | SHIFTCTRL_FJOIN_RX, p->regs + PIO_SM_SHIFTCTRL(k));
		writel(0x000c0000, p->regs + PIO_SM_SHIFTCTRL(k));
		writel(0x14000000, p->regs + PIO_SM_PINCTRL(k));
		writel(0x0000, p->regs + PIO_SM_INSTR(k)); /* jmp 0 */
	}
	for (k = 0; k < 32; k++)
		writel(0, p->regs + PIO_INSTR_MEM(k));
	writel(0, p->regs + PIO_INPUT_SYNC_BYPASS);
	/*
	 * The block's pin outputs and directions belong to no state machine
	 * and only a block reset clears them: a program that stopped with a
	 * pin high would leave it high for the next one, which drives it the
	 * moment it makes the pin an output. Clear them for all 48 pins (32 at
	 * a time from GPIOBASE 16, then 0) from state machine 0.
	 */
	writel(32 << 20, p->regs + PIO_SM_PINCTRL(0)); /* OUT_BASE 0, OUT_COUNT 32 */
	for (k = 16; k >= 0; k -= 16) {
		writel(k, p->regs + PIO_GPIOBASE);
		writel(PIO_MOV_PINS_NULL, p->regs + PIO_SM_INSTR(0));
		writel(PIO_MOV_PINDIRS_NULL, p->regs + PIO_SM_INSTR(0));
	}
	writel(0x14000000, p->regs + PIO_SM_PINCTRL(0));
	writel(0xff, p->regs + PIO_IRQ);
}

static void rp_arm(struct rvemu_pio *p)
{
	unsigned long flags;

	spin_lock_irqsave(&p->irq_lock, flags);
	if (p->irq_off) {
		p->irq_off = false;
		enable_irq(p->irq);
	}
	spin_unlock_irqrestore(&p->irq_lock, flags);
}

static irqreturn_t rp_irq(int irq, void *data)
{
	struct rvemu_pio *p = data;

	/* level: off until a waiter has looked (the program clears the cause) */
	spin_lock(&p->irq_lock);
	if (!p->irq_off) {
		p->irq_off = true;
		disable_irq_nosync(irq);
	}
	spin_unlock(&p->irq_lock);
	wake_up_interruptible(&p->wq);
	return IRQ_HANDLED;
}

static struct rvemu_pio *rp_of(struct file *f)
{
	return container_of(f->private_data, struct rvemu_pio, misc);
}

/* ------------------------------------------------------------ DMA */

static void rp_dma_stop(unsigned int ch)
{
	unsigned long flags;

	writel(0, rp_dma->regs + DMA_CH(ch) + DMA_GO);
	spin_lock_irqsave(&rp_dma->lock, flags);
	writel(readl(rp_dma->regs + DMA_IRQ_EN) & ~BIT(ch), rp_dma->regs + DMA_IRQ_EN);
	writel(BIT(ch), rp_dma->regs + DMA_IRQ);
	rp_dma->owner[ch] = NULL;
	spin_unlock_irqrestore(&rp_dma->lock, flags);
	/* a DMA_WAIT on it is over */
	wake_up_interruptible(&rp_dma->wq);
}

/* Every channel of p's, stopped and given back. */
static void rp_dma_release(struct rvemu_pio *p)
{
	unsigned int ch;

	if (!rp_dma)
		return;
	for (ch = 0; ch < rp_dma->nchan; ch++)
		if (READ_ONCE(rp_dma->owner[ch]) == p)
			rp_dma_stop(ch);
}

static bool rp_dma_owns(struct rvemu_pio *p, u32 ch)
{
	return rp_dma && ch < rp_dma->nchan && READ_ONCE(rp_dma->owner[ch]) == p;
}

static int rp_dma_alloc(struct rvemu_pio *p, u32 size)
{
	if (!rp_dma)
		return -ENODEV;
	if (!size || size > RVEMU_PIO_DMA_MAX_BUF)
		return -EINVAL;
	if (p->dma_page)
		return -EBUSY;
	size = PAGE_ALIGN(size);
	p->dma_page = alloc_pages(GFP_KERNEL | __GFP_ZERO, get_order(size));
	if (!p->dma_page)
		return -ENOMEM;
	p->dma_size = size;
	return 0;
}

static void rp_dma_status(unsigned int ch, struct rvemu_pio_dma *d)
{
	u32 st = readl(rp_dma->regs + DMA_CH(ch) + DMA_STATUS);

	d->busy = !!(st & DMA_ST_BUSY);
	d->error = !!(st & DMA_ST_ERROR);
	d->remaining = readl(rp_dma->regs + DMA_CH(ch) + DMA_REMAIN);
	d->pos = readl(rp_dma->regs + DMA_CH(ch) + DMA_POS);
}

static int rp_dma_start(struct rvemu_pio *p, struct rvemu_pio_dma *d)
{
	unsigned long flags;
	unsigned int ch, size_log2;
	u64 span;
	u32 ctrl;

	if (!rp_dma)
		return -ENODEV;
	if (!p->dma_page)
		return -ENOMEM;
	if (d->sm > 3 || d->flags & ~(RVEMU_PIO_DMA_FROM_SM | RVEMU_PIO_DMA_ENDLESS) || !d->count ||
	    d->count >= BIT(28))
		return -EINVAL;
	switch (d->size) {
	case 1: size_log2 = 0; break;
	case 2: size_log2 = 1; break;
	case 4: size_log2 = 2; break;
	default: return -EINVAL;
	}
	if (d->ring_bits) {
		if (d->ring_bits > 12 || d->ring_bits < size_log2 || d->offset & (BIT(d->ring_bits) - 1))
			return -EINVAL;
		span = BIT(d->ring_bits);
	} else {
		if (d->flags & RVEMU_PIO_DMA_ENDLESS)
			return -EINVAL;
		span = (u64)d->count << size_log2;
	}
	if (d->offset & (d->size - 1) || d->offset + span > p->dma_size)
		return -EINVAL;
	/* a channel: the one named, or a free one */
	spin_lock_irqsave(&rp_dma->lock, flags);
	if (d->channel == RVEMU_PIO_DMA_NEW) {
		for (ch = 0; ch < rp_dma->nchan && rp_dma->owner[ch]; ch++)
			;
		if (ch == rp_dma->nchan) {
			spin_unlock_irqrestore(&rp_dma->lock, flags);
			return -EBUSY;
		}
		rp_dma->owner[ch] = p;
	} else if (d->channel < rp_dma->nchan && rp_dma->owner[d->channel] == p) {
		ch = d->channel;
	} else {
		spin_unlock_irqrestore(&rp_dma->lock, flags);
		return -EINVAL;
	}
	spin_unlock_irqrestore(&rp_dma->lock, flags);
	ctrl = size_log2 | d->ring_bits << 8 | d->sm << 16 | (p->block - 1) << 20;
	if (d->flags & RVEMU_PIO_DMA_FROM_SM)
		ctrl |= DMA_CTRL_FROM_FIFO;
	if (d->flags & RVEMU_PIO_DMA_ENDLESS)
		ctrl |= DMA_CTRL_ENDLESS;
	writel(page_to_phys(p->dma_page) + d->offset, rp_dma->regs + DMA_CH(ch) + DMA_BUF);
	writel(d->count, rp_dma->regs + DMA_CH(ch) + DMA_COUNT);
	writel(ctrl, rp_dma->regs + DMA_CH(ch) + DMA_CTRL);
	writel(1, rp_dma->regs + DMA_CH(ch) + DMA_GO);
	if (readl(rp_dma->regs + DMA_CH(ch) + DMA_STATUS) & DMA_ST_REJECTED) {
		if (d->channel == RVEMU_PIO_DMA_NEW)
			rp_dma_stop(ch);
		return -EINVAL;
	}
	if (!(d->flags & RVEMU_PIO_DMA_ENDLESS)) {
		spin_lock_irqsave(&rp_dma->lock, flags);
		writel(readl(rp_dma->regs + DMA_IRQ_EN) | BIT(ch), rp_dma->regs + DMA_IRQ_EN);
		spin_unlock_irqrestore(&rp_dma->lock, flags);
	}
	d->channel = ch;
	return 0;
}

/* done, or no longer p's (stopped, or given back on release) */
static bool rp_dma_over(struct rvemu_pio *p, unsigned int ch)
{
	return !rp_dma_owns(p, ch) || !(readl(rp_dma->regs + DMA_CH(ch) + DMA_STATUS) & DMA_ST_BUSY);
}

static long rp_dma_ioctl(struct rvemu_pio *p, unsigned int cmd, unsigned long arg)
{
	struct rvemu_pio_dma d;
	long ret = 0, t;

	if (copy_from_user(&d, (void __user *)arg, sizeof(d)))
		return -EFAULT;
	if (cmd != RVEMU_PIO_DMA_START && !rp_dma_owns(p, d.channel))
		return -EINVAL;
	switch (cmd) {
	case RVEMU_PIO_DMA_START:
		mutex_lock(&p->lock);
		ret = rp_dma_start(p, &d);
		mutex_unlock(&p->lock);
		break;
	case RVEMU_PIO_DMA_STATUS:
		rp_dma_status(d.channel, &d);
		break;
	case RVEMU_PIO_DMA_WAIT:
		t = d.timeout_ms ? msecs_to_jiffies(d.timeout_ms) : MAX_SCHEDULE_TIMEOUT;
		t = wait_event_interruptible_timeout(rp_dma->wq, rp_dma_over(p, d.channel), t);
		if (t < 0)
			return t;
		rp_dma_status(d.channel, &d);
		if (!t && d.busy)
			ret = -ETIMEDOUT;
		break;
	case RVEMU_PIO_DMA_STOP:
		/*
		 * Under p->lock, as START is: two threads (or a dup of the fd)
		 * mustn't interleave a stop with a restart of the channel, which
		 * could leave it running unowned, so that release wouldn't stop
		 * it before freeing the buffer it writes.
		 */
		mutex_lock(&p->lock);
		if (rp_dma_owns(p, d.channel))
			rp_dma_stop(d.channel);
		mutex_unlock(&p->lock);
		return 0;
	}
	if (ret && ret != -ETIMEDOUT)
		return ret;
	if (copy_to_user((void __user *)arg, &d, sizeof(d)))
		return -EFAULT;
	return ret;
}

static irqreturn_t rp_dma_irq(int irq, void *data)
{
	u32 pend = readl(rp_dma->regs + DMA_IRQ);

	if (!pend)
		return IRQ_NONE;
	writel(pend, rp_dma->regs + DMA_IRQ);
	wake_up_interruptible(&rp_dma->wq);
	return IRQ_HANDLED;
}

static int rvemu_pio_dma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rp_dma *d;
	int irq, ret;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(d->regs))
		return PTR_ERR(d->regs);
	d->nchan = min_t(u32, readl(d->regs + DMA_NCHAN), DMA_MAX_CH);
	if (!d->nchan)
		return -ENODEV;
	spin_lock_init(&d->lock);
	init_waitqueue_head(&d->wq);
	writel(0, d->regs + DMA_IRQ_EN);
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, rp_dma_irq, 0, dev_name(dev), d);
	if (ret)
		return ret;
	rp_dma = d;
	dev_info(dev, "%u channels\n", d->nchan);
	return 0;
}

static int rp_open(struct inode *inode, struct file *f)
{
	struct rvemu_pio *p = rp_of(f);
	int ret = 0;

	mutex_lock(&p->lock);
	if (p->open) {
		ret = -EBUSY;
	} else {
		p->open = true;
		rp_reset(p);
	}
	mutex_unlock(&p->lock);
	return ret ? ret : nonseekable_open(inode, f);
}

static int rp_release(struct inode *inode, struct file *f)
{
	struct rvemu_pio *p = rp_of(f);
	unsigned int n;

	mutex_lock(&p->lock);
	/* the channels first: they feed the block */
	rp_dma_release(p);
	rp_reset(p);
	for (n = 0; n < NPINS; n++)
		rp_release_pin(p, n);
	if (p->dma_page) {
		__free_pages(p->dma_page, get_order(p->dma_size));
		p->dma_page = NULL;
		p->dma_size = 0;
	}
	p->open = false;
	mutex_unlock(&p->lock);
	return 0;
}

static long rp_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct rvemu_pio *p = rp_of(f);
	struct rvemu_pio_info info = {};
	u32 n;
	long ret;
	unsigned int k;
	u64 valid;

	switch (cmd) {
	case RVEMU_PIO_INFO:
		valid = rp_valid(p);
		info.block = p->block;
		info.clk_hz = readl(p->ctl + CTL_CLK);
		info.valid[0] = valid;
		info.valid[1] = valid >> 32;
		mutex_lock(&p->lock);
		for (k = 0; k < NPINS; k++)
			if (p->pins[k])
				info.claimed[k / 32] |= BIT(k % 32);
		mutex_unlock(&p->lock);
		return copy_to_user((void __user *)arg, &info, sizeof(info)) ? -EFAULT : 0;
	case RVEMU_PIO_PIN_CLAIM:
	case RVEMU_PIO_PIN_RELEASE:
		if (get_user(n, (u32 __user *)arg))
			return -EFAULT;
		if (n >= NPINS)
			return -EINVAL;
		mutex_lock(&p->lock);
		ret = 0;
		if (cmd == RVEMU_PIO_PIN_CLAIM)
			ret = rp_claim(p, n);
		else
			rp_release_pin(p, n);
		mutex_unlock(&p->lock);
		return ret;
	case RVEMU_PIO_DMA_ALLOC:
		if (get_user(n, (u32 __user *)arg))
			return -EFAULT;
		mutex_lock(&p->lock);
		ret = rp_dma_alloc(p, n);
		mutex_unlock(&p->lock);
		return ret;
	case RVEMU_PIO_DMA_START:
	case RVEMU_PIO_DMA_STATUS:
	case RVEMU_PIO_DMA_WAIT:
	case RVEMU_PIO_DMA_STOP:
		return rp_dma_ioctl(p, cmd, arg);
	}
	return -ENOTTY;
}

static ssize_t rp_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
	struct rvemu_pio *p = rp_of(f);
	u32 ints;
	int ret;

	if (len < sizeof(ints))
		return -EINVAL;
	for (;;) {
		ints = readl(p->regs + PIO_IRQ0_INTS);
		if (ints)
			break;
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		rp_arm(p);
		ret = wait_event_interruptible(p->wq, readl(p->regs + PIO_IRQ0_INTS));
		if (ret)
			return ret;
	}
	return copy_to_user(buf, &ints, sizeof(ints)) ? -EFAULT : sizeof(ints);
}

static __poll_t rp_poll(struct file *f, poll_table *wait)
{
	struct rvemu_pio *p = rp_of(f);

	poll_wait(f, &p->wq, wait);
	if (readl(p->regs + PIO_IRQ0_INTS))
		return EPOLLIN | EPOLLRDNORM;
	rp_arm(p);
	return 0;
}

static int rp_mmap(struct file *f, struct vm_area_struct *vma)
{
	struct rvemu_pio *p = rp_of(f);
	unsigned long size = vma->vm_end - vma->vm_start;

	if (vma->vm_pgoff == RVEMU_PIO_DMA_BUF_OFFSET >> PAGE_SHIFT) {
		/* the DMA buffer */
		if (!p->dma_page || size > p->dma_size || !(vma->vm_flags & VM_SHARED))
			return -EINVAL;
		vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
		return remap_pfn_range(vma, vma->vm_start, page_to_pfn(p->dma_page), size,
				       vma->vm_page_prot);
	}
	if (vma->vm_pgoff || size > RVEMU_PIO_MAP_SIZE)
		return -EINVAL;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	return io_remap_pfn_range(vma, vma->vm_start, p->phys >> PAGE_SHIFT,
				  size, vma->vm_page_prot);
}

static const struct file_operations rp_fops = {
	.owner = THIS_MODULE,
	.open = rp_open,
	.release = rp_release,
	.unlocked_ioctl = rp_ioctl,
	.read = rp_read,
	.poll = rp_poll,
	.mmap = rp_mmap,
};

static int rvemu_pio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rvemu_pio *p;
	struct resource *r;
	u32 block;
	int ret;

	if (of_property_read_u32(dev->of_node, "rvemu,block", &block) || block < 1 || block > 2)
		return -EINVAL;
	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	p->block = block;
	p->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &r);
	if (IS_ERR(p->regs))
		return PTR_ERR(p->regs);
	p->phys = r->start;
	/* shared by both blocks: mapped, not requested */
	r = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!r)
		return -EINVAL;
	p->ctl = devm_ioremap(dev, r->start, resource_size(r));
	if (!p->ctl)
		return -ENOMEM;
	/* a firmware without PIO for the guest */
	if (!readl(p->ctl + CTL_CLK))
		return -ENODEV;
	mutex_init(&p->lock);
	spin_lock_init(&p->irq_lock);
	init_waitqueue_head(&p->wq);
	rp_reset(p);
	p->irq = platform_get_irq(pdev, 0);
	if (p->irq < 0)
		return p->irq;
	ret = devm_request_irq(dev, p->irq, rp_irq, 0, dev_name(dev), p);
	if (ret)
		return ret;
	snprintf(p->name, sizeof(p->name), "pio%u", block);
	p->misc.minor = MISC_DYNAMIC_MINOR;
	p->misc.name = p->name;
	p->misc.fops = &rp_fops;
	p->misc.mode = 0600;
	p->misc.parent = dev;
	ret = misc_register(&p->misc);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, p);
	dev_info(dev, "PIO%u, %u MHz\n", block, readl(p->ctl + CTL_CLK) / 1000000);
	return 0;
}

static const struct of_device_id rvemu_pio_of[] = {
	{ .compatible = "rvemu,pio" },
	{}
};

static struct platform_driver rvemu_pio_driver = {
	.probe = rvemu_pio_probe,
	.driver = {
		.name = "rvemu-pio",
		.of_match_table = rvemu_pio_of,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(rvemu_pio_driver);

static const struct of_device_id rvemu_pio_dma_of[] = {
	{ .compatible = "rvemu,pio-dma" },
	{}
};

static struct platform_driver rvemu_pio_dma_driver = {
	.probe = rvemu_pio_dma_probe,
	.driver = {
		.name = "rvemu-pio-dma",
		.of_match_table = rvemu_pio_dma_of,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(rvemu_pio_dma_driver);

MODULE_LICENSE("GPL");
