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
# service image (build/modules/*.zmod), stored in the ROMFS recovery image
# (build/recovery.romfs, pages 2-7 of the 512 KiB ROM) and placed in bank 6
# at boot by the resident loader, which reads the store through the fixed
# ROMFS reader.  There is one boot path: every image boots from the
# recovery store, and residency has exactly two tiers (config/
# residency.json).
#
#   make          build the ROM images under build/
#   make test     run host-side image/layout tests and integrity checks
#   make check    reproducible rebuild + full host validation stack
#   make romfs-test           ROMFS image + host ROMFS tool tests
#   make zmod-test            packed-module tooling tests
#   make zex-test             packed-program tooling tests
#   make subsystem-selftest        current-path and ROMFS acceptance image
#   make nucleus-selftest          revision-13 window/nucleus acceptance image
#   make console-integration-selftest  console + IOC/HID/V9958 diagnostics
#   make console-serial-selftest   serial-console image and its failure store
#   make fs-selftest               filesystem outcome acceptance image
#   make xmem-selftest             extended-memory and aperture outcome image
#   make ns-selftest               namespace acceptance image
#   make ipc-selftest              resident-service IPC acceptance image
#   make helloworld-image          SDK demo image (no kernel change)
#   make clean    remove all generated files
#
# Builds are parallel by default (ZOS_JOBS defaults to nproc); use
# `make ZOS_JOBS=1` for the serial build, which produces identical bytes.

SDCC      ?= sdcc
SDASZ80   ?= sdasz80
SDLDZ80   ?= sdldz80
PYTHON    ?= python3

# Parallel by default.  The dependency graph is complete (the package-28
# zex-fixtures stamp declares its loader-fixture prerequisite), so a
# parallel build is race-free; `make ZOS_JOBS=1` reproduces the serial
# build byte for byte.  MAKEFLAGS is exported to recursive image targets.
ZOS_JOBS  ?= $(shell nproc 2>/dev/null || echo 1)
MAKEFLAGS += -j$(ZOS_JOBS)

BUILD     := build
.DEFAULT_GOAL := all

# System build configuration (work/28b-m9-shell-readiness/WORK.md):
# ZOS_BOOT_PROGRAM selects the program the kernel launcher starts.  The
# value is validated against the image's recovery manifest below, before
# kernel/main.c is compiled.
include config/system.mk

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
             $(BUILD)/bank7_end.rel $(BUILD)/rom_access.rel \
             $(BUILD)/process_launch.rel $(BUILD)/user_copy.rel \
             $(BUILD)/window_copy.rel \
             $(BUILD)/upcall.rel $(BUILD)/diag_io.rel \
             $(BUILD)/ipc_asm.rel $(BUILD)/ipc_common.rel \
             $(BUILD)/service_boot.rel
# Handle-pool, path and CWD machinery (42a's reused set) plus the ROMFS
# reader behind `/SYSTEM`.
VFS_OBJS := $(BUILD)/vfs_path.rel \
               $(BUILD)/vfs_process.rel $(BUILD)/vfs_workspace.rel
C_OBJS    := $(BUILD)/early_console.rel $(BUILD)/panic.rel $(BUILD)/main.rel \
             $(BUILD)/syscall.rel $(BUILD)/diag.rel \
             $(BUILD)/xmem.rel \
             $(BUILD)/irq.rel \
             $(BUILD)/irq_sources.rel \
             $(BUILD)/romfs.rel $(BUILD)/process.rel $(BUILD)/process_exec.rel \
             $(BUILD)/name.rel \
             $(VFS_OBJS)
# Resident-service IPC substrate (package 39) and the service loader
# (package 40): production links both from package 41 on.  The substrate
# is fixed-required - the endpoint table, the call machinery, the
# descriptor table and the boot gate execute while the window shows bank 6
# - and the loader's load pass is a cold-half object.  Both lists are
# pinned by config/residency.json and the checked-in linker file.
# Package 42b retires SYS_IPC_LOOKUP with the rest of the `FS_*` front end,
# so the substrate has no syscall-body object left to place.
IPC_SUBSTRATE_OBJS := $(BUILD)/ipc.rel $(BUILD)/ipc_asm.rel \
             $(BUILD)/ipc_common.rel $(BUILD)/service_boot.rel
IPC_COLD_OBJS := $(BUILD)/service_loader.rel
END_OBJ   := $(BUILD)/irq_code_end.rel
# OBJS is assigned after the acceptance/catalog selection below, once
# every conditional object append has been decided.

# --- resident-service IPC selection -------------------------------------
# The M10 resident-service nucleus is the only memory model: revision 13
# at the 6000h split (the 32 KiB mode-11 window with the 16 KiB fixed core
# at 2000h-5FFFh).  ZOS_IPC_SELFTEST builds the IPC acceptance image that
# links the fixed-core substrate of work package 39.
ZOS_IPC_SELFTEST    ?= 0
# The namespace acceptance image (work package 42a, correction 01) links
# the object/namespace family the production switch-over adopts in 42b.
ZOS_NS_SELFTEST     ?= 0
# The revision-13 mode-11 window is 6000h-DFFFh (architecture section
# 36.2); the fixed core is 2000h-5FFFh.  Every image links that way.
ZOS_REV13_WINDOW_BASE := 0x6000

# --- console selection (work package 42e) -------------------------------
# Consoles are selected per build (architecture 36.11, decision 12):
# `console-v9958-hid` is the production CONSOLE (HID input, VIDEO output);
# `console-serial` is the variant with the SERIAL service in both
# directions.  The two drivers are one shared source compiled into two
# images; an image set that has no HID+VIDEO pair (the subsystem, IOC,
# HID, video and IPC images) selects `console-serial` explicitly.
ZOS_CONSOLE ?= console-v9958-hid

# --- target acceptance build selection ---------------------------------
# Acceptance images link the shared runner (tests/target/support/) plus
# the per-milestone test code under tests/target/; production images link
# none of it.  ZOS_TARGET_TEST_BUILD is the single production-side guard
# (kernel/main.c holds only the two TEST-ONLY HOOKs).
TARGET_TEST_OBJS :=
PK_SUFFIX :=
# Subsystem acceptance image: the current production boot path (VFS plus
# the ROMFS recovery store, the SIO and console service images placed by
# the service loader) with the mechanism selftests linked in.  One image
# carries the syscall, IRQ, ROMFS-reader and process/ZEX acceptance
# surfaces, so no per-milestone acceptance image and no per-milestone
# build exists (work/00-SUPERVISOR.md retention rule).
ifeq ($(ZOS_SUBSYS_SELFTEST),1)
CFLAGS    += -DZOS_SUBSYS_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/syscall_test.rel $(BUILD)/syscall_selftest_asm.rel \
                    $(BUILD)/irq_test.rel $(BUILD)/irq_selftest_asm.rel \
                    $(BUILD)/ctc_test.rel \
                    $(BUILD)/romfs_test.rel \
                    $(BUILD)/process_test.rel
PK_SUFFIX := -subsys-selftest
ZOS_SERVICES := sio console-serial
endif

# console integration acceptance image (package 20, work package 42e):
# the full production stack (SIO + IOC + HID + V9958 + the selected
# CONSOLE driver) from ROMFS, the integration target diagnostic reported
# over the direct early SIO, then the ESC c check program, which resets
# the V9958 display, prints its line and hands the console to the
# unchanged M7 `monitor.zex` for the human HID/V9958 session.  This is
# the image the final physical acceptance gate builds and flashes.
# Work package 42d merged the IOC, HID, V9958 and console acceptance
# images into it: the three device diagnostics and the console session
# run in this one image, and tests/test_console_acceptance.py,
# test_ioc_sim.py, test_hid_sim.py and test_v9958_sim.py replay it
# (plus the production image, which no acceptance image may replace).
ifeq ($(ZOS_CONSOLE_INTEGRATION_SELFTEST),1)
CFLAGS    += -DZOS_CONSOLE_INTEGRATION_SELFTEST=1
CFLAGS    += -DZOS_IOC_SELFTEST=1 -DZOS_HID_SELFTEST=1 -DZOS_VIDEO_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/console_integration_test.rel \
                    $(BUILD)/ioc_test.rel $(BUILD)/hid_test.rel \
                    $(BUILD)/video_test.rel
PK_SUFFIX := -console-integration-selftest
ZOS_BOOT_PROGRAM := /SYSTEM/escc.zex
ZOS_SERVICES := sio ioc hid v9958 $(ZOS_CONSOLE)
endif

# console-serial variant (work package 42e): the production image set with
# `console-serial` in place of the production `console-v9958-hid`, built in
# its own directory.  It boots the production-like store and the monitor;
# the replay proves the monitor works on the serial lane.  The service
# loader's evidence rows are linked so the transcript pins the loaded set.
ifeq ($(ZOS_CONSOLE_SERIAL_SELFTEST),1)
CFLAGS    += -DZOS_SERVICE_TEST_EVIDENCE=1
PK_SUFFIX := -console-serial-selftest
ZOS_SERVICES := sio ioc hid v9958 $(ZOS_CONSOLE) iocfat
endif

# Required-console boot-failure variant (work package 42e): the same set and
# services.man as the serial variant, but the ROMFS manifest omits the
# console image.  The loader's lookup fails, and the boot must stop
# explicitly on the early console (36.11) instead of falling back.
ifeq ($(ZOS_CONSOLE_SERIAL_FAIL_SELFTEST),1)
CFLAGS    += -DZOS_SERVICE_TEST_EVIDENCE=1
PK_SUFFIX := -console-serial-fail-selftest
ZOS_SERVICES := sio ioc hid v9958 $(ZOS_CONSOLE) iocfat
endif

# Filesystem outcome image (work package 42b, correction 01).  The
# production boot set (SIO and IOC, so the FS2 transport is the real one)
# plus `iocfat.zmod` as a bank-6 service publishing `/FS`, the monitor and
# two SDK programs: the checks run in `/SYSTEM/fs_selftest.zex`, which
# SYS_EXECs `/SYSTEM/fs_child.zex` for the nested-exec and handle-release
# outcomes.  The simulator's FS2 peer serves the MCU side, so the same
# image runs on hardware against a real SD card.
ifeq ($(ZOS_FS_SELFTEST),1)
PK_SUFFIX := -fs-selftest
ZOS_BOOT_PROGRAM := /SYSTEM/fs_selftest.zex
ZOS_SERVICES := sio ioc console-serial iocfat
endif

