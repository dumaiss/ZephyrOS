# ZephyrOS build (M5)
#
# Freestanding SDCC/sdasz80/sdldz80 build.  No hosted CRT and no standard
# library are linked; the tiny C runtime initialization ZephyrOS needs is
# implemented in boot/start.asm.  The two SDCC indirect-call helpers the
# freestanding build needs for device ops tables are provided by
# kernel/runtime/sdcc_helpers.asm.
#
# Platform build configuration (include paths and declarative constant
# files) comes from platform/zephyr80/config.mk.
#
# The SIO driver is not linked into the kernel: it is packed as a ZMOD
# module (build/modules/sio.zmod), stored in the ROMFS recovery image
# (build/recovery.romfs, pages 2-7 of the 512 KiB ROM), and loaded at boot
# through the generic M4 loader and its bounded read-source adapter.
# Pre-M5 acceptance images keep an embedded test catalog instead.
#
#   make          build the ROM images under build/
#   make test     run host-side image/layout tests and integrity checks
#   make check    reproducible rebuild + full host validation stack
#   make romfs-test           ROMFS image + host ROMFS tool tests
#   make m3-selftest          build the M3 acceptance image
#   make m4-selftest          build the M4 dynamic-SIO acceptance image
#   make m4-placement-selftest  same module bytes at a shifted arena base
#   make m4-failure-selftest  deliberately failing test module + rollback
#   make m5-selftest          M5 ROMFS success acceptance image
#   make m5-corrupt-selftest  M5 corrupt-recovery-image failure image
#   make clean    remove all generated files

SDCC      ?= sdcc
SDASZ80   ?= sdasz80
SDLDZ80   ?= sdldz80
PYTHON    ?= python3

BUILD     := build
.DEFAULT_GOAL := all

# Zephyr-80 platform build configuration (include paths and the
# declarative constant files consumed by the build and layout tools).
include platform/zephyr80/config.mk

ASFLAGS   := $(ZOS_PLATFORM_ASFLAGS) -Itests/target/include
CFLAGS    := -mz80 -c --nostdinc --Werror $(ZOS_PLATFORM_CFLAGS)

ASM_OBJS  := $(BUILD)/reset.rel $(BUILD)/start.rel $(BUILD)/banking.rel \
             $(BUILD)/page_zero.rel $(BUILD)/syscall_entry.rel \
             $(BUILD)/irq_entry.rel $(BUILD)/irq_vectors.rel \
             $(BUILD)/platform_irq.rel $(BUILD)/sdcc_helpers.rel \
             $(BUILD)/arena.rel $(BUILD)/rom_access.rel \
             $(BUILD)/process_launch.rel $(BUILD)/user_copy.rel \
             $(BUILD)/diag_io.rel
C_OBJS    := $(BUILD)/early_console.rel $(BUILD)/panic.rel $(BUILD)/main.rel \
             $(BUILD)/syscall.rel $(BUILD)/diag.rel $(BUILD)/irq.rel \
             $(BUILD)/irq_sources.rel $(BUILD)/registry.rel \
             $(BUILD)/console.rel \
             $(BUILD)/module_relocate.rel $(BUILD)/module_loader.rel \
             $(BUILD)/romfs.rel $(BUILD)/process.rel $(BUILD)/process_exec.rel
END_OBJ   := $(BUILD)/irq_code_end.rel
# OBJS is assigned after the acceptance/catalog selection below, once
# every conditional object append has been decided.

# --- target acceptance build selection ---------------------------------
# Acceptance images link the shared runner (tests/target/support/) plus
# the per-milestone test code under tests/target/; production images link
# none of it.  ZOS_TARGET_TEST_BUILD is the single production-side guard
# (kernel/main.c holds only the two TEST-ONLY HOOKs).
TARGET_TEST_OBJS :=
PK_SUFFIX :=
# Target-side syscall acceptance (M1); distinct artifact names so the
# images cannot be mixed up when flashing.
ifeq ($(ZOS_SELFTEST),1)
CFLAGS    += -DZOS_SYSCALL_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/syscall_test.rel $(BUILD)/selftest_asm.rel
PK_SUFFIX := -selftest
endif

ifeq ($(ZOS_IRQ_SELFTEST),1)
CFLAGS    += -DZOS_IRQ_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/irq_test.rel $(BUILD)/irq_selftest_asm.rel
PK_SUFFIX := -irq-selftest
endif

ifeq ($(ZOS_M3_SELFTEST),1)
CFLAGS    += -DZOS_M3_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/m3_test.rel $(BUILD)/m3_irq_probe.rel \
                    $(BUILD)/m3_probe_io.rel $(BUILD)/m3_fixtures.rel
PK_SUFFIX := -m3-selftest
endif

# M4 self-test / acceptance build options.  The boot catalog always carries
# the packed SIO module; these options select which variant is embedded and
# which target-side acceptance code is compiled.
ifeq ($(ZOS_M4_SELFTEST),1)
CFLAGS    += -DZOS_M4_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/m4_test.rel $(BUILD)/m4_fixture_catalog.rel \
                    $(BUILD)/m4_fixture_blobs.rel
PK_SUFFIX := -m4-loader-selftest
endif

# M4 acceptance image: IRQ self-test variant of the SIO module; the kernel
# keeps CTC0 live while the test module init claims/counts/releases it.
ifeq ($(ZOS_M4_SIO_IRQ),1)
CFLAGS    += -DZOS_M4_SIO_IRQ=1
PK_SUFFIX := -m4-selftest
endif

# M4 failure image: init-failing SIO test module + rollback verification.
ifeq ($(ZOS_M4_FAILURE_SELFTEST),1)
CFLAGS    += -DZOS_M4_FAILURE_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/m4_test.rel
PK_SUFFIX := -m4-failure
endif

# M4 placement image: shift the linker-defined arena base by whole 512-byte
# granules (the packed module bytes are untouched: same .zmod files).
ifeq ($(ZOS_M4_PLACEMENT),1)
ZOS_M4_ARENA_BASE ?= 0x9E00
ZOS_M4_ARENA_END := $(shell printf '0x%04X' $$(( $(ZOS_M4_ARENA_BASE) + 0x2000 )))
PK_SUFFIX := -m4-placement-selftest
endif

# M5 success image: boot the SIO module from the ROMFS recovery store and
# run the ROMFS acceptance selftest (first/last/EOF/cross-page reads).
ifeq ($(ZOS_M5_SELFTEST),1)
CFLAGS    += -DZOS_M5_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/m5_test.rel
PK_SUFFIX := -m5-selftest
endif

# M5 corrupt image: same recovery store with deliberately corrupted header
# metadata; the kernel must reject it through the early-console panic path.
ifeq ($(ZOS_M5_CORRUPT_SELFTEST),1)
CFLAGS    += -DZOS_M5_CORRUPT_SELFTEST=1
PK_SUFFIX := -m5-corrupt
endif

# M6 success image: boots the SIO module from ROMFS, runs zextest from
# ROMFS in two application banks with repeated cycles, checks exact
# parent restoration, and verifies rejection of every malformed ZEX
# fixture by exact result code.
ifeq ($(ZOS_M6_SELFTEST),1)
CFLAGS    += -DZOS_M6_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/m6_test.rel
PK_SUFFIX := -m6-selftest
endif

