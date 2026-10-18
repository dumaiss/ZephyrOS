"""Structural and simulated checks for ColecoGo's no-return Stage B."""

from __future__ import annotations

import json
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SDK_TOOLS = ROOT / "build" / "kos-sdk-0.1.0" / "tools"
KONA_TESTS = ROOT.parents[3] / "ZephyrOS" / "tests"
sys.path.insert(0, str(SDK_TOOLS))
sys.path.insert(0, str(KONA_TESTS))

import ihx  # noqa: E402
from z80sim import Z80  # noqa: E402


class HandoffTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        obj = ROOT / "build" / "user" / "colecogo"
        cls.memory = ihx.parse_ihx((obj / "colecogo.ihx").read_text())
        cls.symbols = ihx.parse_map_symbols((obj / "colecogo.map").read_text())
        cls.report = json.loads((obj / "colecogo.report.json").read_text())

    def test_primary_layout_has_working_room(self) -> None:
        self.assertLessEqual(self.report["bss"]["size"], 0xA400)
        self.assertGreaterEqual(self.report["stack"]["base"], 0xD800)
        self.assertLess(self.symbols["_handoff_begin"], 0xE000)
        self.assertLessEqual(
            self.symbols["_handoff_end"] - self.symbols["_handoff_begin"],
            0x300,
        )
        self.assertLessEqual(
            self.symbols["_stage_b_end"] - self.symbols["_stage_b_begin"],
            0x100,
        )

    def test_stage_b_constructs_flat_coleco_image(self) -> None:
        machine = Z80(bytearray(0x10000), bytearray(0x10000))
        bank = machine.banks[5]
        begin = self.symbols["_stage_b_begin"]
        end = self.symbols["_stage_b_end"]
        stage = bytes(self.memory[address] for address in range(begin, end))
        bank[0x5F00 : 0x5F00 + len(stage)] = stage

        stub_begin = self.symbols["_handoff_begin"]
        stub_end = self.symbols["_handoff_end"]
        stub_saved = bytes((index * 7 + 3) & 0xFF
                           for index in range(stub_end - stub_begin))
        bios_page = bytes(index ^ 0xA5 for index in range(0x100))
        upper_prefix = bytes((index * 5 + 1) & 0xFF
                             for index in range(0x3F00))
        upper_tail = bytes((index * 11 + 9) & 0xFF
                           for index in range(0x100))

        bank[0x2000:0x5F00] = upper_prefix
        bank[0x6000:0x8000] = b"\xCC" * 0x2000
        bank[0x7A00 : 0x7A00 + len(stub_saved)] = stub_saved
        bank[0x7D00:0x7E00] = bios_page
        bank[0x7E00:0x7F00] = upper_tail

        machine.latch = 0x10 | 5
        machine.pc = 0x5F00
        for _ in range(32):
            if machine.pc == 0:
                break
            machine.step()
        self.assertEqual(machine.pc, 0)
        self.assertEqual(machine.latch, 0x08 | 5)
        self.assertEqual(bank[0x0000:0x0100], bios_page)
        self.assertEqual(bank[stub_begin:stub_end], stub_saved)
        self.assertEqual(bank[0xC000:0xFF00], upper_prefix)
        self.assertEqual(bank[0xFF00:0x10000], upper_tail)
        self.assertEqual(bank[0x6000:0x8000], b"\x00" * 0x2000)


if __name__ == "__main__":
    unittest.main()
