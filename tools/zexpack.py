#!/usr/bin/env python3
"""ZephyrOS ZEX v2 native-executable packer, validator, and inspector.

ZEX v2 is the only native executable container (architecture 36.18): a
primary image loaded at logical 0100h plus zero or more auxiliary
segments, all described by one up-front feasibility-checked header (see
docs/zex-format.md and formats/include/zex.h):

    00 magic[4]="ZEX2"   04 format_version=2   05 abi_version=1
    06 flags=0           07 reserved0=0
    08 header_size=36    0A segment_count      0B total_segment_slots
    0C primary_image_size  0E primary_bss_size 10 entry_offset
    12 stack_size        14 segment_table_offset (u32)
    18 primary_payload_offset (u32)  1C file_size (u32)  20 reserved1=0

then the primary payload, the segment table (count * 16-byte rows: id,
kind, image size, BSS size, reserved, u32 payload offset, u32 reserved)
and the segment payloads.  Every file offset and the total size are
32-bit, so a container may exceed 64 KiB.

A program is linked as one contiguous primary image at 0100h:

    _CODE = 0100h    code, constants, string literals (the image)
    _DATA = 0100h+image_size   zero-initialized data (BSS)

so the loader can copy the image byte-for-byte and zero the BSS without
running a C startup.  Initialized writable data is not part of ZEX v2:
the packer rejects linked content outside the declared image and rejects
initialized-data areas, keeping the load image deterministic.

A `DATA` segment is emitted from a raw payload file; the loader copies it
into a process-owned extended-memory object and zeroes its BSS
(architecture 36.16/36.18).  A `FAR_CODE` segment is linked at `8000h`
inside the program's own link (`--codeseg FAR_CODE` in the SDK rule) and
becomes a bank slice executed in place through the aperture; its
declaration names the linker `area` (for example `_FAR_CODE`) and this
tool resolves `s__<area>`/`l__<area>` from the program's map and takes
those IHX bytes (holes zero-filled).  The 36.17 link rule is enforced
here: with any far-code segment the primary image, its BSS and the stack
must stay outside `8000h-BFFFh`.

Packing validates every geometry boundary before emitting a byte; the
result codes match the kernel reader exactly (formats/include/zex.h).

Usage:
    zexpack.py pack <manifest.json> <out.zex> [--objdir DIR]
                   [--report FILE]
    zexpack.py validate <file.zex>
    zexpack.py dump <file.zex>
    zexpack.py get <manifest.json> <field>

Manifest (zex-v2):

    {
      "format": "zex-v2",
      "name": "zextest.zex",
      "input": "zextest.ihx",
      "map": "zextest.map",
      "entry": "zt_entry",
      "entry_offset": 0,
      "image_size": 1792,
      "bss_size": 512,
      "stack_size": 256,
      "scratch": {"symbol": "zos_scratch", "size": 16},
      "segments": [
        {"id": 1, "kind": "DATA", "input": "seg1.bin", "bss_size": 64},
        {"id": 2, "kind": "FAR_CODE", "area": "_FAR_CODE", "bss_size": 0}
      ]
    }

`input`/`map`/DATA payloads resolve against --objdir (default: the
manifest directory).  `scratch` is optional but required by the provided
user runtime contract: it names the low-window syscall argument block and
is rejected when it cannot fit in 0100h-1FFFh.  `segments` is optional; a
DATA segment's `image_size` is the size of its payload file, a FAR_CODE
segment's is the size of the linked area (`l__<area>`), whose `s__<area>`
must be `8000h` and at most `SEG_MAX_FAR_SIZE`.

Exit status: 0 success, 1 rejected geometry/manifest, 2 usage error.
"""

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))
import pack_image  # noqa: E402  (parse_ihx, parse_map_symbols)

MAGIC = b"ZEX2"
FORMAT_VERSION = 2
ABI_VERSION = 1
FLAGS_NONE = 0
HEADER_SIZE = 36
SEGMENT_ENTRY_SIZE = 16
LOAD_BASE = 0x0100
STACK_TOP = 0xE000
MIN_STACK_SIZE = 2
SYSCALL_WINDOW_END = 0x2000
IMAGE_FILL = 0x00
SEG_SLOT_SIZE = 0x2000
SEG_MAX_SIZE = 0x8000
SEG_MAX_FAR_SIZE = 0x4000
FAR_BASE = 0x8000
# The 36.17 link rule for a program with far-code segments: the primary
# image, its BSS and the whole stack stay outside the aperture.
FAR_IMAGE_END = 0x8000
FAR_STACK_MIN = 0xC000
MAX_SEGMENTS = 8
MAX_TOTAL_SLOTS = 20
PAYLOAD_ALIGN = 4
U32_MAX = 0xFFFFFFFF

