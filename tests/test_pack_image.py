#!/usr/bin/env python3
"""Host-side tests for the M0 ROM image packer and layout contract.

Unit tests build synthetic Intel HEX / map inputs; integration tests check
the real build artifacts under build/ when present (created by `make`).
Run with: make test
"""

import json
import os
import sys
import tempfile
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
        # The kernel body is cold-half code under revision 13; the fixed
        # core carries the gates.
        kernel = [0xC3, 0x30, 0x60]  # jp 0x6030
        common = [0xC9]  # ret
        irq_target = 0xE130
        stub = [0xC3, rst30_target & 0xFF, (rst30_target >> 8) & 0xFF]
        # ROM-access transition block: trampoline OUT (00h),A at EE00h-2,
        # then the mirrored stub (LDIR / EX AF,AF' / OUT / JP).
        trampoline = [0xD3, 0x00]
        rom_stub = [0xED, 0xB0, 0x08, 0xD3, 0x00, 0xC3, 0xF0, 0xE0]
        self.rom_stub = bytes(rom_stub)
        self.ihx = make_ihx(
            [
                (0x0000, reset),
                (0x0030, stub),
                (0x2000, [0xC9]),   # fixed-core padding byte
                (0x6000, kernel),
                (0xE000, common),
                (0xEEFE, trampoline),
                (0xEF00, rom_stub),
                (0xFDFF, [irq_target & 0xFF, irq_target >> 8]),
            ]
        )
        self.map_text = "\n".join(
            [
                "     00000000  _reset                            reset",
                "     00000026  _reset_end                        reset",
                "     00000030  _kos_rst30_vector                 page_zero",
                "     00000033  _kos_rst30_end                    page_zero",
                "     0000E000  _boot_common_entry               start",
                "     0000E010  _early_console_init              early_console",
                "     0000E020  _early_console_putc              early_console",
                "     0000E030  _early_console_puts              early_console",
                "     0000E040  _panic                           panic",
                "     0000E0F0  kos_syscall_entry                syscall_entry",
                "     0000E110  irq_ctc0_stub                   irq_entry",
                "     0000E130  irq_ff_unexpected               irq_entry",
                "     0000E140  irq_common_entry                irq_entry",
                "     0000E1A0  _irq_arch_init                  platform_irq",
                "     0000FD00  irq_vector_page                 irq_vectors",
                "     0000FE00  irq_vector_guard                irq_vectors",
                "     00006000  _kernel_main                     main",
                "     00002100  kos_syscall_dispatch             syscall_entry",
                "     00002200  _irq_dispatch                    irq",
                "     0000EEFE  rom_access_trampoline            rom_access",
                "     0000EF00  rom_access_sync                  rom_access",
                "     0000EF08  rom_access_sync_end              rom_access",
            ]
        )

    def pack(self):
        return pack_image.pack(self.ihx, self.map_text, None)


