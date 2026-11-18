"""pcmconv.py (the SDK's PCM tool): the PCM v1 writer, the reader, the
DPCM4 codec and the conversions.

The WAV fixtures are synthesized with the standard library; the MP3 and
the resampled cases need FFmpeg and are skipped without it.
"""

import array
import io
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOL = os.path.join(ROOT, "tools", "pcmconv.py")
sys.path.insert(0, os.path.join(ROOT, "tools"))

import pcmconv  # noqa: E402

HAVE_FFMPEG = shutil.which("ffmpeg") is not None


def write_wav(path, rate, channels, frames, width=2):
    """`frames` is a list of per-channel integer tuples."""
    with wave.open(path, "wb") as writer:
        writer.setnchannels(channels)
        writer.setsampwidth(width)
        writer.setframerate(rate)
        data = bytearray()
        for frame in frames:
            for value in frame:
                if width == 1:
                    data.append(value + 128)
                else:
                    data += int(value).to_bytes(width, "little", signed=True)
        writer.writeframes(bytes(data))


def convert(*args, env=None):
    return subprocess.run([sys.executable, TOOL] + list(args),
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          env=env)


class HeaderTests(unittest.TestCase):
    def test_header_layout_is_the_specified_bytes(self):
        raw = pcmconv.pack_header(16000, 48000, "West of House")
        self.assertEqual(len(raw), 512)
        self.assertEqual(raw[0:4], b"KPCM")
        self.assertEqual(raw[4:8], bytes([1, 0, 1, 8]))
        self.assertEqual(struct.unpack_from("<IIIIHHI", raw, 8),
                         (16000, 48000, 48000, 512, 512, 0, 0))
        self.assertEqual(raw[32:45], b"West of House")
        self.assertEqual(raw[45:], bytes(512 - 45))

    def test_long_titles_are_cut_to_63_bytes_and_terminated(self):
        raw = pcmconv.pack_header(16000, 1, "T" * 100)
        self.assertEqual(raw[32:95], b"T" * 63)
        self.assertEqual(raw[95], 0)
        self.assertEqual(pcmconv.parse_header(raw)["name"], "T" * 63)

    def test_parse_rejects_what_the_player_rejects(self):
        good = bytearray(pcmconv.pack_header(16000, 10, "x"))
        self.assertEqual(pcmconv.parse_header(bytes(good))["samples"], 10)
        for offset, value in ((0, ord("k")), (4, 2), (5, 2), (6, 2),
                              (7, 16)):
            bad = bytearray(good)
            bad[offset] = value
            with self.assertRaises(pcmconv.ConvError):
                pcmconv.parse_header(bytes(bad))
        bad = bytearray(good)
        struct.pack_into("<I", bad, 16, 11)
        with self.assertRaises(pcmconv.ConvError):
            pcmconv.parse_header(bytes(bad))


class QuantizerTests(unittest.TestCase):
    def test_undithered_levels_are_exact(self):
        q = pcmconv.Quantizer(1.0, "none", 1)
        out = q(array.array("f", [0.0, 0.5, -0.5, -1.0, 0.9921875, 1.5]))
        self.assertEqual(list(out), [128, 192, 64, 0, 255, 255])
        self.assertEqual(q.clipped, 1)

    def test_dither_is_seeded_and_stays_within_one_lsb(self):
        values = array.array("f", [0.25] * 4000)
        a = pcmconv.Quantizer(1.0, "tpdf", 7)(values)
        b = pcmconv.Quantizer(1.0, "tpdf", 7)(values)
        self.assertEqual(a, b)
        self.assertTrue(set(a) <= {159, 160, 161})
        self.assertAlmostEqual(sum(a) / len(a), 160.0, delta=0.1)


