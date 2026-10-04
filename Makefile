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

# M8 IOC acceptance image: production-style boot (derived arena, SIO +
# IOC from ROMFS), the M8 target diagnostic reporting through the direct
# early SIO (command lane plus the bulk READY/DONE acceptance over the
# /SHARED service), then the unchanged monitor launcher.  It keeps the
# derived arena so the acceptance image exercises the production geometry
# with the target objects linked in.
# The V9958 boot load stays compiled out: this image's accepted contract
# is the IOC transport with the SIO console, and the console-capable
# V9958 module (package 19) is exercised by m8-v9958 and m8-console.
ifeq ($(ZOS_M8_IOC_SELFTEST),1)
CFLAGS    += -DZOS_M8_IOC_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/m8_ioc_test.rel
PK_SUFFIX := -m8-ioc-selftest
ZOS_V9958_BOOT := 0
endif

# M8 HID acceptance image: production-style boot (derived arena, SIO +
# IOC + HID from ROMFS), the M8 HID target diagnostic (HID class/device
# shape, the scripted USB-keyboard typing pass with output through the
# bound SIO console, then the forced SIO input fallback), then the
# unchanged monitor launcher.
# The V9958 boot load stays compiled out so the accepted HID/SIO and
# SIO/SIO console bindings of this package's images are preserved; the
# HID/V9958 and SIO/V9958 bindings are covered by the package 19 images.
ifeq ($(ZOS_M8_HID_SELFTEST),1)
CFLAGS    += -DZOS_M8_HID_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/m8_hid_test.rel
PK_SUFFIX := -m8-hid-selftest
ZOS_V9958_BOOT := 0
endif

# M8 HID no-IOC image: the same target diagnostic with the IOC boot load
# disabled.  HID must fail dependency acquisition and be rolled back by
# the loader; the console must still bind the SIO fallback in both
# directions and pass the typed serial byte through the console service.
ifeq ($(ZOS_M8_HID_NOIOC),1)
CFLAGS    += -DZOS_M8_HID_NOIOC=1
ZOS_IOC_BOOT := 0
TARGET_TEST_OBJS += $(BUILD)/m8_hid_test.rel
PK_SUFFIX := -m8-hid-noioc-selftest
ZOS_V9958_BOOT := 0
endif

# M8 HID malformed-dependency image: the ROMFS `ioc.zmod` entry carries a
# test fixture (tests/modules/hid_fake_ioc.c) that registers an IOC-class
# device whose command operation is null.  HID must verify the interface,
# reject it without calling the null pointer, and roll back while the
# fixture device and the SIO fallback stay untouched.  The declared
# boot-set plan counts the fixture bytes actually loaded as "ioc".
ifeq ($(ZOS_M8_HID_BADIOC),1)
CFLAGS    += -DZOS_M8_HID_BADIOC=1
BOOT_IOC_ZMOD = $(MODULES)/fakeioc.zmod
TARGET_TEST_OBJS += $(BUILD)/m8_hid_test.rel
PK_SUFFIX := -m8-hid-badioc-selftest
ZOS_V9958_BOOT := 0
endif

# M8 V9958 video acceptance image: production-style boot (derived
# arena, SIO + IOC + HID + the V9958 video core from ROMFS), the M8
# V9958 target diagnostic (VIDEO class/device shape, all seven native
# operations, malformed rejection, reset, and the deterministic visible
# pattern), then the unchanged monitor launcher.  The smoke image waits
# for the host sync sequence because this bench's reset also resets the
# USB bridge.
ifeq ($(ZOS_M8_V9958_SELFTEST),1)
CFLAGS    += -DZOS_M8_V9958_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/m8_v9958_test.rel
PK_SUFFIX := -m8-v9958-selftest
endif

# M8 V9958 font-failure image: the ROMFS `v9958-6x8.font` entry carries
# a deliberately undersized stub.  The module must fail its resource
# check, publish no device, and leave the SIO fallback, IOC transport
# and HID keyboard untouched.  The target diagnostic proves the absence
# and that the earlier devices still work.
ifeq ($(ZOS_M8_V9958_FONTFAIL),1)
CFLAGS    += -DZOS_M8_V9958_FONTFAIL=1
TARGET_TEST_OBJS += $(BUILD)/m8_v9958_test.rel
PK_SUFFIX := -m8-v9958-fontfail-selftest
endif

# M8 console acceptance image (package 19): the full production stack
# (SIO + IOC + HID + the V9958 video core with its character renderer)
# from ROMFS, the console target diagnostic reporting the selected
# input/output classes over the direct early SIO, and the unchanged M7
# `monitor.zex` running through the normal HID-input/V9958-output
# binding.  This is the image the human keyboard/display pass uses; the
# interactive session is replayed from these exact bytes by
# tests/test_m8_sim.py.
ifeq ($(ZOS_M8_CONSOLE_SELFTEST),1)
CFLAGS    += -DZOS_M8_CONSOLE_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/m8_console_test.rel
PK_SUFFIX := -m8-console-selftest
endif

