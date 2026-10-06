#!/usr/bin/env python3
"""ZephyrOS ZEX v1 native-executable packer, validator, and inspector.

ZEX v1 is the M6 single-bank native executable format: a packed 20-byte
little-endian header followed immediately by the image (see
docs/zex-format.md and include/zephyros/zex.h):

    00 magic[4]="ZEX1"   04 format_version=1   05 abi_version=1
    06 flags=0           07 reserved0=0
    08 header_size=20    0A image_size         0C bss_size
    0E entry_offset      10 stack_size         12 reserved1=0

A program is linked as one contiguous image at 0100h:

    _CODE = 0100h    code, constants, string literals (the image)
    _DATA = 0100h+image_size   zero-initialized data (BSS)

so the loader can copy the image byte-for-byte and zero the BSS without
running a C startup.  Initialized writable data is not part of ZEX v1:
the packer rejects linked content outside the declared image and rejects
initialized-data areas, keeping the load image deterministic.

Packing validates every geometry boundary before emitting a byte; the
result codes match the kernel reader exactly (include/zephyros/zex.h).

Usage:
    zexpack.py pack <manifest.json> <out.zex> [--objdir DIR]
                   [--report FILE]
    zexpack.py validate <file.zex>
    zexpack.py dump <file.zex>
    zexpack.py get <manifest.json> <field>

Manifest (zex-v1):

    {
      "format": "zex-v1",
      "name": "zextest.zex",
      "input": "zextest.ihx",
      "map": "zextest.map",
      "entry": "zt_entry",
      "entry_offset": 0,
      "image_size": 1792,
      "bss_size": 512,
      "stack_size": 256,
      "scratch": {"symbol": "zos_scratch", "size": 16}
    }

`input`/`map` resolve against --objdir (default: the manifest
directory).  `scratch` is optional but required by the provided user
runtime contract: it names the low-window syscall argument block and is
rejected when it cannot fit in 0100h-1FFFh.

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

MAGIC = b"ZEX1"
FORMAT_VERSION = 1
ABI_VERSION = 1
FLAGS_NONE = 0
HEADER_SIZE = 20
LOAD_BASE = 0x0100
STACK_TOP = 0xE000
MIN_STACK_SIZE = 2
SYSCALL_WINDOW_END = 0x2000
IMAGE_FILL = 0x00

# Result codes: kept value-identical with include/zephyros/zex.h; the
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
)
CODE_BY_NAME = dict(RESULT_CODES)

MANIFEST_KEYS = {
    "format", "name", "input", "map", "entry", "entry_offset",
    "image_size", "bss_size", "stack_size", "scratch",
}

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
    """A ZEX image or build violates the v1 contract.

    Carries the boundary name (a ZEX_EXEC_ERR_* key) so the packer, the
    validator, and the target tests all report the same reason.
    """

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


# --- header construction / parsing ---------------------------------------

def build_header(fields):
    """Serialize a header dict (raw values, no validation) to 20 bytes."""
    header = bytearray()
    header += fields["magic"]
    header.append(fields["format_version"] & 0xFF)
    header.append(fields["abi_version"] & 0xFF)
    header.append(fields["flags"] & 0xFF)
    header.append(fields["reserved0"] & 0xFF)
    header += (fields["header_size"] & 0xFFFF).to_bytes(2, "little")
    header += (fields["image_size"] & 0xFFFF).to_bytes(2, "little")
    header += (fields["bss_size"] & 0xFFFF).to_bytes(2, "little")
    header += (fields["entry_offset"] & 0xFFFF).to_bytes(2, "little")
    header += (fields["stack_size"] & 0xFFFF).to_bytes(2, "little")
    header += (fields["reserved1"] & 0xFFFF).to_bytes(2, "little")
    assert len(header) == HEADER_SIZE
    return bytes(header)


def base_fields():
    """A minimal well-formed field set (4-byte image: RET + 3 NOPs)."""
    return {
        "magic": MAGIC,
        "format_version": FORMAT_VERSION,
        "abi_version": ABI_VERSION,
        "flags": FLAGS_NONE,
        "reserved0": 0,
        "header_size": HEADER_SIZE,
        "image_size": 4,
        "bss_size": 4,
        "entry_offset": 0,
        "stack_size": 16,
        "reserved1": 0,
    }


def parse_header(data):
    """Parse the first 20 bytes; raises ZexError on a short file."""
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
        "image_size": data[10] | (data[11] << 8),
        "bss_size": data[12] | (data[13] << 8),
        "entry_offset": data[14] | (data[15] << 8),
        "stack_size": data[16] | (data[17] << 8),
        "reserved1": data[18] | (data[19] << 8),
    }


def validate_fields(fields, file_size):
    """Validate header fields against the total file size.

    Mirrors kernel/process/exec.c zex_validate_header() boundary by
    boundary and returns the ZEX_EXEC_ERR_* name of the first violation,
    or None when the geometry is valid.
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
    if file_size != fields["header_size"] + fields["image_size"]:
        return "ZEX_EXEC_ERR_FILESIZE"
    if fields["entry_offset"] >= fields["image_size"]:
        return "ZEX_EXEC_ERR_ENTRY"
    if fields["stack_size"] < MIN_STACK_SIZE:
        return "ZEX_EXEC_ERR_STACK"
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
    if data[:4] == ZEX2_MAGIC:
        return validate_v2(data), None
    fields = parse_header(data)
    code = validate_fields(fields, len(data))
    if code is not None:
        raise ZexError(code, "%s (image_size %d, file %d bytes)"
                       % (code, fields["image_size"], len(data)))
    return fields, geometry(fields)