KIND_FAR_CODE = 1
KIND_DATA = 2
KIND_BY_NAME = {"FAR_CODE": KIND_FAR_CODE, "DATA": KIND_DATA}

# Result codes: kept value-identical with formats/include/zex.h; the
# host parity test cross-checks the two so they cannot drift.
RESULT_CODES = (
    ("ZEX_EXEC_OK", 0),
    ("ZEX_EXEC_ERR_ARG", -0x21),
    ("ZEX_EXEC_ERR_ROMFS", -0x22),
    ("ZEX_EXEC_ERR_TYPE", -0x23),
    ("ZEX_EXEC_ERR_NOSPACE", -0x24),
    ("ZEX_EXEC_ERR_READ", -0x25),
    ("ZEX_EXEC_ERR_MAGIC", -0x31),
    ("ZEX_EXEC_ERR_FORMAT", -0x32),
    ("ZEX_EXEC_ERR_ABI", -0x33),
    ("ZEX_EXEC_ERR_FLAGS", -0x34),
    ("ZEX_EXEC_ERR_RESERVED0", -0x35),
    ("ZEX_EXEC_ERR_HEADER", -0x36),
    ("ZEX_EXEC_ERR_RESERVED1", -0x37),
    ("ZEX_EXEC_ERR_IMAGE", -0x38),
    ("ZEX_EXEC_ERR_FILESIZE", -0x39),
    ("ZEX_EXEC_ERR_ENTRY", -0x3A),
    ("ZEX_EXEC_ERR_STACK", -0x3B),
    ("ZEX_EXEC_ERR_IMAGE_END", -0x3C),
    ("ZEX_EXEC_ERR_BSS_END", -0x3D),
    ("ZEX_EXEC_ERR_COLLISION", -0x3E),
    ("ZEX_EXEC_ERR_SEG_SLOTS", -0x3F),
    ("ZEX_EXEC_ERR_SEG_TABLE", -0x40),
    ("ZEX_EXEC_ERR_SEGMENT", -0x41),
    ("ZEX_EXEC_ERR_SEG_KIND", -0x42),
)
CODE_BY_NAME = dict(RESULT_CODES)

MANIFEST_KEYS = {
    "format", "name", "input", "map", "entry", "entry_offset",
    "image_size", "bss_size", "stack_size", "scratch", "segments",
}
SEGMENT_MANIFEST_KEYS = {"id", "kind", "input", "area", "bss_size"}

SYMBOL_RE = re.compile(r"\s*([0-9A-Fa-f]{8})\s+(\S+)(?:\s+\S+)?\s*$")


def parse_symbols(map_text):
    """Collect {name: address} from an sdldz80 map.

    Like tools/pack_image.py, but tolerates symbol lines without the
    trailing defining-module column: the linker leaves that column
    empty for its synthesized s__/l__ area symbols, which this tool
    needs for the BSS geometry checks.
    """
    symbols = {}
    for line in map_text.splitlines():
        match = SYMBOL_RE.match(line)
        if match:
            symbols[match.group(2)] = int(match.group(1), 16)
    return symbols


class ZexError(Exception):
    """A ZEX image or build violates the v2 contract.

    Carries the boundary name (a ZEX_EXEC_ERR_* key) so the packer, the
    validator, and the target tests all report the same reason.
    """

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def segment_slots(image_size, bss_size):
    """Whole 8-KiB slots one segment's image + BSS occupies."""
    total = image_size + bss_size
    return (total + SEG_SLOT_SIZE - 1) // SEG_SLOT_SIZE


def align(value, boundary):
    return (value + boundary - 1) // boundary * boundary


# --- header construction / parsing ---------------------------------------

def build_header(fields):
    """Serialize a header dict (raw values, no validation) to 36 bytes."""
    header = bytearray()
    header += fields["magic"]
    header.append(fields["format_version"] & 0xFF)
    header.append(fields["abi_version"] & 0xFF)
    header.append(fields["flags"] & 0xFF)
    header.append(fields["reserved0"] & 0xFF)
    header += (fields["header_size"] & 0xFFFF).to_bytes(2, "little")
    header.append(fields["segment_count"] & 0xFF)
    header.append(fields["total_segment_slots"] & 0xFF)
    header += (fields["image_size"] & 0xFFFF).to_bytes(2, "little")
    header += (fields["bss_size"] & 0xFFFF).to_bytes(2, "little")
    header += (fields["entry_offset"] & 0xFFFF).to_bytes(2, "little")
    header += (fields["stack_size"] & 0xFFFF).to_bytes(2, "little")
    header += (fields["segment_table_offset"] & U32_MAX).to_bytes(4, "little")
    header += (fields["primary_payload_offset"] & U32_MAX).to_bytes(4,
                                                                    "little")
    header += (fields["file_size"] & U32_MAX).to_bytes(4, "little")
    header += (fields["reserved1"] & U32_MAX).to_bytes(4, "little")
    assert len(header) == HEADER_SIZE
    return bytes(header)


