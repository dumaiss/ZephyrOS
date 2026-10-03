# ZephyrOS build (M0)
#
# Freestanding SDCC/sdasz80/sdldz80 build.  No hosted CRT and no standard
# library are linked; the tiny C runtime initialization ZephyrOS needs is
# implemented in boot/start.asm.
#
#   make          build the ROM images under build/
#   make test     run host-side image/layout tests and integrity checks
#   make clean    remove all generated files

SDCC      ?= sdcc
SDASZ80   ?= sdasz80
SDLDZ80   ?= sdldz80
PYTHON    ?= python3

BUILD     := build

ASFLAGS   := -Iplatform
CFLAGS    := -mz80 -c --nostdinc --Werror -I include -I platform

ASM_OBJS  := $(BUILD)/reset.rel $(BUILD)/start.rel $(BUILD)/banking.rel \
             $(BUILD)/page_zero.rel $(BUILD)/syscall_entry.rel
C_OBJS    := $(BUILD)/early_console.rel $(BUILD)/panic.rel $(BUILD)/main.rel \
             $(BUILD)/syscall.rel
OBJS      := $(ASM_OBJS) $(C_OBJS)

# Target-side syscall self-test (extra object + compile define, distinct
# artifact names so the images cannot be mixed up when flashing).
ifeq ($(ZOS_SELFTEST),1)
CFLAGS    += -DZOS_SYSCALL_SELFTEST=1
SELFTEST_OBJS := $(BUILD)/selftest.rel
PK_SUFFIX := -selftest
else
SELFTEST_OBJS :=
PK_SUFFIX :=
endif

IMAGE := $(BUILD)/zephyros$(PK_SUFFIX).bin
MAME_IMAGE := $(BUILD)/zephyr80$(PK_SUFFIX).bin

.PHONY: all test check check-fast clean syscall-selftest smoke-hardware smoke-hardware-selftest

all: $(IMAGE) $(MAME_IMAGE)

$(BUILD):
	mkdir -p $@

# --- assemble ----------------------------------------------------------------

$(BUILD)/reset.rel: platform/zephyr80/reset.asm \
		platform/zephyr80/hardware.inc platform/zephyr80/memory.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/start.rel: boot/start.asm \
		platform/zephyr80/hardware.inc platform/zephyr80/memory.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/banking.rel: platform/zephyr80/banking.asm \
		platform/zephyr80/hardware.inc platform/zephyr80/memory.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/page_zero.rel: platform/zephyr80/page_zero.asm | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/syscall_entry.rel: platform/zephyr80/syscall_entry.asm \
		platform/zephyr80/hardware.inc platform/zephyr80/memory.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/selftest.rel: platform/zephyr80/selftest.asm \
		platform/zephyr80/hardware.inc platform/zephyr80/memory.inc \
		platform/zephyr80/selftest.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

# --- compile -----------------------------------------------------------------
# early_console.c and panic.c are compiled into the _COMMON segment so the
# whole early-console/panic path stays resident in the permanently mapped
# common region (E000h-FEFFh).

$(BUILD)/early_console.rel: kernel/early_console.c \
		include/zephyros/kernel.h include/zephyros/platform.h \
		include/zephyros/types.h platform/zephyr80/hardware.h | $(BUILD)
	$(SDCC) $(CFLAGS) --codeseg COMMON $< -o $@

$(BUILD)/panic.rel: kernel/panic.c \
		include/zephyros/kernel.h include/zephyros/platform.h \
		include/zephyros/types.h platform/zephyr80/hardware.h | $(BUILD)
	$(SDCC) $(CFLAGS) --codeseg COMMON $< -o $@

$(BUILD)/main.rel: kernel/main.c include/zephyros/kernel.h \
		include/zephyros/syscall.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/syscall.rel: kernel/syscall.c include/zephyros/syscall.h \
		include/zephyros/errno.h include/zephyros/kernel.h \
		include/zephyros/selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# --- link --------------------------------------------------------------------

