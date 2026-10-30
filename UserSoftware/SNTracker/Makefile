# SNTrackerKos for KonaOS.  The SDK archive is the build input: replacing it
# automatically refreshes the private extracted copy before relinking.

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
	@touch "$(KOS_SDK_ROOT)"/lib/*
	@printf '%s\n' "$(KOS_SDK_HASH)" > build/.kos-sdk-sha256
	$(MAKE) --no-print-directory KOS_SDK_ROOT="$(abspath $(KOS_SDK_ROOT))" all

clean:
	rm -rf build

test: bootstrap
	$(MAKE) --no-print-directory KOS_SDK_ROOT="$(abspath $(KOS_SDK_ROOT))" test

else

include $(KOS_SDK_ROOT)/mk/kos.mk

$(eval $(call exe_program,sntrack,.,sntracker.c,STACK=1024,LIBS=snd))
$(eval $(call exe_program,sntview,.,sntview.c ztrdoc.c view.c,STACK=768,LIBS=gfx))

.PHONY: all clean test
.DEFAULT_GOAL := all

all: sntrack sntview

test: sntrack sntview
	python3 -m unittest discover -s tests -p 'test_*.py' -v
	$(KOS_PYTHON) $(KOS_TOOLS)/exepack.py validate build/user/sntrack.exe
	$(KOS_PYTHON) $(KOS_TOOLS)/exepack.py validate build/user/sntview.exe

clean:
	rm -rf build

endif
