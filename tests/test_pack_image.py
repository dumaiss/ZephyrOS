#!/usr/bin/env python3
"""Host-side tests for the M0 ROM image packer and layout contract.

Unit tests build synthetic Intel HEX / map inputs; integration tests check
the real build artifacts under build/ when present (created by `make`).
Run with: make test
"""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import pack_image  # noqa: E402


def ihx_record(addr, data, rectype=0x00):
    length = len(data)
    body = bytes([length, (addr >> 8) & 0xFF, addr & 0xFF, rectype]) + bytes(data)
    checksum = (-sum(body)) & 0xFF
    return ":%s%02X" % (body.hex().upper(), checksum)


def make_ihx(segments, eof=True):
    """segments: iterable of (addr, [bytes])"""
    lines = [ihx_record(addr, data) for addr, data in segments]
    if eof:
        lines.append(":00000001FF")
    return "\n".join(lines) + "\n"


class MinimalImageCase:
    """A synthetic, contract-compliant image."""

    def __init__(self, rst30_target=0xE0F0):
        self.rst30_target = rst30_target
        reset = [0xF3] + [0x00] * 5 + [0xC3, 0x00, 0xE0]  # di; ...; jp 0xE000
        kernel = [0xC3, 0x30, 0x20]  # jp 0x2030
        common = [0xC9]  # ret
        stub = [0xC3, rst30_target & 0xFF, (rst30_target >> 8) & 0xFF]
        self.ihx = make_ihx(
            [
                (0x0000, reset),
                (0x0030, stub),
                (0x2000, kernel),
                (0xE000, common),
            ]
        )
        self.map_text = "\n".join(
            [
                "     00000000  _reset                            reset",
                "     00000026  _reset_end                        reset",
                "     00000030  _zos_rst30_vector                 page_zero",
                "     00000033  _zos_rst30_end                    page_zero",
                "     0000E000  _boot_common_entry               start",
                "     0000E010  _early_console_init              early_console",
                "     0000E020  _early_console_putc              early_console",
                "     0000E030  _early_console_puts              early_console",
                "     0000E040  _panic                           panic",
                "     0000E0F0  zos_syscall_entry                syscall_entry",
                "     00002000  _kernel_main                     main",
                "     00002100  zos_syscall_dispatch             syscall_entry",
            ]
        )

    def pack(self):
        return pack_image.pack(self.ihx, self.map_text, None)


class TestSyntheticPacking(unittest.TestCase):
    def setUp(self):
        self.case = MinimalImageCase()

    def test_exact_image_size(self):
        image = self.case.pack()
        self.assertEqual(len(image), 0x20000)

    def test_fill_bytes(self):
        image = self.case.pack()
        self.assertEqual(image[0x0033], 0xFF)    # page-zero reservation
        self.assertEqual(image[0x0100], 0xFF)    # page-1 gap
        self.assertEqual(image[0xC000], 0xFF)    # bank-7 runtime area
        self.assertEqual(image[0xFF00], 0xFF)    # stack region
        self.assertEqual(image[0xFFFF], 0xFF)
        self.assertEqual(image[0x10000 + 0x0100], 0xFF)  # page-1 gap

    def test_copier_mirroring(self):
        image = self.case.pack()
        page0 = image[:0x10000]
        page1 = image[0x10000:]
        self.assertEqual(page0[:0x30], page1[:0x30])
        self.assertNotEqual(page0[0], 0xFF)  # reset vector present

    def test_map_symbols_parsed(self):
        symbols = pack_image.parse_map_symbols(self.case.map_text)
        self.assertEqual(symbols["_reset"], 0x0000)
        self.assertEqual(symbols["_kernel_main"], 0x2000)
        pack_image.check_symbol_regions(symbols)  # must not raise

    def test_rst30_stub_verified(self):
        image = self.case.pack()
        target = self.case.rst30_target
        for page in (image[:0x10000], image[0x10000:]):
            self.assertEqual(
                page[0x30:0x33],
                bytes((0xC3, target & 0xFF, (target >> 8) & 0xFF)),
            )

    def test_rst30_stub_bad_target_rejected(self):
        case = MinimalImageCase(rst30_target=0xE0F1)
        # Image stub still encodes the old target; map symbol moved -> mismatch.
        with self.assertRaises(pack_image.LayoutError):
            pack_image.pack(case.ihx, case.map_text, None)

    def test_rst30_stub_wrong_vector_rejected(self):
        bad_map = self.case.map_text.replace(
            "     00000030  _zos_rst30_vector", "     00000040  _zos_rst30_vector"
        )
        with self.assertRaises(pack_image.LayoutError):
            pack_image.pack(self.case.ihx, bad_map, None)

    def test_symbol_out_of_region_rejected(self):
        bad_map = self.case.map_text.replace(
            "     00002000  _kernel_main", "     0000E000  _kernel_main"
        )
        with self.assertRaises(pack_image.LayoutError):
            pack_image.check_symbol_regions(pack_image.parse_map_symbols(bad_map))

    def test_common_symbol_out_of_region_rejected(self):
        bad_map = self.case.map_text.replace(
            "     0000E040  _panic", "     00002040  _panic"
        )
        with self.assertRaises(pack_image.LayoutError):
            pack_image.check_symbol_regions(pack_image.parse_map_symbols(bad_map))