# M8 integration acceptance image (package 20): the full production
# stack (SIO + IOC + HID + the V9958 video/console module) from ROMFS,
# the integration target diagnostic (class/capability discovery of all
# four residents, the single default binding, PING command/status,
# malformed/null/class/capability rejections and their invariants)
# reported over the direct early SIO, then the unchanged M7
# `monitor.zex` launcher for the human HID/V9958 session.  This is the
# image the final physical acceptance gate builds and flashes;
# tests/test_m8_acceptance.py replays these exact bytes plus the
# production image.
ifeq ($(ZOS_M8_SELFTEST),1)
CFLAGS    += -DZOS_M8_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/m8_test.rel
PK_SUFFIX := -m8-selftest
endif

# M8 fallback-matrix images (package 20): the same four-module boot
# contract with one deliberate failure injected through the variant's
# ROMFS manifest (`ZOS_M8_FALLBACK` = nohid, nohid-nov9958, badioc or
# arena).  The kernel boot gates stay at their production values, so
# every variant exercises the real sequential load path, the precise
# early-SIO diagnostic and the console fallback policy.  The shared
# target diagnostic reports the surviving binding; the expected binding
# and direction per variant are asserted by tests/test_m8_fallback.py
# and the matching serial_smoke modes.
ifeq ($(ZOS_M8_FALLBACK_SELFTEST),1)
CFLAGS    += -DZOS_M8_FALLBACK_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/m8_fallback_test.rel
ifeq ($(ZOS_M8_FALLBACK),nohid)
PK_SUFFIX := -m8-fb-nohid
else ifeq ($(ZOS_M8_FALLBACK),nohid-nov9958)
PK_SUFFIX := -m8-fb-nohid-nov9958
else ifeq ($(ZOS_M8_FALLBACK),badioc)
PK_SUFFIX := -m8-fb-badioc
else ifeq ($(ZOS_M8_FALLBACK),arena)
PK_SUFFIX := -m8-fb-arena
else
$(error ZOS_M8_FALLBACK_SELFTEST needs ZOS_M8_FALLBACK=nohid|nohid-nov9958|badioc|arena)
endif
endif

# Hardware-smoke host sync (kernel/main.c): the M8 IOC smoke image waits
# for the host's two-byte sync sequence on SIO0/B before the banner,
# because this bench's reset line also resets the USB bridge.  Only
# diagnostic images enable it; production and every other acceptance
# image compile the gate out byte-identically.
ifeq ($(ZOS_SMOKE_HOST_SYNC),1)
CFLAGS    += -DZOS_SMOKE_HOST_SYNC=1
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

# IOC command-lane boot load (kernel/main.c): production and the M8
# images load ioc.zmod from the recovery store after the SIO fallback;
# the pre-M8 acceptance images do not carry the module and keep their
# accepted boot byte-for-byte by compiling the block out.
ifneq ($(filter 1,$(ZOS_SELFTEST) $(ZOS_IRQ_SELFTEST) $(ZOS_M3_SELFTEST) \
	$(ZOS_M4_SELFTEST) $(ZOS_M4_SIO_IRQ) $(ZOS_M4_FAILURE_SELFTEST) \
	$(ZOS_M4_PLACEMENT) $(ZOS_M5_SELFTEST) $(ZOS_M5_CORRUPT_SELFTEST) \
	$(ZOS_M6_SELFTEST) $(ZOS_M6_FAILURE_SELFTEST) $(ZOS_M7_SELFTEST)),)
ZOS_IOC_BOOT := 0
endif
ZOS_IOC_BOOT ?= 1
CFLAGS    += -DZOS_IOC_BOOT=$(ZOS_IOC_BOOT)

# HID keyboard/text boot load (kernel/main.c): production and the M8
# images load hid.zmod from the recovery store after the IOC transport;
# the module discovers IOC by class/capability and a missing or
# malformed dependency is a load failure, never a partial registration.
# Pre-M8 acceptance images do not carry the module and compile the block
# out so their accepted boot stays byte-for-byte as validated.
ifneq ($(filter 1,$(ZOS_SELFTEST) $(ZOS_IRQ_SELFTEST) $(ZOS_M3_SELFTEST) \
	$(ZOS_M4_SELFTEST) $(ZOS_M4_SIO_IRQ) $(ZOS_M4_FAILURE_SELFTEST) \
	$(ZOS_M4_PLACEMENT) $(ZOS_M5_SELFTEST) $(ZOS_M5_CORRUPT_SELFTEST) \
	$(ZOS_M6_SELFTEST) $(ZOS_M6_FAILURE_SELFTEST) $(ZOS_M7_SELFTEST)),)
ZOS_HID_BOOT := 0
endif
ZOS_HID_BOOT ?= 1
CFLAGS    += -DZOS_HID_BOOT=$(ZOS_HID_BOOT)

