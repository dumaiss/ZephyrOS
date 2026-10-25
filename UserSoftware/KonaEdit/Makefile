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

$(eval $(call exe_program,konaedit,src,main.c ke_store.c ke_platform.c ke_history.c ke_text.c ke_file.c ke_widgets.c ke_view.c ke_ui.c,STACK=1024,LIBS=tui))

.PHONY: all clean test
.DEFAULT_GOAL := all

all: konaedit

test: konaedit
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