# Extended-memory outcome image (work package 43): the production boot
# path with the SIO/console service set and the SDK outcome program
# booting as /SYSTEM/xmem_selftest.zex, which SYS_EXECs
# /SYSTEM/xmem_child.zex from a path in its own far aperture.  The
# aperture is hardware-authoritative: the owner runs this image on the
# board (architecture 37, far code and data rows without ZEX v2).
ifeq ($(ZOS_XMEM_SELFTEST),1)
PK_SUFFIX := -xmem-selftest
ZOS_BOOT_PROGRAM := /SYSTEM/xmem_selftest.zex
ZOS_SERVICES := sio console-serial
endif

# Nucleus-layout acceptance image (work package 38): the production boot
# path with the revision-13 gate and upcall-shim acceptance surface linked
# in.  The smoke caller runs in application bank 0 with the service page
# selected, and a cold-half caller exercises the fixed upcall shim against
# a stub installed in bank 6.  SIO-only boot set, like the subsystem
# image: the test objects own the cold half's slack.
ifeq ($(ZOS_NUCLEUS_SELFTEST),1)
CFLAGS    += -DZOS_NUCLEUS_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/nucleus_test.rel \
                    $(BUILD)/nucleus_selftest_asm.rel \
                    $(BUILD)/ctc_test.rel
PK_SUFFIX := -nucleus-selftest
ZOS_SERVICES := sio console-serial
endif

# IPC substrate acceptance image (work package 39, M10 stage 3; service
# images from package 40).  The production boot path with the fixed-core
# IPC substrate and the resident service loader linked in, the six
# synthetic services packed as real service images in the recovery store
# and loaded from the boot manifest, and the SDK client program booting as
# /SYSTEM/ipc.zex.  The substrate and the boot gate are fixed-required
# (config/residency.json), the loader is cold code, and the stage-4/5
# `moves` objects fill what the loader leaves: the cold half is nearly
# full, so vfs_path and vfs_workspace overflow into the fixed core's
# leftover space, which is the moves tier's documented fallback
# (see config/residency.json).
IPC_DIR   := tests/target/ipc
# The IPC image uses production's placement: its cold override is empty
# (the ipc selection block sets it, and the target passes it to mapcheck),
# and the two-tier classification of config/residency.json decides every
# object.
IPC_COLD_EXTRA_CSV :=

ifeq ($(ZOS_IPC_SELFTEST),1)
CFLAGS    += -DZOS_IPC_SELFTEST=1 -Itests/target/include
ASFLAGS   += -I$(BUILD)
PK_SUFFIX := -ipc-selftest
# The substrate and the loader are production-linked objects now (OBJS
# carries them), so this image adds nothing to the fixed/cold kernel sets:
# listing them again duplicated every symbol in the probe link.
IPC_FIXED_OBJS := $(IPC_SUBSTRATE_OBJS)
# No image-specific placement override: use the production cold set.
COLD_EXTRA_CSV :=
ZOS_SERVICES := sio console-serial
endif

# Namespace and handle acceptance image (work package 42a, correction 01).
# The object/namespace family is production cold code since 42b; this image
# builds it on top of a small boot set (no filesystem driver) so the
# namespace surfaces can be replayed: the placement config/residency.json
# names is production's, and the image's ledgers are production's.
#   * SIO and the console service: the DEVICE provider's real publications
#     and the standard handles need a live `/dev/console`;
#   * no filesystem driver: the `/FS` path is not exercised here;
#   * the client program is an ordinary SDK program that checks its own
#     results and prints them (`tests/target/ns`).
NS_DIR   := tests/target/ns
ifeq ($(ZOS_NS_SELFTEST),1)
CFLAGS    += -Itests/target/include
PK_SUFFIX := -ns-selftest
# sio and console are the real services whose publications the DEVICE
# table carries (`/dev/serial/0`, `/dev/console`); `nsdev` is the test
# double that adds a third type and a second grouping container
# (tests/target/ns/nsdev.c).
ZOS_SERVICES := sio console-serial nsdev
SVC_MANIFEST_DIR := tests/ns/manifests
ZOS_BOOT_PROGRAM := /SYSTEM/ns.zex
# The object family links where production links it (config/residency.json
# carries vfs_ns, vfs_ns_file and object as `cold`); the shared runner and
# the guard the other acceptance images use come with it.
ZOS_VFS_WS_GUARD := 1
# The family itself compiles cold (its production tier), and so do the
# `moves` objects that still hold the handle pool and the path/CWD
# machinery it calls: this image's cold half has the room, and it is the
# moves tier's documented fallback (architecture 36.20).
COLD_EXTRA := vfs_ns vfs_ns_file object vfs_file
COLD_EXTRA_CSV := vfs_ns,vfs_ns_file,object,vfs_file
endif

# SDK helloworld demo image (work/28c-m9-user-sdk): the production boot path
# in its own build directory, booting the SDK hello demo.  There is no
# kernel define and no target test object -- the image differs from
# production only in the boot-program constant and in the recovery
# manifest, which is the point of the demo.
ifeq ($(ZOS_HELLOWORLD_IMAGE),1)
PK_SUFFIX := -helloworld
# The demo's store carries the production console stack and no filesystem
# driver, so the boot manifest must list exactly that set: inheriting the
# production default made the loader fail on the `iocfat.zmod` its
# manifest does not pack (work package 42d).
ZOS_SERVICES := sio ioc hid v9958 $(ZOS_CONSOLE)
endif

# Hardware-smoke host sync (kernel/main.c): the IOC smoke image waits
# for the host's two-byte sync sequence on SIO0/B before the banner,
# because this bench's reset line also resets the USB bridge.  Only
# diagnostic images enable it; production and every other acceptance
# image compile the gate out byte-identically.
ifeq ($(ZOS_SMOKE_HOST_SYNC),1)
CFLAGS    += -DZOS_SMOKE_HOST_SYNC=1
endif

# The resident service set the nucleus boots (kernel/main.c).  Production
# loads them from the recovery store's `services.man` in the order the
# manifest lists; an acceptance image that needs the serial-only fallback
# or leaves out the filesystem driver lists its own set (see the per-image
# blocks below), which is the M8 `ZOS_*_BOOT` gate's replacement.  The
# filesystem driver is one of them since 42b: IOC-FAT is a bank-6 service
# publishing `/FS`.
ZOS_SERVICES ?= sio ioc hid v9958 $(ZOS_CONSOLE) iocfat

OBJS      := $(ASM_OBJS) $(C_OBJS) $(BUILD)/ipc.rel $(IPC_COLD_OBJS) \
             $(BUILD)/vfs_ns.rel $(BUILD)/vfs_ns_file.rel $(BUILD)/object.rel


# Nucleus residency (architecture 36.5, work package 38; two-tier
# classification from package 42c).  Every production image is a
# revision-13 two-region build: the fixed areas (_CODE and friends,
# _DATA/_INITIALIZED) must link inside 2000h-5FFFh, and the objects in
# config/residency.json's `cold_half` list compile into the cold areas
# (_WINDOW / _WINDOW_DATA at 6000h-DFFFh) that the window shows with
# P = 0.  The classification, its tiers and the reasons live in
# config/residency.json; tools/mapcheck.py fails the build when an object
# is missing or when a fixed-required object links at or above 6000h.
#
# Acceptance images compile their target-test objects cold too, so test
# code consumes the cold half's space, never the fixed core's: an image
# with its own cold set passes it to mapcheck with --cold-extra and to
# the codegen through COLD_EXTRA, so both stay in step.
RESIDENCY  := config/residency.json
# Package 42c: the classification is two-tier (fixed-required and cold)
# and places every object.  The namespace family and the VFS core are
# production cold code (the syscall dispatcher and the service loader run
# with P = 0); an acceptance image with its own cold set passes it here
# (the IPC acceptance image does).
COLD_EXTRA ?=
COLD_EXTRA_CSV ?= $(COLD_EXTRA)
ifeq ($(COLD_EXTRA_CSV),)
COLD_STEMS := $(shell $(PYTHON) tools/mapcheck.py residency \
		--cold-stems $(RESIDENCY))
else
COLD_STEMS := $(shell $(PYTHON) tools/mapcheck.py residency \
		--cold-stems $(RESIDENCY) --cold-extra $(COLD_EXTRA_CSV))
endif
# Bank-7 geometry ---------------------------------------------------------
# Every image derives the two runtime-data bases from a probe link: the
# fixed core's from the runtime-data size, the cold half's straight from
# the linked cold code end.  There is one geometry; the probe link places
# no boundary.


# Common acceptance wiring: the single guard plus the shared runner.
ifneq ($(filter 1,$(ZOS_SUBSYS_SELFTEST) \
	$(ZOS_CONSOLE_INTEGRATION_SELFTEST) \
	$(ZOS_FS_SELFTEST) $(ZOS_NUCLEUS_SELFTEST) \
	$(ZOS_IPC_SELFTEST)),)
CFLAGS    += -DZOS_TARGET_TEST_BUILD=1 -DZOS_VFS_WS_GUARD=1 \
		-Itests/target/include
TARGET_TEST_OBJS := $(BUILD)/target_test.rel \
		$(if $(filter 1,$(ZOS_FS_SELFTEST)),,$(BUILD)/service_client.rel) \
		$(TARGET_TEST_OBJS)
endif

# The two-tier placement decides which objects compile into the window
# areas, so a residency change must rebuild them: without this, an object
# stays in the area its previous classification chose and the mapcheck
# rule fires on a stale .rel.
$(C_OBJS) $(COLD_OBJS): $(RESIDENCY)

# The cold placement follows the target-test list: the acceptance objects
# (and the shared service client every M10 driver diagnostic links) are
# compiled into the window areas at 6000h-DFFFh, exactly like the
# production cold half, so test code consumes the cold half's slack and
# never the fixed core's leftover.  This block sits after the list it
# filters.
COLD_OBJS  := $(addprefix $(BUILD)/,$(addsuffix .rel,$(COLD_STEMS)))
ifneq ($(TARGET_TEST_OBJS),)
COLD_OBJS  += $(filter-out $(IPC_FIXED_OBJS) $(TEST_FIXED_OBJS),$(TARGET_TEST_OBJS))
endif
$(COLD_OBJS): CFLAGS += --codeseg WINDOW --dataseg WINDOW_DATA
$(TEST_FIXED_OBJS): CFLAGS += --dataseg WINDOW_DATA

# Production images (no acceptance objects) must reject every acceptance
# symbol in the linked map; mapcheck --production enforces this.
MAPCHECK_FLAGS := $(if $(TARGET_TEST_OBJS),,--production)

