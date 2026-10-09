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
#   make ioc-selftest         IOC transport acceptance image
#   make hid-selftest         HID input acceptance image
#   make video-selftest       V9958 video acceptance image
#   make console-selftest     console binding acceptance image
#   make m9-vfs-selftest      M9 VFS acceptance image
#   make m9-provider-selftest M9 writable-provider acceptance image
#   make m9-fs-selftest       M9 filesystem syscall acceptance image
#   make m9-exec-selftest     M9 SYS_EXEC nesting acceptance image
#   make m9-iocfat-selftest   M9 IOC-FAT provider acceptance image
#   make ipc-selftest        resident-service IPC acceptance image
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

# M9 VFS mount policy (work/M9-REFERENCE.md).  ZOS_MOUNTS selects the one
# manifest per image; acceptance images point it at tests/m9/mounts-*.json.
# ZOS_VFS_BUILTIN names the filesystem providers statically linked into the
# kernel (the registry/vs-generator invariant is checked by test_mountgen.py).
ZOS_MOUNTS      ?= config/mounts.json
ZOS_VFS_BUILTIN ?= romfs

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
             $(BUILD)/arena.rel $(BUILD)/rom_access.rel \
             $(BUILD)/process_launch.rel $(BUILD)/user_copy.rel \
             $(BUILD)/upcall.rel $(BUILD)/diag_io.rel \
             $(BUILD)/ipc_asm.rel $(BUILD)/ipc_common.rel \
             $(BUILD)/service_boot.rel
# M9 VFS core, generated mount table and statically linked ROMFS provider.
M9_VFS_OBJS := $(BUILD)/vfs_mounts.rel $(BUILD)/vfs_path.rel \
               $(BUILD)/vfs_mount.rel $(BUILD)/vfs_file.rel \
               $(BUILD)/vfs_dir.rel $(BUILD)/vfs_mutate.rel \
               $(BUILD)/vfs_process.rel $(BUILD)/vfs_workspace.rel \
               $(BUILD)/romfs_provider.rel
C_OBJS    := $(BUILD)/early_console.rel $(BUILD)/panic.rel $(BUILD)/main.rel \
             $(BUILD)/syscall.rel $(BUILD)/diag.rel $(BUILD)/fs.rel \
             $(BUILD)/irq.rel \
             $(BUILD)/irq_sources.rel $(BUILD)/registry.rel \
             $(BUILD)/module_relocate.rel $(BUILD)/module_loader.rel \
             $(BUILD)/ioc_shim.rel \
             $(BUILD)/romfs.rel $(BUILD)/process.rel $(BUILD)/process_exec.rel \
             $(BUILD)/name.rel \
             $(M9_VFS_OBJS)
# Resident-service IPC substrate (package 39) and the service loader
# (package 40): production links both from package 41 on.  The substrate
# is fixed-required - the endpoint table, the call machinery, the
# descriptor table and the boot gate execute while the window shows bank 6
# - and the loader's callable half (the syscall bodies and the load pass)
# is a cold-half object.  Both lists are pinned by
# config/residency.json and the checked-in linker file.
IPC_SUBSTRATE_OBJS := $(BUILD)/ipc.rel $(BUILD)/ipc_asm.rel \
             $(BUILD)/ipc_common.rel $(BUILD)/service_boot.rel
IPC_COLD_OBJS := $(BUILD)/ipc_syscall.rel $(BUILD)/service_loader.rel
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
ZOS_SERVICES := sio console
ZOS_ARENA_MODULES :=
endif

# Subsystem corrupt-store image: the same current boot path, but the
# packed ROMFS header metadata is deliberately corrupted, so the kernel
# must reject the whole store through the early-console panic before any
# module code runs.
ifeq ($(ZOS_ROMFS_CORRUPT_SELFTEST),1)
CFLAGS    += -DZOS_ROMFS_CORRUPT_SELFTEST=1
PK_SUFFIX := -romfs-corrupt
ZOS_SERVICES := sio console
ZOS_ARENA_MODULES :=
endif

# IOC acceptance image: production-style boot (derived arena, SIO +
# IOC from ROMFS), the target diagnostic reporting through the direct
# early SIO (command lane plus the bulk READY/DONE acceptance over the
# /SHARED service), then the unchanged monitor launcher.  It keeps the
# derived arena so the acceptance image exercises the production geometry
# with the target objects linked in.
# The V9958 boot load stays compiled out: this image's accepted contract
# is the IOC transport with the SIO console, and the console-capable
# V9958 module (package 19) is exercised by video-selftest and console-selftest.
ifeq ($(ZOS_IOC_SELFTEST),1)
CFLAGS    += -DZOS_IOC_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/ioc_test.rel
PK_SUFFIX := -ioc-selftest
ZOS_SERVICES := sio ioc console
ZOS_ARENA_MODULES :=
endif

# HID acceptance image: production-style boot (derived arena, SIO +
# IOC + HID from ROMFS), the HID target diagnostic (HID class/device
# shape, the scripted USB-keyboard typing pass with output through the
# bound SIO console, then the forced SIO input fallback), then the
# unchanged monitor launcher.
# The V9958 boot load stays compiled out so the accepted HID/SIO and
# SIO/SIO console bindings of this package's images are preserved; the
# HID/V9958 and SIO/V9958 bindings are covered by the package 19 images.
ifeq ($(ZOS_HID_SELFTEST),1)
CFLAGS    += -DZOS_HID_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/hid_test.rel
PK_SUFFIX := -hid-selftest
ZOS_SERVICES := sio ioc hid console
ZOS_ARENA_MODULES :=
endif

# video acceptance image: production-style boot (derived
# arena, SIO + IOC + HID + the V9958 video core from ROMFS), the video
# target diagnostic (VIDEO class/device shape, all seven native
# operations, malformed rejection, reset, and the deterministic visible
# pattern), then the unchanged monitor launcher.  The smoke image waits
# for the host sync sequence because this bench's reset also resets the
# USB bridge.
ifeq ($(ZOS_VIDEO_SELFTEST),1)
CFLAGS    += -DZOS_VIDEO_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/video_test.rel
PK_SUFFIX := -video-selftest
ZOS_SERVICES := sio v9958 console
ZOS_ARENA_MODULES :=
endif

# console acceptance image (package 19): the full production stack
# (SIO + IOC + HID + the V9958 video core with its character renderer)
# from ROMFS, the console target diagnostic reporting the selected
# input/output classes over the direct early SIO, and the unchanged M7
# `monitor.zex` running through the normal HID-input/V9958-output
# binding.  This is the image the human keyboard/display pass uses; the
# interactive session is replayed from these exact bytes by
# tests/test_console_sim.py.
ifeq ($(ZOS_CONSOLE_SELFTEST),1)
CFLAGS    += -DZOS_CONSOLE_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/console_test.rel
PK_SUFFIX := -console-selftest
ZOS_SERVICES := sio ioc hid v9958 console
ZOS_ARENA_MODULES :=
endif

# console integration acceptance image (package 20): the full production
# stack (SIO + IOC + HID + the V9958 video/console module) from ROMFS,
# the integration target diagnostic (class/capability discovery of all
# four residents, the single default binding, PING command/status,
# malformed/null/class/capability rejections and their invariants)
# reported over the direct early SIO, then the unchanged M7
# `monitor.zex` launcher for the human HID/V9958 session.  This is the
# image the final physical acceptance gate builds and flashes;
# tests/test_console_acceptance.py replays these exact bytes plus the
# production image.
ifeq ($(ZOS_CONSOLE_INTEGRATION_SELFTEST),1)
CFLAGS    += -DZOS_CONSOLE_INTEGRATION_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/console_integration_test.rel
PK_SUFFIX := -console-integration-selftest
ZOS_SERVICES := sio ioc hid v9958 console
ZOS_ARENA_MODULES :=
endif

