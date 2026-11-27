#!/usr/bin/env python3
"""OS/SDK instruction baseline on production bytes and existing simulator peers.

Prerequisites: make [BOARD=mk2], make sdk [BOARD=mk2],
make perf-probe [BOARD=mk2]. Output is versioned JSON; timing never asserts a
historical constant. The probes are card EXEs, never part of a shipped image.
"""
import argparse
import collections
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import time
import warnings

# tools/modpack.py leaves its object files to the garbage collector.
warnings.filterwarnings('ignore', category=ResourceWarning, module='modpack')

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path[:0] = [str(HERE), str(ROOT / 'tools')]
import modpack
from test_mk2_shell import (Mk1ShellReplay, Mk2ShellReplay, CARD_PROMPT)
from z80sim import Ctc, IocCommandPeer, SoundPeer, StopSet, read_symbols

BUCKETS = ('sdk', 'entry_return', 'object_handle', 'ipc', 'mapping',
           'driver', 'copy_setup', 'byte_movement', 'interrupt', 'unknown')
BLOCK_COPY = (0xA0, 0xA8, 0xB0, 0xB8)
LIMIT = 40_000_000
# Functions whose entries are counted (calls, not instructions).
ENTRY_POINTS = ('_ipc_call_endpoint', '_api_call', '_ipc_syscall_call_object', '_iocfat_ring_fill',
                '_iocfat_transfer', '_iocfat_file_read', '_iocfat_file_write', '_api_bulk_read',
                '_api_bulk_write', '_api_bulk_copy_out', '_kos_page_copy', '_kos_dat_span_enter',
                '_kos_dat_span_restore', '_api_borrow', '_api_borrowed')
CODE, BLOCK, TEXT, RETURN = 0x1C00, 0x1D00, 0x1D80, 0xD100
PATTERN = bytes((i * 37 + (i >> 8)) & 255 for i in range(65536))


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def command(*argv):
    return subprocess.check_output(argv, cwd=ROOT, text=True).strip()


def physical(z, address):
    """Read identity under the mapping in force, before the instruction."""
    if z.mapper is not None:
        return z.mapper.physical(address)
    return 0x80000 + z.bank_of(address) * 65536 + address


def split_physical(mapper, address, size):
    """Split linked ranges at physical page boundaries."""
    while size:
        n = min(size, 0x2000 - (address & 0x1FFF))
        yield mapper(address), mapper(address) + n, address, n
        address += n
        size -= n


def lst_functions(path):
    """C static/global entry offsets, with their actual assembler area."""
    if not path.exists():
        return []
    area, pending, result = None, None, []
    for line in path.read_text(errors='replace').splitlines():
        match = re.search(r'\.area\s+(\S+)', line)
        if match:
            area = match[1]
        match = re.search(r';\s*Function\s+(\w+)', line)
        if match:
            pending = '_' + match[1]
        match = re.match(r'\s*([0-9A-Fa-f]{8})\s+\d+\s+(\w+)::?', line)
        if match and pending and match[2] == pending:
            result.append((area, int(match[1], 16), pending))
            pending = None
    return result


