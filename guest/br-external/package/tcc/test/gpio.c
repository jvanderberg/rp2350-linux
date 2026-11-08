#include <stdio.h>
#include <linux/gpio.h>
#include <linux/i2c-dev.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
int main(void) { printf("GPIO_GET_CHIPINFO_IOCTL %lx I2C_SLAVE %x SPI_IOC_WR_MODE %lx\n", (unsigned long)GPIO_GET_CHIPINFO_IOCTL, I2C_SLAVE, (unsigned long)SPI_IOC_WR_MODE); return 0; }