def build_segment_row(row):
    """Serialize a segment row dict (raw values) to 16 bytes."""
    data = bytearray()
    data.append(row["id"] & 0xFF)
    data.append(row["kind"] & 0xFF)
    data += (row["image_size"] & 0xFFFF).to_bytes(2, "little")
    data += (row["bss_size"] & 0xFFFF).to_bytes(2, "little")
    data += (row["reserved0"] & 0xFFFF).to_bytes(2, "little")
    data += (row["payload_offset"] & U32_MAX).to_bytes(4, "little")
    data += (row["reserved1"] & U32_MAX).to_bytes(4, "little")
    assert len(data) == SEGMENT_ENTRY_SIZE
    return bytes(data)


def base_fields():
    """A minimal well-formed field set (4-byte image: RET + 3 NOPs)."""
    return {
        "magic": MAGIC,
        "format_version": FORMAT_VERSION,
        "abi_version": ABI_VERSION,
        "flags": FLAGS_NONE,
        "reserved0": 0,
        "header_size": HEADER_SIZE,
        "segment_count": 0,
        "total_segment_slots": 0,
        "image_size": 4,
        "bss_size": 4,
        "entry_offset": 0,
        "stack_size": 16,
        "segment_table_offset": HEADER_SIZE + 4,
        "primary_payload_offset": HEADER_SIZE,
        "file_size": HEADER_SIZE + 4,
        "reserved1": 0,
    }


def base_segment_row():
    """A minimal well-formed row (1-byte payload at the file end)."""
    return {
        "id": 1,
        "kind": KIND_DATA,
        "image_size": 1,
        "bss_size": 0,
        "reserved0": 0,
        "payload_offset": 0,
        "reserved1": 0,
    }


def parse_header(data):
    """Parse the first 36 bytes; raises ZexError on a short file."""
    if len(data) < HEADER_SIZE:
        raise ZexError("ZEX_EXEC_ERR_FILESIZE",
                       "file is %d bytes, shorter than the %d-byte header"
                       % (len(data), HEADER_SIZE))
    return {
        "magic": bytes(data[0:4]),
        "format_version": data[4],
        "abi_version": data[5],
        "flags": data[6],
        "reserved0": data[7],
        "header_size": data[8] | (data[9] << 8),
        "segment_count": data[10],
        "total_segment_slots": data[11],
        "image_size": data[12] | (data[13] << 8),
        "bss_size": data[14] | (data[15] << 8),
        "entry_offset": data[16] | (data[17] << 8),
        "stack_size": data[18] | (data[19] << 8),
        "segment_table_offset": int.from_bytes(data[20:24], "little"),
        "primary_payload_offset": int.from_bytes(data[24:28], "little"),
        "file_size": int.from_bytes(data[28:32], "little"),
        "reserved1": int.from_bytes(data[32:36], "little"),
    }


def parse_segment_row(data, offset=0):
    raw = data[offset:offset + SEGMENT_ENTRY_SIZE]
    if len(raw) < SEGMENT_ENTRY_SIZE:
        raise ZexError("ZEX_EXEC_ERR_SEG_TABLE",
                       "segment table is truncated")
    return {
        "id": raw[0],
        "kind": raw[1],
        "image_size": raw[2] | (raw[3] << 8),
        "bss_size": raw[4] | (raw[5] << 8),
        "reserved0": raw[6] | (raw[7] << 8),
        "payload_offset": int.from_bytes(raw[8:12], "little"),
        "reserved1": int.from_bytes(raw[12:16], "little"),
    }


