#!/usr/bin/env python3
"""Processes and extended-memory objects on Zephyr-80 Mk2 pages.

The image (tests/target/mk2proc/, `make mk2proc`) is the nucleus's page
allocator, page copy, far window, launch gate, process lifecycle, page
loader and extended-memory family built for Mk2 (architecture 2.7.6),
with the syscall gate, the IRQ executive and the event core, around a
bootstrap, a C syscall dispatcher (EXIT, EXEC, XMEM_*) and one program.
The kernel launcher runs the program at depth 1; the tests drive every
process through the program's fragments - a syscall, the far window, a
copy through plain pointers - and read the machine back: registers, SP,
both DAT banks, the mirrors, the cached-image identity, the page owners.

Skipped when the image has not been built.
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import z80sim  # noqa: E402
from z80sim import StopSet, Z80  # noqa: E402

BUILD = os.path.join(ROOT, "build-mk2proc")
IMAGE = os.path.join(BUILD, "mk2proc.bin")
MAP = os.path.join(BUILD, "mk2proc.map")
LIMIT = 6000000

PAGE = 0x2000
KERNEL_PAGES = {1: 0x41, 2: 0x42, 3: 0x43, 7: 0x7F}
NUCLEUS = (0x40, 0x41, 0x42, 0x43, 0x44, 0x7F)
SYSTEM_PAGE0 = 0x40
COMMON_PAGE = 0x7F
OWNER_FREE, OWNER_NUCLEUS = 0x00, 0x80
DAT_STATE = 0xEF10                    # memory.inc KOS_DAT_STATE_BASE
DAT_CACHED = 0xEF21
CTL_SYSTEM, CTL_WORKING = 0x02, 0x03
ARGS = 0x0400                         # argument blocks, process page 0
NMI_STUB = bytes([0xC3, 0x08, 0xEF])  # JP KOS_RT_VECTOR
PROGRAM_BSS = 0x100

EXIT, EXEC = 0x00, 0x05
ALLOC, FREE, READ, WRITE, INFO, MAP_ = 0x20, 0x21, 0x22, 0x23, 0x24, 0x25
SEG_INFO = 0x26
MAPPABLE = 0x0001
EINVAL, EBUSY, ENOSPC, ENOSYS, EFAULT = 0x02, 0x04, 0x06, 0x01, 0x0C

REGS = ("b", "c", "d", "e", "ix", "iy", "alt_a", "alt_f", "alt_b",
        "alt_c", "alt_d", "alt_e", "alt_h", "alt_l")
ABI_REGS = ("b", "c", "d", "e", "ix", "iy", "sp")   # docs/abi.md 1
COPY_WINDOWS = (5, 6)                 # memory.inc KOS_DAT_COPY_*_ENTRY


def kphys(addr):
    """A nucleus address (kernel data, common) as a physical address."""
    return KERNEL_PAGES[addr >> 13] * PAGE | (addr & 0x1FFF)


def hardware(z):
    m = z.mapper
    return tuple(m.entries) + ((m.map_enable << 1) | m.dat_bank,)


def mirror(z):
    base = kphys(DAT_STATE)
    return tuple(z.phys[base:base + 17])


def cached(z):
    base = kphys(DAT_CACHED)
    return z.phys[base] | (z.phys[base + 1] << 8)


def le16(value):
    return bytes([value & 0xFF, value >> 8])


def pattern(seed, length):
    return bytes((seed + i * 7 + (i >> 8)) & 0xFF for i in range(length))


@unittest.skipUnless(os.path.isfile(IMAGE), "make mk2proc builds the image")
class Mk2ProcCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(IMAGE, "rb") as handle:
            cls.rom = handle.read()
        cls.syms = z80sim.read_symbols(MAP)

    def sym(self, name):
        return self.syms[name]

    # ---- the machine ---------------------------------------------------
    def boot(self):
        z = Z80(self.rom, b"", board="mk2")
        self.run_to(z, ("p_idle",))
        self.assertEqual(self.running(z), 0)
        return z

    def run_to(self, z, names, limit=LIMIT):
        stops = [self.sym(name) for name in names]
        reason, _ = z.run(limit, stops=StopSet(stops), check_first=False)
        self.assertEqual(reason, "stop", "none of %s reached" % (names,))
        return z.pc

    def kbytes(self, z, name, length, offset=0):
        base = kphys(self.sym(name) + offset)
        return bytes(z.phys[base:base + length])

    def kword(self, z, name, offset=0):
        data = self.kbytes(z, name, 2, offset)
        return data[0] | (data[1] << 8)

    def running(self, z):
        return self.kbytes(z, "_process_running_pid", 1)[0]

    def image(self, z, pid):
        return self.kbytes(z, "_kos_dat_images", 8, 8 * pid)

    def image_addr(self, pid):
        return self.sym("_kos_dat_images") + 8 * pid

    def owners(self, z):
        return self.kbytes(z, "_kos_page_owner", 64)

    def free_pages(self, z):
        return self.owners(z).count(OWNER_FREE)

    def owned(self, z, pid):
        return [0x40 + i for i, o in enumerate(self.owners(z))
                if o == pid + 1]

    # ---- the running program's view (its working bank) ----------------
    def view(self, z, addr):
        return z.mapper.entries[8 + (addr >> 13)] * PAGE | (addr & 0x1FFF)

    def put(self, z, addr, data):
        for i, byte in enumerate(data):
            z.phys[self.view(z, addr + i)] = byte

    def get(self, z, addr, length):
        return bytes(z.phys[self.view(z, addr + i)] for i in range(length))

    def set_regs(self, z, seed):
        for i, name in enumerate(REGS):
            width = 0xFFFF if name in ("ix", "iy") else 0xFF
            setattr(z, name, (seed * 37 + i * 91 + 0x5A) & width)

    def call(self, z, number, block=b"", seed=1, stops=("p_return",)):
        """One syscall from p_call with `block` at ARGS.  Returns the
        registers the gate preserves, as they were before the call."""
        self.put(z, ARGS, block)
        self.set_regs(z, seed)
        z.a = number
        z.h, z.l = ARGS >> 8, ARGS & 0xFF
        z.pc = self.sym("p_call")
        z.iff1 = z.iff2 = True
        before = {name: getattr(z, name) for name in REGS + ("sp",)}
        self.run_to(z, stops)
        return before

    def ok(self, z, number, block=b"", seed=1):
        self.call(z, number, block, seed)
        self.assertEqual(z.f & 1, 0, "syscall %02Xh failed: %02Xh"
                         % (number, z.a))
        return z.h << 8 | z.l

    def fails(self, z, number, block, error):
        state = (hardware(z), self.owners(z))
        self.call(z, number, block)
        self.assertEqual((z.f & 1, z.a, z.h << 8 | z.l), (1, error, 0))
        self.assertEqual((hardware(z), self.owners(z)), state)

    def exec_child(self, z, tail=b"", seed=2):
        """EXEC from the running program; returns at the child's idle."""
        parent = self.running(z)
        tail_at = 0x0500
        self.put(z, tail_at, tail)
        block = le16(0x0600) + le16(1) + le16(tail_at) + le16(len(tail))
        before = self.call(z, EXEC, block, seed, stops=("p_idle",))
        self.assertNotEqual(self.running(z), parent)
        # The child's run leaves the parent what the ABI preserves.
        return {name: before[name] for name in ABI_REGS}

    def exit(self, z, status):
        self.put(z, ARGS, le16(status))
        z.a = EXIT
        z.h, z.l = ARGS >> 8, ARGS & 0xFF
        z.pc = self.sym("p_call")
        self.run_to(z, ("p_return",))

    def alloc(self, z, size, flags=0):
        return self.ok(z, ALLOC, le16(size) + le16(flags))

    def write(self, z, handle, offset, buffer, data):
        self.put(z, buffer, data)
        moved = self.ok(z, WRITE, le16(handle) + le16(offset) +
                        le16(buffer) + le16(len(data)))
        self.assertEqual(moved, len(data))

    def read(self, z, handle, offset, buffer, length):
        moved = self.ok(z, READ, le16(handle) + le16(offset) +
                        le16(buffer) + le16(length))
        self.assertEqual(moved, length)
        return self.get(z, buffer, length)

    def map_(self, z, handle):
        mapping = self.ok(z, MAP_, le16(handle) + le16(0))
        self.assertEqual(self.get(z, ARGS + 2, 2), le16(0x8000))
        return mapping

    def far(self, z, handle):
        """kos_far_map from the program: (carry, the previous handle)."""
        z.h, z.l = handle >> 8, handle & 0xFF
        z.pc = self.sym("p_far")
        z.iff1 = z.iff2 = True
        self.run_to(z, ("p_far_done",))
        self.assertTrue(z.iff1)
        return z.f & 1, z.h << 8 | z.l

    def copy(self, z, src, dst, length):
        """LDIR in the program's own mapping (plain pointers)."""
        z.h, z.l = src >> 8, src & 0xFF
        z.d, z.e = dst >> 8, dst & 0xFF
        z.b, z.c = length >> 8, length & 0xFF
        z.pc = self.sym("p_copy")
        self.run_to(z, ("p_copy_done",))

    def assert_preserved(self, z, before):
        for name, value in before.items():
            self.assertEqual(getattr(z, name), value, name)

    def assert_program_context(self, z, pid):
        image = self.image(z, pid)
        self.assertEqual(self.running(z), pid)
        self.assertEqual(hardware(z)[8:16], tuple(image))
        self.assertEqual(hardware(z)[16], CTL_WORKING)
        self.assertEqual(hardware(z)[0], SYSTEM_PAGE0)
        self.assertEqual(mirror(z), hardware(z))
        self.assertEqual(self.kbytes(z, "kos_gate_open", 1)[0], 0)

    def state(self, z):
        """The mapping a program sees and the nucleus restores: both
        banks but the copy windows (the nucleus's own, undefined between
        calls), the control register, the mirrors, the cached image and
        the far-window handle."""
        def program(regs):
            return tuple(v for r, v in enumerate(regs)
                         if r not in COPY_WINDOWS)
        return (program(hardware(z)), program(mirror(z)), cached(z),
                self.kword(z, "_kos_far_current"))