# The residency check runs on every image: an acceptance image that links
# its own cold set (the ipc image) passes it through --cold-extra, so the
# placement rule stays enforced for test images too.
RESIDENCY_FLAGS := --residency $(RESIDENCY) \
	$(if $(COLD_EXTRA_CSV),--cold-extra $(COLD_EXTRA_CSV),)
# The geometry derive only understands --residency (the cold set does not
# change the derived bases: it changes which objects link cold).
RESIDENCY_DERIVE_FLAGS := --residency $(RESIDENCY)

IMAGE := $(BUILD)/zephyros$(PK_SUFFIX).bin
MAME_IMAGE := $(BUILD)/zephyr80$(PK_SUFFIX).bin

# Memory-layout source for this build: the platform memory.inc is the one
# declarative source (the two-region split and the stack reservations are
# production constants now), and the derived variant below adds the
# per-image geometry that every assembly unit and layout tool reads.
GEOMETRY_DIR       := $(BUILD)/geometry
GEOMETRY_PROBE_DIR := $(BUILD)/probe
ASM_MEMORY_INC     := $(ZOS_PLATFORM_MEMORY_INC)
GEOMETRY_MEMORY    := $(GEOMETRY_DIR)/memory.inc
MEMORY_INC         := $(GEOMETRY_MEMORY)

# Bank-7 geometry --------------------------------------------------------------
# kernel/memory/bank7_end.asm always includes the generated
# bank7_end.inc.  The build first links a probe image (the same object
# list with a bootstrap boundary object that places nothing), measures the
# loadable areas and the runtime-data size in its map, then regenerates
# the include and the memory.inc variant before the real link.  The final
# map is re-verified by mapcheck.py, so a stale generated value cannot
# survive.

GEOMETRY_INC       := $(GEOMETRY_DIR)/bank7_end.inc

$(GEOMETRY_INC) $(GEOMETRY_MEMORY): $(BUILD)/zephyros.probe.map \
		tools/mapcheck.py $(ASM_MEMORY_INC) $(RESIDENCY) | $(BUILD)
	mkdir -p $(GEOMETRY_DIR)
	$(PYTHON) tools/mapcheck.py geometry derive $(BUILD)/zephyros.probe.map \
		$(ASM_MEMORY_INC) $(GEOMETRY_INC) $(GEOMETRY_MEMORY) \
		$(RESIDENCY_DERIVE_FLAGS)

$(GEOMETRY_PROBE_DIR)/bank7_end.inc: $(ZOS_PLATFORM_MEMORY_INC) \
		tools/mapcheck.py | $(BUILD)
	mkdir -p $(GEOMETRY_PROBE_DIR)
	$(PYTHON) tools/mapcheck.py geometry probe $(ZOS_PLATFORM_MEMORY_INC) $@

$(GEOMETRY_PROBE_DIR)/bank7_end.rel: kernel/memory/bank7_end.asm \
		$(GEOMETRY_PROBE_DIR)/bank7_end.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -I$(GEOMETRY_PROBE_DIR) -o $@ $<

# Probe link: identical objects, a bootstrap boundary that places nothing.
# Kept under build/probe/ so the canonical build/ object list stays exactly
# the committed linker file's list.  Target-test objects (acceptance
# images) are part of the real link and therefore part of the geometry
# probe.
PROBE_OBJS := $(filter-out $(BUILD)/bank7_end.rel,$(OBJS) \
		$(TARGET_TEST_OBJS) $(END_OBJ)) \
		$(GEOMETRY_PROBE_DIR)/bank7_end.rel

$(BUILD)/zephyros.probe.lk: $(PROBE_OBJS) | $(BUILD)
	{ \
	  printf '%s\n' '-n' '-m' '-w'; \
	  printf '%s\n' '-i $(BUILD)/zephyros.probe.ihx'; \
	  printf '%s\n' '-b _CODE = 0x2000' '-b _DATA = 0x5A00' \
	                '-b _WINDOW = $(ZOS_REV13_WINDOW_BASE)' \
	                '-b _WINDOW_DATA = 0xD800' '-b _COMMON = 0xE000'; \
	  printf '%s\n' $(PROBE_OBJS); \
	  printf '%s\n' '-e'; \
	} > $@

$(BUILD)/zephyros.probe.ihx $(BUILD)/zephyros.probe.map: \
		$(BUILD)/zephyros.probe.lk
	$(SDLDZ80) -f $(BUILD)/zephyros.probe.lk

.PHONY: all test check check-fast clean zmod-test romfs-test zex-test \
	monitor-test ioc-test hid-test v9958-test \
	subsystem-selftest nucleus-selftest \
	console-integration-selftest console-serial-selftest \
	fs-selftest ns-selftest ipc-selftest xmem-selftest helloworld-image \
	hardware-images \
	smoke-hardware \
	smoke-hardware-subsystem \
	smoke-hardware-console-integration smoke-hardware-console-serial \
	smoke-hardware-fs

all: $(IMAGE) $(MAME_IMAGE)

$(BUILD):
	mkdir -p $@

# --- assemble ----------------------------------------------------------------