# V9958 video boot load (kernel/main.c): production and the M8 images
# load v9958.zmod from the recovery store after HID.  The module has no
# IOC dependency; a missing font resource or a failed VDP bring-up is
# rolled back by the loader and must not disturb the SIO fallback, the
# IOC transport or HID.  Pre-M8 acceptance images do not carry the
# module and compile the block out so their accepted boot stays
# byte-for-byte as validated.
ifneq ($(filter 1,$(ZOS_SELFTEST) $(ZOS_IRQ_SELFTEST) $(ZOS_M3_SELFTEST) \
	$(ZOS_M4_SELFTEST) $(ZOS_M4_SIO_IRQ) $(ZOS_M4_FAILURE_SELFTEST) \
	$(ZOS_M4_PLACEMENT) $(ZOS_M5_SELFTEST) $(ZOS_M5_CORRUPT_SELFTEST) \
	$(ZOS_M6_SELFTEST) $(ZOS_M6_FAILURE_SELFTEST) $(ZOS_M7_SELFTEST)),)
ZOS_V9958_BOOT := 0
endif
ZOS_V9958_BOOT ?= 1
CFLAGS    += -DZOS_V9958_BOOT=$(ZOS_V9958_BOOT)

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
	$(ZOS_M6_FAILURE_SELFTEST) $(ZOS_M7_SELFTEST) \
	$(ZOS_M8_IOC_SELFTEST) $(ZOS_M8_HID_SELFTEST) \
	$(ZOS_M8_HID_NOIOC) $(ZOS_M8_HID_BADIOC) \
	$(ZOS_M8_V9958_SELFTEST) $(ZOS_M8_V9958_FONTFAIL) \
	$(ZOS_M8_CONSOLE_SELFTEST) $(ZOS_M8_SELFTEST) \
	$(ZOS_M8_FALLBACK_SELFTEST)),)
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

# Declared boot set for the derived-arena plan.  Every ROMFS-boot image
# loads the SIO fallback module and then, per its boot gates, the IOC
# transport and the HID keyboard/text input module; the pre-M5
# embedded-catalog images keep their single boot module.  (MODULES is
# defined below this point, so keep this a recursive assignment.)
BOOT_IOC_ZMOD ?= $(MODULES)/ioc.zmod
ifeq ($(ZOS_ROMFS_BOOT),0)
BOOT_ZMODS = $(BOOT_ZMOD)
else
BOOT_ZMODS = $(BOOT_ZMOD)
ifneq ($(ZOS_IOC_BOOT),0)
BOOT_ZMODS += $(BOOT_IOC_ZMOD)
endif
ifneq ($(ZOS_HID_BOOT),0)
BOOT_ZMODS += $(MODULES)/hid.zmod
endif
ifneq ($(ZOS_V9958_BOOT),0)
BOOT_ZMODS += $(MODULES)/v9958.zmod
endif
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
# committed linker file's list.  Target-test objects (acceptance images)
# are part of the real link and therefore part of the geometry probe.
PROBE_OBJS := $(filter-out $(BUILD)/arena.rel,$(OBJS) $(TARGET_TEST_OBJS) \
		$(END_OBJ)) \
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

.PHONY: all test check check-fast clean syscall-selftest irq-selftest \
	m3-selftest zmod-test m4-loader-selftest m4-selftest \
	m4-placement-selftest m4-failure-selftest romfs-test m5-selftest \
	m5-corrupt-selftest zex-test m6-selftest m6-failure-selftest \
	monitor-test m7-selftest ioc-test m8-ioc-selftest hid-test \
	m8-hid-selftest m8-hid-noioc-selftest m8-hid-badioc-selftest \
	v9958-test m8-v9958-selftest m8-v9958-fontfail-selftest \
	m8-console-selftest \
	m8-selftest m8-fallback-selftests \
	smoke-hardware \
	smoke-hardware-selftest smoke-hardware-irq smoke-hardware-m3 \
	smoke-hardware-m4 smoke-hardware-m4-failure smoke-hardware-m5 \
	smoke-hardware-m5-corrupt smoke-hardware-m6 smoke-hardware-m6-failure \
	smoke-hardware-m7 smoke-hardware-m8-ioc smoke-hardware-m8-hid \
	smoke-hardware-m8-v9958 smoke-hardware-m8-console \
	smoke-hardware-m8 smoke-hardware-m8-fb-nohid \
	smoke-hardware-m8-fb-nohid-nov9958 smoke-hardware-m8-fb-badioc \
	smoke-hardware-m8-fb-arena

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
MOD_CFLAGS := -mz80 -c --nostdinc --Werror -I include -I platform -I drivers/sio \
	-I drivers/ioc

$(MODULES):
	mkdir -p $@

# Declared boot-set arena plan (derived builds only): per-module
# image+BSS/granules and the used/free totals; the build fails here when
# the declared boot set cannot coexist in the linked arena.
ifeq ($(ZOS_ARENA_DERIVED),1)
ARENA_PLAN := $(BUILD)/arena.plan.json
$(ARENA_PLAN): $(BOOT_ZMODS) tools/zmodpack.py $(MEMORY_INC) | $(BUILD)
	$(PYTHON) tools/zmodpack.py plan --geometry $(MEMORY_INC) \
		--report $@ $(BOOT_ZMODS)
else
ARENA_PLAN :=
endif

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

# --- IOC module objects (production source) ---

$(MODULES)/ioc.rel: drivers/ioc/ioc.c drivers/ioc/ioc_driver.h \
		drivers/ioc/ioc_proto.h include/zephyros/ioc.h \
		include/zephyros/device.h include/zephyros/platform.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

