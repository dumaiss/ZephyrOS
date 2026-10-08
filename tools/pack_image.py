#!/usr/bin/env python3
"""ZephyrOS ROM image packer and layout validator.

Builds the 512 KiB programmed image from the linked Intel HEX output:

    page 0  (64 KiB, copied into SRAM bank 0 by the reset copier)
    page 1  (64 KiB, copied into SRAM bank 7 by the reset copier)
    pages 2-7  (384 KiB, read-only ROMFS recovery store)

Region contract (see docs/architecture.md sections 36.2/36.5, docs/boot.md):

    0000h-002Fh   reset copier (page zero, both boot pages, byte-identical)
    0030h-00FFh   page-zero reservation (must stay empty)
    0100h-1FFFh   page-1 caller-window gap (must stay empty)
    2000h-5FFFh   bank-7 fixed core: linked code up to the fixed code end,
                  the fixed runtime data at the derived data base (never
                  ROM-loaded), then the service stack (5D00h-5F00h) and the
                  ISR stack (5F00h-6000h); the code is ROM-loaded
    6000h-DFFFh   bank-7 cold half (the mode-11 window): linked cold code
                  up to the cold code end, the derived driver arena
                  (load-time only, no ROM payload) and the cold runtime
                  data at the derived window data base (never ROM-loaded)
    E000h-FEFFh   permanent common code/data (physical bank 0)
    FF00h-FFFFh   common boot/runtime stack (must stay empty in the image)

Pages 2-7 carry the logical ROMFS stream (build/recovery.romfs, produced
from recovery/manifest.json by tools/romfspack.py): logical byte L is
placed at offset L % C000h of page (ZOS_ROMFS_FIRST_PAGE + L / C000h);
the C000h-FFFFh half of every recovery page and every unused byte are
FFh, except the fixed ROM-access stub mirror at ZOS_ROM_ACCESS_BASE
(linked in common at the same address and copied into every recovery
page here, so instruction fetch survives the mode-00 window).  The boot
pages 0/1 never receive recovery content.

Any linked byte outside its assigned region is a build error, as are map
symbols whose addresses fall outside their expected region.

Usage:
    pack_image.py [--production] <zephyros.ihx> <zephyros.map> <build_dir>
                  [suffix] [build/geometry/memory.inc]

`--production` declares the production placement, which turns on the
fixed-core latch audit (see verify_no_banked_latch_writes); the evaluation
images pass no flag and keep their own placement.

The optional memory.inc is the generated bank-7 geometry include; it must
declare the derived arena start/end, granule count and data base (a missing
key is a hard error, never a fall back to a retired default).  With no
include the synthetic host defaults are used.  When <build_dir> is given,
<build_dir>/recovery.romfs must exist and is placed into pages 2-7; when it
is None (host unit tests) an all-FF recovery store is emitted unless `romfs`
is passed explicitly.
"""

import json
import re
import sys
import hashlib
from pathlib import Path

FILL = 0xFF
PAGE_SIZE = 0x10000
PAGE_COUNT = 8
IMAGE_SIZE = 0x80000

# ROMFS recovery-store geometry (defaults; overridden from memory.inc).
ROMFS_FIRST_PAGE = 2
ROMFS_PAGE_COUNT = 6
ROMFS_WINDOW_SIZE = 0xC000
ROMFS_CAPACITY = 0x48000
ROMFS_HEADER_SIZE = 16  # tools/romfspack.py owns full content validation

# ROM-access transition stub (kernel/romfs/rom_access.asm): linked in
# common at ZOS_ROM_ACCESS_BASE and mirrored into every recovery page so
# the mode-00 window can execute it from ROM.  Pattern-checked below.
ROM_ACCESS_BASE = 0xEF00
ROM_ACCESS_SIZE = 8