$(BUILD)/reset.rel: boot/shadow_copy.asm \
		platform/zephyr80/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/start.rel: boot/start.asm \
		platform/zephyr80/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/banking.rel: kernel/memory/banking.asm \
		platform/zephyr80/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/page_zero.rel: kernel/syscall/page_zero.asm | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/syscall_entry.rel: kernel/syscall/entry.asm \
		platform/zephyr80/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/irq_entry.rel: kernel/irq/entry.asm platform/zephyr80/hardware.inc \
		$(ASM_MEMORY_INC) platform/zephyr80/irq.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/irq_vectors.rel: kernel/irq/vectors.asm \
		platform/zephyr80/irq.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/platform_irq.rel: kernel/irq/init.asm \
		platform/zephyr80/irq.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/sdcc_helpers.rel: kernel/runtime/sdcc_helpers.asm | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/bank7_end.rel: kernel/memory/bank7_end.asm $(GEOMETRY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -I$(dir $(GEOMETRY_INC)) -o $@ $<

$(BUILD)/rom_access.rel: kernel/romfs/rom_access.asm \
		platform/zephyr80/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/process_launch.rel: kernel/process/launch.asm \
		platform/zephyr80/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/user_copy.rel: kernel/memory/user_copy.asm \
		platform/zephyr80/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/window_copy.rel: kernel/memory/window_copy.asm \
		platform/zephyr80/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/upcall.rel: kernel/memory/upcall.asm \
		platform/zephyr80/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
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
		abi/zos/types.h platform/zephyr80/hardware.h | $(BUILD)
	$(SDCC) $(CFLAGS) --codeseg COMMON $< -o $@

$(BUILD)/panic.rel: kernel/panic.c \
		include/zephyros/kernel.h include/zephyros/platform.h \
		abi/zos/types.h platform/zephyr80/hardware.h | $(BUILD)
	$(SDCC) $(CFLAGS) --codeseg COMMON $< -o $@

$(BUILD)/main.rel: kernel/main.c include/zephyros/kernel.h \
		include/zephyros/irq.h \
 include/zephyros/process.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/irq.rel: kernel/irq/irq.c include/zephyros/irq.h \
		abi/zos/types.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/irq_sources.rel: drivers/ctc/ctc.c \
		include/zephyros/irq.h include/zephyros/platform.h \
		platform/zephyr80/hardware.h platform/zephyr80/irq.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/syscall.rel: kernel/syscall.c kernel/syscall/diag.h \
		abi/zos/syscall.h include/zephyros/errno.h \
		include/zephyros/kernel.h include/zephyros/process.h \
		| $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/diag.rel: kernel/syscall/diag.c kernel/syscall/diag.h \
		abi/zos/syscall.h include/zephyros/process.h \
		include/zephyros/platform.h include/zephyros/errno.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# Extended-memory family (work package 43): the xmem syscall bodies, the
# object table and the aperture token.  Cold code like the rest of the
# syscall dispatcher (P = 0 while it runs).
$(BUILD)/xmem.rel: kernel/syscall/xmem.c kernel/syscall/xmem.h \
		abi/zos/xmem.h abi/zos/syscall.h include/zephyros/process.h \
		include/zephyros/errno.h kernel/ipc/ipc.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# --- M3 device/driver/service objects (bank-7 code and data) -----------------


# --- M4 module loader objects (bank-7 code and data) -------------------------




# --- M5 ROMFS objects (bank-7 reader + common ROM-access primitive) ----------

# Shared kernel ASCII name comparison (package 30c, F3): one helper for the
# namespace router and ROMFS.
$(BUILD)/name.rel: kernel/lib/name.c kernel/lib/name.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/romfs.rel: kernel/romfs/romfs.c include/zephyros/romfs.h \
 include/zephyros/platform.h \
		kernel/lib/name.h platform/zephyr80/memory.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# --- VFS objects (the handle pool, paths and the workspace) -------------------

$(BUILD)/vfs_path.rel: kernel/vfs/path.c kernel/vfs/vfs.h \
		abi/zos/fs.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@





$(BUILD)/vfs_process.rel: kernel/vfs/process.c kernel/vfs/vfs.h \
		abi/zos/fs.h \
		include/zephyros/process.h include/zephyros/errno.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/vfs_workspace.rel: kernel/vfs/workspace.c kernel/vfs/vfs.h \
		abi/zos/fs.h \
		include/zephyros/kernel.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# --- namespace and typed-handle family (work package 42a) -------------------

$(BUILD)/vfs_ns.rel: kernel/vfs/ns.c kernel/vfs/ns.h kernel/vfs/vfs.h \
		abi/zos/object.h include/zephyros/ipc.h include/zephyros/romfs.h \
		include/zephyros/errno.h kernel/ipc/ipc.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/vfs_ns_file.rel: kernel/vfs/ns_file.c kernel/vfs/ns.h \
		kernel/vfs/vfs.h abi/zos/object.h abi/zos/protocol.h \
		include/zephyros/ipc.h include/zephyros/romfs.h \
		include/zephyros/errno.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/object.rel: kernel/syscall/object.c kernel/syscall/object.h \
		kernel/syscall/diag.h kernel/vfs/ns.h \
		abi/zos/object.h abi/zos/ipc.h abi/zos/syscall.h \
		include/zephyros/errno.h include/zephyros/process.h | $(BUILD)
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
# Every image packs its own module directory ($(BUILD)/modules): the packed
# bytes are deterministic, so the same bytes reappear in any placement
# build, and an image that boots a *different* resident-service set or
# carries its own fixtures cannot disturb another image's store.  The
# canonical build keeps the historical build/modules/ path the host tests
# read.  Each image embeds the boot module plus (in self-test builds) the
# fixtures through a generated assembly blob emitted by tools/zmodpack.py.

MODULES   := $(BUILD)/modules
# Every module sees the shared include/ and platform/ surfaces plus its own
# driver directory context; no sibling driver's private path is offered here.
# A driver's local headers resolve through same-directory quote includes.
MOD_CFLAGS := -mz80 -c --nostdinc --Werror $(ZOS_SDCC_OPT) \
              -I abi -I include -I platform -I drivers/sio

$(MODULES):
	mkdir -p $@

# --- bank-6 resident services (work package 41) ------------------------------
# The SIO, IOC, HID, V9958 and console driver stacks are bank-6 services:
# each is packed as a service image (ZMOD module_type 2) into the recovery
# store, listed by the boot manifest `services.man` (ZOS_SERVICES, in load
# order) and placed in bank 6 by the nucleus loader
# (kernel/service/service_loader.c).  Per 36.6 the hot tier is _CODE with
# its BSS in _SERVICE_IO (the one bank-6 range both the mode-11 window and
# the mode-10 aperture show, so descriptors and `move` name it), the boot
# tier is _BOOT/_BOOT_DATA, and the V9958 font is a bulk segment the
# run-time display reset re-reads through the nucleus bulk copy.
#
# The driver cores (sio.c, ioc.c, hid.c, v9958.c) are the same sources the
# M8 modules used; the *_service.c halves own the protocol dispatch and
# init, the *_boot.c halves the boot tier.  Every image is a separate
# object set, so each packed byte is identical across placement builds.
SVC_CFLAGS := -mz80 -c --nostdinc --Werror $(ZOS_SDCC_OPT) \
              -I abi -I include -I platform -I $(BUILD) -I .
SVC_HOT    := $(SVC_CFLAGS) --codeseg CODE --dataseg SERVICE_IO
SVC_BOOT   := $(SVC_CFLAGS) --codeseg BOOT --dataseg BOOT_DATA

# The declared production boot set, in load order: IOC before HID (HID's
# image declares the dependency), the selected CONSOLE driver after the
# services it composes (work package 42e: HID and VIDEO for the production
# build, SERIAL for `console-serial`), and the filesystem driver last.
SVC_NAMES := sio ioc hid v9958 console-v9958-hid console-serial iocfat
SVC_ZMODS := $(addprefix $(MODULES)/,$(addsuffix .zmod,$(SVC_NAMES)))

$(MODULES)/sio.rel: drivers/sio/sio.c drivers/sio/sio.h \
		include/zephyros/ipc.h include/zephyros/platform.h | $(MODULES)
	$(SDCC) $(SVC_HOT) $< -o $@

$(MODULES)/sio_io.rel: drivers/sio/sio_io.asm \
		platform/zephyr80/hardware.inc | $(MODULES)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(MODULES)/sio_service.rel: drivers/sio/sio_service.c drivers/sio/sio.h \
		include/zephyros/ipc.h $(SVC_PROTOCOL_H) | $(MODULES)
	$(SDCC) $(SVC_HOT) $< -o $@

$(MODULES)/sio_boot.rel: drivers/sio/sio_boot.c drivers/sio/sio.h \
		include/zephyros/ipc.h $(SVC_PROTOCOL_H) | $(MODULES)
	$(SDCC) $(SVC_BOOT) $< -o $@

$(MODULES)/ioc.rel: drivers/ioc/ioc.c drivers/ioc/ioc_driver.h \
		drivers/ioc/ioc_proto.h include/zephyros/ioc.h \
		include/zephyros/ioc_protocol.h \
 include/zephyros/platform.h | $(MODULES)
	$(SDCC) $(SVC_HOT) $< -o $@

$(MODULES)/ioc_io.rel: drivers/ioc/ioc_io.asm \
		platform/zephyr80/hardware.inc | $(MODULES)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(MODULES)/ioc_service.rel: drivers/ioc/ioc_service.c \
		drivers/ioc/ioc_driver.h drivers/ioc/ioc_proto.h \
		include/zephyros/ioc.h include/zephyros/ioc_protocol.h \
		include/zephyros/ipc.h $(SVC_PROTOCOL_H) | $(MODULES)
	$(SDCC) $(SVC_HOT) $< -o $@

$(MODULES)/ioc_boot.rel: drivers/ioc/ioc_boot.c drivers/ioc/ioc_driver.h \
		include/zephyros/ipc.h $(SVC_PROTOCOL_H) | $(MODULES)
	$(SDCC) $(SVC_BOOT) $< -o $@

$(MODULES)/hid.rel: drivers/hid/hid.c drivers/hid/hid_driver.h \
		include/zephyros/ioc_protocol.h \
		include/zephyros/ioc.h \
		include/zephyros/ipc.h $(SVC_PROTOCOL_H) \
		include/zephyros/platform.h | $(MODULES)
	$(SDCC) $(SVC_HOT) $< -o $@

$(MODULES)/v9958.rel: drivers/v9958/v9958.c drivers/v9958/v9958_driver.h \
		include/zephyros/ipc.h $(SVC_PROTOCOL_H) \
		include/zephyros/platform.h | $(MODULES)
	$(SDCC) $(SVC_HOT) $< -o $@

$(MODULES)/v9958_io.rel: drivers/v9958/v9958_io.asm \
		platform/zephyr80/hardware.inc | $(MODULES)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(MODULES)/v9958_service.rel: drivers/v9958/v9958_service.c \
		drivers/v9958/v9958_driver.h \
		include/zephyros/ipc.h $(SVC_PROTOCOL_H) | $(MODULES)
	$(SDCC) $(SVC_HOT) $< -o $@

$(MODULES)/v9958_boot.rel: drivers/v9958/v9958_boot.c \
		drivers/v9958/v9958_driver.h include/zephyros/ipc.h \
		$(SVC_PROTOCOL_H) | $(MODULES)
	$(SDCC) $(SVC_BOOT) $< -o $@

# The font bulk segment: the pinned CP850 6x8 bitmap, assembled straight
# from the reviewable .inc asset (the same source tools/fontpack.py packs
# for the M8-era ROMFS entry).
$(MODULES)/v9958_font.rel: drivers/v9958/v9958_font.asm \
		drivers/v9958/assets/font_cp850_6x8.inc | $(MODULES)
	$(SDASZ80) $(ASFLAGS) -Idrivers/v9958/assets -o $@ $<

# The two build-selected CONSOLE drivers are one source compiled twice
# (work package 42e): the production HID-input/VIDEO-output image and the
# SERIAL image.  Each packed container names its own object.
$(MODULES)/console-v9958-hid.rel: services/console/console_service.c \
		include/zephyros/ipc.h abi/zos/protocol.h \
		include/zephyros/errno.h | $(MODULES)
	$(SDCC) $(SVC_HOT) $< -o $@

$(MODULES)/console-serial.rel: services/console/console_service.c \
		include/zephyros/ipc.h abi/zos/protocol.h \
		include/zephyros/errno.h | $(MODULES)
	$(SDCC) $(SVC_HOT) -DZOS_CONSOLE_SERIAL=1 $< -o $@

# The filesystem driver's hot image: the same source the M9 resident
# provider used, rebuilt as bank-6 service code.  Its FS2 transfer staging,
# the 512-byte cache and the handle pools are `_SERVICE_IO`; the IOC
# dependency is host-resolved at pack time (`depends`).
$(MODULES)/iocfat.rel: fs/iocfat/iocfat.c \
		include/zephyros/ipc.h include/zephyros/ioc.h \
		abi/zos/fs.h abi/zos/object.h abi/zos/protocol.h \
		include/zephyros/errno.h | $(MODULES)
	$(SDCC) $(SVC_HOT) $< -o $@

$(MODULES)/sdcc_indirect_calls.rel: modules/runtime/sdcc_indirect_calls.asm \
		| $(MODULES)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

# Synthetic IPC service set (work packages 39-40): six images packed as
# real service images with the service section, loaded by the acceptance
# image's `services.man`.  Defined here because the production pack rule
# below links against the whole load-order set and needs their objects.
IPC_MODDIR := $(MODULES)/ipc-modules
IPC_SERVICE_NAMES := ioc fat vfs echo con irqx
IPC_HOT_OBJS := $(addprefix $(IPC_MODDIR)/,$(addsuffix .rel,$(IPC_SERVICE_NAMES)))
IPC_BOOT_OBJS := $(addprefix $(IPC_MODDIR)/,$(addsuffix _boot.rel,$(IPC_SERVICE_NAMES)))
IPC_ZMODS := $(addprefix $(IPC_MODDIR)/ipc-,$(addsuffix .zmod,$(IPC_SERVICE_NAMES)))

SVC_OBJS := $(MODULES)/sdcc_indirect_calls.rel \
	$(MODULES)/sio.rel $(MODULES)/sio_io.rel $(MODULES)/sio_service.rel \
	$(MODULES)/sio_boot.rel \
	$(MODULES)/ioc.rel $(MODULES)/ioc_io.rel $(MODULES)/ioc_service.rel \
	$(MODULES)/ioc_boot.rel \
	$(MODULES)/hid.rel \
	$(MODULES)/v9958.rel $(MODULES)/v9958_io.rel \
	$(MODULES)/v9958_service.rel $(MODULES)/v9958_font.rel \
	$(MODULES)/v9958_boot.rel \
	$(MODULES)/console-v9958-hid.rel $(MODULES)/console-serial.rel \
	$(MODULES)/iocfat.rel

# The packer links every image against the whole load-order set (--set):
# the tier plan, the endpoint numbers and the inter-driver dependencies
# are all host-computed, so one pack reads every member's objects.  Both
# pack rules below wait for the whole set, or a parallel build could link
# an image against a half-built one.
SVC_SET_OBJS = $(SVC_OBJS) $(if $(filter 1,$(ZOS_NS_SELFTEST)),\
	$(MODULES)/nsdev.rel $(MODULES)/nsdev_boot.rel) \
	$(if $(filter 1,$(ZOS_IPC_SELFTEST)),\
	$(IPC_HOT_OBJS) $(IPC_BOOT_OBJS) \
	$(IPC_MODDIR)/con_slow.rel $(IPC_MODDIR)/sdcc_indirect_calls.rel)

# One image per service manifest; the report carries the symbol offsets the
# replays read the boot markers and the placements through.  The packer
# links each image at its final bank-6 address against the whole set it is
# loaded with (--set, in load order): the tier plan, the endpoint numbers
# and the inter-driver dependencies are all host-computed, so every image
# of one build must see the same set.
# The manifests an image packs its boot set from.  Production uses the
# service tree's own manifests; the namespace acceptance image packs the
# same services from its own directory, whose manifests carry their
# publications (work package 42a), so the production manifests and
# containers stay byte for byte what they were.
SVC_MANIFEST_DIR ?= services/manifests
SVC_PACK_SET = $(addprefix $(SVC_MANIFEST_DIR)/,$(addsuffix .json,$(ZOS_SERVICES))) \
	$(if $(filter 1,$(ZOS_IPC_SELFTEST)),$(addprefix $(IPC_DIR)/manifests/,$(addsuffix .json,$(IPC_SERVICE_NAMES))))
SVC_PACK_IMAGES = $(addprefix $(MODULES):,$(addprefix $(SVC_MANIFEST_DIR)/,$(addsuffix .json,$(ZOS_SERVICES)))) \
	$(if $(filter 1,$(ZOS_IPC_SELFTEST)),$(addprefix $(IPC_MODDIR):,$(addprefix $(IPC_DIR)/manifests/,$(addsuffix .json,$(IPC_SERVICE_NAMES)))))

$(SVC_ZMODS): $(MODULES)/%.zmod: services/manifests/%.json $(SVC_SET_OBJS) \
		$(SVC_PACK_SET) tools/zmodpack.py | $(MODULES)
	$(PYTHON) tools/zmodpack.py pack $< $@ --objdir $(MODULES) \
		--set $(SVC_PACK_IMAGES) \
		--report $(MODULES)/$*.report.json

# The namespace acceptance image packs the same services from its own
# manifests, which carry their publications (work package 42a).  It gets
# its own rule rather than a variable in the production one, so the
# production manifests and containers stay byte for byte what they were.
ifeq ($(ZOS_NS_SELFTEST),1)
$(SVC_ZMODS): $(MODULES)/%.zmod: tests/ns/manifests/%.json $(SVC_SET_OBJS) \
		$(SVC_PACK_SET) tools/zmodpack.py | $(MODULES)
	$(PYTHON) tools/zmodpack.py pack $< $@ --objdir $(MODULES) \
		--set $(SVC_PACK_IMAGES) \
		--report $(MODULES)/$*.report.json
endif

# The service set an image boots, resolved to its packed images.
SVC_SET_ZMODS = $(foreach svc,$(ZOS_SERVICES),$(MODULES)/$(svc).zmod)

# ROMFS font asset (DATA): the reviewable .inc source is the pinned
# reference font; fontpack.py verifies the exact 2048 bytes and the
# pinned SHA-256 before emitting the deterministic binary.
$(MODULES)/v9958-6x8.font: drivers/v9958/assets/font_cp850_6x8.inc \
		tools/fontpack.py | $(MODULES)
	$(PYTHON) tools/fontpack.py $< $@

# --- target acceptance objects (tests/target/, acceptance builds only) -------

$(BUILD)/target_test.rel: tests/target/support/target_test.c \
		include/zephyros/kernel.h tests/target/include/syscall_selftest.h \
		tests/target/include/irq_selftest.h \
		tests/target/include/romfs_selftest.h \
		tests/target/include/process_selftest.h \
		tests/target/include/hid_selftest.h \
		tests/target/include/video_selftest.h \
		tests/target/include/console_integration_selftest.h \
		abi/zos/ipc.h \
		include/zephyros/service.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# The resident-service acceptance client every M10 driver diagnostic links
# (package 41): endpoint lookup, descriptor-less calls and the copy gate.
$(BUILD)/service_client.rel: tests/target/support/service_client.c \
		tests/target/include/service_client.h \
		include/zephyros/kernel.h include/zephyros/process.h \
		abi/zos/protocol.h include/zephyros/ipc.h \
		kernel/ipc/ipc.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# Test-owned CTC0 programming helper (acceptance images only): linked in
# place of the retired production helper so no acceptance-only CTC code
# reaches the production map.
$(BUILD)/ctc_test.rel: tests/target/support/ctc_test.c \
		tests/target/include/ctc_test.h \
		platform/zephyr80/hardware.h platform/zephyr80/irq.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# IPC substrate and service loader (work packages 39-40, M10 stages 3-4):
# the fixed-core substrate and its primitives (config/residency.json
# `fixed-required`), the cold syscall bodies, the cold resident-service
# loader and the common boot gate.  The six synthetic services are real
# service images packed into the recovery store and loaded from the boot
# manifest, not fixture blobs.
$(BUILD)/ipc.rel: kernel/ipc/ipc.c kernel/ipc/ipc.h \
		include/zephyros/ipc.h abi/zos/ipc.h \
		include/zephyros/irq.h include/zephyros/platform.h \
		include/zephyros/kernel.h include/zephyros/process.h \
		kernel/irq/irq_private.h kernel/memory/upcall.h \
		kernel/syscall/diag.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/ipc_asm.rel: kernel/ipc/ipc_asm.asm \
		platform/zephyr80/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/ipc_common.rel: kernel/ipc/ipc_common.asm \
		platform/zephyr80/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<


$(BUILD)/service_loader.rel: kernel/service/service_loader.c \
		include/zephyros/service.h include/zephyros/ipc.h \
 include/zephyros/romfs.h \
		include/zephyros/errno.h include/zephyros/kernel.h \
		include/zephyros/process.h kernel/ipc/ipc.h \
		kernel/memory/upcall.h kernel/syscall/diag.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/service_boot.rel: kernel/service/service_boot.asm \
		platform/zephyr80/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

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

$(BUILD)/nucleus_test.rel: tests/target/nucleus/nucleus_test.c \
		include/zephyros/irq.h include/zephyros/kernel.h \
		include/zephyros/platform.h abi/zos/diag.h \
		kernel/memory/upcall.h tests/target/include/nucleus_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/nucleus_selftest_asm.rel: tests/target/nucleus/nucleus_entry.asm \
		tests/target/include/nucleus_entry.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/romfs_test.rel: tests/target/romfs/romfs_test.c \
		include/zephyros/romfs.h \
		include/zephyros/kernel.h \
		tests/target/include/romfs_selftest.h $(BUILD)/romfs_expect.inc | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@


# The process acceptance layout header is generated from the SDK-produced
# zextest manifest (work/28c-m9-user-sdk); there is no hand-written
# manifest.  zextest.c no longer includes it (the program uses the rule's
# STACK), so this generation cannot cycle through the program build.
$(BUILD)/process_expect.inc: $(BUILD)/user/zextest/zextest.json \
		tests/subsys/gen_process_expect.py \
		tests/subsys/gen_zex_fixtures.py tools/zexpack.py | $(BUILD)
	$(PYTHON) tests/subsys/gen_process_expect.py \
		$(BUILD)/user/zextest/zextest.json $@

$(BUILD)/process_test.rel: tests/target/process/process_test.c \
		include/zephyros/process.h abi/zos/syscall.h \
		include/zephyros/kernel.h \
		tests/target/include/process_selftest.h $(BUILD)/process_expect.inc | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# The IOC transport diagnostic is the one explicitly test-only rule allowed
# a private IOC include path: it exercises the READY/DONE admission state,
# the bulk-arming class gate, and the unknown-class reply, which are
# transport-private by design.
$(BUILD)/ioc_test.rel: tests/target/ioc/ioc_test.c \
 include/zephyros/ioc.h \
		include/zephyros/kernel.h drivers/ioc/ioc_proto.h \
		include/zephyros/ioc_protocol.h \
		tests/target/include/ioc_selftest.h \
		tests/target/include/service_client.h | $(BUILD)
	$(SDCC) $(CFLAGS) -Idrivers/ioc $< -o $@

$(BUILD)/hid_test.rel: tests/target/hid/hid_test.c \
 include/zephyros/kernel.h \
		tests/target/include/hid_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/video_test.rel: tests/target/video/v9958_test.c \
		include/zephyros/kernel.h \
		tests/target/include/video_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/console_integration_test.rel: tests/target/console/integration_test.c \
		include/zephyros/kernel.h include/zephyros/ioc.h \
		abi/zos/protocol.h \
		tests/target/include/service_client.h \
		tests/target/include/console_integration_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@







# --- native user programs (SDK, work/28c-m9-user-sdk) ------------------------
# Every ZEX is built by one zex_program line from user/sdk/zex.mk.  The
# sources compile with exactly -I abi -I user/sdk/include plus their own
# directory (extended only by explicit test-owned INCLUDES), crt0 and libzos
# are linked, the manifest is generated from the link map, and zexpack packs
# and validates build/user/<name>.zex.  STAGE=1 additionally copies the
# result to build/modules, which the recovery manifests consume.

include user/sdk/zex.mk

# The helloworld sub-project declares its own demos (one zex_program line
# per program) next to their sources.
include user/helloworld/Makefile

$(eval $(call zex_program,monitor,user/monitor,\
	monitor.c monitor_mem.c monitor_io.c hexfmt.c hexdump.c status.c,\
	STACK=512,STAGE=1))

$(eval $(call zex_program,zextest,tests/target/process,\
	zextest.c zextest_sp.asm,STACK=256,STAGE=1,\
	INCLUDES=tests/target/include))

# Filesystem outcome programs (work package 42b, correction 01): the checks
# the image boots (`ZOS_BOOT_PROGRAM=/SYSTEM/fs_selftest.zex`) and the child
# it SYS_EXECs.  Both are ordinary SDK programs.
$(eval $(call zex_program,fs_selftest,tests/fs,\
	fs_selftest.c,STACK=384,STAGE=1))

$(eval $(call zex_program,fs_child,tests/fs,\
	fs_child.c,STACK=192,STAGE=1))

# Extended-memory outcome programs (work package 43): the checks the image
# boots (`ZOS_BOOT_PROGRAM=/SYSTEM/xmem_selftest.zex`) and the child it
# SYS_EXECs while its aperture is open.  Both are ordinary SDK programs.
$(eval $(call zex_program,xmem_selftest,tests/xmem,\
	xmem_selftest.c,STACK=512,STAGE=1))

$(eval $(call zex_program,xmem_child,tests/xmem,\
	xmem_child.c,STACK=256,STAGE=1))

$(eval $(call zex_program,ipc,tests/target/ipc,\
	ipc.c,STACK=256,STAGE=1,\
	INCLUDES=tests/target/include))

# IPC caller-buffer probe (work package 39, correction 01): the boot
# program of the IPC acceptance image.  NO_CRT0 with a custom entry and
# LOWBASE=0x0300 and CODEBASE=0x0344 put the SDK's 93-byte low-data area
# immediately above 0100h-02FFh, which is left as
# a plain 512-byte buffer: the only way a ZEX image can name the caller
# window itself as a copy destination.
$(eval $(call zex_program,ipccopy,tests/target/ipc,\
	ipccopy.c ipccopy_entry.asm,NO_CRT0=1,ENTRY=ipc_copy_entry,\
	LOWBASE=0x0300,CODEBASE=0x0344,STAGE=1,\
	INCLUDES=tests/target/include))

# Namespace acceptance programs (work package 42a): the client the image
# boots (ZOS_BOOT_PROGRAM=/SYSTEM/ns.zex) and the child it SYS_EXECs.  Both
# are ordinary SDK programs.
$(eval $(call zex_program,ns,tests/ns,\
	ns.c,STACK=256,STAGE=1,\
	INCLUDES=tests/target/include))

$(eval $(call zex_program,nschild,tests/ns,\
	nschild.c,STACK=192,STAGE=1,\
	INCLUDES=tests/target/include))

# ESC c display-reset check (work package 42e, test-only): a tiny SDK
# program the console integration image boots before the monitor.  It
# writes ESC `c` - the V9958 display reset - and a line through the
# console, then hands the console to the monitor, so the owner can watch
# the board's display reset.  It is acceptance content: nothing packs it
# into the production store.
$(eval $(call zex_program,escc,tests/console,\
	escc.c,STACK=256,STAGE=1))

# Deterministic malformed ZEX containers (see
# tests/subsys/gen_zex_fixtures.py) shared by the subsystem acceptance
# manifest; the stamp makes `make clean && make zex-test` self-contained.
$(MODULES)/zex-fixtures.stamp: tests/subsys/gen_zex_fixtures.py \
		tools/zexpack.py | $(MODULES)
	$(PYTHON) tests/subsys/gen_zex_fixtures.py $(MODULES)
	touch $@

# --- ROMFS recovery store (ROM pages 2-7) ------------------------------------
# Every image packs a manifest; the subsystem acceptance image adds the
# loader/ROMFS/ZEX fixtures, and tests/test_romfs_sim.py patches the packed
# subsystem image in place to exercise the kernel's validation (work
# package 42d: the corrupt-store image is gone, its outcome is that test).
ifeq ($(ZOS_CONSOLE_INTEGRATION_SELFTEST),1)
# Console integration acceptance store: the full device service set (the
# filesystem driver is compiled out of this image, so its entry is
# absent), the monitor launcher and the ESC c check program the image
# boots (ZOS_BOOT_PROGRAM=/SYSTEM/escc.zex, package 42e).  Work package
# 42d merged the IOC, HID, video and console acceptance stores into it.
ROMFS_MANIFEST := tests/console/manifest-integration.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex $(MODULES)/escc.zex
else ifeq ($(ZOS_SUBSYS_SELFTEST),1)
ROMFS_MANIFEST := tests/subsys/manifest.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex $(MODULES)/romfs_crosspage.bin \
		$(MODULES)/zextest.zex $(MODULES)/zex-fixtures.stamp
else ifeq ($(ZOS_NUCLEUS_SELFTEST),1)
# Nucleus-layout acceptance store (work package 42d): the same SIO-only
# boot set and fixtures as the subsystem image, which is the store its
# boot path loads.  Before 42d the row fell through to the production
# manifest while packing only this set, so the pack could not find the
# driver images it did not select.
ROMFS_MANIFEST := tests/subsys/manifest.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex $(MODULES)/romfs_crosspage.bin \
		$(MODULES)/zextest.zex $(MODULES)/zex-fixtures.stamp
else ifeq ($(ZOS_CONSOLE_SERIAL_SELFTEST),1)
# Console-serial variant store (work package 42e): the production service
# set with `console-serial` in the CONSOLE slot; the image boots the
# monitor on the serial lane.
ROMFS_MANIFEST := tests/console/manifest-serial.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex
else ifeq ($(ZOS_CONSOLE_SERIAL_FAIL_SELFTEST),1)
# Required-console boot-failure store (work package 42e): the same set and
# services.man, but the ROMFS manifest omits `console-serial.zmod`, so the
# loader's lookup fails and the boot must stop explicitly on the early
# console (36.11) - no fallback and no launcher.
ROMFS_MANIFEST := tests/console/manifest-serial-missing.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex
else ifeq ($(ZOS_FS_SELFTEST),1)
# Filesystem outcome store (work package 42b, correction 01): the SIO/IOC/
# console services the production boot set loads, the IOC-FAT service
# publishing `/FS`, the monitor the boot program hands the console to, and
# the two SDK programs - the checks in /SYSTEM/fs_selftest.zex and the
# child it SYS_EXECs.
ROMFS_MANIFEST := tests/fs/manifest.json
# Recursive: the module variables are defined below this chain.
ROMFS_EXTRA_DEPS = $(MODULES)/fs_selftest.zex $(MODULES)/fs_child.zex \
	$(MODULES)/monitor.zex $(MODULES)/services.man
else ifeq ($(ZOS_XMEM_SELFTEST),1)
# Extended-memory outcome store (work package 43): the SIO and console
# services the production boot path loads, and the two SDK programs - the
# checks in /SYSTEM/xmem_selftest.zex and the child it SYS_EXECs.  No
# monitor: the launcher's exit report is the last line the replay needs.
ROMFS_MANIFEST := tests/xmem/manifest.json
# Recursive: the module variables are defined below this chain.
ROMFS_EXTRA_DEPS = $(MODULES)/xmem_selftest.zex \
	$(MODULES)/xmem_child.zex $(MODULES)/services.man
else ifeq ($(ZOS_HELLOWORLD_IMAGE),1)
# SDK demo image (work/28c-m9-user-sdk): the production boot set and store
# plus the hello demo, which the launcher starts instead of the monitor
# (ZOS_BOOT_PROGRAM=/SYSTEM/hello.zex).  Booting a user program needs no
# kernel change, which is exactly what this image demonstrates.
ROMFS_MANIFEST := user/helloworld/manifest.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex $(MODULES)/hello.zex
else ifeq ($(ZOS_IPC_SELFTEST),1)
# IPC acceptance image (work packages 39-40): the SIO fallback plus the two
# SDK programs the launcher boots and the probe starts
# (ZOS_BOOT_PROGRAM=/SYSTEM/ipccopy.zex).  The probe
# (tests/target/ipc/ipccopy.c) measures the caller-buffer copy paths and
# SYS_EXECs the conformance client (/SYSTEM/ipc.zex), which is where the
# promoted suite lives.  The six synthetic services are real service
# images in the recovery store, listed by the boot manifest
# (services.man) and placed in bank 6 by the nucleus service loader.
ROMFS_MANIFEST := tests/ipc/manifest.json
# Recursive: the service-image variables are defined below this chain.
ROMFS_EXTRA_DEPS = $(MODULES)/ipc.zex $(MODULES)/ipccopy.zex \
	$(IPC_ZMODS) $(MODULES)/services.man
else ifeq ($(ZOS_NS_SELFTEST),1)
# Namespace acceptance store (work package 42a): the SIO and console
# services whose publications the DEVICE table carries, the synthetic
# publisher that adds a second grouping container and a third protocol
# type, and the two SDK programs - the checks run in /SYSTEM/ns.zex, which
# SYS_EXECs /SYSTEM/nschild.zex for the inheritance rule.
ROMFS_MANIFEST := tests/ns/manifest.json
# Recursive: the module variables are defined below this chain.
ROMFS_EXTRA_DEPS = $(MODULES)/ns.zex $(MODULES)/nschild.zex \
	$(MODULES)/nsdev.zmod $(MODULES)/services.man
else
ROMFS_MANIFEST := recovery/manifest.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex
endif

# --- resident service images (work packages 39-40) ---------------------------
# Six synthetic services packed as real service images into the recovery
# store, listed by the boot manifest `services.man` and loaded by the
# nucleus service loader (kernel/service/service_loader.c).
# Acceptance-only; nothing here is built for any other configuration.
# The images live under the shared build/modules/ipc-modules/ so a --set
# token can name their own objdir.  The set's object variables are
# defined with the production service objects above, because the pack
# rules there link every image against the whole set (--set) and have to
# see them before those rules are read.

ifeq ($(ZOS_IPC_SELFTEST),1)
$(IPC_MODDIR):
	mkdir -p $@

# Hot-segment objects: dispatch entries and the init entry.  --dataseg
# SERVICE_IO names the data area after the rule it has to satisfy: the
# loader places it inside bank 6's 8000h-BFFFh (correction 01 to
# architecture 36.8), where both the window and the aperture show it.
$(IPC_HOT_OBJS): $(IPC_MODDIR)/%.rel: $(IPC_DIR)/%.c \
		tests/target/include/ipc_test.h | $(IPC_MODDIR)
	$(SDCC) $(MOD_CFLAGS) --dataseg SERVICE_IO -Itests/target/include $< -o $@

# Boot-segment objects: boot-only code (_BOOT) and its BSS (_BOOT_DATA),
# run once through the mode-10 gate with Q = 6.
$(IPC_BOOT_OBJS): $(IPC_MODDIR)/%_boot.rel: $(IPC_DIR)/%_boot.c \
		tests/target/include/ipc_test.h | $(IPC_MODDIR)
	$(SDCC) $(MOD_CFLAGS) --codeseg BOOT --dataseg BOOT_DATA \
		-Itests/target/include $< -o $@

# `con`'s hot-segment slow buffer (area _HOT): the deliberate service-side
# buffer outside 8000h-BFFFh the path-C copy case needs.
$(IPC_MODDIR)/con_slow.rel: $(IPC_DIR)/con_slow.c \
		tests/target/include/ipc_test.h | $(IPC_MODDIR)
	$(SDCC) $(MOD_CFLAGS) --dataseg HOT -Itests/target/include $< -o $@

$(IPC_MODDIR)/sdcc_indirect_calls.rel: \
		modules/runtime/sdcc_indirect_calls.asm | $(IPC_MODDIR)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

# One service image per manifest; the report carries the symbol offsets
# the replay reads the boot-segment markers and placements through.  The
# whole set's objects are prerequisites because each pack links the set.
$(IPC_ZMODS): $(IPC_MODDIR)/ipc-%.zmod: $(IPC_DIR)/manifests/%.json \
		$(SVC_SET_OBJS) $(SVC_PACK_SET) tools/zmodpack.py | $(IPC_MODDIR)
	$(PYTHON) tools/zmodpack.py pack $< $@ --objdir $(IPC_MODDIR) \
		--set $(SVC_PACK_IMAGES) \
		--report $(IPC_MODDIR)/$*.report.json

endif

ifeq ($(ZOS_NS_SELFTEST),1)
# Namespace acceptance synthetic publisher (work package 42a): one real
# service image in the recovery store, packed from the image's own
# manifest (tests/ns/manifests/nsdev.json) so its /dev publications travel
# in the container the loader reads.  The objects build exactly like
# production service objects: hot code with its SERVICE_IO data, and a
# boot segment that records its single run.
$(MODULES)/nsdev.rel: tests/target/ns/nsdev.c \
		tests/target/include/ns_test.h include/zephyros/ipc.h | $(MODULES)
	$(SDCC) $(SVC_HOT) -Itests/target/include $< -o $@

$(MODULES)/nsdev_boot.rel: tests/target/ns/nsdev_boot.c \
		tests/target/include/ns_test.h | $(MODULES)
	$(SDCC) $(SVC_BOOT) -Itests/target/include $< -o $@

# The container: one pack against the image's whole load-order set, from
# the image's own manifest, so the /dev publications are host-resolved
# beside the endpoint numbers.
$(MODULES)/nsdev.zmod: $(SVC_MANIFEST_DIR)/nsdev.json $(SVC_SET_OBJS) \
		$(SVC_PACK_SET) tools/zmodpack.py | $(MODULES)
	$(PYTHON) tools/zmodpack.py pack $< $@ --objdir $(MODULES) \
		--set $(SVC_PACK_IMAGES) \
		--report $(MODULES)/nsdev.report.json

endif
# The boot manifest: the flat ROMFS names in dependency (load) order,
# validated as a set - unique endpoints, resolvable dependencies and a
# tier allocation that fits - and reported with the two service ledgers.
# An image boots what its manifest lists: the production store lists the
# five driver services, the IPC image lists the SIO and console services
# its SDK programs print through ahead of the synthetic set.
SVC_MAN_INPUTS = $(SVC_SET_ZMODS) \
	$(if $(filter 1,$(ZOS_IPC_SELFTEST)),$(IPC_ZMODS))
$(MODULES)/services.man: $(SVC_MAN_INPUTS) tools/zmodpack.py | $(MODULES)
	$(PYTHON) tools/zmodpack.py services $@ $(SVC_MAN_INPUTS) \
		--report $(MODULES)/services.plan.json

# --- boot-program setting (package 28b) --------------------------------------
# Validate ZOS_BOOT_PROGRAM against the manifest this image packs, then
# compile it into kernel/main.c as a constant string.  The stamp makes the
# check a prerequisite of main.rel, so parallel builds cannot link a
# kernel whose boot path was never validated; the manifest dependency
# re-validates for every image.
BOOT_PROGRAM_STAMP := $(BUILD)/generated/boot_program.stamp

$(BOOT_PROGRAM_STAMP): config/system.mk $(ROMFS_MANIFEST) \
		tools/bootprogram.py | $(BUILD)
	mkdir -p $(BUILD)/generated
	$(PYTHON) tools/bootprogram.py check "$(ZOS_BOOT_PROGRAM)" \
		$(ROMFS_MANIFEST)
	touch $@

# Single quotes protect the C string quotes from the shell.
CFLAGS    += -DZOS_BOOT_PROGRAM='"$(ZOS_BOOT_PROGRAM)"'

$(BUILD)/main.rel: $(BOOT_PROGRAM_STAMP)

$(BUILD)/recovery.romfs: $(ROMFS_MANIFEST) $(SVC_SET_ZMODS) \
		$(MODULES)/services.man $(ROMFS_EXTRA_DEPS) \
		tools/romfspack.py $(ZOS_PLATFORM_MEMORY_INC) | $(BUILD)
	$(PYTHON) tools/romfspack.py --memory-inc $(ZOS_PLATFORM_MEMORY_INC) pack \
		$(ROMFS_MANIFEST) $@ --objdir $(MODULES)

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
		$(ZOS_NUCLEUS_SELFTEST) \
		$(ZOS_CONSOLE_INTEGRATION_SELFTEST) \
		$(ZOS_FS_SELFTEST))
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
	window_data_base=$$(sed -n 's/^ZOS_WINDOW_DATA_BASE = //p' $(GEOMETRY_MEMORY)); \
	test -n "$$window_data_base" || { echo "no derived window data base in $(GEOMETRY_MEMORY)"; exit 1; }; \
	{ \
	  printf '%s\n' '-n' '-m' '-w'; \
	  printf '%s\n' '-i $(BUILD)/zephyros.ihx'; \
	  printf '%s\n' '-b _CODE = 0x2000' "-b _DATA = $$data_base" \
	                '-b _WINDOW = $(ZOS_REV13_WINDOW_BASE)' \
	                "-b _WINDOW_DATA = $$window_data_base" \
	                '-b _COMMON = 0xE000'; \
	  printf '%s\n' $(OBJS) $(TARGET_TEST_OBJS) $(END_OBJ); \
	  printf '%s\n' '-e'; \
	} > $@