def reported_name(code):
    """Human-readable boundary name for a ZEX_EXEC_ERR_* key."""
    return code if code.startswith("ZEX_") else str(code)


# --- manifest handling -----------------------------------------------------

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
    if manifest.get("format") != "zex-v1":
        raise ZexError("ZEX_EXEC_ERR_ARG",
                       "manifest format must be 'zex-v1'")
    for key in ("name", "input", "map", "entry", "entry_offset",
                "image_size", "bss_size", "stack_size"):
        if key not in manifest:
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "manifest is missing '%s'" % key)
    for key in ("entry_offset", "image_size", "bss_size", "stack_size"):
        value = manifest[key]
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
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
    return manifest


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
    if isinstance(raw, dict) and raw.get("format") == "zex-v2":
        return pack_v2(manifest_path, out_path, objdir=objdir,
                       report_path=report_path)
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

    # Declared image: every linked byte must be inside 0100h..0100h+size.
    outside = [addr for addr in sorted(mem)
               if addr < LOAD_BASE or addr >= image_end]
    if outside:
        raise ZexError(
            "ZEX_EXEC_ERR_ARG",
            "linked content at %04Xh lies outside the declared image "
            "%04Xh-%04Xh" % (outside[0], LOAD_BASE, image_end - 1))

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

    # BSS must begin exactly at the image end so the loader's zero fill
    # covers every writable static.  Initialized writable data has no
    # startup support in ZEX v1 and is rejected outright.
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
                           "initialized data is not supported by ZEX v1 "
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

    # The kernel repeats this geometry check; catching it here keeps a
    # bad link from ever reaching ROMFS.
    fields = base_fields()
    fields["image_size"] = image_size
    fields["bss_size"] = bss_size
    fields["entry_offset"] = entry_offset
    fields["stack_size"] = stack_size
    image = bytearray((IMAGE_FILL,) * image_size)
    for addr, byte in mem.items():
        image[addr - LOAD_BASE] = byte
    image = bytes(image)
    code = validate_fields(fields, HEADER_SIZE + image_size)
    if code is not None:
        raise ZexError(code, "declared geometry rejected: %s" % code)

    output = build_header(fields) + image
    out = Path(out_path)
    if out.parent and not out.parent.exists():
        out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(output)

    result = {
        "tool": "zexpack.py",
        "format": "zex-v1",
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
    }
    if report_path is not None:
        report = Path(report_path)
        if report.parent and not report.parent.exists():
            report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return result


def dump(path):
    data = Path(path).read_bytes()
    if data[:4] == ZEX2_MAGIC:
        return dump_v2(path, data)
    fields, geo = validate_image(data)
    image = data[HEADER_SIZE:HEADER_SIZE + fields["image_size"]]
    lines = [
        "zexpack: %s (%d bytes)" % (path, len(data)),
        "  magic %s  format %d  abi %d  flags %d"
        % (fields["magic"].decode("ascii", "replace"), fields["format_version"],
           fields["abi_version"], fields["flags"]),
        "  header_size %d  image_size %d  bss_size %d  entry_offset %d  "
        "stack_size %d  reserved0 %d  reserved1 %d"
        % (fields["header_size"], fields["image_size"], fields["bss_size"],
           fields["entry_offset"], fields["stack_size"], fields["reserved0"],
           fields["reserved1"]),
        "  entry %04Xh  image %04Xh-%04Xh  bss %04Xh-%04Xh  stack %04Xh-%04Xh"
        % (geo["entry"], LOAD_BASE, LOAD_BASE + fields["image_size"] - 1,
           LOAD_BASE + fields["image_size"],
           LOAD_BASE + fields["image_size"] + fields["bss_size"] - 1,
           geo["stack_base"], STACK_TOP - 1),
        "  image sha256 %s" % hashlib.sha256(image).hexdigest(),
        "  file  sha256 %s" % hashlib.sha256(data).hexdigest(),
    ]
    return "\n".join(lines)


def get_field(manifest_path, field):
    manifest = load_manifest(manifest_path)
    if field not in ("image_size", "bss_size", "stack_size"):
        raise ZexError("ZEX_EXEC_ERR_ARG",
                       "get supports image_size, bss_size, stack_size")
    return int(manifest[field])


# ---------------------------------------------------------------------------
# ZEX v2 (M10 design spike)
# ---------------------------------------------------------------------------
#
# ZEX v2 is host-precomputed: this packer resolves every address, encodes
# every ABS16 relocation as a single high-byte patch offset, pre-resolves
# the far-entry table and proves every container bound, so the resident
# loader (kernel/process/exec.c under ZOS_M10_SPIKE) only reads
# descriptors and applies patches.  See include/zephyros/zex2.h.

