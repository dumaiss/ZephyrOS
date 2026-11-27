#!/usr/bin/env python3
"""Copy ledger, driver residency, system-DAT occupancy and working sets.

Host-side analysis on production bytes and the existing simulator peers. It
reuses the performance runner's identity (linked objects, driver layout from
tools/modpack.py) and its rows, and adds a trace to each span: every payload
copy pass with its linked site, byte count and T-states; every system-DAT
write; and the driver code and state a call actually executes and touches.
Nothing here is part of an image.

Prerequisites: make [BOARD=mk2], make sdk [BOARD=mk2], make perf-probe
[BOARD=mk2]. Output is versioned JSON ("kos-ledger/1").
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path[:0] = [str(HERE), str(ROOT / 'tools')]
import modpack
import pack_image
import perf_os_sdk as perf
from perf_os_sdk import PATTERN, CODE, BLOCK, TEXT, RETURN, physical, put, get

BaseSpan = perf.Span

# Instruction classes the trace follows.
RET_OPS = frozenset([0xC9, 0xC0, 0xC8, 0xD0, 0xD8, 0xE0, 0xE8, 0xF0, 0xF8])
CALL_OPS = frozenset([0xCD, 0xC4, 0xCC, 0xD4, 0xDC, 0xE4, 0xEC, 0xF4, 0xFC])
RST_OPS = frozenset(range(0xC7, 0x100, 8))
LD_RR_NN = {0x4B, 0x5B, 0x6B, 0x7B}
LD_NN_RR = {0x43, 0x53, 0x63, 0x73}
HL_READ = frozenset([0x46, 0x4E, 0x56, 0x5E, 0x66, 0x6E, 0x7E, 0x86, 0x8E, 0x96, 0x9E,
                     0xA6, 0xAE, 0xB6, 0xBE])
HL_WRITE = frozenset([0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x77, 0x36])
HL_RMW = frozenset([0x34, 0x35])
# Peripheral data ports: the payload loops of the board's devices.
PORT_GROUP = {0x30: 'ioc_bulk', 0x32: 'ioc_command', 0x22: 'sio_data', 0xA0: 'vdp_data',
              0xA2: 'vdp_palette', 0xE0: 'sound', 0xE1: 'sound', 0xE2: 'sound',
              0xE3: 'sound', 0xE4: 'sound'}
PIO_GAP = 300           # steps between port accesses of one loop
COPY_GAP = 12           # the runner's own run definition
SYSTEM_ENTRIES = 8
STAGES = ('driver', 'copy', 'cold', 'interrupt', 'nucleus')

_Z80_LEN = pack_image._Z80_LENGTH
_INDEXED = pack_image._INDEX_DISPLACEMENT


def insn_length(op, ext, nxt=0):
    """Encoded length of the instruction whose bytes begin op, ext."""
    if op in (0xCB, 0xED):
        return 4 if op == 0xED and ext in LD_RR_NN | LD_NN_RR else 2
    if op in (0xDD, 0xFD):
        if ext == 0xCB:
            return 4
        return 1 + _Z80_LEN[ext] + (1 if ext in _INDEXED else 0)
    return _Z80_LEN[op]


def signed(value):
    return value - 256 if value & 0x80 else value


def data_operands(z, pc, op, ext):
    """Data bytes the instruction at `pc` touches: (kind, address, length)
    with kind r or w. Stack traffic, ports and instruction bytes are not data.
    Registers are the pre-step values; a block move reads BC, HL, DE as they
    stand."""
    out = []
    hl = z.hl
    if op == 0xED:
        if ext in LD_RR_NN or ext in LD_NN_RR:
            addr = z.rd16((pc + 2) & 0xFFFF)
            out.append(('r' if ext in LD_RR_NN else 'w', addr, 2))
        elif ext in (0xA0, 0xA8):
            out += [('r', hl, 1), ('w', z.de, 1)]
        elif ext in (0xB0, 0xB8):
            n = z.bc or 65536
            lo = hl if ext == 0xB0 else (hl - n + 1) & 0xFFFF
            dlo = z.de if ext == 0xB0 else (z.de - n + 1) & 0xFFFF
            out += [('r', lo, n), ('w', dlo, n)]
        elif ext in (0xA1, 0xA9):
            out.append(('r', hl, 1))
        elif ext in (0xA2, 0xAA):
            out.append(('w', hl, 1))
        elif ext in (0xB2, 0xBA):
            n = z.b or 256
            out.append(('w', hl if ext == 0xB2 else (hl - n + 1) & 0xFFFF, n))
        elif ext in (0xA3, 0xAB):
            out.append(('r', hl, 1))
        elif ext in (0xB3, 0xBB):
            n = z.b or 256
            out.append(('r', hl if ext == 0xB3 else (hl - n + 1) & 0xFFFF, n))
        elif ext in (0x67, 0x6F):
            out += [('r', hl, 1), ('w', hl, 1)]
        return out
    if op == 0xCB:
        if ext & 7 == 6:
            out.append(('r', hl, 1))
            if ext < 0x40 or ext >= 0x80:
                out.append(('w', hl, 1))
        return out
    if op in (0xDD, 0xFD):
        index = z.ix if op == 0xDD else z.iy
        if ext == 0xCB:
            addr = (index + signed(z.rd((pc + 2) & 0xFFFF))) & 0xFFFF
            cb = z.rd((pc + 3) & 0xFFFF)
            out.append(('r', addr, 1))
            if cb < 0x40 or cb >= 0x80:
                out.append(('w', addr, 1))
            return out
        if ext in (0x2A, 0x22):
            out.append(('r' if ext == 0x2A else 'w', z.rd16((pc + 2) & 0xFFFF), 2))
            return out
        if ext in HL_READ | HL_WRITE | HL_RMW:
            addr = (index + signed(z.rd((pc + 2) & 0xFFFF))) & 0xFFFF
            if ext in HL_READ | HL_RMW:
                out.append(('r', addr, 1))
            if ext in HL_WRITE | HL_RMW:
                out.append(('w', addr, 1))
        return out
    if op in HL_READ | HL_RMW:
        out.append(('r', hl, 1))
    if op in HL_WRITE | HL_RMW:
        out.append(('w', hl, 1))
    elif op == 0x0A:
        out.append(('r', z.bc, 1))
    elif op == 0x1A:
        out.append(('r', z.de, 1))
    elif op == 0x02:
        out.append(('w', z.bc, 1))
    elif op == 0x12:
        out.append(('w', z.de, 1))
    elif op in (0x3A, 0x2A):
        out.append(('r', z.rd16((pc + 1) & 0xFFFF), 1 if op == 0x3A else 2))
    elif op in (0x32, 0x22):
        out.append(('w', z.rd16((pc + 1) & 0xFFFF), 1 if op == 0x32 else 2))
    return out


class TraceSpan(BaseSpan):
    """The runner's span plus the trace the ledgers are built from."""

    def __init__(self, identity, z):
        super().__init__(identity, z)
        self.z = z
        self.mk2 = z.mapper is not None
        self.stack = []             # shadow call stack: (return pc, label)
        self.passes = []            # finished and open copy / peripheral passes
        self.open_copy = None       # the copy run being extended
        self.open_pio = {}          # (function, port) -> pass
        self.steps = 0
        self.dat_events = []
        self.dat_start = list(z.mapper.entries) if self.mk2 else None
        self.cur = list(self.dat_start) if self.mk2 else None
        self.cur_bank = z.mapper.dat_bank if self.mk2 else 0
        self.stage = None
        self.stage_t = z.tstates
        self.states = collections.Counter()      # (stage, entries 0-7) -> T
        self.state_steps = collections.Counter()
        self.exec_len = {}          # physical address -> instruction length
        self.reads = set()          # physical data bytes read by driver code
        self.writes = set()
        self.driver_sp_low = {}     # driver -> lowest SP in its code
        self.edges = collections.Counter()
        self.pending_call_from = None
        self.last_driver = None
        self.cross = collections.Counter()  # (from, to driver, kind) -> bytes
        self.irq_functions = collections.Counter()
        self.lent = None
        self.working_start = list(z.mapper.entries[8:16]) if self.mk2 else None
        self.dat_instr = 0
        self.api_entries = collections.Counter()

    # -- helpers ---------------------------------------------------------

    def label(self, z):
        who = self.identity.lookup(physical(z, z.pc))
        return who[1] + ':' + who[2] if who[0] != 'unknown' else 'unknown'

    def chain(self, depth=4):
        return [label for _, label in reversed(self.stack[-depth:])]

    def stage_of(self, who, pa, interrupt):
        if interrupt or self.irq_depth:
            return 'interrupt'
        if who[0].startswith('driver:'):
            return 'driver'
        name = who[1] + ' ' + who[2]
        if any(s in name for s in ('page_copy', 'pages_copy', 'pages_move', 'page_side',
                                   'copy_move', 'user_copy', 'bank_copy', 'aux_copy',
                                   'copy_staged', 'domain_switch', 'ipc_common', 'xmem_transfer')):
            return 'copy'
        if self.mk2 and (pa >> 13) in self.identity.cold_pages:
            return 'cold'
        return 'nucleus'

    def close_stage(self, t):
        if self.stage is not None and self.mk2:
            key = (self.stage, tuple(self.cur[:SYSTEM_ENTRIES]), self.cur_bank)
            self.states[key] += t - self.stage_t
        self.stage_t = t

    # -- the step --------------------------------------------------------

    def step(self, z):
        pc, sp0, before = z.pc, z.sp, z.tstates
        pa = physical(z, pc)
        who = self.identity.lookup(pa)
        op, ext = z.rd(pc), z.rd((pc + 1) & 65535)
        interrupt = bool(z.nmi_pending or (z.int_pending and z.iff1 and not z.ei_delay and z.im == 2))
        in_driver = who[0].startswith('driver:')
        stage = self.stage_of(who, pa, interrupt)
        if stage != self.stage:
            self.close_stage(before)
            self.stage = stage
        reads = writes = ()
        if who[2] in ('_api_call', '_api_bulk_map', '_api_bulk_read', '_api_bulk_write') and pa == self.identity.entry(who, pa):
            self.api_entries[who[2]] += 1
            if who[2] == '_api_call' and self.last_driver is not None and not (interrupt or self.irq_depth):
                self.pending_call_from = self.last_driver
        regs = (z.hl, z.de, z.bc)
        if in_driver and not interrupt:
            ilen = self.exec_len.get(pa)
            if ilen is None:
                ilen = self.exec_len[pa] = insn_length(op, ext)
            reads_writes = data_operands(z, pc, op, ext)
            region = next((name for name, lo, hi in self.stacks if lo < sp0 <= hi), 'caller')
            key = who[0] + '|' + region
            if sp0 < self.driver_sp_low.get(key, 65536):
                self.driver_sp_low[key] = sp0
            for kind, addr, length in reads_writes:
                target = self.writes if kind == 'w' else self.reads
                for a in range(addr, addr + length):
                    target.add(physical(z, a & 0xFFFF))
                for other in self.owner_of_data(physical(z, addr & 0xFFFF)):
                    if other != who[0]:
                        self.cross[(who[0], other, kind)] += length
        elif interrupt or self.irq_depth:
            self.irq_functions[':'.join(who)] += 1
        ret_addr = z.rd16(sp0) if (op in RET_OPS or (op == 0xED and ext in (0x4D, 0x45))) else None
        port_io = None
        if op == 0xD3 or op == 0xDB:
            port_io = ('out' if op == 0xD3 else 'in', ext, 1)
        elif op == 0xED and ext & 0xC6 == 0x40:
            port_io = ('out' if ext & 1 else 'in', z.c, 1)
        elif op == 0xED and ext in (0xA2, 0xAA):
            port_io = ('in', z.c, 1)
        elif op == 0xED and ext in (0xA3, 0xAB):
            port_io = ('out', z.c, 1)
        elif op == 0xED and ext in (0xB2, 0xBA):
            port_io = ('in', z.c, z.b or 256)
        elif op == 0xED and ext in (0xB3, 0xBB):
            port_io = ('out', z.c, z.b or 256)
        value_a = z.a
        super().step(z)
        t = z.tstates - before
        self.steps += 1
        mapper_port = port_io is not None and port_io[1] <= 15 and self.mk2
        if self.mk2 and ((op == 0xD3 and ext <= 15) or (op == 0xED and ext & 0xC7 == 0x41 and port_io[1] <= 15)):
            self.dat_instr += 1
        # -- system-DAT writes -------------------------------------------
        if mapper_port and port_io[0] == 'out':
            entries = list(z.mapper.entries)
            self.cur_bank_new = z.mapper.dat_bank
            changed = False
            exact = None
            if op == 0xD3 and ext <= 15 and ext != 0x0F:
                exact = ext
            elif op == 0xED and ext & 0xC7 == 0x41 and z.c <= 15 and z.c != 0x0F:
                exact = z.c
            seen = set()
            for e in range(16):
                if entries[e] != self.cur[e]:
                    self.dat_events.append(dict(step=self.steps, entry=e, old=self.cur[e], page=entries[e],
                                                bank=self.cur_bank, at=before, site=who[1] + ':' + who[2],
                                                chain=self.chain(3), same=False))
                    seen.add(e)
                    changed = True
            if exact is not None and exact not in seen:
                self.dat_events.append(dict(step=self.steps, entry=exact, old=entries[exact], page=entries[exact],
                                            bank=self.cur_bank, at=before, site=who[1] + ':' + who[2],
                                            chain=self.chain(3), same=True))
            if self.cur_bank_new != self.cur_bank:
                self.dat_events.append(dict(step=self.steps, entry='bank', old=self.cur_bank, page=self.cur_bank_new,
                                            bank=self.cur_bank, at=before, site=who[1] + ':' + who[2],
                                            chain=self.chain(3), same=False))
            if self.cur_bank_new != self.cur_bank or changed:
                self.close_stage(z.tstates)
                self.stage_t = z.tstates
            self.cur = entries
            self.cur_bank = self.cur_bank_new
        # -- shadow call stack -------------------------------------------
        if interrupt:
            self.stack.append((pc, 'IRQ'))
        elif op in CALL_OPS and z.sp == ((sp0 - 2) & 0xFFFF) and z.rd16(z.sp) == ((pc + 3) & 0xFFFF):
            self.stack.append(((pc + 3) & 0xFFFF, self.label(z)))
        elif op in RST_OPS and z.sp == ((sp0 - 2) & 0xFFFF) and z.rd16(z.sp) == ((pc + 1) & 0xFFFF):
            self.stack.append(((pc + 1) & 0xFFFF, self.label(z)))
        elif ret_addr is not None and z.pc == ret_addr and z.sp == ((sp0 + 2) & 0xFFFF):
            for i in range(len(self.stack) - 1, max(-1, len(self.stack) - 8), -1):
                if self.stack[i][0] == ret_addr:
                    del self.stack[i:]
                    break
            if self.irq_depth == 0 and self.stack and self.stack[-1][1] == 'IRQ' and False:
                pass
        if who[0].startswith('driver:') and not (interrupt or self.irq_depth):
            if self.pending_call_from is not None and self.pending_call_from != who[0]:
                self.edges[(self.pending_call_from, who[0])] += 1
                self.pending_call_from = None
            self.last_driver = who[0]
        # -- payload passes ------------------------------------------------
        self.note_passes(z, who, op, ext, regs, before, t, port_io, mapper_port, in_driver, pa, interrupt)

    def owner_of_data(self, pa):
        info = self.identity.data_owner(pa)
        return (info,) if info else ()

    def note_passes(self, z, who, op, ext, regs, before, t, port_io, mapper_port, in_driver, pa, interrupt):
        name = ':'.join(who)
        block = op == 0xED and ext in perf.BLOCK_COPY
        if block and not interrupt:
            hl, de, bc = regs
            moved = 1 if ext in (0xA0, 0xA8) else (bc - z.bc) & 65535
            if not moved and bc == 0:
                moved = 65536
            backward = ext in (0xA8, 0xB8)
            src = perf.span_pages(z, hl, moved, backward)
            dst = perf.span_pages(z, de, moved, backward)
            run = self.open_copy
            if run is None or run['function'] != name or self.steps - run['last'] > COPY_GAP:
                run = dict(kind='copy', function=name, pc=z.pc if False else None, start_step=self.steps,
                           t0=before, tstates=0, bytes=0, src=collections.Counter(), dst=collections.Counter(),
                           chain=self.chain(5), last=self.steps, runs=0, driver_owner=who[0])
                run['pa'] = pa
                self.passes.append(run)
                self.open_copy = run
            run['runs'] += 1
            run['bytes'] += moved
            run['last'] = self.steps
            run['t1'] = z.tstates
            run['tstates'] = run['t1'] - run['t0']
            run['src'].update(src)
            run['dst'].update(dst)
            return
        if port_io is not None and not mapper_port and not interrupt:
            direction, port, count = port_io
            group = PORT_GROUP.get(port)
            if group is not None:
                key = (name, port, direction)
                run = self.open_pio.get(key)
                depth = len(self.stack)
                if run is None or self.steps - run['last'] > PIO_GAP or depth < run['depth']:
                    run = dict(kind='pio', function=name, port=port, group=group, direction=direction,
                               start_step=self.steps, t0=before, tstates=0, bytes=0, last=self.steps,
                               depth=depth, chain=self.chain(5), pa=pa, ops=0)
                    self.passes.append(run)
                    self.open_pio[key] = run
                run['bytes'] += count
                run['ops'] += 1
                run['last'] = self.steps
                run['tstates'] = z.tstates - run['t0']

    def finish(self, z):
        self.close_stage(z.tstates)
        result = super().finish(z)
        passes = []
        for p in self.passes:
            q = {k: v for k, v in p.items() if k not in ('src', 'dst', 'last', 'depth', 't0', 't1', 'start_step', 'pa')}
            if p['kind'] == 'copy':
                q['src_pages'] = dict(p['src'])
                q['dst_pages'] = dict(p['dst'])
            q['pa'] = p['pa']
            passes.append(q)
        result['trace'] = dict(passes=passes, steps=self.steps, dat_start=self.dat_start,
                               dat_end=list(z.mapper.entries) if self.mk2 else None,
                               dat_events=self.dat_events,
                               states=[dict(stage=s, entries=list(e), bank=b, tstates=tt)
                                       for (s, e, b), tt in sorted(self.states.items())],
                               exec_len=self.exec_len, reads=self.reads, writes=self.writes,
                               edges={'%s>%s' % k: v for k, v in self.edges.items()},
                               cross={'%s>%s:%s' % k: v for k, v in self.cross.items()},
                               driver_sp_low=self.driver_sp_low,
                               irq_functions=dict(self.irq_functions), api_entries=dict(self.api_entries),
                               working_start=self.working_start, dat_instr=self.dat_instr,
                               dat_bank_end=z.mapper.dat_bank if self.mk2 else None)
        return result