def validate_fields(fields, file_size):
    """Validate header fields against the total file size.

    Mirrors kernel/process/exec.c zex_validate_header() boundary by
    boundary and returns the ZEX_EXEC_ERR_* name of the first violation,
    or None when the header is valid.
    """
    if fields["magic"] != MAGIC:
        return "ZEX_EXEC_ERR_MAGIC"
    if fields["format_version"] != FORMAT_VERSION:
        return "ZEX_EXEC_ERR_FORMAT"
    if fields["abi_version"] != ABI_VERSION:
        return "ZEX_EXEC_ERR_ABI"
    if fields["flags"] != FLAGS_NONE:
        return "ZEX_EXEC_ERR_FLAGS"
    if fields["reserved0"] != 0:
        return "ZEX_EXEC_ERR_RESERVED0"
    if fields["header_size"] != HEADER_SIZE:
        return "ZEX_EXEC_ERR_HEADER"
    if fields["reserved1"] != 0:
        return "ZEX_EXEC_ERR_RESERVED1"
    if fields["image_size"] == 0:
        return "ZEX_EXEC_ERR_IMAGE"
    if file_size != fields["file_size"]:
        return "ZEX_EXEC_ERR_FILESIZE"
    if fields["entry_offset"] >= fields["image_size"]:
        return "ZEX_EXEC_ERR_ENTRY"
    if fields["stack_size"] < MIN_STACK_SIZE:
        return "ZEX_EXEC_ERR_STACK"
    if (fields["segment_count"] > MAX_SEGMENTS or
            fields["total_segment_slots"] > MAX_TOTAL_SLOTS):
        return "ZEX_EXEC_ERR_SEG_SLOTS"
    table_end = fields["segment_table_offset"] + \
        fields["segment_count"] * SEGMENT_ENTRY_SIZE
    if table_end > file_size:
        return "ZEX_EXEC_ERR_SEG_TABLE"
    payload_end = fields["primary_payload_offset"] + fields["image_size"]
    if payload_end > U32_MAX or payload_end > file_size:
        return "ZEX_EXEC_ERR_FILESIZE"
    image_end = LOAD_BASE + fields["image_size"]
    if image_end > STACK_TOP:
        return "ZEX_EXEC_ERR_IMAGE_END"
    bss_end = image_end + fields["bss_size"]
    if bss_end > STACK_TOP:
        return "ZEX_EXEC_ERR_BSS_END"
    stack_base = STACK_TOP - fields["stack_size"]
    if stack_base < LOAD_BASE or bss_end > stack_base:
        return "ZEX_EXEC_ERR_COLLISION"
    return None


def validate_segment_row(row, file_size):
    """Mirror kernel/process/exec.c zex_validate_segment()."""
    if row["kind"] not in (KIND_DATA, KIND_FAR_CODE):
        return "ZEX_EXEC_ERR_SEG_KIND"
    if (row["image_size"] == 0 or row["reserved0"] != 0 or
            row["reserved1"] != 0):
        return "ZEX_EXEC_ERR_SEGMENT"
    limit = (SEG_MAX_FAR_SIZE if row["kind"] == KIND_FAR_CODE
             else SEG_MAX_SIZE)
    if row["image_size"] + row["bss_size"] > limit:
        return "ZEX_EXEC_ERR_SEGMENT"
    if row["payload_offset"] + row["image_size"] > file_size:
        return "ZEX_EXEC_ERR_SEGMENT"
    return None


def geometry(fields):
    return {
        "image_size": fields["image_size"],
        "bss_size": fields["bss_size"],
        "entry": LOAD_BASE + fields["entry_offset"],
        "stack_size": fields["stack_size"],
        "stack_base": STACK_TOP - fields["stack_size"],
    }


def validate_image(data):
    """Validate a complete .zex byte string; returns (fields, geometry)."""
    fields = parse_header(data)
    code = validate_fields(fields, len(data))
    if code is not None:
        raise ZexError(code, "%s (file %d bytes)" % (code, len(data)))
    segments = []
    slots = 0
    for index in range(fields["segment_count"]):
        row = parse_segment_row(data, fields["segment_table_offset"] +
                                index * SEGMENT_ENTRY_SIZE)
        code = validate_segment_row(row, len(data))
        if code is not None:
            raise ZexError(code, "%s (segment %d)" % (code, index + 1))
        if row["id"] != index + 1:
            raise ZexError("ZEX_EXEC_ERR_SEGMENT",
                           "segment %d has id %d, expected %d"
                           % (index + 1, row["id"], index + 1))
        slots += segment_slots(row["image_size"], row["bss_size"])
        segments.append(row)
    if slots != fields["total_segment_slots"]:
        raise ZexError("ZEX_EXEC_ERR_SEG_SLOTS",
                       "segments need %d slots, header says %d"
                       % (slots, fields["total_segment_slots"]))
    return fields, geometry(fields)


def reported_name(code):
    """Human-readable boundary name for a ZEX_EXEC_ERR_* key."""
    return code if code.startswith("ZEX_") else str(code)


# --- manifest handling -----------------------------------------------------

def _non_negative_int(value):
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def load_manifest(path):
    try:
        manifest = json.loads(Path(path).read_text())
    except OSError as exc:
        raise ZexError("ZEX_EXEC_ERR_ARG", "cannot read manifest: %s" % exc)
    except ValueError as exc:
        raise ZexError("ZEX_EXEC_ERR_ARG",
                       "manifest is not valid JSON: %s" % exc)
    if not isinstance(manifest, dict):
        raise ZexError("ZEX_EXEC_ERR_ARG", "manifest must be an object")
    unknown = set(manifest) - MANIFEST_KEYS
    if unknown:
        raise ZexError("ZEX_EXEC_ERR_ARG",
                       "unknown manifest keys: %s" % ", ".join(sorted(unknown)))
    if manifest.get("format") != "zex-v2":
        raise ZexError("ZEX_EXEC_ERR_ARG",
                       "manifest format must be 'zex-v2'")
    for key in ("name", "input", "map", "entry", "entry_offset",
                "image_size", "bss_size", "stack_size"):
        if key not in manifest:
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "manifest is missing '%s'" % key)
    for key in ("entry_offset", "image_size", "bss_size", "stack_size"):
        if not _non_negative_int(manifest[key]):
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "manifest '%s' must be a non-negative integer" % key)
    scratch = manifest.get("scratch")
    if scratch is not None:
        if (not isinstance(scratch, dict) or "symbol" not in scratch
                or "size" not in scratch
                or not isinstance(scratch["size"], int)
                or scratch["size"] <= 0):
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "manifest 'scratch' must be {symbol, size>0}")
    _validate_segment_manifest(manifest.get("segments"))
    return manifest


