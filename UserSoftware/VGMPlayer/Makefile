# VGMPlayer for KonaOS.  The first build unpacks the standalone application
# SDK supplied by the sibling ZephyrOS checkout, then re-enters this makefile
# with the SDK available.

KOS_SDK_ARCHIVE ?= ../../../../ZephyrOS/build/kos-sdk-0.1.0.tar.gz
KOS_SDK_ROOT ?= build/kos-sdk-0.1.0

ifeq ($(wildcard $(KOS_SDK_ROOT)/mk/kos.mk),)

.PHONY: all bootstrap clean
.DEFAULT_GOAL := all

all: bootstrap

bootstrap:
	@test -f "$(KOS_SDK_ARCHIVE)" || { \
		echo "missing KonaOS SDK: $(KOS_SDK_ARCHIVE)"; exit 1; \
	}
	mkdir -p build
	tar -xzf "$(KOS_SDK_ARCHIVE)" -C build
	$(MAKE) --no-print-directory KOS_SDK_ROOT="$(abspath $(KOS_SDK_ROOT))" all

clean:
	rm -rf build

else

include $(KOS_SDK_ROOT)/mk/kos.mk

# diag.h is the SDK diagnostic wrapper.  As documented by the SDK, it is
# included from the wrapper directory so its own kos/diag.h include resolves
# to the public ABI copy first.
$(eval $(call exe_program,vgmplay,.,vgmplay.c,STACK=768,\
	INCLUDES=$(KOS_SDK_ROOT)/include/kos))

.PHONY: all clean
.DEFAULT_GOAL := all

all: vgmplay

clean:
	rm -rf build

endif