# --------------------------------------------------------------------------
# Static model: listings, driver layout, references
# --------------------------------------------------------------------------

LST_LINE = re.compile(r'^\s*([0-9A-Fa-f]{8})\s((?:[0-9A-Fa-f]{2}\s)*)\s*(\d+)\s(.*)$')
DATA_DIRECTIVES = ('.db', '.dw', '.ascii', '.asciz', '.byte', '.word', '.ds', '.blkb', '.str')


def lst_areas(path):
    """Listing lines by area: [(offset, mnemonic text)] in file order."""
    if not Path(path).exists():
        return None
    areas, area = {}, None
    for line in Path(path).read_text(errors='replace').splitlines():
        match = re.search(r'\.area\s+(\S+)', line)
        if match and not LST_LINE.match(line[:0] or 'x'):
            area = match[1]
            areas.setdefault(area, [])
            continue
        found = LST_LINE.match(line)
        if found and area is not None:
            text = found[4].strip()
            if text:
                areas[area].append((int(found[1], 16), text))
    return areas


def const_ranges(path, area, area_size):
    """(offset, size) of constant data inside a code area, from the
    assembler listing; None when the object has no listing."""
    areas = lst_areas(path)
    if areas is None or area not in areas:
        return None
    lines = areas[area]
    ranges = []
    for index, (offset, text) in enumerate(lines):
        mnemonic = text.split(None, 1)[0] if text else ''
        if mnemonic in DATA_DIRECTIVES:
            end = lines[index + 1][0] if index + 1 < len(lines) else area_size
            if end > offset:
                ranges.append((offset, end - offset))
    # Merge adjacent directive lines.
    merged = []
    for offset, size in ranges:
        if merged and merged[-1][0] + merged[-1][1] == offset:
            merged[-1] = (merged[-1][0], merged[-1][1] + size)
        else:
            merged.append((offset, size))
    return merged


