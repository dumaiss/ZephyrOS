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


def main(argv):
    parser = argparse.ArgumentParser(
        prog="zexpack.py", description="ZephyrOS ZEX v1 packer/validator")
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
            print("zexpack: %s -> %s (%d bytes, entry %04Xh, sha256 %s)"
                  % (args.manifest, args.output, result["size"],
                     result["entry"]["address"],
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