def _validate_segment_manifest(segments):
    if segments is None:
        return
    if not isinstance(segments, list):
        raise ZexError("ZEX_EXEC_ERR_ARG",
                       "manifest 'segments' must be a list")
    if len(segments) > MAX_SEGMENTS:
        raise ZexError("ZEX_EXEC_ERR_SEG_SLOTS",
                       "%d segments exceed the %d-row table"
                       % (len(segments), MAX_SEGMENTS))
    for index, segment in enumerate(segments):
        if not isinstance(segment, dict):
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "segment %d must be an object" % (index + 1))
        unknown = set(segment) - SEGMENT_MANIFEST_KEYS
        if unknown:
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "unknown segment keys: %s"
                           % ", ".join(sorted(unknown)))
        for key in ("id", "kind", "bss_size"):
            if key not in segment:
                raise ZexError("ZEX_EXEC_ERR_ARG",
                               "segment %d is missing '%s'"
                               % (index + 1, key))
        if segment["kind"] == "DATA" and (
                not isinstance(segment.get("input"), str)
                or not segment["input"]):
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "segment %d input must be a file name"
                           % (index + 1))
        if (not _non_negative_int(segment["id"]) or segment["id"] < 1 or
                segment["id"] > 255):
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "segment %d id must be 1..255" % (index + 1))
        if not _non_negative_int(segment["bss_size"]):
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "segment %d bss_size must be non-negative"
                           % (index + 1))
        if segment["kind"] == "FAR_CODE":
            # Far code is linked at 8000h: the payload comes from the
            # program's own IHX far area, named through `area`, not from a
            # raw payload file.
            area = segment.get("area")
            if not isinstance(area, str) or not area:
                raise ZexError("ZEX_EXEC_ERR_ARG",
                               "segment %d kind FAR_CODE needs an 'area' "
                               "name" % (index + 1))
            if "input" in segment:
                raise ZexError("ZEX_EXEC_ERR_ARG",
                               "segment %d kind FAR_CODE takes 'area', not "
                               "'input'" % (index + 1))
        elif segment["kind"] == "DATA":
            if "area" in segment:
                raise ZexError("ZEX_EXEC_ERR_ARG",
                               "segment %d kind DATA takes 'input', not "
                               "'area'" % (index + 1))
        else:
            raise ZexError("ZEX_EXEC_ERR_SEG_KIND",
                           "segment %d kind %r is unknown"
                           % (index + 1, segment["kind"]))


def _resolve(raw, manifest_path, objdir):
    candidate = Path(raw)
    if candidate.is_absolute():
        return candidate
    base = Path(objdir) if objdir else Path(manifest_path).resolve().parent
    return base / candidate


# --- packing ----------------------------------------------------------------