class Identity:
    """Exact object extents, static entries, and physical page identity.

    A .rel global's linked map value minus its object offset establishes
    that object's area base. Every other linked global in the same area
    must agree. Static function entries come from that object's listing.
    Driver offsets use the production packer's own layout algorithm.
    """
    def __init__(self, board, replay, z, program=None):
        self.board = board
        self.ranges = []
        self.symbols = {}
        self.checked_globals = 0
        self.by_page = collections.defaultdict(list)
        build = ROOT / ('build-mk2' if board == 'mk2' else 'build')
        self.build = build
        self.syms = replay.syms
        self.kernel_map = build / ('konaos-mk2.map' if board == 'mk2' else 'konaos.map')
        for rel in sorted(build.glob('*.rel')):
            self.add_link_object(rel, self.syms, self.kernel_mapper)
        if board == 'mk2':
            self.add_link_object(build / 'link/cold_veneers.rel', self.syms,
                                 self.kernel_mapper)
        modules = ROOT / replay.STORE
        domain = self.kernel_bytes(z, '_kos_driver_domain', 8) if board == 'mk2' else None
        self.domain = list(domain) if domain else None
        def driver_mapper(addr):
            if board == 'mk2':
                return domain[addr >> 13] * 8192 + (addr & 8191)
            return 0x80000 + 6 * 65536 + addr
        plan = json.loads((modules / 'drivers.plan.json').read_text())
        self.plan = plan
        self.reports = {}
        for row in plan['drivers']:
            name = row['name'][:-4]
            report = json.loads((modules / (name + '.report.json')).read_text())
            self.reports[name] = report
            manifest = modpack.load_manifest(str(modules.parent / 'drivers' / (name + '.json')), str(modules))
            layout = modpack.build_driver_layout(manifest)
            for i, obj in enumerate(manifest['objects']):
                bases = {}
                for area in obj.areas:
                    if area.name not in layout['area_segment']:
                        continue
                    seg = layout['area_segment'][area.name]
                    role = layout['segments'][seg]['role']
                    base = row.get(role + '_base')
                    if base is not None:
                        bases[area.name] = base + layout['instance_offset'][(i, area.name)]
                self.add_object(obj, bases, driver_mapper, 'driver:' + name,
                                Path(obj.path).with_suffix('.lst'))
            for symbol, addr in report['symbols'].items():
                if report['symbol_segments'].get(symbol) == 'hot':
                    self.symbols['driver:' + name + ':' + symbol] = driver_mapper(addr)
        if program:
            pmap = read_symbols(program / (program.name + '.map'))
            self.program_syms = pmap
            # Probe and SDK objects are in the ordinary home image.
            home = list(z.mapper.entries[8:16]) if board == 'mk2' else z.latch & 7
            def app_mapper(addr):
                if board == 'mk2':
                    return home[addr >> 13] * 8192 + (addr & 8191)
                return 0x80000 + home * 65536 + addr
            # Link globals still identify library members. The SDK .rel
            # members are the same objects used by the EXE's archive.
            map_text = (program / (program.name + '.map')).read_text()
            linked = []
            for line in map_text.splitlines():
                # A long object name puts its module list on the next line.
                m = re.match(r'(\S+\.rel)(\s+\[|\s*$)', line)
                lib = re.match(r'(\S+\.lib)\s+\[\s+(\S+\.rel)\s+\]', line)
                if m:
                    linked.append(ROOT / m[1])
                elif lib:
                    root = (ROOT / lib[1]).parent
                    candidates = list(root.rglob(lib[2]))
                    candidates = [p for p in candidates if ('mk2' in p.relative_to(root).parts) == (board == 'mk2' and lib[2] in ('xmemgate.rel', 'vcall.rel'))]
                    assert len(candidates) == 1, (lib[2], candidates)
                    linked.append(candidates[0])
            assert linked
            for rel in linked:
                self.add_link_object(rel, pmap, app_mapper,
                                     'probe' if rel.parent == program else 'sdk')
        for item in self.ranges:
            for page in range(item[0] >> 13, ((item[1] - 1) >> 13) + 1):
                self.by_page[page].append(item)
        self.cache = {}
        self.entries = {}

    def kernel_mapper(self, addr):
        if self.board == 'mk1':
            return 0x80000 + (0 if (addr & 65535) >= 0xE000 else 7) * 65536 + (addr & 65535)
        if addr >= 0x10000:
            page = 0x43 + 2 * (addr >> 16) + (((addr & 65535) - 0x6000) >> 13)
        elif addr >= 0xE000:
            page = 0x7F
        else:
            page = 0x40 + (addr >> 13)
        return page * 8192 + (addr & 8191)

    def kernel_bytes(self, z, symbol, size):
        address = self.kernel_mapper(self.syms[symbol])
        if self.board == 'mk2':
            return bytes(z.phys[address:address + size])
        bank, offset = (address - 0x80000) >> 16, address & 65535
        return bytes(z._sram(bank)[offset:offset + size])

    def add_link_object(self, rel, syms, mapper, owner='kernel'):
        obj = modpack.parse_rel(rel.read_text(), str(rel))
        bases = {}
        for sym in obj.symbols:
            if sym.kind != 'def' or sym.area is None or sym.name not in syms or sym.name.startswith('.'):
                continue
            area = obj.areas[sym.area].name
            base = syms[sym.name] - sym.value
            if area in bases and bases[area] != base:
                raise AssertionError((rel, area, sym.name, base, bases[area]))
            bases[area] = base
            self.checked_globals += 1
        self.add_object(obj, bases, mapper, owner, rel.with_suffix('.lst'))

    def add_object(self, obj, bases, mapper, owner, listing):
        functions = lst_functions(listing)
        for area in obj.areas:
            if area.name not in bases or not area.size:
                continue
            # Initialized data and BSS never become code identities.
            if area.name not in ('_CODE', '_WINDOW', '_COLD1', '_COMMON', '_HOME', '_KOS_SDK_CODE'):
                continue
            base = bases[area.name]
            labels = [(sym.value, sym.name) for sym in obj.symbols
                      if sym.kind == 'def' and sym.area == area.index and not sym.name.startswith('.')]
            labels += [(offset, name) for where, offset, name in functions if where == area.name]
            labels = sorted(set(labels))
            if not labels or labels[0][0] != 0:
                labels.insert(0, (0, '<area>'))
            for index, (offset, label) in enumerate(labels):
                end = labels[index + 1][0] if index + 1 < len(labels) else area.size
                if end <= offset:
                    continue
                for lo, hi, logical, length in split_physical(mapper, base + offset, end - offset):
                    self.ranges.append((lo, hi, owner, Path(obj.path).stem, label, logical))

    def entry(self, who, pa):
        """The lowest physical address of `who` in `pa`'s page."""
        key = (pa >> 13, who)
        if key not in self.entries:
            self.entries[key] = min(r[0] for r in self.by_page[pa >> 13] if r[2:5] == who)
        return self.entries[key]

    def lookup(self, pa):
        if pa not in self.cache:
            matches = [r for r in self.by_page.get(pa >> 13, ()) if r[0] <= pa < r[1]]
            # Duplicate archive members with identical linked identities
            # are acceptable only when their labels agree.
            identities = {(r[2], r[3], r[4]) for r in matches}
            if len(identities) > 1:
                raise AssertionError(('ambiguous linked identity', hex(pa), matches))
            self.cache[pa] = matches[0][2:5] if matches else ('unknown', '', '')
        return self.cache[pa]


def category(identity):
    owner, module, function = identity
    if owner == 'unknown':
        return 'unknown'
    name = module + ' ' + function
    if owner == 'probe':
        return 'entry_return'
    if owner == 'sdk':
        return 'copy_setup' if 'memcpy' in name or 'memmove' in name else 'sdk'
    if owner.startswith('driver:'):
        return 'driver'
    if any(s in name for s in ('page_copy', 'user_copy', 'window_copy', 'copy_move', 'copy_staged', 'api_copy', 'api_bulk_', 'api_borrow', 'bulk_source', 'bulk_transfer', 'kos_pages_copy',
                               'pages_move', 'page_side', 'xmem_transfer', 'aux_copy', 'syscall_bank_', 'syscall_buffer_bank')):
        return 'copy_setup'
    if any(s in name for s in ('cold_', 'dat_map', 'banking', 'domain_enter', 'domain_leave', 'upcall', 'ipc_invoke')):
        return 'mapping'
    if any(s in name for s in ('object', 'vfs_', 'name', 'xmem_lookup')):
        return 'object_handle'
    if module.startswith('ipc') or function.startswith('_desc_'):
        return 'ipc'
    return 'entry_return'


def span_pages(z, address, size, backward=False):
    """Physical 8 KiB pages a linear range touches, with bytes per page."""
    pages = collections.Counter()
    if backward:
        address = (address - size + 1) & 0xFFFF
    while size:
        n = min(size, 0x2000 - (address & 0x1FFF))
        pages[physical(z, address) >> 13] += n
        address = (address + n) & 0xFFFF
        size -= n
    return pages


def page_text(pages):
    return '+'.join('p%02x' % page for page in sorted(pages))


def stack_regions(syms):
    """Named stacks (base, top) from linked symbols; both ends must exist."""
    regions = []
    for name in sorted(syms):
        bare = name.lstrip('_')
        if bare.endswith('stack') and name + '_top' in syms:
            regions.append((bare, syms[name], syms[name + '_top']))
    return regions