# console fallback-matrix images (package 20): the same four-module boot
# contract with one deliberate failure injected through the variant's
# ROMFS manifest (`ZOS_CONSOLE_FALLBACK` = nohid or nohid-nov9958).  The
# boot path is the production one - the loader starts exactly the store's
# service set - so each variant exercises the real load path, the
# per-service evidence rows and the console's own composition policy.
# The shared target diagnostic reports the surviving directions and the
# replay pins the lane each one took (tests/test_console_fallback.py).
ifeq ($(ZOS_CONSOLE_FALLBACK_SELFTEST),1)
CFLAGS    += -DZOS_CONSOLE_FALLBACK_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/console_fallback_test.rel
ZOS_ARENA_MODULES :=
ifeq ($(ZOS_CONSOLE_FALLBACK),nohid)
PK_SUFFIX := -console-fb-nohid
ZOS_SERVICES := sio ioc v9958 console
else ifeq ($(ZOS_CONSOLE_FALLBACK),nohid-nov9958)
PK_SUFFIX := -console-fb-nohid-nov9958
ZOS_SERVICES := sio ioc console
else
$(error ZOS_CONSOLE_FALLBACK_SELFTEST needs ZOS_CONSOLE_FALLBACK=nohid|nohid-nov9958)
endif
endif

# M9 VFS bootstrap acceptance image (package 25): production-style VFS
# boot -- ROMFS mounted at /SYSTEM by vfs_init(), the four resident modules
# and the monitor loaded through VFS paths, the normal mount pass consuming
# a test manifest whose optional mount names an unregistered provider (so it
# fails, is diagnosed, and leaves the namespace usable) -- then the M9 target
# VFS selftest.  The ROMFS carries the four-module set without iocfat.zmod
# (tests/m9/manifest-vfs.json): package 30 added iocfat to the production
# store, but this image's target test already fills the derived arena, so
# the IOC-FAT provider is compiled out here (package 30c correction).
ifeq ($(ZOS_M9_VFS_SELFTEST),1)
CFLAGS    += -DZOS_M9_VFS_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/m9_vfs_test.rel
PK_SUFFIX := -m9-vfs-selftest
ZOS_MOUNTS := tests/m9/mounts-optional-fail.json
ZOS_SERVICES := sio ioc hid v9958 console
ZOS_ARENA_MODULES :=
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
ZOS_SERVICES := sio console
ZOS_ARENA_MODULES :=
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
ZOS_SERVICES := sio console
ZOS_ARENA_MODULES :=
endif

# M9 SYS_EXEC acceptance image (package 28): the SIO-only boot set plus the
# test-only ramfs provider at `/` and `/R2`, and the extended native
# programs fstest.zex, fschild.zex, fsraw.zex and fsdeep.zex.  The target
# hook runs the user-side exec cases and the depth probe.
ifeq ($(ZOS_M9_EXEC_SELFTEST),1)
CFLAGS    += -DZOS_M9_EXEC_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/m9_exec_test.rel
PK_SUFFIX := -m9-exec-selftest
ZOS_MOUNTS := tests/m9/mounts-ramfs.json
ZOS_SERVICES := sio console
ZOS_ARENA_MODULES :=
endif

# M9 IOC-FAT acceptance image (package 30).  The production boot set (SIO,
# IOC, HID, V9958) plus `iocfat.zmod` loaded last, with the production
# `config/mounts.json` attaching `/` to `iocfat` as an optional mount.  The
# target hook drives the provider through the ordinary VFS while the
# simulator's FS2 peer serves the MCU side; the same image runs on hardware
# against a real SD card.  HID and V9958 are dropped so the derived arena
# holds the acceptance object alongside the provider and the console still
# binds the SIO fallback.
ifeq ($(ZOS_M9_IOCFAT_SELFTEST),1)
CFLAGS    += -DZOS_M9_IOCFAT_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/m9_iocfat_test.rel
PK_SUFFIX := -m9-iocfat-selftest
ZOS_SERVICES := sio ioc console
endif

# M9 integration acceptance image (package 31).  The production boot set
# minus HID/V9958 (so the whole acceptance transcript arrives on SIO, as for
# every other M9 acceptance image), the resident IOC-FAT provider, and every
# native acceptance program.  The target hook exec's fstest.zex -- which
# exec's the deep-matrix program fsfat.zex -- then the fsdeep.zex depth
# probe, and checks the process, bank and descriptor pools are back to
# baseline.  With a card, `/` is the real FAT volume, so this is the image
# that proves M9 end to end.
ifeq ($(ZOS_M9_SELFTEST),1)
CFLAGS    += -DZOS_M9_SELFTEST=1 -DZOS_SERVICE_TEST_EVIDENCE=0
TARGET_TEST_OBJS += $(BUILD)/m9_selftest.rel \
		    $(BUILD)/m9_selftest_runner.rel
# This image needs all 13 IOC-FAT arena granules.  Its compact target hook
# fits the fixed code gap; only its bounded data belongs in the cold half.
TEST_FIXED_OBJS := $(BUILD)/target_test.rel $(BUILD)/m9_selftest.rel \
		   $(BUILD)/m9_selftest_runner.rel
PK_SUFFIX := -m9-selftest
ZOS_SERVICES := sio ioc console
endif

# Nucleus-layout acceptance image (work package 38): the production boot
# path with the revision-13 gate and upcall-shim acceptance surface linked
# in.  The smoke caller runs in application bank 0 with the service page
# selected, and a cold-half caller exercises the fixed upcall shim against
# a stub installed in bank 6.  SIO-only boot set, like the subsystem image:
# the test objects own the derived arena.
ifeq ($(ZOS_NUCLEUS_SELFTEST),1)
CFLAGS    += -DZOS_NUCLEUS_SELFTEST=1
TARGET_TEST_OBJS += $(BUILD)/nucleus_test.rel \
                    $(BUILD)/nucleus_selftest_asm.rel \
                    $(BUILD)/ctc_test.rel
PK_SUFFIX := -nucleus-selftest
ZOS_SERVICES := sio console
ZOS_ARENA_MODULES :=
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
# The IPC image uses production's placement.  Its old five-object cold
# override overflowed the cold half after the arena shrink moved the
# derived data base; the remaining `moves` objects fit the fixed core's
# documented leftover space.
IPC_COLD_EXTRA :=
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
ZOS_SERVICES := sio console
ZOS_ARENA_MODULES :=
endif

# Namespace and handle acceptance image (work package 42a, correction 01).
# Production bank 7 cannot hold the object/namespace family while the M9
# `FS_*` front end, the M9 VFS machinery and the IOC-FAT arena are linked
# (the switch-over and those deletions are 42b), so this image builds the
# family on top of a small boot set with no arena modules: the placement
# config/residency.json names is the placement production will use, and the
# image's ledgers are the projection of production's after 42b.
#   * SIO and the console service: the DEVICE provider's real publications
#     and the standard handles need a live `/dev/console`;
#   * no arena modules: the derived arena belongs to the family, and the
#     M9 file path is not exercised here (42b replaces it);
#   * the client program is an ordinary SDK program that checks its own
#     results and prints them (`tests/target/ns`).
NS_DIR   := tests/target/ns
ifeq ($(ZOS_NS_SELFTEST),1)
CFLAGS    += -DZOS_NS_HANDLES=1 -Itests/target/include
PK_SUFFIX := -ns-selftest
# sio and console are the real services whose publications the DEVICE
# table carries (`/dev/serial/0`, `/dev/console`); `nsdev` is the test
# double that adds a third type and a second grouping container
# (tests/target/ns/nsdev.c).
ZOS_SERVICES := sio console nsdev
SVC_MANIFEST_DIR := tests/ns/manifests
ZOS_ARENA_MODULES :=
ZOS_BOOT_PROGRAM := /SYSTEM/ns.zex
# The object family links where production will link it (config/
# residency.json: vfs_ns, vfs_ns_file and object are `cold`), which this
# image reaches by naming them in its extra cold set; the shared runner and
# the guard the other acceptance images use come with it.
ZOS_VFS_WS_GUARD := 1
NS_OBJS := $(BUILD)/vfs_ns.rel $(BUILD)/vfs_ns_file.rel $(BUILD)/object.rel
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
# loads the five services from the recovery store's `services.man` in the
# order the manifest lists; an acceptance image that needs the serial-only
# fallback lists a subset (see the per-image blocks below), which is the
# M8 `ZOS_*_BOOT` gate's replacement.  The arena plan lists the modules the
# loader still places in the derived arena (IOC-FAT in production).
ZOS_SERVICES ?= sio ioc hid v9958 console
ZOS_ARENA_MODULES ?= iocfat