def data_labels(path, area):
    """(offset, name) labels of a data area from the listing."""
    areas = lst_areas(path)
    if not areas or area not in areas:
        return []
    labels = []
    for offset, text in areas[area]:
        match = re.match(r'(\w+)::?$', text)
        if match:
            labels.append((offset, match[1]))
    return labels


CODE_AREAS = ('_CODE', '_BOOT', '_WINDOW', '_COLD1', '_COMMON', '_HOME', '_MAPPED', '_KOS_SDK_CODE')
SKIP_AREAS = ('_CABS', '_DABS', '_GSINIT', '_GSFINAL', '_INITIALIZER', '_HEADER')


def original_area(name):
    """The area a cold-set object's listing names (the link copies rename
    each cold set's areas)."""
    return {'_COLD1': '_WINDOW', '_COLD1_DATA': '_WINDOW_DATA'}.get(name, name)


def area_split(obj, area, listing, listed_area=None):
    """code, const, data and bss bytes of one object area."""
    initialized = sum(len(c.data) for c in obj.chunks if obj.areas[c.area_index].name == area.name)
    if area.name in CODE_AREAS:
        consts = const_ranges(listing, listed_area or area.name, area.size)
        const = sum(size for _, size in consts) if consts else 0
        return dict(code=area.size - const, const=const, data=0, bss=0, const_known=consts is not None)
    if initialized:
        initialized = min(initialized, area.size)
        return dict(code=0, const=0, data=initialized, bss=area.size - initialized, const_known=True)
    return dict(code=0, const=0, data=0, bss=area.size, const_known=True)


class DriverModel:
    """One driver's linked layout: objects, areas, references."""

    def __init__(self, board, identity, plan_row, modules, domain):
        self.board = board
        self.name = plan_row['name'][:-4]
        self.row = plan_row
        self.identity = identity
        self.manifest = modpack.load_manifest(str(modules.parent / 'drivers' / (self.name + '.json')), str(modules))
        self.layout = modpack.build_driver_layout(self.manifest)
        self.symbols = modpack.build_symbol_table(self.manifest)
        self.domain = domain
        self.report = identity.reports[self.name]
        self.objects = []
        for index, obj in enumerate(self.manifest['objects']):
            areas = {}
            listing = Path(obj.path).with_suffix('.lst')
            for area in obj.areas:
                if (index, area.name) not in self.layout['instance_offset'] or not area.size:
                    continue
                segment = self.layout['segments'][self.layout['area_segment'][area.name]]
                role = segment['role']
                base = plan_row.get(role + '_base')
                entry = area_split(obj, area, listing)
                entry.update(size=area.size, role=role,
                             base=None if base is None else base + self.layout['instance_offset'][(index, area.name)])
                areas[area.name] = entry
            self.objects.append(dict(name=Path(obj.path).stem, index=index, areas=areas,
                                     listing=listing.exists()))

    def phys(self, logical):
        if self.board == 'mk2':
            return self.domain[logical >> 13] * 8192 + (logical & 8191)
        return 0x80000 + 6 * 65536 + logical

    def totals(self):
        by_role = {}
        for obj in self.objects:
            for area in obj['areas'].values():
                total = by_role.setdefault(area['role'], collections.Counter())
                for key in ('code', 'const', 'data', 'bss'):
                    total[key] += area[key]
        return {role: dict(counts) for role, counts in by_role.items()}

    def data_objects(self):
        """(logical lo, logical hi, symbol) of every state object: io, bulk,
        mapped and boot data, from listings and global symbols."""
        found = []
        for obj in self.objects:
            rel = self.manifest['objects'][obj['index']]
            listing = Path(rel.path).with_suffix('.lst')
            for name, area in obj['areas'].items():
                if name in CODE_AREAS or area['base'] is None:
                    continue
                labels = data_labels(listing, name) or \
                    [(s.value, s.name) for s in rel.symbols if s.kind == 'def' and s.area is not None
                     and rel.areas[s.area].name == name]
                labels = sorted(set(labels))
                if not labels or labels[0][0] != 0:
                    labels.insert(0, (0, '<%s>' % name))
                for i, (offset, label) in enumerate(labels):
                    end = labels[i + 1][0] if i + 1 < len(labels) else area['size']
                    if end > offset:
                        found.append((area['base'] + offset, area['base'] + end, obj['name'] + ':' + label.lstrip('_'),
                                      area['role']))
        return found

    def references(self):
        """Every relocation, resolved by modpack's own rules: (source logical
        address, target logical address, kind, symbol, resolved)."""
        refs, unresolved = [], []
        layout = self.layout
        for index, obj in enumerate(self.manifest['objects']):
            for chunk in obj.chunks:
                area = obj.areas[chunk.area_index]
                if (index, area.name) not in layout['instance_offset']:
                    continue
                role = layout['segments'][layout['area_segment'][area.name]]['role']
                base = self.row.get(role + '_base')
                if base is None:
                    continue
                for mode, rtp, ref in chunk.relocs:
                    site_offset = layout['instance_offset'][(index, area.name)] + chunk.addr + rtp - modpack.A_BYTES
                    try:
                        target, target_area = modpack.resolve_relocation_area(
                            self.manifest, layout, self.symbols, index, mode, ref)
                    except modpack.ModError as exc:
                        unresolved.append(str(exc))
                        continue
                    addend = chunk.data[rtp - modpack.A_BYTES] | (chunk.data[rtp - modpack.A_BYTES + 1] << 8)
                    segment = layout['area_segment'][target_area]
                    trole = layout['segments'][segment]['role']
                    tbase = self.row.get(trole + '_base')
                    symbol = obj.symbols[ref].name if mode & modpack.R_SYM else '<%s>' % target_area
                    refs.append(dict(site=base + site_offset, site_role=role, site_area=area.name,
                                     target=None if tbase is None else tbase + target + addend,
                                     target_role=trole, target_area=target_area, symbol=symbol,
                                     object=Path(obj.path).stem))
        return refs, unresolved


def extend_identity(identity, board, replay, z):
    """What the trace needs beyond the runner's identity."""
    modules = ROOT / replay.STORE
    domain = identity.domain
    identity.models = {}
    identity.replay_for_ledger = replay
    for row in identity.plan['drivers']:
        model = DriverModel(board, identity, row, modules, domain)
        identity.models[model.name] = model
    identity.data_pages = collections.defaultdict(list)
    for model in identity.models.values():
        for lo, hi, name, role in model.data_objects():
            if role not in ('io', 'bulk'):
                continue
            for logical, end in split_logical(lo, hi):
                pa = model.phys(logical)
                identity.data_pages[pa >> 13].append((pa, pa + (end - logical), 'driver:' + model.name, name, role))
    identity.cold_pages = set()
    if board == 'mk2':
        for addr in (0x6000, 0x8000, 0x16000, 0x18000):
            identity.cold_pages.add(identity.kernel_mapper(addr) >> 13)

    def data_owner(pa):
        for lo, hi, owner, _, _ in identity.data_pages.get(pa >> 13, ()):
            if lo <= pa < hi:
                return owner
        return None
    identity.data_owner = data_owner
    return identity


def split_logical(lo, hi):
    while lo < hi:
        end = min(hi, (lo | 0x1FFF) + 1)
        yield lo, end
        lo = end




# --------------------------------------------------------------------------
# Page roles and pass classification
# --------------------------------------------------------------------------

PAYLOAD_PORTS = ('ioc_bulk', 'vdp_data', 'sound', 'sio_data')
CLASSES = ('pio', 'retained-cache', 'borrow', 'view', 'other')


class Roles:
    """What a physical 8 KiB page is, from the machine state the span began
    in and the linked layout."""

    def __init__(self, board, identity, trace):
        self.board = board
        if board == 'mk2':
            domain = identity.domain
            self.cache = set(domain[0:3])
            self.driver = set(domain[3:7])
            self.cold = set(identity.cold_pages)
            self.caller = set(trace['dat_start'][8:15]) if trace.get('dat_start') else set()
            self.page0 = trace['dat_start'][8] if trace.get('dat_start') else None
        else:
            self.cache = {0x40 + 6 * 8 + i for i in range(3)}
            self.driver = {0x40 + 6 * 8 + i for i in range(3, 8)}

    def of(self, page):
        if self.board == 'mk2':
            if page == 0x7F:
                return 'common'
            if page in (0x40, 0x41, 0x42):
                return 'nucleus'
            if page in self.cold:
                return 'nucleus'
            if page in self.cache:
                return 'cache'
            if page in self.driver:
                return 'driver'
            if page in self.caller:
                return 'caller'
            return 'object'
        bank, sub = ((page - 0x40) >> 3), page & 7
        if bank == 6:
            return 'cache' if sub < 3 else 'driver'
        if bank == 7:
            return 'nucleus'
        if bank == 0 and sub == 7:
            return 'staging'
        return 'caller'

    def dominant(self, pages):
        total = collections.Counter()
        for page, n in pages.items():
            total[self.of(int(page))] += n
        return total.most_common(1)[0][0] if total else 'none'


CONTROL_FUNCTIONS = ('_call', '_fread_span', '_fwrite_span', 'dat_span_', '_ipc_nucleus_call', '_kos_block_copy',
                     '_ioc_io_exchange', '_ioc_driver_command_read', '_iocfat_file_generation', '_console_forward',
                     '_iocfat_ring_fill_frame')