ZEX2_MAGIC = b"ZEX2"
ZEX2_FORMAT_VERSION = 2
ZEX2_ABI_VERSION = 1
ZEX2_FLAG_NONE = 0
ZEX2_HEADER_SIZE = 40
ZEX2_SEGMENT_ENTRY_SIZE = 20
ZEX2_FAR_ENTRY_SIZE = 4
ZEX2_LOAD_BASE = LOAD_BASE
ZEX2_STACK_TOP = STACK_TOP
ZEX2_AUX_SLOT_SIZE = 0x2000
ZEX2_AUX_SLOT_BASE = 0x2000
ZEX2_AUX_SLOT_COUNT = 6
ZEX2_AUX_SLOT_LIMIT = 0xE000
ZEX2_MAX_SEGMENTS = 6
ZEX2_MAX_FAR_ENTRIES = 16
ZEX2_SEG_FLAG_EXEC = 0x01
ZEX2_SEG_FLAG_WRITE = 0x02
ZEX2_SEG_FLAG_MASK = ZEX2_SEG_FLAG_EXEC | ZEX2_SEG_FLAG_WRITE

# Result codes: value-identical with include/zephyros/zex2.h.
ZEX2_RESULT_CODES = (
    ("ZEX2_EXEC_ERR_SEG_COUNT", -0x41),
    ("ZEX2_EXEC_ERR_FAR_COUNT", -0x42),
    ("ZEX2_EXEC_ERR_SLOTS", -0x43),
    ("ZEX2_EXEC_ERR_SEGMENT", -0x44),
    ("ZEX2_EXEC_ERR_RELOC", -0x45),
    ("ZEX2_EXEC_ERR_FAR", -0x46),
)

_V1_PRIMARY_KEYS = {"input", "map", "entry", "entry_offset", "image_size",
                    "bss_size", "stack_size", "scratch"}
_SEGMENT_KEYS = {"id", "flags", "objects", "image_areas", "bss_areas",
                 "far_entries"}


def _zex2_name(code):
    for name, value in ZEX2_RESULT_CODES:
        if value == code:
            return name
    return reported_name(code)


def load_manifest_v2(path):
    try:
        manifest = json.loads(Path(path).read_text())
    except OSError as exc:
        raise ZexError("ZEX_EXEC_ERR_ARG", "cannot read manifest: %s" % exc)
    except ValueError as exc:
        raise ZexError("ZEX_EXEC_ERR_ARG",
                       "manifest is not valid JSON: %s" % exc)
    if not isinstance(manifest, dict) or manifest.get("format") != "zex-v2":
        raise ZexError("ZEX_EXEC_ERR_ARG", "manifest format must be 'zex-v2'")
    unknown = set(manifest) - {"format", "name", "primary", "segments"}
    if unknown:
        raise ZexError("ZEX_EXEC_ERR_ARG",
                       "unknown manifest keys: %s" % ", ".join(sorted(unknown)))
    for key in ("name", "primary"):
        if key not in manifest:
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "manifest is missing '%s'" % key)
    primary = manifest["primary"]
    if not isinstance(primary, dict):
        raise ZexError("ZEX_EXEC_ERR_ARG", "'primary' must be an object")
    p_unknown = set(primary) - _V1_PRIMARY_KEYS
    if p_unknown:
        raise ZexError("ZEX_EXEC_ERR_ARG",
                       "unknown primary keys: %s" % ", ".join(sorted(p_unknown)))
    for key in ("input", "map", "entry", "entry_offset", "image_size",
                "bss_size", "stack_size"):
        if key not in primary:
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "primary is missing '%s'" % key)
    for key in ("entry_offset", "image_size", "bss_size", "stack_size"):
        value = primary[key]
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "primary '%s' must be a non-negative integer" % key)

    segments = manifest.get("segments", [])
    if not isinstance(segments, list):
        raise ZexError("ZEX_EXEC_ERR_ARG", "'segments' must be a list")
    if len(segments) > ZEX2_MAX_SEGMENTS:
        raise ZexError("ZEX2_EXEC_ERR_SEG_COUNT",
                       "%d segments exceed the %d-segment maximum"
                       % (len(segments), ZEX2_MAX_SEGMENTS))
    seen_ids = set()
    for seg in segments:
        if not isinstance(seg, dict):
            raise ZexError("ZEX_EXEC_ERR_ARG", "each segment must be an object")
        s_unknown = set(seg) - _SEGMENT_KEYS
        if s_unknown:
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "unknown segment keys: %s" % ", ".join(sorted(s_unknown)))
        for key in ("id", "objects", "image_areas"):
            if key not in seg:
                raise ZexError("ZEX_EXEC_ERR_ARG",
                               "segment is missing '%s'" % key)
        if not isinstance(seg["id"], int) or not 1 <= seg["id"] <= 0xFF:
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "segment id must be 1..255")
        if seg["id"] in seen_ids:
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "duplicate segment id %d" % seg["id"])
        seen_ids.add(seg["id"])
        if not seg["objects"] or not isinstance(seg["objects"], list):
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "segment %d has no objects" % seg["id"])
        if not seg["image_areas"] or not isinstance(seg["image_areas"], list):
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "segment %d has no image_areas" % seg["id"])
        flags = seg.get("flags", [])
        if not isinstance(flags, list) or set(flags) - {"EXEC", "WRITE"}:
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "segment %d flags must be EXEC/WRITE" % seg["id"])
    return manifest


