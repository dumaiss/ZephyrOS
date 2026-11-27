#!/usr/bin/env python3
"""The Zephyr-80 Mk2 production nucleus on the simulator's Mk2 board.

The image (`make BOARD=mk2`, build-mk2/konaos-mk2.bin) is the whole
nucleus built for Mk2 (architecture 2.7.6): the bootstrap in ROM page 00h
programs both DAT banks in pass mode, copies the nucleus pages, cold set 1
and the common page from ROM to SRAM through the copy windows, builds the
RAM system page-0 image and hands off to kernel_main from common code.
The nucleus prints its banner, initializes the ROMFS reader over the
store's ROM pages (read through the source copy window), loads the
drivers into the driver domain and starts the shell
(tests/test_mk2_shell.py drives it).

The tests read the machine back: the early-console transcript, the DAT
state at the handoff, after irq_init and at the shell's idle, a CTC tick
taken and counted before the drivers claim the CTC, ROMFS reads against
the packed image, and tools/mapcheck.py's Mk2 rules - the regions, the
cold sets and their veneers - with their negative cases.

Skipped when the image has not been built.
"""

import os
import re
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import mapcheck  # noqa: E402
import z80sim  # noqa: E402
from z80sim import Ctc, IocCommandPeer, StopSet, V9958Peer, Z80  # noqa: E402

BUILD = os.path.join(ROOT, "build-mk2")
IMAGE = os.path.join(BUILD, "konaos-mk2.bin")
MAP = os.path.join(BUILD, "konaos-mk2.map")
RESIDENCY = os.path.join(ROOT, "config", "residency-mk2.json")
MK1_RESIDENCY = os.path.join(ROOT, "config", "residency.json")
PLAN = os.path.join(BUILD, "sys", "modules", "drivers.plan.json")
LIMIT = 4000000
BOOT_LIMIT = 40000000

PAGE = 0x2000
SYSTEM_PAGE0, COMMON_PAGE = 0x40, 0x7F
NUCLEUS_PAGES = (0x41, 0x42, 0x43, 0x44)   # memory.inc, entries 1-4
SET1_PAGES, SET1_ROM = (0x45, 0x46), 0x05   # memory.inc, cold set 1
SYSTEM_DAT = (SYSTEM_PAGE0,) + NUCLEUS_PAGES + (0x00, 0x00, COMMON_PAGE)
CTL_SYSTEM, CTL_WORKING = 0x02, 0x03
DAT_STATE, DAT_CACHED = 0xEF10, 0xEF21
RT_VECTOR = 0xEF08
ROMFS_FIRST_PAGE = 0x08
CTC2_PORT, CTC2_VECTOR = 0x41, 0x04
IRQ_CTC2 = 2

TRANSCRIPT = (b"KonaOS 0.0\r\n"
              b"platform: Zephyr-80 Mk2\r\n"
              b"kernel entry: OK\r\n"
              b"serial: OK\r\n")


def common(addr):
    return COMMON_PAGE * PAGE | (addr & 0x1FFF)


def nucleus(addr):
    """A nucleus address (logical pages 1-4) as a physical address."""
    return NUCLEUS_PAGES[(addr >> 13) - 1] * PAGE | (addr & 0x1FFF)


def hardware(z):
    m = z.mapper
    return tuple(m.entries) + ((m.map_enable << 1) | m.dat_bank,)


def fixed_entries(z):
    """The system entries that never change outside a call (0-2 and 7),
    and working entry 15: entries 3-4 show the cold set or the driver
    domain the last call mapped, 5-6 the copy windows or the domain;
    None gives the boot's."""
    state = hardware(z) if z is not None else \
        SYSTEM_DAT + SYSTEM_DAT + (CTL_SYSTEM,)
    return state[0:3] + state[7:8] + state[15:16]


def mirror(z):
    return tuple(z.phys[common(DAT_STATE):common(DAT_STATE) + 17])


def cached(z):
    return z.phys[common(DAT_CACHED)] | (z.phys[common(DAT_CACHED) + 1] << 8)


