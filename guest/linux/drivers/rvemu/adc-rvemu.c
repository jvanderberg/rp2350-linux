// SPDX-License-Identifier: GPL-2.0
/*
 * ADC of the rvemu machine (rvemu-rs/rvcore/src/periph.rs): the RP2350B's
 * 12-bit SAR ADC. Reading register 4*N converts channel N and returns the
 * result (bit 31: conversion error). Channels 0-7 are GP40-GP47, channel 8
 * the internal temperature sensor.
 */
#include <linux/iio/iio.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define RA_RESULT(ch)	((ch) * 4)
#define  RESULT_ERR	BIT(31)
#define TEMP_CH		8
#define VREF_MV		3300

struct rvemu_adc {
	void __iomem *base;
};

#define RA_VOLTAGE(ch) {						\
	.type = IIO_VOLTAGE,						\
	.indexed = 1,							\
	.channel = (ch),						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
}

static const struct iio_chan_spec ra_channels[] = {
	RA_VOLTAGE(0), RA_VOLTAGE(1), RA_VOLTAGE(2), RA_VOLTAGE(3),
	RA_VOLTAGE(4), RA_VOLTAGE(5), RA_VOLTAGE(6), RA_VOLTAGE(7),
	{
		.type = IIO_TEMP,
		.channel = TEMP_CH,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_OFFSET) |
				      BIT(IIO_CHAN_INFO_SCALE),
	},
};

/*
 * RP2350 datasheet, temperature sensor: T = 27 - (V - 0.706) / 0.001721 (degC).
 * With V = raw * 3.3 / 4096 that is (raw - 933.977) * -468.135 m°C.
 */
static int ra_read_raw(struct iio_dev *iio, struct iio_chan_spec const *ch,
		       int *val, int *val2, long mask)
{
	struct rvemu_adc *a = iio_priv(iio);
	u32 r;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		r = readl(a->base + RA_RESULT(ch->channel));
		if (r & RESULT_ERR)
			return -EIO;
		*val = r & 0xfff;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		if (ch->type == IIO_TEMP) {
			*val = -468;
			*val2 = 135000;
			return IIO_VAL_INT_PLUS_MICRO;
		}
		*val = VREF_MV;
		*val2 = 12;
		return IIO_VAL_FRACTIONAL_LOG2;
	case IIO_CHAN_INFO_OFFSET:
		*val = -933;
		*val2 = 977000;
		return IIO_VAL_INT_PLUS_MICRO;
	}
	return -EINVAL;
}

static const struct iio_info ra_info = {
	.read_raw = ra_read_raw,
};

static int rvemu_adc_probe(struct platform_device *pdev)
{
	struct iio_dev *iio;
	struct rvemu_adc *a;

	iio = devm_iio_device_alloc(&pdev->dev, sizeof(*a));
	if (!iio)
		return -ENOMEM;
	a = iio_priv(iio);
	a->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(a->base))
		return PTR_ERR(a->base);
	iio->name = "rp2350-adc";
	iio->info = &ra_info;
	iio->modes = INDIO_DIRECT_MODE;
	iio->channels = ra_channels;
	iio->num_channels = ARRAY_SIZE(ra_channels);
	return devm_iio_device_register(&pdev->dev, iio);
}

static const struct of_device_id rvemu_adc_of_match[] = {
	{ .compatible = "rvemu,adc" },
	{ }
};

static struct platform_driver rvemu_adc_driver = {
	.probe = rvemu_adc_probe,
	.driver = {
		.name = "rvemu-adc",
		.of_match_table = rvemu_adc_of_match,
	},
};
builtin_platform_driver(rvemu_adc_driver);
