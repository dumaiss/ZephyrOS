#!/usr/bin/env python3
"""KonaOS MOD driver-image packer/validator.

Consumes SDCC/sdasz80 relocatable (.rel) output plus a small JSON driver
manifest and produces a deterministic, host-linked **driver image** -
the only MOD form.  The packer mini-links the
listed objects into each declared segment's coordinate space
(segment-relative 16-bit absolute references only), links every segment
at its host-planned bank-6 address, resolves the inter-driver
dependencies and endpoint numbers, and emits the flat load plan the
nucleus loader iterates (kernel/driver/driver_loader.c).

The host computes everything: the container is
already relocated, its tables are final, and the loader checks only the
bounds that protect the nucleus and the fixed driver-bank layout.  There
is no version field, no module type, no fini/relocation target and no
import table.

Container layout (all little-endian):

    offset 0                16-byte header:
                            00 magic[4] "KMOD"
                            04 segment count
                            05 endpoint count
                            06 dependency count
                            07 publication count
                            08 init entry (absolute; FFFFh = none)
                            0A payload offset
                            0C total size
                            0E reserved (0)
    offset 16               segment table, 12 bytes each:
                            { role, 0, base, image_size, bss_size,
                              entry, payload_offset }
    after the segments      endpoint table, 8 bytes each:
                            { class, instance, version, flags,
                              number, 0, dispatch }
    after the endpoints     dependency table, 4 bytes each:
                            { class, instance, number }
    after the dependencies  publication table, 20 bytes each:
                            { name[16], type, number, object, flags }
    payload_offset          segment images, concatenated and linked
    total_size = payload_offset + sum(image_size)

Segment roles keep the architecture-5.2 tiers: `hot` and `io` link
inside bank 6's 6000h-DFFFh (io inside 8000h-BFFFh), `boot` and `bulk`
inside 0000h-5FFFh.  `entry` is the absolute boot entry (only the boot
role may carry one) or FFFFh; the init entry lives in the header.

Supported relocation subset (anything else is rejected):

    mode 0x00  word, relocatable area       -> segment-relative area base
    mode 0x02  word, symbol                 -> segment-relative symbol

Driver manifest (JSON), paths resolved against --objdir (default:
manifest dir):

    {
      "name": "ioc",
      "objects": ["ioc.rel", "ioc_boot.rel", "sdcc_indirect_calls.rel"],
      "init": "ioc_driver_init",    # hot-segment init, called with the
                                     # bootstrap table; optional
      "segments": [
        {"role": "hot",  "image_areas": ["_CODE"],
         "bss_areas": ["_DRIVER_IO"]},
        {"role": "boot", "image_areas": ["_BOOT"],
         "entry": "ioc_boot_entry"}  # boot entry, run once; optional
      ],
      "endpoints": [
        {"class": 5, "instance": 0, "version": 1,
         "dispatch": "ioc_driver_dispatch", "flags": 0}
      ],
      "publishes": [],               # namespace publications
      "depends": []                  # [{"class": 4, "instance": 0}, ...]
    }

Usage:
    modpack.py pack <manifest.json> <out.mod> [--objdir DIR]
                     [--set MANIFEST ...] [--report FILE]
    modpack.py validate <container.mod> [...]
    modpack.py dump <container.mod> [...]
    modpack.py plan <out.plan.json> <container.mod> [...]

The `plan` command validates a set of packed images in load order (unique
endpoints, resolvable dependencies, publications, a tier allocation that
fits) and writes the plan report `drivers.plan.json`: one row per image and
the bank-6 driver ledger tools/pack_image.py reads.  The boot manifest
`drivers.man` and the images' names are not written here: they come
from tools/systemgen.py, which resolves the image's system file and hands
this packer one resolved manifest per selected driver (--set points at
those copies).
"""

import hashlib
import json
import os
import re
import sys

if os.path.dirname(os.path.abspath(__file__)) not in sys.path:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pack_image  # shared budget-ledger format (one definition)

# Flat container geometry; the constants mirror
# formats/include/mod.h and are pinned by host tests.
MAGIC = b"KMOD"
HEADER_SIZE = 16
SEGMENT_SIZE = 12
ENDPOINT_SIZE = 8
DEPENDENCY_SIZE = 4
PUBLICATION_SIZE = 20
PUBLICATION_NAME = 16
NO_ENTRY = 0xFFFF
MAX_SEGMENTS = 4
MAX_ENDPOINTS = 8
MAX_DEPENDENCIES = 8
MAX_PUBLICATIONS = 12
MAX_IMAGES = 8
# Endpoint flags: bit 0 is the re-entrancy declaration (KOS_IPC_EP_REENTRANT),
# bit 1 the release declaration (MOD_EP_RELEASE): the manifest's
# `"release": true` asks the nucleus to tell the endpoint when a handle on
# one of its objects is released; bit 2 the event declaration
# (MOD_EP_EVENTS): the manifest's `"events": true` lets applications
# subscribe to the endpoint's events (SYS_NOTIFY -> KOS_OP_SUBSCRIBE) and
# the driver post them through the bootstrap interface's event_post.
EP_REENTRANT = 0x01
EP_RELEASE = 0x02
EP_EVENTS = 0x04
EP_FLAGS = EP_REENTRANT | EP_RELEASE | EP_EVENTS
# Dependency declaration flags (the manifest's `optional`): an optional
# dependency that no image in the set exports resolves to endpoint 0.
DEP_OPTIONAL = 0x01
# Segment roles (MOD_SEGMENT_*), in the loader's report order.
ROLE_CODES = {"hot": 1, "boot": 2, "io": 3, "bulk": 4}
ROLE_NAMES = {value: key for key, value in ROLE_CODES.items()}
# Destination tiers of each segment role (address range, end exclusive).
TIERS = {
    "hot": (0x6000, 0xE000),
    "io": (0x8000, 0xC000),
    "boot": (0x0000, 0x6000),
    "bulk": (0x0000, 0x6000),
}
# Bank-6 destination geometry (architecture 5.2), mirrored by
# kernel/driver/driver_loader.c: the hot tier 6000h-DFFFh (driver I/O
# buffers first, inside 8000h-BFFFh), the boot/bulk tier 0000h-5FFFh.
HOT_TIER_BASE = 0x6000
HOT_TIER_END = 0xE000
IO_BASE = 0x8000
IO_END = 0xC000
BOOT_TIER_BASE = 0x0000
BOOT_TIER_END = 0x6000
# The DEVICE provider's root (architecture 4.10): a manifest declares the
# full namespace path and the container carries the provider-relative
# remainder, which is what the provider resolves.
DEVICE_ROOT = "/dev"
# The filesystem-namespace root: a filesystem driver
# publishes `/fs` itself, so its publication path is the provider root
# rather than a name below DEVICE_ROOT.
FS_ROOT = "/fs"
# The 8-bit protocol types a publication may name (abi/kos/object.h).
TYPES = {
    "SERIAL": 1, "VIDEO": 2, "BLOCK": 3, "INPUT": 4, "IOC": 5,
    "CONSOLE": 6, "FILE": 7, "FS": 8, "SOUND": 9, "TIMER": 10,
}
# The boot manifest entry is the ROMFS name field exactly (romfs-format.md,
# formats/include/mod.h).
MANIFEST_ENTRY_SIZE = 24

R_WORD = 0x00
R_BYTE = 0x01
R_SYM = 0x02
R_PCR = 0x04
R_PAG0 = 0x20
R_PAG = 0x40
R_ESCAPE_MASK = 0xF0
SUPPORTED_MODES = (R_WORD, R_SYM)

A_BYTES = 4


class ModError(Exception):
    """Packing or validation failed."""


# --------------------------------------------------------------------------
# .rel parsing (SDCC/sdasz80 XL4 object format)
# --------------------------------------------------------------------------

class Symbol:
    __slots__ = ("name", "kind", "value", "area")

    def __init__(self, name, kind, value, area):
        self.name = name
        self.kind = kind          # "def" or "ref"
        self.value = value        # area-relative offset for defs
        self.area = area          # area index for defs, None otherwise


class Area:
    __slots__ = ("name", "size", "flags", "index")

    def __init__(self, name, size, flags, index):
        self.name = name
        self.size = size
        self.flags = flags
        self.index = index


class Chunk:
    __slots__ = ("area_index", "addr", "data", "relocs")

    def __init__(self, area_index, addr, data, relocs):
        self.area_index = area_index
        self.addr = addr
        self.data = data
        self.relocs = relocs      # list of (mode, rtp, ref_index)


class RelObject:
    def __init__(self, path):
        self.path = path
        self.module = None
        self.areas = []
        self.symbols = []
        self.chunks = []

    def area_by_name(self, name):
        for area in self.areas:
            if area.name == name:
                return area
        return None