class TestPages(Mk2ProcCase):
    def test_the_program_runs_on_its_own_pages(self):
        z = self.boot()
        owners = self.owners(z)
        for page in NUCLEUS:
            self.assertEqual(owners[page - 0x40], OWNER_NUCLEUS)
        image = self.image(z, 0)
        self.assertEqual(sorted(image[:7]), self.owned(z, 0))
        self.assertEqual(image[7], COMMON_PAGE)
        self.assertEqual(self.free_pages(z), 64 - len(NUCLEUS) - 7)
        self.assert_program_context(z, 0)
        self.assertEqual(cached(z), self.image_addr(0))
        self.assertEqual(self.kword(z, "_kos_dat_running"),
                         self.image_addr(0))
        # Page zero: the three entry stubs, the startup block, zeroes.
        entry = self.sym("kos_syscall_entry")
        far = self.sym("kos_far_map")
        page0 = bytes(z.phys[image[0] * PAGE:image[0] * PAGE + 0x100])
        self.assertEqual(page0[0x28:0x2B],
                         bytes([0xC3, far & 0xFF, far >> 8]))
        self.assertEqual(page0[0x30:0x33],
                         bytes([0xC3, entry & 0xFF, entry >> 8]))
        self.assertEqual(page0[0x66:0x69], NMI_STUB)
        self.assertEqual(page0[0x80:0x83], bytes([1, 0, 0]))
        rest = (page0[:0x28] + page0[0x2B:0x30] + page0[0x33:0x66] +
                page0[0x69:0x80])
        self.assertEqual(rest, bytes(len(rest)))
        # The image from ROM, then its BSS.
        size = self.sym("p_end") - self.sym("p_entry")
        self.assertEqual(self.get(z, 0x0100, size),
                         self.rom[0x0100:0x0100 + size])
        self.assertEqual(self.get(z, 0x0100 + size, PROGRAM_BSS),
                         bytes(PROGRAM_BSS))