# The IOC-FAT arena provider's boot gate follows the arena set: an image
# whose declared boot set does not carry iocfat does not attempt the load
# (the M8 per-image boot gates' M10 replacement - the store says what the
# image boots, and the image's own code follows it).
ifneq ($(filter iocfat,$(ZOS_ARENA_MODULES)),)
CFLAGS    += -DZOS_IOCFAT_BOOT=1
else
CFLAGS    += -DZOS_IOCFAT_BOOT=0
endif

OBJS      := $(ASM_OBJS) $(C_OBJS) $(BUILD)/ipc.rel $(IPC_COLD_OBJS) \
             $(NS_OBJS)

# Nucleus residency (architecture 36.5, work package 38).  Every
# production image is a revision-13 two-region build: the fixed areas
# (_CODE and friends, _DATA/_INITIALIZED) must link inside 2000h-5FFFh,
# and the objects in config/residency.json's `cold_half` list compile into
# the cold areas (_WINDOW / _WINDOW_DATA at 6000h-DFFFh) that the window
# shows with P = 0.  The classification, its tiers and the reasons live in
# config/residency.json; tools/mapcheck.py fails the build when an object
# is missing or when a fixed-required object links at or above 6000h.
#
# Acceptance images compile their target-test objects cold too, so test
# code consumes the driver arena's slack, never the fixed core.  An image
# whose boot set leaves the cold half nearly empty may also place its
# stage-4/5 `moves` objects there instead of overflowing them into the
# fixed core (the placement freedom of architecture 36.20); it passes
# that extra cold set to mapcheck with --cold-extra and to the codegen
# through COLD_EXTRA, so both stay in step.
RESIDENCY  := config/residency.json
# Package 41: residency.json now names the two `moves` objects production
# compiles cold (vfs_dir, romfs_provider - they fit beside the new cold
# bodies: the resident service loader, the syscall IPC dispatcher and the
# IOC class shim).  The rest keep the moves tier's documented fallback and
# overflow into the fixed core's leftover space, which the substrate now
# shares; both ledgers report the split.  An image that wants a different
# split passes it here (the IPC acceptance image does).
COLD_EXTRA ?=
COLD_EXTRA_CSV ?= $(COLD_EXTRA)
ifeq ($(COLD_EXTRA_CSV),)
COLD_STEMS := $(shell $(PYTHON) tools/mapcheck.py residency \
		--cold-stems $(RESIDENCY))
else
COLD_STEMS := $(shell $(PYTHON) tools/mapcheck.py residency \
		--cold-stems $(RESIDENCY) --cold-extra $(COLD_EXTRA_CSV))
endif
# Driver-arena geometry ---------------------------------------------------
# Every image derives the arena and the runtime data base from the aligned
# linked-kernel end with a two-pass link.  There is one geometry; the probe
# link places no arena.


# Common acceptance wiring: the single guard plus the shared runner.
ifneq ($(filter 1,$(ZOS_SUBSYS_SELFTEST) \
	$(ZOS_IOC_SELFTEST) $(ZOS_HID_SELFTEST) \
	$(ZOS_VIDEO_SELFTEST) \
	$(ZOS_CONSOLE_SELFTEST) $(ZOS_CONSOLE_INTEGRATION_SELFTEST) \
	$(ZOS_CONSOLE_FALLBACK_SELFTEST) $(ZOS_M9_VFS_SELFTEST) \
	$(ZOS_M9_PROVIDER_SELFTEST) $(ZOS_M9_FS_SELFTEST) \
	$(ZOS_M9_EXEC_SELFTEST) $(ZOS_M9_IOCFAT_SELFTEST) \
	$(ZOS_M9_SELFTEST) $(ZOS_NUCLEUS_SELFTEST) \
	$(ZOS_IPC_SELFTEST)),)
CFLAGS    += -DZOS_TARGET_TEST_BUILD=1 -DZOS_VFS_WS_GUARD=1 \
		-Itests/target/include
TARGET_TEST_OBJS := $(BUILD)/target_test.rel \
		$(if $(filter 1,$(ZOS_M9_SELFTEST)),,$(BUILD)/service_client.rel) \
		$(TARGET_TEST_OBJS)
endif

# The cold placement follows the target-test list: the acceptance objects
# (and the shared service client every M10 driver diagnostic links) are
# compiled into the window areas at 6000h-DFFFh, exactly like the
# production cold half, so test code consumes the arena's slack and never
# the fixed core's leftover.  This block sits after the list it filters.
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

# The arena plan's boot set: the modules the loader still places in the
# derived driver arena.  The services are not part of it - they own bank
# 6's resident tiers and are placed by the service loader from
# `services.man` - so production's arena set is the IOC-FAT provider
# alone, and every other image declares its own fixture modules.
# (MODULES is defined below this point, so keep these recursive
# assignments.)
BOOT_ZMODS = $(addprefix $(MODULES)/,$(addsuffix .zmod,$(ZOS_ARENA_MODULES)))


IMAGE := $(BUILD)/zephyros$(PK_SUFFIX).bin
MAME_IMAGE := $(BUILD)/zephyr80$(PK_SUFFIX).bin

# Memory-layout source for this build: the platform memory.inc is the one
# declarative source (the two-region split and the stack reservations are
# production constants now), and the derived variant below adds the
# per-image geometry that every assembly unit and layout tool reads.
GEOMETRY_DIR       := $(BUILD)/geometry
GEOMETRY_PROBE_DIR := $(BUILD)/probe
GEOMETRY_INC       := $(GEOMETRY_DIR)/arena_geometry.inc
ASM_MEMORY_INC     := $(ZOS_PLATFORM_MEMORY_INC)

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
		tools/mapcheck.py $(ASM_MEMORY_INC) $(RESIDENCY) | $(BUILD)
	mkdir -p $(GEOMETRY_DIR)
	$(PYTHON) tools/mapcheck.py geometry derive $(BUILD)/zephyros.probe.map \
		$(ASM_MEMORY_INC) $(GEOMETRY_INC) $(GEOMETRY_MEMORY) \
		$(RESIDENCY_DERIVE_FLAGS)

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
	subsystem-selftest romfs-corrupt-selftest \
	monitor-test ioc-test ioc-selftest hid-test \
	hid-selftest \
	v9958-test video-selftest \
	console-selftest \
	console-integration-selftest console-fallback-selftests \
	m9-vfs-selftest \
	m9-required-mount-selftest \
	m9-provider-selftest \
	m9-fs-selftest \
	m9-exec-selftest \
	m9-iocfat-selftest \
	m9-selftest \
	ipc-selftest \
	nucleus-selftest \
	hardware-images \
	smoke-hardware \
	smoke-hardware-subsystem smoke-hardware-romfs-corrupt \
	smoke-hardware-ioc smoke-hardware-hid \
	smoke-hardware-video smoke-hardware-console \
	smoke-hardware-console-integration smoke-hardware-console-fb-nohid \
	smoke-hardware-console-fb-nohid-nov9958 \
	smoke-hardware-m9-fs \
	smoke-hardware-m9-exec \
	smoke-hardware-m9-iocfat \
	smoke-hardware-m9

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

