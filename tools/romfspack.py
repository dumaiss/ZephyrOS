#!/usr/bin/env python3
"""ZephyrOS ROMFS v1 recovery-image packer, validator, and inspector.

ROMFS is the tiny read-only recovery store placed in ROM pages 2-7 by
tools/pack_image.py (see docs/romfs-format.md).  It is built from a JSON
manifest:

    {
      "files": [
        {"name": "sio.zmod", "type": "ZMOD", "path": "sio.zmod"}
      ]
    }

Input paths resolve against --objdir (default: the manifest directory).
The on-disk layout is little-endian:

    16-byte header   magic[4]="ZRFS", version:u8=1, entry_size:u8=36,
                     file_count:u16, directory_size:u16, reserved:u16=0,
                     image_size:u32
    36-byte entries  name[24] (NUL-terminated, zero-padded),
                     logical_offset:u32, size:u32, flags:u16, reserved:u16=0
    payloads         packed in manifest order without alignment gaps

`logical_offset` is measured across the concatenated 0000h-BFFFh windows
of the configured recovery pages; the reader maps logical byte L to
(page = first_page + L / window, offset = L % window).  The C000h-FFFFh
half of each recovery page is never part of the stream.

Flags: ZMOD=1, ZEX=2, DATA=3.

Usage:
    romfspack.py pack <manifest.json> <recovery.romfs>
                      [--objdir DIR] [--memory-inc PATH]
    romfspack.py validate <recovery.romfs> [--memory-inc PATH]
    romfspack.py dump <recovery.romfs> [--memory-inc PATH]

The default geometry comes from platform/zephyr80/memory.inc so the host
tools and the target cannot drift.  Exit status: 0 success, 1 rejected
image/manifest, 2 usage error.
"""

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

MAGIC = b"ZRFS"
VERSION = 1
HEADER_SIZE = 16
ENTRY_SIZE = 36
NAME_SIZE = 24

FLAG_ZMOD = 1
FLAG_ZEX = 2
FLAG_DATA = 3
FLAG_NAMES = {FLAG_ZMOD: "ZMOD", FLAG_ZEX: "ZEX", FLAG_DATA: "DATA"}
TYPE_FLAGS = {"ZMOD": FLAG_ZMOD, "ZEX": FLAG_ZEX, "DATA": FLAG_DATA}

U32_MAX = 0xFFFFFFFF

DEFAULT_MEMORY_INC = (
    Path(__file__).resolve().parent.parent / "platform" / "zephyr80" / "memory.inc"
)

# Geometry defaults; overridden from the platform declarations.  The
# values are declarative and must match platform/zephyr80/memory.h.
DEFAULT_GEOMETRY = {
    "page_size": 0x10000,
    "first_page": 2,
    "page_count": 6,
    "window_size": 0xC000,
    "capacity": 0x48000,
}


class RomfsError(Exception):
    """The manifest or recovery image violates the ROMFS v1 contract."""


def load_geometry(path=None):
    """Read the ROMFS page geometry from platform/zephyr80/memory.inc."""
    geom = dict(DEFAULT_GEOMETRY)
    path = Path(path) if path else DEFAULT_MEMORY_INC
    if not path.exists():
        raise RomfsError("geometry file not found: %s" % path)
    values = {}
    for line in path.read_text().splitlines():
        line = line.split(";")[0].strip()
        match = re.match(r"([A-Za-z_]\w*)\s+=\s+(0[xX][0-9a-fA-F]+|\d+)$", line)
        if match:
            values[match.group(1)] = int(match.group(2), 0)
    try:
        geom["page_size"] = values["ZOS_ROM_PAGE_SIZE"]
        geom["first_page"] = values["ZOS_ROMFS_FIRST_PAGE"]
        geom["page_count"] = values["ZOS_ROMFS_PAGE_COUNT"]
        geom["window_size"] = values["ZOS_ROMFS_WINDOW_SIZE"]
        geom["capacity"] = values["ZOS_ROMFS_CAPACITY"]
    except KeyError as exc:
        raise RomfsError("memory.inc is missing %s" % exc)
    if geom["capacity"] != geom["page_count"] * geom["window_size"]:
        raise RomfsError("ROMFS capacity does not match page count x window")
    if geom["window_size"] >= geom["page_size"]:
        raise RomfsError("ROMFS window must leave the C000h-FFFFh half unused")
    return geom