# M6 failure image: the ROMFS "zextest.zex" entry is a deliberately
# truncated ZEX; the load must roll back exactly, and the valid copy
# under "zexok.zex" must still run afterwards.
ifeq ($(ZOS_M6_FAILURE_SELFTEST),1)
CFLAGS    += -DZOS_M6_FAILURE_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/m6_test.rel
PK_SUFFIX := -m6-failure
endif

# M7 success image: boots the SIO module and the native monitor from
# ROMFS, runs the kernel-side diagnostic selftest, then hands control to
# the temporary launcher for the scripted interactive acceptance.
ifeq ($(ZOS_M7_SELFTEST),1)
CFLAGS    += -DZOS_M7_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/m7_test.rel
PK_SUFFIX := -m7-selftest
endif

# Temporary pre-shell monitor launcher (kernel/main.c).  Production and
# the M7 image enable it; every earlier acceptance image disables it so
# its accepted behavior is preserved byte-for-byte where possible.
ifneq ($(filter 1,$(ZOS_SELFTEST) $(ZOS_IRQ_SELFTEST) $(ZOS_M3_SELFTEST) \
	$(ZOS_M4_SELFTEST) $(ZOS_M4_SIO_IRQ) $(ZOS_M4_FAILURE_SELFTEST) \
	$(ZOS_M4_PLACEMENT) $(ZOS_M5_SELFTEST) $(ZOS_M5_CORRUPT_SELFTEST) \
	$(ZOS_M6_SELFTEST) $(ZOS_M6_FAILURE_SELFTEST)),)
ZOS_MONITOR_BOOT := 0
endif
ZOS_MONITOR_BOOT ?= 1
CFLAGS    += -DZOS_MONITOR_BOOT=$(ZOS_MONITOR_BOOT)

# Boot-module source selection.  Pre-M5 acceptance images (M1-M4) keep
# their embedded test catalogs; production and the M5 images boot the SIO
# module from the ROMFS recovery store.
ifneq ($(filter 1,$(ZOS_SELFTEST) $(ZOS_IRQ_SELFTEST) $(ZOS_M3_SELFTEST) \
	$(ZOS_M4_SELFTEST) $(ZOS_M4_SIO_IRQ) $(ZOS_M4_FAILURE_SELFTEST) \
	$(ZOS_M4_PLACEMENT)),)
ZOS_ROMFS_BOOT := 0
endif
ZOS_ROMFS_BOOT ?= 1
CFLAGS    += -DZOS_ROMFS_BOOT=$(ZOS_ROMFS_BOOT)
ifeq ($(ZOS_ROMFS_BOOT),0)
# Embedded boot catalog (kernel/module/embedded.c + the generated blob
# assembly): acceptance images only.  --production mapcheck rejects the
# symbols whenever they leak into a production image.
ASM_OBJS  += $(BUILD)/m4_catalog_blobs.rel
C_OBJS    += $(BUILD)/module_embedded.rel
endif

OBJS      := $(ASM_OBJS) $(C_OBJS)

# --- M8 driver-arena geometry selection --------------------------------------
# Production (no acceptance flags) derives the arena from the aligned end
# of the linked kernel with a two-pass link.  Every pre-M8 acceptance
# image and the shifted-placement variant keeps the legacy fixed
# geometry; their transcripts and checks are preserved unchanged.
ZOS_ACCEPTANCE_BUILD := $(filter 1,$(ZOS_SELFTEST) $(ZOS_IRQ_SELFTEST) \
	$(ZOS_M3_SELFTEST) $(ZOS_M4_SELFTEST) $(ZOS_M4_SIO_IRQ) \
	$(ZOS_M4_FAILURE_SELFTEST) $(ZOS_M4_PLACEMENT) $(ZOS_M5_SELFTEST) \
	$(ZOS_M5_CORRUPT_SELFTEST) $(ZOS_M6_SELFTEST) \
	$(ZOS_M6_FAILURE_SELFTEST) $(ZOS_M7_SELFTEST))
ZOS_ARENA_DERIVED ?= $(if $(ZOS_ACCEPTANCE_BUILD),0,1)

# Common acceptance wiring: the single guard plus the shared runner.
ifneq ($(filter 1,$(ZOS_SELFTEST) $(ZOS_IRQ_SELFTEST) $(ZOS_M3_SELFTEST) \
	$(ZOS_M4_SELFTEST) $(ZOS_M4_SIO_IRQ) $(ZOS_M4_FAILURE_SELFTEST) \
	$(ZOS_M5_SELFTEST) $(ZOS_M6_SELFTEST) \
	$(ZOS_M6_FAILURE_SELFTEST) $(ZOS_M7_SELFTEST)),)
CFLAGS    += -DZOS_TARGET_TEST_BUILD=1 -Itests/target/include
TARGET_TEST_OBJS := $(BUILD)/target_test.rel $(TARGET_TEST_OBJS)
endif

# Production images (no acceptance objects) must reject every acceptance
# symbol in the linked map; mapcheck --production enforces this.
MAPCHECK_FLAGS := $(if $(TARGET_TEST_OBJS),,--production)

# Per-image boot module selection (identical bytes across placement builds).
ifeq ($(ZOS_M4_SIO_IRQ),1)
BOOT_ZMOD := build/modules/sio-irq.zmod
else ifeq ($(ZOS_M4_FAILURE_SELFTEST),1)
BOOT_ZMOD := build/modules/sio-fail.zmod
else
BOOT_ZMOD := build/modules/sio.zmod
endif


IMAGE := $(BUILD)/zephyros$(PK_SUFFIX).bin
MAME_IMAGE := $(BUILD)/zephyr80$(PK_SUFFIX).bin

# Memory-layout source for this build.  The placement image regenerates
# platform/zephyr80/memory.inc with a shifted legacy driver arena and
# includes the variant first, so every assembly unit (and mapcheck) sees
# it.  Derived (production) builds generate a full memory.inc variant from
# the probe map for the layout tools.
ifeq ($(ZOS_M4_PLACEMENT),1)
VARIANT_MEMORY := $(BUILD)/platform/zephyr80/memory.inc
ASFLAGS   := -I$(BUILD)/platform $(ZOS_PLATFORM_ASFLAGS) -Itests/target/include
LEGACY_MEMORY := $(VARIANT_MEMORY)
else
LEGACY_MEMORY := $(ZOS_PLATFORM_MEMORY_INC)
endif

GEOMETRY_DIR       := $(BUILD)/geometry
GEOMETRY_PROBE_DIR := $(BUILD)/probe
GEOMETRY_INC       := $(GEOMETRY_DIR)/arena_geometry.inc

ifeq ($(ZOS_ARENA_DERIVED),1)
GEOMETRY_MEMORY := $(GEOMETRY_DIR)/memory.inc
MEMORY_INC := $(GEOMETRY_MEMORY)
else
GEOMETRY_MEMORY :=
MEMORY_INC := $(LEGACY_MEMORY)
endif

# --- M8 arena geometry generation --------------------------------------------
# kernel/module/arena.asm always includes the generated
# arena_geometry.inc.  Legacy builds copy the fixed platform geometry.
# Derived builds first link a probe image (the same object list with a
# bootstrap arena object), measure the loadable areas in its map, then
# regenerate the include and the memory.inc variant before the real link.
# The final map is re-verified by mapcheck.py, so a stale generated value
# cannot survive.
ifeq ($(ZOS_ARENA_DERIVED),1)

