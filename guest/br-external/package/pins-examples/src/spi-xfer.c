/* One full-duplex SPI transfer through spidev: send the bytes given (hex)
 * and print what came back. With MOSI (GP19) wired to MISO (GP16) the
 * reply is the message.
 *   spi-xfer [-s HZ] [-m MODE] BYTE...    default: 1 MHz, mode 0 */
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	uint8_t tx[256], rx[256], mode = 0;
	uint32_t hz = 1000000;
	struct spi_ioc_transfer t;
	int fd, i, n = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-s") && i + 1 < argc)
			hz = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "-m") && i + 1 < argc)
			mode = strtoul(argv[++i], NULL, 0);
		else if (n < (int)sizeof(tx))
			tx[n++] = strtoul(argv[i], NULL, 16);
	}
	if (!n) {
		fprintf(stderr, "usage: spi-xfer [-s HZ] [-m MODE] BYTE...\n");
		return 2;
	}
	fd = open("/dev/spidev0.0", O_RDWR);
	if (fd < 0) {
		perror("/dev/spidev0.0");
		return 1;
	}
	if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0) {
		perror("SPI_IOC_WR_MODE");
		return 1;
	}
	memset(&t, 0, sizeof(t));
	t.tx_buf = (uintptr_t)tx;
	t.rx_buf = (uintptr_t)rx;
	t.len = n;
	t.speed_hz = hz;
	t.bits_per_word = 8;
	if (ioctl(fd, SPI_IOC_MESSAGE(1), &t) < 0) {
		perror("SPI_IOC_MESSAGE");
		return 1;
	}
	printf("tx:");
	for (i = 0; i < n; i++)
		printf(" %02x", tx[i]);
	printf("\nrx:");
	for (i = 0; i < n; i++)
		printf(" %02x", rx[i]);
	printf("\n");
	close(fd);
	return 0;
}
