#!/usr/bin/env python3
"""The KIMG v1 image format: the shipped header and the converter.

Host tests only: `user/sdk/include/kos/kimg.h` pins the format's
layout and constants; `tools/kimg.py` converts an image and the tests
parse the result byte-for-byte (magic, version, mode, geometry,
palette and the payload in each mode's packing).  The converter needs
Pillow, which is not a build dependency: its tests skip without it.
The committed fixtures `tests/gfx/fixtures/*.kimg` are parsed here
too, so a regeneration that drifts from the format fails the suite.
"""

import os
import hashlib
import random
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from test_ioc import struct_member_order  # noqa: E402

import kimg  # noqa: E402

KIMG_H = os.path.join(ROOT, "user", "sdk", "include", "kos", "kimg.h")
FIXTURES = os.path.join(ROOT, "tests", "gfx", "fixtures")

try:
    from PIL import Image
    HAVE_PILLOW = True
except ImportError:  # pragma: no cover - the host simply lacks Pillow
    HAVE_PILLOW = False


def read_text(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def parse_header(path):
    with open(path, "rb") as handle:
        data = handle.read()
    magic, version, mode, flags, palette_count = struct.unpack(
        "<4sBBBB", data[:8])
    width, height, payload_size = struct.unpack("<HHI", data[8:16])
    return {"magic": magic, "version": version, "mode": mode,
            "flags": flags, "palette_count": palette_count,
            "width": width, "height": height,
            "payload_size": payload_size,
            "palette": data[16:16 + palette_count * 3],
            "payload": data[16 + palette_count * 3:]}


def decode_yjk(payload, width):
    """The V9958's YJK display of `payload`, as 5-bit (r, g, b): each
    byte's bits 7-3 are its pixel's Y; per four pixels, K is bits 2-0 of
    the first byte (low) and the second (high), J of the third and the
    fourth; R = Y + J, G = Y + K, B = (5Y - 2J - K) / 4, clamped."""
    def signed6(value):
        return value - 64 if value & 0x20 else value

    def clamp(value):
        return max(0, min(31, value))

    shown = []
    for run in range(0, len(payload), 4):
        quad = payload[run:run + 4]
        k = signed6((quad[0] & 7) | ((quad[1] & 7) << 3))
        j = signed6((quad[2] & 7) | ((quad[3] & 7) << 3))
        for byte in quad:
            y = byte >> 3
            shown.append((clamp(y + j), clamp(y + k),
                          clamp((5 * y - 2 * j - k) // 4)))
    return shown


def synthetic_path(directory, width, height, pixels):
    """A solid-colour RGB source image, deterministic and tiny."""
    image = Image.new("RGB", (width, height), pixels)
    path = os.path.join(directory, "source.png")
    image.save(path)
    return path


class KimgHostTests(unittest.TestCase):
    """The shipped header: layout, constants, static asserts."""

    def test_header_layout_and_constants(self):
        text = read_text(KIMG_H)
        self.assertEqual(struct_member_order(text, "kimg_header"),
                         ["magic", "version", "mode", "flags",
                          "palette_count", "width", "height",
                          "payload_size"])
        self.assertIn('KIMG_MAGIC_0 \'K\'', text)
        self.assertIn('KIMG_MAGIC_3 \'G\'', text)
        self.assertIn("KIMG_VERSION    1u", text)
        self.assertIn("KIMG_HEADER_SIZE 16u", text)
        self.assertIn("sizeof(struct kimg_header) == "
                      "KIMG_HEADER_SIZE", text)
        self.assertIn("offsetof(struct kimg_header, payload_size) == 12",
                      text)

    def test_fixtures_match_the_format(self):
        for name, mode, palette_count, payload_size in (
                ("parrot6.img", 0, 16, 1024),
                ("parrot7.img", 1, 0, 2048),
                ("parroty.img", 2, 0, 2048)):
            header = parse_header(os.path.join(FIXTURES, name))
            self.assertEqual(header["magic"], b"KIMG", name)
            self.assertEqual(header["version"], 1, name)
            self.assertEqual(header["mode"], mode, name)
            self.assertEqual(header["flags"], 0, name)
            self.assertEqual(header["palette_count"], palette_count,
                             name)
            self.assertEqual((header["width"], header["height"]),
                             (64, 32), name)
            self.assertEqual(header["payload_size"], payload_size, name)
            self.assertEqual(len(header["payload"]), payload_size, name)
            self.assertEqual(len(header["palette"]),
                             palette_count * 3, name)
            # The g6 payload packs two 4-bit indices per byte.
            if mode == 0:
                for byte in header["payload"]:
                    self.assertLessEqual(byte >> 4, 15, name)
                    self.assertLessEqual(byte & 0x0F, 15, name)


@unittest.skipUnless(HAVE_PILLOW,
                     "Pillow is required (pip install Pillow)")
class KimgConverterTests(unittest.TestCase):
    """`tools/kimg.py`: convert then parse, byte for byte."""

    def convert(self, directory, mode, size, colour):
        source = synthetic_path(directory, 23, 17, colour)
        output = os.path.join(directory, "out-" + mode + ".kimg")
        kimg.convert(source, output, mode, size)
        return parse_header(output)

    def test_g7_payload_is_the_chip_grb_byte(self):
        with tempfile.TemporaryDirectory() as directory:
            header = self.convert(directory, "g7", (8, 4), (0xA0, 0xC0,
                                                            0xE0))
            self.assertEqual(header["magic"], b"KIMG")
            self.assertEqual(header["version"], 1)
            self.assertEqual(header["mode"], 1)
            self.assertEqual(header["palette_count"], 0)
            self.assertEqual((header["width"], header["height"]), (8, 4))
            self.assertEqual(header["payload_size"], 32)
            # GRAPHIC 7 is green 7-5, red 4-2, blue 1-0: G 6, R 5, B 3.
            self.assertEqual(header["payload"], bytes([0xD7]) * 32)

    def test_yjk_solid_colours_show_their_colour(self):
        """Each colour, encoded and decoded as the chip decodes it,
        is within one 5-bit step of the source."""
        for colour in ((255, 0, 0), (0, 255, 0), (0, 0, 255),
                       (128, 128, 128), (200, 120, 40), (0, 0, 0),
                       (255, 255, 255)):
            with tempfile.TemporaryDirectory() as directory:
                header = self.convert(directory, "yjk", (8, 2), colour)
                self.assertEqual(header["mode"], 2)
                self.assertEqual(header["palette_count"], 0)
                self.assertEqual(header["payload_size"], 16)
                want = tuple(value >> 3 for value in colour)
                for shown in decode_yjk(header["payload"], 8):
                    for got, source in zip(shown, want):
                        self.assertLessEqual(abs(got - source), 1,
                                             (colour, shown))

    def test_yjk_picture_round_trip(self):
        """A picture whose colour changes inside the four-pixel runs:
        the decoded image stays close to the source (the runs share J
        and K, each pixel keeps its own Y)."""
        width, height = 32, 8
        image = Image.new("RGB", (width, height))
        for y in range(height):
            for x in range(width):
                image.putpixel((x, y), ((x * 8) & 0xFF, (y * 32) & 0xFF,
                                        255 - x * 8))
        with tempfile.TemporaryDirectory() as directory:
            source = os.path.join(directory, "gradient.png")
            image.save(source)
            output = os.path.join(directory, "gradient.kimg")
            kimg.convert(source, output, "yjk", (width, height))
            header = parse_header(output)
        shown = decode_yjk(header["payload"], width)
        error = 0
        for index, pixel in enumerate(shown):
            source = image.getpixel((index % width, index // width))
            error += sum(abs(got - (value >> 3))
                         for got, value in zip(pixel, source))
        self.assertLess(error / (3 * len(shown)), 1.5)

    def test_yjk_width_is_a_multiple_of_four(self):
        with tempfile.TemporaryDirectory() as directory:
            source = synthetic_path(directory, 8, 8, (0, 0, 0))
            with self.assertRaises(SystemExit):
                kimg.convert(source, os.path.join(directory, "o.kimg"),
                             "yjk", (6, 4))

    def test_g6_palette_and_packed_indices(self):
        with tempfile.TemporaryDirectory() as directory:
            header = self.convert(directory, "g6", (8, 4), (0x30, 0x50,
                                                            0x70))
            self.assertEqual(header["mode"], 0)
            # A solid colour quantizes to a single palette entry (the
            # format carries the count, at most 16 for G6).
            self.assertEqual(header["palette_count"], 1)
            self.assertEqual(len(header["palette"]), 3)
            self.assertEqual((header["width"], header["height"]), (8, 4))
            self.assertEqual(header["payload_size"], 16)
            self.assertEqual(tuple(header["palette"][0:3]),
                             (0x30, 0x50, 0x70))
            # Every packed nibble is that index: 0.
            self.assertEqual(header["payload"], bytes(16))

    def test_conversion_is_deterministic(self):
        with tempfile.TemporaryDirectory() as directory:
            source = synthetic_path(directory, 23, 17, (0x30, 0x50,
                                                        0x70))
            outputs = []
            for number in range(2):
                output = os.path.join(directory, "d%d.kimg" % number)
                kimg.convert(source, output, "g6", (16, 8))
                with open(output, "rb") as handle:
                    outputs.append(handle.read())
            self.assertEqual(outputs[0], outputs[1])

    def test_bad_geometry_refuses(self):
        with tempfile.TemporaryDirectory() as directory:
            source = synthetic_path(directory, 8, 8, (0, 0, 0))
            for size in ((0, 4), (4, 0), (5, 4)):  # g6 wants even x
                with self.assertRaises(SystemExit):
                    kimg.convert(source, "out.kimg", "g6", size)

    def test_info_subcommand(self):
        with tempfile.TemporaryDirectory() as directory:
            source = synthetic_path(directory, 23, 17, (0x30, 0x50,
                                                        0x70))
            output = os.path.join(directory, "info.kimg")
            kimg.convert(source, output, "g7", (8, 4))
            run = subprocess.run([sys.executable,
                                  os.path.join(ROOT, "tools", "kimg.py"),
                                  "info", output],
                                 capture_output=True, text=True,
                                 check=True)
            self.assertIn("KIMG v1 mode=g7 flags=0 palette=0 8x4 "
                          "payload=32", run.stdout)


class KimgRawTests(unittest.TestCase):
    def test_pack_is_lossless_in_every_mode(self):
        with tempfile.TemporaryDirectory() as directory:
            for mode, count in (("g6", 16), ("g7", 32), ("yjk", 32)):
                source = os.path.join(directory, "raw." + mode)
                output = os.path.join(directory, "out.img")
                payload = bytes((i * 47) & 255 for i in range(count))
                with open(source, "wb") as stream:
                    stream.write(payload)
                with mock.patch.object(kimg, "Image", None):
                    kimg.pack(source, output, mode, (8, 4))
                header = parse_header(output)
                self.assertEqual(header['payload'], payload)
                self.assertEqual(header['mode'], kimg.MODES[mode][0])
                self.assertEqual(header['payload_size'], count)
                self.assertEqual(header['palette_count'], 0)

    def test_palette_and_invalid_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            source = os.path.join(directory, "raw")
            palette = os.path.join(directory, "palette")
            output = os.path.join(directory, "out.img")
            with open(source, "wb") as stream:
                stream.write(bytes(range(16)))
            with open(palette, "wb") as stream:
                stream.write(bytes(range(48)))
            kimg.pack(source, output, 'g6', (8, 4), palette)
            self.assertEqual(parse_header(output)['palette'], bytes(range(48)))
            for mode, size, pal in [('g6', (7, 4), None), ('g6', (8, 5), None),
                                    ('g7', (8, 4), palette), ('yjk', (6, 4), None),
                                    ('g7', (256, 213), None)]:
                with self.subTest(mode=mode, size=size), self.assertRaises(SystemExit):
                    kimg.pack(source, output, mode, size, pal)
            with open(palette, 'wb') as stream:
                stream.write(bytes(32))  # VDP palette dump is not RGB888.
            with self.assertRaises(SystemExit):
                kimg.pack(source, output, 'g6', (8, 4), palette)

    def test_pack_info_and_help_without_pillow(self):
        with tempfile.TemporaryDirectory() as directory:
            source = os.path.join(directory, "raw")
            output = os.path.join(directory, "out.img")
            with open(source, 'wb') as stream:
                stream.write(bytes(range(32)))
            # -S genuinely removes site-packages, including Pillow. The
            # library Image=None check alone would not cover import/CLI use.
            command = [sys.executable, '-S', os.path.join(ROOT, 'tools/kimg.py')]
            subprocess.run(command + ['pack', source, output, '--mode', 'yjk',
                                      '--size', '8x4'], check=True, capture_output=True)
            run = subprocess.run(command + ['info', output], check=True,
                                 capture_output=True, text=True)
            self.assertIn('mode=yjk', run.stdout)
            run = subprocess.run(command + ['convert', '--help'], check=True,
                                 capture_output=True, text=True)
            self.assertIn('--yjk-quality', run.stdout)
            run = subprocess.run(command + ['pack', source, output, '--mode', 'yjk',
                                  '--sharpen'], capture_output=True)
            self.assertNotEqual(run.returncode, 0)


class KimgPreviewTests(unittest.TestCase):
    """`preview`: a KIMG file rendered as the V9958 shows it."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def packed(self, mode, size, payload, palette=None):
        source = os.path.join(self.tmp, "raw.bin")
        with open(source, "wb") as handle:
            handle.write(bytes(payload))
        palette_path = None
        if palette is not None:
            palette_path = os.path.join(self.tmp, "palette.rgb")
            with open(palette_path, "wb") as handle:
                handle.write(bytes(palette))
        output = os.path.join(self.tmp, "%s.kimg" % mode)
        kimg.pack(source, output, mode, size, palette_path)
        return output

    def test_g6_uses_the_file_palette_at_three_bits(self):
        # Entry 0 = (255, 128, 31) -> levels (7, 4, 0); the unset
        # entries keep the console's: 1 dark grey (1, 1, 1), 4 blue
        # (0, 0, 5).
        path = self.packed("g6", (4, 1), [0x01, 0x04],
                           palette=[255, 128, 31])
        mode, width, height, pixels = kimg.decode(path)
        self.assertEqual((mode, width, height), ("g6", 4, 1))
        self.assertEqual(pixels[0], (255, 146, 0))
        self.assertEqual(pixels[1], (36, 36, 36))  # console entry 1
        self.assertEqual(pixels[3], (0, 0, 182))   # console entry 4

    def test_g6_without_a_palette_shows_the_console_palette(self):
        path = self.packed("g6", (2, 1), [0x0F])
        _mode, _w, _h, pixels = kimg.decode(path)
        self.assertEqual(pixels, [(0, 0, 0), (255, 255, 255)])

    def test_g7_is_grb_332_with_four_blue_levels(self):
        path = self.packed("g7", (8, 1),
                           [0x00, 0xFF, 0xE0, 0x1C, 0x01, 0x02, 0x03, 0x24])
        _mode, _w, _h, pixels = kimg.decode(path)
        self.assertEqual(pixels, [
            (0, 0, 0), (255, 255, 255), (0, 255, 0), (255, 0, 0),
            (0, 0, 73), (0, 0, 146), (0, 0, 255), (36, 36, 0)])

    def test_yjk_matches_the_decoder(self):
        rng = random.Random(7)
        payload = [rng.randrange(256) for _ in range(16 * 2)]
        path = self.packed("yjk", (16, 2), payload)
        _mode, _w, _h, pixels = kimg.decode(path)
        expected = [tuple((c << 3) | (c >> 2) for c in rgb)
                    for rgb in decode_yjk(payload, 16)]
        self.assertEqual(pixels, expected)
        white = self.packed("yjk", (4, 1), [0xF8] * 4)
        self.assertEqual(kimg.decode(white)[3], [(255, 255, 255)] * 4)

    @unittest.skipUnless(HAVE_PILLOW, "Pillow is not installed")
    def test_png_and_display_proportions(self):
        for mode, size, scaled in (("g6", (8, 2), (8, 4)),
                                   ("g7", (8, 2), (16, 4)),
                                   ("yjk", (8, 2), (16, 4))):
            with self.subTest(mode=mode):
                count = size[0] * size[1] // (2 if mode == "g6" else 1)
                path = self.packed(mode, size, [0x55] * count)
                out = os.path.join(self.tmp, "%s.png" % mode)
                self.assertEqual(kimg.preview(path, out), size)
                with Image.open(out) as image:
                    self.assertEqual(image.size, size)
                    self.assertEqual(list(image.convert("RGB").getdata()),
                                     kimg.decode(path)[3])
                self.assertEqual(kimg.preview(path, out, display=True),
                                 scaled)

    @unittest.skipUnless(HAVE_PILLOW, "Pillow is not installed")
    def test_command_line(self):
        path = self.packed("g7", (4, 4), [0xE3] * 16)
        out = os.path.join(self.tmp, "cli.png")
        result = subprocess.run(
            [sys.executable, os.path.join(ROOT, "tools", "kimg.py"),
             "preview", path, out, "--display"],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("(8x8)", result.stdout)

    def test_malformed_files_are_refused(self):
        good = self.packed("g7", (4, 1), [1, 2, 3, 4])
        with open(good, "rb") as handle:
            data = handle.read()
        for name, bad in (("magic", b"XIMG" + data[4:]),
                          ("mode", data[:5] + b"\x09" + data[6:]),
                          ("truncated", data[:-1]),
                          ("trailing", data + b"\x00"),
                          ("short", data[:10])):
            with self.subTest(name=name):
                path = os.path.join(self.tmp, "bad.kimg")
                with open(path, "wb") as handle:
                    handle.write(bad)
                with self.assertRaises(SystemExit):
                    kimg.decode(path)


class KimgYjkSearchTests(unittest.TestCase):
    def test_decoder_preserves_blue_floor_and_clamping(self):
        self.assertEqual(kimg.yjk_decode(1, 0, 0), (1, 1, 1))
        self.assertEqual(kimg.yjk_decode(1, -1, 0), (0, 1, 1))  # 7 // 4
        self.assertEqual(kimg.yjk_decode(0, 31, 31), (31, 31, 0))
        self.assertEqual(kimg.yjk_decode(31, -32, -32), (0, 0, 31))
        for j in range(-32, 32):
            for k in range(-32, 32):
                for y in range(32):
                    clamp = lambda c: max(0, min(31, c))
                    self.assertEqual(kimg.yjk_decode(y, j, k),
                        (clamp(y+j), clamp(y+k), clamp((5*y-2*j-k)//4)))

    def test_signed_packing_and_no_regression_on_identical_rgb555(self):
        rng = random.Random(9958)
        groups = [[colour] * 4 for colour in
                  [(31, 0, 0), (0, 31, 0), (0, 0, 31), (0, 0, 0), (31, 31, 31)]]
        groups += [[tuple(rng.randrange(32) for _ in range(3)) for _ in range(4)]
                   for _ in range(80)]
        signs = set()
        for pixels in groups:
            encoded = kimg._yjk_run_optimized(pixels)
            self.assertEqual(len(encoded), 4)
            self.assertEqual(encoded, kimg._yjk_run_optimized(pixels))
            j = (encoded[2] & 7) | ((encoded[3] & 7) << 3)
            k = (encoded[0] & 7) | ((encoded[1] & 7) << 3)
            j = j - 64 if j >= 32 else j
            k = k - 64 if k >= 32 else k
            signs.add((j < 0, k < 0))
            self.assertTrue(-32 <= j <= 31 and -32 <= k <= 31)
            self.assertEqual(decode_yjk(encoded, 4),
                [kimg.yjk_decode(byte >> 3, j, k) for byte in encoded])
            def error(payload):
                return sum((a-b)**2 for p, q in zip(pixels, decode_yjk(payload, 4))
                           for a, b in zip(p, q))
            self.assertLessEqual(error(encoded), error(kimg._yjk_run(pixels)))
        self.assertEqual(signs, {(False, False), (False, True), (True, False), (True, True)})

    def test_exhaustive_oracle_and_ties(self):
        # Independent scalar oracle pins candidate bounds, global group
        # minimum and lexicographic ties, not just a weaker SSE comparison.
        for pixels in [[(0, 0, 0)] * 4, [(31, 0, 0)] * 4,
                       [(2, 30, 5), (31, 1, 24), (0, 14, 31), (29, 20, 1)]]:
            r, g, b = (sum(p[c] for p in pixels) / 4 for c in range(3))
            y = b / 2 + r / 4 + g / 8
            j0, k0 = max(-32, min(31, round(r-y))), max(-32, min(31, round(g-y)))
            choices = []
            for j in range(max(-32, j0-3), min(31, j0+3)+1):
                for k in range(max(-32, k0-3), min(31, k0+3)+1):
                    ys, errors = [], []
                    for pixel in pixels:
                        error, luminance = min((sum((a-b)**2 for a,b in
                            zip(pixel, kimg.yjk_decode(y,j,k))), y) for y in range(32))
                        ys.append(luminance); errors.append(error)
                    choices.append((sum(errors), j, k, ys))
            _, j, k, ys = min(choices)
            lows = [k & 7, (k >> 3) & 7, j & 7, (j >> 3) & 7]
            expected = bytes((y << 3) | low for y, low in zip(ys, lows))
            self.assertEqual(kimg._yjk_run_optimized(pixels), expected)


@unittest.skipUnless(HAVE_PILLOW, "Pillow required for image conversion")
class KimgQualityTests(unittest.TestCase):
    def pattern(self, size=(23, 17)):
        image = Image.new('RGB', size)
        image.putdata([((x*37+y*19)%256, (x*11+y*43)%256, (x*61+y*7)%256)
                       for y in range(size[1]) for x in range(size[0])])
        return image

    def test_default_output_matches_prechange(self):
        # Captured before quality-option changes, Pillow 12.1.1. G7/YJK
        # use no adaptive palette; G6 uses the same quantizer across versions
        # and its exact pinned hash is checked on the capture's Pillow version.
        hashes = {'g6': '06d5f948fe1e648dcdd271276b512d916bbc1d370e2e63e1e3bbd49718ad4dd7',
                  'g7': '4fed012945f2c9fdc301e4e7202aba64a229c70d813703b341bbb0ca40285428',
                  'yjk': 'c4eb2c0045012ef59405c41e137ce56c4e9bb12bc933f8dcff10a9b65ee4c8ab'}
        with tempfile.TemporaryDirectory() as directory:
            source, output = os.path.join(directory, 'source.png'), os.path.join(directory, 'o.img')
            self.pattern().save(source)
            # Old 23x17 -> 16x8 crop, followed unconditionally by NEAREST.
            fitted = self.pattern().crop((0, 3, 23, 14)).resize((16, 8), Image.Resampling.NEAREST)
            for mode in kimg.MODES:
                kimg.convert(source, output, mode, (16, 8))
                with open(output, 'rb') as stream:
                    data = stream.read()
                if mode != 'g6' or Image.__version__ == '12.1.1':
                    self.assertEqual(hashlib.sha256(data).hexdigest(), hashes[mode])
                palette, payload = kimg.PACKERS[mode](fitted, (16, 8))
                self.assertEqual(parse_header(output)['payload'], payload)
                self.assertEqual(parse_header(output)['palette'], bytes(palette))
                kimg.convert(source, output, mode, (16, 8), resample='nearest',
                             sharpen=False, yjk_quality='fast')
                with open(output, 'rb') as stream:
                    self.assertEqual(stream.read(), data)

    def test_resampling_every_mode_and_native_size_bypass(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = os.path.join(directory,'source.png'), os.path.join(directory,'out.img')
            self.pattern().save(source)
            for mode in kimg.MODES:
                for resample in ('nearest', 'box', 'lanczos'):
                    kimg.convert(source, output, mode, (16, 8), resample=resample)
                    header = parse_header(output)
                    self.assertEqual((header['width'],header['height']), (16,8))
                    self.assertEqual(len(header['payload']), 64 if mode == 'g6' else 128)
            native = self.pattern((16, 8))
            with mock.patch.object(Image.Image, 'resize', side_effect=AssertionError('resize called')):
                for resample in ('nearest','box','lanczos'):
                    self.assertIs(kimg._fit(native, (16,8), resample), native)
                    self.assertEqual(kimg._fit(self.pattern((16,10)), (16,8), resample).size, (16,8))
            actual = [kimg._fit(self.pattern(), (16,8), name).tobytes()
                      for name in ('nearest','box','lanczos')]
            self.assertEqual(len(set(actual)), 3)

    def test_sharpen_after_resize_with_configured_values(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = os.path.join(directory,'source.png'), os.path.join(directory,'out.img')
            image = self.pattern(); image.save(source)
            expected = kimg._fit(image,(16,8),'lanczos').filter(
                kimg.ImageFilter.UnsharpMask(radius=1.1, percent=150, threshold=3))
            kimg.convert(source, output, 'g7', (16,8), resample='lanczos',
                         sharpen=True, sharpen_radius=1.1, sharpen_percent=150, sharpen_threshold=3)
            self.assertEqual(parse_header(output)['payload'], kimg._pack_g7(expected,(16,8))[1])
            with mock.patch.object(kimg.ImageFilter, 'UnsharpMask', side_effect=AssertionError):
                kimg.convert(source, output, 'g7', (16,8))
            for options in [dict(sharpen_radius=-1), dict(sharpen_radius=float('nan')),
                            dict(sharpen_percent=-1), dict(sharpen_threshold=256)]:
                with self.assertRaises(SystemExit):
                    kimg.convert(source, output, 'yjk', (16,8), **options)

    def test_optimized_rgb_rounding_and_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = os.path.join(directory,'source.png'), os.path.join(directory,'out.img')
            image = self.pattern((8,4)); image.save(source)
            groups = []
            original = kimg._yjk_run_optimized
            def capture(pixels):
                groups.append(pixels)
                return original(pixels)
            with mock.patch.object(kimg, '_yjk_run_optimized', side_effect=capture):
                kimg.convert(source, output, 'yjk', (8,4), yjk_quality='optimized')
            raw = iter(image.tobytes())
            expected = [tuple((c*31+127)//255 for c in p) for p in zip(raw,raw,raw)]
            self.assertEqual([p for group in groups for p in group], expected)
            run = subprocess.run([sys.executable, os.path.join(ROOT,'tools/kimg.py'),
                'convert', source, output, '--mode','yjk','--size','8x4',
                '--resample','lanczos','--sharpen','--sharpen-radius','0.8',
                '--sharpen-percent','120','--sharpen-threshold','2',
                '--yjk-quality','optimized'], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            kimg.convert(source, output, 'yjk', (8,4), resample='lanczos', sharpen=True,
                         yjk_quality='optimized')
            first = parse_header(output)['payload']
            kimg._yjk_optimized_group.cache_clear()
            kimg._yjk_decoded_ramp.cache_clear()
            kimg.convert(source, output, 'yjk', (8,4), resample='lanczos', sharpen=True,
                         yjk_quality='optimized')
            self.assertEqual(parse_header(output)['payload'], first)

    def test_representative_fixture_comparison(self):
        import kimg_compare
        with tempfile.TemporaryDirectory() as directory:
            for name, image in kimg_compare.comparison_sources().items():
                result = kimg_compare.compare(image, os.path.join(directory,name), (64,32))
                self.assertEqual(result['fast']['file_bytes'], 16+64*32)
                self.assertEqual(result['optimized']['file_bytes'], 16+64*32)
                # Empirical fixture assertion, distinct from the identical
                # RGB555 guarantee tested above (quantizers differ here).
                self.assertLessEqual(result['optimized']['rgb555_rounded_sse'],
                                     result['fast']['rgb555_rounded_sse'])


if __name__ == "__main__":
    unittest.main()
