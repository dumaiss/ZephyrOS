#!/usr/bin/env python3
"""MEM_DECODER revision-13 vectors versus the simulator's decoder model.

The single table is ``platform/zephyr80/hdl/MEM_DECODER_rev13.si``: the
same file WinCUPL's CSIM simulates the PLD against.  These tests parse it
and drive ``tests/z80sim.py``'s ``MemoryDecoder`` with the very same
inputs, so the PLD, the committed vectors and the simulator cannot drift:

* the committed vector file is exactly what ``tools/rev13_vectors.py``
  generates from the equations (regenerate == on-disk);
* every vector's expected RAM bank bits, ROM_CS and SRAM_CS match the
  simulator's decoder model at revision 13;
* the vector set covers every mode x address region x bank x page
  combination;
* revision 13 equals revision 12 whenever P = 0, in every mode, and
  revision 13 is the simulator's default;
* the page field does what the architecture says: modes 00/01 ignore it,
  the mode-10 aperture opens only with P != 0, and the mode-11 window
  shows bank 7 - page over 6000h-DFFFh while 2000h-5FFFh stays bank 7
  and 0000h-1FFFh keeps the bank field.
"""

import os
import re
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from z80sim import MemoryDecoder  # noqa: E402
import pld_terms  # noqa: E402
import rev13_vectors  # noqa: E402

HDL = os.path.join(ROOT, "platform", "zephyr80", "hdl")
PLD = os.path.join(HDL, "MEM_DECODER_rev13.pld")
SI = os.path.join(HDL, "MEM_DECODER_rev13.si")

VALUES = set("01HLNXZ*")


def parse_si(path):
    """Parse the ORDER statement and the vector table."""
    header = {}
    order = None
    vectors = []
    with open(path) as handle:
        lines = handle.readlines()
    for raw in lines:
        line = raw.split("/*")[0].strip()
        if not line or line.startswith("$"):
            continue
        if line.startswith("ORDER:"):
            order = [name.strip() for name in
                     line[len("ORDER:"):].rstrip(";").split(",")]
            continue
        m = re.match(r"^([A-Za-z]+)\s+(\S+);$", line)
        if m and order is None:
            header[m.group(1)] = m.group(2)
            continue
        packed = line.replace(" ", "")
        if packed and packed[0] in "01" and set(packed) <= VALUES:
            vectors.append(packed)
    return header, order, vectors


def vector_inputs(order, packed):
    """(addr, latch, mreq, rd, wr) for one vector, as pin levels."""
    values = {name: packed[i] for i, name in enumerate(order)}
    addr = (int(values["A13"]) << 13) | (int(values["A14"]) << 14) | \
           (int(values["A15"]) << 15)
    latch = ((int(values["PAGE2"]) << 7) | (int(values["PAGE1"]) << 6) |
             (int(values["PAGE0"]) << 5) |
             (int(values["MEM_MODE1"]) << 4) |
             (int(values["MEM_MODE0"]) << 3) |
             (int(values["BANK_Q2"]) << 2) | (int(values["BANK_Q1"]) << 1) |
             int(values["BANK_Q0"]))
    mreq = int(values["MREQ"])
    rd = int(values["!RD"])    # 0 = read assertion
    wr = int(values["!WR"])    # 0 = write assertion
    return addr, latch, mreq, rd, wr