$(BUILD)/arena.rel: kernel/module/arena.asm $(GEOMETRY_INC) | $(BUILD)
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
		include/zephyros/module.h include/zephyros/process.h | $(BUILD)
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

# --- M3 device/driver/service objects (bank-7 code and data) -----------------

$(BUILD)/registry.rel: kernel/device/registry.c include/zephyros/device.h \
		kernel/device/registry.h kernel/lib/name.h \
		include/zephyros/irq.h include/zephyros/errno.h \
		abi/zos/types.h | $(BUILD)
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

$(BUILD)/ioc_shim.rel: kernel/module/ioc_shim.c kernel/module/ioc_shim.h \
		include/zephyros/ioc.h include/zephyros/device.h \
		include/zephyros/ipc.h include/zephyros/process.h \
		abi/zos/protocol.h kernel/ipc/ipc.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# --- M5 ROMFS objects (bank-7 reader + common ROM-access primitive) ----------

# Shared kernel ASCII name comparison (package 30c, F3): one helper for VFS,
# ROMFS and the device registry.
$(BUILD)/name.rel: kernel/lib/name.c kernel/lib/name.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/romfs.rel: kernel/romfs/romfs.c include/zephyros/romfs.h \
		include/zephyros/module.h include/zephyros/platform.h \
		kernel/lib/name.h platform/zephyr80/memory.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# --- M9 VFS objects (bank-7 core + ROMFS provider + generated mount table) ---

$(BUILD)/vfs_path.rel: kernel/vfs/path.c kernel/vfs/vfs.h \
		abi/zos/fs.h include/zephyros/vfs.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/vfs_mount.rel: kernel/vfs/mount.c kernel/vfs/vfs.h \
		kernel/vfs/mount_config.h kernel/lib/name.h abi/zos/fs.h \
		include/zephyros/vfs.h fs/romfs/romfs_provider.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/vfs_file.rel: kernel/vfs/file.c kernel/vfs/vfs.h \
		abi/zos/fs.h include/zephyros/vfs.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/vfs_dir.rel: kernel/vfs/dir.c kernel/vfs/vfs.h kernel/lib/name.h \
		abi/zos/fs.h include/zephyros/vfs.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/vfs_mutate.rel: kernel/vfs/mutate.c kernel/vfs/vfs.h \
		abi/zos/fs.h include/zephyros/vfs.h \
		include/zephyros/process.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/vfs_process.rel: kernel/vfs/process.c kernel/vfs/vfs.h \
		abi/zos/fs.h include/zephyros/vfs.h \
		include/zephyros/process.h include/zephyros/errno.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/vfs_workspace.rel: kernel/vfs/workspace.c kernel/vfs/vfs.h \
		abi/zos/fs.h include/zephyros/vfs.h \
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
		kernel/syscall/fs.h kernel/syscall/diag.h kernel/vfs/ns.h \
		abi/zos/object.h abi/zos/ipc.h abi/zos/syscall.h \
		include/zephyros/errno.h include/zephyros/process.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/fs.rel: kernel/syscall/fs.c kernel/syscall/fs.h \
		kernel/syscall/diag.h kernel/vfs/vfs.h \
		abi/zos/syscall.h abi/zos/fs.h \
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

# Declared boot-set arena plan: per-module image+BSS/granules and the
# used/free totals; the build fails here when the declared boot set cannot
# coexist in the derived arena.  The plan also carries the bank-7 ledger.
ARENA_PLAN := $(BUILD)/arena.plan.json
$(ARENA_PLAN): $(BOOT_ZMODS) tools/zmodpack.py tools/pack_image.py \
		$(MEMORY_INC) | $(BUILD)
	$(PYTHON) tools/zmodpack.py plan --geometry $(MEMORY_INC) \
		--report $@ $(BOOT_ZMODS)

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
# image declares the dependency), and the console last, because its init
# probes the services it composes.
ZOS_SERVICES ?= sio ioc hid v9958 console
SVC_NAMES := sio ioc hid v9958 console
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
		include/zephyros/device.h include/zephyros/platform.h | $(MODULES)
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
		include/zephyros/ioc_protocol.h include/zephyros/hid.h \
		include/zephyros/ioc.h include/zephyros/device.h \
		include/zephyros/ipc.h $(SVC_PROTOCOL_H) \
		include/zephyros/platform.h | $(MODULES)
	$(SDCC) $(SVC_HOT) $< -o $@

$(MODULES)/v9958.rel: drivers/v9958/v9958.c drivers/v9958/v9958_driver.h \
		include/zephyros/video.h include/zephyros/device.h \
		include/zephyros/ipc.h $(SVC_PROTOCOL_H) \
		include/zephyros/platform.h | $(MODULES)
	$(SDCC) $(SVC_HOT) $< -o $@

$(MODULES)/v9958_io.rel: drivers/v9958/v9958_io.asm \
		platform/zephyr80/hardware.inc | $(MODULES)
	$(SDASZ80) $(ASFLAGS) -o $@ $<

$(MODULES)/v9958_service.rel: drivers/v9958/v9958_service.c \
		drivers/v9958/v9958_driver.h include/zephyros/video.h \
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

$(MODULES)/console_service.rel: services/console/console_service.c \
		include/zephyros/ipc.h \
		$(SVC_PROTOCOL_H) include/zephyros/errno.h | $(MODULES)
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
	$(MODULES)/console_service.rel

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

$(MODULES)/abi_probe.rel: tests/modules/abi_probe.c \
		include/zephyros/device.h include/zephyros/driver.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

$(MODULES)/abi_probe.zmod: tests/modules/abi_probe.json $(MODULES)/abi_probe.rel \
		$(MODULES)/sdcc_indirect_calls.rel tools/zmodpack.py
	$(PYTHON) tools/zmodpack.py pack $< $@ --objdir $(MODULES) \
		--report $(MODULES)/abi_probe.report.json

# --- M9 writable-provider fixtures (test only) ---

$(MODULES)/ramfs.rel: tests/modules/ramfs.c \
		include/zephyros/driver.h include/zephyros/vfs.h \
		abi/zos/fs.h include/zephyros/errno.h | $(MODULES)
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

# --- M9 IOC-FAT provider (package 30) ---
# The resident path-based FS2 provider, loaded last by production (see the
# ZOS_IOCFAT_BOOT gate) and mounted at the optional `/`.
$(MODULES)/iocfat.rel: fs/iocfat/iocfat.c \
		include/zephyros/driver.h include/zephyros/vfs.h \
		include/zephyros/ioc.h abi/zos/fs.h include/zephyros/errno.h | $(MODULES)
	$(SDCC) $(MOD_CFLAGS) $< -o $@

$(MODULES)/iocfat.zmod: modules/manifests/iocfat.json $(MODULES)/iocfat.rel \
		$(MODULES)/sdcc_indirect_calls.rel tools/zmodpack.py
	$(PYTHON) tools/zmodpack.py pack $< $@ --objdir $(MODULES) \
		--report $(MODULES)/iocfat.report.json

$(MODULES)/%.zmod: tests/modules/%.json $(MODULES)/%.rel \
		$(MODULES)/sdcc_indirect_calls.rel tools/zmodpack.py
	$(PYTHON) tools/zmodpack.py pack $< $@ --objdir $(MODULES)

FIXTURE_ZMODS := $(MODULES)/m4_probe.zmod $(MODULES)/m4_fail.zmod \
		$(MODULES)/m4_filler.zmod $(MODULES)/abi_probe.zmod

# --- target acceptance objects (tests/target/, acceptance builds only) -------