class WavTests(unittest.TestCase):
    """Conversions whose rate needs no resampling: the payload is fixed
    by the quantizer, byte for byte, with or without FFmpeg."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.dir)

    def path(self, name):
        return os.path.join(self.dir, name)

    def check_file(self, path, rate, samples, title):
        with open(path, "rb") as handle:
            data = handle.read()
        self.assertEqual(len(data), 512 + samples)
        self.assertEqual(data[:512], pcmconv.pack_header(rate, samples, title))
        return data[512:]

    def test_mono_silence_is_all_midpoint(self):
        write_wav(self.path("s.wav"), 16000, 1, [(0,)] * 1000)
        result = convert(self.path("s.wav"), "-o", self.path("s.pcm"),
                         "--dither", "none")
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = self.check_file(self.path("s.pcm"), 16000, 1000, "s")
        self.assertEqual(payload, b"\x80" * 1000)
        self.assertIn(b"1000 samples", result.stdout)
        self.assertIn(b"1512 bytes", result.stdout)

    def test_stereo_is_downmixed_by_averaging(self):
        # L = +0.5, R = -0.25 full scale: the mono sample is +0.125.
        frames = [(16384, -8192), (-16384, -16384), (32767, 32767)] * 100
        write_wav(self.path("st.wav"), 16000, 2, frames)
        result = convert(self.path("st.wav"), "-o", self.path("st.pcm"),
                         "--dither", "none", "--title", "Stereo")
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = self.check_file(self.path("st.pcm"), 16000, 300, "Stereo")
        self.assertEqual(payload[:3], bytes([144, 64, 255]))

    def test_square_wave_round_trips_through_the_wav_export(self):
        frames = [((16384 if (i // 20) % 2 else -16384),)
                  for i in range(16000)]
        write_wav(self.path("sq.wav"), 16000, 1, frames)
        self.assertEqual(convert(self.path("sq.wav"), "-o",
                                 self.path("sq.pcm"),
                                 "--dither", "none").returncode, 0)
        payload = self.check_file(self.path("sq.pcm"), 16000, 16000, "sq")
        self.assertEqual(set(payload), {64, 192})
        result = convert("--to-wav", self.path("sq.pcm"),
                         "-o", self.path("back.wav"))
        self.assertEqual(result.returncode, 0, result.stderr)
        with wave.open(self.path("back.wav"), "rb") as reader:
            self.assertEqual((reader.getnchannels(), reader.getsampwidth(),
                              reader.getframerate()), (1, 1, 16000))
            self.assertEqual(reader.readframes(16000), payload)

    def test_short_clip_and_8_bit_source(self):
        write_wav(self.path("one.wav"), 16000, 1, [(64,)], width=1)
        self.assertEqual(convert(self.path("one.wav"), "-o",
                                 self.path("one.pcm"),
                                 "--dither", "none").returncode, 0)
        self.assertEqual(self.check_file(self.path("one.pcm"), 16000, 1,
                                         "one"), bytes([192]))

    def test_the_same_options_give_the_same_bytes(self):
        frames = [(int(12000 * math.sin(i / 7.0)),) for i in range(5000)]
        write_wav(self.path("sin.wav"), 16000, 1, frames)
        convert(self.path("sin.wav"), "-o", self.path("a.pcm"))
        convert(self.path("sin.wav"), "-o", self.path("b.pcm"))
        with open(self.path("a.pcm"), "rb") as a, \
                open(self.path("b.pcm"), "rb") as b:
            self.assertEqual(a.read(), b.read())

    def test_info_prints_the_header(self):
        write_wav(self.path("i.wav"), 16000, 1, [(0,)] * 32000)
        convert(self.path("i.wav"), "-o", self.path("i.pcm"),
                "--title", "Info test")
        result = convert("--info", self.path("i.pcm"))
        self.assertEqual(result.returncode, 0)
        self.assertIn(b'"Info test" 0:02.000, 32000 samples, 16000 Hz',
                      result.stdout)

    def test_bad_input_fails_cleanly(self):
        with open(self.path("junk.wav"), "wb") as handle:
            handle.write(b"not audio at all")
        result = convert(self.path("junk.wav"), "-o", self.path("j.pcm"))
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"pcmconv:", result.stderr)
        self.assertFalse(os.path.exists(self.path("j.pcm")))
        result = convert(self.path("missing.wav"))
        self.assertEqual(result.returncode, 1)
        result = convert("--info", self.path("junk.wav"))
        self.assertEqual(result.returncode, 1)
        result = convert(self.path("junk.wav"), "--rate", "48000")
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"outside", result.stderr)

    def test_truncated_pcm_is_refused(self):
        with open(self.path("t.pcm"), "wb") as handle:
            handle.write(pcmconv.pack_header(16000, 100, "t") + bytes(50))
        result = convert("--to-wav", self.path("t.pcm"))
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"promises", result.stderr)

    def test_without_ffmpeg(self):
        tools = os.path.join(self.dir, "bin")
        os.mkdir(tools)
        env = dict(os.environ, PATH=tools)
        write_wav(self.path("w.wav"), 16000, 1, [(0,)] * 10)
        write_wav(self.path("w44.wav"), 44100, 1, [(0,)] * 10)
        with open(self.path("m.mp3"), "wb") as handle:
            handle.write(b"\xff\xfb" + bytes(100))
        self.assertEqual(convert(self.path("w.wav"), "-o",
                                 self.path("w.pcm"), env=env).returncode, 0)
        result = convert(self.path("w44.wav"), env=env)
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"needs ffmpeg", result.stderr)
        result = convert(self.path("m.mp3"), env=env)
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"needs ffmpeg", result.stderr)


@unittest.skipUnless(HAVE_FFMPEG, "ffmpeg is not installed")
class FfmpegTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.dir)

    def path(self, name):
        return os.path.join(self.dir, name)

    def lavfi(self, name, source, *extra):
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi",
                        "-i", source] + list(extra) + [self.path(name)],
                       check=True)

    def read_payload(self, name):
        with open(self.path(name), "rb") as handle:
            data = handle.read()
        info = pcmconv.parse_header(data[:512])
        self.assertEqual(len(data), 512 + info["samples"])
        return info, data[512:]

    def tone_level(self, payload, start=1000):
        body = payload[start:len(payload) - start]
        return max(body) - 128, 128 - min(body)

    def test_44k_stereo_wav_is_resampled(self):
        self.lavfi("t.wav", "sine=frequency=440:sample_rate=44100:"
                   "duration=2", "-ac", "2")
        result = convert(self.path("t.wav"), "-o", self.path("t.pcm"))
        self.assertEqual(result.returncode, 0, result.stderr)
        info, payload = self.read_payload("t.pcm")
        self.assertEqual(info["rate"], 16000)
        self.assertAlmostEqual(info["samples"], 32000, delta=16)
        # lavfi's sine is 1/8 full scale, up-mixed to stereo at -3 dB:
        # about 11 steps either way after the average.
        high, low = self.tone_level(payload)
        self.assertTrue(9 <= high <= 14 and 9 <= low <= 14, (high, low))

    def test_mp3_with_normalize(self):
        self.lavfi("t.mp3", "sine=frequency=330:sample_rate=48000:"
                   "duration=3", "-b:a", "128k")
        result = convert(self.path("t.mp3"), "-o", self.path("t.pcm"),
                         "--normalize", "--title", "MP3")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"0 clipped", result.stdout)
        info, payload = self.read_payload("t.pcm")
        self.assertEqual(info["name"], "MP3")
        high, low = self.tone_level(payload)
        self.assertGreaterEqual(min(high, low), 120)
        self.assertTrue(min(payload) >= 0 and max(payload) <= 255)

    def test_other_rates(self):
        self.lavfi("t.wav", "sine=frequency=200:sample_rate=22050:"
                   "duration=1")
        for rate in ("8000", "11025", "22050"):
            result = convert(self.path("t.wav"), "-o", self.path("r.pcm"),
                             "--rate", rate)
            self.assertEqual(result.returncode, 0, result.stderr)
            info, _payload = self.read_payload("r.pcm")
            self.assertEqual(info["rate"], int(rate))
            self.assertAlmostEqual(info["samples"], int(rate),
                                   delta=int(rate) // 100)

    def test_aliasing_is_filtered(self):
        # 12 kHz is above the 8 kHz Nyquist limit of 16 kHz output: the
        # anti-alias filter leaves (dithered) silence, not a 4 kHz alias.
        self.lavfi("hi.wav", "sine=frequency=12000:sample_rate=48000:"
                   "duration=1", "-af", "volume=8")
        convert(self.path("hi.wav"), "-o", self.path("hi.pcm"),
                "--dither", "none")
        _info, payload = self.read_payload("hi.pcm")
        high, low = self.tone_level(payload)
        self.assertLessEqual(max(high, low), 2)

    def test_multi_minute_song_streams(self):
        self.lavfi("long.mp3", "sine=frequency=440:sample_rate=44100:"
                   "duration=200", "-b:a", "64k")
        result = convert(self.path("long.mp3"), "-o", self.path("long.pcm"))
        self.assertEqual(result.returncode, 0, result.stderr)
        info = pcmconv.read_pcm(self.path("long.pcm"))
        self.assertAlmostEqual(info["samples"], 3200000, delta=3000)
        self.assertIn(b"3:20", result.stdout)

    def test_invalid_media_fails_cleanly(self):
        with open(self.path("bad.mp3"), "wb") as handle:
            handle.write(b"definitely not an mp3")
        result = convert(self.path("bad.mp3"), "-o", self.path("bad.pcm"))
        self.assertEqual(result.returncode, 1)
        self.assertIn(b"ffmpeg could not decode", result.stderr)
        self.assertFalse(os.path.exists(self.path("bad.pcm")))
        self.assertFalse(os.path.exists(self.path("bad.pcm.part")))


class Dpcm4Tests(unittest.TestCase):
    """The lossless DPCM4 codec (docs/snd-sdk.md section 12.2)."""

    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.dir)

    def path(self, name):
        return os.path.join(self.dir, name)

    def round_trip(self, block, pred=0x80):
        stored = pcmconv.encode_block(block, pred)
        decoded, end = pcmconv.decode_block(stored, 0, pred)
        self.assertEqual(decoded, bytes(block))
        self.assertEqual(end, len(stored))
        return stored

    def test_small_steps_are_nibbles(self):
        value = 0x80
        block = bytearray()
        for i in range(256):
            value = (value + ((i * 5) % 15) - 7) & 0xFF
            block.append(value)
        stored = self.round_trip(bytes(block))
        self.assertEqual(stored[0], pcmconv.BLOCK_DPCM4)
        self.assertEqual(len(stored), 2 + 128)

    def test_jumps_escape_in_both_nibble_positions(self):
        block = bytearray([0x80] * 256)
        block[3] = 0x20             # an escape in a high nibble
        block[8] = 0xF0             # ... and in a low one
        block[9] = 0x80
        stored = self.round_trip(bytes(block))
        self.assertEqual(stored[0], pcmconv.BLOCK_DPCM4)

    def test_noise_is_stored_raw(self):
        block = bytes((i * 89 + 17) & 0xFF for i in range(256))
        stored = self.round_trip(block)
        self.assertEqual(stored[0], pcmconv.BLOCK_RAW)
        self.assertEqual(len(stored), 257)

    def test_the_predictor_carries_across_blocks(self):
        block = bytes([0x90] * 256)
        stored = pcmconv.encode_block(block, 0x8E)
        self.assertEqual(len(stored), 2 + 128)
        self.assertEqual(pcmconv.decode_block(stored, 0, 0x8E)[0], block)

    def test_recode_is_exact_and_about_half_the_size(self):
        samples = bytes((0x80 + int(60 * math.sin(i / 9.0))) & 0xFF
                        for i in range(10000))
        with open(self.path("a.pcm"), "wb") as handle:
            handle.write(pcmconv.pack_header(16000, len(samples), "S") +
                         samples)
        out = io.StringIO()
        pcmconv.recode(self.path("a.pcm"), self.path("b.pcm"),
                       pcmconv.CODEC_DPCM4, out=out)
        info = pcmconv.read_pcm(self.path("b.pcm"))
        self.assertEqual(info["codec"], pcmconv.CODEC_DPCM4)
        self.assertEqual(info["samples"], len(samples))
        self.assertLess(info["payload"], len(samples) * 0.6)
        pcmconv.recode(self.path("b.pcm"), self.path("c.pcm"),
                       pcmconv.CODEC_U8, out=out)
        with open(self.path("c.pcm"), "rb") as handle:
            self.assertEqual(handle.read()[512:], samples)

    def test_convert_writes_dpcm4_directly(self):
        write_wav(self.path("t.wav"), 16000, 1,
                  [(int(8000 * math.sin(i / 7.0)),) for i in range(5000)])
        out = io.StringIO()
        pcmconv.convert(self.path("t.wav"), self.path("t1.pcm"), "T", 16000,
                        0.0, False, "none", 1, out=out,
                        codec=pcmconv.CODEC_DPCM4)
        pcmconv.convert(self.path("t.wav"), self.path("t0.pcm"), "T", 16000,
                        0.0, False, "none", 1, out=io.StringIO())
        pcmconv.recode(self.path("t1.pcm"), self.path("t2.pcm"),
                       pcmconv.CODEC_U8, out=out)
        with open(self.path("t0.pcm"), "rb") as a, \
                open(self.path("t2.pcm"), "rb") as b:
            self.assertEqual(a.read(), b.read())
        self.assertIn("codec dpcm4", out.getvalue())


if __name__ == "__main__":
    unittest.main()
