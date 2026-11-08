/* Read the ADC through IIO sysfs: GP40-GP47 in volts and the temperature
 * sensor in degrees C.
 *   adc-read */
#include <dirent.h>
#include <stdio.h>
#include <string.h>

static char dev[300];

static double get(const char *attr)
{
	char path[400];
	double v = -1;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dev, attr);
	f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%lf", &v) != 1)
			v = -1;
		fclose(f);
	}
	return v;
}

int main(void)
{
	const char *base = "/sys/bus/iio/devices";
	struct dirent *e;
	DIR *d = opendir(base);
	char name[64] = "";
	int ch;

	while (d && (e = readdir(d))) {
		FILE *f;

		snprintf(dev, sizeof(dev), "%s/%s/name", base, e->d_name);
		f = fopen(dev, "r");
		if (f && fgets(name, sizeof(name), f) && !strncmp(name, "rp2350-adc", 10)) {
			fclose(f);
			snprintf(dev, sizeof(dev), "%s/%s", base, e->d_name);
			break;
		}
		if (f)
			fclose(f);
		name[0] = 0;
	}
	if (!name[0]) {
		fprintf(stderr, "no rp2350-adc in %s\n", base);
		return 1;
	}
	for (ch = 0; ch < 8; ch++) {
		char attr[32];
		double raw;

		snprintf(attr, sizeof(attr), "in_voltage%d_raw", ch);
		raw = get(attr);
		printf("GP%d  raw %4.0f  %.3f V\n", 40 + ch, raw, raw * get("in_voltage_scale") / 1000);
	}
	printf("temp raw %4.0f  %.1f C\n", get("in_temp_raw"),
	       (get("in_temp_raw") + get("in_temp_offset")) * get("in_temp_scale") / 1000);
	return 0;
}
