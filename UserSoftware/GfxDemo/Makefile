# GfxDemo for KonaOS.  The first build unpacks the standalone SDK from
# the sibling ZephyrOS checkout and then re-enters this makefile.

KOS_SDK_ARCHIVE ?= ../../build/kos-sdk-0.2.0.tar.gz
KOS_SDK_ROOT ?= build/kos-sdk-0.2.0
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
	@touch "$(KOS_SDK_ROOT)/lib/libkos.lib" "$(KOS_SDK_ROOT)/lib/crt0.rel"
	@printf '%s\n' "$(KOS_SDK_HASH)" > build/.kos-sdk-sha256
	$(MAKE) --no-print-directory KOS_SDK_ROOT="$(abspath $(KOS_SDK_ROOT))" all

test: bootstrap
	$(MAKE) --no-print-directory KOS_SDK_ROOT="$(abspath $(KOS_SDK_ROOT))" test

clean:
	rm -rf build

else

include $(KOS_SDK_ROOT)/mk/kos.mk

$(eval $(call exe_program,gfxdemo,src,gfxdemo.c,STACK=256,LIBS=gfx))

.PHONY: all clean test
.DEFAULT_GOAL := all

all: gfxdemo

# The demo has no host validator: the test is the production replay
# (tests/test_gfx.py).  Here the rule builds from the repacked SDK and
# confirms the packed program.
test: gfxdemo
	@test -s "$(EXE_USER_ROOT)/gfxdemo.exe" || { \
		echo "gfxdemo: build produced no exe"; exit 1; }
	@echo "gfxdemo: built OK"

clean:
	rm -rf build

endif