$(MODULES)/ioc_io.rel: drivers/ioc/ioc_io.asm \
		platform/zephyr80/hardware.inc | $(MODULES)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(MODULES)/ioc_module.rel: drivers/ioc/ioc_module.c drivers/ioc/ioc_driver.h \
		include/zephyros/driver.h include/zephyros/device.h \
		include/zephyros/ioc.h include/zephyros/platform.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

IOC_MODULE_OBJS := $(MODULES)/ioc.rel $(MODULES)/ioc_io.rel \
		$(MODULES)/ioc_module.rel $(MODULES)/sdcc_indirect_calls.rel

$(MODULES)/ioc.zmod: modules/manifests/ioc.json $(IOC_MODULE_OBJS) \
		tools/zmodpack.py
	$(PYTHON) tools/zmodpack.py pack $< $@ --objdir $(MODULES) \
		--report $(MODULES)/ioc.report.json

# --- HID module objects (production source) ---
# The driver talks to the IOC class only through the discovered interface
# (no imported IOC-module symbol); the private protocol constants it
# needs are the compile-time CMD/RSP_HID_INPUT names it shares with the
# protocol-aware IOC clients (drivers/ioc/ioc_proto.h).

$(MODULES)/hid.rel: drivers/hid/hid.c drivers/hid/hid_driver.h \
		drivers/ioc/ioc_proto.h include/zephyros/hid.h \
		include/zephyros/ioc.h include/zephyros/device.h \
		include/zephyros/platform.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

$(MODULES)/hid_module.rel: drivers/hid/hid_module.c drivers/hid/hid_driver.h \
		include/zephyros/driver.h include/zephyros/device.h \
		include/zephyros/hid.h include/zephyros/platform.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

HID_MODULE_OBJS := $(MODULES)/hid.rel $(MODULES)/hid_module.rel \
		$(MODULES)/sdcc_indirect_calls.rel

$(MODULES)/hid.zmod: modules/manifests/hid.json $(HID_MODULE_OBJS) \
		tools/zmodpack.py
	$(PYTHON) tools/zmodpack.py pack $< $@ --objdir $(MODULES) \
		--report $(MODULES)/hid.report.json

# Malformed-IOC dependency fixture for the badioc acceptance image
# (test artifact; loaded in place of ioc.zmod by a test ROMFS manifest).

$(MODULES)/hid_fake_ioc.rel: tests/modules/hid_fake_ioc.c \
		include/zephyros/device.h include/zephyros/driver.h \
		include/zephyros/ioc.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

$(MODULES)/fakeioc.zmod: tests/modules/hid-fake-ioc.json \
		$(MODULES)/hid_fake_ioc.rel $(MODULES)/sdcc_indirect_calls.rel \
		tools/zmodpack.py
	$(PYTHON) tools/zmodpack.py pack $< $@ --objdir $(MODULES) \
		--report $(MODULES)/fakeioc.report.json

# --- V9958 module objects (production source) ---
# The driver talks to the directly attached V9958 through its own port
# primitives only; the ROMFS font arrives through the ABI-v2 resource
# imports at init.  No kernel symbol and no other module's symbol is
# referenced.

$(MODULES)/v9958.rel: drivers/v9958/v9958.c drivers/v9958/v9958_driver.h \
		include/zephyros/video.h include/zephyros/device.h \
		include/zephyros/driver.h include/zephyros/platform.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

$(MODULES)/v9958_io.rel: drivers/v9958/v9958_io.asm \
		platform/zephyr80/hardware.inc | $(MODULES)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(MODULES)/v9958_module.rel: drivers/v9958/v9958_module.c \
		drivers/v9958/v9958_driver.h include/zephyros/driver.h \
		include/zephyros/device.h include/zephyros/video.h \
		include/zephyros/platform.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

V9958_MODULE_OBJS := $(MODULES)/v9958.rel $(MODULES)/v9958_io.rel \
		$(MODULES)/v9958_module.rel $(MODULES)/sdcc_indirect_calls.rel

$(MODULES)/v9958.zmod: modules/manifests/v9958.json $(V9958_MODULE_OBJS) \
		tools/zmodpack.py
	$(PYTHON) tools/zmodpack.py pack $< $@ --objdir $(MODULES) \
		--report $(MODULES)/v9958.report.json

# ROMFS font asset (DATA): the reviewable .inc source is the pinned
# reference font; fontpack.py verifies the exact 2048 bytes and the
# pinned SHA-256 before emitting the deterministic binary.
$(MODULES)/v9958-6x8.font: drivers/v9958/assets/font_cp850_6x8.inc \
		tools/fontpack.py | $(MODULES)
	$(PYTHON) tools/fontpack.py $< $@

# Undersized font stub for the font-failure acceptance image (test
# artifact; loaded in place of v9958-6x8.font by a test ROMFS manifest).
$(MODULES)/v9958-font-stub.bin: tests/m8/v9958-font-stub.bin | $(MODULES)
	cp $< $@

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

