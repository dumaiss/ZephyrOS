# SC2Kos for KonaOS.  The first build unpacks the standalone application SDK
# supplied by the sibling ZephyrOS checkout, then re-enters this makefile.

KOS_SDK_ARCHIVE ?= ../../../../ZephyrOS/build/kos-sdk-0.1.0.tar.gz
KOS_SDK_ROOT ?= build/kos-sdk-0.1.0
KOS_SDK_HASH := $(shell sha256sum "$(KOS_SDK_ARCHIVE)" 2>/dev/null | cut -c1-64)
KOS_SDK_STORED_HASH := $(shell sed -n '1p' build/.kos-sdk-sha256 2>/dev/null)
KOS_SDK_READY := $(and $(wildcard $(KOS_SDK_ROOT)/mk/kos.mk),$(filter $(KOS_SDK_HASH),$(KOS_SDK_STORED_HASH)))

ifeq ($(KOS_SDK_READY),)

.PHONY: all bootstrap clean test demo
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

demo: bootstrap
	$(MAKE) --no-print-directory KOS_SDK_ROOT="$(abspath $(KOS_SDK_ROOT))" demo

clean:
	rm -rf build

else

include $(KOS_SDK_ROOT)/mk/kos.mk

$(eval $(call exe_program,sc2kos,.,sc2kos.c sheet.c storage.c,STACK=1024))

.PHONY: all clean test demo
.DEFAULT_GOAL := all

all: sc2kos

test:
	mkdir -p build/tests
	$(CC) -std=c99 -Wall -Wextra -Werror -Itests/include -I. \
		tests/test_sheet_host.c sheet.c storage.c -o build/tests/test_sheet
	build/tests/test_sheet

demo:
	mkdir -p build/tools examples
	$(CC) -std=c99 -Wall -Wextra -Werror -Itests/include -I. \
		tools/generate_planning_model.c sheet.c storage.c \
		-o build/tools/generate_planning_model
	build/tools/generate_planning_model examples/planning_model.ksc

clean:
	rm -rf build

endif