$(BUILD)/zephyros.ihx $(BUILD)/zephyros.map: $(OBJS) $(TARGET_TEST_OBJS) \
		$(END_OBJ) $(BUILD)/zephyr80.lk $(GEOMETRY_MEMORY) | $(BUILD)
	$(SDLDZ80) -f $(BUILD)/zephyr80.lk
else
# Canonical build: the checked-in linker template supplies the production
# object list; the two derived data bases are substituted into
# build/zephyr80.lk.
$(BUILD)/zephyr80.lk: linker/zephyr80.lk $(GEOMETRY_MEMORY) | $(BUILD)
	@data_base=$$(sed -n 's/^ZOS_DATA_BASE = //p' $(GEOMETRY_MEMORY)); \
	test -n "$$data_base" || { echo "no derived data base in $(GEOMETRY_MEMORY)"; exit 1; }; \
	window_data_base=$$(sed -n 's/^ZOS_WINDOW_DATA_BASE = //p' $(GEOMETRY_MEMORY)); \
	test -n "$$window_data_base" || { echo "no derived window data base in $(GEOMETRY_MEMORY)"; exit 1; }; \
	sed -e "s/@DATA_BASE@/$$data_base/" \
	    -e "s/@WINDOW_DATA_BASE@/$$window_data_base/" \
	    linker/zephyr80.lk > $@

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
		$(BUILD)/recovery.romfs \
		tools/pack_image.py tools/mapcheck.py $(MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) tools/mapcheck.py $(MAPCHECK_FLAGS) $(RESIDENCY_FLAGS) \
		$(BUILD)/zephyros.map \
		$(MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) tools/pack_image.py $(MAPCHECK_FLAGS) $(BUILD)/zephyros.ihx \
		$(BUILD)/zephyros.map $(BUILD) "$(PK_SUFFIX)" $(MEMORY_INC)

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
# One current-boot-path image carries the mechanism selftests: the syscall
# gate and the IRQ executive (early hook), then the ROMFS reader and the
# process/ZEX lifecycle (late hook), and finally the monitor launcher's
# scripted session.  The declared boot set is the SIO serial service plus
# the console service that composes it, placed by the resident loader
# (work package 42c deleted the arena loader the loader group exercised).

