// SPDX-License-Identifier: GPL-2.0
/*
 * rvemu NOR flash ("rvemu,flash"): the board's /home flash region as an MTD
 * device, for jffs2. The device (rvcore/src/nor.rs) copies between flash
 * and guest memory itself and completes each command before the register
 * store returns, so there are no interrupts and no waiting here.
 */
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/vmalloc.h>

#define RVFL_MAGIC	0x00
#define RVFL_SIZE	0x04
#define RVFL_ERASESIZE	0x08
#define RVFL_WRITESIZE	0x0c
#define RVFL_OFFSET	0x10
#define RVFL_ADDR	0x14
#define RVFL_LEN	0x18
#define RVFL_CMD	0x1c
#define RVFL_STATUS	0x20
#define RVFL_STATS	0x40
#define RVFL_NSTATS	5

#define RVFL_MAGIC_VAL	0x4c465652	/* "RVFL" */
#define RVFL_CMD_READ	1
#define RVFL_CMD_PROGRAM 2
#define RVFL_CMD_ERASE	3

struct rvfl {
	void __iomem *regs;
	struct mtd_info mtd;
	struct mutex lock;
};

/* One command; buf is a kernel buffer (or NULL for an erase). */
static int rvfl_cmd(struct rvfl *f, u32 cmd, loff_t off, const void *buf, size_t len)
{
	int ret = 0;

	mutex_lock(&f->lock);
	while (len) {
		size_t n = len;
		phys_addr_t pa = 0;

		if (buf && is_vmalloc_addr(buf)) {
			/* only physically contiguous within a page */
			n = min_t(size_t, len, PAGE_SIZE - offset_in_page(buf));
			pa = page_to_phys(vmalloc_to_page(buf)) + offset_in_page(buf);
		} else if (buf) {
			pa = virt_to_phys((void *)buf);
		}
		writel(off, f->regs + RVFL_OFFSET);
		writel(pa, f->regs + RVFL_ADDR);
		writel(n, f->regs + RVFL_LEN);
		writel(cmd, f->regs + RVFL_CMD);
		if (readl(f->regs + RVFL_STATUS)) {
			ret = -EIO;
			break;
		}
		off += n;
		len -= n;
		if (buf)
			buf += n;
	}
	mutex_unlock(&f->lock);
	return ret;
}

static int rvfl_read(struct mtd_info *mtd, loff_t from, size_t len, size_t *retlen, u_char *buf)
{
	int ret = rvfl_cmd(mtd->priv, RVFL_CMD_READ, from, buf, len);

	*retlen = ret ? 0 : len;
	return ret;
}

static int rvfl_write(struct mtd_info *mtd, loff_t to, size_t len, size_t *retlen, const u_char *buf)
{
	int ret = rvfl_cmd(mtd->priv, RVFL_CMD_PROGRAM, to, buf, len);

	*retlen = ret ? 0 : len;
	return ret;
}

static int rvfl_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	int ret = rvfl_cmd(mtd->priv, RVFL_CMD_ERASE, instr->addr, NULL, instr->len);

	if (ret)
		instr->fail_addr = instr->addr;
	return ret;
}

/*
 * The device's counters: erases, erases skipped (sector already blank),
 * pages programmed, longest stall (us), total stall (ms).
 */
static ssize_t stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rvfl *f = dev_get_drvdata(dev);
	int i, n = 0;

	for (i = 0; i < RVFL_NSTATS; i++)
		n += sysfs_emit_at(buf, n, "%u%c", readl(f->regs + RVFL_STATS + 4 * i),
				   i == RVFL_NSTATS - 1 ? '\n' : ' ');
	return n;
}
static DEVICE_ATTR_RO(stats);

/*
 * Writing 1 erases the whole device. jffs2 refuses to mount flash that holds
 * no valid node at all (and won't erase it), so overlay-init formats with
 * this when the mount fails.
 */
static ssize_t format_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct rvfl *f = dev_get_drvdata(dev);
	int ret;

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	ret = rvfl_cmd(f, RVFL_CMD_ERASE, 0, NULL, f->mtd.size);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(format);

static struct attribute *rvfl_attrs[] = {
	&dev_attr_stats.attr,
	&dev_attr_format.attr,
	NULL
};
ATTRIBUTE_GROUPS(rvfl);

static int rvfl_probe(struct platform_device *pdev)
{
	struct rvfl *f;
	u32 ws;

	f = devm_kzalloc(&pdev->dev, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	f->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(f->regs))
		return PTR_ERR(f->regs);
	if (readl(f->regs + RVFL_MAGIC) != RVFL_MAGIC_VAL)
		return -ENODEV;
	mutex_init(&f->lock);

	ws = readl(f->regs + RVFL_WRITESIZE);
	f->mtd.name = "rvemu-flash";
	f->mtd.type = MTD_NORFLASH;
	/* writesize 1: bits can be cleared anywhere (plain NOR) */
	f->mtd.flags = ws == 1 ? MTD_CAP_NORFLASH : MTD_WRITEABLE;
	f->mtd.size = readl(f->regs + RVFL_SIZE);
	f->mtd.erasesize = readl(f->regs + RVFL_ERASESIZE);
	f->mtd.writesize = ws;
	f->mtd.writebufsize = ws;
	f->mtd._read = rvfl_read;
	f->mtd._write = rvfl_write;
	f->mtd._erase = rvfl_erase;
	f->mtd.owner = THIS_MODULE;
	f->mtd.priv = f;
	f->mtd.dev.parent = &pdev->dev;
	mtd_set_of_node(&f->mtd, pdev->dev.of_node);
	platform_set_drvdata(pdev, f);
	return mtd_device_register(&f->mtd, NULL, 0);
}

static const struct of_device_id rvfl_of_match[] = {
	{ .compatible = "rvemu,flash" },
	{ }
};
MODULE_DEVICE_TABLE(of, rvfl_of_match);

static struct platform_driver rvfl_driver = {
	.probe = rvfl_probe,
	.driver = {
		.name = "rvemu-flash",
		.of_match_table = rvfl_of_match,
		.dev_groups = rvfl_groups,
	},
};
module_platform_driver(rvfl_driver);

MODULE_DESCRIPTION("rvemu NOR flash");
MODULE_LICENSE("GPL");
