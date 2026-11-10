// SPDX-License-Identifier: GPL-2.0
/*
 * SPI controller of the rvemu machine (rvemu-rs/rvcore/src/periph.rs), one per
 * RP2350 SPI block, with one chip select. Writing GO runs one transfer, start
 * to end, before the write returns: LEN bytes from TX (guest physical; 0
 * sends zeros) while what comes back goes to RX (0: dropped).
 *
 * The transfer is synchronous, so the controller is "unqueued" (.transfer):
 * the SPI core then needs no message-pump thread, which on this 8 MiB machine
 * matters more than the deprecation note it prints.
 *
 * SKIP waits in the firmware for a byte other than the one given (an SD
 * card's data token, or the end of busy): rvemu_spi_skip, for mmc_spi.
 */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include "rvemu-spi.h"

#define RS_TX		0x00
#define RS_RX		0x04
#define RS_LEN		0x08
#define RS_HZ		0x0c
#define RS_MODE		0x10	/* SPI_CPHA | SPI_CPOL | SPI_LSB_FIRST */
#define RS_CS		0x14	/* 1: chip select pin low */
#define RS_GO		0x18
#define RS_SKIP		0x1c	/* write BYTE | MAX << 8; read the byte, or SKIP_NONE */
#define SKIP_MAX	4096
#define SKIP_NONE	0x100
#define MODE_BITS	(SPI_CPHA | SPI_CPOL | SPI_LSB_FIRST)
#define MAX_HZ		50000000
#define MIN_HZ		100000	/* SPI_MIN_HZ: the emulator waits out each transfer */

struct rvemu_spi {
	void __iomem *base;
};

static int rs_buf(const void *p, u32 *pa)
{
	*pa = 0;
	if (!p)
		return 0;
	if (!virt_addr_valid(p))
		return -EINVAL;
	*pa = virt_to_phys((void *)p);
	return 0;
}

/* RS_CS for chip select asserted: SPI_CS_HIGH asserts with the pin high
 * (mmc_spi's power-up clocks) */
static u32 rs_on(struct spi_device *spi)
{
	return !(spi->mode & SPI_CS_HIGH);
}

/* setup leaves chip select inactive */
static int rs_setup(struct spi_device *spi)
{
	struct rvemu_spi *rs = spi_controller_get_devdata(spi->controller);

	writel(!rs_on(spi), rs->base + RS_CS);
	return 0;
}

static int rs_transfer(struct spi_device *spi, struct spi_message *msg)
{
	struct rvemu_spi *rs = spi_controller_get_devdata(spi->controller);
	struct spi_transfer *t;
	u32 on = rs_on(spi);
	bool keep = false;
	u32 tx, rx;
	int ret = 0;

	writel(spi->mode & MODE_BITS, rs->base + RS_MODE);
	writel(on, rs->base + RS_CS);
	list_for_each_entry(t, &msg->transfers, transfer_list) {
		ret = rs_buf(t->tx_buf, &tx) ?: rs_buf(t->rx_buf, &rx);
		if (ret)
			break;
		writel(tx, rs->base + RS_TX);
		writel(rx, rs->base + RS_RX);
		writel(t->len, rs->base + RS_LEN);
		writel(t->speed_hz ?: spi->max_speed_hz, rs->base + RS_HZ);
		writel(1, rs->base + RS_GO);
		msg->actual_length += t->len;
		spi_transfer_delay_exec(t);
		if (!t->cs_change)
			continue;
		if (list_is_last(&t->transfer_list, &msg->transfers)) {
			/* on the last transfer: chip select stays on after it */
			keep = true;
			break;
		}
		writel(!on, rs->base + RS_CS);
		spi_transfer_cs_change_delay_exec(msg, t);
		writel(on, rs->base + RS_CS);
	}
	if (!keep || ret)
		writel(!on, rs->base + RS_CS);
	msg->status = ret;
	if (msg->complete)
		msg->complete(msg->context);
	return 0;
}

int rvemu_spi_skip(struct spi_device *spi, u8 byte, unsigned int max)
{
	struct rvemu_spi *rs;
	u32 r;

	if (spi->controller->transfer != rs_transfer)
		return -EOPNOTSUPP;
	rs = spi_controller_get_devdata(spi->controller);
	writel(spi->mode & MODE_BITS, rs->base + RS_MODE);
	writel(spi->max_speed_hz, rs->base + RS_HZ);
	/* chip select on, and left on, as a message whose last transfer sets
	 * cs_change leaves it (mmc_spi's readback) */
	writel(rs_on(spi), rs->base + RS_CS);
	writel(byte | min_t(u32, max, SKIP_MAX) << 8, rs->base + RS_SKIP);
	r = readl(rs->base + RS_SKIP);
	return r == SKIP_NONE ? -EAGAIN : r;
}
EXPORT_SYMBOL_GPL(rvemu_spi_skip);

static int rvemu_spi_probe(struct platform_device *pdev)
{
	struct spi_controller *host;
	struct rvemu_spi *rs;

	host = devm_spi_alloc_host(&pdev->dev, sizeof(*rs));
	if (!host)
		return -ENOMEM;
	rs = spi_controller_get_devdata(host);
	rs->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rs->base))
		return PTR_ERR(rs->base);
	host->dev.of_node = pdev->dev.of_node;
	host->bus_num = -1;
	host->num_chipselect = 1;
	host->mode_bits = MODE_BITS | SPI_CS_HIGH;
	host->bits_per_word_mask = SPI_BPW_MASK(8);
	host->max_speed_hz = MAX_HZ;
	host->min_speed_hz = MIN_HZ;
	host->setup = rs_setup;
	host->transfer = rs_transfer;
	return devm_spi_register_controller(&pdev->dev, host);
}

static const struct of_device_id rvemu_spi_of_match[] = {
	{ .compatible = "rvemu,spi" },
	{ }
};

static struct platform_driver rvemu_spi_driver = {
	.probe = rvemu_spi_probe,
	.driver = {
		.name = "rvemu-spi",
		.of_match_table = rvemu_spi_of_match,
	},
};
builtin_platform_driver(rvemu_spi_driver);
