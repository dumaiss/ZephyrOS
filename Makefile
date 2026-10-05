# ZephyrOS build
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
# The drivers are not linked into the kernel: each is packed as a ZMOD
# module (build/modules/*.zmod), stored in the ROMFS recovery image
# (build/recovery.romfs, pages 2-7 of the 512 KiB ROM), and loaded at boot
# from /SYSTEM through the VFS and the loader's bounded read-source
# adapter.  There is one boot path: every image derives its driver arena
# from the aligned linked kernel end and boots from the recovery store.
#
#   make          build the ROM images under build/
#   make test     run host-side image/layout tests and integrity checks
#   make check    reproducible rebuild + full host validation stack
#   make romfs-test           ROMFS image + host ROMFS tool tests
#   make zmod-test            packed-module tooling tests
#   make zex-test             packed-program tooling tests
#   make subsystem-selftest   current-path mechanism acceptance image
#   make romfs-corrupt-selftest  corrupt-recovery-store failure image
#   make m8-*-selftest        M8 device/console acceptance images
#   make m9-vfs-selftest      M9 VFS acceptance image
#   make m9-provider-selftest M9 writable-provider acceptance image
#   make m9-fs-selftest       M9 filesystem syscall acceptance image
#   make clean    remove all generated files

SDCC      ?= sdcc
SDASZ80   ?= sdasz80
SDLDZ80   ?= sdldz80
PYTHON    ?= python3

BUILD     := build
.DEFAULT_GOAL := all

# M9 VFS mount policy (work/M9-REFERENCE.md).  ZOS_MOUNTS selects the one
# manifest per image; acceptance images point it at tests/m9/mounts-*.json.
# ZOS_VFS_BUILTIN names the filesystem providers statically linked into the
# kernel (the registry/vs-generator invariant is checked by test_mountgen.py).
ZOS_MOUNTS      ?= config/mounts.json
ZOS_VFS_BUILTIN ?= romfs

# Zephyr-80 platform build configuration (include paths and the
# declarative constant files consumed by the build and layout tools).
include platform/zephyr80/config.mk

ASFLAGS   := $(ZOS_PLATFORM_ASFLAGS) -Itests/target/include
# Kernel and acceptance sources include their private interfaces by
# explicit owning path (kernel/...); the module build (MOD_CFLAGS) does
# not get this path, so a ZMOD cannot reach a kernel-private header.
CFLAGS    := -mz80 -c --nostdinc --Werror $(ZOS_SDCC_OPT) \
             $(ZOS_PLATFORM_CFLAGS) -I .

ASM_OBJS  := $(BUILD)/reset.rel $(BUILD)/start.rel $(BUILD)/banking.rel \
             $(BUILD)/page_zero.rel $(BUILD)/syscall_entry.rel \
             $(BUILD)/irq_entry.rel $(BUILD)/irq_vectors.rel \
             $(BUILD)/platform_irq.rel $(BUILD)/sdcc_helpers.rel \
             $(BUILD)/arena.rel $(BUILD)/rom_access.rel \
             $(BUILD)/process_launch.rel $(BUILD)/user_copy.rel \
             $(BUILD)/diag_io.rel
# M9 VFS core, generated mount table and statically linked ROMFS provider.
M9_VFS_OBJS := $(BUILD)/vfs_mounts.rel $(BUILD)/vfs_path.rel \
               $(BUILD)/vfs_mount.rel $(BUILD)/vfs_file.rel \
               $(BUILD)/vfs_dir.rel $(BUILD)/vfs_mutate.rel \
               $(BUILD)/vfs_process.rel \
               $(BUILD)/romfs_provider.rel
C_OBJS    := $(BUILD)/early_console.rel $(BUILD)/panic.rel $(BUILD)/main.rel \
             $(BUILD)/syscall.rel $(BUILD)/diag.rel $(BUILD)/fs.rel \
             $(BUILD)/irq.rel \
             $(BUILD)/irq_sources.rel $(BUILD)/registry.rel \
             $(BUILD)/console.rel \
             $(BUILD)/module_relocate.rel $(BUILD)/module_loader.rel \
             $(BUILD)/romfs.rel $(BUILD)/process.rel $(BUILD)/process_exec.rel \
             $(M9_VFS_OBJS)
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
# Subsystem acceptance image: the current production boot path (derived
# arena, VFS plus ROMFS recovery store, SIO-only boot set) with the
# relocated mechanism selftests linked in.  One image carries the syscall,
# IRQ, loader, ROMFS-reader and process/ZEX acceptance surfaces, so no
# per-milestone acceptance image and no per-milestone build exists
# (work/00-SUPERVISOR.md retention rule).
ifeq ($(ZOS_SUBSYS_SELFTEST),1)
CFLAGS    += -DZOS_SUBSYS_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/syscall_test.rel $(BUILD)/syscall_selftest_asm.rel \
                    $(BUILD)/irq_test.rel $(BUILD)/irq_selftest_asm.rel \
                    $(BUILD)/ctc_test.rel \
                    $(BUILD)/romfs_test.rel $(BUILD)/loader_test.rel \
                    $(BUILD)/process_test.rel
PK_SUFFIX := -subsys-selftest
endif

# Subsystem corrupt-store image: the same current boot path, but the
# packed ROMFS header metadata is deliberately corrupted, so the kernel
# must reject the whole store through the early-console panic before any
# module code runs.
ifeq ($(ZOS_ROMFS_CORRUPT_SELFTEST),1)
CFLAGS    += -DZOS_ROMFS_CORRUPT_SELFTEST=1
PK_SUFFIX := -romfs-corrupt
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

# M9 VFS bootstrap acceptance image (package 25): production-style VFS
# boot -- ROMFS mounted at /SYSTEM by vfs_init(), the four resident modules
# and the monitor loaded through VFS paths, the normal mount pass consuming
# a test manifest whose optional mount names an unregistered provider (so it
# fails, is diagnosed, and leaves the namespace usable) -- then the M9 target
# VFS selftest.  The ROMFS contents stay the production set.
ifeq ($(ZOS_M9_VFS_SELFTEST),1)
CFLAGS    += -DZOS_M9_VFS_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/m9_vfs_test.rel
PK_SUFFIX := -m9-vfs-selftest
ZOS_MOUNTS := tests/m9/mounts-optional-fail.json
endif

# M9 required-mount failure image (package 25): the same production boot,
# but the configured non-system mount is `required: true` and names an
# unregistered provider, so the normal mount pass must diagnose it and
# panic before the monitor launcher runs (architecture section 32.32.4).
ifeq ($(ZOS_M9_REQUIRED_MOUNT),1)
ZOS_MOUNTS := tests/m9/mounts-required-fail.json
PK_SUFFIX := -m9-required-mount
endif

# M9 writable-provider acceptance image (package 26): the test-only ramfs
# ZMOD registers through driver ABI v3 and the normal mount pass attaches it
# at `/` (required) and `/R2` (optional).  The boot set is SIO-only: the
# console falls back to SIO and the freed arena granules hold the bounded
# RAM provider and the registration fixture modules.  The target selftest
# covers registration rollback, every mutation semantic, the root merge and
# the size/seek cases.
ifeq ($(ZOS_M9_PROVIDER_SELFTEST),1)
CFLAGS    += -DZOS_M9_PROVIDER_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/m9_provider_test.rel
PK_SUFFIX := -m9-provider-selftest
ZOS_MOUNTS := tests/m9/mounts-ramfs.json
ZOS_IOC_BOOT := 0
ZOS_HID_BOOT := 0
ZOS_V9958_BOOT := 0
endif

