# ColecoGo for KonaOS.  The first build unpacks the standalone SDK supplied
# with ZephyrOS, then re-enters this makefile with the SDK available.

include ../sdk.mk

KOS_SDK_HASH := $(shell sha256sum "$(KOS_SDK_ARCHIVE)" 2>/dev/null | cut -c1-64)
KOS_SDK_STORED_HASH := $(shell sed -n '1p' build/.kos-sdk-sha256 2>/dev/null)
KOS_SDK_READY := $(and $(wildcard $(KOS_SDK_ROOT)/mk/kos.mk),$(filter $(KOS_SDK_HASH),$(KOS_SDK_STORED_HASH)))

ifeq ($(KOS_SDK_READY),)

.PHONY: all bootstrap test clean
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

# The Zephyr-80 handoff switches the memory latch itself (handoff.asm);
# the Mk2 board's is SYS_PERSONALITY.
COLECOGO_SRC.mk1 := colecogo.c video.asm handoff.asm
COLECOGO_SRC.mk2 := colecogo.c video.asm

$(eval $(call exe_program,colecogo,.,$(COLECOGO_SRC.$(KOS_BOARD)),\
	STACK=512,INCLUDES=$(KOS_SDK_ROOT)/include/kos))

.PHONY: all test clean
.DEFAULT_GOAL := all

all: colecogo

test: colecogo
	python3 -m unittest discover -s tests -v

clean:
	rm -rf build

endif