class TestOutOfRegionContent(unittest.TestCase):
    def pack_extra(self, addr, data):
        case = MinimalImageCase()
        ihx = make_ihx(
            [
                (0x0000, [0xF3, 0x00, 0xC3, 0x00, 0xE0]),
                (0x0030, [0xC3, 0xF0, 0xE0]),
                (0x2000, [0xC3, 0x30, 0x20]),
                (0xE000, [0xC9]),
                (addr, data),
            ]
        )
        pack_image.pack(ihx, case.map_text, None)

    def test_page_zero_reservation_rejected(self):
        with self.assertRaises(pack_image.LayoutError):
            self.pack_extra(0x0033, [0x00])

    def test_bank7_gap_rejected(self):
        with self.assertRaises(pack_image.LayoutError):
            self.pack_extra(0x0100, [0x00])

    def test_bank7_runtime_area_rejected(self):
        with self.assertRaises(pack_image.LayoutError):
            self.pack_extra(0xC000, [0x00])

    def test_stack_region_rejected(self):
        with self.assertRaises(pack_image.LayoutError):
            self.pack_extra(0xFF00, [0x00])

    def test_missing_reset_vector_rejected(self):
        case = MinimalImageCase()
        ihx = make_ihx(
            [
                (0x2000, [0xC3, 0x30, 0x20]),
                (0xE000, [0xC9]),
            ]
        )
        with self.assertRaises(pack_image.LayoutError):
            pack_image.pack(ihx, case.map_text, None)


class TestRealArtifacts(unittest.TestCase):
    """Integration checks against the artifacts produced by `make`."""

    def setUp(self):
        self.build = os.path.join(ROOT, "build", "zephyros.bin")
        self.mame = os.path.join(ROOT, "build", "zephyr80.bin")

    def test_artifacts_exist_and_match(self):
        if not (os.path.exists(self.build) and os.path.exists(self.mame)):
            self.skipTest("build artifacts missing; run `make` first")
        with open(self.build, "rb") as fh:
            primary = fh.read()
        with open(self.mame, "rb") as fh:
            mame_copy = fh.read()
        self.assertEqual(len(primary), 131072)
        self.assertEqual(primary, mame_copy)

    def test_real_image_regions(self):
        if not os.path.exists(self.build):
            self.skipTest("build artifacts missing; run `make` first")
        with open(self.build, "rb") as fh:
            image = fh.read()
        page0 = image[:0x10000]
        page1 = image[0x10000:]
        # Copier mirroring across the two ROM pages.
        self.assertEqual(page0[:0x30], page1[:0x30])
        # Reset vector is present.
        self.assertNotEqual(page0[0], 0xFF)
        # Forbidden regions stay FF-filled (0030h-0032h holds the RST 30h stub).
        for addr in (0x0033, 0x00FF, 0x0100, 0x1FFF, 0xC000, 0xDFFF, 0xFF00, 0xFFFF):
            self.assertEqual(page0[addr], 0xFF, "byte at page0+%04X" % addr)
        for addr in (0x0100, 0x1FFF, 0xC000, 0xDFFF, 0xE000, 0xFFFF):
            self.assertEqual(page1[addr], 0xFF, "byte at page1+%04X" % addr)
        # Bank-7 kernel content is present.
        self.assertTrue(any(b != 0xFF for b in page1[0x2000:0xC000]))
        # Common region content is present.
        self.assertTrue(any(b != 0xFF for b in page0[0xE000:0xFF00]))


if __name__ == "__main__":
    unittest.main()
