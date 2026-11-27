"""PCMPlay on the production image in the Zephyr-80 simulator.

Boots ZephyrOS's production image of BOARD (mk2 by default, mk1) with the
emulated Afternoon Blend card,
the CTC and the IOC card peer (tests/snd_harness.py), runs pcmplay.exe
(a client of libsnd's snd_pcm_*) from the card and checks the DAC's byte
stream against the file for both codecs, the report, and the state every
exit path must restore: the real-time interrupt released (the NMI vector
idle, the IO controller's RT clock stopped) and the card.

Skipped when the ZephyrOS image or its test harness is not there.
"""

import io
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ZEPHYR = os.path.abspath(os.path.join(ROOT, "..", ".."))
EXE = os.path.join(ROOT, "build", "user", "pcmplay.exe")
sys.path.insert(0, os.path.join(ZEPHYR, "tests"))

from replay import BOARDS, Mk2Board, app_board  # noqa: E402

BOARD = app_board()
SPEC = BOARDS[BOARD]
IMAGE = os.path.join(ZEPHYR, SPEC["subdir"], SPEC["image"])
sys.path.insert(0, os.path.join(ROOT, "build", SPEC["sdk"], "tools"))

import pcmconv  # noqa: E402

try:
    from snd_harness import SndReplay  # noqa: E402
    HARNESS = os.path.isfile(IMAGE)
except ImportError:
    SndReplay = unittest.TestCase
    HARNESS = False
if BOARD == "mk2" and HARNESS:
    class SndReplay(Mk2Board, SndReplay):  # noqa: F811
        """The sound harness on the Mk2 image."""

DAC = 0xE4
RT_VECTOR = 0xEF08              # JP target: the handler, or the idle RETN
RT_IDLE = 0xEF0B
BUDGET = 2_000_000_000
END = (r"(Elapsed \d+|no RT clock|real-time interrupt|not a valid|"
       r"unsupported|Usage)[^\n]*\n")


def pattern(count, seed=0):
    """A payload no two neighbouring windows share."""
    return bytes(((i * 7) ^ (i >> 8) ^ seed) & 0xFF for i in range(count))


@unittest.skipUnless(HARNESS and os.path.isfile(EXE),
                     "needs ZephyrOS build/konaos.bin and pcmplay.exe")