RESET_BASE = 0x0000
RESET_TOP = 0x0030          # M1 installs RST 30h at 0030h
PAGE_ZERO_END = 0x0100
# Revision-13 mode-11 bank-7 split (architecture 36.2, 36.5): the fixed
# core is 2000h-5FFFh, the cold half (the window) is 6000h-DFFFh.  The
# fixed core's runtime data stops below the reserved service stack, which
# sits below the ISR stack at the top of the fixed core.
BANK7_BASE = 0x2000
WINDOW_BASE = 0x6000
BANK7_RUNTIME_END = 0xE000
SERVICE_STACK_BASE = 0x5D00
SERVICE_STACK_TOP = 0x5F00
SERVICE_STACK_SIZE = 0x0200
IRQ_ISR_STACK_BASE = 0x5F00
IRQ_ISR_STACK_TOP = 0x6000
IRQ_ISR_STACK_SIZE = 0x0100
# Bank-6 service tiers (architecture 36.6): nothing is placed there until
# stage 4, so every image reports zero bytes against these capacities.
SERVICE_HOT_CAPACITY = 0x8000      # 6000h-DFFFh of bank 6
SERVICE_BOOT_CAPACITY = 0x6000     # 0000h-5FFFh of bank 6
# Host-synthetic geometry defaults.  A real build always passes its
# generated build/geometry/memory.inc, which must carry every derived
# value; these literals only seed the synthetic path where no include is
# supplied (host unit tests and hand-held validation maps).
ARENA_BASE = 0x6200
ARENA_END = 0x6A00
DATA_BASE = 0x5A00
GRANULE_SIZE = 0x0200
GRANULE_COUNT = ((ARENA_END - ARENA_BASE) // GRANULE_SIZE)
WINDOW_DATA_BASE = 0xD800
# Fixed-core ledger fields (the generated include carries them; the
# synthetic defaults describe an empty overflow).
FIXED_CODE_END = 0x5200
FIXED_REQUIRED_BYTES = FIXED_CODE_END - BANK7_BASE
FIXED_OVERFLOW_BYTES = 0
FIXED_STACKS_BYTES = SERVICE_STACK_SIZE + IRQ_ISR_STACK_SIZE
FIXED_ROOM_BYTES = (WINDOW_BASE - BANK7_BASE - FIXED_REQUIRED_BYTES
                    - FIXED_OVERFLOW_BYTES - FIXED_STACKS_BYTES)
COMMON_BASE = 0xE000
COMMON_CODE_END = 0xFD00
# Architectural ceiling on the packed `_COMMON` + `_COMMON_DATA` run and
# the M10 far-call reservation held back from it (work package 30c, from
# the 30b audit).  The generated memory include overrides both; these
# literals only seed the synthetic host path.
COMMON_CEILING = 0x0C00
COMMON_M10_RESERVATION = 0x0200
IRQ_VECTOR_BASE = 0xFD00
IRQ_VECTOR_END = 0xFE01
IRQ_STATE_BASE = 0xFE01
STACK_BASE = 0xFF00
MEM_TOP = 0x10000

# (start, end, description, expected)
def make_regions():
    return [
        (RESET_BASE, RESET_TOP, "page-zero reset copier", True),
        (RESET_TOP, PAGE_ZERO_END, "page-zero reservation", False),
        (PAGE_ZERO_END, BANK7_BASE, "page-1 caller-window gap", False),
        (BANK7_BASE, DATA_BASE, "bank-7 fixed core code", True),
        (DATA_BASE, SERVICE_STACK_BASE,
         "bank-7 fixed core runtime data (never ROM-loaded)", False),
        (SERVICE_STACK_BASE, WINDOW_BASE,
         "fixed-core service and ISR stacks", False),
        (WINDOW_BASE, ARENA_BASE, "bank-7 cold half code", True),
        (ARENA_BASE, ARENA_END, "driver arena (load-time only)", False),
        (ARENA_END, WINDOW_DATA_BASE,
         "bank-7 cold half runtime data (never ROM-loaded)", False),
        (COMMON_BASE, COMMON_CODE_END, "permanent common code/data", True),
        (IRQ_VECTOR_BASE, IRQ_VECTOR_END, "IM2 vector page plus guard", True),
        (IRQ_STATE_BASE, STACK_BASE, "IRQ transition state/stacks", False),
        (STACK_BASE, MEM_TOP, "common boot/runtime stack", False),
    ]


REGIONS = make_regions()


def require_value(values, name, path):
    """Fetch a required declaration, naming the key and its include.

    The derived build must generate every bank-7 geometry value; a missing
    key is a hard error, never a silent fall back to a retired default."""
    if name not in values:
        raise LayoutError(
            "%s is missing from %s; the derived build must generate it"
            % (name, path))


def apply_memory_inc(path):
    """Override the bank-7 geometry declarations from a generated memory.inc."""
    global ARENA_BASE, ARENA_END, DATA_BASE, GRANULE_SIZE, GRANULE_COUNT
    global REGIONS, FIXED_CODE_END, FIXED_REQUIRED_BYTES
    global FIXED_OVERFLOW_BYTES, FIXED_STACKS_BYTES, FIXED_ROOM_BYTES
    global BANK7_RUNTIME_END, IRQ_ISR_STACK_SIZE
    global SERVICE_STACK_BASE, SERVICE_STACK_TOP, SERVICE_STACK_SIZE
    global WINDOW_BASE, WINDOW_DATA_BASE
    global ROMFS_FIRST_PAGE, ROMFS_PAGE_COUNT, ROMFS_WINDOW_SIZE
    global ROMFS_CAPACITY, ROM_ACCESS_BASE, ROM_ACCESS_SIZE
    global COMMON_CEILING, COMMON_M10_RESERVATION
    values = {}
    for line in open(path):
        line = line.split(";")[0].strip()
        match = re.match(r"([A-Za-z_]\w*)\s+=\s+(0[xX][0-9a-fA-F]+|\d+)$", line)
        if match:
            values[match.group(1)] = int(match.group(2), 0)
    # The bank-7 geometry (fixed code end, fixed data base, cold-half arena
    # start/end and granule count, cold data base, and the fixed-core
    # ledger fields) is derived per image and carried by the generated
    # memory include.  It is required: a missing key must fail loudly
    # rather than validate against a retired boundary.
    for name in ("ZOS_REV13_WINDOW_BASE", "ZOS_SERVICE_STACK_BASE",
                 "ZOS_SERVICE_STACK_TOP", "ZOS_SERVICE_STACK_SIZE",
                 "ZOS_DRIVER_ARENA_BASE", "ZOS_DRIVER_ARENA_END",
                 "ZOS_DRIVER_GRANULES", "ZOS_DATA_BASE",
                 "ZOS_WINDOW_DATA_BASE", "ZOS_FIXED_CODE_END",
                 "ZOS_FIXED_REQUIRED_BYTES", "ZOS_FIXED_OVERFLOW_BYTES",
                 "ZOS_FIXED_STACKS_BYTES", "ZOS_FIXED_ROOM_BYTES"):
        require_value(values, name, path)
    WINDOW_BASE = values["ZOS_REV13_WINDOW_BASE"]
    SERVICE_STACK_BASE = values["ZOS_SERVICE_STACK_BASE"]
    SERVICE_STACK_TOP = values["ZOS_SERVICE_STACK_TOP"]
    SERVICE_STACK_SIZE = values["ZOS_SERVICE_STACK_SIZE"]
    ARENA_BASE = values["ZOS_DRIVER_ARENA_BASE"]
    ARENA_END = values["ZOS_DRIVER_ARENA_END"]
    DATA_BASE = values["ZOS_DATA_BASE"]
    WINDOW_DATA_BASE = values["ZOS_WINDOW_DATA_BASE"]
    FIXED_CODE_END = values["ZOS_FIXED_CODE_END"]
    FIXED_REQUIRED_BYTES = values["ZOS_FIXED_REQUIRED_BYTES"]
    FIXED_OVERFLOW_BYTES = values["ZOS_FIXED_OVERFLOW_BYTES"]
    FIXED_STACKS_BYTES = values["ZOS_FIXED_STACKS_BYTES"]
    FIXED_ROOM_BYTES = values["ZOS_FIXED_ROOM_BYTES"]
    GRANULE_SIZE = values["ZOS_DRIVER_GRANULE_SIZE"]
    GRANULE_COUNT = values["ZOS_DRIVER_GRANULES"]
    BANK7_RUNTIME_END = values["ZOS_BANK7_RUNTIME_END"]
    IRQ_ISR_STACK_SIZE = values["ZOS_IRQ_ISR_STACK_SIZE"]
    ROMFS_FIRST_PAGE = values["ZOS_ROMFS_FIRST_PAGE"]
    ROMFS_PAGE_COUNT = values["ZOS_ROMFS_PAGE_COUNT"]
    ROMFS_WINDOW_SIZE = values["ZOS_ROMFS_WINDOW_SIZE"]
    ROMFS_CAPACITY = values["ZOS_ROMFS_CAPACITY"]
    ROM_ACCESS_BASE = values["ZOS_ROM_ACCESS_BASE"]
    ROM_ACCESS_SIZE = values["ZOS_ROM_ACCESS_SIZE"]
    require_value(values, "ZOS_COMMON_CEILING", path)
    require_value(values, "ZOS_COMMON_M10_RESERVATION", path)
    COMMON_CEILING = values["ZOS_COMMON_CEILING"]
    COMMON_M10_RESERVATION = values["ZOS_COMMON_M10_RESERVATION"]
    REGIONS = make_regions()

# Common-resident entry points (must be callable through the permanently
# mapped E000h-FEFFh region).
COMMON_SYMBOLS = (
    "_boot_common_entry",
    "_early_console_init",
    "_early_console_putc",
    "_early_console_puts",
    "_panic",
    "zos_syscall_entry",
    "irq_ctc0_stub",
    "irq_ff_unexpected",
    "irq_common_entry",
    "_irq_arch_init",
)
BANK7_SYMBOLS = ("_kernel_main", "zos_syscall_dispatch", "_irq_dispatch")

RST30_VECTOR = 0x0030

# Budget ledger formats (tools/zmodpack.py prints the identical lines; a
# host test pins the two against each other and against the linked map).
# Architecture 36.20: nucleus fixed (required, overflow, stacks, room),
# nucleus cold, the two bank-6 service tiers (zero until stage 4) and the
# common line for the permanently mapped window.
NUCLEUS_FIXED_LEDGER_LINE = (
    "nucleus fixed: required %d bytes | overflow %d bytes | stacks %d | "
    "room %d bytes")
NUCLEUS_COLD_LEDGER_LINE = (
    "nucleus cold: code %d bytes end %04Xh | arena %d granules "
    "(%d boot, %d free) | data %d bytes at %04Xh | room %d bytes")
SERVICE_HOT_LEDGER_LINE = "service hot: %d bytes of %d"
SERVICE_BOOT_LEDGER_LINE = "service boot/bulk: %d bytes of %d"
COMMON_LEDGER_LINE = ("common: used %d of %d bytes (code %d, data %d) | "
                      "ceiling %d, m10 %d, limit %d")


class LayoutError(Exception):
    """The linked image violates the ZephyrOS memory-layout contract."""


def parse_ihx(text):
    """Parse Intel HEX text into a dict of address -> byte.

    Raises LayoutError for records outside 0000h-FFFFh, bad checksums, and
    conflicting writes to the same address (bytes the packer would
    otherwise silently discard or place ambiguously).
    """
    mem = {}
    upper = 0
    for lineno, line in enumerate(text.splitlines(), start=1):
        line = line.strip()
        if not line:
            continue
        if not line.startswith(":"):
            raise LayoutError("IHX line %d: not an Intel HEX record" % lineno)
        try:
            raw = bytes.fromhex(line[1:])
        except ValueError:
            raise LayoutError("IHX line %d: malformed record" % lineno)
        length = raw[0]
        if length + 5 != len(raw):
            raise LayoutError("IHX line %d: bad record length" % lineno)
        offset = (raw[1] << 8) | raw[2]
        rectype = raw[3]
        data = raw[4 : 4 + length]
        check = raw[4 + length]
        if (sum(raw[:-1]) + check) & 0xFF != 0:
            raise LayoutError("IHX line %d: checksum mismatch" % lineno)
        if rectype == 0x00:
            for i, byte in enumerate(data):
                addr = upper + offset + i
                if addr >= MEM_TOP:
                    raise LayoutError(
                        "IHX line %d: address %04X outside the 16-bit space"
                        % (lineno, addr)
                    )
                if addr in mem and mem[addr] != byte:
                    raise LayoutError(
                        "IHX line %d: conflicting content for %04Xh "
                        "(%02Xh vs %02Xh)" % (lineno, addr, mem[addr], byte)
                    )
                mem[addr] = byte
        elif rectype == 0x01:
            break
        elif rectype == 0x04:
            upper = ((data[0] << 8) | data[1]) << 16
        else:
            raise LayoutError("IHX line %d: unsupported record type %02X"
                              % (lineno, rectype))
    return mem


def validate_layout(mem):
    """Enforce the region contract on a parsed memory image.

    Every linked byte must fall inside a region that is allowed to contain
    loadable content.  Page zero may hold only the reset copier and the
    three-byte RST 30h vector (verified separately against the map).
    """
    for addr in sorted(mem):
        if RST30_VECTOR <= addr < RST30_VECTOR + 3:
            continue  # checked by verify_rst30_stub
        for start, end, desc, expected in REGIONS:
            if start <= addr < end:
                if expected:
                    break
                raise LayoutError(
                    "byte at %04Xh is inside %04Xh-%04Xh (%s); "
                    "out-of-region content is not allowed"
                    % (addr, start, end - 1, desc)
                )
        else:
            raise LayoutError("byte at %04Xh is outside every defined region" % addr)


def verify_rst30_stub(mem, symbols):
    """Verify the page-zero vector: exactly JP zos_syscall_entry at 0030h."""
    vector = symbols.get("_zos_rst30_vector")
    if vector != RST30_VECTOR:
        raise LayoutError(
            "_zos_rst30_vector is at %04Xh, expected %04Xh"
            % (vector if vector is not None else -1, RST30_VECTOR)
        )
    target = symbols.get("zos_syscall_entry")
    if target is None:
        raise LayoutError("map has no symbol zos_syscall_entry")
    expected = bytes((0xC3, target & 0xFF, (target >> 8) & 0xFF))
    for i, byte in enumerate(expected):
        addr = RST30_VECTOR + i
        if mem.get(addr, FILL) != byte:
            raise LayoutError(
                "RST 30h vector byte at %04Xh is %02Xh, expected %02Xh "
                "(JP zos_syscall_entry)" % (addr, mem.get(addr, FILL), byte)
            )


def verify_im2_guard(mem, symbols):
    """FDFFh/FE00h must encode the linked irq_ff_unexpected address."""
    if symbols.get("irq_vector_page") != IRQ_VECTOR_BASE:
        raise LayoutError("irq_vector_page is not at FD00h")
    if symbols.get("irq_vector_guard") != 0xFE00:
        raise LayoutError("irq_vector_guard is not at FE00h")
    target = symbols.get("irq_ff_unexpected")
    if target is None:
        raise LayoutError("map has no symbol irq_ff_unexpected")
    actual = mem.get(0xFDFF, FILL) | (mem.get(0xFE00, FILL) << 8)
    if actual != target:
        raise LayoutError("word_at(FDFFh) is %04Xh, expected irq_ff_unexpected %04Xh"
                          % (actual, target))


def build_pages(mem, romfs=None):
    """Assemble the eight 64 KiB pages from the validated image."""
    pages = [bytearray([FILL] * PAGE_SIZE) for _ in range(PAGE_COUNT)]

    # Both boot pages carry the page-zero material (reset copier + RST 30h
    # vector): instruction fetch continues across the ROM-page latch
    # change, and the kernel needs the vector bytes available in bank 7
    # when constructing native page zero for application banks.
    for addr in range(RESET_BASE, PAGE_ZERO_END):
        byte = mem.get(addr, FILL)
        pages[0][addr] = byte
        pages[1][addr] = byte

    # Page 0 additionally carries the common region (lands in SRAM bank 0).
    for addr in range(COMMON_BASE, STACK_BASE):
        pages[0][addr] = mem.get(addr, FILL)

    # Page 1 additionally carries the bank-7 loadable content: the fixed
    # core's code and the cold half's code, up to the driver arena.  The
    # arena is never part of a ROM payload; the module loader fills it
    # from the recovery store at boot.  Both runtime-data regions and the
    # two stack reservations stay FILL.
    for addr in range(BANK7_BASE, ARENA_BASE):
        pages[1][addr] = mem.get(addr, FILL)

    # Copier mirroring: instruction fetch continues across the ROM-page
    # latch change, so page 1 must carry identical copier bytes.
    if pages[0][:RESET_TOP] != pages[1][:RESET_TOP]:
        raise LayoutError(
            "reset copier bytes differ between ROM page 0 and ROM page 1"
        )
    if pages[0][RESET_BASE] == FILL:
        raise LayoutError("reset vector at 0000h is not present")

    place_romfs(pages, romfs)
    mirror_rom_access_stub(pages, mem)
    return pages


def rom_access_stub_bytes(mem):
    """Extract and pattern-check the linked ROM-access stub bytes.

    The stub is the fixed block at ZOS_ROM_ACCESS_BASE: one mirrored
    copy in common SRAM, one byte-identical copy per recovery ROM page.
    Checking the encoding here catches any layout drift before an image
    is programmed.
    """
    stub = bytes(mem.get(ROM_ACCESS_BASE + i, FILL)
                 for i in range(ROM_ACCESS_SIZE))
    expected = {
        0: (0xED, "LDIR prefix"),
        1: (0xB0, "LDIR opcode"),
        2: (0x08, "EX AF,AF'"),
        3: (0xD3, "OUT (n),A opcode"),
        4: (0x00, "OUT (00h),A port"),
        5: (0xC3, "JP opcode"),
    }
    for index, (byte, what) in expected.items():
        if stub[index] != byte:
            raise LayoutError(
                "ROM-access stub byte %d at %04Xh is %02Xh, expected %02Xh "
                "(%s); kernel/romfs/rom_access.asm layout changed?"
                % (index, ROM_ACCESS_BASE + index, stub[index], byte, what))
    if stub[6] == FILL and stub[7] == FILL:
        raise LayoutError(
            "ROM-access stub JP target at %04Xh is unlinked"
            % (ROM_ACCESS_BASE + 6))
    # The trampoline immediately before the stub must switch modes.
    for offset, byte in ((-2, 0xD3), (-1, 0x00)):
        actual = mem.get(ROM_ACCESS_BASE + offset, FILL)
        if actual != byte:
            raise LayoutError(
                "ROM-access trampoline byte at %04Xh is %02Xh, expected "
                "%02Xh (OUT (00h),A)" % (ROM_ACCESS_BASE + offset, actual,
                                          byte))
    return stub


def mirror_rom_access_stub(pages, mem):
    """Copy the linked ROM-access stub into every recovery ROM page.

    Mode 00 answers instruction fetch from the selected ROM page, so the
    stub must be byte-identical to its common-SRAM copy at the same
    address (the machine's drive-A ROM primitive uses the same scheme).
    """
    stub = rom_access_stub_bytes(mem)
    for page in range(ROMFS_FIRST_PAGE, ROMFS_FIRST_PAGE + ROMFS_PAGE_COUNT):
        target = pages[page]
        for i in range(ROM_ACCESS_SIZE):
            addr = ROM_ACCESS_BASE + i
            if target[addr] != FILL:
                raise LayoutError(
                    "ROM-access stub mirror collides with content at page "
                    "%d offset %04Xh" % (page, addr))
        target[ROM_ACCESS_BASE:ROM_ACCESS_BASE + ROM_ACCESS_SIZE] = stub


def place_romfs(pages, romfs):
    """Place the logical ROMFS stream across the recovery pages.

    Logical byte L goes to offset L % window of page first_page + L / window.
    The C000h-FFFFh half of every recovery page and every unused byte stay
    FFh.  Recovery pages may not already carry non-FF content (collision),
    and the stream must fit the declared capacity and page count.
    """
    if romfs is None or len(romfs) == 0:
        if romfs is not None:
            raise LayoutError("recovery.romfs is empty")
        return
    if len(romfs) < ROMFS_HEADER_SIZE:
        raise LayoutError(
            "recovery.romfs is %d bytes, shorter than a ROMFS header"
            % len(romfs))
    if len(romfs) > ROMFS_CAPACITY:
        raise LayoutError(
            "recovery.romfs is %d bytes and exceeds the %d-byte ROMFS "
            "capacity" % (len(romfs), ROMFS_CAPACITY))
    logical = 0
    while logical < len(romfs):
        window = logical // ROMFS_WINDOW_SIZE
        page = ROMFS_FIRST_PAGE + window
        if window >= ROMFS_PAGE_COUNT or page >= len(pages):
            raise LayoutError(
                "recovery.romfs logical offset %d maps outside the declared "
                "recovery pages" % logical)
        offset = logical % ROMFS_WINDOW_SIZE
        chunk = min(ROMFS_WINDOW_SIZE - offset, len(romfs) - logical)
        target = pages[page]
        for i in range(chunk):
            if target[offset + i] != FILL:
                raise LayoutError(
                    "recovery byte for logical %d collides with content at "
                    "page %d offset %04Xh" % (logical + i, page, offset + i))
        target[offset:offset + chunk] = romfs[logical:logical + chunk]
        logical += chunk


def build_image(pages):
    image = b"".join(bytes(page) for page in pages)
    if len(image) != IMAGE_SIZE:
        raise LayoutError(
            "image size is %d bytes, expected exactly %d" % (len(image), IMAGE_SIZE)
        )
    return image


def parse_map_symbols(text):
    """Extract {name: address} from an sdldz80 wide map file.

    The trailing module column is optional: linker-synthesized symbols
    (`s__DATA`, `l__DATA`, ...) carry no defining module."""
    symbols = {}
    for line in text.splitlines():
        match = re.match(r"\s*([0-9A-Fa-f]{8})\s+(\S+)", line)
        if match:
            symbols[match.group(2)] = int(match.group(1), 16)
    return symbols


def check_symbol_regions(symbols):
    """Verify that key entry points sit in their required regions."""
    for name in COMMON_SYMBOLS:
        if name not in symbols:
            raise LayoutError("map has no symbol %s" % name)
        addr = symbols[name]
        if not (COMMON_BASE <= addr < COMMON_CODE_END):
            raise LayoutError(
                "%s at %04Xh is outside the common region E000h-FEFFh"
                % (name, addr)
            )
    for name in BANK7_SYMBOLS:
        if name not in symbols:
            raise LayoutError("map has no symbol %s" % name)
        addr = symbols[name]
        if name == "_kernel_main":
            # The kernel body is cold: it lives in the cold half, while the
            # gates that must stay visible stay fixed.
            if not (WINDOW_BASE <= addr < WINDOW_DATA_BASE):
                raise LayoutError(
                    "%s at %04Xh is outside the cold-half code region "
                    "%04Xh-%04Xh" % (name, addr, WINDOW_BASE,
                                     WINDOW_DATA_BASE - 1)
                )
            continue
        if not (BANK7_BASE <= addr < WINDOW_BASE):
            raise LayoutError(
                "%s at %04Xh is outside the fixed core "
                "2000h-%04Xh" % (name, addr, WINDOW_BASE - 1)
            )
    reset = symbols.get("_reset")
    if reset != RESET_BASE:
        raise LayoutError("_reset is at %04Xh, expected 0000h"
                          % (reset if reset is not None else -1))

    # Derived arena bounds: the ROM image never contains arena bytes, but
    # the linked symbols must describe the reserved region exactly.  Real
    # builds additionally pin the start to the aligned linked-kernel end,
    # the end to the derived data base, and enforce the floor.
    arena_start = symbols.get("__driver_arena_start")
    arena_end = symbols.get("__driver_arena_end")
    if arena_start != ARENA_BASE or arena_end != ARENA_END:
        raise LayoutError(
            "driver arena symbols are %s/%s, expected %04Xh/%04Xh"
            % (arena_start, arena_end, ARENA_BASE, ARENA_END)
        )
    if (arena_end - arena_start) != GRANULE_COUNT * GRANULE_SIZE:
        raise LayoutError("driver arena geometry does not match granules")
    kernel_end = symbols.get("__kernel_code_end")
    if kernel_end is not None:
        expected = (kernel_end + GRANULE_SIZE - 1) & ~(GRANULE_SIZE - 1)
        if arena_start != expected:
            raise LayoutError(
                "__driver_arena_start %04Xh is not align_up(%04Xh) = %04Xh"
                % (arena_start, kernel_end, expected))
        if not (WINDOW_BASE <= kernel_end <= WINDOW_DATA_BASE):
            raise LayoutError(
                "__kernel_code_end %04Xh is outside the cold half"
                % kernel_end)
        data_base = symbols.get("s__DATA")
        if data_base != DATA_BASE:
            raise LayoutError(
                "s__DATA is %04Xh, expected derived fixed data base %04Xh"
                % (data_base if data_base is not None else -1, DATA_BASE))
        data_size = (symbols.get("l__DATA", 0)
                     + symbols.get("l__INITIALIZED", 0))
        if DATA_BASE + data_size > SERVICE_STACK_BASE:
            raise LayoutError(
                "fixed-core runtime data ends at %04Xh, overlapping the "
                "service stack reservation at %04Xh"
                % (DATA_BASE + data_size, SERVICE_STACK_BASE))
        window_data_base = symbols.get("s__WINDOW_DATA")
        if window_data_base != WINDOW_DATA_BASE:
            raise LayoutError(
                "s__WINDOW_DATA is %04Xh, expected derived cold data base "
                "%04Xh" % (window_data_base
                           if window_data_base is not None else -1,
                           WINDOW_DATA_BASE))
        if WINDOW_DATA_BASE + symbols.get("l__WINDOW_DATA", 0) > \
                BANK7_RUNTIME_END:
            raise LayoutError("cold runtime data leaves the cold half")


def common_usage(symbols):
    """(code, data, used, capacity) bytes of packed permanent common memory.

    The two `l__` area sizes are the linked `_COMMON` and `_COMMON_DATA`
    run; the fixed absolute reservations above it are declared separately
    (docs/memory.md section 4).  Returns None when the map does not carry
    them (synthetic host maps)."""
    base = symbols.get("s__COMMON")
    code = symbols.get("l__COMMON")
    data = symbols.get("l__COMMON_DATA")
    if base is None or code is None or data is None:
        return None
    capacity = COMMON_CODE_END - COMMON_BASE
    if code + data > capacity:
        raise LayoutError(
            "packed common memory is %d bytes, over the %d-byte window "
            "%04Xh-%04Xh" % (code + data, capacity, COMMON_BASE,
                             COMMON_CODE_END - 1))
    limit = COMMON_CEILING - COMMON_M10_RESERVATION
    if code + data > limit:
        raise LayoutError(
            "packed common memory is %d bytes, over the %d-byte M9 limit "
            "(ceiling %d minus the %d-byte M10 reservation)"
            % (code + data, limit, COMMON_CEILING, COMMON_M10_RESERVATION))
    return code, data, code + data, capacity


def nucleus_ledger(symbols, plan=None):
    """The five ledger lines of architecture 36.20 from a linked map.

    Returns None for synthetic host maps without the derived symbols.
    Every package reports these lines and its own bytes against them, so
    the fields are the stable external contract (see docs/memory.md).
    The fixed-core `required`/`overflow` split is attributed per object by
    the derive step (tools/mapcheck.py geometry derive --residency) and
    carried in the generated memory include; the map checks the extents.
    The service ledgers are zero until stage 4 places images in bank 6."""
    kernel_end = symbols.get("__kernel_code_end")
    data_base = symbols.get("s__DATA")
    window_data_base = symbols.get("s__WINDOW_DATA")
    if kernel_end is None or data_base is None or window_data_base is None:
        return None
    used = sum(entry["granules"] for entry in plan["modules"]) if plan else 0
    cold_code = kernel_end - WINDOW_BASE
    cold_data = symbols.get("l__WINDOW_DATA", 0)
    cold_room = (BANK7_RUNTIME_END - WINDOW_BASE) - cold_code - cold_data \
        - GRANULE_COUNT * GRANULE_SIZE
    common = common_usage(symbols)
    common_fields = {}
    if common is not None:
        common_fields = {
            "common_code_bytes": common[0],
            "common_data_bytes": common[1],
            "common_used_bytes": common[2],
            "common_capacity_bytes": common[3],
            "common_ceiling_bytes": COMMON_CEILING,
            "common_m10_reservation_bytes": COMMON_M10_RESERVATION,
            "common_limit_bytes": COMMON_CEILING - COMMON_M10_RESERVATION,
        }
    return dict(common_fields, **{
        "fixed_code_end": FIXED_CODE_END,
        "fixed_required_bytes": FIXED_REQUIRED_BYTES,
        "fixed_overflow_bytes": FIXED_OVERFLOW_BYTES,
        "fixed_stacks_bytes": FIXED_STACKS_BYTES,
        "fixed_room_bytes": FIXED_ROOM_BYTES,
        "fixed_data_base": data_base,
        "fixed_data_bytes": (symbols.get("l__DATA", 0)
                             + symbols.get("l__INITIALIZED", 0)),
        "cold_code_bytes": cold_code,
        "cold_code_end": kernel_end,
        "cold_arena_base": ARENA_BASE,
        "cold_arena_end": ARENA_END,
        "cold_arena_granules": GRANULE_COUNT,
        "cold_arena_used_granules": used,
        "cold_arena_free_granules": GRANULE_COUNT - used,
        "cold_data_bytes": cold_data,
        "cold_data_base": window_data_base,
        "cold_room_bytes": cold_room,
        "service_hot_bytes": 0,
        "service_hot_capacity_bytes": SERVICE_HOT_CAPACITY,
        "service_boot_bytes": 0,
        "service_boot_capacity_bytes": SERVICE_BOOT_CAPACITY,
    })


def format_nucleus_ledger(ledger):
    """The five budget lines (architecture 36.20) in place of the M9
    `bank7:` line, plus the common-memory line for the permanently mapped
    window.  Byte-identical to tools/zmodpack.py (pinned by a host test)."""
    return "\n".join((
        NUCLEUS_FIXED_LEDGER_LINE % (
            ledger["fixed_required_bytes"], ledger["fixed_overflow_bytes"],
            ledger["fixed_stacks_bytes"], ledger["fixed_room_bytes"]),
        NUCLEUS_COLD_LEDGER_LINE % (
            ledger["cold_code_bytes"], ledger["cold_code_end"],
            ledger["cold_arena_granules"], ledger["cold_arena_used_granules"],
            ledger["cold_arena_free_granules"], ledger["cold_data_bytes"],
            ledger["cold_data_base"], ledger["cold_room_bytes"]),
        SERVICE_HOT_LEDGER_LINE % (
            ledger["service_hot_bytes"],
            ledger["service_hot_capacity_bytes"]),
        SERVICE_BOOT_LEDGER_LINE % (
            ledger["service_boot_bytes"],
            ledger["service_boot_capacity_bytes"]),
        COMMON_LEDGER_LINE % (
            ledger["common_used_bytes"], ledger["common_capacity_bytes"],
            ledger["common_code_bytes"], ledger["common_data_bytes"],
            ledger["common_ceiling_bytes"],
            ledger["common_m10_reservation_bytes"],
            ledger["common_limit_bytes"]),
    ))


def align_up(value, alignment=GRANULE_SIZE):
    """Round `value` up to the next `alignment` boundary."""
    return (value + alignment - 1) & ~(alignment - 1)


def align_down(value, alignment=GRANULE_SIZE):
    """Round `value` down to the previous `alignment` boundary."""
    return value & ~(alignment - 1)


def granule_thresholds(total_size, granule=GRANULE_SIZE):
    """Granule rounding distances for one module/user image.

    `above_lower_bytes` is the saving that reaches the next lower granule
    threshold, so removing it frees one granule.  `before_higher_bytes` is
    the room left before the next higher threshold costs one granule.  A
    saving smaller than `above_lower_bytes` frees no granule at all."""
    if total_size <= 0:
        return {"granules": 0, "above_lower_bytes": 0,
                "before_higher_bytes": granule}
    granules = (total_size + granule - 1) // granule
    return {
        "granules": granules,
        "above_lower_bytes": total_size - (granules - 1) * granule,
        "before_higher_bytes": granules * granule - total_size,
    }


def kernel_granule_thresholds(code_end, data_base, data_size, runtime_end,
                              granule=GRANULE_SIZE):
    """Kernel-side granule thresholds (section 32.39 ledger companion).

    Two boundaries move as the kernel changes: the arena start follows the
    aligned linked code end, and the arena end is the derived data base
    `align_down(runtime_end - data_size, granule)`.  For each, the returned
    pair gives the bytes that free one granule (a downward move) and the
    bytes left before the next higher granule is spent (an upward move).

    `data_above_lower_bytes`/`data_before_higher_bytes` are expressed as
    runtime-data size changes: shrinking the data size by the former moves
    the data base up one granule; growing it by the latter moves it down
    one granule.  With the derived-base invariant the values are exact; a
    synthetic geometry that does not satisfy it clamps at zero."""
    top = runtime_end - data_base
    return {
        "code_end": code_end,
        "code_above_lower_bytes": code_end - align_down(code_end, granule),
        "code_before_higher_bytes": align_up(code_end, granule) - code_end,
        "data_base": data_base,
        "data_size": data_size,
        "data_above_lower_bytes": max(0, data_size - (top - granule)),
        "data_before_higher_bytes": max(0, top - data_size),
    }


def format_granule_thresholds(modules, kernel):
    """Readable granule-threshold table for `zmodpack plan` and the packer.

    `modules` is the boot-set entry list (with `above_lower_bytes`/
    `before_higher_bytes`); `kernel` is a `kernel_granule_thresholds()`
    dict.  Byte-identical between tools/zmodpack.py and tools/pack_image.py
    (pinned by a host test)."""
    lines = ["granule thresholds: bytes above the lower threshold free one "
             "granule; bytes before the higher threshold spend one"]
    header = "  %-10s %7s %6s %7s %8s %12s %13s"
    lines.append(header % ("module", "image", "bss", "total", "granules",
                           "free-granule", "cost-granule"))
    for entry in modules:
        lines.append("  %-10s %7d %6d %7d %8d %12d %13d"
                     % (entry["name"], entry["image_size"],
                        entry["bss_size"], entry["total_size"],
                        entry["granules"], entry["above_lower_bytes"],
                        entry["before_higher_bytes"]))
    lines.append("  %-10s %7s %6s %7s %8s %12s %13s"
                 % ("kernel", "-", "-", "-", "-", "-", "-"))
    lines.append("  code end %04Xh: %d bytes free a granule, %d before one "
                 "is spent"
                 % (kernel["code_end"], kernel["code_above_lower_bytes"],
                    kernel["code_before_higher_bytes"]))
    lines.append("  data size %d at %04Xh: %d bytes free a granule, %d "
                 "before one is spent"
                 % (kernel["data_size"], kernel["data_base"],
                    kernel["data_above_lower_bytes"],
                    kernel["data_before_higher_bytes"]))
    return "\n".join(lines)


def romfs_placement(size):
    """Per-recovery-page placement of a logical ROMFS stream."""
    placement = []
    logical = 0
    while logical < size:
        window = logical // ROMFS_WINDOW_SIZE
        page = ROMFS_FIRST_PAGE + window
        offset = logical % ROMFS_WINDOW_SIZE
        chunk = min(ROMFS_WINDOW_SIZE - offset, size - logical)
        placement.append({
            "page": page,
            "logical_offset": logical,
            "length": chunk,
            "page_offset": offset,
            "physical_offset": page * PAGE_SIZE + offset,
        })
        logical += chunk
    return placement


def load_arena_plan(build_dir):
    """Load the boot-set arena plan emitted by tools/zmodpack.py plan.

    Returns None when no plan was generated (host tests).  The
    plan must agree with the declared arena geometry; a boot set that
    cannot coexist is a build error here as well as in the planner.
    """
    if build_dir is None:
        return None
    path = Path(build_dir) / "arena.plan.json"
    if not path.exists():
        return None
    try:
        plan = json.loads(path.read_text())
    except ValueError as exc:
        raise LayoutError("arena.plan.json is malformed: %s" % exc)
    arena = plan.get("arena", {})
    for field, expected in (("start", ARENA_BASE), ("end", ARENA_END),
                            ("granules", GRANULE_COUNT)):
        if arena.get(field) != expected:
            raise LayoutError(
                "arena plan %s is %r, expected %r"
                % (field, arena.get(field), expected))
    used = sum(entry["granules"] for entry in plan.get("modules", []))
    if used != arena.get("used_granules"):
        raise LayoutError("arena plan used_granules does not match modules")
    if used > GRANULE_COUNT:
        raise LayoutError(
            "declared boot set needs %d granules; arena holds %d"
            % (used, GRANULE_COUNT))
    return plan


def write_manifest(build_dir, suffix, image, pages, symbols, romfs, mem,
                   plan=None):
    """Emit a small machine-readable manifest for debugging/reproducibility."""
    entries = {
        name: symbols.get(name) for name in (
            "_reset", "_zos_rst30_vector", "_kernel_main",
            "zos_syscall_entry", "zos_syscall_dispatch",
            "zos_gate_stack", "zos_gate_stack_top",
            "irq_vector_page", "irq_vector_guard", "irq_ff_unexpected",
            "irq_transition_stack", "irq_transition_stack_top",
            "irq_isr_stack", "irq_isr_stack_top",
            "__kernel_code_end",
            "__driver_arena_start", "__driver_arena_end",
            "s__DATA", "l__DATA", "s__INITIALIZED", "l__INITIALIZED",
            "rom_access_trampoline", "rom_access_sync",
            "rom_access_sync_end",
        )
    }
    stub = rom_access_stub_bytes(mem)
    used = sum(entry["granules"] for entry in plan["modules"]) if plan else 0
    ledger = nucleus_ledger(symbols, plan)
    kernel = None
    if symbols.get("__kernel_code_end") is not None \
            and symbols.get("s__WINDOW_DATA") is not None:
        kernel = kernel_granule_thresholds(
            symbols["__kernel_code_end"], symbols["s__WINDOW_DATA"],
            symbols.get("l__WINDOW_DATA", 0), BANK7_RUNTIME_END)
    manifest = {
        "tool": "pack_image.py",
        "layout": {
            "reset_top": RESET_TOP,
            "page_zero_end": PAGE_ZERO_END,
            "bank7_base": BANK7_BASE,
            "window_base": WINDOW_BASE,
            "bank7_runtime_end": BANK7_RUNTIME_END,
            "service_stack_base": SERVICE_STACK_BASE,
            "service_stack_size": SERVICE_STACK_SIZE,
            "isr_stack_base": IRQ_ISR_STACK_BASE,
            "isr_stack_size": IRQ_ISR_STACK_SIZE,
            "fixed_code_end": FIXED_CODE_END,
            "data_base": DATA_BASE,
            "window_data_base": WINDOW_DATA_BASE,
            "driver_arena_base": ARENA_BASE,
            "driver_arena_end": ARENA_END,
            "common_base": COMMON_BASE,
            "stack_base": STACK_BASE,
            "rom_page_size": PAGE_SIZE,
            "rom_page_count": PAGE_COUNT,
            "romfs_first_page": ROMFS_FIRST_PAGE,
            "romfs_page_count": ROMFS_PAGE_COUNT,
            "romfs_window_size": ROMFS_WINDOW_SIZE,
            "romfs_capacity": ROMFS_CAPACITY,
            "rom_access_base": ROM_ACCESS_BASE,
            "rom_access_size": ROM_ACCESS_SIZE,
        },
        "nucleus": ledger,
        "kernel": kernel,
        "arena": {
            "start": ARENA_BASE,
            "end": ARENA_END,
            "size": ARENA_END - ARENA_BASE,
            "granule_size": GRANULE_SIZE,
            "granules": GRANULE_COUNT,
            "used_granules": used,
            "free_granules": GRANULE_COUNT - used,
            "modules": plan["modules"] if plan else [],
        },
        "image": {"size": len(image), "sha256": hashlib.sha256(image).hexdigest()},
        "pages": [
            {
                "index": index,
                "size": len(page),
                "sha256": hashlib.sha256(bytes(page)).hexdigest(),
                "fill_bytes": page.count(FILL),
            }
            for index, page in enumerate(pages)
        ],
        "page0": {
            "size": len(pages[0]),
            "sha256": hashlib.sha256(bytes(pages[0])).hexdigest(),
            "fill_bytes": pages[0].count(FILL),
        },
        "page1": {
            "size": len(pages[1]),
            "sha256": hashlib.sha256(bytes(pages[1])).hexdigest(),
            "fill_bytes": pages[1].count(FILL),
        },
        "romfs": {
            "present": romfs is not None and len(romfs) > 0,
            "size": len(romfs) if romfs else 0,
            "sha256": hashlib.sha256(romfs).hexdigest() if romfs else None,
            "first_page": ROMFS_FIRST_PAGE,
            "page_count": ROMFS_PAGE_COUNT,
            "window_size": ROMFS_WINDOW_SIZE,
            "capacity": ROMFS_CAPACITY,
            "placement": romfs_placement(len(romfs)) if romfs else [],
        },
        "rom_access": {
            "base": ROM_ACCESS_BASE,
            "size": ROM_ACCESS_SIZE,
            "mirror_pages": ROMFS_PAGE_COUNT,
            "sha256": hashlib.sha256(stub).hexdigest(),
        },
        "entries": entries,
    }
    if build_dir is not None:
        build = Path(build_dir)
        build.mkdir(parents=True, exist_ok=True)
        (build / ("zephyros%s.manifest.json" % suffix)).write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        )
    return manifest


