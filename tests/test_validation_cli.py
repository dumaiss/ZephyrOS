#!/usr/bin/env python3
"""Validator CLI tests: every rejection class must fail with a nonzero exit
status and an actionable diagnostic.

Fixtures are small, deterministic map/IHX files generated into a temporary
directory and fed to tools/mapcheck.py and tools/pack_image.py as real
command-line invocations.
"""

import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from test_m1 import make_map  # noqa: E402  (shared synthetic map builder)

MAPCHECK = os.path.join(ROOT, "tools", "mapcheck.py")
PACK_IMAGE = os.path.join(ROOT, "tools", "pack_image.py")

GATE = 0xE0A8  # zos_syscall_entry address used by the valid IHX fixture
IRQ_FF = 0xE130


def ihx_record(addr, data):
    length = len(data)
    body = bytes([length, (addr >> 8) & 0xFF, addr & 0xFF, 0x00]) + bytes(data)
    checksum = (-sum(body)) & 0xFF
    return ":%s%02X" % (body.hex().upper(), checksum)


def valid_ihx():
    return "\n".join([
        ihx_record(0x0000, [0xF3, 0x00, 0xC3, 0x00, 0xE0]),   # reset copier
        ihx_record(0x0030, [0xC3, GATE & 0xFF, (GATE >> 8) & 0xFF]),  # RST 30h
        ihx_record(0x2000, [0xC3, 0x30, 0x20]),               # kernel
        ihx_record(0xE000, [0xC9]),                           # common
        ihx_record(0xFDFF, [IRQ_FF & 0xFF, IRQ_FF >> 8]),     # FFh guard
        ":00000001FF",
    ]) + "\n"


def insert_before_eof(ihx, record):
    return ihx.replace(":00000001FF", record + "\n:00000001FF")