def pack(manifest_path, out_path, objdir=None, report_path=None):
    try:
        raw = json.loads(Path(manifest_path).read_text())
    except OSError as exc:
        raise ZexError("ZEX_EXEC_ERR_ARG", "cannot read manifest: %s" % exc)
    except ValueError as exc:
        raise ZexError("ZEX_EXEC_ERR_ARG",
                       "manifest is not valid JSON: %s" % exc)
    manifest = load_manifest(manifest_path)
    ihx_path = _resolve(manifest["input"], manifest_path, objdir)
    map_path = _resolve(manifest["map"], manifest_path, objdir)
    if not ihx_path.is_file():
        raise ZexError("ZEX_EXEC_ERR_ARG", "input not found: %s" % ihx_path)
    if not map_path.is_file():
        raise ZexError("ZEX_EXEC_ERR_ARG", "map not found: %s" % map_path)

    try:
        mem = pack_image.parse_ihx(ihx_path.read_text())
    except pack_image.LayoutError as exc:
        raise ZexError("ZEX_EXEC_ERR_ARG", "IHX rejected: %s" % exc)
    symbols = parse_symbols(map_path.read_text())

    image_size = manifest["image_size"]
    bss_size = manifest["bss_size"]
    stack_size = manifest["stack_size"]
    image_end = LOAD_BASE + image_size

    # Far-code segments come out of the program's own linked IHX: resolve
    # each declared area to its map symbols (`s__<area>`/`l__<area>`) and
    # take those bytes before the primary image is checked, because far
    # bytes are linked above the image and are not part of it.  Holes in a
    # linked area are zero-filled.
    declared = manifest.get("segments") or []
    far_ranges = []
    far_payloads = {}
    for index, segment in enumerate(declared):
        if segment["kind"] != "FAR_CODE":
            continue
        area = segment["area"]
        start = symbols.get("s_" + area)
        length = symbols.get("l_" + area)
        if start is None or length is None:
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "segment %d: map has no s_%s/l_%s far area"
                           % (index + 1, area, area))
        if start != FAR_BASE:
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "segment %d: far area %s is linked at %04Xh; "
                           "far code is linked at %04Xh"
                           % (index + 1, area, start, FAR_BASE))
        if length <= 0 or length > SEG_MAX_FAR_SIZE:
            raise ZexError("ZEX_EXEC_ERR_SEGMENT",
                           "segment %d: far area %s is %d bytes, over the "
                           "%d-byte aperture"
                           % (index + 1, area, length, SEG_MAX_FAR_SIZE))
        for other in far_ranges:
            if other == (start, length):
                continue
            if not (start + length <= other[0] or other[0] + other[1] <= start):
                raise ZexError("ZEX_EXEC_ERR_ARG",
                               "segment %d: far area %s at %04Xh overlaps "
                               "the area at %04Xh" % (index + 1, area, start,
                                                      other[0]))
        far_ranges.append((start, length))
        payload = bytearray(length)
        for addr in range(start, start + length):
            if addr in mem:
                payload[addr - start] = mem[addr]
        far_payloads[index] = bytes(payload)

    def in_far(addr):
        return any(start <= addr < start + length
                   for start, length in far_ranges)

    # Declared image: every linked byte is either inside 0100h..image_end
    # or inside a declared far area.
    outside = [addr for addr in sorted(mem)
               if not in_far(addr)
               and (addr < LOAD_BASE or addr >= image_end)]
    if outside:
        raise ZexError(
            "ZEX_EXEC_ERR_ARG",
            "linked content at %04Xh lies outside the declared image "
            "%04Xh-%04Xh%s"
            % (outside[0], LOAD_BASE, image_end - 1,
               " and the declared far areas" if far_ranges else ""))

    # Entry symbol must resolve inside the image, exactly where the
    # declared entry offset says (the manifest is the program-side
    # layout contract; the map proves the link honored it).
    entry_name = manifest["entry"]
    entry_offset = manifest["entry_offset"]
    if entry_offset >= image_size:
        raise ZexError("ZEX_EXEC_ERR_ENTRY",
                       "declared entry_offset %d is outside the image"
                       % entry_offset)
    if entry_name not in symbols:
        raise ZexError("ZEX_EXEC_ERR_ARG",
                       "map has no entry symbol %s" % entry_name)
    entry = symbols[entry_name]
    if entry != LOAD_BASE + entry_offset:
        raise ZexError("ZEX_EXEC_ERR_ARG",
                       "entry %s is at %04Xh, declared offset says %04Xh"
                       % (entry_name, entry, LOAD_BASE + entry_offset))

    # BSS must begin exactly at the image end so the loader's zero-fill
    # covers every writable static.  Initialized writable data has no
    # startup support in ZEX v2 and is rejected outright.
    if symbols.get("s__DATA") != image_end:
        raise ZexError("ZEX_EXEC_ERR_ARG",
                       "BSS base s__DATA is %04Xh, expected image end %04Xh"
                       % (symbols.get("s__DATA", -1), image_end))
    if symbols.get("l__DATA", 0) > bss_size:
        raise ZexError("ZEX_EXEC_ERR_ARG",
                       "BSS is %d bytes, declared bss_size is %d"
                       % (symbols.get("l__DATA", 0), bss_size))
    for area in ("INITIALIZER", "INITIALIZED"):
        if symbols.get("l__" + area, 0) != 0:
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "initialized data is not supported by ZEX v2 "
                           "(_%s is non-empty)" % area)

    scratch = manifest.get("scratch")
    if scratch is not None:
        address = symbols.get(scratch["symbol"])
        if address is None:
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "map has no scratch symbol %s" % scratch["symbol"])
        if not (LOAD_BASE <= address
                and address + scratch["size"] <= SYSCALL_WINDOW_END):
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "scratch %s at %04Xh (size %d) is not inside the "
                           "syscall argument window 0100h-1FFFh"
                           % (scratch["symbol"], address, scratch["size"]))

    # The 36.17 link rule for far-using programs: code, stack and data
    # used while the aperture is open live outside 8000h-BFFFh of the
    # primary image, so the whole primary image and BSS end at 8000h and
    # the stack begins at C000h or above.
    if far_ranges:
        bss_end = image_end + bss_size
        stack_base = STACK_TOP - stack_size
        if (image_end > FAR_IMAGE_END or bss_end > FAR_IMAGE_END or
                stack_base < FAR_STACK_MIN):
            raise ZexError(
                "ZEX_EXEC_ERR_ARG",
                "far code needs the primary image, BSS and stack outside "
                "8000h-BFFFh: image ends %04Xh, BSS ends %04Xh, stack base "
                "%04Xh" % (image_end, bss_end, stack_base))

    image = bytearray((IMAGE_FILL,) * image_size)
    for addr, byte in mem.items():
        if in_far(addr):
            continue
        image[addr - LOAD_BASE] = byte
    image = bytes(image)

    # --- container layout -------------------------------------------------
    # Primary payload at 0100h, then the table, then the payloads (4-byte
    # aligned), then validate the built container exactly like the loader.
    segments = []
    payloads = []
    total_slots = 0
    for index, declared in enumerate(declared):
        if declared["id"] != index + 1:
            raise ZexError("ZEX_EXEC_ERR_SEGMENT",
                           "segment %d declares id %d; ids must be 1..N in "
                           "order" % (index + 1, declared["id"]))
        kind = KIND_BY_NAME[declared["kind"]]
        if kind == KIND_FAR_CODE:
            payload = far_payloads[index]
        else:
            payload_path = _resolve(declared["input"], manifest_path, objdir)
            if not payload_path.is_file():
                raise ZexError("ZEX_EXEC_ERR_ARG",
                               "segment %d payload not found: %s"
                               % (index + 1, payload_path))
            payload = payload_path.read_bytes()
        if len(payload) == 0:
            raise ZexError("ZEX_EXEC_ERR_SEGMENT",
                           "segment %d payload is empty" % (index + 1))
        limit = SEG_MAX_FAR_SIZE if kind == KIND_FAR_CODE else SEG_MAX_SIZE
        if len(payload) + declared["bss_size"] > limit:
            raise ZexError("ZEX_EXEC_ERR_SEGMENT",
                           "segment %d image + BSS is %d bytes, over the %d "
                           "limit" % (index + 1,
                                      len(payload) + declared["bss_size"],
                                      limit))
        total_slots += segment_slots(len(payload), declared["bss_size"])
        payloads.append(payload)
        segments.append({
            "id": declared["id"],
            "kind": kind,
            "image_size": len(payload),
            "bss_size": declared["bss_size"],
            "reserved0": 0,
            "payload_offset": 0,
            "reserved1": 0,
        })
    if total_slots > MAX_TOTAL_SLOTS:
        raise ZexError("ZEX_EXEC_ERR_SEG_SLOTS",
                       "segments need %d slots, over the %d limit"
                       % (total_slots, MAX_TOTAL_SLOTS))

    segment_table_offset = HEADER_SIZE + image_size
    payload_cursor = segment_table_offset + len(segments) * SEGMENT_ENTRY_SIZE
    for index, segment in enumerate(segments):
        payload_cursor = align(payload_cursor, PAYLOAD_ALIGN)
        segment["payload_offset"] = payload_cursor
        payload_cursor += segment["image_size"]
    file_size = payload_cursor

    fields = base_fields()
    fields["segment_count"] = len(segments)
    fields["total_segment_slots"] = total_slots
    fields["image_size"] = image_size
    fields["bss_size"] = bss_size
    fields["entry_offset"] = entry_offset
    fields["stack_size"] = stack_size
    fields["segment_table_offset"] = segment_table_offset
    fields["primary_payload_offset"] = HEADER_SIZE
    fields["file_size"] = file_size

    output = bytearray()
    output += build_header(fields)
    output += image
    for segment in segments:
        output += build_segment_row(segment)
    for index, segment in enumerate(segments):
        while len(output) < segment["payload_offset"]:
            output += b"\x00"
        output += payloads[index]
    assert len(output) == file_size

    code = validate_fields(fields, len(output))
    if code is not None:
        raise ZexError(code, "declared geometry rejected: %s" % code)
    validate_image(bytes(output))

    out = Path(out_path)
    if out.parent and not out.parent.exists():
        out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(output)

    result = {
        "tool": "zexpack.py",
        "format": "zex-v2",
        "name": manifest["name"],
        "output": str(out),
        "size": len(output),
        "sha256": hashlib.sha256(output).hexdigest(),
        "header": {key: (value.hex() if key == "magic" else value)
                   for key, value in sorted(fields.items())},
        "image": {
            "size": image_size,
            "sha256": hashlib.sha256(image).hexdigest(),
            "linked_bytes": len(mem),
            "fill_bytes": image.count(IMAGE_FILL),
        },
        "bss": {"size": bss_size, "linked": symbols.get("l__DATA", 0)},
        "stack": {"size": stack_size,
                  "base": STACK_TOP - stack_size,
                  "top": STACK_TOP},
        "entry": {"symbol": entry_name, "address": entry,
                  "offset": entry_offset},
        "scratch": scratch,
        "segments": [
            {
                "id": segment["id"],
                "kind": ("FAR_CODE" if segment["kind"] == KIND_FAR_CODE
                         else "DATA"),
                "image_size": segment["image_size"],
                "bss_size": segment["bss_size"],
                "payload_offset": segment["payload_offset"],
                "sha256": hashlib.sha256(payloads[index]).hexdigest(),
            }
            for index, segment in enumerate(segments)
        ],
    }
    if report_path is not None:
        report = Path(report_path)
        if report.parent and not report.parent.exists():
            report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return result