def parse_rel(text, path):
    """Parse one XL4 .rel file into areas/symbols/chunks (file order)."""
    obj = RelObject(path)
    current_area = None
    pending = None
    header_counts = None

    for lineno, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line:
            continue
        kind = line[0]
        fields = line.split()

        if kind == "X":
            if line != "XL4":
                raise ModError("%s:%d: unsupported radix/address spec %r "
                                "(only XL4)" % (path, lineno, line))
        elif kind == "H":
            # H <areas-hex> areas <globals-hex> global symbols: sdasz80
            # writes both counts in hex (10 areas arrive as "A").
            try:
                header_counts = (int(fields[1], 16), int(fields[3], 16))
            except (IndexError, ValueError):
                raise ModError("%s:%d: malformed header line" % (path, lineno))
        elif kind == "M":
            obj.module = fields[1] if len(fields) > 1 else ""
        elif kind == "O":
            continue
        elif kind == "S":
            if len(fields) < 3 or len(fields[2]) < 4:
                raise ModError("%s:%d: malformed symbol line" % (path, lineno))
            name = fields[1]
            tag = fields[2][:3]
            try:
                value = int(fields[2][3:], 16)
            except ValueError:
                raise ModError("%s:%d: malformed symbol value" % (path, lineno))
            if tag == "Def":
                obj.symbols.append(Symbol(name, "def", value, current_area))
            elif tag == "Ref":
                obj.symbols.append(Symbol(name, "ref", 0, None))
            else:
                raise ModError("%s:%d: unknown symbol kind %r"
                                % (path, lineno, tag))
        elif kind == "A":
            # A <name> size <hex> flags <hex> addr <hex>
            try:
                name = fields[1]
                size = int(fields[3], 16)
                flags = int(fields[5], 16)
            except (IndexError, ValueError):
                raise ModError("%s:%d: malformed area line" % (path, lineno))
            current_area = len(obj.areas)
            obj.areas.append(Area(name, size, flags, current_area))
        elif kind == "T":
            if pending is not None:
                raise ModError("%s:%d: T line without matching R line"
                                % (path, lineno))
            try:
                values = [int(v, 16) for v in fields[1:]]
            except ValueError:
                raise ModError("%s:%d: malformed data byte" % (path, lineno))
            pending = values
        elif kind == "R":
            if pending is None:
                raise ModError("%s:%d: R line without preceding T line"
                                % (path, lineno))
            try:
                values = [int(v, 16) for v in fields[1:]]
            except ValueError:
                raise ModError("%s:%d: malformed relocation byte"
                                % (path, lineno))
            chunk = decode_chunk(pending, values, path, lineno)
            if chunk is not None:
                obj.chunks.append(chunk)
            pending = None
        else:
            raise ModError("%s:%d: unsupported record %r"
                            % (path, lineno, kind))

    if pending is not None:
        raise ModError("%s: trailing T line without R line" % path)

    if header_counts is not None:
        areas, symbols = header_counts
        if areas != len(obj.areas):
            raise ModError("%s: header declares %d areas, found %d"
                            % (path, areas, len(obj.areas)))
        if symbols != len(obj.symbols):
            raise ModError("%s: header declares %d symbols, found %d"
                            % (path, symbols, len(obj.symbols)))
    return obj


def decode_chunk(values, reloc_values, path, lineno):
    """Decode a T/R pair.  Returns None for an empty data chunk."""
    if len(values) < A_BYTES:
        raise ModError("%s:%d: T line shorter than the address field"
                        % (path, lineno))
    addr = (values[0] | (values[1] << 8) | (values[2] << 16) |
            (values[3] << 24))
    data = bytes(values[A_BYTES:])

    groups = decode_relocs(reloc_values, path, lineno)
    if not groups:
        raise ModError("%s:%d: R line without the initial area group"
                        % (path, lineno))

    init_mode, init_rtp, area_index = groups[0]
    if init_mode != R_WORD or init_rtp != 0:
        raise ModError("%s:%d: unsupported chunk header group (mode %02X)"
                        % (path, lineno, init_mode))
    relocs = []
    for mode, rtp, ref in groups[1:]:
        relocs.append((mode, rtp, ref))

    if not data:
        if relocs:
            raise ModError("%s:%d: relocation on an empty data chunk"
                            % (path, lineno))
        return None
    return Chunk(area_index, addr, data, relocs)


def decode_relocs(values, path, lineno):
    """Decode the flat R byte list into (mode, rtp, ref) groups."""
    groups = []
    pos = 0
    while pos < len(values):
        mode = values[pos]
        if (mode & R_ESCAPE_MASK) == R_ESCAPE_MASK:
            if pos + 1 >= len(values):
                raise ModError("%s:%d: truncated escaped relocation mode"
                                % (path, lineno))
            mode = ((mode & 0x0F) << 8) | values[pos + 1]
            pos += 2
        else:
            pos += 1
        if pos + 3 > len(values):
            raise ModError("%s:%d: truncated relocation group"
                            % (path, lineno))
        rtp = values[pos]
        ref = values[pos + 1] | (values[pos + 2] << 8)
        groups.append((mode, rtp, ref))
        pos += 3
    return groups


# --------------------------------------------------------------------------
# Mini-link: layout, symbol resolution, relocation application
# --------------------------------------------------------------------------

class Packed:
    def __init__(self, name):
        self.kind = "driver"
        self.name = name
        self.image = bytearray()
        self.bss_size = 0
        self.init_offset = NO_ENTRY
        self.symbols = {}         # segment-relative Def offsets
        self.symbol_segments = {}  # symbol name -> segment role


def load_manifest(path, objdir):
    try:
        with open(path, "r") as handle:
            manifest = json.load(handle)
    except (OSError, ValueError) as exc:
        raise ModError("cannot read manifest %s: %s" % (path, exc))

    for field in ("name", "objects"):
        if field not in manifest:
            raise ModError("manifest %s: missing %r" % (path, field))

    kind = manifest.get("type", "driver")
    if kind != "driver":
        raise ModError("manifest %s: unknown module type %r; MOD is a "
                        "driver image" % (path, kind))
    return load_driver_manifest(manifest, path, objdir)


def read_manifest_objects(manifest, path, objdir):
    resolve_dir = objdir if objdir is not None else os.path.dirname(path)
    objects = []
    for name in manifest["objects"]:
        obj_path = name if os.path.isabs(name) else os.path.join(resolve_dir,
                                                                 name)
        try:
            text = open(obj_path).read()
        except OSError as exc:
            raise ModError("cannot read object %s: %s" % (obj_path, exc))
        objects.append(parse_rel(text, obj_path))
    return objects


def read_manifest_asset(segment, path, objdir):
    """Load one segment's raw image asset (a bulk-data payload).

    The asset is a binary file (the pinned V9958 font):
    the segment's image bytes are exactly the file, there are no
    relocations, and the loader places it verbatim.  Paths resolve like
    object names (objdir first, manifest directory otherwise)."""
    resolve_dir = objdir if objdir is not None else os.path.dirname(path)
    name = str(segment["asset"])
    asset_path = name if os.path.isabs(name) else os.path.join(resolve_dir,
                                                               name)
    try:
        with open(asset_path, "rb") as handle:
            data = handle.read()
    except OSError as exc:
        raise ModError("cannot read asset %s: %s" % (asset_path, exc))
    if not data:
        raise ModError("asset %s is empty" % asset_path)
    return data


