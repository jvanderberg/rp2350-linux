// SPDX-License-Identifier: GPL-2.0
/*
 * GPIO controller of the rvemu machine (rvemu-rs/rvcore/src/periph.rs), which
 * the emulator maps onto the RP2350's bank 0 pads.
 *
 * One 32-bit control register per line (output value, output enable, pulls,
 * edge interrupt enables; reading it also returns the pad level), word-wide
 * input / set / clear registers, and a pending-edge register pair behind one
 * PLIC input. Lines the board uses for something else are left out of
 * VALID, so gpiolib never hands them out.
 */
#include <linux/bitops.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define RG_LINE(n)	((n) * 4)
#define  LN_OUT		BIT(0)
#define  LN_OE		BIT(1)
#define  LN_PULL_UP	BIT(2)
#define  LN_PULL_DOWN	BIT(3)
#define  LN_IRQ_RISE	BIT(4)
#define  LN_IRQ_FALL	BIT(5)
#define  LN_IN		BIT(8)
#define RG_NGPIO	0x100
#define RG_VALID(w)	(0x104 + 4 * (w))
#define RG_PEND(w)	(0x110 + 4 * (w))
#define RG_INPUT(w)	(0x120 + 4 * (w))
#define RG_SET(w)	(0x130 + 4 * (w))
#define RG_CLR(w)	(0x138 + 4 * (w))
#define MAX_LINES	64

struct rvemu_gpio {
	struct gpio_chip gc;
	void __iomem *base;
	raw_spinlock_t lock;
	/* LN_IRQ_RISE / LN_IRQ_FALL wanted by each line's irq type */
	u8 edges[MAX_LINES];
};

static void rg_update(struct rvemu_gpio *g, unsigned int n, u32 clr, u32 set)
{
	unsigned long flags;
	u32 v;

	raw_spin_lock_irqsave(&g->lock, flags);
	v = readl(g->base + RG_LINE(n));
	writel((v & ~clr & 0xff) | set, g->base + RG_LINE(n));
	raw_spin_unlock_irqrestore(&g->lock, flags);
}

static int rg_get_direction(struct gpio_chip *gc, unsigned int n)
{
	struct rvemu_gpio *g = gpiochip_get_data(gc);

	return readl(g->base + RG_LINE(n)) & LN_OE ? GPIO_LINE_DIRECTION_OUT :
						     GPIO_LINE_DIRECTION_IN;
}

static int rg_direction_input(struct gpio_chip *gc, unsigned int n)
{
	rg_update(gpiochip_get_data(gc), n, LN_OE, 0);
	return 0;
}

static int rg_direction_output(struct gpio_chip *gc, unsigned int n, int val)
{
	rg_update(gpiochip_get_data(gc), n, LN_OUT, LN_OE | (val ? LN_OUT : 0));
	return 0;
}

static int rg_get(struct gpio_chip *gc, unsigned int n)
{
	struct rvemu_gpio *g = gpiochip_get_data(gc);

	return !!(readl(g->base + RG_LINE(n)) & LN_IN);
}

/* SET / CLR under the lock too: rg_update rewrites the whole register */
static void rg_set(struct gpio_chip *gc, unsigned int n, int val)
{
	struct rvemu_gpio *g = gpiochip_get_data(gc);
	unsigned long flags;

	raw_spin_lock_irqsave(&g->lock, flags);
	writel(BIT(n % 32), g->base + (val ? RG_SET(n / 32) : RG_CLR(n / 32)));
	raw_spin_unlock_irqrestore(&g->lock, flags);
}

static int rg_get_multiple(struct gpio_chip *gc, unsigned long *mask,
			   unsigned long *bits)
{
	struct rvemu_gpio *g = gpiochip_get_data(gc);
	unsigned int w;

	for (w = 0; w < BITS_TO_LONGS(gc->ngpio); w++)
		if (mask[w])
			bits[w] = (bits[w] & ~mask[w]) |
				  (readl(g->base + RG_INPUT(w)) & mask[w]);
	return 0;
}

static void rg_set_multiple(struct gpio_chip *gc, unsigned long *mask,
			    unsigned long *bits)
{
	struct rvemu_gpio *g = gpiochip_get_data(gc);
	unsigned long flags;
	unsigned int w;

	raw_spin_lock_irqsave(&g->lock, flags);
	for (w = 0; w < BITS_TO_LONGS(gc->ngpio); w++) {
		if (mask[w] & bits[w])
			writel(mask[w] & bits[w], g->base + RG_SET(w));
		if (mask[w] & ~bits[w])
			writel(mask[w] & ~bits[w], g->base + RG_CLR(w));
	}
	raw_spin_unlock_irqrestore(&g->lock, flags);
}

static int rg_set_config(struct gpio_chip *gc, unsigned int n,
			 unsigned long config)
{
	struct rvemu_gpio *g = gpiochip_get_data(gc);
	u32 pull;

	switch (pinconf_to_config_param(config)) {
	case PIN_CONFIG_BIAS_DISABLE:
		pull = 0;
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		pull = LN_PULL_UP;
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		pull = LN_PULL_DOWN;
		break;
	default:
		return -ENOTSUPP;
	}
	rg_update(g, n, LN_PULL_UP | LN_PULL_DOWN, pull);
	return 0;
}