def dump(path):
    data = Path(path).read_bytes()
    fields, geo = validate_image(data)
    image = data[fields["primary_payload_offset"]:
                 fields["primary_payload_offset"] + fields["image_size"]]
    lines = [
        "zexpack: %s (%d bytes)" % (path, len(data)),
        "  magic %s  format %d  abi %d  flags %d"
        % (fields["magic"].decode("ascii", "replace"),
           fields["format_version"], fields["abi_version"],
           fields["flags"]),
        "  header_size %d  segments %d/%d slots  image_size %d  bss_size %d"
        % (fields["header_size"], fields["segment_count"],
           fields["total_segment_slots"], fields["image_size"],
           fields["bss_size"]),
        "  entry_offset %d  stack_size %d  reserved0 %d  reserved1 %d"
        % (fields["entry_offset"], fields["stack_size"], fields["reserved0"],
           fields["reserved1"]),
        "  table %08Xh  primary payload %08Xh  file_size %d"
        % (fields["segment_table_offset"],
           fields["primary_payload_offset"], fields["file_size"]),
        "  entry %04Xh  image %04Xh-%04Xh  bss %04Xh-%04Xh  stack %04Xh-%04Xh"
        % (geo["entry"], LOAD_BASE, LOAD_BASE + fields["image_size"] - 1,
           LOAD_BASE + fields["image_size"],
           LOAD_BASE + fields["image_size"] + fields["bss_size"] - 1,
           geo["stack_base"], STACK_TOP - 1),
        "  image sha256 %s" % hashlib.sha256(image).hexdigest(),
        "  file  sha256 %s" % hashlib.sha256(data).hexdigest(),
    ]
    for index in range(fields["segment_count"]):
        row = parse_segment_row(data, fields["segment_table_offset"] +
                                index * SEGMENT_ENTRY_SIZE)
        lines.append(
            "  segment %d  kind %s  image %d  bss %d  payload %08Xh"
            % (row["id"],
               "FAR_CODE" if row["kind"] == KIND_FAR_CODE else "DATA",
               row["image_size"], row["bss_size"], row["payload_offset"]))
    return "\n".join(lines)