def classify_pass(p, roles, ctx):
    """(class, reason, payload) of one pass. `ctx` says whether a device
    receive already happened in the span."""
    function = p['function']
    chain = ' '.join(p.get('chain', ()))
    if p['kind'] == 'pio':
        payload = p['group'] in PAYLOAD_PORTS and p['bytes'] >= 16
        reason = {'ioc_bulk': 'IOC bulk lane receive/send loop',
                  'ioc_command': 'IOC command-lane frame loop (control, not payload)',
                  'vdp_data': 'V9958 data port output loop',
                  'sound': 'sound chip port writes',
                  'sio_data': 'serial data loop'}[p['group']]
        return 'pio', reason, payload
    src = roles.dominant(p['src_pages'])
    dst = roles.dominant(p['dst_pages'])
    # Kernel staging acts as the caller when the nucleus itself reads a file.
    if src == 'nucleus':
        src = 'caller'
    if dst == 'nucleus':
        dst = 'caller'
    if 'xmem' in function or 'xmem' in chain or 'aux_copy' in function:
        return 'view', 'XMEM xread/xwrite: object bytes moved to or from a caller buffer', True
    if 'pages_zero' in chain:
        return 'other', 'page zero-fill of freshly allocated pages (a fill, not a payload copy)', False
    if any(c in function for c in CONTROL_FUNCTIONS):
        return 'other', 'call frame, reply frame or DAT save area (control, not payload)', False
    if p['bytes'] < 16 and not function.startswith('sdk:'):
        # A short request is still payload when the nucleus mover carries it
        # between the cache or ring and the caller; a path string or a frame
        # field is not.
        mover = any(m in function for m in ('page_copy', 'bank_copy', 'domain_switch_end', 'common_begin'))
        if not mover or 'common' in (src, dst) or any(m in chain for m in ('bank_read', 'bank_write', 'copy_path')):
            return 'other', 'short fixed-size move: frame field, generation or restore bytes (control, not payload)', False
    if src in ('driver', 'common') and dst in ('driver', 'common'):
        return 'other', 'driver-internal frame assembly (control, not payload)', False
    if src == 'cache' and dst in ('caller', 'object', 'staging'):
        if dst == 'staging':
            return 'retained-cache', 'cache block to the common staging block (staged delivery)', True
        return 'retained-cache', 'filesystem-cache block to the caller', True
    if src == 'cache' and dst == 'driver':
        return 'retained-cache', 'cache block to the driver ring (read-ahead fill)', True
    if src == 'driver' and dst == 'cache':
        return 'other', ('cache population: the received block is kept for later hits; '
                         'a policy copy, removable only by receiving into the cache page'), True
    if src == 'driver' and dst in ('caller', 'object', 'staging'):
        if ctx['device_before']:
            return 'borrow', 'ring or staging block to the caller after a device receive in this call', True
        return 'retained-cache', 'ring hit: block retained from an earlier call, to the caller', True
    if src == 'staging' and dst in ('caller', 'object'):
        return 'borrow', 'common staging block to the caller (staged delivery bounce)', True
    if src in ('caller', 'object', 'staging') and dst in ('driver', 'staging'):
        return 'borrow', 'caller payload to driver staging (descriptor IN or write staging)', True
    if src in ('caller', 'object') and dst in ('caller', 'object'):
        if function.startswith('sdk:'):
            return 'other', 'SDK stream buffer to caller buffer copy (per-item memcpy)', True
        return 'borrow', 'nucleus staging to the final process pages (a read into the pages in place removes it)', True
    return 'other', 'UNCLASSIFIED %s>%s' % (src, dst), p['bytes'] >= 16


def logical_of(identity, pa):
    for lo, hi, owner, module, label, logical in identity.by_page.get(pa >> 13, ()):
        if lo <= pa < hi:
            return logical + (pa - lo)
    return None


def classify_row(row, roles, identity=None):
    passes = row['trace']['passes']
    ctx = dict(device_before=False)
    ledger = []
    for p in passes:
        cls, reason, payload = classify_pass(p, roles, ctx)
        if p['kind'] == 'pio' and p['group'] == 'ioc_bulk' and p['direction'] == 'in' and p['bytes'] >= 16:
            ctx['device_before'] = True
        ledger.append(dict(site=p['function'], via=p.get('chain', [])[:3], kind=p['kind'], bytes=p['bytes'],
                           tstates=p['tstates'], cls=cls, reason=reason, payload=payload,
                           src=roles.dominant(p['src_pages']) if p['kind'] == 'copy' else p['group'],
                           dst=roles.dominant(p['dst_pages']) if p['kind'] == 'copy' else 'peripheral',
                           port=p.get('port'), pa=p['pa'],
                           logical=logical_of(identity, p['pa']) if identity else None))
    return ledger


def group_passes(ledger):
    """Collapse repeated passes (per-item copies) into counted groups."""
    groups = collections.OrderedDict()
    for p in ledger:
        key = (p['site'], p['cls'], p['reason'], p['payload'], p['src'], p['dst'], p['port'], p['kind'])
        g = groups.setdefault(key, dict(site=p['site'], cls=p['cls'], reason=p['reason'], payload=p['payload'],
                                        src=p['src'], dst=p['dst'], port=p['port'], kind=p['kind'],
                                        linked_address='%05x' % p['pa'],
                                        logical_address=None if p['logical'] is None else '%04X' % (p['logical'] & 0xFFFF),
                                        via=p['via'], passes=0, bytes=0,
                                        tstates=0))
        g['passes'] += 1
        g['bytes'] += p['bytes']
        g['tstates'] += p['tstates']
    return list(groups.values())


def class_totals(groups):
    out = {c: dict(passes=0, bytes=0, tstates=0) for c in CLASSES}
    payload = {c: dict(passes=0, bytes=0, tstates=0) for c in CLASSES}
    for g in groups:
        for table in ([out, payload] if g['payload'] else [out]):
            table[g['cls']]['passes'] += g['passes']
            table[g['cls']]['bytes'] += g['bytes']
            table[g['cls']]['tstates'] += g['tstates']
    return dict(all=out, payload=payload)


# --------------------------------------------------------------------------
# System-DAT occupancy
# --------------------------------------------------------------------------

def replay_events(start, events):
    entries, bank = list(start), None
    for e in events:
        if e['entry'] == 'bank':
            bank = e['page']
        elif not e.get('same'):
            entries[e['entry']] = e['page']
    return entries, bank


def entry_category(entry, page, identity, roles):
    domain = identity.domain
    if page == 0x7F:
        return 'common'
    if entry in (1, 2) and page in (0x41, 0x42):
        return 'fixed'
    if entry == 0:
        if page == 0x40:
            return 'system-low'
        if page == roles.page0:
            return 'caller-page0'
        if page in roles.cache:
            return 'bulk-map'
        if page in roles.caller:
            return 'caller'
        return 'other'
    if entry in (3, 4, 5, 6):
        if page == domain[entry]:
            return 'domain'
        if page in roles.cold:
            return 'cold'
        if page in roles.caller:
            return 'copy-window:caller'
        if page in roles.driver or page in roles.cache:
            return 'copy-window:driver'
        return 'copy-window:object'
    return 'other'


def occupancy(row, identity, roles):
    """Per stage and system entry: the category of the page held, with the
    T-states spent, from the states the DAT writes traced."""
    trace = row['trace']
    stages = {}
    for state in trace['states']:
        if state['bank'] != 0:
            continue
        stage = stages.setdefault(state['stage'], [collections.Counter() for _ in range(SYSTEM_ENTRIES)])
        for entry, page in enumerate(state['entries']):
            stage[entry][entry_category(entry, page, identity, roles)] += state['tstates']
    return {stage: [dict(table) for table in tables] for stage, tables in stages.items()}


# --------------------------------------------------------------------------
# Working sets
# --------------------------------------------------------------------------

def function_sizes(identity):
    """Linked size of every driver function (all page splits summed)."""
    sizes = collections.Counter()
    for items in identity.by_page.values():
        pass
    seen = set()
    for lo, hi, owner, module, label, logical in identity.ranges:
        if owner.startswith('driver:') and (lo, hi) not in seen:
            seen.add((lo, hi))
            sizes[(owner, module, label)] += hi - lo
    return sizes


def page_entry(pa, identity, trace):
    """The system entry a driver instruction reaches physical `pa` through."""
    page = pa >> 13
    domain = identity.domain
    if domain is None:
        return None
    for entry in (3, 4, 5, 6):
        if domain[entry] == page:
            return entry
    if page in (0x40, 0x41, 0x42):
        return page - 0x40
    if page == 0x7F:
        return 7
    if trace.get('working_start') and page == trace['working_start'][0]:
        return 0
    return None


def working_set(row, identity):
    """Executed code and touched state of driver code, from the trace."""
    trace = row['trace']
    functions = collections.Counter()
    by_entry = collections.defaultdict(lambda: collections.Counter())
    for pa, length in trace['exec_len'].items():
        who = identity.lookup(pa)
        functions[':'.join(who)] += length
        entry = page_entry(pa, identity, trace)
        by_entry[entry]['code'] += length
    state = collections.defaultdict(lambda: collections.Counter())
    for kind, addresses in (('read', trace['reads']), ('write', trace['writes'])):
        for pa in addresses:
            owner = identity.data_owner(pa)
            entry = page_entry(pa, identity, trace)
            if owner:
                for lo, hi, o, name, role in identity.data_pages.get(pa >> 13, ()):
                    if lo <= pa < hi:
                        state[o + ':' + name][kind] += 1
                        break
            by_entry[entry]['state_' + kind] += 1
    return dict(functions=dict(functions),
                executed_code_bytes=sum(trace['exec_len'].values()),
                state_objects={k: dict(v) for k, v in state.items()},
                state_read_bytes=len(trace['reads']), state_write_bytes=len(trace['writes']),
                by_entry={str(k): dict(v) for k, v in by_entry.items()})