class TestSyntheticPacking(unittest.TestCase):
    def setUp(self):
        self.case = MinimalImageCase()

    def test_exact_image_size(self):
        image = self.case.pack()
        self.assertEqual(len(image), 0x80000)

    def test_fill_bytes(self):
        image = self.case.pack()
        self.assertEqual(image[0x0033], 0xFF)    # page-zero reservation
        self.assertEqual(image[0x0100], 0xFF)    # page-1 gap
        self.assertEqual(image[0xC000], 0xFF)    # bank-7 runtime area
        self.assertEqual(image[0xFF00], 0xFF)    # stack region
        self.assertEqual(image[0xFFFF], 0xFF)
        self.assertEqual(image[0x10000 + 0x0100], 0xFF)  # page-1 gap
        # Recovery pages 2-7 carry only the ROMFS stream (absent here)
        # and the ROM-access stub mirror at EF00h.
        for page in range(2, 8):
            base = page * 0x10000
            self.assertEqual(image[base], 0xFF)
            self.assertEqual(image[base + 0xC000], 0xFF)
            self.assertEqual(image[base + 0xEF00 - 1], 0xFF)
            self.assertEqual(image[base + 0xEF08], 0xFF)
            self.assertEqual(image[base + 0xFFFF], 0xFF)
            self.assertEqual(image[base + 0xEF00:base + 0xEF08],
                             self.case.rom_stub)

    def test_rom_access_stub_is_mirrored_but_not_into_boot_pages(self):
        image = self.case.pack()
        self.assertEqual(image[0xEF00:0xEF08], self.case.rom_stub)
        self.assertEqual(image[0xEEFE:0xEF00], b"\xD3\x00")
        # Page 1 (bank 7) never carries the stub; mode 00 executes it from
        # page 0 (SRAM bank 0) and the recovery pages only.
        self.assertEqual(image[0x10000 + 0xEF00:0x10000 + 0xEF08],
                         b"\xFF" * 8)

    def test_rom_access_stub_missing_rejected(self):
        ihx = self.case.ihx.replace(
            ihx_record(0xEF00, list(self.case.rom_stub)), "")
        with self.assertRaisesRegex(pack_image.LayoutError,
                                    "ROM-access stub byte"):
            pack_image.pack(ihx, self.case.map_text, None)

    def test_rom_access_trampoline_missing_rejected(self):
        ihx = self.case.ihx.replace(ihx_record(0xEEFE, [0xD3, 0x00]), "")
        with self.assertRaisesRegex(pack_image.LayoutError,
                                    "trampoline"):
            pack_image.pack(ihx, self.case.map_text, None)

    def test_copier_mirroring(self):
        image = self.case.pack()
        page0 = image[:0x10000]
        page1 = image[0x10000:]
        self.assertEqual(page0[:0x30], page1[:0x30])
        self.assertNotEqual(page0[0], 0xFF)  # reset vector present

    def test_map_symbols_parsed(self):
        symbols = pack_image.parse_map_symbols(self.case.map_text)
        self.assertEqual(symbols["_reset"], 0x0000)
        self.assertEqual(symbols["_kernel_main"], 0x6000)
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
            "     00000030  _kos_rst30_vector", "     00000040  _kos_rst30_vector"
        )
        with self.assertRaises(pack_image.LayoutError):
            pack_image.pack(self.case.ihx, bad_map, None)

    def test_broken_ff_guard_rejected(self):
        bad = self.case.ihx.replace(
            ihx_record(0xFDFF, [0x30, 0xE1]),
            ihx_record(0xFDFF, [0x31, 0xE1]),
        )
        with self.assertRaises(pack_image.LayoutError):
            pack_image.pack(bad, self.case.map_text, None)

    def test_symbol_out_of_region_rejected(self):
        bad_map = self.case.map_text.replace(
            "     00006000  _kernel_main", "     0000E000  _kernel_main"
        )
        with self.assertRaises(pack_image.LayoutError):
            pack_image.check_symbol_regions(pack_image.parse_map_symbols(bad_map))

    def test_common_symbol_out_of_region_rejected(self):
        bad_map = self.case.map_text.replace(
            "     0000E040  _panic", "     00002040  _panic"
        )
        with self.assertRaises(pack_image.LayoutError):
            pack_image.check_symbol_regions(pack_image.parse_map_symbols(bad_map))