class Span:
    def __init__(self, identity, z):
        self.identity = identity
        self.start = z.tstates
        self.buckets = collections.Counter({key: 0 for key in BUCKETS})
        self.functions = collections.Counter()
        self.unknown = collections.Counter()
        self.copies = collections.Counter()
        self.copy_runs = collections.Counter()
        self.counts = collections.Counter()
        self.stack_low = {}
        self.stacks = stack_regions(identity.syms)
        self.irq_depth = 0
        self.last_block = None  # identity of the latest block step
        self.gap = 0            # steps since it
        self.sound_start = len(z.sound_peer.writes)

    def step(self, z):
        pc, before, bc, hl, de = z.pc, z.tstates, z.bc, z.hl, z.de
        pa = physical(z, pc)
        who = self.identity.lookup(pa)
        if pc == CODE and z.rd(pc) == 0xF7:
            who = ('probe', 'raw_rst', 'RST30')
        if pc == 0x30 and z.rd(pc) == 0xC3 and z.rd16(pc + 1) == self.identity.syms['kos_syscall_entry']:
            who = ('kernel', 'page_zero', 'syscall_vector')
        op, ext = z.rd(pc), z.rd((pc + 1) & 65535)
        interrupt = bool(z.nmi_pending or (z.int_pending and z.iff1 and not z.ei_delay and z.im == 2))
        if interrupt:
            self.irq_depth += 1
        in_interrupt = self.irq_depth != 0
        block = not interrupt and op == 0xED and ext in BLOCK_COPY
        # Record all mapper writes, including rewrites to the same value.
        if not interrupt:
            port = ext if op == 0xD3 else z.c if op == 0xED and ext & 0xC7 == 0x41 else None
            if port is not None and port <= (15 if z.mapper else 0):
                self.counts['dat_writes' if z.mapper else 'latch_writes'] += 1
            if op == 0xF7:
                self.counts['syscalls'] += 1
            if who[2] in ENTRY_POINTS and pa == self.identity.entry(who, pa):
                self.counts['entry:' + who[2]] += 1
        for name, lo, hi in self.stacks:
            if lo < z.sp <= hi:
                self.stack_low[name] = min(self.stack_low.get(name, hi), z.sp)
        if all(not lo < z.sp <= hi for _, lo, hi in self.stacks):
            self.stack_low['caller'] = min(self.stack_low.get('caller', 65535), z.sp)
        z.step()
        spent = z.tstates - before
        bucket = 'interrupt' if in_interrupt else 'byte_movement' if block else category(who)
        self.buckets[bucket] += spent
        self.functions[':'.join(who)] += spent
        if who[0] == 'unknown':
            self.unknown['%05x:%04x' % (pa, pc)] += spent
        if not interrupt and op == 0xED and ext in (0xA2, 0xAA, 0xB2, 0xBA, 0xA3, 0xAB, 0xB3, 0xBB):
            moved_io = 1 if ext < 0xB0 else ((bc >> 8) - z.b) & 255
            if not moved_io and bc >> 8 == 0:
                moved_io = 256
            self.counts['peripheral_input_bytes' if ext in (0xA2, 0xAA, 0xB2, 0xBA) else 'peripheral_output_bytes'] += moved_io
        if block:
            # A copy run is one block instruction, or one unrolled or
            # looped sequence of LDIs in one function with at most twelve
            # other instructions between: the simulator steps a repeat at a
            # time, and the run is counted once however many steps it takes.
            moved = 1 if ext in (0xA0, 0xA8) else (bc - z.bc) & 65535
            if not moved and bc == 0:
                moved = 65536
            backward = ext in (0xA8, 0xB8)
            # The mapping is unchanged by the move: pages from the
            # registers as they were.
            key = (':'.join(who), page_text(span_pages(z, hl, moved, backward)),
                   page_text(span_pages(z, de, moved, backward)))
            if self.last_block != who or self.gap > 12:
                self.copy_runs[key, 'runs'] += 1
            self.copy_runs[key, 'bytes'] += moved
            self.copies[key[1] + '>' + key[2]] += moved
            self.last_block, self.gap = who, 0
        else:
            self.gap += 1
        if interrupt:
            self.counts['interrupt_entries'] += 1
        elif op == 0xED and ext in (0x4D, 0x45) and self.irq_depth:
            self.irq_depth -= 1

    def finish(self, z):
        total = z.tstates - self.start
        assert sum(self.buckets.values()) == total, (self.buckets, total)
        runs = sorted({key for key, _ in self.copy_runs})
        return dict(tstates=total, buckets=dict(self.buckets), functions=dict(self.functions),
                    unknown_addresses=dict(self.unknown), counts=dict(self.counts),
                    block_copy_bytes=sum(self.copies.values()),
                    copy_runs=[dict(function=f, source_pages=s, dest_pages=d,
                                    runs=self.copy_runs[(f, s, d), 'runs'],
                                    bytes=self.copy_runs[(f, s, d), 'bytes'])
                               for f, s, d in runs],
                    sound_writes=len(z.sound_peer.writes) - self.sound_start,
                    stack_low=self.stack_low)


def run_span(z, stop, identity=None):
    """Run to `stop`. With an identity, charge every instruction; either
    way, report the peer commands the span issued."""
    start = z.tstates
    peers = dict(z.hid.fs2_counts)
    span = Span(identity, z) if identity else None
    if span:
        for _ in range(LIMIT):
            if z.pc == stop:
                break
            span.step(z)
        else:
            raise AssertionError(('span limit', hex(z.pc)))
    else:
        reason, _ = z.run(LIMIT, stops=StopSet([stop]))
        assert reason == 'stop', (reason, hex(z.pc))
    result = span.finish(z) if span else {'tstates': z.tstates - start}
    result['peer_commands'] = {hex(k): v - peers.get(k, 0) for k, v in z.hid.fs2_counts.items() if v != peers.get(k, 0)}
    return result


def merge_spans(parts):
    """One workload result from consecutive spans: totals and counters
    add; the lowest stack mark wins."""
    out = dict(tstates=sum(p['tstates'] for p in parts), spans=len(parts))
    peers = collections.Counter()
    for p in parts:
        peers.update(p['peer_commands'])
    out['peer_commands'] = dict(peers)
    if 'buckets' not in parts[0]:
        return out
    for key in ('buckets', 'functions', 'unknown_addresses', 'counts'):
        total = collections.Counter()
        for p in parts:
            for name, value in p[key].items():
                total[name] += value
        out[key] = dict(total)
    runs = collections.Counter()
    for p in parts:
        for c in p['copy_runs']:
            runs[c['function'], c['source_pages'], c['dest_pages'], 'runs'] += c['runs']
            runs[c['function'], c['source_pages'], c['dest_pages'], 'bytes'] += c['bytes']
    out['copy_runs'] = [dict(function=f, source_pages=s, dest_pages=d, runs=runs[f, s, d, 'runs'],
                             bytes=runs[f, s, d, 'bytes'])
                        for f, s, d in sorted({key[:3] for key in runs})]
    out['block_copy_bytes'] = sum(p['block_copy_bytes'] for p in parts)
    out['sound_writes'] = sum(p['sound_writes'] for p in parts)
    out['stack_low'] = {}
    for p in parts:
        for name, low in p['stack_low'].items():
            out['stack_low'][name] = min(out['stack_low'].get(name, low), low)
    return out


