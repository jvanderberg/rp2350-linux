// SPDX-License-Identifier: GPL-2.0
/*
 * I2C controller of the rvemu machine (rvemu-rs/rvcore/src/periph.rs), on one
 * of the RP2350's I2C blocks. The emulator runs a whole transfer while the
 * driver writes NMSGS: it reads the message table at MSGS (guest physical),
 * moves the data to and from the buffers it names, and leaves the number of
 * messages done in RESULT (bit 31: the address was not acknowledged; bit 30:
 * the bus stayed busy, and the emulator gave up on it). No interrupts, no
 * queues.
 */
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define RI_MSGS		0x00
#define RI_NMSGS	0x04
#define RI_RESULT	0x08
#define  RESULT_NACK	BIT(31)
#define  RESULT_TIMEOUT	BIT(30)
#define  RESULT_DONE	0xff
#define MAX_MSGS	16

struct ri_msg {
	__le16 addr;
	__le16 flags;		/* bit 0: read */
	__le32 len;
	__le32 buf;		/* guest physical */
	__le32 pad;
};

struct rvemu_i2c {
	struct i2c_adapter adap;
	void __iomem *base;
};

static int ri_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct rvemu_i2c *ri = i2c_get_adapdata(adap);
	struct ri_msg *tab;
	u8 *bufs[MAX_MSGS] = { };
	int i, ret;
	u32 r;

	if (num > MAX_MSGS)
		return -EOPNOTSUPP;
	tab = kcalloc(num, sizeof(*tab), GFP_KERNEL);
	if (!tab)
		return -ENOMEM;
	for (i = 0; i < num; i++) {
		if (msgs[i].len) {
			bufs[i] = i2c_get_dma_safe_msg_buf(&msgs[i], 1);
			if (!bufs[i]) {
				ret = -ENOMEM;
				goto out;
			}
			tab[i].buf = cpu_to_le32(virt_to_phys(bufs[i]));
		}
		tab[i].addr = cpu_to_le16(msgs[i].addr);
		tab[i].flags = cpu_to_le16(msgs[i].flags & I2C_M_RD ? 1 : 0);
		tab[i].len = cpu_to_le32(msgs[i].len);
	}
	writel(virt_to_phys(tab), ri->base + RI_MSGS);
	writel(num, ri->base + RI_NMSGS);
	r = readl(ri->base + RI_RESULT);
	if ((r & RESULT_DONE) == num)
		ret = num;
	else if (r & RESULT_NACK)
		ret = -ENXIO;
	else if (r & RESULT_TIMEOUT)
		ret = -ETIMEDOUT;
	else
		ret = -EIO;
out:
	for (i = 0; i < num; i++)
		i2c_put_dma_safe_msg_buf(bufs[i], &msgs[i], ret == num);
	kfree(tab);
	return ret;
}

static u32 ri_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm ri_algo = {
	.xfer = ri_xfer,
	.functionality = ri_func,
};

static int rvemu_i2c_probe(struct platform_device *pdev)
{
	struct rvemu_i2c *ri;

	ri = devm_kzalloc(&pdev->dev, sizeof(*ri), GFP_KERNEL);
	if (!ri)
		return -ENOMEM;
	ri->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ri->base))
		return PTR_ERR(ri->base);
	ri->adap.owner = THIS_MODULE;
	ri->adap.algo = &ri_algo;
	ri->adap.dev.parent = &pdev->dev;
	ri->adap.dev.of_node = pdev->dev.of_node;
	strscpy(ri->adap.name, dev_name(&pdev->dev), sizeof(ri->adap.name));
	i2c_set_adapdata(&ri->adap, ri);
	return devm_i2c_add_adapter(&pdev->dev, &ri->adap);
}

static const struct of_device_id rvemu_i2c_of_match[] = {
	{ .compatible = "rvemu,i2c" },
	{ }
};

static struct platform_driver rvemu_i2c_driver = {
	.probe = rvemu_i2c_probe,
	.driver = {
		.name = "rvemu-i2c",
		.of_match_table = rvemu_i2c_of_match,
	},
};
builtin_platform_driver(rvemu_i2c_driver);