def verify_no_banked_latch_writes(mem, symbols=None, production=True):
    """No un-audited mapping-changing instruction may be linked in bank 7.

    The latch write `OUT (00h),A` encodes as D3 00.  Under revision 13
    two placements are legal:

    - the fixed core (2000h-5FFFh) is mapped unchanged by every page, so a
      write there survives its own page change - but only the audited
      extents below may use that escape, and only to select a window page.
      Any other fixed-core latch write is a bug.
    - permanent common memory (E000h-FFFFh) holds the mapping-changing
      gates, which are visible in every mode.

    The cold half (6000h-DFFFh) is visible only while the window shows
    bank 7 (P = 0), so a latch write there would change the bytes the CPU
    is fetching; every write in it is rejected.

    Two fixed-core extents are data rather than executed code at their
    linked address: the upcall shim (which selects the page it opens) and
    the nucleus self-test's caller blob, which the harness copies into an
    application bank and runs there, so its caller-side selection happens
    in the caller window.  Both are allowed; nothing else is.
    """
    symbols = symbols or {}

    def extent(start_name, end_name):
        start = symbols.get(start_name)
        end = symbols.get(end_name)
        if start is not None and end is not None and end > start:
            return (start, end)
        return None

    shim = extent("_zos_upcall", "zos_upcall_end")
    blob = extent("zos_nucleus_blob_start", "zos_nucleus_blob_end")
    for addr in sorted(mem):
        if mem[addr] != 0xD3 or mem.get(addr + 1) != 0x00:
            continue
        if WINDOW_BASE <= addr < COMMON_BASE:
            raise LayoutError(
                "latch write OUT (00h),A encoding at %04Xh in the "
                "bank-7 cold half; a write there hides the instruction "
                "that follows it (the cold half is only mapped while the "
                "window shows bank 7)" % addr
            )
        if BANK7_BASE <= addr < WINDOW_BASE:
            if not production:
                continue
            if any(r is not None and r[0] <= addr < r[1]
                   for r in (shim, blob)):
                continue
            raise LayoutError(
                "latch write OUT (00h),A encoding at %04Xh in the fixed "
                "core; only the upcall shim (%s-%s) and the self-test "
                "caller blob (%s-%s) may carry one, every other mapping "
                "change is common-resident"
                % (addr,
                   "%04Xh" % shim[0] if shim else "absent",
                   "%04Xh" % shim[1] if shim else "absent",
                   "%04Xh" % blob[0] if blob else "absent",
                   "%04Xh" % blob[1] if blob else "absent")
            )


