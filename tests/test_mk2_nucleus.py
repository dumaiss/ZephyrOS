#!/usr/bin/env python3
"""The Zephyr-80 Mk2 production nucleus on the simulator's Mk2 board.

The image (`make BOARD=mk2`, build-mk2/konaos-mk2.bin) is the whole
nucleus built for Mk2 (architecture 2.7.6): the bootstrap in ROM page 00h
programs both DAT banks in pass mode, copies the nucleus and common pages
from ROM to SRAM through the copy windows, builds the RAM system page-0
image and hands off to kernel_main from common code.  The nucleus prints
its banner, initializes the ROMFS reader over the store's ROM pages (read
through the source copy window) and runs the driver loader, which refuses
the first image: the Mk2 system DAT maps no driver domain.

The tests read the machine back: the boot transcript, the DAT state at the
handoff and after irq_init, a CTC tick taken and counted, ROMFS reads
against the packed image, and tools/mapcheck.py's Mk2 rules with their
negative cases.

Skipped when the image has not been built.
"""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import mapcheck  # noqa: E402
import z80sim  # noqa: E402
from z80sim import Ctc, StopSet, Z80  # noqa: E402

BUILD = os.path.join(ROOT, "build-mk2")
IMAGE = os.path.join(BUILD, "konaos-mk2.bin")
MAP = os.path.join(BUILD, "konaos-mk2.map")
RESIDENCY = os.path.join(ROOT, "config", "residency-mk2.json")
MK1_RESIDENCY = os.path.join(ROOT, "config", "residency.json")
LIMIT = 4000000

PAGE = 0x2000
SYSTEM_PAGE0, COMMON_PAGE = 0x40, 0x7F
NUCLEUS_PAGES = (0x41, 0x42, 0x43, 0x44)   # memory.inc, entries 1-4
SYSTEM_DAT = (SYSTEM_PAGE0,) + NUCLEUS_PAGES + (0x00, 0x00, COMMON_PAGE)
CTL_SYSTEM = 0x02
DAT_STATE, DAT_CACHED = 0xEF10, 0xEF21
RT_VECTOR = 0xEF08
ROMFS_FIRST_PAGE = 0x08
CTC2_PORT, CTC2_VECTOR = 0x41, 0x04
IRQ_CTC2 = 2

TRANSCRIPT = (b"KonaOS 0.0\r\n"
              b"platform: Zephyr-80 Mk2\r\n"
              b"kernel entry: OK\r\n"
              b"serial: OK\r\n"
              b"driver loader: sio.mod: copy\r\n"
              b"drivers: unavailable 0x07\r\n"
              b"console: driver failed\r\n")


def common(addr):
    return COMMON_PAGE * PAGE | (addr & 0x1FFF)


def nucleus(addr):
    """A nucleus address (logical pages 1-4) as a physical address."""
    return NUCLEUS_PAGES[(addr >> 13) - 1] * PAGE | (addr & 0x1FFF)


def hardware(z):
    m = z.mapper
    return tuple(m.entries) + ((m.map_enable << 1) | m.dat_bank,)


def fixed_entries(z):
    """The mapping without the copy windows (system entries 5-6), which
    hold whatever the last copy mapped; None gives the boot's."""
    state = hardware(z) if z is not None else \
        SYSTEM_DAT + SYSTEM_DAT + (CTL_SYSTEM,)
    return state[0:5] + state[7:]


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
        z = Z80(self.rom, b"", board="mk2")
        if ctc:
            Ctc().attach(z)
        return z

    def run_to(self, z, name):
        reason, _ = z.run(LIMIT, stops=StopSet([self.syms[name]]),
                          check_first=False)
        self.assertEqual(reason, "stop", "%s never reached" % name)

    def boot(self, z):
        reason, _ = z.run(LIMIT, halt=True)
        self.assertEqual(reason, "halt")
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
    def test_boot_transcript_to_the_driver_load(self):
        z = self.boot(self.machine())
        self.assertEqual(bytes(z.sio_tx), TRANSCRIPT)
        self.assertFalse(z.iff1)

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
        self.boot(z)
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
        z = self.boot(self.machine())
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
        z = self.boot(self.machine())
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


class TestMapcheck(Mk2NucleusCase):
    def check(self, map_text, cold=None, objdir=BUILD):
        regions, fixed, cold_list = mapcheck.load_mk2_residency(RESIDENCY)
        tiers, _cold, _test = mapcheck.load_residency(MK1_RESIDENCY)
        return mapcheck.check_mk2(map_text, regions, fixed,
                                  cold if cold is not None else cold_list,
                                  ROOT, objdir, tiers)

    def test_the_image_passes(self):
        ledger = self.check(self.map_text)
        self.assertGreaterEqual(ledger["fixed_room"], 0)
        self.assertGreaterEqual(ledger["cold_room"], 0)
        self.assertLessEqual(ledger["common_used"], ledger["common_ceiling"])

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

    def test_a_fixed_required_object_cannot_be_cold(self):
        regions, fixed, cold = mapcheck.load_mk2_residency(RESIDENCY)
        with self.assertRaisesRegex(mapcheck.MapError, "fixed-required"):
            self.check(self.map_text, cold=cold + ["ipc"], objdir=None)


if __name__ == "__main__":
    unittest.main()