class TestExec(Mk2ProcCase):
    def test_exec_and_exit_return_the_parents_exact_state(self):
        z = self.boot()
        owners = self.owners(z)
        state = self.state(z)
        before = self.exec_child(z, b"hello, child")
        child = self.running(z)
        self.assertEqual(child, 1)
        self.assert_program_context(z, child)
        self.assertEqual(self.kword(z, "_kos_dat_running"),
                         self.image_addr(child))
        self.assertEqual(self.get(z, 0x80, 15),
                         bytes([1, 12]) + b"hello, child\x00")
        self.assertTrue(set(self.image(z, child)[:7]).isdisjoint(
            self.image(z, 0)))
        self.assertEqual(self.free_pages(z), owners.count(OWNER_FREE) - 7)
        self.exit(z, 0x1234)
        self.assert_preserved(z, before)
        self.assertEqual((z.h << 8 | z.l, z.f & 1), (0x1234, 0))
        self.assertTrue(z.iff1)
        self.assertEqual(self.state(z), state)
        self.assertEqual(self.owners(z), owners)
        self.assert_program_context(z, 0)

    def test_a_child_that_returns_from_its_entry_exits_with_efault(self):
        z = self.boot()
        owners = self.owners(z)
        state = self.state(z)
        before = self.exec_child(z)
        self.assertEqual(z.sp, 0xDFFE)                 # the trap on top
        z.pc = self.sym("p_ret")
        self.run_to(z, ("p_return",))
        self.assert_preserved(z, before)
        self.assertEqual((z.h << 8 | z.l, z.f & 1), (EFAULT, 0))
        self.assertEqual(self.state(z), state)
        self.assertEqual(self.owners(z), owners)
        self.assert_program_context(z, 0)

    def test_page_counts_return_after_100_cycles(self):
        z = self.boot()
        owners = self.owners(z)
        handles = set()
        for i in range(100):
            self.exec_child(z, seed=i)
            a = self.alloc(z, 0x4800)                  # three pages
            b = self.alloc(z, 0x2000, MAPPABLE)
            self.write(z, b, 0x10, 0x1000, pattern(i, 64))
            self.ok(z, FREE, le16(a))
            self.exit(z, i)                            # b left to exit
            self.assertEqual(z.h << 8 | z.l, i)
            self.assertEqual(self.owners(z), owners)
            h = self.alloc(z, 0x2001)                  # two pages
            handles.add(h)
            self.ok(z, FREE, le16(h))
            self.assertEqual(self.owners(z), owners)
        self.assertGreater(len(handles), 1)            # generations move
        self.assert_program_context(z, 0)

    def test_nested_children_keep_every_open_crossing(self):
        z = self.boot()
        # The parent has its far window open onto an object.
        b = self.alloc(z, 0x4000, MAPPABLE)
        self.write(z, b, 0, 0x2000, pattern(1, 0x400))
        mapping = self.map_(z, b)
        self.assertEqual(self.far(z, mapping), (0, 0))
        parent_bank = hardware(z)[8:16]
        before = self.exec_child(z, b"child", seed=3)
        child = self.running(z)
        self.assertEqual(self.kword(z, "_kos_far_current"), 0)
        # The child: an object, its own far window, a grandchild.
        c = self.alloc(z, 0x2000, MAPPABLE)
        self.write(z, c, 0x100, 0x3000, pattern(2, 0x80))
        self.assertEqual(self.far(z, self.map_(z, c)), (0, 0))
        child_bank = hardware(z)[8:16]
        child_before = self.exec_child(z, b"grandchild", seed=4)
        grandchild = self.running(z)
        d = self.alloc(z, 0x6000)
        self.write(z, d, 0x1FF0, 0x1F00, pattern(3, 0x40))
        self.assertEqual(self.read(z, d, 0x1FF0, 0x4000, 0x40),
                         pattern(3, 0x40))
        # Depth 4 is the last one: its EXEC is refused, nothing allocated.
        self.exec_child(z, seed=5)
        self.assertEqual(self.running(z), 3)
        owners = self.owners(z)
        self.fails(z, EXEC, le16(0x0600) + le16(1) + le16(0) + le16(0),
                   EBUSY)
        self.assertEqual(self.owners(z), owners)
        self.exit(z, 0x4444)
        self.assertEqual(self.running(z), grandchild)
        self.exit(z, 0x3333)
        self.assertEqual(self.running(z), child)
        self.assert_preserved(z, child_before)
        self.assertEqual(z.h << 8 | z.l, 0x3333)
        self.assertEqual(hardware(z)[8:16], child_bank)
        self.assertEqual(mirror(z), hardware(z))
        self.assertEqual(self.kword(z, "_kos_far_current"),
                         self.map_(z, c))
        self.assertEqual(self.get(z, 0x8100, 0x80), pattern(2, 0x80))
        self.exit(z, 0x2222)
        self.assertEqual(self.running(z), 0)
        self.assert_preserved(z, before)
        self.assertEqual(z.h << 8 | z.l, 0x2222)
        self.assertEqual(hardware(z)[8:16], parent_bank)
        self.assertEqual(hardware(z)[16], CTL_WORKING)
        self.assertEqual(hardware(z)[0], SYSTEM_PAGE0)
        self.assertEqual(mirror(z), hardware(z))
        self.assertEqual(self.kword(z, "_kos_far_current"), mapping)
        self.assertEqual(self.get(z, 0x8000, 0x400), pattern(1, 0x400))
        # Every page of the three children is free again.
        self.assertEqual(sorted(self.owned(z, 0)),
                         sorted(list(self.image(z, 0)[:7]) +
                                list(hardware(z)[12:14])))
        self.assertEqual(self.far(z, 0), (0, mapping))
        self.assertEqual(cached(z), self.image_addr(0))

    def test_exec_without_pages_is_refused_whole(self):
        z = self.boot()
        while self.free_pages(z) >= 7:
            self.alloc(z, 0xE000)
        owners = self.owners(z)
        states = self.kbytes(z, "_process_table", 70)[0::10]
        self.fails(z, EXEC, le16(0x0600) + le16(1) + le16(0) + le16(0),
                   ENOSPC)
        self.assertEqual(self.owners(z), owners)
        self.assertEqual(self.kbytes(z, "_process_table", 70)[0::10],
                         states)
        # All or none: a larger object than what is free takes nothing.
        free = self.free_pages(z)
        self.fails(z, ALLOC, le16((free + 1) * PAGE) + le16(0), ENOSPC)
        self.assertEqual(self.free_pages(z), free)


