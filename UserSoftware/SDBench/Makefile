# SDBench for KonaOS.  The first build unpacks the standalone application
# SDK supplied by the sibling ZephyrOS checkout, then re-enters this makefile
# with the SDK available.

include ../sdk.mk
KOS_SDK_HASH := $(shell sha256sum "$(KOS_SDK_ARCHIVE)" 2>/dev/null | cut -c1-64)
KOS_SDK_STORED_HASH := $(shell sed -n '1p' build/.kos-sdk-sha256 2>/dev/null)
KOS_SDK_READY := $(and $(wildcard $(KOS_SDK_ROOT)/mk/kos.mk),$(filter $(KOS_SDK_HASH),$(KOS_SDK_STORED_HASH)))

ifeq ($(KOS_SDK_READY),)

.PHONY: all bootstrap clean test
.DEFAULT_GOAL := all

all: bootstrap

bootstrap:
	@test -f "$(KOS_SDK_ARCHIVE)" || { \
		echo "missing KonaOS SDK: $(KOS_SDK_ARCHIVE)"; exit 1; \
	}
	mkdir -p build
	rm -rf "$(KOS_SDK_ROOT)"
	tar -xzf "$(KOS_SDK_ARCHIVE)" -C build
	@touch "$(KOS_SDK_ROOT)"/lib/*
	@printf '%s\n' "$(KOS_SDK_HASH)" > build/.kos-sdk-sha256
	@printf '%s\n' "$(BOARD)" > build/.board
	$(MAKE) --no-print-directory KOS_SDK_ROOT="$(abspath $(KOS_SDK_ROOT))" all

test: bootstrap
	$(MAKE) --no-print-directory KOS_SDK_ROOT="$(abspath $(KOS_SDK_ROOT))" test

clean:
	rm -rf build

else

include $(KOS_SDK_ROOT)/mk/kos.mk

$(eval $(call exe_program,sdbench,.,sdbench.c,STACK=512))

.PHONY: all clean test
.DEFAULT_GOAL := all

all: sdbench

# The timed run and -n, launched from the shell of the board's production
# image (tests/test_app_launch.py).
test: sdbench
	cd ../.. && APPS=SDBench BOARD=$(BOARD) python3 -m unittest tests.test_app_launch -v

clean:
	rm -rf build

endif
