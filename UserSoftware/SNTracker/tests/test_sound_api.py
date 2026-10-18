import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class SoundApiTests(unittest.TestCase):
    def test_tracker_batches_one_tick_through_sound_driver(self):
        source = (ROOT / "psg.c").read_text()
        self.assertIn('#include "kos/sound.h"', source)
        self.assertNotIn("diag_io_out", source)
        self.assertIn("sound_open(1u)", source)
        self.assertIn("sound_write(sound_handle, psg_batch, psg_batch_count)",
                      source)
        self.assertIn("sound_reset(sound_handle)", source)
        self.assertIn("sound_close(sound_handle)", source)
        capacity = int(re.search(r"#define PSG_BATCH_WRITES\s+(\d+)u",
                                 source).group(1))
        # Twelve tone channels can emit pitch+volume (3 writes each), and
        # four noise channels control+volume (2 each): 44 maximum per tick.
        self.assertGreaterEqual(capacity, 44)

    def test_each_logical_chip_maps_to_a_sound_target(self):
        source = (ROOT / "psg.c").read_text()
        self.assertGreaterEqual(source.count("unsigned char target = number >> 2;"),
                                3)
        self.assertNotIn("PSG_FIRST_PORT", source)
        self.assertNotIn("diag_io_out", source)


if __name__ == "__main__":
    unittest.main()
