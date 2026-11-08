/* Edge interrupts without any wiring: request a line as an input with
 * edge detection, then flip its pull between up and down. The floating pad
 * follows the pull, and each change arrives as an edge event.
 *   gpio-edge [LINE [FLIPS]]    default: GP14, 4 flips */
#include <fcntl.h>
#include <linux/gpio.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define EDGES (GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING)

int main(int argc, char **argv)
{
	int line = argc > 1 ? atoi(argv[1]) : 14;
	int flips = argc > 2 ? atoi(argv[2]) : 4;
	struct gpio_v2_line_request req;
	struct gpio_v2_line_config cfg;
	struct gpio_v2_line_event ev;
	struct pollfd p;
	int chip, i, got = 0;

	chip = open("/dev/gpiochip0", O_RDONLY);
	if (chip < 0) {
		perror("/dev/gpiochip0");
		return 1;
	}
	memset(&req, 0, sizeof(req));
	req.offsets[0] = line;
	req.num_lines = 1;
	req.config.flags = EDGES | GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
	strcpy(req.consumer, "gpio-edge");
	if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
		perror("GPIO_V2_GET_LINE_IOCTL");
		return 1;
	}
	usleep(10000);
	for (i = 0; i < flips; i++) {
		int up = !(i & 1);

		memset(&cfg, 0, sizeof(cfg));
		cfg.flags = EDGES | (up ? GPIO_V2_LINE_FLAG_BIAS_PULL_UP : GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN);
		if (ioctl(req.fd, GPIO_V2_LINE_SET_CONFIG_IOCTL, &cfg) < 0) {
			perror("GPIO_V2_LINE_SET_CONFIG_IOCTL");
			return 1;
		}
		p.fd = req.fd;
		p.events = POLLIN;
		if (poll(&p, 1, 1000) == 1 && read(req.fd, &ev, sizeof(ev)) == sizeof(ev)) {
			printf("pull-%s: %s edge on GP%u (seqno %u)\n", up ? "up" : "down",
			       ev.id == GPIO_V2_LINE_EVENT_RISING_EDGE ? "rising" : "falling", ev.offset, ev.seqno);
			got++;
		} else {
			printf("pull-%s: no edge within 1 s\n", up ? "up" : "down");
		}
	}
	printf("%d of %d edges\n", got, flips);
	return got == flips ? 0 : 1;
}