$(BUILD)/target_test.rel: tests/target/support/target_test.c \
		include/zephyros/kernel.h tests/target/include/syscall_selftest.h \
		tests/target/include/irq_selftest.h \
		tests/target/include/loader_selftest.h \
		tests/target/include/romfs_selftest.h \
		tests/target/include/process_selftest.h \
		tests/target/include/hid_selftest.h \
		tests/target/include/video_selftest.h \
		tests/target/include/console_selftest.h \
		tests/target/include/console_integration_selftest.h \
		tests/target/include/console_fallback_selftest.h \
		tests/target/include/m9_selftest.h \
		tests/target/include/m9_provider_selftest.h \
		tests/target/include/m9_fs_selftest.h \
		tests/target/include/m9_exec_selftest.h \
		tests/target/include/m9_iocfat_selftest.h \
		abi/zos/ipc.h \
		include/zephyros/service.h \
		kernel/syscall/ipc.h | $(BUILD)
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

$(BUILD)/ipc_syscall.rel: kernel/syscall/ipc.c kernel/syscall/ipc.h \
		kernel/ipc/ipc.h abi/zos/ipc.h kernel/syscall/diag.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/service_loader.rel: kernel/service/service_loader.c \
		include/zephyros/service.h include/zephyros/ipc.h \
		include/zephyros/module.h include/zephyros/romfs.h \
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
		include/zephyros/romfs.h include/zephyros/module.h \
		include/zephyros/kernel.h \
		tests/target/include/romfs_selftest.h $(BUILD)/romfs_expect.inc | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/loader_test.rel: tests/target/loader/loader_test.c \
		include/zephyros/module.h include/zephyros/kernel.h \
		include/zephyros/device.h include/zephyros/irq.h \
		include/zephyros/vfs.h \
		tests/target/include/loader_selftest.h | $(BUILD)
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
		include/zephyros/module.h \
		include/zephyros/kernel.h \
		tests/target/include/process_selftest.h $(BUILD)/process_expect.inc | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

# The IOC transport diagnostic is the one explicitly test-only rule allowed
# a private IOC include path: it exercises the READY/DONE admission state,
# the bulk-arming class gate, and the unknown-class reply, which are
# transport-private by design.
$(BUILD)/ioc_test.rel: tests/target/ioc/ioc_test.c \
		include/zephyros/device.h include/zephyros/ioc.h \
		include/zephyros/kernel.h drivers/ioc/ioc_proto.h \
		include/zephyros/ioc_protocol.h \
		tests/target/include/ioc_selftest.h \
		tests/target/include/service_client.h | $(BUILD)
	$(SDCC) $(CFLAGS) -Idrivers/ioc $< -o $@

$(BUILD)/hid_test.rel: tests/target/hid/hid_test.c \
		include/zephyros/device.h \
		include/zephyros/hid.h include/zephyros/kernel.h \
		tests/target/include/hid_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/video_test.rel: tests/target/video/v9958_test.c \
		include/zephyros/video.h include/zephyros/device.h \
		include/zephyros/kernel.h \
		tests/target/include/video_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/console_test.rel: tests/target/console/console_test.c \
		include/zephyros/kernel.h abi/zos/protocol.h \
		tests/target/include/service_client.h \
		tests/target/include/console_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/console_integration_test.rel: tests/target/console/integration_test.c \
		include/zephyros/kernel.h include/zephyros/ioc.h \
		abi/zos/protocol.h \
		tests/target/include/service_client.h \
		tests/target/include/console_integration_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/console_fallback_test.rel: tests/target/console/fallback_test.c \
		include/zephyros/kernel.h abi/zos/protocol.h \
		tests/target/include/service_client.h \
		tests/target/include/console_fallback_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m9_vfs_test.rel: tests/target/m9/m9_vfs_test.c \
		abi/zos/fs.h include/zephyros/vfs.h \
		include/zephyros/romfs.h include/zephyros/module.h \
		include/zephyros/kernel.h \
		kernel/vfs/vfs.h kernel/vfs/mount_config.h \
		tests/target/include/m9_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m9_provider_test.rel: tests/target/m9/m9_provider_test.c \
		abi/zos/fs.h include/zephyros/vfs.h \
		include/zephyros/module.h \
		include/zephyros/kernel.h \
		kernel/vfs/vfs.h kernel/vfs/mount_config.h \
		tests/target/include/m9_provider_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m9_fs_test.rel: tests/target/m9/m9_fs_test.c \
		include/zephyros/process.h \
		include/zephyros/kernel.h abi/zos/fs.h \
		kernel/vfs/vfs.h \
		tests/target/include/m9_fs_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m9_exec_test.rel: tests/target/m9/m9_exec_test.c \
		include/zephyros/process.h \
		include/zephyros/kernel.h abi/zos/fs.h \
		kernel/vfs/vfs.h \
		tests/target/include/m9_exec_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m9_iocfat_test.rel: tests/target/m9/m9_iocfat_test.c \
		include/zephyros/device.h \
		include/zephyros/kernel.h abi/zos/fs.h \
		kernel/vfs/vfs.h \
		tests/target/include/m9_iocfat_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m9_selftest.rel: tests/target/m9/m9_selftest.c \
		tests/target/m9/m9_selftest_internal.h \
		include/zephyros/kernel.h \
		include/zephyros/process.h abi/zos/fs.h \
		kernel/lib/name.h kernel/vfs/vfs.h \
		tests/target/include/m9_integration_selftest.h | $(BUILD)
	$(SDCC) $(CFLAGS) $< -o $@

$(BUILD)/m9_selftest_runner.rel: tests/target/m9/m9_selftest_runner.c \
		tests/target/m9/m9_selftest_internal.h \
		include/zephyros/kernel.h include/zephyros/process.h \
		abi/zos/fs.h kernel/vfs/vfs.h \
		tests/target/include/m9_integration_selftest.h | $(BUILD)
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

$(eval $(call zex_program,fstest,tests/target/m9/fstest,fstest.c,\
	STACK=320,STAGE=1))
$(eval $(call zex_program,fsfat,tests/target/m9/fstest,fsfat.c,\
	STACK=256,STAGE=1))
$(eval $(call zex_program,fstest_l,tests/target/m9/fstest,fstest_l.c,\
	STACK=256,STAGE=1))
$(eval $(call zex_program,fschild,tests/target/m9/exec,fschild.c,\
	STACK=256,STAGE=1))
$(eval $(call zex_program,fsdeep,tests/target/m9/exec,fsdeep.c,\
	STACK=128,STAGE=1))
$(eval $(call zex_program,fsraw,tests/target/m9/exec,fsraw.c fsraw_entry.asm,\
	NO_CRT0=1,ENTRY=rw_entry,STACK=32,SCRATCH=,STAGE=1))

# IPC acceptance client (work package 39): an ordinary SDK program.  It
# consumes the native SYS_IPC_LOOKUP / SYS_IPC_CALL wrappers from libzos
# and the test protocol header; it owns no syscall stub of its own.
$(eval $(call zex_program,ipc,tests/target/ipc,\
	ipc.c,STACK=256,STAGE=1,\
	INCLUDES=tests/target/include))

# IPC caller-buffer probe (work package 39, correction 01): the boot
# program of the IPC acceptance image.  NO_CRT0 with a custom entry and
# LOWBASE=0x0300 and CODEBASE=0x0344 put the SDK's 68-byte low-data area
# immediately above 0100h-02FFh, which is left as
# a plain 512-byte buffer: the only way a ZEX image can name the caller
# window itself as a copy destination.
$(eval $(call zex_program,ipccopy,tests/target/ipc,\
	ipccopy.c ipccopy_entry.asm,NO_CRT0=1,ENTRY=ipc_copy_entry,\
	LOWBASE=0x0300,CODEBASE=0x0344,STAGE=1,\
	INCLUDES=tests/target/include))

