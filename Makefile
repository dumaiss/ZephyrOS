# KonaOS build
#
# Freestanding SDCC/sdasz80/sdldz80 build.  No hosted CRT and no standard
# library are linked; the tiny C runtime initialization KonaOS needs is
# implemented in boot/start.asm.  The two SDCC indirect-call helpers the
# freestanding build needs for device ops tables are provided by
# kernel/runtime/sdcc_helpers.asm.
#
# Platform build configuration (include paths and declarative constant
# files) comes from platform/zephyr80/config.mk.
#
# The drivers are not linked into the kernel: each is packed as a MOD
# driver image (build/modules/*.mod), stored in the ROMFS recovery image
# (build/recovery.romfs, pages 2-7 of the 512 KiB ROM) and placed in bank 6
# at boot by the resident loader, which reads the store through the fixed
# ROMFS reader.  There is one boot path: every image boots from the
# recovery store, and residency has exactly two tiers (config/
# residency.json).
#
#   make          build the production ROM images under build/
#   make SYSTEM=<name>  build that image in place (systems/<name>.json)
#   make test     run host-side image/layout tests and integrity checks
#   make check    reproducible rebuild + full host validation stack
#   make romfs-test           ROMFS image + host ROMFS tool tests
#   make mod-test            packed-module tooling tests
#   make exe-test             packed-program tooling tests
#   make sdk                  standalone SDK tree + tarball (build/kos-sdk-*)
#   make subsystem-selftest        current-path and ROMFS acceptance image
#   make nucleus-selftest          revision-13 window/nucleus acceptance image
#   make console-serial-selftest   serial-console image and its failure store
#   make fs-selftest               filesystem outcome acceptance image
#   make xmem-selftest             extended-memory and aperture outcome image
#   make ns-selftest               namespace acceptance image
#   make ipc-selftest              resident-driver IPC acceptance image
#   make helloworld-image          SDK demo image (no kernel change)
#   make clean    remove all generated files
#
# Builds are parallel by default (KOS_JOBS defaults to nproc); use
# `make KOS_JOBS=1` for the serial build, which produces identical bytes.

SDCC      ?= sdcc
SDASZ80   ?= sdasz80
SDLDZ80   ?= sdldz80
PYTHON    ?= python3

# Parallel by default.  The dependency graph is complete (the
# exe-fixtures stamp declares its loader-fixture prerequisite), so a
# parallel build is race-free; `make KOS_JOBS=1` reproduces the serial
# build byte for byte.  MAKEFLAGS is exported to recursive image targets.
KOS_JOBS  ?= $(shell nproc 2>/dev/null || echo 1)
MAKEFLAGS += -j$(KOS_JOBS)

BUILD     := build
.DEFAULT_GOAL := all

# --- system selection -------------------------------------
# One systems/<name>.json describes the image this makefile builds: its
# drivers in load order, the program the launcher starts, the store's files
# and the image's C flags.  tools/systemgen.py turns that description into
# the generated inputs under $(BUILD) - system.mk, the ROMFS store manifest,
# the boot manifest drivers.man and one resolved driver manifest per
# selected driver - and validates the schemas, the drivers, the symbolic
# dependencies and the boot setting.
#
# `include` builds the generated file first and restarts make, so every rule
# below sees KOS_DRIVERS, KOS_BOOT_PROGRAM and the object lists it carries.
# Every image is a system file, so `make SYSTEM=fs` builds the filesystem
# image in place and the image targets at the bottom are one line each.
SYSTEM       ?= production
SYSTEM_JSON  := systems/$(SYSTEM).json
SYSTEM_MK    := $(BUILD)/system.mk
SYSTEM_STAMP := $(BUILD)/system.stamp

