# KonaEdit for KonaOS. The packaged SDK is the only target build input.

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

EXE_CFLAGS += --opt-code-size --max-allocs-per-node 50000

# The program is larger than its bank, so the SDK places it (sdk.md 3).
# libtui's mouse member is resident in every placed program (the SDK
# declares it reached through a function pointer).  The file layer, the dialog widgets and the init/shutdown member
# are the cold modules; the store, the text walks, the drawing, the
# history and the key decoder keep the resident room.
KE_PLACE := PLACE=ke_file.c=far1:ke_widgets.c=far1:libtui.lib(tui_lifecycle.rel)=far1

$(eval $(call exe_program,kedit,src,main.c ke_store.c ke_platform.c ke_history.c ke_text.c ke_file.c ke_widgets.c ke_view.c ke_ui.c,STACK=1024,LIBS=tui,$(KE_PLACE)))

.PHONY: all clean test
.DEFAULT_GOAL := all

all: kedit

test: kedit
	mkdir -p build/tests
	cc -std=c99 -Wall -Wextra -Werror -DKE_HOST_TEST -Isrc \
		tests/test_store.c src/ke_store.c src/ke_history.c src/ke_text.c \
		src/ke_file.c \
		-o build/tests/test_store
	build/tests/test_store
	python3 -m unittest discover -s tests -p 'test_*.py' -v

clean:
	rm -rf build

endif