# M9 filesystem syscall acceptance image (package 27): the SIO-only boot
# set plus the test-only ramfs provider mounted at `/` (required) and `/R2`
# (optional); the native `fstest.zex` runs the public FS_* family, then the
# launcher-cleanup program is exec'd a second time to prove the descriptor
# pool and CWD were released.  The production boot set (IOC/HID/V9958) is
# compiled out so the derived arena holds the RAM provider.
ifeq ($(ZOS_M9_FS_SELFTEST),1)
CFLAGS    += -DZOS_M9_FS_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/m9_fs_test.rel
PK_SUFFIX := -m9-fs-selftest
ZOS_MOUNTS := tests/m9/mounts-ramfs.json
ZOS_IOC_BOOT := 0
ZOS_HID_BOOT := 0
ZOS_V9958_BOOT := 0
endif

# Hardware-smoke host sync (kernel/main.c): the M8 IOC smoke image waits
# for the host's two-byte sync sequence on SIO0/B before the banner,
# because this bench's reset line also resets the USB bridge.  Only
# diagnostic images enable it; production and every other acceptance
# image compile the gate out byte-identically.
ifeq ($(ZOS_SMOKE_HOST_SYNC),1)
CFLAGS    += -DZOS_SMOKE_HOST_SYNC=1
endif

# IOC command-lane boot load (kernel/main.c): production and the M8
# images load ioc.zmod from the recovery store after the SIO fallback;
# the pre-M8 acceptance images do not carry the module and keep their
# accepted boot byte-for-byte by compiling the block out.
# The subsystem image boots the SIO fallback only, so its loader fixtures
# have the arena to themselves.
ifeq ($(ZOS_SUBSYS_SELFTEST),1)
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
ifeq ($(ZOS_SUBSYS_SELFTEST),1)
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
ifeq ($(ZOS_SUBSYS_SELFTEST),1)
ZOS_V9958_BOOT := 0
endif
ZOS_V9958_BOOT ?= 1
CFLAGS    += -DZOS_V9958_BOOT=$(ZOS_V9958_BOOT)

OBJS      := $(ASM_OBJS) $(C_OBJS)

# --- driver-arena geometry ---------------------------------------------------
# Every image derives the arena and the runtime data base from the aligned
# linked-kernel end with a two-pass link.  There is one geometry; the probe
# link places no arena.

# Common acceptance wiring: the single guard plus the shared runner.
ifneq ($(filter 1,$(ZOS_SUBSYS_SELFTEST) \
	$(ZOS_M8_IOC_SELFTEST) $(ZOS_M8_HID_SELFTEST) \
	$(ZOS_M8_HID_NOIOC) $(ZOS_M8_HID_BADIOC) \
	$(ZOS_M8_V9958_SELFTEST) $(ZOS_M8_V9958_FONTFAIL) \
	$(ZOS_M8_CONSOLE_SELFTEST) $(ZOS_M8_SELFTEST) \
	$(ZOS_M8_FALLBACK_SELFTEST) $(ZOS_M9_VFS_SELFTEST) \
	$(ZOS_M9_PROVIDER_SELFTEST) $(ZOS_M9_FS_SELFTEST)),)
CFLAGS    += -DZOS_TARGET_TEST_BUILD=1 -Itests/target/include
TARGET_TEST_OBJS := $(BUILD)/target_test.rel $(TARGET_TEST_OBJS)
endif

# Production images (no acceptance objects) must reject every acceptance
# symbol in the linked map; mapcheck --production enforces this.
MAPCHECK_FLAGS := $(if $(TARGET_TEST_OBJS),,--production)

# Driver-arena floor.  The 32-granule floor is the production budget gate
# (work/M9-REFERENCE.md); an acceptance image links target-test code and only
# needs an arena that holds its declared boot set, which the arena plan
# (zmodpack plan) enforces separately.
ZOS_ARENA_MIN_GRANULES := $(if $(TARGET_TEST_OBJS),16,32)

# Every image boots the SIO fallback module from the recovery store and
# then, per its boot gates, the IOC transport, the HID keyboard/text input
# module and the V9958 video core.  (MODULES is defined below this point,
# so keep these recursive assignments.)
BOOT_ZMOD = $(MODULES)/sio.zmod
BOOT_IOC_ZMOD ?= $(MODULES)/ioc.zmod
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


IMAGE := $(BUILD)/zephyros$(PK_SUFFIX).bin
MAME_IMAGE := $(BUILD)/zephyr80$(PK_SUFFIX).bin

# Memory-layout source for this build: a full memory.inc variant is
# generated from the probe map, so every assembly unit (and mapcheck) sees
# the derived driver arena.
GEOMETRY_DIR       := $(BUILD)/geometry
GEOMETRY_PROBE_DIR := $(BUILD)/probe
GEOMETRY_INC       := $(GEOMETRY_DIR)/arena_geometry.inc

GEOMETRY_MEMORY := $(GEOMETRY_DIR)/memory.inc
MEMORY_INC := $(GEOMETRY_MEMORY)

# --- driver-arena geometry generation --------------------------------------------
# kernel/module/arena.asm always includes the generated
# arena_geometry.inc.  The build first links a probe image (the same object
# list with a bootstrap arena object that places no arena), measures the
# loadable areas and the runtime-data size in its map, then regenerates the
# include and the memory.inc variant before the real link.  The final map is
# re-verified by mapcheck.py, so a stale generated value cannot survive.

$(GEOMETRY_INC) $(GEOMETRY_MEMORY): $(BUILD)/zephyros.probe.map \
		tools/mapcheck.py $(ZOS_PLATFORM_MEMORY_INC) | $(BUILD)
	mkdir -p $(GEOMETRY_DIR)
	$(PYTHON) tools/mapcheck.py geometry derive $(BUILD)/zephyros.probe.map \
		$(ZOS_PLATFORM_MEMORY_INC) $(GEOMETRY_INC) $(GEOMETRY_MEMORY) \
		--min-granules $(ZOS_ARENA_MIN_GRANULES)

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

.PHONY: all test check check-fast clean zmod-test romfs-test zex-test \
	subsystem-selftest romfs-corrupt-selftest \
	monitor-test ioc-test m8-ioc-selftest hid-test \
	m8-hid-selftest m8-hid-noioc-selftest m8-hid-badioc-selftest \
	v9958-test m8-v9958-selftest m8-v9958-fontfail-selftest \
	m8-console-selftest \
	m8-selftest m8-fallback-selftests \
	m9-vfs-selftest \
	m9-required-mount-selftest \
	m9-provider-selftest \
	m9-fs-selftest \
	hardware-images \
	smoke-hardware \
	smoke-hardware-subsystem smoke-hardware-romfs-corrupt \
	smoke-hardware-m8-ioc smoke-hardware-m8-hid \
	smoke-hardware-m8-v9958 smoke-hardware-m8-console \
	smoke-hardware-m8 smoke-hardware-m8-fb-nohid \
	smoke-hardware-m8-fb-nohid-nov9958 smoke-hardware-m8-fb-badioc \
	smoke-hardware-m8-fb-arena \
	smoke-hardware-m9-fs

