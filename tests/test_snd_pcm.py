"""libsnd PCM playback (snd_pcm_*) on the production image.

Runs tests/snd/sndpcm.c from the card with the emulated Afternoon Blend
card, the CTC and the IOC card peer (tests/snd_harness.py), and checks the
DAC's byte stream against the stream's samples for both codecs, looping,
an early stop and the smallest staging buffer, plus the state every exit
leaves: the RT interrupt released and the DAC at its midpoint.
"""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import pcmconv  # noqa: E402
from snd_harness import SndReplay  # noqa: E402

EXE = os.path.join(ROOT, "build", "user", "sndpcm.exe")
DAC = 0xE4
RT_VECTOR = 0xEF08
RT_IDLE = 0xEF0B
BUDGET = 2_000_000_000
END = r"pcm: (end|open|snd_open|pcm_open -)[^\n]*\n"


def samples(count):
    """Every decoder path: smooth runs (nibbles), jumps (escapes in both
    nibble positions), a noisy stretch (raw blocks), and a count that is
    not a multiple of 256."""
    out = bytearray()
    value = 0x80
    i = 0
    while len(out) < count:
        if (i // 700) % 3 == 2:
            value = (i * 89 + (i >> 3) * 31) & 0xFF        # noise
        elif i % 97 == 0:
            value = (value + 0x55) & 0xFF                   # a jump
        else:
            value = (value + ((i * 5) % 15) - 7) & 0xFF     # small steps
        out.append(value)
        i += 1
    return bytes(out)


def stream(data, codec, rate=16000):
    if codec == pcmconv.CODEC_U8:
        return pcmconv.pack_header(rate, len(data), "T") + data
    blocks = []
    pred = 0x80
    for at in range(0, len(data), pcmconv.BLOCK):
        block = data[at:at + pcmconv.BLOCK]
        block += bytes([block[-1]]) * (pcmconv.BLOCK - len(block))
        blocks.append(pcmconv.encode_block(block, pred))
        pred = block[-1]
    payload = b"".join(blocks)
    return pcmconv.pack_header(rate, len(data), "T", codec,
                               len(payload)) + payload


@unittest.skipUnless(os.path.isfile(EXE), "run make")
class TestSndPcm(SndReplay):

    def play(self, pcm, mode=""):
        with open(EXE, "rb") as handle:
            files = {"SNDPCM.EXE": handle.read(), "T.PCM": pcm}
        command = "/fs/sndpcm.exe /fs/t.pcm %s\r" % mode
        z, since = self.start(files, command.encode())
        self.run_to_pattern(z, END, budget=BUDGET)
        self.run_to_pattern(z, r"/> $", budget=BUDGET)
        text = "".join(z.glyphs)
        dac = bytes(v for p, v, _t in z.sound_peer.writes[since:]
                    if p == DAC)
        return z, text, dac

    def end(self, text):
        found = re.search(r"pcm: end state (\d+) played (\d+) under (\d+) "
                          r"loops (\d+) reads (\d+) error (-?\d+)", text)
        self.assertIsNotNone(found, text[-400:])
        return [int(v) for v in found.groups()]

    def assert_released(self, z, dac):
        self.assertEqual(z.bank0[RT_VECTOR + 1] |
                         (z.bank0[RT_VECTOR + 2] << 8), RT_IDLE)
        self.assertEqual(z.rt_hz, 0)
        self.assertEqual(dac[-1], 0x80)
        # The lane never lost a byte, and the player asked for the gap that
        # covers its handler's page end.
        self.assertEqual(z.ioc_peer.a_overruns, 0)
        self.assertEqual(z.ioc_peer.rt_requests[0], (16000, 20))

    def assert_plays(self, data, codec, mode=""):
        z, text, dac = self.play(stream(data, codec), mode)
        self.assertIn("pcm: info codec %d rate 16000 samples %d"
                      % (codec, len(data)), text)
        state, played, under, _loops, _reads, error = self.end(text)
        self.assertEqual((state, under, error), (4, 0, 0))
        self.assertGreaterEqual(played, len(data))
        start = dac.find(data[:64])
        self.assertGreaterEqual(start, 0)
        self.assertEqual(dac[start:start + len(data)], data)
        self.assert_released(z, dac)
        return z, text

    def test_u8_byte_for_byte(self):
        self.assert_plays(samples(9000), pcmconv.CODEC_U8)

    def test_dpcm4_decodes_byte_for_byte(self):
        data = samples(9000)
        payload = stream(data, pcmconv.CODEC_DPCM4)[512:]
        # The fixture holds both block kinds.
        kinds = set()
        at = 0
        while at < len(payload):
            kinds.add(payload[at])
            at += 257 if payload[at] == 0 else payload[at + 1] + 3
        self.assertEqual(kinds, {0, 1})
        self.assert_plays(data, pcmconv.CODEC_DPCM4)

    def test_the_smallest_staging_buffer(self):
        self.assert_plays(samples(6000), pcmconv.CODEC_DPCM4, "small")

    def test_without_a_session_the_player_holds_the_card(self):
        self.assert_plays(samples(6000), pcmconv.CODEC_DPCM4, "bare")

    def test_staging_in_the_rt_region(self):
        self.assert_plays(samples(9000), pcmconv.CODEC_DPCM4, "rtstage")
        self.assert_plays(samples(9000), pcmconv.CODEC_U8, "rtstage")

    def test_pause_and_resume_keep_the_stream_whole(self):
        data = samples(16000)
        z, text, dac = self.play(stream(data, pcmconv.CODEC_DPCM4), "pause")
        self.assertIn("pcm: pause 0", text)
        self.assertIn("pcm: resume 0", text)
        at, state = (int(v) for v in re.search(
            r"pcm: paused at (\d+) state (\d+)", text).groups())
        self.assertEqual(state, 6)
        state, played, under, _loops, _reads, error = self.end(text)
        self.assertEqual((state, under, error), (4, 0, 0))
        # The samples before the pause, then (after the glides down and up)
        # every sample from the pause on, nothing lost or repeated.
        start = dac.find(data[:64])
        self.assertEqual(dac[start:start + at], data[:at])
        rest = dac.find(data[at:at + 64], start + at)
        self.assertGreater(rest, start + at)
        self.assertEqual(dac[rest:rest + len(data) - at], data[at:])
        # The clock stopped for the pause and started again with the
        # interrupt held throughout (one open), then the close stops it
        # (and the hold's release once more).
        rates = [hz for hz, _gap in z.ioc_peer.rt_requests]
        runs = [hz for i, hz in enumerate(rates) if i == 0 or hz != rates[i - 1]]
        self.assertEqual(runs, [16000, 0, 16000, 0])
        self.assertEqual(rates[:3], [16000, 0, 16000])
        self.assert_released(z, dac)

    def test_loop_restarts_at_the_start(self):
        data = samples(1500)
        z, text, dac = self.play(stream(data, pcmconv.CODEC_DPCM4), "loop")
        _state, _played, under, loops, _reads, error = self.end(text)
        self.assertEqual((under, error), (0, 0))
        self.assertGreaterEqual(loops, 2)
        # Page-granular: each pass is the stream padded to whole pages.
        pages = (len(data) + 255) // 256 * 256
        one = data + bytes([data[-1]]) * (pages - len(data))
        start = dac.find(one[:64])
        self.assertEqual(dac[start:start + 2 * pages], one + one)
        self.assert_released(z, dac)

    def test_an_early_stop(self):
        data = samples(32000)
        z, text, dac = self.play(stream(data, pcmconv.CODEC_U8), "stop")
        state, played, _under, _loops, _reads, error = self.end(text)
        self.assertEqual((state, error), (4, 0))
        self.assertLess(played, len(data) // 2)
        self.assert_released(z, dac)

    def test_an_unknown_codec_is_refused(self):
        pcm = bytearray(stream(samples(300), pcmconv.CODEC_U8))
        pcm[5] = 2
        z, text, _dac = self.play(bytes(pcm))
        self.assertIn("pcm: pcm_open -7", text)
        self.assertEqual(z.ioc_peer.rt_requests, [])


if __name__ == "__main__":
    unittest.main()