@unittest.skipUnless(os.path.isfile(IMAGE), "make BOARD=mk2 builds the image")
class Mk2NucleusCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(IMAGE, "rb") as handle:
            cls.rom = handle.read()
        with open(MAP) as handle:
            cls.map_text = handle.read()
        cls.syms = z80sim.read_symbols(MAP)

    def machine(self, ctc=False):
        """The board with the production peers the drivers bring up (the
        V9958 and the IOC; the CTC on request)."""
        z = Z80(self.rom, b"", board="mk2")
        z.vdp_peer = V9958Peer()
        z.hid = z.ioc_peer = IocCommandPeer()
        if ctc:
            Ctc().attach(z)
        return z

    def run_to(self, z, name):
        reason, _ = z.run(LIMIT, stops=StopSet([self.syms[name]]),
                          check_first=False)
        self.assertEqual(reason, "stop", "%s never reached" % name)

    def boot(self, z):
        """Run to the shell's idle HALT."""
        reason, _ = z.run(BOOT_LIMIT, halt=True)
        self.assertEqual(reason, "halt")
        return z

    def to_driver_load(self, z):
        """Run to the driver load: the system bank selected, the ROMFS
        reader initialized, no driver loaded yet."""
        self.run_to(z, "__cold_driver_load_all")
        return z

    def call(self, z, name, hl=0, de=0, stack=()):
        """Run one nucleus function from the halted machine: sdcccall(1)
        arguments in HL and DE, the rest on the stack, the return into a
        HALT in the free gate stack.  Returns DE."""
        scratch = self.syms["kos_gate_stack"]
        stop = scratch + 0x170
        z.phys[common(stop)] = 0x76
        words = [stop] + list(stack)
        sp = (0x10000 - 2 * len(words)) & 0xFFFF
        for index, word in enumerate(words):
            at = common(sp + 2 * index)
            z.phys[at] = word & 0xFF
            z.phys[at + 1] = word >> 8
        z.sp = sp
        z.hl, z.de = hl, de
        z.pc = self.syms[name]
        z.halted = False
        reason, _ = z.run(LIMIT, stops=StopSet([stop]), check_first=False)
        self.assertEqual(reason, "stop", "%s did not return" % name)
        return z.de


class TestBoot(Mk2NucleusCase):
    def test_boot_transcript_to_the_shell(self):
        z = self.boot(self.machine())
        self.assertEqual(bytes(z.sio_tx), TRANSCRIPT)
        # The shell idles in its own context: the working bank, its image.
        self.assertEqual(hardware(z)[16], CTL_WORKING)
        self.assertTrue(z.iff1)

    def test_handoff_state(self):
        z = self.machine()
        self.run_to(z, "_kernel_main")
        self.assertEqual(hardware(z), SYSTEM_DAT + SYSTEM_DAT + (CTL_SYSTEM,))
        self.assertEqual(mirror(z), hardware(z))
        self.assertEqual(cached(z), 0)
        self.assertFalse(z.iff1)
        self.assertGreaterEqual(z.sp, 0xE000)
        # The nucleus's loaded bytes are the ROM's, copied page by page
        # (the runtime data after each code run is the C runtime's).
        for start, end in ((0x2000, self.syms["s__DATA"]),
                           (0x6000, self.syms["s__WINDOW_DATA"])):
            copied = bytes(z.phys[nucleus(a)] for a in range(start, end))
            self.assertEqual(copied, self.rom[start:end])
        # Cold set 1: ROM pages 05h-06h in SRAM 45h-46h, up to its data.
        end = self.syms["s__COLD1_DATA"] & 0xFFFF
        base = SET1_PAGES[0] * PAGE
        self.assertEqual(bytes(z.phys[base:base + end - 0x6000]),
                         self.rom[SET1_ROM * PAGE:SET1_ROM * PAGE +
                                  end - 0x6000])
        code_end = self.syms["s__COMMON_DATA"]
        self.assertEqual(
            bytes(z.phys[COMMON_PAGE * PAGE:common(code_end)]),
            self.rom[7 * PAGE:code_end])
        # The RAM system page-0 image: the low stubs and nothing else.
        page0 = bytearray(0x100)
        entry = self.syms["kos_syscall_entry"]
        page0[0x30:0x33] = bytes([0xC3, entry & 0xFF, entry >> 8])
        page0[0x66:0x69] = bytes([0xC3, RT_VECTOR & 0xFF, RT_VECTOR >> 8])
        base = SYSTEM_PAGE0 * PAGE
        self.assertEqual(bytes(z.phys[base:base + 0x100]), bytes(page0))

    def test_mapping_after_irq_init(self):
        z = self.machine()
        self.run_to(z, "_vfs_init")
        self.assertEqual(hardware(z), SYSTEM_DAT + SYSTEM_DAT + (CTL_SYSTEM,))
        self.assertEqual(mirror(z), hardware(z))
        self.assertTrue(z.iff1)
        self.assertEqual((z.im, z.i), (2, 0xFD))
        self.boot(z)
        self.assertEqual(fixed_entries(z), fixed_entries(None))
        self.assertEqual(mirror(z), hardware(z))

    def test_a_ctc_tick_is_taken_and_counted(self):
        z = self.machine(ctc=True)
        self.run_to(z, "_vfs_init")
        # Channel 2 as the timer driver programs it, armed from outside:
        # nothing in this image claims its vector.
        z.ctc.write(0x40, 0x00)
        z.ctc.write(CTC2_PORT, 0xA7)
        z.ctc.write(CTC2_PORT, 0x40)
        # Up to the driver load: the timer driver claims the CTC there.
        self.to_driver_load(z)
        self.assertEqual(bytes(z.sio_tx), TRANSCRIPT)
        delivered = z.ctc.delivered[IRQ_CTC2]
        self.assertGreater(delivered, 0)
        self.assertEqual(fixed_entries(z), fixed_entries(None))
        self.assertEqual(mirror(z), hardware(z))
        out = self.syms["kos_gate_stack"]
        self.call(z, "_irq_diag_counters", hl=out)
        counters = z.phys[common(out):common(out) + 5]
        unexpected = counters[0] | (counters[1] << 8)
        unclaimed = counters[2] | (counters[3] << 8)
        self.assertEqual(unexpected + unclaimed, delivered)


