#!/usr/bin/env python3
"""Decoder revision 13 against the simulator's decoder model.

The programmed PLD and its WinCUPL simulation vectors live in the
hardware project; the OS keeps only the model and its behaviour pins, so
the simulator cannot drift from the architecture's decoder rules:

* revision 13 equals revision 12 whenever P = 0, in every mode, region
  and bank, and revision 13 is the simulator's default;
* the page field does what the architecture says: modes 00/01 ignore it,
  the mode-10 aperture opens only with P != 0, and the mode-11 window
  shows bank 7 - page over 6000h-DFFFh while 2000h-5FFFh stays bank 7
  (the fixed core) and 0000h-1FFFh keeps the bank field.
"""

import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from z80sim import MemoryDecoder  # noqa: E402


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