def load_driver_manifest(manifest, path, objdir):
    """Validate the driver schema (segment roles, endpoints, symlinks
    resolved later against the area layout)."""
    if "type" in manifest and manifest["type"] != "driver":
        raise ModError("manifest %s: unknown module type %r"
                        % (path, manifest["type"]))

    segments = manifest.get("segments")
    if not isinstance(segments, list) or not segments:
        raise ModError("manifest %s: segments must be a non-empty list"
                        % path)
    if len(segments) > MAX_SEGMENTS:
        raise ModError("manifest %s: more than %d segments"
                        % (path, MAX_SEGMENTS))

    roles = []
    listed = []
    entries = []
    for index, segment in enumerate(segments):
        where = "%s: segment %d" % (path, index)
        role = segment.get("role")
        if role not in ROLE_CODES:
            raise ModError("%s: unknown role %r" % (where, role))
        if role in roles:
            raise ModError("%s: duplicate role %s" % (where, role))
        roles.append(role)
        image_areas = list(segment.get("image_areas", []))
        bss_areas = list(segment.get("bss_areas", []))
        overlap = set(image_areas) & set(bss_areas)
        if overlap:
            raise ModError("%s: area(s) both image and bss: %s"
                            % (where, ", ".join(sorted(overlap))))
        if "asset" in segment and (image_areas or bss_areas):
            raise ModError("%s: an asset segment takes no areas" % where)
        if "asset" not in segment and not image_areas and not bss_areas:
            raise ModError("%s: segment lists no areas and no asset"
                            % where)
        for name in image_areas + bss_areas:
            if name in listed:
                raise ModError("%s: area %s is claimed twice"
                                % (where, name))
            listed.append(name)
        entry = segment.get("entry")
        if entry is not None and role != "boot":
            raise ModError("%s: only the boot role carries an entry" % where)
        entries.append(entry)
    if "hot" not in roles:
        raise ModError("%s: a driver image needs a hot segment" % path)

    endpoints = list(manifest.get("endpoints", []))
    if len(endpoints) > MAX_ENDPOINTS:
        raise ModError("manifest %s: more than %d endpoints"
                        % (path, MAX_ENDPOINTS))
    for index, endpoint in enumerate(endpoints):
        where = "%s: endpoint %d" % (path, index)
        for field in ("class", "instance", "version", "dispatch"):
            if field not in endpoint:
                raise ModError("%s: missing %r" % (where, field))
        for field in ("class", "instance", "version"):
            value = int(endpoint[field])
            if value < 0 or value > 0xFF:
                raise ModError("%s: %s out of range" % (where, field))
        flags = int(endpoint.get("flags", 0))
        if flags & ~EP_REENTRANT:
            raise ModError("%s: unknown flags %02Xh" % (where, flags))
        release = endpoint.get("release", False)
        if not isinstance(release, bool):
            raise ModError("%s: release must be true or false" % where)
        endpoint["release"] = release
        events = endpoint.get("events", False)
        if not isinstance(events, bool):
            raise ModError("%s: events must be true or false" % where)
        endpoint["events"] = events

    publishes = list(manifest.get("publishes", []))
    if len(publishes) > MAX_PUBLICATIONS:
        raise ModError("manifest %s: more than %d publications"
                        % (path, MAX_PUBLICATIONS))
    for index, publish in enumerate(publishes):
        where = "%s: publication %d" % (path, index)
        full = publish.get("path")
        if not isinstance(full, str):
            raise ModError("%s: path must be a string below %s/ or %s"
                            % (where, DEVICE_ROOT, FS_ROOT))
        if full == FS_ROOT:
            # The filesystem-namespace root: the whole volume is one
            # instance, so it names no object.
            if publish.get("type") != "FS":
                raise ModError("%s: %s must be published with type FS"
                                % (where, FS_ROOT))
            if int(publish.get("object", 0)) != 0:
                raise ModError("%s: %s is the provider root and names no "
                                "object" % (where, FS_ROOT))
            publish["type_code"] = TYPES["FS"]
            name = FS_ROOT
        elif full.startswith(DEVICE_ROOT + "/"):
            name = full[len(DEVICE_ROOT):]
            if len(name) + 1 > PUBLICATION_NAME:
                raise ModError("%s: %s is longer than %d provider-relative "
                                "bytes" % (where, full,
                                           PUBLICATION_NAME - 1))
            if name.endswith("/") or "//" in name:
                raise ModError("%s: %s is not a clean name"
                                % (where, full))
            for char in name:
                if not (0x21 <= ord(char) <= 0x7E):
                    raise ModError("%s: %s carries a non-printable byte"
                                    % (where, full))
            publish_type = publish.get("type")
            if isinstance(publish_type, str):
                if publish_type not in TYPES:
                    raise ModError("%s: unknown type %r"
                                    % (where, publish_type))
                publish["type_code"] = TYPES[publish_type]
            else:
                value = int(publish_type if publish_type is not None else 0)
                if value < 1 or value > 0xFF:
                    raise ModError("%s: type %r out of range"
                                    % (where, publish_type))
                publish["type_code"] = value
        else:
            raise ModError("%s: path must be a string below %s/ or %s"
                            % (where, DEVICE_ROOT, FS_ROOT))
        publish["name"] = name
        endpoint = publish.get("endpoint")
        if not isinstance(endpoint, dict) or "class" not in endpoint or \
                "instance" not in endpoint:
            raise ModError("%s: endpoint must name a class and an instance"
                            % where)
        for field in ("class", "instance"):
            value = int(endpoint[field])
            if value < 0 or value > 0xFF:
                raise ModError("%s: endpoint %s out of range"
                                % (where, field))
        publish["class"] = int(endpoint["class"])
        publish["instance"] = int(endpoint["instance"])
        object_id = int(publish.get("object", 0))
        if object_id < 0 or object_id > 0xFF:
            raise ModError("%s: object out of range" % where)
        publish["object"] = object_id

    depends = list(manifest.get("depends", []))
    if len(depends) > MAX_DEPENDENCIES:
        raise ModError("manifest %s: more than %d dependencies"
                        % (path, MAX_DEPENDENCIES))
    for index, dependency in enumerate(depends):
        where = "%s: dependency %d" % (path, index)
        for field in ("class", "instance"):
            if field not in dependency:
                raise ModError("%s: missing %r" % (where, field))
            value = int(dependency[field])
            if value < 0 or value > 0xFF:
                raise ModError("%s: %s out of range" % (where, field))
        if int(dependency.get("optional", 0)) not in (0, 1):
            raise ModError("%s: optional must be 0 or 1" % where)

    return {
        "kind": "driver",
        "name": str(manifest["name"]),
        "objects": read_manifest_objects(manifest, path, objdir),
        "object_names": [str(name) for name in manifest["objects"]],
        "segments": segments,
        "segment_entries": entries,
        "segment_assets": [
            read_manifest_asset(segment, path, objdir)
            if "asset" in segment else None
            for segment in segments
        ],
        "endpoints": endpoints,
        "depends": depends,
        "publishes": publishes,
        "init": manifest.get("init"),
        "path": path,
    }


def build_symbol_table(manifest):
    """Global Def table across objects; duplicates are rejected."""
    table = {}
    for obj_index, obj in enumerate(manifest["objects"]):
        for symbol in obj.symbols:
            if symbol.kind != "def":
                continue
            if symbol.area is None:
                continue  # absolute/outside any area; not module-local
            area = obj.areas[symbol.area]
            if area.flags & 0x08:
                continue  # absolute area symbol
            key = symbol.name
            entry = (obj_index, area.name, symbol.value)
            if key in table and table[key] != entry:
                raise ModError("symbol %s defined more than once" % key)
            table[key] = entry
    return table


def collect_symbol_offsets(manifest, layout):
    """All module-local Def symbols as module-relative offsets (report)."""
    offsets = {}
    for obj_index, obj in enumerate(manifest["objects"]):
        for symbol in obj.symbols:
            if symbol.kind != "def" or symbol.area is None:
                continue
            area = obj.areas[symbol.area]
            key = (obj_index, area.name)
            if key not in layout["instance_offset"]:
                continue
            offsets[symbol.name] = layout["instance_offset"][key] + \
                symbol.value
    return offsets


def collect_symbol_segments(manifest, layout):
    """Segment role of every module-local Def symbol (driver reports)."""
    roles = {}
    for obj_index, obj in enumerate(manifest["objects"]):
        for symbol in obj.symbols:
            if symbol.kind != "def" or symbol.area is None:
                continue
            area = obj.areas[symbol.area]
            if (obj_index, area.name) not in layout["instance_offset"]:
                continue
            segment = layout["area_segment"][area.name]
            roles[symbol.name] = layout["segments"][segment]["role"]
    return roles


def build_driver_layout(manifest):
    """Assign per-segment image/BSS bases and per-object instance bases.

    Every area belongs to exactly one segment; a segment's coordinate
    space starts at its image (image areas in manifest order) and
    continues with its BSS areas.  The returned bases are therefore
    segment-relative, which is exactly the coordinate space the loader's
    `word += segment base` relocation uses."""
    objects = manifest["objects"]
    segments = manifest["segments"]

    area_segment = {}
    area_size = {}
    for index, segment in enumerate(segments):
        for name in (list(segment.get("image_areas", [])) +
                     list(segment.get("bss_areas", []))):
            area_segment[name] = index
            area_size[name] = 0

    instance_offset = {}
    for obj_index, obj in enumerate(objects):
        for area in obj.areas:
            if area.name not in area_segment:
                if area.size != 0 and (area.flags & 0x08) == 0:
                    raise ModError("%s: area %s is not listed in any "
                                    "segment" % (obj.path, area.name))
                continue
            instance_offset[(obj_index, area.name)] = area_size[area.name]
            area_size[area.name] += area.size

    for name in area_segment:
        if area_size[name] == 0:
            raise ModError("%s: manifest area %s is empty or missing"
                            % (manifest["path"], name))

    layout_segments = []
    area_base = {}
    payload_offset = 0
    for index, segment in enumerate(segments):
        asset = manifest["segment_assets"][index]
        offset = 0
        for name in segment.get("image_areas", []):
            area_base[name] = offset
            offset += area_size[name]
        image_size = offset
        if asset is not None:
            image_size = len(asset)
        for name in segment.get("bss_areas", []):
            area_base[name] = offset
            offset += area_size[name]
        bss_size = offset - image_size
        if asset is not None:
            bss_size = 0
        if image_size == 0 and bss_size == 0:
            raise ModError("%s: segment %d is empty"
                            % (manifest["path"], index))
        layout_segments.append({
            "role": segment["role"],
            "image_size": image_size,
            "bss_size": bss_size,
            "payload_offset": payload_offset,
            "entry_name": manifest["segment_entries"][index],
            "asset": asset,
        })
        payload_offset += image_size

    for (obj_index, name), value in list(instance_offset.items()):
        instance_offset[(obj_index, name)] = area_base[name] + value

    return {
        "area_base": area_base,
        "area_size": area_size,
        "area_segment": area_segment,
        "instance_offset": instance_offset,
        "segments": layout_segments,
        "image_size": payload_offset,
        "bss_size": sum(segment["bss_size"] for segment in layout_segments),
        "total_size": payload_offset,
    }


