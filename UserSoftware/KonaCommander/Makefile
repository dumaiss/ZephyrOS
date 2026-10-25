# KonaCommander for KonaOS.  The first build unpacks the standalone SDK from
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
	@touch "$(KOS_SDK_ROOT)"/lib/*
	@printf '%s\n' "$(KOS_SDK_HASH)" > build/.kos-sdk-sha256
	$(MAKE) --no-print-directory KOS_SDK_ROOT="$(abspath $(KOS_SDK_ROOT))" all

test: bootstrap
	$(MAKE) --no-print-directory KOS_SDK_ROOT="$(abspath $(KOS_SDK_ROOT))" test

clean:
	rm -rf build

else

include $(KOS_SDK_ROOT)/mk/kos.mk

# KonaCommander is large enough that size-oriented SDCC allocation is part of
# its build contract. Its 896-byte stack preserves substantial headroom while
# leaving the fixed application window room for libtui's render transaction.
EXE_CFLAGS += --opt-code-size --max-allocs-per-node 50000

$(eval $(call exe_program,konacmd,src,main.c kc_protocol.c kc_serial.c kc_remote.c kc_ui.c,STACK=896,LIBS=tui_widgets:tui))

.PHONY: all clean test
.DEFAULT_GOAL := all

all: konacmd

test: konacmd
	mkdir -p build/tests
	$(CC) -std=c99 -Wall -Wextra -Werror -Isrc \
		tests/test_protocol.c src/kc_protocol.c -o build/tests/test_protocol
	build/tests/test_protocol
	python3 -m unittest -v tests/test_host.py
	python3 -m unittest -v tests/test_project.py
	python3 -m unittest -v tests/test_ui_sim.py

clean:
	rm -rf build

endif