# Arena-exhaustion fixture for the package-20 fallback matrix: a tiny
# image with a 36-granule BSS pad, large enough that its allocation can
# never succeed in the derived production arena.  Loaded as `v9958.zmod`
# by tests/m8/manifest-fallback-arena.json; the init is never reached.
$(MODULES)/m8regfill.rel: tests/modules/m8_regfill.c \
		include/zephyros/driver.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

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
		tests/target/include/m6_selftest.h \
		tests/target/include/m8_hid_selftest.h \
		tests/target/include/m8_v9958_selftest.h \
		tests/target/include/m8_console_selftest.h \
		tests/target/include/m8_selftest.h \
		tests/target/include/m8_fallback_selftest.h | $(BUILD)
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

$(BUILD)/m8_ioc_test.rel: tests/target/m8/m8_ioc_test.c \
		include/zephyros/device.h include/zephyros/ioc.h \
		include/zephyros/kernel.h drivers/ioc/ioc_proto.h \
		tests/target/include/m8_ioc_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) -Idrivers/ioc $< -o $@

$(BUILD)/m8_hid_test.rel: tests/target/m8/m8_hid_test.c \
		include/zephyros/console.h include/zephyros/device.h \
		include/zephyros/hid.h include/zephyros/kernel.h \
		tests/target/include/m8_hid_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m8_v9958_test.rel: tests/target/m8/m8_v9958_test.c \
		include/zephyros/video.h include/zephyros/device.h \
		include/zephyros/kernel.h \
		tests/target/include/m8_v9958_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m8_console_test.rel: tests/target/m8/m8_console_test.c \
		include/zephyros/console.h include/zephyros/device.h \
		include/zephyros/kernel.h include/zephyros/video.h \
		include/zephyros/hid.h \
		tests/target/include/m8_console_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m8_test.rel: tests/target/m8/m8_test.c \
		include/zephyros/console.h include/zephyros/device.h \
		include/zephyros/hid.h include/zephyros/ioc.h \
		include/zephyros/kernel.h include/zephyros/module.h \
		include/zephyros/video.h drivers/ioc/ioc_proto.h \
		tests/target/include/m8_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) -Idrivers/ioc $< -o $@

$(BUILD)/m8_fallback_test.rel: tests/target/m8/m8_fallback_test.c \
		include/zephyros/console.h include/zephyros/device.h \
		include/zephyros/kernel.h \
		tests/target/include/m8_fallback_selftest.h | $(BUILD)
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
else ifeq ($(ZOS_M8_HID_BADIOC),1)
ROMFS_MANIFEST := tests/m8/manifest-hid-badioc.json
ROMFS_EXTRA_DEPS := $(MODULES)/fakeioc.zmod $(MODULES)/monitor.zex
else ifeq ($(ZOS_M8_V9958_FONTFAIL),1)
ROMFS_MANIFEST := tests/m8/manifest-v9958-fontfail.json
ROMFS_EXTRA_DEPS := $(MODULES)/v9958-font-stub.bin $(MODULES)/monitor.zex
else ifeq ($(ZOS_M8_FALLBACK),nohid)
# Failure variant: the required set minus hid.zmod, so the kernel's HID
# load takes the missing-ROMFS-entry path (-ZOS_ENOENT) and the console
# falls back to SIO input with the V9958 output.
ROMFS_MANIFEST := tests/m8/manifest-fallback-nohid.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex
else ifeq ($(ZOS_M8_FALLBACK),nohid-nov9958)
# Failure variant: HID and V9958 both absent; only the SIO fallback and
# the IOC transport survive, and the console binds SIO/SIO.
ROMFS_MANIFEST := tests/m8/manifest-fallback-nohid-nov9958.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex
else ifeq ($(ZOS_M8_FALLBACK),badioc)
# Failure variant: a malformed IOC interface (fixture device with a null
# command operation) makes HID init fail and roll back while the real
# V9958 still loads; the console binds SIO/V9958.
ROMFS_MANIFEST := tests/m8/manifest-hid-badioc.json
ROMFS_EXTRA_DEPS := $(MODULES)/fakeioc.zmod $(MODULES)/monitor.zex
else ifeq ($(ZOS_M8_FALLBACK),arena)
# Failure variant: the v9958.zmod entry carries an oversized BSS filler,
# so the last boot module fails allocation (ZMOD_ENOSPC) after SIO, IOC
# and HID are resident; the console binds HID/SIO.  The declared boot
# set (BOOT_ZMODS) keeps the real v9958 bytes for the build-time arena
# plan; this image intentionally exercises the runtime allocation
# failure, which the plan cannot express.
ROMFS_MANIFEST := tests/m8/manifest-fallback-arena.json
ROMFS_EXTRA_DEPS := $(MODULES)/m8regfill.zmod $(MODULES)/monitor.zex
else
ROMFS_MANIFEST := recovery/manifest.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex
endif