class TestRecoveryPlacement(unittest.TestCase):
    """ROMFS stream placement, capacity, and boot-page preservation."""

    def make_stream(self, size, fill=0x5A):
        header = bytearray(16)
        header[0:4] = b"KRFS"
        header[4] = 1
        header[5] = 36
        header[12:16] = size.to_bytes(4, "little")
        return bytes(header) + bytes([fill]) * (size - 16)

    def test_stream_is_placed_across_windows(self):
        case = MinimalImageCase()
        size = pack_image.ROMFS_WINDOW_SIZE + 0x40
        stream = self.make_stream(size)
        image = pack_image.pack(case.ihx, case.map_text, None, romfs=stream)
        # First window fully used, second window starts at its base.
        self.assertEqual(image[2 * 0x10000], stream[0])
        self.assertEqual(image[2 * 0x10000 + 0xBFFF], stream[0xBFFF])
        self.assertEqual(image[3 * 0x10000], stream[0xC000])
        self.assertEqual(image[3 * 0x10000 + 0x3F], stream[size - 1])
        # The C000h-FFFFh half of every recovery page stays FF apart from
        # the ROM-access stub mirror.
        for page in (2, 3):
            expected = bytearray(b"\xFF" * 0x4000)
            expected[0xEF00 - 0xC000:0xEF00 - 0xC000 + 8] = case.rom_stub
            self.assertEqual(
                image[page * 0x10000 + 0xC000:(page + 1) * 0x10000],
                bytes(expected),
                "page %d high half" % page)
        # The next free logical byte stays FF.
        self.assertEqual(image[3 * 0x10000 + 0x40], 0xFF)

    def test_boot_pages_are_independent_of_the_recovery_stream(self):
        case = MinimalImageCase()
        without = pack_image.pack(case.ihx, case.map_text, None)
        with_romfs = pack_image.pack(case.ihx, case.map_text, None,
                                     romfs=self.make_stream(0x1000))
        self.assertEqual(without[:0x20000], with_romfs[:0x20000],
                         "ROMFS placement modified the boot pages")

    def test_empty_or_short_stream_rejected(self):
        case = MinimalImageCase()
        with self.assertRaisesRegex(pack_image.LayoutError, "empty"):
            pack_image.pack(case.ihx, case.map_text, None, romfs=b"")
        with self.assertRaisesRegex(pack_image.LayoutError, "shorter"):
            pack_image.pack(case.ihx, case.map_text, None, romfs=b"KRFS")

    def test_capacity_excess_rejected(self):
        case = MinimalImageCase()
        too_big = self.make_stream(0x10) + b"\x00" * pack_image.ROMFS_CAPACITY
        with self.assertRaisesRegex(pack_image.LayoutError, "capacity"):
            pack_image.pack(case.ihx, case.map_text, None, romfs=too_big)

    def test_build_dir_requires_recovery_romfs(self):
        case = MinimalImageCase()
        with tempfile.TemporaryDirectory(prefix="kosimg-") as tmp:
            with self.assertRaisesRegex(pack_image.LayoutError,
                                        "recovery.romfs missing"):
                pack_image.pack(case.ihx, case.map_text, tmp)
            with open(os.path.join(tmp, "recovery.romfs"), "wb") as fh:
                fh.write(self.make_stream(0x100))
            image = pack_image.pack(case.ihx, case.map_text, tmp)
            self.assertEqual(len(image), 0x80000)
            self.assertEqual(image[2 * 0x10000], ord("K"))


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
        self.build = os.path.join(ROOT, "build", "konaos.bin")
        self.mame = os.path.join(ROOT, "build", "zephyr80.bin")

    def test_artifacts_exist_and_match(self):
        if not (os.path.exists(self.build) and os.path.exists(self.mame)):
            self.skipTest("build artifacts missing; run `make` first")
        with open(self.build, "rb") as fh:
            primary = fh.read()
        with open(self.mame, "rb") as fh:
            mame_copy = fh.read()
        self.assertEqual(len(primary), 0x80000)
        self.assertEqual(primary, mame_copy)

    def test_real_image_regions(self):
        if not os.path.exists(self.build):
            self.skipTest("build artifacts missing; run `make` first")
        with open(self.build, "rb") as fh:
            image = fh.read()
        page0 = image[:0x10000]
        page1 = image[0x10000:0x20000]
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
        # Recovery pages carry the ROMFS header at their window base and
        # only the ROM-access stub mirror in the C000h-FFFFh half.
        self.assertEqual(image[2 * 0x10000:2 * 0x10000 + 4], b"KRFS")
        stub = page0[0xEF00:0xEF08]
        self.assertNotEqual(stub, b"\xFF" * 8)
        expected_tail = bytearray(b"\xFF" * 0x4000)
        expected_tail[0xEF00 - 0xC000:0xEF00 - 0xC000 + 8] = stub
        for page in range(2, 8):
            self.assertEqual(
                image[page * 0x10000 + 0xC000:(page + 1) * 0x10000],
                bytes(expected_tail),
                "recovery page %d high half is not FF plus "
                "the stub mirror" % page)
            self.assertEqual(
                image[page * 0x10000 + 0xEF00:page * 0x10000 + 0xEF08],
                stub, "recovery page %d stub mirror differs" % page)


