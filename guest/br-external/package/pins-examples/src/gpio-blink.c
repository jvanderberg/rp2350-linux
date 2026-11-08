/* Blink a GPIO line through the GPIO character device (v2 ioctls) and read
 * the pad back each time.
 *   gpio-blink [LINE [COUNT [MS]]]    default: GP15, 10 times, 250 ms */
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int line = argc > 1 ? atoi(argv[1]) : 15;
	int count = argc > 2 ? atoi(argv[2]) : 10;
	int ms = argc > 3 ? atoi(argv[3]) : 250;
	struct gpio_v2_line_request req;
	struct gpio_v2_line_values v;
	int chip, i;

	chip = open("/dev/gpiochip0", O_RDONLY);
	if (chip < 0) {
		perror("/dev/gpiochip0");
		return 1;
	}
	memset(&req, 0, sizeof(req));
	req.offsets[0] = line;
	req.num_lines = 1;
	req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
	strcpy(req.consumer, "gpio-blink");
	if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
		perror("GPIO_V2_GET_LINE_IOCTL");
		return 1;
	}
	for (i = 0; i < 2 * count; i++) {
		v.mask = 1;
		v.bits = !(i & 1);
		if (ioctl(req.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) < 0) {
			perror("set");
			return 1;
		}
		v.bits = 0;
		if (ioctl(req.fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) < 0) {
			perror("get");
			return 1;
		}
		printf("GP%d set %d, pad reads %d\n", line, !(i & 1), (int)(v.bits & 1));
		usleep(ms * 1000);
	}
	close(req.fd);
	close(chip);
	return 0;
}
