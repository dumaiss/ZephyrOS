#!/usr/bin/env python3
"""ZephyrOS M4 ZMOD packer/validator.

Consumes SDCC/sdasz80 relocatable (.rel) output plus a small JSON module
manifest and produces a deterministic ZMOD v1 driver container.  The
relocation records in the .rel files are the single source of truth: the
packer mini-links the listed objects at base zero (module-relative
offsets), patches every module-local 16-bit absolute reference, and emits
one ZREL_ABS16 record per patched word.  Pointer inference by scanning
image bytes never happens.

A second module type, the M10 **resident service image** (module_type 2),
carries the same header and relocation records plus a *service section*:
segments (hot / boot / io / bulk), their destination tiers, the exported
endpoints and the declared dependencies.  `pack` validates and
pre-computes that layout; `services` validates a whole load-order set
(dependency closure, tier allocation) and emits the flat boot manifest.

Supported relocation subset (anything else is rejected):

    mode 0x00  word, relocatable area       -> module-local area base
    mode 0x02  word, symbol                 -> module-local symbol

Rejected constructs include byte-fragmented address stores (SDCC emits
these for stack-built descriptors), PC-relative/8-bit/3-byte/paged
relocations, unresolved externals, and patches outside the image.

Driver container layout (all little-endian):

    offset 0                      28-byte header
    offset reloc_offset           4 * reloc_count relocation records
                                  (type=1 ZREL_ABS16, target=0,
                                   patch_offset:u16)
    offset payload_offset         linked-at-zero image
    total_size = payload_offset + image_size

Service container layout:

    offset 0                      28-byte header (module_type = 2)
    offset 28                     service section: 16-byte header,
                                  segment/endpoint/dependency tables
    offset reloc_offset           4 * reloc_count relocation records
                                  (type=1 ZREL_ABS16, the second byte
                                   names the target segment, patch
                                   offsets relative to their segment's
                                   image)
    offset payload_offset         segment images, concatenated
    total_size = payload_offset + image_size (all segment images)

Driver manifest (JSON), paths resolved against --objdir (default:
manifest dir):

    {
      "name": "m4.probe",
      "type": "driver",              # only "driver"
      "abi": 3,                      # driver ABI v3
      "flags": 0,
      "objects": ["m4_probe.rel", "m4_sdcc_glue.rel"],
      "image_areas": ["_CODE"],      # concatenated in this order
      "bss_areas": ["_DATA"],        # zeroed by the loader, not loaded
      "init": "m4_probe_init",       # required, must be in an image area
      "fini": null                   # optional cleanup entry
    }

Service manifest (JSON):

    {
      "name": "ioc",
      "type": "service",
      "abi": 2,                      # service ABI v2
      "flags": 0,
      "objects": ["ioc.rel", "ioc_boot.rel", "sdcc_indirect_calls.rel"],
      "init": "ipc_ioc_init",        # hot-segment init, called with the
                                     # bootstrap table; optional
      "segments": [
        {"role": "hot",  "image_areas": ["_CODE"], "bss_areas": []},
        {"role": "boot", "image_areas": ["_BOOT"],
         "entry": "ipc_ioc_boot"},   # boot entry, run once; optional
        {"role": "io",   "bss_areas": ["_SERVICE_IO"]}
      ],
      "endpoints": [
        {"class": 5, "instance": 0, "version": 1,
         "dispatch": "ioc_dispatch", "flags": 0}
      ],
      "depends": []                  # [{"class": 4, "instance": 0}, ...]
    }

Usage:
    zmodpack.py pack <manifest.json> <out.zmod> [--objdir DIR]
                     [--report FILE]
    zmodpack.py validate <container.zmod> [...]
    zmodpack.py dump <container.zmod> [...]
    zmodpack.py emit-asm <out.asm> SYMBOL=container.zmod [...]
    zmodpack.py services <out.man> <container.zmod> [...] [--report FILE]
    zmodpack.py plan --geometry <memory.inc> [--report FILE]
                     [<container.zmod> ...]

The `services` command validates a whole set in load order (unique
endpoints, resolvable dependencies, a tier allocation that fits) and
writes the flat boot manifest: a sequence of 24-byte name records, the
ROMFS name field exactly.  The `plan` command reports every module's
image/BSS/granules, the boot-set totals against the derived arena
declared by the generated geometry memory include, and the single bank-7
budget ledger line.  It fails when the declared boot set cannot coexist
in the arena.
"""

import hashlib
import json
import os
import re
import sys

if os.path.dirname(os.path.abspath(__file__)) not in sys.path:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pack_image  # shared budget-ledger format (one definition)

HEADER_SIZE = 28
RELOC_SIZE = 4
FORMAT_VERSION = 1
TYPE_DRIVER = 1
TYPE_SERVICE = 2
FLAG_NONE = 0
ZREL_ABS16 = 1
NO_ENTRY = 0xFFFF
GRANULE_SIZE = 512
# Format maximum: (ZMOD_ARCH_END - ZMOD_ARCH_BASE) / GRANULE_SIZE = 95.  The
# package-38 layout caps the real arena at the cold half's 64 granules; the
# live bound is the linked arena, checked against the boot-set plan.
MAX_GRANULES = 95
DRIVER_ABI_VERSION = 3
A_BYTES = 4

# Service images (M10 stage 5, architecture 36.6/36.11): the same header
# plus the service section, with every segment linked at its final bank-6
# address.  The packer resolves the tier plan, the endpoint numbers and
# the symbolic inter-driver dependencies on the host; the container
# carries no relocation records.  The constants mirror
# include/zephyros/service.h and are pinned by host tests.
SERVICE_ABI_VERSION = 3
# ABI 4 (work package 42a) adds the publication table; a container whose
# manifest declares no publications stays ABI 3, byte for byte.
SERVICE_ABI_PUBLICATIONS = 4
SERVICE_SECTION_OFFSET = HEADER_SIZE
SERVICE_HEADER_SIZE = 16
SERVICE_SEGMENT_SIZE = 14
SERVICE_ENDPOINT_SIZE = 8
SERVICE_DEPENDENCY_SIZE = 4
SERVICE_PUBLICATION_SIZE = 20
SERVICE_PUBLICATION_NAME = 16
SERVICE_MAX_PUBLICATIONS = 12
# The DEVICE provider's root (architecture 36.13): a manifest declares the
# full namespace path and the container carries the provider-relative
# remainder, which is what the provider resolves.
SERVICE_DEVICE_ROOT = "/dev"
# The filesystem-namespace root (architecture 36.14, work package 42b): a
# filesystem driver publishes `/FS` itself, so its publication path is the
# provider root rather than a name below SERVICE_DEVICE_ROOT.  The record's
# name is the full path ("/FS"), which is what the FS provider matches.
SERVICE_FS_ROOT = "/FS"
# The 8-bit protocol types a publication may name (abi/zos/object.h).
SERVICE_TYPES = {
    "SERIAL": 1, "VIDEO": 2, "BLOCK": 3, "INPUT": 4, "IOC": 5,
    "CONSOLE": 6, "FILE": 7, "FS": 8, "SOUND": 9, "TIMER": 10,
}
SERVICE_ROLE_CODES = {"hot": 1, "boot": 2, "io": 3, "bulk": 4}
SERVICE_ROLE_NAMES = {value: key for key, value in SERVICE_ROLE_CODES.items()}
SERVICE_MAX_SEGMENTS = 4
SERVICE_MAX_ENDPOINTS = 8
SERVICE_MAX_DEPENDENCIES = 8
SERVICE_MAX_IMAGES = 8
SERVICE_EP_REENTRANT = 0x01
# Dependency declaration flags (the manifest's `optional`): an optional
# dependency that no image in the set exports resolves to endpoint 0.
SERVICE_DEP_OPTIONAL = 0x01
# Destination tiers of each segment role (address range, end exclusive).
SERVICE_TIERS = {
    "hot": (0x6000, 0xE000),
    "io": (0x8000, 0xC000),
    "boot": (0x0000, 0x6000),
    "bulk": (0x0000, 0x6000),
}

# Bank-6 destination geometry (architecture 36.6), mirrored by
# kernel/service/service_loader.c: the hot tier 6000h-DFFFh (service I/O
# buffers first, inside 8000h-BFFFh), the boot/bulk tier 0000h-5FFFh.
HOT_TIER_BASE = 0x6000
HOT_TIER_END = 0xE000
SERVICE_IO_BASE = 0x8000
SERVICE_IO_END = 0xC000
BOOT_TIER_BASE = 0x0000
BOOT_TIER_END = 0x6000
# The boot manifest entry is the ROMFS name field exactly (romfs-format.md,
# include/zephyros/service.h).
MANIFEST_ENTRY_SIZE = 24

R_WORD = 0x00
R_BYTE = 0x01
R_SYM = 0x02
R_PCR = 0x04
R_PAG0 = 0x20
R_PAG = 0x40
R_ESCAPE_MASK = 0xF0
SUPPORTED_MODES = (R_WORD, R_SYM)

MAGIC = b"ZMOD"


class ZmodError(Exception):
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
                raise ZmodError("%s:%d: unsupported radix/address spec %r "
                                "(only XL4)" % (path, lineno, line))
        elif kind == "H":
            # H <areas-hex> areas <globals-hex> global symbols: sdasz80
            # writes both counts in hex (10 areas arrive as "A").
            try:
                header_counts = (int(fields[1], 16), int(fields[3], 16))
            except (IndexError, ValueError):
                raise ZmodError("%s:%d: malformed header line" % (path, lineno))
        elif kind == "M":
            obj.module = fields[1] if len(fields) > 1 else ""
        elif kind == "O":
            continue
        elif kind == "S":
            if len(fields) < 3 or len(fields[2]) < 4:
                raise ZmodError("%s:%d: malformed symbol line" % (path, lineno))
            name = fields[1]
            tag = fields[2][:3]
            try:
                value = int(fields[2][3:], 16)
            except ValueError:
                raise ZmodError("%s:%d: malformed symbol value" % (path, lineno))
            if tag == "Def":
                obj.symbols.append(Symbol(name, "def", value, current_area))
            elif tag == "Ref":
                obj.symbols.append(Symbol(name, "ref", 0, None))
            else:
                raise ZmodError("%s:%d: unknown symbol kind %r"
                                % (path, lineno, tag))
        elif kind == "A":
            # A <name> size <hex> flags <hex> addr <hex>
            try:
                name = fields[1]
                size = int(fields[3], 16)
                flags = int(fields[5], 16)
            except (IndexError, ValueError):
                raise ZmodError("%s:%d: malformed area line" % (path, lineno))
            current_area = len(obj.areas)
            obj.areas.append(Area(name, size, flags, current_area))
        elif kind == "T":
            if pending is not None:
                raise ZmodError("%s:%d: T line without matching R line"
                                % (path, lineno))
            try:
                values = [int(v, 16) for v in fields[1:]]
            except ValueError:
                raise ZmodError("%s:%d: malformed data byte" % (path, lineno))
            pending = values
        elif kind == "R":
            if pending is None:
                raise ZmodError("%s:%d: R line without preceding T line"
                                % (path, lineno))
            try:
                values = [int(v, 16) for v in fields[1:]]
            except ValueError:
                raise ZmodError("%s:%d: malformed relocation byte"
                                % (path, lineno))
            chunk = decode_chunk(pending, values, path, lineno)
            if chunk is not None:
                obj.chunks.append(chunk)
            pending = None
        else:
            raise ZmodError("%s:%d: unsupported record %r"
                            % (path, lineno, kind))

    if pending is not None:
        raise ZmodError("%s: trailing T line without R line" % path)

    if header_counts is not None:
        areas, symbols = header_counts
        if areas != len(obj.areas):
            raise ZmodError("%s: header declares %d areas, found %d"
                            % (path, areas, len(obj.areas)))
        if symbols != len(obj.symbols):
            raise ZmodError("%s: header declares %d symbols, found %d"
                            % (path, symbols, len(obj.symbols)))
    return obj


