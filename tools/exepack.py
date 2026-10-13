#!/usr/bin/env python3
"""KonaOS EXE v2 native-executable packer, validator, and inspector.

EXE v2 is the only native executable container (architecture 6.7): a
primary image loaded at logical 0100h plus zero or more auxiliary
segments, all described by one up-front feasibility-checked header (see
docs/exe-format.md and formats/include/exe.h):

    00 magic[4]="KEX2"   04 format_version=2   05 abi_version=1
    06 flags=0           07 reserved0=0
    08 header_size=36    0A segment_count      0B total_segment_slots
    0C primary_image_size  0E primary_bss_size 10 entry_offset
    12 stack_size        14 segment_table_offset (u32)
    18 primary_payload_offset (u32)  1C file_size (u32)  20 reserved1=0

then the primary payload, the segment table (count * 16-byte rows: id,
kind, image size, BSS size, flags, u32 payload offset, u32 reserved) and
the segment payloads.  Every file offset and the total size are
32-bit, so a container may exceed 64 KiB.

A program is linked as one contiguous primary image at 0100h:

    _CODE = 0100h    code, constants, string literals (the image)
    _DATA = 0100h+image_size   zero-initialized data (BSS)

so the loader can copy the image byte-for-byte and zero the BSS without
running a C startup.  Initialized writable data is not part of EXE v2:
the packer rejects linked content outside the declared image and rejects
initialized-data areas, keeping the load image deterministic.

A `DATA` segment is emitted from a raw payload file; the loader copies it
into a process-owned extended-memory object and zeroes its BSS
(architecture 4.8/6.7).  A `FAR_CODE` segment is linked at `8000h`
inside the program's own link (`--codeseg FAR_CODE` in the SDK rule) and
becomes a bank slice executed in place through the aperture; its
declaration names the linker `area` (for example `_FAR_CODE`) and this
tool resolves `s__<area>`/`l__<area>` from the program's map and takes
those IHX bytes (holes zero-filled).  The 7.2 link rule is enforced
here: with any far-code segment the primary image, its BSS and the stack
must stay outside `8000h-BFFFh`.

Packing validates every geometry boundary before emitting a byte; the
result codes match the kernel reader exactly (formats/include/exe.h).

Usage:
    exepack.py pack <manifest.json> <out.exe> [--objdir DIR]
                   [--report FILE]
    exepack.py validate <file.exe>
    exepack.py dump <file.exe>
    exepack.py get <manifest.json> <field>

Manifest (exe-v2):

    {
      "format": "exe-v2",
      "name": "exetest.exe",
      "input": "exetest.ihx",
      "map": "exetest.map",
      "entry": "zt_entry",
      "entry_offset": 0,
      "image_size": 1792,
      "bss_size": 512,
      "stack_size": 256,
      "scratch": {"symbol": "kos_scratch", "size": 16},
      "segments": [
        {"id": 1, "kind": "DATA", "input": "seg1.bin", "bss_size": 64},
        {"id": 2, "kind": "FAR_CODE", "area": "_FAR_CODE", "bss_size": 0},
        {"id": 3, "kind": "DATA", "flags": 1, "area": "_DATA1",
         "bss_area": "_DATA1B"}
      ]
    }

`input`/`map`/DATA payloads resolve against --objdir (default: the
manifest directory).  `scratch` is optional but required by the provided
user runtime contract: it names the low-window syscall argument block and
is rejected when it cannot fit in 0100h-1FFFh.  `segments` is optional; a
DATA segment's `image_size` is the size of its payload file, a FAR_CODE
segment's is the size of the linked area (`l__<area>`), whose `s__<area>`
must be `8000h` and at most `SEG_MAX_FAR_SIZE`.  A DATA segment whose
`flags` ask for `MAPPABLE` names a linked area pair instead: `area` (the
`const` image, based at `8000h`) and `bss_area` (the zeroed tail, which
the link must place immediately after the image), both from the map;
`bss_size` and `input` do not apply to it.

Exit status: 0 success, 1 rejected geometry/manifest, 2 usage error.
"""

import argparse
import hashlib
import json
import sys
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))
import ihx  # noqa: E402  (parse_ihx, parse_map_symbols)

MAGIC = b"KEX2"
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
FAR_ID_MAX = 4
# The 7.2 link rule for a program with far-code segments: the primary
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