class TestObjects(Mk2ProcCase):
    def test_data_survives_and_is_correct_through_read_write_and_map(self):
        z = self.boot()
        a = self.alloc(z, 20000)                       # three pages
        record = 0x0700
        self.ok(z, INFO, le16(a) + le16(record))
        self.assertEqual(self.get(z, record, 8),
                         le16(20000) + le16(0x6000) + bytes([0, 0, 1, 0]))
        # Across a caller page boundary (3000h) and an object page
        # boundary (offset 2000h).
        data = pattern(5, 0x400)
        self.write(z, a, 0x1F00, 0x2F00, data)
        self.write(z, a, 0x5C00, 0x6000, pattern(6, 0x400))   # last page
        b = self.alloc(z, 0x3000, MAPPABLE)
        self.write(z, b, 0, 0x1000, pattern(7, 0x3000))
        self.assertEqual(self.read(z, a, 0x1F00, 0x5000, 0x400), data)
        self.assertEqual(self.read(z, a, 0x5C00, 0x7000, 0x400),
                         pattern(6, 0x400))
        # MAP: the window shows the object at 8000h.
        mapping = self.map_(z, b)
        own = hardware(z)[12:14]
        self.assertEqual(self.far(z, mapping), (0, 0))
        window = hardware(z)[12:14]
        self.assertNotEqual(window, own)
        self.assertEqual(cached(z) >> 8, 0)            # no image matches
        self.assertEqual(mirror(z), hardware(z))
        self.assertEqual(self.get(z, 0x8000, 0x3000), pattern(7, 0x3000))
        self.assertEqual(bytes(z.rd(0x8000 + i) for i in range(16)),
                         pattern(7, 16))
        # Plain pointers write the object; a READ sees it.
        self.put(z, 0x0800, pattern(8, 0x100))
        self.copy(z, 0x0800, 0x9F80, 0x100)            # across 2 pages
        # Buffer identity: a buffer in the open window is the object.
        self.assertEqual(self.read(z, a, 0x1F00, 0x8400, 0x40), data[:64])
        self.write(z, a, 0x100, 0x7F80, self.get(z, 0x7F80, 0x100))
        self.assertEqual(self.far(z, 0), (0, mapping))
        self.assertEqual(hardware(z)[12:14], own)
        self.assertEqual(cached(z), self.image_addr(0))
        got = self.read(z, b, 0, 0x1000, 0x3000)
        expect = bytearray(pattern(7, 0x3000))
        expect[0x1F80:0x2080] = pattern(8, 0x100)
        expect[0x400:0x440] = data[:64]
        self.assertEqual(got, bytes(expect))
        # A one-page object: the window's upper half is the program's own.
        c = self.alloc(z, 0x1000, MAPPABLE)
        self.assertEqual(self.far(z, self.map_(z, c))[0], 0)
        self.assertEqual(hardware(z)[13], own[1])
        self.assertNotEqual(hardware(z)[12], own[0])
        self.far(z, 0)

    def test_freeing_the_object_the_window_shows_closes_the_window(self):
        """XMEM_FREE of the object in the open far window: the window
        shows the program's own pages again on return, the handle in
        force is 0, the working bank holds the program's image and the
        freed object's mapping is refused.  Freeing another object
        leaves an open window as it is."""
        z = self.boot()
        own = hardware(z)[12:14]
        other = self.alloc(z, 0x2000, MAPPABLE)
        shown = self.alloc(z, 0x4000, MAPPABLE)
        other_mapping = self.map_(z, other)
        mapping = self.map_(z, shown)
        self.assertEqual(self.far(z, mapping), (0, 0))
        window = hardware(z)[12:14]
        self.assertNotEqual(window, own)
        self.ok(z, FREE, le16(other))
        self.assertEqual(hardware(z)[12:14], window)
        self.assertEqual(self.kword(z, "_kos_far_current"), mapping)
        self.ok(z, FREE, le16(shown))
        self.assertEqual(hardware(z)[12:14], own)
        self.assertEqual(self.kword(z, "_kos_far_current"), 0)
        self.assertEqual(cached(z), self.image_addr(0))
        self.assert_program_context(z, 0)
        for handle in (mapping, other_mapping):
            with self.subTest(mapping=handle):
                self.assertEqual(self.far(z, handle), (1, handle))
                self.assertEqual(hardware(z)[12:14], own)

    def test_invalid_handles_and_ranges_are_refused(self):
        z = self.boot()
        a = self.alloc(z, 0x2000)
        m = self.alloc(z, 0x2000, MAPPABLE)
        mapping = self.map_(z, m)
        for handle in (0, 0x1FF, 0x200, a ^ 0x0F):
            with self.subTest(handle=handle):
                self.fails(z, FREE, le16(handle), EINVAL)
                self.fails(z, READ, le16(handle) + le16(0) + le16(0x1000)
                           + le16(1), EINVAL)
        self.fails(z, MAP_, le16(a) + le16(0), EINVAL)     # not mappable
        io = le16(a)
        for offset, buffer, length in ((0, 0x1000, 0x2001),
                                       (0x2000, 0x1000, 1),
                                       (0x2001, 0x1000, 0),
                                       (0x1FFF, 0x1000, 2),
                                       (0, 0xDF00, 0x101),
                                       (0, 0x0080, 1),
                                       (0, 0xFFF0, 0x20)):
            with self.subTest(offset=offset, buffer=buffer, length=length):
                self.fails(z, READ, io + le16(offset) + le16(buffer) +
                           le16(length), EINVAL)
                self.fails(z, WRITE, io + le16(offset) + le16(buffer) +
                           le16(length), EINVAL)
        self.fails(z, INFO, le16(a) + le16(0xDFFC), EINVAL)
        for size, flags, error in ((0, 0, EINVAL), (0x100, 2, EINVAL),
                                   (0x4001, MAPPABLE, EINVAL),
                                   (0xE001, 0, ENOSPC)):
            with self.subTest(size=size, flags=flags):
                self.fails(z, ALLOC, le16(size) + le16(flags), error)
        self.fails(z, SEG_INFO, bytes(4), EINVAL)        # no segment 0
        # A stale handle and its mapping: refused, the window untouched.
        self.ok(z, FREE, le16(m))
        self.fails(z, MAP_, le16(m) + le16(0), EINVAL)
        bank = hardware(z)
        # One-byte handles (row << 3) | generation: a high byte, and the
        # row with generation 0.
        self.assertTrue(mapping < 0x100 and mapping & 0xF8)
        for handle in (mapping, 0x0100 | mapping, mapping & 0xF8):
            with self.subTest(mapping=handle):
                carry, hl = self.far(z, handle)
                self.assertEqual((carry, hl), (1, handle))
                self.assertEqual(hardware(z), bank)
        # A foreign handle: the parent's object is not the child's.
        self.exec_child(z)
        self.fails(z, FREE, le16(a), EINVAL)
        self.fails(z, READ, le16(a) + le16(0) + le16(0x1000) + le16(1),
                   EINVAL)
        self.exit(z, 0)
        self.ok(z, FREE, le16(a))


