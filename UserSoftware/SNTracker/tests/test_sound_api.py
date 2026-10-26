import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class SoundApiTests(unittest.TestCase):
    def setUp(self):
        self.source = (ROOT / "sntracker.c").read_text()
        self.makefile = (ROOT / "Makefile").read_text()

    def test_tracker_is_a_streamed_libsnd_client(self):
        self.assertIn("kos_reader_from_stdio(&song_reader, song_file)",
                      self.source)
        self.assertIn("snd_ztr_open(&song_reader)", self.source)
        self.assertIn("snd_ztr_prepare()", self.source)
        self.assertIn("snd_ztr_play()", self.source)
        self.assertIn("snd_service()", self.source)

    def test_timer_handler_advances_libsnd(self):
        self.assertIn("snd_tick((uint16_t)count)", self.source)
        self.assertIn("snd_timer_period(&period)", self.source)

    def test_build_uses_sdk_020_and_only_the_client_source(self):
        self.assertIn("kos-sdk-0.2.0", self.makefile)
        self.assertIn("sntracker.c,STACK=1024,LIBS=snd", self.makefile)
        self.assertNotIn("ztr.c player.c psg.c", self.makefile)

    def test_empty_console_poll_idles_until_an_interrupt(self):
        self.assertIn("if (console_poll() > 0)", self.source)
        self.assertIn("idle();", self.source)


if __name__ == "__main__":
    unittest.main()
