#!/usr/bin/env python3
"""exeplace: the Kona Executable placer (architecture 7.2, sdk.md 3).

Places a program larger than its bank: every object module - a program
object or a linked SDK library member - gets a zone (resident, home or a
far segment), and every call that crosses zones goes through a generated
resident veneer.  The placer works on the objects before the link: it
resolves the library members the way sdldz80 does, reads each module's
areas, symbols and relocations, assigns the zones, then writes renamed
copies of the objects, the veneer module and the link script.

The objects are ASxxxx XL4 relocatable files (text, little-endian, 4-byte
addresses):

    H <n> areas <m> global symbols
    M <module>
    S <name> Def<value> | Ref<value>     symbols, indexed in file order
    A <area> size <hex> flags <hex> addr <hex>
    T a0 a1 a2 a3 b0 b1 ...              area offset, then bytes
    R 00 00 i0 i1 (mode pos x0 x1)*      relocations of the T line above:
                                         the area index, then per item the
                                         mode, the byte position in the T
                                         line and the area/symbol index

Usage:
    exeplace.py analyze LINKSCRIPT
    exeplace.py plan LINKSCRIPT [RESIDENT.rel ...] [MODULE=ZONE ...]
    exeplace.py link LINKSCRIPT OUTDIR STACK [RESIDENT.rel ...] [MODULE=ZONE ...]

`link` writes OUTDIR/placed.lk (the link script to run instead), and for a
placed program the rewritten objects, the veneer and area-order modules,
OUTDIR/placement.txt and OUTDIR/placement.json.  A program whose code and
data fit its bank, with nothing pinned, links unchanged: placed.lk is
LINKSCRIPT.
"""

import json
import os
import re
import subprocess
import sys
from collections import OrderedDict

R_BYTE = 0x01
R_SYM = 0x02
R_PCR = 0x04

# Opcodes whose 16-bit operand is a code target: CALL, JP and their
# conditional forms.  Any other relocated word is an address reference.
CALL_OPS = {0xCD, 0xC4, 0xCC, 0xD4, 0xDC, 0xE4, 0xEC, 0xF4, 0xFC}
JUMP_OPS = {0xC3, 0xC2, 0xCA, 0xD2, 0xDA, 0xE2, 0xEA, 0xF2, 0xFA}

CODE_AREAS = {"_CODE", "_HOME", "_GSINIT", "_GSFINAL", "_CABS"}
DATA_AREAS = {"_DATA", "_INITIALIZED", "_INITIALIZER", "_DABS"}
# A program's constant data (mk/kos.mk compiles its C sources with
# --constseg RCONST): always resident, in the image after the code, since
# a pointer to a string or a table may be read from any zone.
CONST_AREA = "_RCONST"


class PlaceError(Exception):
    pass