subsystem-selftest:
	$(MAKE) BUILD=build-subsys-selftest ZOS_SUBSYS_SELFTEST=1 \
		build-subsys-selftest/zephyros-subsys-selftest.bin \
		build-subsys-selftest/zephyr80-subsys-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-subsys-selftest/zephyros.map \
		build-subsys-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_subsys_sim.py' -v
	$(PYTHON) -m unittest discover -s tests -p 'test_romfs_sim.py' -v
	@echo "subsystem-selftest image: build-subsys-selftest/zephyros-subsys-selftest.bin"

# The SDK's own tests (tests/test_sdk.py) and the generated-manifest
# checks need the packed demo and the argument test program, so the
# standard check always builds them.  Neither is part of a production
# image: the demo lives in user/helloworld/ and zextest is an acceptance
# program.
check-fast: $(ZEX_USER_ROOT)/hello.zex $(MODULES)/zextest.zex
	$(PYTHON) -m unittest discover -s tests -v
	$(PYTHON) tools/mapcheck.py $(MAPCHECK_FLAGS) $(RESIDENCY_FLAGS) \
		$(BUILD)/zephyros.map \
		$(MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) tools/pack_image.py $(MAPCHECK_FLAGS) $(BUILD)/zephyros.ihx \
		$(BUILD)/zephyros.map $(BUILD) "$(PK_SUFFIX)" $(MEMORY_INC)
	sha256sum $(IMAGE) $(MAME_IMAGE)
	cmp $(IMAGE) $(MAME_IMAGE)
	@echo "make check: OK"