$(BUILD)/recovery.romfs: $(ROMFS_MANIFEST) $(MODULES)/sio.zmod \
		$(MODULES)/ioc.zmod $(MODULES)/hid.zmod $(MODULES)/v9958.zmod \
		$(MODULES)/v9958-6x8.font $(ROMFS_EXTRA_DEPS) \
		tools/romfspack.py $(ZOS_PLATFORM_MEMORY_INC) | $(BUILD)
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
		$(ZOS_M6_FAILURE_SELFTEST) $(ZOS_M7_SELFTEST) \
		$(ZOS_M8_IOC_SELFTEST) $(ZOS_M8_HID_SELFTEST) \
		$(ZOS_M8_HID_NOIOC) $(ZOS_M8_HID_BADIOC) \
		$(ZOS_M8_V9958_SELFTEST) $(ZOS_M8_V9958_FONTFAIL) \
		$(ZOS_M8_CONSOLE_SELFTEST) $(ZOS_M8_SELFTEST) \
		$(ZOS_M8_FALLBACK_SELFTEST))
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

# --- M8 IOC tooling, acceptance image, and hardware smoke --------------------
# ioc-test proves the packed command+bulk module and the source/constant
# contracts on the host; m8-ioc-selftest builds the production-geometry
# acceptance image (SIO + IOC from ROMFS) and replays it in the simulator
# against the IOC command+bulk peer; the smoke target drives the flashed
# image over SIO0/B at 115200 8N1.

ioc-test: $(MODULES)/ioc.zmod
	$(PYTHON) tools/zmodpack.py validate $(MODULES)/ioc.zmod
	$(PYTHON) tools/zmodpack.py dump $(MODULES)/ioc.zmod
	$(PYTHON) -m unittest discover -s tests -p 'test_ioc.py' -v
	@echo "ioc-test: OK"

m8-ioc-selftest:
	$(MAKE) BUILD=build-m8-ioc-selftest ZOS_M8_IOC_SELFTEST=1 \
		ZOS_SMOKE_HOST_SYNC=1 \
		build-m8-ioc-selftest/zephyros-m8-ioc-selftest.bin \
		build-m8-ioc-selftest/zephyr80-m8-ioc-selftest.bin
	$(PYTHON) tools/mapcheck.py \
		build-m8-ioc-selftest/zephyros.map \
		build-m8-ioc-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_ioc_sim.py' -v
	@echo "m8-ioc image: build-m8-ioc-selftest/zephyros-m8-ioc-selftest.bin"

# --- M8 HID tooling, acceptance images, and hardware smoke -------------------
# hid-test proves the packed HID module and the class/source contracts on
# the host; m8-hid-selftest builds the production-geometry acceptance
# image (SIO + IOC + HID from ROMFS) and replays it in the simulator;
# m8-hid-noioc-selftest builds and replays the dependency-absence image
# (IOC boot load disabled: HID is rolled back and SIO remains the console
# fallback); m8-hid-badioc-selftest builds and replays the
# malformed-interface image (fake IOC with a null command operation).
# The smoke target drives the flashed HID image over SIO0/B and scripts
# the SIO fallback byte; the USB-keyboard pass is typed by the operator.

hid-test: $(MODULES)/hid.zmod
	$(PYTHON) tools/zmodpack.py validate $(MODULES)/hid.zmod
	$(PYTHON) tools/zmodpack.py dump $(MODULES)/hid.zmod
	$(PYTHON) -m unittest discover -s tests -p 'test_hid.py' -v
	@echo "hid-test: OK"

m8-hid-selftest:
	$(MAKE) BUILD=build-m8-hid-selftest ZOS_M8_HID_SELFTEST=1 \
		ZOS_SMOKE_HOST_SYNC=1 \
		build-m8-hid-selftest/zephyros-m8-hid-selftest.bin \
		build-m8-hid-selftest/zephyr80-m8-hid-selftest.bin
	$(PYTHON) tools/mapcheck.py \
		build-m8-hid-selftest/zephyros.map \
		build-m8-hid-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_hid_sim.py' -v
	@echo "m8-hid image: build-m8-hid-selftest/zephyros-m8-hid-selftest.bin"

m8-hid-noioc-selftest:
	$(MAKE) BUILD=build-m8-hid-noioc-selftest ZOS_M8_HID_NOIOC=1 \
		ZOS_SMOKE_HOST_SYNC=1 \
		build-m8-hid-noioc-selftest/zephyros-m8-hid-noioc-selftest.bin \
		build-m8-hid-noioc-selftest/zephyr80-m8-hid-noioc-selftest.bin
	$(PYTHON) tools/mapcheck.py \
		build-m8-hid-noioc-selftest/zephyros.map \
		build-m8-hid-noioc-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_hid_sim.py' -v
	@echo "m8-hid no-IOC image: build-m8-hid-noioc-selftest/zephyros-m8-hid-noioc-selftest.bin"

m8-hid-badioc-selftest:
	$(MAKE) BUILD=build-m8-hid-badioc-selftest ZOS_M8_HID_BADIOC=1 \
		ZOS_SMOKE_HOST_SYNC=1 \
		build-m8-hid-badioc-selftest/zephyros-m8-hid-badioc-selftest.bin \
		build-m8-hid-badioc-selftest/zephyr80-m8-hid-badioc-selftest.bin
	$(PYTHON) tools/mapcheck.py \
		build-m8-hid-badioc-selftest/zephyros.map \
		build-m8-hid-badioc-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_hid_sim.py' -v
	@echo "m8-hid bad-IOC image: build-m8-hid-badioc-selftest/zephyros-m8-hid-badioc-selftest.bin"