def summarize_row(row, identity, board):
    """Replace the raw trace of a row by its ledgers (the raw sets stay in
    `row['raw']` for the path unions)."""
    trace = row['trace']
    roles = Roles(board, identity, trace)
    row['ledger_groups'] = group_passes(classify_row(row, roles, identity))
    row['ledger_copy_bytes'] = sum(p['bytes'] for p in trace['passes'] if p['kind'] == 'copy')
    row['domain'] = list(identity.domain) if identity.domain else None
    row['class_totals'] = class_totals(row['ledger_groups'])
    payload_bytes = sum(g['bytes'] for g in row['ledger_groups'] if g['payload'] and g['kind'] == 'copy')
    row['payload_copy_bytes'] = payload_bytes
    if row.get('length'):
        row['payload_copy_equivalents'] = round(payload_bytes / row['length'], 2)
    row['working_set'] = working_set(row, identity)
    row['edges'] = trace['edges']
    row['cross_driver_data'] = trace['cross']
    row['driver_sp_low'] = trace['driver_sp_low']
    row['irq_functions'] = trace['irq_functions']
    row['api_entries'] = trace['api_entries']
    if board == 'mk2':
        end, bank = replay_events(trace['dat_start'], trace['dat_events'])
        row['occupancy'] = occupancy(row, identity, roles)
        total_states = sum(s['tstates'] for s in trace['states'])
        row['dat_check'] = dict(
            events=len(trace['dat_events']), instructions=trace['dat_instr'],
            runner_dat_writes=row['counts'].get('dat_writes', 0),
            replay_equals_machine=end == trace['dat_end'],
            states_cover_span=total_states <= row['tstates'],
            state_tstates=total_states, span_tstates=row['tstates'])
        row['dat_events'] = [dict(e, chain=e['chain']) for e in trace['dat_events']]
        row['entries_start'] = trace['dat_start'][:8]
        row['entries_end'] = trace['dat_end'][:8]
    row['raw'] = dict(exec_len=trace['exec_len'], reads=trace['reads'], writes=trace['writes'])
    del row['trace']


def path_key(name):
    if name.startswith('workload.'):
        return name
    m = re.match(r'raw\.file\.(\w+?)\.(populating_miss|ring_hit|cache_hit|uncached_stream)\.\d+$', name)
    if m:
        suffix = '.page0' if m[1] == 'page0' and m[2] == 'uncached_stream' else ''
        return 'file.' + m[2] + suffix
    if name.startswith('service.file'):
        return 'file.uncached_stream.irq'
    if name.startswith('raw.xmem.write'):
        return 'xmem.write'
    if name.startswith('raw.xmem.read'):
        return 'xmem.read'
    if name.startswith('raw.descriptor_in.sound32') or name.startswith('sdk.sound'):
        return 'sound.write'
    if name == 'raw.null_ipc':
        return 'ipc.null'
    if name == 'raw.enosys':
        return 'syscall.enosys'
    if name.startswith('sdk.fread'):
        return 'file.' + ('cache_hit' if 'cache_hit' in name else 'populating_miss')
    if name.startswith(('sdk.fwrite', 'sdk.fputs')):
        return 'file.write'
    if name.startswith('sdk.console.buffered'):
        return 'console.write'
    if name.startswith('sdk.console.flush'):
        return 'console.flush'
    return None


# --------------------------------------------------------------------------
# Residency ledger
# --------------------------------------------------------------------------

def irq_handlers():
    """Handler symbols named at irq_claim calls in the driver sources."""
    found = []
    for path in sorted((ROOT / 'drivers').glob('*/*.c')):
        text = path.read_text()
        for match in re.finditer(r'irq_claim\(\s*(\w+)\s*,\s*\(uint16_t\)\s*(\w+)', text):
            found.append(dict(driver=path.parent.name, source=match[1], handler=match[2]))
    return found


def function_graph(model):
    """Function -> referenced functions and state objects, by relocation."""
    identity = model.identity
    refs, unresolved = model.references()
    data = sorted(model.data_objects())
    def data_at(logical):
        for lo, hi, name, role in data:
            if lo <= logical < hi:
                return name, role
        return None
    code_edges = collections.defaultdict(set)
    data_edges = collections.defaultdict(set)
    kinds = collections.Counter()
    external = collections.Counter()
    for r in refs:
        site = identity.lookup(model.phys(r['site']))
        source = site[2] if site[0] != 'unknown' else '<%s>' % r['site_area']
        if r['target'] is None:
            continue
        target_data = data_at(r['target']) if r['target_role'] in ('io', 'bulk', 'mapped') else None
        if target_data:
            data_edges[source].add(target_data[0])
            kinds['%s>%s' % (r['site_role'], target_data[1])] += 1
        else:
            who = identity.lookup(model.phys(r['target'])) if r['target_role'] in ('hot', 'boot') else None
            if who and who[0] != 'unknown':
                code_edges[source].add(who[2])
                kinds['%s>%s' % (r['site_role'], 'code')] += 1
            else:
                kinds['%s>%s' % (r['site_role'], r['target_role'])] += 1
        if r['symbol'].startswith('___sdcc_call') or r['symbol'] in ('_kos_block_copy',):
            external[r['symbol']] += 1
    return dict(code=code_edges, data=data_edges, kinds=kinds, unresolved=unresolved, count=len(refs),
                indirect=dict(external))


def reachable(graph, roots):
    seen, todo = set(), list(roots)
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        todo.extend(graph['code'].get(f, ()))
    state = set()
    for f in seen:
        state |= graph['data'].get(f, set())
    return seen, state


def residency(identity, board, replay, rows, handlers_observed):
    """The per-driver and per-cold-set ledger."""
    report = dict(drivers={}, handlers=irq_handlers())
    sizes = function_sizes(identity)
    defs_by_name = collections.defaultdict(list)
    for name, model in identity.models.items():
        graph = function_graph(model)
        totals = model.totals()
        segments = {s['role']: s for s in model.report['segments']}
        check = {}
        for role, total in totals.items():
            seg = segments[role]
            image = total['code'] + total['const'] + total['data']
            check[role] = dict(image_bytes=image, report_image=seg['image_size'], bss_bytes=total['bss'],
                               report_bss=seg['bss_size'],
                               agrees=image == seg['image_size'] and total['bss'] == seg['bss_size'])
        shared = [o for o in model.objects if o['name'] == 'sdcc_indirect_calls']
        for o in model.objects:
            for sym in model.manifest['objects'][o['index']].symbols:
                if sym.kind == 'def':
                    defs_by_name[sym.name].append(name)
        report['drivers'][name] = dict(
            segments={r: dict(base=s['base'], image_bytes=s['image_size'], bss_bytes=s['bss_size'])
                      for r, s in segments.items()},
            totals=totals, size_check=check,
            objects=[dict(name=o['name'], listing=o['listing'],
                          areas={a: {k: v for k, v in area.items()} for a, area in o['areas'].items()})
                     for o in model.objects],
            shared_runtime_bytes=sum(a['code'] + a['const'] for o in shared for a in o['areas'].values()),
            dependencies=model.row['dependencies'],
            references=dict(count=graph['count'], by_kind=dict(graph['kinds']),
                            unresolved=graph['unresolved'], indirect_call_sites=graph['indirect']),
            function_graph=dict(code={k: sorted(v) for k, v in graph['code'].items()},
                                data={k: sorted(v) for k, v in graph['data'].items()}),
            entries=entry_footprint(model))
    # Names defined by two drivers: the shared runtime object and nothing else.
    shared_names = {n: sorted(set(d)) for n, d in defs_by_name.items() if len(set(d)) > 1 and not n.startswith('.')}
    report['symbols_defined_in_more_than_one_driver'] = shared_names
    report['cross_driver_static_references'] = sum(
        1 for n in defs_by_name.values() if False)
    # IRQ reachability by relocation closure from each claimed handler.
    irq_code, irq_state = set(), set()
    for item in report['handlers']:
        driver = item['driver']
        model = next((m for n, m in identity.models.items() if n.startswith(driver) or driver.startswith(n)), None)
        if model is None:
            continue
        graph = function_graph(model)
        label = '_' + item['handler']
        functions, state = reachable(graph, [label])
        item['driver'] = model.name
        item['reachable_functions'] = sorted(functions)
        item['reachable_code_bytes'] = sum(
            n for (owner, module, fn), n in sizes.items() if owner == 'driver:' + model.name and fn in functions)
        item['reachable_state'] = sorted(state)
        item['reachable_state_bytes'] = sum(
            hi - lo for lo, hi, name_, role in model.data_objects() if name_ in state)
        irq_code |= {(model.name, f) for f in functions}
        irq_state |= {(model.name, x) for x in state}
    report['irq_union'] = dict(
        code_bytes=sum(n for (owner, module, fn), n in sizes.items() if (owner[7:], fn) in irq_code),
        functions=sorted('%s:%s' % k for k in irq_code),
        state_objects=sorted('%s:%s' % k for k in irq_state))
    return report


def entry_footprint(model):
    """Bytes of the driver in each system entry of the domain."""
    out = collections.defaultdict(lambda: collections.Counter())
    for role, key, size_key in (('hot', 'hot_base', 'image_size'), ('io', 'io_base', 'bss_size')):
        segment = next((s for s in model.report['segments'] if s['role'] == role), None)
        base = model.row.get(key)
        if segment is None or base is None:
            continue
        size = segment[size_key]
        for lo, hi in split_logical(base, base + size):
            out[lo >> 13][role] += hi - lo
    return {str(e): dict(v) for e, v in sorted(out.items())}