# Segment-row flags (formats/include/exe.h): a DATA row of at most 16 KiB may
# ask for the aperture, and the loader then places it at 8000h where its
# `DATAn=` source was linked.
SEG_FLAG_MAPPABLE = 0x0001

# Result codes: kept value-identical with formats/include/exe.h; the
# host parity test cross-checks the two so they cannot drift.
RESULT_CODES = (
    ("EXE_EXEC_OK", 0),
    ("EXE_EXEC_ERR_ARG", -0x21),
    ("EXE_EXEC_ERR_ROMFS", -0x22),
    ("EXE_EXEC_ERR_TYPE", -0x23),
    ("EXE_EXEC_ERR_NOSPACE", -0x24),
    ("EXE_EXEC_ERR_READ", -0x25),
    ("EXE_EXEC_ERR_MAGIC", -0x31),
    ("EXE_EXEC_ERR_FORMAT", -0x32),
    ("EXE_EXEC_ERR_ABI", -0x33),
    ("EXE_EXEC_ERR_FLAGS", -0x34),
    ("EXE_EXEC_ERR_RESERVED0", -0x35),
    ("EXE_EXEC_ERR_HEADER", -0x36),
    ("EXE_EXEC_ERR_RESERVED1", -0x37),
    ("EXE_EXEC_ERR_IMAGE", -0x38),
    ("EXE_EXEC_ERR_FILESIZE", -0x39),
    ("EXE_EXEC_ERR_ENTRY", -0x3A),
    ("EXE_EXEC_ERR_STACK", -0x3B),
    ("EXE_EXEC_ERR_IMAGE_END", -0x3C),
    ("EXE_EXEC_ERR_BSS_END", -0x3D),
    ("EXE_EXEC_ERR_COLLISION", -0x3E),
    ("EXE_EXEC_ERR_SEG_SLOTS", -0x3F),
    ("EXE_EXEC_ERR_SEG_TABLE", -0x40),
    ("EXE_EXEC_ERR_SEGMENT", -0x41),
    ("EXE_EXEC_ERR_SEG_KIND", -0x42),
)
CODE_BY_NAME = dict(RESULT_CODES)

MANIFEST_KEYS = {
    "format", "name", "input", "map", "entry", "entry_offset",
    "image_size", "bss_size", "stack_size", "scratch", "segments",
}
SEGMENT_MANIFEST_KEYS = {"id", "kind", "input", "area", "bss_area", "bss_size",
                        "flags"}