static int rg_init_valid_mask(struct gpio_chip *gc, unsigned long *valid,
			      unsigned int ngpios)
{
	struct rvemu_gpio *g = gpiochip_get_data(gc);
	unsigned int w;

	for (w = 0; w < BITS_TO_LONGS(ngpios); w++)
		valid[w] = readl(g->base + RG_VALID(w));
	return 0;
}

static void rg_irq_ack(struct irq_data *d)
{
	struct rvemu_gpio *g = gpiochip_get_data(irq_data_get_irq_chip_data(d));
	irq_hw_number_t n = irqd_to_hwirq(d);

	writel(BIT(n % 32), g->base + RG_PEND(n / 32));
}

static void rg_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	irq_hw_number_t n = irqd_to_hwirq(d);

	rg_update(gpiochip_get_data(gc), n, LN_IRQ_RISE | LN_IRQ_FALL, 0);
	gpiochip_disable_irq(gc, n);
}

static void rg_irq_unmask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct rvemu_gpio *g = gpiochip_get_data(gc);
	irq_hw_number_t n = irqd_to_hwirq(d);

	gpiochip_enable_irq(gc, n);
	rg_update(g, n, LN_IRQ_RISE | LN_IRQ_FALL, g->edges[n]);
}

static int rg_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct rvemu_gpio *g = gpiochip_get_data(irq_data_get_irq_chip_data(d));
	irq_hw_number_t n = irqd_to_hwirq(d);
	u8 e = 0;

	if (type & ~IRQ_TYPE_EDGE_BOTH)
		return -EINVAL;	/* edges only */
	if (type & IRQ_TYPE_EDGE_RISING)
		e |= LN_IRQ_RISE;
	if (type & IRQ_TYPE_EDGE_FALLING)
		e |= LN_IRQ_FALL;
	g->edges[n] = e;
	irq_set_handler_locked(d, handle_edge_irq);
	return 0;
}

static const struct irq_chip rg_irq_chip = {
	.name = "rvemu-gpio",
	.irq_ack = rg_irq_ack,
	.irq_mask = rg_irq_mask,
	.irq_unmask = rg_irq_unmask,
	.irq_set_type = rg_irq_set_type,
	.flags = IRQCHIP_IMMUTABLE | IRQCHIP_SKIP_SET_WAKE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static void rg_irq_handler(struct irq_desc *desc)
{
	struct gpio_chip *gc = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct rvemu_gpio *g = gpiochip_get_data(gc);
	unsigned long pend;
	unsigned int w, bit;

	chained_irq_enter(chip, desc);
	for (w = 0; w < BITS_TO_LONGS(gc->ngpio); w++) {
		pend = readl(g->base + RG_PEND(w));
		for_each_set_bit(bit, &pend, 32)
			generic_handle_domain_irq(gc->irq.domain, w * 32 + bit);
	}
	chained_irq_exit(chip, desc);
}

static int rvemu_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gpio_irq_chip *girq;
	struct rvemu_gpio *g;
	int irq;

	g = devm_kzalloc(dev, sizeof(*g), GFP_KERNEL);
	if (!g)
		return -ENOMEM;
	g->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(g->base))
		return PTR_ERR(g->base);
	raw_spin_lock_init(&g->lock);

	g->gc.label = "rp2350-gpio";
	g->gc.parent = dev;
	g->gc.owner = THIS_MODULE;
	g->gc.base = -1;
	g->gc.ngpio = min_t(u32, readl(g->base + RG_NGPIO), MAX_LINES);
	g->gc.get_direction = rg_get_direction;
	g->gc.direction_input = rg_direction_input;
	g->gc.direction_output = rg_direction_output;
	g->gc.get = rg_get;
	g->gc.set = rg_set;
	g->gc.get_multiple = rg_get_multiple;
	g->gc.set_multiple = rg_set_multiple;
	g->gc.set_config = rg_set_config;
	g->gc.init_valid_mask = rg_init_valid_mask;

	irq = platform_get_irq_optional(pdev, 0);
	if (irq > 0) {
		girq = &g->gc.irq;
		gpio_irq_chip_set_chip(girq, &rg_irq_chip);
		girq->parent_handler = rg_irq_handler;
		girq->num_parents = 1;
		girq->parents = devm_kcalloc(dev, 1, sizeof(*girq->parents),
					     GFP_KERNEL);
		if (!girq->parents)
			return -ENOMEM;
		girq->parents[0] = irq;
		girq->default_type = IRQ_TYPE_NONE;
		girq->handler = handle_bad_irq;
	}
	return devm_gpiochip_add_data(dev, &g->gc, g);
}

static const struct of_device_id rvemu_gpio_of_match[] = {
	{ .compatible = "rvemu,gpio" },
	{ }
};

static struct platform_driver rvemu_gpio_driver = {
	.probe = rvemu_gpio_probe,
	.driver = {
		.name = "rvemu-gpio",
		.of_match_table = rvemu_gpio_of_match,
	},
};
builtin_platform_driver(rvemu_gpio_driver);
