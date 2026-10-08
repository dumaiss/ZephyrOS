#!/usr/bin/env python3
"""ZephyrOS M4 ZMOD packer/validator.

Consumes SDCC/sdasz80 relocatable (.rel) output plus a small JSON module
manifest and produces a deterministic ZMOD v1 driver container.  The
relocation records in the .rel files are the single source of truth: the
packer mini-links the listed objects at base zero (module-relative
offsets), patches every module-local 16-bit absolute reference, and emits
one ZREL_ABS16 record per patched word.  Pointer inference by scanning
image bytes never happens.

Supported relocation subset (anything else is rejected):

    mode 0x00  word, relocatable area       -> module-local area base
    mode 0x02  word, symbol                 -> module-local symbol

Rejected constructs include byte-fragmented address stores (SDCC emits
these for stack-built descriptors), PC-relative/8-bit/3-byte/paged
relocations, unresolved externals, and patches outside the image.

Container layout (all little-endian):

    offset 0                      28-byte header
    offset reloc_offset           4 * reloc_count relocation records
                                  (type=1 ZREL_ABS16, reserved=0,
                                   patch_offset:u16)
    offset payload_offset         linked-at-zero image
    total_size = payload_offset + image_size

Manifest (JSON), paths resolved against --objdir (default: manifest dir):

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

Usage:
    zmodpack.py pack <manifest.json> <out.zmod> [--objdir DIR]
                     [--report FILE]
    zmodpack.py validate <container.zmod> [...]
    zmodpack.py dump <container.zmod> [...]
    zmodpack.py emit-asm <out.asm> SYMBOL=container.zmod [...]
    zmodpack.py plan --geometry <memory.inc> [--report FILE]
                     <container.zmod> [...]

The `plan` command reports every module's image/BSS/granules, the
boot-set totals against the derived arena declared by the generated
geometry memory include, and the single bank-7 budget ledger line.  It
fails when the declared boot set cannot coexist in the arena.
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
            # H <areas> areas <globals> global symbols
            try:
                header_counts = (int(fields[1]), int(fields[3], 16))
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
        self.name = name
        self.image = bytearray()
        self.bss_size = 0
        self.relocs = []          # sorted patch offsets
        self.init_offset = 0
        self.fini_offset = NO_ENTRY
        self.symbols = {}         # module-relative Def offsets


def load_manifest(path, objdir):
    try:
        with open(path, "r") as handle:
            manifest = json.load(handle)
    except (OSError, ValueError) as exc:
        raise ZmodError("cannot read manifest %s: %s" % (path, exc))

    required = ("name", "objects", "image_areas", "init")
    for field in required:
        if field not in manifest:
            raise ZmodError("manifest %s: missing %r" % (path, field))

    if manifest.get("type", "driver") != "driver":
        raise ZmodError("manifest %s: only module type 'driver' is supported"
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

    return {
        "name": str(manifest["name"]),
        "objects": objects,
        "object_names": [str(name) for name in manifest["objects"]],
        "image_areas": image_areas,
        "bss_areas": bss_areas,
        "init": manifest["init"],
        "fini": manifest.get("fini"),
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


def pack_manifest(manifest):
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
        rreserved = data[rec + 1]
        patch = get16(data, rec + 2)
        if rtype != ZREL_ABS16:
            raise ZmodError("%s: unsupported relocation type %d" % (source, rtype))
        if rreserved != 0:
            raise ZmodError("%s: relocation %d has a bad reserved byte"
                            % (source, i))
        if patch + 2 > image_size:
            raise ZmodError("%s: relocation %d patch %04Xh crosses the image"
                            % (source, i, patch))
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
    lines.append("  format:        ZMOD v%d driver, ABI %d"
                 % (FORMAT_VERSION, DRIVER_ABI_VERSION))
    lines.append("  image:         %d bytes" % info["image_size"])
    lines.append("  bss:           %d bytes" % info["bss_size"])
    lines.append("  relocations:   %d (ZREL_ABS16)" % info["reloc_count"])
    lines.append("  init:          +0x%04X" % info["init_offset"])
    if info["fini_offset"] == NO_ENTRY:
        lines.append("  fini:          none")
    else:
        lines.append("  fini:          +0x%04X" % info["fini_offset"])
    lines.append("  total:         %d bytes" % info["total_size"])
    lines.append("  allocation:    %d x %d-byte granules"
                 % (info["required_granules"], GRANULE_SIZE))
    return "\n".join(lines)


def report_json(info, name, packed=None):
    report = {
        "name": name,
        "format_version": FORMAT_VERSION,
        "module_type": "driver",
        "driver_abi": DRIVER_ABI_VERSION,
        "image_size": info["image_size"],
        "bss_size": info["bss_size"],
        "required_granules": info["required_granules"],
        "init_offset": info["init_offset"],
        "fini_offset": info["fini_offset"],
        "reloc_count": info["reloc_count"],
        "total_size": info["total_size"],
        "image_sha256": hashlib.sha256(info["payload"]).hexdigest(),
        "relocations": [patch for _, patch in info["relocs"]],
    }
    if packed is not None:
        report["symbols"] = dict(sorted(packed.symbols.items()))
    return report


# --------------------------------------------------------------------------
# Commands
# --------------------------------------------------------------------------

def cmd_pack(args):
    if len(args) < 2:
        raise ZmodError("usage: zmodpack.py pack <manifest> <out.zmod> "
                        "[--objdir DIR] [--report FILE]")
    manifest_path, out_path = args[0], args[1]
    objdir = None
    report_path = None
    rest = args[2:]
    while rest:
        flag = rest.pop(0)
        if flag == "--objdir" and rest:
            objdir = rest.pop(0)
        elif flag == "--report" and rest:
            report_path = rest.pop(0)
        else:
            raise ZmodError("unknown pack option %r" % flag)

    manifest = load_manifest(manifest_path, objdir)
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
    if geometry_path is None or not modules:
        raise ZmodError("usage: zmodpack.py plan --geometry <memory.inc> "
                        "[--report FILE] <container.zmod> [...]")

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
        raise ZmodError("unknown command %r" % command)
    except ZmodError as exc:
        print("zmodpack: error: %s" % exc, file=sys.stderr)
        return 1
    except OSError as exc:
        print("zmodpack: error: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
