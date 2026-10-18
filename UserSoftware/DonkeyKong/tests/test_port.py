import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOFTWARE = (ROOT.parents[2] / "Zephyr-80" / "Code" / "HOST" /
            "Software" / "disk1" / "8")


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

    def test_g4_mode_and_takeover_state_are_explicit(self):
        source = (ROOT / "native.asm").read_text()
        init = source.split("_dk_video_init::", 1)[1].split(
            "_dk_video_shutdown::", 1)[0]
        self.assertRegex(init, r"ld\s+a,#0x06\s+ld\s+b,#0")
        self.assertRegex(init, r"ld\s+b,#46\s+call\s+vdp_reg")
        for register in (14, 16, 17, 18, 23, 26, 27):
            self.assertRegex(
                init, rf"xor\s+a\s+ld\s+b,#{register}\s+call\s+vdp_reg")
        self.assertRegex(init, r"ld\s+a,#2\s+ld\s+b,#15\s+call\s+vdp_reg")
        clear = source.split("clear_bitmap:", 1)[1].split(
            "clear_command:", 1)[0]
        self.assertEqual(clear.count("call    vdp_wait_command"), 2)

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

    def test_every_low_bios_parameter_decoder_call_is_relocated(self):
        bios = (SOFTWARE / "coleco.rom").read_bytes()
        call_sites = {
            0x0209, 0x0257, 0x048E, 0x065B, 0x06CD, 0x0FA0,
            0x0FBE, 0x104A, 0x10C5, 0x1B14, 0x1B92, 0x1C16,
            0x1C60, 0x1C7C, 0x1CC2, 0x1CF3, 0x1D30,
        }
        self.assertEqual(
            {offset for offset in range(0x015D, len(bios) - 2)
             if bios[offset:offset + 3] == b"\xCD\x98\x00"},
            call_sites)
        source = (ROOT / "native.asm").read_text()
        patched_operands = {
            int(value, 16)
            for value in re.findall(r"ld\s+\(0x([0-9a-f]+)\),hl", source)
        }
        self.assertTrue({site + 1 for site in call_sites} <= patched_operands)
        self.assertIn("native_param_decoder:", source)

    def test_vdp_event_drives_the_original_nmi_frame(self):
        glue = (ROOT / "main.c").read_text()
        native = (ROOT / "native.asm").read_text()
        self.assertIn("notify(video_handle, on_frame)", glue)
        self.assertNotIn("timer_event(", glue)
        self.assertNotIn("timer_open(", glue)
        handler = glue.split("static void on_frame", 1)[1].split(
            "unsigned int dk_input_poll", 1)[0]
        self.assertIn("input_pump();", handler)
        self.assertIn("event_latch = aperture_open(cart_token)", handler)
        self.assertIn("aperture_close(event_latch)", handler)
        self.assertIn("_dk_frame::", native)
        frame = native.split("_dk_frame::", 1)[1].split("\n\n", 1)[0]
        self.assertIn("call    0x852e", frame)
        self.assertIn("display on, 16x16, IE0 on", native)
        self.assertIn("abort_if_requested:", native)
        self.assertIn("ld      a,(_dk_quit_requested)", native)
        self.assertIn("ld      a,(_dk_in_event)", native)
        self.assertRegex(
            native,
            r"xor\s+a\s+ld\s+b,#15\s+call\s+vdp_reg\s+"
            r"ld\s+a,#0x62")

    def test_vdp_control_pairs_are_interrupt_atomic(self):
        native = (ROOT / "native.asm").read_text()
        reg = native.split("vdp_reg:", 1)[1].split(
            "vdp_reg_unmasked:", 1)[0]
        self.assertIn("di", reg)
        self.assertIn("ei", reg)
        for label in ("vdp_set_write:", "vdp_set_read:"):
            routine = native.split(label, 1)[1].split("\n\n", 1)[0]
            self.assertIn("di", routine)
            self.assertIn("call    vdp_reg_unmasked", routine)
            self.assertIn("ei", routine)


if __name__ == "__main__":
    unittest.main()