def _zex2_segment_flags(names):
    value = 0
    for name in names:
        if name == "EXEC":
            value |= ZEX2_SEG_FLAG_EXEC
        elif name == "WRITE":
            value |= ZEX2_SEG_FLAG_WRITE
    return value


def _zex2_link_segment(seg, manifest_path, objdir):
    """Link one auxiliary segment as a relocatable image at logical 0000h.

    Reuses tools/zmodpack.py's SDCC .rel parser and symbol resolver (the
    same mini-link the ZMOD loader proves out) but applies its own
    relocation loop, because a segment has no init/fini entry and its
    patch sites are emitted as single-byte high-byte offsets rather than
    ZMOD's ZREL_ABS16 records.
    """
    import zmodpack

    objects = []
    for name in seg["objects"]:
        obj_path = _resolve(name, manifest_path, objdir)
        if not obj_path.is_file():
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "segment %d object not found: %s"
                           % (seg["id"], obj_path))
        try:
            objects.append(zmodpack.parse_rel(obj_path.read_text(),
                                              str(obj_path)))
        except zmodpack.ZmodError as exc:
            raise ZexError("ZEX2_EXEC_ERR_SEGMENT", str(exc))

    mini = {
        "name": "seg%d" % seg["id"],
        "objects": objects,
        "image_areas": list(seg["image_areas"]),
        "bss_areas": list(seg.get("bss_areas", [])),
        "path": manifest_path,
    }
    # SDCC emits every area in every object, including empty ones; a
    # declared area that is empty across all objects would make the
    # mini-linker reject the segment, so keep only the non-empty ones.
    present = {}
    for obj in objects:
        for area in obj.areas:
            if area.size > 0 and (area.flags & 0x08) == 0:
                present[area.name] = present.get(area.name, 0) + area.size
    mini["image_areas"] = [a for a in mini["image_areas"] if present.get(a, 0)]
    mini["bss_areas"] = [a for a in mini["bss_areas"] if present.get(a, 0)]
    try:
        layout = zmodpack.build_layout(mini)
        symbols = zmodpack.build_symbol_table(mini)
        image_size = layout["image_size"]
        image = bytearray(image_size)
        reloc_sites = set()

        for obj_index, obj in enumerate(objects):
            for chunk in obj.chunks:
                if chunk.area_index >= len(obj.areas):
                    raise zmodpack.ZmodError(
                        "%s: chunk references area %d out of range"
                        % (obj.path, chunk.area_index))
                area = obj.areas[chunk.area_index]
                key = (obj_index, area.name)
                if key not in layout["instance_offset"]:
                    raise zmodpack.ZmodError(
                        "%s: chunk in unlisted area %s"
                        % (obj.path, area.name))
                if area.name not in mini["image_areas"]:
                    raise zmodpack.ZmodError(
                        "%s: BSS area %s contains initialized data"
                        % (obj.path, area.name))
                base = layout["instance_offset"][key]
                start = base + chunk.addr
                end = start + len(chunk.data)
                area_limit = layout["area_base"][area.name] + \
                    layout["area_size"][area.name]
                if end > area_limit:
                    raise zmodpack.ZmodError(
                        "%s: chunk at %04Xh overruns area %s"
                        % (obj.path, chunk.addr, area.name))
                written = set(range(start, end))
                for site in list(reloc_sites):
                    covered = (site in written) + (site + 1 in written)
                    if covered == 2:
                        reloc_sites.discard(site)
                    elif covered == 1:
                        raise zmodpack.ZmodError(
                            "%s: chunk partially overwrites relocation site "
                            "at %04Xh" % (obj.path, site))
                image[start:end] = chunk.data
                for mode, rtp, ref in chunk.relocs:
                    if mode not in zmodpack.SUPPORTED_MODES:
                        raise zmodpack.ZmodError(
                            "%s: unsupported relocation mode %02Xh"
                            % (obj.path, mode))
                    if rtp < zmodpack.A_BYTES:
                        raise zmodpack.ZmodError(
                            "%s: relocation targets the chunk address field"
                            % obj.path)
                    index = rtp - zmodpack.A_BYTES
                    if index + 2 > len(chunk.data):
                        raise zmodpack.ZmodError(
                            "%s: relocation outside the T data" % obj.path)
                    patch = start + index
                    if patch + 2 > image_size:
                        raise zmodpack.ZmodError(
                            "%s: patch at %04Xh crosses the image end"
                            % (obj.path, patch))
                    target = zmodpack.resolve_relocation(
                        mini, layout, symbols, obj_index, mode, ref)
                    addend = image[patch] | (image[patch + 1] << 8)
                    value = target + addend
                    if value > 0xFFFF:
                        raise zmodpack.ZmodError(
                            "%s: relocation at %04Xh overflows 16 bits"
                            % (obj.path, patch))
                    image[patch] = value & 0xFF
                    image[patch + 1] = (value >> 8) & 0xFF
                    reloc_sites.add(patch)

        bss_size = layout["total_size"] - image_size
        far_entries = []
        for entry in seg.get("far_entries", []):
            if not isinstance(entry, dict) or set(entry) != {"id", "symbol"}:
                raise ZexError("ZEX_EXEC_ERR_ARG",
                               "segment %d far entry must be {id, symbol}"
                               % seg["id"])
            offset = zmodpack.symbol_offset(mini, layout, symbols,
                                            entry["symbol"])
            if offset >= image_size:
                raise ZexError("ZEX2_EXEC_ERR_FAR",
                               "segment %d far entry %s at %04Xh is outside "
                               "the %d-byte image"
                               % (seg["id"], entry["symbol"], offset,
                                  image_size))
            far_entries.append((entry["id"], offset))
    except zmodpack.ZmodError as exc:
        raise ZexError("ZEX2_EXEC_ERR_SEGMENT", str(exc))

    return {
        "id": seg["id"],
        "flags": _zex2_segment_flags(seg.get("flags", [])),
        "image": bytes(image),
        "bss_size": bss_size,
        "relocs": sorted(reloc_sites),
        "far_entries": far_entries,
    }