class TestVectorFile(unittest.TestCase):
    def test_committed_vectors_are_current(self):
        with open(SI) as handle:
            on_disk = handle.read()
        self.assertEqual(on_disk, rev13_vectors.build_si(13),
                         "MEM_DECODER_rev13.si is stale; run "
                         "tools/rev13_vectors.py")

    def test_generator_proves_the_equations(self):
        # Fails loudly inside the generator if the equations drift.
        rev13_vectors.check_equivalence()
        rev13_vectors.check_page_rules()

    def test_header_matches_the_pld(self):
        header, _, _ = parse_si(SI)
        with open(PLD) as handle:
            pld = handle.read()
        for key in ("Name", "Partno", "Device", "Revision"):
            m = re.search(r"^%s\s+(\S+);" % key, pld, re.M)
            self.assertIsNotNone(m, "%s missing from the PLD" % key)
            self.assertEqual(header.get(key), m.group(1),
                             "%s differs between .pld and .si" % key)
        self.assertIn("Revision 13;", pld)

    def test_vectors_cover_every_combination(self):
        _, order, vectors = parse_si(SI)
        self.assertIn("PAGE0", order)
        seen = set()
        for packed in vectors:
            addr, latch, _, _, _ = vector_inputs(order, packed)
            seen.add(((addr >> 13) & 0x7, latch & 0x18, latch & 0x07,
                      (latch >> 5) & 0x07))
        expected = {(region >> 1, mode << 3, bank, page)
                    for region in range(8)
                    for mode in range(4)
                    for bank in range(8)
                    for page in range(8)}
        self.assertTrue(expected <= seen,
                        "vectors do not cover %d combinations"
                        % len(expected - seen))

    def test_pld_declares_the_page_pins_as_inputs(self):
        with open(PLD) as handle:
            pld = handle.read()
        for pin, name in ((18, "PAGE0"), (19, "PAGE1"), (20, "PAGE2")):
            self.assertIn("Pin %d = %s;" % (pin, name), pld)
        # No equation drives them: the only .oe terms are the library's
        # constant-0 defaults, checked in the WinCUPL report when the
        # toolchain runs (make rev13-pld).
        for name in ("PAGE0", "PAGE1", "PAGE2"):
            self.assertIsNone(re.search(r"^%s\s*=" % name, pld, re.M),
                              "%s must not have an equation" % name)

    def test_pld_equations_are_the_minimum_cover(self):
        self.assertEqual(pld_terms.check_pld(PLD), [],
                         "MEM_DECODER_rev13.pld is not the exact minimum "
                         "cover of the revision-13 equations; regenerate "
                         "it with tools/pld_terms.py --equations "
                         "revision-13 --inverted")
        row = [entry for entry in pld_terms.variants()
               if entry["variant"] == "revision-13"][0]
        self.assertTrue(row["fit"])
        self.assertEqual(row["terms"], {"RAM_A16": 12, "RAM_A17": 12,
                                        "RAM_A18": 12})
        self.assertEqual(row["terms_inverted"], {"RAM_A16": 9,
                                                 "RAM_A17": 9,
                                                 "RAM_A18": 9})


class TestSimulatorAgainstVectors(unittest.TestCase):
    def test_simulator_reproduces_every_vector(self):
        _, order, vectors = parse_si(SI)
        self.assertGreater(len(vectors), 2000)
        decoder = MemoryDecoder(13)
        checked = 0
        for packed in vectors:
            addr, latch, mreq, rd, wr = vector_inputs(order, packed)
            values = {name: packed[i] for i, name in enumerate(order)}
            got = decoder.ram_bits(addr, latch)
            want = tuple(1 if values[n] == "H" else 0
                         for n in ("RAM_A16", "RAM_A17", "RAM_A18"))
            self.assertEqual(got, want,
                             "RAM bits differ at vector %s" % packed)
            rom, sram = decoder.chip_selects(mreq, rd, wr, latch)
            self.assertEqual("L" if rom else "H", values["!ROM_CS"],
                             "ROM_CS differs at vector %s" % packed)
            self.assertEqual("L" if sram else "H", values["!SRAM_CS"],
                             "SRAM_CS differs at vector %s" % packed)
            self.assertEqual(values["!CART_CS"], "N")
            checked += 1
        self.assertEqual(checked, len(vectors))

    def test_revision_12_simulator_matches_the_same_vectors(self):
        # The equivalence domain: page = 0 in every mode.  On those
        # vectors revision 12's model must produce the same bank bits as
        # the file's expectations (which come from the revision-13
        # equations).
        _, order, vectors = parse_si(SI)
        old = MemoryDecoder(12)
        new = MemoryDecoder(13)
        checked = 0
        for packed in vectors:
            addr, latch, _, _, _ = vector_inputs(order, packed)
            if (latch >> 5) & 0x07:
                continue
            self.assertEqual(old.ram_bits(addr, latch),
                             new.ram_bits(addr, latch),
                             "revision 12/13 differ at %s" % packed)
            checked += 1
        self.assertGreater(checked, 250)