ifeq ($(ZOS_SELFTEST),1)
# Self-test build: generated linker file with the extra object.
$(BUILD)/zephyr80.lk: $(OBJS) $(SELFTEST_OBJS) | $(BUILD)
	{ \
	  printf '%s\n' '-n' '-m' '-w'; \
	  printf '%s\n' '-i $(BUILD)/zephyros.ihx'; \
	  printf '%s\n' '-b _CODE = 0x2000' '-b _DATA = 0xC000' \
	                '-b _COMMON = 0xE000'; \
	  printf '%s\n' $(OBJS) $(SELFTEST_OBJS); \
	  printf '%s\n' '-e'; \
	} > $@

$(BUILD)/zephyros.ihx $(BUILD)/zephyros.map: $(OBJS) $(SELFTEST_OBJS) \
		$(BUILD)/zephyr80.lk | $(BUILD)
	$(SDLDZ80) -f $(BUILD)/zephyr80.lk
else
$(BUILD)/zephyros.ihx $(BUILD)/zephyros.map: $(OBJS) linker/zephyr80.lk | $(BUILD)
	$(SDLDZ80) -f linker/zephyr80.lk
endif

# --- pack and validate -------------------------------------------------------
# tools/pack_image.py validates the memory layout, builds the two 64 KiB ROM
# pages, emits the 128 KiB images and a machine-readable manifest.
# tools/mapcheck.py is the authoritative post-link layout check and runs
# automatically on every build, before the ROM is considered complete.

$(IMAGE): $(BUILD)/zephyros.ihx $(BUILD)/zephyros.map \
		tools/pack_image.py tools/mapcheck.py platform/zephyr80/memory.inc
	$(PYTHON) tools/mapcheck.py $(BUILD)/zephyros.map platform/zephyr80/memory.inc
	$(PYTHON) tools/pack_image.py $(BUILD)/zephyros.ihx $(BUILD)/zephyros.map \
		$(BUILD) $(PK_SUFFIX)

$(MAME_IMAGE): $(IMAGE)
	cp $< $@

# --- test --------------------------------------------------------------------

test: all
	$(PYTHON) -m unittest discover -s tests -v
	sha256sum $(IMAGE) $(MAME_IMAGE)
	cmp $(IMAGE) $(MAME_IMAGE)
	@echo "make test: OK"

# Map/layout validation: region ownership, gate placement, stack reservation.
# `make check` proves a clean rebuild reproduces the exact binary, then runs
# the full host validation stack (unit tests, map check, image check).
check:
	@$(MAKE) >/dev/null
	@before=$$(sha256sum $(IMAGE) | cut -d' ' -f1); \
	 $(MAKE) clean >/dev/null; \
	 $(MAKE) >/dev/null; \
	 after=$$(sha256sum $(IMAGE) | cut -d' ' -f1); \
	 if [ "$$before" != "$$after" ]; then \
	   echo "check: FAIL - rebuild is not reproducible ($$before != $$after)"; \
	   exit 1; \
	 fi; \
	 echo "check: reproducible rebuild ($$after)"
	@$(MAKE) check-fast

check-fast:
	$(PYTHON) -m unittest discover -s tests -v
	$(PYTHON) tools/mapcheck.py $(BUILD)/zephyros.map platform/zephyr80/memory.inc
	$(PYTHON) tools/pack_image.py $(BUILD)/zephyros.ihx $(BUILD)/zephyros.map $(BUILD) $(PK_SUFFIX)
	sha256sum $(IMAGE) $(MAME_IMAGE)
	cmp $(IMAGE) $(MAME_IMAGE)
	@echo "make check: OK"

clean:
	rm -rf $(BUILD) build-selftest

# --- syscall self-test image (target-side RST 30h round trip) ----------------

syscall-selftest:
	$(MAKE) BUILD=build-selftest ZOS_SELFTEST=1 \
		build-selftest/zephyros-selftest.bin build-selftest/zephyr80-selftest.bin
	$(PYTHON) tools/mapcheck.py build-selftest/zephyros.map platform/zephyr80/memory.inc
	@echo "syscall-selftest image: build-selftest/zephyros-selftest.bin"

# --- physical-hardware serial smoke checks ----------------------------------
# Requires the board connected at 115200 8N1 on SIO0/B and the matching ROM
# programmed.  Pass SERIAL_PORT=/dev/ttyUSB0 (or export ZOS_SERIAL_PORT).

smoke-hardware:
	$(PYTHON) tools/serial_smoke.py --mode normal --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-selftest:
	$(PYTHON) tools/serial_smoke.py --mode selftest --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))