def logical_to_physical(logical, geom):
    """Map one logical stream byte to (page, offset-in-window, window-index)."""
    if logical < 0 or logical >= geom["capacity"]:
        raise RomfsError("logical offset %d outside the ROMFS capacity" % logical)
    window = logical // geom["window_size"]
    offset = logical % geom["window_size"]
    if window >= geom["page_count"]:
        raise RomfsError("logical offset %d has no recovery page" % logical)
    return geom["first_page"] + window, offset


def physical_ranges(logical, size, geom):
    """Yield (page, window_start, length) for a logical range."""
    while size > 0:
        page, offset = logical_to_physical(logical, geom)
        chunk = min(size, geom["window_size"] - offset)
        yield page, offset, chunk
        logical += chunk
        size -= chunk


def format_physical(logical, size, geom):
    """Human-readable physical ROM placement of a logical range."""
    parts = []
    for page, offset, chunk in physical_ranges(logical, size, geom):
        parts.append("page %d %04Xh-%04Xh" % (page, offset, offset + chunk - 1))
    return ", ".join(parts) if parts else "(empty)"


def _load_manifest(manifest_path):
    path = Path(manifest_path)
    try:
        manifest = json.loads(path.read_text())
    except OSError as exc:
        raise RomfsError("cannot read manifest: %s" % exc)
    except ValueError as exc:
        raise RomfsError("manifest is not valid JSON: %s" % exc)
    if not isinstance(manifest, dict) or "files" not in manifest:
        raise RomfsError("manifest has no 'files' list")
    files = manifest["files"]
    if not isinstance(files, list) or not files:
        raise RomfsError("manifest 'files' must be a non-empty list")
    return path, files


def _resolve_input(raw, manifest_path, objdir):
    candidate = Path(raw)
    if candidate.is_absolute():
        return candidate
    base = Path(objdir) if objdir else manifest_path.parent
    return base / candidate


def _validate_name(name, where):
    if not isinstance(name, str) or not name:
        raise RomfsError("%s: file name must be a non-empty string" % where)
    try:
        encoded = name.encode("ascii")
    except UnicodeEncodeError:
        raise RomfsError("%s: file name %r is not ASCII" % (where, name))
    if any(byte < 0x20 or byte > 0x7E for byte in encoded):
        raise RomfsError("%s: file name %r contains a non-printable byte"
                         % (where, name))
    if len(encoded) > NAME_SIZE - 1:
        raise RomfsError("%s: file name %r is longer than %d characters"
                         % (where, name, NAME_SIZE - 1))
    return encoded


def build_image(files, geom):
    """files: list of (name_bytes, flags, payload_bytes).  Returns the stream."""
    count = len(files)
    directory_size = count * ENTRY_SIZE
    if directory_size > 0xFFFF:
        raise RomfsError("directory of %d entries exceeds the u16 size field"
                         % count)
    payload_start = HEADER_SIZE + directory_size
    if payload_start > geom["capacity"]:
        raise RomfsError("directory of %d entries exceeds the ROMFS capacity"
                         % count)

    entries = []
    cursor = payload_start
    for name, flags, payload in files:
        offset = cursor
        end = offset + len(payload)
        if end > U32_MAX or end < offset:
            raise RomfsError("file %s wraps the 32-bit offset space"
                             % name.decode("ascii"))
        if end > geom["capacity"]:
            raise RomfsError(
                "file %s ends at %d, past the %d-byte ROMFS capacity"
                % (name.decode("ascii"), end, geom["capacity"]))
        # Capacity bounds already guarantee every logical byte maps into a
        # 0000h-BFFFh window; re-check through the mapper defensively.
        if payload:
            logical_to_physical(end - 1, geom)
        entries.append((name, offset, len(payload), flags))
        cursor = end

    image_size = cursor
    header = bytearray()
    header += MAGIC
    header.append(VERSION)
    header.append(ENTRY_SIZE)
    header += count.to_bytes(2, "little")
    header += directory_size.to_bytes(2, "little")
    header += (0).to_bytes(2, "little")
    header += image_size.to_bytes(4, "little")
    assert len(header) == HEADER_SIZE

    stream = bytearray(header)
    for name, offset, size, flags in entries:
        stream += name + b"\x00" * (NAME_SIZE - len(name))
        stream += offset.to_bytes(4, "little")
        stream += size.to_bytes(4, "little")
        stream += flags.to_bytes(2, "little")
        stream += (0).to_bytes(2, "little")
    for _, _, payload in files:
        stream += payload

    if len(stream) != image_size:
        raise RomfsError("internal layout error: stream length mismatch")
    return bytes(stream)


