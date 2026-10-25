# VGMPlayer for KonaOS.  Replacing the packaged SDK refreshes the private
# extracted copy before the application is relinked.

KOS_SDK_ARCHIVE ?= ../../build/kos-sdk-0.1.0.tar.gz
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
	@touch "$(KOS_SDK_ROOT)"/lib/*
	@printf '%s\n' "$(KOS_SDK_HASH)" > build/.kos-sdk-sha256
	$(MAKE) --no-print-directory KOS_SDK_ROOT="$(abspath $(KOS_SDK_ROOT))" all

clean:
	rm -rf build

test: bootstrap
	$(MAKE) --no-print-directory KOS_SDK_ROOT="$(abspath $(KOS_SDK_ROOT))" test

else

include $(KOS_SDK_ROOT)/mk/kos.mk

$(eval $(call exe_program,vgmplay,.,vgmplay.c,STACK=768,\
	INCLUDES=$(KOS_SDK_ROOT)/include/kos))

.PHONY: all clean test
.DEFAULT_GOAL := all

all: vgmplay

test: vgmplay
	python3 -m unittest discover -s tests -p 'test_*.py' -v
	$(KOS_PYTHON) $(KOS_TOOLS)/exepack.py validate build/user/vgmplay.exe

clean:
	rm -rf build

endif