$(GEOMETRY_INC) $(GEOMETRY_MEMORY): $(BUILD)/zephyros.probe.map \
		tools/mapcheck.py $(ZOS_PLATFORM_MEMORY_INC) | $(BUILD)
	mkdir -p $(GEOMETRY_DIR)
	$(PYTHON) tools/mapcheck.py geometry derive $(BUILD)/zephyros.probe.map \
		$(ZOS_PLATFORM_MEMORY_INC) $(GEOMETRY_INC) $(GEOMETRY_MEMORY)

$(GEOMETRY_PROBE_DIR)/arena_geometry.inc: $(ZOS_PLATFORM_MEMORY_INC) \
		tools/mapcheck.py | $(BUILD)
	mkdir -p $(GEOMETRY_PROBE_DIR)
	$(PYTHON) tools/mapcheck.py geometry probe $(ZOS_PLATFORM_MEMORY_INC) $@

$(GEOMETRY_PROBE_DIR)/arena.rel: kernel/module/arena.asm \
		$(GEOMETRY_PROBE_DIR)/arena_geometry.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -I$(GEOMETRY_PROBE_DIR) -o $@ $<

# Probe link: identical objects, bootstrap arena symbols.  Kept under
# build/probe/ so the canonical build/ object list stays exactly the
# committed linker file's list.
PROBE_OBJS := $(filter-out $(BUILD)/arena.rel,$(OBJS) $(END_OBJ)) \
		$(GEOMETRY_PROBE_DIR)/arena.rel

$(BUILD)/zephyros.probe.lk: $(PROBE_OBJS) | $(BUILD)
	{ \
	  printf '%s\n' '-n' '-m' '-w'; \
	  printf '%s\n' '-i $(BUILD)/zephyros.probe.ihx'; \
	  printf '%s\n' '-b _CODE = 0x2000' '-b _DATA = 0xC000' \
	                '-b _COMMON = 0xE000'; \
	  printf '%s\n' $(PROBE_OBJS); \
	  printf '%s\n' '-e'; \
	} > $@

$(BUILD)/zephyros.probe.ihx $(BUILD)/zephyros.probe.map: \
		$(BUILD)/zephyros.probe.lk
	$(SDLDZ80) -f $(BUILD)/zephyros.probe.lk

else

$(GEOMETRY_INC): $(LEGACY_MEMORY) tools/mapcheck.py | $(BUILD)
	mkdir -p $(GEOMETRY_DIR)
	$(PYTHON) tools/mapcheck.py geometry legacy $(LEGACY_MEMORY) $@

endif

# Declared boot-set arena plan (derived builds only): per-module
# image+BSS/granules and the used/free totals; the build fails here when
# the declared boot set cannot coexist in the linked arena.
ifeq ($(ZOS_ARENA_DERIVED),1)
ARENA_PLAN := $(BUILD)/arena.plan.json
$(ARENA_PLAN): $(BOOT_ZMOD) tools/zmodpack.py $(MEMORY_INC) | $(BUILD)
	$(PYTHON) tools/zmodpack.py plan --geometry $(MEMORY_INC) \
		--report $@ $(BOOT_ZMOD)
else
ARENA_PLAN :=
endif

.PHONY: all test check check-fast clean syscall-selftest irq-selftest \
	m3-selftest zmod-test m4-loader-selftest m4-selftest \
	m4-placement-selftest m4-failure-selftest romfs-test m5-selftest \
	m5-corrupt-selftest zex-test m6-selftest m6-failure-selftest \
	monitor-test m7-selftest \
	smoke-hardware \
	smoke-hardware-selftest smoke-hardware-irq smoke-hardware-m3 \
	smoke-hardware-m4 smoke-hardware-m4-failure smoke-hardware-m5 \
	smoke-hardware-m5-corrupt smoke-hardware-m6 smoke-hardware-m6-failure \
	smoke-hardware-m7

all: $(IMAGE) $(MAME_IMAGE)

$(BUILD):
	mkdir -p $@

ifeq ($(ZOS_M4_PLACEMENT),1)
$(VARIANT_MEMORY): $(ZOS_PLATFORM_MEMORY_INC) | $(BUILD)
	mkdir -p $(dir $@)
	sed -e 's|^ZOS_DRIVER_ARENA_BASE .*|ZOS_DRIVER_ARENA_BASE   = $(ZOS_M4_ARENA_BASE)|' \
	    -e 's|^ZOS_DRIVER_ARENA_END .*|ZOS_DRIVER_ARENA_END    = $(ZOS_M4_ARENA_END)|' \
	    $< > $@
endif

# --- assemble ----------------------------------------------------------------