all: $(IMAGE) $(MAME_IMAGE)

$(BUILD):
	mkdir -p $@

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

$(BUILD)/irq_code_end.rel: kernel/irq/code_end.asm | $(BUILD)
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

# --- M5 ROMFS objects (bank-7 reader + common ROM-access primitive) ----------

$(BUILD)/romfs.rel: kernel/romfs/romfs.c include/zephyros/romfs.h \
		include/zephyros/module.h include/zephyros/platform.h \
		platform/zephyr80/memory.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# --- M9 VFS objects (bank-7 core + ROMFS provider + generated mount table) ---

$(BUILD)/vfs_path.rel: kernel/vfs/path.c kernel/vfs/vfs.h \
		include/zephyros/fs.h include/zephyros/vfs.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/vfs_mount.rel: kernel/vfs/mount.c kernel/vfs/vfs.h \
		kernel/vfs/mount_config.h include/zephyros/fs.h \
		include/zephyros/vfs.h fs/romfs/romfs_provider.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/vfs_file.rel: kernel/vfs/file.c kernel/vfs/vfs.h \
		include/zephyros/fs.h include/zephyros/vfs.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/vfs_dir.rel: kernel/vfs/dir.c kernel/vfs/vfs.h \
		include/zephyros/fs.h include/zephyros/vfs.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/vfs_mutate.rel: kernel/vfs/mutate.c kernel/vfs/vfs.h \
		include/zephyros/fs.h include/zephyros/vfs.h \
		include/zephyros/process.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/vfs_process.rel: kernel/vfs/process.c kernel/vfs/vfs.h \
		include/zephyros/fs.h include/zephyros/vfs.h \
		include/zephyros/process.h include/zephyros/errno.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/fs.rel: kernel/syscall/fs.c kernel/syscall/fs.h \
		kernel/syscall/diag.h kernel/vfs/vfs.h \
		include/zephyros/syscall.h include/zephyros/fs.h \
		include/zephyros/process.h include/zephyros/errno.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/romfs_provider.rel: fs/romfs/romfs_provider.c \
		fs/romfs/romfs_provider.h kernel/vfs/vfs.h \
		include/zephyros/romfs.h include/zephyros/vfs.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# The mount table is generated in the build tree only (never committed) and
# regenerated whenever the manifest, the generator, or the Makefile variables
# change; its content is exactly the reviewed manifest.
$(BUILD)/generated/vfs_mounts.c $(BUILD)/generated/vfs_mounts.json: \
		$(ZOS_MOUNTS) tools/mountgen.py Makefile | $(BUILD)
	mkdir -p $(BUILD)/generated
	$(PYTHON) tools/mountgen.py $(ZOS_MOUNTS) \
		--builtin "$(ZOS_VFS_BUILTIN)" \
		--out $(BUILD)/generated/vfs_mounts.c \
		--report $(BUILD)/generated/vfs_mounts.json

$(BUILD)/vfs_mounts.rel: $(BUILD)/generated/vfs_mounts.c \
		kernel/vfs/mount_config.h | $(BUILD)
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
# Every module sees the shared include/ and platform/ surfaces plus its own
# driver directory context; no sibling driver's private path is offered here.
# A driver's local headers resolve through same-directory quote includes.
MOD_CFLAGS := -mz80 -c --nostdinc --Werror $(ZOS_SDCC_OPT) \
              -I include -I platform -I drivers/sio

$(MODULES):
	mkdir -p $@

# Declared boot-set arena plan: per-module image+BSS/granules and the
# used/free totals; the build fails here when the declared boot set cannot
# coexist in the derived arena.  The plan also carries the bank-7 ledger.
ARENA_PLAN := $(BUILD)/arena.plan.json
$(ARENA_PLAN): $(BOOT_ZMODS) tools/zmodpack.py $(MEMORY_INC) | $(BUILD)
	$(PYTHON) tools/zmodpack.py plan --geometry $(MEMORY_INC) \
		--report $@ $(BOOT_ZMODS)

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
		include/zephyros/ioc_protocol.h \
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
# (no imported IOC-module symbol); the compile-time CMD/RSP_HID_INPUT
# vocabulary it needs is the client-visible service contract shared with
# the protocol-aware target diagnostics (include/zephyros/ioc_protocol.h).
# It never includes the IOC driver's private header.

$(MODULES)/hid.rel: drivers/hid/hid.c drivers/hid/hid_driver.h \
		include/zephyros/ioc_protocol.h include/zephyros/hid.h \
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

# --- M9 writable-provider fixtures (test only) ---

$(MODULES)/ramfs.rel: tests/modules/ramfs.c \
		include/zephyros/driver.h include/zephyros/vfs.h \
		include/zephyros/fs.h include/zephyros/errno.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

$(MODULES)/m9_probe.rel: tests/modules/m9_probe.c \
		include/zephyros/driver.h include/zephyros/vfs.h \
		include/zephyros/errno.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

$(MODULES)/m9_dup.rel: tests/modules/m9_dup.c \
		include/zephyros/driver.h include/zephyros/vfs.h \
		include/zephyros/errno.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

$(MODULES)/m9_fail.rel: tests/modules/m9_fail.c \
		include/zephyros/driver.h include/zephyros/vfs.h \
		include/zephyros/errno.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

$(MODULES)/%.zmod: tests/modules/%.json $(MODULES)/%.rel \
		$(MODULES)/sdcc_indirect_calls.rel tools/zmodpack.py
	$(PYTHON) tools/zmodpack.py pack $< $@ --objdir $(MODULES)

FIXTURE_ZMODS := $(MODULES)/m4_probe.zmod $(MODULES)/m4_fail.zmod \
		$(MODULES)/m4_filler.zmod $(MODULES)/m8_api.zmod

# --- target acceptance objects (tests/target/, acceptance builds only) -------

$(BUILD)/target_test.rel: tests/target/support/target_test.c \
		include/zephyros/kernel.h tests/target/include/syscall_selftest.h \
		tests/target/include/irq_selftest.h \
		tests/target/include/loader_selftest.h \
		tests/target/include/romfs_selftest.h \
		tests/target/include/process_selftest.h \
		tests/target/include/m8_hid_selftest.h \
		tests/target/include/m8_v9958_selftest.h \
		tests/target/include/m8_console_selftest.h \
		tests/target/include/m8_selftest.h \
		tests/target/include/m8_fallback_selftest.h \
		tests/target/include/m9_selftest.h \
		tests/target/include/m9_provider_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# Test-owned CTC0 programming helper (acceptance images only): linked in