# Every driver manifest is an input: the resolved copies, the boot manifest
# and the store manifest all derive from them.
DRIVER_MANIFESTS := $(wildcard drivers/*/*.json) \
	$(wildcard tests/*/manifests/*.json) \
	$(wildcard tests/target/*/manifests/*.json)

$(SYSTEM_STAMP): $(SYSTEM_JSON) tools/systemgen.py $(DRIVER_MANIFESTS) \
		| $(BUILD)
	$(PYTHON) tools/systemgen.py $(SYSTEM_JSON) $(BUILD) $(SYSTEM_GEN_FLAGS)
	touch $@

# The generated file the build includes: the empty recipe makes make restart
# after the generator ran, which is what brings KOS_DRIVERS into this make.
$(SYSTEM_MK): $(SYSTEM_STAMP) ;

include $(SYSTEM_MK)

# Zephyr-80 platform build configuration (include paths and the
# declarative constant files consumed by the build and layout tools).
include platform/zephyr80/config.mk

# The standalone SDK release version (`make sdk`).
include config/sdk.mk

ASFLAGS   := $(KOS_PLATFORM_ASFLAGS) -Itests/target/include
# Kernel and acceptance sources include their private interfaces by
# explicit owning path (kernel/...), so the nucleus build keeps `-I .`
#; the module/driver build (DRV_CFLAGS, MOD_CFLAGS)
# does not get this path, so a MOD cannot reach a kernel-private header.
CFLAGS    := -mz80 -c --nostdinc --Werror $(KOS_SDCC_OPT) \
             -I . $(KOS_PLATFORM_CFLAGS)
# The image's own flags, straight from its system file's `cflags` field.
CFLAGS    += $(KOS_SYSTEM_CFLAGS)
# The read-path phase instrument: empty in every shipped build, and the
# `phase-image` target's sub-make sets it to -DKOS_IOC_PHASES=1, so the
# kernel and driver compiles below see the option and every other build
# compiles it out.  (The assembly's own switch is the generated
# `ioc_phase_build.inc`: sdasz80 cannot take a symbol from its command
# line.)  Nothing in a shipped image changes when the flag is empty.
KOS_PHASE_FLAGS ?=
CFLAGS    += $(KOS_PHASE_FLAGS)

ASM_OBJS  := $(BUILD)/reset.rel $(BUILD)/start.rel $(BUILD)/banking.rel \
             $(BUILD)/page_zero.rel $(BUILD)/syscall_entry.rel \
             $(BUILD)/irq_entry.rel $(BUILD)/irq_vectors.rel \
             $(BUILD)/platform_irq.rel $(BUILD)/sdcc_helpers.rel \
             $(BUILD)/bank7_end.rel $(BUILD)/rom_access.rel \
             $(BUILD)/process_launch.rel $(BUILD)/user_copy.rel \
             $(BUILD)/window_copy.rel \
             $(BUILD)/upcall.rel $(BUILD)/diag_io.rel \
             $(BUILD)/ipc_asm.rel $(BUILD)/ipc_common.rel \
             $(BUILD)/event_common.rel \
             $(BUILD)/driver_boot.rel
# Handle-pool, path and CWD machinery plus the ROMFS
# reader behind `/system`.
VFS_OBJS := $(BUILD)/vfs_path.rel \
               $(BUILD)/vfs_handles.rel $(BUILD)/vfs_workspace.rel
C_OBJS    := $(BUILD)/early_console.rel $(BUILD)/panic.rel $(BUILD)/main.rel \
             $(BUILD)/syscall.rel $(BUILD)/diag.rel \
             $(BUILD)/event.rel $(BUILD)/event_syscall.rel \
             $(BUILD)/xmem.rel \
             $(BUILD)/irq.rel \
             $(BUILD)/irq_sources.rel \
             $(BUILD)/romfs.rel $(BUILD)/process.rel $(BUILD)/process_exec.rel \
             $(BUILD)/name.rel \
             $(VFS_OBJS)
# Resident-driver IPC substrate and the driver loader.  The substrate
# is fixed-required - the endpoint table, the call machinery, the
# descriptor table and the boot gate execute while the window shows bank 6
# - and the loader's load pass is a cold-half object.  Both lists are
# pinned by config/residency.json and the checked-in linker file.
# The substrate has no syscall-body object of its own: SYS_CALL is in the
# object family.
IPC_SUBSTRATE_OBJS := $(BUILD)/ipc.rel $(BUILD)/ipc_asm.rel \
             $(BUILD)/ipc_common.rel $(BUILD)/driver_boot.rel
IPC_COLD_OBJS := $(BUILD)/driver_loader.rel
END_OBJ   := $(BUILD)/irq_code_end.rel
# OBJS is assigned after the acceptance/catalog selection below, once
# every conditional object append has been decided.

# --- image acceptance wiring -------------------------------------------------
# The image's composition - its drivers, its boot program, its store and its
# C flags - is systems/<name>.json, and $(SYSTEM_MK) carries it into the
# build.  What stays here is the build-side wiring of the acceptance images:
# the objects they link beyond the kernel, their image-name suffix, the
# mapcheck cold override and the toolchain flags only they need.  Every
# entry is keyed on the system name, so no per-image `ifeq` block selects a
# build.
#
# The revision-13 mode-11 window is 6000h-DFFFh (architecture section 2.2);
# the fixed core is 2000h-5FFFh.  Every image links that way.
KOS_REV13_WINDOW_BASE := 0x6000

# Acceptance images link the shared runner (tests/target/support/) plus the
# per-subsystem test code under tests/target/; production images link none
# of it.  KOS_TARGET_TEST_BUILD is the single production-side guard
# (kernel/main.c holds only the two TEST-ONLY HOOKs).
TARGET_TEST_OBJS :=
SYSTEM_ACCEPTANCE := subsystem fs ipc nucleus

# The image-name suffix of each system: the image targets keep their names
# and build into their own directory, build-<image>/konaos<PK_SUFFIX>.bin.
SYSTEM_SUFFIX.subsystem := -subsys-selftest
SYSTEM_SUFFIX.console-serial := -console-serial-selftest
SYSTEM_SUFFIX.console-serial-missing := -console-serial-fail-selftest
SYSTEM_SUFFIX.console-init-fail := -driver-init-fail-selftest
SYSTEM_SUFFIX.fs := -fs-selftest
SYSTEM_SUFFIX.ipc := -ipc-selftest
SYSTEM_SUFFIX.nucleus := -nucleus-selftest
SYSTEM_SUFFIX.ns := -ns-selftest
SYSTEM_SUFFIX.xmem := -xmem-selftest
SYSTEM_SUFFIX.helloworld := -helloworld
SYSTEM_SUFFIX.boot-missing := -boot-missing-selftest
SYSTEM_SUFFIX.boot-corrupt := -boot-corrupt-selftest
# The image-name suffix, after the table above: an immediate assignment
# cannot see the rows that follow it.
PK_SUFFIX := $(SYSTEM_SUFFIX.$(SYSTEM))

# The mechanism-selftest images: the subsystem
# image carries the syscall gate, the IRQ executive, the ROMFS reader and
# the process/EXE lifecycle; the nucleus image carries the
# revision-13 gate and upcall-shim surface.  One image per surface group.
SYSTEM_TEST_OBJS.subsystem := $(BUILD)/syscall_test.rel \
                    $(BUILD)/syscall_selftest_asm.rel \
                    $(BUILD)/irq_test.rel $(BUILD)/irq_selftest_asm.rel \
                    $(BUILD)/ctc_test.rel \
                    $(BUILD)/romfs_test.rel \
                    $(BUILD)/process_test.rel
SYSTEM_TEST_OBJS.nucleus := $(BUILD)/nucleus_test.rel \
                    $(BUILD)/nucleus_selftest_asm.rel \
                    $(BUILD)/ctc_test.rel

# The IPC acceptance image: the gate object needs the
# generated build include, and the substrate and the loader are
# production-linked objects now (OBJS carries them), so this image adds
# nothing to the fixed/cold kernel sets - listing them again duplicated
# every symbol in the probe link.
SYSTEM_ASFLAGS.ipc := -I$(BUILD)
SYSTEM_FIXED_OBJS.ipc := $(IPC_SUBSTRATE_OBJS)

# The namespace image links the namespace family cold, where production
# links it (the family compiles cold, and so do the `moves` objects that
# still hold the handle pool and the path/CWD machinery it calls: this
# image's cold half has the room, and it is the moves tier's documented
# fallback, architecture 9).
SYSTEM_COLD_EXTRA.ns := vfs_router,vfs_system,object,vfs_file

# The namespace image sets the guard variable its selection block always
# set; the acceptance images below compile the guard in as a flag.
SYSTEM_VFS_WS_GUARD.ns := 1

# The required-console failure store: the serial
# variant's set and boot manifest, with the console image left out of the
# store.  The name stays in drivers.man, so the loader's lookup fails and
# the boot must stop explicitly on the early console (5.3) instead of
# falling back.
SYSTEM_OMIT.console-serial-missing := --omit-image console-serial
# The missing-boot-program store: the boot setting names shell.exe and the
# packed store does not carry it, so the launcher's failure path falls back
# to the monitor.
SYSTEM_OMIT.boot-missing := --omit-store shell.exe
# The generator's own flags, after the table above.
SYSTEM_GEN_FLAGS := $(SYSTEM_OMIT.$(SYSTEM))

TARGET_TEST_OBJS += $(SYSTEM_TEST_OBJS.$(SYSTEM))
ASFLAGS += $(SYSTEM_ASFLAGS.$(SYSTEM))
KOS_VFS_WS_GUARD := $(SYSTEM_VFS_WS_GUARD.$(SYSTEM))


OBJS      := $(ASM_OBJS) $(C_OBJS) $(BUILD)/ipc.rel $(IPC_COLD_OBJS) \
             $(BUILD)/vfs_router.rel $(BUILD)/vfs_dev.rel \
             $(BUILD)/vfs_fs.rel $(BUILD)/vfs_system.rel \
             $(BUILD)/object.rel


# Nucleus residency (architecture 4.2; two-tier classification).  Every
# production image is a
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
# The classification is two-tier (fixed-required and cold) and places
# every object.  The namespace family and the VFS core are
# production cold code (the syscall dispatcher and the driver loader run
# with P = 0); an acceptance image with its own cold set passes it here
# (the IPC acceptance image does).
COLD_EXTRA := $(SYSTEM_COLD_EXTRA.$(SYSTEM))
COLD_EXTRA_CSV := $(COLD_EXTRA)
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
ifneq ($(filter $(SYSTEM),$(SYSTEM_ACCEPTANCE)),)
CFLAGS    += -DKOS_TARGET_TEST_BUILD=1 -DKOS_VFS_WS_GUARD=1 \
		-Itests/target/include
TARGET_TEST_OBJS := $(BUILD)/target_test.rel \
		$(if $(filter $(SYSTEM),fs),,$(BUILD)/driver_client.rel) \
		$(TARGET_TEST_OBJS)
endif

# The two-tier placement decides which objects compile into the window
# areas, so a residency change must rebuild them: without this, an object
# stays in the area its previous classification chose and the mapcheck
# rule fires on a stale .rel.
$(C_OBJS) $(COLD_OBJS): $(RESIDENCY)

# The cold placement follows the target-test list: the acceptance objects
# (and the shared driver client every driver diagnostic links) are
# compiled into the window areas at 6000h-DFFFh, exactly like the
# production cold half, so test code consumes the cold half's slack and
# never the fixed core's leftover.  This block sits after the list it
# filters.
COLD_OBJS  := $(addprefix $(BUILD)/,$(addsuffix .rel,$(COLD_STEMS)))
ifneq ($(TARGET_TEST_OBJS),)
COLD_OBJS  += $(filter-out $(SYSTEM_FIXED_OBJS.$(SYSTEM)) $(TEST_FIXED_OBJS),$(TARGET_TEST_OBJS))
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

IMAGE := $(BUILD)/konaos$(PK_SUFFIX).bin
MAME_IMAGE := $(BUILD)/zephyr80$(PK_SUFFIX).bin

# Memory-layout source for this build: the platform memory.inc is the one
# declarative source (the two-region split and the stack reservations are
# production constants now), and the derived variant below adds the
# per-image geometry that every assembly unit and layout tool reads.
GEOMETRY_DIR       := $(BUILD)/geometry
GEOMETRY_PROBE_DIR := $(BUILD)/probe
ASM_MEMORY_INC     := $(KOS_PLATFORM_MEMORY_INC)
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

$(GEOMETRY_INC) $(GEOMETRY_MEMORY) &: $(BUILD)/konaos.probe.map \
		tools/mapcheck.py $(ASM_MEMORY_INC) $(RESIDENCY) | $(BUILD)
	mkdir -p $(GEOMETRY_DIR)
	$(PYTHON) tools/mapcheck.py geometry derive $(BUILD)/konaos.probe.map \
		$(ASM_MEMORY_INC) $(GEOMETRY_INC) $(GEOMETRY_MEMORY) \
		$(RESIDENCY_DERIVE_FLAGS)

$(GEOMETRY_PROBE_DIR)/bank7_end.inc: $(KOS_PLATFORM_MEMORY_INC) \
		tools/mapcheck.py | $(BUILD)
	mkdir -p $(GEOMETRY_PROBE_DIR)
	$(PYTHON) tools/mapcheck.py geometry probe $(KOS_PLATFORM_MEMORY_INC) $@

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

$(BUILD)/konaos.probe.lk: $(PROBE_OBJS) | $(BUILD)
	{ \
	  printf '%s\n' '-n' '-m' '-w'; \
	  printf '%s\n' '-i $(BUILD)/konaos.probe.ihx'; \
	  printf '%s\n' '-b _CODE = 0x2000' '-b _DATA = 0x5A00' \
	                '-b _WINDOW = $(KOS_REV13_WINDOW_BASE)' \
	                '-b _WINDOW_DATA = 0xD800' '-b _COMMON = 0xE000'; \
	  printf '%s\n' $(PROBE_OBJS); \
	  printf '%s\n' '-e'; \
	} > $@

$(BUILD)/konaos.probe.ihx $(BUILD)/konaos.probe.map: \
		$(BUILD)/konaos.probe.lk
	$(SDLDZ80) -f $(BUILD)/konaos.probe.lk

.PHONY: all test check check-fast clean mod-test romfs-test exe-test \
	monitor-test ioc-test hid-test v9958-test sdk \
	subsystem-selftest nucleus-selftest \
	console-serial-selftest \
	fs-selftest ns-selftest ipc-selftest xmem-selftest helloworld-image \
	phase-image \
	hardware-images \
	smoke-hardware \
	smoke-hardware-hwtest \
	smoke-hardware-subsystem \
	smoke-hardware-console-serial \
	smoke-hardware-fs smoke-hardware-shell

all: $(IMAGE) $(MAME_IMAGE)

$(BUILD):
	mkdir -p $@

# --- assemble ----------------------------------------------------------------

$(BUILD)/reset.rel: boot/shadow_copy.asm \
		platform/zephyr80/public/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/start.rel: boot/start.asm \
		platform/zephyr80/public/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/banking.rel: kernel/memory/banking.asm \
		platform/zephyr80/public/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/page_zero.rel: kernel/syscall/page_zero.asm | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/syscall_entry.rel: kernel/syscall/entry.asm \
		platform/zephyr80/public/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/irq_entry.rel: kernel/irq/entry.asm platform/zephyr80/public/hardware.inc \
		$(ASM_MEMORY_INC) platform/zephyr80/private/irq.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/irq_vectors.rel: kernel/irq/vectors.asm \
		platform/zephyr80/private/irq.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/platform_irq.rel: kernel/irq/init.asm \
		platform/zephyr80/private/irq.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/sdcc_helpers.rel: kernel/runtime/sdcc_helpers.asm | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/bank7_end.rel: kernel/memory/bank7_end.asm $(GEOMETRY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -I$(dir $(GEOMETRY_INC)) -o $@ $<

$(BUILD)/rom_access.rel: kernel/romfs/rom_access.asm \
		platform/zephyr80/public/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/process_launch.rel: kernel/process/launch.asm \
		platform/zephyr80/public/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/user_copy.rel: kernel/memory/user_copy.asm \
		platform/zephyr80/public/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/window_copy.rel: kernel/memory/window_copy.asm \
		platform/zephyr80/public/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/upcall.rel: kernel/memory/upcall.asm \
		platform/zephyr80/public/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/diag_io.rel: kernel/syscall/diag_io.asm | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/event_common.rel: kernel/event/event_common.asm \
		platform/zephyr80/public/hardware.inc \
		$(ASM_MEMORY_INC) kernel/event/event.h | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/irq_code_end.rel: kernel/irq/code_end.asm | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

# --- compile -----------------------------------------------------------------
# early_console.c and panic.c are compiled into the _COMMON segment so the
# whole early-console/panic path stays resident in the permanently mapped
# common region (E000h-FEFFh).

$(BUILD)/early_console.rel: kernel/early_console.c \
		kernel/include/kernel.h kernel/include/platform.h \
		abi/kos/types.h platform/zephyr80/public/hardware.h | $(BUILD)
	$(SDCC) $(CFLAGS) --codeseg COMMON $< -o $@

$(BUILD)/panic.rel: kernel/panic.c \
		kernel/include/kernel.h kernel/include/platform.h \
		abi/kos/types.h platform/zephyr80/public/hardware.h | $(BUILD)
	$(SDCC) $(CFLAGS) --codeseg COMMON $< -o $@

$(BUILD)/main.rel: kernel/main.c kernel/include/kernel.h \
		kernel/include/irq_types.h \
 kernel/include/process.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# Device events (architecture 4.10).  The post path is fixed-core code: the
# driver interrupt that calls it runs with the driver's window mapped, where
# the cold half is not visible.  The syscall and teardown bodies are cold like
# the rest of the dispatcher.
$(BUILD)/event.rel: kernel/event/event.c kernel/event/event.h \
		abi/kos/types.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/event_syscall.rel: kernel/event/event_syscall.c kernel/event/event.h \
		abi/kos/syscall.h abi/kos/errno.h abi/kos/ipc.h \
		abi/kos/protocol.h formats/include/mod.h \
		kernel/ipc/ipc.h kernel/vfs/vfs.h kernel/syscall/diag.h \
		kernel/include/process.h kernel/include/platform.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/irq.rel: kernel/irq/irq.c kernel/include/irq_types.h \
		abi/kos/types.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/irq_sources.rel: kernel/irq/ctc.c \
		kernel/include/irq_types.h kernel/include/platform.h \
		platform/zephyr80/public/hardware.h platform/zephyr80/private/irq.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/syscall.rel: kernel/syscall.c kernel/syscall/diag.h \
		abi/kos/syscall.h abi/kos/errno.h \
		kernel/include/kernel.h kernel/include/process.h \
		| $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/diag.rel: kernel/syscall/diag.c kernel/syscall/diag.h \
		abi/kos/syscall.h kernel/include/process.h \
		kernel/include/platform.h abi/kos/errno.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# Extended-memory family: the xmem syscall bodies, the
# object table and the aperture token.  Cold code like the rest of the
# syscall dispatcher (P = 0 while it runs).
$(BUILD)/xmem.rel: kernel/syscall/xmem.c kernel/syscall/xmem.h \
		abi/kos/xmem.h abi/kos/syscall.h kernel/include/process.h \
		abi/kos/errno.h kernel/ipc/ipc.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@


# --- ROMFS objects (bank-7 reader + common ROM-access primitive) -------------

# Shared kernel ASCII name comparison: one helper for the
# namespace router and ROMFS.
$(BUILD)/name.rel: kernel/lib/name.c kernel/lib/name.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/romfs.rel: kernel/romfs/romfs.c formats/include/romfs.h \
 kernel/include/platform.h \
		kernel/lib/name.h platform/zephyr80/private/memory.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# --- VFS objects (one part per file) ------------------------------------------

$(BUILD)/vfs_path.rel: kernel/vfs/path.c kernel/vfs/vfs.h \
		abi/kos/fs.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/vfs_handles.rel: kernel/vfs/handles.c kernel/vfs/vfs.h \
		abi/kos/fs.h \
		kernel/include/process.h abi/kos/errno.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/vfs_workspace.rel: kernel/vfs/workspace.c kernel/vfs/vfs.h \
		abi/kos/fs.h \
		kernel/include/kernel.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/vfs_router.rel: kernel/vfs/router.c kernel/vfs/vfs.h \
		abi/kos/object.h drivers/include/driver.h formats/include/romfs.h \
		abi/kos/errno.h kernel/ipc/ipc.h kernel/syscall/diag.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/vfs_dev.rel: kernel/vfs/dev.c kernel/vfs/vfs.h \
		abi/kos/object.h abi/kos/errno.h kernel/lib/name.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/vfs_fs.rel: kernel/vfs/fs.c kernel/vfs/vfs.h \
		abi/kos/fs.h abi/kos/protocol.h drivers/include/driver.h \
		kernel/ipc/ipc.h kernel/syscall/diag.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/vfs_system.rel: kernel/vfs/system.c kernel/vfs/vfs.h \
		abi/kos/object.h abi/kos/protocol.h drivers/include/driver.h \
		formats/include/romfs.h abi/kos/errno.h kernel/ipc/ipc.h \
		kernel/syscall/diag.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/object.rel: kernel/syscall/object.c kernel/syscall/object.h \
		kernel/syscall/diag.h kernel/vfs/vfs.h \
		abi/kos/object.h abi/kos/ipc.h abi/kos/syscall.h \
		abi/kos/errno.h kernel/include/process.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@



# --- process lifecycle objects (bank-7 code and data) ------------------------

$(BUILD)/process.rel: kernel/process/process.c \
		kernel/include/process.h formats/include/exe.h \
		kernel/include/kernel.h abi/kos/errno.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/process_exec.rel: kernel/process/exec.c \
		kernel/include/process.h formats/include/exe.h \
		formats/include/romfs.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# --- MOD modules (host-built, packed, embedded per-image catalogs) ----------
# Every image packs its own module directory ($(BUILD)/modules): the packed
# bytes are deterministic, so the same bytes reappear in any placement
# build, and an image that boots a *different* resident-driver set or
# carries its own fixtures cannot disturb another image's store.  The
# canonical build keeps the build/modules/ path the host tests
# read.  Each image embeds the boot module plus (in self-test builds) the
# fixtures through a generated assembly blob emitted by tools/modpack.py.

MODULES   := $(BUILD)/modules
# Every module sees the shared driver-facing roots and its own driver
# directory context; no sibling driver's private path and no `.` (package
# 46 T3).  A driver's local headers resolve through same-directory quote
# includes.
MOD_CFLAGS := -mz80 -c --nostdinc --Werror $(KOS_SDCC_OPT) \
              -I abi -I drivers/include -I platform/zephyr80/public \
              -I $(BUILD) -I drivers/sio

$(MODULES):
	mkdir -p $@

# --- bank-6 resident drivers ------------------------------
# The SIO, IOC, HID, V9958 and console driver stacks are bank-6 drivers:
# each is packed as a driver image (MOD module_type 2) into the recovery
# store, listed by the boot manifest `drivers.man` (KOS_DRIVERS, in load
# order) and placed in bank 6 by the nucleus loader
# (kernel/driver/driver_loader.c).  Per 5.2 the hot tier is _CODE with
# its BSS in _DRIVER_IO (the one bank-6 range both the mode-11 window and
# the mode-10 aperture show, so descriptors and `move` name it), the boot
# tier is _BOOT/_BOOT_DATA, and the V9958 font is a bulk segment the
# run-time display reset re-reads through the nucleus bulk copy.
#
# The driver cores (sio.c, ioc.c, hid.c, v9958.c) hold the device logic;
# the *_proto.c halves own the protocol dispatch and
# init, the *_boot.c halves the boot tier.  Every image is a separate
# object set, so each packed byte is identical across placement builds.
DRV_CFLAGS := -mz80 -c --nostdinc --Werror $(KOS_SDCC_OPT) \
              -I abi -I drivers/include -I platform/zephyr80/public \
              -I $(BUILD)
# The phase instrument's C half: the same switch the kernel build gets
# (KOS_PHASE_FLAGS is empty unless the phase-image sub-make sets it).
DRV_CFLAGS += $(KOS_PHASE_FLAGS)
DRV_HOT    := $(DRV_CFLAGS) --codeseg CODE --dataseg DRIVER_IO
DRV_BOOT   := $(DRV_CFLAGS) --codeseg BOOT --dataseg BOOT_DATA

$(MODULES)/sio.rel: drivers/sio/sio.c drivers/sio/sio.h \
		drivers/include/driver.h kernel/include/platform.h | $(MODULES)
	$(SDCC) $(DRV_HOT) $< -o $@

$(MODULES)/sio_io.rel: drivers/sio/sio_io.asm \
		platform/zephyr80/public/hardware.inc | $(MODULES)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(MODULES)/sio_driver.rel: drivers/sio/sio_proto.c drivers/sio/sio.h \
		drivers/include/driver.h $(DRV_PROTOCOL_H) | $(MODULES)
	$(SDCC) $(DRV_HOT) $< -o $@

$(MODULES)/sio_boot.rel: drivers/sio/sio_boot.c drivers/sio/sio.h \
		drivers/include/driver.h $(DRV_PROTOCOL_H) | $(MODULES)
	$(SDCC) $(DRV_BOOT) $< -o $@

$(MODULES)/ioc.rel: drivers/ioc/ioc.c drivers/ioc/ioc_driver.h \
		drivers/include/ioc_phases.h \
		drivers/include/ioc_phases_clock.h \
		drivers/ioc/ioc_proto.h drivers/include/ioc.h \
		drivers/include/ioc_protocol.h \
 kernel/include/platform.h | $(MODULES)
	$(SDCC) $(DRV_HOT) $< -o $@

# The read-path instrument's assembly switch.  sdasz80 is given no way to
# define a symbol, so the build materializes KOS_PHASE_FLAGS as an
# assignment the bulk loop's source includes (`.if ioc_phase_build`): 0
# for every shipped build, 1 only in the sub-make `phase-image` runs.  The
# value is derived from the same variable the C half consumes, so the two
# halves of the instrument cannot be switched on separately.
$(BUILD)/ioc_phase_build.inc:
	@mkdir -p $(dir $@)
	@printf 'ioc_phase_build = %s\n' $(if $(KOS_PHASE_FLAGS),1,0) > $@

$(MODULES)/ioc_io.rel: drivers/ioc/ioc_io.asm \
		platform/zephyr80/public/hardware.inc \
		$(BUILD)/ioc_phase_build.inc | $(MODULES)
	$(SDASZ80) $(ASFLAGS) -I$(BUILD) -o $@ $<

$(MODULES)/ioc_driver.rel: drivers/ioc/ioc_proto.c \
		drivers/ioc/ioc_driver.h drivers/ioc/ioc_proto.h \
		drivers/include/ioc.h drivers/include/ioc_protocol.h \
		drivers/include/driver.h $(DRV_PROTOCOL_H) | $(MODULES)
	$(SDCC) $(DRV_HOT) $< -o $@

$(MODULES)/ioc_boot.rel: drivers/ioc/ioc_boot.c drivers/ioc/ioc_driver.h \
		drivers/include/ioc_phases_clock.h \
		drivers/include/driver.h $(DRV_PROTOCOL_H) | $(MODULES)
	$(SDCC) $(DRV_BOOT) $< -o $@

$(MODULES)/timer.rel: drivers/timer/timer.c \
		drivers/include/driver.h \
		platform/zephyr80/public/hardware.h | $(MODULES)
	$(SDCC) $(DRV_HOT) $< -o $@

$(MODULES)/timer_io.rel: drivers/timer/timer_io.asm \
		platform/zephyr80/public/hardware.inc | $(MODULES)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(MODULES)/timer_driver.rel: drivers/timer/timer_proto.c \
		drivers/include/driver.h abi/kos/protocol.h | $(MODULES)
	$(SDCC) $(DRV_HOT) $< -o $@

$(MODULES)/timer_boot.rel: drivers/timer/timer_boot.c \
		platform/zephyr80/public/hardware.h | $(MODULES)
	$(SDCC) $(DRV_BOOT) $< -o $@

$(MODULES)/hid.rel: drivers/hid/hid.c drivers/hid/hid_driver.h \
		drivers/include/ioc_protocol.h \
		drivers/include/ioc.h \
		drivers/include/driver.h $(DRV_PROTOCOL_H) \
		kernel/include/platform.h | $(MODULES)
	$(SDCC) $(DRV_HOT) $< -o $@

$(MODULES)/v9958.rel: drivers/v9958/v9958.c drivers/v9958/v9958_driver.h \
		drivers/include/driver.h $(DRV_PROTOCOL_H) \
		kernel/include/platform.h | $(MODULES)
	$(SDCC) $(DRV_HOT) $< -o $@

$(MODULES)/v9958_io.rel: drivers/v9958/v9958_io.asm \
		platform/zephyr80/public/hardware.inc | $(MODULES)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(MODULES)/v9958_driver.rel: drivers/v9958/v9958_proto.c \
		drivers/v9958/v9958_driver.h \
		drivers/include/driver.h $(DRV_PROTOCOL_H) | $(MODULES)
	$(SDCC) $(DRV_HOT) $< -o $@

$(MODULES)/v9958_boot.rel: drivers/v9958/v9958_boot.c \
		drivers/v9958/v9958_driver.h drivers/include/driver.h \
		$(DRV_PROTOCOL_H) | $(MODULES)
	$(SDCC) $(DRV_BOOT) $< -o $@

# The font bulk segment: the pinned CP850 6x8 bitmap, assembled straight
# from the reviewable .inc asset (the same source tools/fontpack.py packs
# as the ROMFS DATA copy).
$(MODULES)/v9958_font.rel: drivers/v9958/v9958_font.asm \
		drivers/v9958/assets/font_cp850_6x8.inc | $(MODULES)
	$(SDASZ80) $(ASFLAGS) -Idrivers/v9958/assets -o $@ $<

# The two build-selected CONSOLE drivers are one source compiled twice
#: the production HID-input/VIDEO-output image and the
# SERIAL image.  Each packed container names its own object.
$(MODULES)/console-v9958-hid.rel: drivers/console/console_proto.c \
		drivers/include/driver.h abi/kos/protocol.h \
		abi/kos/errno.h | $(MODULES)
	$(SDCC) $(DRV_HOT) $< -o $@

$(MODULES)/console-serial.rel: drivers/console/console_proto.c \
		drivers/include/driver.h abi/kos/protocol.h \
		abi/kos/errno.h | $(MODULES)
	$(SDCC) $(DRV_HOT) -DKOS_CONSOLE_SERIAL=1 $< -o $@

# The filesystem driver's hot image, bank-6 driver code.  Its FS2 transfer
# staging,
# the 512-byte cache and the handle pools are `_DRIVER_IO`; the IOC
# dependency is host-resolved at pack time (`depends`).
$(MODULES)/iocfat.rel: drivers/iocfat/iocfat.c \
		drivers/include/ioc_phases.h \
		drivers/include/ioc_phases_clock.h \
		drivers/include/driver.h drivers/include/ioc.h \
		abi/kos/fs.h abi/kos/object.h abi/kos/protocol.h \
		abi/kos/errno.h | $(MODULES)
	$(SDCC) $(DRV_HOT) $< -o $@

$(MODULES)/sdcc_indirect_calls.rel: drivers/runtime/sdcc_indirect_calls.asm \
		| $(MODULES)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

# The image's driver set: the load order the system file declares.  Every
# driver's image is <id>.mod in this build's module directory, packed from
# the resolved manifest tools/systemgen.py wrote for it.  The packer links
# each image at its final bank-6 address against the whole set it is loaded
# with (--set, in load order): the tier plan, the endpoint numbers and the
# inter-driver dependencies are all host-computed, so every image of one
# build must see the same set, and every pack reads the whole set's objects
# ($(KOS_DRIVER_OBJECTS), from the system file).
DRV_SET_MODS := $(addprefix $(MODULES)/,$(addsuffix .mod,$(KOS_DRIVERS)))
DRV_PACK_SET := $(addprefix $(BUILD)/drivers/,$(addsuffix .json,$(KOS_DRIVERS)))

# One image per selected driver; the report carries the symbol offsets the
# replays read the boot markers and the placements through.
define drv_pack_rule
$(MODULES)/$(1).mod: $(BUILD)/drivers/$(1).json $(KOS_DRIVER_OBJECTS) \
		$(DRV_PACK_SET) tools/modpack.py | $(MODULES)
	$(PYTHON) tools/modpack.py pack $(BUILD)/drivers/$(1).json $$@ \
		--objdir $(MODULES) --set $(DRV_PACK_SET) \
		--report $(MODULES)/$(1).report.json

endef
$(foreach drv,$(KOS_DRIVERS),$(eval $(call drv_pack_rule,$(drv))))

# ROMFS font asset (DATA): the reviewable .inc source is the pinned
# reference font; fontpack.py verifies the exact 2048 bytes and the
# pinned SHA-256 before emitting the deterministic binary.
$(MODULES)/v9958-6x8.font: drivers/v9958/assets/font_cp850_6x8.inc \
		tools/fontpack.py | $(MODULES)
	$(PYTHON) tools/fontpack.py $< $@

# --- target acceptance objects (tests/target/, acceptance builds only) -------

$(BUILD)/target_test.rel: tests/target/support/target_test.c \
		kernel/include/kernel.h tests/target/include/syscall_selftest.h \
		tests/target/include/irq_selftest.h \
		tests/target/include/romfs_selftest.h \
		tests/target/include/process_selftest.h \
		abi/kos/ipc.h \
		formats/include/mod.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# The resident-driver acceptance client every driver diagnostic links:
# endpoint lookup, descriptor-less calls and the copy gate.
$(BUILD)/driver_client.rel: tests/target/support/driver_client.c \
		tests/target/include/driver_client.h \
		kernel/include/kernel.h kernel/include/process.h \
		abi/kos/protocol.h drivers/include/driver.h \
		kernel/ipc/ipc.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# Test-owned CTC0 programming helper (acceptance images only), so no
# acceptance-only CTC code reaches the production map.
$(BUILD)/ctc_test.rel: tests/target/support/ctc_test.c \
		tests/target/include/ctc_test.h \
		platform/zephyr80/public/hardware.h platform/zephyr80/private/irq.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# IPC substrate and driver loader:
# the fixed-core substrate and its primitives (config/residency.json
# `fixed-required`), the cold syscall bodies, the cold resident-driver
# loader and the common boot gate.  The six synthetic drivers are real
# driver images packed into the recovery store and loaded from the boot
# manifest, not fixture blobs.
$(BUILD)/ipc.rel: kernel/ipc/ipc.c kernel/ipc/ipc.h \
		drivers/include/driver.h abi/kos/ipc.h \
		kernel/include/irq_types.h kernel/include/platform.h \
		kernel/include/kernel.h kernel/include/process.h \
		kernel/irq/irq_private.h kernel/memory/upcall.h \
		kernel/syscall/diag.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/ipc_asm.rel: kernel/ipc/ipc_asm.asm \
		platform/zephyr80/public/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/ipc_common.rel: kernel/ipc/ipc_common.asm \
		platform/zephyr80/public/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<


$(BUILD)/driver_loader.rel: kernel/driver/driver_loader.c \
		formats/include/mod.h drivers/include/driver.h \
 formats/include/romfs.h \
		abi/kos/errno.h kernel/include/kernel.h \
		kernel/include/process.h kernel/ipc/ipc.h \
		kernel/memory/upcall.h kernel/syscall/diag.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/driver_boot.rel: kernel/driver/driver_boot.asm \
		platform/zephyr80/public/hardware.inc $(ASM_MEMORY_INC) | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/syscall_test.rel: tests/target/syscall/syscall_test.c \
		kernel/include/kernel.h \
		tests/target/include/syscall_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/syscall_selftest_asm.rel: tests/target/syscall/syscall_entry.asm \
		tests/target/include/syscall_entry.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/irq_test.rel: tests/target/irq/irq_test.c \
		kernel/include/irq_types.h kernel/include/kernel.h \
		kernel/include/platform.h \
		tests/target/include/irq_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/irq_selftest_asm.rel: tests/target/irq/irq_entry.asm \
		tests/target/include/irq_entry.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/nucleus_test.rel: tests/target/nucleus/nucleus_test.c \
		kernel/include/irq_types.h kernel/include/kernel.h \
		kernel/include/platform.h abi/kos/diag.h \
		kernel/memory/upcall.h tests/target/include/nucleus_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/nucleus_selftest_asm.rel: tests/target/nucleus/nucleus_entry.asm \
		tests/target/include/nucleus_entry.inc | $(BUILD)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(BUILD)/romfs_test.rel: tests/target/romfs/romfs_test.c \
		formats/include/romfs.h \
		kernel/include/kernel.h \
		tests/target/include/romfs_selftest.h $(BUILD)/romfs_expect.inc | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@


# The process acceptance layout header is generated from the packed
# containers the process selftest runs (the SDK-produced exetest plus the
# loader-outcome programs).  Reading the
# containers themselves means the test can only agree with what the loader
# reads.  No program includes this header, so the generation cannot cycle
# through a program build.
$(BUILD)/process_expect.inc: $(BUILD)/user/exetest.exe \
		$(BUILD)/user/exe2probe.exe $(BUILD)/user/exe2seg.exe \
		$(BUILD)/user/exe2big.exe \
		tests/subsys/gen_process_expect.py \
		tests/subsys/gen_exe_fixtures.py tools/exepack.py | $(BUILD)
	$(PYTHON) tests/subsys/gen_process_expect.py $@ \
		PROCESS_EXE=$(BUILD)/user/exetest.exe \
		EXE2_PROBE=$(BUILD)/user/exe2probe.exe \
		EXE2_SEG=$(BUILD)/user/exe2seg.exe \
		EXE2_BIG=$(BUILD)/user/exe2big.exe

$(BUILD)/process_test.rel: tests/target/process/process_test.c \
		kernel/include/process.h abi/kos/syscall.h \
		kernel/include/kernel.h \
		tests/target/include/process_selftest.h $(BUILD)/process_expect.inc | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# --- native user programs (SDK) ------------------------
# Every EXE is built by one exe_program line from user/sdk/exe.mk.  The
# sources compile with exactly -I abi -I user/sdk/include plus their own
# directory (extended only by explicit test-owned INCLUDES), crt0 and libkos
# are linked, the manifest is generated from the link map, and exepack packs
# and validates build/user/<name>.exe.  STAGE=1 additionally copies the
# result to build/modules, which the recovery manifests consume.

include user/sdk/exe.mk

# The helloworld sub-project declares its own demos (one exe_program line
# per program) next to their sources.
include user/helloworld/Makefile

# The monitor calls the SDK's diag wrappers (`diag_mem_read`, ...) and the
# SDK's diag header is the only place they are declared: `kos.h` does not
# include it, so the monitor names the SDK's diag
# directory, which `"diag.h"` resolves to.
$(eval $(call exe_program,monitor,user/monitor,\
	monitor.c monitor_exec.c monitor_mem.c monitor_io.c hexfmt.c \
	hexdump.c status.c,\
	STACK=512,STAGE=1,INCLUDES=user/sdk/include/kos))

# The native shell: `/system/shell.exe`, the production boot program and
# an ordinary SDK program with no far code.  STAGE=1 puts the packed
# program where a system file's store consumes it, beside its monitor.
$(eval $(call exe_program,shell,user/shell,\
	main.c parse.c path.c lookup.c builtin.c fscmd.c,STACK=512,STAGE=1))

# The device-release outcome program: tests/test_device_release.py starts
# it from the monitor or the shell with one mode tail.  It holds the card
# copy, so it is named on the default target beside the shell.
$(eval $(call exe_program,devtest,tests/device,\
	devtest.c,STACK=256,INCLUDES=user/sdk/include/kos))

all: $(EXE_USER_ROOT)/devtest.exe

$(eval $(call exe_program,exetest,tests/target/process,\
	exetest.c exetest_sp.asm,STACK=256,STAGE=1,\
	INCLUDES=tests/target/include))

# EXE v2 loader-outcome programs (test only): the
# subsystem process selftest runs them to prove the 4.8 zero-only rule
# (BSS zeroed, bank not cleared), the DATA-segment wiring and the
# container-over-64-KiB path.  exe2seg and exe2big attach DATA segment
# payloads through SEGMENTS=; the payload files are generated next to the
# program's manifest so the packer resolves them against --objdir.
EXE2_SEG_DIR := $(EXE_USER_ROOT)/exe2seg
EXE2_BIG_DIR := $(EXE_USER_ROOT)/exe2big

$(EXE2_SEG_DIR)/exe2seg.segments.json: tests/subsys/gen_exe2_segments.py
	$(PYTHON) tests/subsys/gen_exe2_segments.py seg $(EXE2_SEG_DIR)

$(EXE2_BIG_DIR)/exe2big.segments.json: tests/subsys/gen_exe2_segments.py
	$(PYTHON) tests/subsys/gen_exe2_segments.py big $(EXE2_BIG_DIR)

$(eval $(call exe_program,exe2probe,tests/target/process,\
	exe2probe.c,STACK=256,STAGE=1,\
	INCLUDES=tests/target/include))

$(eval $(call exe_program,exe2seg,tests/target/process,\
	exe2seg.c,STACK=256,STAGE=1,\
	INCLUDES=tests/target/include,\
	SEGMENTS=$(EXE2_SEG_DIR)/exe2seg.segments.json))

$(eval $(call exe_program,exe2big,tests/target/process,\
	exe2big.c,STACK=256,STAGE=1,\
	INCLUDES=tests/target/include,\
	SEGMENTS=$(EXE2_BIG_DIR)/exe2big.segments.json))

# Filesystem outcome programs: the checks
# the image boots (`KOS_BOOT_PROGRAM=/system/fs_selftest.exe`) and the child
# it SYS_EXECs.  Both are ordinary SDK programs.
$(eval $(call exe_program,fs_selftest,tests/fs,\
	fs_selftest.c,STACK=384,STAGE=1))

$(eval $(call exe_program,fs_child,tests/fs,\
	fs_child.c,STACK=192,STAGE=1))

# Device-event outcome program: the `fs` image's `/system/events.exe`,
# run from the monitor's X.  It is the event mechanism's replay window
# (watch / loop / run / exit modes), so it lives in the filesystem image,
# which has the console, the card and now the timer driver.
$(eval $(call exe_program,events,tests/events,\
	events.c,STACK=384,STAGE=1))

# The I/O bench: the filesystem image's `/system/iobench.exe`, run from
# the monitor's X (`make fs-selftest` replays it), and a copy on the card
# for the bench runs the owner reads, so the default target builds it
# beside the other card programs.  It measures one row per I/O pattern
# with the TIMER clock and prints min/mean/max T-states per call.
$(eval $(call exe_program,iobench,tests/bench,\
	iobench.c,STACK=384,STAGE=1))

all: $(EXE_USER_ROOT)/iobench.exe

# The read-path measurement program (`make phase-image` builds it beside
# the instrumented image; the owner carries it on a card as READPH.EXE -
# the FAT 8.3 namespace cannot hold the source's name - and runs
# `readph BIG.BIN`).  It fetches the phase table the instrumented
# provider keeps and prints min/mean/max per phase; see its own header.
$(eval $(call exe_program,readphase,tests/perf,\
	readphase.c,STACK=384))

all: $(EXE_USER_ROOT)/readphase.exe

# SDK outcome programs for the console-order and stream replays
# (tests/test_io_sim.py): the `fs` image's `/system/iomix.exe` with its
# nested-exec child, and `/system/iostream.exe`, one card-file session
# through the stdio layer.
$(eval $(call exe_program,iomix,tests/io,\
	iomix.c,STACK=384,STAGE=1))

$(eval $(call exe_program,iomix_child,tests/io,\
	iomix_child.c,STACK=192,STAGE=1))

$(eval $(call exe_program,iostream,tests/io,\
	iostream.c,STACK=384,STAGE=1))

# The hardware exerciser: the production store's
# `/system/hwtest.exe` runner, started from the monitor's X command.  It
# drives the programs above and the xmem family below from the store, so
# it carries no checks of its own beyond the namespace stage.
$(eval $(call exe_program,hwtest,tests/hwtest,\
	hwtest.c,STACK=256,STAGE=1))

# The monotonic-clock outcome program: the production store's
# `/system/timer.exe`.  Run bare it is the owner's stopwatch check (one
# elapsed-second line per second); the `stage` tail runs the acceptance
# replay's checks.
$(eval $(call exe_program,timer,tests/timer,\
	timer.c,STACK=256,STAGE=1))

# The device-event board check: the production store's
# `/system/beat.exe`.  Run bare it subscribes to the timer's event source
# at 500 ms, toggles a 440 Hz PSG0 tone at every delivery for ten seconds
# and prints `count/missed` per second - the ear-and-stopwatch check of
# the event mechanism's deadlines on a board.
$(eval $(call exe_program,beat,tests/beat,\
	beat.c,STACK=384,STAGE=1,\
	INCLUDES=user/sdk/include/kos))

# Extended-memory outcome programs: the checks the image boots
# (`KOS_BOOT_PROGRAM=/system/xmem_selftest.exe`), the child it SYS_EXECs
# while its aperture is open, and the far-code program it SYS_EXECs last.
# All are ordinary SDK programs; xmem_far links two far halves into the
# _FAR1/_FAR2 areas at 8000h (FAR1=/FAR2=: one far segment per bank,
# ids 1 and 2) and its container declares the two FAR_CODE
# segments (SEGMENTS=).
$(eval $(call exe_program,xmem_selftest,tests/xmem,\
	xmem_selftest.c,STACK=512,STAGE=1))

$(eval $(call exe_program,xmem_child,tests/xmem,\
	xmem_child.c,STACK=256,STAGE=1))

$(eval $(call exe_program,xmem_far,tests/xmem,\
	xmem_far.c,STACK=384,STAGE=1,\
	FAR1=xmem_far_code.c,\
	FAR2=xmem_far_code2.c,\
	SEGMENTS=tests/xmem/xmem_far.segments.json))

# Far-data outcome program: the filesystem image's
# `/system/far_data.exe` runner, an ordinary SDK program that maps its own
# DATA1 segment, fills a mappable object straight through the window,
# reads the blob row and the store's `known.bin`, loads files whole with
# `xload` and proves the bank-exhaustion refusal.  Its DATA1 pair (a
# `const` table in `_DATA1`, a zeroed array in `_DATA1B`) and its FAR1
# half are the rule's own three rows; `banks.exe` is the hand-written
# six-row container that cannot fit in a bank budget, and both the program
# and the `known.bin` DATA file it reads are store files.
$(eval $(call exe_program,far_data,tests/fardata,\
	far_data.c,STACK=512,STAGE=1,\
	DATA1=far_data_levels.c,\
	FAR1=far_data_far.c,\
	BLOBS=far_data_blob.bin))

$(eval $(call exe_program,banks,tests/fardata,\
	banks.c,STACK=256,STAGE=1,\
	FAR1=banks_far.c,\
	BLOBS=far_data_blob.bin,\
	SEGMENTS=tests/fardata/banks.segments.json))

$(MODULES)/known.bin: tests/fardata/known.bin | $(MODULES)
	cp $< $@

$(eval $(call exe_program,ipc,tests/target/ipc,\
	ipc.c,STACK=256,STAGE=1,\
	INCLUDES=tests/target/include:user/sdk/src))

# IPC caller-buffer probe: the boot
# program of the IPC acceptance image.  NO_CRT0 with a custom entry and
# LOWBASE=0x0300 and CODEBASE=0x0344 put the SDK's 93-byte low-data area
# immediately above 0100h-02FFh, which is left as
# a plain 512-byte buffer: the only way a EXE image can name the caller
# window itself as a copy destination.
$(eval $(call exe_program,ipccopy,tests/target/ipc,\
	ipccopy.c ipccopy_entry.asm,NO_CRT0=1,ENTRY=ipc_copy_entry,\
	LOWBASE=0x0300,CODEBASE=0x0344,STAGE=1,\
	INCLUDES=tests/target/include))

# Namespace acceptance programs: the client the image
# boots (KOS_BOOT_PROGRAM=/system/ns.exe) and the child it SYS_EXECs.  Both
# are ordinary SDK programs.
$(eval $(call exe_program,ns,tests/ns,\
	ns.c,STACK=256,STAGE=1,\
	INCLUDES=tests/target/include))

$(eval $(call exe_program,nschild,tests/ns,\
	nschild.c,STACK=192,STAGE=1,\
	INCLUDES=tests/target/include))

# Deterministic malformed EXE containers (see
# tests/subsys/gen_exe_fixtures.py) shared by the subsystem acceptance
# manifest; the stamp makes `make clean && make exe-test` self-contained.
$(MODULES)/exe-fixtures.stamp: tests/subsys/gen_exe_fixtures.py \
		tools/exepack.py | $(MODULES)
	$(PYTHON) tests/subsys/gen_exe_fixtures.py $(MODULES)
	touch $@

# --- standalone SDK package --------------------------------
# `make sdk` assembles build/kos-sdk-<version>/ and its tarball: the SDK
# headers (with the ABI closure they include), the prebuilt libkos.lib and
# crt0.rel, the standalone tools (exepack, exemanifest, ihx), the program
# rules, the template, the examples and the docs.  A developer unpacks the
# tarball and builds a program with no OS source (docs/sdk.md, "Using the
# standalone SDK"); tests/test_sdk_package.py builds the tarball
# out-of-tree and runs the result.

KOS_SDK_DIR     := $(BUILD)/kos-sdk-$(KOS_SDK_VERSION)
KOS_SDK_TARBALL := $(BUILD)/kos-sdk-$(KOS_SDK_VERSION).tar.gz

SDK_INPUTS := $(wildcard abi/kos/*.h) \
	$(wildcard user/sdk/include/*.h) $(wildcard user/sdk/include/kos/*.h) \
	$(wildcard user/sdk/include/klibc/*.h) \
	$(wildcard user/sdk/template/*) $(wildcard user/sdk/examples/*) \
	docs/sdk.md docs/exe-format.md mk/kos.mk formats/include/exe.h \
	tools/exepack.py tools/exemanifest.py tools/ihx.py tools/sdkpack.py

sdk: $(KOS_SDK_TARBALL)
	@echo "sdk: $(KOS_SDK_DIR)"
	@echo "sdk: $(KOS_SDK_TARBALL)"

$(KOS_SDK_TARBALL): $(EXE_SDK_RELEASE_LIB) $(EXE_CRT0) $(SDK_INPUTS)
	$(PYTHON) tools/sdkpack.py --version $(KOS_SDK_VERSION) \
		--libkos $(EXE_SDK_RELEASE_LIB) --crt0 $(EXE_CRT0) \
		--out $(KOS_SDK_DIR) --tar $@

# --- ROMFS recovery store (ROM pages 2-7) ------------------------------------
# Every image packs the store tools/systemgen.py wrote for its system file:
# the driver images in load order, drivers.man, then the image's own files.
# The subsystem and nucleus acceptance images carry the loader/ROMFS/EXE
# fixtures, and tests/test_romfs_sim.py patches the packed subsystem image in
# place to exercise the kernel's validation (the corrupt-store outcome).
#
# The store's programs arrive through their own rules (one exe_program line
# each); the malformed EXE containers arrive together from one generator run,
# so a store that lists one of them waits for that run's stamp.
EXE_FIXTURES := $(filter $(MODULES)/exe-%,$(KOS_STORE_FILES))
ifneq ($(EXE_FIXTURES),)
$(EXE_FIXTURES): $(MODULES)/exe-fixtures.stamp
endif

$(BUILD)/recovery.romfs: $(BUILD)/store.json $(DRV_SET_MODS) \
		$(MODULES)/drivers.man $(MODULES)/drivers.plan.json \
		$(KOS_STORE_FILES) $(EXE_FIXTURES) tools/romfspack.py \
		$(KOS_PLATFORM_MEMORY_INC) | $(BUILD)
	$(PYTHON) tools/romfspack.py --memory-inc $(KOS_PLATFORM_MEMORY_INC) pack \
		$(BUILD)/store.json $@ --objdir $(MODULES)

# --- acceptance-only driver objects (tests/) --------------------------------
# The test-only drivers are ordinary driver images: their objects build like
# a production driver's (hot code with its DRIVER_IO data, a boot segment
# that records its single run, a bulk asset where the manifest names one) and
# their packs use the same rule as every other selected driver.  They build
# only when a system file selects them.  Their object names carry a prefix so
# they can never collide with a production driver's objects in one module
# directory.

# The six synthetic IPC drivers: hot and boot objects,
# and `con`'s hot-segment slow buffer (area _HOT), the deliberate driver-side
# buffer outside 8000h-BFFFh the path-C copy case needs.  --dataseg DRIVER_IO
# names the data area after the rule it has to satisfy: the loader places it
# inside bank 6's 8000h-BFFFh (architecture 4.9).  These
# test drivers also get the platform-private IRQ constants
# (platform/zephyr80/private/irq.h) that the `irqx` driver programs CTC0
# with; the production driver roots stay public-only.
$(MODULES)/ipc_%.rel: tests/target/ipc/%.c tests/target/include/ipc_test.h \
		| $(MODULES)
	$(SDCC) $(MOD_CFLAGS) --dataseg DRIVER_IO -Itests/target/include \
		-Iplatform/zephyr80/private $< -o $@

$(MODULES)/ipc_%_boot.rel: tests/target/ipc/%_boot.c \
		tests/target/include/ipc_test.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) --codeseg BOOT --dataseg BOOT_DATA \
		-Itests/target/include $< -o $@

$(MODULES)/ipc_con_slow.rel: tests/target/ipc/con_slow.c \
		tests/target/include/ipc_test.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) --dataseg HOT -Itests/target/include $< -o $@

# The namespace acceptance synthetic publisher: one real
# driver image whose /dev publications travel in the container the loader
# reads.
$(MODULES)/nsdev.rel: tests/target/ns/nsdev.c \
		tests/target/include/ns_test.h drivers/include/driver.h | $(MODULES)
	$(SDCC) $(DRV_HOT) -Itests/target/include $< -o $@

$(MODULES)/nsdev_boot.rel: tests/target/ns/nsdev_boot.c \
		tests/target/include/ns_test.h | $(MODULES)
	$(SDCC) $(DRV_BOOT) -Itests/target/include $< -o $@

# The required-driver init-failure acceptance driver: the
# test-only image whose init returns non-zero, so the loader's status branch
# refuses it and the boot stops on the early console (5.3).
$(MODULES)/init_fail.rel: tests/target/console/init_fail.c \
		drivers/include/driver.h | $(MODULES)
	$(SDCC) $(DRV_HOT) $< -o $@
# The boot manifest `drivers.man` is generated by tools/systemgen.py: the
# flat ROMFS names in load order, one 24-byte record each, and the packer's
# `plan` command validates the packed set the manifest lists (unique
# endpoints, resolvable dependencies, a tier allocation that fits) and
# writes the two driver ledgers.

# --- boot-program setting --------------------------------------
# tools/systemgen.py validated KOS_BOOT_PROGRAM against the store it wrote -
# an absolute path to one file directly under /system, at most
# KOS_PATH_MAX - 1 bytes, naming an EXE the store carries - and the setting
# travels as a header, never as a -D value: SDCC 4.5.0's driver aborts when
# any argument contains ".exe" (SDCCutil.c's setPrefixSuffix asserts the
# preprocessor command line free of the extension) and every boot program is
# an EXE.  The header depends on the generated system file, so a parallel
# build cannot link a kernel whose boot path was never validated.
BOOT_PROGRAM_H := $(BUILD)/boot_program.h

$(BOOT_PROGRAM_H): $(SYSTEM_STAMP)
	printf '%s\n' \
	    '#define KOS_BOOT_PROGRAM "$(KOS_BOOT_PROGRAM)"' \
	    '#define KOS_BOOT_AFTER_MONITOR $(if $(filter monitor,$(KOS_BOOT_AFTER_EXIT)),1,0)' \
	    > $@

$(BUILD)/main.rel: $(BOOT_PROGRAM_H)

# The plan report: the packed set's tier bases and the two bank-6 ledgers,
# which tools/pack_image.py reads for its driver ledger lines.
$(MODULES)/drivers.plan.json: $(DRV_SET_MODS) tools/modpack.py | $(MODULES)
	$(PYTHON) tools/modpack.py plan $@ $(DRV_SET_MODS)

# Deterministic cross-page pattern asset for the subsystem store.
$(MODULES)/crosspage.bin: tests/subsys/gen_crosspage.py | $(MODULES)
	$(PYTHON) tests/subsys/gen_crosspage.py $@

# Expected-container constants for the ROMFS-reader target selftest: the
# packed size/sum plus the store-manifest-derived first-payload offset.
$(BUILD)/romfs_expect.inc: $(BUILD)/store.json $(MODULES)/sio.mod \
		tests/subsys/gen_romfs_expect.py | $(BUILD)
	$(PYTHON) tests/subsys/gen_romfs_expect.py $(BUILD)/store.json \
		$(MODULES)/sio.mod $@ SIO_MOD

# --- link --------------------------------------------------------------------
# The canonical `build/` output uses the checked-in linker/zephyr80.lk.
# Every self-test and fallback build (different BUILD dir and/or extra
# objects) generates its linker file from the same object list the link
# step depends on, so the lists cannot drift apart.

KOS_TEST_LINK := $(filter $(SYSTEM),subsystem nucleus fs)
ifeq ($(BUILD),build)
ifneq ($(KOS_TEST_LINK),)
GENERATED_LK := 1
endif
else
GENERATED_LK := 1
endif

ifeq ($(GENERATED_LK),1)
$(BUILD)/zephyr80.lk: $(OBJS) $(TARGET_TEST_OBJS) $(END_OBJ) \
		$(GEOMETRY_MEMORY) | $(BUILD)
	@data_base=$$(sed -n 's/^KOS_DATA_BASE = //p' $(GEOMETRY_MEMORY)); \
	test -n "$$data_base" || { echo "no derived data base in $(GEOMETRY_MEMORY)"; exit 1; }; \
	window_data_base=$$(sed -n 's/^KOS_WINDOW_DATA_BASE = //p' $(GEOMETRY_MEMORY)); \
	test -n "$$window_data_base" || { echo "no derived window data base in $(GEOMETRY_MEMORY)"; exit 1; }; \
	{ \
	  printf '%s\n' '-n' '-m' '-w'; \
	  printf '%s\n' '-i $(BUILD)/konaos.ihx'; \
	  printf '%s\n' '-b _CODE = 0x2000' "-b _DATA = $$data_base" \
	                '-b _WINDOW = $(KOS_REV13_WINDOW_BASE)' \
	                "-b _WINDOW_DATA = $$window_data_base" \
	                '-b _COMMON = 0xE000'; \
	  printf '%s\n' $(OBJS) $(TARGET_TEST_OBJS) $(END_OBJ); \
	  printf '%s\n' '-e'; \
	} > $@

$(BUILD)/konaos.ihx $(BUILD)/konaos.map: $(OBJS) $(TARGET_TEST_OBJS) \
		$(END_OBJ) $(BUILD)/zephyr80.lk $(GEOMETRY_MEMORY) | $(BUILD)
	$(SDLDZ80) -f $(BUILD)/zephyr80.lk
else
# Canonical build: the checked-in linker template supplies the production
# object list; the two derived data bases are substituted into
# build/zephyr80.lk.
$(BUILD)/zephyr80.lk: linker/zephyr80.lk $(GEOMETRY_MEMORY) | $(BUILD)
	@data_base=$$(sed -n 's/^KOS_DATA_BASE = //p' $(GEOMETRY_MEMORY)); \
	test -n "$$data_base" || { echo "no derived data base in $(GEOMETRY_MEMORY)"; exit 1; }; \
	window_data_base=$$(sed -n 's/^KOS_WINDOW_DATA_BASE = //p' $(GEOMETRY_MEMORY)); \
	test -n "$$window_data_base" || { echo "no derived window data base in $(GEOMETRY_MEMORY)"; exit 1; }; \
	sed -e "s/@DATA_BASE@/$$data_base/" \
	    -e "s/@WINDOW_DATA_BASE@/$$window_data_base/" \
	    linker/zephyr80.lk > $@

$(BUILD)/konaos.ihx $(BUILD)/konaos.map: $(BUILD)/zephyr80.lk \
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

$(IMAGE): $(BUILD)/konaos.ihx $(BUILD)/konaos.map \
		$(BUILD)/recovery.romfs \
		tools/pack_image.py tools/mapcheck.py $(MEMORY_INC) $(KOS_PLATFORM_IRQ_INC)
	$(PYTHON) tools/mapcheck.py $(MAPCHECK_FLAGS) $(RESIDENCY_FLAGS) \
		$(BUILD)/konaos.map \
		$(MEMORY_INC) $(KOS_PLATFORM_IRQ_INC)
	$(PYTHON) tools/pack_image.py $(MAPCHECK_FLAGS) $(BUILD)/konaos.ihx \
		$(BUILD)/konaos.map $(BUILD) "$(PK_SUFFIX)" $(MEMORY_INC)

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
# process/EXE lifecycle (late hook), and finally the launcher's monitor,
# started once the program's own session ends.  The declared boot set is
# the SIO serial driver plus
# the console driver that composes it, placed by the resident loader.

subsystem-selftest:
	$(MAKE) BUILD=build-subsys-selftest SYSTEM=subsystem \
		build-subsys-selftest/konaos-subsys-selftest.bin \
		build-subsys-selftest/zephyr80-subsys-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-subsys-selftest/konaos.map \
		build-subsys-selftest/geometry/memory.inc \
		$(KOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_subsys_sim.py' -v
	$(PYTHON) -m unittest discover -s tests -p 'test_romfs_sim.py' -v
	@echo "subsystem-selftest image: build-subsys-selftest/konaos-subsys-selftest.bin"

# The SDK's own tests (tests/test_sdk.py) and the generated-manifest
# checks need the packed demo and the argument test program, so the
# standard check always builds them.  Neither is part of a production
# image: the demo lives in user/helloworld/ and exetest is an acceptance
# program.
check-fast: $(EXE_USER_ROOT)/hello.exe $(MODULES)/exetest.exe
	$(PYTHON) -m unittest discover -s tests -v
	$(PYTHON) tools/mapcheck.py $(MAPCHECK_FLAGS) $(RESIDENCY_FLAGS) \
		$(BUILD)/konaos.map \
		$(MEMORY_INC) $(KOS_PLATFORM_IRQ_INC)
	$(PYTHON) tools/pack_image.py $(MAPCHECK_FLAGS) $(BUILD)/konaos.ihx \
		$(BUILD)/konaos.map $(BUILD) "$(PK_SUFFIX)" $(MEMORY_INC)
	sha256sum $(IMAGE) $(MAME_IMAGE)
	cmp $(IMAGE) $(MAME_IMAGE)
	@echo "make check: OK"

clean:
	rm -rf $(BUILD) build-*

# --- host-side MOD tooling tests -------------------------------------------
# Builds the driver containers with the real toolchain, validates them,
# then runs the host container suite (the packed driver images).

mod-test: $(DRV_SET_MODS)
	$(PYTHON) tools/modpack.py validate $(DRV_SET_MODS)
	$(PYTHON) tools/modpack.py dump $(MODULES)/sio.mod
	$(PYTHON) -m unittest discover -s tests -p 'test_mod.py' -v
	@echo "mod-test: OK"


romfs-test: $(BUILD)/recovery.romfs
	$(PYTHON) tools/romfspack.py --memory-inc $(KOS_PLATFORM_MEMORY_INC) \
		validate $(BUILD)/recovery.romfs
	$(PYTHON) tools/romfspack.py --memory-inc $(KOS_PLATFORM_MEMORY_INC) \
		dump $(BUILD)/recovery.romfs
	$(PYTHON) -m unittest discover -s tests -p 'test_romfs.py' -v
	@echo "romfs-test: OK"

exe-test: $(MODULES)/exetest.exe $(MODULES)/exe-fixtures.stamp
	$(PYTHON) tools/exepack.py validate $(MODULES)/exetest.exe
	$(PYTHON) tools/exepack.py dump $(MODULES)/exetest.exe
	$(PYTHON) -m unittest discover -s tests -p 'test_exe.py' -v
	@echo "exe-test: OK"

monitor-test: $(MODULES)/monitor.exe
	$(PYTHON) tools/exepack.py validate $(MODULES)/monitor.exe
	$(PYTHON) tools/exepack.py dump $(MODULES)/monitor.exe
	$(PYTHON) -m unittest discover -s tests -p 'test_monitor.py' -v
	@echo "monitor-test: OK"

ioc-test: $(MODULES)/ioc.mod
	$(PYTHON) tools/modpack.py validate $(MODULES)/ioc.mod
	$(PYTHON) tools/modpack.py dump $(MODULES)/ioc.mod
	$(PYTHON) -m unittest discover -s tests -p 'test_ioc.py' -v
	@echo "ioc-test: OK"

# --- HID tooling ----------------------------------------------------------

hid-test: $(MODULES)/hid.mod
	$(PYTHON) tools/modpack.py validate $(MODULES)/hid.mod
	$(PYTHON) tools/modpack.py dump $(MODULES)/hid.mod
	$(PYTHON) -m unittest discover -s tests -p 'test_hid.py' -v
	@echo "hid-test: OK"

# --- video tooling --------------------------------------------------------

v9958-test: $(MODULES)/v9958.mod $(MODULES)/v9958-6x8.font
	$(PYTHON) tools/modpack.py validate $(MODULES)/v9958.mod
	$(PYTHON) tools/modpack.py dump $(MODULES)/v9958.mod
	$(PYTHON) tools/fontpack.py verify drivers/v9958/assets/font_cp850_6x8.inc
	$(PYTHON) -m unittest discover -s tests -p 'test_v9958.py' -v
	@echo "v9958-test: OK"

# --- console-serial variant and fallback matrix ----------------------
# console-serial variant: the production image set with
# the other CONSOLE driver.  The first image boots the monitor on the
# serial lane (the runtime-fallback images' replacement outcome); the
# second omits the console image from its ROMFS manifest, so the required
# driver cannot be loaded and the boot must stop explicitly on the early
# console; the third boots a test-only required driver
# whose init refuses, the same stop for the init-status branch.
# tests/test_console_serial.py replays all three.

console-serial-selftest:
	$(MAKE) BUILD=build-console-serial-selftest SYSTEM=console-serial \
		build-console-serial-selftest/konaos-console-serial-selftest.bin \
		build-console-serial-selftest/zephyr80-console-serial-selftest.bin
	$(MAKE) BUILD=build-console-serial-fail-selftest \
		SYSTEM=console-serial-missing \
		build-console-serial-fail-selftest/konaos-console-serial-fail-selftest.bin
	$(MAKE) BUILD=build-driver-init-fail-selftest SYSTEM=console-init-fail \
		build-driver-init-fail-selftest/konaos-driver-init-fail-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-console-serial-selftest/konaos.map \
		build-console-serial-selftest/geometry/memory.inc \
		$(KOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_console_serial.py' -v
	@echo "console serial image: build-console-serial-selftest/konaos-console-serial-selftest.bin"
	@echo "console serial failure image: build-console-serial-fail-selftest/konaos-console-serial-fail-selftest.bin"
	@echo "driver init-failure image: build-driver-init-fail-selftest/konaos-driver-init-fail-selftest.bin"

# Filesystem outcome image: the
# production boot path with the IOC-FAT driver publishing `/fs` and the SDK
# outcome program booting as /system/fs_selftest.exe.  The simulator replay
# serves FS2 from a host peer, so the same image runs on hardware against a
# real card; the checks and their counts are pinned by
# tests/test_fs_selftest_sim.py.
fs-selftest: all
	$(MAKE) BUILD=build-fs-selftest SYSTEM=fs \
		build-fs-selftest/konaos-fs-selftest.bin \
		build-fs-selftest/zephyr80-fs-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-fs-selftest/konaos.map \
		build-fs-selftest/geometry/memory.inc \
		$(KOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_fs_selftest*.py' -v
	$(PYTHON) -m unittest tests.test_events -v
	$(PYTHON) -m unittest tests.test_far_data -v
	$(PYTHON) -m unittest tests.test_io_sim -v
	@echo "filesystem image: build-fs-selftest/konaos-fs-selftest.bin"

# Read-path measurement image (work/57-m13-read-path): the production
# build with the phase instrument compiled in - KOS_IOC_PHASES=1 through
# the kernel, every driver and the bulk loop's assembly switch - plus the
# card program the owner runs (`readphase FILE`, on the card as
# READPH.EXE).  It is a measurement build, not an acceptance image: the
# simulator replay checks the report's shape, the board run produces the
# numbers (the simulator cannot time the card lane honestly).  The shipped
# targets never set KOS_PHASE_FLAGS, so nothing here changes them.
phase-image: all
	$(MAKE) BUILD=build-phase SYSTEM=production \
		KOS_PHASE_FLAGS=-DKOS_IOC_PHASES=1 \
		build-phase/konaos.bin build-phase/zephyr80.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-phase/konaos.map \
		build-phase/geometry/memory.inc \
		$(KOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest tests.test_readpath -v
	@echo "read-path image: build-phase/konaos.bin"
	@echo "card program: build/user/readphase.exe (copy it to the card as READPH.EXE)"

# IPC acceptance image: the production boot path with
# the fixed-core substrate, the six synthetic bank-6 drivers placed by
# the acceptance loader, and the SDK client program booting as
# /system/ipccopy.exe.  The client runs the promoted conformance suite and
# checks its own results; the simulator replay measures the spans, the
# stack high-water marks, the latch intervals and the IRQ delivery.
ipc-selftest: all
	$(MAKE) BUILD=build-ipc-selftest SYSTEM=ipc \
		build-ipc-selftest/konaos-ipc-selftest.bin \
		build-ipc-selftest/zephyr80-ipc-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-ipc-selftest/konaos.map \
		build-ipc-selftest/geometry/memory.inc \
		$(KOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_ipc_selftest*.py' -v
	@echo "ipc image: build-ipc-selftest/konaos-ipc-selftest.bin"

# Extended-memory outcome image: the production boot
# path with the SIO/console driver set and the SDK outcome program
# booting as /system/xmem_selftest.exe.  The client drives the whole
# extended-memory family and the mode-10 aperture and checks its own
# results; it then SYS_EXECs /system/xmem_child.exe with a path living in
# the aperture, and the simulator replay asserts the transcript
# (tests/test_xmem_selftest_sim.py).  The aperture itself is
# hardware-authoritative: the owner runs this image on the board.
xmem-selftest: all
	$(MAKE) BUILD=build-xmem-selftest SYSTEM=xmem \
		build-xmem-selftest/konaos-xmem-selftest.bin \
		build-xmem-selftest/zephyr80-xmem-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-xmem-selftest/konaos.map \
		build-xmem-selftest/geometry/memory.inc \
		$(KOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_xmem_selftest*.py' -v
	@echo "xmem image: build-xmem-selftest/konaos-xmem-selftest.bin"

# Namespace and handle acceptance image:
# the production boot path with the DEVICE/ROMFS providers, the typed
# handle table, bind/call/unbind/ns_enum and the standard handles linked
# on top of a small boot set, and the SDK client program booting as
# /system/ns.exe.  The client checks the namespace outcomes itself and
# prints them; the simulator replay compares the transcript.  Production
# is not touched by this image: it reproduces its own image hash.
ns-selftest: all
	$(MAKE) BUILD=build-ns-selftest SYSTEM=ns \
		build-ns-selftest/konaos-ns-selftest.bin \
		build-ns-selftest/zephyr80-ns-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		--cold-extra vfs_router,vfs_system,object,vfs_file \
		build-ns-selftest/konaos.map \
		build-ns-selftest/geometry/memory.inc \
		$(KOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_ns_selftest*.py' -v
	@echo "namespace image: build-ns-selftest/konaos-ns-selftest.bin"

# Nucleus-layout acceptance image: the revision-13 gates
# with P != 0 on entry (syscall, IRQ), the cold-half handler placement and
# the fixed upcall shim against a stub in bank 6.  SIO-only boot set, so
# the cold half's slack belongs to the test objects.
nucleus-selftest: all
	$(MAKE) BUILD=build-nucleus-selftest SYSTEM=nucleus \
		build-nucleus-selftest/konaos-nucleus-selftest.bin \
		build-nucleus-selftest/zephyr80-nucleus-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-nucleus-selftest/konaos.map \
		build-nucleus-selftest/geometry/memory.inc \
		$(KOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_nucleus_sim.py' -v
	@echo "nucleus image: build-nucleus-selftest/konaos-nucleus-selftest.bin"

# Launcher-failure images: boot-missing names a store file it does not
# carry, boot-corrupt names a malformed EXE container.  Both must report
# the failed line and end at the monitor.

boot-fallback-selftest:
	$(MAKE) BUILD=build-boot-missing-selftest SYSTEM=boot-missing \
		build-boot-missing-selftest/konaos-boot-missing-selftest.bin
	$(MAKE) BUILD=build-boot-corrupt-selftest SYSTEM=boot-corrupt \
		build-boot-corrupt-selftest/konaos-boot-corrupt-selftest.bin
	$(PYTHON) -m unittest discover -s tests -p 'test_boot_policy.py' -v
	@echo "missing-boot image: build-boot-missing-selftest/konaos-boot-missing-selftest.bin"
	@echo "corrupt-boot image: build-boot-corrupt-selftest/konaos-boot-corrupt-selftest.bin"

# SDK helloworld demo image: the production boot path
# in its own build directory (build-helloworld) with the SDK hello demo
# added to the recovery manifest, booted as /system/hello.exe.  This is the
# end-to-end proof that a user program needs no kernel change: the only
# inputs that differ from production are the KOS_BOOT_PROGRAM setting and
# the manifest.  The simulator replay checks the transcript.

helloworld-image: all
	$(MAKE) BUILD=build-helloworld SYSTEM=helloworld \
		build-helloworld/konaos-helloworld.bin \
		build-helloworld/zephyr80-helloworld.bin
	$(PYTHON) -m unittest discover -s tests -p 'test_sdk.py' -v
	@echo "helloworld image: build-helloworld/konaos-helloworld.bin"

# --- physical-hardware serial smoke checks ----------------------------------
# Requires the board connected at 115200 8N1 on SIO0/B and the matching image
# programmed.  Pass SERIAL_PORT=/dev/ttyUSB0 (or export KOS_SERIAL_PORT).
#
# Each smoke mode checks ONE specific image, and the production image and the
# acceptance images have different serial transcripts.  Build the image for
# the mode, flash it, then run the mode:
#
#   mode                          image to flash                              build with
#   smoke-hardware                build/konaos.bin                          make
#   smoke-hardware-hwtest         build/konaos.bin                          make
#   smoke-hardware-subsystem      build-subsys-selftest/konaos-subsys-selftest.bin
#                                                                             make subsystem-selftest
#   smoke-hardware-console-serial      build-console-serial-selftest/konaos-console-serial-selftest.bin  make console-serial-selftest
#   smoke-hardware-fs             build-fs-selftest/konaos-fs-selftest.bin  make fs-selftest
#   smoke-hardware-shell          build-console-serial-selftest/konaos-console-serial-selftest.bin  make console-serial-selftest
#
# `smoke-hardware-hwtest` drives the production image's feature exerciser:
# the tool types `hwtest` at the shell over this lane (the typed bytes reach
# the console through the MCU) and reads the stage lines and the verdict,
# run once with a card and once without.
#
# `smoke-hardware-shell` drives the whole session on the serial production
# image: the prompt, `pwd`, `ls /system`, `hwtest` and `exit` with the
# launcher's relaunch line and the prompt that follows it.
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
		console-serial-selftest \
		fs-selftest xmem-selftest
	@echo "hardware-images: every smoke image is built under build*/"

smoke-hardware:
	@$(call smoke_preflight,build/konaos.bin,all)
	$(PYTHON) tools/serial_smoke.py --mode normal --port $(or $(SERIAL_PORT),$(KOS_SERIAL_PORT))

smoke-hardware-hwtest:
	@$(call smoke_preflight,build/konaos.bin,all)
	$(PYTHON) tools/serial_smoke.py --mode hwtest --port $(or $(SERIAL_PORT),$(KOS_SERIAL_PORT))

smoke-hardware-subsystem:
	@$(call smoke_preflight,build-subsys-selftest/konaos-subsys-selftest.bin,subsystem-selftest)
	$(PYTHON) tools/serial_smoke.py --mode subsystem --port $(or $(SERIAL_PORT),$(KOS_SERIAL_PORT))

# Console mode: the console-serial variant, whose whole dynamic console
# is this serial lane.  The host sync is sent while the banner is
# pending, as for the other console smoke modes.

smoke-hardware-console-serial:
	@$(call smoke_preflight,build-console-serial-selftest/konaos-console-serial-selftest.bin,console-serial-selftest)
	$(PYTHON) tools/serial_smoke.py --mode console-serial --port $(or $(SERIAL_PORT),$(KOS_SERIAL_PORT))

smoke-hardware-fs:
	@$(call smoke_preflight,build-fs-selftest/konaos-fs-selftest.bin,fs-selftest)
	$(PYTHON) tools/serial_smoke.py --mode fs --port $(or $(SERIAL_PORT),$(KOS_SERIAL_PORT))

smoke-hardware-shell:
	@$(call smoke_preflight,build-console-serial-selftest/konaos-console-serial-selftest.bin,console-serial-selftest)
	$(PYTHON) tools/serial_smoke.py --mode shell --port $(or $(SERIAL_PORT),$(KOS_SERIAL_PORT))