def pack(manifest_path, out_path, objdir=None, geom=None):
    """Pack a manifest into a recovery image; returns (stream, report)."""
    geom = geom or load_geometry()
    manifest_path, files_json = _load_manifest(manifest_path)
    files = []
    names = set()
    for index, entry in enumerate(files_json):
        where = "%s: files[%d]" % (manifest_path, index)
        if not isinstance(entry, dict):
            raise RomfsError("%s: entry must be an object" % where)
        name = entry.get("name")
        name_bytes = _validate_name(name, where)
        if name_bytes in names:
            raise RomfsError("%s: duplicate file name %r" % (where, name))
        names.add(name_bytes)
        media_type = entry.get("type")
        if media_type not in TYPE_FLAGS:
            raise RomfsError("%s: unsupported type %r (ZMOD/ZEX/DATA)"
                             % (where, media_type))
        raw_path = entry.get("path")
        if not isinstance(raw_path, str) or not raw_path:
            raise RomfsError("%s: missing input path" % where)
        source = _resolve_input(raw_path, manifest_path, objdir)
        if not source.is_file():
            raise RomfsError("%s: input not found: %s" % (where, source))
        payload = source.read_bytes()
        files.append((name_bytes, TYPE_FLAGS[media_type], payload))

    stream = build_image(files, geom)
    out = Path(out_path)
    if out.parent and not out.parent.exists():
        out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(stream)

    header, entries = parse_image(stream, geom)
    report = ["romfspack: %s (%d bytes of %d capacity)"
              % (out, len(stream), geom["capacity"])]
    report.append(header_text(header, entries, stream, geom))
    return stream, "\n".join(report)


def parse_u16(data, at):
    return data[at] | (data[at + 1] << 8)


def parse_u32(data, at):
    return (data[at] | (data[at + 1] << 8) | (data[at + 2] << 16)
            | (data[at + 3] << 24))


def parse_image(data, geom=None):
    """Validate a recovery image; returns (header, entries).

    Entries are dicts: name, flags, offset, size, payload bytes.
    Raises RomfsError for any malformed or out-of-range field.
    """
    if geom is None:
        geom = load_geometry()
    if len(data) < HEADER_SIZE:
        raise RomfsError("image is shorter than the %d-byte header" % HEADER_SIZE)
    if data[0:4] != MAGIC:
        raise RomfsError("bad magic %r (expected ZRFS)" % bytes(data[0:4]))
    version = data[4]
    if version != VERSION:
        raise RomfsError("unsupported version %d" % version)
    entry_size = data[5]
    if entry_size != ENTRY_SIZE:
        raise RomfsError("entry_size is %d, expected %d" % (entry_size, ENTRY_SIZE))
    file_count = parse_u16(data, 6)
    directory_size = parse_u16(data, 8)
    reserved = parse_u16(data, 10)
    if reserved != 0:
        raise RomfsError("header reserved field is %d, expected 0" % reserved)
    image_size = parse_u32(data, 12)
    if image_size != len(data):
        raise RomfsError("image_size is %d, file is %d bytes (truncated or padded)"
                         % (image_size, len(data)))
    if directory_size != file_count * ENTRY_SIZE:
        raise RomfsError("directory_size %d does not match %d entries of %d"
                         % (directory_size, file_count, ENTRY_SIZE))
    payload_start = HEADER_SIZE + directory_size
    if payload_start > image_size:
        raise RomfsError("directory overlaps or overruns the image")
    if image_size > geom["capacity"]:
        raise RomfsError("image_size %d exceeds the ROMFS capacity %d"
                         % (image_size, geom["capacity"]))

    entries = []
    names = set()
    for index in range(file_count):
        at = HEADER_SIZE + index * ENTRY_SIZE
        raw_name = data[at:at + NAME_SIZE]
        nul = raw_name.find(b"\x00")
        if nul < 0:
            raise RomfsError("entry %d: name is not NUL-terminated" % index)
        if nul == 0:
            raise RomfsError("entry %d: name is empty" % index)
        if any(byte != 0 for byte in raw_name[nul:]):
            raise RomfsError("entry %d: name padding is not zero" % index)
        name = raw_name[:nul].decode("ascii", "replace")
        if any(byte < 0x20 or byte > 0x7E for byte in raw_name[:nul]):
            raise RomfsError("entry %d: name contains a non-printable byte" % index)
        if name in names:
            raise RomfsError("entry %d: duplicate name %r" % (index, name))
        names.add(name)
        flags = parse_u16(data, at + 32)
        if flags not in FLAG_NAMES:
            raise RomfsError("entry %d: unsupported flags %d" % (index, flags))
        entry_reserved = parse_u16(data, at + 34)
        if entry_reserved != 0:
            raise RomfsError("entry %d: reserved field is %d, expected 0"
                             % (index, entry_reserved))
        offset = parse_u32(data, at + 24)
        size = parse_u32(data, at + 28)
        if offset < payload_start:
            raise RomfsError("entry %d (%s): offset %d overlaps the table"
                             % (index, name, offset))
        end = offset + size
        if end > U32_MAX or end > image_size:
            raise RomfsError("entry %d (%s): range %d+%d leaves the image"
                             % (index, name, offset, size))
        if size:
            logical_to_physical(end - 1, geom)
        entries.append({
            "name": name,
            "flags": flags,
            "offset": offset,
            "size": size,
            "payload": data[offset:end],
        })

    ordered = sorted(entries, key=lambda entry: entry["offset"])
    cursor = payload_start
    for entry in ordered:
        if entry["offset"] < cursor:
            raise RomfsError("entry %s overlaps the preceding payload"
                             % entry["name"])
        cursor = entry["offset"] + entry["size"]
    return {"version": version, "entry_size": entry_size,
            "file_count": file_count, "directory_size": directory_size,
            "payload_start": payload_start, "image_size": image_size,
            "reserved": reserved}, entries