clean:
	rm -rf $(BUILD) build-*

# --- host-side ZMOD tooling tests -------------------------------------------
# Builds the service containers with the real toolchain, validates them,
# then runs the host container suite (the packed service images).

zmod-test: $(SVC_SET_ZMODS)
	$(PYTHON) tools/zmodpack.py validate $(SVC_SET_ZMODS)
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

# --- HID tooling ----------------------------------------------------------

hid-test: $(MODULES)/hid.zmod
	$(PYTHON) tools/zmodpack.py validate $(MODULES)/hid.zmod
	$(PYTHON) tools/zmodpack.py dump $(MODULES)/hid.zmod
	$(PYTHON) -m unittest discover -s tests -p 'test_hid.py' -v
	@echo "hid-test: OK"

# --- video tooling --------------------------------------------------------

v9958-test: $(MODULES)/v9958.zmod $(MODULES)/v9958-6x8.font
	$(PYTHON) tools/zmodpack.py validate $(MODULES)/v9958.zmod
	$(PYTHON) tools/zmodpack.py dump $(MODULES)/v9958.zmod
	$(PYTHON) tools/fontpack.py verify drivers/v9958/assets/font_cp850_6x8.inc
	$(PYTHON) -m unittest discover -s tests -p 'test_v9958.py' -v
	@echo "v9958-test: OK"