class TestMappingRules(Mk2ProcCase):
    """The NMI rule (architecture 2.7.6) on the page paths - exec, exit,
    the copy windows, the far window: the mirrors are written before each
    mapper OUT, and RT ticks across them change nothing."""

    def trace(self, z, start, done):
        start(z)
        trace = [(hardware(z), mirror(z))]
        while not done(z):
            z.step()
            trace.append((hardware(z), mirror(z)))
            self.assertLess(len(trace), 400000)
        return trace

    def assert_ordering(self, name, trace):
        changes = [i for i in range(1, len(trace))
                   if trace[i][0] != trace[i - 1][0]]
        self.assertTrue(changes, "%s changes no mapping" % name)
        nxt = iter(changes + [None])
        upcoming = next(nxt)
        for b, (hw, mir) in enumerate(trace):
            while upcoming is not None and upcoming <= b:
                upcoming = next(nxt)
            diff = [r for r in range(17) if hw[r] != mir[r]]
            if upcoming is None:
                self.assertEqual(diff, [], "%s: boundary %d" % (name, b))
                continue
            new = trace[upcoming][0]
            for r in diff:
                self.assertEqual(mir[r], new[r],
                                 "%s: boundary %d register %d"
                                 % (name, b, r))

    def start_call(self, number, block, stop):
        def start(z):
            self.put(z, ARGS, block)
            z.a = number
            z.h, z.l = ARGS >> 8, ARGS & 0xFF
            z.pc = self.sym("p_call")
            z.iff1 = z.iff2 = True
        target = self.sym(stop)
        return start, lambda z: z.pc == target

    def test_every_page_path_orders_mirror_before_out(self):
        z = self.boot()
        b = self.alloc(z, 0x4000, MAPPABLE)
        mapping = self.map_(z, b)
        exec_block = le16(0x0600) + le16(1) + le16(0) + le16(0)
        far_done = self.sym("p_far_done")

        def far_start(handle):
            def start(z):
                z.h, z.l = handle >> 8, handle & 0xFF
                z.pc = self.sym("p_far")
            return start

        sequences = (
            ("far window open", far_start(mapping),
             lambda z: z.pc == far_done),
            ("read through both windows", ) + self.start_call(
                READ, le16(b) + le16(0x1F00) + le16(0x2F00) +
                le16(0x200), "p_return"),
            ("exec", ) + self.start_call(EXEC, exec_block, "p_idle"),
            ("exit", ) + self.start_call(EXIT, le16(7), "p_return"),
            ("far window close", far_start(0),
             lambda z: z.pc == far_done),
        )
        for name, start, done in sequences:
            with self.subTest(sequence=name):
                self.assert_ordering(name, self.trace(z, start, done))
        self.assert_program_context(z, 0)

    def test_an_unchanged_image_is_not_reloaded(self):
        z = self.boot()
        start, done = self.start_call(FREE, le16(0), "p_return")
        trace = self.trace(z, start, done)
        self.assertEqual({hw[8:16] for hw, _ in trace},
                         {tuple(self.image(z, 0))})
        self.assertEqual(cached(z), self.image_addr(0))

    def test_rt_ticks_change_nothing(self):
        z = self.boot()
        z.rt_hz = 20000
        owners = self.owners(z)
        state = self.state(z)
        for i in range(6):
            before = self.exec_child(z, b"tick", seed=i)
            m = self.alloc(z, 0x4000, MAPPABLE)
            self.write(z, m, 0x1F80, 0x1F00, pattern(i, 0x100))
            self.far(z, self.map_(z, m))
            self.assertEqual(self.get(z, 0x9F80, 0x100), pattern(i, 0x100))
            self.far(z, 0)
            self.exit(z, i)
            self.assert_preserved(z, before)
            self.assertEqual(self.state(z), state)
            self.assertEqual(self.owners(z), owners)
        self.assertGreater(z.nmi_count, 20)
        self.assertEqual(self.kword(z, "t_rt_count"), z.nmi_count)


if __name__ == "__main__":
    unittest.main()