# Namespace acceptance programs (work package 42a): the client the image
# boots (ZOS_BOOT_PROGRAM=/SYSTEM/ns.zex) and the child it SYS_EXECs.  Both
# are ordinary SDK programs; the image's SDK variant carries the object
# wrappers and moves the default code base above the enlarged gate blocks
# (user/sdk/zex.mk ZEX_SDK_LOW_SIZE).
$(eval $(call zex_program,ns,tests/ns,\
	ns.c,STACK=256,STAGE=1,\
	INCLUDES=tests/target/include))

$(eval $(call zex_program,nschild,tests/ns,\
	nschild.c,STACK=192,STAGE=1,\
	INCLUDES=tests/target/include))

# Deterministic malformed containers (one per validation boundary; see
# tests/subsys/gen_zex_fixtures.py and gen_zmod_fixtures.py) shared by the
# subsystem acceptance manifest.  gen_zmod_fixtures.py derives its damaged
# ZMODs from the reviewed loader fixture `m4_probe.zmod`, so that fixture
# is a real prerequisite: `make clean && make zex-test` must be able to
# build the stamp on its own.
$(MODULES)/zex-fixtures.stamp: tests/subsys/gen_zex_fixtures.py \
		tests/subsys/gen_zmod_fixtures.py $(MODULES)/m4_probe.zmod \
		tools/zexpack.py | $(MODULES)
	$(PYTHON) tests/subsys/gen_zex_fixtures.py $(MODULES)
	$(PYTHON) tests/subsys/gen_zmod_fixtures.py $(MODULES)
	touch $@

# --- ROMFS recovery store (ROM pages 2-7) ------------------------------------
# Every image packs a manifest; the subsystem acceptance image adds the
# loader/ROMFS/ZEX fixtures, and the corrupt-store image patches the packed
# production image so the kernel's validation is exercised on target.
ifeq ($(ZOS_IOC_SELFTEST),1)
# IOC acceptance store: the SIO fallback and the IOC transport services the
# image boots, plus the monitor launcher that follows the diagnostic.
ROMFS_MANIFEST := tests/ioc/manifest.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex
else ifeq ($(ZOS_HID_SELFTEST),1)
# HID acceptance store: SIO, the IOC transport the HID engine depends on,
# HID itself and the monitor.
ROMFS_MANIFEST := tests/hid/manifest.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex
else ifeq ($(ZOS_VIDEO_SELFTEST),1)
# Video acceptance store: SIO, the V9958 video service (its font travels in
# the image's bulk segment) and the console that composes them.
ROMFS_MANIFEST := tests/video/manifest.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex
else ifeq ($(ZOS_CONSOLE_SELFTEST),1)
# Console acceptance store: the full production service set (the IOC-FAT
# arena module is compiled out of this image, so its entry is absent) plus
# the monitor launcher.
ROMFS_MANIFEST := tests/console/manifest-integration.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex
else ifeq ($(ZOS_CONSOLE_INTEGRATION_SELFTEST),1)
# Console integration acceptance store: the same full service set plus the
# monitor launcher.
ROMFS_MANIFEST := tests/console/manifest-integration.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex
else ifeq ($(ZOS_SUBSYS_SELFTEST),1)
ROMFS_MANIFEST := tests/subsys/manifest.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex $(MODULES)/romfs_crosspage.bin \
		$(MODULES)/m4_probe.zmod $(MODULES)/m4_fail.zmod \
		$(MODULES)/m4_filler.zmod $(MODULES)/abi_probe.zmod \
		$(MODULES)/zextest.zex $(MODULES)/zex-fixtures.stamp
else ifeq ($(ZOS_ROMFS_CORRUPT_SELFTEST),1)
ROMFS_MANIFEST := recovery/manifest.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex tests/subsys/corrupt_romfs.py
else ifeq ($(ZOS_CONSOLE_FALLBACK),nohid)
# Failure variant: the required set minus hid.zmod, so the kernel's HID
# load takes the missing-ROMFS-entry path (-ZOS_ENOENT) and the console
# falls back to SIO input with the V9958 output.
ROMFS_MANIFEST := tests/console/manifest-fallback-nohid.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex
else ifeq ($(ZOS_CONSOLE_FALLBACK),nohid-nov9958)
# Failure variant: HID and V9958 both absent; only the SIO fallback and
# the IOC transport survive, and the console binds SIO/SIO.
ROMFS_MANIFEST := tests/console/manifest-fallback-nohid-nov9958.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex
else ifeq ($(ZOS_M9_VFS_SELFTEST),1)
# Production-style VFS boot without IOC-FAT: the four resident modules, the
# V9958 font and the monitor.  The VFS target test already fills the
# derived arena, so the IOC-FAT provider is compiled out and its ROMFS
# entry is omitted (package 30c correction).
ROMFS_MANIFEST := tests/m9/manifest-vfs.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex
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
		$(MODULES)/fstest.zex $(MODULES)/fstest_l.zex \
		$(MODULES)/fschild.zex $(MODULES)/fsraw.zex
else ifeq ($(ZOS_M9_EXEC_SELFTEST),1)
# SIO-only boot: the SIO fallback, the RAM provider at `/` and `/R2`, the
# monitor and the native exec acceptance programs (the depth probe is only
# used by this image).
ROMFS_MANIFEST := tests/m9/manifest-exec.json
ROMFS_EXTRA_DEPS := $(MODULES)/ramfs.zmod $(MODULES)/monitor.zex \
		$(MODULES)/fstest.zex $(MODULES)/fschild.zex \
		$(MODULES)/fsraw.zex $(MODULES)/fsdeep.zex
else ifeq ($(ZOS_M9_SELFTEST),1)
# Integration image: the IOC transport, the resident FS2 provider and the
# whole native acceptance set, all loaded from the recovery store.  `/` is
# the card, so nothing else may be assumed present.
ROMFS_MANIFEST := tests/m9/manifest-selftest.json
ROMFS_EXTRA_DEPS := $(MODULES)/monitor.zex \
		$(MODULES)/fstest.zex $(MODULES)/fsfat.zex \
		$(MODULES)/fsdeep.zex
else ifeq ($(ZOS_M9_IOCFAT_SELFTEST),1)
# Production boot set minus HID/V9958: the SIO fallback, the IOC transport,
# the resident FS2 filesystem provider and the console, plus the native
# filesystem acceptance program the target hook exec's.  The image's `/` is
# the card, so nothing else may be assumed present.
ROMFS_MANIFEST := tests/m9/manifest-iocfat.json
ROMFS_EXTRA_DEPS := $(MODULES)/iocfat.zmod $(MODULES)/monitor.zex \
		$(MODULES)/fstest.zex
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
		$(MODULES)/services.man $(BOOT_ZMODS) $(ROMFS_EXTRA_DEPS) \
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
		$(ZOS_NUCLEUS_SELFTEST) \
		$(ZOS_ROMFS_CORRUPT_SELFTEST) \
		$(ZOS_IOC_SELFTEST) $(ZOS_HID_SELFTEST) \
		$(ZOS_VIDEO_SELFTEST) \
		$(ZOS_CONSOLE_SELFTEST) $(ZOS_CONSOLE_INTEGRATION_SELFTEST) \
		$(ZOS_CONSOLE_FALLBACK_SELFTEST) $(ZOS_M9_VFS_SELFTEST) \
		$(ZOS_M9_PROVIDER_SELFTEST) $(ZOS_M9_FS_SELFTEST) \
		$(ZOS_M9_EXEC_SELFTEST) $(ZOS_M9_IOCFAT_SELFTEST))
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
		$(BUILD)/recovery.romfs $(ARENA_PLAN) \
		tools/pack_image.py tools/mapcheck.py $(MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) tools/mapcheck.py $(MAPCHECK_FLAGS) $(RESIDENCY_FLAGS) \
		--boot-plan $(ARENA_PLAN) $(BUILD)/zephyros.map \
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
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
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