def put(z, addr, data):
    for i, value in enumerate(data):
        z.wr(addr + i, value)


def get(z, addr, size):
    return bytes(z.rd(addr + i) for i in range(size))


def tool_flags(board):
    """Compiler flags exactly as make resolves them."""
    text = subprocess.run(['make', '-pn', 'BOARD=' + board], cwd=ROOT, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).stdout
    flags = {}
    for name in ('CFLAGS', 'EXE_CFLAGS', 'EXE_ASFLAGS'):
        match = re.search(r'^%s\s*:?=\s*(.*)$' % name, text, re.M)
        flags[name] = match[1] if match else None
    return flags


def memory_ledger(board, runner_replay):
    """Used and free bytes against the limits the build enforces."""
    map_path = ROOT / ('build-mk2/konaos-mk2.map' if board == 'mk2' else 'build/konaos.map')
    areas = {}
    for line in map_path.read_text().splitlines():
        match = re.match(r'(\S+)\s+([0-9A-F]{8})\s+([0-9A-F]{8}) =\s+(\d+)\. bytes \((REL|ABS)', line)
        if match and match[5] == 'REL' and int(match[4]):
            areas[match[1]] = (int(match[2], 16), int(match[4]))
    def end(*names):
        return max(areas[n][0] + areas[n][1] for n in names if n in areas)
    def size(*names):
        return sum(areas[n][1] for n in names if n in areas)
    ledger = dict(areas={k: dict(base=v[0], size=v[1]) for k, v in sorted(areas.items())})
    limits = []
    fixed_end = end('_CODE', '_INITIALIZER', '_GSFINAL', '_DATA', '_INITIALIZED')
    limits.append(dict(name='fixed region 2000h-5CFFh', used=fixed_end - 0x2000, limit=0x5D00 - 0x2000))
    if board == 'mk2':
        limits.append(dict(name='common packed run (_COMMON + _COMMON_DATA)', used=size('_COMMON', '_COMMON_DATA'), limit=3072))
        limits.append(dict(name='cold set 0 (_WINDOW + _WINDOW_DATA)', used=size('_WINDOW', '_WINDOW_DATA'), limit=16384))
        limits.append(dict(name='cold set 1 (_COLD1 + _COLD1_DATA)', used=size('_COLD1', '_COLD1_DATA'), limit=16384))
        plan = json.loads((ROOT / 'build-mk2/sys/modules/drivers.plan.json').read_text())['nucleus']
        for kind in ('boot', 'hot', 'mapped'):
            limits.append(dict(name='driver %s domain' % kind, used=plan['driver_%s_bytes' % kind],
                               limit=plan['driver_%s_capacity_bytes' % kind]))
    else:
        limits.append(dict(name='cold half (_WINDOW + _WINDOW_DATA)', used=size('_WINDOW', '_WINDOW_DATA'), limit=0xE000 - 0x6000))
    for item in limits:
        item['free'] = item['limit'] - item['used']
    ledger['limits'] = limits
    drivers = []
    for path in sorted((ROOT / runner_replay.STORE).glob('*.report.json')):
        report = json.loads(path.read_text())
        drivers.append(dict(name=report['name'], image_bytes=report['image_size'],
                            bss_bytes=report['bss_size'], total_bytes=report['total_size']))
    ledger['driver_images'] = drivers
    syms = runner_replay.syms
    ledger['stacks'] = [dict(name=name, base=lo, top=hi, size=hi - lo) for name, lo, hi in stack_regions(syms)]
    return ledger


