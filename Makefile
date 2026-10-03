# ZephyrOS build (M3)
#
# Freestanding SDCC/sdasz80/sdldz80 build.  No hosted CRT and no standard
# library are linked; the tiny C runtime initialization ZephyrOS needs is
# implemented in boot/start.asm.  The two SDCC indirect-call helpers the
# freestanding build needs for device ops tables are provided by
# platform/zephyr80/sdcc_helpers.asm.
#
#   make          build the ROM images under build/
#   make test     run host-side image/layout tests and integrity checks
#   make check    reproducible rebuild + full host validation stack
#   make m3-selftest     build the M3 acceptance image (all self-tests)
#   make m3-fallback     build the disabled-drivers/binding fallback images
#   make clean    remove all generated files

SDCC      ?= sdcc
SDASZ80   ?= sdasz80
SDLDZ80   ?= sdldz80
PYTHON    ?= python3

BUILD     := build

ASFLAGS   := -Iplatform
CFLAGS    := -mz80 -c --nostdinc --Werror -I include -I platform

ASM_OBJS  := $(BUILD)/reset.rel $(BUILD)/start.rel $(BUILD)/banking.rel \
             $(BUILD)/page_zero.rel $(BUILD)/syscall_entry.rel \
             $(BUILD)/irq_entry.rel $(BUILD)/irq_vectors.rel \
             $(BUILD)/platform_irq.rel $(BUILD)/sdcc_helpers.rel \
             $(BUILD)/sio_io.rel
C_OBJS    := $(BUILD)/early_console.rel $(BUILD)/panic.rel $(BUILD)/main.rel \
             $(BUILD)/syscall.rel $(BUILD)/irq.rel $(BUILD)/irq_sources.rel \
             $(BUILD)/registry.rel $(BUILD)/driver.rel $(BUILD)/console.rel \
             $(BUILD)/sio.rel $(BUILD)/sio_driver.rel
END_OBJ   := $(BUILD)/irq_code_end.rel
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

ifeq ($(ZOS_IRQ_SELFTEST),1)
CFLAGS    += -DZOS_IRQ_SELFTEST=1
IRQ_SELFTEST_OBJS := $(BUILD)/irq_selftest_asm.rel $(BUILD)/irq_selftest_c.rel
PK_SUFFIX := -irq-selftest
else
IRQ_SELFTEST_OBJS :=
endif

ifeq ($(ZOS_M3_SELFTEST),1)
CFLAGS    += -DZOS_M3_SELFTEST=1
M3_SELFTEST_OBJS := $(BUILD)/m3_selftest.rel $(BUILD)/irq_probe.rel \
                    $(BUILD)/irq_probe_io.rel $(BUILD)/fixtures.rel
PK_SUFFIX := -m3-selftest
else
M3_SELFTEST_OBJS :=
endif

# M3 fallback build options: deliberately disable the device layer to
# prove that early boot/panic diagnostics do not depend on it.
ifneq ($(ZOS_M3_NO_STATIC_DRIVERS),)
CFLAGS    += -DZOS_M3_NO_STATIC_DRIVERS=1
endif
ifneq ($(ZOS_M3_NO_CONSOLE_BIND),)
CFLAGS    += -DZOS_M3_NO_CONSOLE_BIND=1
endif

IMAGE := $(BUILD)/zephyros$(PK_SUFFIX).bin
MAME_IMAGE := $(BUILD)/zephyr80$(PK_SUFFIX).bin

.PHONY: all test check check-fast clean syscall-selftest irq-selftest \
	m3-selftest m3-fallback smoke-hardware smoke-hardware-selftest \
	smoke-hardware-irq smoke-hardware-m3

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