def _zex2_primary_geometry(primary):
    """Return a ZEX_EXEC_ERR_* name for a bad primary geometry, or None."""
    image_size = primary["image_size"]
    bss_size = primary["bss_size"]
    stack_size = primary["stack_size"]
    if image_size == 0:
        return "ZEX_EXEC_ERR_IMAGE"
    if primary["entry_offset"] >= image_size:
        return "ZEX_EXEC_ERR_ENTRY"
    if stack_size < MIN_STACK_SIZE:
        return "ZEX_EXEC_ERR_STACK"
    image_end = ZEX2_LOAD_BASE + image_size
    if image_end > ZEX2_STACK_TOP:
        return "ZEX_EXEC_ERR_IMAGE_END"
    bss_end = image_end + bss_size
    if bss_end > ZEX2_STACK_TOP:
        return "ZEX_EXEC_ERR_BSS_END"
    stack_base = ZEX2_STACK_TOP - stack_size
    if stack_base < ZEX2_LOAD_BASE or bss_end > stack_base:
        return "ZEX_EXEC_ERR_COLLISION"
    return None


def _zex2_primary_payload(primary, manifest_path, objdir):
    """Validate the primary link and return (image_bytes, report)."""
    ihx_path = _resolve(primary["input"], manifest_path, objdir)
    map_path = _resolve(primary["map"], manifest_path, objdir)
    if not ihx_path.is_file():
        raise ZexError("ZEX_EXEC_ERR_ARG", "input not found: %s" % ihx_path)
    if not map_path.is_file():
        raise ZexError("ZEX_EXEC_ERR_ARG", "map not found: %s" % map_path)
    try:
        mem = pack_image.parse_ihx(ihx_path.read_text())
    except pack_image.LayoutError as exc:
        raise ZexError("ZEX_EXEC_ERR_ARG", "IHX rejected: %s" % exc)
    symbols = parse_symbols(map_path.read_text())

    image_size = primary["image_size"]
    image_end = ZEX2_LOAD_BASE + image_size
    outside = [addr for addr in sorted(mem)
               if addr < ZEX2_LOAD_BASE or addr >= image_end]
    if outside:
        raise ZexError(
            "ZEX_EXEC_ERR_ARG",
            "linked content at %04Xh lies outside the declared primary "
            "image %04Xh-%04Xh" % (outside[0], ZEX2_LOAD_BASE, image_end - 1))

    entry_name = primary["entry"]
    if entry_name not in symbols:
        raise ZexError("ZEX_EXEC_ERR_ARG",
                       "map has no entry symbol %s" % entry_name)
    entry = symbols[entry_name]
    if entry != ZEX2_LOAD_BASE + primary["entry_offset"]:
        raise ZexError("ZEX_EXEC_ERR_ARG",
                       "entry %s is at %04Xh, declared offset says %04Xh"
                       % (entry_name, entry,
                          ZEX2_LOAD_BASE + primary["entry_offset"]))
    if symbols.get("s__DATA") != image_end:
        raise ZexError("ZEX_EXEC_ERR_ARG",
                       "BSS base s__DATA is %04Xh, expected image end %04Xh"
                       % (symbols.get("s__DATA", -1), image_end))
    if symbols.get("l__DATA", 0) > primary["bss_size"]:
        raise ZexError("ZEX_EXEC_ERR_ARG",
                       "BSS is %d bytes, declared bss_size is %d"
                       % (symbols.get("l__DATA", 0), primary["bss_size"]))
    for area in ("INITIALIZER", "INITIALIZED"):
        if symbols.get("l__" + area, 0) != 0:
            raise ZexError("ZEX_EXEC_ERR_ARG",
                           "initialized data is not supported in the primary "
                           "image (_%s is non-empty)" % area)

    image = bytearray((IMAGE_FILL,) * image_size)
    for addr, byte in mem.items():
        image[addr - ZEX2_LOAD_BASE] = byte
    return bytes(image), entry_name, entry


