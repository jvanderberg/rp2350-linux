// SPDX-License-Identifier: GPL-2.0
/*
 * SPI controller of the rvemu machine (rvemu-rs/rvcore/src/periph.rs), on the
 * RP2350's SPI0 with one chip select. Writing GO runs one transfer, start to
 * end, before the write returns: LEN bytes from TX (guest physical; 0 sends
 * zeros) while what comes back goes to RX (0: dropped).
 *
 * The transfer is synchronous, so the controller is "unqueued" (.transfer):
 * the SPI core then needs no message-pump thread, which on this 8 MiB machine
 * matters more than the deprecation note it prints.
 */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>

#define RS_TX		0x00
#define RS_RX		0x04
#define RS_LEN		0x08
#define RS_HZ		0x0c
#define RS_MODE		0x10	/* SPI_CPHA | SPI_CPOL | SPI_LSB_FIRST */
#define RS_CS		0x14	/* 1: chip select active */
#define RS_GO		0x18
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

static int rs_transfer(struct spi_device *spi, struct spi_message *msg)
{
	struct rvemu_spi *rs = spi_controller_get_devdata(spi->controller);
	struct spi_transfer *t;
	u32 tx, rx;
	int ret = 0;

	writel(spi->mode & MODE_BITS, rs->base + RS_MODE);
	writel(1, rs->base + RS_CS);
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
		if (t->cs_change && !list_is_last(&t->transfer_list, &msg->transfers)) {
			writel(0, rs->base + RS_CS);
			spi_transfer_cs_change_delay_exec(msg, t);
			writel(1, rs->base + RS_CS);
		}
	}
	writel(0, rs->base + RS_CS);
	msg->status = ret;
	if (msg->complete)
		msg->complete(msg->context);
	return 0;
}

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
	host->mode_bits = MODE_BITS;
	host->bits_per_word_mask = SPI_BPW_MASK(8);
	host->max_speed_hz = MAX_HZ;
	host->min_speed_hz = MIN_HZ;
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