class TestNucleusLedgers(unittest.TestCase):
    """The five architecture-36.20 ledger lines, and the driver ledger fix
    package 42c made: `driver hot`/`driver boot/bulk` come from the packed
    driver set's plan instead of being hard-zeroed."""

    def test_ledger_lines_carry_the_driver_plan(self):
        plan = {
            "nucleus": {
                "driver_hot_bytes": 22013,
                "driver_hot_capacity_bytes": 0x8000,
                "driver_boot_bytes": 2170,
                "driver_boot_capacity_bytes": 0x6000,
            },
        }
        symbols = {"__kernel_code_end": 0xA570, "s__WINDOW_DATA": 0xA600,
                   "l__WINDOW_DATA": 0x6D9, "s__DATA": 0x5A00,
                   "l__DATA": 0x40}
        ledger = pack_image.nucleus_ledger(symbols, plan)
        self.assertEqual(ledger["driver_hot_bytes"], 22013)
        self.assertEqual(ledger["driver_boot_bytes"], 2170)
        self.assertEqual(ledger["cold_code_bytes"], 0xA570 - 0x6000)
        self.assertEqual(ledger["cold_data_bytes"], 0x6D9)
        self.assertEqual(ledger["cold_data_base"], 0xA600)
        self.assertEqual(ledger["cold_room_bytes"], 0xE000 - 0xA600 - 0x6D9)

    def test_ledger_without_a_driver_plan_reports_zeros(self):
        # A synthetic host map has no packed driver set: the two bank-6
        # ledgers stay zero rather than being invented.
        symbols = {"__kernel_code_end": 0xA570, "s__WINDOW_DATA": 0xA600,
                   "l__WINDOW_DATA": 0x6D9, "s__DATA": 0x5A00}
        ledger = pack_image.nucleus_ledger(symbols, None)
        self.assertEqual(ledger["driver_hot_bytes"], 0)
        self.assertEqual(ledger["driver_boot_bytes"], 0)

    def test_ledger_without_derived_symbols_is_none(self):
        self.assertIsNone(pack_image.nucleus_ledger({}, None))

    def test_ledger_lines_match_the_built_image(self):
        manifest_path = os.path.join(ROOT, "build",
                                     "konaos.manifest.json")
        plan_path = os.path.join(ROOT, "build", "modules",
                                 "drivers.plan.json")
        if not all(os.path.exists(p) for p in (manifest_path, plan_path)):
            self.skipTest("derived production build missing; run make")
        ledger = json.load(open(manifest_path))["nucleus"]
        plan = json.load(open(plan_path))
        hot = plan["nucleus"]
        # The production ledger and the packer's plan agree: this is what
        # the mis-reported driver line looked like before package 42c.
        self.assertGreater(hot["driver_hot_bytes"], 0)
        self.assertEqual(ledger["driver_hot_bytes"],
                         hot["driver_hot_bytes"])
        self.assertEqual(ledger["driver_boot_bytes"],
                         hot["driver_boot_bytes"])
        self.assertEqual(ledger["driver_hot_capacity_bytes"],
                         hot["driver_hot_capacity_bytes"])
        self.assertGreaterEqual(
            hot["driver_hot_capacity_bytes"] - hot["driver_hot_bytes"],
            4096, "driver hot tier room")
        text = pack_image.format_nucleus_ledger(ledger)
        self.assertIn("driver hot: %d bytes of %d"
                      % (hot["driver_hot_bytes"],
                         hot["driver_hot_capacity_bytes"]), text)
        self.assertIn("driver boot/bulk: %d bytes of %d"
                      % (hot["driver_boot_bytes"],
                         hot["driver_boot_capacity_bytes"]), text)
        self.assertIn("nucleus cold: code %d bytes end %04Xh" % (
            ledger["cold_code_bytes"], ledger["cold_code_end"]), text)

    def test_driver_plan_loader_reads_the_build_directory(self):
        plan_path = os.path.join(ROOT, "build", "modules",
                                 "drivers.plan.json")
        if not os.path.exists(plan_path):
            self.skipTest("derived production build missing; run make")
        plan = pack_image.load_driver_plan(os.path.join(ROOT, "build"))
        self.assertIn("iocfat.mod", plan["order"])
        self.assertIsNone(pack_image.load_driver_plan(None))


class TestCommonCeiling(unittest.TestCase):
    """The pinned common-memory ceiling (the M10 reservation is released)."""

    def test_pinned_values(self):
        self.assertEqual(pack_image.COMMON_CEILING, 3072)

    def test_common_usage_rejects_over_the_ceiling(self):
        # 0x700 + 0x600 = 3328 bytes of packed common.
        symbols = {"s__COMMON": 0xE000, "l__COMMON": 0x700,
                   "s__COMMON_DATA": 0xE700, "l__COMMON_DATA": 0x600}
        with self.assertRaisesRegex(pack_image.LayoutError, "ceiling"):
            pack_image.common_usage(symbols)

    def test_common_usage_accepts_at_the_ceiling(self):
        symbols = {"s__COMMON": 0xE000, "l__COMMON": 0x600,
                   "s__COMMON_DATA": 0xE600, "l__COMMON_DATA": 0x600}
        self.assertEqual(pack_image.common_usage(symbols)[2], 3072)

    def test_production_ledger_carries_the_ceiling(self):
        manifest_path = os.path.join(ROOT, "build",
                                     "konaos.manifest.json")
        if not os.path.exists(manifest_path):
            self.skipTest("derived production build missing; run make")
        ledger = json.load(open(manifest_path))["nucleus"]
        self.assertEqual(ledger["common_ceiling_bytes"], 3072)
        self.assertNotIn("common_m10_reservation_bytes", ledger)
        self.assertNotIn("common_limit_bytes", ledger)
        self.assertLessEqual(ledger["common_used_bytes"],
                             ledger["common_ceiling_bytes"])
        text = pack_image.format_nucleus_ledger(ledger)
        self.assertIn("ceiling 3072", text)
        self.assertNotIn("m10", text)


if __name__ == "__main__":
    unittest.main()