def pack_v2(manifest_path, out_path, objdir=None, report_path=None):
    manifest = load_manifest_v2(manifest_path)
    primary = manifest["primary"]
    code = _zex2_primary_geometry(primary)
    if code is not None:
        raise ZexError(code, "primary geometry rejected: %s" % code)
    primary_image, entry_name, entry = _zex2_primary_payload(
        primary, manifest_path, objdir)

    segments = [_zex2_link_segment(seg, manifest_path, objdir)
                for seg in manifest.get("segments", [])]

    # Far-entry table: ids are ZEX-local and must be unique; every entry
    # resolves inside its segment and names a segment that exists.
    seg_by_id = {seg["id"]: seg for seg in segments}
    far_table = []
    seen_far = set()
    for seg in segments:
        for entry_id, offset in seg["far_entries"]:
            if entry_id in seen_far:
                raise ZexError("ZEX2_EXEC_ERR_FAR",
                               "duplicate far-entry id %d" % entry_id)
            seen_far.add(entry_id)
            far_table.append((entry_id, seg["id"], offset))
    if len(far_table) > ZEX2_MAX_FAR_ENTRIES:
        raise ZexError("ZEX2_EXEC_ERR_FAR_COUNT",
                       "%d far entries exceed the %d-entry maximum"
                       % (len(far_table), ZEX2_MAX_FAR_ENTRIES))

    # Slot accounting: each segment is slot-rounded; the loader's
    # header-only feasibility check uses total_slots.
    total_slots = 0
    for seg in segments:
        total = len(seg["image"]) + seg["bss_size"]
        slots = (total + ZEX2_AUX_SLOT_SIZE - 1) // ZEX2_AUX_SLOT_SIZE
        if slots == 0:
            raise ZexError("ZEX2_EXEC_ERR_SEGMENT",
                           "segment %d is empty" % seg["id"])
        if slots > ZEX2_AUX_SLOT_COUNT:
            raise ZexError("ZEX2_EXEC_ERR_SEGMENT",
                           "segment %d needs %d slots; one auxiliary bank "
                           "holds %d" % (seg["id"], slots,
                                         ZEX2_AUX_SLOT_COUNT))
        seg["slots"] = slots
        seg["alloc_size"] = slots * ZEX2_AUX_SLOT_SIZE
        total_slots += slots
    if total_slots > 0xFF:
        raise ZexError("ZEX2_EXEC_ERR_SLOTS",
                       "total_slots %d overflows the header" % total_slots)

    # File layout: header, primary payload, segment table, far-entry
    # table, segment payloads, relocation streams.
    offset = ZEX2_HEADER_SIZE
    primary_payload_offset = offset
    offset += len(primary_image)
    segment_table_offset = offset
    offset += ZEX2_SEGMENT_ENTRY_SIZE * len(segments)
    far_entry_table_offset = offset
    offset += ZEX2_FAR_ENTRY_SIZE * len(far_table)
    for seg in segments:
        seg["payload_offset"] = offset
        offset += len(seg["image"])
    for seg in segments:
        seg["reloc_offset"] = offset
        offset += 2 * len(seg["relocs"])
    file_size = offset
    if file_size > 0xFFFFFFFF:
        raise ZexError("ZEX_EXEC_ERR_ARG", "container exceeds 32-bit geometry")

    header = bytearray()
    header += ZEX2_MAGIC
    header.append(ZEX2_FORMAT_VERSION)
    header.append(ZEX2_ABI_VERSION)
    header.append(ZEX2_FLAG_NONE)
    header.append(0)  # reserved0
    header += ZEX2_HEADER_SIZE.to_bytes(2, "little")
    header.append(len(segments) & 0xFF)
    header.append(len(far_table) & 0xFF)
    header += primary["image_size"].to_bytes(2, "little")
    header += primary["bss_size"].to_bytes(2, "little")
    header += primary["entry_offset"].to_bytes(2, "little")
    header += primary["stack_size"].to_bytes(2, "little")
    header += total_slots.to_bytes(2, "little")
    header += file_size.to_bytes(4, "little")
    header += primary_payload_offset.to_bytes(4, "little")
    header += segment_table_offset.to_bytes(4, "little")
    header += far_entry_table_offset.to_bytes(4, "little")
    header += (0).to_bytes(2, "little")  # reserved1
    assert len(header) == ZEX2_HEADER_SIZE

    table = bytearray()
    for seg in segments:
        table.append(seg["id"])
        table.append(seg["flags"])
        table += len(seg["image"]).to_bytes(2, "little")
        table += seg["bss_size"].to_bytes(2, "little")
        table += seg["alloc_size"].to_bytes(2, "little")
        table += seg["payload_offset"].to_bytes(4, "little")
        table += seg["reloc_offset"].to_bytes(4, "little")
        table += len(seg["relocs"]).to_bytes(2, "little")
        table += (0).to_bytes(2, "little")
        assert len(seg["image"]) + seg["bss_size"] <= seg["alloc_size"]

    far_bytes = bytearray()
    for entry_id, segment_id, entry_offset in far_table:
        far_bytes.append(entry_id)
        far_bytes.append(segment_id)
        far_bytes += entry_offset.to_bytes(2, "little")

    payloads = bytearray()
    for seg in segments:
        payloads += seg["image"]
    relocs = bytearray()
    for seg in segments:
        for site in seg["relocs"]:
            if site + 1 >= len(seg["image"]):
                raise ZexError("ZEX2_EXEC_ERR_RELOC",
                               "segment %d patch %04Xh crosses the image end"
                               % (seg["id"], site))
            relocs += site.to_bytes(2, "little")

    output = bytes(header + primary_image + table + far_bytes + payloads +
                   relocs)
    assert len(output) == file_size

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
        "header": {
            "header_size": ZEX2_HEADER_SIZE,
            "segment_count": len(segments),
            "far_entry_count": len(far_table),
            "primary_image_size": primary["image_size"],
            "primary_bss_size": primary["bss_size"],
            "primary_entry_offset": primary["entry_offset"],
            "stack_size": primary["stack_size"],
            "total_slots": total_slots,
            "file_size": file_size,
            "primary_payload_offset": primary_payload_offset,
            "segment_table_offset": segment_table_offset,
            "far_entry_table_offset": far_entry_table_offset,
        },
        "primary": {
            "entry_symbol": entry_name,
            "entry_address": entry,
            "image_sha256": hashlib.sha256(primary_image).hexdigest(),
        },
        "segments": [
            {
                "id": seg["id"],
                "flags": seg["flags"],
                "image_size": len(seg["image"]),
                "bss_size": seg["bss_size"],
                "alloc_size": seg["alloc_size"],
                "slots": seg["slots"],
                "reloc_count": len(seg["relocs"]),
                "payload_offset": seg["payload_offset"],
                "reloc_offset": seg["reloc_offset"],
                "image_sha256": hashlib.sha256(seg["image"]).hexdigest(),
            }
            for seg in segments
        ],
        "far_entries": [
            {"id": entry_id, "segment_id": segment_id, "offset": entry_offset}
            for entry_id, segment_id, entry_offset in far_table
        ],
    }
    if report_path is not None:
        report = Path(report_path)
        if report.parent and not report.parent.exists():
            report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return result