def symbol_location(manifest, layout, symbols, name, where):
    """(segment index, segment-relative offset) of a named symbol."""
    for candidate in (name, "_" + name):
        for obj_index, obj in enumerate(manifest["objects"]):
            for symbol in obj.symbols:
                if symbol.kind == "def" and symbol.name == candidate:
                    if symbol.area is None:
                        raise ModError("%s: symbol %s is absolute"
                                        % (where, name))
                    area = obj.areas[symbol.area]
                    key = (obj_index, area.name)
                    if key not in layout["instance_offset"]:
                        raise ModError("%s: symbol %s lives in unlisted "
                                        "area %s" % (where, name, area.name))
                    return (layout["area_segment"][area.name],
                            layout["instance_offset"][key] + symbol.value)
    raise ModError("%s: symbol %s is not defined by any object"
                    % (where, name))


def pack_driver_manifest(manifest):
    """Mini-link one driver image into its per-segment coordinate spaces."""
    layout = build_driver_layout(manifest)
    symbols = build_symbol_table(manifest)
    segments = layout["segments"]
    image = bytearray(layout["image_size"])
    reloc_sites = {}                       # payload offset -> True
    records = [[] for _ in segments]       # per segment, segment-relative
    where = manifest["path"]

    for obj_index, obj in enumerate(manifest["objects"]):
        for chunk in obj.chunks:
            if chunk.area_index >= len(obj.areas):
                raise ModError("%s: chunk references area %d out of range"
                                % (obj.path, chunk.area_index))
            area = obj.areas[chunk.area_index]
            key = (obj_index, area.name)
            if key not in layout["instance_offset"]:
                raise ModError("%s: chunk in unlisted area %s"
                                % (obj.path, area.name))
            segment = layout["area_segment"][area.name]
            base = layout["instance_offset"][key]
            start = segments[segment]["payload_offset"] + base + chunk.addr
            end = start + len(chunk.data)
            area_limit = (segments[segment]["payload_offset"] +
                          layout["area_base"][area.name] +
                          layout["area_size"][area.name])
            if end > area_limit:
                raise ModError("%s: chunk at %04Xh overruns area %s"
                                % (obj.path, chunk.addr, area.name))
            image_areas = list(manifest["segments"][segment].get(
                "image_areas", []))
            if area.name not in image_areas:
                raise ModError("%s: BSS area %s contains initialized data"
                                % (obj.path, area.name))

            # Last-writer-wins: later chunks overwrite earlier bytes, but a
            # relocation site may only be replaced as a whole word.
            written = set(range(start, end))
            for site in list(reloc_sites):
                covered = (site in written) + (site + 1 in written)
                if covered == 2:
                    del reloc_sites[site]
                elif covered == 1:
                    raise ModError(
                        "%s: chunk partially overwrites relocation site "
                        "at %04Xh" % (obj.path, site))
            image[start:end] = chunk.data

            for mode, rtp, ref in chunk.relocs:
                if mode not in SUPPORTED_MODES:
                    reason = "unsupported relocation mode %02Xh" % mode
                    if mode & R_BYTE:
                        reason += " (byte-fragmented address store; use a " \
                                  "static descriptor initializer)"
                    raise ModError("%s: %s" % (obj.path, reason))
                if rtp < A_BYTES:
                    raise ModError("%s: relocation targets the chunk "
                                    "address field" % obj.path)
                index = rtp - A_BYTES
                if index + 2 > len(chunk.data):
                    raise ModError("%s: relocation outside the T data"
                                    % obj.path)
                patch = base + chunk.addr + index
                if patch + 2 > segments[segment]["image_size"]:
                    raise ModError("%s: patch at %04Xh crosses the segment "
                                    "end" % (obj.path, patch))
                target, target_area = resolve_relocation_area(
                    manifest, layout, symbols, obj_index, mode, ref)
                target_segment = layout["area_segment"][target_area]
                patch_role = manifest["segments"][segment]["role"]
                target_role = manifest["segments"][target_segment]["role"]
                # Segments relocate against their own base, so a record
                # names the target segment (its second byte, reserved in
                # driver containers).  Visibility decides which crossings
                # are legal: hot code runs in the mode-11 window, where
                # only the hot and io tiers are visible; boot code runs in
                # mode 10 over the whole bank; bulk data is opaque and
                # moved by nucleus copies, never executed or followed.
                if patch_role == "hot" and target_role in ("boot", "bulk"):
                    raise ModError(
                        "%s: relocation at %04Xh reaches from the hot "
                        "segment into the invisible %s tier"
                        % (obj.path, patch, target_role))
                if patch_role == "bulk":
                    raise ModError(
                        "%s: relocation at %04Xh is inside bulk data, "
                        "which is moved by nucleus copies only"
                        % (obj.path, patch))
                addend = (image[start + index] |
                          (image[start + index + 1] << 8))
                value = target + addend
                if value > 0xFFFF:
                    raise ModError("%s: relocation at %04Xh overflows 16 "
                                    "bits" % (obj.path, patch))
                image[start + index] = value & 0xFF
                image[start + index + 1] = (value >> 8) & 0xFF
                reloc_sites[start + index] = True
                records[segment].append((target_segment, patch))

    # Segment assets (bulk data): the file's bytes are
    # the segment image, copied verbatim, with no relocation records.
    for index, segment in enumerate(layout["segments"]):
        if segment["asset"] is None:
            continue
        start = segment["payload_offset"]
        image[start:start + len(segment["asset"])] = segment["asset"]

    packed = Packed(manifest["name"])
    packed.kind = "driver"
    packed.image = image
    packed.bss_size = layout["bss_size"]
    packed.symbols = collect_symbol_offsets(manifest, layout)
    packed.symbol_segments = collect_symbol_segments(manifest, layout)
    packed.segments = []
    packed.records = []
    packed.endpoints = []
    packed.dependencies = []
    packed.publications = []
    packed.init_offset = NO_ENTRY

    for index, segment in enumerate(layout["segments"]):
        entry_offset = NO_ENTRY
        if segment["entry_name"] is not None:
            entry_segment, entry_offset = symbol_location(
                manifest, layout, symbols, segment["entry_name"],
                "%s: segment entry" % where)
            if entry_segment != index:
                raise ModError("%s: the %s entry %s is not in that segment"
                                % (where, segment["role"],
                                   segment["entry_name"]))
            if entry_offset >= segment["image_size"]:
                raise ModError("%s: the %s entry is outside the image"
                                % (where, segment["role"]))
        packed.segments.append({
            "role": segment["role"],
            "image_size": segment["image_size"],
            "bss_size": segment["bss_size"],
            "entry_offset": entry_offset,
            "payload_offset": segment["payload_offset"],
            "reloc_start": len(packed.records),
            "reloc_count": len(records[index]),
        })
        packed.records.extend(records[index])

    if manifest["init"] is not None:
        init_segment, init_offset = symbol_location(
            manifest, layout, symbols, manifest["init"],
            "%s: init" % where)
        if manifest["segments"][init_segment]["role"] != "hot":
            raise ModError("%s: init entry %s is not in the hot segment"
                            % (where, manifest["init"]))
        if init_offset >= layout["segments"][init_segment]["image_size"]:
            raise ModError("%s: init entry %s is outside the image"
                            % (where, manifest["init"]))
        packed.init_offset = init_offset

    hot_index = [index for index, segment in enumerate(layout["segments"])
                 if segment["role"] == "hot"][0]
    hot_image_size = layout["segments"][hot_index]["image_size"]
    for endpoint in manifest["endpoints"]:
        dispatch_segment, dispatch = symbol_location(
            manifest, layout, symbols, endpoint["dispatch"],
            "%s: endpoint dispatch" % where)
        if dispatch_segment != hot_index or dispatch >= hot_image_size:
            raise ModError("%s: endpoint %d.%d dispatch %s is not in the "
                            "hot segment" % (where, int(endpoint["class"]),
                                             int(endpoint["instance"]),
                                             endpoint["dispatch"]))
        packed.endpoints.append({
            "class": int(endpoint["class"]),
            "instance": int(endpoint["instance"]),
            "version": int(endpoint["version"]),
            "flags": int(endpoint.get("flags", 0)) |
                     (EP_RELEASE if endpoint.get("release", False) else 0) |
                     (EP_EVENTS if endpoint.get("events", False) else 0),
            "dispatch_offset": dispatch,
        })
    for dependency in manifest["depends"]:
        packed.dependencies.append({
            "class": int(dependency["class"]),
            "instance": int(dependency["instance"]),
            "optional": int(dependency.get("optional", 0)),
            "number": 0,
        })
    for publish in manifest.get("publishes", []):
        packed.publications.append({
            "name": publish["name"],
            "type": publish["type_code"],
            "class": publish["class"],
            "instance": publish["instance"],
            "object": publish["object"],
            "number": 0,
        })

    if packed.bss_size > 0xFFFF or len(packed.image) + packed.bss_size > 0xFFFF:
        raise ModError("%s: image + bss overflows 16 bits" % where)
    return packed