class ExeError(Exception):
    """A EXE image or build violates the v2 contract.

    Carries the boundary name (a EXE_EXEC_ERR_* key) so the packer, the
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
    data += (row["flags"] & 0xFFFF).to_bytes(2, "little")
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
        "flags": 0,
        "payload_offset": 0,
        "reserved1": 0,
    }


def parse_header(data):
    """Parse the first 36 bytes; raises ExeError on a short file."""
    if len(data) < HEADER_SIZE:
        raise ExeError("EXE_EXEC_ERR_FILESIZE",
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
        raise ExeError("EXE_EXEC_ERR_SEG_TABLE",
                       "segment table is truncated")
    return {
        "id": raw[0],
        "kind": raw[1],
        "image_size": raw[2] | (raw[3] << 8),
        "bss_size": raw[4] | (raw[5] << 8),
        "flags": raw[6] | (raw[7] << 8),
        "payload_offset": int.from_bytes(raw[8:12], "little"),
        "reserved1": int.from_bytes(raw[12:16], "little"),
    }


def validate_fields(fields, file_size):
    """Validate header fields against the total file size.

    Mirrors kernel/process/exec.c exe_validate_header() boundary by
    boundary and returns the EXE_EXEC_ERR_* name of the first violation,
    or None when the header is valid.
    """
    if fields["magic"] != MAGIC:
        return "EXE_EXEC_ERR_MAGIC"
    if fields["format_version"] != FORMAT_VERSION:
        return "EXE_EXEC_ERR_FORMAT"
    if fields["abi_version"] != ABI_VERSION:
        return "EXE_EXEC_ERR_ABI"
    if fields["flags"] != FLAGS_NONE:
        return "EXE_EXEC_ERR_FLAGS"
    if fields["reserved0"] != 0:
        return "EXE_EXEC_ERR_RESERVED0"
    if fields["header_size"] != HEADER_SIZE:
        return "EXE_EXEC_ERR_HEADER"
    if fields["reserved1"] != 0:
        return "EXE_EXEC_ERR_RESERVED1"
    if fields["image_size"] == 0:
        return "EXE_EXEC_ERR_IMAGE"
    if file_size != fields["file_size"]:
        return "EXE_EXEC_ERR_FILESIZE"
    if fields["entry_offset"] >= fields["image_size"]:
        return "EXE_EXEC_ERR_ENTRY"
    if fields["stack_size"] < MIN_STACK_SIZE:
        return "EXE_EXEC_ERR_STACK"
    if (fields["segment_count"] > MAX_SEGMENTS or
            fields["total_segment_slots"] > MAX_TOTAL_SLOTS):
        return "EXE_EXEC_ERR_SEG_SLOTS"
    table_end = fields["segment_table_offset"] + \
        fields["segment_count"] * SEGMENT_ENTRY_SIZE
    if table_end > file_size:
        return "EXE_EXEC_ERR_SEG_TABLE"
    payload_end = fields["primary_payload_offset"] + fields["image_size"]
    if payload_end > U32_MAX or payload_end > file_size:
        return "EXE_EXEC_ERR_FILESIZE"
    image_end = LOAD_BASE + fields["image_size"]
    if image_end > STACK_TOP:
        return "EXE_EXEC_ERR_IMAGE_END"
    bss_end = image_end + fields["bss_size"]
    if bss_end > STACK_TOP:
        return "EXE_EXEC_ERR_BSS_END"
    stack_base = STACK_TOP - fields["stack_size"]
    if stack_base < LOAD_BASE or bss_end > stack_base:
        return "EXE_EXEC_ERR_COLLISION"
    return None


def validate_segment_row(row, file_size):
    """Mirror kernel/process/exec.c exe_validate_segment()."""
    if row["kind"] not in (KIND_DATA, KIND_FAR_CODE):
        return "EXE_EXEC_ERR_SEG_KIND"
    if (row["image_size"] == 0 or row["flags"] & ~SEG_FLAG_MAPPABLE or
            row["reserved1"] != 0):
        return "EXE_EXEC_ERR_SEGMENT"
    if row["flags"] != 0 and row["kind"] != KIND_DATA:
        return "EXE_EXEC_ERR_SEGMENT"
    limit = (SEG_MAX_FAR_SIZE
             if row["kind"] == KIND_FAR_CODE or row["flags"] != 0
             else SEG_MAX_SIZE)
    if row["image_size"] + row["bss_size"] > limit:
        return "EXE_EXEC_ERR_SEGMENT"
    if row["payload_offset"] + row["image_size"] > file_size:
        return "EXE_EXEC_ERR_SEGMENT"
    return None


def initialized_symbols(symbols, limit=4):
    """The names of the writable objects EXE v2 cannot load.

    SDCC puts the initial values of writable statics in the `_INITIALIZED`
    area and a copy loop in `_INITIALIZER`: the program has no startup
    support for either, so any content there is refused.  The names come
    out of the map so the refusal points at the object to fix - a
    far-data source must hold `const` tables and zeroed objects only
   .
    """
    names = []
    start = symbols.get("s__INITIALIZED")
    length = symbols.get("l__INITIALIZED", 0)
    if start is not None and length:
        inside = sorted(name for name, address in symbols.items()
                        if start <= address < start + length)
        names = [name for name in inside if name.startswith("_")][:limit]
        if len(inside) > len(names):
            names.append("(+%d more)" % (len(inside) - len(names)))
    return names


def geometry(fields):
    return {
        "image_size": fields["image_size"],
        "bss_size": fields["bss_size"],
        "entry": LOAD_BASE + fields["entry_offset"],
        "stack_size": fields["stack_size"],
        "stack_base": STACK_TOP - fields["stack_size"],
    }


def validate_image(data):
    """Validate a complete .exe byte string; returns (fields, geometry)."""
    fields = parse_header(data)
    code = validate_fields(fields, len(data))
    if code is not None:
        raise ExeError(code, "%s (file %d bytes)" % (code, len(data)))
    segments = []
    slots = 0
    for index in range(fields["segment_count"]):
        row = parse_segment_row(data, fields["segment_table_offset"] +
                                index * SEGMENT_ENTRY_SIZE)
        code = validate_segment_row(row, len(data))
        if code is not None:
            raise ExeError(code, "%s (segment %d)" % (code, index + 1))
        if row["id"] != index + 1:
            raise ExeError("EXE_EXEC_ERR_SEGMENT",
                           "segment %d has id %d, expected %d"
                           % (index + 1, row["id"], index + 1))
        slots += segment_slots(row["image_size"], row["bss_size"])
        segments.append(row)
    if slots != fields["total_segment_slots"]:
        raise ExeError("EXE_EXEC_ERR_SEG_SLOTS",
                       "segments need %d slots, header says %d"
                       % (slots, fields["total_segment_slots"]))
    return fields, geometry(fields)


def reported_name(code):
    """Human-readable boundary name for a EXE_EXEC_ERR_* key."""
    return code if code.startswith("EXE_") else str(code)


# --- manifest handling -----------------------------------------------------

def _non_negative_int(value):
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def load_manifest(path):
    try:
        manifest = json.loads(Path(path).read_text())
    except OSError as exc:
        raise ExeError("EXE_EXEC_ERR_ARG", "cannot read manifest: %s" % exc)
    except ValueError as exc:
        raise ExeError("EXE_EXEC_ERR_ARG",
                       "manifest is not valid JSON: %s" % exc)
    if not isinstance(manifest, dict):
        raise ExeError("EXE_EXEC_ERR_ARG", "manifest must be an object")
    unknown = set(manifest) - MANIFEST_KEYS
    if unknown:
        raise ExeError("EXE_EXEC_ERR_ARG",
                       "unknown manifest keys: %s" % ", ".join(sorted(unknown)))
    if manifest.get("format") != "exe-v2":
        raise ExeError("EXE_EXEC_ERR_ARG",
                       "manifest format must be 'exe-v2'")
    for key in ("name", "input", "map", "entry", "entry_offset",
                "image_size", "bss_size", "stack_size"):
        if key not in manifest:
            raise ExeError("EXE_EXEC_ERR_ARG",
                           "manifest is missing '%s'" % key)
    for key in ("entry_offset", "image_size", "bss_size", "stack_size"):
        if not _non_negative_int(manifest[key]):
            raise ExeError("EXE_EXEC_ERR_ARG",
                           "manifest '%s' must be a non-negative integer" % key)
    scratch = manifest.get("scratch")
    if scratch is not None:
        if (not isinstance(scratch, dict) or "symbol" not in scratch
                or "size" not in scratch
                or not isinstance(scratch["size"], int)
                or scratch["size"] <= 0):
            raise ExeError("EXE_EXEC_ERR_ARG",
                           "manifest 'scratch' must be {symbol, size>0}")
    _validate_segment_manifest(manifest.get("segments"))
    return manifest


def _validate_segment_manifest(segments):
    if segments is None:
        return
    if not isinstance(segments, list):
        raise ExeError("EXE_EXEC_ERR_ARG",
                       "manifest 'segments' must be a list")
    if len(segments) > MAX_SEGMENTS:
        raise ExeError("EXE_EXEC_ERR_SEG_SLOTS",
                       "%d segments exceed the %d-row table"
                       % (len(segments), MAX_SEGMENTS))
    for index, segment in enumerate(segments):
        if not isinstance(segment, dict):
            raise ExeError("EXE_EXEC_ERR_ARG",
                           "segment %d must be an object" % (index + 1))
        unknown = set(segment) - SEGMENT_MANIFEST_KEYS
        if unknown:
            raise ExeError("EXE_EXEC_ERR_ARG",
                           "unknown segment keys: %s"
                           % ", ".join(sorted(unknown)))
        for key in ("id", "kind"):
            if key not in segment:
                raise ExeError("EXE_EXEC_ERR_ARG",
                               "segment %d is missing '%s'"
                               % (index + 1, key))
        flags = segment.get("flags", 0)
        if flags not in (0, SEG_FLAG_MAPPABLE):
            raise ExeError("EXE_EXEC_ERR_SEGMENT",
                           "segment %d flags %r is neither 0 nor MAPPABLE"
                           % (index + 1, flags))
        if flags != 0 and segment["kind"] != "DATA":
            raise ExeError("EXE_EXEC_ERR_SEGMENT",
                           "segment %d: only a DATA row may be MAPPABLE"
                           % (index + 1))
        if (not _non_negative_int(segment["id"]) or segment["id"] < 1 or
                segment["id"] > 255):
            raise ExeError("EXE_EXEC_ERR_ARG",
                           "segment %d id must be 1..255" % (index + 1))
        linked = "area" in segment
        if segment["kind"] == "FAR_CODE":
            if segment["id"] > FAR_ID_MAX:
                raise ExeError("EXE_EXEC_ERR_ARG",
                               "segment %d: FAR_CODE id %d is over the %d "
                               "far code segments FAR1=..FAR%d="
                               % (index + 1, segment["id"], FAR_ID_MAX,
                                  FAR_ID_MAX))
            if segment["id"] != index + 1:
                raise ExeError("EXE_EXEC_ERR_ARG",
                               "segment %d: FAR_CODE id %d is not id %d; "
                               "the far code segments are declared first, "
                               "ids 1..k for the FAR1=..FARK= options used "
                               ", and the DATA segments follow"
                               % (index + 1, segment["id"], index + 1))
            # Far code is linked at 8000h: the payload comes from the
            # program's own IHX far area, named through `area`, not from a
            # raw payload file.
            if not isinstance(segment.get("area"), str) or not segment["area"]:
                raise ExeError("EXE_EXEC_ERR_ARG",
                               "segment %d kind FAR_CODE needs an 'area' "
                               "name" % (index + 1))
            if "input" in segment or "bss_area" in segment:
                raise ExeError("EXE_EXEC_ERR_ARG",
                               "segment %d kind FAR_CODE takes 'area', not "
                               "'input'/'bss_area'" % (index + 1))
        elif segment["kind"] == "DATA":
            if linked:
                # A linked far-data row: the `DATAn=` areas.
                if not isinstance(segment["area"], str) or not segment["area"]:
                    raise ExeError("EXE_EXEC_ERR_ARG",
                                   "segment %d DATA area must be a name"
                                   % (index + 1))
                if (not isinstance(segment.get("bss_area"), str)
                        or not segment["bss_area"]):
                    raise ExeError("EXE_EXEC_ERR_ARG",
                                   "segment %d kind DATA with an 'area' "
                                   "needs the 'bss_area' of its zeroed "
                                   "objects" % (index + 1))
                if "input" in segment or "bss_size" in segment:
                    raise ExeError("EXE_EXEC_ERR_ARG",
                                   "segment %d: a linked DATA row takes "
                                   "'area'/'bss_area'; the map supplies "
                                   "its sizes" % (index + 1))
            else:
                if not isinstance(segment.get("input"), str) \
                        or not segment["input"]:
                    raise ExeError("EXE_EXEC_ERR_ARG",
                                   "segment %d kind DATA needs 'input' (a "
                                   "payload file) or an 'area' (a linked "
                                   "DATAn= pair)" % (index + 1))
                if "bss_area" in segment:
                    raise ExeError("EXE_EXEC_ERR_ARG",
                                   "segment %d kind DATA takes 'bss_area' "
                                   "only with 'area'" % (index + 1))
        else:
            raise ExeError("EXE_EXEC_ERR_SEG_KIND",
                           "segment %d kind %r is unknown"
                           % (index + 1, segment["kind"]))
        # `bss_size` is declared for the rows whose BSS is not linked: a
        # FAR_CODE row (0 today) and a payload-file DATA row.  A linked
        # DATA row takes both sizes from the map.
        if not linked:
            if "bss_size" not in segment:
                raise ExeError("EXE_EXEC_ERR_ARG",
                               "segment %d is missing 'bss_size'"
                               % (index + 1))
            if not _non_negative_int(segment["bss_size"]):
                raise ExeError("EXE_EXEC_ERR_ARG",
                               "segment %d bss_size must be non-negative"
                               % (index + 1))


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
        raise ExeError("EXE_EXEC_ERR_ARG", "cannot read manifest: %s" % exc)
    except ValueError as exc:
        raise ExeError("EXE_EXEC_ERR_ARG",
                       "manifest is not valid JSON: %s" % exc)
    manifest = load_manifest(manifest_path)
    ihx_path = _resolve(manifest["input"], manifest_path, objdir)
    map_path = _resolve(manifest["map"], manifest_path, objdir)
    if not ihx_path.is_file():
        raise ExeError("EXE_EXEC_ERR_ARG", "input not found: %s" % ihx_path)
    if not map_path.is_file():
        raise ExeError("EXE_EXEC_ERR_ARG", "map not found: %s" % map_path)

    try:
        # Several far areas share 8000h, one per auxiliary bank, and the
        # linker emits one record stream per area, so a linked address can
        # carry one byte per area at that address.
        mem = ihx.parse_ihx(ihx_path.read_text(), multi=True)
    except ihx.LayoutError as exc:
        raise ExeError("EXE_EXEC_ERR_ARG", "IHX rejected: %s" % exc)
    symbols = ihx.parse_map_symbols(map_path.read_text())

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
    # The linked areas in link order: ascending segment id, which is the
    # order the FAR1=..FAR4= and DATA1=..DATA4= sources are linked and the
    # order sdldz80 writes their record streams.  They all sit at 8000h -
    # one aperture and one auxiliary bank per segment - so a linked address
    # inside them carries one byte per covering area, in that same order.
    # A far-data row also names the BSS area its zeroed objects went into;
    # its size is free (no linked bytes) but it must start exactly where
    # the image ends, or the loader's zero-fill would miss it.
    far_entries = []
    for index, segment in enumerate(declared):
        if segment["kind"] == "FAR_CODE":
            area = segment["area"]
            if area != "_FAR%d" % segment["id"]:
                raise ExeError("EXE_EXEC_ERR_ARG",
                               "segment %d: FAR_CODE id %d is the far area "
                               "`_FAR%d`, not %r (ids 1-4 are "
                               "the far code segments)"
                               % (index + 1, segment["id"], segment["id"],
                                  area))
            bss_area = None
        elif "area" in segment:
            area = segment["area"]
            bss_area = segment["bss_area"]
        else:
            continue
        start = symbols.get("s_" + area)
        length = symbols.get("l_" + area)
        if start is None or length is None:
            raise ExeError("EXE_EXEC_ERR_ARG",
                           "segment %d: map has no s_%s/l_%s area"
                           % (index + 1, area, area))
        if start != FAR_BASE:
            raise ExeError("EXE_EXEC_ERR_ARG",
                           "segment %d: area %s is linked at %04Xh; a "
                           "far-code or far-data segment is linked at %04Xh"
                           % (index + 1, area, start, FAR_BASE))
        if length > SEG_MAX_FAR_SIZE:
            raise ExeError("EXE_EXEC_ERR_SEGMENT",
                           "segment %d: area %s is %d bytes, over the "
                           "%d-byte aperture"
                           % (index + 1, area, length, SEG_MAX_FAR_SIZE))
        if length == 0:
            raise ExeError("EXE_EXEC_ERR_SEGMENT",
                           "segment %d: area %s carries no const object; a "
                           "far-data segment needs an initialized table "
                           "(a segment of only zeroed objects has no image)"
                           % (index + 1, area))
        if bss_area is not None:
            bss_start = symbols.get("s_" + bss_area)
            bss_length = symbols.get("l_" + bss_area)
            if bss_start is None or bss_length is None:
                raise ExeError("EXE_EXEC_ERR_ARG",
                               "segment %d: map has no s_%s/l_%s BSS area"
                               % (index + 1, bss_area, bss_area))
            if bss_start != start + length:
                raise ExeError("EXE_EXEC_ERR_SEGMENT",
                               "segment %d: BSS area %s starts at %04Xh, "
                               "not at the image end %04Xh; the loader "
                               "zeroes the BSS right after the image "
                               "(%04Xh + %d)"
                               % (index + 1, bss_area, bss_start,
                                  start + length, FAR_BASE, length))
            if length + bss_length > SEG_MAX_FAR_SIZE:
                raise ExeError("EXE_EXEC_ERR_SEGMENT",
                               "segment %d: %s + %s is %d bytes, over the "
                               "%d-byte aperture"
                               % (index + 1, area, bss_area,
                                  length + bss_length, SEG_MAX_FAR_SIZE))
        far_entries.append((segment["id"], index, area, start, length))
    far_entries.sort()
    far_ranges = [(entry[3], entry[4]) for entry in far_entries]

    # Which far areas cover each linked address, in link order: the
    # position of an area here is the index of its byte in the address's
    # value list.
    far_owners = {}
    for position, entry in enumerate(far_entries):
        for addr in range(entry[3], entry[3] + entry[4]):
            far_owners.setdefault(addr, []).append(position)

    far_payloads = {}
    for position, entry in enumerate(far_entries):
        seg_id, index, area, start, length = entry
        payload = bytearray(length)
        for addr in range(start, start + length):
            values = mem.get(addr)
            owners = far_owners[addr]
            if values is None:
                continue  # a hole in this area's stream: zero-filled
            if len(values) != len(owners):
                raise ExeError(
                    "EXE_EXEC_ERR_ARG",
                    "segment %d: the link wrote %d byte stream(s) at %04Xh, "
                    "but %d far area(s) cover it; far areas are one record "
                    "stream per segment, in ascending id"
                    % (index + 1, len(values), addr, len(owners)))
            payload[addr - start] = values[owners.index(position)]
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
        raise ExeError(
            "EXE_EXEC_ERR_ARG",
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
        raise ExeError("EXE_EXEC_ERR_ENTRY",
                       "declared entry_offset %d is outside the image"
                       % entry_offset)
    if entry_name not in symbols:
        raise ExeError("EXE_EXEC_ERR_ARG",
                       "map has no entry symbol %s" % entry_name)
    entry = symbols[entry_name]
    if entry != LOAD_BASE + entry_offset:
        raise ExeError("EXE_EXEC_ERR_ARG",
                       "entry %s is at %04Xh, declared offset says %04Xh"
                       % (entry_name, entry, LOAD_BASE + entry_offset))

    # BSS must begin exactly at the image end so the loader's zero-fill
    # covers every writable static.  Initialized writable data has no
    # startup support in EXE v2 and is rejected outright.
    if symbols.get("s__DATA") != image_end:
        raise ExeError("EXE_EXEC_ERR_ARG",
                       "BSS base s__DATA is %04Xh, expected image end %04Xh"
                       % (symbols.get("s__DATA", -1), image_end))
    if symbols.get("l__DATA", 0) > bss_size:
        raise ExeError("EXE_EXEC_ERR_ARG",
                       "BSS is %d bytes, declared bss_size is %d"
                       % (symbols.get("l__DATA", 0), bss_size))
    if (symbols.get("l__INITIALIZED", 0) != 0
            or symbols.get("l__INITIALIZER", 0) != 0):
        names = initialized_symbols(symbols)
        raise ExeError(
            "EXE_EXEC_ERR_ARG",
            "initialized writable data is not supported by EXE v2%s; a "
            "program holds `const` tables and zeroed objects only, and "
            "initializes writable data at run time"
            % (": %s" % ", ".join(names) if names else
               " (_INITIALIZED/_INITIALIZER are non-empty)"))

    scratch = manifest.get("scratch")
    if scratch is not None:
        address = symbols.get(scratch["symbol"])
        if address is None:
            raise ExeError("EXE_EXEC_ERR_ARG",
                           "map has no scratch symbol %s" % scratch["symbol"])
        if not (LOAD_BASE <= address
                and address + scratch["size"] <= SYSCALL_WINDOW_END):
            raise ExeError("EXE_EXEC_ERR_ARG",
                           "scratch %s at %04Xh (size %d) is not inside the "
                           "syscall argument window 0100h-1FFFh"
                           % (scratch["symbol"], address, scratch["size"]))

    # The 7.2 link rule for far-using programs: code, stack and data
    # used while the aperture is open live outside 8000h-BFFFh of the
    # primary image, so the whole primary image and BSS end at 8000h and
    # the stack begins at C000h or above.
    if far_ranges:
        bss_end = image_end + bss_size
        stack_base = STACK_TOP - stack_size
        if (image_end > FAR_IMAGE_END or bss_end > FAR_IMAGE_END or
                stack_base < FAR_STACK_MIN):
            raise ExeError(
                "EXE_EXEC_ERR_ARG",
                "far code needs the primary image, BSS and stack outside "
                "8000h-BFFFh: image ends %04Xh, BSS ends %04Xh, stack base "
                "%04Xh" % (image_end, bss_end, stack_base))

    image = bytearray((IMAGE_FILL,) * image_size)
    for addr, values in mem.items():
        if in_far(addr):
            continue
        if len(values) != 1:
            raise ExeError("EXE_EXEC_ERR_ARG",
                           "linked content at %04Xh has %d definitions "
                           "outside the declared far areas"
                           % (addr, len(values)))
        image[addr - LOAD_BASE] = values[0]
    image = bytes(image)

    # --- container layout -------------------------------------------------
    # Primary payload at 0100h, then the table, then the payloads (4-byte
    # aligned), then validate the built container exactly like the loader.
    segments = []
    payloads = []
    total_slots = 0
    for index, declared in enumerate(declared):
        if declared["id"] != index + 1:
            raise ExeError("EXE_EXEC_ERR_SEGMENT",
                           "segment %d declares id %d; ids must be 1..N in "
                           "order - the far code segments (FAR1=..FAR4=) "
                           "first, then the DATA segments"
                           % (index + 1, declared["id"]))
        kind = KIND_BY_NAME[declared["kind"]]
        linked_area = ("area" in declared
                       and declared["kind"] != "FAR_CODE")
        if kind == KIND_FAR_CODE:
            payload = far_payloads[index]
            row_bss = declared["bss_size"]
        elif linked_area:
            # A far-data row: the image is the linked `area`, the zeroed
            # objects are the `bss_area` right after it.
            payload = far_payloads[index]
            row_bss = symbols.get("l_" + declared["bss_area"], 0)
        else:
            payload_path = _resolve(declared["input"], manifest_path, objdir)
            if not payload_path.is_file():
                raise ExeError("EXE_EXEC_ERR_ARG",
                               "segment %d payload not found: %s"
                               % (index + 1, payload_path))
            payload = payload_path.read_bytes()
            row_bss = declared["bss_size"]
        if len(payload) == 0:
            raise ExeError("EXE_EXEC_ERR_SEGMENT",
                           "segment %d payload is empty" % (index + 1))
        flags = declared.get("flags", 0)
        limit = (SEG_MAX_FAR_SIZE
                 if kind == KIND_FAR_CODE or flags != 0 else SEG_MAX_SIZE)
        if len(payload) + row_bss > limit:
            raise ExeError("EXE_EXEC_ERR_SEGMENT",
                           "segment %d image + BSS is %d bytes, over the %d "
                           "limit" % (index + 1, len(payload) + row_bss,
                                      limit))
        total_slots += segment_slots(len(payload), row_bss)
        payloads.append(payload)
        segments.append({
            "id": declared["id"],
            "kind": kind,
            "image_size": len(payload),
            "bss_size": row_bss,
            "flags": flags,
            "payload_offset": 0,
            "reserved1": 0,
        })
    if total_slots > MAX_TOTAL_SLOTS:
        raise ExeError("EXE_EXEC_ERR_SEG_SLOTS",
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
        raise ExeError(code, "declared geometry rejected: %s" % code)
    validate_image(bytes(output))

    out = Path(out_path)
    if out.parent and not out.parent.exists():
        out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(output)

    result = {
        "tool": "exepack.py",
        "format": "exe-v2",
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
                "flags": segment["flags"],
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
        "exepack: %s (%d bytes)" % (path, len(data)),
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
            "  segment %d  kind %s  flags %d  image %d  bss %d  payload %08Xh"
            % (row["id"],
               "FAR_CODE" if row["kind"] == KIND_FAR_CODE else "DATA",
               row["flags"], row["image_size"], row["bss_size"],
               row["payload_offset"]))
    return "\n".join(lines)


def get_field(manifest_path, field):
    manifest = load_manifest(manifest_path)
    if field not in ("image_size", "bss_size", "stack_size"):
        raise ExeError("EXE_EXEC_ERR_ARG",
                       "get supports image_size, bss_size, stack_size")
    return int(manifest[field])


def main(argv):
    parser = argparse.ArgumentParser(
        prog="exepack.py", description="KonaOS EXE v2 packer/validator")
    sub = parser.add_subparsers(dest="mode", required=True)

    pack_cmd = sub.add_parser("pack", help="build a .exe from linked objects")
    pack_cmd.add_argument("manifest")
    pack_cmd.add_argument("output")
    pack_cmd.add_argument("--objdir", default=None,
                          help="base directory for manifest input paths")
    pack_cmd.add_argument("--report", default=None,
                          help="write a deterministic JSON report")

    for name in ("validate", "dump"):
        cmd = sub.add_parser(name, help="%s a .exe" % name)
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
            print("exepack: %s -> %s (%d bytes, entry %04Xh, sha256 %s)"
                  % (args.manifest, args.output, result["size"],
                     entry_address,
                     result["sha256"][:16]))
        elif args.mode == "validate":
            data = Path(args.image).read_bytes()
            validate_image(data)
            print("exepack: %s OK (%d bytes, sha256 %s)"
                  % (args.image, len(data),
                     hashlib.sha256(data).hexdigest()))
        elif args.mode == "dump":
            print(dump(args.image))
        else:
            print(get_field(args.manifest, args.field))
    except ExeError as exc:
        print("exepack: %s: %s" % (reported_name(exc.code), exc),
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