def kernel_residency(identity, board):
    """Code, constant, data and BSS bytes of each kernel residency set, by
    object, from the linked objects and their listings."""
    sets = ({'cold set 0': ('_WINDOW', '_WINDOW_DATA'), 'cold set 1': ('_COLD1', '_COLD1_DATA'),
             'common': ('_COMMON', '_COMMON_DATA'),
             'fixed region': ('_CODE', '_DATA', '_INITIALIZED', '_INITIALIZER', '_GSINIT', '_GSFINAL', '_HOME')}
            if board == 'mk2' else
            {'cold half': ('_WINDOW', '_WINDOW_DATA'),
             'fixed region': ('_CODE', '_DATA', '_INITIALIZED', '_INITIALIZER', '_GSINIT', '_GSFINAL', '_HOME')})
    result = {name: dict(total=collections.Counter(), objects=[]) for name in sets}
    # The linked inputs: Mk2 links the copies in link/ whose cold-set areas
    # are named per set; Mk1 links the build directory's objects.
    rels = sorted((identity.build / 'link').glob('*.rel')) if board == 'mk2' else sorted(identity.build.glob('*.rel'))
    for rel in rels:
        obj = modpack.parse_rel(rel.read_text(), str(rel))
        if board == 'mk2' and not any(sym.kind == 'def' and sym.name in identity.syms for sym in obj.symbols):
            continue                  # not an input of the linked image
        listing = rel.with_suffix('.lst')
        if not listing.exists():
            listing = identity.build / (rel.stem + '.lst')
        for name, areas in sets.items():
            row = collections.Counter(object=rel.stem)
            for area in obj.areas:
                if area.name in areas and area.size:
                    part = area_split(obj, area, listing, original_area(area.name))
                    for key in ('code', 'const', 'data', 'bss'):
                        row[key] += part[key]
                    row['const_known'] = row.get('const_known', 1) and int(part['const_known'])
            if sum(row[k] for k in ('code', 'const', 'data', 'bss')):
                result[name]['objects'].append(dict(row))
                for key in ('code', 'const', 'data', 'bss'):
                    result[name]['total'][key] += row[key]
    areas = perf.memory_ledger(board, identity.replay_for_ledger)['areas'] if hasattr(identity, 'replay_for_ledger') else {}
    for name, entry in result.items():
        entry['total'] = dict(entry['total'])
        entry['map_area_bytes'] = sum(areas.get(a, {}).get('size', 0) for a in sets[name]) if areas else None
        entry['agrees_with_map'] = ((entry['map_area_bytes'] == sum(entry['total'].values()))
                                    if areas and name != 'fixed region' else None)
        entry['objects'].sort(key=lambda r: -(r['code'] + r['const'] + r['data'] + r['bss']))
        entry['object_count'] = len(entry['objects'])
        entry['objects'] = entry['objects'][:14]
    return result


# --------------------------------------------------------------------------
# Runner glue and the three OS workloads
# --------------------------------------------------------------------------

def merge_rows(name, parts, **meta):
    """One workload row from the spans it consisted of: totals add, trace
    sets union, passes concatenate."""
    row = dict(name=name, tstates=sum(p['tstates'] for p in parts), spans=len(parts), **meta)
    row['buckets'] = {k: sum(p['buckets'][k] for p in parts) for k in parts[0]['buckets']}
    row['counts'] = dict(sum((collections.Counter(p['counts']) for p in parts), collections.Counter()))
    row['stack_low'] = {}
    for p in parts:
        for key, low in p['stack_low'].items():
            row['stack_low'][key] = min(row['stack_low'].get(key, low), low)
    traces = [p['trace'] for p in parts]
    row['trace'] = dict(
        passes=[q for t in traces for q in t['passes']],
        steps=sum(t['steps'] for t in traces),
        dat_start=traces[0]['dat_start'], dat_end=traces[-1]['dat_end'],
        dat_events=[e for t in traces for e in t['dat_events']],
        states=[st for t in traces for st in t['states']],
        exec_len={k: v for t in traces for k, v in t['exec_len'].items()},
        reads=set().union(*(t['reads'] for t in traces)),
        writes=set().union(*(t['writes'] for t in traces)),
        edges=dict(sum((collections.Counter(t['edges']) for t in traces), collections.Counter())),
        cross=dict(sum((collections.Counter(t['cross']) for t in traces), collections.Counter())),
        driver_sp_low={k: min(t['driver_sp_low'].get(k, 65535) for t in traces)
                       for t in traces for k in t['driver_sp_low']},
        irq_functions=dict(sum((collections.Counter(t['irq_functions']) for t in traces), collections.Counter())),
        api_entries=dict(sum((collections.Counter(t['api_entries']) for t in traces), collections.Counter())),
        working_start=traces[0]['working_start'], dat_instr=sum(t['dat_instr'] for t in traces),
        dat_bank_end=traces[-1]['dat_bank_end'])
    row['block_copy_bytes'] = sum(p.get('block_copy_bytes', 0) for p in parts)
    row['split_dat_checks'] = [(len(t['dat_events']), t['dat_instr']) for t in traces]
    return row


class LedgerRunner(perf.Runner):
    def boot(self, sdk=False):
        z = super().boot(sdk)
        extend_identity(self.identity, self.board, self.replay, z)
        return z

    def add(self, name, result, **kw):
        row = super().add(name, result, **kw)
        summarize_row(row, self.identity, self.board)
        row['path'] = path_key(name)
        row['identity_kind'] = 'sdk' if hasattr(self, 'ps') and self.ps else 'raw'
        return row

    def workload_row(self, name, parts, **meta):
        """Register a workload built from several spans, each already traced
        and individually checked."""
        row = merge_rows(name, parts, **meta)
        row['command'] = 'python3 tests/ledger_copy_residency.py --board %s --output FILE' % self.board
        row['interrupt_mode'] = 'masked'
        self.rows.append(row)
        summarize_row(row, self.identity, self.board)
        row['path'] = name
        print('%s workload %s %d T' % (self.board, name, row['tstates']), flush=True)
        return row

    def spans_of(self, calls):
        return [c for c in calls]

    # -- workload 1: file copy with a checksum over OS calls ---------------

    def workload_file_copy(self):
        """SMALL.BIN (8 KiB, uncached) copied to OTHER.BIN in 4096-byte
        chunks through one buffer; the checksum is the application's pass
        over the buffer (no OS copy, not simulated). Spans are the read and
        write calls; open, seek and close are setup."""
        z = self.boot()
        self.small = self.other = self.large = None
        self.fresh_cache()
        src = self.small
        self.close(self.other)
        self.other = None
        dst = self.bind('/fs/OTHER.BIN', 2)
        chunk, buffer = 4096, 0xA000
        parts, checksum = [], 0
        for offset in range(0, 8192, chunk):
            r = self.call(src, 0, chunk, buffer, 1)
            assert r['status'] == chunk and not r['carry'], r
            data = get(z, buffer, chunk)
            assert data == PATTERN[offset:offset + chunk]
            checksum = (checksum + sum(data)) & 0xFFFF
            w = self.call(dst, 1, chunk, buffer, 0)
            assert w['status'] == chunk and not w['carry'], w
            parts += [r, w]
        self.close(src)
        self.close(dst)
        written = bytes(z.hid.fs2_files[('OTHER.BIN',)])
        assert written[:8192] == PATTERN[:8192]
        assert checksum == sum(PATTERN[:8192]) & 0xFFFF
        return self.workload_row('workload.file_copy_checksum', parts, length=8192, buffer='high',
                                 chunk=chunk, checksum=checksum,
                                 boundary='read and write calls of a 2-chunk copy; checksum is application work')

    # -- workload 3: buffered sound through the SDK -----------------------

    def workload_sound(self):
        z = self.boot(sdk=True)
        parts, pairs, ticks = [], 64, 16
        put(z, 0xA000, bytes([4, 0x7F]) * pairs)
        self.var('address', 0xA000)
        self.var('length', pairs)
        before = len(z.sound_peer.writes)
        for _ in range(ticks):
            result = self.sdk_call('sound_write')
            assert result['status'] == pairs
            parts.append(result)
        assert len(z.sound_peer.writes) - before == pairs * ticks
        return self.workload_row('workload.sound_buffered', parts, length=pairs * 2 * ticks, buffer='high',
                                 boundary='16 sound_write calls of 64 pairs, one per tick')

    # -- workload 2: sequential EXE load ----------------------------------

    def workload_exe_load(self):
        """The nucleus loading a card EXE: from the launch of the child
        (path walk, header, every segment streamed through the driver) up to
        the instant the child is entered."""
        r = self.replay
        z = r.make_cpu()
        files = {'SMALL.BIN': PATTERN[:8192], 'LARGE.BIN': PATTERN, 'OTHER.BIN': PATTERN[8192:16384],
                 'PERF.EXE': self.probe.read_bytes()}
        z.hid = z.ioc_peer = perf.IocCommandPeer(fs2_media=True, fs2_files=files)
        z.sound_peer = perf.SoundPeer()
        if self.board == 'mk1':
            perf.Ctc().attach(z)
            r.start_kernel(z)
        r.run_to(z, perf.CARD_PROMPT)
        z.hid.hid_queue.extend(b'/fs/PERF.EXE\r')
        start = r.syms['__cold_process_prepare' if self.board == 'mk2' else '_process_prepare']
        stop = r.syms['_kos_process_launch']
        assert r.drive(z, perf.LIMIT, watch={start}, hook=lambda _: True) is not None
        self.z = z
        self.identity = perf.Identity(self.board, r, z, None)
        extend_identity(self.identity, self.board, r, z)
        span = TraceSpan(self.identity, z)
        for _ in range(perf.LIMIT):
            if z.pc == stop and (z.mapper is None or z.mapper.dat_bank == 0):
                break
            span.step(z)
        else:
            raise AssertionError('exe load span limit')
        result = span.finish(z)
        result['peer_commands'] = {}
        size = len(files['PERF.EXE'])
        row = dict(name='workload.exe_load_sequential', **result, length=size, buffer='process pages',
                   interrupt_mode='enabled', spans=1, command='python3 tests/ledger_copy_residency.py --board %s --output FILE' % self.board,
                   boundary='process_prepare entry (path walk, header, segments) through kos_process_launch entry')
        self.rows.append(row)
        summarize_row(row, self.identity, self.board)
        row['path'] = 'workload.exe_load_sequential'
        print('%s workload %s %d T' % (self.board, row['name'], row['tstates']), flush=True)
        return row



