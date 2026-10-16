#!/usr/bin/env python3
"""KonaOS map/layout checker.

Validates the sdldz80 map against the implemented mode 10/11 memory
ownership contract (see docs/memory.md):

    page zero     0000h-00FFh   reset copier only, below 0030h
    bank 7 kernel 2000h-5FFFh   fixed core: _CODE/_HOME/_INITIALIZER/
                                _GSINIT/_GSFINAL, _DATA/_INITIALIZED
    bank 7 kernel 6000h-DFFFh   cold half: _WINDOW/_WINDOW_DATA
    bank-7 ISR stack 5F00h-6000h  fixed 256-byte handler stack
    common         E000h-FEFFh  _COMMON/_COMMON_DATA, gates, console, panic
    common stack   FE88h-FFFFh  reserved; nothing may be linked there

The two bank-7 runtime data bases are derived, not fixed: the probe link
measures the runtime-data size (`l__DATA + l__INITIALIZED`) and the
exclusive end of the linked cold code (`_WINDOW`); the fixed data base is
`align_down(KOS_DRIVER_STACK_BASE - data_size, 512)` and the cold data
base is `align_up(window_code_end, 512)`, straight after the code.

Drivers are bank-6 images: the nucleus map must not link a driver symbol,
and nothing driver-like may enter permanent common memory.  There is no
reserved block inside bank 7: `__kernel_code_end` (the linked cold code
end, kernel/memory/bank7_end.asm) is the IRQ handler bound and the cold
data base anchor, and the relation is re-verified in the final map.

Empty areas do not get an area-table entry in sdldz80 maps; they still get
s__<area>/l__<area> symbols, so ownership is checked through the symbol
table and empty areas are accepted when their zero-length placement address
lies inside the expected region.

Usage:
    mapcheck.py [--production] <konaos.map>
                [platform/zephyr80/private/memory.inc
                 [platform/zephyr80/private/irq.inc]]

    mapcheck.py geometry probe <memory.inc> <out_bank7_end.inc>
    mapcheck.py geometry derive <probe.map> <memory.inc>
                <out_bank7_end.inc> [<out_memory_variant.inc>]

--production additionally rejects every acceptance symbol (static
driver symbols, target-test runners/results and their generic seam hooks,
CTC0 acceptance helpers, an IRQ vector lookup, fixture catalogs,
and test SIO module entries) so acceptance code cannot leak into
production images.

The `geometry` commands implement the two-pass bank-7 build.  The
`probe` mode emits the bootstrap include used by the probe link (no
boundary is placed; a fixed placeholder could constrain kernel growth).
The `derive` mode measures the exclusive end of the fixed loadable areas
and of `_WINDOW` in the probe-link map, derives the fixed data base as
`align_down(KOS_DRIVER_STACK_BASE - data_size, 512)` and the cold data
base as `align_up(window_code_end, 512)`, and writes both the small
include consumed by kernel/memory/bank7_end.asm and (when requested) a
full memory.inc variant for the layout tools.  The derived values are
re-verified against the final map by the normal checks (__kernel_code_end,
the data bases and the area ownership).
"""

import json
import os
import re
import sys

# Defaults; overridden from platform/zephyr80/private/memory.inc and irq.inc when
# those paths are given on the command line, so the checker derives its
# facts from the actual layout declarations rather than a second copy of
# the memory map.
PAGE_ZERO_END = 0x0100
RESET_TOP = 0x0030
BANK7_BASE = 0x2000
# Revision-13 mode-11 bank-7 split (architecture 2.2, 4.2): the fixed
# core is 2000h-5FFFh, the cold half (the window) is 6000h-DFFFh.  The
# ISR stack and the driver stack are reserved at the top of the fixed
# core, so the fixed core's runtime data stops below 5D00h.
BANK7_RUNTIME_END = 0xE000
REV13_WINDOW_SPLIT = 0x6000
DRIVER_STACK_BASE = 0x5D00
DRIVER_STACK_TOP = 0x5F00
DRIVER_STACK_SIZE = 0x0200
IRQ_ISR_STACK_BASE = 0x5F00
IRQ_ISR_STACK_TOP = 0x6000
IRQ_ISR_STACK_SIZE = 0x0100
# Cold-half geometry: the window code starts at 0x6000, the window data
# base is align_up of its linked end, and the window data runs to
# BANK7_RUNTIME_END.
WINDOW_BASE = REV13_WINDOW_SPLIT
WINDOW_DATA_BASE = 0xD800
# Host-synthetic geometry defaults.  A real image always applies its
# generated build/geometry/memory.inc, which must carry every derived
# value; these literals only seed checks that run without any include
# (e.g. a synthetic host map on the command line with no layout).
BANK7_DATA_ALIGN = 0x0200
DATA_BASE = 0x5A00
# Fixed-core ledger fields (the derived include carries them; the
# synthetic defaults describe an empty overflow).
FIXED_CODE_END = 0x5200
FIXED_REQUIRED_BYTES = FIXED_CODE_END - BANK7_BASE
FIXED_OVERFLOW_BYTES = 0
FIXED_STACKS_BYTES = IRQ_ISR_STACK_SIZE + DRIVER_STACK_SIZE
FIXED_ROOM_BYTES = (REV13_WINDOW_SPLIT - BANK7_BASE
                    - FIXED_REQUIRED_BYTES - FIXED_OVERFLOW_BYTES
                    - FIXED_STACKS_BYTES)
COMMON_BASE = 0xE000
COMMON_CODE_END = 0xFD00
# Architectural ceiling on the packed `_COMMON` + `_COMMON_DATA` run
# (platform/zephyr80/private/memory.inc, architecture 9):
# `used <= COMMON_CEILING` is the rule.
COMMON_CEILING = 0x0C00
IRQ_VECTOR_BASE = 0xFD00
IRQ_VECTOR_SIZE = 0x0101
IRQ_STATE_BASE = 0xFE01
IRQ_STACK_BASE = 0xFE08
IRQ_STACK_TOP = 0xFE88
IRQ_STACK_SIZE = 0x0080
# The whole bank-7 space below common memory.
BANK7_TOP = BANK7_RUNTIME_END
STACK_BASE = 0xFE88
MEM_TOP = 0x10000
# Default when the caller passes no generated include; the build's
# include carries KOS_GATE_STACK_SIZE (platform/zephyr80/private/memory.inc).
GATE_STACK_SIZE = 384
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
    """Parse the layout declarations in platform/zephyr80/private/memory.inc."""
    return parse_inc(path)