def link_driver_image(packed, row):
    """Link one image at the host-planned segment bases (architecture
    5.3): every KREL_ABS16 record is applied here as
    `word += target segment base`, exactly the arithmetic the loader used
    to perform, so the container carries final addresses and no relocation
    records.  Returns the linked payload."""
    image = bytearray(packed.image)
    roles = [segment["role"] for segment in packed.segments]
    for segment in packed.segments:
        start = segment["payload_offset"]
        for index in range(segment["reloc_count"]):
            target, patch = packed.records[segment["reloc_start"] + index]
            at = start + patch
            value = image[at] | (image[at + 1] << 8)
            value += row[roles[target]]
            if value > 0xFFFF:
                raise ModError("%s: relocation at %04Xh overflows 16 bits"
                                % (packed.name, patch))
            image[at] = value & 0xFF
            image[at + 1] = (value >> 8) & 0xFF
    return image


def build_driver_container(packed, row):
    """Assemble the host-linked driver container: the flat plan header,
    the segment/endpoint/dependency/publication tables and the
    concatenated linked segment images.  `row` carries the host-planned
    absolute base of each segment role; every address field in the
    container is final (architecture 5.3) and no version is stored: the
    packer validates, the loader only checks bounds."""
    publications = list(getattr(packed, "publications", []))
    if len(publications) > MAX_PUBLICATIONS:
        raise ModError("%s: %d publications; the table holds %d"
                        % (packed.name, len(publications),
                           MAX_PUBLICATIONS))
    payload_offset = HEADER_SIZE + SEGMENT_SIZE * len(packed.segments) + \
        ENDPOINT_SIZE * len(packed.endpoints) + \
        DEPENDENCY_SIZE * len(packed.dependencies) + \
        PUBLICATION_SIZE * len(publications)
    image = link_driver_image(packed, row)
    total_size = payload_offset + len(image)
    if total_size > 0xFFFF:
        raise ModError("container exceeds the 16-bit size range")

    hot_base = row["hot"]
    init = (NO_ENTRY if packed.init_offset == NO_ENTRY
            else hot_base + packed.init_offset)

    header = bytearray(HEADER_SIZE)
    header[0:4] = MAGIC
    header[4] = len(packed.segments)
    header[5] = len(packed.endpoints)
    header[6] = len(packed.dependencies)
    header[7] = len(publications)
    put16(header, 0x08, init)
    put16(header, 0x0A, payload_offset)
    put16(header, 0x0C, total_size)
    put16(header, 0x0E, 0)

    body = bytearray()
    for segment in packed.segments:
        base = row[segment["role"]]
        entry = (NO_ENTRY if segment["entry_offset"] == NO_ENTRY
                 else base + segment["entry_offset"])
        record = bytearray(SEGMENT_SIZE)
        record[0x00] = ROLE_CODES[segment["role"]]
        record[0x01] = 0
        put16(record, 0x02, base)
        put16(record, 0x04, segment["image_size"])
        put16(record, 0x06, segment["bss_size"])
        put16(record, 0x08, entry)
        put16(record, 0x0A, segment["payload_offset"])
        body += record
    for endpoint in packed.endpoints:
        record = bytearray(ENDPOINT_SIZE)
        record[0x00] = endpoint["class"]
        record[0x01] = endpoint["instance"]
        record[0x02] = endpoint["version"]
        record[0x03] = endpoint["flags"]
        put16(record, 0x04, endpoint["number"])
        put16(record, 0x06, hot_base + endpoint["dispatch_offset"])
        body += record
    for dependency in packed.dependencies:
        record = bytearray(DEPENDENCY_SIZE)
        record[0x00] = dependency["class"]
        record[0x01] = dependency["instance"]
        put16(record, 0x02, dependency["number"])
        body += record
    for publication in publications:
        record = bytearray(PUBLICATION_SIZE)
        raw = publication["name"].encode("ascii")
        record[0:len(raw)] = raw
        record[0x10] = publication["type"]
        record[0x11] = publication["number"]
        record[0x12] = publication["object"]
        record[0x13] = 0
        body += record
    if len(body) != payload_offset - HEADER_SIZE:
        raise ModError("internal: container table size mismatch")
    return bytes(header + body + bytes(image))


def resolve_relocation_area(manifest, layout, symbols, obj_index, mode, ref):
    """(offset, area name) for an R group, so cross-segment references can
    be rejected: segments relocate against their own base only."""
    obj = manifest["objects"][obj_index]
    if (mode & R_SYM) == 0:
        if ref >= len(obj.areas):
            raise ModError("%s: relocation references area %d out of range"
                            % (obj.path, ref))
        name = obj.areas[ref].name
        key = (obj_index, name)
        if key not in layout["instance_offset"]:
            raise ModError("%s: relocation references unlisted area %s"
                            % (obj.path, name))
        return layout["instance_offset"][key], name

    if ref >= len(obj.symbols):
        raise ModError("%s: relocation references symbol %d out of range"
                        % (obj.path, ref))
    symbol = obj.symbols[ref]
    if symbol.kind == "def":
        if symbol.area is None:
            raise ModError("%s: relocation against absolute symbol %s"
                            % (obj.path, symbol.name))
        name = obj.areas[symbol.area].name
        key = (obj_index, name)
        if key not in layout["instance_offset"]:
            raise ModError("%s: symbol %s lives in unlisted area %s"
                            % (obj.path, symbol.name, name))
        return layout["instance_offset"][key] + symbol.value, name

    entry = symbols.get(symbol.name)
    if entry is None:
        raise ModError("%s: unresolved external symbol %s"
                        % (obj.path, symbol.name))
    def_obj, def_area, def_value = entry
    key = (def_obj, def_area)
    if key not in layout["instance_offset"]:
        raise ModError("%s: symbol %s lives in unlisted area %s"
                        % (obj.path, symbol.name, def_area))
    return layout["instance_offset"][key] + def_value, def_area


def put16(buf, offset, value):
    buf[offset] = value & 0xFF
    buf[offset + 1] = (value >> 8) & 0xFF


def get16(buf, offset):
    return buf[offset] | (buf[offset + 1] << 8)


# --------------------------------------------------------------------------
# Container validation/dump (mirrors the target loader rules)
# --------------------------------------------------------------------------

