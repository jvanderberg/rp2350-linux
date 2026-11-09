################################################################################
#
# Buildroot's dropbear on the rvemu native engine: X25519, Ed25519,
# sntrup761 and chacha20-poly1305@openssh.com packets through
# /dev/rvemu-crypto and /dev/rvemu-ssh (guest/linux/drivers/rvemu), with
# Dropbear's own code where the devices are missing. Not a package of its
# own: a hook on Buildroot's dropbear (guest/build.sh rebuilds it when this
# directory or the device headers change).
#
################################################################################

ifeq ($(BR2_PACKAGE_DROPBEAR),y)
DROPBEAR_RVEMU_DIR = $(BR2_EXTERNAL_RVEMU_PATH)/package/dropbear
DROPBEAR_RVEMU_HEADERS = $(BR2_EXTERNAL_RVEMU_PATH)/../linux/drivers/rvemu

define DROPBEAR_RVEMU_ADAPTERS
	cp $(DROPBEAR_RVEMU_DIR)/rvemu-bench.h $(DROPBEAR_RVEMU_DIR)/rvemu-*.inc \
		$(DROPBEAR_RVEMU_HEADERS)/rvemu-crypto.h \
		$(DROPBEAR_RVEMU_HEADERS)/rvemu-ssh.h $(@D)/src/
	python3 $(DROPBEAR_RVEMU_DIR)/patch.py $(@D)
endef
DROPBEAR_POST_PATCH_HOOKS += DROPBEAR_RVEMU_ADAPTERS
endif