class Module(object):
    """One relocatable module: its areas, symbols and references."""

    def __init__(self, name, origin, text):
        self.name = name
        self.origin = origin          # "path" or "lib.lib(member)"
        self.text = text
        self.areas = []               # [name, size, flags]
        self.symbols = []             # [name, defined, area_index, value]
        self.refs = []                # (symbol, kind, from_area)
        self._parse()

    def _parse(self):
        current_area = -1
        images = {}
        last_t = None
        for line in self.text.splitlines():
            if not line:
                continue
            tag = line[0]
            if tag == "A":
                m = re.match(r"A (\S+) size (\S+) flags (\S+)", line)
                self.areas.append([m.group(1), int(m.group(2), 16),
                                   int(m.group(3), 16)])
                current_area = len(self.areas) - 1
                images[current_area] = {}
            elif tag == "S":
                m = re.match(r"S (\S+) (Def|Ref)([0-9A-Fa-f]+)", line)
                defined = m.group(2) == "Def"
                self.symbols.append([m.group(1), defined,
                                     current_area if defined else -1,
                                     int(m.group(3), 16)])
            elif tag == "T":
                last_t = [int(x, 16) for x in line[2:].split()]
            elif tag == "R":
                items = [int(x, 16) for x in line[2:].split()]
                area = items[2] | (items[3] << 8)
                if last_t is None:
                    continue
                base = (last_t[0] | (last_t[1] << 8) |
                        (last_t[2] << 16) | (last_t[3] << 24))
                data = last_t[4:]
                img = images.setdefault(area, {})
                for i, byte in enumerate(data):
                    img[base + i] = byte
                for k in range(4, len(items), 4):
                    mode, pos = items[k], items[k + 1]
                    index = items[k + 2] | (items[k + 3] << 8)
                    if not mode & R_SYM:
                        continue
                    symbol = self.symbols[index][0]
                    at = base + pos - 4
                    kind = "byte" if mode & R_BYTE else "addr"
                    if kind == "addr":
                        op = img.get(at - 1)
                        if op in CALL_OPS:
                            kind = "call"
                        elif op in JUMP_OPS:
                            kind = "jump"
                    self.refs.append((symbol, kind,
                                      self.areas[area][0]
                                      if area < len(self.areas) else "?"))
                last_t = None

    def defs(self):
        return [s[0] for s in self.symbols if s[1] and s[0] != ".__.ABS."]

    def undefined(self):
        return [s[0] for s in self.symbols if not s[1]]

    def size(self, kinds):
        return sum(a[1] for a in self.areas if a[0] in kinds)

    def code_size(self):
        return sum(a[1] for a in self.areas
                   if a[0] not in DATA_AREAS and
                   a[0] not in (CONST_AREA, "_KOS_SDK_DATA"))

    def const_size(self):
        return sum(a[1] for a in self.areas if a[0] == CONST_AREA)

    def data_size(self):
        return sum(a[1] for a in self.areas if a[0] in DATA_AREAS)