def parse_container(data, source="<container>"):
    """Validate one driver container against the flat plan contract and
    return its summary.  This is the host's authoritative validation: the
    container carries no version, so every field is checked here once (the
    target loader only bounds what protects the nucleus)."""
    if len(data) < HEADER_SIZE:
        raise ModError("%s: shorter than the %d-byte header"
                        % (source, HEADER_SIZE))
    if data[0:4] != MAGIC:
        raise ModError("%s: bad magic" % source)
    segment_count = data[4]
    endpoint_count = data[5]
    dependency_count = data[6]
    publication_count = data[7]
    init = get16(data, 0x08)
    payload_offset = get16(data, 0x0A)
    total_size = get16(data, 0x0C)
    if get16(data, 0x0E) != 0:
        raise ModError("%s: the header's reserved word is not zero"
                        % source)
    if segment_count == 0 or segment_count > MAX_SEGMENTS:
        raise ModError("%s: bad segment count %d" % (source, segment_count))
    if endpoint_count > MAX_ENDPOINTS:
        raise ModError("%s: bad endpoint count %d"
                        % (source, endpoint_count))
    if dependency_count > MAX_DEPENDENCIES:
        raise ModError("%s: bad dependency count %d"
                        % (source, dependency_count))
    if publication_count > MAX_PUBLICATIONS:
        raise ModError("%s: bad publication count %d"
                        % (source, publication_count))
    if total_size != len(data):
        raise ModError("%s: total_size %d != container length %d"
                        % (source, total_size, len(data)))
    expected_payload = HEADER_SIZE + SEGMENT_SIZE * segment_count + \
        ENDPOINT_SIZE * endpoint_count + \
        DEPENDENCY_SIZE * dependency_count + \
        PUBLICATION_SIZE * publication_count
    if payload_offset != expected_payload:
        raise ModError("%s: payload_offset %04Xh is not the canonical "
                        "table layout end %04Xh"
                        % (source, payload_offset, expected_payload))
    if payload_offset > total_size:
        raise ModError("%s: payload starts past the container" % source)

    segments = []
    roles = set()
    image_size = 0
    bss_size = 0
    hot_base = None
    bulk_base = 0
    bulk_size = 0
    for index in range(segment_count):
        at = HEADER_SIZE + SEGMENT_SIZE * index
        role_code = data[at]
        if role_code not in ROLE_NAMES:
            raise ModError("%s: segment %d has an unknown role %d"
                            % (source, index, role_code))
        role = ROLE_NAMES[role_code]
        if role in roles:
            raise ModError("%s: duplicate segment role %s" % (source, role))
        roles.add(role)
        if data[at + 1] != 0:
            raise ModError("%s: segment %d flags are not zero"
                            % (source, index))
        base = get16(data, at + 0x02)
        segment_image = get16(data, at + 0x04)
        segment_bss = get16(data, at + 0x06)
        entry = get16(data, at + 0x08)
        segment_payload = get16(data, at + 0x0A)
        if segment_image + segment_bss == 0:
            raise ModError("%s: segment %s is empty" % (source, role))
        if segment_payload + segment_image > total_size - payload_offset:
            raise ModError("%s: segment %s overruns the payload"
                            % (source, role))
        tier_base, tier_end = TIERS[role]
        if base < tier_base or base + segment_image + segment_bss > tier_end:
            raise ModError("%s: segment %s at %04Xh does not fit its tier"
                            % (source, role, base))
        if entry != NO_ENTRY and (role != "boot" or
                                  entry < base or
                                  entry >= base + segment_image):
            raise ModError("%s: segment %s carries a bad entry %04Xh"
                            % (source, role, entry))
        if role == "hot":
            hot_base = base
        if role == "bulk":
            bulk_base = base
            bulk_size = segment_image + segment_bss
        image_size += segment_image
        bss_size += segment_bss
        segments.append({
            "role": role,
            "base": base,
            "image_size": segment_image,
            "bss_size": segment_bss,
            "entry": entry,
            "payload_offset": segment_payload,
        })
    if "hot" not in roles:
        raise ModError("%s: a driver image needs a hot segment" % source)
    if init != NO_ENTRY and (init < hot_base or
                             init >= hot_base + next(
                                 segment["image_size"]
                                 for segment in segments
                                 if segment["role"] == "hot")):
        raise ModError("%s: the init entry %04Xh is outside the hot image"
                        % (source, init))

    endpoints = []
    for index in range(endpoint_count):
        at = HEADER_SIZE + SEGMENT_SIZE * segment_count + \
            ENDPOINT_SIZE * index
        endpoint = {
            "class": data[at],
            "instance": data[at + 1],
            "version": data[at + 2],
            "flags": data[at + 3],
            "number": get16(data, at + 0x04),
            "dispatch": get16(data, at + 0x06),
        }
        if endpoint["number"] == 0:
            raise ModError("%s: endpoint %d has number 0"
                            % (source, index))
        if endpoint["flags"] & ~EP_FLAGS:
            raise ModError("%s: endpoint %d carries unknown flags %02Xh"
                            % (source, index, endpoint["flags"]))
        if endpoint["dispatch"] < hot_base or \
                endpoint["dispatch"] >= hot_base + next(
                    segment["image_size"] for segment in segments
                    if segment["role"] == "hot"):
            raise ModError("%s: endpoint %d dispatch %04Xh is outside the "
                            "hot image" % (source, index,
                                           endpoint["dispatch"]))
        endpoints.append(endpoint)
    highest = max((endpoint["number"] for endpoint in endpoints), default=0)

    dependencies = []
    for index in range(dependency_count):
        at = HEADER_SIZE + SEGMENT_SIZE * segment_count + \
            ENDPOINT_SIZE * endpoint_count + DEPENDENCY_SIZE * index
        number = get16(data, at + 0x02)
        if number > highest:
            raise ModError("%s: dependency %d resolves to endpoint %d, "
                            "which no image exports"
                            % (source, index, number))
        dependencies.append({
            "class": data[at],
            "instance": data[at + 1],
            "number": number,
        })

    publications = []
    for index in range(publication_count):
        at = HEADER_SIZE + SEGMENT_SIZE * segment_count + \
            ENDPOINT_SIZE * endpoint_count + \
            DEPENDENCY_SIZE * dependency_count + PUBLICATION_SIZE * index
        raw = data[at:at + PUBLICATION_NAME]
        length = raw.find(b"\x00")
        if length <= 0 or length >= PUBLICATION_NAME or raw[0:1] != b"/":
            raise ModError("%s: publication %d has a bad name"
                            % (source, index))
        name = raw[:length].decode("ascii")
        publication = {
            "name": name,
            "type": data[at + 0x10],
            "number": data[at + 0x11],
            "object": data[at + 0x12],
            "flags": data[at + 0x13],
        }
        if publication["type"] == 0 or publication["number"] == 0 or \
                publication["flags"] != 0:
            raise ModError("%s: publication %d has a bad record"
                            % (source, index))
        publications.append(publication)

    return {
        "source": source,
        "module_type": "driver",
        "total_size": total_size,
        "payload_offset": payload_offset,
        "payload": data[payload_offset:],
        "image_size": image_size,
        "bss_size": bss_size,
        "init_offset": init,
        "segments": segments,
        "endpoints": endpoints,
        "dependencies": dependencies,
        "publications": publications,
    }


def format_report(info, name=None):
    lines = []
    label = name if name else info["source"]
    lines.append("%s" % label)
    lines.append("  format:        MOD driver image (host-linked plan)")
    lines.append("  image:         %d bytes" % info["image_size"])
    lines.append("  bss:           %d bytes" % info["bss_size"])
    if info["init_offset"] == NO_ENTRY:
        lines.append("  init:          none")
    else:
        lines.append("  init:          %04Xh" % info["init_offset"])
    lines.append("  total:         %d bytes" % info["total_size"])
    for segment in info["segments"]:
        entry = ""
        if segment["entry"] != NO_ENTRY:
            entry = ", entry %04Xh" % segment["entry"]
        lines.append("  %-5s segment: base %04Xh, image %d + bss %d,"
                     " payload +0x%04X%s"
                     % (segment["role"], segment["base"],
                        segment["image_size"], segment["bss_size"],
                        segment["payload_offset"], entry))
    for endpoint in info["endpoints"]:
        lines.append("  endpoint:      %d.%d v%d number %d dispatch "
                     "%04Xh%s"
                     % (endpoint["class"], endpoint["instance"],
                        endpoint["version"], endpoint["number"],
                        endpoint["dispatch"],
                        " reentrant" if endpoint["flags"] else ""))
    for dependency in info["dependencies"]:
        if dependency["number"] == 0:
            target = "none (optional, not in this set)"
        else:
            target = "endpoint %d" % dependency["number"]
        lines.append("  depends on:    %d.%d -> %s"
                     % (dependency["class"], dependency["instance"],
                        target))
    for publication in info.get("publications", []):
        lines.append("  publishes:     %-15s type %d endpoint %d "
                     "object %d"
                     % (publication["name"], publication["type"],
                        publication["number"], publication["object"]))
    return "\n".join(lines)


def report_json(info, name, packed=None):
    report = {
        "name": name,
        "module_type": "driver",
        "image_size": info["image_size"],
        "bss_size": info["bss_size"],
        "init_offset": info["init_offset"],
        "total_size": info["total_size"],
        "image_sha256": hashlib.sha256(info["payload"]).hexdigest(),
        "segments": info["segments"],
        "endpoints": info["endpoints"],
        "dependencies": info["dependencies"],
        "publications": info.get("publications", []),
    }
    if packed is not None:
        report["symbols"] = dict(sorted(packed.symbols.items()))
        if packed.symbol_segments:
            report["symbol_segments"] = dict(
                sorted(packed.symbol_segments.items()))
    return report


# --------------------------------------------------------------------------
# Commands
# --------------------------------------------------------------------------

