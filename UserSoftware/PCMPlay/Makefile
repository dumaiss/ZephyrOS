# PCMPlay for KonaOS (experimental PCM player).  The first build unpacks the
# standalone SDK from the sibling ZephyrOS checkout and then re-enters this
# makefile.

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

$(eval $(call exe_program,pcmplay,src,pcmplay.c,STACK=512,LIBS=snd))

.PHONY: all clean test
.DEFAULT_GOAL := all

all: pcmplay

test: pcmplay
	python3 -m unittest discover -s tests -p 'test_*.py' -v
	$(KOS_PYTHON) $(KOS_TOOLS)/exepack.py validate build/user/pcmplay.exe

clean:
	rm -rf build

endif