$(BUILD)/irq_entry.rel: kernel/irq/entry.asm platform/zephyr80/hardware.inc \
		platform/zephyr80/memory.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/irq_vectors.rel: kernel/irq/vectors.asm \
		platform/zephyr80/memory.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/platform_irq.rel: platform/zephyr80/irq.asm \
		platform/zephyr80/hardware.inc platform/zephyr80/memory.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/sdcc_helpers.rel: platform/zephyr80/sdcc_helpers.asm | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/sio_io.rel: drivers/sio/sio_io.asm \
		platform/zephyr80/hardware.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/irq_probe_io.rel: drivers/test/irq_probe_io.asm | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/irq_code_end.rel: kernel/irq/code_end.asm | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/selftest.rel: platform/zephyr80/selftest.asm \
		platform/zephyr80/hardware.inc platform/zephyr80/memory.inc \
		platform/zephyr80/selftest.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/irq_selftest_asm.rel: platform/zephyr80/irq_selftest.asm \
		platform/zephyr80/hardware.inc platform/zephyr80/memory.inc \
		platform/zephyr80/irq_selftest.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/irq_selftest_c.rel: kernel/irq/irq_selftest.c \
		include/zephyros/irq.h include/zephyros/irq_selftest.h \
		include/zephyros/kernel.h include/zephyros/platform.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

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
		include/zephyros/syscall.h include/zephyros/irq.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/irq.rel: kernel/irq/irq.c include/zephyros/irq.h \
		include/zephyros/types.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/irq_sources.rel: platform/zephyr80/irq_sources.c \
		include/zephyros/irq.h include/zephyros/platform.h \
		platform/zephyr80/hardware.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/syscall.rel: kernel/syscall.c include/zephyros/syscall.h \
		include/zephyros/errno.h include/zephyros/kernel.h \
		include/zephyros/selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# --- M3 device/driver/service objects (bank-7 code and data) -----------------

$(BUILD)/registry.rel: kernel/device/registry.c include/zephyros/device.h \
		include/zephyros/irq.h include/zephyros/errno.h \
		include/zephyros/types.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/driver.rel: kernel/device/driver.c include/zephyros/driver.h \
		include/zephyros/device.h include/zephyros/irq.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/console.rel: kernel/device/console.c include/zephyros/console.h \
		include/zephyros/device.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/sio.rel: drivers/sio/sio.c drivers/sio/sio.h \
		include/zephyros/device.h include/zephyros/platform.h | $(BUILD)
	$(SDCC) $(CFLAGS) -I drivers/sio $< -o $@

$(BUILD)/sio_driver.rel: drivers/sio/driver.c drivers/sio/sio.h \
		include/zephyros/driver.h | $(BUILD)
	$(SDCC) $(CFLAGS) -I drivers/sio $< -o $@

# --- M3 self-test objects (ZOS_M3_SELFTEST builds only) ----------------------

$(BUILD)/m3_selftest.rel: kernel/device/m3_selftest.c \
		include/zephyros/console.h include/zephyros/driver.h \
		include/zephyros/kernel.h include/zephyros/m3_selftest.h \
		include/zephyros/platform.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/irq_probe.rel: drivers/test/irq_probe.c include/zephyros/driver.h \
		include/zephyros/platform.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/fixtures.rel: drivers/test/fixtures.c include/zephyros/driver.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# --- link --------------------------------------------------------------------
# The canonical `build/` output uses the checked-in linker/zephyr80.lk.
# Every self-test and fallback build (different BUILD dir and/or extra
# objects) generates its linker file from the same object list the link
# step depends on, so the lists cannot drift apart.

ZOS_SELFTEST_BUILD := $(filter 1,$(ZOS_SELFTEST) $(ZOS_IRQ_SELFTEST) $(ZOS_M3_SELFTEST))
ifeq ($(BUILD),build)
ifneq ($(ZOS_SELFTEST_BUILD),)
GENERATED_LK := 1
endif
else
GENERATED_LK := 1
endif

ifeq ($(GENERATED_LK),1)
$(BUILD)/zephyr80.lk: $(OBJS) $(SELFTEST_OBJS) $(IRQ_SELFTEST_OBJS) \
		$(M3_SELFTEST_OBJS) $(END_OBJ) | $(BUILD)
	{ \
	  printf '%s\n' '-n' '-m' '-w'; \
	  printf '%s\n' '-i $(BUILD)/zephyros.ihx'; \
	  printf '%s\n' '-b _CODE = 0x2000' '-b _DATA = 0xC000' \
	                '-b _COMMON = 0xE000'; \
	  printf '%s\n' $(OBJS) $(SELFTEST_OBJS) $(IRQ_SELFTEST_OBJS) \
	                $(M3_SELFTEST_OBJS) $(END_OBJ); \
	  printf '%s\n' '-e'; \
	} > $@