# --------------------------------------------------------------------------
# Path unions, packing evaluation, findings
# --------------------------------------------------------------------------

def union_path(rows, identity, sizes):
    """The working set of one IPC path over all its rows."""
    exec_len, reads, writes = {}, set(), set()
    for row in rows:
        exec_len.update(row['raw']['exec_len'])
        reads |= row['raw']['reads']
        writes |= row['raw']['writes']
    functions = collections.Counter()
    drivers = collections.defaultdict(lambda: dict(executed_bytes=0, functions=set()))
    for pa, length in exec_len.items():
        who = identity.lookup(pa)
        functions[who] += length
        if who[0].startswith('driver:'):
            drivers[who[0]]['executed_bytes'] += length
            drivers[who[0]]['functions'].add((who[1], who[2]))
    out = dict(rows=len(rows), drivers={}, entries={})
    for owner, d in sorted(drivers.items()):
        full = sum(sizes[(owner, m, f)] for m, f in d['functions'])
        out['drivers'][owner] = dict(executed_instruction_bytes=d['executed_bytes'], function_count=len(d['functions']),
                                     whole_function_bytes=full)
    state = collections.defaultdict(lambda: collections.Counter())
    for kind, addresses in (('read', reads), ('write', writes)):
        for pa in addresses:
            hit = None
            for lo, hi, owner, name, role in identity.data_pages.get(pa >> 13, ()):
                if lo <= pa < hi:
                    hit = owner
                    break
            if hit:
                state[hit][kind] += 1
    out['state_bytes'] = {k: dict(v) for k, v in sorted(state.items())}
    per_entry = collections.defaultdict(lambda: collections.Counter())
    for pa, length in exec_len.items():
        e = page_entry(pa, identity, {})
        if e is not None:
            per_entry[e]['code'] += length
    for kind, addresses in (('state_read', reads), ('state_write', writes)):
        for pa in addresses:
            e = page_entry(pa, identity, {})
            if e is not None:
                per_entry[e][kind] += 1
    out['entries'] = {str(e): dict(v) for e, v in sorted(per_entry.items())}
    out['domain_entries_touched'] = sorted(e for e in per_entry if e in (3, 4, 5, 6))
    out['_functions'] = sorted(functions)
    return out


