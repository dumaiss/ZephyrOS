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

import json
import os
import re
import sys

# Defaults; overridden from platform/zephyr80/memory.inc and irq.inc when
# those paths are given on the command line, so the checker derives its
# facts from the actual layout declarations rather than a second copy of
# the memory map.
PAGE_ZERO_END = 0x0100
RESET_TOP = 0x0030
BANK7_BASE = 0x2000
# Revision-13 mode-11 bank-7 split (architecture 36.2, 36.5): the fixed
# core is 2000h-5FFFh, the cold half (the window) is 6000h-DFFFh.  The
# ISR stack and the service stack are reserved at the top of the fixed
# core, so the fixed core's runtime data stops below 5D00h.
BANK7_RUNTIME_END = 0xE000
REV13_WINDOW_SPLIT = 0x6000
SERVICE_STACK_BASE = 0x5D00
SERVICE_STACK_TOP = 0x5F00
SERVICE_STACK_SIZE = 0x0200
IRQ_ISR_STACK_BASE = 0x5F00
IRQ_ISR_STACK_TOP = 0x6000
IRQ_ISR_STACK_SIZE = 0x0100
# Cold-half geometry: the window code starts at 0x6000, the driver arena
# runs from its granule-aligned end to the derived window data base, and
# the window data runs to BANK7_RUNTIME_END.
WINDOW_BASE = REV13_WINDOW_SPLIT
WINDOW_DATA_BASE = 0xD800
# Host-synthetic geometry defaults.  A real image always applies its
# generated build/geometry/memory.inc, which must carry every derived
# value; these literals only seed checks that run without any include
# (e.g. a synthetic host map on the command line with no layout).
DRIVER_ARENA_BASE = 0x6200
DRIVER_ARENA_END = 0x6A00
DRIVER_GRANULE_SIZE = 0x0200
DRIVER_GRANULES = ((DRIVER_ARENA_END - DRIVER_ARENA_BASE)
                   // DRIVER_GRANULE_SIZE)
DATA_BASE = 0x5A00
# Fixed-core ledger fields (the derived include carries them; the
# synthetic defaults describe an empty overflow).
FIXED_CODE_END = 0x5200
FIXED_REQUIRED_BYTES = FIXED_CODE_END - BANK7_BASE
FIXED_OVERFLOW_BYTES = 0
FIXED_STACKS_BYTES = IRQ_ISR_STACK_SIZE + SERVICE_STACK_SIZE
FIXED_ROOM_BYTES = (REV13_WINDOW_SPLIT - BANK7_BASE
                    - FIXED_REQUIRED_BYTES - FIXED_OVERFLOW_BYTES
                    - FIXED_STACKS_BYTES)
COMMON_BASE = 0xE000
COMMON_CODE_END = 0xFD00
# Architectural ceiling on the packed `_COMMON` + `_COMMON_DATA` run and
# the M10 far-call reservation held back from it (platform/zephyr80/
# memory.inc; work package 30c).  M9 enforces
# `used <= COMMON_CEILING - COMMON_M10_RESERVATION`.
COMMON_CEILING = 0x0C00
COMMON_M10_RESERVATION = 0x0200
IRQ_VECTOR_BASE = 0xFD00
IRQ_VECTOR_SIZE = 0x0101
IRQ_STATE_BASE = 0xFE01
IRQ_STACK_BASE = 0xFE80
IRQ_STACK_TOP = 0xFF00
IRQ_STACK_SIZE = 0x0080
# The whole bank-7 space below common memory.
BANK7_TOP = BANK7_RUNTIME_END
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


# Nucleus residency classification (config/residency.json).  One entry per
# kernel object; mapcheck fails the build when a fixed-required object is
# compiled into the cold areas, and the derive step uses the tiers to
# split the fixed-core ledger into `required` and `overflow`.
RESIDENCY_TIERS = ("fixed-required", "cold", "moves")

# Fixed-core areas: the bytes of a fixed-required object that must link
# inside 2000h-5FFFh.
FIXED_AREAS = ("_CODE", "_HOME", "_INITIALIZER", "_GSINIT", "_GSFINAL",
               "_DATA", "_INITIALIZED")

# Cold-half areas: a fixed-required object must contribute no bytes here.
COLD_AREAS = ("_WINDOW", "_WINDOW_DATA")

REL_AREA_LINE = re.compile(r"^A (\S+) size ([0-9A-F]+) flags (\d+) addr ")


def load_residency(path):
    """(tiers, cold-half placement set, test-link set) of a residency
    file.  Every object carries exactly one tier; the `cold_half` list
    names the objects the build compiles into the cold areas; `test_link`
    names objects that only acceptance images link (they must still be
    classified, but the production linker script does not list them)."""
    with open(path) as handle:
        data = json.load(handle)
    if data.get("format") != "zos-residency/1":
        raise MapError("%s is not a zos-residency/1 file" % path)
    tiers = {}
    for entry in data.get("objects", []):
        name = entry.get("object")
        tier = entry.get("tier")
        if not name or tier not in RESIDENCY_TIERS:
            raise MapError("%s: bad residency entry %r" % (path, entry))
        if name in tiers:
            raise MapError("%s: %s is classified twice" % (path, name))
        if tier == "moves" and not isinstance(entry.get("stage"), int):
            raise MapError("%s: %s moves without a stage" % (path, name))
        tiers[name] = tier
    cold_half = tuple(data.get("cold_half", ()))
    for name in cold_half:
        if name not in tiers:
            raise MapError("%s: cold_half names unclassified %s"
                           % (path, name))
        if tiers[name] == "fixed-required":
            raise MapError("%s: cold_half names fixed-required %s"
                           % (path, name))
    test_link = tuple(data.get("test_link", ()))
    for name in test_link:
        if name not in tiers:
            raise MapError("%s: test_link names unclassified %s"
                           % (path, name))
    return tiers, cold_half, test_link


def linker_object_list(linker_path):
    """The production object stems of a linker script (its build/*.rel
    lines, in link order)."""
    names = []
    for line in open(linker_path):
        line = line.strip()
        if line.startswith("build/") and line.endswith(".rel"):
            names.append(os.path.basename(line)[:-4])
    return names


def rel_area_sizes(objdir):
    """Per-object area byte counts from a build directory's .rel files."""
    sizes = {}
    for name in sorted(os.listdir(objdir)):
        if not name.endswith(".rel"):
            continue
        areas = {}
        with open(os.path.join(objdir, name), errors="replace") as handle:
            for line in handle:
                match = REL_AREA_LINE.match(line)
                if match:
                    areas[match.group(1)] = int(match.group(2), 16)
        sizes[name[:-4]] = areas
    return sizes


def fixed_core_bytes(objdir, tiers):
    """(required, overflow) fixed-core bytes from the build's object files.

    `required` is the fixed-area content of fixed-required objects;
    `overflow` is everything else that links into the fixed areas
    (unclassified acceptance objects count as overflow)."""
    required = 0
    overflow = 0
    for name, areas in rel_area_sizes(objdir).items():
        total = sum(areas.get(area, 0) for area in FIXED_AREAS)
        if total == 0:
            continue
        if tiers.get(name) == "fixed-required":
            required += total
        else:
            overflow += total
    return required, overflow


def check_residency(objdir, tiers, cold_half, linker_path, test_link=(),
                    cold_extra=()):
    """Residency enforcement (architecture 36.5, package 38).

    Fails when the classification does not cover the production object
    list exactly (objects only acceptance images link are exempt through
    `test_link`), when a fixed-required object is compiled into the cold
    areas (a fixed-required symbol at or above 6000h), or when a
    classified object's linked areas disagree with its placement.

    `cold_extra` names objects that this image compiles into the cold
    areas on top of the residency file's `cold_half`: an image whose boot
    set leaves the cold half nearly empty may place a `moves` object
    there instead of overflowing it into the fixed core (the placement
    freedom of architecture 36.20).

    `test_link` objects are linked only by acceptance images, so their
    placement is not named by `cold_half` (production's cold set): a
    test-linked `cold` object is expected in the cold areas by its tier,
    exactly as production will compile it in package 41."""
    production = linker_object_list(linker_path)
    missing = [name for name in production if name not in tiers]
    if missing:
        raise MapError(
            "object(s) missing from the residency classification %s: %s"
            % (linker_path, ", ".join(sorted(missing))))
    allowed_extras = set(test_link) | set(cold_extra)
    stale = [name for name in tiers
             if name not in production and name not in allowed_extras]
    if stale:
        raise MapError(
            "the residency classification names object(s) not linked by %s: "
            "%s" % (linker_path, ", ".join(sorted(stale))))
    cold_set = set(cold_half) | set(cold_extra)
    cold_set.update(name for name in test_link
                    if tiers.get(name) == "cold")
    sizes = rel_area_sizes(objdir)
    for name, tier in sorted(tiers.items()):
        areas = sizes.get(name)
        if areas is None:
            if name in test_link:
                continue
            raise MapError("no object file for classified %s in %s"
                           % (name, objdir))
        cold_bytes = sum(areas.get(area, 0) for area in COLD_AREAS)
        fixed_bytes = sum(areas.get(area, 0) for area in FIXED_AREAS)
        in_cold = name in cold_set
        if tier == "fixed-required" and cold_bytes:
            raise MapError(
                "fixed-required object %s links %d bytes into the cold "
                "half (at or above %04Xh)" % (name, cold_bytes,
                                              REV13_WINDOW_SPLIT))
        if in_cold and cold_bytes == 0 and fixed_bytes:
            raise MapError(
                "object %s is declared cold-half but compiles into the "
                "fixed areas" % name)
        if not in_cold and cold_bytes:
            raise MapError(
                "object %s compiles into the cold half but the "
                "classification does not place it there" % name)


def apply_layout(values, source=None):
    """Apply memory-region declarations from a generated memory include.

    Every production image is a revision-13 two-region build (architecture
    36.5): the fixed areas (`_CODE`/`_HOME`/`_INITIALIZER`/`_GSINIT`/
    `_GSFINAL`, `_DATA`/`_INITIALIZED`) must link inside 2000h-5FFFh, and
    the cold areas (`_WINDOW`, `_WINDOW_DATA`) inside 6000h-DFFFh.  The
    generated include carries both data bases, the cold-half arena, the
    fixed-core ledger fields and the stack reservations."""
    global PAGE_ZERO_END, RESET_TOP, BANK7_BASE
    global BANK7_RUNTIME_END, BANK7_TOP, COMMON_BASE, COMMON_CODE_END
    global STACK_BASE, WINDOW_BASE, WINDOW_DATA_BASE
    global SERVICE_STACK_BASE, SERVICE_STACK_TOP, SERVICE_STACK_SIZE
    global IRQ_ISR_STACK_BASE, IRQ_ISR_STACK_TOP, IRQ_ISR_STACK_SIZE
    global GATE_STACK_SIZE, OWNED_AREAS
    global DRIVER_ARENA_BASE, DRIVER_ARENA_END, DRIVER_GRANULE_SIZE
    global DRIVER_GRANULES, DATA_BASE
    global FIXED_CODE_END, FIXED_REQUIRED_BYTES, FIXED_OVERFLOW_BYTES
    global FIXED_STACKS_BYTES, FIXED_ROOM_BYTES
    global ROMFS_STAGING_SIZE, ROM_ACCESS_BASE, ROM_ACCESS_SIZE
    global COMMON_CEILING, COMMON_M10_RESERVATION
    PAGE_ZERO_END = values["ZOS_PAGE_ZERO_END"]
    RESET_TOP = values["ZOS_RESET_TOP"]
    BANK7_BASE = values["ZOS_BANK7_BASE"]
    COMMON_BASE = values["ZOS_COMMON_BASE"]
    STACK_BASE = values["ZOS_COMMON_STACK_BASE"]
    BANK7_RUNTIME_END = values["ZOS_BANK7_RUNTIME_END"]
    BANK7_TOP = BANK7_RUNTIME_END
    COMMON_CODE_END = values["ZOS_COMMON_CODE_END"]
    # The revision-13 split, the two stack reservations and the two-data-base
    # geometry are build-time architecture constants; the generated include
    # always carries them.
    require_key(values, "ZOS_REV13_WINDOW_BASE", source)
    require_key(values, "ZOS_SERVICE_STACK_BASE", source)
    require_key(values, "ZOS_SERVICE_STACK_TOP", source)
    require_key(values, "ZOS_SERVICE_STACK_SIZE", source)
    WINDOW_BASE = values["ZOS_REV13_WINDOW_BASE"]
    SERVICE_STACK_BASE = values["ZOS_SERVICE_STACK_BASE"]
    SERVICE_STACK_TOP = values["ZOS_SERVICE_STACK_TOP"]
    SERVICE_STACK_SIZE = values["ZOS_SERVICE_STACK_SIZE"]
    if WINDOW_BASE != REV13_WINDOW_SPLIT:
        raise MapError("the mode-11 window base is %04Xh, expected %04Xh"
                       % (WINDOW_BASE, REV13_WINDOW_SPLIT))
    if not (BANK7_BASE < SERVICE_STACK_BASE <= SERVICE_STACK_TOP
            <= WINDOW_BASE <= BANK7_RUNTIME_END):
        raise MapError("the fixed-core stacks %04Xh-%04Xh do not sit below "
                       "the window base %04Xh"
                       % (SERVICE_STACK_BASE, SERVICE_STACK_TOP,
                          WINDOW_BASE))
    # The common ceiling and the M10 reservation are build-time
    # architecture constants; the generated include always carries them.
    require_key(values, "ZOS_COMMON_CEILING", source)
    require_key(values, "ZOS_COMMON_M10_RESERVATION", source)
    COMMON_CEILING = values["ZOS_COMMON_CEILING"]
    COMMON_M10_RESERVATION = values["ZOS_COMMON_M10_RESERVATION"]
    IRQ_ISR_STACK_BASE = values["ZOS_IRQ_ISR_STACK_BASE"]
    IRQ_ISR_STACK_TOP = values["ZOS_IRQ_ISR_STACK_TOP"]
    IRQ_ISR_STACK_SIZE = values["ZOS_IRQ_ISR_STACK_SIZE"]
    GATE_STACK_SIZE = values["ZOS_GATE_STACK_SIZE"]
    # The bank-7 geometry (fixed data base, cold-half arena start/end and
    # granule count, window data base) is derived per image and carried by
    # the generated memory include.  It is required: a missing key must
    # fail loudly rather than validate against a retired boundary.
    require_key(values, "ZOS_WINDOW_DATA_BASE", source)
    require_key(values, "ZOS_DRIVER_ARENA_BASE", source)
    require_key(values, "ZOS_DRIVER_ARENA_END", source)
    require_key(values, "ZOS_DRIVER_GRANULES", source)
    require_key(values, "ZOS_DATA_BASE", source)
    WINDOW_DATA_BASE = values["ZOS_WINDOW_DATA_BASE"]
    DRIVER_ARENA_BASE = values["ZOS_DRIVER_ARENA_BASE"]
    DRIVER_ARENA_END = values["ZOS_DRIVER_ARENA_END"]
    DRIVER_GRANULE_SIZE = values["ZOS_DRIVER_GRANULE_SIZE"]
    DRIVER_GRANULES = values["ZOS_DRIVER_GRANULES"]
    if not (WINDOW_BASE <= WINDOW_DATA_BASE <= BANK7_RUNTIME_END):
        raise MapError("window data base %04Xh is outside %04Xh-%04Xh"
                       % (WINDOW_DATA_BASE, WINDOW_BASE,
                          BANK7_RUNTIME_END))
    if not (WINDOW_BASE <= DRIVER_ARENA_BASE <= DRIVER_ARENA_END
            <= WINDOW_DATA_BASE):
        raise MapError("the driver arena %04Xh-%04Xh is outside the cold "
                       "half %04Xh-%04Xh"
                       % (DRIVER_ARENA_BASE, DRIVER_ARENA_END,
                          WINDOW_BASE, WINDOW_DATA_BASE))
    # The fixed-core ledger fields travel with the geometry so both ledger
    # printers (tools/pack_image.py, tools/zmodpack.py) show one truth.
    for name in ("ZOS_FIXED_CODE_END", "ZOS_FIXED_REQUIRED_BYTES",
                 "ZOS_FIXED_OVERFLOW_BYTES", "ZOS_FIXED_STACKS_BYTES",
                 "ZOS_FIXED_ROOM_BYTES"):
        require_key(values, name, source)
    # Fixed-core ledger fields (the derived include carries them; a
    # synthetic map carries no per-object attribution and reports an
    # empty overflow).
    FIXED_CODE_END = values["ZOS_FIXED_CODE_END"]
    FIXED_REQUIRED_BYTES = values["ZOS_FIXED_REQUIRED_BYTES"]
    FIXED_OVERFLOW_BYTES = values["ZOS_FIXED_OVERFLOW_BYTES"]
    FIXED_STACKS_BYTES = values["ZOS_FIXED_STACKS_BYTES"]
    FIXED_ROOM_BYTES = values["ZOS_FIXED_ROOM_BYTES"]
    # The fixed core's data starts at the derived base; the cold half's
    # data starts at the derived window data base.
    DATA_BASE = values["ZOS_DATA_BASE"]
    if not (BANK7_BASE < DATA_BASE <= SERVICE_STACK_BASE):
        raise MapError("fixed data base %04Xh is outside the fixed core"
                       % DATA_BASE)
    ROMFS_STAGING_SIZE = values["ZOS_ROMFS_STAGING_SIZE"]
    ROM_ACCESS_BASE = values["ZOS_ROM_ACCESS_BASE"]
    ROM_ACCESS_SIZE = values["ZOS_ROM_ACCESS_SIZE"]
    OWNED_AREAS = {
        "_CODE": (BANK7_BASE, WINDOW_BASE),
        "_HOME": (BANK7_BASE, WINDOW_BASE),
        "_INITIALIZER": (BANK7_BASE, WINDOW_BASE),
        "_GSINIT": (BANK7_BASE, WINDOW_BASE),
        "_GSFINAL": (BANK7_BASE, WINDOW_BASE),
        "_DATA": (DATA_BASE, SERVICE_STACK_BASE),
        "_INITIALIZED": (DATA_BASE, SERVICE_STACK_BASE),
        "_WINDOW": (WINDOW_BASE, WINDOW_DATA_BASE),
        "_WINDOW_DATA": (WINDOW_DATA_BASE, BANK7_RUNTIME_END),
        "_COMMON": (COMMON_BASE, COMMON_CODE_END),
        "_COMMON_DATA": (COMMON_BASE, COMMON_CODE_END),
    }


# Fixed-core loadable sections whose exclusive maximum end defines the
# fixed code end in derived builds.  The cold half's loadable section is
# `_WINDOW`, measured separately.
FIXED_LOADABLE_AREAS = ("_CODE", "_HOME", "_INITIALIZER", "_GSINIT",
                        "_GSFINAL")


def align_up(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def loadable_end(areas, symbols, names=None):
    """Exclusive maximum end of every loadable bank-7 area."""
    end = None
    for name in (names or FIXED_LOADABLE_AREAS):
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


# Packed permanent common content in physical bank 0 (E000h-FCFFh).  The
# fixed absolute reservations above it (IM2 vector page, IRQ transition
# state, ROM-access stub) are declared separately and are not part of this
# run, so these two areas are the whole "used" figure the common ledger
# reports.
COMMON_AREAS = ("_COMMON", "_COMMON_DATA")


def area_sizes(areas, symbols, names):
    """Sizes of the named areas, read from the linker-synthesized `l__`
    symbols when present and from the area table otherwise."""
    sizes = {}
    for name in names:
        base = name.lstrip("_")
        if "l__" + base in symbols:
            sizes[name] = symbols["l__" + base]
        elif name in areas:
            sizes[name] = areas[name][1]
        else:
            sizes[name] = 0
    return sizes


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


# Optional floor override for acceptance images (see geometry_main).
ARENA_MIN_GRANULES = None


def derive_geometry(map_text, values, min_granules=None,
                    fixed_required_bytes=None, fixed_overflow_bytes=None):
    """Compute the derived two-region bank-7 geometry from a probe map.

    Fixed core (2000h-5FFFh): `fixed_code_end` is the exclusive end of the
    fixed loadable areas (`_CODE`/`_HOME`/`_INITIALIZER`/`_GSINIT`/
    `_GSFINAL`); the fixed runtime data base is
    `align_down(service_stack_base - data_size, 512)`.

    Cold half (6000h-DFFFh): `window_code_end` is the exclusive end of
    `_WINDOW`; the window data base is
    `align_down(BANK7_RUNTIME_END - window_data_size, 512)`; the driver
    arena runs from `align_up(window_code_end, 512)` to that data base.
    Raises MapError when either region's content does not fit.

    There is no granule floor: the arena only has to hold the image's
    declared boot set, which tools/zmodpack.py's plan and
    tools/pack_image.py enforce against `arena.plan.json`."""
    areas, symbols = parse_map(map_text)
    granule = values["ZOS_DRIVER_GRANULE_SIZE"]
    fixed_top = values["ZOS_SERVICE_STACK_BASE"]
    data_size = runtime_data_size(areas, symbols)
    fixed_code_end = loadable_end(areas, symbols, FIXED_LOADABLE_AREAS)
    if data_size > fixed_top - BANK7_BASE:
        raise MapError(
            "fixed runtime data size %d bytes does not fit the fixed core "
            "2000h-%04Xh" % (data_size, fixed_top - 1))
    data_base = align_down(fixed_top - data_size, granule)
    if fixed_code_end > data_base:
        raise MapError(
            "linked fixed code ends at %04Xh, past the derived fixed data "
            "base %04Xh (overflow the fixed core cannot hold)"
            % (fixed_code_end, data_base))
    if min_granules is not None:
        # Acceptance-image guard kept for callers that pass an explicit
        # floor; the production build has none.
        min_size = min_granules * granule
    else:
        min_size = 0
    window_code_end = loadable_end(areas, symbols, ("_WINDOW",))
    window_data_size = area_sizes(areas, symbols,
                                  ("_WINDOW_DATA",))["_WINDOW_DATA"]
    top = values["ZOS_BANK7_RUNTIME_END"]
    if window_data_size > top - REV13_WINDOW_SPLIT:
        raise MapError(
            "cold runtime data %d bytes does not fit %04Xh-%04Xh"
            % (window_data_size, REV13_WINDOW_SPLIT, top - 1))
    window_data_base = align_down(top - window_data_size, granule)
    if window_code_end > window_data_base:
        raise MapError(
            "linked cold code ends at %04Xh, past the derived cold data "
            "base %04Xh" % (window_code_end, window_data_base))
    start = align_up(window_code_end, granule)
    size = window_data_base - start
    if size < min_size:
        raise MapError(
            "derived driver arena %04Xh-%04Xh is %d bytes; the minimum is "
            "%d bytes (%d granules, linked cold code ends at %04Xh)"
            % (start, window_data_base - 1, size, min_size, min_granules,
               window_code_end))
    if size < 0 or size % granule != 0:
        raise MapError("derived driver arena is not granule aligned")
    common = area_sizes(areas, symbols, COMMON_AREAS)
    stacks = values["ZOS_SERVICE_STACK_SIZE"] + values["ZOS_IRQ_ISR_STACK_SIZE"]
    if fixed_required_bytes is None:
        fixed_required_bytes = fixed_code_end - BANK7_BASE
    if fixed_overflow_bytes is None:
        fixed_overflow_bytes = 0
    room = (REV13_WINDOW_SPLIT - BANK7_BASE
            - fixed_required_bytes - fixed_overflow_bytes - stacks)
    if room < 0:
        raise MapError(
            "the fixed core overflows: %d required + %d overflow + %d "
            "stacks bytes do not fit %d bytes"
            % (fixed_required_bytes, fixed_overflow_bytes, stacks,
               REV13_WINDOW_SPLIT - BANK7_BASE))
    geometry = {
        "kernel_code_end": window_code_end,
        "fixed_code_end": fixed_code_end,
        "common_code_size": common["_COMMON"],
        "common_data_size": common["_COMMON_DATA"],
        "arena_start": start,
        "arena_end": window_data_base,
        "granules": size // granule,
        "data_base": data_base,
        "data_size": data_size,
        "window_split": REV13_WINDOW_SPLIT,
        "window_code_end": window_code_end,
        "window_data_base": window_data_base,
        "window_data_size": window_data_size,
        "fixed_required_bytes": fixed_required_bytes,
        "fixed_overflow_bytes": fixed_overflow_bytes,
        "fixed_stacks_bytes": stacks,
        "fixed_room_bytes": room,
    }
    return geometry


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
        "; Bank-7 geometry for kernel/module/arena.asm.",
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
        "ZOS_FIXED_CODE_END": "0x%04X" % geometry["fixed_code_end"],
        "ZOS_DRIVER_ARENA_BASE": "0x%04X" % geometry["arena_start"],
        "ZOS_DRIVER_ARENA_END": "0x%04X" % geometry["arena_end"],
        "ZOS_DRIVER_GRANULES": str(geometry["granules"]),
        "ZOS_DATA_BASE": "0x%04X" % geometry["data_base"],
        "ZOS_DATA_SIZE": str(geometry["data_size"]),
        "ZOS_FIXED_REQUIRED_BYTES": str(geometry["fixed_required_bytes"]),
        "ZOS_FIXED_OVERFLOW_BYTES": str(geometry["fixed_overflow_bytes"]),
        "ZOS_FIXED_STACKS_BYTES": str(geometry["fixed_stacks_bytes"]),
        "ZOS_FIXED_ROOM_BYTES": str(geometry["fixed_room_bytes"]),
        "ZOS_COMMON_CODE_SIZE": str(geometry["common_code_size"]),
        "ZOS_COMMON_DATA_SIZE": str(geometry["common_data_size"]),
        "ZOS_REV13_WINDOW_BASE": "0x%04X" % geometry["window_split"],
        "ZOS_WINDOW_CODE_END": "0x%04X" % geometry["window_code_end"],
        "ZOS_WINDOW_DATA_BASE": "0x%04X" % geometry["window_data_base"],
        "ZOS_WINDOW_DATA_SIZE": str(geometry["window_data_size"]),
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


# Owned areas and their allowed placement range.  Fixed-core sections must
# end below the window; the cold areas live in the cold half, with the
# driver arena reserved for load-time modules between the cold code and
# the cold runtime data.
OWNED_AREAS = {
    "_CODE": (BANK7_BASE, WINDOW_BASE),
    "_HOME": (BANK7_BASE, WINDOW_BASE),
    "_INITIALIZER": (BANK7_BASE, WINDOW_BASE),
    "_GSINIT": (BANK7_BASE, WINDOW_BASE),
    "_GSFINAL": (BANK7_BASE, WINDOW_BASE),
    "_DATA": (DATA_BASE, SERVICE_STACK_BASE),
    "_INITIALIZED": (DATA_BASE, SERVICE_STACK_BASE),
    "_WINDOW": (WINDOW_BASE, WINDOW_DATA_BASE),
    "_WINDOW_DATA": (WINDOW_DATA_BASE, BANK7_RUNTIME_END),
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

# Common-resident only if the image links them.  Same reason as above, but
# the object is acceptance-only until package 41, so its absence is not an
# error: an image that does link it must place it in common memory, never
# in the fixed core or the cold half.
COMMON_ONLY_SYMBOLS = (
    # M10 stage 3 correction 01: the path-B aperture copy runs with mode
    # 10 and page 1 in force, where bank 7 is not mapped at all, so the
    # stub, its request block and its interrupt-push reservation must all
    # live in common memory.
    "_ipc_ap_copy",
    "_ipc_ap_req",
    "_ipc_ap_saved_latch",
    "_ipc_ap_saved_sp",
    "_ipc_ap_stack",
    "_ipc_ap_stack_top",
    # M10 stage 4: the service boot gate calls a bank-6 boot segment with
    # mode 10 selected over bank 6, where bank 7 is unmapped; its code,
    # transition state and the loader's upcall request block must live in
    # common memory.
    "_zos_service_run_boot",
    "zos_service_boot_entry",
    "zos_service_boot_saved_latch",
    "zos_service_boot_saved_iff",
    "_zos_service_upcall_block",
)

# Bank-7 internal routines (visible only while mode 11 is active).
BANK7_ROUTINES = ("zos_syscall_dispatch", "_irq_dispatch")

# M3 device registry routines: bank 7 only.  The SIO driver is not
# statically linked (it is a bank-6 service image now) and the static
# driver table was retired; what remains here is the registry the module
# loader still answers class queries from, and its state.  The console
# device is gone with the console syscalls (package 41): the console is a
# bank-6 service, so no `_console_*` symbol belongs in the nucleus map.
M3_BANK7_ROUTINES = (
    "_device_registry_init",
    "_device_register",
    "_device_get",
    "_device_find",
    "_device_interface",
    "_device_registry_checkpoint",
    "_device_registry_rollback",
)

# M3 registry objects and private state: bank-7 data or code, never common
# memory.
M3_BANK7_OBJECTS = (
    "_device_table",
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
    "_ace_",
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
# (the acceptance result globals use the `_ace_` prefix and are handled
# by the production rule).  The one exception is the IOC class shim
# (kernel/module/ioc_shim.c, package 41): it publishes the resident IOC
# service to a bank-7 module and is nucleus code by design.
STATIC_IOC_PREFIXES = ("_ioc_",)
IOC_SHIM_PREFIXES = ("_ioc_shim_",)

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
    "_module_load_source",
    "_module_count",
    "_module_free_granules",
    "_module_failed_count",
    "_module_get",
    "_module_code_allowed",
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
    "_process_prepare",
    "_process_launch",
    "_process_exit",
    "_process_abort",
    "_process_alloc_bank",
    "_process_slot_alloc",
    "_process_slot_release",
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
    "zos_proc_cont",
    "zos_proc_ix",
    "zos_proc_latch",
    "zos_proc_iff",
    "zos_proc_index",
    "zos_proc_status",
    "zos_proc_bank",
    "zos_proc_entry",
    "zos_proc_gate_sp",
    "zos_proc_gate_latch",
    "zos_proc_gate_iff",
    "zos_proc_frames",
    "zos_proc_frames_end",
)

USER_COPY_STAGING_SIZE = 64
USER_COPY_STACK_SIZE = 16

# Package 28 continuation state: ZOS_EXEC_MAX_DEPTH levels, with
# ZOS_EXEC_MAX_DEPTH - 1 gate-frame save areas of ZOS_EXEC_FRAME_SAVE
# bytes (include/zephyros/process.h; pinned here so a mismatch fails the
# production map rather than silently truncating a saved frame).
ZOS_EXEC_MAX_DEPTH = 4
ZOS_EXEC_FRAME_SAVE = 48

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

    # The fixed core's stack reservation 5D00h-6000h (service stack, then
    # ISR stack) is reserved by declaration: only the ABS areas that
    # declare the two stacks may occupy it.
    for name, (addr, size) in sorted(areas.items()):
        if size == 0 or name.startswith(("_IRQ_ISR_STACK",
                                         "_IPC_SERVICE_STACK")):
            continue
        if addr < WINDOW_BASE and addr + size > SERVICE_STACK_BASE:
            raise MapError(
                "%s at %04Xh (size %d) overlaps the fixed-core stack "
                "reservation %04Xh-%04Xh"
                % (name, addr, size, SERVICE_STACK_BASE, WINDOW_BASE - 1)
            )

    # Ordinary bank-7 data stops below the fixed-core stacks, and
    # ordinary common allocation stops below the IM2 page.
    for name in ("_DATA", "_INITIALIZED"):
        if name in areas:
            addr, size = areas[name]
            if addr + size > SERVICE_STACK_BASE:
                raise MapError(
                    "%s overlaps the fixed-core service stack at %04Xh"
                    % (name, SERVICE_STACK_BASE))
    for name in ("_COMMON", "_COMMON_DATA"):
        if name in areas:
            addr, size = areas[name]
            if addr + size > IRQ_VECTOR_BASE:
                raise MapError("%s overlaps the IM2 reservation at FD00h" % name)

    # The packed common window has an architectural ceiling with an M10
    # reservation held back (work package 30c; target architecture 2.18).
    # Growth that would eat the M10 far-call budget fails the build here.
    common_used = sum(areas[name][1] for name in ("_COMMON", "_COMMON_DATA")
                      if name in areas)
    common_limit = COMMON_CEILING - COMMON_M10_RESERVATION
    if common_used > common_limit:
        raise MapError(
            "packed common memory is %d bytes, over the %d-byte M9 limit "
            "(ceiling %d minus the %d-byte M10 reservation)"
            % (common_used, common_limit, COMMON_CEILING,
               COMMON_M10_RESERVATION))


# Symbols that must stay reachable while the window shows another bank:
# the gates, the dispatch bodies and data, the copy primitives and their
# staging, the upcall shim, and the driver arena bounds (module code and
# data an interrupt can dispatch into).  A symbol is acceptable in the
# fixed region (2000h-5FFFh) or in the permanently mapped common window
# (E000h+), never inside the cold half.
REV13_FIXED_SYMBOLS = (
    "irq_common_entry",
    "irq_isr_stack",
    "irq_saved_latch",
    "irq_saved_sp",
    "irq_ctc0_stub",
    "_irq_arch_lock",
    "_irq_arch_unlock",
    "zos_syscall_entry",
    "zos_syscall_dispatch",
    "_irq_dispatch",
    "_irq_active_source",
    "_boot_common_entry",
    "_zos_bank_copy",
    "_zos_user_copy_staging",
    "_platform_memctl_read",
    "_platform_memctl_write",
    "_zos_upcall",
    # M10 stage 3: the IPC substrate executes while the window shows the
    # resident service domain, so its dispatcher, its frame and descriptor
    # tables and its fixed transition primitives stay in the fixed core.
    "_ipc_lookup",
    "_ipc_syscall_call",
    "_ipc_domain_enter",
    "_ipc_domain_set",
    "_ipc_domain_exit",
    "_ipc_copy_run",
    "_ipc_invoke",
    "_ipc_invoke_here",
    # Correction 01: the aperture primitive, its common request block, and
    # the three path counters the acceptance replay reads out of the image.
    "_ipc_ap_copy",
    "_ipc_ap_req",
    "_ipc_copy_window",
    "_ipc_copy_aperture",
    "_ipc_copy_slow",
)

# Cold (cold-half-resident) objects: their absence is not an error (an
# object can be reclassified), but a symbol that exists and sits in the
# fixed region is one.  The syscall gate clears the page field before
# dispatching, so the dispatcher, the boot/idle path and the loader body
# only ever run with P = 0 and live in the cold half.
REV13_COLD_SYMBOLS = (
    "_kernel_main",
    "_kernel_syscall_dispatch",
    "_module_loader_load",
    "_zos_ipc_dispatch",
)


def rev13_symbol_sets():
    """(fixed, cold) symbol expectations for the two-region split."""
    return REV13_FIXED_SYMBOLS, REV13_COLD_SYMBOLS


def check_rev13_window(symbols):
    """Two-region split: fixed symbols outside the cold half, cold symbols
    inside it, and both stacks inside the fixed core."""
    if WINDOW_BASE is None:
        return
    fixed_symbols, cold_symbols = rev13_symbol_sets()
    for name in fixed_symbols:
        if name not in symbols:
            continue
        addr = symbols[name]
        if WINDOW_BASE <= addr < BANK7_TOP:
            raise MapError(
                "%s at %04Xh must stay visible while the window shows "
                "another bank: it is inside the cold half %04Xh-%04Xh"
                % (name, addr, WINDOW_BASE, BANK7_TOP - 1))
    for name, base in (("irq_isr_stack", IRQ_ISR_STACK_BASE),
                       ("irq_isr_stack_top", IRQ_ISR_STACK_TOP),
                       ("zos_service_stack", SERVICE_STACK_BASE),
                       ("zos_service_stack_top", SERVICE_STACK_TOP)):
        if name not in symbols:
            continue
        # The top labels are exclusive (a stack top is the first byte above
        # the reservation), so they may sit on the fixed core's end.
        if not (BANK7_BASE <= symbols[name] <= WINDOW_BASE):
            raise MapError(
                "the fixed-core stack symbol %s at %04Xh is not in "
                "2000h-%04Xh" % (name, symbols[name], WINDOW_BASE))
    if not (BANK7_BASE <= IRQ_ISR_STACK_BASE < WINDOW_BASE):
        raise MapError(
            "the ISR stack at %04Xh is not in the fixed region "
            "2000h-%04Xh" % (IRQ_ISR_STACK_BASE, WINDOW_BASE - 1))
    for name in cold_symbols:
        if name not in symbols:
            continue
        addr = symbols[name]
        if not (WINDOW_BASE <= addr < BANK7_TOP):
            raise MapError(
                "%s at %04Xh is classified cold but does not sit in the "
                "cold half %04Xh-%04Xh"
                % (name, addr, WINDOW_BASE, BANK7_TOP - 1))


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

    # Common-only routines must live in common memory when they are
    # linked at all (the production image has no IPC substrate yet).
    for name in COMMON_ONLY_SYMBOLS:
        if name not in symbols:
            continue
        addr = symbols[name]
        if not (COMMON_BASE <= addr < COMMON_CODE_END):
            raise MapError(
                "%s at %04Xh is not in the common region E000h-FEFFh"
                % (name, addr))

    # Bank-7 routines must stay inside the fixed core.
    for name in BANK7_ROUTINES:
        if name not in symbols:
            raise MapError("map has no symbol %s" % name)
        addr = symbols[name]
        if not (BANK7_BASE <= addr < WINDOW_BASE):
            raise MapError(
                "%s at %04Xh is outside the fixed core 2000h-%04Xh"
                % (name, addr, WINDOW_BASE - 1)
            )

    # M3: registry, console service, static drivers, ops tables, and
    # driver-private state live in bank 7 (code or runtime data), never in
    # the permanently mapped common region.
    for name in M3_BANK7_ROUTINES + M3_BANK7_OBJECTS:
        if name not in symbols:
            raise MapError("map has no M3 symbol %s" % name)
        addr = symbols[name]
        if not (BANK7_BASE <= addr < BANK7_TOP):
            raise MapError(
                "%s at %04Xh is not in the bank-7 region 2000h-DEFFh"
                % (name, addr)
            )
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(M3_BANK7_PREFIXES):
            if not (BANK7_BASE <= addr < BANK7_TOP):
                raise MapError(
                    "driver/service symbol %s at %04Xh is not bank-7 "
                    "resident" % (name, addr)
                )

    # Package 41: the four drivers are bank-6 service images; the nucleus
    # holds exactly one class shim for the IOC class a bank-7 module still
    # asks for (kernel/module/ioc_shim.c), and no driver symbol at all.
    for name, addr in symbols.items():
        if addr == 0 or name.startswith(IOC_SHIM_PREFIXES):
            continue
        for prefixes, what in (
                (STATIC_SIO_PREFIXES, "SIO"),
                (STATIC_IOC_PREFIXES, "IOC"),
                (STATIC_HID_PREFIXES, "HID"),
                (STATIC_V9958_PREFIXES, "V9958")):
            if name.startswith(prefixes):
                raise MapError(
                    "static %s symbol %s at %04Xh must not be linked; the "
                    "driver is a bank-6 service image"
                    % (what, name, addr))
    # M4: loader/relocator code and catalog/fixture data must stay in the
    # bank-7 window below the ISR stack.
    for name in M4_BANK7_ROUTINES:
        if name not in symbols:
            raise MapError("map has no M4 symbol %s" % name)
        addr = symbols[name]
        if not (BANK7_BASE <= addr < BANK7_TOP):
            raise MapError(
                "%s at %04Xh is not in the bank-7 region 2000h-DEFFh"
                % (name, addr)
            )
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(M4_BANK7_PREFIXES):
            if not (BANK7_BASE <= addr < BANK7_TOP):
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
        if not (BANK7_BASE <= addr < BANK7_TOP):
            raise MapError(
                "%s at %04Xh is not in the bank-7 region 2000h-DEFFh"
                % (name, addr))
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(ROMFS_BANK7_PREFIXES):
            if not (BANK7_BASE <= addr < BANK7_TOP):
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
        if not (BANK7_BASE <= addr < BANK7_TOP):
            raise MapError(
                "%s at %04Xh is not in the bank-7 region 2000h-DEFFh"
                % (name, addr))
    for name in ROMFS_M9_ROUTINES:
        if name not in symbols:
            raise MapError("map has no ROMFS symbol %s" % name)
        addr = symbols[name]
        if not (BANK7_BASE <= addr < BANK7_TOP):
            raise MapError(
                "%s at %04Xh is not in the bank-7 region 2000h-DEFFh"
                % (name, addr))
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(VFS_BANK7_PREFIXES):
            if not (BANK7_BASE <= addr < BANK7_TOP):
                raise MapError(
                    "VFS symbol %s at %04Xh is not bank-7 resident"
                    % (name, addr))

    # M6: process/ZEX code and the process table stay bank-7 resident.
    for name in M6_BANK7_ROUTINES:
        if name not in symbols:
            raise MapError("map has no M6 symbol %s" % name)
        addr = symbols[name]
        if not (BANK7_BASE <= addr < BANK7_TOP):
            raise MapError(
                "%s at %04Xh is not in the bank-7 region 2000h-DEFFh"
                % (name, addr))
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(M6_BANK7_PREFIXES):
            if not (BANK7_BASE <= addr < BANK7_TOP):
                raise MapError(
                    "process symbol %s at %04Xh is not bank-7 resident"
                    % (name, addr))

    # M7: diagnostic services and shared dispatcher helpers are bank-7
    # kernel code; they add no permanent-common state.
    for name in M7_BANK7_ROUTINES:
        if name not in symbols:
            raise MapError("map has no M7 symbol %s" % name)
        addr = symbols[name]
        if not (BANK7_BASE <= addr < BANK7_TOP):
            raise MapError(
                "%s at %04Xh is not in the bank-7 region 2000h-DEFFh"
                % (name, addr))
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(M7_BANK7_PREFIXES):
            if not (BANK7_BASE <= addr < BANK7_TOP):
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

    # Package 28: the SYS_EXEC path buffer is ordinary bank-7 runtime
    # data.  It is written by the handler and read by process_prepare(),
    # both in mode 11, so it has no reason to occupy the permanently
    # mapped common window (which is reserved for state that must stay
    # reachable across a mapping change).
    scratch = symbols.get("_zos_process_path_scratch")
    if scratch is None:
        raise MapError("map has no SYS_EXEC path buffer")
    scratch_low = DATA_BASE if WINDOW_BASE is None else WINDOW_DATA_BASE
    if not (scratch_low <= scratch < BANK7_TOP):
        raise MapError(
            "_zos_process_path_scratch at %04Xh is not in the bank-7 "
            "runtime data region %04Xh-%04Xh"
            % (scratch, scratch_low, BANK7_TOP - 1))

    # Package 28: the continuation stack must be sized for the pinned
    # depth and frame save, not truncated by a stale assembly .ds.
    frames_delta = (symbols["zos_proc_frames_end"]
                    - symbols["zos_proc_frames"])
    expected_frames = ZOS_EXEC_FRAME_SAVE * (ZOS_EXEC_MAX_DEPTH - 1)
    if frames_delta != expected_frames:
        raise MapError("exec frame saves are %d bytes, expected %d "
                       "(%d x %d)" % (frames_delta, expected_frames,
                                      ZOS_EXEC_MAX_DEPTH - 1,
                                      ZOS_EXEC_FRAME_SAVE))

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

    fixed_end = loadable_end(areas, symbols, FIXED_LOADABLE_AREAS)
    cold_end = loadable_end(areas, symbols, ("_WINDOW",))
    if fixed_end > DATA_BASE:
        raise MapError(
            "linked fixed code ends at %04Xh, inside the fixed runtime data "
            "region at %04Xh" % (fixed_end, DATA_BASE))
    if cold_end > arena_start:
        raise MapError(
            "linked cold code ends at %04Xh, inside the driver arena at "
            "%04Xh" % (cold_end, arena_start))
    # Derived geometry: the exported arena bounds must match the layout
    # build's declarations exactly, the start must be the aligned end of
    # the linked cold code, and the end must be the derived cold data
    # base.  Synthetic host maps carry no __kernel_code_end and skip the
    # derived-only relation checks.
    kernel_code_end = symbols.get("__kernel_code_end")
    if kernel_code_end is not None:
        if kernel_code_end != cold_end:
            raise MapError(
                "__kernel_code_end is %04Xh, linked cold code ends at "
                "%04Xh" % (kernel_code_end, cold_end))
        expected_start = align_up(kernel_code_end, DRIVER_GRANULE_SIZE)
        if arena_start != expected_start:
            raise MapError(
                "__driver_arena_start is %04Xh, expected align_up(%04Xh, "
                "%04Xh) = %04Xh" % (arena_start, kernel_code_end,
                                    DRIVER_GRANULE_SIZE, expected_start))
        # The fixed-core ledger fields travel in the generated include;
        # the linked extents must reproduce them.
        if symbols.get("s__CODE") != BANK7_BASE:
            raise MapError("s__CODE is %04Xh, expected the fixed core base "
                           "%04Xh" % (symbols.get("s__CODE", -1), BANK7_BASE))
        if FIXED_CODE_END != fixed_end:
            raise MapError(
                "the derived fixed code end %04Xh does not match the linked "
                "fixed areas end %04Xh" % (FIXED_CODE_END, fixed_end))

    # The fixed runtime data base is derived: it must sit below the stacks
    # and the fixed data must stay below them too (checked through the
    # owned-area contract); the cold data base saturates the cold half.
    if symbols.get("s__DATA") != DATA_BASE:
        raise MapError("s__DATA is %04Xh, expected derived data base %04Xh"
                       % (symbols.get("s__DATA", -1), DATA_BASE))
    if symbols.get("s__WINDOW_DATA") != WINDOW_DATA_BASE:
        raise MapError("s__WINDOW_DATA is %04Xh, expected derived cold data "
                       "base %04Xh"
                       % (symbols.get("s__WINDOW_DATA", -1),
                          WINDOW_DATA_BASE))
    data_end = symbols["s__DATA"] + symbols.get("l__DATA", 0)
    initialized_end = (symbols["s__INITIALIZED"]
                       + symbols.get("l__INITIALIZED", 0))
    for label, end in (("_DATA", data_end),
                       ("_INITIALIZED", initialized_end)):
        if end > SERVICE_STACK_BASE:
            raise MapError("%s ends at %04Xh, overlapping the fixed-core "
                           "service stack reservation at %04Xh"
                           % (label, end, SERVICE_STACK_BASE))

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

    # Kernel body must be in bank 7.  With the revision-13 window the
    # kernel main body is window-resident (cold), so the bound is the
    # bank-7 space itself; check_rev13_window() proves it is not in the
    # fixed region.
    if "_kernel_main" not in symbols:
        raise MapError("map has no symbol _kernel_main")
    km = symbols["_kernel_main"]
    if not (BANK7_BASE <= km < BANK7_TOP):
        raise MapError("_kernel_main at %04Xh is outside the bank-7 space"
                       % km)

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
    check_rev13_window(symbols)
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
            # measure its area ends; a fixed placeholder here could
            # otherwise collide with kernel growth.
            if len(argv) not in (3, 5):
                raise MapError("usage: mapcheck.py geometry probe "
                               "<memory.inc> <out.inc>")
            parse_inc(argv[1])
            with open(argv[2], "w") as handle:
                handle.write(format_probe_geometry_inc())
            print("mapcheck: probe geometry (no arena placed) -> %s" % argv[2])
            return 0
        if mode == "derive":
            min_granules = None
            residency_path = None
            args = list(argv[1:])
            for flag in ("--min-granules", "--residency"):
                if flag in args:
                    at = args.index(flag)
                    value = args[at + 1]
                    del args[at:at + 2]
                    if flag == "--min-granules":
                        min_granules = int(value, 0)
                    else:
                        residency_path = value
            if len(args) not in (3, 4):
                raise MapError(
                    "usage: mapcheck.py geometry derive <probe.map> "
                    "<memory.inc> <out.inc> [<memory-variant.inc>] "
                    "[--residency FILE] [--min-granules N]")
            probe_path, memory_path, out_path = args[0], args[1], args[2]
            values = parse_inc(memory_path)
            fixed_required = fixed_overflow = None
            if residency_path is not None:
                tiers, _cold_half, _test_link = load_residency(residency_path)
                fixed_required, fixed_overflow = fixed_core_bytes(
                    os.path.dirname(os.path.abspath(probe_path)), tiers)
            with open(probe_path) as handle:
                geometry = derive_geometry(handle.read(), values,
                                           min_granules,
                                           fixed_required, fixed_overflow)
            with open(out_path, "w") as handle:
                handle.write(format_geometry_inc(geometry))
            if len(args) == 4:
                with open(memory_path) as handle:
                    variant = format_memory_variant(handle.read(), geometry)
                with open(args[3], "w") as handle:
                    handle.write(variant)
            print("mapcheck: derived fixed core: code end %04Xh, %d "
                  "required + %d overflow + %d stacks bytes, room %d"
                  % (geometry["fixed_code_end"],
                     geometry["fixed_required_bytes"],
                     geometry["fixed_overflow_bytes"],
                     geometry["fixed_stacks_bytes"],
                     geometry["fixed_room_bytes"]))
            print("mapcheck: derived cold half: code end %04Xh, arena "
                  "%04Xh-%04Xh (%d granules), data %d bytes at %04Xh "
                  "-> %s"
                  % (geometry["window_code_end"],
                     geometry["arena_start"], geometry["arena_end"],
                     geometry["granules"], geometry["window_data_size"],
                     geometry["window_data_base"], out_path))
            return 0
        raise MapError("unknown geometry mode %r" % mode)
    except MapError as exc:
        print("mapcheck: %s" % exc, file=sys.stderr)
        return 1


def boot_plan_usage(path):
    """(used granules, free granules, arena granules) from an arena plan."""
    with open(path) as handle:
        plan = json.load(handle)
    arena = plan["arena"]
    return (arena["used_granules"], arena["free_granules"],
            arena["granules"])


def main(argv):
    if len(argv) >= 2 and argv[1] == "geometry":
        return geometry_main(argv[2:])
    if len(argv) >= 2 and argv[1] == "residency":
        args = argv[2:]
        cold_extra = []
        if "--cold-extra" in args:
            at = args.index("--cold-extra")
            cold_extra = [name for name in args[at + 1].split(",") if name]
            del args[at:at + 2]
        if len(args) != 2 or args[0] != "--cold-stems":
            print("usage: mapcheck.py residency --cold-stems <file> "
                  "[--cold-extra a,b,c]", file=sys.stderr)
            return 2
        try:
            _tiers, cold_half, _test_link = load_residency(args[1])
        except MapError as exc:
            print("mapcheck: %s" % exc, file=sys.stderr)
            return 1
        print("\n".join(list(cold_half) + cold_extra))
        return 0
    production = "--production" in argv
    argv = [arg for arg in argv if arg != "--production"]
    residency_path = boot_plan_path = None
    cold_extra = ()
    for flag in ("--residency", "--boot-plan", "--cold-extra"):
        if flag in argv:
            at = argv.index(flag)
            value = argv[at + 1]
            del argv[at:at + 2]
            if flag == "--residency":
                residency_path = value
            elif flag == "--boot-plan":
                boot_plan_path = value
            else:
                cold_extra = tuple(name for name in value.split(",") if name)
    if len(argv) not in (2, 3, 4):
        print(__doc__)
        return 2
    if len(argv) >= 3:
        apply_layout(parse_inc(argv[2]), argv[2])
    if len(argv) == 4:
        apply_irq_layout(parse_inc(argv[3]))
    map_path = argv[1]
    try:
        areas, symbols = check_map(open(map_path).read(), production)
        if residency_path is not None:
            tiers, cold_half, test_link = load_residency(residency_path)
            root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
            check_residency(os.path.dirname(os.path.abspath(map_path)),
                            tiers, cold_half,
                            os.path.join(root, "linker", "zephyr80.lk"),
                            test_link, cold_extra)
        if boot_plan_path is not None:
            used, free, total = boot_plan_usage(boot_plan_path)
            if used > DRIVER_GRANULES:
                raise MapError(
                    "the declared boot set needs %d granules; the derived "
                    "arena holds %d" % (used, DRIVER_GRANULES))
    except MapError as exc:
        print("mapcheck: %s" % exc, file=sys.stderr)
        return 1
    print("mapcheck: OK (%d areas, %d symbols)%s"
          % (len(areas), len(symbols),
             " [production]" if production else ""))
    print("mapcheck: fixed core 2000h-%04Xh end %04Xh (%d required + %d "
          "overflow + %d stacks, %d room) | cold half %04Xh-%04Xh end "
          "%04Xh | arena %04Xh-%04Xh (%d granules) | data %04Xh/%04Xh"
          % (WINDOW_BASE - 1, FIXED_CODE_END, FIXED_REQUIRED_BYTES,
             FIXED_OVERFLOW_BYTES, FIXED_STACKS_BYTES, FIXED_ROOM_BYTES,
             WINDOW_BASE, BANK7_TOP - 1,
             symbols.get("__kernel_code_end",
                         loadable_end(areas, symbols, ("_WINDOW",))),
             DRIVER_ARENA_BASE, DRIVER_ARENA_END - 1, DRIVER_GRANULES,
             DATA_BASE, WINDOW_DATA_BASE))
    if boot_plan_path is not None:
        print("mapcheck: boot set uses %d of %d granules, %d free (%s)"
              % (used, total, free, boot_plan_path))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