def parse_header_v2(data):
    if len(data) < ZEX2_HEADER_SIZE:
        raise ZexError("ZEX_EXEC_ERR_FILESIZE",
                       "file is %d bytes, shorter than the %d-byte header"
                       % (len(data), ZEX2_HEADER_SIZE))

    def u16(at):
        return data[at] | (data[at + 1] << 8)

    def u32(at):
        return (data[at] | (data[at + 1] << 8) | (data[at + 2] << 16)
                | (data[at + 3] << 24))

    return {
        "magic": bytes(data[0:4]),
        "format_version": data[4],
        "abi_version": data[5],
        "flags": data[6],
        "reserved0": data[7],
        "header_size": u16(0x08),
        "segment_count": data[0x0A],
        "far_entry_count": data[0x0B],
        "primary_image_size": u16(0x0C),
        "primary_bss_size": u16(0x0E),
        "primary_entry_offset": u16(0x10),
        "stack_size": u16(0x12),
        "total_slots": u16(0x14),
        "file_size": u32(0x16),
        "primary_payload_offset": u32(0x1A),
        "segment_table_offset": u32(0x1E),
        "far_entry_table_offset": u32(0x22),
        "reserved1": u16(0x26),
    }


def validate_v2(data):
    """Validate a complete ZEX v2 container; returns a field dict."""
    fields = parse_header_v2(data)
    if fields["magic"] != ZEX2_MAGIC:
        raise ZexError("ZEX_EXEC_ERR_MAGIC", "magic is not ZEX2")
    if fields["format_version"] != ZEX2_FORMAT_VERSION:
        raise ZexError("ZEX_EXEC_ERR_FORMAT", "format_version is not 2")
    if fields["abi_version"] != ZEX2_ABI_VERSION:
        raise ZexError("ZEX_EXEC_ERR_ABI", "abi_version is not 1")
    if fields["flags"] != ZEX2_FLAG_NONE:
        raise ZexError("ZEX_EXEC_ERR_FLAGS", "flags must be zero")
    if fields["reserved0"] != 0 or fields["reserved1"] != 0:
        raise ZexError("ZEX_EXEC_ERR_RESERVED0", "reserved fields must be zero")
    if fields["header_size"] != ZEX2_HEADER_SIZE:
        raise ZexError("ZEX_EXEC_ERR_HEADER", "header_size is not 40")
    if fields["file_size"] != len(data):
        raise ZexError("ZEX_EXEC_ERR_FILESIZE",
                       "file_size %d does not match %d bytes"
                       % (fields["file_size"], len(data)))
    if fields["segment_count"] > ZEX2_MAX_SEGMENTS:
        raise ZexError("ZEX2_EXEC_ERR_SEG_COUNT",
                       "%d segments exceed the maximum"
                       % fields["segment_count"])
    if fields["far_entry_count"] > ZEX2_MAX_FAR_ENTRIES:
        raise ZexError("ZEX2_EXEC_ERR_FAR_COUNT",
                       "%d far entries exceed the maximum"
                       % fields["far_entry_count"])

    code = _zex2_primary_geometry({
        "image_size": fields["primary_image_size"],
        "bss_size": fields["primary_bss_size"],
        "stack_size": fields["stack_size"],
        "entry_offset": fields["primary_entry_offset"],
    })
    if code is not None:
        raise ZexError(code, "primary geometry rejected: %s" % code)

    def u16(at):
        return data[at] | (data[at + 1] << 8)

    def u32(at):
        return (data[at] | (data[at + 1] << 8) | (data[at + 2] << 16)
                | (data[at + 3] << 24))

    segments = []
    expected_slots = 0
    for i in range(fields["segment_count"]):
        base = fields["segment_table_offset"] + i * ZEX2_SEGMENT_ENTRY_SIZE
        if base + ZEX2_SEGMENT_ENTRY_SIZE > len(data):
            raise ZexError("ZEX2_EXEC_ERR_SEGMENT",
                           "segment table entry %d is outside the file" % i)
        seg = {
            "id": data[base],
            "flags": data[base + 1],
            "image_size": u16(base + 2),
            "bss_size": u16(base + 4),
            "alloc_size": u16(base + 6),
            "payload_offset": u32(base + 8),
            "reloc_offset": u32(base + 12),
            "reloc_count": u16(base + 16),
            "reserved": u16(base + 18),
        }
        if seg["flags"] & ~ZEX2_SEG_FLAG_MASK:
            raise ZexError("ZEX2_EXEC_ERR_SEGMENT",
                           "segment %d has unknown flags" % seg["id"])
        if seg["alloc_size"] == 0 or \
                seg["alloc_size"] % ZEX2_AUX_SLOT_SIZE != 0:
            raise ZexError("ZEX2_EXEC_ERR_SEGMENT",
                           "segment %d alloc_size is not slot-aligned"
                           % seg["id"])
        slots = seg["alloc_size"] // ZEX2_AUX_SLOT_SIZE
        if seg["image_size"] + seg["bss_size"] > seg["alloc_size"]:
            raise ZexError("ZEX2_EXEC_ERR_SEGMENT",
                           "segment %d image+BSS exceeds alloc_size"
                           % seg["id"])
        slots = max(slots, 1)
        if slots > ZEX2_AUX_SLOT_COUNT:
            raise ZexError("ZEX2_EXEC_ERR_SEGMENT",
                           "segment %d needs more than one bank" % seg["id"])
        expected_slots += slots
        if seg["payload_offset"] + seg["image_size"] > len(data):
            raise ZexError("ZEX2_EXEC_ERR_SEGMENT",
                           "segment %d payload is outside the file" % seg["id"])
        if seg["reloc_offset"] + 2 * seg["reloc_count"] > len(data):
            raise ZexError("ZEX2_EXEC_ERR_RELOC",
                           "segment %d reloc stream is outside the file"
                           % seg["id"])
        for j in range(seg["reloc_count"]):
            site = u16(seg["reloc_offset"] + 2 * j)
            if site + 1 >= seg["image_size"]:
                raise ZexError("ZEX2_EXEC_ERR_RELOC",
                               "segment %d patch %04Xh crosses the image end"
                               % (seg["id"], site))
        segments.append(seg)
    if expected_slots != fields["total_slots"]:
        raise ZexError("ZEX2_EXEC_ERR_SLOTS",
                       "total_slots %d does not match the segment table's %d"
                       % (fields["total_slots"], expected_slots))

    far = []
    for i in range(fields["far_entry_count"]):
        base = fields["far_entry_table_offset"] + i * ZEX2_FAR_ENTRY_SIZE
        if base + ZEX2_FAR_ENTRY_SIZE > len(data):
            raise ZexError("ZEX2_EXEC_ERR_FAR",
                           "far-entry table entry %d is outside the file" % i)
        entry = {"id": data[base], "segment_id": data[base + 1],
                 "offset": u16(base + 2)}
        seg = next((s for s in segments if s["id"] == entry["segment_id"]),
                   None)
        if seg is None:
            raise ZexError("ZEX2_EXEC_ERR_FAR",
                           "far entry %d names missing segment %d"
                           % (entry["id"], entry["segment_id"]))
        if entry["offset"] >= seg["image_size"]:
            raise ZexError("ZEX2_EXEC_ERR_FAR",
                           "far entry %d offset %04Xh is outside segment %d"
                           % (entry["id"], entry["offset"], seg["id"]))
        far.append(entry)

    fields["segments"] = segments
    fields["far_entries"] = far
    return fields


