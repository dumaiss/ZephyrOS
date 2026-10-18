import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOFTWARE = ROOT.parent / "Software" / "disk1" / "8"


def crc16(data: bytes) -> int:
    value = 0xFFFF
    for byte in data:
        value ^= byte << 8
        for _ in range(8):
            value = (((value << 1) ^ 0x1021) if value & 0x8000 else
                     (value << 1)) & 0xFFFF
    return value


def expand(pattern: int, colour: int) -> bytes:
    fg, bg = colour >> 4, colour & 15
    pixels = [fg if pattern & (0x80 >> bit) else bg for bit in range(8)]
    return bytes((pixels[i] << 4) | pixels[i + 1] for i in range(0, 8, 2))


class PortTests(unittest.TestCase):
    def test_reference_images(self):
        cart = SOFTWARE / "dk.rom"
        bios = SOFTWARE / "coleco.rom"
        if not cart.exists() or not bios.exists():
            self.skipTest("reference ROMs are not present")
        self.assertEqual((len(cart.read_bytes()), crc16(cart.read_bytes())),
                         (0x4000, 0x0DC3))
        self.assertEqual((len(bios.read_bytes()), crc16(bios.read_bytes())),
                         (0x2000, 0x5351))

    def test_g4_pixel_expansion(self):
        self.assertEqual(expand(0x00, 0xE3), bytes.fromhex("33333333"))
        self.assertEqual(expand(0xFF, 0xE3), bytes.fromhex("EEEEEEEE"))
        self.assertEqual(expand(0xAA, 0xE3), bytes.fromhex("E3E3E3E3"))
        self.assertEqual(expand(0x81, 0xE3), bytes.fromhex("E333333E"))

    def test_link_layout_preserves_bios_and_game_ram(self):
        report = json.loads((ROOT / "build/user/dkkos/dkkos.report.json").read_text())
        image_end = 0x100 + report["header"]["image_size"]
        bss_end = image_end + report["header"]["bss_size"]
        self.assertLessEqual(bss_end, 0x6300)
        self.assertGreaterEqual(report["entry"]["address"], 0x2000)
        self.assertGreaterEqual(report["stack"]["base"], 0xC000)

    def test_all_hardware_bios_entries_are_hooked(self):
        source = (ROOT / "native.asm").read_text()
        expected = {
            "0x11c1", "0x18d4", "0x1979", "0x1ba3", "0x1c82",
            "0x1cca", "0x1d01", "0x1d3e", "0x1d57", "0x1f7c",
            "0x1f82", "0x1fc4", "0x1fd9", "0x1fdc", "0x1fdf",
            "0x1fe2", "0x1feb", "0x1ffd",
        }
        targets = set(re.findall(r"ld\s+de,#(0x[0-9a-f]+)", source))
        self.assertTrue(expected <= targets, expected - targets)


if __name__ == "__main__":
    unittest.main()