def header_text(header, entries, data, geom):
    lines = [
        "  header: version %d, entry_size %d, file_count %d, "
        "directory_size %d, reserved %d, image_size %d"
        % (header["version"], header["entry_size"], header["file_count"],
           header["directory_size"], header["reserved"], header["image_size"]),
        "  %-23s %-4s %9s  %-34s %s"
        % ("name", "type", "size", "logical range", "physical ROM range"),
    ]
    if not entries:
        lines.append("  (no files)")
    for entry in entries:
        start = entry["offset"]
        end = entry["offset"] + entry["size"]
        digest = hashlib.sha256(entry["payload"]).hexdigest()
        lines.append(
            "  %-23s %-4s %9d  %08Xh-%08Xh  %s"
            % (entry["name"], FLAG_NAMES[entry["flags"]], entry["size"],
               start, end - 1 if entry["size"] else start,
               format_physical(start, entry["size"], geom)))
        lines.append("  %-23s sha256 %s" % ("", digest))
    return "\n".join(lines)


def dump(path, geom=None):
    geom = geom or load_geometry()
    data = Path(path).read_bytes()
    header, entries = parse_image(data, geom)
    lines = ["romfspack: %s (%d bytes of %d capacity)"
             % (path, len(data), geom["capacity"]),
             header_text(header, entries, data, geom)]
    return "\n".join(lines)


def main(argv):
    parser = argparse.ArgumentParser(
        prog="romfspack.py", description="ZephyrOS ROMFS v1 packer/validator")
    parser.add_argument("--memory-inc", default=None,
                        help="platform memory.inc providing ROMFS geometry")
    sub = parser.add_subparsers(dest="mode", required=True)

    pack_cmd = sub.add_parser("pack", help="build a recovery image")
    pack_cmd.add_argument("manifest")
    pack_cmd.add_argument("output")
    pack_cmd.add_argument("--objdir", default=None,
                          help="base directory for manifest input paths")

    for name in ("validate", "dump"):
        cmd = sub.add_parser(name, help="%s a recovery image" % name)
        cmd.add_argument("image")

    args = parser.parse_args(argv)
    try:
        geom = load_geometry(args.memory_inc)
        if args.mode == "pack":
            _, report = pack(args.manifest, args.output,
                             objdir=args.objdir, geom=geom)
            print(report)
        elif args.mode == "validate":
            data = Path(args.image).read_bytes()
            header, entries = parse_image(data, geom)
            digest = hashlib.sha256(data).hexdigest()
            print("romfspack: %s OK: %d bytes, %d entries, sha256 %s"
                  % (args.image, header["image_size"], len(entries), digest))
        else:
            print(dump(args.image, geom))
    except RomfsError as exc:
        print("romfspack: %s" % exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
