import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class SoundApiTests(unittest.TestCase):
    def test_player_streams_through_libsnd(self):
        source = (ROOT / "vgmplay.c").read_text()
        self.assertIn('#include "kos/snd.h"', source)
        self.assertNotIn("diag_io_out", source)
        self.assertNotIn('#include "diag.h"', source)
        self.assertNotIn("sound_out", source)
        self.assertIn("snd_open()", source)
        self.assertIn("snd_close()", source)
        self.assertIn("kos_reader_from_stdio(&reader, file_stream)", source)
        self.assertIn("snd_zvg_open(&reader, 0u)", source)
        self.assertIn("snd_zvg_prepare()", source)
        self.assertIn("snd_zvg_play()", source)
        self.assertIn("snd_zvg_get_status(&status)", source)
        self.assertIn("snd_zvg_close()", source)

    def test_the_timer_handler_ticks_libsnd(self):
        source = (ROOT / "vgmplay.c").read_text()
        self.assertIn("snd_timer_period(&period)", source)
        self.assertIn("notify(timer_handle, on_timer_event)", source)
        self.assertIn("timer_event(timer_handle, (unsigned int)period)", source)
        self.assertIn("snd_tick((uint16_t)count)", source)

    def test_player_does_not_hardcode_afternoon_blend_ports(self):
        source = (ROOT / "vgmplay.c").read_text()
        for port in range(0xE0, 0xE5):
            self.assertNotIn("0x%02X" % port, source)

    def test_empty_console_poll_idles_until_an_interrupt(self):
        source = (ROOT / "vgmplay.c").read_text()
        self.assertIn("snd_service()", source)
        self.assertIn("if (console_poll() > 0)", source)
        self.assertIn("idle();", source)
        self.assertNotIn("key_poll_due", source)
        self.assertNotIn("key_poll_count", source)


if __name__ == "__main__":
    unittest.main()
