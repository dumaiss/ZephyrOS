#!/usr/bin/env python3
"""ZephyrOS ROM image packer and layout validator.

Builds the 512 KiB programmed image from the linked Intel HEX output:

    page 0  (64 KiB, copied into SRAM bank 0 by the reset copier)
    page 1  (64 KiB, copied into SRAM bank 7 by the reset copier)
    pages 2-7  (384 KiB, read-only ROMFS recovery store)

Region contract (see docs/architecture.md, docs/boot.md):

    0000h-002Fh   reset copier (page zero, both boot pages, byte-identical)
    0030h-00FFh   page-zero reservation (must stay empty)
    0100h-1FFFh   page-1 caller-window gap (must stay empty)
    2000h-9FFFh   bank-7 loadable kernel (up to the driver arena start)
    arena         M8 production: aligned linked-kernel end .. C000h;
                  pre-M8 acceptance/variant images: A000h-BFFFh
                  (load-time only; no ROM payload)
    C000h-DFFFh   bank-7 zero-initialized runtime area (must stay empty in
                  the ROM image; it is never loaded from ROM)
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
    pack_image.py <zephyros.ihx> <zephyros.map> <build_dir> [suffix]
                  [platform/zephyr80/memory.inc]

The optional memory.inc overrides the driver-arena and ROMFS declarations
(the M4 placement build shifts the arena base; the ROM contract is
otherwise unchanged).  When <build_dir> is given, <build_dir>/recovery.romfs
must exist and is placed into pages 2-7; when it is None (host unit tests)
an all-FF recovery store is emitted unless `romfs` is passed explicitly.
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
BANK7_BASE = 0x2000
BANK7_LOAD_END = 0xC000     # C000h-DFFFh is runtime, never ROM-loaded
ARENA_BASE = 0xA000         # legacy default; overridden from memory.inc
ARENA_END = 0xC000
ARENA_DERIVED = 0
ARENA_MIN_SIZE = 0x4000
ARENA_MIN_GRANULES = 32
GRANULE_SIZE = 0x0200
GRANULE_COUNT = 16
COMMON_BASE = 0xE000
COMMON_CODE_END = 0xFD00
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
        (BANK7_BASE, ARENA_BASE, "bank-7 loadable kernel", True),
        (ARENA_BASE, ARENA_END, "bank-7 driver arena (load-time only)", False),
        (BANK7_LOAD_END, COMMON_BASE,
         "bank-7 runtime area (never ROM-loaded)", False),
        (COMMON_BASE, COMMON_CODE_END, "permanent common code/data", True),
        (IRQ_VECTOR_BASE, IRQ_VECTOR_END, "IM2 vector page plus guard", True),
        (IRQ_STATE_BASE, STACK_BASE, "IRQ transition state/stacks", False),
        (STACK_BASE, MEM_TOP, "common boot/runtime stack", False),
    ]


REGIONS = make_regions()


def apply_memory_inc(path):
    """Override the arena/ROMFS declarations from platform memory.inc."""
    global ARENA_BASE, ARENA_END, GRANULE_SIZE, GRANULE_COUNT, REGIONS
    global ARENA_DERIVED, ARENA_MIN_SIZE, ARENA_MIN_GRANULES
    global ROMFS_FIRST_PAGE, ROMFS_PAGE_COUNT, ROMFS_WINDOW_SIZE
    global ROMFS_CAPACITY, ROM_ACCESS_BASE, ROM_ACCESS_SIZE
    values = {}
    for line in open(path):
        line = line.split(";")[0].strip()
        match = re.match(r"([A-Za-z_]\w*)\s+=\s+(0[xX][0-9a-fA-F]+|\d+)$", line)
        if match:
            values[match.group(1)] = int(match.group(2), 0)
    ARENA_DERIVED = values.get("ZOS_DRIVER_ARENA_DERIVED", 0)
    ARENA_BASE = values["ZOS_DRIVER_ARENA_BASE"]
    ARENA_END = values["ZOS_DRIVER_ARENA_END"]
    ARENA_MIN_SIZE = values.get("ZOS_DRIVER_ARENA_MIN_SIZE", 0x4000)
    ARENA_MIN_GRANULES = values.get("ZOS_DRIVER_ARENA_MIN_GRANULES", 32)
    GRANULE_SIZE = values["ZOS_DRIVER_GRANULE_SIZE"]
    GRANULE_COUNT = values["ZOS_DRIVER_GRANULES"]
    ROMFS_FIRST_PAGE = values["ZOS_ROMFS_FIRST_PAGE"]
    ROMFS_PAGE_COUNT = values["ZOS_ROMFS_PAGE_COUNT"]
    ROMFS_WINDOW_SIZE = values["ZOS_ROMFS_WINDOW_SIZE"]
    ROMFS_CAPACITY = values["ZOS_ROMFS_CAPACITY"]
    ROM_ACCESS_BASE = values["ZOS_ROM_ACCESS_BASE"]
    ROM_ACCESS_SIZE = values["ZOS_ROM_ACCESS_SIZE"]
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

    # Page 1 additionally carries the bank-7 loadable kernel, up to the
    # M4 driver arena.  The arena is never part of a ROM payload; the
    # module loader fills it from the recovery store at boot.
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
    """Extract {name: address} from an sdldz80 wide map file."""
    symbols = {}
    for line in text.splitlines():
        match = re.match(r"\s*([0-9A-Fa-f]{8})\s+(\S+)\s+\S+", line)
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
        if not (BANK7_BASE <= addr < BANK7_LOAD_END):
            raise LayoutError(
                "%s at %04Xh is outside the bank-7 loadable region "
                "2000h-BFFFh" % (name, addr)
            )
    reset = symbols.get("_reset")
    if reset != RESET_BASE:
        raise LayoutError("_reset is at %04Xh, expected 0000h"
                          % (reset if reset is not None else -1))

    # M4/M8 exported arena bounds: the ROM image never contains arena
    # bytes, but the linked symbols must describe the reserved region
    # exactly.  Derived (production) builds additionally pin the start to
    # the aligned linked-kernel end and enforce the 16-KiB minimum.
    arena_start = symbols.get("__driver_arena_start")
    arena_end = symbols.get("__driver_arena_end")
    if arena_start != ARENA_BASE or arena_end != ARENA_END:
        raise LayoutError(
            "driver arena symbols are %s/%s, expected %04Xh/%04Xh"
            % (arena_start, arena_end, ARENA_BASE, ARENA_END)
        )
    if (arena_end - arena_start) != GRANULE_COUNT * GRANULE_SIZE:
        raise LayoutError("driver arena geometry does not match granules")
    if ARENA_DERIVED:
        kernel_end = symbols.get("__kernel_code_end")
        if kernel_end is None:
            raise LayoutError("derived build has no __kernel_code_end")
        expected = (kernel_end + GRANULE_SIZE - 1) & ~(GRANULE_SIZE - 1)
        if arena_start != expected:
            raise LayoutError(
                "__driver_arena_start %04Xh is not align_up(%04Xh) = %04Xh"
                % (arena_start, kernel_end, expected))
        if (arena_end - arena_start) < ARENA_MIN_SIZE:
            raise LayoutError("derived driver arena below the %d-byte minimum"
                              % ARENA_MIN_SIZE)
        if GRANULE_COUNT < ARENA_MIN_GRANULES:
            raise LayoutError("derived driver arena below the %d-granule "
                              "minimum" % ARENA_MIN_GRANULES)


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

    Returns None when no plan was generated (legacy host tests).  The
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
            "rom_access_trampoline", "rom_access_sync",
            "rom_access_sync_end",
        )
    }
    stub = rom_access_stub_bytes(mem)
    used = sum(entry["granules"] for entry in plan["modules"]) if plan else 0
    manifest = {
        "tool": "pack_image.py",
        "layout": {
            "reset_top": RESET_TOP,
            "page_zero_end": PAGE_ZERO_END,
            "bank7_base": BANK7_BASE,
            "bank7_load_end": BANK7_LOAD_END,
            "driver_arena_base": ARENA_BASE,
            "driver_arena_end": ARENA_END,
            "driver_arena_derived": bool(ARENA_DERIVED),
            "driver_arena_min_size": ARENA_MIN_SIZE,
            "driver_arena_min_granules": ARENA_MIN_GRANULES,
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


def verify_no_banked_latch_writes(mem):
    """No mapping-changing instruction may be linked in bank 7.

    The latch write `OUT (00h),A` encodes as D3 00; the bank-7 image must
    not contain it, because code at 2000h-DFFFh changes underneath the CPU
    when the mode/bank changes.  All latch-write paths are common-resident
    and linked in ROM page 0.
    """
    for addr in sorted(mem):
        if BANK7_BASE <= addr < BANK7_LOAD_END and mem[addr] == 0xD3:
            if mem.get(addr + 1) == 0x00:
                raise LayoutError(
                    "latch write OUT (00h),A encoding at %04Xh in the "
                    "bank-7 loadable region; mapping-changing code must "
                    "be common-resident" % addr
                )


def pack(ihx_text, map_text, build_dir, suffix="", romfs=None):
    mem = parse_ihx(ihx_text)
    validate_layout(mem)
    symbols = parse_map_symbols(map_text)
    check_symbol_regions(symbols)
    verify_rst30_stub(mem, symbols)
    verify_im2_guard(mem, symbols)
    verify_no_banked_latch_writes(mem)

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
    global ARENA_MIN_SIZE, ARENA_MIN_GRANULES
    min_granules = None
    if "--min-granules" in argv:
        at = argv.index("--min-granules")
        min_granules = int(argv[at + 1], 0)
        del argv[at:at + 2]
    if len(argv) not in (4, 5, 6):
        print(__doc__)
        return 2
    ihx_path, map_path, build_dir = argv[1:4]
    suffix = argv[4] if len(argv) >= 5 else ""
    if len(argv) == 6:
        apply_memory_inc(argv[5])
    else:
        apply_memory_inc("platform/zephyr80/memory.inc")
    if min_granules is not None:
        ARENA_MIN_GRANULES = min_granules
        ARENA_MIN_SIZE = min_granules * GRANULE_SIZE
    ihx_text = Path(ihx_path).read_text()
    map_text = Path(map_path).read_text()
    try:
        image = pack(ihx_text, map_text, build_dir, suffix)
    except LayoutError as exc:
        print("pack_image.py: layout error: %s" % exc, file=sys.stderr)
        return 1
    symbols = parse_map_symbols(map_text)
    digest = hashlib.sha256(image).hexdigest()
    print("pack_image.py: %d bytes, sha256 %s" % (len(image), digest))
    kernel_end = symbols.get("__kernel_code_end")
    print("pack_image.py: static kernel end %s; driver arena %04Xh-%04Xh: "
          "%d bytes, %d granules, %s"
          % (("%04Xh" % kernel_end) if kernel_end is not None else "n/a",
             ARENA_BASE, ARENA_END - 1, ARENA_END - ARENA_BASE,
             GRANULE_COUNT,
             "derived" if ARENA_DERIVED else "legacy reserved"))
    plan = load_arena_plan(build_dir)
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