def decode_chunk(values, reloc_values, path, lineno):
    """Decode a T/R pair.  Returns None for an empty data chunk."""
    if len(values) < A_BYTES:
        raise ZmodError("%s:%d: T line shorter than the address field"
                        % (path, lineno))
    addr = (values[0] | (values[1] << 8) | (values[2] << 16) |
            (values[3] << 24))
    data = bytes(values[A_BYTES:])

    groups = decode_relocs(reloc_values, path, lineno)
    if not groups:
        raise ZmodError("%s:%d: R line without the initial area group"
                        % (path, lineno))

    init_mode, init_rtp, area_index = groups[0]
    if init_mode != R_WORD or init_rtp != 0:
        raise ZmodError("%s:%d: unsupported chunk header group (mode %02X)"
                        % (path, lineno, init_mode))
    relocs = []
    for mode, rtp, ref in groups[1:]:
        relocs.append((mode, rtp, ref))

    if not data:
        if relocs:
            raise ZmodError("%s:%d: relocation on an empty data chunk"
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
                raise ZmodError("%s:%d: truncated escaped relocation mode"
                                % (path, lineno))
            mode = ((mode & 0x0F) << 8) | values[pos + 1]
            pos += 2
        else:
            pos += 1
        if pos + 3 > len(values):
            raise ZmodError("%s:%d: truncated relocation group"
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
        self.relocs = []          # sorted patch offsets
        self.init_offset = 0
        self.fini_offset = NO_ENTRY
        self.symbols = {}         # module-relative Def offsets
        self.symbol_segments = {}  # service symbols: name -> segment role


def load_manifest(path, objdir):
    try:
        with open(path, "r") as handle:
            manifest = json.load(handle)
    except (OSError, ValueError) as exc:
        raise ZmodError("cannot read manifest %s: %s" % (path, exc))

    required = ("name", "objects")
    for field in required:
        if field not in manifest:
            raise ZmodError("manifest %s: missing %r" % (path, field))

    kind = manifest.get("type", "driver")
    if kind == "driver":
        return load_driver_manifest(manifest, path, objdir)
    if kind == "service":
        return load_service_manifest(manifest, path, objdir)
    raise ZmodError("manifest %s: unsupported module type %r" % (path, kind))


def read_manifest_objects(manifest, path, objdir):
    resolve_dir = objdir if objdir is not None else os.path.dirname(path)
    objects = []
    for name in manifest["objects"]:
        obj_path = name if os.path.isabs(name) else os.path.join(resolve_dir,
                                                                 name)
        try:
            text = open(obj_path).read()
        except OSError as exc:
            raise ZmodError("cannot read object %s: %s" % (obj_path, exc))
        objects.append(parse_rel(text, obj_path))
    return objects


def read_manifest_asset(segment, path, objdir):
    """Load one segment's raw image asset (a bulk-data payload).

    The asset is a binary file (the pinned V9958 font, work package 41):
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
        raise ZmodError("cannot read asset %s: %s" % (asset_path, exc))
    if not data:
        raise ZmodError("asset %s is empty" % asset_path)
    return data


def load_driver_manifest(manifest, path, objdir):
    if "image_areas" not in manifest or "init" not in manifest:
        raise ZmodError("manifest %s: missing 'image_areas' or 'init'"
                        % path)
    if int(manifest.get("abi", DRIVER_ABI_VERSION)) != DRIVER_ABI_VERSION:
        raise ZmodError("manifest %s: unsupported driver ABI %r"
                        % (path, manifest.get("abi")))
    if int(manifest.get("flags", FLAG_NONE)) != FLAG_NONE:
        raise ZmodError("manifest %s: unsupported flags %r"
                        % (path, manifest.get("flags")))

    image_areas = list(manifest["image_areas"])
    bss_areas = list(manifest.get("bss_areas", []))
    if not image_areas:
        raise ZmodError("manifest %s: image_areas must not be empty" % path)
    overlap = set(image_areas) & set(bss_areas)
    if overlap:
        raise ZmodError("manifest %s: area(s) both image and bss: %s"
                        % (path, ", ".join(sorted(overlap))))
    listed = image_areas + bss_areas
    if len(set(listed)) != len(listed):
        raise ZmodError("manifest %s: duplicate area names" % path)

    return {
        "kind": "driver",
        "name": str(manifest["name"]),
        "objects": read_manifest_objects(manifest, path, objdir),
        "object_names": [str(name) for name in manifest["objects"]],
        "image_areas": image_areas,
        "bss_areas": bss_areas,
        "init": manifest["init"],
        "fini": manifest.get("fini"),
        "path": path,
    }


def load_service_manifest(manifest, path, objdir):
    """Validate the service schema (segment roles, endpoints, symlinks
    resolved later against the area layout)."""
    if int(manifest.get("abi", SERVICE_ABI_VERSION)) != SERVICE_ABI_VERSION:
        raise ZmodError("manifest %s: unsupported service ABI %r"
                        % (path, manifest.get("abi")))
    if int(manifest.get("flags", FLAG_NONE)) != FLAG_NONE:
        raise ZmodError("manifest %s: unsupported flags %r"
                        % (path, manifest.get("flags")))

    segments = manifest.get("segments")
    if not isinstance(segments, list) or not segments:
        raise ZmodError("manifest %s: segments must be a non-empty list"
                        % path)
    if len(segments) > SERVICE_MAX_SEGMENTS:
        raise ZmodError("manifest %s: more than %d segments"
                        % (path, SERVICE_MAX_SEGMENTS))

    roles = []
    listed = []
    entries = []
    for index, segment in enumerate(segments):
        where = "%s: segment %d" % (path, index)
        role = segment.get("role")
        if role not in SERVICE_ROLE_CODES:
            raise ZmodError("%s: unknown role %r" % (where, role))
        if role in roles:
            raise ZmodError("%s: duplicate role %s" % (where, role))
        roles.append(role)
        image_areas = list(segment.get("image_areas", []))
        bss_areas = list(segment.get("bss_areas", []))
        overlap = set(image_areas) & set(bss_areas)
        if overlap:
            raise ZmodError("%s: area(s) both image and bss: %s"
                            % (where, ", ".join(sorted(overlap))))
        if "asset" in segment and (image_areas or bss_areas):
            raise ZmodError("%s: an asset segment takes no areas" % where)
        if "asset" not in segment and not image_areas and not bss_areas:
            raise ZmodError("%s: segment lists no areas and no asset"
                            % where)
        for name in image_areas + bss_areas:
            if name in listed:
                raise ZmodError("%s: area %s is claimed twice"
                                % (where, name))
            listed.append(name)
        entry = segment.get("entry")
        if entry is not None and role != "boot":
            raise ZmodError("%s: only the boot role carries an entry" % where)
        entries.append(entry)
    if "hot" not in roles:
        raise ZmodError("%s: a service image needs a hot segment" % path)

    endpoints = list(manifest.get("endpoints", []))
    if len(endpoints) > SERVICE_MAX_ENDPOINTS:
        raise ZmodError("manifest %s: more than %d endpoints"
                        % (path, SERVICE_MAX_ENDPOINTS))
    for index, endpoint in enumerate(endpoints):
        where = "%s: endpoint %d" % (path, index)
        for field in ("class", "instance", "version", "dispatch"):
            if field not in endpoint:
                raise ZmodError("%s: missing %r" % (where, field))
        for field in ("class", "instance", "version"):
            value = int(endpoint[field])
            if value < 0 or value > 0xFF:
                raise ZmodError("%s: %s out of range" % (where, field))
        flags = int(endpoint.get("flags", 0))
        if flags & ~SERVICE_EP_REENTRANT:
            raise ZmodError("%s: unknown flags %02Xh" % (where, flags))

    publishes = list(manifest.get("publishes", []))
    if len(publishes) > SERVICE_MAX_PUBLICATIONS:
        raise ZmodError("manifest %s: more than %d publications"
                        % (path, SERVICE_MAX_PUBLICATIONS))
    for index, publish in enumerate(publishes):
        where = "%s: publication %d" % (path, index)
        full = publish.get("path")
        if not isinstance(full, str):
            raise ZmodError("%s: path must be a string below %s/ or %s"
                            % (where, SERVICE_DEVICE_ROOT, SERVICE_FS_ROOT))
        if full == SERVICE_FS_ROOT:
            # The filesystem-namespace root: the whole volume is one
            # instance, so it names no object.
            if publish.get("type") != "FS":
                raise ZmodError("%s: %s must be published with type FS"
                                % (where, SERVICE_FS_ROOT))
            if int(publish.get("object", 0)) != 0:
                raise ZmodError("%s: %s is the provider root and names no "
                                "object" % (where, SERVICE_FS_ROOT))
            publish["type_code"] = SERVICE_TYPES["FS"]
            name = SERVICE_FS_ROOT
        elif full.startswith(SERVICE_DEVICE_ROOT + "/"):
            name = full[len(SERVICE_DEVICE_ROOT):]
            if len(name) + 1 > SERVICE_PUBLICATION_NAME:
                raise ZmodError("%s: %s is longer than %d provider-relative "
                                "bytes" % (where, full,
                                           SERVICE_PUBLICATION_NAME - 1))
            if name.endswith("/") or "//" in name:
                raise ZmodError("%s: %s is not a clean name"
                                % (where, full))
            for char in name:
                if not (0x21 <= ord(char) <= 0x7E):
                    raise ZmodError("%s: %s carries a non-printable byte"
                                    % (where, full))
            publish_type = publish.get("type")
            if isinstance(publish_type, str):
                if publish_type not in SERVICE_TYPES:
                    raise ZmodError("%s: unknown type %r"
                                    % (where, publish_type))
                publish["type_code"] = SERVICE_TYPES[publish_type]
            else:
                value = int(publish_type if publish_type is not None else 0)
                if value < 1 or value > 0xFF:
                    raise ZmodError("%s: type %r out of range"
                                    % (where, publish_type))
                publish["type_code"] = value
        else:
            raise ZmodError("%s: path must be a string below %s/ or %s"
                            % (where, SERVICE_DEVICE_ROOT, SERVICE_FS_ROOT))
        publish["name"] = name
        endpoint = publish.get("endpoint")
        if not isinstance(endpoint, dict) or "class" not in endpoint or \
                "instance" not in endpoint:
            raise ZmodError("%s: endpoint must name a class and an instance"
                            % where)
        for field in ("class", "instance"):
            value = int(endpoint[field])
            if value < 0 or value > 0xFF:
                raise ZmodError("%s: endpoint %s out of range"
                                % (where, field))
        publish["class"] = int(endpoint["class"])
        publish["instance"] = int(endpoint["instance"])
        object_id = int(publish.get("object", 0))
        if object_id < 0 or object_id > 0xFF:
            raise ZmodError("%s: object out of range" % where)
        publish["object"] = object_id

    depends = list(manifest.get("depends", []))
    if len(depends) > SERVICE_MAX_DEPENDENCIES:
        raise ZmodError("manifest %s: more than %d dependencies"
                        % (path, SERVICE_MAX_DEPENDENCIES))
    for index, dependency in enumerate(depends):
        where = "%s: dependency %d" % (path, index)
        for field in ("class", "instance"):
            if field not in dependency:
                raise ZmodError("%s: missing %r" % (where, field))
            value = int(dependency[field])
            if value < 0 or value > 0xFF:
                raise ZmodError("%s: %s out of range" % (where, field))
        if int(dependency.get("optional", 0)) not in (0, 1):
            raise ZmodError("%s: optional must be 0 or 1" % where)

    return {
        "kind": "service",
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
        "fini": None,
        "path": path,
    }


def build_layout(manifest):
    """Assign module-relative bases and per-object area instance bases."""
    image_areas = manifest["image_areas"]
    bss_areas = manifest["bss_areas"]
    objects = manifest["objects"]

    # Merged area sizes and per-object instance offsets, in object order.
    area_size = {}
    instance_offset = {}
    for name in image_areas + bss_areas:
        area_size[name] = 0
    for obj_index, obj in enumerate(objects):
        for area in obj.areas:
            if area.name not in area_size:
                if area.size != 0 and (area.flags & 0x08) == 0:
                    raise ZmodError("%s: area %s is not listed in the "
                                    "manifest" % (obj.path, area.name))
                continue
            instance_offset[(obj_index, area.name)] = area_size[area.name]
            area_size[area.name] += area.size

    for name in image_areas + bss_areas:
        if area_size[name] == 0:
            raise ZmodError("%s: manifest area %s is empty or missing"
                            % (manifest["path"], name))

    area_base = {}
    offset = 0
    for name in image_areas:
        area_base[name] = offset
        offset += area_size[name]
    image_size = offset
    for name in bss_areas:
        area_base[name] = offset
        offset += area_size[name]

    for (obj_index, name), value in list(instance_offset.items()):
        instance_offset[(obj_index, name)] = area_base[name] + value

    return {
        "area_base": area_base,
        "area_size": area_size,
        "instance_offset": instance_offset,
        "image_size": image_size,
        "total_size": offset,
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
                raise ZmodError("symbol %s defined more than once" % key)
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
    """Segment role of every module-local Def symbol (service reports)."""
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


def resolve_relocation(manifest, layout, symbols, obj_index, mode, ref):
    """Return the module-relative target address for an R group."""
    obj = manifest["objects"][obj_index]
    if (mode & R_SYM) == 0:
        # Area reference: this object's instance of the referenced area.
        if ref >= len(obj.areas):
            raise ZmodError("%s: relocation references area %d out of range"
                            % (obj.path, ref))
        name = obj.areas[ref].name
        key = (obj_index, name)
        if key not in layout["instance_offset"]:
            raise ZmodError("%s: relocation references unlisted area %s"
                            % (obj.path, name))
        return layout["instance_offset"][key]

    # Symbol reference: resolve within the object, then across objects.
    if ref >= len(obj.symbols):
        raise ZmodError("%s: relocation references symbol %d out of range"
                        % (obj.path, ref))
    symbol = obj.symbols[ref]
    if symbol.kind == "def":
        if symbol.area is None:
            raise ZmodError("%s: relocation against absolute symbol %s"
                            % (obj.path, symbol.name))
        name = obj.areas[symbol.area].name
        key = (obj_index, name)
        if key not in layout["instance_offset"]:
            raise ZmodError("%s: symbol %s lives in unlisted area %s"
                            % (obj.path, symbol.name, name))
        return layout["instance_offset"][key] + symbol.value

    entry = symbols.get(symbol.name)
    if entry is None:
        raise ZmodError("%s: unresolved external symbol %s"
                        % (obj.path, symbol.name))
    def_obj, def_area, def_value = entry
    key = (def_obj, def_area)
    if key not in layout["instance_offset"]:
        raise ZmodError("%s: symbol %s lives in unlisted area %s"
                        % (obj.path, symbol.name, def_area))
    return layout["instance_offset"][key] + def_value


def symbol_offset(manifest, layout, symbols, name):
    """Resolve a named symbol to its module-relative address.

    Manifests use C identifiers; sdasz80 records C globals with a leading
    underscore, so both spellings are accepted (exact match first).
    """
    for candidate in (name, "_" + name):
        for obj_index, obj in enumerate(manifest["objects"]):
            for symbol in obj.symbols:
                if symbol.kind == "def" and symbol.name == candidate:
                    if symbol.area is None:
                        raise ZmodError("symbol %s is absolute" % name)
                    area = obj.areas[symbol.area]
                    key = (obj_index, area.name)
                    if key not in layout["instance_offset"]:
                        raise ZmodError("symbol %s lives in unlisted area %s"
                                        % (name, area.name))
                    return layout["instance_offset"][key] + symbol.value
    raise ZmodError("entry symbol %s is not defined by any object" % name)


def build_service_layout(manifest):
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
                    raise ZmodError("%s: area %s is not listed in any "
                                    "segment" % (obj.path, area.name))
                continue
            instance_offset[(obj_index, area.name)] = area_size[area.name]
            area_size[area.name] += area.size

    for name in area_segment:
        if area_size[name] == 0:
            raise ZmodError("%s: manifest area %s is empty or missing"
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
            raise ZmodError("%s: segment %d is empty"
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
                        raise ZmodError("%s: symbol %s is absolute"
                                        % (where, name))
                    area = obj.areas[symbol.area]
                    key = (obj_index, area.name)
                    if key not in layout["instance_offset"]:
                        raise ZmodError("%s: symbol %s lives in unlisted "
                                        "area %s" % (where, name, area.name))
                    return (layout["area_segment"][area.name],
                            layout["instance_offset"][key] + symbol.value)
    raise ZmodError("%s: symbol %s is not defined by any object"
                    % (where, name))


def pack_service_manifest(manifest):
    """Mini-link one service image into its per-segment coordinate spaces."""
    layout = build_service_layout(manifest)
    symbols = build_symbol_table(manifest)
    segments = layout["segments"]
    image = bytearray(layout["image_size"])
    reloc_sites = {}                       # payload offset -> True
    records = [[] for _ in segments]       # per segment, segment-relative
    where = manifest["path"]

    for obj_index, obj in enumerate(manifest["objects"]):
        for chunk in obj.chunks:
            if chunk.area_index >= len(obj.areas):
                raise ZmodError("%s: chunk references area %d out of range"
                                % (obj.path, chunk.area_index))
            area = obj.areas[chunk.area_index]
            key = (obj_index, area.name)
            if key not in layout["instance_offset"]:
                raise ZmodError("%s: chunk in unlisted area %s"
                                % (obj.path, area.name))
            segment = layout["area_segment"][area.name]
            base = layout["instance_offset"][key]
            start = segments[segment]["payload_offset"] + base + chunk.addr
            end = start + len(chunk.data)
            area_limit = (segments[segment]["payload_offset"] +
                          layout["area_base"][area.name] +
                          layout["area_size"][area.name])
            if end > area_limit:
                raise ZmodError("%s: chunk at %04Xh overruns area %s"
                                % (obj.path, chunk.addr, area.name))
            image_areas = list(manifest["segments"][segment].get(
                "image_areas", []))
            if area.name not in image_areas:
                raise ZmodError("%s: BSS area %s contains initialized data"
                                % (obj.path, area.name))

            # Last-writer-wins: later chunks overwrite earlier bytes, but a
            # relocation site may only be replaced as a whole word.
            written = set(range(start, end))
            for site in list(reloc_sites):
                covered = (site in written) + (site + 1 in written)
                if covered == 2:
                    del reloc_sites[site]
                elif covered == 1:
                    raise ZmodError(
                        "%s: chunk partially overwrites relocation site "
                        "at %04Xh" % (obj.path, site))
            image[start:end] = chunk.data

            for mode, rtp, ref in chunk.relocs:
                if mode not in SUPPORTED_MODES:
                    reason = "unsupported relocation mode %02Xh" % mode
                    if mode & R_BYTE:
                        reason += " (byte-fragmented address store; use a " \
                                  "static descriptor initializer)"
                    raise ZmodError("%s: %s" % (obj.path, reason))
                if rtp < A_BYTES:
                    raise ZmodError("%s: relocation targets the chunk "
                                    "address field" % obj.path)
                index = rtp - A_BYTES
                if index + 2 > len(chunk.data):
                    raise ZmodError("%s: relocation outside the T data"
                                    % obj.path)
                patch = base + chunk.addr + index
                if patch + 2 > segments[segment]["image_size"]:
                    raise ZmodError("%s: patch at %04Xh crosses the segment "
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
                    raise ZmodError(
                        "%s: relocation at %04Xh reaches from the hot "
                        "segment into the invisible %s tier"
                        % (obj.path, patch, target_role))
                if patch_role == "bulk":
                    raise ZmodError(
                        "%s: relocation at %04Xh is inside bulk data, "
                        "which is moved by nucleus copies only"
                        % (obj.path, patch))
                addend = (image[start + index] |
                          (image[start + index + 1] << 8))
                value = target + addend
                if value > 0xFFFF:
                    raise ZmodError("%s: relocation at %04Xh overflows 16 "
                                    "bits" % (obj.path, patch))
                image[start + index] = value & 0xFF
                image[start + index + 1] = (value >> 8) & 0xFF
                reloc_sites[start + index] = True
                records[segment].append((target_segment, patch))

    # Segment assets (bulk data, work package 41): the file's bytes are
    # the segment image, copied verbatim, with no relocation records.
    for index, segment in enumerate(layout["segments"]):
        if segment["asset"] is None:
            continue
        start = segment["payload_offset"]
        image[start:start + len(segment["asset"])] = segment["asset"]

    packed = Packed(manifest["name"])
    packed.kind = "service"
    packed.image = image
    packed.bss_size = layout["bss_size"]
    packed.required_granules = 0
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
                raise ZmodError("%s: the %s entry %s is not in that segment"
                                % (where, segment["role"],
                                   segment["entry_name"]))
            if entry_offset >= segment["image_size"]:
                raise ZmodError("%s: the %s entry is outside the image"
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
            raise ZmodError("%s: init entry %s is not in the hot segment"
                            % (where, manifest["init"]))
        if init_offset >= layout["segments"][init_segment]["image_size"]:
            raise ZmodError("%s: init entry %s is outside the image"
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
            raise ZmodError("%s: endpoint %d.%d dispatch %s is not in the "
                            "hot segment" % (where, int(endpoint["class"]),
                                             int(endpoint["instance"]),
                                             endpoint["dispatch"]))
        packed.endpoints.append({
            "class": int(endpoint["class"]),
            "instance": int(endpoint["instance"]),
            "version": int(endpoint["version"]),
            "flags": int(endpoint.get("flags", 0)),
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
        raise ZmodError("%s: image + bss overflows 16 bits" % where)
    return packed


def link_service_image(packed, row):
    """Link one image at the host-planned segment bases (architecture
    36.11): every ZREL_ABS16 record is applied here as
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
                raise ZmodError("%s: relocation at %04Xh overflows 16 bits"
                                % (packed.name, patch))
            image[at] = value & 0xFF
            image[at + 1] = (value >> 8) & 0xFF
    return image


def build_service_container(packed, row):
    """Assemble the host-linked service container: header, service section
    with its three tables, then the concatenated linked segment images.
    `row` carries the host-planned absolute base of each segment role; every
    address field in the container is final, and the header's relocation
    fields are zero (architecture 36.11)."""
    publications = list(getattr(packed, "publications", []))
    if len(publications) > SERVICE_MAX_PUBLICATIONS:
        raise ZmodError("%s: %d publications; the section holds %d"
                        % (packed.name, len(publications),
                           SERVICE_MAX_PUBLICATIONS))
    segment_offset = SERVICE_HEADER_SIZE
    endpoint_offset = segment_offset + SERVICE_SEGMENT_SIZE * \
        len(packed.segments)
    dependency_offset = endpoint_offset + SERVICE_ENDPOINT_SIZE * \
        len(packed.endpoints)
    publication_offset = dependency_offset + SERVICE_DEPENDENCY_SIZE * \
        len(packed.dependencies)
    section_size = publication_offset + SERVICE_PUBLICATION_SIZE * \
        len(publications)
    payload_offset = SERVICE_SECTION_OFFSET + section_size
    image = link_service_image(packed, row)
    image_size = len(image)
    total_size = payload_offset + image_size
    if total_size > 0xFFFF:
        raise ZmodError("container exceeds the 16-bit size range")

    hot_base = row["hot"]
    init = (NO_ENTRY if packed.init_offset == NO_ENTRY
            else hot_base + packed.init_offset)

    header = bytearray(HEADER_SIZE)
    header[0:4] = MAGIC
    header[4] = FORMAT_VERSION
    header[5] = TYPE_SERVICE
    header[6] = (SERVICE_ABI_PUBLICATIONS if publications
                 else SERVICE_ABI_VERSION)
    header[7] = FLAG_NONE
    put16(header, 0x08, image_size)
    put16(header, 0x0A, packed.bss_size)
    put16(header, 0x0C, 0)  # per-tier allocation is the host packer's
    put16(header, 0x0E, init)
    put16(header, 0x10, NO_ENTRY)
    put16(header, 0x12, 0)  # linked on the host: no relocation records
    put16(header, 0x14, 0)
    put16(header, 0x16, payload_offset)
    put16(header, 0x18, total_size)
    put16(header, 0x1A, 0)

    section = bytearray(SERVICE_HEADER_SIZE)
    put16(section, 0x00, section_size)
    put16(section, 0x02, segment_offset)
    put16(section, 0x04, endpoint_offset)
    put16(section, 0x06, dependency_offset)
    section[0x08] = len(packed.segments)
    section[0x09] = len(packed.endpoints)
    section[0x0A] = len(packed.dependencies)
    if publications:
        # ABI 4: the section header's reserved bytes carry the table.
        section[0x0B] = len(publications)
        put16(section, 0x0C, publication_offset)

    for segment in packed.segments:
        base = row[segment["role"]]
        entry = (NO_ENTRY if segment["entry_offset"] == NO_ENTRY
                 else base + segment["entry_offset"])
        record = bytearray(SERVICE_SEGMENT_SIZE)
        record[0x00] = SERVICE_ROLE_CODES[segment["role"]]
        record[0x01] = 0
        put16(record, 0x02, base)
        put16(record, 0x04, segment["image_size"])
        put16(record, 0x06, segment["bss_size"])
        put16(record, 0x08, entry)
        put16(record, 0x0A, segment["payload_offset"])
        put16(record, 0x0C, 0)
        section += record
    for endpoint in packed.endpoints:
        record = bytearray(SERVICE_ENDPOINT_SIZE)
        record[0x00] = endpoint["class"]
        record[0x01] = endpoint["instance"]
        record[0x02] = endpoint["version"]
        record[0x03] = endpoint["flags"]
        put16(record, 0x04, endpoint["number"])
        put16(record, 0x06, hot_base + endpoint["dispatch_offset"])
        section += record
    for dependency in packed.dependencies:
        record = bytearray(SERVICE_DEPENDENCY_SIZE)
        record[0x00] = dependency["class"]
        record[0x01] = dependency["instance"]
        put16(record, 0x02, dependency["number"])
        section += record
    for publication in publications:
        record = bytearray(SERVICE_PUBLICATION_SIZE)
        raw = publication["name"].encode("ascii")
        record[0:len(raw)] = raw
        record[0x10] = publication["type"]
        record[0x11] = publication["number"]
        record[0x12] = publication["object"]
        record[0x13] = 0
        section += record
    if len(section) != section_size:
        raise ZmodError("internal: service section size mismatch")
    return bytes(header + section + bytes(image))


def resolve_relocation_area(manifest, layout, symbols, obj_index, mode, ref):
    """(offset, area name) for an R group, so cross-segment references can
    be rejected: segments relocate against their own base only."""
    obj = manifest["objects"][obj_index]
    if (mode & R_SYM) == 0:
        if ref >= len(obj.areas):
            raise ZmodError("%s: relocation references area %d out of range"
                            % (obj.path, ref))
        name = obj.areas[ref].name
        key = (obj_index, name)
        if key not in layout["instance_offset"]:
            raise ZmodError("%s: relocation references unlisted area %s"
                            % (obj.path, name))
        return layout["instance_offset"][key], name

    if ref >= len(obj.symbols):
        raise ZmodError("%s: relocation references symbol %d out of range"
                        % (obj.path, ref))
    symbol = obj.symbols[ref]
    if symbol.kind == "def":
        if symbol.area is None:
            raise ZmodError("%s: relocation against absolute symbol %s"
                            % (obj.path, symbol.name))
        name = obj.areas[symbol.area].name
        key = (obj_index, name)
        if key not in layout["instance_offset"]:
            raise ZmodError("%s: symbol %s lives in unlisted area %s"
                            % (obj.path, symbol.name, name))
        return layout["instance_offset"][key] + symbol.value, name

    entry = symbols.get(symbol.name)
    if entry is None:
        raise ZmodError("%s: unresolved external symbol %s"
                        % (obj.path, symbol.name))
    def_obj, def_area, def_value = entry
    key = (def_obj, def_area)
    if key not in layout["instance_offset"]:
        raise ZmodError("%s: symbol %s lives in unlisted area %s"
                        % (obj.path, symbol.name, def_area))
    return layout["instance_offset"][key] + def_value, def_area


def pack_manifest(manifest):
    if manifest["kind"] == "service":
        return pack_service_manifest(manifest)
    return pack_driver_manifest(manifest)


def pack_driver_manifest(manifest):
    layout = build_layout(manifest)
    symbols = build_symbol_table(manifest)
    image_size = layout["image_size"]
    image = bytearray(image_size)
    reloc_sites = {}   # patch offset -> True

    for obj_index, obj in enumerate(manifest["objects"]):
        for chunk in obj.chunks:
            if chunk.area_index >= len(obj.areas):
                raise ZmodError("%s: chunk references area %d out of range"
                                % (obj.path, chunk.area_index))
            area = obj.areas[chunk.area_index]
            key = (obj_index, area.name)
            if key not in layout["instance_offset"]:
                raise ZmodError("%s: chunk in unlisted area %s"
                                % (obj.path, area.name))
            base = layout["instance_offset"][key]
            start = base + chunk.addr
            end = start + len(chunk.data)
            area_limit = layout["area_base"][area.name] + \
                layout["area_size"][area.name]
            if end > area_limit:
                raise ZmodError("%s: chunk at %04Xh overruns area %s"
                                % (obj.path, chunk.addr, area.name))
            if area.name not in manifest["image_areas"]:
                raise ZmodError("%s: BSS area %s contains initialized data"
                                % (obj.path, area.name))

            # Last-writer-wins: later chunks overwrite earlier bytes, but a
            # relocation site may only be replaced as a whole word.
            written = set(range(start, end))
            for site in list(reloc_sites):
                covered = (site in written) + (site + 1 in written)
                if covered == 2:
                    del reloc_sites[site]
                elif covered == 1:
                    raise ZmodError(
                        "%s: chunk partially overwrites relocation site "
                        "at %04Xh" % (obj.path, site))
            image[start:end] = chunk.data

            for mode, rtp, ref in chunk.relocs:
                if mode not in SUPPORTED_MODES:
                    reason = "unsupported relocation mode %02Xh" % mode
                    if mode & R_BYTE:
                        reason += " (byte-fragmented address store; use a " \
                                  "static descriptor initializer)"
                    raise ZmodError("%s: %s" % (obj.path, reason))
                if rtp < A_BYTES:
                    raise ZmodError("%s: relocation targets the chunk "
                                    "address field" % obj.path)
                index = rtp - A_BYTES
                if index + 2 > len(chunk.data):
                    raise ZmodError("%s: relocation outside the T data"
                                    % obj.path)
                patch = start + index
                if patch + 2 > image_size:
                    raise ZmodError("%s: patch at %04Xh crosses the image "
                                    "end" % (obj.path, patch))
                target = resolve_relocation(manifest, layout, symbols,
                                            obj_index, mode, ref)
                addend = image[patch] | (image[patch + 1] << 8)
                value = target + addend
                if value > 0xFFFF:
                    raise ZmodError("%s: relocation at %04Xh overflows 16 "
                                    "bits" % (obj.path, patch))
                image[patch] = value & 0xFF
                image[patch + 1] = (value >> 8) & 0xFF
                reloc_sites[patch] = True

    init_offset = symbol_offset(manifest, layout, symbols, manifest["init"])
    if init_offset >= image_size:
        raise ZmodError("init entry %s is outside the image"
                        % manifest["init"])
    fini_offset = NO_ENTRY
    if manifest["fini"]:
        fini_offset = symbol_offset(manifest, layout, symbols,
                                    manifest["fini"])
        if fini_offset >= image_size:
            raise ZmodError("fini entry %s is outside the image"
                            % manifest["fini"])

    bss_size = layout["total_size"] - image_size
    total = image_size + bss_size
    required = (total + GRANULE_SIZE - 1) // GRANULE_SIZE
    if required == 0 or required > MAX_GRANULES:
        raise ZmodError("module needs %d granules; the architectural maximum "
                        "is %d" % (required, MAX_GRANULES))

    packed = Packed(manifest["name"])
    packed.image = image
    packed.bss_size = bss_size
    packed.relocs = sorted(reloc_sites)
    packed.init_offset = init_offset
    packed.fini_offset = fini_offset
    packed.required_granules = required
    packed.symbols = collect_symbol_offsets(manifest, layout)
    return packed


def build_container(packed):
    if packed.kind == "service":
        return build_service_container(packed)
    return build_driver_container(packed)


def build_driver_container(packed):
    reloc_offset = HEADER_SIZE
    payload_offset = reloc_offset + RELOC_SIZE * len(packed.relocs)
    image_size = len(packed.image)
    total_size = payload_offset + image_size
    if total_size > 0xFFFF:
        raise ZmodError("container exceeds the 16-bit size range")

    header = bytearray(HEADER_SIZE)
    header[0:4] = MAGIC
    header[4] = FORMAT_VERSION
    header[5] = TYPE_DRIVER
    header[6] = DRIVER_ABI_VERSION
    header[7] = FLAG_NONE
    put16(header, 0x08, image_size)
    put16(header, 0x0A, packed.bss_size)
    put16(header, 0x0C, packed.required_granules)
    put16(header, 0x0E, packed.init_offset)
    put16(header, 0x10, packed.fini_offset)
    put16(header, 0x12, reloc_offset)
    put16(header, 0x14, len(packed.relocs))
    put16(header, 0x16, payload_offset)
    put16(header, 0x18, total_size)
    put16(header, 0x1A, 0)  # reserved

    body = bytearray()
    for patch in packed.relocs:
        body += bytes((ZREL_ABS16, 0, patch & 0xFF, (patch >> 8) & 0xFF))
    return bytes(header + body + bytes(packed.image))


def put16(buf, offset, value):
    buf[offset] = value & 0xFF
    buf[offset + 1] = (value >> 8) & 0xFF


def get16(buf, offset):
    return buf[offset] | (buf[offset + 1] << 8)


# --------------------------------------------------------------------------
# Container validation/dump (mirrors the target loader rules)
# --------------------------------------------------------------------------

def parse_container(data, source="<container>"):
    """Validate one container and return its summary, whichever module
    type its header declares."""
    if len(data) < HEADER_SIZE:
        raise ZmodError("%s: shorter than the %d-byte header"
                        % (source, HEADER_SIZE))
    if data[0:4] != MAGIC:
        raise ZmodError("%s: bad magic" % source)
    if data[5] == TYPE_SERVICE:
        return parse_service_container(data, source)
    return parse_driver_container(data, source)


def parse_service_container(data, source="<container>"):
    if len(data) < HEADER_SIZE + SERVICE_HEADER_SIZE:
        raise ZmodError("%s: shorter than the header and service section"
                        % source)
    version = data[4]
    abi = data[6]
    flags = data[7]
    image_size = get16(data, 0x08)
    bss_size = get16(data, 0x0A)
    required = get16(data, 0x0C)
    init = get16(data, 0x0E)
    fini_offset = get16(data, 0x10)
    reloc_offset = get16(data, 0x12)
    reloc_count = get16(data, 0x14)
    payload_offset = get16(data, 0x16)
    total_size = get16(data, 0x18)
    reserved = get16(data, 0x1A)

    if version != FORMAT_VERSION:
        raise ZmodError("%s: unsupported format version %d" % (source, version))
    if abi not in (SERVICE_ABI_VERSION, SERVICE_ABI_PUBLICATIONS):
        raise ZmodError("%s: unsupported service ABI %d" % (source, abi))
    if flags != FLAG_NONE or reserved != 0:
        raise ZmodError("%s: bad flags/reserved fields" % source)
    if required != 0:
        raise ZmodError("%s: the service granule field must be zero"
                        % source)
    if fini_offset != NO_ENTRY:
        raise ZmodError("%s: services are permanent; fini must be FFFFh"
                        % source)
    if reloc_offset != 0 or reloc_count != 0:
        raise ZmodError("%s: a host-linked image carries no relocation "
                        "records" % source)
    if total_size != len(data):
        raise ZmodError("%s: total_size %d != container length %d"
                        % (source, total_size, len(data)))
    if payload_offset < HEADER_SIZE + SERVICE_HEADER_SIZE:
        raise ZmodError("%s: payload_offset inside the header" % source)
    if payload_offset + image_size != total_size:
        raise ZmodError("%s: payload does not end at total_size" % source)

    section_at = SERVICE_SECTION_OFFSET
    section_size = get16(data, section_at + 0x00)
    segment_offset = get16(data, section_at + 0x02)
    endpoint_offset = get16(data, section_at + 0x04)
    dependency_offset = get16(data, section_at + 0x06)
    segment_count = data[section_at + 0x08]
    endpoint_count = data[section_at + 0x09]
    dependency_count = data[section_at + 0x0A]
    # ABI 4 (package 42a) reuses the reserved bytes for the publication
    # table; ABI 3 keeps them zero and carries no table.
    if abi == SERVICE_ABI_PUBLICATIONS:
        publication_count = data[section_at + 0x0B]
        publication_offset = get16(data, section_at + 0x0C)
    else:
        publication_count = 0
        publication_offset = 0
    if get16(data, section_at + 0x0E) != 0:
        raise ZmodError("%s: service section reserved bytes are not zero"
                        % source)
    if segment_count == 0 or segment_count > SERVICE_MAX_SEGMENTS:
        raise ZmodError("%s: bad segment count %d" % (source, segment_count))
    if endpoint_count > SERVICE_MAX_ENDPOINTS:
        raise ZmodError("%s: bad endpoint count %d" % (source, endpoint_count))
    if dependency_count > SERVICE_MAX_DEPENDENCIES:
        raise ZmodError("%s: bad dependency count %d"
                        % (source, dependency_count))

    section_end = section_at + section_size
    if section_size < SERVICE_HEADER_SIZE or section_end > payload_offset:
        raise ZmodError("%s: service section overlaps the payload or is "
                        "truncated" % source)
    expected_segment = SERVICE_HEADER_SIZE
    expected_endpoint = expected_segment + SERVICE_SEGMENT_SIZE * \
        segment_count
    expected_dependency = expected_endpoint + SERVICE_ENDPOINT_SIZE * \
        endpoint_count
    expected_publication = expected_dependency + SERVICE_DEPENDENCY_SIZE * \
        dependency_count
    if publication_count > SERVICE_MAX_PUBLICATIONS:
        raise ZmodError("%s: bad publication count %d"
                        % (source, publication_count))
    if abi == SERVICE_ABI_PUBLICATIONS and publication_count == 0:
        raise ZmodError("%s: ABI 4 without a publication table" % source)
    if abi == SERVICE_ABI_VERSION and publication_count != 0:
        raise ZmodError("%s: ABI 3 cannot carry publications" % source)
    expected_end = expected_publication + SERVICE_PUBLICATION_SIZE * \
        publication_count
    if (segment_offset != expected_segment or
            endpoint_offset != expected_endpoint or
            dependency_offset != expected_dependency or
            publication_offset != (expected_publication
                                   if publication_count else 0) or
            section_size < expected_end):
        raise ZmodError("%s: service section tables are not the canonical "
                        "back-to-back layout" % source)

    segments = []
    roles = set()
    for index in range(segment_count):
        at = section_at + segment_offset + SERVICE_SEGMENT_SIZE * index
        role_code = data[at]
        if role_code not in SERVICE_ROLE_NAMES:
            raise ZmodError("%s: segment %d has an unknown role %d"
                            % (source, index, role_code))
        role = SERVICE_ROLE_NAMES[role_code]
        if role in roles:
            raise ZmodError("%s: duplicate segment role %s" % (source, role))
        roles.add(role)
        if data[at + 1] != 0:
            raise ZmodError("%s: segment %d flags are not zero"
                            % (source, index))
        base = get16(data, at + 0x02)
        segment_image = get16(data, at + 0x04)
        segment_bss = get16(data, at + 0x06)
        entry = get16(data, at + 0x08)
        segment_payload = get16(data, at + 0x0A)
        if get16(data, at + 0x0C) != 0:
            raise ZmodError("%s: segment %s reserved field is not zero"
                            % (source, role))
        if segment_image + segment_bss == 0:
            raise ZmodError("%s: segment %s is empty" % (source, role))
        if segment_payload + segment_image > image_size:
            raise ZmodError("%s: segment %s overruns the payload"
                            % (source, role))
        tier_base, tier_end = SERVICE_TIERS[role]
        if base < tier_base or base + segment_image + segment_bss > tier_end:
            raise ZmodError(
                "%s: segment %s at %04Xh (+%d) leaves its tier "
                "%04Xh-%04Xh" % (source, role, base,
                                 segment_image + segment_bss,
                                 tier_base, tier_end - 1))
        if entry != NO_ENTRY:
            if role != "boot":
                raise ZmodError("%s: only the boot role carries an entry"
                                % source)
            if entry < base or entry >= base + segment_image:
                raise ZmodError("%s: the boot entry %04Xh is outside the "
                                "boot image" % (source, entry))
        segments.append({
            "role": role,
            "base": base,
            "image_size": segment_image,
            "bss_size": segment_bss,
            "entry": entry,
            "payload_offset": segment_payload,
        })
    if "hot" not in roles:
        raise ZmodError("%s: a service image needs a hot segment" % source)
    hot = [segment for segment in segments
           if segment["role"] == "hot"][0]
    if init != NO_ENTRY and (init < hot["base"] or
                             init >= hot["base"] + hot["image_size"]):
        raise ZmodError("%s: init %04Xh outside the hot image"
                        % (source, init))

    endpoints = []
    for index in range(endpoint_count):
        at = section_at + endpoint_offset + SERVICE_ENDPOINT_SIZE * index
        endpoint = {
            "class": data[at],
            "instance": data[at + 1],
            "version": data[at + 2],
            "flags": data[at + 3],
            "number": get16(data, at + 0x04),
            "dispatch": get16(data, at + 0x06),
        }
        if endpoint["flags"] & ~SERVICE_EP_REENTRANT:
            raise ZmodError("%s: endpoint %d has unknown flags %02Xh"
                            % (source, index, endpoint["flags"]))
        if endpoint["number"] == 0:
            raise ZmodError("%s: endpoint %d has no host-assigned number"
                            % (source, index))
        if (endpoint["dispatch"] < hot["base"] or endpoint["dispatch"] >=
                hot["base"] + hot["image_size"]):
            raise ZmodError("%s: endpoint %d dispatch %04Xh outside the hot "
                            "image" % (source, index, endpoint["dispatch"]))
        endpoints.append(endpoint)

    dependencies = []
    for index in range(dependency_count):
        at = section_at + dependency_offset + SERVICE_DEPENDENCY_SIZE * index
        dependencies.append({
            "class": data[at],
            "instance": data[at + 1],
            "number": get16(data, at + 0x02),
        })

    publications = []
    for index in range(publication_count):
        at = (section_at + publication_offset +
              SERVICE_PUBLICATION_SIZE * index)
        raw = data[at:at + SERVICE_PUBLICATION_NAME]
        end = raw.find(b"\0")
        if end <= 0 or raw[0:1] != b"/":
            raise ZmodError("%s: publication %d has no name" % (source, index))
        if data[at + 0x10] == 0 or data[at + 0x11] == 0:
            raise ZmodError("%s: publication %d has no type or endpoint"
                            % (source, index))
        if data[at + 0x13] != 0:
            raise ZmodError("%s: publication %d flags are not zero"
                            % (source, index))
        publications.append({
            "name": raw[:end].decode("ascii"),
            "type": data[at + 0x10],
            "number": data[at + 0x11],
            "object": data[at + 0x12],
        })

    return {
        "source": source,
        "module_type": "service",
        "service_abi": abi,
        "image_size": image_size,
        "bss_size": bss_size,
        "required_granules": 0,
        "init_offset": init,
        "fini_offset": fini_offset,
        "reloc_offset": reloc_offset,
        "reloc_count": reloc_count,
        "payload_offset": payload_offset,
        "total_size": total_size,
        "relocs": [],
        "segments": segments,
        "endpoints": endpoints,
        "dependencies": dependencies,
        "publications": publications,
        "payload": bytes(data[payload_offset:payload_offset + image_size]),
    }


def parse_driver_container(data, source="<container>"):
    if len(data) < HEADER_SIZE:
        raise ZmodError("%s: shorter than the %d-byte header"
                        % (source, HEADER_SIZE))
    if data[0:4] != MAGIC:
        raise ZmodError("%s: bad magic" % source)
    version = data[4]
    module_type = data[5]
    abi = data[6]
    flags = data[7]
    image_size = get16(data, 0x08)
    bss_size = get16(data, 0x0A)
    required = get16(data, 0x0C)
    init_offset = get16(data, 0x0E)
    fini_offset = get16(data, 0x10)
    reloc_offset = get16(data, 0x12)
    reloc_count = get16(data, 0x14)
    payload_offset = get16(data, 0x16)
    total_size = get16(data, 0x18)
    reserved = get16(data, 0x1A)

    if version != FORMAT_VERSION:
        raise ZmodError("%s: unsupported format version %d" % (source, version))
    if module_type != TYPE_DRIVER:
        raise ZmodError("%s: unsupported module type %d" % (source, module_type))
    if abi != DRIVER_ABI_VERSION:
        raise ZmodError("%s: unsupported driver ABI %d" % (source, abi))
    if flags != FLAG_NONE or reserved != 0:
        raise ZmodError("%s: bad flags/reserved fields" % source)
    if image_size == 0:
        raise ZmodError("%s: image_size is zero" % source)
    if total_size != len(data):
        raise ZmodError("%s: total_size %d != container length %d"
                        % (source, total_size, len(data)))
    if payload_offset < HEADER_SIZE:
        raise ZmodError("%s: payload_offset inside the header" % source)
    if payload_offset + image_size != total_size:
        raise ZmodError("%s: payload does not end at total_size" % source)
    if reloc_offset < HEADER_SIZE:
        raise ZmodError("%s: reloc_offset inside the header" % source)
    reloc_end = reloc_offset + RELOC_SIZE * reloc_count
    if reloc_end > payload_offset:
        raise ZmodError("%s: relocation table overlaps the payload" % source)
    if init_offset >= image_size:
        raise ZmodError("%s: init_offset outside the image" % source)
    if fini_offset != NO_ENTRY and fini_offset >= image_size:
        raise ZmodError("%s: fini_offset outside the image" % source)
    total = image_size + bss_size
    if total > 0xFFFF:
        raise ZmodError("%s: image + bss overflows 16 bits" % source)
    required_check = (total + GRANULE_SIZE - 1) // GRANULE_SIZE
    if required_check == 0 or required_check != required:
        raise ZmodError("%s: required_granules %d != computed %d"
                        % (source, required, required_check))
    if required_check > MAX_GRANULES:
        raise ZmodError("%s: module needs %d granules; the architectural "
                        "maximum is %d"
                        % (source, required_check, MAX_GRANULES))

    relocs = []
    for i in range(reloc_count):
        rec = reloc_offset + RELOC_SIZE * i
        rtype = data[rec]
        reserved = data[rec + 1]
        patch = get16(data, rec + 2)
        if rtype != ZREL_ABS16:
            raise ZmodError("%s: unsupported relocation type %d" % (source, rtype))
        if reserved != 0:
            raise ZmodError("%s: relocation %d has a bad reserved byte"
                            % (source, i))
        if patch + 2 > image_size:
            raise ZmodError("%s: relocation %d patch %04Xh crosses the image"
                            % (source, i, patch))
        # (type, patch): a driver container relocates its single image, so
        # the record's second byte stays reserved (service containers carry
        # a target segment there; see parse_service_container).
        relocs.append((rtype, patch))

    return {
        "source": source,
        "image_size": image_size,
        "bss_size": bss_size,
        "required_granules": required,
        "init_offset": init_offset,
        "fini_offset": fini_offset,
        "reloc_offset": reloc_offset,
        "reloc_count": reloc_count,
        "payload_offset": payload_offset,
        "total_size": total_size,
        "relocs": relocs,
        "payload": bytes(data[payload_offset:payload_offset + image_size]),
    }


def format_report(info, name=None):
    lines = []
    label = name if name else info["source"]
    lines.append("%s" % label)
    if info.get("module_type") == "service":
        lines.append("  format:        ZMOD v%d driver image, ABI %d"
                     % (FORMAT_VERSION, info.get("service_abi",
                                                 SERVICE_ABI_VERSION)))
        lines.append("  image:         %d bytes (host-linked, no "
                     "relocations)" % info["image_size"])
    else:
        lines.append("  format:        ZMOD v%d driver, ABI %d"
                     % (FORMAT_VERSION, DRIVER_ABI_VERSION))
        lines.append("  image:         %d bytes" % info["image_size"])
    lines.append("  bss:           %d bytes" % info["bss_size"])
    if info.get("module_type") != "service":
        lines.append("  relocations:   %d (ZREL_ABS16)" % info["reloc_count"])
    if info["init_offset"] == NO_ENTRY:
        lines.append("  init:          none")
    else:
        lines.append("  init:          %04Xh" % info["init_offset"])
    if info["fini_offset"] == NO_ENTRY:
        lines.append("  fini:          none")
    else:
        lines.append("  fini:          +0x%04X" % info["fini_offset"])
    lines.append("  total:         %d bytes" % info["total_size"])
    if info.get("module_type") == "service":
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
    else:
        lines.append("  allocation:    %d x %d-byte granules"
                     % (info["required_granules"], GRANULE_SIZE))
    return "\n".join(lines)


def report_json(info, name, packed=None):
    report = {
        "name": name,
        "format_version": FORMAT_VERSION,
        "module_type": info.get("module_type", "driver"),
        "image_size": info["image_size"],
        "bss_size": info["bss_size"],
        "init_offset": info["init_offset"],
        "fini_offset": info["fini_offset"],
        "reloc_count": info["reloc_count"],
        "total_size": info["total_size"],
        "image_sha256": hashlib.sha256(info["payload"]).hexdigest(),
    }
    if info.get("module_type") == "service":
        report["service_abi"] = info.get("service_abi",
                                         SERVICE_ABI_VERSION)
        report["segments"] = info["segments"]
        report["endpoints"] = info["endpoints"]
        report["dependencies"] = info["dependencies"]
        report["publications"] = info.get("publications", [])
    else:
        report["relocations"] = [patch for _, patch in info["relocs"]]
        report["driver_abi"] = DRIVER_ABI_VERSION
        report["required_granules"] = info["required_granules"]
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


def pack_service_set_member(manifest_path, manifest, set_paths, objdir):
    """Link one service image against its whole set (architecture 36.11).

    The set is the load order: every manifest listed by `--set`, this one
    included.  The packer plans the bank-6 tiers, assigns endpoint numbers,
    resolves the symbolic dependencies and links this image's segments at
    their final addresses; the other images in the set are linked by their
    own invocations, with the same plan."""
    if not set_paths:
        raise ZmodError("%s: a service image is linked against its whole "
                        "set: pass --set with every manifest in load order"
                        % manifest_path)
    self_abs = os.path.abspath(manifest_path)
    packs = []
    index_of_self = None
    for spec in set_paths:
        path, member_objdir = set_member_location(spec, objdir)
        loaded = manifest if os.path.abspath(path) == self_abs \
            else load_manifest(path, member_objdir)
        if loaded["kind"] != "service":
            raise ZmodError("%s: not a service manifest" % path)
        if os.path.abspath(path) == self_abs:
            if index_of_self is not None:
                raise ZmodError("%s: listed twice in --set" % manifest_path)
            index_of_self = len(packs)
        packs.append(pack_service_manifest(loaded))
    if index_of_self is None:
        raise ZmodError("%s: the manifest is not in its own --set"
                        % manifest_path)
    rows, _ledger = plan_service_set(packs)
    packed = packs[index_of_self]
    return build_service_container(packed, rows[index_of_self]), packed


def cmd_pack(args):
    if len(args) < 2:
        raise ZmodError("usage: zmodpack.py pack <manifest> <out.zmod> "
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
                raise ZmodError("pack: --set needs at least one manifest")
        else:
            raise ZmodError("unknown pack option %r" % flag)

    manifest = load_manifest(manifest_path, objdir)
    if manifest["kind"] == "service":
        container, packed = pack_service_set_member(
            manifest_path, manifest, set_paths, objdir)
    else:
        if set_paths:
            raise ZmodError("pack: --set is a service-image option")
        packed = pack_manifest(manifest)
        container = build_container(packed)
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
        raise ZmodError("usage: zmodpack.py validate <container.zmod> [...]")
    for path in args:
        data = open(path, "rb").read()
        info = parse_container(data, path)
        if info.get("module_type") == "service":
            print("zmodpack: %s: OK (%d image bytes, %d bss, host-linked)"
                  % (path, info["image_size"], info["bss_size"]))
        else:
            print("zmodpack: %s: OK (%d image bytes, %d bss, %d relocations)"
                  % (path, info["image_size"], info["bss_size"],
                     info["reloc_count"]))
    return 0


def cmd_dump(args):
    if not args:
        raise ZmodError("usage: zmodpack.py dump <container.zmod> [...]")
    for path in args:
        data = open(path, "rb").read()
        info = parse_container(data, path)
        print(format_report(info, path))
        if info.get("module_type") != "service":
            for index, (_, patch) in enumerate(info["relocs"]):
                print("  reloc %3d:    +0x%04X  ZREL_ABS16" % (index, patch))
    return 0


def cmd_emit_asm(args):
    if len(args) < 2:
        raise ZmodError("usage: zmodpack.py emit-asm <out.asm> "
                        "SYMBOL=container.zmod [...] [--header FILE]")
    out_path = args[0]
    specs = []
    header_path = None
    rest = list(args[1:])
    while rest:
        item = rest.pop(0)
        if item == "--header" and rest:
            header_path = rest.pop(0)
        else:
            specs.append(item)
    if not specs:
        raise ZmodError("emit-asm: no SYMBOL=container.zmod arguments")

    lines = [
        "; Generated by tools/zmodpack.py -- do not edit.",
        "; ZMOD container blobs emitted as assembly (test artifact).",
        "\t.module zmod_blobs",
    ]
    names = []
    for spec in specs:
        if "=" not in spec:
            raise ZmodError("emit-asm: expected SYMBOL=container.zmod, got %r"
                            % spec)
        symbol, path = spec.split("=", 1)
        data = open(path, "rb").read()
        parse_container(data, path)
        names.append((symbol, len(data)))
        lines.append("\t.globl _%s, _%s_size" % (symbol, symbol))
    lines.append("\t.area _CODE")
    for spec in specs:
        symbol, path = spec.split("=", 1)
        data = open(path, "rb").read()
        lines.append("")
        lines.append("_%s::" % symbol)
        for offset in range(0, len(data), 16):
            row = data[offset:offset + 16]
            lines.append("\t.db " + ",".join("0x%02X" % byte for byte in row))
        lines.append("_%s_size::" % symbol)
        lines.append("\t.dw %d" % len(data))
    with open(out_path, "w") as handle:
        handle.write("\n".join(lines) + "\n")

    if header_path:
        header = [
            "/* Generated by tools/zmodpack.py -- do not edit. */",
            "#ifndef ZEPHYROS_GENERATED_M4_FIXTURES_INC",
            "#define ZEPHYROS_GENERATED_M4_FIXTURES_INC",
        ]
        for symbol, size in names:
            header.append("#define %s_SIZE %d" % (symbol.upper(), size))
        header.append("#endif")
        with open(header_path, "w") as handle:
            handle.write("\n".join(header) + "\n")

    print("zmodpack: %s: %d fixture blob(s) emitted" % (out_path, len(names)))
    return 0


def parse_geometry_inc(path):
    """Parse NAME = value declarations from a memory/geometry .inc."""
    values = {}
    for line in open(path):
        line = line.split(";")[0].strip()
        match = re.match(r"([A-Za-z_]\w*)\s+=\s+(0[xX][0-9a-fA-F]+|\d+)$",
                         line)
        if match:
            values[match.group(1)] = int(match.group(2), 0)
    return values


def build_nucleus_ledger(values, used_granules, source):
    """The five budget ledgers from the generated geometry memory include.

    Every derived value is required: a missing key raises a clear error
    naming the key and the include it was expected in, never a silent skip.
    The field set matches tools/pack_image.py's nucleus_ledger() so the
    arena.plan.json ledger and the image manifest ledger are identical."""
    required_keys = (
        "ZOS_KERNEL_CODE_END", "ZOS_FIXED_CODE_END",
        "ZOS_FIXED_REQUIRED_BYTES", "ZOS_FIXED_OVERFLOW_BYTES",
        "ZOS_FIXED_STACKS_BYTES", "ZOS_FIXED_ROOM_BYTES",
        "ZOS_DRIVER_ARENA_BASE", "ZOS_DRIVER_ARENA_END",
        "ZOS_DRIVER_GRANULES", "ZOS_DATA_BASE", "ZOS_DATA_SIZE",
        "ZOS_REV13_WINDOW_BASE", "ZOS_WINDOW_DATA_BASE",
        "ZOS_WINDOW_DATA_SIZE", "ZOS_BANK7_RUNTIME_END", "ZOS_COMMON_BASE",
        "ZOS_COMMON_CODE_END", "ZOS_COMMON_CODE_SIZE",
        "ZOS_COMMON_DATA_SIZE", "ZOS_COMMON_CEILING",
        "ZOS_COMMON_M10_RESERVATION",
    )
    for name in required_keys:
        if name not in values:
            raise ZmodError(
                "%s is missing from %s; the derived build must generate it"
                % (name, source))
    common_code = values["ZOS_COMMON_CODE_SIZE"]
    common_data = values["ZOS_COMMON_DATA_SIZE"]
    common_capacity = values["ZOS_COMMON_CODE_END"] - values["ZOS_COMMON_BASE"]
    common_ceiling = values["ZOS_COMMON_CEILING"]
    common_reservation = values["ZOS_COMMON_M10_RESERVATION"]
    common_limit = common_ceiling - common_reservation
    if common_code + common_data > common_capacity:
        raise ZmodError(
            "packed common memory is %d bytes, over the %d-byte window"
            % (common_code + common_data, common_capacity))
    if common_code + common_data > common_limit:
        raise ZmodError(
            "packed common memory is %d bytes, over the %d-byte M9 limit "
            "(ceiling %d minus the %d-byte M10 reservation)"
            % (common_code + common_data, common_limit, common_ceiling,
               common_reservation))
    window_base = values["ZOS_REV13_WINDOW_BASE"]
    window_data_base = values["ZOS_WINDOW_DATA_BASE"]
    cold_code = values["ZOS_KERNEL_CODE_END"] - window_base
    cold_data = values["ZOS_WINDOW_DATA_SIZE"]
    cold_capacity = values["ZOS_BANK7_RUNTIME_END"] - window_base
    arena_bytes = (values["ZOS_DRIVER_ARENA_END"]
                   - values["ZOS_DRIVER_ARENA_BASE"])
    return {
        "fixed_code_end": values["ZOS_FIXED_CODE_END"],
        "fixed_required_bytes": values["ZOS_FIXED_REQUIRED_BYTES"],
        "fixed_overflow_bytes": values["ZOS_FIXED_OVERFLOW_BYTES"],
        "fixed_stacks_bytes": values["ZOS_FIXED_STACKS_BYTES"],
        "fixed_room_bytes": values["ZOS_FIXED_ROOM_BYTES"],
        "fixed_data_base": values["ZOS_DATA_BASE"],
        "fixed_data_bytes": values["ZOS_DATA_SIZE"],
        "cold_code_bytes": cold_code,
        "cold_code_end": values["ZOS_KERNEL_CODE_END"],
        "cold_arena_base": values["ZOS_DRIVER_ARENA_BASE"],
        "cold_arena_end": values["ZOS_DRIVER_ARENA_END"],
        "cold_arena_granules": values["ZOS_DRIVER_GRANULES"],
        "cold_arena_used_granules": used_granules,
        "cold_arena_free_granules": (values["ZOS_DRIVER_GRANULES"]
                                     - used_granules),
        "cold_data_bytes": cold_data,
        "cold_data_base": window_data_base,
        "cold_room_bytes": cold_capacity - cold_code - cold_data
                           - arena_bytes,
        "service_hot_bytes": 0,
        "service_hot_capacity_bytes": pack_image.SERVICE_HOT_CAPACITY,
        "service_boot_bytes": 0,
        "service_boot_capacity_bytes": pack_image.SERVICE_BOOT_CAPACITY,
        "common_code_bytes": common_code,
        "common_data_bytes": common_data,
        "common_used_bytes": common_code + common_data,
        "common_capacity_bytes": common_capacity,
        "common_ceiling_bytes": common_ceiling,
        "common_m10_reservation_bytes": common_reservation,
        "common_limit_bytes": common_limit,
    }


def format_nucleus_ledger(ledger):
    """The five budget lines (architecture 36.20): the nucleus fixed and
    cold halves, the two bank-6 service tiers and the common window.
    Byte-identical to tools/pack_image.py (pinned by a host test)."""
    return "\n".join((
        pack_image.NUCLEUS_FIXED_LEDGER_LINE % (
            ledger["fixed_required_bytes"], ledger["fixed_overflow_bytes"],
            ledger["fixed_stacks_bytes"], ledger["fixed_room_bytes"]),
        pack_image.NUCLEUS_COLD_LEDGER_LINE % (
            ledger["cold_code_bytes"], ledger["cold_code_end"],
            ledger["cold_arena_granules"], ledger["cold_arena_used_granules"],
            ledger["cold_arena_free_granules"], ledger["cold_data_bytes"],
            ledger["cold_data_base"], ledger["cold_room_bytes"]),
        pack_image.SERVICE_HOT_LEDGER_LINE % (
            ledger["service_hot_bytes"],
            ledger["service_hot_capacity_bytes"]),
        pack_image.SERVICE_BOOT_LEDGER_LINE % (
            ledger["service_boot_bytes"],
            ledger["service_boot_capacity_bytes"]),
        pack_image.COMMON_LEDGER_LINE % (
            ledger["common_used_bytes"], ledger["common_capacity_bytes"],
            ledger["common_code_bytes"], ledger["common_data_bytes"],
            ledger["common_ceiling_bytes"],
            ledger["common_m10_reservation_bytes"],
            ledger["common_limit_bytes"]),
    ))


def align2(value):
    return (value + 1) & ~1


def align2_down(value):
    return value & ~1


class ServiceTierPlan:
    """The bank-6 tier allocation the loader performs (architecture 36.6).

    Pass one reserves the service I/O buffers first: every image's io
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
        self.io_cursor = SERVICE_IO_END
        self.hot_cursor = HOT_TIER_BASE
        self.boot_cursor = BOOT_TIER_BASE
        self.bulk_cursor = BOOT_TIER_END

    def reserve_io(self, size):
        if size == 0:
            return None
        base = align2_down(self.io_cursor - size)
        if base < SERVICE_IO_BASE:
            raise ZmodError(
                "service I/O data needs %d bytes; the %04Xh-%04Xh range "
                "holds %d" % (size, SERVICE_IO_BASE, SERVICE_IO_END,
                              SERVICE_IO_END - SERVICE_IO_BASE))
        self.io_cursor = base
        return base

    def place_hot(self, size):
        if size == 0:
            return None
        base = align2(self.hot_cursor)
        if base + size > self.io_cursor and base < SERVICE_IO_END:
            base = SERVICE_IO_END
        if base + size > HOT_TIER_END:
            raise ZmodError(
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
            raise ZmodError(
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
            raise ZmodError(
                "bulk data needs %d more bytes; the tier %04Xh-%04Xh is "
                "full" % (self.boot_cursor - base, BOOT_TIER_BASE,
                          BOOT_TIER_END))
        self.bulk_cursor = base
        return base


def service_segments(pack):
    return {segment["role"]: segment for segment in pack.segments}


def segment_size(segment):
    return segment["image_size"] + segment["bss_size"]


def plan_service_set(packs):
    """Host assembly for one image's driver set (architecture 36.11).

    Places every image's segments with the existing tier plan, assigns the
    host endpoint numbers in load order and table order, and resolves each
    image's symbolic dependencies to the typed endpoint reference of an
    earlier (or the image's own) export.  Duplicate-image,
    duplicate-endpoint, tier-fit and overlap checks all run here.  Returns
    (rows, ledger); each pack carries its resolved dependency numbers and
    each endpoint its host number."""
    if not packs:
        raise ZmodError("services: no images given")
    if len(packs) > SERVICE_MAX_IMAGES:
        raise ZmodError("services: %d images; the loader holds %d"
                        % (len(packs), SERVICE_MAX_IMAGES))
    names = [pack.name for pack in packs]
    lowered = [name.lower() for name in names]
    if len(set(lowered)) != len(lowered):
        raise ZmodError("services: duplicate image name in %s"
                        % ", ".join(names))

    seen = {}
    for pack in packs:
        for endpoint in pack.endpoints:
            key = (endpoint["class"], endpoint["instance"])
            if key in seen:
                raise ZmodError(
                    "%s: endpoint %d.%d is already exported by %s"
                    % (pack.name, key[0], key[1], seen[key]))
            seen[key] = pack.name

    plan = ServiceTierPlan()
    rows = [{"name": pack.name} for pack in packs]
    for row, pack in zip(rows, packs):
        io = service_segments(pack).get("io")
        row["io"] = plan.reserve_io(segment_size(io) if io else 0)

    hot_order = sorted(
        range(len(packs)),
        key=lambda index: (-segment_size(
            service_segments(packs[index])["hot"])))
    for index in hot_order:
        segment = service_segments(packs[index])["hot"]
        rows[index]["hot"] = plan.place_hot(segment_size(segment))
    for row, pack in zip(rows, packs):
        segments = service_segments(pack)
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
    if number - 1 > SERVICE_MAX_ENDPOINTS * SERVICE_MAX_IMAGES:
        raise ZmodError("services: %d endpoints; the nucleus table holds %d"
                        % (number - 1,
                           SERVICE_MAX_ENDPOINTS * SERVICE_MAX_IMAGES))

    # Symbolic dependencies: resolved to the typed endpoint reference of
    # an export by this image or an earlier one in the load order.  An
    # optional dependency that no image exports resolves to 0; a missing
    # required one is a build error (architecture 36.11: no fallback).
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
                raise ZmodError(
                    "%s: dependency %d.%d is not exported by an earlier "
                    "image in the load order"
                    % (pack.name, key[0], key[1]))
            dependency["number"] = resolved

    # Publications (work package 42a): every image's declared namespace
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
            root = (SERVICE_FS_ROOT if publication["name"] ==
                    SERVICE_FS_ROOT else SERVICE_DEVICE_ROOT)
            if resolved == 0:
                raise ZmodError(
                    "%s: publication %s names endpoint %d.%d, which no "
                    "image in the load order exports"
                    % (pack.name, root + publication["name"]
                       if root == SERVICE_DEVICE_ROOT
                       else publication["name"], key[0], key[1]))
            publication["number"] = resolved
            full = (root + publication["name"]).lower()
            if full in seen_paths:
                raise ZmodError("%s: %s is already published by %s"
                                % (pack.name, full, seen_paths[full]))
            seen_paths[full] = pack.name
            published += 1
            name = publication["name"]
            if root == SERVICE_DEVICE_ROOT and "/" in name[1:]:
                parent_rows.add(name.rsplit("/", 1)[0].lower())
    if published + len(parent_rows) > SERVICE_MAX_PUBLICATIONS:
        raise ZmodError(
            "services: %d publications and %d grouping rows; the "
            "provider table takes %d rows"
            % (published, len(parent_rows), SERVICE_MAX_PUBLICATIONS))

    ledger = service_ledger(packs, rows)
    return rows, ledger


def _entry_name(entry):
    return entry["name"] if isinstance(entry, dict) else entry.name


def _entry_segments(entry):
    return entry["segments"] if isinstance(entry, dict) else entry.segments


def check_tier_overlap(rows, entries):
    """Every placed block must lie inside its tier and no two blocks may
    overlap (architecture 36.6, fixed driver-bank layout)."""
    for role in ("hot", "io", "boot", "bulk"):
        tier_base, tier_end = SERVICE_TIERS[role]
        blocks = []
        for row, entry in zip(rows, entries):
            for segment in _entry_segments(entry):
                if segment["role"] != role:
                    continue
                base = row[role]
                size = segment_size(segment)
                if base is None or base < tier_base or \
                        base + size > tier_end:
                    raise ZmodError("%s: %s segment outside its tier"
                                    % (_entry_name(entry), role))
                blocks.append((base, size, _entry_name(entry)))
        blocks.sort()
        end = tier_base
        for base, size, name in blocks:
            if base < end:
                raise ZmodError("%s: %s segment at %04Xh overlaps the "
                                "previous block" % (name, role, base))
            end = base + size


def service_ledger(entries, rows):
    """The architecture-36.20 `driver hot` and `driver boot/bulk` ledgers:
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
        "service_hot_bytes": hot_used,
        "service_hot_capacity_bytes": HOT_TIER_END - HOT_TIER_BASE,
        "service_boot_bytes": boot_used,
        "service_boot_capacity_bytes": BOOT_TIER_END - BOOT_TIER_BASE,
    }


def format_service_row(row):
    parts = []
    for role in ("hot", "io", "boot", "bulk"):
        if row.get(role) is not None:
            parts.append("%s %04Xh" % (role, row[role]))
    return "driver %s: %s" % (row["name"], " ".join(parts))


def cmd_services(args):
    if len(args) < 2:
        raise ZmodError("usage: zmodpack.py services <out.man> "
                        "<container.zmod> [...] [--report FILE]")
    out_path = args[0]
    report_path = None
    containers = []
    rest = list(args[1:])
    while rest:
        item = rest.pop(0)
        if item == "--report" and rest:
            report_path = rest.pop(0)
        else:
            containers.append(item)
    if not containers:
        raise ZmodError("services: no containers given")

    infos = []
    names = []
    rows = []
    seen_key = {}
    seen_number = {}
    number_key = {}
    for path in containers:
        data = open(path, "rb").read()
        info = parse_container(data, path)
        if info.get("module_type") != "service":
            raise ZmodError("%s: not a service image" % path)
        name = os.path.basename(path)
        encoded = name.encode("ascii")
        if (len(encoded) > MANIFEST_ENTRY_SIZE - 1 or "/" in name or
                not all(0x20 <= byte <= 0x7E for byte in encoded)):
            raise ZmodError("%s: the ROMFS name %r is not a flat name of at "
                            "most %d characters"
                            % (path, name, MANIFEST_ENTRY_SIZE - 1))
        if name.lower() in [existing.lower() for existing in names]:
            raise ZmodError("%s: duplicate image name %r" % (path, name))
        row = {"name": name}
        for segment in info["segments"]:
            row[segment["role"]] = segment["base"]
        for endpoint in info["endpoints"]:
            key = (endpoint["class"], endpoint["instance"])
            number = endpoint["number"]
            if key in seen_key:
                raise ZmodError(
                    "%s: endpoint %d.%d is already exported by %s"
                    % (path, key[0], key[1], seen_key[key]))
            if number in seen_number:
                raise ZmodError(
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
                # host-configured fallback (architecture 36.11).
                continue
            if number not in number_key:
                raise ZmodError(
                    "%s: dependency %d.%d resolved to endpoint %d, which no "
                    "earlier image exports"
                    % (path, key[0], key[1], number))
            if number_key[number] != key:
                raise ZmodError(
                    "%s: dependency %d.%d resolved to endpoint %d of "
                    "protocol %d.%d"
                    % (path, key[0], key[1], number,
                       number_key[number][0], number_key[number][1]))
        infos.append(info)
        names.append(name)
        rows.append(row)
    if len(infos) > SERVICE_MAX_IMAGES:
        raise ZmodError("services: %d images; the loader holds %d"
                        % (len(infos), SERVICE_MAX_IMAGES))

    placed = [{"name": name, "segments": info["segments"]}
              for name, info in zip(names, infos)]
    check_tier_overlap(rows, placed)
    ledger = service_ledger(placed, rows)

    manifest = bytearray()
    for name in names:
        encoded = name.encode("ascii")
        manifest += encoded + bytes(MANIFEST_ENTRY_SIZE - len(encoded))
    with open(out_path, "wb") as handle:
        handle.write(bytes(manifest))

    for row in rows:
        print("zmodpack: %s" % format_service_row(row))
    print("zmodpack: %s" % (pack_image.SERVICE_HOT_LEDGER_LINE % (
        ledger["service_hot_bytes"], ledger["service_hot_capacity_bytes"])))
    print("zmodpack: %s" % (pack_image.SERVICE_BOOT_LEDGER_LINE % (
        ledger["service_boot_bytes"], ledger["service_boot_capacity_bytes"])))
    print("zmodpack: %s: %d driver images, load order as listed"
          % (out_path, len(infos)))

    if report_path:
        report = {
            "format": "zos-services/1",
            "manifest": out_path,
            "order": names,
            "services": [
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
                for name, path, row, info in zip(names, containers, rows,
                                                 infos)
            ],
            "nucleus": ledger,
        }
        with open(report_path, "w") as handle:
            json.dump(report, handle, indent=2, sort_keys=True)
            handle.write("\n")
    return 0


def cmd_plan(args):
    geometry_path = None
    report_path = None
    modules = []
    rest = list(args)
    while rest:
        item = rest.pop(0)
        if item == "--geometry" and rest:
            geometry_path = rest.pop(0)
        elif item == "--report" and rest:
            report_path = rest.pop(0)
        else:
            modules.append(item)
    if geometry_path is None:
        raise ZmodError("usage: zmodpack.py plan --geometry <memory.inc> "
                        "[--report FILE] [<container.zmod> ...]")
    # An empty boot set is legal: an image whose drivers are all resident
    # services keeps no arena module and the plan reports the whole arena
    # free (work package 41).

    values = parse_geometry_inc(geometry_path)
    start = values["ZOS_DRIVER_ARENA_BASE"]
    end = values["ZOS_DRIVER_ARENA_END"]
    granule = values.get("ZOS_DRIVER_GRANULE_SIZE", GRANULE_SIZE)
    declared = values["ZOS_DRIVER_GRANULES"]
    size = end - start
    if size <= 0 or size % granule != 0 or size // granule != declared:
        raise ZmodError("arena geometry %s is inconsistent "
                        "(%04Xh-%04Xh, %d granules)" % (geometry_path, start,
                                                        end, declared))

    entries = []
    used = 0
    for path in modules:
        data = open(path, "rb").read()
        info = parse_container(data, path)
        name = os.path.splitext(os.path.basename(path))[0]
        used += info["required_granules"]
        total = info["image_size"] + info["bss_size"]
        thresholds = pack_image.granule_thresholds(total, granule)
        entries.append({
            "name": name,
            "path": path,
            "image_size": info["image_size"],
            "bss_size": info["bss_size"],
            "total_size": total,
            "granules": info["required_granules"],
            "above_lower_bytes": thresholds["above_lower_bytes"],
            "before_higher_bytes": thresholds["before_higher_bytes"],
        })

    if used > declared:
        raise ZmodError(
            "declared boot set needs %d granules; the arena holds %d "
            "(%04Xh-%04Xh)" % (used, declared, start, end))

    plan = {
        "arena": {
            "start": start,
            "end": end,
            "size": size,
            "granule_size": granule,
            "granules": declared,
            "used_granules": used,
            "free_granules": declared - used,
        },
        "modules": entries,
    }
    ledger = build_nucleus_ledger(values, used, geometry_path)
    plan["nucleus"] = ledger
    kernel = pack_image.kernel_granule_thresholds(
        values["ZOS_KERNEL_CODE_END"], values["ZOS_WINDOW_DATA_BASE"],
        values["ZOS_WINDOW_DATA_SIZE"], values["ZOS_BANK7_RUNTIME_END"],
        granule)
    plan["kernel"] = kernel
    print("zmodpack: arena %04Xh-%04Xh: %d granules (derived)"
          % (start, end - 1, declared))
    for entry in entries:
        print("zmodpack:   %-12s image %5d + bss %5d = %5d bytes, "
              "%d granules" % (entry["name"], entry["image_size"],
                               entry["bss_size"], entry["total_size"],
                               entry["granules"]))
    print("zmodpack: boot set uses %d of %d granules; %d free"
          % (used, declared, declared - used))
    if ledger is not None:
        print(format_nucleus_ledger(ledger))
    print(pack_image.format_granule_thresholds(entries, kernel))
    if report_path:
        with open(report_path, "w") as handle:
            json.dump(plan, handle, indent=2, sort_keys=True)
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
        if command == "emit-asm":
            return cmd_emit_asm(argv[2:])
        if command == "plan":
            return cmd_plan(argv[2:])
        if command == "services":
            return cmd_services(argv[2:])
        raise ZmodError("unknown command %r" % command)
    except ZmodError as exc:
        print("zmodpack: error: %s" % exc, file=sys.stderr)
        return 1
    except OSError as exc:
        print("zmodpack: error: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
