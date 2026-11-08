################################################################################
#
# pins-examples: C examples for GPIO, I2C, SPI and the ADC
#
################################################################################

PINS_EXAMPLES_VERSION = 1.0
PINS_EXAMPLES_SITE = $(BR2_EXTERNAL_RVEMU_PATH)/package/pins-examples/src
PINS_EXAMPLES_SITE_METHOD = local
PINS_EXAMPLES_LICENSE = MIT
PINS_EXAMPLES_PROGS = gpio-blink gpio-edge i2c-scan spi-xfer adc-read

# the image carries the sources (to compile on the board) and the programs
define PINS_EXAMPLES_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS) -Wall -Werror"
endef

define PINS_EXAMPLES_INSTALL_TARGET_CMDS
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/usr/share/examples/pins
	$(INSTALL) -m 0644 $(@D)/*.c $(@D)/Makefile $(TARGET_DIR)/usr/share/examples/pins/
	$(INSTALL) -m 0755 $(addprefix $(@D)/,$(PINS_EXAMPLES_PROGS)) $(TARGET_DIR)/usr/share/examples/pins/
endef

$(eval $(generic-package))