# --- M8 V9958 video tooling, acceptance images, and hardware smoke -----------
# v9958-test proves the packed module, the font asset pipeline and the
# class/platform/source contracts on the host; m8-v9958-selftest builds
# the production-geometry acceptance image (SIO + IOC + HID + V9958
# from ROMFS) and replays it in the simulator against the V9958 peer;
# m8-v9958-fontfail-selftest builds and replays the resource-failure
# image (the ROMFS font entry is an undersized stub).

v9958-test: $(MODULES)/v9958.zmod $(MODULES)/v9958-6x8.font
	$(PYTHON) tools/zmodpack.py validate $(MODULES)/v9958.zmod
	$(PYTHON) tools/zmodpack.py dump $(MODULES)/v9958.zmod
	$(PYTHON) tools/fontpack.py verify drivers/v9958/assets/font_cp850_6x8.inc
	$(PYTHON) -m unittest discover -s tests -p 'test_v9958.py' -v
	$(PYTHON) -m unittest discover -s tests -p 'test_v9958_console.py' -v
	@echo "v9958-test: OK"

m8-v9958-selftest:
	$(MAKE) BUILD=build-m8-v9958-selftest ZOS_M8_V9958_SELFTEST=1 \
		ZOS_SMOKE_HOST_SYNC=1 \
		build-m8-v9958-selftest/zephyros-m8-v9958-selftest.bin \
		build-m8-v9958-selftest/zephyr80-m8-v9958-selftest.bin
	$(PYTHON) tools/mapcheck.py \
		build-m8-v9958-selftest/zephyros.map \
		build-m8-v9958-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_v9958_sim.py' -v
	@echo "m8-v9958 image: build-m8-v9958-selftest/zephyros-m8-v9958-selftest.bin"

m8-v9958-fontfail-selftest:
	$(MAKE) BUILD=build-m8-v9958-fontfail-selftest ZOS_M8_V9958_FONTFAIL=1 \
		ZOS_SMOKE_HOST_SYNC=1 \
		build-m8-v9958-fontfail-selftest/zephyros-m8-v9958-fontfail-selftest.bin \
		build-m8-v9958-fontfail-selftest/zephyr80-m8-v9958-fontfail-selftest.bin
	$(PYTHON) tools/mapcheck.py \
		build-m8-v9958-fontfail-selftest/zephyros.map \
		build-m8-v9958-fontfail-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_v9958_sim.py' -v
	@echo "m8-v9958 font-failure image: build-m8-v9958-fontfail-selftest/zephyros-m8-v9958-fontfail-selftest.bin"

# The M8 console acceptance image (package 19) boots the full production
# stack, reports the selected console input/output classes over the
# direct early SIO, and runs the unchanged M7 monitor through the
# HID-input/V9958-output binding.  The serial portion proves boot, the
# renderer char interface and the binding; the visible monitor session
# is replayed from these exact bytes by tests/test_m8_sim.py and typed
# by the operator on hardware.
m8-console-selftest:
	$(MAKE) BUILD=build-m8-console-selftest ZOS_M8_CONSOLE_SELFTEST=1 \
		ZOS_SMOKE_HOST_SYNC=1 \
		build-m8-console-selftest/zephyros-m8-console-selftest.bin \
		build-m8-console-selftest/zephyr80-m8-console-selftest.bin
	$(PYTHON) tools/mapcheck.py \
		build-m8-console-selftest/zephyros.map \
		build-m8-console-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_m8_sim.py' -v
	@echo "m8-console image: build-m8-console-selftest/zephyros-m8-console-selftest.bin"

# --- M8 integration acceptance and fallback matrix (package 20) --------------
# m8-selftest is the final acceptance image: the full production stack
# plus the integration target diagnostic and the unchanged monitor
# launcher.  tests/test_m8_acceptance.py replays the production bytes
# (build/zephyros.bin) and these bytes: boot order, binding, IOC
# command/status/bulk sequencing, HID queue traffic, the V9958 trace,
# the split console, the scripted monitor session with two exit/relaunch
# cycles, post-fault recovery and the malformed-call invariants.
#
# m8-fallback-selftests builds every observable failure variant plus
# the accepted font-failure image and replays the matrix
# (tests/test_m8_fallback.py); each variant's exact binding, early-SIO
# diagnostic and direction are asserted from the real linked bytes.

m8-selftest: all
	$(MAKE) BUILD=build-m8-selftest ZOS_M8_SELFTEST=1 ZOS_SMOKE_HOST_SYNC=1 \
		build-m8-selftest/zephyros-m8-selftest.bin \
		build-m8-selftest/zephyr80-m8-selftest.bin
	$(PYTHON) tools/mapcheck.py \
		build-m8-selftest/zephyros.map \
		build-m8-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_m8_acceptance.py' -v
	@echo "m8 integration image: build-m8-selftest/zephyros-m8-selftest.bin"