# The SDK's own tests (tests/test_sdk.py) and the generated-manifest
# checks need the packed demo and the argument test program, so the
# standard check always builds them.  Neither is part of a production
# image: the demo lives in user/helloworld/ and zextest is an acceptance
# program.
check-fast: $(ZEX_USER_ROOT)/hello.zex $(MODULES)/zextest.zex
	$(PYTHON) -m unittest discover -s tests -v
	$(PYTHON) tools/mapcheck.py $(MAPCHECK_FLAGS) $(RESIDENCY_FLAGS) \
		--boot-plan $(ARENA_PLAN) $(BUILD)/zephyros.map \
		$(MEMORY_INC) $(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) tools/pack_image.py $(MAPCHECK_FLAGS) $(BUILD)/zephyros.ihx \
		$(BUILD)/zephyros.map $(BUILD) "$(PK_SUFFIX)" $(MEMORY_INC)
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

ioc-selftest:
	$(MAKE) BUILD=build-ioc-selftest ZOS_IOC_SELFTEST=1 \
		ZOS_SMOKE_HOST_SYNC=1 \
		build-ioc-selftest/zephyros-ioc-selftest.bin \
		build-ioc-selftest/zephyr80-ioc-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-ioc-selftest/zephyros.map \
		build-ioc-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_ioc_sim.py' -v
	@echo "ioc image: build-ioc-selftest/zephyros-ioc-selftest.bin"

# --- HID tooling, acceptance images, and hardware smoke -------------------
# hid-test proves the packed HID module and the class/source contracts on
# the host; hid-selftest builds the production-geometry acceptance
# image (SIO + IOC + HID from ROMFS) and replays it in the simulator;

hid-test: $(MODULES)/hid.zmod
	$(PYTHON) tools/zmodpack.py validate $(MODULES)/hid.zmod
	$(PYTHON) tools/zmodpack.py dump $(MODULES)/hid.zmod
	$(PYTHON) -m unittest discover -s tests -p 'test_hid.py' -v
	@echo "hid-test: OK"

hid-selftest:
	$(MAKE) BUILD=build-hid-selftest ZOS_HID_SELFTEST=1 \
		ZOS_SMOKE_HOST_SYNC=1 \
		build-hid-selftest/zephyros-hid-selftest.bin \
		build-hid-selftest/zephyr80-hid-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-hid-selftest/zephyros.map \
		build-hid-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_hid_sim.py' -v
	@echo "hid image: build-hid-selftest/zephyros-hid-selftest.bin"

# --- video tooling, acceptance images, and hardware smoke -----------
# v9958-test proves the packed module, the font asset pipeline and the
# class/platform/source contracts on the host; video-selftest builds
# the production-geometry acceptance image (SIO + IOC + HID + V9958
# from ROMFS) and replays it in the simulator against the V9958 peer;

v9958-test: $(MODULES)/v9958.zmod $(MODULES)/v9958-6x8.font
	$(PYTHON) tools/zmodpack.py validate $(MODULES)/v9958.zmod
	$(PYTHON) tools/zmodpack.py dump $(MODULES)/v9958.zmod
	$(PYTHON) tools/fontpack.py verify drivers/v9958/assets/font_cp850_6x8.inc
	$(PYTHON) -m unittest discover -s tests -p 'test_v9958.py' -v
	$(PYTHON) -m unittest discover -s tests -p 'test_v9958_console.py' -v
	@echo "v9958-test: OK"

video-selftest:
	$(MAKE) BUILD=build-video-selftest ZOS_VIDEO_SELFTEST=1 \
		ZOS_SMOKE_HOST_SYNC=1 \
		build-video-selftest/zephyros-video-selftest.bin \
		build-video-selftest/zephyr80-video-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-video-selftest/zephyros.map \
		build-video-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_v9958_sim.py' -v
	@echo "video image: build-video-selftest/zephyros-video-selftest.bin"

# The console acceptance image (package 19) boots the full production
# stack, reports the selected console input/output classes over the
# direct early SIO, and runs the unchanged M7 monitor through the
# HID-input/V9958-output binding.  The serial portion proves boot, the
# renderer char interface and the binding; the visible monitor session
# is replayed from these exact bytes by tests/test_console_sim.py and typed
# by the operator on hardware.
console-selftest:
	$(MAKE) BUILD=build-console-selftest ZOS_CONSOLE_SELFTEST=1 \
		ZOS_SMOKE_HOST_SYNC=1 \
		build-console-selftest/zephyros-console-selftest.bin \
		build-console-selftest/zephyr80-console-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-console-selftest/zephyros.map \
		build-console-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_console_sim.py' -v
	@echo "console image: build-console-selftest/zephyros-console-selftest.bin"

# --- console integration acceptance and fallback matrix (package 20) --------------
# console-integration-selftest is the final acceptance image: the full production stack
# plus the integration target diagnostic and the unchanged monitor
# launcher.  tests/test_console_acceptance.py replays the production bytes
# (build/zephyros.bin) and these bytes: boot order, binding, IOC
# command/status/bulk sequencing, HID queue traffic, the V9958 trace,
# the split console, the scripted monitor session with two exit/relaunch
# cycles, post-fault recovery and the malformed-call invariants.
#
# console-fallback-selftests builds every observable failure variant plus
# the accepted font-failure image and replays the matrix
# (tests/test_console_fallback.py); each variant's exact binding, early-SIO
# diagnostic and direction are asserted from the real linked bytes.

console-integration-selftest: all
	$(MAKE) BUILD=build-console-integration-selftest ZOS_CONSOLE_INTEGRATION_SELFTEST=1 ZOS_SMOKE_HOST_SYNC=1 \
		build-console-integration-selftest/zephyros-console-integration-selftest.bin \
		build-console-integration-selftest/zephyr80-console-integration-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-console-integration-selftest/zephyros.map \
		build-console-integration-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_console_acceptance.py' -v
	@echo "console integration image: build-console-integration-selftest/zephyros-console-integration-selftest.bin"

# console fallback matrix (package 20): the two service-set subsets the
# store expresses (nohid, nohid-nov9958) with the target composition
# diagnostic and the monitor launcher, replayed by
# tests/test_console_fallback.py and by the matching serial_smoke modes.

console-fallback-selftests:
	$(MAKE) BUILD=build-console-fb-nohid ZOS_CONSOLE_FALLBACK_SELFTEST=1 \
		ZOS_CONSOLE_FALLBACK=nohid ZOS_SMOKE_HOST_SYNC=1 \
		build-console-fb-nohid/zephyros-console-fb-nohid.bin
	$(MAKE) BUILD=build-console-fb-nohid-nov9958 \
		ZOS_CONSOLE_FALLBACK_SELFTEST=1 \
		ZOS_CONSOLE_FALLBACK=nohid-nov9958 ZOS_SMOKE_HOST_SYNC=1 \
		build-console-fb-nohid-nov9958/zephyros-console-fb-nohid-nov9958.bin
	$(PYTHON) -m unittest discover -s tests -p 'test_console_fallback.py' -v
	@echo "console fallback images:"
	@echo "  build-console-fb-nohid/zephyros-console-fb-nohid.bin"
	@echo "  build-console-fb-nohid-nov9958/zephyros-console-fb-nohid-nov9958.bin"

# --- M9 VFS bootstrap acceptance image ---------------------------------------
# Production-style VFS boot: ROMFS at /SYSTEM, the four resident modules and
# the monitor loaded through VFS, the optional-mount failure diagnosed, then
# the target VFS selftest replayed in the simulator.

