################################################################################
#
# micropython-machine: machine.Pin, I2C, SPI, PWM, ADC over Linux interfaces
#
################################################################################

MICROPYTHON_MACHINE_VERSION = 1.0
MICROPYTHON_MACHINE_SITE = $(BR2_EXTERNAL_RVEMU_PATH)/package/micropython-machine
MICROPYTHON_MACHINE_SITE_METHOD = local
# rp2.py's assembler is MicroPython's (MIT, Damien P. George)
MICROPYTHON_MACHINE_LICENSE = MIT
MICROPYTHON_MACHINE_LICENSE_FILES = LICENSE LICENSE-micropython
MICROPYTHON_MACHINE_DEPENDENCIES = micropython

ifeq ($(BR2_PACKAGE_MICROPYTHON_MACHINE),y)
# fcntl.ioctl and _rvmem (mmap, 32-bit register access, for rp2.py),
# compiled into the MicroPython binary (guest/build.sh rebuilds MicroPython
# when cmod/ changes: the build directory is shared)
MICROPYTHON_MAKE_OPTS += USER_C_MODULES=$(BR2_EXTERNAL_RVEMU_PATH)/package/micropython-machine/cmod
endif

# precompiled, so importing it doesn't parse the source each time
define MICROPYTHON_MACHINE_BUILD_CMDS
	$(MICROPYTHON_DIR)/mpy-cross/build/mpy-cross -O2 -o $(@D)/machine.mpy $(@D)/machine.py
	$(MICROPYTHON_DIR)/mpy-cross/build/mpy-cross -O2 -o $(@D)/rp2.mpy $(@D)/rp2.py
endef

define MICROPYTHON_MACHINE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/machine.mpy $(TARGET_DIR)/usr/lib/micropython/machine.mpy
	$(INSTALL) -D -m 0644 $(@D)/rp2.mpy $(TARGET_DIR)/usr/lib/micropython/rp2.mpy
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/usr/share/examples/pio
	$(INSTALL) -m 0644 $(@D)/examples/*.py $(TARGET_DIR)/usr/share/examples/pio/
endef

$(eval $(generic-package))