$(BUILD)/zephyros.ihx $(BUILD)/zephyros.map: $(OBJS) $(SELFTEST_OBJS) \
		$(IRQ_SELFTEST_OBJS) $(M3_SELFTEST_OBJS) $(END_OBJ) \
		$(BUILD)/zephyr80.lk | $(BUILD)
	$(SDLDZ80) -f $(BUILD)/zephyr80.lk
else
$(BUILD)/zephyros.ihx $(BUILD)/zephyros.map: $(OBJS) $(END_OBJ) linker/zephyr80.lk | $(BUILD)
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
	rm -rf $(BUILD) build-selftest build-irq-selftest build-m3-selftest \
		build-m3-nodrv build-m3-nobind

# --- syscall self-test image (target-side RST 30h round trip) ----------------

syscall-selftest:
	$(MAKE) BUILD=build-selftest ZOS_SELFTEST=1 \
		build-selftest/zephyros-selftest.bin build-selftest/zephyr80-selftest.bin
	$(PYTHON) tools/mapcheck.py build-selftest/zephyros.map platform/zephyr80/memory.inc
	@echo "syscall-selftest image: build-selftest/zephyros-selftest.bin"

irq-selftest:
	$(MAKE) BUILD=build-irq-selftest ZOS_SELFTEST=1 ZOS_IRQ_SELFTEST=1 \
		build-irq-selftest/zephyros-irq-selftest.bin \
		build-irq-selftest/zephyr80-irq-selftest.bin
	$(PYTHON) tools/mapcheck.py build-irq-selftest/zephyros.map platform/zephyr80/memory.inc
	@echo "irq-selftest image: build-irq-selftest/zephyros-irq-selftest.bin"

# --- M3 acceptance image (syscall + irq + device/console/IRQ driver) ---------

m3-selftest:
	$(MAKE) BUILD=build-m3-selftest ZOS_SELFTEST=1 ZOS_IRQ_SELFTEST=1 \
		ZOS_M3_SELFTEST=1 \
		build-m3-selftest/zephyros-m3-selftest.bin \
		build-m3-selftest/zephyr80-m3-selftest.bin
	$(PYTHON) tools/mapcheck.py build-m3-selftest/zephyros.map platform/zephyr80/memory.inc
	@echo "m3-selftest image: build-m3-selftest/zephyros-m3-selftest.bin"

# --- M3 fallback images (device layer deliberately disabled) -----------------

m3-fallback:
	$(MAKE) BUILD=build-m3-nodrv ZOS_M3_NO_STATIC_DRIVERS=1 \
		build-m3-nodrv/zephyros.bin
	$(PYTHON) tools/mapcheck.py build-m3-nodrv/zephyros.map platform/zephyr80/memory.inc
	$(MAKE) BUILD=build-m3-nobind ZOS_M3_NO_CONSOLE_BIND=1 \
		build-m3-nobind/zephyros.bin
	$(PYTHON) tools/mapcheck.py build-m3-nobind/zephyros.map platform/zephyr80/memory.inc
	@echo "m3-fallback images: build-m3-nodrv/zephyros.bin build-m3-nobind/zephyros.bin"

# --- physical-hardware serial smoke checks ----------------------------------
# Requires the board connected at 115200 8N1 on SIO0/B and the matching ROM
# programmed.  Pass SERIAL_PORT=/dev/ttyUSB0 (or export ZOS_SERIAL_PORT).

smoke-hardware:
	$(PYTHON) tools/serial_smoke.py --mode normal --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-selftest:
	$(PYTHON) tools/serial_smoke.py --mode selftest --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-irq:
	$(PYTHON) tools/serial_smoke.py --mode irq --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m3:
	$(PYTHON) tools/serial_smoke.py --mode m3 --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))
