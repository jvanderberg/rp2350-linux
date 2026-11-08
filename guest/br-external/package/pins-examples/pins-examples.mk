################################################################################
#
# pins-examples: C examples for GPIO, I2C, SPI and the ADC
#
################################################################################

PINS_EXAMPLES_VERSION = 1.0
PINS_EXAMPLES_SITE = $(BR2_EXTERNAL_RVEMU_PATH)/package/pins-examples/src
PINS_EXAMPLES_SITE_METHOD = local
PINS_EXAMPLES_LICENSE = MIT

# built here only to check they compile against the target's headers; the
# image carries the sources
define PINS_EXAMPLES_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS) -Wall -Werror"
endef

define PINS_EXAMPLES_INSTALL_TARGET_CMDS
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/usr/share/examples/pins
	$(INSTALL) -m 0644 $(@D)/*.c $(@D)/Makefile $(TARGET_DIR)/usr/share/examples/pins/
endef

$(eval $(generic-package))