class PcmPlaySim(SndReplay):

    def setUp(self):
        self.load_image(SPEC["subdir"], SPEC["image"], SPEC["map"])

    def run_player(self, files, args, before=None, during=None, end=END):
        with open(EXE, "rb") as handle:
            files = dict(files, **{"PCMPLAY.EXE": handle.read()})
        z, since = self.start(files, ("/fs/pcmplay.exe %s\r" % args).encode())
        dac_since = len(z.sound_peer.writes)
        if before:
            before(z)
        if during:
            self.run_to_pattern(z, "Playing", budget=BUDGET)
            during(z)
        self.run_to_pattern(z, end, budget=BUDGET)
        self.run_to_pattern(z, r"/> $", budget=BUDGET)
        text = "".join(z.glyphs)
        dac = bytes(v for p, v, _t in z.sound_peer.writes[dac_since:]
                    if p == DAC)
        return z, text, dac

    def word(self, z, address):
        return z.rd16(address)

    def assert_restored(self, z):
        self.assertEqual(self.word(z, RT_VECTOR + 1), RT_IDLE)
        self.assertEqual(z.rt_hz, 0)
        self.assertEqual(z.ioc_peer.rt_requests[-1][0], 0)

    def test_plays_the_payload_byte_for_byte_and_restores(self):
        samples = 20000
        payload = pattern(samples)
        pcm = pcmconv.pack_header(16000, samples, "Sim tone") + payload
        z, text, dac = self.run_player({"T.PCM": pcm}, "/fs/t.pcm")
        self.assertIn("Title: Sim tone", text)
        self.assertIn("Codec: u8, 20000 bytes", text)
        self.assertIn("Rate: 16000 Hz on the RT clock, gap 20 us", text)
        self.assertIn("Playback complete.", text)
        self.assertIn("Samples 20000  underruns 0", text)
        start = dac.find(payload[:256])
        self.assertGreaterEqual(start, 0)
        self.assertEqual(dac[start:start + samples], payload)
        # The last page is padded with the final sample; after the drain
        # the card's release sets the midpoint.
        padding = dac[start + samples:start + samples + 256 - samples % 256]
        self.assertEqual(set(padding), {payload[-1]})
        self.assertEqual(dac[-1], 0x80)
        self.assert_restored(z)
        # Every tick was an NMI, and the lane lost no byte.
        self.assertEqual(z.nmi_count, z.rt_ticks)
        self.assertEqual(z.ioc_peer.a_overruns, 0)
        self.assertEqual(z.ioc_peer.rt_requests[0], (16000, 20))

    def test_plays_dpcm4_byte_for_byte(self):
        samples = 20000
        payload = pattern(samples)
        u8 = pcmconv.pack_header(16000, samples, "Sim") + payload
        with open(os.path.join(ROOT, "build", "u8.pcm"), "wb") as handle:
            handle.write(u8)
        pcmconv.recode(os.path.join(ROOT, "build", "u8.pcm"),
                       os.path.join(ROOT, "build", "d4.pcm"),
                       pcmconv.CODEC_DPCM4, out=io.StringIO())
        with open(os.path.join(ROOT, "build", "d4.pcm"), "rb") as handle:
            d4 = handle.read()
        z, text, dac = self.run_player({"D.PCM": d4}, "/fs/d.pcm")
        self.assertIn("Codec: dpcm4", text)
        self.assertIn("Playback complete.", text)
        start = dac.find(payload[:256])
        self.assertEqual(dac[start:start + samples], payload)
        self.assertEqual(z.ioc_peer.a_overruns, 0)
        self.assert_restored(z)

    def test_info_only_plays_nothing(self):
        pcm = pcmconv.pack_header(8000, 16000, "Info") + bytes(16000)
        z, text, dac = self.run_player({"I.PCM": pcm}, "-i /fs/i.pcm",
                                       end=r"RT clock[^\n]*\n")
        self.assertIn("Length: 0:02  (16000 samples)", text)
        self.assertIn("Rate: 8000 Hz on the RT clock", text)
        self.assertEqual(z.ioc_peer.rt_requests, [])
        # Only the session's release touches the DAC: the card's reset
        # sets the midpoint.
        self.assertLessEqual(set(dac), {0x80})

    def test_a_damaged_header_is_refused(self):
        pcm = bytearray(pcmconv.pack_header(16000, 100, "x") + bytes(100))
        pcm[5] = 2
        z, text, _dac = self.run_player({"B.PCM": bytes(pcm)}, "/fs/b.pcm")
        self.assertIn("unsupported codec or no DAC (-7)", text)
        self.assertEqual(z.ioc_peer.rt_requests, [])
        pcm = pcmconv.pack_header(16000, 1000, "x") + bytes(10)
        z, text, _dac = self.run_player({"S.PCM": pcm}, "/fs/s.pcm")
        self.assertIn("not a valid KonaOS PCM v1 file", text)

    def test_a_key_stops_playback_and_restores(self):
        samples = 160000
        pcm = pcmconv.pack_header(16000, samples, "Long") + pattern(samples)

        def press(z):
            self.run_to_pattern(z, r"0:01 / ", budget=BUDGET)
            z.hid.hid_queue.extend(b" ")

        z, text, dac = self.run_player({"L.PCM": pcm}, "/fs/l.pcm",
                                       during=press)
        self.assertIn("Playback stopped.", text)
        played = int(re.search(r"Samples (\d+)", text).group(1))
        self.assertLess(played, samples // 2)
        self.assertEqual(dac[-1], 0x80)
        self.assert_restored(z)

    def test_a_controller_without_the_rt_clock(self):
        pcm = pcmconv.pack_header(16000, 4000, "Old IOC") + pattern(4000)

        def old_firmware(z):
            z.ioc_peer.rt_clock_supported = False

        z, text, dac = self.run_player({"V.PCM": pcm}, "/fs/v.pcm",
                                       before=old_firmware)
        self.assertIn("this IO controller has no RT clock", text)
        self.assertEqual(self.word(z, RT_VECTOR + 1), RT_IDLE)
        self.assertEqual(z.nmi_count, 0)

if __name__ == "__main__":
    unittest.main()