def require_key(values, name, source):
    """Fetch a required declaration, naming the key and its include.

    The derived build must generate every bank-7 geometry value; a missing
    key is a hard error, never a silent fall back to a default."""
    if name not in values:
        raise MapError(
            "%s is missing from %s; the derived build must generate it"
            % (name, source if source else "the memory include"))


# Nucleus residency classification (config/residency.json).  One entry per
# kernel object; mapcheck fails the build when a fixed-required object is
# compiled into the cold areas, and the derive step uses the tiers to
# split the fixed-core ledger into `required` and `overflow`.
RESIDENCY_TIERS = ("fixed-required", "cold")

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
    if data.get("format") != "kos-residency/1":
        raise MapError("%s is not a kos-residency/1 file" % path)
    tiers = {}
    for entry in data.get("objects", []):
        name = entry.get("object")
        tier = entry.get("tier")
        if not name or tier not in RESIDENCY_TIERS:
            raise MapError("%s: bad residency entry %r" % (path, entry))
        if name in tiers:
            raise MapError("%s: %s is classified twice" % (path, name))
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
    """Residency enforcement (architecture 4.2).

    Fails when the classification does not cover the production object
    list exactly (objects only acceptance images link are exempt through
    `test_link`), when a fixed-required object is compiled into the cold
    areas (a fixed-required symbol at or above 6000h), or when a
    classified object's linked areas disagree with its placement.

    `cold_extra` names objects that this image compiles into the cold
    areas on top of the residency file's `cold_half`: an acceptance image
    whose target-test objects must consume the cold half's space rather
    than the fixed core's (the classification has two tiers,
    fixed-required and cold, and every object is placed).

    `test_link` objects are linked only by acceptance images, so their
    placement is not named by `cold_half` (production's cold set): a
    test-linked `cold` object is expected in the cold areas by its tier."""
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
    4.2): the fixed areas (`_CODE`/`_HOME`/`_INITIALIZER`/`_GSINIT`/
    `_GSFINAL`, `_DATA`/`_INITIALIZED`) must link inside 2000h-5FFFh, and
    the cold areas (`_WINDOW`, `_WINDOW_DATA`) inside 6000h-DFFFh.  The
    generated include carries both data bases, the
    fixed-core ledger fields and the stack reservations."""
    global PAGE_ZERO_END, RESET_TOP, BANK7_BASE
    global BANK7_RUNTIME_END, BANK7_TOP, COMMON_BASE, COMMON_CODE_END
    global STACK_BASE, WINDOW_BASE, WINDOW_DATA_BASE
    global DRIVER_STACK_BASE, DRIVER_STACK_TOP, DRIVER_STACK_SIZE
    global IRQ_ISR_STACK_BASE, IRQ_ISR_STACK_TOP, IRQ_ISR_STACK_SIZE
    global GATE_STACK_SIZE, OWNED_AREAS
    global BANK7_DATA_ALIGN, DATA_BASE
    global FIXED_CODE_END, FIXED_REQUIRED_BYTES, FIXED_OVERFLOW_BYTES
    global FIXED_STACKS_BYTES, FIXED_ROOM_BYTES
    global ROMFS_STAGING_SIZE, ROM_ACCESS_BASE, ROM_ACCESS_SIZE
    global COMMON_CEILING
    PAGE_ZERO_END = values["KOS_PAGE_ZERO_END"]
    RESET_TOP = values["KOS_RESET_TOP"]
    BANK7_BASE = values["KOS_BANK7_BASE"]
    COMMON_BASE = values["KOS_COMMON_BASE"]
    STACK_BASE = values["KOS_COMMON_STACK_BASE"]
    BANK7_RUNTIME_END = values["KOS_BANK7_RUNTIME_END"]
    BANK7_TOP = BANK7_RUNTIME_END
    COMMON_CODE_END = values["KOS_COMMON_CODE_END"]
    # The revision-13 split, the two stack reservations and the two-data-base
    # geometry are build-time architecture constants; the generated include
    # always carries them.
    require_key(values, "KOS_REV13_WINDOW_BASE", source)
    require_key(values, "KOS_DRIVER_STACK_BASE", source)
    require_key(values, "KOS_DRIVER_STACK_TOP", source)
    require_key(values, "KOS_DRIVER_STACK_SIZE", source)
    WINDOW_BASE = values["KOS_REV13_WINDOW_BASE"]
    DRIVER_STACK_BASE = values["KOS_DRIVER_STACK_BASE"]
    DRIVER_STACK_TOP = values["KOS_DRIVER_STACK_TOP"]
    DRIVER_STACK_SIZE = values["KOS_DRIVER_STACK_SIZE"]
    if WINDOW_BASE != REV13_WINDOW_SPLIT:
        raise MapError("the mode-11 window base is %04Xh, expected %04Xh"
                       % (WINDOW_BASE, REV13_WINDOW_SPLIT))
    if not (BANK7_BASE < DRIVER_STACK_BASE <= DRIVER_STACK_TOP
            <= WINDOW_BASE <= BANK7_RUNTIME_END):
        raise MapError("the fixed-core stacks %04Xh-%04Xh do not sit below "
                       "the window base %04Xh"
                       % (DRIVER_STACK_BASE, DRIVER_STACK_TOP,
                          WINDOW_BASE))
    # The common ceiling is a build-time architecture constant; the
    # generated include always carries it.
    require_key(values, "KOS_COMMON_CEILING", source)
    COMMON_CEILING = values["KOS_COMMON_CEILING"]
    IRQ_ISR_STACK_BASE = values["KOS_IRQ_ISR_STACK_BASE"]
    IRQ_ISR_STACK_TOP = values["KOS_IRQ_ISR_STACK_TOP"]
    IRQ_ISR_STACK_SIZE = values["KOS_IRQ_ISR_STACK_SIZE"]
    GATE_STACK_SIZE = values["KOS_GATE_STACK_SIZE"]
    # The bank-7 geometry (fixed data base, cold data base) is derived per
    # image and carried by the generated memory include.  It is required:
    # a missing key must fail loudly rather than validate against a
    # stale boundary.
    require_key(values, "KOS_WINDOW_DATA_BASE", source)
    require_key(values, "KOS_DATA_BASE", source)
    WINDOW_DATA_BASE = values["KOS_WINDOW_DATA_BASE"]
    BANK7_DATA_ALIGN = values["KOS_BANK7_DATA_ALIGN"]
    if not (WINDOW_BASE <= WINDOW_DATA_BASE <= BANK7_RUNTIME_END):
        raise MapError("window data base %04Xh is outside %04Xh-%04Xh"
                       % (WINDOW_DATA_BASE, WINDOW_BASE,
                          BANK7_RUNTIME_END))
    # The fixed-core ledger fields travel with the geometry so both ledger
    # printers (tools/pack_image.py, tools/modpack.py) show one truth.
    for name in ("KOS_FIXED_CODE_END", "KOS_FIXED_REQUIRED_BYTES",
                 "KOS_FIXED_OVERFLOW_BYTES", "KOS_FIXED_STACKS_BYTES",
                 "KOS_FIXED_ROOM_BYTES"):
        require_key(values, name, source)
    # Fixed-core ledger fields (the derived include carries them; a
    # synthetic map carries no per-object attribution and reports an
    # empty overflow).
    FIXED_CODE_END = values["KOS_FIXED_CODE_END"]
    FIXED_REQUIRED_BYTES = values["KOS_FIXED_REQUIRED_BYTES"]
    FIXED_OVERFLOW_BYTES = values["KOS_FIXED_OVERFLOW_BYTES"]
    FIXED_STACKS_BYTES = values["KOS_FIXED_STACKS_BYTES"]
    FIXED_ROOM_BYTES = values["KOS_FIXED_ROOM_BYTES"]
    # The fixed core's data starts at the derived base; the cold half's
    # data starts at the derived window data base.
    DATA_BASE = values["KOS_DATA_BASE"]
    if not (BANK7_BASE < DATA_BASE <= DRIVER_STACK_BASE):
        raise MapError("fixed data base %04Xh is outside the fixed core"
                       % DATA_BASE)
    ROMFS_STAGING_SIZE = values["KOS_ROMFS_STAGING_SIZE"]
    ROM_ACCESS_BASE = values["KOS_ROM_ACCESS_BASE"]
    ROM_ACCESS_SIZE = values["KOS_ROM_ACCESS_SIZE"]
    OWNED_AREAS = {
        "_CODE": (BANK7_BASE, WINDOW_BASE),
        "_HOME": (BANK7_BASE, WINDOW_BASE),
        "_INITIALIZER": (BANK7_BASE, WINDOW_BASE),
        "_GSINIT": (BANK7_BASE, WINDOW_BASE),
        "_GSFINAL": (BANK7_BASE, WINDOW_BASE),
        "_DATA": (DATA_BASE, DRIVER_STACK_BASE),
        "_INITIALIZED": (DATA_BASE, DRIVER_STACK_BASE),
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


def derive_geometry(map_text, values,
                    fixed_required_bytes=None, fixed_overflow_bytes=None):
    """Compute the derived two-region bank-7 geometry from a probe map.

    Fixed core (2000h-5FFFh): `fixed_code_end` is the exclusive end of the
    fixed loadable areas (`_CODE`/`_HOME`/`_INITIALIZER`/`_GSINIT`/
    `_GSFINAL`); the fixed runtime data base is
    `align_down(driver_stack_base - data_size, 512)`.

    Cold half (6000h-DFFFh): `window_code_end` is the exclusive end of
    `_WINDOW` and the window data base is
    `align_up(window_code_end, 512)` - code and data sit back to back.
    Raises MapError when either region's content does not fit."""
    areas, symbols = parse_map(map_text)
    granule = values["KOS_BANK7_DATA_ALIGN"]
    fixed_top = values["KOS_DRIVER_STACK_BASE"]
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
    window_code_end = loadable_end(areas, symbols, ("_WINDOW",))
    window_data_size = area_sizes(areas, symbols,
                                  ("_WINDOW_DATA",))["_WINDOW_DATA"]
    top = values["KOS_BANK7_RUNTIME_END"]
    window_data_base = align_up(window_code_end, granule)
    if window_data_base > top or \
            window_data_size > top - window_data_base:
        raise MapError(
            "cold code ends at %04Xh and the %d data bytes do not fit "
            "%04Xh-%04Xh" % (window_code_end, window_data_size,
                             REV13_WINDOW_SPLIT, top - 1))
    common = area_sizes(areas, symbols, COMMON_AREAS)
    stacks = values["KOS_DRIVER_STACK_SIZE"] + values["KOS_IRQ_ISR_STACK_SIZE"]
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
    """Bootstrap include for the probe link: no boundary is placed.

    The probe image only has to link; the derive step measures its loadable
    area ends.  Placing nothing means kernel growth can never collide with
    a placeholder boundary."""
    return ("; Generated by tools/mapcheck.py geometry -- do not edit.\n"
            "; Probe-link geometry: the probe image places no boundary; the\n"
            "; derive step measures the loadable areas in its map.\n"
            "KOS_KERNEL_CODE_END = 0x0000\n")