# place of the retired production helper so no acceptance-only CTC code
# reaches the production map.
$(BUILD)/ctc_test.rel: tests/target/support/ctc_test.c \
		tests/target/include/ctc_test.h \
		platform/zephyr80/hardware.h platform/zephyr80/irq.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/syscall_test.rel: tests/target/syscall/syscall_test.c \
		include/zephyros/kernel.h \
		tests/target/include/syscall_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/syscall_selftest_asm.rel: tests/target/syscall/syscall_entry.asm \
		tests/target/include/syscall_entry.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/irq_test.rel: tests/target/irq/irq_test.c \
		include/zephyros/irq.h include/zephyros/kernel.h \
		include/zephyros/platform.h \
		tests/target/include/irq_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/irq_selftest_asm.rel: tests/target/irq/irq_entry.asm \
		tests/target/include/irq_entry.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/romfs_test.rel: tests/target/romfs/romfs_test.c \
		include/zephyros/romfs.h include/zephyros/module.h \
		include/zephyros/console.h include/zephyros/kernel.h \
		tests/target/include/romfs_selftest.h $(BUILD)/romfs_expect.inc | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/loader_test.rel: tests/target/loader/loader_test.c \
		include/zephyros/module.h include/zephyros/kernel.h \
		include/zephyros/device.h include/zephyros/irq.h \
		include/zephyros/console.h include/zephyros/vfs.h \
		tests/target/include/loader_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/process_test.rel: tests/target/process/process_test.c \
		include/zephyros/process.h include/zephyros/syscall.h \
		include/zephyros/console.h include/zephyros/module.h \
		include/zephyros/kernel.h \
		tests/target/include/process_selftest.h $(BUILD)/process_expect.inc | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# The IOC transport diagnostic is the one explicitly test-only rule allowed
# a private IOC include path: it exercises the READY/DONE admission state,
# the bulk-arming class gate, and the unknown-class reply, which are
# transport-private by design.
$(BUILD)/m8_ioc_test.rel: tests/target/m8/m8_ioc_test.c \
		include/zephyros/device.h include/zephyros/ioc.h \
		include/zephyros/kernel.h drivers/ioc/ioc_proto.h \
		include/zephyros/ioc_protocol.h \
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
		include/zephyros/video.h include/zephyros/ioc_protocol.h \
		tests/target/include/m8_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m8_fallback_test.rel: tests/target/m8/m8_fallback_test.c \
		include/zephyros/console.h include/zephyros/device.h \
		include/zephyros/kernel.h \
		tests/target/include/m8_fallback_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m9_vfs_test.rel: tests/target/m9/m9_vfs_test.c \
		include/zephyros/fs.h include/zephyros/vfs.h \
		include/zephyros/romfs.h include/zephyros/module.h \
		include/zephyros/console.h include/zephyros/kernel.h \
		kernel/vfs/vfs.h kernel/vfs/mount_config.h \
		tests/target/include/m9_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m9_provider_test.rel: tests/target/m9/m9_provider_test.c \
		include/zephyros/fs.h include/zephyros/vfs.h \
		include/zephyros/module.h include/zephyros/console.h \
		include/zephyros/kernel.h \
		kernel/vfs/vfs.h kernel/vfs/mount_config.h \
		tests/target/include/m9_provider_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m9_fs_test.rel: tests/target/m9/m9_fs_test.c \
		include/zephyros/process.h include/zephyros/console.h \
		include/zephyros/kernel.h include/zephyros/fs.h \
		kernel/vfs/vfs.h \
		tests/target/include/m9_fs_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# --- M6 native program (ZEXTEST) ---------------------------------------------
# One ZEX program linked as a flat image at 0100h: _CODE is the image and
# _DATA is the zero-initialized region the loader fills at
# 0100h+image_size.  Geometry comes from tests/subsys/zextest.json; zexpack
# validates the linked map against it and writes the reproducible report.

ZEX_TEST_JSON := tests/subsys/zextest.json
ZEX_TEST_BUILD := $(BUILD)/zextest
ZEX_TEST_IMAGE_SIZE := $(shell $(PYTHON) tools/zexpack.py get $(ZEX_TEST_JSON) image_size)
ZEX_TEST_DATA_BASE  := $(shell printf '0x%04X' $$((0x0100 + $(ZEX_TEST_IMAGE_SIZE))))

$(ZEX_TEST_BUILD):
	mkdir -p $@

$(BUILD)/process_expect.inc: $(ZEX_TEST_JSON) tests/subsys/gen_process_expect.py \
		tests/subsys/gen_zex_fixtures.py tools/zexpack.py | $(BUILD)
	$(PYTHON) tests/subsys/gen_process_expect.py $(ZEX_TEST_JSON) $@