def read_archive(path):
    """The members of an `ar` archive as (member name, text)."""
    with open(path, "rb") as handle:
        blob = handle.read()
    if not blob.startswith(b"!<arch>\n"):
        raise PlaceError("%s: not an archive" % path)
    pos = 8
    out = []
    longnames = b""
    while pos + 60 <= len(blob):
        header = blob[pos:pos + 60]
        name = header[0:16].decode("latin-1").strip()
        size = int(header[48:58].decode("latin-1").strip())
        body = blob[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
        if name == "//":
            longnames = body
            continue
        if name in ("/", "/SYM64/"):
            continue
        if re.match(r"^/\d+$", name):
            at = int(name[1:])
            name = longnames[at:longnames.index(b"/", at)].decode("latin-1")
        out.append((name.rstrip("/"), body.decode("latin-1")))
    return out


def parse_link_script(path):
    """(objects, libraries, bases) from an sdldz80 -f script."""
    objects, libraries, bases = [], [], OrderedDict()
    root = os.path.dirname(os.path.abspath(path))
    with open(path) as handle:
        for line in handle:
            line = line.strip()
            if not line or line in ("-n", "-m", "-w", "-e"):
                continue
            if line.startswith("-l "):
                libraries.append(line[3:].strip())
            elif line.startswith("-b "):
                name, value = line[3:].split("=")
                bases[name.strip()] = int(value.strip(), 0)
            elif line.startswith("-"):
                continue
            else:
                objects.append(line)
    return objects, libraries, bases


def resolve(objects, libraries, seeds=()):
    """The modules a link of `objects` + `libraries` takes, in sdldz80's
    way: every object, then library members that define a still-undefined
    symbol, the libraries searched in order and again until nothing more
    resolves."""
    modules = []
    for path in objects:
        with open(path, "rb") as handle:
            text = handle.read().decode("latin-1")
        name = re.search(r"^M (\S+)", text, re.M).group(1)
        modules.append(Module(name, path, text))
    defined = set()
    undefined = set()
    for module in modules:
        defined.update(module.defs())
    for module in modules:
        undefined.update(s for s in module.undefined() if s not in defined)
    undefined.update(s for s in seeds if s not in defined)
    members = [(lib, [Module(re.search(r"^M (\S+)", text, re.M).group(1),
                             "%s(%s)" % (os.path.basename(lib), member),
                             text)
                      for member, text in read_archive(lib)])
               for lib in libraries]
    taken = set()
    found = True
    while found:
        found = False
        for lib, mods in members:
            for module in mods:
                if module.origin in taken:
                    continue
                if not any(d in undefined for d in module.defs()):
                    continue
                taken.add(module.origin)
                modules.append(module)
                defined.update(module.defs())
                undefined.difference_update(module.defs())
                undefined.update(s for s in module.undefined()
                                 if s not in defined)
                found = True
    return modules, undefined


def graph(modules):
    """owner: symbol -> module; edges: module -> {module: [kinds]}."""
    owner = {}
    for module in modules:
        for name in module.defs():
            owner[name] = module
    edges = OrderedDict((m.origin, OrderedDict()) for m in modules)
    for module in modules:
        for symbol, kind, _area in module.refs:
            target = owner.get(symbol)
            if target is None or target is module:
                continue
            edges[module.origin].setdefault(target.origin, []).append(
                (symbol, kind))
    return owner, edges


def analyze(script):
    objects, libraries, _bases = parse_link_script(script)
    modules, undefined = resolve(objects, libraries)
    owner, edges = graph(modules)
    code = sum(m.code_size() for m in modules)
    data = sum(m.data_size() for m in modules)
    print("%d modules, code %d, data %d, total %d" %
          (len(modules), code, data, code + data))
    if undefined:
        print("undefined: %s" % " ".join(sorted(undefined)))
    for m in sorted(modules, key=lambda m: -(m.code_size() + m.data_size())):
        kinds = {}
        for target, refs in edges[m.origin].items():
            for _symbol, kind in refs:
                kinds[kind] = kinds.get(kind, 0) + 1
        print("%-34s code %5d data %5d  refs %s" %
              (m.origin, m.code_size(), m.data_size(),
               " ".join("%s:%d" % kv for kv in sorted(kinds.items()))))
    return modules, edges


# --- placement --------------------------------------------------------

# SDK libraries whose data is private: they never hand out a pointer to
# their own state, so that state may follow their code out of the
# resident zone.  Any other data - a program's above all, whose pointers
# go anywhere - is resident (a pointer's flow is not in the relocations).
PRIVATE_DATA_LIBS = ("libgfx.lib(",)

LOW_BASE = 0x0100
APERTURE = 0x8000
HIGH_BASE = 0xC000
STACK_TOP = 0xE000
ZONE_SIZE = 0x4000
FAR_MAX = 4



class Plan(object):
    """Each unit's zone: a unit is (module origin, "code" | "data")."""

    def __init__(self):
        self.zone = OrderedDict()
        self.size = OrderedDict()
        self.why = OrderedDict()

    def used(self, zone, kind=None):
        return sum(self.size[u] for u, z in self.zone.items()
                   if z == zone and (kind is None or u[1] == kind))


def unit_of(module, area):
    if area == CONST_AREA:
        return (module.origin, "const")
    return (module.origin, "data" if area in DATA_AREAS else "code")


def unit_edges(modules):
    """unit -> OrderedDict(target unit -> [(symbol, kind)])."""
    owner = {}
    for module in modules:
        for name, defined, area, _value in module.symbols:
            if defined and name != ".__.ABS." and 0 <= area < len(module.areas):
                owner[name] = unit_of(module, module.areas[area][0])
    edges = OrderedDict()
    for module in modules:
        for kind in ("code", "data", "const"):
            edges[(module.origin, kind)] = OrderedDict()
        for symbol, kind, from_area in module.refs:
            target = owner.get(symbol)
            source = unit_of(module, from_area)
            if target is None or target == source:
                continue
            edges[source].setdefault(target, []).append((symbol, kind))
    return edges


def place(modules, stack, resident_sources, pins):
    """Assign every unit a zone: "resident", "home" or "farN".

    resident_sources: program object paths that are resident roots;
    pins: {module origin: zone}."""
    plan = Plan()
    edges = unit_edges(modules)
    for module in modules:
        plan.size[(module.origin, "code")] = module.code_size()
        plan.size[(module.origin, "data")] = module.data_size()
        plan.size[(module.origin, "const")] = module.const_size()
        plan.zone[(module.origin, "const")] = "resident"
        plan.why[(module.origin, "const")] = "const"
    # The resident roots and their closure.
    # The runtime (crt0 and libkos) is resident because the trampolines,
    # stubs and event delivery run whatever is mapped; its own calls out
    # (crt0 to main) happen with the aperture closed, so they are not
    # followed.  Handler sources are followed: a handler runs wherever
    # the program was interrupted.
    roots = []
    for module in modules:
        o = module.origin
        if o.endswith("crt0.rel") or o.startswith("libkos.lib("):
            plan.zone[(o, "code")] = "resident"
            plan.why[(o, "code")] = "runtime"
        elif o in resident_sources or pins.get(o) == "resident":
            roots.append((o, "code"))
    stack_units = list(roots)
    while stack_units:
        unit = stack_units.pop()
        if plan.zone.get(unit) == "resident":
            continue
        if unit not in plan.zone:
            plan.zone[unit] = "resident"
            plan.why[unit] = "root" if unit in roots else "reached"
        elif unit not in roots:
            continue
        for target in edges[unit]:
            if target[1] == "code" and plan.zone.get(target) != "resident":
                stack_units.append(target)
    # Data is resident by default.
    for unit in plan.size:
        if unit[1] == "data":
            plan.zone.setdefault(unit, "resident")
            plan.why.setdefault(unit, "data")
    # Pins.
    for unit in plan.size:
        if unit[1] == "code" and unit not in plan.zone and unit[0] in pins:
            plan.zone[unit] = pins[unit[0]]
            plan.why[unit] = "pin"
    # Free code: resident while it fits, most-called first; then home
    # and far segments by call affinity.
    callers = {}
    for source, targets in edges.items():
        for target, refs in targets.items():
            callers[target] = callers.get(target, 0) + len(refs)
    free = [u for u in plan.size if u[1] == "code" and u not in plan.zone
            and plan.size[u] > 0]
    high_room = STACK_TOP - stack - HIGH_BASE
    def resident_room():
        data = plan.used("resident", "data")
        low_data = max(0, data - high_room)
        return ((APERTURE - LOW_BASE - SDK_LOW) - plan.used("resident", "code")
                - plan.used("resident", "const") - low_data)
    for unit in sorted(free, key=lambda u: (-callers.get(u, 0), u[0])):
        if plan.size[unit] <= resident_room():
            plan.zone[unit] = "resident"
            plan.why[unit] = "room"
    rest = [u for u in free if u not in plan.zone]
    # Group the rest along the call graph: merge the two groups with the
    # most call sites between them while the pair fits one zone.
    weight = {}
    for source, targets in edges.items():
        for target, refs in targets.items():
            if source in rest and target in rest:
                key = tuple(sorted((source, target)))
                weight[key] = weight.get(key, 0) + len(refs)
    # A library places as one unit: its members hand each other pointers
    # to their own constants, which must be visible to all of them.
    group = {u: (u,) for u in rest}
    libraries = OrderedDict()
    for u in rest:
        if ".lib(" in u[0]:
            libraries.setdefault(u[0].split("(")[0], []).append(u)
    for units in libraries.values():
        merged = tuple(sorted(units))
        for u in merged:
            group[u] = merged
    # A group carries the data only its own code touches: that data goes
    # where the group goes, leaving the resident zone to shared state.
    data_users = {}
    for source, targets in edges.items():
        for target in targets:
            if (target[1] == "data" and plan.why.get(target) == "data" and
                    target[0].startswith(PRIVATE_DATA_LIBS)):
                data_users.setdefault(target, set()).add(source)
    def own_data(g):
        members = set(g)
        return [d for d, users in data_users.items()
                if users and users <= members and plan.size[d]]
    def gsize(g):
        return (sum(plan.size[u] for u in g) +
                sum(plan.size[d] for d in own_data(g)))
    while True:
        between = {}
        for (a, b), w in weight.items():
            ga, gb = group[a], group[b]
            if ga != gb:
                key = tuple(sorted((ga, gb)))
                between[key] = between.get(key, 0) + w
        best = None
        for (ga, gb), w in sorted(between.items()):
            if gsize(tuple(sorted(ga + gb))) <= ZONE_SIZE and (
                    best is None or w > best[0]):
                best = (w, ga, gb)
        if best is None:
            break
        merged = tuple(sorted(best[1] + best[2]))
        for u in merged:
            group[u] = merged
    groups = sorted(set(group.values()), key=lambda g: (-gsize(g), g))
    zones = ["home"] + ["far%d" % n for n in range(1, FAR_MAX + 1)]
    for g in groups:
        chosen, score = None, None
        for zone in zones:
            if plan.used(zone) + gsize(g) > ZONE_SIZE:
                continue
            affinity = sum(w for (a, b), w in weight.items()
                           if (a in g and plan.zone.get(b) == zone) or
                           (b in g and plan.zone.get(a) == zone))
            if score is None or affinity > score:
                chosen, score = zone, affinity
        if chosen is None:
            raise PlaceError("a group of %d bytes (%s) fits no zone" %
                             (gsize(g), g[0][0]))
        for u in g:
            plan.zone[u] = chosen
            plan.why[u] = "group %d, affinity %d" % (gsize(g), score)
        for d in own_data(g):
            plan.zone[d] = chosen
            plan.why[d] = "data of its group"
    # Data only one non-resident zone touches may live there (a far
    # segment's BSS, or the home zone), largest first while the resident
    # zone is over.
    if resident_room() < 0:
        users = {}
        for source, targets in edges.items():
            for target in targets:
                if target[1] == "data":
                    users.setdefault(target, set()).add(plan.zone[source])
        movable = [u for u in plan.size
                   if u[1] == "data" and plan.zone[u] == "resident" and
                   u[0].startswith(PRIVATE_DATA_LIBS) and
                   plan.size[u] and len(users.get(u, ())) == 1 and
                   "resident" not in users[u]]
        for unit in sorted(movable, key=lambda u: (-plan.size[u], u[0])):
            if resident_room() >= 0:
                break
            zone = next(iter(users[unit]))
            if plan.used(zone) + plan.size[unit] <= ZONE_SIZE:
                plan.zone[unit] = zone
                plan.why[unit] = "data of %s only" % zone
    if resident_room() < 0:
        raise PlaceError("the resident zone is %d bytes over\n%s" %
                         (-resident_room(), report(plan, edges, stack)))
    return plan, edges


def veneers(plan, edges):
    """The cross-zone calls that need a veneer, and the violations."""
    need, bad = OrderedDict(), []
    for source, targets in edges.items():
        if plan.size.get(source, 0) == 0:
            continue
        zs = plan.zone[source]
        for target, refs in targets.items():
            zt = plan.zone[target]
            if zt == "resident" or zt == zs:
                continue
            for symbol, kind in refs:
                if kind in ("call", "jump") and target[1] == "code":
                    need.setdefault(symbol, zt)
                else:
                    bad.append((source[0], symbol, kind, zs, zt))
    return need, bad


def report(plan, edges, stack):
    out = []
    for zone in ["resident", "home"] + ["far%d" % n for n in range(1, FAR_MAX + 1)]:
        units = [u for u, z in plan.zone.items() if z == zone and plan.size[u]]
        if not units:
            continue
        out.append("%s: code %d, data %d" % (zone, plan.used(zone, "code"),
                                             plan.used(zone, "data")))
        for u in sorted(units, key=lambda u: -plan.size[u]):
            out.append("  %-6d %-4s %-34s %s" % (plan.size[u], u[1], u[0],
                                                plan.why[u]))
    need, bad = veneers(plan, edges)
    out.append("veneers: %d" % len(need))
    for symbol, zone in need.items():
        out.append("  %s -> %s" % (symbol, zone))
    for item in bad:
        out.append("VIOLATION %s: %s (%s) %s -> %s" % item)
    return "\n".join(out)


# --- the placed link ----------------------------------------------------

CODE_AREA = {"resident": "_CODE", "home": "_ZHOME"}
DATA_AREA = {"home": "_ZHOMEB"}
VENEER_BYTES = 6       # CALL kos_vcall, the zone byte, the target word
SDK_LOW = 0x005D       # _KOS_SDK_DATA before _CODE


def zone_code_area(zone):
    return CODE_AREA.get(zone) or "_FAR%s" % zone[3:]


def zone_data_area(zone):
    return DATA_AREA.get(zone) or "_FAR%sB" % zone[3:]


def zone_id(zone):
    return 0 if zone == "home" else int(zone[3:])


def fits_bank(modules, stack):
    total = sum(m.code_size() + m.data_size() + m.const_size()
                for m in modules)
    return LOW_BASE + SDK_LOW + total <= STACK_TOP - stack


def rewrite(module, zone_code, zone_data, renames):
    """The module's text with its areas renamed for its zones and the
    symbols in `renames` (references through veneers) renamed."""
    out = []
    for line in module.text.splitlines():
        if line.startswith("A "):
            m = re.match(r"A (\S+) (size (\S+) .*)$", line)
            name, rest, size = m.group(1), m.group(2), int(m.group(3), 16)
            if name == "_CODE":
                name = zone_code
            elif name == "_DATA":
                name = zone_data
            elif size and name not in ("_KOS_SDK_DATA", CONST_AREA):
                # _KOS_SDK_DATA is the runtime's fixed low area at 0100h;
                # the constants stay in their resident area.
                raise PlaceError("%s: area %s (%d bytes) cannot be placed"
                                 % (module.origin, name, size))
            line = "A %s %s" % (name, rest)
        elif line.startswith("S "):
            m = re.match(r"S (\S+) (Ref.*)$", line)
            if m and m.group(1) in renames:
                line = "S %s %s" % (renames[m.group(1)], m.group(2))
        out.append(line)
    return "\n".join(out) + "\n"


def veneer_name(symbol):
    return "__vn" + symbol


def assemble(source, workdir):
    subprocess.run(["sdasz80", "-plosgff", source], cwd=workdir, check=True,
                   stdout=subprocess.DEVNULL)
    return os.path.join(workdir, os.path.splitext(
        os.path.basename(source))[0] + ".rel")


def placed_link(script, outdir, stack, resident_sources, pins):
    objects, libraries, bases = parse_link_script(script)
    modules, _undefined = resolve(objects, libraries)
    os.makedirs(outdir, exist_ok=True)
    lk = os.path.join(outdir, "placed.lk")
    # Hand-declared far code or data (FARn=, DATAn=) keeps its own
    # layout; the placer handles programs written as one bank.
    declared = any(re.match(r"^_(FAR|DATA)\d", area[0]) and area[1]
                   for module in modules for area in module.areas)
    if declared or (fits_bank(modules, stack) and not resident_sources
                    and not pins):
        with open(script) as src, open(lk, "w") as dst:
            dst.write(src.read())
        with open(os.path.join(outdir, "placement.json"), "w") as handle:
            handle.write("null\n")
        return None
    plan, edges = place(modules, stack, resident_sources, pins)
    need, bad = veneers(plan, edges)
    if need:
        # The veneers call the trampoline: its libkos members join the
        # link (resident, as all of libkos) and the placement is redone.
        modules, _undefined = resolve(objects, libraries, ("kos_vcall",))
        plan, edges = place(modules, stack, resident_sources, pins)
        need, bad = veneers(plan, edges)
    if bad:
        raise PlaceError("\n".join("%s: %s (%s) from %s into %s" % b
                                   for b in bad))
    # Resident data: the largest units fill C000h up to the stack, the
    # rest follows the resident code below 8000h.
    high_room = STACK_TOP - stack - HIGH_BASE
    high, low = [], []
    resident_data = sorted((u for u in plan.size if u[1] == "data" and
                            plan.zone[u] == "resident" and plan.size[u]),
                           key=lambda u: (-plan.size[u], u[0]))
    for unit in resident_data:
        if plan.size[unit] <= high_room:
            high.append(unit)
            high_room -= plan.size[unit]
        else:
            low.append(unit)
    code_end = (LOW_BASE + SDK_LOW + plan.used("resident", "code") +
                VENEER_BYTES * len(need))
    const_end = code_end + plan.used("resident", "const")
    low_end = const_end + sum(plan.size[u] for u in low)
    if low_end > APERTURE:
        raise PlaceError("the resident zone below 8000h ends at %05Xh"
                         % low_end)
    # The zones at 8000h in the order their record streams must come out
    # of the link (exepack takes the home zone's first, then the far
    # segments by id).
    zones_used = sorted(set(z for u, z in plan.zone.items()
                            if plan.size[u] and z != "resident"),
                        key=lambda z: zone_id(z))
    # Rewrite every module.
    workdir = os.path.join(outdir, "placed")
    os.makedirs(workdir, exist_ok=True)
    paths = []
    for index, module in enumerate(modules):
        zc = plan.zone[(module.origin, "code")]
        zd = plan.zone[(module.origin, "data")]
        code_area = zone_code_area(zc)
        if zd == "resident":
            data_area = "_DATA" if (module.origin, "data") in high else "_RDATA"
        else:
            data_area = zone_data_area(zd)
        renames = {}
        for symbol, kind, _area in module.refs:
            if symbol in need and need[symbol] != zc:
                renames[symbol] = veneer_name(symbol)
        path = os.path.join(workdir, "%03d_%s.rel" % (index, module.name))
        with open(path, "w") as handle:
            handle.write(rewrite(module, code_area, data_area, renames))
        paths.append(path)
    # The veneers and the area order.
    lines = ["\t.module kos_veneers", "\t.globl kos_vcall"]
    for symbol in need:
        lines.append("\t.globl %s" % symbol)
    lines.append("\t.area _CODE")
    for symbol, zone in need.items():
        lines += ["%s::" % veneer_name(symbol), "\tcall\tkos_vcall",
                  "\t.db\t%d" % zone_id(zone), "\t.dw\t%s" % symbol]
    with open(os.path.join(workdir, "veneers.asm"), "w") as handle:
        handle.write("\n".join(lines) + "\n")
    order = ["\t.module kos_place_order", "\t.area _KOS_SDK_DATA",
             "\t.area _CODE", "\t.area %s" % CONST_AREA, "\t.area _RDATA"]
    for zone in zones_used:
        order.append("\t.area %s" % zone_code_area(zone))
        order.append("\t.area %s" % zone_data_area(zone))
    order.append("\t.area _DATA")
    with open(os.path.join(workdir, "order.asm"), "w") as handle:
        handle.write("\n".join(order) + "\n")
    order_rel = assemble("order.asm", workdir)
    veneer_rel = assemble("veneers.asm", workdir)
    # The link script.
    ihx = None
    with open(script) as handle:
        for line in handle:
            if line.startswith("-i "):
                ihx = line.strip()
    out = ["-n", "-m", "-w", ihx,
           "-b _KOS_SDK_DATA = 0x%04X" % LOW_BASE,
           "-b _CODE = 0x%04X" % (LOW_BASE + SDK_LOW),
           "-b %s = 0x%04X" % (CONST_AREA, code_end),
           "-b _RDATA = 0x%04X" % const_end]
    for zone in zones_used:
        code = plan.used(zone, "code")
        out.append("-b %s = 0x%04X" % (zone_code_area(zone), APERTURE))
        out.append("-b %s = 0x%04X" % (zone_data_area(zone), APERTURE + code))
    out.append("-b _DATA = 0x%04X" % HIGH_BASE)
    # sdldz80 writes the IHX records module by module, so the bytes the
    # zones stack at 8000h come out in the order of the modules covering
    # them: list the modules zone by zone (the resident ones, then the
    # home zone, then the far segments by id), the order exepack reads.
    rank = {"resident": -1}
    def zone_rank(path, module):
        zone = plan.zone[(module.origin, "code")]
        data = plan.zone[(module.origin, "data")]
        if zone == "resident" and data != "resident":
            zone = data
        return rank.get(zone, zone_id(zone) if zone != "resident" else -1)
    ordered = [p for _r, _i, p in sorted(
        (zone_rank(p, m), i, p) for i, (p, m) in enumerate(zip(paths, modules)))]
    out += [order_rel] + ordered + [veneer_rel, "-e"]
    with open(lk, "w") as handle:
        handle.write("\n".join(out) + "\n")
    with open(os.path.join(outdir, "placement.txt"), "w") as handle:
        handle.write(report(plan, edges, stack) + "\n")
    segments = [{"id": zone_id(z), "kind": "FAR_CODE",
                 "area": zone_code_area(z), "bss_area": zone_data_area(z)}
                for z in zones_used if z != "home"]
    placement = {"home": "home" in zones_used,
                 "home_area": "_ZHOME", "home_bss_area": "_ZHOMEB",
                 "segments": segments,
                 "objects": [os.path.abspath(p) for p in paths + [veneer_rel]],
                 "zones": {p: plan.zone[(m.origin, "code")]
                           for p, m in zip(paths, modules)}}
    with open(os.path.join(outdir, "placement.json"), "w") as handle:
        json.dump(placement, handle, indent=1)
    return placement


def area_zone(area):
    """The zone a placed area belongs to, or None for an unplaced one."""
    if area in ("_CODE", CONST_AREA, "_RDATA", "_DATA", "_KOS_SDK_DATA"):
        return "resident"
    if area in ("_ZHOME", "_ZHOMEB"):
        return "home"
    m = re.match(r"^_FAR(\d)B?$", area)
    return "far" + m.group(1) if m else None


def verify(paths):
    """The link rule over placed objects: a reference is direct only to a
    resident target or within its own zone.  The veneer module is the
    sanctioned crossing.  Returns the violations, empty when it holds."""
    modules = []
    for path in paths:
        with open(path, "rb") as handle:
            text = handle.read().decode("latin-1")
        modules.append(Module(re.search(r"^M (\S+)", text, re.M).group(1),
                              path, text))
    owner = {}
    for module in modules:
        for name, defined, area, _value in module.symbols:
            if defined and name != ".__.ABS." and 0 <= area < len(module.areas):
                owner[name] = area_zone(module.areas[area][0])
    problems = []
    for module in modules:
        if module.name == "kos_veneers":
            continue
        for symbol, kind, from_area in module.refs:
            source, target = area_zone(from_area), owner.get(symbol)
            if source is None or target is None:
                continue
            if target != "resident" and target != source:
                problems.append("%s: %s %s from %s into %s"
                                % (module.name, kind, symbol, source, target))
    return problems


def main(argv):
    if len(argv) == 3 and argv[1] == "analyze":
        analyze(argv[2])
        return 0
    if len(argv) >= 5 and argv[1] == "link":
        resident = set(a for a in argv[5:] if "=" not in a)
        pins = dict(a.split("=", 1) for a in argv[5:] if "=" in a)
        try:
            placed_link(argv[2], argv[3], int(argv[4], 0), resident, pins)
        except PlaceError as exc:
            sys.stderr.write("exeplace: %s\n" % exc)
            return 1
        return 0
    if len(argv) >= 3 and argv[1] == "plan":
        objects, libraries, _bases = parse_link_script(argv[2])
        modules, _undefined = resolve(objects, libraries)
        resident = set(a for a in argv[3:] if not "=" in a)
        pins = dict(a.split("=", 1) for a in argv[3:] if "=" in a)
        plan, edges = place(modules, 1024, resident, pins)
        print(report(plan, edges, 1024))
        return 0
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
