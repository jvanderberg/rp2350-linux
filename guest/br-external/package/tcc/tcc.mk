################################################################################
#
# tcc
#
################################################################################

TCC_VERSION = 70adbb129760b77e3bdfec447c1f986def763f2c
TCC_SITE = https://repo.or.cz/tinycc.git
TCC_SITE_METHOD = git
TCC_LICENSE = LGPL-2.1+
TCC_LICENSE_FILES = COPYING

# musl keeps libm, libpthread etc. in libc; these empty archives make
# -lm and friends work
TCC_MUSL_EMPTY_LIBS = libm.a libpthread.a librt.a libdl.a libcrypt.a \
	libutil.a libxnet.a libresolv.a

# libgcc members that programs built by tcc never need (quad float
# arithmetic, half and bfloat, _BitInt, complex, x87, support code)
TCC_LIBGCC_DROP = \
	addtf3.o divtf3.o eqtf2.o getf2.o letf2.o multf3.o negtf2.o subtf3.o \
	unordtf2.o fixtfsi.o fixunstfsi.o floatsitf.o floatunsitf.o fixtfdi.o \
	fixunstfdi.o floatditf.o floatunditf.o _powitf2.o _multc3.o _divtc3.o \
	extendhfsf2.o extendhfdf2.o extendhftf2.o extendbfsf2.o trunctfhf2.o \
	truncdfhf2.o truncsfhf2.o trunctfbf2.o truncdfbf2.o truncsfbf2.o \
	trunchfbf2.o truncbfhf2.o fixhfsi.o fixhfdi.o fixunshfsi.o fixunshfdi.o \
	floatsihf.o floatdihf.o floatunsihf.o floatundihf.o floatsibf.o \
	floatdibf.o floatunsibf.o floatundibf.o _mulhc3.o _divhc3.o \
	_mulbitint3.o _divmodbitint4.o fixsfbitint.o floatbitintsf.o \
	fixdfbitint.o floatbitintdf.o _mulsc3.o _muldc3.o _mulxc3.o _divsc3.o \
	_divdc3.o _divxc3.o _fixunsxfsi.o _fixxfdi.o _fixunsxfdi.o _floatdixf.o \
	_floatundixf.o _powixf2.o _eprintf.o __main.o _ctors.o _trampoline.o \
	enable-execute-stack.o hardcfr.o strub.o save-restore.o feature_bits.o \
	multi3.o

# Two builds from one tree: a cross tcc on the build host, only to compile
# libtcc1.a and runmain.o (for tcc -run) for the target, and the tcc that
# runs on the target (built by the target gcc). tccdefs.h is read at run time (predefs=no): compiling it
# in would need a host tool from the target build.
define TCC_BUILD_CMDS
	mkdir -p $(@D)/build-host $(@D)/build-target
	cd $(@D)/build-host && $(HOST_MAKE_ENV) $(@D)/configure \
		--cc="$(HOSTCC)" --config-predefs=no
	$(HOST_MAKE_ENV) $(MAKE) -C $(@D)/build-host riscv32-libtcc1.a
	$(@D)/build-host/riscv32-tcc -B$(@D) -c $(@D)/lib/runmain.c \
		-o $(@D)/build-host/riscv32-runmain.o
	cd $(@D)/build-target && $(TARGET_MAKE_ENV) $(@D)/configure \
		--prefix=/usr \
		--cross-prefix=$(TARGET_CROSS) \
		--cpu=riscv32 \
		--config-musl \
		--config-predefs=no \
		--config-bcheck=no \
		--config-backtrace=no \
		--sysincludepaths="{B}/include:/usr/include" \
		--libpaths="{B}:/usr/lib:/lib" \
		--crtprefix=/usr/lib \
		--extra-cflags="$(TARGET_CFLAGS)" \
		--extra-ldflags="$(TARGET_LDFLAGS)"
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/build-target tcc
endef

# Buildroot's target-finalize deletes /usr/include and every *.a under
# /usr/lib, so what tcc needs to build programs (headers, crt*.o,
# libtcc1.a, libgcc.a) is kept in TCC_DEVROOT and copied into the
# filesystem image's copy of target/ by a ROOTFS_PRE_CMD_HOOK.
TCC_DEVROOT = $(TCC_DIR)/devroot

define TCC_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build-target/tcc $(TARGET_DIR)/usr/bin/tcc
	ln -sf tcc $(TARGET_DIR)/usr/bin/cc
	$(INSTALL) -D -m 0644 $(@D)/build-host/riscv32-libtcc1.a \
		$(TCC_DEVROOT)/usr/lib/tcc/libtcc1.a
	$(INSTALL) -m 0644 $(@D)/build-host/riscv32-runmain.o \
		$(TCC_DEVROOT)/usr/lib/tcc/runmain.o
	$(INSTALL) -m 0644 $(addprefix $(STAGING_DIR)/lib/,$(TCC_MUSL_EMPTY_LIBS)) \
		$(TCC_DEVROOT)/usr/lib/tcc/
	$(INSTALL) -d $(TCC_DEVROOT)/usr/lib/tcc/include
	$(INSTALL) -m 0644 $(@D)/include/*.h $(TCC_DEVROOT)/usr/lib/tcc/include/
	$(INSTALL) -m 0644 $(STAGING_DIR)/lib/crt1.o $(STAGING_DIR)/lib/crti.o \
		$(STAGING_DIR)/lib/crtn.o $(TCC_DEVROOT)/usr/lib/
	cp "$$($(TARGET_CC) -print-libgcc-file-name)" $(@D)/libgcc.a
	$(TARGET_AR) d $(@D)/libgcc.a $(TCC_LIBGCC_DROP)
	$(TARGET_STRIP) -g $(@D)/libgcc.a
	$(INSTALL) -m 0644 $(@D)/libgcc.a $(TCC_DEVROOT)/usr/lib/tcc/libgcc.a
	CC="$(TARGET_CC)" $(BR2_EXTERNAL_RVEMU_PATH)/package/tcc/install-headers.sh \
		$(MUSL_DIR)/include $(STAGING_DIR)/usr/include \
		$(TCC_DEVROOT)/usr/include $(@D)
endef

define TCC_INSTALL_DEVFILES
	cp -a $(TCC_DEVROOT)/. $(TARGET_DIR)/
endef
# A global hook, so only when tcc is selected: the initramfs guest has no
# tcc build directory to copy from
ifeq ($(BR2_PACKAGE_TCC),y)
ROOTFS_PRE_CMD_HOOKS += TCC_INSTALL_DEVFILES
endif

$(eval $(generic-package))