def pack(ihx_text, map_text, build_dir, suffix="", romfs=None,
         production=True):
    mem = parse_ihx(ihx_text)
    validate_layout(mem)
    symbols = parse_map_symbols(map_text)
    check_symbol_regions(symbols)
    verify_rst30_stub(mem, symbols)
    verify_im2_guard(mem, symbols)
    verify_no_banked_latch_writes(mem, symbols, production=production)

    # The recovery store is part of the programmed image: when an output
    # build directory is given, build/recovery.romfs must exist (the
    # Makefile generates it with tools/romfspack.py before this step).
    if romfs is None and build_dir is not None:
        romfs_path = Path(build_dir) / "recovery.romfs"
        if not romfs_path.exists():
            raise LayoutError(
                "recovery.romfs missing from %s; the recovery store is part "
                "of the image contract (build it with tools/romfspack.py)"
                % build_dir)
        romfs = romfs_path.read_bytes()

    pages = build_pages(mem, romfs)
    image = build_image(pages)

    if build_dir is not None:
        build = Path(build_dir)
        build.mkdir(parents=True, exist_ok=True)
        for index, page in enumerate(pages):
            (build / ("page%d%s.bin" % (index, suffix))).write_bytes(bytes(page))
        (build / ("zephyros%s.bin" % suffix)).write_bytes(image)
        (build / ("zephyr80%s.bin" % suffix)).write_bytes(image)
        plan = load_arena_plan(build_dir)
        write_manifest(build_dir, suffix, image, pages, symbols, romfs, mem,
                       plan)
    return image


