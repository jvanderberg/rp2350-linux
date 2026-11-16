################################################################################
#
# libpio: the PIO blocks for programs, and pioasm
#
################################################################################

LIBPIO_VERSION = 1.0
LIBPIO_SITE = $(BR2_EXTERNAL_RVEMU_PATH)/package/libpio/src
LIBPIO_SITE_METHOD = local
# examples/ws2812.pio, the instruction encoders in pio.h and pioasm's header
# format are from the pico-sdk and pico-examples (BSD-3-Clause, marked in
# each file). rvemu-pio.h, which pio.c includes, is the kernel's uapi header
# (GPL-2.0-only WITH Linux-syscall-note): included, not installed.
LIBPIO_LICENSE = MIT, BSD-3-Clause
LIBPIO_LICENSE_FILES = LICENSE LICENSE-BSD-3-Clause
LIBPIO_INSTALL_STAGING = YES
LIBPIO_HEADERS = $(BR2_EXTERNAL_RVEMU_PATH)/../linux/drivers/rvemu

define LIBPIO_BUILD_CMDS
	cp $(LIBPIO_HEADERS)/rvemu-pio.h $(@D)/
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS) -Wall -Werror"
endef

define LIBPIO_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/pio.h $(STAGING_DIR)/usr/include/pio.h
	$(INSTALL) -D -m 0755 $(@D)/libpio.so $(STAGING_DIR)/usr/lib/libpio.so
endef

# Buildroot's target-finalize deletes /usr/include, so pio.h (for tcc on
# the board) is copied into the filesystem image's copy of target/ by a
# ROOTFS_PRE_CMD_HOOK, as tcc's headers are
LIBPIO_DEVROOT = $(LIBPIO_DIR)/devroot

define LIBPIO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/libpio.so $(TARGET_DIR)/usr/lib/libpio.so
	$(INSTALL) -D -m 0755 $(@D)/pioasm $(TARGET_DIR)/usr/bin/pioasm
	$(INSTALL) -D -m 0644 $(@D)/pio.h $(LIBPIO_DEVROOT)/usr/include/pio.h
	$(INSTALL) -d -m 0755 $(TARGET_DIR)/usr/share/examples/pio
	$(INSTALL) -m 0644 $(@D)/examples/* $(TARGET_DIR)/usr/share/examples/pio/
endef

define LIBPIO_INSTALL_DEVFILES
	cp -a $(LIBPIO_DEVROOT)/. $(TARGET_DIR)/
endef
ifeq ($(BR2_PACKAGE_LIBPIO),y)
ROOTFS_PRE_CMD_HOOKS += LIBPIO_INSTALL_DEVFILES
endif

$(eval $(generic-package))
