// SPDX-License-Identifier: GPL-2.0
/*
 * PWM controller of the rvemu machine (rvemu-rs/rvcore/src/periph.rs). PWM
 * channel N drives GPIO N: the emulator routes the pin to the RP2350 PWM
 * slice channel that can reach it. Two pins on the same slice channel (GP0
 * and GP16, say) share that channel's period and duty cycle, as on the chip;
 * pins on the two channels of one slice share the period.
 *
 * Registers: SEL picks a channel; PERIOD / DUTY (ns) and CTRL (enable,
 * inverted) are that channel's. Writing CTRL applies the three; PERIOD and
 * DUTY then read back what the hardware does (PERIOD 0: not possible).
 */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/spinlock.h>

#define RP_SEL		0x00
#define RP_PERIOD	0x04
#define RP_DUTY		0x08
#define RP_CTRL		0x0c
#define  CTRL_EN	BIT(0)
#define  CTRL_INV	BIT(1)
#define RP_VALID(w)	(0x10 + 4 * (w))
#define RP_NPWM		0x18

struct rvemu_pwm {
	void __iomem *base;
	spinlock_t lock;
};

static int rp_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct rvemu_pwm *p = pwmchip_get_drvdata(chip);
	u32 n = pwm->hwpwm;

	return readl(p->base + RP_VALID(n / 32)) & BIT(n % 32) ? 0 : -EBUSY;
}

static int rp_apply(struct pwm_chip *chip, struct pwm_device *pwm,
		    const struct pwm_state *st)
{
	struct rvemu_pwm *p = pwmchip_get_drvdata(chip);
	u32 ctrl = st->polarity == PWM_POLARITY_INVERSED ? CTRL_INV : 0;
	u32 period = min_t(u64, st->period, U32_MAX);
	u32 duty = min_t(u64, st->duty_cycle, period);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&p->lock, flags);
	writel(pwm->hwpwm, p->base + RP_SEL);
	if (st->enabled) {
		writel(period, p->base + RP_PERIOD);
		writel(duty, p->base + RP_DUTY);
		ctrl |= CTRL_EN;
	}
	writel(ctrl, p->base + RP_CTRL);
	if (st->enabled && !readl(p->base + RP_PERIOD))
		ret = -EINVAL;
	spin_unlock_irqrestore(&p->lock, flags);
	return ret;
}

static int rp_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
			struct pwm_state *st)
{
	struct rvemu_pwm *p = pwmchip_get_drvdata(chip);
	unsigned long flags;
	u32 ctrl;

	spin_lock_irqsave(&p->lock, flags);
	writel(pwm->hwpwm, p->base + RP_SEL);
	st->period = readl(p->base + RP_PERIOD);
	st->duty_cycle = readl(p->base + RP_DUTY);
	ctrl = readl(p->base + RP_CTRL);
	spin_unlock_irqrestore(&p->lock, flags);
	st->enabled = ctrl & CTRL_EN;
	st->polarity = ctrl & CTRL_INV ? PWM_POLARITY_INVERSED :
					 PWM_POLARITY_NORMAL;
	return 0;
}

static const struct pwm_ops rp_ops = {
	.request = rp_request,
	.apply = rp_apply,
	.get_state = rp_get_state,
};

static int rvemu_pwm_probe(struct platform_device *pdev)
{
	struct pwm_chip *chip;
	struct rvemu_pwm *p;
	void __iomem *base;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);
	chip = devm_pwmchip_alloc(&pdev->dev, readl(base + RP_NPWM), sizeof(*p));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	p = pwmchip_get_drvdata(chip);
	p->base = base;
	spin_lock_init(&p->lock);
	chip->ops = &rp_ops;
	chip->atomic = true;
	return devm_pwmchip_add(&pdev->dev, chip);
}

static const struct of_device_id rvemu_pwm_of_match[] = {
	{ .compatible = "rvemu,pwm" },
	{ }
};

static struct platform_driver rvemu_pwm_driver = {
	.probe = rvemu_pwm_probe,
	.driver = {
		.name = "rvemu-pwm",
		.of_match_table = rvemu_pwm_of_match,
	},
};
builtin_platform_driver(rvemu_pwm_driver);