def get_field(manifest_path, field):
    manifest = load_manifest(manifest_path)
    if field not in ("image_size", "bss_size", "stack_size"):
        raise ZexError("ZEX_EXEC_ERR_ARG",
                       "get supports image_size, bss_size, stack_size")
    return int(manifest[field])


def main(argv):
    parser = argparse.ArgumentParser(
        prog="zexpack.py", description="ZephyrOS ZEX v2 packer/validator")
    sub = parser.add_subparsers(dest="mode", required=True)

    pack_cmd = sub.add_parser("pack", help="build a .zex from linked objects")
    pack_cmd.add_argument("manifest")
    pack_cmd.add_argument("output")
    pack_cmd.add_argument("--objdir", default=None,
                          help="base directory for manifest input paths")
    pack_cmd.add_argument("--report", default=None,
                          help="write a deterministic JSON report")

    for name in ("validate", "dump"):
        cmd = sub.add_parser(name, help="%s a .zex" % name)
        cmd.add_argument("image")

    get_cmd = sub.add_parser("get", help="read one manifest layout field")
    get_cmd.add_argument("manifest")
    get_cmd.add_argument("field")

    args = parser.parse_args(argv)
    try:
        if args.mode == "pack":
            result = pack(args.manifest, args.output, objdir=args.objdir,
                          report_path=args.report)
            entry_address = result["entry"]["address"]
            print("zexpack: %s -> %s (%d bytes, entry %04Xh, sha256 %s)"
                  % (args.manifest, args.output, result["size"],
                     entry_address,
                     result["sha256"][:16]))
        elif args.mode == "validate":
            data = Path(args.image).read_bytes()
            validate_image(data)
            print("zexpack: %s OK (%d bytes, sha256 %s)"
                  % (args.image, len(data),
                     hashlib.sha256(data).hexdigest()))
        elif args.mode == "dump":
            print(dump(args.image))
        else:
            print(get_field(args.manifest, args.field))
    except ZexError as exc:
        print("zexpack: %s: %s" % (reported_name(exc.code), exc),
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
