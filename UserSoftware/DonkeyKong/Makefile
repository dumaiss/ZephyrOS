# DonkeyKongKos: native V9958 re-host of the 16 KiB ColecoVision cartridge.
# Replacing the packaged SDK automatically refreshes the private copy.

KOS_SDK_ARCHIVE ?= ../../../../ZephyrOS/build/kos-sdk-0.1.0.tar.gz
KOS_SDK_ROOT ?= build/kos-sdk-0.1.0
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

# CODEBASE leaves 015Dh-1FFFh free for the run-time copy of OS-7.  The SDK
# gate data remains at 0100h-015Ch; the native code and libkos start at 2000h.
$(eval $(call exe_program,dkkos,.,main.c native.asm,STACK=1024,CODEBASE=0x2000,INCLUDES=$(KOS_SDK_ROOT)/include/kos))

.PHONY: all clean test
.DEFAULT_GOAL := all

all: dkkos

test: dkkos
	python3 -m unittest discover -s tests -v
	$(KOS_PYTHON) $(KOS_TOOLS)/exepack.py validate build/user/dkkos.exe

clean:
	rm -rf build

endif