def set_member_location(spec, objdir):
    """Split one --set entry into (manifest path, object directory).

    `[OBJDIR:]MANIFEST` names the manifest's object directory explicitly,
    so a set that mixes the production images (one module directory) with
    an acceptance image's synthetic images (another) links as one plan;
    a plain path uses the command's --objdir."""
    if ":" in spec:
        member_objdir, path = spec.split(":", 1)
        return path, member_objdir
    return spec, objdir


def pack_driver_set_member(manifest_path, manifest, set_paths, objdir):
    """Link one driver image against its whole set (architecture 5.3).

    The set is the load order: every manifest listed by `--set`, this one
    included.  The packer plans the bank-6 tiers, assigns endpoint numbers,
    resolves the symbolic dependencies and links this image's segments at
    their final addresses; the other images in the set are linked by their
    own invocations, with the same plan."""
    if not set_paths:
        raise ModError("%s: a driver image is linked against its whole "
                        "set: pass --set with every manifest in load order"
                        % manifest_path)
    self_abs = os.path.abspath(manifest_path)
    packs = []
    index_of_self = None
    for spec in set_paths:
        path, member_objdir = set_member_location(spec, objdir)
        loaded = manifest if os.path.abspath(path) == self_abs \
            else load_manifest(path, member_objdir)
        if loaded["kind"] != "driver":
            raise ModError("%s: not a driver manifest" % path)
        if os.path.abspath(path) == self_abs:
            if index_of_self is not None:
                raise ModError("%s: listed twice in --set" % manifest_path)
            index_of_self = len(packs)
        packs.append(pack_driver_manifest(loaded))
    if index_of_self is None:
        raise ModError("%s: the manifest is not in its own --set"
                        % manifest_path)
    rows, _ledger = plan_driver_set(packs)
    packed = packs[index_of_self]
    return build_driver_container(packed, rows[index_of_self]), packed


def cmd_pack(args):
    if len(args) < 2:
        raise ModError("usage: modpack.py pack <manifest> <out.mod> "
                        "[--objdir DIR] [--set MANIFEST ...] [--report FILE]")
    manifest_path, out_path = args[0], args[1]
    objdir = None
    report_path = None
    set_paths = []
    rest = args[2:]
    while rest:
        flag = rest.pop(0)
        if flag == "--objdir" and rest:
            objdir = rest.pop(0)
        elif flag == "--report" and rest:
            report_path = rest.pop(0)
        elif flag == "--set":
            while rest and not rest[0].startswith("--"):
                set_paths.append(rest.pop(0))
            if not set_paths:
                raise ModError("pack: --set needs at least one manifest")
        else:
            raise ModError("unknown pack option %r" % flag)

    manifest = load_manifest(manifest_path, objdir)
    container, packed = pack_driver_set_member(
        manifest_path, manifest, set_paths, objdir)
    info = parse_container(container, out_path)

    with open(out_path, "wb") as handle:
        handle.write(container)
    if report_path:
        with open(report_path, "w") as handle:
            json.dump(report_json(info, manifest["name"], packed), handle,
                      indent=2, sort_keys=True)
            handle.write("\n")

    print(format_report(info, out_path))
    return 0


def cmd_validate(args):
    if not args:
        raise ModError("usage: modpack.py validate <container.mod> [...]")
    for path in args:
        data = open(path, "rb").read()
        info = parse_container(data, path)
        print("modpack: %s: OK (%d image bytes, %d bss, host-linked)"
              % (path, info["image_size"], info["bss_size"]))
    return 0


def cmd_dump(args):
    if not args:
        raise ModError("usage: modpack.py dump <container.mod> [...]")
    for path in args:
        data = open(path, "rb").read()
        info = parse_container(data, path)
        print(format_report(info, path))
    return 0


def align2(value):
    return (value + 1) & ~1


def align2_down(value):
    return value & ~1


class DriverTierPlan:
    """The bank-6 tier allocation the loader performs (architecture 5.2).

    Pass one reserves the driver I/O buffers first: every image's io
    segment gets a slot inside 8000h-BFFFh, packed contiguously from C000h
    down, so the whole block [io_low, C000h) is known before any code is
    placed.  Pass two places the hot segments largest-first: each takes the
    lowest free run of the hot tier that does not cross the reserved io
    block (below it when it fits, otherwise just above C000h).  A
    descending block starts its own size below the cursor, rounded *down*
    to an even address, so the block below it can never overlap.  The boot
    segments bump up from 0000h and the bulk data down from 6000h.  The
    packer runs this policy so a set that packs always places; the loader
    is the runtime authority."""

    def __init__(self):
        self.io_cursor = IO_END
        self.hot_cursor = HOT_TIER_BASE
        self.boot_cursor = BOOT_TIER_BASE
        self.bulk_cursor = BOOT_TIER_END

    def reserve_io(self, size):
        if size == 0:
            return None
        base = align2_down(self.io_cursor - size)
        if base < IO_BASE:
            raise ModError(
                "driver I/O data needs %d bytes; the %04Xh-%04Xh range "
                "holds %d" % (size, IO_BASE, IO_END,
                              IO_END - IO_BASE))
        self.io_cursor = base
        return base

    def place_hot(self, size):
        if size == 0:
            return None
        base = align2(self.hot_cursor)
        if base + size > self.io_cursor and base < IO_END:
            base = IO_END
        if base + size > HOT_TIER_END:
            raise ModError(
                "hot segments need %d more bytes; the tier %04Xh-%04Xh is "
                "full" % (base + size - HOT_TIER_END, HOT_TIER_BASE,
                          HOT_TIER_END))
        self.hot_cursor = base + size
        return base

    def place_boot(self, size):
        if size == 0:
            return None
        base = align2(self.boot_cursor)
        if base + size > self.bulk_cursor:
            raise ModError(
                "boot segments need %d more bytes; the tier %04Xh-%04Xh is "
                "full" % (base + size - self.bulk_cursor, BOOT_TIER_BASE,
                          BOOT_TIER_END))
        self.boot_cursor = base + size
        return base

    def place_bulk(self, size):
        if size == 0:
            return None
        base = align2_down(self.bulk_cursor - size)
        if base < self.boot_cursor:
            raise ModError(
                "bulk data needs %d more bytes; the tier %04Xh-%04Xh is "
                "full" % (self.boot_cursor - base, BOOT_TIER_BASE,
                          BOOT_TIER_END))
        self.bulk_cursor = base
        return base


def driver_segments(pack):
    return {segment["role"]: segment for segment in pack.segments}


def segment_size(segment):
    return segment["image_size"] + segment["bss_size"]