def main(argv):
    production = "--production" in argv
    argv = [arg for arg in argv if arg != "--production"]
    if len(argv) not in (4, 5, 6):
        print(__doc__)
        return 2
    ihx_path, map_path, build_dir = argv[1:4]
    suffix = argv[4] if len(argv) >= 5 else ""
    if len(argv) == 6:
        apply_memory_inc(argv[5])
    ihx_text = Path(ihx_path).read_text()
    map_text = Path(map_path).read_text()
    try:
        image = pack(ihx_text, map_text, build_dir, suffix,
                     production=production)
    except LayoutError as exc:
        print("pack_image.py: layout error: %s" % exc, file=sys.stderr)
        return 1
    symbols = parse_map_symbols(map_text)
    digest = hashlib.sha256(image).hexdigest()
    print("pack_image.py: %d bytes, sha256 %s" % (len(image), digest))
    plan = load_arena_plan(build_dir)
    ledger = nucleus_ledger(symbols, plan)
    if ledger is not None:
        print(format_nucleus_ledger(ledger))
    kernel = None
    if symbols.get("__kernel_code_end") is not None \
            and symbols.get("s__WINDOW_DATA") is not None:
        kernel = kernel_granule_thresholds(
            symbols["__kernel_code_end"], symbols["s__WINDOW_DATA"],
            symbols.get("l__WINDOW_DATA", 0), BANK7_RUNTIME_END)
    if kernel is not None and plan is not None:
        print(format_granule_thresholds(plan["modules"], kernel))
    if plan is not None:
        print("pack_image.py: boot set uses %d granules (%s), %d free"
              % (plan["arena"]["used_granules"],
                 ", ".join("%s x%d" % (entry["name"], entry["granules"])
                           for entry in plan["modules"]),
                 plan["arena"]["free_granules"]))
    print("pack_image.py: rom-access stub %d bytes at %04Xh mirrored into "
          "recovery pages %d-%d"
          % (ROM_ACCESS_SIZE, ROM_ACCESS_BASE, ROMFS_FIRST_PAGE,
             ROMFS_FIRST_PAGE + ROMFS_PAGE_COUNT - 1))
    romfs_path = Path(build_dir) / "recovery.romfs"
    if romfs_path.exists():
        romfs = romfs_path.read_bytes()
        last_page = ROMFS_FIRST_PAGE + ((len(romfs) - 1) // ROMFS_WINDOW_SIZE
                                        if romfs else 0)
        print("pack_image.py: romfs %d bytes in recovery pages %d-%d "
              "(capacity %d, sha256 %s)"
              % (len(romfs), ROMFS_FIRST_PAGE, last_page, ROMFS_CAPACITY,
                 hashlib.sha256(romfs).hexdigest()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