# --- console integration acceptance and fallback matrix (package 20) --------------
# console-integration-selftest is the final acceptance image: the full production stack
# plus the integration target diagnostic, the IOC, HID and V9958 device
# diagnostics (merged here by work package 42d) and the unchanged monitor
# launcher.  tests/test_console_acceptance.py replays the production bytes
# (build/zephyros.bin) and these bytes: boot order, binding, IOC
# command/status/bulk sequencing, HID queue traffic, the V9958 trace,
# the split console, the ESC c display reset, the scripted monitor session
# with two exit/relaunch cycles, post-fault recovery and the malformed-call
# invariants; test_ioc_sim.py, test_hid_sim.py and test_v9958_sim.py
# replay the same image for their device diagnostics (and the console
# session the retired console-selftest image carried).

console-integration-selftest: all
	$(MAKE) BUILD=build-console-integration-selftest ZOS_CONSOLE_INTEGRATION_SELFTEST=1 ZOS_SMOKE_HOST_SYNC=1 \
		build-console-integration-selftest/zephyros-console-integration-selftest.bin \
		build-console-integration-selftest/zephyr80-console-integration-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-console-integration-selftest/zephyros.map \
		build-console-integration-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_console_acceptance.py' -v
	$(PYTHON) -m unittest discover -s tests -p 'test_ioc_sim.py' -v
	$(PYTHON) -m unittest discover -s tests -p 'test_hid_sim.py' -v
	$(PYTHON) -m unittest discover -s tests -p 'test_v9958_sim.py' -v
	@echo "console integration image: build-console-integration-selftest/zephyros-console-integration-selftest.bin"

# console-serial variant (work package 42e): the production image set with
# the other CONSOLE driver.  The first image boots the monitor on the
# serial lane (the runtime-fallback images' replacement outcome); the
# second omits the console image from its ROMFS manifest, so the required
# driver cannot be loaded and the boot must stop explicitly on the early
# console.  tests/test_console_serial.py replays both.

console-serial-selftest:
	$(MAKE) BUILD=build-console-serial-selftest \
		ZOS_CONSOLE_SERIAL_SELFTEST=1 ZOS_CONSOLE=console-serial \
		ZOS_SMOKE_HOST_SYNC=1 \
		build-console-serial-selftest/zephyros-console-serial-selftest.bin \
		build-console-serial-selftest/zephyr80-console-serial-selftest.bin
	$(MAKE) BUILD=build-console-serial-fail-selftest \
		ZOS_CONSOLE_SERIAL_FAIL_SELFTEST=1 ZOS_CONSOLE=console-serial \
		ZOS_SMOKE_HOST_SYNC=1 \
		build-console-serial-fail-selftest/zephyros-console-serial-fail-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-console-serial-selftest/zephyros.map \
		build-console-serial-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_console_serial.py' -v
	@echo "console serial image: build-console-serial-selftest/zephyros-console-serial-selftest.bin"
	@echo "console serial failure image: build-console-serial-fail-selftest/zephyros-console-serial-fail-selftest.bin"

# Filesystem outcome image (work package 42b, correction 01): the
# production boot path with the IOC-FAT service publishing `/FS` and the SDK
# outcome program booting as /SYSTEM/fs_selftest.zex.  The simulator replay
# serves FS2 from a host peer, so the same image runs on hardware against a
# real card; the checks and their counts are pinned by
# tests/test_fs_selftest_sim.py.
fs-selftest: all
	$(MAKE) BUILD=build-fs-selftest ZOS_FS_SELFTEST=1 \
		build-fs-selftest/zephyros-fs-selftest.bin \
		build-fs-selftest/zephyr80-fs-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-fs-selftest/zephyros.map \
		build-fs-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_fs_selftest*.py' -v
	@echo "filesystem image: build-fs-selftest/zephyros-fs-selftest.bin"

# IPC acceptance image (work package 39): the production boot path with
# the fixed-core substrate, the six synthetic bank-6 services placed by
# the acceptance loader, and the SDK client program booting as
# /SYSTEM/ipccopy.zex.  The client runs the promoted conformance suite and
# checks its own results; the simulator replay measures the spans, the
# stack high-water marks, the latch intervals and the IRQ delivery.
ipc-selftest: all
	$(MAKE) BUILD=build-ipc-selftest ZOS_IPC_SELFTEST=1 \
		ZOS_BOOT_PROGRAM=/SYSTEM/ipccopy.zex \
		build-ipc-selftest/zephyros-ipc-selftest.bin \
		build-ipc-selftest/zephyr80-ipc-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		$(if $(IPC_COLD_EXTRA_CSV),--cold-extra $(IPC_COLD_EXTRA_CSV),) \
		build-ipc-selftest/zephyros.map \
		build-ipc-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_ipc_selftest*.py' -v
	@echo "ipc image: build-ipc-selftest/zephyros-ipc-selftest.bin"

# Extended-memory outcome image (work package 43): the production boot
# path with the SIO/console service set and the SDK outcome program
# booting as /SYSTEM/xmem_selftest.zex.  The client drives the whole
# extended-memory family and the mode-10 aperture and checks its own
# results; it then SYS_EXECs /SYSTEM/xmem_child.zex with a path living in
# the aperture, and the simulator replay asserts the transcript
# (tests/test_xmem_selftest_sim.py).  The aperture itself is
# hardware-authoritative: the owner runs this image on the board.
xmem-selftest: all
	$(MAKE) BUILD=build-xmem-selftest ZOS_XMEM_SELFTEST=1 \
		build-xmem-selftest/zephyros-xmem-selftest.bin \
		build-xmem-selftest/zephyr80-xmem-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-xmem-selftest/zephyros.map \
		build-xmem-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_xmem_selftest*.py' -v
	@echo "xmem image: build-xmem-selftest/zephyros-xmem-selftest.bin"

# Namespace and handle acceptance image (work package 42a, correction 01):
# the production boot path with the DEVICE/ROMFS providers, the typed
# handle table, bind/call/unbind/ns_enum and the standard handles linked
# on top of a small boot set, and the SDK client program booting as
# /SYSTEM/ns.zex.  The client checks the namespace outcomes itself and
# prints them; the simulator replay compares the transcript.  Production
# is not touched by this image: it reproduces its own image hash.
ns-selftest: all
	$(MAKE) BUILD=build-ns-selftest ZOS_NS_SELFTEST=1 \
		build-ns-selftest/zephyros-ns-selftest.bin \
		build-ns-selftest/zephyr80-ns-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		--cold-extra vfs_ns,vfs_ns_file,object,vfs_file \
		build-ns-selftest/zephyros.map \
		build-ns-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_ns_selftest*.py' -v
	@echo "namespace image: build-ns-selftest/zephyros-ns-selftest.bin"

# Nucleus-layout acceptance image (work package 38): the revision-13 gates
# with P != 0 on entry (syscall, IRQ), the cold-half handler placement and
# the fixed upcall shim against a stub in bank 6.  SIO-only boot set, so
# the cold half's slack belongs to the test objects.
nucleus-selftest: all
	$(MAKE) BUILD=build-nucleus-selftest ZOS_NUCLEUS_SELFTEST=1 \
		build-nucleus-selftest/zephyros-nucleus-selftest.bin \
		build-nucleus-selftest/zephyr80-nucleus-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-nucleus-selftest/zephyros.map \
		build-nucleus-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_nucleus_sim.py' -v
	@echo "nucleus image: build-nucleus-selftest/zephyros-nucleus-selftest.bin"

# SDK helloworld demo image (work/28c-m9-user-sdk): the production boot path
# in its own build directory (build-helloworld) with the SDK hello demo
# added to the recovery manifest, booted as /SYSTEM/hello.zex.  This is the
# end-to-end proof that a user program needs no kernel change: the only
# inputs that differ from production are the ZOS_BOOT_PROGRAM setting and
# the manifest.  The simulator replay checks the transcript.

helloworld-image: all
	$(MAKE) BUILD=build-helloworld ZOS_HELLOWORLD_IMAGE=1 \
		ZOS_BOOT_PROGRAM=/SYSTEM/hello.zex \
		build-helloworld/zephyros-helloworld.bin \
		build-helloworld/zephyr80-helloworld.bin
	$(PYTHON) -m unittest discover -s tests -p 'test_sdk.py' -v
	@echo "helloworld image: build-helloworld/zephyros-helloworld.bin"

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
#   smoke-hardware-console-integration             build-console-integration-selftest/zephyros-console-integration-selftest.bin  make console-integration-selftest
#   smoke-hardware-console-serial      build-console-serial-selftest/zephyros-console-serial-selftest.bin  make console-serial-selftest
#   smoke-hardware-fs             build-fs-selftest/zephyros-fs-selftest.bin  make fs-selftest
#
# `make hardware-images` builds every image the smoke modes need.  The
# images wait for the tool's host-sync sequence, so start the smoke tool and
# press RESET (the tool re-sends the sequence while the banner is pending).
# Each target below prints the exact image and sha256 to flash and fails with
# the build command when that image is missing.

# $(1) = image path, $(2) = target that builds it.
define smoke_preflight
if [ ! -f "$(1)" ]; then echo "smoke: $(1) is missing - build it first: make $(2)"; exit 1; fi; printf 'smoke: image to flash: %s\n       sha256: %s\n' "$(1)" "$$(sha256sum '$(1)' | cut -d' ' -f1)"
endef

hardware-images: all subsystem-selftest \
		console-integration-selftest console-serial-selftest \
		fs-selftest xmem-selftest
	@echo "hardware-images: every smoke image is built under build*/"

smoke-hardware:
	@$(call smoke_preflight,build/zephyros.bin,all)
	$(PYTHON) tools/serial_smoke.py --mode normal --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-subsystem:
	@$(call smoke_preflight,build-subsys-selftest/zephyros-subsys-selftest.bin,subsystem-selftest)
	$(PYTHON) tools/serial_smoke.py --mode subsystem --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

# Console modes: the full integration image (serial portion: early
# diagnostics, binding report, the target selftest and the ESC c reset
# program's evidence; the interactive monitor session over HID/V9958 is
# the human gate) and the console-serial variant, whose whole dynamic
# console is this serial lane.  The host sync is sent while the banner is
# pending, as for the other console smoke modes.

smoke-hardware-console-integration:
	@$(call smoke_preflight,build-console-integration-selftest/zephyros-console-integration-selftest.bin,console-integration-selftest)
	$(PYTHON) tools/serial_smoke.py --mode console-integration --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-console-serial:
	@$(call smoke_preflight,build-console-serial-selftest/zephyros-console-serial-selftest.bin,console-serial-selftest)
	$(PYTHON) tools/serial_smoke.py --mode console-serial --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-fs:
	@$(call smoke_preflight,build-fs-selftest/zephyros-fs-selftest.bin,fs-selftest)
	$(PYTHON) tools/serial_smoke.py --mode fs --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))