m8-fallback-selftests: all
	$(MAKE) BUILD=build-m8-fb-nohid ZOS_M8_FALLBACK_SELFTEST=1 \
		ZOS_M8_FALLBACK=nohid ZOS_SMOKE_HOST_SYNC=1 \
		build-m8-fb-nohid/zephyros-m8-fb-nohid.bin \
		build-m8-fb-nohid/zephyr80-m8-fb-nohid.bin
	$(MAKE) BUILD=build-m8-fb-nohid-nov9958 ZOS_M8_FALLBACK_SELFTEST=1 \
		ZOS_M8_FALLBACK=nohid-nov9958 ZOS_SMOKE_HOST_SYNC=1 \
		build-m8-fb-nohid-nov9958/zephyros-m8-fb-nohid-nov9958.bin \
		build-m8-fb-nohid-nov9958/zephyr80-m8-fb-nohid-nov9958.bin
	$(MAKE) BUILD=build-m8-fb-badioc ZOS_M8_FALLBACK_SELFTEST=1 \
		ZOS_M8_FALLBACK=badioc ZOS_SMOKE_HOST_SYNC=1 \
		build-m8-fb-badioc/zephyros-m8-fb-badioc.bin \
		build-m8-fb-badioc/zephyr80-m8-fb-badioc.bin
	$(MAKE) BUILD=build-m8-fb-arena ZOS_M8_FALLBACK_SELFTEST=1 \
		ZOS_M8_FALLBACK=arena ZOS_SMOKE_HOST_SYNC=1 \
		build-m8-fb-arena/zephyros-m8-fb-arena.bin \
		build-m8-fb-arena/zephyr80-m8-fb-arena.bin
	$(MAKE) BUILD=build-m8-v9958-fontfail-selftest ZOS_M8_V9958_FONTFAIL=1 \
		ZOS_SMOKE_HOST_SYNC=1 \
		build-m8-v9958-fontfail-selftest/zephyros-m8-v9958-fontfail-selftest.bin \
		build-m8-v9958-fontfail-selftest/zephyr80-m8-v9958-fontfail-selftest.bin
	$(MAKE) BUILD=build-m8-hid-noioc-selftest ZOS_M8_HID_NOIOC=1 \
		ZOS_SMOKE_HOST_SYNC=1 \
		build-m8-hid-noioc-selftest/zephyros-m8-hid-noioc-selftest.bin \
		build-m8-hid-noioc-selftest/zephyr80-m8-hid-noioc-selftest.bin
	$(MAKE) BUILD=build-m8-hid-badioc-selftest ZOS_M8_HID_BADIOC=1 \
		ZOS_SMOKE_HOST_SYNC=1 \
		build-m8-hid-badioc-selftest/zephyros-m8-hid-badioc-selftest.bin \
		build-m8-hid-badioc-selftest/zephyr80-m8-hid-badioc-selftest.bin
	$(PYTHON) tools/mapcheck.py build-m8-fb-nohid/zephyros.map \
		build-m8-fb-nohid/geometry/memory.inc $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) tools/mapcheck.py build-m8-fb-nohid-nov9958/zephyros.map \
		build-m8-fb-nohid-nov9958/geometry/memory.inc $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) tools/mapcheck.py build-m8-fb-badioc/zephyros.map \
		build-m8-fb-badioc/geometry/memory.inc $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) tools/mapcheck.py build-m8-fb-arena/zephyros.map \
		build-m8-fb-arena/geometry/memory.inc $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_m8_fallback.py' -v
	@echo "m8 fallback matrix images:"
	@echo "  build-m8-fb-nohid/zephyros-m8-fb-nohid.bin"
	@echo "  build-m8-fb-nohid-nov9958/zephyros-m8-fb-nohid-nov9958.bin"
	@echo "  build-m8-fb-badioc/zephyros-m8-fb-badioc.bin"
	@echo "  build-m8-fb-arena/zephyros-m8-fb-arena.bin"
	@echo "  build-m8-v9958-fontfail-selftest/zephyros-m8-v9958-fontfail-selftest.bin"
	@echo "  build-m8-selftest/zephyros-m8-selftest.bin + build/zephyros.bin (all-modules row)"

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

smoke-hardware-m8-ioc:
	$(PYTHON) tools/serial_smoke.py --mode m8-ioc --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m8-hid:
	$(PYTHON) tools/serial_smoke.py --mode m8-hid --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m8-v9958:
	$(PYTHON) tools/serial_smoke.py --mode m8-v9958 --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m8-console:
	$(PYTHON) tools/serial_smoke.py --mode m8-console --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

# Package-20 modes: the full integration image (serial portion: early
# diagnostics, binding report and the target selftest; the interactive
# monitor session over HID/V9958 is the human gate) and the four
# observable failure variants, each asserting its exact binding.  The
# host sync is sent while the banner is pending, as for the other M8
# smoke modes.

smoke-hardware-m8:
	$(PYTHON) tools/serial_smoke.py --mode m8 --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m8-fb-nohid:
	$(PYTHON) tools/serial_smoke.py --mode m8-fb-nohid --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m8-fb-nohid-nov9958:
	$(PYTHON) tools/serial_smoke.py --mode m8-fb-nohid-nov9958 --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m8-fb-badioc:
	$(PYTHON) tools/serial_smoke.py --mode m8-fb-badioc --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m8-fb-arena:
	$(PYTHON) tools/serial_smoke.py --mode m8-fb-arena --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))