def dump_v2(path, data):
    fields = validate_v2(data)
    lines = [
        "zexpack: %s (ZEX v2, %d bytes)" % (path, len(data)),
        "  format %d  abi %d  segments %d  far entries %d  total slots %d"
        % (fields["format_version"], fields["abi_version"],
           fields["segment_count"], fields["far_entry_count"],
           fields["total_slots"]),
        "  primary image %d  bss %d  entry_offset %d  stack %d"
        % (fields["primary_image_size"], fields["primary_bss_size"],
           fields["primary_entry_offset"], fields["stack_size"]),
    ]
    for seg in fields["segments"]:
        lines.append(
            "  segment %d flags %02X image %d bss %d slots %d relocs %d"
            % (seg["id"], seg["flags"], seg["image_size"], seg["bss_size"],
               seg["alloc_size"] // ZEX2_AUX_SLOT_SIZE, seg["reloc_count"]))
    for entry in fields["far_entries"]:
        lines.append("  far entry %d -> segment %d +%04Xh"
                     % (entry["id"], entry["segment_id"], entry["offset"]))
    lines.append("  file  sha256 %s" % hashlib.sha256(data).hexdigest())
    return "\n".join(lines)


def main(argv):
    parser = argparse.ArgumentParser(
        prog="zexpack.py", description="ZephyrOS ZEX v1/v2 packer/validator")
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
            if result["format"] == "zex-v2":
                entry_address = result["primary"]["entry_address"]
            else:
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
