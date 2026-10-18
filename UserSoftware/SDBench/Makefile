# SDBench for KonaOS.  The first build unpacks the standalone application
# SDK supplied by the sibling ZephyrOS checkout, then re-enters this makefile
# with the SDK available.

KOS_SDK_ARCHIVE ?= ../../../../ZephyrOS/build/kos-sdk-0.1.0.tar.gz
KOS_SDK_ROOT ?= build/kos-sdk-0.1.0
KOS_SDK_HASH := $(shell sha256sum "$(KOS_SDK_ARCHIVE)" 2>/dev/null | cut -c1-64)
KOS_SDK_STORED_HASH := $(shell sed -n '1p' build/.kos-sdk-sha256 2>/dev/null)
KOS_SDK_READY := $(and $(wildcard $(KOS_SDK_ROOT)/mk/kos.mk),$(filter $(KOS_SDK_HASH),$(KOS_SDK_STORED_HASH)))

ifeq ($(KOS_SDK_READY),)

.PHONY: all bootstrap clean
.DEFAULT_GOAL := all

all: bootstrap

bootstrap:
	@test -f "$(KOS_SDK_ARCHIVE)" || { \
		echo "missing KonaOS SDK: $(KOS_SDK_ARCHIVE)"; exit 1; \
	}
	mkdir -p build
	rm -rf "$(KOS_SDK_ROOT)"
	tar -xzf "$(KOS_SDK_ARCHIVE)" -C build
	@touch "$(KOS_SDK_ROOT)/lib/libkos.lib" "$(KOS_SDK_ROOT)/lib/crt0.rel"
	@printf '%s\n' "$(KOS_SDK_HASH)" > build/.kos-sdk-sha256
	$(MAKE) --no-print-directory KOS_SDK_ROOT="$(abspath $(KOS_SDK_ROOT))" all

clean:
	rm -rf build

else

include $(KOS_SDK_ROOT)/mk/kos.mk

$(eval $(call exe_program,sdbench,.,sdbench.c,STACK=512))

.PHONY: all clean
.DEFAULT_GOAL := all

all: sdbench

clean:
	rm -rf build

endif