m9-vfs-selftest: all
	$(MAKE) BUILD=build-m9-vfs-selftest ZOS_M9_VFS_SELFTEST=1 \
		build-m9-vfs-selftest/zephyros-m9-vfs-selftest.bin \
		build-m9-vfs-selftest/zephyr80-m9-vfs-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) build-m9-vfs-selftest/zephyros.map \
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
	$(PYTHON) tools/mapcheck.py --production --residency $(RESIDENCY) \
		--boot-plan build-m9-required-mount-selftest/arena.plan.json \
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
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
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
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-m9-fs-selftest/zephyros.map \
		build-m9-fs-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_syscall_sim.py' -v
	$(PYTHON) -m unittest discover -s tests -p 'test_m9_fs.py' -v
	@echo "m9 fs image: build-m9-fs-selftest/zephyros-m9-fs-selftest.bin"

# M9 SYS_EXEC acceptance image: SIO-only boot, the test RAM filesystem at
# `/` and `/R2`, and the native fstest/fschild/fsraw/fsdeep programs.  The
# target hook runs the user-side exec cases and the depth probe; the
# simulator replay pins the transcript and the resource baseline.

m9-exec-selftest: all
	$(MAKE) BUILD=build-m9-exec-selftest ZOS_M9_EXEC_SELFTEST=1 \
		build-m9-exec-selftest/zephyros-m9-exec-selftest.bin \
		build-m9-exec-selftest/zephyr80-m9-exec-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-m9-exec-selftest/zephyros.map \
		build-m9-exec-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_m9_exec_sim.py' -v
	@echo "m9 exec image: build-m9-exec-selftest/zephyros-m9-exec-selftest.bin"

# M9 IOC-FAT acceptance image (package 30): the production boot set plus
# `iocfat.zmod` and the production mount manifest (which attaches `/` to
# `iocfat` as optional), with the target-side provider selftest.  The
# simulator replay serves FS2 from a host peer, proves the cache rules with
# FS2 command counts, and measures the gate-stack depth through the
# provider.
m9-iocfat-selftest: all
	$(MAKE) BUILD=build-m9-iocfat-selftest ZOS_M9_IOCFAT_SELFTEST=1 \
		build-m9-iocfat-selftest/zephyros-m9-iocfat-selftest.bin \
		build-m9-iocfat-selftest/zephyr80-m9-iocfat-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-m9-iocfat-selftest/zephyros.map \
		build-m9-iocfat-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_iocfat_sim.py' -v
	@echo "m9 iocfat image: build-m9-iocfat-selftest/zephyros-m9-iocfat-selftest.bin"

# M9 integration acceptance image (package 31): the production boot path with
# `/` served by the resident IOC-FAT provider, and the whole native
# acceptance set.  The target hook exec's fstest.zex (which exec's the
# deep-matrix program fsfat.zex), the fsdeep.zex depth probe and the two
# fault probes, so the image is the milestone's end-to-end proof on a card.
m9-selftest: all
	$(MAKE) BUILD=build-m9-selftest ZOS_M9_SELFTEST=1 \
		build-m9-selftest/zephyros-m9-selftest.bin \
		build-m9-selftest/zephyr80-m9-selftest.bin
	$(PYTHON) tools/mapcheck.py --residency $(RESIDENCY) \
		build-m9-selftest/zephyros.map \
		build-m9-selftest/geometry/memory.inc \
		$(ZOS_PLATFORM_IRQ_INC)
	$(PYTHON) -m unittest discover -s tests -p 'test_m9_selftest_sim.py' -v
	@echo "m9 integration image: build-m9-selftest/zephyros-m9-selftest.bin"

# IPC substrate acceptance image (work package 39, M10 stage 3): the
# production boot path with the fixed-core substrate, the six synthetic
# bank-6 services placed by the acceptance loader, and the SDK client
# program booting as /SYSTEM/ipc.zex.  The client runs the promoted
# conformance suite and checks its own results; the simulator replay
# measures the spans, the stack high-water marks, the latch intervals and
# the IRQ delivery.
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
# the derived arena belongs to the test objects.
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
#   smoke-hardware-romfs-corrupt  build-romfs-corrupt/zephyros-romfs-corrupt.bin
#                                                                             make romfs-corrupt-selftest
#   smoke-hardware-ioc         build-ioc-selftest/zephyros-ioc-selftest.bin
#                                                                             make ioc-selftest
#   smoke-hardware-hid         build-hid-selftest/zephyros-hid-selftest.bin
#                                                                             make hid-selftest
#   smoke-hardware-video       build-video-selftest/zephyros-video-selftest.bin
#                                                                             make video-selftest
#   smoke-hardware-console     build-console-selftest/zephyros-console-selftest.bin
#                                                                             make console-selftest
#   smoke-hardware-console-integration             build-console-integration-selftest/zephyros-console-integration-selftest.bin  make console-integration-selftest
#   smoke-hardware-console-fb-*        build-console-fb-*/zephyros-console-fb-*.bin          make console-fallback-selftests
#   smoke-hardware-m9             build-m9-selftest/zephyros-m9-selftest.bin  make m9-selftest
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

hardware-images: all subsystem-selftest romfs-corrupt-selftest \
		console-integration-selftest ioc-selftest hid-selftest \
		video-selftest console-selftest console-fallback-selftests \
		m9-fs-selftest m9-exec-selftest m9-iocfat-selftest m9-selftest
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

smoke-hardware-ioc:
	@$(call smoke_preflight,build-ioc-selftest/zephyros-ioc-selftest.bin,ioc-selftest)
	$(PYTHON) tools/serial_smoke.py --mode ioc --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-hid:
	@$(call smoke_preflight,build-hid-selftest/zephyros-hid-selftest.bin,hid-selftest)
	$(PYTHON) tools/serial_smoke.py --mode hid --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-video:
	@$(call smoke_preflight,build-video-selftest/zephyros-video-selftest.bin,video-selftest)
	$(PYTHON) tools/serial_smoke.py --mode video --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-console:
	@$(call smoke_preflight,build-console-selftest/zephyros-console-selftest.bin,console-selftest)
	$(PYTHON) tools/serial_smoke.py --mode console --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

# Console modes: the full integration image (serial portion: early
# diagnostics, binding report and the target selftest; the interactive
# monitor session over HID/V9958 is the human gate) and the two retained
# observable failure variants, each asserting its exact binding.  The
# host sync is sent while the banner is pending, as for the other console
# smoke modes.

smoke-hardware-console-integration:
	@$(call smoke_preflight,build-console-integration-selftest/zephyros-console-integration-selftest.bin,console-integration-selftest)
	$(PYTHON) tools/serial_smoke.py --mode console-integration --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-console-fb-nohid:
	@$(call smoke_preflight,build-console-fb-nohid/zephyros-console-fb-nohid.bin,console-fallback-selftests)
	$(PYTHON) tools/serial_smoke.py --mode console-fb-nohid --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-console-fb-nohid-nov9958:
	@$(call smoke_preflight,build-console-fb-nohid-nov9958/zephyros-console-fb-nohid-nov9958.bin,console-fallback-selftests)
	$(PYTHON) tools/serial_smoke.py --mode console-fb-nohid-nov9958 --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m9-fs:
	@$(call smoke_preflight,build-m9-fs-selftest/zephyros-m9-fs-selftest.bin,m9-fs-selftest)
	$(PYTHON) tools/serial_smoke.py --mode m9-fs --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m9-exec:
	@$(call smoke_preflight,build-m9-exec-selftest/zephyros-m9-exec-selftest.bin,m9-exec-selftest)
	$(PYTHON) tools/serial_smoke.py --mode m9-exec --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m9-iocfat:
	@$(call smoke_preflight,build-m9-iocfat-selftest/zephyros-m9-iocfat-selftest.bin,m9-iocfat-selftest)
	$(PYTHON) tools/serial_smoke.py --mode m9-iocfat --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))

smoke-hardware-m9:
	@$(call smoke_preflight,build-m9-selftest/zephyros-m9-selftest.bin,m9-selftest)
	$(PYTHON) tools/serial_smoke.py --mode m9 --port $(or $(SERIAL_PORT),$(ZOS_SERIAL_PORT))