class TestEquivalence(unittest.TestCase):
    def test_page_zero_is_revision_12_in_every_mode(self):
        old = MemoryDecoder(12)
        new = MemoryDecoder(13)
        for mode in (0x00, 0x08, 0x10, 0x18):
            for region in range(8):
                addr = region << 13
                for bank in range(8):
                    latch = mode | bank
                    self.assertEqual(
                        old.ram_bits(addr, latch),
                        new.ram_bits(addr, latch),
                        "mode %02Xh bank %d region %d"
                        % (mode, bank, region))

    def test_modes_00_and_01_ignore_the_page_even_when_not_zero(self):
        old = MemoryDecoder(12)
        new = MemoryDecoder(13)
        for mode in (0x00, 0x08):
            for region in range(8):
                addr = region << 13
                for bank in range(8):
                    for page in range(8):
                        latch = (page << 5) | mode | bank
                        self.assertEqual(
                            old.ram_bits(addr, latch),
                            new.ram_bits(addr, latch),
                            "mode %02Xh page %d bank %d region %d"
                            % (mode, page, bank, region))

    def test_revision_13_is_the_simulator_default(self):
        from z80sim import Z80
        z = Z80(bytes(0x10000), bytes(0x10000))
        self.assertEqual(z.decoder.revision, 13)
        z12 = Z80(bytes(0x10000), bytes(0x10000), decoder_revision=12)
        self.assertEqual(z12.decoder.revision, 12)
        with self.assertRaises(ValueError):
            MemoryDecoder(11)


class TestPageFieldSemantics(unittest.TestCase):
    def test_mode_11_window_layout(self):
        decoder = MemoryDecoder(13)
        for page in range(8):
            latch = (page << 5) | 0x18 | 0x05   # OS mode over bank 5
            self.assertEqual(decoder.bank_of(0x0000, latch), 5)
            self.assertEqual(decoder.bank_of(0x1FFF, latch), 5)
            # The fixed core is 2000h-5FFFh: bank 7 whatever the page.
            for addr in (0x2000, 0x3FFF, 0x4000, 0x5FFF):
                self.assertEqual(decoder.bank_of(addr, latch), 7, addr)
            # The window is 6000h-DFFFh: bank 7 - page.
            for addr in (0x6000, 0x7FFF, 0x8000, 0xBFFF, 0xC000, 0xDFFF):
                self.assertEqual(decoder.bank_of(addr, latch), 7 - page,
                                 addr)
            self.assertEqual(decoder.bank_of(0xE000, latch), 0)
            self.assertEqual(decoder.bank_of(0xFFFF, latch), 0)
        self.assertEqual(decoder.window_span(), (0x6000, 0xE000))

    def test_mode_10_aperture_opens_only_when_page_is_not_zero(self):
        decoder = MemoryDecoder(13)
        for page in range(8):
            latch = (page << 5) | 0x10 | 0x03   # application over bank 3
            for addr in (0x8000, 0x9FFF, 0xA000, 0xBFFF, 0xBFFF):
                want = 3 if page == 0 else 7 - page
                self.assertEqual(decoder.bank_of(addr, latch), want,
                                 "page %d addr %04X" % (page, addr))
            # Everything else in the application view is the bank field.
            for addr in (0x0000, 0x2000, 0x4000, 0x6000, 0x7FFF, 0xC000,
                         0xDFFF):
                self.assertEqual(decoder.bank_of(addr, latch), 3, addr)
            self.assertEqual(decoder.bank_of(0xE000, latch), 0)

    def test_other_modes_ignore_the_window(self):
        decoder = MemoryDecoder(13)
        for mode in (0x00, 0x08, 0x10):
            for page in range(8):
                latch = (page << 5) | mode | 0x03
                if mode == 0x10:
                    for addr in (0x0000, 0x6000, 0xDFFF):
                        self.assertEqual(decoder.bank_of(addr, latch), 3)
                self.assertEqual(decoder.bank_of(0xE000, latch), 0
                                 if mode == 0x10 else 3)

    def test_z80_maps_the_window_and_the_aperture(self):
        from z80sim import Z80
        z = Z80(bytes(0x10000), bytes(0x10000))
        # OS mode, page 1, bank field 5: the window shows bank 6.
        z.latch = (1 << 5) | 0x18 | 0x05
        self.assertEqual(z.bank_of(0x6000), 6)
        self.assertEqual(z.bank_of(0x2000), 7)
        # Application mode, page 2, bank field 3: the aperture shows 5.
        z.latch = (2 << 5) | 0x10 | 0x03
        self.assertEqual(z.bank_of(0x8000), 5)
        self.assertEqual(z.bank_of(0x4000), 3)
        # Page 0 is revision 12 in both modes.
        z.latch = 0x18 | 0x05
        self.assertEqual(z.bank_of(0x6000), 7)
        self.assertEqual(z.bank_of(0x1000), 5)


if __name__ == "__main__":
    unittest.main()