$(ZEX_TEST_BUILD)/zextest_entry.rel: tests/target/process/zextest_entry.asm | $(ZEX_TEST_BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(ZEX_TEST_BUILD)/zex_runtime.rel: user/runtime/zex_runtime.asm | $(ZEX_TEST_BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(ZEX_TEST_BUILD)/zextest.rel: tests/target/process/zextest.c user/runtime/zex_runtime.h \
		tests/target/include/process_selftest.h $(BUILD)/process_expect.inc | $(ZEX_TEST_BUILD)
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

# --- M9 native filesystem acceptance programs (FSTEST.ZEX) --------------------
# fstest.zex (the FS_* acceptance suite) and fstest_l.zex (the second-exec
# launcher-cleanup check) are ordinary ZEX programs linked from the shared
# user runtime plus tests/target/m9/fstest.  The runtime objects are linked
# first so their manifest-checked low-window scratch stays at the _DATA base.

FSTEST_JSON := tests/target/m9/fstest/fstest.json
FSTEST_BUILD := $(BUILD)/fstest
FSTEST_IMAGE_SIZE := $(shell $(PYTHON) tools/zexpack.py get $(FSTEST_JSON) image_size)
FSTEST_DATA_BASE  := $(shell printf '0x%04X' $$((0x0100 + $(FSTEST_IMAGE_SIZE))))

FSTEST_L_JSON := tests/target/m9/fstest/fstest_l.json
FSTEST_L_BUILD := $(BUILD)/fstest_l
FSTEST_L_IMAGE_SIZE := $(shell $(PYTHON) tools/zexpack.py get $(FSTEST_L_JSON) image_size)
FSTEST_L_DATA_BASE  := $(shell printf '0x%04X' $$((0x0100 + $(FSTEST_L_IMAGE_SIZE))))

FSTEST_USER_CFLAGS := -mz80 -c --nostdinc --Werror \
	-I include -I . -I user/runtime -I tests/target/include

$(FSTEST_BUILD) $(FSTEST_L_BUILD):
	mkdir -p $@

$(FSTEST_BUILD)/fstest_entry.rel: tests/target/m9/fstest/fstest_entry.asm | $(FSTEST_BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(FSTEST_BUILD)/zex_runtime.rel: user/runtime/zex_runtime.asm | $(FSTEST_BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(FSTEST_BUILD)/zex_fs_runtime.rel: user/runtime/zex_fs_runtime.asm | $(FSTEST_BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(FSTEST_BUILD)/zos_fs.rel: user/runtime/zos_fs.c user/runtime/zex_fs_runtime.h \
		user/runtime/zex_runtime.h include/zephyros/fs.h \
		include/zephyros/syscall.h | $(FSTEST_BUILD)
	$(SDCC) $(FSTEST_USER_CFLAGS) $< -o $@

$(FSTEST_BUILD)/fstest.rel: tests/target/m9/fstest/fstest.c \
		user/runtime/zex_fs_runtime.h user/runtime/zex_runtime.h \
		include/zephyros/fs.h | $(FSTEST_BUILD)
	$(SDCC) $(FSTEST_USER_CFLAGS) $< -o $@

$(FSTEST_BUILD)/fstest.lk: $(FSTEST_BUILD)/fstest_entry.rel \
		$(FSTEST_BUILD)/zex_runtime.rel \
		$(FSTEST_BUILD)/zex_fs_runtime.rel $(FSTEST_BUILD)/zos_fs.rel \
		$(FSTEST_BUILD)/fstest.rel $(FSTEST_JSON) | $(FSTEST_BUILD)
	{ \
	  printf '%s\n' '-n' '-m' '-w'; \
	  printf '%s\n' '-i $(FSTEST_BUILD)/fstest.ihx'; \
	  printf '%s\n' '-b _CODE = 0x0100' '-b _DATA = $(FSTEST_DATA_BASE)'; \
	  printf '%s\n' $(FSTEST_BUILD)/fstest_entry.rel \
	                $(FSTEST_BUILD)/zex_runtime.rel \
	                $(FSTEST_BUILD)/zex_fs_runtime.rel \
	                $(FSTEST_BUILD)/zos_fs.rel \
	                $(FSTEST_BUILD)/fstest.rel; \
	  printf '%s\n' '-e'; \
	} > $@

$(FSTEST_BUILD)/fstest.ihx $(FSTEST_BUILD)/fstest.map: $(FSTEST_BUILD)/fstest.lk
	$(SDLDZ80) -f $(FSTEST_BUILD)/fstest.lk

$(MODULES)/fstest.zex: $(FSTEST_JSON) $(FSTEST_BUILD)/fstest.ihx \
		$(FSTEST_BUILD)/fstest.map tools/zexpack.py | $(MODULES)
	$(PYTHON) tools/zexpack.py pack $(FSTEST_JSON) $@ \
		--objdir $(FSTEST_BUILD) --report $(MODULES)/fstest.report.json

$(FSTEST_L_BUILD)/fstest_l_entry.rel: tests/target/m9/fstest/fstest_l_entry.asm | $(FSTEST_L_BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(FSTEST_L_BUILD)/zex_runtime.rel: user/runtime/zex_runtime.asm | $(FSTEST_L_BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(FSTEST_L_BUILD)/zex_fs_runtime.rel: user/runtime/zex_fs_runtime.asm | $(FSTEST_L_BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(FSTEST_L_BUILD)/zos_fs.rel: user/runtime/zos_fs.c user/runtime/zex_fs_runtime.h \
		user/runtime/zex_runtime.h include/zephyros/fs.h \
		include/zephyros/syscall.h | $(FSTEST_L_BUILD)
	$(SDCC) $(FSTEST_USER_CFLAGS) $< -o $@

$(FSTEST_L_BUILD)/fstest_l.rel: tests/target/m9/fstest/fstest_l.c \
		user/runtime/zex_fs_runtime.h user/runtime/zex_runtime.h \
		include/zephyros/fs.h | $(FSTEST_L_BUILD)
	$(SDCC) $(FSTEST_USER_CFLAGS) $< -o $@

$(FSTEST_L_BUILD)/fstest_l.lk: $(FSTEST_L_BUILD)/fstest_l_entry.rel \
		$(FSTEST_L_BUILD)/zex_runtime.rel \
		$(FSTEST_L_BUILD)/zex_fs_runtime.rel $(FSTEST_L_BUILD)/zos_fs.rel \
		$(FSTEST_L_BUILD)/fstest_l.rel $(FSTEST_L_JSON) | $(FSTEST_L_BUILD)
	{ \
	  printf '%s\n' '-n' '-m' '-w'; \
	  printf '%s\n' '-i $(FSTEST_L_BUILD)/fstest_l.ihx'; \
	  printf '%s\n' '-b _CODE = 0x0100' '-b _DATA = $(FSTEST_L_DATA_BASE)'; \
	  printf '%s\n' $(FSTEST_L_BUILD)/fstest_l_entry.rel \
	                $(FSTEST_L_BUILD)/zex_runtime.rel \
	                $(FSTEST_L_BUILD)/zex_fs_runtime.rel \
	                $(FSTEST_L_BUILD)/zos_fs.rel \
	                $(FSTEST_L_BUILD)/fstest_l.rel; \
	  printf '%s\n' '-e'; \
	} > $@

$(FSTEST_L_BUILD)/fstest_l.ihx $(FSTEST_L_BUILD)/fstest_l.map: $(FSTEST_L_BUILD)/fstest_l.lk
	$(SDLDZ80) -f $(FSTEST_L_BUILD)/fstest_l.lk

$(MODULES)/fstest_l.zex: $(FSTEST_L_JSON) $(FSTEST_L_BUILD)/fstest_l.ihx \
		$(FSTEST_L_BUILD)/fstest_l.map tools/zexpack.py | $(MODULES)
	$(PYTHON) tools/zexpack.py pack $(FSTEST_L_JSON) $@ \
		--objdir $(FSTEST_L_BUILD) --report $(MODULES)/fstest_l.report.json

# Deterministic malformed containers (one per validation boundary; see
# tests/subsys/gen_zex_fixtures.py and gen_zmod_fixtures.py) shared by the
# subsystem acceptance manifest.
$(MODULES)/zex-fixtures.stamp: tests/subsys/gen_zex_fixtures.py tools/zexpack.py | $(MODULES)
	$(PYTHON) tests/subsys/gen_zex_fixtures.py $(MODULES)
	$(PYTHON) tests/subsys/gen_zmod_fixtures.py $(MODULES)
	touch $@

# --- ROMFS recovery store (ROM pages 2-7) ------------------------------------
# Every image packs a manifest; the subsystem acceptance image adds the
# loader/ROMFS/ZEX fixtures, and the corrupt-store image patches the packed
# production image so the kernel's validation is exercised on target.
ifeq ($(ZOS_SUBSYS_SELFTEST),1)
ROMFS_MANIFEST := tests/subsys/manifest.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex $(MODULES)/romfs_crosspage.bin \
		$(MODULES)/m4_probe.zmod $(MODULES)/m4_fail.zmod \
		$(MODULES)/m4_filler.zmod $(MODULES)/m8_api.zmod \
		$(MODULES)/zextest.zex $(MODULES)/zex-fixtures.stamp
else ifeq ($(ZOS_ROMFS_CORRUPT_SELFTEST),1)
ROMFS_MANIFEST := recovery/manifest.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex tests/subsys/corrupt_romfs.py
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
else ifeq ($(ZOS_M9_PROVIDER_SELFTEST),1)
# SIO-only boot: the SIO fallback module, the writable RAM provider, the
# registration fixtures and the monitor.  No IOC/HID/V9958 entries, so the
# derived arena holds the bounded RAM provider.
ROMFS_MANIFEST := tests/m9/manifest-provider.json
ROMFS_EXTRA_DEPS := $(MODULES)/ramfs.zmod $(MODULES)/m9_probe.zmod \
		$(MODULES)/m9_dup.zmod $(MODULES)/m9_fail.zmod \
		$(MODULES)/monitor.zex
else ifeq ($(ZOS_M9_FS_SELFTEST),1)
# SIO-only boot: the SIO fallback, the RAM provider mounted at `/` and
# `/R2`, the monitor and the two native filesystem acceptance programs.
ROMFS_MANIFEST := tests/m9/manifest-fs.json
ROMFS_EXTRA_DEPS := $(MODULES)/ramfs.zmod $(MODULES)/monitor.zex \
		$(MODULES)/fstest.zex $(MODULES)/fstest_l.zex
else
ROMFS_MANIFEST := recovery/manifest.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex
endif

$(BUILD)/recovery.romfs: $(ROMFS_MANIFEST) $(MODULES)/sio.zmod \
		$(MODULES)/ioc.zmod $(MODULES)/hid.zmod $(MODULES)/v9958.zmod \
		$(MODULES)/v9958-6x8.font $(ROMFS_EXTRA_DEPS) \
		tools/romfspack.py $(ZOS_PLATFORM_MEMORY_INC) | $(BUILD)
ifeq ($(ZOS_ROMFS_CORRUPT_SELFTEST),1)
	$(PYTHON) tools/romfspack.py --memory-inc $(ZOS_PLATFORM_MEMORY_INC) pack \
		$(ROMFS_MANIFEST) $(BUILD)/recovery-clean.romfs --objdir $(MODULES)
	$(PYTHON) tests/subsys/corrupt_romfs.py $(BUILD)/recovery-clean.romfs $@
else
	$(PYTHON) tools/romfspack.py --memory-inc $(ZOS_PLATFORM_MEMORY_INC) pack \
		$(ROMFS_MANIFEST) $@ --objdir $(MODULES)
endif

# Deterministic cross-page pattern asset for the subsystem manifest.
$(MODULES)/romfs_crosspage.bin: tests/subsys/gen_crosspage.py | $(MODULES)
	$(PYTHON) tests/subsys/gen_crosspage.py $@

# Expected-container constants for the ROMFS-reader target selftest: the
# packed size/sum plus the manifest-derived first-payload offset.
$(BUILD)/romfs_expect.inc: tests/subsys/manifest.json $(MODULES)/sio.zmod \
		tests/subsys/gen_romfs_expect.py | $(BUILD)
	$(PYTHON) tests/subsys/gen_romfs_expect.py tests/subsys/manifest.json \
		$(MODULES)/sio.zmod $@ SIO_ZMOD

# --- link --------------------------------------------------------------------
# The canonical `build/` output uses the checked-in linker/zephyr80.lk.
# Every self-test and fallback build (different BUILD dir and/or extra
# objects) generates its linker file from the same object list the link
# step depends on, so the lists cannot drift apart.

ZOS_TEST_LINK := $(filter 1,$(ZOS_SUBSYS_SELFTEST) \
		$(ZOS_ROMFS_CORRUPT_SELFTEST) \
		$(ZOS_M8_IOC_SELFTEST) $(ZOS_M8_HID_SELFTEST) \
		$(ZOS_M8_HID_NOIOC) $(ZOS_M8_HID_BADIOC) \
		$(ZOS_M8_V9958_SELFTEST) $(ZOS_M8_V9958_FONTFAIL) \
		$(ZOS_M8_CONSOLE_SELFTEST) $(ZOS_M8_SELFTEST) \
		$(ZOS_M8_FALLBACK_SELFTEST) $(ZOS_M9_VFS_SELFTEST) \
		$(ZOS_M9_PROVIDER_SELFTEST) $(ZOS_M9_FS_SELFTEST))
ifeq ($(BUILD),build)
ifneq ($(ZOS_TEST_LINK),)
GENERATED_LK := 1
endif
else
GENERATED_LK := 1
endif

ifeq ($(GENERATED_LK),1)
$(BUILD)/zephyr80.lk: $(OBJS) $(TARGET_TEST_OBJS) $(END_OBJ) \
		$(GEOMETRY_MEMORY) | $(BUILD)
	@data_base=$$(sed -n 's/^ZOS_DATA_BASE = //p' $(GEOMETRY_MEMORY)); \
	test -n "$$data_base" || { echo "no derived data base in $(GEOMETRY_MEMORY)"; exit 1; }; \
	{ \
	  printf '%s\n' '-n' '-m' '-w'; \
	  printf '%s\n' '-i $(BUILD)/zephyros.ihx'; \
	  printf '%s\n' '-b _CODE = 0x2000' "-b _DATA = $$data_base" \
	                '-b _COMMON = 0xE000'; \
	  printf '%s\n' $(OBJS) $(TARGET_TEST_OBJS) $(END_OBJ); \
	  printf '%s\n' '-e'; \
	} > $@

$(BUILD)/zephyros.ihx $(BUILD)/zephyros.map: $(OBJS) $(TARGET_TEST_OBJS) \
		$(END_OBJ) $(BUILD)/zephyr80.lk $(GEOMETRY_MEMORY) | $(BUILD)
	$(SDLDZ80) -f $(BUILD)/zephyr80.lk
else
# Canonical build: the checked-in linker template supplies the production
# object list; the derived data base is substituted into build/zephyr80.lk.
$(BUILD)/zephyr80.lk: linker/zephyr80.lk $(GEOMETRY_MEMORY) | $(BUILD)
	@data_base=$$(sed -n 's/^ZOS_DATA_BASE = //p' $(GEOMETRY_MEMORY)); \
	test -n "$$data_base" || { echo "no derived data base in $(GEOMETRY_MEMORY)"; exit 1; }; \
	sed "s/@DATA_BASE@/$$data_base/" linker/zephyr80.lk > $@

$(BUILD)/zephyros.ihx $(BUILD)/zephyros.map: $(BUILD)/zephyr80.lk \
		$(OBJS) $(END_OBJ) | $(BUILD)
	$(SDLDZ80) -f $(BUILD)/zephyr80.lk
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
	$(PYTHON) tools/mapcheck.py $(MAPCHECK_FLAGS) \
		--min-granules $(ZOS_ARENA_MIN_GRANULES) $(BUILD)/zephyros.map \
		$(MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) tools/pack_image.py $(BUILD)/zephyros.ihx $(BUILD)/zephyros.map \
		$(BUILD) "$(PK_SUFFIX)" $(MEMORY_INC) \
		--min-granules $(ZOS_ARENA_MIN_GRANULES)

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

# --- subsystem acceptance image ----------------------------------------------
# One current-boot-path image carries every relocated mechanism selftest: the
# syscall gate and the IRQ executive (early hook), then the ROMFS reader, the
# process/ZEX lifecycle and the loader (late hook), and finally the monitor
# launcher's scripted session.  The declared boot set is the SIO fallback
# only, so the loader's relocation, rollback and arena-exhaustion checks own
# the derived arena (work/25a-retire-pre-m8-acceptance/WORK.md).

subsystem-selftest:
	$(MAKE) BUILD=build-subsys-selftest ZOS_SUBSYS_SELFTEST=1 \
		build-subsys-selftest/zephyros-subsys-selftest.bin \
		build-subsys-selftest/zephyr80-subsys-selftest.bin
	$(PYTHON) tools/mapcheck.py --min-granules 16 \
		build-subsys-selftest/zephyros.map \
		build-subsys-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_subsys_sim.py' -v
	@echo "subsystem-selftest image: build-subsys-selftest/zephyros-subsys-selftest.bin"

# Corrupt-recovery-store image: the production manifest, packed and then
# patched so the kernel's validation rejects the whole store through the
# early-console panic before any module code runs.

romfs-corrupt-selftest:
	$(MAKE) BUILD=build-romfs-corrupt ZOS_ROMFS_CORRUPT_SELFTEST=1 \
		build-romfs-corrupt/zephyros-romfs-corrupt.bin \
		build-romfs-corrupt/zephyr80-romfs-corrupt.bin
	$(PYTHON) tools/mapcheck.py \
		build-romfs-corrupt/zephyros.map \
		build-romfs-corrupt/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_romfs_sim.py' -v
	@echo "romfs-corrupt image: build-romfs-corrupt/zephyros-romfs-corrupt.bin"

check-fast:
	$(PYTHON) -m unittest discover -s tests -v
	$(PYTHON) tools/mapcheck.py $(MAPCHECK_FLAGS) \
		--min-granules $(ZOS_ARENA_MIN_GRANULES) $(BUILD)/zephyros.map \
		$(MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) tools/pack_image.py $(BUILD)/zephyros.ihx $(BUILD)/zephyros.map \
		$(BUILD) "$(PK_SUFFIX)" $(MEMORY_INC) \
		--min-granules $(ZOS_ARENA_MIN_GRANULES)
	sha256sum $(IMAGE) $(MAME_IMAGE)
	cmp $(IMAGE) $(MAME_IMAGE)
	@echo "make check: OK"

clean:
	rm -rf $(BUILD) build-*

# --- host-side ZMOD tooling tests -------------------------------------------
# Builds the module containers and the synthetic loader fixtures with the
# real toolchain, validates them, then runs the host container suite
# (fixture-dependent tests skip when the packed modules are absent).

zmod-test: $(MODULES)/sio.zmod $(MODULES)/sio-irq.zmod \
		$(MODULES)/sio-fail.zmod $(FIXTURE_ZMODS)
	$(PYTHON) tools/zmodpack.py validate $(MODULES)/sio.zmod \
		$(MODULES)/sio-irq.zmod $(MODULES)/sio-fail.zmod
	$(PYTHON) tools/zmodpack.py validate $(FIXTURE_ZMODS)
	$(PYTHON) tools/zmodpack.py dump $(MODULES)/sio.zmod
	$(PYTHON) -m unittest discover -s tests -p 'test_zmod.py' -v
	@echo "zmod-test: OK"


romfs-test: $(BUILD)/recovery.romfs
	$(PYTHON) tools/romfspack.py --memory-inc $(ZOS_PLATFORM_MEMORY_INC) \
		validate $(BUILD)/recovery.romfs
	$(PYTHON) tools/romfspack.py --memory-inc $(ZOS_PLATFORM_MEMORY_INC) \
		dump $(BUILD)/recovery.romfs
	$(PYTHON) -m unittest discover -s tests -p 'test_romfs.py' -v
	@echo "romfs-test: OK"

zex-test: $(MODULES)/zextest.zex $(MODULES)/zex-fixtures.stamp
	$(PYTHON) tools/zexpack.py validate $(MODULES)/zextest.zex
	$(PYTHON) tools/zexpack.py dump $(MODULES)/zextest.zex
	$(PYTHON) -m unittest discover -s tests -p 'test_zex.py' -v
	@echo "zex-test: OK"

monitor-test: $(MODULES)/monitor.zex
	$(PYTHON) tools/zexpack.py validate $(MODULES)/monitor.zex
	$(PYTHON) tools/zexpack.py dump $(MODULES)/monitor.zex
	$(PYTHON) -m unittest discover -s tests -p 'test_monitor.py' -v
	@echo "monitor-test: OK"

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
	$(PYTHON) tools/mapcheck.py --min-granules 16 \
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
	$(PYTHON) tools/mapcheck.py --min-granules 16 \
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
	$(PYTHON) tools/mapcheck.py --min-granules 16 \
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
	$(PYTHON) tools/mapcheck.py --min-granules 16 \
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
	$(PYTHON) tools/mapcheck.py --min-granules 16 \
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
	$(PYTHON) tools/mapcheck.py --min-granules 16 \
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
	$(PYTHON) tools/mapcheck.py --min-granules 16 \
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
	$(PYTHON) tools/mapcheck.py --min-granules 16 \
		build-m8-selftest/zephyros.map \
		build-m8-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_m8_acceptance.py' -v
	@echo "m8 integration image: build-m8-selftest/zephyros-m8-selftest.bin"

# --- M9 VFS bootstrap acceptance image ---------------------------------------
# Production-style VFS boot: ROMFS at /SYSTEM, the four resident modules and
# the monitor loaded through VFS, the optional-mount failure diagnosed, then
# the target VFS selftest replayed in the simulator.

m9-vfs-selftest: all
	$(MAKE) BUILD=build-m9-vfs-selftest ZOS_M9_VFS_SELFTEST=1 \
		build-m9-vfs-selftest/zephyros-m9-vfs-selftest.bin \
		build-m9-vfs-selftest/zephyr80-m9-vfs-selftest.bin
	$(PYTHON) tools/mapcheck.py --min-granules 16 build-m9-vfs-selftest/zephyros.map \
		build-m9-vfs-selftest/geometry/memory.inc $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_m9_vfs_sim.py' -v
	@echo "m9 vfs image: build-m9-vfs-selftest/zephyros-m9-vfs-selftest.bin"

# M9 required-mount failure image: the normal mount pass meets a required
# non-system mount whose provider is absent; boot must diagnose it and panic
# (tests/test_m9_vfs_sim.py TestM9RequiredMount).

m9-required-mount-selftest: all
	$(MAKE) BUILD=build-m9-required-mount-selftest ZOS_M9_REQUIRED_MOUNT=1 \
		build-m9-required-mount-selftest/zephyros-m9-required-mount.bin \
		build-m9-required-mount-selftest/zephyr80-m9-required-mount.bin
	$(PYTHON) tools/mapcheck.py --production \
		--min-granules $(ZOS_ARENA_MIN_GRANULES) \
		build-m9-required-mount-selftest/zephyros.map \
		build-m9-required-mount-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_m9_vfs_sim.py' -v
	@echo "m9 required-mount image: build-m9-required-mount-selftest/zephyros-m9-required-mount.bin"

# M9 writable-provider acceptance image: SIO-only boot, the test RAM
# filesystem mounted at `/` (required) and `/R2` (optional), the
# registration fixtures loaded by the target boot hook, and the provider
# selftest (registration rollback, mutations, root merge, sizes, seek).

m9-provider-selftest: all
	$(MAKE) BUILD=build-m9-provider-selftest ZOS_M9_PROVIDER_SELFTEST=1 \
		build-m9-provider-selftest/zephyros-m9-provider-selftest.bin \
		build-m9-provider-selftest/zephyr80-m9-provider-selftest.bin
	$(PYTHON) tools/mapcheck.py --min-granules 16 \
		build-m9-provider-selftest/zephyros.map \
		build-m9-provider-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_m9_provider_sim.py' -v
	@echo "m9 provider image: build-m9-provider-selftest/zephyros-m9-provider-selftest.bin"

# M9 filesystem syscall acceptance image: SIO-only boot, the test RAM
# filesystem at `/` and `/R2`, and the two native acceptance programs.  The
# target hook launches fstest.zex, checks the pool is clean, then exec's the
# launcher-cleanup program; the simulator replay pins the transcript.

m9-fs-selftest: all
	$(MAKE) BUILD=build-m9-fs-selftest ZOS_M9_FS_SELFTEST=1 \
		build-m9-fs-selftest/zephyros-m9-fs-selftest.bin \
		build-m9-fs-selftest/zephyr80-m9-fs-selftest.bin
	$(PYTHON) tools/mapcheck.py --min-granules 16 \
		build-m9-fs-selftest/zephyros.map \
		build-m9-fs-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_syscall_sim.py' -v
	$(PYTHON) -m unittest discover -s tests -p 'test_m9_fs.py' -v
	@echo "m9 fs image: build-m9-fs-selftest/zephyros-m9-fs-selftest.bin"

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
	$(PYTHON) tools/mapcheck.py --min-granules 16 build-m8-fb-nohid/zephyros.map \
		build-m8-fb-nohid/geometry/memory.inc $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) tools/mapcheck.py --min-granules 16 build-m8-fb-nohid-nov9958/zephyros.map \
		build-m8-fb-nohid-nov9958/geometry/memory.inc $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) tools/mapcheck.py --min-granules 16 build-m8-fb-badioc/zephyros.map \
		build-m8-fb-badioc/geometry/memory.inc $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) tools/mapcheck.py --min-granules 16 build-m8-fb-arena/zephyros.map \
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
# Requires the board connected at 115200 8N1 on SIO0/B and the matching image
# programmed.  Pass SERIAL_PORT=/dev/ttyUSB0 (or export ZOS_SERIAL_PORT).
#
# Each smoke mode checks ONE specific image, and the production image and the
# acceptance images have different serial transcripts.  Build the image for
# the mode, flash it, then run the mode:
#
#   mode                          image to flash                              build with
#   smoke-hardware                build/zephyros.bin                          make
#   smoke-hardware-subsystem      build-subsys-selftest/zephyros-subsys-selftest.bin
#                                                                             make subsystem-selftest
#   smoke-hardware-romfs-corrupt  build-romfs-corrupt/zephyros-romfs-corrupt.bin
#                                                                             make romfs-corrupt-selftest
#   smoke-hardware-m8-ioc         build-m8-ioc-selftest/zephyros-m8-ioc-selftest.bin
#                                                                             make m8-ioc-selftest
#   smoke-hardware-m8-hid         build-m8-hid-selftest/zephyros-m8-hid-selftest.bin
#                                                                             make m8-hid-selftest
#   smoke-hardware-m8-v9958       build-m8-v9958-selftest/zephyros-m8-v9958-selftest.bin
#                                                                             make m8-v9958-selftest
#   smoke-hardware-m8-console     build-m8-console-selftest/zephyros-m8-console-selftest.bin
#                                                                             make m8-console-selftest
#   smoke-hardware-m8             build-m8-selftest/zephyros-m8-selftest.bin  make m8-selftest
#   smoke-hardware-m8-fb-*        build-m8-fb-*/zephyros-m8-fb-*.bin          make m8-fallback-selftests
#
# `make hardware-images` builds every image the smoke modes need.  The M8
# images wait for the tool's host-sync sequence, so start the smoke tool and
# press RESET (the tool re-sends the sequence while the banner is pending).
# Each target below prints the exact image and sha256 to flash and fails with
# the build command when that image is missing.

# $(1) = image path, $(2) = target that builds it.
define smoke_preflight
if [ ! -f "$(1)" ]; then echo "smoke: $(1) is missing - build it first: make $(2)"; exit 1; fi; printf 'smoke: image to flash: %s\n       sha256: %s\n' "$(1)" "$$(sha256sum '$(1)' | cut -d' ' -f1)"
endef

hardware-images: all subsystem-selftest romfs-corrupt-selftest \
		m8-selftest m8-ioc-selftest m8-hid-selftest \
		m8-v9958-selftest m8-console-selftest m8-fallback-selftests \
		m9-fs-selftest
	@echo "hardware-images: every smoke image is built under build*/"

smoke-hardware:
	@$(call smoke_preflight,build/zephyros.bin,all)
	$(PYTHON) tools/serial_smoke.py --mode normal --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-subsystem:
	@$(call smoke_preflight,build-subsys-selftest/zephyros-subsys-selftest.bin,subsystem-selftest)
	$(PYTHON) tools/serial_smoke.py --mode subsystem --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-romfs-corrupt:
	@$(call smoke_preflight,build-romfs-corrupt/zephyros-romfs-corrupt.bin,romfs-corrupt-selftest)
	$(PYTHON) tools/serial_smoke.py --mode romfs-corrupt --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m8-ioc:
	@$(call smoke_preflight,build-m8-ioc-selftest/zephyros-m8-ioc-selftest.bin,m8-ioc-selftest)
	$(PYTHON) tools/serial_smoke.py --mode m8-ioc --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m8-hid:
	@$(call smoke_preflight,build-m8-hid-selftest/zephyros-m8-hid-selftest.bin,m8-hid-selftest)
	$(PYTHON) tools/serial_smoke.py --mode m8-hid --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m8-v9958:
	@$(call smoke_preflight,build-m8-v9958-selftest/zephyros-m8-v9958-selftest.bin,m8-v9958-selftest)
	$(PYTHON) tools/serial_smoke.py --mode m8-v9958 --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m8-console:
	@$(call smoke_preflight,build-m8-console-selftest/zephyros-m8-console-selftest.bin,m8-console-selftest)
	$(PYTHON) tools/serial_smoke.py --mode m8-console --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

# Package-20 modes: the full integration image (serial portion: early
# diagnostics, binding report and the target selftest; the interactive
# monitor session over HID/V9958 is the human gate) and the four
# observable failure variants, each asserting its exact binding.  The
# host sync is sent while the banner is pending, as for the other M8
# smoke modes.

smoke-hardware-m8:
	@$(call smoke_preflight,build-m8-selftest/zephyros-m8-selftest.bin,m8-selftest)
	$(PYTHON) tools/serial_smoke.py --mode m8 --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m8-fb-nohid:
	@$(call smoke_preflight,build-m8-fb-nohid/zephyros-m8-fb-nohid.bin,m8-fallback-selftests)
	$(PYTHON) tools/serial_smoke.py --mode m8-fb-nohid --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m8-fb-nohid-nov9958:
	@$(call smoke_preflight,build-m8-fb-nohid-nov9958/zephyros-m8-fb-nohid-nov9958.bin,m8-fallback-selftests)
	$(PYTHON) tools/serial_smoke.py --mode m8-fb-nohid-nov9958 --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m8-fb-badioc:
	@$(call smoke_preflight,build-m8-fb-badioc/zephyros-m8-fb-badioc.bin,m8-fallback-selftests)
	$(PYTHON) tools/serial_smoke.py --mode m8-fb-badioc --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m8-fb-arena:
	@$(call smoke_preflight,build-m8-fb-arena/zephyros-m8-fb-arena.bin,m8-fallback-selftests)
	$(PYTHON) tools/serial_smoke.py --mode m8-fb-arena --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m9-fs:
	@$(call smoke_preflight,build-m9-fs-selftest/zephyros-m9-fs-selftest.bin,m9-fs-selftest)
	$(PYTHON) tools/serial_smoke.py --mode m9-fs --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))
