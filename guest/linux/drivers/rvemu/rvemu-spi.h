/* Kernel-internal: the rvemu SPI controller's wait in the firmware
 * (spi-rvemu.c), for mmc_spi's token and busy waits. */
#ifndef RVEMU_SPI_H
#define RVEMU_SPI_H
#include <linux/spi/spi.h>
/* Clock 0xff out until a byte other than @byte comes back, at most @max
 * bytes, with chip select as it is: that byte, -EAGAIN if none differed, or
 * -EOPNOTSUPP if @spi's controller isn't an rvemu one. */
int rvemu_spi_skip(struct spi_device *spi, u8 byte, unsigned int max);
#endif