def format_geometry_inc(geometry):
    lines = [
        "; Generated by tools/mapcheck.py geometry -- do not edit.",
        "; Bank-7 geometry for kernel/memory/bank7_end.asm.",
        "KOS_KERNEL_CODE_END = 0x%04X" % geometry["kernel_code_end"],
    ]
    return "\n".join(lines) + "\n"


def format_memory_variant(memory_inc_text, geometry):
    """Full memory.inc copy with the derived geometry declarations."""
    replacements = {
        "KOS_KERNEL_CODE_END": "0x%04X" % geometry["kernel_code_end"],
        "KOS_FIXED_CODE_END": "0x%04X" % geometry["fixed_code_end"],
        "KOS_DATA_BASE": "0x%04X" % geometry["data_base"],
        "KOS_DATA_SIZE": str(geometry["data_size"]),
        "KOS_FIXED_REQUIRED_BYTES": str(geometry["fixed_required_bytes"]),
        "KOS_FIXED_OVERFLOW_BYTES": str(geometry["fixed_overflow_bytes"]),
        "KOS_FIXED_STACKS_BYTES": str(geometry["fixed_stacks_bytes"]),
        "KOS_FIXED_ROOM_BYTES": str(geometry["fixed_room_bytes"]),
        "KOS_COMMON_CODE_SIZE": str(geometry["common_code_size"]),
        "KOS_COMMON_DATA_SIZE": str(geometry["common_data_size"]),
        "KOS_REV13_WINDOW_BASE": "0x%04X" % geometry["window_split"],
        "KOS_WINDOW_CODE_END": "0x%04X" % geometry["window_code_end"],
        "KOS_WINDOW_DATA_BASE": "0x%04X" % geometry["window_data_base"],
        "KOS_WINDOW_DATA_SIZE": str(geometry["window_data_size"]),
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
    IRQ_VECTOR_BASE = values.get("KOS_IRQ_VECTOR_BASE", IRQ_VECTOR_BASE)
    IRQ_VECTOR_SIZE = values.get("KOS_IRQ_VECTOR_SIZE", IRQ_VECTOR_SIZE)
    IRQ_STATE_BASE = values.get("KOS_IRQ_STATE_BASE", IRQ_STATE_BASE)
    IRQ_STACK_BASE = values.get("KOS_IRQ_STACK_BASE", IRQ_STACK_BASE)
    IRQ_STACK_TOP = values.get("KOS_IRQ_STACK_TOP", IRQ_STACK_TOP)
    IRQ_STACK_SIZE = values.get("KOS_IRQ_STACK_SIZE", IRQ_STACK_SIZE)


# Owned areas and their allowed placement range.  Fixed-core sections must
# end below the window; the cold areas live in the cold half, with the
# cold runtime data straight after the linked cold code.
OWNED_AREAS = {
    "_CODE": (BANK7_BASE, WINDOW_BASE),
    "_HOME": (BANK7_BASE, WINDOW_BASE),
    "_INITIALIZER": (BANK7_BASE, WINDOW_BASE),
    "_GSINIT": (BANK7_BASE, WINDOW_BASE),
    "_GSFINAL": (BANK7_BASE, WINDOW_BASE),
    "_DATA": (DATA_BASE, DRIVER_STACK_BASE),
    "_INITIALIZED": (DATA_BASE, DRIVER_STACK_BASE),
    "_WINDOW": (WINDOW_BASE, WINDOW_DATA_BASE),
    "_WINDOW_DATA": (WINDOW_DATA_BASE, BANK7_RUNTIME_END),
    "_COMMON": (COMMON_BASE, COMMON_CODE_END),
    "_COMMON_DATA": (COMMON_BASE, COMMON_CODE_END),
}

# Every routine that changes the memory mapping, or is needed immediately
# after a mapping change, must be common-resident.
COMMON_ROUTINES = (
    "_boot_common_entry",
    "__kos_crt0_init",
    "__kos_mode_fault",
    "kos_enter_os_mode",
    "kos_leave_os_mode",
    "kos_os_continuation",
    "kos_syscall_entry",
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
    # Process launch/resume/trap and the application-bank copy gate
    # all switch the memory mapping; the page-zero vector is copied into
    # process banks from common data.
    "_kos_process_launch",
    "_kos_process_resume",
    "kos_process_return_trap",
    "_kos_process_page_zero_vector",
    "_kos_bank_copy",
)

# Common-resident only if the image links them.  Same reason as above, but
# not every image links these objects, so their absence is not an error:
# an image that does link them must place them in common memory, never in
# the fixed core or the cold half.
COMMON_ONLY_SYMBOLS = (
    # The path-B aperture copy runs with mode
    # 10 and page 1 in force, where bank 7 is not mapped at all, so the
    # stub, its request block and its interrupt-push reservation must all
    # live in common memory.
    "_ipc_ap_copy",
    "_ipc_ap_req",
    "_ipc_ap_saved_latch",
    "_ipc_ap_saved_sp",
    "_ipc_ap_stack",
    "_ipc_ap_stack_top",
    # The driver boot gate calls a bank-6 boot segment with
    # mode 10 selected over bank 6, where bank 7 is unmapped; its code,
    # transition state and the loader's upcall request block must live in
    # common memory.
    "_kos_driver_run_boot",
    "kos_driver_boot_entry",
    "kos_driver_boot_saved_latch",
    "kos_driver_boot_saved_iff",
    "_kos_driver_upcall_block",
    # The device-event table and the delivery state are read from the
    # delivery (application latch) and from driver-interrupt context
    # (driver window) alike, so they are common memory or nothing.
    "_kos_event_table",
    "_kos_event_in_handler",
    "_kos_event_frame",
    "_kos_event_frame_latch",
    "_kos_event_row",
    "_kos_event_latch",
    # Device-event delivery: the two return-path checks and the frame
    # primitives run with the application's own latch in force (mode 10 over
    # its bank), where bank 7 is mapped only by accident.
    "_kos_event_irq_exit",
    "_kos_event_syscall_exit",
    "_kos_event_post_bank",
    "_kos_event_return_frame",
    "_kos_event_pick",
    "_kos_event_enter",
    "_kos_event_unwind",
)

# Bank-7 internal routines (visible only while mode 11 is active).
BANK7_ROUTINES = ("kos_syscall_dispatch", "_irq_dispatch")

# The entries of the driver bootstrap interface (`struct driver_api`,
# drivers/include/driver.h; the table is kernel/ipc/ipc.c's `ipc_api`).
# A resident driver calls every one of them from its own window - mode 10
# over bank 6 - where bank 7 shows only the fixed core 2000h-5FFFh and the
# common region E000h-FCFFh.  An entry that links into the 6000h window is
# not visible there at all, so the driver's call would execute whatever
# the window shows.  The kernel exports the entries as globals so their
# addresses travel in the map; an image without the IPC substrate links
# none of them and skips the check.
DRIVER_API_ENTRIES = (
    "_api_call",
    "_api_copy_in",
    "_api_copy_out",
    "_api_move",
    "_api_bulk_read",
    "_api_bulk_write",
    "_api_console_putc",
    "_api_console_poll",
    "_api_console_getc",
    "_api_irq_claim",
    "_api_irq_release",
    "_api_event_post",
)

# The nucleus has no device registry: discovery is the host-built `/dev`
# publication table plus the driver API, and the drivers are bank-6
# images.  Any symbol with these prefixes must at least stay in bank 7,
# never in common memory.
REGISTRY_BANK7_PREFIXES = (
    "_device_registry_",
    "_device_table",
    "_console_",
    "_m3_",
)

# Production images must not link any acceptance artifact: static driver
# symbols, target-test runners/results and their generic seam hooks, CTC0
# acceptance helpers, an IRQ vector lookup, fixture catalogs, embedded boot
# catalogs (the boot reads the store), or test SIO module entries.
# Enabled with --production (the Makefile passes it for non-acceptance
# images).
PRODUCTION_FORBIDDEN = (
    "_driver_init_static",
    "_boot_drivers",
    "_driver_static_",
    "_m3_",
    "_m4_selftest",
    "_m4_fixture",
    "_mod_fixture",
    "_z4_fixture",
    "_z4_module_boot",
    "_mod_catalog",
    "_target_test",
    "_kos_run_",
    "_kos_target_test_",
    "_kos_ctc0_",
    "_platform_ctc0_",
    "_platform_irq_vector",
    "_m7_",
    "_m8",
    "_ace_",
    "_sio_module_irq",
    "_sio_irq_module",
    "_sio_fail_module",
)

# The SIO driver exists only as a bank-6 image.  Any linked SIO symbol
# means a static copy is in the kernel image.
STATIC_SIO_PREFIXES = ("_sio_", "_sio0b_", "_sio_driver")

# The IOC driver is a bank-6 image too: any linked `_ioc_` symbol means a
# static copy is in the kernel image.
STATIC_IOC_PREFIXES = ("_ioc_",)

# The HID driver is a bank-6 image as well; any linked `_hid_` symbol
# means a static copy is in the kernel image.
STATIC_HID_PREFIXES = ("_hid_",)

# The V9958 video driver is a bank-6 image too; any linked `_v9958_`
# symbol means a static copy is in the kernel image.
STATIC_V9958_PREFIXES = ("_v9958_",)

# The nucleus has no relocating module loader: drivers are host-linked
# bank-6 images.  Any symbol with these prefixes must stay in bank 7.
LOADER_BANK7_ROUTINES = ()

LOADER_BANK7_PREFIXES = ("_module_", "_mod_", "_z4_", "_m4_",
                    "_irq_registry_")

# ROMFS reader routines: bank-7 code only.  `_romfs_lookup_ci` is the
# single ASCII-folded lookup (architecture 4.6), so every image links this
# one name.
ROMFS_BANK7_ROUTINES = (
    "_romfs_init",
    "_romfs_lookup_ci",
    "_romfs_read",
    "_romfs_resource_size",
    "_romfs_resource_read",
)

ROMFS_BANK7_PREFIXES = ("_romfs_",)

# The directory accessor the /system provider's enumeration needs
# (kernel/romfs/romfs.c).  Every image links it.
ROMFS_ENUM_ROUTINES = (
    "_romfs_entry_at",
)

# The VFS: the router, the providers, the handle pool, the path helpers
# and the workspace, all below the ISR stack.
VFS_BANK7_ROUTINES = (
    # The handle pool, the path helpers and the fixed-core workspace the
    # namespace bodies marshal through.
    "_vfs_init",
    "_vfs_phase",
    "_vfs_path_normalize_at",
    "_vfs_handle_alloc",
    "_vfs_handle_find",
    "_vfs_handle_release",
    "_vfs_cwd_for",
    "_vfs_cwd_store",
    "_vfs_process_start",
    "_vfs_process_exit",
    # The root router and its publication table, the `/system` FILE
    # provider and the object syscall bodies.
    "_vfs_ns_init",
    "_vfs_ns_reset",
    "_vfs_ns_install_publication",
    "_vfs_ns_publication_endpoint",
    "_vfs_ns_bind",
    "_vfs_ns_unbind",
    "_vfs_ns_resolve",
    "_vfs_ns_standard_handles",
    "_vfs_ns_enum",
    "_vfs_ns_fs_call",
    "_vfs_ns_fs_release",
    "_vfs_ns_file_bind",
    "_vfs_ns_file_call",
    "_vfs_ns_file_meta",
    "_vfs_ns_file_dispatch",
    "_vfs_ns_dir_check",
    "_kos_object_copy_path",
    "_kos_object_dispatch",
)

VFS_BANK7_PREFIXES = ("_vfs_",)

# Process/EXE code and state: bank 7 only, below the ISR stack (the
# common-resident gates are listed in COMMON_ROUTINES instead).
PROCESS_BANK7_ROUTINES = (
    "_process_exec",
    "_process_prepare",
    "_process_launch",
    "_process_exit",
    "_process_abort",
    "_process_alloc_bank",
    "_process_slot_alloc",
    "_process_slot_release",
    "_exe_validate_header",
)

PROCESS_BANK7_PREFIXES = ("_process_", "_exe_")

# Diagnostic syscall bodies and the shared dispatcher helpers: bank-7
# kernel code only.  They reuse the common copy gate, so they add no
# permanent-common symbol.
DIAG_BANK7_ROUTINES = (
    "_kos_diag_mem_read",
    "_kos_diag_mem_write",
    "_kos_diag_io_in",
    "_kos_diag_io_out",
    "_kos_diag_bank_info",
    "_kos_diag_system_info",
    "_kos_diag_hw_in",
    "_kos_diag_hw_out",
    "_kos_syscall_success",
    "_kos_syscall_error",
    "_kos_syscall_block_ok",
)

DIAG_BANK7_PREFIXES = ("_kos_diag_",)

# Process transition state and the syscall copy staging block (common RAM).
PROCESS_COMMON_STATE = (
    "_kos_user_copy_staging",
    "kos_copy_op",
    "kos_copy_bank",
    "kos_copy_addr",
    "kos_copy_buffer",
    "kos_copy_length",
    "kos_copy_saved_sp",
    "kos_copy_saved_latch",
    "kos_copy_saved_iff",
    "kos_copy_stack",
    "kos_copy_stack_top",
    "kos_proc_cont",
    "kos_proc_ix",
    "kos_proc_latch",
    "kos_proc_iff",
    "kos_proc_index",
    "kos_proc_status",
    "kos_proc_bank",
    "kos_proc_entry",
    "kos_proc_gate_sp",
    "kos_proc_gate_latch",
    "kos_proc_gate_iff",
    "kos_proc_frames",
    "kos_proc_frames_end",
)

USER_COPY_STAGING_SIZE = 64
USER_COPY_STACK_SIZE = 16

# Exec continuation state: KOS_EXEC_MAX_DEPTH levels, with
# KOS_EXEC_MAX_DEPTH - 1 gate-frame save areas of KOS_EXEC_FRAME_SAVE
# bytes (kernel/include/process.h; pinned here so a mismatch fails the
# production map rather than silently truncating a saved frame).
KOS_EXEC_MAX_DEPTH = 4
KOS_EXEC_FRAME_SAVE = 48

# ROM-access transition state: permanent common memory only.  The
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
    "kos_gate_saved_sp",
    "kos_gate_saved_latch",
    "kos_gate_result_hl",
    "kos_gate_result_a",
    "kos_gate_result_c",
    "kos_gate_stack",
    "kos_gate_stack_top",
)