class ValidatorCLIBase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="zosval-")
        self.map_path = os.path.join(self.tmp, "fixture.map")
        self.ihx_path = os.path.join(self.tmp, "fixture.ihx")
        self.write_map(make_map())
        self.write_ihx(valid_ihx())

    def write_map(self, text):
        with open(self.map_path, "w") as fh:
            fh.write(text)

    def write_ihx(self, text):
        with open(self.ihx_path, "w") as fh:
            fh.write(text)

    def run_tool(self, tool, *args):
        result = subprocess.run(
            [sys.executable, tool] + list(args),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        return result

    def assert_fails_with(self, tool, args, pattern):
        result = self.run_tool(tool, *args)
        self.assertNotEqual(result.returncode, 0,
                            "expected %s to fail on %r" % (os.path.basename(tool), args))
        self.assertRegex(result.stderr, pattern,
                         "diagnostic %r missing in: %s" % (pattern, result.stderr))
        return result


class TestValidatorCLISuccess(unittest.TestCase):
    def test_valid_map_passes_mapcheck(self):
        with tempfile.TemporaryDirectory(prefix="zosval-") as tmp:
            map_path = os.path.join(tmp, "ok.map")
            open(map_path, "w").write(make_map())
            result = subprocess.run(
                [sys.executable, MAPCHECK, map_path],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("mapcheck: OK", result.stdout)

    def test_valid_image_passes_packer(self):
        with tempfile.TemporaryDirectory(prefix="zosval-") as tmp:
            map_path = os.path.join(tmp, "ok.map")
            ihx_path = os.path.join(tmp, "ok.ihx")
            open(map_path, "w").write(make_map())
            open(ihx_path, "w").write(valid_ihx())
            result = subprocess.run(
                [sys.executable, PACK_IMAGE, ihx_path, map_path, tmp],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("131072 bytes", result.stdout)


class TestMapcheckRejections(ValidatorCLIBase):
    def check(self, text, pattern):
        self.write_map(text)
        return self.assert_fails_with(
            MAPCHECK, (self.map_path,), pattern)

    def test_reset_copier_overlaps_vector(self):
        self.check(make_map(areas={"_RESET0": (0x0000, 0x31)}),
                   r"RST 30h")

    def test_reset_end_reaches_vector(self):
        self.check(make_map(symbols={"_reset_end": 0x0031}),
                   r"exceeds the 0030h RST 30h reservation")

    def test_vector_area_absent(self):
        self.check(make_map(areas={"_PAGE_ZERO0": None}),
                   r"no _PAGE_ZERO area")

    def test_vector_wrong_address(self):
        self.check(make_map(symbols={"_zos_rst30_vector": 0x0040,
                                     "_zos_rst30_end": 0x0043}),
                   r"expected 0030h")

    def test_vector_wrong_size(self):
        self.check(make_map(symbols={"_zos_rst30_end": 0x0035}),
                   r"exactly 3")

    def test_bank7_section_leaves_region(self):
        self.check(make_map(areas={"_CODE": (0xBF00, 0x200)},
                            symbols={"s__CODE": 0xBF00, "l__CODE": 0x200}),
                   r"leaves 2000h-BFFFh")

    def test_common_section_leaves_region(self):
        self.check(make_map(symbols={"s__COMMON": 0xDF00}),
                   r"outside E000h-FCFFh")

    def test_common_section_overlaps_stack(self):
        self.check(make_map(areas={"_COMMON_DATA": (0xE100, 0x1E10)}),
                   r"overlaps the common stack")

    def test_gate_not_in_common(self):
        self.check(make_map(symbols={"zos_syscall_entry": 0x3000}),
                   r"zos_syscall_entry .* not in the common region")

    def test_dispatcher_not_in_bank7(self):
        self.check(make_map(symbols={"zos_syscall_dispatch": 0xE100}),
                   r"outside the bank-7 loadable region")

    def test_gate_stack_size_inconsistent(self):
        self.check(make_map(symbols={"zos_gate_stack_top": 0xE14F}),
                   r"gate stack is")

    def test_mapping_path_outside_common(self):
        self.check(make_map(symbols={"_platform_memctl_write": 0x3000}),
                   r"_platform_memctl_write .* not in the common region")

    def test_im2_page_misaligned(self):
        self.check(make_map(symbols={"irq_vector_page": 0xFD01}),
                   r"irq_vector_page .* expected FD00h")

    def test_irq_transition_stack_wrong_size(self):
        self.check(make_map(symbols={"irq_transition_stack_top": 0xFEF0}),
                   r"irq_transition_stack_top")

    def test_irq_isr_stack_wrong_size(self):
        self.check(make_map(symbols={"irq_isr_stack": 0xDF10}),
                   r"irq_isr_stack")


class TestPackerRejections(ValidatorCLIBase):
    def check(self, ihx_text, map_text, pattern):
        self.write_ihx(ihx_text)
        self.write_map(map_text)
        return self.assert_fails_with(
            PACK_IMAGE, (self.ihx_path, self.map_path, self.tmp), pattern)

    def test_out_of_region_byte(self):
        self.check(insert_before_eof(valid_ihx(), ihx_record(0x0100, [0x00])),
                   make_map(), r"out-of-region content")

    def test_bad_stub_target(self):
        bad = valid_ihx().replace(ihx_record(0x0030, [0xC3, GATE & 0xFF, (GATE >> 8) & 0xFF]),
                                  ihx_record(0x0030, [0xC3, 0x00, 0x20]))
        self.check(bad, make_map(), r"RST 30h vector byte")

    def test_conflicting_records(self):
        dup = insert_before_eof(valid_ihx(), ihx_record(0xE000, [0xC8]))
        self.check(dup, make_map(), r"conflicting content for E000h")

    def test_bad_checksum(self):
        bad = valid_ihx().replace(
            ihx_record(0xE000, [0xC9]), ihx_record(0xE000, [0xC9])[:-2] + "00")
        self.check(bad, make_map(), r"checksum mismatch")

    def test_latch_write_in_bank7(self):
        bad = valid_ihx().replace(ihx_record(0x2000, [0xC3, 0x30, 0x20]),
                                  ihx_record(0x2000, [0xD3, 0x00, 0xC9]))
        self.check(bad, make_map(), r"latch write OUT \(00h\),A")

    def test_vector_symbol_missing(self):
        self.check(valid_ihx(), make_map(symbols={"_zos_rst30_vector": None}),
                   r"expected 0030h")

    def test_broken_ff_guard(self):
        bad = valid_ihx().replace(
            ihx_record(0xFDFF, [IRQ_FF & 0xFF, IRQ_FF >> 8]),
            ihx_record(0xFDFF, [(IRQ_FF + 1) & 0xFF, IRQ_FF >> 8]),
        )
        self.check(bad, make_map(), r"word_at\(FDFFh\)")


if __name__ == "__main__":
    unittest.main()