class TestRomfs(Mk2NucleusCase):
    def lookup(self, z, name):
        scratch = self.syms["kos_gate_stack"]
        text, record = scratch + 0x140, scratch + 0x100
        data = name.encode("ascii") + b"\0"
        z.phys[common(text):common(text) + len(data)] = data
        self.assertEqual(self.call(z, "_romfs_lookup_ci", hl=text,
                                   de=record), 0)
        raw = z.phys[common(record):common(record) + 10]
        offset = int.from_bytes(raw[0:4], "little")
        size = int.from_bytes(raw[4:8], "little")
        return record, offset, size

    def test_reads_cross_rom_pages_and_match_the_image(self):
        z = self.to_driver_load(self.machine())
        record, offset, size = self.lookup(z, "SHELL.EXE")
        store = ROMFS_FIRST_PAGE * PAGE
        # A read that crosses the next 8 KiB page boundary of the store.
        boundary = (offset // PAGE + 1) * PAGE
        at = boundary - offset - 0x80
        self.assertLess(at + 0x100, size)
        buf = self.syms["kos_gate_stack"]
        got = self.call(z, "_romfs_read", hl=record,
                        stack=(at & 0xFFFF, at >> 16, buf, 0x100))
        self.assertEqual(got, 0x100)
        logical = offset + at
        self.assertEqual(bytes(z.phys[common(buf):common(buf) + 0x100]),
                         self.rom[store + logical:store + logical + 0x100])
        # The copy windows hold whatever the last copy mapped; the rest of
        # the mapping is untouched.
        self.assertEqual(fixed_entries(z), fixed_entries(None))
        self.assertEqual(mirror(z), hardware(z))

    def test_rom_access_copy_reads_a_store_page(self):
        z = self.to_driver_load(self.machine())
        req = self.syms["kos_gate_stack"] + 0x100
        page, offset, length = ROMFS_FIRST_PAGE + 1, 0x1F90, 0x70
        z.phys[common(req):common(req) + 5] = bytes(
            [page, offset & 0xFF, offset >> 8, length, 0])
        z.a = 0xFF
        self.call(z, "_rom_access_copy", hl=req)
        self.assertEqual(z.a, 0)
        staging = nucleus(self.syms["_rom_access_staging"])
        self.assertEqual(bytes(z.phys[staging:staging + length]),
                         self.rom[page * PAGE + offset:
                                  page * PAGE + offset + length])


class TestPageCopy(Mk2NucleusCase):
    """The real linked kos_page_copy, called from the halted machine at the
    driver load (system bank selected), with the request in common memory."""

    LENGTHS = (1, 15, 16, 17, 511, 512, 513, 8192)
    SRC_PAGE, DST_PAGE, ROM_PAGE = 0x50, 0x52, ROMFS_FIRST_PAGE
    DIRECT = 0x6000              # nucleus entries 3-4: 16 KiB of RAM
    ZEROES, DIRECT_KIND = 0x80, 0xFF
    CANARY = 0xA5

    def machine_at_driver_load(self):
        return self.to_driver_load(self.machine())

    def request(self, z, src_page, dst_page, src, dst, length):
        req = self.syms["kos_gate_stack"] + 0x100
        z.phys[common(req):common(req) + 8] = bytes(
            [src_page, dst_page, src & 0xFF, src >> 8, dst & 0xFF,
             dst >> 8, length & 0xFF, length >> 8])
        return req

    def place(self, z, page, offset, data):
        base = page * PAGE + offset
        z.phys[base:base + len(data)] = data

    def fetch(self, z, page, offset, length):
        base = page * PAGE + offset
        return bytes(z.phys[base:base + length])

    def run_copy(self, z, src_page, dst_page, src, dst, length):
        fixed = fixed_entries(z)
        self.call(z, "_kos_page_copy",
                  hl=self.request(z, src_page, dst_page, src, dst, length))
        self.assertEqual(fixed_entries(z), fixed)
        self.assertEqual(mirror(z), hardware(z))

    def test_page_to_page_lengths_with_canaries(self):
        z = self.machine_at_driver_load()
        for length in self.LENGTHS:
            data = bytes((i * 7 + length) & 0xFF for i in range(length))
            src_off = 0 if length == 8192 else 0x0123 % (0x2000 - length)
            dst_off = 0 if length == 8192 else 0x0FFF % (0x2000 - length)
            with self.subTest(length=length):
                self.place(z, self.SRC_PAGE, 0, bytes([0x11]) * 0x2000)
                self.place(z, self.SRC_PAGE, src_off, data)
                self.place(z, self.DST_PAGE, 0,
                           bytes([self.CANARY]) * 0x2000)
                self.run_copy(z, self.SRC_PAGE, self.DST_PAGE, src_off,
                              dst_off, length)
                want = bytearray([self.CANARY]) * 0x2000
                want[dst_off:dst_off + length] = data
                self.assertEqual(self.fetch(z, self.DST_PAGE, 0, 0x2000),
                                 bytes(want))

    def test_zero_fill_lengths_with_canaries(self):
        z = self.machine_at_driver_load()
        base = nucleus(self.DIRECT)
        for length in self.LENGTHS:
            off = 0 if length == 8192 else 0x00F1 % (0x2000 - length)
            want = bytearray([0xFF]) * 0x2000
            want[off:off + length] = bytes(length)
            with self.subTest(length=length, target="page"):
                self.place(z, self.DST_PAGE, 0, bytes([0xFF]) * 0x2000)
                self.run_copy(z, self.ZEROES, self.DST_PAGE, 0, off, length)
                self.assertEqual(self.fetch(z, self.DST_PAGE, 0, 0x2000),
                                 bytes(want))
            with self.subTest(length=length, target="direct"):
                z.phys[base:base + 0x2000] = bytes([0xFF]) * 0x2000
                self.run_copy(z, self.ZEROES, self.DIRECT_KIND, 0,
                              self.DIRECT + off, length)
                self.assertEqual(bytes(z.phys[base:base + 0x2000]),
                                 bytes(want))

    def test_direct_and_page_combinations(self):
        z = self.machine_at_driver_load()
        base = nucleus(self.DIRECT)
        for length in self.LENGTHS:
            data = bytes((i * 5 + 3 * length) & 0xFF for i in range(length))
            at = 0 if length == 8192 else 0x10
            with self.subTest(length=length, kind="direct to page"):
                z.phys[base:base + length] = data
                self.place(z, self.DST_PAGE, 0,
                           bytes([self.CANARY]) * 0x2000)
                self.run_copy(z, self.DIRECT_KIND, self.DST_PAGE,
                              self.DIRECT, at, length)
                want = bytearray([self.CANARY]) * 0x2000
                want[at:at + length] = data
                self.assertEqual(self.fetch(z, self.DST_PAGE, 0, 0x2000),
                                 bytes(want))
            with self.subTest(length=length, kind="page to direct"):
                self.place(z, self.SRC_PAGE, 0, data)
                z.phys[base:base + 0x2000 + 1] = \
                    bytes([self.CANARY]) * 0x2001
                self.run_copy(z, self.SRC_PAGE, self.DIRECT_KIND, 0,
                              self.DIRECT + at, length)
                want = bytearray([self.CANARY]) * 0x2000
                want[at:at + length] = data
                self.assertEqual(bytes(z.phys[base:base + 0x2000]),
                                 bytes(want))
            with self.subTest(length=length, kind="direct to direct"):
                z.phys[base:base + length] = data
                top = base + 0x2000
                z.phys[top:top + length + 1] = \
                    bytes([self.CANARY]) * (length + 1)
                self.run_copy(z, self.DIRECT_KIND, self.DIRECT_KIND,
                              self.DIRECT, self.DIRECT + 0x2000, length)
                self.assertEqual(bytes(z.phys[top:top + length]), data)
                self.assertEqual(z.phys[top + length], self.CANARY)

    def test_rom_source_reaches_page_and_direct(self):
        z = self.machine_at_driver_load()
        for length in (1, 17, 513, 8192):
            want = self.rom[self.ROM_PAGE * PAGE:
                            self.ROM_PAGE * PAGE + length]
            with self.subTest(length=length, kind="page"):
                self.place(z, self.DST_PAGE, 0,
                           bytes([self.CANARY]) * 0x2000)
                self.run_copy(z, self.ROM_PAGE, self.DST_PAGE, 0, 0, length)
                self.assertEqual(self.fetch(z, self.DST_PAGE, 0, length),
                                 want)
                self.assertEqual(
                    self.fetch(z, self.DST_PAGE, length, 0x2000 - length),
                    bytes([self.CANARY]) * (0x2000 - length))
            with self.subTest(length=length, kind="direct"):
                self.run_copy(z, self.ROM_PAGE, self.DIRECT_KIND, 0,
                              self.DIRECT, length)
                base = nucleus(self.DIRECT)
                self.assertEqual(bytes(z.phys[base:base + length]), want)

    def test_interrupt_flags_survive(self):
        z = self.machine_at_driver_load()
        self.place(z, self.SRC_PAGE, 0, bytes(range(256)) * 2)
        for flag in (False, True):
            z.iff1 = z.iff2 = flag
            self.run_copy(z, self.SRC_PAGE, self.DST_PAGE, 0, 0, 512)
            self.assertEqual((z.iff1, z.iff2), (flag, flag))


class TestMapcheck(Mk2NucleusCase):
    def residency(self):
        return mapcheck.load_mk2_residency(RESIDENCY)

    def check(self, map_text, residency=None, objdir=BUILD, plan=None):
        tiers, _cold, _test = mapcheck.load_residency(MK1_RESIDENCY)
        return mapcheck.check_mk2(map_text, residency or self.residency(),
                                  ROOT, objdir, tiers, plan_path=plan)

    def link(self, residency):
        with tempfile.TemporaryDirectory() as out:
            return mapcheck.mk2_cold_link(residency, BUILD, out)

    def test_the_image_passes(self):
        ledger = self.check(self.map_text)
        self.assertGreaterEqual(ledger["fixed_room"], 0)
        self.assertEqual(len(ledger["set_room"]), 2)
        for room in ledger["set_room"]:
            self.assertGreaterEqual(room, 0)
        self.assertLessEqual(ledger["common_used"], ledger["common_ceiling"])

    @unittest.skipUnless(os.path.isfile(PLAN), "make BOARD=mk2 plans the "
                         "driver set")
    def test_no_stack_lies_under_a_remap(self):
        """The driver copies run on the nucleus's own stack, so the V9958
        driver's private stack may lie in the copy windows; a copy on the
        caller's stack, or on a stack the map does not define, fails."""
        ledger = self.check(self.map_text, plan=PLAN)
        self.assertGreaterEqual(ledger["driver_stacks"], 1)
        residency = self.residency()
        for row in residency.remaps:
            if row["entries"] == [5, 6]:
                row["stack"] = "caller"
        with self.assertRaisesRegex(
                mapcheck.MapError,
                "v9958.mod private stack _v9958_alt_stack .* system entry "
                "5, which .* remaps on the caller's stack"):
            self.check(self.map_text, residency, plan=PLAN)
        text = re.sub(r".*\bkos_driver_copy_stack(_top)?\b.*\n", "",
                      self.map_text)
        self.assertNotEqual(text, self.map_text)
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "kos_driver_copy_stack, which the map "
                                    "does not define"):
            self.check(text, plan=PLAN)

    def test_a_nucleus_stack_in_a_remapped_entry_fails(self):
        text = self.map_text + ("     00007000  stray_stack    stray\n"
                                "     00007010  stray_stack_top    stray\n")
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "nucleus stack stray_stack .* entry 3"):
            self.check(text)

    def test_an_object_in_a_copy_window_fails(self):
        text = self.map_text + \
            "_STRAY                              0000B000    00000010 =  " \
            "16. bytes (REL,CON)\n"
        with self.assertRaisesRegex(mapcheck.MapError, "copy windows"):
            self.check(text)
        text = self.map_text + "     0000C123  _stray_symbol    stray\n"
        with self.assertRaisesRegex(mapcheck.MapError, "copy windows"):
            self.check(text)

    def test_a_table_size_mismatch_fails(self):
        end = self.syms["_kos_far_table_end"]
        text = re.sub(r"%08X(\s+_kos_far_table_end)" % end,
                      lambda m: "%08X%s" % (end + 3, m.group(1)),
                      self.map_text)
        self.assertNotEqual(text, self.map_text)
        with self.assertRaisesRegex(mapcheck.MapError, "XMEM_MAX_OBJECTS"):
            self.check(text)

    def test_a_fixed_required_object_cannot_be_in_a_set(self):
        residency = self.residency()
        residency.sets[1].append("ipc")
        with self.assertRaisesRegex(mapcheck.MapError, "fixed-required"):
            self.check(self.map_text, residency, objdir=None)

    def test_a_set_past_its_pages_fails(self):
        start = self.syms["s__COLD1"]
        text = re.sub(r"(\s+)%08X(\s+s__COLD1\b)" % start,
                      lambda m: "%s%08X%s" % (m.group(1), 0x19F00,
                                              m.group(2)),
                      self.map_text)
        self.assertNotEqual(text, self.map_text)
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "cold set 1 .* outside"):
            self.check(text)

    def test_an_undeclared_set_fails(self):
        text = self.map_text + \
            "_COLD2                              00026000    00000010 =  " \
            "16. bytes (REL,CON)\n"
        with self.assertRaisesRegex(mapcheck.MapError, "undeclared"):
            self.check(text)

    def test_every_crossing_into_a_set_has_a_veneer(self):
        """The dispatcher (fixed) reaches the object bodies in set 0 and
        the extended-memory bodies in set 1 through veneers that name the
        target and the set's pages."""
        veneers = {veneer: (symbol, where)
                   for veneer, symbol, where in self.link(self.residency())}
        self.assertEqual(veneers["__cold_kos_object_dispatch"],
                         ("_kos_object_dispatch", 0))
        self.assertEqual(veneers["__cold_kos_xmem_dispatch"],
                         ("_kos_xmem_dispatch", 1))
        z = self.machine()
        self.run_to(z, "_kernel_main")
        at = self.syms["__cold_kos_xmem_dispatch"]
        target = self.syms["_kos_xmem_dispatch"] & 0xFFFF
        call = self.syms["kos_cold_call"]
        self.assertEqual(
            bytes(z.phys[nucleus(at):nucleus(at) + 7]),
            bytes([0xCD, call & 0xFF, call >> 8, target & 0xFF,
                   target >> 8]) + bytes(SET1_PAGES))

    def test_set_data_seen_from_outside_the_set_fails(self):
        """vfs_router (set 0) and vfs_dev read each other's data: in
        different sets one side's data is unmapped while the other runs."""
        residency = self.residency()
        residency.sets[0].remove("vfs_dev")
        residency.sets[1].append("vfs_dev")
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "data of cold set [01]"):
            self.link(residency)

    def test_driver_reachable_code_cannot_reach_a_set(self):
        residency = self.residency()
        residency.driver_api.append("syscall")
        with self.assertRaisesRegex(mapcheck.MapError,
                                    "syscall \\(driver_api\\)"):
            self.link(residency)

    def test_an_unplaced_object_fails_the_link(self):
        residency = self.residency()
        residency.sets[1].remove("rt")
        with self.assertRaisesRegex(mapcheck.MapError, "unplaced.*rt"):
            self.link(residency)


if __name__ == "__main__":
    unittest.main()
