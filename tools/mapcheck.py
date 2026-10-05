#!/usr/bin/env python3
"""ZephyrOS map/layout checker (through M8).

Validates the sdldz80 map against the implemented mode 10/11 memory
ownership contract (see docs/memory.md):

    page zero     0000h-00FFh   reset copier only, below 0030h
    bank 7 kernel 2000h-arena   _CODE/_HOME/_INITIALIZER/_GSINIT/_GSFINAL
    driver arena  align_up(__kernel_code_end)-data base, >= 16 KiB
                                (the one derived geometry; the probe link
                                places no arena)
    bank 7 runtime data base-DFFFh  _DATA/_INITIALIZED (never ROM-loaded)
    bank-7 ISR stack DF00h-DFFFh    fixed 256-byte handler stack
    common         E000h-FEFFh  _COMMON/_COMMON_DATA, gates, console, panic
    common stack   FF00h-FFFFh  reserved; nothing may be linked there

The bank-7 runtime data base is derived, not fixed: the probe link
measures the runtime-data size (`l__DATA + l__INITIALIZED`), the data
base is `align_down(DF00h - data_size, 512)`, and the arena runs from
`align_up(__kernel_code_end, 512)` up to that data base.

M3 adds placement checks for the device registry, console service, static
driver descriptors/ops, and driver-private state: all of it must live in
bank 7 (loadable or runtime area, below the ISR stack) and none of it may
enter permanent common memory.

M4 reserved the bank-7 driver arena; M8 derives the production arena
start from the aligned linked-kernel end (`__kernel_code_end`) and
re-verifies the relation in the final map.  Linked sections must end
below the arena start, no area may overlap the arena, and
__driver_arena_start/end must match the declared granule geometry.
Loader/relocator routines and module/catalog symbols must stay bank-7
resident.

Empty areas do not get an area-table entry in sdldz80 maps; they still get
s__<area>/l__<area> symbols, so ownership is checked through the symbol
table and empty areas are accepted when their zero-length placement address
lies inside the expected region.

Usage:
    mapcheck.py [--production] <zephyros.map>
                [platform/zephyr80/memory.inc
                 [platform/zephyr80/irq.inc]]

    mapcheck.py geometry probe <memory.inc> <out_arena_geometry.inc>
    mapcheck.py geometry derive <probe.map> <memory.inc>
                <out_arena_geometry.inc> [<out_memory_variant.inc>]

--production additionally rejects every acceptance symbol (retired static
driver symbols, target-test runners/results and their generic seam hooks,
CTC0 acceptance helpers, the obsolete IRQ vector lookup, fixture catalogs,
and test SIO module entries) so acceptance code cannot leak into
production images.

Every image boots its modules from the recovery store through the VFS, so
the kernel VFS core, the generated mount table, the ROMFS provider and
the M9-only ROMFS accessors are required in every map.

The `geometry` commands implement the two-pass bank-7 build.  The
`probe` mode emits the bootstrap include used by the probe link, which
places **no** arena (the old fixed placeholder could constrain kernel
growth).  The `derive` mode measures the exclusive end of every loadable
bank-7 area in the probe-link map and the runtime-data size
(`l__DATA + l__INITIALIZED`), derives the data base as
`align_down(DF00h - data_size, 512)`, aligns the kernel end up to the
512-byte granule, enforces the minimum arena, and writes both the small
include consumed by kernel/module/arena.asm and (when requested) a full
memory.inc variant for the layout tools.  The derived values are
re-verified against the final map by the normal checks (__kernel_code_end,
the alignment relation, the data base, and the arena floor).
"""

import re
import sys