IRQ_STATE = (
    "irq_saved_latch", "irq_saved_sp", "irq_common_sp", "_irq_active_source",
)


class MapError(Exception):
    """The linked map violates the KonaOS memory contract."""


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

    # Nothing may occupy the common stack reservation FE88h-FFFFh.
    for name, (addr, size) in sorted(areas.items()):
        if name in reset_areas:
            continue
        if addr + size > MEM_TOP:
            raise MapError("%s (size %d) exceeds the 16-bit space" % (name, size))
        if addr >= COMMON_BASE and addr + size > STACK_BASE:
            raise MapError(
                "%s at %04Xh (size %d) overlaps the common stack "
                "reservation %04Xh-FFFFh" % (name, addr, size, STACK_BASE)
            )

    # The cold half has no reserved block between the
    # linked cold code and the cold runtime data.  The area ownership
    # table bounds `_WINDOW` at the derived data base and the dedicated
    # checks below pin `_WINDOW`'s end to `__kernel_code_end` and the
    # data base to `align_up` of it.

    # The ROM-access transition block is a fixed reservation.  Only
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

    # The fixed core's stack reservation 5D00h-6000h (driver stack, then
    # ISR stack) is reserved by declaration: only the ABS areas that
    # declare the two stacks may occupy it.
    for name, (addr, size) in sorted(areas.items()):
        if size == 0 or name.startswith(("_IRQ_ISR_STACK",
                                         "_IPC_DRIVER_STACK")):
            continue
        if addr < WINDOW_BASE and addr + size > DRIVER_STACK_BASE:
            raise MapError(
                "%s at %04Xh (size %d) overlaps the fixed-core stack "
                "reservation %04Xh-%04Xh"
                % (name, addr, size, DRIVER_STACK_BASE, WINDOW_BASE - 1)
            )

    # Ordinary bank-7 data stops below the fixed-core stacks, and
    # ordinary common allocation stops below the IM2 page.
    for name in ("_DATA", "_INITIALIZED"):
        if name in areas:
            addr, size = areas[name]
            if addr + size > DRIVER_STACK_BASE:
                raise MapError(
                    "%s overlaps the fixed-core driver stack at %04Xh"
                    % (name, DRIVER_STACK_BASE))
    for name in ("_COMMON", "_COMMON_DATA"):
        if name in areas:
            addr, size = areas[name]
            if addr + size > IRQ_VECTOR_BASE:
                raise MapError("%s overlaps the IM2 reservation at FD00h" % name)

    # The packed common window has an architectural ceiling
    # (architecture 9).
    # Growth past the ceiling fails the build here.
    common_used = sum(areas[name][1] for name in ("_COMMON", "_COMMON_DATA")
                      if name in areas)
    if common_used > COMMON_CEILING:
        raise MapError(
            "packed common memory is %d bytes, over the %d-byte ceiling"
            % (common_used, COMMON_CEILING))


