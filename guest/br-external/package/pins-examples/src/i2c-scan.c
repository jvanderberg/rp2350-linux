/* Probe every 7-bit address on an I2C bus, as i2cdetect does (a
 * zero-length write; the emulator turns it into a one-byte read, which
 * the RP2350's controller can do).
 *   i2c-scan [BUS]    default: 0 */
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int bus = argc > 1 ? atoi(argv[1]) : 0;
	char path[32];
	int fd, a, found = 0;

	snprintf(path, sizeof(path), "/dev/i2c-%d", bus);
	fd = open(path, O_RDWR);
	if (fd < 0) {
		perror(path);
		return 1;
	}
	printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f");
	for (a = 0; a < 0x80; a++) {
		struct i2c_msg m = { .addr = a, .flags = 0, .len = 0, .buf = NULL };
		struct i2c_rdwr_ioctl_data d = { .msgs = &m, .nmsgs = 1 };

		if (a % 16 == 0)
			printf("\n%02x:", a);
		if (a < 0x08 || a > 0x77) {
			printf("   ");
			continue;
		}
		if (ioctl(fd, I2C_RDWR, &d) == 1) {
			printf(" %02x", a);
			found++;
		} else {
			printf(" --");
		}
	}
	printf("\n%d device(s) on %s\n", found, path);
	close(fd);
	return 0;
}
