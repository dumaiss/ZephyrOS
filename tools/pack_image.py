#!/usr/bin/env python3
"""ZephyrOS M0 ROM image packer and layout validator.

Builds the 128 KiB boot image from the linked Intel HEX output:

    page 0  (64 KiB, loaded into SRAM bank 0 by the reset copier)
        +-------------------------------+
    page 1  (64 KiB, loaded into SRAM bank 7 by the reset copier)

Region contract (see docs/architecture.md, docs/boot.md):

    0000h-002Fh   reset copier (page zero, both pages, byte-identical)
    0030h-00FFh   page-zero reservation (must stay empty at M0)
    0100h-1FFFh   page-1 caller-window gap (must stay empty)
    2000h-BFFFh   bank-7 loadable kernel image
    C000h-DFFFh   bank-7 zero-initialized runtime area (must stay empty in
                  the ROM image; it is never loaded from ROM)
    E000h-FEFFh   permanent common code/data (physical bank 0)
    FF00h-FFFFh   common boot/runtime stack (must stay empty in the image)

Any linked byte outside its assigned region is a build error, as are map
symbols whose addresses fall outside their expected region.

Usage:
    pack_image.py <zephyros.ihx> <zephyros.map> <build_dir>
"""

import json
import re
import sys
import hashlib
from pathlib import Path

FILL = 0xFF
PAGE_SIZE = 0x10000
IMAGE_SIZE = 0x20000

RESET_BASE = 0x0000
RESET_TOP = 0x0030          # M1 installs RST 30h at 0030h
PAGE_ZERO_END = 0x0100
BANK7_BASE = 0x2000
BANK7_LOAD_END = 0xC000     # C000h-DFFFh is runtime, never ROM-loaded
COMMON_BASE = 0xE000
STACK_BASE = 0xFF00
MEM_TOP = 0x10000

# (start, end, description, expected)
REGIONS = [
    (RESET_BASE, RESET_TOP, "page-zero reset copier", True),
    (RESET_TOP, PAGE_ZERO_END, "page-zero reservation", False),
    (PAGE_ZERO_END, BANK7_BASE, "page-1 caller-window gap", False),
    (BANK7_BASE, BANK7_LOAD_END, "bank-7 loadable kernel", True),
    (BANK7_LOAD_END, COMMON_BASE, "bank-7 runtime area (never ROM-loaded)", False),
    (COMMON_BASE, STACK_BASE, "permanent common region", True),
    (STACK_BASE, MEM_TOP, "common boot/runtime stack", False),
]

# Common-resident entry points (must be callable through the permanently
# mapped E000h-FEFFh region).
COMMON_SYMBOLS = (
    "_boot_common_entry",
    "_early_console_init",
    "_early_console_putc",
    "_early_console_puts",
    "_panic",
    "zos_syscall_entry",
)
BANK7_SYMBOLS = ("_kernel_main", "zos_syscall_dispatch")

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


def build_pages(mem):
    """Assemble the two 64 KiB pages from the validated image."""
    page0 = bytearray([FILL] * PAGE_SIZE)
    page1 = bytearray([FILL] * PAGE_SIZE)

    # Both pages carry the page-zero material (reset copier + RST 30h
    # vector): instruction fetch continues across the ROM-page latch
    # change, and the kernel needs the vector bytes available in bank 7
    # when constructing native page zero for application banks.
    for addr in range(RESET_BASE, PAGE_ZERO_END):
        byte = mem.get(addr, FILL)
        page0[addr] = byte
        page1[addr] = byte

    # Page 0 additionally carries the common region (lands in SRAM bank 0).
    for addr in range(COMMON_BASE, STACK_BASE):
        page0[addr] = mem.get(addr, FILL)

    # Page 1 additionally carries the bank-7 loadable kernel.
    for addr in range(BANK7_BASE, BANK7_LOAD_END):
        page1[addr] = mem.get(addr, FILL)

    # Copier mirroring: instruction fetch continues across the ROM-page
    # latch change, so page 1 must carry identical copier bytes.
    if page0[:RESET_TOP] != page1[:RESET_TOP]:
        raise LayoutError(
            "reset copier bytes differ between ROM page 0 and ROM page 1"
        )
    if page0[RESET_BASE] == FILL:
        raise LayoutError("reset vector at 0000h is not present")

    return page0, page1


def build_image(page0, page1):
    image = page0 + page1
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
        if not (COMMON_BASE <= addr < STACK_BASE):
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


def write_manifest(build_dir, suffix, image, page0, page1, symbols):
    """Emit a small machine-readable manifest for debugging/reproducibility."""
    entries = {
        name: symbols.get(name) for name in (
            "_reset", "_zos_rst30_vector", "_kernel_main",
            "zos_syscall_entry", "zos_syscall_dispatch",
            "zos_gate_stack", "zos_gate_stack_top",
        )
    }
    manifest = {
        "tool": "pack_image.py",
        "layout": {
            "reset_top": RESET_TOP,
            "page_zero_end": PAGE_ZERO_END,
            "bank7_base": BANK7_BASE,
            "bank7_load_end": BANK7_LOAD_END,
            "common_base": COMMON_BASE,
            "stack_base": STACK_BASE,
        },
        "image": {"size": len(image), "sha256": hashlib.sha256(image).hexdigest()},
        "page0": {
            "size": len(page0),
            "sha256": hashlib.sha256(page0).hexdigest(),
            "fill_bytes": page0.count(FILL),
        },
        "page1": {
            "size": len(page1),
            "sha256": hashlib.sha256(page1).hexdigest(),
            "fill_bytes": page1.count(FILL),
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


def pack(ihx_text, map_text, build_dir, suffix=""):
    mem = parse_ihx(ihx_text)
    validate_layout(mem)
    symbols = parse_map_symbols(map_text)
    check_symbol_regions(symbols)
    verify_rst30_stub(mem, symbols)
    verify_no_banked_latch_writes(mem)
    page0, page1 = build_pages(mem)
    image = build_image(page0, page1)

    if build_dir is not None:
        build = Path(build_dir)
        build.mkdir(parents=True, exist_ok=True)
        (build / ("page0%s.bin" % suffix)).write_bytes(page0)
        (build / ("page1%s.bin" % suffix)).write_bytes(page1)
        (build / ("zephyros%s.bin" % suffix)).write_bytes(image)
        (build / ("zephyr80%s.bin" % suffix)).write_bytes(image)
        write_manifest(build_dir, suffix, image, page0, page1, symbols)
    return image


def main(argv):
    if len(argv) not in (4, 5):
        print(__doc__)
        return 2
    ihx_path, map_path, build_dir = argv[1:4]
    suffix = argv[4] if len(argv) == 5 else ""
    ihx_text = Path(ihx_path).read_text()
    map_text = Path(map_path).read_text()
    try:
        image = pack(ihx_text, map_text, build_dir, suffix)
    except LayoutError as exc:
        print("pack_image.py: layout error: %s" % exc, file=sys.stderr)
        return 1
    digest = hashlib.sha256(image).hexdigest()
    print("pack_image.py: %d bytes, sha256 %s" % (len(image), digest))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
