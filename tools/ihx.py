#!/usr/bin/env python3
"""Intel HEX and sdldz80 map parsing shared by the KonaOS tools.

The ROM image packer (`tools/pack_image.py`) and the EXE tools
(`tools/exepack.py`, `tools/exemanifest.py`) read the same two linker
outputs: the Intel HEX image and the wide map.  This module owns that
parsing once, so the standalone SDK can ship it (work package 49c) and
there is no second implementation to drift.  Python 3 standard library
only: `exepack.py` ships inside the SDK, where the OS tools are absent.

Usage:

    import ihx
    mem = ihx.parse_ihx(ihx_text, multi=False)
    symbols = ihx.parse_map_symbols(map_text)

`parse_ihx` raises `LayoutError` for records outside 0000h-FFFFh, bad
checksums, and conflicting writes to one address.
"""

import re

MEM_TOP = 0x10000

# One map symbol line: an 8-digit hex address, the name, and the optional
# defining-module column (the linker leaves it empty for its synthesized
# s__/l__ area symbols).
SYMBOL_RE = re.compile(r"\s*([0-9A-Fa-f]{8})\s+(\S+)(?:\s+\S+)?\s*$")


class LayoutError(Exception):
    """A linked image violates the KonaOS memory-layout contract."""


def parse_ihx(text, multi=False):
    """Parse Intel HEX text into a dict of address -> byte.

    Raises LayoutError for records outside 0000h-FFFFh, bad checksums, and
    conflicting writes to the same address (bytes the packer would
    otherwise silently discard or place ambiguously).

    With `multi`, a second write to an address is kept instead of being
    rejected and every value becomes a list of bytes in record order: a
    program with several far-code areas linked at 8000h emits one record
    stream per area (`_FAR1`, `_FAR2`, ... in link order), so the n-th
    value of an address is the n-th area's byte (work package 49b).
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
                if multi:
                    mem.setdefault(addr, []).append(byte)
                    continue
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


def parse_map_symbols(text):
    """Collect {name: address} from an sdldz80 wide map.

    The trailing defining-module column is optional: the linker-synthesized
    symbols (`s__DATA`, `l__DATA`, ...) carry no defining module."""
    symbols = {}
    for line in text.splitlines():
        match = SYMBOL_RE.match(line)
        if match:
            symbols[match.group(2)] = int(match.group(1), 16)
    return symbols