class Runner:
    def __init__(self, board, profile):
        self.board, self.profile = board, profile
        self.build = ROOT / ('build-mk2/sys' if board == 'mk2' else 'build')
        self.probe = self.build / 'user/perf_probe.exe'
        self.rows = []
        self.proofs = {}
        self.stdio_states = {}
        self.small = self.other = self.large = None
        self.replay = (Mk2ShellReplay.open('build-mk2', 'konaos-mk2.bin', 'konaos-mk2.map')
                       if board == 'mk2' else Mk1ShellReplay.open('build', 'konaos.bin'))

    def boot(self, sdk=False, program='perf_probe'):
        """Boot to the prompt; with `sdk`, run the card EXE `program` (the
        probe, or the formatted-output probe) to its ready point."""
        r = self.replay
        program_dir = self.build / 'user' / program
        z = r.make_cpu()
        files = {'SMALL.BIN': PATTERN[:8192], 'LARGE.BIN': PATTERN,
                 'OTHER.BIN': PATTERN[8192:16384]}
        if sdk:
            files['PERF.EXE'] = (self.build / 'user' / (program + '.exe')).read_bytes()
        z.hid = z.ioc_peer = IocCommandPeer(fs2_media=True, fs2_files=files)
        z.sound_peer = SoundPeer()
        if self.board == 'mk1':
            Ctc().attach(z)
            r.start_kernel(z)
        r.run_to(z, CARD_PROMPT)
        if sdk:
            self.ps = read_symbols(program_dir / (program + '.map'))
            z.hid.hid_queue.extend(b'/fs/PERF.EXE\r')
            ready = self.ps['_perf_ready']
            def at_ready(_):
                return (z.mapper.dat_bank == 1 if z.mapper else z.latch & 0x18 == 0x10)
            assert r.drive(z, LIMIT, watch={ready}, hook=at_ready) is not None
        else:
            r.idle(z)
        self.z = z
        self.identity = Identity(self.board, r, z, program_dir if sdk else None)
        self.id = self.identity if self.profile else None
        return z

    # -- raw syscalls ----------------------------------------------------

    def raw(self, number, data=b'', enabled=False):
        z = self.z
        put(z, BLOCK, data)
        put(z, CODE, b'\xF7\0')
        z.halted = False
        z.iff1 = z.iff2 = enabled
        z.pc, z.a, z.hl = CODE, number, BLOCK
        result = run_span(z, CODE + 1, self.id)
        result['status'] = z.hl - 65536 if z.hl & 0x8000 else z.hl
        result['carry'] = z.f & 1
        if result['carry']:
            result['error'] = z.a
        return result

    def bind(self, path, flags=1):
        put(self.z, TEXT, path.encode() + b'\0')
        result = self.raw(6, struct.pack('<HHHBB', TEXT, len(path), flags, 0, 0))
        assert not result['carry'], result
        return result['status']

    def close(self, handle):
        result = self.raw(8, bytes([handle]))
        assert not result['carry'], result

    def call(self, handle, op, length=0, address=0, direction=255, payload=b'', enabled=False):
        frame = bytes([op, 0, 255, 0]) + struct.pack('<H', length) + payload.ljust(24, b'\0') + b'\0\0'
        data = bytes([handle, op, 0, 32]) + struct.pack('<HHBB', address, length, direction, 0) + frame + b'\0\0'
        return self.raw(7, data, enabled)

    def seek(self, handle, offset):
        # iocfat_seek_op takes the offset at payload[1..4]; payload[0] is
        # the routed object identity.
        result = self.call(handle, 2, payload=b'\0' + offset.to_bytes(4, 'little'))
        assert result['status'] == 0 and not result['carry'], result

    # -- cache scenarios -------------------------------------------------

    def fresh_cache(self):
        """A writable open drops the whole cache and marks every open
        handle uncached, so all handles are reopened afterwards. The
        driver has two file handles."""
        for handle in (self.small, self.other, self.large):
            if handle is not None:
                self.close(handle)
        writer = self.bind('/fs/OTHER.BIN', 2)
        self.close(writer)
        self.small = self.bind('/fs/SMALL.BIN')
        self.other = self.bind('/fs/OTHER.BIN')
        self.large = None

    def open_large(self):
        """The 64 KiB file does not fit the cache: its handle streams
        through the ring alone. It takes the place of SMALL."""
        self.close(self.small)
        self.small = None
        self.large = self.bind('/fs/LARGE.BIN')

    def warm_cache(self):
        """Cache every block of SMALL, then leave the ring on OTHER, so
        the next SMALL read is served from the cache, not the ring."""
        self.seek(self.small, 0)
        for _ in range(2):
            result = self.call(self.small, 0, 4096, 0x200, 1)
            assert result['status'] == 4096, result
        self.evict_ring()

    def evict_ring(self):
        """After a cache drop OTHER's first block comes from the card, so
        the ring moves to it (a cached block reaches the caller straight
        from the cache and leaves the ring alone)."""
        self.seek(self.other, 0)
        result = self.call(self.other, 0, 512, 0x200, 1)
        assert result['status'] == 512, result

    def file_state(self, result, cached):
        """The cache state a file read actually had, from the commands the
        peer saw and the driver ring fills the profile counted."""
        reads = int(result['peer_commands'].get('0x36', 0))
        # A cache hit refills the ring from the cache or delivers straight
        # from the cache to the caller.
        fills = (result['counts'].get('entry:_iocfat_ring_fill', 0) +
                 result['counts'].get('entry:_api_bulk_copy_out', 0)) if 'counts' in result else None
        if reads:
            return 'populating_miss' if cached else 'uncached_stream'
        if fills is None:
            return 'peer_silent'
        return 'cache_hit' if fills else 'ring_hit'

    def add(self, name, result, expect=None, cached=None, length=None, **metadata):
        row = dict(name=name, **metadata, **result)
        if length is not None:
            row['length'] = length
        if expect is not None:
            row['expected_state'] = expect
            row['observed_state'] = observed = self.file_state(result, cached)
            # The plain run has no driver counts: it can only tell a
            # silent peer from a talking one.
            assert observed == expect or (not self.profile and observed == 'peer_silent' and expect in ('ring_hit', 'cache_hit')), \
                (name, expect, observed, result['peer_commands'])
        # Copy passes: block moves of at least 16 bytes; shorter moves are
        # frames and bookkeeping. `copy_runs` is the full table, by the
        # executing function and the physical pages on each side.
        if 'copy_runs' in result:
            big = [c for c in result['copy_runs'] if c['bytes'] // max(c['runs'], 1) >= 16]
            row['copy_passes'] = sum(c['runs'] for c in big)
            row['copy_pass_bytes'] = sum(c['bytes'] for c in big)
            if row.get('length'):
                row['copy_pass_equivalents'] = round(row['copy_pass_bytes'] / row['length'], 2)
        row['command'] = ('python3 tests/perf_os_sdk.py --board %s --output FILE --rows %s'
                          % (self.board, shlex.quote('^' + re.escape(name) + '$')))
        self.rows.append(row)
        print('%s %s %s %d T' % (self.board, 'profile' if self.profile else 'plain', name, result['tstates']), flush=True)
        return row

    # -- raw rows --------------------------------------------------------

    def raw_rows(self):
        z = self.boot()
        self.add('raw.enosys', self.raw(255), interrupt_mode='masked', boundary='RST30 through return')
        joy = self.bind('/dev/input/joy/0', 0)
        result = self.call(joy, 0)
        assert result['status'] == 0 and not result['carry']
        self.add('raw.null_ipc', result, interrupt_mode='masked')
        sound = self.bind('/dev/sound', 0)
        self.small = self.other = self.large = None
        # Raw mapping cases include an active far view through the public
        # SYS_XMEM_MAP result and the board's ordinary opening operation.
        allocated = self.raw(0x20, struct.pack('<HH', 16384, 1))
        assert not allocated['carry']
        far_object = allocated['status']
        mapped = self.raw(0x25, struct.pack('<HH', far_object, 0))
        assert not mapped['carry']
        far_token = mapped['status'] & 65535
        far_address = self.z.rd16(BLOCK + 2)
        # The XMEM rows move to an object that is never mapped, so the
        # far-view buffer and the object cannot alias.
        unmapped = self.raw(0x20, struct.pack('<HH', 16384, 0))
        assert not unmapped['carry']
        xmem_object = unmapped['status']
        cases = [('page0', 0x200, 0x200), ('high', 0xA000, 0xA000), ('high_odd', 0xA001, 0xA001),
                 ('page_boundary', 0x7FC0, 0x7FF0),
                 ('far', far_address + 0x100, far_address + 0x100),
                 ('far_boundary', far_address + 0x1FC0, far_address + 0x1FF0)]
        stream_at = [32768, 49152]
        for location, address, sound_address in cases:
            if location.startswith('far'):
                if self.board == 'mk1':
                    z.latch = far_token
                else:
                    put(z, CODE + 8, b'\xEF\0')
                    z.pc, z.hl, z.halted, z.iff1, z.iff2 = CODE + 8, far_token, False, False, False
                    run_span(z, CODE + 9)
            put(z, sound_address, bytes([4, 0x7F]) * 16)
            before = len(z.sound_peer.writes)
            result = self.call(sound, 1, 32, sound_address, 0)
            assert result['status'] == 32 and len(z.sound_peer.writes) - before == 16, result
            self.add('raw.descriptor_in.sound32.' + location, result, direction='IN', buffer=location,
                     address=sound_address, length=32, alignment=sound_address % 512, interrupt_mode='masked')
            for length in (1, 128, 512, 4096):
                meta = dict(buffer=location, address=address, length=length,
                            alignment=address % 512, file_offset=0, interrupt_mode='masked')
                def read_row(state, handle, offset, cached):
                    self.seek(handle, offset)
                    result = self.call(handle, 0, length, address, 1)
                    assert result['status'] == length and not result['carry'], result
                    actual = get(z, address, length)
                    assert actual == PATTERN[offset:offset + length], (location, length, state)
                    self.add('raw.file.%s.%s.%d' % (location, state, length), result, expect=state,
                             cached=cached, bytes_sha256=hashlib.sha256(actual).hexdigest(), **dict(meta, file_offset=offset))
                # A ring is 2 KiB: a 4 KiB request can only be refilled,
                # so it has no ring-hit state.
                self.fresh_cache()
                read_row('populating_miss', self.small, 0, True)
                if length * 2 <= 2048:
                    read_row('ring_hit', self.small, 0, True)
                self.warm_cache()
                read_row('cache_hit', self.small, 0, True)
                # A cold stream: a distinct offset outside the ring.
                offset = stream_at[0]
                stream_at.reverse()
                self.open_large()
                read_row('uncached_stream', self.large, offset, False)
                if location == 'page0' and length == 512:
                    # The state proof: a second read of the same range is
                    # served by the device again, never by a cache. The
                    # ring moves to LARGE's other stream offset first.
                    self.seek(self.large, offset ^ 16384)
                    result = self.call(self.large, 0, length, address, 1)
                    assert result['status'] == length, result
                    self.seek(self.large, offset)
                    again = self.call(self.large, 0, length, address, 1)
                    self.proofs['uncached_reread_commands'] = int(again['peer_commands'].get('0x36', 0))
                    assert self.proofs['uncached_reread_commands'] > 0
            for length in (1, 128, 512, 4096):
                put(z, address, PATTERN[:length])
                for number, name in ((0x23, 'write'), (0x22, 'read')):
                    result = self.raw(number, struct.pack('<HHHH', xmem_object, 0, address, length))
                    assert result['status'] == length and not result['carry'], result
                    assert get(z, address, length) == PATTERN[:length]
                    self.add('raw.xmem.%s.%d.%s' % (name, length, location), result, buffer=location,
                             address=address, alignment=address % 512, length=length, interrupt_mode='masked')
            if location == 'far_boundary':
                if self.board == 'mk1':
                    z.latch = 0x10 | (z.latch & 7)
                else:
                    z.pc, z.hl = CODE + 8, 0
                    run_span(z, CODE + 9)
        self.fresh_cache()
        self.open_large()
        # Interrupt-enabled service evidence is a separate span; the CTC
        # state continues normally and is never charged to masked rows.
        for length in (512, 4096):
            self.seek(self.large, 16384)
            result = self.call(self.large, 0, length, 0xA000, 1, enabled=True)
            assert result['status'] == length and get(z, 0xA000, length) == PATTERN[16384:16384 + length]
            self.add('service.file.%d' % length, result, buffer='high', length=length,
                     alignment=0, interrupt_mode='enabled')
        self.raw_identity = self.identity
        self.raw_replay_syms = self.replay.syms

    # -- standalone SDK rows ---------------------------------------------

    def var(self, name, value=None):
        addr = self.ps['_perf_' + name]
        if value is None:
            return self.z.rd16(addr)
        self.z.wr16(addr, value)

    def stream_state(self):
        base = self.var('stream')
        # struct kos_stream: handle, mode, state, count, next, pos.
        z = self.z
        return dict(handle_open=z.rd16(base) != 0, mode=z.rd(base + 2), state=z.rd(base + 3),
                    count=z.rd16(base + 4), next=z.rd16(base + 6),
                    pos=z.rd16(base + 8) | z.rd16(base + 10) << 16)

    def sdk_call(self, name):
        z = self.z
        z.halted = False
        z.iff1 = z.iff2 = False
        z.push16(RETURN)
        z.pc = self.ps['_perf_' + name]
        result = run_span(z, RETURN, self.id)
        result['status'] = self.var('result')
        return result

    def sdk_open(self, op, state=None):
        """Prepare the cache state, open the stream and return its initial
        state, which must equal every other form's of the same direction."""
        if state:
            self.sdk_call('prepare_' + state)
        direction = 'read' if op == 'read' else 'write'
        self.sdk_call('open_' + direction)
        assert self.var('stream') != 0
        initial = self.stream_state()
        self.stdio_states.setdefault(direction, initial)
        assert initial == self.stdio_states[direction], (op, initial)
        return initial

    def sdk_rows(self):
        z = self.boot(sdk=True)
        assert self.var('sound') < 0x8000 and 0 < self.var('object') < 0x8000
        total = 4096
        for state, expect in (('warm', 'cache_hit'), ('cold', 'populating_miss')):
            forms = [(size, total // size, 0xA000, '') for size in (1, 16, 512, 4096)]
            forms += [(512, total // 512, 0xA001, '.odd'), (1, 1, 0xA000, '.tiny')]
            for size, items, address, tag in forms:
                length = size * items
                name = 'sdk.fread.item%d%s.%s' % (size, tag, expect) if tag != '.tiny' else 'sdk.fread.tiny.' + expect
                initial = self.sdk_open('read', state)
                self.var('size', size)
                self.var('items', items)
                self.var('address', address)
                result = self.sdk_call('read')
                assert result['status'] == items, (name, result)
                assert get(z, address, length) == PATTERN[:length]
                final = self.stream_state()
                self.add(name, result, expect=expect, cached=True, length=length, item_size=size, items=items,
                         buffer='high', address=address, alignment=address % 512, interrupt_mode='masked',
                         initial_stream=initial, final_stream=final,
                         boundary='probe wrapper entry through RET; no clock/report work')
                self.sdk_call('close')
        for size, items, address, tag in [(s, 4096 // s, 0xA000, '') for s in (1, 16, 512, 4096)] + \
                [(512, 8, 0xA001, '.odd'), (1, 1, 0xA000, '.tiny')]:
            length = size * items
            name = 'sdk.fwrite.item%d%s' % (size, tag) if tag != '.tiny' else 'sdk.fwrite.tiny'
            initial = self.sdk_open('write')
            self.var('size', size)
            self.var('items', items)
            self.var('address', address)
            put(z, address, PATTERN[:length])
            result = self.sdk_call('write')
            assert result['status'] == items, (name, result)
            final = self.stream_state()
            self.add(name, result, length=length, item_size=size, items=items, buffer='high',
                     address=address, alignment=address % 512, interrupt_mode='masked',
                     initial_stream=initial, final_stream=final,
                     boundary='probe wrapper entry through RET; no clock/report work')
            closed = self.sdk_call('close')
            assert bytes(z.hid.fs2_files[('WRITE.BIN',)]) == PATTERN[:length], name
            self.add(name + '.close', closed, length=length, interrupt_mode='masked',
                     boundary='fclose: flush of what the write left buffered, then close')
        for length in (1, 16, 128, 512):
            initial = self.sdk_open('write')
            text = (b'abcdefghijklmnopqrstuvwxyz012345' * 17)[:length]
            put(z, 0xA000, text + b'\0')
            self.var('address', 0xA000)
            result = self.sdk_call('fputs')
            assert result['status'] == 0, result
            self.add('sdk.fputs.file.%d' % length, result, length=length, buffer='high',
                     address=0xA000, alignment=0, interrupt_mode='masked', initial_stream=initial,
                     final_stream=self.stream_state(),
                     boundary='fputs (one-byte items through fwrite) into an empty buffer')
            closed = self.sdk_call('close')
            assert bytes(z.hid.fs2_files[('WRITE.BIN',)]) == text
            self.add('sdk.fputs.file.%d.close' % length, closed, length=length, interrupt_mode='masked',
                     boundary='fclose flush')
        for length in (1, 32, 128, 512):
            put(z, 0xA000, b'x' * length + b'\0')
            result = self.sdk_call('console')
            assert result['status'] == length
            self.add('sdk.console.buffered.%d' % length, result,
                     length=length, buffer='high', interrupt_mode='masked')
            result = self.sdk_call('flush')
            assert result['status'] == 0
            self.add('sdk.console.flush.%d' % length, result,
                     length=length, interrupt_mode='masked')
        for pairs in (1, 16, 128):
            data = bytes([4, 0x7F]) * pairs
            put(z, 0xA000, data)
            self.var('length', pairs)
            before = len(z.sound_peer.writes)
            result = self.sdk_call('sound_write')
            assert result['status'] == pairs
            assert len(z.sound_peer.writes) - before == pairs
            self.add('sdk.sound.%d' % pairs, result, length=pairs * 2,
                     buffer='high', direction='IN', interrupt_mode='masked')
        self.sdk_identity = self.identity
        self.sdk_syms = self.replay.syms

    # -- error outcomes and general workloads ----------------------------

    def guarded_read(self, handle, length, address, guard=16):
        """A read with sentinel bytes around the requested range: bytes
        outside it must be unchanged whatever the outcome."""
        z = self.z
        put(z, address - guard, b'\x5A' * (length + 2 * guard))
        result = self.call(handle, 0, length, address, 1)
        outside = get(z, address - guard, guard) + get(z, address + length, guard)
        assert outside == b'\x5A' * (2 * guard), ('bytes outside the request changed', result)
        return result

    def outcome_rows(self):
        """Error and short outcomes, continuing the raw rows' boot (the
        LARGE and OTHER handles are open)."""
        z = self.z
        meta = dict(buffer='high', address=0xA000, alignment=0, interrupt_mode='masked')
        # A handle number the process does not hold: the syscall fails
        # (carry set, A = KOS_EINVAL).
        result = self.guarded_read(0xF0, 512, 0xA000)
        assert result['carry'] and result['error'] == 2, result
        self.add('outcome.file.bad_handle', result, length=512, **meta)
        # An operation the file object does not implement: the driver
        # replies -KOS_EINVAL.
        result = self.call(self.large, 0x7E)
        assert result['status'] == -2 and not result['carry'], result
        self.add('outcome.file.bad_op', result, length=0, **meta)
        # A descriptor whose range wraps past FFFFh is refused by the IPC
        # layer (a carry-clear -KOS_EINVAL reply).
        # The wrapped tail would land on the page-0 vectors.
        before = get(z, 0, 0xF0)
        result = self.call(self.large, 0, 512, 0xFEF0, 1)
        assert result['status'] == -2 and not result['carry'], result
        assert get(z, 0, 0xF0) == before, 'a refused descriptor changed page 0'
        self.add('outcome.descriptor.wrap', result, **dict(meta, length=512, address=0xFEF0, alignment=0xF0))
        # End of file: nothing is delivered and the call succeeds.
        self.seek(self.large, 65536)
        result = self.guarded_read(self.large, 512, 0xA000)
        assert result['status'] == 0 and not result['carry'], result
        self.add('outcome.file.eof', result, length=512, **meta)
        # A short read at the end of the file delivers what remains.
        self.seek(self.large, 65536 - 100)
        result = self.guarded_read(self.large, 512, 0xA000)
        assert result['status'] == 100 and not result['carry'], result
        assert get(z, 0xA000, 100) == PATTERN[-100:]
        self.add('outcome.file.short', result, length=512, **meta)

    def seek_read(self, handle, span, count=16, length=128):
        z, parts = self.z, []
        for i in range(count):
            offset = (i * 4099 + 777) % (span - length)
            parts.append(self.call(handle, 2, payload=b'\0' + offset.to_bytes(4, 'little')))
            assert parts[-1]['status'] == 0 and not parts[-1]['carry'], parts[-1]
            parts.append(self.call(handle, 0, length, 0xA000, 1))
            assert parts[-1]['status'] == length and not parts[-1]['carry'], parts[-1]
            assert get(z, 0xA000, length) == PATTERN[offset:offset + length]
        result = merge_spans(parts)
        result['status'], result['carry'] = count * length, 0
        return result

    def workload_rows(self):
        meta = dict(buffer='high', address=0xA000, alignment=0, interrupt_mode='masked')
        self.add('workload.seek_read.uncached', self.seek_read(self.large, 65536), length=16 * 128,
                 boundary='16 x (seek, 128-byte read) at scattered offsets of a file the cache does not hold',
                 **meta)
        self.fresh_cache()
        self.warm_cache()
        self.add('workload.seek_read.cached', self.seek_read(self.small, 8192), length=16 * 128,
                 boundary='16 x (seek, 128-byte read) at scattered offsets of a cached file', **meta)
        z = self.boot(sdk=True, program='perf_printf')
        self.var('items', 8)
        result = self.sdk_call('printf')
        assert result['status'] == 8 * 21, result
        self.add('workload.console_printf', result, length=8 * 21, interrupt_mode='masked',
                 boundary='8 printf lines ("%5u %04x %s\\n"), each written out at its newline')

    def run(self):
        self.sdk_rows()
        self.raw_rows()
        self.outcome_rows()
        self.workload_rows()
        return self.rows


def package_inputs(rows):
    """Measured costs by the later package each would serve."""
    table = {row['name']: row for row in rows}
    def get_row(name):
        return table[name]
    def share(row, *buckets):
        return sum(row['buckets'][b] for b in buckets)
    inputs = {}
    inputs['140_bulk_stdio'] = {name: table[name]['tstates'] for name in table
                                if name.startswith(('sdk.fread.item', 'sdk.fwrite.item', 'sdk.fputs.file.'))
                                and not name.endswith('.close')}
    fixed = {}
    for name in ('raw.enosys', 'raw.null_ipc'):
        fixed[name] = dict(tstates=table[name]['tstates'], buckets=table[name]['buckets'])
    inputs['142_syscall_ipc_fixed'] = fixed
    inputs['141_mover'] = {name: dict(byte_movement=row['buckets']['byte_movement'],
                                      copy_setup=row['buckets']['copy_setup'], tstates=row['tstates'],
                                      copy_passes=row.get('copy_passes'),
                                      copy_pass_bytes=row.get('copy_pass_bytes'))
                           for name, row in table.items()
                           if name.startswith('raw.file.high.') and name.endswith(('.512', '.4096'))}
    inputs['143_cache_hit'] = {name: dict(tstates=row['tstates'], copy_passes=row.get('copy_passes'),
                                          byte_movement=row['buckets']['byte_movement'],
                                          copy_setup=row['buckets']['copy_setup'])
                               for name, row in table.items() if '.cache_hit.' in name and name.startswith('raw.file.')}
    inputs['144_uncached'] = {name: dict(tstates=row['tstates'], driver=row['buckets']['driver'],
                                         copy_passes=row.get('copy_passes'),
                                         copy_setup=row['buckets']['copy_setup'],
                                         borrow_calls=row['counts'].get('entry:_api_borrow', 0),
                                         lent_receives=row['counts'].get('entry:_api_borrowed', 0),
                                         peripheral_input_bytes=row['counts'].get('peripheral_input_bytes'))
                              for name, row in table.items() if '.uncached_stream.' in name}
    inputs['145_copy_ledger'] = {name: dict(copy_passes=row.get('copy_passes'),
                                            copy_pass_equivalents=row.get('copy_pass_equivalents'),
                                            copy_pass_bytes=row.get('copy_pass_bytes'),
                                            length=row.get('length'))
                                 for name, row in table.items() if row.get('copy_passes') is not None}
    ranking = []
    for name, row in table.items():
        if not name.startswith(('raw.file.high.', 'raw.xmem', 'sdk.')) or name.endswith('.close'):
            continue
        for bucket, spent in row['buckets'].items():
            ranking.append((spent, name, bucket))
    inputs['ranked_buckets'] = [dict(tstates=s, row=n, bucket=b) for s, n, b in sorted(ranking, reverse=True)[:25]]
    functions = collections.Counter()
    for name, row in table.items():
        if name.startswith(('raw.file.high.', 'raw.xmem', 'sdk.')):
            for function, spent in row['functions'].items():
                functions[function] += spent
    inputs['ranked_functions_exclusive_sum_over_rows'] = [dict(function=f, tstates=s) for f, s in functions.most_common(25)]
    return inputs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--board', required=True, choices=('mk1', 'mk2'))
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--rows', help='regular expression: write only matching rows (every scenario still runs)')
    args = parser.parse_args()
    started = time.time()
    plain = Runner(args.board, False)
    observed = Runner(args.board, True)
    a, b = plain.run(), observed.run()
    assert len(a) == len(b)
    for disabled, enabled in zip(a, b):
        key = lambda row: (row['name'], row['tstates'], row['status'], row['carry'] if 'carry' in row else None, row['peer_commands'])
        assert key(disabled) == key(enabled), (disabled, enabled)
        enabled['unprofiled_tstates'] = disabled['tstates']
        enabled['repeatability_delta'] = enabled['tstates'] - disabled['tstates']
    assert plain.proofs == observed.proofs
    build = observed.build
    image = ROOT / ('build-mk2/konaos-mk2.bin' if args.board == 'mk2' else 'build/konaos.bin')
    sdk = next(build.glob('kos-sdk-*.tar.gz'))
    diff = subprocess.check_output(['git', 'diff', '--binary', 'HEAD'], cwd=ROOT)
    unknown = [(row['buckets']['unknown'] / row['tstates'], row['name']) for row in b]
    stacks = collections.defaultdict(lambda: None)
    for row in b:
        for name, low in row['stack_low'].items():
            stacks[name] = low if stacks[name] is None else min(stacks[name], low)
    memory = memory_ledger(args.board, observed.replay)
    for entry in memory['stacks']:
        low = stacks.get(entry['name'])
        entry['observed_low'] = low
        entry['observed_used'] = None if low is None else entry['top'] - low
        entry['margin'] = None if low is None else low - entry['base']
    memory['caller_sp_low'] = stacks.get('caller')
    rows = [row for row in b if not args.rows or re.search(args.rows, row['name'])]
    output = dict(format='kos-performance/2', board=args.board,
                  identity=dict(revision=command('git', 'rev-parse', 'HEAD'),
                                dirty_status=command('git', 'status', '--short'),
                                dirty_diff_sha256=hashlib.sha256(diff).hexdigest(),
                                image_sha256=sha(image), sdk_sha256=sha(sdk),
                                probe_sha256=sha(observed.probe),
                                printf_probe_sha256=sha(observed.build / 'user/perf_printf.exe'),
                                compiler=command('sdcc', '--version'),
                                flags=tool_flags(args.board),
                                peer='IocCommandPeer FS2 default; read_run false; seeded Mk2 mapper; Ctc; SoundPeer; V9958Peer fixed command_busy=4000',
                                medium='in-memory FS2 files: SMALL 8192, LARGE 65536, OTHER 8192 bytes',
                                interrupts='masked for every row except service.*; Ctc attached',
                                clock_reporting_tstates=0),
                  checked_link_globals=observed.raw_identity.checked_globals + observed.sdk_identity.checked_globals,
                  state_proofs=observed.proofs,
                  stdio_initial_states=observed.stdio_states,
                  unknown_fraction_max=max(unknown),
                  memory_ledger=memory,
                  package_inputs=package_inputs(b),
                  rows_filter=args.rows,
                  wall_seconds=time.time() - started, rows=rows)
    args.output.write_text(json.dumps(output, indent=2, sort_keys=True) + '\n')
    print('wrote %s (%d rows)' % (args.output, len(rows)))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