def plan_driver_set(packs):
    """Host assembly for one image's driver set (architecture 5.3).

    Places every image's segments with the existing tier plan, assigns the
    host endpoint numbers in load order and table order, and resolves each
    image's symbolic dependencies to the typed endpoint reference of an
    earlier (or the image's own) export.  Duplicate-image,
    duplicate-endpoint, tier-fit and overlap checks all run here.  Returns
    (rows, ledger); each pack carries its resolved dependency numbers and
    each endpoint its host number."""
    if not packs:
        raise ModError("drivers: no images given")
    if len(packs) > MAX_IMAGES:
        raise ModError("drivers: %d images; the loader holds %d"
                        % (len(packs), MAX_IMAGES))
    names = [pack.name for pack in packs]
    lowered = [name.lower() for name in names]
    if len(set(lowered)) != len(lowered):
        raise ModError("drivers: duplicate image name in %s"
                        % ", ".join(names))

    seen = {}
    for pack in packs:
        for endpoint in pack.endpoints:
            key = (endpoint["class"], endpoint["instance"])
            if key in seen:
                raise ModError(
                    "%s: endpoint %d.%d is already exported by %s"
                    % (pack.name, key[0], key[1], seen[key]))
            seen[key] = pack.name

    plan = DriverTierPlan()
    rows = [{"name": pack.name} for pack in packs]
    for row, pack in zip(rows, packs):
        io = driver_segments(pack).get("io")
        row["io"] = plan.reserve_io(segment_size(io) if io else 0)

    hot_order = sorted(
        range(len(packs)),
        key=lambda index: (-segment_size(
            driver_segments(packs[index])["hot"])))
    for index in hot_order:
        segment = driver_segments(packs[index])["hot"]
        rows[index]["hot"] = plan.place_hot(segment_size(segment))
    for row, pack in zip(rows, packs):
        segments = driver_segments(pack)
        boot = segments.get("boot")
        bulk = segments.get("bulk")
        row["boot"] = plan.place_boot(segment_size(boot) if boot else 0)
        row["bulk"] = plan.place_bulk(segment_size(bulk) if bulk else 0)

    check_tier_overlap(rows, packs)

    # Host-assigned endpoint numbers: load order, endpoint-table order.
    number = 1
    for pack in packs:
        for endpoint in pack.endpoints:
            endpoint["number"] = number
            number += 1
    if number - 1 > MAX_ENDPOINTS * MAX_IMAGES:
        raise ModError("drivers: %d endpoints; the nucleus table holds %d"
                        % (number - 1,
                           MAX_ENDPOINTS * MAX_IMAGES))

    # Symbolic dependencies: resolved to the typed endpoint reference of
    # an export by this image or an earlier one in the load order.  An
    # optional dependency that no image exports resolves to 0; a missing
    # required one is a build error (architecture 5.3: no fallback).
    for index, pack in enumerate(packs):
        for dependency in pack.dependencies:
            key = (dependency["class"], dependency["instance"])
            resolved = 0
            for earlier in range(index + 1):
                for endpoint in packs[earlier].endpoints:
                    if (endpoint["class"], endpoint["instance"]) == key:
                        resolved = endpoint["number"]
                        break
                if resolved != 0:
                    break
            if resolved == 0 and not dependency["optional"]:
                raise ModError(
                    "%s: dependency %d.%d is not exported by an earlier "
                    "image in the load order"
                    % (pack.name, key[0], key[1]))
            dependency["number"] = resolved

    # Publications: every image's declared namespace
    # names, with the serving endpoint resolved the same way a dependency
    # is - to an export by this image or an earlier one in the load order.
    seen_paths = {}
    parent_rows = set()
    published = 0
    for index, pack in enumerate(packs):
        for publication in pack.publications:
            key = (publication["class"], publication["instance"])
            resolved = 0
            for earlier in range(index + 1):
                for endpoint in packs[earlier].endpoints:
                    if (endpoint["class"], endpoint["instance"]) == key:
                        resolved = endpoint["number"]
                        break
                if resolved != 0:
                    break
            root = (FS_ROOT if publication["name"] ==
                    FS_ROOT else DEVICE_ROOT)
            if resolved == 0:
                raise ModError(
                    "%s: publication %s names endpoint %d.%d, which no "
                    "image in the load order exports"
                    % (pack.name, root + publication["name"]
                       if root == DEVICE_ROOT
                       else publication["name"], key[0], key[1]))
            publication["number"] = resolved
            full = (root + publication["name"]).lower()
            if full in seen_paths:
                raise ModError("%s: %s is already published by %s"
                                % (pack.name, full, seen_paths[full]))
            seen_paths[full] = pack.name
            published += 1
            name = publication["name"]
            if root == DEVICE_ROOT and "/" in name[1:]:
                parent_rows.add(name.rsplit("/", 1)[0].lower())
    if published + len(parent_rows) > MAX_PUBLICATIONS:
        raise ModError(
            "drivers: %d publications and %d grouping rows; the "
            "provider table takes %d rows"
            % (published, len(parent_rows), MAX_PUBLICATIONS))

    ledger = driver_ledger(packs, rows)
    return rows, ledger


def _entry_name(entry):
    return entry["name"] if isinstance(entry, dict) else entry.name


def _entry_segments(entry):
    return entry["segments"] if isinstance(entry, dict) else entry.segments


def check_tier_overlap(rows, entries):
    """Every placed block must lie inside its tier and no two blocks may
    overlap (architecture 5.2, fixed driver-bank layout)."""
    for role in ("hot", "io", "boot", "bulk"):
        tier_base, tier_end = TIERS[role]
        blocks = []
        for row, entry in zip(rows, entries):
            for segment in _entry_segments(entry):
                if segment["role"] != role:
                    continue
                base = row[role]
                size = segment_size(segment)
                if base is None or base < tier_base or \
                        base + size > tier_end:
                    raise ModError("%s: %s segment outside its tier"
                                    % (_entry_name(entry), role))
                blocks.append((base, size, _entry_name(entry)))
        blocks.sort()
        end = tier_base
        for base, size, name in blocks:
            if base < end:
                raise ModError("%s: %s segment at %04Xh overlaps the "
                                "previous block" % (name, role, base))
            end = base + size


def driver_ledger(entries, rows):
    """The architecture-9 `driver hot` and `driver boot/bulk` ledgers:
    the placed bytes of each tier, taken from the plan rows."""
    hot_used = 0
    boot_used = 0
    for row, entry in zip(rows, entries):
        for segment in _entry_segments(entry):
            size = segment_size(segment)
            if segment["role"] in ("hot", "io"):
                hot_used += size
            else:
                boot_used += size
    return {
        "driver_hot_bytes": hot_used,
        "driver_hot_capacity_bytes": HOT_TIER_END - HOT_TIER_BASE,
        "driver_boot_bytes": boot_used,
        "driver_boot_capacity_bytes": BOOT_TIER_END - BOOT_TIER_BASE,
    }


def format_driver_row(row):
    parts = []
    for role in ("hot", "io", "boot", "bulk"):
        if row.get(role) is not None:
            parts.append("%s %04Xh" % (role, row[role]))
    return "driver %s: %s" % (row["name"], " ".join(parts))


def cmd_plan(args):
    if len(args) < 2:
        raise ModError("usage: modpack.py plan <out.plan.json> "
                        "<container.mod> [...]")
    out_path = args[0]
    containers = list(args[1:])

    infos = []
    names = []
    rows = []
    seen_key = {}
    seen_number = {}
    number_key = {}
    for path in containers:
        data = open(path, "rb").read()
        info = parse_container(data, path)
        if info.get("module_type") != "driver":
            raise ModError("%s: not a driver image" % path)
        name = os.path.basename(path)
        if name.lower() in [existing.lower() for existing in names]:
            raise ModError("%s: duplicate image name %r" % (path, name))
        row = {"name": name}
        for segment in info["segments"]:
            row[segment["role"]] = segment["base"]
        for endpoint in info["endpoints"]:
            key = (endpoint["class"], endpoint["instance"])
            number = endpoint["number"]
            if key in seen_key:
                raise ModError(
                    "%s: endpoint %d.%d is already exported by %s"
                    % (path, key[0], key[1], seen_key[key]))
            if number in seen_number:
                raise ModError(
                    "%s: endpoint number %d is already used by %s"
                    % (path, number, seen_number[number]))
            seen_key[key] = path
            seen_number[number] = path
            number_key[number] = key
        for dependency in info["dependencies"]:
            key = (dependency["class"], dependency["instance"])
            number = dependency["number"]
            if number == 0:
                # An optional dependency no image exports: the driver's
                # host-configured fallback (architecture 5.3).
                continue
            if number not in number_key:
                raise ModError(
                    "%s: dependency %d.%d resolved to endpoint %d, which no "
                    "earlier image exports"
                    % (path, key[0], key[1], number))
            if number_key[number] != key:
                raise ModError(
                    "%s: dependency %d.%d resolved to endpoint %d of "
                    "protocol %d.%d"
                    % (path, key[0], key[1], number,
                       number_key[number][0], number_key[number][1]))
        infos.append(info)
        names.append(name)
        rows.append(row)
    if len(infos) > MAX_IMAGES:
        raise ModError("plan: %d images; the loader holds %d"
                        % (len(infos), MAX_IMAGES))

    placed = [{"name": name, "segments": info["segments"]}
              for name, info in zip(names, infos)]
    check_tier_overlap(rows, placed)
    ledger = driver_ledger(placed, rows)

    for row in rows:
        print("modpack: %s" % format_driver_row(row))
    print("modpack: %s" % (pack_image.DRIVER_HOT_LEDGER_LINE % (
        ledger["driver_hot_bytes"], ledger["driver_hot_capacity_bytes"])))
    print("modpack: %s" % (pack_image.DRIVER_BOOT_LEDGER_LINE % (
        ledger["driver_boot_bytes"], ledger["driver_boot_capacity_bytes"])))
    print("modpack: %s: %d driver images, load order as listed"
          % (out_path, len(infos)))

    report = {
        "format": "kos-drivers/1",
        "order": names,
        "drivers": [
            {
                "name": name,
                "container": path,
                "hot_base": row.get("hot"),
                "io_base": row.get("io"),
                "boot_base": row.get("boot"),
                "bulk_base": row.get("bulk"),
                "endpoints": info["endpoints"],
                "dependencies": info["dependencies"],
            }
            for name, path, row, info in zip(names, containers, rows, infos)
        ],
        "nucleus": ledger,
    }
    with open(out_path, "w") as handle:
        json.dump(report, handle, indent=2, sort_keys=True)
        handle.write("\n")
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    command = argv[1]
    try:
        if command == "pack":
            return cmd_pack(argv[2:])
        if command == "validate":
            return cmd_validate(argv[2:])
        if command == "dump":
            return cmd_dump(argv[2:])
        if command == "plan":
            return cmd_plan(argv[2:])
        raise ModError("unknown command %r" % command)
    except ModError as exc:
        print("modpack: error: %s" % exc, file=sys.stderr)
        return 1
    except OSError as exc:
        print("modpack: error: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
