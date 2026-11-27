# KonaCommander for KonaOS.  The first build unpacks the standalone SDK from
# the sibling ZephyrOS checkout and then re-enters this makefile.

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

# KonaCommander is large enough that size-oriented SDCC allocation is part of
# its build contract. Its 896-byte stack preserves substantial headroom while
# leaving the fixed application window room for libtui's render transaction.
EXE_CFLAGS += --opt-code-size --max-allocs-per-node 50000

# The program is larger than its bank, so the SDK places it (sdk.md 3);
# a PLACE= option makes it placed.  libtui's mouse member is resident in
# every placed program (the SDK declares it reached through a function
# pointer); the pin only states it.  The keyboard path (tui_key and the
# key decoder under it) is resident too, so a key reaches the UI in the
# home zone with no crossing.  The PC-protocol client (kc_remote.c) and the
# frame codec (kc_protocol.c) are cold code (a few calls per file
# operation, each a serial round trip of milliseconds) and so is the number
# formatting only printf reaches: they take the far segment.  The serial
# link (kc_serial.c) stays resident: it polls the port while the PC is
# sending.  The pins leave the resident zone room for a one-line change.
KC_PIN_RESIDENT := libtui.lib(tui_mouse.rel)=resident:libtui.lib(tui_key.rel)=resident:libkos.lib(key.rel)=resident
KC_PIN_FAR := kc_remote.c=far1:kc_protocol.c=far1:libkos.lib(itoa.rel)=far1:libkos.lib(ltoa.rel)=far1:libkos.lib(ultoa.rel)=far1
KC_PLACE := PLACE=$(KC_PIN_RESIDENT):$(KC_PIN_FAR)

$(eval $(call exe_program,kc,src,main.c kc_protocol.c kc_serial.c kc_remote.c kc_ui.c,STACK=896,LIBS=tui_widgets:tui,$(KC_PLACE)))

.PHONY: all clean test
.DEFAULT_GOAL := all

all: kc

test: kc
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