$(BUILD)/reset.rel: boot/shadow_copy.asm \
		platform/zephyr80/hardware.inc platform/zephyr80/memory.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/start.rel: boot/start.asm \
		platform/zephyr80/hardware.inc platform/zephyr80/memory.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/banking.rel: kernel/memory/banking.asm \
		platform/zephyr80/hardware.inc platform/zephyr80/memory.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/page_zero.rel: kernel/syscall/page_zero.asm | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/syscall_entry.rel: kernel/syscall/entry.asm \
		platform/zephyr80/hardware.inc platform/zephyr80/memory.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/irq_entry.rel: kernel/irq/entry.asm platform/zephyr80/hardware.inc \
		platform/zephyr80/memory.inc platform/zephyr80/irq.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/irq_vectors.rel: kernel/irq/vectors.asm \
		platform/zephyr80/irq.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/platform_irq.rel: kernel/irq/init.asm \
		platform/zephyr80/irq.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/sdcc_helpers.rel: kernel/runtime/sdcc_helpers.asm | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/arena.rel: kernel/module/arena.asm $(GEOMETRY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -I$(dir $(GEOMETRY_INC)) -o $@ $<

$(BUILD)/rom_access.rel: kernel/romfs/rom_access.asm \
		platform/zephyr80/hardware.inc platform/zephyr80/memory.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/process_launch.rel: kernel/process/launch.asm \
		platform/zephyr80/hardware.inc platform/zephyr80/memory.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/user_copy.rel: kernel/memory/user_copy.asm \
		platform/zephyr80/hardware.inc platform/zephyr80/memory.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/diag_io.rel: kernel/syscall/diag_io.asm | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/m3_probe_io.rel: tests/target/m3/m3_probe_io.asm | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/irq_code_end.rel: kernel/irq/code_end.asm | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/selftest_asm.rel: tests/target/m1/selftest.asm \
		platform/zephyr80/hardware.inc platform/zephyr80/memory.inc \
		tests/target/include/selftest.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/irq_selftest_asm.rel: tests/target/m2/irq_selftest.asm \
		platform/zephyr80/hardware.inc platform/zephyr80/memory.inc \
		tests/target/include/irq_selftest.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/irq_test.rel: tests/target/m2/irq_test.c \
		include/zephyros/irq.h include/zephyros/kernel.h \
		include/zephyros/platform.h tests/target/include/irq_selftest.h | $(BUILD)
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
		include/zephyros/irq.h include/zephyros/console.h \
		include/zephyros/module.h include/zephyros/process.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/irq.rel: kernel/irq/irq.c include/zephyros/irq.h \
		include/zephyros/types.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/irq_sources.rel: drivers/ctc/ctc.c \
		include/zephyros/irq.h include/zephyros/platform.h \
		platform/zephyr80/hardware.h platform/zephyr80/irq.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/syscall.rel: kernel/syscall.c kernel/syscall/diag.h \
		include/zephyros/syscall.h include/zephyros/errno.h \
		include/zephyros/kernel.h include/zephyros/process.h \
		include/zephyros/console.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/diag.rel: kernel/syscall/diag.c kernel/syscall/diag.h \
		include/zephyros/syscall.h include/zephyros/process.h \
		include/zephyros/platform.h include/zephyros/errno.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# --- M3 device/driver/service objects (bank-7 code and data) -----------------

$(BUILD)/registry.rel: kernel/device/registry.c include/zephyros/device.h \
		include/zephyros/irq.h include/zephyros/errno.h \
		include/zephyros/types.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/console.rel: kernel/device/console.c include/zephyros/console.h \
		include/zephyros/device.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# --- M4 module loader objects (bank-7 code and data) -------------------------

$(BUILD)/module_relocate.rel: kernel/module/relocate.c \
		include/zephyros/module.h include/zephyros/driver.h \
		include/zephyros/errno.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/module_loader.rel: kernel/module/loader.c \
		include/zephyros/module.h include/zephyros/driver.h \
		include/zephyros/device.h include/zephyros/irq.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/module_embedded.rel: kernel/module/embedded.c \
		include/zephyros/module.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# --- M5 ROMFS objects (bank-7 reader + common ROM-access primitive) ----------

$(BUILD)/romfs.rel: kernel/romfs/romfs.c include/zephyros/romfs.h \
		include/zephyros/module.h include/zephyros/platform.h \
		platform/zephyr80/memory.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# --- M6 process lifecycle objects (bank-7 code and data) --------------------

$(BUILD)/process.rel: kernel/process/process.c \
		include/zephyros/process.h include/zephyros/zex.h \
		include/zephyros/kernel.h include/zephyros/errno.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/process_exec.rel: kernel/process/exec.c \
		include/zephyros/process.h include/zephyros/zex.h \
		include/zephyros/romfs.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# --- ZMOD modules (host-built, packed, embedded per-image catalogs) ----------
# The SIO driver module and the synthetic M4 fixtures are built once under
# build/modules/ -- shared by every image, so each variant's packed bytes
# are identical across placement builds.  Each image embeds the boot module
# plus (in self-test builds) the fixtures through a generated assembly blob
# emitted by tools/zmodpack.py.

MODULES   := build/modules
MOD_CFLAGS := -mz80 -c --nostdinc --Werror -I include -I platform -I drivers/sio

$(MODULES):
	mkdir -p $@

# --- SIO module objects (production source + test-variant entries) ---

$(MODULES)/sio.rel: drivers/sio/sio.c drivers/sio/sio.h \
		include/zephyros/device.h include/zephyros/platform.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

$(MODULES)/sio_io.rel: drivers/sio/sio_io.asm \
		platform/zephyr80/hardware.inc | $(MODULES)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(MODULES)/sio_module.rel: drivers/sio/sio_module.c drivers/sio/sio.h \
		include/zephyros/driver.h include/zephyros/device.h \
		include/zephyros/irq.h include/zephyros/errno.h \
		include/zephyros/platform.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

$(MODULES)/sio_irq_module.rel: tests/modules/sio_irq_module.c \
		drivers/sio/sio.h include/zephyros/driver.h \
		include/zephyros/device.h include/zephyros/irq.h \
		include/zephyros/errno.h include/zephyros/platform.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

$(MODULES)/sio_fail_module.rel: tests/modules/sio_fail_module.c \
		drivers/sio/sio.h include/zephyros/driver.h \
		include/zephyros/device.h include/zephyros/irq.h \
		include/zephyros/errno.h include/zephyros/platform.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

$(MODULES)/sdcc_indirect_calls.rel: modules/runtime/sdcc_indirect_calls.asm \
		| $(MODULES)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

SIO_MODULE_OBJS := $(MODULES)/sio.rel $(MODULES)/sio_io.rel \
		$(MODULES)/sio_module.rel $(MODULES)/sdcc_indirect_calls.rel

$(MODULES)/sio.zmod: modules/manifests/sio.json $(SIO_MODULE_OBJS) \
		tools/zmodpack.py
	$(PYTHON) tools/zmodpack.py pack $< $@ --objdir $(MODULES) \
		--report $(MODULES)/sio.report.json

$(MODULES)/sio-irq.zmod: tests/modules/sio-irq.json $(MODULES)/sio.rel \
		$(MODULES)/sio_io.rel $(MODULES)/sio_irq_module.rel \
		$(MODULES)/sdcc_indirect_calls.rel tools/zmodpack.py
	$(PYTHON) tools/zmodpack.py pack $< $@ --objdir $(MODULES) \
		--report $(MODULES)/sio-irq.report.json

$(MODULES)/sio-fail.zmod: tests/modules/sio-fail.json $(MODULES)/sio.rel \
		$(MODULES)/sio_io.rel $(MODULES)/sio_fail_module.rel \
		$(MODULES)/sdcc_indirect_calls.rel tools/zmodpack.py
	$(PYTHON) tools/zmodpack.py pack $< $@ --objdir $(MODULES) \
		--report $(MODULES)/sio-fail.report.json

# --- synthetic M4 fixtures ---

$(MODULES)/m4_probe.rel: tests/modules/m4_probe.c \
		include/zephyros/device.h include/zephyros/driver.h \
		include/zephyros/irq.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

$(MODULES)/m4_fail.rel: tests/modules/m4_fail.c \
		include/zephyros/device.h include/zephyros/driver.h \
		include/zephyros/irq.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

$(MODULES)/m4_filler.rel: tests/modules/m4_filler.c \
		include/zephyros/driver.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

$(MODULES)/m8_api.rel: tests/modules/m8_api.c \
		include/zephyros/device.h include/zephyros/driver.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

$(MODULES)/m8_api.zmod: tests/modules/m8_api.json $(MODULES)/m8_api.rel \
		$(MODULES)/sdcc_indirect_calls.rel tools/zmodpack.py
	$(PYTHON) tools/zmodpack.py pack $< $@ --objdir $(MODULES) \
		--report $(MODULES)/m8_api.report.json

$(MODULES)/%.zmod: tests/modules/%.json $(MODULES)/%.rel \
		$(MODULES)/sdcc_indirect_calls.rel tools/zmodpack.py
	$(PYTHON) tools/zmodpack.py pack $< $@ --objdir $(MODULES)

FIXTURE_ZMODS := $(MODULES)/m4_probe.zmod $(MODULES)/m4_fail.zmod \
		$(MODULES)/m4_filler.zmod $(MODULES)/m8_api.zmod

# --- embedded catalogs (production boot module + test fixture catalog) ---
# The production catalog (kernel/module/embedded.c) carries only the boot
# module in every image; the loader-fixture catalog is a test artifact
# (tests/target/m4/m4_fixture_catalog.c) built only for acceptance images.

CATALOG_ASM := $(BUILD)/m4_catalog.asm
CATALOG_INC := $(BUILD)/m4_catalog.inc

$(CATALOG_ASM): $(BOOT_ZMOD) tools/zmodpack.py | $(BUILD)
	$(PYTHON) tools/zmodpack.py emit-asm $@ z4_module_boot=$(BOOT_ZMOD) \
		--header $(CATALOG_INC)

$(CATALOG_INC): $(CATALOG_ASM) ;

$(BUILD)/m4_catalog_blobs.rel: $(CATALOG_ASM) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

# embedded.c includes the generated catalog header in every configuration.
$(BUILD)/module_embedded.rel: $(CATALOG_INC)

FIXTURE_CATALOG_ASM := $(BUILD)/m4_fixture_blobs.asm
FIXTURE_CATALOG_INC := $(BUILD)/m4_fixture_blobs.inc

$(FIXTURE_CATALOG_ASM): $(FIXTURE_ZMODS) tools/zmodpack.py | $(BUILD)
	$(PYTHON) tools/zmodpack.py emit-asm $@ \
		z4_fixture_probe=$(MODULES)/m4_probe.zmod \
		z4_fixture_fail=$(MODULES)/m4_fail.zmod \
		z4_fixture_filler=$(MODULES)/m4_filler.zmod \
		z4_fixture_api=$(MODULES)/m8_api.zmod \
		--header $(FIXTURE_CATALOG_INC)

$(FIXTURE_CATALOG_INC): $(FIXTURE_CATALOG_ASM) ;

$(BUILD)/m4_fixture_blobs.rel: $(FIXTURE_CATALOG_ASM) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

# --- target acceptance objects (tests/target/, acceptance builds only) -------

$(BUILD)/target_test.rel: tests/target/support/target_test.c \
		include/zephyros/kernel.h tests/target/include/selftest.h \
		tests/target/include/irq_selftest.h \
		tests/target/include/m3_selftest.h \
		tests/target/include/m4_selftest.h \
		tests/target/include/m5_selftest.h \
		tests/target/include/m6_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/syscall_test.rel: tests/target/m1/syscall_test.c \
		include/zephyros/kernel.h tests/target/include/selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m3_test.rel: tests/target/m3/m3_test.c \
		include/zephyros/console.h include/zephyros/device.h \
		include/zephyros/kernel.h include/zephyros/platform.h \
		tests/target/include/m3_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m3_irq_probe.rel: tests/target/m3/m3_irq_probe.c \
		include/zephyros/irq.h include/zephyros/platform.h \
		platform/zephyr80/memory.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m3_fixtures.rel: tests/target/m3/m3_fixtures.c \
		include/zephyros/device.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m4_test.rel: tests/target/m4/m4_test.c \
		include/zephyros/module.h include/zephyros/kernel.h \
		include/zephyros/device.h include/zephyros/irq.h \
		include/zephyros/console.h tests/target/include/m4_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m4_fixture_catalog.rel: tests/target/m4/m4_fixture_catalog.c \
		include/zephyros/module.h tests/target/include/m4_selftest.h \
		$(FIXTURE_CATALOG_INC) | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m5_test.rel: tests/target/m5/m5_test.c \
		include/zephyros/romfs.h include/zephyros/module.h \
		include/zephyros/console.h include/zephyros/kernel.h \
		tests/target/include/m5_selftest.h $(BUILD)/m5_expect.inc | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m6_test.rel: tests/target/m6/m6_test.c \
		include/zephyros/process.h include/zephyros/syscall.h \
		include/zephyros/console.h include/zephyros/module.h \
		include/zephyros/kernel.h \
		tests/target/include/m6_selftest.h $(BUILD)/m6_expect.inc | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m7_test.rel: tests/target/m7/m7_test.c \
		include/zephyros/process.h include/zephyros/syscall.h \
		include/zephyros/console.h include/zephyros/module.h \
		include/zephyros/kernel.h include/zephyros/platform.h \
		tests/target/include/m7_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# --- M6 native program (ZEXTEST) ---------------------------------------------
# One ZEX program linked as a flat image at 0100h: _CODE is the image and
# _DATA is the zero-initialized region the loader fills at
# 0100h+image_size.  Geometry comes from tests/m6/zextest.json; zexpack
# validates the linked map against it and writes the reproducible report.

ZEX_TEST_JSON := tests/m6/zextest.json
ZEX_TEST_BUILD := $(BUILD)/zextest
ZEX_TEST_IMAGE_SIZE := $(shell $(PYTHON) tools/zexpack.py get $(ZEX_TEST_JSON) image_size)
ZEX_TEST_DATA_BASE  := $(shell printf '0x%04X' $$((0x0100 + $(ZEX_TEST_IMAGE_SIZE))))

$(ZEX_TEST_BUILD):
	mkdir -p $@

$(BUILD)/m6_expect.inc: $(ZEX_TEST_JSON) tests/m6/gen_expect.py \
		tests/m6/gen_fixtures.py tools/zexpack.py | $(BUILD)
	$(PYTHON) tests/m6/gen_expect.py $(ZEX_TEST_JSON) $@

$(ZEX_TEST_BUILD)/zextest_entry.rel: tests/target/m6/zextest_entry.asm | $(ZEX_TEST_BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(ZEX_TEST_BUILD)/zex_runtime.rel: user/runtime/zex_runtime.asm | $(ZEX_TEST_BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(ZEX_TEST_BUILD)/zextest.rel: tests/target/m6/zextest.c user/runtime/zex_runtime.h \
		tests/target/include/m6_selftest.h $(BUILD)/m6_expect.inc | $(ZEX_TEST_BUILD)
	$(SDCC) -mz80 -c --nostdinc --Werror -I user/runtime \
		-I tests/target/include -I $(BUILD) $< -o $@

$(ZEX_TEST_BUILD)/zextest.lk: $(ZEX_TEST_BUILD)/zextest_entry.rel \
		$(ZEX_TEST_BUILD)/zextest.rel $(ZEX_TEST_BUILD)/zex_runtime.rel | $(ZEX_TEST_BUILD)
	{ \
	  printf '%s\n' '-n' '-m' '-w'; \
	  printf '%s\n' '-i $(ZEX_TEST_BUILD)/zextest.ihx'; \
	  printf '%s\n' '-b _CODE = 0x0100' '-b _DATA = $(ZEX_TEST_DATA_BASE)'; \
	  printf '%s\n' $(ZEX_TEST_BUILD)/zextest_entry.rel \
	                $(ZEX_TEST_BUILD)/zextest.rel \
	                $(ZEX_TEST_BUILD)/zex_runtime.rel; \
	  printf '%s\n' '-e'; \
	} > $@

$(ZEX_TEST_BUILD)/zextest.ihx $(ZEX_TEST_BUILD)/zextest.map: $(ZEX_TEST_BUILD)/zextest.lk
	$(SDLDZ80) -f $(ZEX_TEST_BUILD)/zextest.lk

$(MODULES)/zextest.zex: $(ZEX_TEST_JSON) $(ZEX_TEST_BUILD)/zextest.ihx \
		$(ZEX_TEST_BUILD)/zextest.map tools/zexpack.py | $(MODULES)
	$(PYTHON) tools/zexpack.py pack $(ZEX_TEST_JSON) $@ \
		--objdir $(ZEX_TEST_BUILD) --report $(MODULES)/zextest.report.json

# --- M7 native monitor (MONITOR.ZEX) -----------------------------------------
# The monitor is an ordinary M6 ZEX linked as a flat 0100h image from the
# shared user/runtime components plus user/monitor; geometry comes from
# user/monitor/monitor.json and zexpack validates the linked map against
# it.  It links no kernel object and contains no device knowledge.

MONITOR_JSON := user/monitor/monitor.json
MONITOR_BUILD := $(BUILD)/monitor
MONITOR_IMAGE_SIZE := $(shell $(PYTHON) tools/zexpack.py get $(MONITOR_JSON) image_size)
MONITOR_DATA_BASE  := $(shell printf '0x%04X' $$((0x0100 + $(MONITOR_IMAGE_SIZE))))

MONITOR_USER_CFLAGS := -mz80 -c --nostdinc --Werror \
	-I user/runtime -I user/monitor

MONITOR_OBJECTS := $(MONITOR_BUILD)/monitor_entry.rel \
		$(MONITOR_BUILD)/monitor.rel $(MONITOR_BUILD)/monitor_mem.rel \
		$(MONITOR_BUILD)/monitor_io.rel \
		$(MONITOR_BUILD)/console_io.rel $(MONITOR_BUILD)/hexfmt.rel \
		$(MONITOR_BUILD)/hexdump.rel $(MONITOR_BUILD)/status.rel \
		$(MONITOR_BUILD)/zex_runtime.rel \
		$(MONITOR_BUILD)/zex_diag_runtime.rel

$(MONITOR_BUILD):
	mkdir -p $@

$(MONITOR_BUILD)/monitor_entry.rel: user/monitor/monitor_entry.asm | $(MONITOR_BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(MONITOR_BUILD)/zex_runtime.rel: user/runtime/zex_runtime.asm | $(MONITOR_BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(MONITOR_BUILD)/zex_diag_runtime.rel: user/runtime/zex_diag_runtime.asm | $(MONITOR_BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(MONITOR_BUILD)/monitor.rel: user/monitor/monitor.c user/monitor/monitor.h \
		user/runtime/zex_runtime.h user/runtime/console_io.h \
		user/runtime/hexfmt.h user/runtime/status.h | $(MONITOR_BUILD)
	$(SDCC) $(MONITOR_USER_CFLAGS) $< -o $@

$(MONITOR_BUILD)/monitor_mem.rel: user/monitor/monitor_mem.c \
		user/monitor/monitor.h user/runtime/zex_runtime.h \
		user/runtime/console_io.h user/runtime/hexfmt.h \
		user/runtime/hexdump.h user/runtime/status.h | $(MONITOR_BUILD)
	$(SDCC) $(MONITOR_USER_CFLAGS) $< -o $@

$(MONITOR_BUILD)/monitor_io.rel: user/monitor/monitor_io.c \
		user/monitor/monitor.h user/runtime/zex_runtime.h \
		user/runtime/console_io.h user/runtime/hexfmt.h \
		user/runtime/status.h | $(MONITOR_BUILD)
	$(SDCC) $(MONITOR_USER_CFLAGS) $< -o $@

$(MONITOR_BUILD)/console_io.rel: user/runtime/console_io.c \
		user/runtime/console_io.h user/runtime/zex_runtime.h \
		| $(MONITOR_BUILD)
	$(SDCC) $(MONITOR_USER_CFLAGS) $< -o $@

$(MONITOR_BUILD)/hexfmt.rel: user/runtime/hexfmt.c user/runtime/hexfmt.h \
		| $(MONITOR_BUILD)
	$(SDCC) $(MONITOR_USER_CFLAGS) $< -o $@

$(MONITOR_BUILD)/hexdump.rel: user/runtime/hexdump.c user/runtime/hexdump.h \
		user/runtime/hexfmt.h | $(MONITOR_BUILD)
	$(SDCC) $(MONITOR_USER_CFLAGS) $< -o $@

$(MONITOR_BUILD)/status.rel: user/runtime/status.c user/runtime/status.h \
		user/runtime/console_io.h user/runtime/hexfmt.h | $(MONITOR_BUILD)
	$(SDCC) $(MONITOR_USER_CFLAGS) $< -o $@

$(MONITOR_BUILD)/monitor.lk: $(MONITOR_OBJECTS) $(MONITOR_JSON) | $(MONITOR_BUILD)
	{ \
	  printf '%s\n' '-n' '-m' '-w'; \
	  printf '%s\n' '-i $(MONITOR_BUILD)/monitor.ihx'; \
	  printf '%s\n' '-b _CODE = 0x0100' '-b _DATA = $(MONITOR_DATA_BASE)'; \
	  printf '%s\n' $(MONITOR_OBJECTS); \
	  printf '%s\n' '-e'; \
	} > $@

$(MONITOR_BUILD)/monitor.ihx $(MONITOR_BUILD)/monitor.map: $(MONITOR_BUILD)/monitor.lk
	$(SDLDZ80) -f $(MONITOR_BUILD)/monitor.lk

$(MODULES)/monitor.zex: $(MONITOR_JSON) $(MONITOR_BUILD)/monitor.ihx \
		$(MONITOR_BUILD)/monitor.map tools/zexpack.py | $(MODULES)
	$(PYTHON) tools/zexpack.py pack $(MONITOR_JSON) $@ \
		--objdir $(MONITOR_BUILD) --report $(MODULES)/monitor.report.json

# Deterministic malformed fixtures (one per validation boundary; see
# tests/m6/gen_fixtures.py) shared by the M6 acceptance manifests.
$(MODULES)/zex-fixtures.stamp: tests/m6/gen_fixtures.py tools/zexpack.py | $(MODULES)
	$(PYTHON) tests/m6/gen_fixtures.py $(MODULES)
	touch $@

# --- ROMFS recovery store (ROM pages 2-7) ------------------------------------
# Production images pack recovery/manifest.json; the M5 acceptance images
# use the test manifest (SIO module + deterministic cross-page pattern
# asset) and the corrupt image patches the packed header metadata so the
# kernel's validation is exercised on target.
ifeq ($(ZOS_M5_CORRUPT_SELFTEST),1)
ROMFS_MANIFEST := tests/m5/manifest.json
ROMFS_EXTRA_DEPS := $(MODULES)/m5_crosspage.bin tests/m5/corrupt_romfs.py
else ifeq ($(ZOS_M5_SELFTEST),1)
ROMFS_MANIFEST := tests/m5/manifest.json
ROMFS_EXTRA_DEPS := $(MODULES)/m5_crosspage.bin
else ifeq ($(ZOS_M6_SELFTEST),1)
ROMFS_MANIFEST := tests/m6/manifest.json
ROMFS_EXTRA_DEPS := $(MODULES)/zextest.zex $(MODULES)/zex-fixtures.stamp
else ifeq ($(ZOS_M6_FAILURE_SELFTEST),1)
ROMFS_MANIFEST := tests/m6/manifest-failure.json
ROMFS_EXTRA_DEPS := $(MODULES)/zextest.zex $(MODULES)/zex-fixtures.stamp
else ifeq ($(ZOS_M7_SELFTEST),1)
ROMFS_MANIFEST := tests/m7/manifest.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex
else
ROMFS_MANIFEST := recovery/manifest.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex
endif

$(BUILD)/recovery.romfs: $(ROMFS_MANIFEST) $(MODULES)/sio.zmod \
		$(ROMFS_EXTRA_DEPS) tools/romfspack.py \
		$(ZOS_PLATFORM_MEMORY_INC) | $(BUILD)
ifeq ($(ZOS_M5_CORRUPT_SELFTEST),1)
	$(PYTHON) tools/romfspack.py --memory-inc $(ZOS_PLATFORM_MEMORY_INC) pack \
		$(ROMFS_MANIFEST) $(BUILD)/recovery-clean.romfs --objdir $(MODULES)
	$(PYTHON) tests/m5/corrupt_romfs.py $(BUILD)/recovery-clean.romfs $@
else
	$(PYTHON) tools/romfspack.py --memory-inc $(ZOS_PLATFORM_MEMORY_INC) pack \
		$(ROMFS_MANIFEST) $@ --objdir $(MODULES)
endif

# Deterministic cross-page pattern asset for the M5 acceptance manifest.
$(MODULES)/m5_crosspage.bin: tests/m5/gen_asset.py | $(MODULES)
	$(PYTHON) tests/m5/gen_asset.py $@

# Expected-container constants for the M5 target selftest.
$(BUILD)/m5_expect.inc: $(MODULES)/sio.zmod tests/m5/gen_expect.py | $(BUILD)
	$(PYTHON) tests/m5/gen_expect.py $< $@ M5_SIO_ZMOD

# --- link --------------------------------------------------------------------
# The canonical `build/` output uses the checked-in linker/zephyr80.lk.
# Every self-test and fallback build (different BUILD dir and/or extra
# objects) generates its linker file from the same object list the link
# step depends on, so the lists cannot drift apart.

ZOS_TEST_LINK := $(filter 1,$(ZOS_SELFTEST) $(ZOS_IRQ_SELFTEST) \
		$(ZOS_M3_SELFTEST) $(ZOS_M4_SELFTEST) $(ZOS_M4_SIO_IRQ) \
		$(ZOS_M4_FAILURE_SELFTEST) $(ZOS_M5_SELFTEST) \
		$(ZOS_M5_CORRUPT_SELFTEST) $(ZOS_M6_SELFTEST) \
		$(ZOS_M6_FAILURE_SELFTEST) $(ZOS_M7_SELFTEST))
ifeq ($(BUILD),build)
ifneq ($(ZOS_TEST_LINK),)
GENERATED_LK := 1
endif
else
GENERATED_LK := 1
endif

ifeq ($(GENERATED_LK),1)
$(BUILD)/zephyr80.lk: $(OBJS) $(TARGET_TEST_OBJS) $(END_OBJ) | $(BUILD)
	{ \
	  printf '%s\n' '-n' '-m' '-w'; \
	  printf '%s\n' '-i $(BUILD)/zephyros.ihx'; \
	  printf '%s\n' '-b _CODE = 0x2000' '-b _DATA = 0xC000' \
	                '-b _COMMON = 0xE000'; \
	  printf '%s\n' $(OBJS) $(TARGET_TEST_OBJS) $(END_OBJ); \
	  printf '%s\n' '-e'; \
	} > $@

$(BUILD)/zephyros.ihx $(BUILD)/zephyros.map: $(OBJS) $(TARGET_TEST_OBJS) \
		$(END_OBJ) $(BUILD)/zephyr80.lk | $(BUILD)
	$(SDLDZ80) -f $(BUILD)/zephyr80.lk
else
$(BUILD)/zephyros.ihx $(BUILD)/zephyros.map: $(OBJS) $(END_OBJ) linker/zephyr80.lk | $(BUILD)
	$(SDLDZ80) -f linker/zephyr80.lk
endif

# --- pack and validate -------------------------------------------------------
# tools/pack_image.py validates the memory layout, builds the eight 64 KiB
# ROM pages (boot pages 0/1 + the ROMFS recovery pages 2-7 from
# $(BUILD)/recovery.romfs), emits the 512 KiB images and a machine-readable
# manifest.  tools/mapcheck.py is the authoritative post-link layout check
# and runs automatically on every build, before the ROM is considered
# complete.

$(IMAGE): $(BUILD)/zephyros.ihx $(BUILD)/zephyros.map \
		$(BUILD)/recovery.romfs $(ARENA_PLAN) \
		tools/pack_image.py tools/mapcheck.py $(MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) tools/mapcheck.py $(MAPCHECK_FLAGS) $(BUILD)/zephyros.map \
		$(MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) tools/pack_image.py $(BUILD)/zephyros.ihx $(BUILD)/zephyros.map \
		$(BUILD) "$(PK_SUFFIX)" $(MEMORY_INC)

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
	$(PYTHON) tools/mapcheck.py $(MAPCHECK_FLAGS) $(BUILD)/zephyros.map \
		$(MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) tools/pack_image.py $(BUILD)/zephyros.ihx $(BUILD)/zephyros.map \
		$(BUILD) "$(PK_SUFFIX)" $(MEMORY_INC)
	sha256sum $(IMAGE) $(MAME_IMAGE)
	cmp $(IMAGE) $(MAME_IMAGE)
	@echo "make check: OK"

clean:
	rm -rf $(BUILD) build-*

# --- syscall self-test image (target-side RST 30h round trip) ----------------

syscall-selftest:
	$(MAKE) BUILD=build-selftest ZOS_SELFTEST=1 \
		build-selftest/zephyros-selftest.bin build-selftest/zephyr80-selftest.bin
	$(PYTHON) tools/mapcheck.py build-selftest/zephyros.map \
		$(ZOS_PLATFORM_MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	@echo "syscall-selftest image: build-selftest/zephyros-selftest.bin"

irq-selftest:
	$(MAKE) BUILD=build-irq-selftest ZOS_SELFTEST=1 ZOS_IRQ_SELFTEST=1 \
		build-irq-selftest/zephyros-irq-selftest.bin \
		build-irq-selftest/zephyr80-irq-selftest.bin
	$(PYTHON) tools/mapcheck.py build-irq-selftest/zephyros.map \
		$(ZOS_PLATFORM_MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	@echo "irq-selftest image: build-irq-selftest/zephyros-irq-selftest.bin"

# --- M3 acceptance image (syscall + irq + device/console/IRQ driver) ---------

m3-selftest:
	$(MAKE) BUILD=build-m3-selftest ZOS_SELFTEST=1 ZOS_IRQ_SELFTEST=1 \
		ZOS_M3_SELFTEST=1 \
		build-m3-selftest/zephyros-m3-selftest.bin \
		build-m3-selftest/zephyr80-m3-selftest.bin
	$(PYTHON) tools/mapcheck.py build-m3-selftest/zephyros.map \
		$(ZOS_PLATFORM_MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	@echo "m3-selftest image: build-m3-selftest/zephyros-m3-selftest.bin"

# --- M4 host-side ZMOD tooling tests ----------------------------------------
# Builds the SIO module variants and the synthetic fixtures with the real
# toolchain, packs and validates them, then runs the host ZMOD test suite
# (fixture-dependent tests are skipped by plain `make test` when the packed
# modules are absent).

zmod-test: $(MODULES)/sio.zmod $(MODULES)/sio-irq.zmod \
		$(MODULES)/sio-fail.zmod $(FIXTURE_ZMODS)
	$(PYTHON) tools/zmodpack.py validate $(MODULES)/sio.zmod \
		$(MODULES)/sio-irq.zmod $(MODULES)/sio-fail.zmod
	$(PYTHON) tools/zmodpack.py validate $(FIXTURE_ZMODS)
	$(PYTHON) tools/zmodpack.py dump $(MODULES)/sio.zmod
	$(PYTHON) -m unittest discover -s tests -p 'test_zmod.py' -v
	@echo "zmod-test: OK"

# --- M4 loader acceptance image (synthetic fixtures, no dynamic boot device) -

m4-loader-selftest:
	$(MAKE) BUILD=build-m4-loader-selftest ZOS_M4_SELFTEST=1 \
		build-m4-loader-selftest/zephyros-m4-loader-selftest.bin \
		build-m4-loader-selftest/zephyr80-m4-loader-selftest.bin
	$(PYTHON) tools/mapcheck.py build-m4-loader-selftest/zephyros.map \
		$(ZOS_PLATFORM_MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_m4_sim.py' -v
	@echo "m4-loader-selftest image: build-m4-loader-selftest/zephyros-m4-loader-selftest.bin"

# --- M4 dynamic-driver acceptance image (SIO module + IRQ self-test) ---------

m4-selftest:
	$(MAKE) BUILD=build-m4-selftest ZOS_M4_SELFTEST=1 ZOS_M4_SIO_IRQ=1 \
		build-m4-selftest/zephyros-m4-selftest.bin \
		build-m4-selftest/zephyr80-m4-selftest.bin
	$(PYTHON) tools/mapcheck.py build-m4-selftest/zephyros.map \
		$(ZOS_PLATFORM_MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_m4_sim.py' -v
	@echo "m4-selftest image: build-m4-selftest/zephyros-m4-selftest.bin"

# Same packed module bytes, arena base shifted 0x0200 lower (9E00h-BE00h).

m4-placement-selftest:
	$(MAKE) BUILD=build-m4-placement-selftest ZOS_M4_SELFTEST=1 \
		ZOS_M4_PLACEMENT=1 \
		build-m4-placement-selftest/zephyros-m4-placement-selftest.bin \
		build-m4-placement-selftest/zephyr80-m4-placement-selftest.bin
	$(PYTHON) tools/mapcheck.py build-m4-placement-selftest/zephyros.map \
		build-m4-placement-selftest/platform/zephyr80/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_m4_sim.py' -v
	@echo "m4-placement-selftest image: build-m4-placement-selftest/zephyros-m4-placement-selftest.bin"

# --- M4 failure image (module init fails; rollback must be exact) ------------

m4-failure-selftest:
	$(MAKE) BUILD=build-m4-failure ZOS_M4_FAILURE_SELFTEST=1 \
		build-m4-failure/zephyros-m4-failure.bin \
		build-m4-failure/zephyr80-m4-failure.bin
	$(PYTHON) tools/mapcheck.py build-m4-failure/zephyros.map \
		$(ZOS_PLATFORM_MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_m4_sim.py' -v
	@echo "m4-failure image: build-m4-failure/zephyros-m4-failure.bin"

# --- M5 ROMFS recovery-store tooling and acceptance images -------------------

romfs-test: $(BUILD)/recovery.romfs
	$(PYTHON) tools/romfspack.py --memory-inc $(ZOS_PLATFORM_MEMORY_INC) \
		validate $(BUILD)/recovery.romfs
	$(PYTHON) tools/romfspack.py --memory-inc $(ZOS_PLATFORM_MEMORY_INC) \
		dump $(BUILD)/recovery.romfs
	$(PYTHON) -m unittest discover -s tests -p 'test_romfs.py' -v
	@echo "romfs-test: OK"

m5-selftest:
	$(MAKE) BUILD=build-m5-selftest ZOS_M5_SELFTEST=1 \
		build-m5-selftest/zephyros-m5-selftest.bin \
		build-m5-selftest/zephyr80-m5-selftest.bin
	$(PYTHON) tools/mapcheck.py build-m5-selftest/zephyros.map \
		$(ZOS_PLATFORM_MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_romfs_sim.py' -v
	@echo "m5-selftest image: build-m5-selftest/zephyros-m5-selftest.bin"

m5-corrupt-selftest:
	$(MAKE) BUILD=build-m5-corrupt-selftest ZOS_M5_CORRUPT_SELFTEST=1 \
		build-m5-corrupt-selftest/zephyros-m5-corrupt.bin \
		build-m5-corrupt-selftest/zephyr80-m5-corrupt.bin
	$(PYTHON) tools/mapcheck.py build-m5-corrupt-selftest/zephyros.map \
		$(ZOS_PLATFORM_MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	@echo "m5-corrupt image: build-m5-corrupt-selftest/zephyros-m5-corrupt.bin"

# --- M6 ZEX tooling, acceptance image, and failure image ---------------------

zex-test: $(MODULES)/zextest.zex $(MODULES)/zex-fixtures.stamp
	$(PYTHON) tools/zexpack.py validate $(MODULES)/zextest.zex
	$(PYTHON) tools/zexpack.py dump $(MODULES)/zextest.zex
	$(PYTHON) -m unittest discover -s tests -p 'test_zex.py' -v
	@echo "zex-test: OK"

m6-selftest:
	$(MAKE) BUILD=build-m6-selftest ZOS_M6_SELFTEST=1 \
		build-m6-selftest/zephyros-m6-selftest.bin \
		build-m6-selftest/zephyr80-m6-selftest.bin
	$(PYTHON) tools/mapcheck.py build-m6-selftest/zephyros.map \
		$(ZOS_PLATFORM_MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_m6_sim.py' -v
	@echo "m6-selftest image: build-m6-selftest/zephyros-m6-selftest.bin"

m6-failure-selftest:
	$(MAKE) BUILD=build-m6-failure-selftest ZOS_M6_FAILURE_SELFTEST=1 \
		build-m6-failure-selftest/zephyros-m6-failure.bin \
		build-m6-failure-selftest/zephyr80-m6-failure.bin
	$(PYTHON) tools/mapcheck.py build-m6-failure-selftest/zephyros.map \
		$(ZOS_PLATFORM_MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_m6_sim.py' -v
	@echo "m6-failure image: build-m6-failure-selftest/zephyros-m6-failure.bin"

# --- M7 native monitor tooling and acceptance image --------------------------
# monitor-test proves the ZEX layout and the monitor/runtime source
# contract; m7-selftest builds the bootable M7 image (kernel-side
# diagnostic selftest plus the interactive monitor over ROMFS) and
# replays the scripted interactive transcript in the simulator.

monitor-test: $(MODULES)/monitor.zex
	$(PYTHON) tools/zexpack.py validate $(MODULES)/monitor.zex
	$(PYTHON) tools/zexpack.py dump $(MODULES)/monitor.zex
	$(PYTHON) -m unittest discover -s tests -p 'test_monitor.py' -v
	@echo "monitor-test: OK"

m7-selftest:
	$(MAKE) BUILD=build-m7-selftest ZOS_M7_SELFTEST=1 \
		build-m7-selftest/zephyros-m7-selftest.bin \
		build-m7-selftest/zephyr80-m7-selftest.bin
	$(PYTHON) tools/mapcheck.py build-m7-selftest/zephyros.map \
		$(ZOS_PLATFORM_MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_m7_sim.py' -v
	@echo "m7-selftest image: build-m7-selftest/zephyros-m7-selftest.bin"

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

smoke-hardware-m4:
	$(PYTHON) tools/serial_smoke.py --mode m4 --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m4-failure:
	$(PYTHON) tools/serial_smoke.py --mode m4-failure --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m5:
	$(PYTHON) tools/serial_smoke.py --mode m5 --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m5-corrupt:
	$(PYTHON) tools/serial_smoke.py --mode m5-corrupt --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m6:
	$(PYTHON) tools/serial_smoke.py --mode m6 --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m6-failure:
	$(PYTHON) tools/serial_smoke.py --mode m6-failure --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m7:
	$(PYTHON) tools/serial_smoke.py --mode m7 --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))