# Symbols that must stay reachable while the window shows another bank:
# the gates, the dispatch bodies and data, the copy primitives and their
# staging, and the upcall shim (driver code an interrupt can dispatch
# into is fixed by construction, in bank 6).  A symbol is acceptable in
# the fixed region (2000h-5FFFh) or in the permanently mapped common window
# (E000h+), never inside the cold half.
REV13_FIXED_SYMBOLS = (
    "irq_common_entry",
    "irq_isr_stack",
    "irq_saved_latch",
    "irq_saved_sp",
    "irq_ctc0_stub",
    "_irq_arch_lock",
    "_irq_arch_unlock",
    "kos_syscall_entry",
    "kos_syscall_dispatch",
    # The VFS workspace's path view.  A bind or exec frame names the
    # caller's path by address and a bank-6 provider dereferences it while
    # its own window is open, so the block must never link where that
    # window replaces it (4.2's buffer rule).
    "_vfs_workspace",
    "_irq_dispatch",
    "_irq_active_source",
    "_boot_common_entry",
    "_kos_bank_copy",
    "_kos_user_copy_staging",
    "_platform_memctl_read",
    "_platform_memctl_write",
    "_kos_upcall",
    # The IPC substrate executes while the window shows the
    # resident driver domain, so its dispatcher, its frame and descriptor
    # tables and its fixed transition primitives stay in the fixed core.
    "_ipc_lookup",
    "_ipc_syscall_call",
    "_ipc_domain_enter",
    "_ipc_domain_set",
    "_ipc_domain_exit",
    "_ipc_copy_run",
    "_ipc_invoke",
    "_ipc_invoke_here",
    # The aperture primitive, its common request block, and
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
# dispatching, so the dispatcher and the boot/idle path only ever run
# with P = 0 and live in the cold half.
REV13_COLD_SYMBOLS = (
    "_kernel_main",
    "_kernel_syscall_dispatch",
    "_kos_ipc_dispatch",
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
                       ("kos_driver_stack", DRIVER_STACK_BASE),
                       ("kos_driver_stack_top", DRIVER_STACK_TOP)):
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

    # Driver bootstrap interface entries: visible from the driver's own
    # window (fixed core or common memory), never in the 6000h window.
    for name in DRIVER_API_ENTRIES:
        if name not in symbols:
            continue
        addr = symbols[name]
        if not (BANK7_BASE <= addr < WINDOW_BASE or
                COMMON_BASE <= addr < COMMON_CODE_END):
            raise MapError(
                "the driver API entry %s at %04Xh is not visible from "
                "driver context: it must sit in the fixed core 2000h-%04Xh "
                "or in common memory %04Xh-%04Xh"
                % (name, addr, WINDOW_BASE - 1, COMMON_BASE,
                   COMMON_CODE_END - 1)
            )

    # Registry-like and console symbols, if any, live in bank 7 (code or
    # runtime data), never in the permanently mapped common region (see
    # REGISTRY_BANK7_PREFIXES).
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(REGISTRY_BANK7_PREFIXES):
            if not (BANK7_BASE <= addr < BANK7_TOP):
                raise MapError(
                    "registry/console symbol %s at %04Xh is not bank-7 "
                    "resident" % (name, addr)
                )

    # The drivers are bank-6 images; the nucleus links no driver symbol
    # at all.
    for name, addr in symbols.items():
        if addr == 0:
            continue
        for prefixes, what in (
                (STATIC_SIO_PREFIXES, "SIO"),
                (STATIC_IOC_PREFIXES, "IOC"),
                (STATIC_HID_PREFIXES, "HID"),
                (STATIC_V9958_PREFIXES, "V9958")):
            if name.startswith(prefixes):
                raise MapError(
                    "static %s symbol %s at %04Xh must not be linked; the "
                    "driver is a bank-6 driver image"
                    % (what, name, addr))
    # Loader-like code and catalog/fixture data, if any, stay in the
    # bank-7 window below the ISR stack.
    for name in LOADER_BANK7_ROUTINES:
        if name not in symbols:
            raise MapError("map has no loader symbol %s" % name)
        addr = symbols[name]
        if not (BANK7_BASE <= addr < BANK7_TOP):
            raise MapError(
                "%s at %04Xh is not in the bank-7 region 2000h-DEFFh"
                % (name, addr)
            )
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(LOADER_BANK7_PREFIXES):
            if not (BANK7_BASE <= addr < BANK7_TOP):
                raise MapError(
                    "module symbol %s at %04Xh is not bank-7 resident"
                    % (name, addr)
                )

    # ROMFS: the allocation-free reader is bank-7 code; the ROM-access
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

    # ROM-access transition block: the trampoline lands exactly two
    # bytes before the mirrored stub at KOS_ROM_ACCESS_BASE, the stub is
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

    # The VFS core and the /system provider are bank-7 residents below
    # the ISR stack.
    for name in VFS_BANK7_ROUTINES:
        if name not in symbols:
            raise MapError("map has no VFS symbol %s" % name)
        addr = symbols[name]
        if not (BANK7_BASE <= addr < BANK7_TOP):
            raise MapError(
                "%s at %04Xh is not in the bank-7 region 2000h-DEFFh"
                % (name, addr))
    for name in ROMFS_ENUM_ROUTINES:
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

    # Process/EXE code and the process table stay bank-7 resident.
    for name in PROCESS_BANK7_ROUTINES:
        if name not in symbols:
            raise MapError("map has no process symbol %s" % name)
        addr = symbols[name]
        if not (BANK7_BASE <= addr < BANK7_TOP):
            raise MapError(
                "%s at %04Xh is not in the bank-7 region 2000h-DEFFh"
                % (name, addr))
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(PROCESS_BANK7_PREFIXES):
            if not (BANK7_BASE <= addr < BANK7_TOP):
                raise MapError(
                    "process symbol %s at %04Xh is not bank-7 resident"
                    % (name, addr))

    # Diagnostic bodies and shared dispatcher helpers are bank-7
    # kernel code; they add no permanent-common state.
    for name in DIAG_BANK7_ROUTINES:
        if name not in symbols:
            raise MapError("map has no diagnostic symbol %s" % name)
        addr = symbols[name]
        if not (BANK7_BASE <= addr < BANK7_TOP):
            raise MapError(
                "%s at %04Xh is not in the bank-7 region 2000h-DEFFh"
                % (name, addr))
    for name, addr in symbols.items():
        if addr != 0 and name.startswith(DIAG_BANK7_PREFIXES):
            if not (BANK7_BASE <= addr < BANK7_TOP):
                raise MapError(
                    "diagnostic symbol %s at %04Xh is not bank-7 "
                    "resident" % (name, addr))

    # The copy staging block and process transition state are common;
    # the staging size and copy stack sizes are part of the contract.
    for name in PROCESS_COMMON_STATE:
        if name not in symbols:
            raise MapError("map has no process common-state symbol %s" % name)
        addr = symbols[name]
        if not (COMMON_BASE <= addr < STACK_BASE):
            raise MapError(
                "%s at %04Xh is not in the common region E000h-FEFFh"
                % (name, addr))
    staging_delta = (symbols["kos_copy_op"]
                     - symbols["_kos_user_copy_staging"])
    if staging_delta != USER_COPY_STAGING_SIZE:
        raise MapError("user copy staging is %d bytes, expected %d"
                       % (staging_delta, USER_COPY_STAGING_SIZE))
    stack_delta = (symbols["kos_copy_stack_top"]
                   - symbols["kos_copy_stack"])
    if stack_delta != USER_COPY_STACK_SIZE:
        raise MapError("user copy stack is %d bytes, expected %d"
                       % (stack_delta, USER_COPY_STACK_SIZE))

    # The SYS_EXEC caller path is staged in the VFS workspace's path view,
    # exactly as `bind` stages its own.  The buffer rule it must satisfy
    # (a bank-6 provider dereferences that address while its own window
    # is open) is enforced symbol by symbol in check_rev13_window()
    # through REV13_FIXED_SYMBOLS (`_vfs_workspace`).

    # The continuation stack must be sized for the pinned
    # depth and frame save, not truncated by a stale assembly .ds.
    frames_delta = (symbols["kos_proc_frames_end"]
                    - symbols["kos_proc_frames"])
    expected_frames = KOS_EXEC_FRAME_SAVE * (KOS_EXEC_MAX_DEPTH - 1)
    if frames_delta != expected_frames:
        raise MapError("exec frame saves are %d bytes, expected %d "
                       "(%d x %d)" % (frames_delta, expected_frames,
                                      KOS_EXEC_MAX_DEPTH - 1,
                                      KOS_EXEC_FRAME_SAVE))

    # The cold half has no reserved block; the linked
    # cold code ends at __kernel_code_end and the cold runtime data base
    # is align_up of it.
    fixed_end = loadable_end(areas, symbols, FIXED_LOADABLE_AREAS)
    cold_end = loadable_end(areas, symbols, ("_WINDOW",))
    if fixed_end > DATA_BASE:
        raise MapError(
            "linked fixed code ends at %04Xh, inside the fixed runtime data "
            "region at %04Xh" % (fixed_end, DATA_BASE))
    # Derived geometry: __kernel_code_end must be the exclusive end of the
    # linked cold code and the cold data base must follow it, aligned.
    # Synthetic host maps carry no __kernel_code_end and skip the
    # derived-only relation checks.
    kernel_code_end = symbols.get("__kernel_code_end")
    if kernel_code_end is not None:
        if kernel_code_end != cold_end:
            raise MapError(
                "__kernel_code_end is %04Xh, linked cold code ends at "
                "%04Xh" % (kernel_code_end, cold_end))
        expected_base = align_up(kernel_code_end, BANK7_DATA_ALIGN)
        if WINDOW_DATA_BASE != expected_base:
            raise MapError(
                "the cold data base is %04Xh, expected align_up(%04Xh, "
                "%04Xh) = %04Xh" % (WINDOW_DATA_BASE, kernel_code_end,
                                    BANK7_DATA_ALIGN, expected_base))
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
    # owned-area contract); the cold data base follows the cold code.
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
        if end > DRIVER_STACK_BASE:
            raise MapError("%s ends at %04Xh, overlapping the fixed-core "
                           "driver stack reservation at %04Xh"
                           % (label, end, DRIVER_STACK_BASE))
    if WINDOW_DATA_BASE + symbols.get("l__WINDOW_DATA", 0) > BANK7_RUNTIME_END:
        raise MapError("cold runtime data leaves the cold half at %04Xh"
                       % (WINDOW_DATA_BASE +
                          symbols.get("l__WINDOW_DATA", 0)))

    # Transition state block: common RAM, gate stack of the exact size.
    for name in COMMON_STATE:
        if name not in symbols:
            raise MapError("map has no state symbol %s" % name)
        addr = symbols[name]
        if not (COMMON_BASE <= addr < STACK_BASE):
            raise MapError(
                "%s at %04Xh is not in the common region E000h-FEFFh" % (name, addr)
            )
    stack_size = symbols["kos_gate_stack_top"] - symbols["kos_gate_stack"]
    if stack_size != GATE_STACK_SIZE:
        raise MapError("gate stack is %d bytes, expected %d" % (stack_size, GATE_STACK_SIZE))

    # Fixed absolute IRQ reservations.  ABS area start symbols are not
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
    if symbols.get("_kos_rst30_vector") != RESET_TOP:
        raise MapError(
            "_kos_rst30_vector is at %04Xh, expected %04Xh"
            % (symbols.get("_kos_rst30_vector", -1), RESET_TOP)
        )
    vector_size = symbols.get("_kos_rst30_end", 0) - RESET_TOP
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
            # Bootstrap geometry for the probe link: place no boundary at
            # all.  The probe image just has to link so the derive step
            # can measure its area ends; a fixed placeholder here could
            # otherwise collide with kernel growth.
            if len(argv) not in (3, 5):
                raise MapError("usage: mapcheck.py geometry probe "
                               "<memory.inc> <out.inc>")
            parse_inc(argv[1])
            with open(argv[2], "w") as handle:
                handle.write(format_probe_geometry_inc())
            print("mapcheck: probe geometry (no boundary placed) -> %s"
                  % argv[2])
            return 0
        if mode == "derive":
            residency_path = None
            args = list(argv[1:])
            for flag in ("--residency",):
                if flag in args:
                    at = args.index(flag)
                    value = args[at + 1]
                    del args[at:at + 2]
                    residency_path = value
            if len(args) not in (3, 4):
                raise MapError(
                    "usage: mapcheck.py geometry derive <probe.map> "
                    "<memory.inc> <out.inc> [<memory-variant.inc>] "
                    "[--residency FILE]")
            probe_path, memory_path, out_path = args[0], args[1], args[2]
            values = parse_inc(memory_path)
            fixed_required = fixed_overflow = None
            if residency_path is not None:
                tiers, _cold_half, _test_link = load_residency(residency_path)
                fixed_required, fixed_overflow = fixed_core_bytes(
                    os.path.dirname(os.path.abspath(probe_path)), tiers)
            with open(probe_path) as handle:
                geometry = derive_geometry(handle.read(), values,
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
            print("mapcheck: derived cold half: code end %04Xh, data %d "
                  "bytes at %04Xh -> %s"
                  % (geometry["window_code_end"],
                     geometry["window_data_size"],
                     geometry["window_data_base"], out_path))
            return 0
        raise MapError("unknown geometry mode %r" % mode)
    except MapError as exc:
        print("mapcheck: %s" % exc, file=sys.stderr)
        return 1


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
    residency_path = None
    cold_extra = ()
    for flag in ("--residency", "--cold-extra"):
        if flag in argv:
            at = argv.index(flag)
            value = argv[at + 1]
            del argv[at:at + 2]
            if flag == "--residency":
                residency_path = value
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
    except MapError as exc:
        print("mapcheck: %s" % exc, file=sys.stderr)
        return 1
    print("mapcheck: OK (%d areas, %d symbols)%s"
          % (len(areas), len(symbols),
             " [production]" if production else ""))
    print("mapcheck: fixed core 2000h-%04Xh end %04Xh (%d required + %d "
          "overflow + %d stacks, %d room) | cold half %04Xh-%04Xh end "
          "%04Xh | data %04Xh/%04Xh"
          % (WINDOW_BASE - 1, FIXED_CODE_END, FIXED_REQUIRED_BYTES,
             FIXED_OVERFLOW_BYTES, FIXED_STACKS_BYTES, FIXED_ROOM_BYTES,
             WINDOW_BASE, BANK7_TOP - 1,
             symbols.get("__kernel_code_end",
                         loadable_end(areas, symbols, ("_WINDOW",))),
             DATA_BASE, WINDOW_DATA_BASE))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
