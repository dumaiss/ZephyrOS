import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class SoundApiTests(unittest.TestCase):
    def test_player_uses_exclusive_direct_sound_api(self):
        source = (ROOT / "vgmplay.c").read_text()
        self.assertIn('#include "kos/sound.h"', source)
        self.assertNotIn("diag_io_out", source)
        self.assertNotIn('#include "diag.h"', source)
        self.assertIn("sound_open(1u)", source)
        self.assertIn("sound_info(sound_handle, &info)", source)
        self.assertIn("KSOUND_CAP_DIRECT_IO", source)
        self.assertIn("psg0_port = info.psg_ports[0]", source)
        self.assertIn("sound_out(psg0_port, value)", source)
        self.assertIn("sound_close(sound_handle)", source)

    def test_player_does_not_hardcode_afternoon_blend_ports(self):
        source = (ROOT / "vgmplay.c").read_text()
        for port in range(0xE0, 0xE5):
            self.assertNotIn("0x%02X" % port, source)


if __name__ == "__main__":
    unittest.main()