# Defaults; overridden from platform/zephyr80/memory.inc and irq.inc when
# those paths are given on the command line, so the checker derives its
# facts from the actual layout declarations rather than a second copy of
# the memory map.
PAGE_ZERO_END = 0x0100
RESET_TOP = 0x0030
BANK7_BASE = 0x2000
# Architectural maximum load end.  It is a ceiling, not a boundary: the
# production arena ends at the derived data base (below DF00h), so the
# loadable kernel only ever has to stay below the arena start.
BANK7_LOAD_END = 0xC000
BANK7_RUNTIME_END = 0xE000
# Host-synthetic geometry defaults.  A real image always applies its
# generated build/geometry/memory.inc, which must carry every derived
# value; these literals only seed checks that run without any include
# (e.g. a synthetic host map on the command line with no layout).
DRIVER_ARENA_BASE = 0xA000
DRIVER_ARENA_END = 0xC000
DRIVER_GRANULE_SIZE = 0x0200
DRIVER_GRANULES = ((DRIVER_ARENA_END - DRIVER_ARENA_BASE)
                   // DRIVER_GRANULE_SIZE)
DATA_BASE = DRIVER_ARENA_END
DRIVER_ARENA_MIN_SIZE = 0x4000
DRIVER_ARENA_MIN_GRANULES = 32
COMMON_BASE = 0xE000
COMMON_CODE_END = 0xFD00
IRQ_VECTOR_BASE = 0xFD00
IRQ_VECTOR_SIZE = 0x0101
IRQ_STATE_BASE = 0xFE01
IRQ_STACK_BASE = 0xFE80
IRQ_STACK_TOP = 0xFF00
IRQ_STACK_SIZE = 0x0080
IRQ_ISR_STACK_BASE = 0xDF00
IRQ_ISR_STACK_TOP = 0xE000
IRQ_ISR_STACK_SIZE = 0x0100
STACK_BASE = 0xFF00
MEM_TOP = 0x10000
GATE_STACK_SIZE = 256
ROMFS_STAGING_SIZE = 0x80
ROM_ACCESS_BASE = 0xEF00
ROM_ACCESS_SIZE = 8


def parse_inc(path):
    """Parse NAME = value declarations from a platform .inc file."""
    values = {}
    for line in open(path):
        line = line.split(";")[0].strip()
        match = re.match(r"([A-Za-z_]\w*)\s+=\s+(0[xX][0-9a-fA-F]+|\d+)$", line)
        if match:
            values[match.group(1)] = int(match.group(2), 0)
    return values


def parse_memory_inc(path):
    """Parse the layout declarations in platform/zephyr80/memory.inc."""
    return parse_inc(path)


def require_key(values, name, source):
    """Fetch a required declaration, naming the key and its include.

    The derived build must generate every bank-7 geometry value; a missing
    key is a hard error, never a silent fall back to a retired default."""
    if name not in values:
        raise MapError(
            "%s is missing from %s; the derived build must generate it"
            % (name, source if source else "the memory include"))


def apply_layout(values, source=None):
    """Apply memory-region declarations from a generated memory include."""
    global PAGE_ZERO_END, RESET_TOP, BANK7_BASE, BANK7_LOAD_END
    global BANK7_RUNTIME_END, COMMON_BASE, COMMON_CODE_END, STACK_BASE
    global IRQ_ISR_STACK_BASE, IRQ_ISR_STACK_TOP, IRQ_ISR_STACK_SIZE
    global GATE_STACK_SIZE, OWNED_AREAS
    global DRIVER_ARENA_BASE, DRIVER_ARENA_END, DRIVER_GRANULE_SIZE
    global DRIVER_GRANULES, DATA_BASE
    global DRIVER_ARENA_MIN_SIZE, DRIVER_ARENA_MIN_GRANULES
    global ROMFS_STAGING_SIZE, ROM_ACCESS_BASE, ROM_ACCESS_SIZE
    PAGE_ZERO_END = values["ZOS_PAGE_ZERO_END"]
    RESET_TOP = values["ZOS_RESET_TOP"]
    BANK7_BASE = values["ZOS_BANK7_BASE"]
    BANK7_LOAD_END = values["ZOS_BANK7_LOAD_END"]
    COMMON_BASE = values["ZOS_COMMON_BASE"]
    STACK_BASE = values["ZOS_COMMON_STACK_BASE"]
    BANK7_RUNTIME_END = values["ZOS_BANK7_RUNTIME_END"]
    COMMON_CODE_END = values["ZOS_COMMON_CODE_END"]
    IRQ_ISR_STACK_BASE = values["ZOS_IRQ_ISR_STACK_BASE"]
    IRQ_ISR_STACK_TOP = values["ZOS_IRQ_ISR_STACK_TOP"]
    IRQ_ISR_STACK_SIZE = values["ZOS_IRQ_ISR_STACK_SIZE"]
    GATE_STACK_SIZE = values["ZOS_GATE_STACK_SIZE"]
    # The bank-7 geometry (arena start/end, data base, granule count) is
    # derived per image and carried by the generated memory include.  It is
    # required: a missing key must fail loudly rather than validate against
    # the retired C000h boundary.
    require_key(values, "ZOS_DRIVER_ARENA_BASE", source)
    require_key(values, "ZOS_DRIVER_ARENA_END", source)
    require_key(values, "ZOS_DRIVER_GRANULES", source)
    require_key(values, "ZOS_DATA_BASE", source)
    DRIVER_ARENA_BASE = values["ZOS_DRIVER_ARENA_BASE"]
    DRIVER_ARENA_END = values["ZOS_DRIVER_ARENA_END"]
    DRIVER_GRANULE_SIZE = values["ZOS_DRIVER_GRANULE_SIZE"]
    DRIVER_GRANULES = values["ZOS_DRIVER_GRANULES"]
    # The runtime data starts at the derived data base, which is also the
    # exclusive end of the arena.
    DATA_BASE = values["ZOS_DATA_BASE"]
    DRIVER_ARENA_MIN_SIZE = values.get("ZOS_DRIVER_ARENA_MIN_SIZE", 0x4000)
    DRIVER_ARENA_MIN_GRANULES = values.get("ZOS_DRIVER_ARENA_MIN_GRANULES", 32)
    ROMFS_STAGING_SIZE = values["ZOS_ROMFS_STAGING_SIZE"]
    ROM_ACCESS_BASE = values["ZOS_ROM_ACCESS_BASE"]
    ROM_ACCESS_SIZE = values["ZOS_ROM_ACCESS_SIZE"]
    OWNED_AREAS = {
        "_CODE": (BANK7_BASE, DRIVER_ARENA_BASE),
        "_HOME": (BANK7_BASE, DRIVER_ARENA_BASE),
        "_INITIALIZER": (BANK7_BASE, DRIVER_ARENA_BASE),
        "_GSINIT": (BANK7_BASE, DRIVER_ARENA_BASE),
        "_GSFINAL": (BANK7_BASE, DRIVER_ARENA_BASE),
        "_DATA": (DATA_BASE, BANK7_RUNTIME_END),
        "_INITIALIZED": (DATA_BASE, BANK7_RUNTIME_END),
        "_COMMON": (COMMON_BASE, COMMON_CODE_END),
        "_COMMON_DATA": (COMMON_BASE, COMMON_CODE_END),
    }


# Loadable bank-7 sections whose exclusive maximum end defines
# __kernel_code_end in derived builds.
LOADABLE_AREAS = ("_CODE", "_HOME", "_INITIALIZER", "_GSINIT", "_GSFINAL")


def align_up(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def loadable_end(areas, symbols):
    """Exclusive maximum end of every loadable bank-7 area."""
    end = None
    for name in LOADABLE_AREAS:
        base = name.lstrip("_")
        if "s__" + base in symbols:
            candidate = symbols["s__" + base] + symbols.get("l__" + base, 0)
        elif name in areas:
            addr, size = areas[name]
            candidate = addr + size
        else:
            continue
        if end is None or candidate > end:
            end = candidate
    if end is None:
        raise MapError("map has no loadable bank-7 area")
    return end


RUNTIME_DATA_AREAS = ("_DATA", "_INITIALIZED")


def runtime_data_size(areas, symbols):
    """Total zero-initialized runtime data (`_DATA` + `_INITIALIZED`)."""
    total = 0
    for name in RUNTIME_DATA_AREAS:
        base = name.lstrip("_")
        if "l__" + base in symbols:
            total += symbols["l__" + base]
        elif name in areas:
            total += areas[name][1]
    return total


def align_down(value, alignment):
    return value & ~(alignment - 1)


# Optional floor override for acceptance images (see geometry_main): the
# 32-granule floor is the production budget gate; an acceptance image links
# target-test code and only needs an arena that holds its declared boot set
# (enforced separately by the arena plan).
ARENA_MIN_GRANULES = None


def derive_geometry(map_text, values, min_granules=None):
    """Compute the derived bank-7 geometry from a probe-link map.

    Returns a dict with kernel_code_end (unaligned exclusive end of the
    loadable areas), arena_start (granule aligned), arena_end (the derived
    data base) and granules.  Raises MapError when the arena would fall
    below the minimum size or the kernel would grow into the data base."""
    areas, symbols = parse_map(map_text)
    granule = values["ZOS_DRIVER_GRANULE_SIZE"]
    runtime_end = values["ZOS_BANK7_RUNTIME_END"]
    if min_granules is None:
        min_granules = values["ZOS_DRIVER_ARENA_MIN_GRANULES"]
    min_size = min_granules * granule
    end = loadable_end(areas, symbols)
    data_size = runtime_data_size(areas, symbols)
    if data_size > runtime_end - BANK7_BASE:
        raise MapError(
            "runtime data size %d bytes does not fit the bank-7 window "
            "2000h-%04Xh" % (data_size, runtime_end - 1))
    data_base = align_down(runtime_end - data_size, granule)
    arena_end = data_base
    if end > arena_end:
        raise MapError(
            "linked kernel loadable areas end at %04Xh, past the derived "
            "data base (arena end) %04Xh" % (end, arena_end))
    start = align_up(end, granule)
    size = arena_end - start
    if size < min_size:
        raise MapError(
            "derived driver arena %04Xh-%04Xh is %d bytes; the minimum is "
            "%d bytes (%d granules, linked kernel ends at %04Xh)"
            % (start, arena_end - 1, size, min_size, min_granules, end))
    if size % granule != 0:
        raise MapError("derived driver arena is not granule aligned")
    return {
        "kernel_code_end": end,
        "arena_start": start,
        "arena_end": arena_end,
        "granules": size // granule,
        "data_base": data_base,
        "data_size": data_size,
    }


def format_probe_geometry_inc():
    """Bootstrap include for the probe link: no arena is placed.

    The probe image only has to link; the derive step measures its loadable
    area ends.  Placing nothing means kernel growth can never collide with
    a placeholder arena."""
    return ("; Generated by tools/mapcheck.py geometry -- do not edit.\n"
            "; Probe-link geometry: the probe image places no arena; the\n"
            "; derive step measures the loadable areas in its map.\n"
            "ZOS_ARENA_PROBE = 1\n")


def format_geometry_inc(geometry):
    lines = [
        "; Generated by tools/mapcheck.py geometry -- do not edit.",
        "; Driver-arena geometry for kernel/module/arena.asm.",
        "ZOS_ARENA_PROBE = 0",
        "ZOS_KERNEL_CODE_END = 0x%04X" % geometry["kernel_code_end"],
        "ZOS_DRIVER_ARENA_BASE = 0x%04X" % geometry["arena_start"],
        "ZOS_DRIVER_ARENA_END = 0x%04X" % geometry["arena_end"],
        "ZOS_DATA_BASE = 0x%04X" % geometry["data_base"],
    ]
    return "\n".join(lines) + "\n"


def format_memory_variant(memory_inc_text, geometry):
    """Full memory.inc copy with the derived geometry declarations."""
    replacements = {
        "ZOS_ARENA_PROBE": "0",
        "ZOS_KERNEL_CODE_END": "0x%04X" % geometry["kernel_code_end"],
        "ZOS_DRIVER_ARENA_BASE": "0x%04X" % geometry["arena_start"],
        "ZOS_DRIVER_ARENA_END": "0x%04X" % geometry["arena_end"],
        "ZOS_DRIVER_GRANULES": str(geometry["granules"]),
        "ZOS_DATA_BASE": "0x%04X" % geometry["data_base"],
        "ZOS_DATA_SIZE": str(geometry["data_size"]),
    }
    seen = set()
    lines = []
    for line in memory_inc_text.splitlines():
        code = line.split(";")[0]
        match = re.match(r"([A-Za-z_]\w*)\s*=", code)
        if match and match.group(1) in replacements:
            name = match.group(1)
            lines.append("%s = %s" % (name, replacements[name]))
            seen.add(name)
        else:
            lines.append(line)
    for name, value in replacements.items():
        if name not in seen:
            lines.append("%s = %s" % (name, value))
    return "\n".join(lines) + "\n"


def apply_irq_layout(values):
    """Apply IM2 vector/transition declarations from irq.inc."""
    global IRQ_VECTOR_BASE, IRQ_VECTOR_SIZE, IRQ_STATE_BASE
    global IRQ_STACK_BASE, IRQ_STACK_TOP, IRQ_STACK_SIZE
    IRQ_VECTOR_BASE = values.get("ZOS_IRQ_VECTOR_BASE", IRQ_VECTOR_BASE)
    IRQ_VECTOR_SIZE = values.get("ZOS_IRQ_VECTOR_SIZE", IRQ_VECTOR_SIZE)
    IRQ_STATE_BASE = values.get("ZOS_IRQ_STATE_BASE", IRQ_STATE_BASE)
    IRQ_STACK_BASE = values.get("ZOS_IRQ_STACK_BASE", IRQ_STACK_BASE)
    IRQ_STACK_TOP = values.get("ZOS_IRQ_STACK_TOP", IRQ_STACK_TOP)
    IRQ_STACK_SIZE = values.get("ZOS_IRQ_STACK_SIZE", IRQ_STACK_SIZE)


# Owned areas and their allowed placement range.  Loadable bank-7 sections
# must end below the derived driver arena; the arena itself is reserved
# for load-time modules and never carries linked content.  Runtime data
# starts at the derived data base (the arena's exclusive end).
OWNED_AREAS = {
    "_CODE": (BANK7_BASE, DRIVER_ARENA_BASE),
    "_HOME": (BANK7_BASE, DRIVER_ARENA_BASE),
    "_INITIALIZER": (BANK7_BASE, DRIVER_ARENA_BASE),
    "_GSINIT": (BANK7_BASE, DRIVER_ARENA_BASE),
    "_GSFINAL": (BANK7_BASE, DRIVER_ARENA_BASE),
    "_DATA": (DATA_BASE, BANK7_RUNTIME_END),
    "_INITIALIZED": (DATA_BASE, BANK7_RUNTIME_END),
    "_COMMON": (COMMON_BASE, COMMON_CODE_END),
    "_COMMON_DATA": (COMMON_BASE, COMMON_CODE_END),
}

# Every routine that changes the memory mapping, or is needed immediately
# after a mapping change, must be common-resident.
COMMON_ROUTINES = (
    "_boot_common_entry",
    "__zos_crt0_init",
    "__zos_mode_fault",
    "zos_enter_os_mode",
    "zos_leave_os_mode",
    "zos_os_continuation",
    "zos_syscall_entry",
    "_platform_memctl_read",
    "_platform_memctl_write",
    "_platform_bank_get",
    "_platform_bank_set",
    "_early_console_init",
    "_early_console_putc",
    "_early_console_puts",
    "_panic",
    "irq_ctc0_stub",
    "irq_ff_unexpected",
    "irq_common_entry",
    "_irq_arch_lock",
    "_irq_arch_unlock",
    "_irq_arch_init",
    "_rom_access_copy",
    # M6: process launch/resume/trap and the application-bank copy gate
    # all switch the memory mapping; the page-zero vector is copied into
    # process banks from common data.
    "_zos_process_launch",
    "_zos_process_resume",
    "zos_process_return_trap",
    "_zos_process_page_zero_vector",
    "_zos_bank_copy",
)

# Bank-7 internal routines (visible only while mode 11 is active).
BANK7_ROUTINES = ("zos_syscall_dispatch", "_irq_dispatch")

# M3 device/service routines: bank 7 only.  The SIO driver is not
# statically linked (M4 loads it as a ZMOD module) and the static driver
# table was retired; what remains here is the registry, the console
# service, and their state.
M3_BANK7_ROUTINES = (
    "_device_registry_init",
    "_device_register",
    "_device_get",
    "_device_find",
    "_device_interface",
    "_device_registry_checkpoint",
    "_device_registry_rollback",
    "_console_bind",
    "_console_bind_defaults",
    "_console_putc",
    "_console_write",
    "_console_poll",
    "_console_getc",
)

# M3 registry/console objects and private state: bank-7 data or code,
# never common memory.
M3_BANK7_OBJECTS = (
    "_device_table",
    "_console_in_handle",
    "_console_out_handle",
)

# Any symbol with one of these prefixes is M3 device/service code or state
# and must be bank-7 resident (the sweep also covers the test-only M3
# symbols present in the m3-selftest image).
M3_BANK7_PREFIXES = (
    "_device_",
    "_console_",
    "_m3_",
)

# Production images must not link any acceptance artifact: retired static
# driver symbols, target-test runners/results and their generic seam hooks,
# CTC0 acceptance helpers, the obsolete IRQ vector lookup, fixture
# catalogs, embedded boot catalogs (M5 boots from ROMFS), or test SIO
# module entries.  Enabled with --production (the Makefile passes it for
# non-acceptance images).
PRODUCTION_FORBIDDEN = (
    "_driver_init_static",
    "_boot_drivers",
    "_driver_static_",
    "_m3_",
    "_m4_selftest",
    "_m4_fixture",
    "_zmod_fixture",
    "_z4_fixture",
    "_z4_module_boot",
    "_zmod_catalog",
    "_target_test",
    "_zos_run_",
    "_zos_target_test_",
    "_zos_ctc0_",
    "_platform_ctc0_",
    "_platform_irq_vector",
    "_m7_",
    "_m8",
    "_sio_module_irq",
    "_sio_irq_module",
    "_sio_fail_module",
)

# M4 acceptance: the SIO driver must exist only as a dynamically loaded
# module.  Any linked SIO symbol means a static copy crept back into the
# kernel image.
STATIC_SIO_PREFIXES = ("_sio_", "_sio0b_", "_sio_driver")

# M8: the IOC driver is a dynamically loaded module too.  Any linked
# `_ioc_` symbol means a static copy crept back into the kernel image
# (the target-test runner symbols use the `_m8` prefix and are handled
# by the production/catalog rules).
STATIC_IOC_PREFIXES = ("_ioc_",)

# M8: the HID driver is module-only as well; any linked `_hid_` symbol
# means a static copy crept back into the kernel image.
STATIC_HID_PREFIXES = ("_hid_",)

# M8: the V9958 video driver is module-only too; any linked `_v9958_`
# symbol means a static copy crept back into the kernel image.
STATIC_V9958_PREFIXES = ("_v9958_",)

# M4 module-loader routines: bank-7 code and data only, and never inside
# the driver arena (which has no linked content by design).  The prefix
# sweep also covers the test-only fixture symbols in selftest images.
M4_BANK7_ROUTINES = (
    "_module_loader_init",
    "_module_load",
    "_module_load_source",
    "_module_count",
    "_module_free_granules",
    "_module_failed_count",
    "_module_get",
    "_module_code_allowed",
    "_zmod_validate_container",
    "_zmod_validate_source",
    "_zmod_copy_image",
    "_zmod_zero_bss",
    "_zmod_apply_relocations",
    "_zmod_zero_allocation",
    "_irq_registry_checkpoint",
    "_irq_registry_rollback",
    "_irq_registry_set_owner",
)

M4_BANK7_PREFIXES = ("_module_", "_zmod_", "_z4_", "_m4_",
                    "_irq_registry_")

# M5 ROMFS reader routines: bank-7 code only.  The embedded boot catalog
# (kernel/module/embedded.c) is fully retired and its return is prevented
# by the source-layout retention test; `_z4_module_boot`/`_zmod_catalog`
# remain rejected above as extra defence.
# `_romfs_lookup_ci` is M9's single ASCII-folded lookup (architecture
# section 32.13): the removed case-sensitive `_romfs_lookup` has no
# replacement beside it, so every image links this one name.
ROMFS_BANK7_ROUTINES = (
    "_romfs_init",
    "_romfs_lookup_ci",
    "_romfs_read",
    "_romfs_resource_size",
    "_romfs_resource_read",
)

ROMFS_BANK7_PREFIXES = ("_romfs_",)

# The directory accessor the ROMFS VFS provider's `readdir` needs
# (kernel/romfs/romfs.c).  Every image links it.
ROMFS_M9_ROUTINES = (
    "_romfs_entry_at",
)

# M9 VFS core and the statically linked ROMFS provider: bank-7 code and
# data only, below the ISR stack.  The generated `const` mount table lands
# in the bank-7 loadable image like other ROM-loaded const data.
VFS_BANK7_ROUTINES = (
    "_vfs_init",
    "_vfs_mount_all",
    "_vfs_phase",
    "_vfs_register_filesystem",
    "_vfs_find_provider",
    "_vfs_provider_checkpoint",
    "_vfs_provider_rollback",
    "_vfs_provider_set_owner",
    "_vfs_provider_owner",
    "_vfs_path_normalize",
    "_vfs_name_equal",
    "_vfs_mount_resolve",
    "_vfs_resolve_path",
    "_vfs_is_mountpoint",
    "_vfs_mount_is_root",
    "_vfs_mount_is_root_level",
    "_vfs_open",
    "_vfs_read",
    "_vfs_write",
    "_vfs_seek",
    "_vfs_sync",
    "_vfs_close",
    "_vfs_stat",
    "_vfs_mkdir",
    "_vfs_rmdir",
    "_vfs_unlink",
    "_vfs_rename",
    "_vfs_opendir",
    "_vfs_readdir",
    "_vfs_closedir",
    "_vfs_file_source",
    "_romfs_provider",
)

VFS_BANK7_PREFIXES = ("_vfs_",)

# M6 process/ZEX code and state: bank 7 only, below the ISR stack (the
# common-resident gates are listed in COMMON_ROUTINES instead).
M6_BANK7_ROUTINES = (
    "_process_exec",
    "_process_exit",
    "_process_abort",
    "_process_alloc_bank",
    "_process_slot_alloc",
    "_process_slot_release",
    "_process_launch_child",
    "_zex_validate_header",
)

M6_BANK7_PREFIXES = ("_process_", "_zex_")

# M7 diagnostic services and the shared dispatcher helpers: bank-7
# kernel code only.  The services reuse the M6 common copy gate, so no
# new permanent-common symbol is introduced.
M7_BANK7_ROUTINES = (
    "_zos_diag_mem_read",
    "_zos_diag_mem_write",
    "_zos_diag_io_in",
    "_zos_diag_io_out",
    "_zos_diag_bank_info",
    "_zos_diag_system_info",
    "_zos_diag_hw_in",
    "_zos_diag_hw_out",
    "_zos_syscall_success",
    "_zos_syscall_error",
    "_zos_syscall_block_ok",
)

M7_BANK7_PREFIXES = ("_zos_diag_",)

# M6 transition state and the syscall copy staging block (common RAM).
M6_COMMON_STATE = (
    "_zos_user_copy_staging",
    "zos_copy_op",
    "zos_copy_bank",
    "zos_copy_addr",
    "zos_copy_buffer",
    "zos_copy_length",
    "zos_copy_saved_sp",
    "zos_copy_saved_latch",
    "zos_copy_saved_iff",
    "zos_copy_stack",
    "zos_copy_stack_top",
    "zos_proc_saved_sp",
    "zos_proc_saved_latch",
    "zos_proc_saved_iff",
    "zos_proc_bank",
    "zos_proc_entry",
)

USER_COPY_STAGING_SIZE = 64
USER_COPY_STACK_SIZE = 16

# M5 ROM-access transition state: permanent common memory only.  The
# staging buffer and saved latch/IFF/request state are what the
# mode-00 window depends on; the transition itself is a mirrored,
# stackless stub, so there is deliberately no transition stack.
ROMFS_COMMON_STATE = (
    "_rom_access_staging",
    "rom_access_saved_latch",
    "rom_access_saved_iff",
    "rom_access_page",
    "rom_access_offset",
    "rom_access_length",
    "rom_access_busy",
)

# Transition-critical state block (common RAM).
COMMON_STATE = (
    "zos_gate_saved_sp",
    "zos_gate_saved_latch",
    "zos_gate_result_hl",
    "zos_gate_result_a",
    "zos_gate_result_c",
    "zos_gate_stack",
    "zos_gate_stack_top",
)

IRQ_STATE = (
    "irq_saved_latch", "irq_saved_sp", "irq_common_sp", "_irq_active_source",
)


class MapError(Exception):
    """The linked map violates the ZephyrOS memory contract."""


def parse_map(text):
    """Return (areas, symbols) from an sdldz80 wide map."""
    areas = {}
    symbols = {}
    for line in text.splitlines():
        area = re.match(r"\s*([A-Za-z_.][\w.]*)\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8}) =", line)
        if area:
            name, addr, size = area.group(1), int(area.group(2), 16), int(area.group(3), 16)
            if name != ".ABS.":
                areas[name] = (addr, size)
            continue
        sym = re.match(r"\s*([0-9A-Fa-f]{8})\s+(\S+)", line)
        if sym:
            symbols[sym.group(2)] = int(sym.group(1), 16)
    return areas, symbols


def check_areas(areas, symbols):
    # Page zero: only the reset copier and RST 30h vector areas, entirely
    # within 0000h-00FFh.
    reset_areas = [n for n in areas if n.startswith("_RESET")]
    page_zero_areas = [n for n in areas if n.startswith("_PAGE_ZERO")]
    if not reset_areas:
        raise MapError("map has no page-zero/reset area")
    for name in reset_areas + page_zero_areas:
        addr, size = areas[name]
        if addr >= PAGE_ZERO_END or addr + size > PAGE_ZERO_END:
            raise MapError(
                "%s at %04Xh (size %d) leaves page zero 0000h-00FFh"
                % (name, addr, size)
            )
    # The reset copier must stay strictly below the RST 30h vector.
    for name in reset_areas:
        addr, size = areas[name]
        if addr + size > RESET_TOP:
            raise MapError(
                "%s at %04Xh (size %d) overlaps the %04Xh RST 30h vector "
                "reservation" % (name, addr, size, RESET_TOP)
            )

    # Nothing may occupy the common stack reservation FF00h-FFFFh.
    for name, (addr, size) in sorted(areas.items()):
        if name in reset_areas:
            continue
        if addr + size > MEM_TOP:
            raise MapError("%s (size %d) exceeds the 16-bit space" % (name, size))
        if addr >= COMMON_BASE and addr + size > STACK_BASE:
            raise MapError(
                "%s at %04Xh (size %d) overlaps the common stack "
                "reservation FF00h-FFFFh" % (name, addr, size)
            )

    # M4/M8: nothing linked may occupy the derived driver arena (only
    # zero-length marker/declaration areas may sit on its boundaries).
    for name, (addr, size) in sorted(areas.items()):
        if size == 0:
            continue
        if addr < DRIVER_ARENA_END and addr + size > DRIVER_ARENA_BASE:
            raise MapError(
                "%s at %04Xh (size %d) overlaps the driver arena "
                "%04Xh-%04Xh" % (name, addr, size, DRIVER_ARENA_BASE,
                                  DRIVER_ARENA_END - 1)
            )

    # M5: the ROM-access transition block is a fixed reservation.  Only
    # the _ROM_ACCESS_STUB area itself may occupy it.  sdld lists ABS
    # areas as <name>0 with address 0 (only the size is meaningful); the
    # exported trampoline/sync labels above pin the real placement.
    block_start = ROM_ACCESS_BASE - 2
    block_end = ROM_ACCESS_BASE + ROM_ACCESS_SIZE
    stub_size = None
    for name, (addr, size) in areas.items():
        if name.startswith("_ROM_ACCESS_STUB"):
            stub_size = size if stub_size is None else stub_size + size
    if stub_size is None:
        raise MapError("map has no _ROM_ACCESS_STUB area")
    if stub_size != ROM_ACCESS_SIZE + 2:
        raise MapError(
            "_ROM_ACCESS_STUB is %d bytes, expected %d (trampoline plus "
            "mirrored stub)" % (stub_size, ROM_ACCESS_SIZE + 2))
    for name, (addr, size) in sorted(areas.items()):
        if name.startswith("_ROM_ACCESS_STUB") or size == 0 or addr == 0:
            continue
        if addr < block_end and addr + size > block_start:
            raise MapError(
                "%s at %04Xh (size %d) overlaps the ROM-access transition "
                "block %04Xh-%04Xh" % (name, addr, size, block_start,
                                       block_end - 1)
            )

    # Region ownership via s__/l__ symbols; the linker drops the area's
    # leading underscore in synthesized symbols (_CODE -> s__CODE).
    # Empty areas have no area-table entry but still get zero-length
    # placement symbols.
    for name, (start, end) in OWNED_AREAS.items():
        base = name.lstrip("_")
        for tag in ("s__", "l__"):
            if tag + base not in symbols:
                raise MapError("map has no symbol %s%s" % (tag, base))
        saddr = symbols["s__" + base]
        if not (start <= saddr < end):
            raise MapError(
                "%s at %04Xh is outside %04Xh-%04Xh"
                % (name, saddr, start, end - 1)
            )
        if name in areas:
            addr, size = areas[name]
            if addr != saddr:
                raise MapError("%s table addr %04Xh != s__%s %04Xh"
                               % (name, addr, base, saddr))
            if symbols["l__" + base] != size:
                raise MapError("l__%s = %d != table size %d"
                               % (base, symbols["l__" + base], size))
            if addr + size > end:
                raise MapError(
                    "%s at %04Xh (size %d) leaves %04Xh-%04Xh"
                    % (name, addr, size, start, end - 1)
                )
        elif symbols["l__" + base] != 0:
            raise MapError("%s is missing from the area table but l__%s != 0"
                           % (name, base))

    # Ordinary bank-7 data stops below the dedicated ISR stack, and
    # ordinary common allocation stops below the IM2 page.
    for name in ("_DATA", "_INITIALIZED"):
        if name in areas:
            addr, size = areas[name]
            if addr + size > IRQ_ISR_STACK_BASE:
                raise MapError("%s overlaps the bank-7 ISR stack at DF00h" % name)
    for name in ("_COMMON", "_COMMON_DATA"):
        if name in areas:
            addr, size = areas[name]
            if addr + size > IRQ_VECTOR_BASE:
                raise MapError("%s overlaps the IM2 reservation at FD00h" % name)


def check_symbols(areas, symbols, production=False):
    page_zero_areas = [n for n in areas if n.startswith("_PAGE_ZERO")]

    if production:
        for name, addr in symbols.items():
            if addr == 0:
                continue
            for prefix in PRODUCTION_FORBIDDEN:
                if name.startswith(prefix):
                    raise MapError(
                        "acceptance symbol %s at %04Xh must not be linked "
                        "into a production image" % (name, addr)
                    )

    # Mapping-changing routines and immediate continuations: common only.
    for name in COMMON_ROUTINES:
        if name not in symbols:
            raise MapError("map has no symbol %s" % name)
        addr = symbols[name]
        if not (COMMON_BASE <= addr < COMMON_CODE_END):
            raise MapError(
                "%s at %04Xh is not in the common region E000h-FEFFh" % (name, addr)
            )

    # Bank-7 routines must stay inside the loadable kernel region.
    for name in BANK7_ROUTINES:
        if name not in symbols:
            raise MapError("map has no symbol %s" % name)
        addr = symbols[name]
        if not (BANK7_BASE <= addr < BANK7_LOAD_END):
            raise MapError(
                "%s at %04Xh is outside the bank-7 loadable region "
                "2000h-BFFFh" % (name, addr)
            )

    # M3: registry, console service, static drivers, ops tables, and
    # driver-private state live in bank 7 (code or runtime data), never in
    # the permanently mapped common region.
    for name in M3_BANK7_ROUTINES + M3_BANK7_OBJECTS:
        if name not in symbols:
            raise MapError("map has no M3 symbol %s" % name)
        addr = symbols[name]
        if not (BANK7_BASE <= addr < IRQ_ISR_STACK_BASE):
            raise MapError(
                "%s at %04Xh is not in the bank-7 region 2000h-DEFFh"
                % (name, addr)
            )
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(M3_BANK7_PREFIXES):
            if not (BANK7_BASE <= addr < IRQ_ISR_STACK_BASE):
                raise MapError(
                    "driver/service symbol %s at %04Xh is not bank-7 "
                    "resident" % (name, addr)
                )

    # M4: the SIO driver is a dynamically loaded module; no static copy
    # may appear in the kernel map.
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(STATIC_SIO_PREFIXES):
            raise MapError(
                "static SIO symbol %s at %04Xh must not be linked; the SIO "
                "driver is loaded from a ZMOD module" % (name, addr)
            )

    # M8: the IOC driver is module-only as well.
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(STATIC_IOC_PREFIXES):
            raise MapError(
                "static IOC symbol %s at %04Xh must not be linked; the "
                "IOC driver is loaded from a ZMOD module" % (name, addr)
            )

    # M8: the HID driver is module-only as well.
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(STATIC_HID_PREFIXES):
            raise MapError(
                "static HID symbol %s at %04Xh must not be linked; the "
                "HID driver is loaded from a ZMOD module" % (name, addr)
            )

    # M8: the V9958 video driver is module-only as well.
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(STATIC_V9958_PREFIXES):
            raise MapError(
                "static V9958 symbol %s at %04Xh must not be linked; the "
                "V9958 driver is loaded from a ZMOD module" % (name, addr)
            )

    # M4: loader/relocator code and catalog/fixture data must stay in the
    # bank-7 window below the ISR stack.
    for name in M4_BANK7_ROUTINES:
        if name not in symbols:
            raise MapError("map has no M4 symbol %s" % name)
        addr = symbols[name]
        if not (BANK7_BASE <= addr < IRQ_ISR_STACK_BASE):
            raise MapError(
                "%s at %04Xh is not in the bank-7 region 2000h-DEFFh"
                % (name, addr)
            )
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(M4_BANK7_PREFIXES):
            if not (BANK7_BASE <= addr < IRQ_ISR_STACK_BASE):
                raise MapError(
                    "module symbol %s at %04Xh is not bank-7 resident"
                    % (name, addr)
                )

    # M5 ROMFS: the allocation-free reader is bank-7 code; the ROM-access
    # primitive, its staging buffer, and the saved transition state are
    # permanent common memory.
    for name in ROMFS_BANK7_ROUTINES:
        if name not in symbols:
            raise MapError("map has no ROMFS symbol %s" % name)
        addr = symbols[name]
        if not (BANK7_BASE <= addr < IRQ_ISR_STACK_BASE):
            raise MapError(
                "%s at %04Xh is not in the bank-7 region 2000h-DEFFh"
                % (name, addr))
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(ROMFS_BANK7_PREFIXES):
            if not (BANK7_BASE <= addr < IRQ_ISR_STACK_BASE):
                raise MapError(
                    "ROMFS symbol %s at %04Xh is not bank-7 resident"
                    % (name, addr))
    for name in ROMFS_COMMON_STATE:
        if name not in symbols:
            raise MapError("map has no ROM-access state symbol %s" % name)
        addr = symbols[name]
        if not (COMMON_BASE <= addr < STACK_BASE):
            raise MapError(
                "%s at %04Xh is not in the common region E000h-FEFFh"
                % (name, addr))
    staging = symbols["rom_access_saved_latch"] - symbols["_rom_access_staging"]
    if staging != ROMFS_STAGING_SIZE:
        raise MapError("ROM-access staging buffer is %d bytes, expected %d"
                       % (staging, ROMFS_STAGING_SIZE))

    # M5 ROM-access transition block: the trampoline lands exactly two
    # bytes before the mirrored stub at ZOS_ROM_ACCESS_BASE, the stub is
    # ROM_ACCESS_SIZE bytes, and the whole block is common-resident.
    expected_block = {
        "rom_access_trampoline": ROM_ACCESS_BASE - 2,
        "rom_access_sync": ROM_ACCESS_BASE,
        "rom_access_sync_end": ROM_ACCESS_BASE + ROM_ACCESS_SIZE,
    }
    for name, expected in expected_block.items():
        actual = symbols.get(name)
        if actual != expected:
            raise MapError("%s is at %04Xh, expected %04Xh"
                           % (name, actual if actual is not None else -1,
                              expected))
    if not (COMMON_BASE <= ROM_ACCESS_BASE - 2
            and ROM_ACCESS_BASE + ROM_ACCESS_SIZE <= COMMON_CODE_END):
        raise MapError(
            "ROM-access transition block %04Xh-%04Xh leaves common memory"
            % (ROM_ACCESS_BASE - 2, ROM_ACCESS_BASE + ROM_ACCESS_SIZE - 1))

    # M9: the VFS core and the statically linked ROMFS provider are bank-7
    # residents below the ISR stack; the generated mount table is const
    # ROM data in the bank-7 loadable image.
    for name in VFS_BANK7_ROUTINES:
        if name not in symbols:
            raise MapError("map has no VFS symbol %s" % name)
        addr = symbols[name]
        if not (BANK7_BASE <= addr < IRQ_ISR_STACK_BASE):
            raise MapError(
                "%s at %04Xh is not in the bank-7 region 2000h-DEFFh"
                % (name, addr))
    for name in ROMFS_M9_ROUTINES:
        if name not in symbols:
            raise MapError("map has no ROMFS symbol %s" % name)
        addr = symbols[name]
        if not (BANK7_BASE <= addr < IRQ_ISR_STACK_BASE):
            raise MapError(
                "%s at %04Xh is not in the bank-7 region 2000h-DEFFh"
                % (name, addr))
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(VFS_BANK7_PREFIXES):
            if not (BANK7_BASE <= addr < IRQ_ISR_STACK_BASE):
                raise MapError(
                    "VFS symbol %s at %04Xh is not bank-7 resident"
                    % (name, addr))

    # M6: process/ZEX code and the process table stay bank-7 resident.
    for name in M6_BANK7_ROUTINES:
        if name not in symbols:
            raise MapError("map has no M6 symbol %s" % name)
        addr = symbols[name]
        if not (BANK7_BASE <= addr < IRQ_ISR_STACK_BASE):
            raise MapError(
                "%s at %04Xh is not in the bank-7 region 2000h-DEFFh"
                % (name, addr))
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(M6_BANK7_PREFIXES):
            if not (BANK7_BASE <= addr < IRQ_ISR_STACK_BASE):
                raise MapError(
                    "process symbol %s at %04Xh is not bank-7 resident"
                    % (name, addr))

    # M7: diagnostic services and shared dispatcher helpers are bank-7
    # kernel code; they add no permanent-common state.
    for name in M7_BANK7_ROUTINES:
        if name not in symbols:
            raise MapError("map has no M7 symbol %s" % name)
        addr = symbols[name]
        if not (BANK7_BASE <= addr < IRQ_ISR_STACK_BASE):
            raise MapError(
                "%s at %04Xh is not in the bank-7 region 2000h-DEFFh"
                % (name, addr))
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(M7_BANK7_PREFIXES):
            if not (BANK7_BASE <= addr < IRQ_ISR_STACK_BASE):
                raise MapError(
                    "diagnostic symbol %s at %04Xh is not bank-7 "
                    "resident" % (name, addr))

    # M6: the copy staging block and process transition state are common;
    # the staging size and copy stack sizes are part of the contract.
    for name in M6_COMMON_STATE:
        if name not in symbols:
            raise MapError("map has no M6 common-state symbol %s" % name)
        addr = symbols[name]
        if not (COMMON_BASE <= addr < STACK_BASE):
            raise MapError(
                "%s at %04Xh is not in the common region E000h-FEFFh"
                % (name, addr))
    staging_delta = (symbols["zos_copy_op"]
                     - symbols["_zos_user_copy_staging"])
    if staging_delta != USER_COPY_STAGING_SIZE:
        raise MapError("user copy staging is %d bytes, expected %d"
                       % (staging_delta, USER_COPY_STAGING_SIZE))
    stack_delta = (symbols["zos_copy_stack_top"]
                   - symbols["zos_copy_stack"])
    if stack_delta != USER_COPY_STACK_SIZE:
        raise MapError("user copy stack is %d bytes, expected %d"
                       % (stack_delta, USER_COPY_STACK_SIZE))

    # M4/M8: exported arena bounds must match the layout build's declared
    # geometry and granule arithmetic exactly.  In derived (production)
    # builds the start is additionally pinned to the aligned end of the
    # linked loadable areas and the minimum size is enforced.
    arena_start = symbols.get("__driver_arena_start")
    arena_end = symbols.get("__driver_arena_end")
    if arena_start != DRIVER_ARENA_BASE:
        raise MapError("__driver_arena_start is %04Xh, expected %04Xh"
                       % (arena_start if arena_start is not None else -1,
                          DRIVER_ARENA_BASE))
    if arena_end != DRIVER_ARENA_END:
        raise MapError("__driver_arena_end is %04Xh, expected %04Xh"
                       % (arena_end if arena_end is not None else -1,
                          DRIVER_ARENA_END))
    if (arena_start & (DRIVER_GRANULE_SIZE - 1)) != 0:
        raise MapError("driver arena is not granule aligned")
    if (arena_end - arena_start) != DRIVER_GRANULES * DRIVER_GRANULE_SIZE:
        raise MapError(
            "driver arena is %d bytes, expected %d granules x %d"
            % (arena_end - arena_start, DRIVER_GRANULES, DRIVER_GRANULE_SIZE)
        )

    static_end = loadable_end(areas, symbols)
    if static_end > arena_start:
        raise MapError(
            "linked loadable bank-7 areas end at %04Xh, inside the driver "
            "arena at %04Xh" % (static_end, arena_start))
    # Derived geometry: the exported arena bounds must match the layout
    # build's declarations exactly, the start must be the aligned end of
    # the linked loadable areas, the end must be the derived data base,
    # and the production floor must hold.  Synthetic host maps carry no
    # __kernel_code_end and skip the derived-only relation checks.
    kernel_code_end = symbols.get("__kernel_code_end")
    if kernel_code_end is not None:
        min_granules = (ARENA_MIN_GRANULES if ARENA_MIN_GRANULES is not None
                        else DRIVER_ARENA_MIN_GRANULES)
        min_size = min_granules * DRIVER_GRANULE_SIZE
        if (arena_end - arena_start) < min_size:
            raise MapError(
                "derived driver arena is %d bytes, below the %d-byte "
                "minimum" % (arena_end - arena_start, min_size))
        if DRIVER_GRANULES < min_granules:
            raise MapError(
                "derived driver arena has %d granules, below the %d-granule "
                "minimum" % (DRIVER_GRANULES, min_granules))
        if kernel_code_end != static_end:
            raise MapError(
                "__kernel_code_end is %04Xh, linked loadable areas end at "
                "%04Xh" % (kernel_code_end, static_end))
        expected_start = align_up(kernel_code_end, DRIVER_GRANULE_SIZE)
        if arena_start != expected_start:
            raise MapError(
                "__driver_arena_start is %04Xh, expected align_up(%04Xh, "
                "%04Xh) = %04Xh" % (arena_start, kernel_code_end,
                                    DRIVER_GRANULE_SIZE, expected_start))

    # The runtime data base is derived and must saturate the bank-7 window
    # below the ISR stack: runtime data starts at the arena's exclusive end
    # and must stay below DF00h.
    if symbols.get("s__DATA") != DATA_BASE:
        raise MapError("s__DATA is %04Xh, expected derived data base %04Xh"
                       % (symbols.get("s__DATA", -1), DATA_BASE))
    data_end = symbols["s__DATA"] + symbols.get("l__DATA", 0)
    initialized_end = (symbols["s__INITIALIZED"]
                       + symbols.get("l__INITIALIZED", 0))
    for label, end in (("_DATA", data_end),
                       ("_INITIALIZED", initialized_end)):
        if end > IRQ_ISR_STACK_BASE:
            raise MapError("%s ends at %04Xh, overlapping the bank-7 ISR "
                           "stack reservation at DF00h" % (label, end))

    # Transition state block: common RAM, gate stack of the exact size.
    for name in COMMON_STATE:
        if name not in symbols:
            raise MapError("map has no state symbol %s" % name)
        addr = symbols[name]
        if not (COMMON_BASE <= addr < STACK_BASE):
            raise MapError(
                "%s at %04Xh is not in the common region E000h-FEFFh" % (name, addr)
            )
    stack_size = symbols["zos_gate_stack_top"] - symbols["zos_gate_stack"]
    if stack_size != GATE_STACK_SIZE:
        raise MapError("gate stack is %d bytes, expected %d" % (stack_size, GATE_STACK_SIZE))

    # M2 fixed absolute reservations.  ABS area start symbols are not
    # useful in sdld maps, so validate their exported boundary labels.
    required = {
        "irq_vector_page": IRQ_VECTOR_BASE,
        "irq_vector_guard": IRQ_VECTOR_BASE + 0x100,
        "irq_transition_stack": IRQ_STACK_BASE,
        "irq_transition_stack_top": IRQ_STACK_TOP,
        "irq_isr_stack": IRQ_ISR_STACK_BASE,
        "irq_isr_stack_top": IRQ_ISR_STACK_TOP,
    }
    for name, expected in required.items():
        if symbols.get(name) != expected:
            raise MapError("%s is at %04Xh, expected %04Xh"
                           % (name, symbols.get(name, -1), expected))
    if IRQ_VECTOR_SIZE != 0x101:
        raise MapError("IM2 page plus guard size is %d, expected 257" % IRQ_VECTOR_SIZE)
    if symbols["irq_transition_stack_top"] - symbols["irq_transition_stack"] != IRQ_STACK_SIZE:
        raise MapError("IRQ transition stack size is not %d" % IRQ_STACK_SIZE)
    if symbols["irq_isr_stack_top"] - symbols["irq_isr_stack"] != IRQ_ISR_STACK_SIZE:
        raise MapError("bank-7 ISR stack size is not %d" % IRQ_ISR_STACK_SIZE)
    for name in IRQ_STATE:
        addr = symbols.get(name)
        if addr is None or not (IRQ_STATE_BASE <= addr < IRQ_STACK_BASE):
            raise MapError("%s is not in IRQ transition state FE01h-FE7Fh" % name)

    code_end = symbols.get("_irq_kernel_code_end")
    expected_end = symbols["s__CODE"] + symbols["l__CODE"]
    if code_end != expected_end:
        raise MapError("_irq_kernel_code_end is %04Xh, linked _CODE ends at %04Xh"
                       % (code_end if code_end is not None else -1, expected_end))

    # Kernel body must be in bank 7.
    if "_kernel_main" not in symbols:
        raise MapError("map has no symbol _kernel_main")
    km = symbols["_kernel_main"]
    if not (BANK7_BASE <= km < DRIVER_ARENA_BASE):
        raise MapError("_kernel_main at %04Xh is outside the bank-7 loadable region" % km)

    # Reset copier must stay below the RST 30h reservation, and the
    # RST 30h vector must be exactly the three-byte JP stub at 0030h.
    if "_reset_end" not in symbols:
        raise MapError("map has no symbol _reset_end")
    if symbols["_reset_end"] > RESET_TOP:
        raise MapError(
            "_reset_end at %04Xh exceeds the %04Xh RST 30h reservation"
            % (symbols["_reset_end"], RESET_TOP)
        )
    if not page_zero_areas:
        raise MapError("map has no _PAGE_ZERO area for the RST 30h vector")
    if symbols.get("_zos_rst30_vector") != RESET_TOP:
        raise MapError(
            "_zos_rst30_vector is at %04Xh, expected %04Xh"
            % (symbols.get("_zos_rst30_vector", -1), RESET_TOP)
        )
    vector_size = symbols.get("_zos_rst30_end", 0) - RESET_TOP
    if vector_size != 3:
        raise MapError("RST 30h vector is %d bytes, expected exactly 3" % vector_size)


def check_map(text, production=False):
    areas, symbols = parse_map(text)
    check_areas(areas, symbols)
    check_symbols(areas, symbols, production)
    return areas, symbols


def geometry_main(argv):
    if not argv:
        print("usage: mapcheck.py geometry probe|derive ...", file=sys.stderr)
        return 2
    mode = argv[0]
    try:
        if mode == "probe":
            # Bootstrap geometry for the probe link: place no arena at all.
            # The probe image just has to link so the derive step can
            # measure its loadable area ends; a fixed placeholder here
            # could otherwise collide with kernel growth.
            if len(argv) != 3:
                raise MapError("usage: mapcheck.py geometry probe "
                               "<memory.inc> <out.inc>")
            parse_inc(argv[1])
            with open(argv[2], "w") as handle:
                handle.write(format_probe_geometry_inc())
            print("mapcheck: probe geometry (no arena placed) -> %s" % argv[2])
            return 0
        if mode == "derive":
            min_granules = None
            args = list(argv[1:])
            if "--min-granules" in args:
                at = args.index("--min-granules")
                min_granules = int(args[at + 1], 0)
                del args[at:at + 2]
            if len(args) not in (3, 4):
                raise MapError("usage: mapcheck.py geometry derive <probe.map> "
                               "<memory.inc> <out.inc> [<memory-variant.inc>] "
                               "[--min-granules N]")
            probe_path, memory_path, out_path = args[0], args[1], args[2]
            values = parse_inc(memory_path)
            with open(probe_path) as handle:
                geometry = derive_geometry(handle.read(), values, min_granules)
            with open(out_path, "w") as handle:
                handle.write(format_geometry_inc(geometry))
            if len(args) == 4:
                with open(memory_path) as handle:
                    variant = format_memory_variant(handle.read(), geometry)
                with open(args[3], "w") as handle:
                    handle.write(variant)
            print("mapcheck: derived arena %04Xh-%04Xh, %d granules "
                  "(%d bytes), kernel end %04Xh, data %d bytes at %04Xh "
                  "-> %s"
                  % (geometry["arena_start"], geometry["arena_end"],
                     geometry["granules"],
                     geometry["arena_end"] - geometry["arena_start"],
                     geometry["kernel_code_end"], geometry["data_size"],
                     geometry["data_base"], out_path))
            return 0
        raise MapError("unknown geometry mode %r" % mode)
    except MapError as exc:
        print("mapcheck: %s" % exc, file=sys.stderr)
        return 1


def main(argv):
    global ARENA_MIN_GRANULES
    if len(argv) >= 2 and argv[1] == "geometry":
        return geometry_main(argv[2:])
    production = "--production" in argv
    argv = [arg for arg in argv if arg != "--production"]
    if "--min-granules" in argv:
        at = argv.index("--min-granules")
        ARENA_MIN_GRANULES = int(argv[at + 1], 0)
        del argv[at:at + 2]
    if len(argv) not in (2, 3, 4):
        print(__doc__)
        return 2
    if len(argv) >= 3:
        apply_layout(parse_inc(argv[2]), argv[2])
    if len(argv) == 4:
        apply_irq_layout(parse_inc(argv[3]))
    try:
        areas, symbols = check_map(open(argv[1]).read(), production)
    except MapError as exc:
        print("mapcheck: %s" % exc, file=sys.stderr)
        return 1
    static_end = loadable_end(areas, symbols)
    print("mapcheck: OK (%d areas, %d symbols)%s"
          % (len(areas), len(symbols),
             " [production]" if production else ""))
    print("mapcheck: static kernel end %04Xh; driver arena %04Xh-%04Xh: "
          "%d bytes, %d granules, derived; data base %04Xh"
          % (static_end, DRIVER_ARENA_BASE, DRIVER_ARENA_END - 1,
             DRIVER_ARENA_END - DRIVER_ARENA_BASE, DRIVER_GRANULES,
             DATA_BASE))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