def packing(identity, path, tier_state_bytes):
    """One candidate: all io state on one always-mapped page; the code the
    path executes (whole functions, shared runtime included) in contiguous
    groups of entries."""
    page = 8192
    drivers = path['drivers']
    code = sum(d['whole_function_bytes'] for d in drivers.values())
    exact = sum(d['executed_instruction_bytes'] for d in drivers.values())
    whole = sum(identity.models[o[len('driver:'):]].totals().get('hot', {}).get('code', 0) +
                identity.models[o[len('driver:'):]].totals().get('hot', {}).get('const', 0)
                for o in drivers)
    def pages(n):
        return -(-n // page)
    return dict(io_page_bytes=tier_state_bytes, io_page_slack=page - tier_state_bytes,
                executed_function_code_bytes=code, executed_instruction_bytes=exact,
                whole_driver_code_bytes=whole,
                code_entries_executed_functions=pages(code), code_entries_whole_drivers=pages(whole),
                entries_needed_executed_functions=1 + pages(code), entries_needed_whole_drivers=1 + pages(whole),
                borrow_entry_free_executed_functions=1 + pages(code) <= 3,
                borrow_entry_free_whole_drivers=1 + pages(whole) <= 3)


# --------------------------------------------------------------------------
# Assembly of the output
# --------------------------------------------------------------------------

CANONICAL = ('raw.file.high.ring_hit.512', 'raw.file.high.cache_hit.512', 'raw.file.high.cache_hit.4096',
             'raw.file.high.populating_miss.512', 'raw.file.high.populating_miss.4096',
             'raw.file.high.uncached_stream.512', 'raw.file.high.uncached_stream.4096',
             'raw.file.page0.uncached_stream.512', 'raw.file.page0.uncached_stream.4096',
             'raw.xmem.write.512.high', 'raw.xmem.read.4096.high', 'raw.descriptor_in.sound32.high',
             'sdk.fread.item512.cache_hit', 'sdk.fwrite.item512', 'sdk.console.buffered.128', 'sdk.sound.128',
             'workload.file_copy_checksum', 'workload.exe_load_sequential', 'workload.sound_buffered')


def compact_row(row):
    keep = ('name', 'tstates', 'length', 'buffer', 'address', 'alignment', 'interrupt_mode', 'observed_state',
            'status', 'spans', 'path', 'boundary', 'payload_copy_bytes', 'payload_copy_equivalents',
            'ledger_groups', 'class_totals', 'working_set', 'edges', 'cross_driver_data', 'driver_sp_low',
            'irq_functions', 'api_entries', 'occupancy', 'dat_check', 'entries_start', 'entries_end', 'buckets', 'counts',
            'block_copy_bytes', 'ledger_copy_bytes', 'command')
    out = {k: row[k] for k in keep if k in row}
    if 'dat_events' in row:
        out['dat_events'] = [[e['step'], e['entry'], e['old'], e['page'], e['bank'], e['site'], bool(e['same'])]
                             for e in row['dat_events']]
    return out


def avoidable(row):
    totals = row['class_totals']['payload']
    return {c: totals[c] for c in CLASSES}


def collect(board, only=None):
    # The runner's spans are traced for the duration of the collection
    # only, so importing this module leaves perf_os_sdk as it is.
    base, perf.Span = perf.Span, TraceSpan
    try:
        runner = LedgerRunner(board, True)
        runner.sdk_rows()
        runner.raw_rows()
        for workload in ('workload_file_copy', 'workload_exe_load', 'workload_sound'):
            getattr(runner, workload)()
    finally:
        perf.Span = base
    return runner


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def build_output(board, runner, perf_json=None):
    identity = runner.identity
    rows = runner.rows
    sizes = function_sizes(identity)
    domains = {tuple(r['domain']) for r in rows if r.get('domain')}
    assert len(domains) <= 1, domains
    # Every block-moved byte is inside a classified pass.
    for row in rows:
        if 'block_copy_bytes' in row:
            assert row['block_copy_bytes'] == row['ledger_copy_bytes'], (row['name'], row['block_copy_bytes'], row['ledger_copy_bytes'])
        unclassified = [g for g in row['ledger_groups'] if g['reason'].startswith('UNCLASSIFIED')]
        assert not unclassified, (row['name'], unclassified)
    if perf_json:
        table = {r['name']: r for r in json.loads(Path(perf_json).read_text())['rows']}
        compared = mismatched = 0
        for row in rows:
            ref = table.get(row['name'])
            if ref:
                compared += 1
                if (ref['tstates'], ref['block_copy_bytes']) != (row['tstates'], row['block_copy_bytes']):
                    mismatched += 1
                    print('MISMATCH', row['name'], ref['tstates'], row['tstates'])
        perf_check = dict(file=str(perf_json), compared=compared, mismatched=mismatched)
    else:
        perf_check = None
    by_path = collections.defaultdict(list)
    for row in rows:
        if row.get('path'):
            by_path[row['path']].append(row)
    tier = sum(m.totals().get('io', {}).get('bss', 0) for m in identity.models.values())
    paths = {}
    for key, members in sorted(by_path.items()):
        entry = union_path(members, identity, sizes) if board == 'mk2' else None
        if entry is None:
            continue
        entry.pop('_functions')
        entry['row_names'] = [r['name'] for r in members][:40]
        entry['row_count'] = len(members)
        if entry['drivers']:
            entry['packing'] = packing(identity, entry, tier)
        paths[key] = entry
    resid = residency(identity, board, runner.replay, rows, None)
    resid['io_state_bytes_all_drivers'] = tier
    kernel = kernel_residency(identity, board)
    stacks = {}
    for row in rows:
        for name, low in row['stack_low'].items():
            stacks[name] = min(stacks.get(name, low), low)
    memory = perf.memory_ledger(board, runner.replay)
    for e in memory['stacks']:
        low = stacks.get(e['name'])
        e['observed_low'] = low
        e['observed_used'] = None if low is None else e['top'] - low
        e['margin'] = None if low is None else low - e['base']
    driver_sp = {}
    for row in rows:
        for owner, low in row['driver_sp_low'].items():
            driver_sp[owner] = min(driver_sp.get(owner, low), low)
    tops = {e['name']: e['top'] for e in memory['stacks']}
    for name, model in identity.models.items():
        alt = model.report['symbols'].get('_%s_alt_stack_top' % name)
        if alt is not None:
            tops['caller'] = model.row['io_base'] + alt
    resid['driver_stack_high_water'] = {
        key.replace('|caller', '|private stack in the io block'): dict(
            lowest_sp=low, used_bytes=(tops[key.split('|')[1]] - low) if key.split('|')[1] in tops else None)
        for key, low in sorted(driver_sp.items())}
    edges = collections.Counter()
    cross = collections.Counter()
    bulk_users = collections.Counter()
    irq_seen = collections.Counter()
    for row in rows:
        for k, v in row['edges'].items():
            edges[k] += v
        for k, v in row['cross_driver_data'].items():
            cross[k] += v
        for k, v in row.get('irq_functions', {}).items():
            irq_seen[k] += v
        for ev in row.get('dat_events', ()):
            if ev['entry'] == 0 and not ev['same'] and ev['page'] in identity.domain[0:3]:
                bulk_users[ev['site']] += 1
    resid['observed_call_edges'] = dict(edges)
    resid['observed_cross_driver_data_bytes'] = dict(cross)
    resid['bulk_map_entry0_writers'] = dict(bulk_users)
    resid['irq_context_functions_observed'] = dict(irq_seen.most_common(40))
    resid['api_bulk_map_entries'] = sum(r['api_entries'].get('_api_bulk_map', 0) for r in rows)
    resid['api_bulk_entries'] = {k: sum(r['counts'].get(k, 0) for r in rows) for k in
                                 ('entry:_api_bulk_read', 'entry:_api_bulk_write', 'entry:_api_bulk_copy_out',
                                  'entry:_api_borrow', 'entry:_api_borrowed')}
    out = dict(format='kos-ledger/1', board=board, perf_check=perf_check,
               identity=dict(revision=perf.command('git', 'rev-parse', 'HEAD'),
                             dirty_status=perf.command('git', 'status', '--short'),
                             image_sha256=sha(ROOT / ('build-mk2/konaos-mk2.bin' if board == 'mk2' else 'build/konaos.bin')),
                             sdk_sha256=sha(next(runner.build.glob('kos-sdk-*.tar.gz'))),
                             probe_sha256=sha(runner.probe),
                             peer='IocCommandPeer FS2, read_run false; Ctc; SoundPeer; V9958Peer (the runner\'s configuration)'),
               rows=[compact_row(r) for r in rows],
               paths=paths, residency=resid, kernel_residency=kernel, memory_ledger=memory,
               tier_io_state_bytes=tier)
    return out


def coresidency(out):
    """Co-residency groups the observed and declared reference graph implies."""
    resid = out['residency']
    names = sorted(resid['drivers'])
    declared = {}
    for name in names:
        deps = resid['drivers'][name]['dependencies']
        declared[name] = deps
    # Dependency endpoints are class/instance/number; map them to drivers by plan.
    ep = {}
    plan = json.loads((ROOT / 'build-mk2/sys/modules/drivers.plan.json').read_text())
    for d in plan['drivers']:
        for e in d['endpoints']:
            ep[(e['class'], e['instance'])] = d['name'][:-4]
    calls = collections.Counter()
    for name, deps in declared.items():
        for dep in deps:
            target = ep.get((dep['class'], dep['instance']))
            if target:
                calls[(name, target)] += 0
    for key, n in resid['observed_call_edges'].items():
        a, b = key.split('>')
        calls[(a.replace('driver:', ''), b.replace('driver:', ''))] += n
    data = collections.Counter()
    for key, n in resid['observed_cross_driver_data_bytes'].items():
        a, rest = key.split('>', 1)
        b, kind = rest.rsplit(':', 1)
        data[(a.replace('driver:', ''), b.replace('driver:', ''))] += n
    tight = {frozenset(k) for k in data if (k[1], k[0]) in data}
    groups = []
    for pair in tight:
        groups.append(sorted(pair))
    sizes = {n: resid['drivers'][n]['totals'].get('hot', {}) for n in names}
    def code(n):
        return sizes[n].get('code', 0) + sizes[n].get('const', 0)
    return dict(
        declared_calls=sorted('%s>%s' % k for k in calls if calls[k] == 0 or True),
        observed_call_edges={'%s>%s' % k: v for k, v in calls.items() if v},
        observed_data_pointer_edges={'%s>%s' % k: v for k, v in data.items()},
        tightly_coupled=groups,
        group_code_bytes={'+'.join(g): sum(code(n) for n in g) for g in groups},
        driver_code_bytes={n: code(n) for n in names})


FIXED_ENTRIES = {
    0: 'caller page 0 alias during a syscall (call block and descriptors; the 144 lent-read target); bulk-map span target',
    1: 'fixed nucleus (2000h-3FFFh): transition, IPC and syscall code; cannot be displaced',
    2: 'fixed nucleus (4000h-5FFFh): driver stack at 5D00h, IRQ stack at 5F00h; cannot be displaced',
    7: 'common page (E000h-FFFFh): IM2 and NMI entry, gate stack, RT region; cannot be displaced',
}


def displacement(out):
    """What a borrow window at each system entry would displace: the linked
    contents of the entry, and what each path touches in it."""
    resid = out['residency']
    result = {}
    for entry in range(8):
        if entry in FIXED_ENTRIES:
            result[str(entry)] = dict(holds=FIXED_ENTRIES[entry], displaceable=entry == 0 and 'only for a page-0 borrow',
                                      contents=[])
            continue
        contents = []
        for name, d in resid['drivers'].items():
            part = d['entries'].get(str(entry))
            if part:
                contents.append(dict(driver=name, code_bytes=part.get('hot', 0), state_bytes=part.get('io', 0)))
        touched = {}
        for key, p in out['paths'].items():
            e = p['entries'].get(str(entry))
            if e:
                touched[key] = e
        result[str(entry)] = dict(holds='driver domain page %d' % entry, contents=contents,
                                  touched_by_path=touched,
                                  untouched_by_paths=sorted(k for k, p in out['paths'].items()
                                                            if p['drivers'] and str(entry) not in p['entries']))
    return result


def findings(out):
    """Avoidable copy cost ranked by class over the canonical rows."""
    rows = {r['name']: r for r in out['rows']}
    total = {c: 0 for c in CLASSES}
    per_row = {}
    for name in CANONICAL:
        row = rows.get(name)
        if row is None:
            continue
        payload = row['class_totals']['payload']
        per_row[name] = dict(tstates=row['tstates'], by_class={c: payload[c]['tstates'] for c in CLASSES},
                             bytes_by_class={c: payload[c]['bytes'] for c in CLASSES})
        for c in CLASSES:
            total[c] += payload[c]['tstates']
    ranking = sorted(((t, c) for c, t in total.items()), reverse=True)
    return dict(canonical_rows=per_row, class_tstates_over_canonical_rows=total,
                ranking=[dict(cls=c, tstates=t) for t, c in ranking])


def md_table(header, rows):
    lines = ['| ' + ' | '.join(header) + ' |', '|' + '|'.join('---' for _ in header) + '|']
    lines += ['| ' + ' | '.join(str(c) for c in row) + ' |' for row in rows]
    return '\n'.join(lines)


def tables(out):
    """Markdown tables of the JSON, so a report quotes them exactly."""
    board = out['board']
    rows = {r['name']: r for r in out['rows']}
    text = ['## Tables (%s)\n' % board]
    body = []
    for name in CANONICAL:
        r = rows.get(name)
        if not r:
            continue
        pay = r['class_totals']['payload']
        body.append([name, r['tstates'], r.get('length', ''), r['payload_copy_bytes'],
                     r.get('payload_copy_equivalents', ''),
                     *['%d/%d B/%d T' % (pay[c]['passes'], pay[c]['bytes'], pay[c]['tstates']) for c in CLASSES]])
    text += ['### Payload passes per class: passes / bytes / T-states\n',
             md_table(['row', 'T', 'request B', 'payload copy B', 'x request'] + list(CLASSES), body), '']
    for name in CANONICAL:
        r = rows.get(name)
        if not r:
            continue
        lines = [[g['cls'], g['passes'], g['bytes'], g['tstates'], g['site'].split(':', 1)[-1], g['src'] + '>' + g['dst'],
                  (g['logical_address'] or '-') + 'h', g['reason']] for g in r['ledger_groups'] if g['payload']]
        text += ['#### %s (%d T)\n' % (name, r['tstates']),
                 md_table(['class', 'passes', 'bytes', 'T', 'linked site', 'src>dst', 'link addr', 'reason'], lines), '']
    if board == 'mk2':
        text += ['### System-DAT occupancy by stage (page category held at each entry, by T-states)\n']
        for stage in ('driver', 'copy', 'cold', 'nucleus'):
            body = []
            for name in CANONICAL:
                r = rows.get(name)
                if not r or stage not in r.get('occupancy', {}):
                    continue
                cells = []
                for table in r['occupancy'][stage]:
                    total = sum(table.values()) or 1
                    ranked = sorted(table.items(), key=lambda kv: -kv[1])
                    cells.append('/'.join(c for c, t in ranked if t * 20 >= total))
                body.append([name] + cells)
            text += ['#### stage: %s\n' % stage, md_table(['row'] + ['e%d' % i for i in range(8)], body), '']
        text += ['### Working sets and candidate packing per path\n']
        body = []
        for key, p in out['paths'].items():
            if not p['drivers']:
                continue
            pk = p['packing']
            body.append([key, p['row_count'],
                         ', '.join('%s %d/%d' % (o[7:], d['executed_instruction_bytes'], d['whole_function_bytes'])
                                   for o, d in p['drivers'].items()),
                         sum(v.get('read', 0) + v.get('write', 0) for v in p['state_bytes'].values()),
                         ','.join(map(str, p['domain_entries_touched'])),
                         pk['executed_function_code_bytes'], pk['whole_driver_code_bytes'],
                         pk['entries_needed_executed_functions'], pk['entries_needed_whole_drivers'],
                         'yes' if pk['borrow_entry_free_whole_drivers'] else 'no'])
        text += [md_table(['path', 'rows', 'driver: executed B / whole-function B', 'state B touched',
                           'entries touched now', 'code B (functions)', 'code B (whole drivers)',
                           'entries (functions)', 'entries (whole drivers)', 'entry free (whole drivers)'], body), '']
    if board == 'mk2':
        text += ['### Entry displacement (current layout)\n']
        body = []
        for entry, d in sorted(out['displacement'].items()):
            body.append([entry, d['holds'][:70],
                         '; '.join('%s %dB code %dB state' % (c['driver'], c['code_bytes'], c['state_bytes'])
                                   for c in d['contents']),
                         ', '.join(d.get('untouched_by_paths', []))])
        text += [md_table(['entry', 'holds', 'linked contents', 'driver paths that never touch it'], body), '']
    return '\n'.join(text) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--board', required=True, choices=('mk1', 'mk2'))
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--tables', type=Path, help='also write the markdown tables of the JSON')
    parser.add_argument('--perf', type=Path, help='a perf_os_sdk.py JSON of the same tree to cross-check')
    args = parser.parse_args()
    started = time.time()
    runner = collect(args.board)
    out = build_output(args.board, runner, args.perf)
    out['coresidency'] = coresidency(out) if args.board == 'mk2' else None
    out['findings'] = findings(out)
    if args.board == 'mk2':
        out['displacement'] = displacement(out)
    out['wall_seconds'] = round(time.time() - started, 1)
    if args.tables:
        args.tables.write_text(tables(out))
    args.output.write_text(json.dumps(out, indent=1, sort_keys=True, default=list) + '\n')
    print('wrote %s (%d rows)' % (args.output, len(out['rows'])))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
