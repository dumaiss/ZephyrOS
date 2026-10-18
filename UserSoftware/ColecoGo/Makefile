# ColecoGo for KonaOS.  The first build unpacks the standalone SDK supplied
# with ZephyrOS, then re-enters this makefile with the SDK available.

KOS_SDK_ARCHIVE ?= ../../../../ZephyrOS/kos-sdk-0.1.0.tar.gz
KOS_SDK_ROOT ?= build/kos-sdk-0.1.0

ifeq ($(wildcard $(KOS_SDK_ROOT)/mk/kos.mk),)

.PHONY: all bootstrap test clean
.DEFAULT_GOAL := all

all: bootstrap

bootstrap:
	@test -f "$(KOS_SDK_ARCHIVE)" || { \
		echo "missing KonaOS SDK: $(KOS_SDK_ARCHIVE)"; exit 1; \
	}
	mkdir -p build
	tar -xzf "$(KOS_SDK_ARCHIVE)" -C build
	$(MAKE) --no-print-directory KOS_SDK_ROOT="$(abspath $(KOS_SDK_ROOT))" all

test: bootstrap
	$(MAKE) --no-print-directory KOS_SDK_ROOT="$(abspath $(KOS_SDK_ROOT))" test

clean:
	rm -rf build

else

include $(KOS_SDK_ROOT)/mk/kos.mk

$(eval $(call exe_program,colecogo,.,colecogo.c handoff.asm,\
	STACK=512,INCLUDES=$(KOS_SDK_ROOT)/include/kos))

.PHONY: all test clean
.DEFAULT_GOAL := all

all: colecogo

test: colecogo
	python3 -m unittest discover -s tests -v

clean:
	rm -rf build

endif
